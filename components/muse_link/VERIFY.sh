#!/bin/bash
# components/muse_link/VERIFY.sh — Muse 链路的引入约束（docs/15_muse_direct_design_v1.md 第四节、附录 C）
# ⚠️ 必须 cd 进本目录跑：bash VERIFY.sh
#
# 这里查代码形状与引入规矩；vm_api 的请求/应答字段与凭据隔离在 test_host/run.sh（上游测试原样跑）。
# M2 只引入 vm_api；会话、注册、调用、推送在 M3 加，到时在下面补对应检查。

PASS=0; FAIL=0
check() {
    if eval "$2" > /dev/null 2>&1; then echo "✅ $1"; PASS=$((PASS+1))
    else echo "❌ $1"; FAIL=$((FAIL+1)); fi
}
G="python3 ../../tools/grep_code.py"

# ---- 只属于直连型 + Muse 开关（架构 5.8 R-3） ----
# 只在私有检出里有的内部文档 / 工具（docs/、发布与产线脚本）：公开仓库不导出它们（tools/oss_export.py），
# 那里跳过而不是报红；私有检出里照常判。标志是 docs/开发索引.md 在不在。
internal() { if [ -f ../../docs/开发索引.md ]; then check "$@"; else echo "⏭  $1 —— 依赖内部文档 / 工具，本检出没有，跳过"; fi; }

check "CMake：源文件只在 BITENGINE_ENABLE_MUSE_LINK 下给" "grep -Pzo 'set\\(_muse_link_srcs \"\"\\)\\s*\\nif\\(CONFIG_BITENGINE_ENABLE_MUSE_LINK\\)' CMakeLists.txt"
check "CMake：依赖不放进 if（预展开读不到 CONFIG_*）" "python3 -c \"import re,sys; s=open('CMakeLists.txt').read(); i=s.find('if(CONFIG_BITENGINE_ENABLE_MUSE_LINK)'); e=s.find('endif()', i); sys.exit(1 if 'REQUIRES' in s[i:e] else 0)\""
# 查构建产物（compile_commands），不 grep CMakeLists 文本：哪天加一句注释解释"为什么不编它"就会被绊倒（7.4）
check "固件不编上游 muse_account_api（附录 C：components/muse 不引入，它只在测试目录里）" "python3 -c \"
import json,sys
fs=[e['file'] for e in json.load(open('../../build-direct/compile_commands.json'))]
sys.exit(0 if any(f.endswith('/components/muse_link/vm_api.c') for f in fs) and not any('muse_account' in f for f in fs) else 1)\""
check "CMake：REQUIRES / PRIV_REQUIRES 的参数里没有变量（预展开时变量还是空的）" "python3 -c \"import re,sys; sys.exit(1 if re.search(r'REQUIRES[^)]*[$][{]', open('CMakeLists.txt').read()) else 0)\""

# ---- 原样引入 + 登记（附录 C） ----
for f in vm_api.c include/vm_api.h include/vm_connect.h LICENSE UPSTREAM.md check_upstream_files.sh; do
    check "$f 存在" "[ -f $f ]"
done
check "UPSTREAM.md 记着固定提交和文件清单" "grep -qE '^\`[0-9a-f]{40}\`' UPSTREAM.md && grep -qx '\`\`\`manifest' UPSTREAM.md"
check "引入文件与 UPSTREAM.md 清单一致（check_upstream_files.sh）" "bash check_upstream_files.sh"

# ---- TLS：vm_api 走 HTTPS + 证书包；红线 7（mbedTLS 必须用外部内存） ----
check "默认 API 地址是 https" "grep -qE '#define VM_API_DEFAULT_BASE_URL \"https://' include/vm_api.h"
check "HTTP 客户端挂了证书包" "$G -q 'crt_bundle_attach\\s*=\\s*esp_crt_bundle_attach' vm_api.c"
check "直连型开着证书包、mbedTLS 用外部内存（红线 7）" "grep -qx 'CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y' ../../sdkconfig.direct && grep -qx 'CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y' ../../sdkconfig.direct"

# ---- S1：SDK token 不进日志 ----
# 上游 vm_api.c 以 INFO 打 SDK token 前 12 个字符（"refresh carries SDK token %.12s"）。我们的日志进 log_store、网页可看可导出：
# CMakeLists 按文件定义 LOG_LOCAL_LEVEL=ESP_LOG_WARN，INFO 分支编译期就没了（不改上游文件；运行时 esp_log_level_set 会被
# "*" 清掉，靠不住 —— 2026-10-22 第二轮审计）。下面查编译命令和目标文件，不查 CMake 文本。
check "vm_api.c 的编译命令带 LOG_LOCAL_LEVEL=ESP_LOG_WARN" "python3 -c \"
import json,sys
c=[e['command'] for e in json.load(open('../../build-direct/compile_commands.json')) if e['file'].endswith('/components/muse_link/vm_api.c')]
sys.exit(0 if len(c)==1 and '-DLOG_LOCAL_LEVEL=ESP_LOG_WARN' in c[0] else 1)\""
check "libmuse_link.a 里没有打 SDK token 的格式串（且 W/E 日志还在 = 没把日志整个关掉）" "[ -f ../../build-direct/esp-idf/muse_link/libmuse_link.a ] && ! strings ../../build-direct/esp-idf/muse_link/libmuse_link.a | grep -q 'SDK token %' && strings ../../build-direct/esp-idf/muse_link/libmuse_link.a | grep -q 'HTTP %d auth failure'"
check "上游 vm_api 里带 SDK token 的日志都在 INFO 及以下（WARN 以上压级挡不住）" "! grep -nE 'ESP_LOG[EW]\\s*\\(.*sdk_token' vm_api.c"

