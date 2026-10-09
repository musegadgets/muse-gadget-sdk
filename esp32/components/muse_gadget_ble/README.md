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

# muse_gadget_ble

The device side of the musegadgets BLE protocol
([`protocols/mgcommands.h`](../../../protocols/mgcommands.h), and with
`CONFIG_MUSE_GADGET_BLE_SECURE`
[`protocols/mgcommands-secure.h`](../../../protocols/mgcommands-secure.h)): a
phone, browser or gateway connects over BLE, turns on push-to-talk, and gets
each utterance as SBC or LC3 frames. It's off unless
`CONFIG_MUSE_GADGET_BLE_AUDIO` is set; the Waveshare S3 1.75C overlay turns it
on, with session security. The gadget's device
tokens come from Muse Link setup ([`protocols/README.md`](../../../protocols/README.md),
Gadget setup over Muse Link), and the token proof checks on each connection
that the app and the gadget hold the same one.

## Files

| File | What it does | Host-tested |
|---|---|---|
| `mg_proto.c` | The protocol for one connection, with no BLE stack in it: commands, settings, capture, gestures, the clip queue commands, playback, the token proof, and session security (key exchange, encrypted frames, authenticate, pairing, the Rules) | `test_mg_ble`, `test_mg_token_proof`, `test_mg_secure` |
| `mg_crypto.c` | Suite 1 on PSA Crypto: X25519, SHA-256, HKDF, HMAC, AES-256-GCM | `test_mg_secure` (the JSON vectors) |
| `mg_token_proof.c` | The token proof's crypto on PSA Crypto: K by HKDF-SHA256, the HMAC-SHA256 tags, a constant-time compare (`include/mg_token_proof.h`; the host tests use `tests/mg_token_proof_ref.c`) | `test_mg_token_proof` (the JSON vectors) |
| `mg_codec.c` | SBC and LC3 encoders at 16 kHz mono, whole frames, `change_data_type` parameters | `test_mg_codecs` |
| `mg_queue.c` | The offline clip queue on a flash partition, safe against resets | `test_mg_ble` |
| `mg_route.c` | Where a press goes: BLE, Wi-Fi, the queue, or nowhere | `test_mg_ble` |
| `mg_play.c` | Playback: the ring the client's frames go into (flow control, prebuffer, underruns, drain and drop), and the decoder (SBC, LC3, PCM to 16 kHz) | `test_mg_play`, `test_mg_ble`, `test_mg_ble_client` |
| `mg_resample.c` | Rational polyphase resampler (8, 32, 44.1 and 48 kHz to 16 kHz), integer arithmetic | `test_mg_play` |
| `mg_ble.c` | NimBLE: the GATT services, GAP events, a worker task, NVS (settings, pairings), the Nordic UART log mirror, Battery and Device Information | built only |
| `mg_voice.c` | The voice transport layer the push-to-talk loops call (`include/mg_voice.h`) | built only |

The codecs are vendored once for both SDKs, in
[`../../../xplat/libsbc`](../../../xplat/libsbc) and
[`../../../xplat/liblc3`](../../../xplat/liblc3) (Google's,
Apache-2.0); [`../libsbc`](../libsbc) and [`../liblc3`](../liblc3) build them.

## How it fits with Home Link's BLE

Home Link owns the NimBLE host (`main/ble_server.c`), for setup, community
pairing and provisioning, and Muse's phone setup service already shares it as
a *companion*. This is a second companion (`main/mg_glue.c` registers it):

- **One host, one GATT database.** Its services join Link's at startup, and it
  sees every GAP event. There's one connection at a time, so while an mg client
  is connected nothing else can connect, and the other way round.
- **Advertising.** Legacy advertising holds one 128-bit UUID. Link's server
  advertises the musegadgets UUID (name in the scan response, 100-150 ms
  interval) whenever this wants it: after setup, that's always while no one's
  connected. While Link's own setup advertising is also on (unpaired, or Muse's
  phone setup switched on), the two payloads take turns every 1.5 s, so both
  apps find the device. Clients can also filter on the `MuseGadget` name.
