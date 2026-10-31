#!/bin/bash
# tools/test_api.sh —— WebUI REST API 端到端回归（需要一块在网的板子）
#
#   bash tools/test_api.sh [网关IP或主机名]      # 默认 bitengine.local
#   bash tools/test_api.sh 192.168.1.43 --slow  # 含依赖 scheduler 60s tick 的用例
#   bash tools/test_api.sh <IP> --negative-signing  # 加测「未签名镜像必须被拒」
#   bash tools/test_api.sh <IP> --region            # 加测射频地区（docs/14）：会两次断开 Zigbee 各约 40 s
#     ⚠️ 只在手边能接串口时加这个：万一验签没生效，未签名镜像会被装上并启动，
#        之后设备拒绝一切 OTA，只能串口刷回。
#
# 为什么需要这个：VERIFY.sh 是 grep，它能证明"代码存在"，证明不了"代码有用"。
# 2026-08-13 那次 NTP 功能整个是死的（事件发出去没人接），而
# config_mgr/VERIFY.sh 的 "CONFIG_EVENT_NTP_CHANGED posted" 一直是绿的。
# 本脚本断言的是**行为**：改完配置去读回生效后的状态，而不是看源码长什么样。
#
# ⚠️ 会临时修改网关配置。开头自动备份、结束（含中途失败/Ctrl-C）自动还原。
#    白名单类用例尤其小心：留下一个"启用+错 IP"的状态会挡住 Z2M/ZHA 重连。

set -u
GW="${1:-bitengine.local}"
SLOW=0; NEGSIGN=0; REGION=0
for a in "${@:2}"; do
    case "$a" in
        --slow)            SLOW=1 ;;
        --negative-signing) NEGSIGN=1 ;;
        --region)          REGION=1 ;;
    esac
done

PASS=0; FAIL=0; SKIP=0
BACKUP="$(mktemp /tmp/bitengine-cfg.XXXX.json)"
RESTORED=0

