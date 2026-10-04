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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mg_route.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The voice transport layer between a gadget's push-to-talk loop and where
 * the audio goes. Each utterance asks mg_voice_route() once, at the press:
 *
 *   MG_ROUTE_WIFI   the loop's existing path to Muse, untouched
 *   MG_ROUTE_BLE    mg_voice_begin/audio/end stream it to the mg client
 *   MG_ROUTE_QUEUE  mg_voice_begin/audio/end record it to the offline queue
 *   MG_ROUTE_NONE   refuse the press
 *
 * The policy is CONFIG_MUSE_GADGET_BLE_ROUTE_*. PCM is 16 kHz mono in any
 * chunk size. Call these from the voice task only (the one that reads the
 * mic); mg_voice_gesture from wherever presses are seen.
 */

/* wifi_ready: the existing path could start a turn now. */
mg_route_t mg_voice_route(bool wifi_ready);

/* Every press and release, whatever the route: an mg client that's ready
 * gets [gesture]. False if it didn't go. */
bool mg_voice_gesture(bool down);

/*
 * Opens an utterance on `route` (BLE or QUEUE). For push-to-talk
 * (from_client false) on BLE, plays the activation buzz on the board's
 * vibration motor first, if it has one and the client turned haptics on.
 * False if the route can't take it any more.
 */
bool mg_voice_begin(mg_route_t route, bool from_client);
/* False once the utterance should end (client gone, stop_mic, or the queue's full). */
bool mg_voice_audio(const int16_t *pcm, size_t frames);
/* Closes it: a live one always ends with [stop_mic]. keep: the utterance
 * counts (a queued clip is stored, a short tap's isn't). */
void mg_voice_end(bool keep);

/* The client asked for capture with start_mic and still wants it. */
bool mg_voice_capture_requested(void);

/*
 * The voice loop takes the speaker and mic (a push-to-talk turn, a chirp):
 * a client's playback ends with stop_streaming drop, and new playback is
 * refused until it's released.
 */
void mg_voice_claim(bool claim);
/* A client's audio is playing (or buffering) on the speaker. */
bool mg_voice_playing(void);

/* The connected client has sent assistant_state; older clients never do. */
bool mg_voice_client_reports_state(void);

/*
 * Logs a face (or LED ring) change of a musegadgets turn as
 * "mg.face: <what>", which the Nordic UART mirror carries to the client.
 */
void mg_voice_face(const char *what);

#ifdef __cplusplus
}
#endif
