#!/bin/bash
# components/ota_mgr/VERIFY.sh — MG24 NCP OTA (XMODEM over UART) 设计合规性验证
# 编译通过后运行：bash components/ota_mgr/VERIFY.sh
#
# 检查项来源（全部可追溯到权威出处，不凭记忆）：
#   Gecko Bootloader SDK  ~/src/mg24-fw/sdk/platform/bootloader/
#   docs/01_software_architecture_v1.md §5.3（优雅断开流程）
#   components/watchdog/mg24_reset.c:80-84（IO23 整板复位硬件约束）

cd "$(dirname "$0")" || exit 1

PASS=0; FAIL=0
check() {
    if eval "$2" > /dev/null 2>&1; then
        echo "✅ $1"; ((PASS++))
    else
        echo "❌ $1"; ((FAIL++))
    fi
}

echo "==== 文件存在性 ===="
# 只在私有检出里有的内部文档 / 工具（docs/、发布与产线脚本）：公开仓库不导出它们（tools/oss_export.py），
# 那里跳过而不是报红；私有检出里照常判。标志是 docs/开发索引.md 在不在。
internal() { if [ -f ../../docs/开发索引.md ]; then check "$@"; else echo "⏭  $1 —— 依赖内部文档 / 工具，本检出没有，跳过"; fi; }

check "ota_mg24.c exists"            "[ -f ota_mg24.c ]"
check "xmodem.c exists"              "[ -f xmodem.c ]"
check "ota_mg24.h exists"            "[ -f include/ota_mg24.h ]"
check "xmodem.h exists"              "[ -f include/xmodem.h ]"
# ---- www_image（2026-09-10）：两条 www 更新路径共用的形状校验 ----
check "www_image.c / .h exist"       "[ -f www_image.c ] && [ -f include/www_image.h ]"
check "www_image: 长度必须等于分区"   "grep -q 'len != p->size' www_image.c"
check "www_image: magic 常量来自 spiffsgen" "grep -q '0x20140529' www_image.c && grep -q 'CONFIG_SPIFFS_PAGE_SIZE' www_image.c"
check "www_image: 首末块都查"         "grep -q 'blocks - 1' www_image.c"
check "www_image: 逐扇区擦写"         "grep -q 'esp_partition_erase_range' www_image.c && grep -q 'esp_partition_write' www_image.c"
check "www_image: 两条路径都用它"     "grep -q 'www_image_validate' ../webui/rest_api.c && grep -q 'www_image_validate' ../ota_cloud/ota_cloud.c"
check "rest_api 不再自己擦写 www 分区" "! grep -q 'esp_partition_erase_range(www' ../webui/rest_api.c"
check "CMakeLists.txt exists"        "[ -f CMakeLists.txt ]"

echo ""
echo "==== XMODEM 协议常量（btl_xmodem.h 权威值）===="
check "SOH  = 0x01"                  "grep -q 'XMODEM_SOH.*0x01' include/xmodem.h"
check "EOT  = 0x04"                  "grep -q 'XMODEM_EOT.*0x04' include/xmodem.h"
check "ACK  = 0x06"                  "grep -q 'XMODEM_ACK.*0x06' include/xmodem.h"
check "NAK  = 0x15"                  "grep -q 'XMODEM_NAK.*0x15' include/xmodem.h"
check "CAN  = 0x18"                  "grep -q 'XMODEM_CAN.*0x18' include/xmodem.h"
check "C    = 0x43"                  "grep -q 'XMODEM_C.*0x43' include/xmodem.h"
check "data size = 128"              "grep -q 'XMODEM_DATA_SIZE.*128' include/xmodem.h"
check "packet size = 133"            "grep -q 'XMODEM_PACKET_SIZE.*133' include/xmodem.h"

echo ""
echo "==== CRC-16/XMODEM（btl_crc16.c 权威实现）===="
check "CRC seeded with 0"            "grep -q 'XMODEM_DATA_SIZE, 0)' xmodem.c"
check "CRC poly 0x1021 present"      "grep -q '0x1021\|>> 4\|<< 5' xmodem.c"
check "CRC high byte first in frame" "grep -q 'crc >> 8' xmodem.c"
check "CRC self-test exists"         "grep -q 'xmodem_crc16_selftest' xmodem.c"

