#!/bin/bash
# zhub 主机侧测试（docs/15_muse_direct_design_v1.md 14.1 的 zhub 行）
#
#   bash components/zhub/test_host/run.sh
#
# ① zh_ash_test：zh_ash_core.c 的真值表（CRC / 随机化 / RST 金标、复位与连接、确认与自适应定时器、乱序 / 重复 / 坏帧、
#    重传与超时上限、回调里重入）。
# ②' zh_ses_test：zh_ezsp_ses.c 的真值表（握手、单在途与 seq、确认后才计时、幂等才重发、回显、保活、回调、重入）。
# ③' ses_interop：zh_ash_core + zh_ezsp_ses 对 SDK 真 NCP 状态机 + EZSP 应用层替身的端到端（回显、噪声、应用层挂死、非幂等命令恰好执行一次）。
# ②'' zh_host_test：zh_host_core.c（编排层：复位 → 握手 → networkInit → RUNNING、失败与退避、挂起 / 恢复）的真值表，NCP 是手搓的帧。
# ③'' host_interop：编排层对 SDK 真 NCP 状态机的端到端（上电、没有网络、NCP 死了又活、应用层挂死、NCP 自己重启、挂起、同步回调、噪声）。
# ② zh_ezsp_test：zh_ezsp_core.c 的真值表（头的分类、每个解码器的严格长度 / 帧 ID、编码器的参数校验与 220 字节边界）。
# ③ ncp_interop：与本机 SDK 互通 —— SDK 的 ash-common.c（编解码）、ash-ncp.c（NCP 侧状态机）、legacy_hal crc.c
#    和 sdk_interop/ncp_stub.c（缓冲、串口、计时、halReboot 的替身）编成 libsdkncp.so，测试 dlopen 它；
#    NCP 收到 RST 时重新加载 .so，相当于真重启。SDK 源码是 Silicon Labs MSLA：只在这里从本机路径编，不进仓库。
# ④ ezsp_oracle：拿 SDK 主机侧的 ezsp.c（含 command-functions.h）、ezsp-frame-utilities.c、ezsp-callbacks.c 当预言机——
#    同样的参数编出的命令逐字节相同、同样的回包 / 回调字节解出的字段逐个相同、头的分类逐格相同（有意更严的格子单独断言）。
#    SDK 路径默认 ~/src/mg24-fw/sdk（mg24/ 的固件也从那里编），可用 ZHUB_SDK 改。私有检出里找不到就失败，不跳过；
#    公开仓库（没有 docs/开发索引.md，tools/oss_export.py 不导出 docs/）里 SDK 不在是常态：只跑不依赖它的真值表。
# 需要：gcc。UBSan 一报就算失败（-fno-sanitize-recover=all）。
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
COMP="$(cd "$HERE/.." && pwd)"
CC="${CC:-gcc}"
SDK="${ZHUB_SDK:-$HOME/src/mg24-fw/sdk}"
NO_SDK=0
if [ ! -f "$SDK/simplicity_sdk.slcs" ] && [ ! -f "$COMP/../../docs/开发索引.md" ]; then
    NO_SDK=1
    echo "⏭  没有 Simplicity SDK（ZHUB_SDK=$SDK）：只跑不依赖 SDK 的测试；types.xml 核对、③ 互通、④ 预言机跳过"
fi
OUT="$(mktemp -d /tmp/zhub-test.XXXXXX)"
trap 'rm -rf "$OUT"' EXIT

SAN=(-fsanitize=address,undefined -fno-sanitize-recover=all)
"$CC" -std=c11 -Wall -Wextra -Werror "${SAN[@]}" -I"$COMP/include" \
    -o "$OUT/zh_ash_test" "$HERE/zh_ash_test.c" "$COMP/zh_ash_core.c"
"$OUT/zh_ash_test"
# 产品源文件另外过一遍 -Wconversion（和 UBSan 同开时它会误报测试代码里的整型提升，所以分开）
"$CC" -std=c11 -Wall -Wextra -Werror -Wconversion -I"$COMP/include" -c "$COMP/zh_ezsp_core.c" -o "$OUT/zh_ezsp_core.o"
"$CC" -std=c11 -Wall -Wextra -Werror "${SAN[@]}" -I"$COMP/include" \
    -o "$OUT/zh_ezsp_test" "$HERE/zh_ezsp_test.c" "$COMP/zh_ezsp_core.c"
"$OUT/zh_ezsp_test"
"$CC" -std=c11 -Wall -Wextra -Werror -Wconversion -I"$COMP/include" -c "$COMP/zh_ezsp_ses.c" -o "$OUT/zh_ezsp_ses.o"
"$CC" -std=c11 -Wall -Wextra -Werror "${SAN[@]}" -I"$COMP/include" \
    -o "$OUT/zh_ses_test" "$HERE/zh_ses_test.c" "$COMP/zh_ezsp_ses.c" "$COMP/zh_ezsp_core.c" "$COMP/zh_ash_core.c"
