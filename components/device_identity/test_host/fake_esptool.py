#!/usr/bin/env python3
"""假 esptool，给 run.sh 的「失败路径」段用。按 FAKE_MODE 决定哪一步失败：
    ok                —— 三步都成功（read-flash 原样返回写入内容）
    write_fail        —— write-flash 非零退出
    readback_mismatch —— read-flash 返回被翻了一字节的内容
每次调用的参数追加到 FAKE_LOG，供 run.sh 断言"哪几步真的被调了"。
read-mac 打印 EUI-64（与真板 C5 一致，含 ff:fe），顺带覆盖 normalize_mac 的折算。"""
import os, shutil, sys
args = sys.argv[1:]
mode = os.environ.get("FAKE_MODE", "ok")
with open(os.environ["FAKE_LOG"], "a") as f:
    f.write(" ".join(args) + "\n")
if "read-mac" in args:
    print("MAC: 38:44:be:ff:fe:12:34:56"); sys.exit(0)
if "write-flash" in args:
    if mode == "write_fail":
        print("A fatal error occurred: Failed to write", file=sys.stderr); sys.exit(2)
    shutil.copy(args[-1], os.environ["FAKE_LAST_WRITTEN"]); sys.exit(0)
if "read-flash" in args:
    data = bytearray(open(os.environ["FAKE_LAST_WRITTEN"], "rb").read())
    if mode == "readback_mismatch":
        data[100] ^= 0xFF
    open(args[-1], "wb").write(data); sys.exit(0)
sys.exit(3)
