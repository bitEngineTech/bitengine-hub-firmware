#!/usr/bin/env python3
"""用网关 C5 的 802.15.4 射频旁听 Zigbee 信道，并在主机侧解码 + 解密（诊断固件 CONFIG_BITENGINE_DIAG_ZB_SNIFF）。

    GW_PASS=<密码> python3 tools/zb_sniff.py <网关IP> --channel 20 --seconds 30     # 录一段并解码
    GW_PASS=<密码> python3 tools/zb_sniff.py <网关IP> --fetch-only                   # 只取上一次的结果
    python3 tools/zb_sniff.py --file dump.json                                       # 解码存下来的结果

为什么有它（2026-10-07）：Sengled E11-G13 掉线后通电，MG24 每 2 s 在 MAC 层收一个单播、回一个广播，
却从不经 EZSP 上报入网事件。MG24 在网络层丢掉的帧，HA 这一侧永远看不见，只能去空口看。
C5 只是旁听（与 zigbee_channel_check 扫 Zigbee 信标同一颗射频），不碰 MG24；录制期间 Thread 暂停。

解密：网络密钥取自本机 HA 容器的 zigbee.db 最新网络备份（--key-from-ha，默认开；不打印、不落盘）。
Zigbee NWK 层安全是 AES-CCM*（M=4，安全级别在空口上被清成 0，算 MIC 时按 5 补回）。
正对照：协调器每 15 s 一次的链路状态（NWK 命令 0x08）必须能解开，否则解密结论无效。
"""
import argparse, base64, json, os, struct, subprocess, sys, time, urllib.request

try:
    from cryptography.hazmat.primitives.ciphers.aead import AESCCM
except ImportError:
    AESCCM = None

MAC_CMD = {1: 'assoc_req', 2: 'assoc_rsp', 3: 'disassoc', 4: 'data_request', 5: 'pan_conflict',
           6: 'orphan_notify', 7: 'beacon_request', 8: 'coord_realign', 9: 'gts_req'}
NWK_CMD = {1: 'route_request', 2: 'route_reply', 3: 'network_status', 4: 'leave', 5: 'route_record',
           6: 'rejoin_request', 7: 'rejoin_response', 8: 'link_status', 9: 'network_report',
           10: 'network_update', 11: 'ed_timeout_request', 12: 'ed_timeout_response', 13: 'link_power_delta'}
NWK_STATUS = {0: 'no_route', 1: 'tree_link_fail', 2: 'non_tree_link_fail', 3: 'low_battery', 4: 'no_routing_cap',
              5: 'no_indirect_cap', 6: 'indirect_expiry', 7: 'target_unavailable', 8: 'target_unallocated',
              9: 'parent_link_failure', 10: 'validate_route', 11: 'source_route_fail', 12: 'm2o_route_fail',
              13: 'address_conflict', 14: 'verify_addresses', 15: 'panid_update', 16: 'nwk_addr_update',
              17: 'bad_frame_counter', 18: 'bad_key_seq', 19: 'unknown_command'}
ED_TIMEOUT = ['10s', '2min', '4min', '8min', '16min', '32min', '64min', '128min', '256min', '512min',
              '1024min', '2048min', '4096min', '8192min', '16384min']


def gw_pass():
    p = os.environ.get('GW_PASS')
    if p:
        return p
    f = os.environ.get('GW_PASS_FILE')
    return open(f).read().strip() if f else ''


def http(gw, path, body=None, method=None):
    req = urllib.request.Request(f'http://{gw}{path}', data=body, method=method or ('POST' if body else 'GET'))
    req.add_header('Authorization', 'Basic ' + base64.b64encode((':' + gw_pass()).encode()).decode())
    if body:
        req.add_header('Content-Type', 'application/json')
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.status, r.read().decode()


