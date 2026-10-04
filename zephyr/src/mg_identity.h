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

#ifndef MG_IDENTITY_H_
#define MG_IDENTITY_H_

#include <stdint.h>

/*
 * The names the gadget goes by, from its BLE identity address (the one it
 * advertises from), like an ESP32 gadget's from its MAC:
 *   name       MuseGadget-1A2B3C   (CONFIG_MG_DEVICE_NAME_PREFIX, last 3 bytes)
 *   node_id    homelink-1a2b3c     (the name the Muse server knows it by)
 *   mac        aa:bb:cc:1a:2b:3c
 *   device_id  hatch-link:aa:bb:cc:1a:2b:3c  (the Link setup device id)
 */

/* addr: most significant byte first. */
void mg_identity_init(const char *name_prefix, const uint8_t addr[6]);
const char *mg_identity_name(void);
const char *mg_identity_node_id(void);
const char *mg_identity_mac(void);
const char *mg_identity_device_id(void);

#endif
