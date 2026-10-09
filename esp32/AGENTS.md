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

How to build this firmware and flash it onto your own ESP32 board. See
`README.md` for a shorter human overview.

## What this is

The ESP32 Device SDK: the ESP-IDF firmware (`muse-gadget`) powering Muse
Home Link. It pairs with a phone over BLE, joins Wi-Fi, and holds an encrypted
Noise session to a VM, with an optional home-network tunnel. `main/main.c` is the entry
point and `main/app.c` holds most of the logic. `components/muse` adds a
avatar/voice/settings UI on boards with a display.

## Prerequisites

- **ESP-IDF v6.0.1**, installed and activated (`. $IDF_PATH/export.sh`) so
  `idf.py` is on `PATH`. Other versions are unsupported. Errors about missing
  IDF headers such as `gcm.h` or `gpio_ll.h` usually mean the wrong IDF tag.
- Python 3 and a C/C++ compiler (`cc`, `c++`) for the host tests.
- A USB data cable. Flashing needs access to the serial port. A sandboxed
  agent usually has to run flash and monitor commands outside the sandbox.

## Supported boards

Board overlays live in `devices/`, which has a `README.md` listing each board's
hardware, features, and where to buy it, and an `AGENTS.md` recipe for adding
one. Its Vendor sources table lists each vendor's code for its boards: read it
before adding a feature to one.

| Board | Target | Overlay(s) after `sdkconfig.defaults` | Helper |
|---|---|---|---|
| ESP32-C5 DevKitC-1 (default) | `esp32c5` | none | `tools/board.sh devkit` |
| ESP32-C6 devkit without PSRAM | `esp32c6` | `devices/sdkconfig.c6-nopsram` | `tools/board.sh c6-nopsram` |
| Espressif ESP32-S3-DevKitC-1 v1.1 (N8R8) | `esp32s3` | `devices/sdkconfig.espressif-s3-devkitc-1` | `tools/board.sh espressif-s3-devkitc-1` |
| ideaspark ESP32 + 1.9" ST7789 | `esp32` | `devices/sdkconfig.ideaspark` | `tools/board.sh ideaspark` |
| Waveshare ESP32-C6-LCD-1.47 | `esp32c6` | `devices/sdkconfig.waveshare-c6-lcd-147` | `tools/board.sh waveshare-c6-lcd-147` |
| Waveshare ESP32-S3-1.54inch-ePaper V2 | `esp32s3` | `devices/sdkconfig.waveshare-s3-epaper-154` | `tools/board.sh waveshare-s3-epaper-154` |
| Seeed SenseCAP Indicator | `esp32s3` | `devices/sdkconfig.sensecap-indicator` | `tools/board.sh sensecap-indicator` |
| Seeed reTerminal E1001 | `esp32s3` | `devices/sdkconfig.reterminal-e1001` | `tools/board.sh reterminal-e1001` |
| Seeed reTerminal E1002 | `esp32s3` | `devices/sdkconfig.reterminal-e1002` | `tools/board.sh reterminal-e1002` |
| Home Assistant Voice Preview Edition | `esp32s3` | `devices/sdkconfig.home-assistant-voice` | `tools/board.sh home-assistant-voice` |
| Seeed reSpeaker Lite with XIAO ESP32-S3 (experimental) | `esp32s3` | `devices/sdkconfig.seeed-respeaker-lite` | `tools/board.sh seeed-respeaker-lite` |
| Waveshare ESP32-S3-Touch-AMOLED-1.75C | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-175c` | manual (below) |
| Waveshare ESP32-S3-Touch-AMOLED-1.75 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-175` | manual (below) |
| Waveshare ESP32-S3-Touch-AMOLED-2.16 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-216` | `tools/muse/board.sh build s3-216` |
| Espressif ESP32-S3-BOX-3 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-espressif-box-3` | `tools/muse/board.sh build box3` |
| AIPI Lite | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-aipi` | manual |
| Waveshare ESP32-C6-Touch-AMOLED-1.8 | `esp32c6` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-c6-18` | manual |
| Waveshare ESP32-C6-Touch-AMOLED-2.06 | `esp32c6` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-c6-206` | `tools/muse/board.sh build c6-206` |
| Waveshare C6 1.8, BLE-only musegadgets gadget | `esp32c6` | the C6's, then `devices/sdkconfig.muse-waveshare-c6-18-ble` | `tools/muse/board.sh build c6ble` |
| Seeed SenseCAP Watcher | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-sensecap-watcher` | manual |
| M5Stack Cardputer ADV (experimental) | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-cardputer-adv` | `tools/muse/board.sh build cardputer-adv` |
| M5Stack StickS3 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-sticks3` | manual |
| M5Stack StopWatch | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-stopwatch` | manual |
| M5Stack CoreS3 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-cores3` | `tools/muse/board.sh build cores3` |
| Freenove FNK0104B | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-fnk0104b` | `tools/muse/board.sh build fnk0104b` |
| VN ESP32-S3 1.83-inch NV3023 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-vn-s3-183` | `tools/muse/board.sh build vn183` |
| Guition JC3248W535 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-guition-jc3248w535` | `tools/muse/board.sh build jc3248w535` |
| Waveshare ESP32-S3-Touch-LCD-7 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-lcd7` | `tools/muse/board.sh build lcd7` |
| M5Stack StickC Plus2 | `esp32` | `devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-stickc-plus2` | manual |
| M5Stack Core2 (v1.0) | `esp32` | `devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-core2` | `tools/muse/board.sh build core2` |
| FoloToy AI Passport (experimental) | `esp32c3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-ai-passport` | `tools/muse/board.sh build ai-passport` |

