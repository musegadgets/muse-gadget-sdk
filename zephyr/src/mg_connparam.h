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


#ifndef MG_CONNPARAM_H_
#define MG_CONNPARAM_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Connection parameters. After connecting (once the 2M PHY and data length
 * updates are done, or 2 s), the gadget asks for CONFIG_MG_CONN_INTERVAL_MIN
 * / _MAX (1.25 ms units), CONFIG_MG_CONN_LATENCY and CONFIG_MG_CONN_TIMEOUT
 * (10 ms units): 15 ms, latency 10, 4 s by default. A request the central
 * doesn't grant within 5 s is sent once more.
 *
 * Peripheral latency lets the gadget sleep through connection events when
 * it has nothing to send, which is fine for device->phone traffic (the
 * controller wakes for queued notifications) but slows phone->device
 * transfers: the central's packets wait for the next event the gadget
 * listens on. Zephyr's controller has no way to suspend latency without a
 * parameter update, so while the phone streams to the gadget (an SMP upload,
 * set_tokens, a playback stream) it asks for latency 0, and for
 * CONFIG_MG_CONN_LATENCY again CONFIG_MG_CONN_LATENCY_HOLD_MS after the last
 * activity. Changes are logged: "mg.ble: latency off (dfu)", "mg.ble:
 * latency on".
 */

enum mg_xfer {
	MG_XFER_DFU,
	MG_XFER_TOKENS,
	MG_XFER_STREAM,
	MG_XFER_BENCH,
};

void mg_connparam_init(void);
void mg_connparam_connected(void);
void mg_connparam_disconnected(void);
/* The PHY or data length update finished (either order). */
void mg_connparam_phy_done(void);
void mg_connparam_data_len_done(void);
/* The parameters in force now (le_param_updated). */
void mg_connparam_updated(uint16_t interval, uint16_t latency, uint16_t timeout);
/* Phone->device data for a transfer arrived: keep latency off for a while. */
void mg_connparam_busy(enum mg_xfer what);
bool mg_connparam_latency_off(void);

#endif
