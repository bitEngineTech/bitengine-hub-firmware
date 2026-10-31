#!/usr/bin/env python3
"""「这次是 OTA 后第一次启动」的判定必须取在 mark_valid **之前**。

esp_ota_get_state_partition() 返回 PENDING_VERIFY 是唯一的判据，而
esp_ota_mark_app_valid_cancel_rollback() 干的事正是把它改成 VALID。
顺序写反不会编译报错、也不会崩 —— 只是那个条件**永远为假**，升级完成通知
从此再也不发，而且没有任何症状可循。

顺序类的约束 grep 表达不出来，所以单独成文件比较。
"""
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent / ".." / ".." / "main" / "main.c"
whole = SRC.read_text(encoding="utf-8")

# 直连型（docs/15 设计 2.6）没网时把确认延后给 direct_verify_task：那里也必须先 mark valid 再发通知。
# 它定义在 app_main 前面，所以下面的主流程判据只看 app_main 的函数体，不被它绊倒（2026-10-21）。
dv = whole.find("static void direct_verify_task(")
if dv >= 0:
    body = whole[dv: whole.find("\n}\n", dv)]
    m, p = body.find("esp_ota_mark_app_valid_cancel_rollback"), body.find("WEBUI_EVENT_OTA_VERIFIED")
    if m < 0 or p < 0 or p < m:
        sys.exit("❌ direct_verify_task 里要先 esp_ota_mark_app_valid_cancel_rollback 再发 OTA_VERIFIED")
    # 网可能在 webhook_init() 之前就到：任务得等 app_main 放行（订阅已注册）再发，否则通知无人接收（T2.7）
    wm = re.search(r"xEventGroupWaitBits\(\s*s_net_events\s*,\s*BOOT_SUBSCRIBERS_READY_BIT\b", body)
    if not wm or wm.start() > p:
        sys.exit("❌ direct_verify_task 发 OTA_VERIFIED 前没等 BOOT_SUBSCRIBERS_READY_BIT —— 网早到时通知会丢")
    amb = whole[whole.find("void app_main(void)"):]
    wi, sb = amb.find("webhook_init();"), amb.find("xEventGroupSetBits(s_net_events, BOOT_SUBSCRIBERS_READY_BIT)")
    if wi < 0 or sb < 0 or sb < wi:
        sys.exit("❌ app_main 要在 webhook_init() 之后才置 BOOT_SUBSCRIBERS_READY_BIT")

am = whole.find("void app_main(void)")
if am < 0:
    sys.exit("❌ main.c 里找不到 app_main —— 判据要跟着改")
s = whole[am:]

probe = s.find("esp_ota_get_state_partition")
mark = s.find("esp_ota_mark_app_valid_cancel_rollback")
if probe < 0:
    sys.exit("❌ main.c 里没有 esp_ota_get_state_partition —— 首启判定没了？")
if mark < 0:
    sys.exit("❌ main.c 里没有 esp_ota_mark_app_valid_cancel_rollback")
if probe > mark:
    sys.exit("❌ 首启判定写在了 mark_valid 之后 —— PENDING_VERIFY 已被清掉，"
             "条件永远为假，升级完成通知再也不会发")

post = s.find("WEBUI_EVENT_OTA_VERIFIED")
if post < 0:
    sys.exit("❌ main.c 不再 post WEBUI_EVENT_OTA_VERIFIED")
if post < mark:
    sys.exit("❌ OTA_VERIFIED 在 mark_valid 之前就发了 —— 那时回滚窗口还没关，"
             "通知可能发给一个马上要被回滚的镜像")
sys.exit(0)
