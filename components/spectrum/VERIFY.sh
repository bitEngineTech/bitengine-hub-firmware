#!/bin/bash
# Phase 3 VERIFY.sh — Spectrum + Thread BR (+ the retired Turbo Mode must stay retired)
# CREATED BEFORE ANY IMPLEMENTATION CODE (P1)
BASE="$(dirname "$0")/.."
PASS=0; FAIL=0
check() { if eval "$2" > /dev/null 2>&1; then echo "✅ $1"; ((PASS++)); else echo "❌ $1"; ((FAIL++)); fi; }

echo "=== Phase 3 Verification ==="

echo ""
echo "--- spectrum module ---"
check "spectrum.h exists"              "test -f $BASE/spectrum/include/spectrum.h"
check "spectrum_scan.c exists"         "test -f $BASE/spectrum/spectrum_scan.c"
check "beacon_scan.c exists"           "test -f $BASE/spectrum/beacon_scan.c"
check "channel_recommend.c exists"     "test -f $BASE/spectrum/channel_recommend.c"
# T4.1：整份 spectrum_history 已删（等一颗 BOM 永久不上件的 W25Q32 的空壳）。
# 反向不变量：别让它以任何形式回来，除非真的实现了 NVS 环形缓冲。
check "spectrum_history.c 已删除"      "! test -f $BASE/spectrum/spectrum_history.c"
check "spectrum_monitor.c exists"      "test -f $BASE/spectrum/spectrum_monitor.c"
check "spectrum_init impl"             "grep 'spectrum_init' $BASE/spectrum/*.c"
check "spectrum_energy_scan impl"      "grep 'spectrum_energy_scan' $BASE/spectrum/spectrum_scan.c"
check "spectrum_beacon_scan impl"      "grep 'spectrum_beacon_scan' $BASE/spectrum/beacon_scan.c"
check "spectrum_recommend_channel"     "grep 'spectrum_recommend_channel' $BASE/spectrum/channel_recommend.c"
# ⚠️ 用 grep_code.py 而不是裸 grep：上面那段注释本身就在解释"为什么删掉
#    spectrum_get_history"，裸 grep 会被自己的说明文字绊倒（同一天第五次了）。
check "spectrum_get_history 声明已删"  "python3 ../../tools/grep_code.py --absent -q spectrum_get_history $BASE/spectrum/include/spectrum.h"
check "spectrum_background_check"      "grep 'spectrum_background_check' $BASE/spectrum/spectrum_monitor.c"
check "esp_ieee802154 API used"        "grep 'esp_ieee802154' $BASE/spectrum/spectrum_scan.c"
check "16 channels scanned"            "grep -E '11|26|16' $BASE/spectrum/spectrum_scan.c"
check "spectrum 不再依赖外部 Flash"    "python3 ../../tools/grep_code.py --absent -q HAS_EXT_FLASH $BASE/spectrum/*.c $BASE/spectrum/include/*.h"
check "/spectrum/history 路由已摘掉"    "! grep -q 'spectrum/history' $BASE/webui/rest_api.c"