def ha_network():
    """从 HA 容器读：网络密钥、PAN、nwk↔ieee 映射。密钥只留在内存里。"""
    code = r'''
import sqlite3, json
z=sqlite3.connect('file:/config/zigbee.db?mode=ro',uri=True)
b=json.loads(z.execute('select backup_json from network_backups_v15 order by id desc limit 1').fetchone()[0])
ni=b.get('network_info',b)
devs={r[0]:r[1] for r in z.execute('select ieee,nwk from devices_v15')}
print(json.dumps({'key':ni['network_key']['key'],'pan':ni.get('pan_id'),'channel':ni.get('channel'),'devs':devs}))
'''
    r = subprocess.run(['docker', 'exec', '-i', 'homeassistant', 'python3', '-'], input=code,
                       capture_output=True, text=True)
    if r.returncode == 0:
        return json.loads(r.stdout)
    # HA 容器停着（比如做"协调器长时间离线"实验时）：读宿主机上的副本。zigbee.db 是 WAL 模式，
    # 只读打开要建 -shm 会失败、而且最新改动可能还在 -wal 里 —— 连同 -wal 复制到临时目录再开。
    import shutil, sqlite3, tempfile
    src = os.environ.get('HA_ZIGBEE_DB', '/config/zigbee.db')
    with tempfile.TemporaryDirectory() as td:
        for suf in ('', '-wal'):
            if os.path.exists(src + suf):
                shutil.copy(src + suf, os.path.join(td, 'z.db' + suf))
        z = sqlite3.connect(os.path.join(td, 'z.db'))
        b = json.loads(z.execute('select backup_json from network_backups_v15 order by id desc limit 1').fetchone()[0])
        ni = b.get('network_info', b)
        devs = {row[0]: row[1] for row in z.execute('select ieee,nwk from devices_v15')}
        z.close()
    return {'key': ni['network_key']['key'], 'pan': ni.get('pan_id'), 'channel': ni.get('channel'), 'devs': devs}


class Dec:
    def __init__(self, key_hex, ieee_by_nwk):
        k = bytes.fromhex(key_hex.replace(':', ''))
        self.keys = [k, k[::-1]]
        self.ieee_by_nwk = ieee_by_nwk          # nwk(int) -> ieee bytes over-the-air order (LE)
        self.ok = None                          # (key_idx, ext_rev) 找到后固定

    def decrypt(self, aad, sec_ctl_off, nonce_ext, fc, sec_ctl, payload):
        if AESCCM is None or len(payload) < 4:
            return None
        patched = (sec_ctl & ~0x07) | 0x05
        aad = bytearray(aad)
        aad[sec_ctl_off] = patched
        combos = [self.ok] if self.ok else [(ki, rev) for ki in (0, 1) for rev in (False, True)]
        for ki, rev in combos:
            ext = nonce_ext[::-1] if rev else nonce_ext
            nonce = ext + struct.pack('<I', fc) + bytes([patched])
            try:
                pt = AESCCM(self.keys[ki], tag_length=4).decrypt(nonce, bytes(payload), bytes(aad))
                self.ok = (ki, rev)
                return pt
            except Exception:
                continue
        return None


def addr(mode, b, i):
    if mode == 2:
        return struct.unpack_from('<H', b, i)[0], i + 2
    if mode == 3:
        return b[i:i + 8], i + 8
    return None, i


def fmt_ext(e):
    return ':'.join('%02x' % x for x in e[::-1])


