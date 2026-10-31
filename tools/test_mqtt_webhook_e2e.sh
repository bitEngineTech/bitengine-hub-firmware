#!/bin/bash
# tools/test_mqtt_webhook_e2e.sh —— MQTT 与 Webhook 的**端到端**验证（需要一块在网的板子）
#
#   GW_PASS=<密码> bash tools/test_mqtt_webhook_e2e.sh <网关IP> [--webhook-wait] [--port 18883]
#
# 为什么需要这个：test_api.sh 对 MQTT/Webhook 只验了"能存能读、非法值被拒"，
# 没有一条证明**网关真的连上了 broker、HA 真的能发现它、webhook 真的发得出去**
# （feature_gap_audit D3）。2026-09-05 第一次真发之前，webhook 任务的栈是 3072 字节、
# 空闲余量 1996 —— 第一条 HTTP POST 就可能把设备打崩。这类问题只有让它真发一次才看得见。
#
# 做法：在本机用 docker 起一个匿名 Mosquitto（默认 18883 端口，不碰你自己的 1883）
# 和一个 python HTTP 接收端（18080），把网关指过来，断言收到的东西。
#
#   --webhook-wait   等调度器真的发一条 config_backup（backup_interval 最小 300s，
#                    scheduler 每 60s 才检查一次，首次要先初始化时间戳 → 约 6-7 分钟）。
#                    不加此项只验 webhook 配置接受，不验真实发送。
#
# ⚠️ 会临时修改网关的 MQTT / Webhook 配置。开头自动备份，结束（含失败/Ctrl-C）自动还原。
# ⚠️ 需要 docker、python3、curl；网关必须能反向访问本机（同一网段 / 无防火墙拦截）。

set -u
GW="${1:-bitengine.local}"
WAIT_WEBHOOK=0; BROKER_PORT=18883; HOOK_PORT=18080
shift || true
while [ $# -gt 0 ]; do
    case "$1" in
        --webhook-wait) WAIT_WEBHOOK=1 ;;
        --port)         BROKER_PORT="$2"; shift ;;
    esac
    shift
done

