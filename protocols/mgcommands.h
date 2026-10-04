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

#ifndef MGCOMMANDS_H
#define MGCOMMANDS_H

#include <stdint.h>

#define MG_SPEC_VERSION 1

#define MG_SERVICE_UUID "65E6635D-174A-4AF0-805E-D1EDED7C2A63"
#define MG_CONTROL_UUID "732CE4AB-9F36-4F11-9147-0BAC4FCE97C4"
#define MG_DATA_UUID    "5B84AFDF-3B31-4FEF-89E6-514A4A97ABB3"

// The same UUIDs as little-endian byte lists, the order they take in
// advertising data and in most embedded BLE stacks (e.g. NimBLE's
// BLE_UUID128_INIT).
#define MG_SERVICE_UUID_LE_BYTES                                             \
  0x63, 0x2a, 0x7c, 0xed, 0xed, 0xd1, 0x5e, 0x80, 0xf0, 0x4a, 0x4a, 0x17,   \
      0x5d, 0x63, 0xe6, 0x65
#define MG_CONTROL_UUID_LE_BYTES                                             \
  0xc4, 0x97, 0xce, 0x4f, 0xac, 0x0b, 0x47, 0x91, 0x11, 0x4f, 0x36, 0x9f,   \
      0xab, 0xe4, 0x2c, 0x73
#define MG_DATA_UUID_LE_BYTES                                                \
  0xb3, 0xab, 0x97, 0x4a, 0x4a, 0x51, 0xe6, 0x89, 0xef, 0x4f, 0x31, 0x3b,   \
      0xdf, 0xaf, 0x84, 0x5b

// Standard services every musegadgets device also exposes.
#define MG_BAS_SERVICE_UUID16           0x180F
#define MG_BAS_BATTERY_LEVEL_UUID16     0x2A19
#define MG_DIS_SERVICE_UUID16           0x180A
#define MG_DIS_MANUFACTURER_NAME_UUID16 0x2A29
#define MG_DIS_MODEL_NUMBER_UUID16      0x2A24
#define MG_DIS_SERIAL_NUMBER_UUID16     0x2A25
#define MG_DIS_HARDWARE_REVISION_UUID16 0x2A27
#define MG_DIS_FIRMWARE_REVISION_UUID16 0x2A26
#define MG_DIS_SOFTWARE_REVISION_UUID16 0x2A28

#define MG_NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define MG_NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define MG_NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define MG_NUS_SERVICE_UUID_LE_BYTES                                         \
  0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5,   \
      0x01, 0x00, 0x40, 0x6e
#define MG_NUS_RX_UUID_LE_BYTES                                              \
  0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5,   \
      0x02, 0x00, 0x40, 0x6e
#define MG_NUS_TX_UUID_LE_BYTES                                              \
  0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5,   \
      0x03, 0x00, 0x40, 0x6e

// Clients negotiate at least this ATT MTU; audio frames fit comfortably.
#define MG_MIN_ATT_MTU 100

// Advertised name prefix clients may use as a fallback filter.
#define MG_DEVICE_NAME_PREFIX "MuseGadget"

// Audio defaults when change_data_type carries no format parameters.
#define MG_AUDIO_DEFAULT_SAMPLE_RATE_HZ 16000
#define MG_AUDIO_DEFAULT_CHANNELS       1
#define MG_LC3_DEFAULT_FRAME_US         10000
#define MG_LC3_DEFAULT_FRAME_BYTES      40

