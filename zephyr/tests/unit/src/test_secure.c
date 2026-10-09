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

#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "fakes.h"
#include "mg_core.h"
#include "mg_crypto.h"
#include "mg_pairing.h"
#include "mg_session.h"
#include "mg_settings.h"
#include "mg_setup_store.h"
#include "mg_token_proof.h"
#include "mg_vectors.h"
#include "vectors.h"
#include "mgcommands-secure.h"

/*
 * Session security against protocols/test-vectors/mgcommands-secure-v1.json:
 * first the primitives, then a whole session driven through mg_session with
 * the vectors' device key and nonce, byte for byte. Then the Rules.
 */

static const uint8_t *frame_key(const char *name, struct mg_session_keys *k)
{
	if (strcmp(name, "c2d_control") == 0) {
		return k->c2d_control;
	}
	if (strcmp(name, "d2c_control") == 0) {
		return k->d2c_control;
	}
	if (strcmp(name, "c2d_data") == 0) {
		return k->c2d_data;
	}
	return k->d2c_data;
}

static void *setup(void)
{
	fake_init_once();
	return NULL;
}

/* ---- primitives ---- */

ZTEST(secure_vectors, test_x25519)
{
	struct mg_x25519 dev, cli;
	uint8_t ss[32];

	zassert_equal(mg_x25519_import(&dev, vec_device_private), 0);
	zassert_mem_equal(dev.pub, vec_device_public, 32);
	zassert_equal(mg_x25519_import(&cli, vec_client_private), 0);
	zassert_mem_equal(cli.pub, vec_client_public, 32);
	zassert_equal(mg_x25519_shared(&dev, vec_client_public, ss), 0);
	zassert_mem_equal(ss, vec_shared_secret, 32);
	zassert_equal(mg_x25519_shared(&cli, vec_device_public, ss), 0);
	zassert_mem_equal(ss, vec_shared_secret, 32);

	/* A low-order point gives an all-zero secret: rejected. */
	static const uint8_t zero_point[32];

	zassert_not_equal(mg_x25519_shared(&dev, zero_point, ss), 0);
	mg_x25519_destroy(&dev);
	mg_x25519_destroy(&cli);
}

ZTEST(secure_vectors, test_commitment)
{
	uint8_t c[32];
	const uint8_t *parts[] = {vec_client_public, vec_client_nonce};
	const size_t lens[] = {32, 16};

	zassert_equal(mg_sha256(parts, lens, 2, c), 0);
	zassert_mem_equal(c, vec_commitment, 32);
	zassert_mem_equal(&vec_m1[4], vec_commitment, 32);
}

ZTEST(secure_vectors, test_derive)
{
	struct mg_session_keys k;

	zassert_equal(mg_derive(vec_m1, sizeof(vec_m1), vec_m2, sizeof(vec_m2), vec_m3,
				sizeof(vec_m3), vec_shared_secret, &k),
		      0);
	zassert_mem_equal(k.th, vec_transcript_hash, 32);
	zassert_mem_equal(k.prk, vec_prk, 32);
	zassert_mem_equal(k.c2d_control, vec_c2d_control_key, 32);
	zassert_mem_equal(k.c2d_data, vec_c2d_data_key, 32);
	zassert_mem_equal(k.d2c_control, vec_d2c_control_key, 32);
	zassert_mem_equal(k.d2c_data, vec_d2c_data_key, 32);
	zassert_mem_equal(k.pairing_code, vec_pairing_code_bytes, 4);
	zassert_equal(mg_pairing_code_value(k.pairing_code), VEC_PAIRING_CODE);
	zassert_mem_equal(k.pairing_key, vec_pairing_key, 32);
	zassert_mem_equal(k.key_id, vec_key_id, 8);

	uint8_t mac[32];

	zassert_equal(mg_auth_mac(k.pairing_key, "mg1 client auth", k.th, mac), 0);
	zassert_mem_equal(mac, vec_client_mac, 32);
	zassert_equal(mg_auth_mac(k.pairing_key, "mg1 device auth", k.th, mac), 0);
	zassert_mem_equal(mac, vec_device_mac, 32);
}

