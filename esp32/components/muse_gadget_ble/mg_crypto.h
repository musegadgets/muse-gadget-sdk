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

#include "mgcommands-secure.h"

/*
 * The primitives of mgcommands-secure.h suite 1 (X25519, SHA-256,
 * HKDF-SHA256, HMAC-SHA256, AES-256-GCM), on PSA Crypto: mbedTLS in IDF, and
 * the same TF-PSA-Crypto sources on the host for the vector tests.
 */

enum {
    MG_KEY_C2D_CONTROL,
    MG_KEY_C2D_DATA,
    MG_KEY_D2C_CONTROL,
    MG_KEY_D2C_DATA,
    MG_KEY_COUNT,
};

typedef struct {
    uint8_t th[MG_HASH_SIZE];            /* transcript hash */
    uint8_t keys[MG_KEY_COUNT][32];
    uint8_t pairing_key[32];             /* PK */
    uint8_t key_id[MG_KEY_ID_SIZE];
    uint32_t pairing_code;               /* 0..999999 */
} mg_session_keys_t;

bool mg_crypto_init(void);
bool mg_crypto_random(uint8_t *out, size_t n);

/* public = X25519(private, 9) */
bool mg_crypto_x25519_public(const uint8_t priv[32], uint8_t pub[32]);
/* shared = X25519(private, peer); false for an invalid key or an all-zero result */
bool mg_crypto_x25519(const uint8_t priv[32], const uint8_t peer[32], uint8_t shared[32]);

/* SHA-256 over up to four parts (NULL parts are skipped). */
bool mg_crypto_sha256(const void *a, size_t alen, const void *b, size_t blen, const void *c, size_t clen,
                      const void *d, size_t dlen, uint8_t out[32]);
/* HMAC-SHA256(key, a || b) */
bool mg_crypto_hmac(const uint8_t *key, size_t klen, const void *a, size_t alen, const void *b, size_t blen,
                    uint8_t out[32]);

/* th = SHA-256("musegadgets ble v1" || M1 || M2 || M3); then HKDF as the header says. */
bool mg_crypto_derive(const uint8_t shared[32], const uint8_t *m1, size_t m1len, const uint8_t *m2, size_t m2len,
                      const uint8_t *m3, size_t m3len, mg_session_keys_t *out);

/* client_mac or device_mac over th with PK. */
bool mg_crypto_auth_mac(const uint8_t pk[32], bool device, const uint8_t th[32], uint8_t out[32]);

/* Constant-time comparison. */
bool mg_crypto_equal(const void *a, const void *b, size_t n);
void mg_crypto_wipe(void *p, size_t n);

/* An encrypted frame: [flags 0, LE32 seq, ciphertext, 16-byte tag]. */
typedef struct {
    uint32_t id[MG_KEY_COUNT];   /* psa_key_id_t, 0 when unset */
} mg_crypto_session_t;

/* Imports the four traffic keys (and wipes nothing: the caller owns `keys`). */
bool mg_crypto_session_start(mg_crypto_session_t *s, const mg_session_keys_t *keys);
void mg_crypto_session_end(mg_crypto_session_t *s);
/* frame gets len + MG_FRAME_OVERHEAD bytes. */
bool mg_crypto_seal(const mg_crypto_session_t *s, int key, uint32_t seq, const uint8_t *pt, size_t len,
                    uint8_t *frame);
/* Checks flags and the tag (not seq order); pt gets len - MG_FRAME_OVERHEAD bytes. */
bool mg_crypto_open(const mg_crypto_session_t *s, int key, const uint8_t *frame, size_t len, uint32_t *seq,
                    uint8_t *pt);
