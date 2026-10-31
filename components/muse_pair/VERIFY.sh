#!/bin/bash
# components/muse_pair/VERIFY.sh — Muse 社区配对 v5 的引入约束（docs/15_muse_direct_design_v1.md 第五节、附录 C）
# ⚠️ 必须 cd 进本目录跑：bash VERIFY.sh
#
# 这里查代码形状与引入规矩；协议行为（握手、与 App 共享的 v5 向量、签名者策略、固件侧密钥派生）在 test_host/run.sh。
# 胶水层（pair_ble.c / muse_pair.c / pair_core.c）的检查在后半部分。
# ⚠️ 先跑 bash tools/build_profiles.sh（查的是 build-direct/ 的产物），并 source IDF 的 export.sh（要 riscv32-esp-elf-nm）。

PASS=0; FAIL=0
check() {
    if eval "$2" > /dev/null 2>&1; then echo "✅ $1"; PASS=$((PASS+1))
    else echo "❌ $1"; FAIL=$((FAIL+1)); fi
}
G="python3 ../../tools/grep_code.py"

# ---- 只属于直连型 + Muse 开关（架构 5.8 R-3；HA 型里这个组件是空的） ----
check "CMake：源文件只在 BITENGINE_ENABLE_MUSE_LINK 下给" "grep -Pzo 'set\\(_muse_pair_srcs \"\"\\)\\s*\\nif\\(CONFIG_BITENGINE_ENABLE_MUSE_LINK\\)' CMakeLists.txt"
check "CMake：依赖不放进 if（预展开读不到 CONFIG_*）" "python3 -c \"import re,sys; s=open('CMakeLists.txt').read(); i=s.find('if(CONFIG_BITENGINE_ENABLE_MUSE_LINK)'); e=s.find('endif()', i); sys.exit(1 if 'REQUIRES' in s[i:e] else 0)\""
check "Kconfig：BITENGINE_ENABLE_MUSE_LINK depends on 直连型 && SPIRAM，select BT，默认 n" "grep -Pzo 'config BITENGINE_ENABLE_MUSE_LINK\\n\\s+bool [^\\n]+\\n\\s+depends on BITENGINE_PROFILE_DIRECT && SPIRAM\\n\\s+select BT_ENABLED\\n\\s+default n\\n' ../../main/Kconfig.projbuild"
check "直连型自己的 defaults 写死 NimBLE（choice 成员 select 无效，别靠 HA 型 defaults）、host 栈 ≥ 6 KB" "grep -qx 'CONFIG_BT_NIMBLE_ENABLED=y' ../../sdkconfig.defaults.direct && grep -qx 'CONFIG_BT_NIMBLE_ENABLED=y' ../../sdkconfig.direct && [ \$(sed -n 's/^CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=//p' ../../sdkconfig.direct) -ge 6144 ]"
check "直连型默认打开、入库的 sdkconfig.direct 跟着" "grep -qx 'CONFIG_BITENGINE_ENABLE_MUSE_LINK=y' ../../sdkconfig.defaults.direct && grep -qx 'CONFIG_BITENGINE_ENABLE_MUSE_LINK=y' ../../sdkconfig.direct"
check "HA 型 sdkconfig 里没有这个开关（看不见 = 打不开）" "! grep -q 'BITENGINE_ENABLE_MUSE_LINK' ../../sdkconfig"

# ---- 原样引入（附录 C）：协议文件与上游固定提交逐字节一致，改动必须先在 UPSTREAM.md 登记 ----
for f in link_pairing.c pairing_transcript.c pairing_signer_policy.c include/link_pairing.h \
         include/pairing_transcript.h include/pairing_signer_policy.h LICENSE; do
    check "$f 存在" "[ -f $f ]"
done
check "引入文件与 UPSTREAM.md 清单一致（check_upstream_files.sh）" "bash ../muse_link/check_upstream_files.sh"

