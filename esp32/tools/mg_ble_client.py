#!/usr/bin/env python3
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

"""A musegadgets BLE client for bench tests (protocols/mgcommands.h).

    python3 tools/mg_ble_client.py scan                   gadgets advertising the service
    python3 tools/mg_ble_client.py test [--secs 2]        the protocol checks below, and a WAV
    python3 tools/mg_ble_client.py ptt [--count N]        record each button push-to-talk to a WAV,
                                                         then drive the face: responding, done
    python3 tools/mg_ble_client.py state thinking         send one assistant_state (idle, thinking,
                                                         responding, done, error)
    python3 tools/mg_ble_client.py setup [--access A --refresh R]
                                                         set a gadget up over Muse Link setup:
                                                         pairing (press its button), token-only
                                                         provision_v2; keeps the proof key
    python3 tools/mg_ble_client.py proof [--clear]        the token proof with the kept key; --clear
                                                         then resets the gadget to setup
    python3 tools/mg_ble_client.py queue [--download]     list (and fetch) offline clips
    python3 tools/mg_ble_client.py log [--secs 30]        print the Nordic UART log mirror
    python3 tools/mg_ble_client.py play [--tone 1000 --secs 3 | --sweep | --wav FILE] [--loopback]
                                                         stream audio to the gadget's speaker
    python3 tools/mg_ble_client.py bench [--send SECS] [--recv SECS]
                                                         raw link throughput (default 10 s each way)
    python3 tools/mg_ble_client.py images                MCUboot image slots (Zephyr gadgets, SMP)
    python3 tools/mg_ble_client.py dfu --image zephyr.signed.bin [--test | --confirm] [--reset]
                                [--window N] [--chunk B] upload a firmware image over SMP

Pick a gadget with --address (from scan) or --name (a name prefix, default
MuseGadget); otherwise the first one found is used. `test` connects, reads
Device Information and Battery, subscribes, sends request_status, reads and
sets settings, captures --secs of audio with start_mic/stop_mic, checks the
codec frames decode, saves the WAV, and asks for the offline queue's status.
With a proof key from `setup` for this gadget it runs the token proof too.
It prints PASS/FAIL per step and exits 1 if any failed; on a gadget with a
speaker it also plays a 2 s tone (as `play` does).

`setup` does what the Muse app does for a gadget (protocols/README.md, Gadget
setup over Muse Link), against a gadget advertising the Link setup service
(not yet set up): device_info (it must say wifi optional or none), community
pairing v5 (P-256 ECDH, HKDF-SHA256, AES-GCM records, as
esp32/main/link_pairing.c), a press of the gadget's button, then token-only
provision_v2 with --access/--refresh or generated bench-... test tokens. It
prints the node_id and keeps K, the token proof key, in
~/.mg_ble_client/proof-<address>.json (mode 0600; MG_BLE_CLIENT_HOME moves
it), never the tokens. `proof` runs the token proof against that key on the
musegadgets service: challenge, the device's MAC checked, confirm, result;
--clear then has the gadget erase its tokens and key and go back to setup.
Bench tokens aren't minted by Muse: they only exercise the gadget.

`play` streams SBC to the speaker (mg_command_stream_audio): capabilities,
start_streaming, Data writes under the buffer_update flow control, stop
keep, and the device's own stop_streaming; then the device's per-stream
stats from the Nordic UART mirror (decoded level, and the level that reached
the speaker's codec, which is silence while the speaker is off). --volume N
sets the gadget's speaker_volume first. --loopback also records the gadget's
mic while it plays and checks the tone comes back (frequency and level
against a silent baseline), saving the capture WAV; it fails at once, saying
why, if the speaker is off.

`bench` runs device_action throughput_test: --send has the gadget stream
numbered MTU-3 byte notifications for SECS seconds, --recv writes them to the
gadget as fast as the stack takes Write Without Response (CoreBluetooth's
back-pressure honoured), and each prints the rate on both sides, missing or
corrupt packets, and for --send how many packets arrive together (about one
connection event's worth). Without either it runs both, 10 s each.

`images` and `dfu` speak MCUmgr SMP to gadgets with MCUboot (the Zephyr
gadget): `images` lists both slots (version, hash, active, confirmed,
pending); `dfu` uploads a signed image to the second slot, then --test marks
it to run once at the next reset (the gadget confirms it when it comes up
healthy, or MCUboot swaps back at the reset after), --confirm marks it
permanent, and --reset restarts the gadget to install it (with --test or
--confirm it then waits for the gadget to come back and checks the new image
runs confirmed). It sends requests as large as the gadget's SMP buffer (from
its MCUmgr parameters; --chunk caps them) and keeps --window of them in
flight (default 2 when the gadget has four buffers or more), and prints the
throughput and how long the upload, the reset and swap, and the confirmation
took. Uploading drops the gadget's offline clips (they share the slot):
download them first. `queue --download` prints each clip's download speed.
They need smpclient (`pip install smpclient`), imported only for these two.

Needs bleak (`pip install bleak`) and a C compiler; `setup` also needs
cryptography (`pip install bleak cryptography`), imported only there. Audio is decoded with the
firmware's own codecs (xplat/libsbc, xplat/liblc3), built on first
use into ~/.cache/mg_ble_client. The UUIDs and command numbers are read from
protocols/mgcommands.h, never copied. On macOS, the app running this (e.g.
Terminal) needs Bluetooth permission in System Settings > Privacy & Security.
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import ctypes
import hashlib
import hmac
import json
import math
import os
import re
import secrets
import shlex
import struct
import subprocess
import sys
import time
import wave
from pathlib import Path

ESP32 = Path(__file__).resolve().parents[1]
PROTOCOLS = ESP32.parent / "protocols"


# ---- the protocol, from its header ------------------------------------------

def load_protocol(headers: tuple[Path, ...] = (PROTOCOLS / "mgcommands.h",)) -> dict:
    """UUID strings and enum values from mgcommands.h, by name."""
    text = "\n".join(h.read_text() for h in headers)
    names: dict[str, object] = {}
    for m in re.finditer(r"#define\s+(MG_\w+?)\s+\"([0-9A-Fa-f-]{36})\"", text):
        names[m.group(1)] = m.group(2).lower()
    for m in re.finditer(r"#define\s+(MG_\w+)\s+(0x[0-9A-Fa-f]+|\d+)\b", text):
        names[m.group(1)] = int(m.group(2), 0)
    for m in re.finditer(r"#define\s+(MG_DEVICE_NAME_PREFIX)\s+\"([^\"]+)\"", text):
        names[m.group(1)] = m.group(2)
    for m in re.finditer(r"^\s*(mg_\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*,", text, re.M):
        names[m.group(1)] = int(m.group(2), 0)
    return names


P = load_protocol()


def uuid16(value: int) -> str:
    return f"0000{value:04x}-0000-1000-8000-00805f9b34fb"


def command_name(value: int) -> str:
    for k, v in P.items():
        if k.startswith("mg_command_") and v == value:
            return k[len("mg_command_"):]
    return f"0x{value:02x}"


def data_type_name(value: int) -> str:
    for k, v in P.items():
        if k.startswith("mg_data_type_") and v == value:
            return k[len("mg_data_type_"):]
    return f"type {value}"


def parse_change_data_type(payload: bytes) -> dict:
    """[change_data_type, type, rate, channels, frame bytes, frame us], the format optional."""
    fmt = {"type": payload[1], "rate": P["MG_AUDIO_DEFAULT_SAMPLE_RATE_HZ"],
           "channels": P["MG_AUDIO_DEFAULT_CHANNELS"], "frame_bytes": 0, "frame_us": 0}
    if len(payload) >= 4:
        fmt["rate"] = int.from_bytes(payload[2:4], "little")
    if len(payload) >= 5:
        fmt["channels"] = payload[4]
    if len(payload) >= 7:
        fmt["frame_bytes"] = int.from_bytes(payload[5:7], "little")
    if len(payload) >= 9:
        fmt["frame_us"] = int.from_bytes(payload[7:9], "little")
    if fmt["type"] == P["mg_data_type_audio_lc3"]:
        fmt["frame_bytes"] = fmt["frame_bytes"] or P["MG_LC3_DEFAULT_FRAME_BYTES"]
        fmt["frame_us"] = fmt["frame_us"] or P["MG_LC3_DEFAULT_FRAME_US"]
    return fmt


# Byte width of each setting's value (mg_setting_parameter_t).
SETTING_WIDTH = {
    "spec_version": 1, "push_to_talk_enabled": 1, "haptics_enabled": 1, "ptt_buzz_freq_hz": 2,
    "ptt_buzz_duration_ms": 2, "ptt_buzz_volume_percent": 1, "audio_codec": 1, "audio_queue_enabled": 1,
    "speaker_volume": 1,
}


FACE_RE = re.compile(r"mg\.face: (?:face |ring )?(.+?)\s*$")

ASSISTANT_STATES = ("idle", "thinking", "responding", "done", "error")


def assistant_state(name: str) -> int:
    return P["mg_assistant_state_" + name]


def setting_name(sid: int) -> str:
    for n in SETTING_WIDTH:
        if setting_id(n) == sid:
            return n
    return f"setting {sid}"


def setting_id(name: str) -> int:
    return P["mg_setting_parameter_" + name]


def parse_settings(payload: bytes) -> dict:
    """[get_settings, (param, value)...] -> {name: value}."""
    by_id = {setting_id(n): n for n in SETTING_WIDTH}
    out, i = {}, 1
    while i < len(payload):
        name = by_id.get(payload[i])
        if not name:
            break
        w = SETTING_WIDTH[name]
        out[name] = int.from_bytes(payload[i + 1:i + 1 + w], "little")
        i += 1 + w
    return out


def parse_error(payload: bytes) -> str:
    cmd = command_name(payload[1]) if len(payload) > 1 else "?"
    code = int.from_bytes(payload[3:5], "little") if len(payload) >= 5 else 0
    names = {v: k[len("mg_error_code_"):] for k, v in P.items() if k.startswith("mg_error_code_")}
    return f"error on {cmd}: {names.get(code, code)}"


# ---- decoding with the firmware's codecs ------------------------------------

SHIM = r"""
#include <string.h>
#include "sbc.h"
#include "lc3.h"
static sbc_t s_sbc;
static lc3_decoder_mem_16k_t s_mem;
static lc3_decoder_t s_lc3;
void mg_reset(void) { sbc_reset(&s_sbc); s_lc3 = lc3_setup_decoder(10000, 16000, 0, &s_mem); }
/* Whole SBC frames in buf: returns samples, *used the bytes taken (-1 samples: not SBC). */
int mg_sbc(const unsigned char *buf, int len, short *out, int cap, int *used) {
    int o = 0, u = 0;
    while (len - u >= SBC_PROBE_SIZE) {
        struct sbc_frame f;
        if (sbc_probe(buf + u, &f)) { if (!o) { *used = u; return -1; } break; }
        int fs = (int)sbc_get_frame_size(&f), n = f.nblocks * f.nsubbands;
        if (fs <= 0 || len - u < fs || o + n > cap) break;
        if (sbc_decode(&s_sbc, buf + u, fs, &f, out + o, 1, NULL, 0)) break;
        o += n; u += fs;
    }
    *used = u; return o;
}
/* SBC encoder: mono, 16 blocks, 8 subbands, loudness. Returns the frame size. */
static sbc_t s_enc;
static struct sbc_frame s_ef;
int mg_sbc_enc_setup(int rate, int bitpool) {
    memset(&s_ef, 0, sizeof(s_ef));
    s_ef.freq = rate == 16000 ? SBC_FREQ_16K : rate == 32000 ? SBC_FREQ_32K : rate == 44100 ? SBC_FREQ_44K1
              : rate == 48000 ? SBC_FREQ_48K : -1;
    if ((int)s_ef.freq < 0) return -1;
    s_ef.mode = SBC_MODE_MONO; s_ef.bam = SBC_BAM_LOUDNESS; s_ef.nblocks = 16; s_ef.nsubbands = 8;
    s_ef.bitpool = bitpool;
    sbc_reset(&s_enc);
    return (int)sbc_get_frame_size(&s_ef);
}
/* 128 samples -> one frame. */
int mg_sbc_enc(const short *pcm, unsigned char *out, int cap) {
    if (sbc_encode(&s_enc, pcm, 1, NULL, 0, &s_ef, out, cap)) return -1;
    return (int)sbc_get_frame_size(&s_ef);
}
/* One 10 ms, 16 kHz LC3 frame of `len` bytes (NULL: conceal a lost one). */
int mg_lc3(const unsigned char *buf, int len, short *out) {
    if (!s_lc3) return -1;
    return lc3_decode(s_lc3, buf, len, LC3_PCM_FORMAT_S16, out, 1) < 0 ? -1 : 160;
}
"""

LC3_SRCS = ("attdet.c", "bits.c", "bwdet.c", "energy.c", "lc3.c", "ltpf.c", "mdct.c", "plc.c", "sns.c",
            "spec.c", "tables.c", "tns.c")
LC3_DEFS = ("LC3_ENC_LTPF=0", "LC3_DEC_LTPF=0", "LC3_PLUS=0", "LC3_PLUS_HR=0", "LC3_DT_MASK=8",
            "LC3_SR_MASK=2")


def decoder_sources() -> list[Path]:
    sbc = ESP32.parent / "xplat/libsbc/src"
    lc3 = ESP32.parent / "xplat/liblc3/src"
    return [sbc / "sbc.c", sbc / "bits.c", *(lc3 / f for f in LC3_SRCS)]


class Decoder:
    """SBC and 16 kHz / 10 ms LC3, through ctypes."""

    def __init__(self, cache: Path | None = None):
        cache = cache or Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "mg_ble_client"
        cache.mkdir(parents=True, exist_ok=True)
        srcs = decoder_sources()
        digest = hashlib.sha256(SHIM.encode())
        for s in srcs:
            digest.update(s.read_bytes())
        lib = cache / f"mgdec-{digest.hexdigest()[:12]}{'.dylib' if sys.platform == 'darwin' else '.so'}"
        if not lib.exists():
            shim = cache / "shim.c"
            shim.write_text(SHIM)
            sbc_dir, lc3_dir = ESP32.parent / "xplat/libsbc", ESP32.parent / "xplat/liblc3"
            cc = shlex.split(os.environ.get("CC", "cc"))
            cmd = [*cc, "-O2", "-shared", "-fPIC", "-w", "-o", str(lib),
                   "-I", str(sbc_dir / "include"), "-I", str(sbc_dir / "src"),
                   "-I", str(lc3_dir / "include"), "-I", str(lc3_dir / "src"),
                   *[f"-D{d}" for d in LC3_DEFS], str(shim), *map(str, srcs), "-lm"]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode:
                raise RuntimeError("building the decoder failed:\n" + r.stderr[-4000:])
        self.lib = ctypes.CDLL(str(lib))
        self.lib.mg_sbc.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(ctypes.c_short), ctypes.c_int,
                                    ctypes.POINTER(ctypes.c_int)]
        self.lib.mg_lc3.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(ctypes.c_short)]
        self.lib.mg_sbc_enc.argtypes = [ctypes.POINTER(ctypes.c_short), ctypes.c_char_p, ctypes.c_int]
        self.reset()

    def sbc_encode(self, pcm: list[int], rate: int = 16000, bitpool: int = 26) -> tuple[list[bytes], int]:
        """Mono PCM -> SBC frames (16 blocks, 8 subbands, loudness), padded to whole frames."""
        fb = self.lib.mg_sbc_enc_setup(rate, bitpool)
        if fb <= 0:
            raise ValueError(f"SBC can't encode {rate} Hz")
        frames, buf = [], ctypes.create_string_buffer(fb)
        pcm = list(pcm) + [0] * (-len(pcm) % 128)
        for i in range(0, len(pcm), 128):
            block = (ctypes.c_short * 128)(*pcm[i:i + 128])
            if self.lib.mg_sbc_enc(block, buf, fb) != fb:
                raise ValueError("SBC encode failed")
            frames.append(buf.raw)
        return frames, fb

    def reset(self) -> None:
        self.lib.mg_reset()

    def sbc(self, data: bytes) -> tuple[list[int], int]:
        """Decodes whole SBC frames from data; returns (samples, bytes used)."""
        out = (ctypes.c_short * (len(data) * 4 + 256))()
        used = ctypes.c_int(0)
        n = self.lib.mg_sbc(data, len(data), out, len(out), ctypes.byref(used))
        if n < 0:
            raise ValueError("not SBC")
        return list(out[:n]), used.value

    def lc3(self, frame: bytes | None, frame_bytes: int) -> list[int]:
        out = (ctypes.c_short * 160)()
        n = self.lib.mg_lc3(frame, frame_bytes, out)
        if n < 0:
            raise ValueError("LC3 decode failed")
        return list(out[:n])

    def decode(self, fmt: dict, chunks: list[bytes]) -> tuple[list[int], int]:
        """PCM of the Data notifications of one utterance, and how many frames they held."""
        self.reset()
        pcm, frames = [], 0
        if fmt["type"] == P["mg_data_type_audio_sbc"]:
            for c in chunks:   # whole frames per notification
                samples, used = self.sbc(c)
                if used != len(c):
                    raise ValueError(f"a notification held {len(c) - used} bytes that aren't whole SBC frames")
                pcm += samples
                frames += len(samples) // 128 if samples else 0
        elif fmt["type"] == P["mg_data_type_audio_lc3"]:
            fb = fmt["frame_bytes"]
            for c in chunks:
                if len(c) % fb:
                    raise ValueError(f"a notification of {len(c)} bytes isn't whole {fb}-byte LC3 frames")
                for i in range(0, len(c), fb):
                    pcm += self.lc3(c[i:i + fb], fb)
                    frames += 1
        else:
            raise ValueError(f"can't decode {data_type_name(fmt['type'])}")
        return pcm, frames


def write_wav(path: Path, pcm: list[int], rate: int = 16000) -> None:
    import array
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(array.array("h", pcm).tobytes())


# ---- BLE ----------------------------------------------------------------------

async def wait_write_room(client, timeout: float = 1.0) -> None:
    """Waits until the host stack can take another Write Without Response.

    CoreBluetooth drops a write-without-response it has no room for, and
    bleak doesn't check, so without this a burst loses whole writes. Other
    stacks queue or block, so there this is only a short pause.
    """
    peripheral = getattr(getattr(client, "_backend", None), "_peripheral", None)
    if peripheral is None or not hasattr(peripheral, "canSendWriteWithoutResponse"):
        await asyncio.sleep(0.002)
        return
    deadline = time.monotonic() + timeout
    while not peripheral.canSendWriteWithoutResponse() and time.monotonic() < deadline:
        await asyncio.sleep(0.002)


class Gadget:
    """One connection, collecting Control and Data notifications."""

    def __init__(self, client, verbose: bool = True):
        self.client = client
        self.verbose = verbose
        self.control: asyncio.Queue = asyncio.Queue()
        self.data: list[bytes] = []
        self.nus: list[str] = []
        self.battery: list[int] = []
        # Playback: the latest buffer_update, stop_streaming notifications and stream_audio errors.
        self.buf_avail = 0
        self.buf_received = 0
        self.buf_event = asyncio.Event()
        self.stops: list[tuple[float, int]] = []
        self.stream_errors: list[str] = []
        # Face changes the device logs on the Nordic UART mirror (face_log: print them as they come).
        self.face_log = False
        self.faces: list[str] = []
        self._nus_line = ""
        # Takes Data notifications instead of the list when set (the throughput test).
        self.data_hook = None

    async def start(self, nus: bool = True) -> None:
        await self.client.start_notify(P["MG_CONTROL_UUID"], self._on_control)
        await self.client.start_notify(P["MG_DATA_UUID"], self._on_data)
        if nus:
            await self.client.start_notify(P["MG_NUS_TX_UUID"], self._on_nus)
        try:
            await self.client.start_notify(uuid16(P["MG_BAS_BATTERY_LEVEL_UUID16"]), self._on_battery)
        except Exception:
            pass

    def _on_control(self, _char, payload: bytearray) -> None:
        payload = bytes(payload)
        sa = P["mg_command_stream_audio"]
        if payload[:2] == bytes([sa, P["mg_stream_audio_buffer_update"]]) and len(payload) >= 10:
            self.buf_avail = int.from_bytes(payload[2:6], "little")
            self.buf_received = int.from_bytes(payload[6:10], "little")
            self.buf_event.set()
            return   # frequent: not printed or queued
        if payload[:2] == bytes([sa, P["mg_stream_audio_stop_streaming"]]) and len(payload) >= 3:
            self.stops.append((time.monotonic(), payload[2]))
        if payload[:2] == bytes([P["mg_command_error"], sa]):
            self.stream_errors.append(parse_error(payload))
        if self.verbose:
            print(f"  <- {describe(payload)}")
        self.control.put_nowait(payload)

    def _on_data(self, _char, payload: bytearray) -> None:
        if self.data_hook:
            self.data_hook(bytes(payload))
            return
        self.data.append(bytes(payload))

    def _on_nus(self, _char, payload: bytearray) -> None:
        text = bytes(payload).decode("utf-8", "replace")
        self.nus.append(text)
        if self.face_log:
            # The device's face changes ("mg.face: face thinking"), as whole lines.
            self._nus_line += text
            *lines, self._nus_line = self._nus_line.split("\n")
            for line in lines:
                m = FACE_RE.search(line)
                if m:
                    self.faces.append(m.group(1))
                    print(f"      face: {m.group(1)}")

    def _on_battery(self, _char, payload: bytearray) -> None:
        self.battery.append(payload[0])

    async def send(self, *payload: int) -> None:
        data = bytes(payload)
        if self.verbose:
            print(f"  -> {describe(data)}")
        await self.client.write_gatt_char(P["MG_CONTROL_UUID"], data, response=False)

    async def expect(self, command: int, timeout: float = 3.0, sub: int | None = None) -> bytes:
        """The next Control notification of `command` (and first parameter `sub`); others are skipped."""
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError(f"no {command_name(command)} notification")
            payload = await asyncio.wait_for(self.control.get(), left)
            if payload[0] == command and (sub is None or (len(payload) > 1 and payload[1] == sub)):
                return payload
            if payload[0] == P["mg_command_error"] and len(payload) > 1 and payload[1] == command:
                raise RuntimeError(parse_error(payload))

    async def status(self) -> dict:
        """request_status: the data type, the commands and every typed list, by the command it's about."""
        g = self
        g.drain()
        await g.send(P["mg_command_request_status"])
        cdt = await g.expect(P["mg_command_change_data_type"])
        feats = await g.expect(P["mg_command_supported_features"])
        typed = {}
        deadline = time.monotonic() + 1.0
        while time.monotonic() < deadline:
            try:
                p = await asyncio.wait_for(g.control.get(), deadline - time.monotonic())
            except asyncio.TimeoutError:
                break
            if p[0] == P["mg_command_supported_features"] and len(p) > 2 and p[1] == P["mg_command_sub_feature"]:
                typed[p[2]] = list(p[3:])
        return {"data_type": cdt, "commands": list(feats[1:]), "typed": typed,
                "settings": typed.get(P["mg_command_set_settings"])}

    def drain(self) -> list[bytes]:
        out = []
        while not self.control.empty():
            out.append(self.control.get_nowait())
        return out