"$OUT/zh_ses_test"
"$CC" -std=c11 -Wall -Wextra -Werror -Wconversion -I"$COMP/include" -c "$COMP/zh_host_core.c" -o "$OUT/zh_host_core.o"
"$CC" -std=c11 -Wall -Wextra -Werror "${SAN[@]}" -I"$COMP/include" \
    -o "$OUT/zh_host_test" "$HERE/zh_host_test.c" "$COMP/zh_host_core.c" "$COMP/zh_ezsp_ses.c" "$COMP/zh_ezsp_core.c" "$COMP/zh_ash_core.c"
"$OUT/zh_host_test"
# 设备层（M4 第七步 a）：真值表 + 产品源文件过一遍 -Wconversion
"$CC" -std=c11 -Wall -Wextra -Werror -Wconversion -I"$COMP/include" -c "$COMP/zh_dev.c" -o "$OUT/zh_dev.o"
"$CC" -std=c11 -Wall -Wextra -Werror "${SAN[@]}" -I"$COMP/include" \
    -o "$OUT/zh_dev_test" "$HERE/zh_dev_test.c" "$COMP/zh_dev.c" "$COMP/zh_ezsp_core.c" "$COMP/zh_zcl.c" "$COMP/zh_tbl.c" "$COMP/zh_tuya.c" "$COMP/zh_tuya_db.c" "$COMP/zh_lumi.c" "$COMP/zh_lumi_db.c" -lm
"$OUT/zh_dev_test"
# 涂鸦私有簇（M5 第八步）：线上格式真值表 + 表的结构 + 金样例（导出脚本拿同一批报文喂社区设备库得到的输出，逐条对照固件的解码与换算）+ 模糊
"$CC" -std=c11 -Wall -Wextra -Werror -Wconversion -I"$COMP/include" -c "$COMP/zh_tuya.c" -o "$OUT/zh_tuya.o"
"$CC" -std=c11 -Wall -Wextra -Werror -Wconversion -I"$COMP/include" -c "$COMP/zh_tuya_db.c" -o "$OUT/zh_tuya_db.o"
"$CC" -std=c11 -Wall -Wextra -Werror -Wconversion -I"$COMP/include" -c "$COMP/zh_tbl.c" -o "$OUT/zh_tbl.o"
"$CC" -std=c11 -Wall -Wextra -Werror "${SAN[@]}" -I"$COMP/include" -o "$OUT/zh_tuya_test" "$HERE/zh_tuya_test.c" "$COMP/zh_tbl.c" "$COMP/zh_tuya.c" "$COMP/zh_tuya_db.c"
"$OUT/zh_tuya_test"
# 小米 / Aqara（M5 第十步）：TLV 真值表 + 表的结构 + 金样例（库对同一批 TLV / 属性的输出）+ 电量曲线 + 模糊
"$CC" -std=c11 -Wall -Wextra -Werror -Wconversion -I"$COMP/include" -c "$COMP/zh_lumi.c" -o "$OUT/zh_lumi.o"
"$CC" -std=c11 -Wall -Wextra -Werror -Wconversion -I"$COMP/include" -c "$COMP/zh_lumi_db.c" -o "$OUT/zh_lumi_db.o"
"$CC" -std=c11 -Wall -Wextra -Werror "${SAN[@]}" -I"$COMP/include" -o "$OUT/zh_lumi_test" "$HERE/zh_lumi_test.c" "$COMP/zh_lumi.c" "$COMP/zh_lumi_db.c" "$COMP/zh_tbl.c" "$COMP/zh_zcl.c"
"$OUT/zh_lumi_test"
# ZCL 最小编解码（M4 第七步 b）：真值表 + 每个数据类型的长度对着 SDK 的 types.xml 核 + 模糊（空口报文按恶意输入，Z27）
"$CC" -std=c11 -Wall -Wextra -Werror -Wconversion -I"$COMP/include" -c "$COMP/zh_zcl.c" -o "$OUT/zh_zcl.o"
"$CC" -std=c11 -Wall -Wextra -Werror "${SAN[@]}" -I"$COMP/include" -o "$OUT/zh_zcl_test" "$HERE/zh_zcl_test.c" "$COMP/zh_zcl.c"
if [ "$NO_SDK" = 1 ]; then
    "$OUT/zh_zcl_test" --no-sdk
    echo "不依赖 SDK 的部分全部通过（互通与预言机已跳过：设 ZHUB_SDK 指向 Simplicity SDK 2025.6.3 可跑全套）"
    exit 0
fi
"$OUT/zh_zcl_test" "$SDK/app/zcl/types.xml"

