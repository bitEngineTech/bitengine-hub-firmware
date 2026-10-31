#!/usr/bin/env python3
"""网关重启试验台：每次重启自动记录一行，用于评估 matter-server 自愈补丁的命中率。

背景
----
自愈补丁（tools/matter-server-patch/）依赖 mDNS 事件触发：设备重新入网时若
zeroconf 报出 ServiceStateChange，补丁就强制重建订阅（秒级恢复）；若没有事件，
只能干等 CHIP 存活超时（电池设备最长 600s，实测 4-5 分钟）。

前 5 次重启里 3 次有事件、2 次没有，命中率 60%，原因未明——设备 IP、OMR 前缀
跨重启都不变，理论上记录内容一致就不该有事件。本脚本把每次试验的变量记下来，
重点看**断电时长**是不是决定因素（假设：断电够久设备才会真正离网并重新注册 SRP）。

用法
----
    python3 tools/reboot_trial.py --host 192.168.1.43
然后反复断电重上电即可，每次自动追加一行到 CSV。Ctrl-C 结束。
"""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import subprocess
import sys
import time
import urllib.error
import urllib.request

POLL_S = 3
OBSERVE_S = 600          # 重启后最多观察 10 分钟
HTTP_TIMEOUT_S = 3

FIELDS = [
    "trial", "started_at", "down_seconds", "mdns_event", "mdns_state_change",
    "patch_fired", "liveness_timeout", "recovered_seconds", "recovery_path",
]


def status(host: str) -> dict | None:
    try:
        with urllib.request.urlopen(
            f"http://{host}/api/v1/status", timeout=HTTP_TIMEOUT_S
        ) as r:
            return json.load(r)
    except (urllib.error.URLError, OSError, ValueError, TimeoutError):
        return None


def logs_since(container: str, seconds: int) -> str:
    try:
        p = subprocess.run(
            ["docker", "logs", "--since", f"{seconds}s", container],
            capture_output=True, timeout=30,
        )
        return (p.stdout + p.stderr).decode(errors="replace")
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return ""


def observe(container: str, down_s: float) -> dict:
    """重启后盯着 matter-server，看哪条路径先完成恢复。"""
    t0 = time.time()
    row = {
        "down_seconds": round(down_s, 1),
        "mdns_event": 0, "mdns_state_change": "",
        "patch_fired": 0, "liveness_timeout": 0,
        "recovered_seconds": "", "recovery_path": "none",
    }
    # 只统计本次重启之后新产生的日志，所以每轮都从 t0 起算窗口
    while time.time() - t0 < OBSERVE_S:
        window = int(time.time() - t0) + 5
        text = logs_since(container, window)

        for line in text.splitlines():
            if "MDNS-EVAL state" in line and not row["mdns_event"]:
                row["mdns_event"] = 1
                if "ServiceStateChange." in line:
                    row["mdns_state_change"] = (
                        line.split("ServiceStateChange.")[1].split()[0]
                    )
            if "forcing re-subscription" in line:
                row["patch_fired"] = 1
            if "Liveness timeout" in line:
                row["liveness_timeout"] = 1

        if "Subscription succeeded" in text and (row["patch_fired"] or row["liveness_timeout"]):
            row["recovered_seconds"] = round(time.time() - t0, 1)
            row["recovery_path"] = "patch" if row["patch_fired"] else "liveness_timeout"
            return row

        time.sleep(POLL_S)

    row["recovery_path"] = "timeout_no_recovery"
    return row


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--container", default="matter-server")
    ap.add_argument("--out", default="/var/tmp/reboot_trials.csv")
    args = ap.parse_args()

    out = pathlib.Path(args.out)
    new = not out.exists()
    fh = out.open("a", newline="")
    w = csv.DictWriter(fh, fieldnames=FIELDS)
    if new:
        w.writeheader()
        fh.flush()

    trial = sum(1 for _ in out.open()) - 1 if not new else 0
    print(f"试验台就绪，输出 {out}（已有 {trial} 次记录）")
    print("反复断电重上电即可，每次自动记录一行。Ctrl-C 结束。\n")

    last_uptime = None
    gone_at: float | None = None

    while True:
        st = status(args.host)

        if st is None:
            if gone_at is None:
                gone_at = time.time()
                print(f"[{time.strftime('%H:%M:%S')}] 网关失联…")
            time.sleep(POLL_S)
            continue

        uptime = st.get("uptime")
        if not isinstance(uptime, int):
            time.sleep(POLL_S)
            continue

        rebooted = last_uptime is not None and uptime < last_uptime
        if rebooted:
            down_s = (time.time() - gone_at) if gone_at else 0.0
            trial += 1
            print(f"[{time.strftime('%H:%M:%S')}] 第 {trial} 次重启，"
                  f"断电约 {down_s:.0f}s — 开始观察…")
            row = observe(args.container, down_s)
            row["trial"] = trial
            row["started_at"] = time.strftime("%Y-%m-%dT%H:%M:%S")
            w.writerow(row)
            fh.flush()
            print(f"    → mDNS事件={row['mdns_event']}({row['mdns_state_change']}) "
                  f"补丁触发={row['patch_fired']} "
                  f"恢复路径={row['recovery_path']} "
                  f"耗时={row['recovered_seconds']}s\n")
            st = status(args.host)
            uptime = st.get("uptime", uptime) if st else uptime

        gone_at = None
        last_uptime = uptime
        time.sleep(POLL_S)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n结束")
        sys.exit(0)
