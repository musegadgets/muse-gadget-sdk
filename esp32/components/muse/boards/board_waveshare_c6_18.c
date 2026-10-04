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

/*
 * Waveshare ESP32-C6-Touch-AMOLED-1.8: single-core C6 without PSRAM, 368x448
 * AMOLED (CO5300 or SH8601, the BSP detects which) with touch, one ES8311 for
 * speaker and mic, AXP2101 PMU. BOOT (GPIO9) talks; PWR (on the PMU) is the
 * power button, which also turns the board back on.
 * No PSRAM for Hatch's own TLS session or MP3, so voice notes go over Home
 * Link's session and replies come back as text (muse_chat_link.c).
 */
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"
#include "muse_pmu.h"

static const char *TAG = "board";

#define BOOT_GPIO GPIO_NUM_9
#define PMU_KEY_EVERY 2         /* poll the PMU over I2C every 20 ms */
/*
 * Two draw buffers, LVGL drawing one while the other goes out over QSPI. Each
 * strip costs a walk of the widget tree and a fresh start on every arc, label
 * and image crossing it, so fewer, taller strips draw a frame much faster.
 * Taller buffers trade RAM for fewer walks: 48 lines use about 47 KB more
 * than 16 across both buffers. Keep normal builds at 16 for offline audio;
 * BLE bench builds default to 48, with height configurable in sdkconfig.
 * Buffers remain adapter-owned for their entire lifetime.
 */
#define DRAW_BUF_LINES CONFIG_MUSE_C6_DRAW_BUF_LINES

static muse_gpio_button_t s_boot;

static esp_err_t init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, BOOT_GPIO), TAG, "boot button");
    if (muse_pmu_init(bsp_i2c_get_handle(), true) != ESP_OK) {
        ESP_LOGW(TAG, "no PMU: PWR button and battery unavailable");
    }
    return ESP_OK;
}

#if CONFIG_MUSE_PERF
#include "muse_perf.h"

/* Timestamps each touch interrupt for mg.perf, then hands it on to the adapter. */
static esp_lcd_touch_interrupt_callback_t s_touch_irq;

static void IRAM_ATTR touch_irq(esp_lcd_touch_handle_t tp)
{
    muse_perf_touch_irq();
    s_touch_irq(tp);
}

static void watch_touch_irq(esp_lcd_touch_handle_t tp)
{
    if (tp && tp->config.interrupt_callback) {
        s_touch_irq = tp->config.interrupt_callback;
        esp_lcd_touch_register_interrupt_callback_with_data(tp, touch_irq, tp->config.user_data);
    }
}
#endif

/*
 * LVGL runs through esp_lv_adapter like the S3 boards: its task on MUSE_UI_CORE
 * (the only core) at MUSE_UI_PRIORITY, beside the audio and BLE tasks. Two
 * changes for one core:
 *
 * - The draw thread renders while LVGL's task waits for it, so it gets the same
 *   priority (LVGL's Kconfig caps CONFIG_LV_DRAW_THREAD_PRIO at 4); below it,
 *   anything at 4 or 5 (BLE's worker, Link) would stall the frame mid-render.
 * - LVGL waits for each QSPI transfer on a semaphore the transfer-done
 *   interrupt gives. Otherwise it spins on a flag, which on one core takes the
 *   CPU from everything below the UI for every strip in flight.
 */
static lv_display_t *s_disp;
static SemaphoreHandle_t s_flush_done;

static bool IRAM_ATTR flush_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    (void)io;
    (void)ev;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    bool yield = esp_lv_adapter_display_notify_color_trans_done_from_isr(s_disp);
    return yield || woken == pdTRUE;
}

/* LVGL may skip flush_wait if the adapter IRQ already cleared flushing.
 * That leaves this binary semaphore's token unconsumed. FLUSH_START runs
 * after the previous transfer was released and BEFORE submitting the next:
 * discard its old token so it cannot release an in-flight future transfer. */
static void flush_start(lv_event_t *e)
{
    (void)e;
    xSemaphoreTake(s_flush_done, 0);
}

