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
 * A test client for the gadget in BabbleSim: scans for MG_SERVICE_UUID,
 * connects, negotiates the MTU, subscribes and runs one scenario
 * (-testid=central_ptt, central_offline or central_setup) while the gadget
 * image runs the matching gadget_* hook. Audio is decoded with the vendored
 * SBC and LC3 decoders and checked for the synthetic 1 kHz tone.
 */

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include <lc3.h>
#include <sbc.h>

#include "babblekit/testcase.h"
#include "bstests.h"

#include "mg_json.h"
#include "mg_psa.h"
#include "mg_setup.h"
#include "mg_setup_crypto.h"
#include "mg_token_proof.h"
#include "mgcommands.h"
#include "pairing_transcript.h"

#define CHECK(cond, ...)                                                                           \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			TEST_FAIL(__VA_ARGS__);                                                    \
			return;                                                                    \
		}                                                                                  \
	} while (0)

/* CHECK for functions that return a value. */
#define CHECK_OR(ret, cond, ...)                                                                   \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			TEST_FAIL(__VA_ARGS__);                                                    \
			return ret;                                                                \
		}                                                                                  \
	} while (0)

enum { CH_CTRL, CH_DATA, CH_NUS, CH_SETUP, CH_SETUP_RX, CH_N };

struct note {
	uint8_t ch;
	uint16_t len;
	uint8_t d[244];
};

K_MSGQ_DEFINE(rxq, sizeof(struct note), 256, 4);

static struct bt_conn *conn;
static K_SEM_DEFINE(sem_conn, 0, 1);
static K_SEM_DEFINE(sem_disc, 0, 1);
static K_SEM_DEFINE(sem_done, 0, 1);
static K_SEM_DEFINE(sem_gone, 0, 1);
static K_SEM_DEFINE(sem_rd, 0, 1);
static bt_addr_le_t peer;
static char peer_name[32];
static uint16_t h[CH_N];
static uint16_t h_bas, h_dis_manuf, h_dis_model, h_dis_fw, h_dis_sw;
static uint32_t nus_bytes;

static const struct bt_uuid_128 u_svc = BT_UUID_INIT_128(MG_SERVICE_UUID_LE_BYTES);
static const struct bt_uuid_128 u_ctrl = BT_UUID_INIT_128(MG_CONTROL_UUID_LE_BYTES);
static const struct bt_uuid_128 u_data = BT_UUID_INIT_128(MG_DATA_UUID_LE_BYTES);
static const struct bt_uuid_128 u_nus_tx = BT_UUID_INIT_128(MG_NUS_TX_UUID_LE_BYTES);
static const struct bt_uuid_128 u_setup_tx = BT_UUID_INIT_128(MG_SETUP_TX_UUID_LE);
static const struct bt_uuid_128 u_setup_rx = BT_UUID_INIT_128(MG_SETUP_RX_UUID_LE);

/* ---------------------------------------------------------------- scan */

struct adv_seen {
	bool uuid;
	bool setup;
	bool name;
};

/* Which payloads the scan saw, and whether both came from one address. */
static bool want_both;
static int adv_mg, adv_setup;
static bool adv_other_addr;
static bt_addr_le_t adv_addr;

static bool parse_ad(struct bt_data *data, void *user)
{
	struct adv_seen *s = user;
	static const uint8_t want[] = {MG_SERVICE_UUID_LE_BYTES};
	static const uint8_t setup[] = {MG_SETUP_SERVICE_UUID_LE};

	if (data->type == BT_DATA_UUID128_ALL) {
		for (int i = 0; i + 16 <= data->data_len; i += 16) {
			if (memcmp(&data->data[i], want, 16) == 0) {
				s->uuid = true;
			}
			if (memcmp(&data->data[i], setup, 16) == 0) {
				s->setup = true;
			}
		}
	} else if (data->type == BT_DATA_NAME_COMPLETE) {
		size_t n = MIN(data->data_len, sizeof(peer_name) - 1);

		memcpy(peer_name, data->data, n);
		peer_name[n] = '\0';
		s->name = true;
	}
	return true;
}

static bool seen_uuid;

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad)
{
	struct adv_seen s = {0};

	ARG_UNUSED(rssi);
	bt_data_parse(ad, parse_ad, &s);
	if (s.uuid || s.setup) {
		if (adv_mg + adv_setup == 0) {
			bt_addr_le_copy(&adv_addr, addr);
		} else if (!bt_addr_le_eq(addr, &adv_addr)) {
			adv_other_addr = true;
		}
		adv_mg += s.uuid;
		adv_setup += s.setup;
	}
	if (s.uuid) {
		seen_uuid = true;
		bt_addr_le_copy(&peer, addr);
	}
	/* Connect once the scan response with the name has come in too (and,
	 * when asked, once both payloads have been seen). */
	if (seen_uuid && s.name && type == BT_GAP_ADV_TYPE_SCAN_RSP &&
	    bt_addr_le_eq(addr, &peer) && (!want_both || (adv_mg && adv_setup))) {
		bt_le_scan_stop();
		int err = bt_conn_le_create(&peer, BT_CONN_LE_CREATE_CONN,
					    BT_LE_CONN_PARAM(6, 12, 0, 400), &conn);
		if (err) {
			TEST_FAIL("create conn failed (%d)", err);
		}
	}
}

static void connected(struct bt_conn *c, uint8_t err)
{
	if (err) {
		TEST_FAIL("connection failed (0x%02x)", err);
		return;
	}
	k_sem_give(&sem_conn);
}

static void disconnected(struct bt_conn *c, uint8_t reason)
{
	ARG_UNUSED(reason);
	if (c == conn) {
		bt_conn_unref(conn);
		conn = NULL;
		k_sem_give(&sem_gone);
	}
}

/* The gadget's connection parameter requests, as a phone would see them. */
static volatile uint16_t cp_interval, cp_latency, cp_timeout;
static volatile int cp_updates, cp_requests;

static bool le_param_req(struct bt_conn *c, struct bt_le_conn_param *p)
{
	ARG_UNUSED(c);
	cp_requests++;
	TEST_PRINT("gadget asks for %u-%u x 1.25 ms, latency %u, timeout %u", p->interval_min,
		   p->interval_max, p->latency, p->timeout);
	return true; /* accepted, like a phone within Apple's rules */
}

static void le_param_updated(struct bt_conn *c, uint16_t interval, uint16_t latency,
			     uint16_t timeout)
{
	ARG_UNUSED(c);
	cp_interval = interval;
	cp_latency = latency;
	cp_timeout = timeout;
	cp_updates++;
}

BT_CONN_CB_DEFINE(conn_cbs) = {
	.connected = connected,
	.disconnected = disconnected,
	.le_param_req = le_param_req,
	.le_param_updated = le_param_updated,
};