- **The stack stays up.** Link normally shuts BLE down once setup is done
  (full-UI boards with PSRAM keep it idle for phone setup). With this on, it
  starts the server after setup too and only stops setup advertising.
- **Security doesn't mix.** Link's setup commands keep their own encryption
  and refuse to run once set up. Muse's phone setup keeps requiring an
  authenticated bonded link. The musegadgets characteristics need no BLE
  pairing, as the protocol says; session security runs above GATT. Without
  it the token proof says the gadget holds the token the phone set up, not
  that the link is secure.
- **One address.** Link setup and the musegadgets UUID advertise from the
  same public address (one advertiser taking turns), which apps rely on to
  match the gadget they set up with the one they connect to.

## BLE only

`CONFIG_MUSE_GADGET_BLE_STANDALONE` turns a board into a musegadgets gadget
and nothing else, for boards without PSRAM (the Waveshare C6 profile `c6ble`)
or for testing the apps. `app_run()` hands over to `run_ble_standalone()`
before Wi-Fi: Wi-Fi, the Muse session, the tunnel and OTA never start, and
Muse's phone setup service and Wi-Fi keeper aren't registered. Link setup
runs without Wi-Fi (`device_info` says `wifi: "none"`): until the gadget is
set up it advertises setup, taking turns with the musegadgets UUID, and takes
community pairing v5 (the talk button confirms), token-only `provision_v2`,
`device_info` and `unpair`; `wifi_scan` answers `error_wifi_unsupported` and
OTA isn't there. Set up means the setup marker and the proof key; a boot that
finds anything less (a power cut midway, or a setup from Wi-Fi firmware)
erases it and starts setup again. Afterwards only the musegadgets UUID
advertises. The routing policy
is BLE only, the offline queue is on by default (a client can turn it off),
and without PSRAM a queued clip is staged in internal RAM, up to about 8 s of
SBC. The screen follows the client's state (`mg_ble_link()`, `mg_link.h`):

| State | Shown | When |
|---|---|---|
| never | `OPEN MUSE APP` | no client since boot and no proof key: nobody has set it up |
| disconnected | `DISCONNECTED` | no connection |
| connecting | `CONNECTING` | connected, not yet subscribed to Control and Data (or, secure, not authenticated) |
| session | `APP CONNECTED` | the client's session is up, push-to-talk off |
| ready | `READY` | ...and push-to-talk on: the only state with the mic and speaker icons, and a highlighted Bluetooth icon |

The state is re-read after every GAP event (connect, any disconnect,
subscribe or unsubscribe) and every command, and each change is logged as
`mg.ble: link <old> -> <new>`, with the label as `mg.face: label <LABEL>`,
both on the Nordic UART mirror. The gadget name stays on screen until a
client's session is up.

A phone that keeps the link and its subscriptions after the app is gone
(backgrounded on iOS, or held by the system) still looks ready: nothing on
the link says the app left. Telling that apart needs a liveness signal from
the client, which the protocol doesn't define yet.

## The voice transport layer

Push-to-talk loops (`components/muse/muse_voice.c` on boards with the full UI,
`main/voice.c` on the Voice PE) ask `mg_voice_route()` once per press:

| Policy (`CONFIG_MUSE_GADGET_BLE_ROUTE_*`) | Client ready and push-to-talk on | Else, Wi-Fi up | Else, queue on and no client connected | Else |
|---|---|---|---|---|
| Auto (default) | BLE | Wi-Fi | queue | Wi-Fi (it saves the note or says why not, as before) |
| BLE only | BLE | queue if on, else refused | queue | refused |
| Wi-Fi only | Wi-Fi | Wi-Fi | Wi-Fi | Wi-Fi |

