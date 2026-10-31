#!/bin/bash
cd "$(dirname "$0")"
PASS=0; FAIL=0

check() {
    if eval "$2" > /dev/null 2>&1; then
        echo "✅ $1"; ((PASS++))
    else
        echo "❌ $1"; ((FAIL++))
    fi
}

echo "=== Config Manager Verification ==="
echo ""
echo "--- Events (12 required) ---"
check "CONFIG_EVENT_WIFI_CHANGED posted"     "grep -r 'CONFIG_EVENT_WIFI_CHANGED' *.c | grep esp_event_post"
check "CONFIG_EVENT_ETH_CHANGED posted"      "grep -r 'CONFIG_EVENT_ETH_CHANGED' *.c | grep esp_event_post"
check "CONFIG_EVENT_MQTT_CHANGED posted"     "grep -r 'CONFIG_EVENT_MQTT_CHANGED' *.c | grep esp_event_post"
check "CONFIG_EVENT_WEBHOOK_CHANGED posted"  "grep -r 'CONFIG_EVENT_WEBHOOK_CHANGED' *.c | grep esp_event_post"
check "CONFIG_EVENT_LED_CHANGED posted"      "grep -r 'CONFIG_EVENT_LED_CHANGED' *.c | grep esp_event_post"
check "CONFIG_EVENT_ZIGBEE_CHANGED posted"   "grep -r 'CONFIG_EVENT_ZIGBEE_CHANGED' *.c | grep esp_event_post"
check "CONFIG_EVENT_THREAD_CHANGED posted"   "grep -r 'CONFIG_EVENT_THREAD_CHANGED' *.c | grep esp_event_post"
check "CONFIG_EVENT_NTP_CHANGED posted"      "grep -r 'CONFIG_EVENT_NTP_CHANGED' *.c | grep esp_event_post"
check "CONFIG_EVENT_HOSTNAME_CHANGED posted" "grep -r 'CONFIG_EVENT_HOSTNAME_CHANGED' *.c | grep esp_event_post"
check "CONFIG_EVENT_IMPORTED posted"         "grep -r 'CONFIG_EVENT_IMPORTED' *.c | grep esp_event_post"
check "CONFIG_EVENT_FACTORY_RESET posted"    "grep -r 'CONFIG_EVENT_FACTORY_RESET' *.c | grep esp_event_post"
check "CONFIG_EVENT_NVS_ERROR posted"        "grep -r 'CONFIG_EVENT_NVS_ERROR' *.c | grep esp_event_post"

echo ""
echo "--- Read API (11 functions) ---"
for fn in get_all get_wifi get_eth get_zigbee get_thread get_mqtt get_webhook get_led get_ntp get_sys get_whitelist; do
    check "config_mgr_$fn implemented" "grep 'config_mgr_${fn}' config_mgr.c | grep -v '//' | grep 'return'"
done

echo ""
echo "--- Write API (13 functions) ---"
for fn in set_wifi set_wifi_static_ip set_eth set_zigbee set_thread set_mqtt set_webhook set_led set_ntp set_hostname set_bridge_mode set_password set_whitelist; do
    check "config_mgr_$fn implemented" "grep -c 'config_mgr_${fn}' config_mgr.c | grep -v ':0'"
done

echo ""
echo "--- Other API ---"
check "config_mgr_init implemented"          "grep 'config_mgr_init' config_mgr.c | grep -v '//' | head -1"
check "config_mgr_save_now implemented"      "grep 'config_mgr_save_now' config_mgr.c | grep -v 'esp_err_t config_mgr_save_now' | head -1"
check "config_mgr_export_json implemented"   "grep 'config_mgr_export_json' config_export.c | head -1"
check "config_mgr_import_json implemented"   "grep 'config_mgr_import_json' config_export.c | head -1"
check "config_mgr_factory_reset implemented" "grep 'config_mgr_factory_reset' config_mgr.c | grep -v 'esp_err_t config_mgr_factory_reset' | head -1"
check "config_mgr_verify_password impl"      "grep 'config_mgr_verify_password' config_mgr.c | grep -v 'bool config_mgr_verify_password' | head -1"
check "config_mgr_has_password impl"         "grep 'config_mgr_has_password' config_mgr.c | grep -v 'bool config_mgr_has_password' | head -1"
# 清密码于 2026-08-14 撤销，2026-08-16 连实现一并删除。它是唯一能清空 password_hash
# 又不触发恢复出厂的入口，加回来等于把被否决的攻击面搬回来，且评审看不出异常。
check "no clear_password API"                "! grep -q 'esp_err_t config_mgr_clear_password' config_mgr.c include/config_mgr.h"
check "backup_to_flash implemented"          "grep 'config_mgr_backup_to_flash' config_backup.c | head -1"
check "restore_from_flash implemented"       "grep 'config_mgr_restore_from_flash' config_backup.c | head -1"

