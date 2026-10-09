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

# AGENTS.md

How to build this firmware, test it, and flash it onto a Seeed XIAO nRF54L15.
See `README.md` for a shorter human overview.

## What this is

The Zephyr Device SDK: a freestanding Zephyr application for a push-to-talk
BLE gadget speaking the musegadgets protocol (`../protocols/mgcommands.h`,
optionally `../protocols/mgcommands-secure.h`), set up by the Muse app over
Muse Link setup (`../protocols/README.md`, Gadget setup over Muse Link).
Include those headers; never copy their constants. The build adds
`../protocols` to the include path and compiles
`../esp32/main/pairing_transcript.c`, so build from a full checkout.

| File | Role |
|---|---|
| `src/main.c` | Boot order, the app work queue, battery polling |
| `src/mg_ota.c` | Firmware updates: MCUmgr hooks, confirming a test image, handing the clip slot to OTA and back |
| `src/mg_bench.c` | Bench console (`overlay-bench.conf`): single-key commands and `>pair.confirm` on the console UART |
| `src/mg_core.c` | Protocol engine: commands, push-to-talk, settings and queue commands |
| `src/mg_session.c` | Between GATT and the core: pass-through, or all of mgcommands-secure.h |
| `src/mg_crypto.c`, `src/mg_pairing.c` | Session security's PSA Crypto primitives; stored (key_id, PK) pairs |
| `src/mg_setup.c` | Muse Link setup: the setup service's framing, dispatch, statuses, community pairing v5 phases and timeouts, button confirmation, get_device_info, token-only provision_v2 |
| `src/mg_setup_crypto.c`, `src/mg_psa.c` | Link setup crypto (P-256 ECDH, session keys, AES-256-GCM records, base64url) and SHA-256 / HMAC / HKDF, on PSA Crypto |
| `src/mg_setup_store.c` | What setup stores: tokens, proof key K and the commit record in settings (ZMS), boot recovery |
| `src/mg_token_proof.c` | mg_command_token_proof (34): challenge, response, confirm, result, clear |
| `src/mg_json.c` | The strict flat JSON reader and writer Link setup needs |
| `src/mg_identity.c` | Name, node id, MAC and device id from the identity address |
| `src/mg_ble.c` | GATT service, advertising, connection, PHY / data length, the ordered notification queue |
| `src/mg_connparam.c` | Connection parameters: 15 ms / latency 10 / 4 s, latency off while the phone streams to the gadget |
| `src/mg_throughput.c` | The throughput test (device_action throughput_test): send and receive |
| `src/mg_audio.c` | Audio thread: capture, encode, packetize, offline recording, clip download |
| `src/mg_audio_dmic.c`, `src/mg_audio_synth.c` | PDM microphone, or a 1 kHz test tone |
| `src/mg_codec.c` | SBC and LC3 encoders behind one interface |
| `src/mg_hpf.c` | The microphone's DC-blocking high-pass |
| `src/mg_queue.c` | Offline clip storage: MCUboot's `slot1_partition` on the XIAO (shared with OTA), `audio_queue_partition` elsewhere |
| `src/mg_settings.c` | Settings validation, wire encoding, persistence (Zephyr settings, ZMS) |
| `src/mg_nus.c` | Nordic UART: log mirror and read-only console |
| `sysbuild.conf`, `boards/xiao_nrf54l15_nrf54l15_cpuapp.conf` | MCUboot (swap using move, dev signing key) and MCUmgr SMP for the XIAO |
| `src/mg_led.c`, `src/mg_button.c`, `src/mg_battery.c` | LED patterns and "buzz", button events, VBAT |
| `../xplat/libsbc`, `../xplat/liblc3` | Vendored codecs (Apache-2.0), one copy shared with the ESP32 SDK, each with a provenance README |
| `tools/flash_hex.py` | One padded hex (MCUboot + signed app) for flashing the XIAO |
| `tests/host` | Host C tests (any OS) for Link setup and the token proof |
| `tests/unit` | ztest suites on native_sim |
| `tests/bsim` | BabbleSim: gadget hooks, a test central, `run.sh` |

Threads: all protocol state lives on the app work queue (`mg_app_wq()`);
BLE callbacks post events to it. The audio thread produces everything on
Data and sends `change_data_type` / `stop_mic` itself so Control and Data stay
in order. One notification FIFO feeds the stack from a TX thread. Never block
the system work queue: the Bluetooth host needs it.

## Toolchain

Tested with **Zephyr v4.4.2** and **Zephyr SDK 1.0.1** (GCC 14.3). Older SDKs
(0.16) lack the nRF54L15. Set up a west workspace outside this repository:

