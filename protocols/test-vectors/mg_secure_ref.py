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

"""Reference session security for mgcommands-secure.h, and its test vectors.

Run it to regenerate mgcommands-secure-v1.json next to this file:

    python3 -m pip install cryptography
    python3 protocols/test-vectors/mg_secure_ref.py

Implementations on every platform check themselves against that file.
"""

import hashlib
import hmac
import json
import os
import struct

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric.x25519 import (
    X25519PrivateKey,
    X25519PublicKey,
)
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDFExpand
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

MG_SECURITY_VERSION = 1
SUITE_X25519_AES256GCM_SHA256 = 1

CMD_ENABLE_ENCRYPTION = 0x81
CMD_KEY_EXCHANGE = 0x80
CMD_AUTHENTICATE = 0x82
KX_COMMIT, KX_RESPONSE, KX_REVEAL = 0, 1, 2
AUTH_PROVE, AUTH_PROOF = 0, 1

TRANSCRIPT_LABEL = b"musegadgets ble v1"


def x25519_public(priv: bytes) -> bytes:
    return (
        X25519PrivateKey.from_private_bytes(priv)
        .public_key()
        .public_bytes(Encoding.Raw, PublicFormat.Raw)
    )


def x25519(priv: bytes, peer_pub: bytes) -> bytes:
    ss = X25519PrivateKey.from_private_bytes(priv).exchange(
        X25519PublicKey.from_public_bytes(peer_pub)
    )
    if ss == bytes(32):
        raise ValueError("all-zero shared secret")
    return ss


def hkdf_extract(salt: bytes, ikm: bytes) -> bytes:
    return hmac.new(salt, ikm, hashlib.sha256).digest()


def hkdf_expand(prk: bytes, info: bytes, length: int) -> bytes:
    return HKDFExpand(algorithm=hashes.SHA256(), length=length, info=info).derive(prk)


def m1(commitment: bytes) -> bytes:
    return (
        bytes([CMD_KEY_EXCHANGE, KX_COMMIT, MG_SECURITY_VERSION, SUITE_X25519_AES256GCM_SHA256])
        + commitment
    )


def m2(device_pub: bytes, device_nonce: bytes) -> bytes:
    return (
        bytes([CMD_KEY_EXCHANGE, KX_RESPONSE, MG_SECURITY_VERSION, SUITE_X25519_AES256GCM_SHA256])
        + device_pub
        + device_nonce
    )


def m3(client_pub: bytes, client_nonce: bytes) -> bytes:
    return bytes([CMD_KEY_EXCHANGE, KX_REVEAL]) + client_pub + client_nonce


def derive(ss: bytes, th: bytes) -> dict:
    prk = hkdf_extract(th, ss)
    code_bytes = hkdf_expand(prk, b"mg1 pairing code", 4)
    return {
        "prk": prk,
        "c2d_control": hkdf_expand(prk, b"mg1 c2d control", 32),
        "c2d_data": hkdf_expand(prk, b"mg1 c2d data", 32),
        "d2c_control": hkdf_expand(prk, b"mg1 d2c control", 32),
        "d2c_data": hkdf_expand(prk, b"mg1 d2c data", 32),
        "pairing_code_bytes": code_bytes,
        "pairing_code": "%06d" % (struct.unpack("<I", code_bytes)[0] % 1000000),
        "pairing_key": hkdf_expand(prk, b"mg1 pairing key", 32),
        "key_id": hkdf_expand(prk, b"mg1 key id", 8),
    }


def seal(key: bytes, seq: int, plaintext: bytes) -> bytes:
    header = bytes([0]) + struct.pack("<I", seq)
    nonce = struct.pack("<I", seq) + bytes(8)
    return header + AESGCM(key).encrypt(nonce, plaintext, header)


def open_frame(key: bytes, last_seq: int, frame: bytes) -> tuple:
    if len(frame) < 21 or frame[0] != 0:
        raise ValueError("bad frame")
    seq = struct.unpack("<I", frame[1:5])[0]
    if last_seq is not None and seq <= last_seq:
        raise ValueError("replayed seq")
    nonce = struct.pack("<I", seq) + bytes(8)
    return seq, AESGCM(key).decrypt(nonce, frame[5:], frame[:5])


