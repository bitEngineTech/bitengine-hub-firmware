/* mc_ident_core 的主机测试：往返、每一种坏 blob、失败时不留半截密钥。 bash run.sh */
#include <stdio.h>
#include <string.h>
#include "../mc_ident_core.h"

static int pass, fail;
#define CHECK(c, what) do { if (c) pass++; else { fail++; printf("❌ %s（%s:%d）\n", what, __FILE__, __LINE__); } } while (0)

static mc_ident_t sample(uint16_t rcac_len)
{
    mc_ident_t id;
    memset(&id, 0, sizeof(id));
    id.fabric_id = 0x91D0366FA783EDE7ull;
    id.rcac_id = 0x0102030405060708ull;
    for (int i = 0; i < MC_IDENT_KP_LEN; i++) id.kp[i] = (uint8_t)(0x40 + i);
    for (int i = 0; i < MC_IDENT_IPK_LEN; i++) id.ipk[i] = (uint8_t)(0xA0 + i);
    id.rcac_len = rcac_len;
    for (int i = 0; i < rcac_len; i++) id.rcac[i] = (uint8_t)(i * 7);
    return id;
}

static int all_zero(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) if (b[i]) return 0;
    return 1;
}

int main(void)
{
    uint8_t buf[MC_IDENT_BLOB_MAX + 8];
    mc_ident_t a = sample(431), b;

    /* 往返 + 字节布局 */
    int n = mc_ident_encode(&a, buf, sizeof(buf));
    CHECK(n == 8 + 8 + 1 + 97 + 16 + 2 + 431, "编码长度 = 固定部分 + rcac_len");
    CHECK(buf[0] == 0xE7 && buf[7] == 0x91, "fabric_id 小端");
    CHECK(buf[16] == 97, "kp_len 字节是 97");
    CHECK(buf[16 + 1 + 97 + 16] == (431 & 0xFF) && buf[16 + 1 + 97 + 16 + 1] == (431 >> 8), "rcac_len 小端");
    CHECK(mc_ident_decode(buf, (size_t)n, &b) == MC_IDENT_OK, "解码成功");
    CHECK(memcmp(&a, &b, offsetof(mc_ident_t, rcac) + 431) == 0, "往返逐字节一致");

    /* 边界：rcac 1 与 600 */
    mc_ident_t s1 = sample(1), s6 = sample(MC_IDENT_RCAC_MAX);
    n = mc_ident_encode(&s1, buf, sizeof(buf));
    CHECK(n > 0 && mc_ident_decode(buf, (size_t)n, &b) == MC_IDENT_OK && b.rcac_len == 1, "rcac 1 字节可以");
    n = mc_ident_encode(&s6, buf, sizeof(buf));
    CHECK(n == MC_IDENT_BLOB_MAX && mc_ident_decode(buf, (size_t)n, &b) == MC_IDENT_OK, "rcac 600 字节 = 上限");

    /* 编码拒绝 */
    mc_ident_t z = sample(10); z.fabric_id = 0;
    CHECK(mc_ident_encode(&z, buf, sizeof(buf)) == -MC_IDENT_EZERO, "fabric_id 0 不编");
    z = sample(10); z.rcac_id = 0;
    CHECK(mc_ident_encode(&z, buf, sizeof(buf)) == -MC_IDENT_EZERO, "rcac_id 0 不编");
    z = sample(10); z.rcac_len = 0;
    CHECK(mc_ident_encode(&z, buf, sizeof(buf)) == -MC_IDENT_ELEN, "rcac 空不编");
    z = sample(10); z.rcac_len = MC_IDENT_RCAC_MAX + 1;
    CHECK(mc_ident_encode(&z, buf, sizeof(buf)) == -MC_IDENT_ELEN, "rcac 超 600 不编");
    z = sample(10);
    CHECK(mc_ident_encode(&z, buf, 8 + 8 + 1 + 97 + 16 + 2 + 9) == -MC_IDENT_EARG, "缓冲差一个字节不编");
    CHECK(mc_ident_encode(NULL, buf, sizeof(buf)) == -MC_IDENT_EARG && mc_ident_encode(&z, NULL, 10) == -MC_IDENT_EARG, "空指针不编");

    /* 解码拒绝，且失败时 out 全清零 */
    n = mc_ident_encode(&a, buf, sizeof(buf));
    memset(&b, 0x55, sizeof(b));
    CHECK(mc_ident_decode(buf, (size_t)n - 1, &b) == MC_IDENT_ELEN && all_zero(&b, sizeof(b)), "截断一个字节：拒、清零");
    memset(&b, 0x55, sizeof(b));
    CHECK(mc_ident_decode(buf, (size_t)n + 1, &b) == MC_IDENT_ELEN && all_zero(&b, sizeof(b)), "多一个字节：拒、清零");
    CHECK(mc_ident_decode(buf, 20, &b) == MC_IDENT_ELEN, "短于固定部分：拒");
    buf[16] = 96;
    memset(&b, 0x55, sizeof(b));
    CHECK(mc_ident_decode(buf, (size_t)n, &b) == MC_IDENT_EKP && all_zero(&b, sizeof(b)), "kp_len 不是 97：拒、清零（不留半截私钥）");
    buf[16] = 97;
    buf[16 + 1 + 97 + 16] = 0x59; buf[16 + 1 + 97 + 16 + 1] = 0x02;    /* rcac_len = 601 */
    CHECK(mc_ident_decode(buf, (size_t)n, &b) == MC_IDENT_ELEN, "rcac_len 601：拒");
    n = mc_ident_encode(&a, buf, sizeof(buf));
    memset(buf, 0, 8);
    memset(&b, 0x55, sizeof(b));
    CHECK(mc_ident_decode(buf, (size_t)n, &b) == MC_IDENT_EZERO && all_zero(&b, sizeof(b)), "存着的 fabric_id 是 0：拒、清零");
    CHECK(mc_ident_decode(NULL, 10, &b) == MC_IDENT_EARG && mc_ident_decode(buf, 10, NULL) == MC_IDENT_EARG, "空指针：拒");

    /* wipe */
    uint8_t w[32]; memset(w, 0xEE, sizeof(w)); mc_ident_wipe(w, sizeof(w));
    CHECK(all_zero(w, sizeof(w)), "wipe 清零");
    mc_ident_wipe(NULL, 5);

    printf("mc_ident: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
