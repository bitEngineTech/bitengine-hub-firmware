#!/bin/bash
# components/thread_br/VERIFY.sh
# 编译通过后运行：bash components/thread_br/VERIFY.sh
#
# 重点是"信道"这条线：配置值与实际值必须分开，且建网时要躲开 Zigbee。
# 历史病根：只有一个 thread_br_get_channel() 返回 NVS 配置值，却被
# /node/diagnostics 当作"实际运行信道"报出去 —— 信道迁移过的板子一直报旧值。

PASS=0; FAIL=0
check() {
    if eval "$2" > /dev/null 2>&1; then
        echo "✅ $1"; ((PASS++))
    else
        echo "❌ $1"; ((FAIL++))
    fi
}

cd "$(dirname "$0")" || exit 1

# 函数体（剥注释后）里 PAT1 出现在 PAT2 之前，且相隔不超过 MAXGAP 行（默认不限）。
# 函数用定义行锚定（行尾是 ")"），不会撞上前向声明。
in_order() {  # in_order <函数名> <PAT1> <PAT2> [MAXGAP]
    awk -v fn="$1" '$0 ~ "^static [a-z_ ]*" fn "\\(.*\\)$" {f=1} f&&/^}/{exit} f' thread_br.c > /tmp/.verify_tbr_fn.$$ || return 1
    local a b
    a=$(python3 ../../tools/grep_code.py "$2" /tmp/.verify_tbr_fn.$$ | head -1 | cut -d: -f2)
    b=$(python3 ../../tools/grep_code.py "$3" /tmp/.verify_tbr_fn.$$ | head -1 | cut -d: -f2)
    rm -f /tmp/.verify_tbr_fn.$$
    [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ] && [ $((b - a)) -le "${4:-100000}" ]
}

echo "--- 信道 API：配置值与实际值必须是两个名字 ---"
# 只在私有检出里有的内部文档 / 工具（docs/、发布与产线脚本）：公开仓库不导出它们（tools/oss_export.py），
# 那里跳过而不是报红；私有检出里照常判。标志是 docs/开发索引.md 在不在。
internal() { if [ -f ../../docs/开发索引.md ]; then check "$@"; else echo "⏭  $1 —— 依赖内部文档 / 工具，本检出没有，跳过"; fi; }

check "configured 版存在"          "grep -q 'thread_br_get_configured_channel' include/thread_br.h"
check "active 版存在且返回 esp_err_t" "grep -q 'esp_err_t thread_br_get_active_channel' include/thread_br.h"
# 只匹配声明/定义，不匹配散文——头文件注释里要保留这段历史说明。
check "含糊的旧名字已消失"          "! grep -rqE '^(uint8_t|esp_err_t).*thread_br_get_channel *\(' *.c include/*.h"
check "active 真读 otLinkGetChannel" "grep -q 'otLinkGetChannel(inst)' thread_br.c"
# 调用方里有单任务 httpd：OT 忙时 portMAX_DELAY 会把整个 WebUI 拖死。
check "取锁带超时不用 portMAX_DELAY" "awk '/^esp_err_t thread_br_get_active_channel/,/^}/' thread_br.c | grep -q 'pdMS_TO_TICKS'"
check "取锁失败有返回路径"          "awk '/^esp_err_t thread_br_get_active_channel/,/^}/' thread_br.c | grep -q 'ESP_ERR_TIMEOUT'"
check "未运行时明确拒绝"            "awk '/^esp_err_t thread_br_get_active_channel/,/^}/' thread_br.c | grep -q 'ESP_ERR_INVALID_STATE'"
# OpenThread 没编进来时没有 otLink 可读，必须说"不支持"而不是拿配置值冒充。
check "stub 分支不拿配置值冒充"      "grep -q 'thread_br_get_active_channel(uint8_t \*out) { (void)out; return ESP_ERR_NOT_SUPPORTED' thread_br.c"

echo ""
echo "--- 共存：建网时躲开 Zigbee ---"
check "阈值常量只此一份"            "[ \$(grep -c 'define COEX_TOO_CLOSE_MAX' include/thread_br.h) -eq 1 ]"
check "挑信道函数已导出"            "grep -q 'thread_br_pick_channel' include/thread_br.h"
check "挑信道是纯函数（不在条件编译里）" "awk '/^uint8_t thread_br_pick_channel/,/^}/' thread_br_channel.c | grep -q 'k_wifi_gap_channels'"
check "WiFi 空隙候选集 = {15,20,25}" "grep -q 'k_wifi_gap_channels\[\] = { 15, 20, 25 }' thread_br_channel.c"
check "ch26 被排除"                 "grep -q 'THREAD_CH_MAX 25' thread_br_channel.c"
check "Zigbee 未知时不改出厂行为"    "awk '/^uint8_t thread_br_pick_channel/,/^}/' thread_br_channel.c | grep -q 'return configured_channel'"
check "二级放宽有 W 级日志"          "awk '/^uint8_t thread_br_pick_channel/,/^}/' thread_br_channel.c | grep -q 'ESP_LOGW'"
check "建网时真的调用了它"           "grep -q 'thread_br_pick_channel(zb_known' thread_br.c"
check "建网用的是用户声明值"         "grep -q 'zb->channel_declared' thread_br.c"

echo ""
echo "--- 共存判定：规则只有一处实现 ---"
check "coex_status_t 已定义"        "grep -q 'coex_status_t' include/thread_br.h"
check "get_coex_status 已导出"      "grep -q 'void thread_br_get_coex_status' include/thread_br.h"
check "待定信道判定已导出"          "grep -q 'bool thread_br_channel_too_close' include/thread_br.h"
# 两边都必须是实测值才给结论；拿配置值凑数正是要修的病。
check "两边都要已知才下结论"        "awk '/^void thread_br_get_coex_status/,/^}/' thread_br_channel.c | grep -q 'zigbee_declared_channel'"
check "未知不等于有问题"            "awk '/^bool thread_br_channel_too_close/,/^}/' thread_br_channel.c | grep -q 'return false'"
# Zigbee 信道只有"用户声明"这一个来源；不许再从 ncp_version 拿（读不到）。
check "不依赖 ncp_version"          "! grep -rq 'ncp_netparams' *.c"
check "未声明即未知"                "awk '/^static bool zigbee_declared_channel/,/^}/' thread_br_channel.c | grep -q 'channel_declared'"

echo ""
echo "--- 开机重选：只动空网络 ---"
check "should_repick 已导出"        "grep -q 'bool thread_br_should_repick' include/thread_br.h"
# had_child 是否决票，必须排在最前面且无条件返回 —— 写反会把用户正在用的网络搬走。
check "had_child 是第一道否决"      "awk '/^bool thread_br_should_repick/,/^}/' thread_br_channel.c | grep -m1 'return false' | grep -q 'had_child'"
check "重选后不等于原信道"          "awk '/^bool thread_br_should_repick/,/^}/' thread_br_channel.c | grep -q 'picked == current_thread_channel'"
check "开机分支真的调用了它"        "grep -q 'thread_br_should_repick(cur, had_child' thread_br.c"
# 只改信道：network key / PAN ID / ExtPanId 必须原样保留，否则等于换了个网络。
check "只改 mChannel 不重建网络"    "! awk '/thread_br_should_repick\(cur/,/^    }$/' thread_br.c | grep -q 'otDatasetCreateNewNetwork'"
check "时间戳递增"                  "awk '/thread_br_should_repick\(cur/,/^    }$/' thread_br.c | grep -q 'mActiveTimestamp.mSeconds++'"
check "粘性标记落 NVS"              "grep -q 'TBR_NVS_HAD_CHILD' thread_br.c"
# 缓存必须三态。用 bool（初值 false）冒充"未读"时，建新 dataset 路径的
# store(false) 会被幂等判断短路，NVS 里上个网络残留的 1 永远清不掉。
check "缓存是三态不是 bool"         "grep -q 'int8_t s_had_child_cache = -1' thread_br.c"
check "写之前先对齐缓存"            "awk '/^static void had_child_store/,/^}/' thread_br.c | grep -q 's_had_child_cache < 0) had_child_load'"
check "有子节点即置位（用 total）"   "awk '/^static void child_stats_timer_cb/,/^}/' thread_br.c | grep -q 'total > 0) had_child_store(true)'"
check "建新网络时清零"              "grep -q 'had_child_store(false)' thread_br.c"

