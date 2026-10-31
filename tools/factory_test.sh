#!/bin/bash
# tools/factory_test.sh —— 产线整机自检（一块板一次，约 1 分钟）
#
#   bash tools/factory_test.sh <网关IP>            # 完整流程（含人工确认项）
#   bash tools/factory_test.sh <网关IP> --auto     # 只跑自动项，不等人（烧录后冒烟用）
#   bash tools/factory_test.sh <网关IP> --power=usb   # 这台是 USB 供电（PoE 项按"不适用"消掉）
#   bash tools/factory_test.sh <网关IP> --retest      # 允许同一块 MAC 再测一次（返修复测）
#
# 供电方式固件判不出来：没检测到 PoE 时，"这块板没用 PoE 供电"和"PoE 分压电路坏了"
# 数字读数完全相同（poe.h 有实测表）。所以由本脚本按 --power 告诉它该怎么算：
#   --power=poe（默认）  未检测到 PoE = 不合格
#   --power=usb          未检测到 PoE = 正常，按"不适用"消掉
#
# 设计原则见 components/webui/factory_api.c 顶部：判据集中在固件里，本脚本只负责
# 驱动流程和收口。**manual 项绝不算 pass** —— 整机只有零 fail、零 manual 才放行。
#
# 前置：网线插好、板子已上电并拿到 IP。出厂态无密码，直接可调。
#
# ⚠️ **别紧跟在 tools/test_api.sh 后面跑**：那个脚本会调两次 /zigbee/stats/reset，
#    把 UART 字节计数清零，于是 ezsp_uart 会以"rx/tx 不足 64"的样子假红。
#    桥上的 EZSP 流量是每 8-10 秒一次突发，等一分钟自己就过了。产线不跑 test_api.sh，
#    所以这只影响开发机上的连跑（2026-09-11 踩过一次）。

set -u
GW="${1:?用法: $0 <网关IP> [--auto] [--power=poe|usb] [--retest]}"
shift
AUTO=0; POWER=poe; RETEST=0
for a in "$@"; do
    case "$a" in
        --auto)        AUTO=1 ;;
        --power=poe)   POWER=poe ;;
        --power=usb)   POWER=usb ;;
        --retest)      RETEST=1 ;;
        # 静默忽略 = 打错一个字就等于没传，而 --power 打错会让 PoE 故障被当成正常放行
        *) echo "❌ 不认识的参数：$a（可用：--auto --power=poe|usb --retest）"; exit 2 ;;
    esac
done


# 设备设了密码时必须给凭据，否则每个请求都是 401，断言会以"值不对"的形式一片红，
# 看起来像功能坏了。用户名留空，固件只验冒号后那半（auth.c 的 check_basic_auth）。
#   GW_PASS=<设备密码> bash tools/factory_test.sh <网关IP> [--auto]
# ⚠️ -u 会把密码放进 argv，同机其他用户 ps 得到；这些是本地调试/产线工具，接受。
AUTH=()
[ -n "${GW_PASS:-}" ] && AUTH=(-u ":$GW_PASS")

# 401 预检：必须在任何断言之前。curl 收到 401 也返回 0，所以不能靠退出码探。
# ⚠️ 探针必须打**无条件** AUTH_REQUIRED 的端点。此前探的是 /api/v1/status ——
#    它是 AUTH_REQUIRED_UNLESS_OPEN，rest_auth 出厂关时无凭据也回 200，这段预检
#    从写下那天起就是死代码（2026-09-13 用 pwsh 首次真跑 .ps1 才发现：没带密码
#    看到的是 .NET 异常栈，不是那句人话）。/api/v1/ntp 与前端门禁探针同款；
#    tools/check_factory_test_parity.py 钉着"两边同一端点 + 固件侧确为无条件 AUTH_REQUIRED"。
_sc=$(curl -s -m 10 ${AUTH[@]+"${AUTH[@]}"} -o /dev/null -w '%{http_code}' \
      "http://$GW/api/v1/ntp")
if [ "$_sc" == "401" ]; then
    echo "❌ 需要认证（HTTP 401）。设备设了密码，请带上凭据重跑："
    echo "     GW_PASS=<设备密码> bash tools/factory_test.sh <网关IP> [--auto]"
    exit 1
fi
C() { curl -s -m 10 ${AUTH[@]+"${AUTH[@]}"} "$@"; }
SELF="http://$GW/api/v1/factory/selftest"

show() {   # 打印一次完整自检
    C "$SELF" | python3 -c "
import json,sys
d=json.load(sys.stdin)
m={'pass':'✅','fail':'❌','manual':'🟡'}
for i in d['items']:
    print('  %s %-14s %s'%(m[i['state']], i['name'], i['detail']))
print()
print('  verdict = %s   (fail=%d, manual=%d, acked=%d)'%(d['verdict'],d['fail'],d['manual'],d.get('acked',0)))
"
}
verdict() { C "$SELF" | python3 -c "import json,sys;print(json.load(sys.stdin)['verdict'])"; }