```sh
python3 -m venv ~/zephyrproject/.venv && source ~/zephyrproject/.venv/bin/activate
pip install west
west init -m https://github.com/zephyrproject-rtos/zephyr --mr v4.4.2 ~/zephyrproject
cd ~/zephyrproject
# Only the modules this app needs (much faster than a full update):
west config manifest.group-filter -- +babblesim
west config manifest.project-filter -- '-.*,+cmsis,+cmsis_6,+hal_nordic,+mbedtls,+tf-psa-crypto,+liblc3,+littlefs,+segger,+tinycrypt,+picolibc,+nrf_hw_models,+trusted-firmware-m,+psa-arch-tests,+babblesim_base,+babblesim_ext_2G4_libPhyComv1,+babblesim_ext_2G4_phy_v1,+babblesim_ext_2G4_channel_NtNcable,+babblesim_ext_2G4_channel_multiatt,+babblesim_ext_2G4_modem_magic,+babblesim_ext_2G4_modem_BLE_simple,+babblesim_ext_2G4_device_burst_interferer,+babblesim_ext_2G4_device_WLAN_actmod,+babblesim_ext_2G4_device_playback,+babblesim_ext_libCryptov1'
west update --narrow -o=--depth=1
pip install -r zephyr/scripts/requirements-base.txt
```

MCUboot and MCUmgr need two more projects and imgtool's Python packages:

```sh
west config manifest.project-filter -- "$(west config manifest.project-filter),+mcuboot,+zcbor"
west update --narrow -o=--depth=1 mcuboot zcbor
pip install -r bootloader/mcuboot/scripts/requirements.txt
```

The SDK: the minimal bundle plus the Arm toolchain is enough (macOS arm64
shown; pick the matching `linux-x86_64` files on Linux):

```sh
B=https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.1
curl -LO $B/zephyr-sdk-1.0.1_macos-aarch64_minimal.tar.xz
curl -LO $B/toolchain_gnu_macos-aarch64_arm-zephyr-eabi.tar.xz
tar xf zephyr-sdk-1.0.1_macos-aarch64_minimal.tar.xz -C ~
mkdir -p ~/zephyr-sdk-1.0.1/gnu
tar xf toolchain_gnu_macos-aarch64_arm-zephyr-eabi.tar.xz -C ~/zephyr-sdk-1.0.1/gnu
~/zephyr-sdk-1.0.1/setup.sh -c
```

SDK 1.0 expects toolchains under `gnu/`. An error that
`modules/crypto/mbedtls/tf-psa-crypto` is not an existing directory means the
`tf-psa-crypto` project wasn't fetched.

## Build