echo ""
echo "--- T4.8：OT 构型下的邻居扫描（2026-09-13 接上，此前恒 501）---"
# 出货构型带 OpenThread，裸 esp_ieee802154_transmit() 会撞 OT 端口的 ISR 断言，
# 所以 beacon 扫描只能走 OT 的主动扫描；而 otLinkActiveScan 结构性地看不见
# Zigbee 协调器（短地址 beacon 被 scan_result.cpp:50 丢弃），必须合并 PCAP 原始帧。
# 三条缺一不可：走 OT 扫描、合并 raw、标出自家（否则 recommend 把自己的信道当邻居扣分）。
check "OT 分支走 thread_br_active_scan"      "awk '/#else \/\* CONFIG_OPENTHREAD_ENABLED/,/#endif/' $BASE/spectrum/beacon_scan.c | grep -q 'thread_br_active_scan('"
check "合并 PCAP 原始 beacon（短地址协调器）" "awk '/#else \/\* CONFIG_OPENTHREAD_ENABLED/,/#endif/' $BASE/spectrum/beacon_scan.c | grep -q 'raw_count'"
check "OT 分支不再返回 NOT_SUPPORTED"          "python3 ../../tools/grep_code.py --absent -q ESP_ERR_NOT_SUPPORTED $BASE/spectrum/beacon_scan.c"
check "两个分支都拿 spectrum 锁"               "[ \$(grep -c 'spectrum_scan_lock_take' $BASE/spectrum/beacon_scan.c) -ge 2 ] && [ \$(grep -c 'spectrum_scan_lock_give' $BASE/spectrum/beacon_scan.c) -ge 2 ]"
check "结果结构带 self / completed"            "grep -q 'bool *self;' $BASE/spectrum/include/spectrum.h && grep -q 'bool *completed;' $BASE/spectrum/include/spectrum.h"
check "自家判定用到学到的 PAN"                 "awk '/#else \/\* CONFIG_OPENTHREAD_ENABLED/,/#endif/' $BASE/spectrum/beacon_scan.c | grep -q 'pan_id'"
check "推荐算法跳过自家网络"                   "awk '/spectrum_recommend_channel/,/^}/' $BASE/spectrum/channel_recommend.c | grep -q 'self'"
# ⚠️ 错误码在字符串字面量里，grep_code.py 默认连字符串一起剥 —— 不加 --strings 这条恒绿（P1 阶段就露馅了）
check "REST 不再 501 beacon_scan_unavailable"  "python3 ../../tools/grep_code.py --strings --absent -q beacon_scan_unavailable_with_thread $BASE/webui/rest_api.c"
check "REST 报 completed 与 self"              "awk '/static esp_err_t handle_spectrum_beacon/,/^}/' $BASE/webui/rest_api.c | grep -q 'completed' && awk '/static esp_err_t handle_spectrum_beacon/,/^}/' $BASE/webui/rest_api.c | grep -q '\"self\"'"
check "REST 分清 Thread 没跑（503）"           "awk '/static esp_err_t handle_spectrum_beacon/,/^}/' $BASE/webui/rest_api.c | grep -q 'thread_not_running'"
check "recommend 复用新鲜的 beacon 结果"       "awk '/static esp_err_t handle_spectrum_recommend/,/^}/' $BASE/webui/rest_api.c | grep -q 'spectrum_beacon_scan_cached'"
check "前端不再解释 501"                       "! grep -q 'beacon_scan_unavailable_with_thread' $BASE/../frontend/js/pages/spectrum.js"
check "前端渲染自家标记与未完成提示"           "grep -q 'n.self' $BASE/../frontend/js/pages/spectrum.js && grep -q 'completed' $BASE/../frontend/js/pages/spectrum.js"

echo ""
echo "--- thread_br module ---"
check "thread_br.h exists"            "test -f $BASE/thread_br/include/thread_br.h"
check "thread_br.c exists"            "test -f $BASE/thread_br/thread_br.c"
check "thread_radio_mgr.c exists"     "test -f $BASE/thread_br/thread_radio_mgr.c"
check "thread_br_init impl"           "grep 'thread_br_init' $BASE/thread_br/thread_br.c"
check "thread_br_pause impl"          "grep 'thread_br_pause' $BASE/thread_br/thread_radio_mgr.c"
check "thread_br_resume impl"         "grep 'thread_br_resume' $BASE/thread_br/thread_radio_mgr.c"
check "thread_br_is_running impl"     "grep 'thread_br_is_running' $BASE/thread_br/*.c"
check "radio_mutex used"              "grep 'radio_mutex' $BASE/thread_br/thread_radio_mgr.c"
check "config.thread.enabled check"   "grep 'cfg->enabled' $BASE/thread_br/thread_br.c"