def parse(frame, dec, names):
    raw = bytes.fromhex(frame['hex'])
    b = raw[:-2] if len(raw) >= 5 else raw           # 去掉 FCS
    if len(b) < 3:
        return 'short'
    fcf, seq = struct.unpack_from('<HB', b, 0)
    ftype = fcf & 7
    sec = (fcf >> 3) & 1
    ackreq = (fcf >> 5) & 1
    comp = (fcf >> 6) & 1
    dmode = (fcf >> 10) & 3
    smode = (fcf >> 14) & 3
    i = 3
    dpan = None
    if dmode:
        dpan = struct.unpack_from('<H', b, i)[0]; i += 2
    dst, i = addr(dmode, b, i)
    span = dpan
    if smode and not comp:
        span = struct.unpack_from('<H', b, i)[0]; i += 2
    src, i = addr(smode, b, i)

    def nm(a):
        if a is None:
            return '-'
        if isinstance(a, bytes):
            e = fmt_ext(a)
            return names.get(e, e)
        if a == 0xFFFF:
            return 'bcast'
        return names.get(a, '0x%04X' % a)

    head = '%s→%s' % (nm(src), nm(dst))
    pan = ('pan=%04X ' % span) if span is not None else ''
    if ftype == 2:
        return f'MAC ACK seq={seq}'
    if ftype == 0:
        pl = b[i:]
        zb = ''
        if len(pl) >= 4 + 15 and pl[4] == 0:   # superframe(2) gts(1) pending(1) + Zigbee beacon payload
            p = pl[4:]
            zb = ' zigbee: stack_profile=%d router_cap=%d depth=%d ed_cap=%d ext_pan=%s' % (
                p[1] & 0x0F, (p[2] >> 2) & 1, (p[2] >> 3) & 0x0F, (p[2] >> 7) & 1, fmt_ext(p[3:11]))
        return f'MAC BEACON {pan}{head}{zb}'
    if ftype == 3:
        cmd = b[i] if i < len(b) else -1
        extra = ''
        if cmd == 1 and i + 1 < len(b):
            extra = ' cap=0x%02X' % b[i + 1]
        if cmd == 2 and i + 3 < len(b):
            extra = ' nwk=0x%04X status=%d' % (struct.unpack_from('<H', b, i + 1)[0], b[i + 3])
        return f'MAC CMD {MAC_CMD.get(cmd, cmd)} {pan}{head}{" ackreq" if ackreq else ""}{extra}'
    if ftype != 1:
        return f'MAC type{ftype} {pan}{head}'
    # ---- NWK ----
    n0 = i
    if len(b) < i + 8:
        return f'MAC DATA {pan}{head} (short nwk)'
    nfcf, ndst, nsrc, radius, nseq = struct.unpack_from('<HHHBB', b, i); i += 8
    ntype = nfcf & 3
    nsec = (nfcf >> 9) & 1
    dst_ieee = src_ieee = None
    if (nfcf >> 11) & 1:
        dst_ieee = b[i:i + 8]; i += 8
    if (nfcf >> 12) & 1:
        src_ieee = b[i:i + 8]; i += 8
    if (nfcf >> 8) & 1:
        i += 1
    if (nfcf >> 10) & 1:
        cnt = b[i]; i += 2 + 2 * cnt
    nh = 'NWK %s %s→%s r=%d%s%s' % ({0: 'DATA', 1: 'CMD', 3: 'INTERPAN'}.get(ntype, ntype), nm(nsrc), nm(ndst), radius,
                                    ' src_ieee=' + fmt_ext(src_ieee) if src_ieee else '',
                                    ' dst_ieee=' + fmt_ext(dst_ieee) if dst_ieee else '')
    if not nsec:
        pl = b[i:]
        return f'MAC DATA {pan}{head} | {nh} UNSECURED {pl.hex()}'
    sc = b[i]; sc_off = i - n0; fc = struct.unpack_from('<I', b, i + 1)[0]; i += 5
    nonce_ext = None
    if (sc >> 5) & 1:
        nonce_ext = b[i:i + 8]; i += 8
    if ((sc >> 3) & 3) == 1:
        i += 1
    if nonce_ext is None:
        nonce_ext = dec.ieee_by_nwk.get(nsrc)
    aad = b[n0:i]
    pt = dec.decrypt(aad, sc_off, nonce_ext, fc, sc, b[i:]) if nonce_ext else None
    who = (' ext=' + fmt_ext(nonce_ext)) if nonce_ext is not None and (sc >> 5) & 1 else ''
    if pt is None:
        return f'MAC DATA {pan}{head} | {nh} fc={fc}{who} [解不开]'
    if ntype == 1:
        c = pt[0]
        d = NWK_CMD.get(c, 'cmd%d' % c)
        p = pt[1:]
        if c == 3 and len(p) >= 3:
            d += ' %s dst=%s' % (NWK_STATUS.get(p[0], p[0]), nm(struct.unpack_from('<H', p, 1)[0]))
        elif c == 4 and p:
            d += ' opts=0x%02X(rejoin=%d request=%d remove_children=%d)' % (p[0], (p[0] >> 5) & 1, (p[0] >> 6) & 1, (p[0] >> 7) & 1)
        elif c == 6 and p:
            d += ' cap=0x%02X' % p[0]
        elif c == 7 and len(p) >= 3:
            d += ' nwk=0x%04X status=%d' % (struct.unpack_from('<H', p, 0)[0], p[2])
        elif c == 8 and p:
            d += ' n=%d' % (p[0] & 0x1F)
        elif c == 11 and len(p) >= 2:
            d += ' req=%s cfg=0x%02X' % (ED_TIMEOUT[p[0]] if p[0] < len(ED_TIMEOUT) else p[0], p[1])
        elif c == 12 and len(p) >= 2:
            d += ' status=%d parent_info=0x%02X' % (p[0], p[1])
        return f'{nh} fc={fc}{who} :: {d}'
    # NWK DATA → APS
    if len(pt) < 2:
        return f'{nh} fc={fc} :: aps?'
    afcf = pt[0]
    at = afcf & 3
    if at == 0 and len(pt) >= 8:
        dmode2 = (afcf >> 2) & 3
        j = 1
        dep = None
        if dmode2 in (0, 2):
            dep = pt[j]; j += 1
        cl, pr = struct.unpack_from('<HH', pt, j); j += 4
        sep = pt[j]; j += 1
        return f'{nh} fc={fc}{who} :: APS DATA prof=0x{pr:04X} cluster=0x{cl:04X} ep {sep}→{dep}{" (APS 加密)" if (afcf >> 5) & 1 else ""}'
    return f'{nh} fc={fc}{who} :: APS {["data", "cmd", "ack", "interpan"][at]} fcf=0x{afcf:02X}'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('gw', nargs='?')
    ap.add_argument('--channel', type=int)
    ap.add_argument('--seconds', type=int, default=30)
    ap.add_argument('--fetch-only', action='store_true')
    ap.add_argument('--file')
    ap.add_argument('--save')
    ap.add_argument('--all-pans', action='store_true', help='不过滤其他 PAN 的帧')
    a = ap.parse_args()

    net = ha_network()
    pan = int(net['pan'], 16) if isinstance(net['pan'], str) else net['pan']
    ch = a.channel or net['channel']
    names = {0: 'COORD'}
    ieee_by_nwk = {}
    label = {'b0:ce:18': 'SENGLED', '00:15:8d': 'AQARA', 'b4:0e:06': 'THIRDREALITY', '18:0d:f9': 'COORD'}
    for ieee, nwk in net['devs'].items():
        tag = next((v for k, v in label.items() if ieee.startswith(k)), ieee)
        names[nwk] = f'{tag}(0x{nwk:04X})'
        names[ieee] = tag
        ieee_by_nwk[nwk] = bytes.fromhex(ieee.replace(':', ''))[::-1]
    dec = Dec(net['key'], ieee_by_nwk)

    if a.file:
        d = json.load(open(a.file))
    else:
        if not a.fetch_only:
            st, r = http(a.gw, '/api/v1/diag/zb_sniff', json.dumps({'channel': ch, 'seconds': a.seconds}).encode())
            print(f'start: {st} {r}  —— 信道 {ch}，{a.seconds} s（这段时间 Thread 暂停）')
            time.sleep(a.seconds + 2)
        for _ in range(60):
            d = json.loads(http(a.gw, '/api/v1/diag/zb_sniff')[1])
            if d['state'] in ('done', 'error', 'idle'):
                break
            time.sleep(2)
    if a.save:
        json.dump(d, open(a.save, 'w'))
    print(f"state={d['state']} channel={d['channel']} frames={d['count']} dropped={d['dropped']} tx_seen={d['tx_seen']} err={d['err']!r}")
    shown = 0
    for f in d.get('frames', []):
        raw = bytes.fromhex(f['hex'])
        if len(raw) >= 5 and not a.all_pans:
            fcf = struct.unpack_from('<H', raw, 0)[0]
            if (fcf & 7) in (1, 3) and ((fcf >> 10) & 3):
                dp = struct.unpack_from('<H', raw, 3)[0]
                if dp not in (pan, 0xFFFF):
                    continue
        print('%7.3f %4d %s' % (f['t'] / 1000, f['rssi'], parse(f, dec, names)))
        shown += 1
    print(f'-- {shown} 帧（PAN 0x{pan:04X} 与广播）；解密组合 {dec.ok}')


if __name__ == '__main__':
    main()
