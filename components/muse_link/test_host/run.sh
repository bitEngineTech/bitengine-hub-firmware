#!/bin/bash
# muse_link 主机侧测试 —— 上游 muse-gadget-sdk 的 vm_api 测试原样跑在我们引入的文件上（docs/15 设计附录 C）。
#
#   bash components/muse_link/test_host/run.sh
#
# 做法同 muse_pair/test_host/run.sh：临时目录里搭出上游的目录结构（ROOT/main、ROOT/tests、ROOT/components/muse），
# 源文件链到本组件，测试脚本复制过去，然后原样跑。上游测试一行不改，跟进上游时直接替换 upstream_tests/。
#
# 覆盖（上游 tests/test_link_vm_api.py，带 ASan/UBSan）：fetch_vms / 设备令牌签发与刷新的请求与应答字段、
#   401/403 与其他错误的区分；test_link_vm_auth_token_contract：每台 VM 凭据隔离（vm_connect.h 的 no-fallback
#   规则：VM id 与令牌必须出自 fetch_vms 的同一条，缺了就拒连，绝不拿别的凭据顶）。
# upstream_tests/muse_account/ 是上游 components/muse/muse_account_api.{c,h}：固件**不引入**（附录 C），
#   放在这里只是因为上游 harness 顺带测了它，不带上它 harness 就得改。
# 另跑本仓库自写的 test_host/muse_cfg_core_test.c（编产品源文件 muse_cfg_core.c）、muse_cfg_store_test.c（muse_cfg.c + 假 hub_store）、
# muse_proto_test.c（muse_proto.c）、muse_link_core_test.c（muse_link_core.c）、muse_frame_test.c（muse_frame.c）与 muse_push_core_test.c（muse_push_core.c）。
# 需要：gcc、python3。
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
COMP="$(cd "$HERE/.." && pwd)"
export CC="${CC:-gcc}"

