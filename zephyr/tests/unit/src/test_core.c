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
#include "mg_led.h"
#include "mg_core.h"
#include "mg_queue.h"
#include "mg_session.h"
#include "mg_settings.h"

/* Protocol engine through mg_session. */


static void rx(const uint8_t *buf, size_t len)
{
	mg_session_rx(MG_ATT_CONTROL, buf, len);
}

#define RX(...)                                                                                    \
	do {                                                                                       \
		const uint8_t _b[] = {__VA_ARGS__};                                                \
		rx(_b, sizeof(_b));                                                                \
	} while (0)

static void *setup(void)
{
	fake_init_once();
	return NULL;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	mg_settings_reset();
	fake_reset();
	fake_connect();
}

static void after(void *f)
{
	ARG_UNUSED(f);
	mg_core_button(false);
	mg_session_disconnected();
}

static void assert_error(uint8_t cmd, uint16_t code)
{
	const struct fake_note *n = fake_find(MG_ATT_CONTROL, mg_command_error, 0);

	zassert_not_null(n, "no error notified");
	zassert_true(n->len >= 5);
	zassert_equal(n->data[1], cmd);
	zassert_equal(n->data[2], mg_error_data_code);
	zassert_equal(sys_get_le16(&n->data[3]), code, "code %u", sys_get_le16(&n->data[3]));
}

ZTEST(core, test_ready_after_subscribe)
{
	zassert_true(mg_core_is_ready());
	mg_session_subscribed(MG_ATT_CONTROL, false);
	zassert_false(mg_core_is_ready());
	mg_session_subscribed(MG_ATT_CONTROL, true);
	zassert_true(mg_core_is_ready());
	/* Data matters too: the audio goes there. */
	mg_session_subscribed(MG_ATT_DATA, false);
	zassert_false(mg_core_is_ready());
	mg_session_subscribed(MG_ATT_DATA, true);
	zassert_true(mg_core_is_ready());
}

/* The LED and the "mg.ble: link" log follow the same rule as the ESP32
 * gadgets: ready only when connected, subscribed and push-to-talk on. */
ZTEST(core, test_link_states)
{
	zassert_equal(fake_led_link, MG_LED_LINK_SESSION, "subscribed, push-to-talk off");
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 1);
	zassert_equal(fake_led_link, MG_LED_LINK_READY);
	/* The app unsubscribes while the link stays up. */
	mg_session_subscribed(MG_ATT_DATA, false);
	zassert_equal(fake_led_link, MG_LED_LINK_CONNECTING);
	mg_session_subscribed(MG_ATT_DATA, true);
	zassert_equal(fake_led_link, MG_LED_LINK_READY, "push-to-talk is per connection: kept");
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 0);
	zassert_equal(fake_led_link, MG_LED_LINK_SESSION);
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 1);
	/* Any disconnect: never "never connected" again. */
	mg_session_disconnected();
	zassert_equal(fake_led_link, MG_LED_LINK_DISCONNECTED);
	/* A reconnection that never subscribes stays connecting, and push-to-talk is off. */
	mg_session_connected();
	zassert_equal(fake_led_link, MG_LED_LINK_CONNECTING);
	zassert_equal(mg_settings_get()->push_to_talk_enabled, 0);
	mg_session_disconnected();
	fake_connect();
	zassert_equal(fake_led_link, MG_LED_LINK_SESSION);
}