echo ""
echo "--- 声明值过期检测（PAN 精确匹配）---"
check "PCAP 回调已挂"              "grep -q 'otLinkSetPcapCallback' thread_br.c"
# OT 的扫描结果解析器要求源地址是 64 位（scan_result.cpp:50），而 Zigbee 协调器
# 用 16 位短地址发 beacon —— 不绕过它就永远看不到自家网络。
check "自己解析短地址 beacon"      "grep -q 'src_extended' thread_br.c"
check "全帧计数用于排除歧义"       "grep -q 'pcap_rx_total' thread_br.c"
# 没有下限时会把 -97dBm 的邻居当成自家 PAN 存下（2026-08-12 实测踩过）。
check "自家网络有 RSSI 门限"       "grep -q 'ZB_OWN_MIN_RSSI' thread_br.c"
check "门限也用于 find_strongest"  "awk '/^esp_err_t thread_br_find_strongest/,/^}/' thread_br.c | grep -q 'ZB_OWN_MIN_RSSI'"
# 自家 beacon 不是每轮都抓得到，单轮判定会时对时错。
check "多轮探测"                   "grep -q 'ZB_PROBE_ROUNDS' thread_br.c"
# 2026-09-15：窄扫以前只跑一轮，灵敏度严格低于 probe_pan，是 not_found 误报的
# 最大来源。这两条钉住"窄扫和 probe_pan 一样多轮"与"窄扫只扫声明的那个信道"。
check "窄扫也多轮"                 "awk '/^static uint8_t narrow_find_pan/,/^}/' thread_br.c | grep -q 'ZB_PROBE_ROUNDS'"
check "先窄后宽（省稳态开销）"     "awk '/^static uint8_t narrow_find_pan/,/^}/' thread_br.c | grep -q '1u << channel'"
# 单轮落空更可能是射频被 WiFi 抢占，不是网络没了。not_found 必须由连续落空得出，
# 且落空期间要快速重试——否则"连续 N 轮"会把真故障拖成 N × 6 小时才报出来。
check "单轮落空不下结论"           "awk '/^static uint32_t zigbee_check_once\\(void\\)\$/,/^}\$/' thread_br.c | python3 ../../tools/grep_code.py -q '\\+\\+s_zb_miss_streak < ZB_MISS_STREAK' /dev/stdin"
check "落空不覆盖上一轮结论"       "awk '/^static uint32_t zigbee_check_once\\(void\\)\$/,/^}\$/' thread_br.c | grep -A3 'miss_streak < ZB_MISS_STREAK' | python3 ../../tools/grep_code.py --absent -q 's_zb_check *=' /dev/stdin"
check "落空后快速重试"             "awk '/^static uint32_t zigbee_check_once\\(void\\)\$/,/^}\$/' thread_br.c | python3 ../../tools/grep_code.py -q 'return ZB_RETRY_INTERVAL_S' /dev/stdin"
# not_found 成立后不能退回 6 小时：设置页在已声明时不给信道输入框，用户没有
# "立即重测"可点，挂错了就得挂满一个周期。
check "not_found 后加密复查"       "awk '/^static uint32_t zigbee_check_once\\(void\\)\$/,/^}\$/' thread_br.c | python3 ../../tools/grep_code.py -q 'return ZB_NOTFOUND_INTERVAL_S' /dev/stdin"
check "复查节奏明显快于常规周期"   "python3 -c \"
import re,sys
s=open('thread_br.c',encoding='utf-8').read()
g=lambda n: eval(re.search(r'#define '+n+r' +(\\(?[0-9 *]+\\)?)',s).group(1))
sys.exit(0 if g('ZB_RETRY_INTERVAL_S') < g('ZB_NOTFOUND_INTERVAL_S') < g('ZB_CHECK_INTERVAL_S') else 1)\""
# 2026-08-17 起栈走 PSRAM，创建函数从 xTaskCreate 换成 psram_task_create，
# 断言放宽到"两者之一"，别再因为换了创建方式就红。
check "检测跑在独立任务"           "grep -qE '(xTaskCreate|psram_task_create)\(zigbee_check_task' thread_br.c"

echo ""
echo "--- 主机侧测试 ---"
check "test_host 存在"             "[ -f test_host/run.sh ]"
check "信道规则有常驻测试"          "[ -f test_host/channel_test.c ]"
check "测试编的是真源码不是副本"    "grep -q 'thread_br_channel.c' test_host/run.sh"
check "主机测试通过"                "bash test_host/run.sh"

echo ""
echo "--- 依赖 ---"
check "CMake 声明了 config_mgr"     "grep -q 'config_mgr' CMakeLists.txt"
check "包含 config_mgr.h"          "grep -q '#include \"config_mgr.h\"' thread_br_channel.c"

echo ""
echo ""
echo "--- zb_check 栈在 PSRAM，其前提是这条路径不碰 flash/NVS（2026-08-17）---"
# psram_task.h 规则 1：cache 关闭期间访问 PSRAM 栈 = 随机崩溃。
# 很自然的下一个需求是"把实测到的 Zigbee 信道记进 NVS"，那会直接踩中。
check "zb_check 用 psram_task_create" "grep -q 'psram_task_create(zigbee_check_task' thread_br.c"
check "zigbee_check_once 不碰 NVS"    "! awk '/^static .*zigbee_check_once/,/^}/' thread_br.c | grep -qE '(nvs_|esp_partition_)[a-z0-9_]*\\('"
check "narrow_find_pan 不碰 NVS"     "! awk '/^static uint8_t narrow_find_pan/,/^}/' thread_br.c | grep -qE '(nvs_|esp_partition_)[a-z0-9_]*\\('"
check "active_scan 不碰 NVS"          "! awk '/thread_br_active_scan\\(/,/^}/' thread_br.c | grep -qE '(nvs_|esp_partition_)[a-z0-9_]*\\('"

echo ""
echo "--- T3.3 Thread 关得掉（改动必须活过重启）---"
# 这是**真 bug**不是没露出：PUT /node/state 以前只改运行时状态，不落盘。
# 用户点 Disable，协议栈确实停了，但重启后 thread_br_init() 照着 cfg->enabled=true
# 又拉起来 —— 以为关掉了，它自己回来了，且没有任何提示。
# ⚠️ 这几条一律按**函数体**判，不用 grep -A<n> 猜距离 —— 那段注释加代码有三十
#    多行，猜短了就误红（本 VERIFY 第一版三条全红，而代码本来是对的；
#    device_identity / config_mgr 那边已经栽过同样的跤）。
check "PUT /node/state 落盘且覆盖两个方向" "python3 ./check_state_persist.py"
check "有 /api/v1/thread/config 端点" "grep -q 'thread/config' ../webui/rest_api.c"
check "config 端点挂了跨站守卫"      "grep -A4 'static esp_err_t handle_thread_config_set' ../webui/rest_api.c | grep -q 'CROSS_ORIGIN_GUARD'"
# ⚠️ channel 只对尚未组网的初始 dataset 生效 —— 回包必须说清楚，
#    否则用户改了以为生效了，又是一次"存了 ≠ 生效"
check "回包说明 channel 的适用范围"  "grep -q 'not-yet-formed network' ../webui/rest_api.c"
check "回包说明 enabled 何时生效"    "grep -q 'takes_effect.*reboot' ../webui/rest_api.c"
check "thread_br_init 仍尊重 enabled" "grep -q 'cfg->enabled' thread_br.c"

