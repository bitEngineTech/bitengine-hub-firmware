#!/bin/bash
# components/watchdog/VERIFY.sh — Auto-verify design doc compliance
# Run after idf.py build passes: bash components/watchdog/VERIFY.sh

cd "$(dirname "$0")"
PASS=0; FAIL=0

check() {
    if eval "$2" > /dev/null 2>&1; then
        echo "✅ $1"; ((PASS++))
    else
        echo "❌ $1"; ((FAIL++))
    fi
}

echo "=== Watchdog Module Verification ==="
echo ""

# Events: all 7 must have esp_event_post
echo "--- Events (7 required) ---"
# 只在私有检出里有的内部文档 / 工具（docs/、发布与产线脚本）：公开仓库不导出它们（tools/oss_export.py），
# 那里跳过而不是报红；私有检出里照常判。标志是 docs/开发索引.md 在不在。
internal() { if [ -f ../../docs/开发索引.md ]; then check "$@"; else echo "⏭  $1 —— 依赖内部文档 / 工具，本检出没有，跳过"; fi; }

check "WATCHDOG_EVENT_MG24_RESET posted"          "grep -r 'WATCHDOG_EVENT_MG24_RESET' *.c | grep esp_event_post"
check "WATCHDOG_EVENT_HEAP_LOW posted"             "grep -r 'WATCHDOG_EVENT_HEAP_LOW' *.c | grep esp_event_post"
check "WATCHDOG_EVENT_HEAP_CRITICAL posted"        "grep -r 'WATCHDOG_EVENT_HEAP_CRITICAL' wd_resource.c | grep esp_event_post"
check "WATCHDOG_EVENT_HEAP_LEAK_SUSPECTED posted"  "grep -r 'WATCHDOG_EVENT_HEAP_LEAK_SUSPECTED' *.c | grep esp_event_post"
check "WATCHDOG_EVENT_TEMP_HIGH posted"            "grep -r 'WATCHDOG_EVENT_TEMP_HIGH' *.c | grep esp_event_post"
check "WATCHDOG_EVENT_TEMP_CRITICAL posted"        "grep -r 'WATCHDOG_EVENT_TEMP_CRITICAL' *.c | grep esp_event_post"
check "WATCHDOG_EVENT_ABNORMAL_BOOT posted"        "grep -r 'WATCHDOG_EVENT_ABNORMAL_BOOT' *.c | grep esp_event_post"

echo ""
echo "--- health_status_t fields (10 required) ---"
check "mg24_alive assigned"            "grep -r 'mg24_alive' *.c | grep -v '//' | grep '='"
check "mg24 存活由探针判定"            "grep -q 'mg24_ok' wd_probes.c"
check "mg24_last_reset_time assigned"  "grep -r 'mg24_last_reset_time' *.c | grep '='"
check "mg24_total_resets assigned"     "grep -r 'mg24_total_resets' *.c | grep '='"
check "free_heap assigned"             "grep -r 'free_heap' *.c | grep '='"
check "min_free_heap assigned"         "grep -r 'min_free_heap' *.c | grep '='"
check "temperature assigned"           "grep -r 's_status.temperature' *.c | grep '='"
check "legacy 字段已标注"              "grep -q 'legacy' include/watchdog.h"
check "uptime_seconds assigned"        "grep -r 'uptime_seconds' *.c | grep '='"
check "last_reset_reason assigned"     "grep -r 'last_reset_reason' *.c | grep '='"

echo ""
echo "--- Public API (4 functions) ---"
check "watchdog_init implemented"         "grep -c 'watchdog_init' *.c | grep -v ':0'"
check "watchdog_get_status implemented"   "grep -c 'watchdog_get_status' *.c | grep -v ':0'"
check "watchdog_reset_mg24 implemented"   "grep -c 'watchdog_reset_mg24' *.c | grep -v ':0'"
check "watchdog_restart_esp32 implemented" "grep -c 'watchdog_restart_esp32' *.c | grep -v ':0'"

echo ""
echo "--- Conditional compilation ---"
check "CONFIG_BITENGINE_HAS_HW_WATCHDOG used"    "grep -r 'CONFIG_BITENGINE_HAS_HW_WATCHDOG' *.c"
check "CONFIG_BITENGINE_HAS_MG24_RESET_PIN used"  "grep -r 'CONFIG_BITENGINE_HAS_MG24_RESET_PIN' *.c"

