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

#include "mg_session.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "mg_core.h"
#include "mg_led.h"
#include "mg_throughput.h"
#include "mgcommands.h"

#if defined(CONFIG_MG_SECURE)
#include "mg_crypto.h"
#include "mg_pairing.h"
#include "mgcommands-secure.h"
#endif

LOG_MODULE_REGISTER(mg_session, CONFIG_MG_LOG_LEVEL);

/* Serializes encryption and queueing so seq order matches queue order. */
static K_MUTEX_DEFINE(tx_lock);

static struct {
	bool connected;
	bool sub[MG_ATT_CHAN_COUNT];
	bool ready;
} s;

static void update_ready(void);

#if !defined(CONFIG_MG_SECURE)

/* ------------------------------------------------------------------------
 * Without session security: Control and Data only.
 * ---------------------------------------------------------------------- */

void mg_session_init(bool boot_pairing_request)
{
	ARG_UNUSED(boot_pairing_request);
}

static bool authorized(void)
{
	return s.connected;
}

bool mg_session_authenticated(void)
{
	return authorized();
}

void mg_session_rx(enum mg_att_chan ch, const uint8_t *buf, size_t len)
{
	if (ch == MG_ATT_CONTROL && len > 0) {
		mg_core_control_rx(buf, len);
	} else if (ch == MG_ATT_DATA && s.connected) {
		/* Client Data is only the receive throughput test's. */
		mg_tp_data_rx(buf, len);
	}
}

bool mg_session_button(void)
{
	return false;
}

int mg_session_send_control(const uint8_t *buf, size_t len)
{
	k_mutex_lock(&tx_lock, K_FOREVER);
	int err = mg_transport_notify(MG_ATT_CONTROL, buf, len, K_MSEC(500));

	k_mutex_unlock(&tx_lock);
	return err;
}

int mg_session_send_data(const uint8_t *buf, size_t len, k_timeout_t wait)
{
	k_mutex_lock(&tx_lock, K_FOREVER);
	int err = mg_transport_notify(MG_ATT_DATA, buf, len, wait);

	k_mutex_unlock(&tx_lock);
	return err;
}

size_t mg_session_data_room(void)
{
	uint16_t mtu = mg_transport_att_mtu();

	return mtu > 3 ? mtu - 3 : 0;
}

size_t mg_session_feature_cmds(uint8_t *out)
{
	ARG_UNUSED(out);
	return 0;
}

void mg_session_send_feature_lists(void)
{
}

bool mg_session_pairing_mode(void)
{
	return false;
}

bool mg_session_pair_pending(void)
{
	return false;
}

static void secure_reset(void)
{
}

#else /* CONFIG_MG_SECURE */

/* ------------------------------------------------------------------------
 * mgcommands-secure.h
 * ---------------------------------------------------------------------- */

#define M1_LEN (4 + MG_HASH_SIZE) /* minimum; trailing bytes are hashed too */
#define M2_LEN (4 + MG_PUBLIC_KEY_SIZE + MG_NONCE_SIZE)
#define M3_LEN (2 + MG_PUBLIC_KEY_SIZE + MG_NONCE_SIZE)
#define MAX_PT 244

enum kx_state {
	KX_IDLE,
	KX_COMMITTED,
	KX_DONE,
	KX_FAILED,
};

enum { CTL, DAT };

static struct {
	enum kx_state kx;
	bool encrypted;
	bool authenticated;
	bool kx_in_pairing_mode;
	uint8_t m1[MAX_PT];
	size_t m1_len;
	uint8_t m2[M2_LEN];
	struct mg_x25519 eph;
	struct mg_session_keys keys;
	struct mg_aead rx[2]; /* client -> device */
	struct mg_aead tx[2]; /* device -> client */
	uint32_t tx_seq[2];
	int64_t rx_last[2];
	bool pair_pending;
	bool dev_confirmed;
	bool cli_confirmed;
	uint8_t auth_failures;
	bool has_key_id;
	uint8_t key_id[MG_KEY_ID_SIZE];
} x;

