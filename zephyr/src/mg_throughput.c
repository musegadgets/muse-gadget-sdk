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


#include "mg_throughput.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "mg_audio.h"
#include "mg_connparam.h"
#include "mg_core.h"
#include "mg_session.h"
#include "mg_transport.h"
#include "mgcommands.h"

LOG_MODULE_REGISTER(mg_tp, CONFIG_MG_LOG_LEVEL);

enum mode {
	IDLE,
	SENDING,
	RECEIVING,
};

static struct {
	atomic_t mode;
	bool armed;        /* receive: the client's change_data_type arrived */
	uint16_t duration_s;
	int64_t start_ms;
	struct mg_tp_count c;
	enum mode last;    /* the last test, for a stop after it ended */
} t;

static K_SEM_DEFINE(send_go, 0, 1);
static struct k_work_delayable rx_end;
static uint8_t pkt[244];

void mg_tp_fill(uint8_t *buf, size_t len, uint32_t seq)
{
	if (len < 4) {
		return;
	}
	sys_put_le32(seq, buf);
	for (size_t i = 4; i < len; i++) {
		buf[i] = (uint8_t)(seq + i);
	}
}

bool mg_tp_account(struct mg_tp_count *c, const uint8_t *buf, size_t len, int64_t now_ms)
{
	if (len < 4) {
		return false;
	}
	uint32_t seq = sys_get_le32(buf);

	if (c->packets == 0) {
		c->first_ms = now_ms;
		c->next_seq = seq;
	}
	if (seq > c->next_seq) {
		c->gaps += seq - c->next_seq;
	}
	if (seq >= c->next_seq) {
		c->next_seq = seq + 1;
	}
	c->bytes += len;
	c->packets++;
	c->last_ms = now_ms;
	return true;
}

size_t mg_tp_report(const struct mg_tp_count *c, uint8_t *out)
{
	out[0] = mg_command_device_action;
	out[1] = mg_device_action_throughput_test;
	out[2] = mg_throughput_test_stop;
	sys_put_le32(c->bytes, &out[3]);
	sys_put_le32(c->packets, &out[7]);
	sys_put_le32(c->packets ? (uint32_t)(c->last_ms - c->first_ms) : 0, &out[11]);
	sys_put_le32(c->gaps, &out[15]);
	return 19;
}

static void report(void)
{
	uint8_t m[19];
	uint32_t ms = t.c.packets ? (uint32_t)(t.c.last_ms - t.c.first_ms) : 0;

	LOG_INF("mg.ble: throughput %s: %u B in %u packets, %u ms (%u B/s), %u gaps",
		t.last == SENDING ? "send" : t.last == RECEIVING ? "receive" : "none", t.c.bytes,
		t.c.packets, ms, ms ? (uint32_t)((uint64_t)t.c.bytes * 1000 / ms) : 0, t.c.gaps);
	(void)mg_session_send_control(m, mg_tp_report(&t.c, m));
}

bool mg_tp_running(void)
{
	return atomic_get(&t.mode) != IDLE;
}

/* Ends the current test; with report, tells the client. */
static void finish(bool with_report)
{
	enum mode m = atomic_set(&t.mode, IDLE);

	(void)k_work_cancel_delayable(&rx_end);
	t.armed = false;
	if (m != IDLE) {
		t.last = m;
	}
	if (with_report) {
		report();
	}
}

static void rx_end_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (atomic_get(&t.mode) == RECEIVING) {
		finish(true);
	}
}

static void send_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		k_sem_take(&send_go, K_FOREVER);
		uint32_t seq = 0;

		while (atomic_get(&t.mode) == SENDING) {
			int64_t now = k_uptime_get();

			if (t.duration_s && now - t.start_ms >= t.duration_s * 1000LL) {
				break;
			}
			size_t n = MIN(mg_session_data_room(), sizeof(pkt));

			if (n < 4) {
				break;
			}
			mg_tp_fill(pkt, n, seq);
			/* Blocks while the notification queue is full: that's the pacing. */
			int err = mg_session_send_data(pkt, n, K_MSEC(200));

			if (err == -ENOMEM) {
				continue;
			}
			if (err) {
				LOG_WRN("throughput send stopped (%d)", err);
				break;
			}
			if (t.c.packets == 0) {
				t.c.first_ms = now;
			}
			t.c.bytes += n;
			t.c.packets++;
			t.c.last_ms = k_uptime_get();
			seq++;
		}
		/* Ended by the duration or an error: report; a stop command reports itself. */
		if (atomic_cas(&t.mode, SENDING, IDLE)) {
			t.last = SENDING;
			report();
		}
	}
}

K_THREAD_DEFINE(mg_tp_tx, 1536, send_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(8), 0, 0);

void mg_tp_command(const uint8_t *p, size_t len)
{
	uint8_t cmd = mg_command_device_action;

	if (len < 1) {
		mg_core_send_error(cmd, mg_error_code_invalid_length, NULL);
		return;
	}
	if (p[0] == mg_throughput_test_stop) {
		finish(true);
		return;
	}
	if (p[0] != mg_throughput_test_send && p[0] != mg_throughput_test_receive) {
		mg_core_send_error(cmd, mg_error_code_invalid_value, "direction");
		return;
	}
	if (mg_tp_running() || mg_audio_activity() != MG_AUDIO_IDLE) {
		mg_core_send_error(cmd, mg_error_code_busy, NULL);
		return;
	}
	memset(&t.c, 0, sizeof(t.c));
	t.duration_s = len >= 3 ? sys_get_le16(&p[1]) : 0;
	t.start_ms = k_uptime_get();
	t.armed = false;
	if (p[0] == mg_throughput_test_send) {
		const uint8_t cdt[] = {mg_command_change_data_type, mg_data_type_throughput_test};

		atomic_set(&t.mode, SENDING);
		LOG_INF("mg.ble: throughput send for %u s", t.duration_s);
		/* Data's type first, as for audio: the client knows what follows. */
		(void)mg_session_send_control(cdt, sizeof(cdt));
		k_sem_give(&send_go);
	} else {
		atomic_set(&t.mode, RECEIVING);
		LOG_INF("mg.ble: throughput receive for %u s", t.duration_s);
		mg_connparam_busy(MG_XFER_BENCH);
		if (t.duration_s) {
			k_work_reschedule_for_queue(mg_app_wq(), &rx_end, K_SECONDS(t.duration_s));
		}
	}
}

bool mg_tp_change_data_type(uint8_t type)
{
	if (type != mg_data_type_throughput_test || atomic_get(&t.mode) != RECEIVING) {
		return false;
	}
	t.armed = true;
	return true;
}

void mg_tp_data_rx(const uint8_t *buf, size_t len)
{
	if (atomic_get(&t.mode) != RECEIVING || !t.armed) {
		return;
	}
	mg_connparam_busy(MG_XFER_BENCH);
	(void)mg_tp_account(&t.c, buf, len, k_uptime_get());
}

void mg_tp_reset(void)
{
	finish(false);
}

void mg_tp_init(void)
{
	k_work_init_delayable(&rx_end, rx_end_fn);
}
