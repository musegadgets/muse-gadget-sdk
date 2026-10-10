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

#include "mg_setup.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#if defined(CONFIG_MG_BENCH)
#include <zephyr/sys/printk.h>
#endif

#include "mg_app.h"
#include "mg_core.h"
#include "mg_identity.h"
#include "mg_json.h"
#include "mg_led.h"
#include "mg_psa.h"
#include "mg_setup_crypto.h"
#include "mg_setup_store.h"
#include "mg_token_proof.h"
#include "mg_transport.h"
#include "pairing_transcript.h"

LOG_MODULE_REGISTER(mg_setup, CONFIG_MG_LOG_LEVEL);

/* Framing (ble_server.c). */
#define CHUNK_MAGIC  0xFE
#define CHUNK_HDR    3
#define MAX_NOTIFY   160
/* An encrypted command's ciphertext is at most 4096 base64 characters
 * (3 KB, link_pairing.c); the envelope around it fits in the rest. */
#define RX_MAX       4352
#define PT_MAX       3072
#define TX_MAX       1024
/* The device encrypts only statuses and an empty scan result. */
#define ENC_PT_MAX   384
#define SINGLE_MAX   248
#define STATUS_MAX   64

#define SDK_TOKEN CONFIG_MG_SDK_TOKEN

enum phase {
	PH_IDLE,
	PH_WAIT_FINISHED,
	PH_CONFIRM_REQUIRED,
	PH_READY,
	PH_PROVISIONING,
	PH_DONE, /* provisioned on this session: auth_ok sent */
};

static struct {
	enum phase phase;
	uint32_t gen; /* changes with every phase and reset (link_pairing.c) */
	bool armed;   /* confirm_required went out: the button may confirm */
	int64_t deadline;
	uint64_t rx_ctr, tx_ctr;
	struct mg_setup_keys k;
	bool plaintext_blocked;
	bool connected;
} s;

/* Reassembly of chunked writes (one message at a time). */
static struct {
	size_t len;
	uint8_t total, next, count;
	bool active;
} rx;

/* + the tag, which goes after the ciphertext decoded in place. */
static char rxbuf[RX_MAX + 1 + MG_SETUP_TAG_LEN];
static char single[SINGLE_MAX + 1 + MG_SETUP_TAG_LEN];
static uint8_t cbuf[ENC_PT_MAX + MG_SETUP_TAG_LEN]; /* a record being sealed */
static char txbuf[TX_MAX];
static char jbuf[192 + sizeof(SDK_TOKEN)];

static char last_status[STATUS_MAX] = "idle";
static K_MUTEX_DEFINE(status_lock);

static struct k_work_delayable phase_work;      /* confirm / confirmed-idle timeouts */
static uint32_t phase_work_gen;
static struct k_work_delayable disconnect_work; /* a disconnect a little later */
static uint32_t disconnect_gen;                 /* 0: unconditional */

static bool test_eph;
static uint8_t test_priv[32];
static uint8_t test_nonce[MG_SETUP_NONCE_LEN];

void mg_setup_test_set_ephemeral(const uint8_t priv[32], const uint8_t nonce[16])
{
	memcpy(test_priv, priv, 32);
	memcpy(test_nonce, nonce, 16);
	test_eph = true;
}

/* ------------------------------------------------------------------------
 * Session (link_pairing.c)
 * ---------------------------------------------------------------------- */

static void advance_gen(void)
{
	if (++s.gen == 0) {
		++s.gen; /* zero means "no session" */
	}
}

static void clear_session(void)
{
	mg_psa_wipe(&s.k, sizeof(s.k));
	s.deadline = 0;
	s.rx_ctr = 0;
	s.tx_ctr = 0;
	s.phase = PH_IDLE;
	if (s.armed) {
		mg_led_set(MG_LED_PAIR_PENDING, false);
	}
	s.armed = false;
}

static void reset_session(void)
{
	advance_gen();
	clear_session();
	(void)k_work_cancel_delayable(&phase_work);
}

static bool expired(void)
{
	if (s.phase == PH_IDLE || s.deadline == 0 || k_uptime_get() <= s.deadline) {
		return false;
	}
	LOG_WRN("setup session timed out");
	clear_session();
	return true;
}

static bool keys_available(void)
{
	return s.phase != PH_IDLE;
}

