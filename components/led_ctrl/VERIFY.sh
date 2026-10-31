#!/bin/bash
# Phase 2 Aux Modules VERIFY.sh — covers all 7 modules
# CREATED BEFORE ANY IMPLEMENTATION CODE (P1)
BASE="$(dirname "$0")/.."
PASS=0; FAIL=0
check() { if eval "$2" > /dev/null 2>&1; then echo "✅ $1"; ((PASS++)); else echo "❌ $1"; ((FAIL++)); fi; }

echo "=== Phase 2 Aux Modules Verification ==="

echo ""
echo "--- led_ctrl ---"
# 只在私有检出里有的内部文档 / 工具（docs/、发布与产线脚本）：公开仓库不导出它们（tools/oss_export.py），
# 那里跳过而不是报红；私有检出里照常判。标志是 docs/开发索引.md 在不在。
internal() { if [ -f ../../docs/开发索引.md ]; then check "$@"; else echo "⏭  $1 —— 依赖内部文档 / 工具，本检出没有，跳过"; fi; }

check "led_ctrl.h exists"           "test -f $BASE/led_ctrl/include/led_ctrl.h"
check "led_ctrl.c exists"           "test -f $BASE/led_ctrl/led_ctrl.c"
check "led_ctrl_init implemented"   "grep 'led_ctrl_init' $BASE/led_ctrl/led_ctrl.c"
check "led_ctrl_set_state impl"     "grep 'led_ctrl_set_state' $BASE/led_ctrl/led_ctrl.c"
check "LED_STATE enum"              "grep 'LED_STATE_NORMAL' $BASE/led_ctrl/include/led_ctrl.h"
check "Subscribes NETWORK_EVENT"    "grep 'NETWORK_EVENT' $BASE/led_ctrl/led_ctrl.c"
check "Subscribes WATCHDOG_EVENT"   "grep 'WATCHDOG_EVENT' $BASE/led_ctrl/led_ctrl.c"
check "Subscribes WEBUI_EVENT"      "grep 'WEBUI_EVENT' $BASE/led_ctrl/led_ctrl.c"
check "CONFIG_HAS_WS2812B ifdef"    "grep 'CONFIG_BITENGINE_HAS_WS2812B' $BASE/led_ctrl/led_ctrl.c"
# NETWORK_EVENT_ONLINE 是边沿触发，而 ESP-IDF 默认事件循环不粘不重放：led_ctrl_init()
# 在 main.c 里比 network_mgr_init() 晚近十个组件，以太网早已上线完毕。不补读就永久
# 停在 boot —— SYS 黄常亮而 zigbee/thread/net 全绿（2026-08-13 实测到）。
check "init 补读当前网络状态"       "sed -n '/^esp_err_t led_ctrl_init/,/^}/p' $BASE/led_ctrl/led_ctrl.c | grep -q network_mgr_get_state"
# 补读之后再赋 BOOT 会把补读和事件的结果一起盖掉，等于没修
check "init 末尾不再复位 s_state"   "! sed -n '/psram_task_create(led_task/,/^}/p' $BASE/led_ctrl/led_ctrl.c | grep -q 's_state ='"
# 没有这个出口，SYS 灯对不对就只有眼睛能判，回归测试无从下手
check "led_ctrl_state_name 已导出"  "grep -q 'led_ctrl_state_name' $BASE/led_ctrl/include/led_ctrl.h"
check "status 上报 led_state"       "grep -q 'led_state' $BASE/webui/rest_api.c"
# app_main 在 network_mgr_init() 之后会阻塞等联网（最长 10 分钟）。led_ctrl 若排在
# 那道闸门之后，整个等待期四颗灯全黑，开机要"过好一会儿"SYS 才亮；排在之前则
# 网络事件一条都不会漏。这条顺序是功能性的，不是风格。
check "led_ctrl_init 早于 network_mgr_init" "[ \$(grep -n '^    led_ctrl_init();' $BASE/../main/main.c | cut -d: -f1) -lt \$(grep -n 'ret = network_mgr_init()' $BASE/../main/main.c | cut -d: -f1) ]"
# 安全模式分支曾另调一次，重复注册 handler + 多起一个 led_task
check "main.c 只调一次 led_ctrl_init"       "[ \$(grep -c '^    led_ctrl_init();' $BASE/../main/main.c) -eq 1 ]"

