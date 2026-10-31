/* ota_cloud — 云端更新（查更新 + 用户点一下装；app 与前端成对，NCP 单独）
 *
 * 设计取舍见 include/ota_cloud.h 顶部注释。这里只记实现层面的几条：
 *
 * 1. **本文件没有一行 app 验签代码，这是正确的。** 验签发生在 esp_https_ota_finish()
 *    内部调用的 esp_ota_end() 里（esp_https_ota.c:924）。自己再实现一套是重复的，
 *    而且极容易写成"看起来在验、其实恒真"。VERIFY.sh 有一条反向不变量钉住这点。
 *    前端镜像（www.bin）没有签名机制（审计 A19），靠 HTTPS + manifest 里的 sha256
 *    保证完整性 —— 这是明确知道的差距，不是遗漏。NCP 固件（.gbl）自 T1.2 起在交给
 *    ota_mg24 之后由 C5 验签，云端来源必须带签名（ota_mg24.c 的 signature_required()）。
 *
 * 2. **manifest 是不可信输入。** HTTPS 只证明"连对了服务器"，不证明服务器没被
 *    攻破。所以 URL 强制 https、字符串定长、版本必须严格更新（防降级 ——
 *    签名拦不住"推一个旧的但签名合法的固件"，那个洞得靠版本比较堵）。
 *    www / ncp 项同理：url 必须 https、size 有硬上限、sha256 必须是 64 位十六进制。
 *
 * 3. **锁只护状态，不护网络 IO。** 拉 manifest 要几百 ms 到几秒，握着锁做
 *    会让 get_state() 一起卡住，而前端正在轮询它。
 *
 * 4. **app 与前端成对，顺序是：先把前端下到 PSRAM，再装 app，最后写前端分区。**
 *    2026-09-10 加。此前云端只更新 app，而 app 与前端之间没有任何兼容性检查：
 *    推一个改了 REST 字段的固件出去，用户设备上 app 新、前端旧，页面静默出错，
 *    他也没有渠道知道该更新前端。先下前端再装 app，是为了让任何一步失败都不留下
 *    "半套"；前端分区写失败时把启动分区拨回当前 app，整次升级作废，用户重试即可。
 *
 * 5. **NCP（MG24）固件单独装，不与 app 捆绑。** 2026-09-10 加。刷 NCP 要断 Zigbee 约
 *    15-30 秒、Z2M/ZHA 会掉线重连，时机得用户自己挑；NCP 刷失败也要能与 app 更新隔离。
 *    本文件只负责把 .gbl 下到 PSRAM、核 sha256，然后交给 ota_mg24（与 Web 上传同一条
 *    XMODEM 路径），进度看 /api/v1/ota/zigbee/status。"要不要更新"的判据次序
 *    （指纹 > XNCP 构建串 > 版本串）正本在 version_cmp.c 的 ota_cloud_ncp_differs()。
 */
#include "ota_cloud.h"
#include "utf8_trunc.h"
/* 版本比较单独成文件，是为了让 test_host/ 能原样编译它去测，
   而不是抄一份副本过去测 —— 副本测试只能证明副本是对的。 */
#include "version_cmp.h"
#include "ota_profile.h"
#include "www_image.h"
#include "ota_mg24.h"
#include "mg24_image_record.h"
#include "ncp_version.h"
#include "webhook.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"        /* ESP_ERR_OTA_VALIDATE_FAILED —— 验签失败的返回码；set_boot_partition 用于回拨 */
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "mbedtls/md.h"          /* IDF v6 的 mbedTLS 4.x 已把 mbedtls/sha256.h 收进 private，与 config_mgr 同一写法 */
#include "cJSON.h"
#include "esp_app_format.h"      /* esp_image_header_t / esp_image_segment_header_t：只为下面的静态断言 */
#include <stddef.h>
#include "sdkconfig.h"

/* 防串刷靠镜像里的工程名。这个选项一开，IDF 把 project_name 编成空串（esp_app_desc.c），之后每次 OTA 都会因
   profile_unknown 被拒 —— 一个拒绝一切 OTA 的固件，按红线 1 只能拆机。所以构建时就停。 */
#if CONFIG_APP_EXCLUDE_PROJECT_NAME_VAR
#error "CONFIG_APP_EXCLUDE_PROJECT_NAME_VAR 不能开：防串刷比对镜像的工程名（docs/15 设计 2.7），没有它所有 OTA 都会被拒"
#endif

static const char *TAG = "ota_cloud";

/* ---------------- 防串刷：镜像工程名比对（docs/15 设计 2.7、架构 5.8 R-9） ----------------
 * 两种固件（HA 型 bitengine / 直连型 bitengine-direct）同一把签名钥匙，验签挡不住串刷；串了按红线 1 只能拆机。
 * 解析与比对在 ota_profile.c（不依赖 IDF，test_host 测）；那边手写的镜像格式常量在这里与 IDF 的定义对齐，
 * IDF 哪天改了布局，构建当场失败，而不是守卫悄悄读错位置、开始放行或误拒。 */
_Static_assert(sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) == OTA_PROFILE_DESC_OFFSET,
               "app 描述在镜像里的偏移变了，改 ota_profile.h");
_Static_assert(offsetof(esp_app_desc_t, project_name) == OTA_PROFILE_NAME_OFFSET,
               "project_name 在 esp_app_desc_t 里的偏移变了，改 ota_profile.h");
_Static_assert(sizeof(((esp_app_desc_t *)0)->project_name) == OTA_PROFILE_NAME_LEN,
               "project_name 长度变了，改 ota_profile.h");
_Static_assert(ESP_APP_DESC_MAGIC_WORD == OTA_PROFILE_DESC_MAGIC, "app 描述 magic 变了，改 ota_profile.h");
_Static_assert(ESP_IMAGE_HEADER_MAGIC == OTA_PROFILE_IMAGE_MAGIC, "镜像头 magic 变了，改 ota_profile.h");

