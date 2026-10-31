#!/usr/bin/env python3
"""对照 json_escape_test 写出的随机用例（T9.24 / T9.32 / T9.33）。

每行：<模式> <dst_size> <输入十六进制> <输出十六进制>（空串写 '-'）。
模式：e = webui_json_escape（转义）、s = json_sanitize_copy（剥离）、
      u = utf8_repair_copy（只修 UTF-8）、v = utf8_valid（输出列是 1 / 0，dst_size 无意义）。

参考实现完全独立于 C 代码：用 Python 的 UTF-8 解码器切分输入 —— 自定义错误处理器
拿到的 (start, end) 与 'replace' 一致，即 Unicode 的"最大子部分"，一段换一个
U+FFFD。再按各模式的规则逐单元拼出期望字节，按 dst_size 截到"放得下的最长整单元
前缀"。C 的输出必须与之逐字节相等，且必须能被严格 UTF-8 解码；e / s 两种还必须能
直接放进 JSON 字符串被 json.loads（u 的输出由 cJSON 负责转义，不要求这一条）。
"""
import codecs
import json
import sys

# 输入里不会有 NUL（C 字符串），所以拿 NUL 当"这里是一段非法字节"的记号
codecs.register_error("mark_bad", lambda e: ("\x00", e.end))

ESC = {'"': b'\\"', "\\": b"\\\\", "\n": b"\\n", "\r": b"\\r", "\t": b"\\t"}


FFFD = "\ufffd".encode("utf-8")


def units(raw: bytes, mode: str):
    for ch in raw.decode("utf-8", "mark_bad"):
        if mode == "e":
            if ch == "\x00":
                yield b"\\ufffd"
            elif ch in ESC:
                yield ESC[ch]
            elif ord(ch) < 0x20:
                yield b"\\u%04x" % ord(ch)
            else:
                yield ch.encode("utf-8")
        else:
            if ch == "\x00":
                yield FFFD
            elif mode == "s" and (ch in '"\\' or ord(ch) < 0x20):
                continue                        # 剥掉，不占空间
            else:
                yield ch.encode("utf-8")


def expected(raw: bytes, dst_size: int, mode: str) -> bytes:
    out = b""
    for u in units(raw, mode):
        if len(out) + len(u) >= dst_size:      # 留一个字节给 NUL；装不下整段就停
            break
        out += u
    return out


def main(path: str) -> int:
    bad = total = full = 0
    per_mode = {}
    with open(path) as f:
        for ln, line in enumerate(f, 1):
            mode, ds_s, in_h, out_h = line.split()
            ds = int(ds_s)
            per_mode[mode] = per_mode.get(mode, 0) + 1
            raw = b"" if in_h == "-" else bytes.fromhex(in_h)
            total += 1
            if mode == "v":                 # 判据：严格解码能过 ⇔ 合法
                try:
                    raw.decode("utf-8"); want_v = "1"
                except UnicodeDecodeError:
                    want_v = "0"
                if out_h != want_v:
                    bad += 1
                    if bad <= 10:
                        print(f"❌ 第 {ln} 行 [v] 输入 {raw!r}：期望 {want_v}，实得 {out_h}")
                continue
            got = b"" if out_h == "-" else bytes.fromhex(out_h)
            want = expected(raw, ds, mode)
            full += ds == len(raw) * 6 + 1
            err = None
            if got != want:
                err = f"期望 {want!r}，实得 {got!r}"
            else:
                try:
                    text = got.decode("utf-8")                    # 严格解码：非法 UTF-8 在这里就炸
                    if mode != "u":
                        json.loads('"' + text + '"')
                except (UnicodeDecodeError, ValueError) as e:
                    err = f"不是合法 UTF-8 / JSON 字符串：{e}"
            if err:
                bad += 1
                if bad <= 10:
                    print(f"❌ 第 {ln} 行 [{mode}] 输入 {raw!r} dst_size={ds}：{err}")
    if sorted(per_mode) != ["e", "s", "u", "v"]:
        print(f"❌ 对照用例缺模式：{sorted(per_mode)}（C 端没写全？）")
        return 1
    if total == 0:
        print("❌ 没有对照用例（C 端没写出文件？）")
        return 1
    if bad:
        print(f"❌ 对照 Python：{bad}/{total} 条不一致")
        return 1
    modes = " / ".join(f"{m} {n}" for m, n in sorted(per_mode.items()))
    print(f"✅ 对照 Python decode('utf-8','replace')：{total} 条逐字节一致（{modes}；其中 {full} 条不截断）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