echo ""
echo "--- button ---"
check "button.h exists"             "test -f $BASE/button/include/button.h"
check "button.c exists"             "test -f $BASE/button/button.c"
check "button_init implemented"     "grep 'button_init' $BASE/button/button.c"
check "CONFIG_HAS_BUTTON ifdef"     "grep 'CONFIG_BITENGINE_HAS_BUTTON' $BASE/button/button.c"
# 2026-08-14 起按键只剩重启和恢复出厂两个动作，下面两条改为**反向**断言。
# 撤销理由见 docs/07 §2.2：切桥模式/清密码 WebUI 能做且误触代价高；单独复位
# MG24 在本板做不到（拉低 IO23 是整板重启）。加回来前先读那一节。
check "不再单独复位 MG24"           "! grep -q 'watchdog_reset_mg24' $BASE/button/button.c"
check "Calls factory_reset"         "grep 'factory_reset' $BASE/button/button.c"
check "不再清 WebUI 密码"           "! grep -q 'clear_password' $BASE/button/button.c"

echo ""
echo "--- mqtt_client ---"
check "mqtt header exists"          "test -f $BASE/mqtt_client/include/bitengine_mqtt.h"
check "mqtt source exists"          "test -f $BASE/mqtt_client/bitengine_mqtt.c"
check "mqtt_client_init impl"       "grep 'mqtt_client_init' $BASE/mqtt_client/bitengine_mqtt.c"
check "mqtt_client_is_connected"    "grep 'mqtt_client_is_connected' $BASE/mqtt_client/bitengine_mqtt.c"
check "HA discovery publish"        "grep -i 'discovery\|homeassistant' $BASE/mqtt_client/bitengine_mqtt.c"
check "LWT configured"             "grep -i 'lwt\|last_will\|will_msg' $BASE/mqtt_client/bitengine_mqtt.c"
check "Subscribes CONFIG_EVENT"     "grep 'CONFIG_EVENT_MQTT' $BASE/mqtt_client/bitengine_mqtt.c"
check "Subscribes WATCHDOG_EVENT"   "grep 'WATCHDOG_EVENT' $BASE/mqtt_client/bitengine_mqtt.c"
check "config.mqtt.enabled check"   "grep 'cfg->enabled\|mqtt.*enabled' $BASE/mqtt_client/bitengine_mqtt.c"

echo ""
echo "--- webhook ---"
check "webhook.h exists"            "test -f $BASE/webhook/include/webhook.h"
check "webhook.c exists"            "test -f $BASE/webhook/webhook.c"
check "webhook_init implemented"    "grep 'webhook_init' $BASE/webhook/webhook.c"
check "webhook_post_event impl"     "grep 'webhook_post_event' $BASE/webhook/webhook.c"
check "xQueueReceive usage"         "grep 'xQueueReceive' $BASE/webhook/webhook.c"
check "esp_http_client usage"       "grep 'esp_http_client' $BASE/webhook/webhook.c"
check "5s HTTP timeout"             "grep '5000\|WEBHOOK_TIMEOUT' $BASE/webhook/webhook.c"
check "Subscribes WATCHDOG_EVENT"   "grep 'WATCHDOG_EVENT' $BASE/webhook/webhook.c"
check "Subscribes NETWORK_EVENT"    "grep 'NETWORK_EVENT' $BASE/webhook/webhook.c"
check "Event enable flag check"     "grep 'event_zigbee_crash\|event_network' $BASE/webhook/webhook.c"

echo ""
echo "--- mdns_svc ---"
check "mdns_svc.h exists"           "test -f $BASE/mdns_svc/include/mdns_svc.h"
check "mdns_svc.c exists"           "test -f $BASE/mdns_svc/mdns_svc.c"
check "mdns_svc_init implemented"   "grep 'mdns_svc_init' $BASE/mdns_svc/mdns_svc.c"
check "_zigbee-bridge._tcp service" "grep 'zigbee-bridge' $BASE/mdns_svc/mdns_svc.c"
check "_http._tcp service"          "grep '_http._tcp' $BASE/mdns_svc/mdns_svc.c"
check "TXT records (model/fw)"      "grep 'model\|fw_version' $BASE/mdns_svc/mdns_svc.c"
check "Hostname from config_mgr"    "grep 'config_mgr_get' $BASE/mdns_svc/mdns_svc.c"
check "CONFIG_EVENT_HOSTNAME"       "grep 'HOSTNAME' $BASE/mdns_svc/mdns_svc.c"

