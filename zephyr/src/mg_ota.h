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


#ifndef MG_OTA_H_
#define MG_OTA_H_

#include <stdbool.h>

/*
 * Firmware updates (CONFIG_MG_DFU): MCUmgr SMP over BLE with MCUboot
 * (swap using move). The app confirms a test image once it is healthy, and
 * shares MCUboot's secondary slot with the offline clip queue (mg_queue.h).
 */

/* After mg_queue_init(): decides who holds the slot, registers MCUmgr hooks. */
void mg_ota_init(void);

/* BLE is up and advertising: confirm a test image, then reclaim the slot. */
void mg_ota_healthy(void);

/* For the bench status line. */
bool mg_ota_confirmed(void);
bool mg_ota_upload_active(void);

#endif
