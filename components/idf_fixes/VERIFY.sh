#!/bin/bash
# components/idf_fixes/VERIFY.sh — 本地回合的 IDF 上游修复（T9.47）的机器验证
# 编码前创建（P1）。必须 cd 进组件目录再跑：cd components/idf_fixes && bash VERIFY.sh
# 有产物时顺带查 ELF（build/bitengine.elf、build-direct/bitengine-direct.elf）：wrap 真的挂上了、诊断代码没进出厂固件。
cd "$(dirname "$0")" || exit 1
ROOT=../..
GC="python3 $ROOT/tools/grep_code.py -q"
PASS=0; FAIL=0; SKIP=0
check() {
    if eval "$2" > /dev/null 2>&1; then echo "✅ $1"; ((PASS++)); else echo "❌ $1"; ((FAIL++)); fi
}

echo "=== idf_fixes 验证 ==="

echo ""
echo "--- 文件 ---"
check "crypto_reset_lock.c 存在"        "test -f crypto_reset_lock.c"
check "crypto_race_diag.c 存在"         "test -f crypto_race_diag.c"
check "include/idf_fixes.h 存在"        "test -f include/idf_fixes.h"

echo ""
echo "--- 链接期 wrap（ECDSA 锁连带 SHA/AES 与 MPI 锁，回合上游 ba13c2852a + 48a864dd75）---"
check "wrap 了 ecdsa_lock_acquire"       "grep -q -- '--wrap=esp_crypto_ecdsa_lock_acquire' CMakeLists.txt"
check "wrap 了 ecdsa_lock_release"       "grep -q -- '--wrap=esp_crypto_ecdsa_lock_release' CMakeLists.txt"
# 没被引用的目标文件不会从静态库里拉进来；WHOLE_ARCHIVE 保证 __wrap_ 一定在
check "WHOLE_ARCHIVE"                    "grep -q 'WHOLE_ARCHIVE' CMakeLists.txt"
check "__wrap_ acquire 有定义"           "$GC 'void __wrap_esp_crypto_ecdsa_lock_acquire\s*\(void\)' crypto_reset_lock.c"
check "__wrap_ release 有定义"           "$GC 'void __wrap_esp_crypto_ecdsa_lock_release\s*\(void\)' crypto_reset_lock.c"
# 顺序与上游、与 esp_crypto_ds_lock_acquire 一致：ecdsa → ecc（real 里）→ sha_aes → mpi；放锁倒过来
check "acquire 顺序 real→sha_aes→mpi"     "python3 - <<'EOF'
import re,sys
s=open('crypto_reset_lock.c').read()
b=re.search(r'void __wrap_esp_crypto_ecdsa_lock_acquire\(void\)\s*\{(.*?)\n\}',s,re.S).group(1)
p=[b.find(x) for x in ('__real_esp_crypto_ecdsa_lock_acquire()','esp_crypto_sha_aes_lock_acquire()','esp_crypto_mpi_lock_acquire()')]
sys.exit(0 if -1 not in p and p==sorted(p) else 1)
EOF"
check "release 顺序 mpi→sha_aes→real"     "python3 - <<'EOF'
import re,sys
s=open('crypto_reset_lock.c').read()
b=re.search(r'void __wrap_esp_crypto_ecdsa_lock_release\(void\)\s*\{(.*?)\n\}',s,re.S).group(1)
b=b.split('#endif')[-1]   # 诊断构建的绕过分支在前面，只看正常路径
p=[b.find(x) for x in ('esp_crypto_mpi_lock_release()','esp_crypto_sha_aes_lock_release()','__real_esp_crypto_ecdsa_lock_release()')]
sys.exit(0 if -1 not in p and p==sorted(p) else 1)
EOF"

