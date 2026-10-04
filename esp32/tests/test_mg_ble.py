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

"""musegadgets BLE: the protocol state machine
(components/muse_gadget_ble/mg_proto.c) with the token proof against
protocols/test-vectors/mg-token-proof-v1.json, the offline clip queue with
power cuts, and the BLE/Wi-Fi routing policy, compiled for the host."""

from __future__ import annotations

import json
import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MG = ROOT / "components/muse_gadget_ble"
PROTOCOLS = ROOT.parent / "protocols"
PROOF_VECTORS = PROTOCOLS / "test-vectors/mg-token-proof-v1.json"


def proof_vectors_header(directory: Path) -> Path:
    """mg_proof_vectors.h: the token proof vectors as C strings (hex, and the UTF-8 inputs)."""
    data = json.loads(PROOF_VECTORS.read_text(encoding="utf-8"))

    def c(text: str) -> str:
        return '"' + "".join(f"\\x{b:02x}" for b in text.encode("utf-8")) + '"'

    rows = []
    for v in data["vectors"]:
        f = v["frames"]
        rows.append("    { " + ", ".join([
            c(v["access_token"]), c(v["node_id"]), c(v["client_nonce"]), c(v["device_nonce"]), c(v["K"]),
            c(v["device_mac"]), c(v["client_mac"]), c(f["challenge"]), c(f["response"]), c(f["confirm"]),
        ]) + " },")
    header = directory / "mg_proof_vectors.h"
    header.write_text(
        "#pragma once\n/* Generated from protocols/test-vectors/mg-token-proof-v1.json. */\n"
        "static const struct {\n    const char *access, *node, *cn, *dn, *k, *dmac, *cmac, *challenge, "
        "*response, *confirm;\n} PROOF_VECTORS[] = {\n" + "\n".join(rows) + "\n};\n")
    return header


def compile_and_run(test: unittest.TestCase, sources: list[Path], defines: list[str], includes: list[Path],
                    extra: list[str] | None = None, args: list[str] | None = None, cwd: Path | None = None,
                    vendor: list[Path] | None = None) -> str:
    """Builds the harness and firmware sources strictly, vendored codec sources as upstream ships them."""
    cc = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="mg-test-") as tmp:
        exe = Path(tmp) / "harness"
        common = ["-std=c11", "-O1", *[f"-D{d}" for d in defines]]
        for inc in includes:
            common += ["-I", str(inc)]
        objs = []
        for i, src in enumerate(vendor or []):
            obj = Path(tmp) / f"v{i}_{src.stem}.o"
            r = subprocess.run([*cc, *common, "-w", "-c", str(src), "-o", str(obj)], text=True, capture_output=True)
            test.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            objs.append(str(obj))
        cmd = [*cc, *common, "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
               "-Wno-missing-field-initializers", *[str(s) for s in sources], *objs, *(extra or []),
               "-lm", "-o", str(exe)]
        r = subprocess.run(cmd, text=True, capture_output=True)
        test.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        r = subprocess.run([str(exe), *(args or [])], text=True, capture_output=True, cwd=cwd or tmp)
        test.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout


def codec_build(lc3: bool) -> tuple[list[Path], list[str], list[Path]]:
    """Sources, defines and includes of the vendored codecs as the components build them."""
    sbc = ROOT.parent / "xplat/libsbc"
    srcs = [sbc / "src/sbc.c", sbc / "src/bits.c"]
    defs = ["SBC_MAX_CHANNELS=1", "SBC_WITH_4SB=0", "SBC_SMALL_CRC=1", "SBC_FAST_DCT=1",
            f"MG_WITH_LC3={int(lc3)}"]
    incs = [sbc / "include", sbc / "src"]
    if lc3:
        l = ROOT.parent / "xplat/liblc3"
        srcs += [l / "src" / f for f in ("attdet.c", "bits.c", "bwdet.c", "energy.c", "lc3.c", "ltpf.c",
                                         "mdct.c", "plc.c", "sns.c", "spec.c", "tables.c", "tns.c")]
        defs += ["LC3_ENC_LTPF=0", "LC3_DEC_LTPF=0", "LC3_PLUS=0", "LC3_PLUS_HR=0", "LC3_DT_MASK=8",
                 "LC3_SR_MASK=2"]
        incs += [l / "include", l / "src"]
    return srcs, defs, incs


