#!/bin/bash
# 主机侧测试：T9.20 开机接管服务 IP 的判据（../svc_policy.c）—— 不需要板子、不需要 ESP-IDF，只要 gcc。
#
#   bash components/network_mgr/test_host/run.sh
#
# 为什么存在：svc_policy 决定"网关会不会去占一个可能已经属于别人的 IP"。该接管的不接管 =
# ZHA 断到网线插回；不该接管的接管了 = 局域网 IP 冲突，坏的是别人的设备。两种在板子上都
# 极难复现（要真等租期过期、真造一台冒名设备），所以判据全在这里用例化。
#
# 编的是**产品源文件本身**（../svc_policy.c），不是副本。

set -e
cd "$(dirname "$0")"

CC=${CC:-gcc}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

$CC -std=c11 -Wall -Wextra -Werror -I.. -o "$OUT/svc_policy_test" svc_policy_test.c ../svc_policy.c
"$OUT/svc_policy_test"