echo ""
echo "--- 升级 IDF 时的绊线（上游已修还套一层 = 不可重入锁重复加锁，第一次验签就锁死）---"
check "SOC_ECDSA_USES_MPI 时 #error"     "grep -A2 '^#if SOC_ECDSA_USES_MPI' crypto_reset_lock.c | grep -q '#error'"
check "CMakeLists 调用绊线"              "grep -q 'include(.*check_idf_lock.cmake)' CMakeLists.txt && grep -q 'idf_fixes_check_ecdsa_lock(' CMakeLists.txt"
check "IDF 版本钉（> v6.0.3 先停）"       "grep -A1 'ESP_IDF_VERSION > ESP_IDF_VERSION_VAL(6, 0, 3)' crypto_reset_lock.c | grep -q '#error'"
# 绊线真跑：当前 IDF 不报；把 sha_aes 加进函数体（紧跟 ecc 之后 / 放在 if 块之后的末尾）都必须报
T=$(mktemp -d)
IDF_LOCK="${IDF_PATH:-$HOME/esp-idf-v6.0.2}/components/esp_security/src/esp_crypto_lock.c"
python3 - "$IDF_LOCK" "$T" <<'PY'
import re,sys
s=open(sys.argv[1]).read()
m=re.search(r'(void esp_crypto_ecdsa_lock_acquire\(void\)\s*\{)(.*?)(\n\})',s,re.S)
open(sys.argv[2]+'/early.c','w').write(s[:m.end(1)]+'\n    esp_crypto_sha_aes_lock_acquire();'+s[m.end(1):])
open(sys.argv[2]+'/late.c','w').write(s[:m.start(3)]+'\n    esp_crypto_sha_aes_lock_acquire();'+s[m.start(3):])
PY
check "绊线：当前 IDF 不报"              "cmake -DIDF_LOCK_SRC='$IDF_LOCK' -P check_idf_lock.cmake"
check "绊线：sha_aes 紧跟 ecc 时报错"     "! cmake -DIDF_LOCK_SRC=$T/early.c -P check_idf_lock.cmake"
check "绊线：sha_aes 在 if 块之后也报"    "! cmake -DIDF_LOCK_SRC=$T/late.c -P check_idf_lock.cmake"
# 当前 IDF 确实是没修的那一版（否则上面的 wrap 是重复加锁）
IDF_SRC="${IDF_PATH:-$HOME/esp-idf-v6.0.2}/components/esp_security/src/esp_crypto_lock.c"
check "当前 IDF 的 ECDSA 锁不含 sha_aes" "python3 - '$IDF_SRC' <<'EOF'
import re,sys
s=open(sys.argv[1]).read()
m=re.search(r'void esp_crypto_ecdsa_lock_acquire\(void\)\s*\{(.*?)\n\}',s,re.S)
sys.exit(0 if m and 'esp_crypto_sha_aes_lock_acquire' not in m.group(1) else 1)
EOF"

echo ""
echo "--- 诊断旁路只在诊断构建里（出厂固件没有绕过修复的开关）---"
check "绕过开关都在 DIAG 块内"          "python3 - <<'EOF'
import sys
depth=0; diag=[]; bad=[]
for n,l in enumerate(open('crypto_reset_lock.c'),1):
    t=l.strip()
    if t.startswith('#if'):
        diag.append('CONFIG_BITENGINE_DIAG_CRYPTO_RACE' in t)
    elif t.startswith('#endif'):
        diag.pop()
    elif t.startswith('#else') and diag:
        diag[-1]=False
    elif 's_bypass' in l and not any(diag):
        bad.append(n)
sys.exit(1 if bad else 0)
EOF"
check "诊断文件整体在 DIAG 开关里"       "grep -q '^#if CONFIG_BITENGINE_DIAG_CRYPTO_RACE' crypto_race_diag.c"
check "Kconfig 有诊断开关、默认 n"       "grep -A3 'config BITENGINE_DIAG_CRYPTO_RACE' $ROOT/main/Kconfig.projbuild | grep -q 'default n'"
check "出厂配置没开诊断"                 "! grep -q 'CONFIG_BITENGINE_DIAG_CRYPTO_RACE=y' $ROOT/sdkconfig.defaults $ROOT/sdkconfig.defaults.direct $ROOT/sdkconfig $ROOT/sdkconfig.direct"

