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

"""musegadgets BLE audio playback (mg_command_stream_audio) on the host: the
resampler at every supported rate, the playback ring's flow control,
prebuffer, underruns, drain and drop, and SBC, PCM and LC3 streams decoded
end to end (components/muse_gadget_ble/mg_play.c, mg_resample.c)."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_mg_ble import MG, PROTOCOLS, ROOT, codec_build, compile_and_run  # noqa: E402


class MgPlayTest(unittest.TestCase):
    def test_playback_core(self) -> None:
        for lc3 in (False, True):
            with self.subTest(lc3=lc3):
                srcs, defs, incs = codec_build(lc3)
                out = compile_and_run(
                    self, [ROOT / "tests/mg_play_harness.c", MG / "mg_play.c", MG / "mg_resample.c"], defs,
                    [MG, PROTOCOLS, *incs], vendor=srcs)
                self.assertIn("PASS mg_play", out)


if __name__ == "__main__":
    unittest.main()
