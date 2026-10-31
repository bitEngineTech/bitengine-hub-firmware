#!/bin/bash
# components/webhook/VERIFY.sh —— 编译通过后运行：bash components/webhook/VERIFY.sh
#
# ⚠️ 这不是一份完整的模块校验。它是 2026-08-16 修「关掉开关仍会发」那个 bug 时
# 临时建起来的，webhook 此前**根本没有 VERIFY.sh**（mqtt_client / scheduler /
# mdns_svc / matter_bridge 至今也没有）。本文件目前只锁事件门控与几条资源纪律，
# 设计文档 docs/07 第五节里的其余约束尚未转成断言。要补的人从那份文档往下抄。
#
# 另：形状对不代表行为对（CLAUDE.md 7.5）。「关掉开关真的收不到」这件事
# 只有 tools/test_api.sh 那条端到端断言能证明。

PASS=0; FAIL=0
check() {
    if eval "$2" > /dev/null 2>&1; then
        echo "✅ $1"; ((PASS++))
    else
        echo "❌ $1"; ((FAIL++))
    fi
}

cd "$(dirname "$0")"

echo "--- 事件门控（2026-08-16）---"
# 过滤原先散在 system_event_handler 的每个 if 里，于是绕开那个处理器的两条路
# （spectrum 的干扰告警、scheduler 的定时备份）根本不过滤 —— 用户关掉「干扰」
# 开关照样收得到 interference。设计文档里写的 is_event_enabled() 当时并不存在。
check "存在 is_event_enabled"        "grep -q 'static bool is_event_enabled' webhook.c"
check "门控在 post 这一层"           "awk '/^void webhook_post_event/,/^}/' webhook.c | grep -q 'is_event_enabled'"
check "干扰事件受 event_interference 管" \
      "awk '/^static bool is_event_enabled/,/^}/' webhook.c | grep -q 'WEBHOOK_EVENT_INTERFERENCE:.*event_interference'"
check "未登记的枚举默认不发"          "awk '/^static bool is_event_enabled/,/^}/' webhook.c | grep -q 'return false;'"
# 两处都判 = 两份真源，正是当初漏掉那两条路的原因
check "handler 里不再重复判开关"      "! awk '/^static void system_event_handler/,/^}/' webhook.c | grep -q 'cfg->event_'"
# 主开关：post 与 worker 各有一道，队列里已有的项在配置改动后也要被丢弃
check "post 检查主开关"              "awk '/^void webhook_post_event/,/^}/' webhook.c | grep -q 'cfg->enabled'"
check "worker 也检查主开关"           "awk '/^static void webhook_task/,/^}/' webhook.c | grep -q 'cfg->enabled'"

echo ""
echo "--- 资源与安全 ---"
check "URL 限定 http(s)"             "grep -rq 'http://\|https://' ../webui/rest_api.c"
check "HTTP client 有 cleanup"        "[ \$(grep -c 'esp_http_client_init' webhook.c) -le \$(grep -c 'esp_http_client_cleanup' webhook.c) ]"
check "init 幂等不泄漏"               "awk '/^esp_err_t webhook_init/,/^}/' webhook.c | grep -q 'if (s_queue) return ESP_OK'"
check "队列满不阻塞"                  "awk '/^void webhook_post_event/,/^}/' webhook.c | grep -q 'xQueueSend(s_queue, &item, 0)'"
# 每个用户可见事件都得有生产者。OTA_AVAILABLE 定义了一个月零 post（feature_gap B9/C1），
# 用户开了开关等不到任何东西。2026-09-13 接到 ota_cloud_check()。
check "OTA_AVAILABLE 有生产者"        "grep -q 'webhook_post_event(WEBHOOK_EVENT_OTA_AVAILABLE' ../ota_cloud/ota_cloud.c"
check "worker 喂狗"                   "awk '/^static void webhook_task/,/^}/' webhook.c | grep -q 'wd_kick'"
check "日志不打印 URL"                "! grep -q 'ESP_LOG.*cfg->url' webhook.c"

