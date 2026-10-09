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

#include <string.h>

#include "psa/crypto.h"

#define TRANSCRIPT_LABEL "musegadgets ble v1"

bool mg_crypto_init(void)
{
    return psa_crypto_init() == PSA_SUCCESS;
}

bool mg_crypto_random(uint8_t *out, size_t n)
{
    return psa_generate_random(out, n) == PSA_SUCCESS;
}

void mg_crypto_wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) {
        *v++ = 0;
    }
}

bool mg_crypto_equal(const void *a, const void *b, size_t n)
{
    const volatile uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) {
        d |= x[i] ^ y[i];
    }
    return d == 0;
}

static psa_key_id_t import_x25519(const uint8_t priv[32])
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&a, 255);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&a, PSA_ALG_ECDH);
    psa_key_id_t id = 0;
    if (psa_import_key(&a, priv, 32, &id) != PSA_SUCCESS) {
        return 0;
    }
    return id;
}

bool mg_crypto_x25519_public(const uint8_t priv[32], uint8_t pub[32])
{
    psa_key_id_t id = import_x25519(priv);
    if (!id) {
        return false;
    }
    size_t n = 0;
    psa_status_t st = psa_export_public_key(id, pub, 32, &n);
    psa_destroy_key(id);
    return st == PSA_SUCCESS && n == 32;
}

bool mg_crypto_x25519(const uint8_t priv[32], const uint8_t peer[32], uint8_t shared[32])
{
    psa_key_id_t id = import_x25519(priv);
    if (!id) {
        return false;
    }
    size_t n = 0;
    psa_status_t st = psa_raw_key_agreement(PSA_ALG_ECDH, id, peer, 32, shared, 32, &n);
    psa_destroy_key(id);
    if (st != PSA_SUCCESS || n != 32) {
        mg_crypto_wipe(shared, 32);
        return false;
    }
    static const uint8_t zero[32];
    return !mg_crypto_equal(shared, zero, 32);
}

bool mg_crypto_sha256(const void *a, size_t alen, const void *b, size_t blen, const void *c, size_t clen,
                      const void *d, size_t dlen, uint8_t out[32])
{
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    size_t n = 0;
    bool ok = psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS
              && (!a || psa_hash_update(&op, a, alen) == PSA_SUCCESS)
              && (!b || psa_hash_update(&op, b, blen) == PSA_SUCCESS)
              && (!c || psa_hash_update(&op, c, clen) == PSA_SUCCESS)
              && (!d || psa_hash_update(&op, d, dlen) == PSA_SUCCESS)
              && psa_hash_finish(&op, out, 32, &n) == PSA_SUCCESS && n == 32;
    if (!ok) {
        psa_hash_abort(&op);
    }
    return ok;
}

bool mg_crypto_hmac(const uint8_t *key, size_t klen, const void *a, size_t alen, const void *b, size_t blen,
                    uint8_t out[32])
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_key_id_t id = 0;
    if (psa_import_key(&attr, key, klen, &id) != PSA_SUCCESS) {
        return false;
    }
    psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;
    size_t n = 0;
    bool ok = psa_mac_sign_setup(&op, id, PSA_ALG_HMAC(PSA_ALG_SHA_256)) == PSA_SUCCESS
              && (!a || psa_mac_update(&op, a, alen) == PSA_SUCCESS)
              && (!b || psa_mac_update(&op, b, blen) == PSA_SUCCESS)
              && psa_mac_sign_finish(&op, out, 32, &n) == PSA_SUCCESS && n == 32;
    if (!ok) {
        psa_mac_abort(&op);
    }
    psa_destroy_key(id);
    return ok;
}

/* HKDF-Expand for L <= 32: the first L bytes of HMAC(prk, info || 0x01). */
static bool expand(const uint8_t prk[32], const char *info, uint8_t *out, size_t len)
{
    uint8_t t[32];
    static const uint8_t one = 1;
    bool ok = len <= 32 && mg_crypto_hmac(prk, 32, info, strlen(info), &one, 1, t);
    if (ok) {
        memcpy(out, t, len);
    }
    mg_crypto_wipe(t, sizeof(t));
    return ok;
}