# ---- muse_cfg：hubdb 命名空间 muse 的读写（设计 4.4） ----
check "muse_cfg 只经 hub_store 写 hubdb（不直接 nvs_*）" "$G -q 'hub_store_put\\s*\\(' muse_cfg.c && $G --absent -q '\\bnvs_(set|get|open|erase|commit)\\w*\\s*\\(' muse_cfg.c muse_cfg_core.c"
check "写 SDK token 前先校验格式（不合格不落盘）" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('muse_cfg.c').read())
m=re.search(r'esp_err_t\s+muse_cfg_set_sdk_token\s*\([^)]*\)\s*\{(.*?)\n\}', s, re.S)
b=m.group(1) if m else ''
sys.exit(0 if 0<=b.find('muse_sdk_token_valid(')<b.find('put_str(') else 1)\""
check "读出来的 SDK token 不合格当作没有（且清零）" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('muse_cfg.c').read())
m=re.search(r'esp_err_t\s+muse_cfg_get_sdk_token\s*\([^)]*\)\s*\{(.*?)\n\}', s, re.S)
b=m.group(1) if m else ''
sys.exit(0 if 'muse_sdk_token_valid(' in b and 'mbedtls_platform_zeroize' in b else 1)\""
check "配对凭据落盘：put → flush → 读回比对；任何一步失败整套删掉（不留半套）" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('muse_cfg.c').read())
m=re.search(r'esp_err_t\s+muse_cfg_save_pairing\s*\([^)]*\)\s*\{(.*?)\n\}', s, re.S)
b=m.group(1) if m else ''
p=b.find('put_str(K_ACC_TOK'); f=b.find('hub_store_flush('); r=b.find('readback_equals('); e=b.find('erase_pairing()')
# 读回的结果必须真的决定成败：不许被 true || / 1 || 短路掉，读回不同就得置错
short=re.search(r'(true|1)\\s*\\|\\|\\s*readback_equals', b)
sys.exit(0 if 0<=p<f<r<e and not short and re.search(r'if\\s*\\(\\s*!same\\s*\\)\\s*err\\s*=', b) else 1)\""
check "日志只打秘密的提示（muse_secret_hint：前缀 + 长度）" "$G -q 'muse_secret_hint\\s*\\(' muse_cfg.c && $G --strings -q 'mgst_' muse_cfg_core.c"
check "读回缓冲用完清零" "$G -q 'mbedtls_platform_zeroize\\s*\\(\\s*buf' muse_cfg.c"
check "默认显示名用 esp_read_mac（不用 esp_efuse_mac_get_default：C5 上越界）" "$G -q 'esp_read_mac\\s*\\(' muse_cfg.c && $G --absent -q 'esp_efuse_mac_get_default' muse_cfg.c"
check "muse_cfg_core 不依赖 ESP-IDF（主机测试原样编）" "! grep -qE '#include \"(esp_|freertos|nvs|hub_store)' muse_cfg_core.c include/muse_cfg_core.h"
check "主机测试编产品源文件 muse_cfg_core.c" "grep -qF '\$COMP/muse_cfg_core.c' test_host/run.sh"

# ---- muse_proto：控制面报文（设计 4.2 注 3、4.7、4.8、4.13；14.2 muse_link 行里现在就能查的部分） ----
check "muse_proto 不依赖 ESP-IDF（主机测试原样编）" "! grep -qE '#include [<\"](esp_|freertos|nvs|sdkconfig)' muse_proto.c muse_proto.h"
check "主机测试编产品源文件 muse_proto.c，并且真的执行" "grep -qF '\$COMP/muse_proto.c' test_host/run.sh && grep -qx '\"\$T/muse_proto_test\"' test_host/run.sh"
# ---- muse_link_core：会话状态机（设计 4.2；状态 × 事件全排列在主机测试里） ----
check "muse_link_core 不依赖 ESP-IDF（主机测试原样编）" "$G --strings --absent -q '#\\s*include\\s*[<\"](esp_|freertos|nvs|sdkconfig)' muse_link_core.c include/muse_link_core.h"
check "主机测试编产品源文件 muse_link_core.c，并且真的执行" "grep -qF '\$COMP/muse_link_core.c' test_host/run.sh && grep -qx '\"\$T/muse_link_core_test\"' test_host/run.sh"
check "在线时后台刷新被拒保留令牌（上游 allow_stale=true），只有 REFRESHING 里被拒才撤销" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('muse_link_core.c').read())
m=re.search(r'case MLE_REFRESH_REJECTED:(.*?)break;', s, re.S)
b=m.group(1) if m else ''
sys.exit(0 if re.search(r'if \\(s == MUSE_ST_REFRESHING\\) return to_unpaired', b) and re.search(r'if \\(s == MUSE_ST_ONLINE\\) return o;', b) else 1)\""
# 键名大小写：只认 CaseSensitive（{"ID":5,"id":"x"} 时校验认的是 "id"，处理函数用不分大小写的查找会拿到 "ID":5）
check "自写文件只用 cJSON_GetObjectItemCaseSensitive" "$G --absent -q '\\bcJSON_GetObjectItem\\s*\\(' muse_proto.c muse_cfg.c muse_link_core.c \$(ls muse_link.c muse_session.cpp 2>/dev/null)"
# 用 python 判（for 循环里 `|| exit 1` 不包子 shell 会把整个 VERIFY 退出、一行 ❌ 都不打 —— 2026-10-22 变异抓到）
check "4 个管理命令都有处理分支（device.unpair / list_vms / set_vm / reset_vm）" "python3 -c \"
import sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('muse_proto.c').read(), keep_strings=True)
need=['strcmp(command, \\\"device.%s\\\") == 0) return MP_MGMT_%s;' % (c, c.upper()) for c in ('unpair','list_vms','set_vm','reset_vm')]
sys.exit(0 if all(n in s for n in need) else 1)\""
check "注册被拒还认 ok:false（4.2 注 3；上游只看 error）" "$G -q 'err_set \\|\\| cJSON_IsFalse\\(ok\\)' muse_proto.c"
# 注释里写着"不送 SSID"（7.4）：用 grep_code --strings，只看代码与字符串
check "注册不发 metadata、不带 SSID（4.7；两份上游没有 SSID 时都不发这个键）" "$G --strings --absent -q '(?i)ssid|\"metadata\"' muse_proto.c"
check "注册截断就不发（宁可不注册也不发半张表）：mp_build_register 里截断返回 -1" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('muse_proto.c').read())
m=re.search(r'int\\s+mp_build_register\\s*\\([^)]*\\)\\s*\\{(.*?)\\n\\}', s, re.S)
sys.exit(0 if m and re.search(r'if \\(w\\.truncated\\) \\{[^}]*return -1;', m.group(1)) else 1)\""
# 14.2：隧道、发现、聊天订阅、上游 ble.* 命令都不引入（MD-3、4.9）；--strings 连字符串一起看，注释里解释"为什么不要"不算
check "不出现 tunnel_netif、/link-tunnel、ip_napt、device.discover（作命令）、/chat/subscribe、\"ble.、network_ready" "$G --strings --absent -q 'tunnel_netif|/link-tunnel|ip_napt|\"device\\.discover\"|/chat/subscribe|\"ble\\.|network_ready' muse_proto.c muse_cfg.c muse_cfg_core.c muse_session.cpp muse_link.c muse_frame.c muse_push_core.c ../hub_core/hub_cmds.c ../hub_core/hub_events.c"
# 跨行、连字符串一起看（"Authorization: %s" 的字样在字符串里；参数可能写在下一行）——按语句切，不按行（2026-10-22 审计）
check "自写文件的日志里不打凭据（acc_tok / ref_tok / sdk_tok / access_token / refresh_token / Authorization / bearer）" "python3 -c \"
import re,sys,glob
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
bad=[]
for f in ['muse_proto.c','muse_cfg.c','muse_cfg_core.c','muse_link_core.c']+glob.glob('muse_session.cpp')+glob.glob('muse_link.c'):
    s=strip_comments(open(f).read(), keep_strings=True)
    for m in re.finditer(r'ESP_LOG\\w*\\s*\\((.*?)\\);', s, re.S):
        if re.search(r'(?i)acc_tok|ref_tok|sdk_tok|access_token|refresh_token|authorization|bearer', m.group(1)): bad.append(f)
sys.exit(1 if bad else 0)\""

