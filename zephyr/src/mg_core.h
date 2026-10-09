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

#ifndef MG_CORE_H_
#define MG_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mgcommands.h"

/*
 * The protocol engine for mgcommands.h: command parsing, push-to-talk,
 * settings and offline clip commands. It runs on one thread (the app
 * thread in main.c, or the test thread) and talks to the rest through
 * small functions declared in the module headers, which the unit tests
 * replace with fakes.
 */

/* Commands arrive here only after mg_session has let them through. */
void mg_core_control_rx(const uint8_t *buf, size_t len);

/* A client connected, or its session became ready for gestures and audio
 * (Control and Data subscribed and, with session security, authenticated). */
void mg_core_connected(void);
void mg_core_disconnected(void);
void mg_core_set_ready(bool ready);
bool mg_core_is_ready(void);
bool mg_core_is_connected(void);
/* Recomputes the link state (LED, "mg.ble: link" log). */
void mg_core_link_refresh(void);

void mg_core_button(bool pressed);

/* The audio thread finished a stream (live, offline recording or clip). */
void mg_core_audio_idle(void);

/* Notify [error, cmd, code, log] on Control. log may be NULL. */
void mg_core_send_error(uint8_t cmd, uint16_t code, const char *log);

/* Composes the supported_features payloads; exposed for tests. */
void mg_core_send_status(void);

void mg_core_init(void);

#endif