ASH_COMMON="$SDK/platform/service/legacy_common_ash/src/ash-common.c"
ASH_NCP="$SDK/platform/service/legacy_ncp_ash/src/ash-ncp.c"
CRC="$SDK/platform/service/legacy_hal/src/crc.c"
EZSP_DIR="$SDK/protocol/zigbee/app/util/ezsp"
for f in "$ASH_COMMON" "$ASH_NCP" "$CRC" "$EZSP_DIR/ezsp.c" "$EZSP_DIR/ezsp-frame-utilities.c" "$EZSP_DIR/ezsp-callbacks.c" \
         "$EZSP_DIR/command-functions.h"; do
    if [ ! -f "$f" ]; then
        echo "❌ 找不到 SDK 源文件 $f —— 设 ZHUB_SDK 指向 Simplicity SDK（与 mg24/ 用的同一份）"
        exit 1
    fi
done
# 版本要对：互通结论只对这一版 SDK 成立（UPSTREAM.md 登记了文件的 sha256）
SLCS="$SDK/simplicity_sdk.slcs"
if ! grep -q '^sdk_version: "2025.6.3"' "$SLCS" 2>/dev/null; then
    echo "❌ $SLCS 的 sdk_version 不是 2025.6.3 —— 换了 SDK 要重核 components/zhub/UPSTREAM.md 并更新这里"
    exit 1
fi
S="$HERE/sdk_interop"
INC=(-I "$S/stub" -I "$SDK/platform/service/legacy_common_ash/inc" -I "$SDK/platform/service/legacy_common_ash/config"
     -I "$SDK/platform/service/legacy_ncp_ash/inc" -I "$SDK/platform/service/legacy_hal/inc"
     -I "$SDK/protocol/zigbee" -I "$SDK/protocol/zigbee/app/util/ezsp")
# SDK 源文件按它自己的习惯编（-w：不为它的告警背锅）；我们的替身照样 -Wall -Werror
"$CC" -std=gnu11 -fPIC -w -DSL_ZIGBEE_TEST '-DPLATFORM_HEADER="sdk_plat.h"' "${INC[@]}" -c "$ASH_NCP" -o "$OUT/ash-ncp.o"
"$CC" -std=gnu11 -fPIC -w -DSL_ZIGBEE_TEST '-DPLATFORM_HEADER="sdk_plat.h"' "${INC[@]}" -c "$ASH_COMMON" -o "$OUT/ash-common.o"
"$CC" -std=gnu11 -fPIC -w -I "$SDK/platform/service/legacy_hal/inc" -c "$CRC" -o "$OUT/crc.o"
"$CC" -std=gnu11 -fPIC -Wall -Wextra -Werror -DSL_ZIGBEE_TEST '-DPLATFORM_HEADER="sdk_plat.h"' "${INC[@]}" -c "$S/ncp_stub.c" -o "$OUT/ncp_stub.o"
"$CC" -shared -o "$OUT/libsdkncp.so" "$OUT/ash-ncp.o" "$OUT/ash-common.o" "$OUT/crc.o" "$OUT/ncp_stub.o"
"$CC" -std=c11 -Wall -Wextra -Werror "${SAN[@]}" -I"$COMP/include" \
    -o "$OUT/ncp_interop" "$S/ncp_interop.c" "$COMP/zh_ash_core.c" -ldl
"$OUT/ncp_interop" "$OUT/libsdkncp.so"
# 会话层端到端：我们的 zh_ash_core + zh_ezsp_ses 对 SDK 真 NCP 状态机 + EZSP 应用层替身（同一个 libsdkncp.so）
"$CC" -std=c11 -Wall -Wextra -Werror "${SAN[@]}" -I"$COMP/include" \
    -o "$OUT/ses_interop" "$S/ses_interop.c" "$COMP/zh_ash_core.c" "$COMP/zh_ezsp_core.c" "$COMP/zh_ezsp_ses.c" -ldl
"$OUT/ses_interop" "$OUT/libsdkncp.so"
# 编排层端到端：zh_host_core（连同下面三层）对同一个 libsdkncp.so
"$CC" -std=c11 -Wall -Wextra -Werror "${SAN[@]}" -I"$COMP/include" \
    -o "$OUT/host_interop" "$S/host_interop.c" "$COMP/zh_host_core.c" "$COMP/zh_ash_core.c" "$COMP/zh_ezsp_core.c" "$COMP/zh_ezsp_ses.c" -ldl
"$OUT/host_interop" "$OUT/libsdkncp.so"

