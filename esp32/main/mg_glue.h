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

#pragma once

// Adds musegadgets BLE audio (components/muse_gadget_ble) to Link's BLE
// server, with this board's battery, name and mic. Only built with
// CONFIG_MUSE_GADGET_BLE_AUDIO. Call from app_run() once NVS is up, before
// the BLE server starts.
void mg_glue_start(void);

// Link setup stored or erased the token proof key ("mg_proof_k").
void mg_glue_setup_changed(void);