echo ""
echo "--- Task config ---"
check "wdt_feed_task priority 22"      "grep 'wdt_feed_task.*22\|22.*wdt_feed' wd_feed.c"
check "health_task priority 5"         "grep 'health.*5\|5.*health' health_monitor.c"

echo ""
echo "--- MG24 dual-mode detection ---"
check "Bridge state check"             "grep 'BRIDGE_STATE_RUNNING' wd_probes.c"
check "active_clients check"           "grep 'active_clients' wd_probes.c"
check "ezsp_bridge_health_probe call"  "grep 'ezsp_bridge_health_probe' wd_probes.c"
check "uart_rx_bytes monitoring"       "grep 'uart_rx_bytes' health_monitor.c"

echo ""
echo "--- Cooldown ---"
check "300s cooldown check"            "grep 'MG24_RESET_COOLDOWN_S' mg24_reset.c"

# ============================================================
# v2 架构断言（docs/04_watchdog_design_v2.md 第十一节）
# ============================================================
echo ""
echo "--- v2 铁律 1：不看聚合堆 ---"
check "资源守卫不用聚合堆 API"      "! grep -nE '^[^*/]*esp_get_(minimum_)?free_heap_size\(' wd_resource.c"
check "健康检查里已无堆阈值判定"    "! grep -n 'HEAP_WARN_THRESHOLD\|HEAP_CRITICAL_THRESHOLD' health_monitor.c"
check "错误阈值常量已删除"          "! grep -nE 'define HEAP_(WARN|CRITICAL)_THRESHOLD' include/watchdog.h"
check "资源判定以分配失败为主"      "grep -q 'ALLOC_FAIL_CRIT_PER_MIN' wd_resource.c"
check "水位只告警不重启"            "! grep -A6 'INTERNAL_FREE_WARN' wd_resource.c | grep -q 'WD_LEVEL_REBOOT'"

echo ""
echo "--- v2 层 B：任务心跳 ---"
check "注册表零动态分配"            "! grep -nE 'malloc|calloc' wd_registry.c"
check "wd_register 存在"            "grep -q 'int wd_register' wd_registry.c"
check "wd_kick 存在"                "grep -q 'void wd_kick' wd_registry.c"
check "OTA 豁免 wd_suspend 存在"    "grep -q 'void wd_suspend' wd_registry.c"
check "每个注册点都有对应 kick"     "[ $(grep -rl --exclude-dir=test_host 'wd_register(' ../../components ../../main | wc -l) -le $(grep -rl --exclude-dir=test_host 'wd_kick(' ../../components ../../main | wc -l) ]"
check "扫描在喂狗任务里"            "grep -q 'wd_registry_scan' wd_feed.c"
check "httpd 心跳走工作队列"        "grep -q 'httpd_queue_work' ../webui/websocket.c"
check "EZSP 数据通路订阅 TWDT"      "grep -q 'esp_task_wdt_add' ../ezsp_bridge/tcp_server.c"

check "主机侧单元测试存在"          "[ -x test_host/run.sh ]"
check "主机侧单元测试通过"          "bash test_host/run.sh"
check "在板回归脚本存在"            "[ -x regress.sh ]"

echo ""
echo "--- v2 层 C：探针 ---"
check "MG24 空闲探测窗口存在"       "grep -q 'wd_mg24_probe_active' wd_probes.c"

