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

#include "mg_voice.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "mg_ble_priv.h"
#include "mg_codec.h"
#include "mg_config.h"
#include "mgcommands.h"

static const char *TAG = "mg.voice";

#define SLICE 320            /* samples encoded at a time: at most 3 SBC or 2 LC3 frames */
#define OUT_CAP 512
#define SEND_TRIES 6
#define QUEUE_MIN_ROOM 8192  /* a press needs at least this much queue left */
#define STAGE_MIN_S 3        /* without PSRAM: stage at least this much of a clip, or don't record */
#define STAGE_HEAP_KEEP (48 * 1024)   /* ...and leave this much internal RAM free */


#if MG_STANDALONE || CONFIG_MUSE_GADGET_BLE_ROUTE_BLE_ONLY
#define POLICY MG_POLICY_BLE_ONLY   /* standalone: there's no Wi-Fi path */
#elif CONFIG_MUSE_GADGET_BLE_ROUTE_WIFI_ONLY
#define POLICY MG_POLICY_WIFI_ONLY
#else
#define POLICY MG_POLICY_AUTO
#endif

static mg_encoder_t *s_enc;
static mg_route_t s_route = MG_ROUTE_NONE;
static bool s_from_client;
static int64_t s_start_us;
static uint32_t s_start_ms;
static uint8_t s_out[OUT_CAP];
static size_t s_nout;          /* encoded bytes waiting to go (BLE) */
static uint8_t *s_clip;        /* the clip being recorded (QUEUE) */
static size_t s_clip_len, s_clip_cap;
static uint32_t s_sent, s_dropped;

static void *big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}

mg_route_t mg_voice_route(bool wifi_ready)
{
    if (!mg_active()) {
        /* As if the option were off; standalone has nowhere else to send it. */
        return MG_STANDALONE ? MG_ROUTE_NONE : MG_ROUTE_WIFI;
    }
#if MG_STANDALONE
    wifi_ready = false;
#endif
    mg_route_inputs_t in = { .wifi_ready = wifi_ready };
    mg_lock();
    const mg_proto_t *p = mg_proto_get();
    mg_queue_t *q = mg_queue_get();
    in.client_connected = p->connected;
    in.client_ready = mg_proto_ready(p);
    in.ptt_enabled = mg_proto_ptt_enabled(p);
    in.queue_available = q && mg_queue_fits(q, QUEUE_MIN_ROOM);
    in.queue_enabled = mg_proto_settings(p)->queue_enabled;
    mg_unlock();
    mg_route_t r = mg_route_decide(POLICY, &in);
    ESP_LOGI(TAG, "press goes to %s", mg_route_name(r));
    return r;
}

bool mg_voice_gesture(bool down)
{
    if (!mg_active()) {
        return false;
    }
    mg_lock();
    bool sent = mg_proto_gesture(mg_proto_get(), down ? mg_gesture_button_down : mg_gesture_button_up);
    mg_unlock();
    return sent;
}

bool mg_voice_client_reports_state(void)
{
    if (!mg_active()) {
        return false;
    }
    mg_lock();
    bool r = mg_proto_get()->reports_state;
    mg_unlock();
    return r;
}

void mg_voice_face(const char *what)
{
    ESP_LOGI("mg.face", "%s", what);
}

void mg_voice_claim(bool claim)
{
    if (!mg_active()) {
        return;
    }
    mg_lock();
    mg_proto_claim_audio(mg_proto_get(), claim);
    mg_unlock();
    mg_play_kick();
}

bool mg_voice_playing(void)
{
    if (!mg_active()) {
        return false;
    }
    mg_lock();
    const mg_play_t *pl = mg_proto_get()->cfg.play;
    bool r = pl && mg_play_active(pl);
    mg_unlock();
    return r;
}

bool mg_voice_capture_requested(void)
{
    if (!mg_active()) {
        return false;
    }
    mg_lock();
    bool r = mg_capture_req()->requested && mg_proto_ready(mg_proto_get());
    mg_unlock();
    return r;
}

static bool begin(mg_route_t route, bool from_client);

bool mg_voice_begin(mg_route_t route, bool from_client)
{
    if (!mg_active()) {
        return false;
    }
    if (begin(route, from_client)) {
        return true;
    }
    if (from_client) {
        /* Don't keep asking the voice task for a capture that can't start. */
        mg_lock();
        mg_capture_req()->requested = false;
        mg_unlock();
    }
    return false;
}

