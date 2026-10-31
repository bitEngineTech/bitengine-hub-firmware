#!/bin/bash
# components/ota_cloud/VERIFY.sh — 设计约束的机器判据
# ⚠️ 必须 cd 进本目录跑：bash VERIFY.sh（脚本用相对路径引用 ../../）
#
# 这里检查的是**代码形状**，不是行为。行为断言在 tools/test_api.sh，
# 版本比较的真值表在 test_host/run.sh。三者缺一不可 —— 见 CLAUDE.md 7.5。

PASS=0; FAIL=0
check() {
    if eval "$2" > /dev/null 2>&1; then echo "✅ $1"; PASS=$((PASS+1))
    else echo "❌ $1"; FAIL=$((FAIL+1)); fi
}

# ---- API 完整性：头文件声明的每个函数都要有定义 ----
# 在**两个** .c 里找：version_cmp 刻意抽在 version_cmp.c（好让 test_host 原样编它）
for f in ota_cloud_init ota_cloud_check ota_cloud_install ota_cloud_get_state \
         ota_cloud_get_latest ota_cloud_progress ota_cloud_busy \
         ota_cloud_last_error ota_cloud_version_cmp; do
    check "$f 有定义" "grep -qE '^[a-z_]+ .*$f\(|^const .*$f\(|^int $f\(|^bool $f\(' ota_cloud.c version_cmp.c"
done

# version_cmp.c 必须保持无 ESP-IDF 依赖，否则 test_host/run.sh 编不过 ——
# 而那 23 条真值表是防降级判据的唯一自动化证明
check "version_cmp.c 不依赖 ESP-IDF" "! grep -qE '#include \"esp_|#include \"freertos' version_cmp.c"

# ---- 状态机：7 个状态都要被真的写进去过 ----
for s in IDLE CHECKING UP_TO_DATE UPDATE_AVAILABLE DOWNLOADING SUCCESS FAILED; do
    check "OTA_CLOUD_$s 被赋值过" "grep -q 'OTA_CLOUD_$s' ota_cloud.c"
done

# ---- S1 安全：manifest 是不可信输入 ----
# URL 必须强制 https —— 否则被攻破的 manifest 能把下载降级到明文 http
check "强制 https:// 前缀"        "grep -q 'https://' ota_cloud.c && grep -qi 'strncmp.*https\|url_is_https' ota_cloud.c"
# 防降级：必须严格新于当前版本才允许装。签名防不住"推旧的合法固件"
check "拒绝非更新版本(防降级)"     "grep -q 'version_cmp' ota_cloud.c && grep -qiE 'downgrade|<= 0|not_newer' ota_cloud.c"
# 所有从 manifest 拷进来的字符串必须定长
check "manifest 字符串定长拷贝"    "! grep -qE '\bstrcpy\(|\bstrcat\(|\bsprintf\(' ota_cloud.c"
# 不能把 manifest 的 URL 打进日志的同时打出别的敏感项；至少不许打配置里的密码
check "日志不打密码/token"        "! grep -qE 'ESP_LOG.*password|ESP_LOG.*token|ESP_LOG.*secret' ota_cloud.c"

# ---- S2 线程：esp_https_ota 是阻塞的，必须在自己的任务里 ----
check "install 走后台任务"        "grep -q 'xTaskCreate' ota_cloud.c"
check "esp_https_ota 不在 httpd 任务里" "! grep -B30 'esp_https_ota(' ota_cloud.c | grep -q 'ota_cloud_install(void)'"
check "状态有互斥保护"            "grep -qE 'xSemaphoreTake|portENTER_CRITICAL|Mutex' ota_cloud.c"

# ---- S3 资源：每个错误路径都要复位状态，否则永久卡在 DOWNLOADING ----
check "失败路径置 FAILED"          "[ $(grep -c 'OTA_CLOUD_FAILED' ota_cloud.c) -ge 3 ]"
check "任务自删"                   "grep -q 'vTaskDelete(NULL)' ota_cloud.c"
check "http client 有 cleanup"     "[ $(grep -c 'esp_http_client_init' ota_cloud.c) -le $(grep -c 'esp_http_client_cleanup' ota_cloud.c) ]"
check "cJSON 有 Delete"            "[ $(grep -c 'cJSON_Parse' ota_cloud.c) -le $(grep -c 'cJSON_Delete' ota_cloud.c) ]"

# ---- 与既有 OTA 路径互斥（A5 的教训：两路同时写 flash）----
check "其余 OTA 入口检查 ota_cloud_busy" \
      "[ $(grep -c 'ota_cloud_busy()' ../webui/rest_api.c) -ge 3 ]"