static bool pairing_mode;
static uint8_t pairing_attempts;
static struct k_work_delayable pair_timeout;

static bool test_eph;
static uint8_t test_priv[32];
static uint8_t test_nonce[16];

void mg_session_test_set_ephemeral(const uint8_t priv[32], const uint8_t nonce[16])
{
	memcpy(test_priv, priv, 32);
	memcpy(test_nonce, nonce, 16);
	test_eph = true;
}

bool mg_session_pairing_mode(void)
{
	return pairing_mode;
}

bool mg_session_pair_pending(void)
{
	return x.pair_pending;
}

static void set_pairing_mode(bool on)
{
	pairing_mode = on;
	pairing_attempts = 0;
	mg_led_set(MG_LED_PAIRING_MODE, on);
	LOG_INF("pairing mode %s", on ? "on" : "off");
}

static bool authorized(void)
{
	return s.connected && x.authenticated;
}

bool mg_session_authenticated(void)
{
	return authorized();
}

static void secure_reset(void)
{
	(void)k_work_cancel_delayable(&pair_timeout);
	mg_x25519_destroy(&x.eph);
	for (int i = 0; i < 2; i++) {
		mg_aead_destroy(&x.rx[i]);
		mg_aead_destroy(&x.tx[i]);
	}
	k_mutex_lock(&tx_lock, K_FOREVER);
	memset(&x, 0, sizeof(x));
	x.rx_last[CTL] = -1;
	x.rx_last[DAT] = -1;
	k_mutex_unlock(&tx_lock);
	mg_led_set(MG_LED_PAIR_PENDING, false);
}

/* Plaintext Control, used before encryption and for decrypt_failed. */
static int send_plain(const uint8_t *buf, size_t len)
{
	return mg_transport_notify(MG_ATT_CONTROL, buf, len, K_MSEC(500));
}

static int send_sealed(int which, const uint8_t *buf, size_t len, k_timeout_t wait)
{
	uint8_t frame[MAX_PT + MG_FRAME_OVERHEAD];

	if (len > MAX_PT) {
		return -EMSGSIZE;
	}
	if (x.tx_seq[which] == UINT32_MAX) {
		/* A sender that would wrap disconnects. */
		mg_transport_disconnect();
		return -ENOTCONN;
	}
	int n = mg_frame_seal(&x.tx[which], x.tx_seq[which], buf, len, frame);

	if (n < 0) {
		return n;
	}
	x.tx_seq[which]++;
	return mg_transport_notify(which == CTL ? MG_ATT_ENC_CONTROL : MG_ATT_ENC_DATA, frame, n,
				   wait);
}

int mg_session_send_control(const uint8_t *buf, size_t len)
{
	int err;

	k_mutex_lock(&tx_lock, K_FOREVER);
	if (x.encrypted) {
		err = send_sealed(CTL, buf, len, K_MSEC(500));
	} else {
		err = send_plain(buf, len);
	}
	k_mutex_unlock(&tx_lock);
	return err;
}

int mg_session_send_data(const uint8_t *buf, size_t len, k_timeout_t wait)
{
	int err;

	k_mutex_lock(&tx_lock, K_FOREVER);
	err = x.authenticated ? send_sealed(DAT, buf, len, wait) : -EACCES;
	k_mutex_unlock(&tx_lock);
	return err;
}

size_t mg_session_data_room(void)
{
	int room = (int)mg_transport_att_mtu() - 3 - MG_FRAME_OVERHEAD;

	return room > 0 ? MIN((size_t)room, (size_t)MAX_PT) : 0;
}

size_t mg_session_feature_cmds(uint8_t *out)
{
	out[0] = mg_command_key_exchange;
	out[1] = mg_command_enable_encryption;
	out[2] = mg_command_authenticate;
	return 3;
}