/* The link the gadget asks for: 2M PHY, then 15 ms / latency 10 / 4 s. */
static bool check_link(void)
{
	struct bt_conn_info info;

	for (int i = 0; i < 40 && !(cp_interval == 12 && cp_latency == 10); i++) {
		k_sleep(K_MSEC(100));
	}
	CHECK_OR(false, cp_requests > 0, "the gadget asked for no connection parameters");
	CHECK_OR(false, cp_interval == 12 && cp_latency == 10 && cp_timeout == 400,
	      "connection %u x 1.25 ms, latency %u, timeout %u", cp_interval, cp_latency,
	      cp_timeout);
	CHECK_OR(false, bt_conn_get_info(conn, &info) == 0, "conn info");
	CHECK_OR(false, info.le.phy->tx_phy == BT_GAP_LE_PHY_2M && info.le.phy->rx_phy == BT_GAP_LE_PHY_2M,
	      "PHY tx %u rx %u", info.le.phy->tx_phy, info.le.phy->rx_phy);
	/* 251-byte PDUs this way too, as phones ask for (the gadget only sets its own
	 * direction). Asked once the gadget's own procedures are done. */
	for (int i = 0; i < 20 && info.le.data_len->tx_max_len < 251; i++) {
		(void)bt_conn_le_data_len_update(conn, BT_LE_DATA_LEN_PARAM_MAX);
		k_sleep(K_MSEC(100));
		(void)bt_conn_get_info(conn, &info);
	}
	CHECK_OR(false, info.le.data_len->tx_max_len == 251, "central TX data length %u",
		 info.le.data_len->tx_max_len);
	TEST_PRINT("link: 2M PHY, 15.00 ms, latency 10, timeout 4000 ms");
	return true;
}

/* ---------------------------------------------------------------- GATT */

static uint8_t discover_cb(struct bt_conn *c, const struct bt_gatt_attr *attr,
			   struct bt_gatt_discover_params *p)
{
	if (attr == NULL) {
		k_sem_give(&sem_disc);
		return BT_GATT_ITER_STOP;
	}
	const struct bt_gatt_chrc *chrc = attr->user_data;
	uint16_t v = chrc->value_handle;

	if (!bt_uuid_cmp(chrc->uuid, &u_ctrl.uuid)) {
		h[CH_CTRL] = v;
	} else if (!bt_uuid_cmp(chrc->uuid, &u_data.uuid)) {
		h[CH_DATA] = v;
	} else if (!bt_uuid_cmp(chrc->uuid, &u_nus_tx.uuid)) {
		h[CH_NUS] = v;
	} else if (!bt_uuid_cmp(chrc->uuid, &u_setup_tx.uuid)) {
		h[CH_SETUP] = v;
	} else if (!bt_uuid_cmp(chrc->uuid, &u_setup_rx.uuid)) {
		h[CH_SETUP_RX] = v;
	} else if (!bt_uuid_cmp(chrc->uuid, BT_UUID_DECLARE_16(MG_BAS_BATTERY_LEVEL_UUID16))) {
		h_bas = v;
	} else if (!bt_uuid_cmp(chrc->uuid, BT_UUID_DECLARE_16(MG_DIS_MANUFACTURER_NAME_UUID16))) {
		h_dis_manuf = v;
	} else if (!bt_uuid_cmp(chrc->uuid, BT_UUID_DECLARE_16(MG_DIS_MODEL_NUMBER_UUID16))) {
		h_dis_model = v;
	} else if (!bt_uuid_cmp(chrc->uuid, BT_UUID_DECLARE_16(MG_DIS_FIRMWARE_REVISION_UUID16))) {
		h_dis_fw = v;
	} else if (!bt_uuid_cmp(chrc->uuid, BT_UUID_DECLARE_16(MG_DIS_SOFTWARE_REVISION_UUID16))) {
		h_dis_sw = v;
	}
	return BT_GATT_ITER_CONTINUE;
}

/* The throughput test's stream, counted instead of queued. */
static volatile bool tp_counting;
static uint32_t tp_bytes, tp_pkts, tp_gaps, tp_next;

static void tp_account(const uint8_t *d, uint16_t len)
{
	if (len < 4) {
		return;
	}
	uint32_t seq = sys_get_le32(d);

	if (seq > tp_next) {
		tp_gaps += seq - tp_next;
	}
	for (uint16_t i = 4; i < len; i++) {
		if (d[i] != (uint8_t)(seq + i)) {
			TEST_FAIL("throughput packet %u corrupt at byte %u", seq, i);
			break;
		}
	}
	tp_next = seq + 1;
	tp_bytes += len;
	tp_pkts++;
}

static uint8_t notify_cb(struct bt_conn *c, struct bt_gatt_subscribe_params *p, const void *data,
			 uint16_t len)
{
	struct note n;

	if (data == NULL) {
		return BT_GATT_ITER_STOP;
	}
	n.ch = CH_N;
	for (int i = 0; i < CH_N; i++) {
		if (p->value_handle == h[i]) {
			n.ch = i;
		}
	}
	if (n.ch == CH_NUS) {
		nus_bytes += len;
		return BT_GATT_ITER_CONTINUE;
	}
	if (n.ch == CH_DATA && tp_counting) {
		tp_account(data, len);
		return BT_GATT_ITER_CONTINUE;
	}
	n.len = MIN(len, sizeof(n.d));
	memcpy(n.d, data, n.len);
	if (k_msgq_put(&rxq, &n, K_NO_WAIT)) {
		TEST_FAIL("rx queue overflow");
	}
	return BT_GATT_ITER_CONTINUE;
}

static struct bt_gatt_subscribe_params subs[CH_N];
static struct bt_gatt_discover_params sub_disc[CH_N];

static void sub_cb(struct bt_conn *c, uint8_t err, struct bt_gatt_subscribe_params *p)
{
	ARG_UNUSED(c);
	ARG_UNUSED(p);
	if (err) {
		TEST_FAIL("subscribe failed (%u)", err);
	}
	k_sem_give(&sem_done);
}

static int subscribe(int ch)
{
	struct bt_gatt_subscribe_params *p = &subs[ch];

	if (h[ch] == 0) {
		return -ENOENT;
	}
	memset(p, 0, sizeof(*p));
	p->notify = notify_cb;
	p->subscribe = sub_cb;
	p->value = BT_GATT_CCC_NOTIFY;
	p->value_handle = h[ch];
	p->ccc_handle = BT_GATT_AUTO_DISCOVER_CCC_HANDLE;
	p->end_handle = 0xffff;
	p->disc_params = &sub_disc[ch];
	int err = bt_gatt_subscribe(conn, p);

	if (err) {
		return err;
	}
	return k_sem_take(&sem_done, K_SECONDS(5));
}