echo ""
echo "--- T1.4 射频发射功率旋钮（2026-09-11）---"
# ⚠️ 这是**第三个射频**：WiFi 在 sdkconfig、Zigbee 在 MG24（网关管不到），
#    Thread 的 802.15.4 在这里。它此前从来没人设过也没人读过 —— IDF 把 16 个
#    信道全填成功率表末项，OT 正常路径不调 otPlatRadioSetTransmitPower。
#    认证按"能发的最大"测，所以这行调用消失 = 认证口径静默失效。
check "thread_br 调 apply_tx_power"   "python3 ../../tools/grep_code.py -q 'apply_tx_power\(\)' thread_br.c"
# 顺序：必须在 esp_openthread_init 之后 —— 之前调时 PIB 还没初始化，写进去会被
# 随后的 pib_init 覆盖回满功率，且不报错。
check "apply_tx_power 在 OT init 之后" "python3 ./check_tx_power_order.py"
# 日志行必须在 #ifdef 之外：关着开关时它打印的就是本平台功率表的上限，
# 而那个数字在别处没有任何出口（get_txpower 回显的是请求值，覆盖后问不出原值）。
check "功率日志不在 ifdef 里"          "python3 ./check_tx_power_order.py --log-unconditional"
check "Kconfig 有 TX power 旋钮"       "grep -q 'BITENGINE_THREAD_TX_POWER_OVERRIDE' ../../main/Kconfig.projbuild"
check "覆盖失败走 ESP_LOGE"            "awk '/^static void apply_tx_power/,/^}/' thread_br.c | grep -q 'ESP_LOGE'"
# WiFi 功率只有 sdkconfig 一个出处。多一个运行时写入点 = "配置说 A、实际发 B"，
# 这种偏差在认证报告里是致命的。⚠️ 用 grep_code 而不是裸 grep：解释"为什么不调
# 它"的注释正是好代码该有的，裸 grep 会被自己的注释绊倒（CLAUDE.md 7.4）。
check "固件不调 set_max_tx_power"      "python3 ../../tools/grep_code.py --absent -q 'esp_wifi_set_max_tx_power' ../network_mgr/*.c ../../main/*.c thread_br.c"
check "sdkconfig 显式写了 WiFi 功率"   "grep -q '^CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=' ../../sdkconfig.defaults"
internal "有认证文档"                     "[ -f ../../docs/hardware/射频功率与认证.md ]"

echo ""
echo "--- T2.20 pdMS_TO_TICKS 溢出（2026-09-14）---"
# 宏先乘 tick 率再除，1000 Hz 下实参超过 4,294 s 就溢出：6 小时算出来是 125 s。
# 反向不变量用 grep_code（剥掉注释再匹配，否则解释这件事的注释就把它绊红了）。
check "pdMS_TO_TICKS 实参不含小时级常量"  "python3 ../../tools/grep_code.py --absent -q 'pdMS_TO_TICKS\([^)]*(3600|_S \* 1000|\* 60 \* 60)' thread_br.c"
# 2026-09-15 起睡多久由上一轮结论决定，切片睡收进 zb_sleep_seconds()，
# 判据跟着搬 —— 钉的是"给 pdMS_TO_TICKS 的实参永远是 1000"这件事本身。
check "信道检查按秒切片睡"               "awk '/^static void zb_sleep_seconds/,/^}/' thread_br.c | grep -q 'pdMS_TO_TICKS(1000)'"
check "检查任务只经切片睡等待"           "awk '/^static void zigbee_check_task\\(void \\*arg\\)\$/,/^}\$/' thread_br.c | python3 ../../tools/grep_code.py --absent -q 'pdMS_TO_TICKS\\([^)]*INTERVAL_S' /dev/stdin"

echo ""
echo "--- 声明信道后立刻刷新结论（2026-09-15，跨侧契约）---"
# 声明时本来就在该信道上探过一次 PAN，不刷新的话横幅挂着旧 not_found 干等 6 小时。
check "seed API 存在"                    "grep -q 'void thread_br_zigbee_check_seed' include/thread_br.h"
check "两种构型都实现了 seed"            "[ \$(grep -c '^void thread_br_zigbee_check_seed' thread_br.c) -eq 2 ]"
check "探不到 PAN 时作废旧结论"          "awk '/^void thread_br_zigbee_check_seed/,/^}/' thread_br.c | grep -q 'memset(&s_zb_check'"
check "声明信道的 handler 调用了它"      "grep -q 'thread_br_zigbee_check_seed' ../webui/rest_api.c"

# ---- OT CLI 闸门（2026-09-15，射频实验室用）----
# 三条钉的是"出厂关得掉"这件事本身，任一条红都意味着 diag 入口可能跟着出厂固件走。
check "OT CLI 出厂默认关"        "grep -A2 'config BITENGINE_ENABLE_OT_CLI' ../../main/Kconfig.projbuild | grep -q 'default n'"
check "OT CLI 调用点有编译期守卫" "grep -B1 'esp_openthread_cli_init()' thread_br.c | grep -q 'CONFIG_BITENGINE_ENABLE_OT_CLI\|s_cli_started'"
# IDF 没有 CLI 的 deinit，而 thread_br_start() 在 stop 之后可能再被调用（失败路径、配网结束后的延迟启动）——
# 少了一次性守卫，Thread 每重启一次就多一个 ot_cli 任务抢同一个 stdin。
check "OT CLI 只建一次（有一次性守卫）" "grep -q 's_cli_started = true' thread_br.c && grep -q 'if (!s_cli_started)' thread_br.c"
# 反向：CLI 绝不能出现在 #if 之外（grep_code 剥注释，否则上面那段解释自己会绊红）
check "sdkconfig 里 OT CLI 是关的"  "grep -q '# CONFIG_BITENGINE_ENABLE_OT_CLI is not set' ../../sdkconfig"

echo ""; echo "--- 功率按地区（docs/14 第 7.5 节，2026-09-15）---"
check "apply_tx_power 读 sys.radio_region"       "python3 ../../tools/grep_code.py -q 'sys->radio_region' thread_br.c"
check "四档常量 CE10/FCC20/UNKNOWN8/UNSET10"      "grep -q 'THREAD_TXP_CE_DBM *10' thread_br.c && grep -q 'THREAD_TXP_FCC_DBM *20' thread_br.c && grep -q 'THREAD_TXP_UNKNOWN_DBM *8' thread_br.c && grep -q 'THREAD_TXP_UNSET_DBM *10' thread_br.c"
check "Kconfig 覆盖仍压过地区（实验室用）"        "grep -B1 'const char \*why = \"Kconfig override\"' thread_br.c | grep -q 'CONFIG_BITENGINE_THREAD_TX_POWER_DBM'"
check "订阅 CONFIG_EVENT_RADIO_REGION_CHANGED"    "python3 ../../tools/grep_code.py -q 'esp_event_handler_register\(CONFIG_EVENT, CONFIG_EVENT_RADIO_REGION_CHANGED' thread_br.c"
check "事件里只在 s_running 时重设"               "python3 ../../tools/grep_code.py -q 'id == CONFIG_EVENT_RADIO_REGION_CHANGED && s_running' thread_br.c"
check "成功后才记 s_tx_power_dbm"                 "grep -B2 's_tx_power_dbm = want;' thread_br.c | grep -q 'else'"
check "编译掉 OpenThread 时两个 getter 有桩"      "grep -A4 'CONFIG_OPENTHREAD_ENABLED not set' thread_br.c | grep -q 'thread_br_get_tx_power_dbm'"

