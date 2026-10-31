#!/bin/bash
# 主机侧版本比较测试 —— 不需要板子、不需要 ESP-IDF，只要 gcc。
#
#   bash components/ota_cloud/test_host/run.sh
#
# 为什么存在：ota_cloud_version_cmp() 是防降级那道闸的全部实现
# （install() 的判据就是 cmp(latest, current) > 0）。写错的表现是
# "该拦的没拦住"或"该升的升不了"，两种在板子上都极难归因 ——
# 前者要等到有人真推一个旧固件才暴露，后者会被当成网络问题。
#
# 编的是**产品源文件本身**（../version_cmp.c），不是副本。

set -e
cd "$(dirname "$0")"

CC=${CC:-gcc}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

$CC -std=c11 -Wall -Wextra -Werror -I.. -o "$OUT/version_test" version_test.c
"$OUT/version_test"

# utf8_safe_copy：notes 是唯一的自由文本字段，按字节截断会切坏多字节字符。
# 2026-09-15 线上真出过一次（见 ../utf8_trunc.c 的注释）。
$CC -std=c11 -Wall -Wextra -Werror -I.. -o "$OUT/utf8_test" utf8_test.c ../utf8_trunc.c
"$OUT/utf8_test"

# 防串刷（docs/15 设计 2.7、架构 5.8 R-9）：两种固件同一把签名钥匙，挡串刷的只有工程名比对。
# 写错 = 直连型装进 HA 型设备（没有 Wi-Fi，拆机才救得回，红线 1），所以先在这里证明再上板。
$CC -std=c11 -Wall -Wextra -Werror -I.. -o "$OUT/profile_test" profile_test.c ../ota_profile.c
"$OUT/profile_test"