T="$(mktemp -d /tmp/muse-link-test.XXXXXX)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/main" "$T/tests" "$T/components/muse"
ln -s "$COMP/vm_api.c" "$T/main/vm_api.c"
# 会话层也放进 main/：上游的凭据检查（NoLegacyCredentialLintTest）扫的就是 main/（UPSTREAM.md 已知覆盖缺口，M3 兑现）
for f in muse_session.cpp muse_link.c muse_proto.c; do ln -s "$COMP/$f" "$T/main/$f"; done
for f in "$COMP"/include/*.h; do ln -s "$f" "$T/main/$(basename "$f")"; done
U="$HERE/upstream_tests"
for f in muse_account_api.c muse_account_api.h; do ln -s "$U/muse_account/$f" "$T/components/muse/$f"; done
for f in link_vm_api_harness.c link_vm_connect_harness.c host_compat.h link_noise_upgrade_harness.c ws_mask_harness.c; do ln -s "$U/$f" "$T/tests/$f"; done
ln -s "$U/link_fakes" "$T/tests/link_fakes"
cp "$U"/test_*.py "$T/tests/"
touch "$T/tests/__init__.py"

# 本仓库自写的判据（muse_cfg_core：SDK token 格式、令牌 / URL / 显示名校验、日志提示）
"$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$COMP/include" -I"$COMP/../utf8/include" \
    -o "$T/muse_cfg_core_test" "$HERE/muse_cfg_core_test.c" "$COMP/muse_cfg_core.c" "$COMP/../utf8/utf8.c"
"$T/muse_cfg_core_test"

# 本仓库自写的 muse_cfg 存储错误处理（编产品源文件 muse_cfg.c，hub_store 换成能注入错误的内存假实现；桩头在 test_host/stub/）：
# 只有确实没有令牌才报 NOT_FOUND、开机标志读不到就报错、清凭据的读回超时不算确认（M3 第四步审计 ❌）
"$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$HERE/stub" -I"$COMP/include" \
    -I"$COMP/../hub_core/include" -I"$COMP/../utf8/include" \
    -o "$T/muse_cfg_store_test" "$HERE/muse_cfg_store_test.c" "$COMP/muse_cfg.c" "$COMP/muse_cfg_core.c" "$COMP/../utf8/utf8.c"
"$T/muse_cfg_store_test" | grep -v "^[WE] "
[ "${PIPESTATUS[0]}" -eq 0 ] || exit 1

# 本仓库自写的 muse_proto（控制面报文：注册 / 应答判定 / 调用分类 / 超长报文扫 id / 分派 / 参数校验 / 结果信封；设计 4.7、4.8、4.13）
CJSON="$COMP/../../managed_components/espressif__cjson/cJSON"
[ -f "$CJSON/cJSON.c" ] || { echo "❌ 找不到 $CJSON/cJSON.c（先 idf.py build 拉 managed_components）"; exit 1; }
"$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$COMP" -I"$COMP/../hub_core/include" -I"$CJSON" \
    -o "$T/muse_proto_test" "$HERE/muse_proto_test.c" "$COMP/muse_proto.c" \
    "$COMP/../hub_core/hub_json.c" "$COMP/../hub_core/hub_registry.c" "$COMP/../hub_core/hub_params.c" "$CJSON/cJSON.c"
"$T/muse_proto_test"

# 本仓库自写的 muse_link_core（会话状态机与退避；设计 4.2 的状态 × 事件全排列）
"$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$COMP" -I"$COMP/include" \
    -o "$T/muse_link_core_test" "$HERE/muse_link_core_test.c" "$COMP/muse_link_core.c"
"$T/muse_link_core_test"

# 本仓库自写的 muse_frame（WebSocket 增量解析 + 控制流消息重组；模糊测试）
"$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$COMP" \
    -o "$T/muse_frame_test" "$HERE/muse_frame_test.c" "$COMP/muse_frame.c"
"$T/muse_frame_test"

# 本仓库自写的 muse_push_core（推送队列：优先级与挤占、过期、不合并 / 不去重 / 不限配额、在途标记、回包分类、请求体；设计 4.9、6.7、TC-MU13 主机侧）
"$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$COMP" -I"$COMP/../hub_core/include" -I"$CJSON" \
    -o "$T/muse_push_core_test" "$HERE/muse_push_core_test.c" "$COMP/muse_push_core.c" "$COMP/../hub_core/hub_json.c" "$CJSON/cJSON.c"
"$T/muse_push_core_test"

cd "$T"
set +e
# 会话层的两条上游测试只跑能独立运行的那条：test_ws_mask 的另一条、test_link_noise_session 的另外三条检查的是上游
# noise_control.cpp / app.c 的源码形状，我们不引入它们（改写成 muse_session.cpp，形状由本组件的 VERIFY 查）
timeout 600 python3 -m unittest -v tests.test_link_vm_api tests.test_link_vm_auth_token_contract \
    tests.test_ws_mask.WsMaskTest.test_word_masking_matches_the_byte_loop \
    tests.test_link_noise_session.LinkNoiseSessionTest.test_upgrade_request_and_classification > "$T/out.txt" 2>&1
rc=$?
set -e
grep -E "^test_|ok$|FAIL|ERROR|skipped|^Ran|^OK|^FAILED" "$T/out.txt" || true
# 退出码 + "OK" + 没有跳过，三者都要：跳过也算不过（缺 IDF / cryptography / 编译器时上游测试会静默跳过）
if [ $rc -eq 0 ] && grep -qE "^OK" "$T/out.txt" && ! grep -qE "skipped" "$T/out.txt" && grep -qE "^Ran 5 tests" "$T/out.txt"; then
    echo "全部通过"
else
    echo "❌ 有失败、跳过或少跑的用例（unittest 退出码 $rc，124 = 超时；应跑 5 条）"
    tail -30 "$T/out.txt"
    exit 1
fi