ZTEST(core, test_assistant_state)
{
	int faces = fake_led_faces;

	RX(mg_command_assistant_state, mg_assistant_state_responding);
	zassert_equal(fake_nnotes, 0, "no reply");
	zassert_equal(fake_led_face, MG_LED_FACE_RESPONDING);
	RX(mg_command_assistant_state, mg_assistant_state_done);
	zassert_equal(fake_led_face, MG_LED_FACE_DONE);
	RX(mg_command_assistant_state, mg_assistant_state_error);
	zassert_equal(fake_led_face, MG_LED_FACE_ERROR);
	RX(mg_command_assistant_state, mg_assistant_state_idle);
	zassert_equal(fake_led_face, MG_LED_FACE_IDLE);
	zassert_equal(fake_led_faces, faces + 4);
	RX(mg_command_assistant_state, 1); /* reserved */
	assert_error(mg_command_assistant_state, mg_error_code_invalid_value);
	fake_reset();
	RX(mg_command_assistant_state);
	assert_error(mg_command_assistant_state, mg_error_code_invalid_length);

	/* A client that has reported states: thinking once an utterance ends,
	 * and it stays there however long the reply takes to start. */
	RX(mg_command_start_mic);
	fake_audio_state = MG_AUDIO_IDLE;
	mg_core_audio_idle();
	zassert_equal(fake_led_face, MG_LED_FACE_THINKING);
	k_sleep(K_MSEC(CONFIG_MG_STATE_COMPAT_TIMEOUT_MS + 1000));
	zassert_equal(fake_led_face, MG_LED_FACE_THINKING, "no compat done for this client");

	/* A fresh connection, like the iOS app: it never sends thinking, so at
	 * stop_mic it hasn't sent any assistant_state yet. Thinking, not done. */
	mg_session_disconnected();
	zassert_equal(fake_led_face, MG_LED_FACE_IDLE, "idle on disconnect");
	fake_connect();
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 1);
	mg_core_button(true);
	mg_core_button(false);
	fake_audio_state = MG_AUDIO_IDLE;
	mg_core_audio_idle();
	zassert_equal(fake_led_face, MG_LED_FACE_THINKING, "thinking at stop_mic");
	/* responding 2.8 s later takes over; the compat timeout never fires. */
	k_sleep(K_MSEC(2800));
	RX(mg_command_assistant_state, mg_assistant_state_responding);
	zassert_equal(fake_led_face, MG_LED_FACE_RESPONDING);
	k_sleep(K_MSEC(CONFIG_MG_STATE_COMPAT_TIMEOUT_MS + 1000));
	zassert_equal(fake_led_face, MG_LED_FACE_RESPONDING, "no stray done");
	RX(mg_command_assistant_state, mg_assistant_state_done);
	zassert_equal(fake_led_face, MG_LED_FACE_DONE);

	/* An older client that never sends assistant_state: thinking, then done
	 * after the compat timeout (then idle, by the LED's own timer). */
	mg_session_disconnected();
	fake_connect();
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 1);
	mg_core_button(true);
	mg_core_button(false);
	fake_audio_state = MG_AUDIO_IDLE;
	mg_core_audio_idle();
	zassert_equal(fake_led_face, MG_LED_FACE_THINKING);
	k_sleep(K_MSEC(CONFIG_MG_STATE_COMPAT_TIMEOUT_MS - 1000));
	zassert_equal(fake_led_face, MG_LED_FACE_THINKING, "not yet");
	k_sleep(K_MSEC(2000));
	zassert_equal(fake_led_face, MG_LED_FACE_DONE, "compat done");

	/* A disconnect during thinking cancels the compat timer. */
	mg_core_button(true);
	mg_core_button(false);
	fake_audio_state = MG_AUDIO_IDLE;
	mg_core_audio_idle();
	zassert_equal(fake_led_face, MG_LED_FACE_THINKING);
	mg_session_disconnected();
	faces = fake_led_faces;
	k_sleep(K_MSEC(CONFIG_MG_STATE_COMPAT_TIMEOUT_MS + 1000));
	zassert_equal(fake_led_faces, faces, "nothing after the disconnect");
	zassert_equal(fake_led_face, MG_LED_FACE_IDLE);
	fake_connect();

	/* An audio idle without a capture (a clip download) is no turn. */
	faces = fake_led_faces;
	mg_core_audio_idle();
	zassert_equal(fake_led_faces, faces);
}

