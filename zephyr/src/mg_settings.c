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

#include "mg_settings.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(mg_settings, CONFIG_MG_LOG_LEVEL);

#define MG_SETTINGS_KEY     "mg/cfg"
#define MG_SETTINGS_VERSION 1

/* Stored blob: version byte, then the wire encoding of each persisted
 * parameter in this order. New parameters go at the end.
 * push_to_talk_enabled and haptics_enabled are per connection and never
 * stored. */
static const uint8_t persisted[] = {
	mg_setting_parameter_ptt_buzz_freq_hz,
	mg_setting_parameter_ptt_buzz_duration_ms,
	mg_setting_parameter_ptt_buzz_volume_percent,
	mg_setting_parameter_audio_codec,
	mg_setting_parameter_audio_queue_enabled,
};

static struct mg_settings live;
static bool loaded;

void mg_settings_defaults(struct mg_settings *s)
{
	s->push_to_talk_enabled = 0;
	s->haptics_enabled = 0;
	s->ptt_buzz_freq_hz = 210;
	s->ptt_buzz_duration_ms = 100;
	s->ptt_buzz_volume_percent = 80;
	s->audio_codec = mg_data_type_audio_sbc;
	s->audio_queue_enabled = 0;
}

const struct mg_settings *mg_settings_get(void)
{
	if (!loaded) {
		mg_settings_defaults(&live);
		loaded = true;
	}
	return &live;
}

void mg_settings_new_connection(void)
{
	struct mg_settings *s = (struct mg_settings *)mg_settings_get();

	s->push_to_talk_enabled = 0;
	s->haptics_enabled = 0;
}

void mg_settings_reset(void)
{
	mg_settings_defaults(&live);
	loaded = true;
}

bool mg_settings_codec_supported(uint8_t codec)
{
	return codec == mg_data_type_audio_sbc ||
	       (IS_ENABLED(CONFIG_MG_LC3) && codec == mg_data_type_audio_lc3);
}

size_t mg_settings_value_size(uint8_t param)
{
	switch (param) {
	case mg_setting_parameter_spec_version:
	case mg_setting_parameter_push_to_talk_enabled:
	case mg_setting_parameter_haptics_enabled:
	case mg_setting_parameter_ptt_buzz_volume_percent:
	case mg_setting_parameter_audio_codec:
	case mg_setting_parameter_audio_queue_enabled:
		return 1;
	case mg_setting_parameter_ptt_buzz_freq_hz:
	case mg_setting_parameter_ptt_buzz_duration_ms:
		return 2;
	default:
		return 0;
	}
}

bool mg_settings_accepted(uint8_t param)
{
	switch (param) {
	case mg_setting_parameter_haptics_enabled:
	case mg_setting_parameter_ptt_buzz_freq_hz:
	case mg_setting_parameter_ptt_buzz_duration_ms:
	case mg_setting_parameter_ptt_buzz_volume_percent:
		return IS_ENABLED(CONFIG_MG_HAPTICS);
	default:
		return mg_settings_value_size(param) != 0;
	}
}

size_t mg_settings_encode(const struct mg_settings *s, uint8_t param, uint8_t *out, size_t room)
{
	size_t n = mg_settings_value_size(param);

	if (n == 0 || room < 1 + n) {
		return 0;
	}
	out[0] = param;
	switch (param) {
	case mg_setting_parameter_spec_version:
		out[1] = MG_SPEC_VERSION;
		break;
	case mg_setting_parameter_push_to_talk_enabled:
		out[1] = s->push_to_talk_enabled;
		break;
	case mg_setting_parameter_haptics_enabled:
		out[1] = s->haptics_enabled;
		break;
	case mg_setting_parameter_ptt_buzz_freq_hz:
		sys_put_le16(s->ptt_buzz_freq_hz, &out[1]);
		break;
	case mg_setting_parameter_ptt_buzz_duration_ms:
		sys_put_le16(s->ptt_buzz_duration_ms, &out[1]);
		break;
	case mg_setting_parameter_ptt_buzz_volume_percent:
		out[1] = s->ptt_buzz_volume_percent;
		break;
	case mg_setting_parameter_audio_codec:
		out[1] = s->audio_codec;
		break;
	case mg_setting_parameter_audio_queue_enabled:
		out[1] = s->audio_queue_enabled;
		break;
	}
	return 1 + n;
}

