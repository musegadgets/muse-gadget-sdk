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

"""The musegadgets BLE voice codecs: SBC (xplat/libsbc) and LC3
(xplat/liblc3) through the firmware's encoder wrapper, decoded back with
the vendored decoders, and the SBC stream with FFmpeg's decoder when it's
installed."""

from __future__ import annotations

import array
import math
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_mg_ble import MG, PROTOCOLS, ROOT, codec_build, compile_and_run


def best_snr(ref: array.array, out: array.array) -> float:
    best = -100.0
    for lag in range(0, 400):
        sig = err = 0.0
        for i in range(2000, min(len(ref), len(out) - lag), 3):
            d = out[i + lag] - ref[i]
            sig += ref[i] * ref[i]
            err += d * d
        best = max(best, 10 * math.log10(sig / (err + 1e-9)))
    return best


class MgCodecsTest(unittest.TestCase):
    def test_round_trip(self) -> None:
        srcs, defs, incs = codec_build(True)
        with tempfile.TemporaryDirectory(prefix="mg-codec-") as tmp:
            out = compile_and_run(
                self, [ROOT / "tests/mg_codec_harness.c", MG / "mg_codec.c"], defs,
                [MG, PROTOCOLS, *incs], vendor=srcs,
                args=[str(Path(tmp) / "voice.sbc")], cwd=Path(tmp))
            self.assertIn("PASS mg_codec", out)
            snr = {m[0]: float(m[1]) for m in re.findall(r"SNR (\w+) (-?[\d.]+)", out)}
            # Waveform SNR of a voiced signal; the perceptual numbers are in the READMEs.
            self.assertGreater(snr["sbc"], 20.0, out)
            self.assertGreater(snr["lc3"], 12.0, out)

            ffmpeg = shutil.which("ffmpeg")
            probe = subprocess.run([ffmpeg, "-hide_banner", "-decoders"], capture_output=True, text=True) \
                if ffmpeg else None
            if not probe or probe.returncode != 0 or not re.search(r"^ A\S* sbc ", probe.stdout, re.M):
                return  # no working FFmpeg with an SBC decoder here
            dec = Path(tmp) / "ffmpeg.raw"
            r = subprocess.run([ffmpeg, "-v", "error", "-y", "-f", "sbc", "-i", str(Path(tmp) / "voice.sbc"),
                                "-f", "s16le", "-ac", "1", "-ar", "16000", str(dec)],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            ref = array.array("h", (Path(tmp) / "input.raw").read_bytes())
            got = array.array("h", dec.read_bytes())
            self.assertGreater(best_snr(ref, got), 12.0, "FFmpeg decodes the SBC stream")

    def test_sbc_only_build(self) -> None:
        srcs, defs, incs = codec_build(False)
        out = compile_and_run(self, [ROOT / "tests/mg_codec_harness.c", MG / "mg_codec.c"], defs,
                              [MG, PROTOCOLS, *incs], vendor=srcs)
        self.assertIn("PASS mg_codec", out)

    def test_vendored_code_is_apache(self) -> None:
        for comp in ("libsbc", "liblc3"):
            d = ROOT.parent / "xplat" / comp
            self.assertIn("Apache License", (d / "LICENSE").read_text())
            readme = (d / "README.md").read_text()
            self.assertIn("github.com/google/", readme)
            for src in list(d.rglob("*.c")) + list(d.rglob("*.h")):
                text = src.read_text(errors="replace")
                self.assertIn("Licensed under the Apache License", text[:2000], src)
                self.assertNotIn("GNU Lesser", text, src)


if __name__ == "__main__":
    unittest.main()
