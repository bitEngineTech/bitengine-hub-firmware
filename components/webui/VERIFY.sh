#!/bin/bash
# components/webui/VERIFY.sh — Auto-verify design doc compliance
# CREATED BEFORE ANY IMPLEMENTATION CODE (P1 requirement)
cd "$(dirname "$0")"
ROOT_CFG="$(cd "$(dirname "$0")/../config_mgr" && pwd)"
PASS=0; FAIL=0

check() {
    if eval "$2" > /dev/null 2>&1; then
        echo "✅ $1"; ((PASS++))
    else
        echo "❌ $1"; ((FAIL++))
    fi
}

echo "=== WebUI Module Verification ==="

echo ""
echo "--- Files (7 required) ---"
check "webui.h exists"          "test -f include/webui.h"
check "webui_server.c exists"   "test -f webui_server.c"
check "rest_api.c exists"       "test -f rest_api.c"
check "websocket.c exists"      "test -f websocket.c"
check "auth.c exists"           "test -f auth.c"
check "smlight_compat.c exists" "test -f smlight_compat.c"
check "static_files.c exists"   "test -f static_files.c"

echo ""
echo "--- Public API (3 functions) ---"
check "webui_init implemented"          "grep -r 'webui_init' *.c | grep -v '//' | head -1"
check "webui_stop implemented"          "grep -r 'webui_stop' *.c | grep -v '//' | head -1"
check "webui_ws_push_event implemented" "grep -r 'webui_ws_push_event' *.c | grep -v '//' | head -1"

echo ""
echo "--- Auth endpoints ---"
check "POST /api/v1/auth/login"    "grep -r 'auth/login' *.c"
check "GET /api/v1/auth/status"    "grep -r 'auth/status' *.c"
check "GET /api/v1/auth/logout"    "grep -r 'auth/logout' *.c"
check "POST /api/v1/auth/password" "grep -r 'auth/password' *.c"

echo ""
echo "--- Core REST endpoints ---"
check "GET /api/v1/status"          "grep -r '/api/v1/status' *.c"
check "GET /api/v1/wifi/scan"       "grep -r 'wifi/scan' *.c"
check "POST /api/v1/wifi/config"    "grep -r 'wifi/config' *.c"
check "GET /api/v1/ethernet/status" "grep -r 'ethernet/status' *.c"
check "POST /api/v1/ethernet/config" "grep -r 'ethernet/config' *.c"
check "GET /api/v1/zigbee/status"   "grep -r 'zigbee/status' *.c"
check "POST /api/v1/zigbee/restart" "grep -r 'zigbee/restart' *.c"
check "GET /api/v1/config/export"   "grep -r 'config/export' *.c"
check "POST /api/v1/config/import"  "grep -r 'config/import' *.c"
check "POST /api/v1/config/factory" "grep -r 'config/factory' *.c"
check "POST /api/v1/ota/esp32"     "grep -r 'ota/esp32' *.c"
check "POST /api/v1/mode"          "grep -r '/api/v1/mode' *.c"
# handle_mode 是**唯一**代表"用户选了这个模式"的地方，落盘必须在这里 ——
# 见 ezsp_bridge/VERIFY.sh 里为什么不能落在 switch_mode 那层
check "切模式要落盘"                "awk '/handle_mode\(httpd_req_t/,/^}/' rest_api.c | grep -q 'config_mgr_set_bridge_mode'"
check "落盘失败不回 500"            "awk '/handle_mode\(httpd_req_t/,/^}/' rest_api.c | grep -A2 'config_mgr_set_bridge_mode' | grep -q 'ESP_LOGW'"

echo ""
echo "--- C 桶接线：四个此前零调用点的 API（2026-08-17）---"
check "踢客户端端点"                "grep -q 'zigbee/clients/disconnect' rest_api.c"
check "统计清零端点"                "grep -q 'zigbee/stats/reset' rest_api.c"
check "SoftAP 开关端点 GET+POST"    "[ \$(grep -c 'api/v1/network/softap' rest_api.c) -ge 2 ]"
check "WiFi 静态 IP 端点"           "grep -q 'api/v1/wifi/static' rest_api.c"
# /status 的 clients 数组是压缩快照，数组下标 != tcp_server 槽位号。
# 不吐出 index，按下标寻址就会踢错人。
check "clients 暴露槽位 index"      "awk '/handle_zigbee_status/,/^}/' rest_api.c | grep -q 'clients\\[i\\].index'"
# 没命中要回 404：用户点了"踢掉"却什么也没发生，必须看得出来
check "踢不到回 404 不回 200"       "awk '/handle_clients_disconnect/,/^}/' rest_api.c | grep -q 'client_not_connected'"
# 静态 IP 只落配置不即时下发 —— 即时下发会断掉正在应答的这条连接
check "静态 IP 声明生效时机"        "awk '/handle_wifi_static/,/^}/' rest_api.c | grep -q 'takes_effect'"
# static_dns 此前两侧都没解析，而 apply_static_ip_config() 会下发它
check "以太网解析 static_dns"       "awk '/handle_eth_config/,/^}/' rest_api.c | grep -q 'static_dns'"
check "WiFi 解析 static_dns"        "awk '/handle_wifi_static/,/^}/' rest_api.c | grep -q 'static_dns'"
# SoftAP 与 Thread 互斥，回包要说清楚
check "SoftAP 回包提示 Thread"      "awk '/handle_softap_set/,/^}/' rest_api.c | grep -q 'thread_paused'"

echo ""
echo "--- SMLIGHT compat endpoints ---"
check "GET /api2/info"     "grep -r '/api2/info' *.c"
check "GET /api2/state"    "grep -r '/api2/state' *.c"
check "POST /api2/reboot"  "grep -r '/api2/reboot' *.c"
check "POST /api2/zb_restart" "grep -r 'zb_restart' *.c"

echo ""
echo "--- WebSocket ---"
check "WS endpoint /ws"               "grep -r '\"/ws\"' *.c"
check "httpd_ws_send_frame_async"      "grep -r 'httpd_ws_send_frame_async' *.c"
# WS push 是独立任务，不是 FreeRTOS Timer：Timer Service Task 里调阻塞的 WiFi API
# 会拖垮全系统定时器（CLAUDE.md S2 教训）。旧检查项写的是 xTimerCreate，已脱节。
check "WS push task (not Timer)"       "grep -r 'xTaskCreate.*ws_push' *.c"
check "WS push NOT on Timer Service"   "! grep -r 'xTimerCreate' *.c"
check "MAX_WS_CLIENTS defined"         "grep -r 'MAX_WS_CLIENTS' *.c include/*.h"

echo ""
echo "--- Auth system ---"
check "Session struct defined"         "grep -r 'session_t' *.c include/*.h"
check "esp_fill_random for token"      "grep -r 'esp_fill_random' *.c"
check "Set-Cookie header"              "grep -r 'Set-Cookie' *.c"
check "AUTH_REQUIRED macro"            "grep -r 'AUTH_REQUIRED' *.c include/*.h"
check "config_mgr_verify_password"     "grep -r 'config_mgr_verify_password' *.c"
check "config_mgr_has_password"        "grep -r 'config_mgr_has_password' *.c"

echo ""
echo "--- Static file serving ---"
check "SPIFFS mount"                   "grep -r 'spiffs' *.c"
check "Content-Encoding gzip"          "grep -r 'Content-Encoding' *.c"
# 只有客户端显式排除 gzip 时才不能发（RFC 9110 §12.5.3：没有该头 = 什么编码都收）。
# 镜像里没有 identity 副本，所以那种客户端必须拿 406，不能硬塞 gzip。
check "Accept-Encoding is honored"     "grep -q 'httpd_req_get_hdr_value_str(req, \"Accept-Encoding\"' static_files.c"
check "Refusing gzip yields 406"       "grep -q '406 Not Acceptable' static_files.c"
check "SPA fallback index.html"        "grep -r 'index.html' *.c"

# 前端版本上报：镜像里的 /www/version.json → /api/v1/status 的 www_version
check "www version reader"             "grep -r 'version.json' static_files.c"
check "webui_get_www_version declared" "grep -r 'webui_get_www_version' include/webui.h"
check "www_version in status JSON"     "grep -r 'www_version' rest_api.c"
check "www_version sanitized"          "grep -r 'json_sanitize_copy(safe_www' rest_api.c"
check "version.json parse errors handled" "grep -c 'ESP_LOGW' static_files.c | grep -qv '^0$'"