static bool confirmed(void)
{
	return s.phase == PH_READY || s.phase == PH_PROVISIONING;
}

static void set_deadline(int ms)
{
	s.deadline = k_uptime_get() + ms;
}

/* ------------------------------------------------------------------------
 * Sending (ble_server.c)
 * ---------------------------------------------------------------------- */

static bool notify(const uint8_t *buf, size_t len)
{
	return s.connected &&
	       mg_transport_notify(MG_ATT_SETUP, buf, len, K_MSEC(500)) == 0;
}

static void set_last_status(const char *st, size_t n)
{
	k_mutex_lock(&status_lock, K_FOREVER);
	n = MIN(n, sizeof(last_status) - 1);
	memcpy(last_status, st, n);
	last_status[n] = '\0';
	k_mutex_unlock(&status_lock);
}

size_t mg_setup_read_status(char *out, size_t cap)
{
	k_mutex_lock(&status_lock, K_FOREVER);
	size_t n = MIN(strlen(last_status), cap);

	memcpy(out, last_status, n);
	k_mutex_unlock(&status_lock);
	return n;
}

/* [0xFE, index, total, fragment], each at most 160 bytes and MTU - 3. */
static bool send_chunked(const char *data, size_t total_len)
{
	uint8_t pkt[MAX_NOTIFY];
	size_t max = mg_transport_att_mtu();

	if (max <= 3) {
		return false;
	}
	max = MIN(max - 3, (size_t)MAX_NOTIFY);
	if (max <= CHUNK_HDR) {
		return false;
	}
	size_t usable = max - CHUNK_HDR;
	size_t chunks = MAX((total_len + usable - 1) / usable, (size_t)1);

	if (chunks > 255) {
		LOG_WRN("TX message exceeds the framing limit");
		return false;
	}
	for (size_t i = 0; i < chunks; i++) {
		size_t off = i * usable;
		size_t n = MIN(usable, total_len - off);

		pkt[0] = CHUNK_MAGIC;
		pkt[1] = (uint8_t)i;
		pkt[2] = (uint8_t)chunks;
		memcpy(&pkt[CHUNK_HDR], &data[off], n);
		if (!notify(pkt, n + CHUNK_HDR)) {
			return false;
		}
	}
	return true;
}

static bool plaintext_status_allowed(const char *st)
{
	return strcmp(st, "error_encryption_required") == 0 ||
	       strcmp(st, "error_pairing_invalid_hello") == 0 ||
	       strcmp(st, "error_pairing_unavailable") == 0 ||
	       strcmp(st, "error_pairing_decrypt") == 0;
}