static void mtu_cb(struct bt_conn *c, uint8_t err, struct bt_gatt_exchange_params *p)
{
	ARG_UNUSED(p);
	if (err) {
		TEST_FAIL("MTU exchange failed");
	}
	k_sem_give(&sem_done);
}

static uint8_t rd_buf[64];
static uint16_t rd_len;

static uint8_t read_cb(struct bt_conn *c, uint8_t err, struct bt_gatt_read_params *p,
		       const void *data, uint16_t len)
{
	if (data) {
		uint16_t n = MIN(len, sizeof(rd_buf) - 1 - rd_len);

		memcpy(&rd_buf[rd_len], data, n);
		rd_len += n;
		return BT_GATT_ITER_CONTINUE;
	}
	rd_buf[rd_len] = 0;
	if (err) {
		TEST_PRINT("read error 0x%02x", err);
	}
	k_sem_give(&sem_rd);
	return BT_GATT_ITER_STOP;
}

static const char *read_str(uint16_t handle)
{
	static struct bt_gatt_read_params p;

	rd_len = 0;
	memset(&p, 0, sizeof(p));
	p.func = read_cb;
	p.handle_count = 1;
	p.single.handle = handle;
	p.single.offset = 0;
	int err = bt_gatt_read(conn, &p);

	if (err || k_sem_take(&sem_rd, K_SECONDS(5))) {
		TEST_PRINT("read of handle 0x%04x failed (%d)", handle, err);
		return NULL;
	}
	return (const char *)rd_buf;
}

static void write_cmd(int ch, const uint8_t *buf, size_t len)
{
	int err = bt_gatt_write_without_response(conn, h[ch], buf, len, false);

	if (err) {
		TEST_FAIL("write failed (%d)", err);
	}
}

#define CMD(...)                                                                                   \
	do {                                                                                       \
		const uint8_t _b[] = {__VA_ARGS__};                                                \
		write_cmd(CH_CTRL, _b, sizeof(_b));                                                \
	} while (0)

/* Next notification; any channel. */
static bool next(struct note *n, int ms)
{
	return k_msgq_get(&rxq, n, K_MSEC(ms)) == 0;
}

/* Waits for a notification on ch starting with cmd, dropping others. */
static bool expect(int ch, uint8_t cmd, struct note *n, int ms)
{
	int64_t end = k_uptime_get() + ms;

	while (k_uptime_get() < end) {
		if (next(n, MAX(1, (int)(end - k_uptime_get()))) && n->ch == ch && n->len &&
		    n->d[0] == cmd) {
			return true;
		}
	}
	return false;
}

static void drain(void)
{
	struct note n;

	while (next(&n, 200)) {
	}
}

/* Scan, connect, MTU, discover, subscribe. */
static int connect_and_setup(void)
{
	static struct bt_gatt_exchange_params mtu;
	static struct bt_gatt_discover_params disc;
	int err;

	seen_uuid = false;
	adv_mg = adv_setup = 0;
	adv_other_addr = false;
	memset(h, 0, sizeof(h));
	/* Every report, not just the first per address: until it is set up, the
	 * gadget's payload takes turns between Link setup and mg. */
	err = bt_le_scan_start(BT_LE_SCAN_PARAM(BT_LE_SCAN_TYPE_ACTIVE, BT_LE_SCAN_OPT_NONE,
						BT_GAP_SCAN_FAST_INTERVAL, BT_GAP_SCAN_FAST_WINDOW),
			       device_found);
	if (err) {
		return err;
	}
	if (k_sem_take(&sem_conn, K_SECONDS(10))) {
		return -ETIMEDOUT;
	}
	mtu.func = mtu_cb;
	err = bt_gatt_exchange_mtu(conn, &mtu);
	if (err || k_sem_take(&sem_done, K_SECONDS(5))) {
		return -EIO;
	}
	disc.uuid = NULL;
	disc.func = discover_cb;
	disc.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	disc.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	disc.type = BT_GATT_DISCOVER_CHARACTERISTIC;
	err = bt_gatt_discover(conn, &disc);
	if (err || k_sem_take(&sem_disc, K_SECONDS(10))) {
		return -EIO;
	}
	if (!h[CH_CTRL] || !h[CH_DATA] || !h[CH_NUS] || !h[CH_SETUP] || !h[CH_SETUP_RX]) {
		return -ENOENT;
	}
	err = subscribe(CH_CTRL);
	err = err ?: subscribe(CH_DATA);
	err = err ?: subscribe(CH_NUS);
	err = err ?: subscribe(CH_SETUP);
	TEST_PRINT("connected to %s, ATT MTU %u", peer_name, bt_gatt_get_mtu(conn));
	return err;
}

static void disconnect(void)
{
	bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	k_sem_take(&sem_gone, K_SECONDS(5));
}

/* --------------------------------------------------------------- audio */

static int16_t pcm[16000 * 3];
static int npcm;
static sbc_t sbc_dec;
static LC3_DECODER_MEM_T(10000, 16000) lc3_mem;
static lc3_decoder_t lc3_dec;

/* Share of the decoded energy in the synthetic source's 1 kHz tone. */
static double tone_ratio(void)
{
	double total = 0, re = 0, im = 0;
	int start = 800; /* skip the first 50 ms */

	for (int i = start; i < npcm; i++) {
		double ph = 2 * 3.14159265358979 * 1000.0 * i / 16000.0;

		total += (double)pcm[i] * pcm[i];
		re += pcm[i] * cos(ph);
		im += pcm[i] * sin(ph);
	}
	int n = npcm - start;

	if (n <= 0 || total == 0) {
		return 0;
	}
	/* Power of the 1 kHz component: 2|X|^2/N against sum x^2. */
	return 2 * (re * re + im * im) / n / total;
}

static bool decode_sbc(const uint8_t *d, size_t len, int *frames)
{
	struct sbc_frame f;

	for (size_t off = 0; off < len;) {
		if (sbc_probe(&d[off], &f) || f.msbc || f.freq != SBC_FREQ_16K ||
		    f.mode != SBC_MODE_MONO || f.nblocks != 16 || f.nsubbands != 8) {
			return false;
		}
		unsigned int sz = sbc_get_frame_size(&f);

		if (off + sz > len || npcm + 128 > (int)ARRAY_SIZE(pcm) ||
		    sbc_decode(&sbc_dec, &d[off], sz, &f, &pcm[npcm], 1, NULL, 0)) {
			return false;
		}
		npcm += 128;
		off += sz;
		(*frames)++;
	}
	return true;
}

