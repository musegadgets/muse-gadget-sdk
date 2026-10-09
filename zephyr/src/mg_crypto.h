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

#ifndef MG_CRYPTO_H_
#define MG_CRYPTO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <psa/crypto.h>

#include "mgcommands-secure.h"

/* Primitives for mgcommands-secure.h suite x25519_aes256gcm_sha256, on the
 * PSA Crypto API. */

struct mg_x25519 {
	psa_key_id_t key;
	uint8_t pub[MG_PUBLIC_KEY_SIZE];
};

struct mg_aead {
	psa_key_id_t key;
};

/* Everything derived from one key exchange. */
struct mg_session_keys {
	uint8_t th[MG_HASH_SIZE];
	uint8_t prk[MG_HASH_SIZE];
	uint8_t c2d_control[32];
	uint8_t c2d_data[32];
	uint8_t d2c_control[32];
	uint8_t d2c_data[32];
	uint8_t pairing_code[4];
	uint8_t pairing_key[32];
	uint8_t key_id[MG_KEY_ID_SIZE];
};

int mg_crypto_init(void);
int mg_random(uint8_t *buf, size_t len);
bool mg_ct_equal(const uint8_t *a, const uint8_t *b, size_t len);

int mg_x25519_generate(struct mg_x25519 *k);
int mg_x25519_import(struct mg_x25519 *k, const uint8_t priv[32]);
/* Fails on an all-zero shared secret. */
int mg_x25519_shared(const struct mg_x25519 *k, const uint8_t peer[32], uint8_t ss[32]);
void mg_x25519_destroy(struct mg_x25519 *k);

/* SHA-256 / HMAC-SHA256 over the concatenation of up to 4 parts. */
int mg_sha256(const uint8_t *const parts[], const size_t lens[], int n, uint8_t out[32]);
int mg_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *const parts[],
		   const size_t lens[], int n, uint8_t out[32]);
/* HKDF-Expand with a single block (len <= 32). */
int mg_hkdf_expand(const uint8_t prk[32], const char *info, uint8_t *out, size_t len);

/* th, prk and every key from the transcript M1..M3 and the shared secret. */
int mg_derive(const uint8_t *m1, size_t m1_len, const uint8_t *m2, size_t m2_len,
	      const uint8_t *m3, size_t m3_len, const uint8_t ss[32], struct mg_session_keys *k);
/* HMAC-SHA256(PK, label || th). */
int mg_auth_mac(const uint8_t pk[32], const char *label, const uint8_t th[32], uint8_t mac[32]);
/* LE32(code bytes) % 1000000. */
uint32_t mg_pairing_code_value(const uint8_t code[4]);

int mg_aead_setup(struct mg_aead *a, const uint8_t key[32]);
void mg_aead_destroy(struct mg_aead *a);
/* Builds [flags=0, seq, ciphertext, tag]; out needs len + MG_FRAME_OVERHEAD. */
int mg_frame_seal(const struct mg_aead *a, uint32_t seq, const uint8_t *pt, size_t len,
		  uint8_t *out);
/* Checks flags and tag (not seq ordering); returns plaintext length or <0. */
int mg_frame_open(const struct mg_aead *a, const uint8_t *frame, size_t len, uint32_t *seq,
		  uint8_t *pt);

#endif