static void u64_dec(uint64_t v, char out[21])
{
	char tmp[21];
	size_t i = sizeof(tmp);

	do {
		tmp[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	memcpy(out, &tmp[i], sizeof(tmp) - i);
	out[sizeof(tmp) - i] = '\0';
}

/* Encrypts plain into a pairing_encrypted envelope in txbuf and sends it.
 * gen 0: the current session; otherwise only that phase's. */
static bool send_encrypted(const char *plain, uint32_t gen)
{
	size_t n = strlen(plain);
	char ctr[21], tag_b64[24];
	struct mg_jw w;
	int len;

	if ((gen != 0 && gen != s.gen) || expired() || !keys_available()) {
		return false;
	}
	if (n > ENC_PT_MAX) {
		return false;
	}
	if (mg_setup_seal(s.k.tx, MG_SETUP_D2M, s.tx_ctr, s.k.session_id_b64,
			  (const uint8_t *)plain, n, cbuf)) {
		return false;
	}
	static char ct_b64[(ENC_PT_MAX + 2) / 3 * 4 + 1];

	if (mg_b64url_encode(cbuf, n, ct_b64, sizeof(ct_b64)) < 0 ||
	    mg_b64url_encode(&cbuf[n], MG_SETUP_TAG_LEN, tag_b64, sizeof(tag_b64)) < 0) {
		return false;
	}
	u64_dec(s.tx_ctr, ctr);
	mg_jw_begin(&w, txbuf, sizeof(txbuf));
	mg_jw_str(&w, "type", "pairing_encrypted");
	mg_jw_str(&w, "session_id", s.k.session_id_b64);
	mg_jw_str(&w, "counter", ctr);
	mg_jw_str(&w, "ciphertext", ct_b64);
	mg_jw_str(&w, "tag", tag_b64);
	len = mg_jw_end(&w);
	mg_psa_wipe(cbuf, n + MG_SETUP_TAG_LEN);
	if (len < 0) {
		return false;
	}
	/* The counter is spent once the record exists, sent or not. */
	s.tx_ctr++;
	return send_chunked(txbuf, (size_t)len);
}

/* ble_server.c send_status(): encrypted while a session has keys; otherwise
 * plaintext, and then only the four errors a client may see before one. */
static bool send_status(const char *st, uint32_t gen)
{
	struct mg_jw w;

	if (keys_available() && !expired() && (gen == 0 || gen == s.gen)) {
		mg_jw_begin(&w, jbuf, sizeof(jbuf));
		mg_jw_str(&w, "type", "status");
		mg_jw_str(&w, "status", st);
		if (SDK_TOKEN[0] != '\0' && strcmp(st, "pairing_confirmed") == 0) {
			mg_jw_str(&w, "sdk_token", SDK_TOKEN);
			LOG_INF("pairing_confirmed carries the SDK token (%u chars)",
				(unsigned int)strlen(SDK_TOKEN));
		}
		bool sent = mg_jw_end(&w) > 0 && send_encrypted(jbuf, gen);

		mg_psa_wipe(jbuf, sizeof(jbuf));
		set_last_status("encrypted_status", 16);
		if (sent) {
			LOG_INF("status: %s", st);
		}
		return sent;
	}
	if (gen != 0 || s.plaintext_blocked || !plaintext_status_allowed(st)) {
		LOG_WRN("plaintext status suppressed: %s", st);
		return false;
	}
	set_last_status(st, strlen(st));
	bool sent = notify((const uint8_t *)st, strlen(st));

	if (sent) {
		LOG_INF("status: %s", st);
	}
	return sent;
}

static void disconnect_later(int ms, uint32_t gen)
{
	disconnect_gen = gen;
	k_work_reschedule_for_queue(mg_app_wq(), &disconnect_work, K_MSEC(ms));
}

static void disconnect_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (disconnect_gen != 0) {
		/* ble_server_disconnect_pairing_session(): only if that session is
		 * still the current one. */
		if (disconnect_gen != s.gen) {
			return;
		}
		reset_session();
	}
	if (s.connected) {
		LOG_INF("disconnecting the setup client");
		mg_transport_disconnect();
	}
}

/* ------------------------------------------------------------------------
 * Handshake
 * ---------------------------------------------------------------------- */

static bool json_nonempty(const struct mg_json_obj *o, const char *key, const char **out)
{
	const char *v = mg_json_str(o, key);

	if (v == NULL || v[0] == '\0') {
		return false;
	}
	*out = v;
	return true;
}

static const char *handle_hello(const struct mg_json_obj *o, int *reply_len)
{
	const struct mg_json_field *ver = mg_json_get(o, "version");
	const char *auth, *policy, *mpub_s, *mnonce_s;
	uint8_t mpub[MG_SETUP_P256_PUB_LEN], dpub[MG_SETUP_P256_PUB_LEN];
	uint8_t mnonce[MG_SETUP_NONCE_LEN], dnonce[MG_SETUP_NONCE_LEN];
	uint8_t ss[32], th[32];
	char mpub_b64[96], dpub_b64[96], mn_b64[32], dn_b64[32], th_b64[48];
	psa_key_id_t key = PSA_KEY_ID_NULL;
	size_t n;
	int err;

	if (ver == NULL || ver->type != MG_JSON_NUMBER || !ver->num_is_int || ver->num != 5 ||
	    !json_nonempty(o, "pairing_auth", &auth) || strcmp(auth, PAIRING_COMMUNITY_AUTH) != 0 ||
	    !json_nonempty(o, "pairing_policy", &policy) ||
	    strcmp(policy, PAIRING_POLICY_BUTTON) != 0 || !json_nonempty(o, "mobile_pub", &mpub_s) ||
	    !json_nonempty(o, "mobile_nonce", &mnonce_s)) {
		return "error_pairing_invalid_hello";
	}
	/* Every hello starts over. */
	reset_session();
	if (mg_b64url_decode(mpub_s, strlen(mpub_s), mpub, sizeof(mpub), &n) || n != sizeof(mpub) ||
	    mpub[0] != 0x04 ||
	    mg_b64url_decode(mnonce_s, strlen(mnonce_s), mnonce, sizeof(mnonce), &n) ||
	    n != sizeof(mnonce)) {
		reset_session();
		return "error_pairing_invalid_hello";
	}
	if (test_eph) {
		test_eph = false;
		err = mg_setup_p256_import(&key, test_priv, dpub);
		memcpy(dnonce, test_nonce, sizeof(dnonce));
	} else {
		err = mg_setup_p256_generate(&key, dpub);
		err = err ?: mg_psa_random(dnonce, sizeof(dnonce));
	}
	/* The client's point is checked by the key agreement. */
	err = err ?: mg_setup_ecdh(key, mpub, ss);
	if (key != PSA_KEY_ID_NULL) {
		(void)psa_destroy_key(key);
	}
	if (err || mg_b64url_encode(mpub, sizeof(mpub), mpub_b64, sizeof(mpub_b64)) < 0 ||
	    mg_b64url_encode(dpub, sizeof(dpub), dpub_b64, sizeof(dpub_b64)) < 0 ||
	    mg_b64url_encode(mnonce, sizeof(mnonce), mn_b64, sizeof(mn_b64)) < 0 ||
	    mg_b64url_encode(dnonce, sizeof(dnonce), dn_b64, sizeof(dn_b64)) < 0) {
		mg_psa_wipe(ss, sizeof(ss));
		reset_session();
		return "error_pairing_invalid_hello";
	}
	const pairing_transcript_fields_t fields = {
		.device_id = mg_identity_device_id(),
		.node_id = mg_identity_node_id(),
		.mac = mg_identity_mac(),
		.firmware_version = mg_app_version(),
		.mobile_pub = mpub_b64,
		.device_pub = dpub_b64,
		.mobile_nonce = mn_b64,
		.device_nonce = dn_b64,
	};
	char *transcript = pairing_transcript_build(true, 0, PAIRING_POLICY_BUTTON, &fields);

	err = transcript == NULL ||
	      mg_psa_sha256((const uint8_t *)transcript, strlen(transcript), th) ||
	      mg_b64url_encode(th, sizeof(th), th_b64, sizeof(th_b64)) < 0 ||
	      mg_setup_derive(ss, mnonce, dnonce, th, &s.k, NULL);
	free(transcript);
	mg_psa_wipe(ss, sizeof(ss));
	if (err) {
		reset_session();
		return "error_pairing_invalid_hello";
	}

	struct mg_jw w;

	mg_jw_begin(&w, txbuf, sizeof(txbuf));
	mg_jw_str(&w, "type", "pairing_ready");
	mg_jw_int(&w, "version", PAIRING_VERSION);
	mg_jw_str(&w, "device_id", mg_identity_device_id());
	mg_jw_str(&w, "node_id", mg_identity_node_id());
	mg_jw_str(&w, "mac", mg_identity_mac());
	mg_jw_str(&w, "model", PAIRING_MODEL);
	mg_jw_str(&w, "firmware_version", mg_app_version());
	mg_jw_str(&w, "pairing_auth", PAIRING_COMMUNITY_AUTH);
	mg_jw_int(&w, "pairing_auth_epoch", 0);
	mg_jw_str(&w, "pairing_policy", PAIRING_POLICY_BUTTON);
	mg_jw_str(&w, "device_pub", dpub_b64);
	mg_jw_str(&w, "device_nonce", dn_b64);
	mg_jw_str(&w, "transcript_hash", th_b64);
	mg_jw_str(&w, "session_id", s.k.session_id_b64);
	*reply_len = mg_jw_end(&w);
	if (*reply_len < 0) {
		reset_session();
		return "error_pairing_invalid_hello";
	}
	set_deadline(MG_SETUP_CLIENT_FINISHED_MS);
	s.rx_ctr = 0;
	s.tx_ctr = 0;
	s.phase = PH_WAIT_FINISHED;
	LOG_INF("pairing_ready sent; waiting for client_finished");
	return NULL;
}

static bool parse_u64(const char *str, uint64_t *out)
{
	uint64_t v = 0;

	if (str == NULL || *str == '\0') {
		return false;
	}
	for (const char *p = str; *p; p++) {
		if (*p < '0' || *p > '9') {
			return false;
		}
		uint64_t d = (uint64_t)(*p - '0');

		if (v > (UINT64_MAX - d) / 10) {
			return false;
		}
		v = v * 10 + d;
	}
	*out = v;
	return true;
}

/* Decrypts a pairing_encrypted record in place: the ciphertext's base64
 * text in rxbuf becomes the ciphertext, then the tag after it, then the
 * plaintext, NUL-terminated; *out points at it. (The fields the tag may
 * overwrite were all checked first; the buffers have room for a tag past
 * the end of the message.) */
static const char *decrypt_record(const struct mg_json_obj *o, char **out, size_t *out_len)
{
	const char *sid, *ctr_s, *ct_s, *tag_s;
	uint8_t tag[MG_SETUP_TAG_LEN];
	uint64_t ctr;
	size_t cl, tl;

	if (expired() || !keys_available()) {
		reset_session();
		return "error_pairing_decrypt";
	}
	if (!json_nonempty(o, "session_id", &sid) || strcmp(sid, s.k.session_id_b64) != 0 ||
	    !json_nonempty(o, "counter", &ctr_s) || !parse_u64(ctr_s, &ctr) || ctr != s.rx_ctr ||
	    !json_nonempty(o, "ciphertext", &ct_s) || !json_nonempty(o, "tag", &tag_s)) {
		reset_session();
		return "error_pairing_decrypt";
	}
	uint8_t *ct = (uint8_t *)ct_s;

	if (mg_b64url_decode(tag_s, strlen(tag_s), tag, sizeof(tag), &tl) ||
	    tl != MG_SETUP_TAG_LEN ||
	    mg_b64url_decode(ct_s, strlen(ct_s), ct, PT_MAX, &cl)) {
		reset_session();
		return "error_pairing_decrypt";
	}
	memcpy(&ct[cl], tag, MG_SETUP_TAG_LEN);
	if (mg_setup_open(s.k.rx, MG_SETUP_M2D, s.rx_ctr, s.k.session_id_b64, ct,
			  cl + MG_SETUP_TAG_LEN, ct)) {
		mg_psa_wipe(ct, cl + MG_SETUP_TAG_LEN);
		reset_session();
		return "error_pairing_decrypt";
	}
	mg_psa_wipe(&ct[cl], MG_SETUP_TAG_LEN);
	ct[cl] = '\0';
	s.rx_ctr++;
	*out = (char *)ct;
	*out_len = cl;
	return NULL;
}

static void phase_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (phase_work_gen != s.gen) {
		return;
	}
	if (s.phase == PH_CONFIRM_REQUIRED) {
		LOG_WRN("pairing confirmation timed out");
	} else if (s.phase == PH_READY) {
		LOG_WRN("confirmed setup session timed out before provisioning");
	} else {
		return;
	}
	(void)send_status("pairing_confirm_timeout", s.gen);
	disconnect_later(100, s.gen);
}