static bool decode_lc3(const uint8_t *d, size_t len, int frame_bytes, int *frames)
{
	if (len % frame_bytes) {
		return false;
	}
	for (size_t off = 0; off < len; off += frame_bytes) {
		if (npcm + 160 > (int)ARRAY_SIZE(pcm) ||
		    lc3_decode(lc3_dec, &d[off], frame_bytes, LC3_PCM_FORMAT_S16, &pcm[npcm], 1)) {
			return false;
		}
		npcm += 160;
		(*frames)++;
	}
	return true;
}

static void reset_decoders(void)
{
	npcm = 0;
	sbc_reset(&sbc_dec);
	lc3_dec = lc3_setup_decoder(10000, 16000, 0, &lc3_mem);
}

/* --------------------------------------------------------------- tests */

static uint16_t err_code(const struct note *n)
{
	return n->len >= 5 ? sys_get_le16(&n->d[3]) : 0;
}

static bool test_throughput(void);

static void test_ptt(void)
{
	struct note n;
	int frames = 0;

	TEST_START("central_ptt");
	CHECK(bt_enable(NULL) == 0, "bt_enable");
	CHECK(connect_and_setup() == 0, "connect/setup failed");
	CHECK(strncmp(peer_name, MG_DEVICE_NAME_PREFIX "-", strlen(MG_DEVICE_NAME_PREFIX) + 1) == 0 &&
		      strlen(peer_name) == strlen(MG_DEVICE_NAME_PREFIX) + 7,
	      "bad name %s", peer_name);
	CHECK(bt_gatt_get_mtu(conn) >= MG_MIN_ATT_MTU, "MTU");
	if (!check_link()) {
		return;
	}

	/* Standard services. */
	const char *s = read_str(h_dis_manuf);

	CHECK(s && strcmp(s, "Seeed Studio") == 0, "DIS manufacturer '%s'", s ? s : "?");
	s = read_str(h_dis_model);
	CHECK(s && strcmp(s, "XIAO nRF54L15") == 0, "DIS model");
	s = read_str(h_dis_sw);
	CHECK(s && strcmp(s, "mg1") == 0, "DIS software revision");
	s = read_str(h_dis_fw);
	CHECK(s && strlen(s) > 0, "DIS firmware revision");
	TEST_PRINT("DIS firmware revision %s", s);
	s = read_str(h_bas);
	CHECK(s && rd_len == 1 && rd_buf[0] == 100, "battery level %u", rd_buf[0]);

	/* request_status: change_data_type, then supported_features. */
	CMD(mg_command_request_status);
	CHECK(expect(CH_CTRL, mg_command_change_data_type, &n, 2000), "no change_data_type");
	CHECK(n.len == 9 && n.d[1] == mg_data_type_audio_sbc && sys_get_le16(&n.d[2]) == 16000 &&
		      n.d[4] == 1 && sys_get_le16(&n.d[5]) == 60 && sys_get_le16(&n.d[7]) == 8000,
	      "bad change_data_type");
	CHECK(expect(CH_CTRL, mg_command_supported_features, &n, 2000), "no features");
	CHECK(memchr(&n.d[1], mg_command_start_mic, n.len - 1) != NULL, "no start_mic");
	CHECK(memchr(&n.d[1], mg_command_gesture, n.len - 1) == NULL, "lists a notify-only command");
	CHECK(memchr(&n.d[1], 0x80, n.len - 1) == NULL, "lists a reserved command");
	CHECK(expect(CH_CTRL, mg_command_supported_features, &n, 2000), "no codec list");
	CHECK(n.len == 5 && n.d[1] == mg_command_sub_feature && n.d[2] == mg_command_start_mic &&
		      n.d[3] == mg_data_type_audio_sbc && n.d[4] == mg_data_type_audio_lc3,
	      "bad codec list");

	/* set_settings, read back. */
	CMD(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 1);
	CMD(mg_command_get_settings, mg_setting_parameter_push_to_talk_enabled,
	    mg_setting_parameter_audio_codec);
	CHECK(expect(CH_CTRL, mg_command_get_settings, &n, 2000), "no get_settings reply");
	CHECK(n.len == 5 && n.d[2] == 1 && n.d[4] == mg_data_type_audio_sbc, "settings readback");
	CMD(mg_command_set_settings, 0x7e, 0);
	CHECK(expect(CH_CTRL, mg_command_error, &n, 2000), "no error for unknown setting");
	CHECK(n.d[1] == mg_command_set_settings && err_code(&n) == mg_error_code_unsupported,
	      "bad error");

	/* Push-to-talk (the gadget presses for 1.5 s): gesture down,
	 * change_data_type, whole SBC frames, gesture up, stop_mic. */
	reset_decoders();
	CHECK(expect(CH_CTRL, mg_command_gesture, &n, 10000), "no gesture");
	CHECK(n.d[1] == mg_gesture_button_down, "first gesture not down");
	CHECK(next(&n, 2000) && n.ch == CH_CTRL && n.d[0] == mg_command_change_data_type &&
		      n.d[1] == mg_data_type_audio_sbc,
	      "change_data_type must come before audio");
	bool up = false, stopped = false;
	int chunks = 0;

	while (!stopped && next(&n, 3000)) {
		if (n.ch == CH_DATA) {
			CHECK(!stopped, "audio after stop_mic");
			CHECK(decode_sbc(n.d, n.len, &frames), "chunk %d is not whole SBC frames",
			      chunks);
			chunks++;
		} else if (n.ch == CH_CTRL && n.d[0] == mg_command_gesture) {
			CHECK(n.d[1] == mg_gesture_button_up, "second gesture not up");
			up = true;
		} else if (n.ch == CH_CTRL && n.d[0] == mg_command_stop_mic) {
			CHECK(up, "stop_mic before gesture up");
			stopped = true;
		}
	}
	CHECK(stopped, "no stop_mic");
	double r = tone_ratio();

	TEST_PRINT("PTT: %d SBC frames in %d notifications (%.2f s), tone ratio %.3f", frames,
		   chunks, frames * 0.008, r);
	CHECK(frames >= 150 && frames <= 200, "expected ~1.5 s of audio, got %d frames", frames);
	CHECK(r > 0.9, "decoded audio isn't the 1 kHz tone (%.3f)", r);

	/* Client-driven capture with LC3. */
	reset_decoders();
	frames = 0;
	CMD(mg_command_start_mic, mg_data_type_audio_lc3);
	CHECK(expect(CH_CTRL, mg_command_change_data_type, &n, 2000), "no LC3 change_data_type");
	CHECK(n.len == 9 && n.d[1] == mg_data_type_audio_lc3 && sys_get_le16(&n.d[5]) == 40 &&
		      sys_get_le16(&n.d[7]) == 10000,
	      "bad LC3 format");
	int64_t until = k_uptime_get() + 1000;

	while (k_uptime_get() < until) {
		if (next(&n, 100) && n.ch == CH_DATA) {
			CHECK(decode_lc3(n.d, n.len, 40, &frames), "bad LC3 chunk (%u bytes)", n.len);
		}
	}
	CMD(mg_command_stop_mic);
	/* The device ends every capture with stop_mic, after the last chunk. */
	stopped = false;
	while (!stopped && next(&n, 2000)) {
		if (n.ch == CH_DATA) {
			CHECK(decode_lc3(n.d, n.len, 40, &frames), "bad LC3 chunk");
		} else if (n.ch == CH_CTRL && n.d[0] == mg_command_stop_mic) {
			stopped = true;
		}
	}
	CHECK(stopped, "no stop_mic after the client's stop_mic");
	CHECK(!next(&n, 300) || n.ch != CH_DATA, "audio after stop_mic");
	r = tone_ratio();
	TEST_PRINT("LC3: %d frames, tone ratio %.3f", frames, r);
	CHECK(frames >= 80 && frames <= 120, "expected ~1 s of LC3, got %d frames", frames);
	CHECK(r > 0.9, "LC3 audio isn't the tone (%.3f)", r);
	CHECK(nus_bytes > 0, "no log on NUS");
	TEST_PRINT("NUS mirrored %u bytes of log", nus_bytes);

	if (!test_throughput()) {
		return;
	}
	disconnect();
	TEST_PASS("push-to-talk, settings, SBC, LC3 and throughput test verified");
}

