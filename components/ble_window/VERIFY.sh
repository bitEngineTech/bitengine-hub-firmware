#!/bin/bash
# components/ble_window/VERIFY.sh — BLE 的唯一入口（docs/15_muse_direct_design_v1.md 第三节、2.6、2.1）
# ⚠️ 必须 cd 进本目录跑：bash VERIFY.sh
#
# 这里查代码形状；"什么时候进配对剖面"的判定（魔数 + 复位原因 + 安全模式）与窗口请求校验的真值表在
# test_host/run.sh（blew_core.c 不依赖 IDF，原样编进去测）。上板：TC-MU01–05、TC-BW04/05。
# 眼下只有策略 C（隔离重启）；A / B 等 G0-4（Kconfig 的 help 写着理由）。

PASS=0; FAIL=0
check() {
    if eval "$2" > /dev/null 2>&1; then echo "✅ $1"; PASS=$((PASS+1))
    else echo "❌ $1"; FAIL=$((FAIL+1)); fi
}
G="python3 ../../tools/grep_code.py"

# ---- 只属于直连型 + Muse 开关（架构 5.8 R-3） ----
check "CMake：源文件只在 BITENGINE_ENABLE_MUSE_LINK 下给" "grep -Pzo 'set\\(_blew_srcs \"\"\\)\\s*\\nif\\(CONFIG_BITENGINE_ENABLE_MUSE_LINK\\)' CMakeLists.txt"
check "CMake：依赖不放进 if（预展开读不到 CONFIG_*）" "python3 -c \"import re,sys; s=open('CMakeLists.txt').read(); i=s.find('if(CONFIG_BITENGINE_ENABLE_MUSE_LINK)'); e=s.find('endif()', i); sys.exit(1 if 'REQUIRES' in s[i:e] else 0)\""
check "CMake：REQUIRES / PRIV_REQUIRES 的参数里没有变量" "python3 -c \"import re,sys; sys.exit(1 if re.search(r'REQUIRES[^)]*[$][{]', open('CMakeLists.txt').read()) else 0)\""
check "Kconfig：策略 choice 依赖 MUSE_LINK，默认 C，眼下只有 C" "grep -Pzo 'choice BITENGINE_BLE_WINDOW_POLICY\\n[^\\n]*\\n\\s+depends on BITENGINE_ENABLE_MUSE_LINK\\n\\s+default BITENGINE_BLE_WINDOW_POLICY_ISOLATED' ../../main/Kconfig.projbuild && [ \$(grep -c '^        config BITENGINE_BLE_WINDOW_POLICY_' ../../main/Kconfig.projbuild) -eq 1 ]"
check "Kconfig：直连型里 ble_proxy 不可选（两者都会初始化 NimBLE）" "grep -Pzo 'config BITENGINE_ENABLE_BLE_PROXY\\n[^\\n]*\\n\\s+depends on !BITENGINE_PROFILE_DIRECT' ../../main/Kconfig.projbuild"
check "直连型 sdkconfig 里 ble_proxy 关着、策略是 C" "! grep -qx 'CONFIG_BITENGINE_ENABLE_BLE_PROXY=y' ../../sdkconfig.direct && grep -qx 'CONFIG_BITENGINE_BLE_WINDOW_POLICY_ISOLATED=y' ../../sdkconfig.direct"

# ---- 文件与 API（3.1） ----
for f in include/ble_window.h ble_window.c blew_boot.c blew_core.c blew_core.h blew_selfcheck.c test_host/run.sh test_host/blew_core_test.c; do
    check "$f 存在" "[ -f $f ]"
done
for fn in ble_window_open ble_window_close ble_window_scan_begin ble_window_scan_end ble_window_resume_thread_early \
          ble_window_effective_policy ble_window_request_isolated ble_window_take_pair_boot ble_window_pair_boot_active \
          ble_window_get_status; do
    check "API $fn 有声明有定义" "$G -q '\\b$fn\\s*\\(' include/ble_window.h && $G -q '^[a-z_ ]*\\b$fn\\s*\\([^;]*\$' ble_window.c blew_boot.c"
done
for v in BLEW_OWNER_MUSE_PAIR BLEW_OWNER_MATTER BLEW_POLICY_CONCURRENT BLEW_POLICY_PAUSE_THREAD BLEW_POLICY_ISOLATED \
         BLEW_ROLE_PERIPHERAL BLEW_ROLE_CENTRAL; do
    check "枚举 $v" "$G -q '\\b$v\\b' include/ble_window.h"
