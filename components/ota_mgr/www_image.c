/* www_image — www 分区镜像的形状校验与写入。设计说明见 include/www_image.h。 */
#include "www_image.h"

#include <string.h>
#include "esp_partition.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "www_image";

/* esp_spiffs 挂载时 log_block_size = 4096（flash 扇区），log_page_size = CONFIG_SPIFFS_PAGE_SIZE。
   两个值一变，下面的 magic 就变 —— 那正是它的用处。 */
#define WWW_BLOCK_SIZE   4096u
#define WWW_PAGE_SIZE    ((uint32_t)CONFIG_SPIFFS_PAGE_SIZE)

static const esp_partition_t *find_www(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "www");
}

size_t www_image_partition_size(void)
{
    const esp_partition_t *p = find_www();
    return p ? p->size : 0;
}

/* H1 —— 校验值的来源，别凭记忆改：
 *   spiffsgen.py:_calc_magic   magic = 0x20140529 ^ page_size ^ (blocks_lim - bix)，取低 16 位
 *   spiffsgen.py:magicfy       写在每块"最后一个查找页"的倒数第二个 obj_id 槽（i == remaining - 2）
 *   spiffs_nucleus.h           SPIFFS_MAGIC / SPIFFS_MAGIC_PADDR 与之镜像；obj_id 是 u16
 * 本项目 page=256、block=4096：每块 1 个查找页、128 个 obj_id 槽，magic 在第 126 槽 = 字节偏移 252。
 * 2026-09-10 用 build/www.bin（128 块）逐块核对：128/128 命中，偏移 252。 */
static uint16_t expected_magic(uint32_t blocks, uint32_t bix)
{
    return (uint16_t)((0x20140529u ^ WWW_PAGE_SIZE ^ (blocks - bix)) & 0xFFFFu);
}

static uint32_t magic_offset_in_block(void)
{
    uint32_t pages_per_block = WWW_BLOCK_SIZE / WWW_PAGE_SIZE;                 /* 16 */
    uint32_t lookup_pages    = (pages_per_block * sizeof(uint16_t)) / WWW_PAGE_SIZE;
    if (lookup_pages == 0) lookup_pages = 1;                                  /* SPIFFS_OBJ_LOOKUP_PAGES 的 MAX(1, …) */
    return lookup_pages * WWW_PAGE_SIZE - 2 * sizeof(uint16_t);               /* 倒数第二个槽 */
}

esp_err_t www_image_validate(const uint8_t *buf, size_t len, const char **reason)
{
    const char *why = "www_invalid";
    esp_err_t ret = ESP_ERR_INVALID_ARG;

    if (!buf || len == 0) goto out;

    const esp_partition_t *p = find_www();
    if (!p) { why = "no_www_partition"; ret = ESP_ERR_NOT_FOUND; goto out; }

    /* spiffsgen 的输出恒等于分区大小；不等于的东西不是给这个分区生成的镜像 */
    if (len != p->size || (len % WWW_BLOCK_SIZE) != 0) {
        why = "www_size_mismatch"; ret = ESP_ERR_INVALID_SIZE; goto out;
    }

    uint32_t blocks = (uint32_t)(p->size / WWW_BLOCK_SIZE);
    uint32_t off    = magic_offset_in_block();
    /* 首块与末块都查：magic 含 (blocks - bix)，两端一起对上才说明整张镜像是按本分区大小生成的 */
    const uint32_t bixs[2] = { 0, blocks - 1 };
    for (int i = 0; i < 2; i++) {
        size_t at = (size_t)bixs[i] * WWW_BLOCK_SIZE + off;
        uint16_t got = (uint16_t)buf[at] | (uint16_t)((uint16_t)buf[at + 1] << 8);
        if (got != expected_magic(blocks, bixs[i])) {
            ESP_LOGW(TAG, "block %u magic %04x != %04x — not a SPIFFS image for this partition",
                     (unsigned)bixs[i], got, expected_magic(blocks, bixs[i]));
            why = "www_not_spiffs"; ret = ESP_ERR_INVALID_CRC; goto out;
        }
    }

    why = NULL; ret = ESP_OK;
out:
    if (reason) *reason = why;
    return ret;
}

esp_err_t www_image_write(const uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return ESP_ERR_INVALID_ARG;
    const esp_partition_t *p = find_www();
    if (!p) return ESP_ERR_NOT_FOUND;
    if (len > p->size) return ESP_ERR_INVALID_SIZE;

    /* 逐扇区擦 + 写：每次 flash 停顿都短，RS706 喂狗任务能继续跑（app OTA 的同一课）。 */
    esp_err_t err = ESP_OK;
    for (size_t off = 0; off < len && err == ESP_OK; off += WWW_BLOCK_SIZE) {
        size_t chunk = len - off;
        if (chunk > WWW_BLOCK_SIZE) chunk = WWW_BLOCK_SIZE;
        err = esp_partition_erase_range(p, off, WWW_BLOCK_SIZE);
        if (err == ESP_OK) err = esp_partition_write(p, off, buf + off, chunk);
        if (err != ESP_OK) ESP_LOGE(TAG, "write failed at 0x%x: %s", (unsigned)off, esp_err_to_name(err));
    }
    return err;
}