# 硅版本上报：产线批次追溯，装机后唯一可远程读到的凭据
check "chip_revision in status JSON"   "grep -r 'chip_revision' rest_api.c"
check "esp_chip_info included"         "grep -r 'esp_chip_info.h' rest_api.c"
# 静态字段不进 WS 周期推送（那里只放动态字段）
check "chip_revision not in WS push"   "! grep -r 'chip_revision' websocket.c"

echo ""
echo "--- OTA ---"
check "esp_ota_begin"                  "grep -r 'esp_ota_begin' *.c"
check "esp_ota_write"                  "grep -r 'esp_ota_write' *.c"
check "esp_ota_end"                    "grep -r 'esp_ota_end' *.c"
check "esp_ota_set_boot_partition"     "grep -r 'esp_ota_set_boot_partition' *.c"
check "3MB size limit check"           "grep -r '3.*1024.*1024\|0x300000\|3145728' *.c"

echo ""
echo "--- Events ---"
check "WEBUI_EVENT base defined"       "grep -r 'WEBUI_EVENT' *.c include/*.h"
check "OTA events posted"             "grep -r 'WEBUI_EVENT_OTA' *.c | grep esp_event_post"

echo ""
echo "--- Error handling ---"
check "401 Unauthorized response"      "grep -r '401' *.c"
check "400 Bad Request handling"       "grep -r '400\|HTTPD_400' *.c"
check "500 error handling"             "grep -r '500\|HTTPD_500' *.c"
check "501 stub for unimplemented"     "grep -r '501' *.c"

echo ""
echo "--- Integration ---"
check "Depends on config_mgr"         "grep -r 'config_mgr' *.c"
check "Depends on network_mgr"        "grep -r 'network_mgr' *.c"
check "Depends on watchdog"            "grep -r 'watchdog' *.c"
check "Depends on ezsp_bridge"        "grep -r 'ezsp_bridge' *.c"

echo ""
echo "--- 信道必须报实测值，不许拿 config 值冒充 ---"
# 病根：config 里的 zigbee.channel 从未下发给 MG24，thread.channel 只在首次
# 建网时用过一次；两者都曾被当成"实际信道"报给 WebUI / HA。
check "status 用声明值"             "grep -q 'zbc->channel_declared' rest_api.c"
check "有声明信道的端点"            "grep -q 'zigbee/channel' rest_api.c"
check "信道未知时报 null"           "grep -q 'strlcpy(zb_ch_buf, \"null\"' rest_api.c"
check "zigbee/status 带 channel_source" "grep -q 'channel_source' rest_api.c"
check "zigbee/status 保留 configured_*"  "grep -q 'configured_channel' rest_api.c"
check "diagnostics 用 otLinkGetChannel"  "grep -q 'otLinkGetChannel(inst)' otbr_api.c"
# 名字含糊的旧 API 必须彻底消失：只要它还存在就迟早有人再拿它当实际信道用。
# 它没了 = otbr_api 不可能在调它（否则链接不过）。
check "旧 thread_br_get_channel 已删除" "! grep -rqE '^(uint8_t|esp_err_t).*thread_br_get_channel *\(' ../thread_br/*.c ../thread_br/include/*.h"
check "smlight zb_channel 用声明值"  "grep -q 'channel_declared' smlight_compat.c"
check "webui 不再依赖 netparams"    "! grep -rq 'ncp_netparams' *.c"
check "推荐带 thread_channel_source" "grep -q 'thread_channel_source' rest_api.c"
check "推荐区分未跑/读不到"          "grep -q 'thread_br_get_active_channel' rest_api.c"

check "声明时实测 PAN"              "grep -q 'thread_br_probe_pan' rest_api.c"
check "填错时给出提示"              "grep -q 'thread_br_find_strongest' rest_api.c"
# 正常与"从没跑过"必须能区分：null 同时表示两者会误导。
check "检测结果带 state 字段"       "grep -q zigbee_channel_check rest_api.c && grep -q not_found rest_api.c"
check "beacon 诊断端点存在"         "grep -q 'diag/beacon' rest_api.c"

echo ""
echo "--- Zigbee/Thread 共存 ---"
check "status 输出 coex_warning"    "grep -q 'coex_warning' rest_api.c"
check "共存判定只调用不自算"        "grep -q 'thread_br_get_coex_status' rest_api.c"
check "rest 不自己写阈值比较"       "! grep -q 'COEX_TOO_CLOSE_MAX)' rest_api.c || grep -q 'COEX_TOO_CLOSE_MAX,' rest_api.c"
check "推数据集只告警不拒绝"        "grep -A6 'thread_br_channel_too_close' otbr_api.c | grep -q 'ESP_LOGW'"
check "推数据集不返回 4xx"          "! grep -B4 'thread_br_channel_too_close' otbr_api.c | grep -q 'send_error'"
# 建网前的信道建议：可用列表必须由固件算好下发。前端自己写距离判断 = 第二份
# 规则，阈值改了不会同步，会把用户引到冲突信道上。
check "可用信道由后端算"            "grep -q 'thread_br_list_safe_zigbee_channels' rest_api.c"
check "下发 channel_advice"         "grep -q 'channel_advice' rest_api.c"
check "拼不下就不发半截"            "awk '/n_ok > 0/,/^    }$/' rest_api.c | grep -q 'strlcpy(advice_buf, \"null\"'"
check "前端不自己算距离"            "! grep -q 'Math.abs(c - th)' ../../frontend/js/pages/zigbee.js"
check "前端只读后端给的列表"        "grep -q 'a.ok.join' ../../frontend/js/pages/zigbee.js"
# JSON 路径会 otDatasetCreateNewNetwork()（换网络，甩掉所有设备），不是迁移。
# 注释必须留着，否则下一个人很容易照着 JSON 分支去实现"改信道"。
check "JSON 路径标注了不可用于迁移"  "awk '/body\[0\] == .{./,/otDatasetCreateNewNetwork/' otbr_api.c | grep -q '不能用来做信道迁移'"

echo ""
echo "--- LED / MQTT / Webhook 配置端点（原 501，2026-08-12 接线）---"
# 五个端点必须真正接上 handler，不能退回 handle_not_implemented
for ep in "led/config" "led/nightmode" "mqtt/status" "mqtt/config" "webhook/config"; do
    check "$ep 不再是 501 桩" \
        "! grep -q '\"/api/v1/$ep\".*handle_not_implemented' rest_api.c"
done
check "led/config → handle_led_config"      "grep -q '\"/api/v1/led/config\".*handle_led_config' rest_api.c"
check "led/nightmode → handle_led_nightmode" "grep -q '\"/api/v1/led/nightmode\".*handle_led_nightmode' rest_api.c"
check "mqtt/status → handle_mqtt_status"    "grep -q '\"/api/v1/mqtt/status\".*handle_mqtt_status' rest_api.c"
check "mqtt/config → handle_mqtt_config"    "grep -q '\"/api/v1/mqtt/config\".*handle_mqtt_config' rest_api.c"
check "webhook/config → handle_webhook_config" "grep -q '\"/api/v1/webhook/config\".*handle_webhook_config' rest_api.c"

# ---- S1 安全 ----
# MQTT 密码绝不能回显：状态端点只给 password_set 布尔
check "mqtt 状态不回显密码"        "! grep -A14 'send_mqtt_status_json' rest_api.c | grep -q '\\\\\"password\\\\\":'"
check "mqtt 状态给 password_set"   "grep -q 'password_set' rest_api.c"
# webhook URL 是网关会主动去请求的地址，必须限定 http(s)
check "webhook URL 限定 scheme"    "grep -q 'invalid_url_scheme' rest_api.c"
# 从 NVS 读出来的字符串进 JSON 前一律转义（broker/topic/url 都来自用户输入）
check "mqtt 字段转义后再拼 JSON"   "grep -B6 'password_set' rest_api.c | grep -q 'json_sanitize_copy'"
# 超长输入必须拒绝而不是静默截断（截断后的 broker 指向别处）
check "超长字符串拒绝不截断"       "grep -A6 'json_opt_str' rest_api.c | grep -q 'strlen.*>= dst_size'"

# ---- S2 线程上下文 ----
# httpd task 里禁止直接 restart MQTT：stop/destroy 会阻塞，且 07 模块因此漏过 task
# 匹配的是**调用**（带分号），不是名字本身 —— 解释"为什么不调"的注释必须留得住，
# 否则下一个人删掉注释重新犯一遍，而 VERIFY 还是绿的。
check "不在 httpd 里 restart mqtt" "! grep -Eq 'mqtt_client_restart\(\) *;' rest_api.c"