def describe(payload: bytes) -> str:
    if not payload:
        return "(empty)"
    name = command_name(payload[0])
    if payload[0] == P["mg_command_change_data_type"]:
        f = parse_change_data_type(payload)
        return (f"change_data_type {data_type_name(f['type'])} {f['rate']} Hz x{f['channels']} "
                f"{f['frame_bytes']} B/{f['frame_us']} us")
    if payload[0] == P["mg_command_supported_features"]:
        if len(payload) > 1 and payload[1] == P["mg_command_sub_feature"]:
            return f"supported_features[{command_name(payload[2])}] {list(payload[3:])}"
        return "supported_features " + " ".join(command_name(b) for b in payload[1:])
    if payload[0] == P["mg_command_error"]:
        return parse_error(payload)
    if payload[0] == P["mg_command_stream_audio"] and len(payload) > 1:
        sub = {v: k[len("mg_stream_audio_"):] for k, v in P.items() if k.startswith("mg_stream_audio_")
               and not k.startswith(("mg_stream_audio_capability_", "mg_stream_audio_parameter_",
                                     "mg_stream_audio_buffer_keep", "mg_stream_audio_buffer_drop"))}
        name = sub.get(payload[1], str(payload[1]))
        if payload[1] == P["mg_stream_audio_report_capabilities"]:
            return f"stream_audio report_capabilities {parse_capabilities(payload)}"
        if payload[1] == P["mg_stream_audio_stop_streaming"] and len(payload) > 2:
            return f"stream_audio stop_streaming {'keep' if payload[2] == P['mg_stream_audio_buffer_keep'] else 'drop'}"
        return f"stream_audio {name} {payload[2:].hex(' ')}".strip()
    if payload[0] == P["mg_command_gesture"]:
        return "gesture " + ("button_down" if payload[1] == P["mg_gesture_button_down"] else "button_up")
    return f"{name} {payload[1:].hex(' ')}".strip()


def note_advertisement(seen: dict, address: str, name: str, rssi: int, uuids) -> None:
    """Adds one advertisement to seen[address] = {"name", "rssi", "uuids"}.

    Gadgets take turns between the Link setup and musegadgets payloads from one
    address, so the UUIDs of every packet in the scan count, not just the
    last one's (BleakScanner.discover keeps only the latest per device)."""
    entry = seen.setdefault(address, {"name": "", "rssi": rssi, "uuids": set()})
    entry["uuids"].update(u.lower() for u in uuids)
    entry["name"] = name or entry["name"]
    entry["rssi"] = max(entry["rssi"], rssi)


