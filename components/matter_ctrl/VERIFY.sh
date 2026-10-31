#!/bin/bash
# components/matter_ctrl/VERIFY.sh — 直连型 Matter 控制器的设计约束（docs/15_muse_direct_design_v1.md 第九节、9.8a / 9.8b；架构 5.8）
# ⚠️ 必须 cd 进本目录跑：bash VERIFY.sh
#
# 查代码形状。M7 第一步：构建接入、起控制器、自己的 fabric、状态。配网 / 访谈 / 订阅 / 命令 / 移除的判据随后面几步加。

PASS=0; FAIL=0
check() {
    if eval "$2" > /dev/null 2>&1; then echo "✅ $1"; PASS=$((PASS+1))
    else echo "❌ $1"; FAIL=$((FAIL+1)); fi
}
G="python3 ../../tools/grep_code.py"
ROOT=../..

# ---- 只属于直连型（架构 5.8 R-3）：HA 型里是空组件，也不拉 esp_matter ----
check "CMake：源文件只在开了 MATTER_CTRL 时给" "grep -Pzo 'set\\(_mc_srcs \"\"\\)\\s*\\n(?:\\s*set\\([^\\n]*\\n)*\\s*if\\(CONFIG_BITENGINE_ENABLE_MATTER_CTRL\\)' CMakeLists.txt"
check "CMake：依赖不放进 if（预展开读不到 CONFIG_*，hub_core 2026-10-21 踩过）" "python3 -c \"import re,sys; s=open('CMakeLists.txt').read(); i=s.find('if(CONFIG_BITENGINE_ENABLE_MATTER_CTRL)'); e=s.find('endif()', i); sys.exit(1 if 'REQUIRES' in s[i:e] else 0)\""
check "CMake：不显式依赖 espressif__esp_matter（HA 型里没有它；直连型由 idf_component.yml 注入）" "$G --absent -q 'REQUIRES[^)]*espressif__esp_matter' CMakeLists.txt"
check "idf_component.yml：esp_matter 版本钉死 ==1.6.0~1" "grep -qE 'version: \"==1\\.6\\.0~1\"' idf_component.yml"
check "idf_component.yml：条件是直连型（HA 型里看得见的符号；MATTER_CTRL 在 HA 型不可见，组件管理器会报缺选项）" "grep -qF '\$CONFIG{BITENGINE_PROFILE_DIRECT} == True' idf_component.yml"
check "idf_component.yml：私有依赖" "grep -qE 'require: private' idf_component.yml"
check "顶层：直连型用自己的锁文件（HA 型的 dependencies.lock 不随直连型的解算改）" "grep -qE 'idf_build_set_property\\(DEPENDENCIES_LOCK \"\\\$\\{CMAKE_CURRENT_LIST_DIR\\}/dependencies\\.direct\\.lock\"\\)' $ROOT/CMakeLists.txt && [ -f $ROOT/dependencies.direct.lock ]"
check "直连型锁文件里有 esp_matter 1.6.0~1，HA 型的没有" "python3 -c \"
import re,sys
s=open('$ROOT/dependencies.direct.lock').read()
m=re.search(r'^  espressif/esp_matter:\\n(.*?)(?=^  \\S|^\\S)', s, re.S|re.M)
ok=m and re.search(r'^    version: 1\\.6\\.0~1\\s*$', m.group(1), re.M)
sys.exit(0 if ok and 'esp_matter' not in open('$ROOT/dependencies.lock').read() else 1)\""
check "顶层：esp_matter 补丁只在 MATTER_CTRL 下打，失败就停" "grep -Pzo 'if\\(CONFIG_BITENGINE_ENABLE_MATTER_CTRL\\)\\s*\\n\\s*execute_process\\(\\s*\\n\\s*COMMAND[^\\n]*\\n\\s*\\\$\\{CMAKE_CURRENT_LIST_DIR\\}/tools/patch_esp_matter\\.py' $ROOT/CMakeLists.txt && grep -q 'esp-matter 补丁失败' $ROOT/CMakeLists.txt"

# ---- 构建期补丁（9.8a 第 7 条、9.8b 第 11 条）：原文匹配、幂等、只查模式 ----
check "补丁：mType 存基础类型（9.8b 第 11 条）" "grep -q 'subtypeEnd ? subtypeEnd + 6 : type' $ROOT/tools/patch_esp_matter.py"
check "补丁：控制器不宣告自己（9.8a 第 7 条：宣告会改掉网关的 mDNS 主机名）" "grep -q 'm_operational_advertising = false' $ROOT/tools/patch_esp_matter.py"
check "补丁脚本有 --check 与 --self-test" "grep -q -- '--check' $ROOT/tools/patch_esp_matter.py && grep -q -- '--self-test' $ROOT/tools/patch_esp_matter.py"
check "补丁脚本自测通过（原文匹配不到 / 只打了一半都要报错）" "python3 $ROOT/tools/patch_esp_matter.py --self-test"