/*
 * =============================================================================
 * musegadgets BLE Protocol (DRAFT)
 * =============================================================================
 *
 * A small command protocol for BLE gadgets that stream push-to-talk audio to
 * a client (phone app, browser, or gateway). The gadget is the GATT server.
 * Multi-byte integers are little-endian.
 *
 * Command 6, commands 0x80-0x82 and error codes 0x0100-0x01FF are reserved
 * for a future security extension.
 *
 * =============================================================================
 * Advertising
 * =============================================================================
 *
 *   Advertising data:  Flags, and the Complete List of 128-bit Service UUIDs
 *                      containing MG_SERVICE_UUID. Clients scan by this UUID.
 *   Scan response:     Complete Local Name (the GAP Device Name), which
 *                      should start with MG_DEVICE_NAME_PREFIX, e.g.
 *                      "MuseGadget-1A2B3C". Appearance and the 16-bit BAS
 *                      UUID may be added if they fit.
 *
 * =============================================================================
 * GATT Service Definition
 * =============================================================================
 *
 * Primary Service: musegadgets
 *   Service UUID: 65E6635D-174A-4AF0-805E-D1EDED7C2A63
 *
 *   Characteristic 1: Control
 *     UUID: 732CE4AB-9F36-4F11-9147-0BAC4FCE97C4
 *     Properties: Write Without Response, Notify
 *     Permissions: Write
 *     CCC Descriptor: Read, Write
 *     Description:
 *       Command channel. The client writes mg_command_t commands here (first
 *       byte = command ID, remaining bytes = parameters). The device notifies
 *       responses, status updates, and data type changes.
 *
 *   Characteristic 2: Data
 *     UUID: 5B84AFDF-3B31-4FEF-89E6-514A4A97ABB3
 *     Properties: Write Without Response, Notify
 *     Permissions: Write
 *     CCC Descriptor: Read, Write
 *     Description:
 *       Bulk data channel. The device notifies payloads (audio, offline clips)
 *       whose type is set by the most recent mg_command_change_data_type
 *       notification on Control. Payloads are chunked to ATT_MTU - 3 bytes.
 *       For live audio, a notification carries whole codec frames only, so a
 *       client can decode each notification on its own.
 *       During audio playback the client writes the encoded audio here (see
 *       Audio playback).
 *
 * Primary Service: Battery Service (standard BLE BAS), required
 *   Service UUID: 0x180F
 *   Characteristic: Battery Level (0x2A19), Read + Notify, uint8_t 0-100.
 *   Devices without a battery report 100.
 *
 * Primary Service: Device Information Service (standard BLE DIS), required
 *   Service UUID: 0x180A
 *   Characteristics (Read, UTF-8, no terminator):
 *     Manufacturer Name String (0x2A29)  required, e.g. "Seeed Studio"
 *     Model Number String      (0x2A24)  required, board name,
 *                                        e.g. "XIAO nRF54L15"
 *     Firmware Revision String (0x2A26)  required, firmware version,
 *                                        e.g. "1.2.0"
 *     Software Revision String (0x2A28)  optional, "mg<MG_SPEC_VERSION>"
 *     Hardware Revision String (0x2A27)  optional
 *     Serial Number String     (0x2A25)  optional
 *   The user-facing device name is the GAP Device Name (0x2A00).
 *
 * Primary Service: Nordic UART Service (NUS), required
 *   Service UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX (6E400002-...): Write, Write Without Response. Client -> device text.
 *   TX (6E400003-...): Notify. Device -> client text.
 *   A line-oriented debug console: the device mirrors its log on TX and may
 *   accept console commands on RX. Clients must not depend on its contents.
 *   BAS, DIS and NUS must never carry audio, keys, or user content.
 *
 * =============================================================================
 * Protocol Overview
 * =============================================================================
 *
 * Command format (client write):     [mg_command_t, parameters...]
 * Notification format (device):      [mg_command_t, response parameters...]
 *
 * Values not defined in an enum are reserved. In mg_command_t, 0x00-0x7F
 * keep their legacy (mbcommands) numbering and the gaps are held so future
 * commands can return with the same numbers; 0x80-0xFD are
 * musegadgets-specific. Receivers ignore trailing parameter bytes they do not
 * understand, so optional parameters can be added later.
 *
 * Connection flow:
 *   1. Client negotiates ATT MTU >= MG_MIN_ATT_MTU and subscribes to the
 *      Control and Data CCC descriptors.
 *   2. Client writes mg_command_request_status on Control; the device
 *      notifies change_data_type and supported_features. The session is
 *      ready.
 *   3. If supported_features lists mg_command_token_proof and the client
 *      holds a proof key for this device, it runs the token proof.
 *
 * Push-to-talk:
 *   - The client enables device-side push-to-talk with set_settings
 *     [mg_setting_parameter_push_to_talk_enabled, 1].
 *   - Button down: device notifies [gesture, button_down], plays the
 *     activation buzz (if it lists and has haptics enabled), notifies
 *     [change_data_type, <audio type>, <format...>], then streams audio on
 *     Data.
 *   - Button up: device notifies [gesture, button_up], stops capture, and
 *     notifies [stop_mic] to mark the end of the utterance.
 *   - Device-initiated capture uses the codec from the most recent start_mic
 *     on this connection, else mg_setting_parameter_audio_codec.
 *   - With push-to-talk disabled, the device only reports gestures; the
 *     client may drive capture itself with start_mic and stop_mic.
 *
 * Assistant state:
 *   A device with a display or status light lists mg_command_assistant_state
 *   and shows the same sequence as a Wi-Fi Muse gadget: listening, thinking,
 *   responding, then its finished animation, then idle.
 *   - The device shows listening while it captures, and thinking by itself
 *     once an utterance ends (stop_mic), since the utterance is now with the
 *     assistant. It never shows its finished animation at stop_mic, even
 *     from a client that has sent no assistant_state yet: clients need not
 *     send thinking.
 *   - The client then reports the turn with assistant_state: responding when
 *     the reply starts (its text streaming, or its audio playing on the
 *     device), done once the reply has fully arrived and, if the device plays
 *     it, once playback has ended (stop_streaming keep acknowledged), and
 *     error if the turn fails. idle cancels the turn.
 *   - While it plays stream_audio the device shows responding whatever the
 *     last assistant_state was, and returns to that state after playback.
 *     A client that plays the reply on the device therefore sends done after
 *     the device acknowledges stop_streaming keep, not when the text ends.
 *   - A device left in thinking or responding for 60 s without another
 *     assistant_state, or whose client disconnects, returns to idle.
 *   - Any assistant_state takes over from the device's own thinking, and
 *     marks the client as one that reports the turn for the rest of the
 *     connection.
 *   - Older clients never send assistant_state. While the client has sent
 *     none on this connection, the device ends the turn by itself, playing
 *     its finished animation and going idle instead of staying in (or
 *     returning to) thinking: a device that plays the reply does so when
 *     playback ends; a device without stream_audio does so about 15 s after
 *     stop_mic if no assistant_state has arrived.
 *
 * Audio formats (Data payloads for each mg_data_type_t):
 *   - audio_sbc:  standard SBC frames (syncword 0x9C), self-describing.
 *                 Devices use 16 kHz mono, 16 blocks, 8 subbands, loudness
 *                 allocation; not mSBC. This is the default codec.
 *   - audio_lc3:  LC3 frames (ETSI TS 103 634) of a fixed size, back to back.
 *                 The frame duration and size come from change_data_type,
 *                 defaulting to MG_LC3_DEFAULT_FRAME_US and
 *                 MG_LC3_DEFAULT_FRAME_BYTES.
 *   - audio_opus: raw Opus packets, one per notification.
 *   - audio_pcm:  signed 16-bit little-endian samples.
 *
 * Audio playback (client -> device):
 *   A device with a speaker lists mg_command_stream_audio in
 *   supported_features. The flow mirrors mbcommands' stream_audio:
 *   1. The client sends request_capabilities; the device answers
 *      report_capabilities (codecs, buffer size, sample rates, maximum
 *      bitrate and channels).
 *   2. The client sends start_streaming with its parameters. The device
 *      answers buffer_update if it accepts them, or error with
 *      mg_error_code_unsupported / mg_error_code_invalid_value if not.
 *   3. The client writes the encoded audio on Data with Write Without
 *      Response: whole codec frames, in order, no header, in the codec from
 *      start_streaming. It keeps the bytes in flight (bytes written minus the
 *      total received from the latest buffer_update) below the available
 *      bytes that update reported. The device sends buffer_update in reply to
 *      request_buffer_update and on its own at least every 250 ms while
 *      streaming.
 *   4. stop_streaming with keep: the device plays what it holds, then
 *      notifies stop_streaming keep. With drop: it discards its buffer at
 *      once and notifies stop_streaming drop.
 *   The device notifies stop_streaming drop on its own when it must end
 *   playback, e.g. when push-to-talk takes over the audio path; the client
 *   stops writing. Devices start playing once they hold about 100 ms of
 *   audio, and fill underruns with silence. Playback follows the device's
 *   speaker setting (mg_setting_parameter_speaker_volume): with the speaker
 *   off, the stream is accepted and paced but plays silence.
 *
 * Token proof:
 *   A gadget gets its device token pair through Muse Link setup (see
 *   protocols/README.md), not over this protocol. Setup leaves the device
 *   and the app each holding a proof key
 *     K = HKDF-SHA256(salt "mg token proof v1", IKM access token,
 *                     info node_id, 32 bytes)
 *   and on every connection the client checks that both sides agree on it
 *   without sending the token:
 *     client -> challenge [client_nonce 16]
 *     device -> response  [device_nonce 16,
 *                          HMAC-SHA256(K, "mg token proof v1 device" ||
 *                                      client_nonce || device_nonce)]
 *     client -> confirm   [HMAC-SHA256(K, "mg token proof v1 client" ||
 *                                      client_nonce || device_nonce)]
 *     device -> result    [1 match, 0 mismatch]
 *   A device without a token answers challenge with error not_found. A new
 *   challenge restarts the exchange; confirm without a pending response is
 *   invalid_value. The result lasts for the connection. The device doesn't
 *   gate other commands on it: the proof tells the app that the gadget holds
 *   the token this phone set up, not that the link is secure.
 *   clear (after result 1 on this connection) erases the device's tokens and
 *   K and returns it to setup; the device answers result 1 first. Before a
 *   match it is error proof_required.
 *
 * Throughput test (a bench command):
 *   mg_command_device_action with mg_device_action_throughput_test measures
 *   the link, numbered as in mbcommands (which defines the device->client
 *   direction; mg adds client->device). A device that lists device_action
 *   offers it. A device refuses it with mg_error_code_busy while audio is
 *   live, recording or playing, or a test is already running.
 *   - [device_action, throughput_test, 1, duration_s (uint16)]: send. The
 *     device notifies change_data_type throughput_test, then streams Data
 *     notifications as fast as the link takes them, each MTU-3 bytes: a
 *     uint32 sequence number from 0, then the pattern byte (sequence + i) &
 *     0xFF for byte i of the rest. It stops after duration_s seconds (0:
 *     until stop) and notifies the report below on its own.
 *   - [device_action, throughput_test, 2, duration_s (uint16)]: receive. The
 *     client writes change_data_type throughput_test on Control, then Data
 *     frames in the same format with Write Without Response, honouring its
 *     stack's back-pressure. The device counts them; after duration_s (0:
 *     until stop) it reports and ignores further test data.
 *   - [device_action, throughput_test, 0]: stop. The device answers with the
 *     report for whichever direction ran (a report of zeros if none did):
 *     [device_action, throughput_test, 0, bytes (uint32), packets (uint32),
 *     ms (uint32), sequence gaps (uint32)], counted from the first test
 *     packet to the last. For send, what it sent; for receive, what it got,
 *     and gaps counts sequence numbers missing from what arrived.
 *   While a receive test runs the device keeps its connection latency off,
 *   as for any client->device transfer.
 *
 * Offline clips:
 *   A device with storage may record push-to-talk utterances while no client
 *   is connected, and hand them over later with mg_command_audio_queue. The
 *   client lists clips with status and clip_info, reads each with read_clip,
 *   and erases the queue with clear once every clip is safe. Live
 *   push-to-talk wins over a download: the device finishes or abandons the
 *   clip transfer before streaming live audio, and the client stops issuing
 *   read_clip while a live utterance is open.
 *
 * =============================================================================
 */

