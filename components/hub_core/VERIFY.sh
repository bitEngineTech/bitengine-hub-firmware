#!/bin/bash
# components/hub_core/VERIFY.sh — hubdb / hub_store 与命令层的设计约束（docs/15_muse_direct_design_v1.md 2.5、2.6、第六节、14.2）
# ⚠️ 必须 cd 进本目录跑：bash VERIFY.sh
#
# 这里查代码形状；防抖、版本规则、重试的真值表在 test_host/run.sh（hub_store_core.c 不依赖 IDF，原样编进去测）；
# 上板行为（写进去、重启后读回）用 /api/v1/diag/hubdb（CONFIG_BITENGINE_DIAG_HUBDB，开发期）。

PASS=0; FAIL=0
check() {
    if eval "$2" > /dev/null 2>&1; then echo "✅ $1"; PASS=$((PASS+1))
    else echo "❌ $1"; FAIL=$((FAIL+1)); fi
}
G="python3 ../../tools/grep_code.py"

# ---- 只属于直连型（架构 5.8 R-3）：HA 型里这个组件是空的 ----
check "CMake：源文件只在直连型里给（HA 型不编任何代码）" "grep -Pzo 'set\\(_hub_core_srcs \"\"\\)\\s*\\nif\\(CONFIG_BITENGINE_PROFILE_DIRECT\\)' CMakeLists.txt"
check "CMake：依赖不放进 if（预展开读不到 CONFIG_*）" "python3 -c \"import re,sys; s=open('CMakeLists.txt').read(); i=s.find('if(CONFIG_BITENGINE_PROFILE_DIRECT)'); e=s.find('endif()', i); sys.exit(1 if 'REQUIRES' in s[i:e] else 0)\""

# ---- 文件齐全，纯逻辑不依赖 IDF ----
for f in include/hub_store.h hub_store_core.h hub_store_core.c hub_store.c test_host/run.sh test_host/hub_store_test.c; do
    check "$f 存在" "[ -f $f ]"
done
check "hub_store_core.c 不依赖 ESP-IDF（test_host 原样编它）" "! grep -qE '#include \"(esp_|freertos|nvs)' hub_store_core.c hub_store_core.h"
check "主机测试编的是产品源文件" "grep -q 'hub_store_test.c ../hub_store_core.c' test_host/run.sh"

# ---- 规则 1：只有 hub_store 写 hubdb；任务栈在内部内存（PSRAM 栈的任务不能碰 flash） ----
check "nvs_set_* / nvs_erase_* / nvs_commit 只在 hub_store.c" "[ -z \"\$($G '\\bnvs_(set_\\w+|erase_key|erase_all|commit)\\s*\\(' ../*/*.c ../../main/*.c 2>/dev/null | grep -i hubdb)\" ] && $G -q 'nvs_set_blob\\s*\\(' hub_store.c"
# 判"谁在用 hubdb 分区名"要连字符串一起看（grep_code 默认剥字符串）：用普通 grep 找带引号的 "hubdb"
check "全仓库只有 hub_store.c 写出 \"hubdb\" 这个分区名" "[ \"\$(grep -l '\"hubdb\"' ../*/*.c ../../main/*.c 2>/dev/null)\" = '../hub_core/hub_store.c' ]"
check "hub_store 任务用内部栈（xTaskCreate，不是 psram_task_create / WithCaps）" "$G -q '\\bxTaskCreate\\s*\\(\\s*hub_store_task' hub_store.c && $G --absent -q 'psram_task_create|xTaskCreateWithCaps' hub_store.c"
check "任务栈 3 KB、优先级 4（设计 8.2 的任务表）" "$G -q 'HUB_STORE_STACK\\s+3072' hub_store.c && $G -q 'HUB_STORE_PRIO\\s+4' hub_store.c"

# ---- 规则 2：初始化只擦 hubdb，永远不碰主 nvs ----
check "nvs_flash_init_partition(\"hubdb\")" "$G -q 'nvs_flash_init_partition\\s*\\(\\s*HUBDB_PART' hub_store.c && grep -qE '#define HUBDB_PART +\"hubdb\"' hub_store.c"
check "NO_FREE_PAGES / NEW_VERSION_FOUND 时只擦 hubdb" "$G -q 'ESP_ERR_NVS_NO_FREE_PAGES' hub_store.c && $G -q 'ESP_ERR_NVS_NEW_VERSION_FOUND' hub_store.c && $G -q 'nvs_flash_erase_partition\\s*\\(\\s*HUBDB_PART\\s*\\)' hub_store.c"
check "不碰主 nvs：不调 nvs_flash_init() / nvs_flash_erase() 这两个无分区参数的" "$G --absent -q '\\bnvs_flash_(init|erase)\\s*\\(\\s*\\)' hub_store.c"
check "打开命名空间用 nvs_open_from_partition（不会落进主 nvs）" "$G -q 'nvs_open_from_partition\\s*\\(\\s*HUBDB_PART' hub_store.c && $G --absent -q '\\bnvs_open\\s*\\(' hub_store.c"
check "擦过 hubdb 要在状态里置位" "$G -q 'reset_at_init\\s*=\\s*true' hub_store.c"

# ---- 规则 3：写失败重试 3 次，仍失败就 5 s 红闪（复用 CONFIG_EVENT_NVS_ERROR）并记日志 ----
check "共试 3 次（HSC_MAX_TRIES，设计 2.5 规则 3）" "$G -q 'HSC_MAX_TRIES\\s+3' hub_store_core.h"
check "规则 4：读到更新的版本、写被拒都打 W 级日志" "$G -q 'rc == HSC_ENEWER' hub_store.c && $G -q 'refused_newer != refused_seen' hub_store.c"
check "重启前落盘：登记 shutdown handler 并检查结果、在状态里报" "$G -q 'esp_register_shutdown_handler\\(hub_store_on_shutdown\\)' hub_store.c && $G -q 'flush_on_restart = \\(reg == ESP_OK\\)' hub_store.c"
check "同步读超时那一刻做完：等到任务 give 再释放（审计 ❌：零等待会对已释放内存 give）" "! $G -q 'xSemaphoreTake\\(m->done,\\s*0\\)' hub_store.c"
check "最终失败发 CONFIG_EVENT_NVS_ERROR（灯 5 s 红闪，手册第三节已有这个语义）" "$G -q 'esp_event_post\\s*\\(\\s*CONFIG_EVENT\\s*,\\s*CONFIG_EVENT_NVS_ERROR' hub_store.c"
check "2 s 防抖（关键设计决策 7）" "$G -q 'HSC_DEBOUNCE_MS\\s+2000' hub_store_core.h"