# ---- 语义 ----
# 夜间熄灯的不变量已全部移到 components/led_ctrl/VERIFY.sh 的「H1 夜间模式必须与
# s_state 正交」一节（2026-08-16）。原先这里有两条断言去 grep ../led_ctrl/led_ctrl.c，
# 结果 2026-08-15 把 NIGHT 从 s_state 里拆出来时，改的人只看得到 led_ctrl 自己的
# VERIFY.sh —— 长在别人家的断言没人看见，于是它一直红着，还和 led_ctrl/VERIFY.sh:128
# 形成了一对**永远不可能同时绿**的矛盾断言（一个要求 s_state == LED_STATE_NIGHT 存在，
# 一个要求它不存在）。教训：断言要跟被断言的源文件待在同一个模块。
check "scheduler 暴露 night_active"  "grep -q 'scheduler_night_active' ../scheduler/include/scheduler.h"
# color_* 至今无人读取，接进来就是假功能 —— 必须留证据说明为什么不接
check "说明 color_* 为何不接"       "grep -q 'color_\\*' rest_api.c"

echo ""
echo "--- NTP / 白名单端点（2026-08-13 补齐，原先只能整包 import）---"
check "GET /ntp 已注册"            "grep -q '\"/api/v1/ntp\".*HTTP_GET.*handle_ntp_get' rest_api.c"
check "POST /ntp 已注册"           "grep -q '\"/api/v1/ntp\".*HTTP_POST.*handle_ntp_set' rest_api.c"
check "GET /whitelist 已注册"      "grep -q '\"/api/v1/whitelist\".*HTTP_GET' rest_api.c"
check "POST /whitelist 已注册"     "grep -q '\"/api/v1/whitelist\".*HTTP_POST' rest_api.c"
# NTP 改了必须真的下发，否则就是个存了不生效的假端点（时区尤其：夜间模式靠它）
check "NTP_CHANGED 有消费者"       "grep -q 'CONFIG_EVENT_NTP_CHANGED' ../../main/main.c"
check "时区真的 setenv+tzset"      "grep -q 'setenv(\"TZ\"' ../../main/main.c && grep -q 'tzset' ../../main/main.c"
check "改配置后重启 SNTP"          "grep -q 'esp_sntp_restart' ../../main/main.c"
# 板子的本地时间必须回给用户：时区默认 UTC+0，不给这个字段没人能确认设对没有
check "/ntp 回包给 local_time"     "grep -q 'local_time' rest_api.c"
check "/ntp 回包给 synced"         "grep -q 'synced' rest_api.c"
# enabled 但表为空时 tcp_server 不过滤，必须如实上报，不能让用户以为拦住了
check "白名单区分 enabled/active"  "grep -q '\\\\\"active\\\\\"' rest_api.c"
check "白名单上限用 MAX_WHITELIST_IPS" "grep -q 'MAX_WHITELIST_IPS' rest_api.c"

echo ""
echo "--- LED 测试面板（index→灯名 单一出处）---"
check "GET /led/test 已注册"        "grep -q '\"/api/v1/led/test\".*HTTP_GET.*handle_led_test_info' rest_api.c"
check "灯名来自 led_ctrl"           "grep -q 'led_ctrl_name' rest_api.c"
check "灯数来自 led_ctrl"           "grep -q 'led_ctrl_count' rest_api.c"
# 映射只有一份：rest 层不得自己写灯名字符串
check "rest 不自写灯名表"           "! grep -Eq '\"(sys|zigbee|thread|net)\"' rest_api.c"
# 前端同理：不得写死 index→灯名
check "前端不写死灯位表"            "! grep -Eq \"\\['sys'|\\\"sys\\\",\" ../../frontend/js/pages/settings.js"
check "前端用后端下发的 leds"       "grep -q 'info.leds.map' ../../frontend/js/pages/settings.js"
# 固件侧名字表长度必须与灯数一致，编译期就要炸
check "灯名表长度编译期断言"        "grep -q '_Static_assert' ../led_ctrl/led_ctrl.c"
check "没有 WS2812B 时面板禁用"     "grep -q 'supported' ../../frontend/js/pages/settings.js"

echo ""
echo "==============================="
echo ""
echo "--- 栈只能按实测裁（2026-08-17 同日踩了两次）---"
# 坑一：静态估算低估 —— ws_push 按 objdump 数栈帧估成 704，裁到 1536 后
#        上板 stack_left 只剩 16 字节。
# 坑二：**空载实测同样低估** —— httpd 刚启动时 stack_left=7388（用 804），
#        跑完 tools/test_api.sh 变成 800（用 4320）。高水位只反映跑过的路径。
check "httpd 栈保持 8192"        "grep -q 'config.stack_size        = 8192;' webui_server.c"
check "ws_push 栈保持 3072"      "grep -q 'ws_push\", 3072' websocket.c"
check "两处都指向实测入口"        "grep -q 'diag/watchdog' webui_server.c && grep -q 'diag/watchdog' websocket.c"
# 逐任务栈余量必须对外可见，否则裁栈只能靠猜（这正是踩坑的根因）
check "diag 暴露逐任务 stack_left" "grep -q 'stack_left.*:%u' rest_api.c"

# ---- 固件签名（2026-08-19）----
# OTA 信任锚是运行中 app 自己的签名块，不是 eFuse。刷进未签名固件的整机会永久
# 拒绝一切 OTA 且外部零症状，产线是唯一能拦住它的地方。
check "status 暴露 app_signed"      "grep -q '\"app_signed\\\\\":%s' rest_api.c"
check "自检有 app_signed 项"        "grep -q '\"app_signed\"' factory_api.c"
# 必须是 fail 不是 manual：manual 靠操作员回写 ack 消掉，而这条不该能被人工放行
check "app_signed 是硬 fail 不是 manual" \
      "grep -A2 '\"app_signed\", signed_ok' factory_api.c | grep -q '\"fail\"' && ! grep -A2 '\"app_signed\", signed_ok' factory_api.c | grep -q 'manual'"

# ---- T2.2 产线自检补项（2026-09-11）----
# 三项各自在拦什么：
#   flash_rw   —— `flash` 只读 SPI Flash 的 ID/容量，虚焊到写不进去的片子照样报 8MB。
#                 真故障是"配置存不住"：用户设完密码重启回出厂态，而自检全绿。
#   wifi_scan  —— `wifi` 只看"连上了没"，产线通常没有 5GHz AP，于是它恒 manual，
#                 即此前**没有任何一项**证明过 WiFi 射频是好的。
#   poe        —— PoE 供电却读不到 = 电源链路有问题，此前产线完全看不见。
check "自检有 flash_rw 项"          "grep -q '\"flash_rw\"' factory_api.c"
check "自检有 wifi_scan 项"         "grep -q '\"wifi_scan\"' factory_api.c"
check "自检有 poe 项"               "grep -q '\"poe\"' factory_api.c"
# flash_rw 必须是完整的写-读-擦回合：只写不读证明不了什么
check "flash_rw 写读擦俱全"         "python3 ../../tools/grep_code.py -q 'nvs_set_u64' factory_api.c && python3 ../../tools/grep_code.py -q 'nvs_get_u64' factory_api.c && python3 ../../tools/grep_code.py -q 'nvs_erase_key' factory_api.c"
# 写死常量的话，上一次残留的键会让"根本没写进去"也读成功 —— 自检退化成恒真
check "flash_rw 的写入值每次不同"   "awk '/^static bool flash_rw_check/,/^}/' factory_api.c | grep -q 'esp_timer_get_time'"
# 可能落到 manual 的项必须在 ACKABLE 白名单里，否则 verdict 永久卡 incomplete
check "wifi_scan 可被 ack"          "awk '/ACKABLE\[\] = \{/,/\};/' factory_api.c | grep -q 'wifi_scan'"
check "poe 可被 ack"                "awk '/ACKABLE\[\] = \{/,/\};/' factory_api.c | grep -q '\"poe\"'"
check "ACK_MAX 装得下白名单"        "grep -q '_Static_assert' factory_api.c"
# flash_rw 是自动判定，不该能被人工放行（同 app_signed / identity 的道理）
check "flash_rw 不可被 ack"         "! awk '/ACKABLE\[\] = \{/,/\};/' factory_api.c | grep -q 'flash_rw'"
# ncp_tables（2026-10-07，T9.12②）：工位 3 认"烧的是不是当前版 NCP"的唯一机器门。三条别放宽：
#   不进 ack 白名单（manual 能被人工确认消掉，门就没了）；没读到判 fail；比较对象是常量（值由 tools/check_capacity_docs.py 对 slcp 核）
check "自检有 ncp_tables 项，比的是直连子设备与产测常量" "python3 ../../tools/grep_code.py -q 'max_end_device_children == FACTORY_NCP_END_DEVICE_CHILDREN' factory_api.c"
check "ncp_tables 没读到表容量判 fail（不是 manual）"   "python3 ../../tools/grep_code.py -q 'cfg\\.valid' factory_api.c && grep -A1 '!nv->cfg.valid' factory_api.c | grep -q 'item(rp, \"ncp_tables\", \"fail\"'"
check "ncp_tables 不可被 ack"                          "! awk '/ACKABLE\[\] = \{/,/\};/' factory_api.c | grep -q 'ncp_tables'"
# 项数 / 只开 selftest 命名空间 / 绝不碰 fctry —— 三条都在这个脚本里，
# ⚠️ 不用裸 grep：判"代码里不能有 fctry"会被解释"为什么不碰 fctry"的注释绊倒（7.4），
#    而且要保留字符串字面量（nvs_open("fctry") 的证据就在字面量里）。
check "自检项数与命名空间不变量"    "python3 ./check_selftest_items.py"