#ifdef __OBJC__
#import <Foundation/Foundation.h>
#define MG_ENUM(_type, _name) typedef NS_ENUM(_type, _name)
#else
#define MG_ENUM(_type, _name)                                                \
  typedef _type _name;                                                      \
  enum _name##_Constants
#endif

MG_ENUM(uint8_t, mg_command_t) {
  mg_command_reserved = 0,
  // param 1 (optional): mg_data_type_t audio codec (default: device choice)
  // param 2..N: reserved, ignored by devices
  // The device notifies change_data_type, then streams audio on Data.
  mg_command_start_mic = 1,
  // Client -> device: stop capture.
  // Device -> client: capture has ended. Sent whenever capture ends,
  // whichever side started it.
  mg_command_stop_mic = 2,
  // 3-5 reserved
  // 6 reserved for a future security extension
  // 7 reserved
  // param 1: mg_data_type_t
  // Audio types may add, in order, each optional:
  //   param 2: uint16_t sample rate in Hz (default 16000)
  //   param 3: uint8_t channel count (default 1)
  //   param 4: uint16_t encoded frame size in bytes (0 = variable)
  //   param 5: uint16_t frame duration in microseconds
  mg_command_change_data_type = 8,
  // param 1: mic gain (1-100)
  mg_command_set_mic_gain = 9,
  // No params. Response: change_data_type (current type), supported_features.
  mg_command_request_status = 10,
  // param 1: mg_gesture_t, param 2 (optional): button index
  mg_command_gesture = 11,
  // 12-15 reserved
  // Response payloads:
  //   Command list (commands the device accepts from the client; commands it
  //   only notifies, such as gesture or error, are not listed):
  //     [mg_command_supported_features, <mg_command_t>...]
  //   Typed sub-feature list for namespaces under a command:
  //     [mg_command_supported_features, mg_command_sub_feature,
  //      <owning mg_command_t>, <namespace-specific values...>]
  // Defined typed lists:
  //   [supported_features, sub_feature, mg_command_start_mic,
  //    <mg_data_type_t>...]   audio codecs the device can capture with
  //   [supported_features, sub_feature, mg_command_set_settings,
  //    <mg_setting_parameter_t>...]   every setting the device accepts in
  //    get_settings and set_settings. Clients offer only these (e.g. no
  //    vibration controls on a device that doesn't list haptics_enabled)
  //    and never send others.
  mg_command_supported_features = 16,
  // 17-24 reserved
  // param 1: mg_command_t that errored, param 2: array of mg_error_data_t
  mg_command_error = 25,
  // 26-28 reserved
  // param 1: mg_stream_audio_command_t, additional params depend on
  // sub-command. See Audio playback.
  mg_command_stream_audio = 29,
  // param 1: mg_device_action_t, additional params depend on the action.
  // A bench command; see Throughput test.
  mg_command_device_action = 30,
  // 31-33 reserved
  // param 1: mg_token_proof_command_t, additional params depend on
  // sub-command. See Token proof.
  mg_command_token_proof = 34,
  // 35-62 reserved
  // param 1: array of mg_setting_parameter_t to fetch
  // Response: array of (mg_setting_parameter_t, value) pairs; each value's
  //         byte width is determined by its parameter (see mg_setting_parameter_t)
  mg_command_get_settings = 63,
  // 64 reserved
  // param 1: array of (mg_setting_parameter_t, value) pairs; each value's
  //         byte width is determined by its parameter (see mg_setting_parameter_t)
  // No response on success; read values back with get_settings.
  // A device that meets an unknown parameter stops parsing and notifies
  // mg_error_code_unsupported.
  mg_command_set_settings = 65,
  // 66-75 reserved
  // param 1: mg_audio_queue_command_t, additional params depend on sub-command.
  // See Offline clips.
  mg_command_audio_queue = 76,
  // 77-0x7F reserved
  // 0x80-0x82 reserved for a future security extension
  // Client -> device. param 1: mg_assistant_state_t. No reply.
  // See Assistant state.
  mg_command_assistant_state = 0x83,
  // 0x84-0xFD reserved
  // Reserved for typed mg_command_supported_features payloads only.
  // This is not a standalone top-level command.
  mg_command_sub_feature = 0xFE,
  // Reserved for future extended command protocol
  mg_command_extended = 0xFF,
};