# 头文件里每个帧 ID 常量注释的 ezsp-enum.h 行号、四个协议常量注释的 ezsp-protocol.h 行号，逐个对着 SDK 核（数值错了预言机抓得到，行号错了只有这里）
python3 - "$COMP/include/zh_ezsp_core.h" "$EZSP_DIR" "$COMP/include/zh_ezsp_ses.h" <<'PY'
import re, sys
h = open(sys.argv[1], encoding="utf-8").read()
ses = open(sys.argv[3], encoding="utf-8").read()
enum = open(sys.argv[2] + "/ezsp-enum.h", encoding="utf-8").read().split("\n")
proto = open(sys.argv[2] + "/ezsp-protocol.h", encoding="utf-8").read().split("\n")
bad, n = [], 0
for name, val, line in re.findall(r"#define ZH_EZSP_ID_(\w+)\s+(0x[0-9A-F]{4})\s+/\* ezsp-enum\.h:(\d+) \*/", h):
    n += 1
    if not re.search(r"\bSL_ZIGBEE_EZSP_%s\s*=\s*%s\b" % (name, val), enum[int(line) - 1], re.I):
        bad.append("ZH_EZSP_ID_%s → ezsp-enum.h:%s" % (name, line))
for zn, val, line, sn in re.findall(r"#define (ZH_EZSP_\w+)\s+(\w+)\s+/\* ezsp-protocol\.h:(\d+) (EZSP_\w+)", h):
    n += 1
    if not re.search(r"#define\s+%s\b" % sn, proto[int(line) - 1]):
        bad.append("%s → ezsp-protocol.h:%s" % (zn, line))
# 其余头文件里"ezsp-enum.h:N SL_ZIGBEE_X"写法的出处（zh_ezsp_ses.h 的原因码等）：那一行要真有这个名字
for line, sn in re.findall(r"ezsp-enum\.h:(\d+) (SL_ZIGBEE_\w+)", ses):
    n += 1
    if not re.search(r"\b%s\b" % sn, enum[int(line) - 1]):
        bad.append("zh_ezsp_ses.h %s → ezsp-enum.h:%s" % (sn, line))
if n < 45 or bad:
    print("❌ zh_ezsp_core.h 的 SDK 行号对不上（核了 %d 个）：%s" % (n, bad))
    sys.exit(1)
print("zh_ezsp_core.h / zh_ezsp_ses.h：%d 处 SDK 行号（ezsp-enum.h / ezsp-protocol.h）与 SDK 一致" % n)
PY
# zh_host_core.h 的 sl_status.h / sl_zigbee_types.h 出处：那一行要真有这个名字、值也对
python3 - "$COMP/include/zh_host_core.h" "$SDK" <<'PY'
import re, sys
h = open(sys.argv[1], encoding="utf-8").read()
st = open(sys.argv[2] + "/platform/common/inc/sl_status.h", encoding="utf-8").read().split("\n")
ty = open(sys.argv[2] + "/protocol/zigbee/stack/include/sl_zigbee_types.h", encoding="utf-8").read().split("\n")
names = {"OK": "SL_STATUS_OK", "NETWORK_UP": "SL_STATUS_NETWORK_UP", "NETWORK_DOWN": "SL_STATUS_NETWORK_DOWN", "NOT_JOINED": "SL_STATUS_NOT_JOINED",
         "TC_SWAP_CHANGED": "SL_STATUS_ZIGBEE_TRUST_CENTER_SWAP_EUI_HAS_CHANGED", "TC_SWAP_SAME": "SL_STATUS_ZIGBEE_TRUST_CENTER_SWAP_EUI_HAS_NOT_CHANGED",
         "NETWORK_OPENED": "SL_STATUS_ZIGBEE_NETWORK_OPENED", "NETWORK_CLOSED": "SL_STATUS_ZIGBEE_NETWORK_CLOSED"}
bad, n = [], 0
for name, val, line in re.findall(r"#define ZH_SL_(\w+)\s+(0x[0-9A-F]{4})u\s+/\* sl_status\.h:(\d+)", h):
    n += 1
    if not re.search(r"#define\s+%s\s+\(\(sl_status_t\)%s\)" % (names.get(name, "?"), val), st[int(line) - 1], re.I):
        bad.append("ZH_SL_%s → sl_status.h:%s" % (name, line))
for line, sn, val in ((642, "SL_ZIGBEE_NETWORK_INIT_PARENT_INFO_IN_TOKEN", "0x0001"), (644, "SL_ZIGBEE_NETWORK_INIT_END_DEVICE_REJOIN_ON_REBOOT", "0x0002")):
    n += 1
    if not re.search(r"\b%s\s*=\s*%s\b" % (sn, val), ty[line - 1]):
        bad.append("%s → sl_zigbee_types.h:%d" % (sn, line))
enum = open(sys.argv[2] + "/protocol/zigbee/app/util/ezsp/ezsp-enum.h", encoding="utf-8").read().split("\n")
for name, val, line in re.findall(r"#define ZH_EZSP_CFG_(\w+)\s+(0x[0-9A-F]{2})u\s+/\* ezsp-enum\.h:(\d+)", h):
    n += 1
    if not re.search(r"\bSL_ZIGBEE_EZSP_CONFIG_%s\s*=\s*%s\b" % (name, val), enum[int(line) - 1], re.I):
        bad.append("ZH_EZSP_CFG_%s → ezsp-enum.h:%s" % (name, line))