echo ""
echo "--- 无客户端不探测（T9.31，2026-10-08）---"
# ASH RST 会把 NCP 的协议栈打回未初始化而网关拉不起来；协调器长时间不在时网络会停，几小时后有设备
# 丢网回出厂态（10-07 Sengled）。无客户端只看线路电平；唯一的 ASH RST 是"已判死之后"的确认。
PM="awk '/^static void probe_mg24\\(void\\)/,/^}/' wd_probes.c"
check "宽限期 / 空闲探测常量已删"   "python3 ../../tools/grep_code.py --absent -q 'MG24_IDLE_' wd_probes.c"
check "无客户端分支只在线路判死后发 RST" "python3 -c \"
import sys
s=open('wd_probes.c',encoding='utf-8').read()
f=s[s.index('static void probe_mg24(void)'):]
a=f.index('s_client_since_s = 0;'); b=f.index('注入 mg24_mute')
br=f[a:b]
i=br.index('if (s_view.mg24_ok || !s_dead_by_line) {'); j=br.index('ezsp_bridge_health_probe')
sys.exit(0 if i < j and br.count('ezsp_bridge_health_probe')==1 else 1)\""
check "已判死才确认、且限频"        "$PM | python3 ../../tools/grep_code.py -q 't - s_last_confirm_s < MG24_CONFIRM_INTERVAL_S' /dev/stdin"
check "判死计时不算没有客户端的时间" "$PM | python3 ../../tools/grep_code.py -q 's_view.mg24_last_ok_s > s_client_since_s' /dev/stdin"
# 不判定不能靠把 last_ok 刷成现在来"冻结"判死 —— 那是伪造证据，本模块 v1 原罪。
check "不判定不伪造存活证据"       "python3 -c \"
import sys
s=open('wd_probes.c',encoding='utf-8').read()
f=s[s.index('static void probe_mg24(void)'):]
a=f.index('s_client_since_s = 0;'); b=f.index('注入 mg24_mute')
sys.stdout.write(f[a:b])\" | python3 ../../tools/grep_code.py --absent -q 'mg24_ok *= *true|mg24_last_ok_s *=' /dev/stdin"
check "不判定是显式状态"           "grep -q 'mg24_judged' ../watchdog/include/watchdog.h"
check "不判定状态对外可见"         "grep -q 'mg24_judged' ../webui/rest_api.c"
# D：线路电平是不花钱的负面证据，让 A 敢于退避而不变瞎。
check "有线路电平判据"             "awk '/^static void probe_mg24\\(void\\)/,/^}/' wd_probes.c | python3 ../../tools/grep_code.py -q 'uart_bridge_line_says_absent' /dev/stdin"
check "线路判据要连续多个 tick"    "awk '/^static void probe_mg24\\(void\\)/,/^}/' wd_probes.c | python3 ../../tools/grep_code.py -q 's_line_absent < MG24_LINE_ABSENT_TICKS' /dev/stdin"
# 两种配置各一个 MG24 探针（直连型的 probe_mg24_zhub 在 #if CONFIG_BITENGINE_ENABLE_ZHUB 里，2026-10-22），每个探针里"宣布活着"各只有一处。
# 上面几条的 awk / index 都精确到 probe_mg24(void)：前缀匹配会先撞上直连型的 probe_mg24_zhub
check "存活结论只有一个来源（每种配置的探针里各一处）" "[ \$(awk '/^static void probe_mg24\\(void\\)/,/^}/' wd_probes.c | grep -c 's_view.mg24_ok = true') -eq 1 ] && [ \$(awk '/^static void probe_mg24_zhub\\(void\\)/,/^}/' wd_probes.c | grep -c 's_view.mg24_ok = true') -eq 1 ] && [ \$(grep -c 's_view.mg24_ok = true' wd_probes.c) -eq 2 ]"
check "唯一那处在证据路径里"       "grep -B2 's_view.mg24_ok = true' wd_probes.c | grep -q 'if (evidence)'"
check "线路采样不发字节"           "awk '/^void uart_bridge_sample_lines/,/^}/' ../ezsp_bridge/uart_bridge.c | python3 ../../tools/grep_code.py --absent -q 'uart_write|uart_bridge_write' /dev/stdin"

