/* 差分验证：把 ncp_version.c 里的 CRC/LFSR/组帧逻辑与 SDK 原版逐位比对。
 * SDK 原版函数直接从 mg24-fw/sdk 抄过来（下方 sdk_* 前缀），不是我重写的。 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

/* ============ SDK 原版（legacy_hal/src/crc.c:28，逐字复制）============ */
static uint16_t sdk_halCommonCrc16(uint8_t newByte, uint16_t prevResult)
{
  prevResult = ((uint16_t) (prevResult >> 8)) | ((uint16_t) (prevResult << 8));
  prevResult ^= newByte;
  prevResult ^= (prevResult & 0xff) >> 4;
  prevResult ^= (uint16_t) (((uint16_t) (prevResult << 8)) << 4);

  prevResult ^= ((uint8_t) (((uint8_t) (prevResult & 0xff)) << 5))
                | ((uint16_t) ((uint16_t) ((uint8_t) (((uint8_t) (prevResult & 0xff)) >> 3)) << 8));

  return prevResult;
}

/* ============ SDK 原版（ash-common.c:204，逐字复制）============ */
#define LFSR_POLY 0xB8
#define LFSR_SEED 0x42
static uint8_t sdk_ashRandomizeArray(uint8_t seed, uint8_t *buf, uint8_t len)
{
  if (seed == 0) {
    seed = LFSR_SEED;
  }
  while (len--) {
    *buf++ ^= seed;
    seed = (seed & 1) ? ((seed >> 1) ^ LFSR_POLY) : (seed >> 1);
  }
  return seed;
}

/* ============ 我写的版本（从 ncp_version.c 复制）============ */
static uint16_t crc16_byte(uint8_t new_byte, uint16_t prev)
{
    prev = (uint16_t)((prev >> 8) | (uint16_t)(prev << 8));
    prev ^= new_byte;
    prev ^= (uint16_t)((prev & 0xFF) >> 4);
    prev ^= (uint16_t)((uint16_t)(prev << 8) << 4);
    prev ^= (uint16_t)((uint8_t)((uint8_t)(prev & 0xFF) << 5)
                       | (uint16_t)((uint16_t)(uint8_t)((uint8_t)(prev & 0xFF) >> 3) << 8));
    return prev;
}
static uint16_t crc16_buf(const uint8_t *buf, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) crc = crc16_byte(buf[i], crc);
    return crc;
}
static void ash_randomize(uint8_t *buf, int len)
{
    uint8_t seed = LFSR_SEED;
    while (len--) {
        *buf++ ^= seed;
        seed = (seed & 1) ? (uint8_t)((seed >> 1) ^ LFSR_POLY) : (uint8_t)(seed >> 1);
    }
}

#define ASH_FLAG 0x7E
#define ASH_ESC  0x7D
#define ASH_XON  0x11
#define ASH_XOFF 0x13
#define ASH_SUB  0x18
#define ASH_CAN  0x1A
#define ASH_FLIP 0x20
#define ASH_IS_DATA(c) (((c) & 0x80) == 0)

static bool needs_escape(uint8_t b)
{
    return b == ASH_FLAG || b == ASH_ESC || b == ASH_XON ||
           b == ASH_XOFF || b == ASH_SUB || b == ASH_CAN;
}

static int build_frame(uint8_t control, const uint8_t *data, int len, uint8_t *out)
{
    uint8_t raw[256];
    int n = 0;
    raw[n++] = control;
    if (len > 0) {
        memcpy(raw + n, data, len);
        if (ASH_IS_DATA(control)) ash_randomize(raw + n, len);
        n += len;
    }
    uint16_t crc = crc16_buf(raw, n);
    raw[n++] = (uint8_t)(crc >> 8);
    raw[n++] = (uint8_t)(crc & 0xFF);

    int o = 0;
    for (int i = 0; i < n; i++) {
        if (needs_escape(raw[i])) { out[o++] = ASH_ESC; out[o++] = raw[i] ^ ASH_FLIP; }
        else out[o++] = raw[i];
    }
    out[o++] = ASH_FLAG;
    return o;
}

/* ============ 测试 ============ */
static int fails = 0;
static void check(const char *name, bool ok)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) fails++;
}

static void dump(const char *label, const uint8_t *b, int n)
{
    printf("     %s:", label);
    for (int i = 0; i < n; i++) printf(" %02X", b[i]);
    printf("\n");
}

