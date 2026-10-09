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

#ifndef MG_PAIRING_H_
#define MG_PAIRING_H_

#include <stdint.h>

#include "mgcommands-secure.h"

/* Stored (key_id, PK) pairs, persisted with Zephyr settings under
 * "mg/pair/<slot>". Oldest is evicted when full. */

int mg_pairing_load(void);
int mg_pairing_count(void);
/* 0 and PK copied out, or -ENOENT. */
int mg_pairing_find(const uint8_t key_id[MG_KEY_ID_SIZE], uint8_t pk[32]);
int mg_pairing_add(const uint8_t key_id[MG_KEY_ID_SIZE], const uint8_t pk[32]);
int mg_pairing_remove(const uint8_t key_id[MG_KEY_ID_SIZE]);
/* Test hook: forget everything in RAM and storage. */
void mg_pairing_clear(void);

#endif
