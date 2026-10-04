<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# liblc3

Google's LC3 (Low Complexity Communication Codec, ETSI TS 103 634) encoder and
decoder, the second codec of the musegadgets BLE protocol: 16 kHz mono, 10 ms
frames, 40 bytes a frame (32 kb/s) by default. One copy serves both SDKs: the
ESP32 component [`esp32/components/liblc3`](../../esp32/components/liblc3)
(built with `CONFIG_MUSE_GADGET_BLE_LC3`) and the Zephyr build
([`zephyr/cmake/mg_common.cmake`](../../zephyr/cmake/mg_common.cmake), with
`CONFIG_MG_LC3`) compile these sources with their own options.

| | |
|---|---|
| Upstream | https://github.com/google/liblc3, the library Zephyr ships as its `liblc3` module |
| Version | commit `8e1e722cda8dbdcc4b3cb9ba559d11c236c33d07` (2026-09-09) |
| License | Apache-2.0, Copyright 2022 Google LLC. See [`LICENSE`](LICENSE). The trimming is by Meta Platforms, Inc. and affiliates under the same license. |

## What's upstream and what's ours

The sources are an optimized build of upstream from Meta's codec benchmark
(the `lc3opt` tree, configuration "L5v"). Each changed file says so in its
header. The bitstream stays standard LC3.

| File | From |
|---|---|
| `include/lc3.h` | upstream, unmodified |
| `include/lc3_private.h` | upstream, modified: state sized for builds without LTPF |
| `src/lc3.c` | upstream, modified: frame durations and sample rates outside `LC3_DT_MASK` / `LC3_SR_MASK` are rejected so their code drops out; LTPF and TNS can be compiled out |
| `src/tns.c`, `src/tns.h`, `src/tables.h` | upstream, modified: optional TNS and LTPF trimming |
| `src/tables.c`, `src/ltpf.c` | upstream, regenerated for 16 kHz / 10 ms: other configurations' table entries are NULL so the linker drops their tables |
| the other `src/` files | upstream, unmodified |
| `LICENSE` | upstream, unmodified |
| `README.md` | Meta |

## Build options

Both SDKs build the same configuration:

- `LC3_DT_MASK=8`, `LC3_SR_MASK=2`: 10 ms frames at 16 kHz only; other
  durations and rates are rejected at setup.
- `LC3_ENC_LTPF=0`, `LC3_DEC_LTPF=0`: no long-term postfilter. These size the
  encoder and decoder state, so every user of `lc3.h` needs them. The encoder
  signals LTPF off in every frame, so any standard LC3 decoder plays the
  stream.
- `LC3_PLUS=0`, `LC3_PLUS_HR=0`: no LC3plus modes.
- `LC3_ENC_TNS` exists but stays at its default (TNS on).

The ESP32 adds `-ffast-math`. A firmware that only encodes (the Zephyr
gadget) gets no decoder: the linker drops it.

## Numbers

16 kHz speech at 32 kb/s:

| | Upstream | This build |
|---|---|---|
| PESQ-WB | 4.13 | 4.15 (4.17 on Cortex-M33) |
| Encoder code + data, ESP32-S3 | 102 KB | 35 KB |
| Encoder, Xtensa with FPU (ESP32 in QEMU; the S3 is similar) | 23.6 MIPS | 7.5 MIPS |
| Encoder, RV32IMC without FPU (ESP32-C3 in QEMU; the C6 and C5 have no FPU either) | 180 MIPS | 85 MIPS |
| Encoder, Cortex-M33 with FPU (emulator, `-O2`) | 105 KB flash | 35 KB flash, 1.9 KB static RAM, 2.5 KB stack, 7.1 MIPS |

LC3 is floating point, so on the ESP32-C6 and C5 it takes about half the CPU
while recording; SBC, the default, needs 2 MIPS there.

## Updating

Copy `include/` and `src/` from the new tree, regenerate `tables.c` and
`ltpf.c` for 16 kHz / 10 ms, and keep the modification notes in the file
headers. Then run `python3 -m unittest tests.test_mg_codecs` from `esp32/` and
`zephyr/tests/run_all.sh unit`.