# ---- 规则 4：每个 blob 第一个字节是格式版本；更新的版本当作没有、不覆盖写 ----
check "读到更新的版本返回 HSC_ENEWER" "$G -q 'rc\\s*=\\s*HSC_ENEWER' hub_store_core.c"
check "写之前比对已存的版本，更新的不覆盖" "$G -q 'refused_newer\\+\\+' hub_store_core.c"

# ---- 线程与生命周期（S2 / S3） ----
check "跨任务的读请求带\"已放弃\"标志（调用方超时后由任务负责释放，不写死掉的栈）" "$G -q 'abandoned' hub_store.c"
check "看门狗登记 hub_store（OBSERVE 60 s，设计 8.2）" "$G -q 'wd_register' hub_store.c && $G -q 'WD_SEV_OBSERVE' hub_store.c && $G -q '60000' hub_store.c"
check "任务循环里 kick" "$G -q 'wd_kick\\s*\\(' hub_store.c"
check "启动幂等（重复调用不起第二个任务）" "$G -q 's_started' hub_store.c"
check "日志不打 blob 内容（凭据会经它落盘，设计 4.4）" "$G --absent -q 'ESP_LOG\\w*\\([^;]*\\b(data|buf|payload)\\b[^;]*\\)' hub_store.c"

# ---- 启动序列（设计 2.6）：安全模式出口之后、只在直连型里起 ----
check "main.c 只在直连型里调 hub_store_start()（每一处都在直连型专属的 #if 里：PROFILE_DIRECT 或依赖它的 ENABLE_MUSE_LINK）" "python3 - <<'PY'
import re,sys
src=open('../../main/main.c').read()
hits=[m.start() for m in re.finditer(r'hub_store_start\(', src)]
def guarded(i):
    j=max(src.rfind('#if CONFIG_BITENGINE_PROFILE_DIRECT',0,i), src.rfind('#if CONFIG_BITENGINE_ENABLE_MUSE_LINK',0,i))
    return j>=0 and j>src.rfind('#endif',0,i) and j>src.rfind('#else',0,i)
sys.exit(0 if hits and all(guarded(i) for i in hits) else 1)
PY"

# ---- 恢复出厂（设计第十二节）：两种都清 muse，全清再清 hub，zhub 永远保留；先 flush 再擦 ----
check "hub_store_factory_reset：先 flush，muse 无条件清、hub 只在 wipe_all 时清、从不碰 zhub" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('hub_store.c').read())
m=re.search(r'esp_err_t\s+hub_store_factory_reset\s*\(bool wipe_all\)\s*\{(.*?)\n\}', s, re.S)
b=m.group(1) if m else ''
f=b.find('hub_store_flush('); mu=b.find('hub_store_erase_ns(HUB_STORE_NS_MUSE')
hub=re.search(r'wipe_all\)\s*err\s*=\s*hub_store_erase_ns\(HUB_STORE_NS_HUB', b)
sys.exit(0 if m and 0<=f<mu and hub and 'ZHUB' not in b and 'hub_store_start(' in b else 1)\""
check "config_mgr 的恢复出厂在直连型里调 hubdb 钩子（强定义在 hub_store.c，弱定义在 config_mgr.c）" "grep -Pzo '#if CONFIG_BITENGINE_PROFILE_DIRECT\\n[^\\n]*\\n\\s+esp_err_t herr = config_mgr_hubdb_factory_reset\\(scope == FACTORY_RESET_WIPE_ALL\\)' ../config_mgr/config_mgr.c && $G -q '__attribute__\\(\\(weak\\)\\)\\s+esp_err_t\\s+config_mgr_hubdb_factory_reset' ../config_mgr/config_mgr.c && $G -q '^esp_err_t\\s+config_mgr_hubdb_factory_reset\\s*\\(bool' hub_store.c"
check "直连型 ELF 里钩子是强定义（T 不是 W：hub_store.c 那份覆盖了弱定义）" "riscv32-esp-elf-nm ../../build-direct/bitengine-direct.elf | grep -q ' T config_mgr_hubdb_factory_reset\$'"

# ---- 命令层（6.2–6.5、6.8、6.9；M3 第二步）：纯逻辑、主机测试编产品源文件 ----
CMD_SRCS="hub_json.c hub_sanitize.c hub_model.c hub_registry.c hub_policy.c"
for f in $CMD_SRCS include/hub_json.h include/hub_sanitize.h include/hub_model.h include/hub_registry.h include/hub_policy.h hub_utf8.h; do
    check "$f 存在" "[ -f $f ]"
done
# 反向检查一律 grep_code（14.2）；#include 的路径在字符串里，要 --strings
check "命令层不依赖 ESP-IDF（test_host 原样编它们）" "$G --strings --absent -q '#\\s*include\\s*[<\"](esp_|freertos|nvs|sdkconfig)' $CMD_SRCS hub_utf8.h include/hub_json.h include/hub_sanitize.h include/hub_model.h include/hub_registry.h include/hub_policy.h"
check "命令层只在直连型里编（在 CMake 的直连型 SRCS 里）" "python3 -c \"
import json,sys,os
fs=[os.path.basename(e['file']) for e in json.load(open('../../build-direct/compile_commands.json')) if '/components/hub_core/' in e['file']]
ha=[e['file'] for e in json.load(open('../../build/compile_commands.json')) if '/components/hub_core/' in e['file']]
sys.exit(0 if all(f in fs for f in '$CMD_SRCS'.split()) and not ha else 1)\""
for t in hub_json hub_sanitize hub_registry hub_policy; do
    check "主机测试 ${t}_test 编的是产品源文件" "grep -qE '${t}_test\.c ../${t}\.c' test_host/run.sh"
done
check "主机测试跑 commands_v2 与附录 A 的金标比对" "grep -q 'python3 check_commands_v2.py' test_host/run.sh && grep -q '附录 A：' test_host/check_commands_v2.py"
# 14.2 的 hub_core 不变量
check "每条命令都写了 perm（命令表 9 条、.perm 9 处）" "[ \$(grep -c '^        \.name = \"' hub_registry.c) -ge 9 ] && [ \$(grep -c '\.perm = HUB_PERM_' hub_registry.c) -eq \$(grep -c '^        \.name = \"' hub_registry.c) ]"
check "每条命令都写了限速类别 rl" "[ \$(grep -c '\.rl = HUB_RL_' hub_registry.c) -eq \$(grep -c '^        \.name = \"' hub_registry.c) ]"
check "SENSITIVE 出厂为 0（HUB_PERM_DEFAULT 只有 CONTROL | MANAGE）" "grep -qE '#define HUB_PERM_DEFAULT +\(HUB_PERMBIT_CONTROL \| HUB_PERMBIT_MANAGE\)' include/hub_registry.h"
# 说明字符串在字符串字面量里：带 --strings 找；注释里写 % 不算
check "命令与参数说明里没有 %（6.8：说明是编译期常量，不插运行时数据）" "$G --strings --absent -q '\"[^\"]*%[^\"]*\"' hub_registry.c"
check "命令层不调格式化输出（说明、名字都不拼接）" "$G --absent -q '\b(s?n?printf|strcat|sprintf)\s*\(' hub_registry.c hub_sanitize.c hub_policy.c hub_model.c"
check "永不注册的命令不在表里（device.ota / discover / unpair / *_vm / ble.*）" "$G --strings --absent -q '\\.name = \"(device\\.(ota|discover|unpair|list_vms|set_vm|reset_vm)|ble\\.)' hub_registry.c"