def auth_macs(pairing_key: bytes, th: bytes) -> tuple:
    client = hmac.new(pairing_key, b"mg1 client auth" + th, hashlib.sha256).digest()
    device = hmac.new(pairing_key, b"mg1 device auth" + th, hashlib.sha256).digest()
    return client, device


def vectors() -> dict:
    client_priv = bytes(range(0x00, 0x20))
    device_priv = bytes(range(0x40, 0x60))
    client_nonce = bytes(range(0xA0, 0xB0))
    device_nonce = bytes(range(0xC0, 0xD0))
    client_pub = x25519_public(client_priv)
    device_pub = x25519_public(device_priv)

    commitment = hashlib.sha256(client_pub + client_nonce).digest()
    msg1, msg2, msg3 = m1(commitment), m2(device_pub, device_nonce), m3(client_pub, client_nonce)
    th = hashlib.sha256(TRANSCRIPT_LABEL + msg1 + msg2 + msg3).digest()
    ss = x25519(client_priv, device_pub)
    assert ss == x25519(device_priv, client_pub)
    k = derive(ss, th)
    client_mac, device_mac = auth_macs(k["pairing_key"], th)

    prove = bytes([CMD_AUTHENTICATE, AUTH_PROVE]) + k["key_id"] + client_mac
    proof = bytes([CMD_AUTHENTICATE, AUTH_PROOF]) + device_mac
    audio_chunk = bytes([0x9C, 0x71, 0x1A, 0x00]) + bytes(range(40))
    frames = [
        ("c2d_control", 0, bytes([CMD_ENABLE_ENCRYPTION])),
        ("d2c_control", 0, bytes([CMD_ENABLE_ENCRYPTION])),
        ("c2d_control", 1, prove),
        ("d2c_control", 1, proof),
        ("d2c_control", 2, bytes([0x06])),
        ("d2c_data", 0, audio_chunk),
        ("d2c_data", 1, audio_chunk),
    ]

    h = bytes.hex
    return {
        "description": "mgcommands-secure.h v1, suite x25519_aes256gcm_sha256. "
        "All byte strings are lowercase hex.",
        "client_private": h(client_priv),
        "client_public": h(client_pub),
        "client_nonce": h(client_nonce),
        "device_private": h(device_priv),
        "device_public": h(device_pub),
        "device_nonce": h(device_nonce),
        "commitment": h(commitment),
        "m1": h(msg1),
        "m2": h(msg2),
        "m3": h(msg3),
        "transcript_hash": h(th),
        "shared_secret": h(ss),
        "prk": h(k["prk"]),
        "c2d_control_key": h(k["c2d_control"]),
        "c2d_data_key": h(k["c2d_data"]),
        "d2c_control_key": h(k["d2c_control"]),
        "d2c_data_key": h(k["d2c_data"]),
        "pairing_code_bytes": h(k["pairing_code_bytes"]),
        "pairing_code": k["pairing_code"],
        "pairing_key": h(k["pairing_key"]),
        "key_id": h(k["key_id"]),
        "client_mac": h(client_mac),
        "device_mac": h(device_mac),
        "frames": [
            {
                "key": name,
                "seq": seq,
                "plaintext": h(pt),
                "frame": h(seal(k[name], seq, pt)),
            }
            for name, seq, pt in frames
        ],
    }


def main() -> None:
    v = vectors()
    for f in v["frames"]:
        key = bytes.fromhex(v[f["key"] + "_key"])
        seq, pt = open_frame(key, None, bytes.fromhex(f["frame"]))
        assert seq == f["seq"] and pt.hex() == f["plaintext"]
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mgcommands-secure-v1.json")
    with open(out, "w") as fh:
        json.dump(v, fh, indent=2)
        fh.write("\n")
    print(out)


if __name__ == "__main__":
    main()
