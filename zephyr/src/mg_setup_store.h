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

#ifndef MG_SETUP_STORE_H_
#define MG_SETUP_STORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * What Muse Link setup leaves on the gadget (protocols/README.md, Gadget
 * setup over Muse Link), in Zephyr settings (ZMS):
 *
 *   mg/setup/at    the access token, as provisioned
 *   mg/setup/rt    the refresh token
 *   mg/setup/k     the token proof key K (32 bytes, mgcommands.h)
 *   mg/setup/done  the commit record: magic, flags (setup complete, Wi-Fi
 *                  skipped) and the three lengths
 *
 * ZMS writes one entry atomically, not several, so "done" is the commit
 * point: it is written last, after the other three, and erased first. At
 * boot, setup counts as complete only if "done" is valid and the three
 * entries exist with the lengths it records; anything else (a provisioning
 * or a reset cut short by a power loss) is erased, and the gadget is back in
 * setup. A token-only setup records Wi-Fi as skipped in the same record, so
 * nothing treats the missing Wi-Fi as a half-written pairing.
 *
 * Tokens and K are never logged: lengths only.
 */

#define MG_SETUP_TOKEN_MAX 2048
#define MG_SETUP_K_LEN     32

/* Reads the commit record and cleans up a half-written one (call after
 * settings_subsys_init()). */
int mg_setup_store_load(void);

/* Setup is complete: the gadget holds tokens and K. */
bool mg_setup_store_complete(void);
bool mg_setup_store_wifi_skipped(void);

/*
 * Stores a token-only setup: the tokens, K, then the commit record (setup
 * complete, Wi-Fi skipped). 0 once it is all durable; on any failure
 * whatever was written is erased again and a negative errno returned.
 */
int mg_setup_store_commit(const char *access, size_t access_len, const char *refresh,
			  size_t refresh_len, const uint8_t k[MG_SETUP_K_LEN]);

/* K, read from storage. -ENOENT without a complete setup. */
int mg_setup_store_get_k(uint8_t k[MG_SETUP_K_LEN]);

/* Erases the commit record, K and both tokens (in that order). 0, or the
 * first error (the rest is still attempted; boot recovery finishes it). */
int mg_setup_store_erase(void);

#endif