# ---- Kconfig 与配置（9.2 的表 + 9.8a 第 1–6、10 条）----
check "C5 的 GPIO_RANGE_MAX 在我们的 Kconfig 里补（9.8a 第 6 条），只在直连型可见" "grep -Pzo 'config GPIO_RANGE_MAX\\s*\\n\\s*int\\s*\\n\\s*depends on BITENGINE_PROFILE_DIRECT\\s*\\n\\s*default 28 if IDF_TARGET_ESP32C5' Kconfig"
D=$ROOT/sdkconfig.defaults.direct
for kv in CONFIG_BITENGINE_ENABLE_MATTER_CTRL=y CONFIG_ENABLE_CHIP_CONTROLLER_BUILD=y CONFIG_ESP_MATTER_CONTROLLER_ENABLE=y \
          CONFIG_ESP_MATTER_COMMISSIONER_ENABLE=y CONFIG_ESP_MATTER_ENABLE_MATTER_SERVER=n CONFIG_ENABLE_CHIP_SHELL=n \
          CONFIG_WIFI_NETWORK_COMMISSIONING_DRIVER=n CONFIG_ENABLE_ESP32_BLE_CONTROLLER=n CONFIG_ENABLE_CHIPOBLE=n \
          CONFIG_CHIP_MEM_ALLOC_MODE_EXTERNAL=y CONFIG_ESP_MATTER_MEM_ALLOC_MODE_EXTERNAL=y CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y \
          CONFIG_CHIP_TASK_STACK_SIZE=15360 CONFIG_ESP_MATTER_COMMISSIONER_SUPPORT_TEST_CD=n CONFIG_CHIP_LOG_DEFAULT_LEVEL_ERROR=y \
          CONFIG_ENABLE_MATTER_OVER_THREAD=n CONFIG_ENABLE_WIFI_STATION=n CONFIG_USE_TCPIP_CORE_LOCK_FOR_THREAD_SAFETY=n \
          CONFIG_ENABLE_ROUTE_HOOK=n CONFIG_USE_MINIMAL_MDNS=n CONFIG_ENABLE_ETHERNET_TELEMETRY=y CONFIG_ETHERNET_NETWORK_COMMISSIONING_DRIVER=n; do
    check "sdkconfig.defaults.direct：$kv" "grep -qxF '$kv' $D"
done
for p in FACTORY CONFIG COUNTERS KVS; do
    check "CHIP 的 $p 进 matter 分区（2.4）" "grep -qxF 'CONFIG_CHIP_${p}_NAMESPACE_PARTITION_LABEL=\"matter\"' $D"
done
check "CHIPProjectConfig.h 指向本组件" "grep -qxF 'CONFIG_CHIP_PROJECT_CONFIG=\"components/matter_ctrl/CHIPProjectConfig.h\"' $D"
check "限额（2026-10-24 Matter 32 → 128）：会话池 = 节点上限 + 16、CASE 客户端 8、同时配网 2；节点表结构容量 ≥ 节点上限" "$G -q 'CHIP_CONFIG_SECURE_SESSION_POOL_SIZE\\s+\\(CONFIG_BITENGINE_MATTER_MAX_NODES \\+ 16\\)' CHIPProjectConfig.h && $G -q 'CHIP_CONFIG_CONTROLLER_MAX_ACTIVE_CASE_CLIENTS\\s+8' CHIPProjectConfig.h && $G -q 'CHIP_CONFIG_CONTROLLER_MAX_ACTIVE_DEVICES\\s+2' CHIPProjectConfig.h && grep -q '#define MC_NODES_CAP       128' mc_nodes_core.h && grep -q 'range 1 128' ../../main/Kconfig.projbuild"
check "容量配置两份一致（defaults 改了不会覆盖 sdkconfig.direct 里已有的值）：节点上限 128、定时器 ≥ 2 × 节点上限、交换上下文 32" "python3 - <<'PY'
import re,sys
def vals(p):
    s=open(p).read(); return {k:int(v) for k,v in re.findall(r'^CONFIG_(BITENGINE_MATTER_MAX_NODES|NUM_TIMERS|MAX_EXCHANGE_CONTEXTS)=(\\d+)', s, re.M)}
