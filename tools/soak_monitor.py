#!/usr/bin/env python3
"""长跑监测：持续记录网关的可达性、时延与健康指标，事后出报告。

    python3 tools/soak_monitor.py run    [--gw 192.168.1.34] [--interval 60] [--log 路径]
    python3 tools/soak_monitor.py report [--log 路径] [--since 2026-10-05T12:00] [--hours 24]

── 为什么要它 ───────────────────────────────────────────────────────────

上市前验证清单要"长期稳定性的公开实测数据"（5GHz 和 PoE 两种回传），而此前只有
`docs/test_cases_stability.md` 测试 9 里一段手抄的 bash 循环，结果表一直是空的；
设备连续在线 11 天这种事实，也因为没人记录而只剩一个 uptime 读数。

── 测什么 ───────────────────────────────────────────────────────────────

每 interval 秒，对网关的**每个地址**（以太网、WiFi 各一个，首轮从 /status 自动发现）
各取一次 `/api/v1/status`，记下是否成功和往返时延 —— 同一时刻、同一台设备、两条回传，
所以 WiFi 与以太网的时延可以直接对比。另从 OTBR 端口取 `/node/counters`，用
`rx_total` 是否在涨判断 802.15.4 射频是否还活着（射频静默死亡、协议栈照报 leader
的故障发生过两次，见 test_cases_stability.md）。

⚠️ 绝不探 6638 端口：桥只收一个客户端，探活连接要么顶掉、要么被拒，而 bellows 对
   "accept 后立刻 close"会卡死（docs/upstream/bellows-reject-ab.py）。上位机连没连着，
   看 /status 的 tcp_clients 和 zigbee_host_absent_s 就够了。
⚠️ 时延是"HA 主机所在网段 → 网关"的 HTTP 往返，不是 Zigbee 命令时延。后者要在 HA
   侧测（网关只转发字节，看不到命令），本工具不冒充它。

── 输出 ─────────────────────────────────────────────────────────────────

每次采样一行 JSON（JSONL），默认 ~/.local/state/bitengine-soak/soak.jsonl。
report 子命令据此算：不可达窗口、时延 P50/P95/P99、重启（uptime 回退）、
上位机断开窗口、MG24 复位、内部堆走势、WiFi 断线、Thread 射频停摆，
并按测试 9 的判据给出结论。常驻运行见 tools/systemd/README.md 的 be-soak-monitor。
"""
import argparse, datetime as dt, json, os, statistics, sys, time, urllib.request

DEFAULT_LOG = os.path.expanduser("~/.local/state/bitengine-soak/soak.jsonl")
KEEP = ("uptime", "fw_version", "active_interface", "eth_link_up", "wifi_connected", "wifi_rssi",
        "wifi_channel", "wifi_dfs", "wifi_disconnects", "wifi_down_s", "wifi_last_outage_s",
        "tcp_clients", "zigbee_host_absent_s", "mg24_alive", "mg24_resets", "internal_free_heap",
        "alloc_failures", "thread_children", "poe_present", "safe_mode", "last_reset_reason",
        "wifi_ip", "svc_takeover",   # T9.20：重启后服务 IP 有没有接管到 WiFi、WiFi 实际用的哪个地址
        # T9.37 服务身份：服务地址是哪个、此刻在哪张卡上。⚠️ 交换着时 eth / wifi 两条探测路径的语义对调
        # （"wifi" 那条打的 WiFi 自己的地址此刻可能是网口身份的租约），看可达性时以 svc_ip 为准
        "eth_ip", "svc_identity", "svc_ip", "svc_holder", "svc_id_mode")


def fetch(url, timeout, auth=None):
    req = urllib.request.Request(url)
    if auth:
        import base64
        req.add_header("Authorization", "Basic " + base64.b64encode(f":{auth}".encode()).decode())
    t0 = time.monotonic()
    with urllib.request.urlopen(req, timeout=timeout) as r:
        body = r.read()
    return json.loads(body), (time.monotonic() - t0) * 1000


def now_iso():
    return dt.datetime.now().astimezone().isoformat(timespec="seconds")


