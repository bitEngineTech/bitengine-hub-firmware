#!/bin/bash
# tools/build_profiles.sh —— 依次构建两种产品配置，再跑检查（docs/15 设计 2.7；架构 5.8 R-3 / R-4；用例 TC-PF02）
#
#   bash tools/build_profiles.sh                 # 两种都编、都查 —— 合入前必须这样跑一次（R-4）
#   bash tools/build_profiles.sh --only ha       # 日常只编一种
#   bash tools/build_profiles.sh --only direct
#   bash tools/build_profiles.sh --check-only    # 不编，只查已有的产物
#
#   HA 型    ：idf.py build                                  → build/bitengine.bin（产物、路径与以前完全一样）
#   直连型   ：idf.py -B build-direct -DSDKCONFIG=sdkconfig.direct \
#                     -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.direct" \
#                     -DBITENGINE_PROFILE=direct build      → build-direct/bitengine-direct.bin
#
# 检查三道，任一不过就非零退出；每道都连同它自己的变异自测一起跑（用例 TC-PF02）：
#   1. tools/check_profile_rules.py [--self-test]   源码形状：R-1 Kconfig 依赖、R-2 没有运行时的 Zigbee 主机开关、R-9 防串刷
#   2. tools/check_profiles.py [--self-test]        产物：HA 型里没有直连型组件、两种镜像的工程名、分区、app 余量
#   3. components/ota_cloud/test_host/run.sh + mutate_profile.py   防串刷比对的真值表（红线 1：先在主机上证明）
#   4. tools/migrate_profile.py --self-test        换固件脚本的判定（拿真实 build-direct 当被刷的产物；docs/15 设计 8.12）
#   5. Muse 引入的上游代码：check_upstream_files.sh（与 UPSTREAM.md 清单逐字节一致）+ muse_pair / muse_link / noise_core 主机测试
#      （上游测试原样跑；docs/15 设计附录 C）
#
# ⚠️ 直连型的产物里也有 bitengine-direct-unsigned.bin：同 bitengine-unsigned.bin，签名构建的中间产物，绝不能刷（红线 3）。
# ⚠️ 直连型镜像不要往 HA 型设备上传：设备层守卫会拒（profile_mismatch），但现场还没升级到带守卫版本的
#    HA 型固件挡不住（设计 2.7 第 3 层）。

set -u
cd "$(dirname "$0")/.." || exit 1

ONLY=""; BUILD=1
while [ $# -gt 0 ]; do
    case "$1" in
        --only)        ONLY="$2"; shift 2 ;;
        --check-only)  BUILD=0; shift ;;
        -h|--help)     sed -n 2,22p "$0"; exit 0 ;;
        *)             echo "❌ 未知参数 $1" >&2; exit 2 ;;
    esac
done
case "$ONLY" in ""|ha|direct) ;; *) echo "❌ --only 只认 ha / direct" >&2; exit 2 ;; esac

# shellcheck disable=SC1090
[ -n "${IDF_PATH:-}" ] || source "$HOME/esp-idf-v6.0.2/export.sh" >/dev/null 2>&1 \
    || { echo "❌ 需要 ESP-IDF 环境（source ~/esp-idf-v6.0.2/export.sh）" >&2; exit 1; }

build_one() {
    local name="$1"; shift
    local dir="$1"; shift
    mkdir -p "$dir"
    local log="$dir/build_profiles.log"
    printf '── 构建 %-6s → %s/ ……' "$name" "$dir"
    local t0=$SECONDS
    if idf.py -B "$dir" "$@" build > "$log" 2>&1; then
        printf ' ✅ %ds\n' $((SECONDS - t0))
    else
        printf ' ❌（日志 %s，末尾如下）\n' "$log"
        tail -25 "$log"
        exit 1
    fi
}

if [ "$BUILD" = 1 ]; then
    [ "$ONLY" = "direct" ] || build_one ha build
    [ "$ONLY" = "ha" ] || build_one direct build-direct \
        -DSDKCONFIG=sdkconfig.direct "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.direct" -DBITENGINE_PROFILE=direct
fi

FAIL=0
quiet() {   # 跑一个检查，只在不过时把输出摊开
    local what="$1"; shift
    local out
    if out=$("$@" 2>&1); then echo "✅ $what"; else echo "❌ $what："; echo "$out" | grep -vE '^\s*✅' | tail -20; FAIL=1; fi
}
echo ""; python3 tools/check_profile_rules.py || FAIL=1
quiet "check_profile_rules.py --self-test（变异自测）" python3 tools/check_profile_rules.py --self-test
echo ""; python3 tools/check_profiles.py ${ONLY:+--only "$ONLY"} || FAIL=1
quiet "check_profiles.py --self-test（变异自测）" python3 tools/check_profiles.py --self-test
echo ""
quiet "components/ota_cloud/test_host/run.sh（含防串刷真值表）" bash components/ota_cloud/test_host/run.sh
quiet "防串刷真值表的变异测试（mutate_profile.py）" python3 components/ota_cloud/test_host/mutate_profile.py
quiet "migrate_profile.py --self-test（换固件判定）" python3 tools/migrate_profile.py --self-test
echo ""
# 本机有上游检出（默认 ~/src/muse-gadget-sdk，MUSE_SDK_DIR 可改）就连上游逐字节比，否则只比本地清单
MUSE_SDK="${MUSE_SDK_DIR:-$HOME/src/muse-gadget-sdk}"
if [ -d "$MUSE_SDK/.git" ]; then
    quiet "Muse 上游引入文件：已登记、与清单一致、与上游固定提交逐字节一致" bash components/muse_link/check_upstream_files.sh "$MUSE_SDK"
else
    quiet "Muse 上游引入文件：已登记、与清单一致（本机没有上游检出 $MUSE_SDK，没比上游）" bash components/muse_link/check_upstream_files.sh
fi
quiet "components/muse_pair/test_host/run.sh（社区配对 v5 + 固件侧密钥派生对向量）" bash components/muse_pair/test_host/run.sh
quiet "components/muse_link/test_host/run.sh（vm_api + 凭据隔离）" bash components/muse_link/test_host/run.sh
quiet "components/noise_core/test_host/run.sh（上游 Noise 测试 + 对 Linux SDK 响应方的互通）" bash components/noise_core/test_host/run.sh
quiet "components/zhub/test_host/run.sh（ASH / EZSP 真值表 + 与本机 SDK 的 ash-common.c / ash-ncp.c 互通 + 拿 SDK 的 ezsp.c 当预言机）" bash components/zhub/test_host/run.sh
quiet "components/matter_ctrl/test_host/run.sh（fabric 身份的存储格式：往返、坏 blob、失败不留半截密钥）" bash components/matter_ctrl/test_host/run.sh
quiet "tools/patch_esp_matter.py --self-test（esp-matter 构建期补丁）" python3 tools/patch_esp_matter.py --self-test
# 直连型前端（docs/15 设计 2.7 A-29、10.2）：Muse 页行为、直连型中文表、全前端英文唯一源语言
quiet "frontend/direct/test_muse.sh（Muse 页：token 不回显、没密码时配对禁用、回读一致、出站清单逐项）" bash frontend/direct/test_muse.sh
quiet "frontend/direct/test_i18n.sh（直连型中文表与源码键双向一致）" bash frontend/direct/test_i18n.sh
quiet "tools/check_english_only.sh（含 frontend/direct）" bash tools/check_english_only.sh

echo ""
if [ "$FAIL" = 0 ]; then echo "✅ build_profiles：通过${ONLY:+（只做了 $ONLY）}"; else echo "❌ build_profiles：有不过的项，见上"; fi
exit $FAIL
