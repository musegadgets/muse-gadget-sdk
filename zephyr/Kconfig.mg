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

# Options shared by the app and the tests.

DT_ZEPHYR_USER := /zephyr,user

menu "Muse Gadget"

config MG_DEVICE_NAME_PREFIX
	string "Advertised name prefix"
	default "MuseGadget-"
	help
	  The GAP Device Name is this prefix followed by the last three bytes
	  of the identity address in hex, e.g. MuseGadget-1A2B3C.

config MG_SDK_TOKEN
	string "Muse Gadgets SDK token"
	default ""
	help
	  Your mgst_ SDK token from gadgets.muse.ai (Account > SDK tokens).
	  Muse Link setup hands it to the app in pairing_confirmed, for the
	  device-token mint. Keep it out of committed config files.

config MG_RESET_HOLD_MS
	int "Hold the button this long at power-up to reset setup (ms)"
	default 5000
	help
	  Holding the talk button while powering up, and on for this long,
	  erases what Muse Link setup stored (the device tokens, the token
	  proof key and the setup markers) and advertises Link setup again.

config MG_SBC_BITPOOL
	int "SBC bitpool"
	range 2 128
	default 26
	help
	  SBC at 16 kHz mono, 16 blocks, 8 subbands, loudness allocation.
	  Bitpool 26 gives 60-byte frames every 8 ms (60 kb/s), the same
	  quality point as mSBC.

config MG_LC3
	bool "LC3 codec"
	default y
	help
	  Adds LC3 (16 kHz mono, 10 ms frames) to the start_mic codec list.

config MG_LC3_FRAME_BYTES
	int "LC3 frame size in bytes"
	depends on MG_LC3
	range 20 400
	default 40

config MG_AUDIO_MAX_PACKET_MS
	int "Longest stretch of audio in one Data notification (ms)"
	default 40
	help
	  Whole codec frames are packed into each notification up to the ATT
	  payload size and this much audio, which bounds the added latency.

config MG_MIC_DEFAULT_GAIN
	int "Default mic gain (1-100)"
	range 1 100
	default 50
	help
	  Software gain applied to the PCM before encoding, and what a client's
	  set_mic_gain changes. The scale is gain/12.5, so 50 is +12 dB and 12
	  is about unity.

config MG_MIC_HPF_HZ
	int "DC-blocking high-pass corner (Hz, 0 = off)"
	range 0 200
	default 30
	help
	  A one-pole high-pass right after the microphone, before the gain:
	  PDM microphones start with a large DC offset that drifts for seconds.
	  Primed with the first sample at each start, so the first frames carry
	  no settling step.

config MG_DMIC_HW_GAIN_DB
	int "PDM hardware gain (dB, -20 to +20)"
	range -20 20
	default 10
	depends on NRFX_PDM
	help
	  The nRF PDM peripheral's own gain, in 0.5 dB steps from its 0 dB
	  default, applied before decimation so quiet speech keeps its
	  resolution. +10 dB with the default software gain puts speech from
	  about 30 cm near -20 dBFS on the XIAO nRF54L15 Sense.

config MG_HAPTICS
	bool "The board has a vibration motor"
	help
	  Only boards with a vibration motor list and accept haptics_enabled
	  and the ptt_buzz_* settings (mgcommands.h), so apps show vibration
	  controls only for them. The XIAO has none (its "buzz" is an LED
	  flash): off, those settings answer unsupported.

config MG_ASSISTANT_STATE
	bool "Show the assistant state on the LED"
	default y if $(dt_alias_enabled,led0)
	help
	  Lists mg_command_assistant_state and shows listening, thinking,
	  responding, done and error on the LED.

config MG_STATE_COMPAT_TIMEOUT_MS
	int "Turn timeout for clients that send no assistant_state (ms)"
	depends on MG_ASSISTANT_STATE
	default 15000
	help
	  After stop_mic the LED shows thinking by itself. A client that has
	  sent no assistant_state on this connection and sends none within this
	  time gets done (then idle) instead: older clients never report the
	  turn. Any assistant_state cancels it for the rest of the connection.

