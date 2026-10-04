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

#include "mg_token_proof.h"

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>

#include "mg_core.h"
#include "mg_psa.h"
#include "mg_session.h"
#include "mg_setup.h"
#include "mg_setup_store.h"

LOG_MODULE_REGISTER(mg_proof, CONFIG_MG_LOG_LEVEL);

#define NONCE MG_TOKEN_PROOF_NONCE_LEN
#define MAC   MG_TOKEN_PROOF_MAC_LEN

static struct {
	bool pending; /* a response went out; confirm may follow */
	bool matched; /* the last confirm matched */
	uint8_t client_nonce[NONCE];
	uint8_t device_nonce[NONCE];
} st;

static bool test_nonce_set;
static uint8_t test_nonce[NONCE];

void mg_token_proof_test_set_nonce(const uint8_t nonce[NONCE])
{
	memcpy(test_nonce, nonce, NONCE);
	test_nonce_set = true;
}

void mg_token_proof_connection_reset(void)
{
	mg_psa_wipe(&st, sizeof(st));
}

bool mg_token_proof_matched(void)
{
	return st.matched;
}

int mg_token_proof_derive_key(const char *access, size_t access_len, const char *node_id,
			      uint8_t k[32])
{
	return mg_psa_hkdf32((const uint8_t *)MG_TOKEN_PROOF_SALT, sizeof(MG_TOKEN_PROOF_SALT) - 1,
			     (const uint8_t *)access, access_len, (const uint8_t *)node_id,
			     strlen(node_id), k);
}

int mg_token_proof_mac(const uint8_t k[32], bool device, const uint8_t client_nonce[NONCE],
		       const uint8_t device_nonce[NONCE], uint8_t mac[MAC])
{
	const char *label = device ? MG_TOKEN_PROOF_DEVICE_LABEL : MG_TOKEN_PROOF_CLIENT_LABEL;
	const uint8_t *parts[] = {(const uint8_t *)label, client_nonce, device_nonce};
	const size_t lens[] = {strlen(label), NONCE, NONCE};

	return mg_psa_hmac_sha256(k, 32, parts, lens, 3, mac);
}

static void send_result(uint8_t matched)
{
	const uint8_t m[] = {mg_command_token_proof, mg_token_proof_result, matched};

	(void)mg_session_send_control(m, sizeof(m));
}

static void on_challenge(const uint8_t *p, size_t len)
{
	uint8_t k[32];
	uint8_t m[2 + NONCE + MAC];
	int err;

	if (len != NONCE) {
		mg_core_send_error(mg_command_token_proof, mg_error_code_invalid_length, NULL);
		return;
	}
	/* A new challenge restarts the exchange. */
	mg_token_proof_connection_reset();
	if (!mg_setup_store_complete()) {
		mg_core_send_error(mg_command_token_proof, mg_error_code_not_found, NULL);
		return;
	}
	memcpy(st.client_nonce, p, NONCE);
	if (test_nonce_set) {
		memcpy(st.device_nonce, test_nonce, NONCE);
		test_nonce_set = false;
		err = 0;
	} else {
		err = mg_psa_random(st.device_nonce, NONCE);
	}
	err = err ?: mg_setup_store_get_k(k);
	err = err ?: mg_token_proof_mac(k, true, st.client_nonce, st.device_nonce, &m[2 + NONCE]);
	mg_psa_wipe(k, sizeof(k));
	if (err) {
		mg_token_proof_connection_reset();
		mg_core_send_error(mg_command_token_proof, mg_error_code_busy, "proof failed");
		return;
	}
	m[0] = mg_command_token_proof;
	m[1] = mg_token_proof_response;
	memcpy(&m[2], st.device_nonce, NONCE);
	st.pending = true;
	(void)mg_session_send_control(m, sizeof(m));
}

static void on_confirm(const uint8_t *p, size_t len)
{
	uint8_t k[32], want[MAC];
	bool ok;

	if (!st.pending) {
		mg_core_send_error(mg_command_token_proof, mg_error_code_invalid_value, NULL);
		return;
	}
	if (len != MAC) {
		mg_core_send_error(mg_command_token_proof, mg_error_code_invalid_length, NULL);
		return;
	}
	st.pending = false;
	ok = mg_setup_store_get_k(k) == 0 &&
	     mg_token_proof_mac(k, false, st.client_nonce, st.device_nonce, want) == 0 &&
	     mg_psa_ct_equal(want, p, MAC);
	mg_psa_wipe(k, sizeof(k));
	mg_psa_wipe(want, sizeof(want));
	st.matched = ok;
	LOG_INF("token proof %s", ok ? "matched" : "did not match");
	send_result(ok ? 1 : 0);
}

static void on_clear(void)
{
	if (!st.matched) {
		mg_core_send_error(mg_command_token_proof, mg_error_code_proof_required, NULL);
		return;
	}
	/* Answer first: the client waits for it before forgetting its record. */
	send_result(1);
	mg_token_proof_connection_reset();
	if (mg_setup_factory_reset("token proof clear")) {
		mg_core_send_error(mg_command_token_proof, mg_error_code_busy, "erase failed");
	}
}

void mg_token_proof_rx(const uint8_t *p, size_t len)
{
	if (len < 1) {
		mg_core_send_error(mg_command_token_proof, mg_error_code_invalid_length, NULL);
		return;
	}
	switch (p[0]) {
	case mg_token_proof_challenge:
		on_challenge(&p[1], len - 1);
		break;
	case mg_token_proof_confirm:
		on_confirm(&p[1], len - 1);
		break;
	case mg_token_proof_clear:
		on_clear();
		break;
	default:
		mg_core_send_error(mg_command_token_proof, mg_error_code_unsupported, NULL);
		break;
	}
}