echo ""
echo ""
echo "--- T3.7 自动备份发真数据 ---"
# 以前发的是一句占位符（只有 hostname + "去调 /config/export"），因为队列项的
# data 只有 128 字节。那样这个"自动备份"是个空壳 —— 收到通知的人还得自己再拉一次。
check "备份在发送时生成完整配置" "grep -q 'config_mgr_export_json' webhook.c"
check "scheduler 不再拼占位摘要"  "! grep -q 'use /api/v1/config/export for full backup' ../scheduler/scheduler.c"
check "body 按需分配而非定长"     "! grep -q 'char body\\[384\\]' webhook.c"
# ⚠️ 每条 continue 路径都必须释放，否则这个每分钟跑一次的任务会一直积
check "导出结果有释放"            "grep -q 'free(backup_json)' webhook.c"
check "body 有释放"               "grep -q 'free(body)' webhook.c"
check "释放次数覆盖所有退出路径"  "python3 ./check_free_paths.py"

echo ""
echo "--- T3.8 出口网卡绑定 ---"
# 双上行且两口同网段时，lwip 按 netif 链表顺序选出口，不看默认路由 ——
# 对端看到的来源 IP 是 WiFi 那个，哪怕 active_interface=eth。
check "webhook 绑定出口网卡"      "grep -q 'if_name = ifname\\[0\\]' webhook.c"
check "取名字走 network_mgr"       "grep -q 'network_mgr_active_ifname' webhook.c"
# 取不到名字时必须退回"不指定"，而不是发不出去
check "取不到名字时仍发送"        "grep -q 'ifname\\[0\\] ? &ifr : NULL' webhook.c"
check "MQTT 也绑了"               "grep -q 'network.if_name' ../mqtt_client/bitengine_mqtt.c"
# ⚠️ 云端 OTA **故意不绑**：目的地在公网、跨网段本就按默认路由走，
#    绑死反而挡掉"这个口没外网、另一个有"的回退。
# ⚠️ 用 grep_code.py：那段注释本身就在解释"为什么不设 .if_name"。
check "云端 OTA 没有真的绑出口"   "python3 ../../tools/grep_code.py --absent -q '\\.if_name' ../ota_cloud/ota_cloud.c"
check "云端 OTA 写明了为什么不绑" "grep -q '故意不设' ../ota_cloud/ota_cloud.c"

echo ""
echo "--- ota_complete 真的发得出去（2026-09-13 云端真发版才发现 T2.7 仍是死的）---"
# esp_event 不回放：OTA_VERIFIED 若在 webhook_init() 之前 post，当场分发、无人接收。
# 判据只认代码行（注释里也提到 OTA_VERIFIED，所以匹配带 esp_event_post( 前缀的那一行）。
MAIN=../../main/main.c
L_INIT=$(grep -n '^\s*webhook_init();' $MAIN | head -1 | cut -d: -f1)
# 只看 app_main 里那一处：直连型的 direct_verify_task 定义在 app_main 前面，它那处的顺序由
# webui/check_ota_order.py 管（等 BOOT_SUBSCRIBERS_READY_BIT 再发）。
L_AM=$(grep -n '^void app_main(void)' $MAIN | head -1 | cut -d: -f1)
L_POST=$(awk -v am="${L_AM:-999999}" 'NR > am && /esp_event_post\(WEBUI_EVENT, WEBUI_EVENT_OTA_VERIFIED/ { print NR; exit }' $MAIN)
check "main.c 仍然 post OTA_VERIFIED"          "[ -n \"$L_POST\" ]"
check "OTA_VERIFIED 在 webhook_init() 之后 post" "[ -n \"$L_INIT\" ] && [ -n \"$L_POST\" ] && [ \"$L_POST\" -gt \"$L_INIT\" ]"
check "webhook 订的是 OTA_VERIFIED 不是 OTA_COMPLETE" "grep -q 'id == WEBUI_EVENT_ID_OTA_VERIFIED' webhook.c && ! grep -q 'id == WEBUI_EVENT_ID_OTA_COMPLETE' webhook.c"

echo ""
echo "--- T2.16 network_recovered ---"
check "订了 NETWORK_EVENT_RECOVERED"        "grep -q 'NETWORK_EVENT_RECOVERED' webhook.c"
check "network_recovered 有名字"            "grep -q '\"network_recovered\"' webhook.c"
check "network_recovered 与 network_down 同开关" "grep -A2 'case WEBHOOK_EVENT_NETWORK_FAILOVER:' webhook.c | grep -q 'NETWORK_RECOVERED' && grep -A2 'case WEBHOOK_EVENT_NETWORK_FAILOVER:' webhook.c | grep -q 'event_network_down'"

echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
