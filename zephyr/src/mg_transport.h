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

#ifndef MG_TRANSPORT_H_
#define MG_TRANSPORT_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

/* The musegadgets characteristics that carry notifications. */
enum mg_att_chan {
	MG_ATT_CONTROL,
	MG_ATT_DATA,
	MG_ATT_ENC_CONTROL,
	MG_ATT_ENC_DATA,
	/* Muse Link setup's TX characteristic (notifications), and its RX
	 * characteristic for writes (mg_setup.h). */
	MG_ATT_SETUP,
	MG_ATT_CHAN_COUNT,
};

/*
 * Queues one notification. Notifications go out in the order they are
 * queued, across all characteristics. Returns -ENOMEM if no buffer freed up
 * within wait, -ENOTCONN without a subscribed client. Implemented by
 * mg_ble.c (and by fakes in the unit tests).
 */
int mg_transport_notify(enum mg_att_chan ch, const uint8_t *buf, size_t len, k_timeout_t wait);
void mg_transport_disconnect(void);
uint16_t mg_transport_att_mtu(void);
/* The advertised name, e.g. MuseGadget-1A2B3C (set once BLE is up). */
const char *mg_transport_name(void);

/* Asks the central for connection parameters (units: 1.25 ms, events,
 * 10 ms). 0, or a negative errno if it couldn't be sent. */
int mg_transport_conn_params(uint16_t interval_min, uint16_t interval_max, uint16_t latency,
			     uint16_t timeout);

/* The app work queue: all protocol state lives on it. */
struct k_work_q *mg_app_wq(void);

#endif
