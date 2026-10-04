# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# musegadgets BLE audio (CONFIG_MUSE_GADGET_BLE_AUDIO): the protocol in
# protocols/ at the repository root, served on Home Link's NimBLE host. See
# README.md. Without the option it builds nothing; its headers stay available
# so callers compile unchanged.
set(protocols "../../../protocols")
if(CONFIG_MUSE_GADGET_BLE_AUDIO)
    set(srcs "mg_ble.c" "mg_voice.c" "mg_proto.c" "mg_codec.c" "mg_route.c" "mg_queue.c" "mg_play.c"
        "mg_resample.c" "mg_token_proof.c")
else()
    set(srcs)
endif()

# Requirements resolve before Kconfig, so they're unconditional.
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include" ${protocols}
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES bt nvs_flash mbedtls esp_partition esp_timer esp_app_format libsbc liblc3
)