ZTEST(core, test_request_status)
{
	RX(mg_command_request_status);
	zassert_equal(fake_nnotes, 5);

	/* change_data_type with the SBC format. */
	const struct fake_note *n = &fake_notes[0];

	zassert_equal(n->len, 9);
	zassert_equal(n->data[0], mg_command_change_data_type);
	zassert_equal(n->data[1], mg_data_type_audio_sbc);
	zassert_equal(sys_get_le16(&n->data[2]), 16000);
	zassert_equal(n->data[4], 1);
	zassert_equal(sys_get_le16(&n->data[5]), 60);
	zassert_equal(sys_get_le16(&n->data[7]), 8000);

	/* Command list, then the typed start_mic codec list. */
	n = &fake_notes[1];
	zassert_equal(n->data[0], mg_command_supported_features);
	zassert_not_null(memchr(&n->data[1], mg_command_start_mic, n->len - 1));
	zassert_not_null(memchr(&n->data[1], mg_command_audio_queue, n->len - 1));
	zassert_not_null(memchr(&n->data[1], mg_command_set_settings, n->len - 1));
	zassert_is_null(memchr(&n->data[1], 0x80, n->len - 1), "0x80 is reserved");
	/* Only commands the device accepts. */
	zassert_is_null(memchr(&n->data[1], mg_command_gesture, n->len - 1));
	zassert_is_null(memchr(&n->data[1], mg_command_error, n->len - 1));
	zassert_is_null(memchr(&n->data[1], mg_command_change_data_type, n->len - 1));
	zassert_is_null(memchr(&n->data[1], mg_command_supported_features, n->len - 1));
	zassert_not_null(memchr(&n->data[1], mg_command_token_proof, n->len - 1));
	zassert_not_null(memchr(&n->data[1], mg_command_assistant_state, n->len - 1));
	zassert_not_null(memchr(&n->data[1], mg_command_device_action, n->len - 1));
	zassert_equal(n->len, 1 + 10,
		      "start/stop_mic, gain, status, get/set_settings, queue, token_proof, "
		      "assistant_state, device_action");

	n = &fake_notes[2];
	const uint8_t codecs[] = {mg_command_supported_features, mg_command_sub_feature,
				  mg_command_start_mic, mg_data_type_audio_sbc,
				  mg_data_type_audio_lc3};
	zassert_equal(n->len, sizeof(codecs));
	zassert_mem_equal(n->data, codecs, sizeof(codecs));

	/* Device actions: the throughput test. */
	n = &fake_notes[3];
	const uint8_t actions[] = {mg_command_supported_features, mg_command_sub_feature,
				   mg_command_device_action, mg_device_action_throughput_test};
	zassert_equal(n->len, sizeof(actions));
	zassert_mem_equal(n->data, actions, sizeof(actions));

	/* The typed settings list: exactly what get/set_settings accept. */
	n = &fake_notes[4];
#if defined(CONFIG_MG_HAPTICS)
	const uint8_t settings[] = {mg_command_supported_features, mg_command_sub_feature,
				    mg_command_set_settings, mg_setting_parameter_spec_version,
				    mg_setting_parameter_push_to_talk_enabled,
				    mg_setting_parameter_haptics_enabled,
				    mg_setting_parameter_ptt_buzz_freq_hz,
				    mg_setting_parameter_ptt_buzz_duration_ms,
				    mg_setting_parameter_ptt_buzz_volume_percent,
				    mg_setting_parameter_audio_codec,
				    mg_setting_parameter_audio_queue_enabled};
#else
	/* No vibration motor: no haptics or ptt_buzz_* settings. */
	const uint8_t settings[] = {mg_command_supported_features, mg_command_sub_feature,
				    mg_command_set_settings, mg_setting_parameter_spec_version,
				    mg_setting_parameter_push_to_talk_enabled,
				    mg_setting_parameter_audio_codec,
				    mg_setting_parameter_audio_queue_enabled};
#endif
	zassert_equal(n->len, sizeof(settings), "%u", n->len);
	zassert_mem_equal(n->data, settings, sizeof(settings));
}

ZTEST(core, test_unknown_command)
{
	RX(0x05);
	assert_error(0x05, mg_error_code_unsupported);
	fake_reset();
	RX(0x80, 0, 1, 1); /* reserved for a future security extension */
	assert_error(0x80, mg_error_code_unsupported);
}

