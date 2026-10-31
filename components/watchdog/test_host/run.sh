#!/bin/bash
# 看门狗主机侧单元测试 —— 不需要板子，不需要 ESP-IDF，只要 gcc。
#
#   bash components/watchdog/test_host/run.sh
#   WD_TEST_VERBOSE=1 bash .../run.sh    # 连被测代码的日志一起打出来
#
# 为什么存在：恢复引擎的状态机分支远多于能在板子上注入的场景——冷却窗口、
# fail_streak、策略封顶、3600 秒的自愈滑动窗口、32 位时间戳回绕。这些在板子上
# 验一条要等一个冷却周期甚至一小时，还往往伴随重启；这里时间是变量，秒级跑完。
#
# 最该被这套测试锁住的不变量：**MG24(PERIPH) 故障封顶 L2**。AM1 板上硬件复位
# 会带走整板且可能丢配网，策略一旦被手滑改掉，代价是用户重新配网。
#
#   registry_test.c   层 B 心跳注册表：deadline 边界、豁免、幂等、时间回绕
#   recovery_test.c   层 E 恢复引擎：升降级、冷却、封顶、安全模式
#   forensics_test.c  层 G 取证：自愈窗口数学、环形历史、NVS 损坏降级
#   probes_test.c     层 C 探针：Thread 停摆持续时定期再报（只报一次 = 永远升不过 L2）
#
# 在板部分见 docs/test_cases_watchdog.md（TC-W01~W10，需要故障注入固件）。

set -e
cd "$(dirname "$0")"

CC=${CC:-gcc}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

FLAGS="-Wall -Wextra -Werror -Wno-unused-parameter -O1"
INC="-I stub -I ../include -I . -include stub/compat.h"
[ -n "${WD_TEST_VERBOSE:-}" ] && FLAGS="$FLAGS -DWD_TEST_VERBOSE"

FAIL=0
for t in registry recovery forensics; do
    echo "=== $t ==="
    $CC $FLAGS $INC -o "$OUT/$t" "${t}_test.c"
    "$OUT/$t" || FAIL=1
    echo
done

# 注册表再按直连型编一次：槽数上限按配置分开（WD_MAX_TASKS 20 / HEAP_MARK_MAX 32，docs/15 设计第十五节 A-24）
echo "=== registry（直连型） ==="
$CC $FLAGS $INC -DCONFIG_BITENGINE_PROFILE_DIRECT=1 -o "$OUT/registry_direct" registry_test.c
"$OUT/registry_direct" || FAIL=1
echo

# 恢复引擎再按直连型编一次：应急重启的历史写明卡在哪一步（wd_recovery_doing，只在直连型）
echo "=== recovery（直连型） ==="
$CC $FLAGS $INC -DCONFIG_BITENGINE_PROFILE_DIRECT=1 -o "$OUT/recovery_direct" recovery_test.c
"$OUT/recovery_direct" || FAIL=1
echo

# 探针测试单独一种编法：wd_probes.c 作为独立编译单元与测试链接，测试里的强符号
# （thread_br_get_health / wd_report …）才能盖掉它的 weak 默认实现与真实引擎。
echo "=== probes ==="
$CC $FLAGS $INC -o "$OUT/probes" probes_test.c ../wd_probes.c
"$OUT/probes" || FAIL=1
echo

if [ $FAIL -ne 0 ]; then
    echo "❌ 主机侧测试失败"
    exit 1
fi
echo "✅ 主机侧测试全部通过"
