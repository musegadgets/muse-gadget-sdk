/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mg_ble.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "mg_ble_priv.h"
#include "mg_codec.h"
#include "mg_play.h"
#include "mg_config.h"
#include "mgcommands.h"
#if MG_WITH_SECURE
#include "mg_crypto.h"
#include "mgcommands-secure.h"
#endif

static const char *TAG = "mg.ble";
static const char *PLAY_TAG = "mg.play";   /* on the Nordic UART mirror (tag prefix "mg") */

#define NVS_NS "mg"
#define NVS_SETTINGS "settings"
#define SETTINGS_VERSION 1
#define MAX_PAIRINGS 4
#define PAIRING_WINDOW_MS (120 * 1000)
#define QUEUE_PART_SUBTYPE 0x4D   /* partitions_muse.csv: mg_queue */
/* The key exchange (X25519, HKDF) and the token proof's HMACs run on the worker. */
#define WORKER_STACK (MG_WITH_SECURE ? 8192 : 6144)
#define PLAYER_STACK 4096
#define PLAY_CHUNK 320                    /* 20 ms at 16 kHz: what the player writes at a time */
#define PLAY_TAIL_CHUNKS 5                /* silence after a stream, so the DMA holds no stale audio */
#define NUS_RING 2048
#define NUS_LINE 160

#define STR_(x) #x
#define STR(x) STR_(x)

/* ---- state ---------------------------------------------------------------- */

static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(MG_SERVICE_UUID_LE_BYTES);
static const ble_uuid128_t CONTROL_UUID = BLE_UUID128_INIT(MG_CONTROL_UUID_LE_BYTES);
static const ble_uuid128_t DATA_UUID = BLE_UUID128_INIT(MG_DATA_UUID_LE_BYTES);
#if MG_WITH_SECURE
static const ble_uuid128_t ENC_CONTROL_UUID = BLE_UUID128_INIT(MG_ENCRYPTED_CONTROL_UUID_LE_BYTES);
static const ble_uuid128_t ENC_DATA_UUID = BLE_UUID128_INIT(MG_ENCRYPTED_DATA_UUID_LE_BYTES);
#endif
static const ble_uuid128_t NUS_UUID = BLE_UUID128_INIT(MG_NUS_SERVICE_UUID_LE_BYTES);
static const ble_uuid128_t NUS_RX_UUID = BLE_UUID128_INIT(MG_NUS_RX_UUID_LE_BYTES);
static const ble_uuid128_t NUS_TX_UUID = BLE_UUID128_INIT(MG_NUS_TX_UUID_LE_BYTES);
static const uint8_t ADV_UUID[16] = { MG_SERVICE_UUID_LE_BYTES };

static mg_ble_platform_t s_platform;
static bool s_ready;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_events;
static TaskHandle_t s_worker;
static TaskHandle_t s_player;
static mg_proto_t s_proto;
static mg_play_t s_play;
/* The client's state (mg_link.h), re-read after anything that can change it. */
static volatile mg_link_t s_link = MG_LINK_NEVER;
static volatile bool s_set_up;   /* Link setup left a proof key: the gadget has been set up */
static mg_capture_req_t s_capture;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_handle[MG_CH_COUNT];
static uint16_t s_bas_handle, s_nus_tx_handle;
static volatile bool s_bas_sub, s_nus_sub;
static int s_bas_last = -1;

#if CONFIG_MUSE_GADGET_BLE_QUEUE
static mg_queue_t s_queue;
static const esp_partition_t *s_queue_part;
#endif
static bool s_have_queue;

/* Pairing mode and the prompt (session security). */
#if MG_WITH_SECURE
static int64_t s_window_until_us;
static int s_attempts;
#endif
static volatile uint8_t s_prompt_method;
static volatile uint32_t s_prompt_code;

typedef enum {
    EV_WRITE,
    EV_CONFIRM,
    EV_CLIP,
    EV_KICK,
} ev_type_t;

typedef struct {
    ev_type_t type;
    uint8_t ch;
    uint8_t codec;
    uint32_t len;
    uint32_t start_ms;
    uint8_t *data;
} ev_t;

bool mg_active(void)
{
    return s_ready;
}

void mg_lock(void)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

void mg_unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

mg_proto_t *mg_proto_get(void)
{
    return &s_proto;
}

mg_capture_req_t *mg_capture_req(void)
{
    return &s_capture;
}

uint32_t mg_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

mg_queue_t *mg_queue_get(void)
{
#if CONFIG_MUSE_GADGET_BLE_QUEUE
    return s_have_queue ? &s_queue : NULL;
#else
    return NULL;
#endif
}

void mg_play_kick(void)
{
    if (s_player) {
        xTaskNotifyGive(s_player);
    }
}

bool mg_vibrate(uint16_t freq_hz, uint16_t ms, uint8_t volume_pct)
{
    if (!s_platform.vibrate) {
        return false;
    }
    s_platform.vibrate(freq_hz, ms, volume_pct);
    return true;
}

static void op_assistant_state(void *ctx, uint8_t state)
{
    (void)ctx;
    s_platform.assistant_state(state);
}

