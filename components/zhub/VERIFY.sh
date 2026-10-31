#!/bin/bash
# components/zhub/VERIFY.sh —— zhub（直连型的 Zigbee 主机，docs/15_muse_direct_design_v1.md 第八节）设计合规的形状检查
#
#   cd components/zhub && bash VERIFY.sh        （必须先 cd 进组件目录：脚本里全是相对路径）
#
# M4 第一步只有 ASH 层的纯逻辑 zh_ash_core（8.3）。代码"有没有用"由 test_host/run.sh 证明：自写真值表 + 与本机 SDK
# 的 ash-common.c（编解码）和 ash-ncp.c（NCP 侧状态机）互通 —— SDK 代码只在测试时从本机路径编，不进仓库（Silicon Labs MSLA）。

PASS=0; FAIL=0
check() {
    if eval "$2" > /dev/null 2>&1; then
        echo "✅ $1"; ((PASS++))
    else
        echo "❌ $1"; ((FAIL++))
    fi
}
G="python3 ../../tools/grep_code.py"

# ---- 组件骨架与配置隔离（架构 5.8 R-3：HA 型里不编任何源文件）----
# 只在私有检出里有的内部文档 / 工具（docs/、发布与产线脚本）：公开仓库不导出它们（tools/oss_export.py），
# 那里跳过而不是报红；私有检出里照常判。标志是 docs/开发索引.md 在不在。
internal() { if [ -f ../../docs/开发索引.md ]; then check "$@"; else echo "⏭  $1 —— 依赖内部文档 / 工具，本检出没有，跳过"; fi; }

check "CMakeLists 只在 CONFIG_BITENGINE_ENABLE_ZHUB 下给源文件（源文件名在 if 与 endif 之间，注释里的不算）" "python3 -c \"
import re,sys; s=re.sub(r'#.*', '', open('CMakeLists.txt').read())
m=re.search(r'if\(CONFIG_BITENGINE_ENABLE_ZHUB\)(.*?)endif\(\)', s, re.S)
outside=re.sub(r'if\(CONFIG_BITENGINE_ENABLE_ZHUB\).*?endif\(\)', '', s, flags=re.S)
sys.exit(0 if m and all('\\\"%s\\\"' % f in m.group(1) for f in ('zh_ash_core.c', 'zh_ezsp_core.c', 'zh_ezsp_ses.c', 'zh_host_core.c', 'zhub.c')) and '.c' not in outside else 1)\""
check "zh_ash_core 不依赖 ESP-IDF（test_host 原样编它）" "$G --strings --absent -q '#\s*include\s*[<\"](esp_|freertos|sdkconfig|driver/)' zh_ash_core.c include/zh_ash_core.h"
check "VERIFY 与主机测试在位" "[ -f test_host/run.sh ] && [ -f test_host/zh_ash_test.c ]"

# ---- 8.3 常量：与 SDK ash-protocol.h 逐个对上（每个都带出处注释，H1）----
for c in 'ZH_ASH_FLAG +0x7E' 'ZH_ASH_ESC +0x7D' 'ZH_ASH_XON +0x11' 'ZH_ASH_XOFF +0x13' 'ZH_ASH_SUB +0x18' 'ZH_ASH_CAN +0x1A' 'ZH_ASH_WAKE +0xFF' \
         'ZH_ASH_FLIP +0x20' 'ZH_ASH_CTRL_RST +0xC0' 'ZH_ASH_CTRL_RSTACK +0xC1' 'ZH_ASH_CTRL_ERROR +0xC2' 'ZH_ASH_LFSR_SEED +0x42' 'ZH_ASH_LFSR_POLY +0xB8' \
         'ZH_ASH_VERSION +2' 'ZH_ASH_DATA_MAX +220' 'ZH_ASH_DATA_MIN +3'; do
    check "常量 $c（带 ash-protocol.h / ezsp-protocol.h 出处）" "grep -E '#define $c\b.*(ash-protocol\.h|ezsp-protocol\.h|ash-common\.c|em2xx)' include/zh_ash_core.h"
done
check "复位原因 SOFTWARE = 11（em2xx-reset-defs.h）：发了 RST 只认它" "grep -E '#define ZH_ASH_RESET_SOFTWARE +11\b.*em2xx-reset-defs\.h' include/zh_ash_core.h && $G -q 'ZH_ASH_RESET_SOFTWARE' zh_ash_core.c"

# ---- 编解码 ----
check "CRC-CCITT 0x1021 / 初值 0xFFFF，高字节先发（紧跟数据的那个字节是 crc >> 8）" "$G -q '0xFFFF' zh_ash_core.c && $G -q '0x1021' zh_ash_core.c && $G -q 'raw\[1 \+ len\] = \(uint8_t\)\(crc >> 8\);' zh_ash_core.c && $G -q 'raw\[2 \+ len\] = \(uint8_t\)\(crc & 0xFF\);' zh_ash_core.c"
check "只有 DATA 帧的数据域做随机化" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_ash_core.c').read())
n=len(re.findall(r'zh_ash_randomize\(', S))
sys.exit(0 if n >= 3 and re.search(r'ZH_ASH_IS_DATA\(', S) else 1)\""
check "0xFF（WAKE）只在帧与帧之间丢掉，帧内是普通数据（ncp_version.c 栽过）" "$G -q 'b == ZH_ASH_WAKE && .*len == 0' zh_ash_core.c"
check "RTS/CTS 下 XON / XOFF 不入帧（ash-common.c 的主机分支）" "$G -q 'case ZH_ASH_XON:' zh_ash_core.c && $G -q 'case ZH_ASH_XOFF:' zh_ash_core.c"
check "CAN 丢弃当前帧、SUB 标记整帧作废" "$G -q 'case ZH_ASH_CAN:' zh_ash_core.c && $G -q 'case ZH_ASH_SUB:' zh_ash_core.c"

# ---- 状态机（8.3 + SDK ash-host.c 的语义）----
check "发 RST 前、开始重传时先发一个 CAN（1A C0 38 BC 7E；同 ash-host.c 的 FLG_CAN）" "$G -q 'out\[n\+\+\] = ZH_ASH_CAN;' zh_ash_core.c && $G -q 'a->flags = F_RST \| F_CAN;' zh_ash_core.c && $G -q 'a->flags \|= F_RETX \| F_CAN;' zh_ash_core.c"
check "RSTACK：版本必须是 2，原因必须是 SOFTWARE，否则不算连上" "$G -q 'ZH_ASH_VERSION' zh_ash_core.c && $G -q '!= ZH_ASH_RESET_SOFTWARE' zh_ash_core.c"
check "ackNum 只认窗口内的（WITHIN_RANGE(ack_rx, ackNum, frm_tx)）" "$G -q 'within_range\(a->ack_rx, ack, a->frm_tx\)' zh_ash_core.c"
check "重传的 DATA 带 rFlag（0x08）" "$G -q 'ZH_ASH_RFLAG' zh_ash_core.c && grep -E '#define ZH_ASH_RFLAG +0x08' include/zh_ash_core.h"
check "乱序：第一次乱序回 NAK 并进拒收状态、拒收中不再 NAK；收到期望的帧出拒收" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_ash_core.c').read())
r=re.search(r'static void reject_frame\(zh_ash_t \*a\)\n\{(.*?)\n\}', S, re.S).group(1)
sys.exit(0 if '!(a->flags & F_REJ)' in r and 'a->flags |= F_REJ | F_NAK;' in r and 'a->flags &= (uint16_t)~(F_REJ | F_NAK);' in S else 1)\""
check "ACK 超时自适应：(7×旧 + 4×往返)/8，超时翻倍，夹在上下限" "$G -q 'period \* 7' zh_ash_core.c && $G -q 'rtt \* 4' zh_ash_core.c && $G -q 'ack_period \* 2' zh_ash_core.c"
check "连续超时到上限就判链路断（交给上层走复位）" "$G -q 'timeouts >= a->cfg.max_timeouts' zh_ash_core.c"
check "连接后再收到 RSTACK = NCP 自己重启了，报给上层" "$G -q 'ZH_ASH_EV_NCP_RESET' zh_ash_core.c"
check "ERROR 帧报给上层（带错误码）" "$G -q 'ZH_ASH_EV_NCP_ERROR' zh_ash_core.c"
check "发送窗口取配置，默认 1（停等，8.3）：看的是 zh_ash_cfg_default 里的赋值" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_ash_core.c').read())
d=re.search(r'void zh_ash_cfg_default\(zh_ash_cfg_t \*c\)\n\{(.*?)\n\}', S, re.S).group(1)
sys.exit(0 if re.search(r'c->tx_k = 1;', d) else 1)\""
check "有待发的东西时 next_timeout 返回 0（第一轮审计：reset / send 之后按定时器睡会睡死）" "$G -q 'if \(zh_ash_tx_pending\(a\)\) return 0;' zh_ash_core.c"