/* Validates and stores one value. Returns 0 or a mg_error_code_t. */
static uint16_t set_one(struct mg_settings *s, uint8_t param, const uint8_t *v)
{
	uint16_t u16 = sys_get_le16(v);

	switch (param) {
	case mg_setting_parameter_spec_version:
		return mg_error_code_invalid_value; /* read-only */
	case mg_setting_parameter_push_to_talk_enabled:
	case mg_setting_parameter_haptics_enabled:
	case mg_setting_parameter_audio_queue_enabled:
		if (v[0] > 1) {
			return mg_error_code_invalid_value;
		}
		if (param == mg_setting_parameter_push_to_talk_enabled) {
			s->push_to_talk_enabled = v[0];
		} else if (param == mg_setting_parameter_haptics_enabled) {
			s->haptics_enabled = v[0];
		} else {
			s->audio_queue_enabled = v[0];
		}
		return 0;
	case mg_setting_parameter_ptt_buzz_freq_hz:
		if (u16 < 20 || u16 > 20000) {
			return mg_error_code_invalid_value;
		}
		s->ptt_buzz_freq_hz = u16;
		return 0;
	case mg_setting_parameter_ptt_buzz_duration_ms:
		if (u16 < 1 || u16 > 5000) {
			return mg_error_code_invalid_value;
		}
		s->ptt_buzz_duration_ms = u16;
		return 0;
	case mg_setting_parameter_ptt_buzz_volume_percent:
		if (v[0] > 100) {
			return mg_error_code_invalid_value;
		}
		s->ptt_buzz_volume_percent = v[0];
		return 0;
	case mg_setting_parameter_audio_codec:
		if (!mg_settings_codec_supported(v[0])) {
			return mg_error_code_invalid_value;
		}
		s->audio_codec = v[0];
		return 0;
	default:
		return mg_error_code_unsupported;
	}
}

static void save(void)
{
	uint8_t blob[1 + ARRAY_SIZE(persisted) * 3];
	size_t n = 0;

	blob[n++] = MG_SETTINGS_VERSION;
	for (size_t i = 0; i < ARRAY_SIZE(persisted); i++) {
		n += mg_settings_encode(&live, persisted[i], &blob[n], sizeof(blob) - n);
	}
	int err = settings_save_one(MG_SETTINGS_KEY, blob, n);

	if (err) {
		LOG_ERR("saving settings failed (%d)", err);
	}
}

uint16_t mg_settings_apply(const uint8_t *buf, size_t len, uint8_t *bad_param)
{
	struct mg_settings *s = (struct mg_settings *)mg_settings_get();
	struct mg_settings before = *s;
	uint16_t err = 0;
	size_t i = 0;

	while (i < len) {
		uint8_t param = buf[i];
		size_t n = mg_settings_value_size(param);

		*bad_param = param;
		if (n == 0 || !mg_settings_accepted(param)) {
			err = mg_error_code_unsupported;
			break;
		}
		if (i + 1 + n > len) {
			err = mg_error_code_invalid_length;
			break;
		}
		err = set_one(s, param, &buf[i + 1]);
		if (err) {
			break;
		}
		i += 1 + n;
	}
	if (memcmp(&before, s, sizeof(before)) != 0) {
		save();
	}
	return err;
}

static int load_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg, void *param)
{
	uint8_t blob[64];
	struct mg_settings *s = param;

	ARG_UNUSED(key);
	if (len < 1 || len > sizeof(blob)) {
		return 0;
	}
	ssize_t n = read_cb(cb_arg, blob, len);

	if (n < 1 || blob[0] != MG_SETTINGS_VERSION) {
		LOG_WRN("ignoring saved settings (version %d)", n > 0 ? blob[0] : -1);
		return 0;
	}
	/* Replays the stored pairs through the same validation as the wire,
	 * skipping any per-connection setting an older build stored. */
	for (size_t i = 1; i < (size_t)n;) {
		size_t w = mg_settings_value_size(blob[i]);

		if (w == 0 || i + 1 + w > (size_t)n) {
			break;
		}
		if (blob[i] != mg_setting_parameter_push_to_talk_enabled &&
		    blob[i] != mg_setting_parameter_haptics_enabled) {
			(void)set_one(s, blob[i], &blob[i + 1]);
		}
		i += 1 + w;
	}
	return 0;
}

int mg_settings_load(void)
{
	struct mg_settings *s = (struct mg_settings *)mg_settings_get();

	mg_settings_defaults(s);
	int err = settings_load_subtree_direct(MG_SETTINGS_KEY, load_cb, s);

	LOG_INF("settings: codec %u queue %u buzz %u Hz %u ms %u%%", s->audio_codec,
		s->audio_queue_enabled, s->ptt_buzz_freq_hz, s->ptt_buzz_duration_ms,
		s->ptt_buzz_volume_percent);
	return err;
}
