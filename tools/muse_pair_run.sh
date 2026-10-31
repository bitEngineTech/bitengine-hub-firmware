#!/bin/bash
# tools/muse_pair_run.sh —— 直连型上板跑一次 Muse 配对（docs/15 设计第五节，TC-MU01 / TC-MU05）。
#
#   GW_PASS=<管理密码> bash tools/muse_pair_run.sh <网关地址> [--set-token] [--no-pair] [--watch-link]
#
#   --set-token  先把 ~/.muse-sdk-token 写进网关（POST /api/v1/muse/config，token 经标准输入喂给 curl：不进命令行参数、不打印），
#                并启用 Muse。不带就沿用网关里已有的。
#   --no-pair    只看状态（含上次配对结果与 Thread 自检），不发起配对。
#   --watch-link 不配对，只盯会话（M3：/muse/status 的 link）：每 5 s 打一次状态变化，到 online 或 10 分钟为止。
#
# 流程：前提检查（直连型、有密码、有 token、已启用）→ POST /api/v1/muse/pair（网关重启进配对剖面）→ 等它回来（/status 的
# mode=muse_pair）→ 提示手机上怎么做 → 轮询配对进度 → 等它重启回正常开机 → 打出 last_result → 等 Thread 自检出结果（约 2–7 分钟）。
#
# ⚠️ 只对直连型固件有意义（HA 型没有这些端点）。板子换成直连型要 USB 线刷：python3 tools/migrate_profile.py to-direct -p <口>，
#    刷完 PoE 和 USB 都拔（IO28，见 CLAUDE.md）。
# ⚠️ 不打印 SDK token、管理密码；密码用 curl 的 -u（同机其他用户 ps 看得到，本地调试工具，接受，同 test_api.sh）。
set -u
GW="${1:-}"
[ -n "$GW" ] || { echo "用法：GW_PASS=<密码> bash tools/muse_pair_run.sh <网关地址> [--set-token] [--no-pair]"; exit 2; }
SET_TOKEN=0; NO_PAIR=0; WATCH=0
for a in "${@:2}"; do
    case "$a" in
        --set-token) SET_TOKEN=1 ;;
        --no-pair)   NO_PAIR=1 ;;
        --watch-link) WATCH=1; NO_PAIR=1 ;;
        *) echo "不认识的参数：$a"; exit 2 ;;
    esac
