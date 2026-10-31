#!/bin/bash
# components/device_identity/test_host/run.sh
#   —— 序列号形状校验的主机侧测试（只要 gcc + python3，不用板子）
#
#   bash components/device_identity/test_host/run.sh
#
# ── 为什么这条要单独测 ──────────────────────────────────────────────
#
# 形状校验有**两份实现**：固件里的 serial_shape_ok()（决定设备认不认自己的
# 身份、产线自检红不红），和 tools/generate_nvs.py 里的 check_serial()
# （决定产线让不让这个号写下去）。两份规则漂移的后果不对称：
#
#   工具放行、固件判非法 → 号已经烧进 Flash，工位 3 才红，整块板返工
#   工具拒绝、固件本会放行 → 产线多报一次错，没有损失
#
# 所以 cases.txt 是**共用**的，两边跑同一组样例，结论必须逐条相同。
# VERIFY.sh 是 grep，它证明不了这件事 —— 它只看得见函数在不在。

set -u
cd "$(dirname "$0")" || exit 1
DIR=$(pwd)
ROOT=$(cd "$DIR/../../.." && pwd)
SRC="$DIR/../device_identity.c"
CASES="$DIR/cases.txt"

command -v gcc >/dev/null     || { echo "需要 gcc"; exit 1; }
command -v python3 >/dev/null || { echo "需要 python3"; exit 1; }
[ -f "$CASES" ] || { echo "找不到 cases.txt"; exit 1; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# ---- 把固件那份实现原样抠出来编译 ------------------------------------------
# 不重写一份"等价"的 C —— 重写出来的就不是被测对象了。直接从 .c 里截取
# serial_shape_ok 函数体，连同它依赖的 ID_MAX。这样改了固件而忘了改测试时，
# 抠不出来会直接报错，而不是继续测一份影子实现。
python3 - "$SRC" "$TMP/shape.c" <<'PY'
import re, sys
src = open(sys.argv[1], encoding='utf-8').read()
m = re.search(r'static bool serial_shape_ok\(const char \*s\)\s*\{.*?\n\}', src, re.S)
if not m:
    sys.exit("❌ 在 device_identity.c 里找不到 serial_shape_ok() —— 改名了？")
body = m.group(0)
# 头文件里的 DEVICE_ID_MAX 与 .c 里的 ID_MAX 别名
hdr = open(sys.argv[1].replace('device_identity.c',
                               'include/device_identity.h'), encoding='utf-8').read()
mm = re.search(r'#define\s+DEVICE_ID_MAX\s+(\d+)', hdr)
if not mm:
    sys.exit("❌ 头文件里找不到 DEVICE_ID_MAX")
open(sys.argv[2], 'w', encoding='utf-8').write(f"""
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#define ID_MAX {mm.group(1)}
{body}
int main(int argc, char **argv) {{
    if (argc < 2) return 2;
    return serial_shape_ok(argv[1]) ? 0 : 1;
}}
""")
print(f"   （抠出 serial_shape_ok，ID_MAX={mm.group(1)}）")
PY
[ $? -eq 0 ] || exit 1

gcc -std=c11 -Wall -Wextra -Werror -o "$TMP/shape" "$TMP/shape.c" || {
    echo "❌ 编译失败"; exit 1; }

PASS=0; FAIL=0
ok()  { echo "  ✅ $1"; PASS=$((PASS+1)); }
bad() { echo "  ❌ $1"; FAIL=$((FAIL+1)); }

echo ""
echo "── 固件实现 vs cases.txt ──"
while IFS=$'\t' read -r want sn; do
    case "$want" in ''|'#'*) continue ;; esac
    [ -z "${sn+x}" ] && continue
    if "$TMP/shape" "$sn"; then got=ok; else got=bad; fi
    if [ "$got" == "$want" ]; then ok "固件 $want: $sn"
    else bad "固件判成 $got，应为 $want: $sn"; fi
done < "$CASES"

# 空串单独测（cases.txt 里表达不了空字段）
if "$TMP/shape" ""; then bad "固件：空串应判非法"; else ok "固件 bad: (空串)"; fi

echo ""
echo "── 产线工具实现 vs 同一组样例 ──"
PY_OUT=$(python3 - "$ROOT/tools/generate_nvs.py" "$CASES" <<'PY'
import re, sys, importlib.util
spec = importlib.util.spec_from_file_location("gen", sys.argv[1])
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)

import contextlib, io

