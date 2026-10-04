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
 * Muse Link setup on the gadget, driven as the Muse app drives it: mg_setup.c
 * with real PSA crypto, the real store over an in-memory settings fake, and
 * the app's side computed here. The gadget's identity, firmware version and
 * ephemeral key are those of the community_v5 vector, so pairing_ready must
 * carry the vector's transcript hash and session id, and the vector's own
 * client_finished record must open.
 */

#include <errno.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include "check.h"
#include "host_fakes.h"
#include "mg_identity.h"
#include "mg_json.h"
#include "mg_psa.h"
#include "mg_setup.h"
#include "mg_setup_crypto.h"
#include "mg_setup_store.h"
#include "mg_token_proof.h"
#include "mg_transport.h"
#include "vectors.h"

static const struct link_vec *V;
static uint8_t dev_tx[32], dev_rx[32]; /* the device's keys = the app's rx / tx */
static uint64_t app_ctr, dev_ctr;
static int seen; /* notes consumed */

/* ---- app side ---- */

static void write_chunked(const char *msg, size_t chunk)
{
	size_t len = strlen(msg);
	size_t total = (len + chunk - 1) / chunk;
	uint8_t pkt[256];

	for (size_t i = 0; i < total; i++) {
		size_t n = MIN(chunk, len - i * chunk);

		pkt[0] = 0xFE;
		pkt[1] = (uint8_t)i;
		pkt[2] = (uint8_t)total;
		memcpy(&pkt[3], &msg[i * chunk], n);
		mg_setup_rx(pkt, n + 3);
	}
}

static void app_send(const char *msg)
{
	if (strlen(msg) <= 100) {
		mg_setup_rx((const uint8_t *)msg, strlen(msg));
	} else {
		write_chunked(msg, 150);
	}
}

/* The next message from the device: a reassembled chunked one, or a raw
 * plaintext status. NULL if none is waiting. */
static char *next_msg(void)
{
	static char buf[4096];
	size_t len = 0;

	while (seen < host_nnotes) {
		struct host_note *n = &host_notes[seen++];

		if (n->ch != MG_ATT_SETUP) {
			continue;
		}
		if (n->len < 3 || n->data[0] != 0xFE) {
			memcpy(buf, n->data, n->len);
			buf[n->len] = '\0';
			return buf;
		}
		CHECK(n->len <= 160 && n->len <= host_mtu - 3);
		memcpy(&buf[len], &n->data[3], n->len - 3);
		len += n->len - 3;
		if (n->data[1] + 1 == n->data[2]) {
			buf[len] = '\0';
			return buf;
		}
	}
	return NULL;
}

static bool no_more(void)
{
	return next_msg() == NULL;
}

static void app_send_enc(const char *plain)
{
	static uint8_t ct[3200];
	static char ct_b64[4400], env[4600];
	char tag_b64[32], ctr[24];
	size_t n = strlen(plain);
	char sid[24];

	strcpy(sid, V->session_id);
	CHECK(mg_setup_seal(dev_rx, MG_SETUP_M2D, app_ctr, sid, (const uint8_t *)plain, n, ct) ==
	      0);
	mg_b64url_encode(ct, n, ct_b64, sizeof(ct_b64));
	mg_b64url_encode(&ct[n], 16, tag_b64, sizeof(tag_b64));
	snprintf(ctr, sizeof(ctr), "%llu", (unsigned long long)app_ctr++);
	snprintf(env, sizeof(env),
		 "{\"action\":\"pairing_encrypted\",\"session_id\":\"%s\",\"counter\":\"%s\","
		 "\"ciphertext\":\"%s\",\"tag\":\"%s\"}",
		 sid, ctr, ct_b64, tag_b64);
	app_send(env);
}