# ---- 事件管线（6.7；M3 第五步）----
check "hub_evlog 不依赖 ESP-IDF，主机测试编产品源文件并真的执行" "$G --strings --absent -q '#\s*include\s*[<\"](esp_|freertos|nvs|sdkconfig)' hub_evlog.c include/hub_evlog.h && grep -qE 'hub_evlog_test\.c ../hub_evlog\.c' test_host/run.sh && grep -qx '\"\$OUT/hub_evlog_test\"' test_host/run.sh"
check "事件环与推送队列在 PSRAM（内部常驻增量 ≤ 2 KB，4.12）" "$G -q 's_ring = heap_caps_calloc\(1, sizeof\(hev_ring_t\), MALLOC_CAP_SPIRAM' hub_events.c && $G -q 'xQueueCreateWithCaps\(PUSH_Q_LEN, sizeof\(hev_t\), MALLOC_CAP_SPIRAM' hub_events.c"
check "管线的锁都有时限、推送队列只投不等（S2：生产者在事件循环里）" "$G --absent -q 'portMAX_DELAY' hub_events.c && $G -q 'xQueueSend\(s_pushq, &ev, 0\)' hub_events.c && [ \$($G 'xSemaphoreTake\(s_lock, LOCK_TICKS\)' hub_events.c | wc -l) -ge 3 ]"
check "管线不用 esp_timer（红线 10：emit 要拿锁，不能在 esp_timer 回调里调）" "$G --absent -q 'esp_timer_create' hub_events.c"
check "kind 只许 [a-z0-9_]（它原样进 JSON 和去重键）、文字 / 字段要有 NUL 才进环" "$G --strings -q '\*p == +_' hub_evlog.c && $G -q '!terminated\(ev->text, sizeof\(ev->text\)\)' hub_evlog.c && $G -q '!terminated\(ev->kind, sizeof\(ev->kind\)\)' hub_evlog.c && $G -q '!terminated\(ev->dev_id, sizeof\(ev->dev_id\)\)' hub_evlog.c && $G -q '!kind_ok\(ev->kind\)' hub_evlog.c"
check "异常重启事件只记重启后补发的那份（wd_recovery 抢发的 reset_reason 0 不记）" "$G -q 'if \(ab->reset_reason == 0\) return;' hub_events.c"
check "Thread 掉网：起来过之后连续两次才报、用户关了 Thread 不算" "$G -q 'if \(!s_th_seen_up \|\| s_th_down_reported\) return;' hub_events.c && $G -q 'if \(\+\+s_th_bad >= 2\)' hub_events.c && $G -q 'if \(!tc \|\| !tc->enabled\)' hub_events.c"
check "hub.events.recent 有处理函数，参数按附录 A 夹边界（limit 1–50）" "$G --strings -q '\{ \"hub.events.recent\",  cmd_events_recent \}' hub_cmds.c && $G --strings -q 'param_uint\(call, \"limit\", 1, 50, 20, &limit\)' hub_cmds.c"
check "注册表里的每条读命令都有处理函数（M3 出厂的 5 条：注册了就不能永远回 unsupported）" "python3 -c \"
import re,sys
reg=open('hub_registry.c').read(); cmds=open('hub_cmds.c').read()
names=re.findall(r'\.name = \\\"([a-z.]+)\\\",[^}]*?\.needs = HUB_NEEDS_NONE', reg, re.S)
impl=re.findall(r'\{ \\\"([a-z.]+)\\\",\s+cmd_', cmds)
sys.exit(0 if names and set(names) <= set(impl) else 1)\""

check "看门狗事件回调只记原因、不在系统事件任务里生成事件（栈 2304 B，emit 约 0.6 KB；第五步审计）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('hub_events.c').read())
b=re.search(r'static void on_wd_event\([^)]*\)\n\{(.*?)\n\}', S, re.S).group(1)
p=re.search(r'void hub_events_poll\(void\)\n\{(.*?)\n\}', S, re.S).group(1)
sys.exit(0 if 'hub_event_emit' not in b and 'snprintf' not in b and 's_ab_pending = true;' in b and 'emit_pending_restart();' in p else 1)\""
check "推给用户的重启句子不带内部原因串（会被 Muse 原样转述）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('hub_events.c').read())
b=re.search(r'static void emit_pending_restart\(void\)\n\{(.*?)\n\}', S, re.S).group(1)
sys.exit(0 if 'restart_phrase(s_ab_cause)' in b and b.count('s_ab_cause') == 1 else 1)\""
check "网关不对推送做人为干预（2026-10-24 用户定）：没有"不去重"这种标志 —— 推送路上本来就不去重" "! grep -rq 'NO_DEDUP' --include=*.c --include=*.h . ../muse_link"
check "hub.events.recent 拿不到锁就回错误（不回空列表）" "$G -q 'bool hub_events_write_recent\(hj_t \*w, uint32_t limit, uint32_t since_s\)' hub_events.c && $G -q 'if \(!hub_events_write_recent\(call->out, limit, since\)\) \{' hub_cmds.c && $G --strings --absent -q '\"busy\"' hub_events.c"
check "Thread 监视：OT 锁忙那次清零连续计数（严格连续两次才报）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('hub_events.c').read())
sys.exit(0 if re.search(r'if \(!known\) \{\s*s_th_bad = 0;\s*return;', S) else 1)\""