From the workspace (`source ~/zephyrproject/.venv/bin/activate`, and
`export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-1.0.1` if CMake can't find it):

```sh
cd ~/zephyrproject
west build --sysbuild -p -b xiao_nrf54l15/nrf54l15/cpuapp /path/to/muse-gadget-sdk/zephyr -d build-xiao
west build --sysbuild -p -b xiao_nrf54l15/nrf54l15/cpuapp /path/to/muse-gadget-sdk/zephyr -d build-xiao-secure \
  -- -DEXTRA_CONF_FILE=overlay-secure.conf
# bench build: add overlay-bench.conf ("overlay-secure.conf;overlay-bench.conf" with security)
west build --sysbuild -p -b xiao_nrf54l15/nrf54l15/cpuapp /path/to/muse-gadget-sdk/zephyr -d build-bench \
  -- -DEXTRA_CONF_FILE=overlay-bench.conf
```

The XIAO boots through MCUboot, so always build it with `--sysbuild` (or
`west config build.sysbuild True`); without it CMake stops with a message
saying so. A sysbuild directory holds two images:

| File | What |
|---|---|
| `build-xiao/mcuboot/zephyr/zephyr.hex` | MCUboot, flashed once |
| `build-xiao/zephyr/zephyr/zephyr.signed.hex` | the app, signed: flash it, or `zephyr.signed.bin` for an over-the-air update |

Flash them as one padded hex from `tools/flash_hex.py` (see Flash: the raw
hexes through `west flash` can leave the last bytes unwritten).

The version is `VERSION` (it's the Device Information firmware revision and
the image version MCUboot and SMP report); bump `PATCHLEVEL` for a test
update. Last measured (v4.4.2, SDK 1.0.1), app / MCUboot:

| Build | App flash | App RAM |
|---|---|---|
| plain | 286,488 B | 114,792 B |
| secure | 294,576 B | 117,040 B |
| MCUboot | 30,612 B of 62 KB | 17,376 B |

Muse Link setup and the token proof cost about 46 KB of flash and 10 KB of
RAM over the build with device tokens (239,904 B / 104,504 B): their own
code about 15 KB and 8 KB of RAM (mostly the 4.3 KB setup receive buffer and
1 KB transmit buffer), mbedTLS's P-256, AES-GCM, SHA-256 and HMAC about
29 KB, a 2 KB larger app work queue stack (P-256 runs on it), and GATT
caching, which PSA Crypto turns on (its database hash, 1.5 KB of RAM for
the host's long work queue); dropping device tokens saved about 2 KB of each.
Session security adds about 8 KB of flash and 2 KB of RAM on top (X25519,
the session, stored pairings, 16 PSA key slots).
MCUmgr SMP adds about 18 KB of flash and 7.5 KB of RAM; the SBC encoder is about
3.4 KB of flash and LC3 about 37 KB flash / 1.8 KB RAM (`-DCONFIG_MG_LC3=n`
drops it). The app can't outgrow its slot: it links into `slot0_partition`
(712 KB, ending where slot1 starts), and imgtool refuses an image too big for
the slot.

Board notes (`boards/xiao_nrf54l15_nrf54l15_cpuapp.*`):

- The RRAM (1524 KB, no external flash) keeps the board's standard MCUboot
  map (`nrf54l15_cpuapp_partition.dtsi`):

  | Partition | Offset | Size | Holds |
  |---|---|---|---|
  | `boot_partition` | 0x0 | 62 KB | MCUboot |
  | `slot0_partition` | 0x10000 | 712 KB | the running app |
  | `slot1_partition` | 0xC2000 | 712 KB | an uploaded update, or the offline clips |
  | `storage_partition` | 0x174000 | 36 KB | settings (ZMS): app settings, what Link setup stores |

- MCUboot runs swap using move (`sysbuild.conf`): an update runs on test
  after the next reset, the app confirms it (`boot_write_img_confirmed`) once
  BLE is up and advertising, and an image that never gets there is swapped
  back at the reset after. There's no watchdog yet, so an image that hangs
  stays until someone resets the board. Swapping 712 KB of RRAM takes a few
  seconds at boot.
- Images are signed with MCUboot's ECDSA P-256 development key
  (`bootloader/mcuboot/root-ec-p256.pem`; the build warns about it). Anyone
  can sign for that key: production builds must set
  `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` to their own key, kept out of the
  repository, and boards flashed with a dev-key MCUboot only take dev-key
  updates.
- The FPU is on (`CONFIG_FPU=y`); LC3 is float code.
- Battery: `/zephyr,user io-channels = <&adc 7>` (AIN7, P1.14), the board's
  ADC channel 7 (gain 1/4, internal reference, 12 bits, 8x oversampling). VBAT
  reaches it through a 2:1 divider (`CONFIG_MG_BATTERY_DIVIDER_RATIO`) that
  the board switches with P1.15 (`vbat_pwr`, a fixed regulator the board
  turns on at boot and leaves on). Seeed's sample uses the same channel,
  switch and ratio; it hasn't been measured on this board yet. Below 2.5 V
  (no cell, or one past use) Battery Level reads 100, as for a gadget
  without a battery; on USB with no cell the pin may also read the charger's
  output (about 4.2 V, so 100 again). The bench `s` key prints the raw mV.
- The PDM mic is `dmic_dev` (pdm20, CLK P1.12, DIN P1.13), powered by the
  board's `pdm_imu_pwr` regulator. The channel is taken as LEFT. On the
  hardware it starts with a DC offset near -1100 that drifts for seconds:
  a one-pole high-pass (`CONFIG_MG_MIC_HPF_HZ`, 30 Hz, `src/mg_hpf.c`) runs
  right after the PDM, before the gain, primed with the first sample of each
  capture so there's no settling step. Speech from 30 cm peaked near -30 dBFS
  with the default software gain (`CONFIG_MG_MIC_DEFAULT_GAIN` 50, +12 dB,
  still what set_mic_gain changes), so the PDM's own gain is raised to +10 dB
  (`CONFIG_MG_DMIC_HW_GAIN_DB`) for about -20 dBFS; the PDM clock is left to
  the driver (1.0 to 3.5 MHz requested). Not re-measured on hardware since.
- No vibration motor (`CONFIG_MG_HAPTICS` off): haptics_enabled and the
  ptt_buzz_* settings aren't listed and answer `unsupported`, and the
  activation "buzz" never runs. The typed settings list (`[supported_features,
  sub_feature, set_settings, …]`, sent with request_status) names exactly the
  settings get/set_settings accept.
- The LED shows the link (ready only with a client connected, subscribed to
  Control and Data, authenticated on secure builds, and push-to-talk on) and
  the assistant's turn (`CONFIG_MG_ASSISTANT_STATE`, `mg_command_assistant_state`):
  listening while capturing, thinking after stop_mic, then responding, done
  (a short flourish) or error (3 s) as the client reports, idle after 60 s
  without an update or on disconnect. Thinking at stop_mic comes from the
  device itself, whatever the client has sent (the iOS app never sends
  thinking); any assistant_state takes over. A client that has sent none on
  the connection gets done `CONFIG_MG_STATE_COMPAT_TIMEOUT_MS` (15 s) after
  stop_mic if nothing arrives. Changes are logged as
  `mg.ble: link <old> -> <new>`, `mg.face: <state>` and `mg.led: <pattern>`
  (the README lists the patterns).

### Muse Link setup

The gadget gets its device token pair the way Wi-Fi Muse gadgets do, over
the Muse Link BLE setup service (`src/mg_setup.c`), and follows the ESP32
reference message for message (`esp32/main/ble_server.c` dispatch and
statuses, `link_pairing.c` session, `app.c` phases):

- **Service** `7fdd3d1c-…` with RX (write, write without response) and TX
  (read, notify), `0xFE` chunk framing both ways (chunks of at most 160 bytes
  and MTU - 3; messages up to 4.3 KB in). TX reads return the last plaintext
  status, or `encrypted_status`.
- **Community pairing v5** only (`pairing_auth` `none`, epoch 0, policy
  `confirm_press`, no manufacturer attestation): `pairing_client_hello` ->
  `pairing_ready` (P-256 ECDH, the transcript from
  `esp32/main/pairing_transcript.c`, HKDF session secret, record keys and
  session id) -> encrypted `pairing_client_finished` -> `confirm_required`
  -> a press of the talk button (or the bench console's `>pair.confirm`)
  -> `pairing_confirmed`, carrying `sdk_token`
  when `CONFIG_MG_SDK_TOKEN` is set (default empty; put yours in a local conf
  file, never a committed one). Records are AES-256-GCM with the ESP32's
  nonce, AAD and counter rules. Phases time out as on the ESP32: 60 s for
  client_finished, 60 s for the press (then `pairing_confirm_timeout` and a
  disconnect), 120 s for provision_v2. While a confirmation is pending the
  press is never push-to-talk; the LED blinks very fast.
- **Plaintext**: before a session only `error_encryption_required`,
  `error_pairing_invalid_hello`, `error_pairing_unavailable` and
  `error_pairing_decrypt` go out; after `pairing_ready` plaintext commands
  are ignored.
- **get_device_info** (plaintext, before a session): node id, version,
  device id (`hatch-link:<mac>`), mac (the identity address), model
  `hatch_link`, protocol 5, auth `none`, epoch 0, policy `confirm_press`,
  `wifi: "none"`, `mgcommands: 1`. The node id is `homelink-` and the last
  six hex digits of the advertised name in lowercase (`MuseGadget-1A2B3C` is
  `homelink-1a2b3c`), matching the ESP32 scheme.
- **provision_v2** is token-only: `access_token`, `refresh_token` and
  `token_type: "device"` are required (`error_missing_credentials`); a
  nonempty `ssid` or `password` gets `error_wifi_unsupported` and stores
  nothing; other fields are ignored. Each token is at most 2048 bytes.
  The gadget derives the token proof key K (mgcommands.h, Token proof) from
  the access token and its node id, stores tokens, K and the commit record
  (`src/mg_setup_store.h`: `mg/setup/at`, `rt`, `k`, then `done` with "setup
  complete" and "Wi-Fi skipped" and the lengths; ZMS writes one entry
  atomically, so `done` is the commit point, written last and erased first),
  and only then answers `auth_ok`. A failed write sends `error_storage`,
  erases what was written and drops the session. Boot erases anything
  without a valid `done`. Logs show token lengths, never tokens or K.
- **Afterwards** Link setup advertising stops (mg only), and
  `pairing_client_hello` gets `error_pairing_unavailable`.
- **Reset**: hold the button at power-up for `CONFIG_MG_RESET_HOLD_MS` (5 s),
  or the token proof's `clear` after a match: tokens, K and the markers are
  erased and Link setup is advertised again.

Advertising (`src/mg_ble.c`): one legacy advertiser on the identity address
(the board's static random address; no privacy), so Link setup and the mg
service are found at one address, which the apps key their record by.
Until setup is complete the payload takes turns every 1.5 s between the Link
setup UUID (with the ESP32's `0xFFFF` manufacturer data) and the mg UUID
(`bt_le_adv_update_data`, the same advertiser); afterwards it is mg only.

### Token proof

`mg_command_token_proof` (34, mgcommands.h): the client sends a challenge
nonce, the gadget answers a fresh nonce from PSA and HMAC-SHA256(K, device
label || nonces), the client confirms with its own MAC, and the gadget
answers result 1 or 0 (constant-time compare). Without K a challenge is
`not_found`; confirm with nothing pending is `invalid_value`; a new
challenge restarts the exchange (and drops an earlier match). The result
lasts for the connection (reset on connect and disconnect) and gates
nothing. On a secure build it is a command like the others: refused in
plaintext (`encryption_required`) and before authentication
(`authentication_required`), and run on Encrypted Control once the session
is authenticated. `clear` after a match answers result 1, then erases the setup;
before a match it is `proof_required`. The supported_features list
includes 34.

### The BLE link

These are in `prj.conf` and `Kconfig.mg`, so every board this app builds for
(the XIAO and any other nRF54L15) gets them, not just the XIAO's files:

- **2M PHY and 251-byte PDUs.** Right after connecting the gadget asks for
  the 2M PHY (`bt_conn_le_phy_update`; `CONFIG_BT_USER_PHY_UPDATE`,
  `CONFIG_BT_CTLR_PHY_2M`), the largest data length (`CONFIG_BT_CTLR_DATA_LENGTH_MAX=251`,
  `CONFIG_BT_USER_DATA_LEN_UPDATE`) and the largest ATT MTU, and logs what it
  gets: `mg.ble: phy tx 2M rx 2M`, `mg.ble: data length tx 251 B / 2120 us …`.
  A central that starts its own PHY or data length update at the same time
  (phones often do; so does the BabbleSim test central) makes the controller
  refuse ours (logged as "already under way"); the outcome is logged all
  the same.
- **Connection parameters.** Then (once PHY and data length are done, or
  after 2 s) it asks for interval 12-12 (15 ms, exactly: iOS should not
  pick 7.5 ms), peripheral latency 10 and supervision timeout 400 (4 s):
  `CONFIG_MG_CONN_INTERVAL_MIN` / `_MAX`, `CONFIG_MG_CONN_LATENCY`,
  `CONFIG_MG_CONN_TIMEOUT`. They meet Apple's accessory rules (15 ms is
  allowed when min = max; latency ≤ 30; 2 s ≤ timeout ≤ 6 s; 15 ms × 11 × 3
  = 495 ms < 4 s). GAP's Peripheral Preferred Connection Parameters
  characteristic, which iOS reads, carries the same values
  (`CONFIG_BT_PERIPHERAL_PREF_*` default to them), and the host's own
  automatic update is off (`CONFIG_BT_GAP_AUTO_UPDATE_CONN_PARAMS=n`) so
  nothing else asks for other values. The host holds a peripheral's request
  until 1 s after connecting (`CONFIG_BT_CONN_PARAM_UPDATE_TIMEOUT=1000`, the
  Core spec's minimum pause; Zephyr's default is 5 s). A request not granted
  within 5 s is sent once more; then the central's choice stands. Every update is logged:
  `mg.ble: conn 15.00 ms latency 10 timeout 4000 ms`.
- **No skipped events while the phone sends.** Latency lets the gadget sleep
  through events when it has nothing to send. Device->phone traffic doesn't
  care: the controller cancels latency as soon as a notification is queued
  (`ull_periph_latency_cancel`), so push-to-talk audio and clip downloads go
  out at the next event. Phone->device traffic does: the central's packets
  wait for an event the gadget listens on, up to 165 ms. Zephyr's open
  controller has no way to suspend latency short of a parameter update, so
  while the phone streams to the gadget (every SMP image-group request, so
  an upload and the image list a client reads just before it; a future
  stream_audio) it asks for
  latency 0, and for latency 10 again 2 s (`CONFIG_MG_CONN_LATENCY_HOLD_MS`)
  after the last data. Logged as `mg.ble: latency off (dfu)` and
  `mg.ble: latency on`. A parameter update takes effect a few events after
  it's granted, so a short transfer gains little.
- **Checking it.** All of these lines go to the console and the Nordic UART
  mirror (`python3 esp32/tools/mg_ble_client.py log`): after a connect,
  `mg.ble: phy tx 2M rx 2M`, `mg.ble: data length …`, `mg.ble: asking for
  12-12 x 1.25 ms, latency 10, timeout 4000 ms`, then what the central
  granted, `mg.ble: conn 15.00 ms latency 10 timeout 4000 ms`; during an
  upload, `mg.ble: latency off (dfu)` and a `conn … latency 0` line, and
  `mg.ble: latency on` 2 s after it. The BabbleSim `ptt` scenario checks the
  same from the central's side (2M PHY, 15 ms, latency 10, 4 s).

### Throughput

| Direction | What limits it | Settings |
|---|---|---|
| Phone -> gadget (SMP upload) | SMP round trips, latency, then the central's packets per event | Latency 0 while an upload runs (above). SMP (`boards/xiao_*.conf`): `CONFIG_MCUMGR_TRANSPORT_BT_REASSEMBLY` with four 2475-byte buffers, so one request spans about ten ATT writes, and `CONFIG_MCUMGR_GRP_OS_MCUMGR_PARAMS` tells clients that size (smpclient fills it; nRF Connect Device Manager too). `CONFIG_IMG_BLOCK_BUF_SIZE=4096`: RRAM is written through the radio-synchronised flash driver (`CONFIG_SOC_FLASH_NRF_RADIO_SYNC_TICKER`, which waits for a gap between connection events), so bigger blocks mean fewer waits. `CONFIG_BT_CTLR_RX_BUFFERS=8` and `CONFIG_BT_BUF_ACL_RX_COUNT_EXTRA=7` (`prj.conf`) are headroom for when the host drains received PDUs more slowly than they come (during an image flash write); in BabbleSim it keeps up with the defaults too. |
| Gadget -> phone (clip download, live audio) | the central's packets per event; then 2M airtime | Up to ten notifications handed to the stack at once (`TX_INFLIGHT` in `mg_ble.c`) within `CONFIG_BT_CONN_TX_MAX` and `CONFIG_BT_BUF_ACL_TX_COUNT` (12, also the controller's TX buffers). One 251-byte PDU and its acknowledgement take 1.4 ms at 2M, so a 15 ms event carries at most about ten: 160 KB/s. A Mac took about four per event (65 KB/s) with six in flight. Live SBC is 7.5 KB/s. |

RAM for this: about 17 KB (the four SMP buffers are 10 KB, the image block
buffer 3.5 KB more, RX buffers 4 KB). In BabbleSim (2M PHY, 15 ms, 251-byte
PDUs both ways, latency 0) the throughput test moves 171 KB/s gadget ->
central and 163 KB/s central -> gadget: the airtime limit, about ten PDUs
an event.

Measure it with the bench client: `dfu` prints the upload rate and each
phase, `queue --download` each clip's download rate, and the gadget logs
the `mg.ble: latency off (dfu)` / `latency on` pair around an upload.

Raw link throughput: `device_action throughput_test` (mgcommands.h,
Throughput test; `src/mg_throughput.c`). Send streams numbered MTU-3 byte
notifications from a thread of its own, the notification queue kept full;
receive counts the client's Data writes with latency off for as long as
they come. Each logs `mg.ble: throughput send|receive: … B in … packets, …
ms (… B/s), … gaps` and reports it to the client. `python3
esp32/tools/mg_ble_client.py bench --send 10 --recv 10` runs both and
prints the rate seen on each side, losses, and packets per connection event
(from how notifications bunch up on arrival).

Kconfig options live in `Kconfig.mg` (`CONFIG_MG_*`): SBC bitpool, LC3 frame
size, audio source, mic gain, queue size, the setup reset hold, security,
stored pairings, firmware updates and SMP access, the bench console.

### Firmware updates (MCUmgr SMP)

The SMP GATT service sits beside the mg service, DIS, BAS and NUS, with the
image group (list, upload, test, confirm, erase) and the OS group (echo,
reset, info), so nRF Connect Device Manager, `mcumgr` / `smpmgr` and
`esp32/tools/mg_ble_client.py images|dfu` can update it. Upload
`build-*/zephyr/zephyr/zephyr.signed.bin`. The OS group's MCUmgr parameters
report 2475-byte buffers, four of them; the bench client fills each request
and keeps two in flight (Zephyr's SMP transport handles queued requests in
order, one buffer holding the response and one the request being
reassembled).

`CONFIG_MG_BENCH_NO_CONFIRM` (bench only) makes an image never confirm
itself, so MCUboot swaps the previous one back at the next reset: build a
test image with it to show a revert, never ship it.

Who may use it (`MG_SMP_ACCESS`): plain builds are open to any connected
client. Secure builds default to `MG_SMP_ACCESS_SESSION`: every SMP command
is refused (`MGMT_ERR_EACCESSDENIED`) unless the connection carries an
authenticated mgcommands-secure session, so only a paired client that has
proved its key (the Muse app) can update; generic SMP tools can't. BLE-level
pairing (`MCUMGR_TRANSPORT_BT_PERM_RW_AUTHEN`) would be the alternative for
tools that can't speak the session.

### Offline clips and updates share slot1

The clip queue (`src/mg_queue.h`) lives in `slot1_partition`, which updates
need too:

- **Clips own it** while MCUboot needs nothing there. The queue never writes
  the last 12 KB (`CONFIG_MG_QUEUE_SLOT1_TRAILER`; MCUboot's trailer for 178
  sectors is 8.6 KB), and offset 0, where MCUboot looks for an image header,
  only ever holds a clip header (`MGQ1`) or blank cells. That leaves 700 KB,
  about 90 s of SBC.
- **An upload's first chunk hands it to OTA** (img_mgmt's DFU-started hook,
  before anything is written): recording stops, the clips are dropped and
  logged (`firmware update takes the clip store: N clip(s), B bytes
  dropped`). The app should download clips before updating.
- **OTA keeps it** while an upload runs, an upgrade is pending (test,
  permanent or revert), or the running image is on test (MCUboot may still
  swap back to the old image now in slot1). Meanwhile the queue reports
  itself off with capacity 0, its other commands answer `busy`, and presses
  without a phone are dropped and logged.
- **The queue takes it back** at boot or when the app confirms itself, once
  none of that holds: an old image, an abandoned upload or an image uploaded
  but never marked are discarded by blanking the image header and the
  trailer (RRAM needs no erase; the rest is overwritten by clips).

### Bench build

`overlay-bench.conf` (`CONFIG_MG_BENCH`) adds single-key commands on the
console UART (uart20, the XIAO's USB serial port, 115200 baud): `d` / `u`
press and release the talk button through the real button path, `s` prints
link state, push-to-talk, battery mV and %, image version and whether it's
confirmed, and the queue (owner, clips, bytes). Normal builds keep the
console output-only.

For unattended setup tests it also takes the ESP32 console's line
`>pair.confirm`: the button's setup press (`mg_setup_button()`, on the app
work queue), never push-to-talk, answered `@pair.confirm confirmed` or
`@pair.confirm none` when nothing waits; `@pair.pending` goes out when a
confirmation starts waiting. `../esp32/tools/mg_confirm.py PORT` answers it
while the Muse app or `mg_ble_client.py setup` drives the other side, or
`mg_ble_client.py setup --confirm-serial PORT` does both (`esp32/AGENTS.md`,
Automated setup tests). `../esp32/tests/test_mg_confirm.py` checks the tool
and this side of it. `d` then `u` also confirms (it is the real button), but
pushes to talk when nothing waits, so rigs use the line.

## Flash

The XIAO flashes through its on-board SAMD11, a CMSIS-DAP probe that also
carries the console UART; not nrfutil, which wants a J-Link.

The RRAM is written in 16-byte words. OpenOCD's `nrf54l-load` (the board's
own config, used by `west flash`) writes through the memory bus, and on this
board it left a signed image's final partial word (an odd-length signature
tail) unwritten, and an earlier firmware's bytes in the primary slot's
trailer, which MCUboot then misreads. So don't flash the raw hexes; make one
padded hex:

