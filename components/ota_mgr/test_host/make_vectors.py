#!/usr/bin/env python3
"""Build GBL test vectors for gbl_test.c — no commander, no private key needed.

    python3 make_vectors.py <outdir>

The container layout mirrors what `commander gbl create --app … --sign …`
emits (checked byte-for-byte against commander 1v23p1b1971 on 2026-09-14):
header tag (version, type with bit 0x100 when signed), an application-info
tag, one PROG tag, the ECDSA-P256 signature tag (raw r||s over SHA-256 of
every byte before it), then the END tag whose payload is the CRC-32 of
everything before the CRC field.

Two throwaway keys are generated per run; only their public points are
written out (pub.xy / other.xy, 64 bytes X||Y each).
"""

import random
import struct
import sys
import zlib
from pathlib import Path

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

HDR, APPINFO, PROG, SIG, END = 0x03A617EB, 0xF40A0AF4, 0xFE0101FE, 0xF70A0AF7, 0xFC0404FC
SIGNED_FLAG = 0x100


def tag(tid: int, payload: bytes) -> bytes:
    return struct.pack("<II", tid, len(payload)) + payload


def header(signed: bool) -> bytes:
    return tag(HDR, struct.pack("<II", 0x03000000, SIGNED_FLAG if signed else 0))


def finish(prefix: bytes) -> bytes:
    head = prefix + struct.pack("<II", END, 4)
    return head + struct.pack("<I", zlib.crc32(head))


def refix_crc(img: bytes) -> bytes:
    return img[:-4] + struct.pack("<I", zlib.crc32(img[:-4]))


def raw_sig(key: ec.EllipticCurvePrivateKey, data: bytes) -> bytes:
    r, s = decode_dss_signature(key.sign(data, ec.ECDSA(hashes.SHA256())))
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def xy(key: ec.EllipticCurvePrivateKey) -> bytes:
    n = key.public_key().public_numbers()
    return n.x.to_bytes(32, "big") + n.y.to_bytes(32, "big")


def main(out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    rnd = random.Random(20260914)            # same body in every vector
    body = tag(APPINFO, bytes(28)) + tag(PROG, struct.pack("<I", 0x08006000) + bytes(rnd.getrandbits(8) for _ in range(300)))

    k1, k2 = ec.generate_private_key(ec.SECP256R1()), ec.generate_private_key(ec.SECP256R1())
    (out / "pub.xy").write_bytes(xy(k1))
    (out / "other.xy").write_bytes(xy(k2))

    signed_prefix = header(True) + body
    good = finish(signed_prefix + tag(SIG, raw_sig(k1, signed_prefix)))
    v = {
        "good.gbl":            good,
        "unsigned.gbl":        finish(header(False) + body),          # == strip(good)
        "wrongkey.gbl":        finish(signed_prefix + tag(SIG, raw_sig(k2, signed_prefix))),
        "unsigned-flagged.gbl": finish(header(True) + body),          # flag set, no tag
        "twosig.gbl":          finish(signed_prefix + tag(SIG, raw_sig(k1, signed_prefix)) * 2),
        "sig-not-last.gbl":    finish(signed_prefix + tag(SIG, raw_sig(k1, signed_prefix)) + tag(PROG, b"\x00" * 8)),
        "badsiglen.gbl":       finish(signed_prefix + tag(SIG, raw_sig(k1, signed_prefix)[:63])),
        "truncated.gbl":       good[:-1],
        "badcrc.gbl":          good[:-1] + bytes([good[-1] ^ 0x01]),
        "after-end.gbl":       good + b"\x00",
        "overrun.gbl":         refix_crc(header(True) + struct.pack("<II", PROG, 0xFFFFFFFF) + b"\x00" * 16 + struct.pack("<II", END, 4) + b"\x00" * 4),
        "badhdrlen.gbl":       refix_crc(tag(HDR, b"\x00" * 12) + body + struct.pack("<II", END, 4) + b"\x00" * 4),
    }
    # tampered: one payload byte flipped inside the PROG data, CRC made valid again
    t = bytearray(good)
    t[len(header(True)) + 8 + 28 + 8 + 4 + 10] ^= 0x55
    v["tampered.gbl"] = refix_crc(bytes(t))

    for name, data in v.items():
        (out / name).write_bytes(data)
    print(f"{len(v)} vectors in {out}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.stderr.write(__doc__)
        sys.exit(2)
    main(Path(sys.argv[1]))