echo ""
echo "==== GBL 校验（btl_gbl_format.h:49）===="
check "GBL magic 0x03A617EB checked" "grep -q '0x03A617EB' include/gbl_image.h && grep -q 'GBL_TAG_ID_HEADER_V3' gbl_image.c"
check "GBL header validated"         "grep -q 'gbl_valid\|validate_gbl\|GBL_MAGIC' ota_mg24.c"

echo ""
echo "==== 最后一块填充规则（btl_gbl_parser.c:848 PARSER_EOF 陷阱）===="
# GBL 解析完成后再喂整块 → PARSER_EOF → bootloader 发 CAN 中止。
# 因此块数必须是 ceil(size/128)，绝不能多发一整块。
check "block count uses ceil"        "grep -q '+ XMODEM_DATA_SIZE - 1) / XMODEM_DATA_SIZE\|(len + 127) / 128' ota_mg24.c"
check "padding documented"           "grep -q 'PARSER_EOF\|多发\|extra full block\|pad' ota_mg24.c"

echo ""
echo "==== Bootloader 会话（btl_comm_xmodem_common.c 权威流程）===="
check "menu prompt 'BL >' awaited"   "python3 ../../tools/grep_code.py --strings -q 'define MG24_BL_PROMPT \"BL > \"' include/mg24_bl_entry.h && python3 ../../tools/grep_code.py -q 'wait_text\\(io->ctx, MG24_BL_PROMPT' mg24_bl_entry.c"
check "sends '1' to start upload"    "grep -q \"'1'\" ota_mg24.c"
check "waits for 'begin upload'"     "grep -q 'begin upload' ota_mg24.c"
check "waits for 'C' before block 1" "grep -q 'XMODEM_C' ota_mg24.c"
check "detects 'upload complete'"    "grep -q 'Serial upload complete' ota_mg24.c"
check "detects 'upload aborted'"     "grep -q 'Serial upload aborted' ota_mg24.c"
check "sends '2' to run app"         "grep -q \"'2'\" ota_mg24.c"

echo ""
echo "==== 波特率切换（bootloader 115200 vs NCP 460800）===="
# bootloader 的波特率是 MG24 镜像的编译期属性，两侧成对烧录，所以必须来自
# Kconfig 而不是头文件里的常量——曾经那里写着 115200，实际早已是 460800。
check "bl baud from Kconfig"         "grep -q 'CONFIG_BITENGINE_MG24_BOOTLOADER_BAUD' ota_mg24.c"
check "no stale baud constant"       "! grep -q 'define MG24_BOOTLOADER_BAUD' include/ota_mg24.h"
check "uart_bridge_set_baud exists"  "grep -q 'uart_bridge_set_baud' ../ezsp_bridge/uart_bridge.h"
check "baud restored to EZSP_UART_BAUD" "grep -q 'EZSP_UART_BAUD' ota_mg24.c"

