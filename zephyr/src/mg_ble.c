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

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "mg_app.h"
#include "mg_connparam.h"
#include "mg_identity.h"
#include "mg_session.h"
#include "mg_setup.h"
#include "mg_transport.h"
#include "mgcommands.h"
#if defined(CONFIG_MG_SECURE)
#include "mgcommands-secure.h"
#endif

LOG_MODULE_REGISTER(mg_ble, CONFIG_MG_LOG_LEVEL);

#define MAX_ATT 244 /* ATT payload with the largest MTU we accept */

/* ------------------------------------------------------------------------
 * Events from the BT stack, handled in order on the app work queue.
 * ---------------------------------------------------------------------- */

enum evt_type {
	EVT_CONNECTED,
	EVT_DISCONNECTED,
	EVT_CCC,
	EVT_WRITE,
};

struct evt {
	uint8_t type;
	uint8_t ch;
	uint16_t len;
	uint8_t data[MAX_ATT];
};

K_MSGQ_DEFINE(evt_q, sizeof(struct evt), 8, 4);
static struct k_work evt_work;

static void post(uint8_t type, uint8_t ch, const void *data, uint16_t len)
{
	struct evt e = {.type = type, .ch = ch, .len = MIN(len, MAX_ATT)};

	if (data) {
		memcpy(e.data, data, e.len);
	}
	if (k_msgq_put(&evt_q, &e, K_MSEC(100)) != 0) {
		LOG_WRN("event queue full, dropped event %u", type);
		return;
	}
	k_work_submit_to_queue(mg_app_wq(), &evt_work);
}

static void evt_fn(struct k_work *w)
{
	static struct evt e;

	ARG_UNUSED(w);
	while (k_msgq_get(&evt_q, &e, K_NO_WAIT) == 0) {
		switch (e.type) {
		case EVT_CONNECTED:
			mg_setup_connected();
			mg_session_connected();
			break;
		case EVT_DISCONNECTED:
			mg_setup_disconnected();
			mg_session_disconnected();
			break;
		case EVT_CCC:
			if (e.ch != MG_ATT_SETUP) {
				mg_session_subscribed(e.ch, e.data[0]);
			}
			break;
		case EVT_WRITE:
			if (e.ch == MG_ATT_SETUP) {
				mg_setup_rx(e.data, e.len);
			} else {
				mg_session_rx(e.ch, e.data, e.len);
			}
			break;
		}
	}
}

/* ------------------------------------------------------------------------
 * GATT
 * ---------------------------------------------------------------------- */

static const struct bt_uuid_128 mg_svc_uuid = BT_UUID_INIT_128(MG_SERVICE_UUID_LE_BYTES);
static const struct bt_uuid_128 mg_ctrl_uuid = BT_UUID_INIT_128(MG_CONTROL_UUID_LE_BYTES);
static const struct bt_uuid_128 mg_data_uuid = BT_UUID_INIT_128(MG_DATA_UUID_LE_BYTES);
#if defined(CONFIG_MG_SECURE)
static const struct bt_uuid_128 mg_ectrl_uuid =
	BT_UUID_INIT_128(MG_ENCRYPTED_CONTROL_UUID_LE_BYTES);
static const struct bt_uuid_128 mg_edata_uuid = BT_UUID_INIT_128(MG_ENCRYPTED_DATA_UUID_LE_BYTES);
#endif

static ssize_t on_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(flags);
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len > MAX_ATT) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	post(EVT_WRITE, (uint8_t)(uintptr_t)attr->user_data, buf, len);
	return len;
}

#define CCC_HANDLER(name, chan)                                                                    \
	static void name(const struct bt_gatt_attr *attr, uint16_t value)                          \
	{                                                                                          \
		uint8_t on = value == BT_GATT_CCC_NOTIFY;                                          \
		ARG_UNUSED(attr);                                                                  \
		post(EVT_CCC, chan, &on, 1);                                                       \
	}

CCC_HANDLER(ccc_ctrl, MG_ATT_CONTROL)
CCC_HANDLER(ccc_data, MG_ATT_DATA)
CCC_HANDLER(ccc_setup, MG_ATT_SETUP)
#if defined(CONFIG_MG_SECURE)
CCC_HANDLER(ccc_ectrl, MG_ATT_ENC_CONTROL)
CCC_HANDLER(ccc_edata, MG_ATT_ENC_DATA)
#endif