echo ""
echo "--- Core mechanisms ---"
check "2s debounce timer created"            "grep 'xTimerCreate' config_mgr.c"
check "xTimerReset in mark_dirty"            "grep 'xTimerReset' config_mgr.c"
check "config_mutex created"                 "grep 'xSemaphoreCreateMutex' config_mgr.c"
check "SHA256 password hashing"              "grep 'MBEDTLS_MD_SHA256' config_mgr.c"
check "NVS key pass->password migration"     "grep 'pass.*password\|Migrated' config_mgr.c"
check "Kconfig WiFi fallback"                "grep 'CONFIG_BITENGINE_WIFI_SSID' config_mgr.c"
check "config_set_defaults exists"           "grep 'config_set_defaults' config_defaults.c"
check "config_run_migrations exists"         "grep 'config_run_migrations' config_migrate.c"

echo ""
echo "--- WiFi migration ---"
check "wifi_mgr.c no direct nvs_open"        "! grep 'nvs_open' ../network_mgr/wifi_mgr.c"
check "wifi_mgr uses config_mgr_get_wifi"    "grep 'config_mgr_get_wifi' ../network_mgr/wifi_mgr.c"

echo ""
echo "==============================="

echo ""
echo "--- Zigbee 信道声明必须完整往返 ---"
# 只导出 channel 而不导出 channel_declared，恢复备份时会把"未声明"复活成
# "声明了某个值" —— 凭空造一个假声明，正是这一轮在消灭的东西。
check "导出带 channel_declared"    "grep -q 'channel_declared' config_export.c"
check "导出带 pan_id/pan_known"    "grep -q 'pan_id' config_export.c && grep -q 'pan_known' config_export.c"
check "导入也读这三项"             "[ $(grep -c 'IMPORT_.*channel_declared\|IMPORT_.*pan_id\|IMPORT_.*pan_known' config_export.c) -eq 3 ]"
# 老备份没有 channel_declared，其 channel 是从未被确认过的默认值。
check "老备份不被当成已声明"       "grep -q 'channel_declared &&' config_export.c"
check "出厂默认未声明"             "grep -q 'channel_declared = false' config_defaults.c"


# --- 恢复出厂不得连 Thread 网络一起擦 ---
# OpenThread 的数据集和网关配置同在 nvs 分区（thread_br.c 写死 storage_partition_name
# = "nvs"）。整分区擦 = 所有 Thread / Matter 设备变孤儿，只能是用户显式勾选的结果。
echo ""
echo "--- 恢复出厂范围 ---"
# 极性：只擦列举过的，没列举的一律不碰。反过来（列保留名单、其余全擦）曾经
# 擦掉 psa_its —— OpenThread 网络密钥的实际存放处 —— 导致 Thread 网络永久失联，
# 而数据集/ChildInfo/信道全都完好，所有检查项都是绿的。
check "只擦自有命名空间"         "grep -q 'OWNED_NS\[\]' config_mgr.c"
check "不遍历分区"               "! grep -q 'nvs_entry_find' config_mgr.c"
check "绝不整分区擦"             "! grep -q 'nvs_flash_erase_partition' config_mgr.c"
# psa_its 必须只在 WIPE_ALL 里擦：密钥材料在这，KEEP_THREAD 擦了 Thread 就废了
check "psa_its 只在 WIPE_ALL"    "grep -A3 'THREAD_NS\[\]' config_mgr.c | grep -q 'psa_its'"
check "Thread 三件套齐全"        "grep -A3 'THREAD_NS\[\]' config_mgr.c | grep -q 'openthread' && grep -A3 'THREAD_NS\[\]' config_mgr.c | grep -q 'thread_br'"
check "WIPE_ALL 才擦 Thread 组"  "grep -B2 'THREAD_NS\[i\]' config_mgr.c | grep -q 'scope == FACTORY_RESET_WIPE_ALL'"
# config_save_to_nvs() 写的每个命名空间都必须在 OWNED_NS 里，否则恢复出厂清不掉它
for ns in $(grep -o 'NVS_WRITE_NS("[a-z0-9_]*"' config_mgr.c | sed 's/.*("//;s/"//'); do
    check "OWNED_NS 覆盖 '$ns'" "grep -A6 'OWNED_NS\[\]' config_mgr.c | grep -q '\"$ns\"'"
