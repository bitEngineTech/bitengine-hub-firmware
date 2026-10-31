#!/usr/bin/env python3
"""在**代码**里找一个模式，剥掉注释和字符串字面量再找。

    python3 tools/grep_code.py <正则> <文件>...        # 命中则 exit 0
    python3 tools/grep_code.py --absent <正则> <文件>... # 不命中则 exit 0

── 为什么需要这个 ────────────────────────────────────────────────────

VERIFY.sh 里大量反向不变量长这样：「代码里**不能**出现 X」。用裸 grep 写，
会被**解释"为什么不能有 X"的那段注释**绊倒 —— 而那段注释恰恰是好代码该有的。

2026-09-11 一天之内栽了五次，每次都是同一个形状：

  device_identity  注释解释"为什么没有 device_secret"     → "没有 device_secret" 变红
  factory_api      注释解释"为什么不用 efuse_mac_get_default" → 同上
  ota_cloud        注释解释"为什么不设 .if_name"            → 同上
  mqtt_client      注释里写 "ifr 要活到 esp_mqtt_client_init() 之后"
                                                          → init/destroy 计数对不上
  spectrum         注释解释"为什么删掉 spectrum_get_history" → 同上

每次都是打补丁式地改那一条判据（加引号、改成带实参的形式、只认赋值……），
第六次还会以新形状出现。所以把"只看代码不看注释"做成一个共用工具。

⚠️ 局限（够用即可，别当编译器）：
  · 按 C/C++ 的 // 和 /* */ 剥注释，不处理字符串里出现的 "//"
  · --strings 可另外保留字符串字面量（默认一并剥掉：JSON 模板里的键名常常
    和函数名撞车）
  · 不做预处理，#if 0 里的代码仍算代码
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def strip_comments(src: str, keep_strings: bool = False) -> str:
    """剥掉 C 注释（可选连字符串字面量一起剥）。保留换行以便行号仍对得上。"""
    out = []
    i, n = 0, len(src)
    state = "code"          # code | line_comment | block_comment | string | char
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "/":
                state = "line_comment"; i += 2; continue
            if c == "/" and nxt == "*":
                state = "block_comment"; i += 2; continue
            if c == '"':
                state = "string"
                out.append('"' if keep_strings else " ")
                i += 1; continue
            if c == "'":
                state = "char"; out.append(" "); i += 1; continue
            out.append(c); i += 1; continue
        if state == "line_comment":
            if c == "\n":
                state = "code"; out.append("\n")
            i += 1; continue
        if state == "block_comment":
            if c == "*" and nxt == "/":
                state = "code"; i += 2; continue
            out.append("\n" if c == "\n" else " ")
            i += 1; continue
        if state in ("string", "char"):
            quote = '"' if state == "string" else "'"
            if c == "\\":
                out.append(src[i:i + 2] if keep_strings else "  ")
                i += 2; continue
            if c == quote:
                state = "code"
                out.append(quote if keep_strings else " ")
                i += 1; continue
            out.append(c if keep_strings else (" " if c != "\n" else "\n"))
            i += 1; continue
    return "".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="只在代码里 grep（剥掉注释）")
    ap.add_argument("--absent", action="store_true",
                    help="反向：模式**不出现**才算通过")
    ap.add_argument("--strings", action="store_true",
                    help="保留字符串字面量（默认连字符串一起剥）")
    ap.add_argument("--quiet", "-q", action="store_true", help="不打印命中行")
    ap.add_argument("pattern")
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()

    rx = re.compile(args.pattern)
    hits = []
    for f in args.files:
        p = Path(f)
        if not p.exists():
            print(f"❌ 文件不存在: {f}", file=sys.stderr)
            return 2
        code = strip_comments(p.read_text(encoding="utf-8", errors="replace"),
                              keep_strings=args.strings)
        for ln, line in enumerate(code.splitlines(), 1):
            if rx.search(line):
                hits.append(f"{f}:{ln}: {line.strip()}")

    if not args.quiet:
        for h in hits:
            print(h)

    if args.absent:
        if hits:
            print(f"❌ 代码里仍有 {len(hits)} 处匹配 {args.pattern!r}", file=sys.stderr)
            return 1
        return 0
    return 0 if hits else 1


if __name__ == "__main__":
    sys.exit(main())
