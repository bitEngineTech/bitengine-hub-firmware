#!/bin/bash
# components/ezsp_bridge/VERIFY.sh
#
# 覆盖两件事：
#  1) docs/02 的核心设计约束（纯字节流透传、每客户端独立 ring buffer 等）
#  2) ncp_version.c 的 ASH/EZSP 线格式常量——每一条都能追到 SDK 源码的
#     具体文件行，不是凭记忆写的（Simplicity SDK v2025.6.3，~/src/mg24-fw/sdk）
#
# 字节级逻辑本身不靠 grep 验证，靠 test_host/run.sh 在主机上跑真代码。

cd "$(dirname "$0")" || exit 1

PASS=0; FAIL=0
check() {
    if eval "$2" > /dev/null 2>&1; then
        echo "✅ $1"; ((PASS++))
    else
        echo "❌ $1"; ((FAIL++))
    fi
}

echo "==== 文件与构建 ===="
for f in ezsp_bridge.c uart_bridge.c tcp_server.c usb_bridge.c client_buffer.c \
         bridge_stats.c ncp_version.c; do
    check "$f exists"                "[ -f $f ]"
    check "$f in CMakeLists"         "grep -q '$f' CMakeLists.txt"
done
# 只在私有检出里有的内部文档 / 工具（docs/、发布与产线脚本）：公开仓库不导出它们（tools/oss_export.py），
# 那里跳过而不是报红；私有检出里照常判。标志是 docs/开发索引.md 在不在。
internal() { if [ -f ../../docs/开发索引.md ]; then check "$@"; else echo "⏭  $1 —— 依赖内部文档 / 工具，本检出没有，跳过"; fi; }

check "ncp_version.h exported"       "[ -f include/ncp_version.h ]"

echo ""
echo "==== docs/02 核心约束 ===="
# 决策 1：纯字节流透传，不解析 ASH 帧。桥接路径里不许出现 0x7E 分帧/CRC 计算。
# （ncp_version.c 不算：它是独占 UART 时的一次性查询，不在转发路径上）
check "bridge does no ASH framing"   "! grep -q '0x7E\|crc' tcp_server.c uart_bridge.c"
check "TCP binds 0.0.0.0"            "grep -q 'INADDR_ANY\|0.0.0.0' tcp_server.c"
check "per-client ring buffer"       "grep -q 'client_buffer' tcp_server.c"

echo ""
echo "==== ASH 线格式常量（ash-protocol.h:39-51）===="
check "FLAG  = 0x7E"                 "grep -q 'ASH_FLAG *0x7E' ncp_version.c"
check "ESC   = 0x7D"                 "grep -q 'ASH_ESC *0x7D' ncp_version.c"
check "XON   = 0x11"                 "grep -q 'ASH_XON *0x11' ncp_version.c"
check "XOFF  = 0x13"                 "grep -q 'ASH_XOFF *0x13' ncp_version.c"
check "SUB   = 0x18"                 "grep -q 'ASH_SUB *0x18' ncp_version.c"
check "CAN   = 0x1A"                 "grep -q 'ASH_CAN *0x1A' ncp_version.c"
check "FLIP  = 0x20"                 "grep -q 'ASH_FLIP *0x20' ncp_version.c"
check "RST   = 0xC0"                 "grep -q 'ASH_CTRL_RST *0xC0' ncp_version.c"
check "RSTACK= 0xC1"                 "grep -q 'ASH_CTRL_RSTACK *0xC1' ncp_version.c"
check "ERROR = 0xC2"                 "grep -q 'ASH_CTRL_ERROR *0xC2' ncp_version.c"
check "LFSR seed 0x42 (ash-common.c:38)" "grep -q 'LFSR_SEED *0x42' ncp_version.c"
check "LFSR poly 0xB8 (ash-common.c:37)" "grep -q 'LFSR_POLY *0xB8' ncp_version.c"
# CRC 高字节先发（ash-common.c:98-101）——写反了对端每帧都 NAK
check "CRC high byte first"          "grep -q 'crc >> 8' ncp_version.c"
# 只有 DATA 帧随机化（ashRandomizeBuffer 的两个调用点都在 DATA 路径上）
check "randomize only DATA frames"   "grep -q 'if (ASH_IS_DATA(control)) ash_randomize' ncp_version.c"

