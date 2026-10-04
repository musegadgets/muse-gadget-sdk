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

/* The host shims behind tests/host/shim, and fakes for the firmware modules
 * the portable ones call (transport, LED, core, session). */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include "host_fakes.h"
#include "mg_core.h"
#include "mg_led.h"
#include "mg_transport.h"

int host_log_verbose;

/* ---- kernel: fake clock and delayed work ---- */

int64_t host_now_ms = 1000;
static struct k_work_delayable *works[16];
static int nworks;

void k_work_init_delayable(struct k_work_delayable *dw, k_work_handler_t handler)
{
	memset(dw, 0, sizeof(*dw));
	dw->work.handler = handler;
	for (int i = 0; i < nworks; i++) {
		if (works[i] == dw) {
			return;
		}
	}
	works[nworks++] = dw;
}

int k_work_reschedule(struct k_work_delayable *dw, k_timeout_t delay)
{
	dw->pending = true;
	dw->due = host_now_ms + delay.ms;
	return 0;
}

int k_work_reschedule_for_queue(struct k_work_q *q, struct k_work_delayable *dw,
				k_timeout_t delay)
{
	(void)q;
	return k_work_reschedule(dw, delay);
}

int k_work_cancel_delayable(struct k_work_delayable *dw)
{
	dw->pending = false;
	return 0;
}

int64_t k_uptime_get(void)
{
	return host_now_ms;
}

void host_advance(int64_t ms)
{
	int64_t end = host_now_ms + ms;

	for (;;) {
		struct k_work_delayable *next = NULL;

		for (int i = 0; i < nworks; i++) {
			if (works[i]->pending && works[i]->due <= end &&
			    (!next || works[i]->due < next->due)) {
				next = works[i];
			}
		}
		if (!next) {
			break;
		}
		if (next->due > host_now_ms) {
			host_now_ms = next->due;
		}
		next->pending = false;
		next->work.handler(&next->work);
	}
	host_now_ms = end;
}

/* ---- settings ---- */

struct entry {
	char name[48];
	uint8_t *val;
	size_t len;
};

static struct entry store[32];
static int fail_save_in;

void host_settings_clear(void)
{
	for (int i = 0; i < 32; i++) {
		free(store[i].val);
	}
	memset(store, 0, sizeof(store));
	fail_save_in = 0;
}

void host_settings_fail_save(int nth)
{
	fail_save_in = nth;
}

static struct entry *find(const char *name)
{
	for (int i = 0; i < 32; i++) {
		if (store[i].val && strcmp(store[i].name, name) == 0) {
			return &store[i];
		}
	}
	return NULL;
}

int settings_save_one(const char *name, const void *value, size_t len)
{
	if (fail_save_in > 0 && --fail_save_in == 0) {
		return -EIO;
	}
	struct entry *e = find(name);

	if (!e) {
		for (int i = 0; i < 32 && !e; i++) {
			if (!store[i].val) {
				e = &store[i];
			}
		}
		if (!e) {
			return -ENOSPC;
		}
		strncpy(e->name, name, sizeof(e->name) - 1);
	} else {
		free(e->val);
	}
	e->val = malloc(len ? len : 1);
	memcpy(e->val, value, len);
	e->len = len;
	return 0;
}

int settings_delete(const char *name)
{
	struct entry *e = find(name);

	if (e) {
		free(e->val);
		memset(e, 0, sizeof(*e));
	}
	return 0;
}

struct rd {
	const struct entry *e;
};

static ssize_t read_cb(void *arg, void *data, size_t len)
{
	struct rd *r = arg;
	size_t n = len < r->e->len ? len : r->e->len;

	memcpy(data, r->e->val, n);
	return (ssize_t)n;
}

int settings_load_subtree_direct(const char *subtree, settings_load_direct_cb cb, void *param)
{
	size_t sl = strlen(subtree);

	for (int i = 0; i < 32; i++) {
		struct entry *e = &store[i];

		if (!e->val || strncmp(e->name, subtree, sl) != 0) {
			continue;
		}
		const char *rest = NULL;

		if (e->name[sl] == '/') {
			rest = &e->name[sl + 1];
		} else if (e->name[sl] != '\0') {
			continue;
		}
		struct rd r = {.e = e};

		cb(rest, e->len, read_cb, &r, param);
	}
	return 0;
}

int host_settings_len(const char *name)
{
	struct entry *e = find(name);

	return e ? (int)e->len : -1;
}

const void *host_settings_get(const char *name)
{
	struct entry *e = find(name);

	return e ? e->val : NULL;
}

int host_settings_count(void)
{
	int n = 0;

	for (int i = 0; i < 32; i++) {
		n += store[i].val != NULL;
	}
	return n;
}

/* ---- firmware fakes ---- */

struct host_note host_notes[HOST_MAX_NOTES];
int host_nnotes;
uint16_t host_mtu = 185;
int host_disconnects;
int host_adv_refreshes;
int host_link_refreshes;
bool host_pair_led;
static struct k_work_q app_q;

void host_notes_clear(void)
{
	host_nnotes = 0;
}

static void note(uint8_t ch, const uint8_t *buf, size_t len)
{
	if (host_nnotes < HOST_MAX_NOTES) {
		struct host_note *n = &host_notes[host_nnotes++];

		n->ch = ch;
		n->len = (uint16_t)len;
		memcpy(n->data, buf, len);
	}
}

int mg_transport_notify(enum mg_att_chan ch, const uint8_t *buf, size_t len, k_timeout_t wait)
{
	(void)wait;
	if (len > host_mtu - 3) {
		return -EMSGSIZE;
	}
	note(ch, buf, len);
	return 0;
}

uint16_t mg_transport_att_mtu(void)
{
	return host_mtu;
}

void mg_transport_disconnect(void)
{
	host_disconnects++;
}

struct k_work_q *mg_app_wq(void)
{
	return &app_q;
}

const char *mg_app_version(void)
{
	return "1.0.0";
}

void mg_ble_refresh_advertising(void)
{
	host_adv_refreshes++;
}

void mg_core_link_refresh(void)
{
	host_link_refreshes++;
}

void mg_led_set(uint32_t flag, bool on)
{
	if (flag == MG_LED_PAIR_PENDING) {
		host_pair_led = on;
	}
}

int mg_session_send_control(const uint8_t *buf, size_t len)
{
	note(MG_ATT_CONTROL, buf, len);
	return 0;
}

void mg_core_send_error(uint8_t cmd, uint16_t code, const char *log)
{
	uint8_t b[5] = {mg_command_error, cmd, mg_error_data_code, (uint8_t)code,
			(uint8_t)(code >> 8)};

	(void)log;
	note(MG_ATT_CONTROL, b, sizeof(b));
}
