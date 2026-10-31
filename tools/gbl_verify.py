#!/usr/bin/env python3
"""在主机上验一个 MG24 .gbl 的签名 —— 和网关固件（components/ota_mgr/gbl_image.c）同一套规则。

    python3 tools/gbl_verify.py <image.gbl> [--pub mg24/release/mg24_gbl_signing_key.pub] [--quiet]

退出码：0 = 签名合法且是官方钥匙；2 = 没有签名标签（网关只有按键解锁后才收）；
        1 = 有签名但验不过（不是官方钥匙、或文件被改过）/ 容器坏了 / 参数错。

发版前的最后一道闸：tools/make_manifest.py 在把 .gbl 写进 manifest 之前调用它，
mg24/ota_mg24.sh 上传前调用它（能省一次注定失败的上传）。规则（2026-09-14 对着
SDK 源码与 commander 实测）：ECDSA P-256 对「签名标签之前全部字节」的 SHA-256，
签名标签 id 0xF70A0AF7、64 字节裸 r||s，END 标签的 CRC-32 覆盖它之前的一切。

依赖 python3 + cryptography（ESP-IDF 的环境自带；产线机器没有就跳过这一步，
设备侧照样会验）。
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import zlib
from pathlib import Path

HDR, SIG, END = 0x03A617EB, 0xF70A0AF7, 0xFC0404FC
ROOT = Path(__file__).resolve().parent.parent
DEFAULT_PUB = ROOT / "mg24" / "release" / "mg24_gbl_signing_key.pub"


class GblError(Exception):
    pass


def parse(data: bytes):
    """返回 (signed_prefix_len, signature) ；没有签名标签时 signature 为 None。"""
    if len(data) < 28 or struct.unpack("<I", data[:4])[0] != HDR:
        raise GblError("not a GBL container (bad magic)")
    if struct.unpack("<II", data[-12:-4]) != (END, 4):
        raise GblError("missing END tag")
    if zlib.crc32(data[:-4]) != struct.unpack("<I", data[-4:])[0]:
        raise GblError("CRC32 mismatch")
    off, sig_off, sig, prev = 0, None, None, None
    while off + 8 <= len(data):
        tid, tl = struct.unpack("<II", data[off:off + 8])
        if tl > len(data) - off - 8:
            raise GblError("tag length overruns image")
        if tid == END:
            if off + 8 + tl != len(data):
                raise GblError("data after END tag")
            if sig is not None and prev != SIG:
                raise GblError("data between signature and END tag")
            return sig_off, sig
        if tid == SIG:
            if sig is not None:
                raise GblError("more than one signature tag")
            if tl != 64:
                raise GblError("signature tag length is not 64")
            sig_off, sig = off, data[off + 8:off + 72]
        prev = tid
        off += 8 + tl
    raise GblError("no END tag")


def verify(path: Path, pub: Path) -> int:
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature

    data = path.read_bytes()
    sig_off, sig = parse(data)
    if sig is None:
        return 2
    key = serialization.load_pem_public_key(pub.read_bytes())
    if not isinstance(key, ec.EllipticCurvePublicKey) or key.curve.name != "secp256r1":
        raise GblError(f"{pub} is not a secp256r1 public key")
    der = encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big"))
    try:
        key.verify(der, data[:sig_off], ec.ECDSA(hashes.SHA256()))
    except InvalidSignature:
        return 1
    # sanity: the digest the device will compute
    hashlib.sha256(data[:sig_off]).hexdigest()
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("gbl")
    ap.add_argument("--pub", default=str(DEFAULT_PUB))
    ap.add_argument("--quiet", "-q", action="store_true")
    a = ap.parse_args(argv[1:])
    say = (lambda *x: None) if a.quiet else print
    try:
        rc = verify(Path(a.gbl), Path(a.pub))
    except GblError as e:
        say(f"❌ {a.gbl}: {e}")
        return 1
    except FileNotFoundError as e:
        say(f"❌ {e}")
        return 1
    except ImportError:
        say("❌ 需要 python 包 cryptography（ESP-IDF 环境自带：source export.sh）")
        return 1
    if rc == 0:
        say(f"✅ {a.gbl}: 签名合法（{Path(a.pub).name}）")
    elif rc == 2:
        say(f"⚠️ {a.gbl}: 没有签名标签 —— 网关只有按键解锁后才会接受")
    else:
        say(f"❌ {a.gbl}: 签名验不过 —— 不是官方钥匙签的，或文件被改过")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
