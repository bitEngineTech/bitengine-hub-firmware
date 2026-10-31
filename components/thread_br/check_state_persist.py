#!/usr/bin/env python3
"""PUT /node/state 的 enable 和 disable **两个方向**都必须落盘。

只落 disable 的话，用户再点 Enable 又会变成"重启后消失"，只是方向反过来，
同样难查。判据：落盘那段代码不能只在某一个分支里 —— 它应该在 enable/disable
的分支合流之后、用同一个变量写。
"""
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent / ".." / "webui" / "otbr_api.c"
s = SRC.read_text(encoding="utf-8")

m = re.search(r"static esp_err_t handle_state_put\(httpd_req_t \*req\)\s*\{(.*?)\n\}", s, re.S)
if not m:
    sys.exit("❌ 找不到 handle_state_put()")
body = m.group(1)

if "config_mgr_set_thread" not in body:
    sys.exit("❌ handle_state_put() 不落盘 —— 改动活不过重启")

# 落盘必须发生在 if(enable)/else 那个分支结构**之后**：在分支里就只覆盖一个方向
branch = body.find("if (enable) {")
save = body.find("config_mgr_set_thread")
if branch < 0:
    sys.exit("❌ 找不到 enable/disable 分支（改写法了？）")
if save < branch:
    sys.exit("❌ 落盘发生在分支之前，覆盖不到两个方向")

# 落盘那段必须用 enable 变量赋值，而不是写死 true/false
seg = body[save - 300:save + 200]
if "tc.enabled = enable" not in seg:
    sys.exit("❌ 落盘时没有用 enable 变量赋值 —— 可能写死了某一个方向")

# 落盘失败必须报出来。静默失败的表现是"我明明关掉了，重启又回来了"，
# 和修之前一模一样 —— 等于白改。
if "state_applied_but_not_saved" not in body:
    sys.exit("❌ 落盘失败没有报错 —— 静默失败的症状和修之前一模一样")
sys.exit(0)