async def scan_adverts(secs: float) -> dict:
    from bleak import BleakScanner

    seen: dict = {}

    def detected(dev, adv):
        note_advertisement(seen, dev.address, adv.local_name or dev.name or "", adv.rssi, adv.service_uuids)

    async with BleakScanner(detection_callback=detected):
        await asyncio.sleep(secs)
    return seen


async def find(args):
    want = P["MG_SERVICE_UUID"]
    print(f"scanning {args.scan_secs:.0f} s for the musegadgets service ...")
    hits = []
    for addr, e in (await scan_adverts(args.scan_secs)).items():
        has = want in e["uuids"]
        if has or (e["name"].startswith(P["MG_DEVICE_NAME_PREFIX"]) and args.address is None):
            hits.append((addr, e["name"], e["rssi"], has))
    return hits


async def pick(args):
    hits = await find(args)
    if args.address:
        hits = [h for h in hits if h[0].lower() == args.address.lower()] or [(args.address, "?", 0, True)]
    elif args.name:
        hits = [h for h in hits if h[1].startswith(args.name)]
    if not args.address and not args.name:
        # Wi-Fi Muse gadgets share the name prefix; only a name the user asked for may stand in for the service.
        hits = [h for h in hits if h[3]]
    if not hits:
        raise SystemExit("no musegadgets gadget advertising (connected to a phone? try `scan`, or pass --name/--address)")
    addr, name, rssi, _ = sorted(hits, key=lambda h: -h[2])[0]
    print(f"using {name} {addr} (RSSI {rssi})")
    return addr, name


async def cmd_scan(args) -> int:
    hits = await find(args)
    for addr, name, rssi, has in sorted(hits, key=lambda h: -h[2]):
        print(f"{addr}  {name or '(no name)'}  RSSI {rssi}  {'mg service' if has else 'name only'}")
    return 0 if hits else 1


class Results:
    def __init__(self):
        self.failed = []

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        print(f"{'PASS' if ok else 'FAIL'}  {name}{': ' + detail if detail else ''}")
        if not ok:
            self.failed.append(name)
        return ok


async def read_str(client, uuid: int) -> str:
    return bytes(await client.read_gatt_char(uuid16(uuid))).decode("utf-8", "replace")


async def cmd_test(args) -> int:
    from bleak import BleakClient

    r = Results()
    addr, name = await pick(args)
    r.check("advertised name", name.startswith(P["MG_DEVICE_NAME_PREFIX"]), name)
    dec = Decoder()
    async with BleakClient(addr, timeout=20.0) as client:
        r.check("connect", client.is_connected)
        mtu = client.mtu_size
        r.check("ATT MTU", mtu >= P["MG_MIN_ATT_MTU"], f"{mtu} (protocol minimum {P['MG_MIN_ATT_MTU']})")
        try:
            man = await read_str(client, P["MG_DIS_MANUFACTURER_NAME_UUID16"])
            model = await read_str(client, P["MG_DIS_MODEL_NUMBER_UUID16"])
            fw = await read_str(client, P["MG_DIS_FIRMWARE_REVISION_UUID16"])
            sw = await read_str(client, P["MG_DIS_SOFTWARE_REVISION_UUID16"])
            r.check("Device Information", bool(man and model and fw),
                    f"manufacturer={man!r} model={model!r} firmware={fw!r} software={sw!r}")
            r.check("software revision", sw == f"mg{P['MG_SPEC_VERSION']}", sw)
        except Exception as e:
            r.check("Device Information", False, repr(e))
        try:
            level = bytes(await client.read_gatt_char(uuid16(P["MG_BAS_BATTERY_LEVEL_UUID16"])))[0]
            r.check("Battery Level", 0 <= level <= 100, f"{level}%")
        except Exception as e:
            r.check("Battery Level", False, repr(e))

        g = Gadget(client)
        await g.start()
        await asyncio.sleep(0.3)

        listed = None
        try:
            st = await g.status()
            cdt = st["data_type"]
            fmt = parse_change_data_type(cdt)
            r.check("request_status: change_data_type", True, describe(cdt))
            cmds = st["commands"]
            codecs = st["typed"].get(P["mg_command_start_mic"], [])
            r.check("request_status: supported_features", P["mg_command_start_mic"] in cmds,
                    " ".join(command_name(c) for c in cmds))
            r.check("start_mic codecs", P["mg_data_type_audio_sbc"] in codecs,
                    ", ".join(data_type_name(c) for c in codecs))
            listed = st["settings"]
            r.check("supported settings list", listed is not None and setting_id("spec_version") in listed,
                    ", ".join(setting_name(s) for s in listed or []) or "not sent")
        except Exception as e:
            r.check("request_status", False, repr(e))
            cmds, fmt = [], None
        if listed is None:
            # Firmware from before the typed list: guess from the commands.
            optional = {"audio_queue_enabled": P["mg_command_audio_queue"],
                        "speaker_volume": P["mg_command_stream_audio"]}
            listed = [setting_id(n) for n in SETTING_WIDTH if n not in optional or optional[n] in cmds]
        ids = [s for s in listed if s in {setting_id(n) for n in SETTING_WIDTH}]
        await g.send(P["mg_command_get_settings"], *ids)
        try:
            settings = parse_settings(await g.expect(P["mg_command_get_settings"]))
            r.check("get_settings", settings.get("spec_version") == P["MG_SPEC_VERSION"], str(settings))
            r.check("push_to_talk_enabled starts at 0", settings.get("push_to_talk_enabled") == 0)
        except Exception as e:
            r.check("get_settings", False, repr(e))
        await g.send(P["mg_command_set_settings"], setting_id("push_to_talk_enabled"), 1)
        await asyncio.sleep(0.3)
        errors = [p for p in g.drain() if p[0] == P["mg_command_error"]]
        r.check("set_settings push_to_talk_enabled=1 (no reply on success)", not errors,
                "; ".join(parse_error(e) for e in errors))
        await g.send(P["mg_command_get_settings"], setting_id("push_to_talk_enabled"))
        try:
            settings = parse_settings(await g.expect(P["mg_command_get_settings"]))
            r.check("push_to_talk_enabled reads back 1", settings.get("push_to_talk_enabled") == 1)
        except Exception as e:
            r.check("push_to_talk_enabled reads back", False, repr(e))
        # Every listed setting round-trips (set to what it is, so nothing changes)...
        for sid in ids:
            name = setting_name(sid)
            if name in ("spec_version", "push_to_talk_enabled"):
                continue   # read-only; checked above
            val = await get_setting(g, name)
            errors = await set_setting(g, name, val) if val is not None else ["no value"]
            back = await get_setting(g, name)
            r.check(f"{name} round trip", not errors and back == val,
                    f"{val}{' (speaker off)' if name == 'speaker_volume' and val == 0 else ''} set and read "
                    f"back {back}" + (f"; {'; '.join(errors)}" if errors else ""))
        # ...and every setting it doesn't list is refused.
        for name in SETTING_WIDTH:
            if setting_id(name) in listed:
                continue
            g.drain()
            await g.send(P["mg_command_get_settings"], setting_id(name))
            await asyncio.sleep(0.3)
            got = g.drain()
            refused_get = any(p[0] == P["mg_command_error"] for p in got)
            errors = await set_setting(g, name, 0)
            r.check(f"unlisted {name} refused", refused_get and bool(errors),
                    "; ".join(errors) if errors else "set_settings accepted it")

        # Client-driven capture.
        codec = P["mg_data_type_audio_" + args.codec]
        g.data.clear()
        await g.send(P["mg_command_start_mic"], codec)
        try:
            cdt = await g.expect(P["mg_command_change_data_type"], timeout=5)
            fmt = parse_change_data_type(cdt)
            r.check("start_mic: change_data_type", fmt["type"] == codec, describe(cdt))
        except Exception as e:
            r.check("start_mic: change_data_type", False, repr(e))
        await asyncio.sleep(args.secs)
        await g.send(P["mg_command_stop_mic"])
        try:
            await g.expect(P["mg_command_stop_mic"], timeout=3)
            r.check("stop_mic notification", True)
        except Exception as e:
            r.check("stop_mic notification", False, repr(e))
        await asyncio.sleep(0.3)
        chunks = list(g.data)
        payload = max((len(c) for c in chunks), default=0)
        r.check("audio on Data", bool(chunks),
                f"{len(chunks)} notifications, {sum(map(len, chunks))} bytes, largest {payload} "
                f"(fits {mtu - 3})")
        if chunks and fmt:
            try:
                pcm, frames = dec.decode(fmt, chunks)
                secs = len(pcm) / fmt["rate"]
                ok = secs > args.secs * 0.6
                r.check("frames decode", ok, f"{frames} frames, {secs:.2f} s of audio for {args.secs:.1f} s asked")
                peak = max((abs(s) for s in pcm), default=0)
                print(f"      peak {20 * __import__('math').log10(peak / 32768 + 1e-9):.1f} dBFS")
                out = Path(args.out)
                write_wav(out, pcm, fmt["rate"])
                print(f"      saved {out}")
            except Exception as e:
                r.check("frames decode", False, repr(e))

        if P["mg_command_audio_queue"] in cmds:
            await g.send(P["mg_command_audio_queue"], P["mg_audio_queue_command_status"])
            try:
                st = await g.expect(P["mg_command_audio_queue"], sub=P["mg_audio_queue_command_status"])
                enabled, used, cap, count = st[2], int.from_bytes(st[3:7], "little"), \
                    int.from_bytes(st[7:11], "little"), int.from_bytes(st[11:13], "little")
                r.check("audio_queue status", cap > 0,
                        f"enabled={enabled} {count} clips, {used} of {cap} bytes used")
            except Exception as e:
                r.check("audio_queue status", False, repr(e))
        else:
            print("NOTE  no offline queue on this gadget")
        rec = load_proof_record(addr)
        if P["mg_command_token_proof"] not in cmds:
            print("NOTE  no token proof on this gadget")
        elif not rec:
            print("NOTE  no proof key for this gadget (run `setup`): token proof skipped")
        else:
            try:
                result = await run_proof(g, rec["proof_key"])
                r.check("token proof", result == "match", f"{result} for {rec['node_id']}")
            except Exception as e:
                r.check("token proof", False, repr(e))
        if P["mg_command_stream_audio"] in cmds:
            await play_stream(g, dec, tone_pcm(1000, 2.0), 16000, r, mtu, label="playback: ")
        else:
            print("NOTE  no playback (stream_audio) on this gadget")
        if g.nus:
            print("NUS log mirror:\n  " + "".join(g.nus).strip().replace("\n", "\n  ")[-1500:])
        r.check("Nordic UART log mirror", bool(g.nus), f"{len(g.nus)} notifications")
    print(f"\n{len(r.failed)} failed" + (": " + ", ".join(r.failed) if r.failed else ""))
    return 1 if r.failed else 0


# ---- playback (stream_audio) ------------------------------------------------

def parse_capabilities(payload: bytes) -> dict:
    """[stream_audio, report_capabilities, count, (capability, value)...] -> dict."""
    caps, i = {}, 3
    for _ in range(payload[2] if len(payload) > 2 else 0):
        cap = payload[i]
        i += 1
        if cap == P["mg_stream_audio_capability_codecs"]:
            n = payload[i]
            caps["codecs"] = list(payload[i + 1:i + 1 + n])
            i += 1 + n
        elif cap == P["mg_stream_audio_capability_buffer_size"]:
            caps["buffer_size"] = int.from_bytes(payload[i:i + 4], "little")
            i += 4
        elif cap == P["mg_stream_audio_capability_sample_rates"]:
            n = payload[i]
            caps["sample_rates"] = [int.from_bytes(payload[i + 1 + 4 * k:i + 5 + 4 * k], "little") for k in range(n)]
            i += 1 + 4 * n
        elif cap == P["mg_stream_audio_capability_max_bitrate"]:
            caps["max_bitrate"] = int.from_bytes(payload[i:i + 2], "little")
            i += 2
        elif cap == P["mg_stream_audio_capability_max_channels"]:
            caps["max_channels"] = payload[i]
            i += 1
        else:
            break   # unknown: can't know its width
    return caps


def start_streaming(codec: int, rate: int, channels: int = 1) -> bytes:
    sa = P["mg_command_stream_audio"]
    return bytes([sa, P["mg_stream_audio_start_streaming"], 3,
                  P["mg_stream_audio_parameter_codec"], codec,
                  P["mg_stream_audio_parameter_sample_rate"], *rate.to_bytes(4, "little"),
                  P["mg_stream_audio_parameter_channels"], channels])


def tone_pcm(freq: float, secs: float, rate: int = 16000, dbfs: float = -12.0) -> list[int]:
    amp = 32767 * 10 ** (dbfs / 20)
    n = int(secs * rate)
    fade = int(0.01 * rate)   # 10 ms ramps: no clicks
    out = []
    for i in range(n):
        env = min(1.0, i / fade, (n - 1 - i) / fade) if fade else 1.0
        out.append(int(round(amp * env * math.sin(2 * math.pi * freq * i / rate))))
    return out