void mg_session_send_feature_lists(void)
{
	const uint8_t suites[] = {mg_command_supported_features, mg_command_sub_feature,
				  mg_command_key_exchange,
				  mg_crypto_suite_x25519_aes256gcm_sha256};
	const uint8_t methods[] = {mg_command_supported_features, mg_command_sub_feature,
				   mg_command_authenticate, mg_pairing_method_physical_confirm};

	mg_session_send_control(suites, sizeof(suites));
	mg_session_send_control(methods, sizeof(methods));
}

static void kx_fail(uint16_t code)
{
	x.kx = KX_FAILED;
	mg_x25519_destroy(&x.eph);
	mg_core_send_error(mg_command_key_exchange, code, NULL);
}

static void on_commit(const uint8_t *buf, size_t len)
{
	if (x.kx != KX_IDLE) {
		kx_fail(mg_error_code_key_exchange_failed);
		return;
	}
	if (len < M1_LEN || len > MAX_PT) {
		kx_fail(mg_error_code_key_exchange_failed);
		return;
	}
	if (buf[2] != MG_SECURITY_VERSION) {
		mg_core_send_error(mg_command_key_exchange, mg_error_code_unsupported_version, NULL);
		return;
	}
	if (buf[3] != mg_crypto_suite_x25519_aes256gcm_sha256) {
		mg_core_send_error(mg_command_key_exchange, mg_error_code_unsupported_suite, NULL);
		return;
	}
	/* The transcript hashes M1 as sent, trailing bytes included. */
	memcpy(x.m1, buf, len);
	x.m1_len = len;

	uint8_t *m2 = x.m2;
	int err;

	if (test_eph) {
		err = mg_x25519_import(&x.eph, test_priv);
		memcpy(&m2[4 + MG_PUBLIC_KEY_SIZE], test_nonce, MG_NONCE_SIZE);
		test_eph = false;
	} else {
		err = mg_x25519_generate(&x.eph);
		err = err ?: mg_random(&m2[4 + MG_PUBLIC_KEY_SIZE], MG_NONCE_SIZE);
	}
	if (err) {
		kx_fail(mg_error_code_key_exchange_failed);
		return;
	}
	m2[0] = mg_command_key_exchange;
	m2[1] = mg_key_exchange_command_response;
	m2[2] = MG_SECURITY_VERSION;
	m2[3] = mg_crypto_suite_x25519_aes256gcm_sha256;
	memcpy(&m2[4], x.eph.pub, MG_PUBLIC_KEY_SIZE);
	x.kx = KX_COMMITTED;
	send_plain(m2, M2_LEN);
}

static void on_reveal(const uint8_t *buf, size_t len)
{
	uint8_t digest[MG_HASH_SIZE];
	uint8_t ss[32];

	if (x.kx != KX_COMMITTED || len < M3_LEN) {
		kx_fail(mg_error_code_key_exchange_failed);
		return;
	}
	const uint8_t *parts[] = {&buf[2]};
	const size_t lens[] = {MG_PUBLIC_KEY_SIZE + MG_NONCE_SIZE};

	if (mg_sha256(parts, lens, 1, digest) || !mg_ct_equal(digest, &x.m1[4], MG_HASH_SIZE)) {
		LOG_WRN("key exchange: commitment mismatch");
		kx_fail(mg_error_code_key_exchange_failed);
		return;
	}
	if (mg_x25519_shared(&x.eph, &buf[2], ss) ||
	    mg_derive(x.m1, x.m1_len, x.m2, M2_LEN, buf, len, ss, &x.keys) ||
	    mg_aead_setup(&x.rx[CTL], x.keys.c2d_control) ||
	    mg_aead_setup(&x.rx[DAT], x.keys.c2d_data) ||
	    mg_aead_setup(&x.tx[CTL], x.keys.d2c_control) ||
	    mg_aead_setup(&x.tx[DAT], x.keys.d2c_data)) {
		memset(ss, 0, sizeof(ss));
		kx_fail(mg_error_code_key_exchange_failed);
		return;
	}
	memset(ss, 0, sizeof(ss));
	mg_x25519_destroy(&x.eph);
	x.kx = KX_DONE;

	/* Each key exchange completed in pairing mode is one attempt. */
	if (pairing_mode) {
		x.kx_in_pairing_mode = true;
		if (++pairing_attempts >= MG_PAIRING_MAX_ATTEMPTS) {
			pairing_mode = false;
			mg_led_set(MG_LED_PAIRING_MODE, false);
			LOG_INF("pairing mode off after %d attempts", MG_PAIRING_MAX_ATTEMPTS);
		}
	}
	LOG_INF("key exchange done");
}