/* One give per flush: LVGL calls this only while a flush is outstanding. */
static void flush_wait(lv_display_t *disp)
{
    (void)disp;
    xSemaphoreTake(s_flush_done, portMAX_DELAY);
}

/* Both panels take whole pixel pairs (the BSP's rounder). */
static void round_area(lv_event_t *e)
{
    lv_area_t *area = lv_event_get_param(e);
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    TaskHandle_t draw = xTaskGetHandle("swdraw");   /* LVGL's software draw thread */
    if (draw) {
        vTaskPrioritySet(draw, MUSE_UI_PRIORITY);
    }

    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    if (bsp_display_new(NULL, &panel, &io) != ESP_OK) {
        return NULL;
    }
    s_flush_done = xSemaphoreCreateBinary();
    if (!s_flush_done) {
        return NULL;
    }
    esp_lv_adapter_set_default_display_idf_callback_registration_enabled(false);
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = DRAW_BUF_LINES,
            .use_psram = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    s_disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!s_disp) {
        return NULL;
    }
    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = flush_done };
    esp_lcd_panel_io_register_event_callbacks(io, &cbs, NULL);
    lv_display_set_flush_wait_cb(s_disp, flush_wait);
    lv_display_add_event_cb(s_disp, flush_start, LV_EVENT_FLUSH_START, NULL);
    /* Draw in the panel's byte order, so each strip goes out as drawn instead
     * of being swapped first (the adapter swaps only plain RGB565). */
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_add_event_cb(s_disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);

    esp_lcd_touch_handle_t tp = NULL;
    if (bsp_touch_new(NULL, &tp) == ESP_OK) {
        const esp_lv_adapter_touch_config_t tp_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(s_disp, tp);
        *touch = esp_lv_adapter_register_touch(&tp_cfg);
#if CONFIG_MUSE_PERF
        watch_touch_irq(tp);
#endif
    } else {
        ESP_LOGW(TAG, "no touch controller");
    }
    /* OLED brightness is a QSPI command on the same IO as pixel DMA, not
     * an independent PWM. Finish it before the worker can submit a flush:
     * SPI's bus lock does not serialize two tasks using the SAME device. */
    if (bsp_display_brightness_init() != ESP_OK) {
        return NULL;
    }
    if (esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    ESP_LOGI(TAG, "C6 display ready: %d-line buffers", DRAW_BUF_LINES);
    return s_disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    /* The adapter mutex is recursive: safe both from its LVGL timers and
     * from another task. Keep QSPI parameter commands out of pixel submits. */
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        bsp_display_brightness_set(pct);
        esp_lv_adapter_unlock();
    }
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    /* Set up I2S the way muse_audio opens it, instead of the BSP's mono 22 kHz default. */
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
        },
    };
    ESP_RETURN_ON_ERROR(bsp_audio_init(&std_cfg), TAG, "i2s");
    *spk = bsp_audio_codec_speaker_init();
    *mic = bsp_audio_codec_microphone_init();
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    static unsigned tick;
    unsigned ev = muse_gpio_button_poll(&s_boot);
    if (tick++ % PMU_KEY_EVERY == 0) {
        unsigned key = muse_pmu_poll_key();
        ev |= (key & MUSE_PMU_KEY_PRESS ? MUSE_BTN_AUX_PRESS : 0) |
              (key & MUSE_PMU_KEY_RELEASE ? MUSE_BTN_AUX_RELEASE : 0);
    }
    return ev;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-C6-Touch-AMOLED-1.8",
    .width = BSP_LCD_H_RES,
    .height = BSP_LCD_V_RES,
    .round = false,
    .touch = true,
    .diagonal_in = 1.8f,
    .talk_button = "boot",
    .aux_button = "pwr",
    /* Both buttons are on the right edge, about 100 px from the top and bottom. */
    .talk_hint = { LV_ALIGN_RIGHT_MID, -10, -124 },
    .aux_hint = { LV_ALIGN_RIGHT_MID, -10, 126 },
    .frame_ms = 50,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .audio_init = audio_init,
    .mic_slot = 0,              /* one mic, on the left slot */
    .poll_buttons = poll_buttons,
    .read_power = muse_pmu_read_power,
    .power_off = muse_pmu_power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