bool mg_crypto_derive(const uint8_t shared[32], const uint8_t *m1, size_t m1len, const uint8_t *m2, size_t m2len,
                      const uint8_t *m3, size_t m3len, mg_session_keys_t *out)
{
    static const char *const infos[MG_KEY_COUNT] = {
        "mg1 c2d control", "mg1 c2d data", "mg1 d2c control", "mg1 d2c data",
    };
    uint8_t prk[32], code[4];
    memset(out, 0, sizeof(*out));
    bool ok = mg_crypto_sha256(TRANSCRIPT_LABEL, strlen(TRANSCRIPT_LABEL), m1, m1len, m2, m2len, m3, m3len, out->th)
              && mg_crypto_hmac(out->th, 32, shared, 32, NULL, 0, prk);   /* HKDF-Extract(salt = th, IKM = ss) */
    for (int i = 0; ok && i < MG_KEY_COUNT; i++) {
        ok = expand(prk, infos[i], out->keys[i], 32);
    }
    ok = ok && expand(prk, "mg1 pairing code", code, 4) && expand(prk, "mg1 pairing key", out->pairing_key, 32)
         && expand(prk, "mg1 key id", out->key_id, MG_KEY_ID_SIZE);
    if (ok) {
        uint32_t v = (uint32_t)code[0] | (uint32_t)code[1] << 8 | (uint32_t)code[2] << 16 | (uint32_t)code[3] << 24;
        out->pairing_code = v % 1000000u;
    } else {
        mg_crypto_wipe(out, sizeof(*out));
    }
    mg_crypto_wipe(prk, sizeof(prk));
    mg_crypto_wipe(code, sizeof(code));
    return ok;
}

bool mg_crypto_auth_mac(const uint8_t pk[32], bool device, const uint8_t th[32], uint8_t out[32])
{
    const char *label = device ? "mg1 device auth" : "mg1 client auth";
    return mg_crypto_hmac(pk, 32, label, strlen(label), th, 32, out);
}

bool mg_crypto_session_start(mg_crypto_session_t *s, const mg_session_keys_t *keys)
{
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < MG_KEY_COUNT; i++) {
        psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&a, PSA_KEY_TYPE_AES);
        psa_set_key_bits(&a, 256);
        /* Each key only ever works one way on the device. */
        bool rx = i == MG_KEY_C2D_CONTROL || i == MG_KEY_C2D_DATA;
        psa_set_key_usage_flags(&a, rx ? PSA_KEY_USAGE_DECRYPT : PSA_KEY_USAGE_ENCRYPT);
        psa_set_key_algorithm(&a, PSA_ALG_GCM);
        psa_key_id_t id = 0;
        if (psa_import_key(&a, keys->keys[i], 32, &id) != PSA_SUCCESS) {
            mg_crypto_session_end(s);
            return false;
        }
        s->id[i] = id;
    }
    return true;
}

void mg_crypto_session_end(mg_crypto_session_t *s)
{
    for (int i = 0; i < MG_KEY_COUNT; i++) {
        if (s->id[i]) {
            psa_destroy_key(s->id[i]);
            s->id[i] = 0;
        }
    }
}

static void header(uint32_t seq, uint8_t hdr[MG_FRAME_HEADER_SIZE], uint8_t nonce[12])
{
    hdr[0] = 0;
    for (int i = 0; i < 4; i++) {
        hdr[1 + i] = (uint8_t)(seq >> (8 * i));
    }
    memset(nonce, 0, 12);
    memcpy(nonce, hdr + 1, 4);
}

bool mg_crypto_seal(const mg_crypto_session_t *s, int key, uint32_t seq, const uint8_t *pt, size_t len,
                    uint8_t *frame)
{
    if (key < 0 || key >= MG_KEY_COUNT || !s->id[key]) {
        return false;
    }
    uint8_t nonce[12];
    header(seq, frame, nonce);
    size_t n = 0;
    psa_status_t st = psa_aead_encrypt(s->id[key], PSA_ALG_GCM, nonce, sizeof(nonce), frame, MG_FRAME_HEADER_SIZE,
                                       pt, len, frame + MG_FRAME_HEADER_SIZE, len + MG_FRAME_TAG_SIZE, &n);
    return st == PSA_SUCCESS && n == len + MG_FRAME_TAG_SIZE;
}

bool mg_crypto_open(const mg_crypto_session_t *s, int key, const uint8_t *frame, size_t len, uint32_t *seq,
                    uint8_t *pt)
{
    if (key < 0 || key >= MG_KEY_COUNT || !s->id[key] || len < MG_FRAME_OVERHEAD || frame[0] != 0) {
        return false;
    }
    uint32_t q = (uint32_t)frame[1] | (uint32_t)frame[2] << 8 | (uint32_t)frame[3] << 16 | (uint32_t)frame[4] << 24;
    uint8_t hdr[MG_FRAME_HEADER_SIZE], nonce[12];
    header(q, hdr, nonce);
    size_t n = 0;
    psa_status_t st = psa_aead_decrypt(s->id[key], PSA_ALG_GCM, nonce, sizeof(nonce), frame, MG_FRAME_HEADER_SIZE,
                                       frame + MG_FRAME_HEADER_SIZE, len - MG_FRAME_HEADER_SIZE, pt,
                                       len - MG_FRAME_OVERHEAD, &n);
    if (st != PSA_SUCCESS || n != len - MG_FRAME_OVERHEAD) {
        return false;
    }
    *seq = q;
    return true;
}
