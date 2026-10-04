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

/* Shared between mg_ble.c (the BLE side) and mg_voice.c (the voice task side). */

#include <stdbool.h>
#include <stdint.h>

#include "mg_proto.h"

/* mg_ble_init() succeeded; nothing else here may be used before. */
bool mg_active(void);

/* Serializes every use of the protocol state. Recursive. */
void mg_lock(void);
void mg_unlock(void);
mg_proto_t *mg_proto_get(void);

/* The offline queue, or NULL without the partition (or the option). */
mg_queue_t *mg_queue_get(void);
/* Hands a recorded clip to the worker to store; it frees `data`. */
bool mg_queue_submit(uint8_t codec, uint32_t start_ms, uint8_t *data, uint32_t bytes);

/* start_mic state from the client, under mg_lock. */
typedef struct {
    bool requested;   /* start_mic, not yet stopped */
    uint8_t codec;
} mg_capture_req_t;
mg_capture_req_t *mg_capture_req(void);

uint32_t mg_now_ms(void);

/* Wakes the playback task (a stream started, stopped or was claimed). */
void mg_play_kick(void);

/* The activation buzz on the board's vibration motor; false without one. */
bool mg_vibrate(uint16_t freq_hz, uint16_t ms, uint8_t volume_pct);