echo ""
echo "--- turbo mode retired 2026-09-10 (T1.5: power is owned by Z2M/ZHA) ---"
# The switch used to show "(20 dBm Turbo)" from an NVS value that never reached
# the NCP. Lock in that it cannot come back by accident.
check "zigbee/turbo endpoint removed"          "! grep -q 'zigbee/turbo' $BASE/webui/rest_api.c $BASE/webui/static_files.c"
check "/status tags tx power source as host"   "grep -q 'zigbee_tx_power_source.*host' $BASE/webui/rest_api.c"
check "/status prints no tx_power value"       "! grep -q 'zigbee_tx_power\\\\\":' $BASE/webui/rest_api.c"
check "zigbee_config_t has no tx_power/turbo"  "! grep -E '^\\s*(int8_t|bool)\\s+(tx_power|turbo_mode)' $BASE/config_mgr/include/config_mgr.h"
check "config_mgr never touches tx_power NVS"  "! grep -q 'nvs_.*\"tx_power\"\\|nvs_.*\"turbo\"' $BASE/config_mgr/config_mgr.c"
check "config export carries no tx_power"      "! grep -q '\"tx_power\"\\|\"turbo_mode\"' $BASE/config_mgr/config_export.c | grep -v '^\\s*/\\*'"
check "no Turbo switch in zigbee page"         "! grep -qi 'zb-turbo\\|turboToggle' $BASE/../frontend/js/pages/zigbee.js $BASE/../frontend/js/api.js"
# 2026-09-15 b2e4f58：ZHA 改为按 HA 国家向芯片要推荐值（docs/14），页面不再给 ZHA 粘贴片段；
# Z2M 不问芯片，片段保留，数字 20 → 19（射频最高档 = FCC 上限，超出部分芯片按地区压住）。
check "zigbee page has no ZHA tx_power snippet" "! grep -q 'SNIPPET_ZHA\|[^_]tx_power: [0-9]' $BASE/../frontend/js/pages/zigbee.js"
check "zigbee page carries Z2M snippet"        "grep -q 'transmit_power: 19' $BASE/../frontend/js/pages/zigbee.js"
check "Z2M snippet carries the EU value too"   "grep -q 'transmit_power: 19.*EU/UK: 10' $BASE/../frontend/js/pages/zigbee.js"
check "ZHA one-time use_thread setup kept"     "grep -q 'use_thread: false' $BASE/../frontend/js/pages/zigbee.js"

echo ""
echo "--- scheduler integration ---"
check "spectrum_background in sched"  "grep 'spectrum_background\|spectrum' $BASE/scheduler/scheduler.c"

echo ""
echo "--- S1 security auto-checks ---"
check "No strcmp on secrets"           "! grep -rn 'strcmp.*password\|strcmp.*hash\|strcmp.*token' $BASE/spectrum/*.c $BASE/thread_br/*.c"
check "No secrets in logs"             "! grep -rn 'ESP_LOG.*->password\|ESP_LOG.*->url' $BASE/spectrum/*.c $BASE/thread_br/*.c"

echo ""
echo "--- S2 threading auto-checks ---"
check "No blocking in event callbacks" "! grep -B5 'event_handler' $BASE/spectrum/*.c $BASE/thread_br/*.c 2>/dev/null | grep 'client_stop\|vTaskDelay'"
# 扫描不再自己 pause/resume Thread：OT 构建下一律走 thread_br_energy_scan()（otLinkEnergyScan）。
# 射频归 OpenThread 端口所有，绕过它直接操作驱动两种错法都撞过（见 08 模块记录）。
# 非 OT 构建才允许用裸 esp_ieee802154_*，所以裸驱动调用必须在 CONFIG_OPENTHREAD_ENABLED 的 #else 分支里。
check "Scan goes through OT API"       "grep -q 'thread_br_energy_scan' $BASE/spectrum/spectrum_scan.c"
check "Raw radio drv is OT-guarded"    "! awk '/#ifdef CONFIG_OPENTHREAD_ENABLED/{g=1} /#else/{if(g)g=2} /#endif/{g=0} g==1 && /esp_ieee802154_(enable|set_channel|energy_detect)\\(/{found=1} END{exit !found}' $BASE/spectrum/spectrum_scan.c"