#define MG_CHRC(u, chan, ccc)                                                                      \
	BT_GATT_CHARACTERISTIC(&(u).uuid, BT_GATT_CHRC_WRITE_WITHOUT_RESP | BT_GATT_CHRC_NOTIFY, \
			       BT_GATT_PERM_WRITE, NULL, on_write, (void *)(uintptr_t)(chan)),      \
		BT_GATT_CCC(ccc, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE)

BT_GATT_SERVICE_DEFINE(mg_svc,
	BT_GATT_PRIMARY_SERVICE(&mg_svc_uuid.uuid),
	MG_CHRC(mg_ctrl_uuid, MG_ATT_CONTROL, ccc_ctrl),
	MG_CHRC(mg_data_uuid, MG_ATT_DATA, ccc_data),
#if defined(CONFIG_MG_SECURE)
	MG_CHRC(mg_ectrl_uuid, MG_ATT_ENC_CONTROL, ccc_ectrl),
	MG_CHRC(mg_edata_uuid, MG_ATT_ENC_DATA, ccc_edata),
#endif
);

/* Muse Link setup (mg_setup.h): RX takes the app's (chunked) JSON, TX
 * notifies the replies and reads back the last plaintext status. */
static const struct bt_uuid_128 setup_svc_uuid = BT_UUID_INIT_128(MG_SETUP_SERVICE_UUID_LE);
static const struct bt_uuid_128 setup_rx_uuid = BT_UUID_INIT_128(MG_SETUP_RX_UUID_LE);
static const struct bt_uuid_128 setup_tx_uuid = BT_UUID_INIT_128(MG_SETUP_TX_UUID_LE);

static ssize_t setup_tx_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			     uint16_t len, uint16_t offset)
{
	char st[64];
	size_t n = mg_setup_read_status(st, sizeof(st));

	return bt_gatt_attr_read(conn, attr, buf, len, offset, st, n);
}

BT_GATT_SERVICE_DEFINE(setup_svc,
	BT_GATT_PRIMARY_SERVICE(&setup_svc_uuid.uuid),
	BT_GATT_CHARACTERISTIC(&setup_rx_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, on_write, (void *)(uintptr_t)MG_ATT_SETUP),
	BT_GATT_CHARACTERISTIC(&setup_tx_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, setup_tx_read, NULL, NULL),
	BT_GATT_CCC(ccc_setup, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* Value attribute of each notifying characteristic: in the mg service,
 * service then (decl, value, CCC) per characteristic; in the setup service,
 * service, RX (decl, value), TX (decl, value, CCC). */
static const struct bt_gatt_attr *value_attr(enum mg_att_chan ch)
{
	return ch == MG_ATT_SETUP ? &setup_svc.attrs[4] : &mg_svc.attrs[2 + 3 * ch];
}

/* ------------------------------------------------------------------------
 * Connection
 * ---------------------------------------------------------------------- */

static struct bt_conn *cur_conn;
static K_MUTEX_DEFINE(conn_lock);
static struct k_work adv_work;
static struct bt_gatt_exchange_params mtu_params;

static void mtu_cb(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *p)
{
	ARG_UNUSED(p);
	LOG_INF("ATT MTU %u%s", bt_gatt_get_mtu(conn), err ? " (exchange failed)" : "");
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_WRN("connection failed (0x%02x)", err);
		k_work_submit(&adv_work);
		return;
	}
	k_mutex_lock(&conn_lock, K_FOREVER);
	if (cur_conn) {
		k_mutex_unlock(&conn_lock);
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_CONN_LIMIT_EXCEEDED);
		return;
	}
	cur_conn = bt_conn_ref(conn);
	k_mutex_unlock(&conn_lock);
	LOG_INF("connected");

	/* Ask for what audio needs; the client may settle on less: the largest
	 * MTU, 251-byte link-layer PDUs and the 2M PHY, then (mg_connparam.c)
	 * the connection interval and latency. */
	mg_connparam_connected();
	mtu_params.func = mtu_cb;
	(void)bt_gatt_exchange_mtu(conn, &mtu_params);
	/* A central running its own update at the same time makes the
	 * controller refuse ours; the result still comes to the callbacks
	 * below. */
	int e = bt_conn_le_data_len_update(conn, BT_LE_DATA_LEN_PARAM_MAX);

	if (e) {
		LOG_INF("mg.ble: data length update already under way (%d)", e);
	}
	e = bt_conn_le_phy_update(conn, BT_CONN_LE_PHY_PARAM_2M);
	if (e) {
		LOG_INF("mg.ble: 2M PHY update already under way (%d)", e);
	}
	post(EVT_CONNECTED, 0, NULL, 0);
}