# ---- Thread 网络层清单（第七节；M3 第六步）----
check "hub_ti_core 不依赖 ESP-IDF，主机测试编产品源文件并真的执行" "$G --strings --absent -q '#\s*include\s*[<\"](esp_|freertos|nvs|sdkconfig|openthread)' hub_ti_core.c include/hub_ti_core.h && grep -qE 'hub_ti_test\.c ../hub_ti_core\.c' test_host/run.sh && grep -qx '\"\$OUT/hub_ti_test\"' test_host/run.sh"
check "清单只在直连型里编（hub_core 的直连型 SRCS），没动 HA 型共用的 thread_br" "python3 -c \"
import json,sys,os
fs=[os.path.basename(e['file']) for e in json.load(open('../../build-direct/compile_commands.json')) if '/components/hub_core/' in e['file']]
ha=[e['file'] for e in json.load(open('../../build/compile_commands.json')) if 'thread_inventory' in e['file'] or 'hub_ti_core' in e['file']]
sys.exit(0 if 'thread_inventory.c' in fs and 'hub_ti_core.c' in fs and not ha else 1)\" && ! ls ../thread_br/thread_inventory.c"
check "快照在 PSRAM（约 161 KB，内部常驻增量 ≤ 2 KB）" "[ \$($G 'heap_caps_calloc\(1, sizeof\(ti_(raw|list|prev)_t\), MALLOC_CAP_SPIRAM' thread_inventory.c | wc -l) -eq 3 ]"
check "OT 调用都在有时限的 OT 锁里；清单锁有时限；不用 esp_timer（红线 10）" "$G -q 'esp_openthread_lock_acquire\(OT_LOCK_TICKS\)' thread_inventory.c && $G --absent -q 'portMAX_DELAY|esp_timer_create' thread_inventory.c && [ \$($G 'esp_openthread_lock_acquire\(' thread_inventory.c | wc -l) -eq 2 ] && [ \$($G 'esp_openthread_lock_acquire\((OT_LOCK_TICKS|pdMS_TO_TICKS\(50\))\)' thread_inventory.c | wc -l) -eq 2 ]"
check "关了 Thread 清空清单、基线重来；掉网期间不比较、回来重建基线（不把这段时间的进出报成事件）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('thread_inventory.c').read())
b=re.search(r'esp_err_t thread_inventory_refresh\(void\)\n\{(.*?)\n\}', S, re.S).group(1)
ok='if (!tc || !tc->enabled)' in b and 'memset(s_prev, 0, sizeof(*s_prev));' in b and 'case COPY_DOWN:' in b and 's_outage = true;' in b \
   and re.search(r'if \(s_outage\) \{\s*s_prev->have_baseline = false;', b) and b.find('have_baseline = false') < b.find('ti_diff(')
sys.exit(0 if ok else 1)\""
check "成员离开推送（AVAILABILITY），新来的只进环（LIFECYCLE、不推）" "$G --strings -q 'hub_event_emit\(HUB_EVC_LIFECYCLE, id, \"thread_member_joined\", \"[^\"]*\", 0\)' thread_inventory.c && $G --strings -q 'HUB_EVC_AVAILABILITY, id, \"thread_member_left\"' thread_inventory.c"
check "离开要连续 TI_LEFT_AFTER（2）轮、且距上次看到满 TI_LEFT_MIN_MS（110 s）；第一轮只建基线；同一主机名换 id 原地改名（不报假离开）" "grep -qE '#define TI_LEFT_AFTER +2' include/hub_ti_core.h && grep -qE '#define TI_LEFT_MIN_MS +110000u' include/hub_ti_core.h && $G -q 'if \(p->miss >= TI_LEFT_AFTER && now_ms >= p->last_seen_ms && now_ms - p->last_seen_ms >= TI_LEFT_MIN_MS\)' hub_ti_core.c && $G -q 'const bool report = prev->have_baseline;' hub_ti_core.c && $G -q 'strcmp\(q->host, hname\) == 0 && !in_list\(cur, q->id\)' hub_ti_core.c"
check "关联（第六步两轮审计 ❌）：FTD 子节点按 RLOC 找、全 0 扩展地址不当 id、③④ 把子节点 / 路由器并进对应的主机那条（不出重复 id、不丢 fabric）" "$G -q 'if \(\(raw->my_rloc16 & 0x01FFu\) == 0 && \(r & 0xFC00u\) == \(raw->my_rloc16 & 0xFC00u\)\)' hub_ti_core.c && $G -q 'static bool ext_known\(const uint8_t ext\[8\]\)' hub_ti_core.c && [ \$($G 'ext_known\(' hub_ti_core.c | wc -l) -ge 3 ] && [ \$($G 'host_entry_for\(out, raw, id\)' hub_ti_core.c | wc -l) -eq 2 ] && grep -q 'test_audit_regressions();' test_host/hub_ti_test.c && grep -q 'test_hex_host_merge();' test_host/hub_ti_test.c"
check "BR 重启后恢复中的子节点不算在网" "$G -q 'otThreadGetChildInfoByIndex\(inst, i, &ci\) != OT_ERROR_NONE \|\| ci\.mIsStateRestoring\) continue;' thread_inventory.c"
check "子节点上限有编译期断言（CONFIG_OPENTHREAD_MLE_MAX_CHILDREN ≤ TI_MAX_CHILDREN）" "$G -q '_Static_assert\(CONFIG_OPENTHREAD_MLE_MAX_CHILDREN <= TI_MAX_CHILDREN' thread_inventory.c"
check "不说名字（7.5）：name 恒为空串；可配网设备自报的 DN 放 advertised_name；主机名 / DN 输出前过 hub_sanitize（6.8）；不输出 VID / PID" "$G --strings -q 'hj_kstr\(w, \"name\", \"\"\);' hub_ti_core.c && $G -q 'hub_sanitize_device_str\(h->dn, strlen\(h->dn\), adv,' hub_ti_core.c && $G -q 'hub_sanitize_device_str\(h->name, strlen\(h->name\), host,' hub_ti_core.c && $G --absent -q 'hj_k?str\([^;]*h->(name|dn)[,)]' hub_ti_core.c && $G --strings --absent -q '\"(vendor_id|product_id)\"' hub_ti_core.c"
check "建基线后静默期里新出现的不报（TI_QUIET_MS 5 分钟）；SRP 服务没在跑时不比较" "grep -qE '#define TI_QUIET_MS +300000u' include/hub_ti_core.h && $G -q 'const bool report_join = report && now_ms >= prev->baseline_ms && now_ms - prev->baseline_ms >= TI_QUIET_MS;' hub_ti_core.c && $G -q 'if \(otSrpServerGetState\(inst\) == OT_SRP_SERVER_STATE_RUNNING\) copy_hosts\(inst, raw\);' thread_inventory.c && python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('thread_inventory.c').read())
b=re.search(r'case COPY_NO_SRP:(.*?)break;', S, re.S).group(1)
sys.exit(0 if 's_outage = true;' in b and 'ti_diff' not in b else 1)\""
check "主机上限跟着 CONFIG_MDNS_MAX_SERVICES（编译期断言）；清单统计进 /muse/status" "$G -q '_Static_assert\(TI_MAX_HOSTS >= CONFIG_MDNS_MAX_SERVICES - 4' thread_inventory.c && $G -q 'thread_inventory_get_stats\(&ti\);' ../webui/muse_api.c"
check "hub.devices.list / hub.device.get 的 Thread 成员来自清单（不再直接读子节点表）" "$G -q 'thread_inventory_with\(c\.want_thread \? TI_FRESH_MS : 0, list_cb, &c\)' hub_cmds.c && $G -q 'thread_inventory_with\(TI_FRESH_MS, get_cb, &c\)' hub_cmds.c && $G --absent -q 'otThreadGetChildInfoByIndex\(inst, i, &ci\) != OT_ERROR_NONE\) continue;' hub_cmds.c"

