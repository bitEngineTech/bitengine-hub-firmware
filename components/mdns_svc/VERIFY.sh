#!/bin/bash
# components/mdns_svc/VERIFY.sh —— bash components/mdns_svc/VERIFY.sh
#
# ⚠️ 部分覆盖。2026-08-17 做 mDNS 多包应答补丁时建起来的，目前只锁那件事
# 与几条基本形状。docs/07 里 mdns_svc 的其余约束尚未转成断言。
PASS=0; FAIL=0
check() { if eval "$2" >/dev/null 2>&1; then echo "✅ $1"; ((PASS++)); else echo "❌ $1"; ((FAIL++)); fi; }
cd "$(dirname "$0")"
ROOT=../..

echo "--- 多包应答补丁必须在（2026-08-17）---"
# mdns 组件把一次应答塞进一个 1460 缓冲，放不下就静默丢弃：不置 TC、不报错、
# ANCOUNT 只数塞进去的那些。实测 187 字节/设备 → 第 8 个 Matter 设备起浏览
# 应答就不完整，外部表现是「设备在 Thread 网里活着、控制器里查无此人」。
# 只在私有检出里有的内部文档 / 工具（docs/、发布与产线脚本）：公开仓库不导出它们（tools/oss_export.py），
# 那里跳过而不是报红；私有检出里照常判。标志是 docs/开发索引.md 在不在。
internal() { if [ -f ../../docs/开发索引.md ]; then check "$@"; else echo "⏭  $1 —— 依赖内部文档 / 工具，本检出没有，跳过"; fi; }

check "补丁脚本存在"            "[ -f $ROOT/tools/patch_mdns.py ]"
check "构建期挂了补丁"          "grep -q 'patch_mdns.py' $ROOT/CMakeLists.txt"
check "补丁失败要中断构建"      "grep -q 'FATAL_ERROR.*mdns' $ROOT/CMakeLists.txt"
# 组件目录被 gitignore，只有本地打过才查得到；没解出来时跳过而不是判错
if [ -f $ROOT/managed_components/espressif__mdns/mdns_send.c ]; then
    check "补丁已落到组件里"    "python3 $ROOT/tools/patch_mdns.py --component-dir $ROOT/managed_components/espressif__mdns --check"
    check "包长仍是 1460"        "grep -q 'MDNS_MAX_PACKET_SIZE        1460' $ROOT/managed_components/espressif__mdns/private_include/mdns_private.h"
else
    echo "⏭️  managed_components 未解析，跳过组件内检查"
fi
# 抬 MAX_SERVICES 的前提就是补丁在；两者必须一起动
KV="python3 -c \"
import re,sys
def kv(f): return dict(re.findall(r'^(CONFIG_\\w+)=(\\S+)\\s*\$', open(f).read(), re.M))
d,c=kv('$ROOT/sdkconfig.defaults'),kv('$ROOT/sdkconfig')
ms,st='CONFIG_MDNS_MAX_SERVICES','CONFIG_MDNS_TASK_STACK_SIZE'
sys.exit(0 if (d.get(ms)==c.get(ms) and d.get(st)==c.get(st) and SEL) else 1)\""
check "MAX_SERVICES ≥ 128，sdkconfig 与 defaults 同步" "${KV/SEL/int(d[ms]) >= 128}"
# 组件在 mdns 任务栈上按服务数开变长数组（restart_all_pcbs / restart_pcb 各 4 B×全部服务，探测中再来一份）
check "MAX_SERVICES > 128 时 mdns 任务栈 ≥ 8192（栈在 PSRAM）" "${KV/SEL/int(d[ms]) <= 128 or int(d[st]) >= 8192}"
check "MAX_SERVICES ≤ 256（再往上卡在两处 O(N²)，见 docs/test_cases_scale_baseline.md「SRP 代播满载压测」）" "${KV/SEL/int(d[ms]) <= 256}"
# 四组补丁在原版上能打、幂等、组间不重叠（后打的改到先打的替换文本 = 重跑 configure 时构建停下）
PRISTINE=$(ls -d ~/.cache/Espressif/ComponentManager/service_*/espressif__mdns_1.11.0_* 2>/dev/null | head -1)
if [ -n "$PRISTINE" ]; then
    check "补丁在原版组件上可重放（--selftest）" "python3 $ROOT/tools/patch_mdns.py --selftest '$PRISTINE'"
else
    echo "⏭️  组件管理器缓存里没有 mdns 1.11.0 原版，跳过 --selftest"
fi