def sweep_pcm(f0: float, f1: float, secs: float, rate: int = 16000, dbfs: float = -12.0) -> list[int]:
    """A logarithmic sweep from f0 to f1."""
    amp = 32767 * 10 ** (dbfs / 20)
    n = int(secs * rate)
    k = math.log(f1 / f0)
    fade = int(0.01 * rate)
    out = []
    for i in range(n):
        t = i / rate
        phase = 2 * math.pi * f0 * secs / k * (math.exp(t / secs * k) - 1)
        env = min(1.0, i / fade, (n - 1 - i) / fade)
        out.append(int(round(amp * env * math.sin(phase))))
    return out


def read_wav(path: Path) -> tuple[list[int], int]:
    """16-bit WAV -> mono samples (channels averaged), rate."""
    import array
    with wave.open(str(path), "rb") as w:
        if w.getsampwidth() != 2:
            raise SystemExit(f"{path}: only 16-bit WAV files")
        ch, rate = w.getnchannels(), w.getframerate()
        a = array.array("h", w.readframes(w.getnframes()))
    if sys.byteorder == "big":
        a.byteswap()
    if ch > 1:
        a = [sum(a[i:i + ch]) // ch for i in range(0, len(a), ch)]
    return list(a), rate


def resample_linear(pcm: list[int], rate: int, to: int) -> list[int]:
    """Plain linear interpolation: fine for a test signal, not for music."""
    if rate == to:
        return pcm
    n = int(len(pcm) * to / rate)
    out = []
    for i in range(n):
        x = i * rate / to
        j = int(x)
        f = x - j
        a = pcm[j] if j < len(pcm) else 0
        b = pcm[j + 1] if j + 1 < len(pcm) else a
        out.append(int(a + (b - a) * f))
    return out


def goertzel_db(x: list[int], freq: float, rate: int = 16000) -> float:
    """Level of `freq` in x, in dBFS of a sine."""
    if not x:
        return -120.0
    k = 2 * math.cos(2 * math.pi * freq / rate)
    s1 = s2 = 0.0
    for v in x:
        s0 = v + k * s1 - s2
        s2, s1 = s1, s0
    p = s1 * s1 + s2 * s2 - k * s1 * s2
    amp = 2 * math.sqrt(max(p, 0.0)) / len(x)
    return 20 * math.log10(amp / 32768 + 1e-9)


def rms_db(x: list[int]) -> float:
    if not x:
        return -120.0
    return 10 * math.log10(sum(v * v for v in x) / len(x) / 32768 ** 2 + 1e-12)


def zero_cross_hz(x: list[int], rate: int = 16000) -> float:
    """Dominant frequency by rising zero crossings, after removing DC."""
    if len(x) < 2:
        return 0.0
    mean = sum(x) / len(x)
    crossings = sum(1 for a, b in zip(x, x[1:]) if a - mean < 0 <= b - mean)
    return crossings * rate / len(x)


STATS_RE = re.compile(
    r"stream end \((?P<how>\w+)\): (?P<codec>\w+) (?P<rate>\d+) Hz, (?P<frames_in>\d+) frames in, "
    r"(?P<decoded>\d+) decoded \((?P<bad>\d+) bad\), (?P<underruns>\d+) underruns, (?P<silence_ms>\d+) ms silence"
    r".*?stream bytes: (?P<received>\d+) received \((?P<dropped>\d+) dropped\), (?P<played_ms>\d+) ms played"
    r".*?stream level: decoded rms (?P<decoded_rms>-?[\d.]+) peak (?P<decoded_peak>-?[\d.]+) dBFS, "
    r"speaker rms (?P<rms>-?[\d.]+) peak (?P<peak>-?[\d.]+) dBFS(?P<off>, speaker off(?: (?P<off_ms>\d+) ms)?)?",
    re.S)


def parse_stream_stats(text: str) -> dict | None:
    """The last per-stream stats the gadget logged on the Nordic UART mirror.

    rms/peak are what reached the speaker's codec (silence while it's off);
    decoded_rms/decoded_peak the decoded audio. speaker_off: the whole stream
    played muted; off_ms how long it was muted if only part.
    """
    found = list(STATS_RE.finditer(text))
    if not found:
        return None
    d = found[-1].groupdict()
    out = {}
    for k, v in d.items():
        if k == "off":
            continue
        if k in ("how", "codec"):
            out[k] = v
        elif k in ("rms", "peak", "decoded_rms", "decoded_peak"):
            out[k] = float(v)
        else:
            out[k] = int(v) if v is not None else 0
    out["speaker_off"] = bool(d["off"]) and d["off_ms"] is None
    return out


async def get_setting(g: "Gadget", name: str) -> int | None:
    await g.send(P["mg_command_get_settings"], setting_id(name))
    try:
        return parse_settings(await g.expect(P["mg_command_get_settings"])).get(name)
    except Exception:
        return None


async def set_setting(g: "Gadget", name: str, value: int) -> list[str]:
    """set_settings one value; returns the errors it drew (none on success)."""
    g.drain()
    w = SETTING_WIDTH[name]
    await g.send(P["mg_command_set_settings"], setting_id(name), *value.to_bytes(w, "little"))
    await asyncio.sleep(0.3)
    return [parse_error(p) for p in g.drain() if p[0] == P["mg_command_error"]]


async def play_stream(g: "Gadget", dec: "Decoder", pcm: list[int], rate: int, r: "Results", mtu: int,
                      bitpool: int = 26, label: str = "") -> dict:
    """Streams pcm as SBC with the protocol's flow control; PASS/FAIL lines into r. Returns what it saw."""
    sa = P["mg_command_stream_audio"]
    out = {"ok": False}
    g.stream_errors.clear()
    g.stops.clear()
    nus_from = len(g.nus)
    await g.send(sa, P["mg_stream_audio_request_capabilities"])
    try:
        caps = parse_capabilities(await g.expect(sa, sub=P["mg_stream_audio_report_capabilities"]))
    except Exception as e:
        r.check(f"{label}capabilities", False, repr(e))
        return out
    sbc = P["mg_data_type_audio_sbc"]
    r.check(f"{label}capabilities", sbc in caps.get("codecs", []) and 16000 in caps.get("sample_rates", []),
            f"codecs {', '.join(data_type_name(c) for c in caps.get('codecs', []))}; "
            f"rates {caps.get('sample_rates')}; buffer {caps.get('buffer_size')} B; "
            f"max {caps.get('max_bitrate')} kb/s, {caps.get('max_channels')} ch")
    if rate not in caps.get("sample_rates", []):
        print(f"NOTE  {rate} Hz isn't offered: resampling to 16000 (linear)")
        pcm, rate = resample_linear(pcm, rate, 16000), 16000
    frames, fb = dec.sbc_encode(pcm, rate, bitpool)
    secs = len(frames) * 128 / rate
    per_write = max(1, (mtu - 3) // fb)
    writes = [b"".join(frames[i:i + per_write]) for i in range(0, len(frames), per_write)]
    total = sum(map(len, writes))
    g.buf_event.clear()
    await g.client.write_gatt_char(P["MG_CONTROL_UUID"], start_streaming(sbc, rate), response=False)
    try:
        await asyncio.wait_for(g.buf_event.wait(), 3)
        r.check(f"{label}start_streaming accepted", not g.stream_errors,
                f"SBC {rate} Hz, {len(frames)} frames of {fb} B ({secs:.2f} s, "
                f"{fb * 8 * rate / 128 / 1000:.0f} kb/s), {per_write} per write; buffer {g.buf_avail} B free")
    except asyncio.TimeoutError:
        r.check(f"{label}start_streaming accepted", False, "; ".join(g.stream_errors) or "no buffer_update")
        return out
    sent, waits, t_first = 0, 0, None
    for w in writes:
        while sent - g.buf_received + len(w) > g.buf_avail:
            # Not room for it: ask where the device is, and wait for its answer
            # (at most every 20 ms: the device frees a chunk per 20 ms it plays).
            if waits:
                await asyncio.sleep(0.02)
            waits += 1
            g.buf_event.clear()
            await g.client.write_gatt_char(P["MG_CONTROL_UUID"],
                                           bytes([sa, P["mg_stream_audio_request_buffer_update"]]), response=False)
            try:
                await asyncio.wait_for(g.buf_event.wait(), 2)
            except asyncio.TimeoutError:
                r.check(f"{label}buffer_update while streaming", False, "none for 2 s")
                return out
            if g.stops:
                break
        if g.stops:
            break
        await wait_write_room(g.client)
        await g.client.write_gatt_char(P["MG_DATA_UUID"], w, response=False)
        t_first = t_first or time.monotonic()
        sent += len(w)
    # Everything the device received, by its own count.
    deadline = time.monotonic() + 3
    while g.buf_received < sent and time.monotonic() < deadline and not g.stops:
        await asyncio.sleep(0.02)
        g.buf_event.clear()
        await g.client.write_gatt_char(P["MG_CONTROL_UUID"],
                                       bytes([sa, P["mg_stream_audio_request_buffer_update"]]), response=False)
        try:
            await asyncio.wait_for(g.buf_event.wait(), 1)
        except asyncio.TimeoutError:
            pass
    r.check(f"{label}bytes sent = device total received", g.buf_received == sent == total,
            f"sent {sent} of {total}, device received {g.buf_received}; {waits} flow-control waits")
    await g.send(sa, P["mg_stream_audio_stop_streaming"], P["mg_stream_audio_buffer_keep"])
    deadline = time.monotonic() + secs + 5
    while not g.stops and time.monotonic() < deadline:
        await asyncio.sleep(0.05)
    t_stop = g.stops[0][0] if g.stops else None
    kept = bool(g.stops) and g.stops[0][1] == P["mg_stream_audio_buffer_keep"]
    r.check(f"{label}stop_streaming keep acknowledged", kept,
            "keep" if kept else ("drop" if g.stops else "no stop_streaming from the device"))
    r.check(f"{label}no errors", not g.stream_errors, "; ".join(g.stream_errors))
    if t_stop and t_first:
        took = t_stop - t_first
        # Playback starts after ~100 ms of prebuffer and ends with 100 ms of silence.
        r.check(f"{label}duration", secs - 0.3 <= took <= secs + 1.5,
                f"{took:.2f} s from the first write to the device's stop, for {secs:.2f} s of audio")
    # The device's own account of the stream, on the Nordic UART mirror.
    deadline = time.monotonic() + 2
    stats = None
    while time.monotonic() < deadline and not (stats := parse_stream_stats("".join(g.nus[nus_from:]))):
        await asyncio.sleep(0.1)
    if stats:
        good = (stats["decoded"] == len(frames) and stats["bad"] == 0 and stats["dropped"] == 0
                and abs(stats["played_ms"] - secs * 1000) <= 60)
        r.check(f"{label}device stats", good,
                f"{stats['decoded']} of {len(frames)} frames decoded, {stats['bad']} bad, {stats['underruns']} "
                f"underruns ({stats['silence_ms']} ms silence), {stats['played_ms']} ms played; decoded rms "
                f"{stats['decoded_rms']} dBFS, speaker rms {stats['rms']} peak {stats['peak']} dBFS"
                + (", SPEAKER OFF" if stats["speaker_off"] else f", speaker off {stats['off_ms']} ms"
                   if stats["off_ms"] else ""))
    else:
        print(f"NOTE  no stats line on the Nordic UART mirror (subscribed: {bool(g.nus)})")
    out.update(ok=kept and not g.stream_errors, secs=secs, frames=len(frames), sent=sent, stats=stats)
    return out


async def capture(g: "Gadget", dec: "Decoder", secs: float | None = None, until=None) -> list[int]:
    """start_mic (SBC), then stop_mic after `secs` or once `until` (a coroutine) finishes; decoded PCM."""
    g.data.clear()
    await g.send(P["mg_command_start_mic"], P["mg_data_type_audio_sbc"])
    fmt = parse_change_data_type(await g.expect(P["mg_command_change_data_type"], timeout=5))
    result = None
    if until is not None:
        result = await until
    else:
        await asyncio.sleep(secs)
    await g.send(P["mg_command_stop_mic"])
    await g.expect(P["mg_command_stop_mic"], timeout=3)
    await asyncio.sleep(0.2)
    pcm, _ = dec.decode(fmt, list(g.data))
    return pcm if until is None else (pcm, result)


async def cmd_play(args) -> int:
    from bleak import BleakClient

    r = Results()
    dec = Decoder()
    if args.wav:
        pcm, rate = read_wav(Path(args.wav))
        what = f"{args.wav} ({len(pcm) / rate:.2f} s at {rate} Hz)"
    elif args.sweep:
        rate = args.rate
        pcm = sweep_pcm(200, min(7000, rate / 2 * 0.9), args.secs, rate, args.level)
        what = f"a 200 Hz-7 kHz sweep, {args.secs:.1f} s at {rate} Hz, {args.level:.0f} dBFS"
    else:
        rate = args.rate
        pcm = tone_pcm(args.tone, args.secs, rate, args.level)
        what = f"a {args.tone:.0f} Hz tone, {args.secs:.1f} s at {rate} Hz, {args.level:.0f} dBFS"
    addr, _ = await pick(args)
    async with BleakClient(addr, timeout=20.0) as client:
        mtu = client.mtu_size
        g = Gadget(client, verbose=args.verbose)
        await g.start()
        await asyncio.sleep(0.3)
        await g.send(P["mg_command_request_status"])
        feats = await g.expect(P["mg_command_supported_features"])
        if not r.check("stream_audio offered", P["mg_command_stream_audio"] in feats[1:]):
            return 1
        if args.volume is not None:
            errors = await set_setting(g, "speaker_volume", args.volume)
            vol = await get_setting(g, "speaker_volume")
            r.check(f"speaker_volume set to {args.volume}", not errors and vol == args.volume,
                    f"reads back {vol}" + (f"; {'; '.join(errors)}" if errors else ""))
        else:
            vol = await get_setting(g, "speaker_volume")
        if vol == 0:
            print("NOTE  the gadget's speaker is off (speaker_volume 0): it plays silence; turn it on with --volume N")
        elif vol is not None:
            print(f"      speaker volume {vol}")
        print(f"playing {what}")
        if not args.loopback:
            await play_stream(g, dec, pcm, rate, r, mtu, args.bitpool)
        else:
            # A silent baseline from the mic first, then the mic again while the speaker plays.
            base = await capture(g, dec, secs=1.5)
            cap, res = await capture(g, dec, until=play_stream(g, dec, pcm, rate, r, mtu, args.bitpool))
            out = Path(args.out)
            write_wav(out, cap)
            print(f"      saved the mic capture to {out} ({len(cap) / 16000:.2f} s)")
            stats = res.get("stats") or {}
            if vol == 0 or stats.get("speaker_off"):
                r.check("loopback", False, "the speaker is off (speaker_volume 0), so the mic can't hear it: "
                                           "rerun with --volume 60")
                print(f"\n{len(r.failed)} failed: " + ", ".join(r.failed))
                return 1
            if stats and stats["rms"] < -60:
                r.check("loopback", False, f"the speaker got silence (rms {stats['rms']} dBFS at the codec)")
            skip = int(0.4 * 16000)   # start_mic, prebuffer and the first frames
            seg = cap[skip:skip + int((res.get("secs", args.secs) - 0.3) * 16000)]
            b = base[int(0.3 * 16000):]
            spk = (f"; speaker at volume {vol}, {stats['rms']} dBFS rms at the codec" if stats else
                   f"; speaker at volume {vol}")
            if args.sweep or args.wav:
                lvl, floor = rms_db(seg), rms_db(b)
                r.check("loopback level", lvl >= floor + 6,
                        f"mic {lvl:.1f} dBFS while playing vs {floor:.1f} dBFS silent{spk}")
            else:
                f = args.tone
                lvl, floor = goertzel_db(seg, f), goertzel_db(b, f)
                others = max(goertzel_db(seg, f * m) for m in (0.8, 1.25))
                zc = zero_cross_hz(seg)
                r.check("loopback tone level", lvl >= floor + 10,
                        f"{f:.0f} Hz at {lvl:.1f} dBFS while playing vs {floor:.1f} dBFS silent{spk}")
                r.check("loopback tone frequency", lvl >= others + 6 and abs(zc - f) <= 0.1 * f,
                        f"{f:.0f} Hz bin {lvl - others:.1f} dB above its neighbours; zero crossings say {zc:.0f} Hz")
        await g.send(P["mg_command_request_status"])
        if args.verbose and g.nus:
            print("NUS log mirror:\n  " + "".join(g.nus).strip().replace("\n", "\n  ")[-2000:])
    print(f"\n{len(r.failed)} failed" + (": " + ", ".join(r.failed) if r.failed else ""))
    return 1 if r.failed else 0


async def cmd_ptt(args) -> int:
    """Waits for the talk button: records each utterance until stop_mic, then saves it."""
    from bleak import BleakClient

    addr, _ = await pick(args)
    dec = Decoder()
    async with BleakClient(addr, timeout=20.0) as client:
        g = Gadget(client)
        g.face_log = True
        await g.start()
        st = await g.status()
        states = not args.no_states and P["mg_command_assistant_state"] in st["commands"]
        if not args.no_states and not states:
            print("NOTE  this gadget doesn't take assistant_state: no face states sent")
        on = [setting_id("push_to_talk_enabled"), 1]
        if args.haptics:
            if setting_id("haptics_enabled") in (st["settings"] or []):
                on += [setting_id("haptics_enabled"), 1]
            else:
                print("NOTE  no vibration motor (haptics_enabled isn't listed): no activation buzz")
        await g.send(P["mg_command_set_settings"], *on)
        print(f"push-to-talk on: hold the gadget's talk button and speak (Ctrl-C to stop) ...")
        n = 0
        while args.count == 0 or n < args.count:
            fmt = None
            while True:
                p = await g.control.get()
                if p[0] == P["mg_command_change_data_type"]:
                    fmt = parse_change_data_type(p)
                    g.data.clear()
                    t0 = time.monotonic()
                elif p[0] == P["mg_command_stop_mic"] and fmt:
                    break
            await asyncio.sleep(0.2)
            n += 1
            chunks = list(g.data)
            try:
                pcm, frames = dec.decode(fmt, chunks)
                out = Path(args.out_dir) / f"ptt_{time.strftime('%H%M%S')}_{n}.wav"
                out.parent.mkdir(parents=True, exist_ok=True)
                write_wav(out, pcm, fmt["rate"])
                print(f"utterance {n}: {data_type_name(fmt['type'])}, {frames} frames, "
                      f"{len(pcm) / fmt['rate']:.2f} s ({time.monotonic() - t0:.2f} s wall) -> {out}")
            except Exception as e:
                print(f"utterance {n}: {len(chunks)} notifications, decode failed: {e}")
            if states:
                # A turn as the Muse app runs it: the gadget shows thinking by itself after stop_mic.
                await asyncio.sleep(args.think)
                await g.send(P["mg_command_assistant_state"], assistant_state("responding"))
                await asyncio.sleep(args.respond)
                await g.send(P["mg_command_assistant_state"], assistant_state("done"))
                await asyncio.sleep(0.5)
    return 0


# ---- tokens, for printing ------------------------------------------------------

def mask_token(t: str) -> str:
    """Enough of a token to tell two apart, never the whole of it."""
    if not t:
        return "(none)"
    return f"{t[:5]}...{t[-4:]} ({len(t)} chars)" if len(t) > 12 else f"({len(t)} chars)"


def token_fingerprint(t: bytes) -> str:
    """The first 4 bytes of SHA-256, as the device logs it."""
    return hashlib.sha256(t).hexdigest()[:8]


# ---- Muse Link setup (protocols/README.md, Gadget setup over Muse Link) --------
# The setup service of esp32/main/ble_server.c, and community pairing v5 as
# esp32/main/link_pairing.c and pairing_transcript.c run it. Vectors:
# esp32/tests/vectors/link_pairing_v5.json.

SETUP_SERVICE_UUID = "7fdd3d1c-38ea-46cf-8b46-314ecf5f240c"
SETUP_RX_UUID = "4d593029-28a2-4a6e-a1f0-3c2d5e8f9b01"
SETUP_TX_UUID = "d75dc4ca-7b2b-4e9c-8f0a-1d2e3f4a5b6c"
SETUP_CHUNK_MAGIC = 0xFE
PAIRING_VERSION = 5
PAIRING_MODEL = "hatch_link"
PAIRING_SUITE = "p256-hkdf-sha256-aes-gcm-v1"
PAIRING_POLICY = "confirm_press"
PAIRING_AUTH = "none"            # community pairing: no manufacturer attestation
PAIRING_CONFIRM_TIMEOUT_S = 60
PAIRING_RECORD_LABEL = "hatch-link ble setup v1"
PAIRING_SESSION_ID_LABEL = b"hatch-link session id v1"


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def unb64url(text: str) -> bytes:
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


def hkdf_sha256(salt: bytes, ikm: bytes, info: bytes, length: int = 32) -> bytes:
    """RFC 5869 extract and expand."""
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    out, block, i = b"", b"", 1
    while len(out) < length:
        block = hmac.new(prk, block + info + bytes([i]), hashlib.sha256).digest()
        out += block
        i += 1
    return out[:length]


def pairing_transcript(device: dict, mobile_pub: str, mobile_nonce: str) -> str:
    """The v5 community transcript both sides hash (pairing_transcript.c); device is pairing_ready."""
    return "\n".join([
        f"hatch-link-pairing-v{PAIRING_VERSION}",
        f"version={PAIRING_VERSION}",
        "initiator_role=mobile",
        "responder_role=link",
        f"device_id={device['device_id']}",
        f"node_id={device['node_id']}",
        f"mac={device['mac']}",
        f"model={PAIRING_MODEL}",
        f"firmware_version={device['firmware_version']}",
        f"selected_cipher_suite={PAIRING_SUITE}",
        f"pairing_auth={PAIRING_AUTH}",
        "pairing_auth_epoch=0",
        f"pairing_policy={PAIRING_POLICY}",
        f"confirm_timeout_seconds={PAIRING_CONFIRM_TIMEOUT_S}",
        f"mobile_pub={mobile_pub}",
        f"device_pub={device['device_pub']}",
        f"mobile_nonce={mobile_nonce}",
        f"device_nonce={device['device_nonce']}",
    ])


def pairing_keys(ecdh: bytes, mobile_nonce: bytes, device_nonce: bytes, transcript_hash: bytes) -> dict:
    """The session's keys (derive_session_keys in link_pairing.c), from the mobile side."""
    salt = hashlib.sha256(mobile_nonce + device_nonce + transcript_hash).digest()
    secret = hkdf_sha256(salt, ecdh, PAIRING_RECORD_LABEL.encode())

    def expand(label: bytes) -> bytes:
        return hmac.new(secret, label + b"\x01", hashlib.sha256).digest()

    sid = hashlib.sha256(PAIRING_SESSION_ID_LABEL + transcript_hash + ecdh).digest()[:16]
    return {"session_secret": secret, "tx": expand(b"mobile->device"), "rx": expand(b"device->mobile"),
            "session_id": b64url(sid)}


def record_nonce(direction: int, counter: int) -> bytes:
    """direction 0 is mobile to device, 1 device to mobile; the counter big-endian in the last 8 bytes."""
    return bytes([direction, 0, 0, 0]) + counter.to_bytes(8, "big")


def record_aad(session_id: str, direction: int, counter: int) -> bytes:
    return f"{PAIRING_RECORD_LABEL}|{session_id}|{'m2d' if direction == 0 else 'd2m'}|{counter}".encode()


def seal_record(key: bytes, session_id: str, counter: int, plaintext: bytes, direction: int = 0) -> dict:
    """A pairing_encrypted envelope (the client's carry "action"; the device's "type")."""
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM

    sealed = AESGCM(key).encrypt(record_nonce(direction, counter), plaintext, record_aad(session_id, direction, counter))
    return {"action" if direction == 0 else "type": "pairing_encrypted", "session_id": session_id,
            "counter": str(counter), "ciphertext": b64url(sealed[:-16]), "tag": b64url(sealed[-16:])}


def open_record(key: bytes, session_id: str, counter: int, env: dict, direction: int = 1) -> bytes:
    """The plaintext of a pairing_encrypted envelope; raises on a wrong session, counter or tag."""
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM

    if env.get("session_id") != session_id or env.get("counter") != str(counter):
        raise ValueError(f"unexpected record: session {env.get('session_id')} counter {env.get('counter')}")
    data = unb64url(env["ciphertext"]) + unb64url(env["tag"])
    return AESGCM(key).decrypt(record_nonce(direction, counter), data, record_aad(session_id, direction, counter))


def setup_chunks(data: bytes, mtu: int) -> list[bytes]:
    """RX writes of one message: [0xFE, index, total] then up to MTU - 6 bytes each."""
    room = max(1, min(mtu - 3, 180) - 3)
    parts = [data[i:i + room] for i in range(0, len(data), room)] or [b""]
    if len(parts) > 255:
        raise ValueError("message too long for the setup framing")
    return [bytes([SETUP_CHUNK_MAGIC, i, len(parts)]) + p for i, p in enumerate(parts)]


class SetupReassembler:
    """TX notifications into messages: chunked JSON, or a bare status string (plaintext errors)."""

    def __init__(self):
        self.parts: dict[int, bytes] = {}
        self.total = 0

    def feed(self, payload: bytes) -> bytes | None:
        if len(payload) < 3 or payload[0] != SETUP_CHUNK_MAGIC:
            return payload
        idx, total = payload[1], payload[2]
        if idx == 0 or total != self.total:
            self.parts, self.total = {}, total
        self.parts[idx] = payload[3:]
        if len(self.parts) < self.total:
            return None
        out = b"".join(self.parts[i] for i in range(self.total))
        self.parts, self.total = {}, 0
        return out


class SetupLink:
    """A connection to the setup service: write messages, read them back in order."""

    def __init__(self, client):
        self.client = client
        self.rx = SetupReassembler()
        self.messages: asyncio.Queue = asyncio.Queue()

    async def start(self) -> None:
        await self.client.start_notify(SETUP_TX_UUID, self._on_tx)

    def _on_tx(self, _char, payload: bytearray) -> None:
        msg = self.rx.feed(bytes(payload))
        if msg is not None:
            self.messages.put_nowait(msg)

    async def send(self, obj: dict) -> None:
        data = json.dumps(obj, separators=(",", ":")).encode()
        for chunk in setup_chunks(data, getattr(self.client, "mtu_size", 23)):
            await self.client.write_gatt_char(SETUP_RX_UUID, chunk, response=True)

    async def receive(self, timeout: float) -> dict | str:
        """The next message: a JSON object, or a bare status string."""
        msg = await asyncio.wait_for(self.messages.get(), timeout)
        try:
            return json.loads(msg)
        except ValueError:
            return msg.decode("utf-8", "replace")


class PairingSession:
    """The mobile side of community pairing v5, over a SetupLink."""

    def __init__(self, link: SetupLink, private_value: int | None = None, mobile_nonce: bytes | None = None):
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

        self.link = link
        self.priv = (ec.derive_private_key(private_value, ec.SECP256R1()) if private_value
                     else ec.generate_private_key(ec.SECP256R1()))
        self.mobile_pub = b64url(self.priv.public_key().public_bytes(Encoding.X962, PublicFormat.UncompressedPoint))
        self.mobile_nonce = mobile_nonce or os.urandom(16)
        self.keys: dict = {}
        self.device: dict = {}
        self.tx_counter = 0
        self.rx_counter = 0

    def hello(self) -> dict:
        return {"action": "pairing_client_hello", "version": PAIRING_VERSION, "pairing_auth": PAIRING_AUTH,
                "pairing_policy": PAIRING_POLICY, "mobile_pub": self.mobile_pub,
                "mobile_nonce": b64url(self.mobile_nonce)}

    def accept_ready(self, ready: dict) -> None:
        """Checks pairing_ready (transcript hash, session id) and keeps the session keys."""
        from cryptography.hazmat.primitives.asymmetric import ec

        if ready.get("type") != "pairing_ready" or ready.get("version") != PAIRING_VERSION:
            raise RuntimeError(f"not a v5 pairing_ready: {ready}")
        if ready.get("pairing_auth") != PAIRING_AUTH or ready.get("pairing_policy") != PAIRING_POLICY:
            raise RuntimeError("the device offered another pairing mode")
        transcript = pairing_transcript(ready, self.mobile_pub, b64url(self.mobile_nonce))
        th = hashlib.sha256(transcript.encode()).digest()
        if b64url(th) != ready.get("transcript_hash"):
            raise RuntimeError("transcript hash mismatch")
        peer = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), unb64url(ready["device_pub"]))
        ecdh = self.priv.exchange(ec.ECDH(), peer)
        keys = pairing_keys(ecdh, self.mobile_nonce, unb64url(ready["device_nonce"]), th)
        if keys["session_id"] != ready.get("session_id"):
            raise RuntimeError("session id mismatch")
        self.keys, self.device = keys, ready

    def seal(self, obj: dict) -> dict:
        env = seal_record(self.keys["tx"], self.keys["session_id"], self.tx_counter,
                          json.dumps(obj, separators=(",", ":")).encode())
        self.tx_counter += 1
        return env

    def open(self, env: dict) -> dict:
        plain = open_record(self.keys["rx"], self.keys["session_id"], self.rx_counter, env)
        self.rx_counter += 1
        return json.loads(plain)

    async def status(self, timeout: float) -> dict:
        """The next encrypted status; a bare plaintext status is an error from the device."""
        msg = await self.link.receive(timeout)
        if isinstance(msg, str):
            raise RuntimeError(f"device: {msg}")
        if msg.get("type") != "pairing_encrypted":
            raise RuntimeError(f"unexpected message: {msg}")
        return self.open(msg)