done
check "blew_core.c 不依赖 ESP-IDF（test_host 原样编它）" "! grep -qE '#include \"(esp_|freertos|nvs|host/|nimble)' blew_core.c blew_core.h"
check "主机测试编的是产品源文件" "grep -q '../blew_core.c' test_host/run.sh"

# ---- 策略 C（3.2）：RTC 魔数一次性、只认软件复位、在补重启分支之前取走 ----
check "魔数 0x4D555345 和它的取反" "$G -q 'BLEW_PAIR_MAGIC\\s+0x4D555345u' blew_core.h && $G -q '~\\s*BLEW_PAIR_MAGIC' blew_core.c"
check "魔数放 RTC_NOINIT，是自己的变量（不和 wd_forensics 的 RTC 区共用）" "$G -q 'RTC_NOINIT_ATTR\\s+uint32_t\\s+s_pair_magic' blew_boot.c && $G --absent -q 's_rtc\\b' blew_boot.c"
check "取魔数时不管判定结果一律清掉（一次性）" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('blew_boot.c').read())
m=re.search(r'bool\s+ble_window_take_pair_boot\s*\([^)]*\)\s*\{(.*?)\n\}', s, re.S)
b=m.group(1) if m else ''
d=b.find('blew_boot_decide'); c=b.find('s_pair_magic[0] = 0')
pre=b[:c]
# 清零前不许 return，也不许落在任何 if / 块里（花括号深度回到 0）—— 挪进 if (armed) 是审计实测逃逸过的变异
sys.exit(0 if m and d>=0 and c>d and 'return' not in pre and pre.count('{')==pre.count('}') and not re.search(r'\bif\s*\([^;{]*\)\s*$', pre.rstrip()) else 1)\""
check "判定只认软件复位（ESP_RST_SW），而且用的是原始的 esp_reset_reason()" "$G -q 'ESP_RST_SW' blew_boot.c && $G --absent -q 'wd_boot_reason' blew_boot.c"
check "main.c：取魔数在\"非干净复位补重启\"分支之前（否则 panic 后补重启会误进配对剖面）" "python3 -c \"
import sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('../../main/main.c').read())
a=s.find('ble_window_take_pair_boot('); b=s.find('rst != ESP_RST_POWERON && rst != ESP_RST_SW')
sys.exit(0 if 0<=a<b else 1)\""
check "main.c：安全模式优先 —— 配对剖面要求 !wd_safe_mode()" "$G -q 'pair_boot\\s*&&\\s*!\\s*wd_safe_mode\\s*\\(\\s*\\)' ../../main/main.c"
check "main.c：配对剖面在 thread_br / spectrum / mqtt / webhook / scheduler 之前 return" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('../../main/main.c').read())
i=s.find('muse_pair_boot_run(')
r=s.find('return;', i); e=s.find('#endif', i)
later=[s.find(x) for x in ('spectrum_init();','thread_br_init();','mqtt_client_init();','webhook_init();','scheduler_init();')]
sys.exit(0 if i>=0 and i<r<e and all(x>e for x in later) else 1)\""
check "请求隔离重启：只接受 MUSE_PAIR，先记计划内重启的取证再 esp_restart" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('blew_boot.c').read())
m=re.search(r'esp_err_t\s+ble_window_request_isolated\s*\([^)]*\)\s*\{(.*?)\n\}', s, re.S)
b=m.group(1) if m else ''
o=b.find('BLEW_OWNER_MUSE_PAIR'); f=b.find('wd_forensics_record_planned('); r=b.find('esp_restart(')
sys.exit(0 if m and 0<=o<f<r else 1)\""
check "策略 C 的正常开机释放 BT 控制器内存（窗口外不占内部内存，设计 2.6）" "$G -q 'esp_bt_mem_release\\s*\\(\\s*ESP_BT_MODE_BLE\\s*\\)' blew_boot.c && $G -q 'ble_window_release_bt_mem\\s*\\(' ../../main/main.c"
check "不用任何芯片级复位手段（IO28：芯片级复位会锁进下载模式）" "$G --absent -q '\\b(esp_restart_noos|esp_system_abort|rtc_wdt|esp_rom_software_reset_system|gpio_set_level)\\b' blew_boot.c ble_window.c"