# 把人工判定回写设备。此前操作员在 shell 里答了 y，那个答案从不回写 ——
# 于是 led_visual 永远是 manual，verdict 的 pass 分支是死分支，本脚本必然 exit 1。
ACK="http://$GW/api/v1/factory/ack"
ack() {   # ack <item> <pass|fail>
    C -X POST -H 'Content-Type: application/json' \
      -d "{\"item\":\"$1\",\"result\":\"$2\"}" "$ACK" > /dev/null \
      || { echo "  ⚠️  回写 $1=$2 失败（固件是否为 2026-08-17 之后的版本？）"; return 1; }
}
# 同一块板重测时，先清掉上一轮的确认，否则会把上次的 y 当成这次的
C -X POST -H 'Content-Type: application/json' -d '{"clear":true}' "$ACK" > /dev/null 2>&1
# 取某一项的 state。⚠️ 项名打错时必须**报错**而不是返回空串 —— 返回空串的话
# `| grep -q manual` 恒假，那一项就被静默跳过，产线以为测了其实没测。
item_state() { C "$SELF" | python3 -c "
import json,sys
want='$1'
m=[i['state'] for i in json.load(sys.stdin)['items'] if i['name']==want]
if not m: sys.exit('自检里没有名为 %s 的项 —— 固件与本脚本版本不匹配'%want)
print(m[0])"; }
btn_state() { item_state button; }

echo "═══ 1. 自动项 ═══"
show
if C "$SELF" | grep -q '"state":"fail"'; then
    echo ""
    echo "❌ 有自动项不通过，停止 —— 上面 detail 里写了是什么值不满足什么阈值"
    exit 1
fi

echo ""
echo "═══ 1b. MAC 查重 ═══"
# 固件侧**不存**全局表（它只知道自己那一块），查重只能在产线这一侧做。
# 拦的是两件事：同一块板被当成两块走完产线（返修板混进良品流），以及真的撞了 MAC
# （乐鑫 eFuse 理论上唯一，但克隆/翻新片子在二级市场是存在的）。
#
# ⚠️ 台账文件与 generate_nvs.py 的身份台账**分开**：那一份是"写了什么身份"的正本，
#    这一份是"测过哪些板"的流水。两件事的生命周期不同（返修板会重测但不重写身份）。
# ⚠️ 和身份台账一样，这个文件也**必须备份** —— 丢了就等于产线失去了查重能力，
#    而且是静默失去：脚本照跑，只是再也查不出重复。
LEDGER="$(dirname "$0")/.factory_test_log.csv"
MAC=$(C "$SELF" | python3 -c "
import json,re,sys
d=json.load(sys.stdin)
det=[i['detail'] for i in d['items'] if i['name']=='identity'][0]
m=re.search(r'mac=([0-9A-Fa-f]{12})', det)
print(m.group(1).upper() if m else '')")
SN=$(C "$SELF" | python3 -c "
import json,re,sys
d=json.load(sys.stdin)
det=[i['detail'] for i in d['items'] if i['name']=='identity'][0]
m=re.search(r'SN=(\S+)', det)
print(m.group(1) if m else '-')")

if [ -z "$MAC" ]; then
    echo "  ❌ 读不到 MAC —— identity 项的 detail 变了格式？固件与本脚本要成对更新"
    exit 1
fi
[ -f "$LEDGER" ] || echo "utc_time,mac,sn,verdict,note" > "$LEDGER"

if cut -d, -f2 "$LEDGER" | grep -qx "$MAC"; then
    if [ "$RETEST" = "1" ]; then
        echo "  🔁 $MAC 此前测过 —— 按 --retest 放行（会另记一行 note=retest）"
        NOTE=retest
    else
        echo "  ❌ MAC $MAC 在台账里已经出现过："
        grep ",$MAC," "$LEDGER" | sed 's/^/      /'
        echo "     要么这块板已经走过产线（返修复测请加 --retest），"
        echo "     要么两块板撞了 MAC —— 后者必须查清，别放行"
        exit 1
    fi
else
    echo "  ✅ MAC $MAC 未出现过（SN=$SN）"
    NOTE=new
fi

if [ "$AUTO" = "1" ]; then
    echo ""
    echo "⏭  --auto：跳过人工确认项。注意 verdict 仍是 $(verdict)，**不等于整机通过**"
    exit 0
fi

echo ""
echo "═══ 2. LED 目视（唯一判据是四颗全白）═══"
# 判据必须是白色：WS2812 线序是 GRB，红在线上是 00 FF 00，第一个非零字节之后本来
# 就没有内容 —— 而 T1H 过大的失效恰恰是"该字节之后全部作废"，单色对它天然失明。
C -X POST -H 'Content-Type: application/json' \
  -d '{"index":-1,"color":"FFFFFF","hold_s":5}' "http://$GW/api/v1/led/test" > /dev/null
echo "  四颗灯已点白 5 秒。"
read -r -p "  四颗是否**全部纯白**（不是绿/黄/偏色，末端那颗也要亮）？[y/N] " a
case "$a" in
    [yY]*) ack led_visual pass; echo "  ✅ LED 目视通过（已回写设备）";;
    *)     ack led_visual fail
           echo "  ❌ LED 目视不通过 —— 查 T0H/T1H，见 docs/hardware 的 X5"
           echo "     （已回写 fail，设备侧 verdict 也会是 fail，不会留下「没测」的假象）"
           exit 1;;
esac

# WiFi 在产线不配 WiFi 时是 manual。不处理它 = 永远 incomplete。
if item_state wifi | grep -q manual; then
    echo ""
    read -r -p "  WiFi 未连接。本产线是否**不配 WiFi**（跳过该项）？[y/N] " w
    case "$w" in
        [yY]*) ack wifi pass; echo "  ⏭  WiFi 按「产线不测」记为通过";;
        *)     ack wifi fail; echo "  ❌ WiFi 应连未连 —— 查天线/凭据"; exit 1;;
    esac
