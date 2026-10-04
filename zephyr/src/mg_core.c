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

#include "mg_core.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "mg_audio.h"
#include "mg_led.h"
#include "mg_session.h"
#include "mg_settings.h"
#include "mg_setup.h"
#include "mg_setup_store.h"
#include "mg_throughput.h"
#include "mg_token_proof.h"
#include "mg_transport.h"
#if defined(CONFIG_MG_AUDIO_QUEUE)
#include "mg_queue.h"
#endif

LOG_MODULE_REGISTER(mg_core, CONFIG_MG_LOG_LEVEL);

static struct {
	bool connected;
	bool ready;
	bool button;
	/* gesture button_down went out for the current press. */
	bool down_sent;
	/* Codec from the most recent start_mic on this connection, or 0. */
	uint8_t conn_codec;
	/* Push-to-talk capture is running for the current press. */
	bool ptt_live;
	bool recording;
	uint8_t mic_gain;
	/* A live capture for this client is running: its end is a turn for the assistant. */
	bool turn;
	/* The client sent assistant_state on this connection. */
	bool reports_state;
	/* A client's session has been up since boot. */
	bool ever_ready;
	enum mg_led_link link;
} c;

/* Ready only with a client connected, subscribed and push-to-talk on, as
 * on the ESP32 gadgets. */
void mg_core_link_refresh(void)
{
	enum mg_led_link l;

	if (c.connected && c.ready) {
		c.ever_ready = true;
		l = mg_settings_get()->push_to_talk_enabled ? MG_LED_LINK_READY : MG_LED_LINK_SESSION;
	} else if (c.connected) {
		l = MG_LED_LINK_CONNECTING;
	} else {
		l = c.ever_ready || mg_setup_store_complete() ? MG_LED_LINK_DISCONNECTED
							       : MG_LED_LINK_NEVER;
	}
	if (l != c.link) {
		LOG_INF("mg.ble: link %s -> %s", mg_led_link_name(c.link), mg_led_link_name(l));
		c.link = l;
	}
	mg_led_set_link(l);
}

#if defined(CONFIG_MG_ASSISTANT_STATE)
/* Clients that never send assistant_state: the turn the device started at
 * stop_mic (thinking) ends with done if nothing arrives in time. */
static struct k_work_delayable compat_work;

static void compat_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (c.connected && !c.reports_state && mg_led_face() == MG_LED_FACE_THINKING) {
		mg_led_set_face(MG_LED_FACE_DONE, "client sent no assistant_state");
	}
}
#endif

static void compat_cancel(void)
{
#if defined(CONFIG_MG_ASSISTANT_STATE)
	(void)k_work_cancel_delayable(&compat_work);
#endif
}

static void on_assistant_state(const uint8_t *p, size_t len)
{
	if (len < 1) {
		mg_core_send_error(mg_command_assistant_state, mg_error_code_invalid_length, NULL);
		return;
	}
	switch (p[0]) {
	case mg_assistant_state_idle:
		mg_led_set_face(MG_LED_FACE_IDLE, NULL);
		break;
	case mg_assistant_state_thinking:
		mg_led_set_face(MG_LED_FACE_THINKING, NULL);
		break;
	case mg_assistant_state_responding:
		mg_led_set_face(MG_LED_FACE_RESPONDING, NULL);
		break;
	case mg_assistant_state_done:
		mg_led_set_face(MG_LED_FACE_DONE, NULL);
		break;
	case mg_assistant_state_error:
		mg_led_set_face(MG_LED_FACE_ERROR, NULL);
		break;
	default:
		mg_core_send_error(mg_command_assistant_state, mg_error_code_invalid_value, NULL);
		return;
	}
	/* The client reports the turn from now on: no compat fallback. No reply. */
	c.reports_state = true;
	compat_cancel();
}

static void send(const uint8_t *buf, size_t len)
{
	(void)mg_session_send_control(buf, len);
}

