#!/bin/bash
# noise_core 主机侧测试 —— 上游 muse-gadget-sdk 的 noise_core 测试原样跑在我们引入的组件上（docs/15 设计附录 C）。
#
#   bash components/noise_core/test_host/run.sh
#
# 上游 test_noise_core.py 要主机上的 PSA Crypto 开发库（pkg-config mbedcrypto 或 -lmbedcrypto），本机没有（只有运行库、没有头文件，
# 也没有 pkg-config）—— 缺了它上游测试会**跳过**。这里在临时目录用 IDF 自带的 tf-psa-crypto 编出来（同 muse_pair/test_host/run.sh），
# 把几个归档合成一个 libmbedcrypto.a，经 CXX 环境变量带上 -I / -L（上游脚本把 CXX 按 shell 拆词，允许带参数）。上游测试一行不改。
# 覆盖：test_noise_core（握手 + 会话 + 传输帧编解码，带 PSA 后端）、test_noise_core_inplace（原地加密与拷贝路径一致、会话原地封装）。
# 本仓库自加：test_noise_interop（固件的发起方 + Transport 对 Linux SDK 的 Python 响应方走完 Noise XX 并互发密文）。上游测试从没
# 拿发起方和别的实现握过手，协议名改坏照样全绿（2026-10-22 变异）；对端 linux_sdk/noise_xx.py 是上游原样，要 python3-cryptography。
# 依赖 noise_control（会话层）的上游测试不在这里：那层在 M3 改写成 muse_link/muse_session.cpp，到时一起移植。
# 需要：gcc/g++、cmake、python3（带 cryptography）、ESP-IDF v6.0.2 源码（只用它的 mbedtls）。
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
COMP="$(cd "$HERE/.." && pwd)"
IDF_PATH="${IDF_PATH:-$HOME/esp-idf-v6.0.2}"
# 只认 v6.0.2：环境里若 source 过 ~/esp-idf（master 快照），不核版本就会悄悄拿 master 的 mbedtls 来测（2026-10-22 审计）
idf_ver="$(sed -nE 's/^set\(IDF_VERSION_(MAJOR|MINOR|PATCH) ([0-9]+)\)$/\2/p' "$IDF_PATH/tools/cmake/version.cmake" 2>/dev/null | paste -sd.)"
[ "$idf_ver" = "6.0.2" ] || { echo "❌ IDF_PATH=$IDF_PATH 是 ESP-IDF '${idf_ver:-?}'，这里只认 v6.0.2（固件的版本）"; exit 1; }
MBEDTLS="$IDF_PATH/components/mbedtls/mbedtls"
CRYPTO="$MBEDTLS/tf-psa-crypto"

T="$(mktemp -d /tmp/noise-core-test.XXXXXX)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/components" "$T/tests" "$T/lib"
ln -s "$COMP" "$T/components/noise_core"
U="$HERE/upstream_tests"
for f in "$U"/*.cpp; do ln -s "$f" "$T/tests/$(basename "$f")"; done
cp "$U"/test_*.py "$T/tests/"
ln -s "$HERE/noise_interop_harness.cpp" "$T/tests/noise_interop_harness.cpp"
cp "$HERE/test_noise_interop.py" "$HERE/linux_sdk/noise_xx.py" "$T/tests/"
touch "$T/tests/__init__.py"

echo "== 编 IDF 的 tf-psa-crypto（主机，纯软件实现）"
# IDF 把 mbedtls/bignum.h、ecp.h 挪进了 mbedtls/private/：同上游 test_link_pairing_handshake.py 的 compat 做法，只映射到软件实现
mkdir -p "$T/compat/mbedtls"
for n in bignum ecp; do
    printf '#pragma once\n#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS\n#include "mbedtls/private/%s.h"\n' "$n" > "$T/compat/mbedtls/$n.h"
done
: > "$T/compat/sdkconfig.h"
cmake -S "$MBEDTLS" -B "$T/crypto-build" -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DGEN_FILES=OFF \
      -DMBEDTLS_FATAL_WARNINGS=OFF -DDISABLE_PACKAGE_CONFIG_AND_INSTALL=ON -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=gcc "-DCMAKE_C_FLAGS=-I$T/compat" > "$T/cmake.log" 2>&1 || { tail -20 "$T/cmake.log"; exit 1; }
cmake --build "$T/crypto-build" --target tfpsacrypto -j 8 > "$T/build.log" 2>&1 || { tail -20 "$T/build.log"; exit 1; }
B="$T/crypto-build/tf-psa-crypto"
# 几个归档合成一个 libmbedcrypto.a（上游测试链接时只给 -lmbedcrypto）
{
    echo "create $T/lib/libmbedcrypto.a"
    for a in "$B"/core/libtfpsacrypto.a "$B"/drivers/*/lib*.a; do echo "addlib $a"; done
    echo "save"; echo "end"
} | ar -M
export CXX="g++ -I$T/compat -I$CRYPTO/include -I$CRYPTO/drivers/builtin/include -I$B/include -L$T/lib"

cd "$T"
set +e
# 套 timeout：互通测试读 harness 输出是阻塞的，harness 卡住时别把 build_profiles.sh 一起挂死（2026-10-22 审计）
timeout 600 python3 -m unittest -v tests.test_noise_core tests.test_noise_core_inplace tests.test_noise_interop > "$T/out.txt" 2>&1
rc=$?
set -e
grep -E "^test_|ok$|FAIL|ERROR|skipped|^Ran|^OK|^FAILED" "$T/out.txt" || true
# 用例数也钉住：哪条用例被改名不再收集，"全部通过"就是假的（上游 3 + 自加 3）
if [ $rc -eq 0 ] && grep -qE "^OK" "$T/out.txt" && ! grep -qE "skipped" "$T/out.txt" && grep -qE "^Ran 6 tests" "$T/out.txt"; then
    echo "全部通过"
else
    echo "❌ 有失败、跳过或少跑的用例（unittest 退出码 $rc，124 = 超时；跳过也算不过：缺 PSA 头文件时上游测试会静默跳过；应跑 6 条）"
    tail -30 "$T/out.txt"
    exit 1
fi