# ---- 只用社区模式（附录 C：不启用厂商 eFuse 认证） ----
# 查构建产物里的事实，不 grep CMake / Kconfig 文本：文本检查会被"为什么不定义它"的注释绊倒（7.4），
# glob 匹配不到文件时 grep 退出码 2、取反后恒绿（2026-10-22 审计抓到的空过）。先跑 build_profiles.sh 再跑本脚本。
CC_JSON=../../build-direct/compile_commands.json
check "直连型产物在（下面三条查的是 build-direct/）" "[ -f $CC_JSON ] && [ -f ../../build-direct/config/sdkconfig.h ] && [ -f ../../build-direct/esp-idf/muse_pair/libmuse_pair.a ]"
check "上游三个源文件的编译命令都带 AUTH_EPOCH=1、都不带 EFUSE_AUTH" "python3 -c \"
import json,sys
up=('link_pairing.c','pairing_transcript.c','pairing_signer_policy.c')
cmds=[e.get('command','') for e in json.load(open('$CC_JSON')) if '/components/muse_pair/' in e['file'] and e['file'].endswith(up)]
sys.exit(0 if len(cmds)==3 and all('-DCONFIG_HOMEHUB_PAIRING_AUTH_EPOCH=1' in c and 'EFUSE_AUTH' not in c for c in cmds) else 1)\""
check "sdkconfig.h 里没有任何 HOMEHUB_* 符号（上游 Kconfig 一个都没进来）" "[ -f ../../build-direct/config/sdkconfig.h ] && ! grep -q 'HOMEHUB_' ../../build-direct/config/sdkconfig.h"
check "libmuse_pair.a 不引用 eFuse / 硬件签名（社区模式的结果）" "[ -z \"\$(riscv32-esp-elf-nm ../../build-direct/esp-idf/muse_pair/libmuse_pair.a | grep -E ' (esp_efuse_|hardware_sign_hash)')\" ] && riscv32-esp-elf-nm ../../build-direct/esp-idf/muse_pair/libmuse_pair.a | grep -q ' T link_pairing_init'"
check "CMake：REQUIRES / PRIV_REQUIRES 的参数里没有变量（预展开时变量还是空的）" "python3 -c \"import re,sys; sys.exit(1 if re.search(r'REQUIRES[^)]*[$][{]', open('CMakeLists.txt').read()) else 0)\""

# ---- S1：SDK token 不进日志（同 muse_link/VERIFY.sh；上游 link_pairing.c:1058 以 INFO 打 token 前 12 个字符） ----
check "link_pairing.c 的编译命令带 LOG_LOCAL_LEVEL=ESP_LOG_WARN" "python3 -c \"
import json,sys
c=[e['command'] for e in json.load(open('$CC_JSON')) if e['file'].endswith('/components/muse_pair/link_pairing.c')]
sys.exit(0 if len(c)==1 and '-DLOG_LOCAL_LEVEL=ESP_LOG_WARN' in c[0] else 1)\""
check "libmuse_pair.a 里没有打 SDK token 的格式串（且 W/E 日志还在）" "[ -f ../../build-direct/esp-idf/muse_pair/libmuse_pair.a ] && ! strings ../../build-direct/esp-idf/muse_pair/libmuse_pair.a | grep -q 'SDK token %' && strings ../../build-direct/esp-idf/muse_pair/libmuse_pair.a | grep -q 'no memory to decrypt'"
check "上游 link_pairing 里带 SDK token 的日志都在 INFO 及以下" "! grep -nE 'ESP_LOG[EW]\\s*\\(.*sdk_token' link_pairing.c"

# ---- 主机侧测试（P1：协议行为靠它，不靠 grep） ----
for f in test_host/run.sh test_host/kdf_vectors_harness.c test_host/test_kdf_vectors.py; do
    check "$f 存在" "[ -f $f ]"
done
check "run.sh 跑全部 4 组（上游 3 组 + 自加的固件侧密钥派生）" "grep -q 'tests.test_link_pairing_transcript tests.test_link_pairing_signer_policy' test_host/run.sh && grep -q 'tests.test_link_pairing_handshake tests.test_kdf_vectors' test_host/run.sh"
check "run.sh 把\"跳过\"当失败（缺 IDF/cryptography 时上游测试会静默跳过）" "grep -q '! grep -qE \"skipped\"' test_host/run.sh"
check "自加测试调的是固件自己的 derive_session_keys / aes_gcm_decrypt" "$G -q 'derive_session_keys\\s*\\(' test_host/kdf_vectors_harness.c && $G -q 'aes_gcm_decrypt\\s*\\(\\s*s_rx_key' test_host/kdf_vectors_harness.c"

# ---- pair_ble.c：GATT 外设（设计 5.2；改写自上游 ble_server.c） ----
for f in pair_ble.c pair_ble.h pair_core.c pair_core.h muse_pair.c include/muse_pair.h test_host/pair_core_test.c; do
    check "$f 存在" "[ -f $f ]"
