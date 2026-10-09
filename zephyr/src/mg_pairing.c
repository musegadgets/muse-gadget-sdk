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

#include "mg_pairing.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(mg_pairing, CONFIG_MG_LOG_LEVEL);

#define N CONFIG_MG_SECURE_MAX_PAIRINGS

struct slot {
	uint8_t used;
	uint32_t age; /* higher is newer */
	uint8_t key_id[MG_KEY_ID_SIZE];
	uint8_t pk[32];
};

/* Stored form: age (LE32), key_id, PK. */
#define BLOB_LEN (4 + MG_KEY_ID_SIZE + 32)

static struct slot slots[N];
static uint32_t next_age = 1;

static int save_slot(int i)
{
	char key[24];

	snprintf(key, sizeof(key), "mg/pair/%d", i);
	if (!slots[i].used) {
		return settings_delete(key);
	}
	uint8_t blob[BLOB_LEN];

	sys_put_le32(slots[i].age, blob);
	memcpy(&blob[4], slots[i].key_id, MG_KEY_ID_SIZE);
	memcpy(&blob[4 + MG_KEY_ID_SIZE], slots[i].pk, 32);
	int err = settings_save_one(key, blob, sizeof(blob));

	memset(blob, 0, sizeof(blob));
	return err;
}

static int set_cb(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	uint8_t blob[BLOB_LEN];
	int i = atoi(name);

	if (i < 0 || i >= N || len != sizeof(blob) || read_cb(cb_arg, blob, len) != (ssize_t)len) {
		return 0;
	}
	slots[i].used = 1;
	slots[i].age = sys_get_le32(blob);
	memcpy(slots[i].key_id, &blob[4], MG_KEY_ID_SIZE);
	memcpy(slots[i].pk, &blob[4 + MG_KEY_ID_SIZE], 32);
	next_age = MAX(next_age, slots[i].age + 1);
	memset(blob, 0, sizeof(blob));
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(mg_pair, "mg/pair", NULL, set_cb, NULL, NULL);

int mg_pairing_load(void)
{
	memset(slots, 0, sizeof(slots));
	int err = settings_load_subtree("mg/pair");

	LOG_INF("%d stored pairing(s)", mg_pairing_count());
	return err;
}

int mg_pairing_count(void)
{
	int n = 0;

	for (int i = 0; i < N; i++) {
		n += slots[i].used;
	}
	return n;
}

static int find(const uint8_t key_id[MG_KEY_ID_SIZE])
{
	for (int i = 0; i < N; i++) {
		if (slots[i].used && memcmp(slots[i].key_id, key_id, MG_KEY_ID_SIZE) == 0) {
			return i;
		}
	}
	return -1;
}

int mg_pairing_find(const uint8_t key_id[MG_KEY_ID_SIZE], uint8_t pk[32])
{
	int i = find(key_id);

	if (i < 0) {
		return -ENOENT;
	}
	memcpy(pk, slots[i].pk, 32);
	return 0;
}

int mg_pairing_add(const uint8_t key_id[MG_KEY_ID_SIZE], const uint8_t pk[32])
{
	int i = find(key_id);

	if (i < 0) {
		/* Free slot, else the oldest. */
		i = 0;
		for (int j = 0; j < N; j++) {
			if (!slots[j].used) {
				i = j;
				break;
			}
			if (slots[j].age < slots[i].age) {
				i = j;
			}
		}
	}
	slots[i].used = 1;
	slots[i].age = next_age++;
	memcpy(slots[i].key_id, key_id, MG_KEY_ID_SIZE);
	memcpy(slots[i].pk, pk, 32);
	return save_slot(i);
}

int mg_pairing_remove(const uint8_t key_id[MG_KEY_ID_SIZE])
{
	int i = find(key_id);

	if (i < 0) {
		return -ENOENT;
	}
	memset(&slots[i], 0, sizeof(slots[i]));
	return save_slot(i);
}

void mg_pairing_clear(void)
{
	for (int i = 0; i < N; i++) {
		memset(&slots[i], 0, sizeof(slots[i]));
		(void)save_slot(i);
	}
}