a,b=vals('../../sdkconfig.defaults.direct'),vals('../../sdkconfig.direct')
ok = a==b and a.get('BITENGINE_MATTER_MAX_NODES')==128 and a.get('NUM_TIMERS',0) >= 2*a['BITENGINE_MATTER_MAX_NODES'] and a.get('MAX_EXCHANGE_CONTEXTS')==32
sys.exit(0 if ok else 1)
PY"
check "静态池进 PSRAM：本组件与 esp_matter 两个归档都映射（9.8a 第 8 条）" "grep -q 'archive: libmatter_ctrl.a' linker.lf && grep -q 'archive: libespressif__esp_matter.a' linker.lf && [ \$(grep -c '(extram_bss)' linker.lf) -ge 2 ]"
check "片内 EMAC 的配网驱动不编（9.8a 第 5 条）" "grep -Pzo 'NetworkCommissioningDriver_Ethernet\\.cpp\"\\s*\\n\\s*TARGET_DIRECTORY[^\\n]*\\n\\s*PROPERTIES HEADER_FILE_ONLY ON' CMakeLists.txt"
check "平台 mDNS 的实现加回 esp_matter（9.8a 第 4 条）" "grep -q 'target_sources(\${_em_lib} PRIVATE \"\${_em_dir}/connectedhomeip/connectedhomeip/src/platform/ESP32/ESP32DnssdImpl.cpp\")' CMakeLists.txt"
check "PeerAddress 兼容头只强制包含进那一个文件（9.8a 第 9 条）" "grep -q 'esp_matter_controller_pairing_command.cpp' CMakeLists.txt && grep -q 'using chip::Transport::PeerAddress' esp_matter_compat.h"

