/* mc_iv_core（访谈结果）的主机测试：往返、各种坏 blob、组装（端点 / 设备类型 / 簇的去重与上限）、字符串截在字符边界。 bash run.sh */
#include <stdio.h>
#include <string.h>
#include "../mc_iv_core.h"

static int pass, fail;
#define CHECK(c, what) do { if (c) pass++; else { fail++; printf("❌ %s（%s:%d）\n", what, __FILE__, __LINE__); } } while (0)

static mc_iv_t eve(void)
{
    mc_iv_t iv;
    memset(&iv, 0, sizeof(iv));
    iv.node_id = 6; iv.vid = 0x130A; iv.pid = 0x0050; iv.flags = MC_IV_F_DONE;
    mc_iv_set_str(iv.vendor, "Eve Systems", 11);
    mc_iv_set_str(iv.product, "Eve Energy 20EBU4101", 20);
    mc_iv_ep_t *e0 = mc_iv_ep(&iv, 0);
    mc_iv_add_dt(&iv, e0, 0x0016);
    mc_iv_add_cl(&iv, e0, 0x0028); mc_iv_add_cl(&iv, e0, 0x001D);
    mc_iv_ep_t *e1 = mc_iv_ep(&iv, 1);
    mc_iv_add_dt(&iv, e1, 0x010A);
    mc_iv_add_cl(&iv, e1, 0x0006); mc_iv_add_cl(&iv, e1, 0x0090); mc_iv_add_cl(&iv, e1, 0x0091);
    return iv;
}

int main(void)
{
    uint8_t buf[MC_IV_BLOB_MAX + 8];
    mc_iv_t a = eve(), b;
    int n = mc_iv_encode(&a, buf, sizeof(buf));
    CHECK(n > 0 && mc_iv_decode(buf, (size_t)n, &b) && memcmp(&a, &b, sizeof(a)) == 0, "往返逐字节一致");
    CHECK(b.n_ep == 2 && b.ep[1].ep == 1 && b.ep[1].dt[0] == 0x010A && b.ep[1].n_cl == 3, "端点 / 设备类型 / 簇读回");
    CHECK(mc_iv_has_cluster(&b, 0x0006) && !mc_iv_has_cluster(&b, 0x0008), "有没有某个簇");

    /* 坏 blob：失败时清零 */
    memset(&b, 0x55, sizeof(b));
    CHECK(!mc_iv_decode(buf, (size_t)n - 1, &b) && b.node_id == 0 && b.n_ep == 0, "截断：拒、清零");
    CHECK(!mc_iv_decode(buf, (size_t)n + 1, &b), "多一个字节：拒");
    uint8_t bad[sizeof(buf)];
    memcpy(bad, buf, (size_t)n); bad[13] = MC_IV_EP_MAX + 1;
    CHECK(!mc_iv_decode(bad, (size_t)n, &b), "n_ep 超上限：拒");
    memcpy(bad, buf, (size_t)n); bad[14] = MC_IV_STR_MAX + 1;
    CHECK(!mc_iv_decode(bad, (size_t)n, &b), "字符串长度超上限：拒");
    memcpy(bad, buf, (size_t)n); bad[15] = 0;                          /* vendor 里塞个 NUL */
    CHECK(!mc_iv_decode(bad, (size_t)n, &b), "字符串里有 NUL：拒");
    mc_iv_t d = eve(); d.ep[1].ep = 0;                                  /* 端点号重复 */
    n = mc_iv_encode(&d, buf, sizeof(buf));
    CHECK(n > 0 && !mc_iv_decode(buf, (size_t)n, &b), "端点号重复：拒");
    CHECK(!mc_iv_decode(NULL, 14, &b) && !mc_iv_decode(buf, 13, &b), "空 / 太短");

    /* 编码拒绝 */
    mc_iv_t e = eve(); e.n_ep = MC_IV_EP_MAX + 1;
    CHECK(mc_iv_encode(&e, buf, sizeof(buf)) < 0, "n_ep 超上限：不编");
    e = eve();
    CHECK(mc_iv_encode(&e, buf, 20) < 0, "缓冲不够：不编");

    /* 组装：去重与上限 */
    mc_iv_t g;
    memset(&g, 0, sizeof(g));
    mc_iv_ep_t *x = mc_iv_ep(&g, 3), *x2 = mc_iv_ep(&g, 3);
    CHECK(x && x == x2 && g.n_ep == 1, "同一端点只建一次");
    for (uint16_t i = 0; i < MC_IV_EP_MAX + 2; i++) (void)mc_iv_ep(&g, (uint16_t)(10 + i));
    CHECK(g.n_ep == MC_IV_EP_MAX && (g.flags & MC_IV_F_EP_TRUNC), "端点满了置 EP_TRUNC");
    memset(&g, 0, sizeof(g));
    x = mc_iv_ep(&g, 1);
    mc_iv_add_cl(&g, x, 0x0006); mc_iv_add_cl(&g, x, 0x0006);
    CHECK(x->n_cl == 1, "簇去重");
    mc_iv_add_cl(&g, x, 0x131BFC00u);
    CHECK(x->n_cl == 1, "厂商簇不记");
    for (uint32_t i = 0; i < MC_IV_CL_MAX + 3; i++) mc_iv_add_cl(&g, x, 0x0100 + i);
    CHECK(x->n_cl == MC_IV_CL_MAX && (g.flags & MC_IV_F_CL_TRUNC), "簇满了置 CL_TRUNC");
    for (uint32_t i = 0; i < MC_IV_DT_MAX + 2; i++) mc_iv_add_dt(&g, x, 0x0100 + i);
    CHECK(x->n_dt == MC_IV_DT_MAX, "设备类型上限");

    /* 字符串：截在 UTF-8 字符边界 */
    char s[MC_IV_STR_MAX + 1];
    char src[64];
    memset(src, 'a', 31); memcpy(src + 31, "\xe4\xb8\xad\xe6\x96\x87", 6);   /* 31 个 a + "中文"：第 32 字节落在"中"中间 */
    mc_iv_set_str(s, src, 37);
    CHECK(strlen(s) == 31, "截断不切开多字节字符（31 个 a，丢掉半个'中'）");
    mc_iv_set_str(s, "abc\0def", 7);
    CHECK(!strcmp(s, "abc"), "遇到 NUL 就停");
    mc_iv_set_str(s, NULL, 5);
    CHECK(s[0] == 0, "NULL → 空串");
    memset(src, 'b', 40);
    mc_iv_set_str(s, src, 40);
    CHECK(strlen(s) == MC_IV_STR_MAX, "ASCII 截到 32");

    /* 够不够算访谈完了（审计）：要有根端点 0 的簇表；路径级错误让读成功、内容空的不算 */
    {
        mc_iv_t c = eve();
        CHECK(mc_iv_complete(&c), "完整：根端点有簇表");
        mc_iv_t z;
        memset(&z, 0, sizeof(z));
        CHECK(!mc_iv_complete(&z) && !mc_iv_complete(NULL), "什么都没读到 / NULL：不算");
        mc_iv_set_str(z.vendor, "Eve Systems", 11);
        (void)mc_iv_ep(&z, 1);
        mc_iv_add_cl(&z, &z.ep[0], 0x0006);
        CHECK(!mc_iv_complete(&z), "只有厂家与端点 1、没有根端点：不算");
        (void)mc_iv_ep(&z, 0);
        CHECK(!mc_iv_complete(&z), "根端点在、簇表空：不算");
    }

    printf("mc_iv: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