# ---- EZSP 层（8.4；M4 第二步）：纯逻辑 zh_ezsp_core ----
check "zh_ezsp_core 不依赖 ESP-IDF（在直连型源文件里由第一条 CMake 检查管）" "$G --strings --absent -q '#\s*include\s*[<\"](esp_|freertos|sdkconfig|driver/)' zh_ezsp_core.c include/zh_ezsp_core.h"
check "协议号 0x12、每个帧 ID 常量都带 ezsp-enum.h 的行号（附录 D，不手抄）" "grep -qE '#define ZH_EZSP_PROTOCOL_VERSION +0x12\b.*ezsp-protocol\.h:33' include/zh_ezsp_core.h && [ \$(grep -cE '^#define ZH_EZSP_ID_[A-Z0-9_]+ +0x[0-9A-F]{4}' include/zh_ezsp_core.h) -ge 40 ] && [ \$(grep -E '^#define ZH_EZSP_ID_[A-Z0-9_]+ +0x[0-9A-F]{4}' include/zh_ezsp_core.h | grep -cvE 'ezsp-enum\.h:[0-9]+') -eq 0 ]"
check "解析：方向不对（命令回显）、保留位、截断都拒；溢出只是标志（ezsp.c responseReceived）" "$G -q 'ZH_EZSP_E_DIRECTION' zh_ezsp_core.c && $G -q 'ZH_EZSP_E_RESERVED' zh_ezsp_core.c && $G -q 'ZH_EZSP_E_TRUNCATED' zh_ezsp_core.c && $G -q 'ZH_EZSP_FCH_RESERVED_MASK' zh_ezsp_core.c"
check "每个解码器都要求参数恰好读完（CRC 结构性漏洞：少一个字节、CRC 仍对的帧要拒）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_ezsp_core.c').read())
fns=dict(re.findall(r'\nint (zh_ezsp_dec_\w+)\([^)]*\)\n\{(.*?)\n\}', S, re.S))
decl=set(re.findall(r'\bint (zh_ezsp_dec_\w+)\(', strip_comments(open('include/zh_ezsp_core.h').read())))
bad=[n for n,b in fns.items() if 'return fin(' not in b]
sys.exit(0 if len(decl) >= 24 and decl == set(fns) and not bad else 1)\""
check "首条 version 用旧式 3 字节头，其余扩展头（fc_hi 0x01）" "$G -q 'zh_ezsp_enc_version_legacy' zh_ezsp_core.c && $G -q 'ZH_EZSP_FCH_FORMAT_V1' zh_ezsp_core.c"
check "主机测试拿 SDK 的 ezsp.c 当预言机（编码逐字节、回包与回调的解析逐字段对照）" "grep -q 'ezsp.c' test_host/run.sh && [ -f test_host/sdk_interop/ezsp_oracle.c ]"
check "预言机覆盖头的分类全排列，且把\"我们比 SDK 严\"的格子单独断言（溢出洗白 / 原因码 0、0x34）" "grep -q 't_classify' test_host/sdk_interop/ezsp_oracle.c && grep -q 'predicted' test_host/sdk_interop/ezsp_oracle.c && grep -q 'SL_ZIGBEE_EZSP_ERROR_OVERFLOW' test_host/sdk_interop/ezsp_oracle.c"
check "run.sh 跑 zh_ezsp_test（严格长度 / 帧 ID / 编码参数校验），UBSan 一报就失败" "grep -q 'zh_ezsp_test' test_host/run.sh && grep -q 'fno-sanitize-recover=all' test_host/run.sh"
check "解码器自己再查一遍头（回显 / 截断 / 保留位），旧式头只给 version" "$G -q 'ZH_EZSP_FC_RESPONSE\) == 0\) return ZH_EZSP_E_DIRECTION' zh_ezsp_core.c && [ \$(python3 ../../tools/grep_code.py 'return ZH_EZSP_E_DIRECTION' zh_ezsp_core.c | wc -l) -ge 2 ] && $G -q 'fr->legacy && id != ZH_EZSP_ID_VERSION' zh_ezsp_core.c"
check "入网窗口必须有期限：permitJoining(0xFF) = 永久开放，编码器拒（设计 8.6、14.2）" "$G -q 'duration == 0xFF\) return 0' zh_ezsp_core.c && grep -q 'permit_joining(b, sizeof(b), 6, 255) == 0' test_host/zh_ezsp_test.c"
check "run.sh 逐个核对帧 ID 注释里的 ezsp-enum.h 行号（行号写错 VERIFY 看不出，要对着 SDK）" "grep -q 'ezsp-enum.h 行号' test_host/run.sh"
check "EZSP 解码器的帧 ID 白名单：只回状态 / 空回包 / send 三类通用解码器不许借道" "$G -q 'in_list\\(id, k_status_only' zh_ezsp_core.c && $G -q 'in_list\\(id, k_empty' zh_ezsp_core.c && $G -q 'ZH_EZSP_ID_SEND_UNICAST && id != ZH_EZSP_ID_SEND_BROADCAST' zh_ezsp_core.c"
check "setSourceRouteDiscoveryMode（0x005A）的编解码在位、回包是 4 B 的剩余毫秒数（不是状态），预言机覆盖（第五步）" "grep -qE '#define ZH_EZSP_ID_SET_SOURCE_ROUTE_DISCOVERY_MODE +0x005A +/\\* ezsp-enum\\.h:929' include/zh_ezsp_core.h && $G -q 'zh_ezsp_dec_source_route_discovery_mode\\(' zh_ezsp_core.c && grep -q 'zh_ezsp_enc_set_source_route_discovery_mode' test_host/sdk_interop/ezsp_oracle.c && grep -q 'zh_ezsp_dec_source_route_discovery_mode' test_host/sdk_interop/ezsp_oracle.c"
check "incomingMessageHandler 消息后面还有一个字节（源路由开销，SDK 主机在分发函数之外另取：ezsp.c:752-757、af-host.c:810-814；第五步首次上板真设备的上报条条被拒）：解码器读它、真 NCP 帧的回归在位、预言机也取它" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_ezsp_core.c').read())
b=re.search(r'int zh_ezsp_dec_incoming_message\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S).group(1)
i=b.find('zh_ezsp_r_ptr(&r, l)'); j=b.find('zh_ezsp_r_u8(&r)', i)
sys.exit(0 if 0 <= i < j and 'return fin(&r)' in b else 1)\" && grep -q 't_real_ncp_frames' test_host/zh_ezsp_test.c && grep -q 'getSourceRouteOverhead' test_host/sdk_interop/ezsp_oracle.c"
check "zh_ezsp_core 不分配内存" "$G --absent -q '\\bmalloc\\(|\\bcalloc\\(|\\bfree\\(' zh_ezsp_core.c"

# ---- EZSP 会话层（8.4 收发规则、8.13；M4 第三步）：纯逻辑 zh_ezsp_ses ----
check "zh_ezsp_ses 不依赖 ESP-IDF、不分配内存" "$G --strings --absent -q '#\s*include\s*[<\"](esp_|freertos|sdkconfig|driver/)' zh_ezsp_ses.c include/zh_ezsp_ses.h && $G --absent -q '\\bmalloc\\(|\\bcalloc\\(|\\bfree\\(' zh_ezsp_ses.c"
check "握手：连上后会话自己发首条 version（旧式头），协议号是 0x12 才 READY，否则停住报 mismatch" "$G -q 'zh_ezsp_enc_version_legacy' zh_ezsp_ses.c && $G -q '== ZH_EZSP_PROTOCOL_VERSION' zh_ezsp_ses.c && $G -q 'ZH_SES_EV_VERSION_MISMATCH' zh_ezsp_ses.c"
check "C15：应答计时从 ASH 确认之后才起算（ops.unacked），另有交给 ASH 后久不确认的兜底" "$G -q 'ops\.unacked\(' zh_ezsp_ses.c && $G -q 'ack_guard_ms' zh_ezsp_ses.c && $G -q 'rsp_timeout_ms' zh_ezsp_ses.c"
check "Z59：只有回显后才重发、只给幂等命令；应答超时绝不重发（超时即复位链路：NCP 可能还有应答在重传，再发就撞缓冲）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_ezsp_ses.c').read())
fns=dict(re.findall(r'\\nstatic \\w+ (on_echo|on_timeout)\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S))
ok = len(fns) == 2 and 'idempotent' in fns['on_echo'] and 'retry(' in fns['on_echo'] and 'retry(' not in fns['on_timeout'] and 'stop(' in fns['on_timeout']
sys.exit(0 if ok else 1)\""
check "每次发送（含重发）都换新 seq，迟到的旧应答按 seq 当过期丢掉" "$G -q 'buf\[0\] = s->seq_next\+\+' zh_ezsp_ses.c && $G -q 'stale\+\+' zh_ezsp_ses.c"
check "空闲保活发 nop、NCP 说还有回调待取时发 callback 取（Z58、E13）" "$G -q 'zh_ezsp_enc_nop' zh_ezsp_ses.c && $G -q 'zh_ezsp_enc_callback' zh_ezsp_ses.c && $G -q 'zh_ezsp_pending_cb' zh_ezsp_ses.c && $G -q 'keepalive_ms' zh_ezsp_ses.c"
check "会话判断链路不可用时报 RESET_LINK（保活 / 应答超时 / 握手失败 / ASH 久不确认或交不出去 / 内部命令回显用完 / VERSION_NOT_SET），而且总是报（回调里 link_up 被忽略）" "[ \$(python3 ../../tools/grep_code.py 'ZH_SES_EV_RESET_LINK' zh_ezsp_ses.c | wc -l) -ge 1 ] && $G -q 'ZH_SES_INVALID_VERSION_NOT_SET' zh_ezsp_ses.c"
check "命令了结后整块清零、了结时不把整块命令拷到栈上（setInitialSecurityState 带网络密钥，Z17）" "$G -q 'memset\\(c, 0, sizeof\\(\\*c\\)\\)' zh_ezsp_ses.c && $G --absent -q 'zh_ses_cmd_t \\w+ = s->' zh_ezsp_ses.c && grep -q 'key_in_session' test_host/zh_ses_test.c"
check "LINK_DOWN 只给从没交给 ASH 的命令（交出去过的回 UNKNOWN：可能执行了，Z59）" "$G -q 'm->tries \\? ZH_SES_R_UNKNOWN : ZH_SES_R_LINK_DOWN' zh_ezsp_ses.c && grep -q 'LINK_DOWN 的确定没执行' test_host/sdk_interop/ses_interop.c"
check "正向证据接口（Z47）只在 zh_ses_rx / on_version 里更新（配上的应答、合法回调），超时 / 停住 / 了结路径不碰它，也不用 t_idle" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_ezsp_ses.c').read())
fns=re.findall(r'\\n(?:static )?\\w+ (\\w+)\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S)
users=sorted(n for n,b in fns if re.search(r'\\balive\\(s, now\\)', b))
sys.exit(0 if users == ['on_version', 'zh_ses_rx'] and 'zh_ses_alive_evidence' in S else 1)\""
check "头文件写明线程契约（无锁，所有入口与 ops 同一任务串行；跨任务模型在 zh_ash 那一步定，CHECKLIST Z60）" "grep -q '线程契约' include/zh_ezsp_ses.h && grep -q 'Z60' include/zh_ezsp_ses.h && grep -q '^| Z60 ' CHECKLIST.md"
check "callback / noCallbacks 是会话自己的：submit 拒收" "$G -q 'id == ZH_EZSP_ID_CALLBACK \\|\\| id == ZH_EZSP_ID_NO_CALLBACKS\\) return ZH_SES_E_ARG' zh_ezsp_ses.c"
check "NCP 替身把原地应答撞缓冲当硬错误（会话的应答超时就是为了不让它发生）" "grep -q '原地应答撞缓冲' test_host/sdk_interop/ncp_stub.c"
check "主机测试：会话真值表 + 与 SDK 真 NCP 状态机的端到端（回显、噪声、非幂等命令恰好执行一次）" "[ -f test_host/zh_ses_test.c ] && [ -f test_host/sdk_interop/ses_interop.c ] && grep -q 'zh_ses_test' test_host/run.sh && grep -q 'ses_interop' test_host/run.sh"

# ---- 编排层（8.2 合并后的单任务、8.5 开头一段、8.13；M4 第四步）：纯逻辑 zh_host_core ----
check "zh_host_core 不依赖 ESP-IDF、不分配内存" "$G --strings --absent -q '#\s*include\s*[<\"](esp_|freertos|sdkconfig|driver/)' zh_host_core.c include/zh_host_core.h && $G --absent -q '\\bmalloc\\(|\\bcalloc\\(|\\bfree\\(' zh_host_core.c"
check "链路级转移只在 tick 里做：link_up 只在 zh_host_tick，ASH 复位只在 do_reset（会话 / ASH 的回调里不许 link_up、不在 ASH 自己的调用栈里复位它）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_host_core.c').read())
fns=re.findall(r'\\n(?:static )?[\\w ]+?\\**(\\w+)\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S)
up=sorted(n for n,b in fns if 'zh_ses_link_up(' in b); rst=sorted(n for n,b in fns if 'zh_ash_reset(' in b)
cb=[b for n,b in fns if n in ('on_ash_event','on_ses_event','on_ses_result','on_ses_callback','on_ash_frame')]
bad=[b for b in cb if re.search(r'\\b(do_reset|zh_ash_reset|zh_ses_link_up|zh_ash_stop|handle_fail)\\(', b)]
sys.exit(0 if up == ['zh_host_tick'] and rst == ['do_reset'] and len(cb) == 5 and not bad else 1)\""
check "不建网、不碰密钥 / 表容量 / 功率（红线 4、Z11、Z14、Z15）：编排层不用这些编码器；zhub.c 一条协议命令都不自己发（只放行三条只读诊断：readCounters / getCurrentSecurityState / getChildData）；ALLOW_JOINS 只出现在窗口打开那一步（ZH_HOST_TC_POLICY_OPEN）" "$G --absent -q 'zh_ezsp_enc_(form_network|set_initial_security_state|leave_network|set_value|custom_frame|start_scan|set_radio_power)\\b' zh_host_core.c zhub.c && $G --absent -q 'zh_ezsp_enc_(?!read_counters\\b|get_child_data\\b|get_current_security_state\\b)\\w+\\(' zhub.c && python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_host_core.c').read())
fns=re.findall(r'\\n(?:static )?[\\w ]+?\\**(\\w+)\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S)
users=sorted(n for n,b in fns if 'ZH_HOST_TC_POLICY_OPEN' in b or 'ZH_EZSP_DECISION_ALLOW_JOINS' in b)
sys.exit(0 if users == ['submit_win'] and 'ZH_EZSP_DECISION_ALLOW_JOINS' not in S else 1)\" && grep -qE '^#define ZH_HOST_TC_POLICY_OPEN +\(ZH_EZSP_DECISION_ALLOW_JOINS \| ZH_EZSP_DECISION_ALLOW_UNSECURED_REJOINS\)' include/zh_host_core.h"
check "networkInit 之前的初始化序列只有 k_init 一张表（第五步，设计 8.5）：三个运行时配置（STACK_PROFILE、SECURITY_LEVEL、第七步 b 加的 MAC 间接发送超时 —— 不写表容量，红线 4）→ 7 条策略（表 8.5-2）→ 端点 1（表 8.5-3）；setConfigurationValue / addEndpoint 只在 submit_want 里编一次、参数取自 k_init；setPolicy 在 submit_want 里一次（k_init），另外只在入网窗口的开 / 关里各一次（第六步）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_host_core.c').read())
tbl=re.search(r'k_init\\[\\] = \\{(.*?)\\};', S, re.S).group(1)
rows=re.findall(r'\\{ *(STEP_\\w+), *(\\w+), *(\\w+), *(\\w+) *\\}', tbl)
want=[('STEP_CFG','ZH_EZSP_CFG_STACK_PROFILE','ZH_HOST_STACK_PROFILE','false'),('STEP_CFG','ZH_EZSP_CFG_SECURITY_LEVEL','ZH_HOST_SECURITY_LEVEL','false'),
      ('STEP_CFG','ZH_EZSP_CFG_INDIRECT_TRANSMISSION_TIMEOUT','ZH_HOST_INDIRECT_TX_TIMEOUT_MS','true'),
      ('STEP_POLICY','ZH_EZSP_POLICY_TRUST_CENTER','ZH_HOST_TC_POLICY_CLOSED','false'),
      ('STEP_POLICY','ZH_EZSP_POLICY_TC_REJOINS_USING_WELL_KNOWN_KEY','ZH_HOST_TC_WELL_KNOWN_REJOINS','false'),
      ('STEP_POLICY','ZH_EZSP_POLICY_TC_KEY_REQUEST','ZH_EZSP_DECISION_ALLOW_TC_KEY_REQUESTS_AND_SEND_CURRENT_KEY','false'),
      ('STEP_POLICY','ZH_EZSP_POLICY_APP_KEY_REQUEST','ZH_EZSP_DECISION_DENY_APP_KEY_REQUESTS','false'),
      ('STEP_POLICY','ZH_EZSP_POLICY_BINDING_MODIFICATION','ZH_EZSP_DECISION_DISALLOW_BINDING_MODIFICATION','false'),
      ('STEP_POLICY','ZH_EZSP_POLICY_MESSAGE_CONTENTS_IN_CALLBACK','ZH_EZSP_DECISION_MESSAGE_TAG_ONLY_IN_CALLBACK','false'),
      ('STEP_POLICY','ZH_EZSP_POLICY_POLL_HANDLER','ZH_EZSP_DECISION_POLL_HANDLER_IGNORE','false'),
      ('STEP_ENDPOINT','ZH_HOST_ENDPOINT','0','false')]
sub=re.search(r'\\nstatic void submit_want\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S).group(1)
win=re.search(r'\\nstatic void submit_win\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S).group(1)
ok = rows == want and all(len(re.findall(r'zh_ezsp_enc_%s\\(' % e, S)) == 1 and ('zh_ezsp_enc_%s(' % e) in sub for e in ('set_config','add_endpoint'))
ok = ok and len(re.findall(r'zh_ezsp_enc_set_policy\\(', sub)) == 1 and len(re.findall(r'zh_ezsp_enc_set_policy\\(', win)) == 2 and len(re.findall(r'zh_ezsp_enc_set_policy\\(', S)) == 3
ok = ok and len(re.findall(r'k_init\\[h->init_step\\]', sub)) >= 1
sys.exit(0 if ok else 1)\""
check "信任中心策略：窗口外只许重新入网（ALLOW_UNSECURED_REJOINS，同 SDK 框架关窗口与 herdsman；bellows 窗口外留着 ALLOW_JOINS，与设计\"只在窗口内允许新设备加入\"相反），用公开密钥的 TC 重新入网写死不许（0）；策略 / 取值常量都带 ezsp-enum.h 行号（run.sh 对着 SDK 核）" "grep -qE '^#define ZH_HOST_TC_POLICY_CLOSED +ZH_EZSP_DECISION_ALLOW_UNSECURED_REJOINS +/\\*' include/zh_host_core.h && grep -qE '^#define ZH_HOST_TC_WELL_KNOWN_REJOINS +0u +/\\*' include/zh_host_core.h && [ \$(grep -cE '^#define ZH_EZSP_(POLICY|DECISION)_\\w+ +0x[0-9A-F]{2}u +/\\* ezsp-enum\\.h:[0-9]+' include/zh_host_core.h) -eq 14 ]"
check "端点 1（表 8.5-3）：HA 0x0104、IAS CIE 0x0400 版本 0；输入 Basic / Identify / Time，输出 Identify / OnOff / Level / Color / DoorLock / WindowCovering / Thermostat / IAS Zone / IAS WD；OTA 0x0019 两边都没有（第七步能回\"没有镜像\"时加进输入簇）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_host_core.c').read()); h=open('include/zh_host_core.h').read()
def arr(n): return [int(x,16) for x in re.findall(r'0x([0-9A-Fa-f]{4})', re.search(r'k_%s\\[\\] = \\{(.*?)\\};' % n, S, re.S).group(1))]
ok = arr('ep_in') == [0x0000,0x0003,0x000A] and arr('ep_out') == [0x0003,0x0006,0x0008,0x0300,0x0101,0x0102,0x0201,0x0500,0x0502]
ok = ok and all(re.search(r'#define %s +%s\\b' % (n,v), h) for n,v in (('ZH_HOST_ENDPOINT','1u'),('ZH_HOST_EP_PROFILE','0x0104u'),('ZH_HOST_EP_DEVICE','0x0400u'),('ZH_HOST_EP_VERSION','0u')))
sys.exit(0 if ok else 1)\""
check "多对一路由只在网络 UP 之后发（herdsman 注释：按 SDK 在 NETWORK_UP 之后）：setConcentrator / setSourceRouteDiscoveryMode 只在 UP 之后的序列（zh_host_up_step_t）里；序列只从 start_up_steps 起，它只在 enter_running（net == UP 时）与 net_up 里调；参数常量带 SDK 出处（run.sh 核）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_host_core.c').read())
fns=re.findall(r'\\n(?:static )?[\\w ]+?\\**(\\w+)\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S)
callers=sorted(n for n,b in fns if 'start_up_steps(h)' in b)
want_up=sorted(n for n,b in fns if 'want = ZH_HOST_WANT_UP' in b)
er=dict(fns)['enter_running']
sub=dict(fns)['submit_want']
c=re.findall(r'zh_ezsp_enc_set_concentrator\\(([^;]*)\\);', S)
ok = callers == ['enter_running','net_up'] and 'if (h->net == ZH_NET_UP) start_up_steps(h);' in er and set(want_up) <= {'start_up_steps','up_result','zh_host_tick'}
ok = ok and len(c) == 1 and all(k in c[0] for k in ('true','ZH_HOST_CONC_TYPE','ZH_HOST_CONC_MIN_S','ZH_HOST_CONC_MAX_S','ZH_HOST_CONC_ROUTE_ERR','ZH_HOST_CONC_DELIVERY_FAIL','ZH_HOST_CONC_MAX_HOPS'))
ok = ok and 'zh_ezsp_enc_set_concentrator(' in sub and 'zh_ezsp_enc_set_source_route_discovery_mode(b, sizeof(b), 0, ZH_HOST_MTORR_MODE)' in sub
sys.exit(0 if ok else 1)\""
check "入网窗口（第六步，设计 8.6）：permitJoining / 广播 / 清临时链路密钥只在 submit_win 里编；时长 ≤ ZH_HOST_PERMIT_MAX_S（254，af-types.h：255 = 永久）；Mgmt_Permit_Joining_req 发给 0xFFFC、TC significance 1；关窗那几步里恢复信任中心策略（ZH_HOST_TC_POLICY_CLOSED）与清临时链路密钥都在" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_host_core.c').read()); h=open('include/zh_host_core.h').read()
fns=dict(re.findall(r'\\n(?:static )?[\\w ]+?\\**(\\w+)\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S))
win=fns['submit_win']
enc=lambda e: sorted(n for n,b in fns.items() if ('zh_ezsp_enc_%s(' % e) in b)
ok = enc('permit_joining') == ['submit_win'] and enc('send_broadcast') == ['submit_win'] and enc('clear_transient_link_keys') == ['submit_win']
ok = ok and 'ZH_HOST_TC_POLICY_CLOSED' in win and 'ZH_ZDO_BCAST_ROUTERS' in win and 'ZH_ZDO_PERMIT_JOINING_REQ' in win and 'ZH_ZDO_TC_SIGNIFICANCE' in win
pj=fns['zh_host_permit_join']
ok = ok and re.search(r'seconds > ZH_HOST_PERMIT_MAX_S\\) return ZH_PJ_INVALID', pj) is not None
ok = ok and all(re.search(r'#define %s +%s' % (n,v), h) for n,v in (('ZH_HOST_PERMIT_MAX_S','254u'),('ZH_ZDO_BCAST_ROUTERS','0xFFFCu'),('ZH_ZDO_PERMIT_JOINING_REQ','0x0036u'),('ZH_ZDO_TC_SIGNIFICANCE','1u')))
sys.exit(0 if ok else 1)\""
check "入网窗口的状态跟着链路走：链路复位 / 挂起（clear_round）窗口回 CLOSED（NCP 重启后 k_init 又把信任中心写回窗口外的取值，第五步第二轮审计）；网络掉了正开着的窗口走关窗；到期在 tick 里关；关窗时恢复信任中心策略被拒要重试、不许跳过，连着被拒 ZH_HOST_WIN_POLICY_TRIES 次复位链路（第六步审计）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_host_core.c').read())
fns=dict(re.findall(r'\\n(?:static )?[\\w ]+?\\**(\\w+)\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S))
ok = 'h->win_state = ZH_WIN_CLOSED' in fns['clear_round'] and 'win_net_gone(h)' in fns['net_down'] and 'win_net_gone(h)' in fns['stack_status']
ok = ok and re.search(r'ZH_WIN_OPEN && expired\\(now_ms, h->t_win_end\\)', fns['zh_host_tick']) is not None
wr=fns['win_result']
ok = ok and re.search(r'WC_POLICY_CLOSED\\) +\\{.*?ZH_HOST_WIN_POLICY_TRIES.*?fail_later\\(h, ZH_HOST_FAIL_WIN_POLICY.*?win_retry = true', wr, re.S) is not None
sys.exit(0 if ok else 1)\""
check "入网窗口总时长上限（用户 2026-10-22 定按默认做）：滚动 60 min 里最多开 ZH_HOST_PERMIT_HOUR_CAP_S（600 s）；剩的不够 30 s 回 ZH_PJ_QUOTA，够就开、截到剩的；早关的只算真开了的时长；处理函数把 QUOTA 回成 rate_limited 并带几分钟后能再开" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_host_core.c').read()); h=open('include/zh_host_core.h').read()
fns=dict(re.findall(r'\\n(?:static )?[\\w ]+?\\**(\\w+)\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S))
pj=fns['zh_host_permit_join']
ok = 'ZH_PJ_QUOTA' in pj and 'win_quota_left_s(' in pj and re.search(r'#define ZH_HOST_PERMIT_HOUR_CAP_S +600u', h) and re.search(r'#define ZH_HOST_PERMIT_HOUR_MS +3600000u', h)
ok = ok and 'win_rec_close(' in fns['win_start_close'] and 'win_hist_full(h, now_ms)' in pj
Z=strip_comments(open('zhub.c').read())
t=re.search(r'\\nstatic void zhub_task\\(void \\*arg\\)\\n\\{(.*?)\\n\\}', Z, re.S).group(1)
ok = ok and re.search(r'\\n        zh_host_expire_records\\(&s_z->h, now_ms\\(\\)\\);', t) is not None
C=strip_comments(open('../hub_core/hub_cmds.c').read())
ok = ok and re.search(r'case ZH_PJ_QUOTA:', C) is not None
sys.exit(0 if ok else 1)\""
check "UP 之后的读数与多对一路由是尽力而为：失败只记账、过 ZH_HOST_UP_RETRY_MS 再试同一步，绝不因此复位链路（up_result 里不调 fail_later；bellows 也只打警告）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_host_core.c').read())
b=re.search(r'\\nstatic void up_result\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S).group(1)
sys.exit(0 if 'fail_later(' not in b and 'up_later(' in b and 'ZH_HOST_UP_RETRY_MS' in S else 1)\""
check "networkInit 位图与状态码带 SDK 出处（sl_zigbee_types.h / sl_status.h；行号对不对由 run.sh 对着 SDK 核）" "python3 -c \"
import re,sys; h=open('include/zh_host_core.h').read()
want=[('ZH_SL_OK','0x0000'),('ZH_SL_NETWORK_UP','0x0015'),('ZH_SL_NETWORK_DOWN','0x0016'),('ZH_SL_NOT_JOINED','0x0017'),('ZH_SL_TC_SWAP_CHANGED','0x0C10'),('ZH_SL_TC_SWAP_SAME','0x0C11')]
ok=all(re.search(r'#define %s +%su +/\\* sl_status\\.h:\\d+' % (n,v), h) for n,v in want)
ok=ok and re.search(r'#define ZH_HOST_NET_INIT_BITMASK 0x0003u +/\\* sl_zigbee_types\\.h:642、644', h)
sys.exit(0 if ok else 1)\""
check "退避（8.13）：连续失败按表递增、连续 RUNNING 满 ZH_HOST_STABLE_MS 才清零；进退避前 ASH 先静音（handle_fail 里 zh_ash_stop 在置 BACKOFF 之前）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_host_core.c').read())
f=re.search(r'\\nstatic void handle_fail\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S).group(1)
i=f.find('zh_ash_stop(&h->ash)'); j=f.find('h->state = ZH_HOST_BACKOFF')
sys.exit(0 if 0 <= i < j and 'k_backoff_ms[' in S and 'ZH_HOST_STABLE_MS' in S else 1)\""
check "networkInit 每次 NCP 启动只发一次：只在握手后走完初始化序列（init_result）时要，别处一律不要（Z59；第一轮审计 ❌2）；序列只从握手完成（on_ses_event）开始" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_host_core.c').read())
fns=re.findall(r'\\n(?:static )?[\\w ]+?\\**(\\w+)\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S)
users=sorted(n for n,b in fns if 'ZH_HOST_WANT_NET_INIT' in b and 'want' in b and 'submit' not in n)
cfg=sorted(n for n,b in fns if 'want = ZH_HOST_WANT_INIT' in b)
sys.exit(0 if users == ['init_result'] and cfg == ['on_ses_event'] and 'ZH_HOST_FAIL_NET_DOWN' in S and 'zh_ezsp_enc_network_state' in S else 1)\""
check "自己 link_down（收摊）时交回的结果不算失败（teardown 标志），回调里只记账" "$G -q 'h->teardown = true' zh_host_core.c && $G -q 'if \\(h->teardown\\) return' zh_host_core.c"
check "NOT_JOINED 只记\"没有网络\"，绝不自动建网（迁移 / 建网在 8.5.1、8.12）" "$G -q 'ZH_SL_NOT_JOINED' zh_host_core.c && $G -q 'ZH_NET_NONE' zh_host_core.c"
check "主机测试：编排层真值表 + 与 SDK 真 NCP 状态机的端到端（上电、NCP 不回、NCP 重启、应用层挂死、挂起 / 恢复）" "[ -f test_host/zh_host_test.c ] && [ -f test_host/sdk_interop/host_interop.c ] && grep -q 'zh_host_test' test_host/run.sh && grep -q 'host_interop' test_host/run.sh"
check "zh_ash_stop：回 DOWN、不报事件、不清统计（退避期间 ASH 必须安静）" "grep -q 'void zh_ash_stop' include/zh_ash_core.h && grep -q 't_stop' test_host/zh_ash_test.c"

# ---- 设备层（8.6 入网回调、8.9 设备库、8.12 ③ 被动学习；M4 第七步 a）：纯逻辑 zh_dev ----
check "zh_dev 不依赖 ESP-IDF、不分配内存；CMake 只在直连型给它" "$G --strings --absent -q '#\s*include\s*[<\"](esp_|freertos|sdkconfig|driver/)' zh_dev.c include/zh_dev.h && $G --absent -q '\\bmalloc\\(|\\bcalloc\\(|\\bfree\\(' zh_dev.c && python3 -c \"
import re,sys; s=re.sub(r'#.*', '', open('CMakeLists.txt').read())
m=re.search(r'if\(CONFIG_BITENGINE_ENABLE_ZHUB\)(.*?)endif\(\)', s, re.S)
sys.exit(0 if m and '\\\"zh_dev.c\\\"' in m.group(1) else 1)\""
check "设备记录按设计 8.9 的 zdev_rec_t（ver 1、packed、IEEE / nwk / 节点类型 / 标志 / 厂商码 / 厂商 / 型号 / 端点 / 能力 / ZoneType / 怪癖），编译期钉住大小" "python3 -c \"
import re,sys; h=open('include/zh_dev.h').read()
b=re.search(r'typedef struct __attribute__\(\(packed\)\) \{(.*?)\} zh_dev_rec_t;', h, re.S)
ok = b and all(k in b.group(1) for k in ('uint8_t  ver;','ieee[8]','nwk;','node_type;','flags;','manuf_code;','manuf[24]','model[24]','sw_build_crc;','n_ep;','ep[3]','caps;','zone_type;','quirk_id;'))
ok = ok and re.search(r'_Static_assert\(sizeof\(zh_dev_rec_t\)', h) and re.search(r'#define ZH_DEV_REC_VER +1u', h)
sys.exit(0 if ok else 1)\""
check "被动学习（8.12 ③）：不认识的 nwk 先查 NCP 的地址表（lookupEui64ByNodeId），查不到再发 ZDO IEEE_addr_req；同一个 nwk 两次查询至少隔 ZH_DEV_LOOKUP_GAP_MS（10 s）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_dev.c').read()); h=open('include/zh_dev.h').read()
ok = 'zh_ezsp_enc_lookup_eui64(' in S and 'ZH_ZDO_IEEE_ADDR_REQ' in S and 'ZH_DEV_LOOKUP_GAP_MS' in S and re.search(r'#define ZH_DEV_LOOKUP_GAP_MS +10000u', h)
ok = ok and S.index('zh_ezsp_enc_lookup_eui64(') < S.index('ZH_ZDO_IEEE_ADDR_REQ')
sys.exit(0 if ok else 1)\""
check "建记录只认信任中心放行的入网（被拒的 DENY_JOIN 不建；第六步两轮审计 ❌ 的判据沿用）、全 0 / 全 FF 的 EUI64 不认" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_dev.c').read())
f=re.search(r'\\nvoid zh_dev_on_tc_join\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S).group(1)
ok = 'decision == ZH_JOIN_DENY' in f and 'eui_valid(' in S and re.search(r'static bool eui_valid\\(', S)
sys.exit(0 if ok else 1)\""
check "只在结构变化时落盘（8.9）：ops.save 只经 dev_save 一处调；收到消息只更新内存里的 last_seen / 信号" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_dev.c').read())
fns=dict(re.findall(r'\\n(?:static )?[\\w ]+?\\**(\\w+)\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S))
users=sorted(n for n,b in fns.items() if 'ops.save(' in b)
rx=fns.get('zh_dev_on_rx','')
sys.exit(0 if users == ['dev_save'] and 'dev_save(' not in rx.split('learn_')[0] else 1)\""
check "IEEE_addr_req 的 APS 选项照 SDK 同类先例（不认识的设备发 IEEE_addr_req：device-table-tracking.c handleUnknownDevice 只用 RETRY；第一轮审计 H1）；行号由 run.sh 核" "grep -qE '#define ZH_APS_OPTS_UNICAST +0x0040u +/\\* device-table-tracking\\.c:[0-9]+ handleUnknownDevice' include/zh_dev.h"
check "zh_host 的应用命令队列优先级最低：want 槽与入网窗口都空着才交（设备层的查询不跟初始化 / 窗口抢）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_host_core.c').read())
t=re.search(r'\\nvoid zh_host_tick\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S).group(1)
sys.exit(0 if t.count('submit_app(') == 1 and re.search(r'zh_ses_can_submit\\(&h->ses\\)\\) \\{\\s*if \\(h->want != ZH_HOST_WANT_NONE\\) submit_want\\(h, now_ms\\);\\s*else if \\(h->win_want\\) submit_win\\(h, now_ms\\);\\s*else if \\(h->app_n\\) submit_app\\(h, now_ms\\);', t) else 1)\""
check "zhub 不依赖 hub_core：存取与事件经 hub_core 登记的钩子（zhub_set_hooks），zhub.c 不 include hub_* 头" "$G --strings --absent -q '#\s*include\s*[<\"]hub_' zhub.c zh_dev.c && $G -q 'zhub_set_hooks\\(' ../hub_core/hub_zigbee.c && ! grep -q 'hub_core' CMakeLists.txt"
check "设备表给别的任务读：zhub_dev_list 在锁里拷一份（zhub 任务改完才发布）；锁拿不到回 -1，hub_core 回 unavailable 不说成没有设备（第二轮审计）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zhub.c').read()); C=strip_comments(open('../hub_core/hub_cmds.c').read()); Z=strip_comments(open('../hub_core/hub_zigbee.c').read())
f=re.search(r'\\nint zhub_dev_list\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S)
ok = f and 'xSemaphoreTake(s_dev_lock' in f.group(1) and 'return -1;' in f.group(1)
R=open('../hub_core/hub_cmds.c').read()
ok = ok and 'return HUB_ZB_BUSY;' in Z and C.count('HUB_ZB_BUSY') >= 2 and len(re.findall(r'HUB_ZB_BUSY\\)[^;]*fail\\(call, .unavailable.', R)) >= 1 and R.count('LIST_RC_BUSY') >= 2
sys.exit(0 if ok else 1)\""
check "列表的年龄读的时候才算（设计 6.1；第二轮审计 ❌：没人说话时快照不重发，last_seen 停在 0、永远在线）：zhub_dev_list 按 t_seen 现算 age_ms；zh_dev_tick 把 t_seen 封在 ZH_DEV_AGE_CAP_MS 以内（32 位毫秒回绕）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zhub.c').read()); D=strip_comments(open('zh_dev.c').read())
f=re.search(r'\\nint zhub_dev_list\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S).group(1)
t=re.search(r'\\nvoid zh_dev_tick\\([^)]*\\)\\n\\{(.*?)\\n\\}', D, re.S).group(1)
ok = re.search(r'out\\[i\\]\\.age_ms = now - out\\[i\\]\\.t_seen;', f) and 'ZH_DEV_AGE_CAP_MS' in t and 'd->dirty = true;' in t
sys.exit(0 if ok else 1)\""
check "zh_dev_tick 每圈都跑（挂起 / 没拿到 UART 时封顶也不能停）、时刻现取（修复复核 ❌：用这一圈开头的 now，回调里写的 t_seen 比它晚，被当成很老封掉）" "[ \$(python3 ../../tools/grep_code.py 'zh_dev_tick\\(' zhub.c | wc -l) -eq 1 ] && python3 ../../tools/grep_code.py -q 'zh_dev_tick\\(&s_z->dev, now_ms\\(\\)\\);' zhub.c && grep -q '_Static_assert(ZH_DEV_AGE_CAP_MS' include/zh_dev.h && python3 ../../tools/grep_code.py -q 'const int32_t age = \\(int32_t\\)\\(now_ms - x->t_seen\\);' zh_dev.c"
check "读不出来不是没有（第二轮审计 ❌）：load 钩子三态，ERR 的格子 zh_dev_reserve、这次开机不分配给新设备；hub_zigbee.c 的超时 / 读错 / 格式更新回 ZHUB_LOAD_ERR" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zhub.c').read()); Z=strip_comments(open('../hub_core/hub_zigbee.c').read()); D=strip_comments(open('zh_dev.c').read())
ok = re.search(r'if \\(r == ZHUB_LOAD_ERR\\) \\{\\s*\\(void\\)zh_dev_reserve\\(', S) and 'return ZHUB_LOAD_ERR;' in Z
ok = ok and 'if (d->dev[i].used || d->dev[i].reserved) continue;' in D
sys.exit(0 if ok else 1)\""
check "一台设备的增 / 离只出一条事件（第二轮审计 ❌）：hub_events.c 不再按入网计数出 zigbee_device_*；离开 / 回来归 AVAILABILITY（6.7，同 thread_member_left）" "$G --strings --absent -q 'zigbee_device_' ../hub_core/hub_events.c && python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
Z=strip_comments(open('../hub_core/hub_zigbee.c').read())
ok = all(re.search(r'case ZH_DEV_EV_%s:((?!break;).)*?cls = HUB_EVC_AVAILABILITY;' % e, Z, re.S) for e in ('LEFT','BACK'))
sys.exit(0 if ok else 1)\" && grep -q 'kind = \"zigbee_device_left\"' ../hub_core/hub_zigbee.c"
check "存盘不拖住 zhub 任务（ASH 也在里面）：hk_save 用 hub_store_put_wait(…, 0)；开机加载超时重试一次、整体有预算（第一轮审计）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
Z=strip_comments(open('../hub_core/hub_zigbee.c').read())
f=re.search(r'\\nstatic void hk_save\\([^)]*\\)\\n\\{(.*?)\\n\\}', Z, re.S).group(1)
l=re.search(r'\\nstatic zhub_load_t hk_load\\([^)]*\\)\\n\\{(.*?)\\n\\}', Z, re.S).group(1)
ok = re.search(r'hub_store_put_wait\\([^;]*, 0\\);', f) and 'hub_store_put(' not in f
ok = ok and 'LOAD_RETRY_MS' in l and 'LOAD_BUDGET_US' in l and l.count('hub_store_get(') == 2
sys.exit(0 if ok else 1)\""
check "device.health 的 devices.total 带上 Zigbee 设备（附录 A：how many devices it manages；第二轮审计 ❌）" "python3 ../../tools/grep_code.py -q 'total \\+= \\(unsigned long\\)zn' ../hub_core/hub_cmds.c && python3 ../../tools/grep_code.py -q 'hub_zigbee_count\\(\\)' ../hub_core/hub_cmds.c"
check "hub.devices.list / hub.device.get 带上 Zigbee 设备（id = zb-<16 位小写十六进制 IEEE>，附录 A），只在直连型编" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=open('../hub_core/hub_zigbee.c').read(); C=strip_comments(open('../hub_core/hub_cmds.c').read())
ok = 'zb-%02x%02x%02x%02x%02x%02x%02x%02x' in S and 'hub_zigbee_write_list(' in C and 'hub_zigbee_write_device(' in C
z=C.index('hub_zigbee_write_list('); pre=C[:z]
ok = ok and pre.rfind('#if CONFIG_BITENGINE_ENABLE_ZHUB') > pre.rfind('#endif')
sys.exit(0 if ok else 1)\""

# ---- 访谈（设计 8.6；M4 第七步 b）：zh_dev 里的访谈 + zh_zcl 最小编解码 ----
check "zh_zcl 纯逻辑：不依赖 ESP-IDF、不分配内存；CMake 只在直连型给它；run.sh 跑它的真值表 + 模糊（带 SDK 的 types.xml 逐个核类型长度）" "$G --strings --absent -q '#\s*include\s*[<\"](esp_|freertos|sdkconfig|driver/)' zh_zcl.c include/zh_zcl.h && $G --absent -q '\\bmalloc\\(|\\bcalloc\\(|\\bfree\\(' zh_zcl.c && python3 -c \"
import re,sys; s=re.sub(r'#.*', '', open('CMakeLists.txt').read())
m=re.search(r'if\(CONFIG_BITENGINE_ENABLE_ZHUB\)(.*?)endif\(\)', s, re.S)
sys.exit(0 if m and '\\\"zh_zcl.c\\\"' in m.group(1) else 1)\" && grep -q 'zh_zcl_test.*types.xml' test_host/run.sh"
check "访谈步骤（8.6）：NODE_DESC → ACTIVE_EP → SIMPLE_DESC → BASIC → ZONE → CIE → ENROLL（M5 第四步）；三对 ZDO 簇号与 ZCL 常量都带 SDK 出处（run.sh 核）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
h=open('include/zh_dev.h').read(); D=strip_comments(open('zh_dev.c').read())
e=re.search(r'ZH_IV_NONE = 0,\s*ZH_IV_NODE,\s*ZH_IV_ACTIVE,\s*ZH_IV_SIMPLE,\s*ZH_IV_BASIC,\s*ZH_IV_ZONE,\s*ZH_IV_CIE,\s*ZH_IV_ENROLL,\s*ZH_IV_BIND,\s*ZH_IV_REPORT,\s*ZH_IV_FAILED,', strip_comments(h))
ok = e and all(re.search(r'#define %s +0x%04Xu +/\\* zigbee-device-stack\\.h:\\d+' % (n, v), h) for n, v in
     (('ZH_ZDO_NODE_DESC_REQ',2),('ZH_ZDO_NODE_DESC_RSP',0x8002),('ZH_ZDO_SIMPLE_DESC_REQ',4),('ZH_ZDO_SIMPLE_DESC_RSP',0x8004),('ZH_ZDO_ACTIVE_EP_REQ',5),('ZH_ZDO_ACTIVE_EP_RSP',0x8005)))
ok = ok and all(k in D for k in ('ZH_ZDO_NODE_DESC_REQ','ZH_ZDO_ACTIVE_EP_REQ','ZH_ZDO_SIMPLE_DESC_REQ','zh_zcl_enc_read_attrs(','ZH_CL_IAS_ZONE','ZH_IAS_ZONE_TYPE'))
sys.exit(0 if ok else 1)\""
check "访谈节奏（8.6）：每步最多 3 次、每次等 5 s、整体 30 min；同时只有一条访谈请求在途（iv_busy）；休眠 / 还不知道的设备只在它最近一条消息之后 ZH_IV_AWAKE_MS 内发" "grep -qE '#define ZH_IV_TRIES +3u' include/zh_dev.h && grep -qE '#define ZH_IV_STEP_TIMEOUT_MS +5000u' include/zh_dev.h && grep -qE '#define ZH_IV_TOTAL_MS +1800000u' include/zh_dev.h && python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
D=strip_comments(open('zh_dev.c').read())
f=re.search(r'\\nstatic bool iv_can_talk\\([^)]*\\)\\n\\{(.*?)\\n\\}', D, re.S)
g=re.search(r'\\nstatic bool iv_always_on\\([^)]*\\)\\n\\{(.*?)\\n\\}', D, re.S)
ok = f and g and 'ZH_IV_AWAKE_MS' in f.group(1) and 'iv_always_on(x)' in f.group(1) and 'ZH_DEVF_RX_ON' in g.group(1) and 'ZH_DEVF_ANNCED' in g.group(1)
ok = ok and re.search(r'if \(d->iv_busy \|\| !d->net_up\) return;', D) and 'ZH_IV_STEP_TIMEOUT_MS' in D and 'ZH_IV_TOTAL_MS' in D and 'ZH_IV_TRIES' in D
sys.exit(0 if ok else 1)\""
check "Basic 两个一组读、缺了 / 回 0x89 / 默认应答报错改成一个一个读（一次读四个最坏 103 B 超 APS 载荷；zigpy 同法）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
D=strip_comments(open('zh_dev.c').read())
T=open('test_host/zh_dev_test.c').read()
ok = 'iv_single = true' in D and 'ZH_ZCL_ST_UNSUP_ATTR' in D and 'ZH_ZCL_CMD_DEFAULT_RSP' in D and re.search(r'x->iv_single \? 1u? : 2u?', D)
ok = ok and re.search(r'0x05, 0x00, 0x89 \}.*?\n.*?iv_single', T, re.S) and 'D18 两个一组被默认应答拒：改单读' in T
sys.exit(0 if ok else 1)\""
internal "访谈的 ZDO / ZCL 单播用 SDK 框架默认 APS 选项（集中器那一支 0x1040，config.h:306-309）；只开 RETRY 的恰好两处：IEEE_addr_req（device-table 先例）、主动发现的 LQI 表请求（zdo-cli.c:296-298 先例）" "grep -qE '#define ZH_APS_OPTS_DEFAULT +0x1040u +/\\* config\\.h:30[0-9]-30[0-9]' include/zh_dev.h && [ \$(python3 ../../tools/grep_code.py '\\.options = ZH_APS_OPTS_DEFAULT' zh_dev.c | wc -l) -ge 1 ] && [ \$(python3 ../../tools/grep_code.py '\\.options = ZH_APS_OPTS_UNICAST' zh_dev.c | wc -l) -eq 2 ] && grep -q 'zdo-cli.c:296-298' zh_dev.c && grep -A1 'void zdoMgmtLqiCommand' ~/src/mg24-fw/sdk/protocol/zigbee/app/framework/cli/zdo-cli.c >/dev/null && sed -n 298p ~/src/mg24-fw/sdk/protocol/zigbee/app/framework/cli/zdo-cli.c | grep -q 'SL_ZIGBEE_APS_OPTION_RETRY'"
check "访谈完成：结果落盘一次、标 INTERVIEWED、发 IDENTIFIED（iv_identified；CONFIGURE 第二步在这之后、收尾 cfg_end 不再出事件）；失败发 IV_FAILED、不落盘（它重新宣告 / 失败一小时后再说话 / 重新配对 / 下次开机时重来）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
D=strip_comments(open('zh_dev.c').read())
done=re.search(r'\\nstatic void iv_done\\([^)]*\\)\\n\\{(.*?)\\n\\}', D, re.S).group(1)
idf=re.search(r'\\nstatic void iv_identified\\([^)]*\\)\\n\\{(.*?)\\n\\}', D, re.S).group(1)
fail=re.search(r'\\nstatic void iv_fail\\([^)]*\\)\\n\\{(.*?)\\n\\}', D, re.S).group(1)
end=re.search(r'\\nstatic void cfg_end\\([^)]*\\)\\n\\{(.*?)\\n\\}', D, re.S).group(1)
ok = 'iv_identified(' in done and 'ZH_DEVF_INTERVIEWED' in idf and idf.count('dev_save(') == 1 and 'ZH_DEV_EV_IDENTIFIED' in idf and 'dev_event(' not in end and 'iv_identified(' not in end
ok = ok and 'dev_save(' not in fail and 'ZH_DEV_EV_IV_FAILED' in fail and 'ZH_IV_FAILED' in fail
sys.exit(0 if ok else 1)\""
check "设备自报的字符串（6.8）：只进 manufacturer / model 字段、输出前 hub_sanitize_device_str；事件文字里没有它们；生命周期类只有这次开机新入网的访谈完才推送（另外两处推送是 AVAILABILITY 的离线 / 恢复，M5 第十一步）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
Z=strip_comments(open('../hub_core/hub_zigbee.c').read())
ev=re.search(r'\\nstatic void hk_event\\([^)]*\\)\\n\\{(.*?)\\n\\}', Z, re.S).group(1)
ok = 'hub_sanitize_device_str(' in Z and 'manuf' not in ev and 'model' not in ev
ok = ok and re.search(r'const bool join = src == ZH_DEV_SRC_JOIN;', ev) and len(re.findall(r'if \(join\) flags = HEV_F_PUSH;', ev)) == 2 and ev.count('HEV_F_PUSH') == 4
av=re.search(r'case ZH_DEV_EV_OFFLINE:(.*?)case ZH_DEV_EV_IDENTIFIED:', ev, re.S)
ok = ok and av and av.group(1).count('flags = HEV_F_PUSH;') == 2
sys.exit(0 if ok else 1)\" && python3 -c \"
import sys; r=open('../hub_core/hub_zigbee.c').read()
sys.exit(0 if '\\\"manufacturer\\\"' in r and '\\\"model\\\"' in r else 1)\""
check "Zigbee 的事件名都短于 HEV_KIND_MAX（hub_event_emit 对超长的整条拒收、事件不进环也不推送；修复复核 ❌：zigbee_device_interview_failed 30 字符）" "python3 -c \"
import re,sys
lim=int(re.search(r'#define HEV_KIND_MAX +(\\d+)', open('../hub_core/include/hub_evlog.h').read()).group(1))
names=re.findall(r'kind = \\\"([a-z0-9_]+)\\\"', open('../hub_core/hub_zigbee.c').read())
names+=re.findall(r'hub_event_emit\\([^,]+, [^,]+, \\\"([a-z0-9_]+)\\\"', open('../hub_core/hub_events.c').read())
sys.exit(0 if len(names) >= 9 and all(len(n) < lim for n in names) else 1)\""
check "ZCL 消息交设备层：zhub.c 把 profile 不是 0 的 incomingMessageHandler 交 zh_dev_on_zcl；网关端点与 HA profile 和 zh_host 一致（静态断言）" "python3 ../../tools/grep_code.py -q 'zh_dev_on_zcl\\(' zhub.c && grep -q '_Static_assert(ZH_DEV_SRC_EP == ZH_HOST_ENDPOINT' zhub.c && grep -q '_Static_assert(ZH_HA_PROFILE == ZH_HOST_EP_PROFILE' zhub.c"

# ---- 读数与能力（M5 第一步：只读状态）：上报 / IAS 通知进 zh_dev 的读数，hub_core 的 hub_zcap 定能力与 kind ----
check "读数只在内存（8.9：状态不落盘、开机后靠上报重建）：zh_state_t 不在落盘记录里，更新读数的函数不调 dev_save" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
h=open('include/zh_dev.h').read(); D=strip_comments(open('zh_dev.c').read())
rec=re.search(r'typedef struct __attribute__\(\(packed\)\) \{(.*?)\} zh_dev_rec_t;', h, re.S).group(1)
f=re.search(r'\\nstatic void st_apply\\([^)]*\\)\\n\\{(.*?)\\n\\}', D, re.S)
g=re.search(r'\\nstatic void st_zone\\([^)]*\\)\\n\\{(.*?)\\n\\}', D, re.S)
ok = 'zh_state_t' not in rec and f and g and 'dev_save(' not in f.group(1) and 'dev_save(' not in g.group(1)
sys.exit(0 if ok else 1)\""
check "上报按恶意输入解析（Z27）：属性上报走 zh_zcl_next_report_rec；无效值哨兵（温度 / 气压 0x8000、湿度 / 照度 0xFFFF、电量 0xFF）不收" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
D=strip_comments(open('zh_dev.c').read())
ok = 'zh_zcl_next_report_rec(' in D and 'ZH_ZCL_CMD_REPORT_ATTR' in D
f=re.search(r'\\nstatic void st_apply\\([^)]*\\)\\n\\{(.*?)\\n\\}', D, re.S).group(1)
ok = ok and f.count('0x8000') >= 2 and f.count('0xFFFF') >= 2 and '0xFF' in f
sys.exit(0 if ok else 1)\""
check "只有报警类读数翻转才发事件（IAS alarm1 / 防拆 / 电量低、占用）：ZH_DEV_EV_STATE 只经 st_emit，st_emit 只由 st_zone_from / st_occ / st_batt_pct / st_tbl_zone 调（M5 第八步把三支抽成函数给涂鸦复用，第十步表读数通用化给小米），温湿度等不发" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
D=strip_comments(open('zh_dev.c').read())
fns=dict(re.findall(r'\\n(?:static )?[\\w ]+?\\**(\\w+)\\([^)]*\\)\\n\\{(.*?)\\n\\}', D, re.S))
users=sorted(n for n,b in fns.items() if 'ZH_DEV_EV_STATE' in b)
callers=sorted(n for n,b in fns.items() if n != 'st_emit' and re.search(r'\\bst_emit\\(', b))
ok = users == ['st_emit'] and callers == ['st_batt_pct', 'st_occ', 'st_tbl_zone', 'st_zone_from']
ok = ok and 'st_zone_from(' in fns.get('st_zone','') and 'st_occ(' in fns.get('st_apply','') and 'st_batt_pct(' in fns.get('st_apply','')
sys.exit(0 if ok else 1)\""
check "IAS 写 CIE 地址 + 自动登记（设计 8.6 IAS 第 1、2 条，CONFIGURE 第一步）：ZONE → CIE（写 0x0010、类型 0xF0、网关 EUI64 小端，SDK ias-zone-client.c:395-404）→ ENROLL（发登记应答再回读 ZoneState，读到已登记才算完、否则连应答一起重发：只认自动登记应答的设备这是唯一途径）；没成记 ZH_DEVF_CIE_OWED，重新宣告 / 加密重新入网 / 登记请求 / 一小时后说话 / 读到自己的 EUI64 时只补这两步；总期限落在这几步不算访谈失败" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
D=strip_comments(open('zh_dev.c').read())
H=open('include/zh_dev.h').read()
ok = re.search(r'ZH_IV_ZONE,\s*ZH_IV_CIE,\s*ZH_IV_ENROLL,\s*ZH_IV_BIND,', strip_comments(H)) and 'void zh_dev_set_own_eui(' in H and re.search(r'#define ZH_DEVF_CIE_OWED +0x80u', H)
iss=re.search(r'\nstatic bool iv_issue\([^)]*\)\n\{(.*?)\n\}', D, re.S).group(1)
ok = ok and re.search(r'case ZH_IV_CIE:.*?zh_zcl_enc_write_eui64\([^;]*ZH_IAS_CIE_ADDR, d->own_eui\)', iss, re.S)
en=iss[iss.index('case ZH_IV_ENROLL:'):]
ok = ok and en.find('ias_enroll_rsp(') >= 0 and en.find('ias_enroll_rsp(') < en.find('ZH_IAS_ZONE_STATE') and re.search(r'if \(!ias_enroll_rsp\([^)]*\)\) return false;', en)
z=re.search(r'\nstatic void iv_on_zcl\([^)]*\)\n\{(.*?)\n\}', D, re.S).group(1)
ok = ok and re.search(r'f\.cmd == ZH_ZCL_CMD_WRITE_ATTR_RSP', z) and re.search(r'f\.pl\[0\] == ZH_ZCL_ST_SUCCESS', z) and re.search(r'iv_set_step\(x, ZH_IV_ENROLL\)', z) \
     and re.search(r'state == ZH_IAS_STATE_ENROLLED\)\s*\{[^}]*cie_finish\(d, idx, CIE_DONE\)', z)
ab=re.search(r'\nstatic void iv_after_zone\([^)]*\)\n\{(.*?)\n\}', D, re.S)
ok = ok and ab and 'd->have_own_eui' in ab.group(1) and 'ZH_IV_CIE' in ab.group(1) and 'CIE_OWED' in ab.group(1)
sf=re.search(r'\nstatic void iv_step_failed\([^)]*\)\n\{(.*?)\n\}', D, re.S).group(1)
ok = ok and re.search(r'case ZH_IV_CIE:[^:]*cie_finish\(d, idx, CIE_OWED\)', sf) and re.search(r'case ZH_IV_ENROLL:[^:]*cie_finish\(d, idx, CIE_OWED\)', sf)
tk=re.search(r'\nstatic void iv_tick\([^)]*\)\n\{(.*?)\n\}', D, re.S).group(1)
ok = ok and re.search(r'if \(x->iv >= ZH_IV_ZONE\)\s*\{[^}]*cie_finish\(d, i, CIE_OWED\)', tk)
for fn in ('zh_dev_on_rx','zh_dev_set_own_eui','st_on_zcl'):
    b=re.search(r'\n(?:void|static void) %s\([^)]*\)\n\{(.*?)\n\}' % fn, D, re.S).group(1)
    ok = ok and 'cie_resume(' in b
ok = ok and len(re.findall(r'cie_resume\(d, \(unsigned\)idx, ZH_CIE_RESUME_GAP_MS\)', D)) >= 2 and 'cie_resume(d, (unsigned)idx, ZH_IV_RETRY_MS)' in D and 'cie_resume(d, idx, ZH_CIE_RESUME_GAP_MS)' in D and 'cie_resume(d, i, 0)' in D
C=strip_comments(open('zh_zcl.c').read())
w=re.search(r'\nsize_t zh_zcl_enc_write_eui64\([^)]*\)\n\{(.*?)\n\}', C, re.S)
ok = ok and w and 'ZH_ZCL_CMD_WRITE_ATTR' in w.group(1) and 'ZH_ZCL_T_EUI64' in w.group(1) and re.search(r'out\[0\] = 0x00;', w.group(1))
Zc=strip_comments(open('zhub.c').read())
ok = ok and re.search(r'if \([^)]*have_eui64\)[^;]*zh_dev_set_own_eui\(', Zc)
ok = ok and re.search(r'v->iv >= ZH_IV_NODE && v->iv <= ZH_IV_REPORT', open('../hub_core/hub_zigbee.c').read())
sys.exit(0 if ok else 1)\""
check "绑定 + 上报配置（设计 8.6 表 8.6-1，CONFIGURE 第二步）：只对这次开机经入网窗口新加 / 重新配对的设备（迁移来的不改它的上报节奏）；IAS 那几步之后逐簇 BIND（ZDO 0x0021：[seq][源 EUI64][源端点][簇][0x03][网关 EUI64][1]，zigbee-device-stack.h:451-458）→ REPORT（全局 0x06：[方向 0][属性][类型][最小][最大][变化量只给模拟量]，reporting.c:422-473）；失败 / 不回 / 期限到都不算访谈失败" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
D=strip_comments(open('zh_dev.c').read()); H=strip_comments(open('include/zh_dev.h').read())
ok = re.search(r'ZH_IV_ENROLL,\s*ZH_IV_BIND,\s*ZH_IV_REPORT,\s*ZH_IV_FAILED,', H)
tc=re.search(r'\nstatic void iv_to_config\([^)]*\)\n\{(.*?)\n\}', D, re.S)
ok = ok and tc and re.search(r'x->iv_new \|\| x->iv_owe_push', tc.group(1)) and 'd->have_own_eui' in tc.group(1)
iss=re.search(r'\nstatic bool iv_issue\([^)]*\)\n\{(.*?)\n\}', D, re.S).group(1)
b=iss[iss.index('case ZH_IV_BIND:'):]
ok = ok and 'ZH_ZDO_BIND_REQ' in b and 'ZH_ZDO_UNICAST_BINDING' in b and 'd->own_eui' in b and 'ZH_DEV_SRC_EP' in b
ok = ok and re.search(r'case ZH_IV_REPORT:.*?zh_zcl_enc_cfg_report\(', iss, re.S)
C=strip_comments(open('zh_zcl.c').read())
w=re.search(r'\nsize_t zh_zcl_enc_cfg_report\([^)]*\)\n\{(.*?)\n\}', C, re.S)
ok = ok and w and 'ZH_ZCL_CMD_CFG_REPORT' in w.group(1) and re.search(r'chg_len', w.group(1))
tk=re.search(r'\nstatic void iv_tick\([^)]*\)\n\{(.*?)\n\}', D, re.S).group(1)
ok = ok and re.search(r'x->iv == ZH_IV_BIND \|\| x->iv == ZH_IV_REPORT', tk)
sf=re.search(r'\nstatic void iv_step_failed\([^)]*\)\n\{(.*?)\n\}', D, re.S).group(1)
ok = ok and 'case ZH_IV_BIND:' in sf and 'case ZH_IV_REPORT:' in sf and 'iv_fail' not in sf[sf.index('case ZH_IV_BIND:'):sf.index('default:')]
ok = ok and re.search(r'v->iv >= ZH_IV_NODE && v->iv <= ZH_IV_REPORT', open('../hub_core/hub_zigbee.c').read())
sys.exit(0 if ok else 1)\""
check "开机补读电量（M5 第六步，用户定）：zh_dev_tick 调 batt_tick；只给访谈完、非路由器、有电源配置簇、还没电量读数、刚说过话（ZH_IV_AWAKE_MS 内）的；读 0x0021 + 0x0020；次数 ZH_BATT_READ_TRIES、间隔 ZH_BATT_READ_GAP_MS；交不出去不占次数" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
D=strip_comments(open('zh_dev.c').read())
b=re.search(r'\nstatic void batt_tick\([^)]*\)\n\{(.*?)\n\}', D, re.S).group(1)
tk=re.search(r'\nvoid zh_dev_tick\([^)]*\)\n\{(.*?)\n\}', D, re.S).group(1)
ok = 'batt_tick(d, now_ms);' in tk and 'ZH_DEVF_INTERVIEWED' in b and 'ZH_NODE_ROUTER' in b and 'ZH_ST_BATTERY_PCT' in b and 'ZH_ST_BATTERY_MV' in b
ok = ok and 'ZH_IV_AWAKE_MS' in b and 'ZH_BATT_READ_TRIES' in b and 'ZH_BATT_READ_GAP_MS' in b and 'ZH_ATTR_BATTERY_PCT2' in b and 'ZH_ATTR_BATTERY_MV100' in b and 'd->net_up' in b
ok = ok and b.find('d->ops.send(') < b.find('x->batt_reads++')
sys.exit(0 if ok else 1)\""
check "控制路径（设计 8.11；M5 第七步）：zh_dev_ctl_start 给有对应输入簇的（休眠设备排进待发、回 PENDING，M5 第十二步；编帧在 ctl_frame）；ZCL 簇专用、客户端 → 服务端、不禁默认应答（fc 0x01；只有涂鸦分支照 z2m 禁默认应答，M5 第九步）；开 / 关 / 切换 = OnOff 0x01 / 0x00 / 0x02，亮度 = Level 0x04 MoveToLevelWithOnOff；结论：成功的默认应答或窗口内这个簇的上报 = confirmed、messageSent 成功 = delivered、交不出去 / 被拒 / 没送达 / 设备回错 = failed；zhub.c 把 messageSentHandler 交给 zh_dev_on_msg_sent" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
D=strip_comments(open('zh_dev.c').read()); H=open('include/zh_dev.h').read()
st=re.search(r'\nzh_ctl_err_t zh_dev_ctl_start\([^)]*\)\n\{(.*?)\n\}', D, re.S)
fr=re.search(r'\nstatic zh_ctl_err_t ctl_frame\([^)]*\)\n\{(.*?)\n\}', D, re.S)
tb=re.search(r'if \\(td >= 0\\) \\{(.*?)\\n    \\}', fr.group(1), re.S) if fr else None
ok = st and fr and tb and 'ZH_CTL_R_PENDING' in st.group(1) and 'ZH_CTL_E_BUSY' in st.group(1) and 'ctl_frame(' in st.group(1) and 'ZH_CTL_E_NO_CLUSTER' in fr.group(1) \\
     and 'f->pl[0] = ZH_ZCL_FC_CLUSTER;' in fr.group(1) and fr.group(1).count('ZH_ZCL_FC_DIS_DR') == tb.group(1).count('ZH_ZCL_FC_DIS_DR') == 1
ok = ok and re.search(r'#define ZH_CL_CMD_ON +0x01u', open('include/zh_zcl.h').read()) and re.search(r'#define ZH_CL_CMD_OFF +0x00u', open('include/zh_zcl.h').read()) \
     and re.search(r'#define ZH_CL_CMD_TOGGLE +0x02u', open('include/zh_zcl.h').read()) and re.search(r'#define ZH_CL_CMD_MOVE_TO_LEVEL_ONOFF +0x04u', open('include/zh_zcl.h').read())
oz=re.search(r'\nstatic void ctl_on_zcl\([^)]*\)\n\{(.*?)\n\}', D, re.S)
ok = ok and oz and 'ZH_ZCL_CMD_DEFAULT_RSP' in oz.group(1) and 'ZH_ZCL_CMD_REPORT_ATTR' in oz.group(1) and 'ZH_CTL_R_CONFIRMED' in oz.group(1) and 'ZH_CTL_R_FAILED' in oz.group(1)
ms=re.search(r'\nvoid zh_dev_on_msg_sent\([^)]*\)\n\{(.*?)\n\}', D, re.S)
ok = ok and ms and 'ZH_CTL_R_FAILED' in ms.group(1) and 'delivered' in ms.group(1)
ok = ok and re.search(r'ZH_EZSP_ID_MESSAGE_SENT_HANDLER.*?zh_dev_on_msg_sent\(', strip_comments(open('zhub.c').read()), re.S)
sys.exit(0 if ok else 1)\""
check "IAS 登记（设计 8.6 IAS 第 2 条）：收到 Zone Enroll Request 回 Enroll Response（成功、zone id = 设备下标），簇专用、客户端 → 服务端、HA profile、APS 0x1040 | 源 EUI64（SDK ias-zone-client.c 同）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
D=strip_comments(open('zh_dev.c').read())
f=re.search(r'\\nstatic (?:void|bool) ias_enroll_rsp\\([^)]*\\)\\n\\{(.*?)\\n\\}', D, re.S)
ok = f and 'ZH_IAS_CMD_ENROLL_RSP' in f.group(1) and 'ZH_IAS_ENROLL_SUCCESS' in f.group(1) and 'ZH_ZCL_FC_CLUSTER' in f.group(1) and 'ZH_HA_PROFILE' in f.group(1) and 'ZH_APS_OPTS_DEFAULT' in f.group(1)
g=re.search(r'\\nstatic void st_on_zcl\\([^)]*\\)\\n\\{(.*?)\\n\\}', D, re.S)
ok = ok and g and re.search(r'f\\.cmd == ZH_IAS_CMD_ENROLL_REQ\\) \\{.*?ias_enroll_rsp\\(d, idx, src_ep\\);', g.group(1), re.S) and 'ZH_APS_OPT_SOURCE_EUI64' in f.group(1)
sys.exit(0 if ok else 1)\""
check "能力与 kind（设计 8.8）在 hub_core 的 hub_zcap：纯逻辑（不依赖 ESP-IDF）、主机测试；IAS 按 ZoneType 定人体 / 门磁 / 漏水 / 烟雾 / 振动；燃气 / CO 不硬套成 smoke" "$G --strings --absent -q '#\\s*include\\s*[<\\\"](esp_|freertos|sdkconfig)' ../hub_core/hub_zcap.c && grep -q 'hub_zcap_test' ../hub_core/test_host/run.sh && python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
Z=strip_comments(open('../hub_core/hub_zcap.c').read())
ok = all(k in Z for k in ('ZH_ZONE_MOTION','ZH_ZONE_CONTACT','ZH_ZONE_WATER','ZH_ZONE_FIRE','ZH_ZONE_VIBRATION','ZH_ZONE_GAS','ZH_ZONE_CO'))
g=re.search(r'case ZH_ZONE_GAS:(.*?)break;', Z, re.S)
ok = ok and g and 'HUB_CAP_SMOKE' not in g.group(1)
sys.exit(0 if ok else 1)\""
check "事件：漏水 / 烟雾 / 燃气 / CO / 防拆是 ALARM（恒推）；人体 / 门磁 / 振动是 SECURITY（推不推 = 设备 notify 位，M5 第二步；默认关 = 只进环）；句子里只有用户起的名字或 unnamed + 类 + IEEE 末四位" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
Z=strip_comments(open('../hub_core/hub_zigbee.c').read())
f=re.search(r'\\nstatic void state_event\\([^)]*\\)\\n\\{(.*?)\\n\\}', Z, re.S)
ok = f and 'HUB_EVC_ALARM' in f.group(1) and 'HUB_EVC_SECURITY' in f.group(1) and 'manuf' not in f.group(1) and 'model' not in f.group(1)
ok = ok and re.search(r'sec_flags = HEV_F_PUSH;', f.group(1) or '')
B=re.search(r'\\nstatic void state_event\\([^)]*\\)\\n\\{(.*?)\\n\\}', open('../hub_core/hub_zigbee.c').read(), re.S).group(1)
ok = ok and all(re.search(r'emit_ev\\(HUB_EVC_ALARM, id, on \\? \\\"%s' % k, B) for k in ('water_leak','smoke_detected','gas_detected','co_detected'))
ok = ok and re.search(r'emit_ev\\(HUB_EVC_ALARM, id, \\\"tamper\\\"', B) and re.search(r'emit_ev\\(HUB_EVC_SECURITY, id, \\\"motion_detected\\\", t, sec_flags\\)', B)
sys.exit(0 if ok else 1)\" && grep -q 'unnamed %s %04x' ../hub_core/hub_meta_core.c"

# ---- 任务外壳（zhub.c，只在直连型编）：单任务、UART 认领、看门狗 ----
check "单任务（CHECKLIST Z60 定：zh_ash 与 zh_main 合并）：zhub.c 只建一个任务，会话 / ASH / 编排都在它里面跑" "[ \$(python3 ../../tools/grep_code.py 'xTaskCreate' zhub.c | wc -l) -eq 1 ] && grep -q '^| Z60 .*✅' CHECKLIST.md"
check "看门狗：注册 FATAL、每圈无条件 kick —— wd_kick 是任务循环体最后一句、在循环体这一层（不在任何分支里：MG24 停在 bootloader 的几小时里也照打，Z45 / C5）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zhub.c').read())
t=re.search(r'\\nstatic void zhub_task\\(void \\*arg\\)\\n\\{(.*?)\\n\\}', S, re.S).group(1)
loop=t[t.index('for (;;) {'):]
lines=[l for l in loop.split('\\n') if l.strip()]
kick=[l for l in lines if 'wd_kick(s_wd)' in l]
sys.exit(0 if len(kick) == 1 and kick[0].startswith('        wd_kick(s_wd);') and lines[-2].strip() == 'wd_kick(s_wd);' and lines[-1].strip() == '}' and 'WD_SEV_FATAL' in S and 'wd_register(' in t else 1)\""
check "UART 只经桥的读写函数碰（写有超时：流控卡死不阻塞任务），不直接调 uart_* 驱动" "$G -q 'uart_bridge_read\\(' zhub.c && $G -q 'uart_bridge_write\\(' zhub.c && $G --absent -q '\\buart_(read_bytes|write_bytes|tx_chars|driver_install|set_pin|param_config)\\(' zhub.c"
check "不碰 IO23 / 硬复位 / 旧探测（8.13、K5、K8）：zhub 里没有 gpio_* / mg24_do_reset / watchdog_reset_mg24 / ezsp_bridge_health_probe / ncp_version_query" "$G --absent -q '\\bgpio_\\w+\\(|mg24_do_reset|watchdog_reset_mg24|ezsp_bridge_health_probe|ncp_version_query' zhub.c"
check "UART 认领用独立的 BRIDGE_STATE_ZHUB（不借 OTA 状态：ota_mg24_exit_bootloader 拿 state == OTA 判断\"已经是我的\"），只在直连型声明" "python3 -c \"
import re,sys; s=open('../ezsp_bridge/include/ezsp_bridge.h').read()
m=re.search(r'#if CONFIG_BITENGINE_ENABLE_ZHUB\\s*(?:/\\*.*?\\*/\\s*)*BRIDGE_STATE_ZHUB,\\s*#endif', s, re.S)
sys.exit(0 if m else 1)\" && $G -q 'ezsp_bridge_zhub_claim\\(' zhub.c && $G -q 'ezsp_bridge_zhub_release\\(' zhub.c"
check "别人要 UART（MG24 升级 / 基准测试 / ncp_resync / bootloader）时 ota_claim 先请 zhub 让出、release 后放它回来（直连型）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('../ezsp_bridge/ezsp_bridge.c').read())
c=re.search(r'esp_err_t ezsp_bridge_ota_claim\\(void\\)\\n\\{(.*?)\\n\\}', S, re.S).group(1)
r=re.search(r'void ezsp_bridge_ota_release\\(void\\)\\n\\{(.*?)\\n\\}', S, re.S).group(1)
fail=c[c.index('&expected, BRIDGE_STATE_OTA)) {'):]
ok = 's_zhub_suspend(' in c and c.index('s_zhub_suspend(') < c.index('BRIDGE_STATE_OTA') and 's_zhub_resume(' in r and 's_zhub_resume(' in fail.split('return ESP_ERR_INVALID_STATE')[0]
sys.exit(0 if ok else 1)\" && $G -q 'ezsp_bridge_set_zhub_hooks\\(zhub_suspend, zhub_resume\\)' zhub.c"
check "直连型里 health_probe 一律不发 RST（K5：防有人绕过仲裁打断 zhub；不按状态判，查和用之间有竞态）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('../ezsp_bridge/ezsp_bridge.c').read())
b=re.search(r'bool ezsp_bridge_health_probe\\(int timeout_ms\\)\\n\\{(.*?)\\n\\}', S, re.S).group(1)
m=re.match(r'\\s*#if CONFIG_BITENGINE_ENABLE_ZHUB\\s*\\(void\\)timeout_ms;\\s*return false;\\s*#else(.*)#endif\\s*$', b, re.S)
sys.exit(0 if m and 'uart_bridge_write' in m.group(1) else 1)\""
check "看门狗的 MG24 探针在直连型改吃 zhub 的正向证据、不调 health_probe（K6、Z47）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('../watchdog/wd_probes.c').read())
f=re.search(r'static void probe_mg24_zhub\\(void\\)\\n\\{(.*?)\\n\\}', S, re.S)
call=re.search(r'#if CONFIG_BITENGINE_ENABLE_ZHUB\\s*probe_mg24_zhub\\(\\);\\s*#else\\s*probe_mg24\\(\\);\\s*#endif', S)
b=f.group(1) if f else ''
forged=re.findall(r'mg24_last_ok_s = (?!t - age_s;)', b)
sys.exit(0 if f and call and 'zhub_mg24_evidence(' in b and 'health_probe' not in b and not forged and 'mg24_last_ok_s = t - age_s;' in b
         and 'FAULT_ACTIVE(' in b else 1)\""
check "main.c：zhub_start 只在 ENABLE_ZHUB 下，排在 hub_store / hub_svc 之后、muse_link 之前（设计 2.6 第 12 步；安全模式出口之后）" "python3 -c \"
import sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('../../main/main.c').read())
a=S.index('if (wd_safe_mode()) {'); b=S.index('hub_svc_start()'); z=S.index('zhub_start()'); m=S.index('muse_link_init()')
pre=S[:z]; guard=pre.rfind('#if CONFIG_BITENGINE_ENABLE_ZHUB'); end=pre.rfind('#endif')
sys.exit(0 if a < b < z < m and S.count('zhub_start()') == 1 and guard > end else 1)\""
check "/zigbee/status 的 hub 对象只在直连型编（HA 型回包一个字节都不变）" "python3 -c \"
import sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('../webui/rest_api.c').read())
z=S.index('zhub_get_status('); pre=S[:z]
sys.exit(0 if pre.rfind('#if CONFIG_BITENGINE_ENABLE_ZHUB') > pre.rfind('#endif') else 1)\""

check "回调交上层（第五步起）：zhub.c 给编排层登记 on_callback；incomingMessageHandler 只计数、记最后一条的来源（节点 / 簇 / 信号），回调里不打日志、不碰载荷" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zhub.c').read())
b=re.search(r'\\nstatic void on_host_callback\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S)
ok = b and 'zh_ezsp_dec_incoming_message(' in b.group(1) and 'ESP_LOG' not in b.group(1) and 'zh_host_init(&s_z->h, &ops)' in S and '.on_callback = on_host_callback' in S
sys.exit(0 if ok else 1)\""
check "hub.zigbee.permit_join 的处理函数（第六步）：只在直连型编，参数 0 或 30–254（附录 A），经 zhub_permit_join 投给 zhub 任务；seconds:0（关窗）不进限速桶，本地 REST 与 Muse 两个出口都用同一个豁免判断（6.5 注记）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('../hub_core/hub_cmds.c').read())
z=S.index('cmd_zigbee_permit_join'); pre=S[:z]
tbl=S[S.index('k_impl[] = {'):]; tbl=tbl[:tbl.index('};')]
ent=tbl.index('cmd_zigbee_permit_join }'); tpre=tbl[:ent]
fn=S[z:S.index('\\n}', z)]
ok = pre.rfind('#if CONFIG_BITENGINE_ENABLE_ZHUB') > pre.rfind('#endif') and 'zhub_permit_join(' in S and tpre.rfind('#if CONFIG_BITENGINE_ENABLE_ZHUB') > tpre.rfind('#endif')
ok = ok and re.search(r'param_uint\\(call, +, 0, HUB_PJ_MAX_S, 120, &secs\\) \\|\\| \\(secs != 0 && secs < HUB_PJ_MIN_S\\)', fn) is not None
H=open('../hub_core/include/hub_params.h').read()
ok = ok and re.search(r'#define HUB_PJ_MIN_S +30u', H) and re.search(r'#define HUB_PJ_MAX_S +254u', H)
A=strip_comments(open('../webui/hub_api.c').read()); L=strip_comments(open('../muse_link/muse_link.c').read())
ok = ok and 'hub_params_rl_exempt(' in A and 'hub_params_rl_exempt(' in L
sys.exit(0 if ok else 1)\""
check "入网窗口请求的结果交回调用方之前先刷快照（zhub 任务里 snapshot() 紧挨在置 BIT_PJ_DONE 之前；修复复核 ❌：调用方醒来读到旧快照，把\"正在开\"回成\"没开成\"）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zhub.c').read())
sys.exit(0 if re.search(r'snapshot\\(\\);\\s*xEventGroupSetBits\\(s_ev, BIT_PJ_DONE\\);', S) else 1)\""
check "/zigbee/status 的 hub 对象带 join_window / joins（窗口状态、剩余秒数、入网 / 重新入网 / 离开计数、最近一次入网）" "python3 -c \"
import re,sys
S=open('../webui/rest_api.c').read()
b=re.search(r'\\nstatic bool zigbee_hub_json\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S)
sys.exit(0 if b and all(k in b.group(1) for k in (r'\\\\\\\"join_window\\\\\\\":', r'\\\\\\\"joins\\\\\\\":', r'\\\\\\\"left_s\\\\\\\":')) else 1)\""
check "/zigbee/status 的 hub 对象带 eui64 / concentrator / incoming / last_rx（板上验\"设备上报到了\"用）" "python3 -c \"
import re,sys
S=open('../webui/rest_api.c').read()
b=re.search(r'\\nstatic bool zigbee_hub_json\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S)
sys.exit(0 if b and all(('%s' % k) in b.group(1) for k in (r'\\\\\\\"eui64\\\\\\\":', r'\\\\\\\"concentrator\\\\\\\":', r'\\\\\\\"incoming\\\\\\\":', r'\\\\\\\"last_rx\\\\\\\":')) else 1)\""
check "\"接管以来的证据\"比计数不比时间戳（持有满 2^31 ms 后 32 位差值变号，第一轮审计 ❌1）；没有证据时的年龄用 64 位" "$G -q 'zh_host_evidence_since\\(' zhub.c && $G -q 'zh_host_evidence_mark\\(' zhub.c && $G --absent -q 't_alive - s_t_claimed' zhub.c && $G -q 'static uint64_t +s_t_claimed' zhub.c && grep -q 'T24' test_host/zh_host_test.c"
check "外部立即复位（/zigbee/restart、看门狗 L2）有最短间隔，冷却期内 /zigbee/restart 回 429" "$G -q 'ZHUB_RESTART_MIN_MS' zhub.c && $G -q 'ESP_ERR_NOT_ALLOWED' zhub.c && python3 ../../tools/grep_code.py --strings -q 'ESP_ERR_NOT_ALLOWED\\) return send_error\\(req, 429' ../webui/rest_api.c"

# ---- 社区设备库：涂鸦私有簇 0xEF00 的读数（设计 8.8 注记"社区设备库"；M5 第八步）----
check "CMakeLists 在 CONFIG_BITENGINE_ENABLE_ZHUB 下编 zh_tuya.c 与生成的 zh_tuya_db.c" "python3 -c \"
import re,sys; s=re.sub(r'#.*', '', open('CMakeLists.txt').read())
m=re.search(r'if\(CONFIG_BITENGINE_ENABLE_ZHUB\)(.*?)endif\(\)', s, re.S)
sys.exit(0 if m and '\\\"zh_tuya.c\\\"' in m.group(1) and '\\\"zh_tuya_db.c\\\"' in m.group(1) else 1)\""
check "zh_tuya / zh_tuya_db 不依赖 ESP-IDF（test_host 原样编）" "$G --strings --absent -q '#\s*include\s*[<\"](esp_|freertos|sdkconfig|driver/)' zh_tuya.c zh_tuya_db.c include/zh_tuya.h include/zh_tuya_db.h"
check "zh_tuya_db.c 是生成物：带\"自动生成\"、来源版本、MIT 许可证全文与版权人（再分发义务）" "head -40 zh_tuya_db.c | grep -q '自动生成' && head -40 zh_tuya_db.c | grep -q 'MIT License' && head -40 zh_tuya_db.c | grep -q 'Copyright (c) 2018 Koen Kanters' && head -40 zh_tuya_db.c | grep -q 'THE SOFTWARE IS PROVIDED'"
check "生成物与导出脚本钉的是同一个库版本（regen.sh 的 ZHC_VERSION = zh_tdb_source 里的版本 = 金样例头里的版本）" "python3 -c \"
import re,sys
v=re.search(r'^ZHC_VERSION=([0-9.]+)', open('../../tools/zhc_export/regen.sh').read(), re.M).group(1)
db=open('zh_tuya_db.c').read(); g=open('test_host/zh_tuya_gold.inc').read()
sys.exit(0 if ('zh_tdb_source[] = \\\"zigbee-herdsman-converters %s\\\"' % v) in db and ('zigbee-herdsman-converters %s ' % v) in g.split('\n')[0] else 1)\""
check "线上格式常量带出处（zigbee-herdsman cluster.js / buffaloZcl.js、zhc lib/tuya.js）：簇 0xEF00、dataResponse 0x01 / dataReport 0x02 / activeStatusReportAlt 0x05 / activeStatusReport 0x06 / dataQuery 0x03、类型 bool 1 / number 2 / enum 4 / bitmap 5" "python3 -c \"
import re,sys
h=open('include/zh_tuya.h').read()
need=[r'#define ZH_CL_TUYA +0xEF00u', r'#define ZH_TUYA_CMD_DATA_RSP +0x01u', r'#define ZH_TUYA_CMD_DATA_REPORT +0x02u', r'#define ZH_TUYA_CMD_STATUS_ALT +0x05u', r'#define ZH_TUYA_CMD_STATUS +0x06u', r'#define ZH_TUYA_CMD_DATA_QUERY +0x03u',
      r'ZH_TUYA_T_BOOL = 1', r'ZH_TUYA_T_NUMBER = 2', r'ZH_TUYA_T_ENUM = 4', r'ZH_TUYA_T_BITMAP = 5', r'cluster\.js:7149', r'buffaloZcl\.js:598', r'tuya\.js:167']
sys.exit(0 if all(re.search(n, h) for n in need) else 1)\""
check "解码同 zhc getDataValue：4 字节数值按有符号 int32、不足 4 字节按无符号；bool 只认 data[0] == 1；最后一条不完整丢掉、前面照收（herdsman 同样 break）" "$G -q 'int32_t\)' zh_tuya.c && $G -q '== 1u?\)?' zh_tuya.c && grep -q 'T3' test_host/zh_tuya_test.c && grep -q 'T4' test_host/zh_tuya_test.c"
check "查表是二分（指纹按厂商名、型号排好；测试核了表确实有序），优先级：厂商名 + 型号 → 厂商名 + 任意型号 → 只按型号" "$G -q 'lo *< *hi|while \(lo' zh_tuya.c && grep -q 'sorted' test_host/zh_tuya_test.c && grep -q 'T5' test_host/zh_tuya_test.c"
check "zh_dev：0xEF00 帧在\"厂商专用的不认\"之前分流到 st_tuya（涂鸦帧不带厂商位也照收）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_dev.c').read())
b=re.search(r'\nstatic void st_on_zcl\([^)]*\)\n\{(.*?)\n\}', S, re.S).group(1)
i=b.find('cluster == ZH_CL_TUYA'); j=b.find('ZH_ZCL_FC_MFG')
sys.exit(0 if 0 <= i < j and 'st_tuya(' in b[i:j] else 1)\""
check "涂鸦读数复用现有事件逻辑，不另写一份：st_tuya 走 st_occ / st_batt_pct / st_zone_from（IAS 与占用的去重间隔、电量低每天一条照旧）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_dev.c').read())
def body(n): m=re.search(r'\nstatic \w+ '+n+r'\([^)]*\)\n\{(.*?)\n\}', S, re.S); return m.group(1) if m else ''
t=body('st_tuya')+body('st_tbl_val')+body('st_tbl_zone'); a=body('st_apply')
ok = all(f+'(' in t for f in ('st_occ','st_batt_pct','st_zone_from')) and 'st_occ(' in a and 'st_batt_pct(' in a
ok = ok and 'ZH_STEV_OCC_ON' not in t and 'ZH_STEV_ALARM_ON' not in t
sys.exit(0 if ok else 1)\""
check "主报警字段才进 alarm1（烟雾 + CO 二合一不会把 CO 报成烟）；第一次只收到电量 / 防拆时不假装门是关着的（不置 ZONE 读数，只出事件）" "$G -q 'zh_tbl_alarm_field\(' zh_dev.c && grep -q 'T7' test_host/zh_tuya_test.c && grep -q 'D41' test_host/zh_dev_test.c"
check "有效 ZoneType 一处算：zh_dev_zone_type（IAS 设备看记录、涂鸦设备看表）；st_zone 的人体类间隔、hub_zigbee 的事件 / 读数键、hub_zcap 都用它" "$G -q 'uint16_t zh_dev_zone_type\(const zh_dev_rec_t' zh_dev.c include/zh_dev.h && $G -q 'zh_dev_zone_type\(' ../hub_core/hub_zigbee.c && $G -q 'zh_dev_zone_type\(' ../hub_core/hub_zcap.c && python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('../hub_core/hub_zigbee.c').read())
sys.exit(0 if len(re.findall(r'v->rec\.zone_type', S)) == 0 else 1)\""
check "可控只看控制路径真能驱动的：controllable 用 hub_zcap_ctl_caps（ZCL 输入簇 + 涂鸦表里有写法的，M5 第九步）；控制前查的权限 / 能力检查仍按列表里的能力，驱动不了的另回 unsupported（审计 ❌：别对列表里有 onoff 的设备说\"做不了\"）" "$G -q 'uint32_t +hub_zcap_ctl_caps\(' ../hub_core/hub_zcap.c ../hub_core/include/hub_zcap.h && python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=open('../hub_core/hub_zigbee.c').read()
C=open('../hub_core/hub_cmds.c').read()
ok = re.search(r'hj_kbool\(w, \\\"controllable\\\", \(hub_zcap_ctl_caps\(r\)', S) and re.search(r'\*ctl_caps = hub_zcap_ctl_caps\(', S) and re.search(r'\*caps = hub_zcap_caps\(', S)
ok = ok and re.search(r'if \(\(ctl_caps & a->cap\) != a->cap\) return fail\(call, \\\"unsupported\\\"', C)
sys.exit(0 if ok else 1)\""
check "涂鸦设备的招呼（magic packet 读 Basic + dataQuery）每次开机每台最多一轮，休眠的只在它刚说过话时、配置 / 补 CIE 进行中不发，一拍一条" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_dev.c').read())
m=re.search(r'\nstatic void tuya_tick\([^)]*\)\n\{(.*?)\n\}', S, re.S); b=m.group(1) if m else ''
ok = 'tuya_hello' in b and 'ZH_IV_AWAKE_MS' in b and 'x->iv != ZH_IV_NONE' in b and 'return;' in b and 'ZH_TUYA_CMD_DATA_QUERY' in b and '0xFFFE' in b
ok = ok and re.search(r'tuya_tick\(d, now_ms\)', S) is not None
sys.exit(0 if ok else 1)\""
check "/zigbee/status 的 hub.devices 带涂鸦计数（tuya_frames / tuya_unknown / tuya_unmapped / tuya_hellos）" "python3 -c \"
import sys; S=open('../webui/rest_api.c').read()
sys.exit(0 if all(k in S for k in ('tuya_frames', 'tuya_unknown', 'tuya_unmapped', 'tuya_hellos')) else 1)\""
check "来源登记：UPSTREAM.md 记了 zigbee-herdsman-converters（版本、MIT、只导数据不拷代码、重新生成的命令）" "grep -q 'zigbee-herdsman-converters' UPSTREAM.md && grep -q 'MIT' UPSTREAM.md && grep -q 'regen.sh' UPSTREAM.md"
check "主机测试：zh_tuya_test 对照金样例（库对同一批报文的输出）逐条核查表 + 解码 + 换算，金样例不少于 1000 条" "grep -q 'zh_tuya_test' test_host/run.sh && grep -q 'zh_tuya_gold.inc' test_host/zh_tuya_test.c && [ \$(grep -c '^{ \"' test_host/zh_tuya_gold.inc) -ge 1000 ]"

# ---- 社区设备库第二阶段：涂鸦设备的开关 / 亮度（写数据点；M5 第九步）----
check "写方向的线上格式带出处：dataRequest 0x00 / sendData 0x04（cluster.js:7157、7168）；负载 = [seq u16 小端][dp][类型][长度 u16 大端][数据]，数值 4 字节大端、布尔 / 枚举 1 字节（zhc tuya.js:261-269）" "python3 -c \"
import re,sys
h=open('include/zh_tuya.h').read()
sys.exit(0 if re.search(r'#define ZH_TUYA_CMD_DATA_REQUEST +0x00u', h) and re.search(r'#define ZH_TUYA_CMD_SEND_DATA +0x04u', h) and 'cluster.js:7157' in h and 'cluster.js:7168' in h and 'tuya.js:261' in h else 1)\""
check "写表是生成物：zh_tdb_ctl_t 字段顺序与导出脚本写的初始化顺序一致（命令号、开关 dp / 类型 / 开 / 关、亮度 dp / mul / off / 下限 / 上限）" "python3 -c \"
import re,sys
h=open('include/zh_tuya_db.h').read()
m=re.search(r'typedef struct \{((?:(?!typedef).)*?)\} zh_tdb_ctl_t;', h, re.S).group(1)
names=re.findall(r'^\s*u?int\d+_t\s+([a-z_, ]+);', m, re.M)
flat=[n.strip() for g in names for n in g.split(',')]
sys.exit(0 if flat == ['cmd','onoff_dp','onoff_type','on_raw','off_raw','level_dp','level_mul','level_off','level_lo','level_hi'] else 1)\""
check "导出只给\"同一个 dp 也读得回来\"的写法（确认看回报、列表能力与可控一致）；亮度按库发过的最小 / 最大原始值封顶" "grep -q 'x.field === 1 && x.dp === c.onoff.dp' ../../tools/zhc_export/export_tuya.js && grep -q 'x.field === 2 && x.dp === c.level.dp' ../../tools/zhc_export/export_tuya.js && $G -q 'level_lo' zh_tuya.c && $G -q 'level_hi' zh_tuya.c"
check "导出脚本的主流程出错必须非零退出（全局 unhandledRejection 会吞掉它，留下写了一半的文件）" "grep -q '})().catch(e =>' ../../tools/zhc_export/export_tuya.js && grep -q 'process.exit(1)' ../../tools/zhc_export/export_tuya.js"
check "ctl_frame（同步与排队共用）：没有 ZCL 的开关 / 亮度簇时才走涂鸦表（ZCL 优先）；照 z2m 不要默认应答（fc 0x11）；切换要知道当前开关状态（ZH_CTL_E_STATE）；确认 = 窗口内设备回报写的那个 dp" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_dev.c').read())
b=re.search(r'\nstatic zh_ctl_err_t ctl_frame\([^)]*\)\n\{(.*?)\n\}', S, re.S).group(1)
c=re.search(r'\nstatic void ctl_on_zcl\([^)]*\)\n\{(.*?)\n\}', S, re.S).group(1)
ok = b.find('ep_with_in(&x->rec, cl)') < b.find('tuya_of(x)') and 'zh_tuya_enc_ctl(' in b and 'ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_DIS_DR' in b and 'ZH_CTL_E_STATE' in b
ok = ok and 'c->tuya_dp' in c and 'zh_tuya_is_data_cmd(' in c and 'ZH_CTL_CONFIRM_MS' in c
sys.exit(0 if ok else 1)\""
check "hub_zcap_ctl_caps 认涂鸦表的写法；hub.device.control 把 ZH_CTL_E_STATE 翻成用户能照做的句子（用 turn_on / turn_off）" "$G -q 'zh_tuya_ctl_fields\(' ../hub_core/hub_zcap.c && grep -q 'ZH_CTL_E_STATE' ../hub_core/hub_cmds.c && grep -q 'turn_on or turn_off' ../hub_core/hub_cmds.c"
check "/zigbee/status 的 hub.devices 带 ctl_tuya（走涂鸦路的控制命令数）" "grep -q 'ctl_tuya' ../webui/rest_api.c"
check "主机测试：写方向金样例（库的 toZigbee 对开 / 关 / 亮度发出的数据点）逐条对照固件编出的负载，不少于 100 条；zh_dev 的涂鸦控制用例 D43" "grep -q 'zh_tuya_gold_ctl.inc' test_host/zh_tuya_test.c && [ \$(grep -c '^{ \"' test_host/zh_tuya_gold_ctl.inc) -ge 100 ] && grep -q 'D43' test_host/zh_dev_test.c && grep -q 'zh_tuya_gold_ctl.inc' ../../tools/zhc_export/regen.sh"

# ---- 社区设备库第三阶段：小米 / Aqara（M5 第十步）----
check "导出器共用一套模拟（tools/zhc_export/zhc_sim.js：字段表、线性 / 开关推断、字符串池），涂鸦与小米各一个导出器；regen.sh 两个都跑、--check 都核" "grep -q \"require('./zhc_sim')\" ../../tools/zhc_export/export_tuya.js && grep -q \"require('./zhc_sim')\" ../../tools/zhc_export/export_lumi.js && grep -q 'export_lumi.js' ../../tools/zhc_export/regen.sh && grep -q 'zh_lumi_db.c' ../../tools/zhc_export/regen.sh"
check "字段编号只有一处正本：zh_tbl.h 的 ZH_TF_* 顺序 = zhc_sim.js 的 FIELDS 顺序（涂鸦表与小米表共用）" "python3 -c \"
import re,sys
h=open('include/zh_tbl.h').read(); j=open('../../tools/zhc_export/zhc_sim.js').read()
e=re.search(r'enum \{\s*ZH_TF_NONE = 0,(.*?)ZH_TF_COUNT', h, re.S).group(1)
hn=[x.strip()[6:] for x in e.replace('\n',' ').split(',') if x.strip()]
f=re.search(r'const FIELDS = \[(.*?)\n\];', j, re.S).group(1)
jn=re.findall(r\\\"name: '([A-Z_]+)'\\\", f)
sys.exit(0 if hn == jn and 'VOLTAGE' in jn and 'PRESSURE' in jn and 'enum {' not in re.sub(r'#include.*','',open('include/zh_tuya_db.h').read()).split('ZH_TDB_LIN')[0] else 1)\""
check "CMakeLists 在 CONFIG_BITENGINE_ENABLE_ZHUB 下编 zh_lumi.c 与生成的 zh_lumi_db.c；zh_lumi 不依赖 ESP-IDF" "python3 -c \"
import re,sys; s=re.sub(r'#.*', '', open('CMakeLists.txt').read())
m=re.search(r'if\(CONFIG_BITENGINE_ENABLE_ZHUB\)(.*?)endif\(\)', s, re.S)
sys.exit(0 if m and '\\\"zh_lumi.c\\\"' in m.group(1) and '\\\"zh_lumi_db.c\\\"' in m.group(1) else 1)\" && $G --strings --absent -q '#\s*include\s*[<\"](esp_|freertos|sdkconfig|driver/)' zh_lumi.c zh_lumi_db.c include/zh_lumi.h include/zh_lumi_db.h"
check "zh_lumi_db.c 是生成物：带\"自动生成\"、来源版本与 MIT 许可证全文；版本与 regen.sh 的 ZHC_VERSION 一致" "head -40 zh_lumi_db.c | grep -q '自动生成' && head -40 zh_lumi_db.c | grep -q 'Copyright (c) 2018 Koen Kanters' && python3 -c \"
import re,sys
v=re.search(r'^ZHC_VERSION=([0-9.]+)', open('../../tools/zhc_export/regen.sh').read(), re.M).group(1)
sys.exit(0 if ('zh_ldb_source[] = \\\"zigbee-herdsman-converters %s\\\"' % v) in open('zh_lumi_db.c').read() else 1)\""
check "TLV 格式带出处（herdsman foundation.js:45 只对 0xFF01 + 字符串类型按小米结构读、buffaloZcl.js:642 readMiStruct；zhc lumi.js buffer2DataObject 读 0x00F7）：[标签][ZCL 类型][值，小端]，长度按 zh_zcl_type_len" "grep -q 'foundation.js:45' include/zh_lumi.h && grep -q 'buffaloZcl.js:642' include/zh_lumi.h && grep -q 'buffer2DataObject' include/zh_lumi.h && $G -q 'zh_zcl_type_len\(' zh_lumi.c"
check "电量曲线照 zhc utils.batteryVoltageToPercentage（线性 min/max/vOffset、3V_2100 分段、3V_1500_2800）" "grep -q 'batteryVoltageToPercentage' include/zh_lumi.h && $G -q 'ZH_LDB_BATT_3V_2100' zh_lumi.c && $G -q 'ZH_LDB_BATT_3V_1500_2800' zh_lumi.c && $G -q 'ZH_LDB_BATT_LINEAR' zh_lumi.c"
check "zh_dev：0xFCC0（厂商专用）在\"厂商专用的不认\"之前分流到 st_lumi；Basic 的 0xFF01 进 TLV；Basic 的厂家 / 型号上报学进记录（小米休眠设备几乎不应访谈）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_dev.c').read())
b=re.search(r'\nstatic void st_on_zcl\([^)]*\)\n\{(.*?)\n\}', S, re.S).group(1)
i=b.find('cluster == ZH_CL_LUMI'); j=b.find('ZH_ZCL_FC_MFG')
a=re.search(r'\nstatic void st_apply\([^)]*\)\n\{(.*?)\n\}', S, re.S).group(1)
ok = 0 <= i < j and 'st_lumi(' in b[i:j] and 'ZH_ATTR_LUMI_FF01' in a and 'st_learn_str(' in a
sys.exit(0 if ok else 1)\""
check "访谈只在重新配对（is_new）时清厂家 / 型号：从上报学来的小米型号不随访谈重来丢掉" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_dev.c').read())
b=re.search(r'\nstatic void iv_start\([^)]*\)\n\{(.*?)\n\}', S, re.S).group(1)
sys.exit(0 if re.search(r'if \(is_new\) \{[^}]*memset\(r->manuf', b, re.S) else 1)\""
check "读数复用同一套写入：涂鸦与小米都走 st_tbl_val / st_tbl_zone（没有两份报警合成）；标准簇的小米改写在 st_apply 开头先查表" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_dev.c').read())
def body(n): m=re.search(r'\nstatic \w+ '+n+r'\([^)]*\)\n\{(.*?)\n\}', S, re.S); return m.group(1) if m else ''
ok = 'st_tbl_val(' in body('st_tuya') and 'st_tbl_val(' in body('st_lumi_apply') and 'st_tbl_zone(' in body('st_tuya') and 'st_tbl_zone(' in body('st_lumi_apply')
a=body('st_apply'); ok = ok and 0 <= a.find('zh_lumi_map(') < a.find('switch (cluster)')
sys.exit(0 if ok else 1)\""
check "小米人体只报\"有人\"：表里带清零秒数的，有人之后到时自动报没人（同 z2m occupancy_timeout）" "$G -q 'zh_lumi_occ_s\(' zh_dev.c && $G -q 't_occ_clear' zh_dev.c && grep -q 'D44' test_host/zh_dev_test.c"
check "hub_zcap：访谈没走完但型号在小米表里的也能定能力与设备类（只用表，不看簇）" "$G -q 'zh_dev_lumi_dev\(' ../hub_core/hub_zcap.c && grep -q 'Z8' ../hub_core/test_host/hub_zcap_test.c"
check "主机测试：小米金样例（库对同一批 TLV / 属性的输出）逐条对照固件，不少于 100 条；TLV 解析的边界与模糊" "grep -q 'zh_lumi_test' test_host/run.sh && grep -q 'zh_lumi_gold.inc' test_host/zh_lumi_test.c && [ \$(grep -c '^{ \"' test_host/zh_lumi_gold.inc) -ge 100 ]"
check "/zigbee/status 的 hub.devices 带小米计数（lumi_frames / lumi_unknown / learned）" "python3 -c \"
import sys; S=open('../webui/rest_api.c').read()
sys.exit(0 if all(k in S for k in ('lumi_frames', 'lumi_unknown', 'learned')) else 1)\""

# ---- 可用性（设计 8.10；Z32、TC-ZH06；M5 第十一步）----
check "常量照 8.10：常电静默 10 min 开始 ping、10 s 一次、3 次没回判离线；电池静默 25 h（= Z2M passive 1500 min）" "grep -Eq '#define ZH_AV_MAINS_SILENT_MS +600000u' include/zh_dev.h && grep -Eq '#define ZH_AV_PING_GAP_MS +10000u' include/zh_dev.h && grep -Eq '#define ZH_AV_PING_TRIES +3u' include/zh_dev.h && grep -Eq '#define ZH_AV_BATT_SILENT_MS +90000000u' include/zh_dev.h"
check "av_tick：网不是 UP 不判（链路复位时常电设备会被成批误判离线 —— Z32 ⚠️），网恢复后从恢复那刻起算；ping = 读 Basic 0x0000、一拍一条、交不出去不占次数；离线 / 恢复出 ZH_DEV_EV_OFFLINE / ONLINE" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_dev.c').read())
b=re.search(r'\nstatic void av_tick\([^)]*\)\n\{(.*?)\n\}', S, re.S); b=b.group(1) if b else ''
o=re.search(r'\nstatic void av_offline\([^)]*\)\n\{(.*?)\n\}', S, re.S); b += o.group(1) if o else ''
ok = '!d->net_up' in b and 't_net_up' in b and 'ZH_AV_MAINS_SILENT_MS' in b and 'ZH_AV_BATT_SILENT_MS' in b and 'ZH_DEV_EV_OFFLINE' in b and 'return;' in b
r=re.search(r'\nvoid zh_dev_on_rx\([^)]*\)\n\{(.*?)\n\}', S, re.S)
ok = ok and r and 'ZH_DEV_EV_ONLINE' in r.group(1) and re.search(r'av_tick\(d, now_ms\)', S) is not None
sys.exit(0 if ok else 1)\""
check "hub：列表的 online 看 zhub 的可用性（不再按 25 h 年龄算）；离线 / 恢复事件是 AVAILABILITY、推送" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=open('../hub_core/hub_zigbee.c').read()
ok = re.search(r'hj_kbool\(w, \\\"online\\\", !left && !v->offline\)', S) is not None and 'ZB_OFFLINE_S' not in strip_comments(S)
ok = ok and 'ZH_DEV_EV_OFFLINE' in S and 'ZH_DEV_EV_ONLINE' in S and 'zigbee_offline' in S and 'zigbee_online' in S
sys.exit(0 if ok else 1)\""
check "主机测试 D45：常电 ping 的帧与节奏、3 次没回判离线、电池 25 h、收到消息恢复、网断时不判、交不出去不占次数" "grep -q 'D45' test_host/zh_dev_test.c"
check "/zigbee/status 的 hub.devices 带可用性计数（av_pings / av_offline / av_online）" "python3 -c \"
import sys; S=open('../webui/rest_api.c').read()
sys.exit(0 if all(k in S for k in ('av_pings', 'av_offline', 'av_online')) else 1)\""

# ---- 休眠设备的控制排队（设计 8.11 第 6 步；Z33、TC-ZH05；M5 第十二步）----
check "常量照 8.11 第 6 步：排队最长 30 min；醒着的窗口用访谈那个 3 s（ZH_IV_AWAKE_MS）" "grep -Eq '#define ZH_CTLQ_MAX_MS +1800000u' include/zh_dev.h"
check "zh_dev_ctl_start：休眠设备不再回 SLEEPY，进这台的待发槽（新命令顶掉旧的）、结论立刻是 PENDING；ZH_CTL_R_PENDING 在 zh_ctl_result_t 里" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_dev.c').read()); H=open('include/zh_dev.h').read()
b=re.search(r'\nzh_ctl_err_t zh_dev_ctl_start\([^)]*\)\n\{(.*?)\n\}', S, re.S).group(1)
ok = 'ZH_CTL_R_PENDING' in H and 'ZH_CTL_R_PENDING' in b and 'return ZH_CTL_E_SLEEPY' not in b and 'cq' in b
sys.exit(0 if ok else 1)\""
check "ctlq_tick：只在它刚说过话的 3 s 里补发、一拍一条、交不出去不占次数；30 min 作废；送达靠 messageSent（tag 种类 ZH_DEV_TAG_KIND_QCTL），没送达留到下次醒来" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_dev.c').read())
b=re.search(r'\nstatic void ctlq_tick\([^)]*\)\n\{(.*?)\n\}', S, re.S); b=b.group(1) if b else ''
ok = 'ZH_IV_AWAKE_MS' in b and 'ZH_CTLQ_MAX_MS' in b and 'ZH_DEV_TAG_KIND_QCTL' in b and re.search(r'ctlq_tick\(d, now_ms\)', S) is not None
m=re.search(r'\nvoid zh_dev_on_msg_sent\([^)]*\)\n\{(.*?)\n\}', S, re.S)
ok = ok and m and 'ZH_DEV_TAG_KIND_QCTL' in S and 'cq' in m.group(1)
sys.exit(0 if ok else 1)\""
check "补发的期限是 ZH_CTLQ_SENT_MS（不是常电的 8 s：间接发 + APS 重试要二十几秒）、每发一次换代号、迟到的送达照样清槽；常电路径发出 / 离网 / 重新配对都清待发槽（审计 ❌ ×2、⚠️）；下标 6 位有静态断言" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_dev.c').read()); H=strip_comments(open('include/zh_dev.h').read())
def body(n): m=re.search(r'\n(?:static )?\w+ '+n+r'\([^)]*\)\n\{(.*?)\n\}', S, re.S); return m.group(1) if m else ''
t=body('ctlq_tick'); ms=body('zh_dev_on_msg_sent'); cs=body('zh_dev_ctl_start'); tj=body('zh_dev_on_tc_join')
ok = 'ZH_CTLQ_SENT_MS' in t and 'ZH_CTL_DEADLINE_MS' not in t and re.search(r'q->gen = \(uint8_t\)\(\+\+d->cq_gen\);\s*if \(!ctl_send', t)
ok = ok and re.search(r'if \(!q->on \|\| q->gen != \(uint8_t\)msg_tag\) return;\s*if \(!q->inflight && status != 0\) return;', ms)
ok = ok and re.search(r'd->ctl_gen = gen;\s*if \(x->cq.on\) \{\s*x->cq.on = false;', cs) and tj.count('memset(&d->dev[idx].cq, 0') == 2
ok = ok and re.search(r'_Static_assert\(ZH_DEV_MAX <= 64u', H)
sys.exit(0 if ok else 1)\""
check "控制的默认应答成功时乐观更新读数（ctl_optimistic：开 / 关 / 已知开关的切换 / 亮度；回包不再带命令之前的缓存，2026-10-23 灯泡上板）；只在成功分支调" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_dev.c').read())
ok = re.search(r'if \\(f.pl\\[1\\] == ZH_ZCL_ST_SUCCESS\\) \\{\\s*ctl_optimistic\\(d, now\\);\\s*ctl_finish\\(d, ZH_CTL_R_CONFIRMED, now\\);', S)
sys.exit(0 if ok and S.count('ctl_optimistic(') == 2 else 1)\""
check "可用性：这次开机没听到过的电池设备网 UP 满 25 h 静默判离线（av_offline_quiet：不出事件），静默离线的恢复也不出事件（2026-10-23；原先永远不判）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_dev.c').read())
q=re.search(r'\\nstatic void av_offline_quiet\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S)
ok = q and 'dev_event' not in q.group(1) and 'av_quiet = true' in q.group(1)
ok = ok and re.search(r'if \\(x->seen\\) av_offline\\(d, i\\);\\s*else av_offline_quiet\\(d, i\\);', S)
ok = ok and re.search(r'if \\(x->av_quiet\\) \\{\\s*x->av_quiet = false;\\s*\\} else \\{\\s*d->st.av_online\\+\\+;\\s*dev_event\\(d, ZH_DEV_EV_ONLINE', S)
sys.exit(0 if ok else 1)\""
check "小米 / Aqara 的电池设备不绑、不配上报（不是路由器且型号在小米表里；2026-10-23 上板 lumi.weather 全失败）" "grep -q 'if (x->rec.node_type != ZH_NODE_ROUTER && lumi_of(x) >= 0) {' zh_dev.c"
check "同步与排队两条路共用一个编帧函数（ctl_frame），涂鸦设备也能排队" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_dev.c').read())
def body(n): m=re.search(r'\n(?:static )?\w+ '+n+r'\([^)]*\)\n\{(.*?)\n\}', S, re.S); return m.group(1) if m else ''
sys.exit(0 if 'ctl_frame(' in body('zh_dev_ctl_start') and 'ctl_frame(' in body('ctlq_tick') and 'zh_tuya_enc_ctl(' in body('ctl_frame') else 1)\""
check "hub.device.control 的 pending：回 result=pending、文字照 8.11（queued until the device wakes; do not resend）；休眠设备也算可控" "grep -q 'queued until the device wakes; do not resend' ../hub_core/hub_cmds.c && grep -q 'ZH_CTL_R_PENDING' ../hub_core/hub_cmds.c"
check "主机测试 D46：排队（立刻 PENDING、顶掉旧的）、醒来 3 s 内补发（帧与同步路一致）、送达清掉 / 没送达留着、30 min 作废、涂鸦切换按补发时的读数" "grep -q 'D46' test_host/zh_dev_test.c"
check "/zigbee/status 的 hub.devices 带排队计数（ctlq_queued / ctlq_sent / ctlq_delivered / ctlq_expired）" "python3 -c \"
import sys; S=open('../webui/rest_api.c').read()
sys.exit(0 if all(k in S for k in ('ctlq_queued', 'ctlq_sent', 'ctlq_delivered', 'ctlq_expired')) else 1)\""

# ---- 主机测试 ----
check "run.sh 编产品源文件、跑自写真值表" "grep -q 'zh_ash_core.c' test_host/run.sh && grep -q 'zh_ash_test' test_host/run.sh"
check "run.sh 与 SDK 的 ash-common.c 互通（编解码）" "grep -q 'ash-common.c' test_host/run.sh"
check "run.sh 与 SDK 的 ash-ncp.c 互通（NCP 状态机）" "grep -q 'ash-ncp.c' test_host/run.sh"
check "run.sh 找不到 SDK、或 SDK 版本不是 2025.6.3 时失败而不是跳过" "grep -q 'exit 1' test_host/run.sh && grep -q 'sdk_version' test_host/run.sh && grep -q '2025.6.3' test_host/run.sh"
check "互通测试：NCP 原地应答（em260）、上层丢回显并超时重发、看门狗复位的 ERROR 路径、k=3 流水线" "grep -q 'ncp_reply' test_host/sdk_interop/ncp_stub.c && grep -q 's_echo' test_host/sdk_interop/ncp_interop.c && grep -q 'S5w' test_host/sdk_interop/ncp_interop.c && grep -q 'fresh(2, 3)' test_host/sdk_interop/ncp_interop.c"
check "SDK 来源登记在位（UPSTREAM.md：版本、文件 sha256、开源前的处置）" "[ -f UPSTREAM.md ] && grep -q '2025.6.3' UPSTREAM.md && grep -q 'ash-host.c' UPSTREAM.md && grep -q '开源前' UPSTREAM.md"

# ---- S1/S2/S3 自动检查（CLAUDE.md 编码后第 8 条）----
check "No strcmp on secrets" "! grep -rn 'strcmp.*password\|strcmp.*token' *.c"
check "No secrets in logs：密钥字段不进日志 / printf / hj_str（设计 14.2、第十三节）" "$G --absent -q '(ESP_LOG\w*|printf|hj_str)\s*\([^;]*(network_key|nwk_key|preconfigured_key)' *.c"
# ---- 入网窗口先导临时链路密钥（2026-10-23：迁移来的网是 bellows 散列 TCLK，小米抓包实锤解不开 Transport Key）----
check "开窗第一步导临时链路密钥：WO_KEY 是第 0 步、用 ZH_HOST_TRANSIENT_EUI64 + ZH_HOST_WELL_KNOWN_LINK_KEY（= ZigBeeAlliance09，SDK util.h:66-70）；导入只在开窗那一步；关窗照旧清；主机测试 T36 / T39b、host_interop H11 钉字节" "$G -q 'enum \\{ WO_KEY = 0, WO_POLICY, WO_PERMIT, WO_BCAST, WO_N \\};' zh_host_core.c && grep -q '#define ZH_HOST_WELL_KNOWN_LINK_KEY { 0x5A, 0x69, 0x67, 0x42, 0x65, 0x65, 0x41, 0x6C, 0x6C, 0x69, 0x61, 0x6E, 0x63, 0x65, 0x30, 0x39 }' include/zh_host_core.h && grep -q '#define ZH_HOST_TRANSIENT_EUI64     { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }' include/zh_host_core.h && [ \$(grep -c 'zh_ezsp_enc_import_transient_key(' zh_host_core.c) -eq 1 ] && ! grep -q 'zh_ezsp_enc_import_transient_key(' zhub.c zh_dev.c && $G -q 'WC_CLEAR_KEYS' zh_host_core.c && grep -q 'T39b' test_host/zh_host_test.c && grep -q 's_key_bad' test_host/sdk_interop/host_interop.c"

# ---- NCP 侧诊断（2026-10-23：小米重新配对放行后一帧收不到，先测量）----
check "NCP 诊断的 readCounters / getChildData 编解码过 SDK 预言机（ezsp_oracle）" "grep -q 'zh_ezsp_enc_read_counters' test_host/sdk_interop/ezsp_oracle.c && grep -q 'zh_ezsp_dec_child_data' test_host/sdk_interop/ezsp_oracle.c && grep -q 'sl_zigbee_ezsp_get_child_data' test_host/sdk_interop/ezsp_oracle.c"
check "NCP 诊断在 zhub 里一条在途、链路不是 RUNNING 就作废、tag 种类 0xF（设备层只用 1–7）" "$G -q 'ZHUB_TAG_KIND_DIAG 0xFu' zhub.c && $G -q 'if \\(s_z->h.state != ZH_HOST_RUNNING\\) \\{' zhub.c && $G -q 'if \\(s_dg.inflight\\) \\{' zhub.c"
check "/api/v1/zigbee/ncp_diag 只在直连型（hub_api.c 的 ENABLE_ZHUB 里，不占 HA 型槽位）、首行跨站守卫再认证" "grep -Pzoq 'static esp_err_t handle_zigbee_ncp_diag\\(httpd_req_t \\*req\\)\\n\\{\\n\\s+CROSS_ORIGIN_GUARD\\(req\\);\\n\\s+AUTH_REQUIRED\\(req\\);' ../webui/hub_api.c && grep -Pzoq '#if CONFIG_BITENGINE_ENABLE_ZHUB\\n\\s+\\{ \\.uri = \"/api/v1/zigbee/ncp_diag\"' ../webui/hub_api.c && ! grep -q 'ncp_diag' ../webui/rest_api.c"

# ---- NCP 恢复的故障注入（TC-ZH07；设计第十四节）----
check "TC-ZH07 注入只在故障注入构建里：zhub_fault_ash_rst 的定义、标志与任务里的写都在 #if CONFIG_BITENGINE_FAULT_INJECTION 里；写的是 CAN + RST（C0，CRC 38 BC）+ FLAG；接管 UART 时清掉没处理的注入" "python3 -c \"
import re,sys; S=open('zhub.c').read()
ok = re.search(r'#if CONFIG_BITENGINE_FAULT_INJECTION\nesp_err_t zhub_fault_ash_rst\(void\)\n\{[^}]*\}\n#endif', S)
ok = ok and re.search(r'#if CONFIG_BITENGINE_FAULT_INJECTION\n\s+if \(atomic_exchange\(&s_fault_rst, false\)\) \{.*?\{ ZH_ASH_CAN, ZH_ASH_CTRL_RST, 0x38, 0xBC, ZH_ASH_FLAG \}.*?\n#endif', S, re.S)
ok = ok and re.search(r'#if CONFIG_BITENGINE_FAULT_INJECTION\nstatic atomic_bool\s+s_fault_rst;', S) and re.search(r'#if CONFIG_BITENGINE_FAULT_INJECTION\n\s+atomic_store\(&s_fault_rst, false\);\n#endif', S)
ok = ok and len(re.findall(r's_fault_rst', S)) == 4
sys.exit(0 if ok else 1)\""
check "TC-ZH07 不碰 IO23：zhub.c 里没有 GPIO 操作（复位 NCP 只走 ASH）" "$G --absent -q 'gpio_set_level|gpio_config|ZIGBEE_RESET|MG24_RESET_PIN' zhub.c"
check "TC-ZH07 的入口 /diag/fault 的 zhub_ash_rst 同时在故障注入与 ENABLE_ZHUB 里（HA 型与出厂直连型都没有）" "python3 -c \"
import re,sys; S=open('../webui/rest_api.c').read()
m=re.search(r'#if CONFIG_BITENGINE_FAULT_INJECTION\n// ---- POST /api/v1/diag/fault ----\nstatic esp_err_t handle_diag_fault\(httpd_req_t \*req\)\n\{(.*?)\n\}\n#endif', S, re.S)
ok = m and re.search(r'#if CONFIG_BITENGINE_ENABLE_ZHUB\n[^#]*zhub_ash_rst[^#]*zhub_fault_ash_rst\(\)[^#]*#endif', m.group(1)) and S.count('zhub_fault_ash_rst') == 1
sys.exit(0 if ok else 1)\""
internal "TC-ZH07 发版脚本拒发故障注入构建（publish_ota.sh 认 FAULT_INJECTION）" "grep -q 'FAULT_INJECTION' ../../tools/publish_ota.sh"
check "TC-ZH07 上板脚本在（tools/test_zhub_ncp_reset.py：注入 → 5 s 内 ncp_resets 加一 → 回到 running / up、net_inits 加一、last_fail = ncp_reset、不重启）" "grep -q 'zhub_ash_rst' ../../tools/test_zhub_ncp_reset.py && grep -q 'ncp_resets' ../../tools/test_zhub_ncp_reset.py"

check "不整帧转储：ESP_LOG_BUFFER_HEX 之类会把 setInitialSecurityState 帧里的网络密钥打出来" "$G --absent -q 'ESP_LOG_BUFFER_(HEX|HEXDUMP|CHAR)' *.c"
check "zh_ash_core 不分配内存（全部在调用方给的结构体里）" "$G --absent -q '\bmalloc\(|\bcalloc\(|\bfree\(' zh_ash_core.c"

# ---- 主动发现（设计 8.12 ④；M6 第一步，2026-10-23）----
check "LQI 表请求 / 应答的簇号与条目长度带 SDK 出处（run.sh 逐个核行号）" "grep -q '#define ZH_ZDO_LQI_TABLE_REQ   0x0031u  /\* zigbee-device-stack.h:' include/zh_dev.h && grep -q '#define ZH_ZDO_LQI_TABLE_RSP   0x8031u  /\* zigbee-device-stack.h:' include/zh_dev.h && grep -q '#define ZH_ZDO_LQI_ENTRY_LEN   22u' include/zh_dev.h && grep -q 'ZH_ZDO_LQI_TABLE_REQ' test_host/run.sh"
check "节奏：一次一条、两条至少隔 1 s、每页最多 3 次、最多 5 层（设计\"每秒 1 个、最多 5 层\"）" "grep -q '#define ZH_DISC_GAP_MS         1000u' include/zh_dev.h && grep -q '#define ZH_DISC_TRIES          3u' include/zh_dev.h && grep -q '#define ZH_DISC_DEPTH_MAX      5u' include/zh_dev.h && $G -q 'if \(s->inflight\) \{' zh_dev.c"
check "网不是 UP 时暂停（在途的作废、不算次数）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_dev.c').read())
f=re.search(r'\nstatic void disc_tick\(zh_dev_core_t \*d, uint32_t now\)\n\{(.*?)\n\}', S, re.S).group(1)
p=[f.find(x) for x in ('if (!d->net_up) {','s->inflight = false;','disc_send(d, now)')]
sys.exit(0 if -1 not in p and p==sorted(p) else 1)\""
check "应答只认当前这台、序号对上的（迟到的 / 别人的不认）" "$G -q 'sender != s->q\[s->q_i\]\.nwk \|\| pl\[0\] != s->zdo_seq' zh_dev.c"
check "只建\"IEEE 和短地址都不认识\"的：已有记录不改、短地址被占的不建（邻居表可能是旧的）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_dev.c').read())
f=re.search(r'\nstatic void disc_entry\(zh_dev_core_t \*d, const uint8_t \*e, uint8_t depth\)\n\{(.*?)\n\}', S, re.S).group(1)
p=[f.find(x) for x in ('zh_dev_find_ieee(d, ieee) >= 0','zh_dev_find_nwk(d, nwk) >= 0','learn(d, nwk, ieee, ZH_DEV_SRC_DISCOVERY, &caps')]
sys.exit(0 if -1 not in p and p==sorted(p) and f.count('learn(')==1 else 1)\""
check "跳过旧条目（前子节点）与别的网的（扩展 PAN 不同）；只把路由器排进下一层" "$G -q 'rel >= ZH_LQI_REL_PREV_CHILD' zh_dev.c && $G -q 'memcmp\(e, d->xpan, 8\) != 0' zh_dev.c && $G -q 'type == ZH_LQI_TYPE_ROUTER && depth \+ 1u <= ZH_DISC_DEPTH_MAX' zh_dev.c"
check "发现来的路由器马上访谈：iv_always_on 认路由器（与可用性 / 控制的 rx_on 判据一致）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
D=strip_comments(open('zh_dev.c').read())
g=re.search(r'\\nstatic bool iv_always_on\\([^)]*\\)\\n\\{(.*?)\\n\\}', D, re.S)
sys.exit(0 if g and 'x->rec.node_type == ZH_NODE_ROUTER' in g.group(1) else 1)\""
# 不保存邻居表（docs/00 第九节 A.2 的直连型例外）：按行为查，不按名字（审计 ⚠️：只扫名字换个名就漏）——
# 条目只在 disc_on_rsp 里按偏移读、交给 disc_entry 当场处理；条目长度常量不出这两个函数；disc_* 里没有 memcpy（没把条目拷去存）；
# zh_disc_t 里只有路由器短地址 + 层数的队列（没有 8 字节 IEEE / 22 字节条目的数组）
check "不保存邻居表：条目当场处理、不拷、zh_disc_t 只存短地址 + 层数" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_dev.c').read()); H=strip_comments(open('include/zh_dev.h').read())
fn={m.group(1):m.group(2) for m in re.finditer(r'\nstatic \w+ (disc_\w+)\([^)]*\)\n\{(.*?)\n\}', S, re.S)}
ok = {'disc_entry','disc_on_rsp','disc_tick','disc_send'} <= set(fn)
ok = ok and all('memcpy' not in b for b in fn.values())
ok = ok and S.count('ZH_ZDO_LQI_ENTRY_LEN') == fn['disc_on_rsp'].count('ZH_ZDO_LQI_ENTRY_LEN')
t=re.search(r'typedef struct \{(.*?)\} zh_disc_t;', H, re.S).group(1)
ok = ok and re.search(r'struct \{ uint16_t nwk; uint8_t depth; \} q\[ZH_DISC_Q_MAX\];', t) and not re.search(r'\[(8|22|ZH_ZDO_LQI_ENTRY_LEN)\]', t)
sys.exit(0 if ok else 1)\""
check "zhub.c 把扩展 PAN 交给设备层（过滤别的网的条目）；NCP 复位清了 have_params 也不撤（不留空窗）" "$G -q 'if \(s_z->h.have_params\) zh_dev_set_ext_pan\(&s_z->dev, s_z->h.params.ext_pan_id\);' zhub.c && $G --absent -q 'zh_dev_set_ext_pan\([^)]*NULL' zhub.c"
check "不知道本网扩展 PAN 不开始；等待期掉网回到没开始（回来重新等满 60 s）" "$G -q 'if \(!expired\(now, s->t_start\) \|\| !d->have_xpan\) return;' zh_dev.c && $G -q 'if \(s->state == ZH_DISC_WAIT\) s->state = ZH_DISC_IDLE;' zh_dev.c"
check "本地队列满交不出去不占次数（只记 send_busy、t_next 往后推）；NCP 拒了才算一次" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_dev.c').read())
f=re.search(r'\nstatic void disc_send\(zh_dev_core_t \*d, uint32_t now\)\n\{(.*?)\n\}', S, re.S).group(1)
m=re.search(r'if \(!d->ops.send \|\| !d->ops.send\([^)]*\)\) \{(.*?)\}', f, re.S)
ok = m and 'disc_try_failed' not in m.group(1) and 'send_busy++' in m.group(1) and 's->t_next = now + ZH_DISC_GAP_MS;' in m.group(1)
ok = ok and re.search(r'ZH_DEV_TAG_KIND_DISC && tag <= ZH_DEV_TAG_MAX\) \{.*?disc_try_failed\(d, now_ms\);', S, re.S)
sys.exit(0 if ok else 1)\""
check "访谈的\"随时可发\"只有一个判据 iv_always_on（iv_can_talk 与第 0 遍共用：路由器、邻居表说空闲时也收的常电终端）" "$G -q 'if \(iv_always_on\(x\)\) return true;' zh_dev.c && $G -q 'const bool rx_on = iv_always_on\(x\);' zh_dev.c && $G -q 'ZH_DISC_F_RX' zh_dev.c"
check "主动发现建的、一次都没听到过的常电设备判离线静默不推（表里的旧条目）" "$G -q 'if \(!x->seen && \(x->disc & ZH_DISC_F_NEW\)\) av_offline_quiet\(d, i\);' zh_dev.c"
check "/zigbee/status 的 discovery 缓冲在 PSRAM、所有路径都释放（S3）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('../webui/rest_api.c').read())
f=re.search(r'\nstatic bool zigbee_hub_json\(char \*buf, size_t cap\)\n\{(.*?)\n\}', S, re.S).group(1)
a=f.find('heap_caps_malloc(DISC_CAP'); tail=f[a:]
rets=[m.start() for m in re.finditer(r'return ', tail)]
frees=[m.start() for m in re.finditer(r'heap_caps_free\(disc\);', tail)]
ok = a>=0 and 'char disc[' not in f and len(frees)==2 and all(any(fr < r for fr in frees) for r in rets[1:])
sys.exit(0 if ok else 1)\""
check "/zigbee/status 的 hub.discovery 报主动发现的进度与计数（上板看爬没爬、回环通不通）" "grep -q '\\\\\"discovery\\\\\":%s' ../webui/rest_api.c && grep -q 'ds->disc_reqs' ../webui/rest_api.c"

# 访谈失败、随时听得见的：到点自己重来（2026-10-24 Sengled 灯：入网那一刻访谈不回、从不说话也不宣告，直到网关重启才认出来）。
# 在 iv_tick 里、网 UP 之后；只对 iv_always_on 且 iv_can_talk 的；间隔按连着失败的次数取档，最后一档是 ZH_IV_RETRY_MS；一拍只重开一台；
# 失败次数在 iv_fail 加、访谈完 / 重新配对清；/zigbee/status 报 iv_retries
check "访谈失败的常电 / 路由器到点自己重来（档位、门槛、一拍一台、计数清零、iv_retries 上报）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('zh_dev.c').read())
def body(n): m=re.search(r'\n(?:static )?[\w ]+ '+n+r'\([^)]*\)\n\{(.*?)\n\}', S, re.S); return m.group(1) if m else ''
t=body('iv_tick'); g=body('iv_on_retry_gap')
p=[t.find(x) for x in ('if (d->iv_busy || !d->net_up) return;', 'x->iv != ZH_IV_FAILED || !iv_always_on(x) || !iv_can_talk(x, now)', 'iv_on_retry_gap(x->iv_fails)', 'd->st.iv_retries++;', 'iv_start(d, i, false, now);', 'break;')]
ok = -1 not in p and p == sorted(p)
ok = ok and all(k in g for k in ('ZH_IV_ON_RETRY1_MS', 'ZH_IV_ON_RETRY2_MS', 'ZH_IV_ON_RETRY3_MS', 'ZH_IV_RETRY_MS'))
ok = ok and 'x->iv_fails++' in body('iv_fail') and 'x->iv_fails = 0;' in body('iv_identified') and 'if (is_new) x->iv_fails = 0;' in body('iv_start')
# 一轮只出一次失败事件（不在了的设备每小时一条会冲掉事件环）；重新宣告 / 离开后回来清零；发现建的从没听到过的不每小时问；无符号差
ok = ok and 'if (x->iv_new || first) dev_event(d, ZH_DEV_EV_IV_FAILED' in body('iv_fail') and 'const bool first = x->iv_fails == 0;' in body('iv_fail')
ok = ok and re.search(r'if \(x->iv == ZH_IV_FAILED\) \{\s*x->iv_fails = 0;\s*iv_start\(d, \(unsigned\)idx, false, now_ms\);', body('zh_dev_on_zdo'))
ok = ok and re.search(r'if \(!\(x->rec.flags & ZH_DEVF_INTERVIEWED\)\) \{\s*x->iv_fails = 0;\s*iv_start\(d, \(unsigned\)idx, false, now\);', S)
ok = ok and 'if (now - x->iv_t_fail < iv_on_retry_gap(x->iv_fails)) continue;' in t and 'if (x->iv_fails > 3u && !x->seen && (x->disc & ZH_DISC_F_NEW)) continue;' in t
H=open('include/zh_dev.h').read()
ok = ok and re.search(r'#define ZH_IV_ON_RETRY1_MS\s+30000u', H) and re.search(r'#define ZH_IV_ON_RETRY2_MS\s+120000u', H) and re.search(r'#define ZH_IV_ON_RETRY3_MS\s+600000u', H)
R=open('../webui/rest_api.c').read()
ok = ok and 'iv_retries' in R and 'ds->iv_retries' in R
sys.exit(0 if ok else 1)\""
check "主机测试 D48：常电到点重来的档位与门槛（休眠 / 网不是 UP / 短地址作废不重来、一轮一次事件、各处清零、发现建的不每小时问、25 天差值、一拍一台）" "grep -q 'D48 第 %d 次失败后满' test_host/zh_dev_test.c && grep -q 'D48 休眠终端 30 s 后说话' test_host/zh_dev_test.c && grep -q 'D48 到点重来又失败的不再出事件' test_host/zh_dev_test.c && grep -q 'D48 失败 25 天后' test_host/zh_dev_test.c"

echo ""; echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