echo ""
echo "--- scheduler ---"
check "scheduler.h exists"          "test -f $BASE/scheduler/include/scheduler.h"
check "scheduler.c exists"          "test -f $BASE/scheduler/scheduler.c"
check "scheduler_init implemented"  "grep 'scheduler_init' $BASE/scheduler/scheduler.c"
check "Night mode logic"            "grep -i 'night' $BASE/scheduler/scheduler.c"
check "Time validity check"         "grep '2024\|tm_year' $BASE/scheduler/scheduler.c"
check "Backup interval check"       "grep 'backup_interval' $BASE/scheduler/scheduler.c"
check "60s loop interval"           "grep '60000' $BASE/scheduler/scheduler.c"

echo ""
echo "--- main.c integration ---"
check "mdns_svc_init in main"       "grep 'mdns_svc_init' $BASE/../main/main.c"
check "mqtt_client_init in main"    "grep 'mqtt_client_init' $BASE/../main/main.c"
check "webhook_init in main"        "grep 'webhook_init' $BASE/../main/main.c"
check "led_ctrl_init in main"       "grep 'led_ctrl_init' $BASE/../main/main.c"
check "button_init in main"         "grep 'button_init' $BASE/../main/main.c"
check "scheduler_init in main"      "grep 'scheduler_init' $BASE/../main/main.c"

echo ""
echo "==============================="

echo ""
echo "--- thread 灯判据（2026-08-15 重定义）---"
# 旧判据只数网关的**直连**子节点。网状网里终端设备会挂到离它更近的路由器下面：
# 实测门窗传感器改挂到另一台 FTD 之后，网关子节点表为空、HA 一切正常，而一条
# 再也回不来的 restoring 残留条目让灯黄闪了好几分钟；反方向 role=detached 时
# 灯却是绿的——最该报警时反而说正常。两个方向都错，判据整个换掉。
check "灯看 attached 不看子节点数" "grep -q 's_thread_attached' $BASE/led_ctrl/led_ctrl.c"
check "子节点数不再喂给灯"        "! grep -q 's_thread_children' $BASE/led_ctrl/led_ctrl.c"
check "recovery setter 已删除"    "! grep -q 'led_ctrl_set_thread_recovery' $BASE/led_ctrl/led_ctrl.c"
check "health 带 attached 形参"   "grep -q 'led_ctrl_set_thread_health(bool running, bool radio_stalled, bool attached)' $BASE/led_ctrl/led_ctrl.c"
# 射频停摆仍然优先于"没入网"：前者是故障，后者只是进行中
check "射频停摆判在 attached 之前" "[ $(grep -n 's_thread_radio_stalled)' $BASE/led_ctrl/led_ctrl.c | head -1 | cut -d: -f1) -lt $(grep -n '!s_thread_attached' $BASE/led_ctrl/led_ctrl.c | head -1 | cut -d: -f1) ]"
# 2026-10-05 拔网线实测：Thread 网内部正常（leader）而边界路由停了，HA 连不到任何 Thread
# 设备，灯却是绿的（测试 6c）。边界路由停了必须落在琥珀档，判在绿灯之前。
check "THREAD 灯换档记日志（写明判据）" "grep -q 'THREAD 灯 → %s' $BASE/led_ctrl/led_ctrl.c && grep -q '琥珀慢闪（边界路由停了）' $BASE/led_ctrl/led_ctrl.c"
check "有边界路由输入"                 "grep -q 'void led_ctrl_set_thread_border_routing(bool running)' $BASE/led_ctrl/led_ctrl.c"
check "边界路由停了判在 attached 之后、绿灯之前" "[ $(grep -n '!s_thread_attached' $BASE/led_ctrl/led_ctrl.c | head -1 | cut -d: -f1) -lt $(grep -n '!s_thread_br_ok' $BASE/led_ctrl/led_ctrl.c | head -1 | cut -d: -f1) ] && [ $(grep -n '!s_thread_br_ok' $BASE/led_ctrl/led_ctrl.c | head -1 | cut -d: -f1) -lt $(grep -n 'zled_set_pixel(LED_THREAD, 0, scale(200, b), 0)' $BASE/led_ctrl/led_ctrl.c | head -1 | cut -d: -f1) ]"