ZTEST(secure_vectors, test_frames)
{
	struct mg_session_keys k;
	uint8_t out[128], pt[128];

	memcpy(k.c2d_control, vec_c2d_control_key, 32);
	memcpy(k.c2d_data, vec_c2d_data_key, 32);
	memcpy(k.d2c_control, vec_d2c_control_key, 32);
	memcpy(k.d2c_data, vec_d2c_data_key, 32);
	for (size_t i = 0; i < ARRAY_SIZE(vec_frames); i++) {
		const struct vec_frame *f = &vec_frames[i];
		struct mg_aead a;
		uint32_t seq;

		zassert_equal(mg_aead_setup(&a, frame_key(f->key, &k)), 0);
		zassert_equal(mg_frame_seal(&a, f->seq, f->pt, f->pt_len, out),
			      f->pt_len + MG_FRAME_OVERHEAD);
		zassert_mem_equal(out, f->frame, f->frame_len, "frame %u", (unsigned)i);
		zassert_equal(mg_frame_open(&a, f->frame, f->frame_len, &seq, pt), f->pt_len);
		zassert_equal(seq, f->seq);
		zassert_mem_equal(pt, f->pt, f->pt_len);

		/* Any flipped bit, and non-zero flags, fail. */
		memcpy(out, f->frame, f->frame_len);
		out[f->frame_len - 1] ^= 1;
		zassert_true(mg_frame_open(&a, out, f->frame_len, &seq, pt) < 0);
		memcpy(out, f->frame, f->frame_len);
		out[0] = 1;
		zassert_true(mg_frame_open(&a, out, f->frame_len, &seq, pt) < 0);
		memcpy(out, f->frame, f->frame_len);
		out[1] ^= 1; /* seq is in the AAD and the nonce */
		zassert_true(mg_frame_open(&a, out, f->frame_len, &seq, pt) < 0);
		mg_aead_destroy(&a);
	}
}

ZTEST_SUITE(secure_vectors, NULL, setup, NULL, NULL, NULL);

/* ---- whole sessions through mg_session ---- */

static struct mg_aead c2d_ctl, c2d_dat, d2c_ctl, d2c_dat;
static uint32_t cli_seq;

static const struct fake_note *last(void)
{
	return fake_nnotes ? &fake_notes[fake_nnotes - 1] : NULL;
}

/* Client side: encrypt and write a command on Encrypted Control. */
static void send_enc(const uint8_t *pt, size_t len)
{
	uint8_t f[128];
	int n = mg_frame_seal(&c2d_ctl, cli_seq++, pt, len, f);

	zassert_true(n > 0);
	mg_session_rx(MG_ATT_ENC_CONTROL, f, n);
}

#define SEND_ENC(...)                                                                              \
	do {                                                                                       \
		const uint8_t _b[] = {__VA_ARGS__};                                                \
		send_enc(_b, sizeof(_b));                                                          \
	} while (0)

/* Client side: decrypt the nth notification on an encrypted characteristic. */
static int open_note(const struct fake_note *n, struct mg_aead *a, uint8_t *pt)
{
	uint32_t seq;

	zassert_not_null(n);
	return mg_frame_open(a, n->data, n->len, &seq, pt);
}

static const uint8_t *find_enc(uint8_t cmd, int nth, uint8_t *pt, int *len)
{
	for (int i = 0; i < fake_nnotes; i++) {
		if (fake_notes[i].ch != MG_ATT_ENC_CONTROL) {
			continue;
		}
		int n = open_note(&fake_notes[i], &d2c_ctl, pt);

		if (n > 0 && pt[0] == cmd && nth-- == 0) {
			*len = n;
			return pt;
		}
	}
	return NULL;
}

static void keys_from_vectors(void)
{
	mg_aead_destroy(&c2d_ctl);
	mg_aead_destroy(&c2d_dat);
	mg_aead_destroy(&d2c_ctl);
	mg_aead_destroy(&d2c_dat);
	zassert_equal(mg_aead_setup(&c2d_ctl, vec_c2d_control_key), 0);
	zassert_equal(mg_aead_setup(&c2d_dat, vec_c2d_data_key), 0);
	zassert_equal(mg_aead_setup(&d2c_ctl, vec_d2c_control_key), 0);
	zassert_equal(mg_aead_setup(&d2c_dat, vec_d2c_data_key), 0);
	cli_seq = 0;
}