# ---- 启动：防崩溃循环（红线 1）、晚起、只碰 matter 分区 ----
check "开机晚起（等 SRP 重注册潮过去，memory matter-srp-scale-256）" "$G -q 'MC_START_DELAY_MS\\s+30000' matter_ctrl.cpp"
check "防崩溃循环：RTC 计数，连续 2 次没撑过 60 s 就不起" "$G -q 'RTC_NOINIT_ATTR' matter_ctrl.cpp && $G -q 'MC_MAX_TRIES\\s+2' matter_ctrl.cpp && $G -q 'MC_STABLE_MS\\s+60000' matter_ctrl.cpp && $G -q 's_rtc_tries\\s*=\\s*0' matter_ctrl.cpp"
check "只初始化 / 擦 matter 分区（绝不碰主 nvs、fctry）" "$G -q 'nvs_flash_init_partition\\s*\\(\\s*MC_PART\\s*\\)' matter_ctrl.cpp && $G -q 'nvs_flash_erase_partition\\s*\\(\\s*MC_PART\\s*\\)' matter_ctrl.cpp && grep -qE '#define MC_PART +\"matter\"' matter_ctrl.cpp && $G --absent -q '\\bnvs_flash_(init|erase)\\s*\\(\\s*\\)' matter_ctrl.cpp && $G --absent -q 'fctry' matter_ctrl.cpp"
check "自己的命名空间开在 matter 分区里（nvs_open_from_partition），不落主 nvs" "$G -q 'nvs_open_from_partition\\s*\\(\\s*MC_PART' matter_ctrl.cpp && $G --absent -q '\\bnvs_open\\s*\\(' matter_ctrl.cpp"
check "fabric ID 随机 64 位、非 0（9.3）" "$G -q 'esp_fill_random\\s*\\(\\s*&fid' matter_ctrl.cpp && $G -q 'fid == 0' matter_ctrl.cpp"
check "matter 分区与身份绑定：记号对不上才擦；读出错（不是\"没有\"）时不擦、直接返回（审计）" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('matter_ctrl.cpp').read())
m=re.search(r'static esp_err_t matter_bind\\(.*?\\n\\}', s, re.S); b=m.group(0) if m else ''
g=b.find('if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;')
ok=b.find('if (err == ESP_OK && bound == fid) return ESP_OK;')
e=[x.start() for x in re.finditer(r'nvs_flash_erase_partition\\(MC_PART\\)', b)]
sys.exit(0 if m and 0<ok<g and len(e)>=2 and e[0]<g<e[1] else 1)\""
check "绑定记号的命名空间不叫 mctrl（那是 hubdb 里身份正本的名字，MX03）" "grep -qE '#define MC_BIND_NS +\"mc_bind\"' matter_ctrl.cpp && $G --strings --absent -q '\"mctrl\"' matter_ctrl.cpp"
check "防崩溃计数只认崩溃类复位（wd_boot_reason_get 的真原因：panic / 看门狗），其余复位清零（审计）" "$G -q 'wd_boot_reason_get\\(\\)' matter_ctrl.cpp && $G -q 'ESP_RST_PANIC' matter_ctrl.cpp && $G -q 'ESP_RST_TASK_WDT' matter_ctrl.cpp && $G -q '!crashed\\)' matter_ctrl.cpp && $G --absent -q '\\besp_reset_reason\\s*\\(' matter_ctrl.cpp"
# ---- 凭据（M7 第二步；设计 9.3；TODO T9.51 / T9.52；CHECKLIST MC12–MC15、MX03 / MX04）----
check "用自己的签发器（CUSTOM），不用 esp-matter 的测试签发器" "grep -qxF 'CONFIG_CUSTOM_OPERATIONAL_CREDS_ISSUER=y' $D && ! grep -qxF 'CONFIG_TEST_OPERATIONAL_CREDS_ISSUER=y' $D && $G -q 'set_custom_credentials_issuer\\s*\\(\\s*&s_issuer\\s*\\)' matter_ctrl.cpp"
check "厂商号开发期用 CSA 测试号 0xFFF1（T9.52；esp-matter 默认 4891 是乐鑫的）" "grep -qxF 'CONFIG_ESP_MATTER_CONTROLLER_VENDOR_ID=65521' $D"
check "不用中间 CA：只有根证书与根直接签的 NOC" "python3 check_mc_shape.py no_icac"
check "证书无到期日（kNullCertTime）、notBefore 固定在 2025-01-01（789004800）" "$G -q 'kCertNotBefore = 789004800' mc_issuer.cpp && $G --absent -q 'mNow|SetCurrentEpoch|SetCertificateValidityPeriod' mc_issuer.cpp"
check "IPK 随机生成；控制器这一侧写入同一个值（SetSingleIpkEpochKey）；不用 CHIP 的默认 / 测试 IPK" "$G -q 'esp_fill_random\\s*\\(\\s*out\\.ipk' mc_issuer.cpp && python3 check_mc_shape.py ipk_ctrl && $G --absent -q 'DefaultIpkValue|GroupTesting' mc_issuer.cpp matter_ctrl.cpp"
check "补丁去掉 esp-matter 写的测试组数据与默认 IPK（第 3 条）" "grep -q 'no_test_group_data' $ROOT/tools/patch_esp_matter.py && grep -q 'GroupTesting::InitData' $ROOT/tools/patch_esp_matter.py"
check "签 NOC 只认 commissioner 这一次给的节点号（没给就拒，不自己分配）、只签自己的 fabric" "$G -q 'VerifyOrReturnError\\(m_node_requested, CHIP_ERROR_INCORRECT_STATE\\)' mc_issuer.cpp && $G -q 'VerifyOrReturnError\\(fabric == m_fabric' mc_issuer.cpp && $G --absent -q 'mNextAvailableNodeId|\\+\\+\\s*m_next_node|m_next_node\\s*\\+\\+' mc_issuer.cpp"
check "装身份时核对根证书的公钥就是这把密钥（hubdb 里的两样必须是同一次生成的）" "$G -q 'cert_pub\\.Matches\\(m_root\\.Pubkey\\(\\)\\)' mc_issuer.cpp"
check "身份：读到\"没有\"才新建；读不出来 / 解不开都失败、不新建（换根 = 设备全成孤儿）" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('matter_ctrl.cpp').read())
m=re.search(r'static esp_err_t ident_read\\(.*?\\n\\}', s, re.S); b=m.group(0) if m else ''
ok = 'if (r == MATTER_CTRL_LOAD_EMPTY) {' in b and '*need_new = true;' in b and 'r != MATTER_CTRL_LOAD_OK' in b and 'ESP_ERR_INVALID_CRC' in b and b.count('*need_new = true') == 1
sys.exit(0 if m and ok else 1)\""
check "新建的身份落盘并读回确认之后才装进签发器（MC12）；失败就停" "python3 check_mc_shape.py save_before_load"
check "身份里的私钥用完即清（装进签发器后、各失败路径）" "python3 check_mc_shape.py wipes"
check "hub_matter：身份存 hubdb/mctrl；保存 = put + flush + 读回逐字节比对；读回缓冲清零" "$G -q 'HUB_STORE_NS_MCTRL' ../hub_core/hub_matter.c && $G -q 'hub_store_flush\\(' ../hub_core/hub_matter.c && $G -q 'memcmp\\(back, buf, len\\)' ../hub_core/hub_matter.c && $G -q 'wipe\\(back' ../hub_core/hub_matter.c"
check "hub_matter：日志不带 buf / back（身份是秘密）" "$G --absent -q 'ESP_LOG\\w*\\([^;]*\\b(buf|back)\\b[^;]*\\)' ../hub_core/hub_matter.c"
check "hub_matter：读不出来（非 NOT_FOUND）回 ERR，不回 EMPTY" "grep -Pzo 'if \\(e == ESP_ERR_NOT_FOUND\\) return MATTER_CTRL_LOAD_EMPTY;\\s*\\n(?:\\s*/\\*[^\\n]*\\n)?\\s*ESP_LOGE[^\\n]*\\n\\s*return MATTER_CTRL_LOAD_ERR;' ../hub_core/hub_matter.c"
check "钩子在 hub_svc_start 里登记（main.c 里 hub_svc 在 matter_ctrl_start 之前）" "grep -Pzo '#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL\\n\\s*hub_matter_init\\(\\);' ../hub_core/hub_svc.c && python3 -c \"
import sys; s=open('../../main/main.c').read(); a=s.find('hub_svc_start()'); b=s.find('matter_ctrl_start()'); sys.exit(0 if 0<=a<b else 1)\""
check "身份丢了（hubdb 里没有、matter 分区却记着绑过）不新建，停在 ident_lost（审计）" "python3 check_mc_shape.py ident_lost"
check "esp_matter::start / ScheduleWork 失败时也清掉身份（审计）" "python3 check_mc_shape.py wipe_on_start_fail"
check "恢复出厂全清：先删绑定记号、再删 hubdb 的 mctrl；记号删不掉就保留身份；冻结期间 mctrl 的写也丢掉" "python3 check_mc_shape.py factory_reset"
# ---- 配网与移除（M7 第三步；设计 9.4 B、10.1；CHECKLIST MC12、MC16–MC23、MC37）----
check "节点号落盘读回确认之后才去配（MC12）" "python3 check_mc_shape.py node_before_pair"
check "直接调 PairDevice（不用 esp-matter 吞返回值的 pairing_code）、只在网络上发现、先撤残留委托、失败自己撤（9.8b 第 13 条）" "python3 check_mc_shape.py pair_direct"
check "只能蓝牙配的新设备码直接拒（P3 不做 BLE 配网）" "python3 check_mc_shape.py ble_only_rejected"
check "配网码（含 PIN）用完清零：各拒收路径与收尾" "python3 check_mc_shape.py pin_wiped"
check "期限：总时长短于 hub.matter.commission 的 timeout_ms；发现超时 CHIP 不回调，自己 StopPairing" "python3 check_mc_shape.py deadlines"
check "句子放得下（晚到的 ≤ 199、同步拒的 ≤ 159 字节）" "python3 check_mc_shape.py msg_len"
check "等本机待配网记录：按 discriminator 比 TXT D（9.8b 第 12 条）" "$G -q 'mc_code_disc_match\\(s_op\\.disc_short, s_op\\.disc' mc_comm.cpp && $G -q 'mdns_lookup_delegated_service\\(' mc_comm.cpp"
check "节点表开机读回：解不开报错、不覆盖（覆盖 = next_node 回到 2，号会重发）；读回成功后才丢 PENDING；读不回这次开机不配网" "python3 check_mc_shape.py boot_load_safe"
check "移除：设备确认了才删；force 才在不答话时删" "python3 -c \"
import re,sys
sys.path.insert(0,'../../tools'); from grep_code import strip_comments
s=strip_comments(open('mc_comm.cpp').read(), keep_strings=True)
i=s.find('static void on_unpair('); b=s[i:s.find('\\n}', i)]
sys.exit(0 if 'if (err != CHIP_NO_ERROR && !s_op.force) {' in b and b.find('!s_op.force') < b.find('mc_nodes_remove') else 1)\""
check "REST：/hub/job 与 /hub/device/remove 先 CROSS_ORIGIN_GUARD 再 AUTH（红线 5）；移除要管理密码与 confirm=REMOVE" "python3 check_mc_shape.py rest_guards"
check "Muse：作业的结论晚到 —— 不当场回包、结束时发 link.result；新会话清掉旧的等待" "python3 check_mc_shape.py muse_deferred"
check "作业表：结束时记审计并通知、hub_svc 每轮过期；当场拒的放弃作业（不留结论、不另记审计，审计 #4）" "python3 check_mc_shape.py jobs_audit"
check "先收尾再 StopPairing（它同步回调，审计 #1）；计时器按代号判断；回调都看阶段" "python3 check_mc_shape.py stop_after_finish"
check "配网成功、落盘失败：报失败、内存里也撤掉（审计 #2）" "python3 check_mc_shape.py save_fail_consistent"
check "节点表满了当场拒（ESP_ERR_NO_MEM，审计 #5）" "$G -q 'return ESP_ERR_NO_MEM;' mc_comm.cpp && $G -q 's_nodes\\.n >= s_nodes\\.max' mc_comm.cpp"
check "内置 PAA 快照与清单逐个对得上（tools/fetch_paa.py --check；T9.53）" "python3 ../../tools/fetch_paa.py --check"
check "设备认证用内置快照（CUSTOM），不在线查 DCL、不用测试库" "grep -qxF 'CONFIG_CUSTOM_ATTESTATION_TRUST_STORE=y' $D && ! grep -qxF 'CONFIG_DCL_ATTESTATION_TRUST_STORE=y' $D && grep -qxF 'CONFIG_CUSTOM_ATTESTATION_TRUST_STORE=y' $ROOT/sdkconfig.direct && $G -q 'set_custom_attestation_trust_store\\(s_store\\)' mc_paa.cpp"
check "PAA 信任库在 setup_commissioner 之前装上（它在里面取一次）" "python3 -c \"
import sys; s=open('matter_ctrl.cpp').read(); i=s.find('mc_paa_install()'); j=s.find('client.setup_commissioner()'); sys.exit(0 if 0<i<j else 1)\""
check "身份格式的主机测试（往返、各种坏 blob、失败不留半截密钥）" "bash test_host/run.sh"
check "控制器节点号 1（9.3）" "$G -q 'kControllerNodeId\\s*=\\s*1\\b' matter_ctrl.cpp"
check "不做 ICD 注册（9.1 v1）" "$G -q 'set_icd_registration\\s*\\(\\s*false\\s*\\)' matter_ctrl.cpp"
check "CHIP 的 API 都投到 CHIP 任务里（ScheduleWork）" "$G -q 'PlatformMgr\\(\\)\\.ScheduleWork\\s*\\(\\s*setup_in_chip_task' matter_ctrl.cpp"
check "起控制器的任务用内部栈（它写 NVS，psram_task.h 规则 1）" "$G -q '\\bxTaskCreate\\s*\\(\\s*start_task' matter_ctrl.cpp && $G --absent -q 'psram_task_create|xTaskCreateWithCaps' matter_ctrl.cpp"
check "重复调用 start 不起第二个任务" "$G -q 's_started' matter_ctrl.cpp"
check "状态快照加锁拷贝（httpd 任务读、启动任务 / CHIP 任务写）" "$G -q 'portENTER_CRITICAL\\s*\\(\\s*&s_mux' matter_ctrl.cpp && $G -q 'matter_ctrl_get_status' matter_ctrl.cpp"
check "9.8b 第 14 条：不用 esp_matter 的 subscribe_command / read_command（会拖进 DataModelLogger，+590 KB）" "$G --absent -q 'subscribe_command|read_command|send_subscribe_attr_command|send_read_command|DataModelLogger' *.cpp *.h include/*.h"
check "日志不打密钥 / 证书" "$G --absent -q 'ESP_LOG\\w*\\([^;]*(key|Key|rcac|noc|cert)[^;]*\\)' matter_ctrl.cpp"