echo ""; echo "--- 上行切换时边界路由跟着换口（2026-10-05，测试 6c / 实验 1、1b）---"
check "选口只有一张候选表，开机与跟随共用"       "[ \$(python3 ../../tools/grep_code.py 'pick_backbone\(' thread_br.c | wc -l) -ge 3 ] && [ \$(grep -c 'BACKBONE_CANDIDATES\[\] = {' thread_br.c) -eq 1 ]"
check "候选顺序：以太网优先于 WiFi"             "awk '/BACKBONE_CANDIDATES\[\] = \{/{f=1} f' thread_br.c | head -3 | grep -n 'ETH_DEF' | grep -q '^2:'"
check "FAILOVER/RECOVERED/ONLINE 触发跟随"      "awk '/static void net_event_handler/{f=1} f' thread_br.c | head -12 | grep -q 'NETWORK_EVENT_FAILOVER' && awk '/static void net_event_handler/{f=1} f' thread_br.c | head -12 | grep -q 'NETWORK_EVENT_RECOVERED' && awk '/static void net_event_handler/{f=1} f' thread_br.c | head -12 | python3 ../../tools/grep_code.py -q 'backbone_follow_kick\(\)' /dev/stdin"
check "事件回调里不直接换口（只打标记）"         "awk '/static void net_event_handler/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py --absent -q 'thread_br_switch_backbone\(' /dev/stdin"
check "跟随前等开机的 border_router_init 做完"  "in_order backbone_follow_timer_cb 'if \\(!s_br_init_done\\) return;' 'pick_backbone\\('"
check "开机 border_router_init 做完后补踢一次跟随" "in_order ot_br_init_task 's_br_init_done = true;' 'backbone_follow_kick\\(\\);' 4"
check "ot_br_init_task 做完边界路由后置位"      "python3 ../../tools/grep_code.py -q 's_br_init_done = true;' thread_br.c"
check "stop 清掉 s_br_init_done"                "awk '/^static esp_err_t stop_impl\(void\)$/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py -q 's_br_init_done = false;' /dev/stdin"
check "换口（do_backbone_switch）持生命周期锁且拿锁后复查 s_running" "awk '/^static void do_backbone_switch/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py -q 'life_take\(\)' /dev/stdin && awk '/^static void do_backbone_switch/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py -q '!s_running \|\| thread_br_radio_is_paused\(\)' /dev/stdin"
check "公开 start/stop 都持生命周期锁"          "awk '/^esp_err_t thread_br_start\(void\)$/{f=1} f&&/^}/{exit} f' thread_br.c | grep -q 'life_take()' && awk '/^esp_err_t thread_br_stop\(void\)$/{f=1} f&&/^}/{exit} f' thread_br.c | grep -q 'life_take()'"
check "生命周期锁是递归的（start 失败路径会调 stop）" "python3 ../../tools/grep_code.py -q 'xSemaphoreCreateRecursiveMutex\(\)' thread_br.c"
check "换口任务用内部 SRAM 栈（库可能写 NVS）"   "python3 ../../tools/grep_code.py -q 'xTaskCreate\(backbone_switch_task' thread_br.c && python3 ../../tools/grep_code.py --absent -q 'psram_task_create\(backbone_switch_task' thread_br.c"
check "灯拿到边界路由状态"                      "python3 ../../tools/grep_code.py -q 'led_ctrl_set_thread_border_routing\(br_ok\)' thread_br.c && python3 ../../tools/grep_code.py -q 'br_state = otBorderRoutingGetState\(inst\);' thread_br.c && python3 ../../tools/grep_code.py -q 'br_ok = \(br_state == OT_BORDER_ROUTING_STATE_RUNNING\);' thread_br.c"
check "边界路由状态变化成对记日志（灯琥珀/回绿可回查）" "grep -q '边界路由恢复运行（backbone=' thread_br.c && grep -q '边界路由没在运行（state=' thread_br.c && python3 ../../tools/grep_code.py -q 'br_sampled && ' thread_br.c"
check "拿不到 OT 锁时灯沿用上次状态（不当成没入网）"  "python3 ../../tools/grep_code.py -q 'attached = s_prev_attached;' thread_br.c && python3 ../../tools/grep_code.py -q 'br_ok    = s_prev_br_ok;' thread_br.c"
check "自动跟随不另起换口任务（拔线时只占一份内部栈）" "awk '/^static void backbone_follow_task/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py --absent -q 'thread_br_switch_backbone\(' /dev/stdin && awk '/^static void backbone_follow_task/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py -q 'do_backbone_switch\(' /dev/stdin"
check "防抖靠定时器：事件只重置定时器、不建任务（4 KB 不压在重连高峰上）" "awk '/^static void backbone_follow_kick\\(void\\)$/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py --absent -q 'xTaskCreate|vTaskDelay' /dev/stdin && awk '/^static void backbone_follow_kick\\(void\\)$/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py -q 'backbone_follow_arm\\(\\);' /dev/stdin && awk '/^static void backbone_follow_arm\\(void\\)$/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py -q 'esp_timer_start_once\\(s_follow_timer' /dev/stdin"
check "跟随任务不睡（只在要换口时存在）"          "awk '/^static void backbone_follow_task/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py --absent -q 'vTaskDelay' /dev/stdin"
check "已在对的口上就不建任务"                  "in_order backbone_follow_timer_cb 'want == s_backbone_netif\\) return;' 'xTaskCreate\\(backbone_follow_task'"
check "跟随定时器 init 建一次、stop 时停"         "awk '/^esp_err_t thread_br_init\(void\)$/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py -q 'esp_timer_create\(&fargs, &s_follow_timer\)' /dev/stdin && awk '/^static esp_err_t stop_impl\(void\)$/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py -q 'esp_timer_stop\(s_follow_timer\)' /dev/stdin"
check "跟随/换口任务栈都 ≤4096（dma.min_free 判据 6）" "grep -q 'xTaskCreate(backbone_follow_task, \"ot_bb_follow\", 4096' thread_br.c && grep -q 'xTaskCreate(backbone_switch_task, \"ot_bb_switch\", 4096' thread_br.c"
check "不用 esp_timer_restart（IDF 里它在锁外判状态，会摘两次链表）" "python3 ../../tools/grep_code.py --absent -q 'esp_timer_restart\\(' thread_br.c"
check "跟随失败有上限地自动重试（建任务失败 / init 失败）" "awk '/^static void backbone_follow_timer_cb\\(void \\*arg\\)$/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py -q 'backbone_follow_retry\\(' /dev/stdin && awk '/^static void backbone_follow_task/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py -q 'backbone_follow_retry\\(' /dev/stdin && awk '/^static void backbone_follow_retry/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py -q 's_follow_retries >= BACKBONE_FOLLOW_MAX_RETRY' /dev/stdin"
check "网络事件把重试计数清零（忙时重排不清零）" "awk '/^static void backbone_follow_kick\\(void\\)$/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py -q 's_follow_retries = 0;' /dev/stdin && awk '/^static void backbone_follow_arm\\(void\\)$/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py --absent -q 's_follow_retries' /dev/stdin"
check "选口缓存网口句柄（定时器回调里不每次走 IPC）" "awk '/^static esp_netif_t \\*pick_backbone/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py -q 'if \\(!n\\) n = s_cand_netif\\[i\\] = esp_netif_get_handle_from_ifkey' /dev/stdin && [ \$(awk '/^static esp_netif_t \\*pick_backbone/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py 'esp_netif_get_handle_from_ifkey' /dev/stdin | wc -l) -eq 1 ]"
check "换口拿锁后复查 init 已完成、目标口仍 up" "awk '/^static void do_backbone_switch/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py -q '!s_br_init_done' /dev/stdin && awk '/^static void do_backbone_switch/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py -q 'esp_netif_is_netif_up\\(to\\)' /dev/stdin"
check "认领在临界区内" "awk '/^static bool once_claim/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py -q 'portENTER_CRITICAL' /dev/stdin"
check "换口成功后补建 zb_check、补刷 mDNS（开机无上行的情形，T9.7②③）" "awk '/^static void backbone_follow_task/{f=1} f&&/^}/{exit} f' thread_br.c | awk '/init_err == ESP_OK/{o=1} o&&/zb_check_ensure\\(\\);/{z=1} o&&/mdns_refresh_kick\\(\\);/{m=1} /init_err == ESP_ERR_INVALID_STATE/{o=0} END{exit !(z&&m)}'"
check "开机路径也走 zb_check_ensure（不再内联建）" "awk '/^static void ot_br_init_task/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py -q 'zb_check_ensure\\(\\);' /dev/stdin"
check "诊断换口接口出厂默认关"                  "awk '/config BITENGINE_DIAG_THREAD_BACKBONE/{f=1} f' ../../main/Kconfig.projbuild | head -5 | grep -q 'default n'"


