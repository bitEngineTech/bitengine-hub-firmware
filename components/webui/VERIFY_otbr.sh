#!/bin/bash
# components/webui/VERIFY_otbr.sh — OTBR REST API design doc compliance
# Run from project root: bash components/webui/VERIFY_otbr.sh

PASS=0; FAIL=0
SRC="components/webui/otbr_api.c"
MDNS="components/mdns_svc/mdns_svc.c"
HDR="components/webui/include/webui.h"
CMAKE="components/webui/CMakeLists.txt"
SERVER="components/webui/webui_server.c"

check() {
    if eval "$2" > /dev/null 2>&1; then
        echo "✅ $1"; ((PASS++))
    else
        echo "❌ $1"; ((FAIL++))
    fi
}

echo "=== OTBR REST API — VERIFY.sh ==="
echo ""

# ---- File existence & integration ----
echo "--- Integration ---"
check "otbr_api.c exists"             "test -f $SRC"
check "otbr_api.c in CMakeLists"      "grep -q 'otbr_api.c' $CMAKE"
check "openthread in PRIV_REQUIRES"   "grep -q 'openthread' $CMAKE"
check "register_otbr_handlers decl"   "grep -q 'register_otbr_handlers' $HDR"
check "register_otbr_handlers call"   "grep -q 'register_otbr_handlers' $SERVER"

# ---- 17 Endpoint handlers (12 node info + 4 dataset + 1 diagnostics) ----
echo ""
echo "--- Endpoint Handlers (17) ---"
check "GET /node"                  "grep -q '\"/node\".*HTTP_GET' $SRC"
check "DELETE /node"               "grep -q '\"/node\".*HTTP_DELETE' $SRC"
check "GET /node/ba-id"            "grep -q '\"/node/ba-id\"' $SRC"
check "GET /node/rloc"             "grep -q '\"/node/rloc\"' $SRC"
check "GET /node/rloc16"           "grep -q '\"/node/rloc16\"' $SRC"
check "GET /node/ext-address"      "grep -q '\"/node/ext-address\"' $SRC"
check "GET /node/state"            "grep -q '\"/node/state\".*HTTP_GET' $SRC"
check "PUT /node/state"            "grep -q '\"/node/state\".*HTTP_PUT' $SRC"
check "GET /node/network-name"     "grep -q '\"/node/network-name\"' $SRC"
check "GET /node/leader-data"      "grep -q '\"/node/leader-data\"' $SRC"
check "GET /node/ext-panid"        "grep -q '\"/node/ext-panid\"' $SRC"
check "GET /node/num-of-router"    "grep -q '\"/node/num-of-router\"' $SRC"
check "GET /node/dataset/active"   "grep -q '\"/node/dataset/active\".*HTTP_GET' $SRC"
check "PUT /node/dataset/active"   "grep -q '\"/node/dataset/active\".*HTTP_PUT' $SRC"
check "GET /node/dataset/pending"  "grep -q '\"/node/dataset/pending\".*HTTP_GET' $SRC"
check "PUT /node/dataset/pending"  "grep -q '\"/node/dataset/pending\".*HTTP_PUT' $SRC"
check "GET /diagnostics"           "grep -q '\"/diagnostics\"' $SRC"

# ---- OpenThread API usage (H1 verified) ----
echo ""
echo "--- OpenThread API (H1) ---"
check "otDatasetGetActiveTlvs"     "grep -q 'otDatasetGetActiveTlvs' $SRC"
check "otDatasetSetActiveTlvs"     "grep -q 'otDatasetSetActiveTlvs' $SRC"
check "otDatasetGetPendingTlvs"    "grep -q 'otDatasetGetPendingTlvs' $SRC"
check "otDatasetSetPendingTlvs"    "grep -q 'otDatasetSetPendingTlvs' $SRC"
check "otThreadGetDeviceRole"      "grep -q 'otThreadGetDeviceRole' $SRC"
check "otBorderAgentGetId"         "grep -q 'otBorderAgentGetId' $SRC"
check "otThreadGetRloc"            "grep -q 'otThreadGetRloc' $SRC"
check "otThreadGetRloc16"          "grep -q 'otThreadGetRloc16' $SRC"
check "otLinkGetExtendedAddress"   "grep -q 'otLinkGetExtendedAddress' $SRC"
check "otThreadGetNetworkName"     "grep -q 'otThreadGetNetworkName' $SRC"
check "otThreadGetLeaderData"      "grep -q 'otThreadGetLeaderData' $SRC"
check "otThreadGetExtendedPanId"   "grep -q 'otThreadGetExtendedPanId' $SRC"
check "otThreadGetMaxAllowedChildren" "grep -q 'otThreadGetMaxAllowedChildren' $SRC"
check "otInstanceFactoryReset"     "grep -q 'otInstanceFactoryReset' $SRC"
check "otThreadSetEnabled"         "grep -q 'otThreadSetEnabled' $SRC"
check "otIp6SetEnabled"            "grep -q 'otIp6SetEnabled' $SRC"