static void authenticated(void)
{
	static const uint8_t secured[] = {mg_command_connection_secured};

	x.authenticated = true;
	mg_session_send_control(secured, sizeof(secured));
	LOG_INF("session authenticated");
	update_ready();
}

static void try_complete_pairing(void)
{
	uint8_t msg[2 + MG_KEY_ID_SIZE];

	if (!x.pair_pending || !x.dev_confirmed || !x.cli_confirmed) {
		return;
	}
	(void)k_work_cancel_delayable(&pair_timeout);
	x.pair_pending = false;
	mg_led_set(MG_LED_PAIR_PENDING, false);
	if (mg_pairing_add(x.keys.key_id, x.keys.pairing_key)) {
		LOG_ERR("storing the pairing failed");
	}
	memcpy(x.key_id, x.keys.key_id, MG_KEY_ID_SIZE);
	x.has_key_id = true;
	set_pairing_mode(false);
	msg[0] = mg_command_authenticate;
	msg[1] = mg_authenticate_command_pair_complete;
	memcpy(&msg[2], x.keys.key_id, MG_KEY_ID_SIZE);
	mg_session_send_control(msg, sizeof(msg));
	authenticated();
}

static void pair_timeout_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (!x.pair_pending) {
		return;
	}
	x.pair_pending = false;
	mg_led_set(MG_LED_PAIR_PENDING, false);
	LOG_INF("pairing timed out");
	mg_core_send_error(mg_command_authenticate, mg_error_code_pairing_rejected, NULL);
}

static void on_prove(const uint8_t *buf, size_t len)
{
	uint8_t pk[32];
	uint8_t mac[MG_HASH_SIZE];
	uint8_t reply[2 + MG_HASH_SIZE];

	if (len != 2 + MG_KEY_ID_SIZE + MG_HASH_SIZE) {
		mg_core_send_error(mg_command_authenticate, mg_error_code_invalid_length, NULL);
		return;
	}
	bool ok = mg_pairing_find(&buf[2], pk) == 0 &&
		  mg_auth_mac(pk, "mg1 client auth", x.keys.th, mac) == 0 &&
		  mg_ct_equal(mac, &buf[2 + MG_KEY_ID_SIZE], MG_HASH_SIZE);

	if (!ok) {
		memset(pk, 0, sizeof(pk));
		mg_core_send_error(mg_command_authenticate, mg_error_code_auth_failed, NULL);
		if (++x.auth_failures >= MG_AUTH_MAX_FAILURES) {
			LOG_WRN("too many failed proves, disconnecting");
			mg_transport_disconnect();
		}
		return;
	}
	reply[0] = mg_command_authenticate;
	reply[1] = mg_authenticate_command_proof;
	if (mg_auth_mac(pk, "mg1 device auth", x.keys.th, &reply[2])) {
		memset(pk, 0, sizeof(pk));
		return;
	}
	memset(pk, 0, sizeof(pk));
	memcpy(x.key_id, &buf[2], MG_KEY_ID_SIZE);
	x.has_key_id = true;
	mg_session_send_control(reply, sizeof(reply));
	authenticated();
}