# SCALE ⑧ 的 IDLE 时刻来自 thread_br（ot_cpu_guard.c）的强定义，mdns 里是弱兜底：链错了外部零症状，只能看 ELF 里是哪一个
ELF=$ROOT/build/bitengine.elf
if [ -f "$ELF" ] && command -v riscv32-esp-elf-nm >/dev/null 2>&1; then
    check "链进来的是 ot_cpu_guard 的强定义 bitengine_idle_last_us（不是 mdns 里的弱兜底）" "riscv32-esp-elf-nm '$ELF' | grep -q ' T bitengine_idle_last_us\$'"
    # SVCIP ①（T9.34）：本机 A 记录的地址由 network_mgr 的强定义给；链成 mdns 里的弱兜底 = 静默退回上游行为（WiFi 地址打头）
    check "链进来的是 network_mgr 的强定义 bitengine_mdns_self_ipv4（不是 mdns 里的弱兜底）" "riscv32-esp-elf-nm '$ELF' | grep -q ' T bitengine_mdns_self_ipv4\$'"
else
    echo "⏭️  没有 build/bitengine.elf 或没 source export.sh，跳过链接检查"
fi

echo ""
echo "--- refresh 只重宣告自有服务（2026-10-07）---"
# 以前是 mdns_hostname_set(同名) = mdns_priv_restart_all_pcbs：全部服务（含每条代播的 Matter 记录）重新探测，
# 代播服务一多 mdns 任务就连续占满 CPU、任务看门狗 panic。理由见 mdns_svc_refresh 的注释。
REFRESH="awk '/^esp_err_t mdns_svc_refresh\\(void\\)/{f=1} f&&/^}/{exit} f' mdns_svc.c"
check "refresh 不碰主机名"              "$REFRESH | python3 $ROOT/tools/grep_code.py --absent -q 'mdns_hostname_set' /dev/stdin"
check "refresh 用 port_set 逐个重宣告"   "$REFRESH | python3 $ROOT/tools/grep_code.py -q 'mdns_service_port_set\\(' /dev/stdin"
# 7.6：init 里注册的自有服务类型与 refresh 表一一对应（加了服务忘了加进 refresh，它就再也不会被重宣告）
# 比对（类型，端口）对：refresh 靠"把端口设一遍"触发宣告，端口不一致它会每 30 s 把对外宣告的端口改掉
check "refresh 表覆盖 init 注册的每个自有服务（类型与端口都一致）" "python3 - <<'PY'
import re,sys
s=re.sub(r'/[*].*?[*]/|//[^\\n]*','',open('mdns_svc.c').read(),flags=re.S)
init=set(re.findall(r'mdns_service_add\\(\\s*\"[^\"]*\",\\s*\"(_[\\w-]+)\",\\s*\"_tcp\",\\s*([\\w]+)', s))
body=s.split('esp_err_t mdns_svc_refresh(void)')[1].split('\\n}\\n')[0]
ref=set(re.findall(r'\\{\\s*\"(_[\\w-]+)\",\\s*([\\w]+)\\s*\\}', body))
sys.exit(0 if init and init==ref else 1)
PY"

echo ""
echo "--- 基本形状 ---"
check "发布 _zigbee-bridge"     "grep -q '_zigbee-bridge' mdns_svc.c"
check "发布 _http"              "grep -q '_http' mdns_svc.c"
check "_meshcop 品牌覆盖"       "grep -q 'mdns_svc_brand_meshcop' mdns_svc.c"
check "品牌覆盖失败要可重试"    "awk '/mdns_svc_brand_meshcop/,/^}/' mdns_svc.c | grep -q 'return err'"
# _meshcop 的 xa/nn/xp 归 BR 库发布与维护（数据集等一变就重发）。我们自己写只会在服务发布前失败（每次开机三条 E 级
# "Service doesn't exist"）或重复库的值 —— 2026-10-22 删掉了 mdns_svc_update_meshcop_txt，别加回来。只许覆盖 vn/mn。
# ⚠️ 要 --strings：判据里有字符串字面量，grep_code 默认连字符串一起剥，不加就永远匹配不上（变异实测过空过）。
G=../../tools/grep_code.py
check "不自己写 _meshcop 的 xa/nn/xp（只覆盖 vn/mn 品牌）" "python3 $G --strings --absent -q 'mdns_svc_update_meshcop_txt|\"_meshcop\"\\s*,\\s*\"_udp\"\\s*,\\s*\"(xa|xp|nn)\"' mdns_svc.c ../thread_br/thread_br.c ../webui/otbr_api.c ../webui/rest_api.c"