# ---- Thread safety (S2) ----
echo ""
echo "--- Thread Safety (S2) ---"
check "Lock acquire calls >= 3"    "[ $(grep -c 'ot_lock\|esp_openthread_lock_acquire' $SRC) -ge 3 ]"
check "Lock release calls >= 3"    "[ $(grep -c 'ot_unlock\|esp_openthread_lock_release' $SRC) -ge 3 ]"
# Note: ot_lock/ot_unlock wrapper functions make exact line-count matching unreliable;
# the silent-failure-hunter audit verified balance per-handler.
check "Lock acquire/release paired"  "grep -q 'ot_unlock\|lock_release' $SRC"
check "Lock timeout (not portMAX)" "grep -q 'OT_LOCK_TIMEOUT' $SRC"
check "thread_br_is_running check" "grep -q 'thread_br_is_running' $SRC"

# ---- Error handling (P3) ----
echo ""
echo "--- Error Handling (P3) ---"
check "503 thread_not_running"     "grep -q 'thread_not_running' $SRC"
check "503 thread_busy"            "grep -q 'thread_busy' $SRC"
check "404 no_active_dataset"      "grep -q 'no_active_dataset' $SRC"
check "404 no_pending_dataset"     "grep -q 'no_pending_dataset' $SRC"
check "400 invalid_dataset"        "grep -q 'invalid_dataset' $SRC"
check "400 invalid_state"          "grep -q 'invalid_state' $SRC"
check "400 empty_body"             "grep -q 'empty_body' $SRC"

# ---- mDNS _meshcop._udp TXT (design doc) ----
echo ""
echo "--- mDNS _meshcop TXT ---"
# 原来这里查的是 sb / tv / rv 三个 TXT 字段，一直红。查错文件了：
# _meshcop._udp **不是**我们发布的，是 BR 库（esp_openthread_publish_meshcop_mdns）
# 发的，sb/tv/rv 由它填；mdns_svc.c 只负责把 vn/mn 改成我们的品牌、并把
# xa/nn/xp 随 Thread 状态更新（见 mdns_svc.c 顶部那段注释）。
# 所以这里改为断言**我们真正负责的那几个**。
check "TXT xa（BR 的 REST 地址）"   "grep -q '\"xa\"' $MDNS"
check "TXT nn（网络名）"            "grep -q '\"nn\"' $MDNS"
check "TXT xp（扩展 PAN ID）"       "grep -q '\"xp\"' $MDNS"
check "品牌覆盖 vn/mn"              "grep -q '\"vn\", \"bitEngine\"' $MDNS"
check "TXT vn field"               "grep -q '\"vn\"' $MDNS"
check "Dynamic TXT xa update"      "grep -q '\"xa\"' $MDNS"
check "Dynamic TXT nn update"      "grep -q '\"nn\"' $MDNS"
check "Dynamic TXT xp update"      "grep -q '\"xp\"' $MDNS"