echo ""
echo "--- T9.27 真复位原因 / T9.30 ①④（2026-10-08）---"
# 主机测试只测了函数本身，这几条钉调用点：漏一处，告警就又发不出去或在升级中途重启。
GC="python3 ../../tools/grep_code.py"
check "补重启前留下真原因、之后取回" "python3 -c \"
import sys
s=open('../../main/main.c',encoding='utf-8').read()
a=s.index('wd_boot_reason_note(rst);'); b=s.index('esp_restart();', a); c=s.index('wd_boot_reason_latch();')
sys.exit(0 if a < b < c else 1)\""
check "check_boot_reason 不再直接读复位原因" "awk '/^static void check_boot_reason/,/^}/' health_monitor.c | $GC -q 'wd_boot_reason_get\(\)' /dev/stdin && awk '/^static void check_boot_reason/,/^}/' health_monitor.c | $GC --absent -q 'esp_reset_reason\(\)' /dev/stdin"
check "整机重启先等 OTA、等完才写取证" "awk '/^static void do_reboot/,/^}/' wd_recovery.c | python3 -c \"
import sys
s=sys.stdin.read(); a=s.find('wait_for_ota();'); b=s.find('wd_forensics_record(')
sys.exit(0 if 0 <= a < b else 1)\""
check "OTA 查询覆盖三条路并已注册"   "$GC -q 'webui_ota_busy\(\) \|\| ota_cloud_busy\(\) \|\| ota_mg24_busy\(\)' ../../main/main.c && $GC -q 'wd_recovery_set_ota_busy_query\(any_ota_busy\)' ../../main/main.c"
check "异常开机事件带 cause 出去"     "$GC -q '&s_abnormal_boot, sizeof\\(s_abnormal_boot\\)' health_monitor.c && grep -q 'ev->cause' ../webhook/webhook.c && grep -q 'ev->cause' ../mqtt_client/bitengine_mqtt.c"

echo ""
echo "--- 设计文档与实现一致（7.6 规矩 3：重述了就配判据）---"
D=../../docs/04_watchdog_design_v2.md
# 正本是 v2 第六节。这几条钉的是"错了会误导人"的硬事实，不比对自然语言。
internal "v2 §6.1 讲了三态"            "grep -q 'mg24_judged' \$D"
internal "v2 §6.1 讲了无客户端不探测"  "grep -q 'MG24_CONFIRM_INTERVAL_S' \$D"
internal "v2 §6.1 讲了线路判据只判死"  "grep -q 'uart_bridge_line_says_absent\\|只能当负面判据' \$D"
# v1 第五、六章已被取代，打开 v1 的人必须看得到指针，否则会照着废稿写代码。
internal "v1 第六章有取代指针"         "grep -A6 '^## 六、health_task 实现' ../../docs/04_watchdog_design_v1.md | grep -q '04_watchdog_design_v2'"
internal "索引指向现行正本"            "grep -q '写看门狗代码.*04_watchdog_design_v2' ../../CLAUDE.md"
# 文档说 MG24_FAIL_THRESHOLD 是死宏。它哪天被人接回去，这条就该红。
check "FAIL_THRESHOLD 仍是死宏"     "! python3 ../../tools/grep_code.py -q 'MG24_FAIL_THRESHOLD' *.c"
check "accept 期间避让探测"         "grep -q 'wd_mg24_probe_active' ../ezsp_bridge/tcp_server.c"
check "Thread 射频探针复用缓存"     "grep -q 'thread_br_get_health' wd_probes.c"
check "网络探针要正向证据"          "grep -q 'network_mgr_get_status' wd_probes.c"

echo ""
echo "--- v2 层 E：恢复引擎 ---"
check "策略表存在"                  "grep -q 's_policy' wd_recovery.c"
check "恢复动作不在扫描任务里执行"  "grep -q 'wd_action' wd_recovery.c"
check "动作任务用内部栈(会写NVS)"   "! grep -q 'psram_task_create(wd_action' wd_recovery.c"
check "L4 前写取证"                 "grep -B12 'esp_restart' wd_recovery.c | grep -q 'wd_forensics_record'"
check "重启风暴保护"                "grep -q 'SELF_HEAL_MAX' wd_recovery.c"
check "安全模式可退出"              "grep -q 'wd_safe_mode_exit' wd_recovery.c"
check "L5 未实现(等 AM2)"           "! grep -q 'MR_PIN' wd_recovery.c"
check "MG24 不自动整机复位"         "grep -q 'MG24 不自动' wd_recovery.c || grep -q 'no_auto_reset' wd_recovery.c"

echo ""
echo "--- v2 层 G：取证 ---"
check "RTC_NOINIT 记录"             "grep -q 'RTC_NOINIT_ATTR' wd_forensics.c"
check "NVS 环形历史"                "grep -q 'WD_HISTORY_MAX' wd_forensics.c"
check "自愈重启可识别"              "grep -q 'wd_forensics_last_was_self_heal' wd_forensics.c"