done
check "UUID 字节从 UUID 字符串独立验算（手抄过一次全错，2026-10-22）" "python3 -c \"
import re,sys,uuid
s=open('pair_ble.c').read()
want={'SVC_UUID':'7fdd3d1c-38ea-46cf-8b46-314ecf5f240c','RX_UUID':'4d593029-28a2-4a6e-a1f0-3c2d5e8f9b01','TX_UUID':'d75dc4ca-7b2b-4e9c-8f0a-1d2e3f4a5b6c'}
ok=True
for name,u in want.items():
    m=re.search(name+r'\s*=\s*BLE_UUID128_INIT\(([^)]*)\)', s)
    got=bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})', m.group(1))) if m else b''
    ok = ok and got == uuid.UUID(u).bytes[::-1]
sys.exit(0 if ok else 1)\""
check "分片上限照上游：收 8192 B、发 160 B/片" "$G -q 'MAX_RX_TOTAL_BYTES\\s+8192' pair_ble.c && $G -q 'MAX_NOTIFY_CHUNK\\s+160' pair_ble.c"
check "明文与解密后的帧都先过 JSON 深度检查再解析" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('pair_ble.c').read())
m=re.search(r'static\s+void\s+dispatch_command_ex\s*\([^)]*\)\s*\{(.*?)\n\}', s, re.S)
b=m.group(1) if m else ''
sys.exit(0 if 0<=b.find('pair_json_depth_valid(')<b.find('cJSON_Parse') else 1)\""
check "不实现 BLE OTA / unpair / 上游 ble.* 命令（设计 5.2；加密送来一律 error_unknown_action）" "$G --strings --absent -q '\"(device\\.)?ota\"|\"unpair\"|\"ble\\.' pair_ble.c muse_pair.c"
check "不发 network_ready（设计 5.3）" "$G --strings --absent -q 'network_ready' pair_ble.c muse_pair.c"
check "provision_v2 按 Linux SDK 判（密码可为空串），不照上游 ESP32 的非空要求" "$G -q 'pair_provision_ok\\s*\\(' pair_ble.c && $G --absent -q '!\\s*\\*\\s*(s|p)->valuestring' pair_ble.c"
check "解密后的命令用完清零（里面有令牌）" "$G -q 'if\\s*\\(decrypted\\)\\s*wipe_json_strings' pair_ble.c && $G -q 'mbedtls_platform_zeroize\\s*\\(\\s*plain' pair_ble.c"
check "所有等待有上限：没有 portMAX_DELAY（上游重组与发送用的是它）" "$G --absent -q 'portMAX_DELAY' pair_ble.c muse_pair.c"
check "host 回调只投递：post() 是 0 等待的 xQueueSend" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('muse_pair.c').read())
m=re.search(r'static\s+bool\s+post\s*\([^)]*\)\s*\{(.*?)\n\}', s, re.S)
sys.exit(0 if m and re.search(r'xQueueSend\s*\([^;]*,\s*0\s*\)', m.group(1)) else 1)\""