echo ""
echo "==== 硬件约束：绝不碰 IO23（mg24_reset.c:80-84 整板复位）===="
# IO23 拉低经 R113/R58 拖垮 C5 的 EN —— 整板复位。进 bootloader 只能靠
# nBOOT + 让 MG24 上的固件自己软复位（NCP 认 ASH RST，RAILtest 认 CLI reset），不能用 MG24_RESET_PIN。
check "never drives MG24_RESET_PIN"  "python3 ../../tools/grep_code.py --absent -q 'MG24_RESET_PIN' ota_mg24.c mg24_bl_entry.c"
check "never calls mg24_do_reset"    "python3 ../../tools/grep_code.py --absent -q 'mg24_do_reset' ota_mg24.c mg24_bl_entry.c"
check "uses nBOOT pin"               "python3 ../../tools/grep_code.py -q 'mg24_set_nboot\\(0\\)' ota_mg24.c"
check "uses ASH RST for soft reset"  "python3 ../../tools/grep_code.py -q '0x1A, 0xC0, 0x38, 0xBC, 0x7E' mg24_bl_entry.c"
# RAILtest（射频测试固件）不认 ASH RST；没有 CLI reset 这一步，刷回 NCP 只能拆机走 J-Link。
# 顺序与"健康 NCP 绝不收到 reset 字节"由 test_host/run.sh 的 3c 段在假 MG24 上钉住，这里只钉形状。
check "RAILtest CLI reset fallback"  "python3 ../../tools/grep_code.py --strings -q 'CLI_RESET\\[\\] = \"\\\\rreset\\\\r\"' mg24_bl_entry.c && python3 ../../tools/grep_code.py -q 'io->send\\(io->ctx, CLI_RESET' mg24_bl_entry.c"
check "entry goes through the module" "python3 ../../tools/grep_code.py -q 'mg24_bl_entry_run\\(&s_bl_io' ota_mg24.c"
check "bl entry host test wired"     "grep -qxF '\"\$OUT/bl_entry_test\"' test_host/run.sh"
check "bl entry built into firmware" "grep -q 'mg24_bl_entry.c' CMakeLists.txt"
check "IO23 hazard documented"       "grep -q 'R113\|R58\|整板复位\|whole-board' ota_mg24.c"

echo ""
echo "==== 桥接优雅断开（docs/01 §5.3 步骤 1-4）===="
check "switches bridge to DISABLED"  "grep -q 'BRIDGE_MODE_DISABLED' ota_mg24.c"
check "restores previous bridge mode" "grep -q 'prev_mode\|saved_mode' ota_mg24.c"
check "BRIDGE_STATE_OTA declared"    "grep -q 'BRIDGE_STATE_OTA' ../ezsp_bridge/include/ezsp_bridge.h"
# 这条不变式在 watchdog v2 重构（b94f54e）里从 health_monitor.c 搬到了 wd_probes.c，
# 检查一直指着旧文件，于是自那以后一直是红的 —— 检查过期，不是行为丢了。
check "health probe skips in OTA"    "grep -q 'BRIDGE_STATE_RUNNING' ../watchdog/wd_probes.c"

echo ""
echo "==== S1 安全 ===="
check "size upper bound enforced"    "grep -q 'MG24_OTA_MAX_SIZE' ota_mg24.c"
check "rejects empty/short image"    "grep -q 'len < \|size < ' ota_mg24.c"
check "no secrets in logs"           "! grep -rn 'ESP_LOG.*password\|ESP_LOG.*token' ota_mg24.c"
check "concurrent OTA rejected"      "grep -q 'ESP_ERR_INVALID_STATE\|in_progress' ota_mg24.c"

echo ""
echo "==== S2 线程上下文 ===="
check "transfer runs in own task"    "grep -q 'xTaskCreate' ota_mg24.c"
check "no blocking call in httpd ctx" "grep -q 'ota_mg24_start_flash\|task' ota_mg24.c"
check "state guarded by mutex/atomic" "grep -q 'SemaphoreHandle_t\|atomic_' ota_mg24.c"

echo ""
echo "==== S3 资源生命周期 ===="
check "staging buffer freed"         "grep -q 'free(s_image)' ota_mg24.c && [ \$(grep -c 'free_image()' ota_mg24.c) -ge 4 ]"
check "abort path frees buffer"      "grep -q 'ota_mg24_abort' ota_mg24.c"
check "nBOOT released on every exit" "[ \$(grep -c 'mg24_set_nboot(1)\|nboot.*1' ota_mg24.c) -ge 2 ]"
check "bridge restored on failure"   "grep -q 'restore_bridge\|cleanup' ota_mg24.c"

echo ""
echo "==== 镜像来源追溯（mg24_image_record）===="
check "record module exists"         "[ -f mg24_image_record.c ] && [ -f include/mg24_image_record.h ]"
check "record in build"              "grep -q 'mg24_image_record.c' CMakeLists.txt"
check "digest of the UPLOADED bytes recorded on success" "grep -q 'mg24_image_record_save_digest(s_upload_sha' ota_mg24.c"
check "recorded before buffer freed" "[ \$(grep -n 'mg24_image_record_save' ota_mg24.c | cut -d: -f1) -lt \$(grep -n 'free_image();' ota_mg24.c | tail -1 | cut -d: -f1) ]"
check "cache warmed at init"         "grep -q 'mg24_image_record_get();' ota_mg24.c"
check "nvs handle closed everywhere" "[ \$(grep -c 'nvs_open(' mg24_image_record.c) -le \$(grep -c 'nvs_close(' mg24_image_record.c) ]"
check "implausible clock -> 0"       "grep -q 'PLAUSIBLE_EPOCH' mg24_image_record.c"

