#!/usr/bin/env python3
"""T1.4：802.15.4 发射功率旋钮的两条顺序/结构不变量。

    python3 check_tx_power_order.py                      # 顺序
    python3 check_tx_power_order.py --log-unconditional  # 日志行不在 #ifdef 里

为什么不写成 shell：这两条都是"位置"约束，用 `grep -A<n>` 猜距离会被中间那段
三十多行的注释绊倒（2026-09-11 已经栽过三次，代码本来是对的却红）。

── 两条不变量各自在防什么 ──────────────────────────────────────────

① **apply_tx_power() 必须在 esp_openthread_init() 之后**
   之前调时 802.15.4 的 PIB 还没初始化，写进去的值会被随后的 pib_init 覆盖回
   功率表末项（满功率），而且**不报任何错**。症状是"配置里写了 11 dBm，实测还是满功率"。

② **那行 ESP_LOGI 必须在 #ifdef 之外**
   关着开关时它打印的就是本平台功率表的上限 —— 这个数字在别处没有任何出口：
   esp_ieee802154_get_txpower() 回显的是 PIB 里存的请求值，一旦覆盖过就再也
   问不出原值了。把日志塞进 #ifdef = 认证要的那个数永久消失。
"""
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent / "thread_br.c"
src = SRC.read_text(encoding="utf-8")

# 剥注释：判的是代码位置，不能被解释这件事的注释带偏（CLAUDE.md 7.4）
code = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
code = re.sub(r"//[^\n]*", "", code)


def check_order() -> None:
    init = code.find("esp_openthread_init(&s_ot_platform_cfg)")
    if init < 0:
        sys.exit("❌ 找不到 esp_openthread_init() 调用点（改写法了？）")
    call = code.find("apply_tx_power()", init)
    if call < 0:
        sys.exit("❌ esp_openthread_init() 之后没有 apply_tx_power() —— "
                 "Thread 会停在功率表末项（满功率），认证口径静默失效")
    # 定义本身也含 "apply_tx_power(" 字样，确认命中的是调用而不是定义
    if "static void apply_tx_power" in code[call - 40:call]:
        sys.exit("❌ 只找到定义没找到调用")


def check_log_unconditional() -> None:
    m = re.search(r"static void apply_tx_power\(void\)\s*\{(.*?)\n\}", code, re.S)
    if not m:
        sys.exit("❌ 找不到 apply_tx_power() 定义")
    body = m.group(1)
    log = body.find('"802.15.4 TX power:')
    if log < 0:
        sys.exit("❌ apply_tx_power() 里没有那行功率日志")
    ifdef = body.find("#ifdef")
    if 0 <= ifdef < log:
        sys.exit("❌ 功率日志落在 #ifdef 里 —— 关着开关时功率表上限就再也读不到了")


if "--log-unconditional" in sys.argv:
    check_log_unconditional()
else:
    check_order()
sys.exit(0)