static void tx_flush(void);

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	k_mutex_lock(&conn_lock, K_FOREVER);
	if (conn != cur_conn) {
		k_mutex_unlock(&conn_lock);
		return;
	}
	bt_conn_unref(cur_conn);
	cur_conn = NULL;
	k_mutex_unlock(&conn_lock);
	LOG_INF("disconnected (0x%02x)", reason);
	mg_connparam_disconnected();
	tx_flush();
	post(EVT_DISCONNECTED, 0, NULL, 0);
}

static void recycled(void)
{
	k_work_submit(&adv_work);
}

static const char *phy_name(uint8_t phy)
{
	return phy == BT_GAP_LE_PHY_2M ? "2M" : phy == BT_GAP_LE_PHY_1M ? "1M" : "coded";
}

static void phy_updated(struct bt_conn *conn, struct bt_conn_le_phy_info *p)
{
	ARG_UNUSED(conn);
	LOG_INF("mg.ble: phy tx %s rx %s", phy_name(p->tx_phy), phy_name(p->rx_phy));
	mg_connparam_phy_done();
}

static void data_len_updated(struct bt_conn *conn, struct bt_conn_le_data_len_info *d)
{
	ARG_UNUSED(conn);
	LOG_INF("mg.ble: data length tx %u B / %u us, rx %u B / %u us", d->tx_max_len,
		d->tx_max_time, d->rx_max_len, d->rx_max_time);
	mg_connparam_data_len_done();
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
			     uint16_t timeout)
{
	ARG_UNUSED(conn);
	uint32_t us = interval * 1250U;

	LOG_INF("mg.ble: conn %u.%02u ms latency %u timeout %u ms", us / 1000, (us % 1000) / 10,
		latency, timeout * 10U);
	mg_connparam_updated(interval, latency, timeout);
}

static bool le_param_req(struct bt_conn *conn, struct bt_le_conn_param *p)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(p);
	return true; /* the central decides; ours follow (mg_connparam.c) */
}

BT_CONN_CB_DEFINE(conn_cb) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
	.le_param_req = le_param_req,
	.le_param_updated = le_param_updated,
	.le_phy_updated = phy_updated,
	.le_data_len_updated = data_len_updated,
};

int mg_transport_conn_params(uint16_t interval_min, uint16_t interval_max, uint16_t latency,
			     uint16_t timeout)
{
	const struct bt_le_conn_param p = {
		.interval_min = interval_min,
		.interval_max = interval_max,
		.latency = latency,
		.timeout = timeout,
	};
	int err = -ENOTCONN;

	k_mutex_lock(&conn_lock, K_FOREVER);
	if (cur_conn) {
		err = bt_conn_le_param_update(cur_conn, &p);
	}
	k_mutex_unlock(&conn_lock);
	return err;
}

uint16_t mg_transport_att_mtu(void)
{
	uint16_t mtu = 23;

	k_mutex_lock(&conn_lock, K_FOREVER);
	if (cur_conn) {
		mtu = bt_gatt_get_mtu(cur_conn);
	}
	k_mutex_unlock(&conn_lock);
	return MIN(mtu, MAX_ATT + 3);
}