"Ready" means connected, subscribed to Control and Data (the encrypted ones on
a secure build), and authenticated on a secure build. Wi-Fi turns run exactly
as without this option. On BLE the loop streams the pre-roll and the mic into
`mg_voice_audio()` until release. A client's `start_mic` wakes the loop to
capture until `stop_mic`. Every capture ends with a `stop_mic` notification,
whichever side started or stopped it. Every press and release goes to a ready
client as a gesture, whatever the route.

**Haptics.** Only a board with a vibration motor (`mg_ble_platform_t.vibrate`)
lists `haptics_enabled` and the `ptt_buzz_*` settings and plays the
activation buzz; no board has one yet, so they answer `unsupported`
everywhere. `request_status` ends with the typed list
`[supported_features, sub_feature, set_settings, ...]` naming every setting
the board accepts: `spec_version`, `push_to_talk_enabled`, `audio_codec`,
plus `audio_queue_enabled` with a queue and `speaker_volume` with a speaker.

## The face (assistant_state)

A board with a display or status light (`mg_ble_platform_t.assistant_state`:
the boards with the full UI and the Voice PE) lists `mg_command_assistant_state` and runs
the Wi-Fi path's sequence for a BLE turn: listening while push-to-talk
captures, thinking once it ends (the client's reply is coming), then what
the client says: responding shows speaking, done the happy animation then
idle, error the error face for 3 s then idle, idle cancels. While a client's
audio plays (stream_audio) the face shows speaking, with the mouth following
the audio, and goes back to the last state after; a state that arrives
during playback applies once it ends. Thinking or responding with no update
for 60 s, or a disconnect, returns to idle. An utterance saved to the
offline queue ends happy, since nobody is answering, and a capture the
client drove with `start_mic` returns to the face it had (the client sends
thinking if a turn follows). Boards with the full UI apply this in the voice loop
(`components/muse/muse_voice.c`), the Voice PE on its LED ring
(`main/voice.c`). Every face change of these turns is logged as
`mg.face: face <mode>` (`ring <state>` on the Voice PE), which the Nordic
UART mirror carries: `tools/mg_ble_client.py ptt` and `state` print them.

## Audio playback

A board with a speaker lists `mg_command_stream_audio` (29) and plays what
the client streams, as the protocol's Audio playback section says. The Muse
boards have one (`speaker_write` in `main/mg_glue.c`, through
`muse_audio_write`, so the speaker volume and mute settings apply); the
Voice PE doesn't offer it yet.

- **Capabilities:** SBC (mono; 16, 32, 44.1 or 48 kHz), LC3 (16 kHz, 10 ms,
  20-400 byte frames; only where `CONFIG_MUSE_GADGET_BLE_LC3` builds the
  decoder) and PCM (s16le mono, 8 or 16 kHz: faster PCM is over the
  256 kb/s maximum). Sample rates 16000 (the default), 8000, 32000, 44100
  and 48000; one channel. Rates other than 16 kHz are resampled to the
  speaker's 16 kHz by `mg_resample.c` (Blackman-windowed sinc, cut at 92%
  of 8 kHz, over 70 dB of alias rejection in the tests). The capabilities
  aren't per codec, so a combination a codec can't do (LC3 at 48 kHz, SBC at
  8 kHz) gets `invalid_value` at start_streaming.
- **Buffer and flow control:** a `CONFIG_MUSE_GADGET_BLE_PLAY_BUFFER` ring
  (16 KB: 2 s of 60 kb/s SBC, 500 ms at the maximum bitrate), allocated at
  start_streaming and freed after the stream, in PSRAM when there is some.
  buffer_update reports its free bytes and the total received, in reply to
  start_streaming and request_buffer_update, and from the worker every
  100 ms when the space changed, at least every 250 ms. Data writes go into
  the ring straight from the NimBLE host task, so none is lost to a full
  queue or overtakes the Control command before it. A write that isn't
  whole frames of the stream's format, or doesn't fit, is dropped whole (so
  the frames after it still line up) and counted as received and dropped.