static void op_play_kick(void *ctx)
{
    (void)ctx;
    mg_play_kick();
    if (s_platform.attention) {
        s_platform.attention();   /* keeps the codecs powered for the stream */
    }
}

static void kick(void)
{
    if (s_events) {
        ev_t ev = { .type = EV_KICK };
        xQueueSend(s_events, &ev, 0);
    }
}

bool mg_queue_submit(uint8_t codec, uint32_t start_ms, uint8_t *data, uint32_t bytes)
{
    ev_t ev = { .type = EV_CLIP, .codec = codec, .start_ms = start_ms, .len = bytes, .data = data };
    if (!s_events || xQueueSend(s_events, &ev, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(data);
        return false;
    }
    return true;
}

/* ---- persistence ---------------------------------------------------------- */

static void load_settings(mg_settings_t *s)
{
    mg_settings_defaults(s);
    /* A BLE-only gadget keeps presses made while no client is connected until
     * one is; clients can turn that off (audio_queue_enabled). */
    s->queue_enabled = MG_STANDALONE;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t b[8];
    size_t n = sizeof(b);
    if (nvs_get_blob(h, NVS_SETTINGS, b, &n) == ESP_OK && n == 8 && b[0] == SETTINGS_VERSION) {
        s->buzz_freq_hz = (uint16_t)(b[1] | b[2] << 8);
        s->buzz_ms = (uint16_t)(b[3] | b[4] << 8);
        s->buzz_volume = b[5];
        s->codec = b[6];
        s->queue_enabled = b[7];
    }
    nvs_close(h);
}

static void save_settings(const mg_settings_t *s)
{
    uint8_t b[8] = {
        SETTINGS_VERSION, (uint8_t)s->buzz_freq_hz, (uint8_t)(s->buzz_freq_hz >> 8), (uint8_t)s->buzz_ms,
        (uint8_t)(s->buzz_ms >> 8), s->buzz_volume, s->codec, s->queue_enabled,
    };
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, NVS_SETTINGS, b, sizeof(b)) != ESP_OK || nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "settings not saved");
    }
    nvs_close(h);
}

/* ---- token proof -----------------------------------------------------------
 * K comes from Link's config (the platform's proof_key); it never stays in
 * this component's memory longer than one HMAC.
 */

static bool op_random(void *ctx, uint8_t *out, size_t n)
{
    (void)ctx;
    esp_fill_random(out, n);   /* the hardware RNG, with the radio on */
    return true;
}

static bool op_proof_key(void *ctx, uint8_t k[MG_TOKEN_PROOF_KEY_LEN])
{
    (void)ctx;
    return s_platform.proof_key(k);
}

static void op_proof_clear(void *ctx)
{
    (void)ctx;
    ESP_LOGW(TAG, "token proof clear: resetting setup");
    s_set_up = false;
    s_platform.proof_clear();
}

#if MG_WITH_SECURE
/* Pairings: "p0".."p3", each key_id[8] || PK[32], and "pnext", the slot to overwrite next. */
static bool pairing_slot(nvs_handle_t h, int i, uint8_t rec[40])
{
    char key[4] = { 'p', (char)('0' + i), 0 };
    size_t n = 40;
    return nvs_get_blob(h, key, rec, &n) == ESP_OK && n == 40;
}

static int pairing_count(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    int count = 0;
    uint8_t rec[40];
    for (int i = 0; i < MAX_PAIRINGS; i++) {
        count += pairing_slot(h, i, rec);
    }
    mg_crypto_wipe(rec, sizeof(rec));
    nvs_close(h);
    return count;
}

static bool op_pairing_find(void *ctx, const uint8_t id[8], uint8_t pk[32])
{
    (void)ctx;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool found = false;
    uint8_t rec[40];
    for (int i = 0; i < MAX_PAIRINGS && !found; i++) {
        if (pairing_slot(h, i, rec) && mg_crypto_equal(rec, id, 8)) {
            memcpy(pk, rec + 8, 32);
            found = true;
        }
    }
    mg_crypto_wipe(rec, sizeof(rec));
    nvs_close(h);
    return found;
}

static bool op_pairing_save(void *ctx, const uint8_t id[8], const uint8_t pk[32])
{
    (void)ctx;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    uint8_t rec[40];
    int slot = -1;
    for (int i = 0; i < MAX_PAIRINGS && slot < 0; i++) {
        if (!pairing_slot(h, i, rec)) {
            slot = i;
        }
    }
    uint8_t next = 0;
    if (slot < 0) {
        nvs_get_u8(h, "pnext", &next);
        slot = next % MAX_PAIRINGS;
        nvs_set_u8(h, "pnext", (uint8_t)((slot + 1) % MAX_PAIRINGS));
    }
    memcpy(rec, id, 8);
    memcpy(rec + 8, pk, 32);
    char key[4] = { 'p', (char)('0' + slot), 0 };
    bool ok = nvs_set_blob(h, key, rec, sizeof(rec)) == ESP_OK && nvs_commit(h) == ESP_OK;
    mg_crypto_wipe(rec, sizeof(rec));
    nvs_close(h);
    ESP_LOGI(TAG, "paired a client (slot %d)%s", slot, ok ? "" : ": not saved");
    return ok;
}

