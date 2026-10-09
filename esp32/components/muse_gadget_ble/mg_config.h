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

/*
 * Build options of the musegadgets BLE component, from Kconfig on the device.
 * The host tests define the MG_* macros themselves.
 */

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

#ifndef MG_WITH_SECURE
#ifdef CONFIG_MUSE_GADGET_BLE_SECURE
#define MG_WITH_SECURE 1
#else
#define MG_WITH_SECURE 0
#endif
#endif

/* A BLE-only gadget: no Wi-Fi path to route to. */
#ifndef MG_STANDALONE
#ifdef CONFIG_MUSE_GADGET_BLE_STANDALONE
#define MG_STANDALONE 1
#else
#define MG_STANDALONE 0
#endif
#endif

#ifndef MG_WITH_LC3
#ifdef CONFIG_MUSE_GADGET_BLE_LC3
#define MG_WITH_LC3 1
#else
#define MG_WITH_LC3 0
#endif
#endif

#ifndef MG_SBC_BITPOOL
#ifdef CONFIG_MUSE_GADGET_BLE_SBC_BITPOOL
#define MG_SBC_BITPOOL CONFIG_MUSE_GADGET_BLE_SBC_BITPOOL
#else
#define MG_SBC_BITPOOL 26
#endif
#endif

/* Longest utterance, from the press or start_mic. */
#ifndef MG_MAX_UTTERANCE_S
#define MG_MAX_UTTERANCE_S 30
#endif
