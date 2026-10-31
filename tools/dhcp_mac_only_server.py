#!/usr/bin/env python3
"""只认 MAC 的 DHCP 服务器 + 可调时间的 SNTP 应答 —— 测 T9.37 服务身份的降级路径（测试 6e R3 / R4）。

**只能在隔离网段上跑**（专用网桥、专用热点），绝不能接到家里那张网上：它会抢着应答所有 DHCP 请求。

为什么不用 dnsmasq --dhcp-ignore-clid：测试主机开着 ufw，DHCP 服务端口 67 / NTP 123 的入站被挡，要放行得 root 改防火墙。
这里收发都走 AF_PACKET 原始套接字（在 netfilter 之前收、发出去也不经过它），只要 NET_RAW：

    docker run --rm --net=host --cap-add NET_RAW -v "$PWD/tools:/w:ro" -v /tmp/be-dhcp:/state \\
        python:3.12-slim python -u /w/dhcp_mac_only_server.py --iface br-betest --server-ip 10.77.0.1 --state /state

行为（模拟"按 MAC 认租约"的路由器 —— 或在路由器上按 MAC 做了 DHCP 保留）：
  - 租约按 chaddr（MAC）分配，**完全不看 option 61（client-id）**；同一个 MAC 永远拿同一个地址（存 <state>/leases.json）；
  - DISCOVER → OFFER 该 MAC 的地址；REQUEST 请求的地址（option 50 或 ciaddr）是该 MAC 的 → ACK，否则 NAK；
  - RELEASE 只记日志、不收回映射（家用路由器多半如此）；DECLINE 记日志；
  - 回包一律广播（以太网 ff:ff:ff:ff:ff:ff + 255.255.255.255），不依赖客户端能不能收单播；
  - SNTP：对发到 server-ip:123 的请求按"本机时间 + 偏移"应答。偏移每次请求都重读 <state>/ntp_offset_s（整数秒，
    没有就是 0）—— 测"只认 MAC 满 7 天重试"时往里写 691200（8 天）即可，不用重启本程序。
每个报文打一行：时间、类型、chaddr、client-id、请求 / 分配的地址。只读本机时间，不改主机任何设置。
"""
import argparse
import ipaddress
import json
import os
import socket
import struct
import time

ETH_P_IP = 0x0800
BOOTREQUEST, BOOTREPLY = 1, 2
MAGIC = b"\x63\x82\x53\x63"
TYPES = {1: "DISCOVER", 2: "OFFER", 3: "REQUEST", 4: "DECLINE", 5: "ACK", 6: "NAK", 7: "RELEASE", 8: "INFORM"}
NTP_EPOCH_OFFSET = 2208988800          # 1900-01-01 → 1970-01-01


