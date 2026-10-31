#!/usr/bin/env python3
"""灯的行为定义在多份文档里都有，别让它们各说各话（2026-09-12）。

    python3 check_led_docs.py

── 为什么需要它 ────────────────────────────────────────────────────────

「灯是什么行为」被写在至少五个地方：用户手册、架构文档、led_ctrl 设计文档、
产测指南、产品功能现状。2026-09-12 逐份核对时发现它们已经各自漂了：

  · docs/01 的状态图写着「Thread **活动时青色闪烁**」—— 这个功能**从来不存在**，
    连渲染档位都没有，纯粹是文档承诺（比 NET 那次更彻底：NET 好歹有死代码）
  · docs/01 还写着 PWR LED / ZB LED 两颗固定灯 —— 早已取消
  · docs/12 §3.9 让前端做「5 种状态颜色选择器」—— 那些字段是故意不接的，
    做了只会存一份永不生效的配置
  · docs/00 功能清单里「备用固定 LED × 2 ... RGB LED 故障时的后备」—— 不存在
  · 产测指南写「14 个检查项」「白名单三项」—— 当天已变成 17 项、五项

这个脚本不去比对自然语言（那不可行），只钉住**最容易被写错、且错了会误导人**
的几条硬事实。判据全部以代码为准，代码变了就该回去改文档。
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
src = (HERE / "led_ctrl.c").read_text(encoding="utf-8")

fails = []


def doc(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


# ── 1. Thread 灯没有活动档：代码里不能有，文档里不能承诺 ────────────────
# 判据取自渲染函数：zigbee / net 各有一个 *_activity_until 分支，thread 没有。
if re.search(r"s_thread_activity", src):
    fails.append("代码里出现了 s_thread_activity —— 若真做了 Thread 活动闪，"
                 "请回头更新 docs/01 状态图、docs/07、Hub使用手册第三节与本脚本")
else:
    # 代码里没有 → 文档里就不许说有
    for rel in ("docs/01_software_architecture_v1.md",
                "docs/guide/Hub使用手册.md"):
        t = doc(rel)
        # 只在"未被订正块包住"的地方算数：订正块本身要引用这句话来说明它是错的
        bad = re.search(r"Thread\s*活动时[^\n]*闪", t)
        if bad and "从来不存在" not in t and "没有活动闪" not in t:
            fails.append(f"{rel} 仍承诺「Thread 活动闪」，而代码里没有这个功能")

# ── 2. 两颗活动灯的来源数，文档若写了数字就得对 ──────────────────────────
NET_SOURCES = 4      # 桥接 TCP / 每个 HTTP 请求 / MQTT 发布 / webhook 投递
sources = 0
for rel in ("components/ezsp_bridge/tcp_server.c",
            "components/webui/webui_server.c",
            "components/mqtt_client/bitengine_mqtt.c",
            "components/webhook/webhook.c"):
    if "led_ctrl_notify_network_activity" in doc(rel):
        sources += 1
if sources != NET_SOURCES:
    fails.append(f"NET 活动灯的来源变成了 {sources} 个（文档按 {NET_SOURCES} 个写的）—— "
                 f"改完请同步 Hub使用手册第三节与 docs/07")

# ── 3. 固定 LED 已取消：文档不许再把它说成"后备" ────────────────────────
t = doc("docs/00_产品功能现状.md")
if re.search(r"备用固定 LED[^\n|]*\|[^\n|]*\|[^\n|]*后备", t) and "已取消" not in t:
    fails.append("docs/00 仍把「备用固定 LED」说成 RGB 故障时的后备 —— 那两颗不上件")

# ── 4. 以太网 PHY LED 是空实现，不许说夜间会关掉它 ──────────────────────
if not re.search(r"void led_ctrl_set_eth_led\(bool on\)\s*\{[^}]*\}", src, re.S):
    fails.append("led_ctrl_set_eth_led 的形状变了，第 4 条判据失效")
else:
    body = re.search(r"void led_ctrl_set_eth_led\(bool on\)\s*\{(.*?)\n\}", src, re.S).group(1)
    stripped = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    stripped = re.sub(r"//[^\n]*", "", stripped)
    if re.search(r"gpio_set_level", stripped):
        fails.append("led_ctrl_set_eth_led 开始真的驱动引脚了 —— 那条网络接的是 "
                     "RS706 的 MR，拉低 = 整板重启（X8）")

# ── 5. Muse 配对指示（直连型 + Muse，docs/15 设计第十一节）────────────────────
# 代码：s_pair_ind 的每一处都在 #if CONFIG_BITENGINE_ENABLE_MUSE_LINK 里（HA 型一个像素都不能变）；
# 文档：设计第十一节列着四种 SYS 显示，与枚举一一对应；HA 型的用户手册不许出现（这些灯 HA 型里没有，直连型手册在 M6）。
def guarded_by_muse(text: str, needle: str) -> bool:
    depth_ok = True
    stack = []
    for line in text.split("\n"):
        t = line.strip()
        if t.startswith("#if"):
            stack.append("CONFIG_BITENGINE_ENABLE_MUSE_LINK" in t)
        elif t.startswith("#else") and stack:
            stack[-1] = False
        elif t.startswith("#endif") and stack:
            stack.pop()
        elif needle in line and not t.startswith(("/*", "*", "//")):
            depth_ok = depth_ok and any(stack)
    return depth_ok

hdr = (HERE / "include" / "led_ctrl.h").read_text(encoding="utf-8")
if "s_pair_ind" in src:
    if not guarded_by_muse(src, "pair_ind") or not guarded_by_muse(hdr, "LED_PAIR_"):
        fails.append("配对指示（s_pair_ind / LED_PAIR_*）有一处不在 #if CONFIG_BITENGINE_ENABLE_MUSE_LINK 里 —— HA 型会被带动")
    vals = re.findall(r"\b(LED_PAIR_(?!NONE)[A-Z]+)\s*,", hdr)
    if sorted(vals) != ["LED_PAIR_CONFIRM", "LED_PAIR_FAILED", "LED_PAIR_SUCCESS", "LED_PAIR_WINDOW"]:
        fails.append(f"配对指示的档位变了（{vals}）—— 同步改 docs/15 设计第十一节与本脚本")
    d15 = doc("docs/15_muse_direct_design_v1.md")
    sec = d15[d15.find("## 十一、指示灯与按键"):d15.find("## 十二、")]
    for want in ("青色呼吸", "青色快闪", "绿色", "红色快闪"):
        if want not in sec:
            fails.append(f"docs/15 设计第十一节没有「{want}」—— 与代码里的配对指示对不上")
    man = doc("docs/guide/Hub使用手册.md")
    if re.search(r"青色(呼吸|快闪)", man):
        fails.append("Hub使用手册（HA 型）出现了青色呼吸 / 快闪 —— 那是直连型的配对指示，HA 型里没有")

if fails:
    print("❌ 灯的行为定义与文档不一致：")
    for f in fails:
        print("   ·", f)
    sys.exit(1)
sys.exit(0)