/* Opens the device's next record; returns its plaintext (NULL on failure). */
static char *app_recv_enc(void)
{
	static char pt[600];
	static struct mg_json_obj o;
	char *m = next_msg();
	uint8_t raw[700];
	size_t cl, tl;
	char ctr[24];

	if (!m) {
		return NULL;
	}
	if (mg_json_parse(m, strlen(m), &o) || !mg_json_str(&o, "type") ||
	    strcmp(mg_json_str(&o, "type"), "pairing_encrypted") != 0) {
		printf("    not a record: %s\n", m);
		return NULL;
	}
	snprintf(ctr, sizeof(ctr), "%llu", (unsigned long long)dev_ctr);
	CHECK_STR(mg_json_str(&o, "session_id"), V->session_id);
	CHECK_STR(mg_json_str(&o, "counter"), ctr);
	const char *c = mg_json_str(&o, "ciphertext"), *t = mg_json_str(&o, "tag");

	if (!c || !t || mg_b64url_decode(c, strlen(c), raw, sizeof(raw) - 16, &cl) ||
	    mg_b64url_decode(t, strlen(t), &raw[cl], 16, &tl) || tl != 16 ||
	    mg_setup_open(dev_tx, MG_SETUP_D2M, dev_ctr, V->session_id, raw, cl + 16,
			  (uint8_t *)pt)) {
		printf("    record doesn't open\n");
		return NULL;
	}
	dev_ctr++;
	pt[cl] = '\0';
	return pt;
}

static bool status_is(const char *plain, const char *want)
{
	static struct mg_json_obj o;
	static char b[600];

	if (!plain) {
		return false;
	}
	strcpy(b, plain);
	return mg_json_parse(b, strlen(b), &o) == 0 && mg_json_str(&o, "type") &&
	       strcmp(mg_json_str(&o, "type"), "status") == 0 && mg_json_str(&o, "status") &&
	       strcmp(mg_json_str(&o, "status"), want) == 0;
}

static void hello(void)
{
	char h[400];

	snprintf(h, sizeof(h),
		 "{\"action\":\"pairing_client_hello\",\"version\":5,\"pairing_auth\":\"none\","
		 "\"pairing_policy\":\"confirm_press\",\"mobile_pub\":\"%s\","
		 "\"mobile_nonce\":\"%s\"}",
		 V->mobile_pub, V->mobile_nonce);
	app_send(h);
}

/* ---- setup ---- */

static void boot(void)
{
	static const uint8_t addr[6] = {0x02, 0, 0, 0, 0, 0x01};

	mg_identity_init("MuseGadget-", addr);
	(void)mg_setup_store_load();
	mg_setup_init();
	mg_token_proof_connection_reset();
}

static void connect(void)
{
	host_notes_clear();
	seen = 0;
	app_ctr = dev_ctr = 0;
	mg_setup_connected();
}

static void fresh(void)
{
	host_settings_clear();
	host_disconnects = 0;
	host_now_ms = 1000;
	boot();
	connect();
}

/* hello .. pairing_confirmed, checked step by step. */
static void handshake(void)
{
	static struct mg_json_obj o;
	uint8_t dpriv[32], dnonce[16];
	size_t n;

	memcpy(dpriv, V->device_priv, 32);
	mg_b64url_decode(V->device_nonce, strlen(V->device_nonce), dnonce, 16, &n);
	mg_setup_test_set_ephemeral(dpriv, dnonce);
	hello();
	char *m = next_msg();

	CHECK(m != NULL);
	if (!m) {
		return;
	}
	CHECK(mg_json_parse(m, strlen(m), &o) == 0);
	CHECK_STR(mg_json_str(&o, "type"), "pairing_ready");
	CHECK(mg_json_get(&o, "version") && mg_json_get(&o, "version")->num == 5);
	CHECK_STR(mg_json_str(&o, "device_id"), V->device_id);
	CHECK_STR(mg_json_str(&o, "node_id"), V->node_id);
	CHECK_STR(mg_json_str(&o, "mac"), V->mac);
	CHECK_STR(mg_json_str(&o, "model"), "hatch_link");
	CHECK_STR(mg_json_str(&o, "firmware_version"), "1.0.0");
	CHECK_STR(mg_json_str(&o, "pairing_auth"), "none");
	CHECK(mg_json_get(&o, "pairing_auth_epoch") &&
	      mg_json_get(&o, "pairing_auth_epoch")->num == 0);
	CHECK_STR(mg_json_str(&o, "pairing_policy"), "confirm_press");
	CHECK_STR(mg_json_str(&o, "device_pub"), V->device_pub);
	CHECK_STR(mg_json_str(&o, "device_nonce"), V->device_nonce);
	CHECK_STR(mg_json_str(&o, "transcript_hash"), V->transcript_hash);
	CHECK_STR(mg_json_str(&o, "session_id"), V->session_id);
	CHECK(mg_json_get(&o, "device_proof") == NULL);

	/* The vector's own client_finished record. */
	char env[300];

	snprintf(env, sizeof(env),
		 "{\"action\":\"pairing_encrypted\",\"session_id\":\"%s\",\"counter\":\"0\","
		 "\"ciphertext\":\"%s\",\"tag\":\"%s\"}",
		 V->session_id, V->cf_ct, V->cf_tag);
	app_send(env);
	app_ctr = 1;
	CHECK(status_is(app_recv_enc(), "confirm_required"));
	CHECK(host_pair_led);

	/* The talk button confirms; pairing_confirmed carries the SDK token. */
	CHECK(mg_setup_button() == true);
	CHECK(!host_pair_led);
	char *pc = app_recv_enc();

	CHECK(status_is(pc, "pairing_confirmed"));
	CHECK(pc && strstr(pc, "\"sdk_token\":\"" CONFIG_MG_SDK_TOKEN "\"") != NULL);
}