/* 网络来的名字打进日志前去掉控制字符（S1：别让镜像里的字节改写日志行） */
static void printable_name(char *dst, size_t dst_sz, const char *src, size_t src_len)
{
    size_t i = 0;
    for (; i < src_len && i + 1 < dst_sz && src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    dst[i] = '\0';
}

static const char *profile_verdict(ota_profile_result_t r, const char *incoming, size_t incoming_len)
{
    const char *why = ota_profile_error_str(r);
    if (why) {
        char in[OTA_PROFILE_NAME_LEN + 1];
        printable_name(in, sizeof(in), incoming ? incoming : "", incoming ? incoming_len : 0);
        ESP_LOGE(TAG, "拒装：镜像是\"%s\"，本机运行的是\"%.32s\"（%s）—— 两种固件同一把签名钥匙，签名挡不住串刷",
                 in, esp_app_get_description()->project_name, why);
    }
    return why;
}

const char *ota_cloud_profile_check_image(const void *img, size_t len)
{
    char name[OTA_PROFILE_NAME_LEN + 1];
    ota_profile_result_t r = ota_profile_name_from_image((const uint8_t *)img, len, name);
    if (r == OTA_PROFILE_OK) {
        r = ota_profile_match(esp_app_get_description()->project_name, name, sizeof(name) - 1);
    }
    return profile_verdict(r, name, sizeof(name) - 1);
}

#ifndef CONFIG_BITENGINE_OTA_CLOUD_URL
#define CONFIG_BITENGINE_OTA_CLOUD_URL ""
#endif
#ifndef CONFIG_BITENGINE_OTA_CLOUD_TIMEOUT_MS
#define CONFIG_BITENGINE_OTA_CLOUD_TIMEOUT_MS 6000
#endif

/* manifest 体积上限。正常几百字节；给到 2KB 已经很宽裕，
   超了直接拒收而不是扩缓冲 —— 这是不可信输入。 */
#define MANIFEST_MAX_LEN   2048

/* 两个安装任务的栈，单位字节，**必须是内部 SRAM**（理由见 install_task 前的长注释）。
   内部 SRAM 是全系统最紧的资源，这两个数字直接决定装机那十几秒的谷底深度，
   所以提成常量：收窄时只改这里，任务结束时打的水位日志也拿它做分母。
   ⚠️ 改这两个数字前先看一次实测水位（ESP_LOGW "栈水位"），别拍脑袋。

   NCP_TASK_STACK 6144 是**实测后**定的（T2.25，2026-09-14 真板）：8192 那版跑完一次
   云端装 NCP，水位日志是「最小剩余 4980，峰值用掉 3212」，近 5 KB 白占。峰值由两段
   互不重叠的深度决定 —— fetch_blob 的 TLS 握手，和 commit_blocking 在本任务栈上跑的
   ota_run_session()（后者上限已知 ≤ MG24_OTA_TASK_STACK = 4096，因为非阻塞路径就用
   那么大的任务跑同一段代码；XMODEM 的 packet[133] 也在栈上，但不是大头）。
   6144 相对实测峰值留 91% 余量：不贴着 3212 切，是因为 TLS 握手深度会随证书链变化，
   而这条路一年只走几次、省下的 2 KB 却是常态谷底的一半。参照物：常驻任务里最紧的
   led_task 只留 1196 字节就在正常跑。万一真估低了，失败方向也是**可见**的 ——
   CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y，溢出会 panic 并重启，不是静默损坏。

   ⚠️ INSTALL_TASK_STACK 仍是 8192，**因为它没有实测数据** —— 那条路多一段
   esp_https_ota，深度未必与 NCP 相同。要收窄它，先装一次 app 拿到它自己的水位日志。 */
#define INSTALL_TASK_STACK 8192
#define NCP_TASK_STACK     6144

static SemaphoreHandle_t    s_lock;
static ota_cloud_state_t    s_state = OTA_CLOUD_IDLE;
static ota_cloud_manifest_t s_latest;
static ota_cloud_manifest_t s_scratch;      /* check() 解析用；结构体 ~1.2KB，不放 httpd 栈上 */
static char                 s_err[96];
static char                 s_running_www[40];   /* webui 挂好 SPIFFS 后填；空 = 未知 */
static const char          *s_phase = "";        /* "app" / "www" / "write" / ""，只指向静态字符串 */
static volatile int         s_progress = -1;
static volatile bool        s_task_running;      /* app/前端安装任务 */

static ota_cloud_ncp_state_t s_ncp_state = OTA_CLOUD_NCP_IDLE;
static char                  s_ncp_err[64];
static volatile bool         s_ncp_task_running;
static char                  s_why_buf[32];      /* fetch_blob 的 "http_%d" 之类；两个任务互斥，单缓冲够用 */

/* ---------------- 状态存取（唯一允许碰上面那几个变量的地方）---------------- */

static void set_state(ota_cloud_state_t st, const char *err)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = st;
    if (err) {
        strlcpy(s_err, err, sizeof(s_err));
    } else if (st != OTA_CLOUD_FAILED) {
        s_err[0] = '\0';
    }
    xSemaphoreGive(s_lock);
}

static void set_ncp_state(ota_cloud_ncp_state_t st, const char *err)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ncp_state = st;
    if (err) {
        strlcpy(s_ncp_err, err, sizeof(s_ncp_err));
    } else if (st != OTA_CLOUD_NCP_FAILED) {
        s_ncp_err[0] = '\0';
    }
    xSemaphoreGive(s_lock);
}

ota_cloud_state_t ota_cloud_get_state(void)
{
    if (!s_lock) return OTA_CLOUD_IDLE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ota_cloud_state_t st = s_state;
    xSemaphoreGive(s_lock);
    return st;
}

ota_cloud_ncp_state_t ota_cloud_ncp_state(void)
{
    if (!s_lock) return OTA_CLOUD_NCP_IDLE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ota_cloud_ncp_state_t st = s_ncp_state;
    xSemaphoreGive(s_lock);
    /* 交给 ota_mg24 之后，结果（success/failed）在 /api/v1/ota/zigbee/status 里；
       它跑完了本组件就回 IDLE，别让前端看到一个永远的 "flashing"。 */
    if (st == OTA_CLOUD_NCP_FLASHING && !ota_mg24_busy()) return OTA_CLOUD_NCP_IDLE;
    return st;
}