echo ""
# ---- 发现服务的取舍（2026-08-22）----
# `_slzb-06._tcp` 已停止广播。它当初（97fe85d，2026-03-30）是唯一能让 HA 自动发现
# 我们 Zigbee 协调器的办法（靠冒充 SMLIGHT 设备），但：
#   · 对 ZHA 从来没起过作用 —— ZHA 按**实例名前缀** slzb-06* 匹配，我们叫 "bitEngine Hub"
#   · 正牌替代 _zigbee-coordinator._tcp 已于 13439b7 就位并实测通过
#   · 它实际效果是负的：HA 的 SMLIGHT 集成按服务类型匹配，于是用户看到一张
#     竞品品牌的卡片指向我们的设备，点进去还要密码
# ⚠️ /api2/* 端点**保留**，用户仍可手工添加 SMLIGHT 集成 —— 去掉的只是自动发现。
check "不再广播 _slzb-06"        "! grep -q 'mdns_service_add(.*_slzb-06' mdns_svc.c"
check "SMLIGHT 端点仍保留"       "[ $(grep -c 'api2' ../webui/smlight_compat.c) -ge 10 ]"
# 这两条是 _slzb-06 的正牌替代，掉了任何一条就等于把发现能力弄丢了
check "仍广播 _zigbee-coordinator" "grep -q 'mdns_service_add(.*_zigbee-coordinator' mdns_svc.c"
check "仍广播 _zigbee-bridge"      "grep -q 'mdns_service_add(.*_zigbee-bridge' mdns_svc.c"


# ---- 与 HA 集成的跨侧一致性（2026-08-23）----
# 这一组锁的是「固件广播的 TXT」与「HA 那边怎么匹配/怎么用它」之间的契约。
# 断链的表现全都是**静默的**：设备照常广播、HA 照常运行，只是那张发现卡片
# 再也不出现，没有任何一侧报错。这个项目已经在同一类事上翻过车
# （_zigbee-bridge vs _zigbee-coordinator，一字之差与 ZHA 擦肩而过）。
echo ""
echo "--- 与 HA 集成的跨侧契约 ---"

MANIFEST="$ROOT/ha_integration/custom_components/bitengine/manifest.json"
check "HA 集成 manifest 找得到" "test -f '$MANIFEST'"

# ★ model 大小写。HA 在匹配前对**属性值**做 .lower()
#   （homeassistant/components/zeroconf/discovery.py:103
#    `_memorized_fnmatch(prop_val.lower(), value)`），但**模式本身不做 lower**，
#   而 fnmatch 是大小写敏感的。
#   我们固件广播的是 model=bitEngine-Hub-C5（大写），manifest 里写的是
#   bitengine-hub*（小写）—— 能对上纯属这条 lower() 的功劳。
#   谁哪天"顺手对齐大小写"把 manifest 改成 bitEngine-Hub*，自动发现会**静默失效**。
check "model 模式必须全小写" "python3 -c \"
import json,sys
m=json.load(open('$MANIFEST'))['zeroconf'][0]['properties']['model']
sys.exit(0 if m==m.lower() else 1)\""

# 用 fnmatchcase 精确复刻 HA 的行为（HA 用 re.compile(fnmatch.translate(pat))，
# 大小写敏感），左边先 lower 模拟 HA 对属性值的处理。
check "固件 model 能被 manifest 匹中" "python3 -c \"
import json,re,sys
from fnmatch import fnmatchcase
mf=json.load(open('$MANIFEST'))['zeroconf'][0]['properties']['model']
src=open('mdns_svc.c').read()
m=re.search(r'\{\s*\\\"model\\\"\s*,\s*\\\"([^\\\"]+)\\\"', src)
sys.exit(0 if m and fnmatchcase(m.group(1).lower(), mf) else 1)\""

check "manifest 盯的服务类型与固件一致" "python3 -c \"
import json,sys
t=json.load(open('$MANIFEST'))['zeroconf'][0]['type']
src=open('mdns_svc.c').read()
sys.exit(0 if t=='_zigbee-bridge._tcp.local.' and '\\\"_zigbee-bridge\\\"' in src else 1)\""

# ZHA 那一侧：ZEROCONF_PROPERTIES_SCHEMA 要求 radio_type ∈ RadioType 且有
# serial_number，缺任一条 ZHA 直接 async_abort(reason='invalid_zeroconf_data') ——
# 用户什么也看不到。见 zha/config_flow.py 的 ZEROCONF_PROPERTIES_SCHEMA。
check "ZHA 必填 radio_type=ezsp" "grep -q '\"radio_type\",  *\"ezsp\"' mdns_svc.c"
check "ZHA 必填 serial_number"   "grep -q '\"serial_number\"' mdns_svc.c"
# ZHA 用 SRV 端口拼 socket://host:port，端口写错就是连一个不存在的口
check "coordinator 端口用桥接端口常量" \
      "awk '/_zigbee-coordinator/,/;/' mdns_svc.c | grep -q 'CONFIG_BITENGINE_TCP_PORT'"