/* ---- tests ---- */

static void test_device_info(void)
{
	static struct mg_json_obj o;

	printf("  device_info\n");
	fresh();
	app_send("{\"action\":\"get_device_info\"}");
	char *m = next_msg();

	CHECK(m && mg_json_parse(m, strlen(m), &o) == 0);
	CHECK_STR(mg_json_str(&o, "type"), "device_info");
	CHECK_STR(mg_json_str(&o, "node_id"), "homelink-000001");
	CHECK_STR(mg_json_str(&o, "version"), "1.0.0");
	CHECK_STR(mg_json_str(&o, "device_id"), "hatch-link:02:00:00:00:00:01");
	CHECK_STR(mg_json_str(&o, "mac"), "02:00:00:00:00:01");
	CHECK_STR(mg_json_str(&o, "model"), "hatch_link");
	CHECK(mg_json_get(&o, "pairing_protocol") && mg_json_get(&o, "pairing_protocol")->num == 5);
	CHECK_STR(mg_json_str(&o, "pairing_auth"), "none");
	CHECK(mg_json_get(&o, "pairing_auth_epoch")->num == 0);
	CHECK_STR(mg_json_str(&o, "pairing_policy"), "confirm_press");
	CHECK_STR(mg_json_str(&o, "wifi"), "none");
	CHECK(mg_json_get(&o, "mgcommands") && mg_json_get(&o, "mgcommands")->type == MG_JSON_NUMBER &&
	      mg_json_get(&o, "mgcommands")->num == 1);
	CHECK(mg_setup_advertise_link());
}