# ---- 反向不变量：本组件**不该**自己做验签 ----
# esp_https_ota 内部已经验（esp_ota_end）。这里自己再写一套是重复且容易写错的，
# 而"没看到验签代码"很容易让下一个人误以为漏了。用注释钉住意图。
check "不自行实现验签"             "! grep -qE 'esp_secure_boot_verify|verify_signature_block' ota_cloud.c"
check "注释说明验签在何处发生"      "grep -q 'esp_ota_end' include/ota_cloud.h"

# ---- Kconfig：URL 必须可配，且不能带着占位符出货 ----
check "manifest URL 是 Kconfig 项"  "grep -q 'CONFIG_BITENGINE_OTA_CLOUD_URL' ota_cloud.c"
check "Kconfig 里有该项定义"        "grep -q 'BITENGINE_OTA_CLOUD_URL' ../../main/Kconfig.projbuild"
check "manifest 默认 URL 已定为出货域名" "grep -A2 'config BITENGINE_OTA_CLOUD_URL' ../../main/Kconfig.projbuild | grep default | grep -q 'updates.bitengine.tech/hub/manifest.json'"
check "sdkconfig.defaults 与 Kconfig 默认一致" "grep -q 'CONFIG_BITENGINE_OTA_CLOUD_URL=\"https://updates.bitengine.tech/hub/manifest.json\"' ../../sdkconfig.defaults"
check "占位域名不再出现在构建配置里"  "! grep -q 'updates.example.com' ../../sdkconfig ../../sdkconfig.defaults ../../main/Kconfig.projbuild"
check "make_manifest.py 存在且先验签"   "grep -q 'signing_key.sh' ../../tools/make_manifest.py"
check "占位域名有编译期告警"        "grep -q 'example.com' ota_cloud.c"

# ---- app 与前端成对（2026-09-10）：形状不变量 ----
check "解析 manifest 的 www 项"            "grep -q '\"www\"' ota_cloud.c"
check "附件 URL 也强制 https"              "grep -q '_url_not_https' ota_cloud.c && grep -q 'url_is_https(ju->valuestring)' ota_cloud.c"
check "附件的 sha256 形状先验(64 hex)"      "grep -q 'is_hex64' ota_cloud.c && grep -q '_sha256_invalid' ota_cloud.c"
check "附件下载后校 sha256(无签名的替代)"   "grep -q 'MBEDTLS_MD_SHA256' ota_cloud.c && grep -q 'sha256_mismatch' ota_cloud.c"
check "www 写入前过形状校验"                "grep -q 'www_image_validate' ota_cloud.c"
check "manifest 的 www 大小须等于分区"      "grep -q 'www_size_mismatch' ota_cloud.c && grep -q 'www_image_partition_size' ota_cloud.c"
check "先下 www 再装 app(失败不留半套)"      "[ \$(grep -n 'fetch_blob(s_latest.www_url' ota_cloud.c | head -1 | cut -d: -f1) -lt \$(grep -n 'esp_https_ota_begin' ota_cloud.c | head -1 | cut -d: -f1) ]"
check "www 写失败时把启动分区拨回"           "grep -q 'esp_ota_set_boot_partition' ota_cloud.c && grep -q 'www_write_failed' ota_cloud.c"
check "www 缓冲明确放 PSRAM 且有 free"       "grep -q 'MALLOC_CAP_SPIRAM' ota_cloud.c && grep -q 'free(www_buf)' ota_cloud.c"
check "app 比当前旧时整份拒绝(前端不单独换)" "grep -q 'cmp >= 0' ota_cloud.c"
check "webui 把运行中前端版本喂给本组件"     "grep -q 'ota_cloud_set_running_www_version' ../webui/static_files.c"
check "状态回包带 app_update/www_update/phase" "grep -q 'www_update' ../webui/rest_api.c && grep -q 'ota_cloud_phase' ../webui/rest_api.c"
check "make_manifest.py 会带上 www"          "grep -q '\"www\"' ../../tools/make_manifest.py"

# ---- NCP 走云端（2026-09-10）：单独装，不与 app 捆绑 ----
check "解析 manifest 的 ncp 项"            "grep -q '\"ncp\"' ota_cloud.c"
check "ncp 大小限定在 MG24_OTA 范围"        "grep -q 'MG24_OTA_MAX_SIZE' ota_cloud.c && grep -q 'MG24_OTA_MIN_SIZE' ota_cloud.c"
check "ncp 走 ota_mg24 begin/write/commit"  "grep -q 'ota_mg24_begin' ota_cloud.c && grep -q 'ota_mg24_write' ota_cloud.c && grep -q 'ota_mg24_commit' ota_cloud.c"
check "ncp 失败路径 abort 暂存区"           "[ \$(grep -c 'ota_mg24_abort()' ota_cloud.c) -ge 2 ]"
check "ncp 下载有独立任务且与 app 任务互斥"  "grep -q 'ncp_task' ota_cloud.c && grep -q '!s_task_running && !s_ncp_task_running' ota_cloud.c"
check "busy 覆盖 ncp 下载态"                "grep -A4 'bool ota_cloud_busy' ota_cloud.c | grep -q 'NCP_DOWNLOADING'"
check "ncp 指纹比对用 mg24_image_record"     "grep -q 'mg24_image_record_get' ota_cloud.c && grep -q 'ncp_version_format' ota_cloud.c"
check "ncp 交出去后不永远 flashing"          "grep -A12 'ota_cloud_ncp_state_t ota_cloud_ncp_state(void)' ota_cloud.c | grep -q 'ota_mg24_busy'"
check "REST 注册 /ota/cloud/ncp"            "grep -q '/api/v1/ota/cloud/ncp' ../webui/rest_api.c"
check "状态回包带 ncp_update/ncp_state"      "grep -q 'ncp_update' ../webui/rest_api.c && grep -q 'ncp_state' ../webui/rest_api.c"
check "make_manifest.py 会带上 ncp"          "grep -q '\"ncp\"' ../../tools/make_manifest.py"
check "mg24/release 有 ncp_version.txt"     "[ -s ../../mg24/release/ncp_version.txt ]"

