#!/usr/bin/env python3
"""给网关的 SRP server 注册 / 撤回 N 个合成主机 —— 扩容实验 D 方案的"规模"那一半。

✅ **2026-10-07 起走诊断接口**（诊断固件 CONFIG_BITENGINE_DIAG_SRP_INJECT=y）：网关把收到的 UPDATE 经 OpenThread
   内部 UDP 栈发给自己的 SRP server —— 走的是完整真实链路（SRP server → advertising proxy → mDNS → 局域网）。
   步骤与判据见 docs/test_cases_scale_baseline.md「SRP 代播满载压测」。
       GW_PASS=… python3 tools/srp_load.py <网关IP> --burst --n 250 --detail    # 主用法：注册潮（10 s 窗口内并发，模拟网关重启后）
       GW_PASS=… python3 tools/srp_load.py <网关IP> --burst --n 250 --remove    # 撤回同一批
       GW_PASS=… python3 tools/srp_load.py <网关IP> --inject --n 20             # 单条一来一回：只做功能验证
   ⚠️ `--inject` 量不到并发：两条之间 CPU 有空档，OT / mdns 任务被连续占满的那种崩溃它复现不出来（10-07 就是这么漏过一次）。
   密钥按序号确定性派生：重跑同一批 = 续租，撤回也认得出主人（SRP 是"名字先到先得 + 密钥"）。

⛔ 不带 `--inject` 的老路（从局域网直发 UDP）**仍然跑不通**，卡在"够不到 SRP server"，不是报文格式问题（2026-09-15）。
   证据链（逐条实测）：
     · 网关 OMR 地址 ping6 通（1.4ms），路由由 BR 的 RA 装上 —— **三层是通的**
     · `/api/v1/diag/srp` 拿到端口 53551、hosts=2 services=2 —— **服务器在跑**
     · 往 [OMR]:53551 发签名 UPDATE：**完全无应答**
     · 而 OT 的 `Server::ProcessMessage` 只检查 QR=0 与 opcode=5，
       `ProcessDnsUpdate` 出错**一定会回 RCODE**（srp_server.cpp:999）——
       所以"无应答"意味着**包没到那一层**，与报文格式无关
     · 往同一个 OMR 地址的 5353 发 mDNS 查询：有应答，但**来自另一台设备**
       （`fda3:...:3844:beff:fe12:3457`，正是路由表里的 RA 下一跳），网关自己没回
   推断：SRP server 活在 **OpenThread 自己的 IPv6/UDP 栈**里，而从基础设施链路
   进来的、发往 BR 自身地址的包没有被交给它（ESP-IDF 的 openthread netif glue
   与 lwIP 的分工所致）。真实 Thread 设备走 802.15.4 直接进 OT 栈，所以不受影响。
   **没有验证过的替代路径**：从 mesh-local 地址发（本机路由表里没有 ML 前缀，
   加路由要 root）、或在设备侧加一条把外来 UDP 转交给 OT 的通路（要改固件）。
   报文构造与签名部分已经写完并可复用，见下 —— 卡的是传输，不是密码学。


    python3 tools/srp_load.py <网关IP> --n 20 [--fabric 088E...] [--lease 3600]
    python3 tools/srp_load.py <网关IP> --n 0          # 全部撤回

为什么需要：`/api/v1/diag/mdns_load`（C 方案）**绕过 SRP**直接打 mDNS，
所以量不到 SRP server 的 host/service 表、租约、以及 advertising proxy 那条真实链路。
而一块 Thread 开发板只能是**一个** SRP host（OT 的 SRP client 一个实例一个 host name），
补不了规模。本脚本直接扮演 N 个 SRP 客户端，跑的是完整真实链路。

⚠️ 前提（都已实测，2026-09-15）：
  1. **局域网到 Thread 的 IPv6 路由必须通**。BR 会用 RA 把 OMR 前缀通告到局域网，
     本机 `ip -6 route` 里应有 `fd..:../64 ... proto ra`。验证：ping6 网关的 OMR 地址。
  2. **SRP server 端口必须从设备拿**：OT 在 53535-53554 里随机选一个并持久化，
     **从外部探测不出来** —— 畸形 UPDATE 被静默丢弃，关闭端口也不回 ICMPv6
     unreachable（两种探法都试过，全部沉默）。所以本脚本读 `/api/v1/diag/srp`。
  3. **OT 的 SRP server 会验签**（`srp_server.cpp: VerifySignature`，ECDSA P-256
     SHA-256 的 SIG(0)），所以每个合成 host 自带一对 P-256 密钥、自己签自己的 UPDATE。
     SRP 的信任模型是"名字先到先得"，不是 CA —— 自签是合法的。

⚠️ 签名覆盖的字节范围是照 OT 的实现逐条对的，不是照 RFC 记忆写的：
     SHA256( SIG RDATA 前 18 字节 ‖ **未压缩**的 signer name ‖ ARCOUNT-1 的 DNS 头
             ‖ 头部之后、SIG RR 之前的全部字节 )
   改这段前请重读 `srp_server.cpp` 的 `Server::VerifySignature`。
"""
import argparse, hashlib, os, random, socket, struct, sys, time, json, urllib.request, urllib.error, base64

