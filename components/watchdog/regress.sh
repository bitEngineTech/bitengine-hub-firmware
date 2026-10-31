#!/bin/bash
# 看门狗回归测试 —— docs/test_cases_watchdog.md 中可自动化的部分
#
#   bash components/watchdog/regress.sh <网关IP>              全跑（含 4 次自愈重启）
#   bash components/watchdog/regress.sh <网关IP> --safe       跳过会重启的用例
#   bash components/watchdog/regress.sh <网关IP> --soak 600   只跑无注入的浸泡断言
#   bash components/watchdog/regress.sh --offline             只跑主机侧 + VERIFY
#
# 环境变量：GW_PASS=<WebUI 密码>（设过密码时必填）
#
# 需要故障注入固件：CONFIG_BITENGINE_FAULT_INJECTION=y 重新编译烧录。
# 出厂固件该项为 n，此时 /api/v1/diag/fault 返回 404，脚本会直接告诉你。
#
# ⚠️ 会重启的用例（W02/W08）每次都会打断 Z2M/ZHA 的连接，ZHA 通常几十秒内自己
#    重连；若它卡住不重连，重启 HA 容器即可（这是 bellows 的老问题，不是网关）。
#    不想被打扰就加 --safe。

set -u
cd "$(dirname "$0")/../.." || exit 1

HOST=""; SAFE=0; OFFLINE=0; SOAK=0
for a in "$@"; do
    case "$a" in
        --safe)    SAFE=1 ;;
        --offline) OFFLINE=1 ;;
        --soak)    SOAK=1 ;;
        [0-9]*)    if [ $SOAK -eq 1 ] && [ -z "${SOAK_S:-}" ]; then SOAK_S=$a
                   else HOST=$a; fi ;;
        *)         HOST=$a ;;
    esac
done
SOAK_S=${SOAK_S:-600}

PASS=0; FAIL=0; SKIP=0
ok()   { echo "  ✅ $1"; PASS=$((PASS+1)); }
bad()  { echo "  ❌ $1"; FAIL=$((FAIL+1)); }
skip() { echo "  ⏭  $1"; SKIP=$((SKIP+1)); }

AUTH=""
[ -n "${GW_PASS:-}" ] && AUTH="-u admin:$GW_PASS"

api() { curl -s -m "${2:-20}" $AUTH -H "Connection: close" "http://$HOST$1"; }
post() { curl -s -m "${3:-20}" $AUTH -H "Connection: close" -X POST \
         -H "Content-Type: application/json" -d "$2" "http://$HOST$1"; }