echo ""
echo "==== EZSP 常量（ezsp-enum.h / ezsp-protocol.h）===="
check "version    id = 0x0000"       "grep -q 'EZSP_FRAME_ID_VERSION *0x0000' ncp_version.c"
check "getValue   id = 0x00AA"       "grep -q 'EZSP_FRAME_ID_GET_VALUE *0x00AA' ncp_version.c"
check "VERSION_INFO  = 0x11"         "grep -q 'EZSP_VALUE_VERSION_INFO *0x11' ncp_version.c"
check "protocol ver  = 0x12"         "grep -q 'EZSP_PROTOCOL_WANTED *0x12' ncp_version.c"
check "ext format ver= 0x01"         "grep -q 'EZSP_EXT_FORMAT_VERSION *0x01' ncp_version.c"
# 第一条 version 命令必须用 legacy 头（ezsp.c:525-531），否则 NCP 不认
check "version sent legacy (4 bytes)" "grep -q 'ezsp_txn(cmd, 4' ncp_version.c"
check "getValue sent extended (6)"    "grep -q 'ezsp_txn(gcmd, 6' ncp_version.c"
# getValue 的 status 是 4 字节 sl_status_t，不是 1 字节（command-functions.h:233）
check "getValue status is 32-bit"     "grep -q 'resp\[pi + 3\] << 24' ncp_version.c"
check "VERSION_INFO is 7 bytes"       "grep -q 'value_len != 7' ncp_version.c"

echo ""
echo "==== 安全/时序约束 ===="
# 有客户端时禁止发 ASH RST：会打断 Z2M/ZHA 的握手（health_monitor.c:72-83 同理）
check "refuses query with clients"   "grep -q 'active_clients > 0' ncp_version.c"
# 查询必须发生在 tcp_server_start() 之前，那是唯一无竞争的窗口
check "query before tcp_server_start" "[ \$(grep -n 'ncp_version_query(' ezsp_bridge.c | head -1 | cut -d: -f1) -lt \$(grep -n 'tcp_server_start();' ezsp_bridge.c | head -1 | cut -d: -f1) ]"
# 失败不能留旧值，成功要整体发布（避免 HTTP 线程读到半个结构体）
check "cache published atomically"   "grep -q 's_cache = v;' ncp_version.c"
check "no unbounded wait"            "! grep -q 'portMAX_DELAY' ncp_version.c"
check "every wait has a deadline"    "[ \$(grep -c 'deadline' ncp_version.c) -ge 3 ]"

echo ""
echo "==== 上报接线 ===="
check "status exposes ncp_version"   "grep -q 'ncp_version' ../webui/rest_api.c"
# 曾经这两个字段写死成 "EmberZNet" / "EmberZNet 9.0.1"，HA 的 SMLIGHT 集成把它
# 当真实版本显示。现在必须是 %s，值由 ncp_version_format() 提供。
check "smlight zb_version not hardcoded" "! grep -q 'zb_version.....EmberZNet' ../webui/smlight_compat.c"
check "smlight zb_version from NCP"      "grep -q 'ncp_version_format' ../webui/smlight_compat.c"

echo ""
echo "==== 主机侧协议测试 ===="
check "test_host present"            "[ -f test_host/run.sh ]"
check "host protocol tests pass"     "bash test_host/run.sh"

echo ""
echo "--- Task WDT 订阅/退订必须成对（2026-08-11 整板重启真因）---"
# 任务自删前不退订 = TWDT 表里留下指向已释放 TCB 的悬空句柄，5s 后 panic 重启整板。
# 间歇性：新任务 TCB 若复用同一块内存，句柄相同会被当成"已订阅"而侥幸不炸。
check "TWDT add/delete 成对"      "[ $(grep -c 'twdt_subscribe(' tcp_server.c) -eq $(grep -c 'twdt_unsubscribe(' tcp_server.c) ]"
check "无裸 esp_task_wdt_add"     "[ $(grep -c 'esp_task_wdt_add' tcp_server.c) -le 1 ]"
check "退订早于 exit 信号量"      "! grep -A2 'xSemaphoreGive(s_task_exit_sem)' tcp_server.c | grep -q 'twdt_unsubscribe'"
check "订阅查返回值"              "grep -A3 'esp_task_wdt_add(NULL)' tcp_server.c | grep -q 'ESP_ERR_INVALID_ARG'"