echo ""
echo "==== 升级后回读 NCP 版本 ===="
check "version re-read after flash"  "grep -q 'ncp_version_query(' ota_mg24.c"
# 必须在 ota_release 之前：释放之后客户端随时可能接入，ASH RST 会打断它的握手
check "re-read before ota_release"   "[ \$(grep -n 'ncp_version_query(' ota_mg24.c | head -1 | cut -d: -f1) -lt \$(grep -n 'ezsp_bridge_ota_release();' ota_mg24.c | head -1 | cut -d: -f1) ]"
check "only re-read when flashed ok"  "grep -q 'flashed_ok' ota_mg24.c"

echo ""
echo "==== REST 端点接线 ===="
check "/api/v1/ota/zigbee wired"     "grep -q 'ota/zigbee' ../webui/rest_api.c"
check "image sha exposed"            "grep -q 'image_sha256' ../webui/rest_api.c"
check "sha sanitized before JSON"    "grep -q 'json_sanitize_copy(safe_sha' ../webui/rest_api.c"
check "WebUI has zigbee OTA card"    "grep -q 'zigbeeOtaUpload' ../../frontend/js/pages/ota.js"
check "ota/zigbee not stubbed"       "! grep -q '\"/api/v1/ota/zigbee\".*handle_not_implemented' ../webui/rest_api.c"
check "zigbee/bootloader not stubbed" "! grep -q '\"/api/v1/zigbee/bootloader\".*handle_not_implemented' ../webui/rest_api.c"
check "progress endpoint exists"     "grep -q 'ota/zigbee/status' ../webui/rest_api.c"