The default profile expects the C5 DevKitC-1: an addressable status LED on
GPIO27, the BOOT button on GPIO28 (active low), 8 MB flash and quad PSRAM.
The DevKitC-1's LED takes red first (`CONFIG_HOMEHUB_LED_RGB_ORDER`, on by
default). If yours shows green as red and amber as green, it takes green first
like most WS2812s: turn the option off in `idf.py menuconfig`, or in
`build-devkit/sdkconfig`, and rebuild.

## Build

Every build needs the user's SDK token (`mgst_…`, from gadgets.muse.ai >
Account > SDK tokens). Ask for it, then set `CONFIG_GADGET_SDK_TOKEN="mgst_…"`
in that build directory's `sdkconfig` (or with `idf.py menuconfig`) before
building. Without it the build warns, and the gadget will stop pairing once
Muse requires tokens. Never commit the token or print it in full.

### DevKitC-1 (default)

```sh
idf.py build                    # -> build/muse-gadget.bin, config in build/sdkconfig
```

### Other boards, with the helper

`tools/board.sh BOARD [build|flash|monitor|flash-monitor] [PORT]` sets the
target and overlays and gives each board its own `build-<board>/` directory
and `build-<board>/sdkconfig`:

```sh
tools/board.sh sensecap-indicator build
tools/board.sh ideaspark flash-monitor /dev/cu.usbserial-110
```

Without a port it uses the only matching serial port, and fails if it finds
none or more than one. If `idf.py` is not on `PATH`, it sources `export.sh`
from `$IDF_PATH`, `~/esp/esp-idf-v6.0.1`, `~/esp/esp-idf-v6` or `~/esp/esp-idf`.

### Home Assistant Voice Preview Edition

`tools/board.sh home-assistant-voice build` builds a status-and-voice gadget:
the LED ring shows the status colours, holding the centre button records a
voice note that Muse answers in the app, and the dial sets the speaker volume
(shown on the ring, kept across restarts). It advertises as
`MuseGadget-ha-voice-XXXXXX`.

- The console and flashing go through the S3's own USB-Serial-JTAG, which
  shows up as `/dev/cu.usbmodem*` like a DevKitC-1. With both plugged in, pass
  the port, and check it with `python -m esptool read-mac` before flashing.
- Flashing replaces the vendor firmware on the S3 only. The XMOS audio chip
  keeps its own firmware; never reflash it.
- While voice chat is ready, the centre button is push-to-talk. Turn the mute
  switch on to get its setup role back (pairing confirmation, the 5 s
  factory-reset hold).

### Boards with the full UI, by hand

`tools/muse/board.sh build|flash <s3|s3n|s3-216|aipi|box3|c6|c6-206|c6ble|watcher|sticks3|plus2|cardputer-adv|stopwatch|cores3|core2|jc3248w535|lcd7|vn183|ai-passport> [SERIAL|PORT]`
builds one board in `build-muse-<profile>/`, logs to
`/tmp/muse_build_<board>.log`, and clears `managed_components/` before and
after so it doesn't clash with other boards. When flashing, it finds the
board's port with `tools/muse/ports.py`, by the USB device behind it rather
than the port name, which changes when boards are re-cabled. With several
boards of one kind attached, pass the device's USB serial number (on the chip's
own USB serial port, its MAC) or the port; `tools/muse/ports.py --list` shows
them. It finds ESP-IDF the way `tools/board.sh` does, trying
`~/.espressif/esp-idf-v6.0.1` first. For an install anywhere else, set
`IDF_EXPORT` to its `export.sh`, e.g. in your shell profile:
`export IDF_EXPORT=/path/to/esp-idf/export.sh`. `tools/muse/avatar.py` builds
through `board.sh`, so it needs the same.

For bench testing, `MUSE_BENCH=1 tools/muse/board.sh build|flash ...` adds
`devices/sdkconfig.muse-bench` and uses `build-muse-<profile>-bench/`. That
turns on screenshots: `tools/muse/snap.py PORT KEYS OUT.png` sends bench keys
and saves the screen, and `>face=thinking` (or `idle`, `listening`,
`speaking`, `error`, `boot`, `off`, `happy`) in KEYS picks the avatar mode first.
Screenshots are off in normal builds because each one takes a buffer the size
of the screen. The bench overlay also logs UI timing on `mg.perf` every 5 s
(`CONFIG_MUSE_PERF`, `components/muse/muse_perf.h`): frames drawn and their
rate, frame time split into render, panel wait and flush, pixels per frame,
the avatar's frame tick, free internal RAM and each task's CPU share, plus a
line per touch (interrupt, read, first flush, frame done) and per talk press
(mode change, UI tick, first flush, frame done). On a musegadgets BLE build
`tools/mg_ble_client.py log` shows them over the Nordic UART mirror. `>face=` works in any build. Or run `idf.py` directly:

```sh
idf.py -B build-muse-aipi -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-muse-aipi/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-aipi" build
```