# ---- BT 生命周期（3.4；H1 已查：IDF v6.0.2 的 nimble_port_init/deinit 自己管控制器） ----
# 全仓库的 C 源文件（不含主机测试、不含 HA 型专属的 ble_proxy —— Kconfig 让它在直连型里不可选）
ALL_C="$(find ../ ../../main -name '*.c' -not -path '*/test_host/*' -not -path '../ble_proxy/*' | sort | tr '\n' ' ')"
OTHERS_C="$(echo $ALL_C | tr ' ' '\n' | grep -v '^\.\./ble_window/' | tr '\n' ' ')"
check "nimble_port_init 全仓库只在 ble_window（ble_proxy 是 HA 型专属，Kconfig 挡着）" "$G --absent -q 'nimble_port_init\\s*\\(' $OTHERS_C && $G -q 'nimble_port_init\\s*\\(' ble_window.c"
check "esp_bt_controller_* 全仓库没人手调（nimble_port_init/deinit 自己管控制器）" "$G --absent -q 'esp_bt_controller_(init|enable|disable|deinit)\\s*\\(' $ALL_C"
check "全仓库没有 BLE 扫描（ble_gap_disc）：M2 没有中心角色" "$G --absent -q '\\bble_gap_disc\\s*\\(' $ALL_C"
check "nimble_port_init 的返回值有检查（上游没查）" "$G -q 'nimble_port_init\\s*\\(\\s*\\)\\s*;' ble_window.c && ! $G -q '^\\s*nimble_port_init\\s*\\(\\s*\\)\\s*;' ble_window.c"
check "反初始化走 nimble_port_stop → nimble_port_deinit" "$G -q 'nimble_port_stop\\s*\\(' ble_window.c && $G -q 'nimble_port_deinit\\s*\\(' ble_window.c"
check "内存闸门：内部空闲 + DMA 空闲在 BT 初始化之前查" "python3 -c \"
import sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('ble_window.c').read())
import re
m=re.search(r'static\s+esp_err_t\s+blew_do_open\s*\(void\)\s*\{(.*?)\n\}', s, re.S)
b=m.group(1) if m else ''
g=b.find('blew_mem_gate_ok('); n=b.find('nimble_port_init(')
sys.exit(0 if m and 0<=g<n and 'MALLOC_CAP_INTERNAL' in s and 'MALLOC_CAP_DMA' in s else 1)\""

# ---- 任务与定时器（3.1；S2；红线 10） ----
check "窗口在自己的任务里串行执行：内部栈 xTaskCreate、4 KB、优先级 5" "$G --strings -q 'xTaskCreate\\s*\\(\\s*blew_task\\s*,\\s*\"ble_window\"\\s*,\\s*BLEW_TASK_STACK' ble_window.c && $G -q 'BLEW_TASK_STACK\\s+4096' ble_window.c && $G -q 'BLEW_TASK_PRIO\\s+5' ble_window.c"
check "esp_timer 回调只投递（0 等待的 xQueueSend），不拿锁、不碰 NimBLE / OT" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('ble_window.c').read())
cbs=re.findall(r'\.callback\s*=\s*(\w+)', s)
ok=bool(cbs)
for cb in cbs:
    m=re.search(r'static\s+void\s+'+cb+r'\s*\([^)]*\)\s*\{(.*?)\n\}', s, re.S)
    b=m.group(1) if m else 'MISSING'
    if b=='MISSING' or re.search(r'xSemaphoreTake|nimble_|ble_gap|ble_gatts|thread_br|esp_openthread|vTaskDelay|portMAX_DELAY', b): ok=False
    if not re.search(r'xQueueSend\w*\s*\([^;]*,\s*0\s*\)', b): ok=False
sys.exit(0 if ok else 1)\""
check "看门狗：任务登记心跳，窗口外 wd_slot_offline（不然空闲时被判停转）" "$G -q 'wd_register\\s*\\(' ble_window.c && $G -q 'wd_kick\\s*\\(' ble_window.c && $G -q 'wd_slot_offline\\s*\\(' ble_window.c"
check "所有等待有上限：没有 portMAX_DELAY" "$G --absent -q 'portMAX_DELAY' ble_window.c blew_boot.c"
check "同时只允许一个窗口：忙时回 ESP_ERR_INVALID_STATE" "$G -q 'ESP_ERR_INVALID_STATE' ble_window.c && $G -q 'blew_req_check\\s*\\(' ble_window.c"
check "A / B / 中心角色还没有：三个 API 各自回 ESP_ERR_NOT_SUPPORTED（不是假装成功）" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('ble_window.c').read())
ok=all(re.search(r'esp_err_t\s+'+f+r'\s*\(void\)\s*\{\s*return\s+ESP_ERR_NOT_SUPPORTED;\s*\}', s) for f in
       ('ble_window_scan_begin','ble_window_scan_end','ble_window_resume_thread_early'))