check "停桥时标记槽位无主"        "awk '/^void tcp_server_stop/,/^}/' tcp_server.c | grep -q 'wd_slot_offline'"

echo ""
echo "--- 不许再尝试从这里读 Zigbee 信道 ---"
# 读它必须先 write_config（给协议栈配表），否则 networkInit/getNetworkParameters
# 恒回 NOT_JOINED。2026-08-12 上板证实：我们报"没有网络"时 ZHA 正在 ch20 收发。
# 实现 write_config 等于造 EZSP 主机，违反 docs/02 决策 1。信道改由用户声明。
check "不发 getNetworkParameters"  "! grep -q '0x0028' ncp_version.c"
check "不发 networkInit"           "! grep -q '0x0017' ncp_version.c"
check "不导出 netparams API"       "! grep -q 'ncp_netparams' include/ncp_version.h"
check "头文件记着为什么不能读"     "grep -q 'write_config' include/ncp_version.h"
check "有客户端时拒绝查询"         "grep -q 'active_clients > 0' ncp_version.c"
# 0xFF 在帧内是普通数据字节（SDK 的 ashEncodeStuffByte 不转义它）。整帧无条件
# 跳过会丢一个字节 → CRC 失败 → 整条事务超时，且随载荷内容确定性复现。
check "0xFF 只在帧间当唤醒字节"    "grep -q 'b == ASH_WAKE && n == 0' ncp_version.c"
check "ASH_WAKE 不再是 switch 分支" "! grep -q 'case ASH_WAKE' ncp_version.c"

echo ""
echo "--- 桥接模式必须跨重启保持（2026-08-16）---"
# 此前 init 恒 s_mode = BRIDGE_MODE_LAN，写入端 config_mgr_set_bridge_mode() 零调用：
# 用户切到 USB/DISABLED 只活在 RAM，任何一次重启（含看门狗自愈）都悄悄回 LAN。
check "init 读回 sys.bridge_mode"  "awk '/^esp_err_t ezsp_bridge_init/,/^}/' ezsp_bridge.c | grep -q 'config_mgr_get_sys'"
check "init 不再硬编码 LAN"        "! awk '/^esp_err_t ezsp_bridge_init/,/^}/' ezsp_bridge.c | grep -q 's_mode  = BRIDGE_MODE_LAN;'"
check "起不来要回退 LAN"           "awk '/^esp_err_t ezsp_bridge_init/,/^}/' ezsp_bridge.c | grep -q 'falling back to LAN'"
# 落盘必须在用户意图层（rest_api 的 handle_mode），不能在 switch_mode：
# OTA / uart_bench 都会临时切 DISABLED 再恢复，落在这层会把临时态写进 NVS，
# OTA 中途掉电网关就永久带着 DISABLED 回来。
check "switch_mode 不落盘"         "! awk '/^esp_err_t ezsp_bridge_switch_mode/,/^}/' ezsp_bridge.c | grep -q 'config_mgr_set_bridge_mode'"

echo ""
echo "--- 断开客户端（2026-08-17 接线）---"
# 查快照拿下标、再按下标断开，两步之间槽位可能已被新连接复用（Z2M/ZHA 重连很快）
# → 踢掉无辜连接。按 IP 那条必须在同一次持锁内匹配+关闭。
check "有按 IP 断开的入口"         "grep -q 'tcp_server_disconnect_by_ip' tcp_server.c"
check "匹配与关闭同一次持锁"        "awk '/^esp_err_t tcp_server_disconnect_by_ip/,/^}/' tcp_server.c | grep -c 'xSemaphoreTake\|xSemaphoreGive' | grep -q '^2$'"
check "一条没命中回 NOT_FOUND"      "awk '/^esp_err_t tcp_server_disconnect_by_ip/,/^}/' tcp_server.c | grep -q 'ESP_ERR_NOT_FOUND'"
# 空槽位回 OK 会让端点对着空气回 200 ok
check "按下标断开也区分空槽位"      "awk '/^esp_err_t tcp_server_disconnect_client/,/^}/' tcp_server.c | grep -q 'ESP_ERR_NOT_FOUND'"