echo ""; echo "--- ot_br_init 不再占着内部栈刷 5 分钟 mDNS（2026-10-05，failover DMA 低谷）---"
OTI="awk '/^static void ot_br_init_task\(void \*arg\)\$/{f=1} f&&/^}/{exit} f' thread_br.c"
MRT="awk '/^static void mdns_refresh_task\(void \*arg\)\$/{f=1} f&&/^}/{exit} f' thread_br.c"
check "ot_br_init 做完就退出（不在里面刷 mDNS、kick 后紧接自删）" "$OTI | python3 ../../tools/grep_code.py --absent -q 'mdns_svc_refresh|mdns_svc_brand_meshcop|30000|MDNS_REFRESH_GAP_MS' /dev/stdin && in_order ot_br_init_task 'mdns_refresh_kick\\(\\);' 'vTaskDelete\\(NULL\\);' 3"
check "mdns_refresh 栈在 PSRAM（没退回 xTaskCreate）"  "python3 ../../tools/grep_code.py -q 'psram_task_create\(mdns_refresh_task' thread_br.c && python3 ../../tools/grep_code.py --absent -q 'xTaskCreate\(mdns_refresh_task' thread_br.c"
check "mdns_refresh 常驻不自删（psram_task.h 规则 2）" "$MRT | python3 ../../tools/grep_code.py --absent -q 'vTaskDelete' /dev/stdin"
check "mdns_refresh 不碰 flash/NVS（psram_task.h 规则 1）" "$MRT | python3 ../../tools/grep_code.py --absent -q 'nvs_|esp_partition_|config_mgr_set' /dev/stdin"
check "mdns_refresh 只建一次（临界区认领，两处调用不会建两个）" "awk '/^static void mdns_refresh_kick\\(void\\)$/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py -q 'once_claim\\(&s_mdns_refresh_claimed\\)' /dev/stdin"
check "zb_check 只建一次（常驻，Thread 重启 / 换口都不多建）" "awk '/^static void zb_check_ensure\\(void\\)$/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py -q 'once_claim\\(&s_zb_check_claimed\\)' /dev/stdin && [ \$(python3 ../../tools/grep_code.py 'psram_task_create\\(zigbee_check_task' thread_br.c | wc -l) -eq 1 ]"

# 边界路由要等骨干网卡的链路本地地址可用再起（DAD 期间发不出 RS / 组播探测，开机偶发两条 E 级），且在拿 OT 锁之前等
check "ot_br_init 在拿 OT 锁之前等骨干链路本地地址可用（有上限）" "python3 - <<'PY'
import re,sys
src=open('thread_br.c',encoding='utf-8').read()
t=src.find('static void ot_br_init_task(void *arg)'); body=src[t:t+1500]
a=body.find('backbone_ll_wait('); b=body.find('esp_openthread_lock_acquire(')
sys.exit(0 if 0<=a<b and re.search(r'#define BACKBONE_LL_WAIT_MS\s+\d+', src) else 1)
PY"

echo ""; echo "--- 802.15.4 帧挂起表 ≥ Thread 子设备上限（2026-10-06）---"
# 表满时 ESP 的 OT 端口不报错、OT 兜底失效 → 没进表的休眠子设备下行断几分钟。两侧都钉：配置值 + 编译期断言
KV="python3 -c \"
import re,sys
def kv(f): return dict(re.findall(r'^(CONFIG_\w+)=(\S+)\s*$', open(f).read(), re.M))
d,c=kv('../../sdkconfig.defaults'),kv('../../sdkconfig')
pt,mc='CONFIG_IEEE802154_PENDING_TABLE_SIZE','CONFIG_OPENTHREAD_MLE_MAX_CHILDREN'
ok = int(d[pt]) >= int(d[mc]) and c.get(pt)==d[pt] and c.get(mc)==d[mc] and int(c[pt]) >= int(c[mc])
sys.exit(0 if ok else 1)\""
check "挂起表 ≥ 子设备上限，且 sdkconfig 与 sdkconfig.defaults 同步"   "$KV"
check "thread_br.c 编译期断言挂起表 ≥ 子设备上限（宏缺了直接编译失败）" "python3 ../../tools/grep_code.py -q '_Static_assert\\(CONFIG_IEEE802154_PENDING_TABLE_SIZE >= CONFIG_OPENTHREAD_MLE_MAX_CHILDREN *,' thread_br.c"
# 整表 reset 会连真子设备的条目一起清掉而 OT 以为还在 = 人为制造下行卡死。全仓禁止（诊断代码也不许）
check "全仓不调 esp_ieee802154_reset_pending_table"    "python3 ../../tools/grep_code.py --absent -q 'esp_ieee802154_reset_pending_table\\(' \$(find ../../components ../../main -name '*.c' -not -path '*/test_host/*')"
check "挂起表压测接口出厂默认关"                      "awk '/config BITENGINE_DIAG_THREAD_PENDING/{f=1} f' ../../main/Kconfig.projbuild | head -5 | grep -q 'default n'"
check "出厂 sdkconfig / defaults 没开两个 Thread 诊断开关" "grep -q '^# CONFIG_BITENGINE_DIAG_THREAD_PENDING is not set' ../../sdkconfig && grep -q '^# CONFIG_BITENGINE_DIAG_THREAD_BACKBONE is not set' ../../sdkconfig && ! grep -qE '^CONFIG_BITENGINE_DIAG_THREAD_(PENDING|BACKBONE)=y' ../../sdkconfig.defaults"
check "压测实现整段在诊断开关里（出厂编出来是空的）"  "python3 -c \"
import re,sys
s=re.sub(r'/[*].*?[*]/|//[^\\\\n]*', '', open('thread_br_diag.c').read(), flags=re.S)   # 剥注释：头注释里提到这些名字
i=s.index('#if CONFIG_BITENGINE_DIAG_THREAD_PENDING'); j=s.rindex('#endif')
sys.exit(0 if all(i < s.index(k) < j for k in ('esp_ieee802154_add_pending_addr','esp_openthread_lock_acquire','thread_br_diag_pending(')) else 1)\""
dln() { python3 ../../tools/grep_code.py "$1" thread_br_diag.c | cut -d: -f2; }   # 剥注释后的行号
# thread_br_diag.c 里有两段各自的 #if 块（帧挂起表压测、SRP 注入）。下面的锁 / return 判据都按段落算，
# 别按整个文件的第一次取锁、最后一次放锁 —— 2026-10-07 加了第二段之后，那样的区间会跨段、互相误报
sect() { python3 - "$1" <<'PY'
import sys
k = sys.argv[1]; L = open('thread_br_diag.c').read().split('\n')
a = next(i for i, l in enumerate(L, 1) if l.strip() == f'#if {k}')
b = next(i for i, l in enumerate(L, 1) if i > a and l.strip().startswith('#endif') and k in l)
print(a, b)
PY
}
dln_in() { local lo hi; read -r lo hi < <(sect "$1"); dln "$2" | awk -v lo="$lo" -v hi="$hi" '$1>=lo && $1<=hi'; }
# 段内"第一次取锁 … 最后一次放锁"之间没有 return（提前返回 = OT 锁泄漏，整个 Thread 栈卡死，S3）
no_return_in_locked() { local a b; a=$(dln_in "$1" 'esp_openthread_lock_acquire\(' | head -1); b=$(dln_in "$1" 'esp_openthread_lock_release\(' | tail -1)
    [ -n "$a" ] && [ -n "$b" ] && [ -z "$(dln '\breturn\b' | awk -v a="$a" -v b="$b" '$1>a && $1<b')" ]; }