# ---- 会话（M3 第四步：muse_frame / muse_session / muse_link；14.2 muse_link 行） ----
for f in muse_frame.h muse_frame.c muse_session.h muse_session.cpp muse_link_priv.h muse_link.c include/muse_link.h include/noise_upgrade.h include/ws_mask.h; do
    check "$f 存在" "[ -f $f ]"
done
check "muse_frame 不依赖 ESP-IDF，主机测试编产品源文件并真的执行" "$G --strings --absent -q '#\\s*include\\s*[<\"](esp_|freertos|nvs|sdkconfig)' muse_frame.c muse_frame.h && grep -qF '\$COMP/muse_frame.c' test_host/run.sh && grep -qx '\"\$T/muse_frame_test\"' test_host/run.sh"
check "会话任务用 xTaskCreateWithCaps + MALLOC_CAP_SPIRAM，没有退回内部内存的写法（14.2）" "$G -q 'xTaskCreateWithCaps\\s*\\(\\s*sess_task' muse_link.c && grep -A1 'xTaskCreateWithCaps(sess_task' muse_link.c | grep -q 'MALLOC_CAP_SPIRAM' && $G --absent -q '\\bpsram_task_create\\s*\\(|\\bxTaskCreate\\s*\\(' muse_link.c"
check "会话层不写 NVS（PSRAM 栈规则 1；凭据经 hub_store 落盘）" "$G --absent -q '\\bnvs_(set|commit|erase|open)\\w*\\s*\\(' muse_session.cpp muse_link.c muse_frame.c"
check "看门狗：wd_register 与 wd_kick 成对（主循环与会话轮询都 kick）" "[ \$($G 'wd_kick\\s*\\(s_wd\\)' muse_link.c | wc -l) -ge 2 ] && $G -q 'wd_register\\s*\\(&spec\\)' muse_link.c"
check "事件订阅有防重（s_inited），回调里只投队列（不阻塞，S2）" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('muse_link.c').read())
m=re.search(r'static void on_event\\(.*?\\{(.*?)\\n\\}', s, re.S)
b=m.group(1) if m else 'x'
init=re.search(r'esp_err_t muse_link_init\\(void\\)\\s*\\{(.*?)\\n\\}', s, re.S)
ok=m and 'post_event(' in b and not re.search(r'msess_|vm_api_|muse_cfg_|vTaskDelay|xSemaphoreTake', b) and init and 'if (s_inited) return ESP_OK;' in init.group(1)
sys.exit(0 if ok else 1)\""
check "device_family / model_id 只取 Kconfig 常量，代码里不写死（14.2）" "$G -q 'CONFIG_BITENGINE_MUSE_DEVICE_FAMILY' muse_link.c && $G -q 'CONFIG_BITENGINE_MUSE_MODEL_ID' muse_link.c && $G --strings --absent -q '\"(homehub|bitengine-hub-c5|esp-link)\"' muse_link.c muse_proto.c muse_session.cpp"
check "node_id 的格式与配对时逐字一致（muse_pair/pair_core.c；7.6：两处由这条钉住）" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
f=lambda p: re.findall(r'\"homelink-%02x%02x%02x\"', strip_comments(open(p).read(), keep_strings=True))
sys.exit(0 if f('muse_cfg_core.c') and f('../muse_pair/pair_core.c') else 1)\""
check "会话缓冲只从 PSRAM 分配，没有 malloc / 内部内存的退路（UPSTREAM.md 调用方义务）" "$G --absent -q '\\b(malloc|calloc|realloc)\\s*\\(|MALLOC_CAP_INTERNAL' muse_session.cpp && [ \$($G 'MALLOC_CAP_SPIRAM' muse_session.cpp | wc -l) -ge 2 ]"
check "ClientSession 在 msess_run 的作用域里（每次会话析构、清会话密钥），不是静态 / 全局" "$G -q '^\\s+ClientSession session\\(crypto\\);' muse_session.cpp && $G --absent -q '^(static\\s+)?ClientSession\\s' muse_session.cpp"
check "Noise msg3 的载荷是空的（鉴权靠 WS 升级头；上游 :805-822）" "$G -q 'WriteHandshakeMessage3\\(ConstByteSpan\\(\\),' muse_session.cpp"
check "入站帧处理失败（含 ResourceExhausted）一律断会话" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('muse_session.cpp').read())
sys.exit(0 if re.search(r'if \\(!r\\.ok\\(\\)\\) \\{[^}]*fail = true;', s) else 1)\""
check "所有设备令牌刷新的 access token 都传 NULL（被动和 30 天主动都是；上游 app.c:918-928 唯一调用点，第四步第一轮审计 ❌）" "[ \$($G 'vm_api_refresh_device_token\\s*\\(' muse_link.c | wc -l) -ge 2 ] && [ \$($G 'vm_api_refresh_device_token\\s*\\(' muse_link.c | wc -l) -eq \$($G 'vm_api_refresh_device_token\\(NULL, ref, s_node_id' muse_link.c | wc -l) ]"
check "30 天主动刷新记\"试过\"的时刻（不是成功的时刻；上游 app.c:909-917）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('muse_link.c').read())
b=re.search(r'if \\(s_bg_refresh_due\\) \\{(.*?)wd_kick', S, re.S).group(1)
sys.exit(0 if 0 <= b.find('s_tok_ms = now_ms()') < b.find('vm_api_refresh_device_token(') else 1)\""
check "配置值（SDK token / API 地址 / noise_host）在开机等存储之后才读，确定读到过之前不取 VM（第四步两轮审计 ❌）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('muse_link.c').read())
t=re.search(r'static void sess_task\\(void \\*arg\\)\\n\\{(.*?)\\n\\}', S, re.S).group(1)
f=re.search(r'static void do_fetch\\(void\\)\\n\\{(.*?)\\n\\}', S, re.S).group(1)
ok=0 <= t.find('muse_cfg_load_flags(') < t.find('setup_vm_api()') and t.count('setup_vm_api()')==1 \\
   and re.match(r'\\s*if \\(!s_cfg_loaded && !setup_vm_api\\(\\)\\) \\{\\s*step\\(MLE_VM_ERR\\);', f)