# ---- SMLIGHT 兼容层不许写死（2026-08-20）----
# 三处写死值曾让 HA 里的实体长期显示假信息：coord_mode=0 让 connection_mode
# 永远显示 eth（实际走 WiFi）、fw_channel 写死 release（本地 dirty 构建也自称
# 发布版）、device_ip 只看 WiFi（走以太网时恒报 0.0.0.0）。
# ⚠️ 判据用 grep -F 定长字符串。第一版用带转义的正则写，结果**变异测试里没红** ——
# 反斜杠经 python→shell→grep 三层转义后已经匹配不到任何东西，恒绿。
check "coord_mode 不是写死 0"      "! grep -qF 'coord_mode\\\":0,' smlight_compat.c"
check "coord_mode 按实际接口算"     "grep -q 'coord_mode = 2' smlight_compat.c && grep -q 'NET_IF_ETH' smlight_compat.c"
check "fw_channel 不是写死 release" "! grep -qF 'fw_channel\\\":\\\"release\\\",' smlight_compat.c"
check "fw_channel 由构建实际推导"   "grep -qF 'strstr(app->version, \"-dirty\")' smlight_compat.c"
check "device_ip 考虑以太网"        "grep -q 'eth_has_ip' smlight_compat.c"
check "legacy_api 必须是 0"        "grep -qF 'legacy_api\\\":0,' smlight_compat.c"
# 报 2 = HA 完全不读 /ha_sensors（coordinator.py:167）+ 挂 error 级修复提示
check "legacy_api 不是 2"          "! grep -qF 'legacy_api\\\":2,' smlight_compat.c"
check "ha_sensors 报开关状态"      "grep -qF 'disable_leds' smlight_compat.c && grep -qF 'night_mode' smlight_compat.c"
# 开关必须读写成对：只报状态会让 HA 里的开关看着能拨、拨完弹错再跳回去
check "开关可写(saveParams)"       "grep -q 'settings/saveParams' smlight_compat.c"
check "saveParams 认 LED 页"       "grep -q 'nightMode' smlight_compat.c && grep -q 'disableLeds' smlight_compat.c"
# 未实现的 pageId 不能回 404 —— pysmlight 会抛 SmlightConnectionError 打掉整个集成
check "未知 pageId 不回 404"       "grep -q 'page_not_implemented' smlight_compat.c"
# zb_temp 拿不到就别编：MG24 温度要成为完整 EZSP 主机才读得到，违反决策 1
# 只看 JSON 键，不看注释 —— 注释里正解释着"为什么不报它"
check "不伪造 zb_temp"             "! grep -qF 'zb_temp\\\":' smlight_compat.c"

# ---- 以太网 MAC 必须问网卡要，不能自己推（2026-08-22）----
# 原来用 esp_read_mac(emac, ESP_MAC_ETH)，报出来的值和线上真实的差一个 bit：
#     .43 线上真实(ARP)   3a:44:be:12:34:57
#     我们上报            38:44:BE:12:34:57   ← 差本地管理位 0x02
# DM9051 是 SPI 模块、自身无 MAC，ethernet_init.c 先 esp_read_mac(ESP_MAC_ETH)
# 再 esp_derive_local_mac() 派生（置 UL 位），最后写进网卡的是**派生后**那个。
# 用户拿我们报的值去路由器做 DHCP 保留 → 永远匹配不上 → 静默失效，
# 而"让地址别变"正是我们让他做保留要解决的问题。
check "以太网 MAC 从网卡实读"    "grep -q 'esp_netif_get_mac(eth_nif_mac' rest_api.c"
# ⚠️ 锚点用「以分号结尾的调用」而不是 ^[^*/]* 前缀：本仓库块注释的续行是纯缩进、
#    没有 ` * ` 前缀，所以前缀法排不掉注释里的提及 —— 上面那段订正说明本身就会
#    把判据打红（2026-08-22 实测，正确代码上假红）。代码有 `;`，中文说明没有。
check "不再用 esp_read_mac 推 ETH" "! grep -qE 'esp_read_mac\\([^)]*ESP_MAC_ETH\\);' rest_api.c"

# ---- /api/v1/logs?level=W/E 必须走告警环（2026-08-22）----
# 「取回主环再按 level 过滤」是无效的：主环只留 111 秒，取回的 100 条里本来就
# 没有告警。这条判据钉住的是"别改回主环过滤"。
check "level=W/E 走告警环"    "grep -q 'log_store_fetch_warn' rest_api.c"
check "回包标注数据源"        "grep -q 'warn_ring' rest_api.c && grep -q 'main_ring' rest_api.c"

# ============================================================
# 跨站守卫（2026-08-23）
# ⚠️ 全部判据锚在**函数体范围**（awk）而不是全文 grep / 行距（grep -A2）。
#    2026-08-23 的对抗复核实测过两种最危险的假绿：
#      · `grep -A2 'Origin", origin' | grep 'return 0'` 在把"缺失即放行"改成
#        "必须有 Origin"之后**仍然绿** —— -A2 的窗口滑进了下一句的 return 0
#      · `grep -q host_is_ip_literal` 在"删掉调用点、只留函数定义"时也绿
#    这两种正是最可能发生的改法。
# ------------------------------------------------------------
echo ""
echo "--- 跨站守卫：守卫函数本体 ---"

GUARD_BODY='awk "/^int webui_cross_origin_check/,/^\}/" cross_origin.c'

# 失效方向：只有 ESP_ERR_NOT_FOUND 才等于「头缺失」。TRUNC 也放行 = 攻击者换一个
# 长域名，整层守卫静默失效（httpd_parse.c:1126-1129 返回 TRUNC 而非 NOT_FOUND），
# 而日志不打、行为测试也全绿（测试用的都是短域名）。这条是整层防护的成败所在。
check "只把 NOT_FOUND 当作头缺失" \
      "[ \$($GUARD_BODY | grep -c 'ESP_ERR_NOT_FOUND') -eq 3 ]"
check "非 NOT_FOUND 的读失败一律拒" \
      "! $GUARD_BODY | grep -q 'e != ESP_OK) return 0'"

# 放行路径恰好 3 条：Host 缺失、Origin 缺失、Origin 命中。
# 多一条 = 有人偷偷加了个"某某情况也放行"；少一条 = 有人把"缺失即放行"改成了拒
# （那会打死全部 curl 脚本和 HA 集成）。双向都能变红。
check "放行路径恰好 3 条 return 0" \
      "[ \$($GUARD_BODY | grep -c 'return 0;') -eq 3 ]"

# 整标签匹配的边界。放宽成前缀匹配会让 bitengine.attacker.com 通过 ⇒ 防御归零。
NAME_BODY='awk "/^static bool host_name_ok/,/^\}/" cross_origin.c'
check "主机名匹配查边界字符" \
      "$NAME_BODY | grep -q \"h\\[n\\] != '\\.'\""