fi

# wifi_scan：产线没有 5GHz AP 环境时扫到 0 个，是正常的；但若现场确有 5GHz AP
# 却扫不到，那就是这块板的射频有问题 —— 两者只有操作员分得清。
if item_state wifi_scan | grep -q manual; then
    echo ""
    read -r -p "  扫不到 5GHz AP。本产线**周围确实没有** 5GHz AP？[y/N] " sc
    case "$sc" in
        [yY]*) ack wifi_scan pass; echo "  ⏭  wifi_scan 按「无 5GHz 环境」记为通过";;
        *)     ack wifi_scan fail; echo "  ❌ 有 5GHz AP 却扫不到 —— 查 WiFi 天线/射频"; exit 1;;
    esac
fi

# PoE：固件分不出"没用 PoE 供电"和"PoE 分压坏了"，由 --power 决定该怎么算。
if item_state poe | grep -q manual; then
    echo ""
    if [ "$POWER" = "usb" ]; then
        ack poe pass
        echo "  ⏭  PoE 未检测到，按 --power=usb 记为不适用"
    else
        ack poe fail
        echo "  ❌ 按 PoE 供电却检测不到（IO0 为低）—— 查 TMI7321 与 R96/R97 分压"
        echo "     若这台确实是 USB 供电，请加 --power=usb 重跑"
        exit 1
    fi
fi

echo ""
echo "═══ 3. 按键 ═══"
if [ "$(btn_state)" = "pass" ]; then
    echo "  ✅ 本次上电已记录到按压，跳过"
else
    echo "  请按住按钮约 5 秒后松开。"
    echo "  （2-10 秒落在保护带，不会重启、不会擦配置；红灯亮起是正常的进度提示）"
    T=$(( $(date +%s) + 120 ))
    until [ "$(btn_state)" = "pass" ]; do
        [ $(date +%s) -ge $T ] && { echo "  ❌ 120 秒内没检测到按压 —— 查 IO26 焊接与上拉"; exit 1; }
        sleep 3
    done
    echo "  ✅ 按压已记录"
fi
C "$SELF" | python3 -c "
import json,sys
b=[i for i in json.load(sys.stdin)['items'] if i['name']=='button'][0]
print('    ',b['detail'])
import re
m=re.search(r'(\d+) ms → (\w+)', b['detail'])
if m:
    ms,act=int(m.group(1)),m.group(2)
    ok = 2000 <= ms < 10000 and act=='none'
    print('     %s 时长 %dms / 动作 %s（期望 2000-10000ms 且 none）'%('✅' if ok else '❌',ms,act))
    sys.exit(0 if ok else 1)
"  || { echo "  ❌ 按压时长或分档不对"; exit 1; }

echo ""
echo "═══ 结果 ═══"
V=$(verdict)
if [ "$V" = "pass" ]; then
    # 只有**通过**才记台账。不合格的板子会返修后重测，那次才是它真正走产线的一次；
    # 每次尝试都记 = 返修板第二次必然撞查重，把一条真判据变成天天喊狼来了。
    printf '%s,%s,%s,%s,%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$MAC" "$SN" "$V" "${NOTE:-new}" \
        >> "$LEDGER"
    echo "  ✅ 整机通过（已记台账：$MAC $SN）"
else
    echo "  ⚠️  verdict=$V —— 自动项与人工项都过了，但仍有 manual 未消。逐项看："
    show
    exit 1
fi
