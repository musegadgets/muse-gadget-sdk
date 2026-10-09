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

#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/sys/ring_buffer.h>

#include "mg_app.h"
#include "mg_audio.h"
#include "mg_core.h"
#include "mg_session.h"
#include "mg_setup_store.h"
#include "mg_settings.h"
#include "mg_transport.h"
#if defined(CONFIG_MG_AUDIO_QUEUE)
#include "mg_queue.h"
#endif

/*
 * Nordic UART Service: TX mirrors the log, RX takes a few read-only console
 * commands. Any client can reach NUS (it sits outside session security), so
 * nothing here may start capture, press the button or touch setup or keys.
 */

RING_BUF_DECLARE(tx_ring, 2048);
static struct k_spinlock ring_lock;
/* Delayable, and never sleeping: it runs on the system work queue, which
 * the Bluetooth host also needs. */
static struct k_work_delayable tx_work;
static struct k_work rx_work;
static atomic_t enabled;
static char rx_line[64];
static size_t rx_len;
static K_MUTEX_DEFINE(rx_lock);

static void put(const uint8_t *data, size_t len)
{
	k_spinlock_key_t key = k_spin_lock(&ring_lock);

	(void)ring_buf_put(&tx_ring, data, len); /* drops what doesn't fit */
	k_spin_unlock(&ring_lock, key);
	if (atomic_get(&enabled)) {
		k_work_schedule(&tx_work, K_NO_WAIT);
	}
}

static void tx_fn(struct k_work *w)
{
	uint8_t buf[64];

	ARG_UNUSED(w);
	/* A few chunks per run, then yield the work queue. */
	for (int i = 0; i < 4 && atomic_get(&enabled); i++) {
		size_t room = MIN(sizeof(buf), (size_t)(mg_transport_att_mtu() - 3));
		k_spinlock_key_t key = k_spin_lock(&ring_lock);
		uint32_t n = ring_buf_peek(&tx_ring, buf, room);

		k_spin_unlock(&ring_lock, key);
		if (n == 0) {
			return;
		}
		if (bt_nus_send(NULL, buf, n) != 0) {
			/* No buffers now; try again shortly. */
			k_work_reschedule(&tx_work, K_MSEC(20));
			return;
		}
		key = k_spin_lock(&ring_lock);
		ring_buf_get(&tx_ring, NULL, n);
		k_spin_unlock(&ring_lock, key);
	}
	if (atomic_get(&enabled) && !ring_buf_is_empty(&tx_ring)) {
		k_work_reschedule(&tx_work, K_MSEC(5));
	}
}

static void reply(const char *fmt, ...)
{
	char line[96];
	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintk(line, sizeof(line) - 2, fmt, ap);

	va_end(ap);
	n = MIN(n, (int)sizeof(line) - 3);
	line[n++] = '\r';
	line[n++] = '\n';
	put((const uint8_t *)line, n);
}

static void rx_fn(struct k_work *w)
{
	char cmd[sizeof(rx_line)];

	ARG_UNUSED(w);
	k_mutex_lock(&rx_lock, K_FOREVER);
	memcpy(cmd, rx_line, sizeof(cmd));
	k_mutex_unlock(&rx_lock);

	const struct mg_settings *s = mg_settings_get();

	if (strcmp(cmd, "status") == 0) {
		reply("ready %d audio %d set_up %d pairing_mode %d", mg_core_is_ready(),
		      mg_audio_activity(), mg_setup_store_complete(), mg_session_pairing_mode());
	} else if (strcmp(cmd, "version") == 0) {
		reply("%s mg%d", mg_app_version(), MG_SPEC_VERSION);
	} else if (strcmp(cmd, "settings") == 0) {
		reply("ptt %u haptics %u buzz %u Hz %u ms %u%% codec %u queue %u",
		      s->push_to_talk_enabled, s->haptics_enabled, s->ptt_buzz_freq_hz,
		      s->ptt_buzz_duration_ms, s->ptt_buzz_volume_percent, s->audio_codec,
		      s->audio_queue_enabled);
#if defined(CONFIG_MG_AUDIO_QUEUE)
	} else if (strcmp(cmd, "queue") == 0) {
		struct mg_queue_status st;

		mg_queue_get_status(&st);
		reply("clips %u used %u of %u", st.clip_count, st.used_bytes, st.capacity_bytes);
#endif
	} else if (strcmp(cmd, "battery") == 0) {
		reply("battery %u%%", mg_app_battery_percent());
	} else {
		reply("commands: status version settings queue battery");
	}
}

static void received(struct bt_conn *conn, const void *data, uint16_t len, void *ctx)
{
	const char *p = data;

	ARG_UNUSED(conn);
	ARG_UNUSED(ctx);
	for (uint16_t i = 0; i < len; i++) {
		if (p[i] == '\r' || p[i] == '\n') {
			if (rx_len == 0) {
				continue;
			}
			k_mutex_lock(&rx_lock, K_FOREVER);
			rx_line[rx_len] = '\0';
			k_mutex_unlock(&rx_lock);
			rx_len = 0;
			k_work_submit_to_queue(mg_app_wq(), &rx_work);
		} else if (rx_len < sizeof(rx_line) - 1) {
			rx_line[rx_len++] = p[i];
		}
	}
}

static void notif_enabled(bool on, void *ctx)
{
	ARG_UNUSED(ctx);
	atomic_set(&enabled, on);
	if (on) {
		k_work_schedule(&tx_work, K_NO_WAIT);
	}
}

static struct bt_nus_cb nus_cb = {
	.notif_enabled = notif_enabled,
	.received = received,
};

/* Log backend feeding the ring. */

static uint8_t log_buf[128];

static int log_out(uint8_t *data, size_t length, void *ctx)
{
	ARG_UNUSED(ctx);
	put(data, length);
	return length;
}

LOG_OUTPUT_DEFINE(nus_log_output, log_out, log_buf, sizeof(log_buf));

static void log_process(const struct log_backend *const backend, union log_msg_generic *msg)
{
	ARG_UNUSED(backend);
	if (log_msg_get_level(&msg->log) > LOG_LEVEL_INF) {
		return;
	}
	log_format_func_t fmt = log_format_func_t_get(LOG_OUTPUT_TEXT);

	fmt(&nus_log_output, &msg->log, LOG_OUTPUT_FLAG_LEVEL | LOG_OUTPUT_FLAG_CRLF_NONE);
	put((const uint8_t *)"\r\n", 2);
}

static void log_panic(const struct log_backend *const backend)
{
	ARG_UNUSED(backend);
}

static void log_dropped(const struct log_backend *const backend, uint32_t cnt)
{
	ARG_UNUSED(backend);
	ARG_UNUSED(cnt);
}

static const struct log_backend_api nus_log_api = {
	.process = log_process,
	.panic = log_panic,
	.dropped = log_dropped,
};

LOG_BACKEND_DEFINE(mg_nus_log, nus_log_api, true);

void mg_nus_init(void)
{
	k_work_init_delayable(&tx_work, tx_fn);
	k_work_init(&rx_work, rx_fn);
	(void)bt_nus_cb_register(&nus_cb, NULL);
}
