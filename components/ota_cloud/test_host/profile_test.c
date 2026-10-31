/* 防串刷的真值表 —— 主机侧，不用板子（docs/15 设计 2.7、TC-PF01 的主机半）。
 *
 * 为什么单独测：两种固件同一把签名钥匙，挡串刷的只有这一处比对。它写错的表现要么是
 * "该拦的没拦住"（直连型装进 HA 型设备 → 没有 Wi-Fi、拆机才救得回，红线 1），要么是
 * "同型号也升不了级"。前者在板子上要真串一次才暴露，所以红线 1 要求先在这里证明。
 *
 *   bash components/ota_cloud/test_host/run.sh
 *
 * 编的是产品源文件本身（../ota_profile.c，见 run.sh）。
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../ota_profile.h"

static int fails = 0;

static void expect(ota_profile_result_t got, ota_profile_result_t want, const char *why)
{
    printf("%s %s（得 %d，期望 %d）\n", got == want ? "✅" : "❌", why, (int)got, (int)want);
    if (got != want) fails++;
}

/* 按 IDF 的布局造一个镜像开头：0xE9 头 + 段头 + app 描述（magic、版本、工程名） */
static size_t make_image(uint8_t *buf, size_t cap, const char *name, size_t name_bytes)
{
    memset(buf, 0, cap);
    buf[0] = 0xE9;                                        /* ESP_IMAGE_HEADER_MAGIC */
    buf[1] = 6;                                           /* segment_count */
    buf[12] = 23;                                         /* chip_id = ESP32-C5，与比对无关 */
    uint8_t *d = buf + OTA_PROFILE_DESC_OFFSET;
    d[0] = 0x32; d[1] = 0x54; d[2] = 0xCD; d[3] = 0xAB;  /* 0xABCD5432 小端 */
    memcpy(d + 16, "v1.0.0-5-g9efe0a9", 17);              /* version，与比对无关 */
    memcpy(d + OTA_PROFILE_NAME_OFFSET, name, name_bytes);
    return OTA_PROFILE_DESC_OFFSET + 256;                 /* 头 32 + 整个 esp_app_desc_t */
}

