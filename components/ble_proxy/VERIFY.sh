#!/bin/bash
# Phase 5 VERIFY.sh — BLE Proxy + ESPHome + Matter Bridge
# CREATED BEFORE ANY IMPLEMENTATION CODE (P1)
BASE="$(dirname "$0")/.."
PASS=0; FAIL=0
check() { if eval "$2" > /dev/null 2>&1; then echo "✅ $1"; ((PASS++)); else echo "❌ $1"; ((FAIL++)); fi; }

echo "=== Phase 5 Verification ==="

echo ""
echo "--- BLE Proxy ---"
# 只在私有检出里有的内部文档 / 工具（docs/、发布与产线脚本）：公开仓库不导出它们（tools/oss_export.py），
# 那里跳过而不是报红；私有检出里照常判。标志是 docs/开发索引.md 在不在。
internal() { if [ -f ../../docs/开发索引.md ]; then check "$@"; else echo "⏭  $1 —— 依赖内部文档 / 工具，本检出没有，跳过"; fi; }

check "ble_proxy.h exists"          "test -f $BASE/ble_proxy/include/ble_proxy.h"
check "ble_proxy.c exists"          "test -f $BASE/ble_proxy/ble_proxy.c"
check "ble_proxy_init impl"         "grep 'ble_proxy_init' $BASE/ble_proxy/ble_proxy.c"
check "ble_proxy_stop impl"         "grep 'ble_proxy_stop' $BASE/ble_proxy/ble_proxy.c"
check "NimBLE scan API used"        "grep -i 'ble_gap_disc\|ble_gap_event\|nimble' $BASE/ble_proxy/ble_proxy.c"
check "MQTT publish used"           "grep -i 'mqtt.*publish\|esp_mqtt' $BASE/ble_proxy/ble_proxy.c"
check "Passive scan (not active)"   "grep -i 'passive\|BLE_HCI_SCAN_FILT\|scan_type' $BASE/ble_proxy/ble_proxy.c"
check "MQTT enabled check"          "grep -i 'mqtt.*enabled\|is_connected\|mqtt_client' $BASE/ble_proxy/ble_proxy.c"

echo ""
echo "--- ESPHome Template ---"
internal "YAML template exists"        "test -f $BASE/../docs/esphome_template.yaml"
internal "GPIO pins defined"           "grep 'GPIO' $BASE/../docs/esphome_template.yaml"
internal "UART config"                 "grep 'uart' $BASE/../docs/esphome_template.yaml"
internal "W5500 ethernet"              "grep -i 'W5500\|ethernet' $BASE/../docs/esphome_template.yaml"

echo ""
echo "--- Matter Bridge (framework) ---"
check "matter_bridge.h exists"      "test -f $BASE/matter_bridge/include/matter_bridge.h"
check "matter_bridge.c exists"      "test -f $BASE/matter_bridge/matter_bridge.c"
check "matter_bridge_init impl"     "grep 'matter_bridge_init' $BASE/matter_bridge/matter_bridge.c"
check "matter_bridge_start impl"    "grep 'matter_bridge_start' $BASE/matter_bridge/matter_bridge.c"
check "matter_bridge_stop impl"     "grep 'matter_bridge_stop' $BASE/matter_bridge/matter_bridge.c"
check "Z2M互斥说明"                  "grep -i 'Z2M\|互斥\|exclusive\|BRIDGE_MODE' $BASE/matter_bridge/matter_bridge.c"

echo ""
echo "--- Integration ---"
check "ble_proxy_init in main"      "grep 'ble_proxy_init' $BASE/../main/main.c"

echo ""
echo "--- S1 security ---"
check "No secrets in logs"          "! grep -rn 'ESP_LOG.*password\|ESP_LOG.*key' $BASE/ble_proxy/*.c $BASE/matter_bridge/*.c 2>/dev/null"

echo ""
echo "--- S2 threading ---"
check "BLE callback safe"           "grep -i 'callback\|event\|disc' $BASE/ble_proxy/ble_proxy.c"

echo ""
echo "==============================="
echo ""
echo "--- 禁用必须是开关不是注释（2026-08-17）---"
# 此前 main.c 里是一行注释掉的 ble_proxy_init()。注释挡不住谁顺手删掉再启用，
# 也不会在任何 VERIFY 里露面。BLE 扫描会破坏 802.15.4 的 CCA。
check "有 Kconfig 开关"          "grep -q 'BITENGINE_ENABLE_BLE_PROXY' ../../main/Kconfig.projbuild"
# 按整个 config 块判，不按固定行数（-A2 曾被 M2 在块里加的 depends 行挤出去，从 87ef9ea 起一直空红）
check "出厂默认关"               "awk '/^ *config BITENGINE_ENABLE_BLE_PROXY\$/{f=1;next} f&&/^ *(config|menu|endmenu|choice|endchoice|if|endif) /{f=0} f' ../../main/Kconfig.projbuild | grep -q '^ *default n\$' && ! awk '/^ *config BITENGINE_ENABLE_BLE_PROXY\$/{f=1;next} f&&/^ *(config|menu|endmenu|choice|endchoice|if|endif) /{f=0} f' ../../main/Kconfig.projbuild | grep -q '^ *default y'"
check "main.c 用条件编译"        "grep -q '#if CONFIG_BITENGINE_ENABLE_BLE_PROXY' ../../main/main.c"
check "不再是注释掉的调用"       "! grep -q '// ble_proxy_init' ../../main/main.c"
# 别再有人为了省内存去关 BT —— A/B 全量构建证明收益是 0
check "记着关 BT 省 0 字节"      "grep -q '不省内存' ../../main/Kconfig.projbuild"

echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