# ---- ota_available webhook（2026-09-13，B9/C1 遗留：事件定义了、开关也在，此前零 post）----
check "发现新版本时 post ota_available"      "awk '/^esp_err_t ota_cloud_check/,/^}/' ota_cloud.c | grep -q 'webhook_post_event(WEBHOOK_EVENT_OTA_AVAILABLE'"
check "同一版本只提醒一次（去重）"           "grep -q 's_notified_version' ota_cloud.c && awk '/^esp_err_t ota_cloud_check/,/^}/' ota_cloud.c | grep -q 'strlcpy(s_notified_version'"
check "只按 app/前端触发，不按 ncp_differs"   "awk '/^esp_err_t ota_cloud_check/,/^}/' ota_cloud.c | grep -B3 'webhook_post_event(WEBHOOK_EVENT_OTA_AVAILABLE' | grep -qv 'ncp_differs &&'"
check "CMake 依赖 webhook"                   "grep -q 'webhook' CMakeLists.txt"

echo ""; echo ""
echo "--- notes 按字符边界截断（2026-09-15 线上事故）---"
# 一条 164 字节的中文 notes 被 strlcpy 砍在第 159 字节，切坏一个 3 字节字符，
# /api/v1/ota/check 的回包不再是合法 UTF-8。设备侧自保 + 发布侧闸门，两头都要有。
check "notes 不用裸 strlcpy"          "! python3 ../../tools/grep_code.py -q 'strlcpy\\(out->notes' ota_cloud.c"
check "notes 走 utf8_safe_copy"        "python3 ../../tools/grep_code.py -q 'utf8_safe_copy\\(out->notes' ota_cloud.c"
check "截断函数单独成文件"             "[ -f utf8_trunc.c ]"
check "已进 CMake 源列表"              "grep -q 'utf8_trunc.c' CMakeLists.txt"
check "有主机侧测试且已接线"           "[ -f test_host/utf8_test.c ] && grep -q 'utf8_test' test_host/run.sh"
check "测试编的是产品源文件不是副本"   "grep -q 'utf8_test.c ../utf8_trunc.c' test_host/run.sh"
# 病根是两个数：发布侧写死 200、设备侧 notes[160]。上限现在从头文件读。
check "发布闸门从 ota_cloud.h 读上限"  "grep -q 'ota_cloud.h' ../../tools/make_manifest.py"
check "发布闸门不再写死 200"           "! python3 ../../tools/grep_code.py -q 'encode\\(\\)\\) > 200' ../../tools/make_manifest.py"

# ---- 防串刷（docs/15 设计 2.7、架构 5.8 R-9；2026-10-21）----
# 两种固件同一把签名钥匙，验签挡不住"把直连型装进 HA 型设备"；挡它的是写 flash 之前比对镜像的工程名。
# 解析与比对抽在 ota_profile.c（同 version_cmp.c：不依赖 IDF，test_host 原样编它）。
check "ota_profile.c 不依赖 ESP-IDF（test_host 原样编它）" "[ -f ota_profile.c ] && ! grep -qE '#include \"esp_|#include \"freertos' ota_profile.c"
check "ota_profile.c 已进 CMake 源列表"       "grep -q 'ota_profile.c' CMakeLists.txt"
check "有主机侧测试、编的是产品源文件"         "[ -f test_host/profile_test.c ] && grep -q 'profile_test.c ../ota_profile.c' test_host/run.sh"
check "镜像格式常量有 IDF 静态断言兜底"       "python3 ../../tools/grep_code.py -q '_Static_assert\\(offsetof\\(esp_app_desc_t, project_name\\) == OTA_PROFILE_NAME_OFFSET' ota_cloud.c"
check "R-9：两条写入路径都在写之前比对工程名" "python3 ../../tools/check_profile_rules.py --only r9"

echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1