echo ""
echo "--- 能量扫描卡死 MAC 的回归防线（2026-08-09）---"
# ESP-IDF v6.0.2 的 OT port 没有实现 esp_ieee802154_ed_failed()（weak 空桩），
# ED 被共存仲裁器拒绝时 otPlatRadioEnergyScanDone() 永不到来，OpenThread 的 Mac
# 永久停在 kOperationEnergyScan —— 整个 Thread 网络死亡，只有整板重启能恢复。
# 三条不变量缺一不可：必须强制收尾、必须循环走完信道、必须如实报错。
check "Forced scan completion exists"  "grep -q 'force_finish_energy_scan' $BASE/thread_br/thread_br.c"
check "Forced completion calls OT done" "grep -q 'otPlatRadioEnergyScanDone' $BASE/thread_br/thread_br.c"
# 每次 Done 只推进一个信道（Mac::HandleEnergyScanDone → PerformEnergyScan），必须循环
check "Forced completion loops chans"  "awk '/static void force_finish_energy_scan/,/^}/' $BASE/thread_br/thread_br.c | grep -q 'for *('"
# 强制收尾后的数据是 -100 哨兵不是测量值，报 ESP_OK 等于伪造频谱
check "Forced scan reports failure"    "grep -q 'completed_naturally' $BASE/thread_br/thread_br.c"
check "REST distinguishes coex block"  "grep -q 'energy_scan_blocked_by_coex' $BASE/webui/rest_api.c"
# 推荐信道不能吃失败的扫描结果（-100 会被 normalize 成"最干净"）
check "Recommend rejects failed scan"  "awk '/handle_spectrum_recommend/,/^}/' $BASE/webui/rest_api.c | grep -q 'send_energy_scan_error'"

echo ""
echo "--- ED 诊断（P0，纯观测） ---"
# 强符号覆盖是本方案唯一会"静默失效"的地方：weak 桩已满足引用，
# 若定义被挪到一个没人引用的新 .c 里，ld 不会去抽它，且没有任何编译告警。
# ELF 里必须是 T（强符号），不能是 W。
NM=$(ls "$HOME"/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-nm 2>/dev/null | head -1)
ELF="$BASE/../build/bitengine.elf"
if [ -n "$NM" ] && [ -f "$ELF" ]; then
check "ed_failed override wins link"   "$NM $ELF | grep -q '^[0-9a-f]* T esp_ieee802154_ed_failed'"
else
echo "⏭  ed_failed override wins link（未编译或找不到 nm，跳过）"
fi
# 覆盖必须留在 thread_br.c 里 —— 该 .o 本来就会被链接进来
check "ed_failed lives in thread_br.c" "grep -q 'void esp_ieee802154_ed_failed' $BASE/thread_br/thread_br.c"
# 计数器清零与快照必须成对，否则读到的不是本次扫描的增量
check "counters cleared before scan"   "grep -q 'ieee802154_ll_clear_debug_cnt(ED_CNT_BITS)' $BASE/thread_br/thread_br.c"
# 定义 + 唯一调用点。扫描一旦开始就只有一条出口，两处以上说明有分支漏记快照。
check "scan diag recorded"             "[ \$(grep -c 'record_scan_diag(' $BASE/thread_br/thread_br.c) -ge 2 ]"
# 硬件计数器读取所需的组件依赖
check "esp_hal_ieee802154 required"    "grep -q 'esp_hal_ieee802154' $BASE/thread_br/CMakeLists.txt"
check "diag/ed endpoint registered"    "grep -q '\"/api/v1/diag/ed\"' $BASE/webui/rest_api.c"