# ⚠️ 否定式判据（! grep）必须先断言目标存在。2026-08-23 把守卫拆到 cross_origin.c
#    之后，这条还指着 webui_server.c —— awk 什么都没找到、grep 自然也找不到、
#    `!` 一取反就**恒真**。文件改名/函数搬家是最常见的触发方式。
# ⚠️ 用函数不用变量：变量形态要经过 check 里的 eval 再展开一次，双重 eval 会把
#    awk 正则里的引号和反斜杠吃掉，表现是"命令直接跑没问题、放进 check 就空"。
suffix_body() { awk '/^const char \*const webui_host_suffixes/,/^};/' cross_origin.c; }
check "后缀表本身找得到"       "[ $(suffix_body | grep -c '\"') -ge 1 ]"
check "后缀表不含公网 TLD"     "! suffix_body | grep -qE '\"(com|net|org|io|cn|dev)\"'"
# .lan 是我们自己的固件让路由器建的（wifi_mgr.c/eth_mgr.c 设 DHCP option 12），
# 也是用户最常用的入口。删掉它 = 把用户锁在自己造的门外。
check "后缀表含 local 与 lan"  "suffix_body | grep -q '\"local\"' && suffix_body | grep -q '\"lan\"'"

# 421 的出路不能靠阻塞 API 拿 IP（network_mgr.h 明确警告过三个阻塞 ioctl），
# 且本函数无需凭据即可触发，挂在单任务 httpd 上等于白送一条阻塞放大器。
host_err_body() { awk '/^esp_err_t send_error_host/,/^}/' webui_server.c; }
check "send_error_host 找得到"  "[ $(host_err_body | wc -l) -ge 10 ]"
# 2026-08-23 加 OTBR token 时把取地址的逻辑抽成了 webui_local_addr（两处在用），
# 所以判据从 send_error_host 挪到这里 —— 不变量没变：**不能用阻塞 API 拿 IP**。
# 调用它的两处都挂在单任务 httpd 上，其中一处还无需凭据即可触发。
local_addr_body() { awk '/^void webui_local_addr/,/^}/' webui_server.c; }
check "取本机地址用 getsockname 而非 network_mgr" \
      "local_addr_body | grep -q 'getsockname' && ! local_addr_body | grep -q 'network_mgr_get_status'"
check "421 走 webui_local_addr" "host_err_body | grep -q 'webui_local_addr('"
# T9.22（2026-10-08）：OTBR 的可粘贴 URL 优先给设备地址（svc_ip），访问地址只是兜底 —— HA 的 OTBR 条目改不了，
# 经 WiFi 自己的地址打开网页时给出那个地址，拔插网线后它就不在网关上了
otbr_host_body() { awk '/^static void otbr_url_host\(/,/^}/' rest_api.c; }
check "OTBR 可粘贴 URL 先取设备地址、再退回访问地址" \
      "otbr_host_body | grep -q 'network_mgr_svc_ip()' && otbr_host_body | grep -q 'webui_local_addr(req' && [ \$(awk '/^static esp_err_t handle_otbr_token_(get|set)\(/,/^}/' rest_api.c | python3 ../../tools/grep_code.py 'otbr_url_host\(req' /dev/stdin | wc -l) -eq 2 ] && awk '/^static esp_err_t handle_otbr_token_(get|set)\(/,/^}/' rest_api.c | python3 ../../tools/grep_code.py --absent -q 'webui_local_addr\(' /dev/stdin"
# 双栈监听时普通 IPv4 连接也是 AF_INET6（v4-mapped），不还原就会给出
# "[::FFFF:192.168.1.44]" 这种没人认得的地址，而它会被粘进用户的 HA 配置。
# 2026-08-23 生成第一个 OTBR token 时实测撞到 —— 只有上板才会暴露。
check "v4-mapped 地址被还原成点分十进制" \
      "local_addr_body | grep -q '0xFF' && local_addr_body | grep -q '%u.%u.%u.%u'"
check "421 回包带出路 use 字段" "host_err_body | grep -q 'use'"
check "send_error 认识 403/421"  "grep -q 'case 403:' webui_server.c && grep -q 'case 421:' webui_server.c"

echo ""
echo "--- 跨站守卫：挂载点（逐 handler，不是数数）---"
# 逐 handler 而不是数总数。34==34 的算术在「把 34 个宏全塞进一个 GET handler」时
# 照样绿（对抗复核实测），而现实中的失效形态正是"新加 POST 时宏加错了 handler"。
MISSING=$(python3 - <<'PY'
import re
miss=[]
s=open('rest_api.c').read()
ent=re.findall(r'URI_ENTRY\(\s*"([^"]+)"\s*,\s*HTTP_(\w+)\s*,\s*(\w+)\s*\)',s)
want={f for u,m,f in ent if m in ('POST','PUT','DELETE')}
# 会动硬件 / 吐敏感数据的 GET：<img src> 跨站 GET 不带 Origin，只有 Sec-Fetch-Site 挡得住
want|={'handle_spectrum_scan','handle_spectrum_beacon','handle_spectrum_recommend',
       'handle_wifi_scan','handle_ota_check','handle_config_export'}
for f in ['rest_api.c','otbr_api.c','smlight_compat.c','factory_api.c','auth.c','muse_api.c','hub_api.c']:
    t=open(f).read()
    if f=='muse_api.c':   # 直连型 + Muse 的端点：全部要守卫（精简 /status 与进度页同正常的 /status，故意不挂）
        want|={h for h in re.findall(r'\.handler\s*=\s*(handle_\w+)',t) if h not in ('handle_pair_status','handle_pair_page')}
    if f in ('otbr_api.c','hub_api.c'):   # hub_api.c：直连型本地出口（/hub/invoke、/hub/devices、/muse/audit），全部要守卫
        want|=set(re.findall(r'\.handler\s*=\s*(handle_\w+)',t))
    if f=='smlight_compat.c':
        want|={'handle_settings_save','handle_api2_reboot','handle_api2_zb_restart'}
    if f=='factory_api.c':
        want|={'handle_factory_selftest','handle_factory_ack'}
    if f=='auth.c':
        want|={'handle_auth_login','handle_auth_password'}
for f in ['rest_api.c','otbr_api.c','smlight_compat.c','factory_api.c','auth.c','muse_api.c','hub_api.c']:
    t=open(f).read()
    for n in sorted(want):
        m=re.search(r'(?:static\s+)?esp_err_t\s+'+n+r'\s*\(\s*httpd_req_t\s*\*\s*req\s*\)\s*\n\{\n((?:.*\n){0,6})',t)
        if not m: continue
        head=m.group(1)
        if 'CROSS_ORIGIN_GUARD(req)' not in head:
            miss.append(f'{f}:{n} 无守卫'); continue
        # 顺序：守卫必须在 AUTH 之前，否则 check_basic_auth 构成密码预言机
        lines=head.split('\n')
        gi=next((i for i,l in enumerate(lines) if 'CROSS_ORIGIN_GUARD' in l), 99)
        ai=next((i for i,l in enumerate(lines) if re.search(r'(OTBR_)?AUTH_REQUIRED', l)), 99)
        if ai<gi: miss.append(f'{f}:{n} 守卫在 AUTH 之后（密码预言机）')
print('\n'.join(miss))
PY
)
check "每个写 handler 都挂了守卫且在 AUTH 之前" "[ -z \"$MISSING\" ]"
# 配对进度页（策略 C 的配对剖面）上的两句硬事实（设计第十三节"配对"行的残余风险、button.c 的按键时长）：直连型 Muse 页的确认框里也有，
# 由 frontend/direct/test_muse.sh 钉；这里钉进度页（7.6 规矩 3）
check "配对进度页写着'只在你自己的 App 提示时按键'和按短 / 按长的后果" "grep -q 'Only press the button when your own Muse app is asking for it' muse_api.c && grep -q 'shorter than 2 seconds does \"' muse_api.c && grep -q 'nothing while pairing (use Cancel pairing below to stop)' muse_api.c && ! grep -q 'Shorter than 2 seconds restarts' muse_api.c && grep -q 'resets it to factory settings' muse_api.c"
[ -n "$MISSING" ] && echo "$MISSING" | sed 's/^/     /'