void mg_transport_disconnect(void)
{
	k_mutex_lock(&conn_lock, K_FOREVER);
	if (cur_conn) {
		(void)bt_conn_disconnect(cur_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
	k_mutex_unlock(&conn_lock);
}

/* ------------------------------------------------------------------------
 * Notification queue: one FIFO for every characteristic keeps Control and
 * Data in order. A TX thread feeds the stack with a bounded number in
 * flight.
 * ---------------------------------------------------------------------- */

#define TX_BUFS     24
/* Notifications handed to the stack at once: enough to fill a 15 ms
 * connection event at 2M PHY (about ten 251-byte PDUs), within
 * CONFIG_BT_CONN_TX_MAX and CONFIG_BT_BUF_ACL_TX_COUNT. */
#define TX_INFLIGHT 10

struct tx_item {
	void *fifo_reserved;
	uint8_t ch;
	uint16_t len;
	uint8_t data[MAX_ATT];
};

K_MEM_SLAB_DEFINE_STATIC(tx_slab, sizeof(struct tx_item), TX_BUFS, 4);
static K_FIFO_DEFINE(tx_fifo);
static K_SEM_DEFINE(inflight, TX_INFLIGHT, TX_INFLIGHT);

int mg_transport_notify(enum mg_att_chan ch, const uint8_t *buf, size_t len, k_timeout_t wait)
{
	struct tx_item *it;
	bool ok;

	if (len > MAX_ATT || ch >= MG_ATT_CHAN_COUNT) {
		return -EMSGSIZE;
	}
	k_mutex_lock(&conn_lock, K_FOREVER);
	ok = cur_conn && bt_gatt_is_subscribed(cur_conn, value_attr(ch), BT_GATT_CCC_NOTIFY);
	k_mutex_unlock(&conn_lock);
	if (!ok) {
		return -ENOTCONN;
	}
	if (k_mem_slab_alloc(&tx_slab, (void **)&it, wait) != 0) {
		return -ENOMEM;
	}
	it->ch = ch;
	it->len = len;
	memcpy(it->data, buf, len);
	k_fifo_put(&tx_fifo, it);
	return 0;
}

static void tx_flush(void)
{
	struct tx_item *it;

	while ((it = k_fifo_get(&tx_fifo, K_NO_WAIT)) != NULL) {
		k_mem_slab_free(&tx_slab, it);
	}
}

static void sent_cb(struct bt_conn *conn, void *user_data)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(user_data);
	k_sem_give(&inflight);
}

static void tx_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		struct tx_item *it = k_fifo_get(&tx_fifo, K_FOREVER);
		struct bt_conn *conn;

		k_mutex_lock(&conn_lock, K_FOREVER);
		conn = cur_conn ? bt_conn_ref(cur_conn) : NULL;
		k_mutex_unlock(&conn_lock);

		while (conn) {
			struct bt_gatt_notify_params p = {
				.attr = value_attr(it->ch),
				.data = it->data,
				.len = it->len,
				.func = sent_cb,
			};

			if (k_sem_take(&inflight, K_MSEC(1000)) != 0) {
				/* Completions stopped (link lost); start over. */
				k_sem_reset(&inflight);
				for (int i = 0; i < TX_INFLIGHT; i++) {
					k_sem_give(&inflight);
				}
				continue;
			}
			int err = bt_gatt_notify_cb(conn, &p);

			if (err == -ENOMEM || err == -ENOBUFS) {
				k_sem_give(&inflight);
				k_sleep(K_MSEC(5));
				continue;
			}
			if (err) {
				k_sem_give(&inflight);
				LOG_DBG("notify failed (%d)", err);
			}
			break;
		}
		if (conn) {
			bt_conn_unref(conn);
		}
		k_mem_slab_free(&tx_slab, it);
	}
}

K_THREAD_DEFINE(mg_tx, 2048, tx_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(3), 0, 0);

/* ------------------------------------------------------------------------
 * Advertising: one legacy advertiser on the identity address, so the Link
 * setup and the mg service are found at the same address (the apps key what
 * setup leaves them by it). Until setup is complete the payload takes turns
 * every ADV_TURN_MS between Link setup and mg (one 128-bit UUID fits), as
 * the ESP32's ble_server does; afterwards it is mg only.
 * ---------------------------------------------------------------------- */

#define ADV_TURN_MS 1500

static const uint8_t svc_uuid_le[] = {MG_SERVICE_UUID_LE_BYTES};
static const uint8_t setup_uuid_le[] = {MG_SETUP_SERVICE_UUID_LE};
/* As the ESP32's: a test company id and "not set up". */
static const uint8_t setup_mfg[] = {0xff, 0xff, 0x00};
static char name[CONFIG_BT_DEVICE_NAME_MAX];
static K_MUTEX_DEFINE(adv_lock);
static bool ble_ready;
static bool link_turn; /* the Link setup payload is up */
static struct k_work_delayable adv_turn_work;

static void adv_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	(void)mg_ble_start_advertising();
}