# 策略 ID：SL_ZIGBEE_EZSP_<名>_POLICY；取值：SL_ZIGBEE_EZSP_DECISION_<名> 或 SL_ZIGBEE_EZSP_<名>（决策 ID 不带 DECISION_ 前缀）
for name, val, line in re.findall(r"#define ZH_EZSP_POLICY_(\w+)\s+(0x[0-9A-F]{2})u\s+/\* ezsp-enum\.h:(\d+)", h):
    n += 1
    if not re.search(r"\bSL_ZIGBEE_EZSP_%s_POLICY\s*=\s*%s\b" % (name, val), enum[int(line) - 1], re.I):
        bad.append("ZH_EZSP_POLICY_%s → ezsp-enum.h:%s" % (name, line))
for name, val, line in re.findall(r"#define ZH_EZSP_DECISION_(\w+)\s+(0x[0-9A-F]{2})u\s+/\* ezsp-enum\.h:(\d+)", h):
    n += 1
    v = "0x%04X" % int(val, 16) if "DECISION_" + name in enum[int(line) - 1] else val
    if not re.search(r"\bSL_ZIGBEE_EZSP_(DECISION_)?%s\s*=\s*%s\b" % (name, v), enum[int(line) - 1], re.I):
        bad.append("ZH_EZSP_DECISION_%s → ezsp-enum.h:%s" % (name, line))
# 集中器：类型与发现模式在 sl_zigbee_types.h，时间与阈值是 SDK 集中器插件的默认值
cc = open(sys.argv[2] + "/protocol/zigbee/app/framework/plugin/concentrator/config/concentrator-config.h", encoding="utf-8").read().split("\n")
for name, val, line, sn in re.findall(r"#define (ZH_HOST_CONC_\w+)\s+(\d+)u\s+/\* concentrator-config\.h:(\d+) (\w+)", h):
    n += 1
    if not re.search(r"#define SL_ZIGBEE_AF_PLUGIN_CONCENTRATOR_%s\s+%s\b" % (sn, val), cc[int(line) - 1]):
        bad.append("%s → concentrator-config.h:%s" % (name, line))
for name, val, line, sn in re.findall(r"#define (ZH_HOST_(?:CONC_TYPE|MTORR_MODE))\s+(0x[0-9A-F]+)u\s+/\* sl_zigbee_types\.h:(\d+) (\w+)", h):
    n += 1
    if not re.search(r"\b%s\b\s*=?\s*%su?\b" % (sn, val), ty[int(line) - 1], re.I):
        bad.append("%s → sl_zigbee_types.h:%s" % (name, line))
# 入网窗口与 ZDO（第六步）：每个常量的 [文件, 行号, 那一行必须有的名字, 数值]，名字与数值都要在那一行
Z = sys.argv[2] + "/protocol/zigbee/"
files = {"af-types.h": Z + "app/framework/include/af-types.h", "zigbee-device-stack.h": Z + "stack/include/zigbee-device-stack.h",
         "zigbee-device-common.c": Z + "app/util/zigbee-framework/zigbee-device-common.c", "af-common.c": Z + "app/framework/util/af-common.c",
         "sl_zigbee_types.h": Z + "stack/include/sl_zigbee_types.h"}
for name, sym in (("ZH_HOST_PERMIT_MAX_S", "SL_ZIGBEE_AF_PERMIT_JOIN_MAX_TIMEOUT"), ("ZH_ZDO_PROFILE", "SL_ZIGBEE_ZDO_PROFILE_ID"),
                  ("ZH_ZDO_ENDPOINT", "SL_ZIGBEE_ZDO_ENDPOINT"), ("ZH_ZDO_BCAST_ROUTERS", "SL_ZIGBEE_BROADCAST_ADDRESS"),
                  ("ZH_NULL_NODE_ID", "SL_ZIGBEE_NULL_NODE_ID"), ("ZH_ZDO_PERMIT_JOINING_REQ", "PERMIT_JOINING_REQUEST"),
                  ("ZH_ZDO_DEVICE_ANNCE", "END_DEVICE_ANNOUNCE"), ("ZH_ZDO_RADIUS", "zigDevRequestRadius"), ("ZH_ZDO_TC_SIGNIFICANCE", "TC significance"),
                  ("ZH_ZDO_APP_SEQ_MASK", "APPLICATION_ZDO_SEQUENCE_MASK")):
    m = re.search(r"#define %s\s+(0x[0-9A-Fa-f]+|\d+)u\s+/\* ([\w.-]+\.[ch]):(\d+)" % name, h)
    n += 1
    if not m or m.group(2) not in files:
        bad.append("%s：没有出处注释" % name)
        continue
    line = open(files[m.group(2)], encoding="utf-8").read().split("\n")[int(m.group(3)) - 1]
    nums = [int(x, 0) for x in re.findall(r"\b(0x[0-9A-Fa-f]+|\d+)u?\b", line)]
    if sym not in line or int(m.group(1), 0) not in nums:
        bad.append("%s → %s:%s" % (name, m.group(2), m.group(3)))