- **Playing:** the `mg_play` task (priority 7, above the voice loop) pops
  about 20 ms of whole frames, decodes and resamples them outside the lock,
  and writes 20 ms chunks to the speaker, which paces it. It starts once it
  holds 100 ms, plays silence on an underrun and waits for 100 ms again,
  and ends a stream the client went quiet on without stop_streaming after
  10 s (stop_streaming drop). stop_streaming keep plays out the buffer and
  then notifies keep; drop empties it and notifies drop at once.
- **Push-to-talk wins:** the voice loop claims the audio path for its own
  turns, chirps and bench tests (`mg_voice_claim`): a stream in progress
  ends with stop_streaming drop, and start_streaming gets `busy` until the
  loop is idle. Disconnecting ends a stream silently. While a client's
  audio plays, the voice loop keeps the codecs powered and keeps it out of
  the push-to-talk pre-roll.
- **Secure builds:** the frames come on Encrypted Data after
  authentication; plain Data is ignored.
- **Speaker volume:** `mg_setting_parameter_speaker_volume` (10) is the
  Muse speaker setting, through `main/mg_glue.c`: get returns the volume, or
  0 while the speaker is off; set 0 turns it off (the volume is kept), any
  other value turns it on at that volume. The board persists it. Boards
  without a speaker answer `unsupported`. With the speaker off, a stream is
  accepted and paced but plays silence.
- **Stats:** each stream ends with three `mg.play` log lines, mirrored on the
  Nordic UART service: frames in and decoded, bad frames, underruns and
  silence; bytes received and dropped, and milliseconds played; and the RMS
  and peak of the decoded audio and of what reached the speaker's codec
  (zero while the speaker is off, marked `speaker off`).
- **Loopback:** the ES8311 runs capture and playback at once (the same I2S
  bus, as `muse_audio_loopback_test` does), and a client's start_mic isn't a
  claim, so a client can record the mic while its own audio plays:
  `tools/mg_ble_client.py play --loopback` checks the tone comes back.

## For clients

Things a phone app or other client has to get right beyond the header:

- **Write Without Response back-pressure.** stream_audio's buffer_update
  covers the device's playback buffer only. The client's own BLE stack also
  has a queue for Write Without Response, and on iOS a write it has no room
  for is dropped without an error. Before each Data write, wait for the stack:
  iOS `CBPeripheral.canSendWriteWithoutResponse` and
  `peripheralIsReady(toSendWriteWithoutResponse:)`; Android, one write at a
  time, the next after `onCharacteristicWrite` (API 33+: a
  `writeCharacteristic` that returns `ERROR_GATT_WRITE_REQUEST_BUSY` is
  retried after the callback); Web Bluetooth, await each
  `writeValueWithoutResponse`. `tools/mg_ble_client.py` does this with
  CoreBluetooth on macOS; without it macOS dropped 6 of 94 writes in a burst
  while the device saw none of them.
- **Count what the device says it received.** Bytes in flight are what you
  wrote minus buffer_update's total received; a write the device drops
  (not whole frames, or too big) still counts as received, so the two stay
  in step.
- **Check the speaker.** Read `speaker_volume` before playing: 0 means the
  device will play silence.

## Throughput test