static size_t build_ad(struct bt_data *ad, bool link)
{
	static const uint8_t flags = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;

	ad[0] = (struct bt_data)BT_DATA(BT_DATA_FLAGS, &flags, 1);
	if (link) {
		ad[1] = (struct bt_data)BT_DATA(BT_DATA_UUID128_ALL, setup_uuid_le,
						sizeof(setup_uuid_le));
		ad[2] = (struct bt_data)BT_DATA(BT_DATA_MANUFACTURER_DATA, setup_mfg,
						sizeof(setup_mfg));
		return 3;
	}
	ad[1] = (struct bt_data)BT_DATA(BT_DATA_UUID128_ALL, svc_uuid_le, sizeof(svc_uuid_le));
	return 2;
}

/* The scan response, the same for both payloads: the name and BAS. */
static size_t build_sd(struct bt_data *sd)
{
	static const uint8_t bas[] = {BT_UUID_16_ENCODE(MG_BAS_SERVICE_UUID16)};

	sd[0] = (struct bt_data)BT_DATA(BT_DATA_NAME_COMPLETE, name, strlen(name));
	sd[1] = (struct bt_data)BT_DATA(BT_DATA_UUID16_SOME, bas, sizeof(bas));
	return 2;
}

static void adv_turn_fn(struct k_work *w)
{
	struct bt_data ad[3], sd[2];

	ARG_UNUSED(w);
	k_mutex_lock(&adv_lock, K_FOREVER);
	link_turn = mg_setup_advertise_link() && !link_turn;
	size_t n = build_ad(ad, link_turn);
	/* Both again: a NULL scan response would empty it. -EAGAIN when not
	 * advertising (connected): the next start resumes. */
	int err = bt_le_adv_update_data(ad, n, sd, build_sd(sd));

	if (err == 0 && mg_setup_advertise_link()) {
		k_work_reschedule(&adv_turn_work, K_MSEC(ADV_TURN_MS));
	}
	k_mutex_unlock(&adv_lock);
}

int mg_ble_start_advertising(void)
{
	struct bt_data ad[3], sd[2];
	size_t sn = build_sd(sd);
	const struct bt_le_adv_param param = BT_LE_ADV_PARAM_INIT(
		BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL);

	k_mutex_lock(&conn_lock, K_FOREVER);
	bool busy = cur_conn != NULL;

	k_mutex_unlock(&conn_lock);
	if (busy) {
		return 0;
	}
	k_mutex_lock(&adv_lock, K_FOREVER);
	bool link = mg_setup_advertise_link();

	link_turn = link;
	size_t n = build_ad(ad, link_turn);
	int err = bt_le_adv_start(&param, ad, n, sd, sn);

	if (err == -EALREADY) {
		err = bt_le_adv_update_data(ad, n, sd, sn);
	}
	if (!err && link) {
		k_work_reschedule(&adv_turn_work, K_MSEC(ADV_TURN_MS));
	} else {
		(void)k_work_cancel_delayable(&adv_turn_work);
	}
	k_mutex_unlock(&adv_lock);
	if (err) {
		LOG_ERR("advertising failed (%d)", err);
		return err;
	}
	LOG_INF("advertising as %s%s", name, link ? " (Link setup and mg, taking turns)" : "");
	return 0;
}

void mg_ble_refresh_advertising(void)
{
	if (ble_ready) {
		(void)mg_ble_start_advertising();
	}
}

const char *mg_transport_name(void)
{
	return name;
}

void mg_ble_set_battery(uint8_t percent)
{
	(void)bt_bas_set_battery_level(percent);
}

int mg_ble_init(void)
{
	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = ARRAY_SIZE(addrs);
	int err;

	k_work_init(&evt_work, evt_fn);
	k_work_init(&adv_work, adv_fn);
	k_work_init_delayable(&adv_turn_work, adv_turn_fn);
	mg_connparam_init();
	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
		return err;
	}
	bt_id_get(addrs, &count);
	/* The identity address, most significant byte first. */
	uint8_t a[6];

	for (int i = 0; i < 6; i++) {
		a[i] = addrs[0].a.val[5 - i];
	}
	mg_identity_init(CONFIG_MG_DEVICE_NAME_PREFIX, a);
	snprintf(name, sizeof(name), "%s", mg_identity_name());
	(void)bt_set_name(name);
	ble_ready = true;
	return 0;
}
