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

#ifndef FAKES_H_
#define FAKES_H_

#include <stdbool.h>
#include <stdint.h>

#include "mg_audio.h"
#include "mg_transport.h"

struct fake_note {
	uint8_t ch;
	uint16_t len;
	uint8_t data[260];
};

#define FAKE_MAX_NOTES 64
extern struct fake_note fake_notes[FAKE_MAX_NOTES];
extern int fake_nnotes;
extern uint16_t fake_mtu;
extern int fake_disconnects;
extern int fake_buzzes;
extern uint32_t fake_led_flags;
extern int fake_led_link;
extern int fake_led_face;
extern int fake_led_faces; /* face changes */

/* Connection parameter requests (mg_transport_conn_params). */
struct fake_conn_req {
	uint16_t min, max, latency, timeout;
};
extern struct fake_conn_req fake_conn_reqs[16];
extern int fake_nconn_reqs;

/* Audio calls mg_core made. */
extern int fake_audio_starts;
extern uint8_t fake_audio_codec;
extern bool fake_audio_ptt;
extern int fake_audio_stops;
extern int fake_audio_recordings;
extern int fake_audio_clip_reads;
extern uint16_t fake_audio_clip;
extern uint8_t fake_audio_gain;
extern enum mg_audio_activity fake_audio_state;

/* Sleep this long in each Data notification (paces a sending thread). */
extern int fake_notify_delay_us;
/* Data notifications seen, and how many (counted even past FAKE_MAX_NOTES). */
extern int fake_data_notes;

void fake_reset(void);
/* nth (0-based) notification on ch whose first byte is cmd, or NULL. */
const struct fake_note *fake_find(uint8_t ch, uint8_t cmd, int nth);
int fake_count(uint8_t ch, uint8_t cmd);
/* Connects and subscribes the characteristics a session needs. */
void fake_connect(void);
/* settings, queue, core and session, once. */
void fake_init_once(void);

#endif