m = re.search(r"#define ZH_HOST_INDIRECT_TX_TIMEOUT_MS\s+(\d+)u\s+/\* af-host\.c:(\d+)", h)
n += 1
af = open(Z + "app/framework/util/af-host.c", encoding="utf-8").read().split("\n")
if not m or af[int(m.group(2)) - 1].strip() != m.group(1) + "," or "SL_ZIGBEE_EZSP_CONFIG_INDIRECT_TRANSMISSION_TIMEOUT" not in af[int(m.group(2)) - 2]:
    bad.append("ZH_HOST_INDIRECT_TX_TIMEOUT_MS → af-host.c")
if n != 45 or bad or "sl_zigbee_types.h:642、644" not in h:
    print("❌ zh_host_core.h 的 SDK 出处对不上（核了 %d 个）：%s" % (n, bad))
    sys.exit(1)
print("zh_host_core.h：%d 处 SDK 出处（sl_status.h / sl_zigbee_types.h / ezsp-enum.h / concentrator-config.h）与 SDK 一致" % n)
PY

# zh_dev.h 的 SDK 出处：[常量, 文件, 那一行必须有的名字]，名字与数值都要在那一行（APS 选项看 SDK 同类先例那一行就是 RETRY、RETRY 的值是 0x0040）
python3 - "$COMP/include/zh_dev.h" "$SDK" <<'PY'
import re, sys
h = open(sys.argv[1], encoding="utf-8").read()
Z = sys.argv[2] + "/protocol/zigbee/"
bad, n = [], 0
for name, path, sym in (("ZH_ZDO_IEEE_ADDR_REQ", "stack/include/zigbee-device-stack.h", "IEEE_ADDRESS_REQUEST"),
                        ("ZH_ZDO_IEEE_ADDR_RSP", "stack/include/zigbee-device-stack.h", "IEEE_ADDRESS_RESPONSE"),
                        ("ZH_OUTGOING_DIRECT", "stack/include/sl_zigbee_types.h", "SL_ZIGBEE_OUTGOING_DIRECT")):
    m = re.search(r"#define %s\s+(0x[0-9A-Fa-f]+|\d+)u\s+/\* [\w.-]+\.h:(\d+)" % name, h)
    n += 1
    if not m:
        bad.append(name + "：没有出处"); continue
    line = open(Z + path, encoding="utf-8").read().split("\n")[int(m.group(2)) - 1]
    if sym not in line or (name != "ZH_OUTGOING_DIRECT" and int(m.group(1), 0) not in [int(x, 0) for x in re.findall(r"\b(0x[0-9A-Fa-f]+|\d+)u?\b", line)]):
        bad.append("%s → %s:%s" % (name, path, m.group(2)))
m = re.search(r"#define ZH_APS_OPTS_UNICAST\s+0x0040u\s+/\* device-table-tracking\.c:(\d+) handleUnknownDevice", h)
n += 1
line = open(Z + "app/framework/plugin/device-table/device-table-tracking.c", encoding="utf-8").read().split("\n")[int(m.group(1)) - 1] if m else ""
ty = open(Z + "stack/include/sl_zigbee_types.h", encoding="utf-8").read()
if not m or line.strip() != "SL_ZIGBEE_APS_OPTION_RETRY);" or not re.search(r"SL_ZIGBEE_APS_OPTION_RETRY\s*=\s*0x0040,", ty):
    bad.append("ZH_APS_OPTS_UNICAST → device-table-tracking.c")