echo ""
echo "--- 只接一个上位机（2026-09-23）---"
# EZSP/ASH 点对点：第二个上位机连上时的 RST 会复位第一个正在用的 NCP。
# 谁要改回 2，先读 docs/02 §1 那段订正 —— "ASH 序列号会 NAK 其中一个"是错的。
check "MAX_TCP_CLIENTS 为 1"        "grep -qE '^#define MAX_TCP_CLIENTS[[:space:]]+1([[:space:]]|\$)' include/ezsp_bridge.h"
check "槽位满走 reject_hold 不直接关" "awk '/if \\(slot < 0\\)/,/}/' tcp_server.c | grep -q 'reject_hold_locked(fd)'"

echo ""
echo ""
echo "--- NCP 表容量必须读得到（2026-08-17）---"
# Zigbee 能接多少设备的约束全在 MG24 侧，C5 是纯字节流透传、0 可见性。
# H1 已核（Simplicity SDK v2025.6.3）：帧 0x0052，应答 status 是 **4 字节**。
check "读 getConfigurationValue"   "grep -q 'EZSP_FRAME_ID_GET_CONFIG 0x0052' ncp_version.c"
check "status 按 4 字节解"          "awk '/^static void read_ncp_config/,/^}/' ncp_version.c | grep -q 'resp\\[pi + 3\\] << 24'"
check "一次失败就收手"              "awk '/^static void read_ncp_config/,/^}/' ncp_version.c | grep -q '放弃读余下的表容量'"
check "表容量进 zigbee/status"      "grep -q 'ncp_tables' ../webui/rest_api.c"
internal "README 记了实测值"           "grep -q 'MAX_END_DEVICE_CHILDREN' ../../mg24/README.md"
internal "README 写明是上电默认值"     "grep -q '上电默认值' ../../mg24/README.md"

# ---- 重启前干净断开（2026-08-20）----
# 网关重启时不发 FIN，ZHA 的 bellows 会卡死不自愈（实测：网关 08-19 00:37 重启，
# 它挣扎到 08-20 15:47 后彻底放弃，tcp_clients 恒 0，只能重启 HA）。
check "有重启前断开函数"         "grep -q 'tcp_server_close_all_clients' tcp_server.c"
# 钉"注册了重启前断开"这件事，不钉具体函数名 —— 2026-08-21 为了给地址变化
# 复用，close_all_clients 加了 settle 参数，shutdown handler 改成包一层的
# shutdown_close_clients，原判据锁死了旧名字就成了假红。
check "挂到了 shutdown handler"  "grep -A3 'esp_register_shutdown_handler' tcp_server.c | grep -q 'close_all_clients\|close_clients'"
# ⚠️ 2026-08-22：这里原有三条判据钉住 `SO_LINGER` / `l_linger = 0` /
# `CLIENT_RST_SETTLE_MS`，理由是"必须发 RST 而不是优雅 FIN"。**那个前提是错的** ——
# CONFIG_LWIP_SO_LINGER 从未开启（sdkconfig:3653），lwipopts.h:1024 令
# LWIP_SO_LINGER=0，sockets.c:3466 的 case 整段被编掉，那行 setsockopt 是空操作，
# 发出去的一直是 FIN。也就是说那三条判据钉住的是**一段死代码的存在**，
# 是标准的恒绿判据：它永远通过，且通过时什么也没证明。
# 死代码已删，判据换成反向不变量 —— 防的是"有人又加一行自以为能发 RST 的 setsockopt"。
check "不要再加空操作的 SO_LINGER" "! grep -q '^[^*/]*setsockopt(.*SO_LINGER' tcp_server.c"
check "关闭后有沉降窗口"          "grep -q '^#define CLIENT_CLOSE_SETTLE_MS' tcp_server.c"
check "沉降窗口真的被用上"        "grep -q 'vTaskDelay(pdMS_TO_TICKS(CLIENT_CLOSE_SETTLE_MS))' tcp_server.c"
# 顺序不变量：esp_restart() 逆序执行 handler，所以 ezsp_bridge_init 必须晚于
# network_mgr_init —— 否则 eth PHY 会先被按住复位，FIN 永远发不出去。
# 以太网(PoE)是主部署形态，这不是理论问题。
NM=$(grep -n 'ret = network_mgr_init()' ../../main/main.c | head -1 | cut -d: -f1)
EZ=$(grep -n 'ret = ezsp_bridge_init()' ../../main/main.c | head -1 | cut -d: -f1)
check "init 顺序 ezsp 晚于 network_mgr（FIN 依赖网络还活着）" "[ -n \"$NM\" ] && [ -n \"$EZ\" ] && [ $NM -lt $EZ ]"