echo ""
echo "--- 短驻留多轮扫描 + valid_mask 全链路 ---"
# 钩子必须真的转发，否则被拒的信道仍然只能靠超时收尾
check "ed_failed forwards to OT"       "awk '/void esp_ieee802154_ed_failed/,/^}/' $BASE/thread_br/thread_br.c | grep -q 'esp_ieee802154_energy_detect_done'"
# 转发必须有门控：不在扫描窗口内注入 EnergyScanDone 会踢掉 OT 正在进行的操作
check "forward gated by hook flag"     "awk '/void esp_ieee802154_ed_failed/,/^}/' $BASE/thread_br/thread_br.c | grep -q 's_ed_hook_armed'"
# 扫描窗口必须使能 ED 中断，否则钩子永远不触发（实测：不 arm = 0 次）
check "scan arms ED irq"               "grep -q 'ed_irq_arm()' $BASE/thread_br/thread_br.c"
check "scan restores ED irq"           "[ \$(grep -c 'ed_irq_restore(' $BASE/thread_br/thread_br.c) -ge 2 ]"
# 摘钩子必须早于强行收尾，否则中断和 force_finish 会各推进一次
check "hook cleared before force"      "awk '/static bool run_one_scan_pass/,/^}/' $BASE/thread_br/thread_br.c | grep -B2 'force_finish_energy_scan' | grep -q 's_ed_hook_armed = false'"
# max-hold：多轮里取最大值，不是最后一轮覆盖前面
check "callback does max-hold"         "awk '/static void energy_scan_cb/,/^}/' $BASE/thread_br/thread_br.c | grep -q 'mMaxRssi > s_escan.energy'"
# 驻留上界必须钳位：>1048ms 会在 IDF 里静默截断成完全不同的时长
check "dwell clamped to 1000"          "grep -q 'dwell_ms > 1000' $BASE/thread_br/thread_br.c"
check "dwell never zero"               "grep -q 'dwell_ms == 0' $BASE/thread_br/thread_br.c"
# 回归防线：默认驻留一旦被写回 100，扫描会静默回到 0/16（实测过一次）
check "spectrum default not 100ms"     "! grep -q 'duration_per_ch_ms = 100' $BASE/spectrum/spectrum_scan.c || grep -B2 'duration_per_ch_ms = 100' $BASE/spectrum/spectrum_scan.c | grep -q 'ifndef\\|else'"
check "REST dwell default delegates"   "awk '/scan_dwell_from_query/,/^}/' $BASE/webui/rest_api.c | grep -q 'return 0;'"

# valid_mask 必须贯穿到每一个消费 energy[] 的地方 —— 少一处就会有人把 -100 当能量
check "result carries valid_mask"      "grep -q 'valid_mask' $BASE/spectrum/include/spectrum.h"
check "scan fills valid_mask"          "grep -q 'valid_mask' $BASE/spectrum/spectrum_scan.c"
check "recommend uses valid_mask"      "grep -q 'valid_mask' $BASE/spectrum/channel_recommend.c"
check "recommend excludes invalid"     "awk '/spectrum_recommend_channel/,/^}/' $BASE/spectrum/channel_recommend.c | grep -q 'usable\[i\]'"
check "recommend no magic 15"          "! grep -q 'return 15;' $BASE/spectrum/channel_recommend.c"
check "monitor uses valid_mask"        "grep -q 'spectrum_ch_valid' $BASE/spectrum/spectrum_monitor.c"
check "monitor drops -100 sentinel"    "! grep -q 'energy\[i\] <= -100' $BASE/spectrum/spectrum_monitor.c"
# REST 不能把 -100 当能量发出去（前端会画成"最干净"）
check "REST emits null for invalid"    "grep -q 'energy..:null' $BASE/webui/rest_api.c"
check "REST reports valid_count"       "grep -q 'valid_count' $BASE/webui/rest_api.c"
check "REST says why no recommend"     "grep -q 'insufficient_spectrum_data' $BASE/webui/rest_api.c"

echo ""
echo "--- S3 resource auto-checks ---"
check "Semaphore give after take"      "grep -c 'xSemaphoreGive' $BASE/thread_br/thread_radio_mgr.c"

echo ""
echo "==============================="
echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