# WS 不能用那个宏 —— 宏 return ESP_OK，而在 post-handshake 回调里 ESP_OK 的意思
# 正是"接受这个连接"，套上去等于把拒绝写成了放行。
# ⚠️ 必须剥掉注释行再匹配。第一版没剥，结果两条判据命中的是**我自己写的注释**
#    （注释里恰好解释了"不能用 CROSS_ORIGIN_GUARD"和"必须在 check_auth 之前"），
#    于是正确的代码被判红。这个项目在同一个坑上栽过不止一次：判据只能看代码。
# ⚠️ 判据锚在**语句形态**上（带括号和分号），不是裸的标识符。
#    本仓库的注释续行是纯缩进的（没有前导 *），所以 grep -v '^[[:space:]]*[/*]'
#    剥不掉它们 —— 第一版就因此把"注释里提到 CROSS_ORIGIN_GUARD"当成了"代码里
#    用了这个宏"，把正确代码判红。而 `CROSS_ORIGIN_GUARD(req);` 这种形态
#    不会出现在散文里。
WSB='awk "/^static esp_err_t ws_client_registered/,/^\}/" websocket.c'
check "WS 用函数不用宏"        "$WSB | grep -q 'webui_cross_origin_check(req)' && ! $WSB | grep -q 'CROSS_ORIGIN_GUARD(req);'"
check "WS 拒绝时 return ESP_FAIL" "$WSB | grep -A2 'webui_cross_origin_check' | grep -q 'ESP_FAIL'"
check "WS 守卫在 check_auth 之前" \
      "[ \$($WSB | grep -n 'webui_cross_origin_check(req)' | cut -d: -f1 | head -1) -lt \$($WSB | grep -n '!check_auth(req)' | cut -d: -f1 | head -1) ]"

# 决策 1.5：/api/actions 必须保持 404，否则 python-otbr-api 抛 OTBRError →
# ConfigEntryNotReady → HA 的 otbr 集成加载不起来。守卫绝不能做成全局过滤器。
check "守卫不是全局过滤器"     "! grep -q 'httpd_register_err_handler.*cross_origin' webui_server.c"
check "静态兜底未被挂守卫"     "! awk '/^static esp_err_t handle_static/,/^\}/' static_files.c | grep -q 'CROSS_ORIGIN_GUARD'"

# 这条不是"代码风格"，是一整类 CSRF 的唯一防线：设备从不发 WWW-Authenticate
# ⇒ 浏览器永远不会为本设备建立 Basic 凭据缓存 ⇒ Basic 不构成跨站自动附带路径。
# 谁为了"标准合规"给 401 补上挑战头，防护会**静默**塌掉。
# ⚠️ 旧写法 grep 'httpd_resp_set_hdr' | grep 'WWW-Authenticate' 对折行调用失明
#    （实测 GREEN），所以直接查字符串出现，只排掉注释行。
check "从不发 WWW-Authenticate" \
      "! grep -n 'WWW-Authenticate' *.c | grep -v ':[[:space:]]*[/*]' | grep -q 'httpd_resp\|set_hdr'"

# ============================================================
# 登录节流 / 会话生命周期（2026-08-23）
# ------------------------------------------------------------
echo ""
echo "--- 登录节流 ---"

login_body() { awk '/^static esp_err_t handle_auth_login/,/^}/' auth.c; }
basic_body() { awk '/^static bool check_basic_auth/,/^}/' auth.c; }

# ★ 这是整层节流唯一有意义的不变量：**先判锁，再比密码**。
#   反过来写（先比密码、对了就放行）的话，攻击者一次都不会慢下来，429 纯属装饰，
#   而且**用错密码测两种实现返回值完全相同** —— grep 是这里唯一便宜的判据，
#   行为判据见 tools/test_auth_throttle.sh 第 3 条（那条要板子）。
check "登录：先判锁再比密码" \
      "[ $(login_body | grep -n 'auth_throttled()' | cut -d: -f1 | head -1) -lt $(login_body | grep -n 'config_mgr_verify_password' | cut -d: -f1 | head -1) ]"
check "Basic：先判锁再比密码" \
      "[ $(basic_body | grep -n 'auth_throttled()' | cut -d: -f1 | head -1) -lt $(basic_body | grep -n 'config_mgr_verify_password' | cut -d: -f1 | head -1) ]"
# 两条路都必须记账，否则从没记账的那条就能无限爆破
check "登录路径记账"          "login_body | grep -q 'auth_note('"
check "Basic 路径记账"        "basic_body | grep -q 'auth_note('"
# httpd 是单任务，睡在认证里 = 全端点无响应。节流只能记时刻、立即返回。
check "节流不用 vTaskDelay"   "! awk '/^static void auth_note/,/^}/' auth.c | grep -q 'vTaskDelay' && ! awk '/^static bool auth_throttled/,/^}/' auth.c | grep -q 'vTaskDelay'"
check "退避有上限"            "awk '/^static void auth_note/,/^}/' auth.c | grep -q 'shift > 4'"

echo ""
echo "--- 会话生命周期 ---"
# 攻击者趁无密码窗口白拿的 token，原来会在用户设完密码后继续有效 24 小时
pwd_body() { awk '/^static esp_err_t handle_auth_password/,/^}/' auth.c; }
# ⚠️ 必须带括号锚定**调用形态**：调用点上方的注释里也写着这个函数名，
#    grep 裸标识符会命中注释 ⇒ 把调用删掉照样绿（2026-08-23 变异测试实测）。
check "设密码后作废旧会话" \
      "[ $(pwd_body | grep -n 'config_mgr_set_password(' | cut -d: -f1 | head -1) -lt $(pwd_body | grep -n 'invalidate_sessions_except(' | cut -d: -f1 | head -1) ]"
check "保留发起本次修改的会话" \
      "awk '/^static void invalidate_sessions_except/,/^}/' auth.c | grep -q 'token_equal'"
check "设密码后清失败计数"    "pwd_body | grep -q 'auth_note(true)'"
# 那段死代码（!has_password 之后又判一次 !has_password）不能回来。
# ⚠️ 判据不能 grep 'NET_STATE_SOFTAP_ONLY' —— check_auth 的注释里**引用了**被删的
#    那段代码，一 grep 就命中注释，正确代码反而判红。本仓库注释续行是纯缩进的，
#    剥不掉。（2026-08-23 同一天第三次踩这个坑，前两次在跨站守卫那批判据里。）
#    改用**编译器强制**的代理不变量：唯一的使用者删掉后 include 也删了，谁想把
#    死分支加回来就必须先把 include 加回来，否则编不过 —— 而 include 行的形态
#    不会出现在散文注释里。
check "auth 不依赖网络状态"   "! grep -q '^#include \"network_mgr.h\"' auth.c"


echo ""
echo "--- OTBR 专用 token 的作用域隔离 ---"
# 这一组锁的是这个特性存在的**全部理由**：泄了只是 Thread 数据集，不是整机。
# 行为断言在 tools/test_api.sh 的 TC-T 组（T03 那三条），这里锁代码形状。
basic_body() { awk '/^webui_basic_result_t webui_basic_check/,/^}/' auth.c; }
chkauth_body() { awk '/^bool check_auth/,/^}/' auth.c; }

check "webui_basic_check 找得到"  "[ $(basic_body | wc -l) -ge 15 ]"
# ★ check_auth 绝不能认识 token —— 认识了，token 就等价于管理密码，特性归零
check "check_auth 不认识 token"   "! chkauth_body | grep -q 'otbr_token'"
check "token 只在 otbr 闸门里比对" "[ $(grep -c 'otbr_token' otbr_api.c) -ge 1 ] && ! grep -q 'otbr_token' static_files.c websocket.c smlight_compat.c factory_api.c"
# ★ 不碰节流：HA 拿着过期 token 反复轮询会把管理员锁死，节奏由第三方决定
check "token 比对不碰登录节流"    "! basic_body | grep -q 'auth_note('"
check "token 比对是常量时间"      "basic_body | grep -q 'volatile' && basic_body | grep -q 'diff |='"
# 长度不等直接判负，避免拿长度做侧信道
check "长度不等直接判负"          "basic_body | grep -q 'strlen(got) != want_len'"

echo ""
echo "--- OTBR token 的存储与导出 ---"
cd "$ROOT_CFG" 2>/dev/null || cd ../config_mgr
# token 是凭据 —— 必须脱敏。与 extra_host 相反（那个是权限授予，必须看得见）
check "导出时 token 脱敏"        "grep -q 'ADD_SECRET(thr, \"otbr_token\"' config_export.c"
check "导入时跳过占位符"          "grep -q 'IMPORT_SECRET_STR(thr, \"otbr_token\"' config_export.c"
# 导入直接写结构体，绕过 setter 的校验 —— 畸形值的后果是"永远匹配不上"，
# 而 UI 上 token 明明显示着，无从排查。所以导入路径必须自己再校验一次。
check "导入路径也校验 token 格式" "awk '/导入的 otbr_token 格式非法/,/^    }/' config_export.c | grep -q \"otbr_token\\[0\\] = '\\\\\\\\0'\""
# setter 只收 32 位小写十六进制：放宽会踩回 aiohttp/yarl 那两个坑
setter_body() { awk '/^esp_err_t config_mgr_set_otbr_token/,/^}/' config_mgr.c; }
check "setter 只收 32 位"        "setter_body | grep -q 'n != 32'"
check "setter 只收小写十六进制"   "setter_body | grep -q \"c >= 'a' && c <= 'f'\""
# 日志会被 /api/v1/logs 读走，凭据不能进去
check "setter 不把 token 打进日志" "! setter_body | grep -q 'ESP_LOGW.*%s.*t)'"
cd - >/dev/null