PASS=0; FAIL=0
AUTH=()
[ -n "${GW_PASS:-}" ] && AUTH=(-u ":$GW_PASS")
C()    { curl -s -m 10 ${AUTH[@]+"${AUTH[@]}"} "$@"; }
api()  { C "http://$GW/api/v1$1"; }
apip() { C -X POST "http://$GW/api/v1$1" -d "$2"; }
jq_()  { python3 -c "import json,sys
try: d=json.load(sys.stdin)
except Exception: print('<非JSON>'); sys.exit()
for k in '$1'.split('.'): d = d.get(k) if isinstance(d,dict) else None
print(d if isinstance(d,str) else json.dumps(d))"; }
ok()  { echo "  ✅ $1"; PASS=$((PASS+1)); }
bad() { echo "  ❌ $1"; echo "       期望: $2"; echo "       实际: $3"; FAIL=$((FAIL+1)); }
eq()  { [ "$2" == "$3" ] && ok "$1" || bad "$1" "$2" "$3"; }

for t in docker python3 curl; do
    command -v $t >/dev/null || { echo "❌ 需要 $t"; exit 1; }
done

# 网关必须是 IP：要算出本机朝它那一侧的地址
GWIP=$(getent ahostsv4 "$GW" | awk 'NR==1{print $1}')
[ -z "$GWIP" ] && { echo "❌ 解析不了 $GW"; exit 1; }
ME=$(ip -4 route get "$GWIP" | awk '{for(i=1;i<=NF;i++) if($i=="src") print $(i+1)}' | head -1)
[ -z "$ME" ] && { echo "❌ 算不出本机朝 $GWIP 的地址"; exit 1; }

WORK="$(mktemp -d /tmp/bitengine-e2e.XXXX)"
BACKUP="$WORK/config.json"
HOOKLOG="$WORK/hook.jsonl"
CT="bitengine-e2e-mosquitto"
PREFIX="e2etest"
LISTENER_PID=""
RESTORED=0

cleanup() {
    [ $RESTORED -eq 1 ] && return
    RESTORED=1
    echo ""; echo "── 还原与清理 ──"
    r=""
    if [ -s "$BACKUP" ]; then
        r=$(C -X POST "http://$GW/api/v1/config/import" --data-binary @"$BACKUP" | jq_ status)
        [ "$r" == "ok" ] && echo "  ✅ 网关配置已还原" || echo "  ⚠️  还原失败！备份仍在 $BACKUP"
    fi
    [ -n "$LISTENER_PID" ] && kill "$LISTENER_PID" 2>/dev/null
    docker rm -f "$CT" >/dev/null 2>&1
    [ "$r" == "ok" ] && rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

echo "=== bitEngine MQTT/Webhook 端到端 @ $GW（本机 $ME）==="
code=$(C -o /dev/null -w '%{http_code}' "http://$GW/api/v1/mqtt/status")
[ "$code" == "401" ] && { echo "  ❌ 401：设备设了密码，用 GW_PASS=<密码> 重跑"; exit 1; }
[ "$code" != "200" ] && { echo "  ❌ 连不上 $GW（HTTP $code）"; exit 1; }
C "http://$GW/api/v1/config/export" -o "$BACKUP"
[ -s "$BACKUP" ] || { echo "  ❌ 备份配置失败，不继续"; exit 1; }
echo "  已备份配置 → $BACKUP"

# ── 本机测试桩 ──
printf 'listener 1883 0.0.0.0\nallow_anonymous true\n' > "$WORK/mosquitto.conf"
docker rm -f "$CT" >/dev/null 2>&1
docker run -d --name "$CT" -p "$BROKER_PORT:1883" \
    -v "$WORK/mosquitto.conf:/mosquitto/config/mosquitto.conf:ro" eclipse-mosquitto:2 >/dev/null \
    || { echo "  ❌ 起不了 Mosquitto 容器（端口 $BROKER_PORT 被占？用 --port 换）"; exit 1; }
sub() { docker exec "$CT" mosquitto_sub -h 127.0.0.1 "$@" 2>/dev/null; }

python3 - "$HOOK_PORT" "$HOOKLOG" <<'EOF' &
import json, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
port, log = int(sys.argv[1]), sys.argv[2]
class H(BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get('Content-Length') or 0)
        body = self.rfile.read(n).decode('utf-8', 'replace') if n else ''
        with open(log, 'a') as f:
            f.write(json.dumps({"peer": self.client_address[0], "path": self.path,
                                "ctype": self.headers.get('Content-Type'), "body": body}) + "\n")
        self.send_response(200); self.send_header('Content-Length', '2'); self.end_headers()
        self.wfile.write(b'ok')
    def log_message(self, *a): pass
ThreadingHTTPServer(('0.0.0.0', port), H).serve_forever()
EOF
LISTENER_PID=$!
sleep 2

# ─────────────────────────────── MQTT ───────────────────────────────
echo ""; echo "── MQTT ──"
r=$(apip /mqtt/config "{\"enabled\":true,\"broker\":\"$ME\",\"port\":$BROKER_PORT,\"username\":\"\",\"password\":\"\",\"topic_prefix\":\"$PREFIX\",\"publish_interval\":10,\"tls_enabled\":false}")
eq "配置被接受（enabled=true）" "true" "$(echo "$r" | jq_ enabled)"

conn=false
for i in $(seq 1 15); do
    sleep 2
    conn=$(api /mqtt/status | jq_ connected)
    [ "$conn" == "true" ] && break
done
eq "30s 内连上本机 broker" "true" "$conn"

sleep 3   # 让 retained 的 discovery 全部落到 broker
DISC=$(sub -t 'homeassistant/#' -W 3 -F '%r %t %p')
eq "HA discovery 恰好 6 条且全为 retained" "6" "$(echo "$DISC" | grep -c '^1 homeassistant/')"
eq "discovery 载荷全部是合法 JSON 且含 stat_t/uniq_id/dev.ids" "ok" "$(echo "$DISC" | python3 -c "
import sys, json
ids=set(); bad=0
for line in sys.stdin:
    parts=line.strip().split(' ',2)
    if len(parts)<3: continue
    try:
        d=json.loads(parts[2]); assert d['stat_t'].startswith('$PREFIX/'); assert d['dev']['ids']
        assert d['uniq_id'] not in ids; ids.add(d['uniq_id'])
    except Exception as e: bad+=1
print('ok' if bad==0 and len(ids)==6 else 'bad=%d ids=%d'%(bad,len(ids)))")"
eq "binary_sensor(mg24_alive) 走 binary_sensor 组件" "1" "$(echo "$DISC" | grep -c 'homeassistant/binary_sensor/.*/mg24_alive/config')"
eq "在线状态 <prefix>/status 为 retained online" "1 $PREFIX/status online" "$(sub -t "$PREFIX/status" -C 1 -W 3 -F '%r %t %p')"

SENS=$(timeout 15 docker exec "$CT" mosquitto_sub -h 127.0.0.1 -t "$PREFIX/sensor/#" -F '%t %p' 2>/dev/null)
for s in temperature free_memory wifi_rssi uptime mg24_alive fw_version; do
    eq "15s 内收到 sensor/$s" "yes" "$(echo "$SENS" | grep -q "^$PREFIX/sensor/$s " && echo yes || echo no)"
done
eq "temperature 是数字" "yes" "$(echo "$SENS" | awk '$1=="'$PREFIX'/sensor/temperature"{print $2; exit}' | grep -Eq '^-?[0-9]+(\.[0-9]+)?$' && echo yes || echo no)"
eq "mg24_alive 是 HA binary_sensor 默认的 ON/OFF" "yes" "$(echo "$SENS" | awk '$1=="'$PREFIX'/sensor/mg24_alive"{print $2; exit}' | grep -Eq '^(ON|OFF)$' && echo yes || echo no)"

# ─────────────────────────────── Webhook ───────────────────────────────
echo ""; echo "── Webhook ──"
UP0=$(api /status | jq_ uptime)
ST0=$(api /diag/watchdog | python3 -c "import json,sys; print([t['stack_left'] for t in json.load(sys.stdin)['tasks'] if t['name']=='webhook'][0])" 2>/dev/null)
r=$(apip /webhook/config "{\"enabled\":true,\"url\":\"http://$ME:$HOOK_PORT/e2e\",\"backup_interval\":300,\"event_zigbee_crash\":true,\"event_network_down\":true,\"event_interference\":true,\"event_ota\":true,\"event_temp_high\":true,\"event_memory_low\":true}")
eq "配置被接受（enabled=true, backup=300）" "true 300" "$(echo "$r" | jq_ enabled) $(echo "$r" | jq_ backup_interval)"

if [ $WAIT_WEBHOOK -eq 1 ]; then
    synced=$(api /ntp | jq_ synced)
    [ "$synced" != "true" ] && echo "  ⚠️  NTP 未同步：scheduler 不会跑 check_auto_backup，下面必然超时"
    echo "  等待调度器发出 config_backup（最多 8 分钟，通常 6-7 分钟）..."
    got=""
    for i in $(seq 1 96); do
        sleep 5
        got=$(grep -m1 '"event\\":\\"config_backup' "$HOOKLOG" 2>/dev/null)
        [ -n "$got" ] && break
    done
    eq "本机收到 config_backup POST" "yes" "$([ -n "$got" ] && echo yes || echo no)"
    if [ -n "$got" ]; then
        eq "Content-Type 是 application/json" "application/json" "$(echo "$got" | jq_ ctype)"
        # 双上行且两口同网段时，lwip 对"目的地在本网段"的包按 netif 链表顺序选出口，
        # 不看默认路由 —— 实测 active_interface=eth 而 POST 从 WiFi 地址发出。所以只要
        # 来源是网关的任一地址就算对，不能钉死在访问它用的那个 IP 上。
        eq "来源是网关的某个地址（eth 或 wifi）" "yes" "$(api /status | python3 -c "
import json,sys; d=json.load(sys.stdin); ips={d.get('eth_ip'), d.get('wifi_ip'), '$GWIP'}
print('yes' if '$(echo "$got" | jq_ peer)' in ips else 'no: peer not in %r' % ips)")"
        eq "载荷含 event/timestamp/device(12 位 MAC)/data.hostname" "ok" "$(echo "$got" | python3 -c "
import json,sys,re
b=json.loads(json.load(sys.stdin)['body'])
print('ok' if b['event']=='config_backup' and isinstance(b['timestamp'],int) and re.fullmatch(r'[0-9A-F]{12}',b['device']) and b['data']['hostname'] else 'bad: %r'%b)")"
        UP1=$(api /status | jq_ uptime)
        eq "发送后设备没有重启（uptime 单调）" "yes" "$([ "${UP1:-0}" -gt "${UP0:-0}" ] && echo yes || echo no)"
        ST1=$(api /diag/watchdog | python3 -c "import json,sys; print([t['stack_left'] for t in json.load(sys.stdin)['tasks'] if t['name']=='webhook'][0])" 2>/dev/null)
        echo "  ℹ️  webhook 任务栈余量：发送前 ${ST0:-?} → 发送后 ${ST1:-?} 字节"
        # 2026-09-05 实测一次 HTTP POST 吃掉数 KB；余量低于 2KB 意味着 HTTPS（mbedtls）必溢出
        eq "发送后栈余量 ≥ 2048（HTTPS 握手还要更多）" "yes" "$([ "${ST1:-0}" -ge 2048 ] && echo yes || echo no)"
    fi
else
    echo "  ℹ️  未加 --webhook-wait：只验了配置接受，没验真实发送（需约 6-7 分钟）"
fi

echo ""
echo "=== 结果：$PASS 通过，$FAIL 失败 ==="
[ $FAIL -eq 0 ]
