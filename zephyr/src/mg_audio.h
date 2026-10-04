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

#ifndef MG_AUDIO_H_
#define MG_AUDIO_H_

#include <stdbool.h>
#include <stdint.h>

#include "mg_codec.h"

enum mg_audio_activity {
	MG_AUDIO_IDLE,
	MG_AUDIO_LIVE,
	MG_AUDIO_RECORDING,
	MG_AUDIO_CLIP,
};

/*
 * The audio thread produces everything on Data: live capture, offline
 * recording into the queue, and clip downloads. It sends change_data_type
 * before a stream's first Data chunk and stop_mic after a live stream's
 * last, so Control and Data stay in order. Starting anything stops
 * what was running first. mg_core_audio_idle() runs when it goes idle.
 */

int mg_audio_start_live(uint8_t codec, bool ptt);
int mg_audio_start_recording(uint8_t codec);
int mg_audio_read_clip(uint16_t index);
/* Ends capture (flushing the last chunk) or abandons a clip download. */
void mg_audio_stop(void);

enum mg_audio_activity mg_audio_activity(void);
/* The type most recently announced with change_data_type. */
void mg_audio_current_format(struct mg_audio_format *fmt);
void mg_audio_set_gain(uint8_t gain);
void mg_audio_init(void);

/* Audio source (mg_audio_dmic.c or mg_audio_synth.c): 10 ms blocks of
 * 160 mono 16-bit samples at 16 kHz. */
#define MG_AUDIO_BLOCK_SAMPLES 160
int mg_audio_src_start(void);
/* Blocks up to timeout_ms for the next block; returns 0 or <0. */
int mg_audio_src_read(int16_t *pcm, int timeout_ms);
void mg_audio_src_stop(void);

#endif