static void op_pairing_forget(void *ctx, const uint8_t id[8])
{
    (void)ctx;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    uint8_t rec[40];
    for (int i = 0; i < MAX_PAIRINGS; i++) {
        if (pairing_slot(h, i, rec) && mg_crypto_equal(rec, id, 8)) {
            char key[4] = { 'p', (char)('0' + i), 0 };
            nvs_erase_key(h, key);
            ESP_LOGI(TAG, "forgot a paired client (slot %d)", i);
        }
    }
    nvs_commit(h);
    mg_crypto_wipe(rec, sizeof(rec));
    nvs_close(h);
}

static bool op_pairing_mode(void *ctx)
{
    (void)ctx;
    if (s_attempts >= MG_PAIRING_MAX_ATTEMPTS) {
        return false;
    }
    return esp_timer_get_time() < s_window_until_us || pairing_count() == 0;
}

static void op_pairing_attempt(void *ctx)
{
    (void)ctx;
    if (++s_attempts >= MG_PAIRING_MAX_ATTEMPTS) {
        ESP_LOGW(TAG, "leaving pairing mode after %d attempts", s_attempts);
    }
}

static void op_pairing_prompt(void *ctx, uint8_t method, uint32_t code)
{
    (void)ctx;
    s_prompt_code = code;
    s_prompt_method = method;
    if (method) {
        ESP_LOGI(TAG, "pairing: waiting for the button (%s)",
                 method == mg_pairing_method_numeric_comparison ? "compare the code" : "press to accept");
        if (s_platform.attention) {
            s_platform.attention();
        }
    }
}
#endif

/* ---- protocol ops ------------------------------------------------------- */

static void terminate_cb(void *arg)
{
    (void)arg;
    uint16_t conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static bool op_notify(void *ctx, mg_ch_t ch, const uint8_t *data, size_t len)
{
    (void)ctx;
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE || !s_handle[ch]) {
        return false;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        return false;
    }
    return ble_gatts_notify_custom(conn, s_handle[ch], om) == 0;
}

static void op_disconnect(void *ctx)
{
    (void)ctx;
    /* A moment later, so the error just queued goes out first. */
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t a = { .callback = terminate_cb, .name = "mg_dc" };
        if (esp_timer_create(&a, &t) != ESP_OK) {
            terminate_cb(NULL);
            return;
        }
    }
    esp_timer_stop(t);
    esp_timer_start_once(t, 150 * 1000);
}

static void op_capture(void *ctx, bool start, uint8_t codec)
{
    (void)ctx;
    s_capture.requested = start;
    s_capture.codec = codec;
    if (start) {
        ESP_LOGI(TAG, "client started capture");
        if (s_platform.attention) {
            s_platform.attention();
        }
    }
}

static void op_mic_gain(void *ctx, uint8_t gain)
{
    (void)ctx;
    if (s_platform.mic_gain) {
        s_platform.mic_gain(gain);
    }
}

static void op_settings_changed(void *ctx)
{
    (void)ctx;
    save_settings(mg_proto_settings(&s_proto));
}

static uint8_t op_speaker_volume(void *ctx)
{
    (void)ctx;
    int v = s_platform.speaker_volume();
    return (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
}

static void op_set_speaker_volume(void *ctx, uint8_t volume)
{
    (void)ctx;
    s_platform.set_speaker_volume(volume);
    ESP_LOGI(TAG, "speaker volume %u%s", volume, volume ? "" : " (speaker off)");
}

static uint32_t op_now(void *ctx)
{
    (void)ctx;
    return mg_now_ms();
}

/* ---- GATT ----------------------------------------------------------------- */

static int mg_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (!s_ready || !len || len > 512 || conn != s_conn) {
        return 0;
    }
    mg_ch_t ch = (mg_ch_t)(uintptr_t)arg;
    if (ch == MG_CH_DATA || ch == MG_CH_ENC_DATA) {
        /*
         * Playback audio: straight into the ring, here on the host task, so
         * no write is ever lost to a full queue (the client's flow control
         * counts on every byte arriving) and none overtakes the Control
         * command before it. The host task is the only writer of s_rx.
         */
        static uint8_t s_rx[512];
        uint16_t n = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, s_rx, sizeof(s_rx), &n) == 0 && n) {
            mg_lock();
            mg_proto_write(&s_proto, ch, s_rx, n);
            mg_unlock();
        }
        return 0;
    }
    uint8_t *buf = malloc(len);
    if (!buf) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, &n) != 0) {
        free(buf);
        return BLE_ATT_ERR_UNLIKELY;
    }
    ev_t ev = { .type = EV_WRITE, .ch = (uint8_t)(uintptr_t)arg, .len = n, .data = buf };
    if (xQueueSend(s_events, &ev, 0) != pdTRUE) {
        free(buf);   /* the worker is swamped; like a lost write-without-response */
    }
    return 0;
}

static int battery_level(void)
{
    int pct = s_platform.battery_pct ? s_platform.battery_pct() : -1;
    return pct < 0 ? 100 : pct > 100 ? 100 : pct;
}