static void test_plaintext_rules(void)
{
	printf("  plaintext rules\n");
	fresh();
	/* Only the four errors are sent in plaintext before a session. */
	app_send("{\"action\":\"no_such_action\"}");
	CHECK(no_more());
	app_send("not json");
	CHECK(no_more());
	app_send("{\"action\":\"provision_v2\",\"access_token\":\"a\"}");
	CHECK_STR(next_msg(), "error_encryption_required");
	app_send("{\"action\":\"wifi_scan\"}");
	CHECK_STR(next_msg(), "error_encryption_required");
	app_send("{\"action\":\"pairing_client_hello\",\"version\":4,\"pairing_auth\":\"none\"}");
	CHECK_STR(next_msg(), "error_pairing_invalid_hello");
	app_send("{\"action\":\"pairing_client_hello\",\"version\":5,\"pairing_auth\":"
		 "\"fleet_ecdsa_p256_v1\",\"pairing_policy\":\"confirm_press\",\"mobile_pub\":\"x\","
		 "\"mobile_nonce\":\"y\"}");
	CHECK_STR(next_msg(), "error_pairing_invalid_hello");
	/* A point off the curve. */
	char h[400], pub[100];

	strcpy(pub, V->mobile_pub);
	pub[60] = pub[60] == 'A' ? 'B' : 'A';
	snprintf(h, sizeof(h),
		 "{\"action\":\"pairing_client_hello\",\"version\":5,\"pairing_auth\":\"none\","
		 "\"pairing_policy\":\"confirm_press\",\"mobile_pub\":\"%s\",\"mobile_nonce\":\"%s\"}",
		 pub, V->mobile_nonce);
	app_send(h);
	CHECK_STR(next_msg(), "error_pairing_invalid_hello");
	app_send("{\"action\":\"pairing_encrypted\",\"session_id\":\"x\",\"counter\":\"0\","
		 "\"ciphertext\":\"AAAA\",\"tag\":\"AAAA\"}");
	CHECK_STR(next_msg(), "error_pairing_decrypt");
	host_advance(400);
	CHECK(host_disconnects == 1);

	/* After pairing_ready, plaintext commands are ignored (even device_info). */
	fresh();
	handshake();
	app_send("{\"action\":\"get_device_info\"}");
	CHECK(no_more());
	app_send("{\"action\":\"provision_v2\"}");
	CHECK(no_more());
	/* The press is never push-to-talk while a confirmation is pending; with
	 * none pending it is. */
	CHECK(mg_setup_button() == false);
}

static void test_provision(void)
{
	const struct proof_vec *pv = &proof_vecs[2]; /* node homelink-000001 */
	char msg[1400];

	printf("  token-only provision_v2\n");
	CHECK_STR(pv->node_id, "homelink-000001");
	fresh();
	handshake();

	/* Missing credentials and Wi-Fi fields: answered, nothing stored. */
	app_send_enc("{\"action\":\"provision_v2\",\"access_token\":\"a\",\"token_type\":\"device\"}");
	CHECK(status_is(app_recv_enc(), "error_missing_credentials"));
	app_send_enc("{\"action\":\"provision_v2\",\"access_token\":\"a\",\"refresh_token\":\"r\","
		     "\"token_type\":\"user\"}");
	CHECK(status_is(app_recv_enc(), "error_missing_credentials"));
	app_send_enc("{\"action\":\"provision_v2\",\"ssid\":\"home\",\"password\":\"pw\","
		     "\"access_token\":\"a\",\"refresh_token\":\"r\",\"token_type\":\"device\"}");
	CHECK(status_is(app_recv_enc(), "error_wifi_unsupported"));
	app_send_enc("{\"action\":\"provision_v2\",\"ssid\":\"\",\"password\":\"pw\","
		     "\"access_token\":\"a\",\"refresh_token\":\"r\",\"token_type\":\"device\"}");
	CHECK(status_is(app_recv_enc(), "error_wifi_unsupported"));
	CHECK(host_settings_count() == 0 && !mg_setup_store_complete());
	app_send_enc("{\"action\":\"get_device_info\"}");
	CHECK(status_is(app_recv_enc(), "error_unknown_action"));

	/* Token-only: empty / absent Wi-Fi fields, extra fields ignored. */
	snprintf(msg, sizeof(msg),
		 "{\"action\":\"provision_v2\",\"ssid\":\"\",\"password\":null,"
		 "\"access_token\":\"%s\",\"refresh_token\":\"refresh-\\u00fc-1\","
		 "\"token_type\":\"device\",\"username\":\"u\",\"api_url_v2\":\"https://x\","
		 "\"ota_url\":\"https://ota\",\"ota_force\":true,\"extra\":{\"a\":[1,2]}}",
		 pv->access_token);
	app_send_enc(msg);
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	CHECK(mg_setup_store_complete() && mg_setup_store_wifi_skipped());
	CHECK(!mg_setup_advertise_link());
	CHECK(host_settings_len("mg/setup/at") == (int)strlen(pv->access_token));
	CHECK(host_settings_len("mg/setup/rt") == (int)strlen("refresh-\xc3\xbc-1"));
	CHECK(host_settings_get("mg/setup/rt") &&
	      memcmp(host_settings_get("mg/setup/rt"), "refresh-\xc3\xbc-1", 11) == 0);
	CHECK(host_settings_len("mg/setup/k") == 32);
	CHECK(host_settings_len("mg/setup/done") > 0);
	uint8_t k[32];

	CHECK(mg_setup_store_get_k(k) == 0);
	CHECK_MEM(k, pv->K, 32);
	CHECK(host_disconnects == 0);

	/* Provisioning again on this session: setup is done. */
	app_send_enc(msg);
	CHECK(status_is(app_recv_enc(), "error_pairing_unavailable"));

	/* A new connection: hello is refused (plaintext, no session). */
	mg_setup_disconnected();
	connect();
	hello();
	CHECK_STR(next_msg(), "error_pairing_unavailable");

	/* A reboot keeps it. */
	boot();
	CHECK(mg_setup_store_complete());
	CHECK(mg_setup_store_get_k(k) == 0);
	CHECK_MEM(k, pv->K, 32);
}

