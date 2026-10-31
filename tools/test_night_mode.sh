#!/bin/bash
# tools/test_night_mode.sh —— 夜间模式抗事件干扰的自动回归（需要一块在网的板子）
#
#   bash tools/test_night_mode.sh <网关IP>
#
# 验的是 2026-08-15 修掉的那个 bug：LED_STATE_NIGHT 曾和 BOOT/NORMAL/ERROR 挤在
# 同一个枚举里，led_event_handler() 写 s_state 时不看夜间，夜里网络一抖就把它顶掉；
# 而 scheduler 的夜间判定是边沿触发的，**永远不会再设回来** —— 灯亮到天亮。
#
# ⚠️ 为什么不能靠眼睛：闪一下只有几十毫秒，而且要连续盯几分钟。所以本脚本采的是
#    GET /api/v1/led/test 的 pixels 字段（灯此刻实际在显示什么），全程逐帧断言全零。
#
# ⚠️ 会临时改夜间模式配置，结束时（含失败/Ctrl-C）自动还原。

set -u
GW="${1:?用法: $0 <网关IP或主机名>}"
PASS=0; FAIL=0

# 设备设了密码时必须给凭据，否则每个请求都是 401，断言会以"值不对"的形式一片红，
# 看起来像功能坏了。用户名留空，固件只验冒号后那半（auth.c 的 check_basic_auth）。
#   GW_PASS=<设备密码> bash tools/test_night_mode.sh <网关IP>
# ⚠️ -u 会把密码放进 argv，同机其他用户 ps 得到；这些是本地调试/产线工具，接受。
AUTH=()
[ -n "${GW_PASS:-}" ] && AUTH=(-u ":$GW_PASS")

# 401 预检：必须在任何断言之前。curl 收到 401 也返回 0，所以不能靠退出码探。
_sc=$(curl -s -m 10 ${AUTH[@]+"${AUTH[@]}"} -o /dev/null -w '%{http_code}' \
      "http://$GW/api/v1/status")
if [ "$_sc" == "401" ]; then
    echo "❌ 需要认证（HTTP 401）。设备设了密码，请带上凭据重跑："
    echo "     GW_PASS=<设备密码> bash tools/test_night_mode.sh <网关IP>"
    exit 1
fi
C() { curl -s -m 10 ${AUTH[@]+"${AUTH[@]}"} "$@"; }
ok()  { echo "  ✅ $1"; PASS=$((PASS+1)); }
bad() { echo "  ❌ $1"; FAIL=$((FAIL+1)); }

BACKUP=$(C "http://$GW/api/v1/config/export" | python3 -c "
import json,sys; l=json.load(sys.stdin)['led']
print('%s %02d:%02d %02d:%02d'%(str(l['night_mode_enabled']).lower(),
      l['night_start_hour'],l['night_start_min'],l['night_end_hour'],l['night_end_min']))")
restore() {
    set -- $BACKUP
    C -X POST -H 'Content-Type: application/json' \
      -d "{\"enabled\":$1,\"start\":\"$2\",\"end\":\"$3\"}" \
      "http://$GW/api/v1/led/nightmode" > /dev/null
    echo "  ↩ 夜间配置已还原: enabled=$1 $2-$3"
}
trap restore EXIT

led_state() { C "http://$GW/api/v1/status" | python3 -c "import json,sys;print(json.load(sys.stdin)['led_state'])"; }
pixels()    { C "http://$GW/api/v1/led/test" | python3 -c "import json,sys;print(json.load(sys.stdin).get('pixels'))"; }
all_dark()  { [ "$(pixels)" = "[[0, 0, 0], [0, 0, 0], [0, 0, 0], [0, 0, 0]]" ]; }

echo "=== 1. 让夜间模式覆盖当前时刻 ==="
read -r H M <<< "$(C "http://$GW/api/v1/ntp" | python3 -c "
import json,sys; t=json.load(sys.stdin)['local_time'].split()[1].split(':'); print(t[0],t[1])")"
START=$(printf '%02d:%02d' $((10#$H)) $((10#$M)))
END=$(printf '%02d:%02d' $(( (10#$H+2)%24 )) $((10#$M)))
echo "  本地时间 $H:$M，窗口设为 $START-$END"
C -X POST -H 'Content-Type: application/json' \
  -d "{\"enabled\":true,\"start\":\"$START\",\"end\":\"$END\"}" \
  "http://$GW/api/v1/led/nightmode" > /dev/null

echo "  等 scheduler 的 60s tick..."
for _ in $(seq 1 12); do [ "$(led_state)" = "night" ] && break; sleep 8; done
[ "$(led_state)" = "night" ] && ok "夜间模式已生效（led_state=night）" \
                             || { bad "夜间模式没生效，后面不用测了"; exit 1; }
all_dark && ok "四颗灯实际全灭（pixels 全零）" || bad "夜间却不是全零: $(pixels)"

echo ""
echo "=== 2. 打入会写 s_state 的事件，逐帧盯像素 ==="
# 用 OTA 失败：该路径不重启、不动 Zigbee 桥和 Thread BR，只发 WEBUI_EVENT_OTA_FAILED，
# 而 led_event_handler 收到它会把 s_state 写成 LED_STATE_ERROR（红色快闪）。
head -c 4096 /dev/urandom | C -X POST --data-binary @- "http://$GW/api/v1/ota/esp32" > /dev/null

LIT=0
for i in $(seq 1 40); do          # 40 × 0.5s = 20s，采样密度远高于人眼
    P="$(pixels)"
    if [ "$P" != "[[0, 0, 0], [0, 0, 0], [0, 0, 0], [0, 0, 0]]" ]; then
        LIT=$((LIT+1)); echo "     第 $i 次采样亮了: $P"
    fi
    sleep 0.5
done
[ "$LIT" -eq 0 ] && ok "事件后 20s / 40 次采样，像素始终全零" \
                 || bad "事件把灯点亮了 $LIT 次 —— 夜间模式又被顶掉了"
[ "$(led_state)" = "night" ] && ok "led_state 仍是 night" || bad "led_state 变成了 $(led_state)"

echo ""
echo "=== 3. 退出夜间窗后必须恢复显示 ==="
C -X POST -H 'Content-Type: application/json' \
  -d '{"enabled":false,"start":"22:00","end":"07:00"}' "http://$GW/api/v1/led/nightmode" > /dev/null
for _ in $(seq 1 12); do [ "$(led_state)" != "night" ] && break; sleep 8; done
[ "$(led_state)" != "night" ] && ok "已退出夜间（led_state=$(led_state)）" || bad "退不出夜间模式"
all_dark && bad "退出夜间后灯仍全灭 —— 抑制没解除" || ok "灯已恢复显示"

echo ""
echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
