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

"""tools/mg_ble_client.py without a radio: it reads the protocol from
protocols/mgcommands.h, parses notifications, and decodes the firmware's SBC
and LC3 notifications back to the audio that went in."""

from __future__ import annotations

import array
import asyncio
import ctypes
import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import mg_ble_client as C  # noqa: E402
from test_mg_ble import MG, PROTOCOLS, ROOT, codec_build, compile_and_run  # noqa: E402
from test_mg_codecs import best_snr  # noqa: E402


class MgBleClientTest(unittest.TestCase):
    def test_protocol_comes_from_the_header(self) -> None:
        P = C.P
        self.assertEqual(P["MG_SERVICE_UUID"], "65e6635d-174a-4af0-805e-d1eded7c2a63")
        self.assertEqual(P["MG_NUS_TX_UUID"], "6e400003-b5a3-f393-e0a9-e50e24dcca9e")
        self.assertEqual(P["mg_command_start_mic"], 1)
        self.assertEqual(P["mg_command_sub_feature"], 0xFE)
        self.assertEqual(P["mg_data_type_audio_lc3"], 19)
        self.assertEqual(C.uuid16(P["MG_BAS_BATTERY_LEVEL_UUID16"]), "00002a19-0000-1000-8000-00805f9b34fb")
        self.assertEqual(P["MG_DEVICE_NAME_PREFIX"], "MuseGadget")
        for name in C.SETTING_WIDTH:
            self.assertIn("mg_setting_parameter_" + name, P)

    def test_parsers(self) -> None:
        f = C.parse_change_data_type(bytes.fromhex("08 01 803e 01 3c00 401f"))
        self.assertEqual(f, {"type": 1, "rate": 16000, "channels": 1, "frame_bytes": 60, "frame_us": 8000})
        f = C.parse_change_data_type(bytes([8, 19]))
        self.assertEqual((f["frame_bytes"], f["frame_us"]), (40, 10000))
        s = C.parse_settings(bytes.fromhex("3f 00 01 02 01 04 2c01 07 01"))
        self.assertEqual(s, {"spec_version": 1, "push_to_talk_enabled": 1, "ptt_buzz_freq_hz": 300, "audio_codec": 1})
        self.assertEqual(C.parse_error(bytes.fromhex("19 41 01 0100")), "error on set_settings: unsupported")
        self.assertIn("supported_features[start_mic] [1, 19]", C.describe(bytes.fromhex("10 fe 01 01 13")))

    def test_decodes_the_firmware_codecs(self) -> None:
        srcs, defs, incs = codec_build(True)
        with tempfile.TemporaryDirectory(prefix="mg-client-") as tmp:
            t = Path(tmp)
            compile_and_run(self, [ROOT / "tests/mg_codec_harness.c", MG / "mg_codec.c"], defs,
                            [MG, PROTOCOLS, *incs], vendor=srcs,
                            args=[str(t / "voice.sbc"), str(t / "voice.lc3")], cwd=t)
            ref = array.array("h", (t / "input.raw").read_bytes())
            dec = C.Decoder(cache=t / "cache")
            sbc = (t / "voice.sbc").read_bytes()
            # Whole frames per notification, as the gadget sends them: 3 SBC frames of 60 bytes.
            chunks = [sbc[i:i + 180] for i in range(0, len(sbc), 180)]
            pcm, frames = dec.decode({"type": 1, "rate": 16000, "frame_bytes": 60, "frame_us": 8000}, chunks)
            self.assertEqual(frames, len(sbc) // 60)
            self.assertGreater(best_snr(ref, array.array("h", pcm)), 20.0)
            with self.assertRaises(ValueError):
                dec.decode({"type": 1}, [sbc[:90]])   # a frame and a half
            lc3 = (t / "voice.lc3").read_bytes()
            chunks = [lc3[i:i + 80] for i in range(0, len(lc3), 80)]
            pcm, frames = dec.decode(C.parse_change_data_type(bytes([8, 19])), chunks)
            self.assertEqual(frames, len(lc3) // 40)
            self.assertGreater(best_snr(ref, array.array("h", pcm)), 12.0)
            C.write_wav(t / "out.wav", pcm)
            self.assertGreater((t / "out.wav").stat().st_size, 44)


    def test_playback_helpers(self) -> None:
        caps = C.parse_capabilities(bytes.fromhex(
            "1d 01 05 01 02 01 06 02 00400000 03 02 803e0000 80bb0000 04 0001 05 01"))
        self.assertEqual(caps, {"codecs": [1, 6], "buffer_size": 16384, "sample_rates": [16000, 48000],
                                "max_bitrate": 256, "max_channels": 1})
        self.assertEqual(C.start_streaming(1, 16000).hex(" "), "1d 02 03 01 01 02 80 3e 00 00 04 01")
        t = C.tone_pcm(1000, 1.0, dbfs=-12)
        self.assertAlmostEqual(C.goertzel_db(t, 1000), -12, delta=0.2)
        self.assertLess(C.goertzel_db(t, 1250), -50)
        self.assertAlmostEqual(C.zero_cross_hz(t), 1000, delta=5)
        sw = C.sweep_pcm(200, 7000, 1.0)
        self.assertEqual(len(sw), 16000)
        self.assertLess(C.goertzel_db(sw[:2000], 5000), C.goertzel_db(sw[:2000], 250))   # starts low
        self.assertEqual(len(C.resample_linear(t, 16000, 48000)), 48000)
        self.assertIn("stop_streaming keep", C.describe(bytes.fromhex("1d 03 00")))
        text = ("I (1) mg.play: stream end (keep): sbc 16000 Hz, 250 frames in, 250 decoded (0 bad), 0 underruns, "
                "110 ms silence\nI (2) mg.play: stream bytes: 15000 received (0 dropped), 2000 ms played\n"
                "I (3) mg.play: stream level: decoded rms -15.1 peak -12.0 dBFS, speaker rms -15.1 peak -12.0 dBFS\n")
        st = C.parse_stream_stats(text)
        self.assertEqual((st["how"], st["decoded"], st["played_ms"], st["peak"], st["speaker_off"], st["off_ms"]),
                         ("keep", 250, 2000, -12.0, False, 0))
        st = C.parse_stream_stats(text.replace("speaker rms -15.1 peak -12.0 dBFS",
                                               "speaker rms -120.0 peak -120.0 dBFS, speaker off"))
        self.assertTrue(st["speaker_off"])
        self.assertEqual((st["rms"], st["decoded_rms"]), (-120.0, -15.1))
        st = C.parse_stream_stats(text.replace("peak -12.0 dBFS\n", "peak -12.0 dBFS, speaker off 500 ms\n"))
        self.assertEqual((st["speaker_off"], st["off_ms"]), (False, 500))
        self.assertEqual(C.SETTING_WIDTH["speaker_volume"], 1)
        m = C.FACE_RE.search("I (5123) mg.face: face thinking")
        self.assertEqual(m.group(1), "thinking")
        self.assertEqual(C.FACE_RE.search("I (1) mg.face: ring responding\r").group(1), "responding")
        self.assertEqual([C.assistant_state(s) for s in C.ASSISTANT_STATES], [0, 2, 3, 4, 5])
        self.assertEqual(C.setting_name(10), "speaker_volume")
        self.assertEqual(C.mask_token("mgst_0123456789abcdef"), "mgst_...cdef (21 chars)")
        self.assertNotIn("0123456789", C.mask_token("mgst_0123456789abcdef"))
        self.assertEqual(C.token_fingerprint(b"abc"), "ba7816bf")
        self.assertEqual(C.setting_id("speaker_volume"), 10)

    def test_token_proof_against_the_vectors(self) -> None:
        import json
        data = json.loads((PROTOCOLS / "test-vectors/mg-token-proof-v1.json").read_text(encoding="utf-8"))
        for v in data["vectors"]:
            k = C.proof_key(v["access_token"], v["node_id"])
            cn, dn = bytes.fromhex(v["client_nonce"]), bytes.fromhex(v["device_nonce"])
            self.assertEqual(k.hex(), v["K"], v["name"])
            self.assertEqual(C.proof_mac(k, True, cn, dn).hex(), v["device_mac"], v["name"])
            self.assertEqual(C.proof_mac(k, False, cn, dn).hex(), v["client_mac"], v["name"])
        self.assertEqual(C.P["mg_command_token_proof"], 34)
        self.assertEqual(C.P["mg_error_code_proof_required"], 6)
        self.assertEqual((C.P["MG_TOKEN_PROOF_NONCE_LEN"], C.P["MG_TOKEN_PROOF_MAC_LEN"]), (16, 32))

    def test_proof_record_is_private(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            old = os.environ.get("MG_BLE_CLIENT_HOME")
            os.environ["MG_BLE_CLIENT_HOME"] = str(Path(tmp) / "home")
            try:
                k = bytes(range(32))
                path = C.save_proof_record("AB:CD/12-34", "homelink-a7f5b4", k, "bench-access-x")
                self.assertEqual(path.stat().st_mode & 0o777, 0o600)
                self.assertEqual(path.parent.stat().st_mode & 0o777, 0o700)
                self.assertNotIn("bench-access-x", path.read_text())   # the key, never the token
                rec = C.load_proof_record("ab:cd/12-34")
                self.assertEqual((rec["proof_key"], rec["node_id"]), (k, "homelink-a7f5b4"))
                self.assertIsNone(C.load_proof_record("other"))
            finally:
                if old is None:
                    os.environ.pop("MG_BLE_CLIENT_HOME", None)
                else:
                    os.environ["MG_BLE_CLIENT_HOME"] = old

    def test_setup_framing_and_uuids(self) -> None:
        # The setup service's UUIDs, as ble_server.c declares them (little-endian bytes).
        import re
        text = (ROOT / "main/ble_server.c").read_text()

        def declared(name: str) -> str:
            m = re.search(name + r" = BLE_UUID128_INIT\(([^)]*)\)", text)
            b = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1)))[::-1].hex()
            return f"{b[:8]}-{b[8:12]}-{b[12:16]}-{b[16:20]}-{b[20:]}"

        self.assertEqual(declared("SVC_UUID"), C.SETUP_SERVICE_UUID)
        self.assertEqual(declared("RX_UUID"), C.SETUP_RX_UUID)
        self.assertEqual(declared("TX_UUID"), C.SETUP_TX_UUID)
        chunks = C.setup_chunks(b"x" * 400, 185)
        self.assertEqual([len(c) for c in chunks], [180, 180, 49])
        self.assertEqual(chunks[1][:3], bytes([0xFE, 1, 3]))
        r = C.SetupReassembler()
        self.assertEqual([r.feed(c) for c in chunks], [None, None, b"x" * 400])
        self.assertEqual(r.feed(b"error_pairing_unavailable"), b"error_pairing_unavailable")
        self.assertEqual(C.unb64url(C.b64url(b"\xff\x00\x01")), b"\xff\x00\x01")

    def test_scans_keep_every_payloads_uuids(self) -> None:
        # A gadget takes turns between the Link setup and musegadgets payloads
        # from one address: whichever packet came last, both commands find it.
        import types

        mg, setup = C.P["MG_SERVICE_UUID"], C.SETUP_SERVICE_UUID
        packets = [("AA:01", "MuseGadget-A1", -60, [setup.upper()]), ("AA:01", "", -55, [mg]),
                   ("BB:02", "other", -40, ["0000180f-0000-1000-8000-00805f9b34fb"])]

        class FakeScanner:
            def __init__(self, detection_callback):
                self.cb = detection_callback

            async def __aenter__(self):
                for addr, name, rssi, uuids in packets:
                    self.cb(types.SimpleNamespace(address=addr, name=None),
                            types.SimpleNamespace(local_name=name, rssi=rssi, service_uuids=uuids))
                return self

            async def __aexit__(self, *exc):
                return False

        args = types.SimpleNamespace(scan_secs=0, address=None, name=None)
        saved = sys.modules.get("bleak")
        sys.modules["bleak"] = types.SimpleNamespace(BleakScanner=FakeScanner)
        try:
            setup_hits = asyncio.run(C.find_setup(args))
            mg_hits = asyncio.run(C.find(args))
        finally:
            if saved is None:
                del sys.modules["bleak"]
            else:
                sys.modules["bleak"] = saved
        self.assertEqual(setup_hits, [("AA:01", "MuseGadget-A1", -55)])
        self.assertEqual(mg_hits, [("AA:01", "MuseGadget-A1", -55, True)])

    def test_setup_crypto_against_link_vectors(self) -> None:
        try:
            import cryptography  # noqa: F401
        except ImportError:
            self.skipTest("pip install cryptography for the Link setup client tests")
        import json
        v = next(x for x in json.loads((ROOT / "tests/vectors/link_pairing_v5.json").read_text())["vectors"]
                 if x["name"] == "community_v5")
        s = C.PairingSession(None, private_value=int(v["mobile_private_scalar_hex"], 16),
                             mobile_nonce=C.unb64url(v["mobile_nonce"]))
        self.assertEqual(s.mobile_pub, v["mobile_pub"])
        transcript = C.pairing_transcript(v, s.mobile_pub, v["mobile_nonce"])
        self.assertEqual(transcript, v["transcript"])
        th = C.hashlib.sha256(transcript.encode()).digest()
        self.assertEqual(C.b64url(th), v["transcript_hash"])
        keys = C.pairing_keys(bytes.fromhex(v["ecdh_secret_hex"]), C.unb64url(v["mobile_nonce"]),
                              C.unb64url(v["device_nonce"]), th)
        self.assertEqual(keys["session_secret"].hex(), v["session_secret_hex"])
        self.assertEqual(keys["tx"].hex(), v["mobile_tx_key_hex"])
        self.assertEqual(keys["rx"].hex(), v["mobile_rx_key_hex"])
        self.assertEqual(keys["session_id"], v["session_id"])
        self.assertEqual(C.record_aad(keys["session_id"], 0, 0).decode(), v["client_finished_aad"])
        env = C.seal_record(keys["tx"], keys["session_id"], 0, v["client_finished_plaintext"].encode())
        self.assertEqual((env["ciphertext"], env["tag"]), (v["client_finished_ciphertext"], v["client_finished_tag"]))
        self.assertEqual(env["action"], "pairing_encrypted")
        # The whole pairing_ready check, ECDH included.
        ready = dict(type="pairing_ready", version=5, pairing_auth="none", pairing_auth_epoch=0,
                     pairing_policy="confirm_press", transcript_hash=v["transcript_hash"],
                     session_id=v["session_id"], **{k: v[k] for k in ("device_id", "node_id", "mac", "model",
                                                                       "firmware_version", "device_pub",
                                                                       "device_nonce")})
        s.accept_ready(ready)
        self.assertEqual(s.keys["tx"].hex(), v["mobile_tx_key_hex"])
        with self.assertRaises(RuntimeError):
            s.accept_ready(dict(ready, transcript_hash=C.b64url(bytes(32))))
        # Records the device seals (direction 1) open, and only with the right counter.
        dev = C.seal_record(keys["rx"], keys["session_id"], 0, b'{"type":"status"}', direction=1)
        self.assertEqual(C.open_record(keys["rx"], keys["session_id"], 0, dev), b'{"type":"status"}')
        with self.assertRaises(ValueError):
            C.open_record(keys["rx"], keys["session_id"], 1, dev)

    def test_setup_flow_against_a_simulated_device(self) -> None:
        try:
            from cryptography.hazmat.primitives.asymmetric import ec
            from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat
        except ImportError:
            self.skipTest("pip install cryptography for the Link setup client tests")
        import json

        class Device:
            """The device side of link_pairing.c for one setup, plus a token-only provision_v2."""

            def __init__(self, wifi="none"):
                self.wifi, self.got, self.notify = wifi, None, None
                self.priv = ec.derive_private_key(2, ec.SECP256R1())
                self.pub = C.b64url(self.priv.public_key().public_bytes(Encoding.X962,
                                                                        PublicFormat.UncompressedPoint))
                self.rx = C.SetupReassembler()
                self.tx_counter = self.rx_counter = 0
                self.mtu_size = 185

            def send(self, obj):
                for c in C.setup_chunks(json.dumps(obj).encode(), self.mtu_size):
                    self.notify(None, bytearray(c))

            def status(self, status):
                body = json.dumps({"type": "status", "status": status}).encode()
                self.send(C.seal_record(self.keys["rx"], self.keys["session_id"], self.tx_counter, body, 1))
                self.tx_counter += 1

            async def start_notify(self, uuid, cb):
                assert uuid == C.SETUP_TX_UUID
                self.notify = cb

            async def write_gatt_char(self, uuid, data, response=False):
                assert uuid == C.SETUP_RX_UUID
                msg = self.rx.feed(bytes(data))
                if msg is None:
                    return
                m = json.loads(msg)
                act = m.get("action")
                if act == "get_device_info":
                    info = {"type": "device_info", "node_id": "homelink-000001", "pairing_protocol": 5,
                            "pairing_auth": "none", "pairing_policy": "confirm_press", "mgcommands": 1}
                    if self.wifi != "required":
                        info["wifi"] = self.wifi
                    self.send(info)
                elif act == "pairing_client_hello":
                    dn = bytes(range(16, 32))
                    ready = {"type": "pairing_ready", "version": 5, "device_id": "hatch-link:02:00:00:00:00:01",
                             "node_id": "homelink-000001", "mac": "02:00:00:00:00:01", "model": "hatch_link",
                             "firmware_version": "1.0.0", "pairing_auth": "none", "pairing_auth_epoch": 0,
                             "pairing_policy": "confirm_press", "device_pub": self.pub, "device_nonce": C.b64url(dn)}
                    th = C.hashlib.sha256(C.pairing_transcript(ready, m["mobile_pub"], m["mobile_nonce"])
                                          .encode()).digest()
                    peer = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), C.unb64url(m["mobile_pub"]))
                    keys = C.pairing_keys(self.priv.exchange(ec.ECDH(), peer), C.unb64url(m["mobile_nonce"]), dn, th)
                    # The device's rx is the mobile's tx.
                    self.keys = {"rx": keys["rx"], "tx": keys["tx"], "session_id": keys["session_id"]}
                    self.send(dict(ready, transcript_hash=C.b64url(th), session_id=keys["session_id"]))
                elif act == "pairing_encrypted":
                    plain = json.loads(C.open_record(self.keys["tx"], self.keys["session_id"], self.rx_counter,
                                                     m, direction=0))
                    self.rx_counter += 1
                    if plain["action"] == "pairing_client_finished":
                        self.status("confirm_required")
                    elif plain["action"] == "provision_v2":
                        assert "ssid" not in plain and plain["token_type"] == "device"
                        self.got = plain
                        self.status("auth_ok")

        async def run(dev):
            link = C.SetupLink(dev)
            await link.start()
            prompts = []

            def press(msg):
                prompts.append(msg)
                dev.status("pairing_confirmed")   # the button

            got = await C.setup_flow(link, "bench-access-1", "bench-refresh-1", prompt=press)
            return got, prompts

        dev = Device()
        got, prompts = asyncio.run(run(dev))
        self.assertEqual(len(prompts), 1)
        self.assertEqual(dev.got["access_token"], "bench-access-1")
        self.assertEqual(dev.got["refresh_token"], "bench-refresh-1")
        self.assertEqual((got["node_id"], got["wifi"], got["mgcommands"]), ("homelink-000001", "none", True))
        self.assertEqual(got["proof_key"], C.proof_key("bench-access-1", "homelink-000001"))
        # A device that needs Wi-Fi isn't set up token-only.
        with self.assertRaises(RuntimeError):
            asyncio.run(run(Device(wifi="required")))

    def test_dfu_image_and_requests(self) -> None:
        """The MCUboot image parsing dfu relies on, and (with smpclient) the SMP packets it sends."""
        import hashlib
        import struct

        body = bytes(range(256)) * 10
        hdr = struct.pack("<IIHHII", C.MCUBOOT_IMAGE_MAGIC, 0, 0x200, 0, len(body), 0)
        hdr += struct.pack("<BBHI", 0, 1, 1, 0) + b"\0" * 4
        hdr += b"\xff" * (0x200 - len(hdr))
        sha = hashlib.sha256(hdr + body).digest()
        tlvs = struct.pack("<HH", 0x10, 32) + sha + struct.pack("<HH", 0x22, 4) + b"sig!"
        img = hdr + body + struct.pack("<HH", C.MCUBOOT_TLV_INFO_MAGIC, 4 + len(tlvs)) + tlvs
        info = C.mcuboot_image_info(img)
        self.assertEqual(info["version"], "0.1.1")
        self.assertEqual(info["sha256"], sha)
        self.assertEqual(info["size"], len(img))
        with self.assertRaises(ValueError):
            C.mcuboot_image_info(b"\0" * 64)  # an unsigned zephyr.bin
        # A protected TLV area (e.g. a security counter) comes first.
        prot = struct.pack("<HH", C.MCUBOOT_TLV_PROT_INFO_MAGIC, 12) + struct.pack("<HH", 0x50, 4) + b"\1\0\0\0"
        hdr2 = hdr[:10] + struct.pack("<H", len(prot)) + hdr[12:]
        self.assertEqual(C.mcuboot_image_info(hdr2 + body + prot + img[len(hdr) + len(body):])["sha256"], sha)
        signed = Path.home() / "zephyrproject/build-B/zephyr/zephyr/zephyr.signed.bin"
        if signed.exists():  # a real one, when a Zephyr build is around
            self.assertTrue(C.mcuboot_image_info(signed.read_bytes())["version"].startswith("0.1."))
        try:
            import smpclient  # noqa: F401
        except ImportError:
            self.skipTest("smpclient isn't installed: SMP packet checks skipped")
        test, = C.dfu_state_requests(sha, "test")
        confirm, = C.dfu_state_requests(sha, "confirm")
        self.assertEqual(C.dfu_state_requests(sha, None), [])
        # SMP v2 header: op 2 (write), group 1 (image), id 0 (state); CBOR {hash, confirm}.
        for req, flag in ((test, b"\xf4"), (confirm, b"\xf5")):
            raw = req.BYTES
            self.assertEqual(raw[0] & 0x07, 2)
            self.assertEqual(int.from_bytes(raw[4:6], "big"), 1)
            self.assertEqual(raw[7], 0)
            self.assertEqual(int.from_bytes(raw[2:4], "big"), len(raw) - 8)
            self.assertIn(b"dhashX\x20" + sha, raw)
            self.assertTrue(raw.endswith(b"gconfirm" + flag))

        class Im:
            def __init__(self, slot, active):
                self.slot, self.version, self.hash = slot, f"0.1.{slot}", bytes([slot]) * 32
                self.active, self.confirmed, self.pending = active, active, not active
        lines = C.format_image_states([Im(0, True), Im(1, False)])
        self.assertIn("slot 0: 0.1.0", lines[0])
        self.assertIn("active confirmed", lines[0])
        self.assertIn("pending", lines[1])

    def test_throughput_test_helpers(self) -> None:
        """bench's packets match the firmware's format, and the counter finds gaps and bursts."""
        self.assertEqual(C.P["mg_command_device_action"], 30)
        self.assertEqual(C.P["mg_device_action_throughput_test"], 11)
        self.assertEqual(C.P["mg_data_type_throughput_test"], 20)
        p = C.tp_fill(244, 0x01020304)
        self.assertEqual(p[:4], bytes([4, 3, 2, 1]))
        self.assertEqual(p[4], (4 + 4) & 0xFF)
        self.assertEqual(len(p), 244)
        c = C.TpCounter()
        t = 0.0
        for seq in (0, 1, 2, 3, 6, 7):          # 4 and 5 lost
            c.add(C.tp_fill(244, seq), t)
            t += 0.001 if seq % 2 == 0 else 0.015   # two per "event"
        c.add(C.tp_fill(244, 8)[:-1] + b"\0", t)    # corrupt
        self.assertEqual((c.packets, c.gaps, c.bad), (7, 2, 1))
        self.assertEqual(c.bytes, 7 * 244)
        self.assertEqual(c.bursts, [2, 2, 2, 1])
        rep = bytes([30, 11, 0]) + (1000).to_bytes(4, "little") + (4).to_bytes(4, "little") + \
            (50).to_bytes(4, "little") + (2).to_bytes(4, "little")
        self.assertEqual(C.parse_tp_report(rep), {"bytes": 1000, "packets": 4, "ms": 50, "gaps": 2})

    def test_dfu_pipelined_upload(self) -> None:
        """smp_upload against a fake SMP server that behaves like Zephyr's img_mgmt:
        requests handled in order, each answered with the next offset."""
        try:
            import smpclient  # noqa: F401
            from smp import header as smphdr
            from smp import image_management as smpimg
            from smpclient import SMPClient
            from smpclient.transport import SMPTransport
        except ImportError:
            self.skipTest("smpclient isn't installed")

        class FakeServer(SMPTransport):
            def __init__(self, drop_at: int | None = None):
                self.rx = bytearray()     # the image as written
                self.out: list[bytes] = []
                self.inflight = 0
                self.max_inflight = 0
                self.drop_at = drop_at
                self.requests = 0
                self.initialize(2475)

            @property
            def mtu(self) -> int:
                return 244

            async def connect(self, address, timeout_s):
                pass

            async def disconnect(self):
                pass

            async def send(self, data: bytes) -> None:
                self.inflight += 1
                self.max_inflight = max(self.max_inflight, self.inflight)
                h = smphdr.Header.loads(data[:smphdr.Header.SIZE])
                req = smpimg.ImageUploadWriteRequest.loads(data)
                self.requests += 1
                assert len(data) <= 2475, len(data)
                if self.drop_at is not None and self.requests == self.drop_at:
                    self.drop_at = None   # a lost request: no answer at all
                    self.inflight -= 1
                    return
                if req.off == len(self.rx):
                    self.rx += req.data
                # Out of order (after a loss): img_mgmt answers with where it is.
                self.out.append(smpimg.ImageUploadWriteResponse(sequence=h.sequence, off=len(self.rx)).BYTES)

            async def receive(self) -> bytes:
                while not self.out:
                    await asyncio.sleep(0)
                self.inflight -= 1
                return self.out.pop(0)

            async def send_and_receive(self, data: bytes) -> bytes:
                await self.send(data)
                return await self.receive()

        image = bytes(range(256)) * 400  # 100 KB
        for window, drop in ((1, None), (2, None), (3, None), (2, 7)):
            fake = FakeServer(drop)
            client = SMPClient(fake, "fake")
            offs = []
            if drop is None:
                sent = asyncio.run(C.smp_upload(client, image, window, offs.append, timeout_s=1.0))
            else:
                # A lost request times out its response; the next ones answer with the
                # server's offset and the upload resumes from it.
                async def run():
                    try:
                        return await C.smp_upload(client, image, window, offs.append, timeout_s=0.5)
                    except asyncio.TimeoutError:
                        return None
                sent = asyncio.run(run())
                if sent is None:
                    continue  # a loss with nothing behind it is a timeout, as with smpclient
            self.assertEqual(bytes(fake.rx), image, f"window {window}")
            self.assertEqual(offs[-1], len(image))
            self.assertLessEqual(fake.max_inflight, window)
            if window > 1 and drop is None:
                self.assertEqual(fake.max_inflight, window, "requests pipelined")
            # About 2.4 KB of image per request, not one ATT write's worth.
            self.assertLess(sent, len(image) // 2000 + 3)

    def test_smp_transport_receive_splits_responses(self) -> None:
        """Two responses arriving back to back come out one at a time."""
        try:
            from smp import image_management as smpimg
        except ImportError:
            self.skipTest("smpclient isn't installed")
        T = C.smp_transport_class()
        t = T()
        a = smpimg.ImageUploadWriteResponse(sequence=1, off=100).BYTES
        b = smpimg.ImageUploadWriteResponse(sequence=2, off=2500).BYTES
        t._buffer.extend(a + b)
        self.assertEqual(asyncio.run(t.receive()), a)
        self.assertEqual(asyncio.run(t.receive()), b)
        t.initialize(2475)
        t.chunk_cap = 1000
        self.assertEqual(t.max_unencoded_size, 1000)

    def test_play_against_the_firmware(self) -> None:
        """play_stream's flow control and checks against the firmware's own protocol and playback code."""
        srcs, defs, incs = codec_build(False)
        with tempfile.TemporaryDirectory(prefix="mg-fake-") as tmp:
            lib = Path(tmp) / ("fake.dylib" if sys.platform == "darwin" else "fake.so")
            cc = shlex.split(os.environ.get("CC", "cc"))
            cmd = [*cc, "-std=c11", "-O1", "-shared", "-fPIC", "-w", *[f"-D{d}" for d in defs]]
            for inc in (MG, MG / "include", PROTOCOLS, *incs):
                cmd += ["-I", str(inc)]
            cmd += [str(ROOT / "tests/mg_fake_gadget.c"), str(ROOT / "tests/mg_token_proof_ref.c"),
                    *(str(MG / f) for f in ("mg_proto.c", "mg_play.c", "mg_resample.c", "mg_codec.c",
                                            "mg_queue.c")), *map(str, srcs),
                "-lm", "-o", str(lib)]
            r = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[-4000:])
            fake = ctypes.CDLL(str(lib))
            asyncio.run(self._play(fake, Path(tmp)))

    async def _play(self, fake, tmp: Path) -> None:
        CB = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.POINTER(ctypes.c_ubyte), ctypes.c_int)
        uuids = {C.P["MG_CONTROL_UUID"]: 0, C.P["MG_DATA_UUID"]: 1}

        class FakeClient:
            mtu_size = 247

            def __init__(self):
                self.handlers = {}

            async def start_notify(self, uuid, cb):
                self.handlers[uuid] = cb

            async def write_gatt_char(self, uuid, data, response=False):
                buf = (ctypes.c_ubyte * len(data)).from_buffer_copy(bytes(data))
                fake.fake_write(uuids[uuid], buf, len(data))

        client = FakeClient()
        g = C.Gadget(client, verbose=False)
        await g.start()
        names = {0: C.P["MG_CONTROL_UUID"], 1: C.P["MG_DATA_UUID"]}
        notify = CB(lambda ch, d, n: client.handlers[names[ch]](None, bytearray(d[:n])))
        nus = CB(lambda ch, d, n: g._on_nus(None, bytearray(d[:n]) + b"\n"))
        # A 3 KB buffer, so 1.5 s of 60 kb/s SBC has to wait on buffer_update.
        fake.fake_init(3072, 247, notify, nus)

        stop = asyncio.Event()

        async def device():
            while not stop.is_set():
                fake.fake_step()
                await asyncio.sleep(0.02)   # real time: play_stream checks the duration

        task = asyncio.create_task(device())
        try:
            # The typed settings list: what a C6-like board accepts, no haptics.
            st = await g.status()
            self.assertEqual(st["settings"], [0, 2, 7, 10])
            self.assertIn(C.P["mg_command_assistant_state"], st["commands"])
            self.assertEqual(await C.set_setting(g, "haptics_enabled", 1) != [], True)
            await g.send(C.P["mg_command_assistant_state"], C.assistant_state("responding"))
            self.assertEqual(fake.fake_assistant(), 3)
            # Face lines on the log mirror are picked out whole, across notifications.
            g.face_log = True
            g._on_nus(None, bytearray(b"I (1) mg.face: face thi"))
            g._on_nus(None, bytearray(b"nking\nI (2) mg.play: x\n"))
            self.assertEqual(g.faces, ["thinking"])
            g.face_log = False
            # The token proof against the firmware's own protocol code: no key, a match, a
            # wrong key (the device's MAC is caught before any confirm), then clear.
            self.assertIn(C.P["mg_command_token_proof"], st["commands"])
            k = C.proof_key("bench-access-0123", "homelink-a7f5b4")
            self.assertEqual(await C.run_proof(g, k), "not_found")
            key = (ctypes.c_ubyte * 32).from_buffer_copy(k)
            fake.fake_set_proof_key(key)
            self.assertEqual(await C.run_proof(g, k), "match")
            self.assertEqual(await C.run_proof(g, C.proof_key("other-token", "homelink-a7f5b4")),
                             "device_mac_mismatch")
            self.assertEqual(await C.run_proof(g, k, clear=True), "match")
            self.assertEqual(fake.fake_proof_clears(), 1)
            self.assertEqual(await C.run_proof(g, k), "not_found")   # the fake forgot its key
            r = C.Results()
            res = await C.play_stream(g, C.Decoder(cache=tmp / "cache"), C.tone_pcm(1000, 1.5), 16000, r, 247)
            self.assertEqual(r.failed, [], "play_stream failed against the firmware")
            self.assertTrue(res["ok"])
            self.assertEqual(res["stats"]["decoded"], res["frames"])
            self.assertFalse(res["stats"]["speaker_off"])
            self.assertGreater(res["stats"]["rms"], -20)
            # speaker_volume: read, turn the speaker off, and a stream plays (paced) but silent at the codec.
            self.assertEqual(await C.get_setting(g, "speaker_volume"), 70)
            self.assertEqual(await C.set_setting(g, "speaker_volume", 0), [])
            self.assertEqual(await C.get_setting(g, "speaker_volume"), 0)
            self.assertIn("error", " ".join(await C.set_setting(g, "speaker_volume", 101)))
            r = C.Results()
            res = await C.play_stream(g, C.Decoder(cache=tmp / "cache"), C.tone_pcm(1000, 0.5), 16000, r, 247)
            self.assertEqual(r.failed, [])
            self.assertTrue(res["stats"]["speaker_off"])
            self.assertEqual(res["stats"]["rms"], -120.0)
            self.assertGreater(res["stats"]["decoded_rms"], -20)
            self.assertEqual(await C.set_setting(g, "speaker_volume", 60), [])
            # Push-to-talk during playback: the device ends it with stop_streaming drop.
            g.stops.clear()
            sa = C.P["mg_command_stream_audio"]
            await client.write_gatt_char(C.P["MG_CONTROL_UUID"], C.start_streaming(1, 16000))
            await asyncio.sleep(0.05)
            fake.fake_claim(1)
            self.assertEqual([s[1] for s in g.stops], [C.P["mg_stream_audio_buffer_drop"]])
            await client.write_gatt_char(C.P["MG_CONTROL_UUID"], C.start_streaming(1, 16000))
            self.assertTrue(any("busy" in e for e in g.stream_errors))
            fake.fake_claim(0)
            del sa
        finally:
            stop.set()
            await task


if __name__ == "__main__":
    unittest.main()
