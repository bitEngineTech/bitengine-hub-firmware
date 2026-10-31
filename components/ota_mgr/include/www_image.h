/* www_image — www（前端 SPIFFS）分区镜像的形状校验与写入
 *
 * 两条更新路径共用：POST /api/v1/ota/www（webui）与云端更新（ota_cloud）。
 * 放在 ota_mgr 是依赖方向决定的：ota_cloud 不能依赖 webui（webui 依赖它），
 * 而 ota_mgr 两边都能引。
 *
 * 2026-09-10 抽出。此前 rest_api.c 的上传只判"长度不超过分区"，curl 直传一个
 * ≤512KB 的非 SPIFFS 文件会被原样写进去，前端整个打不开（审计 A19）。
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** www 分区大小（字节）；找不到分区返回 0。 */
size_t www_image_partition_size(void);

/** 镜像形状校验，写入前必须过：
 *   - len 必须等于分区大小（spiffsgen 的输出恒等于分区大小）
 *   - 首块与末块的 SPIFFS magic 必须与"本分区大小 + 本页大小"算出来的值一致
 *     （magic 含块数与块号，为别的分区尺寸生成的镜像会被拒）
 *  失败时 *reason 指向一个短标识（www_size_mismatch / www_not_spiffs /
 *  no_www_partition），可直接作 REST 错误码；成功时置 NULL。
 *  @return ESP_OK / ESP_ERR_INVALID_SIZE / ESP_ERR_INVALID_CRC / ESP_ERR_NOT_FOUND / ESP_ERR_INVALID_ARG */
esp_err_t www_image_validate(const uint8_t *buf, size_t len, const char **reason);

/** 逐扇区擦 + 写整张镜像。buf 可在 PSRAM（两条路径历来如此）。
 *  不做校验（调用方先过 validate），不重启（由调用方决定何时 esp_restart()）。
 *  @return ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_NOT_FOUND / ESP_ERR_INVALID_SIZE / flash 错误 */
esp_err_t www_image_write(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
