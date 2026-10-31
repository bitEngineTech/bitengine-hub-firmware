#!/bin/bash
# components/mqtt_client/VERIFY.sh —— 设计约束的机器判据
#
#   cd components/mqtt_client && bash VERIFY.sh
#
# ── 为什么这份来得这么晚（T2.9）────────────────────────────────────────
#
# CLAUDE.md 里那套 S1/S2/S3 审查模板，**出处就是这个模块**：07 模块批量开发时
# 一口气写完 7 个文件才审计，抓出 9 个 bug，其中一个 CRITICAL 就是
# 「MQTT restart 每次泄漏 task + event handler」。规范因它而生，它自己却一直
# 没有 VERIFY.sh —— 规则写了不执行等于没有规则，这份就是来补这个洞的。
#
# 所以下面的重点不是"函数在不在"，而是那个 CRITICAL 不能复发：
# task 只建一次、handler 只注册一次、restart 走标志位而不是重新创建任何东西。

PASS=0; FAIL=0
DIR="$(cd "$(dirname "$0")" && pwd)"
C="$DIR/bitengine_mqtt.c"
H="$DIR/include/mqtt_client.h"
[ -f "$H" ] || H=$(ls "$DIR"/include/*.h 2>/dev/null | head -1)

check() {
    if eval "$2" > /dev/null 2>&1; then echo "✅ $1"; PASS=$((PASS+1))
    else echo "❌ $1"; FAIL=$((FAIL+1)); fi
}

echo "--- API 完整性 ---"
for fn in mqtt_client_init mqtt_client_stop mqtt_client_restart \
          mqtt_client_is_connected mqtt_client_publish; do
    check "$fn 已声明"  "grep -q '$fn' '$H'"
    check "$fn 已实现"  "grep -qE '^[a-z_]+ +$fn\\(|^bool +$fn\\(|^void +$fn\\(' '$C'"
done

echo ""
echo "--- ⚠️ 07 模块那个 CRITICAL：restart 不能泄漏 task / handler ---"
# 原缺陷：每次配置变更都重新 xTaskCreate + 重新注册 event handler，
# 改十次配置就有十个 task 和十份 handler，且旧的永不退出。
# ⚠️ 窗口要够大：守卫与建 task 之间隔着 wd_spec 的声明和两次 wd_register，
#    -B2 看不到（第一版就是这么红的，而代码本来是对的）。
check "task 只建一次（有 s_task_running 守卫）" \
    "grep -B10 'psram_task_create\\|xTaskCreate' '$C' | grep -q 'if (!s_task_running)'"
check "handler 只注册一次（有 s_events_registered 守卫）" \
    "grep -B2 'esp_event_handler_instance_register' '$C' | grep -q 's_events_registered'"
check "全文件只有一处创建 task" \
    "[ \$(python3 ../../tools/grep_code.py 'psram_task_create|xTaskCreate' '$C' | wc -l) -eq 1 ]"   # 剥注释再数（7.4：T9.38 的注释里提到 esp-mqtt 用 xTaskCreate）
# restart 必须只置一个标志位，真正的重启在 task 里做
check "restart 只置标志位" \
    "python3 '$DIR/check_restart.py'"
check "task 循环里消费该标志位" \
    "grep -q 's_restart_requested = false' '$C'"

echo ""
echo "--- S2 线程上下文 ---"
# event handler 里**不能**直接停客户端：esp_mqtt_client_stop 会等事件循环，
# 而我们正在那个循环里 —— 自己等自己。代码里那行注释写明了这一点。
check "event handler 不直接 stop/destroy 客户端" \
    "! grep -A12 'static void system_event_handler' '$C' | grep -qE 'esp_mqtt_client_(stop|destroy)'"
check "注明了为什么放到 task 里做" \
    "grep -q 'avoid deadlock\\|NOT here' '$C'"
# 周期发布用 enqueue（非阻塞）：publish 在断线时会阻塞到超时，把发布 task 卡住
check "通用 publish 用 enqueue 而非阻塞 publish" \
    "grep -A12 'void mqtt_client_publish' '$C' | grep -q 'esp_mqtt_client_enqueue'"
check "publish 里不用阻塞版 publish" \
    "! grep -A12 'void mqtt_client_publish' '$C' | grep -q 'esp_mqtt_client_publish'"

echo ""
echo "--- S3 资源生命周期 ---"
# ⚠️ 用 grep_code.py 数**代码里**的调用：注释里有一句 "ifr 必须活到
#    esp_mqtt_client_init() 之后"，裸 grep -c 会把它算成第二次 init。
check "client_init 与 client_destroy 成对" \
    "[ \$(python3 $DIR/../../tools/grep_code.py esp_mqtt_client_init '$C' | wc -l) \
       -le \$(python3 $DIR/../../tools/grep_code.py esp_mqtt_client_destroy '$C' | wc -l) ]"
# destroy 必须在锁内，且先把 s_client 置空：否则别的 task 正拿着句柄 publish
# 就是 use-after-free
check "destroy 前先把 s_client 置空" \
    "grep -B4 'esp_mqtt_client_destroy' '$C' | grep -q 's_client = NULL'"
check "stop/destroy 在 client 锁内" \
    "grep -B8 'esp_mqtt_client_destroy' '$C' | grep -q 'xSemaphoreTake(s_client_lock'"
check "publish 也取同一把锁" \
    "grep -A6 'void mqtt_client_publish' '$C' | grep -q 'xSemaphoreTake(s_client_lock'"

echo ""
echo "--- S1 安全 ---"
check "不打印 broker 密码"  "! grep -qE 'ESP_LOG.*(password|passwd)' '$C'"
check "不打印完整 broker URI" "! grep -qE 'ESP_LOG.*(->uri|mqtt_cfg.broker.address.uri)' '$C'"
# tls_enabled 定义了就必须被用，且用了就要挂信任锚 —— 否则每次 mqtts 握手都失败
check "tls_enabled 真的被消费"  "grep -q 'cfg->tls_enabled' '$C'"
check "TLS 挂了证书 bundle"     "grep -A4 'cfg->tls_enabled' '$C' | grep -q 'crt_bundle_attach'"

echo ""
echo "--- 看门狗接入 ---"
# 发布 task 必须喂狗，否则它卡死了没人知道
check "注册了看门狗槽位"   "grep -q 'wd_register(&wd_spec)' '$C'"
check "task 循环里喂狗"    "grep -q 'wd_kick(s_wd_slot_mqtt_pub)' '$C'"
check "注册了恢复回调"     "grep -q 'wd_register_recovery(\"mqtt\"' '$C'"

echo ""
echo ""
echo "--- T2.16 network_recovered（有分支必须也有订阅）---"
check "发布 event/network_recovered"      "grep -q 'event/network_recovered' bitengine_mqtt.c"
check "订阅了 NETWORK_EVENT_RECOVERED"     "grep -A1 'instance_register(NETWORK_EVENT, NETWORK_EVENT_RECOVERED' bitengine_mqtt.c | grep -q system_event_handler"

echo ""
echo "--- T2.19 切换时按新网口重建 + 连上补发（2026-09-13 真板三次拔线验出）---"
check "check_t219.py（分支置位+重启、不直接 publish、CONNECTED 补发、关着时清位）" \
    "python3 '$DIR/check_t219.py'"

echo ""
echo "--- T9.21 上线事件也要跟网口（2026-10-08；WiFi 先上线、网线后到时只有 ONLINE，没有 FAILOVER / RECOVERED）---"
check "订阅了 NETWORK_EVENT_ONLINE" \
    "grep -A1 'instance_register(NETWORK_EVENT, NETWORK_EVENT_ONLINE' bitengine_mqtt.c | grep -q system_event_handler"
check "ONLINE 只置位，不补发 MQTT 事件（不是告警）" \
    "grep -A2 'id == NETWORK_EVENT_ONLINE) {' bitengine_mqtt.c | grep -q 's_iface_check_requested = true;' && ! grep -A2 'id == NETWORK_EVENT_ONLINE) {' bitengine_mqtt.c | grep -q 's_pending_net_events'"
check "mqtt_task 比较活动网口与绑定的网口，不一样才重建；建客户端时记下绑定的网口" \
    "grep -q 'strcmp(now, s_bound_if) != 0' bitengine_mqtt.c && grep -q 'strlcpy(s_bound_if, ifname, sizeof(s_bound_if));' bitengine_mqtt.c"

check "MQTT 事件只认当前客户端（重建时被拆的旧客户端晚到的 CONNECTED 不算）" \
    "awk '/^static void mqtt_event_handler\(/,/^}/' bitengine_mqtt.c | grep -q 'if (!ev || ev->client != s_client) return;'"

check "T9.38：开机推迟建客户端 —— mqtt_client_init 里不建，mqtt_task 等 MQTT_BOOT_DEFER_S 后再建" \
    "awk '/^esp_err_t mqtt_client_init\(/,/^}/' bitengine_mqtt.c | python3 ../../tools/grep_code.py --absent -q 'mqtt_start_client\(' /dev/stdin && awk '/^static void mqtt_task\(/,/^}/' bitengine_mqtt.c | grep -q 'MQTT_BOOT_DEFER_S - esp_timer_get_time()'"

echo ""
echo "--- T9.39：开着 MQTT 不再吃掉 ~8 KB 内部 SRAM ---"
# mqtt_task 的 6 KB 栈经构建期补丁放 PSRAM；收发缓冲区靠 > ALWAYSINTERNAL 自己去 PSRAM。
check "构建期打 esp-mqtt 补丁，失败即停" \
    "grep -q 'tools/patch_mqtt.py' ../../CMakeLists.txt && grep -A12 'tools/patch_mqtt.py' ../../CMakeLists.txt | grep -q 'FATAL_ERROR'"
check "补丁已作用在组件上（组件还没下载时跳过）" \
    "[ ! -d ../../managed_components/espressif__mqtt ] || python3 ../../tools/patch_mqtt.py --check --component-dir ../../managed_components/espressif__mqtt"
check "收发缓冲区 > ALWAYSINTERNAL（否则 2 KB 落内部）" \
    "python3 -c \"
import re,sys
s=open('bitengine_mqtt.c',encoding='utf-8').read()
c=open('../../sdkconfig.defaults',encoding='utf-8').read()
lim=int(re.search(r'CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=(\\d+)',c).group(1))
v=[int(x) for x in re.findall(r'\\.buffer\\.(?:out_)?size = (\\d+)',s)]
sys.exit(0 if len(v)==2 and min(v)>lim else 1)\""
# PSRAM 栈的前提：在 mqtt_task 里跑的东西（esp-mqtt 与本文件的事件回调）不碰 flash / NVS（psram_task.h 前提 1）
check "本文件不碰 flash / NVS（mqtt_task 与 mqtt_pub 都是 PSRAM 栈）" \
    "python3 ../../tools/grep_code.py --absent -q 'nvs_[a-z_]+\\(|esp_partition_(write|erase)|spi_flash_(write|erase)|config_mgr_set_' bitengine_mqtt.c"

echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