```sh
python3 tools/flash_hex.py ~/zephyrproject/build-xiao -o mg-xiao.hex           # reflash
python3 tools/flash_hex.py ~/zephyrproject/build-xiao -o mg-xiao-first.hex --fresh  # first flash
```

It merges MCUboot and the signed app, pads every segment to whole 16-byte
words with 0xff, and blanks the last 16 KB of slot0 (MCUboot's swap state);
`--fresh` also blanks slot1's header and trailer and the settings partition
(drops settings and what Link setup stored), which replaces a chip erase on
a board fresh from the factory.

Then flash it with either tool:

- **pyOCD** (`pip install pyocd`; the built-in `nrf54l` target needs no
  pack, and it writes through a proper RRAM flash algorithm):

  ```sh
  pyocd list                                       # the probe's unique id
  pyocd flash -t nrf54l -u <probe id> mg-xiao-first.hex
  ```

- **OpenOCD** with the board's config (no `target/nordic/nrf54l.cfg`
  needed; the Zephyr SDK's OpenOCD or Homebrew's 0.12 both work):

  ```sh
  S=~/zephyr-sdk-1.0.1/hosttools/opt/openocd/share/openocd/scripts
  ~/zephyr-sdk-1.0.1/hosttools/usr/bin/openocd -s $S \
    -f ~/zephyrproject/zephyr/boards/seeed/xiao_nrf54l15/support/openocd.cfg \
    -c "adapter serial <probe serial>" -c init -c "targets nrf54l.cpu" -c "reset init" \
    -c "nrf54l-load mg-xiao-first.hex" -c "reset init" -c "verify_image mg-xiao-first.hex" \
    -c "reset run" -c shutdown
  ```

