#!/usr/bin/env python3
"""NET 活动灯的两条不变量（2026-09-11）。

    python3 check_net_activity.py --callers   # 四个来源都还接着
    python3 check_net_activity.py --timing    # 节流 > 保持窗口

── 为什么需要它 ────────────────────────────────────────────────────────

`led_ctrl_notify_network_activity()` 曾经**零调用者**：渲染分支、函数体、
头文件声明三样俱全，就是没人调，于是 NET 灯那一档白闪是死代码。
T4.4 的 TODO 标题写的是「NET / Zigbee 灯活动闪」并标了已完成，实际只接了
Zigbee 那半边 —— 而留下的形状恰恰是第 4 节要消灭的那种断链。
最后是**用户肉眼发现**的（"net 上没看到闪烁"），任何自动检查都没抓到。

所以这里验的不是"代码在不在"，而是"**四个来源都还接着**"。少一个都意味着
某一类流量不再点亮 NET 灯，而那是没有任何报错、只能靠盯着灯看才发现的退化。
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from grep_code import strip_comments          # noqa: E402

FN = "led_ctrl_notify_network_activity"

# 四个来源 —— 语义是"网口上有数据进出"，这四类覆盖了本设备全部主动网络流量
SOURCES = {
    "桥接 TCP（Z2M/ZHA 的 EZSP 流量）": ROOT / "components/ezsp_bridge/tcp_server.c",
    "WebUI 的每个 HTTP 请求":          ROOT / "components/webui/webui_server.c",
    "MQTT 发布":                       ROOT / "components/mqtt_client/bitengine_mqtt.c",
    "webhook 投递":                    ROOT / "components/webhook/webhook.c",
}


def code(p: Path) -> str:
    return strip_comments(p.read_text(encoding="utf-8"), keep_strings=False)


def check_callers() -> None:
    missing = [name for name, p in SOURCES.items() if FN not in code(p)]
    if missing:
        sys.exit("❌ NET 活动灯少了这些来源（那类流量不再点亮 NET 灯，且无任何报错）：\n"
                 + "\n".join("      · " + m for m in missing))
    # 定义本身在 led_ctrl.c，不算来源；确认它确实还在
    if FN not in code(HERE / "led_ctrl.c"):
        sys.exit(f"❌ {FN}() 的定义不见了")


def check_timing() -> None:
    src = (HERE / "led_ctrl.c").read_text(encoding="utf-8")
    hold = re.search(r"#define\s+ZB_ACTIVITY_HOLD_MS\s+(\d+)", src)
    thr = re.search(r"#define\s+NET_ACTIVITY_THROTTLE_MS\s+(\d+)", src)
    if not hold or not thr:
        sys.exit("❌ 找不到 ZB_ACTIVITY_HOLD_MS / NET_ACTIVITY_THROTTLE_MS")
    hold_ms, thr_ms = int(hold.group(1)), int(thr.group(1))
    # 节流若不明显大于保持窗口，下一次置位会接在上一次的保持窗口里 → 常白，
    # "绿=在线"这个基色被彻底盖掉，看起来反而像坏了。留 2 倍余量。
    if thr_ms < hold_ms * 2:
        sys.exit(f"❌ 节流 {thr_ms}ms 不够大于保持 {hold_ms}ms —— "
                 f"连续流量下 NET 灯会常白，盖掉「绿=在线」这个基色")


if "--timing" in sys.argv:
    check_timing()
else:
    check_callers()
sys.exit(0)