# 设备设了密码时必须给凭据。不给的话每个请求都是 401，断言会以"值不对"的形式
# 一片红，看起来像功能全坏了 —— 而真实原因只是没登录。
#   GW_PASS=xxx bash tools/test_api.sh <网关IP>
# 用户名随便填，固件只验冒号后那半（auth.c 的 check_basic_auth）。
# ⚠️ -u 会把密码放进 argv，同机其他用户 ps 得到；本脚本是本地调试工具，接受这个。
AUTH=()
[ -n "${GW_PASS:-}" ] && AUTH=(-u ":$GW_PASS")
C() { curl -s -m 10 ${AUTH[@]+"${AUTH[@]}"} "$@"; }
api()  { C "http://$GW/api/v1$1"; }                                   # GET
apip() { C -X POST "http://$GW/api/v1$1" -d "$2"; }                   # POST
code() { C -o /dev/null -w '%{http_code}' -X "$1" "http://$GW/api/v1$2" ${3:+-d "$3"}; }
# 取 JSON 字段。字符串原样输出（不带引号），其余一律走 json.dumps ——
# 布尔必须是 true/false 而不是 Python 的 True/False，否则断言全对不上。
jq_() { python3 -c "import json,sys
try: d=json.load(sys.stdin)
except Exception: print('<非JSON>'); sys.exit()
for k in '$1'.split('.'): d = d.get(k) if isinstance(d,dict) else None
print(d if isinstance(d,str) else json.dumps(d))"; }

ok()   { echo "  ✅ $1"; PASS=$((PASS+1)); }
bad()  { echo "  ❌ $1"; echo "       期望: $2"; echo "       实际: $3"; FAIL=$((FAIL+1)); }
eq()   { [ "$2" == "$3" ] && ok "$1" || bad "$1" "$2" "$3"; }
# 标记那些直接对应已修 bug 的用例，退化了要一眼看得出严重性
reg()  { [ "$2" == "$3" ] && ok "$1  [防回归]" || bad "$1  [防回归·曾经的真 bug]" "$2" "$3"; }

restore() {
    [ $RESTORED -eq 1 ] && return
    RESTORED=1
    if [ -s "$BACKUP" ]; then
        echo ""
        echo "── 还原配置 ──"
        r=$(C -X POST "http://$GW/api/v1/config/import" --data-binary @"$BACKUP" | jq_ status)
        [ "$r" == "ok" ] && echo "  ✅ 已还原" || echo "  ⚠️  还原失败！备份仍在 $BACKUP"
    fi
    rm -f "$BACKUP" 2>/dev/null
}
# ZH14 改了某台 Zigbee 设备的名字 / 房间 / notify：中途退出也要改回去（第一轮审计：原先只在走完时还原，Ctrl-C 会丢掉用户起的真名字）
ZMETA_RESTORE=""
meta_restore() {
    [ -n "$ZMETA_RESTORE" ] || return 0
    if C -X POST "http://$GW/api/v1/hub/invoke" -d "$ZMETA_RESTORE" 2>/dev/null | grep -q '"result"'; then
        echo "  ↩️  已把 ZH14 改过的设备名字 / 房间 / notify 改回去"
    else
        echo "  ⚠️  ZH14 改过的设备名字 / 房间 / notify 没改回去，手动还原：$ZMETA_RESTORE"
    fi
    ZMETA_RESTORE=""
}
# INT / TERM 只负责退出（EXIT 那个 trap 做还原）：trap 处理完信号不退出的话脚本会接着往下跑，还原过的又被改掉（修复复核）
trap 'meta_restore; restore' EXIT
trap 'exit 130' INT TERM

# ─────────────────────────────── 前置 ───────────────────────────────
echo "=== bitEngine API 回归 @ $GW ==="
if ! C -o /dev/null "http://$GW/api/v1/status"; then
    echo "  ❌ 连不上 $GW"; exit 1
fi
# curl 收到 401 也返回 0，所以上面那句探不出"没登录"。必须单独查状态码，
# 否则后面每条断言都会以"值是 null"的形式失败，把认证问题伪装成功能问题。
SC=$(C -o /dev/null -w '%{http_code}' "http://$GW/api/v1/status")
if [ "$SC" == "401" ]; then
    echo "  ❌ 需要认证（HTTP 401）。设备设了密码，请带上凭据重跑："
    echo "       GW_PASS=<设备密码> bash tools/test_api.sh $GW"
    exit 1
fi

# #17（2026-09-13）：JSON 回包必须带 charset=utf-8。Windows PowerShell 5.1 的
# Invoke-RestMethod 没有 charset 就按 ISO-8859-1 解码，factory_api 的 detail 中文
# 会成乱码（pwsh 7 默认 UTF-8 测不出来）。两条路各测一次：send_json_response
# （200）与 send_error（这里用 404 的 JSON 错误体）。
ctype() { C -o /dev/null -w '%{content_type}' "http://$GW$1"; }
eq "JSON 200 回包 Content-Type 带 charset=utf-8" \
   "application/json; charset=utf-8" "$(ctype /api/v1/status)"
eq "JSON 错误回包 Content-Type 带 charset=utf-8" \
   "application/json; charset=utf-8" "$(ctype /api/v1/this-endpoint-does-not-exist)"
[ "$SC" == "200" ] || { echo "  ❌ /api/v1/status 回了 $SC，不是 200"; exit 1; }
CLIENTS_BEFORE=$(api /status | jq_ tcp_clients)
echo "  网关可达，tcp_clients=$CLIENTS_BEFORE"
# 纯以太网剖面（直连型，docs/15 设计 2.2）：/status 带 "wifi":"absent"，/wifi 下的端点与 /network/softap 不注册。
# 依赖它们的几段（SoftAP、Wi-Fi 静态 IP、/wifi/config）跳过，换成"这些端点确实不在"的断言。HA 型照旧全跑。
WIFI_ABSENT=$( [ "$(api /status | jq_ wifi)" == "absent" ] && echo 1 || echo 0 )
if [ "$WIFI_ABSENT" = 1 ]; then
    echo ""; echo "── 纯以太网剖面（/status 报 wifi=absent）──"
    # "没注册"的判据是"和一个不存在的地址回同样的码"：GET 是 404；POST 是 405 —— 前端静态文件挂着 GET 通配，
    # httpd 见路径匹配上了通配、方法不对就回 405（2026-10-21 上板：POST /nonexistent 同样 405）。
    NX_POST=$(code POST /nonexistent_for_e0x '{}')
    eq "E01 /wifi/scan 不注册（404）"        "404" "$(code GET /wifi/scan)"
    eq "E02 /wifi/config 不注册（同不存在的地址）" "$NX_POST" "$(code POST /wifi/config '{"ssid":"x","password":"12345678"}')"
    eq "E03 /network/softap 不注册（404）"   "404" "$(code GET /network/softap)"
    eq "E04 /wifi/static 不注册（同不存在的地址）" "$NX_POST" "$(code POST /wifi/static '{"dhcp":true}')"
    eq "E05 wifi_connected 恒为 false"       "false" "$(api /status | jq_ wifi_connected)"
    eq "E06 /status 报 profile=direct（设计 10.1：没有这个字段即 HA 型）" "direct" "$(api /status | jq_ profile)"
fi

# 直连型不带桥（docs/15 架构 5.8 R-2、设计 2.8 A-04）：没有 :6638、SMLIGHT 兼容层、只服务桥的端点，不宣告 _zigbee-*，
# /zigbee/restart 不走 IO23。依赖桥的几段（白名单、踢客户端、/api2/info、桥接模式落盘）跳过，换成"确实不在"的断言。
NO_BRIDGE=$( [ "$(api /status | jq_ profile)" == "direct" ] && echo 1 || echo 0 )
if [ "$NO_BRIDGE" = 1 ]; then
    echo ""; echo "── 直连型：没有桥 ──"
    NX_GET=$(code GET /nonexistent_for_b0x); NX_POST=$(code POST /nonexistent_for_b0x '{}')
    eq "B01 /mode 不注册（同不存在的地址）"                 "$NX_POST" "$(code POST /mode '{"mode":0}')"
    eq "B02 /whitelist 不注册（同不存在的地址）"            "$NX_GET"  "$(code GET /whitelist)"
    eq "B03 /zigbee/clients/disconnect 不注册（同不存在的地址）" "$NX_POST" "$(code POST /zigbee/clients/disconnect '{"index":0}')"
    eq "B04 SMLIGHT 兼容层 /api2/info 不在"                 "404" "$(C -o /dev/null -w '%{http_code}' "http://$GW/api2/info")"
    eq "B05 :6638 连不上（没有 TCP 服务）"                  "refused" \
       "$(timeout 4 bash -c "exec 3<>/dev/tcp/$GW/6638" 2>/dev/null && echo open || echo refused)"
    # B06：/zigbee/restart 是 zhub 软重启（2026-10-22 起，docs/15 2.8 表），断言放在下面"直连型：zhub"一段里（要先等它跑起来）
    eq "B07 /status 的 bridge_mode 恒为 disabled"           "disabled" "$(api /status | jq_ bridge_mode)"
    # 跨源 POST：注册了就是守卫先拦 403（不会真重启，也顺带证明 CROSS_ORIGIN_GUARD 排在最前）；没注册会是 405。
    # ⚠️ 别用 GET 判：前端静态文件的 GET 通配会接住，回 404，和不存在的地址一模一样（审计 2026-10-22）。
    eq "B08 有原生重启端点 /system/reboot（跨源 POST 被守卫拦下，不真按）" "403" \
       "$(C -o /dev/null -w '%{http_code}' -X POST -H 'Origin: http://evil.example' "http://$GW/api/v1/system/reboot" -d '{}')"
    eq "B11 zigbee_host_absent_s 为 null（没有上位机这回事，不报协调器断开）" "null" "$(api /status | jq_ zigbee_host_absent_s)"
    eq "B12 /zigbee/status 的 restart_resets_board 为 false（不碰 IO23）" "false" "$(api /zigbee/status | jq_ restart_resets_board)"

    echo ""; echo "── 直连型：zhub（网关自己当 Zigbee 主机，docs/15 第八节；读回生效后的状态，CLAUDE.md 7.5）──"
    zs() { api /zigbee/status | jq_ "$1"; }
    st=""; for _ in $(seq 1 30); do st=$(zs hub.state); [ "$st" == "running" ] && break; sleep 1; done
    eq "ZH01 hub.state 30 s 内是 running（ASH 复位 → 握手 → networkInit 走完）" "running" "$st"
    # 默认判 up：开发板的 NCP 里有 ZHA 建的网，读到 none 等于网没了（红线 4 的范围）。NCP 确实是空的（新板、擦过 NVM3）才设 ZHUB_EXPECT_NO_NETWORK=1
    zn=$(zs hub.net)
    if [ "${ZHUB_EXPECT_NO_NETWORK:-0}" = 1 ]; then
        eq "ZH02 hub.net=none（声明了 NCP 是空的：zhub 不自动建网）" "none" "$zn"
    else
        eq "ZH02 hub.net=up（NCP 里的网络起来了；读到 none 说明网没了 —— NCP 确实是空的才设 ZHUB_EXPECT_NO_NETWORK=1）" "up" "$zn"
    fi
    eq "ZH03 UART 在 zhub 手里（hub.uart=true、suspended=false）" "true false" "$(zs hub.uart) $(zs hub.suspended)"
    zrx=$(zs uart_rx)
    [ "${zrx:-0}" -gt 0 ] 2>/dev/null && ok "ZH04 zhub 收的字节计入 bridge_stats（uart_rx=$zrx；产测 ezsp_uart 项靠它）" || bad "ZH04 uart_rx > 0" ">0" "$zrx"
    eq "ZH05 有正向证据（配上的 EZSP 应答 / 回调，看门狗的 MG24 探针读它）" "true" "$(zs hub.evidence)"
    # device.health（Muse 读的那份）里的 Zigbee 要跟 zhub 一致：原先写死 not_in_use，Muse 据此对用户说"Zigbee 还不工作"（2026-10-22 上板）
    zdh=$(C -X POST -H 'Content-Type: application/json' -d '{"command":"device.health","params":{}}' "http://$GW/api/v1/hub/invoke")
    eq "ZH08 device.health 的 zigbee 读 zhub（radio=answering、network 与 hub.net 一致）" "answering $(zs hub.net)" \
       "$(echo "$zdh" | jq_ result.zigbee.radio) $(echo "$zdh" | jq_ result.zigbee.network)"
    # 第五步：UP 之后的序列（读参数 → EUI64 → 集中器 → MTORR）走完；没有网络时一样都不该做（不对空 NCP 设集中器）
    zc=""; for _ in $(seq 1 15); do zc="$(zs hub.concentrator) $(zs hub.eui64 | grep -cE '^[0-9A-F]{16}$')"; [ "$zc" == "true 1" ] && break; [ "$zn" != "up" ] && break; sleep 1; done
    if [ "$zn" == "up" ]; then
        eq "ZH09 网络 UP 之后：集中器设好（hub.concentrator=true）、读回 16 位十六进制的 EUI64" "true 1" "$zc"
        eq "ZH09b networkInit 之前的可选配置（MAC 间接发送超时 7680）NCP 都收下了（hub.init_opt_rejects=0；被拒只记数不复位）" "0" "$(zs hub.init_opt_rejects)"
    else
        eq "ZH09 没有网络：不设集中器、不读 EUI64（hub.concentrator=false、eui64=null）" "false null" "$(zs hub.concentrator) $(zs hub.eui64)"
    fi
    echo "  ℹ️  zhub：实际信道 $(zs hub.channel)（声明的 $(zs configured_channel)，不一致只说明声明过期）、PAN $(zs hub.pan_id)、stackType $(zs hub.stack_type)、回调 $(zs hub.callbacks) 条、ASH 重传 $(zs hub.ash.retx)"
    echo "  ℹ️  zhub：设备发来的消息 $(zs hub.incoming) 条（解不开 $(zs hub.incoming_bad)，最近一条解不开的 $(zs hub.incoming_bad_last)），最近一条 $(zs hub.last_rx)；UP 之后的步骤没成 $(zs hub.up_bad) 次"
    # 第六步：入网窗口经本地出口 /hub/invoke 开 / 关（Muse 那边第七步才注册），读回 NCP 报的状态（hub.join_window.ncp_open）与事件环
    zpw=$(api /auth/status | jq_ password_required)   # 同下面 HAS_PW 的判法：true = 设了管理密码
    if [ "$zn" == "up" ] && [ "$zpw" != "true" ]; then
        # 没设管理密码：写命令一律 409（第十三节"写端点要密码"；permit_join 是第一条写命令，第二轮审计），窗口不许开
        r=$(C -X POST -w '\n%{http_code}' "http://$GW/api/v1/hub/invoke" -d '{"command":"hub.zigbee.permit_join","params":{"seconds":30}}')
        eq "ZH10 没设密码：permit_join → 409 password_required、窗口仍关着" "409 password_required closed" \
           "$(echo "$r" | tail -1) $(echo "$r" | head -1 | jq_ error) $(zs hub.join_window.state)"
    elif [ "$zn" == "up" ]; then
        inv() { C -X POST -w '\n%{http_code}' "http://$GW/api/v1/hub/invoke" -d "$1"; }
        r=$(inv '{"command":"hub.zigbee.permit_join","params":{"seconds":10}}')
        eq "ZH10 permit_join seconds=10（< 30）→ 400 invalid_params" "400 invalid_params" "$(echo "$r" | tail -1) $(echo "$r" | head -1 | jq_ error)"
        r=$(inv '{"command":"hub.zigbee.permit_join","params":{"seconds":30}}')
        zl=$(echo "$r" | head -1 | jq_ result.seconds_left)
        eq "ZH10b 开 30 s → 200 open=true（回包时 NCP 那边已经开了：处理函数等它落定）" "200 true" "$(echo "$r" | tail -1) $(echo "$r" | head -1 | jq_ result.open)"
        zw=""; for _ in $(seq 1 10); do zw="$(zs hub.join_window.state) $(zs hub.join_window.ncp_open)"; [ "$zw" == "open true" ] && break; sleep 1; done
        eq "ZH10c 窗口开了、NCP 报了 NETWORK_OPENED（hub.join_window.state=open、ncp_open=true）" "open true" "$zw"
        [ "${zl:-99}" -le 30 ] 2>/dev/null && [ "${zl:-0}" -ge 25 ] 2>/dev/null && ok "ZH10d 回包里的剩余秒数合理（$zl s）" || bad "ZH10d 回包里的剩余秒数 25–30" "25..30" "$zl"
        r=$(inv '{"command":"hub.zigbee.permit_join","params":{"seconds":60}}')
        eq "ZH10e 60 s 内再开 → 429 rate_limited（每 60 s 1 次）" "429 rate_limited" "$(echo "$r" | tail -1) $(echo "$r" | head -1 | jq_ error)"
        zc0=$(zs hub.join_window.closes)
        r=$(inv '{"command":"hub.zigbee.permit_join","params":{"seconds":0}}')
        eq "ZH10f 关窗不进限速桶 → 200 open=false" "200 false" "$(echo "$r" | tail -1) $(echo "$r" | head -1 | jq_ result.open)"
        zw=""; for _ in $(seq 1 10); do zw="$(zs hub.join_window.state) $(zs hub.join_window.ncp_open) $(zs hub.join_window.closes)"; [ "$zw" == "closed false $((zc0 + 1))" ] && break; sleep 1; done
        eq "ZH10g 关完（策略写回、清临时链路密钥）、NCP 报 NETWORK_CLOSED" "closed false $((zc0 + 1))" "$zw"
        zq=$(zs hub.join_window.quota_left_s)
        [ "${zq:-600}" -lt 600 ] 2>/dev/null && ok "ZH10i 总时长上限记了这次开的时长（滚动一小时还剩 $zq s）" || bad "ZH10i 总时长上限扣了这次" "< 600" "$zq"
        ze=""; for _ in $(seq 1 15); do ze=$(C -X POST "http://$GW/api/v1/hub/invoke" -d '{"command":"hub.events.recent","params":{"limit":20}}' | python3 -c "
import json,sys
d=json.load(sys.stdin); k=[e.get('kind') for e in d.get('result',{}).get('events',[])]
print('yes' if 'zigbee_pairing_open' in k and 'zigbee_pairing_closed' in k else 'no')" 2>/dev/null); [ "$ze" == "yes" ] && break; sleep 1; done
        eq "ZH10h 事件环里有 zigbee_pairing_open / zigbee_pairing_closed（hub_svc 每 10 s 比一次快照）" "yes" "$ze"
    fi
    # /hub/invoke 有限速（连发 5 条后回 rate_limited）：这几条一口气调好几次，碰上限速就等一会儿再试，别把限速当成断言失败
    hinv() { local r i; for i in 1 2 3 4 5 6; do r=$(C -X POST "http://$GW/api/v1/hub/invoke" -d "$1"); echo "$r" | grep -q '"rate_limited"' || break; sleep 3; done; echo "$r"; }
    # 第七步 a：设备记录 + 被动学习。设备什么时候说话不由我们定（被动学习要它先发一条），所以只断言一致性：
    # 列表里 zb- 的条数 = hub.devices.count；有设备时 hub.device.get 读得回同一条
    zdn=""; zdc=""
    for _ in 1 2 3; do
        zdc=$(zs hub.devices.count)
        zdn=$(hinv '{"command":"hub.devices.list","params":{"protocol":"zigbee"}}' | python3 -c "
import json,sys
d=json.load(sys.stdin)['result']['devices']
print(len(d) if all(x.get('id','').startswith('zb-') and x.get('protocol')=='zigbee' for x in d) else 'non-zigbee-entry')" 2>/dev/null)
        [ "$zdn" == "$zdc" ] && break; sleep 1   # 两次读之间正好认出一台：再读一遍
    done
    eq "ZH11 hub.devices.list（protocol=zigbee）只出 zb- 条目、条数与 /zigbee/status 的 hub.devices.count 一致" "$zdc" "$zdn"
    if [ "${zdc:-0}" -gt 0 ] 2>/dev/null; then
        zid=$(hinv '{"command":"hub.devices.list","params":{"protocol":"zigbee"}}' | python3 -c "import json,sys; print(json.load(sys.stdin)['result']['devices'][0]['id'])" 2>/dev/null)
        zg=$(hinv "{\"command\":\"hub.device.get\",\"params\":{\"id\":\"$zid\"}}")
        eq "ZH11b hub.device.get 读回同一台（id、protocol=zigbee、16 位短地址 0xNNNN）" "$zid zigbee 1" \
           "$(echo "$zg" | jq_ result.id) $(echo "$zg" | jq_ result.protocol) $(echo "$zg" | jq_ result.zigbee.nwk | grep -cE '^0x[0-9A-F]{4}$')"
    else
        echo "  ℹ️  ZH11b 跳过：设备表还是空的（被动学习要设备先发一条消息；收到的消息 $(zs hub.incoming) 条）"
    fi
    echo "  ℹ️  zhub 设备层：$(api /zigbee/status | python3 -c "import json,sys; print(json.load(sys.stdin)['hub'].get('devices'))" 2>/dev/null)"
    eq "ZH11c /zigbee/status 的 hub.devices 带 CONFIGURE 的计数（cie_* / enroll_* / cie_resumes / iv_send_fail，M5 第四步；bind_* / report_* / cfg_skipped，第五步；tuya_* 第八步、ctl_tuya 第九步、lumi_* / learned 第十步、av_* 第十一步、ctlq_* 第十二步；iv_retries 10-24）" "ok" \
       "$(api /zigbee/status | python3 -c "import json,sys; d=json.load(sys.stdin)['hub']['devices']; print('ok' if all(isinstance(d.get(k),int) for k in ('cie_ok','cie_fail','cie_skipped','enroll_ok','enroll_unconfirmed','cie_resumes','iv_send_fail','bind_ok','bind_fail','report_ok','report_fail','cfg_skipped','batt_reads','ctl_sent','ctl_confirmed','ctl_delivered','ctl_failed','tuya_frames','tuya_unknown','tuya_unmapped','tuya_hellos','ctl_tuya','lumi_frames','lumi_unknown','learned','av_pings','av_offline','av_online','av_quiet','ctlq_queued','ctlq_sent','ctlq_delivered','ctlq_expired','iv_retries')) else 'bad:%s' % d)" 2>/dev/null)"
    # M6 第一步：主动发现（设计 8.12 ④）。字段齐全、状态取值合法；开始了的必须发过请求（7.5：它真的跑了）；网 UP 着、开机满 10 min 的必须已经开始
    # （网 UP 60 s 后开始。不按开机时长判"发过"：网起得晚 / 没网（ZHUB_EXPECT_NO_NETWORK）时状态本来就停在 idle / waiting —— 审计 ⚠️）
    eq "ZH11d /zigbee/status 的 hub.discovery：计数字段齐全、状态合法；开始了就发过请求；网 UP 着开机满 10 min 已开始" "ok" \
       "$(api /zigbee/status | python3 -c "
import json,sys
h=json.load(sys.stdin)['hub']; d=h.get('discovery') or {}
keys=('routers_queued','routers_done','reqs','rsps','timeouts','send_fail','routers','unsupported','gave_up','entries','added','known','conflict','xpan','skipped','bad','rounds','q_full')
ok=all(isinstance(d.get(k),int) for k in keys) and d.get('state') in ('idle','waiting','running','done')
up=$(api /status | jq_ uptime)
if ok and d['state'] in ('running','done'): ok = d['reqs']>=1
if ok and h.get('net')=='up' and up>=600: ok = d['state'] in ('running','done')
print('ok' if ok else 'bad:%s net=%s up=%s' % (d, h.get('net'), up))" 2>/dev/null)"
    echo "  ℹ️  主动发现：$(api /zigbee/status | python3 -c "import json,sys; print(json.load(sys.stdin)['hub'].get('discovery'))" 2>/dev/null)"
    # 第七步 b：访谈。完成了的设备 hub.device.get 要有厂家或型号（净化过）、端点；访谈状态只许 pending / running / done / failed。
    # 设备什么时候醒不由我们定（休眠设备只在它刚说过话时访谈），没有访谈完的设备就只断言状态取值
    zi=$(hinv '{"command":"hub.devices.list","params":{"protocol":"zigbee"}}' | python3 -c "
import json,sys
d=json.load(sys.stdin)['result']['devices']
st=[x.get('zigbee',{}).get('interview') for x in d]
done=[x['id'] for x in d if x.get('zigbee',{}).get('interview') == 'done']
print(('ok' if all(s in ('pending','running','done','failed') for s in st) else 'bad:%s' % st), done[0] if done else '-')" 2>/dev/null)
    eq "ZH12 列表里每台 Zigbee 设备的访谈状态都是 pending / running / done / failed 之一" "ok" "${zi%% *}"
    zid12=${zi##* }
    if [ -n "$zid12" ] && [ "$zid12" != "-" ] && [ "$zid12" != "ok" ]; then
        zg=$(hinv "{\"command\":\"hub.device.get\",\"params\":{\"id\":\"$zid12\"}}" | python3 -c "
import json,sys
r=json.load(sys.stdin)['result']
eps=r.get('zigbee',{}).get('endpoints') or []
ok=isinstance(eps,list) and all(isinstance(e.get('endpoint'),int) and e.get('profile','').startswith('0x') for e in eps) and isinstance(r.get('manufacturer',''),str)
print('ok' if ok and r['zigbee'].get('identified') is True else 'bad')" 2>/dev/null)
        eq "ZH12b 访谈完的 $zid12：hub.device.get 有端点列表（identified=true；厂家 / 型号设备不支持时可以没有）" "ok" "$zg"
        zie=$(hinv "{\"command\":\"hub.device.get\",\"params\":{\"id\":\"$zid12\"}}" | python3 -c "
import json,sys
z=json.load(sys.stdin)['result']['zigbee']
has_ias=any('0x0500' in e.get('in_clusters',[]) for e in z.get('endpoints',[]))
v=z.get('ias_enrollment')
print('ok' if (v in ('pending','done')) == has_ias else 'bad:%s/%s' % (has_ias, v))" 2>/dev/null)
        eq "ZH12c 有 IAS Zone 的设备详情带 zigbee.ias_enrollment（pending / done），没有的不带（M5 第四步）" "ok" "$zie"
    else
        echo "  ℹ️  ZH12b 跳过：还没有访谈完的 Zigbee 设备（休眠设备要等它说话）"
    fi
    # M5 第一步：能力 / kind / 读数。访谈完的设备 kind 不是 unknown 就要有能力；capability 过滤列出来的都真有这个能力；
    # 读数只在设备说过话之后才有：有 state 键就核类型（motion / onoff 布尔、contact open|closed、battery_pct 0–100）
    zk=$(hinv '{"command":"hub.devices.list","params":{"protocol":"zigbee"}}' | python3 -c "
import json,sys
d=json.load(sys.stdin)['result']['devices']
bad=[]
for x in d:
    z=x.get('zigbee',{}); caps=x.get('capabilities',[]); st=x.get('state',{})
    if z.get('identified') and x.get('kind') not in ('unknown','gas_sensor','co_sensor',None) and not caps: bad.append(x['id']+':caps')   # 燃气 / CO 没有对应能力（8.8 注记）
    if not z.get('identified') and (caps or x.get('kind')!='unknown'): bad.append(x['id']+':pre-iv')
    for k in ('motion','onoff','water_leak','smoke','vibration','gas','co','tamper','battery_low'):
        if k in st and not isinstance(st[k],bool): bad.append(x['id']+':'+k)
    if 'contact' in st and st['contact'] not in ('open','closed'): bad.append(x['id']+':contact')
    if 'battery_pct' in st and not (0<=st['battery_pct']<=100): bad.append(x['id']+':battery')
print('ok' if not bad else 'bad:'+','.join(bad))" 2>/dev/null)
    eq "ZH13 访谈完的 Zigbee 设备有能力 / kind、访谈前没有；读数的键与取值类型对（附录 A）" "ok" "$zk"
    zm=$(hinv '{"command":"hub.devices.list","params":{"protocol":"zigbee","capability":"motion"}}' | python3 -c "
import json,sys
d=json.load(sys.stdin)['result']['devices']
print('ok' if all('motion' in x.get('capabilities',[]) for x in d) else 'bad', len(d))" 2>/dev/null)
    eq "ZH13b capability=motion 过滤：列出来的每台都有 motion 能力（${zm##* } 台）" "ok" "${zm%% *}"
    # M5 第二步：hub.device.update（名字 / 房间 / 推送开关）。改第一台 Zigbee 设备，读回、按房间过滤，最后还原成原来的样子
    zu=$(hinv '{"command":"hub.devices.list","params":{"protocol":"zigbee"}}' | python3 -c "
import json,sys
d=json.load(sys.stdin)['result']['devices']
x=d[0] if d else None
print(json.dumps([x['id'],x['name'],x['room'],x['notify']], ensure_ascii=False) if x else '')" 2>/dev/null)
    if [ -z "$zu" ]; then
        echo "  ℹ️  ZH14 跳过：设备表还是空的"
    elif [ "$zpw" != "true" ]; then
        # 没设管理密码：写命令一律 409（同 ZH10），名字改不了
        zuid=$(echo "$zu" | python3 -c "import json,sys; print(json.load(sys.stdin)[0])")
        r=$(C -X POST -w '\n%{http_code}' "http://$GW/api/v1/hub/invoke" -d "{\"command\":\"hub.device.update\",\"params\":{\"id\":\"$zuid\",\"name\":\"x\"}}")
        eq "ZH14 没设密码：hub.device.update → 409 password_required" "409 password_required" "$(echo "$r" | tail -1) $(echo "$r" | head -1 | jq_ error)"
    else
        zuid=$(echo "$zu" | python3 -c "import json,sys; print(json.load(sys.stdin)[0])")
        zuid_up=$(echo "$zuid" | tr 'a-z' 'A-Z')
        echo "  ℹ️  ZH14 改 $zuid（原值 [id,name,room,notify] = $zu；跑完 / 中途退出都改回去）"
        ZMETA_RESTORE=$(echo "$zu" | python3 -c "
import json,sys
i,n,r,no=json.load(sys.stdin)
print(json.dumps({'command':'hub.device.update','params':{'id':i,'name':n,'room':r,'notify':no}}, ensure_ascii=False))")
        r=$(hinv "{\"command\":\"hub.device.update\",\"params\":{\"id\":\"$zuid_up\",\"name\":\"  API  test 名字 \",\"room\":\"Test   room\",\"notify\":true}}")
        eq "ZH14 hub.device.update 改名字 / 房间 / notify：回读是净化后的样子、id 小写（大写 id 也认）" "$zuid|API test 名字|Test room|True" \
           "$(echo "$r" | python3 -c "import json,sys; r=json.load(sys.stdin)['result']; print('%s|%s|%s|%s' % (r['id'],r['name'],r['room'],r['notify']))" 2>/dev/null)"
        # notify_active = notify AND 门窗 / 人体这一类推送开着：跟 /muse/config 的 notify.security 比（修复复核：只验回读自洽，类别位取错也照样绿）；
        # 没有 Muse 页（编译关掉 Muse）时类别位是出厂默认（全开）
        zsec=$( [ "$(code GET /muse/config)" == "200" ] && api /muse/config | jq_ notify.security || echo true )
        eq "ZH14j 回读 notify_active = notify AND 类别开关（notify.security=$zsec）；类别关着时带 message" "ok" \
           "$(echo "$r" | ZSEC="$zsec" python3 -c "
import json,os,sys; r=json.load(sys.stdin)['result']; sec=os.environ['ZSEC']=='true'
ok = r.get('notify_active') is (bool(r['notify']) and sec) and (('message' in r) == (bool(r['notify']) and not sec))
print('ok' if ok else 'bad:%s' % r)" 2>/dev/null)"
        r=$(hinv "{\"command\":\"hub.device.get\",\"params\":{\"id\":\"$zuid\"}}")
        eq "ZH14b hub.device.get 读回同样的名字 / 房间 / notify" "API test 名字|Test room|True" \
           "$(echo "$r" | python3 -c "import json,sys; r=json.load(sys.stdin)['result']; print('%s|%s|%s' % (r['name'],r['room'],r['notify']))" 2>/dev/null)"
        r=$(hinv '{"command":"hub.devices.list","params":{"room":" test ROOM "}}')
        r2=$(hinv '{"command":"hub.devices.list","params":{"room":"no-such-room-x"}}')
        eq "ZH14c room 过滤不分大小写、首尾空白不算；不存在的房间一台都没有（Thread 成员没有房间）" "1 0" \
           "$(echo "$r" | python3 -c "import json,sys; d=json.load(sys.stdin)['result']['devices']; print(int(any(x['id']=='$zuid' for x in d) and all(x.get('room','').lower()=='test room' for x in d)))" 2>/dev/null) $(echo "$r2" | python3 -c "import json,sys; print(len(json.load(sys.stdin)['result']['devices']))" 2>/dev/null)"
        r=$(hinv "{\"command\":\"hub.device.update\",\"params\":{\"id\":\"$zuid\",\"notify\":false}}")
        eq "ZH14d 只给 notify：名字 / 房间不动" "API test 名字|Test room|False" \
           "$(echo "$r" | python3 -c "import json,sys; r=json.load(sys.stdin)['result']; print('%s|%s|%s' % (r['name'],r['room'],r['notify']))" 2>/dev/null)"
        eq "ZH14e 名字带换行 → invalid_params（原值不动）" "invalid_params|API test 名字" \
           "$(hinv "{\"command\":\"hub.device.update\",\"params\":{\"id\":\"$zuid\",\"name\":\"a\\nb\"}}" | jq_ error)|$(hinv "{\"command\":\"hub.device.get\",\"params\":{\"id\":\"$zuid\"}}" | jq_ result.name)"
        eq "ZH14f 名字 33 B → invalid_params；什么都没给 → invalid_params；不存在的 zb- → not_found；th- → invalid_params" "invalid_params invalid_params not_found invalid_params" \
           "$(hinv "{\"command\":\"hub.device.update\",\"params\":{\"id\":\"$zuid\",\"name\":\"123456789012345678901234567890123\"}}" | jq_ error) $(hinv "{\"command\":\"hub.device.update\",\"params\":{\"id\":\"$zuid\"}}" | jq_ error) $(hinv '{"command":"hub.device.update","params":{"id":"zb-0000000000000001","name":"x"}}' | jq_ error) $(hinv '{"command":"hub.device.update","params":{"id":"th-0000000000000001","name":"x"}}' | jq_ error)"
        eq "ZH14g /zigbee/status 的 hub.meta：起来了、这次开机没有只读页" "true 0" "$(zs hub.meta.ready) $(zs hub.meta.pages_ro)"
        if [ "$(code GET '/muse/audit?limit=30')" == "200" ]; then
            eq "ZH14i 审计记下改了哪几项、不记名字原文（动作列 name.room.notify_on；C17 第二轮审计）" "yes" \
               "$(api '/muse/audit?limit=30' | python3 -c 'import json,sys; e=json.load(sys.stdin)["entries"]; print("yes" if any(x["command"]=="hub.device.update" and x.get("action")=="name.room.notify_on" and x["result"]=="ok" for x in e) and not any("API test" in json.dumps(x, ensure_ascii=False) for x in e) else "no")' 2>/dev/null || echo no)"
        fi
        r=$(hinv "$ZMETA_RESTORE")
        eq "ZH14h 还原成原来的名字 / 房间 / notify" "$(echo "$zu" | python3 -c "import json,sys; i,n,r,no=json.load(sys.stdin); print('%s|%s|%s' % (n,r,no))")" \
           "$(echo "$r" | python3 -c "import json,sys; r=json.load(sys.stdin)['result']; print('%s|%s|%s' % (r['name'],r['room'],r['notify']))" 2>/dev/null)"
        echo "$r" | grep -q '"result"' && ZMETA_RESTORE=""   # 还原成功：退出时不用再改
    fi
    # M5 第七步：hub.device.control 的拒绝路径（板上没有可控设备：人体传感器不可控）；真开关灯要配上灯泡后人在场验（TC-ZH04）
    if [ "$zpw" == "true" ]; then
        zc=$(hinv '{"command":"hub.devices.list","params":{"protocol":"zigbee"}}' | python3 -c "
import json,sys
d=json.load(sys.stdin)['result']['devices']
nc=[x['id'] for x in d if not x.get('controllable')]
print(nc[0] if nc else '-', 'ok' if all(isinstance(x.get('controllable'),bool) for x in d) else 'bad')" 2>/dev/null)
        eq "ZH15 列表项都带 controllable（布尔）" "ok" "${zc##* }"
        znc=${zc%% *}
        if [ -n "$znc" ] && [ "$znc" != "-" ]; then
            eq "ZH15b 不可控的设备 turn_on → invalid_params（没有这个能力）" "invalid_params" \
               "$(hinv "{\"command\":\"hub.device.control\",\"params\":{\"id\":\"$znc\",\"action\":\"turn_on\"}}" | jq_ error)"
        fi
        eq "ZH15c 不认识的动作 → invalid_params；亮度 150 → invalid_params；不存在的 zb- → not_found；th- → not_found" "invalid_params invalid_params not_found not_found" \
           "$(hinv '{"command":"hub.device.control","params":{"id":"zb-0000000000000001","action":"fly"}}' | jq_ error) $(hinv '{"command":"hub.device.control","params":{"id":"zb-0000000000000001","action":"set_brightness","value":"150"}}' | jq_ error) $(hinv '{"command":"hub.device.control","params":{"id":"zb-0000000000000001","action":"turn_on"}}' | jq_ error) $(hinv '{"command":"hub.device.control","params":{"id":"th-0000000000000001","action":"turn_on"}}' | jq_ error)"
    fi
    zrs0=$(zs hub.resets); zrt0=$(zs hub.restarts)
    [[ "$zrs0" =~ ^[0-9]+$ ]] || zrs0=0; [[ "$zrt0" =~ ^[0-9]+$ ]] || zrt0=0   # 没有 hub 对象时别让算术把脚本弄挂（上面 Z01 已经红了）
    zt_b06=$(date +%s)
    eq "B06 /zigbee/restart：zhub 软重启（200；绝不走 IO23）" "200" "$(code POST /zigbee/restart '{}')"
    st=""; for _ in $(seq 1 30); do sleep 1; st=$(zs hub.state); [ "$st" == "running" ] && [ "$(zs hub.resets)" != "$zrs0" ] && break; done
    eq "B06b 软重启之后 restarts +1、resets +1、30 s 内回到 running" "$((zrt0 + 1)) $((zrs0 + 1)) running" "$(zs hub.restarts) $(zs hub.resets) $st"
    # 冷却时长从源码读（zhub.c 的 ZHUB_RESTART_MIN_MS），不抄数；B06b 等久了、已出冷却期就别按 —— 按了是真重启
    zcool=$(grep -oE '#define ZHUB_RESTART_MIN_MS +[0-9]+' "$(dirname "$0")/../components/zhub/zhub.c" 2>/dev/null | grep -oE '[0-9]+$')
    if [ -n "$zcool" ] && [ $(( ($(date +%s) - zt_b06) * 1000 + 1500 )) -lt "$zcool" ]; then
        eq "B06c 冷却期内再按一次：429（软重启跳过退避，限最短间隔）" "429" "$(code POST /zigbee/restart '{}')"
    else
        echo "  ⏭️  B06c：B06b 等了 $(( $(date +%s) - zt_b06 )) s，已出冷却期，不再按"; SKIP=$((SKIP+1))
    fi
    # 看门狗的 MG24 探针真在读 zhub 的证据：judged 必须是 true —— mg24_ok 的初值就是 true，单看它什么也证明不了（第二轮审计）
    zj=""; for _ in $(seq 1 40); do zj="$(api /diag/watchdog | jq_ probes.mg24_judged) $(api /diag/watchdog | jq_ probes.mg24_ok)"; [ "$zj" == "true true" ] && break; sleep 1; done
    eq "ZH06 看门狗的 MG24 探针读到 zhub 的证据（probes.mg24_judged / mg24_ok，最多等 40 s：探针按自己的节拍跑）" "true true" "$zj"
    # UART 仲裁端到端：基准测试经 ezsp_bridge_ota_claim 认领 UART —— zhub 要让出，测完自己接回来（resets +1、回到 running）
    zrs1=$(zs hub.resets); [[ "$zrs1" =~ ^[0-9]+$ ]] || zrs1=0
    eq "ZH07 POST /diag/uart（要 UART 的别人）：受理（202 Accepted）" "202" "$(code POST /diag/uart '{}')"
    ub=""; for _ in $(seq 1 30); do sleep 1; ub=$(api /diag/uart | jq_ state); [ "$ub" == "done" ] && break; done
    eq "ZH07b 基准测试拿到了 UART、NCP 有回应（result=0、tx_blocked=false、ncp_alive=true）" "done 0 false true" \
       "$ub $(api /diag/uart | jq_ result) $(api /diag/uart | jq_ tx_blocked) $(api /diag/uart | jq_ ncp_alive)"
    st=""; for _ in $(seq 1 30); do sleep 1; st=$(zs hub.state); [ "$st" == "running" ] && [ "$(zs hub.resets)" != "$zrs1" ] && break; done
    eq "ZH07c 测完 zhub 自己接回 UART（resets +1、running、suspended=false）" "$((zrs1 + 1)) running false" "$(zs hub.resets) $st $(zs hub.suspended)"
    if command -v dig >/dev/null; then
        eq "B09 mDNS 不宣告 _zigbee-coordinator" "" \
           "$(dig +short +time=2 +tries=1 -p 5353 @"$GW" _zigbee-coordinator._tcp.local PTR 2>/dev/null | grep -v '^;')"
        eq "B10 mDNS 照常宣告 _http（对照：B09 不是因为查不到）" "yes" \
           "$(dig +short +time=2 +tries=1 -p 5353 @"$GW" _http._tcp.local PTR 2>/dev/null | grep -q WebUI && echo yes || echo no)"
    else
        echo "  ⏭️  没有 dig，跳过 mDNS 两条"; SKIP=$((SKIP+2))
    fi
fi

# Muse 设置与配对入口（直连型 + BITENGINE_ENABLE_MUSE_LINK，docs/15 设计 10.1、3.2）。不破坏现场：
#  - 只发非法的 SDK token（校验在落盘之前，不会覆盖真 token）；绝不写一个"合法"的假 token。
#  - 显示名改了会还原；/muse/pair 只在前提不满足（会回 409）时才发，前提都满足时跳过 —— 否则网关真的会重启进配对剖面。
MUSE=$( [ "$(code GET /muse/status)" == "200" ] && echo 1 || echo 0 )
if [ "$MUSE" = 1 ]; then
    echo ""; echo "── 直连型：Muse 设置与配对入口 ──"
    TOK0=$(api /muse/config | jq_ sdk_token)
    NAME0=$(api /muse/config | jq_ display_name)
    eq "M01 GET /muse/config 的 sdk_token 只报 set / unset" "yes" "$( [[ "$TOK0" == set || "$TOK0" == unset ]] && echo yes || echo no)"
    eq "M02 格式不对的 SDK token → 400 invalid_sdk_token" "400 invalid_sdk_token" \
       "$(r=$(C -X POST -w '\n%{http_code}' "http://$GW/api/v1/muse/config" -d '{"sdk_token":"mgst_tooshort"}'); echo "$(echo "$r" | tail -1) $(echo "$r" | head -1 | jq_ error)")"
    eq "M03 拒收之后 token 状态没变（校验在落盘之前）" "$TOK0" "$(api /muse/config | jq_ sdk_token)"
    eq "M04 显示名带换行 → 400" "400" "$(code POST /muse/config '{"display_name":"a\nb"}')"
    eq "M05 enabled 不是布尔 → 400" "400" "$(code POST /muse/config '{"enabled":"yes"}')"
    eq "M06 什么都没给 → 400" "400" "$(code POST /muse/config '{}')"
    eq "M07 改显示名生效（读回）" "API Test 名字" \
       "$(apip /muse/config '{"display_name":"API Test 名字"}' >/dev/null; api /muse/config | jq_ display_name)"
    if [[ "$NAME0" =~ ^bitEngine\ Hub\ [0-9A-F]{4}$ ]]; then apip /muse/config '{"display_name":""}' >/dev/null
    else apip /muse/config "{\"display_name\":$(python3 -c 'import json,sys; print(json.dumps(sys.argv[1]))' "$NAME0")}" >/dev/null; fi
    eq "M08 显示名还原" "$NAME0" "$(api /muse/config | jq_ display_name)"
    eq "M09 正常开机里取消配对 → 409 not_pairing" "409 not_pairing" \
       "$(r=$(C -X POST -w '\n%{http_code}' "http://$GW/api/v1/muse/pair/cancel" -d '{}'); echo "$(echo "$r" | tail -1) $(echo "$r" | head -1 | jq_ error)")"
    eq "M10 跨源 POST /muse/pair 被守卫先拦（403，不会真重启）" "403" \
       "$(C -o /dev/null -w '%{http_code}' -X POST -H 'Origin: http://evil.example' "http://$GW/api/v1/muse/pair" -d '{}')"
    if [ "$TOK0" == "unset" ]; then
        eq "M11 没设 SDK token 时 /muse/pair → 409 sdk_token_required" "409 sdk_token_required" \
           "$(r=$(C -X POST -w '\n%{http_code}' "http://$GW/api/v1/muse/pair" -d '{}'); echo "$(echo "$r" | tail -1) $(echo "$r" | head -1 | jq_ error)")"
    elif [ "$(api /muse/config | jq_ enabled)" == "false" ]; then
        eq "M11 Muse 没启用时 /muse/pair → 409 muse_disabled" "409 muse_disabled" \
           "$(r=$(C -X POST -w '\n%{http_code}' "http://$GW/api/v1/muse/pair" -d '{}'); echo "$(echo "$r" | tail -1) $(echo "$r" | head -1 | jq_ error)")"
    else
        echo "  ⏭  M11：前提都满足，发 /muse/pair 会真的重启进配对剖面 —— 跳过（配对走 TC-MU01）"; SKIP=$((SKIP+1))
    fi
    eq "M12 /status 带 muse_state=idle（正常开机）" "idle" "$(api /status | jq_ muse_state)"
    eq "M13 /status 带 blew_state=idle" "idle" "$(api /status | jq_ blew_state)"
    eq "M14 正常开机已把 BT 静态内存还给堆（策略 C，设计 2.6）" "true" "$(api /muse/status | jq_ blew.bt_mem_released)"
    eq "M15 策略是 isolated、这次不是配对剖面" "isolated false" "$(api /muse/status | jq_ blew.policy) $(api /muse/status | jq_ pair_boot)"
    eq "M16 /muse/status 报 BLE 窗口之后的 Thread 自检状态（设计 3.3）" "yes" \
       "$(s=$(api /muse/status | jq_ blew.selfcheck.state); [[ "$s" =~ ^(none|pending|pass|degraded_ack|degraded_children|degraded_both|inconclusive|no_thread|interrupted)$ ]] && echo yes || echo "no:$s")"
    # M3 会话（muse_link）：只看 REST 面，不解绑、不改状态（解绑会删令牌，要重新配对）
    LS=$(api /muse/status | jq_ link.state)
    eq "M17 /muse/status 的 link.state 是会话状态机的一个状态（4.2）" "yes" \
       "$( [[ "$LS" =~ ^(disabled|unpaired|wait_net|low_mem|fetch_vm|connecting|registering|online|backoff|refreshing|protocol_error)$ ]] && echo yes || echo "no:$LS")"
    # 两次读之间状态可能正好在变：不相等就隔 1 s 再比一次
    eq "M18 /status 末尾的 muse_link 与 /muse/status 的 link.state 相同（同一个状态快照）" "yes" \
       "$(for i in 1 2; do a=$(api /status | jq_ muse_link); b=$(api /muse/status | jq_ link.state); [ "$a" == "$b" ] && [ -n "$a" ] && { echo yes; break; }; [ $i == 2 ] && echo "no:$a/$b"; sleep 1; done)"
    eq "M19 /muse/unpair 不带确认 → 400 confirm_required（防误点，不会真解绑）" "400 confirm_required" \
       "$(r=$(C -X POST -w '\n%{http_code}' "http://$GW/api/v1/muse/unpair" -d '{"confirm":"yes"}'); echo "$(echo "$r" | tail -1) $(echo "$r" | head -1 | jq_ error)")"
    eq "M20 跨源 POST /muse/unpair 被守卫先拦（403；红线 5：守卫在认证之前）" "403" \
       "$(C -o /dev/null -w '%{http_code}' -X POST -H 'Origin: http://evil.example' "http://$GW/api/v1/muse/unpair" -d '{"confirm":"UNPAIR"}')"
    # docs/15 附录 E33：安全模式退出时"先关掉 Muse"。正常开机里先判安全模式 → 409，而且什么都没记下（读回 hold_pending）。
    # 真进安全模式走一遍要故障注入构建（docs/test_cases_watchdog.md TC-W08c），这里只钉"不在安全模式不写"
    if [ "$(api /status | jq_ safe_mode)" == "false" ]; then   # 板子正停在安全模式时这条会真的退出、关掉 Muse、重启：先确认
        eq "M20b 正常开机里 safemode/exit 带 muse_off → 409 not_in_safe_mode（不会真重启）" "409 not_in_safe_mode" \
           "$(r=$(C -X POST -w '\n%{http_code}' "http://$GW/api/v1/diag/safemode/exit" -d '{"muse_off":true}'); echo "$(echo "$r" | tail -1) $(echo "$r" | head -1 | jq_ error)")"
        eq "M20c 被拒之后没留下\"关掉 Muse\"的留言（hold_pending=false）" "false" "$(api /muse/status | jq_ hold_pending)"
    else
        echo "  ⏭  M20b–c：板子在安全模式，跳过（会真退出）"; SKIP=$((SKIP+2))
    fi
    eq "M20d 跨源 POST safemode/exit 被守卫先拦（403）" "403" \
       "$(C -o /dev/null -w '%{http_code}' -X POST -H 'Origin: http://evil.example' "http://$GW/api/v1/diag/safemode/exit" -d '{"muse_off":true}')"
    # 7.5 生效断言（14.5）：在线时改显示名，会话真的重新注册（registrations 加一），再改回去。不在线跳过。
    # 前面的用例（发非法 SDK token 之类）会让会话重连一次：先等它回到在线（最多 60 s），别每次都跳过（M5 第三步发现 M21 历来都被跳过）
    for i in $(seq 1 60); do [ "$(api /muse/status | jq_ link.state)" == "online" ] && break; sleep 1; done
    if [ "$(api /muse/status | jq_ link.state)" == "online" ]; then
        NM0=$(api /muse/config | jq_ display_name); R0=$(api /muse/status | jq_ link.registrations)
        apip /muse/config '{"display_name":"API Test Rereg"}' >/dev/null
        R1=$R0; for i in $(seq 1 45); do R1=$(api /muse/status | jq_ link.registrations); [ "$R1" -gt "$R0" ] 2>/dev/null && break; sleep 1; done
        eq "M21 在线时改显示名 → 重新注册（registrations $R0 → 加一，45 s 内）" "yes" "$( [ "$R1" -gt "$R0" ] 2>/dev/null && echo yes || echo "no:$R0→$R1")"
        if [[ "$NM0" =~ ^bitEngine\ Hub\ [0-9A-F]{4}$ ]]; then apip /muse/config '{"display_name":""}' >/dev/null
        else apip /muse/config "{\"display_name\":$(python3 -c 'import json,sys; print(json.dumps(sys.argv[1]))' "$NM0")}" >/dev/null; fi
        eq "M22 显示名还原" "$NM0" "$(api /muse/config | jq_ display_name)"
        # M5 第三步（CHECKLIST C17）：注册用的后端就绪位 = zhub 在跑就 1 + 4（ZHUB | CONTROL：第七步控制路径做了，命令表带上 update 与 control），入网的位（8）还没报
        # M7 第三步：matter_ctrl 在跑再加 2（MCTRL：注册 hub.matter.commission）；控制器开机约 40 s 才起，位一变 Muse 就重注册
        zst=$(api /zigbee/status | jq_ hub.started)
        mst=$(api /matter/status | jq_ state)
        zbe=$( { [ "$zst" == "true" ] && echo 5 || echo 0; } ); [ "$mst" == "running" ] && zbe=$((zbe + 2))
        rb=""; for i in $(seq 1 15); do rb=$(api /muse/status | jq_ link.reg_backends); [ "$rb" == "$zbe" ] && break; sleep 1; done
        eq "M22b 在线时注册的后端位 reg_backends = $zbe（zhub started=$zst、matter_ctrl $mst；ZHUB | CONTROL [| MCTRL]，没有 ZB_JOIN 位：permit_join 不注册）" "$zbe" "$rb"
    else
        echo "  ⏭  M21–M22b：Muse 不在线（$(api /muse/status | jq_ link.state)），重新注册的生效断言跳过"; SKIP=$((SKIP+3))
    fi
    # 推送（4.9、10.1）：不在线 409；在线时真发一条会出现在用户的 Muse 里 —— 默认跳过，MUSE_TEST_PUSH=1 才发
    eq "M23 跨源 POST /muse/test_push 被守卫先拦（403）" "403" \
       "$(C -o /dev/null -w '%{http_code}' -X POST -H 'Origin: http://evil.example' "http://$GW/api/v1/muse/test_push" -d '{}')"
    # M22 改回显示名会触发重新注册（关会话 → 重连），这时立刻取样几乎一定读到 connecting：等它稳定（最多 45 s）再分支（核实审计 ❌）
    LS2=$(api /muse/status | jq_ link.state)
    for i in $(seq 1 45); do [[ "$LS2" =~ ^(fetch_vm|connecting|registering)$ ]] || break; sleep 1; LS2=$(api /muse/status | jq_ link.state); done
    if [ "$LS2" != "online" ] && [ "${MUSE_TEST_PUSH:-0}" == "1" ]; then
        echo "  ⏭  M24–M26：要求真发测试推送，但 Muse 不在线（$LS2）—— 跳过，别当成通过"; SKIP=$((SKIP+3))
    elif [ "$LS2" != "online" ]; then
        EXP="409 not_online"; [ "$LS2" == "not_running" ] && EXP="409 muse_not_running"
        eq "M24 不在线时 /muse/test_push → $EXP" "$EXP" \
           "$(r=$(C -X POST -w '\n%{http_code}' "http://$GW/api/v1/muse/test_push" -d '{}'); echo "$(echo "$r" | tail -1) $(echo "$r" | head -1 | jq_ error)")"
    elif [ "${MUSE_TEST_PUSH:-0}" == "1" ]; then
        P0=$(api /muse/status | jq_ link.pushes.sent)
        eq "M24 在线时 /muse/test_push → 202 queued" "202 queued" \
           "$(r=$(C -X POST -w '\n%{http_code}' "http://$GW/api/v1/muse/test_push" -d '{}'); echo "$(echo "$r" | tail -1) $(echo "$r" | head -1 | jq_ status)")"
        P1=$P0; for i in $(seq 1 40); do P1=$(api /muse/status | jq_ link.pushes.sent); [ "$P1" -gt "$P0" ] 2>/dev/null && break; sleep 1; done
        eq "M25 测试推送真的送达（link.pushes.sent $P0 → 加一，40 s 内）" "yes" "$( [ "$P1" -gt "$P0" ] 2>/dev/null && echo yes || echo "no:$P0→$P1")"
        # TC-MU13 板上的一条：网关不去重、不限配额（2026-10-24 用户定）—— 马上再点一次，照样送达，丢掉的不变
        D0=$(api /muse/status | jq_ link.pushes.dropped); S0=$(api /muse/status | jq_ link.pushes.sent)
        C -o /dev/null -X POST "http://$GW/api/v1/muse/test_push" -d '{}'
        S1=$S0; for i in $(seq 1 40); do S1=$(api /muse/status | jq_ link.pushes.sent); [ "$S1" -gt "$S0" ] 2>/dev/null && break; sleep 1; done   # 计数 10 s 采一次
        eq "M26 紧接着再发一条同样的测试推送：照样送达（sent 加一、dropped 不变 —— 网关不去重）" "$((S0+1)) $D0" "$S1 $(api /muse/status | jq_ link.pushes.dropped)"
    else
        echo "  ⏭  M24–M26：Muse 在线，真发测试推送要 MUSE_TEST_PUSH=1（会出现在 Muse 的主对话里；推送一律进主会话，设计 6.7）"; SKIP=$((SKIP+3))
    fi
    # M3 第七步：权限位 / 推送类别、本地出口 /hub/invoke、审计（6.5、6.10、10.1、MD-9）—— 每条都读回生效后的状态（7.5）
    eq "M27 /muse/config 带 perm 与 notify（报警恒推）" "yes" \
       "$(c=$(api /muse/config); [ "$(echo "$c" | jq_ perm.control)" != "" ] && [ "$(echo "$c" | jq_ notify.alarm)" == "true" ] && echo yes || echo "no:$c")"
    eq "M28 notify 关不掉报警 → 400 alarm_cannot_be_disabled" "400 alarm_cannot_be_disabled" \
       "$(r=$(C -X POST -w '\n%{http_code}' "http://$GW/api/v1/muse/config" -d '{"notify":{"alarm":false}}'); echo "$(echo "$r" | tail -1) $(echo "$r" | head -1 | jq_ error)")"
    eq "M29 perm 里拼错的键 → 400 invalid_perm（不悄悄忽略）" "400 invalid_perm" \
       "$(r=$(C -X POST -w '\n%{http_code}' "http://$GW/api/v1/muse/config" -d '{"perm":{"contrl":true}}'); echo "$(echo "$r" | tail -1) $(echo "$r" | head -1 | jq_ error)")"
    # 读回存储不够（7.5）：/muse/status 的 notify_mask 是会话任务读进 hub_events 的那份（GATEWAY = bit 4 = 16），异步，最多等 12 s。
    # set -u 下 $(( null & 16 )) 会让整个脚本退出 —— 退在"关掉"之后、"还原"之前，设备就停在网关事件不推送（第七步审计）：
    # 读数先过数字检查；G0 读不出 true/false（旧固件 / 一时超时）就整段跳过，不改、也不发会被 400 拒掉的还原
    nm_bit() { local v; v=$(api /muse/status | jq_ notify_mask); [[ "$v" =~ ^[0-9]+$ ]] && echo $((v & 16)) || echo "?"; }
    G0=$(api /muse/config | jq_ notify.gateway)
    if [ "$G0" == "true" ] || [ "$G0" == "false" ]; then
        apip /muse/config '{"notify":{"gateway":false}}' >/dev/null
        GM="?"; for i in $(seq 1 12); do GM=$(nm_bit); [ "$GM" = 0 ] && break; sleep 1; done
        eq "M30 关掉 GATEWAY 推送：存储读回 + 生效掩码里那一位也清了" "false 0" "$(api /muse/config | jq_ notify.gateway) $GM"
        apip /muse/config "{\"notify\":{\"gateway\":$G0}}" >/dev/null
        WANT=$([ "$G0" == "true" ] && echo 16 || echo 0)
        for i in $(seq 1 12); do GM=$(nm_bit); [ "$GM" = "$WANT" ] && break; sleep 1; done
        eq "M31 还原（存储 + 生效掩码）" "$G0 $WANT" "$(api /muse/config | jq_ notify.gateway) $GM"
    else
        echo "  ⏭  M30–M31：读不出 notify.gateway（「$G0」，旧固件或一时超时）—— 不改设置"; SKIP=$((SKIP+2))
    fi
    eq "M32 /hub/invoke device.health → 200、带固件版本（MD-9 本地出口走同一个处理函数）" "yes" \
       "$(r=$(C -X POST "http://$GW/api/v1/hub/invoke" -d '{"command":"device.health"}'); [ "$(echo "$r" | jq_ ok)" == "true" ] && [ -n "$(echo "$r" | jq_ result.firmware)" ] && echo yes || echo "no:$r")"
    eq "M33 /hub/invoke 不认识的命令 → 404 unsupported" "404 unsupported" \
       "$(r=$(C -X POST -w '\n%{http_code}' "http://$GW/api/v1/hub/invoke" -d '{"command":"device.ota"}'); echo "$(echo "$r" | tail -1) $(echo "$r" | head -1 | jq_ error)")"
    eq "M34 /hub/invoke 参数类型错 → 400 invalid_params" "400 invalid_params" \
       "$(r=$(C -X POST -w '\n%{http_code}' "http://$GW/api/v1/hub/invoke" -d '{"command":"hub.device.get","params":{"id":5}}'); echo "$(echo "$r" | tail -1) $(echo "$r" | head -1 | jq_ error)")"
    eq "M35 跨源 POST /hub/invoke 被守卫先拦（403；红线 5）" "403" \
       "$(C -o /dev/null -w '%{http_code}' -X POST -H 'Origin: http://evil.example' "http://$GW/api/v1/hub/invoke" -d '{"command":"device.health"}')"
    eq "M36 /hub/devices → 200、devices 是数组" "yes" \
       "$(r=$(api /hub/devices); echo "$r" | python3 -c 'import json,sys; d=json.load(sys.stdin); print("yes" if isinstance(d.get("result",{}).get("devices"), list) else "no")' 2>/dev/null || echo "no:$r")"
    # 审计真的记了（生效断言）：刚才的 /hub/invoke device.ota 应该是一条 rest / unsupported
    eq "M37 审计环记下了本地调用（rest、命令名、结果码）" "yes" \
       "$(api '/muse/audit?limit=10' | python3 -c 'import json,sys; e=json.load(sys.stdin)["entries"]; print("yes" if any(x["source"]=="rest" and x["command"]=="device.ota" and x["result"]=="unsupported" for x in e) else "no")' 2>/dev/null || echo no)"
    eq "M38 /muse/audit?limit=0 → 400 invalid_limit" "400" "$(code GET '/muse/audit?limit=0')"
    # 14.5：在线时改权限位 → 重新注册（registrations 加一），再改回去。不在线跳过
    PM0=$(api /muse/config | jq_ perm.manage)
    if [ "$(api /muse/status | jq_ link.state)" == "online" ] && { [ "$PM0" == "true" ] || [ "$PM0" == "false" ]; }; then   # 读不出现值就不改（还原会被 400 拒）
        R0=$(api /muse/status | jq_ link.registrations)
        NEW=$([ "$PM0" == "true" ] && echo false || echo true)
        apip /muse/config "{\"perm\":{\"manage\":$NEW}}" >/dev/null
        R1=$R0; for i in $(seq 1 45); do R1=$(api /muse/status | jq_ link.registrations); [ "$R1" -gt "$R0" ] 2>/dev/null && break; sleep 1; done
        eq "M39 在线时改权限位 → 重新注册（registrations $R0 → 加一，14.5）" "yes" "$( [ "$R1" -gt "$R0" ] 2>/dev/null && echo yes || echo "no:$R0→$R1")"
        apip /muse/config "{\"perm\":{\"manage\":$PM0}}" >/dev/null
        eq "M40 权限位还原" "$PM0" "$(api /muse/config | jq_ perm.manage)"
    else
        echo "  ⏭  M39–M40：Muse 不在线（或读不出 perm.manage），改权限位后重新注册的生效断言跳过"; SKIP=$((SKIP+2))
    fi
    # 事件管线 / Thread 清单 / 审计环在常驻任务 hub_svc 里（第十七节"M4 之前"）：VERIFY 只证明代码形状，这里证明它真的在转（7.5）。
    # 10 s 一轮：隔 12 s 读两次 rounds 必须涨；Muse 关着时也涨 —— 才算真的不再挂在 Muse 会话任务上
    num() { [[ "$1" =~ ^[0-9]+$ ]] && echo "$1" || echo -1; }
    H0=$(num "$(api /muse/status | jq_ hub_svc.rounds)")
    eq "M41 hub_svc 在跑（/muse/status 的 hub_svc.running）" "true" "$(api /muse/status | jq_ hub_svc.running)"
    sleep 12
    H1=$(num "$(api /muse/status | jq_ hub_svc.rounds)")
    MEN=$(api /muse/config | jq_ enabled)
    eq "M42 hub_svc 的轮数 12 s 内在涨（$H0 → $H1；Muse ${MEN:-?}）" "yes" "$( [ "$H0" -ge 0 ] && [ "$H1" -gt "$H0" ] && echo yes || echo "no:$H0→$H1")"
    eq "M43 Thread 清单刷新过（thread_inventory.refreshes ≥ 1，由 hub_svc 刷）" "yes" \
       "$( [ "$(num "$(api /muse/status | jq_ thread_inventory.refreshes)")" -ge 1 ] && echo yes || echo no)"
    if [ "$MEN" != "false" ]; then
        echo "  ℹ️  Muse 开着：M42 没单独证明\"关着 Muse 也转\"（不替用户关 Muse；关着时跑一遍 test_api 就是那条证明）"
    fi
fi

# Matter 控制器（直连型 M7；docs/15 第九节）。只做不改设备的断言：状态字段、作业接口的守卫与错误码、移除接口的输入校验。
# 配网本身要有人从别的 App 开分享窗口，不在这里跑（scratchpad 的 m7.sh；hub.matter.commission 还限速 1 次 / 60 s）
if [ "$(code GET /matter/status)" == "200" ]; then
    echo ""; echo "── Matter 控制器（/matter/status、/hub/job、/hub/device/remove）──"
    mst() { api /matter/status | jq_ "$1"; }
    MS=""; for i in $(seq 1 20); do MS=$(mst state); [ "$MS" == "running" ] && break; sleep 3; done
    eq "MC1 /matter/status：控制器在跑（开机约 40 s 才起）" "running" "$MS"
    eq "MC2 节点表开机读回了（nodes_ok；false 时这次开机不配网、不移除）" "true" "$(mst nodes_ok)"
    eq "MC3 厂商号是开发期的 CSA 测试号 0xFFF1（TODO T9.52）" "65521" "$(mst vendor_id)"
    eq "MC4 fabric ID 是 16 位十六进制、非 0" "yes" "$(mst fabric_id | grep -qE '^[0-9A-F]{16}$' && [ "$(mst fabric_id)" != "0000000000000000" ] && echo yes || echo no)"
    echo "  ℹ️  Matter：$(api /matter/status)"
    eq "MC5 GET /hub/job 不带 id → 400" "400" "$(code GET /hub/job)"
    eq "MC6 GET /hub/job?id=4000000000（没有这个作业）→ 404" "404" "$(code GET '/hub/job?id=4000000000')"
    eq "MC7 跨源 POST /hub/device/remove 被守卫先拦（403）" "403" \
       "$(C -o /dev/null -w '%{http_code}' -X POST -H 'Origin: http://evil.example' "http://$GW/api/v1/hub/device/remove" -d '{}')"
    if [ "$(C "http://$GW/api/v1/auth/status" | jq_ password_required)" != "false" ]; then
        eq "MC8 移除不带 confirm=REMOVE → 400" "400" "$(code POST /hub/device/remove '{"id":"mt-0000000000000002"}')"
        eq "MC9 移除不存在的节点 → 404（不发 RemoveFabric）" "404" "$(code POST /hub/device/remove '{"id":"mt-00000000000FFFFF","confirm":"REMOVE"}')"
        eq "MC10 移除 Zigbee 设备 → 404 unsupported（ZDO Leave 还没做）" "404" "$(code POST /hub/device/remove '{"id":"zb-0011223344556677","confirm":"REMOVE"}')"
    else
        echo "  ⏭  MC8–MC10：没设管理密码（移除要密码，回 409）"; SKIP=$((SKIP+3))
    fi
    # 10.1 的两个本地操作（MC22 测试设备开关、重置 Matter 控制器）：开关打开再关上、每次读回生效；重置在身份没丢时必须被拒（绝不能真重置）
    eq "MC25 /matter/status 报 allow_test_devices，默认关" "false" "$(mst allow_test_devices)"
    eq "MC26 跨源 POST /matter/allow_test_devices、/matter/reset 被守卫先拦（403）" "403 403" \
       "$(C -o /dev/null -w '%{http_code}' -X POST -H 'Origin: http://evil.example' "http://$GW/api/v1/matter/allow_test_devices" -d '{"allow":true}') $(C -o /dev/null -w '%{http_code}' -X POST -H 'Origin: http://evil.example' "http://$GW/api/v1/matter/reset" -d '{"confirm":"no"}')"   # 不带真的 RESET：守卫哪天回归了也只回 400，不会重置
    if [ "$(C "http://$GW/api/v1/auth/status" | jq_ password_required)" != "false" ]; then
        AT0=$(mst allow_test_devices)                     # 用户自己开着的话测完恢复原值
        A1=$(C -X POST -H 'Content-Type: application/json' "http://$GW/api/v1/matter/allow_test_devices" -d '{"allow":true}' | jq_ allow); S1=$(mst allow_test_devices)
        A0=$(C -X POST -H 'Content-Type: application/json' "http://$GW/api/v1/matter/allow_test_devices" -d '{"allow":false}' | jq_ allow); S0=$(mst allow_test_devices)
        [ "$AT0" == "true" ] && C -X POST -H 'Content-Type: application/json' "http://$GW/api/v1/matter/allow_test_devices" -d '{"allow":true}' > /dev/null
        eq "MC27 测试设备开关：开 → 读回 true，关 → 读回 false（测完恢复原值 $AT0）" "true true false false" "$A1 $S1 $A0 $S0"
        if [ "$(mst state)" == "running" ]; then   # ⚠️ 身份真丢了时这条会真的重置：只在确认控制器在跑时发（读不到状态也不发，审计）
            eq "MC28 身份没丢时重置被拒（409，不重启）；不带 RESET 确认 → 400" "409 400" \
               "$(code POST /matter/reset '{"confirm":"RESET"}') $(code POST /matter/reset '{"confirm":"yes"}')"
        else
            echo "  ⏭  MC28：控制器不在 running（可能是 ident_lost），不发重置请求（会真的重置）"; SKIP=$((SKIP+1))
        fi
    else
        echo "  ⏭  MC27–MC28：没设管理密码"; SKIP=$((SKIP+2))
    fi
    # MX21：配网码当场被拒（打错一位）不扣限速：连着两个坏码都回 invalid_params，第二个不是 rate_limited（不发起配网、不碰设备）
    if [ "$(C "http://$GW/api/v1/auth/status" | jq_ password_required)" != "false" ]; then
        BC1=$(C -X POST -H 'Content-Type: application/json' "http://$GW/api/v1/hub/invoke" -d '{"command":"hub.matter.commission","params":{"setup_code":"not-a-code"}}' | jq_ error)
        BC2=$(C -X POST -H 'Content-Type: application/json' "http://$GW/api/v1/hub/invoke" -d '{"command":"hub.matter.commission","params":{"setup_code":"0000"}}' | jq_ error)
        reg "MC24 配网码当场被拒不扣限速（MX21）：连着两个坏码都是 invalid_params" "invalid_params invalid_params" "$BC1 $BC2"
    else
        echo "  ⏭  MC24：没设管理密码（配网要密码）"; SKIP=$((SKIP+1))
    fi
    # 第四步：mt- 设备进 hub.devices.list / hub.device.get（7.3、9.6）。只断言形状与一致性；改了房间立刻改回去
    minv() { local r i; for i in 1 2 3 4 5 6; do r=$(C -X POST -H 'Content-Type: application/json' "http://$GW/api/v1/hub/invoke" -d "$1"); echo "$r" | grep -q '"rate_limited"' || break; sleep 3; done; echo "$r"; }
    ML=$(minv '{"command":"hub.devices.list","params":{"protocol":"matter"}}')
    eq "MC11 Matter 设备的列表项：id 是小写 mt-<16 hex>、protocol matter、有设备类与能力数组、访谈状态合法、有 controllable；条数 = 节点表里配好的" "yes" "$(echo "$ML" | python3 -c "
import json,re,sys
d=json.load(sys.stdin)['result']['devices']; n=int(sys.argv[1]) if sys.argv[1].isdigit() else -1
ok=all(re.fullmatch(r'mt-[0-9a-f]{16}',e['id']) and e['protocol']=='matter' and e.get('kind') and isinstance(e.get('capabilities'),list)
       and e['matter']['interview'] in ('pending','running','done') and isinstance(e.get('controllable'), bool)
       and e['matter']['node_id'].lower()==e['id'][3:] for e in d)
print('yes' if ok and len(d)<=n else 'no:%d/%d' % (len(d),n))" "$(mst nodes)" 2>/dev/null || echo no)"
    MT1=$(echo "$ML" | python3 -c "import json,sys; d=json.load(sys.stdin)['result']['devices']; print(d[0]['id'] if d else '')" 2>/dev/null)
    if [ -n "$MT1" ]; then
        MA=$(minv '{"command":"hub.devices.list"}')
        eq "MC12 全部列表：id 不重复、mt- 一条不少（本网关 fabric 上的 Thread 成员不再另按 th- 列，7.3）" "yes" "$(python3 -c "
import json,sys
a=[e['id'] for e in json.loads(sys.argv[1])['result']['devices']]; m=[e['id'] for e in json.loads(sys.argv[2])['result']['devices']]
print('yes' if len(a)==len(set(a)) and set(m)<=set(a) else 'no')" "$MA" "$ML" 2>/dev/null || echo no)"
        MH=$(minv '{"command":"device.health"}')
        MHV=$(python3 -c '
import json,sys
a=json.loads(sys.argv[1])["result"]; h=json.loads(sys.argv[2])["result"]["devices"]
print("yes" if a.get("truncated") or h.get("partial") or h["total"]==len(a["devices"]) else "no:%s/%d" % (h["total"],len(a["devices"])))' "$MA" "$MH" 2>/dev/null || echo no)
        eq "MC12b device.health 的设备总数 = 列表条数（mt- 只数一次，不再连同它的 th- 数两遍）" "yes" "$MHV"
        MG=$(minv "{\"command\":\"hub.device.get\",\"params\":{\"id\":\"$MT1\"}}")
        eq "MC13 hub.device.get $MT1：同一个 id；访谈完的带端点（含根端点 0）" "yes" "$(echo "$MG" | python3 -c "
import json,sys
r=json.load(sys.stdin)['result']; m=r['matter']
print('yes' if r['id']=='$MT1' and (m['interview']!='done' or any(e['endpoint']==0 for e in m['endpoints'])) else 'no')" 2>/dev/null || echo no)"
        eq "MC14 device.get：大写 MT- 也认；不存在的节点 / 坏 id → not_found" "$MT1 not_found not_found" \
           "$(minv "{\"command\":\"hub.device.get\",\"params\":{\"id\":\"$(echo "$MT1" | tr a-z A-Z)\"}}" | jq_ result.id) $(minv '{"command":"hub.device.get","params":{"id":"mt-00000000000fffff"}}' | jq_ error) $(minv '{"command":"hub.device.get","params":{"id":"mt-00000000000000zz"}}' | jq_ error)"
        MR0=$(echo "$MG" | jq_ result.room)
        minv "{\"command\":\"hub.device.update\",\"params\":{\"id\":\"$MT1\",\"room\":\"Test room MC15\"}}" > /dev/null
        MR1=$(minv "{\"command\":\"hub.device.get\",\"params\":{\"id\":\"$MT1\"}}" | jq_ result.room)
        MRL=$(minv '{"command":"hub.devices.list","params":{"room":"test room mc15"}}' | python3 -c "import json,sys; print(' '.join(e['id'] for e in json.load(sys.stdin)['result']['devices']))" 2>/dev/null)
        minv "{\"command\":\"hub.device.update\",\"params\":{\"id\":\"$MT1\",\"room\":\"$MR0\"}}" > /dev/null
        eq "MC15 hub.device.update 改 mt- 的房间：读回生效、按房间过滤列得出来；改回原值（\"$MR0\"）" "Test room MC15 $MT1 $MR0" \
           "$MR1 $MRL $(minv "{\"command\":\"hub.device.get\",\"params\":{\"id\":\"$MT1\"}}" | jq_ result.room)"
        MC1C=$(echo "$MG" | python3 -c "import json,sys; c=json.load(sys.stdin)['result']['capabilities']; print(c[0] if c else '')" 2>/dev/null)
        if [ -n "$MC1C" ]; then
            MCL=$(minv "{\"command\":\"hub.devices.list\",\"params\":{\"capability\":\"$MC1C\"}}")
            MCV=$(python3 -c '
import json,sys
d=json.loads(sys.argv[1])["result"]["devices"]
print("yes" if sys.argv[2] in [e["id"] for e in d] and all(sys.argv[3] in e["capabilities"] for e in d) else "no")' "$MCL" "$MT1" "$MC1C" 2>/dev/null || echo no)
            eq "MC16 按能力过滤（$MC1C）：列得出 $MT1，列出来的每台都有这个能力" "yes" "$MCV"
        else
            echo "  ⏭  MC16：$MT1 还没访谈出能力"; SKIP=$((SKIP+1))
        fi
        # 第五步：订阅（9.5）。开机后控制器约 40 s 起、照表开订（同一时刻 2 条在建；ICD 要等它醒）：最多等 120 s 全部建好
        SU=""; for i in $(seq 1 40); do SU=$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print("yes" if d.get("subs",0)>0 and d["subs"]==d.get("subs_up") else "no:%s/%s" % (d.get("subs_up"),d.get("subs")))' "$(api /matter/status)" 2>/dev/null); [ "$SU" == "yes" ] && break; sleep 3; done
        eq "MC17 /matter/status：访谈完的设备都订上了（subs_up = subs）" "yes" "$SU"
        ML2=$(minv '{"command":"hub.devices.list","params":{"protocol":"matter"}}')
        MSV=$(python3 -c '
import json,sys
d=json.loads(sys.argv[1])["result"]["devices"]
bad=[e["id"] for e in d if e["matter"]["interview"]=="done" and not (e.get("online") is True and e.get("state") and set(e["state"])==set(e.get("state_age_s",{})) and isinstance(e.get("last_seen_s"),int))]
print("yes" if d and not bad else "no:"+",".join(bad))' "$ML2" 2>/dev/null || echo no)
        eq "MC18 订上的设备：online true、有 last_seen_s、state 非空、state_age_s 的键与 state 一致" "yes" "$MSV"
        MCV2=$(python3 -c '
import json,sys
d=json.loads(sys.argv[1])["result"]["devices"]
ok=all(e["state"].get("contact") in ("open","closed") for e in d if "contact" in e.get("state",{}))
print("yes" if ok else "no")' "$ML2" 2>/dev/null || echo no)
        eq "MC19 门磁读数是 \"open\" / \"closed\"（同 Zigbee 的键与取值）" "yes" "$MCV2"
        # 第六步：控制（9.5「命令」）。只把开关设成它现在的状态（不改设备的实际状态）：本来就是目标、订阅建好着 = confirmed
        MCT=$(python3 -c '
import json,sys
d=json.loads(sys.argv[1])["result"]["devices"]
c=[e for e in d if e.get("controllable") and "onoff" in e.get("capabilities",[]) and "onoff" in e.get("state",{})]
n=[e for e in d if "onoff" not in e.get("capabilities",[])]
print((c[0]["id"]+" "+("turn_on" if c[0]["state"]["onoff"] else "turn_off")) if c else "-", n[0]["id"] if n else "-",
      "yes" if all(e.get("controllable") == ("onoff" in e.get("capabilities",[]) or "level" in e.get("capabilities",[])) for e in d) else "no")' "$ML2" 2>/dev/null)
        read -r MCID MCACT MCNO MCCTL <<< "$MCT"
        eq "MC20 controllable = 能开关或调亮度（Matter 控制路径，第六步）" "yes" "$MCCTL"
        if [ "$MCID" != "-" ] && [ -n "$MCACT" ]; then
            MCR=$(minv "{\"command\":\"hub.device.control\",\"params\":{\"id\":\"$MCID\",\"action\":\"$MCACT\"}}")
            MCRV=$(python3 -c '
import json,sys
r=json.loads(sys.argv[1]).get("result",{})
print("yes" if r.get("result")=="confirmed" and r.get("state",{}).get("onoff")==(sys.argv[2]=="turn_on") else "no:"+sys.argv[1][:160])' "$MCR" "$MCACT" 2>/dev/null || echo no)
            eq "MC21 hub.device.control $MCID $MCACT（设成它现在的状态）：confirmed、回读的 state 一致" "yes" "$MCRV"
            eq "MC22 没有亮度簇的设备 set_brightness → invalid_params；不存在的 mt- → not_found；不认识的动作 → invalid_params" "invalid_params not_found invalid_params" \
               "$(minv "{\"command\":\"hub.device.control\",\"params\":{\"id\":\"$MCID\",\"action\":\"set_brightness\",\"value\":\"40\"}}" | jq_ error) $(minv '{"command":"hub.device.control","params":{"id":"mt-00000000000fffff","action":"turn_on"}}' | jq_ error) $(minv "{\"command\":\"hub.device.control\",\"params\":{\"id\":\"$MCID\",\"action\":\"fly\"}}" | jq_ error)"
        else
            echo "  ⏭  MC21–MC22：没有订上、能开关的 Matter 设备"; SKIP=$((SKIP+2))
        fi
        if [ "$MCNO" != "-" ] && [ -n "$MCNO" ]; then
            eq "MC23 没有开关的设备（$MCNO）turn_on → invalid_params（不发命令）" "invalid_params" "$(minv "{\"command\":\"hub.device.control\",\"params\":{\"id\":\"$MCNO\",\"action\":\"turn_on\"}}" | jq_ error)"
        else
            echo "  ⏭  MC23：没有不带开关的 Matter 设备"; SKIP=$((SKIP+1))
        fi
    else
        echo "  ⏭  MC12–MC23：网关 fabric 上还没有 Matter 设备（配一台：scratchpad 的 m7.sh commission）"; SKIP=$((SKIP+12))
    fi
fi

# hubdb / hub_store（直连型开发固件才有 /diag/hubdb：CONFIG_BITENGINE_DIAG_HUBDB，docs/15 设计 2.5）。
# 每条都读回来断言（7.5）：写进去的值读得到、两次连写只落一次、删了就没了、计数与之相符。
if [ "$(code GET /diag/hubdb)" != "404" ]; then
    echo ""; echo "── hubdb / hub_store（/diag/hubdb）──"
    hdb() { api /diag/hubdb | jq_ "$1"; }
    eq "H01 hub_store 就绪"                  "true" "$(hdb ready)"
    eq "H02 开机没擦过 hubdb（reset_at_init）" "false" "$(hdb reset_at_init)"
    eq "H02b 重启前落盘的钩子登记上了（shutdown handler 槽位只有 5 个）" "true" "$(hdb flush_on_restart)"
    W0=$(hdb writes); CO0=$(hdb coalesced)
    P1="probe-$RANDOM"
    eq "H03 写探针（排队，2 s 防抖）"          "queued" "$(apip /diag/hubdb "{\"text\":\"$P1\"}" | jq_ result)"
    eq "H04 读己之写：防抖中也读得到"           "$P1" "$(hdb probe)"
    eq "H05 防抖中还挂着一条"                 "1" "$(hdb pending)"
    apip /diag/hubdb '{"text":"q\"uote\\back"}' >/dev/null
    # 先读探针：那次 GET 的读请求排在第二次写后面，读回来时写已处理完；状态快照取在读之前，所以计数下一次再看
    eq "H06 引号与反斜杠原样读回（JSON 转义）"   'q"uote\back' "$(hdb probe)"
    eq "H07 2 s 内连写第二次 → 合并（coalesced +1）" "$((CO0+1))" "$(hdb coalesced)"
    sleep 3
    eq "H08 防抖到期：两次写只落一次（writes +1）" "$((W0+1))" "$(hdb writes)"
    eq "H09 落盘后不再挂着"                   "0" "$(hdb pending)"
    eq "H10 超过 64 字节 → 400"               "400" "$(code POST /diag/hubdb "{\"text\":\"$(printf 'x%.0s' $(seq 65))\"}")"
    eq "H11 什么都没给 → 400"                 "400" "$(code POST /diag/hubdb '{}')"
    eq "H12 删除（不防抖）"                   "erase_queued" "$(apip /diag/hubdb '{"erase":true}' | jq_ result)"
    eq "H13 flush 立即落盘"                  "flushed" "$(apip /diag/hubdb '{"flush":true}' | jq_ result)"
    eq "H14 删了就读不到"                    "ESP_ERR_NOT_FOUND" "$(hdb probe_err)"
    eq "H15 删除也算一次写（writes +2）"       "$((W0+2))" "$(hdb writes)"
    eq "H16 没有写失败、没有被拒"              "0 0" "$(hdb failures) $(hdb refused_newer)"
    eq "H17 跨域 POST 被挡（CROSS_ORIGIN_GUARD 在认证之前）" "403" \
       "$(C -o /dev/null -w '%{http_code}' -X POST -H 'Origin: http://evil.example' "http://$GW/api/v1/diag/hubdb" -d '{"flush":true}')"
fi

C "http://$GW/api/v1/config/export" -o "$BACKUP"
# 判据 6 要在**空闲**条件下读（docs/test_cases_scale_baseline.md）：本脚本自己会在
# 「C 桶四根线」那段把 ZHA 踢掉让它重连，一次重连实测就值 1316 字节（5319→4003，
# v341 与带地区功能的版本都精确停在这个数）。所以在**动任何东西之前**先取一次，
# 那才是判据想管的"这台设备的余量"。水位自启动单调，早读晚读都是同一个下界。
DMA_IDLE=$(api /diag/heap | python3 -c "import json,sys;print(json.load(sys.stdin)['dma']['min_free'])" 2>/dev/null)
# 只查"非空"不够：401 的错误体 `{"error":"login_required"}` 也是非空的，
# 会被当成合法备份存下来，然后脚本放心地去改配置 —— 那才是真出事的路径。
python3 -c "
import json,sys
d = json.load(open('$BACKUP'))
missing = [k for k in ('system','wifi','zigbee','led') if k not in d]
if missing: sys.exit('缺少字段: ' + ','.join(missing))
" 2>/dev/null || { echo "  ❌ 备份内容不是合法配置（可能是错误响应），中止"; head -c 120 "$BACKUP"; echo; exit 1; }
echo "  配置已备份（$(wc -c < "$BACKUP") 字节）"

# ───────────────────────── 云端更新 ─────────────────────────
# ⚠️ 不触发 /api/v1/ota/check —— 它是同步联网的，会把整个 Web 界面卡住最长 6s，
# 而后面还有几十条断言要跑。它的行为要真验，得有一台可控的 manifest 服务器，
# 属人工用例（docs/guide/云端OTA设计.md 第六节）。
# 这里验的是三件不需要网络、但足以证明"这条路真的接上了"的事。
echo "── 云端更新 ──"

# 1) 路由不再是 501 桩。曾经 /api/v1/ota/check 注册了但返回 not_yet_implemented
eq "GET /ota/cloud/status 可用" "200" "$(code GET /ota/cloud/status)"
CLOUD_STATE=$(api /ota/cloud/status | jq_ state)
[ -n "$CLOUD_STATE" ] && [ "$CLOUD_STATE" != "null" ] \
    && ok "cloud status 回了 state=$CLOUD_STATE" \
    || bad "cloud status 应有 state 字段" "非空" "$CLOUD_STATE"

# 2) 没查到可装版本时必须拒绝安装，且是 409 不是 500 —— 这条同时钉住了
#    "install 不会在 s_latest 为空时起任务"这个 S3 不变量
eq "无可用更新时 POST /ota/cloud 回 409" "409" "$(code POST /ota/cloud)"

# 4) www 镜像形状校验（审计 A19 / TODO T2.1）：不是按本分区生成的 SPIFFS 镜像必须 400。
#    100 字节垃圾在长度那一关就被拒（spiffsgen 输出恒等于分区大小），且不能是 500。
eq "POST /ota/www 100 字节垃圾回 400" "400" \
   "$(C -o /dev/null -w '%{http_code}' -X POST "http://$GW/api/v1/ota/www" -H 'Content-Type: application/octet-stream' --data-binary @<(head -c 100 /dev/zero))"

# 5) NCP 走云端：没查到可装的 NCP 固件时必须 409 —— 与 (2) 同一个 S3 不变量
eq "无 NCP 更新时 POST /ota/cloud/ncp 回 409" "409" "$(code POST /ota/cloud/ncp)"

# 3) 防降级判据存在。install 的判据是 cmp(latest, current) > 0，
#    真值表在 components/ota_cloud/test_host/run.sh（不用板子，23 条）
eq "current_version 回的是本机固件版本" \
   "$(api /status | jq_ fw_version)" "$(api /ota/cloud/status | jq_ current_version)"

# ───────────────────────── 固件签名（OTA 前置）─────────────────────────
# 本项目 OTA 的信任锚是**运行中 app 自己的签名块**，不是 eFuse
# （CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT，见 sdkconfig.defaults 末尾）。
# 刷进未签名固件的设备会拒绝一切后续 OTA，且外部零症状 —— 用户第一次点升级
# 才发现，那时已经出货，只能拆机串口重刷。
# 这条断言是「这台机器还能不能被远程升级」的唯一出口，必须放在最前面：
# 它红了，后面所有 OTA 相关的结论都不成立。
echo "── 固件签名 ──"
eq "运行固件带签名块（否则本机永久无法 OTA）" "true" "$(api /status | jq_ app_signed)"

# ─────────────────────────────── LED ───────────────────────────────
echo ""; echo "── LED 配置 ──"
eq "亮度部分更新"        "30"    "$(apip /led/config '{"brightness":30}' | jq_ brightness)"
eq "缺省字段保持原值"    "30"    "$(apip /led/config '{}' | jq_ brightness)"
eq "亮度超范围被拒"      "400"   "$(code POST /led/config '{"brightness":300}')"
eq "亮度类型错被拒"      "400"   "$(code POST /led/config '{"brightness":"x"}')"
eq "时分超范围被拒"      "400"   "$(code POST /led/config '{"night_start_hour":24}')"
# color_* 是有意不接的：渲染器用固定色语义，接了也不生效 = 假功能
eq "声明颜色不可配"      "false" "$(apip /led/config '{}' | jq_ colors_configurable)"

r=$(apip /led/nightmode '{"enabled":true,"start":"03:07","end":"04:09"}')
eq "夜间模式 HH:MM 解析"  "03:07" "$(echo "$r" | jq_ night_start)"
eq "夜间模式结束时间"     "04:09" "$(echo "$r" | jq_ night_end)"
eq "宽松时间串被拒"       "400"   "$(code POST /led/nightmode '{"start":"3:7"}')"
eq "非法时间被拒"         "400"   "$(code POST /led/nightmode '{"start":"25:00"}')"
eq "带垃圾后缀被拒"       "400"   "$(code POST /led/nightmode '{"start":"03:07x"}')"

echo ""; echo "── LED 灯位映射 / 位时序 ──"
eq "灯位表有 4 项"        "4"     "$(api /led/test | python3 -c 'import json,sys;print(len(json.load(sys.stdin)["leds"]))')"
eq "index 0 是 sys"       "sys"   "$(api /led/test | python3 -c 'import json,sys;print(json.load(sys.stdin)["leds"][0]["name"])')"
eq "index 3 是 net"       "net"   "$(api /led/test | python3 -c 'import json,sys;print(json.load(sys.stdin)["leds"][3]["name"])')"
eq "全部灯的 index 是 -1" "-1"    "$(api /led/test | jq_ all_index)"
T1H=$(api /led/timing | jq_ t1h_ns)
eq "T1L = 1200 - T1H"     "$((1200-T1H))" "$(api /led/timing | jq_ t1l_ns)"
# T0H>=T1H 会让 0 和 1 分不开，setter 必须拦住
eq "T0H>=T1H 被拒"        "400"   "$(code POST /led/timing '{"t0h_ns":900,"t1h_ns":500}')"
eq "低电平不足被拒"       "400"   "$(code POST /led/timing '{"t1h_ns":1150}')"
apip /led/timing "{\"t1h_ns\":$T1H}" > /dev/null   # 还原（时序只在内存，稳妥起见仍复位）

# SYS 灯语义。能跑到这一行说明网关在网，那 SYS 就绝不该还是 boot ——
# 是 boot 就说明 led_ctrl 又错过了 NETWORK_EVENT_ONLINE（默认事件循环不重放，
# 注册晚于 network_mgr 就永远收不到），且不会自愈。
# 2026-08-13 实测现象：SYS 黄常亮，而 zigbee/thread/net 三颗全绿，整机功能正常。
# 只断言"不是 boot"而不锁定具体值：normal/night/error 都可能是当时的合法状态。
LS=$(api /status | jq_ led_state)
if [ "$LS" != "boot" ] && [ "$LS" != "<非JSON>" ] && [ -n "$LS" ]; then
    ok "SYS 灯已离开 boot（当前 $LS）  [防回归]"
else
    bad "SYS 灯卡在 boot  [防回归·曾经的真 bug]" "≠ boot" "$LS"
fi

# ─────────────────────────────── MQTT ───────────────────────────────
echo ""; echo "── MQTT ──"
eq "部分更新只改 port"    "8883"  "$(apip /mqtt/config '{"port":8883}' | jq_ port)"
eq "其余字段未被清空"     "bitengine" "$(api /mqtt/status | jq_ topic_prefix)"
# ★ T9.33：写进配置的字符串必须是合法 UTF-8（WiFi SSID 除外）。body 里直接放原始坏字节
#   （浏览器发不出，脚本和别的客户端发得出）：必须 400、配置一个字节都不动。
#   坏字节一旦进 NVS，/config/export 就吐出非法 JSON —— 10-07 T9.32 第一版断言就是这么
#   把后面 12 条靠导出读值的断言连带打红的；所以万一真被收进去了，这里也当场还原。
#   （T9.32 的 json_sanitize_copy 修复在板上已经没有可注入的入口了，行为由
#    components/webui/test_host 对照 Python 钉住。）
#   ⚠️ WiFi 密码 / 管理密码的拒收**不在这里测**：判据万一失效，前者会换掉 WiFi 凭据、
#   后者会把管理员锁在门外。那两处由 webui / config_mgr 的 VERIFY 钉形状。
PREV_USER_BODY=$(api /mqtt/status | python3 -c 'import json,sys;print(json.dumps({"username":json.load(sys.stdin)["username"]}))')
PREV_USER=$(api /mqtt/status | jq_ username)
reg "MQTT 用户名含非法 UTF-8 → 400（T9.33）" "400" \
   "$(printf '{"username":"\xff\xfet933"}' | C -o /dev/null -w '%{http_code}' -X POST "http://$GW/api/v1/mqtt/config" --data-binary @-)"
NOW_USER=$(api /mqtt/status | jq_ username)
eq "被拒后用户名没动" "$PREV_USER" "$NOW_USER"
[ "$NOW_USER" != "$PREV_USER" ] && apip /mqtt/config "$PREV_USER_BODY" > /dev/null   # 万一收进去了，当场还原
reg "导入含非法 UTF-8 的备份 → 整份拒绝（T9.33）" "400" \
   "$(printf '{"version":1,"ntp":{"server":"t933.example"},"mqtt":{"username":"\xff"}}' | C -o /dev/null -w '%{http_code}' -X POST "http://$GW/api/v1/config/import" --data-binary @-)"
eq "整份拒绝：同一份里合法的 ntp.server 也没被写进去" "no" \
   "$(api /ntp | grep -q 't933.example' && echo yes || echo no)"
[ "$(api /mqtt/status | jq_ username)" != "$PREV_USER" ] && apip /mqtt/config "$PREV_USER_BODY" > /dev/null
# 合法输入没被误伤：当前导出是合法 UTF-8 的 JSON；SSID 合法时不带 ssid_hex（格式与以前一样）
reg "导出是合法 UTF-8 JSON、合法 SSID 不带 ssid_hex（T9.33）" "ok" \
   "$(C "http://$GW/api/v1/config/export" | python3 -c '
import json,sys
d=json.loads(sys.stdin.buffer.read().decode("utf-8"))   # 严格：非法 UTF-8 在这里就炸
w=d.get("wifi",{})
print("ok" if "ssid_hex" not in w else "带了 ssid_hex，ssid=%r" % w.get("ssid"))' 2>&1 | tail -1)"
apip /mqtt/config '{"password":"regression-secret"}' > /dev/null
# 密码只能以 password_set 布尔形式出现，绝不回显明文
reg "状态不回显密码明文"  ""      "$(api /mqtt/status | grep -o 'regression-secret')"
eq "密码已设标志"         "true"  "$(api /mqtt/status | jq_ password_set)"
eq "port 0 被拒"          "400"   "$(code POST /mqtt/config '{"port":0}')"
eq "port 越界被拒"        "400"   "$(code POST /mqtt/config '{"port":70000}')"
eq "发布间隔过小被拒"     "400"   "$(code POST /mqtt/config '{"publish_interval":1}')"
eq "启用但无 broker 被拒" "400"   "$(code POST /mqtt/config '{"enabled":true}')"
# 超长输入必须拒绝而不是静默截断：截断后的 broker 语法合法但指向别处
LONG=$(python3 -c "print('a'*300)")
reg "超长 broker 被拒"    "400"   "$(code POST /mqtt/config "{\"broker\":\"$LONG\"}")"
HUGE=$(python3 -c "print('x'*900)")
reg "超大 body 回 413"    "413"   "$(code POST /mqtt/config "{\"broker\":\"$HUGE\"}")"

# ─────────────────────────────── Webhook ───────────────────────────────
echo ""; echo "── Webhook ──"
for u in 'file:///etc/passwd' 'ftp://x/a' 'javascript:alert(1)' 'notaurl'; do
    reg "URL scheme 拦截 $u" "400" "$(code POST /webhook/config "{\"url\":\"$u\"}")"
done
r=$(apip /webhook/config '{"url":"https://example.com/h","enabled":true,"event_temp_high":false,"backup_interval":3600}')
eq "合法 https 接受"      "true"  "$(echo "$r" | jq_ enabled)"
eq "事件开关可单独关"     "false" "$(echo "$r" | jq_ event_temp_high)"
eq "其余事件开关未受影响" "true"  "$(echo "$r" | jq_ event_zigbee_crash)"
eq "备份间隔过小被拒"     "400"   "$(code POST /webhook/config '{"backup_interval":60}')"
eq "启用但无 URL 被拒"    "400"   "$(code POST /webhook/config '{"enabled":true,"url":""}')"

# ─────────────────────────────── NTP ───────────────────────────────
echo ""; echo "── NTP / 时区 ──"
# ★ 这组是本脚本存在的理由：2026-08-13 之前 CONFIG_EVENT_NTP_CHANGED 没有消费者，
#   改时区只是存进 NVS 不生效，而 VERIFY 的 "NTP_CHANGED posted" 一直是绿的。
#   唯一能发现它的办法就是改完去读回**设备真实的本地时间**。
t0=$(apip /ntp '{"timezone":"UTC0"}' | jq_ local_time)
t8=$(apip /ntp '{"timezone":"XXX-8"}' | jq_ local_time)   # POSIX 符号相反：-8 = UTC+8
DIFF=$(python3 -c "
from datetime import datetime as D
a=D.strptime('$t0','%Y-%m-%d %H:%M:%S'); b=D.strptime('$t8','%Y-%m-%d %H:%M:%S')
print(round(((b-a).total_seconds()/60) % 1440 / 60))" 2>/dev/null)
reg "改时区真的改变设备时间（差 8 小时）" "8" "$DIFF"
eq "tz_declared 随时区置位"  "true"  "$(api /ntp | jq_ tz_declared)"
# 只改 server/interval 不该把时区标成"用户确认过"，否则就是拿默认值冒充
reg "只改周期不置 tz_declared" "1800" "$(apip /ntp '{"sync_interval":1800}' | jq_ sync_interval)"
eq "同步周期过小被拒"     "400"   "$(code POST /ntp '{"sync_interval":10}')"
eq "同步周期过大被拒"     "400"   "$(code POST /ntp '{"sync_interval":999999}')"
eq "服务器不可为空"       "400"   "$(code POST /ntp '{"server":""}')"
eq "时区不可为空"         "400"   "$(code POST /ntp '{"timezone":""}')"
eq "synced 字段存在"      "true"  "$(api /ntp | jq_ synced)"

# ★ 导入配置必须让时区**立刻生效**，不能只写进 NVS。
#   2026-08-13 实测：恢复备份后 NVS 是对的、运行时 TZ 还是旧值，因为没人处理
#   CONFIG_EVENT_IMPORTED。这条断言就是钉住那个修复的。
apip /ntp '{"timezone":"XXX-8"}' > /dev/null              # 先把运行时 TZ 弄脏
TZ_BAK="$(mktemp /tmp/tzcheck.XXXX.json)"
C "http://$GW/api/v1/config/export" -o "$TZ_BAK"
python3 - "$TZ_BAK" <<'PY'
import json,sys
p=sys.argv[1]; d=json.load(open(p)); d['ntp']['timezone']='UTC0'; json.dump(d,open(p,'w'))
PY
C -X POST "http://$GW/api/v1/config/import" --data-binary @"$TZ_BAK" > /dev/null
sleep 1
reg "导入配置后时区立刻生效" "UTC0" "$(api /ntp | jq_ timezone)"
# 真正的判据不是字段值（那只证明存进去了），而是设备时间真的按新时区走
H_UTC=$(api /ntp | jq_ local_time | cut -d' ' -f2 | cut -d: -f1)
H_REF=$(date -u +%H)
reg "导入后设备时间按新时区走"  "$H_REF" "$H_UTC"
rm -f "$TZ_BAK"

# ─────────────────────────────── PoE 供电检测 ───────────────────────────────
echo ""; echo "── PoE 供电检测 ──"
eq "/poe/status 不再是 501" "200"  "$(code GET /poe/status)"
P=$(api /poe/status)
# poe_present 必须与 probe.no_pull 一致 —— 判定只认这一格，另外两格不参与
reg "判定只取 no_pull"      "$(echo "$P" | jq_ probe.no_pull)" "$(echo "$P" | jq_ poe_present)"
eq "source 与 poe_present 自洽" "yes" \
   "$(echo "$P" | python3 -c 'import json,sys
d=json.load(sys.stdin)
print("yes" if (d["source"]=="poe")==d["poe_present"] else "no")')"
# 曾经用 pull_up 判"引脚悬空"，实测证明分辨不了（正常无 PoE 时上拉也读 1），
# 会误报硬件故障。这条钉住"不许再加回来"。
reg "不再输出 floating 判定" "yes" "$(echo "$P" | grep -q floating && echo no || echo yes)"
eq "上报分压参数供核算"     "yes"  "$(echo "$P" | grep -q '0.595' && echo yes || echo no)"
# 三格探测读数必须都在
eq "probe 三格齐全"         "3"    "$(echo "$P" | python3 -c 'import json,sys;print(len(json.load(sys.stdin)["probe"]))')"

# ─────────────────────────────── 系统日志 ───────────────────────────────
echo ""; echo "── 系统日志 ──"
eq "/logs 不再是 501"     "200"   "$(code GET /logs)"
L=$(api "/logs?limit=5")
eq "回包给 capacity"      "600"   "$(echo "$L" | jq_ capacity)"
eq "limit 生效"           "5"     "$(echo "$L" | python3 -c 'import json,sys;print(len(json.load(sys.stdin)["entries"]))')"
eq "limit 上限封顶 100"   "100"   "$(api '/logs?limit=9999' | python3 -c 'import json,sys;print(min(len(json.load(sys.stdin)["entries"]),100))')"
# ★ 端到端：制造一条日志，确认它真的被抓到了。
#   只断言"端点返回 200"证明不了捕获链路работ —— 那正是本文件存在的理由。
SEQ=$(api /logs | jq_ next_seq)
apip /ntp '{"sync_interval":3601}' > /dev/null      # 这个会打一条 "SNTP: server=..."
sleep 1
reg "新产生的日志能被抓到" "yes" \
    "$(api "/logs?since=$((SEQ-1))&limit=50" | grep -q 'SNTP' && echo yes || echo no)"
# 级别过滤
reg "level 过滤只回该级别" "yes" \
    "$(api '/logs?level=W&limit=20' | python3 -c '
import json,sys
e=json.load(sys.stdin)["entries"]
print("yes" if e and all(x["level"]=="W" for x in e) else ("yes" if not e else "no"))')"
# 正文里的引号/反斜杠必须转义而不是剥离，否则 JSON 会碎或日志被改写
reg "回包是合法 JSON"      "ok"   "$(api '/logs?limit=100' | python3 -c 'import json,sys;json.load(sys.stdin);print("ok")' 2>&1 | tail -1)"
# ★ T9.24：坏字节进了日志，回包也必须是合法 UTF-8。注入路径是真实的那条 ——
#   跨站守卫在认证之前，**不用登录**就能把 \xff 塞进 Host 头、被原样写进 W 日志。
#   读回时必须严格解码 stdin.buffer：文本模式的 stdin 在某些 locale 下是
#   surrogateescape，会把坏字节悄悄吞掉，这条就恒绿了。
MARK="t924-$$"
curl -s -m 5 -o /dev/null -X POST "http://$GW/api/v1/led/config" -H $'Host: \xff\xfe'"$MARK" -d '{}'
sleep 1
reg "坏字节进日志后回包仍是合法 UTF-8（T9.24）" "ok" \
   "$(api '/logs?level=W&limit=100' | MARK="$MARK" python3 -c '
import json,os,sys
d=json.loads(sys.stdin.buffer.read().decode("utf-8"))      # 严格：非法 UTF-8 在这里就炸
hit=[e["msg"] for e in d["entries"] if os.environ["MARK"] in e["msg"]]
print("ok" if hit and "�" in hit[-1] else "注入那条没进日志或没换成替换字符: %r" % hit[-1:])' 2>&1 | tail -1)"
# 不传 since 必须给**最新的** limit 条（tail 语义）。曾经给的是最旧的，
# 导致拿 ?limit=100 找刚产生的日志翻到的全是开机日志，白白怀疑了一轮功能。
reg "不传 since 给最新的" "yes" \
   "$(api '/logs?limit=3' | python3 -c 'import json,sys
d=json.load(sys.stdin); e=d["entries"]
print("yes" if e and e[-1]["seq"] >= d["next_seq"]-10 else "no")')"

# ─────────────────────────────── 白名单 ───────────────────────────────
echo ""; echo "── TCP 白名单 ──"
if [ "$NO_BRIDGE" = 1 ]; then
    echo "  ⏭️  直连型没有 :6638，白名单不编（上面 B02 已断言不在）"; SKIP=$((SKIP+1))
else
r=$(apip /whitelist '{"enabled":true,"ips":["192.168.222.11","192.168.222.12"]}')
eq "两个 IP 写入"         "2"     "$(echo "$r" | jq_ count)"
eq "启用且非空 → active"  "true"  "$(echo "$r" | jq_ active)"
eq "整表替换"             "1"     "$(apip /whitelist '{"ips":["10.0.0.7"]}' | jq_ count)"
eq "只关开关保留列表"     "1"     "$(apip /whitelist '{"enabled":false}' | jq_ count)"
# tcp_server 的判断是 `enabled && count>0`：开了但表空其实不过滤，必须如实上报
reg "启用+空表 → active=false" "false" "$(apip /whitelist '{"enabled":true,"ips":[]}' | jq_ active)"
eq "超上限被拒"           "400"   "$(code POST /whitelist '{"ips":["1.1.1.1","1.1.1.2","1.1.1.3","1.1.1.4","1.1.1.5","1.1.1.6","1.1.1.7","1.1.1.8","1.1.1.9"]}')"
eq "非法 IP 被拒"         "400"   "$(code POST /whitelist '{"ips":["999.1.1.1"]}')"
eq "非数组被拒"           "400"   "$(code POST /whitelist '{"ips":"192.168.1.1"}')"
apip /whitelist '{"enabled":false,"ips":[]}' > /dev/null   # 绝不把"启用"状态留在板上
fi   # NO_BRIDGE

# ────────────────────── 依赖 scheduler tick 的用例 ──────────────────────
echo ""; echo "── 夜间模式时间窗（需 scheduler 60s tick）──"
if [ $SLOW -eq 1 ]; then
    H=$(api /ntp | jq_ local_time | cut -d' ' -f2 | cut -d: -f1)
    N=$(printf "%02d" $(( (10#$H + 1) % 24 )))
    apip /led/nightmode "{\"enabled\":true,\"start\":\"${H}:00\",\"end\":\"${N}:00\"}" > /dev/null
    sleep 65
    reg "窗口含当前 → night_active" "true" "$(apip /led/config '{}' | jq_ night_active)"
    apip /led/nightmode '{"enabled":true,"start":"03:00","end":"03:30"}' > /dev/null
    sleep 65
    reg "窗口不含当前 → 不生效"     "false" "$(apip /led/config '{}' | jq_ night_active)"
    apip /led/nightmode '{"enabled":false}' > /dev/null
else
    echo "  ⏭  跳过（加 --slow 启用，约 +2.5 分钟）"; SKIP=$((SKIP+2))
fi

# ─────────────── Thread 网络密钥存活（2026-08-15 事故防回归）───────────────
# 恢复出厂曾经擦掉 psa_its —— OpenThread 网络密钥材料的实际存放处。后果极其隐蔽：
# 数据集、ChildInfo、ExtPANID、网络名、信道全部完好，专门写的恢复出厂检查脚本
# 7/7 全绿，唯独密钥没了，所有 Thread 设备的报文被回 "Failed to process UDP -
# Security"，整网永久失联。密钥是**唯一**能暴露这个故障的字段，所以单独断言。
echo ""; echo "── Thread 网络密钥 ──"
DS_HEX=$(C -H 'Accept: text/plain' "http://$GW/node/dataset/active")
if [ -z "$DS_HEX" ] || [ "${#DS_HEX}" -lt 20 ]; then
    echo "  ⏭  跳过（Thread 未启用或无数据集）"; SKIP=$((SKIP+1))
else
    NETKEY=$(python3 -c "
import sys
b=bytes.fromhex('$DS_HEX'); i=0; out=''
while i < len(b):
    t=b[i]; l=b[i+1]; v=b[i+2:i+2+l]
    if t == 5: out = v.hex()          # TLV type 5 = NetworkKey
    i += 2+l
print(out or '<缺失>')" 2>/dev/null)
    if [ "$NETKEY" == "00000000000000000000000000000000" ] || [ "$NETKEY" == "<缺失>" ]; then
        bad "网络密钥非全零  [防回归·曾经的真 bug]" "非全零的 16 字节" "$NETKEY"
    else
        ok "网络密钥非全零  [防回归]"
    fi
fi

# ────────────────── OTBR 路由的认证（2026-08-15 补全）──────────────────
# 25 条 OTBR 路由里此前只有 1 条有认证，GET /node/dataset/active 会把 Thread
# NetworkKey 和 PSKc 明文吐给局域网里任何人。加认证的风险是**反过来**把
# HA 的 OTBR 集成挡在门外，所以这里断言的是"没挡错人"。
#
# ⚠️ 只在**无密码**设备上有意义：check_auth() 在没设密码时恒真（auth.c:176），
#    所以这几条证明的是"加了宏没有把原本能通的路径弄坏"。
#    "设了密码就该 401" 那半没法在这里测 —— 设密码之后没有 API 能撤销，
#    只能恢复出厂（docs/00:145 的既定决策），放进脚本会把人锁在外面。
#    那半见 docs/test_cases_webui_config.md。
# ────────────────── 设备密码的长度策略 ──────────────────
# 只测**拒绝**路径：两条都不改任何状态，所以放进来是安全的。
# 上限 63 字节的推导见 components/webui/include/webui.h 的 PASSWORD_MAX_BYTES ——
# 改密码接口的 body 缓冲比登录的大一倍，中间那段能设出一个自己再也登不进来的
# 密码，而 set_password 立刻 SHA256、长度信息就没了，只能恢复出厂。
# ⚠️ 不测"63 字节能通过"：那条会真的改掉设备密码，脚本无法安全还原
#    （没有清除密码的 API，导入备份又会把旧密码设回去）。手工验过一次即可。
echo ""; echo "── 密码长度策略 ──"
PW_LONG=$(python3 -c "print('a'*64)")
reg "64 字节被拒（否则能设出登不进去的密码）" "password_too_long" \
    "$(apip /auth/password "{\"new_password\":\"$PW_LONG\"}" | jq_ error)"
eq  "3 字节被拒"  "password_too_short" "$(apip /auth/password '{"new_password":"abc"}' | jq_ error)"
# 被拒之后密码必须没变 —— 用当前凭据还能进就说明没被改
if [ -n "${GW_PASS:-}" ]; then
    reg "被拒后密码未被改动" "200" "$(C -o /dev/null -w '%{http_code}' "http://$GW/api/v1/status")"
else
    SKIP=$((SKIP+1))
fi

echo ""; echo "── OTBR 路由认证开关 ──"
# ⚠️ 不要断言"默认是关的"：rest_auth 是**用户可改并持久化**的设置，谁把它打开
#    谁就永久红。记录当前值，测完原样还原。
OTBR_WAS=$(api /otbr/auth | jq_ enabled)
echo "  ℹ️  otbr/auth 当前 = $OTBR_WAS（出厂默认 false）"
eq "非布尔被拒" "400" "$(code POST /otbr/auth '{"enabled":"yes"}')"

# 开关改动也要能被中途退出还原，挂进同一个 trap
otbr_restore() {
    [ -n "${OTBR_WAS:-}" ] && [ "$OTBR_WAS" != "<非JSON>" ] && \
        apip /otbr/auth "{\"enabled\":$OTBR_WAS}" > /dev/null 2>&1
}
trap 'otbr_restore; meta_restore; restore' EXIT

HAS_PW=$(C "http://$GW/api/v1/auth/status" | jq_ password_required)
if [ "$HAS_PW" == "false" ]; then
    # 无密码时开启认证 = 假保护（check_auth 恒真），必须直接拒绝
    reg "无密码时拒绝开启认证" "400"   "$(code POST /otbr/auth '{"enabled":true}')"
    eq  "被拒后仍是关的"      "false" "$(api /otbr/auth | jq_ enabled)"
    echo "  ⏭️  无密码，跳过开启后的行为断言（那半需要密码）"; SKIP=$((SKIP+5))
elif [ -z "${GW_PASS:-}" ]; then
    echo "  ⏭️  未提供 GW_PASS，跳过开启后的行为断言"; SKIP=$((SKIP+7))
else
    # ★ 这一段才是这个功能存在的意义：证明**开启后真的挡住了**。
    #   CLAUDE.md 7.5：setter 存了值不算数，要断言它真的作用到系统上。
    apip /otbr/auth '{"enabled":true}' > /dev/null
    eq "开启后读回是 true" "true" "$(api /otbr/auth | jq_ enabled)"
    # OTBR 路由在根路径上，不带 /api/v1 前缀，所以不能用 api()/code()
    for u in /node /node/dataset/active; do
        reg "开启后 :80 $u 无凭据 401"   "401" "$(curl -s -m 10 -o /dev/null -w '%{http_code}' "http://$GW$u")"
    done
    reg "开启后 :8081 /node 无凭据 401"  "401" "$(curl -s -m 10 -o /dev/null -w '%{http_code}' "http://$GW:8081/node")"
    reg "开启后带凭据仍 200"             "200" "$(C -o /dev/null -w '%{http_code}' "http://$GW/node")"
    # HA 的 python-otbr-api 每个方法开头都探这条，只接受 200/404；
    # 变 401 会让整个集成加载不起来。开着认证时也必须是 404。
    reg "开启后 /api/actions 仍是 404"   "404" "$(curl -s -m 10 -o /dev/null -w '%{http_code}' "http://$GW/api/actions")"

    apip /otbr/auth "{\"enabled\":$OTBR_WAS}" > /dev/null
    eq "已还原为测试前的值" "$OTBR_WAS" "$(api /otbr/auth | jq_ enabled)"
    # 关着时才有的断言：确认 HA 那条路通
    if [ "$OTBR_WAS" == "false" ]; then
        reg "还原后 :8081 /node 无凭据 200" "200" "$(curl -s -m 10 -o /dev/null -w '%{http_code}' "http://$GW:8081/node")"
    else
        SKIP=$((SKIP+1))
    fi
fi

echo ""; echo "── C 桶四根线（2026-08-17 接线）──"
# 1) 踢 TCP 客户端。**不主动踢真客户端** —— 收尾断言要求 tcp_clients 不变，
#    而 Z2M/ZHA 被踢后未必在脚本窗口内重连回来。只验错误路径与寻址语义。
if [ "$NO_BRIDGE" = 1 ]; then
    echo "  ⏭️  直连型没有 TCP 客户端，踢客户端与槽位号两项跳过（上面 B03 已断言端点不在）"; SKIP=$((SKIP+1))
else
eq "踢客户端：缺参数被拒"   "400" "$(code POST /zigbee/clients/disconnect '{}')"
eq "踢客户端：下标越界被拒" "400" "$(code POST /zigbee/clients/disconnect '{"index":99}')"
# 没连着的 IP 必须回 404 而不是 200 —— 否则点了"踢掉"什么也没发生却显示成功
reg "踢不存在的 IP 回 404"  "404" "$(code POST /zigbee/clients/disconnect '{"ip":"203.0.113.99"}')"
# /status 必须吐槽位号，否则按下标那条路无从寻址（数组是压缩快照，下标≠槽位）
# ⚠️ 无客户端时必须显式 SKIP，不能算通过 —— 空数组下"字段在不在"无从判定，
#    写成通过就是一条空泛断言（本条 2026-08-17 首次上板时正是这么骗过自己的）。
CIDX=$(api /zigbee/status | python3 -c "import json,sys
d=json.load(sys.stdin); c=d.get('clients') or []
print('none' if not c else ('yes' if 'index' in c[0] else 'no'))")
if [ "$CIDX" == "none" ]; then
    echo "  ⏭️  当前无桥接客户端，index 字段无从验证"; SKIP=$((SKIP+1))
else
    reg "clients 带槽位 index" "yes" "$CIDX"
fi
fi   # NO_BRIDGE

# 2) 统计清零：清完 uart_rx_bytes 必须比清之前小（桥在跑就会立刻重新涨，
#    所以比的是"清零点之后的值 < 清零前的累计值"，不是等于 0）
RX_BEFORE=$(api /status | jq_ uart_rx_bytes)
apip /zigbee/stats/reset '{}' > /dev/null
RX_AFTER=$(api /status | jq_ uart_rx_bytes)
if [ "$RX_BEFORE" -gt 0 ] 2>/dev/null; then
    [ "$RX_AFTER" -lt "$RX_BEFORE" ] \
        && ok "统计清零真的生效（$RX_BEFORE → $RX_AFTER）" \
        || bad "统计清零真的生效" "< $RX_BEFORE" "$RX_AFTER"
else
    echo "  ⏭️  uart_rx_bytes 本来就是 0，清零无从判定"; SKIP=$((SKIP+1))
fi

# 2k) Zigbee 活动闪（T4.4）。led_ctrl_notify_zigbee_activity() 以前**零调用者**，
#     而 docs/07 写着"Zigbee 活动蓝闪"。像素回读能机器验证：活动档是纯蓝
#     (0,0,B)，常态是纯绿 (0,G,0)，两者不会混。
#
#     ⚠️ **必须耐心采样**。实测桥上的 EZSP 流量是**每 8-10 秒一次突发**，中间
#        完全静默；蓝色只保持 120ms，所以占空比约 1%。按固定 40 次采样去测，
#        命中率只有 5%，跑三次红两次 —— 那不是固件坏了，是判据选错了。
#        现在的做法：每 50ms 采一次（短于 120ms 的保持时间，不会漏过窗口），
#        最多等 20 秒，见到就停。有流量时通常几秒内命中。
#     ⚠️ 用脚本自带的 api 助手，**别在内联 python 里自己拼 curl**：第一版那么写，
#        把 AUTH 数组展开错了，认证丢失导致其后二十多条断言一起变红，
#        看起来像固件坏了（2026-09-11）。
if [ "$(api /status | jq_ tcp_clients)" == "0" ]; then
    echo "  ⏭️  无桥接客户端，Zigbee 活动闪无从触发"; SKIP=$((SKIP+1))
else
    ZB_HIT=no
    ZB_DEADLINE=$(( $(date +%s) + 20 ))
    while [ "$(date +%s)" -lt "$ZB_DEADLINE" ]; do
        PX=$(api /led/test | python3 -c "
import json,sys
try: d=json.load(sys.stdin)
except Exception: print('x'); raise SystemExit
p=(d.get('pixels') or [[0,0,0]]*2)[1]
print('blue' if p[2] > 0 and p[1] == 0 else 'o')" 2>/dev/null)
        [ "$PX" == "blue" ] && { ZB_HIT=yes; break; }
        sleep 0.05
    done
    [ "$ZB_HIT" == "yes" ] \
        && ok "Zigbee 灯出现活动闪（桥上有流量时）" \
        || bad "Zigbee 灯出现活动闪" "20 秒内至少一次蓝" "一次都没有 —— notify 又没人调了？"
fi

# 2j) Thread 配置落盘（T3.3）。这是**真 bug** 不是没露出：PUT /node/state 以前
#     只改运行时状态，不落盘 —— 用户点 Disable 协议栈确实停了，但重启后
#     thread_br_init() 照着 cfg->enabled=true 又拉起来，且没有任何提示。
#     ⚠️ 本段**不真的关 Thread**（关了子设备要 4-6 分钟才归队）。只验配置端点
#        的读写往返与校验；"disable 活过重启"那条需要真重启，见 docs 的手工用例。
TC=$(api /thread/config)
case "$(echo "$TC" | jq_ enabled)" in
    true|false) ok "/thread/config 报出 enabled" ;;
    *) bad "/thread/config 报出 enabled" "true 或 false" "$TC" ;;
esac
TC_CH=$(echo "$TC" | jq_ channel)
[ "$TC_CH" -ge 11 ] 2>/dev/null && [ "$TC_CH" -le 26 ] \
    && ok "/thread/config 报出合法信道（$TC_CH）" \
    || bad "/thread/config 信道合法" "11-26" "$TC_CH"
# 回包必须说清 channel 的适用范围，否则用户改了以为生效了
reg "回包说明 channel 只对未组网生效" "yes" \
    "$(echo "$TC" | grep -q 'not-yet-formed' && echo yes || echo no)"
# 校验：越界信道要被拒，不能存进去
eq "信道 10 被拒(400)"   "400" "$(code POST /thread/config '{"channel":10}')"
eq "信道 27 被拒(400)"   "400" "$(code POST /thread/config '{"channel":27}')"
eq "enabled 非布尔被拒"  "400" "$(code POST /thread/config '{"enabled":"yes"}')"
eq "空对象被拒"          "400" "$(code POST /thread/config '{}')"
# 往返：写回当前信道（等值写入，不改变任何实际状态）
eq "写回当前信道被接受"  "ok"  "$(apip /thread/config "{\"channel\":$TC_CH}" | jq_ status)"
reg "读回仍是同一个信道" "$TC_CH" "$(api /thread/config | jq_ channel)"
eq "回包标注 takes_effect=reboot" "reboot" "$(apip /thread/config "{\"channel\":$TC_CH}" | jq_ takes_effect)"

# 2i) 主机名可改（T3.2）。此前 config_mgr_set_hostname() 与 mdns_svc_update_hostname()
#     **都是零调用者** —— 写好了从没被调过，设置页只把它显示成只读卡片。
#     ⚠️ 改名会换 mDNS 广播名。本脚本用 IP 访问，不受影响；测完还原。
HN_ORIG=$(api /status | jq_ hostname)
if [ -z "$HN_ORIG" ] || [ "$HN_ORIG" == "None" ]; then
    echo "  ⏭️  读不到当前主机名"; SKIP=$((SKIP+1))
else
    HN_TEST="${HN_ORIG}-t"
    eq "改主机名被接受" "ok" "$(apip /system/hostname "{\"hostname\":\"$HN_TEST\"}" | jq_ status)"
    reg "改完 /status 立刻是新名字" "$HN_TEST" "$(api /status | jq_ hostname)"
    # ⚠️ 校验必须长在 setter 里而不是 REST 处理器里 —— 否则配置导入能绕过去。
    #    这几条同时也在验"非法值不会进 mDNS 广播和 JSON 回包"。
    eq "空名被拒(400)"        "400" "$(code POST /system/hostname '{"hostname":""}')"
    eq "带点的名被拒(400)"    "400" "$(code POST /system/hostname '{"hostname":"a.b"}')"
    eq "带空格的名被拒(400)"  "400" "$(code POST /system/hostname '{"hostname":"a b"}')"
    eq "带引号的名被拒(400)"  "400" "$(code POST /system/hostname '{"hostname":"a\"b"}')"
    eq "连字符开头被拒(400)"  "400" "$(code POST /system/hostname '{"hostname":"-ab"}')"
    eq "超长名被拒(400)"      "400" "$(code POST /system/hostname '{"hostname":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}')"
    eq "缺字段被拒(400)"      "400" "$(code POST /system/hostname '{}')"
    # 还原
    apip /system/hostname "{\"hostname\":\"$HN_ORIG\"}" > /dev/null
    eq "还原回原名" "$HN_ORIG" "$(api /status | jq_ hostname)"
fi

# 2g) bridge_stats 计数器露出（T3.4）。14 个字段此前只出了 uart_rx/tx 两个。
#     tcp_client_drops 只算"TX ring buffer 满被踢"：非 0 说明是桥这边把慢客户端
#     踢了；为 0 不排除桥侧别的断开（只进 tcp_disconnects）。ncp_resets_detected
#     每次上位机握手都会 +1，不等于 NCP 自己重启。口径见 rest_api.c 同名字段的注释。
ZS=$(api /zigbee/status)
MISSING=""
for f in uart_rx_errors uart_tx_timeouts tcp_rx_bytes tcp_tx_bytes \
         tcp_connects tcp_disconnects tcp_client_drops \
         usb_rx_bytes usb_tx_bytes ncp_resets_detected; do
    echo "$ZS" | grep -q "\"$f\"" || MISSING="$MISSING $f"
done
[ -z "$MISSING" ] && ok "bridge_stats 十个计数器都露出了" \
                  || bad "bridge_stats 计数器齐全" "全部出现" "缺:$MISSING"
# 回包别撑爆缓冲：溢出会回 500，Zigbee 页整页打不开
ZS_LEN=$(echo -n "$ZS" | wc -c)
# 缓冲大小按配置从源码读（rest_api.c 的 handle_zigbee_status：直连型多一个 hub 对象，缓冲在 PSRAM 上、更大），不抄数（7.6）
ZS_CAP=$(python3 - "$(dirname "$0")/../components/webui/rest_api.c" "$NO_BRIDGE" <<'PY'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()
f = s[s.index("static esp_err_t handle_zigbee_status("):]
m = re.search(r"#if CONFIG_BITENGINE_ENABLE_ZHUB.*?enum \{ ZS_BUF = (\d+) \};.*?#else\s*char buf\[(\d+)\];", f, re.S)
print(m.group(1) if sys.argv[2] == "1" else m.group(2))
PY
)
[ "$ZS_LEN" -lt "${ZS_CAP:-0}" ] && ok "/zigbee/status 在缓冲内（$ZS_LEN / $ZS_CAP）" \
                       || bad "/zigbee/status 在缓冲内" "< $ZS_CAP" "$ZS_LEN"
# 清零要对新计数器同样生效
apip /zigbee/stats/reset '{}' > /dev/null
reg "清零后 tcp_client_drops 归零" "0" "$(api /zigbee/status | jq_ tcp_client_drops)"

# 2h) readvertise 回包说清 force_bye（T3.6）。出厂 FORCE_BYE=n 时
#     readvertise_one() 直接 return，什么都不做，而 REST 以前一律回 202 scheduled
#     —— 调用方无从知道这次调用不会有任何后续动作。
RV=$(api /thread/readvertise)
case "$(echo "$RV" | jq_ force_bye)" in
    true|false) ok "GET /thread/readvertise 报出 force_bye" ;;
    *) bad "GET /thread/readvertise 报出 force_bye" "true 或 false" "$RV" ;;
esac
RVP=$(apip /thread/readvertise '{}')
RVF=$(echo "$RVP" | jq_ force_bye)
if [ "$RVF" == "false" ]; then
    reg "FORCE_BYE 关时 POST 不谎称 scheduled" "tracking_only" "$(echo "$RVP" | jq_ status)"
elif [ "$RVF" == "true" ]; then
    eq "FORCE_BYE 开时 POST 回 scheduled" "scheduled" "$(echo "$RVP" | jq_ status)"
else
    echo "  ⏭️  Thread 未运行，readvertise 无从验证"; SKIP=$((SKIP+1))
fi

# 2f) OTA 槽位与回滚可见（T2.7）。回滚以前完全不可见：机制是对的（bootloader
#     会切回旧槽），但没人查过 —— 设备默默回到旧版本，用户只觉得"升级没生效"。
OTA_SLOT=$(api /status | jq_ ota_slot)
case "$OTA_SLOT" in
    ota_0|ota_1) ok "/status 报出运行槽位（$OTA_SLOT）" ;;
    *) bad "/status 的 ota_slot" "ota_0 或 ota_1" "$OTA_SLOT" ;;
esac
ROLLED=$(api /status | jq_ last_ota_rolled_back)
case "$ROLLED" in
    true|false) ok "last_ota_rolled_back 是布尔（$ROLLED）" ;;
    *) bad "last_ota_rolled_back 是布尔" "true 或 false" "$ROLLED" ;;
esac
# true 不算失败 —— 它是历史事实，但要让人看见
[ "$ROLLED" == "true" ] && echo "     ⚠️ 这台设备发生过 OTA 回滚，查 /api/v1/logs 找原因"

# 2g) WiFi 回传诊断（2026-10-05，上市前验证清单第 1 项"提供重连次数和断线时长的诊断"）。
#     真板上没法自动造一次 WiFi 断线（要动路由器），所以这里验形状与自洽；
#     "真的数对了"由 stability 测试 6 人工断一次路由器、看 wifi_disconnects +1 来证。
WS=$(api /status)
WDISC=$(echo "$WS" | jq_ wifi_disconnects)
case "$WDISC" in
    ''|null|*[!0-9]*) bad "/status 露出 wifi_disconnects" "整数（固件 ≥ v0.5.0-395）" "$WDISC" ;;
    *) ok "/status 露出 wifi_disconnects（$WDISC）" ;;
esac
eq "WiFi 诊断字段自洽" "yes" "$(echo "$WS" | python3 -c '
import json,sys; d=json.load(sys.stdin); bad=[]
ch, dfs = d.get("wifi_channel"), d.get("wifi_dfs")
if d.get("wifi_connected"):
    if not (isinstance(ch,int) and 1 <= ch <= 177): bad.append(f"在线但 wifi_channel={ch}")
    elif dfs is not (52 <= ch <= 144):             bad.append(f"信道 {ch} 的 wifi_dfs={dfs}")
else:
    if ch is not None or dfs is not None:          bad.append("离线时信道/DFS 应为 null")
n = d.get("wifi_disconnects")
if n == 0 and (d.get("wifi_last_outage_s") is not None or d.get("wifi_longest_outage_s") is not None):
    bad.append("没断过却有断线时长")
lo, lg = d.get("wifi_last_outage_s"), d.get("wifi_longest_outage_s")
if lo is not None and lg is not None and lo > lg: bad.append(f"上一次 {lo}s > 最长 {lg}s")
ds = d.get("wifi_down_s")
if d.get("wifi_connected") and d.get("wifi_ip","0.0.0.0") != "0.0.0.0" and ds != 0:
    bad.append(f"有 IP 却 wifi_down_s={ds}")
print("yes" if not bad else "; ".join(bad))')"
[ "$(echo "$WS" | jq_ wifi_dfs)" == "true" ] && echo "     ⚠️ 路由器在 DFS 信道上：雷达触发换信道时 WiFi 会断 1 分钟以上（手册第一节）"

# 2h) 开机接管以太网服务 IP（T9.20，2026-10-06）。真要验"接管了"得拔线重启，在
#     docs/test_cases_stability.md 测试 6d；这里验字段在、取值合法，以及一条不变量：
#     以太网本次拿到了地址，开机接管就该已退场（不会还停在 waiting / wait_clock / probing）。
eq "svc_takeover 取值合法且与以太网状态自洽" "yes" "$(echo "$WS" | python3 -c '
import json,sys; d=json.load(sys.stdin); v=d.get("svc_takeover")
ok = v in ("off","none","waiting","wait_clock","probing","taken") or (isinstance(v,str) and v.startswith("denied_"))
if not ok: print(f"svc_takeover={v!r}（固件 ≥ T9.20 才有）"); sys.exit()
e = d.get("eth_ip","0.0.0.0")
if e != "0.0.0.0" and v in ("waiting","wait_clock","probing"):
    print(f"以太网已有地址 {e}，svc_takeover 却是 {v}（应已退场）"); sys.exit()
print("yes")')"

# 2e) NVS 落盘失败可见（T2.6）。此前 CONFIG_EVENT_NVS_ERROR **零订阅者**，
#     落盘失败只剩一条 ESP_LOGE，用户看到的是"保存成功、重启后回退"。
#     ⚠️ 没法在真板上**制造**一次 NVS 写失败，所以这里只能验"字段在且类型对"，
#        以及健康板子上应为 0。真正的反向不变量（事件有订阅者、两处都记计数）
#        在 components/config_mgr/VERIFY.sh 里。
NVSF=$(api /status | jq_ nvs_write_failures)
case "$NVSF" in
    ''|*[!0-9]*) bad "nvs_write_failures 是数字" "整数" "$NVSF" ;;
    *) ok "/status 露出 nvs_write_failures（$NVSF）" ;;
esac
[ "$NVSF" == "0" ] && ok "健康板子上落盘失败次数为 0" \
                   || bad "健康板子上落盘失败次数为 0" "0" "$NVSF（这台设备保存设置时出过错，查 /api/v1/logs）"

# 2d) PoE 状态露出（T2.5）。此前 poe_is_present() **零调用者** —— /poe/status 有，
#     /status、WS、Dashboard、HA 集成全都没有。两个出口必须报同一件事。
PP_STATUS=$(api /status | jq_ poe_present)
PP_POE=$(api /poe/status | jq_ poe_present)
reg "/status 露出 poe_present"        "$PP_POE" "$PP_STATUS"
case "$PP_STATUS" in
    true|false) ok "poe_present 是布尔（不是字符串/null）" ;;
    *) bad "poe_present 是布尔" "true 或 false" "$PP_STATUS" ;;
esac
# 供电方式与 /poe/status 的 source 字段必须自洽
SRC=$(api /poe/status | jq_ source)
if [ "$PP_STATUS" == "true" ]; then
    eq "PoE 供电时 source=poe"   "poe" "$SRC"
else
    reg "非 PoE 时 source 不是 poe" "yes" "$([ "$SRC" != "poe" ] && echo yes || echo no)"
fi

# 2a) 出厂身份（2026-09-11 T1.3）。身份写在 fctry 分区，固件只读。
#     ⚠️ 本机这块板是 PoE 以太网、无串口，写不进 fctry，所以这里跑的是
#        **无身份**那条路径。有身份的路径靠 components/device_identity/test_host
#        和真板子上的产线流程（T2.3）覆盖。
SN=$(api /status | jq_ serial_number)
if [ "$SN" == "null" ]; then
    ok "无出厂身份时 /status 的 serial_number 是 null（不是空串）"
    # 没身份就必须在产线自检里红 —— 那是唯一拦得住"忘了写身份"的地方
    IDST=$(api /factory/selftest | python3 -c "import json,sys
d=json.load(sys.stdin)
print([i['state'] for i in d['items'] if i['name']=='identity'][0])" 2>/dev/null)
    eq "无身份时自检 identity 报 fail" "fail" "$IDST"
else
    # 有身份：形状必须过固件那套校验（字母数字分段、连字符分隔、≤31）
    echo "$SN" | grep -qE '^[A-Za-z0-9]+(-[A-Za-z0-9]+)+$' \
        && [ ${#SN} -le 31 ] \
        && ok "serial_number 形状合法（$SN）" \
        || bad "serial_number 形状合法" "字母数字分段、≤31" "$SN"
    IDST=$(api /factory/selftest | python3 -c "import json,sys
d=json.load(sys.stdin)
print([i['state'] for i in d['items'] if i['name']=='identity'][0])" 2>/dev/null)
    eq "有身份时自检 identity 报 pass" "pass" "$IDST"
fi
# 18②（2026-09-13）：eth_ping 从以太网口 ping 网关。板子插着网线时必须 pass；
# 没插（走 WiFi 跑本脚本）时它是 fail 而不是 manual —— 产测必须插网线，这里只在有链路时断言。
EP=$(api /factory/selftest | python3 -c "import json,sys
d=json.load(sys.stdin)
it=[i for i in d['items'] if i['name']=='eth_ping'][0]
print(it['state']+'|'+it.get('detail',''))" 2>/dev/null)
if [ "$(api /status | jq_ eth_link_up)" == "true" ]; then
    eq "有网线时自检 eth_ping 报 pass" "pass" "${EP%%|*}"
    echo "     ($EP)"
else
    ok "无网线，跳过 eth_ping 断言（${EP%%|*}）"
fi
# mDNS 的 serial_number 是 MAC，是 ZHA 认设备的键，绝不能被产品 SN 顶替。
# 这里从 /api2/info 侧面验：它报的 mac 必须仍是 MAC 形状。
if [ "$NO_BRIDGE" = 1 ]; then
    echo "  ⏭️  直连型没有 /api2（SMLIGHT 兼容层），MAC 形状这条跳过"; SKIP=$((SKIP+1))
else
reg "mDNS/兼容层的标识仍是 MAC 形状" "yes" \
    "$(C "http://$GW/api2/info" | python3 -c "import json,sys
try: d=json.load(sys.stdin)
except Exception: print('no'); raise SystemExit
m=d.get('mac','')
print('yes' if len(m.replace(':',''))==12 else 'no')" 2>/dev/null)"
fi

# 2b) 发射功率归上位机（2026-09-10 T1.5）。以前 /status 一上电就报
#     zigbee_tx_power=20，而 NCP 实际跑的是 ZHA 写进去的 8 dBm（zigbee.db
#     network_backups_v15 里能查到）。那个值只在 NVS 里活过，从未下发。
#     断言的是"网关不再对功率发表意见"，而不是某个新值。
reg "/status 不再报 zigbee_tx_power 数值"  "null" "$(api /status | jq_ zigbee_tx_power)"
reg "/status 标注功率归 host"              "host" "$(api /status | jq_ zigbee_tx_power_source)"
reg "/zigbee/status 不再报 turbo_mode"      "null" "$(api /zigbee/status | jq_ turbo_mode)"
reg "/zigbee/status 不再报 configured_tx_power" "null" "$(api /zigbee/status | jq_ configured_tx_power)"
# ⚠️ 不是 404：esp_httpd 对"路径匹配了别的 method 的 handler"回 405，而 GET 有
#    SPA 静态兜底 /*，所以任何不存在的 POST 路径在本服务器上都是 405。
eq  "/zigbee/turbo 端点已移除 (405)"        "405"  "$(code POST /zigbee/turbo '{"enabled":true}')"
# 旧备份里带 tx_power/turbo_mode 键：导入必须按未知键忽略，不能 400。
# 只发一个最小文档（各段都是可选的），别把整份备份中途灌回去打乱后面的用例。
CFG_VER=$(python3 -c "import json; print(json.load(open('$BACKUP'))['version'])")
eq  "导入带旧 tx_power 键的文档不报错"      "200"  "$(code POST /config/import "{\"version\":$CFG_VER,\"zigbee\":{\"tx_power\":20,\"turbo_mode\":true}}")"

# 2c) /ethernet/config（T2.8）。此前**零断言** —— 它是 D1 清单上最后两条之一。
#     ⚠️ 这个端点只有 POST 没有 GET，读回只能走 /config/export 的 ethernet 段。
#     ⚠️ 写进去的静态值**故意用板子当前真实的 IP/网关/掩码**：万一脚本中途被打断、
#        配置没还原就赶上一次重启，板子会按这份静态配置起来 —— 用真值它照样在线，
#        用 10.9.9.9 就把唯一的访问路径写没了。生效点是下次重启，本次不受影响。
ETH_NOW=$(api /ethernet/status)
E_IP=$(echo "$ETH_NOW"  | jq_ ip)
E_GW=$(echo "$ETH_NOW"  | jq_ gateway)
E_NM=$(echo "$ETH_NOW"  | jq_ netmask)
if [ -z "$E_IP" ] || [ "$E_IP" == "0.0.0.0" ] || [ "$E_IP" == "None" ]; then
    echo "  ⏭️  以太网没拿到 IP，静态配置往返无从验证"; SKIP=$((SKIP+1))
else
    # 点分十进制 → 小端 u32（导出里存的是数字）
    ip2u32() { python3 -c "
import sys
a=sys.argv[1].split('.')
print(int(a[0])|int(a[1])<<8|int(a[2])<<16|int(a[3])<<24)" "$1"; }
    # 输出格式必须与 jq_ 一致：布尔要 true/false 而不是 Python 的 True/False，
    # 否则断言全对不上（第一版就是这么红的）。
    ETH_EXPORT() { C "http://$GW/api/v1/config/export" | python3 -c "import json,sys
d=json.load(sys.stdin).get('ethernet') or {}
v=d.get('$1')
print(v if isinstance(v,str) else json.dumps(v))"; }

    R=$(apip /ethernet/config "{\"dhcp\":false,\"static_ip\":\"$E_IP\",\"static_gateway\":\"$E_GW\",\"static_netmask\":\"$E_NM\",\"static_dns\":\"$E_GW\"}")
    eq "以太网静态配置被接受"        "ok"     "$(echo "$R" | jq_ status)"
    # 必须明说要重启才生效 —— 唯一生效点是 network_mgr_init()，改完不重启什么都没发生
    reg "回包说明 takes_effect=reboot" "reboot" "$(echo "$R" | jq_ takes_effect)"
    eq "dhcp 落盘为 false"           "false"  "$(ETH_EXPORT dhcp)"
    eq "static_ip 落盘"       "$(ip2u32 "$E_IP")" "$(ETH_EXPORT static_ip)"
    eq "static_gateway 落盘"  "$(ip2u32 "$E_GW")" "$(ETH_EXPORT static_gateway)"
    eq "static_netmask 落盘"  "$(ip2u32 "$E_NM")" "$(ETH_EXPORT static_netmask)"
    # static_dns 单列：它**曾经根本没被解析过**，静态 IP 配好了却没有 DNS，域名全不通
    reg "static_dns 落盘（曾经从不解析）" "$(ip2u32 "$E_GW")" "$(ETH_EXPORT static_dns)"

    # 错误路径：关了 DHCP 又不给 IP，必须被拒而不是存进去
    eq "关 DHCP 不给 IP 被拒(400)" "400" "$(code POST /ethernet/config '{"dhcp":false,"static_ip":"0.0.0.0"}')"
    eq "空 body 被拒(400)"         "400" "$(code POST /ethernet/config '')"

    # 还原成 DHCP（收尾的 config/import 也会还原，这里提前做一次，缩短危险窗口）
    apip /ethernet/config '{"dhcp":true}' > /dev/null
    eq "还原回 DHCP"                 "true"   "$(ETH_EXPORT dhcp)"
fi

# 3) SoftAP 开关：只读 + 校验，**不真的开** —— 开了 Thread 会全掉线
if [ "$WIFI_ABSENT" = 1 ]; then
    echo "  ⏭  跳过 SoftAP 与 Wi-Fi 静态 IP 两段：纯以太网剖面没有这些端点（上面 E03/E04 已断言 404）"; SKIP=$((SKIP+1))
else
SOFTAP_NOW=$(api /network/softap | jq_ active)
eq "SoftAP 状态可读"        "$SOFTAP_NOW" "$(api /network/softap | jq_ active)"
eq "SoftAP 非布尔被拒"      "400" "$(code POST /network/softap '{"enabled":"yes"}')"
eq "SoftAP 缺字段被拒"      "400" "$(code POST /network/softap '{}')"
# 设成当前值应当是 no-op，且回包要说明没变
eq "设成当前值不产生变更"   "false" "$(apip /network/softap "{\"enabled\":$SOFTAP_NOW}" | jq_ changed)"

# 4) WiFi 静态 IP：写进去要能从 config/export 读回来（含 DNS —— 老 setter 从不写它）
# ⚠️ 顺序有讲究：配置端点一律**部分更新**（字段缺省=保持原值），WiFi 侧又没有
#    独立的 dhcp 字段，static_ip==0 就是"走 DHCP"。所以"只给网关不给 IP 要被拒"
#    这条只在**已清空**的状态下才成立 —— 已经有 IP 时只改网关是合法的部分更新。
#    先清空再验，否则测的是自己刚写进去的那个 IP。
apip /wifi/static '{"dhcp":true}' > /dev/null
eq  "DHCP 态下缺 IP 被拒"    "400" "$(code POST /wifi/static '{"static_gateway":"192.168.1.1"}')"
apip /wifi/static '{"static_ip":"192.168.222.10","static_gateway":"192.168.222.1","static_netmask":"255.255.255.0","static_dns":"192.168.222.53"}' > /dev/null
WS=$(api /config/export | python3 -c "import json,sys,ipaddress
d=json.load(sys.stdin)['wifi']
f=lambda v: str(ipaddress.IPv4Address(int(v).to_bytes(4,'little')))
print(','.join(f(d[k]) for k in ('static_ip','static_gateway','static_netmask','static_dns')))")
reg "静态 IP 四个字段都落盘" "192.168.222.10,192.168.222.1,255.255.255.0,192.168.222.53" "$WS"
# 有 IP 时只改网关 = 合法的部分更新，必须放行（和以太网侧同语义）
eq  "有 IP 时可单改网关"     "200" "$(code POST /wifi/static '{"static_gateway":"192.168.222.254"}')"
# dhcp:true 清空
apip /wifi/static '{"dhcp":true}' > /dev/null
eq "dhcp:true 清空静态 IP"   "0" "$(api /config/export | jq_ wifi.static_ip)"
fi   # WIFI_ABSENT

echo ""; echo "── 桥接模式落盘（2026-08-16 修）──"
# 曾经 handle_mode 只切运行态不写配置，config_mgr_set_bridge_mode() 零调用者：
# 用户切到 USB/DISABLED，任何一次重启（含看门狗自愈）都悄悄回 LAN。
#
# 这里**不改运行态**——本脚本的收尾断言「桥接未受影响」不允许踢掉 Z2M/ZHA。
# 做法是先把**存储值**投毒成一个和运行态不同的数（config/import 只写配置、
# 不调 switch_mode，已核实），再 POST 当前运行的那个模式：switch_mode 见
# mode == s_mode 会立刻返回、一个字节都不动，于是这条断言测的纯粹是"落盘"。
BM_RUN=$(api /status | jq_ bridge_mode)
case "$BM_RUN" in lan) BM_NUM=0;; usb) BM_NUM=1;; disabled) BM_NUM=2;; *) BM_NUM=-1;; esac
if [ "$NO_BRIDGE" = 1 ]; then
    echo "  ⏭️  直连型没有桥接模式（/mode 不注册，上面 B01、B07 已断言）"; SKIP=$((SKIP+4))
elif [ "$BM_NUM" -lt 0 ]; then
    echo "  ⏭️  /status 的 bridge_mode 读不出（得到 '$BM_RUN'），跳过"; SKIP=$((SKIP+4))
else
    POISON=$([ "$BM_NUM" -eq 2 ] && echo 1 || echo 2)
    PT=$(mktemp)
    api /config/export > "$PT"
    python3 -c "
import json
d=json.load(open('$PT')); d['system']['bridge_mode']=$POISON
json.dump(d, open('$PT','w'))"
    C -X POST "http://$GW/api/v1/config/import" --data-binary @"$PT" > /dev/null
    rm -f "$PT"
    eq "投毒：存储值变成 $POISON"  "$POISON" "$(api /config/export | jq_ system.bridge_mode)"
    eq "import 不动运行态"        "$BM_RUN"  "$(api /status | jq_ bridge_mode)"
    # ★ 真正的断言。改之前这里会停在 $POISON——因为根本没人写配置。
    apip /mode "{\"mode\":$BM_NUM}" > /dev/null
    reg "切模式落盘"              "$BM_NUM"  "$(api /config/export | jq_ system.bridge_mode)"
    eq  "落盘不影响运行态"        "$BM_RUN"  "$(api /status | jq_ bridge_mode)"
fi
# ⚠️ 本段证明的是"值写进了配置层"。写进 NVS 有 2s 防抖，而"重启后真的还是它"
#    只能靠重启验——见 docs/test_cases_stability.md，那条目前还得人工跑。

echo ""; echo "── 内存与容量水位（2026-08-17 新增）──"
# 这一节此前完全不存在：内存指标从来没进过回归，而 alloc_failures 已经真的
# 出现过 1 次（2026-08-17 查内部 SRAM 时发现，当时 min_free 只剩 12 字节）
# —— 没人会发现，因为没有任何断言看它。
#
# ⚠️ 判据分两类，别混：
#   硬停 —— alloc_failures 必须为 0。非 0 说明已经有分配失败了，
#           而失败的后果（任务建不起来、WiFi 缓冲拿不到）是随机且难归因的。
#   水位 —— min_free / largest / 栈余量 / OT 缓冲，只设一个很低的地板做回归防线，
#           不锁具体数值：它们随运行时长与负载浮动，锁死了只会变成噪声。
H=$(api /diag/heap)
AF=$(echo "$H" | python3 -c "import json,sys;print(json.load(sys.stdin)['alloc_failures']['count'])" 2>/dev/null)
reg "内部内存零分配失败" "0" "$AF"

MF=$(echo "$H" | python3 -c "import json,sys;print(json.load(sys.stdin)['internal']['min_free'])" 2>/dev/null)
LG=$(echo "$H" | python3 -c "import json,sys;print(json.load(sys.stdin)['internal']['largest'])" 2>/dev/null)
echo "  ℹ️  internal min_free=$MF largest=$LG（2026-08-17 基线：约 13500 / 13312）"
# 地板取 4096：低于它说明出了系统性回退，而不是负载波动
if [ "${MF:-0}" -ge 4096 ] 2>/dev/null; then ok "内部内存低水位 >= 4096"; else bad "内部内存低水位 >= 4096" ">=4096" "$MF"; fi
# 连续块要装得下一个 3072 任务栈 —— 装不下时"切桥接模式回不去"会复现
if [ "${LG:-0}" -ge 3072 ] 2>/dev/null; then ok "最大连续块 >= 3072（够建一个任务栈）"; else bad "最大连续块 >= 3072" ">=3072" "$LG"; fi

# ⚠️ 第 6 条硬停判据（2026-09-15 新增）：DMA 池低水位。
# 上面两条看的 internal 是 MALLOC_CAP_INTERNAL|MALLOC_CAP_DEFAULT —— **不含**
# SPIRAM_MALLOC_RESERVE_INTERNAL 划走的那块，会漏看真正见底的池子。
# 实锤：09-15 那次 A/B 里同一个改动让 internal.min_free 只动 +132，而
# dma.min_free 从 2119 动到 5319；改前三条 internal 判据全绿，而 dma 已经很紧。
# 地板取 3072，与上面"够建一个任务栈"同一条理由。经验锚点：08-17 实测
# dma.min_free 一度只有 1783，当天就真的发生了一次 4096 字节的分配失败。
DM=$(echo "$H" | python3 -c "import json,sys;print(json.load(sys.stdin)['dma']['min_free'])" 2>/dev/null)
DL=$(echo "$H" | python3 -c "import json,sys;print(json.load(sys.stdin)['dma']['largest'])" 2>/dev/null)
# ⚠️ 判据读的是**脚本开跑前**那个空闲值 DMA_IDLE，不是跑到这里的 DM ——
#    两者之间隔着本脚本自己踢掉 ZHA 的那一步（见文件开头 DMA_IDLE 处的注释）。
#    DM 仍然打出来做参考：它比 DMA_IDLE 低多少 = 这套回归自身的瞬时开销。
echo "  ℹ️  dma min_free 空闲 $DMA_IDLE → 跑完 $DM，largest=$DL（空闲基线与读法见 docs/test_cases_scale_baseline.md 第 6 条）"
if [ "${DMA_IDLE:-0}" -ge 3072 ] 2>/dev/null; then ok "DMA 池低水位（空闲）>= 3072  [硬停判据 6]"; else bad "DMA 池低水位（空闲）>= 3072  [硬停判据 6]" ">=3072" "$DMA_IDLE"; fi

# 逐任务栈余量：2026-08-17 曾按静态估算把 ws_push 裁到只剩 16 字节
SL=$(api /diag/watchdog | python3 -c "
import json,sys
d=json.load(sys.stdin); t=[x for x in d['tasks'] if x.get('stack_left')]
if not t: print('none'); raise SystemExit
w=min(t, key=lambda x:x['stack_left']); print('%s %d'%(w['name'],w['stack_left']))" 2>/dev/null)
if [ "$SL" == "none" ] || [ -z "$SL" ]; then
    echo "  ⏭️  栈余量还没采到（任务需先各 kick 一次）"; SKIP=$((SKIP+1))
else
    SLN=${SL##* }; echo "  ℹ️  最紧的栈：$SL 字节"
    if [ "$SLN" -ge 512 ] 2>/dev/null; then ok "所有任务栈余量 >= 512  [防回归]"; else bad "所有任务栈余量 >= 512  [防回归·曾裁到只剩 16]" ">=512" "$SL"; fi
fi

# 系统总任务数（2026-10-06 加进 /diag/watchdog）：PSRAM 栈的常驻任务不在登记表里，
# 任务泄漏只能靠它看 —— 受控的前后对比放在 L3 子系统重启上做（TC-W07 httpd；Thread 自 10-07 起没有 L3），这里只验字段真的在出数
TCN=$(api /diag/watchdog | python3 -c "import json,sys; print(json.load(sys.stdin).get('task_count'))" 2>/dev/null)
echo "  ℹ️  系统任务总数 $TCN"
if [ "$TCN" -ge 10 ] 2>/dev/null && [ "$TCN" -le 80 ]; then ok "diag/watchdog 报出系统任务总数（10..80）"; else bad "diag/watchdog 报出系统任务总数（10..80）" "10..80" "$TCN"; fi

# OT 消息缓冲水位：扩容时最该盯的一个数，现在开始记基线
BU=$(api /diag/thread | python3 -c "
import json,sys
b=json.load(sys.stdin).get('buffers') or {}
print('%s/%s'%(b.get('max_used'),b.get('total')))" 2>/dev/null)
echo "  ℹ️  OT 消息缓冲峰值 $BU（2026-08-17 基线：2 台设备 23/1024）"

# MG24 表容量：Zigbee 扩容的约束全在这儿，C5 侧看不到
# ⚠️ 布尔要走 json.dumps，直接 print 会得到 Python 的 True/False —— 本脚本
#    开头 jq_ 的注释就警告过这件事，这里第一版又踩了一次。
# 2026-10-06 起钉的是扩容版的全部值（mg24/README.md「NCP 表容量」）：板上要是换回了小表版
# （源路由 7、地址 12…），Zigbee 照样能用、只是大网络会广播风暴 —— 外部零症状，只有这条看得见。
# ⚠️ 例外：芯片跑过直连子设备 64 之后再换回 NVM3 缓存 200 的老镜像，不是零症状而是开机即崩 ——
# 那时这里读到的是 known=false（NCP 不应答），见下面那行提示。顺序：已读到 子设备 邻居 地址 源路由 密钥 路由 APS单播 包缓冲堆
NT=$(api /zigbee/status | python3 -c "
import json,sys
t=json.load(sys.stdin).get('ncp_tables') or {}
k=('max_end_device_children','neighbor_table','address_table','source_route_table','key_table','route_table','aps_unicast_msgs','packet_buffer_heap')
print(' '.join([json.dumps(t.get('known'))]+[str(t.get(x)) for x in k]))" 2>/dev/null)
# 10-06 晚直连子设备 32 → 64（NVM3 缓存同时 200 → 400，mg24/README.md「直连子设备 64：根因实测」）。
# ⚠️ 用 Z2M 时它会被握手写回 stack_config.json 的值（默认 32），但这里读的是网关开机时的值，不受上位机影响
eq "NCP 表容量 = 扩容版（64 26 128 254 20 254 128 16384）" "true 64 26 128 254 20 254 128 16384" "$NT"
[ "$NT" = "true 32 16 12 7 20 16 10 8192" ] && echo "  ⚠️  板上是出厂那版小表 NCP（有人刷了老镜像？）—— 见 mg24/README.md「NCP 表容量」"
[ "$NT" = "true 32 26 128 254 20 254 128 16384" ] && echo "  ⚠️  板上是直连 32 的扩容版（10-06 白天那版或回退镜像）—— 要是 NVM3 缓存 200 的那版，跑过 64 的芯片刷它会开机即崩，见 mg24/README.md「直连子设备 64：根因实测」"
case "$NT" in false*) echo "  ⚠️  读不到 NCP 表容量：NCP 没应答。刚换过 NCP 的话先看 /api/v1/diag/ncp_line 的 cts_transitions —— 非 0 且一直跳 = 崩溃循环（例如跑过直连 64 的芯片被刷了缓存 200 的老镜像）" ;; esac

# ────────────────────── mDNS 本机地址（T9.34，2026-10-07）──────────────────
# 插着网线时 bitengine.local 的 A 记录只能是以太网服务 IP（HA / ZHA / Z2M 添加那一刻记下的就是它）。
# 不能用 avahi / python-zeroconf 判：看不到同一包里几个 A、有没有 0.0.0.0（工具头注有说明）。
echo ""; echo "── mDNS 本机地址（T9.34）──"
MDNS_TOOL="$(cd "$(dirname "$0")" && pwd)/mdns_self_addr.py"
ST=$(api /status)
ETH_IP=$(echo "$ST" | jq_ eth_ip); WIFI_IP=$(echo "$ST" | jq_ wifi_ip); HN=$(echo "$ST" | jq_ hostname)
SVC_IP=$(echo "$ST" | jq_ svc_ip); SVC_HOLDER=$(echo "$ST" | jq_ svc_holder)
# T9.37 服务身份生效时（svc_holder 不是 off），服务地址是 /status 的 svc_ip（可能是 WiFi 那份身份的地址，此刻挂在网口上）；
# 关着或退回旧行为时仍是以太网地址
if [ -n "$SVC_HOLDER" ] && [ "$SVC_HOLDER" != "off" ]; then WANT_A="$SVC_IP"; else WANT_A="$ETH_IP"; fi
if [ "$(echo "$ST" | jq_ eth_link_up)" = "true" ] && [ -n "$ETH_IP" ] && [ "$ETH_IP" != "0.0.0.0" ]; then
    MD=$(python3 "$MDNS_TOOL" --query --name "$HN.local" --from "$ETH_IP,$WIFI_IP" 2>&1)
    reg "插着网线时本机 A 记录只有服务地址（T9.34 / T9.37）" "$WANT_A" \
        "$(echo "$MD" | grep -oE "'A [0-9.]+" | awk '{print $2}' | sort -u | tr '\n' ' ' | sed 's/ $//')"
    eq "主机名没被改成 -2" "（这段时间没有任何带 $HN-2.local A/AAAA 的包）" \
        "$(python3 "$MDNS_TOOL" --query --name "$HN-2.local" 2>&1 | tail -1)"
else
    echo "  ⏭  以太网不在线，跳过（这条只在插着网线时有意义）"; SKIP=$((SKIP+1))
fi

# ────────────────────── 服务身份（T9.37，2026-10-07）──────────────────
# 服务地址（HA / ZHA / Z2M 记的那个）由某个 DHCP 身份持有、此刻挂在持有者那张卡上。设计 docs/16，行为正本 Kconfig
# BITENGINE_SVC_IDENTITY 的 help。拔插网线的行为在 docs/test_cases_stability.md 测试 6e，这里只验静态形状与"地址确实在持有者身上"。
echo ""; echo "── 服务身份（T9.37）──"
ST=$(api /status)
SID=$(echo "$ST" | jq_ svc_identity); SH=$(echo "$ST" | jq_ svc_holder); SM=$(echo "$ST" | jq_ svc_id_mode); SIP=$(echo "$ST" | jq_ svc_ip)
case "$SID" in eth|wifi|undecided|off) ok "svc_identity 取值合法（$SID）" ;; *) bad "svc_identity 取值合法" "eth|wifi|undecided|off" "$SID" ;; esac
case "$SH" in eth|wifi|wifi_pinned|pending|off) ok "svc_holder 取值合法（$SH）" ;; *) bad "svc_holder 取值合法" "eth|wifi|wifi_pinned|pending|off" "$SH" ;; esac
case "$SM" in client_id|mac_only|unknown|off) ok "svc_id_mode 取值合法（$SM）" ;; *) bad "svc_id_mode 取值合法" "client_id|mac_only|unknown|off" "$SM" ;; esac
case "$SH" in
    eth)  eq "服务地址挂在持有者（网口）上" "$SIP" "$(echo "$ST" | jq_ eth_ip)" ;;
    wifi|wifi_pinned) eq "服务地址挂在持有者（WiFi）上" "$SIP" "$(echo "$ST" | jq_ wifi_ip)" ;;
    *)    echo "  ⏭  身份机制没在管 / 开机待定（svc_holder=$SH），跳过地址归属"; SKIP=$((SKIP+1)) ;;
esac

# ────────────────────── TC-X 跨站守卫（2026-08-23）──────────────────
# ⚠️ 判据必须**真的发一个跨站形态的请求**。components/webui/VERIFY.sh 能证明宏
#    在代码里，证明不了它被 httpd 真的执行到 —— 这正是 2026-08-13 那次 NTP 的
#    教训（事件发了没人接，而 VERIFY 一直绿）。
#
# ⚠️ 下面几条必须**显式给 Host**，不能指望 $GW 恰好是个名字：本仓库到处示范传 IP，
#    而传 IP 时 host_is_ip_literal() 直接短路，host_name_ok() 整套一次都不跑，
#    于是"白名单有没有生效"根本没被测到。
echo ""; echo "── TC-X 跨站守卫 ──"

# 用 IP 打，好让 Host 检查天然放行，从而单独测 Origin 那一层
IP=$(api /status | jq_ wifi_ip); [ -z "$IP" ] || [ "$IP" == "None" ] && IP="$GW"
xcode() {   # $1=方法 $2=路径 $3..=额外 curl 参数
    local m="$1" path="$2"; shift 2
    C -o /dev/null -w '%{http_code}' -X "$m" "http://$GW$path" "$@"
}

# X01 经典 CSRF：第三方页面发起 —— Host 是我们的，Origin 是别人的
eq "X01 跨站 Origin 被拒(403)" "403" \
   "$(xcode POST /api/v1/led/config -H 'Origin: http://evil.com' -d '{}')"

# X01b 超长 Origin。长度由攻击者自选，这条专打 TRUNC 绕过：
#      httpd_req_get_hdr_value_str 超长时返回 TRUNC 而非 NOT_FOUND，
#      写成「!= ESP_OK 就放行」的话这条会变 200 —— 而 X01 照样绿。
eq "X01b 超长 Origin 被拒(403)" "403" \
   "$(xcode POST /api/v1/led/config -H "Origin: http://$(python3 -c 'print("a"*400)').com" -d '{}')"

# X02 DNS rebinding：Origin 与 Host **一致**，都是攻击者的域名。
#     这正是「只做 Origin 一致性检查」会放行的形态 —— 把两层的必要性变成判据。
eq "X02 rebinding 被拒(421)" "421" \
   "$(xcode POST /api/v1/led/config -H 'Host: evil.com' -H 'Origin: http://evil.com' -d '{}')"

# X02b 超长 Host —— 同 X01b，打的是 Host 那一层的 TRUNC
eq "X02b 超长 Host 被拒(421)" "421" \
   "$(xcode POST /api/v1/led/config -H "Host: $(python3 -c 'print("a"*400)').com" -d '{}')"

# X02c 前缀匹配的绕过。把边界检查放宽成 strncasecmp 不查边界 ⇒ 这条变 200 ⇒ 防御归零
eq "X02c 主机名前缀绕过被拒(421)" "421" \
   "$(xcode POST /api/v1/led/config -H "Host: $(api /status | jq_ hostname).attacker.com" -d '{}')"

# X03 不带 Origin 的跨站 GET —— <img src> 的形态。Origin 那层对它天然失明，
#     只有 Sec-Fetch-Site 挡得住。打的是会**真的独占射频**的那条。
eq "X03 跨站 img 形态 GET 被拒(403)" "403" \
   "$(xcode GET /api/v1/spectrum/scan -H 'Sec-Fetch-Site: cross-site')"

# X04 rebinding 读不到密钥材料 —— 最高价值目标。
#     守卫在 AUTH 之前 ⇒ 期望值与 thread.rest_auth 开关无关，是个恒定值。
eq "X04 rebinding 读不到 NetworkKey(421)" "421" \
   "$(C -o /dev/null -w '%{http_code}' -H 'Host: evil.com' "http://$GW/node/dataset/active")"

# ★★ X05 合法客户端没被误伤 —— 比上面七条加起来更重要。
#    一旦红了，说明产线脚本和 HA 集成全线死掉，而现场症状会伪装成"设备突然全不通"。
eq "X05a curl 无 Origin 照常(200)" "200" "$(xcode GET /api/v1/status)"
eq "X05b 前端同源 POST 照常(200)" "200" \
   "$(xcode POST /api/v1/led/config -H "Origin: http://$GW" -H 'Sec-Fetch-Site: same-origin' -d '{}')"
# ⚠️ 下面这一串**必须打一个真的挂了守卫的端点**。第一版打的是 /api/v1/status，
#    而那个端点是**故意不挂守卫**的（横幅要显示，页面就得先能加载 —— 见
#    webui.h 的 AUTH_REQUIRED_UNLESS_OPEN 和设计文档「挡不住什么」第 2 条）。
#    结果 8 条断言全部恒真：Host 填什么都是 200，守卫拆掉也一样绿。
#    /api/v1/config/export 是挂了守卫的 GET，且允许时回 200、被拒时回 421。
GUARDED=/api/v1/config/export
hcode() { C -o /dev/null -w '%{http_code}' -H "Host: $1" "http://$GW$GUARDED"; }

eq "X05c 用 IP 直连照常(200)"     "200" "$(hcode "$IP")"
eq "X05d mDNS 名照常(200)"        "200" "$(hcode "$(api /status | jq_ hostname).local")"
# .lan 是我们自己的固件让路由器建的（wifi_mgr.c/eth_mgr.c 的 DHCP option 12），
# 也是用户最常用的入口。漏掉它 = 把用户锁在自己造的门外。
eq "X05e 路由器 .lan 名照常(200)" "200" "$(hcode "$(api /status | jq_ hostname).lan")"
# 尾点 FQDN：Chrome/Firefox 会原样保留 http://bitengine.local./ 里的那个点
eq "X05f 尾点 FQDN 照常(200)"     "200" "$(hcode "$(api /status | jq_ hostname).local.")"
eq "X05g 带端口的 Host 照常(200)" "200" "$(hcode "$(api /status | jq_ hostname).local:80")"
# 反例：证明上面几条不是恒真的
eq "X05g2 陌生 Host 确实被拒(421)" "421" "$(hcode "not-our-name.example")"

# X05h extra_host 的出路真的通 —— 这是 421 唯一的自救路径，断了就是售后灾难
apip /net/access-host '{"extra_host":"gw-test.example"}' >/dev/null
eq "X05h extra_host 生效(200)"     "200" "$(hcode "gw-test.example")"
eq "X05i 未登记的名字仍被拒(421)"  "421" "$(hcode "other.example")"
apip /net/access-host '{"extra_host":""}' >/dev/null
eq "X05j 清空后立即失效(421)"      "421" "$(hcode "gw-test.example")"
eq "X05k extra_host 拒绝非法字符(400)" "400" \
   "$(C -o /dev/null -w '%{http_code}' -X POST "http://$GW/api/v1/net/access-host" -d '{"extra_host":"bad host:80"}')"

# X06 决策 1.5 没被破坏 —— 守卫绝不能污染 404 兜底，否则 python-otbr-api
#     抛 OTBRError → ConfigEntryNotReady → HA 的 otbr 集成加载不起来
eq "X06 /api/actions 仍是 404" "404" \
   "$(C -o /dev/null -w '%{http_code}' "http://$GW/api/actions")"
eq "X06b 跨站下 /api/actions 也是 404" "404" \
   "$(C -o /dev/null -w '%{http_code}' -H 'Origin: http://evil.com' "http://$GW/api/actions")"

# X07 421 必须带出路，否则用户被锁在门外且无从自救。
#     同样必须打挂了守卫的端点 —— 打 /status 的话根本不会 421，恒假。
eq "X07 421 回包带 use 字段" "true" \
   "$(C -H 'Host: evil.com' "http://$GW$GUARDED" | python3 -c 'import json,sys
try: print("true" if json.load(sys.stdin).get("use","").startswith("http://") else "false")
except Exception: print("false")')"

# ────────────────── TC-T OTBR 专用 token（2026-08-23）──────────────────
# 这一组的价值集中在 T03：**作用域不能泄漏**。token 存在的全部理由就是
# "泄了也只是 Thread 数据集，不是整机"，一旦它能打 /api/v1 下的端点，
# 这个理由就不成立，整个特性变成"换了个名字的管理密码"。
echo ""; echo "── TC-T OTBR 访问 token ──"

T_GEN=$(apip /otbr/token '{"action":"generate"}')
TOK=$(echo "$T_GEN" | jq_ token)
eq "T01 生成返回 32 位 token" "32" "${#TOK}"
eq "T01b 回包直接给出可粘贴 URL" "true" \
   "$(echo "$T_GEN" | python3 -c 'import json,sys
try: print("true" if json.load(sys.stdin).get("url","").startswith("http://otbr:") else "false")
except Exception: print("false")')"

# T01c（T9.22）可粘贴 URL 里的地址是设备地址（svc_ip），不是这次访问用的地址
T_SVC=$(api /status | jq_ svc_ip)
if [ -n "$T_SVC" ] && [ "$T_SVC" != "0.0.0.0" ]; then
    eq "T01c 可粘贴 URL 用设备地址" "http://otbr:$TOK@$T_SVC:8081" "$(echo "$T_GEN" | jq_ url)"
else
    echo "  ⏭  T01c 还没有设备地址（老固件 / 开机早期），跳过"; SKIP=$((SKIP+1))
fi

# T02 token 在 OTBR 路由上有效
eq "T02 token 能过 OTBR 路由" "200" \
   "$(curl -s -m10 -o /dev/null -w '%{http_code}' -u "otbr:$TOK" "http://$GW/node")"
eq "T02b :8081 上同样有效" "200" \
   "$(curl -s -m10 -o /dev/null -w '%{http_code}' -u "otbr:$TOK" "http://$GW:8081/node")"

# ★★ T03 作用域不能泄漏 —— 这一组里唯一不能红的
eq "T03 token 打 /api/v1/status 被拒" "401" \
   "$(curl -s -m10 -o /dev/null -w '%{http_code}' -u "otbr:$TOK" "http://$GW/api/v1/ntp")"
eq "T03b token 打写端点被拒" "401" \
   "$(curl -s -m10 -o /dev/null -w '%{http_code}' -X POST -u "otbr:$TOK" \
      "http://$GW/api/v1/led/config" -d '{}')"
eq "T03c token 打 /api/v1/otbr/token 自己也被拒" "401" \
   "$(curl -s -m10 -o /dev/null -w '%{http_code}' -u "otbr:$TOK" "http://$GW/api/v1/otbr/token")"

# T04 设了 token 之后，OTBR 路由上不再认管理密码（有意为之，见 otbr_api.c）
eq "T04 管理密码在 OTBR 路由上失效" "401" \
   "$(curl -s -m10 -o /dev/null -w '%{http_code}' -u ":$GW_PASS" "http://$GW/node")"
# 但 WebUI 自己（会话 cookie）必须照常 —— 否则 Thread 页面直接白屏
JAR=$(mktemp)
curl -s -m10 -c "$JAR" -X POST "http://$GW/api/v1/auth/login" \
     -H 'Content-Type: application/json' -d "{\"password\":\"$GW_PASS\"}" >/dev/null
eq "T04b 会话 cookie 仍能读 OTBR 路由" "200" \
   "$(curl -s -m10 -b "$JAR" -o /dev/null -w '%{http_code}' "http://$GW/node")"
rm -f "$JAR"

# ★ T05 错 token 不能顶登录节流的计数器。
#   这条防的是一个由**第三方轮询频率**决定的拒绝服务：HA 拿着过期 token 反复
#   轮询，若每次失败都走管理密码比对，5 次就把管理员锁在门外，而且会一直锁着。
for i in 1 2 3 4 5 6 7 8; do
    curl -s -m10 -o /dev/null -u "otbr:00000000000000000000000000000000" "http://$GW/node"
done
eq "T05 连错 8 次 token 后管理员仍能登录" "200" \
   "$(curl -s -m10 -o /dev/null -w '%{http_code}' -X POST "http://$GW/api/v1/auth/login" \
      -H 'Content-Type: application/json' -d "{\"password\":\"$GW_PASS\"}")"

# T06 格式校验：token 由设备生成，接口不接受用户自选
eq "T06 未知 action 被拒" "400" \
   "$(C -o /dev/null -w '%{http_code}' -X POST "http://$GW/api/v1/otbr/token" -d '{"action":"set","token":"abc"}')"

# T07 轮换：换发之后旧 token 必须立刻失效。
#   ⚠️ 这条不能写成"清除之后旧 token 还能不能用"——清除之后路由本来就开放，
#      拿旧 token 打当然是 200，那样的断言**恒真**，证明不了任何事。
#      要证明失效，必须在闸门仍然开着的前提下比。
TOK2=$(apip /otbr/token '{"action":"generate"}' | jq_ token)
eq "T07 换发得到不同的 token" "true" "$([ "$TOK2" != "$TOK" ] && echo true || echo false)"
eq "T07b 旧 token 立刻失效" "401" \
   "$(curl -s -m10 -o /dev/null -w '%{http_code}' -u "otbr:$TOK" "http://$GW/node")"
eq "T07c 新 token 有效" "200" \
   "$(curl -s -m10 -o /dev/null -w '%{http_code}' -u "otbr:$TOK2" "http://$GW/node")"

# T08 清除后回到默认的开放状态（否则等于偷偷把出厂默认改了）
apip /otbr/token '{"action":"clear"}' >/dev/null
eq "T08 清除后 enabled=false" "false" "$(api /otbr/token | jq_ enabled)"
eq "T08b 清除后 OTBR 路由重新开放（无凭据）" "200" \
   "$(curl -s -m10 -o /dev/null -w '%{http_code}' "http://$GW/node")"

# X08 WebSocket 的跨站守卫。单独用 python 裸 socket，因为 curl 做不了 WS 握手。
#     ⚠️ 判据不能只看 101：握手由 httpd 内部完成，守卫在 post-handshake 回调里
#        才拒绝，所以**拒绝的表现是 101 之后连接被关**，不是握手失败。
#        只断言"没拿到 101"的话，这条判据恒假（永远红），换个写法又会恒真。
#     ⚠️ WS 是这套防护里最需要守卫的一处：跨站脚本可以直接 new WebSocket()，
#        浏览器不做 CORS 预检，SameSite 只管 cookie 不管连接本身。
WSRES=$(python3 - "$GW" "${GW_PASS:-}" <<'PY'
import socket, base64, os, sys
gw, pw = sys.argv[1], sys.argv[2]
def probe(host_hdr, origin):
    try:
        s = socket.create_connection((gw, 80), timeout=6)
    except Exception:
        return "unreachable"
    key = base64.b64encode(os.urandom(16)).decode()
    req = (f"GET /ws HTTP/1.1\r\nHost: {host_hdr}\r\nUpgrade: websocket\r\n"
           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
           f"Sec-WebSocket-Version: 13\r\n")
    if pw: req += "Authorization: Basic " + base64.b64encode(f":{pw}".encode()).decode() + "\r\n"
    if origin: req += f"Origin: {origin}\r\n"
    s.sendall((req + "\r\n").encode())
    hdr = b""
    try:
        while b"\r\n\r\n" not in hdr:
            b = s.recv(4096)
            if not b: break
            hdr += b
    except Exception: pass
    if b"101" not in hdr.split(b"\r\n")[0]:
        s.close(); return "no101"
    s.settimeout(4)
    try:
        alive = len(s.recv(4096)) > 0        # 收到推送帧 = 真被登记了
    except socket.timeout:
        alive = True                          # 无数据但连接没断
    except Exception:
        alive = False
    s.close()
    return "open" if alive else "closed"
print(probe(gw, f"http://{gw}"), probe(gw, "http://evil.com"), probe("evil.com", "http://evil.com"))
PY
)
eq "X08a 同源 WS 存活"        "open"   "$(echo $WSRES | cut -d' ' -f1)"
eq "X08b 跨站 WS 被关"        "closed" "$(echo $WSRES | cut -d' ' -f2)"
eq "X08c rebinding WS 被关"   "closed" "$(echo $WSRES | cut -d' ' -f3)"

# ⚠️ TC-S 放在**最后**：它连着传两个 2MB 镜像，跑完 internal_free 会被压到
#    1 万字节以下。2026-08-23 它原本排在 X08 之前，紧随其后的 WebSocket
#    握手偶发失败 —— 断言红了而 WS 本身没毛病（单独重测三种形态全对）。
#    这类「被前一条用例的资源压力连坐」的假红最费时间，因为它完全像真 bug。
# ── TC-S 签名验证的负路径（默认跳过，加 --negative-signing 才跑）──
#
# ⚠️ **只在手边能接串口时跑。** 万一验签没生效，未签名镜像会被装上并启动，
#    之后设备拒绝一切 OTA，只能串口刷回。这也正是这条测试的意义所在 ——
#    它唯一可能失败的方式，恰好也是唯一需要救砖的方式。
#
# 为什么值得单独一条：签名→签名的 OTA 即使验签**完全没生效**也会成功，
# 所以正向用例证明不了任何事。只有"未签名镜像必须被拒"有决定性。
#
# 单变量：bitengine-unsigned.bin 与 bitengine.bin 前 2162688 字节逐字节相同，
# 只差 4096 字节签名扇区。被拒就只可能是签名的原因。
if [ $NEGSIGN -eq 1 ]; then
    echo ""; echo "── TC-S 签名负路径（破坏性，需能串口救砖）──"
    UNSIGNED="$(cd "$(dirname "$0")/.." && pwd)/build/bitengine-unsigned.bin"
    SIGNED="$(cd "$(dirname "$0")/.." && pwd)/build/bitengine.bin"
    if [ ! -f "$UNSIGNED" ]; then
        echo "  ⏭  跳过：找不到 $UNSIGNED（先 idf.py build）"; SKIP=$((SKIP+1))
    else
        # 前置：两个镜像必须只差签名扇区，否则这条测的就不是签名
        SAME=$(python3 -c "
a=open('$UNSIGNED','rb').read(); b=open('$SIGNED','rb').read()
print('true' if len(b)-len(a)==4096 and a==b[:len(a)] else 'false')" 2>/dev/null)
        eq "S00 两镜像仅差签名扇区（单变量前提）" "true" "$SAME"

        UP0=$(api /status | jq_ uptime)
        # ⚠️ 不能用 C()：那个助手带 -m 10，而 2MB 上传要 20-30 秒，curl 会自己
        #    超时返回 000 —— 断言红了，但红的原因是测试而不是设备。
        SEQ0=$(api '/logs?limit=1&level=E' | jq_ next_seq)
        # SEQ0 打出来：万一它是 null/<非JSON>，since 会被解析成 0 而"意外变绿"，
        # 那种假绿比假红更危险 —— 屏幕上留一行就能事后对账。
        echo "     (SEQ0=$SEQ0)"
        eq "S01 未签名镜像被拒(500)" "500" \
           "$(curl -s -m 120 ${AUTH[@]+"${AUTH[@]}"} -o /dev/null -w '%{http_code}' -X POST \
              -H 'Content-Type: application/octet-stream' \
              --data-binary @"$UNSIGNED" "http://$GW/api/v1/ota/esp32")"
        # ★ 只看 500 不够：ota_fail 那个标签有好几个入口（begin 失败、write 失败、
        #   内存不足都会走到），返回码分不清是谁拒的。必须在日志里看到是**验签**拒的。
        #   ⚠️ 而且必须只看**本次之后**新增的日志（seq > SEQ0）：日志环里可能还留着
        #      上一轮的那条，不卡 seq 的话这条判据在验签坏掉之后照样绿。
        # 2026-08-23 这条曾经红过一次，报的是 "false"（看起来像"验签根本没跑"）。
        # ⚠️ 根因**至今没查清**。当时归因为 limit=100 的 17KB malloc 撞上 OTA 之后
        #    的低内存，但那是错的：CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024，
        #    >1KB 的 malloc 本来就走 PSRAM，而且那个 500 从未被真正观察到。
        #    手工按同样顺序复现过多次，每次都正常（since=N 精确返回新增的 4 条）。
        #    所以下面两处是**加固而不是修复**：limit 降到 20、加 2 秒沉降。
        # ★ 真正有价值的改动是把失败分成三种情况报。原来的写法把
        #   "查询挂了" / "没有新日志" / "有日志但不是验签失败" 全折叠成一个 false，
        #   于是那次红指向了完全错误的方向。**失败信息骗人比失败本身更费时间。**
        sleep 2
        eq "S02 拒绝来自验签本身（不是 OOM/写失败）" "true" \
           "$(api "/logs?limit=20&level=E&since=$SEQ0" | python3 -c 'import json,sys
# 失败时把**能解释这次失败的一切**都打出来。2026-08-23 这条红过一次，当时只
# 输出一个 "false"，于是根因查了很久最后也没查清 —— 而它红的那一刻，屏幕上
# 本可以直接显示"查回了几条、来自哪个环、都是些什么"。
try:
    d=json.load(sys.stdin)
except Exception as ex:
    print("查日志失败:响应不是JSON(%s)" % type(ex).__name__); sys.exit()
if "entries" not in d:
    print("查日志失败:error=%s" % d.get("error")); sys.exit()
e=d["entries"]
if any("signature verification failed" in x["msg"] for x in e):
    print("true"); sys.exit()
gotm=" | ".join(x["tag"]+":"+x["msg"][:40] for x in e[:4]) or "(空)"
print("false[source=%s stored=%s dropped=%s 取回%d条: %s]"
      % (d.get("source"), d.get("stored"), d.get("dropped"), len(e), gotm))')"
        sleep 3
        UP1=$(api /status | jq_ uptime)
        eq "S03 设备没有重启（uptime 连续）" "true" \
           "$([ "${UP1:-0}" -gt "${UP0:-0}" ] && echo true || echo false)"
        # jq_ 把布尔按 JSON 输出，是 true 不是 Python 的 True
        eq "S04 仍在跑签名固件" "true" "$(api /status | jq_ app_signed)"
        # 失败路径必须复位 s_ota_in_progress，否则"测一次就把升级堵死"
        eq "S05 OTA 通道仍可用（不是 409）" "500" \
           "$(curl -s -m 120 ${AUTH[@]+"${AUTH[@]}"} -o /dev/null -w '%{http_code}' -X POST \
              -H 'Content-Type: application/octet-stream' \
              --data-binary @"$UNSIGNED" "http://$GW/api/v1/ota/esp32")"
    fi
fi

# ─────────────────── D1 补：三个此前零行为断言的端点（2026-09-13）───────────────────
echo ""; echo "── /wifi/config /zigbee/channel /zigbee/restart ──"
# /wifi/config 不敢正向改（会把板子的 WiFi 改掉），断言的是**拒绝路径不落盘**：
# 四种非法输入都 400，且之后导出的 SSID 一字不变。
if [ "$WIFI_ABSENT" = 1 ]; then
    echo "  ⏭  跳过 W01–W05：纯以太网剖面没有 /wifi/config（上面 E02 已断言 404）"; SKIP=$((SKIP+1))
else
WIFI_SSID_BEFORE=$(api /config/export | jq_ wifi.ssid)
eq "W01 /wifi/config 空体 400"        "400" "$(code POST /wifi/config)"
eq "W02 /wifi/config 空 SSID 400"     "400" "$(code POST /wifi/config '{"ssid":""}')"
eq "W03 /wifi/config 7 位密码 400"    "400" "$(code POST /wifi/config '{"ssid":"x","password":"1234567"}')"
eq "W04 /wifi/config 33 位 SSID 400"  "400" "$(code POST /wifi/config "{\"ssid\":\"$(printf 'a%.0s' {1..33})\"}")"
eq "W05 四次拒绝后 SSID 未变"         "$WIFI_SSID_BEFORE" "$(api /config/export | jq_ wifi.ssid)"
fi

# /zigbee/channel：越界拒绝不落盘；用当前声明值重声明一次 → 200 回显 + /zigbee/status 读回一致。
# 重声明会顺带重探自家 PAN（thread_br_probe_pan，800ms × 若干轮；探不到还会全频段找最强的，
# 约 10s），所以单独给 30s 超时。probe 结果只报不判 —— 自家 beacon 不是每轮都抓得到。
ZB_CH_BEFORE=$(api /config/export | jq_ zigbee.channel); ZB_DECL_BEFORE=$(api /config/export | jq_ zigbee.channel_declared)
eq "Z01 /zigbee/channel 越界 400"     "400" "$(code POST /zigbee/channel '{"channel":5}')"
eq "Z02 越界后声明值未变"             "$ZB_CH_BEFORE" "$(api /config/export | jq_ zigbee.channel)"
if [ "$ZB_DECL_BEFORE" == "true" ]; then
    ZR=$(curl -s -m 30 ${AUTH[@]+"${AUTH[@]}"} -X POST "http://$GW/api/v1/zigbee/channel" -d "{\"channel\":$ZB_CH_BEFORE}")
    eq "Z03 重声明当前信道 200 且回显"   "$ZB_CH_BEFORE" "$(echo "$ZR" | jq_ channel)"
    eq "Z04 /zigbee/status 读回同一信道" "$ZB_CH_BEFORE" "$(api /zigbee/status | jq_ channel)"
    echo "     ℹ️  probe: pan_id=$(echo "$ZR" | jq_ pan_id) rssi=$(echo "$ZR" | jq_ rssi)"
else
    echo "  ⏭  Z03/Z04 跳过：这块板没有声明 Zigbee 信道"; SKIP=$((SKIP+2))
fi

# /zigbee/restart **不在自动化里真按**：AM1 板上它经 IO23 拉塌 EN = 整板断电重启，而且是
# 硬件复位、没有软重启前的 PHY 复位缓解，每按一次都是 IO28 下载模式的抽奖（产测指南
# §7.1/§7.2）。属 ③ 档人工用例。这里只钉住前端赖以显示警告的字段存在且是布尔。
eq "R01 /zigbee/status 带 restart_resets_board 布尔" "true" \
   "$(api /zigbee/status | python3 -c 'import json,sys; d=json.load(sys.stdin); print(str(isinstance(d.get("restart_resets_board"),bool)).lower())')"

# ─────────────────────────── OTBR 扩展端点 ───────────────────────────
# /node/coprocessor/version（T3.5）有实现无断言 —— 2026-09-13 审计发现，补一条最低限度的。
echo ""; echo "── OTBR 扩展端点 ──"
eq "O01 /node/coprocessor/version 200" "200" "$(C -o /dev/null -w '%{http_code}' "http://$GW/node/coprocessor/version")"

# ─────────────────────────── 频谱：邻居扫描（T4.8）───────────────────────────
# 2026-09-13 之前出货构型下恒 501。现在走 OT 主动扫描 + PCAP，需要 Thread BR 在跑：
# 在跑 → 200 且回对象（completed/networks）；没跑 → 503 thread_not_running_or_busy。
# 两种期望都断言，别把"Thread 没开"当成扫描坏了。约 7s（16 信道 × 400ms），单独给超时。
echo ""; echo "── 频谱：邻居扫描 ──"
THREAD_ROLE=$(C "http://$GW/node/state" | tr -d '"[:space:]')
BEACON_RAW=$(curl -s -m 25 ${AUTH[@]+"${AUTH[@]}"} -w '\n%{http_code}' "http://$GW/api/v1/spectrum/beacon")
BEACON_CODE=$(echo "$BEACON_RAW" | tail -1); BEACON_BODY=$(echo "$BEACON_RAW" | sed '$d')
# Thread 在跑却 503：撞上了 thread_br 自己的周期性 Zigbee 信道检查（它不拿扫描锁，beacon_scan.c 注明"重试即可"）。
# 只重试一次；Thread 没跑、或重试仍 503 照样红（2026-10-21 直连型上撞到一次，单独复现 4/4 都是 200）。
if [ "$BEACON_CODE" = "503" ] && [ -n "$THREAD_ROLE" ] && [ "$THREAD_ROLE" != "disabled" ]; then
    echo "     ℹ️  第一次 503（OT 正忙），3 s 后重试一次"; sleep 3
    BEACON_RAW=$(curl -s -m 25 ${AUTH[@]+"${AUTH[@]}"} -w '\n%{http_code}' "http://$GW/api/v1/spectrum/beacon")
    BEACON_CODE=$(echo "$BEACON_RAW" | tail -1); BEACON_BODY=$(echo "$BEACON_RAW" | sed '$d')
fi
if [ -n "$THREAD_ROLE" ] && [ "$THREAD_ROLE" != "disabled" ]; then
    eq "S01 Thread 在跑（$THREAD_ROLE）→ /spectrum/beacon 200" "200" "$BEACON_CODE"
    eq "S02 回包是对象且带 completed" "true" "$(echo "$BEACON_BODY" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(str(isinstance(d,dict) and isinstance(d.get("completed"),bool) and isinstance(d.get("networks"),list)).lower())' 2>/dev/null)"
    eq "S03 count 与 networks 长度一致" "true" "$(echo "$BEACON_BODY" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(str(d.get("count")==len(d.get("networks",[]))).lower())' 2>/dev/null)"
    # 自家协调器在同板几厘米外（-20 dBm）——抓到它就证明整条链（OT 扫描 → PCAP → 合并 → self 判定）是通的。
    # ⚠️ 不是每轮都抓得到（thread_br.c 实测 800ms 才稳定，这里默认 400ms），所以只报不判。
    echo "     ℹ️  networks=$(echo "$BEACON_BODY" | jq_ count) completed=$(echo "$BEACON_BODY" | jq_ completed) self=$(echo "$BEACON_BODY" | python3 -c 'import json,sys; d=json.load(sys.stdin); print([hex(n["pan_id"]) for n in d.get("networks",[]) if n.get("self")])' 2>/dev/null)"
else
    eq "S01 Thread 没跑 → /spectrum/beacon 503" "503" "$BEACON_CODE"
    eq "S02 错误码分清是 Thread 没跑" "thread_not_running_or_busy" "$(echo "$BEACON_BODY" | jq_ error)"
fi

echo ""; echo "── MG24 固件验签与开发者解锁（T1.2）──"
# 造一个最小的**未签名** GBL（头 + 一段 PROG + END/CRC，160 字节 ≥ 128 下限）。
# 不用 mg24/release 里的文件：步骤 4 之后那个是签名版，这条断言就会失去意义。
FAKE_GBL=$(mktemp)
python3 - "$FAKE_GBL" <<'PY'
import struct, zlib, sys
def tag(i, p): return struct.pack('<II', i, len(p)) + p
d = tag(0x03A617EB, struct.pack('<II', 0x03000000, 0)) + tag(0xFE0101FE, struct.pack('<I', 0x08006000) + b'\xff' * 120)
h = d + struct.pack('<II', 0xFC0404FC, 4)
open(sys.argv[1], 'wb').write(h + struct.pack('<I', zlib.crc32(h)))
PY
zb_up() { C -X POST "http://$GW/api/v1/ota/zigbee" -H 'Content-Type: application/octet-stream' --data-binary @"$FAKE_GBL" | jq_ error; }
eq "未签名 .gbl 在碰 MG24 之前被拒"        "ncp_image_unsigned" "$(zb_up)"
eq "/status: 出厂锁着（ncp_signing_unlocked=false）" "false" "$(api /status | jq_ ncp_signing_unlocked)"
r=$(apip /ota/zigbee/unlock '{}')
eq "发起解锁 → armed"                       "true"  "$(echo "$r" | jq_ armed)"
eq "发起解锁 ≠ 已解锁"                      "false" "$(echo "$r" | jq_ unlocked)"
# ★ 这条是这个特性的全部意义：密码只能"发起"，没有人在设备旁边按键就什么都不放行。
#   退化成"发起即解锁"时返回值会变成 202/accepted —— 那等于把密钥交给了远程攻击者。
eq "★ 只发起、没按键，未签名包仍被拒"       "ncp_image_unsigned" "$(zb_up)"
r=$(apip /ota/zigbee/unlock '{"lock":true}')
eq "lock 取消发起"                          "false" "$(echo "$r" | jq_ armed)"
eq "GET /ota/zigbee/unlock 可读且仍锁着"    "false" "$(api /ota/zigbee/unlock | jq_ unlocked)"
eq "unlock GET 未登录必须 401"              "401"   "$(curl -s -m 5 -o /dev/null -w '%{http_code}' "http://$GW/api/v1/ota/zigbee/unlock")"
rm -f "$FAKE_GBL"
# 按键确认那半边要人在设备旁边：docs/test_cases_mg24.md TC-M26

echo ""; echo "── 防串刷：另一种固件的镜像在写 flash 之前被拒（docs/15 设计 2.7，TC-PF01 的网页那一半）──"
# 传的是"另一种"的工程名：HA 型设备传直连型镜像（正方向），直连型设备传 HA 型镜像（反方向）。
# ⚠️ 写死 bitengine-direct 的话，对直连型设备它是**同名**，守卫放行，备用槽会被擦写（2026-10-21 上板前发现）。
PF_OTHER=$( [ "$(api /status | jq_ profile)" == "direct" ] && echo bitengine || echo bitengine-direct )
# 两种固件同一把签名钥匙，esp_ota_end 的验签挡不住串刷；挡它的是写 flash 之前比对镜像的工程名。
# 造的只是镜像开头（头 + 段头 + app 描述，288 字节）：工程名不对时设备在 esp_ota_begin 之前就拒，
# 一个字节都不写，所以这条不具破坏性、每次都跑。万一守卫失效，这个残缺镜像也只会在 esp_ota_end 被拒（500），
# 不会被装上 —— 但那时下一次 OTA 的备用槽已经被擦写过，所以判据看的是"409 + profile_mismatch"，不是"没装上"。
FAKE_APP=$(mktemp)
mk_app() { python3 - "$FAKE_APP" "$1" "$2" <<'PY'
import sys
out, name, length = sys.argv[1], sys.argv[2].encode(), int(sys.argv[3])
b = bytearray(288)
b[0] = 0xE9; b[1] = 6; b[12] = 23                       # 镜像头：magic、段数、chip_id = ESP32-C5
b[32:36] = (0xABCD5432).to_bytes(4, "little")             # app 描述 magic（镜像偏移 32）
b[48:48 + 17] = b"v9.9.9-0-g0000000"                      # version（偏移 48）
b[80:80 + len(name)] = name                               # project_name（偏移 80，32 字节）
open(out, "wb").write(bytes(b[:length]))
PY
}
app_up() { C -m 30 -X POST "http://$GW/api/v1/ota/esp32" -H 'Content-Type: application/octet-stream' --data-binary @"$FAKE_APP" -w '\n%{http_code}'; }
# ⚠️ 只对带守卫的固件跑：没有守卫的旧固件会把这 288 字节当正常上传，esp_ota_begin(OTA_SIZE_UNKNOWN) 先把
#    整个备用槽擦掉、还作废它的 otadata（回滚目标就没了），最后才在 esp_ota_end 失败。守卫从 v1.0.0-6 起有；
#    工作区编的 -dirty 固件版本号看不出来，确认带守卫时用 PF_FORCE=1 强制跑。
PF_FW=$(api /status | jq_ fw_version)
if [ "${PF_FORCE:-0}" != 1 ] && ! python3 - "$PF_FW" <<'PY'
import re, sys
m = re.match(r"^v(\d+)\.(\d+)\.(\d+)(?:-(\d+)-g[0-9a-f]+)?", sys.argv[1])
sys.exit(0 if m and tuple(int(x or 0) for x in m.groups()) >= (1, 0, 0, 6) else 1)
PY
then
    echo "  ⏭  跳过：固件 $PF_FW 早于 v1.0.0-6，可能没有防串刷守卫，跑了会擦掉备用槽（确认带守卫可 PF_FORCE=1）"; SKIP=$((SKIP+1))
else
UP_PF0=$(api /status | jq_ uptime)
mk_app "$PF_OTHER" 288; r=$(app_up)
eq "PF1 另一种固件（$PF_OTHER）的镜像 → 409"   "409"              "$(echo "$r" | tail -1)"
eq "PF2 错误标识 profile_mismatch"            "profile_mismatch" "$(echo "$r" | head -1 | jq_ error)"
mk_app "$PF_OTHER" 100; r=$(app_up)
eq "PF3 不够 112 字节的残片 → 400 image_invalid（不往下写）" "400 image_invalid" "$(echo "$r" | tail -1) $(echo "$r" | head -1 | jq_ error)"
# 拒装路径必须复位"OTA 进行中"标志，否则测一次就把升级堵死（第二次会是 409 ota_already_in_progress）
mk_app "$PF_OTHER" 288; r=$(app_up)
eq "PF4 再传一次仍是 profile_mismatch（进行中标志已复位）" "profile_mismatch" "$(echo "$r" | head -1 | jq_ error)"
UP_PF1=$(api /status | jq_ uptime)
eq "PF5 设备没有重启"                         "true" "$([ "${UP_PF1:-0}" -ge "${UP_PF0:-1}" ] && echo true || echo false)"
fi
rm -f "$FAKE_APP"

# ─────────────────────────────── 收尾 ───────────────────────────────
restore
echo ""; echo "── 桥接未受影响 ──"
eq "tcp_clients 未变化"   "$CLIENTS_BEFORE" "$(api /status | jq_ tcp_clients)"
eq "MG24 仍在线"          "true"  "$(api /status | jq_ mg24_alive)"

echo ""
echo "==============================="
# ─────────────────────────────── 射频地区（docs/14，2026-09-15）───────────────────────────────
# ⚠️ 只在 --region 时跑：每次 POST 都独占 UART 把地区发给 NCP → Zigbee 断开约 40 s、MG24 软复位一次。
#    结束时 restore 会按备份还原 NVS，但 NCP 里的网关声明只在开机 / POST 时同步，
#    所以最后一步显式 POST 回原值，别留下"NVS 说 A、NCP 记着 B"。
if [ $REGION -eq 1 ]; then
echo ""; echo "── 射频地区（--region）──"
R0=$(api /radio/region)
ORIG_REGION=$(echo "$R0" | jq_ region)
eq  "/radio/region 可读"                    "200"   "$(code GET /radio/region)"
eq  "menu = EU/US/CA/ZZ（固件唯一一份菜单）" '["EU", "US", "CA", "ZZ"]' "$(echo "$R0" | jq_ menu)"
eq  "不在菜单的码被拒（DE）"                "400"   "$(code POST /radio/region '{"region":"DE"}')"
eq  "长度不对被拒（USA）"                   "400"   "$(code POST /radio/region '{"region":"USA"}')"
eq  "缺 region 键被拒"                      "400"   "$(code POST /radio/region '{}')"
eq  "非法 JSON 被拒"                        "400"   "$(code POST /radio/region 'not json')"
# ★ 设 US：存下、下发到 NCP、Thread 跟着变。小写输入证明规范化在固件里，不靠前端
R=$(apip /radio/region '{"region":"us"}')
eq  "小写 us 存成 US"                       "US"    "$(echo "$R" | jq_ region)"
eq  "declared=true"                         "true"  "$(echo "$R" | jq_ declared)"
eq  "GET 读回 US"                           "US"    "$(api /radio/region | jq_ region)"
eq  "导出配置带 radio_region=US"            "US"    "$(api /config/export | jq_ system.radio_region)"
APPLIED=$(echo "$R" | jq_ applied); XN=$(echo "$R" | jq_ zigbee.ncp_xncp)
if [ "$APPLIED" == "now" ] && [ "$XN" == "true" ]; then
    reg "NCP 侧网关声明 = US"               "US"    "$(echo "$R" | jq_ zigbee.gw_region)"
    eq  "NCP 侧网关等级 = FCC(3)"           "3"     "$(echo "$R" | jq_ zigbee.gw_class)"
    # 生效上限 = min(网关 19, ZHA 声明)：ZHA 没声明或也是 FCC → 19；ZHA 声明欧盟 → 10；未知国家 → 8
    CAP=$(echo "$R" | jq_ zigbee.cap_dbm)
    eq  "生效上限 ∈ {8,10,19}"              "yes"   "$(case "$CAP" in 8|10|19) echo yes;; *) echo "$CAP";; esac)"
    eq  "/zigbee/status.tx_power 与之一致"  "US"    "$(api /zigbee/status | jq_ tx_power.gw_region)"
else
    echo "  ⏭  NCP 侧断言跳过：applied=$APPLIED ncp_xncp=$XN（旧 NCP 固件，或 MG24 正忙）"; SKIP=$((SKIP+4))
fi
TH=$(echo "$R" | jq_ thread_tx_dbm)
if [ "$TH" != "null" ]; then eq "Thread 请求功率 US → 20" "20" "$TH"
else echo "  ⏭  Thread 未运行，跳过功率断言"; SKIP=$((SKIP+1)); fi
# 清除：region=null 必须被接受（不是 400），NCP 侧声明清空，Thread 回 10
R=$(apip /radio/region '{"region":null}')
eq  "清除后 region=null"                    "null"  "$(echo "$R" | jq_ region)"
eq  "清除后 declared=false"                 "false" "$(echo "$R" | jq_ declared)"
TH=$(echo "$R" | jq_ thread_tx_dbm)
[ "$TH" != "null" ] && eq "Thread 未设置 → 10" "10" "$TH"
if [ "$(echo "$R" | jq_ applied)" == "now" ] && [ "$(echo "$R" | jq_ zigbee.ncp_xncp)" == "true" ]; then
    eq  "NCP 侧网关声明清空"                ""      "$(echo "$R" | jq_ zigbee.gw_region)"
    eq  "NCP 侧网关等级 = NONE(0)"          "0"     "$(echo "$R" | jq_ zigbee.gw_class)"
fi
# 放回原值（NVS 由 restore 还原，NCP 侧要显式再 POST 一次）
if [ "$ORIG_REGION" != "null" ]; then apip /radio/region "{\"region\":\"$ORIG_REGION\"}" > /dev/null; fi
fi

echo "Result: $PASS passed, $FAIL failed, $SKIP skipped"
echo "（白名单的**实际拦截效果**不在本脚本内 —— 见 tools/test_whitelist.sh）"
[ $FAIL -eq 0 ] || exit 1