# ---- 合成 mDNS 负载闸门（2026-09-15，扩容实验 C 方案）----
# 钉的是"出厂关得掉"和"撞墙时不漏记录"这两件事。
check "mdns_load 出厂默认关" \
    "grep -A2 'config BITENGINE_ENABLE_MDNS_LOAD' ../../main/Kconfig.projbuild | grep -q 'default n'"
check "sdkconfig 里 mdns_load 是关的" \
    "grep -q '# CONFIG_BITENGINE_ENABLE_MDNS_LOAD is not set' ../../sdkconfig"
check "整个实现被 Kconfig 包住" \
    "grep -q '#if CONFIG_BITENGINE_ENABLE_MDNS_LOAD' mdns_load.c"
check "两个端点都挂在编译期守卫里" \
    "[ \$(grep -c 'CONFIG_BITENGINE_ENABLE_MDNS_LOAD' ../webui/rest_api.c) -ge 3 ]"
# ⚠️ 撞墙（service_add 返 NO_MEM）时必须把刚加的 delegate host 撤掉，
#    否则每撞一次就漏一个主机记录，后续所有读数被污染 —— 这是本模块最容易写错的地方。
check "撞墙时回滚 delegate host" \
    "grep -A6 'add service.*撞墙' mdns_load.c | grep -q 'mdns_delegate_hostname_remove'"
# 名字由序号确定性推出 = 不需要跟踪表，也就没有"表与实际状态不同步"这个缺陷类
check "名字确定性推导（无跟踪表）" \
    "grep -q 'static void synth_names' mdns_load.c && python3 ../../tools/grep_code.py --absent --quiet 'static .*s_track' mdns_load.c"

# 对外规格里的三个设备容量数（Zigbee 直连 / Thread 直连 / Matter）被抄进了手册、docs/00 等七八处，正本是配置：
# 改了 CONFIG_MDNS_MAX_SERVICES 或网关自用的服务，跑这条就知道哪几处文档要跟着改（CLAUDE.md 7.6 第 3 条）
internal "对外规格的三个容量数与配置一致（tools/check_capacity_docs.py）" "python3 $ROOT/tools/check_capacity_docs.py"

echo ""
echo "--- T9.34 本机地址只宣告以太网服务 IP（SVCIP，2026-10-07）---"
# 补丁本身由上面的 --check / --selftest 管（含 SVCIP 的四条不变量）；这里钉跨组件的约定
check "SVCIP 补丁组在脚本里、且构建时会打" "grep -q '^SVCIP_PATCHES = \[' $ROOT/tools/patch_mdns.py && grep -q 'SVCIP_PATCHES, \"服务地址' $ROOT/tools/patch_mdns.py"
check "钩子返回值约定两侧一致（补丁里 0/1/2 = svc_policy.h 的 DEFAULT/FIXED/HOLD）" "grep -q 'BITENGINE_SELF_DEFAULT  0' $ROOT/tools/patch_mdns.py && grep -q 'BITENGINE_SELF_FIXED    1' $ROOT/tools/patch_mdns.py && grep -q 'BITENGINE_SELF_HOLD     2' $ROOT/tools/patch_mdns.py && grep -q 'SVC_MDNS_DEFAULT = 0' $ROOT/components/network_mgr/svc_policy.h && grep -q 'SVC_MDNS_FIXED   = 1' $ROOT/components/network_mgr/svc_policy.h && grep -q 'SVC_MDNS_HOLD    = 2' $ROOT/components/network_mgr/svc_policy.h"
check "出厂配置开着（BITENGINE_MDNS_SVC_IP_ONLY=y）" "grep -q '^CONFIG_BITENGINE_MDNS_SVC_IP_ONLY=y' $ROOT/sdkconfig"
# 2026-10-07 用户拍板：两口同网段时固定以太网口应答（不占与 Thread 分时的射频，WiFi 断连不切换应答口）
check "固定应答的是以太网口（bitengine_if_answers 选 MDNS_IF_ETH）" "awk '/^bool bitengine_if_answers\\(mdns_if_t tcpip_if\\)\$/{f=1} f{print} f&&/^}/{exit}' $ROOT/managed_components/espressif__mdns/mdns_netif.c | grep -q 'predef_if == MDNS_IF_ETH'"

echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