int main(void)
{
    /* 1. CRC 与 SDK 原版逐字节比对（全 256 字节 × 多个初值） */
    bool crc_same = true;
    uint16_t seeds[] = {0xFFFF, 0x0000, 0x1234, 0xABCD};
    for (unsigned s = 0; s < sizeof(seeds)/sizeof(seeds[0]); s++)
        for (int b = 0; b < 256; b++)
            if (crc16_byte((uint8_t)b, seeds[s]) != sdk_halCommonCrc16((uint8_t)b, seeds[s]))
                crc_same = false;
    check("CRC16 与 SDK halCommonCrc16 逐位一致", crc_same);

    /* 2. CRC-CCITT 标准向量: "123456789" -> 0x29B1 */
    uint16_t v = crc16_buf((const uint8_t *)"123456789", 9);
    check("CRC16(\"123456789\") == 0x29B1", v == 0x29B1);
    if (v != 0x29B1) printf("     实际: 0x%04X\n", v);

    /* 3. LFSR 与 SDK 原版一致 */
    uint8_t a[32], b[32];
    for (int i = 0; i < 32; i++) a[i] = b[i] = (uint8_t)(i * 7 + 3);
    ash_randomize(a, 32);
    sdk_ashRandomizeArray(0, b, 32);
    check("LFSR 随机化与 SDK ashRandomizeArray 一致", memcmp(a, b, 32) == 0);

    /* 4. 随机化是自反的（XOR 两次还原） */
    uint8_t c[16]; for (int i = 0; i < 16; i++) c[i] = (uint8_t)(i * 13);
    uint8_t orig[16]; memcpy(orig, c, 16);
    ash_randomize(c, 16); ash_randomize(c, 16);
    check("randomize 两次还原原文", memcmp(c, orig, 16) == 0);

    /* 5. RST 帧必须是业界皆知的 C0 38 BC 7E */
    uint8_t out[64];
    int n = build_frame(0xC0, NULL, 0, out);
    static const uint8_t want_rst[] = {0xC0, 0x38, 0xBC, 0x7E};
    check("RST 帧 == C0 38 BC 7E",
          n == 4 && memcmp(out, want_rst, 4) == 0);
    if (n != 4 || memcmp(out, want_rst, 4)) dump("实际", out, n);

    /* 6. EZSP version 命令（legacy 格式，seq=0，desired=0x12）整帧 */
    const uint8_t ver_cmd[] = {0x00, 0x00, 0x00, 0x12};
    n = build_frame(0x00, ver_cmd, 4, out);
    dump("version DATA 帧", out, n);
    /* 数据被随机化后第一字节应为 0x00^0x42 = 0x42 */
    check("version 帧首个数据字节 = seq ^ 0x42", out[1] == 0x42);

    /* 7. 转义：数据里出现 0x7E/0x11 等必须被 ESC+FLIP */
    const uint8_t esc_payload[] = {0x7E, 0x11, 0x13, 0x18, 0x1A, 0x7D};
    uint8_t plain[6]; memcpy(plain, esc_payload, 6);
    n = build_frame(0x80, esc_payload, 6, out);   /* 非 DATA：不随机化，便于看转义 */
    bool has_raw_special = false;
    for (int i = 0; i < n - 1; i++)               /* 末字节是 FLAG，跳过 */
        if (out[i] != ASH_ESC && needs_escape(out[i])) has_raw_special = true;
    check("帧内无未转义的特殊字节", !has_raw_special);
    dump("转义帧", out, n);

    /* 8. 收发闭环：build_frame 出来的帧能被同样的 CRC 校验通过 */
    n = build_frame(0x00, ver_cmd, 4, out);
    uint8_t unstuffed[64]; int u = 0; bool esc = false;
    for (int i = 0; i < n - 1; i++) {
        if (out[i] == ASH_ESC) { esc = true; continue; }
        unstuffed[u++] = esc ? (out[i] ^ ASH_FLIP) : out[i];
        esc = false;
    }
    uint16_t want = (uint16_t)((unstuffed[u-2] << 8) | unstuffed[u-1]);
    check("解帧后 CRC 自洽", crc16_buf(unstuffed, u - 2) == want);

    printf("\n%s (%d failed)\n", fails ? "有失败项" : "全部通过", fails);
    return fails ? 1 : 0;
}
