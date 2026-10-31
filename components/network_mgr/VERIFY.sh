#!/bin/bash
# components/network_mgr/VERIFY.sh — 自动验证设计文档合规性
# 对应 docs/03_network_mgr_design_v1.md
#
# 补写说明：本模块开发在 P1「先写 VERIFY.sh」规范确立之前，所以这份是事后补的。
# 补写的动机很具体——2026-08-08 那轮换 SDK 回归在这个模块里测出了三个真缺陷
# （见 docs/test_cases_factory_provisioning.md），全部集中在以太网路径上，
# 因为长期只在 WiFi 上开发验证。下面凡标注「回归」的检查都对应一个实际踩过的坑。
#
# 编译通过后运行：bash components/network_mgr/VERIFY.sh
cd "$(dirname "$0")"
PASS=0; FAIL=0

check() {
    if eval "$2" > /dev/null 2>&1; then
        echo "✅ $1"; ((PASS++))
    else
        echo "❌ $1"; ((FAIL++))
    fi
}

# FN <函数定义行开头的正则>：取该函数的整个函数体（到第一个顶格的 }）。有前置声明的函数，正则要锚到定义行
# （以 ")$" 结尾），否则先匹配到声明那一行、取出来的不是函数体（2026-10-07 T9.37 踩过）
FN() { awk -v pat="$1" '$0 ~ pat {f=1} f{print} f&&/^}/{exit}' network_mgr.c; }

echo "=== Network Manager Module Verification ==="

echo ""
echo "--- Files ---"
check "network_mgr.h exists"     "test -f include/network_mgr.h"
check "network_mgr.c exists"     "test -f network_mgr.c"
check "wifi_mgr.c/.h exist"      "test -f wifi_mgr.c && test -f wifi_mgr.h"
check "eth_mgr.c/.h exist"       "test -f eth_mgr.c && test -f eth_mgr.h"
check "net_events.c exists"      "test -f net_events.c"

echo ""
echo "--- Public API (11 functions) ---"
for f in network_mgr_init network_mgr_get_state \
         network_mgr_get_status network_mgr_set_wifi network_mgr_scan_wifi \
         network_mgr_set_eth_ip network_mgr_start_softap network_mgr_stop_softap \
         network_mgr_softap_active network_mgr_sta_connected network_state_machine; do
    check "$f declared+defined" \
          "grep -q '$f' include/network_mgr.h && grep -q '^[a-z].*$f' *.c"
done

echo ""
echo "--- State machine: all 9 states handled ---"
# 9 个状态每个都要有 case 分支；state_name() 里也要有对应字符串
check "9 states in enum"          "[ \$(grep -c 'NET_STATE_[A-Z_]*,' include/network_mgr.h) -ge 9 ]"
check "9 case arms in switch"     "[ \$(grep -c 'case NET_STATE_' network_mgr.c) -ge 9 ]"
check "state_name covers all"     "[ \$(grep -A12 'static const char \\*state_name' network_mgr.c | grep -c 'NET_STATE_') -ge 9 ]"

echo ""
echo "--- Internal events: all 9 consumed ---"
for e in WIFI_CONNECTED WIFI_DISCONNECTED WIFI_GOT_IP WIFI_LOST_IP \
         WIFI_RECONNECT_EXHAUSTED ETH_LINK_UP ETH_LINK_DOWN ETH_GOT_IP \
         WIFI_CREDENTIALS_SET; do
    check "NET_EVT_$e handled" "grep -q 'NET_EVT_$e' network_mgr.c"
done

echo ""
echo "--- Outbound events: declared == posted ---"
# CLAUDE.md 第 5 条：每个 event enum 值都要有 esp_event_post。
# 已知缺口：NETWORK_EVENT_WIFI_CREDENTIALS_SET 全库只出现在自己的枚举声明里，
# 从未 post、也无人订阅（内部用的是 NET_EVT_WIFI_CREDENTIALS_SET，两者不同）。
# 这条 check 现在就是红的，是有意留红——要么补 post，要么把枚举值删掉。
for e in ONLINE OFFLINE FAILOVER RECOVERED STATE_CHANGED SOFTAP_STARTED SOFTAP_STOPPED \
         WIFI_CREDENTIALS_SET; do
    check "NETWORK_EVENT_$e posted" \
          "grep -rq 'NETWORK_EVENT_$e' *.c"
done

echo ""
echo "--- network_status_t: every field assigned ---"
for f in state active_interface eth_link_up eth_has_ip eth_ip eth_gateway \
         eth_netmask wifi_connected wifi_has_ip wifi_ip wifi_rssi wifi_ssid \
         wifi_channel softap_active softap_clients softap_ssid; do
    check "out->$f assigned" "grep -q 'out->$f' network_mgr.c"
done

echo ""
echo "--- 关键设计决策（docs/03 二、）---"
# T2.16（2026-09-13）：FAILOVER 只在退到 WiFi 时发；以太网接管只有"退过"才算 RECOVERED，
# 开机时的接管什么都不发（此前每次开机必发一条假 FAILOVER）。
check "FAILOVER 只在切到 WiFi 时发" \
    "awk '/^static void set_default_route/,/^}/' network_mgr.c | grep -B3 'NETWORK_EVENT_FAILOVER' | grep -q 'iface == NET_IF_WIFI'"
# ⚠️ 判据必须落在守卫那一行本身（else if (s_failed_over)），不能是"附近有这个词"——
#    第一版用 grep -B3，被下面那句 s_failed_over = false 绊成恒绿，变异当场没红。
check "RECOVERED 只在退过之后发"   \
    "awk '/^static void set_default_route/,/^}/' network_mgr.c | grep -q 'else if (s_failed_over) {'"
check "退过的标记在 FAILOVER 处置位、RECOVERED 处清零" \
    "[ \$(awk '/^static void set_default_route/,/^}/' network_mgr.c | grep -c 's_failed_over = true') -eq 1 ] && [ \$(awk '/^static void set_default_route/,/^}/' network_mgr.c | grep -c 's_failed_over = false') -eq 1 ]"
# 决策 1：failover 靠切默认路由，不是停接口
check "Failover switches route"   "grep -q 'esp_netif_set_default_netif' network_mgr.c"
check "Failover never stops iface" "! grep -q 'esp_wifi_stop()' network_mgr.c"
# T2.17（2026-09-13）：PMF 默认开。关 PMF 只能躲在 Kconfig 逃生口后面，且默认 n。
check "关 PMF 只在 CONFIG_BITENGINE_WIFI_DISABLE_PMF 下" \
    "grep -B1 'esp_wifi_disable_pmf_config(WIFI_IF_STA)' wifi_mgr.c | grep -q '#if CONFIG_BITENGINE_WIFI_DISABLE_PMF'"
check "逃生口 Kconfig default n" \
    "grep -A3 'config BITENGINE_WIFI_DISABLE_PMF' ../../main/Kconfig.projbuild | grep -q 'default n'"
# ⚠️ 加了 Kconfig 项之后 idf.py build **不一定**重跑 kconfig（2026-09-13 就没有：sdkconfig 里没这一行，
#    逃生口等于不存在，而固件行为一样所以毫无症状）。必须 idf.py reconfigure 一次，这里钉住 sdkconfig 认得它。
check "sdkconfig 认得逃生口（没有就 idf.py reconfigure）" "grep -q 'CONFIG_BITENGINE_WIFI_DISABLE_PMF' ../../sdkconfig"
# 5GHz only 是全局硬约束（CLAUDE.md 关键设计决策 4）：绝不开 2.4GHz
check "5G_ONLY band mode"         "grep -q 'WIFI_BAND_MODE_5G_ONLY' wifi_mgr.c"
check "No 2.4G band mode"         "! grep -q 'WIFI_BAND_MODE_2G' wifi_mgr.c"
# 2026-10-10：光锁 band mode 不够 —— 网页触发的扫描把 2.4G 也扫了，同时进行的重连连上了同名路由器的 2.4G ch11，
# 随后热点也被拖到 2.4G。四道防线的结构由 check_band_lock.py 钉住（只看代码，不看注释）。
check "5 GHz 锁四道防线（check_band_lock.py）" "python3 check_band_lock.py ."
# 决策 2：SoftAP 用 5GHz ch36
check "SoftAP on channel 36"      "grep -q '.channel *= *36' wifi_mgr.c"
# 决策：SSID 用 MAC 末两字节大写十六进制
check "SoftAP SSID from MAC"      "grep -q 'bitEngine-Hub-%02X%02X' wifi_mgr.c"
# 状态转换表：重试 5 次失败 → SOFTAP_ONLY
# 原有一条 check "Reconnect max retry = 5" 把次数写死，2026-08-21 调成 7 后
# 变成假红。已删 —— 上面那条按退避序列算覆盖时长的判据严格更好：
# 它对 BASE/MAX/次数 任何一个的改动都成立，且表达的是**意图**（要盖住
# 路由器重启的 30-90 秒）而不是某个具体数字。
check "Max retry actually used"   "grep -q 'WIFI_RECONNECT_MAX_RETRY' wifi_mgr.c"

echo ""
echo "--- 回归：换 SDK 那轮测出的三个缺陷 ---"
# 回归 1（TC-02）：共存仲裁器激活（Thread/802.15.4 在跑）时，驱动会拒绝自定义
# scan_time，扫描静默返回 0 个 AP。必须留空用驱动默认值。
check "Scan uses default scan_time" \
      "! grep -q 'scan_time.active' wifi_mgr.c"
# 回归 2：backbone netif 只找 WIFI_STA_DEF，有线部署下边界路由起不来。
# 选择顺序必须与默认路由策略一致（ETH 优先），且逐个检查接口是否真的 up。
check "Backbone considers ETH"    "grep -q 'ETH_DEF' ../thread_br/thread_br.c"
check "Backbone checks netif_up"  "grep -q 'esp_netif_is_netif_up' ../thread_br/thread_br.c"
# 回归 3：ETH 侧必须建 IPv6 链路本地地址，否则 OT 发不出 RS、不发布 fc00::/7。
# 时机关键：必须在 GOT_IP，放在 ETHERNET_EVENT_CONNECTED 会被 glue 抹掉。
check "ETH creates IPv6 link-local" \
      "grep -q 'esp_netif_create_ip6_linklocal' net_events.c"
# 注意匹配的是 "case XXX:" 而不是裸词——上面那段注释里就写着
# ETHERNET_EVENT_CONNECTED，用裸词会被自己的注释骗过去（第一版就栽在这）。
check "ETH IPv6 LL at GOT_IP not link-up" \
      "awk '/case IP_EVENT_ETH_GOT_IP:/{g=1} /case ETHERNET_EVENT_CONNECTED:/{g=0} g && /esp_netif_create_ip6_linklocal\\(eth_nif\\)/{f=1} END{exit !f}' net_events.c"

echo ""
echo "--- S1 security auto-checks ---"
check "No strcmp on secrets"      "! grep -rn 'strcmp.*password\|strcmp.*psk' *.c"
check "No password in logs"       "! grep -rn 'ESP_LOG.*password' *.c"
# set_wifi 是对外接口，必须验长度：SSID 1..32、密码 8..64 或空（WPA2 规定）
check "set_wifi validates SSID"   "grep -A8 'network_mgr_set_wifi' network_mgr.c | grep -q 'strlen'"

echo ""
echo "--- S2 threading auto-checks ---"
# 周期性快照路径（ws_push 每 2s、health 每 10s）上不得有阻塞 WiFi ioctl：
# 未关联时每次调用都让驱动打一条 "Haven't to connect"，实测 120s 刷 120 条。
# 判定改用事件维护的标志位。
check "is_connected reads flag not driver" \
      "grep -A6 'bool wifi_mgr_is_connected' wifi_mgr.c | grep -qv 'esp_wifi_sta_get_ap_info'"
