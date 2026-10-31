#!/usr/bin/env python3
"""matter_bridge_start() 的失败路径必须把 Zigbee 桥还原回去。

start() 第一步就把桥切成 BRIDGE_MODE_DISABLED。SDK 还没接进来，所以它**必然**
走到失败返回 —— 那条路径上如果不还原，用户的 Z2M/ZHA 连接就被永久丢在
DISABLED 上：一个没做成的功能顺手打断了正在用的功能。

判据：函数体里，在 return ESP_ERR_NOT_SUPPORTED 之前必须出现一次
switch_mode(s_prev_mode)。
"""
import re
import sys
from pathlib import Path

src = (Path(__file__).resolve().parent / "matter_bridge.c").read_text(encoding="utf-8")
m = re.search(r"esp_err_t matter_bridge_start\([^)]*\)\s*\{(.*?)\n\}", src, re.S)
if not m:
    sys.exit("❌ 找不到 matter_bridge_start()")
body = m.group(1)

ret = body.find("return ESP_ERR_NOT_SUPPORTED")
if ret < 0:
    sys.exit("❌ start() 不再返回 NOT_SUPPORTED —— 本检查要按真实实现重写")
if "switch_mode(s_prev_mode)" not in body[:ret]:
    sys.exit("❌ start() 的失败返回之前没有 ezsp_bridge_switch_mode(s_prev_mode) —— "
             "桥会被丢在 DISABLED 上")
sys.exit(0)