try:
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature, Prehashed
except ImportError:
    sys.exit("需要 python3-cryptography（本仓库 tools/gbl_verify.py 也用它）")

DOMAIN = "default.service.arpa"
T_SOA, T_PTR, T_TXT, T_KEY, T_AAAA, T_SRV, T_OPT, T_SIG, T_ANY = 6, 12, 16, 25, 28, 33, 41, 24, 255
C_IN, C_NONE, C_ANY = 1, 254, 255
ALG_ECDSAP256 = 13

def enc_name(n):
    out = b''
    for lab in n.split('.'):
        if lab:
            out += bytes([len(lab)]) + lab.encode()
    return out + b'\x00'

def rr(name, rtype, rclass, ttl, rdata):
    return enc_name(name) + struct.pack('>HHIH', rtype, rclass, ttl, len(rdata)) + rdata

def key_tag(rdata):
    """RFC 4034 附录 B：算法 != 1 时按 RDATA 求和。"""
    total = 0
    for i, b in enumerate(rdata):
        total += b if i % 2 else b << 8
    total += (total >> 16) & 0xFFFF
    return total & 0xFFFF

def txt_rdata(items):
    out = b''
    for kv in items:
        b = kv.encode()
        out += bytes([len(b)]) + b
    return out

P256_ORDER = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551

def host_key(seed, i):
    """第 i 台合成主机的私钥：由 seed 与序号确定性派生。SRP 认主人靠密钥，随机生成的话下次既撤不回也续不上。"""
    d = int.from_bytes(hashlib.sha256(f"{seed}-{i}".encode()).digest(), 'big') % (P256_ORDER - 1) + 1
    return ec.derive_private_key(d, ec.SECP256R1())

