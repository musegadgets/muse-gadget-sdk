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


#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "fakes.h"
#include "mg_connparam.h"
#include "mg_session.h"
#include "mg_throughput.h"
#include "mgcommands.h"

/* Through mg_session without session security; the secure suite covers
 * commands behind authentication. */
#if !defined(CONFIG_MG_SECURE)

#define RX(...)                                                                                    \
	do {                                                                                       \
		const uint8_t _b[] = {__VA_ARGS__};                                                \
		mg_session_rx(MG_ATT_CONTROL, _b, sizeof(_b));                                     \
	} while (0)

static void *setup(void)
{
	fake_init_once();
	return NULL;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	fake_connect();
	fake_reset();
	fake_audio_state = MG_AUDIO_IDLE;
}

static void after(void *f)
{
	ARG_UNUSED(f);
	fake_notify_delay_us = 0;
	mg_session_disconnected();
}

static const struct fake_note *report_note(void)
{
	for (int i = fake_nnotes - 1; i >= 0; i--) {
		const struct fake_note *n = &fake_notes[i];

		if (n->ch == MG_ATT_CONTROL && n->len == 19 && n->data[0] == mg_command_device_action) {
			return n;
		}
	}
	return NULL;
}

ZTEST(throughput, test_packet_format_and_counting)
{
	uint8_t p[244];
	struct mg_tp_count c = {0};
	uint8_t r[19];

	mg_tp_fill(p, sizeof(p), 0x01020304);
	zassert_equal(sys_get_le32(p), 0x01020304);
	zassert_equal(p[4], (uint8_t)(0x04 + 4));
	zassert_equal(p[243], (uint8_t)(0x04 + 243));

	/* Packets 0, 1, 2, then 5 (two missing), then a late 3: two gaps. */
	for (uint32_t s = 0; s < 3; s++) {
		mg_tp_fill(p, sizeof(p), s);
		zassert_true(mg_tp_account(&c, p, sizeof(p), 1000 + s * 5));
	}
	mg_tp_fill(p, sizeof(p), 5);
	zassert_true(mg_tp_account(&c, p, sizeof(p), 1020));
	mg_tp_fill(p, sizeof(p), 3);
	zassert_true(mg_tp_account(&c, p, sizeof(p), 1025));
	zassert_false(mg_tp_account(&c, p, 3, 1030), "too short for a sequence");
	zassert_equal(c.packets, 5);
	zassert_equal(c.bytes, 5 * 244);
	zassert_equal(c.gaps, 2);
	zassert_equal(mg_tp_report(&c, r), 19);
	zassert_equal(r[0], mg_command_device_action);
	zassert_equal(r[1], mg_device_action_throughput_test);
	zassert_equal(r[2], mg_throughput_test_stop);
	zassert_equal(sys_get_le32(&r[3]), 5 * 244);
	zassert_equal(sys_get_le32(&r[7]), 5);
	zassert_equal(sys_get_le32(&r[11]), 25, "first to last packet");
	zassert_equal(sys_get_le32(&r[15]), 2);
}

/* Receive: the client's change_data_type, then numbered Data writes; latency off. */
ZTEST(throughput, test_receive)
{
	uint8_t p[244];

	RX(mg_command_device_action, mg_device_action_throughput_test, mg_throughput_test_receive,
	   0, 0);
	zassert_true(mg_tp_running());
	zassert_equal(fake_nnotes, 0, "no reply when it starts");

	/* Data before change_data_type is not test data. */
	mg_tp_fill(p, sizeof(p), 99);
	mg_session_rx(MG_ATT_DATA, p, sizeof(p));
	RX(mg_command_change_data_type, mg_data_type_throughput_test);
	zassert_equal(fake_nnotes, 0);
	for (uint32_t s = 0; s < 10; s++) {
		if (s == 4) {
			continue; /* lost */
		}
		mg_tp_fill(p, sizeof(p), s);
		mg_session_rx(MG_ATT_DATA, p, sizeof(p));
		k_sleep(K_MSEC(15));
	}
	RX(mg_command_device_action, mg_device_action_throughput_test, mg_throughput_test_stop);
	zassert_false(mg_tp_running());
	const struct fake_note *n = report_note();

	zassert_not_null(n);
	zassert_equal(sys_get_le32(&n->data[3]), 9 * 244);
	zassert_equal(sys_get_le32(&n->data[7]), 9);
	zassert_true(sys_get_le32(&n->data[11]) >= 8 * 15, "%u ms", sys_get_le32(&n->data[11]));
	zassert_equal(sys_get_le32(&n->data[15]), 1, "one gap");

	/* After the test, Data is ignored again, and a type other than the test's is refused. */
	fake_reset();
	mg_session_rx(MG_ATT_DATA, p, sizeof(p));
	RX(mg_command_change_data_type, mg_data_type_audio_sbc);
	zassert_equal(fake_notes[0].data[0], mg_command_error);
}

