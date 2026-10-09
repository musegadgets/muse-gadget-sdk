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

#include "fakes.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "mg_led.h"
#include "mg_session.h"
#include "mg_settings.h"

struct fake_note fake_notes[FAKE_MAX_NOTES];
int fake_nnotes;
uint16_t fake_mtu = 247;
int fake_disconnects;
int fake_buzzes;
uint32_t fake_led_flags;

int fake_audio_starts;
uint8_t fake_audio_codec;
bool fake_audio_ptt;
int fake_audio_stops;
int fake_audio_recordings;
int fake_audio_clip_reads;
uint16_t fake_audio_clip;
uint8_t fake_audio_gain;
enum mg_audio_activity fake_audio_state;

void fake_reset(void)
{
	fake_nnotes = 0;
	fake_mtu = 247;
	fake_disconnects = 0;
	fake_buzzes = 0;
	fake_audio_starts = 0;
	fake_audio_codec = 0;
	fake_audio_ptt = false;
	fake_audio_stops = 0;
	fake_audio_recordings = 0;
	fake_audio_clip_reads = 0;
	fake_audio_state = MG_AUDIO_IDLE;
}

const struct fake_note *fake_find(uint8_t ch, uint8_t cmd, int nth)
{
	for (int i = 0; i < fake_nnotes; i++) {
		if (fake_notes[i].ch == ch && fake_notes[i].len > 0 &&
		    fake_notes[i].data[0] == cmd && nth-- == 0) {
			return &fake_notes[i];
		}
	}
	return NULL;
}

int fake_count(uint8_t ch, uint8_t cmd)
{
	int n = 0;

	while (fake_find(ch, cmd, n)) {
		n++;
	}
	return n;
}

void fake_connect(void)
{
	mg_session_connected();
	mg_session_subscribed(MG_ATT_CONTROL, true);
	mg_session_subscribed(MG_ATT_DATA, true);
#if defined(CONFIG_MG_SECURE)
	mg_session_subscribed(MG_ATT_ENC_CONTROL, true);
	mg_session_subscribed(MG_ATT_ENC_DATA, true);
#endif
}

/* Transport */

int fake_notify_delay_us;
int fake_data_notes;

int mg_transport_notify(enum mg_att_chan ch, const uint8_t *buf, size_t len, k_timeout_t wait)
{
	ARG_UNUSED(wait);
	if (ch == MG_ATT_DATA || ch == MG_ATT_ENC_DATA) {
		fake_data_notes++;
		if (fake_notify_delay_us) {
			k_sleep(K_USEC(fake_notify_delay_us));
			return 0; /* not kept: a test stream would fill the list */
		}
	}
	if (fake_nnotes == FAKE_MAX_NOTES || len > sizeof(fake_notes[0].data)) {
		return -ENOMEM;
	}
	fake_notes[fake_nnotes].ch = ch;
	fake_notes[fake_nnotes].len = len;
	memcpy(fake_notes[fake_nnotes].data, buf, len);
	fake_nnotes++;
	return 0;
}

void mg_transport_disconnect(void)
{
	fake_disconnects++;
}

uint16_t mg_transport_att_mtu(void)
{
	return fake_mtu;
}

const char *mg_transport_name(void)
{
	return "MuseGadget-A1B2C3";
}

const char *mg_app_version(void)
{
	return "1.0.0";
}

void mg_ble_refresh_advertising(void)
{
}

struct k_work_q *mg_app_wq(void)
{
	return &k_sys_work_q;
}

/* LED */

void mg_led_set(uint32_t flag, bool on)
{
	if (on) {
		fake_led_flags |= flag;
	} else {
		fake_led_flags &= ~flag;
	}
}

struct fake_conn_req fake_conn_reqs[16];
int fake_nconn_reqs;

int mg_transport_conn_params(uint16_t interval_min, uint16_t interval_max, uint16_t latency,
			     uint16_t timeout)
{
	if (fake_nconn_reqs < (int)ARRAY_SIZE(fake_conn_reqs)) {
		fake_conn_reqs[fake_nconn_reqs] = (struct fake_conn_req){
			interval_min, interval_max, latency, timeout};
	}
	fake_nconn_reqs++;
	return 0;
}

int fake_led_link;
int fake_led_face;
int fake_led_faces;

void mg_led_set_link(enum mg_led_link link)
{
	fake_led_link = link;
}

void mg_led_set_face(enum mg_led_face face, const char *why)
{
	ARG_UNUSED(why);
	fake_led_face = face;
	fake_led_faces++;
}

enum mg_led_face mg_led_face(void)
{
	return fake_led_face;
}

const char *mg_led_link_name(enum mg_led_link link)
{
	static const char *const n[] = {"never", "disconnected", "connecting", "session", "ready"};

	return link < ARRAY_SIZE(n) ? n[link] : "?";
}

void mg_led_buzz(uint16_t freq_hz, uint16_t duration_ms, uint8_t volume)
{
	ARG_UNUSED(freq_hz);
	ARG_UNUSED(duration_ms);
	ARG_UNUSED(volume);
	fake_buzzes++;
}

/* Audio thread */

int mg_audio_start_live(uint8_t codec, bool ptt)
{
	fake_audio_starts++;
	fake_audio_codec = codec;
	fake_audio_ptt = ptt;
	fake_audio_state = MG_AUDIO_LIVE;
	return 0;
}

int mg_audio_start_recording(uint8_t codec)
{
	fake_audio_recordings++;
	fake_audio_codec = codec;
	fake_audio_state = MG_AUDIO_RECORDING;
	return 0;
}

int mg_audio_read_clip(uint16_t index)
{
	fake_audio_clip_reads++;
	fake_audio_clip = index;
	fake_audio_state = MG_AUDIO_CLIP;
	return 0;
}

void mg_audio_stop(void)
{
	fake_audio_stops++;
	fake_audio_state = MG_AUDIO_IDLE;
}

enum mg_audio_activity mg_audio_activity(void)
{
	return fake_audio_state;
}

void mg_audio_current_format(struct mg_audio_format *fmt)
{
	(void)mg_codec_format(mg_settings_get()->audio_codec, fmt);
}

void mg_audio_set_gain(uint8_t gain)
{
	fake_audio_gain = gain;
}

/* One-time bring-up shared by the suites. */
#include <zephyr/settings/settings.h>

#include "mg_core.h"
#include "mg_identity.h"
#include "mg_queue.h"
#include "mg_setup.h"
#include "mg_setup_store.h"

void fake_init_once(void)
{
	static bool done;

	if (done) {
		return;
	}
	done = true;
	(void)settings_subsys_init();
	(void)mg_settings_load();
	(void)mg_setup_store_load();
	(void)mg_queue_init();
	mg_core_init();
	static const uint8_t addr[6] = {0x02, 0, 0, 0, 0, 0x01};

	mg_identity_init("MuseGadget-", addr);
	mg_setup_init();
	mg_session_init(false);
}