check "sta_connected flag is assigned" \
      "grep -q 'wifi_mgr_set_sta_connected' net_events.c"
# softap_active 给 thread_br 在事件回调里查，必须是非阻塞只读标志
check "softap_active is non-blocking" \
      "grep -A5 'bool wifi_mgr_softap_active' wifi_mgr.c | grep -qv 'esp_wifi_'"

echo ""
echo "--- S3 resource auto-checks ---"
# 这里的定时器是模块生命周期内常驻的（重连退避、ETH DHCP 超时），不成对释放是
# 设计如此，所以不查 create/delete 配平——那个判据对本模块没有意义。
# 真正有价值的约束是：创建失败必须被发现，否则后面 esp_timer_start 会打在空句柄上。
check "Every esp_timer_create checked" \
      "[ \$(grep -h 'esp_timer_create(' *.c | wc -l) -eq \$(grep -hE 'ESP_RETURN_ON_ERROR\(esp_timer_create\(|if \(esp_timer_create\(.*\) != ESP_OK\)' *.c | wc -l) ]"
# ↑ 两种都算检查了：init 里用 ESP_RETURN_ON_ERROR（建不成 init 就失败）；T9.37 的 svc_id_init 建不成只是本次开机不用身份机制（2026-10-07）
# 代码用的是 instance 版 API，别 grep 成 esp_event_handler_register
check "Event handlers registered" \
      "grep -q 'esp_event_handler_instance_register' net_events.c"
check "eth_mgr_get_netif NULL-guarded at call sites" \
      "! grep -n 'esp_netif_create_ip6_linklocal(eth_mgr_get_netif())' net_events.c"

echo ""
echo "==============================="
echo ""
echo "--- DM9051 引脚只能有一份真源（2026-08-17）---"
# devkit_pins.h 的 ETH_SPI_* 8 个宏里 7 个零引用，真正驱动 DM9051 的是
# ethernet_init 读的 CONFIG_ETHERNET_SPI_*。改 devkit_pins.h 不改变任何行为 ——
# 这正是 RTS/CTS 写反那个教训的同款结构（错值长期无症状）。静态断言把两边绑死。
check "有编译期一致性断言"      "grep -q '_Static_assert(ETH_SPI_CLK_PIN' eth_mgr.c"
check "八个引脚全覆盖"          "[ \$(grep -c '_Static_assert(ETH_SPI_' eth_mgr.c) -eq 8 ]"
# GCC 把中文字符串按八进制转义打，中文断言消息在编译错误里完全没法读
check "断言消息是 ASCII"        "! awk '/_Static_assert\\(ETH_SPI_/,/;/' eth_mgr.c | grep -qP '[^\\x00-\\x7F]'"

# ---- esp_wifi_connect() 的失败必须重新排程（2026-08-21）----
# 它返错时**不会产生 STA_DISCONNECTED 事件**（压根没发起连接），而整条重试链
# 全靠那个事件驱动。只打日志就返回 = 链条当场断死，设备永远停在
# WIFI_RECONNECTING，连 SoftAP 兜底都轮不到。
# 现场表现：路由器重启一次，WiFi 上联再也回不来。
# 返错是真实场景：驱动未就绪返 NOT_STARTED，正在扫描时调用返 ESP_ERR_WIFI_STATE
# （esp_wifi.h 的 attention 3）。
#
# ⚠️ 判据必须锚在**带赋值的调用行**上，不能数 'esp_wifi_connect()' 出现次数 ——
# 注释里也会写这个名字，首版就是这么算出 8 个调用点的（实际只有 3 个）。
# 2026-10-10 起两处写成 "if (err == ESP_OK) err = esp_wifi_connect();"（先上 5 GHz 锁），所以锚 'err = esp_wifi_connect();'
CONN=$(grep -c 'err = esp_wifi_connect();' wifi_mgr.c)
REARM=$(grep -A14 'err = esp_wifi_connect();' wifi_mgr.c | grep -c 'wifi_mgr_schedule_')
check "每个 esp_wifi_connect 失败都重排" "[ $REARM -ge $CONN ] && [ $CONN -eq 3 ]"

# ---- 有凭据时绝不开 SoftAP（2026-08-21 决策）----
# 以前重连耗尽时无条件开一个 WIFI_AUTH_OPEN 的开放热点：用户没要求配网，
# 只是路由器暂时不在，却凭空多出一个谁都能连的 AP。且 SoftAP 与 Thread BR 互斥。
# ⚠️ 锚点必须用 'else if (event == ...)'，不能用裸关键字 —— 后者会先命中
# event_name() 里那个 case，awk 范围从错误的地方开始，判据在好坏代码上都红。
check "耗尽不再直接开 SoftAP" \
      "! grep -A12 'else if (event == NET_EVT_WIFI_RECONNECT_EXHAUSTED)' network_mgr.c | grep -q 'wifi_mgr_start_softap'"
check "耗尽走 no_network_fallback" \
      "grep -A12 'else if (event == NET_EVT_WIFI_RECONNECT_EXHAUSTED)' network_mgr.c | grep -q 's_state = no_network_fallback'"
# 唯一允许开 SoftAP 的地方必须带无凭据判断
check "开 SoftAP 必须先判无凭据" \
      "grep -B2 'wifi_mgr_start_softap() == ESP_OK' network_mgr.c | grep -q '!wifi_mgr_has_credentials()'"
# NO_NETWORK 必须续重试链，否则有凭据的设备落进去就再也不重连了
check "NO_NETWORK 处理 WIFI_DISCONNECTED" \
      "awk '/case NET_STATE_NO_NETWORK:/,/break;/' network_mgr.c | grep -q 'schedule_background_retry'"
# 2026-10-21：开机插着网线、网口 DHCP 回来之前 WiFi 断一次，以前重试链就断了（ETH_CONNECTING 不处理）。
# 范围锚在行尾的冒号上：state_name() 里同名 case 后面跟着 return，不会被误当成状态机那段。
check "ETH_CONNECTING 处理 WIFI_DISCONNECTED 并续重试链" \
      "awk '/^    case NET_STATE_ETH_CONNECTING:\$/,/^    case NET_STATE_ETH_ONLINE:\$/' network_mgr.c | grep -A14 'event == NET_EVT_WIFI_DISCONNECTED' | grep -q 'wifi_mgr_schedule_reconnect()'"