static int bas_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t level = (uint8_t)battery_level();
    return os_mbuf_append(ctxt->om, &level, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

enum { DIS_MANUFACTURER, DIS_MODEL, DIS_FIRMWARE, DIS_SOFTWARE };

static int dis_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    const char *s = "";
    switch ((int)(uintptr_t)arg) {
    case DIS_MANUFACTURER: s = CONFIG_MUSE_GADGET_BLE_MANUFACTURER; break;
    case DIS_MODEL: s = s_platform.model && s_platform.model() ? s_platform.model() : CONFIG_IDF_TARGET; break;
    case DIS_FIRMWARE: s = esp_app_get_description()->version; break;
    case DIS_SOFTWARE: s = "mg" STR(MG_SPEC_VERSION); break;
    }
    return os_mbuf_append(ctxt->om, s, strlen(s)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static void nus_command(char *line);

static int nus_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    char line[64];
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, line, sizeof(line) - 1, &n) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    line[n] = '\0';
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
        line[--n] = '\0';
    }
    if (s_ready) {
        nus_command(line);
    }
    return 0;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &CONTROL_UUID.u,
                .access_cb = mg_access,
                .arg = (void *)(uintptr_t)MG_CH_CONTROL,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_handle[MG_CH_CONTROL],
            },
            {
                .uuid = &DATA_UUID.u,
                .access_cb = mg_access,
                .arg = (void *)(uintptr_t)MG_CH_DATA,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_handle[MG_CH_DATA],
            },
#if MG_WITH_SECURE
            {
                .uuid = &ENC_CONTROL_UUID.u,
                .access_cb = mg_access,
                .arg = (void *)(uintptr_t)MG_CH_ENC_CONTROL,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_handle[MG_CH_ENC_CONTROL],
            },
            {
                .uuid = &ENC_DATA_UUID.u,
                .access_cb = mg_access,
                .arg = (void *)(uintptr_t)MG_CH_ENC_DATA,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_handle[MG_CH_ENC_DATA],
            },
#endif
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(MG_BAS_SERVICE_UUID16),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = BLE_UUID16_DECLARE(MG_BAS_BATTERY_LEVEL_UUID16),
                .access_cb = bas_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_bas_handle,
            },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(MG_DIS_SERVICE_UUID16),
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = BLE_UUID16_DECLARE(MG_DIS_MANUFACTURER_NAME_UUID16), .access_cb = dis_access,
              .arg = (void *)DIS_MANUFACTURER, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(MG_DIS_MODEL_NUMBER_UUID16), .access_cb = dis_access,
              .arg = (void *)DIS_MODEL, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(MG_DIS_FIRMWARE_REVISION_UUID16), .access_cb = dis_access,
              .arg = (void *)DIS_FIRMWARE, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(MG_DIS_SOFTWARE_REVISION_UUID16), .access_cb = dis_access,
              .arg = (void *)DIS_SOFTWARE, .flags = BLE_GATT_CHR_F_READ },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &NUS_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = &NUS_RX_UUID.u, .access_cb = nus_access,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &NUS_TX_UUID.u, .access_cb = nus_access, .flags = BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_nus_tx_handle },
            { 0 },
        },
    },
    { 0 },
};

const struct ble_gatt_svc_def *mg_ble_services(void)
{
    return s_svcs;
}

const uint8_t *mg_ble_adv_uuid128(void)
{
    return ADV_UUID;
}

bool mg_ble_wants_advertising(void)
{
    return s_ready && s_conn == BLE_HS_CONN_HANDLE_NONE;
}

bool mg_ble_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

/* Call with mg_lock held. */
static void link_check(void)
{
    mg_link_t l = mg_proto_link(&s_proto, s_set_up);
    if (l != s_link) {
        ESP_LOGI(TAG, "link %s -> %s", mg_link_name(s_link), mg_link_name(l));
        s_link = l;
    }
}

mg_link_t mg_ble_link(void)
{
    return s_ready ? s_link : MG_LINK_NEVER;
}

bool mg_ble_ptt_ready(void)
{
    return mg_ble_link() == MG_LINK_READY;
}

int mg_ble_gap_event(struct ble_gap_event *ev)
{
    if (!s_ready) {
        return 0;
    }
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            mg_lock();
            s_conn = ev->connect.conn_handle;
            s_capture.requested = false;
            mg_proto_connect(&s_proto);
            mg_unlock();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        mg_lock();
        mg_proto_disconnect(&s_proto);
        s_capture.requested = false;
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_bas_sub = s_nus_sub = false;
        mg_unlock();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE: {
        uint16_t h = ev->subscribe.attr_handle;
        bool on = ev->subscribe.cur_notify;
        if (h == s_bas_handle) {
            s_bas_sub = on;
            s_bas_last = -1;
        } else if (h == s_nus_tx_handle) {
            s_nus_sub = on;
        } else {
            mg_lock();
            for (int ch = 0; ch < MG_CH_COUNT; ch++) {
                if (s_handle[ch] && h == s_handle[ch]) {
                    mg_proto_subscribe(&s_proto, (mg_ch_t)ch, on);
                }
            }
            mg_unlock();
        }
        break;
    }
    case BLE_GAP_EVENT_MTU:
        mg_lock();
        mg_proto_set_mtu(&s_proto, ev->mtu.value);
        mg_unlock();
        break;
    default:
        break;
    }
    mg_lock();
    link_check();   /* connect, disconnect (any reason), (un)subscribe */
    mg_unlock();
    return 0;
}