/* Runs Step 1 with the vectors (device key and nonce from the JSON). */
static void key_exchange(void)
{
	mg_session_test_set_ephemeral(vec_device_private, vec_device_nonce);
	mg_session_rx(MG_ATT_CONTROL, vec_m1, sizeof(vec_m1));
	const struct fake_note *n = last();

	zassert_not_null(n);
	zassert_equal(n->ch, MG_ATT_CONTROL);
	zassert_equal(n->len, sizeof(vec_m2));
	zassert_mem_equal(n->data, vec_m2, sizeof(vec_m2), "M2 matches the vectors");
	int before = fake_nnotes;

	mg_session_rx(MG_ATT_CONTROL, vec_m3, sizeof(vec_m3));
	zassert_equal(fake_nnotes, before, "no reply to M3");
	keys_from_vectors();
}

static void enable_encryption(void)
{
	mg_session_rx(MG_ATT_ENC_CONTROL, vec_frames[0].frame, vec_frames[0].frame_len);
	cli_seq = 1;
	const struct fake_note *n = last();

	zassert_equal(n->ch, MG_ATT_ENC_CONTROL);
	zassert_equal(n->len, vec_frames[1].frame_len);
	zassert_mem_equal(n->data, vec_frames[1].frame, vec_frames[1].frame_len,
			  "enable_encryption reply matches the vectors");
}

static void before(void *f)
{
	ARG_UNUSED(f);
	mg_settings_reset();
	mg_pairing_clear();
	fake_reset();
	fake_connect();
}

static void after(void *f)
{
	ARG_UNUSED(f);
	mg_core_button(false);
	mg_session_disconnected();
}

ZTEST(secure_session, test_vectors_end_to_end)
{
	/* The client paired earlier: (key_id, PK) from the vectors are stored. */
	zassert_equal(mg_pairing_add(vec_key_id, vec_pairing_key), 0);

	/* request_status on plaintext Control lists key_exchange and the suites. */
	const uint8_t rs[] = {mg_command_request_status};

	mg_session_rx(MG_ATT_CONTROL, rs, 1);
	const struct fake_note *feat = fake_find(MG_ATT_CONTROL, mg_command_supported_features, 0);

	zassert_not_null(feat);
	zassert_not_null(memchr(feat->data, mg_command_key_exchange, feat->len));
	const uint8_t suites[] = {mg_command_supported_features, mg_command_sub_feature,
				  mg_command_key_exchange, mg_crypto_suite_x25519_aes256gcm_sha256};
	const uint8_t methods[] = {mg_command_supported_features, mg_command_sub_feature,
				   mg_command_authenticate, mg_pairing_method_physical_confirm};
	zassert_mem_equal(fake_find(MG_ATT_CONTROL, mg_command_supported_features, 2)->data,
			  suites, sizeof(suites));
	zassert_mem_equal(fake_find(MG_ATT_CONTROL, mg_command_supported_features, 3)->data,
			  methods, sizeof(methods));

	key_exchange();
	enable_encryption();
	zassert_false(mg_core_is_ready());

	/* Step 3, returning client: prove (vector frame), proof and
	 * connection_secured (vector frames), byte for byte. */
	mg_session_rx(MG_ATT_ENC_CONTROL, vec_frames[2].frame, vec_frames[2].frame_len);
	zassert_mem_equal(fake_notes[fake_nnotes - 2].data, vec_frames[3].frame,
			  vec_frames[3].frame_len, "proof matches the vectors");
	zassert_mem_equal(fake_notes[fake_nnotes - 1].data, vec_frames[4].frame,
			  vec_frames[4].frame_len, "connection_secured matches the vectors");
	zassert_true(mg_core_is_ready());

	/* Data: two audio chunks match the d2c_data vectors. */
	fake_reset();
	zassert_equal(mg_session_send_data(vec_frames[5].pt, vec_frames[5].pt_len, K_NO_WAIT), 0);
	zassert_equal(mg_session_send_data(vec_frames[6].pt, vec_frames[6].pt_len, K_NO_WAIT), 0);
	zassert_equal(fake_notes[0].ch, MG_ATT_ENC_DATA);
	zassert_mem_equal(fake_notes[0].data, vec_frames[5].frame, vec_frames[5].frame_len);
	zassert_mem_equal(fake_notes[1].data, vec_frames[6].frame, vec_frames[6].frame_len);

	/* Plaintext Control is ignored once encrypted; commands work encrypted. */
	fake_reset();
	mg_session_rx(MG_ATT_CONTROL, rs, 1);
	zassert_equal(fake_nnotes, 0);
	cli_seq = 2;
	SEND_ENC(mg_command_get_settings, mg_setting_parameter_spec_version);
	uint8_t pt[128];
	int len;

	zassert_not_null(find_enc(mg_command_get_settings, 0, pt, &len));
	zassert_equal(len, 3);
	zassert_equal(pt[2], MG_SPEC_VERSION);
}