def discover(gw, timeout, auth):
    d, _ = fetch(f"http://{gw}/api/v1/status", timeout, auth)
    hosts = {}
    for name, key in (("eth", "eth_ip"), ("wifi", "wifi_ip")):
        ip = d.get(key)
        if ip and ip != "0.0.0.0":
            hosts[name] = ip
    return hosts or {"gw": gw}


def cmd_run(a):
    os.makedirs(os.path.dirname(a.log), exist_ok=True)
    auth = os.environ.get("GW_PASS") or None
    hosts = None
    while hosts is None:
        try:
            hosts = discover(a.gw, a.timeout, auth)
        except Exception as e:
            print(f"{now_iso()} 发现地址失败（{e}），30 s 后重试", flush=True)
            time.sleep(30)
    print(f"{now_iso()} 开始监测 {hosts}，每 {a.interval} s 一次 → {a.log}", flush=True)
    while True:
        t_next = time.monotonic() + a.interval
        lines = []
        for name, ip in hosts.items():
            rec = {"t": now_iso(), "path": name, "host": ip}
            try:
                d, ms = fetch(f"http://{ip}/api/v1/status", a.timeout, auth)
                rec.update(ok=True, ms=round(ms, 1), **{k: d.get(k) for k in KEEP})
            except Exception as e:
                rec.update(ok=False, err=type(e).__name__)
            lines.append(rec)
        # Thread 计数器只取一次（同一颗射频，两个地址看到的是同一个数），走能通的那条
        for rec in lines:
            if not rec.get("ok"):
                continue
            try:
                c, _ = fetch(f"http://{rec['host']}:8081/node/counters", a.timeout, auth)
                rec["thread_rx_total"] = c.get("rx_total")
                rec["thread_tx_total"] = c.get("tx_total")
            except Exception as e:
                rec["thread_err"] = type(e).__name__
            break
        with open(a.log, "a", encoding="utf-8") as f:
            for rec in lines:
                f.write(json.dumps(rec, ensure_ascii=False) + "\n")
        time.sleep(max(1.0, t_next - time.monotonic()))


# ─────────────────────────────── 报告 ───────────────────────────────

