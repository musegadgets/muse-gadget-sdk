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


#include <zephyr/ztest.h>

#include "fakes.h"
#include "mg_connparam.h"

#define MIN_I CONFIG_MG_CONN_INTERVAL_MIN
#define MAX_I CONFIG_MG_CONN_INTERVAL_MAX
#define LAT   CONFIG_MG_CONN_LATENCY
#define TMO   CONFIG_MG_CONN_TIMEOUT
#define HOLD  CONFIG_MG_CONN_LATENCY_HOLD_MS

static void *setup(void)
{
	fake_init_once();
	mg_connparam_init();
	return NULL;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	mg_connparam_disconnected();
	fake_nconn_reqs = 0;
}

static void settle(void)
{
	k_sleep(K_MSEC(10));
}

static void assert_req(int i, uint16_t latency)
{
	zassert_true(fake_nconn_reqs > i, "request %d not sent (%d)", i, fake_nconn_reqs);
	zassert_equal(fake_conn_reqs[i].min, MIN_I);
	zassert_equal(fake_conn_reqs[i].max, MAX_I);
	zassert_equal(fake_conn_reqs[i].latency, latency, "latency %u", fake_conn_reqs[i].latency);
	zassert_equal(fake_conn_reqs[i].timeout, TMO);
}

/* The defaults satisfy Apple's accessory rules. */
ZTEST(connparam, test_apple_rules)
{
	zassert_equal(MIN_I, 12, "15 ms, not 7.5-15 ms");
	zassert_equal(MAX_I, 12);
	zassert_true(MIN_I * 5 >= 60, "interval >= 15 ms");
	zassert_true(MIN_I == MAX_I || MAX_I * 5 >= MIN_I * 5 + 60);
	zassert_true(LAT <= 30);
	zassert_true(TMO >= 200 && TMO <= 600, "2-6 s");
	/* interval max x (latency + 1) x 3 < timeout, in ms */
	zassert_true(MAX_I * 125 / 100 * (LAT + 1) * 3 < TMO * 10);
#if defined(CONFIG_BT_PERIPHERAL_PREF_MIN_INT)
	zassert_equal(CONFIG_BT_PERIPHERAL_PREF_MIN_INT, MIN_I, "GAP PPCP says the same");
	zassert_equal(CONFIG_BT_PERIPHERAL_PREF_MAX_INT, MAX_I);
	zassert_equal(CONFIG_BT_PERIPHERAL_PREF_LATENCY, LAT);
	zassert_equal(CONFIG_BT_PERIPHERAL_PREF_TIMEOUT, TMO);
#endif
}

/* Asked once the PHY and data length updates are done (or after 2 s). */
ZTEST(connparam, test_initial_request)
{
	mg_connparam_connected();
	settle();
	zassert_equal(fake_nconn_reqs, 0, "waits for PHY and data length");
	mg_connparam_phy_done();
	settle();
	zassert_equal(fake_nconn_reqs, 0);
	mg_connparam_data_len_done();
	settle();
	assert_req(0, LAT);
	mg_connparam_updated(MIN_I, LAT, TMO);
	k_sleep(K_SECONDS(10));
	zassert_equal(fake_nconn_reqs, 1, "granted: nothing more");

	/* A central that never reports PHY or data length: asked after 2 s. */
	mg_connparam_disconnected();
	fake_nconn_reqs = 0;
	mg_connparam_connected();
	k_sleep(K_MSEC(1900));
	zassert_equal(fake_nconn_reqs, 0);
	k_sleep(K_MSEC(200));
	assert_req(0, LAT);
}

/* Not granted: asked once more, then the central's choice stands. */
ZTEST(connparam, test_retry_once)
{
	mg_connparam_connected();
	mg_connparam_phy_done();
	mg_connparam_data_len_done();
	settle();
	assert_req(0, LAT);
	mg_connparam_updated(24, 0, 500); /* the central's own: 30 ms */
	k_sleep(K_MSEC(5100));
	assert_req(1, LAT);
	k_sleep(K_SECONDS(30));
	zassert_equal(fake_nconn_reqs, 2, "no third request");
}

/* Phone->device transfers turn latency off, and it comes back 2 s after. */
ZTEST(connparam, test_latency_off_while_streaming_to_device)
{
	mg_connparam_connected();
	mg_connparam_phy_done();
	mg_connparam_data_len_done();
	settle();
	mg_connparam_updated(MIN_I, LAT, TMO);
	settle();
	zassert_equal(fake_nconn_reqs, 1);
	zassert_false(mg_connparam_latency_off());

	/* An SMP upload: a chunk every 50 ms for 3 s. */
	for (int i = 0; i < 60; i++) {
		mg_connparam_busy(MG_XFER_DFU);
		if (i == 0) {
			settle();
			zassert_true(mg_connparam_latency_off());
			assert_req(1, 0);
			mg_connparam_updated(MIN_I, 0, TMO);
		}
		k_sleep(K_MSEC(50));
	}
	zassert_true(mg_connparam_latency_off(), "still on while chunks arrive");
	zassert_equal(fake_nconn_reqs, 2, "one request for the whole upload");
	k_sleep(K_MSEC(HOLD - 300));
	zassert_true(mg_connparam_latency_off());
	k_sleep(K_MSEC(400));
	zassert_false(mg_connparam_latency_off(), "back on after the hold");
	assert_req(2, LAT);
	mg_connparam_updated(MIN_I, LAT, TMO);
	settle();
	zassert_equal(fake_nconn_reqs, 3);

	/* set_tokens chunks do the same. */
	mg_connparam_busy(MG_XFER_TOKENS);
	settle();
	assert_req(3, 0);
}

/* A disconnect ends it all; the next connection starts afresh. */
ZTEST(connparam, test_disconnect)
{
	mg_connparam_connected();
	mg_connparam_phy_done();
	mg_connparam_data_len_done();
	settle();
	mg_connparam_busy(MG_XFER_DFU);
	settle();
	mg_connparam_disconnected();
	int n = fake_nconn_reqs;

	k_sleep(K_SECONDS(10));
	zassert_equal(fake_nconn_reqs, n);
	zassert_false(mg_connparam_latency_off());
}

ZTEST_SUITE(connparam, NULL, setup, before, NULL, NULL);