P=CONFIG_BITENGINE_DIAG_THREAD_PENDING; I=CONFIG_BITENGINE_DIAG_SRP_INJECT
check "压测只在 OT 锁内动表、调 OT（取锁在第一次 add 之前；最后一次 clear / 读计数 / 读子设备都在放锁之前）" "[ -n \"\$(dln_in \$P 'esp_openthread_lock_release\\(')\" ] && [ \$(dln_in \$P 'esp_openthread_lock_acquire\\(' | head -1) -lt \$(dln_in \$P 'esp_ieee802154_add_pending_addr\\(' | head -1) ] && [ \$(dln_in \$P 'esp_ieee802154_clear_pending_addr\\(|otLinkGetCounters\\(|otThreadGetChildInfoByIndex\\(' | tail -1) -lt \$(dln_in \$P 'esp_openthread_lock_release\\(' | tail -1) ]"
check "压测持锁区间里没有 return（锁不会泄漏）"     "no_return_in_locked \$P"

echo "---- SRP 注入（扩容实验 D 方案的传输层，2026-10-07）----"
check "SRP 注入接口出厂默认关"                        "awk '/config BITENGINE_DIAG_SRP_INJECT/{f=1} f' ../../main/Kconfig.projbuild | head -5 | grep -q 'default n'"
check "出厂 sdkconfig / defaults 没开 SRP 注入"       "! grep -qE '^CONFIG_BITENGINE_DIAG_SRP_INJECT=y' ../../sdkconfig ../../sdkconfig.defaults"
check "注入实现整段在自己的诊断开关里（出厂编出来是空的）" "[ -n \"\$(dln_in \$I 'otUdpSend\\(')\" ] && [ -n \"\$(dln_in \$I 'otUdpOpen\\(')\" ] && [ \$(dln 'otUdpSend\\(|otUdpOpen\\(' | wc -l) -eq \$(dln_in \$I 'otUdpSend\\(|otUdpOpen\\(' | wc -l) ]"
# 按函数算区间：同一段 #if 里还有注册潮的代码，按整段的"第一次取锁…最后一次放锁"会跨函数误报（2026-10-07）
fn_locked_no_return() { python3 - "$1" "$2" <<'PY'
import re, sys
f, sig = sys.argv[1], sys.argv[2]
s = re.sub(r'/[*].*?[*]/', lambda m: '\n' * m.group(0).count('\n'), open(f).read(), flags=re.S)
L = re.sub(r'//[^\n]*', '', s).split('\n')
a = next(i for i, l in enumerate(L) if re.search(sig, l))
b = next(i for i in range(a + 1, len(L)) if L[i] == '}')
body = L[a:b + 1]
acq = [i for i, l in enumerate(body) if 'esp_openthread_lock_acquire(' in l]
rel = [i for i, l in enumerate(body) if 'esp_openthread_lock_release(' in l]
sys.exit(0 if acq and rel and not any(re.search(r'\breturn\b', l) for l in body[acq[0]:rel[-1]]) else 1)
PY
}
check "注入持锁区间里没有 return（每条失败路径先放锁再 goto out）" "fn_locked_no_return thread_br_diag.c '^esp_err_t thread_br_diag_srp_inject\\('"
# 等应答时必须不持 OT 锁：SRP server 就跑在 OT 任务里，持着锁等它 = 等到超时、永远收不到应答
check "注入等应答时不持 OT 锁（等之前最近的一次锁操作是放锁）" "t=\$(dln_in \$I 'xSemaphoreTake\\(s_inj_done, *pdMS_TO_TICKS\\(timeout_ms\\)\\)'); [ -n \"\$t\" ] && [ \"\$(dln_in \$I 'esp_openthread_lock_(acquire|release)\\(' | awk -v t=\$t '\$1<t' | tail -1)\" = \"\$(dln_in \$I 'esp_openthread_lock_release\\(' | awk -v t=\$t '\$1<t' | tail -1)\" ]"
check "注入按 DNS 报文 ID 认应答（上一条超时后迟到的应答不会顶替这一条）" "python3 ../../tools/grep_code.py -q 's_inj_id = \\(uint16_t\\)\\(\\(req\\[0\\] << 8\\) \\| req\\[1\\]\\)' thread_br_diag.c && python3 ../../tools/grep_code.py -q '!= s_inj_id\\) return' thread_br_diag.c"
check "REST 接口挂在同一个开关下"                      "grep -B1 'URI_ENTRY(\"/api/v1/diag/srp/inject\"' ../webui/rest_api.c | grep -q '#if CONFIG_BITENGINE_DIAG_SRP_INJECT'"

echo "---- 注册潮（成批按时间表发，诊断构建）----"
BT="awk '/^static void burst_task\\(void \\*arg\\)\$/{f=1} f&&/^}/{exit} f' thread_br_diag.c"
check "注册潮发送持锁区间里没有 return"                "fn_locked_no_return thread_br_diag.c '^static void burst_run\\(void\\)'"
# FreeRTOS 要等任务放掉**全部**互斥锁才撤销继承来的优先级：整轮持着注入锁，测量本身就偏了（coredump 里 16/4）
check "注册潮任务不持注入锁（继承来的优先级会整轮不撤）" "$BT | python3 ../../tools/grep_code.py --absent -q 's_inj_mutex' /dev/stdin"
check "单条注入在注册潮进行时直接回忙（先查 running 再拿注入锁）" "python3 - <<'PY'
import re, sys
s = re.sub(r'/[*].*?[*]/|//[^\n]*', '', open('thread_br_diag.c').read(), flags=re.S)
b = s.split('esp_err_t thread_br_diag_srp_inject(')[1].split('\n}\n')[0]
i, j = b.find('if (s_b.running) return ESP_ERR_INVALID_STATE;'), b.find('xSemaphoreTake(s_inj_mutex')
sys.exit(0 if -1 < i < j else 1)
PY"
check "注册潮任务常驻（PSRAM 栈不能自删）"            "$BT | python3 ../../tools/grep_code.py --absent -q 'vTaskDelete' /dev/stdin && python3 ../../tools/grep_code.py -q 'psram_task_create\\(burst_task' thread_br_diag.c"
check "IDLE 计时用 32 位（64 位读写在 RV32 上不是原子的）" "python3 ../../tools/grep_code.py -q 'static volatile uint32_t s_idle_last_us' thread_br_diag.c && python3 ../../tools/grep_code.py -q 'static volatile uint32_t s_idle_last_us' ot_cpu_guard.c"
check "注册潮 / coredump 的 REST 接口都在各自的诊断开关里" "python3 - <<'PY'
import re, sys
s = open('../webui/rest_api.c').read()
def guarded(uri, flag):
    i = s.index(f'URI_ENTRY(\"{uri}\"')
    head = s[:i]
    return head.rfind(f'#if {flag}') > head.rfind('#endif')