def csum(b: bytes) -> int:
    if len(b) % 2:
        b += b"\0"
    s = sum(struct.unpack("!%dH" % (len(b) // 2), b))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def frame(src_mac: bytes, dst_mac: bytes, src_ip: str, dst_ip: str, sport: int, dport: int, payload: bytes) -> bytes:
    udp = struct.pack("!HHHH", sport, dport, 8 + len(payload), 0) + payload          # IPv4 下 UDP 校验和可以是 0
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 0, 0, 64, 17, 0,
                     socket.inet_aton(src_ip), socket.inet_aton(dst_ip))
    ip = ip[:10] + struct.pack("!H", csum(ip)) + ip[12:]
    return dst_mac + src_mac + struct.pack("!H", ETH_P_IP) + ip + udp


def parse_opts(b: bytes) -> dict:
    out, i = {}, 0
    while i < len(b):
        t = b[i]
        if t == 255:
            break
        if t == 0:
            i += 1
            continue
        if i + 1 >= len(b):
            break
        l = b[i + 1]
        out[t] = b[i + 2:i + 2 + l]
        i += 2 + l
    return out


def mac_s(m: bytes) -> str:
    return ":".join("%02x" % x for x in m)


class Server:
    def __init__(self, a):
        self.a = a
        self.iface = a.iface
        self.sip = a.server_ip
        net = ipaddress.ip_network(a.server_ip + "/" + a.mask, strict=False)
        self.mask = a.mask
        self.pool = [str(h) for h in net.hosts() if a.pool_start <= int(str(h).split(".")[-1]) <= a.pool_end]
        self.state = a.state
        os.makedirs(self.state, exist_ok=True)
        self.lease_path = os.path.join(self.state, "leases.json")
        try:
            with open(self.lease_path) as f:
                self.leases = json.load(f)
        except (OSError, ValueError):
            self.leases = {}
        self.sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_IP))
        self.sock.bind((self.iface, 0))
        self.my_mac = self.sock.getsockname()[4][:6]

    def log(self, msg: str):
        print(time.strftime("%H:%M:%S"), msg, flush=True)

    def ip_for(self, mac: str) -> str:
        if mac not in self.leases:
            used = set(self.leases.values())
            free = [p for p in self.pool if p not in used]
            if not free:
                raise RuntimeError("地址池用完")
            self.leases[mac] = free[0]
            tmp = self.lease_path + ".tmp"
            with open(tmp, "w") as f:
                json.dump(self.leases, f, indent=1)
            os.replace(tmp, self.lease_path)
        return self.leases[mac]

    def ntp_offset(self) -> int:
        try:
            with open(os.path.join(self.state, "ntp_offset_s")) as f:
                return int(f.read().strip() or "0")
        except (OSError, ValueError):
            return 0

    def reply_dhcp(self, req: bytes, chaddr: bytes, xid: bytes, mtype: int, yiaddr: str):
        opts = bytes([53, 1, mtype, 54, 4]) + socket.inet_aton(self.sip)
        if mtype != 6:
            lt = self.a.lease_s
            opts += bytes([51, 4]) + struct.pack("!I", lt)
            opts += bytes([58, 4]) + struct.pack("!I", lt // 2) + bytes([59, 4]) + struct.pack("!I", lt * 7 // 8)
            opts += bytes([1, 4]) + socket.inet_aton(self.mask) + bytes([3, 4]) + socket.inet_aton(self.sip)
        opts += b"\xff"
        flags = req[10:12]
        bootp = (struct.pack("!BBBB", BOOTREPLY, 1, 6, 0) + xid + b"\0\0" + flags + b"\0" * 4 +
                 socket.inet_aton(yiaddr if mtype != 6 else "0.0.0.0") + socket.inet_aton(self.sip) + b"\0" * 4 +
                 chaddr.ljust(16, b"\0") + b"\0" * 192 + MAGIC + opts)
        bootp = bootp.ljust(300, b"\0")
        self.sock.send(frame(self.my_mac, b"\xff" * 6, self.sip, "255.255.255.255", 67, 68, bootp))

    def handle_dhcp(self, bootp: bytes):
        if len(bootp) < 240 or bootp[0] != BOOTREQUEST or bootp[236:240] != MAGIC:
            return
        xid = bootp[4:8]
        ciaddr = socket.inet_ntoa(bootp[12:16])
        chaddr = bootp[28:34]
        o = parse_opts(bootp[240:])
        mtype = o.get(53, b"\0")[0]
        cid = o.get(61, b"").hex(":") or "-"
        mac = mac_s(chaddr)
        req_ip = socket.inet_ntoa(o[50]) if 50 in o and len(o[50]) == 4 else (ciaddr if ciaddr != "0.0.0.0" else "")
        sid = socket.inet_ntoa(o[54]) if 54 in o and len(o[54]) == 4 else ""
        name = TYPES.get(mtype, str(mtype))
        if sid and sid != self.sip:
            self.log(f"{name:8} chaddr={mac} cid={cid} 给别的服务器 {sid}，不理")
            return
        mine = self.ip_for(mac)
        if mtype == 1:
            self.reply_dhcp(bootp, chaddr, xid, 2, mine)
            self.log(f"DISCOVER chaddr={mac} cid={cid} → OFFER {mine}")
        elif mtype == 3:
            if req_ip == mine:
                self.reply_dhcp(bootp, chaddr, xid, 5, mine)
                self.log(f"REQUEST  chaddr={mac} cid={cid} 请求={req_ip} → ACK")
            else:
                self.reply_dhcp(bootp, chaddr, xid, 6, "0.0.0.0")
                self.log(f"REQUEST  chaddr={mac} cid={cid} 请求={req_ip or '-'} → NAK（这个 MAC 的地址是 {mine}）")
        else:
            self.log(f"{name:8} chaddr={mac} cid={cid} 地址={req_ip or '-'}（只记日志）")

    def handle_ntp(self, src_mac: bytes, src_ip: str, sport: int, data: bytes):
        if len(data) < 48:
            return
        now = time.time() + self.ntp_offset()
        sec = int(now) + NTP_EPOCH_OFFSET
        frac = int((now % 1) * (1 << 32))
        ts = struct.pack("!II", sec, frac)
        vn = (data[0] >> 3) & 7 or 4
        pkt = (bytes([(vn << 3) | 4, 1, data[2], 0xEC]) + b"\0" * 8 + b"LOCL" + ts + data[40:48] + ts + ts)
        self.sock.send(frame(self.my_mac, src_mac, self.sip, src_ip, 123, sport, pkt))
        self.log(f"SNTP     {src_ip} → {time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(now))}（偏移 {self.ntp_offset()} s）")

    def run(self):
        self.log(f"在 {self.iface}（{mac_s(self.my_mac)}）上按 MAC 发址：{self.pool[0]}–{self.pool[-1]}，租期 {self.a.lease_s} s，"
                 f"租约存 {self.lease_path}")
        while True:
            pkt, addr = self.sock.recvfrom(4096)
            if addr[2] == socket.PACKET_OUTGOING or len(pkt) < 42:
                continue
            ihl = (pkt[14] & 0x0F) * 4
            if pkt[23] != 17:
                continue
            u = 14 + ihl
            sport, dport = struct.unpack("!HH", pkt[u:u + 4])
            data = pkt[u + 8:]
            if dport == 67:
                try:
                    self.handle_dhcp(data)
                except Exception as e:                       # 坏包 / 池满：记下，别退出
                    self.log(f"处理 DHCP 报文出错：{e!r}")
            elif dport == 123 and socket.inet_ntoa(pkt[30:34]) == self.sip:
                self.handle_ntp(pkt[6:12], socket.inet_ntoa(pkt[26:30]), sport, data)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--iface", required=True)
    ap.add_argument("--server-ip", required=True)
    ap.add_argument("--mask", default="255.255.255.0")
    ap.add_argument("--pool-start", type=int, default=100)
    ap.add_argument("--pool-end", type=int, default=199)
    ap.add_argument("--lease-s", type=int, default=3600)
    ap.add_argument("--state", default="/state")
    Server(ap.parse_args()).run()


if __name__ == "__main__":
    main()
