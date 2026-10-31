#!/bin/bash
# tools/test_whitelist.sh —— TCP 白名单**实际拦截效果**验证（TC-W01）
#
#   bash tools/test_whitelist.sh [网关IP] [本机IP]
#
# 为什么单独一个脚本：test_api.sh 只验证了配置读写和 enabled/active 三态上报，
# 那只证明"能存能读"。而白名单存在的**全部意义**是"真的拦得住" —— 6638 端口
# 没有任何认证（EZSP 协议没有，桥又是纯字节流透传），局域网里任何能连上它的
# 东西都拿到 Zigbee 网络的完全控制权，包括开锁。这条必须单独证。
#
# ⚠️ 两个判据上的坑，改这个脚本前先读：
#
# 1. **判据必须在服务端，客户端侧已经分不出来了**（2026-09-23 改）。
#    拒绝路径现在是 accept 之后挂住、**不主动关**（tcp_server.c 的 reject_hold_locked，
#    因为立刻 close 会让 ZHA 永久卡死，见 docs/guide/ZHA连接不自愈.md 第九节）。
#    于是从客户端看，"被拒"和"被放行"长得一模一样：都是连上、没有任何数据。
#    · `nc -z` 不行：握手本来就会完成，恒报"成功"。
#    · 旧判据"立刻 EOF = 被拒"也不行：现在恒不成立，会以**"白名单不拦了"**
#      这个最吓人的形状报红。
#    现在用的判据：连上并保持，同时问服务端 `/zigbee/status` 有没有**新登记**的
#    客户端条目（uptime 很小的那种）。被拒的连接永远进不了客户端表。
#
# 2. **放行路径没法从已被桥占用的 IP 上验证**。tcp_server.c 有"同 IP 新连接
#    踢掉旧连接"的规则（为 ZHA 重连准备的）。若本机就是 ZHA 所在的机器，
#    探测连接会踢掉 ZHA，ZHA 立刻重连又踢掉探测，探测拿到 EOF 被误判为"被拒"。
#    所以放行路径改为**观察 ZHA 自己**：把它的 IP 加进名单后它仍在线即为通过。

set -u
GW="${1:-bitengine.local}"
ME="${2:-$(hostname -I | awk '{print $1}')}"
PORT=6638
PASS=0; FAIL=0

ok()  { echo "  ✅ $1"; PASS=$((PASS+1)); }
bad() { echo "  ❌ $1"; echo "       期望 $2 / 实际 $3"; FAIL=$((FAIL+1)); }
eq()  { [ "$2" == "$3" ] && ok "$1" || bad "$1" "$2" "$3"; }


# 设备设了密码时必须给凭据，否则每个请求都是 401，断言会以"值不对"的形式一片红，
# 看起来像功能坏了。用户名留空，固件只验冒号后那半（auth.c 的 check_basic_auth）。
#   GW_PASS=<设备密码> bash tools/test_whitelist.sh <网关IP>
# ⚠️ -u 会把密码放进 argv，同机其他用户 ps 得到；这些是本地调试/产线工具，接受。
AUTH=()
[ -n "${GW_PASS:-}" ] && AUTH=(-u ":$GW_PASS")

# 401 预检：必须在任何断言之前。curl 收到 401 也返回 0，所以不能靠退出码探。
_sc=$(curl -s -m 10 ${AUTH[@]+"${AUTH[@]}"} -o /dev/null -w '%{http_code}' \
      "http://$GW/api/v1/status")
if [ "$_sc" == "401" ]; then
    echo "❌ 需要认证（HTTP 401）。设备设了密码，请带上凭据重跑："
    echo "     GW_PASS=<设备密码> bash tools/test_whitelist.sh <网关IP>"
    exit 1
fi
api()  { curl -s -m 10 ${AUTH[@]+"${AUTH[@]}"} "http://$GW/api/v1$1"; }
apip() { curl -s -m 10 ${AUTH[@]+"${AUTH[@]}"} -X POST "http://$GW/api/v1$1" -d "$2"; }