// Other values reserved.
MG_ENUM(uint8_t, mg_data_type_t) {
  mg_data_type_unknown = 0,
  mg_data_type_audio_sbc = 1,
  mg_data_type_audio_adpcm = 2,
  mg_data_type_audio_opus = 3,
  mg_data_type_audio_pcm = 6,
  mg_data_type_audio_lc3 = 19,
  // Throughput test data (see Throughput test), the same number as
  // mb_data_type_throughput_test.
  mg_data_type_throughput_test = 20,
  // Compressed bytes of one offline clip. The clip's codec and byte count come
  // from mg_audio_queue_command_clip_info; the type stays set for the whole
  // download.
  mg_data_type_audio_queue = 23,
};

// Other values reserved.
MG_ENUM(uint8_t, mg_gesture_t) {
  mg_gesture_button_down = 0,
  mg_gesture_button_up = 1,
};

MG_ENUM(uint8_t, mg_error_data_t) {
  // param 1: mg_error_code_t (uint16)
  mg_error_data_code = 1,
  // param 1: size (uint16), param 2: string
  mg_error_data_log = 2,
};

// 0x0100-0x01FF are reserved for a future security extension.
MG_ENUM(uint16_t, mg_error_code_t) {
  // Unknown command, sub-command, or setting parameter
  mg_error_code_unsupported = 1,
  mg_error_code_invalid_length = 2,
  mg_error_code_invalid_value = 3,
  // The device cannot do this right now (e.g. read_clip during live audio).
  mg_error_code_busy = 4,
  // The thing asked for doesn't exist (e.g. a token proof challenge on a
  // device that holds no token).
  mg_error_code_not_found = 5,
  // clear before a matching token proof on this connection.
  mg_error_code_proof_required = 6,
};