echo ""; echo "--- 地区下发（docs/14 第 7.5 节，2026-09-15）---"
check "customFrame 帧号 0x0047"                  "python3 ../../tools/grep_code.py -q 'EZSP_FRAME_ID_CUSTOM_FRAME +0x0047' ncp_version.c"
check "私有命令 0x7001 / 应答 0xF001"           "python3 ../../tools/grep_code.py -q 'XNCP_CMD_BE_SET_REGION +0x7001' ncp_version.c && python3 ../../tools/grep_code.py -q 'XNCP_RSP_BE_SET_REGION +0xF001' ncp_version.c"
check "0x7001 只从独占会话里发（sync_region 只被 ncp_session 调一次）" "[ \$(grep -c 'sync_region(&v, gw_region);' ncp_version.c) -eq 1 ]"
check "非 0 status 或空回复 → xncp=false"        "python3 ../../tools/grep_code.py -q 'status != 0 .* rl < 3' ncp_version.c"
check "应答校验 rsp_id/status/长度三件"          "python3 ../../tools/grep_code.py -q 'rsp_id != XNCP_RSP_BE_SET_REGION' ncp_version.c && python3 ../../tools/grep_code.py -q 'rl != 3 \+ XNCP_STATE_LEN' ncp_version.c"
check "国家码空格 → 空串（对外不吐空格）"        "grep -q \"st\[5\] == ' '\" ncp_version.c"
check "开机路径带网关地区调 query"               "python3 ../../tools/grep_code.py -q 'region = \(sys0 && sys0->radio_region\[0\]\) \? sys0->radio_region :' ezsp_bridge.c && python3 ../../tools/grep_code.py -q 'ncp_version_query\(region\)' ezsp_bridge.c"
# T9.31 ②：C5 单独重启时 MG24 多半还在带网，开机读 NCP（ASH RST）会把它的协议栈打掉 —— 只在整板上电、
# 没缓存、或缓存里的网关地区与配置不符时才读；读到的经回调落盘（ncp_cache.c）。
check "开机读 NCP 只在整板上电或缓存不可用时"    "python3 ../../tools/grep_code.py -q 'rst != ESP_RST_POWERON && ncp_cache_load\(&cached\) && cached.txp.xncp' ezsp_bridge.c && python3 ../../tools/grep_code.py -q 'strcmp\(cached.txp.gw_cc, region_cc\) == 0' ezsp_bridge.c"
check "读到的 NCP 读数经回调落盘"               "python3 ../../tools/grep_code.py -q 'ncp_version_set_persist_hook\(ncp_cache_save\)' ezsp_bridge.c"
check "落盘内容没变不写 NVS"                    "awk '/^void ncp_cache_save/,/^}/' ncp_cache.c | python3 ../../tools/grep_code.py -q 'memcmp\(&old, &b.v, sizeof\(old\)\) == 0\) return' /dev/stdin"
check "resync：DISABLED → claim → query → release" "grep -A22 '^esp_err_t ezsp_bridge_ncp_resync' ezsp_bridge.c | grep -q 'ezsp_bridge_switch_mode(BRIDGE_MODE_DISABLED)' && grep -A22 '^esp_err_t ezsp_bridge_ncp_resync' ezsp_bridge.c | grep -q 'ezsp_bridge_ota_claim' && grep -A30 '^esp_err_t ezsp_bridge_ncp_resync' ezsp_bridge.c | grep -q 'ezsp_bridge_ota_release'"
check "resync 拿不到独占时恢复原模式"            "grep -A16 '^esp_err_t ezsp_bridge_ncp_resync' ezsp_bridge.c | grep -q 'if (prev != BRIDGE_MODE_DISABLED) ezsp_bridge_switch_mode(prev)'"
check "主机测试覆盖 US / 清除 / 旧固件 / 别家 XNCP" "grep -q 'set_region_US' test_host/ncp_sim_test.c && grep -q 'set_region_clear' test_host/ncp_sim_test.c && grep -q '旧 NCP 固件' test_host/ncp_sim_test.c && grep -q '别家 XNCP' test_host/ncp_sim_test.c"
check "主机测试的向量文件与 MG24 侧共用"          "grep -q 'mg24/ncp/test_host/vectors.txt' test_host/ncp_sim_test.c && [ -f ../../mg24/ncp/test_host/vectors.txt ]"