sys.exit(0 if ok else 1)\""

# ---- 关窗协议：到时先通知 owner、不拆 BT；宽限期到了强制拆之前先让 owner 断掉 NimBLE 调用（审计 + 复核 2026-10-22） ----
check "到时 / BT 出错：只通知（on_expire）+ 起宽限定时器，不在这条路径上拆 BT" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('ble_window.c').read())
m=re.search(r'case MSG_HOST_RESET:(.*?)case MSG_GRACE:', s, re.S)
b=m.group(1) if m else ''
sys.exit(0 if m and 'on_expire' in b and 's_grace_timer' in b and 'blew_do_close' not in b else 1)\""
check "强制关：先同步调 owner 的 on_force_close，再 blew_do_close" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('ble_window.c').read())
m=re.search(r'case MSG_GRACE:(.*?)break;\s*\}', s, re.S)
b=m.group(1) if m else ''
sys.exit(0 if 0<=b.find('on_force_close')<b.find('blew_do_close(') else 1)\""
check "宽限期用独立定时器（旧的窗口超时消息不会被当成宽限到点）" "$G -q 'esp_timer_create\\s*\\(\\s*&gargs\\s*,\\s*&s_grace_timer' ble_window.c && $G -q 'esp_timer_stop\\s*\\(\\s*s_grace_timer' ble_window.c"

# ---- BLE 窗口之后的 Thread 自检（设计 3.3，TC-MU05）：判定真值表在 test_host（blew_sc_verdict / blew_res_*） ----
check "blew_selfcheck.c 存在且进了 CMake" "[ -f blew_selfcheck.c ] && grep -q 'blew_selfcheck.c' CMakeLists.txt"
check "基线放自己的 RTC_NOINIT 变量；每条路径上 magic 都最后写（其余字段先写完）" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('blew_selfcheck.c').read())
m=re.search(r'void\s+blew_selfcheck_capture_baseline\s*\(void\)\s*\{(.*?)\n\}', s, re.S)
b=m.group(1) if m else ''
ok='RTC_NOINIT_ATTR sc_rtc_t s_sc_rtc' in s and m and 's_sc_rtc.magic = SC_MAGIC' in b
for seg in re.split(r'\breturn\b', b):
    mg=seg.rfind('s_sc_rtc.magic = SC_MAGIC')
    if mg<0: continue
    others=[x.start() for x in re.finditer(r's_sc_rtc(\.(?!magic)\w+)?\s*=[^=]', seg)]
    if others and max(others)>mg: ok=False
