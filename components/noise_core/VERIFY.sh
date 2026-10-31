#!/bin/bash
# components/noise_core/VERIFY.sh —— Noise 协议核心的引入约束（docs/15_muse_direct_design_v1.md 附录 C；M3 第一步）
# ⚠️ 必须 cd 进本目录跑：bash VERIFY.sh
#
# noise_core 是上游 muse-gadget-sdk 的 esp32/components/noise_core 原样引入（CMakeLists 登记为 modified），登记与哈希在
# components/muse_link/UPSTREAM.md。协议正确性在 test_host/run.sh：上游测试原样跑 + 自加的对 Linux SDK 响应方的互通测试。
# 这里查：只属于直连型、编进去的就是上游那 7 个源文件、编译选项、谁能依赖它、测试没被弄松。
# 产物检查读 build/（HA 型）与 build-direct/ 的 compile_commands.json：先 bash tools/build_profiles.sh。

PASS=0; FAIL=0
check() {
    if eval "$2" > /dev/null 2>&1; then echo "✅ $1"; PASS=$((PASS+1))
    else echo "❌ $1"; FAIL=$((FAIL+1)); fi
}
G="python3 ../../tools/grep_code.py"
SRCS="ClientSession InitiatorHandshake PsaCryptoBackend ServiceCodec Status Transport TransportFrameCodec"

# ---- 只属于直连型 + Muse 开关（架构 5.8 R-3） ----
check "CMake：源文件只在 BITENGINE_ENABLE_MUSE_LINK 下给" "grep -Pzo 'set\\(_noise_core_srcs \"\"\\)\\s*\\nif\\(CONFIG_BITENGINE_ENABLE_MUSE_LINK\\)' CMakeLists.txt"
check "CMake：依赖不放进 if（预展开读不到 CONFIG_*）" "python3 -c \"import sys; s=open('CMakeLists.txt').read(); i=s.find('if(CONFIG_BITENGINE_ENABLE_MUSE_LINK)'); e=s.find('endif()', i); sys.exit(1 if i < 0 or 'REQUIRES' in s[i:e] else 0)\""
check "CMake：REQUIRES / PRIV_REQUIRES 的参数里没有变量" "python3 -c \"import re,sys; sys.exit(1 if re.search(r'REQUIRES[^)]*[$][{]', open('CMakeLists.txt').read()) else 0)\""
# 下面几条查构建产物，不 grep CMake 文本（7.4）。产物比 CMakeLists 旧 = 判的是上一版的事实：先拦下来
check "build/ 与 build-direct/ 不比任何组件的 CMakeLists.txt 旧（旧了先 bash tools/build_profiles.sh）" "[ -f ../../build/project_description.json ] && [ -f ../../build-direct/project_description.json ] && [ -z \"\$(find ../*/CMakeLists.txt ../../main/CMakeLists.txt -newer ../../build/project_description.json; find ../*/CMakeLists.txt ../../main/CMakeLists.txt -newer ../../build-direct/project_description.json)\" ]"
check "直连型编进去的 noise_core 源文件正好是上游 CMakeLists 那 7 个（MbedtlsCryptoBackend.cpp 不编）" "python3 -c \"
import json,sys,os
fs=sorted(os.path.basename(e['file']) for e in json.load(open('../../build-direct/compile_commands.json')) if '/components/noise_core/' in e['file'])
sys.exit(0 if fs==sorted(s+'.cpp' for s in '$SRCS'.split()) else 1)\""
check "HA 型一个 noise_core 源文件都不编（R-3）" "python3 -c \"
import json,sys
fs=[e['file'] for e in json.load(open('../../build/compile_commands.json'))]
sys.exit(0 if fs and not any('/components/noise_core/' in f for f in fs) else 1)\""
check "直连型的 noise_core 源文件都带 -fno-exceptions -fno-rtti（同上游）" "python3 -c \"
import json,sys
cs=[e['command'] for e in json.load(open('../../build-direct/compile_commands.json')) if '/components/noise_core/' in e['file']]
sys.exit(0 if cs and all('-fno-exceptions' in c and '-fno-rtti' in c for c in cs) else 1)\""

# ---- 谁能用它：只有 muse_link（M3 的 muse_session）。HA 型组件里出现依赖 = 往 HA 型里拖直连型代码 ----
# 依赖读构建事实（project_description.json 的 reqs / priv_reqs），不逐行 grep CMakeLists：依赖名写在续行上就漏（2026-10-22 审计实测）
check "两种构建里依赖 noise_core 的组件只有 muse_link（或还没有）" "python3 -c \"
import json,sys
for b in ('../../build','../../build-direct'):
    bci=json.load(open(b+'/project_description.json'))['build_component_info']
    if 'noise_core' not in bci: sys.exit(1)
    users=[n for n,c in bci.items() if 'noise_core' in (c.get('reqs') or [])+(c.get('priv_reqs') or [])]
    if set(users)-{'muse_link'}: print(b, users); sys.exit(1)