async def setup_flow(link: SetupLink, access: str, refresh: str, prompt=print, session: PairingSession | None = None,
                     confirm_timeout: float = PAIRING_CONFIRM_TIMEOUT_S + 5) -> dict:
    """device_info, pairing, the button, then token-only provision_v2. Returns what setup stored."""
    await link.send({"action": "get_device_info"})
    info = await link.receive(10)
    if not isinstance(info, dict) or info.get("type") != "device_info":
        raise RuntimeError(f"no device_info: {info}")
    wifi = info.get("wifi", "required")
    if wifi == "required":
        raise RuntimeError("this device needs Wi-Fi in provision_v2 (no gadget setup); use the Muse app")
    if info.get("pairing_protocol") != PAIRING_VERSION or info.get("pairing_auth") != PAIRING_AUTH:
        raise RuntimeError(f"needs community pairing v5, the device offers {info.get('pairing_protocol')} "
                           f"{info.get('pairing_auth')}")
    s = session or PairingSession(link)
    await link.send(s.hello())
    ready = await link.receive(10)
    if isinstance(ready, str):
        raise RuntimeError(f"device: {ready}")
    s.accept_ready(ready)
    if ready["node_id"] != info["node_id"]:
        raise RuntimeError("pairing_ready names another node")
    await link.send(s.seal({"action": "pairing_client_finished"}))
    st = await s.status(10)
    if st.get("status") != "confirm_required":
        raise RuntimeError(f"expected confirm_required, got {st}")
    prompt("press the gadget's button to confirm pairing (talk button on Muse boards)")
    st = await s.status(confirm_timeout)
    if st.get("status") != "pairing_confirmed":
        raise RuntimeError(f"pairing not confirmed: {st.get('status')}")
    sdk_token = st.get("sdk_token", "")
    await link.send(s.seal({"action": "provision_v2", "access_token": access, "refresh_token": refresh,
                            "token_type": "device"}))
    st = await s.status(30)
    if st.get("status") != "auth_ok":
        raise RuntimeError(f"provisioning failed: {st.get('status')}")
    return {"node_id": info["node_id"], "wifi": wifi, "mgcommands": info.get("mgcommands") == 1,
            "sdk_token": sdk_token, "proof_key": proof_key(access, info["node_id"])}


