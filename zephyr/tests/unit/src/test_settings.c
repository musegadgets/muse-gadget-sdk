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

#include <zephyr/settings/settings.h>
#include <zephyr/ztest.h>

#include "fakes.h"
#include "mg_settings.h"

static void *setup(void)
{
	fake_init_once();
	return NULL;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	mg_settings_reset();
}

ZTEST(settings, test_defaults)
{
	struct mg_settings d;

	mg_settings_defaults(&d);
	zassert_equal(d.push_to_talk_enabled, 0);
	zassert_equal(d.haptics_enabled, 0);
	zassert_equal(d.ptt_buzz_freq_hz, 210);
	zassert_equal(d.ptt_buzz_duration_ms, 100);
	zassert_equal(d.ptt_buzz_volume_percent, 80);
	zassert_equal(d.audio_codec, mg_data_type_audio_sbc);
	zassert_equal(d.audio_queue_enabled, 0);
}

ZTEST(settings, test_value_sizes)
{
	zassert_equal(mg_settings_value_size(mg_setting_parameter_spec_version), 1);
	zassert_equal(mg_settings_value_size(mg_setting_parameter_ptt_buzz_freq_hz), 2);
	zassert_equal(mg_settings_value_size(mg_setting_parameter_ptt_buzz_duration_ms), 2);
	zassert_equal(mg_settings_value_size(1), 0, "1 is reserved");
	zassert_equal(mg_settings_value_size(9), 0);
}

ZTEST(settings, test_apply_all)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_MG_HAPTICS);
	const uint8_t in[] = {
		mg_setting_parameter_push_to_talk_enabled, 1,
		mg_setting_parameter_haptics_enabled, 0,
		mg_setting_parameter_ptt_buzz_freq_hz, 0x10, 0x27, /* 10000 */
		mg_setting_parameter_ptt_buzz_duration_ms, 0xe8, 0x03, /* 1000 */
		mg_setting_parameter_ptt_buzz_volume_percent, 55,
		mg_setting_parameter_audio_codec, mg_data_type_audio_lc3,
		mg_setting_parameter_audio_queue_enabled, 1,
	};
	uint8_t bad;

	zassert_equal(mg_settings_apply(in, sizeof(in), &bad), 0);
	const struct mg_settings *s = mg_settings_get();

	zassert_equal(s->push_to_talk_enabled, 1);
	zassert_equal(s->haptics_enabled, 0);
	zassert_equal(s->ptt_buzz_freq_hz, 10000);
	zassert_equal(s->ptt_buzz_duration_ms, 1000);
	zassert_equal(s->ptt_buzz_volume_percent, 55);
	zassert_equal(s->audio_codec, mg_data_type_audio_lc3);
	zassert_equal(s->audio_queue_enabled, 1);
}

ZTEST(settings, test_unknown_stops_parsing)
{
	/* ptt=1 applies, then 9 is unknown and haptics=1 after it is not parsed. */
	const uint8_t in[] = {mg_setting_parameter_push_to_talk_enabled, 1, 9, 0,
			      mg_setting_parameter_haptics_enabled, 1};
	uint8_t bad;

	zassert_equal(mg_settings_apply(in, sizeof(in), &bad), mg_error_code_unsupported);
	zassert_equal(bad, 9);
	zassert_equal(mg_settings_get()->push_to_talk_enabled, 1);
	zassert_equal(mg_settings_get()->haptics_enabled, 0);
}

ZTEST(settings, test_invalid)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_MG_HAPTICS);
	uint8_t bad;
	const uint8_t trunc[] = {mg_setting_parameter_ptt_buzz_freq_hz, 0x10};
	const uint8_t lowfreq[] = {mg_setting_parameter_ptt_buzz_freq_hz, 19, 0};
	const uint8_t longdur[] = {mg_setting_parameter_ptt_buzz_duration_ms, 0x89, 0x13}; /* 5001 */
	const uint8_t zerodur[] = {mg_setting_parameter_ptt_buzz_duration_ms, 0, 0};
	const uint8_t vol[] = {mg_setting_parameter_ptt_buzz_volume_percent, 101};
	const uint8_t boolv[] = {mg_setting_parameter_push_to_talk_enabled, 2};
	const uint8_t opus[] = {mg_setting_parameter_audio_codec, mg_data_type_audio_opus};
	const uint8_t ro[] = {mg_setting_parameter_spec_version, 2};

	zassert_equal(mg_settings_apply(trunc, sizeof(trunc), &bad), mg_error_code_invalid_length);
	zassert_equal(mg_settings_apply(lowfreq, sizeof(lowfreq), &bad),
		      mg_error_code_invalid_value);
	zassert_equal(mg_settings_apply(longdur, sizeof(longdur), &bad),
		      mg_error_code_invalid_value);
	zassert_equal(mg_settings_apply(zerodur, sizeof(zerodur), &bad),
		      mg_error_code_invalid_value);
	zassert_equal(mg_settings_apply(vol, sizeof(vol), &bad), mg_error_code_invalid_value);
	zassert_equal(mg_settings_apply(boolv, sizeof(boolv), &bad), mg_error_code_invalid_value);
	zassert_equal(mg_settings_apply(opus, sizeof(opus), &bad), mg_error_code_invalid_value);
	zassert_equal(mg_settings_apply(ro, sizeof(ro), &bad), mg_error_code_invalid_value);

	struct mg_settings d;

	mg_settings_defaults(&d);
	const struct mg_settings *s = mg_settings_get();

	zassert_true(s->push_to_talk_enabled == d.push_to_talk_enabled &&
		     s->haptics_enabled == d.haptics_enabled &&
		     s->ptt_buzz_freq_hz == d.ptt_buzz_freq_hz &&
		     s->ptt_buzz_duration_ms == d.ptt_buzz_duration_ms &&
		     s->ptt_buzz_volume_percent == d.ptt_buzz_volume_percent &&
		     s->audio_codec == d.audio_codec &&
		     s->audio_queue_enabled == d.audio_queue_enabled,
		     "nothing changed");
}