echo ""
echo "==== T1.2 .gbl 签名验证在 C5 侧（2026-09-14 决策：bootloader 不动）===="
# 在 ota_mg24_commit() 函数体内，<模式>（awk 正则）第一次出现的相对行号；没有则为空
commit_line() { awk -v pat="$1" '/^static esp_err_t commit_staged\(/{f=1} f{n++; if ($0 ~ pat) {print n; exit}} f&&/^}/{exit}' ota_mg24.c; }
# A 必须在 B 之前出现，且两者都必须存在 —— 缺一个就是红（别让 [ -lt ] 退化成单参数恒真）
before_in_commit() { local a b; a=$(commit_line "$1"); b=$(commit_line "$2"); [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ]; }
# 同上，但限定在任意一个函数体内：before_in_fn <函数名> <A> <B>
fn_line() { awk -v fn="$1" -v pat="$2" '$0 ~ ("^[a-z_][a-z_ ]*" fn "\\(") {f=1} f{n++; if ($0 ~ pat) {print n; exit}} f&&/^}/{exit}' ota_mg24.c; }
before_in_fn() { local a b; a=$(fn_line "$1" "$2"); b=$(fn_line "$1" "$3"); [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ]; }
# 同上，但作用在 ota_cloud 的 ncp_task 上
cloud_line() { awk -v pat="$1" '/^static void ncp_task/{f=1} f{n++; if ($0 ~ pat) {print n; exit}} f&&/^}/{exit}' ../ota_cloud/ota_cloud.c; }
before_in_cloud() { local a b; a=$(cloud_line "$1"); b=$(cloud_line "$2"); [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ]; }
check "gbl_image.c / .h exist"                "[ -f gbl_image.c ] && [ -f include/gbl_image.h ]"
check "gbl_crypto_psa.c exists"               "[ -f gbl_crypto_psa.c ]"
check "signature tag id 0xF70A0AF7 (btl_gbl_format.h:87)" "grep -q '0xF70A0AF7' include/gbl_image.h"
check "header 'signed' bit 0x100 cleared on strip"        "grep -q 'GBL_TYPE_SIGNATURE_ECDSA' gbl_image.c"
check "strip recomputes END CRC"              "python3 ../../tools/grep_code.py -q 'gbl_crc32\\(img, new_len - 4' gbl_image.c"
check "signature must be last before END"     "grep -q 'sig_is_last' gbl_image.c"
check "verify goes through PSA (psa_verify_hash)" "grep -q 'psa_verify_hash' gbl_crypto_psa.c"
check "key imported as SECP_R1 public point"  "grep -q 'PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1)' gbl_crypto_psa.c"
check "PSA key handle destroyed after verify" "grep -q 'psa_destroy_key' gbl_crypto_psa.c"
check "no memcmp-style signature compare"     "python3 ../../tools/grep_code.py --absent -q 'memcmp\\(.*sig' gbl_image.c gbl_crypto_psa.c ota_mg24.c"
check "public key derived at build from mg24/release/*.pub" "grep -q 'mg24_gbl_signing_key.pub' CMakeLists.txt && grep -q 'gen_mg24_pubkey.py' CMakeLists.txt"
check "generator exists"                      "[ -f ../../tools/gen_mg24_pubkey.py ]"
check "public key committed"                  "[ -f ../../mg24/release/mg24_gbl_signing_key.pub ]"
check "generator accepts the committed key"   "python3 ../../tools/gen_mg24_pubkey.py ../../mg24/release/mg24_gbl_signing_key.pub /dev/null"
check "Kconfig switch, default y"             "grep -A2 'config BITENGINE_MG24_REQUIRE_SIGNED_GBL' ../../main/Kconfig.projbuild | grep -q 'default y'"
check "sdkconfig knows the switch (idf.py reconfigure ran)" "grep -q '^CONFIG_BITENGINE_MG24_REQUIRE_SIGNED_GBL=y' ../../sdkconfig"
check "commit: signature verified before the flash task exists" "before_in_commit 'gbl_sig_verify\\(' 'xTaskCreate\\(ota_task'"
check "commit: strip after verify"            "before_in_commit 'gbl_sig_verify\\(' 'gbl_strip_signature\\('"
check "commit: upload digest taken before strip" "before_in_commit 'gbl_sha256\\(s_upload_sha|gbl_sha256\\(s_image' 'gbl_strip_signature\\('"
check "commit: rejection decided before the task exists" "before_in_commit 'ncp_image_unsigned' 'xTaskCreate\\(ota_task'"
check "a present signature is always verified (not gated by Kconfig)" "python3 ../../tools/grep_code.py -q 'if \\(sig\\.has_signature\\)' ota_mg24.c"
check "policy helper decides only the unsigned case" "grep -q 'signature_required' ota_mg24.c"
check "cloud source always requires a signature" "python3 ../../tools/grep_code.py -q 'OTA_MG24_SOURCE_CLOUD\\) return true' ota_mg24.c"
check "cloud path declares itself CLOUD"      "python3 ../../tools/grep_code.py -q 'OTA_MG24_SOURCE_CLOUD' ../ota_cloud/ota_cloud.c"
check "web path declares itself WEB"          "python3 ../../tools/grep_code.py -q 'OTA_MG24_SOURCE_WEB' ../webui/rest_api.c"
check "REST returns the reject code"          "grep -q 'ota_mg24_last_reject_code' ../webui/rest_api.c"
check "cloud records the reject code"         "grep -q 'ota_mg24_last_reject_code' ../ota_cloud/ota_cloud.c"
check "four reject codes defined"             "grep -q 'ncp_image_unsigned' ota_mg24.c && grep -q 'ncp_signature_invalid' ota_mg24.c && grep -q 'ncp_image_malformed' ota_mg24.c && grep -q 'not_a_gbl_image' ota_mg24.c"
check "old record API gone (record takes a digest)" "python3 ../../tools/grep_code.py --absent -q 'mg24_image_record_save\\(' ota_mg24.c mg24_image_record.c include/mg24_image_record.h"
check "frontend explains the new codes"       "grep -q 'ncp_image_unsigned' ../../frontend/js/pages/ota.js && grep -q 'ncp_signature_invalid' ../../frontend/js/pages/ota.js && grep -q 'ncp_image_malformed' ../../frontend/js/pages/ota.js"
check "host test harness present"             "[ -f test_host/run.sh ] && [ -f test_host/gbl_test.c ] && [ -f test_host/make_vectors.py ]"

