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

/*
 * The token proof through the protocol engine (mg_session -> mg_core) with
 * Zephyr's own PSA Crypto and settings (ZMS on the flash simulator), against
 * protocols/test-vectors/mg-token-proof-v1.json; and Muse Link setup's first
 * steps against esp32/tests/vectors/link_pairing_v5.json. tests/host covers
 * the rest of Link setup (every phase, rollback, timeouts) on any host.
 */

#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "fakes.h"
#include "mg_core.h"
#include "mg_session.h"
#include "mg_settings.h"
#include "mg_setup.h"
#include "mg_setup_crypto.h"
#include "mg_setup_store.h"
#include "mg_token_proof.h"
#include "vectors.h"

static void *setup(void)
{
	fake_init_once();
	return NULL;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	mg_settings_reset();
	(void)mg_setup_store_erase();
	fake_reset();
	fake_connect();
}

static void after(void *f)
{
	ARG_UNUSED(f);
	mg_session_disconnected();
	(void)mg_setup_store_erase();
}

static void rx(const struct proof_frame *fr)
{
	mg_session_rx(MG_ATT_CONTROL, fr->b, fr->n);
}

static const struct fake_note *last(void)
{
	return fake_nnotes ? &fake_notes[fake_nnotes - 1] : NULL;
}

static void assert_error(uint16_t code)
{
	const struct fake_note *n = last();

	zassert_not_null(n);
	zassert_equal(n->data[0], mg_command_error);
	zassert_equal(n->data[1], mg_command_token_proof);
	zassert_equal(sys_get_le16(&n->data[3]), code, "code %u", sys_get_le16(&n->data[3]));
}

/* On a secure build the proof needs an authenticated session: test_secure.c. */
#if !defined(CONFIG_MG_SECURE)
ZTEST(proof, test_vectors_frame_by_frame)
{
	for (int i = 0; i < PROOF_VEC_COUNT; i++) {
		const struct proof_vec *v = &proof_vecs[i];
		uint8_t k[32];

		zassert_ok(mg_token_proof_derive_key(v->access_token, strlen(v->access_token),
						     v->node_id, k));
		zassert_mem_equal(k, v->K, 32, "K, vector %s", v->name);
		zassert_ok(mg_setup_store_commit(v->access_token, strlen(v->access_token), "r", 1, k));
		zassert_true(mg_setup_store_complete());

		fake_reset();
		mg_token_proof_test_set_nonce(v->device_nonce);
		rx(&v->challenge);
		zassert_equal(fake_nnotes, 1);
		zassert_equal(last()->ch, MG_ATT_CONTROL);
		zassert_equal(last()->len, v->response.n);
		zassert_mem_equal(last()->data, v->response.b, v->response.n, "response, %s",
				  v->name);
		rx(&v->confirm);
		zassert_equal(last()->len, v->result.n);
		zassert_mem_equal(last()->data, v->result.b, v->result.n);
		/* clear: result 1, then setup is erased. */
		rx(&v->clear);
		zassert_equal(last()->len, 3);
		zassert_equal(last()->data[2], 1);
		zassert_false(mg_setup_store_complete());
		zassert_true(mg_setup_advertise_link());
		uint8_t got[32];

		zassert_equal(mg_setup_store_get_k(got), -ENOENT);
	}
}

ZTEST(proof, test_rules)
{
	const struct proof_vec *v = &proof_vecs[0];

	/* No token. */
	rx(&v->challenge);
	assert_error(mg_error_code_not_found);
	zassert_ok(mg_setup_store_commit(v->access_token, strlen(v->access_token), "r", 1, v->K));
	/* clear before a match. */
	rx(&v->clear);
	assert_error(mg_error_code_proof_required);
	zassert_true(mg_setup_store_complete());
	/* confirm with nothing pending. */
	rx(&v->confirm);
	assert_error(mg_error_code_invalid_value);
	/* A match lasts for the connection, not past it. */
	mg_token_proof_test_set_nonce(v->device_nonce);
	rx(&v->challenge);
	rx(&v->confirm);
	zassert_equal(last()->data[2], 1);
	mg_session_disconnected();
	fake_connect();
	rx(&v->clear);
	assert_error(mg_error_code_proof_required);
	/* Other commands are never gated on it. */
	fake_reset();
	const uint8_t status[] = {mg_command_request_status};

	mg_session_rx(MG_ATT_CONTROL, status, sizeof(status));
	zassert_not_null(fake_find(MG_ATT_CONTROL, mg_command_supported_features, 0));
}

#endif /* !CONFIG_MG_SECURE */

ZTEST(proof, test_setup_device_info_and_hello)
{
	const struct link_vec *lv = &link_vecs[0];
	char msg[600];
	size_t n = 0;

	mg_setup_connected();
	const char *q = "{\"action\":\"get_device_info\"}";

	mg_setup_rx((const uint8_t *)q, strlen(q));
	for (int i = 0; i < fake_nnotes; i++) {
		if (fake_notes[i].ch == MG_ATT_SETUP && fake_notes[i].data[0] == 0xFE) {
			memcpy(&msg[n], &fake_notes[i].data[3], fake_notes[i].len - 3);
			n += fake_notes[i].len - 3;
		}
	}
	msg[n] = '\0';
	zassert_not_null(strstr(msg, "\"type\":\"device_info\""), "%s", msg);
	zassert_not_null(strstr(msg, "\"node_id\":\"homelink-000001\""));
	zassert_not_null(strstr(msg, "\"wifi\":\"none\""));
	zassert_not_null(strstr(msg, "\"mgcommands\":1"));
	zassert_not_null(strstr(msg, "\"pairing_protocol\":5"));

	/* hello with the vector's keys: the vector's transcript hash and session id. */
	uint8_t dnonce[16];
	size_t dl;
	char hello[400];

	mg_b64url_decode(lv->device_nonce, strlen(lv->device_nonce), dnonce, 16, &dl);
	mg_setup_test_set_ephemeral(lv->device_priv, dnonce);
	snprintf(hello, sizeof(hello),
		 "{\"action\":\"pairing_client_hello\",\"version\":5,\"pairing_auth\":\"none\","
		 "\"pairing_policy\":\"confirm_press\",\"mobile_pub\":\"%s\",\"mobile_nonce\":\"%s\"}",
		 lv->mobile_pub, lv->mobile_nonce);
	fake_reset();
	size_t hl = strlen(hello);
	uint8_t pkt[160];
	int total = (int)((hl + 99) / 100);

	for (int i = 0; i < total; i++) {
		size_t c = MIN((size_t)100, hl - i * 100);

		pkt[0] = 0xFE;
		pkt[1] = i;
		pkt[2] = total;
		memcpy(&pkt[3], &hello[i * 100], c);
		mg_setup_rx(pkt, c + 3);
	}
	n = 0;
	for (int i = 0; i < fake_nnotes; i++) {
		if (fake_notes[i].ch == MG_ATT_SETUP) {
			memcpy(&msg[n], &fake_notes[i].data[3], fake_notes[i].len - 3);
			n += fake_notes[i].len - 3;
		}
	}
	msg[n] = '\0';
	char want[80];

	snprintf(want, sizeof(want), "\"transcript_hash\":\"%s\"", lv->transcript_hash);
	zassert_not_null(strstr(msg, want), "%s", msg);
	snprintf(want, sizeof(want), "\"session_id\":\"%s\"", lv->session_id);
	zassert_not_null(strstr(msg, want));
	mg_setup_disconnected();
}

ZTEST_SUITE(proof, NULL, setup, before, after, NULL);