# 访谈的 ZDO（第七步 b）：簇号 / 最小长度那一行有名字和数值
ds = open(Z + "stack/include/zigbee-device-stack.h", encoding="utf-8").read().split("\n")
for name, sym in (("ZH_ZDO_NODE_DESC_REQ", "NODE_DESCRIPTOR_REQUEST"), ("ZH_ZDO_NODE_DESC_RSP", "NODE_DESCRIPTOR_RESPONSE"),
                  ("ZH_ZDO_NODE_RSP_MIN", "NODE_DESCRIPTOR_RESPONSE_MINIMUM_LENGTH"), ("ZH_ZDO_SIMPLE_DESC_REQ", "SIMPLE_DESCRIPTOR_REQUEST"),
                  ("ZH_ZDO_SIMPLE_DESC_RSP", "SIMPLE_DESCRIPTOR_RESPONSE"), ("ZH_ZDO_ACTIVE_EP_REQ", "ACTIVE_ENDPOINTS_REQUEST"),
                  ("ZH_ZDO_ACTIVE_EP_RSP", "ACTIVE_ENDPOINTS_RESPONSE"),
                  ("ZH_ZDO_BIND_REQ", "BIND_REQUEST"), ("ZH_ZDO_BIND_RSP", "BIND_RESPONSE"), ("ZH_ZDO_UNICAST_BINDING", "UNICAST_BINDING"),
                  ("ZH_ZDO_LQI_TABLE_REQ", "LQI_TABLE_REQUEST"), ("ZH_ZDO_LQI_TABLE_RSP", "LQI_TABLE_RESPONSE")):   # 主动发现（M6 第一步）
    m = re.search(r"#define %s\s+(0x[0-9A-Fa-f]+|\d+)u\s+/\* zigbee-device-stack\.h:(\d+) %s\b" % (name, sym), h)
    n += 1
    line = ds[int(m.group(2)) - 1] if m else ""
    if not m or not re.match(r"#define %s\s+%s(u|\b)" % (sym, m.group(1)), line):
        bad.append("%s → zigbee-device-stack.h" % name)
m = re.search(r"#define ZH_ZDP_SUCCESS\s+0x00u\s+/\* sl_zigbee_types\.h:(\d+) SL_ZIGBEE_ZDP_SUCCESS", h)
n += 1
if not m or not re.search(r"\bSL_ZIGBEE_ZDP_SUCCESS\s*=\s*0x00,", ty.split("\n")[int(m.group(1)) - 1]):
    bad.append("ZH_ZDP_SUCCESS → sl_zigbee_types.h")
m = re.search(r"#define ZH_APS_OPTS_DEFAULT\s+0x1040u\s+/\* config\.h:(\d+)-(\d+)", h)
n += 1
seg = "\n".join(open(Z + "app/framework/util/config.h", encoding="utf-8").read().split("\n")[int(m.group(1)) - 1:int(m.group(2))]) if m else ""
if not m or "#define SL_ZIGBEE_AF_DEFAULT_APS_OPTIONS" not in seg or "SL_ZIGBEE_APS_OPTION_RETRY" not in seg or "SL_ZIGBEE_APS_OPTION_ENABLE_ADDRESS_DISCOVERY" not in seg \
   or "ROUTE_DISCOVERY" in seg or not re.search(r"SL_ZIGBEE_APS_OPTION_ENABLE_ADDRESS_DISCOVERY\s*=\s*0x1000", ty):
    bad.append("ZH_APS_OPTS_DEFAULT → config.h（要的是集中器那一支：不带 ROUTE_DISCOVERY）")
m = re.search(r"#define ZH_APS_OPT_SOURCE_EUI64 0x0400u /\* sl_zigbee_types\.h:(\d+) SL_ZIGBEE_APS_OPTION_SOURCE_EUI64", h)
n += 1
if not m or not re.search(r"\bSL_ZIGBEE_APS_OPTION_SOURCE_EUI64\s*=\s*0x0400,", ty.split("\n")[int(m.group(1)) - 1]):
    bad.append("ZH_APS_OPT_SOURCE_EUI64 → sl_zigbee_types.h")
ic = open(Z + "app/framework/plugin/ias-zone-client/ias-zone-client.c", encoding="utf-8").read().split("\n")
if "SL_ZIGBEE_APS_OPTION_SOURCE_EUI64" not in ic[334 - 1] or "check our EUI64 against his CIE Address" not in ic[333 - 1] or "ias-zone-client.c:332-334" not in h:
    bad.append("ZH_APS_OPT_SOURCE_EUI64 的理由 → ias-zone-client.c:332-334")
# 邻居条目（M6 第一步）：长度与三个位域的出处行里要有对应的写法（数值：长度 22、类型掩码 0x03、rx 掩码 0x0C = 3 << 2、关系掩码 0x70、前子节点 0x40 = 4 << 4）
for name, pat, want in (("ZH_ZDO_LQI_ENTRY_LEN", r"#define ZH_ZDO_LQI_ENTRY_LEN\s+22u\s+/\* zigbee-device-stack\.h:(\d+)：", "<entry:22>"),
                        ("ZH_LQI_TYPE_MASK", r"#define ZH_LQI_TYPE_MASK\s+0x03u\s+/\* zigbee-device-stack\.h:(\d+)-\d+：", "device type      0x03"),
                        ("ZH_LQI_RX_SHIFT", r"#define ZH_LQI_RX_SHIFT\s+2u\s+/\* zigbee-device-stack\.h:(\d+)-\d+：0x0C", "rx mode          0x0C"),
                        ("ZH_LQI_REL_SHIFT", r"#define ZH_LQI_REL_SHIFT\s+4u\s+/\* zigbee-device-stack\.h:(\d+)-\d+：0x70", "relationship     0x70")):
    m = re.search(pat, h)
    n += 1
    if not m or want not in ds[int(m.group(1)) - 1]:
        bad.append("%s → zigbee-device-stack.h" % name)
