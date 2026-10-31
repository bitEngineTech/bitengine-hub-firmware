#!/usr/bin/env python3
"""TC-ZH07（docs/15 第十四节）：NCP 恢复 —— 直连型的 zhub 遇到"NCP 自己重启"能不能自己回来。

注入：故障注入构建（CONFIG_BITENGINE_FAULT_INJECTION=y）的 POST /api/v1/diag/fault {"kind":"zhub_ash_rst"}，
zhub 背着自己的 ASH 状态机往 UART 写一帧 RST —— NCP 复位、回 RSTACK，连着的 ASH 按"NCP 复位"处理
（ZH_HOST_FAIL_NCP_RESET），走正常的复位 → 握手 → networkInit。**不碰 IO23**（红线：IO23 在 AM1 上会拖垮 EN）。

每轮判据：
  ① 注入被收下（200）；不是故障注入构建回 400 unknown_kind → 退出码 2
  ② 5 s 内 hub.ash.ncp_resets 加一（zhub 发现了 NCP 复位）
  ③ 期限内回到 state=running、net=up，net_inits 加一（第 1 次失败立即重来；连续失败按退避表 5 s / 30 s / 60 s 封顶）
  ④ last_fail = ncp_reset；板子没重启（uptime 连续）；设备记录数不变；Thread 角色不变（取得到时）
轮与轮之间默认等 65 s（连续 RUNNING 满 ZH_HOST_STABLE_MS = 60 s 才清连续失败次数），--back-to-back 则紧接着打、验退避那一档。

用法：GW_PASS=... python3 tools/test_zhub_ncp_reset.py [IP] [--rounds N] [--back-to-back]
"""
import argparse
import base64
import json
import os
import sys
import time
import urllib.error
import urllib.request


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ip", nargs="?", default="192.168.1.40")
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--back-to-back", action="store_true", help="上一轮恢复后立刻再打（连续失败计数没清：走退避的 5 s 那一档）")
    a = ap.parse_args()
    pw = os.environ.get("GW_PASS")
    if not pw:
        print("要 GW_PASS 环境变量（网关 admin 密码）", file=sys.stderr)
        return 2
    auth = "Basic " + base64.b64encode(f"admin:{pw}".encode()).decode()
    base = f"http://{a.ip}"

    def req(path, body=None, timeout=5):
        r = urllib.request.Request(base + path, data=None if body is None else json.dumps(body).encode(),
                                   headers={"Authorization": auth, "Content-Type": "application/json"},
                                   method="GET" if body is None else "POST")
        try:
            with urllib.request.urlopen(r, timeout=timeout) as f:
                return f.status, json.loads(f.read() or b"null")
        except urllib.error.HTTPError as e:
            try:
                return e.code, json.loads(e.read() or b"null")
            except ValueError:
                return e.code, None

    def hub():
        s, d = req("/api/v1/zigbee/status")
        return d["hub"] if s == 200 and d and "hub" in d else None

    def uptime():
        s, d = req("/api/v1/status")
        return d.get("uptime") if s == 200 and d else None

    def thread_role():
        """OTBR REST 的 /node/state 回纯文本（leader / router / …，不是 JSON）；开着 OTBR 认证时要带认证头（审计 ❌：原来两样都错，这项从来没执行过）"""
        try:
            r = urllib.request.Request(f"http://{a.ip}:8081/node/state", headers={"Authorization": auth})
            with urllib.request.urlopen(r, timeout=3) as f:
                return f.read().decode().strip().strip('"') or None
        except Exception as e:
            print(f"⚠️ 读不到 Thread 角色（{e}）：这一轮不比 Thread")
            return None

    def wait_running(limit_s):
        t = time.monotonic()
        while time.monotonic() - t < limit_s:
            h = hub()
            if h and h.get("state") == "running" and h.get("net") == "up":
                return h
            time.sleep(1)
        return None

    fails = 0

    def check(ok, msg):
        nonlocal fails
        print(("✅ " if ok else "❌ ") + msg)
        if not ok:
            fails += 1

    h0 = hub()
    if not h0 or not h0.get("started"):
        print("❌ 读不到 hub 快照（不是直连型，或 zhub 没起）")
        return 1
    if h0.get("state") != "running" or h0.get("net") != "up":
        print(f"❌ 开始前 zhub 不在 running / up（{h0.get('state')} / {h0.get('net')}）：先等它稳定")
        return 1

    for rnd in range(1, a.rounds + 1):
        if rnd > 1 and not a.back_to_back:
            print("…等 65 s（连续 RUNNING 满 60 s 清连续失败次数）")
            time.sleep(65)
        h0 = wait_running(90)                     # 每轮先确认它在 running / up：退避或握手中注入，RSTACK 会被丢掉 / 当成握手应答，结论会记错（审计 ⚠️）
        if not h0:
            check(False, "注入前 zhub 一直没回到 running / up（90 s）")
            break
        up0, role0 = uptime(), thread_role()
        n_dev0 = (h0.get("devices") or {}).get("count")
        print(f"— 第 {rnd} 轮：ncp_resets={h0['ash']['ncp_resets']} net_inits={h0['net_inits']} fails={h0['fails']} uptime={up0} thread={role0}")
        t0 = time.monotonic()
        s, d = req("/api/v1/diag/fault", {"kind": "zhub_ash_rst"})
        if s in (404, 405) or (s == 400 and d and d.get("error") == "unknown_kind"):   # 非故障注入构建没有这个端点（静态文件的通配路由只注册了 GET：POST 回 405，10-23 实测）；HA 型 / 旧的故障注入构建不认这个 kind（400）
            print("⚠️ 不是故障注入构建（/diag/fault 不认 zhub_ash_rst）：要 CONFIG_BITENGINE_FAULT_INJECTION=y 的直连型固件")
            return 2
        check(s == 200, f"注入被收下（HTTP {s} {d}）")
        if s != 200:
            continue
        t_det = t_rec = None
        limit = 15 + (0, 5, 30, 60)[min(h0["fails"], 3)]   # handle_fail 先加一再查 k_backoff_ms {0, 5, 30, 60 s}；15 s 是握手 + networkInit 的余量
        h = h0
        while time.monotonic() - t0 < limit + 30:
            time.sleep(0.5)
            hh = hub()
            if not hh:
                continue
            h = hh
            if t_det is None and h["ash"]["ncp_resets"] > h0["ash"]["ncp_resets"]:
                t_det = time.monotonic() - t0
            if t_det is not None and h["state"] == "running" and h["net"] == "up" and h["net_inits"] > h0["net_inits"]:
                t_rec = time.monotonic() - t0
                break
        check(t_det is not None and t_det <= 5, f"zhub 发现 NCP 复位（ash.ncp_resets 加一）：{'%.1f s' % t_det if t_det is not None else '没发现'}")
        check(t_rec is not None and t_rec <= limit, f"回到 running / up、重新 networkInit：{'%.1f s' % t_rec if t_rec is not None else '没回来'}（期限 {limit} s）")
        check(h.get("last_fail") == "ncp_reset", f"last_fail = {h.get('last_fail')}（应为 ncp_reset）")
        up1 = uptime()
        check(up0 is not None and up1 is not None and up1 >= up0, f"板子没重启（uptime {up0} → {up1}）")
        n_dev1 = (h.get("devices") or {}).get("count")
        check(n_dev0 == n_dev1, f"设备记录数不变（{n_dev0} → {n_dev1}）")
        role1 = thread_role()
        if role0 is not None:
            check(role1 == role0, f"Thread 角色不变（{role0} → {role1}）")
    print(f"\n结果：{'全部通过' if fails == 0 else f'{fails} 项失败'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