choice MG_AUDIO_SRC
	prompt "Audio source"
	default MG_AUDIO_SRC_DMIC if $(dt_nodelabel_enabled,dmic_dev)
	default MG_AUDIO_SRC_SYNTH

config MG_AUDIO_SRC_DMIC
	bool "PDM microphone (DMIC API)"
	select AUDIO
	select AUDIO_DMIC

config MG_AUDIO_SRC_SYNTH
	bool "Synthetic test signal"
	help
	  A deterministic tone pattern at 16 kHz, for simulation and boards
	  without a microphone.

endchoice

config MG_AUDIO_QUEUE
	bool "Offline clip queue"
	default y if $(dt_nodelabel_enabled,audio_queue_partition)
	default y if MG_QUEUE_IN_SLOT1
	select FLASH
	select FLASH_MAP
	select CRC
	help
	  Records push-to-talk utterances to the audio_queue_partition (or
	  MCUboot's secondary slot, MG_QUEUE_IN_SLOT1) while no client is
	  connected (when audio_queue_enabled is set) and serves them with
	  mg_command_audio_queue.

config MG_QUEUE_IN_SLOT1
	bool "Keep offline clips in MCUboot's secondary slot"
	default y if BOOTLOADER_MCUBOOT && $(dt_nodelabel_enabled,slot1_partition)
	help
	  The clip queue lives in slot1_partition, which firmware updates also
	  use: an upload hands it to OTA (dropping the clips), and the queue
	  takes it back once MCUboot needs nothing there (see src/mg_queue.h).

config MG_QUEUE_SLOT1_TRAILER
	hex "Bytes at the end of the slot the queue never writes"
	depends on MG_QUEUE_IN_SLOT1
	default 0x3000
	help
	  MCUboot's image trailer (magic, flags, swap status). With swap using
	  move it is 3 x (sectors per slot) x (write block) bytes of status plus
	  about 80 bytes: 3 x 178 x 16 + 80 = 8.6 KB for the XIAO's 712 KB slot
	  of 4 KB sectors, rounded up to whole sectors.

config MG_DFU
	bool "Firmware updates over BLE (MCUmgr SMP)"
	default y if BOOTLOADER_MCUBOOT && MCUMGR_TRANSPORT_BT && MCUMGR_GRP_IMG
	depends on BOOTLOADER_MCUBOOT
	select MCUMGR_SMP_COMMAND_STATUS_HOOKS
	help
	  MCUmgr's image and OS groups on the SMP GATT service, for nRF Connect
	  Device Manager and mcumgr clients. The app confirms a new image once
	  BLE is up and advertising, so an image that never gets there is
	  reverted at the next reset. Needs a sysbuild build with MCUboot.

choice MG_SMP_ACCESS
	prompt "Who may use MCUmgr SMP"
	depends on MG_DFU
	default MG_SMP_ACCESS_SESSION if MG_SECURE
	default MG_SMP_ACCESS_OPEN

config MG_SMP_ACCESS_OPEN
	bool "Anyone connected"
	help
	  Any connected client can list, upload, test, confirm and reset. For
	  development and plaintext gadgets.

config MG_SMP_ACCESS_SESSION
	bool "Only clients with an authenticated session"
	depends on MG_SECURE
	help
	  SMP commands are answered only while the connection carries an
	  authenticated mgcommands-secure session (a paired client that has
	  proved its key); otherwise every command gets MGMT_ERR_EACCESSDENIED.
	  The Muse app updates the gadget after authenticating; generic SMP
	  tools can't.

endchoice

menu "Connection parameters"

config MG_CONN_INTERVAL_MIN
	int "Connection interval, minimum (1.25 ms units)"
	range 6 3200
	default 12
	help
	  12 is 15 ms. Apple's accessory rules: at least 15 ms, and the maximum
	  at least 15 ms above the minimum unless both are 15 ms.

config MG_CONN_INTERVAL_MAX
	int "Connection interval, maximum (1.25 ms units)"
	range 6 3200
	default 12

config MG_CONN_LATENCY
	int "Peripheral latency (connection events)"
	range 0 30
	default 10
	help
	  Events the gadget may skip while it has nothing to send. Off (0)
	  while the phone streams to it (MG_CONN_LATENCY_HOLD_MS). Apple:
	  at most 30, and interval max x (latency + 1) x 3 below the timeout.

config MG_CONN_TIMEOUT
	int "Supervision timeout (10 ms units)"
	range 10 3200
	default 400
	help
	  400 is 4 s (Apple: 2 to 6 s).

config MG_CONN_LATENCY_HOLD_MS
	int "Latency stays off this long after the last phone->device data (ms)"
	default 2000

endmenu

config MG_BENCH_NO_CONFIRM
	bool "Never confirm a new image (revert test)"
	depends on MG_DFU
	help
	  Bench builds only: the app doesn't confirm an image running on test,
	  so MCUboot swaps the previous image back at the next reset. Build a
	  test image with it to show a revert; never ship it.

config MG_BENCH
	bool "Bench console"
	select CONSOLE_SUBSYS
	help
	  Single-key commands on the console UART, for testing without a phone
	  or a hand on the button: d / u press and release the talk button
	  through the real button path, s prints the status. Normal builds keep
	  the console output-only.

config MG_AUDIO_QUEUE_MAX_CLIPS
	int "Most clips the queue holds"
	depends on MG_AUDIO_QUEUE
	default 128

config MG_BATTERY_ADC
	bool "Read the battery voltage with the ADC"
	default y if $(dt_node_has_prop,$(DT_ZEPHYR_USER),io-channels)
	select ADC
	help
	  Without it, Battery Level reads 100.

config MG_BATTERY_DIVIDER_RATIO
	int "Battery divider ratio (VBAT / pin voltage)"
	depends on MG_BATTERY_ADC
	default 2

config MG_NUS
	bool "Nordic UART Service log mirror and console"
	default y
	select BT_ZEPHYR_NUS
	select RING_BUFFER

config MG_SECURE
	bool "Session security (mgcommands-secure.h)"
	select PSA_CRYPTO
	select PSA_WANT_ALG_ECDH
	select PSA_WANT_ECC_MONTGOMERY_255
	select PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_GENERATE
	select PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_IMPORT
	select PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_EXPORT
	select PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY
	select PSA_WANT_ALG_SHA_256
	select PSA_WANT_ALG_HMAC
	select PSA_WANT_KEY_TYPE_HMAC
	select PSA_WANT_ALG_GCM
	select PSA_WANT_KEY_TYPE_AES
	help
	  Key exchange, application-layer AES-256-GCM on the Encrypted
	  Control and Encrypted Data characteristics, physical_confirm pairing
	  with the button, and pairing keys persisted with Zephyr settings.

config MG_SECURE_MAX_PAIRINGS
	int "Stored pairings"
	depends on MG_SECURE
	default 4

config MG_LINK_SETUP_CRYPTO
	bool
	default y
	select PSA_CRYPTO
	select PSA_WANT_ALG_ECDH
	select PSA_WANT_ECC_SECP_R1_256
	select PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_GENERATE
	select PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_IMPORT
	select PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_EXPORT
	select PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY
	select PSA_WANT_ALG_SHA_256
	select PSA_WANT_ALG_HMAC
	select PSA_WANT_KEY_TYPE_HMAC
	select PSA_WANT_ALG_GCM
	select PSA_WANT_KEY_TYPE_AES
	help
	  Muse Link setup (P-256 ECDH, HKDF-SHA256, AES-256-GCM) and the
	  token proof (HMAC-SHA256), on PSA Crypto.

config MG_BSIM_TESTS
	bool "BabbleSim test hooks"
	depends on SOC_SERIES_BSIM_NRFXX
	help
	  Adds the bstest entry points used by tests/bsim to press the button
	  from the simulation.

module = MG
module-str = Muse Gadget
source "subsys/logging/Kconfig.template.log_config"

endmenu
