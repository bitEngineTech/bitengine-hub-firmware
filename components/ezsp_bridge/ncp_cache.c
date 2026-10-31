/*
 * NCP 读数落盘（T9.31 ②，2026-10-08）。
 *
 * 读 NCP 版本要先发 ASH RST，而 ASH RST 会把 MG24 的 Zigbee 协议栈打回未初始化 —— 网关自己
 * 拉不起来，得等 ZHA/Z2M 来。整板上电时这无所谓（MG24 也刚上电，协议栈本来就没起来）；但 C5
 * 单独重启时（OTA、看门狗自愈、网页重启）MG24 多半还在带网，没有协调器的话一发就停网，停几小时
 * 有设备会丢网回出厂态（10-07 Sengled）。所以那种开机改用这里存的上一次读数，不碰 UART。
 *
 * 只存"真读到的"（ncp_version_query 读到有效版本后经回调存），内容没变不写 —— 平时开机零写入。
 * 命名空间 ncp_cache 不在恢复出厂的擦除表里：不需要擦，开机时会拿缓存里 NCP 回报的网关地区与
 * 当前配置比对，对不上就重读（恢复出厂清掉了地区，正好触发一次重读把空声明发下去）。
 */

#include "ncp_cache.h"

#include "esp_log.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "ncp_cache";

#define NVS_NS        "ncp_cache"
#define NVS_KEY       "v"
/* 结构版本：ncp_version_t 一改布局就加一。旧固件存的东西长度或版本对不上 → 当作没有缓存、重读一次 */
#define CACHE_SCHEMA  1u

typedef struct {
    uint32_t      schema;
    ncp_version_t v;
} ncp_cache_blob_t;

/* 比较时忽略"什么时候读的、是不是缓存"这两项：它们每次都变，比进去就每次开机都写一遍 */
static void normalize(ncp_version_t *v)
{
    v->txp.read_uptime_s = 0;
    v->from_cache = false;
}

bool ncp_cache_load(ncp_version_t *out)
{
    if (!out) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;   /* 从没存过 */
    ncp_cache_blob_t b;
    size_t len = sizeof(b);
    esp_err_t err = nvs_get_blob(h, NVS_KEY, &b, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(b) || b.schema != CACHE_SCHEMA || !b.v.valid) {
        if (err == ESP_OK) ESP_LOGW(TAG, "缓存的结构版本对不上（len=%u schema=%u），当作没有",
                                    (unsigned)len, (unsigned)b.schema);
        return false;
    }
    *out = b.v;
    return true;
}

void ncp_cache_save(const ncp_version_t *v)
{
    if (!v || v->from_cache) return;
    if (!v->valid) {
        /* 这次会话真跑了但没读到：芯片里现在是什么不知道，旧读数不能再拿来顶替 */
        ncp_cache_invalidate();
        return;
    }

    ncp_cache_blob_t b = { .schema = CACHE_SCHEMA, .v = *v };
    normalize(&b.v);

    ncp_version_t old;
    if (ncp_cache_load(&old)) {
        normalize(&old);
        if (memcmp(&old, &b.v, sizeof(old)) == 0) return;      /* 没变，不磨 flash */
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open 失败（%s），NCP 读数没存 —— 下次 C5 单独重启会再读一次 NCP",
                 esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(h, NVS_KEY, &b, sizeof(b));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "写 NVS 失败（%s），NCP 读数没存", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "NCP 读数已存（下次 C5 单独重启时沿用，不发 ASH RST）");
    }
}

void ncp_cache_invalidate(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;     /* 从没存过，也就没什么可作废的 */
    esp_err_t err = nvs_erase_key(h, NVS_KEY);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        /* 作废失败的后果是"刷坏了之后软重启仍报旧读数"，不致命，但要留痕 */
        ESP_LOGE(TAG, "作废 NCP 读数缓存失败（%s）", esp_err_to_name(err));
    }
}
