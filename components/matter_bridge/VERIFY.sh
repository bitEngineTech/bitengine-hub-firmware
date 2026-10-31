#!/bin/bash
# components/matter_bridge/VERIFY.sh —— 空壳模块的机器判据
#
#   cd components/matter_bridge && bash VERIFY.sh
#
# ── 这个模块是空壳，VERIFY 要守的是"别假装能用"（T2.9）──────────────
#
# 85 行，Matter SDK 一行都没链进来。四个动作类 API 全部返回 ESP_ERR_NOT_SUPPORTED。
# 这种模块最危险的不是"没实现"，而是哪天有人把某个 return 改成 ESP_OK 让它
# "看起来通了" —— 调用方于是以为桥起来了，而实际什么都没发生。
#
# 所以下面全是反向不变量：**在 SDK 真的接进来之前，这些必须保持不能用。**
# 真做 Matter 时，这份 VERIFY 会整体失效，那时按真实实现重写它，而不是删掉它。
#
# 第二条守的是 start() 的失败回滚：它会先把 Zigbee 桥关掉再去做 Matter 的事，
# 失败时若不还原，用户的 Z2M/ZHA 连接就被丢在 DISABLED 上 —— 一个"没做成的功能"
# 顺手打断了正在用的功能。stop() 那边一直是对的，start() 的错误路径曾经漏了。

PASS=0; FAIL=0
DIR="$(cd "$(dirname "$0")" && pwd)"
C="$DIR/matter_bridge.c"
H="$DIR/include/matter_bridge.h"

check() {
    if eval "$2" > /dev/null 2>&1; then echo "✅ $1"; PASS=$((PASS+1))
    else echo "❌ $1"; FAIL=$((FAIL+1)); fi
}

echo "--- API 完整性 ---"
for fn in matter_bridge_init matter_bridge_start matter_bridge_stop \
          matter_bridge_add_device matter_bridge_remove_device; do
    check "$fn 已声明"  "grep -q '$fn' '$H'"
    check "$fn 已实现"  "grep -qE '^esp_err_t +$fn\\(' '$C'"
done

echo ""
echo "--- ⚠️ 空壳不许假装能用 ---"
# 四个**动作类** API 必须仍返回 NOT_SUPPORTED。init 例外：它只打一行日志，
# 返回 ESP_OK 是对的（main.c:351 无条件调它，返回错误会污染启动日志）。
# 只在私有检出里有的内部文档 / 工具（docs/、发布与产线脚本）：公开仓库不导出它们（tools/oss_export.py），
# 那里跳过而不是报红；私有检出里照常判。标志是 docs/开发索引.md 在不在。
internal() { if [ -f ../../docs/开发索引.md ]; then check "$@"; else echo "⏭  $1 —— 依赖内部文档 / 工具，本检出没有，跳过"; fi; }

check "start 仍返回 NOT_SUPPORTED" \
    "python3 '$DIR/check_stub.py' matter_bridge_start"
check "add_device 仍返回 NOT_SUPPORTED" \
    "python3 '$DIR/check_stub.py' matter_bridge_add_device"
check "remove_device 仍返回 NOT_SUPPORTED" \
    "python3 '$DIR/check_stub.py' matter_bridge_remove_device"
# SDK 没链进来 —— 出现这些符号就说明状态变了，这份 VERIFY 该整体重写
check "确实没链 Matter SDK" \
    "! grep -qE 'esp_matter|chip::|CHIP_ERROR' '$C'"
check "CMakeLists 没依赖 Matter 组件" \
    "! grep -qiE 'esp_matter|connectedhomeip' '$DIR/CMakeLists.txt'"
check "文件规模仍是空壳级（< 200 行）" \
    "[ \$(wc -l < '$C') -lt 200 ]"

echo ""
echo "--- ⚠️ start 失败必须把 Zigbee 桥还原回去 ---"
# start() 第一步就 ezsp_bridge_switch_mode(BRIDGE_MODE_DISABLED)。
# 失败路径若不还原，用户的 Z2M/ZHA 就被丢在 DISABLED 上 —— 一个没做成的功能
# 顺手打断了正在用的功能。
check "start 里先存了原模式"       "grep -A4 'matter_bridge_start' '$C' | grep -q 's_prev_mode = ezsp_bridge_get_mode'"
check "start 的失败路径有还原"     "python3 '$DIR/check_rollback.py'"
check "stop 还原成存下来的模式而非写死 LAN" \
    "grep -A6 'esp_err_t matter_bridge_stop' '$C' | grep -q 'switch_mode(s_prev_mode)'"
check "不把 BRIDGE_MODE_LAN 写死进还原" \
    "! grep -qE 'switch_mode\\(BRIDGE_MODE_LAN\\)' '$C'"

echo ""
echo "--- 对外口径 ---"
# 文档必须说明它是空壳，别让人以为能用
internal "docs/00 写明它是空壳" \
    "grep -qE 'matter_bridge.*(空壳|未实现|85 行)' '$DIR/../../docs/00_产品功能现状.md'"

echo ""
echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