# ---- the token proof (mgcommands.h, Token proof) ------------------------------

PROOF_SALT = b"mg token proof v1"


def proof_key(access_token: str, node_id: str) -> bytes:
    """K = HKDF-SHA256(salt "mg token proof v1", IKM access token, info node_id, 32 bytes)."""
    return hkdf_sha256(PROOF_SALT, access_token.encode("utf-8"), node_id.encode("utf-8"))


def proof_mac(k: bytes, device: bool, client_nonce: bytes, device_nonce: bytes) -> bytes:
    label = b"mg token proof v1 device" if device else b"mg token proof v1 client"
    return hmac.new(k, label + client_nonce + device_nonce, hashlib.sha256).digest()


def proof_dir() -> Path:
    return Path(os.environ.get("MG_BLE_CLIENT_HOME", Path.home() / ".mg_ble_client"))


def proof_path(address: str) -> Path:
    return proof_dir() / ("proof-" + re.sub(r"[^0-9A-Za-z]", "_", address.lower()) + ".json")


def save_proof_record(address: str, node_id: str, k: bytes, access_token: str) -> Path:
    """What the app keeps for a gadget: node_id and K (and a fingerprint), never the tokens. Mode 0600."""
    d = proof_dir()
    d.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(d, 0o700)
    path = proof_path(address)
    body = json.dumps({"address": address, "node_id": node_id, "proof_key": k.hex(),
                       "access_fingerprint": token_fingerprint(access_token.encode())}, indent=1)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(body + "\n")
    os.chmod(path, 0o600)
    return path


def load_proof_record(address: str) -> dict | None:
    try:
        rec = json.loads(proof_path(address).read_text())
    except (OSError, ValueError):
        return None
    rec["proof_key"] = bytes.fromhex(rec["proof_key"])
    return rec


async def run_proof(g: "Gadget", k: bytes, clear: bool = False) -> str:
    """challenge, check the device's MAC, confirm. "match", "mismatch", "device_mac_mismatch" or "not_found"."""
    tp = P["mg_command_token_proof"]
    client_nonce = os.urandom(P["MG_TOKEN_PROOF_NONCE_LEN"])
    await g.send(tp, P["mg_token_proof_challenge"], *client_nonce)
    try:
        rsp = await g.expect(tp, sub=P["mg_token_proof_response"])
    except RuntimeError as e:
        if "not_found" in str(e):
            return "not_found"
        raise
    n = P["MG_TOKEN_PROOF_NONCE_LEN"]
    device_nonce, mac = rsp[2:2 + n], rsp[2 + n:2 + n + P["MG_TOKEN_PROOF_MAC_LEN"]]
    if not hmac.compare_digest(mac, proof_mac(k, True, client_nonce, device_nonce)):
        return "device_mac_mismatch"   # the gadget doesn't hold this key: don't confirm
    await g.send(tp, P["mg_token_proof_confirm"], *proof_mac(k, False, client_nonce, device_nonce))
    res = await g.expect(tp, sub=P["mg_token_proof_result"])
    if res[2] != 1:
        return "mismatch"
    if clear:
        await g.send(tp, P["mg_token_proof_clear"])
        res = await g.expect(tp, sub=P["mg_token_proof_result"])
        if res[2] != 1:
            raise RuntimeError("clear refused")
    return "match"


async def find_setup(args):
    """Devices advertising the Link setup service (they take turns with the musegadgets UUID)."""
    print(f"scanning {args.scan_secs:.0f} s for the Muse Link setup service ...")
    hits = [(addr, e["name"], e["rssi"]) for addr, e in (await scan_adverts(args.scan_secs)).items()
            if SETUP_SERVICE_UUID in e["uuids"]]
    if args.address:
        hits = [h for h in hits if h[0].lower() == args.address.lower()]
    elif args.name:
        hits = [h for h in hits if h[1].startswith(args.name)]
    return sorted(hits, key=lambda h: -h[2])


async def cmd_setup(args) -> int:
    from bleak import BleakClient

    try:
        import cryptography  # noqa: F401
    except ImportError:
        print("FAIL  setup needs the cryptography package: pip install bleak cryptography")
        return 2
    hits = await find_setup(args)
    if not hits:
        print("FAIL  nothing advertising Link setup (set up already? reset it first; or pass --address)")
        return 1
    addr, name, rssi = hits[0]
    print(f"using {name} {addr} (RSSI {rssi})")
    access = args.access or "bench-access-" + secrets.token_hex(16)
    refresh = args.refresh or "bench-refresh-" + secrets.token_hex(16)
    async with BleakClient(addr, timeout=20.0) as client:
        link = SetupLink(client)
        await link.start()
        try:
            got = await setup_flow(link, access, refresh, prompt=lambda m: print(f">>> {m}"))
        except (RuntimeError, ValueError, asyncio.TimeoutError) as e:
            print(f"FAIL  {e or type(e).__name__}")
            return 1
    print(f"PASS  set up {got['node_id']} (wifi {got['wifi']}, mgcommands {'yes' if got['mgcommands'] else 'no'}), "
          f"access {len(access)} B ({token_fingerprint(access.encode())}), refresh {len(refresh)} B")
    print(f"SDK token   {mask_token(got['sdk_token'])}")
    path = save_proof_record(addr, got["node_id"], got["proof_key"], access)
    print(f"proof key saved to {path} (0600)")
    return 0


async def cmd_proof(args) -> int:
    from bleak import BleakClient

    addr, _ = await pick(args)
    rec = load_proof_record(addr)
    if not rec:
        print(f"FAIL  no proof key for {addr} in {proof_dir()}: run `setup` first")
        return 1
    async with BleakClient(addr, timeout=20.0) as client:
        g = Gadget(client, verbose=args.verbose)
        await g.start(nus=False)
        st = await g.status()
        if P["mg_command_token_proof"] not in st["commands"]:
            print("FAIL  this gadget doesn't list token_proof")
            return 1
        try:
            result = await run_proof(g, rec["proof_key"], clear=args.clear)
        except (RuntimeError, asyncio.TimeoutError) as e:
            print(f"FAIL  {e or type(e).__name__}")
            return 1
    ok = result == "match"
    print(f"{'PASS' if ok else 'FAIL'}  token proof for {rec['node_id']}: {result}")
    if ok and args.clear:
        proof_path(addr).unlink(missing_ok=True)
        print("cleared: the gadget erased its tokens and returns to setup; local key deleted")
    return 0 if ok else 1


async def cmd_state(args) -> int:
    """Sends one assistant_state, then prints the face changes the gadget logs for a few seconds."""
    from bleak import BleakClient

    addr, _ = await pick(args)
    async with BleakClient(addr, timeout=20.0) as client:
        g = Gadget(client)
        g.face_log = True
        await g.start()
        st = await g.status()
        if P["mg_command_assistant_state"] not in st["commands"]:
            print("FAIL  this gadget doesn't list assistant_state")
            return 1
        await g.send(P["mg_command_assistant_state"], assistant_state(args.state))
        await asyncio.sleep(args.wait)
        errors = [parse_error(p) for p in g.drain() if p[0] == P["mg_command_error"]]
        if errors:
            print("FAIL  " + "; ".join(errors))
            return 1
        if not g.faces:
            print("NOTE  no face change logged (already in that state, or the log mirror is off)")
    return 0