static void start_phase_timer(int ms)
{
	phase_work_gen = s.gen;
	k_work_reschedule_for_queue(mg_app_wq(), &phase_work, K_MSEC(ms));
}

static bool exact_client_finished(const struct mg_json_obj *o)
{
	const char *a = mg_json_str(o, "action");

	return o->n == 1 && a != NULL && strcmp(a, "pairing_client_finished") == 0;
}

static void handle_client_finished(const struct mg_json_obj *o)
{
	bool ok = exact_client_finished(o) && !expired() && s.phase == PH_WAIT_FINISHED &&
		  s.rx_ctr == 1;

	if (!ok) {
		(void)send_status("error_pairing_decrypt", 0);
		reset_session();
		disconnect_later(300, 0);
		return;
	}
	advance_gen();
	s.phase = PH_CONFIRM_REQUIRED;
	set_deadline(MG_SETUP_CONFIRM_MS);
	/* The prompt goes out before the button is armed, so an early press
	 * can't make pairing_confirmed overtake it. */
	if (!send_status("confirm_required", s.gen)) {
		disconnect_later(0, s.gen);
		return;
	}
	s.armed = true;
	set_deadline(MG_SETUP_CONFIRM_MS);
	mg_led_set(MG_LED_PAIR_PENDING, true);
	start_phase_timer(MG_SETUP_CONFIRM_MS);
	LOG_INF("setup: press the button to confirm");
#if defined(CONFIG_MG_BENCH)
	/* For a test rig on the bench console, which answers ">pair.confirm". */
	printk("@pair.pending\n");
#endif
}