sys.exit(0 if ok else 1)\""
check "刷新拿到的新令牌落盘失败时先留在内存里用（keep_tokens），不直接丢" "$G -q 'keep_tokens\\(t\\.access_token, t\\.refresh_token\\)' muse_link.c && [ \$($G 'keep_tokens\\(' muse_link.c | wc -l) -ge 3 ] && $G --absent -q 'muse_cfg_save_tokens\\(t\\.' muse_link.c"
check "会话收包连续不断时定期让出 CPU（不饿死 IDLE / 任务看门狗，N7）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('muse_session.cpp').read())
sys.exit(0 if re.search(r'else if \(esp_timer_get_time\(\) - x\.last_yield_us >= YIELD_EVERY_MS \* 1000LL\) \{\s*vTaskDelay\(1\);', S) else 1)\""
check "/muse/unpair 先判 hubdb 就绪（删不掉时不回 200）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('../webui/muse_api.c').read(), keep_strings=True)
b=re.search(r'handle_muse_unpair\(httpd_req_t \*req\)\n\{(.*?)\n\}', S, re.S).group(1)
sys.exit(0 if re.search(r'if \(!hubdb_ready\(\)\) return send_error\(req, 503, \\\"hubdb_not_ready\\\"\);', b) and b.find('hubdb_ready()') < b.find('muse_link_unpair_local(') else 1)\""
check "主循环开头清掉会话外残留的 s_end_session（否则挡住 resync 补 NET_UP；核实审计）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('muse_link.c').read())
t=re.search(r'static void sess_task\(void \*arg\)\n\{(.*?)\n\}', S, re.S).group(1)
loop=t[t.find('for (;;) {'):]
sys.exit(0 if 0 <= loop.find('s_end_session = false;') < loop.find('resync()') else 1)\""
check "刷新拿到的令牌先校验再用（超长不截断着用：截断的令牌 401 会被当成被拒、清凭据）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('muse_link.c').read())
k=re.search(r'static bool keep_tokens\([^)]*\)\n\{(.*?)\n\}', S, re.S).group(1)
l=re.search(r'static esp_err_t load_pending\([^)]*\)\n\{(.*?)\n\}', S, re.S).group(1)
sys.exit(0 if k.find('muse_token_valid(acc') >= 0 and k.find('muse_token_valid(acc') < k.find('muse_cfg_save_tokens(') and 'n >= cap' in l and 'snprintf' not in l else 1)\""
check "没有 SDK token 时不取 VM（删 token 不等于解绑）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('muse_link.c').read())
f=re.search(r'static void do_fetch\(void\)\n\{(.*?)\n\}', S, re.S).group(1)
sys.exit(0 if re.search(r'if \(!s_sdk_tok_set\) \{\s*step\(MLE_VM_ERR\);', f) and f.find('s_sdk_tok_set') < f.find('vm_api_fetch_vms(') else 1)\""
check "注册时权限位读不准：按只读注册并排一次延迟重注册" "$G -q 'if \(muse_cfg_read_perm\(&s_reg_perm\) != ESP_OK\) \{' muse_link.c && $G -q 's_rereg_at_ms = now_ms\(\) \+ CFG_RETRY_MS;' muse_link.c && $G -q 'if \(s_rereg_at_ms && now_ms\(\) >= s_rereg_at_ms\)' muse_link.c"
check "对外接口要求任务真的起来了（start 失败时网页显示 not_running、解绑回 409，不假装成功）" "[ \$($G 'if \\(!s_inited \\|\\| !s_started' muse_link.c | wc -l) -ge 5 ] && $G -q 'if \\(!s_q \\|\\| !s_started\\) return ESP_ERR_INVALID_STATE' muse_link.c"
check "device.unpair：先删并读回确认、再回 accepted、最后才投 UNPAIRED_EVT（4.13；CLOSE 在先会把回包关掉）" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('muse_link.c').read())
m=re.search(r'if \\(mg == MP_MGMT_UNPAIR\\) \\{(.*?)\\n    \\}', s, re.S)
b=m.group(1) if m else ''
c=b.find('muse_cfg_clear_pairing('); r=b.find('msess_send('); e=b.find('step(MLE_UNPAIRED_EVT)')
sys.exit(0 if 0<=c<r<e else 1)\""
check "大块状态在 PSRAM（内部常驻增量 ≤ 2 KB，4.12）" "$G -q 's_big = heap_caps_calloc\\(1, sizeof\\(ml_big_t\\), MALLOC_CAP_SPIRAM' muse_link.c"
check "命令处理函数不输出任何网络密钥（附录 A hub.thread.network 的承诺）" "$G --absent -q 'NetworkKey|otThreadGetPskc|Pskc|ActiveDataset' ../hub_core/hub_cmds.c"

# ---- 第四步审计后补的（存储错误、事件丢失、控制流结束、秘密清零） ----
# 函数体：去注释后从 "static ... name(" 到第一个行首 "}"
FB="import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments; S=strip_comments(open('muse_link.c').read()); body=lambda n: (re.search(r'\n(?:static )?\w[\w ]*\b'+n+r'\(void\)\n\{(.*?)\n\}', S, re.S) or [None,''])[1]"
check "只有 ESP_ERR_NOT_FOUND 才走刷新 / 撤销；读令牌的其他错误按可重试退避（审计 ❌：超时一次就清凭据）" "python3 -c \"$FB
f=body('do_fetch'); r=body('do_refresh')
ok=re.search(r'if \(ge == ESP_ERR_NOT_FOUND\) \{[^}]*step\(MLE_VM_AUTH\);[^}]*\} else \{[^}]*step\(MLE_VM_ERR\);', f) \
 and re.search(r'if \(ge == ESP_ERR_NOT_FOUND\) \{[^}]*step\(MLE_REFRESH_REJECTED\);[^}]*\} else \{[^}]*step\(MLE_REFRESH_ERR\);', r) \
 and not re.search(r'muse_cfg_get_(access|refresh)_token\([^;]*\)\s*!=\s*ESP_OK', f+r)
