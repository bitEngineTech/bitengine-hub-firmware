"""noise_core 互通测试 —— 本仓库自加（不是上游文件），说明见 noise_interop_harness.cpp 文件头。

固件的 InitiatorHandshake + Transport（C++，带 PSA 后端编成主机程序）对 Linux SDK 的 NoiseXXResponder
（linux_sdk/noise_xx.py，上游原样，Python + cryptography）。两边不共享一行代码，所以协议名、HKDF 链、mix 顺序、
nonce 字节序、收发方向任何一处走样都会在这里打不开或对不上。

布局同上游测试：run.sh 把本文件、noise_xx.py、harness 放进临时目录的 tests/，组件在 components/noise_core。
"""
from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import noise_xx  # noqa: E402  (Linux SDK 原样)

ROOT = HERE.parent
COMPONENT = ROOT / "components" / "noise_core"
SRCS = ["ClientSession", "InitiatorHandshake", "PsaCryptoBackend", "ServiceCodec", "Status", "Transport",
        "TransportFrameCodec"]

MSG2_PAYLOAD = b"responder-msg2-payload"
MSG3_PAYLOAD = b"bitengine-msg3-payload"   # 与 harness 的 kMsg3Payload 一致
MESSAGES = 4                                # 与 harness 的 kMessages 一致
HIGH_NONCE = 0x0001020304050607             # 与 harness 的 kHighNonce 一致


def c2s_plaintext(i: int) -> bytes:        # 与 harness 的 C2sPlaintext 一致
    return f"c2s-{i}-".encode() + bytes([ord("a") + i]) * (i * 37)


def s2c_plaintext(i: int) -> bytes:
    return f"s2c-{i}-".encode() + bytes([0x30 + i]) * (i * 53 + 1)


class Peer:
    def __init__(self, binary: Path) -> None:
        self.p = subprocess.Popen([str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def get(self, tag: str) -> bytes:
        line = self.p.stdout.readline().strip()
        head, _, hexpart = line.partition(" ")
        if head != tag:
            raise AssertionError(f"expected {tag!r} from harness, got {line!r}")
        return bytes.fromhex(hexpart)

    def line(self) -> str:
        return self.p.stdout.readline().strip()

    def put(self, tag: str, data: bytes) -> None:
        self.p.stdin.write(f"{tag} {data.hex()}\n")
        self.p.stdin.flush()

    def finish(self) -> int:
        try:
            self.p.stdin.close()
        except BrokenPipeError:
            pass
        rc = self.p.wait(timeout=10)
        self.p.stdout.close()
        return rc


class NoiseInteropTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cxx = shlex.split(os.environ.get("CXX", "c++"))
        if not cxx or shutil.which(cxx[0]) is None:
            raise AssertionError("C++ compiler not available")   # 不跳过：缺工具链就该红
        cls._tmp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls._tmp.name) / "noise_interop_harness"
        cmd = [*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fno-exceptions", "-fno-rtti", "-g", "-O1",
               "-DNOISE_BUILDING_TESTS", "-I", str(COMPONENT / "include"), str(HERE / "noise_interop_harness.cpp"),
               *(str(COMPONENT / "src" / f"{s}.cpp") for s in SRCS), "-pthread", "-lmbedcrypto", "-o", str(cls.binary)]
        proc = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True)
        if proc.returncode != 0:
            raise AssertionError("harness build failed:\n" + proc.stdout + proc.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        cls._tmp.cleanup()

    def _handshake(self, peer: Peer):
        """走完三条握手消息并核对两边的结果；返回响应方的 (send, recv) CipherState。"""
        responder = noise_xx.NoiseXXResponder(payload=MSG2_PAYLOAD)
        responder.initialize()
        m1 = peer.get("M1")
        self.assertEqual(len(m1), 32)
        m2 = responder.read_message1_and_write_message2(m1)
        responder_static = responder._s.public_key_bytes
        peer.put("M2", m2)
        self.assertEqual(peer.get("PX"), MSG2_PAYLOAD)
        m3 = peer.get("M3")
        hh = peer.get("HH")
        # read_message3 不返回发起方载荷：包一层 decrypt_and_hash 把明文接出来（第一次是发起方静态公钥，第二次是载荷）
        seen = []
        orig = responder._ss.decrypt_and_hash
        responder._ss.decrypt_and_hash = lambda c: seen.append(orig(c)) or seen[-1]
        responder.read_message3(m3)
        self.assertEqual(len(seen), 2)
        self.assertEqual(len(seen[0]), 32)
        self.assertEqual(seen[1], MSG3_PAYLOAD)
        self.assertEqual(hh, responder.handshake_hash())
        self.assertNotEqual(hh, bytes(32))
        self.assertEqual(peer.get("RS"), responder_static)
        return responder.split()

    def test_handshake_and_transport_interop(self) -> None:
        peer = Peer(self.binary)
        send_cs, recv_cs = self._handshake(peer)
        for i in range(MESSAGES):
            self.assertEqual(recv_cs.decrypt_with_ad(b"", peer.get("C2S")), c2s_plaintext(i))
        for i in range(MESSAGES):
            pt = s2c_plaintext(i)
            peer.put("S2C", send_cs.encrypt_with_ad(b"", pt))
            self.assertEqual(peer.get("PT"), pt)
        # 计数 0..3 只用到 nonce 的最低字节；跳到高计数钉住 4..11 字节的大端顺序
        recv_cs._nonce = HIGH_NONCE
        send_cs._nonce = HIGH_NONCE
        self.assertEqual(recv_cs.decrypt_with_ad(b"", peer.get("C2S")), c2s_plaintext(MESSAGES))
        pt = s2c_plaintext(MESSAGES)
        peer.put("S2C", send_cs.encrypt_with_ad(b"", pt))
        self.assertEqual(peer.get("PT"), pt)
        self.assertEqual(peer.finish(), 0)

    def test_tampered_msg2_is_rejected(self) -> None:
        peer = Peer(self.binary)
        responder = noise_xx.NoiseXXResponder(payload=MSG2_PAYLOAD)
        responder.initialize()
        m2 = bytearray(responder.read_message1_and_write_message2(peer.get("M1")))
        m2[40] ^= 0x01                      # 加密的响应方静态公钥（字节 32..79）里翻一位
        peer.put("M2", bytes(m2))
        self.assertEqual(peer.line(), "ERR m2")
        self.assertEqual(peer.finish(), 3)

    def test_tampered_transport_frame_is_rejected(self) -> None:
        peer = Peer(self.binary)
        send_cs, _ = self._handshake(peer)
        for _ in range(MESSAGES):
            peer.get("C2S")
        ct = bytearray(send_cs.encrypt_with_ad(b"", s2c_plaintext(0)))
        ct[-1] ^= 0x80                      # 改标签
        peer.put("S2C", bytes(ct))
        self.assertEqual(peer.line(), "ERR open")
        self.assertEqual(peer.finish(), 4)


if __name__ == "__main__":
    unittest.main()
