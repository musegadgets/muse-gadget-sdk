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

#ifndef MG_APP_H_
#define MG_APP_H_

#include <stdint.h>

const char *mg_app_version(void);
uint8_t mg_app_battery_percent(void);
/* The last battery reading in mV, or <0 without one. */
int mg_app_battery_mv(void);

/* mg_bench.c (CONFIG_MG_BENCH) */
void mg_bench_init(void);

/* mg_ble.c */
int mg_ble_init(void);
int mg_ble_start_advertising(void);
/* Re-reads what to advertise (Link setup or not) and applies it. */
void mg_ble_refresh_advertising(void);
void mg_ble_set_battery(uint8_t percent);

/* mg_nus.c */
void mg_nus_init(void);

#endif