echo ""
echo "--- H1 夜间模式必须与 s_state 正交（2026-08-15）---"
# 曾经 NIGHT 是 s_state 的一个枚举值：事件处理器写 s_state 时不看夜间，
# 夜里网络一抖就把它顶掉；而 scheduler 是边沿触发的，不会再设回来 → 灯亮到天亮
check "有独立的 s_night"          "grep -q 'static volatile bool        s_night' $BASE/led_ctrl/led_ctrl.c"
check "渲染看 s_night 不看枚举"    "grep -q 'if (s_night) {' $BASE/led_ctrl/led_ctrl.c"
check "不再有 s_state == NIGHT"   "! grep -q 's_state == LED_STATE_NIGHT' $BASE/led_ctrl/led_ctrl.c"
check "set_state(NIGHT) 被拦截"    "grep -A3 'if (state == LED_STATE_NIGHT)' $BASE/led_ctrl/led_ctrl.c | grep -q 'led_ctrl_set_night(true)'"
# 对外契约不变：/api/v1/status 与 tools/test_api.sh 仍按 "night" 认
check "get_state 仍合并回 night"   "grep -q 's_night ? LED_STATE_NIGHT : effective_state()' $BASE/led_ctrl/led_ctrl.c"
check "scheduler 改用新接口"       "! grep -q 'led_ctrl_set_state(LED_STATE_NIGHT)' $BASE/scheduler/scheduler.c"
# 从 webui/VERIFY.sh 搬来（2026-08-16）：它断言的是本模块的源文件，却长在 webui 那边，
# 于是 2026-08-15 拆 s_night 时没人看见它。熄灯的唯一权威是 scheduler 的时间窗算出的
# s_night，led_ctrl 不得拿配置里的开关字段直接熄灯 —— 那正是 2026-08-12 修掉的那个 bug
# （下午 2 点打开开关灯就再也不亮）。只禁止把它用在条件里，不禁止提它。
check "熄灯条件不看 enabled 字段"  "! grep -Eq '(if *\(|\|\||&&).*night_mode_enabled' $BASE/led_ctrl/led_ctrl.c"

echo ""
echo "--- H4 RMT 建不起来不得静默 ---"
# 以前只在启动打一条 ESP_LOGE，之后每帧无声 return：现场是四颗灯全黑而
# /api/v1/status 的 led_state 一切正常，排查的人拿不到任何线索
check "未就绪时周期性告警"         "grep -A3 'if (!s_led_ready) {' $BASE/led_ctrl/led_ctrl.c | grep -q 'ESP_LOGE'"
check "未就绪时会重试重建"         "grep -A4 'if (!s_led_ready) {' $BASE/led_ctrl/led_ctrl.c | grep -q 'led_strip_create()'"

echo ""
echo "--- H3 诊断动作不得撑爆看门狗槽 ---"
# sweep 约 11s、probe 最长 60s，都同步跑在渲染任务里，而槽 deadline 是 5000ms
check "sweep 循环内喂狗"           "grep -A10 'k_sweep\[0\]); v++)' $BASE/led_ctrl/led_ctrl.c | grep -q 'wd_kick'"
check "probe 循环内喂狗"           "grep -A2 'i < sec \* 2; i++' $BASE/led_ctrl/led_ctrl.c | grep -q 'wd_kick'"


echo ""
echo "--- 像素回读：让 LED 行为可机器断言 ---"
# 在此之前"灯此刻在显示什么"没有任何机器可读出口，led_state 报的是语义状态，
# 中间还隔着覆盖层/夜间抑制/RMT 就绪三道关 —— 只能问人"闪了吗"，而人看不见
# 50ms 一帧的闪烁。回归脚本 tools/test_night_mode.sh 依赖这个接口。
check "有 get_pixels"             "grep -q 'int led_ctrl_get_pixels' $BASE/led_ctrl/led_ctrl.c"
check "回读转成 RGB（线上是 GRB）" "grep -A6 'int led_ctrl_get_pixels' $BASE/led_ctrl/led_ctrl.c | grep -q 'R —— 线上是 GRB'"
check "REST 暴露 pixels"          "grep -q 'led_ctrl_get_pixels(buf' $BASE/webui/rest_api.c"


echo ""
echo "--- H6 状态分层：网络态与瞬时事件不得互相覆盖 ---"
# 以前两者挤在一个 s_state 里，于是每个报错点都要记一份"待会儿恢复成什么"
# （s_error_recover_to），4 个点各猜各的；MG24 那处还要靠 prev==ERROR?NORMAL:prev
# 猜前一个状态。猜错就是灯长期显示错的东西。
check "s_state 已拆掉"            "! grep -qE '^static volatile led_state_t s_state' $BASE/led_ctrl/led_ctrl.c"
check "有独立的网络态"            "grep -q 's_net_state' $BASE/led_ctrl/led_ctrl.c"
check "有独立的事件层"            "grep -q 's_event_on' $BASE/led_ctrl/led_ctrl.c"
check "恢复记账已删除"            "! grep -q 'led_state_t s_error_recover_to' $BASE/led_ctrl/led_ctrl.c"
check "死变量 s_prev_state 已删"  "! grep -q 's_prev_state' $BASE/led_ctrl/led_ctrl.c"
# 事件到期必须落回网络态，而不是落回某个记下来的值
check "事件到期只清标志"          "grep -A2 'tick_reached(s_event_until)' $BASE/led_ctrl/led_ctrl.c | grep -q 's_event_on = false'"
check "渲染用 effective_state"    "grep -q 'switch (effective_state())' $BASE/led_ctrl/led_ctrl.c"