echo ""
echo "--- T2.7 OTA 回滚可见 + 升级通知发在正确时机 ---"
# webhook 以前订的是 OTA_COMPLETE —— 那之后 500ms 就 esp_restart()，而一次 webhook
# 要走 DNS + TCP + TLS 几秒起步，这条通知从来就没送出去过。
check "webhook 订 OTA_VERIFIED 而非 OTA_COMPLETE" \
    "grep -A6 'base == WEBUI_EVENT' ../webhook/webhook.c | grep -q 'WEBUI_EVENT_ID_OTA_VERIFIED'"
check "webhook 不再订 OTA_COMPLETE 发通知" \
    "! grep -A6 'base == WEBUI_EVENT' ../webhook/webhook.c | grep -q 'id == WEBUI_EVENT_ID_OTA_COMPLETE'"
# 重启前那次事件要**留着**：LED 靠它转 BOOT 态，本地效果 500ms 够用
check "重启前仍 post OTA_COMPLETE（LED 要）" \
    "grep -q 'WEBUI_EVENT_OTA_COMPLETE' rest_api.c"
check "led_ctrl 仍消费 OTA_COMPLETE" \
    "grep -q '_LED_WEBUI_OTA_COMPLETE' ../led_ctrl/led_ctrl.c"
# 跨侧：webhook 抄的 id 与 enum 顺序必须一致（它不能 include webui.h，会成环）
check "webhook 抄的事件 id 与 enum 一致" "python3 ./check_event_ids.py"
# 判据必须在 mark_valid **之前**取 —— PENDING_VERIFY 正是被那行清掉的
check "首启判定取在 mark_valid 之前" "python3 ./check_ota_order.py"
check "/status 露出 ota_slot"            "grep -q 'ota_slot' rest_api.c"
check "/status 露出 last_ota_rolled_back" "grep -q 'last_ota_rolled_back' rest_api.c"
check "回滚真的被查了一次"                "grep -q 'esp_ota_get_last_invalid_partition' rest_api.c ../../main/main.c"

echo ""
echo "--- 第 3 节露出（T3.4 / T3.5 / T3.6）---"
# T3.4：十个计数器都要在回包里，且缓冲要跟着长
for f in uart_rx_errors uart_tx_timeouts tcp_rx_bytes tcp_tx_bytes \
         tcp_connects tcp_disconnects tcp_client_drops \
         usb_rx_bytes usb_tx_bytes ncp_resets_detected; do
    check "zigbee/status 出 $f" "grep -q '$f' rest_api.c"
done
# 1088 装不下：加字段前实测 811 字节，十个计数器最坏再占 280
check "zigbee/status 缓冲已扩容" "! grep -q 'char buf\\[1088\\]' rest_api.c"
# T3.5：python-otbr-api 有这条，HA 的 OTBR 集成用它显示协处理器版本
check "OTBR 有 /node/coprocessor/version" "grep -q 'node/coprocessor/version' otbr_api.c"
check "协处理器版本走 otPlatRadioGetVersionString" "grep -q 'otPlatRadioGetVersionString' otbr_api.c"
check "该端点挂了 OTBR 认证闸门" \
    "grep -A4 'handle_coprocessor_version' otbr_api.c | grep -q 'OTBR_AUTH_REQUIRED'"
# T3.6：关着时不能谎称 scheduled
check "readvertise 回包带 force_bye" "[ \$(grep -c 'force_bye' rest_api.c) -ge 2 ]"
check "FORCE_BYE 关时不回 scheduled" "grep -q 'tracking_only' rest_api.c"

echo ""
echo "--- #17 JSON 回包带 charset（PowerShell 5.1 解码）---"
# 正向：宏定义本身带 charset；反向：任何 .c 里不许再直接写裸 "application/json"
# 给 httpd_resp_set_type（static_files.c 那处是静态文件的 MIME 表，不在此列）。
# ⚠️ 反向用 --strings 保留字面量，否则字符串被剥掉后恒绿。
check "宏 WEBUI_JSON_CONTENT_TYPE 带 charset=utf-8" \
    "grep -q 'WEBUI_JSON_CONTENT_TYPE \"application/json; charset=utf-8\"' include/webui.h"
check "send_json_response 用该宏"      "grep -A2 '^esp_err_t send_json_response' webui_server.c | grep -q 'WEBUI_JSON_CONTENT_TYPE'"
check "无裸 application/json 的 set_type" \
    "python3 ../../tools/grep_code.py --absent --strings -q 'httpd_resp_set_type\\(req, \"application/json\"' *.c"
# T9.24：charset 说的是 UTF-8，正文就得真是 UTF-8。真转义（不是剥离）的地方只许走
# json_escape.c 这一份 —— 2026-10-07 前 rest_api.c 与 otbr_api.c 各有一份，都把 ≥0x80
# 的字节原样照抄。行为判据在 test_host/run.sh（对照 Python 的 UTF-8 解码器）。
check "JSON 转义器只有 json_escape.c 一份" \
    "python3 ../../tools/grep_code.py --absent -q '^static[^;=]*\\bjson_escape\\s*\\(' *.c"
check "日志与 OTBR 网络名都走它" \
    "python3 ../../tools/grep_code.py -q 'webui_json_escape\\(msg,' rest_api.c && [ \$(python3 ../../tools/grep_code.py 'webui_json_escape\\(' otbr_api.c | wc -l) -ge 3 ]"
# T9.32：剥离式拷贝原来有三份（rest_api.c 的 json_safe_copy、webui_server.c 的
# json_sanitize_copy、websocket.c 推 wifi_ssid 的内联循环），都把 >= 0x80 原样照抄。
# 现在只许有 json_escape.c 那一份；cJSON 那条路上唯一不可信的是扫描到的 SSID。
check "剥离式拷贝只有 json_escape.c 一份" \
    "python3 ../../tools/grep_code.py -q '^void json_sanitize_copy\\(' json_escape.c && python3 ../../tools/grep_code.py --absent -q '^(static\\s+)?void\\s+json_(safe|sanitize)_copy\\s*\\(' \$(ls *.c | grep -v '^json_escape.c\$')"
# 按循环的**形状**认（字符字面量会被 grep_code 剥成空白，按引号写的模式在 eval 里也极易恒绿 ——
# 2026-10-07 第一版就是恒绿的，变异测试才发现）
check "没有内联的剥引号循环" \
    "python3 ../../tools/grep_code.py --absent -q '>= 0x20\\)\\s*\\w+\\[\\w+\\+\\+\\]|< 0x20\\)\\s*continue' \$(ls *.c | grep -v '^json_escape.c\$')"
# T9.33：UTF-8 判据全仓只有 components/utf8 一份（config_mgr 的写入口也用它）
check "json_escape.c 不再自带 UTF-8 扫描器" \
    "python3 ../../tools/grep_code.py --absent -q 'size_t\\s+utf8_scan\\s*\\(' *.c && grep -q 'utf8' CMakeLists.txt"
# T9.33：写进配置的字符串必须是合法 UTF-8（SSID 除外），REST 入口先回 400，
# 否则会被 setter 拒掉后报成 500 save_failed / broker_required，把人引去查别处
check "json_opt_str 拒收非法 UTF-8（MQTT / webhook / NTP 都走它）" \
    "awk '/^static bool json_opt_str/,/^}/' rest_api.c | python3 ../../tools/grep_code.py -q 'utf8_valid\\(it->valuestring\\)' /dev/stdin"
check "WiFi 配置：密码查 UTF-8、SSID 不查" \
    "awk '/^static esp_err_t handle_wifi_config/,/^}/' rest_api.c | python3 ../../tools/grep_code.py -q 'utf8_valid\\(pw\\)' /dev/stdin && awk '/^static esp_err_t handle_wifi_config/,/^}/' rest_api.c | python3 ../../tools/grep_code.py --absent -q 'utf8_valid\\(ssid' /dev/stdin"