# ---- 启动序列（设计 2.6 第 13 步）：zhub 之后、muse_link 之前；只在 MATTER_CTRL 下 ----
check "main.c：matter_ctrl_start() 只在 #if CONFIG_BITENGINE_ENABLE_MATTER_CTRL 里" "python3 - <<'PY'
import re,sys
src=open('../../main/main.c').read()
hits=[m.start() for m in re.finditer(r'matter_ctrl_start\(', src)]
def guarded(i):
    j=src.rfind('#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL',0,i)
    return j>=0 and j>src.rfind('#endif',0,i)
sys.exit(0 if hits and all(guarded(i) for i in hits) else 1)
PY"
check "main.c：在 zhub_start 之后、muse_hold_apply 之前（2.6 第 12 → 13 → 14 步）" "python3 -c \"
import sys; s=open('../../main/main.c').read()
z=s.find('zhub_start()'); m=s.find('matter_ctrl_start()'); u=s.find('muse_hold_apply()')
sys.exit(0 if 0<=z<m<u else 1)\""

# ---- 接口：新端点先 CROSS_ORIGIN_GUARD 再 AUTH_REQUIRED（红线 5）----
check "GET /api/v1/matter/status：在 hub_api.c（只在直连型编，不占 HA 型的槽位），只在 MATTER_CTRL 下注册" "python3 - <<'PY'
import re,sys
s=open('../webui/hub_api.c').read()
i=s.find('{ .uri = \"/api/v1/matter/status\"')
j=s.rfind('#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL',0,i)
sys.exit(0 if i>0 and j>s.rfind('#endif',0,i) else 1)
PY"
check "handle_matter_status：首行 CROSS_ORIGIN_GUARD、其后 AUTH_REQUIRED（红线 5）" "grep -Pzo 'static esp_err_t handle_matter_status\\(httpd_req_t \\*req\\)\\s*\\n\\{\\s*\\n\\s*CROSS_ORIGIN_GUARD\\(req\\);\\s*\\n\\s*AUTH_REQUIRED\\(req\\);' ../webui/hub_api.c"
check "状态输出截断时回 500（S3 第 4 问）" "grep -A22 'static esp_err_t handle_matter_status' ../webui/hub_api.c | grep -q 'send_error(req, 500, \"truncated\")'"