# jq 不一定装了，用 python 取字段（仓库其它脚本也是这么做的）
field() { python3 -c "
import json,sys
try: d=json.load(sys.stdin)
except Exception: print('__ERR__'); sys.exit()
for k in '$1'.split('.'):
    d = d[int(k)] if k.isdigit() else d.get(k, '__MISS__')
    if d == '__MISS__': break
print(d)"; }

wait_up() {
    local t=0
    while [ $t -lt 180 ]; do
        if api /api/v1/diag/watchdog 8 | grep -q '"tasks"'; then return 0; fi
        sleep 5; t=$((t+5))
    done
    return 1
}

# ============================================================
echo "=== 0/4 主机侧单元测试（不需要板子）==="
if bash components/watchdog/test_host/run.sh > /tmp/wd_host.log 2>&1; then
    ok "主机侧 registry/recovery/forensics 全部通过"
    grep -E "passed" /tmp/wd_host.log | sed 's/^/     /'
else
    bad "主机侧测试失败，详见 /tmp/wd_host.log"
    tail -20 /tmp/wd_host.log | sed 's/^/     /'
fi

echo
echo "=== 1/4 静态断言 ==="
if bash components/watchdog/VERIFY.sh > /tmp/wd_verify.log 2>&1; then
    ok "VERIFY.sh $(grep Result /tmp/wd_verify.log)"
else
    bad "VERIFY.sh 失败"; grep "❌" /tmp/wd_verify.log | sed 's/^/     /'
fi

if [ $OFFLINE -eq 1 ]; then
    echo; echo "==============================="
    echo "结果: $PASS 通过, $FAIL 失败, $SKIP 跳过（离线模式）"
    [ $FAIL -eq 0 ] || exit 1; exit 0
fi

[ -z "$HOST" ] && { echo "用法: $0 <网关IP> [--safe|--soak N]"; exit 2; }

echo
echo "=== 2/4 基线检查 ($HOST) ==="
# 刚烧完固件时板子会做一次刻意的软复位（main.c 的非干净复位处理），
# 直接请求会落在 httpd 还没起来的窗口里，所以先等。
if ! wait_up; then
    bad "3 分钟内拿不到 /api/v1/diag/watchdog（认证？固件版本？板子没起来？）"
    api /api/v1/diag/watchdog | head -2
    exit 1
fi
WD=$(api /api/v1/diag/watchdog)
ok "诊断端点可用"

SM=$(echo "$WD" | field safe_mode)
[ "$SM" = "False" ] && ok "不在安全模式" || bad "处于安全模式，先退出再跑：POST /api/v1/diag/safemode/exit"

for lv in task subsys resource periph; do
    v=$(echo "$WD" | field "levels.$lv")
    [ "$v" = "0" ] && ok "levels.$lv = 0" || bad "levels.$lv = $v（上一轮的级别未回落，冷却会污染结果）"
done

# 每个任务都在 deadline 内
STALE=$(echo "$WD" | python3 -c "
import json,sys
d=json.load(sys.stdin)
bad=[t['name'] for t in d['tasks'] if t['age_ms'] >= t['deadline_ms'] and not t['suspended']]
print(','.join(bad) if bad else 'OK')")
NTASKS=$(echo "$WD" | python3 -c "import json,sys;print(len(json.load(sys.stdin)['tasks']))")
[ "$STALE" = "OK" ] && ok "全部 $NTASKS 个任务在 deadline 内" \
                    || bad "已超 deadline 的任务: $STALE"

# 注入端点是否存在
FCODE=$(curl -s -o /dev/null -w "%{http_code}" -m 10 $AUTH -X POST \
        -H "Content-Type: application/json" -d '{"kind":"clear"}' \
        "http://$HOST/api/v1/diag/fault")
if [ "$FCODE" = "200" ]; then
    INJECT=1; ok "故障注入端点可用"
else
    INJECT=0; skip "故障注入端点不可用（HTTP $FCODE）——出厂固件正常如此；"
    echo "     要跑 TC-W01~W08 需 CONFIG_BITENGINE_FAULT_INJECTION=y 重新烧录"
fi

echo
echo "=== 3/4 浸泡断言（无注入，$SOAK_S 秒）==="
if [ $SOAK -eq 1 ] || [ $SOAK_S -gt 0 ]; then
    F0=$(echo "$WD" | field resource.alloc_failures)
    echo "  起始 alloc_failures=$F0，等待 ${SOAK_S}s ..."
    sleep "$SOAK_S"
    WD2=$(api /api/v1/diag/watchdog)
    F1=$(echo "$WD2" | field resource.alloc_failures)
    # 内存两条与扩容硬停判据同口径（正本 docs/test_cases_scale_baseline.md「硬停判据」第 3、6 条，阈值别在这里另定）。
    # 原来要求 internal_min / dma_min 都 ≥ 10 KB，是另一套更早的口径，现在的发布固件 DMA 读数就够不着（T9.19 ⑪）。
    HEAP=$(api /api/v1/diag/heap)
    ILARGE=$(echo "$HEAP" | field internal.largest)
    DMIN=$(echo "$HEAP" | field dma.min_free)
    [ "$F1" = "$F0" ] && ok "alloc_failures 无增长（$F1）" \
                      || bad "alloc_failures $F0 → $F1（协议栈级丢包，见 sdkconfig.defaults 验收口径）"
    [ "$ILARGE" -ge 3072 ] 2>/dev/null && ok "internal.largest=$ILARGE ≥ 3072（硬停判据 3）" \
                                       || bad "internal.largest=$ILARGE < 3072（硬停判据 3：建不起任务栈）"
    [ "$DMIN" -ge 3072 ] 2>/dev/null && ok "dma.min_free=$DMIN ≥ 3072（硬停判据 6）" \
                                     || bad "dma.min_free=$DMIN < 3072（硬停判据 6；读数偏悲观，见基线文档「第 6 条的读数偏悲观」）"
fi

echo
echo "=== 4/4 故障注入用例 ==="
if [ $INJECT -eq 0 ]; then
    skip "TC-W01/W03/W07（需注入固件）"
    [ $SAFE -eq 1 ] && skip "TC-W02/W08（--safe）" || skip "TC-W02/W08（需注入固件）"
else
    # ---- TC-W01：OBSERVE 级只记录不恢复 ----
    echo "  TC-W01 挂起 led_task 30s（OBSERVE）"
    post /api/v1/diag/fault '{"kind":"task_hang","target":"led_task","ms":30000}' > /dev/null
    sleep 25
    W=$(api /api/v1/diag/watchdog)
    [ "$(echo "$W" | field levels.task)" = "1" ] && ok "TC-W01 停在 L1" || bad "TC-W01 级别不是 L1"
    [ "$(echo "$W" | field last_action)" = "L1 log" ] && ok "TC-W01 动作是 L1 log" \
                                                      || bad "TC-W01 动作: $(echo "$W" | field last_action)"
    sleep 15

    # ---- TC-W03：分配失败驱动资源判定 ----
    echo "  TC-W03 抽干 DMA 内存并重复 12 次"
    for i in $(seq 1 12); do
        post /api/v1/diag/fault '{"kind":"heap_drain","target":"dma","bytes":8192}' 10 > /dev/null
    done
    echo "     等窗口关闭（70s）..."
    sleep 70
    W=$(api /api/v1/diag/watchdog)
    CAPS=$(echo "$W" | field resource.last_caps)
    [ "$(echo "$W" | field levels.resource)" != "0" ] && ok "TC-W03 资源级别已升（L$(echo "$W" | field levels.resource)）" \
                                                      || bad "TC-W03 资源级别仍为 0"
    [ "$CAPS" = "8" ] && ok "TC-W03 失败 caps=0x8 (DMA)" || bad "TC-W03 caps=$CAPS，期望 8"
    post /api/v1/diag/fault '{"kind":"clear"}' > /dev/null
    ok "TC-W03 已清除注入"

    # ---- TC-W07：httpd 被堵 → L3 重启 ----
    echo "  TC-W07 堵住 httpd 90s（deadline 60s）"
    post /api/v1/diag/fault '{"kind":"httpd_block","ms":90000}' > /dev/null
    sleep 130
    if wait_up; then
        W=$(api /api/v1/diag/watchdog 25)
        H3=$(echo "$W" | python3 -c "
import json,sys
d=json.load(sys.stdin)
print('yes' if any(h['level']==3 and 'httpd' in h['detail'] for h in d['history']) else 'no')")
        [ "$H3" = "yes" ] && ok "TC-W07 取证里有 httpd 的 L3 记录" || bad "TC-W07 取证里没有 L3 记录"
        [ "$(echo "$W" | field probes.httpd_ok)" = "True" ] && ok "TC-W07 httpd 已恢复" \
                                                            || bad "TC-W07 httpd 未恢复"
    else
        bad "TC-W07 后网关未恢复响应"
    fi

    # ---- TC-W02/W08：会重启，默认跑，--safe 跳过 ----
    if [ $SAFE -eq 1 ]; then
        skip "TC-W02/W08（--safe：会触发 4 次重启并打断 Zigbee）"
    else
        echo "  TC-W02 挂起 health 40s（FATAL → L4 重启）"
        post /api/v1/diag/fault '{"kind":"task_hang","target":"health","ms":40000}' > /dev/null
        sleep 60
        if wait_up; then
            W=$(api /api/v1/diag/watchdog)
            H4=$(echo "$W" | python3 -c "
import json,sys
d=json.load(sys.stdin)
print('yes' if any(h['level']==4 and 'health' in h['detail'] for h in d['history']) else 'no')")
            [ "$H4" = "yes" ] && ok "TC-W02 取证里有 health 的 L4 记录" || bad "TC-W02 取证里没有 L4 记录"
            SH=$(echo "$W" | field self_heal_count)
            [ "$SH" -ge 1 ] 2>/dev/null && ok "TC-W02 自愈计数=$SH" || bad "TC-W02 自愈计数未增长"
        else
            bad "TC-W02 后网关未恢复响应"
        fi

        echo "  TC-W08 再触发 2 次自愈 → 应进安全模式"
        for r in 1 2; do
            wait_up || break
            post /api/v1/diag/fault '{"kind":"task_hang","target":"health","ms":40000}' > /dev/null
            sleep 70
        done
        if wait_up; then
            W=$(api /api/v1/diag/watchdog)
            [ "$(echo "$W" | field safe_mode)" = "True" ] && ok "TC-W08 已进入安全模式" \
                                                          || bad "TC-W08 未进入安全模式"
            NT=$(echo "$W" | python3 -c "import json,sys;print(len(json.load(sys.stdin)['tasks']))")
            [ "$NT" -lt 11 ] 2>/dev/null && ok "TC-W08 子系统已裁剪（$NT 个任务）" \
                                         || bad "TC-W08 任务数 $NT，未见裁剪"

            echo "  TC-W08b 退出安全模式"
            post /api/v1/diag/safemode/exit '{}' > /dev/null
            sleep 15
            if wait_up; then
                [ "$(api /api/v1/diag/watchdog | field safe_mode)" = "False" ] \
                    && ok "TC-W08b 已退出安全模式" || bad "TC-W08b 仍在安全模式"
            else
                bad "TC-W08b 退出后网关未恢复"
            fi
        else
            bad "TC-W08 后网关未恢复响应"
        fi
    fi
fi

echo
echo "==============================="
echo "结果: $PASS 通过, $FAIL 失败, $SKIP 跳过"
echo
echo "未自动化（见 docs/test_cases_watchdog.md）："
echo "  TC-W04/W05  MG24 静默 —— 需人工确认探测窗口不把 Z2M 打掉"
echo "  TC-W06      Thread 射频冻结 —— 需真实 Thread 子设备在网（结尾会整机重启）"
echo "  TC-W11      网页停用 Thread 不触发看门狗 —— 约 6 分钟，测完记得 enable"
echo "  TC-W09      OTA 期间豁免 —— 需真实 MG24 升级流程"
echo "  条件喂狗/L5 —— 等 AM2 修好 A6，AM1 上无法验证"
[ $FAIL -eq 0 ] || exit 1