done
# WiFi 驱动自己的凭据不擦的话，恢复出厂后设备会自动连回原来的 AP
check "擦 nvs.net80211"          "grep -A6 'OWNED_NS\[\]' config_mgr.c | grep -q 'nvs.net80211'"
# T9.20：network_mgr 自管的服务 IP 快照（components/network_mgr/network_mgr.c 的 SVC_NVS_NS）
# 也要随恢复出厂擦掉。用 sed 取整个 OWNED_NS[] 块，别用 -A<N> 死窗口（见下一条的说明）
check "OWNED_NS 覆盖 network_mgr 的 'net_svc'" \
  "sed -n '/OWNED_NS\[\] = {/,/^};/p' config_mgr.c | grep -q '\"net_svc\"' && grep -q 'SVC_NVS_NS *\"net_svc\"' ../network_mgr/network_mgr.c"
# T9.37（2026-10-07 用户拍板）：服务身份记录（设备地址属于哪个 DHCP 身份）**恢复出厂不擦** —— 它在 network_mgr 的
# 独立命名空间 net_id。擦了的话，先用 WiFi 装、后插网线的用户恢复出厂后设备地址会被定成网口的，HA 记的地址失效。
# 用 grep_code 剥注释（保留字符串）：OWNED_NS 里那段解释"为什么不擦 net_id"的注释本身就带着这个名字（7.4）。
check "恢复出厂不擦服务身份（net_id 不在 OWNED_NS / THREAD_NS）" \
  "sed -n '/OWNED_NS\[\] = {/,/^};/p' config_mgr.c | python3 ../../tools/grep_code.py --absent --strings -q '\"net_id\"' /dev/stdin && sed -n '/THREAD_NS\[\] = {/,/^};/p' config_mgr.c | python3 ../../tools/grep_code.py --absent --strings -q '\"net_id\"' /dev/stdin && grep -q 'SVC_ID_NVS_NS *\"net_id\"' ../network_mgr/network_mgr.c"
# 2026-10-08：重启前把 2 s 防抖里的配置写进去。设置页"重启"0.5 s 后就 esp_restart，改完设置马上点重启会悄悄丢掉改动。
# 关机钩子用不了（IDF 5 个槽已满），所以钉住每个用户 / 恢复动作能触发的重启入口：函数体里先 config_mgr_flush 再 esp_restart。
check "config_mgr_flush 拿锁带超时（发起重启的一方可能正持锁、或持锁的任务就是卡死的那个）" \
  "awk '/^esp_err_t config_mgr_flush\(/,/^}/' config_mgr.c | grep -q 'xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms))'"