sys.exit(0 if ok else 1)\""
check "muse_cfg：令牌读取只把\"确实没有\"映射成 NOT_FOUND（超时等原样返回）—— 行为由 muse_cfg_store_test 钉住且 run.sh 真的执行" "grep -qF '\$COMP/muse_cfg.c' test_host/run.sh && grep -q '\"\$T/muse_cfg_store_test\"' test_host/run.sh"
check "开机用 muse_cfg_load_flags 确定读到开关与配对状态才起状态机（不用读不到就算 false 的 muse_cfg_enabled / paired）" "python3 -c \"$FB
t=re.search(r'static void sess_task\(void \*arg\)\n\{(.*?)\n\}', S, re.S).group(1)
i=t.find('muse_cfg_load_flags('); j=t.find('mlc_init(')
sys.exit(0 if 0<=i<j and 'muse_cfg_enabled(' not in t and 'muse_cfg_paired(' not in t else 1)\""
check "每一步 step 之前刷新网络 / 内存（事件丢了内存闸门也看现值）" "python3 -c \"$FB
m=re.search(r'static mlc_out_t step\(mle_t ev\)\n\{(.*?)\n\}', S, re.S).group(1)
sys.exit(0 if m.find('mlc_set_env(')>=0 and m.find('mlc_set_env(')<m.find('mlc_step(') else 1)\""
check "resync 兜底：主循环空闲与会话轮询都调；补 NET_UP 只在 WAIT_NET，开关按存着的值" "python3 -c \"$FB
r=body('resync'); p=re.search(r'bool ml_session_poll\(void\)\n\{(.*?)\n\}', S, re.S).group(1); t=re.search(r'static void sess_task\(void \*arg\)\n\{(.*?)\n\}', S, re.S).group(1)
ok='resync()' in p and 'resync()' in t and re.search(r'MUSE_ST_WAIT_NET && net_online\(\)\) \{[^}]*step\(MLE_NET_UP\)', r) and 'muse_cfg_load_flags(' in r and 'MLE_NET_DOWN' not in r
sys.exit(0 if ok else 1)\""
check "post_event 把队列满报给调用方；set_enabled / unpair_local 原样返回它，reregister 走标志（丢不掉）" "python3 -c \"$FB
ok=re.search(r'static esp_err_t post_event\(mle_t ev\)', S) and all(re.search(r'esp_err_t '+n+r'\([^)]*\)\n\{[^}]*return post_event\(', S) for n in ('muse_link_set_enabled','muse_link_unpair_local')) and re.search(r'esp_err_t muse_link_reregister\(void\)\n\{[^}]*s_rereg_dirty = true;', S)
sys.exit(0 if ok else 1)\""
check "config_changed 不拿着状态锁读存储（先读后锁）；配置变更走标志、丢不掉" "python3 -c \"$FB
c=re.search(r'esp_err_t muse_link_config_changed\(void\)\n\{(.*?)\n\}', S, re.S).group(1)
lk=c[c.find('xSemaphoreTake('):c.find('xSemaphoreGive(')]
sys.exit(0 if 's_cfg_dirty = true' in c and 'muse_cfg_' not in lk and lk else 1)\""
check "上游 vm_api 的 free 不清零：令牌 / VM 列表只经清零的包装释放" "[ \$($G '\bvm_device_tokens_free\s*\(' muse_link.c | wc -l) -eq 1 ] && [ \$($G '\bvm_list_free\s*\(' muse_link.c | wc -l) -eq 1 ] && $G -q 'zero_str\(vms\[i\]\.vm_auth_token\)' muse_link.c && $G -q 'zero_str\(t->refresh_token\)' muse_link.c"
check "关掉 / 解绑时清掉内存里的 VM bearer 和没推出去的事件（N5）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('muse_link.c').read())
m=re.search(r'if \\(o\\.to == MUSE_ST_DISABLED \\|\\| o\\.to == MUSE_ST_UNPAIRED\\) \\{(.*?)\\n    \\}', S, re.S)
sys.exit(0 if m and 'clear_bearer();' in m.group(1) and 'push_clear();' in m.group(1) else 1)\""
check "选 VM 与 device.set_vm 用同一个 vm_derive_id 取标签（不拿 vm_connect_params 的 id 比）" "python3 -c \"$FB
p=re.search(r'static esp_err_t pick_vm\([^)]*\)\n\{(.*?)\n\}', S, re.S).group(1)
sys.exit(0 if 'vm_derive_id(vms[i].vm_url' in p and 'vm_connect_params' not in p else 1)\""
check "会话结束（STOPPED）但没有哪一步接手：兜底按断线处理" "python3 -c \"$FB
d=body('do_connect')
c=(re.search(r'case MSESS_STOPPED:(.*?)\n        break;', d, re.S) or [None,''])[1]
sys.exit(0 if 'MUSE_ST_REGISTERING' in c and 'MUSE_ST_ONLINE' in c and '!s_do_connect' in c and 'step(MLE_WS_ERR)' in c else 1)\""
check "注册没发出去算故障（FAILED），不算钩子要求结束" "$G -q 'result = x\.send_failed \? MSESS_FAILED : MSESS_STOPPED;' muse_session.cpp && [ \$($G 'x\.send_failed \? MSESS_FAILED : MSESS_STOPPED' muse_session.cpp | wc -l) -ge 2 ]"
check "服务端结束控制流（BodyChunk / Response 的 end_body）就断开重连；Response 自带的 body 照常解析" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('muse_session.cpp').read())
f=re.search(r'static FrameOutcome on_service_frame\(.*?\n\}', s, re.S).group(0)
ok=re.search(r'f\.body_chunk\.end_body\) \{[^}]*return FrameOutcome::Fail;', f) and re.search(r'f\.response\.end_body\) \{[^}]*return FrameOutcome::Fail;', f) and 'feed_control(x, ctl, f.response.body)' in f
sys.exit(0 if ok else 1)\""
check "hub_cmds 在会话任务（PSRAM 栈）里跑：不碰 flash（nvs / esp_partition / spi_flash / esp_flash）" "$G --absent -q '\b(nvs_\w+|esp_partition_\w+|spi_flash_\w+|esp_flash_\w+)\s*\(' ../hub_core/hub_cmds.c"
check "Thread 成员报 link_margin（附录 A），按 OT 的算法（RSSI − 噪声底，噪声底 = otPlatRadioGetReceiveSensitivity）" "$G --strings -q '\"link_margin\"' ../hub_core/hub_ti_core.c && $G --strings --absent -q '\"link_quality\", *[a-z]' ../hub_core/hub_ti_core.c ../hub_core/hub_cmds.c && $G -q 'otPlatRadioGetReceiveSensitivity\(inst\)' ../hub_core/thread_inventory.c && $G -q 'return \(rss == RSSI_INVALID \|\| m < 0\) \? 0u : \(unsigned\)m;' ../hub_core/hub_ti_core.c"

# ---- 推送（4.9、6.7；M3 第五步）----
check "muse_push_core 不依赖 ESP-IDF，主机测试编产品源文件并真的执行" "$G --strings --absent -q '#\s*include\s*[<\"](esp_|freertos|nvs|sdkconfig)' muse_push_core.c muse_push_core.h && grep -qF '\$COMP/muse_push_core.c' test_host/run.sh && grep -qx '\"\$T/muse_push_core_test\"' test_host/run.sh"
check "只在注册成功（ONLINE）之后推送" "$G -q 'if \(!s_end_session && s_mlc\.st == MUSE_ST_ONLINE\) push_pump\(t\);' muse_link.c && [ \$($G 'push_pump\(' muse_link.c | wc -l) -eq 2 ]"
check "推送走 POST /chat/stream，请求头照上游 muse_chat_link.c（x-request-id muse-%08x-%08x、x-app-id hatch-web、JSON）" "$G --strings -q '#define PUSH_PATH +\"/chat/stream\"' muse_link.c && $G --strings -q '\"muse-%08\" PRIx32 \"-%08\" PRIx32' muse_session.cpp && $G --strings -q 'StringView\(\"x-app-id\"\), StringView\(\"hatch-web\"\)' muse_session.cpp && $G --strings -q 'StringView\(\"Accept\"\), StringView\(\"application/json\"\)' muse_session.cpp && $G -q 'req\.end_body = true;' muse_session.cpp"
check "请求流（id ≥ 16）的帧交给 muse_link、不影响控制会话；控制流判定在它之后" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('muse_session.cpp').read())
f=re.search(r'static FrameOutcome on_service_frame\(.*?\n\}', S, re.S).group(0)
a=f.find('if (f.stream_id >= MSESS_REQ_BASE)'); b=f.find('if (f.stream_id != CTRL_STREAM_ID)')
blk=f[a:b]
sys.exit(0 if 0 <= a < b and blk.count('ml_session_response(')==3 and 'return x.send_failed ? FrameOutcome::Fail : FrameOutcome::Continue;' in blk else 1)\""
check "在途推送：发出去才标在途，回包按 2xx / 4xx / 其他三分，会话断了把在途的退回队列" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('muse_link.c').read())
p=re.search(r'static void push_pump\(uint64_t now\)\n\{(.*?)\n\}', S, re.S).group(1)
f=re.search(r'static void push_finish\(int status, uint64_t now\)\n\{(.*?)\n\}', S, re.S).group(1)
c=re.search(r'static void do_connect\(void\)\n\{(.*?)\n\}', S, re.S).group(1)
ok=0 <= p.find('msess_post(') < p.find('mpq_begin(') and all(x in f for x in ('MPQ_REPLY_SENT','mpq_mark_sent(','MPQ_REPLY_REJECTED','mpq_drop(','mpq_abort(')) \
   and 0 <= c.find('msess_run(') < c.find('push_session_ended()')