static void on_authenticate(const uint8_t *buf, size_t len)
{
	if (len < 2) {
		mg_core_send_error(mg_command_authenticate, mg_error_code_invalid_length, NULL);
		return;
	}
	uint8_t sub = buf[1];

	if (x.authenticated && sub != mg_authenticate_command_unpair) {
		/* Authentication runs once per connection. */
		mg_core_send_error(mg_command_authenticate,
				   sub == mg_authenticate_command_prove
					   ? mg_error_code_auth_failed
					   : mg_error_code_pairing_not_allowed,
				   NULL);
		return;
	}
	switch (sub) {
	case mg_authenticate_command_prove:
		on_prove(buf, len);
		break;
	case mg_authenticate_command_pair_request: {
		if (len < 3) {
			mg_core_send_error(mg_command_authenticate, mg_error_code_invalid_length,
					   NULL);
			return;
		}
		if (buf[2] != mg_pairing_method_physical_confirm || !x.kx_in_pairing_mode) {
			mg_core_send_error(mg_command_authenticate,
					   mg_error_code_pairing_not_allowed, NULL);
			return;
		}
		if (x.pair_pending) {
			return;
		}
		const uint8_t pending[] = {mg_command_authenticate,
					   mg_authenticate_command_pair_pending,
					   mg_pairing_method_physical_confirm};

		x.pair_pending = true;
		x.dev_confirmed = false;
		x.cli_confirmed = false;
		mg_led_set(MG_LED_PAIR_PENDING, true);
		k_work_reschedule_for_queue(mg_app_wq(), &pair_timeout,
					    K_SECONDS(MG_PAIRING_TIMEOUT_S));
		mg_session_send_control(pending, sizeof(pending));
		LOG_INF("pairing: press the button to accept");
		break;
	}
	case mg_authenticate_command_pair_confirm:
		if (!x.pair_pending) {
			mg_core_send_error(mg_command_authenticate, mg_error_code_pairing_rejected,
					   NULL);
			return;
		}
		x.cli_confirmed = true;
		try_complete_pairing();
		break;
	case mg_authenticate_command_unpair: {
		if (!x.authenticated) {
			mg_core_send_error(mg_command_authenticate,
					   mg_error_code_authentication_required, NULL);
			return;
		}
		const uint8_t echo[] = {mg_command_authenticate, mg_authenticate_command_unpair};

		if (x.has_key_id) {
			(void)mg_pairing_remove(x.key_id);
			x.has_key_id = false;
		}
		if (mg_pairing_count() == 0) {
			set_pairing_mode(true);
		}
		mg_session_send_control(echo, sizeof(echo));
		break;
	}
	default:
		mg_core_send_error(mg_command_authenticate, mg_error_code_unsupported, NULL);
		break;
	}
}

bool mg_session_button(void)
{
	if (!x.pair_pending) {
		return false;
	}
	x.dev_confirmed = true;
	LOG_INF("pairing accepted on the device");
	try_complete_pairing();
	return true;
}

static void drop_session(void)
{
	const uint8_t err[] = {mg_command_error, mg_command_enable_encryption, mg_error_data_code,
			       mg_error_code_decrypt_failed & 0xff,
			       mg_error_code_decrypt_failed >> 8};

	LOG_WRN("frame failed to decrypt, dropping the session");
	k_mutex_lock(&tx_lock, K_FOREVER);
	send_plain(err, sizeof(err));
	k_mutex_unlock(&tx_lock);
	mg_transport_disconnect();
}

/* Opens one encrypted frame; returns plaintext length or <0 (session dropped). */
static int open_frame(int which, const uint8_t *buf, size_t len, uint8_t *pt)
{
	uint32_t seq;

	if (x.kx != KX_DONE || len > MAX_PT + MG_FRAME_OVERHEAD) {
		drop_session();
		return -EBADMSG;
	}
	int n = mg_frame_open(&x.rx[which], buf, len, &seq, pt);

	if (n < 0 || (int64_t)seq <= x.rx_last[which]) {
		drop_session();
		return -EBADMSG;
	}
	x.rx_last[which] = seq;
	return n;
}