bool mg_setup_button(void)
{
	if (expired() || s.phase != PH_CONFIRM_REQUIRED) {
		return false;
	}
	/* While a confirmation is pending the press is never push-to-talk. */
	if (!s.armed || !s.connected) {
		LOG_INF("press ignored: no confirmation armed");
		return true;
	}
	advance_gen();
	s.phase = PH_READY;
	s.armed = false;
	mg_led_set(MG_LED_PAIR_PENDING, false);
	set_deadline(MG_SETUP_CONFIRMED_IDLE_MS);
	start_phase_timer(MG_SETUP_CONFIRMED_IDLE_MS);
	LOG_INF("setup confirmed on the device");
	if (!send_status("pairing_confirmed", s.gen)) {
		disconnect_later(0, s.gen);
	}
	return true;
}

/* ------------------------------------------------------------------------
 * Provisioning: token-only (wifi "none")
 * ---------------------------------------------------------------------- */

/* Absent, null or an empty string. */
static bool absent_or_empty(const struct mg_json_obj *o, const char *key)
{
	const struct mg_json_field *f = mg_json_get(o, key);

	return f == NULL || f->type == MG_JSON_NULL ||
	       (f->type == MG_JSON_STRING && f->str_len == 0);
}

static void provision_failed(const char *status, uint32_t gen)
{
	LOG_WRN("provisioning failed (%s); nothing kept", status);
	(void)send_status(status, gen);
	disconnect_later(500, gen);
}