bool ota_cloud_busy(void)
{
    ota_cloud_state_t st = ota_cloud_get_state();
    if (st == OTA_CLOUD_CHECKING || st == OTA_CLOUD_DOWNLOADING) return true;
    return ota_cloud_ncp_state() == OTA_CLOUD_NCP_DOWNLOADING;
}

const char *ota_cloud_last_error(void)
{
    return s_err;   /* 定长静态缓冲，永不为 NULL；竞争最坏是读到半新半旧的字符串 */
}

const char *ota_cloud_ncp_last_error(void)
{
    return s_ncp_err;
}

int ota_cloud_progress(void)
{
    return s_progress;
}

const char *ota_cloud_phase(void)
{
    return s_phase;
}

const ota_cloud_manifest_t *ota_cloud_get_latest(void)
{
    return &s_latest;
}

void ota_cloud_set_running_www_version(const char *v)
{
    if (!v) { s_running_www[0] = '\0'; return; }
    strlcpy(s_running_www, v, sizeof(s_running_www));
}

/* ---------------- manifest 拉取与解析 ---------------- */

static bool url_is_https(const char *u)
{
    /* 必须是 https。被攻破的 manifest 若能把下载地址降级成 http，
       中间人就能替换固件 —— 虽然签名还会拦住它，但没有理由让请求发出去。 */
    return u && strncmp(u, "https://", 8) == 0;
}

static bool is_hex64(const char *s)
{
    if (!s) return false;
    for (int i = 0; i < 64; i++) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    return s[64] == '\0';
}

static esp_err_t fetch_manifest(char *buf, size_t bufsz)
{
    const char *url = CONFIG_BITENGINE_OTA_CLOUD_URL;
    if (!url || url[0] == '\0') {
        set_state(OTA_CLOUD_FAILED, "manifest_url_not_configured");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!url_is_https(url)) {
        set_state(OTA_CLOUD_FAILED, "manifest_url_not_https");
        return ESP_ERR_INVALID_ARG;
    }

    /* ⚠️ 这里**故意不设** .if_name（T3.8 的出口绑定只做了 webhook 与 MQTT）：
       manifest 服务器在公网，属于跨网段，本来就按默认路由走，绑不绑一样；
       而绑死了反而会挡掉"活动接口没有外网、另一个口有"时的回退。
       出口绑定只对**本网段**目的地有意义 —— 那正是 webhook / MQTT 的常见场景。 */
    esp_http_client_config_t cfg = {
        .url               = url,
        .timeout_ms        = CONFIG_BITENGINE_OTA_CLOUD_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* 与 webhook 同一套根证书 */
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        set_state(OTA_CLOUD_FAILED, "http_client_init_failed");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_FAIL;
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        set_state(OTA_CLOUD_FAILED, "connect_failed");
        ret = err;
        goto out;
    }

    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        char m[48];
        snprintf(m, sizeof(m), "manifest_http_%d", status);
        set_state(OTA_CLOUD_FAILED, m);
        ret = ESP_FAIL;
        goto out;
    }

    int n = esp_http_client_read_response(c, buf, (int)bufsz - 1);
    if (n < 0) {
        set_state(OTA_CLOUD_FAILED, "manifest_read_failed");
        ret = ESP_FAIL;
        goto out;
    }
    /* 读满缓冲 = 要么真超长要么被截断，两种都不能当合法 manifest 用 */
    if (n >= (int)bufsz - 1) {
        set_state(OTA_CLOUD_FAILED, "manifest_too_large");
        ret = ESP_ERR_INVALID_SIZE;
        goto out;
    }
    buf[n] = '\0';
    ret = ESP_OK;

out:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ret;
}

/* 解析 www / ncp 这类"附件"对象：四项必须完整合法。半个对象直接拒掉整份 manifest
   （fail closed），而不是静默当作没有：那会让"忘了填 sha256"表现成"永远不更新"。
   失败时 *why 得到 "<tag>_missing_version" 之类。 */
static bool parse_attachment(const cJSON *obj, const char *tag,
                             char *ver, size_t ver_sz, char *url, size_t url_sz,
                             char *sha, size_t sha_sz, uint32_t *size,
                             double max_size, char *why, size_t why_sz)
{
    const cJSON *jv = cJSON_GetObjectItem(obj, "version");
    const cJSON *ju = cJSON_GetObjectItem(obj, "url");
    const cJSON *js = cJSON_GetObjectItem(obj, "size");
    const cJSON *jh = cJSON_GetObjectItem(obj, "sha256");
    if (!cJSON_IsObject(obj) || !cJSON_IsString(jv) || !jv->valuestring[0]) {
        snprintf(why, why_sz, "%s_missing_version", tag); return false;
    }
    if (!cJSON_IsString(ju) || !url_is_https(ju->valuestring)) {
        snprintf(why, why_sz, "%s_url_not_https", tag); return false;
    }
    if (!cJSON_IsString(jh) || !is_hex64(jh->valuestring)) {
        snprintf(why, why_sz, "%s_sha256_invalid", tag); return false;
    }
    if (!cJSON_IsNumber(js) || js->valuedouble <= 0 || js->valuedouble > max_size) {
        snprintf(why, why_sz, "%s_size_invalid", tag); return false;
    }
    strlcpy(ver, jv->valuestring, ver_sz);
    strlcpy(url, ju->valuestring, url_sz);
    strlcpy(sha, jh->valuestring, sha_sz);
    *size = (uint32_t)js->valuedouble;
    return true;
}