def tool_ok(sn):
    """复用工具自己的判据。check_serial 失败时打印并 sys.exit，这里都接住 ——
    不吞掉 stderr 的话，工具正常拒绝时打的那些 ❌ 会被外层的计数 grep 数进去。"""
    try:
        with contextlib.redirect_stderr(io.StringIO()):
            gen.check_serial(sn)
        return True
    except SystemExit:
        return False

bad = 0
for raw in open(sys.argv[2], encoding='utf-8'):
    line = raw.rstrip('\n')
    if not line.strip() or line.lstrip().startswith('#'):
        continue
    want, _, sn = line.partition('\t')
    want = want.strip()
    if want not in ('ok', 'bad'):
        continue
    got = 'ok' if tool_ok(sn) else 'bad'
    if got == want:
        print(f"  ✅ 工具 {want}: {sn}")
    else:
        print(f"  ❌ 工具判成 {got}，应为 {want}: {sn}")
        bad += 1
if tool_ok(''):
    print("  ❌ 工具：空串应判非法"); bad += 1
else:
    print("  ✅ 工具 bad: (空串)")
sys.exit(1 if bad else 0)
PY
)
PY_RC=$?
echo "$PY_OUT"
PY_PASS=$(echo "$PY_OUT" | grep -c '✅')
PY_FAIL=$(echo "$PY_OUT" | grep -c '❌')
PASS=$((PASS + PY_PASS)); FAIL=$((FAIL + PY_FAIL))
SHAPE_FAIL=$FAIL          # 形状校验两段的失败数，供末尾的一致性判定用

echo ""
echo "── 生成的镜像里真的有那三个键吗 ──"
# 形状校验对了不代表镜像对。这一段真跑一次 generate_nvs.py，再到产出的二进制里
# 找键名和值。NVS 把键名和短字符串按明文存，所以字节搜索就能证明"确实写进去了"。
# ⚠️ 这证明不了固件读得出来（那要真板子 + 串口，本机板子是 PoE 以太网，
#    写不进 fctry）。它证明的是**生成侧**没有静默产出一个空壳镜像 ——
#    docs/10 那份示例 CSV 因为键名超长根本生成不出来，正是这类故障。
if ! python3 -c "import os,sys; sys.exit(0 if os.environ.get('IDF_PATH') else 1)" 2>/dev/null; then
    echo "  ⏭️  未 source ESP-IDF 环境，跳过镜像生成检查"
else
    IMGDIR=$(mktemp -d)
    if python3 "$ROOT/tools/generate_nvs.py" --serial BEH-000042 \
            --hw-rev AM1 --date 2026-09-11 --out "$IMGDIR" >/dev/null 2>&1; then
        IMG=$(ls "$IMGDIR"/fctry_*.bin 2>/dev/null | head -1)
        if [ -n "$IMG" ]; then
            SZ=$(stat -c%s "$IMG")
            [ "$SZ" -eq 16384 ] && ok "镜像大小 16384 = 分区大小（docs/10 写的 0x2000 是错的）" \
                                 || bad "镜像 $SZ 字节，分区是 16384"
            for tok in factory serial_number hw_revision mfg_date \
                       BEH-000042 AM1 2026-09-11; do
                grep -qa -- "$tok" "$IMG" && ok "镜像里有 \"$tok\"" \
                                          || bad "镜像里找不到 \"$tok\""
            done
            grep -qa "device_secret" "$IMG" && bad "镜像里竟然有 device_secret" \
                                            || ok "镜像里没有 device_secret"
        else
            bad "generate_nvs.py 没产出 .bin"
        fi
    else
        bad "generate_nvs.py 生成失败"
    fi
    rm -rf "$IMGDIR"
fi

echo ""
echo "── --flash 的失败路径（假 esptool，台账隔离）──"
# 2026-09-13 真板首写抓到三条 bug 之后，收尾改成"停在下载模式 + 提示断电"，但那段
# finally 在真板上只走过成功路径。这里用假 esptool 逼出三种失败，断言：
#   exit 1 / stderr 有断电提示 / 台账一行不多 / 后续 esptool 步骤没再被调；
# 以及一条对照：没碰过板子就 die 的（--flash 无 --port）**不许**提示断电，
# 否则操作员会去拔一块根本没进下载模式的板子。
# ⚠️ 台账用 --ledger 指到临时文件，真台账 tools/.factory_identity_log.csv 绝不能被测试碰。
if [ -z "${IDF_PATH:-}" ]; then
    echo "  ⏭️  未 source ESP-IDF 环境，跳过（镜像生成要 nvs_partition_gen）"
