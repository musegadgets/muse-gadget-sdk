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
#include "mg_throughput.h"
#include "mgcommands.h"

LOG_MODULE_REGISTER(mg_session, CONFIG_MG_LOG_LEVEL);

/* One sender at a time on Control and Data. */
static K_MUTEX_DEFINE(tx_lock);

static struct {
	bool connected;
	bool sub[MG_ATT_CHAN_COUNT];
	bool ready;
} s;

static void update_ready(void);

/* ------------------------------------------------------------------------
 * Control and Data, in plaintext.
 * ---------------------------------------------------------------------- */

void mg_session_init(void)
{
}

static bool authorized(void)
{
	return s.connected;
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

static void update_ready(void)
{
	/* Ready for gestures and audio: Control and Data both subscribed. */
	bool ready = authorized() && s.sub[MG_ATT_CONTROL] && s.sub[MG_ATT_DATA];

	if (ready != s.ready) {
		s.ready = ready;
		mg_core_set_ready(ready);
	}
}

void mg_session_connected(void)
{
	memset(&s, 0, sizeof(s));
	s.connected = true;
	mg_core_connected();
	update_ready();
}

void mg_session_disconnected(void)
{
	s.connected = false;
	update_ready();
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
