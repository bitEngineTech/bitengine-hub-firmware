#!/bin/bash
# components/device_identity/VERIFY.sh —— 出厂身份（fctry）的设计约束自动验证
#
#   bash components/device_identity/VERIFY.sh
#
# 本模块的危险不在"读错"，在"**写**"和"**擦**"：fctry 是产线一次性写入、
# 之后决定售后与保修追溯的东西，固件里任何一条写路径都是隐患。所以下面一半
# 的检查是反向不变量（"这段代码必须不存在"），而不是正向的"函数在不在"。

PASS=0; FAIL=0
DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../.." && pwd)"
C="$DIR/device_identity.c"
H="$DIR/include/device_identity.h"

check() {
    if eval "$2" > /dev/null 2>&1; then echo "✅ $1"; PASS=$((PASS+1))
    else echo "❌ $1"; FAIL=$((FAIL+1)); fi
}

echo "--- 文件与 API ---"
check "device_identity.c 存在"        "test -f '$C'"
check "device_identity.h 存在"        "test -f '$H'"
check "CMakeLists 注册了本组件"        "grep -q 'device_identity.c' '$DIR/CMakeLists.txt'"
for fn in device_identity_init device_identity_valid device_identity_serial \
          device_identity_hw_revision device_identity_mfg_date; do
    check "API $fn 已声明"            "grep -q '$fn' '$H'"
    check "API $fn 已实现"            "grep -q '$fn' '$C'"
done

echo ""
echo "--- ⚠️ 只读：固件绝不能写或擦 fctry ---"
# 擦掉 fctry = 设备永久失去身份，且外部零症状直到售后要查 SN 才发现。
# 产线写入走 tools/generate_nvs.py + esptool，不经过固件。
check "不调用 nvs_set_*"              "! grep -qE 'nvs_set_' '$C'"
check "不调用 nvs_erase_*"            "! grep -qE 'nvs_erase_' '$C'"
check "不调用 nvs_commit"             "! grep -q 'nvs_commit' '$C'"
check "不调用 nvs_flash_erase_partition" "! grep -q 'nvs_flash_erase_partition' '$C'"
check "打开 NVS 时用 NVS_READONLY"    "grep -q 'NVS_READONLY' '$C'"
check "不以 NVS_READWRITE 打开"       "! grep -q 'NVS_READWRITE' '$C'"
# 全仓范围：别的模块也不许碰 fctry 分区
check "全仓没有第二处碰 fctry 分区" \
    "[ \$(grep -rln 'FCTRY_PARTITION\|\"fctry\"' $ROOT/components $ROOT/main --include=*.c --include=*.h | grep -v device_identity | wc -l) -eq 0 ]"
# 恢复出厂只擦 "nvs" 分区里的命名空间；fctry 是**另一个分区**，天然安全。
# 这条防的是有人哪天"顺手把所有 NVS 分区都擦了"。
check "恢复出厂代码不提 fctry"        "! grep -q 'fctry' '$ROOT/components/config_mgr/config_mgr.c'"

echo ""
echo "--- 缺身份时必须能正常启动（出厂前的板子、开发板都没有）---"
# 读不到身份只能降级，不能拦启动：产线是"先烧固件再写身份"，若固件缺身份就
# 起不来，产线第一步就死了。
check "init 失败不返回致命错误码"      "grep -q 'ESP_OK' '$C'"
check "缺身份时 serial 返回空串而非 NULL" "grep -qE 'return +s_id\.serial|serial\[0\]' '$C'"
check "头文件写明缺身份的行为"        "grep -qE '未写|无出厂身份|空串' '$H'"

echo ""
echo "--- 不写 device_secret（本期决策：最小版）---"
# 没有消费者的密钥就是纯负债：一旦落盘，开源、日志脱敏、导出脱敏都要额外照顾它。
# ⚠️ 用 grep_code.py：头文件里有一整段在解释"为什么没有 device_secret"，
#    裸 grep 会被自己的说明文字绊倒。
check "固件不读 device_secret 键"     "python3 $ROOT/tools/grep_code.py --absent -q device_secret '$C' '$H'"

echo ""
echo "--- 契约：不能动 mDNS 的 serial_number ---"
# ⚠️ mDNS TXT 里**已经有**一个 serial_number，值是 MAC，且是 ZHA 必需的键
#    （缺了 ZHA 直接 async_abort(invalid_zeroconf_data)）。它和本模块的产品
#    SN 是两回事，同名纯属巧合。把它改成产品 SN 会换掉 ZHA 认设备的标识。
check "mDNS 的 serial_number 仍取 MAC" \
    "grep -A1 '\"serial_number\"' '$ROOT/components/mdns_svc/mdns_svc.c' | grep -q 'mac'"
check "mDNS 不引用 device_identity"   "! grep -q 'device_identity' '$ROOT/components/mdns_svc/mdns_svc.c'"