check "改管理密码拒收非法 UTF-8" \
    "awk '/^static esp_err_t handle_auth_password/,/^}/' auth.c | python3 ../../tools/grep_code.py -q 'utf8_valid\\(pwd->valuestring\\)' /dev/stdin"
check "扫描结果的 SSID 先修 UTF-8 再给 cJSON" \
    "python3 ../../tools/grep_code.py -q 'utf8_repair_copy\\(ssid,' rest_api.c && python3 ../../tools/grep_code.py --absent --strings -q 'cJSON_AddStringToObject\\(ap, \"ssid\", \\(char' rest_api.c"

echo ""
echo "--- URI handler 槽位余量（2026-09-19，第二次踩同一个坑后加的构建期判据）---"
# 静态通配 handler 最后注册，槽位用满时被挤掉的就是它 —— 现象是整个前端 404
# 而 API 全正常。2026-08-11 第一次踩，只加了运行时 ESP_LOGE；2026-09-16 又踩，
# 而那条日志只在串口里，于是 v0.5.0-358/-365 带着"前端全 404"发了出去。
check "槽位余量充足（含 ≥20 空槽）" "python3 ../../tools/check_uri_slots.py"


echo ""
echo "--- coredump 下载（诊断构建，2026-10-07）---"
# 镜像里是崩溃那一刻各任务的栈，可能夹着口令 / 密钥明文：出厂必须编不进来，诊断构建里也要先过守卫再认证
check "coredump 端点出厂默认关"            "awk '/config BITENGINE_DIAG_COREDUMP/{f=1} f' ../../main/Kconfig.projbuild | head -5 | grep -q 'default n'"
check "出厂 sdkconfig / defaults 没开它"    "! grep -qE '^CONFIG_BITENGINE_DIAG_COREDUMP=y' ../../sdkconfig ../../sdkconfig.defaults"
check "coredump handler：跨站守卫在认证之前" "awk '/^static esp_err_t handle_diag_coredump/{f=1} f&&/^}/{exit} f' rest_api.c | grep -n 'CROSS_ORIGIN_GUARD\\|AUTH_REQUIRED' | head -2 | tr '\\n' ' ' | grep -qE '^[0-9]+:.*CROSS_ORIGIN_GUARD.* [0-9]+:.*AUTH_REQUIRED'"
check "注册潮 POST：跨站守卫在认证之前"      "awk '/^static esp_err_t handle_diag_srp_burst_post/{f=1} f&&/^}/{exit} f' rest_api.c | grep -n 'CROSS_ORIGIN_GUARD\\|AUTH_REQUIRED' | head -2 | tr '\\n' ' ' | grep -qE '^[0-9]+:.*CROSS_ORIGIN_GUARD.* [0-9]+:.*AUTH_REQUIRED'"

echo ""
echo "--- 路由表不许放栈上（2026-10-07）---"
# 看门狗 L3 在 wd_action（3072 栈）里经 webui_httpd_restart() 重新注册全部路由。局部表每次调用现搭在栈上
# （rest_api.c 两张合计 93 条 ≈ 2.6KB），当天 coredump：wd_action 2960/3068 时溢出，httpd 卡住的恢复变成整机 panic。
check "所有 httpd_uri_t 表都是 static"       "python3 ../../tools/grep_code.py --absent -q '(?<!static )const\\s+httpd_uri_t\\s+\\w+\\s*\\[' *.c"
check "判据没失明：static 路由表找得到 6 张"  "[ \$(python3 ../../tools/grep_code.py 'static const httpd_uri_t \\w+\\[' *.c | wc -l) -ge 6 ]"
# 防串刷（docs/15 设计 2.7、架构 5.8 R-9；2026-10-21）：网页上传在 esp_ota_begin 之前比对镜像的工程名，
# 不一致回 409 profile_mismatch、一个字节都不写。判据正本在 tools/check_profile_rules.py（与 ota_cloud 共用）。
check "R-9：网页上传写 flash 之前比对工程名"  "python3 ../../tools/check_profile_rules.py --only r9"
# 网页上传边写边擦（2026-10-21 审计）：OTA_SIZE_UNKNOWN 会在 esp_ota_begin 里一次擦掉整个 3 MB 槽（esp_ota_ops.c），
# 几秒的擦除会让 RS706 喂狗断档；AM2 修好 A6（X7）之后就是升级途中整板复位。只有 OTA_WITH_SEQUENTIAL_WRITES 是逐扇区擦。
check "网页上传 esp_ota_begin 用 OTA_WITH_SEQUENTIAL_WRITES" "python3 ../../tools/grep_code.py -q 'esp_ota_begin\\(update_part, OTA_WITH_SEQUENTIAL_WRITES' rest_api.c"
check "rest_api.c 代码里不再用 OTA_SIZE_UNKNOWN"   "python3 ../../tools/grep_code.py --absent -q 'OTA_SIZE_UNKNOWN' rest_api.c"

# 安全模式退出时"先关掉 Muse"（docs/15 附录 E33）：先判安全模式再记留言（不在安全模式什么都不写）；没记下就不退出；
# 整段只在 MUSE_LINK 构建里（HA 型的这个端点不读请求体，与以前一样）
check "safemode/exit：判安全模式 → 记留言 → 退出，没记下就 return" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('rest_api.c').read())
f=re.search(r'\nstatic esp_err_t handle_safemode_exit\(httpd_req_t \*req\)\n\{(.*?)\n\}', S, re.S).group(1)
p=[f.find(x) for x in ('CROSS_ORIGIN_GUARD(','AUTH_REQUIRED(','if (!wd_safe_mode())','muse_hold_request()','wd_safe_mode_exit()')]
ok = -1 not in p and p==sorted(p) and 'if (muse_off && muse_hold_request() != ESP_OK) return send_error' in f
m=re.search(r'#if CONFIG_BITENGINE_ENABLE_MUSE_LINK(.*?)#else(.*?)#endif', f, re.S)
ok = ok and m and 'muse_hold_request' in m.group(1)
# HA 型那一支（#else）与改动前逐字一致：只发固定回包，不读请求体（连字符串一起看）
F=strip_comments(open('rest_api.c').read(), keep_strings=True)
g=re.search(r'\nstatic esp_err_t handle_safemode_exit\(httpd_req_t \*req\)\n\{(.*?)\n\}', F, re.S).group(1)
e=re.search(r'#else(.*?)#endif', g, re.S).group(1).strip()
ok = ok and re.sub(r'[^A-Za-z_(),:{};]', '', e) == 'send_json_response(req,{status:ok,note:rebooting});'
sys.exit(0 if ok else 1)\""

# 层 B 心跳：直连型 hub 接口每处理完一条请求记一次（2026-10-24：连点十几次失联的灯，每条同步等 8 s，心跳工作项排不上、L3 → L4 整机重启）。
# 声明 / 定义都只在直连型里、放在文件末尾（HA 型产物一个字节不变）；kick 的是 httpd 的槽；两个同步等设备的入口在 run_command 之后都调
check "ws_httpd_progress：直连型专属、kick httpd 槽、只在 /hub/invoke 同步等了 ≥ 1 s 之后调（快请求 / 轮询不调）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
H=strip_comments(open('include/webui.h').read()); W=strip_comments(open('websocket.c').read()); A=strip_comments(open('hub_api.c').read())
blk=lambda S: re.findall(r'#if CONFIG_BITENGINE_PROFILE_DIRECT\n(.*?)#endif', S, re.S)
ok = any('void ws_httpd_progress(void);' in b for b in blk(H)) and H.count('ws_httpd_progress') == 1
d=[b for b in blk(W) if 'void ws_httpd_progress(void)' in b]
ok = ok and len(d) == 1 and re.search(r'if \(s_wd_slot_httpd >= 0\) wd_kick\(s_wd_slot_httpd\);', d[0]) and W.count('ws_httpd_progress') == 1
ok = ok and W.rstrip().endswith('#endif') and H.rstrip().endswith('#endif')   # 末尾：前面的行号不动
f=re.search(r'\nstatic esp_err_t handle_invoke\(httpd_req_t \*req\)\n\{(.*?)\n\}', A, re.S).group(1)
ok = ok and re.search(r'run_command\(req, [^;]*params\);\s*const uint32_t ms = \(uint32_t\)\(\(esp_timer_get_time\(\) - t0\) / 1000\);\s*if \(ms >= 1000u\) ws_httpd_progress\(\);', f) is not None
ok = ok and A.count('ws_httpd_progress') == 1   # /hub/devices（页面每 10 s 轮询）不调
sys.exit(0 if ok else 1)\""

echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
