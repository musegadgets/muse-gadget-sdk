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

#include "mg_setup_store.h"

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(mg_setup_store, CONFIG_MG_LOG_LEVEL);

#define TREE     "mg/setup"
#define KEY_AT   TREE "/at"
#define KEY_RT   TREE "/rt"
#define KEY_K    TREE "/k"
#define KEY_DONE TREE "/done"

#define FLAG_COMPLETE     0x01
#define FLAG_WIFI_SKIPPED 0x02

/* The commit record, little-endian. */
struct done_rec {
	uint8_t magic[4]; /* "MGS1" */
	uint8_t flags;
	uint8_t reserved;
	uint8_t at_len[2];
	uint8_t rt_len[2];
	uint8_t k_len[2];
};

static const uint8_t MAGIC[4] = {'M', 'G', 'S', '1'};

/* What load found: each entry's length (0: absent), and the record. */
static struct {
	size_t at, rt, k;
	bool have_done;
	struct done_rec done;
} found;

static bool complete;
static bool wifi_skipped;

static uint16_t get16(const uint8_t b[2])
{
	return (uint16_t)(b[0] | (b[1] << 8));
}

static void put16(uint8_t b[2], size_t v)
{
	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
}

static int scan_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
		   void *param)
{
	ARG_UNUSED(param);
	if (key == NULL) {
		return 0;
	}
	if (strcmp(key, "at") == 0) {
		found.at = len;
	} else if (strcmp(key, "rt") == 0) {
		found.rt = len;
	} else if (strcmp(key, "k") == 0) {
		found.k = len;
	} else if (strcmp(key, "done") == 0) {
		found.have_done = len == sizeof(found.done) &&
				  read_cb(cb_arg, &found.done, sizeof(found.done)) ==
					  (ssize_t)sizeof(found.done);
	}
	return 0;
}

static bool record_valid(void)
{
	const struct done_rec *d = &found.done;

	return found.have_done && memcmp(d->magic, MAGIC, sizeof(MAGIC)) == 0 &&
	       (d->flags & FLAG_COMPLETE) && get16(d->k_len) == MG_SETUP_K_LEN &&
	       found.k == MG_SETUP_K_LEN && get16(d->at_len) != 0 && found.at == get16(d->at_len) &&
	       get16(d->rt_len) != 0 && found.rt == get16(d->rt_len);
}

int mg_setup_store_load(void)
{
	memset(&found, 0, sizeof(found));
	complete = false;
	wifi_skipped = false;
	int err = settings_load_subtree_direct(TREE, scan_cb, NULL);

	if (err) {
		LOG_ERR("couldn't read the setup store (%d)", err);
		return err;
	}
	if (record_valid()) {
		complete = true;
		wifi_skipped = (found.done.flags & FLAG_WIFI_SKIPPED) != 0;
		LOG_INF("setup complete (Wi-Fi %s): access %u B, refresh %u B, proof key held",
			wifi_skipped ? "skipped" : "set", (unsigned int)found.at,
			(unsigned int)found.rt);
		return 0;
	}
	if (found.have_done || found.at || found.rt || found.k) {
		LOG_WRN("half-written setup (record %s, access %u B, refresh %u B, key %u B): erased",
			found.have_done ? "invalid" : "missing", (unsigned int)found.at,
			(unsigned int)found.rt, (unsigned int)found.k);
		(void)mg_setup_store_erase();
	} else {
		LOG_INF("not set up: waiting for Muse Link setup");
	}
	return 0;
}

bool mg_setup_store_complete(void)
{
	return complete;
}

bool mg_setup_store_wifi_skipped(void)
{
	return wifi_skipped;
}

int mg_setup_store_erase(void)
{
	static const char *const keys[] = {KEY_DONE, KEY_K, KEY_AT, KEY_RT};
	int first = 0;

	complete = false;
	wifi_skipped = false;
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		int err = settings_delete(keys[i]);

		if (err && err != -ENOENT && !first) {
			first = err;
		}
	}
	memset(&found, 0, sizeof(found));
	if (first) {
		LOG_ERR("erasing the setup store failed (%d)", first);
	}
	return first;
}

int mg_setup_store_commit(const char *access, size_t access_len, const char *refresh,
			  size_t refresh_len, const uint8_t k[MG_SETUP_K_LEN])
{
	struct done_rec d = {.flags = FLAG_COMPLETE | FLAG_WIFI_SKIPPED};
	int err;

	if (access_len == 0 || refresh_len == 0 || access_len > MG_SETUP_TOKEN_MAX ||
	    refresh_len > MG_SETUP_TOKEN_MAX) {
		return -EINVAL;
	}
	/* From a clean slate: nothing old can be mistaken for part of this. */
	err = mg_setup_store_erase();
	err = err ?: settings_save_one(KEY_AT, access, access_len);
	err = err ?: settings_save_one(KEY_RT, refresh, refresh_len);
	err = err ?: settings_save_one(KEY_K, k, MG_SETUP_K_LEN);
	if (!err) {
		memcpy(d.magic, MAGIC, sizeof(MAGIC));
		put16(d.at_len, access_len);
		put16(d.rt_len, refresh_len);
		put16(d.k_len, MG_SETUP_K_LEN);
		err = settings_save_one(KEY_DONE, &d, sizeof(d));
	}
	if (err) {
		LOG_ERR("storing the setup failed (%d): rolled back", err);
		(void)mg_setup_store_erase();
		return err < 0 ? err : -EIO;
	}
	found.at = access_len;
	found.rt = refresh_len;
	found.k = MG_SETUP_K_LEN;
	found.have_done = true;
	found.done = d;
	complete = true;
	wifi_skipped = true;
	LOG_INF("setup stored (token-only, Wi-Fi skipped): access %u B, refresh %u B",
		(unsigned int)access_len, (unsigned int)refresh_len);
	return 0;
}

struct read_k {
	uint8_t *out;
	ssize_t got;
};

static int read_k_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
		     void *param)
{
	struct read_k *r = param;

	if (key == NULL && len == MG_SETUP_K_LEN) {
		r->got = read_cb(cb_arg, r->out, MG_SETUP_K_LEN);
	}
	return 0;
}

int mg_setup_store_get_k(uint8_t k[MG_SETUP_K_LEN])
{
	struct read_k r = {.out = k, .got = -ENOENT};

	if (!complete) {
		return -ENOENT;
	}
	(void)settings_load_subtree_direct(KEY_K, read_k_cb, &r);
	if (r.got != MG_SETUP_K_LEN) {
		memset(k, 0, MG_SETUP_K_LEN);
		LOG_ERR("couldn't read the proof key (%d)", (int)r.got);
		return -EIO;
	}
	return 0;
}