ZTEST(core, test_start_stop_mic)
{
	RX(mg_command_start_mic);
	zassert_equal(fake_audio_starts, 1);
	zassert_equal(fake_audio_codec, mg_data_type_audio_sbc, "default codec");
	zassert_false(fake_audio_ptt);

	RX(mg_command_start_mic, mg_data_type_audio_lc3, 0xaa, 0xbb); /* trailing bytes ignored */
	zassert_equal(fake_audio_starts, 2);
	zassert_equal(fake_audio_codec, mg_data_type_audio_lc3);

	RX(mg_command_stop_mic);
	zassert_equal(fake_audio_stops, 1);
	zassert_equal(fake_nnotes, 0, "stop_mic comes from the audio thread, after the last chunk");

	RX(mg_command_start_mic, mg_data_type_audio_opus);
	assert_error(mg_command_start_mic, mg_error_code_invalid_value);
	zassert_equal(fake_audio_starts, 2);
}

ZTEST(core, test_mic_gain)
{
	RX(mg_command_set_mic_gain, 80);
	zassert_equal(fake_audio_gain, 80);
	RX(mg_command_set_mic_gain, 0);
	assert_error(mg_command_set_mic_gain, mg_error_code_invalid_value);
	fake_reset();
	RX(mg_command_set_mic_gain);
	assert_error(mg_command_set_mic_gain, mg_error_code_invalid_length);
}

#if !defined(CONFIG_MG_HAPTICS)
/* A board without a vibration motor refuses the haptics settings. */
ZTEST(core, test_no_motor)
{
	RX(mg_command_get_settings, mg_setting_parameter_ptt_buzz_freq_hz);
	assert_error(mg_command_get_settings, mg_error_code_unsupported);
	fake_reset();
	RX(mg_command_get_settings);
	zassert_equal(fake_notes[0].len, 1 + 4 * 2, "spec, ptt, codec, queue");
	zassert_is_null(memchr(&fake_notes[0].data[1], mg_setting_parameter_haptics_enabled, 8));
	for (uint8_t p = mg_setting_parameter_haptics_enabled;
	     p <= mg_setting_parameter_ptt_buzz_volume_percent; p++) {
		uint8_t v[3] = {p, 1, 0};

		fake_reset();
		const uint8_t m[] = {mg_command_set_settings, v[0], v[1], v[2]};

		rx(m, 2 + mg_settings_value_size(p));
		assert_error(mg_command_set_settings, mg_error_code_unsupported);
	}
	/* Push-to-talk works, without the activation buzz. */
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 1,
	   mg_setting_parameter_haptics_enabled, 1);
	zassert_equal(mg_settings_get()->push_to_talk_enabled, 1, "applied before the refusal");
	mg_core_button(true);
	zassert_equal(fake_audio_starts, 1);
	zassert_equal(fake_buzzes, 0);
	mg_core_button(false);
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 0);
}
#endif

ZTEST(core, test_get_settings)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_MG_HAPTICS);
	RX(mg_command_get_settings, mg_setting_parameter_spec_version,
	   mg_setting_parameter_ptt_buzz_freq_hz, mg_setting_parameter_audio_codec);
	const uint8_t want[] = {mg_command_get_settings,
				mg_setting_parameter_spec_version, MG_SPEC_VERSION,
				mg_setting_parameter_ptt_buzz_freq_hz, 210, 0,
				mg_setting_parameter_audio_codec, mg_data_type_audio_sbc};

	zassert_equal(fake_nnotes, 1);
	zassert_equal(fake_notes[0].len, sizeof(want));
	zassert_mem_equal(fake_notes[0].data, want, sizeof(want));

	fake_reset();
	RX(mg_command_get_settings);
	zassert_equal(fake_notes[0].len, 1 + 6 * 2 + 2 * 3, "all eight settings");

	fake_reset();
	RX(mg_command_get_settings, 1);
	assert_error(mg_command_get_settings, mg_error_code_unsupported);
}

