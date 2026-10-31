#!/usr/bin/env python3
"""数网关 mDNS 应答里到底有几条记录 —— 扩容硬停判据第 4 条。

    python3 tools/mdns_count.py [网关IP]
    python3 tools/mdns_count.py <以太网IP>,<WiFi IP> --summary [--repeat 3]   # 几百台时只看汇总；--repeat 见下

为什么需要：mdns 组件把一次应答塞进一个 1460 字节缓冲，放不下就**静默丢弃**
（不置 TC、不报错）。实测 187 字节/设备 → 第 8 台 Matter 设备起浏览应答就不完整，
外部表现是「设备配上了、Thread 网里活着、控制器里查无此人」，只能抓包发现。
tools/patch_mdns.py 的多包补丁修的就是它；本脚本是验收手段。

判据：PTR 条数必须等于实际设备数。打了补丁之后应答可能是**多个包**，
把各包的记录数加起来看。

⚠️ 必须从 5353 发查询：源端口不是 5353 时 responder 按 legacy unicast 处理，
   那条路径按设计不分包，量不到多包行为（2026-08-17 第一次测就栽在这）。
"""
import socket, struct, sys, time

import sys
# --summary：只打汇总（唯一实例数、包数、最大包），几百台时逐条打印没法看（2026-10-07，SRP 代播满载压测）
SUMMARY = '--summary' in sys.argv
# --repeat N：同一个查询背靠背发 N 次，模拟几个控制器同时浏览。网关每个**在应答的**网口各拼一次大应答。
#   ⚠️ 2026-10-07 起（tools/patch_mdns.py SVCIP，T9.34）两口同网段时固定一口应答，N 个就是 N 个大动作；
#   这之前的记录写的"2N"是当时两口都应答（或以太网地址大时以太网应答）的口径，重测时别把减半误判成回退
# 排在 mdns 任务里；之后看 /api/v1/diag/srp 的 idle_starve_max_ms（诊断固件看注册潮 GET 的 idle_gap_max_ms）
# 离 5 s 任务看门狗多远（2026-10-07，tools/patch_mdns.py SCALE ⑧）
REPEAT = int(sys.argv[sys.argv.index('--repeat') + 1]) if '--repeat' in sys.argv else 1
ARGS = [a for i, a in enumerate(sys.argv[1:], 1) if not a.startswith('--') and sys.argv[i - 1] != '--repeat']
GW = ARGS[0] if ARGS else '192.168.1.43'
# 多个地址用逗号分隔：双上行网关两个网口都可能应答
GWS = set(x.strip() for x in GW.split(',') if x.strip())
SEEN_SRC = set()

def build(name, qtype):
    pkt=b'\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00'
    for lab in name.split('.'):
        if lab: pkt+=bytes([len(lab)])+lab.encode()
    return pkt+b'\x00'+struct.pack('>HH', qtype, 1)

def rdname(d,i):
    out=[]; 
    while True:
        if i>=len(d): return '?',i
        l=d[i]
        if l==0: return '.'.join(out), i+1
        if l&0xC0==0xC0:
            p=((l&0x3F)<<8)|d[i+1]
            sub,_=rdname(d,p); out.append(sub); return '.'.join(out), i+2
        out.append(d[i+1:i+1+l].decode('utf8','replace')); i+=1+l

def query(name, qtype=12, wait=3.0):
    # ⚠️ 必须**从 5353 发**：源端口不是 5353 时 responder 按 legacy unicast 处理，
    # 那条路径按设计不分包。要测多包必须扮演正常的组播查询者。
    s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR,1)
    try: s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT,1)
    except Exception: pass
    s.bind(('',5353))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                struct.pack('4sl', socket.inet_aton('224.0.0.251'), socket.INADDR_ANY))
    s.settimeout(1.0)
    for _ in range(REPEAT):
        s.sendto(build(name,qtype), ('224.0.0.251',5353))
    wait += 2.0 * (REPEAT - 1)                # 每多一个查询网关要多拼两次大应答
    res=[]; t=time.time()
    while time.time()-t<wait:
        try: d,a=s.recvfrom(9000)
        except socket.timeout: continue
        # ⚠️ 源 IP 过滤要按**地址集合**，不能写死一个（2026-09-15 教训）：本网关是
        #    双上行、两个网口在同一网段，**重启后可能换成从另一个地址应答**。
        #    只认一个地址会把应答全丢掉，现象是"mDNS 完全不回"——极易误判成设备
        #    故障（我就这么误判过一次，还去查了固件）。多地址用逗号分隔传入。
        SEEN_SRC.add(a[0])
        if a[0] not in GWS: continue
        fl=struct.unpack('>H', d[2:4])[0]
        if not (fl & 0x8000): continue        # 只收应答
        qd,an,ns,ar=struct.unpack('>HHHH', d[4:12])
        i=12
        for _ in range(qd):
            _,i=rdname(d,i); i+=4
        recs=[]
        for _ in range(an+ns+ar):
            nm,i=rdname(d,i)
            t_,c,ttl,rl=struct.unpack('>HHIH', d[i:i+10])
            tgt = rdname(d, i+10)[0] if t_ == 12 else None     # PTR 的目标名 = 实例名
            i+=10+rl
            recs.append((nm,t_,rl,tgt))
        res.append((len(d), an, ar, recs, a[0]))
    s.close(); return res

TYPE={12:'PTR',33:'SRV',16:'TXT',28:'AAAA',1:'A',47:'NSEC'}
for nm,qt in [('_services._dns-sd._udp.local',12), ('_matter._tcp.local',12)]:
    print(f"\n===== {nm} ({TYPE.get(qt)}) =====")
    pk = query(nm,qt)
    if SUMMARY:
        # 实例 = owner 恰好是被浏览的服务名的 PTR 的目标（子类型 PTR 的 owner 带 ._sub. 不算）；
        # 双上行网关两个网口各回一份，按名字去重
        inst = {r[3] for p in pk for r in p[3] if r[1] == 12 and r[0] == nm and r[3]}
        srcs = sorted({p[4] for p in pk})
        print(f"  应答包 {len(pk)} 个（来源 {', '.join(srcs) or '无'}），最大 {max((p[0] for p in pk), default=0)} 字节，"
              f"记录合计 {sum(len(p[3]) for p in pk)} 条")
        print(f"  唯一实例 {len(inst)} 个" + (f"（其中 BE57 合成 {sum(1 for x in inst if '-BE57' in x)}）" if nm.startswith('_matter') else ''))
        continue
    for ln,an,ar,recs,_src in pk:
        print(f"  应答包 {ln} 字节  AN={an} AR={ar}  记录 {len(recs)} 条")
        for r in recs: print(f"     {TYPE.get(r[1],r[1]):5} rdlen={r[2]:3}  {r[0][:72]}")

if SEEN_SRC and not (SEEN_SRC & GWS):
    print(f"\n⚠️ 指定的 {sorted(GWS)} 一条应答都没有，但网上这些地址在应答："
          f"{sorted(SEEN_SRC)}\n   双上行网关重启后可能换网口应答 —— 把两个地址都传进来："
          f"\n   python3 tools/mdns_count.py <以太网IP>,<WiFi IP>")
