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

#include "mg_crypto.h"

#include <errno.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>

#define TRANSCRIPT_LABEL "musegadgets ble v1"

int mg_crypto_init(void)
{
	return psa_crypto_init() == PSA_SUCCESS ? 0 : -EIO;
}

int mg_random(uint8_t *buf, size_t len)
{
	return psa_generate_random(buf, len) == PSA_SUCCESS ? 0 : -EIO;
}

bool mg_ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
	volatile uint8_t d = 0;

	for (size_t i = 0; i < len; i++) {
		d |= a[i] ^ b[i];
	}
	return d == 0;
}

static void x25519_attrs(psa_key_attributes_t *attr)
{
	*attr = psa_key_attributes_init();
	psa_set_key_type(attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
	psa_set_key_bits(attr, 255);
	psa_set_key_algorithm(attr, PSA_ALG_ECDH);
	psa_set_key_usage_flags(attr, PSA_KEY_USAGE_DERIVE);
	psa_set_key_lifetime(attr, PSA_KEY_LIFETIME_VOLATILE);
}

static int x25519_finish(struct mg_x25519 *k)
{
	size_t n;

	if (psa_export_public_key(k->key, k->pub, sizeof(k->pub), &n) != PSA_SUCCESS ||
	    n != sizeof(k->pub)) {
		mg_x25519_destroy(k);
		return -EIO;
	}
	return 0;
}

int mg_x25519_generate(struct mg_x25519 *k)
{
	psa_key_attributes_t attr;

	x25519_attrs(&attr);
	if (psa_generate_key(&attr, &k->key) != PSA_SUCCESS) {
		k->key = PSA_KEY_ID_NULL;
		return -EIO;
	}
	return x25519_finish(k);
}

int mg_x25519_import(struct mg_x25519 *k, const uint8_t priv[32])
{
	psa_key_attributes_t attr;

	x25519_attrs(&attr);
	if (psa_import_key(&attr, priv, 32, &k->key) != PSA_SUCCESS) {
		k->key = PSA_KEY_ID_NULL;
		return -EIO;
	}
	return x25519_finish(k);
}

int mg_x25519_shared(const struct mg_x25519 *k, const uint8_t peer[32], uint8_t ss[32])
{
	static const uint8_t zero[32];
	size_t n;

	if (psa_raw_key_agreement(PSA_ALG_ECDH, k->key, peer, 32, ss, 32, &n) != PSA_SUCCESS ||
	    n != 32) {
		return -EINVAL;
	}
	if (mg_ct_equal(ss, zero, 32)) {
		memset(ss, 0, 32);
		return -EINVAL;
	}
	return 0;
}

void mg_x25519_destroy(struct mg_x25519 *k)
{
	if (k->key != PSA_KEY_ID_NULL) {
		psa_destroy_key(k->key);
		k->key = PSA_KEY_ID_NULL;
	}
}

int mg_sha256(const uint8_t *const parts[], const size_t lens[], int n, uint8_t out[32])
{
	psa_hash_operation_t op = psa_hash_operation_init();
	size_t olen;

	if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
		return -EIO;
	}
	for (int i = 0; i < n; i++) {
		if (psa_hash_update(&op, parts[i], lens[i]) != PSA_SUCCESS) {
			psa_hash_abort(&op);
			return -EIO;
		}
	}
	return psa_hash_finish(&op, out, 32, &olen) == PSA_SUCCESS ? 0 : -EIO;
}

int mg_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *const parts[],
		   const size_t lens[], int n, uint8_t out[32])
{
	psa_key_attributes_t attr = psa_key_attributes_init();
	psa_mac_operation_t op = psa_mac_operation_init();
	psa_key_id_t id;
	size_t olen;
	int err = -EIO;

	psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
	psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
	if (psa_import_key(&attr, key, key_len, &id) != PSA_SUCCESS) {
		return -EIO;
	}
	if (psa_mac_sign_setup(&op, id, PSA_ALG_HMAC(PSA_ALG_SHA_256)) != PSA_SUCCESS) {
		goto out;
	}
	for (int i = 0; i < n; i++) {
		if (psa_mac_update(&op, parts[i], lens[i]) != PSA_SUCCESS) {
			psa_mac_abort(&op);
			goto out;
		}
	}
	if (psa_mac_sign_finish(&op, out, 32, &olen) == PSA_SUCCESS && olen == 32) {
		err = 0;
	}
out:
	psa_destroy_key(id);
	return err;
}

int mg_hkdf_expand(const uint8_t prk[32], const char *info, uint8_t *out, size_t len)
{
	static const uint8_t one = 1;
	uint8_t t[32];
	const uint8_t *parts[] = {(const uint8_t *)info, &one};
	const size_t lens[] = {strlen(info), 1};

	if (len > sizeof(t)) {
		return -EINVAL;
	}
	int err = mg_hmac_sha256(prk, 32, parts, lens, 2, t);

	if (!err) {
		memcpy(out, t, len);
	}
	memset(t, 0, sizeof(t));
	return err;
}

