#!/bin/bash
# tools/test_auth_throttle.sh —— 登录节流的行为断言（需要一块在网的板子）
#
#   GW_PASS=<正确密码> bash tools/test_auth_throttle.sh <网关IP或主机名>
#
# ⚠️ **破坏性**：会故意把设备锁住最多 ~32 秒。跑完自动等到解锁再退出，
#    但中途 Ctrl-C 的话，等 32 秒再跑别的脚本。
#
# 为什么单独一个脚本而不是塞进 test_api.sh：节流是最容易把后续断言连坐打红的
# 一层 —— 一旦锁上，test_api.sh 后面几十条全部 401，看起来像功能全坏了。
#
# ★ 本脚本的核心是第 3 条断言：**锁定期间用正确密码打，也必须被拒**。
#   只用错密码测的话，「拒绝到底」和「错了才 429、对了照样放行」返回值完全相同，
#   而后者攻击者一次都不会慢下来 —— 429 纯属装饰。这条是唯一能把两者分开的判据。

set -u
GW="${1:-bitengine.local}"
: "${GW_PASS:?需要 GW_PASS=<正确密码>}"

PASS=0; FAIL=0
ok()  { echo "  ✅ $1"; PASS=$((PASS+1)); }
bad() { echo "  ❌ $1"; echo "       期望: $2"; echo "       实际: $3"; FAIL=$((FAIL+1)); }
eq()  { [ "$2" == "$3" ] && ok "$1" || bad "$1" "$2" "$3"; }

LOGIN="http://$GW/api/v1/auth/login"
# 登录接口：回状态码
try_login() { curl -s -m 8 -o /dev/null -w '%{http_code}' -X POST "$LOGIN" \
              -H 'Content-Type: application/json' -d "{\"password\":\"$1\"}"; }
# Basic 那条路（rebinding 拿 token 会走它），打一个裸 AUTH_REQUIRED 的端点
try_basic() { curl -s -m 8 -o /dev/null -w '%{http_code}' -u ":$1" \
              "http://$GW/api/v1/ntp"; }

unlock() {   # 等到真的解锁为止，最多 40 秒
    for _ in $(seq 1 40); do
        [ "$(try_login "$GW_PASS")" == "200" ] && return 0
        sleep 1
    done
    return 1
}

echo "── 前置：确认密码是对的、且当前没被锁 ──"
unlock || { echo "  ⚠️ 起手就登不进去，密码不对或设备异常，中止"; exit 2; }
ok "正确密码可登录（起始状态干净）"

echo ""
echo "── 1. 阈值以下不锁 ──"
for i in 1 2 3 4; do
    c=$(try_login "definitely-wrong-$i")
    [ "$c" == "401" ] || { bad "第 $i 次错密码应为 401" "401" "$c"; }
done
[ $FAIL -eq 0 ] && ok "连错 4 次仍是 401（没有提前锁）"
eq "第 4 次之后正确密码仍能登录" "200" "$(try_login "$GW_PASS")"

echo ""
echo "── 2. 第 5 次触发锁定 ──"
# 上一步成功登录已经把计数清零，所以这里要重新连错 5 次
for i in 1 2 3 4 5; do try_login "definitely-wrong-$i" > /dev/null; done
c=$(try_login "definitely-wrong-6")
eq "锁定后错密码回 429 而不是 401" "429" "$c"

echo ""
echo "── 3. ★ 锁定期间正确密码也必须被拒 ──"
echo "     （唯一能把「拒绝到底」和「错了才 429」分开的判据）"
eq "锁定期间正确密码 → 429" "429" "$(try_login "$GW_PASS")"
# Basic 那条路没有 429 语义，check_basic_auth 直接返回 false ⇒ 401
eq "锁定期间 Basic 正确密码 → 401" "401" "$(try_basic "$GW_PASS")"

echo ""
echo "── 4. 退避真的在变长 ──"
# 锁定期间的请求不递增计数（auth_throttled 早于 auth_note），所以要等窗口过去
# 再失败一次，计数才涨到 6 ⇒ 窗口 2<<1 = 4s。
sleep 3
t0=$(date +%s)
try_login "definitely-wrong-x" > /dev/null      # 第 6 次失败 → 锁 4s
for _ in $(seq 1 12); do
    [ "$(try_login "$GW_PASS")" == "200" ] && break
    sleep 1
done
t1=$(date +%s); dt=$((t1 - t0))
if [ "$dt" -ge 3 ]; then ok "第 6 次失败后的窗口 ${dt}s ≥ 3s（首个窗口是 2s，确实在长）"
else bad "退避没有增长" ">=3s" "${dt}s"; fi

echo ""
echo "── 5. 必须能恢复（否则这是个永久锁死机制）──"
unlock && ok "等待后正确密码恢复可用" || bad "始终登不进去" "200" "超时"
eq "恢复后连续两次正确密码都通" "200" "$(try_login "$GW_PASS")"

echo ""
echo "── 收尾：确保设备处于未锁状态 ──"
unlock && echo "  ✅ 已解锁" || echo "  ⚠️ 仍处于锁定，等 40 秒再跑别的脚本"

echo ""
echo "==============================="
echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