static bool begin(mg_route_t route, bool from_client)
{
    if (route != MG_ROUTE_BLE && route != MG_ROUTE_QUEUE) {
        return false;
    }
    if (!s_enc && !(s_enc = big_alloc(sizeof(*s_enc)))) {
        ESP_LOGE(TAG, "no memory for the encoder");
        return false;
    }
    s_nout = 0;
    s_sent = s_dropped = 0;
    s_from_client = from_client;
    s_start_us = esp_timer_get_time();
    s_start_ms = mg_now_ms();

    mg_lock();
    mg_proto_t *p = mg_proto_get();
    const mg_settings_t *set = mg_proto_settings(p);
    uint8_t codec = route == MG_ROUTE_QUEUE ? set->codec
                    : from_client            ? mg_capture_req()->codec
                                             : mg_proto_capture_codec(p);
    bool haptics = route == MG_ROUTE_BLE && !from_client && mg_proto_haptics(p);
    uint16_t freq = set->buzz_freq_hz, ms = set->buzz_ms;
    uint8_t vol = set->buzz_volume;
    bool ready = route == MG_ROUTE_QUEUE || mg_proto_ready(p);
    mg_unlock();
    if (!ready || !mg_encoder_open(s_enc, codec)) {
        return false;
    }

    if (route == MG_ROUTE_QUEUE) {
        mg_audio_format_t f = s_enc->fmt;
        const size_t per_s = 1000000u / f.frame_us * f.frame_bytes;
        s_clip_cap = (size_t)MG_MAX_UTTERANCE_S * per_s;
        s_clip = heap_caps_malloc(s_clip_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_clip) {
            /* No PSRAM: stage what internal RAM can spare (about 8 s of SBC in 60 KB). */
            size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            size_t cap = largest > STAGE_HEAP_KEEP ? largest - STAGE_HEAP_KEEP : 0;
            cap = cap < s_clip_cap ? cap / f.frame_bytes * f.frame_bytes : s_clip_cap;
            if (cap >= STAGE_MIN_S * per_s && (s_clip = heap_caps_malloc(cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT))) {
                s_clip_cap = cap;
                ESP_LOGI(TAG, "staging up to %.1f s of the clip in internal RAM", (double)cap / per_s);
            }
        }
        if (!s_clip) {
            ESP_LOGW(TAG, "no memory to record a clip");
            return false;
        }
        s_clip_len = 0;
        s_route = route;
        return true;
    }

    if (haptics) {
        mg_vibrate(freq, ms, vol);   /* only boards with a vibration motor list haptics */
    }
    mg_lock();
    bool ok = mg_proto_live_begin(mg_proto_get(), codec);
    mg_unlock();
    if (ok) {
        s_route = route;
    }
    return ok;
}

/* Sends the whole frames in s_out, as few notifications as fit. */
static bool send_pending(void)
{
    const size_t fb = s_enc->fmt.frame_bytes;
    while (s_nout >= fb) {
        mg_lock();
        mg_proto_t *p = mg_proto_get();
        size_t fpc = mg_codec_frames_per_chunk(&s_enc->fmt, mg_proto_data_payload(p));
        bool live = mg_proto_live(p);
        mg_unlock();
        if (!live) {
            return false;
        }
        if (!fpc) {
            s_dropped += s_nout / fb;   /* MTU below MG_MIN_ATT_MTU: nothing fits */
            s_nout = 0;
            return true;
        }
        size_t n = fpc * fb < s_nout / fb * fb ? fpc * fb : s_nout / fb * fb;
        bool sent = false;
        for (int t = 0; t < SEND_TRIES && !sent; t++) {
            mg_lock();
            sent = mg_proto_send_data(mg_proto_get(), s_out, n);
            mg_unlock();
            if (!sent) {
                vTaskDelay(pdMS_TO_TICKS(4));   /* out of controller buffers */
            }
        }
        if (sent) {
            s_sent += n;
        } else {
            s_dropped += n / fb;
        }
        memmove(s_out, s_out + n, s_nout - n);
        s_nout -= n;
    }
    return true;
}

bool mg_voice_audio(const int16_t *pcm, size_t frames)
{
    if (s_route == MG_ROUTE_NONE) {
        return false;
    }
    if (esp_timer_get_time() - s_start_us > MG_MAX_UTTERANCE_S * 1000000LL) {
        return false;
    }
    while (frames) {
        size_t n = frames < SLICE ? frames : SLICE;
        if (s_route == MG_ROUTE_QUEUE) {
            size_t room = s_clip_cap - s_clip_len;
            s_clip_len += mg_encoder_feed(s_enc, pcm, n, s_clip + s_clip_len, room);
            if (s_clip_cap - s_clip_len < s_enc->fmt.frame_bytes * 3u) {
                return false;   /* full */
            }
        } else {
            s_nout += mg_encoder_feed(s_enc, pcm, n, s_out + s_nout, sizeof(s_out) - s_nout);
            if (!send_pending()) {
                return false;
            }
        }
        pcm += n;
        frames -= n;
    }
    if (s_route == MG_ROUTE_BLE && s_from_client) {
        mg_lock();
        bool want = mg_capture_req()->requested;
        mg_unlock();
        return want;
    }
    return true;
}

void mg_voice_end(bool keep)
{
    mg_route_t route = s_route;
    s_route = MG_ROUTE_NONE;
    if (route == MG_ROUTE_QUEUE) {
        s_clip_len += mg_encoder_flush(s_enc, s_clip + s_clip_len, s_clip_cap - s_clip_len);
        if (keep && s_clip_len) {
            ESP_LOGI(TAG, "recorded a %u-byte clip for the queue", (unsigned)s_clip_len);
            mg_queue_submit(s_enc->fmt.type, s_start_ms, s_clip, (uint32_t)s_clip_len);
        } else {
            free(s_clip);
        }
        s_clip = NULL;
        return;
    }
    if (route != MG_ROUTE_BLE) {
        return;
    }
    s_nout += mg_encoder_flush(s_enc, s_out + s_nout, sizeof(s_out) - s_nout);
    send_pending();
    mg_lock();
    if (s_from_client) {
        mg_capture_req()->requested = false;
    }
    /* stop_mic ends every capture, whichever side started or stopped it. */
    mg_proto_live_end(mg_proto_get(), true);
    mg_unlock();
    ESP_LOGI(TAG, "utterance over BLE: %.1f s, %u bytes, %u frames dropped",
             (esp_timer_get_time() - s_start_us) / 1e6, (unsigned)s_sent, (unsigned)s_dropped);
}