To flash, add `-p PORT flash` with the same `-B`, target and defaults
arguments, except on the SenseCAP Watcher: `idf.py flash` runs plain esptool,
which fails on its USB bridge (see "Flash it"). Build the Watcher with
`idf.py`, flash it with `tools/muse/board.sh flash watcher`, then
`idf.py … -p PORT monitor` as usual; reading from the bridge works. Muse builds use `partitions_muse.csv` and need 16 MB of flash or
more, except the StickS3, StickC Plus2, Cardputer ADV and AI Passport, which have 8 MB and use
`partitions_muse_8mb.csv`.

All boards share `managed_components/` and `dependencies.lock` in this
directory. If the component manager fails after you switch between a board
with the full UI and one without (lvgl is the usual offender), delete both and build again.

### A new board

Follow `devices/AGENTS.md`, which covers boards with a status light, a status
screen or the full UI from the overlay to verification. In short: copy the
closest overlay to `devices/sdkconfig.<yourboard>`, load it last, and add the
board to `devices/README.md`. At minimum set `CONFIG_IDF_TARGET`,
`CONFIG_HOMEHUB_BUTTON_GPIO`, the LED backend (`CONFIG_HOMEHUB_LED_BACKEND_*`,
or `..._NONE`), and the flash size and mode. Without PSRAM, also set
`CONFIG_SPIRAM=n`, `CONFIG_HOMEHUB_TUNNEL=n` and the mbedtls
internal-allocation options, as `devices/sdkconfig.ideaspark` does. Signed
apps on a classic ESP32 need chip revision 3.0 or later
(`CONFIG_ESP32_REV_MIN_3=y`).

## Flash

### Identify the board first

When you're asked to flash, work out which board is attached instead of asking
which one it is or assuming the default. Say which board you found and what told
you, then flash that profile. Getting it wrong writes another board's pin map,
flash size and status backend.

1. **Ask the running firmware.** A board already running this firmware names
   itself, and this needs no guessing:

   ```sh
   python3 tools/muse/chat.py --status     # {"board": "M5Stack StickS3", ...}
   ```

   It also logs the name once at startup, as `muse: board: <name>`, which
   `tools/muse/monitor.py PORT` captures because it resets the board first.
   `components/muse/muse_app.c` logs it; each `components/muse/boards/board_*.c`
   sets its `.name`. Boards with the full UI only: the other overlays don't log a name.

2. **Read the USB descriptor.** This works with no write access to the port and
   whatever the board is running, including vendor firmware or nothing.
   `ioreg -p IOUSB -l -w 0` on macOS, `lsusb -v` or `udevadm info /dev/ttyACM0`
   on Linux:

   | Descriptor | Board |
   |---|---|
   | Espressif `303a:1001`, "USB JTAG/serial debug unit" | the chip's own USB: C5, C6, S3 and the S3 boards with the full UI. Its serial number is the MAC |
   | CH340 (`1a86:7523`) | ideaspark, SenseCAP Indicator, reTerminal E1001 and E1002 |
   | CH9102 | M5Stack StickC Plus2 |
   | CH343 (`1a86:55d3`), "USB Single Serial" | Waveshare ESP32-S3-Touch-LCD-7 |
   | CH342, two `usbmodem` ports | SenseCAP Watcher: the S3 console is the one ending in `3`, the other is the Himax camera |

3. **Ask the chip.** When you can write to the port (this resets the board):

   ```sh
   python -m esptool -p PORT chip-id      # read-mac for the MAC
   ```

   The target narrows it a long way: `esp32c5` is the DevKitC-1, `esp32c6` the
   Waveshare C6 or a C6 devkit, `esp32` the ideaspark or the StickC Plus2.

4. **Fall back to a read-only capture.** If the board is mid-run and you can't
   write to the port, the `## Monitor` recipe below reads it without resetting,
   and a talk press logs the board's own button name as `muse_input: waking
   (<hint>)`:

   | Hint | Board |
   |---|---|
   | `top-right` (`yellow` on older firmware) | M5Stack StopWatch |
   | `power` | M5Stack CoreS3 |
   | `front` | M5Stack StickS3, or StickC Plus2 — tell them apart by the port: the StickS3 is native USB, the Plus2 is a CH9102 `usbserial` |
   | `top` | Waveshare ESP32-S3-Touch-AMOLED-1.75C |
   | `key` | Waveshare ESP32-S3-Touch-AMOLED-2.16 |
   | `bottom right` | AIPI Lite |
   | `wheel` | Seeed SenseCAP Watcher |
   | `boot` | Waveshare ESP32-C6-Touch-AMOLED-1.8, the ESP32-S3-Touch-AMOLED-1.75, the Guition JC3248W535 or the VN ESP32-S3 1.83-inch NV3023 — tell the C6 by its target (`esp32c6`), and the S3 boards by the `muse: board:` line a reset logs |

Ask the user only when these come up empty or contradict each other, and say
what you found and what's ambiguous rather than asking from scratch.

### Flash it

```sh
idf.py -p PORT flash                           # default DevKitC-1 build
tools/board.sh devkit flash PORT               # same, in build-devkit/
cd build && python -m esptool --chip esp32c5 -b 460800 \
  --before default-reset --after hard-reset write-flash "@flash_args"