static void test_rollback(void)
{
	printf("  storage failure rolls back\n");
	for (int fail = 1; fail <= 4; fail++) {
		fresh();
		handshake();
		host_settings_fail_save(fail);
		app_send_enc("{\"action\":\"provision_v2\",\"access_token\":\"acc\","
			     "\"refresh_token\":\"ref\",\"token_type\":\"device\"}");
		CHECK(status_is(app_recv_enc(), "error_storage"));
		CHECK(no_more());
		CHECK(!mg_setup_store_complete());
		CHECK(host_settings_count() == 0);
		host_advance(600);
		CHECK(host_disconnects == 1);
		boot();
		CHECK(!mg_setup_store_complete() && mg_setup_advertise_link());
	}
	/* A token too long to store: error_storage, nothing written. */
	static char big[2400], msg[2600];

	fresh();
	handshake();
	memset(big, 'a', 2049);
	big[2049] = 0;
	snprintf(msg, sizeof(msg),
		 "{\"action\":\"provision_v2\",\"access_token\":\"%s\",\"refresh_token\":\"r\","
		 "\"token_type\":\"device\"}",
		 big);
	app_send_enc(msg);
	CHECK(status_is(app_recv_enc(), "error_storage"));
	CHECK(host_settings_count() == 0);
}

static void test_boot_recovery(void)
{
	printf("  boot recovery\n");
	host_settings_clear();
	/* Tokens without the commit record: a provisioning cut short. */
	settings_save_one("mg/setup/at", "acc", 3);
	settings_save_one("mg/setup/rt", "ref", 3);
	boot();
	CHECK(!mg_setup_store_complete());
	CHECK(host_settings_count() == 0);

	/* A record whose lengths don't match. */
	uint8_t k[32] = {1};

	CHECK(mg_setup_store_commit("acc", 3, "ref", 3, k) == 0);
	settings_save_one("mg/setup/rt", "refresh", 7);
	boot();
	CHECK(!mg_setup_store_complete() && host_settings_count() == 0);

	/* Reset cut short after the record went: the rest is erased. */
	CHECK(mg_setup_store_commit("acc", 3, "ref", 3, k) == 0);
	settings_delete("mg/setup/done");
	boot();
	CHECK(!mg_setup_store_complete() && host_settings_count() == 0);

	/* Whole: kept. */
	CHECK(mg_setup_store_commit("acc", 3, "ref", 3, k) == 0);
	boot();
	CHECK(mg_setup_store_complete() && mg_setup_store_wifi_skipped());
	CHECK(mg_setup_store_erase() == 0 && host_settings_count() == 0);
	CHECK(!mg_setup_store_complete());
	CHECK(mg_setup_store_get_k(k) == -ENOENT);
}

