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

# liblc3: Google's Apache-2.0 LC3 codec, trimmed to 16 kHz / 10 ms and no LTPF
# for voice over musegadgets BLE (components/muse_gadget_ble). The sources are
# the one copy in xplat/liblc3, shared with the Zephyr SDK; see its
# README.md.
set(src_dir "${CMAKE_CURRENT_LIST_DIR}/../../../xplat/liblc3")
if(CONFIG_MUSE_GADGET_BLE_LC3)
    set(srcs)
    foreach(f attdet bits bwdet energy lc3 ltpf mdct plc sns spec tables tns)
        list(APPEND srcs "${src_dir}/src/${f}.c")
    endforeach()
    idf_component_register(
        SRCS ${srcs}
        INCLUDE_DIRS "${src_dir}/include"
        PRIV_INCLUDE_DIRS "${src_dir}/src")
    # The LTPF options size the encoder and decoder state, so they are public.
    target_compile_definitions(${COMPONENT_LIB}
        PUBLIC LC3_ENC_LTPF=0 LC3_DEC_LTPF=0
        PRIVATE LC3_PLUS=0 LC3_PLUS_HR=0 LC3_DT_MASK=8 LC3_SR_MASK=2)
    # Upstream code as it ships: unused tables of the trimmed configurations stay.
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -ffast-math -Wno-unused-function -Wno-unused-const-variable)
else()
    idf_component_register()
endif()