echo ""
echo "--- v2 层 G：可观测 ---"
check "/api/v1/diag/watchdog 已注册" "grep -q 'diag/watchdog' ../webui/rest_api.c"
check "status 新增字段而非改语义"    "grep -q 'internal_free_heap' ../webui/rest_api.c && grep -q 'min_free_heap' ../webui/rest_api.c"
check "safe_mode 上报"               "grep -q 'safe_mode' ../webui/rest_api.c"

echo ""
echo "--- v2 事件消费者全覆盖 ---"
for e in MG24_RESET HEAP_LOW HEAP_CRITICAL HEAP_LEAK_SUSPECTED TEMP_HIGH TEMP_CRITICAL ABNORMAL_BOOT WIFI_RSSI_LOW UART_ERROR_RATE_HIGH; do
    check "事件 $e 有消费者" "grep -rq 'WATCHDOG_EVENT_$e' ../webhook ../mqtt_client ../led_ctrl"
done

echo ""
echo "==============================="
echo ""
echo "--- 不得持有可被删除任务的句柄（2026-08-11 整板重启第二真因）---"
# wd_registry 曾把 TaskHandle_t 存起来供层 D 事后查栈余量。任务一自删，
# 句柄就指向已释放的 TCB，health_task 每 tick 解引用一次 -> Load access fault。
# 改成"任务自己在 wd_kick 里采值、只缓存整数"后，没有指针可悬空。
check "registry 不存任务句柄"     "! grep -q 'TaskHandle_t  *handle' wd_registry.c"
check "stack_low 只读缓存值"      "! awk '/void wd_registry_stack_low/,/^}/' wd_registry.c | grep -q 'uxTaskGetStackHighWaterMark'"
check "栈余量在任务上下文自采"    "awk '/^void wd_kick/,/^}/' wd_registry.c | grep -q 'uxTaskGetStackHighWaterMark(NULL)'"
check "采样有节流"                "grep -q 'WD_STACK_SAMPLE_INTERVAL_MS' wd_registry.c"

check "无主槽位扫描时跳过"        "awk '/^bool wd_registry_scan/,/^}/' wd_registry.c | grep -q 'offline'"
check "kick 自动清除无主标记"      "awk '/^void wd_kick/,/^}/' wd_registry.c | grep -q 'offline = false'"

echo ""
echo "--- 层 H 注入必须真的作用到探针上（2026-08-16）---"
# 此前 wd_fault_active() 全仓库零调用点：注入 mg24_mute / thread_radio_freeze
# 返回 200，设备行为完全不变，TC-W04/W05/W06 验的是空气。
check "wd_fault_active 有消费者"   "grep -q 'FAULT_ACTIVE' wd_probes.c"
check "mg24_mute 抹掉存活证据"     "awk '/^static void probe_mg24\\(void\\)/,/^}/' wd_probes.c | grep -q 'FAULT_ACTIVE(\"mg24_mute\")'"
check "thread_freeze 判为停摆"     "awk '/^static void probe_thread/,/^}/' wd_probes.c | grep -q 'FAULT_ACTIVE(\"thread_radio_freeze\")'"
# 注入关闭时必须编译为常假，否则出厂固件里会留下入口
check "注入关时 FAULT_ACTIVE 为假" "grep -q 'define FAULT_ACTIVE(k)  (false)' wd_probes.c"
# 冻结要在 !running 早退之前判：真实故障正是"协议栈活着、只有射频哑了"
check "freeze 判在 running 早退前" "awk '/^static void probe_thread/,/^}/' wd_probes.c | grep -nE 'FAULT_ACTIVE\(\"thread_radio_freeze\"\)|if \(!running\)' | head -1 | grep -q FAULT_ACTIVE"