int main(void)
{
    uint8_t img[512];
    char out[OTA_PROFILE_NAME_LEN + 1];
    size_t n;

    /* ---- 正常：同型号放行，跨型号拒 ---- */
    n = make_image(img, sizeof(img), "bitengine", 9);
    expect(ota_profile_check_image("bitengine", img, n), OTA_PROFILE_OK, "HA 型镜像装进 HA 型设备 → 放行");
    expect(ota_profile_check_image("bitengine-direct", img, n), OTA_PROFILE_MISMATCH, "HA 型镜像装进直连型设备 → 拒");
    n = make_image(img, sizeof(img), "bitengine-direct", 16);
    expect(ota_profile_check_image("bitengine", img, n), OTA_PROFILE_MISMATCH, "直连型镜像装进 HA 型设备 → 拒（TC-PF01 那一向）");
    expect(ota_profile_check_image("bitengine-direct", img, n), OTA_PROFILE_OK, "直连型镜像装进直连型设备 → 放行");

    /* ---- 前缀相同、大小写、尾随字节 ---- */
    n = make_image(img, sizeof(img), "bitengine-", 10);
    expect(ota_profile_check_image("bitengine", img, n), OTA_PROFILE_MISMATCH, "\"bitengine-\" ≠ \"bitengine\"（不能按较短的那边比前缀）");
    n = make_image(img, sizeof(img), "bitengin", 8);
    expect(ota_profile_check_image("bitengine", img, n), OTA_PROFILE_MISMATCH, "\"bitengin\" ≠ \"bitengine\"");
    n = make_image(img, sizeof(img), "BitEngine", 9);
    expect(ota_profile_check_image("bitengine", img, n), OTA_PROFILE_MISMATCH, "大小写不同 → 拒（工程名就是逐字节的标识）");
    n = make_image(img, sizeof(img), "bitengine\0direct", 16);
    expect(ota_profile_check_image("bitengine", img, n), OTA_PROFILE_OK, "NUL 之后的字节不算名字");

    /* ---- 正好 32 个字符、没有 NUL 结尾（两边都一样）---- */
    const char *n32 = "abcdefghijklmnopqrstuvwxyz012345";       /* 32 个字符 */
    n = make_image(img, sizeof(img), n32, 32);
    expect(ota_profile_name_from_image(img, n, out), OTA_PROFILE_OK, "32 字符的名字取得出来");
    expect(strcmp(out, n32) == 0 ? OTA_PROFILE_OK : OTA_PROFILE_MISMATCH, OTA_PROFILE_OK, "  取出来的是完整 32 个字符且 NUL 结尾");
    char run32[40];                                              /* 运行中那边同样是没有 NUL 的 32 字节字段，后面紧跟别的字段 */
    memcpy(run32, n32, 32); memcpy(run32 + 32, "12:00:00", 8);
    expect(ota_profile_match(run32, (const char *)img + OTA_PROFILE_DESC_OFFSET + OTA_PROFILE_NAME_OFFSET, OTA_PROFILE_NAME_LEN),
           OTA_PROFILE_OK, "运行中 32 字节字段无 NUL、后面紧跟 time 字段 → 只比前 32 字节");

    /* ---- 坏镜像：一律拒，不猜 ---- */
    n = make_image(img, sizeof(img), "bitengine", 9);
    expect(ota_profile_check_image("bitengine", img, OTA_PROFILE_MIN_IMAGE_LEN - 1), OTA_PROFILE_TRUNCATED, "差 1 字节没到工程名末尾 → 截断");
    expect(ota_profile_check_image("bitengine", img, OTA_PROFILE_MIN_IMAGE_LEN), OTA_PROFILE_OK, "正好到工程名末尾 → 够了");
    expect(ota_profile_check_image("bitengine", img, 0), OTA_PROFILE_TRUNCATED, "空文件 → 截断");
    img[0] = 0xEA;
    expect(ota_profile_check_image("bitengine", img, n), OTA_PROFILE_BAD_IMAGE_MAGIC, "头 magic 不是 0xE9（比如传了 www.bin）→ 拒");
    n = make_image(img, sizeof(img), "bitengine", 9);
    img[OTA_PROFILE_DESC_OFFSET] = 0x33;
    expect(ota_profile_check_image("bitengine", img, n), OTA_PROFILE_BAD_DESC_MAGIC, "描述 magic 不对 → 拒");
    n = make_image(img, sizeof(img), "", 0);
    expect(ota_profile_check_image("bitengine", img, n), OTA_PROFILE_NO_NAME, "新镜像工程名为空 → 拒");
    n = make_image(img, sizeof(img), "bitengine", 9);
    expect(ota_profile_check_image("", img, n), OTA_PROFILE_NO_NAME, "运行中工程名为空 → 拒（两边都空不能算相等）");
    n = make_image(img, sizeof(img), "", 0);
    expect(ota_profile_check_image("", img, n), OTA_PROFILE_NO_NAME, "两边都空 → 仍然拒");

    /* ---- 参数 ---- */
    expect(ota_profile_check_image(NULL, img, n), OTA_PROFILE_INVALID_ARG, "running 为 NULL");
    expect(ota_profile_check_image("bitengine", NULL, 100), OTA_PROFILE_INVALID_ARG, "镜像为 NULL");
    expect(ota_profile_name_from_image(img, n, NULL), OTA_PROFILE_INVALID_ARG, "out 为 NULL");
    expect(ota_profile_match("bitengine", NULL, 9), OTA_PROFILE_INVALID_ARG, "字段为 NULL");
    expect(ota_profile_match("bitengine", "bitengine-direct", 9), OTA_PROFILE_OK, "field_len 截住：只看前 9 字节");
    expect(ota_profile_match("bitengine", "bitengine", 64), OTA_PROFILE_OK, "field_len 超过 32 按 32 截");

    /* ---- 错误串：REST / 云端状态拿它当标识，前端按它翻译 ---- */
    expect(ota_profile_error_str(OTA_PROFILE_OK) == NULL ? OTA_PROFILE_OK : OTA_PROFILE_MISMATCH, OTA_PROFILE_OK, "OK 没有错误串");
    expect(strcmp(ota_profile_error_str(OTA_PROFILE_MISMATCH), "profile_mismatch") == 0 ? OTA_PROFILE_OK : OTA_PROFILE_MISMATCH,
           OTA_PROFILE_OK, "MISMATCH → \"profile_mismatch\"（设计 2.7 定的名字）");
    expect(strcmp(ota_profile_error_str(OTA_PROFILE_TRUNCATED), "image_invalid") == 0 ? OTA_PROFILE_OK : OTA_PROFILE_MISMATCH,
           OTA_PROFILE_OK, "坏镜像 → \"image_invalid\"");
    expect(strcmp(ota_profile_error_str((ota_profile_result_t)99), "internal_error") == 0 ? OTA_PROFILE_OK : OTA_PROFILE_MISMATCH,
           OTA_PROFILE_OK, "枚举外的值 → \"internal_error\"（照样拒）");

    /* ---- 真产物（有就测）：idf.py build 出来的 build/bitengine.bin ---- */
    FILE *f = fopen("../../../build/bitengine.bin", "rb");
    if (f) {
        size_t got = fread(img, 1, sizeof(img), f);
        fclose(f);
        expect(ota_profile_name_from_image(img, got, out), OTA_PROFILE_OK, "真产物 build/bitengine.bin 取得出工程名");
        expect(strcmp(out, "bitengine") == 0 ? OTA_PROFILE_OK : OTA_PROFILE_MISMATCH, OTA_PROFILE_OK, "  而且就是 \"bitengine\"");
    } else {
        printf("⏭  没有 build/bitengine.bin，跳过真产物那两条\n");
    }

    printf("\n%s\n", fails ? "有失败" : "全部通过");
    return fails ? 1 : 0;
}