`mg_command_device_action` with `mg_device_action_throughput_test`
(mgcommands.h, Throughput test; numbered as in mbcommands) is a bench
command on every build, listed with a typed list `[10 fe 1e 0b]`. Send has
the worker pump numbered MTU - 3 byte Data notifications as fast as the
stack takes them (up to 16 a round, every 5 ms) until the duration or stop;
receive counts the client's Data writes after its `change_data_type
throughput_test`, ending at the next one-second tick after the duration, or
on stop. Either reports `[1e 0b 00, bytes, packets, ms, gaps]`. It's `busy`
while audio is live, playing or claimed, start_mic is `busy` while it runs,
push-to-talk ends a send test (reported), and a disconnect ends either
unreported. `tools/mg_ble_client.py bench` drives it.

## Token proof

`mg_command_token_proof` (34; mgcommands.h, Token proof) is listed on every
gadget build. Link setup derives K = HKDF-SHA256(salt "mg token proof v1",
the access token as provisioned, the node id) and stores it beside the tokens
as `mg_proof_k` (hex) in Link's config; `main/mg_glue.c` hands it to this
component (`mg_ble_platform_t.proof_key`), which keeps it only for one HMAC.
A token refresh never changes it; reset or clear erases it.

- **challenge** [16-byte client nonce]: a fresh 16-byte device nonce from
  `esp_fill_random`, then response [device nonce, HMAC-SHA256(K, "mg token
  proof v1 device" || client nonce || device nonce)]. The expected confirm is
  computed now, and K wiped. No key: `not_found`. A new challenge restarts.
- **confirm** [32-byte MAC]: compared in constant time, result 1 or 0; one
  confirm per challenge, else `invalid_value`.
- **clear** after result 1 on this connection: result 1, then the board
  erases the tokens, K and the setup markers and restarts into Link setup
  (`app_gadget_clear_setup()`). Before a match: `proof_required`.
- Nonces, the expected MAC and the match are wiped at every connect and
  disconnect. Nothing else waits on the proof.
- **Logs** never show a token or K: lengths and the first 4 bytes of a
  token's SHA-256 at most.
- **Secure builds** take it on Encrypted Control after authentication only,
  like every command but the key exchange and request_status.

## Codecs

| | SBC (default) | LC3 |
|---|---|---|
| Frames | 16 kHz mono, 16 blocks, 8 subbands, loudness, bitpool 26: 60 B per 8 ms | 16 kHz mono, 10 ms, 40 B |
| Bit rate | 60 kb/s | 32 kb/s |
| Encoder, Xtensa (S3) | 2.6 MIPS | 7.5 MIPS |
| Encoder, RISC-V without FPU (C6, C5) | 2.0 MIPS | 85 MIPS |
| Flash | 3.7 KB | 35 KB |
| Offered | always | `CONFIG_MUSE_GADGET_BLE_LC3`, on by default on the S3 and ESP32 |

Each notification carries whole frames: as many as fit in ATT_MTU - 3 (minus
21 on a secure build), one per notification at the minimum MTU of 100, four at
247. The loop sends what each 20 ms of audio produced, so latency stays under
about 30 ms plus the link.

## Session security

With `CONFIG_MUSE_GADGET_BLE_SECURE` (default off; on for the Waveshare S3
1.75C), the Encrypted Control and Encrypted Data characteristics are added and
every Rule in `mgcommands-secure.h` applies. Gestures, audio and the token
proof then only go to a client that has paired and authenticated.
Pairing methods: physical confirm everywhere, and numeric comparison on boards
with a screen (the avatar shows the code on its pairing card). The talk
button confirms. Pairing mode is on while there are no pairings, and for two
minutes after the user turns on BLE phone setup (Settings, or a double press
of the aux button on touch boards); five key exchanges end it. Up to four
pairings are kept in NVS (namespace `mg`); the fifth replaces the oldest.

## Offline clips

With a `mg_queue` partition (data, subtype `0x4D`; `partitions_muse.csv` has
2 MB after everything else, nothing moved) and the client's
`audio_queue_enabled`, presses made while no client is connected are encoded
with the `audio_codec` setting, staged in PSRAM, and written as one clip when
released. The client lists and downloads them with `mg_command_audio_queue`.
Boards on `partitions.csv` or `partitions_muse_8mb.csv`, or updated over the
air from an older table, have no partition, and don't offer the queue. Clips
are stored in plain flash, like NVS without encryption.

## Nordic UART

TX mirrors log lines whose tag starts with one of
`CONFIG_MUSE_GADGET_BLE_NUS_LOG_TAGS` (default `mg,link.main,link.heartbeat`).
The service has no security, so the default leaves out tags that log what
people say. RX takes `status` and `version`.