echo ""
echo "--- 诊断堆追踪 wd_heaptrace（2026-10-05，failover DMA 低谷排查；出厂固件不编进去）---"
GC="python3 ../../tools/grep_code.py"
fn_body() { awk -v re="$1" '$0 ~ re {f=1} f&&/^}/{print; exit} f' wd_heaptrace.c; }
check "Kconfig 出厂默认 n"                    "awk '/config BITENGINE_DIAG_HEAP_TRACE/{f=1} f' ../../main/Kconfig.projbuild | head -6 | grep -q 'default n'"
check "出厂 sdkconfig 没开"                   "grep -q '^# CONFIG_BITENGINE_DIAG_HEAP_TRACE is not set' ../../sdkconfig && ! grep -q 'CONFIG_BITENGINE_DIAG_HEAP_TRACE=y' ../../sdkconfig.defaults"
check "整个实现包在 #if 里（出厂为空文件）"     "[ \$(grep -c '^#if CONFIG_BITENGINE_DIAG_HEAP_TRACE\$' wd_heaptrace.c) -eq 1 ] && grep -v '^\s*\$' wd_heaptrace.c | tail -1 | grep -q '^#endif /\* CONFIG_BITENGINE_DIAG_HEAP_TRACE \*/'"
check "main.c 的 init 调用包在 #if 里"        "awk '/^#if CONFIG_BITENGINE_DIAG_HEAP_TRACE/{f=1} f&&/wd_heaptrace_init\(\)/{print \"hit\"} /^#endif/{f=0}' ../../main/main.c | grep -q hit"
check "init 在网络起来之前（先于 network_mgr 订阅断线）" "[ \$(grep -n 'wd_heaptrace_init()' ../../main/main.c | cut -d: -f1) -lt \$(grep -n 'ret = network_mgr_init();' ../../main/main.c | cut -d: -f1) ]"
check "两个钩子都在 IRAM（堆函数没挪去 flash，声明上的 HEAP_IRAM_ATTR 即 IRAM_ATTR）" "( for f in ../../sdkconfig ../../build_diag_heap/sdkconfig; do [ ! -f \$f ] || grep -q '^# CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH is not set' \$f || exit 1; done ) && grep -q 'HEAP_IRAM_ATTR void esp_heap_trace_alloc_hook' \${IDF_PATH:-\$HOME/esp-idf-v6.0.2}/components/heap/include/esp_heap_caps.h && grep -q 'HEAP_IRAM_ATTR void esp_heap_trace_free_hook' \${IDF_PATH:-\$HOME/esp-idf-v6.0.2}/components/heap/include/esp_heap_caps.h"
check "钩子先查 cache 再进慢路径"              "( for h in alloc free; do fn_body \"^void esp_heap_trace_\${h}_hook\" | \$GC -q 'spi_flash_cache_enabled\(\)' /dev/stdin || exit 1; fn_body \"^void esp_heap_trace_\${h}_hook\" | awk '/spi_flash_cache_enabled/{c=NR} /ht_on_/{o=NR} END{exit !(c && o && c<o)}' || exit 1; done )"
check "钩子不调可能落在 flash 的 inline 判断（-Og）" "( for h in alloc free; do fn_body \"^void esp_heap_trace_\${h}_hook\" | \$GC --absent -q 'esp_ptr_dma_capable|esp_ptr_internal' /dev/stdin || exit 1; done )"
check "钩子路径里不分配、不打日志"             "( for f in '^static void ht_on_alloc' '^static void ht_on_free' '^static int h_find' '^static void h_put'; do fn_body \"\$f\" | \$GC --absent -q 'malloc|calloc|realloc|ESP_LOG|printf' /dev/stdin || exit 1; done )"
check "记录表与索引都在 PSRAM"                 "[ \$(fn_body '^esp_err_t wd_heaptrace_init' | \$GC 'heap_caps_calloc\(.*MALLOC_CAP_SPIRAM\)' /dev/stdin | wc -l) -eq 3 ]"
check "下标装得进 uint16、索引大于记录上限"     "python3 -c \"import re;s=open('wd_heaptrace.c').read();m=int(re.search(r'#define HT_MAX_RECS\s+(\d+)u',s).group(1));b=int(re.search(r'#define HT_HASH_BITS\s+(\d+)',s).group(1));assert m<=65536 and (1<<b)>m\""
check "断线只在 IDLE 时自动开窗（抖动不覆盖第一次结果）" "fn_body '^static void ht_eth_down_handler' | \$GC -q 's_state != WD_HT_IDLE\) return;' /dev/stdin"
check "临界区内复查 s_armed"                   "( for f in '^static void ht_on_alloc' '^static void ht_on_free'; do fn_body \"\$f\" | \$GC -q 'portENTER_CRITICAL' /dev/stdin && fn_body \"\$f\" | \$GC -q 'if \(s_armed\)' /dev/stdin || exit 1; done )"