/* ---- Nordic UART: log mirror ------------------------------------------- */

static char *s_nus_ring;
static size_t s_nus_head, s_nus_len;
static portMUX_TYPE s_nus_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev_vprintf;

/* The log line's tag is on the allowlist. Lines look like "I (123) tag: text". */
static bool nus_tag_allowed(const char *line)
{
    static const char tags[] = CONFIG_MUSE_GADGET_BLE_NUS_LOG_TAGS;
    if (tags[0] == '*') {
        return true;
    }
    const char *t = strchr(line, ')');
    if (!t) {
        return false;
    }
    t++;
    while (*t == ' ') {
        t++;
    }
    for (const char *p = tags; *p;) {
        const char *end = strchr(p, ',');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        while (n && *p == ' ') {
            p++;
            n--;
        }
        if (n && strncmp(t, p, n) == 0) {
            return true;
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
    return false;
}

static int nus_vprintf(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int r = s_prev_vprintf ? s_prev_vprintf(fmt, args) : vprintf(fmt, args);
    if (s_nus_sub && s_nus_ring) {
        char line[NUS_LINE];
        int n = vsnprintf(line, sizeof(line), fmt, copy);
        if (n > 0 && nus_tag_allowed(line)) {
            size_t len = n < (int)sizeof(line) ? (size_t)n : sizeof(line) - 1;
            /* Strip colour codes' escape byte so the line reads as text. */
            portENTER_CRITICAL(&s_nus_mux);
            if (NUS_RING - s_nus_len >= len) {
                for (size_t i = 0; i < len; i++) {
                    s_nus_ring[(s_nus_head + s_nus_len++) % NUS_RING] = line[i] == 0x1b ? ' ' : line[i];
                }
            }
            portEXIT_CRITICAL(&s_nus_mux);
        }
    }
    va_end(copy);
    return r;
}

/* Sends what's queued for NUS, a notification at a time. True if more is left. */
static bool nus_drain(void)
{
    uint16_t conn = s_conn;
    if (!s_nus_ring || !s_nus_len) {
        return false;
    }
    if (!s_nus_sub || conn == BLE_HS_CONN_HANDLE_NONE) {
        portENTER_CRITICAL(&s_nus_mux);
        s_nus_len = 0;
        portEXIT_CRITICAL(&s_nus_mux);
        return false;
    }
    uint16_t mtu = ble_att_mtu(conn);
    size_t max = mtu > 3 ? mtu - 3 : 20;
    uint8_t buf[244];
    if (max > sizeof(buf)) {
        max = sizeof(buf);
    }
    for (int k = 0; k < 4 && s_nus_len; k++) {
        size_t n = 0;
        portENTER_CRITICAL(&s_nus_mux);
        while (n < max && n < s_nus_len) {
            buf[n] = (uint8_t)s_nus_ring[(s_nus_head + n) % NUS_RING];
            n++;
        }
        portEXIT_CRITICAL(&s_nus_mux);
        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, n);
        if (!om || ble_gatts_notify_custom(conn, s_nus_tx_handle, om) != 0) {
            return true;   /* out of buffers: later */
        }
        portENTER_CRITICAL(&s_nus_mux);
        s_nus_head = (s_nus_head + n) % NUS_RING;
        s_nus_len -= n;
        portEXIT_CRITICAL(&s_nus_mux);
    }
    return s_nus_len != 0;
}

static void nus_command(char *line)
{
    if (!strcmp(line, "status")) {
        mg_lock();
        const mg_proto_t *p = &s_proto;
        ESP_LOGI(TAG, "status: connected=%d ready=%d ptt=%d live=%d mtu=%u queue=%s proof=%s%s", p->connected,
                 mg_proto_ready(p), p->ptt_enabled, p->live, p->mtu, s_have_queue ? "yes" : "no",
                 mg_proto_proof_matched(p) ? "matched" : "no", MG_WITH_SECURE ? " secure" : "");
        mg_unlock();
    } else if (!strcmp(line, "version")) {
        ESP_LOGI(TAG, "version %s, mg%d", esp_app_get_description()->version, MG_SPEC_VERSION);
    } else {
        ESP_LOGI(TAG, "console: status, version");
    }
}

/* ---- worker ------------------------------------------------------------- */

static void check_battery(void)
{
    uint16_t conn = s_conn;
    if (!s_bas_sub || conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    int level = battery_level();
    if (level != s_bas_last) {
        uint8_t b = (uint8_t)level;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(&b, 1);
        if (om && ble_gatts_notify_custom(conn, s_bas_handle, om) == 0) {
            s_bas_last = level;
        }
    }
}

static void worker(void *arg)
{
    (void)arg;
    int64_t next_tick = 0, next_battery = 0;
    for (;;) {
        mg_lock();
        bool transfer = mg_proto_transfer_active(&s_proto);
        bool streaming = mg_play_active(&s_play);
        mg_unlock();
        bool nus = s_nus_len != 0;
        TickType_t wait = transfer ? pdMS_TO_TICKS(5)
                          : nus    ? pdMS_TO_TICKS(20)
                          : streaming ? pdMS_TO_TICKS(50)
                                      : pdMS_TO_TICKS(1000);
        ev_t ev;
        if (xQueueReceive(s_events, &ev, wait) == pdTRUE) {
            switch (ev.type) {
            case EV_WRITE:
                mg_lock();
                mg_proto_write(&s_proto, (mg_ch_t)ev.ch, ev.data, ev.len);
                link_check();   /* push-to-talk on or off, authenticated */
                mg_unlock();
                memset(ev.data, 0, ev.len);
                free(ev.data);
                break;
            case EV_CONFIRM:
                mg_lock();
                mg_proto_device_confirm(&s_proto, ev.ch != 0);
                link_check();
                mg_unlock();
                break;
            case EV_CLIP: {
                mg_queue_t *q = mg_queue_get();
                bool ok = false;
                if (q) {
                    /* The erase first, outside the lock (only this task changes
                     * the queue): BLE callbacks wait on the lock. */
                    mg_queue_prepare(q, ev.len);
                    mg_lock();
                    ok = mg_queue_append(q, ev.codec, ev.start_ms, ev.data, ev.len);
                    mg_unlock();
                }
                ESP_LOGI(TAG, "offline clip of %u bytes %s", (unsigned)ev.len, ok ? "saved" : "not saved");
                free(ev.data);
                break;
            }
            case EV_KICK:
                break;
            }
        }
        if (transfer) {
            mg_lock();
            mg_proto_pump(&s_proto);
            mg_unlock();
        }
        if (streaming) {
            mg_lock();
            mg_proto_stream_tick(&s_proto);   /* buffer_update at least every 250 ms */
            mg_unlock();
        }
        nus_drain();
        int64_t now = esp_timer_get_time();
        if (now >= next_tick) {
            next_tick = now + 1000 * 1000;
            mg_lock();
            mg_proto_tick(&s_proto);
            mg_unlock();
        }
        if (now >= next_battery) {
            next_battery = now + 30 * 1000 * 1000;
            check_battery();
        }
    }
}

/* ---- playback -------------------------------------------------------------- */

static const char *codec_name(uint8_t codec)
{
    return codec == mg_data_type_audio_sbc ? "sbc" : codec == mg_data_type_audio_lc3 ? "lc3" : "pcm";
}

/* One line per stream, on the Nordic UART mirror: what a client can check playback by. */
static void log_stream(const char *how, const mg_play_params_t *pp, const mg_play_stats_t *st, const mg_dec_t *d,
                       const mg_level_t *spk)
{
    char text[400];
    mg_play_format_stats(text, sizeof(text), how, pp, st, d, spk);
    for (char *line = text; line;) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }
        ESP_LOGI(PLAY_TAG, "%s", line);
        line = nl ? nl + 1 : NULL;
    }
}

