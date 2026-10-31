#pragma once
/* 防串刷：比对 app 镜像的工程名（docs/15 设计 2.7、架构 5.8 R-9）。
 *
 * 两种固件（HA 型 `bitengine` / 直连型 `bitengine-direct`）用同一把签名钥匙，验签挡不住"把直连型装进
 * HA 型设备"。串了就是没有 Wi-Fi、分区对不上，本板没有 BOOT 键和串口，只能拆机（CLAUDE.md 红线 1）。
 * 所以两条 app OTA 写入路径都在写 flash / 设启动分区之前比对新镜像的 project_name 与正在运行的固件。
 *
 * 本文件与 ota_profile.c 不依赖 ESP-IDF（同 version_cmp.c），test_host/run.sh 原样编它们。
 * 镜像格式常量与 IDF 定义的一致性由 ota_cloud.c 里的 _Static_assert 在固件构建时兜底。 */

#include <stddef.h>
#include <stdint.h>

/* ---- app 镜像格式（IDF v6.0.2，2026-10-21 按头文件与实际产物核实）----
 * esp_image_header_t 24 B（esp_app_format.h，ESP_STATIC_ASSERT 钉着）+ 第一个段的
 * esp_image_segment_header_t 8 B，之后就是 esp_app_desc_t（esp_https_ota.c 的 get_description_from_image
 * 用的是同一个偏移）。esp_app_desc_t：magic_word(4) secure_version(4) reserv1[2](8) version[32]
 * project_name[32] …，所以工程名在描述里偏移 48、在镜像里偏移 80，32 字节、不保证以 NUL 结尾。 */
#define OTA_PROFILE_IMAGE_MAGIC     0xE9u         /* ESP_IMAGE_HEADER_MAGIC */
#define OTA_PROFILE_DESC_OFFSET     32u           /* 24 + 8 */
#define OTA_PROFILE_DESC_MAGIC      0xABCD5432u   /* ESP_APP_DESC_MAGIC_WORD，小端存放 */
#define OTA_PROFILE_NAME_OFFSET     48u           /* offsetof(esp_app_desc_t, project_name) */
#define OTA_PROFILE_NAME_LEN        32u           /* sizeof(esp_app_desc_t.project_name) */
#define OTA_PROFILE_MIN_IMAGE_LEN   (OTA_PROFILE_DESC_OFFSET + OTA_PROFILE_NAME_OFFSET + OTA_PROFILE_NAME_LEN)

typedef enum {
    OTA_PROFILE_OK = 0,
    OTA_PROFILE_INVALID_ARG,        /* NULL 指针 */
    OTA_PROFILE_TRUNCATED,          /* 不够 OTA_PROFILE_MIN_IMAGE_LEN 字节 */
    OTA_PROFILE_BAD_IMAGE_MAGIC,    /* 第 0 字节不是 0xE9：不是 ESP app 镜像 */
    OTA_PROFILE_BAD_DESC_MAGIC,     /* 偏移 32 处没有 app 描述 */
    OTA_PROFILE_NO_NAME,            /* 工程名为空（运行中的或新镜像的） */
    OTA_PROFILE_MISMATCH,           /* 两边都读得出、但不是同一种固件 */
} ota_profile_result_t;

/* 从镜像开头取工程名，写进 out（NUL 结尾，最多 OTA_PROFILE_NAME_LEN 个字符）。
 * 失败时 out 置空串。任何拿不准的情况都返回非 OK —— 调用方据此拒装（读不出就当不是我们的）。 */
ota_profile_result_t ota_profile_name_from_image(const uint8_t *img, size_t len,
                                                 char out[OTA_PROFILE_NAME_LEN + 1]);

/* 比对运行中固件的工程名（NUL 结尾的字符串）与新镜像描述里那 32 字节字段（不保证 NUL 结尾，
 * 按 field_len 截住，最多 OTA_PROFILE_NAME_LEN）。完全相同才 OK；任一边为空 → NO_NAME。 */
ota_profile_result_t ota_profile_match(const char *running, const char *field, size_t field_len);

/* 一步做完：镜像开头 → 取名 → 与 running 比对。网页上传路径用它（整包已在内存里）。 */
ota_profile_result_t ota_profile_check_image(const char *running, const uint8_t *img, size_t len);

/* 给 REST / 云端状态用的短标识："profile_mismatch"、"image_invalid"… OK 时返回 NULL。 */
const char *ota_profile_error_str(ota_profile_result_t r);