sys.exit(0 if guarded('/api/v1/diag/srp/burst', 'CONFIG_BITENGINE_DIAG_SRP_INJECT')
         and guarded('/api/v1/diag/coredump', 'CONFIG_BITENGINE_DIAG_COREDUMP') else 1)
PY"
check "mdns 写接口计时只在诊断构建里 wrap（出厂不改链接）" "awk '/if\\(CONFIG_BITENGINE_DIAG_SRP_INJECT\\)/{f=1} f&&/endif\\(\\)/{exit} f' CMakeLists.txt | grep -q 'mdns_delegate_hostname_add'"

echo "---- OT 任务过载让出 CPU（ot_cpu_guard.c，2026-10-07）----"
# 252 台 10 s 内重注册，OT 任务被验签 + 处理连续占满 → IDLE 5 s 跑不到 → 任务看门狗 → 重启 → 再成批重注册
check "出厂也 wrap 验签（不在诊断开关里）"            "awk '/if\\(CONFIG_OPENTHREAD_ENABLED\\)/{f=1} f&&/endif\\(\\)/{exit} f' CMakeLists.txt | grep -q -- '--wrap=otPlatCryptoEcdsaVerify'"
check "同一符号只 wrap 一次（诊断清单里没有验签）"     "! awk '/if\\(CONFIG_BITENGINE_DIAG_SRP_INJECT\\)/{f=1} f&&/endif\\(\\)/{exit} f' CMakeLists.txt | grep -q 'otPlatCryptoEcdsaVerify'"
check "守卫：调真验签、IDLE 久未运行就 vTaskDelay"     "awk '/^otError __wrap_otPlatCryptoEcdsaVerify/{f=1} f&&/^}/{exit} f' ot_cpu_guard.c | python3 ../../tools/grep_code.py -q '__real_otPlatCryptoEcdsaVerify\\(' /dev/stdin && awk '/^otError __wrap_otPlatCryptoEcdsaVerify/{f=1} f&&/^}/{exit} f' ot_cpu_guard.c | python3 ../../tools/grep_code.py -q 'vTaskDelay\\(pdMS_TO_TICKS\\(GUARD_YIELD_MS\\)\\)' /dev/stdin"
check "守卫门限远小于任务看门狗（≤ 一半）"             "python3 -c \"
import re,sys
g=int(re.search(r'#define GUARD_STARVE_MS\\s+(\\d+)', open('ot_cpu_guard.c').read()).group(1))
w=int(re.search(r'^CONFIG_ESP_TASK_WDT_TIMEOUT_S=(\\d+)', open('../../sdkconfig').read(), re.M).group(1))
sys.exit(0 if g*2 <= w*1000 else 1)\""
check "thread_br_init 一开头就装守卫"                  "awk '/^esp_err_t thread_br_init\\(void\\)\$/{f=1} f&&/^}/{exit} f' thread_br.c | head -4 | python3 ../../tools/grep_code.py -q 'thread_br_cpu_guard_init\\(\\);' /dev/stdin"
check "守卫读数从 /api/v1/diag/srp 报出"              "grep -q 'cpu_guard_yields' ../webui/rest_api.c && grep -q 'thread_br_cpu_guard_stats(&yields, &starve_ms)' ../webui/rest_api.c"
# mdns 补丁 SCALE ⑧ 让拍时靠它判断"IDLE 真的跑过没有"；mdns 里只有弱定义兜底（恒 0 = 只睡一拍）——这里没了不会报错，
# 只会静默退化回"只睡一拍"，而那在三连浏览时实测让 IDLE 连续饿 3.1 s（docs/test_cases_scale_baseline.md 判据 7）
check "给 mdns 让拍提供 IDLE 时刻的强定义（bitengine_idle_last_us）" "awk '/^uint32_t bitengine_idle_last_us\\(void\\)\$/{f=1} f&&/^}/{exit} f' ot_cpu_guard.c | python3 ../../tools/grep_code.py -q 's_idle_last_us' /dev/stdin"

echo "---- OT 日志级别（2026-10-07）----"
# 漏设时 OT 停在编译期上限 DEBG：每条 SRP 注册多打 22 行 INFO 以 115200 忙等写进没引出的 UART0（数据见 docs/test_cases_scale_baseline.md「SRP 代播满载压测」第 2 件）
check "esp_openthread_init 之后、建主循环任务之前设级别" "python3 - <<'PY'
import re, sys
L = re.sub(r'/[*].*?[*]/', lambda m: '\n' * m.group(0).count('\n'), open('thread_br.c').read(), flags=re.S).split('\n')
ln = lambda pat: next((i for i, l in enumerate(L) if re.search(pat, l)), -1)
a, b, c = ln(r'esp_openthread_init\(&s_ot_platform_cfg\)'), ln(r'otLoggingSetLevel\(\(otLogLevel\)CONFIG_BITENGINE_OT_LOG_LEVEL\)'), ln(r'xTaskCreate\(ot_main_task')
sys.exit(0 if -1 < a < b < c else 1)
PY"
# grep_code.py 会剥字符串字面量，所以"有这个调用"用它、"标签是 OPENTHREAD"用普通 grep（注释里没有带括号的这一串）
check "代播库直接打的 OPENTHREAD INFO 也跟着压"        "python3 ../../tools/grep_code.py -q 'esp_log_level_set\\(' thread_br.c && grep -q 'esp_log_level_set(\"OPENTHREAD\",' thread_br.c"
check "出厂默认 WARN 或更低（Kconfig / sdkconfig / defaults）" "awk '/config BITENGINE_OT_LOG_LEVEL/{f=1} f' ../../main/Kconfig.projbuild | head -6 | grep -qE 'default [12]\$' && grep -qE '^CONFIG_BITENGINE_OT_LOG_LEVEL=[12]\$' ../../sdkconfig && ! grep -qE '^CONFIG_BITENGINE_OT_LOG_LEVEL=[345]' ../../sdkconfig.defaults"

# USB 桥模式下副控制台承载 EZSP 字节流、"*" 被静音；按标签设的级别优先于默认值 —— 这里一设，OT 告警就混进 EZSP 流
check "USB 桥模式下不给 OPENTHREAD 设级别"            "python3 - <<'PY'
import re, sys
s = re.sub(r'/[*].*?[*]/|//[^\n]*', '', open('thread_br.c').read(), flags=re.S)
m = re.search(r'if \(ezsp_bridge_get_mode\(\) != BRIDGE_MODE_USB\) \{\s*esp_log_level_set\(\"OPENTHREAD\", BITENGINE_OT_ESP_LOG_LEVEL\);', s)
sys.exit(0 if m and s.count('esp_log_level_set(\"OPENTHREAD\"') == 1 else 1)
PY"
check "USB 桥退出时把 OPENTHREAD 设回（设 \"*\" 会清掉按标签的级别）" "python3 - <<'PY'
import re, sys
s = re.sub(r'/[*].*?[*]/|//[^\n]*', '', open('../ezsp_bridge/usb_bridge.c').read(), flags=re.S)
b = s.split('void usb_bridge_stop(void)')[1].split('\n}\n')[0]
i = b.rfind('esp_log_level_set(\"*\", CONFIG_LOG_DEFAULT_LEVEL)'); j = b.find('esp_log_level_set(\"OPENTHREAD\", BITENGINE_OT_ESP_LOG_LEVEL)')
sys.exit(0 if -1 < i < j else 1)
PY"

