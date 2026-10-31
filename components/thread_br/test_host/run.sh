#!/bin/bash
# srp_readvertise 主机侧单元测试 —— 不需要板子，不需要 ESP-IDF，只要 gcc。
#
#   bash components/thread_br/test_host/run.sh
#   SRP_TEST_VERBOSE=1 bash .../run.sh     # 连被测代码的日志一起打出来
#
# 覆盖什么：只覆盖**纯逻辑**——名字解析、DNS-SD TXT 解析、跟踪表、缓冲区分配、
# 时序常数。这些是本模块唯一能脱离硬件判定对错的部分，其中 parse_txt 与
# split_service_name 直接吃来自空口的 SRP 注册内容，按 S1 规则必须当恶意输入测。
#
# 不覆盖什么（也不要试图在这里覆盖）：
#   * bye 是否真的上线 —— 取决于 mdns 组件内部 PCB 状态机，只能在板抓包
#   * 控制器认不认这次 Added —— 取决于对端实现
#   * SRP 注册表遍历 —— 需要真 OT 实例与真设备
# 这三项见 docs/test_cases_srp_readvertise.md 的 TC-S01~S04。
#
# ⚠️ 今天真正咬人的那个 bug（bye 落在 PCB 探测窗口里被静默吞掉）本套测试抓不到，
# 它是靠上板 + tcpdump 发现的。这里锁的是另一类失误：畸形输入、重构回归、
# 有人手滑把时序常数调小。别把这套测试当成在板验证的替代品。
#
#   parse_test.c   first_label / split_service_name / parse_txt 的边界与畸形输入
#   track_test.c   跟踪表增删查、表满、槽位复用、PSRAM 分配与回退、时序常数

set -e
cd "$(dirname "$0")"

CC=${CC:-gcc}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

FLAGS="-Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function -O1 -g"
# 被测代码里的 CONFIG_* 由 Kconfig 生成，主机上手工给等价值
DEFS="-DCONFIG_OPENTHREAD_ENABLED=1 -DCONFIG_BITENGINE_SRP_READVERTISE=1"
DEFS="$DEFS -DCONFIG_BITENGINE_SRP_READV_BYE_GAP_MS=2000 -DCONFIG_MDNS_MAX_SERVICES=32"
INC="-I stub -I ../include -I ."
[ -n "${SRP_TEST_VERBOSE:-}" ] && FLAGS="$FLAGS -DSRP_TEST_VERBOSE"

FAIL=0
# 两种构型各编一遍。强制 bye 于 2026-08-15 改为可选且默认关（487386d），
# 它带着 srp_readvertise.c 里 70 余行分支一起进了 #if；只按默认构型编的话，
# 那些行连语法错误都发现不了 —— 而"默认关"恰恰意味着开的那条路没人走过。
for BYE in 0 1; do
    echo "########## CONFIG_BITENGINE_SRP_READV_FORCE_BYE=$BYE ##########"
    for t in parse track; do
        echo "=== $t ==="
        $CC $FLAGS $DEFS -DCONFIG_BITENGINE_SRP_READV_FORCE_BYE=$BYE \
            $INC -o "$OUT/$t" "${t}_test.c"
        "$OUT/$t" || FAIL=1
        echo
    done
done

# channel 用**真** thread_br.h（要 COEX_TOO_CLOSE_MAX / coex_status_t），所以
# ../include 必须排在 stub 前面 —— 上面那个 INC 是反过来的，srp 那两个测试靠
# stub/thread_br.h 顶掉真头文件。另外要 ezsp_bridge 的 ncp_version.h。
# 编的是真源码 thread_br_channel.c，不是副本。
echo "=== channel ==="
$CC $FLAGS $DEFS -I ../include -I ../../config_mgr/include -I stub -I . \
    -o "$OUT/channel" channel_test.c ../thread_br_channel.c
"$OUT/channel" || FAIL=1
echo

if [ $FAIL -ne 0 ]; then
    echo "❌ 主机侧测试失败"
    exit 1
fi
echo "✅ 主机侧测试全部通过"