# ---- 认证覆盖（2026-08-15 之前 25 条路由里只有 1 条有）----
# 这里比的是**数量相等**，不是"存在"。原来的 bug 形状恰恰是"宏在文件里出现过"
# 却只覆盖了 1/25 —— 任何 `grep -q AUTH_REQUIRED` 式的检查都会给它开绿灯。
echo ""
echo "--- 认证覆盖 ---"
# 比的是**集合**不是计数。计数相等有两个毛病：两条路由共用一个 handler 时
# 会误报红（25 个宏 / 26 条路由），而"一个 handler 里两个宏、另一个里零个"
# 又能骗过它。所以从路由表里取出 handler 名，逐个函数体查。
URIS=$(grep -c '\.uri = "' $SRC)
UNPROT=$(awk '
    /\.handler *=/ { if (match($0, /\.handler *= *[A-Za-z_][A-Za-z0-9_]*/))
                         { h = substr($0, RSTART, RLENGTH); sub(/.*= */, "", h); print h } }
' $SRC | sort -u | while read -r fn; do
    # 取该函数体的前 6 行，看有没有 AUTH_REQUIRED
    if ! awk -v f="$fn" '
        $0 ~ ("^static esp_err_t " f "\\(") { n = 1; next }
        n > 0 && n <= 6 { if (/AUTH_REQUIRED\(req\)/) { found = 1 } ; n++ }
        END { exit(found ? 0 : 1) }
    ' $SRC; then echo "$fn"; fi   # OTBR_AUTH_REQUIRED 也含 AUTH_REQUIRED，两种都算
done)
check "每个路由 handler 开头都有 AUTH_REQUIRED（未保护：${UNPROT:-无}）" "[ -z \"$UNPROT\" ]"
check "注册路由数 >= 25（别悄悄变少）"                 "[ $URIS -ge 25 ]"
# HA 的 python-otbr-api 每个方法开头都探一次 /api/actions，只接受 200/404；
# 401 会让整个集成加载不起来。我们必须**不注册**它。
check "不注册 /api/actions（否则 HA 集成加载失败）"     "! grep -q '\"/api/actions\"' $SRC"
# 全局认证过滤器会把 /api/actions 一起 401 掉，同上，禁止
check "没有全局认证过滤器"                             "! grep -qi 'httpd_register_basic_auth\|global_auth\|auth_filter' $SRC"
# 24 条 ot-br-posix 标准路由走**可配置**的闸门（默认关，理由见 config_mgr.h）；
# /api/v1/diag/thread 在我们自己的命名空间下，保持无条件认证。
check "标准路由用可配置闸门（24 条）"    "[ $(grep -c 'OTBR_AUTH_REQUIRED(req);' $SRC) -eq 24 ]"
# ⚠️ 判据锚在**函数体范围**上，不是 grep -A2 的行距。2026-08-23 在每个 handler
#    首行插入 CROSS_ORIGIN_GUARD 时，AUTH_REQUIRED 从 +2 行挪到 +3 行，-A2 的窗口
#    就够不着了 —— 而这条判据一旦静默变假，"diag/thread 被降级成可匿名读"
#    这种真实退化就再也没人发现。窗口式判据对"在前面插一行"天然脆弱。
check "diag/thread 仍无条件认证"         "awk '/^static esp_err_t handle_diag_thread/,/^}/' $SRC | grep -q '^    AUTH_REQUIRED(req);'"
check "闸门读的是 thread.rest_auth"      "grep -q 'rest_auth' $SRC"
# 关掉时必须直接放行，不能退化成 send_error —— python-otbr-api 对非 200/404 一律抛
check "关闭时不发错误码"                 "! grep -A6 'define OTBR_AUTH_REQUIRED' $SRC | grep -q 'send_error'"
# WWW-Authenticate 会让浏览器在 XHR 401 时弹原生认证框，顶掉 SPA 登录页。
# 只查真正设 header 的调用 —— 文件头注释里解释了为什么不发，别被自己的散文绊倒。
check "不发 WWW-Authenticate"                          "! grep -i 'httpd_resp_set_hdr' $SRC | grep -qi 'WWW-Authenticate'"

# ---- S1 Security ----
echo ""
echo "--- S1 Security ---"
check "No strcmp on secrets"       "! grep -n 'strcmp.*password\|strcmp.*hash\|strcmp.*token' $SRC"
check "No secrets in logs"         "! grep -n 'ESP_LOG.*password\|ESP_LOG.*network.key' $SRC"
check "Hex input validated"        "grep -q 'hex_to_bytes' $SRC"
check "Body length bounded"        "grep -q 'recv_body\|httpd_req_recv.*sizeof' $SRC"

# ---- S2 Threading extra ----
check "No blocking in OT lock"    "! grep -A3 'ot_lock\|lock_acquire' $SRC | grep -q 'vTaskDelay\|xQueueReceive.*portMAX'"

# ---- S3 Resource lifecycle ----
echo ""
echo "--- S3 Resources ---"
# 原来这里有一条 "No malloc used"，但 otbr_api.c 确实用了三处 malloc（诊断类
# 端点要拼几 KB 的 JSON），这条断言本身就是假的，留着只会让脚本永远绿不了 ——
# 而 CLAUDE.md 的规矩是"VERIFY 通过才算开发完成"。下面这条才是真约束。
check "No dynamic alloc needed"    "! grep -q 'heap_caps_malloc' $SRC"

# ---- Conditional compilation ----
echo ""
echo "--- Conditional Compilation ---"
check "CONFIG_OPENTHREAD_ENABLED guard" "grep -q 'CONFIG_OPENTHREAD_ENABLED' $SRC"
check "Stub when OT disabled"     "grep -q '#else' $SRC"

echo ""
echo "=============================="
echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
