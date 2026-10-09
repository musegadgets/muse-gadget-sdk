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

# Zephyr Device SDK

A push-to-talk Bluetooth gadget for Muse, on the **Seeed XIAO nRF54L15 Sense**.
Hold the button and talk: the gadget streams your voice to the Muse app over
Bluetooth LE. With no phone around, it keeps what you say and hands it over
the next time the app connects.

It speaks the **musegadgets** BLE protocol in [`../protocols`](../protocols),
so the same Muse app that talks to the ESP32 gadgets talks to this one. The
firmware is a plain [Zephyr](https://zephyrproject.org) application: port it
to another Zephyr board with a button, an LED and a PDM microphone.

> **Note:** This is for experienced developers who are comfortable hacking on
> device firmware and using SDKs.

## What you need

- **A Seeed XIAO nRF54L15 Sense** (the Sense variant has the microphone).
  The plain XIAO nRF54L15 builds too, with a test tone instead of your voice.
- **A USB-C cable** that carries data.
- **A computer** running macOS or Linux, with the Zephyr toolchain (see
  [`AGENTS.md`](AGENTS.md) for the exact versions and commands).
- **The Muse app** on your phone.

## What it does

- **Push-to-talk.** Press the user button: the app gets a button-down
  gesture, the LED lights, and the microphone streams 16 kHz mono audio until
  you let go (DC-blocked, with the PDM's own gain at +10 dB so speech from
  arm's length lands near -20 dBFS). The app can also start and stop the
  microphone itself.
- **Codecs.** SBC by default (16 kHz mono, 60 kb/s), LC3 on request (32 kb/s).
  Both are optimized builds of Google's open source codecs, in
  [`../xplat`](../xplat) (one copy, shared with the ESP32 SDK).
- **Offline clips.** Turn on the audio queue from the app, and utterances you
  record while no phone is connected are saved to the board's flash (about
  90 seconds of SBC) until the app downloads and clears them. They live in
  the spare firmware slot, so a firmware update drops them: download first.
- **Firmware updates over Bluetooth.** MCUboot with test, confirm and revert:
  nRF Connect Device Manager (or any MCUmgr client, or
  `esp32/tools/mg_ble_client.py dfu`) uploads a signed image; it runs on test
  after the next reset and keeps itself once it's up and advertising, or
  MCUboot swaps the old one back. Secure builds only take updates from a
  client with an authenticated session.
- **Settings** set by the app (codec, queue) survive a restart. Push-to-talk
  is per connection, as the protocol says. The gadget lists exactly the
  settings it accepts.
- **Setup with the Muse app.** Until it is set up, the gadget also offers
  Muse Link setup, the same Bluetooth setup the Wi-Fi gadgets use (community
  pairing v5): the app finds it, you press the button to confirm, and the app
  sends a device token pair for its node id (`homelink-` and the end of its
  name). The gadget tells the app it has no Wi-Fi, so the app skips that
  step. It stores the tokens and a proof key derived from them, then stops
  advertising setup. Set your SDK token with `CONFIG_MG_SDK_TOKEN`: setup
  hands it to the app.
- **Token proof.** On every connection the app checks that the gadget holds
  the token it set up, without either side sending it (a challenge and
  response on a key derived from the token, mgcommands command 34). The app
  can also erase the gadget's token that way when you forget it.
- **Reset.** Hold the button while plugging the board in, and keep holding for
  5 seconds: the gadget forgets its tokens and is ready for setup again.
- **The radio link.** The gadget asks for the 2M PHY and 251-byte packets
  as soon as a phone connects, then for a 15 ms connection interval with
  peripheral latency 10 and a 4 s timeout (within Apple's rules), so it
  sleeps between events when it has nothing to say. While the phone sends it
  something big (a firmware update) it turns latency off so
  no event is skipped, and back on 2 s after. Watch it on the Nordic UART
  mirror (`esp32/tools/mg_ble_client.py log`) or the serial console:
  `mg.ble: phy tx 2M rx 2M`, `mg.ble: conn 15.00 ms latency 10 timeout 4000
  ms`, `mg.ble: latency off (dfu)`, `mg.ble: latency on`.
- **Standard services**: Battery (read from the battery if one is connected,
  100% on USB without one), Device Information, a Nordic UART console that
  mirrors the log, and MCUmgr's SMP service.
- **Optional session security** (`CONFIG_MG_SECURE`, see
  [`../protocols/mgcommands-secure.h`](../protocols/mgcommands-secure.h)):
  an encrypted, authenticated session above Bluetooth, paired by pressing the
  button when the app asks. On a secure build the token proof (and every
  other command but request_status) runs on Encrypted Control after
  authentication.

## The LED

| Pattern | Meaning |
|---|---|
| short blink every 2 s | no app connected (advertising) |
| half-second blink every 2 s | the app is connected but not ready: not subscribed yet, or push-to-talk off |
| short blink every 4 s | ready: press to talk |
| on | listening: streaming your voice |
| blinking twice a second | thinking (after you let go, until the reply starts) |
| mostly on, flickering | responding |
| three quick blinks | done |
| long blinks for 3 s | something went wrong |
| fast blink | recording an offline clip |
| double blink | pairing mode (secure builds) |
| very fast blink | press the button to confirm setup with the Muse app, or to accept pairing (secure builds) |

After you let go the LED shows thinking until the app reports the reply
(responding, then done, or error); with an app that never reports it, it
shows done after 15 s. Each
change is logged (`mg.ble: link …`, `mg.face: …`, `mg.led: …`) on the console
and the Nordic UART mirror. The board has no vibration motor, so it doesn't
offer the vibration settings.

## Build it

With the toolchain set up as in [`AGENTS.md`](AGENTS.md):

```sh
west build --sysbuild -b xiao_nrf54l15/nrf54l15/cpuapp path/to/muse-gadget-sdk/zephyr -d build-xiao
python3 path/to/muse-gadget-sdk/zephyr/tools/flash_hex.py build-xiao -o mg-xiao.hex
pyocd flash -t nrf54l mg-xiao.hex
```

`--sysbuild` builds MCUboot alongside the app; the board doesn't boot
without it. Flash through the XIAO's on-board CMSIS-DAP probe (its SAMD11)
with pyOCD or OpenOCD, from the one padded hex `tools/flash_hex.py` makes
([`AGENTS.md`](AGENTS.md) has the commands).
Add `-- -DEXTRA_CONF_FILE=overlay-secure.conf` to the build for session
security, and `overlay-bench.conf` for a console that takes single-key
commands (talk button, status) and `>pair.confirm`, which confirms the Muse
app's setup instead of a press, for unattended tests
(`../esp32/tools/mg_confirm.py`). On a secure build, hold the button while
plugging the board in to let a new phone pair (the first phone can pair
without that); keep holding for 5 s to reset setup as well.

Images are signed with MCUboot's development key, which anyone has: build
production firmware with your own (`SB_CONFIG_BOOT_SIGNATURE_KEY_FILE`).

## Tests

`python3 tests/host/run.py` checks Muse Link setup and the token proof on
any computer with a C compiler and mbedTLS: the setup crypto against the
ESP32's vectors, every setup step as the app drives it, storage and its
rollback, and the token proof against the protocol's vectors. Unit tests run
on Zephyr's `native_sim`, and end-to-end tests run two simulated radios in
[BabbleSim](https://babblesim.github.io): the gadget and a test phone that
connects, talks push-to-talk, downloads offline clips, and sets the gadget
up over Muse Link and runs the token proof. Those need Linux;
[`AGENTS.md`](AGENTS.md) shows how to run them from a Mac with a container.

## License

Apache 2.0. See [`../LICENSE`](../LICENSE). The codecs in
[`../xplat`](../xplat) are Apache 2.0 from Google, with their own license files and provenance notes.