def build_update(host, instance, addr16, priv, lease, keylease, port=5540,
                 txt=("SII=5000", "SAI=300", "SAT=4000"), remove=False, subtype=None, msg_id=None):
    hostn = f"{host}.{DOMAIN}"
    svc_type = f"_matter._tcp.{DOMAIN}"
    inst = f"{instance}.{svc_type}"

    pub = priv.public_key().public_numbers()
    pubbytes = pub.x.to_bytes(32, 'big') + pub.y.to_bytes(32, 'big')
    key_rdata = struct.pack('>HBB', 0, 3, ALG_ECDSAP256) + pubbytes
    tag = key_tag(key_rdata)

    ups = []
    if remove:
        # 撤回：租约置 0（SRP 的"删除"语义）。仍要带 host description 与签名。
        # ⚠️ 必须先带"服务发现指令"（类 NONE 的 PTR = 从 PTR 集合里删这一条）：OT 的 SRP server 靠它认出哪个名字
        #    是服务实例；不带的话下面那条"删实例名下全部 RRset"会被当成主机名，接着真正的主机名再来就
        #    SetFullName 冲突 → REFUSED（2026-10-07 实测，srp_server.cpp ProcessHostDescriptionInstruction）
        ups.append(rr(svc_type, T_PTR, C_NONE, 0, enc_name(inst)))
        if subtype:
            ups.append(rr(f"{subtype}._sub.{svc_type}", T_PTR, C_NONE, 0, enc_name(inst)))
        ups.append(rr(inst, T_ANY, C_ANY, 0, b''))
        ups.append(rr(hostn, T_ANY, C_ANY, 0, b''))
        key_ttl = lease          # ⚠️ KEY 的 TTL 必须非零、且与其余记录一致（srp_server.cpp LeaseTracker::ProcessTtl
        lease = keylease = 0     #    拒 0 → REFUSED）；"撤回"只靠下面 OPT 里的租约 0 表达，不靠 TTL
    else:
        ups.append(rr(svc_type, T_PTR, C_IN, lease, enc_name(inst)))          # 服务发现
        if subtype:   # Matter 设备都注册 _I<compressed fabric id> 子类型，mDNS 里多一条 PTR —— 不带就比真设备轻
            ups.append(rr(f"{subtype}._sub.{svc_type}", T_PTR, C_IN, lease, enc_name(inst)))
        ups.append(rr(inst, T_ANY, C_ANY, 0, b''))                            # 清旧 RRset
        ups.append(rr(inst, T_SRV, C_IN, lease,
                      struct.pack('>HHH', 0, 0, port) + enc_name(hostn)))
        ups.append(rr(inst, T_TXT, C_IN, lease, txt_rdata(txt)))
        ups.append(rr(hostn, T_ANY, C_ANY, 0, b''))
        ups.append(rr(hostn, T_AAAA, C_IN, lease, addr16))
    ups.append(rr(hostn, T_KEY, C_IN, key_ttl if remove else lease, key_rdata))

    zone = enc_name(DOMAIN) + struct.pack('>HH', T_SOA, C_IN)
    # EDNS0 OPT + Update Lease（选项码 2）：lease(4) + key-lease(4)
    opt_rdata = struct.pack('>HH', 2, 8) + struct.pack('>II', lease, keylease)
    opt = enc_name('.') + struct.pack('>HHIH', T_OPT, 1232, 0, len(opt_rdata)) + opt_rdata

    if msg_id is None:       # --burst 要连续的 ID（网关按 ID 直接定位应答）；ID 在签名范围内，只能在这里定
        msg_id = int.from_bytes(os.urandom(2), 'big')
    body = zone + b''.join(ups) + opt
    # ARCOUNT 含 OPT 与 SIG
    hdr_signed = struct.pack('>HHHHHH', msg_id, 5 << 11, 1, 0, len(ups), 1)   # ARCOUNT-1
    hdr_final  = struct.pack('>HHHHHH', msg_id, 5 << 11, 1, 0, len(ups), 2)

    now = int(time.time())
    sig_fixed = struct.pack('>HBBIIIH', 0, ALG_ECDSAP256, 0, 0, now + 300, now - 300, tag)
    signer = enc_name(hostn)

    h = hashlib.sha256()
    h.update(sig_fixed)      # SIG RDATA 前 18 字节
    h.update(signer)         # 未压缩的 signer name
    h.update(hdr_signed)     # ARCOUNT-1 的头
    h.update(body)           # 头之后、SIG RR 之前
    der = priv.sign(h.digest(), ec.ECDSA(Prehashed(hashes.SHA256())))
    r, s = decode_dss_signature(der)
    sig = r.to_bytes(32, 'big') + s.to_bytes(32, 'big')

    sig_rr = rr('.', T_SIG, C_ANY, 0, sig_fixed + signer + sig)
    return hdr_final + body + sig_rr

def api(gw, path, pwd):
    req = urllib.request.Request(f"http://{gw}{path}")
    req.add_header("Authorization", "Basic " + base64.b64encode(f":{pwd}".encode()).decode())
    return json.load(urllib.request.urlopen(req, timeout=10))

RCODE = {0: "NOERROR", 1: "FORMERR", 2: "SERVFAIL", 3: "NXDOMAIN", 4: "NOTIMP",
         5: "REFUSED", 6: "YXDOMAIN", 7: "YXRRSET", 8: "NXRRSET", 9: "NOTAUTH", 10: "NOTZONE"}

def send_inject(gw, pwd, msg, timeout=8):
    """经诊断接口把 UPDATE 送进网关内部的 OT UDP 栈，回 SRP server 的应答（原样字节）。"""
    req = urllib.request.Request(f"http://{gw}/api/v1/diag/srp/inject", data=msg, method="POST")
    req.add_header("Authorization", "Basic " + base64.b64encode(f":{pwd}".encode()).decode())
    req.add_header("Content-Type", "application/octet-stream")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        body = e.read()[:120].decode(errors="replace")
        if e.code == 404:
            raise RuntimeError("404：这块固件没编 CONFIG_BITENGINE_DIAG_SRP_INJECT（诊断构建才有）")
        raise RuntimeError(f"HTTP {e.code} {body}")
    except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
        # 网关没应答 / 重启了：记一次失败，交给主循环的"连续失败 3 次就停"，别让整个脚本崩掉
        raise RuntimeError(f"无应答（{getattr(e, 'reason', e)}）")