echo ""; echo "--- 被拒连接不主动关（2026-09-23，A/B 见 docs/upstream/bellows-reject-ab.py）---"
# 为什么：accept 后立刻 close 是 bellows 的确定性卡死触发器。实测 bellows 1.0.2
# 上 11/12 次让 HA 记成永久 setup_error、10/12 次 unload 挂死。关闭必须由对端发起。
# ⚠️ 反向不变量用 grep_code.py（剥注释），否则会被上面那段解释"为什么不能 close"
#    的注释绊倒 —— 见 CLAUDE.md 7.4。
# ⚠️ awk 的起点必须钉死函数**定义**：只写 /^static void tcp_accept_task/ 会先撞上
#    文件顶部的前向声明，范围一路吃到别的函数，把 reject_hold_locked 里那个
#    兜底 close(fd) 也算进来（2026-09-23 第一版就这么假红过）。
# accept 任务里**允许**有 close(fd)，但只允许在停机分支（!s_running）里——那是
# 竞态闭合，不是拒绝。拒绝路径一个都不许有。判据：每个 close(fd) 往回 3 行内
# 必须有 s_running；离得远的就是拒绝路径又开始自己关连接了。
# ⚠️ 第一版写成"一个都不许有"，加了停机守卫之后立刻假红——不变量本身写错了。
_badclose=$(awk '/^static void tcp_accept_task\(void \*arg\)$/,/^}/' tcp_server.c |
            awk '/s_running/{g=NR} /close\(fd\);/{ if (NR-g>3) print NR }')
check "accept 任务里的 close(fd) 只出现在停机分支里（拒绝路径不许关）" \
      "[ -z \"$_badclose\" ]"
check "两处拒绝都走 reject_hold_locked"  "[ \$(grep -c 'reject_hold_locked(fd);' tcp_server.c) -eq 2 ]"
check "reject_hold_locked 有定义"        "grep -q '^static void reject_hold_locked(int fd)' tcp_server.c"
check "hold 的 fd 设成非阻塞"            "python3 ../../tools/grep_code.py -q 'F_SETFL, flags \\| O_NONBLOCK' tcp_server.c"
check "设非阻塞失败就不挂（否则持锁阻塞整座桥）" \
      "grep -A3 'if (flags < 0 || fcntl(fd, F_SETFL' tcp_server.c | grep -q 'close(fd)'"
check "有 fd 上限与时间上限"              "grep -q '^#define MAX_REJECT_HOLDS' tcp_server.c && grep -q '^#define REJECT_HOLD_CAP_S' tcp_server.c"
check "上限真的被用上"                    "python3 ../../tools/grep_code.py -q 'REJECT_HOLD_CAP_S' tcp_server.c"
check "stop() 里释放 holds"               "awk '/^void tcp_server_stop/,/^}/' tcp_server.c | grep -q 'reject_holds_close_all_locked()'"
# 停机竞态：accept() 之后隔着最长 3s 的探测避让，而 stop 只 join 3000ms。
# 错过清理的 fd 会在下次 start() 被"只抹不关"丢掉 → 永久泄漏。必须锁内复查。
check "登记前在锁内复查 s_running（否则停机竞态泄漏 fd）" \
      "python3 ../../tools/grep_code.py -q 'if \\(!s_running\\) \\{' tcp_server.c && awk '/^static void tcp_accept_task\\(void \\*arg\\)\$/,/^\\}/' tcp_server.c | grep -c 's_running' | grep -qv '^0\$'"
# 顺序不变量：service 必须在"桥不在 RUNNING 就 continue"之前，否则 MG24 升级
# 期间 accept 照样在拒绝连接，而 hold 的 fd 一个都回收不掉。
SVC=$(grep -n 'reject_holds_service();' tcp_server.c | tail -1 | cut -d: -f1)
GATE=$(grep -n 'if (ezsp_bridge_get_state() != BRIDGE_STATE_RUNNING) {' tcp_server.c | tail -1 | cut -d: -f1)
check "holds 的回收早于 RUNNING 早返回"   "[ -n \"$SVC\" ] && [ -n \"$GATE\" ] && [ $SVC -lt $GATE ]"

echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