static bool speaker_muted(void)
{
    return s_platform.speaker_volume && s_platform.speaker_volume() == 0;
}

static void speaker(const int16_t *pcm, size_t n)
{
    if (!s_platform.speaker_write(pcm, n)) {
        vTaskDelay(pdMS_TO_TICKS(n * 1000 / MG_PLAY_OUT_RATE));   /* codecs off: keep the pace */
    }
}

/*
 * Decodes the client's stream to the speaker: pops about 20 ms of whole
 * frames at a time from the ring (under mg_lock), decodes and resamples them
 * outside it, and writes 20 ms chunks; silence while prebuffering or starved.
 * speaker_write blocks on the I2S DMA, which paces the loop.
 */
static void player(void *arg)
{
    (void)arg;
    static const int16_t silence[PLAY_CHUNK];
    static int16_t fifo[MG_PLAY_MAX_OUT + PLAY_CHUNK];
    static uint8_t frames[MG_PLAY_MAX_POP];
    mg_dec_t *dec = NULL;
    uint32_t stream = 0;
    size_t nfifo = 0;
    mg_play_params_t pp = { 0 };
    mg_play_stats_t st = { 0 };
    mg_level_t spk = { 0 };   /* what reached the codec (the speaker's mute zeroes it) */
    for (;;) {
        mg_lock();
        mg_play_state_t state = s_play.state;
        bool fresh = state != MG_PLAY_IDLE && (!dec || s_play.stream != stream);
        uint32_t new_stream = s_play.stream;
        mg_play_params_t new_pp = s_play.params;
        size_t n = 0;
        if (!fresh && (state == MG_PLAY_PREBUFFER || state == MG_PLAY_PLAYING || state == MG_PLAY_DRAINING)) {
            n = mg_play_pop(&s_play, frames, sizeof(frames), 20000);
        }
        if (!fresh && state != MG_PLAY_IDLE) {
            st = s_play.st;   /* this stream's; a replaced one keeps its last */
        }
        mg_unlock();

        if (dec && (fresh || state == MG_PLAY_IDLE)) {
            /* Dropped, or replaced by a new stream. */
            log_stream("drop", &pp, &st, dec, &spk);
            mg_dec_close(dec);
            free(dec);
            dec = NULL;
            nfifo = 0;
            for (int i = 0; i < PLAY_TAIL_CHUNKS; i++) {
                speaker(silence, PLAY_CHUNK);
            }
        }
        if (state == MG_PLAY_IDLE) {
            mg_lock();
            mg_play_release(&s_play);
            mg_unlock();
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (fresh) {
            stream = new_stream;
            pp = new_pp;
            memset(&st, 0, sizeof(st));
            memset(&spk, 0, sizeof(spk));
            dec = malloc(sizeof(*dec));
            if (!dec || !mg_dec_open(dec, &pp)) {
                ESP_LOGW(PLAY_TAG, "no memory to decode the stream");
                if (dec) {
                    mg_dec_close(dec);
                }
                free(dec);
                dec = NULL;
                mg_lock();
                if (s_play.stream == stream && mg_play_active(&s_play)) {
                    mg_play_stop(&s_play, false);
                    s_play.state = MG_PLAY_DONE;   /* notifies stop_streaming drop */
                    mg_proto_stream_finished(&s_proto);
                }
                mg_unlock();
                continue;
            }
            ESP_LOGI(PLAY_TAG, "stream start: %s %lu Hz%s%s", codec_name(pp.codec), (unsigned long)pp.rate,
                     pp.rate != MG_PLAY_OUT_RATE ? ", resampled to 16 kHz" : "", speaker_muted() ? ", speaker off" : "");
            continue;
        }
        if (state == MG_PLAY_DONE) {
            /* Played out (keep), or the device gave up on a starved stream (drop). */
            if (nfifo) {
                mg_level_add(&spk, fifo, nfifo, speaker_muted());
                memset(fifo + nfifo, 0, (PLAY_CHUNK - nfifo % PLAY_CHUNK) % PLAY_CHUNK * sizeof(int16_t));
                speaker(fifo, (nfifo + PLAY_CHUNK - 1) / PLAY_CHUNK * PLAY_CHUNK);
                nfifo = 0;
            }
            for (int i = 0; i < PLAY_TAIL_CHUNKS; i++) {
                speaker(silence, PLAY_CHUNK);
            }
            mg_lock();
            bool keep = s_play.done_keep;
            mg_proto_stream_finished(&s_proto);
            mg_unlock();
            log_stream(keep ? "keep" : "starved", &pp, &st, dec, &spk);
            mg_dec_close(dec);
            free(dec);
            dec = NULL;
            continue;
        }
        if (n) {
            nfifo += mg_dec_run(dec, frames, n, fifo + nfifo, MG_PLAY_MAX_OUT + PLAY_CHUNK - nfifo);
        }
        if (nfifo >= PLAY_CHUNK) {
            size_t whole = nfifo / PLAY_CHUNK * PLAY_CHUNK;
            mg_level_add(&spk, fifo, whole, speaker_muted());
            speaker(fifo, whole);
            memmove(fifo, fifo + whole, (nfifo - whole) * sizeof(int16_t));
            nfifo -= whole;
        } else if (!n) {
            speaker(silence, PLAY_CHUNK);   /* prebuffering or starved (mg_play counts it) */
        }
    }
}

void mg_ble_setup_changed(void)
{
    uint8_t k[MG_TOKEN_PROOF_KEY_LEN];
    s_set_up = s_platform.proof_key && s_platform.proof_key(k);
    mg_token_proof_wipe(k, sizeof(k));
    if (s_ready) {
        mg_lock();
        link_check();
        mg_unlock();
    }
}

/* ---- pairing UX ---------------------------------------------------------- */

bool mg_ble_pairing_prompt(mg_ble_prompt_t *out)
{
    out->method = s_prompt_method;
    out->code = s_prompt_code;
    return out->method != 0;
}

bool mg_ble_confirm_press(void)
{
    if (!s_ready || !s_prompt_method) {
        return false;
    }
    ev_t ev = { .type = EV_CONFIRM, .ch = 1 };
    xQueueSend(s_events, &ev, 0);
    return true;
}

void mg_ble_open_pairing_window(void)
{
#if MG_WITH_SECURE
    s_window_until_us = esp_timer_get_time() + PAIRING_WINDOW_MS * 1000LL;
    s_attempts = 0;
    ESP_LOGI(TAG, "pairing mode for %d s", PAIRING_WINDOW_MS / 1000);
#endif
}

/* ---- init ---------------------------------------------------------------- */

#if CONFIG_MUSE_GADGET_BLE_QUEUE
static bool st_read(void *ctx, uint32_t off, void *buf, size_t n)
{
    return esp_partition_read(ctx, off, buf, n) == ESP_OK;
}
static bool st_write(void *ctx, uint32_t off, const void *buf, size_t n)
{
    return esp_partition_write(ctx, off, buf, n) == ESP_OK;
}
/* Each sector erase stops the cache, and with it everything running from
 * flash, for tens of ms: pause between sectors so a long clip's save doesn't
 * freeze the screen (and the rest of the worker's work) for a second. */
#define ERASE_PAUSE_MS 20

static bool st_erase(void *ctx, uint32_t off, size_t n)
{
    bool ok = esp_partition_erase_range(ctx, off, n) == ESP_OK;
    vTaskDelay(pdMS_TO_TICKS(ERASE_PAUSE_MS));
    return ok;
}

static void mount_queue(void)
{
    s_queue_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, QUEUE_PART_SUBTYPE, "mg_queue");
    if (!s_queue_part) {
        ESP_LOGI(TAG, "no mg_queue partition: offline clips off");
        return;
    }
    mg_store_t st = {
        .ctx = (void *)s_queue_part,
        .size = s_queue_part->size - s_queue_part->size % s_queue_part->erase_size,
        .sector = s_queue_part->erase_size,
        .read = st_read,
        .write = st_write,
        .erase = st_erase,
    };
    s_have_queue = mg_queue_mount(&s_queue, &st);
    if (s_have_queue) {
        ESP_LOGI(TAG, "offline queue: %u clips, %u of %u KB used", s_queue.count,
                 (unsigned)(mg_queue_used(&s_queue) / 1024), (unsigned)(mg_queue_capacity(&s_queue) / 1024));
    }
}
#endif