echo ""
echo "--- S2 / S3（诊断压测）---"
# 压测任务优先级必须低于 ot_main(16) / zhub(12)，且每圈都让出 CPU，否则 IDLE 饿 5 s 触发任务看门狗
check "工作者每个忙段后让出 CPU"         "$GC 'vTaskDelay\s*\(\s*pdMS_TO_TICKS\s*\(\s*WORK_SLEEP_MS' crypto_race_diag.c"
check "ECDSA 任务每次验签后让出 CPU"      "$GC 'vTaskDelay\s*\(\s*pdMS_TO_TICKS\s*\(\s*ECDSA_GAP_MS' crypto_race_diag.c"
# 两个任务（控制 / ECDSA）各自删一次：只数到一个 = 有一个任务退出时没删自己
check "压测两个任务做完都自删"          "[ \$(grep -cE 'vTaskDelete\s*\(\s*NULL\s*\)' crypto_race_diag.c) -ge 2 ]"
check "mbedtls_mpi init/free 成对"       "[ \$(grep -o 'mbedtls_mpi_init(' crypto_race_diag.c | wc -l) -le \$(grep -o 'mbedtls_mpi_free(' crypto_race_diag.c | wc -l) ]"
check "PSA 密钥用完销毁"                "$GC 'psa_destroy_key' crypto_race_diag.c"

echo ""
echo "--- 产物（有才查）---"
source "${IDF_PATH:-$HOME/esp-idf-v6.0.2}/export.sh" >/dev/null 2>&1
OBJDUMP=riscv32-esp-elf-objdump; NM=riscv32-esp-elf-nm
# 全片反汇编，列出调用原函数（esp_crypto_ecdsa_lock_acquire/release）的函数：必须非空、且只有 __wrap_ 自己
# （别处直接调 = 绕过修复）。写成函数而不是塞进 check 的字符串：check 用 eval，字符串里的 exit 会把整个脚本退掉
only_wrap_calls_real() {
    local callers
    callers=$($OBJDUMP -d --no-show-raw-insn "$1" | awk '/^[0-9a-f]+ <.*>:$/ {fn=$2}
        /<esp_crypto_ecdsa_lock_(acquire|release)>$/ && !/^[0-9a-f]+ </ {print fn}' | sort -u)
    [ -n "$callers" ] && ! echo "$callers" | grep -qvE '^<__wrap_esp_crypto_ecdsa_lock_(acquire|release)>:$'
}
for ELF in $ROOT/build/bitengine.elf $ROOT/build-direct/bitengine-direct.elf; do
    [ -f "$ELF" ] || { echo "⏭️  $(basename "$ELF") 不在，跳过"; ((SKIP++)); continue; }
    tag=$(basename "$ELF" .elf)
    check "$tag: 有 __wrap_ acquire/release"  "$NM $ELF | grep -q ' T __wrap_esp_crypto_ecdsa_lock_acquire' && $NM $ELF | grep -q ' T __wrap_esp_crypto_ecdsa_lock_release'"
    # 硬件 ECDSA 的拿锁点（esp_ecdsa_acquire_hardware 可能被内联）调的是 __wrap_，没有绕过它直接调原函数的
    D=$($OBJDUMP -d --no-show-raw-insn --disassemble=esp_ecdsa_transparent_verify_hash_complete --disassemble=esp_ecdsa_acquire_hardware --disassemble=esp_ecdsa_release_hardware "$ELF" 2>/dev/null)
    check "$tag: ECDSA 驱动调 __wrap_ 拿锁"   "echo \"\$D\" | grep -q '<__wrap_esp_crypto_ecdsa_lock_acquire>'"
    check "$tag: ECDSA 驱动调 __wrap_ 放锁"   "echo \"\$D\" | grep -q '<__wrap_esp_crypto_ecdsa_lock_release>'"
    check "$tag: 驱动里没有直接调原函数的"    "! echo \"\$D\" | grep -qE '<esp_crypto_ecdsa_lock_(acquire|release)>'"
    # 全片：调原函数的只能是 __wrap_ 自己（别处直接调 = 绕过修复）
    check "$tag: 全片只有 __wrap_ 调原函数"   "only_wrap_calls_real $ELF"
    check "$tag: 不是诊断构建"                "! grep -q 'define CONFIG_BITENGINE_DIAG_CRYPTO_RACE 1' \$(dirname $ELF)/config/sdkconfig.h"
    check "$tag: 没有旁路 / 压测符号"         "! $NM $ELF | grep -qE 'idf_fixes_diag_|crypto_race_diag_start'"
done

rm -rf "$T"
echo ""; echo "Result: $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ] || exit 1
