#!/bin/bash
# 主机侧协议测试 —— 不需要板子，不需要 ESP-IDF，只要 gcc。
#
#   bash components/ezsp_bridge/test_host/run.sh
#
# 为什么存在：ncp_version.c 写的是 ASH/EZSP 的**字节级**协议，一个 CRC 位序或
# 一个字段宽度写错，在板子上的表现只是"版本读不到"，没有任何线索指向根因。
# 这里把它编到主机上，对着一个用 SDK 原版 CRC/LFSR 搭的模拟 NCP 跑通全流程。
#
#   ash_selftest.c  组帧原语 vs SDK 原版逐位比对（CRC / LFSR / 转义 / RST 帧）
#   ncp_sim_test.c  真实的 ncp_version.c 对模拟 MG24 跑完整会话 + 故障注入
#
# 改了 ncp_version.c 就跑一遍这个，比烧板子快得多。

set -e
cd "$(dirname "$0")"

CC=${CC:-gcc}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

FLAGS="-Wall -Wextra -Wno-unused-parameter -O1"

echo "=== 1/2 组帧原语差分测试 ==="
$CC $FLAGS -o "$OUT/ash_selftest" ash_selftest.c
"$OUT/ash_selftest"

echo
echo "=== 2/2 模拟 NCP 会话测试 ==="
$CC $FLAGS -o "$OUT/ncp_sim_test" \
    -I stub -I .. -I ../include \
    ncp_sim_test.c ../ncp_version.c
"$OUT/ncp_sim_test"