ZTEST(secure_session, test_rules_before_encryption)
{
	uint8_t pt[64];
	int len;
	const uint8_t start[] = {mg_command_start_mic};

	mg_session_rx(MG_ATT_CONTROL, start, 1);
	const struct fake_note *n = fake_find(MG_ATT_CONTROL, mg_command_error, 0);

	zassert_not_null(n);
	zassert_equal(n->data[1], mg_command_start_mic);
	zassert_equal(sys_get_le16(&n->data[3]), mg_error_code_encryption_required);
	zassert_equal(fake_audio_starts, 0);

	/* No gestures before authentication. */
	fake_reset();
	mg_core_button(true);
	mg_core_button(false);
	zassert_equal(fake_count(MG_ATT_CONTROL, mg_command_gesture), 0);
	zassert_equal(mg_session_send_data(pt, 4, K_NO_WAIT), -EACCES);

	/* Before authentication, Encrypted Control takes only enable_encryption,
	 * authenticate and request_status. */
	key_exchange();
	enable_encryption();
	fake_reset();
	SEND_ENC(mg_command_start_mic);
	zassert_not_null(find_enc(mg_command_error, 0, pt, &len));
	zassert_equal(pt[1], mg_command_start_mic);
	zassert_equal(sys_get_le16(&pt[3]), mg_error_code_authentication_required);
	fake_reset();
	SEND_ENC(mg_command_request_status);
	zassert_not_null(find_enc(mg_command_supported_features, 0, pt, &len));
}

/* The token proof (mgcommands.h) on a secure device: refused in plaintext and
 * before authentication, then run on Encrypted Control, never in the clear. */
ZTEST(secure_session, test_token_proof_after_auth)
{
	const struct proof_vec *v = &proof_vecs[0];
	uint8_t pt[260];
	int len;

	zassert_ok(mg_setup_store_commit(v->access_token, strlen(v->access_token), "r", 1, v->K));

	/* Plaintext: refused. */
	mg_session_rx(MG_ATT_CONTROL, v->challenge.b, v->challenge.n);
	zassert_equal(fake_count(MG_ATT_CONTROL, mg_command_error), 1);
	zassert_equal(sys_get_le16(&fake_find(MG_ATT_CONTROL, mg_command_error, 0)->data[3]),
		      mg_error_code_encryption_required);

	/* Encrypted but not authenticated: refused too. */
	zassert_equal(mg_pairing_add(vec_key_id, vec_pairing_key), 0);
	key_exchange();
	enable_encryption();
	fake_reset();
	send_enc(v->challenge.b, v->challenge.n);
	zassert_not_null(find_enc(mg_command_error, 0, pt, &len));
	zassert_equal(pt[1], mg_command_token_proof);
	zassert_equal(sys_get_le16(&pt[3]), mg_error_code_authentication_required);

	/* Authenticated (the vectors' prove, client frame 1, so a new connection):
	 * the vector's exchange, frame for frame, on Encrypted Control. */
	mg_session_disconnected();
	fake_reset();
	fake_connect();
	key_exchange();
	enable_encryption();
	mg_session_rx(MG_ATT_ENC_CONTROL, vec_frames[2].frame, vec_frames[2].frame_len);
	zassert_true(mg_core_is_ready());
	cli_seq = 2;
	fake_reset();
	mg_token_proof_test_set_nonce(v->device_nonce);
	send_enc(v->challenge.b, v->challenge.n);
	zassert_not_null(find_enc(mg_command_token_proof, 0, pt, &len));
	zassert_equal(len, v->response.n);
	zassert_mem_equal(pt, v->response.b, v->response.n);
	send_enc(v->confirm.b, v->confirm.n);
	zassert_not_null(find_enc(mg_command_token_proof, 1, pt, &len));
	zassert_mem_equal(pt, v->result.b, v->result.n);
	send_enc(v->clear.b, v->clear.n);
	zassert_not_null(find_enc(mg_command_token_proof, 2, pt, &len));
	zassert_equal(pt[2], 1);
	zassert_false(mg_setup_store_complete());
	zassert_equal(fake_count(MG_ATT_CONTROL, mg_command_token_proof), 0, "nothing in the clear");
}

