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

"""musegadgets BLE session security (protocols/mgcommands-secure.h) with real
crypto: the firmware's primitives (mg_crypto.c) and the protocol state machine
(mg_proto.c) against protocols/test-vectors/mgcommands-secure-v1.json, with
the token proof (mg_token_proof.c) on Encrypted Control after authentication.

Needs the Mbed TLS sources ESP-IDF ships (TF-PSA-Crypto), like
test_link_pairing_handshake:
  IDF_PATH=/path/to/esp-idf python3 -m unittest tests.test_mg_secure
or MBEDTLS_SOURCE_DIR=<Mbed TLS project root containing tf-psa-crypto/>.
"""

from __future__ import annotations

import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_mg_ble import MG, PROTOCOLS, ROOT, codec_build, compile_and_run, proof_vectors_header  # noqa: E402

VECTORS = PROTOCOLS / "test-vectors/mgcommands-secure-v1.json"


def vectors_header(path: Path) -> None:
    v = json.loads(VECTORS.read_text())
    lines = ["#pragma once", "/* Generated from protocols/test-vectors/mgcommands-secure-v1.json. */"]
    for key, value in v.items():
        if isinstance(value, str) and key != "description" and key != "pairing_code":
            lines.append(f'#define V_{key.upper()} "{value}"')
    lines.append(f"#define V_PAIRING_CODE {int(v['pairing_code'])}u")
    frames = []
    for i, f in enumerate(v["frames"]):
        lines.append(f'#define V_F{i}_PT "{f["plaintext"]}"')
        lines.append(f'#define V_F{i}_FRAME "{f["frame"]}"')
        frames.append(f'{{ "{f["key"]}", "{f["plaintext"]}", "{f["frame"]}", {f["seq"]}u }}')
    lines.append("#define V_FRAMES { " + ", ".join(frames) + " }")
    path.write_text("\n".join(lines) + "\n")


class MgSecureTest(unittest.TestCase):
    def test_session_security_against_vectors(self) -> None:
        idf_path = os.environ.get("IDF_PATH")
        crypto_path = os.environ.get("MBEDTLS_SOURCE_DIR")
        if crypto_path:
            mbedtls = Path(crypto_path).resolve()
        elif idf_path:
            mbedtls = Path(idf_path).resolve() / "components/mbedtls/mbedtls"
        else:
            self.skipTest("Set IDF_PATH or MBEDTLS_SOURCE_DIR for the session security tests")
        crypto = mbedtls / "tf-psa-crypto"
        self.assertTrue((crypto / "CMakeLists.txt").is_file(), "No tf-psa-crypto in the Mbed TLS sources")
        cc = shlex.split(os.environ.get("CC", "cc"))
        cmake = shutil.which("cmake")
        self.assertTrue(cmake, "CMake is required")

        with tempfile.TemporaryDirectory(prefix="mg-secure-") as directory:
            temp = Path(directory)
            compat = temp / "compat"
            (compat / "mbedtls").mkdir(parents=True)
            # IDF relocated these public wrappers while keeping the upstream
            # APIs; map them to the software headers, as the pairing test does.
            for name in ("bignum", "ecp"):
                (compat / "mbedtls" / f"{name}.h").write_text(
                    '#pragma once\n#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS\n'
                    f'#include "mbedtls/private/{name}.h"\n')
            (compat / "sdkconfig.h").write_text("")
            build = temp / "crypto-build"
            for cmd in (
                [cmake, "-S", str(mbedtls), "-B", str(build), "-DENABLE_TESTING=OFF", "-DENABLE_PROGRAMS=OFF",
                 "-DGEN_FILES=OFF", "-DMBEDTLS_FATAL_WARNINGS=OFF", "-DDISABLE_PACKAGE_CONFIG_AND_INSTALL=ON",
                 "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_C_COMPILER={cc[0]}",
                 "-DCMAKE_C_FLAGS=-I" + shlex.quote(str(compat))],
                [cmake, "--build", str(build), "--target", "tfpsacrypto", "-j", "4"],
            ):
                r = subprocess.run(cmd, cwd=temp, text=True, capture_output=True)
                self.assertEqual(r.returncode, 0, r.stdout[-8000:] + r.stderr[-8000:])
            built = build / "tf-psa-crypto"
            libs = [built / "core/libtfpsacrypto.a", built / "drivers/builtin/libbuiltin.a",
                    built / "drivers/everest/libeverest.a", built / "drivers/p256-m/libp256m.a"]
            libs = [str(p) for p in libs if p.exists()]
            if sys.platform.startswith("linux"):
                libs = ["-Wl,--start-group", *libs, "-Wl,--end-group"]
            gen = temp / "gen"
            gen.mkdir()
            vectors_header(gen / "mg_vectors.h")
            proof_vectors_header(gen)
            srcs, defs, incs = codec_build(True)
            out = compile_and_run(
                self,
                [ROOT / "tests/mg_proto_harness.c", MG / "mg_proto.c", MG / "mg_queue.c", MG / "mg_route.c",
                 MG / "mg_codec.c", MG / "mg_crypto.c", MG / "mg_token_proof.c", MG / "mg_play.c",
                 MG / "mg_resample.c"],
                ["MG_WITH_SECURE=1", *defs],
                [gen, MG, MG / "include", PROTOCOLS, crypto / "include", crypto / "drivers/builtin/include",
                 built / "include", *incs],
                extra=libs, vendor=srcs)
            self.assertIn("PASS mg_proto secure", out)


if __name__ == "__main__":
    unittest.main()