static void handle_provision(const struct mg_json_obj *o)
{
	const struct mg_json_field *at = mg_json_get(o, "access_token");
	const struct mg_json_field *rt = mg_json_get(o, "refresh_token");
	const char *tt = mg_json_str(o, "token_type");
	uint8_t k[MG_SETUP_K_LEN];
	uint32_t gen;

	if (at == NULL || at->type != MG_JSON_STRING || at->str_len == 0 || rt == NULL ||
	    rt->type != MG_JSON_STRING || rt->str_len == 0 || tt == NULL ||
	    strcmp(tt, "device") != 0) {
		(void)send_status("error_missing_credentials", 0);
		return;
	}
	/* wifi "none": no Wi-Fi to join; nothing is stored. */
	if (!absent_or_empty(o, "ssid") || !absent_or_empty(o, "password")) {
		(void)send_status("error_wifi_unsupported", 0);
		return;
	}
	/* link_pairing_mark_provisioning_active() */
	if (!expired() && s.phase == PH_READY) {
		advance_gen();
		s.phase = PH_PROVISIONING;
		set_deadline(MG_SETUP_PROVISIONING_MS);
		(void)k_work_cancel_delayable(&phase_work);
	}
	if (s.phase != PH_PROVISIONING) {
		(void)send_status("error_operation_in_progress", 0);
		disconnect_later(0, s.gen);
		return;
	}
	gen = s.gen;
	LOG_INF("token-only provisioning: access %u B, refresh %u B", (unsigned int)at->str_len,
		(unsigned int)rt->str_len);
	if (at->str_len > MG_SETUP_TOKEN_MAX || rt->str_len > MG_SETUP_TOKEN_MAX) {
		LOG_ERR("token longer than %d bytes", MG_SETUP_TOKEN_MAX);
		provision_failed("error_storage", gen);
		return;
	}
	if (mg_token_proof_derive_key(at->str, at->str_len, mg_identity_node_id(), k) ||
	    mg_setup_store_commit(at->str, at->str_len, rt->str, rt->str_len, k)) {
		mg_psa_wipe(k, sizeof(k));
		(void)mg_setup_store_erase();
		provision_failed("error_storage", gen);
		return;
	}
	mg_psa_wipe(k, sizeof(k));
	/* Durable: only now auth_ok. */
	s.phase = PH_DONE;
	s.deadline = 0;
	(void)send_status("auth_ok", gen);
	LOG_INF("setup complete; Link setup advertising stops");
	mg_ble_refresh_advertising();
	mg_core_link_refresh();
}

