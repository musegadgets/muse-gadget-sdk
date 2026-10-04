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

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "mg_app.h"
#include "mg_audio.h"
#include "mg_button.h"
#include "mg_core.h"
#include "mg_led.h"
#if defined(CONFIG_MG_DFU)
#include "mg_ota.h"
#endif
#include "mg_session.h"
#include "mg_settings.h"
#include "mg_setup.h"
#include "mg_setup_store.h"
#include "mg_transport.h"
#if defined(CONFIG_MG_AUDIO_QUEUE)
#include "mg_queue.h"
#endif
#if defined(CONFIG_MG_BATTERY_ADC)
#include "mg_battery.h"
#endif

LOG_MODULE_REGISTER(mg_main, CONFIG_MG_LOG_LEVEL);

/* Link setup runs P-256 ECDH (mbedTLS) on this queue. */
K_THREAD_STACK_DEFINE(app_stack, 6144);
static struct k_work_q app_wq;

struct k_work_q *mg_app_wq(void)
{
	return &app_wq;
}

const char *mg_app_version(void)
{
	return CONFIG_BT_DIS_FW_REV_STR;
}

static uint8_t battery = 100;
static int battery_mv = -ENODEV;
static struct k_work_delayable battery_work;

uint8_t mg_app_battery_percent(void)
{
	return battery;
}

int mg_app_battery_mv(void)
{
	return battery_mv;
}

/* Below this, no cell is connected (or it's flat beyond use): Battery Level
 * reports 100, as for a gadget without one. */
#define NO_BATTERY_MV 2500

static void battery_fn(struct k_work *w)
{
	ARG_UNUSED(w);
#if defined(CONFIG_MG_BATTERY_ADC)
	int mv = mg_battery_read_mv();

	battery_mv = mv;
	if (mv >= NO_BATTERY_MV) {
		battery = mg_battery_percent_from_mv(mv);
	} else {
		battery = 100;
	}
	LOG_DBG("battery %d mV, %u%%", mv, battery);
#endif
	mg_ble_set_battery(battery);
	k_work_reschedule(&battery_work, K_SECONDS(60));
}

/* Held at power-up and still held CONFIG_MG_RESET_HOLD_MS later: reset. */
static bool held_for_reset(void)
{
	LOG_INF("button held at power-up: keep holding %d s to reset setup",
		CONFIG_MG_RESET_HOLD_MS / 1000);
	for (int t = 0; t < CONFIG_MG_RESET_HOLD_MS; t += 100) {
		k_sleep(K_MSEC(100));
		if (!mg_button_held_at_boot()) {
			LOG_INF("released: no reset");
			return false;
		}
	}
	return true;
}

int main(void)
{
	bool held;

	LOG_INF("Muse Gadget %s starting (mg%d)", mg_app_version(), MG_SPEC_VERSION);
	k_work_queue_start(&app_wq, app_stack, K_THREAD_STACK_SIZEOF(app_stack),
			   K_PRIO_PREEMPT(5), NULL);
	k_thread_name_set(&app_wq.thread, "mg_app");

	mg_led_init();
	held = mg_button_held_at_boot();
	mg_button_init();

	if (settings_subsys_init()) {
		LOG_ERR("settings unavailable; using defaults");
	}
	(void)mg_settings_load();
	(void)mg_setup_store_load();
	if (held && held_for_reset()) {
		(void)mg_setup_factory_reset("button held at power-up");
	}
#if defined(CONFIG_MG_AUDIO_QUEUE)
	(void)mg_queue_init();
#endif
#if defined(CONFIG_MG_DFU)
	mg_ota_init();
#endif
	mg_core_init();
	mg_audio_init();

	if (mg_ble_init()) {
		return 0;
	}
	/* After BLE: the identity comes from the address. */
	mg_setup_init();
	mg_session_init();
	mg_core_link_refresh();
#if defined(CONFIG_MG_NUS)
	mg_nus_init();
#endif
	k_work_init_delayable(&battery_work, battery_fn);
	k_work_reschedule(&battery_work, K_NO_WAIT);
	int err = mg_ble_start_advertising();

#if defined(CONFIG_MG_DFU)
	if (!err) {
		/* Healthy: keep this image (and take the clip store back). */
		mg_ota_healthy();
	}
#endif
#if defined(CONFIG_MG_BENCH)
	mg_bench_init();
#endif
	ARG_UNUSED(err);
	return 0;
}