def send_udp(omr, port, msg, timeout=5):
    s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM); s.settimeout(timeout)
    try:
        s.sendto(msg, (omr, port))
        d, _ = s.recvfrom(2048)
        return d
    except socket.timeout:
        raise RuntimeError("超时（无应答）")
    finally:
        s.close()

def http(gw, pwd, path, data=None, timeout=10):
    req = urllib.request.Request(f"http://{gw}{path}", data=data, method="POST" if data is not None else "GET")
    req.add_header("Authorization", "Basic " + base64.b64encode(f":{pwd}".encode()).decode())
    if data is not None:
        req.add_header("Content-Type", "application/octet-stream")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)

def run_burst(a, msgs):
    """注册潮：整批交给网关，由它的后台任务按时间表发（POST /api/v1/diag/srp/burst），这边只轮询。
    计划时刻在 [0, window) 里均匀随机 —— OT SRP client 发现服务器重启后的重注册抖动上限就是 10 s
    （srp_client.hpp kMaxTxJitterOnServerRestart）。为什么不能用单条注入代替：一来一回之间 CPU 有空档，
    看不到 OT 任务 / mdns 任务被连续占满，而那正是任务看门狗 panic 的形状。"""
    rnd = random.Random(f"{a.seed}-{a.start}-{a.n}-{a.window}")
    ats = sorted(int(rnd.uniform(0, a.window * 1000)) for _ in msgs)
    body = b''.join(struct.pack('>IH', at, len(m)) + m for at, m in zip(ats, msgs))
    try:
        up0 = api(a.gw, "/api/v1/status", a.pwd).get("uptime")
    except Exception:  # noqa: BLE001
        up0 = None
    try:
        r = http(a.gw, a.pwd, f"/api/v1/diag/srp/burst?timeout_ms={a.timeout * 1000}", data=body, timeout=60)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            sys.exit("404：这块固件没有 /api/v1/diag/srp/burst（要 CONFIG_BITENGINE_DIAG_SRP_INJECT 的新诊断构建）")
        sys.exit(f"提交失败：HTTP {e.code} {e.read()[:120].decode(errors='replace')}")
    print(f"注册潮：{r.get('n')} 条，{a.window:g} s 窗口内随机发出（{len(body) / 1024:.0f} KB），开机 {up0} s 时开始")
    t0 = time.time(); st = {}; dead = 0; prev = None
    while True:
        time.sleep(1)
        try:
            st = http(a.gw, a.pwd, "/api/v1/diag/srp/burst", timeout=5); dead = 0
        except Exception as e:  # noqa: BLE001
            dead += 1
            print(f"  {time.time() - t0:5.1f} s  网关无应答（{getattr(e, 'reason', e)}）")
            if dead >= 5:
                break
            continue
        # 各任务 CPU：两次轮询之间各任务累计运行时间之差 / 全部任务之差（单核，总和 = 墙钟）
        cpu = ""
        cur = {t[0]: t[2] for t in st.get("tasks", [])}
        if prev and cur:
            d = {k: (v - prev.get(k, v)) & 0xFFFFFFFF for k, v in cur.items()}
            tot = sum(d.values()) or 1
            cpu = "  CPU " + " ".join(f"{k} {v * 100 // tot}%" for k, v in sorted(d.items(), key=lambda kv: -kv[1])[:4])
        prev = cur
        print(f"  {time.time() - t0:5.1f} s  送出 {st['sent']}/{st['n']}  应答 {st['responded']}  RCODE {st['rcode']}"
              f"  OT 锁最长等 {st['ot_lock_wait_max_ms']} ms  IDLE 最长被饿 {st['idle_gap_max_ms']} ms{cpu}", flush=True)
        if not st["running"]:
            break
    try:
        up1 = api(a.gw, "/api/v1/status", a.pwd).get("uptime")
    except Exception:  # noqa: BLE001
        up1 = None
    if up0 is not None and up1 is not None and up1 < up0:
        print(f"⚠️ 网关重启过：开始时开机 {up0} s，现在 {up1} s —— 去 /api/v1/diag/coredump 取现场")
        return
    if not st:
        return
    lat = st["lat_ms"]
    print(f"结果：送出 {st['sent']}/{st['n']}（失败 {st['send_fail']}，缓冲不够重试 {st['retries']} 次），"
          f"应答 {st['responded']}，RCODE {st['rcode']}")
    print(f"  送出→应答 p50 {lat['p50']} / p95 {lat['p95']} / 最长 {lat['max']} ms；实际送出最多比计划晚 {st['send_lag_max_ms']} ms")
    print(f"  OT 锁最长等 {st['ot_lock_wait_max_ms']} ms（≈ OT 任务最长一次连续占用）；"
          f"IDLE 最长被饿 {st['idle_gap_max_ms']} ms（任务看门狗 5000 ms 就 panic）")
    mc = {k: v for k, v in st.get("mdns_calls", {}).items() if v[0]}
    if mc:
        print("  代播调 mdns（次数 / 累计 ms / 最长 ms）：" +
              "；".join(f"{k} {v[0]}/{v[1]}/{v[2]}" for k, v in sorted(mc.items(), key=lambda kv: -kv[1][1])))
    ma = st.get("mdns_actions", {})
    if ma:
        print("  mdns 任务各类动作（次数 / 累计 ms / 最长 ms）：" +
              "；".join(f"{k} {v[0]}/{v[1]}/{v[2]}" for k, v in sorted(ma.items(), key=lambda kv: -kv[1][2])))
    for t in st.get("tasks", []):
        if t[0] in ("mdns", "ot_main", "srp_readv"):
            print(f"  任务 {t[0]}：栈最少还剩 {t[3]} 字节")
    if a.detail:
        d = http(a.gw, a.pwd, "/api/v1/diag/srp/burst?detail=1", timeout=15)
        sl = d.get("slots", [])
        k = max(1, len(sl) // 10)
        print("  按送出顺序每 10% 一段：条数 / 延迟中位 / 最长 / 非 0 RCODE")
        for j in range(0, len(sl), k):
            seg = sl[j:j + k]
            ls = sorted(r_ - s_ for _, s_, r_, _ in seg if s_ >= 0 and r_ >= 0)
            bad = sum(1 for *_, rc in seg if rc not in (0,))
            if ls:
                print(f"    [{j:4d}..{j + len(seg) - 1:4d}] {len(seg):4d}  {ls[len(ls) // 2]:6d}  {ls[-1]:6d}  {bad}")
            else:
                print(f"    [{j:4d}..{j + len(seg) - 1:4d}] {len(seg):4d}  （无应答）  {bad}")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gw"); ap.add_argument("--n", type=int, required=True, help="主机个数（序号 start..start+n-1）")
    ap.add_argument("--start", type=int, default=0)
    ap.add_argument("--remove", action="store_true", help="撤回这批（要与注册时同一个 --seed）")
    ap.add_argument("--inject", action="store_true", help="经诊断接口注入（唯一跑得通的路，见头注）")
    ap.add_argument("--fabric", default="088E927C6F07E0AC")
    ap.add_argument("--no-subtype", action="store_true", help="不注册 _I<fabric> 子类型（默认注册，同真设备）")
    ap.add_argument("--seed", default="bitengine-srp-load")
    ap.add_argument("--lease", type=int, default=3600)
    ap.add_argument("--pass", dest="pwd", default=os.environ.get("GW_PASS", ""))
    ap.add_argument("--omr", default=None, help="网关 OMR 地址（默认从 /node/ipaddrs 找；合成主机的地址也用它的前缀）")
    ap.add_argument("--every", type=int, default=50, help="每多少条报一次进度（带用时，找出事那一刻用）")
    ap.add_argument("--pace", type=float, default=0, help="每条之间停多少秒（默认 0 = 一条应答完立刻发下一条）")
    ap.add_argument("--burst", action="store_true",
                    help="注册潮：整批交给网关后台按时间表并发地发（诊断固件），模拟网关重启后设备成批重注册")
    ap.add_argument("--window", type=float, default=10.0, help="--burst 的时间窗（秒），默认 10 = OT 的重注册抖动上限")
    ap.add_argument("--timeout", type=int, default=30, help="--burst：最后一条送出后最多再等应答多少秒")
    ap.add_argument("--detail", action="store_true", help="--burst：结束后按送出顺序分段打印延迟，看它随条数怎么涨")
    a = ap.parse_args()
    if not a.pwd: sys.exit("要密码：--pass 或 GW_PASS=")
    if a.n < 0 or a.start < 0: sys.exit("--n / --start 不能为负")

    srp = api(a.gw, "/api/v1/diag/srp", a.pwd)
    port = srp["port"]
    print(f"SRP server: 端口 {port}，当前 hosts={srp['hosts']} services={srp['services']}")

    omr = a.omr
    if not omr:
        for ip in api(a.gw, "/node/ipaddrs", a.pwd):
            ip = ip.split('/')[0]
            # OMR：全局可路由的 fd:: 前缀，排除 mesh-local（含 :0:ff:fe00: 的 RLOC 形状）
            if ip.startswith("fd") and ":0:ff:fe00:" not in ip and not ip.startswith("fe80"):
                omr = ip; break
    if not omr and not (a.inject or a.burst): sys.exit("找不到 OMR 地址，用 --omr 指定")
    prefix = socket.inet_pton(socket.AF_INET6, omr)[:8] if omr else bytes.fromhex("fd16f00dbe570001")
    print(f"传输: {'诊断接口注册潮' if a.burst else '诊断接口注入' if a.inject else f'UDP → [{omr}]:{port}'}；"
          f"{'撤回' if a.remove else '注册'} 序号 {a.start}..{a.start + a.n - 1}")

    if a.burst:
        if a.n > 1024: sys.exit("--burst 一次最多 1024 条（THREAD_BR_DIAG_BURST_MAX）")
        id0 = int.from_bytes(os.urandom(2), 'big')
        msgs = []
        for k, i in enumerate(range(a.start, a.start + a.n)):
            host = f"BE57{i:012X}"
            msgs.append(build_update(host, f"{a.fabric}-{host}", prefix + b"\xbe\x57\x00\x00" + struct.pack(">I", i),
                                     host_key(a.seed, i), a.lease, a.lease * 2, remove=a.remove,
                                     subtype=None if a.no_subtype else f"_I{a.fabric}", msg_id=(id0 + k) & 0xFFFF))
        run_burst(a, msgs)
        time.sleep(2)
        try:
            after = api(a.gw, "/api/v1/diag/srp", a.pwd)
            print(f"服务器侧：hosts={after['hosts']} services={after['services']}")
        except Exception as e:  # noqa: BLE001
            print(f"服务器侧：读不到（{e}）")
        return

    ok = err = streak = 0
    t_start = time.time()
    for i in range(a.start, a.start + a.n):
        host = f"BE57{i:012X}"
        inst = f"{a.fabric}-BE57{i:012X}"
        addr = prefix + b"\xbe\x57\x00\x00" + struct.pack(">I", i)
        msg = build_update(host, inst, addr, host_key(a.seed, i), a.lease, a.lease * 2,
                           remove=a.remove, subtype=None if a.no_subtype else f"_I{a.fabric}")
        try:
            d = send_inject(a.gw, a.pwd, msg) if a.inject else send_udp(omr, port, msg)
            rc = struct.unpack('>H', d[2:4])[0] & 0xF if len(d) >= 4 else -1
            if rc == 0:
                ok += 1; streak = 0
            else:
                err += 1; streak += 1
                print(f"  [{i}] RCODE={rc} {RCODE.get(rc,'?')}")
        except RuntimeError as e:
            err += 1; streak += 1
            print(f"  [{i}] {e}（第 {time.time() - t_start:.1f} s）")
        if streak >= 3:
            print("  连续失败 3 次，停"); break
        if a.every > 0 and (i - a.start + 1) % a.every == 0:
            print(f"  … {i - a.start + 1}/{a.n}，{(time.time() - t_start):.1f} s", flush=True)
        if a.pace > 0:
            time.sleep(a.pace)
    dt = time.time() - t_start
    print(f"成功 {ok} / 失败 {err}，用时 {dt:.1f} s（每条 {dt / max(ok + err, 1) * 1000:.0f} ms）")
    time.sleep(2)
    try:
        after = api(a.gw, "/api/v1/diag/srp", a.pwd)
        print(f"服务器侧：hosts={after['hosts']} services={after['services']}")
    except Exception as e:  # noqa: BLE001 —— 网关可能刚重启
        print(f"服务器侧：读不到（{e}）")
    try:
        rv = api(a.gw, "/api/v1/thread/readvertise", a.pwd)
        print(f"代播跟踪：tracked={rv.get('tracked')} 扫描持锁 {rv.get('scan_lock_us')} us"
              f"（最长 {rv.get('scan_lock_max_us')} us），本轮见到 {rv.get('scan_services')} 个服务")
    except Exception as e:  # noqa: BLE001 —— 老固件没有这几个字段
        print(f"代播跟踪：读不到（{e}）")

if __name__ == "__main__":
    main()