/* device_action throughput_test: 1 s each way, counts checked against the report. */
static bool test_throughput(void)
{
	struct note n;
	uint8_t pkt[244];

	tp_bytes = tp_pkts = tp_gaps = tp_next = 0;
	tp_counting = true;
	CMD(mg_command_device_action, mg_device_action_throughput_test, mg_throughput_test_send, 1,
	    0);
	CHECK_OR(false, expect(CH_CTRL, mg_command_change_data_type, &n, 1000) &&
				n.d[1] == mg_data_type_throughput_test,
		 "no change_data_type for the send test");
	CHECK_OR(false, expect(CH_CTRL, mg_command_device_action, &n, 3000) && n.len == 19,
		 "no send report");
	k_sleep(K_MSEC(100));
	tp_counting = false;
	uint32_t bytes = sys_get_le32(&n.d[3]), pkts = sys_get_le32(&n.d[7]),
		 ms = sys_get_le32(&n.d[11]);

	CHECK_OR(false, pkts > 0 && pkts == tp_pkts && bytes == tp_bytes && tp_gaps == 0,
		 "send: device %u packets / %u B, central %u / %u, %u gaps", pkts, bytes, tp_pkts,
		 tp_bytes, tp_gaps);
	TEST_PRINT("throughput send: %u B in %u ms (%u B/s simulated), %u packets, no gaps", bytes,
		   ms, ms ? bytes * 1000 / ms : 0, pkts);

	CMD(mg_command_device_action, mg_device_action_throughput_test, mg_throughput_test_receive,
	    0, 0);
	CMD(mg_command_change_data_type, mg_data_type_throughput_test);
	/* The gadget turns its latency off for the test; the update takes about a second
	 * to apply at latency 10. */
	for (int i = 0; i < 30 && cp_latency != 0; i++) {
		k_sleep(K_MSEC(100));
	}
	CHECK_OR(false, cp_latency == 0, "latency still %u during the receive test", cp_latency);
	int64_t end = k_uptime_get() + 1000;
	uint32_t sent = 0;

	while (k_uptime_get() < end) {
		for (int i = 0; i < (int)sizeof(pkt); i++) {
			pkt[i] = (uint8_t)(sent + i);
		}
		sys_put_le32(sent, pkt);
		int err = bt_gatt_write_without_response(conn, h[CH_DATA], pkt, sizeof(pkt), false);

		if (err == -ENOMEM || err == -ENOBUFS) {
			k_sleep(K_MSEC(1));
			continue;
		}
		CHECK_OR(false, err == 0, "data write failed (%d)", err);
		sent++;
	}
	k_sleep(K_MSEC(200));
	CMD(mg_command_device_action, mg_device_action_throughput_test, mg_throughput_test_stop);
	CHECK_OR(false, expect(CH_CTRL, mg_command_device_action, &n, 2000) && n.len == 19,
		 "no receive report");
	bytes = sys_get_le32(&n.d[3]);
	pkts = sys_get_le32(&n.d[7]);
	ms = sys_get_le32(&n.d[11]);
	CHECK_OR(false, pkts == sent && bytes == sent * sizeof(pkt) && sys_get_le32(&n.d[15]) == 0,
		 "receive: wrote %u, device got %u packets, %u gaps", sent, pkts,
		 sys_get_le32(&n.d[15]));
	TEST_PRINT("throughput receive: %u B in %u ms (%u B/s simulated), %u packets, no gaps",
		   bytes, ms, ms ? bytes * 1000 / ms : 0, pkts);
	return true;
}

static bool read_clip(uint16_t idx, uint32_t bytes, int *frames)
{
	static uint8_t clip[40000];
	struct note n;
	uint32_t got = 0;

	if (bytes > sizeof(clip)) {
		return false;
	}
	CMD(mg_command_audio_queue, mg_audio_queue_command_read_clip, idx & 0xff, idx >> 8);
	if (!expect(CH_CTRL, mg_command_change_data_type, &n, 2000) || n.len != 2 ||
	    n.d[1] != mg_data_type_audio_queue) {
		TEST_PRINT("no change_data_type(audio_queue)");
		return false;
	}
	while (got < bytes && next(&n, 3000)) {
		if (n.ch != CH_DATA) {
			continue;
		}
		if (got + n.len > bytes || n.len > bt_gatt_get_mtu(conn) - 3) {
			return false;
		}
		memcpy(&clip[got], n.d, n.len);
		got += n.len;
	}
	if (got != bytes) {
		TEST_PRINT("clip %u: %u of %u bytes", idx, got, bytes);
		return false;
	}
	reset_decoders();
	return decode_sbc(clip, bytes, frames) && tone_ratio() > 0.9;
}

