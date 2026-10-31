/* 防串刷：镜像工程名的解析与比对（说明见 ota_profile.h）。
 * ⚠️ 不许引 ESP-IDF 头文件：test_host/run.sh 原样编这个文件，VERIFY.sh 钉着。 */
#include "ota_profile.h"

#include <string.h>

/* 字段可能不以 NUL 结尾（工程名正好 32 个字符时），所以手写有界长度，不用 strnlen（C11 里没有） */
static size_t bounded_len(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n] != '\0') n++;
    return n;
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

ota_profile_result_t ota_profile_name_from_image(const uint8_t *img, size_t len,
                                                 char out[OTA_PROFILE_NAME_LEN + 1])
{
    if (!out) return OTA_PROFILE_INVALID_ARG;
    out[0] = '\0';
    if (!img) return OTA_PROFILE_INVALID_ARG;
    if (len < OTA_PROFILE_MIN_IMAGE_LEN) return OTA_PROFILE_TRUNCATED;
    if (img[0] != OTA_PROFILE_IMAGE_MAGIC) return OTA_PROFILE_BAD_IMAGE_MAGIC;
    if (rd_le32(img + OTA_PROFILE_DESC_OFFSET) != OTA_PROFILE_DESC_MAGIC) return OTA_PROFILE_BAD_DESC_MAGIC;

    const char *field = (const char *)img + OTA_PROFILE_DESC_OFFSET + OTA_PROFILE_NAME_OFFSET;
    size_t n = bounded_len(field, OTA_PROFILE_NAME_LEN);
    if (n == 0) return OTA_PROFILE_NO_NAME;
    memcpy(out, field, n);
    out[n] = '\0';
    return OTA_PROFILE_OK;
}

ota_profile_result_t ota_profile_match(const char *running, const char *field, size_t field_len)
{
    if (!running || !field) return OTA_PROFILE_INVALID_ARG;
    if (field_len > OTA_PROFILE_NAME_LEN) field_len = OTA_PROFILE_NAME_LEN;
    /* 运行中那边也是 esp_app_desc_t 的 32 字节字段，名字正好 32 个字符时同样没有 NUL */
    size_t rn = bounded_len(running, OTA_PROFILE_NAME_LEN);
    size_t fn = bounded_len(field, field_len);
    /* 任一边为空都拒：空名说明描述被改过或构建出了问题，"两边都空所以相等"不能算数 */
    if (rn == 0 || fn == 0) return OTA_PROFILE_NO_NAME;
    /* 长度先比：防 "bitengine" 与 "bitengine-direct" 这种前缀相同的被当成一样 */
    if (rn != fn || memcmp(running, field, rn) != 0) return OTA_PROFILE_MISMATCH;
    return OTA_PROFILE_OK;
}

ota_profile_result_t ota_profile_check_image(const char *running, const uint8_t *img, size_t len)
{
    if (!running) return OTA_PROFILE_INVALID_ARG;   /* 参数先验：别让镜像的毛病盖住调用方的错 */
    char name[OTA_PROFILE_NAME_LEN + 1];
    ota_profile_result_t r = ota_profile_name_from_image(img, len, name);
    if (r != OTA_PROFILE_OK) return r;
    return ota_profile_match(running, name, sizeof(name) - 1);
}

const char *ota_profile_error_str(ota_profile_result_t r)
{
    switch (r) {
    case OTA_PROFILE_OK:              return NULL;
    case OTA_PROFILE_MISMATCH:        return "profile_mismatch";
    case OTA_PROFILE_NO_NAME:         return "profile_unknown";
    case OTA_PROFILE_TRUNCATED:
    case OTA_PROFILE_BAD_IMAGE_MAGIC:
    case OTA_PROFILE_BAD_DESC_MAGIC:  return "image_invalid";
    case OTA_PROFILE_INVALID_ARG:     return "internal_error";
    }
    return "internal_error";        /* 枚举外的值（内存被踩）也按拒装处理 */
}