/* 解析 JSON 到 out。失败时 out->valid 保持 false。 */
static esp_err_t parse_manifest(const char *json, ota_cloud_manifest_t *out)
{
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        set_state(OTA_CLOUD_FAILED, "manifest_not_json");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_ERR_INVALID_ARG;
    char why[48];
    const cJSON *jv = cJSON_GetObjectItem(root, "version");
    const cJSON *ju = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(jv) || !jv->valuestring[0]) {
        set_state(OTA_CLOUD_FAILED, "manifest_missing_version");
        goto out;
    }
    if (!cJSON_IsString(ju) || !url_is_https(ju->valuestring)) {
        set_state(OTA_CLOUD_FAILED, "firmware_url_not_https");
        goto out;
    }

    strlcpy(out->version, jv->valuestring, sizeof(out->version));
    strlcpy(out->url,     ju->valuestring, sizeof(out->url));

    const cJSON *jn = cJSON_GetObjectItem(root, "notes");
    /* notes 是唯一的**自由文本**字段（会有中文），所以只有它需要按字符边界截断。
       version / url 是 ASCII 形状，过长本来就该判错而不是截断。 */
    if (cJSON_IsString(jn)) utf8_safe_copy(out->notes, sizeof(out->notes), jn->valuestring);

    const cJSON *js = cJSON_GetObjectItem(root, "size");
    if (cJSON_IsNumber(js) && js->valuedouble > 0 && js->valuedouble < 16.0e6) {
        out->size = (uint32_t)js->valuedouble;
    }

    const cJSON *jw = cJSON_GetObjectItem(root, "www");
    if (jw) {
        if (!parse_attachment(jw, "www", out->www_version, sizeof(out->www_version),
                              out->www_url, sizeof(out->www_url),
                              out->www_sha256, sizeof(out->www_sha256),
                              &out->www_size, 4.0e6, why, sizeof(why))) {
            set_state(OTA_CLOUD_FAILED, why);
            goto out;
        }
        out->has_www = true;
    }

    const cJSON *jz = cJSON_GetObjectItem(root, "ncp");
    if (jz) {
        if (!parse_attachment(jz, "ncp", out->ncp_version, sizeof(out->ncp_version),
                              out->ncp_url, sizeof(out->ncp_url),
                              out->ncp_sha256, sizeof(out->ncp_sha256),
                              &out->ncp_size, (double)MG24_OTA_MAX_SIZE, why, sizeof(why))) {
            set_state(OTA_CLOUD_FAILED, why);
            goto out;
        }
        if (out->ncp_size < MG24_OTA_MIN_SIZE) {
            set_state(OTA_CLOUD_FAILED, "ncp_size_invalid");
            goto out;
        }
        /* 可选：XNCP 构建串（T9.19 ⑦，tools/make_manifest.py 从 .gbl 里取）。老清单没有它 → 留空、退回比版本串。
           格式不对就当没有，不让整份清单作废：它只影响"提不提示"，不影响装不装。 */
        out->ncp_build[0] = '\0';
        const cJSON *jb = cJSON_GetObjectItem(jz, "build");
        if (cJSON_IsString(jb) && jb->valuestring && strlen(jb->valuestring) < sizeof(out->ncp_build)) {
            strlcpy(out->ncp_build, jb->valuestring, sizeof(out->ncp_build));
        }
        out->has_ncp = true;
    }

    out->valid = true;
    ret = ESP_OK;

out:
    cJSON_Delete(root);
    return ret;
}

/* NCP 要不要更新：判据次序（指纹 > 构建串 > 版本串 > 读不到就提示）的正本在 version_cmp.c 的
   ota_cloud_ncp_differs()，主机侧测着。构建串那一档是给产线 J-Link 首烧、没有指纹的整机的（T9.19 ⑦）：
   EmberZNet 版本串所有 build 都一样，以前这种整机云端永远不提示 NCP 更新。 */
static bool ncp_image_differs(const ota_cloud_manifest_t *m)
{
    if (!m->has_ncp) return false;
    const mg24_image_record_t *rec = mg24_image_record_get();
    char cur[40];
    ncp_version_format(cur, sizeof(cur));
    return ota_cloud_ncp_differs((rec && rec->valid) ? rec->sha256 : NULL,
                                 ncp_version_get()->build_string, cur,
                                 m->ncp_sha256, m->ncp_build, m->ncp_version);
}

/* check 与 install 共用的决定：app 装不装、前端换不换、NCP 提不提示。
   app 严格更新才装；前端只在 app 不比当前旧时跟随（相等 = 只换前端，热修前端的路径）。
   app 比当前旧 → app/前端什么都不装：前端是配着 manifest 那个 app 的，单独换过去就是错配。
   NCP 独立判断，与 app 版本无关。 */
static void decide(ota_cloud_manifest_t *m, const char *cur)
{
    int cmp = ota_cloud_version_cmp(m->version, cur);
    m->app_newer   = cmp > 0;
    m->www_differs = m->has_www && cmp >= 0 && strcmp(m->www_version, s_running_www) != 0;
    m->ncp_differs = ncp_image_differs(m);
}

/* ---------------- 对外：检查更新 ---------------- */

/* 同一版本每次开机只提醒一次。检查是用户手动点的（首发没有后台轮询），
   每点一次都发一条 webhook 等于把"提醒"变成"骚扰"；但换了版本就该再提醒。 */
static char s_notified_version[40];