echo ""
echo "==== T1.2 步骤 3：实体按键开发者解锁（密码 + 在场，两者缺一不可）===="
check "ncp_unlock.c / .h exist (pure, host-tested)" "[ -f ncp_unlock.c ] && [ -f include/ncp_unlock.h ] && [ -f test_host/unlock_test.c ] && grep -q 'unlock_test' test_host/run.sh"
check "windows: arm 30 s, unlock 600 s"       "grep -q 'NCP_UNLOCK_ARM_S      30' include/ncp_unlock.h && grep -q 'NCP_UNLOCK_WINDOW_S   600' include/ncp_unlock.h"
check "confirm consumes the window"           "python3 ../../tools/grep_code.py -q 'u->arm_until    = 0;' ncp_unlock.c"
check "unlock state lives in RAM only (reboot locks)" "python3 ../../tools/grep_code.py --absent -q 'nvs_' ncp_unlock.c && python3 ../../tools/grep_code.py --absent -q '(unlock.*nvs_|nvs_.*unlock)' ota_mg24.c && python3 ../../tools/grep_code.py -q 'ncp_unlock_lock\\(&s_unlock\\);' ota_mg24.c"
check "policy: unlock only lifts the UNSIGNED case on the web path" "python3 ../../tools/grep_code.py -q 'return !ncp_unlock_active' ota_mg24.c"
check "policy: cloud still strict (before the unlock check)" "before_in_fn 'signature_required' 'OTA_MG24_SOURCE_CLOUD\\\\) return true' 'ncp_unlock_active'"
check "button confirmation subscribed (BUTTON_EVENT_HOLD_CONFIRM)" "python3 ../../tools/grep_code.py -q 'esp_event_handler_register\\(BUTTON_EVENT, BUTTON_EVENT_HOLD_CONFIRM' ota_mg24.c"
check "button handler only confirms, never arms"  "python3 ../../tools/grep_code.py --absent -q 'ncp_unlock_arm' <(awk '/static void on_button_hold_confirm/{f=1} f{print} f&&/^}/{exit}' ota_mg24.c)"
check "arm / lock / status API declared"      "grep -q 'ota_mg24_unlock_arm' include/ota_mg24.h && grep -q 'ota_mg24_unlock_lock' include/ota_mg24.h && grep -q 'ota_mg24_unlock_get' include/ota_mg24.h"
check "arming and confirmation are WARN log lines" "grep -c 'ESP_LOGW(TAG, \"NCP developer unlock' ota_mg24.c | grep -q '^[2-9]'"
check "unsigned accept under unlock is a WARN log line" "grep -q 'ESP_LOGW(TAG, \"unsigned NCP image accepted' ota_mg24.c"
check "REST: POST /ota/zigbee/unlock guarded (cross-origin before auth)" "awk '/static esp_err_t handle_ota_zigbee_unlock_post/{f=1} f&&/CROSS_ORIGIN_GUARD/{g=NR} f&&/AUTH_REQUIRED/{a=NR; exit} END{exit !(g && a && g<a)}' ../webui/rest_api.c"
check "REST: GET /ota/zigbee/unlock requires auth" "awk '/static esp_err_t handle_ota_zigbee_unlock_get/{f=1} f&&/AUTH_REQUIRED/{a=1; exit} END{exit !a}' ../webui/rest_api.c"
check "REST: both routes wired"               "grep -q '\"/api/v1/ota/zigbee/unlock\",  *HTTP_GET' ../webui/rest_api.c && grep -q '\"/api/v1/ota/zigbee/unlock\",  *HTTP_POST' ../webui/rest_api.c"
check "/status exposes ncp_signing_unlocked"  "grep -q 'ncp_signing_unlocked' ../webui/rest_api.c"
check "frontend: unlock button + gesture text" "grep -q 'ota/zigbee/unlock' ../../frontend/js/api.js && grep -q '_unlockNcp' ../../frontend/js/pages/ota.js && grep -qi 'hold the button' ../../frontend/js/pages/ota.js"
internal "manual §2 explains the 2-10 s confirmation" "grep -q '确认解锁' ../../docs/guide/Hub使用手册.md"
check "test_api covers armed-but-unconfirmed still 400" "grep -q '只发起、没按键' ../../tools/test_api.sh"