if not re.search(r"#define ZH_LQI_REL_PREV_CHILD\s+4u", h) or "0x40 previous child" not in ds[502 - 1] or "zigbee-device-stack.h:498-502" not in h:
    bad.append("ZH_LQI_REL_PREV_CHILD → zigbee-device-stack.h:502（0x40 = 4 << ZH_LQI_REL_SHIFT）")
if n != 23 or bad:
    print("❌ zh_dev.h 的 SDK 出处对不上（核了 %d 个）：%s" % (n, bad)); sys.exit(1)
print("zh_dev.h：%d 处 SDK 出处与 SDK 一致" % n)
PY

# zh_zcl.h 的 SDK 出处（第七步 b）：af.h 的位定义、app/zcl 下 XML 的那一行 —— 数值要在那一行（af.h 按 BIT(n) 算），名字在那一行（<code> / <profileId> 行没有名字）
python3 - "$COMP/include/zh_zcl.h" "$SDK" <<'PY'
import re, sys
h = open(sys.argv[1], encoding="utf-8").read()
S = sys.argv[2]
paths = {"af.h": S + "/protocol/zigbee/app/framework/include/af.h"}
bad, n = [], 0
for name, val, f, line, sym in re.findall(r"#define (ZH_\w+)\s+(0x[0-9A-Fa-f]+)u\s+/\* ([\w.-]+):(\d+) (\w+)", h):
    n += 1
    p = paths.get(f, S + "/app/zcl/" + f)
    try:
        L = open(p, encoding="utf-8").read().split("\n")[int(line) - 1]
    except (OSError, IndexError):
        bad.append("%s：%s:%s 打不开" % (name, f, line)); continue
    v = int(val, 16)
    if f == "af.h":
        bits = [int(b) for b in re.findall(r"BIT\((\d+)\)", L)]
        ok = ("#define " + sym) in L and sum(1 << b for b in bits) == v
    else:
        nums = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)", L)]
        ok = v in nums and (sym in L or "<code>" in L or "<profileId" in L)
    if not ok:
        bad.append("%s → %s:%s" % (name, f, line))
if n != 73 or bad:
    print("❌ zh_zcl.h 的 SDK 出处对不上（核了 %d 个）：%s" % (n, bad)); sys.exit(1)
print("zh_zcl.h：%d 处 SDK 出处（af.h / app/zcl 的 XML）与 SDK 一致" % n)
PY

# ④ EZSP 预言机：SDK 的三份源文件按它自己的习惯编（真 platform-header.h，-w）；ezsp_stub/ 补齐它要的三个生成头。
#    energy_scan_result、no_callbacks 在 ezsp-callbacks.c 里是强符号，用它自己的宏让出来；其余要截的回调在那里是 WEAK
EZ_SDK_DIRS=(protocol/zigbee protocol/zigbee/app/util/ezsp protocol/zigbee/stack/include platform/common/inc
             platform/service/legacy_hal/inc platform/radio/mac util/silicon_labs/silabs_core)
EZ_INC=(-I "$S/ezsp_stub"); EZ_SYS=(-I "$S/ezsp_stub")
for d in "${EZ_SDK_DIRS[@]}"; do EZ_INC+=(-I "$SDK/$d"); EZ_SYS+=(-isystem "$SDK/$d"); done
EZ_PH="-DPLATFORM_HEADER=\"$SDK/platform/service/legacy_hal/inc/platform-header.h\""
for f in ezsp.c ezsp-frame-utilities.c; do
    "$CC" -std=gnu11 -w -DEZSP_HOST "$EZ_PH" "${EZ_INC[@]}" -c "$EZSP_DIR/$f" -o "$OUT/$f.o"
done
"$CC" -std=gnu11 -w -DEZSP_HOST -DEZSP_APPLICATION_HAS_ENERGY_SCAN_RESULT_HANDLER -DEZSP_APPLICATION_HAS_NO_CALLBACKS "$EZ_PH" "${EZ_INC[@]}" \
    -c "$EZSP_DIR/ezsp-callbacks.c" -o "$OUT/ezsp-callbacks.c.o"
# 我们的测试照样 -Wall -Werror；SDK 头走 -isystem，不为它的告警背锅
"$CC" -std=gnu11 -Wall -Wextra -Werror "${SAN[@]}" -DEZSP_HOST "$EZ_PH" "${EZ_SYS[@]}" -I"$COMP/include" \
    -o "$OUT/ezsp_oracle" "$S/ezsp_oracle.c" "$COMP/zh_ezsp_core.c" \
    "$OUT/ezsp.c.o" "$OUT/ezsp-frame-utilities.c.o" "$OUT/ezsp-callbacks.c.o"
"$OUT/ezsp_oracle"

echo "全部通过"