static void rx_plain_control(const uint8_t *buf, size_t len)
{
	if (x.encrypted) {
		return; /* ignored once encryption is on */
	}
	switch (buf[0]) {
	case mg_command_key_exchange:
		if (len < 2) {
			kx_fail(mg_error_code_key_exchange_failed);
		} else if (buf[1] == mg_key_exchange_command_commit) {
			on_commit(buf, len);
		} else if (buf[1] == mg_key_exchange_command_reveal) {
			on_reveal(buf, len);
		} else {
			mg_core_send_error(mg_command_key_exchange, mg_error_code_unsupported, NULL);
		}
		break;
	case mg_command_request_status:
		mg_core_control_rx(buf, len);
		break;
	default:
		mg_core_send_error(buf[0], mg_error_code_encryption_required, NULL);
		break;
	}
}

static void rx_enc_control(const uint8_t *buf, size_t len)
{
	uint8_t pt[MAX_PT];
	int n = open_frame(CTL, buf, len, pt);

	if (n <= 0) {
		return;
	}
	if (!x.encrypted) {
		if (pt[0] == mg_command_enable_encryption) {
			const uint8_t echo[] = {mg_command_enable_encryption};

			x.encrypted = true;
			mg_session_send_control(echo, sizeof(echo));
			LOG_INF("encryption on");
		} else {
			mg_core_send_error(pt[0], mg_error_code_encryption_required, NULL);
		}
		return;
	}
	switch (pt[0]) {
	case mg_command_enable_encryption:
		break; /* already on */
	case mg_command_key_exchange:
		mg_core_send_error(pt[0], mg_error_code_key_exchange_failed, NULL);
		break;
	case mg_command_authenticate:
		on_authenticate(pt, n);
		break;
	case mg_command_request_status:
		mg_core_control_rx(pt, n);
		break;
	default:
		if (x.authenticated) {
			mg_core_control_rx(pt, n);
		} else {
			mg_core_send_error(pt[0], mg_error_code_authentication_required, NULL);
		}
		break;
	}
}

void mg_session_rx(enum mg_att_chan ch, const uint8_t *buf, size_t len)
{
	uint8_t pt[MAX_PT];

	if (!s.connected || len == 0) {
		return;
	}
	switch (ch) {
	case MG_ATT_CONTROL:
		rx_plain_control(buf, len);
		break;
	case MG_ATT_ENC_CONTROL:
		rx_enc_control(buf, len);
		break;
	case MG_ATT_ENC_DATA: {
		/* Client Data is only the receive throughput test's; every frame counts
		 * towards the replay window all the same. */
		int n = open_frame(DAT, buf, len, pt);

		if (n > 0 && authorized()) {
			mg_tp_data_rx(pt, n);
		}
		break;
	}
	default:
		break;
	}
}

void mg_session_init(bool boot_pairing_request)
{
	k_work_init_delayable(&pair_timeout, pair_timeout_fn);
	if (mg_crypto_init()) {
		LOG_ERR("PSA crypto init failed");
	}
	(void)mg_pairing_load();
	secure_reset();
	set_pairing_mode(boot_pairing_request || mg_pairing_count() == 0);
}

#endif /* CONFIG_MG_SECURE */

static void update_ready(void)
{
	enum mg_att_chan ctl = IS_ENABLED(CONFIG_MG_SECURE) ? MG_ATT_ENC_CONTROL : MG_ATT_CONTROL;
	enum mg_att_chan dat = IS_ENABLED(CONFIG_MG_SECURE) ? MG_ATT_ENC_DATA : MG_ATT_DATA;
	/* Ready for gestures and audio: Control and Data both subscribed. */
	bool ready = authorized() && s.sub[ctl] && s.sub[dat];

	if (ready != s.ready) {
		s.ready = ready;
		mg_core_set_ready(ready);
	}
}

void mg_session_connected(void)
{
	secure_reset();
	memset(&s, 0, sizeof(s));
	s.connected = true;
	mg_core_connected();
	update_ready();
}

void mg_session_disconnected(void)
{
	s.connected = false;
	update_ready();
	secure_reset();
	memset(&s, 0, sizeof(s));
	mg_core_disconnected();
}

void mg_session_subscribed(enum mg_att_chan ch, bool enabled)
{
	if (ch < MG_ATT_CHAN_COUNT) {
		s.sub[ch] = enabled;
		update_ready();
	}
}
