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

#ifndef MG_SESSION_H_
#define MG_SESSION_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mg_transport.h"

/*
 * Sits between the BLE characteristics and mg_core. Without session
 * security it passes Control through. With CONFIG_MG_SECURE it runs
 * mgcommands-secure.h: key exchange, frame encryption, authentication,
 * pairing, and the Rules about which command may arrive where.
 *
 * The rx and connection functions run on the app work queue; the send
 * functions may be called from any thread.
 */

void mg_session_init(bool boot_pairing_request);
void mg_session_connected(void);
void mg_session_disconnected(void);
void mg_session_subscribed(enum mg_att_chan ch, bool enabled);
void mg_session_rx(enum mg_att_chan ch, const uint8_t *buf, size_t len);

/* The device button went down. Returns true if it was taken to confirm a
 * pending pairing, so it isn't push-to-talk. */
bool mg_session_button(void);

/* Sends one Control notification (encrypted once encryption is on). */
int mg_session_send_control(const uint8_t *buf, size_t len);
/* Sends one Data chunk. Fails with -EACCES before authentication. */
int mg_session_send_data(const uint8_t *buf, size_t len, k_timeout_t wait);
/* Largest Data chunk (plaintext) that fits one notification. */
size_t mg_session_data_room(void);

/* Adds the secure commands to a supported_features list; returns count. */
size_t mg_session_feature_cmds(uint8_t *out);
/* Sends the typed feature lists mgcommands-secure.h defines. */
void mg_session_send_feature_lists(void);

bool mg_session_pairing_mode(void);
/* A client is connected and, with session security, authenticated. */
bool mg_session_authenticated(void);
/* A pair_request waits for the button. */
bool mg_session_pair_pending(void);

#if defined(CONFIG_MG_SECURE)
struct mg_x25519;
/* Test hook: the next key exchange uses this private key and nonce. */
void mg_session_test_set_ephemeral(const uint8_t priv[32], const uint8_t nonce[16]);
#endif

#endif
