#!/usr/bin/env python3
"""看网关在 mDNS 里给自己的名字宣告了哪个地址（T9.34，2026-10-07）。只读：不改网关任何东西。

    python3 tools/mdns_self_addr.py --query                 # 发一次 <名字>.local 的 A 查询，打出每个应答的源地址与 A 记录
    python3 tools/mdns_self_addr.py --listen 75             # 被动听 75 s：网关发出的每个带本机 A/AAAA 的包（配合重启 / 拔插网线）
    选项：--name bitengine.local（默认）  --from 192.168.1.34,192.168.1.36（只看这些源地址；默认不过滤）

判据（行为正本是 Kconfig BITENGINE_MDNS_SVC_IP_ONLY / BITENGINE_SVC_IDENTITY 的 help；上板用例在
docs/test_cases_stability.md 测试 6e「每一步都看」第 2 条，降级路径见 6d 的 mDNS 判据）：
  - 插着网线 / 拔线 / 插回 / 重启：A 记录只有设备地址（/status 的 svc_ip，T9.37 起按 DHCP 身份在两张卡间浮动），
    从不出现另一张卡的地址、也不出现 0.0.0.0；
  - 开机设备地址还没定下来时（首次安装）：没有任何带本机 A/AAAA 的包（暂不给），而不是先给 WiFi 地址；
  - 应答源：两口同网段时固定以太网口（tools/patch_mdns.py 的 bitengine_if_answers）；拔着网线时是 WiFi 口。
    先用 WiFi 装、插着网线时，以太网口的地址就是设备地址（网口在用 WiFi 的身份）。

为什么不用 avahi-resolve / python-zeroconf：它们只给"解析结果"，看不到哪个口在应答、同一个包里几个 A、
有没有 0.0.0.0 —— 这几样正是 T9.34 要验的。python-zeroconf 还会把同一包里的地址倒序列出（后到的在前）。
"""
import argparse
import socket
import struct
import time


def qname(n: str) -> bytes:
    return b"".join(bytes([len(p)]) + p.encode() for p in n.split(".")) + b"\0"


def read_name(b: bytes, i: int, depth: int = 0):
    out = []
    while True:
        l = b[i]
        if l == 0:
            return ".".join(out), i + 1
        if l & 0xC0 == 0xC0:
            if depth > 10:
                raise ValueError("压缩指针套太深")
            ptr = ((l & 0x3F) << 8) | b[i + 1]
            n, _ = read_name(b, ptr, depth + 1)
            out.append(n)
            return ".".join(out), i + 2
        out.append(b[i + 1:i + 1 + l].decode(errors="replace"))
        i += 1 + l


def self_records(b: bytes, name: str):
    """包里所有 owner == name 的 A / AAAA 记录（三个段都看），按出现顺序；TTL 0 标 bye。"""
    qd, an, ns, ar = struct.unpack(">HHHH", b[4:12])
    i = 12
    for _ in range(qd):
        _, i = read_name(b, i)
        i += 4
    recs = []
    for sec, cnt in (("an", an), ("ns", ns), ("ar", ar)):
        for _ in range(cnt):
            n, i = read_name(b, i)
            t, _c, ttl, l = struct.unpack(">HHIH", b[i:i + 10])
            i += 10
            if n.lower().rstrip(".") == name:
                bye = " bye" if ttl == 0 else ""
                if t == 1:
                    recs.append(f"A {socket.inet_ntoa(b[i:i + 4])}{bye} ({sec})")
                elif t == 28:
                    recs.append(f"AAAA {socket.inet_ntop(socket.AF_INET6, b[i:i + 16])}{bye} ({sec})")
            i += l
    return recs


def open_sock():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", 5353))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                 socket.inet_aton("224.0.0.251") + socket.inet_aton("0.0.0.0"))
    s.settimeout(0.5)
    return s


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--query", action="store_true", help="发一次 A 查询，收 3 s")
    g.add_argument("--listen", type=float, metavar="SECONDS", help="被动听这么多秒")
    ap.add_argument("--name", default="bitengine.local")
    ap.add_argument("--from", dest="srcs", default="", help="只看这些源地址（逗号分隔）")
    a = ap.parse_args()
    name = a.name.lower().rstrip(".")
    srcs = {x.strip() for x in a.srcs.split(",") if x.strip()}

    s = open_sock()
    if a.query:
        s.sendto(struct.pack(">HHHHHH", 0, 0, 1, 0, 0, 0) + qname(name) + struct.pack(">HH", 1, 1),
                 ("224.0.0.251", 5353))
        end = time.time() + 3
    else:
        end = time.time() + a.listen
    seen = 0
    while time.time() < end:
        try:
            b, src = s.recvfrom(9000)
        except socket.timeout:
            continue
        if srcs and src[0] not in srcs:
            continue
        try:
            recs = self_records(b, name)
        except Exception as e:  # noqa: BLE001 —— 坏包照样记一行，别让工具自己崩
            print(time.strftime("%H:%M:%S"), "源", src[0], "解析失败:", e, flush=True)
            continue
        if recs:
            seen += 1
            print(time.strftime("%H:%M:%S"), "源", src[0], recs, flush=True)
    if not seen:
        print(f"（这段时间没有任何带 {name} A/AAAA 的包）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