async def cmd_queue(args) -> int:
    from bleak import BleakClient

    addr, _ = await pick(args)
    dec = Decoder()
    async with BleakClient(addr, timeout=20.0) as client:
        g = Gadget(client, verbose=False)
        await g.start(nus=False)
        q = P["mg_command_audio_queue"]
        await g.send(q, P["mg_audio_queue_command_status"])
        st = await g.expect(q, sub=P["mg_audio_queue_command_status"])
        count = int.from_bytes(st[11:13], "little")
        print(f"queue: enabled={st[2]} {count} clips, {int.from_bytes(st[3:7], 'little')} of "
              f"{int.from_bytes(st[7:11], 'little')} bytes used")
        for i in range(count):
            await g.send(q, P["mg_audio_queue_command_clip_info"], i & 0xFF, i >> 8)
            info = await g.expect(q, sub=P["mg_audio_queue_command_clip_info"])
            codec, start, size = info[4], int.from_bytes(info[5:9], "little"), int.from_bytes(info[9:13], "little")
            print(f"  clip {i}: {data_type_name(codec)}, {size} bytes, started at uptime {start / 1000:.1f} s")
            if not args.download:
                continue
            g.data.clear()
            t0 = time.monotonic()
            await g.send(q, P["mg_audio_queue_command_read_clip"], i & 0xFF, i >> 8)
            await g.expect(P["mg_command_change_data_type"])
            t1 = time.monotonic()
            deadline = t1 + 30
            while sum(map(len, g.data)) < size and time.monotonic() < deadline:
                await asyncio.sleep(0.01)
            t2 = time.monotonic()
            blob = b"".join(g.data)[:size]
            print(f"    downloaded in {t2 - t0:.2f} s ({t1 - t0:.2f} s to start): "
                  f"{len(blob) / max(t2 - t1, 1e-3) / 1024:.1f} KB/s, {len(g.data)} notifications")
            fmt = {"type": codec, "rate": 16000, "channels": 1, "frame_bytes": P["MG_LC3_DEFAULT_FRAME_BYTES"],
                   "frame_us": P["MG_LC3_DEFAULT_FRAME_US"]}
            try:
                if codec == P["mg_data_type_audio_sbc"]:
                    pcm, _ = dec.decode(fmt, [blob])
                else:
                    pcm, _ = dec.decode(fmt, [blob[:len(blob) // fmt["frame_bytes"] * fmt["frame_bytes"]]])
                out = Path(args.out_dir) / f"clip_{i}.wav"
                out.parent.mkdir(parents=True, exist_ok=True)
                write_wav(out, pcm)
                print(f"    {len(blob)} of {size} bytes, {len(pcm) / 16000:.2f} s -> {out}")
            except Exception as e:
                print(f"    {len(blob)} of {size} bytes, decode failed: {e}")
        if args.clear:
            await g.send(q, P["mg_audio_queue_command_clear"])
            st = await g.expect(q, sub=P["mg_audio_queue_command_status"])
            print(f"cleared: {int.from_bytes(st[11:13], 'little')} clips left")
    return 0


# ---- throughput test (device_action throughput_test) ----

def tp_fill(n: int, seq: int) -> bytes:
    """A throughput test packet, as the firmware makes them: the sequence, then (seq + i) & 0xFF."""
    return seq.to_bytes(4, "little") + bytes((seq + i) & 0xFF for i in range(4, n))


class TpCounter:
    """Counts a throughput test stream: bytes, packets, sequence gaps, corrupt packets, and
    arrival bursts (notifications that land together, roughly one connection event each)."""

    def __init__(self, burst_gap_s: float = 0.004):
        self.bytes = self.packets = self.gaps = self.bad = 0
        self.next_seq = 0
        self.first = self.last = 0.0
        self.burst_gap_s = burst_gap_s
        self.bursts: list[int] = []

    def add(self, payload: bytes, t: float) -> None:
        if len(payload) < 4:
            return
        seq = int.from_bytes(payload[:4], "little")
        if self.packets == 0:
            self.first = t
            self.next_seq = seq
        if seq > self.next_seq:
            self.gaps += seq - self.next_seq
        if seq >= self.next_seq:
            self.next_seq = seq + 1
        if payload != tp_fill(len(payload), seq):
            self.bad += 1
        if not self.bursts or t - self.last > self.burst_gap_s:
            self.bursts.append(0)
        self.bursts[-1] += 1
        self.bytes += len(payload)
        self.packets += 1
        self.last = t

    def rate(self) -> float:
        return self.bytes / max(self.last - self.first, 1e-3)

    def per_burst(self) -> str:
        if not self.bursts:
            return "-"
        return f"{self.packets / len(self.bursts):.1f} on average, at most {max(self.bursts)}"


def parse_tp_report(payload: bytes) -> dict:
    """[device_action, throughput_test, 0, bytes, packets, ms, gaps]."""
    v = [int.from_bytes(payload[i:i + 4], "little") for i in (3, 7, 11, 15)]
    return {"bytes": v[0], "packets": v[1], "ms": v[2], "gaps": v[3]}


async def cmd_bench(args) -> int:
    """Raw link throughput both ways with device_action throughput_test."""
    from bleak import BleakClient

    r = Results()
    da, tt = P["mg_command_device_action"], P["mg_device_action_throughput_test"]
    send_s, recv_s = args.send, args.recv
    if send_s is None and recv_s is None:
        send_s = recv_s = 10
    addr, _ = await pick(args)
    async with BleakClient(addr, timeout=20.0) as client:
        g = Gadget(client, verbose=False)
        await g.start(nus=False)
        st = await g.status()
        if da not in st["commands"]:
            print("FAIL  this gadget doesn't list device_action (older firmware)")
            return 1
        payload = client.mtu_size - 3
        print(f"ATT MTU {client.mtu_size}: {payload} B packets")

        if send_s:
            c = TpCounter()
            g.data_hook = lambda d: c.add(d, time.monotonic())
            await g.send(da, tt, P["mg_throughput_test_send"], send_s & 0xFF, send_s >> 8)
            cdt = await g.expect(P["mg_command_change_data_type"])
            r.check("send: change_data_type throughput_test", cdt[1] == P["mg_data_type_throughput_test"])
            rep = parse_tp_report(await g.expect(da, timeout=send_s + 10, sub=tt))
            await asyncio.sleep(0.3)   # the last notifications
            g.data_hook = None
            dev_rate = rep["bytes"] / max(rep["ms"], 1) * 1000
            print(f"send (device -> {sys.platform}): device sent {rep['packets']} packets, {rep['bytes']} B in "
                  f"{rep['ms']} ms = {dev_rate / 1024:.1f} KB/s")
            print(f"  received {c.packets} packets, {c.bytes} B in {c.last - c.first:.2f} s = "
                  f"{c.rate() / 1024:.1f} KB/s; packets per burst (about one connection event): "
                  f"{c.per_burst()}")
            r.check("send: everything arrived", c.packets == rep["packets"] and c.gaps == 0 and c.bad == 0,
                    f"{c.gaps} missing, {c.bad} corrupt")

        if recv_s:
            await g.send(da, tt, P["mg_throughput_test_receive"], 0, 0)   # until stop
            await g.send(P["mg_command_change_data_type"], P["mg_data_type_throughput_test"])
            # The gadget turns its peripheral latency off for the test; at latency 10 the
            # update takes about a second to apply.
            await asyncio.sleep(1.5)
            t0 = time.monotonic()
            seq = 0
            while time.monotonic() - t0 < recv_s:
                await wait_write_room(client)
                await client.write_gatt_char(P["MG_DATA_UUID"], tp_fill(payload, seq), response=False)
                seq += 1
            wrote_s = time.monotonic() - t0
            await asyncio.sleep(0.5)
            await g.send(da, tt, P["mg_throughput_test_stop"])
            rep = parse_tp_report(await g.expect(da, timeout=5, sub=tt))
            dev_rate = rep["bytes"] / max(rep["ms"], 1) * 1000
            print(f"receive ({sys.platform} -> device): wrote {seq} packets, {seq * payload} B in {wrote_s:.2f} s = "
                  f"{seq * payload / wrote_s / 1024:.1f} KB/s")
            print(f"  device got {rep['packets']} packets, {rep['bytes']} B in {rep['ms']} ms = "
                  f"{dev_rate / 1024:.1f} KB/s, {rep['gaps']} missing")
            r.check("receive: everything arrived", rep["packets"] == seq and rep["gaps"] == 0,
                    f"{seq - rep['packets']} lost")
    return 1 if r.failed else 0


# ---- firmware update over MCUmgr SMP (images, dfu) ----

MCUBOOT_IMAGE_MAGIC = 0x96F3B83D
MCUBOOT_TLV_INFO_MAGIC = 0x6907
MCUBOOT_TLV_PROT_INFO_MAGIC = 0x6908
MCUBOOT_TLV_SHA256 = 0x10


def mcuboot_image_info(data: bytes) -> dict:
    """Version and SHA-256 (the hash SMP names images by) of an MCUboot-signed image."""
    if len(data) < 32:
        raise ValueError("too short for an MCUboot image")
    magic, _load, hdr_size, prot_size, img_size, _flags = struct.unpack_from("<IIHHII", data, 0)
    if magic != MCUBOOT_IMAGE_MAGIC:
        raise ValueError("not an MCUboot image (sign it: use zephyr.signed.bin)")
    major, minor, rev, build = struct.unpack_from("<BBHI", data, 20)
    off = hdr_size + img_size
    if prot_size:
        tlv_magic, tot = struct.unpack_from("<HH", data, off)
        if tlv_magic != MCUBOOT_TLV_PROT_INFO_MAGIC:
            raise ValueError("bad protected TLV area")
        off += tot
    tlv_magic, tot = struct.unpack_from("<HH", data, off)
    if tlv_magic != MCUBOOT_TLV_INFO_MAGIC:
        raise ValueError("no TLV area after the image")
    end, p, sha = off + tot, off + 4, None
    while p + 4 <= end:
        t, n = struct.unpack_from("<HH", data, p)
        if t == MCUBOOT_TLV_SHA256:
            sha = bytes(data[p + 4:p + 4 + n])
        p += 4 + n
    if sha is None:
        raise ValueError("no SHA-256 TLV")
    return {"version": f"{major}.{minor}.{rev}" + (f"+{build}" if build else ""), "sha256": sha,
            "size": len(data)}


def dfu_state_requests(sha: bytes, mode: str) -> list:
    """The SMP requests that follow an upload: mark the image to test or confirm it."""
    from smpclient.requests.image_management import ImageStatesWrite

    if mode == "test":
        return [ImageStatesWrite(hash=sha, confirm=False)]
    if mode == "confirm":
        return [ImageStatesWrite(hash=sha, confirm=True)]
    return []


def format_image_states(images) -> list[str]:
    lines = []
    for im in images:
        flags = [n for n in ("active", "confirmed", "pending", "permanent", "bootable") if getattr(im, n, False)]
        h = bytes(im.hash).hex() if getattr(im, "hash", None) else "-"
        lines.append(f"slot {im.slot}: {im.version:<12} {h[:16]}…  {' '.join(flags) or '-'}")
    return lines


def smp_ok(rsp) -> bool:
    from smpclient.generics import success

    return success(rsp)


def smp_transport_class():
    """smpclient's BLE transport, made safe for bursts and pipelining.

    - send() waits for room before each Write Without Response
      (wait_write_room: CoreBluetooth silently drops writes it has no room
      for, and one lost write ruins a reassembled SMP request);
    - receive() takes one response off the front of the buffer, so two
      responses that arrive back to back (a window of requests) don't break it;
    - max_unencoded_size can be capped (--chunk).
    """
    from smp import header as smphdr
    from smpclient.transport.ble import SMPBLETransport

    class MgSMPBLETransport(SMPBLETransport):
        chunk_cap: int | None = None

        async def send(self, data: bytes) -> None:
            for off in range(0, len(data), self.mtu):
                await wait_write_room(self._client)
                await self._client.write_gatt_char(self._smp_characteristic, data[off:off + self.mtu],
                                                   response=False)

        async def receive(self) -> bytes:
            async with self._notify_condition:
                while True:
                    if len(self._buffer) >= smphdr.Header.SIZE:
                        h = smphdr.Header.loads(bytes(self._buffer[:smphdr.Header.SIZE]))
                        n = h.length + smphdr.Header.SIZE
                        if len(self._buffer) >= n:
                            out = bytes(self._buffer[:n])
                            del self._buffer[:n]
                            return out
                    await self._notify_or_disconnect()

        @property
        def max_unencoded_size(self) -> int:
            n = super().max_unencoded_size
            return min(n, self.chunk_cap) if self.chunk_cap else n

    return MgSMPBLETransport


async def smp_client(args, addr: str | None = None):
    from smpclient import SMPClient

    if addr is None:
        addr, _ = await pick(args)
    t = smp_transport_class()()
    t.chunk_cap = getattr(args, "chunk", None)
    return SMPClient(t, addr, timeout_s=10.0)


async def smp_list(client) -> list:
    from smpclient.requests.image_management import ImageStatesRead

    rsp = await client.request(ImageStatesRead())
    if not smp_ok(rsp):
        raise SystemExit(f"FAIL  image list: {rsp}")
    return list(rsp.images)


async def smp_params(client) -> tuple[int, int] | None:
    """The server's SMP buffer size and count (OS group MCUmgr parameters), or None."""
    from smpclient.requests.os_management import MCUMgrParametersRead

    try:
        rsp = await client.request(MCUMgrParametersRead())
    except Exception:
        return None
    return (rsp.buf_size, rsp.buf_count) if smp_ok(rsp) else None


def smp_decode(req, frame: bytes):
    """A response frame for req, as SMPClient.request decodes it."""
    for kind in (req._Response, req._ErrorV1, req._ErrorV2):
        try:
            return kind.loads(frame)
        except Exception:
            continue
    raise ValueError(f"undecodable SMP response: {frame[:16].hex()}")


async def smp_upload(client, image: bytes, window: int = 1, progress=None, timeout_s: float = 10.0) -> int:
    """Uploads image to the second slot with up to window requests in flight.

    The first request (offset 0, length and SHA-256) goes alone: the server
    prepares the slot before answering. After it, requests are pipelined:
    Zephyr's SMP transport queues them and handles them in order, so each
    response's offset says where the next one should have started; on a
    mismatch (a request lost or refused) the outstanding ones are drained and
    the upload resumes from the server's offset. Returns the number of
    requests sent.
    """
    from collections import deque
    from smpclient.requests.image_management import ImageUploadWrite

    tr = client._transport
    first = client._maximize_upload_packet(
        ImageUploadWrite(off=0, data=b"", image=0, len=len(image), sha=hashlib.sha256(image).digest(),
                         upgrade=False), image)
    rsp = await client.request(first, timeout_s=40.0)
    if not smp_ok(rsp) or rsp.off is None:
        raise RuntimeError(f"upload refused: {rsp}")
    acked = next_off = rsp.off
    sent = 1
    if progress:
        progress(acked)
    pending: deque = deque()
    while acked < len(image):
        while len(pending) < max(1, window) and next_off < len(image):
            req = client._maximize_upload_packet(ImageUploadWrite(off=next_off, data=b""), image)
            await tr.send(req.BYTES)
            pending.append(req)
            next_off += len(req.data)
            sent += 1
        req = pending.popleft()
        frame = await asyncio.wait_for(tr.receive(), timeout_s)
        rsp = smp_decode(req, frame)
        if not smp_ok(rsp) or rsp.off is None:
            raise RuntimeError(f"upload failed at offset {req.off}: {rsp}")
        acked = rsp.off
        if acked != req.off + len(req.data):
            # The server is somewhere else: forget what's in flight, resume there.
            for _ in range(len(pending)):
                try:
                    await asyncio.wait_for(tr.receive(), 2.0)
                except Exception:
                    break
            pending.clear()
            next_off = acked
        if progress:
            progress(acked)
    return sent


async def cmd_images(args) -> int:
    async with await smp_client(args) as client:
        for line in format_image_states(await smp_list(client)):
            print(line)
    return 0


async def smp_reconnect(args, addr: str, deadline_s: float = 90.0):
    """Connects again once the gadget is back (after a reset and MCUboot's swap)."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < deadline_s:
        client = await smp_client(args, addr)
        try:
            await client.connect(10.0)
            return client
        except Exception:
            await asyncio.sleep(1.0)
    raise SystemExit("FAIL  the gadget didn't come back after the reset")


async def cmd_dfu(args) -> int:
    from smpclient.requests.os_management import ResetWrite

    r = Results()
    data = Path(args.image).read_bytes()
    info = mcuboot_image_info(data)
    print(f"image {args.image}: version {info['version']}, {info['size']} bytes, sha256 {info['sha256'].hex()[:16]}…")
    mode = "test" if args.test else "confirm" if args.confirm else None
    addr, _ = await pick(args)
    t_connect = time.monotonic()
    client = await smp_client(args, addr)
    await client.connect(20.0)
    try:
        print(f"connected in {time.monotonic() - t_connect:.1f} s, ATT write {client._transport.mtu} B")
        params = await smp_params(client)
        window = args.window
        if params:
            buf_size, buf_count = params
            # One buffer holds the response, one the request being reassembled.
            auto = max(1, min(2, buf_count - 2))
            window = window or auto
            print(f"SMP server: {buf_size} B x {buf_count} buffers; requests of up to "
                  f"{client._transport.max_unencoded_size} B, window {window}")
        else:
            window = window or 1
            print(f"SMP server: no MCUmgr parameters (older firmware): one ATT write per request, window {window}")
        before = await smp_list(client)
        for line in format_image_states(before):
            print("before  " + line)
        print("NOTE  uploading drops the gadget's offline clips (download them first with `queue --download`)")
        t0, last = time.monotonic(), [-1]

        def progress(off: int) -> None:
            pct = off * 100 // len(data)
            if pct // 10 != last[0]:
                last[0] = pct // 10
                el = time.monotonic() - t0
                print(f"  {pct:3d}%  {off} of {len(data)} bytes, {off / max(el, 1e-3) / 1024:.1f} KB/s")

        sent = await smp_upload(client, data, window, progress)
        secs = time.monotonic() - t0
        r.check("upload", True, f"{len(data)} bytes in {secs:.1f} s = {len(data) / secs / 1024:.1f} KB/s "
                f"({sent} requests, {len(data) / max(sent, 1):.0f} B each, window {window})")
        after = await smp_list(client)
        r.check("image in the second slot",
                any(im.slot == 1 and bytes(im.hash) == info["sha256"] for im in after),
                f"version {info['version']}")
        for req in dfu_state_requests(info["sha256"], mode):
            rsp = await client.request(req)
            r.check(f"mark for {mode}", smp_ok(rsp), "" if smp_ok(rsp) else str(rsp))
        for line in format_image_states(await smp_list(client)):
            print("after   " + line)
        if not args.reset:
            if mode:
                print("NOTE  installs at the next reset (`dfu ... --reset`, or reset the board)")
            return 1 if r.failed else 0
        rsp = await client.request(ResetWrite())
        r.check("reset", smp_ok(rsp), "the gadget restarts" + (" into the new image" if mode else ""))
        t_reset = time.monotonic()
    finally:
        try:
            await client.disconnect()
        except Exception:
            pass
    if not mode:
        return 1 if r.failed else 0
    # Reset and MCUboot's swap, then the new image confirming itself once BLE is up.
    await asyncio.sleep(2.0)
    client = await smp_reconnect(args, addr)
    t_back = time.monotonic()
    r.check("back after reset and swap", True, f"{t_back - t_reset:.1f} s")
    try:
        confirmed = False
        while time.monotonic() - t_back < 30.0:
            imgs = await smp_list(client)
            run = next((im for im in imgs if im.slot == 0), None)
            if run and bytes(run.hash) == info["sha256"] and (run.confirmed or mode == "confirm"):
                confirmed = True
                break
            await asyncio.sleep(1.0)
        r.check("running and confirmed", confirmed,
                f"{time.monotonic() - t_back:.1f} s after reconnecting" if confirmed
                else "the new image isn't running confirmed (reverted, or not confirmed yet)")
        for line in format_image_states(await smp_list(client)):
            print("now     " + line)
    finally:
        await client.disconnect()
    print(f"phases: upload {secs:.1f} s, reset and swap {t_back - t_reset:.1f} s, total "
          f"{time.monotonic() - t0:.1f} s")
    return 1 if r.failed else 0


async def cmd_log(args) -> int:
    from bleak import BleakClient

    addr, _ = await pick(args)
    async with BleakClient(addr, timeout=20.0) as client:
        await client.start_notify(P["MG_NUS_TX_UUID"],
                                  lambda _c, d: print(bytes(d).decode("utf-8", "replace"), end="", flush=True))
        if args.command:
            await client.write_gatt_char(P["MG_NUS_RX_UUID"], args.command.encode() + b"\n", response=True)
        await asyncio.sleep(args.secs)
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--address", help="BLE address (a UUID on macOS) from `scan`")
    ap.add_argument("--name", default=None, help=f"name prefix (default: any {P['MG_DEVICE_NAME_PREFIX']}*)")
    ap.add_argument("--scan-secs", type=float, default=6.0)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("scan")
    t = sub.add_parser("test")
    t.add_argument("--secs", type=float, default=2.0, help="how long to capture")
    t.add_argument("--codec", choices=("sbc", "lc3"), default="sbc")
    t.add_argument("--out", default="mg_capture.wav")
    p = sub.add_parser("ptt")
    p.add_argument("--count", type=int, default=0, help="utterances to record (0: until Ctrl-C)")
    p.add_argument("--haptics", action="store_true", help="also turn on the activation buzz")
    p.add_argument("--out-dir", default="mg_ptt")
    p.add_argument("--no-states", action="store_true",
                   help="don't send assistant_state (responding, then done) after each utterance")
    p.add_argument("--think", type=float, default=1.0, help="seconds of thinking before responding")
    p.add_argument("--respond", type=float, default=1.5, help="seconds of responding before done")
    su = sub.add_parser("setup")
    su.add_argument("--access", help="the access token to provision (default: a generated bench-access-...)")
    su.add_argument("--refresh", help="the refresh token (default: a generated bench-refresh-...)")
    pr = sub.add_parser("proof")
    pr.add_argument("--clear", action="store_true", help="after a match, have the gadget reset to setup")
    pr.add_argument("--verbose", action="store_true")
    s = sub.add_parser("state")
    s.add_argument("state", choices=ASSISTANT_STATES)
    s.add_argument("--wait", type=float, default=4.0, help="seconds to watch the face log")
    q = sub.add_parser("queue")
    q.add_argument("--download", action="store_true")
    q.add_argument("--clear", action="store_true", help="erase the queue afterwards")
    q.add_argument("--out-dir", default="mg_queue")
    pl = sub.add_parser("play")
    pl.add_argument("--tone", type=float, default=1000.0, help="tone frequency in Hz (default 1000)")
    pl.add_argument("--secs", type=float, default=3.0)
    pl.add_argument("--sweep", action="store_true", help="a 200 Hz to 7 kHz sweep instead of a tone")
    pl.add_argument("--wav", help="play a 16-bit WAV file instead")
    pl.add_argument("--rate", type=int, default=16000, choices=(16000, 32000, 44100, 48000),
                    help="SBC sample rate for --tone/--sweep")
    pl.add_argument("--level", type=float, default=-12.0, help="tone level in dBFS")
    pl.add_argument("--bitpool", type=int, default=26)
    pl.add_argument("--volume", type=int, choices=range(0, 101), metavar="0-100",
                    help="set the gadget's speaker_volume first (0 turns the speaker off); default: leave it")
    pl.add_argument("--loopback", action="store_true",
                    help="record the gadget's mic while it plays and check the tone comes back")
    pl.add_argument("--out", default="mg_loopback.wav", help="where --loopback saves the mic capture")
    pl.add_argument("--verbose", action="store_true")
    b = sub.add_parser("bench")
    b.add_argument("--send", type=int, metavar="SECS", help="device -> client for SECS seconds")
    b.add_argument("--recv", type=int, metavar="SECS", help="client -> device for SECS seconds")
    sub.add_parser("images")
    d = sub.add_parser("dfu")
    d.add_argument("--image", required=True, help="an MCUboot-signed image (zephyr.signed.bin)")
    m = d.add_mutually_exclusive_group()
    m.add_argument("--test", action="store_true", help="run it once at the next reset (reverts unless confirmed)")
    m.add_argument("--confirm", action="store_true", help="make it permanent")
    d.add_argument("--reset", action="store_true",
                   help="restart the gadget afterwards; with --test/--confirm, wait for it to come back confirmed")
    d.add_argument("--chunk", type=int, help="cap each SMP request at this many bytes (default: the server's buffer)")
    d.add_argument("--window", type=int, default=0,
                   help="requests in flight (default: 2 when the server has 4+ buffers, else 1)")
    lg = sub.add_parser("log")
    lg.add_argument("--secs", type=float, default=30.0)
    lg.add_argument("--command", help="a console command to send first (status, version)")
    args = ap.parse_args(argv)
    fn = {"scan": cmd_scan, "test": cmd_test, "ptt": cmd_ptt, "queue": cmd_queue, "log": cmd_log,
          "play": cmd_play, "state": cmd_state, "setup": cmd_setup, "proof": cmd_proof,
          "images": cmd_images,
          "dfu": cmd_dfu, "bench": cmd_bench}[args.cmd]
    try:
        return asyncio.run(fn(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
