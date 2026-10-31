#!/usr/bin/env python3
"""webhook.c 里写死的 WEBUI_EVENT_ID_* 必须与 webui.h 的 enum 顺序一致。

webhook 不能 include webui.h —— webui → spectrum → webhook 会成环，所以它把
几个 id 抄成了 #define。抄的东西会漂：往 enum 中间插一个值，webhook 就会去订
另一件事，而且**编译不报错**，表现是"某类通知莫名其妙不发了或发错了"。

2026-09-11(T2.7) 正是往那个 enum 末尾加了 OTA_VERIFIED，所以这条检查必须有。
"""
import re
import sys
from pathlib import Path

D = Path(__file__).resolve().parent
hdr = (D / "include" / "webui.h").read_text(encoding="utf-8")
wh = (D / ".." / "webhook" / "webhook.c").read_text(encoding="utf-8")

m = re.search(r"typedef enum \{(.*?)\} webui_event_id_t;", hdr, re.S)
if not m:
    sys.exit("❌ webui.h 里找不到 webui_event_id_t")
names = re.findall(r"^\s*(WEBUI_EVENT_[A-Z_]+),", m.group(1), re.M)
order = {n: i for i, n in enumerate(names)}

bad = []
for name, val in re.findall(r"#define\s+WEBUI_EVENT_ID_([A-Z_]+)\s+(\d+)", wh):
    full = "WEBUI_EVENT_" + name
    if full not in order:
        bad.append(f"{full} 在 webui.h 的 enum 里不存在")
    elif order[full] != int(val):
        bad.append(f"{full}: webhook.c 写 {val}，enum 里是 {order[full]}")
if not re.search(r"#define\s+WEBUI_EVENT_ID_", wh):
    bad.append("webhook.c 里一个 WEBUI_EVENT_ID_* 都没有（改写法了？）")

for b in bad:
    print("❌ " + b, file=sys.stderr)
sys.exit(1 if bad else 0)