void mg_core_send_error(uint8_t cmd, uint16_t code, const char *log)
{
	uint8_t buf[5 + 3 + 40];
	size_t n = 0;

	buf[n++] = mg_command_error;
	buf[n++] = cmd;
	buf[n++] = mg_error_data_code;
	sys_put_le16(code, &buf[n]);
	n += 2;
	if (log != NULL) {
		size_t l = MIN(strlen(log), sizeof(buf) - n - 3);

		buf[n++] = mg_error_data_log;
		sys_put_le16(l, &buf[n]);
		n += 2;
		memcpy(&buf[n], log, l);
		n += l;
	}
	LOG_DBG("error cmd %u code %u", cmd, code);
	send(buf, n);
}

void mg_core_send_status(void)
{
	struct mg_audio_format fmt;
	uint8_t buf[32];
	size_t n;

	mg_audio_current_format(&fmt);
	n = mg_codec_change_data_type(&fmt, buf);
	send(buf, n);

	n = 0;
	buf[n++] = mg_command_supported_features;
	/* Only commands the device accepts; notify-only ones aren't listed. */
	buf[n++] = mg_command_start_mic;
	buf[n++] = mg_command_stop_mic;
	buf[n++] = mg_command_set_mic_gain;
	buf[n++] = mg_command_request_status;
	buf[n++] = mg_command_get_settings;
	buf[n++] = mg_command_set_settings;
#if defined(CONFIG_MG_AUDIO_QUEUE)
	/* Listed while a firmware update holds the storage too: status says so. */
	if (mg_queue_present()) {
		buf[n++] = mg_command_audio_queue;
	}
#endif
	buf[n++] = mg_command_token_proof;
#if defined(CONFIG_MG_ASSISTANT_STATE)
	buf[n++] = mg_command_assistant_state;
#endif
	buf[n++] = mg_command_device_action;
	send(buf, n);

	n = 0;
	buf[n++] = mg_command_supported_features;
	buf[n++] = mg_command_sub_feature;
	buf[n++] = mg_command_start_mic;
	buf[n++] = mg_data_type_audio_sbc;
#if defined(CONFIG_MG_LC3)
	buf[n++] = mg_data_type_audio_lc3;
#endif
	send(buf, n);

	/* Device actions: the throughput test. */
	n = 0;
	buf[n++] = mg_command_supported_features;
	buf[n++] = mg_command_sub_feature;
	buf[n++] = mg_command_device_action;
	buf[n++] = mg_device_action_throughput_test;
	send(buf, n);

	/* Every setting get_settings and set_settings accept. */
	n = 0;
	buf[n++] = mg_command_supported_features;
	buf[n++] = mg_command_sub_feature;
	buf[n++] = mg_command_set_settings;
	for (unsigned int s = 0; s <= 0xff && n < sizeof(buf); s++) {
		if (mg_settings_accepted((uint8_t)s)) {
			buf[n++] = (uint8_t)s;
		}
	}
	send(buf, n);
}

static uint8_t ptt_codec(void)
{
	return c.conn_codec ? c.conn_codec : mg_settings_get()->audio_codec;
}

static void buzz(void)
{
	const struct mg_settings *st = mg_settings_get();

	if (st->haptics_enabled) {
		mg_led_buzz(st->ptt_buzz_freq_hz, st->ptt_buzz_duration_ms,
			    st->ptt_buzz_volume_percent);
	}
}

static void on_start_mic(const uint8_t *p, size_t len)
{
	uint8_t codec = len >= 1 && p[0] != mg_data_type_unknown ? p[0]
								  : mg_settings_get()->audio_codec;

	if (!mg_settings_codec_supported(codec)) {
		mg_core_send_error(mg_command_start_mic, mg_error_code_invalid_value,
				   "codec not supported");
		return;
	}
	if (mg_audio_activity() == MG_AUDIO_RECORDING) {
		mg_core_send_error(mg_command_start_mic, mg_error_code_busy, "recording clip");
		return;
	}
	if (mg_tp_running()) {
		mg_core_send_error(mg_command_start_mic, mg_error_code_busy, "throughput test");
		return;
	}
	c.conn_codec = codec;
	c.ptt_live = false;
	/* Live audio wins over a clip download: starting abandons it. */
	if (mg_audio_start_live(codec, false)) {
		mg_core_send_error(mg_command_start_mic, mg_error_code_busy, NULL);
	} else {
		c.turn = true;
	}
}