# ---- MC06：网页上传 app 的大小上限按目标槽判（直连型槽 3.5 MB，镜像带 Matter 约 3 MB；红线 1），HA 型照旧 3 MB ----
check "rest_api.c：直连型按 update_part->size 判，HA 型分支照旧写死 3 MB" "grep -Pzo '#if CONFIG_BITENGINE_PROFILE_DIRECT\\n(?:[^\\n]*\\n){1,10}?\\s*if \\(\\(size_t\\)content_len > update_part->size\\)[^\\n]*file_too_large[^\\n]*\\n#else\\n\\s*if \\(content_len > 3 \\* 1024 \\* 1024\\)' ../webui/rest_api.c"
check "直连型前端把 OTA 页的上限提到 3.5 MB（共用的 ota.js 不动）" "grep -qx 'OTA_TARGETS.fw.maxSize = 0x380000;' $ROOT/frontend/direct/js/ota_slot.js && grep -q 'maxSize: 3 \\* 1024 \\* 1024' $ROOT/frontend/js/pages/ota.js"

# ---- 访谈（设计 9.6；M7 第四步）：读厂家 / 型号 / 端点 / 簇 / 电源，存 hubdb，hub_core 映射成能力与设备类 ----
check "访谈只读需要的属性（BasicInformation 1–5、各端点 Descriptor 0/1、PowerSource FeatureMap），走 BufferedReadCallback" "$G -q 'AttributePathParams\\(chip::ClusterId\\(0x001D\\), chip::AttributeId\\(0\\)\\)' mc_iv.cpp && $G -q 'chip::ClusterId\\(0x002F\\), chip::AttributeId\\(0xFFFC\\)' mc_iv.cpp && $G -q 'BufferedReadCallback \\*s_buf_cb;' mc_iv.cpp"
check "同一时刻只有一个访谈读请求" "python3 check_mc_shape.py iv_one_at_a_time"
check "访谈有总时限、失败退避、有次数上限；超时后迟到的回调不认" "python3 check_mc_shape.py iv_deadline"
check "\"加好了\"只推配网刚成功时那一次（开机补访谈不推）" "python3 check_mc_shape.py iv_added_only"
check "移除后忘掉访谈记录（内存与 hubdb）" "python3 check_mc_shape.py iv_forget_on_remove"
check "BufferedReadCallback 每次读前重建（审计）" "python3 check_mc_shape.py iv_buf_fresh"
check "读成功但没根端点簇表 ≠ 访谈完了（审计）" "python3 check_mc_shape.py iv_complete_needed"
check "跨任务读访谈状态不撕裂、开机读回在锁里写（审计）" "python3 check_mc_shape.py iv_cross_task"
check "访谈表在 PSRAM、只在 CHIP 任务里写、别的任务加锁拷" "$G -q 'heap_caps_calloc\\(CONFIG_BITENGINE_MATTER_MAX_NODES, sizeof\\(iv_slot_t\\), MALLOC_CAP_SPIRAM' mc_iv.cpp && grep -A8 '^bool matter_ctrl_node_iv' mc_iv.cpp | grep -q 'xSemaphoreTake(s_lock'"
check "开机：读回的访谈记录没做完（或读不回来）才排访谈" "$G -q 's_iv\\[i\\]\\.queued = !\\(s_iv\\[i\\]\\.iv\\.flags & MC_IV_F_DONE\\)' mc_iv.cpp"
check "访谈记录的格式：主机测试（往返、截断、厂商私有簇丢弃、坏 blob 拒）" "grep -q 'mc_iv_test' test_host/run.sh"