ZTEST(core, test_set_settings)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_MG_HAPTICS);
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 1,
	   mg_setting_parameter_audio_codec, mg_data_type_audio_lc3);
	zassert_equal(fake_nnotes, 0, "no reply on success");
	zassert_equal(mg_settings_get()->push_to_talk_enabled, 1);
	zassert_equal(mg_settings_get()->audio_codec, mg_data_type_audio_lc3);

	RX(mg_command_set_settings, 0x7f, 1);
	assert_error(mg_command_set_settings, mg_error_code_unsupported);
	fake_reset();
	RX(mg_command_set_settings, mg_setting_parameter_haptics_enabled);
	assert_error(mg_command_set_settings, mg_error_code_invalid_length);
	fake_reset();
	RX(mg_command_set_settings, mg_setting_parameter_ptt_buzz_volume_percent, 200);
	assert_error(mg_command_set_settings, mg_error_code_invalid_value);

	/* The saved codec is used for device-initiated capture. */
	fake_reset();
	mg_core_button(true);
	zassert_equal(fake_audio_codec, mg_data_type_audio_lc3);
	mg_core_button(false);

	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 0,
	   mg_setting_parameter_audio_codec, mg_data_type_audio_sbc);
}

ZTEST(core, test_ptt_disabled_reports_gestures)
{
	mg_core_button(true);
	mg_core_button(false);
	zassert_equal(fake_nnotes, 2);
	zassert_equal(fake_notes[0].data[0], mg_command_gesture);
	zassert_equal(fake_notes[0].data[1], mg_gesture_button_down);
	zassert_equal(fake_notes[1].data[1], mg_gesture_button_up);
	zassert_equal(fake_audio_starts, 0);
	zassert_equal(fake_buzzes, 0);
}

ZTEST(core, test_ptt)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_MG_HAPTICS);
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 1,
	   mg_setting_parameter_haptics_enabled, 1);
	mg_core_button(true);
	zassert_equal(fake_count(MG_ATT_CONTROL, mg_command_gesture), 1);
	zassert_equal(fake_notes[0].data[1], mg_gesture_button_down);
	zassert_equal(fake_buzzes, 1, "activation buzz (LED)");
	zassert_equal(fake_audio_starts, 1);
	zassert_true(fake_audio_ptt);
	zassert_equal(fake_audio_codec, mg_data_type_audio_sbc);

	mg_core_button(false);
	zassert_equal(fake_notes[1].data[0], mg_command_gesture);
	zassert_equal(fake_notes[1].data[1], mg_gesture_button_up);
	zassert_equal(fake_audio_stops, 1);

	/* Haptics off: no buzz. The last start_mic codec wins. */
	RX(mg_command_set_settings, mg_setting_parameter_haptics_enabled, 0);
	RX(mg_command_start_mic, mg_data_type_audio_lc3);
	RX(mg_command_stop_mic);
	mg_core_button(true);
	zassert_equal(fake_buzzes, 1);
	zassert_equal(fake_audio_codec, mg_data_type_audio_lc3);
	mg_core_button(false);

	/* A new connection forgets that codec, and push-to-talk is off again
	 * until the client enables it. */
	mg_session_disconnected();
	fake_connect();
	zassert_equal(mg_settings_get()->push_to_talk_enabled, 0);
	zassert_equal(mg_settings_get()->haptics_enabled, 0);
	int starts = fake_audio_starts;

	mg_core_button(true);
	mg_core_button(false);
	zassert_equal(fake_audio_starts, starts, "gestures only");
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 1);
	mg_core_button(true);
	zassert_equal(fake_audio_codec, mg_data_type_audio_sbc);
	mg_core_button(false);
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 0,
	   mg_setting_parameter_haptics_enabled, 1);
}

ZTEST(core, test_offline_recording)
{
	RX(mg_command_set_settings, mg_setting_parameter_audio_queue_enabled, 1);
	mg_session_disconnected();
	fake_reset();

	mg_core_button(true);
	zassert_equal(fake_audio_recordings, 1);
	zassert_equal(fake_nnotes, 0, "no gestures without a client");
	mg_core_button(false);
	zassert_equal(fake_audio_stops, 1);

	/* Disabled: nothing recorded. */
	fake_connect();
	RX(mg_command_set_settings, mg_setting_parameter_audio_queue_enabled, 0);
	mg_session_disconnected();
	mg_core_button(true);
	mg_core_button(false);
	zassert_equal(fake_audio_recordings, 1);
	fake_connect();
}