static void on_get_settings(const uint8_t *p, size_t len)
{
	static const uint8_t all[] = {
		mg_setting_parameter_spec_version,
		mg_setting_parameter_push_to_talk_enabled,
		mg_setting_parameter_haptics_enabled,
		mg_setting_parameter_ptt_buzz_freq_hz,
		mg_setting_parameter_ptt_buzz_duration_ms,
		mg_setting_parameter_ptt_buzz_volume_percent,
		mg_setting_parameter_audio_codec,
		mg_setting_parameter_audio_queue_enabled,
	};
	const struct mg_settings *st = mg_settings_get();
	uint8_t buf[1 + ARRAY_SIZE(all) * 3];
	uint8_t mine[ARRAY_SIZE(all)];
	size_t n = 0;

	if (len == 0) {
		/* No list: everything this device accepts. */
		len = 0;
		for (size_t i = 0; i < ARRAY_SIZE(all); i++) {
			if (mg_settings_accepted(all[i])) {
				mine[len++] = all[i];
			}
		}
		p = mine;
	}
	buf[n++] = mg_command_get_settings;
	for (size_t i = 0; i < len; i++) {
		size_t w = mg_settings_accepted(p[i])
				   ? mg_settings_encode(st, p[i], &buf[n], sizeof(buf) - n)
				   : 0;

		if (w == 0) {
			if (!mg_settings_accepted(p[i])) {
				mg_core_send_error(mg_command_get_settings,
						   mg_error_code_unsupported, "unknown setting");
			} else {
				mg_core_send_error(mg_command_get_settings,
						   mg_error_code_invalid_length, "too many");
			}
			return;
		}
		n += w;
	}
	send(buf, n);
}

static void on_set_settings(const uint8_t *p, size_t len)
{
	uint8_t bad = 0;

	if (len == 0) {
		mg_core_send_error(mg_command_set_settings, mg_error_code_invalid_length, NULL);
		return;
	}
	uint16_t err = mg_settings_apply(p, len, &bad);

	mg_core_link_refresh(); /* push-to-talk on or off */
	if (err) {
		char log[24];

		snprintk(log, sizeof(log), "setting %u", bad);
		mg_core_send_error(mg_command_set_settings, err, log);
	}
}

#if defined(CONFIG_MG_AUDIO_QUEUE)
static void send_queue_status(void)
{
	struct mg_queue_status st;
	uint8_t buf[2 + 1 + 4 + 4 + 2];

	mg_queue_get_status(&st);
	buf[0] = mg_command_audio_queue;
	buf[1] = mg_audio_queue_command_status;
	/* Off while a firmware update holds the storage (capacity 0 then too). */
	buf[2] = mg_settings_get()->audio_queue_enabled && mg_queue_ready();
	sys_put_le32(st.used_bytes, &buf[3]);
	sys_put_le32(st.capacity_bytes, &buf[7]);
	sys_put_le16(st.clip_count, &buf[11]);
	send(buf, sizeof(buf));
}

