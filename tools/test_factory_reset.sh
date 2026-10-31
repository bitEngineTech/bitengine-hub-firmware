#!/bin/bash
# tools/test_factory_reset.sh —— 恢复出厂的**行为**断言（TC-F01 / TC-F02）
#
#   bash tools/test_factory_reset.sh <网关IP>                  # 只测 KEEP_THREAD（默认，安全）
#   bash tools/test_factory_reset.sh <网关IP> --wipe-thread    # 追测 WIPE_ALL（⚠️ 会毁掉 Thread 网络）
#
# ── 为什么单独一个脚本，不并进 test_api.sh ───────────────────────────────
#
# 这条路径是**破坏性且不可回滚**的：擦 NVS + 立刻 esp_restart，WiFi 凭据、
# 以太网静态 IP、密码全没了，设备会以 DHCP 重新出现（IP 可能变）。
# test_api.sh 的"自动备份还原配置"救不了它 —— 备份接口本身就要认证，
# 而密码已经被擦掉了。所以它必须是一个人明确知道自己在干什么才跑的脚本。
#
# ── 为什么必须有它（这才是重点）───────────────────────────────────────
#
# 2026-08-15：恢复出厂擦掉了 Thread 网络的密钥材料（psa_its 命名空间），
# Thread 网络永久失联 —— 而当时专门写的恢复出厂检查脚本 **7/7 全绿**。
# 因为那 7 项看的全是数据集：ExtPANID / 网络名 / 信道 / had_child 逐项都在，
# 唯独密钥材料没了。修复见 ff8ab98（反转擦除极性）。
#
# 教训是双份的，两条防线各守一半：
#   · 代码形状 → components/config_mgr/VERIFY.sh（OWNED_NS/THREAD_NS 的极性不变量，
#     含"OWNED_NS 不含 psa_its"这条反向断言）。它守的是"别再写错"。
#   · 端到端行为 → 本脚本。它守的是"真的按极性生效了"。
#
# ⚠️ 本脚本**证明不了密钥材料还在**，别以为它能。psa_its 从设备外面不可观测：
#    密钥没了以后网关照样能当 leader，只有真实子设备来敲门才会暴露
#    （"Failed to process UDP - Security"）。所以最后一步是 manual，
#    按 factory_api.c 的规矩：**manual 不算 pass**。

set -u
GW="${1:?用法: $0 <网关IP> [--wipe-thread]}"
WIPE=0; [ "${2:-}" = "--wipe-thread" ] && WIPE=1

AUTH=()
[ -n "${GW_PASS:-}" ] && AUTH=(-u ":$GW_PASS")
C() { curl -s -m 10 ${AUTH[@]+"${AUTH[@]}"} "$@"; }

PASS=0; FAIL=0
ok()   { echo "  ✅ $1"; PASS=$((PASS+1)); }
bad()  { echo "  ❌ $1"; FAIL=$((FAIL+1)); }
note() { echo "     $1"; }

jget() { python3 -c "
import json,sys
d=json.load(sys.stdin)
for k in '$1'.split('.'):
    if d is None: break
    d=d.get(k) if isinstance(d,dict) else None
print('' if d is None else d)
" 2>/dev/null; }

# ---------------------------------------------------------------------------
echo "═══ 0. 前置检查 ═══"

_sc=$(curl -s -m 10 ${AUTH[@]+"${AUTH[@]}"} -o /dev/null -w '%{http_code}' \
      "http://$GW/api/v1/status")
[ "$_sc" = "401" ] && { echo "❌ 需要认证。GW_PASS=<密码> bash $0 $GW"; exit 1; }
[ "$_sc" = "200" ] || { echo "❌ 设备不可达（HTTP $_sc）"; exit 1; }

# 以太网必须是通的：恢复出厂会擦掉 WiFi 凭据，只有网线能把设备带回来。
ETH_UP=$(C "http://$GW/api/v1/status" | jget "eth_link_up")
if [ "$ETH_UP" != "True" ] && [ "$ETH_UP" != "true" ]; then
    echo "❌ 以太网未连接。恢复出厂会擦掉 WiFi 凭据（nvs.net80211 + wifi 命名空间），"
    echo "   没有网线的话设备重启后就再也回不来了，只能靠 5GHz SoftAP 或拆机。"
    echo "   插好网线再跑。"
    exit 1
fi
ok "以太网在线，重启后能回来"

# ---------------------------------------------------------------------------
echo ""
echo "═══ 1. 记录 Thread 现状（这是稍后比对的基准）═══"

BEFORE=$(C "http://$GW/node/dataset/active" 2>/dev/null)
XPAN_BEFORE=$(echo "$BEFORE"   | jget "ExtPanId")
NAME_BEFORE=$(echo "$BEFORE"   | jget "NetworkName")
CHAN_BEFORE=$(echo "$BEFORE"   | jget "Channel")
STATE_BEFORE=$(C "http://$GW/node/state" 2>/dev/null | tr -d '"')

if [ -z "$XPAN_BEFORE" ]; then
    echo "❌ 读不到 Thread 活动数据集 —— 设备当前没有 Thread 网络，本测试无意义。"
    echo "   （若 thread.rest_auth 打开了，请带 GW_PASS）"
    exit 1
fi
note "ExtPanId    = $XPAN_BEFORE"
note "NetworkName = $NAME_BEFORE"
note "Channel     = $CHAN_BEFORE"
note "node/state  = $STATE_BEFORE"
ok "基准已记录"

wait_back() {   # 等设备重启回来，最多 90s
    echo "  ⏳ 等待重启回来（最多 90s）…"
    local t=$(( $(date +%s) + 90 ))
    until curl -s -m 3 -o /dev/null "http://$GW/api/v1/status"; do
        if [ "$(date +%s)" -ge "$t" ]; then
            echo "  ❌ 90 秒内没回来。恢复出厂会把静态 IP 也擦掉 —— 设备很可能换了"
            echo "     DHCP 地址。查路由器租约表或试 bitengine.local，然后手工核对。"
            return 1
        fi
        sleep 3
    done
    sleep 8   # 给 OpenThread attach 留时间
    return 0
}

# ---------------------------------------------------------------------------
echo ""
echo "═══ 2. TC-F01：KEEP_THREAD（不带 body）—— Thread 必须原样存活 ═══"
echo "  ⚠️  这会擦掉 WiFi/以太网/密码/LED/MQTT 等全部用户配置并重启。"
read -r -p "  继续？[y/N] " a
case "$a" in [yY]*) ;; *) echo "  已取消"; exit 0;; esac