ZTEST(secure_session, test_key_exchange_errors)
{
	uint8_t m1[sizeof(vec_m1)];
	const struct fake_note *n;

	memcpy(m1, vec_m1, sizeof(m1));
	m1[2] = 2;
	mg_session_rx(MG_ATT_CONTROL, m1, sizeof(m1));
	n = fake_find(MG_ATT_CONTROL, mg_command_error, 0);
	zassert_equal(sys_get_le16(&n->data[3]), mg_error_code_unsupported_version);
	fake_reset();
	m1[2] = 1;
	m1[3] = 9;
	mg_session_rx(MG_ATT_CONTROL, m1, sizeof(m1));
	n = fake_find(MG_ATT_CONTROL, mg_command_error, 0);
	zassert_equal(sys_get_le16(&n->data[3]), mg_error_code_unsupported_suite);

	/* M3 that doesn't match the commitment. */
	fake_reset();
	mg_session_rx(MG_ATT_CONTROL, vec_m1, sizeof(vec_m1));
	uint8_t m3[sizeof(vec_m3)];

	memcpy(m3, vec_m3, sizeof(m3));
	m3[sizeof(m3) - 1] ^= 1;
	mg_session_rx(MG_ATT_CONTROL, m3, sizeof(m3));
	n = fake_find(MG_ATT_CONTROL, mg_command_error, 0);
	zassert_not_null(n);
	zassert_equal(n->data[1], mg_command_key_exchange);
	zassert_equal(sys_get_le16(&n->data[3]), mg_error_code_key_exchange_failed);

	/* Once per connection: a second M1 fails. */
	fake_reset();
	mg_session_rx(MG_ATT_CONTROL, vec_m1, sizeof(vec_m1));
	n = fake_find(MG_ATT_CONTROL, mg_command_error, 0);
	zassert_equal(sys_get_le16(&n->data[3]), mg_error_code_key_exchange_failed);

	/* M3 without M1 on a fresh connection. */
	mg_session_disconnected();
	fake_reset();
	fake_connect();
	mg_session_rx(MG_ATT_CONTROL, vec_m3, sizeof(vec_m3));
	n = fake_find(MG_ATT_CONTROL, mg_command_error, 0);
	zassert_equal(sys_get_le16(&n->data[3]), mg_error_code_key_exchange_failed);
}

ZTEST(secure_session, test_replay_drops_session)
{
	key_exchange();
	enable_encryption();
	fake_reset();
	/* The same seq 0 frame again. */
	mg_session_rx(MG_ATT_ENC_CONTROL, vec_frames[0].frame, vec_frames[0].frame_len);
	const struct fake_note *n = fake_find(MG_ATT_CONTROL, mg_command_error, 0);
	const uint8_t want[] = {mg_command_error, mg_command_enable_encryption, mg_error_data_code,
				0x05, 0x01};

	zassert_not_null(n, "decrypt_failed on plaintext Control");
	zassert_mem_equal(n->data, want, sizeof(want));
	zassert_equal(fake_disconnects, 1);
}

ZTEST(secure_session, test_bad_tag_drops_session)
{
	uint8_t f[64];

	key_exchange();
	memcpy(f, vec_frames[0].frame, vec_frames[0].frame_len);
	f[vec_frames[0].frame_len - 1] ^= 0x80;
	mg_session_rx(MG_ATT_ENC_CONTROL, f, vec_frames[0].frame_len);
	zassert_equal(fake_disconnects, 1);
}

ZTEST(secure_session, test_auth_failures_disconnect)
{
	uint8_t pt[64];
	int len;
	uint8_t prove[2 + 8 + 32];

	zassert_equal(mg_pairing_add(vec_key_id, vec_pairing_key), 0);
	key_exchange();
	enable_encryption();
	prove[0] = mg_command_authenticate;
	prove[1] = mg_authenticate_command_prove;
	memcpy(&prove[2], vec_key_id, 8);
	memset(&prove[10], 0x55, 32); /* wrong MAC */
	for (int i = 0; i < MG_AUTH_MAX_FAILURES; i++) {
		fake_reset();
		send_enc(prove, sizeof(prove));
		zassert_not_null(find_enc(mg_command_error, 0, pt, &len));
		zassert_equal(sys_get_le16(&pt[3]), mg_error_code_auth_failed);
		zassert_equal(fake_disconnects, i == MG_AUTH_MAX_FAILURES - 1 ? 1 : 0);
	}
	zassert_false(mg_core_is_ready());
}

