#!/bin/bash
# components/log_store/VERIFY.sh — 设计约束的机器验证
# 编码前创建（P1），编译通过后运行：bash components/log_store/VERIFY.sh
cd "$(dirname "$0")"
PASS=0; FAIL=0
check() {
    if eval "$2" > /dev/null 2>&1; then echo "✅ $1"; ((PASS++)); else echo "❌ $1"; ((FAIL++)); fi
}

echo "=== log_store 模块验证 ==="

echo ""
echo "--- 文件与 API ---"
check "log_store.c 存在"        "test -f log_store.c"
check "log_store.h 存在"        "test -f include/log_store.h"
check "log_store_init"          "grep -q 'esp_err_t log_store_init' log_store.c"
check "log_store_fetch"         "grep -q 'int log_store_fetch' log_store.c"
check "log_store_stats"         "grep -q 'void log_store_stats' log_store.c"

echo ""
echo "--- S2 线程/中断安全（这是本模块最大的风险面）---"
# 日志来自所有任务，钩子必须无阻塞：禁止 mutex / 信号量 / vTaskDelay
check "钩子不用 mutex"          "! grep -qE 'xSemaphoreTake|xSemaphoreCreateMutex' log_store.c"
check "钩子不阻塞"              "! grep -q 'vTaskDelay' log_store.c"
check "用 spinlock 保护环形"    "grep -q 'portMUX_TYPE' log_store.c"
# ⚠️ ISR 里禁止碰 PSRAM：flash 写入期间 cache 关闭，从 ISR 访问 PSRAM 会崩
check "ISR 上下文直接跳过"      "grep -q 'xPortInIsrContext' log_store.c"
# 钩子里再打日志 = 无限递归
check "捕获路径不含 ESP_LOG"    "! awk '/^static void capture/,/^}/' log_store.c | grep -q 'ESP_LOG'"
check "va_list 先 va_copy"      "grep -q 'va_copy' log_store.c"
# 串口输出绝不能因为本模块而丢
check "原 vprintf 仍被调用"     "grep -q 's_orig' log_store.c"

echo ""
echo "--- 存储位置（不落 Flash，不动分区表）---"
check "环形缓冲在 PSRAM"        "grep -q 'MALLOC_CAP_SPIRAM' log_store.c"
check "掉电前尾巴用 RTC_NOINIT" "grep -q 'RTC_NOINIT_ATTR' log_store.c"
check "不写 NVS"                "! grep -q 'nvs_' log_store.c"
check "不写分区"                "! grep -q 'esp_partition' log_store.c"

echo ""
echo "--- 如实上报（丢了多少必须说出来）---"
# 环形缓冲溢出丢行时必须计数并上报，否则用户以为看到的是全部
check "有 dropped 计数"         "grep -q 'dropped' log_store.c"
check "stats 暴露 dropped"      "grep -q 'dropped' include/log_store.h"
check "截断有明确标记"          "grep -q 'truncat\|截断' log_store.c"
# 截断必须落在 UTF-8 字符边界（2026-10-06：中文长日志被切在字符中间，/logs 的 JSON 非法）。
# 两个截断点（msg 140 字节、RTC 尾巴行）都要过 utf8_cut；判据是代码里真的调用，不是注释提到
check "截断退到 UTF-8 字符边界（两处都用 utf8_cut）" \
    "[ \$(python3 ../../tools/grep_code.py 'utf8_cut\\(' log_store.c | grep -v 'static size_t utf8_cut' | wc -l) -ge 2 ]"
check "utf8_cut 认续字节 10xxxxxx" "python3 ../../tools/grep_code.py -q '& 0xC0\\) == 0x80' log_store.c"

echo ""
echo "--- 解析健壮性 ---"
# ESP-IDF 日志默认带 ANSI 颜色码，直接进 JSON 会变成乱码
check "剥离 ANSI 转义"          "grep -q '\\\\033\|0x1b\|ANSI' log_store.c"
# 解析失败必须降级保留原文，不能丢日志
check "解析失败降级为 '?'"      "grep -q \"'?'\" log_store.c"

echo ""
echo "--- REST 层 ---"
check "/logs 不再是 501"        "! grep -q '\"/api/v1/logs\".*handle_not_implemented' ../webui/rest_api.c"
check "/logs 接 handle_logs"    "grep -q '\"/api/v1/logs\".*handle_logs' ../webui/rest_api.c"
# 日志正文是任意文本：引号/反斜杠/换行必须**转义**而不是像 json_sanitize_copy 那样剥离，
# 否则日志会被改得面目全非
check "JSON 真转义而非剥离"     "python3 ../../tools/grep_code.py -q 'webui_json_escape\\(msg,' ../webui/rest_api.c"
# T9.24：正文里可能有外部送进来的任意字节（不用登录就能把 \xff 塞进 Host 头、被守卫写进
# W 日志），转义必须同时保证输出是合法 UTF-8 —— 行为测试在 webui/test_host（对照 Python）
check "转义器校验 UTF-8"        "python3 ../../tools/grep_code.py -q 'utf8_scan\\(' ../webui/json_escape.c"
check "支持 since 增量拉取"     "grep -q 'since' ../webui/rest_api.c"