ZTEST(settings, test_encode)
{
	uint8_t out[3];

	zassert_equal(mg_settings_encode(mg_settings_get(), mg_setting_parameter_spec_version, out,
					 sizeof(out)),
		      2);
	zassert_equal(out[1], MG_SPEC_VERSION);
	zassert_equal(mg_settings_encode(mg_settings_get(), mg_setting_parameter_ptt_buzz_freq_hz,
					 out, sizeof(out)),
		      3);
	zassert_equal(out[1] | (out[2] << 8), 210);
	zassert_equal(mg_settings_encode(mg_settings_get(), mg_setting_parameter_ptt_buzz_freq_hz,
					 out, 2),
		      0, "no room");
}

ZTEST(settings, test_persisted)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_MG_HAPTICS); /* stores the ptt_buzz_* settings */
	const uint8_t in[] = {mg_setting_parameter_push_to_talk_enabled, 1,
			      mg_setting_parameter_haptics_enabled, 1,
			      mg_setting_parameter_ptt_buzz_volume_percent, 42,
			      mg_setting_parameter_audio_codec, mg_data_type_audio_lc3,
			      mg_setting_parameter_audio_queue_enabled, 1};
	uint8_t bad;

	zassert_equal(mg_settings_apply(in, sizeof(in), &bad), 0);
	/* Forget the RAM copy, then load what was saved. */
	mg_settings_reset();
	zassert_equal(mg_settings_get()->push_to_talk_enabled, 0);
	zassert_equal(mg_settings_load(), 0);
	zassert_equal(mg_settings_get()->ptt_buzz_volume_percent, 42);
	zassert_equal(mg_settings_get()->audio_codec, mg_data_type_audio_lc3);
	zassert_equal(mg_settings_get()->audio_queue_enabled, 1);
	/* Per connection: never stored. */
	zassert_equal(mg_settings_get()->push_to_talk_enabled, 0);
	zassert_equal(mg_settings_get()->haptics_enabled, 0);

	/* Back to defaults in storage too, for the other suites. */
	const uint8_t back[] = {mg_setting_parameter_push_to_talk_enabled, 0,
				mg_setting_parameter_ptt_buzz_volume_percent, 80,
				mg_setting_parameter_audio_codec, mg_data_type_audio_sbc,
				mg_setting_parameter_audio_queue_enabled, 0};
	zassert_equal(mg_settings_apply(back, sizeof(back), &bad), 0);
}

ZTEST(settings, test_new_connection_resets_ptt_and_haptics)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_MG_HAPTICS); /* stores the ptt_buzz_* settings */
	const uint8_t in[] = {mg_setting_parameter_push_to_talk_enabled, 1,
			      mg_setting_parameter_haptics_enabled, 1,
			      mg_setting_parameter_ptt_buzz_duration_ms, 50, 0};
	uint8_t bad;

	zassert_equal(mg_settings_apply(in, sizeof(in), &bad), 0);
	mg_settings_new_connection();
	zassert_equal(mg_settings_get()->push_to_talk_enabled, 0);
	zassert_equal(mg_settings_get()->haptics_enabled, 0);
	zassert_equal(mg_settings_get()->ptt_buzz_duration_ms, 50, "persisted ones stay");
	const uint8_t back[] = {mg_setting_parameter_ptt_buzz_duration_ms, 100, 0};

	zassert_equal(mg_settings_apply(back, sizeof(back), &bad), 0);
}

ZTEST_SUITE(settings, NULL, setup, before, NULL, NULL);