# ---- 订阅（设计 9.5；M7 第五步）：在线与读数 ----
check "订阅按 SendAutoResubscribeRequest 建、间隔 [1, 300]、清旧订阅；同步失败走 teardown 不重复释放" "python3 check_mc_shape.py sub_create"
check "重订：刚断就地重订一次（5 s，超时断的重建 CASE），再不成整条结束、按 30 → 120 s 带抖动重建（受同时在建上限约束）；设备拒了路径数就收窄；不用 CHIP 默认策略" "python3 check_mc_shape.py sub_resub_policy"
check "ReadClient 生命周期：OnDone 里删、主动拆先删 ReadClient 再删回调；移除时拆；同一时刻最多 2 条在建" "python3 check_mc_shape.py sub_lifetime"
check "访谈完（含开机读回）就开订、控制器起来照表立即开订、路径没变不动" "python3 check_mc_shape.py sub_wiring"
check "读数跨任务：CHIP 任务里在锁里写、别的任务加锁拷" "python3 check_mc_shape.py sub_cross_task"
check "订阅表在 PSRAM；ReadClient / 回调 / 路径经 chip::Platform 分配（本配置走 PSRAM）" "$G -q 'heap_caps_calloc\\(CONFIG_BITENGINE_MATTER_MAX_NODES, sizeof\\(sub_slot_t\\), MALLOC_CAP_SPIRAM' mc_sub.cpp && $G -q 'chip::Platform::New<ReadClient>' mc_sub.cpp && grep -q '^CONFIG_ESP_MATTER_MEM_ALLOC_MODE_EXTERNAL=y' ../../sdkconfig.direct"
check "不用 esp-matter 的 subscribe_command（拖进 DataModelLogger）：订阅自己管 ReadClient" "$G --absent -q 'subscribe_command|interaction::subscribe::send_request' mc_sub.cpp"
check "订阅的纯逻辑：主机测试（路径、换算、翻转事件、离线判定、退避）" "grep -q 'mc_sub_test' test_host/run.sh"
check "第五步审计：空保活也算听到、先置在建再发起、路径逐字段比、计时器挂不上要说、移除落盘失败也拆订阅" "python3 check_mc_shape.py sub_audit5"
check "/matter/status 报订阅数（subs / subs_up）" "grep -q 'mc_sub_status(&out->subs, &out->subs_up);' matter_ctrl.cpp && grep -q 'subs_up' ../webui/hub_api.c"