echo ""
echo "==== T1.2 步骤 4：发布侧（release 包已签名，发版前验签）===="
check "release .gbl signed with the committed key"   "python3 ../../tools/gbl_verify.py -q ../../mg24/release/zigbee_ncp_uart_hw.gbl"
check "release SHA256SUMS consistent"                "(cd ../../mg24/release && sha256sum -c --quiet SHA256SUMS)"
check "make_manifest refuses an unsigned/foreign NCP" "grep -q 'gbl_verify.py' ../../tools/make_manifest.py"
check "ota_mg24.sh pre-checks the signature"         "grep -q 'gbl_verify.py' ../../mg24/ota_mg24.sh"
check "ota_mg24.ps1 warns when no signature tag"     "grep -q 'hasSig' ../../mg24/ota_mg24.ps1"
internal "ops manual states the publish ordering"       "grep -q '发版顺序' ../../docs/guide/云端OTA运维手册.md"

echo ""
echo "==== 刷写会话的执行上下文（2026-09-14：云端装 NCP 概率性 commit_failed）===="
# 云端那条路自己就占着一块内部栈，HTTPS 下载又把内部堆打碎，再要 4 KB 内部栈
# 建任务是掷硬币（2026-09-14 实测栽过一次）。所以云端内联跑、网页仍建任务。
# 栈的当前值与实测数据在 ota_cloud.c 的 NCP_TASK_STACK（T2.25 已按水位收窄）。
check "会话体与任务壳分开"              "grep -q 'static void ota_run_session(void)' ota_mg24.c && grep -q 'ota_run_session();' ota_mg24.c"
# ★ 最危险的退化：会话体里留着 vTaskDelete —— 内联执行会把**调用方**（云端任务）删掉，
#   它的下载缓冲永远不释放、s_ncp_task_running 永远为真，之后再也装不了 NCP。
check "★ vTaskDelete 只在任务壳里（会话体内绝不能有）" "[ \$(python3 ../../tools/grep_code.py -q 'vTaskDelete' ota_mg24.c; grep -c 'vTaskDelete' ota_mg24.c) -eq 1 ] && python3 ../../tools/grep_code.py --absent -q 'vTaskDelete' <(awk '/^static void ota_run_session/{f=1} f{print} f&&/^}/{exit}' ota_mg24.c)"
check "内联分支跑会话体、不建任务"      "before_in_fn 'commit_staged' 'if \\(run_inline\\)' 'xTaskCreate'"
check "内联分支借调用方的栈时对齐优先级并还原" "[ \$(awk '/^static esp_err_t commit_staged/{f=1} f&&/vTaskPrioritySet/{n++} f&&/^}/{exit} END{print n+0}' ota_mg24.c) -eq 2 ]"
check "两个入口都在（网页建任务 / 云端内联）" "grep -q 'esp_err_t ota_mg24_commit(void)' ota_mg24.c && grep -q 'esp_err_t ota_mg24_commit_blocking(void)' ota_mg24.c"
check "云端用 blocking 版"              "python3 ../../tools/grep_code.py -q 'ota_mg24_commit_blocking\\(\\)' ../ota_cloud/ota_cloud.c"
check "网页仍用建任务那版（httpd 不能阻塞 40 s）" "python3 ../../tools/grep_code.py -q 'ota_mg24_commit\\(\\)' ../webui/rest_api.c && python3 ../../tools/grep_code.py --absent -q 'ota_mg24_commit_blocking' ../webui/rest_api.c"
# 阻塞版返回只说明"这一场跑完了"，成败在状态机里；只看返回值会把 XMODEM 失败报成成功
check "云端在 blocking 返回后查状态机"  "python3 ../../tools/grep_code.py -q 'st.state != MG24_OTA_SUCCESS' ../ota_cloud/ota_cloud.c"
check "云端在提交前就还掉下载缓冲"      "before_in_cloud 'free\\(buf\\); buf = NULL;' 'ota_mg24_commit_blocking'"