sys.exit(0 if ok else 1)\""
check "侧边会话 id 一时读不到不生成新的（否则另开一个侧边会话）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('muse_link.c').read())
b=re.search(r'static bool chat_sid_ready\(void\)\n\{(.*?)\n\}', S, re.S).group(1)
sys.exit(0 if 0 <= b.find('if (e != ESP_ERR_NOT_FOUND) return false;') < b.find('mp_format_uuid(') else 1)\""
check "测试推送：不在线 ESP_ERR_NOT_ALLOWED、走标志；端点 409 / 202（守卫在认证之前）" "$G -q 'if \(st != MUSE_ST_ONLINE\) return ESP_ERR_NOT_ALLOWED;' muse_link.c && $G -q 's_test_push = true;' muse_link.c && python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('../webui/muse_api.c').read(), keep_strings=True)
b=re.search(r'static esp_err_t handle_muse_test_push\(httpd_req_t \*req\)\n\{(.*?)\n\}', S, re.S).group(1)
ok=0 <= b.find('CROSS_ORIGIN_GUARD(req);') < b.find('AUTH_REQUIRED(req);') and 'ESP_ERR_NOT_ALLOWED' in b and '202 Accepted' in b
sys.exit(0 if ok else 1)\""
check "推送计数进状态快照（pushes_sent / dropped / queued；14.5 用它证明 notify 关了不推）" "$G -q 's_st\.pushes_sent = s_mpq\.sent;' muse_link.c && $G -q 's_st\.pushes_dropped = mpq_dropped_total\(&s_mpq\)' muse_link.c"
check "BLE 自检的退化只报这次开机看着它跑完的（上次存的结果不重复报）" "$G --strings -q 'if \(strcmp\(sc, \"pending\"\) == 0\)' muse_link.c && $G --strings -q 'else if \(s_sc_seen == 1 && strncmp\(sc, \"degraded\", 8\) == 0\)' muse_link.c"

check "关着 / 没配对时事件不进推送队列（只进事件环）；其余一个事件一条、直接入队（不合并、不去重、不限配额：网关不做人为干预，2026-10-24 用户定）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('muse_link.c').read())
b=re.search(r'static void push_collect\(uint64_t now\)\n\{(.*?)\n\}', S, re.S).group(1)
ok='s_mlc.st != MUSE_ST_DISABLED && s_mlc.st != MUSE_ST_UNPAIRED' in b and 'if (!accept) continue;' in b
w=b[b.find('while (hub_events_take(&ev))'):b.find('if (s_test_push && !accept)')]
ok = ok and 'mpq_enqueue(&s_mpq, &m, now)' in w and not re.search(r'mpm_|dedup|is_recent|NO_DEDUP|window', w)
C=strip_comments(open('muse_push_core.c').read()) + strip_comments(open('muse_push_core.h').read())
ok = ok and not re.search(r'mpm_|dedup|quota|recent|MPQ_HOUR_CAP|MPQ_DAY_CAP|MPQ_ALARM_CAP|WINDOW', C)
sys.exit(0 if ok else 1)\""
check "在途推送：应答头到了就按头判（2xx 送达不重发、4xx 丢、5xx 重试），头之后的复位不覆盖它；没到头按断线 / 超时、30 s 后再发" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('muse_link.c').read())
e=re.search(r'static void push_session_ended\(void\)\n\{(.*?)\n\}', S, re.S).group(1)
p=re.search(r'static void push_pump\(uint64_t now\)\n\{(.*?)\n\}', S, re.S).group(1)
r=re.search(r'void ml_session_response\([^)]*\)\n\{(.*?)\n\}', S, re.S).group(1)
ok='push_finish(s_px_status > 0 ? s_px_status : -1,' in e and 'push_finish(s_px_status > 0 ? s_px_status : 0, now);' in p \
   and 'if (status > 0 || (status < 0 && s_px_status <= 0)) s_px_status = status;' in r
sys.exit(0 if ok else 1)\""
check "关掉 / 解绑只清排着的：计数保留" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('muse_link.c').read())
b=re.search(r'static void push_clear\(void\)\n\{(.*?)\n\}', S, re.S).group(1)
sys.exit(0 if 'mpq_clear_queue(&s_mpq);' in b and 'mpq_init' not in b else 1)\""
check "主机循环每圈都调 resync（只在等超时才调会被持续的叫醒饿死）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('muse_link.c').read())
t=re.search(r'static void sess_task\(void \*arg\)\n\{(.*?)\n\}', S, re.S).group(1)
loop=t[t.find('for (;;) {'):]
sys.exit(0 if re.search(r'step\(MLE_TIMER\);\s*\}\s*resync\(\);', loop) else 1)\""
check "测试推送也记进事件环（Muse 会用 hub.events.recent 核对）" "$G --strings -q 'hub_event_emit\(HUB_EVC_GATEWAY, NULL, \"test_push\", k_test, 0\)' muse_link.c"
check "叫醒消息合并成一条（不让推送把 16 格事件队列挤满、挤掉解绑）" "$G -q 'if \(s_wake_pending \|\| !s_q\) return;' muse_link.c && $G -q 's_wake_pending = false;' muse_link.c"
check "推送句子只写事实不写指令（MD-10）：thread_degraded 那句没有 unplug / restart 之类" "$G --strings --absent -q '\"[^\"]*(unplug|plug it back|power cycle)[^\"]*\"' muse_link.c ../hub_core/hub_events.c"

# ---- 审计、限速顺序、故障注入（6.10、6.5、TC-MU09；M3 第七步）----
check "每次调用（含 4 个管理命令）都记审计；限速在权限检查之前（6.5 注记）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('muse_link.c').read())
d=re.search(r'static void dispatch\(const mp_invoke_t \*inv\)\n\{(.*?)\n\}', S, re.S).group(1)
i=re.search(r'static const char \*dispatch_inner\(const mp_invoke_t \*inv\)\n\{(.*?)\n\}', S, re.S).group(1)
ok='hub_audit_record(HAU_SRC_MUSE, inv->command, inv->params, res,' in d and 'return handle_mgmt(mg, inv);' in i \
   and 0 <= i.find('hub_rl_take(') < i.find('hub_policy_check_cmd(')