/* Receive with a duration reports on its own. */
ZTEST(throughput, test_receive_duration)
{
	uint8_t p[100];

	RX(mg_command_device_action, mg_device_action_throughput_test, mg_throughput_test_receive,
	   1, 0);
	RX(mg_command_change_data_type, mg_data_type_throughput_test);
	mg_tp_fill(p, sizeof(p), 0);
	mg_session_rx(MG_ATT_DATA, p, sizeof(p));
	k_sleep(K_MSEC(1100));
	zassert_false(mg_tp_running());
	const struct fake_note *n = report_note();

	zassert_not_null(n, "reported after 1 s");
	zassert_equal(sys_get_le32(&n->data[7]), 1);
}

/* Send: change_data_type, numbered notifications until the duration, then the report. */
ZTEST(throughput, test_send)
{
	fake_notify_delay_us = 1000; /* a link taking a packet per ms (per tick, in practice) */
	RX(mg_command_device_action, mg_device_action_throughput_test, mg_throughput_test_send, 1,
	   0);
	zassert_equal(fake_notes[0].ch, MG_ATT_CONTROL);
	zassert_equal(fake_notes[0].data[0], mg_command_change_data_type);
	zassert_equal(fake_notes[0].data[1], mg_data_type_throughput_test);
	/* Busy while it runs: another test, and start_mic. */
	RX(mg_command_device_action, mg_device_action_throughput_test, mg_throughput_test_receive,
	   0, 0);
	zassert_equal(fake_notes[fake_nnotes - 1].data[0], mg_command_error);
	zassert_equal(sys_get_le16(&fake_notes[fake_nnotes - 1].data[3]), mg_error_code_busy);
	k_sleep(K_MSEC(1300));
	zassert_false(mg_tp_running(), "stopped after 1 s");
	const struct fake_note *n = report_note();

	zassert_not_null(n);
	uint32_t packets = sys_get_le32(&n->data[7]);

	/* native_sim's tick decides how long each paced send takes: 1 to 20 ms. */
	zassert_true(packets >= 45 && packets <= 1001, "%u packets", packets);
	zassert_equal(sys_get_le32(&n->data[3]), packets * 244, "MTU - 3 each");
	zassert_equal(sys_get_le32(&n->data[15]), 0);
	zassert_equal(fake_data_notes, (int)packets);

	/* Stop while sending: the report comes once. */
	fake_reset();
	fake_data_notes = 0;
	RX(mg_command_device_action, mg_device_action_throughput_test, mg_throughput_test_send, 0,
	   0);
	k_sleep(K_MSEC(50));
	RX(mg_command_device_action, mg_device_action_throughput_test, mg_throughput_test_stop);
	k_sleep(K_MSEC(20));
	int reports = 0;

	for (int i = 0; i < fake_nnotes; i++) {
		reports += fake_notes[i].len == 19 && fake_notes[i].data[0] == mg_command_device_action;
	}
	zassert_equal(reports, 1);
	zassert_false(mg_tp_running());
}

ZTEST(throughput, test_refused)
{
	/* Busy while audio is live. */
	fake_audio_state = MG_AUDIO_LIVE;
	RX(mg_command_device_action, mg_device_action_throughput_test, mg_throughput_test_send, 0,
	   0);
	zassert_equal(fake_notes[0].data[0], mg_command_error);
	zassert_equal(sys_get_le16(&fake_notes[0].data[3]), mg_error_code_busy);
	zassert_false(mg_tp_running());
	fake_audio_state = MG_AUDIO_IDLE;
	/* Unknown direction, unknown action, nothing at all. */
	fake_reset();
	RX(mg_command_device_action, mg_device_action_throughput_test, 7);
	zassert_equal(sys_get_le16(&fake_notes[0].data[3]), mg_error_code_invalid_value);
	fake_reset();
	RX(mg_command_device_action, 0);
	zassert_equal(sys_get_le16(&fake_notes[0].data[3]), mg_error_code_unsupported);
	fake_reset();
	RX(mg_command_device_action);
	zassert_equal(sys_get_le16(&fake_notes[0].data[3]), mg_error_code_invalid_length);
	/* A stop with nothing run reports zeros. */
	fake_reset();
	RX(mg_command_device_action, mg_device_action_throughput_test, mg_throughput_test_stop);
	zassert_not_null(report_note());
	/* A disconnect ends a receive test silently. */
	RX(mg_command_device_action, mg_device_action_throughput_test, mg_throughput_test_receive,
	   0, 0);
	mg_session_disconnected();
	zassert_false(mg_tp_running());
	fake_connect();
}

ZTEST_SUITE(throughput, NULL, setup, before, after, NULL);

#endif /* !CONFIG_MG_SECURE */