static void test_offline(void)
{
	struct note n;

	TEST_START("central_offline");
	CHECK(bt_enable(NULL) == 0, "bt_enable");
	CHECK(connect_and_setup() == 0, "connect/setup failed");
	CMD(mg_command_request_status);
	CHECK(expect(CH_CTRL, mg_command_supported_features, &n, 2000), "no features");
	CHECK(memchr(&n.d[1], mg_command_audio_queue, n.len - 1) != NULL, "no audio_queue");
	drain();

	/* Turn the queue on, start empty, and leave. */
	CMD(mg_command_set_settings, mg_setting_parameter_audio_queue_enabled, 1);
	CMD(mg_command_audio_queue, mg_audio_queue_command_clear);
	CHECK(expect(CH_CTRL, mg_command_audio_queue, &n, 5000), "no status after clear");
	CHECK(n.len == 13 && n.d[1] == mg_audio_queue_command_status && n.d[2] == 1 &&
		      sys_get_le16(&n.d[11]) == 0,
	      "queue not empty/enabled");
	uint32_t capacity = sys_get_le32(&n.d[7]);

	disconnect();
	TEST_PRINT("queue on (capacity %u), disconnected", capacity);

	/* The gadget records two utterances meanwhile. */
	k_sleep(K_SECONDS(6));
	CHECK(connect_and_setup() == 0, "reconnect failed");
	CMD(mg_command_audio_queue, mg_audio_queue_command_status);
	CHECK(expect(CH_CTRL, mg_command_audio_queue, &n, 2000), "no status");
	uint16_t count = sys_get_le16(&n.d[11]);

	TEST_PRINT("queue: %u clips, %u bytes used", count, sys_get_le32(&n.d[3]));
	CHECK(count == 2, "expected 2 clips, got %u", count);

	uint32_t start[2], bytes[2];

	for (uint16_t i = 0; i < 2; i++) {
		CMD(mg_command_audio_queue, mg_audio_queue_command_clip_info, i, 0);
		CHECK(expect(CH_CTRL, mg_command_audio_queue, &n, 2000), "no clip_info");
		CHECK(n.len == 13 && n.d[1] == mg_audio_queue_command_clip_info &&
			      sys_get_le16(&n.d[2]) == i && n.d[4] == mg_data_type_audio_sbc,
		      "bad clip_info");
		start[i] = sys_get_le32(&n.d[5]);
		bytes[i] = sys_get_le32(&n.d[9]);
		TEST_PRINT("clip %u: starts at %u ms, %u bytes", i, start[i], bytes[i]);
		CHECK(bytes[i] % 60 == 0, "clip isn't whole SBC frames");
	}
	CHECK(start[1] > start[0] + 1000, "clip timestamps");
	/* 1.2 s and 0.8 s presses, 8 ms per 60-byte frame. */
	CHECK(bytes[0] > 130 * 60 && bytes[0] < 165 * 60, "clip 0 length");
	CHECK(bytes[1] > 80 * 60 && bytes[1] < 115 * 60, "clip 1 length");

	for (uint16_t i = 0; i < 2; i++) {
		int frames = 0;

		CHECK(read_clip(i, bytes[i], &frames), "reading clip %u failed", i);
		TEST_PRINT("clip %u downloaded: %d frames decode to the 1 kHz tone", i, frames);
	}

	CMD(mg_command_audio_queue, mg_audio_queue_command_read_clip, 7, 0);
	CHECK(expect(CH_CTRL, mg_command_error, &n, 2000), "no error for a bad index");
	CHECK(n.d[1] == mg_command_audio_queue && err_code(&n) == mg_error_code_invalid_value,
	      "bad error for a bad index");

	CMD(mg_command_audio_queue, mg_audio_queue_command_clear);
	CHECK(expect(CH_CTRL, mg_command_audio_queue, &n, 30000), "no status after clear");
	CHECK(sys_get_le16(&n.d[11]) == 0 && sys_get_le32(&n.d[3]) == 0, "not cleared");
	disconnect();
	TEST_PASS("offline queue: 2 clips listed, downloaded, decoded and cleared");
}

/* ----------------------------------------------------- Muse Link setup */

/* The app's side of Link setup (esp32/main/link_pairing.c is the device
 * reference), then the token proof over mgcommands with the K both sides
 * derive. */

static struct mg_setup_keys sk; /* sk.rx: mobile -> device, sk.tx: device -> mobile */
static uint64_t s_tx_ctr, s_rx_ctr;
static char node_id[32];

static void setup_write(const char *msg)
{
	size_t len = strlen(msg);
	size_t chunk = bt_gatt_get_mtu(conn) - 3 - 3;
	size_t total = (len + chunk - 1) / chunk;
	uint8_t pkt[244];

	for (size_t i = 0; i < total; i++) {
		size_t n = MIN(chunk, len - i * chunk);

		pkt[0] = 0xFE;
		pkt[1] = i;
		pkt[2] = total;
		memcpy(&pkt[3], &msg[i * chunk], n);
		for (int tries = 0; bt_gatt_write_without_response(conn, h[CH_SETUP_RX], pkt, n + 3,
							     false) != 0;
		     tries++) {
			if (tries > 100) {
				TEST_FAIL("setup write failed");
				return;
			}
			k_sleep(K_MSEC(10));
		}
	}
}

/* The next setup message: a reassembled chunked one or a raw status. */
static char *setup_read(int ms)
{
	static char buf[2048];
	size_t len = 0;
	struct note n;
	int64_t end = k_uptime_get() + ms;

	while (k_uptime_get() < end) {
		if (!next(&n, MAX(1, (int)(end - k_uptime_get()))) || n.ch != CH_SETUP) {
			continue;
		}
		if (n.len < 3 || n.d[0] != 0xFE) {
			memcpy(buf, n.d, n.len);
			buf[n.len] = '\0';
			return buf;
		}
		if (len + n.len - 3 >= sizeof(buf)) {
			return NULL;
		}
		memcpy(&buf[len], &n.d[3], n.len - 3);
		len += n.len - 3;
		if (n.d[1] + 1 == n.d[2]) {
			buf[len] = '\0';
			return buf;
		}
	}
	return NULL;
}

static void setup_write_enc(const char *plain)
{
	static uint8_t ct[1024];
	static char ct_b64[1400], env[1600];
	char tag[32], ctr[24];
	size_t n = strlen(plain);

	mg_setup_seal(sk.rx, MG_SETUP_M2D, s_tx_ctr, sk.session_id_b64, (const uint8_t *)plain, n,
		      ct);
	mg_b64url_encode(ct, n, ct_b64, sizeof(ct_b64));
	mg_b64url_encode(&ct[n], 16, tag, sizeof(tag));
	snprintf(ctr, sizeof(ctr), "%u", (unsigned int)s_tx_ctr++);
	snprintf(env, sizeof(env),
		 "{\"action\":\"pairing_encrypted\",\"session_id\":\"%s\",\"counter\":\"%s\","
		 "\"ciphertext\":\"%s\",\"tag\":\"%s\"}",
		 sk.session_id_b64, ctr, ct_b64, tag);
	setup_write(env);
}