# ---- 控制（设计 9.5「命令」；M7 第六步） ----
check "控制同一时刻一件、按 gen 认领、静态完成信号先清、兜底计时器先挂、调用方超时就放弃" "python3 check_mc_shape.py ctl_single"
check "结论口径：3 s 内报回目标 = confirmed（本来就是也算）、否则 delivered；没发出去与没回应分开；连接回调核节点号" "python3 check_mc_shape.py ctl_results"
check "第六步审计：发了没有分开报、调用方核对是不是自己这件、查不了敏感标记回忙不放行" "python3 check_mc_shape.py ctl_audit6"
check "允许测试设备（MC22）：默认关、测试 PAA 只在开着且生产 PAA 找不到时查、setup 后与切换时设 CD 测试密钥、编译开关仍是 n" "python3 check_mc_shape.py allow_test"
check "命令在 CHIP 任务里发（ScheduleWork → GetConnectedDevice → InvokeCommandRequest），不用 esp-matter 吃 JSON 的 cluster_command" "$G -q 'chip::Controller::InvokeCommandRequest\\(' mc_ctl.cpp && $G --absent -q 'cluster_command|interaction::invoke::send_request' mc_ctl.cpp"
check "控制的纯逻辑：主机测试（亮度换算往返、到没到目标、本来就是目标）" "grep -q 'mc_ctl_reached' test_host/mc_sub_test.c && grep -q 'mc_ctl_already' test_host/mc_sub_test.c"
check "setup 里建控制的锁（RUNNING 之前）" "grep -A6 '^void mc_comm_on_setup' mc_comm.cpp | grep -q 'mc_ctl_init();'"

echo ""; echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