check "realloc 搬家补记旧块释放：--wrap 与包装函数成对、都只在诊断开关下" "python3 ../../tools/grep_code.py -q '__wrap_heap_caps_realloc_base\\(' wd_heaptrace.c && awk '/^if\\(CONFIG_BITENGINE_DIAG_HEAP_TRACE\\)/{f=1} f&&/^endif/{exit} f' CMakeLists.txt | grep -q -- '--wrap=heap_caps_realloc_base'"
check "开窗先在临界区占位（并发开窗不互相清索引）" "awk '/^esp_err_t wd_heaptrace_arm/{f=1} f&&/^}/{exit} f' wd_heaptrace.c | python3 ../../tools/grep_code.py -q 'bool mine = \\(s_state != WD_HT_ARMED\\);' /dev/stdin && awk '/^esp_err_t wd_heaptrace_arm/{f=1} f&&/^}/{exit} f' wd_heaptrace.c | awk '/bool mine/{a=NR} /memset\\(s_hkey/{b=NR} END{exit !(a && b && a<b)}'"

echo ""
echo "--- Thread 射频停摆：L2 之后直接 L4，没有 L3（T9.18 方案 A，2026-10-07）---"
# L3 原本是 thread_br_stop() → start() 整栈重启，正常态实测不可用（IDF 侧三个缺陷，其一 panic，
# 记录在 components/webui/otbr_api.c 末尾）。行为本身由 test_host/recovery_test.c 与 probes_test.c 锁住，
# 这里钉住"别有人顺手把 L3 接回去"和"探针别退回只报一次"。
TGC="python3 ../../tools/grep_code.py"
check "策略覆盖：thread 封顶 L4 且跳过 L3"            "$TGC --strings -q '\\{ *\"thread\", *WD_LEVEL_REBOOT, *true *\\}' wd_recovery.c"
check "覆盖项有自己的级别/冷却（不与网络共用一份）"  "awk '/^static void handle\\(const wd_action_msg_t \\*m\\)\$/,/^}\$/' wd_recovery.c | $TGC -q 's_ov_state\\[' /dev/stdin"
check "thread_br 不再登记 L3 入口"                    "$TGC --absent --strings -q 'wd_register_recovery\\(\"(thread|ot_main)\"' ../thread_br/thread_br.c"
check "整栈重启入口已删（thread_br_restart）"          "$TGC --absent -q 'thread_br_restart' ../thread_br/thread_br.c ../thread_br/include/thread_br.h"
check "l3 注入已删（Thread 没有 L3 了，留着只会触发整机重启）" "$TGC --absent --strings -q '\"l3\"' wd_fault_inject.c"
check "持续停摆定期再报（只报一次就永远升不上去）"   "awk '/^static void probe_thread\\(void\\)\$/,/^}\$/' wd_probes.c | $TGC -q 'THREAD_REREPORT_S' /dev/stdin"
check "再报间隔不短于 L2 冷却（短了会被冷却吞掉，白报）" "python3 - <<'PY'
import re,sys
p=open('wd_probes.c').read(); r=open('wd_recovery.c').read()
a=int(re.search(r'#define THREAD_REREPORT_S\\s+(\\d+)', p).group(1))
b=int(re.search(r'\\[WD_LEVEL_TARGET\\]\\s*=\\s*(\\d+)', r).group(1))
sys.exit(0 if a >= b else 1)
PY"
check "用户停用 Thread 不算停摆（否则约 4 分钟后整机重启）" "awk '/^static void child_stats_timer_cb\\(void \\*arg\\)\$/,/^}\$/' ../thread_br/thread_br.c | $TGC -q 'if \\(running && config_mgr_get_thread\\(\\)->enabled\\)' /dev/stdin"
check "OT 主循环意外退出报给看门狗（不再静默）"     "awk '/^static void ot_main_task\\(void \\*arg\\)\$/,/^}\$/' ../thread_br/thread_br.c | $TGC --strings -q 'wd_report\\(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, \"thread\", NULL\\)' /dev/stdin"
check "报过停摆后 Thread 停了要报 recovered"        "awk '/^static void probe_thread\\(void\\)\$/,/^}\$/' wd_probes.c | awk '/if \\(!running\\)/{f=1} f' | head -12 | $TGC -q 'wd_report_recovered' /dev/stdin"
check "「已恢复」走队列（与上报同序，级别只在 wd_action 里改）" "awk '/^void wd_report_recovered/,/^}\$/' wd_recovery.c | $TGC -q 'xQueueSend\\(s_queue' /dev/stdin"
check "主机测试锁住 thread L2→L4、网络不受影响、探针再报" "grep -q 'test_thread_l2_then_reboot' test_host/recovery_test.c && grep -q 'test_network_still_capped_at_l3' test_host/recovery_test.c && grep -q 'probes' test_host/run.sh && [ -f test_host/probes_test.c ]"