esp_err_t ota_cloud_check(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (ota_cloud_busy()) return ESP_ERR_INVALID_STATE;

    set_state(OTA_CLOUD_CHECKING, NULL);

    char *buf = malloc(MANIFEST_MAX_LEN);
    if (!buf) {
        set_state(OTA_CLOUD_FAILED, "out_of_memory");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = fetch_manifest(buf, MANIFEST_MAX_LEN);
    if (err != ESP_OK) { free(buf); return err; }   /* fetch_manifest 已置 FAILED */

    ota_cloud_manifest_t *m = &s_scratch;
    err = parse_manifest(buf, m);
    free(buf);
    if (err != ESP_OK) return err;                  /* parse_manifest 已置 FAILED */

    const esp_app_desc_t *app = esp_app_get_description();
    const char *cur = app ? app->version : "";
    decide(m, cur);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(&s_latest, m, sizeof(s_latest));
    /* 新一轮检查：上一轮 NCP 的失败原因作废（正在刷的不动） */
    if (s_ncp_state == OTA_CLOUD_NCP_FAILED) { s_ncp_state = OTA_CLOUD_NCP_IDLE; s_ncp_err[0] = '\0'; }
    xSemaphoreGive(s_lock);

    if (m->app_newer || m->www_differs) {
        ESP_LOGI(TAG, "有更新：app %s（当前 %s）%s，前端 %s（当前 %s）%s",
                 m->version, cur, m->app_newer ? "要装" : "不动",
                 m->has_www ? m->www_version : "-", s_running_www[0] ? s_running_www : "?",
                 m->www_differs ? "要换" : "不动");
        set_state(OTA_CLOUD_UPDATE_AVAILABLE, NULL);
        /* ota_available webhook（B9/C1 遗留：事件名与开关早就在，从没有人发过）。
           口径：**手动检查发现新版本就算 available** —— 首发没有后台自动检查，这是
           它唯一的触发点。只按 app/前端触发，NCP 是否不同只放在 data 的 "ncp" 里
           （判据见 version_cmp.c 的 ota_cloud_ncp_differs()；没有指纹的板子比构建串，不是恒 true）。
           data 上限 128B（webhook_item_t.data），只放版本与三个布尔，别加 current。 */
        if (strcmp(s_notified_version, m->version) != 0) {
            char data[96];
            /* %.31s：version 字段的缓冲比 data 大得多，不截断的话 -Werror=format-truncation
               会拒绝编译；真实版本串（如 v0.5.0-271-g9789909-dirty）不到 31 字。 */
            snprintf(data, sizeof(data),
                     "{\"version\":\"%.31s\",\"app\":%s,\"www\":%s,\"ncp\":%s}",
                     m->version, m->app_newer ? "true" : "false",
                     m->www_differs ? "true" : "false", m->ncp_differs ? "true" : "false");
            webhook_post_event(WEBHOOK_EVENT_OTA_AVAILABLE, data);
            strlcpy(s_notified_version, m->version, sizeof(s_notified_version));
        }
    } else {
        ESP_LOGI(TAG, "已是最新（服务器 %s，当前 %s）", m->version, cur);
        set_state(OTA_CLOUD_UP_TO_DATE, NULL);
    }
    if (m->has_ncp) {
        ESP_LOGI(TAG, "NCP 固件 %s：%s", m->ncp_version, m->ncp_differs ? "与本机不同，可更新" : "与本机相同");
    }
    return ESP_OK;
}

/* ---------------- 下载附件：整张进 PSRAM 并核大小、sha256 ---------------- */

static bool sha256_hex_equal(const uint8_t d[32], const char *hex)
{
    for (int i = 0; i < 32; i++) {
        char h[3];
        snprintf(h, sizeof(h), "%02x", d[i]);
        if (tolower((unsigned char)hex[2 * i]) != h[0] ||
            tolower((unsigned char)hex[2 * i + 1]) != h[1]) return false;
    }
    return hex[64] == '\0';
}

/* 成功时 *out 归调用方 free，*why 为 NULL；失败时 *why 是不带前缀的短标识
   （connect_failed / http_404 / size_mismatch / download_failed / sha256_mismatch / out_of_memory），
   调用方自己加 "www_" / "ncp_" 前缀记进状态。 */
static esp_err_t fetch_blob(const char *url, size_t expect, const char *sha_hex,
                            uint8_t **out, const char **why)
{
    *out = NULL;
    *why = "download_failed";

    if (!url_is_https(url)) { *why = "url_not_https"; return ESP_ERR_INVALID_ARG; }
    if (expect == 0 || expect > (4u << 20)) { *why = "size_invalid"; return ESP_ERR_INVALID_SIZE; }

    /* 明确要 PSRAM：几百 KB 内部堆根本没有；写 flash / 交给 ota_mg24 时源在 PSRAM，
       与两条 Web 上传路径一致 */
    uint8_t *buf = heap_caps_malloc(expect, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { *why = "out_of_memory"; return ESP_ERR_NO_MEM; }

    esp_http_client_config_t cfg = {
        .url               = url,
        .timeout_ms        = CONFIG_BITENGINE_OTA_CLOUD_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { free(buf); *why = "http_client_init_failed"; return ESP_ERR_NO_MEM; }

    esp_err_t ret = ESP_FAIL;
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) { *why = "connect_failed"; ret = err; goto out; }

    int64_t cl = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        snprintf(s_why_buf, sizeof(s_why_buf), "http_%d", status);
        *why = s_why_buf;
        goto out;
    }
    if (cl > 0 && (size_t)cl != expect) {          /* 服务器上的文件与 manifest 不符 */
        *why = "size_mismatch"; ret = ESP_ERR_INVALID_SIZE; goto out;
    }

    size_t got = 0;
    while (got < expect) {
        int n = esp_http_client_read(c, (char *)buf + got, (int)(expect - got));
        if (n <= 0) break;                        /* 0 = 正文结束，<0 = 错误；都由下面的长度判 */
        got += (size_t)n;
        s_progress = (int)((int64_t)got * 100 / (int64_t)expect);
    }
    if (got != expect) { *why = "download_failed"; goto out; }
    {   /* chunked 传输没有 Content-Length：读满后再探一字节，还有就是文件比声明的大 */
        char extra;
        if (esp_http_client_read(c, &extra, 1) > 0) {
            *why = "size_mismatch"; ret = ESP_ERR_INVALID_SIZE; goto out;
        }
    }

    uint8_t digest[32];
    /* HW SHA 驱动接受 PSRAM 输入（port/sha/core/sha.c 对 external_ram 有专门路径），不用搬到内部 RAM */
    if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), buf, expect, digest) != 0 ||
        !sha256_hex_equal(digest, sha_hex)) {
        *why = "sha256_mismatch";
        goto out;
    }

    *out = buf;
    buf = NULL;                                   /* 所有权交给调用方 */
    *why = NULL;
    ret = ESP_OK;

out:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    free(buf);                                    /* 成功路径上已置 NULL */
    return ret;
}

/* ---------------- 对外：安装 app + 前端 ---------------- */

