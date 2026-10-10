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

# Protocols

Wire contracts shared by the gadget firmware and the Muse apps. Include these
headers; don't copy their constants into other files.

| File | What it defines |
|---|---|
| [`mgcommands.h`](mgcommands.h) | The musegadgets BLE protocol: advertising, the GATT service (Control and Data), the required standard services (Battery, Device Information, Nordic UART), commands, push-to-talk, audio formats, offline clips and the token proof. |
| [`test-vectors/mg-token-proof-v1.json`](test-vectors/mg-token-proof-v1.json) | Token proof vectors, from [`test-vectors/mg_token_proof_ref.py`](test-vectors/mg_token_proof_ref.py) (Python standard library only; run it to check the file). |

The header is plain C99 and also compiles as C++ and Objective-C, where the
enums become `NS_ENUM`s that Swift can import.

## Gadget setup over Muse Link

A musegadgets device gets its device token pair through the Muse Link BLE
setup protocol, not over mgcommands. The transport is unchanged: the setup
service `7fdd3d1c-38ea-46cf-8b46-314ecf5f240c` (RX `4d593029-28a2-4a6e-a1f0-3c2d5e8f9b01`,
TX `d75dc4ca-7b2b-4e9c-8f0a-1d2e3f4a5b6c`), its chunk framing, community
pairing v5 with a physical button press, the `pairing_encrypted` records, and
`pairing_confirmed`, which carries `sdk_token` when the firmware has one. The
reference implementation is `esp32/main/link_pairing.c`, `ble_server.c` and
`pairing_transcript.c`, with vectors in `esp32/tests/vectors/link_pairing_v5.json`.
Gadgets add:

- **`device_info`** gains two fields. `wifi` is `"required"`, `"optional"` or
  `"none"`; absent means `"required"`, which is what every firmware without
  this change sends. `mgcommands: 1` says the device also serves the
  musegadgets service. ESP32 BLE-only builds and the Zephyr gadget send
  `wifi: "none"`, ESP32 Wi-Fi boards with musegadgets `wifi: "optional"`;
  other firmware omits both.
- **`provision_v2`** still needs `access_token`, `refresh_token` and
  `token_type: "device"`. `ssid` and `password`:
  - `required`: required, as before.
  - `optional`: both absent or empty makes the provisioning **token-only**;
    a nonempty `ssid` runs the Wi-Fi flow as before.
  - `none`: must be absent or empty, else the device answers
    `error_wifi_unsupported` and stores nothing.

  `username`, `api_url`, `api_url_v2` and `noise_host` are stored unless
  `wifi` is `"none"`; token-only provisioning ignores `ota_url` and `ota_force`.
- **Token-only provisioning** stores, in one commit, the token pair, the
  token proof key K (mgcommands.h, Token proof), the setup-complete marker and
  a marker that Wi-Fi was skipped, so boot recovery keeps a setup without
  Wi-Fi. Only then does it send the encrypted status `auth_ok`; a failed
  commit sends `error_storage` and leaves nothing behind. It never sends
  `wifi_*` statuses, joins Wi-Fi or reaches a VM. Afterwards the device stops
  advertising Link setup (musegadgets advertising continues), and a new
  `pairing_client_hello` gets `error_pairing_unavailable`, as after any setup.
- **Reset** (the device's hold-to-reset or menu, or the token proof's
  `clear` after a match) erases the tokens, K and both markers and returns
  the device to Link setup advertising.

The app derives K from the access token it provisioned and the `node_id`
from `device_info`, keeps K (not the tokens) for the device, and runs the
token proof on every musegadgets connection. Apps key K by the BLE
peripheral they ran Link setup with, so gadget firmware must advertise Link
setup and the musegadgets service from the same identity address (ESP32
uses one public-address advertiser that takes turns between the two UUIDs).

Automated tests have nobody to press the button. The firmware takes the
press on its USB serial console, which asks for the same physical access:
it prints `@pair.pending` when a confirmation starts waiting and confirms on
the line `>pair.confirm` (answering `@pair.confirm confirmed`, or `none`).
ESP32 boards with the Muse UI and the Zephyr bench build have it;
`esp32/tools/mg_confirm.py` answers it during setup (`esp32/AGENTS.md`,
Automated setup tests). It changes nothing on the air.