echo ""
echo "--- T9.25 NCP 防降级（2026-10-08）---"
GC="python3 ../../tools/grep_code.py"
# 顺序：验签通过之后、gbl_sha256 / 剥签名之前（剥签名会改 s_image；先拒再动镜像）
ln() { awk '/^static esp_err_t commit_staged\(/,/^}/' ota_mg24.c | $GC "$1" /dev/stdin | head -1 | cut -d: -f2; }
check "防降级在验签之后、算指纹之前" \
  "[ -n \"\$(ln 'gbl_ncp_guard\\(')\" ] && [ \$(ln 'gbl_sig_verify\\(') -lt \$(ln 'gbl_ncp_guard\\(') ] && [ \$(ln 'gbl_ncp_guard\\(') -lt \$(ln 'gbl_sha256\\(') ]"
check "只有网页上传能被开发者解锁放行（云端永远不放）" \
  "awk '/^static esp_err_t commit_staged\(/,/^}/' ota_mg24.c | grep -q 'bypass = (s_source == OTA_MG24_SOURCE_WEB) && ncp_unlock_active'"
check "底线只在刷写成功后抬高" \
  "$GC -q 'if \(err == ESP_OK && s_image_nvm3c > 0\) \(void\)mg24_image_record_note_nvm3\(s_image_nvm3c\);' ota_mg24.c && [ \$($GC 'mg24_image_record_note_nvm3\\(' ota_mg24.c | wc -l) -eq 1 ]"
# T9.19 ⑨：指纹只在新固件答话后记；没答话时按线路电平分"在不停重启"（失败）与"未确认"
check "指纹只在新固件答话后记" \
  "[ \$($GC 'mg24_image_record_save_digest\\(' ota_mg24.c | wc -l) -eq 1 ] && grep -A1 'case NCP_CONFIRMED:' ota_mg24.c | grep -q 'mg24_image_record_save_digest(s_upload_sha, s_upload_len);'"
check "刷完读不回版本时看线路判重启" \
  "awk '/^static ncp_confirm_t confirm_after_flash/,/^}/' ota_mg24.c | $GC -q 'cts_transitions >= 2' /dev/stdin"
check "未确认有独立状态且对外可见" \
  "grep -q 'MG24_OTA_UNCONFIRMED' include/ota_mg24.h && grep -q '\"unconfirmed\"' ../webui/rest_api.c && grep -q 'MG24_OTA_UNCONFIRMED' ../ota_cloud/ota_cloud.c"
check "底线只升不降、默认出厂 400" \
  "grep -q 'define MG24_NVM3C_FLOOR_DEFAULT 400' include/mg24_image_record.h && awk '/^esp_err_t mg24_image_record_note_nvm3\(/,/^}/' mg24_image_record.c | grep -q 'if (nvm3c <= mg24_image_record_nvm3_floor()) return ESP_OK;'"
check "底线所在的 mg24 命名空间不随恢复出厂擦（它描述的是芯片，不是用户设置）" \
  "sed -n '/OWNED_NS\[\] = {/,/^};/p' ../config_mgr/config_mgr.c | $GC --absent --strings -q '\"mg24\"' /dev/stdin && grep -q 'define NVS_NAMESPACE   \"mg24\"' mg24_image_record.c"
check "降级回 409（文件本身没问题，是和这块芯片冲突）" \
  "grep -q 'strcmp(code, \"ncp_image_downgrade\") == 0 ? 409 : 400' ../webui/rest_api.c"
check "判据与产线脚本同一套：没有构建串 / 没有 nvm3c= 都按 200 算" \
  "grep -q 'define GBL_NVM3C_LEGACY   200' include/gbl_image.h && grep -q 'MIN_NVM3_CACHE = 400' ../../mg24/ncp_image_check.py"

echo ""
echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