/* The device's next record, opened; its status (or NULL). */
static const char *setup_read_status(int ms)
{
	static struct mg_json_obj o, p;
	static char pt[600];
	uint8_t raw[700];
	size_t cl, tl;
	char *m = setup_read(ms);

	if (!m || mg_json_parse(m, strlen(m), &o) || !mg_json_str(&o, "type") ||
	    strcmp(mg_json_str(&o, "type"), "pairing_encrypted") != 0) {
		TEST_PRINT("not a record: %s", m ? m : "(nothing)");
		return NULL;
	}
	const char *c = mg_json_str(&o, "ciphertext"), *t = mg_json_str(&o, "tag");

	if (!c || !t || mg_b64url_decode(c, strlen(c), raw, sizeof(raw) - 16, &cl) ||
	    mg_b64url_decode(t, strlen(t), &raw[cl], 16, &tl) ||
	    mg_setup_open(sk.tx, MG_SETUP_D2M, s_rx_ctr, sk.session_id_b64, raw, cl + 16,
			  (uint8_t *)pt)) {
		TEST_PRINT("record doesn't open");
		return NULL;
	}
	s_rx_ctr++;
	pt[cl] = '\0';
	TEST_PRINT("setup <- %s", pt);
	if (mg_json_parse(pt, cl, &p)) {
		return NULL;
	}
	return mg_json_str(&p, "status");
}

/* get_device_info .. pairing_confirmed, the gadget's button pressed by its
 * hook. */
static bool link_handshake(void)
{
	static struct mg_json_obj o;
	static char info[600], ready[1024];
	psa_key_id_t key;
	uint8_t mpub[65], dpub[65], mn[16], dn[16], ss[32], th[32];
	char mpub_b64[96], mn_b64[32], th_b64[48];
	size_t n;

	s_tx_ctr = s_rx_ctr = 0;
	setup_write("{\"action\":\"get_device_info\"}");
	char *m = setup_read(3000);

	CHECK_OR(false, m, "no device_info");
	strcpy(info, m);
	CHECK_OR(false, mg_json_parse(m, strlen(m), &o) == 0, "device_info isn't JSON");
	CHECK_OR(false, mg_json_str(&o, "wifi") && strcmp(mg_json_str(&o, "wifi"), "none") == 0,
		 "wifi not none: %s", info);
	CHECK_OR(false, mg_json_get(&o, "mgcommands") && mg_json_get(&o, "mgcommands")->num == 1,
		 "no mgcommands 1");
	CHECK_OR(false, mg_json_get(&o, "pairing_protocol")->num == 5, "not protocol 5");
	CHECK_OR(false, strcmp(mg_json_str(&o, "pairing_auth"), "none") == 0, "not community");
	snprintf(node_id, sizeof(node_id), "%s", mg_json_str(&o, "node_id"));
	char want_node[32];

	snprintf(want_node, sizeof(want_node), "homelink-%c%c%c%c%c%c", peer_name[11] | 0x20,
		 peer_name[12] | 0x20, peer_name[13] | 0x20, peer_name[14] | 0x20,
		 peer_name[15] | 0x20, peer_name[16] | 0x20);
	for (char *p = want_node; *p; p++) {
		if (*p >= 'A' && *p <= 'Z') {
			*p += 32;
		}
	}
	CHECK_OR(false, strcmp(node_id, want_node) == 0, "node_id %s for %s", node_id, peer_name);
	TEST_PRINT("device_info: %s", info);

	CHECK_OR(false, mg_setup_p256_generate(&key, mpub) == 0 && mg_psa_random(mn, 16) == 0,
		 "keygen");
	mg_b64url_encode(mpub, 65, mpub_b64, sizeof(mpub_b64));
	mg_b64url_encode(mn, 16, mn_b64, sizeof(mn_b64));
	char hello[300];

	snprintf(hello, sizeof(hello),
		 "{\"action\":\"pairing_client_hello\",\"version\":5,\"pairing_auth\":\"none\","
		 "\"pairing_policy\":\"confirm_press\",\"mobile_pub\":\"%s\",\"mobile_nonce\":\"%s\"}",
		 mpub_b64, mn_b64);
	setup_write(hello);
	m = setup_read(5000);
	CHECK_OR(false, m, "no pairing_ready");
	strcpy(ready, m);
	CHECK_OR(false, mg_json_parse(ready, strlen(ready), &o) == 0 &&
				strcmp(mg_json_str(&o, "type"), "pairing_ready") == 0,
		 "not pairing_ready: %s", m);
	const char *dpub_s = mg_json_str(&o, "device_pub"), *dn_s = mg_json_str(&o, "device_nonce");

	CHECK_OR(false, dpub_s && dn_s &&
				mg_b64url_decode(dpub_s, strlen(dpub_s), dpub, 65, &n) == 0 &&
				n == 65 && mg_b64url_decode(dn_s, strlen(dn_s), dn, 16, &n) == 0,
		 "bad device_pub / nonce");
	const pairing_transcript_fields_t f = {
		.device_id = mg_json_str(&o, "device_id"),
		.node_id = mg_json_str(&o, "node_id"),
		.mac = mg_json_str(&o, "mac"),
		.firmware_version = mg_json_str(&o, "firmware_version"),
		.mobile_pub = mpub_b64,
		.device_pub = dpub_s,
		.mobile_nonce = mn_b64,
		.device_nonce = dn_s,
	};
	char *t = pairing_transcript_build(true, 0, "confirm_press", &f);

	CHECK_OR(false, t && mg_psa_sha256((const uint8_t *)t, strlen(t), th) == 0, "transcript");
	free(t);
	mg_b64url_encode(th, 32, th_b64, sizeof(th_b64));
	CHECK_OR(false, strcmp(th_b64, mg_json_str(&o, "transcript_hash")) == 0,
		 "transcript hash differs");
	CHECK_OR(false, mg_setup_ecdh(key, dpub, ss) == 0 &&
				mg_setup_derive(ss, mn, dn, th, &sk, NULL) == 0,
		 "key agreement");
	psa_destroy_key(key);
	CHECK_OR(false, strcmp(sk.session_id_b64, mg_json_str(&o, "session_id")) == 0,
		 "session id differs");
	TEST_PRINT("pairing_ready: transcript hash and session id agree");

	setup_write_enc("{\"action\":\"pairing_client_finished\"}");
	const char *st = setup_read_status(5000);

	CHECK_OR(false, st && strcmp(st, "confirm_required") == 0, "no confirm_required");
	/* The gadget's hook presses the button. */
	st = setup_read_status(10000);
	CHECK_OR(false, st && strcmp(st, "pairing_confirmed") == 0, "no pairing_confirmed");
	return true;
}

static uint8_t proof_k[32];

