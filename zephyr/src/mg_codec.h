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

#ifndef MG_CODEC_H_
#define MG_CODEC_H_

#include <stddef.h>
#include <stdint.h>

#include "mgcommands.h"

#define MG_CODEC_MAX_FRAME_SAMPLES 160
#define MG_CODEC_MAX_FRAME_BYTES   400

/* Format parameters announced in change_data_type. */
struct mg_audio_format {
	uint8_t type;         /* mg_data_type_t */
	uint16_t sample_rate; /* Hz */
	uint8_t channels;
	uint16_t frame_bytes; /* 0 = variable */
	uint16_t frame_us;
	uint16_t frame_samples;
};

/* Fills in the format a codec encodes with. Returns -ENOTSUP for codecs
 * this build lacks. */
int mg_codec_format(uint8_t type, struct mg_audio_format *fmt);

/* Encoder. One instance; not thread-safe. */
int mg_codec_open(uint8_t type);
/* Encodes fmt.frame_samples samples; returns bytes written or <0. */
int mg_codec_encode(const int16_t *pcm, uint8_t *out, size_t room);

/* Builds [change_data_type, type, rate, channels, frame_bytes, frame_us]. */
size_t mg_codec_change_data_type(const struct mg_audio_format *fmt, uint8_t *out);

#endif