ZTEST(core, test_audio_queue_commands)
{
	struct mg_queue_status st;

	zassert_equal(mg_queue_clear(), 0);
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 1234), 0);
	zassert_equal(mg_queue_append("0123456789", 10), 0);
	zassert_equal(mg_queue_end(), 0);
	mg_queue_get_status(&st);

	RX(mg_command_audio_queue, mg_audio_queue_command_status);
	const struct fake_note *n = fake_find(MG_ATT_CONTROL, mg_command_audio_queue, 0);

	zassert_not_null(n);
	zassert_equal(n->len, 13);
	zassert_equal(n->data[1], mg_audio_queue_command_status);
	zassert_equal(n->data[2], 0, "queue disabled");
	zassert_equal(sys_get_le32(&n->data[3]), st.used_bytes);
	zassert_equal(sys_get_le32(&n->data[7]), st.capacity_bytes);
	zassert_equal(sys_get_le16(&n->data[11]), 1);

	fake_reset();
	RX(mg_command_audio_queue, mg_audio_queue_command_clip_info, 0, 0);
	n = fake_find(MG_ATT_CONTROL, mg_command_audio_queue, 0);
	zassert_not_null(n);
	zassert_equal(n->len, 13);
	zassert_equal(n->data[1], mg_audio_queue_command_clip_info);
	zassert_equal(sys_get_le16(&n->data[2]), 0);
	zassert_equal(n->data[4], mg_data_type_audio_sbc);
	zassert_equal(sys_get_le32(&n->data[5]), 1234);
	zassert_equal(sys_get_le32(&n->data[9]), 10);

	fake_reset();
	RX(mg_command_audio_queue, mg_audio_queue_command_clip_info, 1, 0);
	assert_error(mg_command_audio_queue, mg_error_code_invalid_value);
	fake_reset();
	RX(mg_command_audio_queue, mg_audio_queue_command_clip_info, 0);
	assert_error(mg_command_audio_queue, mg_error_code_invalid_length);
	fake_reset();
	RX(mg_command_audio_queue, 2);
	assert_error(mg_command_audio_queue, mg_error_code_unsupported);

	fake_reset();
	RX(mg_command_audio_queue, mg_audio_queue_command_read_clip, 0, 0);
	zassert_equal(fake_audio_clip_reads, 1);
	zassert_equal(fake_audio_clip, 0);

	/* Live audio wins: read_clip is busy while it runs, start_mic abandons
	 * a download. */
	RX(mg_command_start_mic);
	zassert_equal(fake_audio_state, MG_AUDIO_LIVE);
	fake_reset();
	fake_audio_state = MG_AUDIO_LIVE;
	RX(mg_command_audio_queue, mg_audio_queue_command_read_clip, 0, 0);
	assert_error(mg_command_audio_queue, mg_error_code_busy);
	RX(mg_command_stop_mic);

	fake_reset();
	RX(mg_command_audio_queue, mg_audio_queue_command_clear);
	n = fake_find(MG_ATT_CONTROL, mg_command_audio_queue, 0);
	zassert_not_null(n);
	zassert_equal(n->data[1], mg_audio_queue_command_status);
	zassert_equal(sys_get_le16(&n->data[11]), 0, "cleared");
	zassert_equal(sys_get_le32(&n->data[3]), 0);
}

ZTEST(core, test_error_log_string)
{
	RX(mg_command_start_mic, mg_data_type_audio_pcm);
	const struct fake_note *n = fake_find(MG_ATT_CONTROL, mg_command_error, 0);

	zassert_not_null(n);
	zassert_true(n->len > 8);
	zassert_equal(n->data[5], mg_error_data_log);
	uint16_t l = sys_get_le16(&n->data[6]);

	zassert_equal(n->len, 8 + l);
	zassert_mem_equal(&n->data[8], "codec not supported", l);
}

ZTEST_SUITE(core, NULL, setup, before, after, NULL);

