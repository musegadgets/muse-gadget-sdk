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

#ifndef MG_SETTINGS_H_
#define MG_SETTINGS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mgcommands.h"

struct mg_settings {
	uint8_t push_to_talk_enabled;
	uint8_t haptics_enabled;
	uint16_t ptt_buzz_freq_hz;
	uint16_t ptt_buzz_duration_ms;
	uint8_t ptt_buzz_volume_percent;
	uint8_t audio_codec;
	uint8_t audio_queue_enabled;
};

/* The live settings. Change them only through mg_settings_apply(). */
const struct mg_settings *mg_settings_get(void);
void mg_settings_defaults(struct mg_settings *s);

/* Value width in bytes for a parameter, or 0 if the parameter is unknown. */
size_t mg_settings_value_size(uint8_t param);

/* The device lists and accepts it in get_settings and set_settings: haptics
 * and the ptt_buzz_* settings only with a vibration motor (CONFIG_MG_HAPTICS). */
bool mg_settings_accepted(uint8_t param);

/* Encodes one (param, value) pair; returns bytes written or 0 if unknown
 * or out of room. */
size_t mg_settings_encode(const struct mg_settings *s, uint8_t param, uint8_t *out, size_t room);

/*
 * Parses a set_settings payload (after the command byte) and applies each
 * pair in order. Pairs before an error stay applied. Returns 0 or a
 * mg_error_code_t; *bad_param gets the offending parameter.
 */
uint16_t mg_settings_apply(const uint8_t *buf, size_t len, uint8_t *bad_param);

/* True if codec is one this build can capture with. */
bool mg_settings_codec_supported(uint8_t codec);

/* Loads saved settings (call after settings_subsys_init()). */
int mg_settings_load(void);

/* push_to_talk_enabled and haptics_enabled are per connection: back to 0. */
void mg_settings_new_connection(void);

/* Test hook: forget the live settings and reset to defaults, not saved. */
void mg_settings_reset(void);

#endif
