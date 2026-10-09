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

"""Turns mgcommands-secure-v1.json into a C header for the unit tests."""

import json
import sys


def arr(name, hexstr):
    b = bytes.fromhex(hexstr)
    body = ", ".join("0x%02x" % x for x in b)
    return "static const uint8_t %s[%d] = {%s};\n" % (name, len(b), body)


def main():
    src, out = sys.argv[1], sys.argv[2]
    v = json.load(open(src))
    lines = ["/* Generated from %s. Do not edit. */\n" % src.split("/")[-1], "#include <stdint.h>\n"]
    for k, val in v.items():
        if k in ("description", "frames", "pairing_code"):
            continue
        lines.append(arr("vec_" + k, val))
    lines.append("#define VEC_PAIRING_CODE %d\n" % int(v["pairing_code"]))
    lines.append("struct vec_frame { const char *key; uint32_t seq; const uint8_t *pt; uint16_t pt_len; "
                 "const uint8_t *frame; uint16_t frame_len; };\n")
    for i, f in enumerate(v["frames"]):
        lines.append(arr("vec_f%d_pt" % i, f["plaintext"]))
        lines.append(arr("vec_f%d_frame" % i, f["frame"]))
    lines.append("static const struct vec_frame vec_frames[] = {\n")
    for i, f in enumerate(v["frames"]):
        lines.append('  {"%s", %d, vec_f%d_pt, sizeof(vec_f%d_pt), vec_f%d_frame, sizeof(vec_f%d_frame)},\n'
                     % (f["key"], f["seq"], i, i, i, i))
    lines.append("};\n")
    open(out, "w").write("".join(lines))


if __name__ == "__main__":
    main()