/* mgcommands.h, Token proof, with the stdlib-free PSA helpers. */
static int proof_key(const char *access, const char *node, uint8_t k[32])
{
	return mg_psa_hkdf32((const uint8_t *)MG_TOKEN_PROOF_SALT, strlen(MG_TOKEN_PROOF_SALT),
			     (const uint8_t *)access, strlen(access), (const uint8_t *)node,
			     strlen(node), k);
}

static void proof_mac(bool device, const uint8_t cn[16], const uint8_t dn[16], uint8_t mac[32])
{
	const char *label = device ? MG_TOKEN_PROOF_DEVICE_LABEL : MG_TOKEN_PROOF_CLIENT_LABEL;
	const uint8_t *parts[] = {(const uint8_t *)label, cn, dn};
	const size_t lens[] = {strlen(label), 16, 16};

	mg_psa_hmac_sha256(proof_k, 32, parts, lens, 3, mac);
}

/* challenge / response / confirm; returns the result byte, or -1. */
static int run_proof(void)
{
	struct note n;
	uint8_t cmd[2 + 32], mac[32];

	cmd[0] = mg_command_token_proof;
	cmd[1] = mg_token_proof_challenge;
	mg_psa_random(&cmd[2], 16);
	write_cmd(CH_CTRL, cmd, 18);
	if (!expect(CH_CTRL, mg_command_token_proof, &n, 3000) ||
	    n.d[1] != mg_token_proof_response || n.len != 50) {
		return -1;
	}
	proof_mac(true, &cmd[2], &n.d[2], mac);
	if (!mg_psa_ct_equal(mac, &n.d[18], 32)) {
		TEST_PRINT("device MAC doesn't verify");
		return -1;
	}
	uint8_t conf[34] = {mg_command_token_proof, mg_token_proof_confirm};

	proof_mac(false, &cmd[2], &n.d[2], &conf[2]);
	write_cmd(CH_CTRL, conf, sizeof(conf));
	if (!expect(CH_CTRL, mg_command_token_proof, &n, 3000) ||
	    n.d[1] != mg_token_proof_result) {
		return -1;
	}
	return n.d[2];
}

static void test_setup(void)
{
	static const char access[] = "bsim-access-0123456789abcdef";
	struct note n;
	char msg[300];

	TEST_START("central_setup");
	CHECK(bt_enable(NULL) == 0, "bt_enable");
	CHECK(mg_psa_init() == 0, "psa");

	/* Unprovisioned: both payloads, from one address. */
	want_both = true;
	CHECK(connect_and_setup() == 0, "connect/setup failed");
	want_both = false;
	CHECK(adv_mg && adv_setup && !adv_other_addr,
	      "advertising: mg %d, Link setup %d, other address %d", adv_mg, adv_setup,
	      adv_other_addr);
	TEST_PRINT("advertising: mg %d, Link setup %d, one address", adv_mg, adv_setup);

	/* No token yet. */
	CMD(mg_command_token_proof, mg_token_proof_challenge, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
	    11, 12, 13, 14, 15);
	CHECK(expect(CH_CTRL, mg_command_error, &n, 2000) && n.d[1] == mg_command_token_proof &&
		      err_code(&n) == mg_error_code_not_found,
	      "challenge without a token isn't not_found");

	if (!link_handshake()) {
		return;
	}
	setup_write_enc("{\"action\":\"provision_v2\",\"ssid\":\"home\",\"password\":\"x\","
			"\"access_token\":\"a\",\"refresh_token\":\"r\",\"token_type\":\"device\"}");
	const char *st = setup_read_status(3000);

	CHECK(st && strcmp(st, "error_wifi_unsupported") == 0, "Wi-Fi not refused");
	snprintf(msg, sizeof(msg),
		 "{\"action\":\"provision_v2\",\"access_token\":\"%s\",\"refresh_token\":"
		 "\"bsim-refresh\",\"token_type\":\"device\"}",
		 access);
	setup_write_enc(msg);
	st = setup_read_status(5000);
	CHECK(st && strcmp(st, "auth_ok") == 0, "no auth_ok");
	TEST_PRINT("token-only provisioning: auth_ok");

	/* mgcommands on the same connection: 34 listed, the proof matches. */
	CHECK(proof_key(access, node_id, proof_k) == 0, "K");
	drain();
	CMD(mg_command_request_status);
	CHECK(expect(CH_CTRL, mg_command_supported_features, &n, 2000), "no features");
	CHECK(memchr(&n.d[1], mg_command_token_proof, n.len - 1) != NULL, "34 not listed");
	drain();
	CHECK(run_proof() == 1, "proof didn't match");
	TEST_PRINT("token proof: match");
	disconnect();

	/* Set up: mg only, hello refused, the proof still matches. */
	k_sleep(K_MSEC(500));
	CHECK(connect_and_setup() == 0, "reconnect failed");
	CHECK(adv_mg && !adv_setup, "Link setup still advertised (%d)", adv_setup);
	setup_write("{\"action\":\"pairing_client_hello\",\"version\":5,\"pairing_auth\":\"none\","
		    "\"pairing_policy\":\"confirm_press\",\"mobile_pub\":\"x\","
		    "\"mobile_nonce\":\"y\"}");
	char *m = setup_read(3000);

	CHECK(m && strcmp(m, "error_pairing_unavailable") == 0, "hello after setup: %s",
	      m ? m : "(nothing)");
	CHECK(run_proof() == 1, "proof didn't match after reconnecting");
	/* clear: result 1, then the gadget is back in setup. */
	CMD(mg_command_token_proof, mg_token_proof_clear);
	CHECK(expect(CH_CTRL, mg_command_token_proof, &n, 3000) &&
		      n.d[1] == mg_token_proof_result && n.d[2] == 1,
	      "clear not answered with result 1");
	disconnect();

	k_sleep(K_MSEC(500));
	want_both = true;
	CHECK(connect_and_setup() == 0, "reconnect after clear failed");
	want_both = false;
	CHECK(adv_setup > 0, "Link setup not advertised after clear");
	CMD(mg_command_token_proof, mg_token_proof_challenge, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
	    11, 12, 13, 14, 15);
	CHECK(expect(CH_CTRL, mg_command_error, &n, 2000) && err_code(&n) == mg_error_code_not_found,
	      "token survived clear");
	disconnect();
	TEST_PASS("Link setup (v5, button), token-only provisioning, token proof and clear verified");
}

static const struct bst_test_instance tests[] = {
	{.test_id = "central_ptt", .test_main_f = test_ptt},
	{.test_id = "central_offline", .test_main_f = test_offline},
	{.test_id = "central_setup", .test_main_f = test_setup},
	BSTEST_END_MARKER,
};

static struct bst_test_list *install(struct bst_test_list *list)
{
	return bst_add_tests(list, tests);
}

bst_test_install_t test_installers[] = {install, NULL};

int main(void)
{
	bst_main();
	return 0;
}
