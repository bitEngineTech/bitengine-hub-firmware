#!/bin/bash
# muse_pair 主机侧测试 —— 上游 muse-gadget-sdk 的配对测试原样跑在我们引入的文件上（docs/15 设计附录 C）。
#
#   bash components/muse_pair/test_host/run.sh
#
# 上游测试脚本按它自己的目录结构找文件（ROOT/main/*.c、ROOT/tests/*、ROOT/managed_components/espressif__cjson）。
# 这里在临时目录里搭出同样的结构：源文件链到本组件，测试脚本复制过去（脚本用 Path(__file__).resolve() 定 ROOT，
# 链接会被解析回本目录，所以必须复制），然后原样跑。上游测试一行不改，跟进上游时直接替换 upstream_tests/。
#
# 覆盖：
#   test_link_pairing_handshake    真握手：主机上编 IDF 自带的 tf-psa-crypto（P-256 ECDH / HKDF / AES-GCM），
#                                  社区模式与厂商认证模式各跑一遍状态迁移（几秒，大头是编 crypto）
#   test_link_pairing_transcript   与 App 共享的 v5 测试向量（tests/vectors/link_pairing_v5.json）逐字节一致
#   test_link_pairing_signer_policy 签名者策略
#   test_kdf_vectors（本仓库自加）  固件自己的密钥派生 / AES-GCM 解密对上同一份 v5 向量 —— 上游这里是自洽测试，
#                                  固件派生改错了照样全绿（见 kdf_vectors_harness.c 文件头）
#   pair_core_test（本仓库自加）     胶水层的纯判据（编产品源文件 pair_core.c）
# 需要：gcc、cmake、python3（含 cryptography）、ESP-IDF v6.0.2 源码（只用它的 mbedtls）。
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
COMP="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$COMP/../.." && pwd)"
export IDF_PATH="${IDF_PATH:-$HOME/esp-idf-v6.0.2}"
# 只认 v6.0.2（同 noise_core/test_host/run.sh：环境里 source 过 master 快照时别悄悄换了 mbedtls）
idf_ver="$(sed -nE 's/^set\(IDF_VERSION_(MAJOR|MINOR|PATCH) ([0-9]+)\)$/\2/p' "$IDF_PATH/tools/cmake/version.cmake" 2>/dev/null | paste -sd.)"
[ "$idf_ver" = "6.0.2" ] || { echo "❌ IDF_PATH=$IDF_PATH 是 ESP-IDF '${idf_ver:-?}'，这里只认 v6.0.2（固件的版本）"; exit 1; }
export CC="${CC:-gcc}"

T="$(mktemp -d /tmp/muse-pair-test.XXXXXX)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/main" "$T/tests" "$T/managed_components"
for f in link_pairing.c pairing_transcript.c pairing_signer_policy.c; do ln -s "$COMP/$f" "$T/main/$f"; done
for f in "$COMP"/include/*.h; do ln -s "$f" "$T/main/$(basename "$f")"; done
U="$HERE/upstream_tests"
for f in "$U"/*.c; do ln -s "$f" "$T/tests/$(basename "$f")"; done
ln -s "$U/pairing_fakes" "$T/tests/pairing_fakes"
ln -s "$U/vectors" "$T/tests/vectors"
cp "$U"/test_*.py "$T/tests/"
touch "$T/tests/__init__.py"
ln -s "$HERE/kdf_vectors_harness.c" "$T/tests/kdf_vectors_harness.c"
cp "$HERE/test_kdf_vectors.py" "$T/tests/"
# 唯一的适配：上游写死了 p256-m 的库名 libp256m.a，IDF v6.0.2 的目标名是 p256-m（编出 libp256-m.a）。
# 只改临时副本、只在 IDF 的目标名确实是 p256-m 时改；上游哪天改掉这行就不再替换，测试本身照常判。
H="$T/tests/test_link_pairing_handshake.py"
P256_CMAKE="$IDF_PATH/components/mbedtls/mbedtls/tf-psa-crypto/drivers/p256-m/CMakeLists.txt"
if grep -q 'drivers/p256-m/libp256m\.a' "$H" && grep -q '_PREFIX}p256-m")' "$P256_CMAKE"; then
    sed -i 's|drivers/p256-m/libp256m\.a|drivers/p256-m/libp256-m.a|' "$H"
fi
ln -s "$REPO/managed_components/espressif__cjson" "$T/managed_components/espressif__cjson"

# 本仓库自写的判据（pair_core：provision 校验、JSON 深度、身份字符串、wifi_scan 回包、配对结果编码）
ML="$COMP/../muse_link"
"$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$COMP" -I"$ML/include" -I"$COMP/../utf8/include" \
    -o "$T/pair_core_test" "$HERE/pair_core_test.c" "$COMP/pair_core.c" "$ML/muse_cfg_core.c" "$COMP/../utf8/utf8.c"
"$T/pair_core_test"

cd "$T"
set +e
python3 -m unittest -v tests.test_link_pairing_transcript tests.test_link_pairing_signer_policy \
    tests.test_link_pairing_handshake tests.test_kdf_vectors > "$T/out.txt" 2>&1
rc=$?
set -e
grep -E "^test_|ok$|FAIL|ERROR|skipped|^Ran|^OK|^FAILED" "$T/out.txt" || true
# 退出码 + "OK" + 没有跳过，三者都要：跳过也算不过（缺 IDF / cryptography / 编译器时上游测试会静默跳过）
if [ $rc -eq 0 ] && grep -qE "^OK" "$T/out.txt" && ! grep -qE "skipped" "$T/out.txt"; then
    echo "全部通过"
else
    echo "❌ 有失败或跳过的用例（unittest 退出码 $rc）"
    tail -30 "$T/out.txt"
    exit 1
fi
