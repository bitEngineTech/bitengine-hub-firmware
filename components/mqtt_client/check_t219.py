#!/usr/bin/env python3
"""T2.19：网络切换时 MQTT 必须"按新网口重建 + 连上补发"，而不是在断线瞬间直接发。

2026-09-13 真板三次拔线验出：客户端绑在建立时的活动网口上（T3.8），切换后只会在
没链路的口上重连；而 FAILOVER/RECOVERED 发出时 MQTT 必然断着，直接 publish 见
!s_connected 就丢 —— network_recovered 一次都没到过。

看的是函数体内容，所以和 check_restart.py 一样单独成文件；先剥掉注释再匹配，
免得被"解释为什么不能直接 publish"的注释绊倒（CLAUDE.md 7.4）。
"""
import re, sys
from pathlib import Path

SRC = Path(__file__).resolve().parent / "bitengine_mqtt.c"
text = SRC.read_text(encoding="utf-8")
code = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
code = re.sub(r"//[^\n]*", "", code)

def body(name):
    m = re.search(r"\b%s\([^)]*\)\s*\{(.*?)\n\}" % re.escape(name), code, re.S)
    if not m: sys.exit("❌ 找不到 %s()" % name)
    return m.group(1)

fails = []
def check(label, ok):
    print(("✅ " if ok else "❌ ") + label)
    if not ok: fails.append(label)

# 1. 切换分支：置待发位 + 置重启标志，且不直接 publish
h = body("system_event_handler")
m = re.search(r"NETWORK_EVENT_FAILOVER.*?NETWORK_EVENT_RECOVERED[^{]*\{(.*?)\n    \}", h, re.S)
branch = m.group(1) if m else ""
check("FAILOVER/RECOVERED 分支存在", bool(m))
check("分支置待发位 atomic_fetch_or(&s_pending_net_events", "atomic_fetch_or(&s_pending_net_events" in branch)
check("分支置 s_restart_requested = true（按新网口重建）", "s_restart_requested = true" in branch)
check("分支不直接 publish（断线瞬间必丢）", "publish" not in branch)

# 2. 连上补发
c = body("mqtt_event_handler")
conn = c.split("MQTT_EVENT_CONNECTED")[1].split("break;")[0] if "MQTT_EVENT_CONNECTED" in c else ""
check("CONNECTED 里调用 flush_pending_net_events()", "flush_pending_net_events()" in conn)
f = body("flush_pending_net_events")
ex = f.find("atomic_exchange(&s_pending_net_events, 0)")
pub = f.find("esp_mqtt_client_publish")
check("flush 先取走清零再发（exchange 在 publish 之前）", ex >= 0 and pub > ex)
check("flush 发 event/network_failover", "event/network_failover" in f)
check("flush 发 event/network_recovered", "event/network_recovered" in f)
check("两条事件 QoS 1（至少送达一次）", f.count(", 0, 1, 0)") == 2)

# 3. 关着时不攒旧闻
t = body("mqtt_task")
dis = re.search(r"if \(!cfg->enabled\) \{(.*?)\n        \}", t, re.S)
check("MQTT 关着时清待发位", bool(dis) and "atomic_store(&s_pending_net_events, 0)" in dis.group(1))

print("Result: %d failed" % len(fails))
sys.exit(1 if fails else 0)