static void test_timeouts(void)
{
	printf("  timeouts\n");
	/* client_finished after 60 s: the session is gone. */
	fresh();
	uint8_t dpriv[32], dnonce[16];
	size_t n;

	memcpy(dpriv, V->device_priv, 32);
	mg_b64url_decode(V->device_nonce, strlen(V->device_nonce), dnonce, 16, &n);
	mg_setup_test_set_ephemeral(dpriv, dnonce);
	hello();
	CHECK(next_msg() != NULL);
	host_advance(MG_SETUP_CLIENT_FINISHED_MS + 1);
	char env[300];

	snprintf(env, sizeof(env),
		 "{\"action\":\"pairing_encrypted\",\"session_id\":\"%s\",\"counter\":\"0\","
		 "\"ciphertext\":\"%s\",\"tag\":\"%s\"}",
		 V->session_id, V->cf_ct, V->cf_tag);
	app_send(env);
	CHECK(no_more()); /* error_pairing_decrypt, but plaintext is blocked now */
	host_advance(400);
	CHECK(host_disconnects == 1);

	/* No press within 60 s: pairing_confirm_timeout, then a disconnect. */
	fresh();
	mg_setup_test_set_ephemeral(dpriv, dnonce);
	hello();
	(void)next_msg();
	app_send(env);
	app_ctr = 1;
	CHECK(status_is(app_recv_enc(), "confirm_required"));
	host_advance(MG_SETUP_CONFIRM_MS - 10);
	CHECK(no_more());
	host_advance(20);
	CHECK(status_is(app_recv_enc(), "pairing_confirm_timeout"));
	host_advance(200);
	CHECK(host_disconnects == 1);
	CHECK(mg_setup_button() == false); /* the session is gone */

	/* Confirmed but no provision_v2 within 120 s. */
	fresh();
	handshake();
	host_advance(MG_SETUP_CONFIRMED_IDLE_MS + 10);
	CHECK(status_is(app_recv_enc(), "pairing_confirm_timeout"));
	host_advance(200);
	CHECK(host_disconnects == 1);

	/* A replayed record (old counter) drops the session. */
	fresh();
	handshake();
	app_ctr = 0;
	app_send_enc("{\"action\":\"wifi_scan\"}");
	CHECK(no_more());
	host_advance(400);
	CHECK(host_disconnects == 1);

	/* Disconnecting ends the session: the next connection starts over. */
	fresh();
	handshake();
	mg_setup_disconnected();
	connect();
	app_send_enc("{\"action\":\"wifi_scan\"}");
	CHECK_STR(next_msg(), "error_pairing_decrypt");
}

static void test_sensitive_before_confirm(void)
{
	printf("  sensitive actions need the press\n");
	fresh();
	uint8_t dpriv[32], dnonce[16];
	size_t n;
	char env[300];

	memcpy(dpriv, V->device_priv, 32);
	mg_b64url_decode(V->device_nonce, strlen(V->device_nonce), dnonce, 16, &n);
	mg_setup_test_set_ephemeral(dpriv, dnonce);
	hello();
	(void)next_msg();
	snprintf(env, sizeof(env),
		 "{\"action\":\"pairing_encrypted\",\"session_id\":\"%s\",\"counter\":\"0\","
		 "\"ciphertext\":\"%s\",\"tag\":\"%s\"}",
		 V->session_id, V->cf_ct, V->cf_tag);
	app_send(env);
	app_ctr = 1;
	CHECK(status_is(app_recv_enc(), "confirm_required"));
	app_send_enc("{\"action\":\"provision_v2\",\"access_token\":\"a\",\"refresh_token\":\"r\","
		     "\"token_type\":\"device\"}");
	CHECK(status_is(app_recv_enc(), "error_pairing_confirm_required"));
	CHECK(host_settings_count() == 0);
	CHECK(mg_setup_button());
	CHECK(status_is(app_recv_enc(), "pairing_confirmed"));
	app_send_enc("{\"action\":\"wifi_scan\"}");
	char *r = app_recv_enc();

	CHECK(r && strstr(r, "wifi_scan_result") && strstr(r, "\"networks\":[]"));
}

/* Token proof (mgcommands.h) against mg-token-proof-v1.json, through the
 * command handler. */
