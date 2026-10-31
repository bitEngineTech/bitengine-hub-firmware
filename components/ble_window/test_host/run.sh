#!/bin/bash
# 主机侧测试 —— 不需要板子、不需要 ESP-IDF，只要 gcc。
#
#   bash components/ble_window/test_host/run.sh
#
# blew_core.c 决定"这次开机进不进配对剖面"和"开窗口的请求接不接"。判错的后果：断电后卡在配对剖面里出不来
# （TC-MU04），或者 panic 之后误进配对剖面 —— 板子上都要拆机看串口才能归因，所以先在这里证明。编的是产品源文件本身。
set -e
cd "$(dirname "$0")"
CC=${CC:-gcc}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
$CC -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I.. -o "$OUT/blew_core_test" blew_core_test.c ../blew_core.c
"$OUT/blew_core_test"