else
    FK="$TMP/fake"; mkdir -p "$FK/bin" "$FK/out"
    cp "$DIR/fake_esptool.py" "$FK/bin/esptool"; chmod +x "$FK/bin/esptool"
    LEDGER="$FK/ledger.csv"
    printf 'written_at,mac,serial_number,hardware_revision,manufacture_date\n' > "$LEDGER"
    printf '2026-09-13T00:00:00,38:44:be:12:34:56,BEH-000001,AM1,2026-09-13\n' >> "$LEDGER"
    cp "$LEDGER" "$FK/ledger.before"
    HINT="停在下载模式"
    fl() {  # fl <场景名> <FAKE_MODE> <期望 esptool 调用次数> <额外参数...>
        local name=$1 mode=$2 ncalls=$3; shift 3
        : > "$FK/calls"
        PATH="$FK/bin:$PATH" FAKE_MODE=$mode FAKE_LOG="$FK/calls" \
            FAKE_LAST_WRITTEN="$FK/written.bin" \
            python3 "$ROOT/tools/generate_nvs.py" --port /dev/fake0 --flash \
                --ledger "$LEDGER" --out "$FK/out" "$@" >"$FK/o" 2>"$FK/e"
        local rc=$?
        [ $rc -eq 1 ] && ok "$name: exit 1" || bad "$name: exit $rc，应为 1"
        grep -q "$HINT" "$FK/e" && ok "$name: stderr 提示断电" || bad "$name: 没提示断电"
        [ "$(wc -l < "$FK/calls")" -eq "$ncalls" ] && ok "$name: esptool 恰好调了 $ncalls 步" \
            || bad "$name: esptool 调了 $(wc -l < "$FK/calls") 步，应为 $ncalls"
    }
    fl "读回不一致" readback_mismatch 3 --serial BEH-000001
    grep -q "读回的内容与写入的不一致" "$FK/e" && ok "读回不一致: 报的是比对失败" \
                                               || bad "读回不一致: 错误文案不对"
    fl "写入失败"   write_fail        2 --serial BEH-000001
    fl "台账撞号"   ok                1 --serial BEH-000099
    grep -q -- "--force" "$FK/e" && ok "台账撞号: 提示了 --force" || bad "台账撞号: 没提示 --force"
    cmp -s "$FK/ledger.before" "$LEDGER" && ok "三次失败后台账一行没多" \
                                        || bad "失败路径写了台账"
    # 对照：成功路径要记台账、不提示断电以外的错误
    : > "$FK/calls"
    PATH="$FK/bin:$PATH" FAKE_MODE=ok FAKE_LOG="$FK/calls" FAKE_LAST_WRITTEN="$FK/written.bin" \
        python3 "$ROOT/tools/generate_nvs.py" --port /dev/fake0 --flash \
            --ledger "$LEDGER" --out "$FK/out" --serial BEH-000001 >"$FK/o" 2>"$FK/e"
    [ $? -eq 0 ] && ok "成功路径: exit 0" || bad "成功路径: exit 非 0"
    [ "$(wc -l < "$LEDGER")" -eq 3 ] && ok "成功路径: 台账多了一行" || bad "成功路径: 台账没记"
    grep -q "$HINT" "$FK/e" && ok "成功路径: 也提示断电（成败都停在下载模式）" \
                            || bad "成功路径: 没提示断电"
    grep -q "38:44:be:12:34:56" "$LEDGER" && ok "台账里是 EUI-48（ff:fe 已折掉）" \
                                          || bad "台账里 MAC 不是折算后的 EUI-48"
    # 对照：没碰板子就 die 的不许提示断电
    python3 "$ROOT/tools/generate_nvs.py" --flash --ledger "$LEDGER" --out "$FK/out" >"$FK/o" 2>"$FK/e"
    [ $? -eq 1 ] && ! grep -q "$HINT" "$FK/e" && ok "没碰板子就 die: 不提示断电" \
                                              || bad "没碰板子就 die: 竟然提示断电"
fi

echo ""
echo "── 跨侧一致性 ──"
# ⚠️ 只看形状校验那两段的结果。第一版把镜像检查的失败也算进来了，于是镜像里
#    少个键会报成"两侧规则不一致"，指向完全错误的地方。
if [ $PY_RC -eq 0 ] && [ $SHAPE_FAIL -eq 0 ]; then
    ok "固件与产线工具对全部样例结论一致"
else
    bad "两侧结论不一致 —— 工具放行而固件判非法的号会烧进 Flash 才被发现"
fi

echo ""
echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
