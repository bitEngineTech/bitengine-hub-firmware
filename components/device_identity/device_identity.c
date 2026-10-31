#include <string.h>
#include <ctype.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "device_identity.h"

static const char *TAG = "device_identity";

/* 分区名与命名空间。命名空间 "factory" 与 tools/generate_nvs.py 生成的 CSV
   第一行（namespace,namespace,,factory）必须逐字一致，写错的表现是"分区在、
   能打开、但一个键都读不到"，很容易被误判成产线没写。 */
#define FCTRY_PARTITION   "fctry"
#define FCTRY_NAMESPACE   "factory"

/* 长度上限在头文件里（DEVICE_ID_MAX = 32，即 31 个字符）：调用方拷贝时要用
   同一个数。读到超长值按"非法"处理而不是截断 —— 截断出来的 SN 会静默指向
   另一台设备。 */
#define ID_MAX            DEVICE_ID_MAX

typedef struct {
    bool loaded;                    /* init 跑过了 */
    bool valid;                     /* 有格式合法的 SN */
    char serial[ID_MAX];
    char hw_rev[ID_MAX];
    char mfg_date[ID_MAX];
} identity_t;

static identity_t s_id;             /* 全零 = 三个空串 + 未加载 */

/* 序列号形状校验。刻意不认前缀，理由见头文件。
 *
 * 认：至少两段、段间单个 '-'、段内只有字母数字、整体 ≤ 31。
 * 不认：空、含空白或控制字符、前后有 '-'、连续 '--'、含 '/' '"' 等 ——
 *       后者尤其重要：SN 会被拼进 JSON（/status）和产线自检的 detail，
 *       放行引号类字符等于给自己开一个注入口。 */
static bool serial_shape_ok(const char *s)
{
    if (!s || !*s) return false;
    size_t n = strlen(s);
    if (n >= ID_MAX) return false;

    int segments = 1;
    bool seg_has_char = false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '-') {
            if (!seg_has_char) return false;    /* 空段：开头、结尾或 "--" */
            segments++;
            seg_has_char = false;
            continue;
        }
        if (!isalnum(c)) return false;          /* 顺带挡掉空白与所有标点 */
        seg_has_char = true;
    }
    if (!seg_has_char) return false;            /* 以 '-' 结尾 */
    return segments >= 2;
}

/* 读一个字符串键。读不到、太长、或带内嵌 NUL 都当作"没有"。 */
static void read_key(nvs_handle_t h, const char *key, char *out, size_t out_sz)
{
    size_t len = out_sz;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    if (err != ESP_OK) {
        out[0] = '\0';
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "读 %s 失败: %s", key, esp_err_to_name(err));
        }
        return;
    }
    out[out_sz - 1] = '\0';
}

esp_err_t device_identity_init(void)
{
    if (s_id.loaded) return ESP_OK;
    s_id.loaded = true;

    /* ⚠️ nvs_flash_init_partition() 对一个**从未被写过**的分区（全 0xFF）会
       返回 ESP_ERR_NVS_NO_FREE_PAGES。这里**绝不能**像 main.c 对待主 nvs 分区
       那样"erase 一下再来"——那是把产线写进去的身份擦掉的最快方式。
       没有就是没有，降级即可。 */
    esp_err_t err = nvs_flash_init_partition(FCTRY_PARTITION);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "fctry 分区不可用 (%s) —— 本机无出厂身份",
                 esp_err_to_name(err));
        return ESP_OK;
    }

    nvs_handle_t h;
    err = nvs_open_from_partition(FCTRY_PARTITION, FCTRY_NAMESPACE,
                                  NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "fctry 里没有 '%s' 命名空间 (%s) —— 本机无出厂身份",
                 FCTRY_NAMESPACE, esp_err_to_name(err));
        return ESP_OK;
    }

    /* ⚠️ 键名受 NVS 的 15 字符上限约束。docs/10 §5.2 写的 hardware_revision(17)
       与 manufacture_date(16) **超限**，nvs_partition_gen.py 会直接拒绝生成，
       那份 CSV 从来就写不进去。这里的短名与 tools/generate_nvs.py 逐字一致。 */
    read_key(h, "serial_number", s_id.serial,   sizeof(s_id.serial));
    read_key(h, "hw_revision",   s_id.hw_rev,   sizeof(s_id.hw_rev));
    read_key(h, "mfg_date",      s_id.mfg_date, sizeof(s_id.mfg_date));
    nvs_close(h);

    /* 读完就把分区的页缓存还回去。身份已经在 s_id 里，运行期不会再读第二次，
       没必要让 NVS 一直替一个只读 16KB 分区留着元数据。本机内部 SRAM 一直紧
       （docs/内部SRAM杠杆），能还的就还。
       之后谁再想读 fctry 都得自己 init —— VERIFY.sh 保证了没有第二处。 */
    nvs_flash_deinit_partition(FCTRY_PARTITION);

    s_id.valid = serial_shape_ok(s_id.serial);
    if (!s_id.valid && s_id.serial[0]) {
        /* 有值但形状不对：产线写错了。整串打出来，否则无从排查。 */
        ESP_LOGW(TAG, "序列号格式非法，按无身份处理: \"%s\"", s_id.serial);
        s_id.serial[0] = '\0';
    }

    if (s_id.valid) {
        ESP_LOGI(TAG, "SN=%s hw=%s 产期=%s", s_id.serial,
                 s_id.hw_rev[0]   ? s_id.hw_rev   : "(未写)",
                 s_id.mfg_date[0] ? s_id.mfg_date : "(未写)");
    } else {
        ESP_LOGI(TAG, "无出厂身份（产线未写入，或这是开发板）");
    }
    return ESP_OK;
}

bool device_identity_valid(void)            { return s_id.valid; }
const char *device_identity_serial(void)    { return s_id.serial; }
const char *device_identity_hw_revision(void) { return s_id.hw_rev; }
const char *device_identity_mfg_date(void)  { return s_id.mfg_date; }