// Other values reserved.
MG_ENUM(uint8_t, mg_setting_parameter_t) {
  // value: uint8_t MG_SPEC_VERSION (read-only)
  mg_setting_parameter_spec_version = 0,
  // Per connection, not persisted: 0 at every new connection.
  // value: uint8_t bool (0=disabled, 1=enabled)
  mg_setting_parameter_push_to_talk_enabled = 2,
  // Enables the push-to-talk activation buzz. Only devices with a vibration
  // motor list this and the ptt_buzz_* settings.
  // Per connection, not persisted: 0 at every new connection.
  // value: uint8_t bool (0=disabled, 1=enabled)
  mg_setting_parameter_haptics_enabled = 3,
  // Push-to-talk activation buzz frequency. Persisted on device.
  // value: uint16_t freq_hz (20-20000 Hz). Default 210.
  mg_setting_parameter_ptt_buzz_freq_hz = 4,
  // Push-to-talk activation buzz duration. Persisted on device.
  // value: uint16_t duration_ms (1-5000 ms). Default 100.
  mg_setting_parameter_ptt_buzz_duration_ms = 5,
  // Push-to-talk activation buzz volume. Persisted on device.
  // value: uint8_t volume percent (0-100). Default 80.
  mg_setting_parameter_ptt_buzz_volume_percent = 6,
  // Codec for device-initiated capture. Persisted on device.
  // value: uint8_t mg_data_type_t. Default mg_data_type_audio_sbc.
  mg_setting_parameter_audio_codec = 7,
  // Record push-to-talk utterances to the offline queue while no client is
  // connected. Persisted on device.
  // value: uint8_t bool (0=disabled, 1=enabled)
  mg_setting_parameter_audio_queue_enabled = 8,
  // 9 reserved
  // Speaker volume for playback (see Audio playback) and device sounds.
  // Persisted on device. 0 turns the speaker off; any other value turns it on
  // at that volume. Only devices with a speaker accept it.
  // value: uint8_t volume percent (0-100)
  mg_setting_parameter_speaker_volume = 10,
};