sys.exit(0 if ok else 1)\""
check "推送过期丢弃记审计（4.9）；推送类别开关读不到沿用现值、不挡取 VM" "$G --strings -q 'hub_audit_record\(HAU_SRC_MUSE, \"push.expired\", NULL, res, 0\);' muse_link.c && $G -q 'if \(notify_ok\) hub_events_set_notify\(nm\);' muse_link.c"
check "TC-MU09 故障注入只在诊断构建（CONFIG_BITENGINE_DIAG_MUSE_FAULT）" "[ \$($G -q '#if CONFIG_BITENGINE_DIAG_MUSE_FAULT' muse_link.c && $G '#if CONFIG_BITENGINE_DIAG_MUSE_FAULT' muse_link.c | wc -l) -eq 2 ] && grep -q 'config BITENGINE_DIAG_MUSE_FAULT' ../../main/Kconfig.projbuild && grep -qx '# CONFIG_BITENGINE_DIAG_MUSE_FAULT is not set' ../../sdkconfig.direct"
internal "TC-MU09 发版脚本拒发诊断构建（publish_ota.sh 认 CONFIG_BITENGINE_DIAG_）" "grep -q 'CONFIG_BITENGINE_DIAG_' ../../tools/publish_ota.sh"
check "本地出口 /hub/invoke：守卫在认证之前、形状预检后才进 cJSON、写类命令要密码、记审计（rest）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('../webui/hub_api.c').read())
h=re.search(r'static esp_err_t handle_invoke\(httpd_req_t \*req\)\n\{(.*?)\n\}', S, re.S).group(1)
r=re.search(r'static const char \*run_command\([^)]*\)\n\{(.*?)\n\}', S, re.S).group(1)
ok=0 <= h.find('CROSS_ORIGIN_GUARD(req);') < h.find('AUTH_REQUIRED(req);') and 0 <= h.find('hub_json_shape_ok(') < h.find('cJSON_ParseWithLength(') \
   and 'hub_audit_record(HAU_SRC_REST,' in h and 'cmd->perm != HUB_PERM_READ && !config_mgr_has_password()' in r \
   and 0 <= r.find('hub_params_check(') < r.find('hub_rl_take(') and 'HUB_SRC_REST' in r
sys.exit(0 if ok else 1)\""
check "/muse/config：开敏感权限要密码（409）、报警关不掉、perm / notify 的键必须认识；改权限位重新注册" "$G --strings -q '\"alarm_cannot_be_disabled\"' ../webui/muse_api.c && $G -q '\(perm_new & HUB_PERMBIT_SENSITIVE\) && !\(perm_cur & HUB_PERMBIT_SENSITIVE\) && !config_mgr_has_password\(\)' ../webui/muse_api.c && $G -q 'if \(name \|\| \(perm && \(perm_new != perm_cur \|\| perm_bad\)\)\) \(void\)muse_link_reregister\(\);' ../webui/muse_api.c"
check "/muse/config 与 /unpair 进 cJSON 前过形状预检；权限位存储格式坏时照只读显示、给了 perm 就覆盖（不永久 503）；/muse/status 报生效中的推送掩码（第七步审计）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('../webui/muse_api.c').read())
def fn(n): return re.search(r'static esp_err_t '+n+r'\(httpd_req_t \*req\)\n\{(.*?)\n\}', S, re.S).group(1)
post, unp, get, st = fn('handle_muse_config_post'), fn('handle_muse_unpair'), fn('handle_muse_config_get'), fn('handle_muse_status')
ok=all(0 <= f.find('hub_json_shape_ok(') < f.find('cJSON_Parse(') for f in (post, unp)) \
   and 'perm_format_bad(pe)' in post and 'perm_format_bad(pe)' in get and 'B(perm_bad)' in get \
   and 'perm && (perm_new != perm_cur || perm_bad)) err = muse_cfg_set_perm(perm_new);' in post \
   and 'hub_events_get_notify()' in st
sys.exit(0 if ok else 1)\""
check "回了错的调用都记审计：Muse 侧 busy / 超长或嵌套 / INVOKE_BAD，REST 侧 body 过大 / 坏 JSON / 缺 command（6.10，第七步审计）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('muse_link.c').read())
m=re.search(r'bool ml_session_message\(const char \*json, size_t len\)\n\{(.*?)\n\}', S, re.S).group(1)
H=strip_comments(open('../webui/hub_api.c').read())
h=re.search(r'static esp_err_t handle_invoke\(httpd_req_t \*req\)\n\{(.*?)\n\}', H, re.S).group(1)
ok=m.count('hub_audit_record(HAU_SRC_MUSE,') >= 3 and 'hub_audit_record(HAU_SRC_MUSE, inv.command, inv.params, ' in m \
   and h.count('hub_audit_record(HAU_SRC_REST, ') >= 4
sys.exit(0 if ok else 1)\""
check "推送取队列时再按推送类别复核（事件由 hub_svc 产生，可能早于本任务读进用户设置；报警恒推）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('muse_link.c').read())
f=re.search(r'static void push_collect\\(uint64_t now\\)\\n\\{(.*?)\\n\\}', S, re.S).group(1)
w=f[f.find('while (hub_events_take(&ev))'):]
ok='if (ev.cls != HUB_EVC_ALARM && !(hub_events_get_notify() & (1u << ev.cls))) continue;' in w and w.find('hub_events_get_notify()') < w.find('mpq_enqueue(')
sys.exit(0 if ok else 1)\""

# ---- 注册的后端就绪位（CHECKLIST C17；M5 第三步）：zhub 起来后注册 hub.device.update，控制 / 入网各等自己的前提 ----
check "current_backends 报 zhub（开了 zhub、任务在跑才报 HUB_BACKEND_ZHUB | HUB_BACKEND_CONTROL：控制路径 M5 第七步做了）；matter_ctrl 在跑才报 HUB_BACKEND_MCTRL | HUB_BACKEND_CONTROL（M7 第三步 / 第六步，只在 MATTER_CTRL 编译开关里）；不报 ZB_JOIN（入网前提 Z21 没满足）；Muse 调用带上当前权限位（敏感动作判定）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('muse_link.c').read())
f=re.search(r'\nstatic uint8_t current_backends\(void\)\n\{(.*?)\n\}', S, re.S).group(1)
ok = '#if CONFIG_BITENGINE_ENABLE_ZHUB' in f
mc = re.search(r'#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL\n(.*?)#endif', f, re.S)
ok = ok and mc and re.search(r'if \(mc\.state == MATTER_CTRL_RUNNING\) b \|= HUB_BACKEND_MCTRL \| HUB_BACKEND_CONTROL;', mc.group(1)) and 'HUB_BACKEND_MCTRL' not in f.replace(mc.group(0), '')
ok = ok and set(re.findall(r'HUB_BACKEND_\w+', f)) == {'HUB_BACKEND_ZHUB', 'HUB_BACKEND_CONTROL', 'HUB_BACKEND_MCTRL'} and re.search(r'if \(zhub_started\(\)\) b \|= HUB_BACKEND_ZHUB \| HUB_BACKEND_CONTROL;', f) and set(re.findall(r'\b\d\w*\b', f)) <= {'0'} and re.findall(r'return[^;]*;', f) == ['return b;']
ok = ok and 'HUB_BACKEND_ZB_JOIN' not in S and 'HUB_BACKEND_ALL' not in S and re.search(r'hub_call_t call = \{[^}]*\.perm = muse_cfg_get_perm\(\)', S)
sys.exit(0 if ok else 1)\""
check "后端变了（zhub 比 Muse 晚起：started 只会 false → true 一次）就重注册：resync 比现值与 s_reg_backends（只在线时）；注册时先取现值再建命令表；分发只认这次注册的位（4.8）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('muse_link.c').read())
f=re.search(r'\nstatic void resync\(void\)\n\{(.*?)\n\}', S, re.S).group(1)
ok = re.search(r'const uint8_t be_now = current_backends\(\);', f) and re.search(r's_mlc\.st == MUSE_ST_ONLINE && be_now != s_reg_backends\)\s*\{[^}]*s_rereg_dirty = true;', f, re.S)
g=re.search(r'\nstatic void send_register\(void\)\n\{(.*?)\n\}', S, re.S).group(1)
ok = ok and g.find('s_reg_backends = current_backends();') >= 0 and g.find('s_reg_backends = current_backends();') < g.find('mp_build_register(') \
     and re.search(r'mp_build_register\([^;]*s_reg_perm, s_reg_backends\)', g)