def pct(v, p):
    if not v:
        return None
    s = sorted(v)
    k = (len(s) - 1) * p / 100
    lo, hi = int(k), min(int(k) + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def windows(recs, bad):
    """连续满足 bad(rec) 的采样段 → [(开始, 结束, 采样数)]。"""
    out, start, last, n = [], None, None, 0
    for r in recs:
        if bad(r):
            if start is None:
                start, n = r["t"], 0
            last, n = r["t"], n + 1
        elif start is not None:
            out.append((start, last, n)); start = None
    if start is not None:
        out.append((start, last, n))
    return out


def span_s(a, b):
    return (dt.datetime.fromisoformat(b) - dt.datetime.fromisoformat(a)).total_seconds()


def fmt_s(s):
    s = int(s)
    return f"{s // 86400}d{s % 86400 // 3600}h" if s >= 86400 else (
        f"{s // 3600}h{s % 3600 // 60}m" if s >= 3600 else f"{s // 60}m{s % 60}s")


def cmd_report(a):
    if not os.path.exists(a.log):
        sys.exit(f"没有日志：{a.log}（先跑 run）")
    recs = [json.loads(l) for l in open(a.log, encoding="utf-8") if l.strip()]
    if a.since:
        recs = [r for r in recs if r["t"] >= a.since]
    if a.hours:
        cut = (dt.datetime.now().astimezone() - dt.timedelta(hours=a.hours)).isoformat(timespec="seconds")
        recs = [r for r in recs if r["t"] >= cut]
    if not recs:
        sys.exit("所选时间段内没有采样")
    paths = sorted({r["path"] for r in recs})
    t0, t1 = recs[0]["t"], recs[-1]["t"]
    upgrades = upgrade_boots(recs)   # 升级引起的重启时刻（墙钟），其前后的不可达是预期内的
    print(f"# 长跑报告  {t0} → {t1}（{fmt_s(span_s(t0, t1))}）\n")
    verdict = {}

    for p in paths:
        rs = [r for r in recs if r["path"] == p]
        ok = [r for r in rs if r.get("ok")]
        ms = [r["ms"] for r in ok]
        print(f"## 回传 {p}（{rs[0]['host']}）")
        print(f"- 采样 {len(rs)}，成功 {len(ok)}（{100 * len(ok) / len(rs):.2f}%）")
        if ms:
            print(f"- /status 往返时延 ms：P50 {pct(ms, 50):.0f} · P95 {pct(ms, 95):.0f} · "
                  f"P99 {pct(ms, 99):.0f} · 最大 {max(ms):.0f}")
        iv = a_interval(rs)
        down = windows(rs, lambda r: not r.get("ok"))
        # 升级时网关要重启，那一两次采样不通是预期内的，不算回传故障
        upg = [w for w in down if any(near(b, w, iv) for b in upgrades)]
        real = [w for w in down if w not in upg]
        print(f"- 不可达窗口：{len(real)} 段" + ("" if not real else "，最长 " +
              fmt_s(max(span_s(s, e) for s, e, _ in real) + iv)) +
              (f"（另有 {len(upg)} 段是固件升级重启，不计）" if upg else ""))
        for s, e, n in real[:10]:
            print(f"  - {s} → {e}（{n} 次采样）")
        verdict[p] = not real
        print()

    # 设备级指标：取每个时刻任一成功的采样（两条回传看到的是同一台设备）
    by_t = {}
    for r in recs:
        if r.get("ok") and r["t"] not in by_t:
            by_t[r["t"]] = r
    dev = [by_t[k] for k in sorted(by_t)]
    if not dev:
        print("（全程没有一次成功采样）"); return
    print("## 设备")
    boots = [(b["t"], a_["fw_version"], b["fw_version"]) for a_, b in zip(dev, dev[1:])
             if (b.get("uptime") or 0) < (a_.get("uptime") or 0)]
    print(f"- 重启：{len(boots)} 次" + ("".join(
        f"\n  - {t}（{'固件 ' + f0 + ' → ' + f1 + '，升级' if f0 != f1 else '固件未变 —— 非升级重启'}）"
        for t, f0, f1 in boots)))
    print(f"- 期末 uptime：{fmt_s(dev[-1].get('uptime') or 0)}，固件 {dev[-1].get('fw_version')}")
    host_gone = windows(dev, lambda r: (r.get("tcp_clients") or 0) == 0)
    print(f"- 上位机（ZHA/Z2M）断开：{len(host_gone)} 段" + "".join(
        f"\n  - {s} → {e}（{n} 次采样）" for s, e, n in host_gone[:10]))
    d_res = (dev[-1].get("mg24_resets") or 0) - (dev[0].get("mg24_resets") or 0)
    d_af = (dev[-1].get("alloc_failures") or 0) - (dev[0].get("alloc_failures") or 0)
    print(f"- MG24 复位增量：{d_res}　内存分配失败增量：{d_af}")
    # mg24_resets 只在网关**主动**复位 MG24 时 +1（AM1 上恒 0）；NCP 自己崩溃循环只在 mg24_alive=false 上看得到
    # （10-06 子设备 64 那次：soak.jsonl 里 20 个 false 采样，报告却写"复位增量 0"，T9.19 ①）
    mg24_dead = windows(dev, lambda r: r.get("mg24_alive") is False)
    print(f"- MG24 判死（mg24_alive=false）：{len(mg24_dead)} 段" + "".join(
        f"\n  - {s} → {e}（{n} 次采样）" for s, e, n in mg24_dead[:10]))
    heap = [r["internal_free_heap"] for r in dev if r.get("internal_free_heap") is not None]
    if heap:
        print(f"- 内部堆空闲：首 {heap[0]} · 末 {heap[-1]} · 最低 {min(heap)}（差 {heap[-1] - heap[0]:+d} 字节）")
    wifi = [r for r in dev if r.get("wifi_rssi") is not None and r.get("wifi_connected")]
    if wifi:
        rssi = [r["wifi_rssi"] for r in wifi]
        ch = wifi[-1].get("wifi_channel")
        print(f"- WiFi RSSI：中位 {statistics.median(rssi):.0f} · 最差 {min(rssi)} dBm；" +
              (f"信道 {ch}（DFS：{'是' if wifi[-1].get('wifi_dfs') else '否'}）" if ch else "信道（固件未报，v0.5.0-395 之前）"))
    disc = [r.get("wifi_disconnects") for r in dev if r.get("wifi_disconnects") is not None]
    if disc:
        print(f"- WiFi 断线：{disc[-1] - disc[0]} 次（固件计数）" +
              (f"，最近一次持续 {dev[-1].get('wifi_last_outage_s')} s" if dev[-1].get('wifi_last_outage_s') is not None else ""))
    rx = [(r["t"], r["thread_rx_total"]) for r in dev if r.get("thread_rx_total") is not None]
    stalls = windows([{"t": t, "stall": i > 0 and v <= rx[i - 1][1]} for i, (t, v) in enumerate(rx)],
                     lambda r: r["stall"])
    stalls = [w for w in stalls if w[2] >= max(1, round(300 / a_interval(dev)))]   # 停 ≥ 5 分钟才算
    print(f"- Thread 射频停摆（rx_total ≥5 分钟不涨）：{len(stalls)} 段" + "".join(
        f"\n  - {s} → {e}" for s, e, _ in stalls[:10]))

    print("\n## 测试 9 判据（docs/test_cases_stability.md）")
    crit = [
        ("网关始终可达（以太网路径）" if "eth" in verdict else "网关始终可达",
         verdict.get("eth", all(verdict.values()))),
        ("mg24_resets 不增", d_res == 0),
        ("MG24 从未判死（mg24_alive 始终为 true）", not mg24_dead),
        ("内部堆无持续下降（首末差 > -5 KB）", not heap or heap[-1] - heap[0] > -5120),
        ("上位机连接始终保持", not host_gone),
        ("无非升级重启", all(f0 != f1 for _, f0, f1 in boots)),
    ]
    for name, good in crit:
        print(f"- {'✅' if good else '❌'} {name}")
    print("- ⬜ 期末传感器仍能触发（人工）")


def upgrade_boots(recs):
    """固件版本变了的那次重启，返回其墙钟时刻 = 重启后首个采样时刻 − uptime。"""
    ok = sorted((r for r in recs if r.get("ok") and r.get("uptime") is not None), key=lambda r: r["t"])
    out = []
    for a, b in zip(ok, ok[1:]):
        if b["uptime"] < a["uptime"] and a.get("fw_version") != b.get("fw_version"):
            boot = dt.datetime.fromisoformat(b["t"]) - dt.timedelta(seconds=b["uptime"])
            if not out or abs((boot - out[-1]).total_seconds()) > 5:
                out.append(boot)
    return out


def near(boot, win, iv):
    """不可达窗口 [开始, 结束] 是否被这次重启解释：重启时刻落在窗口前一个间隔到窗口后两个间隔之间。"""
    s, e = dt.datetime.fromisoformat(win[0]), dt.datetime.fromisoformat(win[1])
    return s - dt.timedelta(seconds=iv * 2) <= boot <= e + dt.timedelta(seconds=iv * 2)


def a_interval(rs):
    if len(rs) < 2:
        return 60
    ts = [dt.datetime.fromisoformat(r["t"]) for r in rs[:50]]
    gaps = [(b - a).total_seconds() for a, b in zip(ts, ts[1:]) if b > a]
    return statistics.median(gaps) if gaps else 60


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--gw", default="192.168.1.34")
    r.add_argument("--interval", type=int, default=60)
    r.add_argument("--timeout", type=float, default=5.0)
    r.add_argument("--log", default=DEFAULT_LOG)
    p = sub.add_parser("report")
    p.add_argument("--log", default=DEFAULT_LOG)
    p.add_argument("--since")
    p.add_argument("--hours", type=float)
    a = ap.parse_args()
    (cmd_run if a.cmd == "run" else cmd_report)(a)


if __name__ == "__main__":
    main()