# ---- 快速重连必须覆盖路由器重启（2026-08-21）----
# 路由器重启要 30-90 秒。快速阶段的累计时长必须显著超过它，否则每次路由器
# 重启都会掉进慢速路径，用户体感是"WiFi 掉了要等好久"。
# 这里直接算退避序列的累计秒数，而不是硬编码次数 —— 改 BASE/MAX/次数 任何
# 一个都会被这条判据接住。
COVER=$(python3 -c "
import re
h=open('include/network_mgr.h').read()
g=lambda k: int(re.search(r'#define %s\s+(\d+)'%k,h).group(1))
n,b,m=g('WIFI_RECONNECT_MAX_RETRY'),g('WIFI_RECONNECT_BASE_MS'),g('WIFI_RECONNECT_MAX_MS')
print(sum(min(b*(1<<c),m) for c in range(n))//1000)")
check "快速重连覆盖 >=120s（实为 ${COVER}s）" "[ $COVER -ge 120 ]"
# 上限别调太大：延时是 BASE << count，count 到 22 时 uint32_t 溢出
check "重连次数不超过 20"  "[ $(grep -oE 'WIFI_RECONNECT_MAX_RETRY\s+[0-9]+' include/network_mgr.h | grep -oE '[0-9]+$') -le 20 ]"

# 两处排慢速重试的地方必须同口径：热点在就得用 PROVISIONING 间隔，否则
# "用户正在配网、密码填错"这条路径会每分钟把配网的人踢一次（射频离开 ch36）。
# 两处排慢速重试必须**逐字同口径**：先判无凭据早返回，再按热点在不在选间隔。
# ⚠️ 用 -A1：表达式跨行，只在同一行里找会漏掉。
check "慢速间隔两处同口径"       "[ $(grep -A1 's_softap_active ? WIFI_PROVISIONING_RETRY_MS' wifi_mgr.c | grep -c 'WIFI_BACKGROUND_RETRY_MS') -ge 2 ]"
# 没凭据 = 没有连接目标，排重试是去连一个不存在的 AP，两处都必须早返回
check "无凭据两处都不排重试"     "[ $(grep -c 'if (!wifi_mgr_has_credentials()) return;' wifi_mgr.c) -ge 2 ]"

# ---- 地址变化：db7cab8 的「三件套」只剩一件（2026-08-22 结算）----
# 那三件里有两件是错的，都在 2026-08-21/22 现场证实并撤回。三条都保留在这里，
# 因为它们的**动机**下一个人还会重新想到 —— 留着记录比删掉安全。
#   ① DHCP 主机名 —— **保留**。路由器把 bitengine 注册进本地 DNS，容器里解析得了，
#      而 mDNS 的 .local 在 Docker 里解析不了（实测确认）。
#   ② ~~地址一变就断开 EZSP 客户端~~ —— **已撤回（1f7e01b 之后）**。判据本身就错：
#      它跟踪「默认路由指向哪个 IP」，而连接存活取决于「这条连接自己的本地地址
#      还在不在」。监听 socket 绑 0.0.0.0，双上行时两个地址都在服务。
#      实测一次拔插踢**两次**，两次都是自伤。地址真消失时协议栈自己会逐 pcb
#      tcp_abort()，比我们准。详见 network_mgr.c 状态机末尾那段说明。
#   ③ ~~接口下线归还 DHCP 租约~~ —— **已撤回（1f7e01b）**，停机级缺陷。
#      为什么不能做见下方「禁止裸 dhcpc_stop」。
# ---- 状态 × 事件：ETH_LINK_UP 不能被报成异常（2026-08-22）----
# 「以太网当前不在线」的那几个状态收到 ETH_LINK_UP 是**正常**的（用户插网线）。
# 之前 WIFI_ONLINE / WIFI_RECONNECTING 没有这个分支，落到 else 打
# "Unexpected event ETH_LINK_UP in state ..."，两次拔插实测都命中。
# 不是功能故障（DHCP 由 IDF 自己起，随后的 ETH_GOT_IP 有分支），但把正常事件
# 报成异常会**掩盖真正异常的那些**。
# ⚠️ 用状态白名单而不是写死条数 —— db7cab8 记着这个教训（判据写死 5、改 7 后假红）。
# ⚠️ ETH_CONNECTING / ETH_ONLINE / BOTH_ONLINE 故意**不在**名单里：那几个状态下
#    以太网本来就是 up 的，再来一个 LINK_UP 确实异常，该告警。
MISS=""
for ST in SOFTAP_ONLY WIFI_CONNECTING WIFI_ONLINE WIFI_RECONNECTING WIFI_PROVISION_FAILED NO_NETWORK; do
    # ⚠️ 锚点必须是 `^    case NET_STATE_X:$`（行尾即冒号）—— state_name() 里是
    #    `    case NET_STATE_X:       return "..."`，只匹配 case 名会从那里就开始，
    #    范围跨到别的状态去，判据变成恒绿。2026-08-22 变异测试当场抓到过一次。
    awk "/^    case NET_STATE_$ST:\$/,/^        break;/" network_mgr.c \
      | grep -q 'NET_EVT_ETH_LINK_UP' || MISS="$MISS $ST"
done
check "WiFi 侧状态都处理 ETH_LINK_UP (缺:${MISS:- 无})" "[ -z \"$MISS\" ]"

check "两个接口都设 DHCP 主机名" "[ $(grep -c 'esp_netif_set_hostname' wifi_mgr.c eth_mgr.c | grep -c ':1') -eq 2 ]"
# 反向不变量：网络层不得因地址变化去断 EZSP 连接（含绕过 ezsp_bridge 直接调 tcp_server）
check "网络层不断 EZSP 连接"     "! grep -qE '^[^*/]*(ezsp_bridge_kick|tcp_server_close_all_clients|tcp_server_disconnect)' network_mgr.c"
check "网络层不依赖 ezsp_bridge"  "! grep -q '^#include \"ezsp_bridge.h\"' network_mgr.c"
# 断开虽然撤了，但地址变化仍要可观测 —— 否则将来排查换址问题时手里什么都没有
check "地址变化仍留痕"           "grep -q 'addr snapshot' network_mgr.c"
# ---- ⚠️ 反向不变量：禁止裸 dhcpc_stop（2026-08-21，撤回 db7cab8 之后加）----
# db7cab8 加过「接口下线归还 DHCP 租约」，当天现场证实是**停机级**缺陷：
# dhcpc_stop 把 dhcpc_status 置为 STOPPED（不是 INIT，esp_netif_lwip.c:1638）并清空
# ip_info（:1626），链路恢复时 esp_netif_action_connected 落到 STOPPED 分支，
# ip 无效 → ESP_LOGE("invalid static ip") 且**不启 DHCP**；esp_netif_down 只在
# STARTED 时才还回 INIT（:1911），救不了。那条腿到整机重启为止拿不到 IP。
# 详见 network_mgr.c 里「不要再加「接口下线时主动归还 DHCP 租约」」那段注释
# （可直接 grep 这句话），以及 docs/guide/WiFi逻辑全解.md 第八节。
#
# 因此：**每个 dhcpc_stop 后面必须紧跟 set_ip_info 或 dhcpc_start**（即只允许
# 「配静态 IP」这一种合法意图），不允许 stop 完就撒手。
# ⚠️ 窗口必须是 12 行不是 6：apply_static_ip_config() 里 stop 在 :649、
#    set_ip_info 在 :656（L+7，中间隔着 esp_netif_ip_info_t 初始化），
#    6 行窗口会在**正确代码上**假红。
# ⚠️ ^[^*/]* 前缀是为了排除注释里的提及 —— 本项目栽过「grep -c 把自己写的注释
#    也数进去」（8 vs 实际 3）。代价：匹配点**之前**含 / 或 * 的真实代码行
#    （如行内块注释 `if (x) { /* c */ esp_netif_dhcpc_stop(y); }`）会被整个漏掉。
#    本仓库不这么写，接受。
# ⚠️ 这条判据只看**形状**，不看 netif 是不是同一个：12 行窗口内出现任意
#    set_ip_info 就放行。它防的是「stop 完就撒手」，不是防所有误用。
# ⚠️ 扫描面含 wifi_mgr.c / eth_mgr.c —— netif 句柄住在那两个文件里，
#    是「把这件事换个地方再做一遍」最自然的落点。
SCAN="network_mgr.c wifi_mgr.c eth_mgr.c"
STOPN=$(grep -c '^[^*/]*esp_netif_dhcpc_stop(' $SCAN | awk -F: '{s+=$2} END{print s+0}')
BAD=$(grep -Hn '^[^*/]*esp_netif_dhcpc_stop(' $SCAN | while IFS=: read -r F L _; do
    sed -n "$((L+1)),$((L+12))p" "$F" \
      | grep -q '^[^*/]*esp_netif_set_ip_info(\|^[^*/]*esp_netif_dhcpc_start(' || echo "$F:$L"
  done | wc -l)
# 下限只防「全被删光 → grep 零匹配 → BAD=0 假绿」，所以是 >=1 不是写死的具体条数。
# （db7cab8 自己记着这个教训：判据写死 5、改成 7 之后假红。）
check "dhcpc_stop 必须配静态IP，无裸调用 (裸 $BAD / 共 $STOPN)" "[ $STOPN -ge 1 ] && [ $BAD -eq 0 ]"

echo ""
echo "--- T2.22 WiFi 重连不得抢占以太网（2026-09-14）---"
# WiFi 拿到 IP 必须走统一入口 wifi_got_ip_online()，该入口在以太网在线时保持 ETH、进 BOTH_ONLINE。
# 反向：不允许再出现"WiFi got-IP 后无条件 set_default_route(NET_IF_WIFI) + WIFI_ONLINE"的裸写法。
check "存在 wifi_got_ip_online 且尊重 s_eth_has_ip -> BOTH_ONLINE" \
    "python3 ../../tools/grep_code.py -q 'wifi_got_ip_online' network_mgr.c && awk '/static void wifi_got_ip_online/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | grep -q 's_eth_has_ip' && awk '/static void wifi_got_ip_online/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | grep -q 'NET_STATE_BOTH_ONLINE'"
# WIFI_CONNECTING / WIFI_RECONNECTING 的 got-IP 分支必须调统一入口，不得自己切 WiFi 默认路由
WGOTIP_HELPER=$(grep -c 'wifi_got_ip_online()' network_mgr.c)
check "WiFi got-IP 分支统一走 helper（>=6 处）" "[ $WGOTIP_HELPER -ge 6 ]"

echo ""
echo "--- T2.23 浮动服务 IP（2026-09-14）---"
check "float_svc_update 存在且按 eth链路/wifi有IP/服务IP有效 判定" \
    "awk '/static void float_svc_update/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | grep -q 's_eth_link_up' && awk '/static void float_svc_update/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | grep -q 's_wifi_has_ip' && awk '/static void float_svc_update/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | grep -q 's_svc_ip_valid'"
check "浮动失败退回 WiFi 自己的寻址（不留死状态）" \
    "awk '/static void float_svc_update/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | grep -A14 'set_ip_info(sta, &s_svc_ip)' | grep -q 'wifi_restore_own_addr(sta)'"
# 还原函数的兜底必须是 dhcpc_start：静态还原失败也不能让 WiFi 停在没地址的死状态
check "wifi_restore_own_addr 最后兜底 DHCP" \
    "awk '/^static void wifi_restore_own_addr\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | tail -3 | python3 ../../tools/grep_code.py -q 'esp_netif_dhcpc_start\(sta\);' /dev/stdin"
# 收回不再一律 dhcpc_start（配了静态的 WiFi 会被改成 DHCP 直到重启，2026-10-06 审计）
check "收回时 WiFi 还原成自己的寻址（float_svc_update 里不直接 dhcpc_start）" \
    "awk '/^static void float_svc_update\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py --absent -q 'esp_netif_dhcpc_start\(' /dev/stdin"
check "ETH_GOT_IP 时捕获服务IP快照" \
    "python3 ../../tools/grep_code.py -q 's_svc_ip_valid = true' network_mgr.c"
check "状态机结尾 reconcile 浮动IP" \
    "python3 ../../tools/grep_code.py -q 'float_svc_update\(\);' network_mgr.c"
check "以太网链路回来即收回（reconcile 依赖 s_eth_link_up 而非仅 s_eth_has_ip）" \
    "grep -q 's_eth_link_up = true' network_mgr.c && grep -q 's_eth_link_up = false' network_mgr.c"
# 2026-10-06 随 T9.20 补的两件（运行中浮动也受益）
FSU="awk '/^static void float_svc_update/{f=1} f{print} f&&/^}/{exit}' network_mgr.c"
check "浮动前比网段（有线、无线不在同一网段就不浮）" \
    "$FSU | python3 ../../tools/grep_code.py -q 'svc_same_subnet\\(' /dev/stdin"
check "浮动时把 WiFi 的 DNS 搬过去（set_ip_info 会清全局 DNS）" \
    "$FSU | python3 ../../tools/grep_code.py -q 'esp_netif_get_dns_info\\(sta, ESP_NETIF_DNS_MAIN' /dev/stdin && $FSU | grep -A2 'set_ip_info(sta, &s_svc_ip)' | python3 ../../tools/grep_code.py -q 'float_dns_restore\\(' /dev/stdin"
check "开机时按真实链路初始化 s_eth_link_up" \
    "python3 ../../tools/grep_code.py -q 's_eth_link_up = eth_mgr_link_is_up\\(\\);' network_mgr.c"

echo ""
echo "--- T9.20 网线断着时重启：开机接管以太网服务 IP（2026-10-06）---"
# 判据（行为）在 svc_policy.c，主机侧用例化；这里只钉代码形状（7.5：两类检查各管各的）。
# 行为正本是 Kconfig help（BITENGINE_WIFI_FLOAT_BOOT_TAKEOVER）。
check "svc_policy 不引 ESP-IDF / FreeRTOS / lwIP 头（主机侧可编）" \
    "! grep -qE '^#include *[<\"](esp_|freertos/|lwip/|nvs|sdkconfig)' svc_policy.c svc_policy.h"
check "CMake 把 svc_policy.c 编进组件" "grep -q '\"svc_policy.c\"' CMakeLists.txt"
check "test_host 编的是产品源文件本身" "grep -q '\\.\\./svc_policy\\.c' test_host/run.sh"
check "test_host 通过" "bash test_host/run.sh >/dev/null 2>&1"
check "Kconfig 开关在 sdkconfig 里（默认开）" "grep -q '^CONFIG_BITENGINE_WIFI_FLOAT_BOOT_TAKEOVER=y' ../../sdkconfig"
# 租约读数的两个前提：coarse tick 1 s（t0_timeout 才等于秒数）；dhcp_timeout_t 32 位（不被截成 18 h）
check "lwIP DHCP coarse tick = 1 s" "grep -q '^CONFIG_LWIP_DHCP_COARSE_TIMER_SECS=1\$' ../../sdkconfig"
check "dhcp 计时字段 32 位有编译期断言" \
    "python3 ../../tools/grep_code.py -q '_Static_assert\\(sizeof\\(\\(\\(struct dhcp \\*\\)0\\)->t0_timeout\\) >= 4' network_mgr.c"
# 本构建（CONFIG_LWIP_DHCP_DOES_ARP_CHECK=y）的 acd_* 是 ESP 给 DHCP 专用的替身 port/acd_dhcp_check.c：
# acd_start 忽略地址参数、acd_stop 不存在（链接失败）、DHCP 处于 CHECKING 时调到 acd_remove 会让它永远卡住
check "不调 lwIP acd_*（本构建是 DHCP 专用替身）" \
    "python3 ../../tools/grep_code.py --absent -q '\\bacd_(add|start|stop|remove)\\s*\\(' network_mgr.c"
check "不用 LOCK_TCPIP_CORE（本配置是空宏，写了也不保护）" \
    "python3 ../../tools/grep_code.py --absent -q 'LOCK_TCPIP_CORE' network_mgr.c"
# lwIP 线程里的回调绝不能拿状态机锁：持锁的状态机正同步等 lwIP 线程处理 esp_netif IPC → 死锁。
# 也不进状态机、不写 NVS（flash 操作别压在网络协议栈的线程上）。逐个函数体判。
TCPIP_FUNCS=$(grep -oE '^static void [a-z_]+_in_tcpip\(' network_mgr.c | sed 's/static void //;s/(//')
TCPIP_N=$(echo $TCPIP_FUNCS | wc -w)
BADT=""
for fn in $TCPIP_FUNCS; do
    awk "/^static void $fn\\(/{f=1} f{print} f&&/^}/{exit}" network_mgr.c \
      | python3 ../../tools/grep_code.py -q 'xSemaphoreTake|network_state_machine|float_svc_update|boot_takeover_eval|svc_deny|nvs_' /dev/stdin \
      && BADT="$BADT $fn"
done
check "lwIP 线程回调不拿锁、不进状态机、不碰 NVS（$TCPIP_N 个，违规:${BADT:- 无}）" "[ -z \"$BADT\" ] && [ $TCPIP_N -ge 4 ]"
# 读 struct dhcp 只能在 lwIP 线程（没有 core lock）
DHCPR_ALL=$(python3 ../../tools/grep_code.py 'netif_dhcp_data\(' network_mgr.c | wc -l)
# 第二个读者是 T9.37 的诊断（svc_dhcp_read_in_tcpip，经 esp_netif_tcpip_exec 在 lwIP 线程里跑，2026-10-08）
DHCPR_IN=$(awk '/^static void lease_read_in_tcpip\(/{f=1} /^static esp_err_t svc_dhcp_read_in_tcpip\(/{f=1} f{print} f&&/^}/{f=0}' network_mgr.c | python3 ../../tools/grep_code.py 'netif_dhcp_data\(' /dev/stdin | wc -l)
check "struct dhcp 只在 lwIP 线程里的读函数里读（lease_read_in_tcpip / svc_dhcp_read_in_tcpip，共 $DHCPR_ALL 处）" "[ $DHCPR_ALL -ge 1 ] && [ $DHCPR_ALL -eq $DHCPR_IN ]"
# NVS 写只在 svc_persist_step 与它调的 svc_rec_erase 里（esp_timer 任务、内部栈、不持状态机锁）
NVSW_ALL=$(python3 ../../tools/grep_code.py 'nvs_set_|nvs_commit|nvs_erase' network_mgr.c | wc -l)
NVSW_IN=$(awk '/^static void (svc_persist_step|svc_rec_erase|svc_id_store)\(/{f=1} f{print} f&&/^}/{f=0}' network_mgr.c | python3 ../../tools/grep_code.py 'nvs_set_|nvs_commit|nvs_erase' /dev/stdin | wc -l)
check "NVS 只在 svc_persist_step / svc_rec_erase / svc_id_store 里写（共 $NVSW_ALL 处）" "[ $NVSW_ALL -ge 1 ] && [ $NVSW_ALL -eq $NVSW_IN ]"
ERASE_CALLERS=$(python3 ../../tools/grep_code.py 'svc_rec_erase\(' network_mgr.c | grep -v 'static void svc_rec_erase' | wc -l)
ERASE_IN=$(awk '/^static void svc_persist_step\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py 'svc_rec_erase\(' /dev/stdin | wc -l)
check "svc_rec_erase 只被 svc_persist_step 调（$ERASE_CALLERS 处）" "[ $ERASE_CALLERS -ge 1 ] && [ $ERASE_CALLERS -eq $ERASE_IN ]"
# 只看"换址"那个分支内部（从地址比较到它的第一个 return）：同一函数里还有别的 svc_rec_erase，
# 按整个函数体判会被另一处满足（变异测试抓到过）
check "以太网换址而墙钟不可信时作废旧快照（审计 F4）" \
    "awk '/^static void svc_persist_step\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | awk '/rd.ip != s_rec_cache.ip/{f=1} f{print} f&&/return;/{exit}' | python3 ../../tools/grep_code.py -q 'svc_rec_erase\(' /dev/stdin"
check "svc_persist_step 不持状态机锁" \
    "awk '/^static void svc_persist_step\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py --absent -q 'xSemaphoreTake' /dev/stdin"
# status_log_task 的栈在 PSRAM：写 flash 关 cache → 栈不可用 → 崩（psram_task.h 规则 1）
check "PSRAM 栈的 status_log_task 不碰 NVS / 开机接管" \
    "awk '/^static void status_log_task\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py --absent -q 'nvs_|svc_' /dev/stdin"
# 读回来的快照绝不能直接生效：只有探测通过（svc_probe_consume）和以太网实时拿到地址两处能置位
# 第三处是 T9.37 服务身份把 A_S 镜像进来（svc_id_update，只在身份机制生效时跑）；退回旧行为（只认 MAC）时必须清掉，
# 否则网线断着时静态浮动会不经探测挂上一个从 NVS 读来的地址（2026-10-07）
VALID_N=$(python3 ../../tools/grep_code.py 's_svc_ip_valid\s*=\s*true' network_mgr.c | wc -l)
VALID_ID=$(awk '/^static void svc_id_update\(void\)$/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py 's_svc_ip_valid\s*=\s*true' /dev/stdin | wc -l)
VALID_N=$((VALID_N - VALID_ID))
check "退回旧行为时，本次没确认过的服务地址作废（不经探测别浮）" \
    "FN '^static void svc_id_fallback\\\\(' | python3 ../../tools/grep_code.py -q 'if \\(!s_id_as_live\\) s_svc_ip_valid = false;' /dev/stdin && FN '^static void svc_id_on_got_ip\\\\(svc_nic_t nic\\\\)\$' | awk '/case SVC_JUDGE_MAC_ONLY:/{f=1} f&&/return;/{exit} f' | grep -q 'svc_id_fallback(true'"
check "身份定时器只排将来的时刻（过去的会忙循环）" \
    "awk '/^static void svc_id_update\\(void\\)\$/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | grep -q 'grace_end > now' && awk '/^static void svc_id_update\\(void\\)\$/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | grep -q 'if (d > now'"
check "s_svc_ip_valid 只在两处置位（ETH_GOT_IP / 探测通过；身份机制镜像另计 $VALID_ID 处；其余实为 $VALID_N）" \
    "[ $VALID_N -eq 2 ] && awk '/^static bool svc_probe_consume\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py -q 's_svc_ip_valid\s*=\s*true' /dev/stdin"
check "读回快照的 svc_boot_load 不置 s_svc_ip_valid" \
    "awk '/^static void svc_boot_load\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py --absent -q 's_svc_ip_valid' /dev/stdin"
# 网线一插上 / WiFi 一掉，要当场作废在途探测：状态机每跑一次都推闸门，且在 float_svc_update 之前
SM="awk '/^void network_state_machine\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c"
check "状态机结尾先推闸门再 reconcile 浮动" \
    "L1=\$($SM | grep -n 'boot_takeover_eval();' | head -1 | cut -d: -f1); L2=\$($SM | grep -n 'float_svc_update();' | tail -1 | cut -d: -f1); [ -n \"\$L1\" ] && [ -n \"\$L2\" ] && [ \$L1 -lt \$L2 ]"
check "以太网本次拿到地址 → 开机接管退场" \
    "python3 ../../tools/grep_code.py -q 's_eth_ip_seen = true;' network_mgr.c"
# s_failed_over 置在"地址真的挂上了"那一刻（float_svc_update 浮动成功的分支），不在探测通过时：两者之间网线
# 可能插回、根本没浮，网线回来就不该发 RECOVERED（2026-10-06 复核）。网线回来发 RECOVERED 靠它
check "挂上服务 IP 时置 s_failed_over（网线回来发 RECOVERED，MQTT / 边界路由跟随）" \
    "awk '/^static void float_svc_update\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | grep -A14 'set_ip_info(sta, &s_svc_ip)' | grep -B20 'wifi_restore_own_addr(sta)' | python3 ../../tools/grep_code.py -q 's_failed_over\s*=\s*true' /dev/stdin"
check "探测通过处不置 s_failed_over（等真的挂上）" \
    "awk '/^static bool svc_probe_consume\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py --absent -q 's_failed_over' /dev/stdin"
check "探测用 etharp_query 发起、etharp_find_addr 收结果" \
    "awk '/^static void probe_start_in_tcpip\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py -q 'etharp_query\(' /dev/stdin && awk '/^static void probe_check_in_tcpip\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py -q 'etharp_find_addr\(' /dev/stdin"
check "应答方是自己的 WiFi / 以太网 MAC 不算冲突" \
    "awk '/^static void probe_check_in_tcpip\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py -q 'lw->hwaddr' /dev/stdin && awk '/^static void probe_check_in_tcpip\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py -q 'elw->hwaddr' /dev/stdin"
# 必须是"写回那一行自己带代号比较"：函数开头还有一处同样的比较（读 sent），光看函数体里有没有
# 这个表达式会被那一处满足 —— 而开头比较之后、写回之前，别的任务可能已经作废并发起了新一轮，
# 不带比较的写回会把新一轮的状态盖掉、让它永远等不到结论（变异测试抓到过）
check "晚到的探测结果按代号丢弃（写回本身带代号比较）" \
    "awk '/^static void probe_check_in_tcpip\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py -q 'if \(s_probe_res.gen == gen\) s_probe_res = res;' /dev/stdin"
check "探测发起时记 sent 也带代号比较" \
    "awk '/^static void probe_start_in_tcpip\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py -q 'if \(s_probe_res.gen == gen\)' /dev/stdin"
# SNTP 的同步回调槽全系统只有一个：只在 main.c 注册一次，并把信号转给 network_mgr
# 墙钟出处（审计 R2-01）：死机 / 看门狗之后 main.c 补的那次 esp_restart 会让复位原因变成 SW，
# 只看复位原因就会把带漂移的墙钟当可信。判据：可信 = 本次对过时，或继承（SW 且 RTC 魔数）；
# main.c 在补重启之前清标记。
check "墙钟可信只认本次对时或继承来的标记（不直接看复位原因）" \
    "awk '/^static bool svc_clock_trusted\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py -q 's_clock_inherited' /dev/stdin && awk '/^static bool svc_clock_trusted\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py --absent -q 's_reset_reason' /dev/stdin"
check "继承墙钟要求 ESP_RST_SW 且 RTC 魔数" \
    "python3 ../../tools/grep_code.py -q 's_clock_inherited = \(s_reset_reason == ESP_RST_SW && s_clock_trust == SVC_CLOCK_TRUSTED\)' network_mgr.c"
check "main.c 补那次 esp_restart 之前清墙钟标记" \
    "awk '/rst != ESP_RST_POWERON && rst != ESP_RST_SW && attempts == 0/{f=1} f{print} f&&/esp_restart\(\);/{exit}' ../../main/main.c | python3 ../../tools/grep_code.py -q 'network_mgr_note_unclean_reset\(\);' /dev/stdin"
check "对时成功时置可信标记" \
    "awk '/^void network_mgr_notify_time_synced\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py -q 's_clock_trust = SVC_CLOCK_TRUSTED' /dev/stdin"
# 探测不 fail-open（审计 F7）：请求没真发出去 / 窗口不够长 → 没探成，不是"干净"
check "探测结论要求请求真的发出去且窗口够长" \
    "awk '/^static void probe_check_in_tcpip\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py -q 'if \(sent && esp_timer_get_time\(\) - sent_us >= .*SVC_PROBE_MIN_SENT_MS' /dev/stdin"
check "作废探测先停定时器再改代号" \
    "awk '/^static void svc_probe_cancel\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | grep -n 'esp_timer_stop(s_svc_probe_timer)\|s_bt_gen++' | head -1 | grep -q 'esp_timer_stop'"
# T2.16 口径不变（审计 R2-02）：只有服务 IP 真的挪到了 WiFi 上才算本次开机退过
check "s_failed_over 只在接管成立时置（闸门判定处不置）" \
    "awk '/^static void boot_takeover_eval\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py --absent -q 's_failed_over' /dev/stdin"
# esp_timer 回调里不同步等 lwIP 线程、也不等状态机的锁：闸门与挂地址都在事件循环（svc_gate_work）里做
check "svc_work_cb 只写盘与投递（不挂地址、不推闸门）" \
    "awk '/^static void svc_work_cb\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py --absent -q 'float_svc_update|boot_takeover_eval|svc_probe_consume' /dev/stdin && awk '/^static void svc_work_cb\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py -q 'esp_event_post\(NET_DEFER_EVENT, NET_DEFER_SVC_GATE' /dev/stdin"
check "svc_gate_work 推闸门并挂地址" \
    "awk '/^static void svc_gate_work\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py -q 'float_svc_update\(\);' /dev/stdin && awk '/^static void svc_gate_work\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py -q 'svc_probe_consume\(\)' /dev/stdin"

echo ""
echo "--- T9.28 esp_timer 回调不拿状态机的锁（2026-10-07）---"
# esp_timer 任务优先级 22：回调 portMAX_DELAY 等 s_sm_mutex 时，持锁方被优先级继承抬到 22，
# 等的那段全系统定时器都停。回调只投递 NET_DEFER_EVENT，活在 defer_handler（sys_evt）里做。
check "所有 esp_timer 回调都不拿锁、不进状态机（逐个回调）" "python3 check_timer_cbs.py ."
check "投递事件的处理函数在任何定时器建立之前注册" \
    "awk '/esp_event_handler_instance_register\(NET_DEFER_EVENT/{a=NR} /esp_timer_create\(&dhcp_timer_args/{b=NR} END{exit !(a && b && a < b)}' network_mgr.c"
check "DHCP 超时只经 arm / disarm 启停（直接 start 只剩 arm 里与回调重排两处）" \
    "[ \$(python3 ../../tools/grep_code.py 'esp_timer_start_once\(s_eth_dhcp_timer' network_mgr.c | wc -l) -eq 2 ] && [ \$(python3 ../../tools/grep_code.py 'esp_timer_stop\(s_eth_dhcp_timer' network_mgr.c | wc -l) -eq 2 ]"
check "DHCP 超时带代号，事件循环里对不上就丢" \
    "awk '/^static void eth_dhcp_timeout_work\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py -q 'gen != s_eth_dhcp_gen' /dev/stdin"
check "arm 先停再起、代号 +1" \
    "awk '/^static void eth_dhcp_timer_arm\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | grep -n 'esp_timer_stop\|s_eth_dhcp_gen++\|esp_timer_start_once' | awk -F: '{print \$2}' | tr -d ' ' | tr '\n' ' ' | grep -q '^esp_timer_stop.*s_eth_dhcp_gen++.*esp_timer_start_once'"
check "重连耗尽经事件循环进状态机" \
    "awk '/^static void exhausted_timer_cb\([^;]*$/{f=1} f{print} f&&/^}/{exit}' wifi_mgr.c | python3 ../../tools/grep_code.py -q 'network_mgr_defer_event\(NET_EVT_WIFI_RECONNECT_EXHAUSTED\)' /dev/stdin"
check "投递失败会重排自己（三处都不丢活）" \
    "awk '/^static void eth_dhcp_timeout_cb\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | grep -q 'esp_timer_start_once(s_eth_dhcp_timer, NET_DEFER_RETRY_US)' && awk '/^static void svc_work_cb\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | grep -q 'svc_kick(NET_DEFER_RETRY_US)' && awk '/^static void exhausted_timer_cb\([^;]*$/{f=1} f{print} f&&/^}/{exit}' wifi_mgr.c | grep -q 'esp_timer_start_once(s_exhausted_timer'"
check "闸门的以太网链路取两个来源（防漏掉开机那次 LINK_UP）" \
    "awk '/^static void boot_takeover_eval\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py -q 's_eth_link_up \|\| eth_mgr_link_is_up\(\)' /dev/stdin"
# SSID 锁（2026-10-06 复核）：以太网缺席时换了 WiFi，服务 IP 不跟去新网络；但同 SSID 重提、换新 WiFi 失败
# 回滚到原凭据都要能浮回来 —— 所以不能在 CREDENTIALS_SET 里一刀切作废 s_svc_ip_valid
check "浮动要求当前 SSID 与服务 IP 所属的一致（SSID 锁）" \
    "awk '/^static void float_svc_update\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py -q 'want_float = .*svc_ssid_ok\(\)' /dev/stdin"
# 两个赋值点分别钉（计数会把 svc_ssid_ok 里的 == 也数进去，变异测试抓到过）
check "SSID 锁在以太网拍快照时记下" \
    "python3 ../../tools/grep_code.py -q 's_svc_ssid = wifi_cfg_ssid_hash\(\);' network_mgr.c"
check "SSID 锁在开机接管时记下" \
    "awk '/^static bool svc_probe_consume\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py -q 's_svc_ssid\s*=\s*s_bt_rec.ssid_hash;' /dev/stdin"
check "换凭据时不作废服务 IP（只作废 WiFi 自己的寻址快照）" \
    "awk '/if \(event == NET_EVT_WIFI_CREDENTIALS_SET\)/{f=1} f{print} f&&/return;/{exit}' network_mgr.c | python3 ../../tools/grep_code.py --absent -q 's_svc_ip_valid' /dev/stdin && awk '/if \(event == NET_EVT_WIFI_CREDENTIALS_SET\)/{f=1} f{print} f&&/return;/{exit}' network_mgr.c | python3 ../../tools/grep_code.py -q 's_wifi_own.valid = false;' /dev/stdin"
# WiFi 自己的寻址快照：按浮动前的实际寻址还原（不重读配置：已保存未生效的静态 IP 不能提前下发）；
# 拿到自己的地址就刷新、断线 / 丢地址就作废；全局 DNS 空着时用快照里的 DNS
check "收回按快照还原，不重读 WiFi 配置" \
    "awk '/^static void wifi_restore_own_addr\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py --absent -q 'config_mgr_get_wifi' /dev/stdin"
check "WiFi 拿到自己的地址时刷新快照、断线 / 丢地址时作废" \
    "awk '/^void network_state_machine\(/{f=1} f{print} f&&/switch \(s_state\)/{exit}' network_mgr.c | python3 ../../tools/grep_code.py -q 'wifi_own_refresh\(sta\)' /dev/stdin && awk '/^void network_state_machine\(/{f=1} f{print} f&&/switch \(s_state\)/{exit}' network_mgr.c | python3 ../../tools/grep_code.py -q 'NET_EVT_WIFI_LOST_IP\) \{' /dev/stdin"
check "浮动时全局 DNS 已空就用快照里的 DNS" \
    "awk '/^static void float_svc_update\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py -q 'if \(ESP_IP_IS_ANY\(dns_main.ip\)\) \{ dns_main = s_wifi_own.dns_main' /dev/stdin"
check "擦快照失败时保留缓存（下次重试）" \
    "awk '/^static void svc_rec_erase\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py -q 'if \(err == ESP_OK\) s_rec_cache_ok = false;' /dev/stdin"
# 静态以太网 IP 开机不接管（没有租期可确认）：这个排除在 network_mgr.c 里，主机侧测试测不到 —— 钉住两处早退
# 要真的早退：只查 if 在不在会被"删了 return"的改动骗过（变异测试抓到过）
check "静态以太网 IP：开机不读快照、也不存快照" \
    "awk '/^static void svc_boot_load\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | awk '/if \(!config_mgr_get_eth\(\)->dhcp\)/{f=1} f{print} f&&/^    }/{exit}' | python3 ../../tools/grep_code.py -q 'return;' /dev/stdin && awk '/^static void svc_persist_step\(/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | python3 ../../tools/grep_code.py -q 'if \(!config_mgr_get_eth\(\)->dhcp\) return;' /dev/stdin"
for ST in WIFI_ONLINE BOTH_ONLINE; do
    check "$ST 认 WIFI_GOT_IP（浮动自己的 set_ip_info 也会发；LOST_IP 后要能恢复）" \
        "awk '/^    case NET_STATE_$ST:\$/,/^        break;/' network_mgr.c | python3 ../../tools/grep_code.py -q 'event == NET_EVT_WIFI_GOT_IP' /dev/stdin"
done
check "SNTP 同步回调只在 main.c 注册一次并通知 network_mgr" \
    "[ \$(python3 ../../tools/grep_code.py 'sntp_set_time_sync_notification_cb\(' ../../main/main.c ../*/*.c | wc -l) -eq 1 ] && python3 ../../tools/grep_code.py -q 'network_mgr_notify_time_synced\(\);' ../../main/main.c"
check "/status 露出 svc_takeover" "grep -q 'svc_takeover' ../webui/rest_api.c && grep -q 'network_mgr_svc_takeover_state()' ../webui/rest_api.c"
check "恢复出厂会擦快照命名空间" "sed -n '/OWNED_NS\\[\\] = {/,/^};/p' ../config_mgr/config_mgr.c | grep -q '\"net_svc\"'"

# ---- T?: wifi_keepalive 的栈必须留在 PSRAM（2026-09-15）----
# 它在 net_init 区间内，而以太网主用时常态空转；退回 xTaskCreate = 白吃 3072 内部 SRAM。
check "wifi_keepalive 栈在 PSRAM" \
    "grep -q 'psram_task_create(wifi_keepalive_task' wifi_mgr.c"
# 反向不变量：用 grep_code.py 剥掉注释再判，否则上面那段解释性注释会自己把检查绊红（7.4）
check "wifi_keepalive 没退回 xTaskCreate" \
    "python3 ../../tools/grep_code.py --absent --quiet 'xTaskCreate\\(wifi_keepalive_task' wifi_mgr.c"
# psram_task.h 规则 1：该任务的调用链不得碰 flash/NVS，否则栈在 PSRAM 上会随机崩
check "wifi_keepalive 调用链不碰 NVS" \
    "python3 ../../tools/grep_code.py --absent --quiet 'nvs_|esp_partition_' wifi_mgr.c"

echo ""
echo "--- WiFi 回传诊断：断线次数与时长、信道/DFS（2026-10-05）---"
check "wifi_link_stats_t 与 network_mgr_wifi_link_stats 已声明" \
    "grep -q 'wifi_link_stats_t' include/network_mgr.h && grep -q 'network_mgr_wifi_link_stats' include/network_mgr.h"
check "DFS 判定 = 52..144" \
    "grep -A2 'network_mgr_wifi_channel_is_dfs' include/network_mgr.h | grep -q 'ch >= 52 && ch <= 144'"
# 顺序是这个功能能不能数对的关键：note_link_lost 靠"刚才还连着"区分新断线与重试失败，
# 放到 set_sta_connected(false) 之后就一次都数不到
check "STA_DISCONNECTED 里 note_link_lost 在 set_sta_connected(false) 之前" \
    "L=\$(grep -n 'wifi_mgr_note_link_lost();' net_events.c | head -1 | cut -d: -f1); S=\$(grep -n 'wifi_mgr_set_sta_connected(false);' net_events.c | head -1 | cut -d: -f1); [ -n \"\$L\" ] && [ -n \"\$S\" ] && [ \$L -lt \$S ]"
check "GOT_IP 里调 note_link_restored" \
    "awk '/case IP_EVENT_STA_GOT_IP/{f=1} f&&/case IP_EVENT_STA_LOST_IP/{exit} f' net_events.c | python3 ../../tools/grep_code.py -q 'wifi_mgr_note_link_restored\(\);' /dev/stdin"
check "重试失败不重复计数（只数连着→断开）" \
    "python3 ../../tools/grep_code.py -q 'if \(!s_sta_connected \|\| s_down_since_s\) return;' wifi_mgr.c"
# 在子 shell 里循环：eval 里的 exit 会直接结束整个 VERIFY.sh（第一版就这么无声地截断过）
check "/status 露出六个 WiFi 诊断字段" \
    "( for f in wifi_channel wifi_dfs wifi_disconnects wifi_down_s wifi_last_outage_s wifi_longest_outage_s; do grep -qE \"\$f..:\" ../webui/rest_api.c || exit 1; done )"

echo ""
echo "--- 浮动 IP 必须主动发 GARP（2026-10-05：LWIP_ACD=1 时 lwIP 改地址不发）---"
check "浮到 WiFi 后立刻宣告 GARP" \
    "awk '/^static void float_svc_update/{f=1} f&&/^}/{exit} f' network_mgr.c | grep -A3 's_svc_on_wifi = true;' | python3 ../../tools/grep_code.py -q 'float_garp_announce\(\);' /dev/stdin"
check "GARP 经 tcpip_try_callback 投到 lwIP 线程" \
    "python3 ../../tools/grep_code.py -q 'tcpip_try_callback\(garp_in_tcpip' network_mgr.c && awk '/^static void garp_in_tcpip/{f=1} f&&/^}/{exit} f' network_mgr.c | python3 ../../tools/grep_code.py -q 'etharp_gratuitous\(lw\)' /dev/stdin"
check "补发 ≥2 次且收回后停发" \
    "grep -qE '#define FLOAT_GARP_REPEAT +[2-9]' network_mgr.c && awk '/^static void garp_timer_cb/{f=1} f&&/^}/{exit} f' network_mgr.c | python3 ../../tools/grep_code.py -q 'if \(!s_svc_on_wifi\)' /dev/stdin"
check "注释写明为什么不能指望 lwIP 自动发（LWIP_ACD）" "grep -q 'LWIP_ARP && !LWIP_ACD' network_mgr.c"


echo ""; echo "--- T2.21 保活自己发 ICMP，不再用 esp_ping（2026-10-05，failover DMA 低谷）---"
KAO="awk '/^static bool keepalive_open\(void\)\$/{f=1} f&&/^}/{exit} f' wifi_mgr.c"
KAD="awk '/^static void keepalive_drain\(void\)\$/{f=1} f&&/^}/{exit} f' wifi_mgr.c"
KAS="awk '/^static void wifi_keepalive_supervise\(void\)\$/{f=1} f&&/^}/{exit} f' wifi_mgr.c"
check "保活不调 esp_ping（它另起 2.5 KB 内部栈任务）" "python3 ../../tools/grep_code.py --absent -q 'esp_ping_' wifi_mgr.c"
check "保活 socket 钉在 STA 口"                 "$KAO | python3 ../../tools/grep_code.py -q 'SO_BINDTODEVICE' /dev/stdin && $KAO | python3 ../../tools/grep_code.py -q 'esp_netif_get_netif_impl_index\(s_sta_netif\)' /dev/stdin"
check "保活回包非阻塞收干净"                     "$KAD | python3 ../../tools/grep_code.py -q 'MSG_DONTWAIT' /dev/stdin"
check "WiFi 不做主时关掉保活 socket"            "$KAS | python3 ../../tools/grep_code.py -q 'keepalive_stop\(\);' /dev/stdin"
check "停保活时日志带发送/回应计数（空口生效的证据）" "grep -q 'WiFi keepalive stopped (sent %' wifi_mgr.c"

check "停保活前先收最后一个回包（否则永远少数一个）" "awk '/^static void keepalive_stop\\(void\\)$/{f=1} f&&/^}/{exit} f' wifi_mgr.c | awk '/keepalive_drain\\(\\);/{a=NR} /close\\(s_ka_sock\\)/{b=NR} /s_ka_gw = 0/{c=NR} END{exit !(a && b && c && a<b && a<c)}'"

echo ""
echo "--- T9.34 本机 mDNS 只宣告以太网服务 IP（2026-10-07）---"
# 行为正本：Kconfig BITENGINE_MDNS_SVC_IP_ONLY 的 help；判据：svc_policy.c 的 svc_mdns_decide（test_host 测）
# FN <函数定义行开头的正则>：取该函数的整个函数体（到第一个顶格的 }）
FN() { awk -v pat="$1" '$0 ~ pat {f=1} f{print} f&&/^}/{exit}' network_mgr.c; }
check "钩子在 mdns 任务里跑：不拿锁、不阻塞、不调会 IPC 的取句柄" \
    "FN '^int bitengine_mdns_self_ipv4\\\\(esp_netif_t \\\\*netif, esp_ip4_addr_t \\\\*out\\\\)\$' | python3 ../../tools/grep_code.py --absent -q 'xSemaphoreTake|esp_netif_get_handle_from_ifkey|vTaskDelay|esp_event_post|mdns_netif_action' /dev/stdin && FN '^int bitengine_mdns_self_ipv4\\\\(esp_netif_t \\\\*netif, esp_ip4_addr_t \\\\*out\\\\)\$' | grep -q 'taskENTER_CRITICAL(&s_mdns_mux)'"
check "钩子只认 STA / ETH 两个口（SoftAP 照上游）" \
    "FN '^int bitengine_mdns_self_ipv4\\\\(esp_netif_t \\\\*netif, esp_ip4_addr_t \\\\*out\\\\)\$' | grep -q 'netif != s_mdns_sta && netif != s_mdns_eth'"
check "状态机末尾、闸门末尾、复查事件都会更新本机地址" \
    "FN '^void network_state_machine\\\\(' | grep -q 'mdns_self_update();' && FN '^static void svc_gate_work\\\\(' | grep -q 'mdns_self_update();' && awk '/case NET_DEFER_MDNS_RECHECK:/{f=1} f&&/break;/{exit} f' network_mgr.c | grep -q 'mdns_self_update();'"
check "状态机里先 reconcile 浮动、再定本机地址" \
    "FN '^void network_state_machine\\\\(' | awk 'index(\$0, \"float_svc_update();\"){a=NR} index(\$0, \"mdns_self_update();\"){b=NR} END{exit !(a && b && a < b)}'"
check "复查事件进来要拿状态机的锁" \
    "awk '/case NET_DEFER_MDNS_RECHECK:/{f=1} f&&/break;/{exit} f' network_mgr.c | grep -q 'xSemaphoreTakeRecursive(s_sm_mutex'"
check "暂不给（HOLD）时不触发宣告（首次与 2 s 后补的那次都不）" \
    "FN '^static void mdns_self_update\\\\(void\\\\)\$' | awk 'index(\$0, \"if (m != SVC_MDNS_HOLD) {\"){a=NR} index(\$0, \"mdns_self_announce();\"){if(!b)b=NR} END{exit !(a && b && a < b)}' && FN '^static void mdns_self_update\\\\(void\\\\)\$' | grep -q 'if (m != SVC_MDNS_HOLD) mdns_self_announce();'"
check "WiFi 口宣告服务 IP 前要过网段闸门（两口不同网段时给 WiFi 自己的地址）" \
    "FN '^int bitengine_mdns_self_ipv4\\\\(esp_netif_t \\\\*netif, esp_ip4_addr_t \\\\*out\\\\)\$' | grep -q 'svc_same_subnet(lwip_ntohl(a), lwip_ntohl(si.ip.addr), lwip_ntohl(si.netmask.addr))'"
check "WiFi 口地址为 0（服务 IP 刚收回、WiFi 重拿 DHCP）时暂不给，不退回上游（否则宣告 A=0.0.0.0）" \
    "grep -q 'if (!si.ip.addr) return SVC_MDNS_HOLD;' network_mgr.c"
check "以太网\"有地址\"同时要求链路在、链路或上 eth_mgr_link_is_up()" \
    "grep -q 'bool link = s_eth_link_up || eth_mgr_link_is_up();' network_mgr.c && grep -q '.eth_has_ip       = s_eth_has_ip && link,' network_mgr.c"
check "换地址后 2 s 补宣告一次（mdns 队列满会静默丢）" \
    "grep -q 's_mdns_reannounce_us = now + SVC_MDNS_REANNOUNCE_US;' network_mgr.c"
check "判 FIXED 却没有服务 IP 时退回上游（不宣告 0.0.0.0）" \
    "FN '^static void mdns_self_update\\\\(void\\\\)\$' | grep -q 'if (m == SVC_MDNS_FIXED && !a) m = SVC_MDNS_DEFAULT'"
check "模式值与补丁约定的编译期断言在" \
    "grep -q '_Static_assert(SVC_MDNS_DEFAULT == 0 && SVC_MDNS_FIXED == 1 && SVC_MDNS_HOLD == 2' network_mgr.c"
check "等 DHCP 的计时只在链路由断到通时记（别每个 GOT_IP 都刷新）" \
    "grep -q 'if (!s_eth_link_up) s_eth_link_up_us = esp_timer_get_time();' network_mgr.c"
check "init 里缓存两口句柄并发布开机时的结论" \
    "FN '^esp_err_t network_mgr_init\\\\(void\\\\)\$' | grep -q 's_mdns_sta     = wifi_mgr_get_sta_netif();' && FN '^esp_err_t network_mgr_init\\\\(void\\\\)\$' | grep -q 'mdns_self_update();'"
check "Kconfig 依赖浮动 IP、默认开" \
    "awk '/config BITENGINE_MDNS_SVC_IP_ONLY/{f=1} f&&/help/{exit} f' ../../main/Kconfig.projbuild | grep -q 'depends on BITENGINE_WIFI_FLOAT_ETH_IP' && awk '/config BITENGINE_MDNS_SVC_IP_ONLY/{f=1} f&&/help/{exit} f' ../../main/Kconfig.projbuild | grep -q 'default y'"

echo ""
echo "--- T9.37 服务身份：服务地址双向浮动（2026-10-07）---"
# 设计：docs/16_service_identity_dual_float_design_v1.md；行为正本：Kconfig BITENGINE_SVC_IDENTITY 的 help；
# 判据：svc_policy.c 的 svc_id_*（test_host 测）；DHCP client-id 钩子：svc_cid.c + lwip_hooks/bitengine_lwip_hooks.h
GC="python3 ../../tools/grep_code.py"
CFN() { awk -v pat="$1" '$0 ~ pat {f=1} f{print} f&&/^}/{exit}' svc_cid.c; }
check "Kconfig：依赖浮动 IP 与开机接管、默认开" \
    "awk '/config BITENGINE_SVC_IDENTITY\$/{f=1} f&&/help/{exit} f' ../../main/Kconfig.projbuild | grep -q 'depends on BITENGINE_WIFI_FLOAT_ETH_IP && BITENGINE_WIFI_FLOAT_BOOT_TAKEOVER' && awk '/config BITENGINE_SVC_IDENTITY\$/{f=1} f&&/help/{exit} f' ../../main/Kconfig.projbuild | grep -q 'default y'"
check "关掉 IDF 默认的 option 61（否则报文里有两个 client-id）" \
    "grep -q '^CONFIG_LWIP_DHCP_DISABLE_CLIENT_ID=y' ../../sdkconfig.defaults"
check "钩子头文件：重定义 APPEND 宏、没关默认 client-id 就编不过" \
    "grep -q '#undef LWIP_HOOK_DHCP_APPEND_OPTIONS' lwip_hooks/bitengine_lwip_hooks.h && grep -q '#error' lwip_hooks/bitengine_lwip_hooks.h && grep -q 'ESP_DHCP_DISABLE_CLIENT_ID' lwip_hooks/bitengine_lwip_hooks.h"
check "顶层 CMake 把钩子头文件挂进 lwip（同 IDF 示例 vlan_support）" \
    "grep -F 'ESP_IDF_LWIP_HOOK_FILENAME=' ../../CMakeLists.txt | grep -q 'bitengine_lwip_hooks.h' && grep -q 'components/network_mgr/lwip_hooks' ../../CMakeLists.txt"
check "钩子先调 IDF 原来的 dhcp_append_extra_opts（保留 vendor class）" \
    "CFN '^void bitengine_dhcp_append_options\\\\(' | grep -q 'dhcp_append_extra_opts(netif, state, msg, len);'"
check "钩子对所有报文都带 option 61（含 RELEASE / DECLINE；不按状态挑）" \
    "CFN '^void bitengine_dhcp_append_options\\\\(' | $GC -q 'DHCP_OPTION_CLIENT_ID' /dev/stdin && CFN '^void bitengine_dhcp_append_options\\\\(' | $GC 'DHCP_STATE_' /dev/stdin | grep -v '_reqs\\[me\\]++;' | $GC --absent -q 'DHCP_STATE_' /dev/stdin"
check "钩子在 lwIP 线程：只在临界区里拷表，不拿锁、不阻塞" \
    "CFN '^void bitengine_dhcp_append_options\\\\(' | grep -q 'taskENTER_CRITICAL(&s_cid_mux)' && CFN '^void bitengine_dhcp_append_options\\\\(' | $GC --absent -q 'xSemaphoreTake|vTaskDelay|esp_event_post|ESP_LOG' /dev/stdin"
check "钩子装进去之前 / 不认识的口：用本卡 MAC（与 IDF 默认格式一致，现有租约不变）" \
    "CFN '^void bitengine_dhcp_append_options\\\\(' | grep -qF 'svc_cid_t kind = SVC_CID_SELF;' && CFN '^void bitengine_dhcp_append_options\\\\(' | grep -qF 'memcpy(&cid[1], netif->hwaddr, 6);'"
check "交接：先把两张卡设成哑身份，再停 DHCP（RELEASE 对不上租约）" \
    "FN '^static void svc_id_handover\\\\(' | awk 'index(\$0, \"SVC_CID_DUMMY\"){if(!a)a=NR} index(\$0, \"esp_netif_dhcpc_stop(\"){if(!b)b=NR} END{exit !(a && b && a < b)}'"
check "交接：两张卡都停了再起（旧持有者先放下服务地址，新的申请时 ARP 检查没人应答）" \
    "FN '^static void svc_id_handover\\\\(' | awk 'index(\$0, \"esp_netif_dhcpc_stop(\"){s=NR} index(\$0, \"esp_netif_dhcpc_start(\"){if(!t)t=NR} END{exit !(s && t && s < t)}'"
check "身份生效时不做静态浮动（T2.23 只在退回旧行为时用）" \
    "FN '^static void float_svc_update\\\\(void\\\\)\$' | grep -q '!svc_id_on()'"
check "身份生效时开机接管（T9.20）不发起" \
    "FN '^static void svc_boot_load\\\\(void\\\\)\$' | grep -q 'svc_id_on()'"
check "身份记录只在 svc_persist_step 里写（svc_id_store 的调用方只有它）" \
    "[ \$($GC 'svc_id_store\\(' network_mgr.c | grep -v 'static void svc_id_store' | wc -l) -ge 1 ] && [ \$($GC 'svc_id_store\\(' network_mgr.c | grep -v 'static void svc_id_store' | wc -l) -eq \$(FN '^static void svc_persist_step\\\\(' | $GC 'svc_id_store\\(' /dev/stdin | wc -l) ]"
check "判据函数都在 svc_policy.h（纯函数，主机侧可测）" \
    "grep -q 'svc_id_next_holder(' svc_policy.h && grep -q 'svc_id_cid_for(' svc_policy.h && grep -q 'svc_id_judge(' svc_policy.h && grep -q 'svc_id_rec_valid(' svc_policy.h"
check "主机测试覆盖身份判据" \
    "grep -q 'svc_id_next_holder' test_host/svc_policy_test.c && grep -q 'svc_id_judge' test_host/svc_policy_test.c && grep -q 'svc_id_cid_for' test_host/svc_policy_test.c"
check "状态机末尾先定持有者、再 reconcile 浮动与 mDNS" \
    "a=\$(FN '^void network_state_machine\\\\(' | $GC 'svc_id_update\\(\\);' /dev/stdin | head -1 | cut -d: -f2); b=\$(FN '^void network_state_machine\\\\(' | $GC 'float_svc_update\\(\\);' /dev/stdin | head -1 | cut -d: -f2); c=\$(FN '^void network_state_machine\\\\(' | $GC 'mdns_self_update\\(\\);' /dev/stdin | head -1 | cut -d: -f2); [ -n \"\$a\" ] && [ -n \"\$b\" ] && [ -n \"\$c\" ] && [ \$a -lt \$b ] && [ \$b -lt \$c ]"
# ↑ 先剥注释再比行号（grep_code）：用 awk 在原文里找，把调用注释掉照样绿 —— 变异测试抓到过（2026-10-07）
check "拿到地址时按 §7 判（GOT_IP 两张卡都进）" \
    "FN '^void network_state_machine\\\\(' | grep -q 'svc_id_on_got_ip(SVC_NIC_ETH)' && FN '^void network_state_machine\\\\(' | grep -q 'svc_id_on_got_ip(SVC_NIC_WIFI)'"
check "esp_timer 回调禁表里有 svc_id_update / svc_id_handover" \
    "grep -q 'svc_id_update' check_timer_cbs.py && grep -q 'svc_id_handover' check_timer_cbs.py"
check "静态 IP（任一张卡）不参与身份机制" \
    "FN '^static void svc_id_init\\\\(void\\\\)\$' | grep -q 'config_mgr_get_eth()->dhcp' && FN '^static void svc_id_init\\\\(void\\\\)\$' | grep -q 'static_ip'"
check "mDNS 在身份机制下宣告服务地址（A_S）" \
    "FN '^static void mdns_self_update\\\\(void\\\\)\$' | grep -q 'id_active'"
check "/status 露出 svc_identity / svc_ip / svc_holder / svc_id_mode" \
    "grep -q 'svc_identity' ../webui/rest_api.c && grep -q 'svc_holder' ../webui/rest_api.c && grep -q 'svc_id_mode' ../webui/rest_api.c && grep -q 'network_mgr_svc_identity()' ../webui/rest_api.c && grep -q 'network_mgr_svc_ip()' ../webui/rest_api.c"

echo ""
echo "--- T9.37 两轮审计修复（2026-10-07）---"
check "没配 WiFi 时定的 S 不落盘（产线插网线测不配 WiFi）" \
    "FN '^static void svc_id_persist_req\\\\(' | grep -q 'if (s_id_rec.net_ssid == 0)' && grep -q 'if (r->net_ssid == 0) return false;' svc_policy.c"
check "记录属于别的 WiFi 网络：开机按首次安装重定；迁移只认同一网络的快照" \
    "FN '^static void svc_id_init\\\\(void\\\\)\$' | grep -q 'r.net_ssid != ssid_now' && FN '^static void svc_id_init\\\\(void\\\\)\$' | grep -q 'br.ssid_hash == ssid_now'"
check "记录读出错（不是没有）时本次不用身份机制，不当首次安装（免得覆盖好记录）" \
    "FN '^static void svc_id_init\\\\(void\\\\)\$' | grep -q 'err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND'"
check "WiFi 在另一个网络上拿到地址才重定 S（提交凭据那一刻不动：失败会回滚）" \
    "FN '^static void svc_id_on_got_ip\\\\(svc_nic_t nic\\\\)\$' | grep -q 's_id_rec.net_ssid != wifi_cfg_ssid_hash()' && FN '^static void svc_id_on_creds_set\\\\(' | $GC --absent -q 'memset\\(&s_id_rec' /dev/stdin"
check "WiFi 交换着拿不到地址有超时退回（只退本次开机）" \
    "grep -q 'define SVC_ID_SWAP_TIMEOUT_MS' network_mgr.c && FN '^static void svc_id_update\\\\(void\\\\)\$' | grep -q 'svc_id_fallback(false'"
check "两张网（网口在服务网段、WiFi 在别处）时 WiFi 不当持有者" \
    "FN '^static void svc_id_update\\\\(void\\\\)\$' | grep -q 'svc_id_wifi_can_hold('"
check "持有者变了就补判它手上的地址" \
    "FN '^static void svc_id_update\\\\(void\\\\)\$' | grep -q 'svc_id_on_got_ip(nh);'"
check "\"只认 MAC\"只在拿到的正是这张卡自己的地址时判；DECLINE 过的判不了" \
    "grep -q 'if (in->own_ip && in->a == in->own_ip) return SVC_JUDGE_MAC_ONLY;' svc_policy.c && grep -q 'if (in->declined) return SVC_JUDGE_INCONCLUSIVE;' svc_policy.c"
check "钩子记下 DECLINE、交接时清" \
    "grep -q 'msg_type == DHCP_DECLINE' svc_cid.c && FN '^static void svc_id_handover\\\\(' | grep -q 'svc_cid_clear_declined'"
check "钩子给结束符留 1 字节、include 钩子头文件核对签名" \
    "grep -q 'CID_LEN + 1u > DHCP_OPTIONS_LEN' svc_cid.c && grep -q 'lwip_hooks/bitengine_lwip_hooks.h' svc_cid.c"
check "钩子在关掉服务身份的构建里也常驻（与 IDF 默认 client-id 二选一）" \
    "grep -q '^#if ESP_DHCP_DISABLE_CLIENT_ID' lwip_hooks/bitengine_lwip_hooks.h && grep -q '^#if CONFIG_LWIP_DHCP_DISABLE_CLIENT_ID' svc_cid.c"
check "DHCP 意外停在 STOPPED 时补起" \
    "FN '^static void svc_id_update\\\\(void\\\\)\$' | grep -q 'ESP_NETIF_DHCP_STOPPED'"
check "WiFi 换到网口、之前没退过：补发 ONLINE（边界路由跟到网线上），不发 FAILOVER / RECOVERED" \
    "awk '/^static void set_default_route/{f=1} f{print} f&&/^}/{exit}' network_mgr.c | grep -q 'post_online(NET_IF_ETH);'"
check "mDNS 钩子的网段闸门对两个口一视同仁" \
    "FN '^int bitengine_mdns_self_ipv4\\\\(esp_netif_t \\\\*netif, esp_ip4_addr_t \\\\*out\\\\)\$' | $GC --absent -q 'if \\(netif == s_mdns_sta\\)' /dev/stdin"

echo ""
echo "--- T9.37 用户拍板三条（2026-10-07 晚）：恢复出厂保留身份 / S = WiFi 退化时钉在 WiFi / 只认 MAC 7 天后重试 ---"
check "身份记录在 net_id（恢复出厂不擦），写盘只写这里" \
    "grep -q 'define SVC_ID_NVS_NS *\"net_id\"' network_mgr.c && FN '^static void svc_id_store\\\\(void\\\\)\$' | grep -q 'nvs_open(SVC_ID_NVS_NS, NVS_READWRITE'"
check "v2 老位置（net_svc/id）开机读得进来，写进 net_id 后删掉老键" \
    "FN '^static void svc_id_init\\\\(void\\\\)\$' | grep -q 's_id_legacy_key = true' && FN '^static void svc_id_store\\\\(void\\\\)\$' | grep -q 'nvs_erase_key(h, SVC_ID_NVS_KEY)'"
check "开机按 svc_id_rec_load 解析（v2 升 v3），不直接 memcpy 校验" \
    "FN '^static void svc_id_init\\\\(void\\\\)\$' | grep -q 'svc_id_rec_load(buf, len, &r)'"
check "配回记录的那个网络时沿用记录：换网络分支与首次安装分支都先试" \
    "[ \$(FN '^static void svc_id_on_got_ip\\\\(svc_nic_t nic\\\\)\$' | grep -c 'if (svc_id_try_restore())') -ge 2 ]"
check "退化时按 S 分流：S = WiFi 钉在 WiFi 上，其余走旧规则" \
    "FN '^static void svc_id_fallback\\\\(' | grep -q 'svc_id_degrade_mode(' && grep -q 'return s == SVC_NIC_WIFI ? SVC_DEGRADE_PIN_WIFI : SVC_DEGRADE_LEGACY;' svc_policy.c"
check "钉住时身份机制仍算在管（旧规则的静态浮动 / 开机接管不出来顶掉 WiFi 上的服务地址）" \
    "FN '^static bool svc_id_on\\\\(void\\\\)\$' | grep -q 'svc_id_pinned()'"
check "钉住时持有者恒为 WiFi：update 在推进持有者之前先处理、开机直接定 WiFi" \
    "FN '^static void svc_id_update\\\\(void\\\\)\$' | grep -q 'if (svc_id_pinned())' && FN '^static void svc_id_init\\\\(void\\\\)\$' | grep -q 'if (svc_id_pinned())'"
check "/status 能分出钉住（wifi_pinned）" \
    "FN '^const char \\\\*network_mgr_svc_holder\\\\(void\\\\)\$' | grep -q 'wifi_pinned'"
check "只认 MAC 到期重试只在开机做（运行中只补记开始时刻）" \
    "FN '^static void svc_id_init\\\\(void\\\\)\$' | grep -q 'case SVC_MACONLY_RETRY:' && FN '^static void svc_id_mac_only_stamp\\\\(void\\\\)\$' | $GC --absent -q 'SVC_MACONLY_RETRY' /dev/stdin"
check "记下只认 MAC 时墙钟可信才计时；对上时后补记（SNTP 回调只投递）" \
    "FN '^static void svc_id_fallback\\\\(' | grep -q 'svc_clock_trusted()' && FN '^static void svc_id_on_time_synced\\\\(void\\\\)\$' | grep -q 'esp_event_post(NET_DEFER_EVENT, NET_DEFER_SVC_ID'"
check "mode 退回 unknown 的每一处都清 mode_ts（否则记录校验不过、写不进 NVS）" \
    "[ \$($GC 's_id_rec.mode = SVC_IDMODE_UNKNOWN;' network_mgr.c | wc -l) -eq \$($GC 's_id_rec.mode_ts = 0;' network_mgr.c | wc -l) ]"

echo ""
echo "--- T9.37 隔离网段实测修复（2026-10-08）：开机时 DHCP 先于身份表起来 ---"
check "钩子记下每个 REQUEST 实际带的身份" \
    "grep -A1 'if (msg_type == DHCP_REQUEST && me != SVC_NIC_NONE) {' svc_cid.c | grep -q 's_sent\[me\] = (uint8_t)used;'"
check "判定按实际发出去的身份（svc_cid_sent），不按表；哑身份拿到的不算" \
    "FN '^static void svc_id_on_got_ip\\\\(svc_nic_t nic\\\\)\$' | grep -q 'svc_cid_t sent = svc_cid_sent(nic);' && FN '^static void svc_id_on_got_ip\\\\(svc_nic_t nic\\\\)\$' | grep -q 'if (sent == SVC_CID_DUMMY) return;' && FN '^static void svc_id_on_got_ip\\\\(svc_nic_t nic\\\\)\$' | $GC --absent -q 'swapped = \(svc_cid_get\(nic\)' /dev/stdin"
check "开机设身份表走交接（已经起来的 DHCP 要按哑身份→停→换→起重来），不直接改表" \
    "FN '^static void svc_id_init\\\\(void\\\\)\$' | grep -q 'svc_id_handover(svc_id_cid_for(SVC_NIC_ETH' && FN '^static void svc_id_init\\\\(void\\\\)\$' | $GC --absent -q 'svc_cid_set\(' /dev/stdin"

check "钩子用缓存的对方 MAC（开机时对方 netif 的 hwaddr 可能还是空的），不读对方 lwIP netif" \
    "grep -q 'esp_read_mac(ms, ESP_MAC_WIFI_STA)' svc_cid.c && grep -q 'esp_netif_get_mac(eth, me)' svc_cid.c && $GC --absent -q 'peer->hwaddr' svc_cid.c"
check "记下的是实际带出去的身份（对方 MAC 不知道、退成本卡时记 SELF）" \
    "grep -q 's_sent\[me\] = (uint8_t)used;' svc_cid.c && grep -q 'used = SVC_CID_SELF;' svc_cid.c"

check "诊断：钩子数续租 / 重绑请求；DHCP 状态只在 lwIP 线程里读（esp_netif_tcpip_exec）" \
    "grep -q 'state == DHCP_STATE_RENEWING)  s_renew_reqs\[me\]++' svc_cid.c && grep -q 'state == DHCP_STATE_REBINDING) s_rebind_reqs\[me\]++' svc_cid.c && FN '^int network_mgr_diag_dhcp_json\\\\(' | grep -q 'esp_netif_tcpip_exec(svc_dhcp_read_in_tcpip' && FN '^int network_mgr_diag_dhcp_json\\\\(' | $GC --absent -q 'netif_dhcp_data' /dev/stdin"

echo ""
echo "--- T9.40 插着网线配 WiFi：网口在线就留在 ETH_ONLINE、不宣布离线（2026-10-08 真板）---"
# 以前配网一律切 WIFI_CONNECTING；新旧凭据都连不上时重连耗尽 → no_network_fallback 不看网口 → OFFLINE + NO_NETWORK，
# SYS 灯蓝闪、NET 灯红，WiFi 后来连上进 BOTH_ONLINE 又不补 ONLINE，灯一直不回来。
check "no_network_fallback 先看网口：有地址回 ETH_ONLINE，判在 post_offline 之前" \
    "FN '^static net_state_t no_network_fallback\\\\(void\\\\)\$' | $GC 'if \(s_eth_has_ip\)|post_offline\(\)' /dev/stdin | head -1 | grep -q 's_eth_has_ip' && FN '^static net_state_t no_network_fallback\\\\(void\\\\)\$' | $GC -q 'return NET_STATE_ETH_ONLINE;' /dev/stdin"
check "配网开始：网口有地址时主状态留 ETH_ONLINE" \
    "awk '/if \(event == NET_EVT_WIFI_CREDENTIALS_SET\)/{f=1} f{print} f&&/return;/{exit}' network_mgr.c | $GC -q 's_state *= s_eth_has_ip \? NET_STATE_ETH_ONLINE : NET_STATE_WIFI_CONNECTING;' /dev/stdin"
check "ETH_ONLINE 收口配网：断开走 prov_on_disconnect、GOT_IP 置 PROV_OK" \
    "awk '/^    case NET_STATE_ETH_ONLINE:\$/,/^        break;/' network_mgr.c | $GC -q 'prov_on_disconnect\(\)' /dev/stdin && awk '/^    case NET_STATE_ETH_ONLINE:\$/,/^        break;/' network_mgr.c | $GC -q 's_prov_state *= PROV_OK' /dev/stdin"
check "配网判据只有一份（auth_bad 只在 prov_on_disconnect 里算）" \
    "[ \$($GC 'bool auth_bad' network_mgr.c | wc -l) -eq 1 ] && FN '^static prov_step_t prov_on_disconnect\\\\(void\\\\)\$' | $GC -q 'bool auth_bad' /dev/stdin"
check "配网失败又无旧凭据（hold）时 ETH_ONLINE 不排后台重试；重填凭据时清 hold" \
    "awk '/^    case NET_STATE_ETH_ONLINE:\$/,/^        break;/' network_mgr.c | $GC -q 'else if \(!s_wifi_prov_hold\)' /dev/stdin && awk '/if \(event == NET_EVT_WIFI_CREDENTIALS_SET\)/{f=1} f{print} f&&/return;/{exit}' network_mgr.c | $GC -q 's_wifi_prov_hold *= false;' /dev/stdin"
check "网口断链在 switch 之前统一清 s_eth_has_ip" \
    "awk '/^void network_state_machine\(net_event_t event\)/{f=1} f&&/switch \(s_state\)/{exit} f{print}' network_mgr.c | $GC -q 'if \(event == NET_EVT_ETH_LINK_DOWN\) s_eth_has_ip = false;' /dev/stdin"

check "ETH_ONLINE 里 WiFi 连上就关热点（网口在线时手动开热点配网，审计 ❌）" \
    "awk '/^    case NET_STATE_ETH_ONLINE:\$/,/^        break;/' network_mgr.c | $GC -q 'wifi_mgr_stop_softap\(\)' /dev/stdin"
check "退回旧凭据后 ETH_ONLINE 快速重连、处理快速重连耗尽" \
    "awk '/^    case NET_STATE_ETH_ONLINE:\$/,/^        break;/' network_mgr.c | $GC -q 'else if \(s_wifi_fast_after_rollback\)' /dev/stdin && awk '/^    case NET_STATE_ETH_ONLINE:\$/,/^        break;/' network_mgr.c | $GC -q 'event == NET_EVT_WIFI_RECONNECT_EXHAUSTED' /dev/stdin"
# ⚠️ 范围必须收到 GAVE_UP 那个 case：同一状态块里 GOT_IP 分支也有 if (s_softap_on)，按整块取第一处匹配恒绿（变异测试抓到）
check "配网放弃时只有热点在才落 WIFI_PROVISION_FAILED（没热点没网口 = 死路）" \
    "[ \"\$(awk '/^    case NET_STATE_WIFI_CONNECTING:\$/,/^        break;/' network_mgr.c | awk '/case PROV_STEP_GAVE_UP:\$/,/^                    break;/' | $GC 'if \(s_softap_on\)|NET_STATE_WIFI_PROVISION_FAILED|no_network_fallback\(\)' /dev/stdin | sed -E 's/.*(s_softap_on|PROVISION_FAILED|no_network_fallback).*/\1/' | tr '\n' ' ')\" = 's_softap_on PROVISION_FAILED no_network_fallback ' ]"


# ---- 纯以太网剖面（docs/15 设计 2.2 / 14.2，只在直连型里能开）----
# 剖面打开时 esp_wifi_* / WIFI_EVENT / 共存调用只许在 #if !CONFIG_BITENGINE_NET_ETH_ONLY 里，CMake 换成 wifi_none.c；
# 判据正本在 tools/check_profile_rules.py（--self-test 有变异）。链接面（Wi-Fi 库 0 字节）看产物：tools/check_profiles.py
# 以太网 GOT_IP 时链路本地地址已可用就别重建：lwIP 重建会打回 TENTATIVE 重做 DAD，期间 OT 的 RS 等全失败、
# HA → Thread 的下一跳不应答（服务身份交接每次开机都会多来一次 GOT_IP，2026-10-22 上板查实）
check "ETH GOT_IP 先查链路本地地址是否已可用，可用就不重建" "python3 - <<'PY'
import re,sys
src=open('net_events.c',encoding='utf-8').read()
g=src.find('ETH got IP'); blk=src[g:src.find('network_state_machine(NET_EVT_ETH_GOT_IP)', g)]
a=blk.find('esp_netif_get_ip6_linklocal(eth_nif'); b=blk.find('esp_netif_create_ip6_linklocal(eth_nif')
sys.exit(0 if 0<=a<b and 'else if' in blk[a-40:a] else 1)
PY"
# 直连型开机不等网，SNTP 可能先于网络起来 → 网到时要重启一次，否则退避一两分钟才对时（2026-10-21 上板实测 133 s）
check "直连型 NETWORK_EVENT_ONLINE 时重启 SNTP（只在直连型）" "python3 - <<'PY'
import re,sys,subprocess
src=open('../../main/main.c',encoding='utf-8').read()
h=src[src.find('static void network_event_handler'):]
h=h[:h.find('\\n}\\n')]
on=h[h.find('NETWORK_EVENT_ONLINE'):h.find('NETWORK_EVENT_OFFLINE')]
m=re.search(r'#if CONFIG_BITENGINE_PROFILE_DIRECT\\s*\\n(?:\\s*/\\*.*?\\*/\\s*\\n)?\\s*esp_sntp_restart\\(\\);\\s*\\n\\s*#endif', on, re.S)
sys.exit(0 if m else 1)
PY"
check "纯以太网剖面：Wi-Fi 引用都在 #if !CONFIG_BITENGINE_NET_ETH_ONLY 里" "python3 ../../tools/check_profile_rules.py --only eth"
check "wifi_none.c 覆盖 wifi_mgr.h / wifi_probe.h 的每个函数" "python3 - <<'PY'
import re,sys
hdr=open('wifi_mgr.h').read()+open('include/wifi_probe.h').read()
api=set(re.findall(r'^[a-z_][\\w \\*]*?\\b((?:wifi_mgr|wifi_probe)_\\w+)\\s*\\(', hdr, re.M))
impl=set(re.findall(r'\\b((?:wifi_mgr|wifi_probe)_\\w+)\\s*\\(', open('wifi_none.c').read()))
miss=sorted(api-impl); print(miss); sys.exit(1 if miss or len(api)<20 else 0)
PY"

echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