for spec in "../webui/smlight_compat.c:handle_api2_reboot" "../watchdog/health_monitor.c:watchdog_restart_esp32" \
            "../webui/rest_api.c:handle_ota_www" "../webui/rest_api.c:handle_ota_esp32" \
            "../watchdog/wd_recovery.c:do_reboot" "../watchdog/wd_recovery.c:wd_safe_mode_exit"; do
    f=${spec%%:*}; fn=${spec##*:}
    check "重启前先落盘：$fn" \
      "awk '/^(static )?(esp_err_t|void) $fn\(/,/^}/' $f | python3 ../../tools/grep_code.py 'config_mgr_flush\(|esp_restart\(' /dev/stdin | head -1 | grep -q 'config_mgr_flush('"
done
# 反向不变量：Thread 三件套**绝不能**出现在 OWNED_NS 里。上面那几条守的是
# "THREAD_NS 齐全"，但没人拦着谁把 psa_its 顺手加进 OWNED_NS —— 那等于让
# KEEP_THREAD 又开始擦密钥材料，2026-08-15 的 bug 原样复活，且所有既有检查仍全绿。
# 用 sed 取 OWNED_NS[] 到它自己的 }; 之间，不要用 grep -A<N> —— 窗口是死的，
# OWNED_NS 里多加两行就会滑进下面 THREAD_NS 的注释区，把检查变成永远通过。
for ns in openthread psa_its thread_br; do
    check "OWNED_NS 不含 '$ns'" \
      "! sed -n '/OWNED_NS\[\] = {/,/^};/p' config_mgr.c | grep -q '\"$ns\"'"
done

# ---- 导出不得含机密（2026-08-22）----
# 起因：auth.c:176 是「没设密码 → 全部放行」，所以出厂网关上 /api/v1/config/export
# 对局域网任何人敞开，而它当时明文吐 wifi.password —— 泄露的正是我们声称的
# 那条信任边界本身。四个机密字段现在导出为占位符。
# ⚠️ 逐字段点名，不是数个数。2026-08-23 加 otbr_token 时这条计数判据变红了 ——
#    红得对（确实多了一个），但它表达的不是意图：把四个 ADD_SECRET 全挪到同一个
#    字段上、或者拿一个非机密字段去凑数，算术照样成立。点名的好处是新增机密时
#    **强制**在这里登记一笔，而把某个机密改回明文会立刻变红。
for f in password_hash password otbr_token url; do
    check "机密字段 $f 走 ADD_SECRET" "grep -q 'ADD_SECRET([a-z]*, \"$f\"' config_export.c"
done
check "ADD_SECRET 的数量与登记的一致" "[ $(grep 'ADD_SECRET(' config_export.c | grep -v '^#define' | wc -l) -eq 5 ]"
check "导出无裸机密字符串"          "! grep -qE 'cJSON_AddStringToObject\\((sys|wifi|mqtt|wh), \"(password_hash|password|url)\"' config_export.c"
# ⚠️ 这条最要紧：导入端必须跳过占位符。少了它，用户还原备份会把 WiFi 密码
#    真的写成 "********"，设备当场连不上网、只能按键恢复出厂。
for f in password_hash password otbr_token url; do
    check "导入端 $f 跳过占位符" "grep -q 'IMPORT_SECRET_STR([a-z]*, \"$f\"' config_export.c"
done
check "IMPORT_SECRET_STR 数量与登记的一致" "[ $(grep 'IMPORT_SECRET_STR(' config_export.c | grep -v '^#define' | wc -l) -eq 5 ]"
check "占位符跳过逻辑真的在"        "grep -A3 'define IMPORT_SECRET_STR' config_export.c | grep -q 'strcmp(_v->valuestring, CFG_REDACTED) != 0'"

echo ""
echo "--- T2.10 Thread 信道兜底与出厂默认必须同源 ---"
# 2026-09-11 之前：出厂默认 25，而配置导入的越界兜底写死 15 —— 正好压在 ZHA 的
# 默认信道上，导一份信道越界的备份就把两张射频网撞到一起，而 25 正是为避开它才选的。
check "THREAD_CHANNEL_DEFAULT 有定义"  "grep -q 'define THREAD_CHANNEL_DEFAULT' include/config_mgr.h"
check "出厂默认引用该常量"             "grep -q 'thread.channel *= *THREAD_CHANNEL_DEFAULT' config_defaults.c"
check "导入兜底引用该常量"             "grep -q 'thread.channel *= *THREAD_CHANNEL_DEFAULT' config_export.c"
check "两处都不再写死数字"             "! grep -qE 'thread\\.channel *= *[0-9]+;' config_defaults.c config_export.c"
check "默认值与 Z2M(11)/ZHA(15) 间隔都 > COEX_TOO_CLOSE_MAX" "python3 ./check_thread_default.py"

echo ""
echo "--- T2.6 NVS 落盘失败必须可见 ---"
# 当年 NTP 的教训：只检查了事件被 post，而没人订阅，功能整个是死的。
# 这里两头都查：post 了、有人接、而且计数真的被加。
check "两个失败点都 post NVS_ERROR" \
    "[ \$(grep -c 'CONFIG_EVENT_NVS_ERROR' config_mgr.c) -ge 2 ]"
check "两个失败点都记了计数" \
    "[ \$(grep -c 'nvs_health_record()' config_mgr.c) -ge 2 ]"
# ⚠️ 反向不变量：事件必须有**订阅者**，否则就是又一个 post 了没人接的死事件
check "NVS_ERROR 有订阅者（led_ctrl）" \
    "grep -q 'CONFIG_EVENT_NVS_ERROR' ../led_ctrl/led_ctrl.c"
check "订阅者真的注册了这个 id" \
    "grep -q 'esp_event_handler_instance_register(CONFIG_EVENT, CONFIG_EVENT_NVS_ERROR' ../led_ctrl/led_ctrl.c"
# ⚠️ 窗口要够大：那段说明注释本身就有 6 行，-A6 全被注释占满（第一版就是这么红的）。
check "订阅者做的事是点灯而不是只打日志" \
    "grep -A12 'id == CONFIG_EVENT_NVS_ERROR' ../led_ctrl/led_ctrl.c | grep -q 'event_enter(LED_STATE_ERROR'"
check "取值 API 已声明"  "grep -q 'config_mgr_get_nvs_health' include/config_mgr.h"
check "取值 API 已实现"  "grep -q 'void config_mgr_get_nvs_health' config_mgr.c"
check "/status 露出 nvs_write_failures" \
    "grep -q 'nvs_write_failures' ../webui/rest_api.c"
check "前端在非 0 时出横幅" \
    "grep -q 'nvs_write_failures' ../../frontend/js/pages/dashboard.js"

echo ""
echo "--- T3.2 主机名可改 + 校验长在 setter 里 ---"
check "setter 有真正的校验"        "grep -q 'static bool hostname_valid' config_mgr.c"
check "setter 调用了它"            "grep -q 'if (!hostname_valid(hostname))' config_mgr.c"
# ⚠️ 校验必须在 setter 里：REST 和**配置导入**是两条写入路径，只在 REST 那侧拦
#    等于 API 拒掉的值可以从一份备份里绕进来（extra_host 吃过这个亏）。
check "校验不是只查非空"           "! grep -A2 'esp_err_t config_mgr_set_hostname' config_mgr.c | grep -q \"strlen(hostname) == 0) return\""
check "导入路径补发 HOSTNAME_CHANGED" "grep -q 'CONFIG_EVENT_HOSTNAME_CHANGED' config_export.c"
check "HOSTNAME_CHANGED 有订阅者"   "grep -q 'CONFIG_EVENT_HOSTNAME_CHANGED' ../mdns_svc/mdns_svc.c"
check "REST 端点存在"               "grep -q 'system/hostname' ../webui/rest_api.c"
check "端点挂了跨站守卫"            "grep -A3 'handle_system_hostname' ../webui/rest_api.c | grep -q 'CROSS_ORIGIN_GUARD'"
check "前端有输入框"                "grep -q 'set-hostname-in' ../../frontend/js/pages/settings.js"

echo ""
echo "--- T4.2 等硬件的空壳已删（别让它回来）---"
# config_backup.c 四处实现全在 #ifdef HAS_EXT_FLASH 里等一颗 BOM 永久不上件的
# W25Q32，而 backup_to_flash() 还挂在**每次配置落盘**的路径上白调一次。
# ⚠️ 用 grep_code.py：头文件里有一整段在解释"为什么删掉它"（见 CLAUDE.md 7.4）。
check "config_backup.c 已删除"      "! test -f config_backup.c"
check "两个 API 声明已删"           "python3 ../../tools/grep_code.py --absent -q 'config_mgr_(backup_to|restore_from)_flash' include/config_mgr.h"
check "落盘路径上不再白调它"        "python3 ../../tools/grep_code.py --absent -q 'backup_to_flash' config_mgr.c"
check "config_mgr 不再依赖外部 Flash" "python3 ../../tools/grep_code.py --absent -q HAS_EXT_FLASH *.c include/*.h"

echo ""
echo "--- T4.3 firmware_channel 已摘（存了零消费者）---"
# 它 NVS 读写/导出/导入/校验全有，而消费者为零；唯一提到它的是一句"别用它"的注释。
# 云端 OTA 已上线且 manifest **没有** channels 键，补接等于改一个在跑的格式。
check "结构体字段已删"      "python3 ../../tools/grep_code.py --absent -q firmware_channel include/config_mgr.h"
check "出厂默认不再写它"    "python3 ../../tools/grep_code.py --absent -q firmware_channel config_defaults.c"
check "NVS 不再读写 fw_channel" "python3 ../../tools/grep_code.py --absent -q fw_channel config_mgr.c"
check "导出不再含该键"      "python3 ../../tools/grep_code.py --absent -q firmware_channel config_export.c"
check "setter 已删"         "python3 ../../tools/grep_code.py --absent -q config_mgr_set_firmware_channel config_mgr.c include/config_mgr.h"

echo ""; echo "--- 射频地区（docs/14 第 7.5 节，2026-09-15）---"
check "CONFIG_EVENT_RADIO_REGION_CHANGED posted" "grep -r 'CONFIG_EVENT_RADIO_REGION_CHANGED' *.c | grep -q esp_event_post"
check "sys 结构里有 radio_region"                "grep -q 'char *radio_region\[RADIO_REGION_CODE_LEN + 1\]' include/config_mgr.h"
check "NVS 键 radio_region 读与写都在"           "grep -q 'nvs_read_str(h, \"radio_region\"' config_mgr.c && grep -q 'nvs_set_str(h, \"radio_region\"' config_mgr.c"
check "NVS 读入后按菜单校验（坏值当没设过）"     "grep -A3 'nvs_read_str(h, \"radio_region\"' config_mgr.c | grep -q 'radio_region_code_valid'"
check "setter 先校验再写（NULL/长度/菜单）"      "python3 ../../tools/grep_code.py -q 'if \(!cc\) return ESP_ERR_INVALID_ARG' config_mgr.c && python3 ../../tools/grep_code.py -q 'radio_region_code_valid\(norm\)' config_mgr.c"
check "setter 存大写（先 normalize）"            "python3 ../../tools/grep_code.py -q 'radio_region_normalize\(norm\)' config_mgr.c"
check "导出带 radio_region"                      "grep -q '\"radio_region\", c->sys.radio_region' config_export.c"
check "导入带 radio_region 且按菜单清洗"         "grep -q 'IMPORT_STR(sys, \"radio_region\"' config_export.c && grep -q 'radio_region_code_valid(g_config.sys.radio_region)' config_export.c"
check "出厂默认未设置"                           "grep -q 'radio_region\[0\] = ' config_defaults.c"
check "菜单恰好四项 EU/US/CA/ZZ"                 "[ \$(grep -c '^    { \"[A-Z][A-Z]\", RADIO_REGION_CLASS_' radio_region.c) -eq 4 ] && grep -q '\"EU\", RADIO_REGION_CLASS_CE' radio_region.c && grep -q '\"US\", RADIO_REGION_CLASS_FCC' radio_region.c && grep -q '\"CA\", RADIO_REGION_CLASS_FCC' radio_region.c && grep -q '\"ZZ\", RADIO_REGION_CLASS_UNKNOWN' radio_region.c"
check "等级编号与 NCP 侧一致（NONE0 UNKNOWN1 CE2 FCC3）" "grep -q 'RADIO_REGION_CLASS_NONE *= 0' include/radio_region.h && grep -q 'RADIO_REGION_CLASS_UNKNOWN *= 1' include/radio_region.h && grep -q 'RADIO_REGION_CLASS_CE *= 2' include/radio_region.h && grep -q 'RADIO_REGION_CLASS_FCC *= 3' include/radio_region.h"
check "radio_region.c 在 CMake 源列表里"         "grep -q 'radio_region.c' CMakeLists.txt"

echo ""
echo "--- Zigbee 协调器粘性标记（2026-09-15）---"
# 区分"从没配过 Zigbee"与"以前好好的现在断了"。没有它，纯 Thread/Matter 用户会
# 一直挂着一条无法消除、也不该消除的红色横幅。
check "host_ever_seen 有字段"      "grep -q 'host_ever_seen' include/config_mgr.h"
check "落 NVS（读+写成对）"        "[ \$(grep -c '\"host_seen\"' config_mgr.c) -eq 2 ]"
check "NVS 键名不超 15 字符"       "python3 -c \"import sys; sys.exit(0 if len('host_seen')<=15 else 1)\""
check "只在有协调器接入时置位"     "python3 ../../tools/grep_code.py -q 'host_ever_seen = true' ../ezsp_bridge/ezsp_bridge.c"
check "置位前先查，只写一次"       "awk '/^static void on_client_connected/,/^}/' ../ezsp_bridge/ezsp_bridge.c | python3 ../../tools/grep_code.py -q 'cur->host_ever_seen' /dev/stdin"
check "不在 accept 的锁内写配置"   "! python3 ../../tools/grep_code.py -q 'config_mgr_set_zigbee' ../ezsp_bridge/tcp_server.c"
check "事件只注册一次"             "python3 ../../tools/grep_code.py -q 'if \\(!s_client_evt_registered\\)' ../ezsp_bridge/ezsp_bridge.c"

echo ""
echo "--- T9.33 写进配置的字符串必须是合法 UTF-8（WiFi SSID 除外，2026-10-07 定）---"
# 坏字节一旦进 NVS，/config/export 与 webhook 自动备份吐出来的就不再是合法 JSON。
# 行为判据在 test_host/run.sh（导入导出往返、整份拒收、ssid_hex、字符边界截断，各做过变异）；
# 这里钉代码形状。判据本身只有一份：components/utf8。
check "setter：MQTT 四个字符串都查"       "awk '/^esp_err_t config_mgr_set_mqtt/,/^}/' config_mgr.c | grep -c 'str_field_ok(mqtt->' | grep -qx 4"
check "setter：webhook URL 查"            "awk '/^esp_err_t config_mgr_set_webhook/,/^}/' config_mgr.c | grep -q 'str_field_ok(wh->url'"
check "setter：NTP 两个字符串都查"        "awk '/^esp_err_t config_mgr_set_ntp/,/^}/' config_mgr.c | grep -c 'str_field_ok(ntp->' | grep -qx 2"
check "setter：管理密码查"                "awk '/^esp_err_t config_mgr_set_password/,/^}/' config_mgr.c | python3 ../../tools/grep_code.py -q 'utf8_valid\\(new_password\\)' /dev/stdin"
check "setter：WiFi 密码查、SSID 不查"    "awk '/^esp_err_t config_mgr_set_wifi\\(/,/^}/' config_mgr.c | python3 ../../tools/grep_code.py -q 'utf8_valid\\(password\\)' /dev/stdin && awk '/^esp_err_t config_mgr_set_wifi\\(/,/^}/' config_mgr.c | python3 ../../tools/grep_code.py --absent -q 'utf8_valid\\(ssid\\)' /dev/stdin"
check "导入：先整份预检、SSID 豁免"       "python3 ../../tools/grep_code.py -q 'first_invalid_string\\(root, ssid_j, 0\\)' config_export.c"
check "导入：预检在拿锁写配置之前"        "awk '/first_invalid_string\\(root, ssid_j/{a=NR} /xSemaphoreTake\\(s_mutex/{if(!b)b=NR} END{exit !(a && b && a < b)}' config_export.c"
check "导入：只有 SSID 原样拷字节"        "[ \$(python3 ../../tools/grep_code.py 'IMPORT_RAW_STR\\(' config_export.c | grep -vc '#define') -eq 1 ] && python3 ../../tools/grep_code.py --strings -q 'IMPORT_RAW_STR\\(wifi, \"ssid\"' config_export.c"
check "导入：IMPORT_STR 截在字符边界"     "grep -A2 '^#define IMPORT_STR(' config_export.c | grep -q 'utf8_repair_copy'"
check "导入：ssid_hex 优先于 ssid"        "python3 ../../tools/grep_code.py -q 'if \\(have_ssid_hex\\) strlcpy\\(g_config.wifi.ssid' config_export.c"
check "导出：非 UTF-8 SSID 另存 ssid_hex" "python3 ../../tools/grep_code.py --strings -q '\"ssid_hex\", hex' config_export.c"
check "依赖 utf8 组件"                    "grep -q 'utf8' CMakeLists.txt"
check "主机侧导入导出测试全绿"            "bash test_host/run.sh"

echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