check "T9.36：复用 / 无主时把已上报停转记成待确认恢复，新实例真 kick 过才发恢复" \
    "[ \$(grep -c 'mark_recover_pending(&s_slots' wd_registry.c) -eq 2 ] && grep -q 's->recover_pending && s->last_kick_ms != s->recover_mark_ms' wd_registry.c"

check "槽数 / 堆水位标记上限按配置分开：直连型 20 / 32，HA 型仍 16 / 24（docs/15 设计第十五节 A-24）" "python3 - <<'PY'
import re,sys
s=re.sub(r'/\*.*?\*/', '', open('include/watchdog.h').read(), flags=re.S)
def two(name):
    m=re.search(r'#if CONFIG_BITENGINE_PROFILE_DIRECT\s*#define %s (\d+)\s*#else\s*#define %s (\d+)\s*#endif' % (name, name), s)
    return m and (int(m.group(1)), int(m.group(2)))
ok = two('WD_MAX_TASKS') == (20, 16) and two('HEAP_MARK_MAX') == (32, 24) and len(re.findall(r'#define (WD_MAX_TASKS|HEAP_MARK_MAX)\b', s)) == 4
sys.exit(0 if ok else 1)
PY"
check "主机测试按直连型再编一次注册表（上限按配置）" "grep -q 'DCONFIG_BITENGINE_PROFILE_DIRECT=1 -o \"\$OUT/registry_direct\"' test_host/run.sh"

# 恢复引擎停转的应急重启写明它卡在哪一步（直连型；2026-10-24 停转 120 s → 应急重启 → 卡进下载模式，断电后 RTC 里的线索全没了）
check "应急重启的历史带上恢复引擎的阶段（idle / msg / rec / L1–L4 / ota），只在直连型" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('wd_recovery.c').read(), keep_strings=True)
e=re.search(r'void wd_recovery_emergency\(const char \*detail\)\n\{(.*?)\n\}', S, re.S).group(1)
ok = re.search(r'#if CONFIG_BITENGINE_PROFILE_DIRECT\s*char what\[24\];\s*wd_recovery_doing\(what, sizeof\(what\)\);\s*wd_forensics_record\(WD_FAULT_TASK_STALL, WD_LEVEL_REBOOT, what\);\s*#else\s*wd_forensics_record\(WD_FAULT_TASK_STALL, WD_LEVEL_REBOOT, \\\"wd_action\\\"\);\s*#endif', e)
ok = ok and all(('DOING(\\\"%s\\\"' % k) in S for k in ('idle', 'L1', 'L2', 'L3', 'L4', 'ota')) and 'DOING(m->recovered ? \\\"rec\\\" : \\\"msg\\\", m->detail);' in S
ok = ok and re.search(r'#else\s*#define DOING\(what, det\) do \{ \} while \(0\)', S)
H=open('include/watchdog.h').read()
ok = ok and re.search(r'#if CONFIG_BITENGINE_PROFILE_DIRECT[^#]*void wd_recovery_doing\(char \*out, size_t n\);\s*#endif\s*#ifdef __cplusplus\s*\}\s*#endif\s*$', H)
sys.exit(0 if ok else 1)\""

echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