ZTEST(secure_session, test_physical_confirm_pairing)
{
	uint8_t pt[64];
	int len;

	/* No stored pairings: pairing mode is on. */
	mg_session_disconnected();
	mg_session_init(false);
	zassert_true(mg_session_pairing_mode());
	fake_reset();
	fake_connect();
	key_exchange();
	enable_encryption();

	/* Numeric comparison needs a display: not offered. */
	fake_reset();
	SEND_ENC(mg_command_authenticate, mg_authenticate_command_pair_request,
		 mg_pairing_method_numeric_comparison);
	zassert_not_null(find_enc(mg_command_error, 0, pt, &len));
	zassert_equal(sys_get_le16(&pt[3]), mg_error_code_pairing_not_allowed);

	fake_reset();
	SEND_ENC(mg_command_authenticate, mg_authenticate_command_pair_request,
		 mg_pairing_method_physical_confirm);
	zassert_not_null(find_enc(mg_command_authenticate, 0, pt, &len));
	zassert_equal(pt[1], mg_authenticate_command_pair_pending);
	zassert_equal(pt[2], mg_pairing_method_physical_confirm);

	/* The button confirms on the device (and is not a gesture). */
	mg_core_button(true);
	mg_core_button(false);
	zassert_equal(fake_audio_starts, 0);
	SEND_ENC(mg_command_authenticate, mg_authenticate_command_pair_confirm);
	zassert_not_null(find_enc(mg_command_authenticate, 1, pt, &len));
	zassert_equal(pt[1], mg_authenticate_command_pair_complete);
	zassert_mem_equal(&pt[2], vec_key_id, 8);
	zassert_not_null(find_enc(mg_command_connection_secured, 0, pt, &len));
	zassert_true(mg_core_is_ready());
	zassert_false(mg_session_pairing_mode(), "pairing mode ends with a pairing");

	uint8_t pk[32];

	zassert_equal(mg_pairing_find(vec_key_id, pk), 0);
	zassert_mem_equal(pk, vec_pairing_key, 32, "PK stored");

	/* Now gestures flow, encrypted. */
	fake_reset();
	mg_core_button(true);
	zassert_not_null(find_enc(mg_command_gesture, 0, pt, &len));
	mg_core_button(false);

	/* Unpair deletes it and echoes. */
	fake_reset();
	SEND_ENC(mg_command_authenticate, mg_authenticate_command_unpair);
	zassert_not_null(find_enc(mg_command_authenticate, 0, pt, &len));
	zassert_equal(pt[1], mg_authenticate_command_unpair);
	zassert_equal(mg_pairing_find(vec_key_id, pk), -ENOENT);
	zassert_true(mg_session_pairing_mode(), "no pairings left");
}

ZTEST(secure_session, test_pairing_not_allowed_outside_pairing_mode)
{
	uint8_t pt[64];
	int len;

	zassert_equal(mg_pairing_add(vec_key_id, vec_pairing_key), 0);
	mg_session_disconnected();
	mg_session_init(false);
	zassert_false(mg_session_pairing_mode());
	fake_reset();
	fake_connect();
	key_exchange();
	enable_encryption();
	fake_reset();
	SEND_ENC(mg_command_authenticate, mg_authenticate_command_pair_request,
		 mg_pairing_method_physical_confirm);
	zassert_not_null(find_enc(mg_command_error, 0, pt, &len));
	zassert_equal(sys_get_le16(&pt[3]), mg_error_code_pairing_not_allowed);

	/* Holding the button at boot enters pairing mode. */
	mg_session_disconnected();
	mg_session_init(true);
	zassert_true(mg_session_pairing_mode());
}

ZTEST(secure_session, test_pairing_attempts_bounded)
{
	mg_session_disconnected();
	mg_session_init(true);
	for (int i = 0; i < MG_PAIRING_MAX_ATTEMPTS; i++) {
		zassert_true(mg_session_pairing_mode(), "attempt %d", i);
		fake_reset();
		fake_connect();
		key_exchange();
		mg_session_disconnected();
	}
	zassert_false(mg_session_pairing_mode(), "left after %d attempts",
		      MG_PAIRING_MAX_ATTEMPTS);
	fake_connect();
}