C -X POST "http://$GW/api/v1/config/factory" > /dev/null 2>&1
wait_back || exit 1

AFTER=$(C "http://$GW/node/dataset/active" 2>/dev/null)
XPAN_AFTER=$(echo "$AFTER" | jget "ExtPanId")
NAME_AFTER=$(echo "$AFTER" | jget "NetworkName")
CHAN_AFTER=$(echo "$AFTER" | jget "Channel")
STATE_AFTER=$(C "http://$GW/node/state" 2>/dev/null | tr -d '"')

[ "$XPAN_AFTER" = "$XPAN_BEFORE" ] \
    && ok "ExtPanId 未变（$XPAN_AFTER）" \
    || bad "ExtPanId 变了：$XPAN_BEFORE → ${XPAN_AFTER:-（读不到）}｜极性反了，openthread 被误擦"
[ "$NAME_AFTER" = "$NAME_BEFORE" ] \
    && ok "NetworkName 未变（$NAME_AFTER）" \
    || bad "NetworkName 变了：$NAME_BEFORE → ${NAME_AFTER:-（读不到）}"
[ "$CHAN_AFTER" = "$CHAN_BEFORE" ] \
    && ok "Channel 未变（$CHAN_AFTER）" \
    || bad "Channel 变了：$CHAN_BEFORE → ${CHAN_AFTER:-（读不到）}"

case "$STATE_AFTER" in
    leader|router|child) ok "node/state = $STATE_AFTER（已重新入网）";;
    *) bad "node/state = ${STATE_AFTER:-（读不到）}，期望 leader/router/child";;
esac

# 用户配置这一半必须真的被擦掉，否则"恢复出厂"是假的
PWD_REQ=$(C "http://$GW/api/v1/auth/status" | jget "password_required")
if [ "$PWD_REQ" = "False" ] || [ "$PWD_REQ" = "false" ]; then
    ok "密码已清除（password_required=false）—— 用户配置确实被擦了"
else
    bad "password_required=$PWD_REQ，密码没被擦掉 —— 那这次根本没擦到 sys 命名空间"
fi

echo ""
echo "  🟡 manual —— 本脚本到此为止，剩下这一步只有真实子设备能证："
echo "     密钥材料（psa_its）**从设备外面不可观测**。上面四项全绿也不代表它还在 ——"
echo "     2026-08-15 那次就是数据集全对、唯独密钥没了，检查脚本 7/7 全绿。"
echo "     请确认：HA 里原有的 Matter over Thread 设备是否仍然在线且可控？"
echo "     若设备掉线且日志出现 \"Failed to process UDP - Security\"，就是密钥被擦了。"

# ---------------------------------------------------------------------------
if [ "$WIPE" = "1" ]; then
    echo ""
    echo "═══ 3. TC-F02：WIPE_ALL（wipe_thread=true）—— Thread 必须真的没了 ═══"
    echo "  ⚠️⚠️  这会**销毁当前 Thread 网络**：所有已配对的 Thread/Matter 设备"
    echo "        都要重新配网。本仓库只有一块生产板，想清楚再继续。"
    read -r -p "  确认销毁 Thread 网络？输入大写 YES 继续： " y
    if [ "$y" = "YES" ]; then
        C -X POST -H 'Content-Type: application/json' \
          -d '{"wipe_thread":true}' "http://$GW/api/v1/config/factory" > /dev/null 2>&1
        wait_back || exit 1

        W=$(C "http://$GW/node/dataset/active" 2>/dev/null)
        XPAN_W=$(echo "$W" | jget "ExtPanId")
        if [ -z "$XPAN_W" ] || [ "$XPAN_W" != "$XPAN_BEFORE" ]; then
            ok "Thread 数据集已清除或已重建（ExtPanId=${XPAN_W:-空}）"
        else
            bad "ExtPanId 仍是 $XPAN_W —— wipe_thread=true 没生效，THREAD_NS 没被擦"
        fi
    else
        echo "  ⏭  已跳过 TC-F02"
    fi
else
    echo ""
    echo "⏭  未测 WIPE_ALL。要测：加 --wipe-thread（会销毁 Thread 网络）"
fi

# ---------------------------------------------------------------------------
echo ""
echo "═══ 结果 ═══"
echo "  $PASS passed, $FAIL failed（外加 1 项 manual：子设备是否仍在线）"
echo "  ⚠️  manual 不算 pass —— 见 components/webui/factory_api.c 顶部"
[ "$FAIL" -eq 0 ] || exit 1