```

Typical ports: `/dev/cu.usbmodem*` or `/dev/ttyACM*` for native USB (C5, S3 and
C6 boards), and `/dev/cu.usbserial-*`, `/dev/cu.wchusbserial*` or `/dev/ttyUSB*`
for CH340 bridges (ideaspark, SenseCAP Indicator, reTerminal E1001 and E1002). On Linux,
add yourself to the `dialout` (or `uucp`) group. If the chip won't enter the
bootloader, hold BOOT, tap RESET, release BOOT, and flash again.

The M5Stack StickC Plus2's CH9102 bridge shows up as `/dev/cu.usbserial-*` or
`/dev/ttyACM*`. It drops out above 230400 baud, so `tools/muse/board.sh`
flashes it at 230400.

The SenseCAP Watcher's bottom USB-C port has a CH342 bridge with two ports,
both `/dev/cu.usbmodem*` on macOS. The ESP32-S3 console is the second (ending
in `3`); the first is the Himax camera chip. Muse overwrites the Watcher's
factory data, so back up its `nvsfactory` partition first (see
`devices/README.md`).

The CH342 has no flow control and drops bytes when esptool sends a whole
packet at once, at any baud and on any USB port. Plain esptool then fails with
`Failed to write to target RAM (result was 0107: Checksum error)` while
uploading its stub, or `0105: The format of the received message is invalid`
on the first flash block, by which point it has erased the bootloader. That
leaves the Watcher unable to boot until a flash succeeds; the ROM loader still
answers, so it isn't bricked. `tools/muse/board.sh flash watcher` goes through
`tools/muse/paced_esptool.py`, which takes esptool's arguments and sends 64
bytes at a time at the line rate, at 115200 baud. Use it in place of
`python -m esptool` for anything that writes to the Watcher. A lower baud or
another USB port doesn't help. Pacing works with macOS's built-in driver, so
WCH's driver isn't needed.

`board.sh` prints only esptool's last three lines, so a Watcher flash shows
nothing for about three minutes. Don't interrupt it, which leaves the board
unbootable again. To watch the progress, run the wrapper from the build
directory:

```sh
cd build-muse-sensecap-watcher
python ../tools/muse/paced_esptool.py --chip esp32s3 -p PORT -b 115200 \
  --before default-reset --after hard-reset write-flash "@flash_args"