ZTEST(secure_session, test_pairing_times_out)
{
	uint8_t pt[64];
	int len;

	mg_session_disconnected();
	mg_session_init(true);
	fake_reset();
	fake_connect();
	key_exchange();
	enable_encryption();
	SEND_ENC(mg_command_authenticate, mg_authenticate_command_pair_request,
		 mg_pairing_method_physical_confirm);
	fake_reset();
	k_sleep(K_SECONDS(MG_PAIRING_TIMEOUT_S + 1));
	zassert_not_null(find_enc(mg_command_error, 0, pt, &len));
	zassert_equal(pt[1], mg_command_authenticate);
	zassert_equal(sys_get_le16(&pt[3]), mg_error_code_pairing_rejected);
	/* Too late now. */
	fake_reset();
	SEND_ENC(mg_command_authenticate, mg_authenticate_command_pair_confirm);
	zassert_not_null(find_enc(mg_command_error, 0, pt, &len));
	zassert_equal(sys_get_le16(&pt[3]), mg_error_code_pairing_rejected);
	zassert_false(mg_core_is_ready());
}

ZTEST(secure_session, test_random_keys_each_session)
{
	/* Without the test hook the device key and nonce are fresh. */
	const struct fake_note *n;
	uint8_t m2a[sizeof(vec_m2)];

	mg_session_rx(MG_ATT_CONTROL, vec_m1, sizeof(vec_m1));
	n = last();
	zassert_equal(n->len, sizeof(vec_m2));
	memcpy(m2a, n->data, sizeof(m2a));
	mg_session_disconnected();
	fake_reset();
	fake_connect();
	mg_session_rx(MG_ATT_CONTROL, vec_m1, sizeof(vec_m1));
	n = last();
	zassert_false(memcmp(&m2a[4], &n->data[4], 48) == 0, "fresh key and nonce");
}

ZTEST(secure_session, test_trailing_bytes_in_transcript)
{
	/* M1 and M3 with trailing bytes: accepted, and hashed as sent. */
	uint8_t m1[sizeof(vec_m1) + 3], m3[sizeof(vec_m3) + 2];
	struct mg_session_keys k;
	struct mg_x25519 cli;
	uint8_t ss[32], pt[64];
	int len;

	memcpy(m1, vec_m1, sizeof(vec_m1));
	memcpy(&m1[sizeof(vec_m1)], "\x01\x02\x03", 3);
	memcpy(m3, vec_m3, sizeof(vec_m3));
	memcpy(&m3[sizeof(vec_m3)], "\xaa\xbb", 2);

	mg_session_test_set_ephemeral(vec_device_private, vec_device_nonce);
	mg_session_rx(MG_ATT_CONTROL, m1, sizeof(m1));
	zassert_mem_equal(last()->data, vec_m2, sizeof(vec_m2));
	mg_session_rx(MG_ATT_CONTROL, m3, sizeof(m3));
	zassert_equal(fake_count(MG_ATT_CONTROL, mg_command_error), 0);

	zassert_equal(mg_x25519_import(&cli, vec_client_private), 0);
	zassert_equal(mg_x25519_shared(&cli, vec_device_public, ss), 0);
	zassert_equal(mg_derive(m1, sizeof(m1), vec_m2, sizeof(vec_m2), m3, sizeof(m3), ss, &k), 0);
	zassert_false(memcmp(k.th, vec_transcript_hash, 32) == 0, "different transcript");
	mg_x25519_destroy(&cli);

	mg_aead_destroy(&c2d_ctl);
	mg_aead_destroy(&d2c_ctl);
	zassert_equal(mg_aead_setup(&c2d_ctl, k.c2d_control), 0);
	zassert_equal(mg_aead_setup(&d2c_ctl, k.d2c_control), 0);
	cli_seq = 0;
	fake_reset();
	SEND_ENC(mg_command_enable_encryption);
	zassert_not_null(find_enc(mg_command_enable_encryption, 0, pt, &len),
			 "device derived the same keys from M1..M3 as sent");
}

ZTEST_SUITE(secure_session, NULL, setup, before, after, NULL);