MG_ENUM(uint8_t, mg_assistant_state_t) {
  // No turn in progress.
  mg_assistant_state_idle = 0,
  // The assistant is working on the utterance. Devices show it on their own
  // after stop_mic; clients send it after a client-driven capture.
  mg_assistant_state_thinking = 2,
  // The reply is arriving or playing.
  mg_assistant_state_responding = 3,
  // The reply is complete; the device plays its finished animation, then
  // goes idle.
  mg_assistant_state_done = 4,
  // The turn failed; the device shows its error state, then goes idle.
  mg_assistant_state_error = 5,
};

// Actions under mg_command_device_action, numbered as mbcommands'
// mb_device_action_t so the two never diverge: values mg doesn't define are
// reserved for mb's meanings.
MG_ENUM(uint8_t, mg_device_action_t) {
  // 0-10 reserved (mbcommands device actions)
  // param 1: uint8_t 0 = stop (and report), 1 = send (device -> client),
  //          2 = receive (client -> device; an mg extension)
  // param 2: uint16_t duration in seconds (0 = until stop), when starting
  // See Throughput test.
  mg_device_action_throughput_test = 11,
  // 12-0xFF reserved (mbcommands device actions)
};

// Throughput test directions, param 1 of mg_device_action_throughput_test.
MG_ENUM(uint8_t, mg_throughput_test_t) {
  mg_throughput_test_stop = 0,
  mg_throughput_test_send = 1,
  mg_throughput_test_receive = 2,
};

#define MG_TOKEN_PROOF_NONCE_LEN 16
#define MG_TOKEN_PROOF_MAC_LEN 32

