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

"""Confirming Muse Link setup over the serial console: tools/mg_confirm.py against a
simulated console on a pty, and the firmware's side of the contract on both SDKs."""

from __future__ import annotations

import os
import re
import select
import sys
import tempfile
import threading
import time
import tty
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import mg_confirm  # noqa: E402

ZEPHYR = ROOT.parent / "zephyr"


class Board(threading.Thread):
    """A gadget's console on the far side of a pty: prints `lines`, then answers each
    ">pair.confirm" with the next of `answers` ("confirmed" or "none")."""

    def __init__(self, master: int, lines: list[str], answers: list[str]):
        super().__init__(daemon=True)
        self.master, self.answers, self.commands = master, list(answers), []
        self.stop = False
        os.write(master, "".join(f"{ln}\n" for ln in lines).encode())

    def run(self) -> None:
        buf = b""
        while not self.stop:
            if not select.select([self.master], [], [], 0.05)[0]:
                continue
            try:
                buf += os.read(self.master, 256)
            except OSError:
                return
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self.commands.append(line)
                if line == b">pair.confirm":
                    answer = self.answers.pop(0) if self.answers else "none"
                    os.write(self.master, f"I (1) noise\n@pair.confirm {answer}\n".encode())


class MgConfirmTest(unittest.TestCase):
    def setUp(self) -> None:
        self.master, slave = os.openpty()
        tty.setraw(slave)   # a board's console doesn't echo
        self.port = os.ttyname(slave)
        self.addCleanup(os.close, slave)   # keeps the pty open while the tool reopens it
        self.addCleanup(os.close, self.master)

    def board(self, lines, answers) -> Board:
        b = Board(self.master, lines, answers)
        b.start()

        def stop():
            b.stop = True
            b.join(2)

        self.addCleanup(stop)   # before the pty closes
        return b

    def test_watch_answers_the_pending_confirmation(self) -> None:
        b = self.board(["I (10) link.app: setup stage: advertising -> confirm", "@pair.pending"], ["confirmed"])
        out = []
        self.assertEqual(mg_confirm.watch(self.port, 1, 5.0, out=out.append), 0)
        self.assertEqual(b.commands, [b">pair.confirm"])
        self.assertEqual(out, ["confirmed setup (1)"])

    def test_watch_sends_nothing_without_a_pending_confirmation(self) -> None:
        b = self.board(["I (10) boot", "bench: keys d (talk down)"], [])
        self.assertEqual(mg_confirm.watch(self.port, 1, 0.6, out=lambda _: None), 1)
        self.assertEqual(b.commands, [])

    def test_watch_reports_a_confirmation_that_did_not_take(self) -> None:
        self.board(["@pair.pending"], ["none"])
        out = []
        self.assertEqual(mg_confirm.watch(self.port, 1, 1.0, out=out.append), 1)
        self.assertIn("nothing pending", out[0])

    def test_confirm_now_retries_until_the_firmware_notices(self) -> None:
        b = self.board([], ["none", "confirmed"])
        t = time.monotonic()
        self.assertTrue(mg_confirm.confirm_now(self.port, tries=5.0))
        self.assertEqual(b.commands, [b">pair.confirm", b">pair.confirm"])
        self.assertLess(time.monotonic() - t, 4.0)

    def test_confirm_now_gives_up(self) -> None:
        self.board([], ["none"] * 20)
        self.assertFalse(mg_confirm.confirm_now(self.port, tries=0.8))

    def test_log_keeps_the_console(self) -> None:
        self.board(["hello", "@pair.pending"], ["confirmed"])
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        path = Path(tmp.name) / "console.log"
        mg_confirm.watch(self.port, 1, 5.0, log=str(path), out=lambda _: None)
        text = path.read_bytes()
        self.assertIn(b"hello\n@pair.pending\n", text)
        self.assertIn(b"@pair.confirm confirmed", text)


class FirmwareContractTest(unittest.TestCase):
    """Both SDKs print the same marker and take the same line, and the command is the
    talk button's setup path, never push-to-talk."""

    def test_esp32_console(self) -> None:
        src = (ROOT / "components" / "muse" / "muse_input.c").read_text()
        body = re.search(r'if \(!strcmp\(line, "pair\.confirm"\)\) \{(.*?)\n    \}', src, re.S)
        self.assertIsNotNone(body, "muse_input.c takes >pair.confirm")
        self.assertIn("muse_link_talk_press()", body.group(1))
        self.assertNotIn("post(", body.group(1))
        self.assertIn('"@pair.confirm %s\\n", confirmed ? "confirmed" : "none"', body.group(1))
        self.assertIn('printf("@pair.pending\\n")', src)
        glue = (ROOT / "main" / "muse_glue.c").read_text()
        self.assertRegex(glue, r"op_talk_press\(void\) \{\s+return app_confirm_pairing_press\(\);")

    def test_zephyr_bench(self) -> None:
        bench = (ZEPHYR / "src" / "mg_bench.c").read_text()
        self.assertIn('strcmp(line, "pair.confirm")', bench)
        self.assertIn("mg_setup_confirm_pending() && mg_setup_button()", bench)
        self.assertIn('"@pair.confirm %s\\n", confirmed ? "confirmed" : "none"', bench)
        self.assertIn("k_work_submit_to_queue(mg_app_wq(), &confirm_work)", bench)
        setup = (ZEPHYR / "src" / "mg_setup.c").read_text()
        self.assertRegex(setup, r'#if defined\(CONFIG_MG_BENCH\)[^#]*printk\("@pair\.pending\\n"\);')


if __name__ == "__main__":
    unittest.main()