done
AUTH=()
[ -n "${GW_PASS:-}" ] && AUTH=(-u ":$GW_PASS")
C()   { curl -s -m 10 ${AUTH[@]+"${AUTH[@]}"} "$@"; }
api() { C "http://$GW/api/v1$1"; }
jq_() { python3 -c "import json,sys
try: d=json.load(sys.stdin)
except Exception: print('<非JSON>'); sys.exit()
for k in '$1'.split('.'): d = d.get(k) if isinstance(d,dict) else None
print(d if isinstance(d,str) else json.dumps(d))"; }
ts()  { date +%H:%M:%S; }

echo "=== Muse 配对 @ $GW ==="
[ "$(api /status | jq_ profile)" == "direct" ] || { echo "❌ 不是直连型固件（/status 没有 profile=direct）"; exit 1; }
code=$(C -o /dev/null -w '%{http_code}' "http://$GW/api/v1/muse/status")
[ "$code" == "200" ] || { echo "❌ /api/v1/muse/status 回 $code（401 = 要 GW_PASS；404 = 固件没带 Muse）"; exit 1; }

if [ "$SET_TOKEN" = 1 ]; then
    TOKF="$HOME/.muse-sdk-token"
    [ -r "$TOKF" ] || { echo "❌ 读不到 $TOKF"; exit 1; }
    # token 只经标准输入进 curl（--data-binary @-），不出现在任何命令行参数里
    r=$(python3 -c "import json,sys; print(json.dumps({'sdk_token': open(sys.argv[1]).read().strip(), 'enabled': True}))" "$TOKF" \
        | C -X POST -H 'Content-Type: application/json' --data-binary @- -w '\n%{http_code}' "http://$GW/api/v1/muse/config")
    rc=$(echo "$r" | tail -1)
    [ "$rc" == "200" ] && echo "  ✅ SDK token 已写入、Muse 已启用" || { echo "❌ 写 token 失败：HTTP $rc $(echo "$r" | head -1 | jq_ error)"; exit 1; }
fi

show() {
    local s; s=$(api /muse/status)
    echo "  状态：state=$(echo "$s" | jq_ state)  paired=$(echo "$s" | jq_ paired)  sdk_token=$(echo "$s" | jq_ sdk_token)  enabled=$(echo "$s" | jq_ enabled)"
    echo "  上次配对结果：$(echo "$s" | jq_ last_result)   Thread 自检：$(echo "$s" | jq_ blew.selfcheck.state)" \
         "（ACK $(echo "$s" | jq_ blew.selfcheck.ack_before_pm)‰ → $(echo "$s" | jq_ blew.selfcheck.ack_after_pm)‰，" \
         "连续退化 $(echo "$s" | jq_ blew.selfcheck.consecutive_degraded)，共 $(echo "$s" | jq_ blew.selfcheck.total) 次）"
    echo "  BLE 窗口：bt_mem_released=$(echo "$s" | jq_ blew.bt_mem_released)  host 栈余量=$(echo "$s" | jq_ blew.host_stack_free)" \
         "  开窗前内部空闲=$(echo "$s" | jq_ blew.internal_free_at_open)  窗口期最低=$(echo "$s" | jq_ blew.internal_min_open)"
    echo "  会话（M3）：link.state=$(echo "$s" | jq_ link.state)  last_error=$(echo "$s" | jq_ link.last_error)" \
         "  registrations=$(echo "$s" | jq_ link.registrations)  vm=$(echo "$s" | jq_ link.vm_name)  node=$(echo "$s" | jq_ link.node_id)"
}
show
if [ "$WATCH" = 1 ]; then
    # 会话状态机：fetch_vm → connecting → registering → online；backoff / refreshing / low_mem / unpaired 时打原因
    echo ""; echo "[$(ts)] 盯会话（最多 10 分钟）"
    last=""
    for i in $(seq 1 120); do
        s=$(api /muse/status)
        st=$(echo "$s" | jq_ link.state)
        cur="$st | $(echo "$s" | jq_ link.last_error) | reg=$(echo "$s" | jq_ link.registrations)"
        [ "$cur" != "$last" ] && { echo "[$(ts)] $cur"; last="$cur"; }
        [ "$st" == "online" ] && { echo "✅ 会话在线"; show; exit 0; }
        sleep 5
    done
    echo "❌ 10 分钟没到 online（看上面的 last_error；TLS 握不上先怀疑 E6：hatch.metaaivm.com 是否接受 TLS 1.2）"
    exit 1
fi
[ "$NO_PAIR" = 1 ] && exit 0

echo ""
echo "[$(ts)] 发起配对（网关会重启进配对剖面，约 30–60 s 后回来）"
r=$(C -X POST -w '\n%{http_code}' "http://$GW/api/v1/muse/pair" -d '{}')
rc=$(echo "$r" | tail -1)
[ "$rc" == "202" ] || { echo "❌ /muse/pair 回 $rc：$(echo "$r" | head -1 | jq_ error)"; exit 1; }

# 等它重启进配对剖面
for i in $(seq 1 60); do
    sleep 3
    [ "$(api /status | jq_ mode)" == "muse_pair" ] && break
done
[ "$(api /status | jq_ mode)" == "muse_pair" ] || { echo "❌ 3 分钟没等到配对剖面（/status mode=muse_pair）—— 看串口 / 再查 last_result"; exit 1; }
BLE=$(api /muse/status | jq_ ble_name)
echo "[$(ts)] 已进配对剖面。手机上：Muse App → 打开 Developer mode → 添加设备 → 选 $BLE；"
echo "         App 要求确认时，按住网关按键 2–10 秒后松开（SYS 灯青色快闪的时候）。也可以在浏览器开 http://$GW/ 看进度。"

last=""
for i in $(seq 1 400); do          # 最多约 20 分钟（窗口 ≤ 600 s + 收尾）
    sleep 3
    s=$(api /status)
    m=$(echo "$s" | jq_ mode)
    if [ "$m" == "muse_pair" ]; then
        st=$(api /muse/status | jq_ state); left=$(api /muse/status | jq_ remaining_s)
        [ "$st" != "$last" ] && { echo "[$(ts)] 配对进度：$st（窗口还剩 ${left}s）"; last="$st"; }
    elif [ "$m" == "null" ] && [ "$(echo "$s" | jq_ profile)" == "direct" ]; then
        break                             # 回到正常开机（正常的 /status 没有 mode 字段）
    fi                                    # 连不上（<非JSON>）= 正在重启，接着等
done
echo ""
echo "[$(ts)] 回到正常开机："
show
echo ""
echo "[$(ts)] 等 Thread 自检（设计 3.3；要有单播流量，没有就是 inconclusive）"
for i in $(seq 1 100); do
    sleep 6
    sc=$(api /muse/status | jq_ blew.selfcheck.state)
    [ "$sc" != "pending" ] && break
done
show
