#!/usr/bin/env python3
"""mqtt_client_restart() 必须**只置一个标志位**，别的什么都不做。

这是 07 模块那个 CRITICAL 的防线：原缺陷是每次配置变更都重新 xTaskCreate +
重新注册 event handler，改十次配置就有十个 task，且旧的永不退出。

正确的形状是 restart 只写 s_restart_requested，真正的停/起在 mqtt_task 的循环里
做 —— 那里既不在事件循环上下文（不会自己等自己），也不会重复建 task。

单独成文件而不是塞进 VERIFY.sh：要看的是**函数体内容**，shell 里做多行匹配
既难写又难读，转义还必错一次。
"""
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent / "bitengine_mqtt.c"
m = re.search(r"esp_err_t mqtt_client_restart\(void\)\s*\{(.*?)\n\}", SRC.read_text(encoding="utf-8"), re.S)
if not m:
    sys.exit("❌ 找不到 mqtt_client_restart()（改名了？）")

body = m.group(1)
banned = {
    "xTaskCreate":                  "重新创建 task",
    "psram_task_create":            "重新创建 task",
    "esp_event_handler_instance_register": "重新注册事件处理器",
    "esp_event_handler_register":   "重新注册事件处理器",
    "esp_mqtt_client_init":         "在这里建客户端",
    "esp_mqtt_client_destroy":      "在这里销毁客户端（可能死锁）",
    "esp_mqtt_client_stop":         "在这里停客户端（可能死锁）",
    "vTaskDelay":                   "在这里阻塞",
}
hits = [f"{k}（{v}）" for k, v in banned.items() if k in body]
if hits:
    sys.exit("❌ mqtt_client_restart() 里出现了: " + "; ".join(hits))

if "s_restart_requested" not in body:
    sys.exit("❌ mqtt_client_restart() 没有置 s_restart_requested")
sys.exit(0)