int mg_derive(const uint8_t *m1, size_t m1_len, const uint8_t *m2, size_t m2_len,
	      const uint8_t *m3, size_t m3_len, const uint8_t ss[32], struct mg_session_keys *k)
{
	const uint8_t *tp[] = {(const uint8_t *)TRANSCRIPT_LABEL, m1, m2, m3};
	const size_t tl[] = {strlen(TRANSCRIPT_LABEL), m1_len, m2_len, m3_len};
	const uint8_t *ep[] = {ss};
	const size_t el[] = {32};
	int err;

	err = mg_sha256(tp, tl, 4, k->th);
	/* HKDF-Extract(salt = th, IKM = ss) = HMAC(th, ss). */
	err = err ?: mg_hmac_sha256(k->th, sizeof(k->th), ep, el, 1, k->prk);
	err = err ?: mg_hkdf_expand(k->prk, "mg1 c2d control", k->c2d_control, 32);
	err = err ?: mg_hkdf_expand(k->prk, "mg1 c2d data", k->c2d_data, 32);
	err = err ?: mg_hkdf_expand(k->prk, "mg1 d2c control", k->d2c_control, 32);
	err = err ?: mg_hkdf_expand(k->prk, "mg1 d2c data", k->d2c_data, 32);
	err = err ?: mg_hkdf_expand(k->prk, "mg1 pairing code", k->pairing_code, 4);
	err = err ?: mg_hkdf_expand(k->prk, "mg1 pairing key", k->pairing_key, 32);
	err = err ?: mg_hkdf_expand(k->prk, "mg1 key id", k->key_id, MG_KEY_ID_SIZE);
	return err;
}

int mg_auth_mac(const uint8_t pk[32], const char *label, const uint8_t th[32], uint8_t mac[32])
{
	const uint8_t *parts[] = {(const uint8_t *)label, th};
	const size_t lens[] = {strlen(label), 32};

	return mg_hmac_sha256(pk, 32, parts, lens, 2, mac);
}

uint32_t mg_pairing_code_value(const uint8_t code[4])
{
	return sys_get_le32(code) % 1000000U;
}

int mg_aead_setup(struct mg_aead *a, const uint8_t key[32])
{
	psa_key_attributes_t attr = psa_key_attributes_init();

	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, 256);
	psa_set_key_algorithm(&attr, PSA_ALG_GCM);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
	if (psa_import_key(&attr, key, 32, &a->key) != PSA_SUCCESS) {
		a->key = PSA_KEY_ID_NULL;
		return -EIO;
	}
	return 0;
}

void mg_aead_destroy(struct mg_aead *a)
{
	if (a->key != PSA_KEY_ID_NULL) {
		psa_destroy_key(a->key);
		a->key = PSA_KEY_ID_NULL;
	}
}

static void nonce_for(uint32_t seq, uint8_t nonce[12])
{
	memset(nonce, 0, 12);
	sys_put_le32(seq, nonce);
}

int mg_frame_seal(const struct mg_aead *a, uint32_t seq, const uint8_t *pt, size_t len,
		  uint8_t *out)
{
	uint8_t nonce[12];
	size_t olen;

	out[0] = 0;
	sys_put_le32(seq, &out[1]);
	nonce_for(seq, nonce);
	if (psa_aead_encrypt(a->key, PSA_ALG_GCM, nonce, sizeof(nonce), out, MG_FRAME_HEADER_SIZE,
			     pt, len, &out[MG_FRAME_HEADER_SIZE], len + MG_FRAME_TAG_SIZE,
			     &olen) != PSA_SUCCESS ||
	    olen != len + MG_FRAME_TAG_SIZE) {
		return -EIO;
	}
	return (int)(len + MG_FRAME_OVERHEAD);
}

int mg_frame_open(const struct mg_aead *a, const uint8_t *frame, size_t len, uint32_t *seq,
		  uint8_t *pt)
{
	uint8_t nonce[12];
	size_t olen;

	if (len < MG_FRAME_OVERHEAD || frame[0] != 0) {
		return -EBADMSG;
	}
	*seq = sys_get_le32(&frame[1]);
	nonce_for(*seq, nonce);
	if (psa_aead_decrypt(a->key, PSA_ALG_GCM, nonce, sizeof(nonce), frame,
			     MG_FRAME_HEADER_SIZE, &frame[MG_FRAME_HEADER_SIZE],
			     len - MG_FRAME_HEADER_SIZE, pt, len - MG_FRAME_OVERHEAD,
			     &olen) != PSA_SUCCESS) {
		return -EBADMSG;
	}
	return (int)olen;
}