static void install_task(void *arg)
{
    (void)arg;
    esp_https_ota_handle_t h = NULL;
    uint8_t *www_buf = NULL;
    size_t   www_len = 0;
    bool     app_done = false;
    const char *why = NULL;
    char     e[48];
    esp_err_t err;
    s_progress = 0;

    /* 顺序：先前端进 PSRAM（失败什么都没碰），再装 app，最后写前端分区。见文件头第 4 条。 */
    if (s_latest.www_differs) {
        s_phase = "www";
        size_t part = www_image_partition_size();
        if (part == 0) { set_state(OTA_CLOUD_FAILED, "no_www_partition"); goto done; }
        /* manifest 声明的大小必须等于分区大小 —— 下载前就拒，省 512KB 流量，也挡住
           "为别的分区尺寸生成的镜像"这种错配 */
        if (s_latest.www_size != part) { set_state(OTA_CLOUD_FAILED, "www_size_mismatch"); goto done; }
        err = fetch_blob(s_latest.www_url, part, s_latest.www_sha256, &www_buf, &why);
        if (err != ESP_OK) {
            snprintf(e, sizeof(e), "www_%s", why ? why : "download_failed");
            set_state(OTA_CLOUD_FAILED, e);
            if (strstr(e, "sha256")) ESP_LOGE(TAG, "前端镜像 sha256 与 manifest 不符，拒绝写入");
            goto done;
        }
        if (www_image_validate(www_buf, part, &why) != ESP_OK) {
            set_state(OTA_CLOUD_FAILED, why ? why : "www_invalid");
            goto done;
        }
        www_len = part;
        s_progress = 0;
    }

    if (s_latest.app_newer) {
        s_phase = "app";
        esp_http_client_config_t http_cfg = {
            .url               = s_latest.url,
            .timeout_ms        = CONFIG_BITENGINE_OTA_CLOUD_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .keep_alive_enable = true,
        };
        /* 不设 buffer_caps：sdkconfig.defaults 的 SPIRAM_MALLOC_ALWAYSINTERNAL=1024
           已经把这个量级的分配路由到 PSRAM 了，显式指定 MALLOC_CAP_SPIRAM 反而要
           多考虑一层"flash 写入源在 PSRAM"的边界情况。内部 SRAM 只剩十几 KB，
           这条别改（见 sdkconfig.defaults 的内存分配策略一节）。 */
        esp_https_ota_config_t ota_cfg = {
            .http_config = &http_cfg,
        };

        err = esp_https_ota_begin(&ota_cfg, &h);
        if (err != ESP_OK || !h) {
            set_state(OTA_CLOUD_FAILED, "download_begin_failed");
            goto done;
        }

        /* 防串刷（R-9）：perform 写 flash 之前先读镜像描述、比对工程名。get_img_desc 只把头部读进
           OTA 缓冲，perform 接着用，不重复下载（esp_https_ota.c 的 get_description_from_image）。 */
        esp_app_desc_t new_desc;
        if (esp_https_ota_get_img_desc(h, &new_desc) != ESP_OK) {
            /* 头 1 KB 没读全也走这里（连接断了），所以按下载失败报，别把网络抖动说成"文件不是固件"。
               magic 之类的内容问题由下面的比对接住（读不出名字 = profile_unknown）。 */
            esp_https_ota_abort(h);
            h = NULL;               /* 同下：abort 已释放 handle */
            set_state(OTA_CLOUD_FAILED, "download_failed");
            goto done;
        }
        const char *pwhy = profile_verdict(ota_profile_match(esp_app_get_description()->project_name,
                                                             new_desc.project_name, sizeof(new_desc.project_name)),
                                           new_desc.project_name, sizeof(new_desc.project_name));
        if (pwhy) {
            esp_https_ota_abort(h);
            h = NULL;
            set_state(OTA_CLOUD_FAILED, pwhy);
            goto done;
        }

        int total = esp_https_ota_get_image_size(h);
        while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            int read = esp_https_ota_get_image_len_read(h);
            s_progress = (total > 0) ? (int)((int64_t)read * 100 / total) : -1;
        }

        if (err != ESP_OK) {
            /* 验签失败也走这里：esp_ota_write 阶段不验，验在 finish 里，
               但镜像头损坏之类会在 perform 阶段就被拒。 */
            esp_https_ota_abort(h);
            h = NULL;               /* abort 已释放 handle，不清空会在 done: 二次释放 */
            set_state(OTA_CLOUD_FAILED, "download_failed");
            goto done;
        }
        if (!esp_https_ota_is_complete_data_received(h)) {
            esp_https_ota_abort(h);
            h = NULL;
            set_state(OTA_CLOUD_FAILED, "incomplete_download");
            goto done;
        }

        /* ⚠️ 签名验证发生在这一步里面（finish → esp_ota_end）。
           返回 ESP_ERR_OTA_VALIDATE_FAILED 就是"签名不是我们的钥匙签的"。 */
        err = esp_https_ota_finish(h);
        h = NULL;
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            set_state(OTA_CLOUD_FAILED, "signature_invalid");
            ESP_LOGE(TAG, "镜像签名验证失败 —— 不是我们的钥匙签的，已拒绝安装");
            goto done;
        }
        if (err != ESP_OK) {
            set_state(OTA_CLOUD_FAILED, "install_failed");
            goto done;
        }
        app_done = true;
    }

    if (www_buf) {
        s_phase = "write";
        err = www_image_write(www_buf, www_len);
        if (err != ESP_OK) {
            /* app 已切了启动分区而前端没写进去 → 重启后是新 app + 旧前端，正是本改动要
               消灭的组合。把启动分区拨回当前运行的这个，整次升级作废，用户重试即可。 */
            if (app_done) {
                const esp_partition_t *run = esp_ota_get_running_partition();
                if (run && esp_ota_set_boot_partition(run) != ESP_OK) {
                    ESP_LOGE(TAG, "前端写入失败且启动分区拨不回 —— 重启后将是新 app + 旧前端");
                }
            }
            set_state(OTA_CLOUD_FAILED, "www_write_failed");
            goto done;
        }
    }

    s_progress = 100;
    ESP_LOGI(TAG, "已安装%s%s，等待重启生效",
             s_latest.app_newer ? " app" : "", s_latest.www_differs ? " 前端" : "");
    set_state(OTA_CLOUD_SUCCESS, NULL);

