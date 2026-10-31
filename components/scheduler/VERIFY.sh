#!/bin/bash
# components/scheduler/VERIFY.sh —— bash components/scheduler/VERIFY.sh
#
# ⚠️ 部分覆盖。2026-08-17 把 scheduler 任务栈迁到 PSRAM 时建起来的，
# 目前只锁「迁 PSRAM 的前提不能被破坏」这一件事 + 几条基本形状。
# docs/07 第七节的其余约束尚未转成断言。
PASS=0; FAIL=0
check() { if eval "$2" >/dev/null 2>&1; then echo "✅ $1"; ((PASS++)); else echo "❌ $1"; ((FAIL++)); fi; }
cd "$(dirname "$0")"

echo "--- 任务栈在 PSRAM，其前提是本模块不碰 flash/NVS ---"
# psram_task.h 规则 1：cache 关闭期间访问 PSRAM 栈 = 随机崩溃。
# scheduler.c 里那行 "OTA check would go here" 一旦接上云端 OTA 并写 NVS
# 记录检查时间，这条会红 —— 那时必须把 psram_task_create 改回 xTaskCreate。
check "用 psram_task_create"      "grep -q 'psram_task_create(scheduler_task' scheduler.c"
# 要求真正的调用形态（带左括号），否则注释里提一句 nvs_ 就会误红
check "不碰 NVS/flash"            "! grep -qE '(nvs_|esp_partition_|esp_flash_)[a-z0-9_]*\\(' scheduler.c"
check "不直接改配置（会落盘）"     "! grep -q 'config_mgr_set' scheduler.c"
check "任务不自删（规则 2）"       "! grep -q 'vTaskDelete(NULL)' scheduler.c"

echo ""
echo "--- 三件事 ---"
check "夜间模式判定"              "grep -q 'check_night_mode' scheduler.c"
check "自动备份"                  "grep -q 'check_auto_backup' scheduler.c"
check "夜间窗口对外可读"           "grep -q 'scheduler_night_active' include/scheduler.h"
# 备份只发通知不搬配置：webhook 队列项装不下整份配置
check "备份走 webhook 不搬配置"    "awk '/check_auto_backup/,/^}/' scheduler.c | grep -q 'webhook_post_event'"

echo ""
echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