clients() { api /zigbee/status | python3 -c "
import json,sys
print(','.join(c['ip'] for c in json.load(sys.stdin).get('clients',[])))"; }

# 连上去看能不能维持：EOF=被拒，超时/有数据=放行
probe() { python3 -c "
import socket,sys
s=socket.socket(); s.settimeout(4)
try: s.connect(('$GW',$PORT))
except Exception: print('refused'); sys.exit()
try:
    s.settimeout(2.5)
    print('rejected' if s.recv(64)==b'' else 'allowed')
except socket.timeout: print('allowed')
finally: s.close()"; }

# 连上去并**保持** $1 秒；首行输出 connected / refused。
# 保持是必须的：判据靠"连接还在的时候去问服务端"，连完就断什么也看不到。
hold_open() { python3 -c "
import socket,sys,time
s=socket.socket(); s.settimeout(4)
try: s.connect(('$GW',$PORT))
except Exception: print('refused',flush=True); sys.exit()
print('connected',flush=True)
time.sleep(float(sys.argv[1])); s.close()" "$1"; }

# 刚被登记的客户端条目数（uptime = 已连接秒数）。被拒的连接永远不进这张表。
fresh() { api /zigbee/status | python3 -c "
import json,sys
print(sum(1 for c in json.load(sys.stdin).get('clients',[]) if c.get('uptime',9999)<=5))"; }

restore() {
    apip /whitelist '{"enabled":false,"ips":[]}' > /dev/null
}
trap restore EXIT INT TERM

echo "=== 白名单实际拦截验证 @ $GW（本机 $ME）==="
BEFORE=$(clients)
echo "  测试前桥上客户端: [$BEFORE]"

# ---- ① 拒绝路径 ----
apip /whitelist '{"enabled":true,"ips":["10.99.99.99"]}' > /dev/null
sleep 1
# 等"新鲜条目"老化，免得把刚重连回来的 ZHA 当成我们的探测连接
for _ in 1 2 3 4 5 6 7 8 9 10; do [ "$(fresh)" == "0" ] && break; sleep 1; done
HOLDLOG=$(mktemp)
hold_open 8 > "$HOLDLOG" &
HOLDPID=$!
sleep 2.5
eq "TCP 握手仍会完成（拒绝发生在 accept 之后）" "connected" "$(head -1 "$HOLDLOG")"
eq "名单不含本机 → 没有被登记为桥客户端（服务端判据）" "0" "$(fresh)"
wait "$HOLDPID" 2>/dev/null
rm -f "$HOLDLOG"

# 服务端应留下拒绝日志（用 /logs 的 tail 语义交叉验证）
sleep 1
HIT=$(api "/logs?limit=60" | python3 -c "
import json,sys
e=[x for x in json.load(sys.stdin)['entries'] if 'hitelist' in x['msg']]
print(e[-1]['msg'] if e else '')")
[ -n "$HIT" ] && ok "服务端记录了拒绝日志：$HIT" || bad "服务端记录了拒绝日志" "有" "无"

# ---- ② 已建立的连接不受影响（accept 时才查）----
eq "启用白名单不踢掉已有连接" "$BEFORE" "$(clients)"

# ---- ③ 放行路径：把 ZHA 的 IP 加进名单，观察它 ----
if [ -n "$BEFORE" ]; then
    apip /whitelist "{\"enabled\":true,\"ips\":[\"${BEFORE%%,*}\"]}" > /dev/null
    sleep 2
    eq "名单含 ZHA IP → 它仍在线" "$BEFORE" "$(clients)"
else
    echo "  ⏭  桥上无客户端，跳过放行路径（需要一个活的 Z2M/ZHA 连接）"
fi

# ---- ④ 关闭后恢复放行 ----
#
# ⚠️ 这一条**不能用 probe()**（2026-08-17 上板发现）。文件头第 2 条说的同 IP 互踢
#    对拒绝路径无害（①里探测本来就该被拒），但对放行路径是致命的假阴性：
#    探测连上 → 踢掉 ZHA → ZHA 立刻重连 → 踢掉探测 → 探测读到 EOF → 报"rejected"。
#    ③ 已经因此改成"观察 ZHA"，④ 当时漏改了，于是在**唯一能跑它的那台机器上**
#    （ZHA 所在的机器）必然红一条，看起来像固件放行坏了。
#
# 判据换成"固件有没有拒过"：白名单关闭时 tcp_server 走的是
#     if (!wl->enabled || wl->count == 0) → 直接放行，一行日志都不打
# 所以只要探测之后没有新的 "Whitelist: rejecting"，就证明它没被白名单拦。
# 这个判据对同 IP 互踢免疫 —— 被踢掉产生的是 "Evicting stale client"，不是拒绝。
restore
sleep 1

SEQ_BEFORE=$(api "/logs?limit=1" | python3 -c "import json,sys;print(json.load(sys.stdin)['next_seq'])")
probe > /dev/null          # 只为触发一次 accept，结果本身不作判据
sleep 2
REJ=$(api "/logs?limit=60" | python3 -c "
import json,sys
d=json.load(sys.stdin)
new=[x for x in d['entries'] if x['seq'] >= $SEQ_BEFORE and 'hitelist' in x['msg'] and 'eject' in x['msg']]
print(len(new))")
eq "白名单关闭 → 固件不再拒绝任何连接" "0" "$REJ"

# 顺带确认状态确实是关的（判据的前提）
eq "白名单已关闭" "false" "$(api /whitelist | python3 -c "import json,sys;print(str(json.load(sys.stdin)['enabled']).lower())")"

sleep 2
AFTER=$(clients)
echo "  测试后桥上客户端: [$AFTER]"
# ④ 的探测会因同 IP 规则踢掉 ZHA，它应当自行重连回来
eq "ZHA 已重新连上" "yes" "$([ -n "$AFTER" ] && echo yes || echo no)"

echo ""
echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