\""
# --strings：#include "xplat/noise/…" 的路径在字符串字面量里，默认剥字符串会让引号写法永远匹配不上（审计实测空过）
check "除 noise_core / muse_link 外没有代码 include 它的头文件" "! for f in \$(grep -rlE 'xplat/noise/' ../*/ ../../main --include=*.c --include=*.cpp --include=*.h | grep -vE '^\\.\\./(noise_core|muse_link)/'); do $G --strings -q '#\\s*include\\s*[<\"]xplat/noise/' \$f && echo \$f; done | grep -q ."

# ---- 原样引入 + 登记（附录 C） ----
for f in LICENSE include/xplat/noise/core/InitiatorHandshake.h include/xplat/noise/core/Transport.h \
         include/xplat/noise/core/PsaCryptoBackend.h test_host/linux_sdk/noise_xx.py; do
    check "$f 存在" "[ -f $f ]"
done
for s in $SRCS MbedtlsCryptoBackend; do check "src/$s.cpp 存在" "[ -f src/$s.cpp ]"; done
check "引入文件与 UPSTREAM.md 清单一致、本目录每个文件都已登记（check_upstream_files.sh）" "bash ../muse_link/check_upstream_files.sh"
check "UPSTREAM.md 里本组件只有 CMakeLists.txt 登记为 modified" "[ \"\$(sed -n '/^\`\`\`manifest\$/,/^\`\`\`\$/p' ../muse_link/UPSTREAM.md | awk '\$2 ~ /^noise_core\\// && \$4 == \"modified\" {print \$2}')\" = 'noise_core/CMakeLists.txt' ]"

# ---- 主机测试没被弄松 ----
check "run.sh 跑上游两组 + 自加互通" "grep -qE 'unittest -v tests\\.test_noise_core tests\\.test_noise_core_inplace tests\\.test_noise_interop' test_host/run.sh"
check "run.sh 把跳过当失败（缺 PSA 头文件时上游测试会静默跳过）" "grep -q '! grep -qE \"skipped\"' test_host/run.sh"
check "互通 harness 用的是产品的 InitiatorHandshake 与 Transport（不是自己再写一份）" "$G -q 'tn::InitiatorHandshake hs\\(backend\\)' test_host/noise_interop_harness.cpp && $G -q 'tn::Transport t\\(backend' test_host/noise_interop_harness.cpp"
check "互通测试编的是组件里的源文件（不是副本）" "grep -q 'COMPONENT / \"src\"' test_host/test_noise_interop.py && grep -q 'COMPONENT = ROOT / \"components\" / \"noise_core\"' test_host/test_noise_interop.py"
check "互通测试的对端是 Linux SDK 的 NoiseXXResponder" "grep -q '^import noise_xx' test_host/test_noise_interop.py && grep -q 'noise_xx.NoiseXXResponder' test_host/test_noise_interop.py"
check "互通测试核对握手哈希、对端静态公钥、双方载荷" "grep -q 'assertEqual(hh, responder.handshake_hash())' test_host/test_noise_interop.py && grep -q 'peer.get(\"RS\"), responder_static' test_host/test_noise_interop.py && grep -q 'peer.get(\"PX\"), MSG2_PAYLOAD' test_host/test_noise_interop.py && grep -q 'seen\\[1\\], MSG3_PAYLOAD' test_host/test_noise_interop.py"
check "互通测试有高计数 nonce 一轮（钉高字节字节序）" "grep -q 'HIGH_NONCE = 0x0001020304050607' test_host/test_noise_interop.py && $G -q 'setSendNonceForTesting\\(kHighNonce\\)' test_host/noise_interop_harness.cpp"
check "互通测试有两条反例（篡改 msg2、篡改密文标签）" "grep -q 'def test_tampered_msg2_is_rejected' test_host/test_noise_interop.py && grep -q 'def test_tampered_transport_frame_is_rejected' test_host/test_noise_interop.py"
check "互通测试缺编译器时报错而不是跳过" "! grep -qE 'SkipTest|unittest\\.skip' test_host/test_noise_interop.py"
check "互通主用例在（改名不收集会让 run.sh 少跑一条；run.sh 另钉 Ran 6 tests）" "grep -q '    def test_handshake_and_transport_interop(self)' test_host/test_noise_interop.py && grep -q '\\^Ran 6 tests' test_host/run.sh"
check "run.sh 只认 IDF v6.0.2、unittest 套了 timeout" "grep -q '\\[ \"\\\$idf_ver\" = \"6.0.2\" \\]' test_host/run.sh && grep -q '^timeout [0-9]* python3 -m unittest' test_host/run.sh"

echo ""; echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