void mg_ble_init(const mg_ble_platform_t *platform)
{
    if (s_ready) {
        return;
    }
    if (platform) {
        s_platform = *platform;
    }
    s_lock = xSemaphoreCreateRecursiveMutex();
    s_events = xQueueCreate(16, sizeof(ev_t));
    if (!s_lock || !s_events) {
        ESP_LOGE(TAG, "no memory");
        return;
    }
#if CONFIG_MUSE_GADGET_BLE_QUEUE
    mount_queue();
#endif
    mg_settings_t settings;
    load_settings(&settings);
    mg_play_init(&s_play, CONFIG_MUSE_GADGET_BLE_PLAY_BUFFER);
    bool speaker = s_platform.speaker_write != NULL;
    mg_proto_config_t cfg = {
        .secure = MG_WITH_SECURE,
        .queue = mg_queue_get(),
        .play = speaker ? &s_play : NULL,
        .haptics = s_platform.vibrate != NULL,
        .assistant = s_platform.assistant_state != NULL,
    };
    mg_proto_ops_t ops = {
        .notify = op_notify,
        .disconnect = op_disconnect,
        .capture = op_capture,
        .mic_gain = op_mic_gain,
        .settings_changed = op_settings_changed,
        .now_ms = op_now,
        .play_kick = op_play_kick,
        .random = op_random,
#if MG_WITH_SECURE
        .pairing_find = op_pairing_find,
        .pairing_save = op_pairing_save,
        .pairing_forget = op_pairing_forget,
        .pairing_mode = op_pairing_mode,
        .pairing_attempt = op_pairing_attempt,
        .pairing_prompt = op_pairing_prompt,
#endif
    };
#if MG_WITH_SECURE
    if (!mg_crypto_init()) {
        ESP_LOGE(TAG, "PSA crypto unavailable");
        return;
    }
    if (s_platform.display) {
        cfg.methods[cfg.n_methods++] = mg_pairing_method_numeric_comparison;
    }
    cfg.methods[cfg.n_methods++] = mg_pairing_method_physical_confirm;
#endif
    if (speaker && s_platform.speaker_volume && s_platform.set_speaker_volume) {
        ops.speaker_volume = op_speaker_volume;
        ops.set_speaker_volume = op_set_speaker_volume;
    }
    if (cfg.assistant) {
        ops.assistant_state = op_assistant_state;
    }
    if (s_platform.proof_key && s_platform.proof_clear) {
        ops.proof_key = op_proof_key;
        ops.proof_clear = op_proof_clear;
        mg_ble_setup_changed();
        ESP_LOGI(TAG, "token proof: %s", s_set_up ? "set up" : "not set up");
    }
    mg_proto_init(&s_proto, &cfg, &ops, &settings);

    s_nus_ring = heap_caps_malloc(NUS_RING, MALLOC_CAP_8BIT);
    if (s_nus_ring) {
        s_prev_vprintf = esp_log_set_vprintf(nus_vprintf);
    }
    /* Each notification would log a line at info; audio sends dozens a second. */
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    if (xTaskCreate(worker, "mg_ble", WORKER_STACK, NULL, 5, &s_worker) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the worker");
        return;
    }
    /* Above the voice loop (6), so the speaker isn't starved while it encodes. */
    if (speaker && xTaskCreate(player, "mg_play", PLAYER_STACK, NULL, 7, &s_player) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the player");
        cfg.play = NULL;
        mg_proto_init(&s_proto, &cfg, &ops, &settings);
    }
    s_ready = true;
    ESP_LOGI(TAG, "musegadgets BLE ready: %s%s%s, queue %s, playback %s", "SBC", MG_WITH_LC3 ? "+LC3" : "",
             MG_WITH_SECURE ? ", secure" : "", s_have_queue ? "on" : "off", s_player ? "on" : "off");
    if (s_platform.refresh_advertising) {
        s_platform.refresh_advertising();
    }
    kick();
}
