#!/usr/bin/env python3
"""network_mgr 的 esp_timer 回调不许拿状态机的锁（T9.28，2026-10-07）。

esp_timer 任务优先级 22，全系统的定时器都在它上面串行跑。回调里 portMAX_DELAY 等 s_sm_mutex 时，
持锁的一方（事件循环、httpd 改 WiFi……）被优先级继承抬到 22，等的那段时间所有定时器都停 ——
与 10-07 在 thread_br / mdns 里修掉的是同一形状。所以回调只许投递（esp_event_post / tcpip_try_callback /
重排自己），状态机的活在事件循环（sys_evt）里做。

做法：扫本组件所有 .c，找出 esp_timer_create_args_t 里登记的回调，取出函数体（去掉注释），
检查里面不出现下面这些调用。只查直接调用 —— 间接的靠这张表把已知会拿锁的入口列全：
  xSemaphoreTake*            —— 直接拿锁
  network_state_machine      —— 一进去就拿 s_sm_mutex
  network_mgr_recover        —— 同上
  boot_takeover_eval / svc_probe_consume / float_svc_update / mdns_self_update /
  svc_id_update / svc_id_handover / svc_id_on_got_ip（T9.37）—— 头注写着"持 s_sm_mutex"调

用法：python3 check_timer_cbs.py [组件目录]   （VERIFY.sh 在组件目录里调）
退出码 0 = 全部干净；1 = 有回调违规或一个回调都没找到（多半是解析坏了，别让它恒绿）。
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from grep_code import strip_comments  # noqa: E402

FORBIDDEN = [
    r"\bxSemaphoreTake\w*\s*\(",
    r"\bnetwork_state_machine\s*\(",
    r"\bnetwork_mgr_recover\s*\(",
    r"\bboot_takeover_eval\s*\(",
    r"\bsvc_probe_consume\s*\(",
    r"\bfloat_svc_update\s*\(",
    r"\bmdns_self_update\s*\(",
    r"\bsvc_id_update\s*\(",
    r"\bsvc_id_handover\s*\(",
    r"\bsvc_id_on_got_ip\s*\(",
]

# 回调名：.callback = foo（同一行或多行写法都认）
CB_RE = re.compile(r"\.callback\s*=\s*(\w+)")


def function_body(src: str, name: str) -> str | None:
    """取 `static void name(...)` 的函数体：从定义行起，到第一个顶格的 `}` 为止（本仓库的 C 风格）。"""
    m = re.search(r"^(?:static\s+)?void\s+" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", src, re.M)
    if not m:
        return None
    end = re.search(r"^\}", src[m.end():], re.M)
    return src[m.start(): m.end() + (end.start() if end else len(src))]


def main() -> int:
    comp = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    files = sorted(comp.glob("*.c"))
    stripped = {f: strip_comments(f.read_text(encoding="utf-8")) for f in files}
    cbs = sorted({(f, cb) for f, s in stripped.items() for cb in CB_RE.findall(s)}, key=lambda x: (x[0].name, x[1]))
    if not cbs:
        print("❌ 一个 esp_timer 回调都没找到 —— 解析坏了？")
        return 1
    bad = 0
    for f, cb in cbs:
        body = function_body(stripped[f], cb)
        if body is None:
            print(f"❌ {f.name}: 找不到回调 {cb} 的定义")
            bad += 1
            continue
        hits = [p for p in FORBIDDEN if re.search(p, body)]
        if hits:
            names = ", ".join(re.sub(r"\\[bsw]\*?|\\\(|\s|\*|\\s|\(", "", p) for p in hits)
            print(f"❌ {f.name}: {cb} 里有 {names}")
            bad += 1
        else:
            print(f"✅ {f.name}: {cb}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