echo ""
echo "--- L1/L2/L3 ---"
# tick 是 32 位，直接比大小在回绕点（约 497 天）会瞬间判成已到期或永不到期
check "L2 tick 比较 wrap-safe"    "grep -q '(int32_t)(xTaskGetTickCount() - deadline) >= 0' $BASE/led_ctrl/led_ctrl.c"
check "L2 覆盖层也用 tick_reached" "grep -q 'if (!tick_reached(s_test_until))' $BASE/led_ctrl/led_ctrl.c"
# 像素数组非 volatile、deadline 是 volatile，无屏障时编译器可把 deadline 的写提前
check "L1 有配对的内存屏障"        "[ $(grep -c '__atomic_thread_fence' $BASE/led_ctrl/led_ctrl.c) -ge 2 ]"
# 夜里 RMT 挂了没有任何痕迹，而"灯全灭"和夜间模式长得一模一样
check "L3 三条出口共用限流告警"    "[ $(grep -c 'refresh_checked();' $BASE/led_ctrl/led_ctrl.c) -eq 3 ]"

echo ""
echo "--- T4.4 两盏活动灯真的被触发了 ---"
# ⚠️ 这一组存在的理由：渲染档位写好了、notify 函数写好了、头文件声明也有，
#    **就是没人调**。Zigbee 那半边 2026-09-11 接上了，NET 那半边当天被漏掉，
#    直到用户肉眼发现"net 上没看到闪烁"才暴露 —— 而那正是第 4 节要消灭的形状。
#    所以两盏灯都必须逐条验"有真实调用者"，别只验渲染分支还在。
check "zigbee notify 有真实调用者"  "python3 ../../tools/grep_code.py -q led_ctrl_notify_zigbee_activity ../ezsp_bridge/tcp_server.c"
check "zigbee 收发两个方向都接了"   "[ \$(python3 ../../tools/grep_code.py led_activity_tick ../ezsp_bridge/tcp_server.c | wc -l) -ge 3 ]"
# ⚠️ 必须节流：EZSP 是字节流，Z2M 正常工作时每秒几十到几百次读写，
#    不节流等于每帧都在闪，既看不出"有活动"又白占 CPU。
# ⚠️ 判据必须落在 led_activity_tick() **函数体内**。上一版写的是
#    `grep -q '100000' tcp_server.c`，匹配到的其实是 select() 的
#    `tv_usec = 100000`，跟 LED 毫无关系 —— 把节流整段删掉它照样绿，
#    而且标签写"100ms"、真实值是 400ms。假绿的断言比没有断言更糟。
check "zigbee 节流在函数体内且为 400ms" \
      "awk '/^static void led_activity_tick/,/^}/' ../ezsp_bridge/tcp_server.c | grep -q '400000'"

echo ""
echo "--- NET 活动闪（2026-09-11 补接，用户肉眼发现漏了）---"
check "net notify 有真实调用者"     "python3 ./check_net_activity.py --callers"
# 节流必须在 led_ctrl 这个汇合点，不在各调用方：四个来源各自 400ms 叠起来最快
# 每 100ms 置位一次，而保持窗口 120ms —— 结果是常白，把"绿=在线"盖掉。
check "net 节流在 led_ctrl 汇合点"  "awk '/^void led_ctrl_notify_network_activity/,/^}/' led_ctrl.c | grep -q 'NET_ACTIVITY_THROTTLE_MS'"
check "net 节流明显大于保持窗口"    "python3 ./check_net_activity.py --timing"
check "两盏灯的渲染档位都仍在"      "grep -q 's_zigbee_activity' led_ctrl.c && grep -q 's_net_activity' led_ctrl.c"

# 灯的行为定义散在五份文档里（用户手册 / 01 架构 / 07 设计 / 产测指南 / 00 现状），
# 2026-09-12 逐份核对时发现它们已经各自漂了 —— 最离谱的一条是 docs/01 写着
# "Thread 活动时青色闪烁"，而那个功能**从来不存在**，连渲染档位都没有。
internal "文档与灯的行为一致"          "python3 ./check_led_docs.py"