MG_ENUM(uint8_t, mg_token_proof_command_t) {
  // Client -> device. Payload: uint8_t client_nonce[16]
  mg_token_proof_challenge = 0,
  // Device -> client. Payload: uint8_t device_nonce[16], uint8_t mac[32]
  mg_token_proof_response = 1,
  // Client -> device. Payload: uint8_t mac[32]
  mg_token_proof_confirm = 2,
  // Device -> client. Payload: uint8_t matched (1 or 0)
  mg_token_proof_result = 3,
  // Client -> device. No payload. Only after a match on this connection.
  mg_token_proof_clear = 4,
};

MG_ENUM(uint8_t, mg_stream_audio_command_t) {
  // Client -> device. No payload.
  mg_stream_audio_request_capabilities = 0,
  // Device -> client. Payload: uint8_t count, then count entries, each an
  // mg_stream_audio_capability_t followed by its value.
  mg_stream_audio_report_capabilities = 1,
  // Client -> device. Payload: uint8_t count, then count entries, each an
  // mg_stream_audio_parameter_t followed by its value. Codec is required;
  // omitted parameters take the device's first reported value.
  mg_stream_audio_start_streaming = 2,
  // Both directions. Payload: mg_stream_audio_buffer_action_t.
  // Client -> device: end playback. Device -> client: playback has ended.
  mg_stream_audio_stop_streaming = 3,
  // Client -> device. No payload.
  mg_stream_audio_request_buffer_update = 4,
  // Device -> client. Payload: uint32_t available buffer bytes,
  // uint32_t total bytes received since start_streaming.
  mg_stream_audio_buffer_update = 5,
};

MG_ENUM(uint8_t, mg_stream_audio_capability_t) {
  // value: uint8_t count, then count mg_data_type_t (supported codecs)
  mg_stream_audio_capability_codecs = 1,
  // value: uint32_t playback buffer size in bytes
  mg_stream_audio_capability_buffer_size = 2,
  // value: uint8_t count, then count uint32_t sample rates in Hz
  mg_stream_audio_capability_sample_rates = 3,
  // value: uint16_t maximum bitrate in kb/s
  mg_stream_audio_capability_max_bitrate = 4,
  // value: uint8_t maximum channel count
  mg_stream_audio_capability_max_channels = 5,
};

MG_ENUM(uint8_t, mg_stream_audio_parameter_t) {
  // value: mg_data_type_t codec
  mg_stream_audio_parameter_codec = 1,
  // value: uint32_t sample rate in Hz
  mg_stream_audio_parameter_sample_rate = 2,
  // value: uint16_t bitrate in kb/s
  mg_stream_audio_parameter_bitrate = 3,
  // value: uint8_t channel count
  mg_stream_audio_parameter_channels = 4,
  // value: uint16_t encoded frame size in bytes (0 = variable)
  mg_stream_audio_parameter_frame_size = 5,
};

MG_ENUM(uint8_t, mg_stream_audio_buffer_action_t) {
  // Play what is buffered, then stop.
  mg_stream_audio_buffer_keep = 0,
  // Discard what is buffered and stop now.
  mg_stream_audio_buffer_drop = 1,
};

MG_ENUM(uint8_t, mg_audio_queue_command_t) {
  // Request: no payload.
  // Response: [mg_command_audio_queue, status, uint8_t enabled,
  //            uint32_t used_bytes, uint32_t capacity_bytes,
  //            uint16_t clip_count]
  mg_audio_queue_command_status = 1,
  // Request: no payload. Erases every clip, then notifies status.
  mg_audio_queue_command_clear = 3,
  // Metadata for one complete clip by zero-based queue index.
  // Request: uint16_t clip_index.
  // Response: [mg_command_audio_queue, clip_info, uint16_t clip_index,
  //            mg_data_type_t codec, uint32_t start_timestamp_ms,
  //            uint32_t compressed_bytes]
  // start_timestamp_ms is device uptime at the start of the clip. It resets
  // on reboot, so use it for ordering within one boot only; clips are
  // queued oldest first.
  mg_audio_queue_command_clip_info = 4,
  // Request: uint16_t clip_index.
  // The device notifies [change_data_type, mg_data_type_audio_queue], then
  // streams the clip's compressed_bytes on Data, chunked to the MTU. The
  // client detects completion by byte count. Codec frames are stored whole
  // and back to back, in the format described under Audio formats.
  mg_audio_queue_command_read_clip = 5,
};

#endif