static void test_token_proof(void)
{
	printf("  token proof\n");
	for (int i = 0; i < PROOF_VEC_COUNT; i++) {
		const struct proof_vec *pv = &proof_vecs[i];
		uint8_t k[32], mac[32];

		printf("    vector %s\n", pv->name);
		CHECK(mg_token_proof_derive_key(pv->access_token, strlen(pv->access_token),
						pv->node_id, k) == 0);
		CHECK_MEM(k, pv->K, 32);
		CHECK(mg_token_proof_mac(k, true, pv->client_nonce, pv->device_nonce, mac) == 0);
		CHECK_MEM(mac, pv->device_mac, 32);
		CHECK(mg_token_proof_mac(k, false, pv->client_nonce, pv->device_nonce, mac) == 0);
		CHECK_MEM(mac, pv->client_mac, 32);

		/* The exchange, frame for frame. */
		host_settings_clear();
		boot();
		CHECK(mg_setup_store_commit(pv->access_token, strlen(pv->access_token), "r", 1, k) ==
		      0);
		host_notes_clear();
		mg_token_proof_connection_reset();
		mg_token_proof_test_set_nonce(pv->device_nonce);
		mg_token_proof_rx(&pv->challenge.b[1], pv->challenge.n - 1);
		CHECK(host_nnotes == 1 && host_notes[0].ch == MG_ATT_CONTROL &&
		      host_notes[0].len == pv->response.n &&
		      memcmp(host_notes[0].data, pv->response.b, pv->response.n) == 0);
		mg_token_proof_rx(&pv->confirm.b[1], pv->confirm.n - 1);
		CHECK(host_nnotes == 2 && host_notes[1].len == pv->result.n &&
		      memcmp(host_notes[1].data, pv->result.b, pv->result.n) == 0);
		CHECK(mg_token_proof_matched());
		/* clear: result 1 first, then everything is erased. */
		int refreshes = host_adv_refreshes;

		mg_token_proof_rx(&pv->clear.b[1], pv->clear.n - 1);
		CHECK(host_nnotes == 3 && host_notes[2].len == 3 && host_notes[2].data[2] == 1);
		CHECK(!mg_setup_store_complete() && host_settings_count() == 0);
		CHECK(mg_setup_advertise_link() && host_adv_refreshes > refreshes);
		CHECK(!mg_token_proof_matched());
	}

	const struct proof_vec *pv = &proof_vecs[0];
	uint8_t bad[33];

#define LAST_ERR(code)                                                                             \
	(host_nnotes > 0 && host_notes[host_nnotes - 1].data[0] == mg_command_error &&            \
	 host_notes[host_nnotes - 1].data[1] == mg_command_token_proof &&                          \
	 host_notes[host_nnotes - 1].data[3] == (code))

	/* No token: challenge is not_found. */
	host_settings_clear();
	boot();
	host_notes_clear();
	mg_token_proof_rx(&pv->challenge.b[1], pv->challenge.n - 1);
	CHECK(LAST_ERR(mg_error_code_not_found));
	/* clear before a match: proof_required, and nothing is erased. */
	CHECK(mg_setup_store_commit(pv->access_token, strlen(pv->access_token), "r", 1, pv->K) == 0);
	mg_token_proof_connection_reset();
	host_notes_clear();
	mg_token_proof_rx(&pv->clear.b[1], 1);
	CHECK(LAST_ERR(mg_error_code_proof_required));
	CHECK(mg_setup_store_complete());
	/* confirm without a pending response: invalid_value. */
	mg_token_proof_rx(&pv->confirm.b[1], pv->confirm.n - 1);
	CHECK(LAST_ERR(mg_error_code_invalid_value));
	/* Wrong lengths. */
	mg_token_proof_rx(&pv->challenge.b[1], pv->challenge.n - 2);
	CHECK(LAST_ERR(mg_error_code_invalid_length));
	mg_token_proof_rx((const uint8_t *)"", 0);
	CHECK(LAST_ERR(mg_error_code_invalid_length));
	/* Device -> client sub-commands and unknown ones: unsupported. */
	bad[0] = mg_token_proof_response;
	mg_token_proof_rx(bad, 1);
	CHECK(LAST_ERR(mg_error_code_unsupported));
	bad[0] = 9;
	mg_token_proof_rx(bad, 1);
	CHECK(LAST_ERR(mg_error_code_unsupported));
	/* A wrong client MAC: result 0, and clear stays refused. */
	mg_token_proof_test_set_nonce(pv->device_nonce);
	mg_token_proof_rx(&pv->challenge.b[1], pv->challenge.n - 1);
	memcpy(bad, &pv->confirm.b[1], 33);
	bad[32] ^= 1;
	host_notes_clear();
	mg_token_proof_rx(bad, 33);
	CHECK(host_nnotes == 1 && host_notes[0].data[0] == mg_command_token_proof &&
	      host_notes[0].data[1] == mg_token_proof_result && host_notes[0].data[2] == 0);
	CHECK(!mg_token_proof_matched());
	mg_token_proof_rx(&pv->confirm.b[1], pv->confirm.n - 1); /* nothing pending now */
	CHECK(LAST_ERR(mg_error_code_invalid_value));
	mg_token_proof_rx(&pv->clear.b[1], 1);
	CHECK(LAST_ERR(mg_error_code_proof_required));
	/* A match, then a new challenge: the exchange restarts (no clear until it
	 * matches again). */
	mg_token_proof_test_set_nonce(pv->device_nonce);
	mg_token_proof_rx(&pv->challenge.b[1], pv->challenge.n - 1);
	mg_token_proof_rx(&pv->confirm.b[1], pv->confirm.n - 1);
	CHECK(mg_token_proof_matched());
	mg_token_proof_rx(&pv->challenge.b[1], pv->challenge.n - 1);
	CHECK(!mg_token_proof_matched());
	/* Random device nonces differ, and the MAC follows them. */
	host_notes_clear();
	mg_token_proof_rx(&pv->challenge.b[1], pv->challenge.n - 1);
	mg_token_proof_rx(&pv->challenge.b[1], pv->challenge.n - 1);
	CHECK(host_nnotes == 2 && memcmp(&host_notes[0].data[2], &host_notes[1].data[2], 16) != 0);
	uint8_t mac[32];

	mg_token_proof_mac(pv->K, true, pv->client_nonce, &host_notes[1].data[2], mac);
	CHECK_MEM(mac, &host_notes[1].data[18], 32);
	/* The connection ends: no match carries over. */
	mg_token_proof_rx(&pv->confirm.b[1], pv->confirm.n - 1); /* result 0 (other nonce) */
	mg_token_proof_connection_reset();
	host_notes_clear();
	mg_token_proof_rx(&pv->clear.b[1], 1);
	CHECK(LAST_ERR(mg_error_code_proof_required));
}