echo "---- 健康采样 / 读数（2026-10-07 审计）----"
CST="awk '/^static void child_stats_timer_cb\\(void \\*arg\\)\$/{f=1} f&&/^}/{exit} f' thread_br.c"
# 没拿到锁那一拍若把 0 当读数写进去，0≠N、N≠0 会把"连续不动"清零两次：射频死了、协议栈还在忙时永远判不出停摆
check "停摆探针：没拿到锁按没动计、不覆盖读数"        "$CST | python3 ../../tools/grep_code.py -q 'if \\(!sampled \\|\\| \\(tx == s_last_tx && rx == s_last_rx\\)\\) s_still_ticks\\+\\+;' /dev/stdin && $CST | python3 ../../tools/grep_code.py -q 'if \\(sampled\\) \\{ s_last_tx = tx; s_last_rx = rx; \\}' /dev/stdin"
check "拿不到锁的告警要连续几拍才记（不刷告警环）"    "$CST | python3 ../../tools/grep_code.py -q '\\+\\+s_lock_miss == LOCK_MISS_WARN' /dev/stdin"
check "/status 的子设备数读缓存，不在 httpd 里等 OT 锁" "awk '/^void thread_br_get_child_stats/{f=1} f&&/^}/{exit} f' thread_br.c | python3 ../../tools/grep_code.py --absent -q 'esp_openthread_lock_acquire' /dev/stdin && $CST | python3 ../../tools/grep_code.py -q 's_child_total = total;' /dev/stdin"
check "IDLE 最长间隔由 idle hook 自己量（不是验签时刻的下界）" "awk '/^static bool idle_hook/{f=1} f&&/^}/{exit} f' ot_cpu_guard.c | python3 ../../tools/grep_code.py -q 's_idle_gap_max_us = gap' /dev/stdin"

echo "---- esp_timer 回调里不阻塞拿 OT 锁（2026-10-07）----"
# esp_timer 任务优先级 22：回调一等 OT 锁，全系统的定时器跟着停，持锁的 ot_main 还被优先级继承抬到 22
check "所有 esp_timer 回调：拿 OT 锁只试不等、不调会等 5 s 的 get_child_stats" "python3 - <<'PY'
import re, sys
s = re.sub(r'/[*].*?[*]/|//[^\n]*', '', open('thread_br.c').read(), flags=re.S)
cbs = set(re.findall(r'\.callback\s*=\s*(\w+)', s))
bad = []
for cb in cbs:
    m = re.search(r'\nstatic void ' + cb + r'\(void \*arg\)\n\{(.*?)\n\}\n', s, re.S)
    if not m: bad.append(cb + ': 找不到函数体'); continue
    body = m.group(1)
    for arg in re.findall(r'esp_openthread_lock_acquire\(([^)]*)\)', body):
        if arg.strip() != '0': bad.append(f'{cb}: lock_acquire({arg})')
    if 'thread_br_get_child_stats(' in body: bad.append(cb + ': thread_br_get_child_stats')
print('; '.join(bad)); sys.exit(1 if bad or not cbs else 0)
PY"

echo "---- 代播跟踪扫描开销（2026-10-07）----"
check "扫描量的是持 OT 锁那一段（计时起点在取锁之后、终点在放锁之前）" "python3 - <<'PY'
import re, sys
s = re.sub(r'/[*].*?[*]/|//[^\n]*', '', open('srp_readvertise.c').read(), flags=re.S)
b = s.split('static void scan_registry(void)')[1].split('\n}\n')[0]
acq, t0 = b.find('esp_openthread_lock_acquire('), b.find('int64_t t0 = esp_timer_get_time()')
held, rel = b.find('esp_timer_get_time() - t0'), b.find('esp_openthread_lock_release()')
sys.exit(0 if -1 < acq < t0 < held < rel else 1)
PY"
check "扫描开销从 GET /api/v1/thread/readvertise 报出去"  "grep -q 'scan_lock_max_us' ../webui/rest_api.c && grep -q 'srp_readv_get_scan_stats(&lock_us, &lock_max_us, &scanned)' ../webui/rest_api.c"

# 拆边界路由必须经 br_deinit_locked()：IDF 注销不掉 "dns_upstream_query" 这个工作流，直接 deinit 会在主循环
# 表里留一个恒失败的条目，主循环下一圈就退出、Thread 永久停（T9.18 实验 2，2026-10-07；理由见 br_deinit_locked 头注）
check "border_router_deinit 只在 br_deinit_locked 里调（全仓）" "[ \$(cat ../*/*.c ../../main/*.c | python3 ../../tools/grep_code.py 'esp_openthread_border_router_deinit *\\(' /dev/stdin | wc -l) -eq 1 ] && awk '/^static esp_err_t br_deinit_locked\\(void\\)\$/,/^}\$/' thread_br.c | python3 ../../tools/grep_code.py -q 'esp_openthread_border_router_deinit *\\(' /dev/stdin"
check "br_deinit_locked 用存下来的截断名注销残留条目" "awk '/^static esp_err_t br_deinit_locked\\(void\\)\$/,/^}\$/' thread_br.c | python3 ../../tools/grep_code.py -q 'esp_openthread_platform_workflow_unregister\\(OT_DNS_UPSTREAM_WORKFLOW_STORED_NAME\\)' /dev/stdin && python3 ../../tools/grep_code.py --strings -q 'define OT_DNS_UPSTREAM_WORKFLOW_STORED_NAME \"dns_upstream_qu\"' thread_br.c"
check "主循环意外退出会报 E（不再静默）" "awk '/^static void ot_main_task\\(void \\*arg\\)\$/,/^}\$/' thread_br.c | python3 ../../tools/grep_code.py -q 'if \\(!s_stopping\\)' /dev/stdin"

# Zigbee 抓包（诊断，2026-10-07）：otLinkSetChannel 在 MLE 停着时会清掉 Thread 的 Active / Pending 数据集，
# 抓包只许用平台层 otPlatRadioReceive 调射频（理由见 zb_sniff.c 头注 H1 第 1 条）
check "Zigbee 抓包出厂默认关"                 "awk '/config BITENGINE_DIAG_ZB_SNIFF/{f=1} f' ../../main/Kconfig.projbuild | head -5 | grep -q 'default n'"
check "出厂 sdkconfig 没开 Zigbee 抓包"        "! grep -q '^CONFIG_BITENGINE_DIAG_ZB_SNIFF=y' ../../sdkconfig ../../sdkconfig.defaults"
check "全仓不调 otLinkSetChannel（会清 Thread 数据集）" "cat ../*/*.c ../../main/*.c | python3 ../../tools/grep_code.py --absent -q 'otLinkSetChannel *\\(' /dev/stdin"

# 对外规格里的三个设备容量数（Zigbee 直连 / Thread 直连 / Matter）被抄进了手册、docs/00 等七八处，正本是配置：
# 改了 CONFIG_OPENTHREAD_MLE_MAX_CHILDREN，跑这条就知道哪几处文档要跟着改（CLAUDE.md 7.6 第 3 条）
internal "对外规格的三个容量数与配置一致（tools/check_capacity_docs.py）" "python3 ../../tools/check_capacity_docs.py"

echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