echo ""
echo "==============================="
# ---- 告警环（2026-08-22）----
# 主环实测保留窗口只有 111 秒：丢弃率 90.4%（5637/6238），OPENTHREAD 的 INFO
# 占 88%。于是「刚才有没有报错」这个问题主环答不了 —— 用户报故障时现场早没了。
# 告警环只收 E/W，200 槽能装几天。
check "有独立告警环"          "grep -q '^#define WARN_SLOTS' log_store.c"
check "E/W 会写进告警环"      "grep -q \"rec.level == 'E' || rec.level == 'W'\" log_store.c"
check "告警环独立 head/count" "grep -q 's_whead' log_store.c && grep -q 's_wcount' log_store.c"
check "告警环有取数接口"      "grep -q '^int log_store_fetch_warn' log_store.c"
check "告警环接口已导出"      "grep -q 'log_store_fetch_warn' include/log_store.h"
# 分配失败要能降级，不能让主环跟着不启用
check "告警环分配失败可降级"  "grep -A2 's_wring = heap_caps_calloc' log_store.c | grep -q 'ESP_LOGW'"


# ---- tag 噪声过滤（2026-08-23）----
# 起因：主环最近 100 条里 93 条是 OpenThread 逐帧 INFO，600 槽只覆盖 5-9 分钟。
# ⚠️ 与告警环无关：W/E 早已单独存在 WARN_SLOTS 环里（实测 dropped=0），
#    错误告警本来就不丢。这里解决的是"事件前后的 INFO 上下文"。
echo ""
echo "--- tag 噪声过滤 ---"
noisy_body() { awk '/^static bool tag_is_noisy/,/^}/' log_store.c; }
check "过滤器存在"            "[ $(noisy_body | wc -l) -ge 5 ]"
# ★ 只挡 I/D/V。OpenThread 的 W/E 恰恰是排 Thread 故障时最该看到的
#   （射频静默死掉那类），一并挡掉等于把功能挡没了。
check "W/E 永远不被挡"        "noisy_body | grep -q \"level != 'I'\" && ! noisy_body | grep -qE \"level == 'W'|level == 'E'\""
check "按 tag 挡而不是按内容"  "noisy_body | grep -q 'strcmp(rec->tag'"
check "OPENTHREAD 在噪声表里"  "awk '/^static const char \\*const k_noisy_tags/,/^};/' log_store.c | grep -q '\"OPENTHREAD\"'"
# 被挡的行必须在分配 seq **之前**返回，否则客户端按 ?since= 翻页会看到空洞，
# 而且噪声会占满 RTC 那 8 行 —— 那是"重启前最后发生了什么"的唯一线索。
check "被挡的行不占 seq"      "[ $(awk '/^static void store_line/,/^}/' log_store.c | grep -n 'tag_is_noisy' | cut -d: -f1) -lt $(awk '/^static void store_line/,/^}/' log_store.c | grep -n 's_next_seq++' | cut -d: -f1) ]"
# filtered 与 dropped 分开：合成一个数会让人看着数字去加容量，而病根是噪声源
check "filtered 与 dropped 分开计" "grep -q 's_filtered' log_store.c && grep -q 's_dropped' log_store.c"
check "filtered 对外可见"      "grep -q 'log_store_filtered' include/log_store.h"


# ---- 告警环容量（2026-08-23）----
# 上板实测 72 条 W/E 每小时，200 槽只够 2.8 小时 —— 而"事后能回看"是这个环
# 存在的全部理由。谁为了省内存把它调回小值，这条会红。
# ⚠️ 判据是下限不是等值：往上调不该被拦。
# ⚠️ 用函数不用内联：变量/内联形态要经过 check 的 eval 再展开一次，
#    双重 eval 会把正则里的反斜杠吃掉（本文件 2026-08-23 已踩过一次）。
warn_slots() { awk '/^#define WARN_SLOTS/{print $3}' log_store.c; }
check "告警环 >= 1000 槽"      "[ $(warn_slots) -ge 1000 ]"
# 两个环都必须在 PSRAM：内部 RAM 只剩一万多字节，放这里会直接分配失败
check "告警环分配自 PSRAM"     "grep -A1 's_wring = heap_caps_calloc' log_store.c | grep -q 'MALLOC_CAP_SPIRAM' || grep 's_wring = heap_caps_calloc' log_store.c | grep -q 'MALLOC_CAP_SPIRAM'"
# 分配失败必须能优雅退回主环，否则 ?level=W 直接 500
check "告警环分配失败可退回主环" "grep -q 'want_warn && wcap == 0' ../webui/rest_api.c"

echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
