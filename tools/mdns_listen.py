#!/usr/bin/env python3
"""被动听局域网 mDNS，数网关实际发出了什么 —— SRP 代播满载压测的"宣告够不够"那一项（2026-10-07）。

    python3 tools/mdns_listen.py <网关IP[,另一个IP]> [秒数，默认 60] [--host bitengine] [--match -BE57]

一般和注册潮同时跑（先起它，再跑 `tools/srp_load.py --burst`），见 docs/test_cases_scale_baseline.md「SRP 代播满载压测」。
输出：
  - 每个来源的应答 / 查询包数、记录数、字节数（网关标 ←）；
  - 网关每个地址族里，名字含 --match 的实例各自的 SRV 被发出了几次：最少 / 中位 / 最多 + 分布。
    宣告本身每轮发三遍；局域网上有人解析新实例（HA 会）时应答里还会再带，所以中位数常比 3 大。
  - 查询最多的几个（来源, 名字, 类型）—— 看是谁在问。

为什么要它：`tools/mdns_count.py` 是主动查一次、量"答得全不全"；宣告是网关自己发的、没人问也发，
只能被动听。mdns 组件的宣告状态机原来会被普通应答包推进（patch_mdns.py SCALE ⑨ 修的），
那时有实例只被宣告了一遍 —— 这种缺陷查询永远看不出来。

⚠️ 只在一张网卡上听（默认按到网关的路由自动选）：本机常有有线 + 无线两张网卡在同一网段，两张都听会收到两份。
⚠️ 网关的 IPv6 源地址是链路本地地址，事先不知道；带了 --host 主机名（或 BE57 合成实例）记录的 IPv6 来源就认作网关。
⚠️ WiFi 上的组播会丢：某个实例只收到 1 次不一定是网关少发了，先看同一时段别的实例。
"""
import argparse, collections, select, socket, struct, subprocess, sys, time


def rdname(d, i, depth=0):
    out = []
    while True:
        if i >= len(d) or depth > 20:
            return '?', i + 1
        n = d[i]
        if n == 0:
            return '.'.join(out), i + 1
        if n & 0xC0 == 0xC0:
            sub, _ = rdname(d, ((n & 0x3F) << 8) | d[i + 1], depth + 1)
            out.append(sub)
            return '.'.join(out), i + 2
        out.append(d[i + 1:i + 1 + n].decode('utf8', 'replace'))
        i += 1 + n


def parse(d):
    fl = struct.unpack('>H', d[2:4])[0]
    qd, an, ns, ar = struct.unpack('>HHHH', d[4:12])
    i, qs, recs = 12, [], []
    for _ in range(qd):
        n, i = rdname(d, i)
        qs.append((n, struct.unpack('>H', d[i:i + 2])[0]))
        i += 4
    for _ in range(an + ns + ar):
        n, i = rdname(d, i)
        if i + 10 > len(d):
            break
        t, _c, ttl, rl = struct.unpack('>HHIH', d[i:i + 10])
        recs.append((n, t, ttl))
        i += 10 + rl
    return fl, qs, recs


def route_iface(gw):
    """到网关的路由走哪张网卡、用哪个源地址（`ip route get`）。"""
    try:
        out = subprocess.run(['ip', 'route', 'get', gw], capture_output=True, text=True, timeout=3).stdout.split()
        return out[out.index('dev') + 1], out[out.index('src') + 1]
    except Exception:
        return None, None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('gw', help='网关 IPv4，多个用逗号分隔（双上行时两个口都可能发）')
    ap.add_argument('seconds', nargs='?', type=float, default=60)
    ap.add_argument('--if', dest='ifname', help='在哪张网卡上听（默认按到网关的路由选）')
    ap.add_argument('--host', default='bitengine', help='网关主机名（认 IPv6 来源用）')
    ap.add_argument('--match', default='-BE57', help='只统计名字含这个子串的实例（默认 = srp_load.py 的合成实例）')
    a = ap.parse_args()
    gws = {x.strip() for x in a.gw.split(',') if x.strip()}

    ifname, ifaddr = route_iface(next(iter(gws)))
    ifname = a.ifname or ifname
    if not ifname or not ifaddr:
        sys.exit('找不到到网关的路由；用 --if 指定网卡')

    s4 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s4.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s4.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    s4.bind(('', 5353))
    s4.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton('224.0.0.251') + socket.inet_aton(ifaddr))
    socks = [s4]
    try:
        s6 = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        s6.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s6.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        s6.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
        s6.bind(('', 5353))
        s6.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_JOIN_GROUP,
                      socket.inet_pton(socket.AF_INET6, 'ff02::fb') + struct.pack('@I', socket.if_nametoindex(ifname)))
        socks.append(s6)
    except OSError as e:
        print(f'⚠️ IPv6 没听上（{e}），只统计 IPv4')

    host = a.host.lower()
    by_src = collections.defaultdict(lambda: [0, 0, 0, 0])   # 应答包, 应答记录, 查询包, 字节
    srv_cnt = collections.Counter()                          # (地址族, 实例名) -> SRV 被发出次数（只算网关）
    qnames = collections.Counter()
    gw6 = set()
    t0 = time.time()
    while time.time() - t0 < a.seconds:
        r, _, _ = select.select(socks, [], [], 0.5)
        for s in r:
            d, src = s.recvfrom(9000)
            src = src[0]
            if len(d) < 12:
                continue
            try:
                fl, qs, recs = parse(d)
            except Exception:
                continue
            st = by_src[src]
            st[3] += len(d)
            if not fl & 0x8000:
                st[2] += 1
                for n, qt in qs:
                    qnames[(src, n, qt)] += 1
                continue
            st[0] += 1
            st[1] += len(recs)
            if src not in gws and any(host in n.lower() or a.match in n for n, *_ in recs):
                gw6.add(src)
            if src in gws or src in gw6:
                for n, t, ttl in recs:
                    if t == 33 and ttl and a.match in n:      # SRV，且不是撤回（ttl 0）
                        srv_cnt[('IPv4' if src in gws else 'IPv6', n)] += 1

    print(f'听了 {a.seconds:.0f} s（{ifname} {ifaddr}）')
    for src, (ra, rr, qq, by) in sorted(by_src.items(), key=lambda x: -x[1][3])[:12]:
        mark = '  ← 网关' if (src in gws or src in gw6) else ''
        print(f'  {src:40} 应答 {ra:5} 包 / {rr:6} 条记录   查询 {qq:4} 包   {by / 1024:7.1f} KB{mark}')
    for fam in ('IPv4', 'IPv6'):
        c = sorted(v for (f, _n), v in srv_cnt.items() if f == fam)
        if c:
            h = collections.Counter(c)
            print(f'  网关 {fam}：含 "{a.match}" 的实例 {len(c)} 个，SRV 发出次数 最少 {c[0]} / 中位 {c[len(c) // 2]} / '
                  f'最多 {c[-1]}；分布 ' + ' '.join(f'{k}次:{h[k]}' for k in sorted(h)))
    top = sorted(qnames.items(), key=lambda x: -x[1])[:6]
    if top:
        print('  查询最多的（来源, 名字, 类型）：')
        for (src, n, qt), k in top:
            print(f'     {k:4} × {src} {n} {qt}')


if __name__ == '__main__':
    main()