static void test_reset(void)
{
	printf("  factory reset\n");
	fresh();
	handshake();
	app_send_enc("{\"action\":\"provision_v2\",\"access_token\":\"acc\",\"refresh_token\":\"ref\","
		     "\"token_type\":\"device\"}");
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	CHECK(mg_setup_factory_reset("test") == 0);
	CHECK(!mg_setup_store_complete() && host_settings_count() == 0 && mg_setup_advertise_link());
	/* Set up again from scratch on a new connection. */
	mg_setup_disconnected();
	connect();
	handshake();
	app_send_enc("{\"action\":\"provision_v2\",\"access_token\":\"acc2\",\"refresh_token\":"
		     "\"ref2\",\"token_type\":\"device\"}");
	CHECK(status_is(app_recv_enc(), "auth_ok"));
	CHECK(host_settings_len("mg/setup/at") == 4);
}

static void test_small_mtu(void)
{
	printf("  default MTU (23): chunks of 17 bytes\n");
	host_mtu = 23;
	fresh();
	handshake();
	for (int i = 0; i < host_nnotes; i++) {
		CHECK(host_notes[i].len <= 20);
	}
	host_mtu = 185;
}

static void setup_all(void)
{
	CHECK(mg_psa_init() == 0);
	V = &link_vecs[0];
	CHECK_STR(V->name, "community_v5");
	memcpy(dev_tx, V->mobile_rx_key, 32);
	memcpy(dev_rx, V->mobile_tx_key, 32);
	host_log_verbose = getenv("HOST_VERBOSE") != NULL;
}

HOST_MAIN(setup_all, test_device_info, test_plaintext_rules, test_provision, test_rollback,
	  test_boot_recovery, test_timeouts, test_sensitive_before_confirm, test_token_proof,
	  test_reset, test_small_mtu)