```

- `flash` writes the bootloader, the partition table (at `0x10000`, not the IDF
  default `0x8000`), otadata, phy_init and the app to `ota_0`. NVS is left
  alone, so pairing and Wi-Fi credentials survive a reflash.
- `idf.py -p PORT erase-flash` wipes everything, including pairing. Do this
  only when you mean to start from scratch.
- The app is signed with the committed `dev_signing_key.pem`, and Secure Boot
  is **not** enabled, so flashing never burns eFuses. Keep it that way: don't
  enable `CONFIG_SECURE_BOOT`, flash encryption, or
  `CONFIG_HOMEHUB_PAIRING_EFUSE_AUTH` on a board you want to keep reflashing.

## Monitor

`idf.py -p PORT monitor` (Ctrl-] to quit) resets the board and needs an
interactive terminal. An agent without a TTY can do a read-only capture that
doesn't reset the board:

```py
import os, termios, time
fd = os.open("/dev/cu.usbmodem1101", os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
attrs = termios.tcgetattr(fd)
attrs[2] &= ~termios.HUPCL   # don't toggle DTR/RTS (which resets) on close
termios.tcsetattr(fd, termios.TCSANOW, attrs)
deadline = time.monotonic() + 10
while time.monotonic() < deadline:
    try:
        chunk = os.read(fd, 4096)
        if chunk:
            os.write(1, chunk)
    except BlockingIOError:
        time.sleep(0.05)
```

To capture from boot, use `tools/muse/monitor.py PORT [secs]`, which resets the
board first (it needs `pyserial`). A healthy boot logs
`link.main: Muse Gadget starting`. For a board with the full UI, `$(tools/muse/ports.py BOARD)`
gives the port, with BOARD as in `tools/muse/board.sh`.

## Update my avatar

When someone asks to put their own avatar on their board (or to update or
change it), run this from `esp32/` with the sandbox off (it uses the serial
port) and let it finish. It takes several minutes, mostly waiting for Muse:

```sh
python3 tools/muse/avatar.py                    # draw their avatar
python3 tools/muse/avatar.py --edit "CHANGE"    # change the one they have
```

It checks the board, asks their Muse for the renderer through the board,
saves it to `components/muse/avatar/muse_pixel.c` (gitignored), checks it on
the host, then builds and flashes. Progress and errors go to stderr. Relay the
line that starts with `Muse drew:` and the GIF paths. On failure, pass the
message on. The exit status says what kind of failure it was:

- **2**: no board, or it can't do this. If it says the board doesn't answer,
  rerun with `--board s3` or `--board aipi` (from the board's name, or ask) to
  flash firmware that can. On the C6 boards or Watcher, follow the manual steps in
  `tools/muse/AVATAR_RECIPE.md`.
- **3**: the board isn't on Wi-Fi, or it isn't paired in the Muse app and has
  no device token, so their Muse isn't connected. Tell them, and point them to
  the Muse app or `tools/muse/ble_setup.html`.
- **1**: Muse's file still fails after two fix rounds, or the build or flash
  failed. The previous avatar is in `muse_pixel.c.prev`, next to the new one.

Don't commit anything in `components/muse/avatar/`. To see what the board
says, run `python3 tools/muse/chat.py --status`. To ask their Muse something,
run `python3 tools/muse/chat.py "question"`.

The default avatar is in `avatar/`: its renderer (`muse_pixel.c`) and
its animation (`jollybot.gif`, and `happy_anim.c/.h` made from it by
`tools/gen_happy_anim.py`).

Third-party code keeps its upstream license and header: `minimp3.h` (CC0) and
`main/pixel_font.c` (BSD-2-Clause, Adafruit). Don't restyle them or replace
their headers with the Apache one; `components/minimp3/README.md` says how to
update minimp3.

The Apache License doesn't cover the Jollybot avatar in `avatar/`. Its files
carry only a Meta copyright line; don't add the Apache header to them.

## First boot and pairing

The status LED (or the edge bars or avatar on display boards) shows the state:

| Colour | Meaning |
|---|---|
| orange, breathing | BLE advertising, waiting for setup |
| blue, breathing | pairing needs a physical button press |
| blue | Wi-Fi or VM session coming up |
| green | connected (tunnel up, or control session up when the tunnel is off) |
| yellow, blinking | VM switching or connection lost |
| purple | unpaired |
| red, blinking | error |

Button (BOOT on the dev boards):

- **short press**: confirm a pending pairing, or reopen the setup window if
  setup isn't complete
- **hold for 5 s**: reset setup (unpair and forget Wi-Fi)

On boards with the full UI, a test rig confirms a pairing over the serial
console instead of a finger on the button: see Automated setup tests under
musegadgets BLE audio.

The device advertises as `MuseGadget-XXXXXX` (`MuseGadget-Disp-XXXXXX` on the
ideaspark, SenseCAP Indicator and reTerminal E1001 and E1002 overlays, `MuseGadget-ha-voice-XXXXXX` on the
Voice PE, `MuseGadget-respeaker-XXXXXX` on the reSpeaker Lite). It uses
**community pairing v5**, so the phone app must support v5
and list community devices. Community pairing needs the button press but has no
manufacturer attestation, and it doesn't stop an active man-in-the-middle.

Until it's paired, a board with the full UI shows that name on its screen, dim under the
state, so you can tell which gadget to pick in the Muse app. A screen too narrow
for the whole name shows the `XXXXXX` tail on its own, and a square 128 px
screen (AIPI Lite) leaves it out, the same as it leaves out the state. Once it's
paired (or has a token set by hand), the name goes, and the mic icon and the
touch boards' speaker button appear. They're hidden until then, since a press
can't reach Muse and there are no replies to mute. The StopWatch is an exception:
its dedicated microphone, speaker/mute and power rim icons stay visible before
pairing as well, identifying its three physical controls.

To skip BLE Wi-Fi provisioning while you iterate, set
`CONFIG_HOMEHUB_WIFI_SSID` and `CONFIG_HOMEHUB_WIFI_PASSWORD` in `menuconfig`.
The device still needs to be paired once for its token.

## musegadgets BLE audio

`components/muse_gadget_ble` implements the musegadgets BLE protocol
(`../protocols/mgcommands.h`, and `mgcommands-secure.h` for session security):
a phone, browser or gateway connects over BLE, turns on push-to-talk, and gets
each utterance as SBC (the default) or LC3. Its `README.md` covers the design.
Include the protocol headers; never copy their constants.

- **Turn it on** with `CONFIG_MUSE_GADGET_BLE_AUDIO=y` in the board's overlay
  (the Waveshare S3 1.75C has it, with session security) or under "Muse Gadget BLE audio" in
  `idf.py -B <dir> menuconfig`. Off, nothing changes. It needs a board with a
  mic to be useful: the full-UI boards with PSRAM, and the Voice PE.
- **Options:** the routing policy (`CONFIG_MUSE_GADGET_BLE_ROUTE_AUTO`,
  `_BLE_ONLY`, `_WIFI_ONLY`), `CONFIG_MUSE_GADGET_BLE_LC3` (default on the S3
  and ESP32 only: LC3 is floating point and the C6 and C5 have no FPU),
  `CONFIG_MUSE_GADGET_BLE_SECURE` (session security; default off, on for the
  Waveshare S3 1.75C, whose clients must pair once),
  `CONFIG_MUSE_GADGET_BLE_QUEUE` (offline clips; needs the `mg_queue`
  partition in `partitions_muse.csv`), and `CONFIG_MUSE_GADGET_BLE_NUS_LOG_TAGS`.
- **It shares Link's BLE server** (`main/ble_server.c`) as a companion, like
  Muse's phone setup: keep `ble_server_set_companion()` to two companions, and
  keep Link's setup advertising and `ble_server_full_shutdown()` behaviour
  unchanged for builds without it.
- **Setup and device tokens** go through Link's BLE setup, not mgcommands
  (`../protocols/README.md`, Gadget setup over Muse Link). Gadget builds add
  `wifi` (`"optional"`, or `"none"` BLE-only) and `mgcommands: 1` to
  `device_info` (`link_pairing_set_gadget()`); `provision_v2` without an ssid
  is token-only: the pair in Link's `access_token` / `refresh_token`, the
  token proof key `mg_proof_k` (hex), `setup_wifi=skipped` and
  `setup_complete`, all in one commit under the provisioning session
  (`commit_setup()` in `main/app.c`), `auth_ok` only after it, then Link
  setup advertising stops while musegadgets advertising goes on. A Wi-Fi
  setup on a gadget stores the key too. Boot recovery keeps a token-only
  setup; reset (hold, the Muse menu, or the token proof's clear) erases all
  of it. Never log the tokens or the key; lengths and a SHA-256 prefix are
  fine. Builds without gadget support behave as before (no `wifi` field,
  Wi-Fi required).
- **Token proof:** `mg_command_token_proof` (34) on every gadget build
  (`mg_token_proof.c`, PSA HMAC): the client proves it holds the key setup
  left, without sending the token. It gates nothing; `clear` after a match
  resets setup and restarts the board into Link setup advertising. On a
  secure build it runs on Encrypted Control after authentication.
- **BLE only:** `CONFIG_MUSE_GADGET_BLE_STANDALONE=y` makes a board a
  musegadgets gadget and nothing else: `app_run()` hands over to
  `run_ble_standalone()` before Wi-Fi, so Wi-Fi, the Muse session, the
  tunnel and OTA never start, and push-to-talk goes to the client or the
  offline queue (policy BLE only). Link setup runs without Wi-Fi: until the
  gadget is set up (the setup marker and the proof key; anything less is
  wiped at boot) it advertises setup, taking turns with the musegadgets UUID
  from the same address, and accepts community pairing v5 (the talk button
  confirms), token-only `provision_v2`, `device_info` and `unpair`. The
  Waveshare C6 has a profile for it: `tools/muse/board.sh build c6ble`
  (`devices/sdkconfig.muse-waveshare-c6-18-ble`, SBC only). A healthy boot
  logs `musegadgets BLE ready` and `BLE-only musegadgets gadget`, and the
  heartbeat shows the setup stage and free internal RAM.
- **Playback:** boards with a speaker (the boards with the full UI) offer
  `mg_command_stream_audio`: SBC, PCM and (with LC3 on) LC3 from the client,
  at 8 to 48 kHz resampled to 16 kHz, into a 16 KB ring
  (`CONFIG_MUSE_GADGET_BLE_PLAY_BUFFER`) with buffer_update flow control.
  Push-to-talk ends a stream with stop_streaming drop. The client reads and
  sets the Muse speaker level with the `speaker_volume` setting (0 = off:
  playback is then silent). Each stream logs its stats on `mg.play` (on the
  Nordic UART mirror), including the level that reached the speaker. The
  component README has the details, and what clients must do about their
  stack's Write Without Response back-pressure.
- **Face:** boards with a display or status light take `assistant_state`
  from the client: listening, thinking after push-to-talk, then responding
  (speaking), done (happy, then idle) or error, as the Wi-Fi path does, and
  speaking while a client's audio plays; idle after 60 s without an update.
  Face changes are logged on `mg.face` (on the Nordic UART mirror). Only
  boards with a vibration motor (none yet) list `haptics_enabled` and the
  `ptt_buzz_*` settings; `request_status` lists every accepted setting.
- **Bench client:** `tools/mg_ble_client.py` (needs `pip install bleak` and a
  C compiler; `setup` also `pip install bleak cryptography`) is a client for a
  Mac or Linux box:
  `scan`, `test` (Device Information, Battery, request_status, settings, a
  start_mic capture decoded to `mg_capture.wav`, queue status, and a 2 s
  playback on boards with a speaker), `play` (`--tone HZ --secs N`,
  `--sweep`, `--wav FILE`; `--volume N` sets the speaker level first;
  `--loopback` records the mic while it plays and checks the tone comes
  back, saving `mg_loopback.wav`), `ptt` (records
  each button press to a WAV, then sends responding and done and prints the
  face changes; `--no-states` skips that), `state <name>` (one
  assistant_state), `setup [--access A --refresh R] [--confirm-serial PORT]`
  (Link setup as the Muse app does it: community pairing v5, a press of the
  gadget's button, or `>pair.confirm` on its console with `--confirm-serial`,
  then token-only `provision_v2` with the given or generated `bench-...` tokens;
  it keeps the proof key in `~/.mg_ble_client/`, mode 0600), `proof
  [--clear]` (the token proof with that key; `--clear` resets the gadget),
  `queue --download` and `log` (the Nordic UART
  mirror), `bench [--send SECS] [--recv SECS]` (raw throughput both ways with
  `device_action throughput_test`), and `images` / `dfu` (MCUmgr SMP updates
  on the Zephyr gadget). `test` round-trips every setting the board lists, checks the others
  are refused, and runs the token proof when `setup` left a key for the
  gadget. On macOS the terminal needs Bluetooth permission; a sandboxed agent
  usually can't use Bluetooth.
- **Automated setup tests:** Link setup waits up to 60 s for the button
  after `confirm_required`; on a rig the USB serial console presses it, so
  setup runs with nobody at the bench:
  - Boards with the full UI (`components/muse`, the BLE-only C6 build
    included) print `@pair.pending` when a confirmation starts waiting, and take the
    console line `>pair.confirm`: the talk button's setup press
    (`muse_link_talk_press()`), never push-to-talk. They answer
    `@pair.confirm confirmed`, or `@pair.confirm none` when nothing waits.
    The `d` / `u` keys are push-to-talk only and never confirm. The Zephyr
    gadget's bench build takes the same line (`zephyr/AGENTS.md`, Bench build).
  - `tools/mg_confirm.py PORT` watches a console for `@pair.pending` and
    answers it (`--count 0 --secs N` keeps answering, `--log FILE` saves the
    console, `--now` confirms what waits now); start it, then run the Muse
    app or `tools/mg_ble_client.py setup` as usual. `mg_ble_client.py setup
    --confirm-serial PORT` does both in one process. A sandboxed agent that
    can use Bluetooth but not serial ports (or the reverse) runs the two as
    separate processes.
  - Both open the port without changing DTR or RTS, which would reset the
    board, and reopen it if the board restarts.
  - The console is the USB cable, so this asks for the same physical access
    as the button, like the console's other setup commands. Light and
    status-screen boards have no console input: their button stays the only
    way to confirm.
- **Pairing a client** (secure builds): pairing mode is on while no client is
  paired, and for two minutes after turning on BLE phone setup. The talk button
  confirms; boards with the full UI show the code on the pairing card. The
  bench client speaks plaintext only.
- **The Wi-Fi path stays as it was.** The push-to-talk loops only call into
  `mg_voice.h` under `#if CONFIG_MUSE_GADGET_BLE_AUDIO`.
- **Test** with the host tests below: `test_mg_ble` (protocol, routing,
  queue, the token proof against `../protocols/test-vectors/mg-token-proof-v1.json`),
  `test_mg_token_proof` (the same on the device's PSA code; needs `IDF_PATH`
  like `test_link_pairing_handshake`), `test_link_gadget_setup` (Wi-Fi-optional
  `provision_v2`, the token-only commit and its rollback, BLE-only boot
  recovery), `test_mg_codecs` (SBC and LC3 round trips), `test_mg_secure`
  (session security against `../protocols/test-vectors/mgcommands-secure-v1.json`,
  the token proof on Encrypted Control; needs `IDF_PATH`), `test_mg_play`
  (playback: resampler, ring, decoders), `test_mg_ble_client` (the bench
  client's parsing and decoding, its `play` flow control and token proof
  against the firmware's protocol code, and its Link setup crypto against
  `tests/vectors/link_pairing_v5.json`, which needs `cryptography`; setup
  with `--confirm-serial` against a console on a pty) and `test_mg_confirm`
  (`tools/mg_confirm.py` against a simulated console, and `>pair.confirm` /
  `@pair.pending` in both SDKs' firmware). Build the
  board with it on and one without it. BLE isn't emulated in QEMU; on a board,
  use `tools/mg_ble_client.py`.
- **Vendored codecs** (`../xplat/libsbc`, `../xplat/liblc3`) are
  Google's Apache-2.0 code, one copy shared with the Zephyr SDK;
  `components/libsbc` and `components/liblc3` only build them. Keep their
  `LICENSE` files and the modification notes in their headers, change them
  only through compile-time options both SDKs can choose, and never vendor
  LGPL code such as BlueZ's SBC.

## Configuration gotchas

- Each build's generated `sdkconfig` lives in its build directory
  (`build/sdkconfig`, `build-<board>/sdkconfig`). Once it exists, it overrides
  later edits to `sdkconfig.defaults` or the overlays. After changing those,
  delete the generated `sdkconfig` (or the whole build directory) and rebuild.
  `cmake/validate_config.cmake` stops the build if a stale config has the old
  TCP buffer sizes or cJSON nesting limit.
- Edit per-build settings with `idf.py -B <dir> menuconfig` (under "ESP32
  Device SDK"). Don't commit a generated `sdkconfig`: it's gitignored for a reason.
- OTA is off by default (`CONFIG_HOMEHUB_OTA_ENABLED=n`, version `999.0.0`),
  except on boards with the full UI. Set a version with `-DPROJECT_VER=1.0.0` or
  `version.txt`.
- Don't move offsets in `partitions.csv`. `prod_data` and `prod_bak` are fixed
  manufacturing locations, and the table offset of `0x10000` leaves room for a
  larger Secure Boot bootloader. Check the `check_sizes` line in the build
  output: app slots are 2 MB (4 MB on Muse).

## Adding a command

Muse calls a gadget's commands by name: the firmware lists them in
`link.register` and answers each `link.invoke`. A command you add lives in two
places, which must use the same name. (`device.health` and `device.ota` are
handled in `noise_control.cpp` itself; everything else goes through
`on_ws_command()`.)

1. **Advertise it** in `build_register_json()` in `main/noise_control.cpp`:
   `add_command(commands, "relay.set", "<description>", required, optional)`.
   `required` and `optional` map each parameter's name to `{type,
   description}` (`string_param()` makes a string one). Muse reads the
   descriptions, so say what the command does and what it returns. If it can
   take longer than the default 30 seconds, set its `timeout_ms`, as
   `device.discover` does.
2. **Handle it** in `on_ws_command()` in `main/app.c`. Return
   `{"ok": true, "payload": {...}}`, or `command_error(code, message)` for a
   failure. Only `ok`, `payload` (or a `payload_json` string) and the
   error's `message` reach the Muse. Always return a result: `NULL` reaches
   the Muse as a generic "command handler did not return a result" error.
   Validate the parameters yourself: `params` is `NULL` when the request has
   none, and the firmware doesn't check them against the advertised
   `required` and `optional`, so check each one's presence, type, length and
   allowed values.
3. **Don't block.** `on_ws_command()` runs on the Noise session's task, so
   anything slow (the network, a slow sensor, a camera) belongs in its own
   task. Copy `request_id`, `session_generation` and every parameter the task
   needs, strings included, into memory the task owns (`device.discover`
   uses `cJSON_Duplicate()`): the request is freed as soon as
   `on_ws_command()` returns. If an allocation or the task start fails, free
   what you allocated and return `command_error()`. Otherwise start the task
   and return `{"_async": true}`. The task then calls
   `noise_ctrl_send_command_result()` once, failures included, or the Muse
   waits out the timeout. It takes ownership of the result and frees it, so
   don't free or reuse it afterwards. `camera.capture` and `device.discover`
   work this way. A result from an earlier session is dropped.
4. **Gate it on a Kconfig option** in `main/Kconfig.projbuild` when it needs
   particular hardware, and wrap both places in the same `#if`, as
   `sensors.read` does with `CONFIG_HOMEHUB_SENSECAP_SENSORS`. Add new source
   files to `main/CMakeLists.txt`.
5. **Keep `link.register` small.** It's printed into at most 8 KB, and a
   device whose registration doesn't fit never registers.
6. **Add a host test** in `tests/`. `test_link_sensecap_sensors.py` checks
   that `sensors.read` is advertised and dispatched under the same option, and
   runs its parser against a harness.

Report environmental sensors (temperature, humidity, CO2, tVOC, light and so
on) through `sensors.read`, on every board that has them, so Muse has one
command to call wherever it runs. Don't add them to `device.health`, which is
for the link itself (battery, power, network), or give them a board-specific
command. Return each reading as `{value, unit, age_s}`, as
`sensecap_sensors.c` does, and report a sensor with no recent reading as null.
Read slow or I2C sensors in a background task and have `sensors.read` return
the latest value.

Muse sees the command once the board reconnects with the new firmware. Keep
the management commands that `on_ws_command()` also handles (`device.list_vms`,
`device.set_vm`, `device.reset_vm` and `device.unpair`) out of
`link.register`: `tests/test_link_transport_contract.py` checks they stay
unadvertised.

## Say Muse, never Hatch

Users never see the name Hatch.

- Anything a person reads says Muse, the Muse app, or the Muse's name:
  - screen text and error captions (`CAN'T REACH MUSE`, not `CAN'T REACH HATCH`)
  - settings labels
  - Kconfig prompts and help
  - log lines
  - tool and script output
  - docs
- Don't use `hatch` in a new file name or identifier. Use `muse` or
  `muse_gadget`. The Muse chat code is `components/muse/muse_chat*`.
- The ESP32 account clients use `https://api.muse.ai`, or the `api_url_v2`
  base the app sends during pairing, with bare API paths. They ignore
  `api_url`: only older firmware reads it, and that firmware adds `/hatch/`
  itself. Keep saving it so a device flashed back to older firmware still works.
- `hatch` stays only where the server or the Muse app depends on it. Don't
  rename these:
  - the VM host `hatch.metaaivm.com`
  - the `hatch_refresh:` auth prefix, the `hatch-web` app id and the
    `HatchLink/` user agent
  - pairing labels and ids such as `hatch-link-pairing-v%d`, the `hatch_link`
    model and the `hatch-link:` device id
  - the bug report fields
  - the setup commands (`hatch.token` and the rest) and the `"hatch"` key
    in the status JSON, which the app and older tools use
- Some older identifiers still carry the name (`muse_hatch_*`,
  `MUSE_HATCH_*`, `CONFIG_MUSE_HATCH`). Leave them unless you're asked to
  rename them. Don't copy the name into new code.

## Tests

These are host-side Python unittest harnesses that compile the firmware C/C++
against fakes, with no board or IDF environment required:

```sh
python3 -m unittest discover -s tests -p 'test_*.py'
```

Run one `idf.py build` first: `test_link_discovery` compiles cJSON from
`managed_components/`, and that directory only exists after a build. Set `CC`
or `CXX` to change compilers. Set `IDF_PATH` (activating ESP-IDF does) so the
real-crypto tests, `test_link_pairing_handshake`, `test_mg_token_proof` and
`test_mg_secure`, run
instead of skipping.

Two tests skip quietly when their inputs are missing; check the summary for
`skipped=`:

- `test_noise_core` links against the host's PSA Crypto library, found with
  `pkg-config mbedcrypto`. Install `libmbedtls-dev` and `pkg-config` on Debian
  or Ubuntu (as CI does), or `mbedtls` and `pkgconf` with Homebrew.
- `test_link_pairing_handshake` builds Mbed TLS from source for its
  real-crypto case, so it needs `IDF_PATH` (set by `export.sh`) or
  `MBEDTLS_SOURCE_DIR`.

## Before you hand back work

1. `idf.py build` (and the board build you touched) passes, with the size check
   under the slot limit.
2. The host tests pass.
3. If you flashed, the boot log reaches `starting`, and the log shows no
   panic or reboot loop.
