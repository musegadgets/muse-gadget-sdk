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

#ifndef MG_TOKEN_PROOF_H_
#define MG_TOKEN_PROOF_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mgcommands.h"

/*
 * mg_command_token_proof (mgcommands.h, Token proof): the client checks that
 * the gadget holds the proof key K this phone set up, without either side
 * sending the token. State is per connection; nothing else is gated on it.
 * Runs on the app thread, behind mg_session.
 */

#define MG_TOKEN_PROOF_SALT         "mg token proof v1"
#define MG_TOKEN_PROOF_DEVICE_LABEL "mg token proof v1 device"
#define MG_TOKEN_PROOF_CLIENT_LABEL "mg token proof v1 client"

/* A token_proof command (the bytes after the command byte). */
void mg_token_proof_rx(const uint8_t *p, size_t len);

/* Forgets nonces and the result: at every connection and disconnection. */
void mg_token_proof_connection_reset(void);

/* A matching proof ran on this connection. */
bool mg_token_proof_matched(void);

/* K = HKDF-SHA256(salt MG_TOKEN_PROOF_SALT, IKM access token, info node_id). */
int mg_token_proof_derive_key(const char *access, size_t access_len, const char *node_id,
			      uint8_t k[32]);
/* HMAC-SHA256(K, label || client_nonce || device_nonce), label the device's
 * or the client's. */
int mg_token_proof_mac(const uint8_t k[32], bool device,
		       const uint8_t client_nonce[MG_TOKEN_PROOF_NONCE_LEN],
		       const uint8_t device_nonce[MG_TOKEN_PROOF_NONCE_LEN],
		       uint8_t mac[MG_TOKEN_PROOF_MAC_LEN]);

/* Test hook: the next challenge's device nonce. */
void mg_token_proof_test_set_nonce(const uint8_t nonce[MG_TOKEN_PROOF_NONCE_LEN]);

#endif
