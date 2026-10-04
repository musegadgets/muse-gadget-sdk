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

"""Host tests for the portable Link setup and token proof code (any OS).

Compiles src/mg_json.c, mg_psa.c, mg_setup_crypto.c, mg_setup_store.c,
mg_setup.c, mg_token_proof.c and mg_identity.c with the host C compiler
against small Zephyr shims (tests/host/shim) and the host's PSA Crypto
library (mbedTLS 3.6 or 4.x / TF-PSA-Crypto, found with pkg-config
mbedcrypto; or set PSA_CFLAGS and PSA_LIBS), with AddressSanitizer and
UBSan, and runs them:

  test_setup_crypto  vectors: esp32/tests/vectors/link_pairing_v5.json
  test_json          the flat JSON reader and writer
  test_setup         Link setup as the app drives it, storage, rollback,
                     boot recovery, timeouts, and the token proof against
                     protocols/test-vectors/mg-token-proof-v1.json

  python3 tests/host/run.py [-v]
"""

import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ZEPHYR = HERE.parents[1]
ROOT = ZEPHYR.parent
SRC = ZEPHYR / "src"


def psa_flags():
    if "PSA_CFLAGS" in os.environ or "PSA_LIBS" in os.environ:
        return shlex.split(os.environ.get("PSA_CFLAGS", "")), shlex.split(os.environ.get("PSA_LIBS", ""))
    try:
        cflags = subprocess.check_output(["pkg-config", "--cflags", "mbedcrypto"], text=True)
        libs = subprocess.check_output(["pkg-config", "--libs", "mbedcrypto"], text=True)
    except (OSError, subprocess.CalledProcessError):
        sys.exit("PSA Crypto not found: install mbedtls (brew install mbedtls / apt install "
                 "libmbedtls-dev) or set PSA_CFLAGS and PSA_LIBS")
    return shlex.split(cflags), shlex.split(libs)


def main():
    verbose = "-v" in sys.argv
    cc = shlex.split(os.environ.get("CC", "cc"))
    pc, pl = psa_flags()
    common = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-O1", "-g",
              "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    with tempfile.TemporaryDirectory(prefix="mg-host-") as tmp:
        tmp = Path(tmp)
        subprocess.check_call([sys.executable, str(HERE / "gen_vectors.py"),
                               str(ROOT / "esp32/tests/vectors/link_pairing_v5.json"),
                               str(ROOT / "protocols/test-vectors/mg-token-proof-v1.json"),
                               str(tmp / "vectors.h")])
        inc = ["-I" + str(HERE / "shim"), "-I" + str(tmp), "-I" + str(SRC),
               "-I" + str(ROOT / "protocols"), "-I" + str(ROOT / "esp32/main"),
               "-DCONFIG_MG_LOG_LEVEL=3", '-DCONFIG_MG_SDK_TOKEN="mgst_host_test"']
        transcript = str(ROOT / "esp32/main/pairing_transcript.c")
        crypto = [str(SRC / "mg_setup_crypto.c"), str(SRC / "mg_psa.c")]
        tests = {
            "test_setup_crypto": [str(HERE / "test_setup_crypto.c"), transcript] + crypto,
            "test_json": [str(HERE / "test_json.c"), str(SRC / "mg_json.c")],
            "test_setup": [str(HERE / "test_setup.c"), str(HERE / "host_zephyr.c"),
                           str(SRC / "mg_setup.c"), str(SRC / "mg_setup_store.c"),
                           str(SRC / "mg_token_proof.c"), str(SRC / "mg_json.c"),
                           str(SRC / "mg_identity.c"), transcript] + crypto,
        }
        failed = 0
        for name, srcs in tests.items():
            exe = tmp / name
            build = subprocess.run(cc + common + inc + pc + srcs + pl + ["-o", str(exe)],
                                   capture_output=True, text=True)
            if build.returncode:
                print(f"{name}: BUILD FAILED\n{build.stdout}{build.stderr}")
                failed += 1
                continue
            env = dict(os.environ, HOST_VERBOSE="1") if verbose else os.environ
            run = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
            out = (run.stdout + run.stderr).strip()
            last = out.splitlines()[-1] if out else ""
            print(f"{name}: {'ok' if run.returncode == 0 else 'FAILED'} ({last})")
            if run.returncode or verbose:
                print(out)
            failed += run.returncode != 0
        sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