static void handle_unpair(void)
{
	if (mg_setup_store_erase()) {
		(void)send_status("error_storage", 0);
		return;
	}
	(void)send_status("unpaired", 0);
	reset_session();
	mg_ble_refresh_advertising();
	mg_core_link_refresh();
	disconnect_later(300, 0);
}

/* ------------------------------------------------------------------------
 * Dispatch (ble_server.c dispatch_command_ex)
 * ---------------------------------------------------------------------- */

static bool sensitive(const char *a)
{
	static const char *const list[] = {"provision", "provision_v2", "wifi_scan", "ota",
					   "device.ota", "unpair",       "set_wifi",  "set_auth"};

	for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++) {
		if (strcmp(a, list[i]) == 0) {
			return true;
		}
	}
	return false;
}

static void send_device_info(void)
{
	struct mg_jw w;
	int n;

	mg_jw_begin(&w, txbuf, sizeof(txbuf));
	mg_jw_str(&w, "type", "device_info");
	mg_jw_str(&w, "node_id", mg_identity_node_id());
	mg_jw_str(&w, "version", mg_app_version());
	mg_jw_str(&w, "device_id", mg_identity_device_id());
	mg_jw_str(&w, "mac", mg_identity_mac());
	mg_jw_str(&w, "model", PAIRING_MODEL);
	mg_jw_int(&w, "pairing_protocol", PAIRING_VERSION);
	mg_jw_str(&w, "pairing_auth", PAIRING_COMMUNITY_AUTH);
	mg_jw_int(&w, "pairing_auth_epoch", 0);
	mg_jw_str(&w, "pairing_policy", PAIRING_POLICY_BUTTON);
	mg_jw_str(&w, "build_sha", "");
	mg_jw_str(&w, "wifi", "none");
	mg_jw_int(&w, "mgcommands", 1);
	n = mg_jw_end(&w);
	if (n > 0) {
		(void)send_chunked(txbuf, (size_t)n);
	}
}

static void dispatch(char *data, size_t len, bool decrypted)
{
	/* One for both levels: a record's fields are done with once it opens. */
	static struct mg_json_obj obj;
	struct mg_json_obj *o = &obj;

	if (mg_json_parse(data, len, o)) {
		LOG_WRN("RX invalid JSON (%u bytes)", (unsigned int)len);
		(void)send_status("error_invalid_command", 0);
		return;
	}
	const char *act = mg_json_str(o, "action");

	if (act == NULL) {
		act = "";
	}
	LOG_INF("RX action: %s%s", act, decrypted ? " (encrypted)" : "");

	if (!decrypted && strcmp(act, "pairing_client_hello") == 0 && mg_setup_store_complete()) {
		/* Re-pairing goes through a reset, never a new session here. */
		(void)send_status("error_pairing_unavailable", 0);
	} else if (!decrypted && strcmp(act, "pairing_client_hello") == 0) {
		int n = 0;
		const char *err = handle_hello(o, &n);

		if (err) {
			(void)send_status(err, 0);
		} else {
			s.plaintext_blocked = true;
			(void)send_chunked(txbuf, (size_t)n);
		}
	} else if (!decrypted && strcmp(act, "pairing_encrypted") == 0) {
		char *pt = NULL;
		size_t pl = 0;
		const char *err = decrypt_record(o, &pt, &pl);

		if (err) {
			(void)send_status(err, 0);
			disconnect_later(300, 0);
		} else {
			dispatch(pt, pl, true);
			mg_psa_wipe(pt, pl);
		}
	} else if (!decrypted && s.plaintext_blocked) {
		LOG_WRN("plaintext command suppressed after pairing_ready: %s", act);
	} else if (!decrypted && sensitive(act)) {
		(void)send_status("error_encryption_required", 0);
	} else if (decrypted && strcmp(act, "pairing_client_finished") == 0) {
		handle_client_finished(o);
	} else if (decrypted && sensitive(act) && mg_setup_store_complete()) {
		(void)send_status("error_pairing_unavailable", 0);
	} else if (decrypted && sensitive(act) && (expired() || !confirmed())) {
		(void)send_status("error_pairing_confirm_required", 0);
	} else if (decrypted && strcmp(act, "wifi_scan") == 0) {
		/* No Wi-Fi radio: an empty list (apps skip the scan for wifi "none"). */
		(void)send_encrypted("{\"type\":\"wifi_scan_result\",\"networks\":[]}", 0);
	} else if (decrypted && strcmp(act, "unpair") == 0) {
		handle_unpair();
	} else if (decrypted && strcmp(act, "provision_v2") != 0) {
		(void)send_status("error_unknown_action", 0);
	} else if (decrypted) {
		handle_provision(o);
	} else if (strcmp(act, "get_device_info") == 0) {
		send_device_info();
	} else if (strcmp(act, "get_version") == 0) {
		char v[48] = "version:";

		strncat(v, mg_app_version(), sizeof(v) - strlen(v) - 1);
		(void)send_status(v, 0);
	} else {
		(void)send_status("error_unknown_action", 0);
	}
	if (decrypted) {
		mg_psa_wipe(o, sizeof(*o));
	}
}