# ---- muse_pair.c：配对流程（设计 5.1–5.6） ----
check "先发加密的 confirm_required，再使能按键（上游：提前按下不能抢在手机期待的记录前面）" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('muse_pair.c').read(), keep_strings=True)
m=re.search(r'static\s+void\s+on_client_finished\s*\([^)]*\)\s*\{(.*?)\n\}', s, re.S)
b=m.group(1) if m else ''
sys.exit(0 if 0<=b.find('confirm_required')<b.find('link_pairing_arm_confirmation(') else 1)\""
check "fetch_vms 只在 provision 任务里调（最坏一分多钟，不能在 host 任务 / httpd / pair 任务里）" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('muse_pair.c').read())
m=re.search(r'static\s+void\s+prov_task\s*\([^)]*\)\s*\{(.*?)\n\}', s, re.S)
sys.exit(0 if m and s.count('vm_api_fetch_vms(')==1 and 'vm_api_fetch_vms(' in m.group(1) else 1)\""
check "provision 任务：内部栈 8 KB（xTaskCreate，不是 PSRAM 栈；上游同值）" "$G -q 'PROV_TASK_STACK\\s+8192' muse_pair.c && $G --strings -q 'xTaskCreate\\s*\\(\\s*prov_task' muse_pair.c && $G --absent -q 'WithCaps' muse_pair.c"
check "验证通过才落盘：fetch_vms 在 commit 之前，落盘经 link_pairing_commit_provisioning + muse_cfg_save_pairing" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('muse_pair.c').read(), keep_strings=True)
m=re.search(r'static\s+void\s+prov_task\s*\([^)]*\)\s*\{(.*?)\n\}', s, re.S)
b=m.group(1) if m else ''
f=b.find('vm_api_fetch_vms('); c=b.find('link_pairing_commit_provisioning('); a=b.find('auth_ok')
sys.exit(0 if 0<=f<c<a and 'muse_cfg_save_pairing(' in s else 1)\""
check "结束时：存结果 → 记计划内重启（muse_pair_exit）→ esp_restart" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('muse_pair.c').read())
m=re.search(r'static\s+void\s+finish_and_restart\s*\([^)]*\)\s*\{(.*?)\n\}', s, re.S)
b=m.group(1) if m else ''
r=b.find('store_result('); w=b.find('wd_forensics_record_planned('); e=b.find('esp_restart(')
sys.exit(0 if 0<=r<w<e else 1)\""
check "强制关窗：muse_pair 的 on_force_close 调 pair_ble_mark_dead；BT 死了 shutdown 不调 ble_gap_*、但照样 link_pairing_reset" "$G -q '\\.on_force_close\\s*=\\s*on_window_force_close' muse_pair.c && python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('muse_pair.c').read())
m=re.search(r'static\s+void\s+on_window_force_close\s*\(void\)\s*\{(.*?)\n\}', s, re.S)
p=strip_comments(open('pair_ble.c').read())
n=re.search(r'void\s+pair_ble_shutdown\s*\(void\)\s*\{(.*?)\n\}', p, re.S)
b=n.group(1) if n else ''
g=re.search(r'if\s*\(\s*!s_bt_dead\s*\)\s*\{[^}]*ble_gap_adv_stop', b, re.S)
i=b.rfind('link_pairing_reset()')
stmt=b[max(b.rfind(';',0,i), b.rfind('}',0,i), b.rfind('{',0,i))+1:i]
sys.exit(0 if m and 'pair_ble_mark_dead()' in m.group(1) and g and i>b.find('s_bt_dead') and not re.search(r'\\bif\\b', stmt) else 1)\""
check "窗口没开过就不碰 NimBLE / link_pairing（没 token / 没网 / 预检失败时它们还没初始化）" "$G -q 'if\\s*\\(\\s*s_window_open_us\\s*\\)' muse_pair.c"
check "SDK token 只放内存，结束前清零" "$G -q 'mbedtls_platform_zeroize\\s*\\(\\s*s_sdk_token' muse_pair.c"
check "身份用 esp_read_mac(WIFI_STA)，不用 esp_efuse_mac_get_default（C5 上越界）" "$G -q 'esp_read_mac\\s*\\([^)]*ESP_MAC_WIFI_STA' muse_pair.c && $G --absent -q 'esp_efuse_mac_get_default' muse_pair.c pair_ble.c"
check "按键确认：注册与注销成对" "$G -q 'esp_event_handler_register\\s*\\(\\s*BUTTON_EVENT' muse_pair.c && $G -q 'esp_event_handler_unregister\\s*\\(\\s*BUTTON_EVENT' muse_pair.c"
# 日志的格式参数里不许出现令牌 / SDK token 本身（设计 14.2）。按"一条 ESP_LOG 调用"整体匹配（可跨行），不靠 ! grep（缺文件时恒绿）
check "日志里不出现令牌 / SDK token / 鉴权头的值（S1，设计 14.2）" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
bad=[]
for f in ('pair_ble.c','muse_pair.c','pair_core.c','../muse_link/muse_cfg.c','../muse_link/muse_cfg_core.c','../webui/muse_api.c'):
    s=strip_comments(open(f).read())
    for m in re.finditer(r'ESP_LOG[A-Z]\s*\((.*?)\);', s, re.S):
        args=m.group(1).split(',',2)[2:] if m.group(1).count(',')>=2 else []
        if any(re.search(r'(?i)access_token|refresh_token|sdk_tok|acc_tok|ref_tok|authorization|auth_header|\\btok(en)?\\b', a) for a in args):
            bad.append(f)
sys.exit(1 if bad else 0)\""
check "等按键 60 s、确认后等凭据 120 s（照上游 app.c）" "$G -q 'CONFIRM_WAIT_MS\\s+\\(60 \\* 1000\\)' muse_pair.c && $G -q 'CONFIRMED_IDLE_MS\\s+\\(120 \\* 1000\\)' muse_pair.c"
check "开窗前预检 api.muse.ai（M2：会话任务还没有，预检放配对剖面里），连不上记 api_unreachable" "$G -q 'preflight_api_reachable\\s*\\(\\s*\\)' muse_pair.c && $G -q 'PAIR_RES_API_UNREACHABLE' muse_pair.c"
check "主机测试编产品源文件 pair_core.c" "grep -qF 'pair_core_test.c\" \"\$COMP/pair_core.c' test_host/run.sh"

echo ""; echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