sys.exit(0 if ok else 1)\""
check "请求进配对剖面时、重启之前记基线" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('blew_boot.c').read())
m=re.search(r'esp_err_t\s+ble_window_request_isolated\s*\([^)]*\)\s*\{(.*?)\n\}', s, re.S)
b=m.group(1) if m else ''
sys.exit(0 if 0<=b.find('blew_selfcheck_capture_baseline(')<b.find('esp_restart(') else 1)\""
check "自检基线一次性：取走时先清 magic，再起任务；配对剖面里不碰" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('blew_selfcheck.c').read())
m=re.search(r'esp_err_t\s+ble_window_selfcheck_start\s*\(void\)\s*\{(.*?)\n\}', s, re.S)
b=m.group(1) if m else ''
p=b.find('ble_window_pair_boot_active()'); c=b.find('s_sc_rtc.magic = 0'); t=b.find('xTaskCreate(')
sys.exit(0 if 0<=p<c<t else 1)\""
check "main.c：hub_store 起来之后才启动自检（结果要落 hubdb）" "python3 -c \"
import sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('../../main/main.c').read())
h=s.rfind('hub_store_start()'); c=s.find('ble_window_selfcheck_start(')
sys.exit(0 if 0<=h<c else 1)\""
check "策略 C 下子节点只记录不判（配对期间 Thread 整段不在，子节点改挂是 C 自己的停机）" "$G -q '\\.judge_children\\s*=\\s*false' blew_selfcheck.c && $G -q 'judge_children\\s*&&' blew_core.c"
check "配对剖面里窗口真开起来才记 bt_used；没开过不做自检" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
w=strip_comments(open('ble_window.c').read())
m=re.search(r'static\s+esp_err_t\s+blew_do_open\s*\(void\)\s*\{(.*?)\n\}', w, re.S)
b=m.group(1) if m else ''
s=strip_comments(open('blew_selfcheck.c').read())
n=re.search(r'esp_err_t\s+ble_window_selfcheck_start\s*\(void\)\s*\{(.*?)\n\}', s, re.S)
c=n.group(1) if n else ''
sys.exit(0 if 0<=b.find('s_state = ST_OPEN')<b.find('blew_selfcheck_note_bt_used()') and 0<=c.find('bt_used')<c.find('xTaskCreate(') else 1)\""
check "自检进行中又请求配对：沿用进行中那一轮的基线并记 interrupted（不拿刚开机的瞬态当基线）" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('blew_selfcheck.c').read())
m=re.search(r'void\s+blew_selfcheck_capture_baseline\s*\(void\)\s*\{(.*?)\n\}', s, re.S)
b=m.group(1) if m else ''
p=b.find('if (s_sc_pending)'); t=b.find('s_sc_rtc = s_task_base'); i=b.find('BLEW_SC_INTERRUPTED'); w=b.find('thread_br_get_ack_window(')
sys.exit(0 if 0<=p<t<i<w else 1)\""
check "策略 C 下自检退化不自动重启（再复位一次救不回来，只会循环）" "$G --absent -q 'esp_restart\\s*\\(' blew_selfcheck.c"
check "观察窗照设计 3.3：第 60–120 s 看 ACK（总上限 240 s）" "$G -q 'SC_OBS_START_S\\s+60' blew_selfcheck.c && $G -q 'SC_OBS_END_S\\s+120' blew_selfcheck.c && $G -q 'SC_CHILD_LIMIT_S\\s+240' blew_selfcheck.c"
check "thread_br 的 ACK 接口只在直连型 + Muse，读 2 s 缓存、不拿 OT 锁" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('../thread_br/thread_br.c').read())
bodies=[re.search(r'bool\s+'+f+r'\s*\([^)]*\)\s*\{(.*?)\n\}', s, re.S) for f in ('thread_br_get_ack_now','thread_br_get_ack_window')]
ok=all(b and 'esp_openthread_lock_acquire' not in b.group(1) for b in bodies)
raw=open('../thread_br/thread_br.c').read()
for f in ('bool thread_br_get_ack_now','bool thread_br_get_ack_window'):
    for mm in re.finditer(re.escape(f), raw):
        g2=raw.rfind('#if CONFIG_BITENGINE_ENABLE_MUSE_LINK',0,mm.start()); e2=raw.rfind('#endif',0,mm.start())
        ok = ok and g2>e2
h=open('../thread_br/include/thread_br.h').read()
i=h.find('thread_br_get_ack_now'); g=h.rfind('#if defined(CONFIG_BITENGINE_ENABLE_MUSE_LINK)', 0, i); e=h.rfind('#endif', 0, i)
sys.exit(0 if ok and g>e else 1)\""

# ---- 取证：计划内重启（3.2，交叉清单 A-26） ----
check "watchdog 有直连型专属的计划内重启类，放在 CLASS_MAX 之后（不进恢复策略表：不算自愈、不进安全模式计数）" "python3 -c \"
import sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('../watchdog/include/watchdog.h').read())
p=s.find('WD_FAULT_PLANNED ='); m=s.find('WD_FAULT_CLASS_MAX,'); i=s.rfind('#if', 0, p)
sys.exit(0 if 0<=m<i<p and s[i:].startswith('#if CONFIG_BITENGINE_PROFILE_DIRECT') else 1)\""
check "计划内重启按 L3（进 NVS 历史）或 L2（只记 RTC）记，永远 < L4（不加自愈计数、不进安全模式）" "$G -q 'wd_forensics_record\\s*\\(\\s*WD_FAULT_PLANNED\\s*,\\s*to_history\\s*\\?\\s*WD_LEVEL_SUBSYS\\s*:\\s*WD_LEVEL_TARGET' ../watchdog/wd_forensics.c && $G --absent -q 'WD_FAULT_PLANNED\\s*,\\s*WD_LEVEL_REBOOT' ../watchdog/wd_forensics.c"
check "一次配对只进一条历史：进门 false、出门 true" "$G --strings -q 'wd_forensics_record_planned\\s*\\(\\s*\"muse_pair_enter\"\\s*,\\s*false' blew_boot.c && $G --strings -q 'wd_forensics_record_planned\\s*\\(\\s*\"muse_pair_exit\"\\s*,\\s*true' ../muse_pair/muse_pair.c"

echo ""; echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