# 热点关了却没有随后的 ONLINE（网口在线时开、关热点）：以前 SYS 灯一直黄呼吸（2026-10-08 T9.40 审计）
check "热点关闭（SOFTAP_STOPPED）时 SYS 灯落回主状态" \
    "awk '/^static void led_event_handler\(/{f=1} f{print} f&&/^}/{exit}' led_ctrl.c | python3 ../../tools/grep_code.py -q 'id == NETWORK_EVENT_SOFTAP_STOPPED' /dev/stdin"

# ---- 直连型 ZIGBEE 灯（CHECKLIST Z54，2026-10-24 用户定）：没有桥，看 zhub 推来的状态；HA 型那一支原样 ----
check "直连型 ZIGBEE 灯：四种状态各有一档（绿 / 黄慢闪 / 红慢闪 / 灭），只在 ENABLE_ZHUB 里；HA 型照旧看桥" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('led_ctrl.c').read())
m=re.search(r'#if CONFIG_BITENGINE_ENABLE_ZHUB\s*switch \(s_zb_hub\) \{(.*?)\}\s*#else\s*if \(s_zb_mode_off\) \{(.*?)#endif', S, re.S)
ok = m is not None
d = m.group(1) if m else ''
ok = ok and all(k in d for k in ('case LED_ZB_HUB_UP:', 'case LED_ZB_HUB_STARTING:', 'case LED_ZB_HUB_JOIN:', 'case LED_ZB_HUB_FAIL:', 'default:'))
ok = ok and 'zled_set_pixel(LED_ZIGBEE, 0, scale(200, b), 0);' in d and 'scale(pulse ? 255 : 20, b), scale(pulse ? 140 : 11, b), 0' in d and 'scale(pulse ? 255 : 0, b), 0, 0' in d
ok = ok and 's_zigbee_activity_until' in d.split('case LED_ZB_HUB_STARTING:')[0]   # 蓝闪只叠在\"在线\"上
ok = ok and 'if (s_zigbee_activity_until && tick_reached(s_zigbee_activity_until)) s_zigbee_activity_until = 0;' in d   # 到期清零（24.9 天回绕，审计 ⚠️）
H=open('include/led_ctrl.h').read()
ok = ok and re.search(r'#if CONFIG_BITENGINE_ENABLE_ZHUB.*void\s+led_ctrl_set_zigbee_hub\(led_zb_hub_t st\);\s*#endif\s*$', H, re.S)
sys.exit(0 if ok else 1)\""
check "zhub 每圈推灯的状态（没拿 UART / 建网中 = 黄、连着失败 = 红、在线 = 绿、开窗 = 黄），设备消息蓝闪 400 ms 节流" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
Z=strip_comments(open('../zhub/zhub.c').read())
f=re.search(r'static void led_push\(bool claim_stuck\)\n\{(.*?)\n\}', Z, re.S).group(1)
p=[f.find(x) for x in ('if (holding && s_z->h.state == ZH_HOST_RUNNING && s_z->h.net == ZH_NET_UP)', 'LED_ZB_HUB_JOIN : LED_ZB_HUB_UP', 'else if ((holding && zh_host_failing(&s_z->h)) || (!holding && claim_stuck)) st = LED_ZB_HUB_FAIL;', 'else st = LED_ZB_HUB_STARTING;', 'led_ctrl_set_zigbee_hub(st);')]
ok = -1 not in p and p == sorted(p)   # 在线先判（恢复后 fails 晚 60 s 才清零，审计 ⚠️）
t=re.search(r'static void zhub_task\(void \*arg\)\n\{(.*?)\n\}', Z, re.S).group(1)
ok = ok and re.search(r'snapshot\(\);\s*led_push\(atomic_load\(&s_suspend_cnt\) == 0 && claim_misses >= ZHUB_CLAIM_WARN\);', t)
ok = ok and re.search(r'if \(t - s_t_led_act >= ZHUB_LED_ACT_MS\) \{\s*s_t_led_act = t;\s*led_ctrl_notify_zigbee_activity\(\);', Z) and re.search(r'#define ZHUB_LED_ACT_MS\s+400', open('../zhub/zhub.c').read())
ok = ok and 'led_ctrl' in open('../zhub/CMakeLists.txt').read()
sys.exit(0 if ok else 1)\""

echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