class MgBleTest(unittest.TestCase):
    def test_protocol_queue_and_routing(self) -> None:
        # The token proof's crypto is the host reference here; test_mg_token_proof
        # runs the same harness on the device's PSA implementation.
        for lc3 in (False, True):
            with self.subTest(lc3=lc3), tempfile.TemporaryDirectory(prefix="mg-gen-") as gen:
                proof_vectors_header(Path(gen))
                srcs, defs, incs = codec_build(lc3)
                out = compile_and_run(
                    self,
                    [ROOT / "tests/mg_proto_harness.c", MG / "mg_proto.c", MG / "mg_queue.c",
                     MG / "mg_route.c", MG / "mg_codec.c", MG / "mg_play.c", MG / "mg_resample.c",
                     ROOT / "tests/mg_token_proof_ref.c"],
                    defs, [Path(gen), MG, MG / "include", PROTOCOLS, *incs], vendor=srcs)
                self.assertIn("PASS mg_proto", out)

    def test_every_capture_ends_with_stop_mic(self) -> None:
        # mgcommands.h: the device sends stop_mic whenever capture ends,
        # whichever side started (or stopped) it.
        text = (MG / "mg_voice.c").read_text()
        end = text[text.index("void mg_voice_end("):]
        self.assertIn("mg_proto_live_end(mg_proto_get(), true);", end)
        self.assertNotIn("mg_proto_live_end(mg_proto_get(), false)", text)

    def test_standalone_starts_no_wifi_session_or_ota(self) -> None:
        # CONFIG_MUSE_GADGET_BLE_STANDALONE: app_run hands over to
        # run_ble_standalone() before Wi-Fi, which never returns and starts
        # none of Wi-Fi, setup advertising, the Muse session, the tunnel or OTA.
        app = (ROOT / "main/app.c").read_text()
        start = app.index("static void __attribute__((noreturn)) run_ble_standalone(void) {")
        body = app[start:app.index("\n}\n", start)]
        for call in ("wifi_mgr_init", "noise_ctrl_init", "ble_server_begin_advertising", "tunnel_netif",
                     "ota_verify_task", "net_discovery", "muse_glue_link_ready", "start_ble_setup_server"):
            self.assertNotIn(call, body)
        self.assertIn("ble_server_start(identity_ble_name(), NULL)", body)
        run = app[app.index("void app_run(void) {"):]
        self.assertLess(run.index("run_ble_standalone();"), run.index("wifi_mgr_init();"))
        glue = (ROOT / "main/muse_glue.c").read_text()
        standalone = glue[glue.index("#if CONFIG_MUSE_GADGET_BLE_STANDALONE\n    // A BLE-only gadget: no phone"):]
        standalone = standalone[:standalone.index("#else")]
        self.assertNotIn("keeper_task, \"", standalone)
        self.assertNotIn("ble_server_set_companion", standalone)
        voice = (MG / "mg_voice.c").read_text()
        self.assertIn("#if MG_STANDALONE || CONFIG_MUSE_GADGET_BLE_ROUTE_BLE_ONLY", voice)
        kconfig = (MG / "Kconfig").read_text()
        self.assertIn("config MUSE_GADGET_BLE_STANDALONE", kconfig)
        overlay = (ROOT / "devices/sdkconfig.muse-waveshare-c6-18-ble").read_text()
        self.assertIn("CONFIG_MUSE_GADGET_BLE_STANDALONE=y", overlay)

    def test_protocol_header_is_included_not_copied(self) -> None:
        # The protocol's constants live in protocols/; the component includes them.
        for path in list(MG.glob("*.[ch]")) + list((MG / "include").glob("*.h")):
            text = path.read_text()
            for uuid in ("65E6635D", "732CE4AB", "5B84AFDF", "70DF812E", "8DE9F24B"):
                self.assertNotIn(uuid, text.upper(), f"{path.name} copies a protocol UUID")
        cmake = (MG / "CMakeLists.txt").read_text()
        self.assertIn("../../../protocols", cmake)


if __name__ == "__main__":
    unittest.main()