check "重建基线前就有、之后没再出现的不报离开（静默期后退役）；并成一条时旧条目退役；两次刷新之间轻量采样网络 / SRP（第六步核实审计）" "$G -q 'if \(p->last_seen_ms < prev->baseline_ms\) \{' hub_ti_core.c && $G -q 'if \(p && hname\[0\] && !p->host\[0\]\) \{' hub_ti_core.c && grep -q 'test_verify_regressions();' test_host/hub_ti_test.c && $G -q 'void thread_inventory_watch\(void\)' thread_inventory.c && $G -q 'thread_inventory_watch\(\);' hub_svc.c"
check "粘连 id 每个主机只算一次（不在 ③④ 里对每个子节点 × 主机现查）；主机名就是扩展地址优先于粘连" "[ \$($G '\bsticky_id\(' hub_ti_core.c | wc -l) -eq 2 ] && $G -q 'const bool hit = pass == 0 \? \(host_hex_id' hub_ti_core.c"

# ---- 审计、参数校验、推送类别（6.10、4.8、6.7；M3 第七步）----
check "hub_params / hub_audit_core 不依赖 ESP-IDF；审计主机测试编产品源文件并真的执行" "$G --strings --absent -q '#\s*include\s*[<\"](esp_|freertos|nvs|sdkconfig)' hub_params.c include/hub_params.h hub_audit_core.c include/hub_audit_core.h && grep -qE 'hub_audit_test\.c ../hub_audit_core\.c' test_host/run.sh && grep -qx '\"\$OUT/hub_audit_test\"' test_host/run.sh"
check "参数校验与形状预检只有 hub_core 一份（muse_proto 只是转调，MD-9 两个出口同一套规矩）" "$G -q 'return hub_params_check\(c, params, err, err_cap\);' ../muse_link/muse_proto.c && $G -q 'return hub_json_shape_ok\(json, len, max_depth, max_values\);' ../muse_link/muse_proto.c && $G --absent -q 'static bool type_ok' ../muse_link/muse_proto.c"
check "审计环在 PSRAM、锁有时限；只取 id / action 两个参数的原文（不记参数里的秘密）；hub.device.update 的动作列只写改了哪几项、不碰名字原文（C17 第二轮审计）" "$G -q 's_ring = heap_caps_calloc\(1, sizeof\(hau_ring_t\), MALLOC_CAP_SPIRAM' hub_audit.c && $G --absent -q 'portMAX_DELAY' hub_audit.c && [ \$($G --strings 'param_str\(params, \"' hub_audit.c | grep -o 'param_str(params, \"' | wc -l) -eq 2 ] && $G --strings -q 'record\(src, command, param_str\(params, \"id\"\), act,' hub_audit.c && python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('hub_audit.c').read())
u=re.search(r'\\nstatic const char \\*update_action\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S).group(1)
U=open('hub_audit.c').read(); U=U[U.index('static const char *update_action('):]; U=U[:U.index(chr(10)+'}')]
lits=[l for l in re.findall(r'\\\"([^\\\"]*)\\\"', U) if '%' not in l]
ok = 'valuestring' not in u and 'param_str' not in u and 'GetStringValue' not in u and all(re.fullmatch(r'[A-Za-z0-9_.-]*', l) for l in lits) and 'notify_on' in lits
sys.exit(0 if ok else 1)\""
check "log_store：权限被拒与限速被拒各自每分钟最多一行、带压下的次数（第七步两轮审计：别让刷屏冲掉 W 环）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('hub_audit.c').read())
f=re.search(r'static void record\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S).group(1)
g=re.search(r'static bool gate_pass\\([^)]*\\)\\n\\{(.*?)\\n\\}', S, re.S).group(1)
logs=[m.start() for m in re.finditer(r'ESP_LOGW\\(', f)]
ok=len(logs) == 2 and 'log_forbidden = gate_pass(&s_gate_forbidden, e.mono_ms, &before);' in f \\
   and 'log_rl = gate_pass(&s_gate_rl, e.mono_ms, &before);' in f \\
   and 0 <= f.rfind('if (log_forbidden)', 0, logs[0]) and f.rfind('if (log_rl)', 0, logs[1]) > logs[0] \\
   and 'now - g->logged_ms < LOG_GATE_MS' in g and 'g->suppressed++;' in g and 'return false;' in g
sys.exit(0 if ok else 1)\""
check "命令结果里没有出站清单'不会送'的那类字段（键名不含 ip / ssid / psk / key / password / secret / token；设计第十三节，7.6 规矩 3）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
keys=set()
for f in ('hub_cmds.c','hub_ti_core.c','hub_evlog.c','hub_audit_core.c'):
    keys|=set(re.findall(r'hj_k\\w*\\(\\s*\\w+,\\s*\\\"([^\\\"]*)\\\"', strip_comments(open(f).read(), keep_strings=True)))
bad=[k for k in keys if set(k.lower().split('_')) & {'ip','ipv4','ipv6','ssid','psk','pskc','key','password','passwd','secret','token'}]
print(len(keys), bad)
sys.exit(0 if len(keys) >= 40 and not bad else 1)\""
check "相同调用合并成一条（最近 8 条、≤ 60 s，repeat 计数、挪到最前），主机测试覆盖合并与不合并的各个维度" "$G -q 'for \(uint32_t n = 0; n < HAU_MERGE_SCAN; n\+\+\)' hub_audit_core.c && $G -q 'e.mono_ms - c->mono_ms > HAU_MERGE_MS' hub_audit_core.c && $G -q 'if \(m.repeat < UINT16_MAX\) m.repeat\+\+;' hub_audit_core.c && grep -q '第 9 条不在范围里' test_host/hub_audit_test.c && grep -q '时间倒退：不合并' test_host/hub_audit_test.c && grep -q '来源不同：不合并' test_host/hub_audit_test.c"
check "审计字段进环前净化（[A-Za-z0-9_.-] 以外换 ?）" "$G --strings -q \"c == '_' \|\| c == '\\\\.' \|\| c == '-'\" hub_audit_core.c || $G -q 'dst\[n\+\+\] = ok \? c : ' hub_audit_core.c"
check "推送类别在 emit 一处判（ALARM 恒推）；关掉的类别照样进环" "$G -q 'bool push = \(flags & HEV_F_PUSH\) && \(cls == HUB_EVC_ALARM \|\| \(s_notify & \(1u << cls\)\)\);' hub_events.c && python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('hub_events.c').read())
b=re.search(r'esp_err_t hub_event_emit\([^)]*\)\n\{(.*?)\n\}', S, re.S).group(1)
sys.exit(0 if b.find('hev_ring_add(') >= 0 and b.find('hev_ring_add(') < b.find('xQueueSend(s_pushq') else 1)\""

# ---- hub_svc：事件管线 / Thread 清单 / 审计环独立于 Muse 会话任务（设计第十七节"M4 之前"）----
check "hub_svc.c 在直连型的源文件里" "[ -f hub_svc.c ] && [ -f include/hub_svc.h ] && grep -q '\"hub_svc.c\"' CMakeLists.txt"
check "hub_svc_start 起事件管线、审计环、Thread 清单，任务栈在 PSRAM、看门狗只观察" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('hub_svc.c').read())
st=re.search(r'esp_err_t hub_svc_start\\(void\\)\\n\\{(.*?)\\n\\}', S, re.S).group(1)
ok=all(x in st for x in ('hub_events_init()','hub_audit_init()','thread_inventory_init()')) and 'xTaskCreateWithCaps(' in st and 'MALLOC_CAP_SPIRAM' in st \\
   and '.severity = WD_SEV_OBSERVE' in S
sys.exit(0 if ok else 1)\""
check "hub_svc 的循环：每轮 hub_events_poll，每 6 轮（60 s）刷新清单、其余轮 watch，喂狗，固定周期（xTaskDelayUntil）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('hub_svc.c').read())
t=re.search(r'static void svc_task\\(void \\*arg\\)\\n\\{(.*?)\\n\\}', S, re.S).group(1)
ok=all(x in t for x in ('hub_events_poll();','thread_inventory_refresh()','thread_inventory_watch();','wd_kick(','xTaskDelayUntil(')) and 'round % INVENTORY_ROUNDS == 0' in t \\
   and re.search(r'#define INVENTORY_ROUNDS \\(INVENTORY_MS / TICK_MS\\)', S)
sys.exit(0 if ok else 1)\""
check "Muse 会话任务不再起 / 不再轮询这三样（只留推送的唤醒回调与推送类别）" "$G --absent -q 'hub_events_init\\(|hub_audit_init\\(|thread_inventory_init\\(|hub_events_poll\\(|thread_inventory_refresh\\(|thread_inventory_watch\\(' ../muse_link/muse_link.c && $G -q 'hub_events_set_wake\\(push_wake\\)' ../muse_link/muse_link.c"
check "hub_events_poll 全仓库只有 hub_svc 一个调用者（hub_events.h：内部状态不加锁）" "[ \$($G 'hub_events_poll\\(\\);' \$(ls ../*/*.c ../*/*.cpp ../../main/*.c 2>/dev/null) | wc -l) -eq 1 ] && $G -q 'hub_events_poll\\(\\);' hub_svc.c"
check "main.c：hub_svc_start 在直连型块里、hub_store_start 之后、muse_link_init 之前，不依赖 Muse 开关" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
S=strip_comments(open('../../main/main.c').read())
i=S.find('hub_svc_start()'); h=S.rfind('hub_store_start()', 0, i); m=S.find('muse_link_init()', i)
pre=S[:i]
ok=i > 0 and h > 0 and m > i and pre.rfind('#if CONFIG_BITENGINE_PROFILE_DIRECT') > pre.rfind('#endif') or False
# 不在 MUSE_LINK 的 #if 里：从 hub_store_start 到 hub_svc_start 之间没有开 MUSE_LINK 的块
ok=ok and '#if CONFIG_BITENGINE_ENABLE_MUSE_LINK' not in S[h:i]
sys.exit(0 if ok else 1)\""

# ---- 控制路径（设计 8.11、6.2、6.5；M5 第七步）----
check "hub.device.control：处理函数在表里（Zigbee 或 Matter 开着就登记）；动作按 hub_action_lookup、值按 hub_val_parse、两个后端都过第 3 道检查 hub_policy_check_action（Muse 带权限位、敏感标记取自 hub_meta）；只开放开 / 关 / 切换 / 亮度；回 confirmed / delivered / failed 与当前读数" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
C=strip_comments(open('hub_cmds.c').read(), keep_strings=True)
ok = re.search(r'#if CONFIG_BITENGINE_ENABLE_ZHUB \|\| CONFIG_BITENGINE_ENABLE_MATTER_CTRL\n +\{ \\\"hub.device.control\\\", +cmd_device_control \}', C)
fn=lambda n: re.search(r'\nstatic int %s\([^)]*\)\n\{(.*?)\n\}' % n, C, re.S)
f, ca, zb, mt = fn('cmd_device_control'), fn('check_action'), fn('zb_control'), fn('mt_control')
ok = ok and f and ca and zb and mt and 'hub_action_lookup(' in f.group(1) and 'hub_val_parse(' in f.group(1) and re.search(r'hub_policy_check_action\([^;]*call->perm[^;]*call->src', ca.group(1))
ok = ok and re.search(r'hub_zigbee_ctl_info\([^;]*&sensitive\)', zb.group(1)) and re.search(r'hub_matter_ctl_info\([^;]*&sensitive\)', mt.group(1))
ok = ok and all(re.search(r'if \(check_action\(call, a, sensitive, caps\) != 0\) return -1;', g.group(1)) for g in (zb, mt))
Z=strip_comments(open('hub_zigbee.c').read())
ci=re.search(r'\nint hub_zigbee_ctl_info\([^)]*\)\n\{(.*?)\n\}', Z, re.S)
M=strip_comments(open('hub_matter.c').read())
mi=re.search(r'\nint hub_matter_ctl_info\([^)]*\)\n\{(.*?)\n\}', M, re.S)
ok = ok and ci and 'HUB_META_F_SENSITIVE' in ci.group(1) and mi and 'HUB_META_F_SENSITIVE' in mi.group(1)
ok = ok and all(all(k in g.group(1) for k in ('HUB_ACT_TURN_ON','HUB_ACT_TURN_OFF','HUB_ACT_TOGGLE','HUB_ACT_SET_BRIGHTNESS','confirmed','delivered','failed')) for g in (zb, mt))
ok = ok and re.search(r'uint8_t +perm;', open('include/hub_call.h').read())
sys.exit(0 if ok else 1)\""
# ---- 注册就绪位（CHECKLIST C17；M5 第三步）：update 只要有后端，control / permit_join 各要自己的就绪位 ----
check "注册就绪位拆开：hub.device.control 要 HUB_NEEDS_CONTROL、hub.zigbee.permit_join 要 HUB_NEEDS_ZB_JOIN、hub.device.update 只要 ANY_BACKEND；hub_cmd_available 判这两位" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
R=strip_comments(open('hub_registry.c').read(), keep_strings=True)
def needs(name):
    i=R.index('.name = \\\"%s\\\"' % name); j=R.index('.needs', i); return R[j:R.index('.rl', j)]
ok = 'HUB_NEEDS_CONTROL' in needs('hub.device.control') and 'HUB_NEEDS_ANY_BACKEND' in needs('hub.device.control')
ok = ok and 'HUB_NEEDS_ZB_JOIN' in needs('hub.zigbee.permit_join') and 'HUB_NEEDS_ZHUB' in needs('hub.zigbee.permit_join')
ok = ok and 'HUB_NEEDS_CONTROL' not in needs('hub.device.update') and 'HUB_NEEDS_ZB_JOIN' not in needs('hub.device.update')
a=re.search(r'\nbool hub_cmd_available\([^)]*\)\n\{(.*?)\n\}', R, re.S).group(1)
ok = ok and re.search(r'if \(\(c->needs & HUB_NEEDS_CONTROL\) && !\(backends & HUB_BACKEND_CONTROL\)\) return false;', a) \
     and re.search(r'if \(\(c->needs & HUB_NEEDS_ZB_JOIN\) && !\(backends & HUB_BACKEND_ZB_JOIN\)\) return false;', a)
sys.exit(0 if ok else 1)\""
# ---- 设备元数据（6.1 元数据持久化、附录 A hub.device.update；M5 第二步）：hub_meta_core 纯逻辑 + hub_meta 存取 ----
check "元数据纯逻辑 hub_meta_core 不依赖 ESP-IDF、主机测试编产品源文件" "[ -f hub_meta_core.c ] && [ -f include/hub_meta_core.h ] && $G --strings --absent -q '#\\s*include\\s*[<\\\"](esp_|freertos|sdkconfig|nvs)' hub_meta_core.c include/hub_meta_core.h && grep -q 'hub_meta_test.c ../hub_meta_core.c' test_host/run.sh"
check "元数据分页存（hub_store 单 blob ≤ HSC_BLOB_MAX 4000 B：设计 6.1 写的单个 12 KB 的 blob 放不下）：每页大小编译期钉住不超上限，键 meta0…" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
h=strip_comments(open('include/hub_meta_core.h').read())
ok = re.search(r'_Static_assert\(HUB_META_PAGE_BYTES <= 4000', h) and re.search(r'#define HUB_META_PAGES', h) and re.search(r'#define HUB_META_PER_PAGE', h)
M=open('hub_meta.c').read()
ok = ok and 'meta%u' in M and 'HUB_STORE_NS_HUB' in M
sys.exit(0 if ok else 1)\""
check "名字 / 房间按 6.8 用户名规则净化（hub_sanitize_user_name：拒换行、≤ 32 / 24 B），Muse 不能改敏感标记（hub.device.update 没有这个参数；按剥掉注释的代码查，7.4）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
M=strip_comments(open('hub_meta_core.c').read())
a=re.search(r'\nhub_meta_res_t hub_meta_apply\([^)]*\)\n\{(.*?)\n\}', M, re.S).group(1)
ok = re.search(r'hub_sanitize_user_name\(c->name,[^;]*HUB_NAME_MAX\)', a) and re.search(r'hub_sanitize_user_name\(c->room,[^;]*HUB_ROOM_MAX\)', a)
ok = ok and not re.search(r'flags\s*=[^;]*\?\s*HUB_META_F_NOTIFY\s*:', a)
sl=[l for l in a.split(chr(10)) if 'HUB_META_F_SENSITIVE' in l]
ok = ok and len(sl) == 2 and all(re.search(r'c->sensitive == HUB_META_SENS_(ON|OFF)\)', l) for l in sl)   # 敏感位只由 sensitive 操作码改（本地接口，2026-10-23）
C=strip_comments(open('hub_cmds.c').read(), keep_strings=True)
f=re.search(r'\nstatic int cmd_device_update\([^)]*\)\n\{(.*?)\n\}', C, re.S)
ok = ok and f and not re.search(r'\x22sensitive\x22|\.sensitive\b|HUB_META_SENS_', f.group(1))   # 不读 sensitive 参数、不写 .sensitive（提示文字里提到这个词不算，2026-10-23）
sys.exit(0 if ok else 1)\""
check "读不出来 ≠ 没有（muse_link 三次教训）：esp_err 逐个映射（其余一律 RD_ERR = 只读），判定在 core（主机真值表 M6），只读页记下、有只读页就不新建条目（两轮审计 ❌）" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
M=strip_comments(open('hub_meta.c').read())
r=re.search(r'\nstatic hub_meta_rd_t rd_of\(esp_err_t e\)\n\{(.*?)\n\}', M, re.S).group(1)
cases=re.findall(r'case (\w+):\s*return (\w+);', r)
ok = sorted(cases)==sorted([('ESP_OK','HUB_META_RD_OK'),('ESP_ERR_NOT_FOUND','HUB_META_RD_NONE'),('ESP_ERR_INVALID_STATE','HUB_META_RD_DOWN'),('ESP_ERR_TIMEOUT','HUB_META_RD_TIMEOUT')]) \
     and re.search(r'default:\s*return HUB_META_RD_ERR;', r)
ok = ok and re.search(r'case HUB_META_LD_READONLY:\s*s_page_bad\[p\] = true;', M) and re.search(r'hub_meta_apply\([^;]*page_ok_mask\(\)', M)
pm=re.search(r'\nstatic uint32_t page_ok_mask\(void\)\n\{(.*?)\n\}', M, re.S)
ok = ok and pm and re.search(r'if \(!s_page_bad\[p\]\) ok \|=', pm.group(1))
K=strip_comments(open('hub_meta_core.c').read())
lp=re.search(r'\nhub_meta_ld_t hub_meta_load_page\([^)]*\)\n\{(.*?)\n\}', K, re.S).group(1)
ok = ok and re.search(r'case HUB_META_RD_TIMEOUT:\s*case HUB_META_RD_ERR:\s*default:\s*return HUB_META_LD_READONLY;', lp)
a=re.search(r'\nhub_meta_res_t hub_meta_apply\([^)]*\)\n\{(.*?)\n\}', K, re.S).group(1)
ok = ok and re.search(r'if \(slot < 0\) \{\s*if \(\(page_ok & HUB_META_ALL_PAGES\) != HUB_META_ALL_PAGES\) return HUB_META_STORE;', a)
sys.exit(0 if ok else 1)\""
check "SECURITY 推不推 = 类别位 AND 设备 notify 位（6.7），两样都在 hub_event_emit 一处判（设计第十七节「M4 之前」）；Zigbee 生产者照常带推送标志、门磁不去重" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
E=strip_comments(open('hub_events.c').read())
em=re.search(r'\nesp_err_t hub_event_emit\([^)]*\)\n\{(.*?)\n\}', E, re.S).group(1)
dn=re.search(r'\nstatic bool dev_notify\(const char \*dev_id\)\n\{(.*?)\n\}', E, re.S)
ok = re.search(r'if \(push && cls == HUB_EVC_SECURITY\) push = dev_notify\(dev_id\);', em) and dn and 'HUB_META_F_NOTIFY' in dn.group(1) and 'hub_meta_lookup(' in dn.group(1)
Z=strip_comments(open('hub_zigbee.c').read(), keep_strings=True)
f=re.search(r'\nstatic void state_event\([^)]*\)\n\{(.*?)\n\}', Z, re.S).group(1)
ok = ok and re.search(r'const unsigned sec_flags = HEV_F_PUSH;', f) and re.search(r'emit_ev\(HUB_EVC_SECURITY, id, on \? \\\"contact_open\\\" : \\\"contact_closed\\\", t, sec_flags\)', f)
sys.exit(0 if ok else 1)\""
check "事件句子里的设备名走 hub_meta_event_name（6.7 模板，主机真值表 M8：引号 / 方括号替换、房间括号、最长不截断），状态事件与设备事件都带上元数据" "python3 -c \"
import re,sys; sys.path.insert(0,'../../tools'); from grep_code import strip_comments
Z=strip_comments(open('hub_zigbee.c').read())
n=re.search(r'\nstatic void name_of\([^)]*\)\n\{(.*?)\n\}', Z, re.S).group(1)
ok = re.search(r'hub_meta_event_name\(m, HUB_META_PROTO_ZIGBEE, kind,', n)
for fn in ('state_event','hk_event'):
    b=re.search(r'\nstatic void %s\([^)]*\)\n\{(.*?)\n\}' % fn, Z, re.S).group(1)
    ok = ok and re.search(r'meta_of\(v->rec\.ieee, &m, META_LOCK_ZHUB_MS\);', b) and re.search(r'name_of\(v, &m, name, sizeof\(name\)\);', b)
sys.exit(0 if ok else 1)\""

# ---- Matter 设备（mt-）进列表 / 详情（设计 7.3、9.6；M7 第四步）----
check "mt- 的写法只有一处：输出小写、解析不分大小写" "python3 check_hub_mt.py mt_ids"
check "Matter 事件里的称呼走 hub_meta_event_name；只有\"加好了\"推送" "python3 check_hub_mt.py ev_name"
check "列表顺序 Zigbee → Matter → Thread；本网关 fabric 上的 Thread 成员不再按 th- 列（7.3）" "python3 check_hub_mt.py list_order"
check "要 Matter 设备时不走只要 Zigbee 的捷径（Thread 信息要从清单里取）" "python3 check_hub_mt.py matter_needs_inventory"
check "device.get：mt- 走 hub_matter；本 fabric 上的 th- 指向 mt-" "python3 check_hub_mt.py get_paths"
check "hub.device.update 认 mt-" "python3 check_hub_mt.py update_mt"
check "列表项：只列配好了的、能力来自 hub_mcap、能开关 / 调亮度的才可控、Thread 信息找到才挂；缓冲够装节点表" "python3 check_hub_mt.py entry_shape"
check "跳过 th- 要节点号在节点表里配好了（只看 fabric 会让强制移除 / 表没读回的设备整台消失，审计 ❌）" "python3 check_hub_mt.py own_needs_table"
check "device.health 的设备总数与列表同口径（mt- 只数一次、不在 Thread 上的 Matter 设备也算）" "python3 check_hub_mt.py health_count"
check "本地敏感标记接口认 mt-（MC35）" "grep -A6 'only devices this hub manages\\|只认这个网关管着的设备' ../webui/hub_api.c | grep -q 'hub_matter_write_device(NULL, id, NULL, NULL)'"
check "Matter 的事件：五种都认；离线 / 回来、门磁、人体、漏水、烟雾、电量低的类别与推送标志同 Zigbee（6.7；M7 第五步）" "python3 check_hub_mt.py mt_events"
check "Matter 的读数键名同 Zigbee（门磁是字符串）；state + state_age_s；不知道在不在线就不写 online" "python3 check_hub_mt.py mt_state_keys"
check "配网当场拒不扣限速（MX21）：处理函数置 rl_refund、受理才清，两个出口都退还；退还不超过容量（主机测试 hub_policy）" "python3 check_hub_mt.py rl_refund"
check "重置 Matter 控制器只在 ident_lost、先清名字与数据最后删记号；测试设备开关落盘确认才生效；两个本地接口守卫顺序 / 密码 / 审计、Muse 碰不到" "python3 check_hub_mt.py reset_and_opts"
check "名字表按协议清空（重置用）：主机测试 M9" "grep -q 'M9 清掉两条 Matter' test_host/hub_meta_test.c"
check "hub_mcap.c 进构建；能力映射有主机测试（9.6 的表）" "grep -q '\"hub_mcap.c\"' CMakeLists.txt && grep -q 'hub_mcap_test' test_host/run.sh"

# ---- S1 自动检查（CLAUDE.md 编码后第 8 条）----
check "No strcmp on secrets" "! grep -rn 'strcmp.*password\|strcmp.*token' *.c"
check "malloc/free 大致平衡" "[ \$(grep -c 'malloc\\|hsc_alloc\\|s->alloc' hub_store.c hub_store_core.c | awk -F: '{s+=\$2} END {print s}') -le \$(grep -c 'free' hub_store.c hub_store_core.c | awk -F: '{s+=\$2} END {print s}') ]"

# ---- 敏感标记只在本地设（6.2；2026-10-23 加 /api/v1/hub/device/sensitive）----
check "Muse 改不了敏感标记：hub.device.update 按字段名初始化、不写 sensitive（= 不改）；HUB_META_SENS_ON / OFF 只在 webui/hub_api.c 的本地接口里用；全零 = 不改" "grep -q 'const hub_meta_change_t ch = { .name = name, .room = room, .notify = ' hub_cmds.c && ! grep -q 'HUB_META_SENS_ON\|HUB_META_SENS_OFF' hub_cmds.c ../muse_link/*.c && grep -q 'HUB_META_SENS_KEEP = 0' include/hub_meta_core.h && grep -q 'handle_device_sensitive' ../webui/hub_api.c && grep -Pzoq 'static esp_err_t handle_device_sensitive\(httpd_req_t \*req\)\n\{\n\s+CROSS_ORIGIN_GUARD\(req\);\n\s+AUTH_REQUIRED\(req\);\n\s+if \(!config_mgr_has_password\(\)\)' ../webui/hub_api.c"

echo ""; echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