/* ------------------------------------------------------------------------
 * RX framing
 * ---------------------------------------------------------------------- */

static void rx_reset(void)
{
	if (rx.active) {
		mg_psa_wipe(rxbuf, rx.len);
	}
	memset(&rx, 0, sizeof(rx));
}

void mg_setup_rx(const uint8_t *data, size_t len)
{
	if (!s.connected || len == 0) {
		return;
	}
	if (len < CHUNK_HDR || data[0] != CHUNK_MAGIC) {
		size_t n = MIN(len, (size_t)SINGLE_MAX);

		memcpy(single, data, n);
		single[n] = '\0';
		dispatch(single, n, false);
		mg_psa_wipe(single, sizeof(single));
		return;
	}
	uint8_t idx = data[1], total = data[2];
	const uint8_t *frag = &data[CHUNK_HDR];
	size_t flen = len - CHUNK_HDR;

	if (total == 0) {
		rx_reset();
		return;
	}
	if (idx == 0 || rx.total != total) {
		rx_reset();
		rx.total = total;
		rx.active = true;
	}
	if (idx != rx.next || idx >= rx.total || rx.len + flen > RX_MAX) {
		rx_reset();
		return;
	}
	memcpy(&rxbuf[rx.len], frag, flen);
	rx.len += flen;
	rx.count++;
	rx.next = idx + 1;
	if (rx.count < rx.total) {
		return;
	}
	size_t n = rx.len;

	rxbuf[n] = '\0';
	memset(&rx, 0, sizeof(rx));
	LOG_DBG("RX reassembled (%u bytes)", (unsigned int)n);
	dispatch(rxbuf, n, false);
	mg_psa_wipe(rxbuf, sizeof(rxbuf));
}

/* ------------------------------------------------------------------------
 * Connection, reset, init
 * ---------------------------------------------------------------------- */

void mg_setup_connected(void)
{
	rx_reset();
	reset_session();
	s.plaintext_blocked = false;
	s.connected = true;
	set_last_status("idle", 4);
}

void mg_setup_disconnected(void)
{
	s.connected = false;
	rx_reset();
	reset_session();
	s.plaintext_blocked = false;
	(void)k_work_cancel_delayable(&disconnect_work);
}

bool mg_setup_confirm_pending(void)
{
	return s.phase == PH_CONFIRM_REQUIRED && s.armed;
}

bool mg_setup_advertise_link(void)
{
	return !mg_setup_store_complete();
}

int mg_setup_factory_reset(const char *why)
{
	LOG_INF("%s: erasing the setup", why);
	int err = mg_setup_store_erase();

	reset_session();
	mg_token_proof_connection_reset();
	mg_ble_refresh_advertising();
	mg_core_link_refresh();
	return err;
}

void mg_setup_init(void)
{
	memset(&s, 0, sizeof(s));
	memset(&rx, 0, sizeof(rx));
	k_work_init_delayable(&phase_work, phase_fn);
	k_work_init_delayable(&disconnect_work, disconnect_fn);
	if (mg_psa_init()) {
		LOG_ERR("PSA crypto init failed");
	}
	LOG_INF("Link setup: node %s, %s", mg_identity_node_id(),
		mg_setup_store_complete() ? "set up" : "waiting for setup");
}
