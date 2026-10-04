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


#include "mg_connparam.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "mg_transport.h"

LOG_MODULE_REGISTER(mg_connparam, CONFIG_MG_LOG_LEVEL);

#define READY_WAIT_MS 2000 /* PHY and data length updates, at most */
#define GRANT_WAIT_MS 5000 /* for the central to apply a request */

static const char *const XFER_NAMES[] = {"dfu", "tokens", "stream", "throughput test"};

static struct k_spinlock lock;
static struct {
	bool connected;
	bool phy, dle;
	int64_t conn_at;
	int64_t busy_until;
	uint8_t busy_what;
	bool latency_off;    /* the mode we're in */
	bool have_cur;
	uint16_t interval, latency, timeout; /* in force */
	bool pending;
	int64_t req_at;
	uint8_t retries;
	bool gave_up;
} s;
static struct k_work_delayable work;

static void schedule_at(int64_t at, int64_t now)
{
	k_work_reschedule_for_queue(mg_app_wq(), &work, K_MSEC(MAX(at - now, 1)));
}

static void eval(struct k_work *w)
{
	ARG_UNUSED(w);
	k_spinlock_key_t key = k_spin_lock(&lock);
	int64_t now = k_uptime_get();
	int64_t next = 0;
	bool request = false, log_mode = false, log_gave_up = false;
	uint16_t latency = 0;

	if (!s.connected) {
		goto out;
	}
	if (!(s.phy && s.dle) && now - s.conn_at < READY_WAIT_MS) {
		next = s.conn_at + READY_WAIT_MS;
		goto out;
	}
	bool off = s.busy_until > now;

	if (off != s.latency_off) {
		s.latency_off = off;
		s.retries = 0;
		s.pending = false;
		s.gave_up = false;
		log_mode = true;
	}
	if (off) {
		next = s.busy_until;
	}
	latency = off ? 0 : CONFIG_MG_CONN_LATENCY;
	if (s.have_cur && s.latency == latency && s.interval >= CONFIG_MG_CONN_INTERVAL_MIN &&
	    s.interval <= CONFIG_MG_CONN_INTERVAL_MAX && s.timeout == CONFIG_MG_CONN_TIMEOUT) {
		s.pending = false;
		goto out;
	}
	if (s.gave_up) {
		goto out;
	}
	if (s.pending && now - s.req_at < GRANT_WAIT_MS) {
		int64_t t = s.req_at + GRANT_WAIT_MS;

		next = next ? MIN(next, t) : t;
		goto out;
	}
	if (s.pending) {
		if (s.retries >= 1) {
			s.pending = false;
			s.gave_up = true;
			log_gave_up = true;
			goto out;
		}
		s.retries++;
	}
	s.pending = true;
	s.req_at = now;
	request = true;
	next = next ? MIN(next, now + GRANT_WAIT_MS) : now + GRANT_WAIT_MS;
out:
	if (next) {
		schedule_at(next, now);
	}
	uint8_t what = s.busy_what;
	bool off_now = s.latency_off;
	uint8_t retries = s.retries;

	k_spin_unlock(&lock, key);

	if (log_mode) {
		if (off_now) {
			LOG_INF("mg.ble: latency off (%s)", XFER_NAMES[what]);
		} else {
			LOG_INF("mg.ble: latency on");
		}
	}
	if (log_gave_up) {
		LOG_WRN("mg.ble: the central didn't grant latency %u; keeping what it chose",
			off_now ? 0 : CONFIG_MG_CONN_LATENCY);
	}
	if (request) {
		int err = mg_transport_conn_params(CONFIG_MG_CONN_INTERVAL_MIN,
						   CONFIG_MG_CONN_INTERVAL_MAX, latency,
						   CONFIG_MG_CONN_TIMEOUT);

		LOG_INF("mg.ble: asking for %u-%u x 1.25 ms, latency %u, timeout %u ms%s (%d)",
			CONFIG_MG_CONN_INTERVAL_MIN, CONFIG_MG_CONN_INTERVAL_MAX, latency,
			CONFIG_MG_CONN_TIMEOUT * 10, retries ? ", again" : "", err);
	}
}

static void kick(void)
{
	k_work_reschedule_for_queue(mg_app_wq(), &work, K_NO_WAIT);
}

void mg_connparam_init(void)
{
	k_work_init_delayable(&work, eval);
}

void mg_connparam_connected(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	memset(&s, 0, sizeof(s));
	s.connected = true;
	s.conn_at = k_uptime_get();
	k_spin_unlock(&lock, key);
	kick();
}

void mg_connparam_disconnected(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	memset(&s, 0, sizeof(s));
	k_spin_unlock(&lock, key);
	(void)k_work_cancel_delayable(&work);
}

void mg_connparam_phy_done(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	s.phy = true;
	k_spin_unlock(&lock, key);
	kick();
}

void mg_connparam_data_len_done(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	s.dle = true;
	k_spin_unlock(&lock, key);
	kick();
}

void mg_connparam_updated(uint16_t interval, uint16_t latency, uint16_t timeout)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	s.have_cur = true;
	s.interval = interval;
	s.latency = latency;
	s.timeout = timeout;
	k_spin_unlock(&lock, key);
	kick();
}

void mg_connparam_busy(enum mg_xfer what)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool was_off = s.busy_until > k_uptime_get();

	s.busy_until = k_uptime_get() + CONFIG_MG_CONN_LATENCY_HOLD_MS;
	if (!was_off) {
		s.busy_what = what;
	}
	k_spin_unlock(&lock, key);
	if (!was_off) {
		kick();
	}
}

bool mg_connparam_latency_off(void)
{
	return s.latency_off;
}