A chip with access port protection on needs a mass erase first, which wipes
everything: `pyocd erase -t nrf54l -u <probe id> --mass`, or with OpenOCD
`-c init -c nrf54l_mass_erase -c shutdown` (the board config also recovers a
locked chip by itself when it can't examine it). Updates over BLE (SMP) don't
have the alignment problem: img_mgmt writes through the flash driver.

The console is on the USB serial port at 115200 baud (uart20). A healthy boot
logs `Muse Gadget <version> starting` and `advertising as MuseGadget-XXXXXX`.
Holding the button while powering up, for 5 s, resets setup (`setup
complete`/`half-written setup` and `not set up` lines tell what boot found).
With session security, holding it at power-up (for any time) also turns on
pairing mode (the first pairing needs no button hold).

## Tests

`tests/host` runs anywhere with a C compiler and mbedTLS (Homebrew
`mbedtls`, Debian `libmbedtls-dev`; found with `pkg-config mbedcrypto`, or
set `PSA_CFLAGS` / `PSA_LIBS`), with AddressSanitizer and UBSan:

```sh
python3 tests/host/run.py       # -v for the firmware's log lines
```

The other two suites need Linux: native_sim and BabbleSim don't run on macOS, and
BabbleSim images are 32-bit x86 host programs (the Zephyr BLE controller is
not 64-bit clean). `tests/run_all.sh` runs everything inside Linux with
`ZEPHYR_BASE`, `BSIM_OUT_PATH` and `BSIM_COMPONENTS_PATH` set:

```sh
tests/run_all.sh          # unit (4 configurations) + BabbleSim (4 scenarios)
tests/run_all.sh unit
tests/run_all.sh bsim
```

BabbleSim itself is built once in the workspace:

```sh
cd ~/zephyrproject/tools/bsim && ln -sf components/common/Makefile Makefile
BSIM_OUT_PATH=$PWD BSIM_COMPONENTS_PATH=$PWD/components make everything -j
```

(`libCryptov1` may fail to build; it is only needed for link-layer
encryption, which these tests don't use.)

### From a Mac

Use an x86-64 Linux container. With colima (a `vz` VM works; Rosetta is
optional, QEMU user emulation is enough):

```sh
colima start zephyr --arch aarch64 --vm-type vz --cpu 8 --memory 16 --disk 80
docker --context colima-zephyr run --privileged --rm tonistiigi/binfmt --install amd64
docker --context colima-zephyr build --platform linux/amd64 -t mgzephyr86 tests/docker
```

Copy the workspace into a volume once (bind mounts through sshfs are slow and
can serve stale file sizes), then stream this directory in for each run:

```sh
docker --context colima-zephyr run --rm -v ~/zephyrproject:/src:ro -v zws86:/zp \
  mgzephyr86 rsync -a --exclude .venv /src/ /zp/
tar -C /path/to/muse-gadget-sdk --no-xattrs -cf - zephyr protocols xplat | \
  docker --context colima-zephyr run -i --rm --platform linux/amd64 -v zws86:/zp \
  -e ZEPHYR_BASE=/zp/zephyr -e BSIM_OUT_PATH=/zp/tools/bsim \
  -e BSIM_COMPONENTS_PATH=/zp/tools/bsim/components -w /zp mgzephyr86 \
  bash -c 'mkdir -p /work && tar -xf - -C /work && /work/zephyr/tests/run_all.sh'
```

Build BabbleSim inside that container the first time (command above, paths
under `/zp`). Under emulation a Python step occasionally segfaults; rerun.
If the VM has no DNS, point `/etc/resolv.conf` in it at the colima gateway
(`192.168.5.2`); behind a proxy pass `--build-arg https_proxy=...`.

### What the tests cover

- `tests/host` (any OS): the Link setup crypto against
  `../esp32/tests/vectors/link_pairing_v5.json` (keys, ECDH, transcript,
  hash, session secret, record keys, session id, AAD, client_finished both
  ways, tampering) and RFC 5869; the JSON reader and writer (escapes,
  surrogates, depth, duplicates, malformed input); Link setup driven as the
  app drives it, with the gadget's identity and ephemeral key set to the
  vector's so pairing_ready must match it: device_info, the plaintext rules,
  every phase, the press, sdk_token, provision_v2's refusals, token-only
  provisioning and what it stores, K against the proof vectors,
  error_pairing_unavailable afterwards, a failed write at each of the four
  steps rolled back, boot recovery, the timeouts, replay, small MTUs, reset;
  and the token proof frame by frame against
  `../protocols/test-vectors/mg-token-proof-v1.json` and its rules.
- `tests/unit` (ztest, native_sim): settings (validation, persistence,
  per-connection reset), the protocol engine through `mg_session` with the
  audio thread faked, SBC and LC3 round trips (frame format, SNR), the clip
  queue on NOR-style flash and on RRAM-style rewritable flash
  (`CONFIG_FLASH_SIMULATOR_EXPLICIT_ERASE=n`) including power-loss recovery,
  the queue in MCUboot's slot1 (the `slot1` configuration): it never writes
  the image magic at offset 0 or the trailer even when full, and the slot
  changes hands (clips, upload, an image across a reboot, reclaimed, clips
  again) with the ownership rules; the link states behind the LED
  (unsubscribe, disconnect, reconnect without subscribing), assistant_state
  and the turn after stop_mic, the typed settings list with and without a
  vibration motor (the `slot1` configuration has none, like the XIAO), and the
  mic high-pass (a DC step settles within 50 ms, a primed start, speech
  passes), the connection parameters (Apple's rules, the request after
  PHY and data length or 2 s, one retry, latency off for an upload's
  duration plus the hold), and the throughput test (packet format, gaps,
  send pacing and its report, receive, durations, refusals), and the token
  proof through the engine with Zephyr's PSA Crypto and ZMS (every vector
  frame by frame, clear, the rules, per-connection results) plus Link setup's
  device_info and pairing_ready against the vector (`test_proof.c`), and
  session security: every value in
  `../protocols/test-vectors/mgcommands-secure-v1.json` (the header is
  generated from the JSON at build time), a whole session byte for byte
  against the vectors, the Rules (ordering, replay, failures, pairing
  mode, attempts, timeout, unpair), and the token proof on Encrypted
  Control after authentication (refused in plaintext and before it).
- `tests/bsim`: the real firmware image (synthetic audio source, built
  without MCUboot: the queue is on its own `audio_queue_partition`) against a
  test central on simulated radios. `ptt`: scan by UUID and name, MTU, the link (2M PHY, and the
  15 ms / latency 10 / 4 s parameters the gadget asks for), the throughput
  test for a second each way (every packet checked, counts matching the
  report), DIS
  and BAS reads, request_status, settings, a button press streamed as SBC
  (decoded and checked for the tone), then client-driven LC3. `offline`:
  enable the queue, disconnect, two presses, reconnect, clip_info, download
  and decode both clips, clear. `setup`: both advertising payloads from one
  address, then the central as the Muse app: get_device_info, hello, the
  transcript hash and session id it computes itself, client_finished,
  confirm_required, the gadget's hook presses the button, pairing_confirmed,
  Wi-Fi refused, token-only provision_v2, auth_ok; the token proof on the
  same connection; reconnect (mg only advertised, hello refused, proof
  matches), clear, reconnect (Link setup advertised, no token). `secure`:
  key exchange, physical_confirm pairing with the button, encrypted
  commands, reconnect with prove, the token proof refused in plaintext and
  answered on Encrypted Control, replay rejected.
- The unit tests print the encoded streams (`MGSTREAM` lines) for decoding
  with other decoders.

## Say Muse, never Hatch

Anything a person reads (logs, docs, Kconfig prompts, the advertised name)
says Muse. Don't use `hatch` in file names or identifiers.

## Before you hand back work

1. Both board builds (plain and `overlay-secure.conf`, with `--sysbuild`) pass.
2. `tests/host/run.py` passes, and `tests/run_all.sh` on Linux.
3. New files carry the Apache-2.0 header; vendored code keeps its own.