static void on_audio_queue(const uint8_t *p, size_t len)
{
	struct mg_clip_info ci;

	if (!mg_queue_present()) {
		mg_core_send_error(mg_command_audio_queue, mg_error_code_unsupported, "no storage");
		return;
	}
	if (len < 1) {
		mg_core_send_error(mg_command_audio_queue, mg_error_code_invalid_length, NULL);
		return;
	}
	if (!mg_queue_ready() && p[0] != mg_audio_queue_command_status) {
		mg_core_send_error(mg_command_audio_queue, mg_error_code_busy, "firmware update");
		return;
	}
	switch (p[0]) {
	case mg_audio_queue_command_status:
		send_queue_status();
		break;
	case mg_audio_queue_command_clear:
		if (mg_audio_activity() == MG_AUDIO_RECORDING) {
			mg_core_send_error(mg_command_audio_queue, mg_error_code_busy, NULL);
			return;
		}
		if (mg_audio_activity() == MG_AUDIO_CLIP) {
			mg_audio_stop();
		}
		if (mg_queue_clear()) {
			mg_core_send_error(mg_command_audio_queue, mg_error_code_busy, "erase failed");
			return;
		}
		send_queue_status();
		break;
	case mg_audio_queue_command_clip_info:
	case mg_audio_queue_command_read_clip: {
		if (len < 3) {
			mg_core_send_error(mg_command_audio_queue, mg_error_code_invalid_length,
					   NULL);
			return;
		}
		uint16_t idx = sys_get_le16(&p[1]);

		if (mg_queue_clip_info(idx, &ci)) {
			mg_core_send_error(mg_command_audio_queue, mg_error_code_invalid_value,
					   "no such clip");
			return;
		}
		if (p[0] == mg_audio_queue_command_clip_info) {
			uint8_t buf[2 + 2 + 1 + 4 + 4];

			buf[0] = mg_command_audio_queue;
			buf[1] = mg_audio_queue_command_clip_info;
			sys_put_le16(idx, &buf[2]);
			buf[4] = ci.codec;
			sys_put_le32(ci.start_ms, &buf[5]);
			sys_put_le32(ci.bytes, &buf[9]);
			send(buf, sizeof(buf));
			return;
		}
		enum mg_audio_activity a = mg_audio_activity();

		if (a == MG_AUDIO_LIVE || a == MG_AUDIO_RECORDING) {
			mg_core_send_error(mg_command_audio_queue, mg_error_code_busy, "live audio");
			return;
		}
		if (mg_audio_read_clip(idx)) {
			mg_core_send_error(mg_command_audio_queue, mg_error_code_busy, NULL);
		}
		break;
	}
	default:
		mg_core_send_error(mg_command_audio_queue, mg_error_code_unsupported, NULL);
		break;
	}
}
#endif

void mg_core_control_rx(const uint8_t *buf, size_t len)
{
	if (len == 0) {
		return;
	}
	const uint8_t *p = &buf[1];
	size_t plen = len - 1;

	switch (buf[0]) {
	case mg_command_request_status:
		mg_core_send_status();
		break;
	case mg_command_start_mic:
		on_start_mic(p, plen);
		break;
	case mg_command_stop_mic:
		if (mg_audio_activity() == MG_AUDIO_LIVE) {
			c.ptt_live = false;
			mg_audio_stop();
		}
		break;
	case mg_command_set_mic_gain:
		if (plen < 1) {
			mg_core_send_error(buf[0], mg_error_code_invalid_length, NULL);
		} else if (p[0] < 1 || p[0] > 100) {
			mg_core_send_error(buf[0], mg_error_code_invalid_value, NULL);
		} else {
			c.mic_gain = p[0];
			mg_audio_set_gain(p[0]);
		}
		break;
	case mg_command_get_settings:
		on_get_settings(p, plen);
		break;
	case mg_command_set_settings:
		on_set_settings(p, plen);
		break;
#if defined(CONFIG_MG_AUDIO_QUEUE)
	case mg_command_audio_queue:
		on_audio_queue(p, plen);
		break;
#endif
	case mg_command_token_proof:
		mg_token_proof_rx(p, plen);
		break;
	case mg_command_device_action:
		if (plen >= 1 && p[0] == mg_device_action_throughput_test) {
			mg_tp_command(&p[1], plen - 1);
		} else {
			mg_core_send_error(buf[0], plen ? mg_error_code_unsupported
							: mg_error_code_invalid_length,
					   NULL);
		}
		break;
	case mg_command_change_data_type:
		/* From a client only for the receive throughput test. */
		if (plen < 1 || !mg_tp_change_data_type(p[0])) {
			mg_core_send_error(buf[0], mg_error_code_unsupported, NULL);
		}
		break;
#if defined(CONFIG_MG_ASSISTANT_STATE)
	case mg_command_assistant_state:
		on_assistant_state(p, plen);
		break;
#endif
	default:
		mg_core_send_error(buf[0], mg_error_code_unsupported, NULL);
		break;
	}
}

static void send_gesture(uint8_t g)
{
	const uint8_t buf[] = {mg_command_gesture, g};

	send(buf, sizeof(buf));
}

