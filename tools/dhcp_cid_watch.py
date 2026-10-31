#!/usr/bin/env python3
"""看网关两张卡发 / 收的每个 DHCP 报文：类型、chaddr、option 61（client-id）、请求 / 分配的地址（T9.37 服务身份，2026-10-07）。只读。

要原始套接字。本机不用 root（账号在 docker 组）：
    docker run --rm --net=host --cap-add NET_RAW -v "$PWD/tools:/w:ro" python:3.12-slim python -u /w/dhcp_cid_watch.py 120

判据（行为正本是 Kconfig BITENGINE_SVC_IDENTITY 的 help，上板用例在 docs/test_cases_stability.md 测试 6e）：
  - 持有者的 DISCOVER / REQUEST 带服务身份 S 的 client-id（S 是另一张卡时 = 交换），另一张卡带另一个；
  - 交接时停 DHCP 发出的 RELEASE 带的是"哑身份 BEHOFF"—— 带真身份的 RELEASE 会退掉真租约；
  - 交接中没有 DECLINE（有 = 旧持有者没先放下地址，DHCP 的 ARP 检查撞上了）。
同一个报文在两张网卡上各收到一次时只打一行。
"""
import socket, struct, sys, time
import argparse
ap = argparse.ArgumentParser(description="看网关两张卡的 DHCP 报文与 option 61（T9.37）")
ap.add_argument("seconds", type=float, nargs="?", default=60)
ap.add_argument("--wifi-mac", default="38:44:be:12:34:56", help="网关 WiFi 口 MAC（/api/v1/status 或路由器设备表）")
ap.add_argument("--eth-mac",  default="3a:44:be:12:34:57", help="网关网口 MAC")
args = ap.parse_args()
DUR = args.seconds
GW_MACS = {args.wifi_mac.lower(): "WiFi", args.eth_mac.lower(): "网口"}
T = {1: "DISCOVER", 2: "OFFER", 3: "REQUEST", 4: "DECLINE", 5: "ACK", 6: "NAK", 7: "RELEASE", 8: "INFORM"}
mac = lambda b: ":".join(f"{x:02x}" for x in b)
def cid_name(c):
    if not c: return "无"
    if c[0] == 1 and len(c) == 7:
        m = mac(c[1:]); return f"01:{m}（{GW_MACS.get(m, '?')}）"
    if c[0] == 0: return "哑身份 " + c[1:].decode(errors="replace")
    return c.hex()
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(0x0003)); s.settimeout(0.5)
seen = set(); end = time.time() + DUR
while time.time() < end:
    try: b, a = s.recvfrom(4096)
    except socket.timeout: continue
    if b[12:14] != b"\x08\x00" or b[23] != 17: continue
    ihl = (b[14] & 0xF) * 4; u = 14 + ihl
    sp, dp = struct.unpack("!HH", b[u:u+4])
    if {sp, dp} != {67, 68}: continue
    d = b[u+8:]
    if len(d) < 240: continue
    xid = d[4:8]; ch = mac(d[28:34]); ciaddr = socket.inet_ntoa(d[12:16]); yiaddr = socket.inet_ntoa(d[16:20])
    if ch not in GW_MACS: continue
    o = d[240:]; i = 0; mt = None; cid = b""; req = ""
    while i < len(o) and o[i] != 255:
        if o[i] == 0: i += 1; continue
        t, l = o[i], o[i+1]; v = o[i+2:i+2+l]
        if t == 53: mt = v[0]
        elif t == 61: cid = v
        elif t == 50 and l == 4: req = socket.inet_ntoa(v)
        i += 2 + l
    key = bytes(b[u:])
    if key in seen: continue
    seen.add(key)
    print(time.strftime("%H:%M:%S"), f"{T.get(mt, mt):8}", f"chaddr={GW_MACS[ch]}", f"cid={cid_name(cid)}",
          f"ciaddr={ciaddr}" if ciaddr != "0.0.0.0" else "", f"请求={req}" if req else "", f"分配={yiaddr}" if yiaddr != "0.0.0.0" else "", flush=True)
