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

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(muse_gadget_zephyr)

# The XIAO's flash map (mcuboot, slot0, slot1, storage) boots through MCUboot:
# without it the image would sit at slot0 with nothing at 0 to start it.
if(CONFIG_BOARD_XIAO_NRF54L15 AND NOT CONFIG_BOOTLOADER_MCUBOOT)
  message(FATAL_ERROR "Build the XIAO nRF54L15 with --sysbuild: it boots through MCUboot "
    "(sysbuild.conf). See AGENTS.md, Build.")
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/mg_common.cmake)

target_include_directories(app PRIVATE ${MG_PROTOCOLS} src)

target_sources(app PRIVATE
  src/main.c
  src/mg_core.c
  src/mg_settings.c
  src/mg_session.c
  src/mg_identity.c
  src/mg_json.c
  src/mg_psa.c
  src/mg_setup.c
  src/mg_setup_crypto.c
  src/mg_setup_store.c
  src/mg_token_proof.c
  src/mg_codec.c
  src/mg_audio.c
  src/mg_hpf.c
  src/mg_ble.c
  src/mg_connparam.c
  src/mg_throughput.c
  src/mg_led.c
  src/mg_button.c
)
target_sources_ifdef(CONFIG_MG_AUDIO_QUEUE app PRIVATE src/mg_queue.c)
target_sources_ifdef(CONFIG_MG_SECURE app PRIVATE src/mg_crypto.c src/mg_pairing.c)
target_sources_ifdef(CONFIG_MG_BATTERY_ADC app PRIVATE src/mg_battery.c)
target_sources_ifdef(CONFIG_MG_NUS app PRIVATE src/mg_nus.c)
target_sources_ifdef(CONFIG_MG_DFU app PRIVATE src/mg_ota.c)
target_sources_ifdef(CONFIG_MG_BENCH app PRIVATE src/mg_bench.c)
target_sources_ifdef(CONFIG_MG_AUDIO_SRC_DMIC app PRIVATE src/mg_audio_dmic.c)
target_sources_ifdef(CONFIG_MG_AUDIO_SRC_SYNTH app PRIVATE src/mg_audio_synth.c)
if(CONFIG_MG_BSIM_TESTS)
  add_subdirectory(${ZEPHYR_BASE}/tests/bsim/babblekit babblekit)
  target_link_libraries(app PRIVATE babblekit)
  target_sources(app PRIVATE tests/bsim/gadget_hooks.c)
  zephyr_include_directories(
    $ENV{BSIM_COMPONENTS_PATH}/libUtilv1/src $ENV{BSIM_COMPONENTS_PATH}/libPhyComv1/src)
endif()

mg_add_transcript(app)
mg_add_sbc(app)
if(CONFIG_MG_LC3)
  mg_add_lc3(app)
endif()