void mg_core_button(bool pressed)
{
	if (pressed == c.button) {
		return;
	}
	c.button = pressed;

	if (pressed) {
		if (mg_setup_button()) {
			/* Confirmed Link setup; the release is swallowed too. */
			c.button = false;
			return;
		}
		if (c.ready) {
			send_gesture(mg_gesture_button_down);
			c.down_sent = true;
			if (mg_settings_get()->push_to_talk_enabled && mg_tp_running()) {
				LOG_WRN("push-to-talk skipped: a throughput test is running");
			} else if (mg_settings_get()->push_to_talk_enabled) {
				buzz();
				c.ptt_live = true;
				if (mg_audio_start_live(ptt_codec(), true)) {
					c.ptt_live = false;
				} else {
					c.turn = true;
				}
			}
			return;
		}
#if defined(CONFIG_MG_AUDIO_QUEUE)
		if (mg_settings_get()->audio_queue_enabled && !mg_queue_ready() &&
		    mg_queue_present()) {
			LOG_WRN("press not recorded: a firmware update holds the clip store");
		} else if (mg_settings_get()->audio_queue_enabled && mg_queue_ready()) {
			/* No client, so no per-connection haptics: the LED blinks. */
			if (mg_audio_start_recording(mg_settings_get()->audio_codec) == 0) {
				c.recording = true;
			}
		}
#endif
		return;
	}

	/* Released. */
	bool down_sent = c.down_sent;

	c.down_sent = false;
	if (c.recording) {
		c.recording = false;
		mg_audio_stop();
	}
	if (c.ready && down_sent) {
		send_gesture(mg_gesture_button_up);
	}
	if (c.ptt_live) {
		c.ptt_live = false;
		/* The audio thread sends stop_mic after the last chunk. */
		mg_audio_stop();
	}
}

void mg_core_audio_idle(void)
{
	/* Stale if something new was started after the audio thread went idle. */
	if (mg_audio_activity() != MG_AUDIO_IDLE) {
		return;
	}
	c.ptt_live = false;
	c.recording = false;
	if (c.turn) {
		/* stop_mic went out: the utterance is with the assistant. */
		c.turn = false;
#if defined(CONFIG_MG_ASSISTANT_STATE)
		if (c.ready) {
			/* The device shows thinking by itself (mgcommands.h); the
			 * client's assistant_state takes over. One that hasn't sent
			 * any on this connection gets done after a while instead. */
			mg_led_set_face(MG_LED_FACE_THINKING, "after stop_mic");
			if (!c.reports_state) {
				k_work_reschedule_for_queue(mg_app_wq(), &compat_work,
							    K_MSEC(CONFIG_MG_STATE_COMPAT_TIMEOUT_MS));
			}
		}
#endif
	}
}

void mg_core_connected(void)
{
	c.connected = true;
	c.conn_codec = 0;
	c.turn = false;
	c.reports_state = false;
	compat_cancel();
	mg_token_proof_connection_reset();
	mg_settings_new_connection();
	mg_core_link_refresh();
}

void mg_core_disconnected(void)
{
	c.connected = false;
	c.ready = false;
	c.conn_codec = 0;
	c.turn = false;
	c.reports_state = false;
	compat_cancel();
	mg_tp_reset();
	mg_token_proof_connection_reset();
	mg_settings_new_connection();
	if (mg_led_face() != MG_LED_FACE_IDLE) {
		mg_led_set_face(MG_LED_FACE_IDLE, "disconnected");
	}
	mg_core_link_refresh();
	enum mg_audio_activity a = mg_audio_activity();

	if (a == MG_AUDIO_LIVE || a == MG_AUDIO_CLIP) {
		c.ptt_live = false;
		mg_audio_stop();
	}
}

void mg_core_set_ready(bool ready)
{
	c.ready = ready;
	LOG_INF("session %s", ready ? "ready" : "not ready");
	mg_core_link_refresh();
	if (!ready && mg_audio_activity() == MG_AUDIO_LIVE) {
		c.ptt_live = false;
		mg_audio_stop();
	}
}

bool mg_core_is_ready(void)
{
	return c.ready;
}

bool mg_core_is_connected(void)
{
	return c.connected;
}

void mg_core_init(void)
{
	memset(&c, 0, sizeof(c));
	c.link = MG_LED_LINK_NEVER;
	c.mic_gain = CONFIG_MG_MIC_DEFAULT_GAIN;
	mg_audio_set_gain(c.mic_gain);
#if defined(CONFIG_MG_ASSISTANT_STATE)
	k_work_init_delayable(&compat_work, compat_fn);
#endif
	mg_tp_init();
}
