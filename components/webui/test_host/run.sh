#!/bin/bash
# 主机侧测试 —— 不需要板子、不需要 ESP-IDF，只要 gcc + python3。
#   1. 跨站守卫（cross_origin_test.c）
#   2. 外来字符串进 JSON 的三种拷贝都输出合法 UTF-8（json_escape_test.c + json_escape_check.py，T9.24 / T9.32）
#
#   bash components/webui/test_host/run.sh
#
# ⚠️ 上板之前必须全绿。理由见 cross_origin_test.c 顶部：本板无串口、无 BOOT 键，
#    而 /api/v1/ota/esp32 自己也挂着这个守卫 —— 守卫一旦误伤 curl，就既推不上
#    新固件、也没法串口救砖。
#
# 编的是**产品源文件本身**（../cross_origin.c），不是副本。
# 做法：先造一组**空的**同名头文件放进 shim 目录并优先 -I，让 cross_origin.c 的
# #include 全部落空；它需要的类型和桩全部由 cross_origin_test.c 在 #include 之前
# 自己定义好。这样产品代码一个字都不用为测试改动。
set -e
cd "$(dirname "$0")"
CC=${CC:-gcc}
OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT
mkdir -p "$OUT/shim"
for h in webui.h config_mgr.h esp_log.h; do : > "$OUT/shim/$h"; done
$CC -std=c11 -Wall -Wextra -Wno-unused-function \
    -I"$OUT/shim" -o "$OUT/t" cross_origin_test.c
"$OUT/t"

echo ""
echo "══ 外来字符串进 JSON：转义 / 剥离 / 只修 UTF-8 + utf8_valid 判据（T9.24 / T9.32 / T9.33）══"
# 编产品源文件本身（json_escape_test.c 里 #include "../json_escape.c" 与 "../../utf8/utf8.c"）。
# 对照部分交给 Python：它的 UTF-8 解码器是一份独立实现，比自己再写一份参考可信。
$CC -std=c11 -Wall -Wextra -Werror -I../../utf8/include -o "$OUT/je" json_escape_test.c
"$OUT/je" "$OUT/je_cases.txt"
python3 json_escape_check.py "$OUT/je_cases.txt"