done:
    /* 只兜底"begin 成功但还没走到任何 abort/finish"的路径。
       ⚠️ 每条 abort 之后都必须把 h 清成 NULL —— abort 内部会 free(handle)，
       漏清就是在这里二次释放。2026-08-20 首版正是这么写的。 */
    if (h) esp_https_ota_abort(h);
    free(www_buf);
    s_phase = "";
    if (ota_cloud_get_state() != OTA_CLOUD_SUCCESS) s_progress = -1;
    /* 同 ncp_task 的 T2.25 实测口 —— 这条路同样是内部栈，且多一段 esp_https_ota
       的 TLS。成功路径上板子随后会重启，这行日志只在 /api/v1/logs 的 RTC 尾巴里留得住。 */
    {
        UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGW(TAG, "install_task 栈水位：分配 %d，最小剩余 %u，峰值用掉 %u（字节）",
                 INSTALL_TASK_STACK, (unsigned)hw, (unsigned)(INSTALL_TASK_STACK - hw));
    }
    s_task_running = false;
    vTaskDelete(NULL);
}

esp_err_t ota_cloud_install(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (s_task_running || s_ncp_task_running) return ESP_ERR_INVALID_STATE;
    if (!s_latest.valid || !url_is_https(s_latest.url)) return ESP_ERR_INVALID_STATE;
    if (s_latest.has_www && !url_is_https(s_latest.www_url)) return ESP_ERR_INVALID_STATE;

    /* 防降级：签名验证证明"这是我们发的"，但证明不了"这是新的"。
       控制了 manifest 的攻击者可以指向一个旧的、签名合法、但有已知漏洞的固件。
       eFuse 版的 anti-rollback 需要烧熔丝（本项目决策不烧），所以在这里用
       软件判据堵住网络侧的降级路径。物理接触者能绕过，但他直接串口刷更省事。
       app 相等而前端不同 = 只换前端，这是允许的（热修前端）。 */
    const esp_app_desc_t *app = esp_app_get_description();
    const char *cur = app ? app->version : "";
    if (ota_cloud_version_cmp(s_latest.version, cur) < 0) {
        set_state(OTA_CLOUD_FAILED, "refused_downgrade");
        ESP_LOGE(TAG, "拒绝安装 %s：比当前 %s 旧（防降级）", s_latest.version, cur);
        return ESP_ERR_INVALID_VERSION;
    }
    decide(&s_latest, cur);         /* 用此刻的运行版本重算，别信 check 时的快照 */
    if (!s_latest.app_newer && !s_latest.www_differs) {
        set_state(OTA_CLOUD_FAILED, "refused_downgrade");   /* 相等且前端相同：没东西可装 */
        ESP_LOGE(TAG, "拒绝安装 %s：不比当前 %s 新，前端也相同", s_latest.version, cur);
        return ESP_ERR_INVALID_VERSION;
    }

    /* 认领"我要起任务了"这件事必须和上面的检查在同一把锁里，否则两个并发请求
       会各自看到 false 然后起两个任务、同时写 flash。当前 httpd 是单任务，
       这条路走不到 —— 但 OTBR 那个 :8081 实例是第二个 httpd，别指望它一直如此。 */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool claimed = !s_task_running && !s_ncp_task_running;
    if (claimed) s_task_running = true;
    xSemaphoreGive(s_lock);
    if (!claimed) return ESP_ERR_INVALID_STATE;

    set_state(OTA_CLOUD_DOWNLOADING, NULL);
    /* 栈大小见 INSTALL_TASK_STACK：mbedtls 握手是这里的栈大头。低优先级(4)，别和 EZSP 抢。
       ⚠️ **这块栈必须在内部 SRAM，不能迁 PSRAM**（psram_task.h 的两条禁忌这里
       全中）：本任务写 flash（esp_ota_write / www_image_write），而擦写期间 cache 关闭、
       PSRAM 就在 cache 后面，访问自己的栈直接挂掉；本任务还 vTaskDelete(NULL) 自删。
       内部 SRAM 是本项目最紧的资源（min_free 约 15KB 量级），但这笔开销是**临时**
       的 —— 只在用户点了升级之后存在，装完即释放。分配不到时 xTaskCreate 返回
       pdFAIL，走下面的 task_create_failed，失败方向安全。 */
    if (xTaskCreate(install_task, "ota_cloud", INSTALL_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        s_task_running = false;
        set_state(OTA_CLOUD_FAILED, "task_create_failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---------------- 对外：安装 NCP（MG24 .gbl）---------------- */

static void ncp_task(void *arg)
{
    (void)arg;
    uint8_t *buf = NULL;
    const char *why = NULL;
    char e[48];
    s_progress = 0;

    esp_err_t err = fetch_blob(s_latest.ncp_url, s_latest.ncp_size, s_latest.ncp_sha256, &buf, &why);
    if (err != ESP_OK) {
        snprintf(e, sizeof(e), "ncp_%s", why ? why : "download_failed");
        set_ncp_state(OTA_CLOUD_NCP_FAILED, e);
        goto done;
    }

    /* 从这里起与 POST /api/v1/ota/zigbee 完全同一条路：暂存 → 校验 GBL → 后台 XMODEM。
       ota_mg24 自己会禁桥、进 bootloader、成功后记指纹并重读 NCP 版本。 */
    err = ota_mg24_begin(s_latest.ncp_size, OTA_MG24_SOURCE_CLOUD);   /* 云端永远只收签名包 */
    if (err != ESP_OK) {
        set_ncp_state(OTA_CLOUD_NCP_FAILED,
                      err == ESP_ERR_INVALID_SIZE ? "ncp_size_invalid" :
                      err == ESP_ERR_NO_MEM       ? "out_of_memory"    : "mg24_ota_busy");
        goto done;
    }
    err = ota_mg24_write(buf, s_latest.ncp_size);
    if (err != ESP_OK) {
        ota_mg24_abort();
        set_ncp_state(OTA_CLOUD_NCP_FAILED, "ncp_stage_failed");
        goto done;
    }
    /* 下载用的那份可以先还回去：ota_mg24_write 已经拷进自己的暂存区，
       留着它只是让 PSRAM 峰值多占一份 243 KB。 */
    free(buf); buf = NULL;

    /* 先置 FLASHING 再提交：下面这一步是**阻塞**的（~40 s），期间轮询者靠
       ota_mg24_busy() 维持 FLASHING，写完自动落回 IDLE（见 ota_cloud_ncp_state）。 */
    set_ncp_state(OTA_CLOUD_NCP_FLASHING, NULL);
    ESP_LOGI(TAG, "NCP 固件 %s 开始写入（在本任务栈上跑，进度看 /api/v1/ota/zigbee/status）", s_latest.ncp_version);

    /* ⚠️ 用 blocking 版本而不是 ota_mg24_commit()：后者要再建一个 4 KB **内部**栈的
       任务，而本任务自己就占着一块内部栈、HTTPS 下载又把内部堆打碎 —— 2026-09-14
       实测（当时栈还是 8192、最大块只剩 ~3.4 KB），第一次云端装 NCP 就栽在这里
       （commit_failed，/diag/heap 里一条 4096/caps=0x804），重试又过了。
       T2.25 收窄栈之后地板高了（见 NCP_TASK_STACK），但**仍然用 blocking**：
       不建第二个任务正是收窄能成立的前提。见 ota_mg24.h 的说明。 */
    err = ota_mg24_commit_blocking();
    if (err != ESP_OK) {
        ota_mg24_abort();
        set_ncp_state(OTA_CLOUD_NCP_FAILED,
                      err == ESP_ERR_NOT_SUPPORTED ? ota_mg24_last_reject_code() :
                      err == ESP_ERR_INVALID_SIZE  ? "truncated_body"  : "commit_failed");
        goto done;
    }

    /* 阻塞版返回只代表"这一场跑完了"，成败在状态机里 —— 只看返回值会把
       XMODEM 失败报成成功。 */
    {
        mg24_ota_status_t st;
        ota_mg24_get_status(&st);
        if (st.state == MG24_OTA_UNCONFIRMED) {
            /* 写进去了，但新固件没答话（T9.19 ⑨）：不是成功，也没证实失败；指纹没记，下次检查仍会提示 */
            set_ncp_state(OTA_CLOUD_NCP_FAILED, "ncp_unconfirmed");
            goto done;
        }
        if (st.state != MG24_OTA_SUCCESS) {
            set_ncp_state(OTA_CLOUD_NCP_FAILED, "ncp_flash_failed");
            goto done;
        }
    }
    ESP_LOGI(TAG, "NCP 固件 %s 写入完成", s_latest.ncp_version);

done:
    free(buf);                      /* ota_mg24_write 已拷进它自己的暂存区 */
    if (ota_cloud_ncp_state() != OTA_CLOUD_NCP_FLASHING) s_progress = -1;
    /* T2.25 实测口：这块内部栈是装机那十几秒谷底的一部分，收窄前先看它。
       单位是**字节**（IDF 的 task.h:1521「in bytes (as opposed to words in vanilla
       FreeRTOS)」，xTaskCreate 的栈参数同样是字节，见 task.h:316），两者可直接相减。
       用 W 级：默认日志级别下也进 /api/v1/logs，一次装机只打一行。
       失败路径也会走到这里，那种情况下的水位偏浅（没跑到 commit），别拿来定尺寸。 */
    {
        UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGW(TAG, "ncp_task 栈水位：分配 %d，最小剩余 %u，峰值用掉 %u（字节）",
                 NCP_TASK_STACK, (unsigned)hw, (unsigned)(NCP_TASK_STACK - hw));
    }
    s_ncp_task_running = false;
    vTaskDelete(NULL);
}

esp_err_t ota_cloud_install_ncp(bool force)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (s_task_running || s_ncp_task_running) return ESP_ERR_INVALID_STATE;
    if (ota_cloud_busy() || ota_mg24_busy()) return ESP_ERR_INVALID_STATE;
    if (!s_latest.valid || !s_latest.has_ncp || !url_is_https(s_latest.ncp_url)) return ESP_ERR_INVALID_STATE;

    s_latest.ncp_differs = ncp_image_differs(&s_latest);   /* 用此刻的指纹重算 */
    if (!s_latest.ncp_differs && !force) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool claimed = !s_task_running && !s_ncp_task_running;
    if (claimed) s_ncp_task_running = true;
    xSemaphoreGive(s_lock);
    if (!claimed) return ESP_ERR_INVALID_STATE;

    set_ncp_state(OTA_CLOUD_NCP_DOWNLOADING, NULL);
    /* 同 install_task：内部 SRAM 栈（TLS 握手 + 自删），临时开销。大小见 NCP_TASK_STACK */
    if (xTaskCreate(ncp_task, "ota_cloud_ncp", NCP_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        s_ncp_task_running = false;
        set_ncp_state(OTA_CLOUD_NCP_FAILED, "task_create_failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---------------- 初始化 ---------------- */

esp_err_t ota_cloud_init(void)
{
    if (s_lock) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    memset(&s_latest, 0, sizeof(s_latest));
    s_err[0] = '\0';
    s_ncp_err[0] = '\0';

    const char *url = CONFIG_BITENGINE_OTA_CLOUD_URL;
    if (!url || !url[0]) {
        ESP_LOGW(TAG, "未配置 manifest URL，检查更新会返回 not_configured");
    } else if (strstr(url, "example.com")) {
        /* 出厂固件带着占位域名 = 用户点"检查更新"永远失败，而且我们收不到任何
           信号。这条日志是唯一的现场线索，别删。 */
        ESP_LOGE(TAG, "manifest URL 仍是占位域名 %s —— 出货前必须改成真实地址", url);
    } else {
        ESP_LOGI(TAG, "manifest: %s", url);
    }
    return ESP_OK;
}