echo ""
echo "--- REST 与产线自检 ---"
check "/status 吐 serial_number"      "grep -q 'serial_number' '$ROOT/components/webui/rest_api.c'"
check "自检 identity 项查 SN"         "grep -q 'device_identity' '$ROOT/components/webui/factory_api.c'"
# identity 从"无条件 pass"改成"没身份就 fail" —— 产线唯一拦得住"忘了写身份"的地方
check "identity 项有 fail 分支"       "grep -A6 '\"identity\"' '$ROOT/components/webui/factory_api.c' | grep -q 'fail'"
# ⚠️ ESP32-C5 支持 802.15.4，esp_efuse_mac_get_default() 在这种芯片上要 **8 字节**
#    缓冲区（EUI-64）。给 6 字节 = 写越界 2 字节，且打出来的值是 EUI-64 的前 6 字节，
#    在网络上根本不存在。2026-09-11 修过一次，别回去。
# ⚠️ 判据只认**语句位置**的调用，不认注释里提到的词 —— 上面那段注释就在解释
#    "为什么不用它"，裸 grep 会被自己的说明文字绊倒（device_secret 那条已栽过一次）。
# ⚠️ 用 grep_code.py：factory_api.c 里有一整段在解释"为什么不用它"。
check "全仓没有 efuse_mac_get_default 的实际调用" \
    "python3 $ROOT/tools/grep_code.py --absent -q esp_efuse_mac_get_default \
     $ROOT/components/webui/factory_api.c $ROOT/components/webui/rest_api.c"
check "产线自检用 ESP_MAC_EFUSE_FACTORY" \
    "grep -q 'ESP_MAC_EFUSE_FACTORY' '$ROOT/components/webui/factory_api.c'"

echo ""
echo "--- 产线工具 ---"
GEN="$ROOT/tools/generate_nvs.py"
check "tools/generate_nvs.py 存在"    "test -f '$GEN'"
# 2026-09-13 真板首写抓到的两条：
# ① esptool 在 C5 上打印 EUI-64（38:44:be:ff:fe:12:34:56），旧正则 {17} 会把
#    38:44:be:ff:fe:12 这个不存在的地址记进台账 —— 行为判据，不比正则文本
check "generate_nvs: EUI-64 折 EUI-48 且小写" "python3 - <<'PY'
import importlib.util
sp=importlib.util.spec_from_file_location('gn','$GEN'); gn=importlib.util.module_from_spec(sp); sp.loader.exec_module(gn)
assert gn.normalize_mac('38:44:be:ff:fe:12:34:56')=='38:44:be:12:34:56'
assert gn.normalize_mac('38:44:BE:12:34:56')=='38:44:be:12:34:56'
PY"
# ② read-mac / write / read-flash 必须串成一次下载会话，且收尾不 hard-reset：
#    esptool 的复位不复位 DM9051，IO28 会把板子拉回下载模式（2/2）。用 AST 看
#    esptool(...) 的实参，不被注释里的 hard-reset 字样绊倒（CLAUDE.md 7.4）
check "generate_nvs: esptool 调用不带 hard-reset，且后续步骤 --before no-reset" "python3 - <<'PY'
import ast
t=ast.parse(open('$GEN',encoding='utf-8').read())
calls=[n for n in ast.walk(t) if isinstance(n,ast.Call) and getattr(n.func,'id','')=='esptool']
args=[[a.value for a in c.args if isinstance(a,ast.Constant)] for c in calls]
assert calls, 'no esptool() calls'
assert not any('hard-reset' in a for a in args), 'hard-reset present'
cmds=[next((x for x in a if x in ('read-mac','write-flash','read-flash')),None) for a in args]
for a,c in zip(args,cmds):
    if c in ('write-flash','read-flash'): assert 'no-reset' in a and '--before' in a, c
    if c=='read-mac': assert '--after' in a and 'no-reset' in a, c
PY"
check "生成器不写 device_secret 键"   "! grep -q '\"device_secret\"' '$GEN'"
check "生成器命名空间是 factory"      "grep -q 'NVS_NAMESPACE = \"factory\"' '$GEN'"
# 偏移**不能**写死在生成器里：分区表一改，写死的偏移就把身份写进隔壁分区
# （fctry 前面正好是 coredump），而且 esptool 不会有任何报错。
check "生成器不写死 fctry 偏移"     "! grep -qi '0x6b0000' '$GEN'"
check "生成器从 partitions.csv 现算" "grep -q 'partitions.csv' '$GEN'"
# 真跑一次，确认算出来的就是当前分区表里的值
check "算出的偏移/大小与分区表一致" \
    "python3 -c \"
import importlib.util,sys
spec=importlib.util.spec_from_file_location('g','$GEN')
m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
off,size=m.partition_geometry('fctry')
sys.exit(0 if (off,size)==(0x6b0000,0x4000) else 1)
\""
# 直连型的分区表另算（docs/15 设计 2.4）：fctry 在 0x7F0000。HA 型那条保留不动
check "直连型：按 partitions_direct.csv 算出 fctry 0x7F0000/0x4000" \
    "python3 -c \"
import importlib.util,sys
spec=importlib.util.spec_from_file_location('g','$GEN')
m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
off,size=m.partition_geometry('fctry','partitions_direct.csv','sdkconfig.direct')
sys.exit(0 if (off,size)==(0x7f0000,0x4000) else 1)
\""
# 键名：两侧逐字一致 + ≤15 字符 + 不含 device_secret。
# 逻辑在单独的 .py 里，不塞进 shell —— 正则经 bash→python 两层转义必错一次。
check "生成器与固件的键名一致"        "python3 '$DIR/check_keys.py'"

echo ""
echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