ok = ok and len(re.findall(r'mp_dispatch\(', S)) == 1 and re.search(r'mp_dispatch\(inv->command, s_reg_perm, s_reg_backends,', S)
sys.exit(0 if ok else 1)\""
check "/muse/status 报这次注册用的后端位 reg_backends（test_api 证明命令表真的变了，CLAUDE.md 7.5）" "grep -q 's_st.reg_backends = s_reg_backends;' muse_link.c && grep -qF '\\\"reg_backends\\\":%u' ../webui/muse_api.c && grep -q '(unsigned)ls.reg_backends' ../webui/muse_api.c"

# ---- 主机侧测试 ----
check "test_host/run.sh 存在" "[ -f test_host/run.sh ]"
check "run.sh 跑 vm_api 与凭据隔离两组" "grep -q 'tests.test_link_vm_api tests.test_link_vm_auth_token_contract' test_host/run.sh"
check "run.sh 把\"跳过\"当失败" "grep -q '! grep -qE \"skipped\"' test_host/run.sh"

# ---- 推送去向（设计 6.7；2026-10-23 上板对照：侧边会话在 App 里找不到，用户定一律进主会话）----
check "推送一律进主会话：真推送用 MUSE_PUSH_MAIN_CHAT（= true），测试推送默认主会话、?main=0 才走侧边会话" "grep -q '^#define MUSE_PUSH_MAIN_CHAT true' muse_link.c && grep -q 'm.main_chat = MUSE_PUSH_MAIN_CHAT;' muse_link.c && ! grep -q 'ev.cls == HUB_EVC_ALARM, now' muse_link.c && grep -q \"v\[0\] == '0'\" ../webui/muse_api.c"

check "推送正文带来源前缀 [bitEngine Hub]（拼请求体时加；2026-10-23 用户定）" "grep -q '#define MUSE_PUSH_PREFIX   \"\\[bitEngine Hub\\] \"' muse_push_core.h && grep -q 'snprintf(msg, sizeof(msg), \"%s%s\", MUSE_PUSH_PREFIX, m->text);' muse_push_core.c"
check "处理函数里的 forbidden / rate_limited 计进 invokes_denied（不是 failed）" "grep -q 'st_count(!strcmp(code, \"forbidden\") || !strcmp(code, \"rate_limited\") ? &s_st.invokes_denied : &s_st.invokes_failed);' muse_link.c"


# ---- 安全模式退出时"先关掉 Muse"（附录 E33，2026-10-23 用户定 B）----
check "muse_hold.c 在 SRCS 里，且在 if(CONFIG_BITENGINE_ENABLE_MUSE_LINK) 块内（HA 型不编）" "python3 -c \"
import re,sys
S=open('CMakeLists.txt').read()
m=re.search(r'if\(CONFIG_BITENGINE_ENABLE_MUSE_LINK\)(.*?)endif\(\)', S, re.S)
sys.exit(0 if m and '\\\"muse_hold.c\\\"' in m.group(1) and S.count('muse_hold.c')==1 else 1)\""
check "apply：flush 之后读回确认已是关才擦留言（flush 回 OK 不等于那条写进去了）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('muse_hold.c').read())
f=re.search(r'\nesp_err_t muse_hold_apply\(void\)\n\{(.*?)\n\}', S, re.S).group(1)
p=[f.find(x) for x in ('hub_store_flush(','muse_cfg_load_flags(','if (err == ESP_OK && en) err =','nvs_erase_key(')]
sys.exit(0 if -1 not in p and p==sorted(p) else 1)\""
check "/muse/config 打开 Muse 时作废留言（用户最新的选择为准）" "grep -q 'if (cJSON_IsTrue(en)) muse_hold_clear();' ../webui/muse_api.c"
check "留言当场落盘：request 里 nvs_set_u8 之后 nvs_commit（调用方紧接着重启，不能走防抖）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('muse_hold.c').read())
f=re.search(r'\nesp_err_t muse_hold_request\(void\)\n\{(.*?)\n\}', S, re.S).group(1)
a,b=f.find('nvs_set_u8('),f.find('nvs_commit(')
sys.exit(0 if 0<=a<b and 'hub_store' not in f else 1)\""
check "apply 先只读打开（READWRITE 会把不存在的命名空间建出来、每次开机写 flash）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('muse_hold.c').read())
f=re.search(r'\nesp_err_t muse_hold_apply\(void\)\n\{(.*?)\n\}', S, re.S).group(1)
opens=re.findall(r'nvs_open\(HOLD_NS, (NVS_\w+)', f)
sys.exit(0 if opens and opens[0]=='NVS_READONLY' else 1)\""
check "apply：先写关 + hub_store_flush，落盘了才擦留言（顺序 + 失败就 return）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('muse_hold.c').read())
f=re.search(r'\nesp_err_t muse_hold_apply\(void\)\n\{(.*?)\n\}', S, re.S).group(1)
p=[f.find(x) for x in ('muse_cfg_set_enabled(false)','hub_store_flush(','nvs_erase_key(')]
ok = -1 not in p and p==sorted(p)
g=f[p[1]:p[2]]
ok = ok and re.search(r'if \(err != ESP_OK\) \{[^}]*return err;', g)
sys.exit(0 if ok else 1)\""
check "main.c：muse_hold_apply 在 muse_link_init 之前，失败就不起 muse_link" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('../../main/main.c').read())
sys.exit(0 if re.search(r'if \(muse_hold_apply\(\) != ESP_OK\) \{[^}]*\} else if \(muse_link_init\(\) != ESP_OK', S) and len(re.findall(r'muse_link_init\(\)', S))==1 else 1)\""
check "/muse/status 报 hold_pending（test_api 读回留言有没有被消费，CLAUDE.md 7.5）" "grep -qF '\\\"hold_pending\\\":%s' ../webui/muse_api.c && grep -q 'muse_hold_pending() ? \"true\" : \"false\"' ../webui/muse_api.c"

echo ""; echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
