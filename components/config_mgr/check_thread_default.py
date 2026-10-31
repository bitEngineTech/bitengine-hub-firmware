#!/usr/bin/env python3
"""THREAD_CHANNEL_DEFAULT 必须真的离 Zigbee 的常见默认信道足够远。

单独成文件而不是塞进 VERIFY.sh：正则经 bash -> python 两层转义总会错一次
（device_identity 那边已经栽过）。

Z2M 默认 ch11、ZHA 默认 ch15。两者的间隔都必须 > COEX_TOO_CLOSE_MAX，
否则出厂就把 Thread 和 Zigbee 撞在一起 —— 而这正是不写死数字的理由。
"""
import re
import sys
from pathlib import Path

D = Path(__file__).resolve().parent
hdr = (D / "include" / "config_mgr.h").read_text(encoding="utf-8")
thr = (D / ".." / "thread_br" / "include" / "thread_br.h").read_text(encoding="utf-8")

m = re.search(r"define\s+THREAD_CHANNEL_DEFAULT\s+(\d+)", hdr)
n = re.search(r"define\s+COEX_TOO_CLOSE_MAX\s+(\d+)", thr)
if not m or not n:
    sys.exit("❌ 找不到 THREAD_CHANNEL_DEFAULT 或 COEX_TOO_CLOSE_MAX")

default, limit = int(m.group(1)), int(n.group(1))
bad = [f"ch{z}" for z in (11, 15) if abs(default - z) <= limit]
if not (11 <= default <= 26):
    bad.append(f"{default} 不在 11-26 内")
if bad:
    sys.exit(f"❌ THREAD_CHANNEL_DEFAULT={default} 与 {', '.join(bad)} 距离 <= {limit}")
sys.exit(0)
