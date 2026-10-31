#!/usr/bin/env python3
"""某个 API 是否仍是"不能用"的桩：函数体里出现过 ESP_ERR_NOT_SUPPORTED 的 return，
且**没有**任何 `return ESP_OK;`。

    python3 check_stub.py matter_bridge_start

为什么不是简单 grep：要看的是**这个函数体**，而不是整个文件。文件里别的函数
（init、stop）本来就该返回 ESP_OK，按文件 grep 分不开。
"""
import re
import sys
from pathlib import Path

if len(sys.argv) < 2:
    sys.exit("用法: check_stub.py <函数名>")
fn = sys.argv[1]
src = (Path(__file__).resolve().parent / "matter_bridge.c").read_text(encoding="utf-8")

m = re.search(rf"esp_err_t {re.escape(fn)}\([^)]*\)\s*\{{(.*?)\n\}}", src, re.S)
if not m:
    sys.exit(f"❌ 找不到 {fn}()（改名了？）")
body = m.group(1)

if "ESP_ERR_NOT_SUPPORTED" not in body:
    sys.exit(f"❌ {fn}() 不再返回 ESP_ERR_NOT_SUPPORTED —— "
             f"Matter SDK 接进来了？那要按真实实现重写本模块的 VERIFY.sh")
if re.search(r"return\s+ESP_OK\s*;", body):
    sys.exit(f"❌ {fn}() 里出现了 return ESP_OK —— 空壳不能假装成功，"
             f"调用方会以为桥起来了")
sys.exit(0)
