/* mc_nodes_core / mc_code_core 的主机测试。 bash run.sh */
#include <stdio.h>
#include <string.h>
#include "../mc_nodes_core.h"
#include "../mc_code_core.h"

static int pass, fail;
#define CHECK(c, what) do { if (c) pass++; else { fail++; printf("❌ %s（%s:%d）\n", what, __FILE__, __LINE__); } } while (0)

static void test_nodes(void)
{
    mc_nodes_t t, u;
    uint8_t buf[MC_NODES_BLOB_MAX + 4];
    uint64_t id = 0;
    mc_nodes_init(&t, 3);
    CHECK(t.next_node == 2 && t.n == 0 && t.max == 3, "空表：next_node 从 2 起");
    CHECK(mc_nodes_alloc(&t, 0x1234, 0x0056, &id) == MC_NODES_OK && id == 2 && t.next_node == 3, "第一个号是 2");
    CHECK(t.nodes[0].state == MC_NODE_PENDING && t.nodes[0].vid == 0x1234 && t.nodes[0].pid == 0x56, "新号先记 PENDING、带 vid/pid");
    CHECK(mc_nodes_alloc(&t, 0, 0, &id) == MC_NODES_OK && id == 3, "第二个号 3");
    CHECK(mc_nodes_set_state(&t, 2, MC_NODE_OK) == MC_NODES_OK && mc_nodes_find(&t, 2)->state == MC_NODE_OK, "改 OK");
    CHECK(mc_nodes_remove(&t, 3) == MC_NODES_OK && t.n == 1 && t.next_node == 4, "删掉 3：号不还（next_node 还是 4）");
    CHECK(mc_nodes_alloc(&t, 0, 0, &id) == MC_NODES_OK && id == 4, "删过之后再分配是 4，不是 3");
    CHECK(mc_nodes_alloc(&t, 0, 0, &id) == MC_NODES_OK && id == 5 && t.n == 3, "满到 max");
    CHECK(mc_nodes_alloc(&t, 0, 0, &id) == MC_NODES_EFULL && t.next_node == 6, "满了拒、next_node 不动");
    CHECK(mc_nodes_set_state(&t, 99, MC_NODE_OK) == MC_NODES_ENOTFOUND && mc_nodes_remove(&t, 99) == MC_NODES_ENOTFOUND, "不存在的号");
    CHECK(mc_nodes_set_state(&t, 2, (mc_node_state_t)7) == MC_NODES_EARG, "不认识的状态拒");

    /* 往返 */
    int n = mc_nodes_encode(&t, buf, sizeof(buf));
    CHECK(n == 9 + 3 * 16, "编码长度 = 9 + 16 × 条数");
    CHECK(mc_nodes_decode(buf, (size_t)n, 3, &u) == MC_NODES_OK && u.n == 3 && u.next_node == 6 && u.nodes[0].node_id == 2 &&
          u.nodes[0].state == MC_NODE_OK && u.nodes[0].vid == 0x1234 && u.nodes[1].node_id == 4, "往返");

    /* 开机：丢掉 PENDING */
    CHECK(mc_nodes_drop_pending(&u) == 2 && u.n == 1 && u.nodes[0].node_id == 2 && u.next_node == 6, "开机丢掉配到一半的两条，号不还");

    /* 坏 blob：失败时是空表 */
    CHECK(mc_nodes_decode(buf, (size_t)n - 1, 3, &u) == MC_NODES_ELEN && u.n == 0 && u.next_node == 2, "截断：拒、空表");
    CHECK(mc_nodes_decode(buf, (size_t)n, 2, &u) == MC_NODES_ELEN, "条数超过 max：拒");
    uint8_t bad[sizeof(buf)];
    memcpy(bad, buf, (size_t)n); bad[9 + 16 + 0] = 2;  /* 第二条的号改成 2：重复 */
    for (int i = 1; i < 8; i++) bad[9 + 16 + i] = 0;
    CHECK(mc_nodes_decode(bad, (size_t)n, 3, &u) == MC_NODES_EBAD, "号重复：拒");
    memcpy(bad, buf, (size_t)n); bad[9] = 1;  for (int i = 1; i < 8; i++) bad[9 + i] = 0;
    CHECK(mc_nodes_decode(bad, (size_t)n, 3, &u) == MC_NODES_EBAD, "号 1（控制器自己）：拒");
    memcpy(bad, buf, (size_t)n); bad[9] = 6;
    CHECK(mc_nodes_decode(bad, (size_t)n, 3, &u) == MC_NODES_EBAD, "号 ≥ next_node：拒");
    memcpy(bad, buf, (size_t)n); bad[9 + 12] = 9;
    CHECK(mc_nodes_decode(bad, (size_t)n, 3, &u) == MC_NODES_EBAD, "状态不认识：拒");
    memcpy(bad, buf, (size_t)n); memset(bad, 0, 8); bad[0] = 1;
    CHECK(mc_nodes_decode(bad, (size_t)n, 3, &u) == MC_NODES_EBAD, "next_node < 2：拒");
    CHECK(mc_nodes_decode(NULL, 9, 3, &u) == MC_NODES_EARG && mc_nodes_decode(buf, 9, 3, NULL) == MC_NODES_EARG, "空指针");
    mc_nodes_t e; mc_nodes_init(&e, 3);
    n = mc_nodes_encode(&e, buf, sizeof(buf));
    CHECK(n == 9 && mc_nodes_decode(buf, 9, 3, &u) == MC_NODES_OK && u.n == 0 && u.next_node == 2, "空表往返");
    CHECK(mc_nodes_encode(&t, buf, 9 + 2 * 16) == -MC_NODES_EARG, "缓冲不够：拒");
    mc_nodes_init(&e, 0);
    CHECK(e.max == MC_NODES_CAP, "max 0 当成结构容量");
}

static void test_code(void)
{
    char out[MC_CODE_MAX + 8];
    CHECK(mc_code_normalize("34970112332", out, sizeof(out)) == MC_CODE_MANUAL && !strcmp(out, "34970112332"), "11 位手动码");
    CHECK(mc_code_normalize("  3497-011-2332 \n", out, sizeof(out)) == MC_CODE_MANUAL && !strcmp(out, "34970112332"), "带连字符与空白");
    CHECK(mc_code_normalize("749701123365521327694", out, sizeof(out)) == MC_CODE_MANUAL && strlen(out) == 21, "21 位");
    CHECK(mc_code_normalize("3497011233", out, sizeof(out)) == 0 && out[0] == 0, "10 位：拒");
    CHECK(mc_code_normalize("349701123321", out, sizeof(out)) == 0, "12 位：拒");
    CHECK(mc_code_normalize("3497O112332", out, sizeof(out)) == 0, "字母 O：拒");
    CHECK(mc_code_normalize("", out, sizeof(out)) == 0 && mc_code_normalize("   ", out, sizeof(out)) == 0, "空：拒");
    CHECK(mc_code_normalize("MT:Y.K9042C00KA0648G00", out, sizeof(out)) == MC_CODE_QR && !strcmp(out, "MT:Y.K9042C00KA0648G00"), "QR 原样");
    CHECK(mc_code_normalize(" MT:Y.K9042C00KA0648G00 ", out, sizeof(out)) == MC_CODE_QR, "QR 去首尾空白");
    CHECK(mc_code_normalize("MT:Y.K90 42C00", out, sizeof(out)) == 0, "QR 中间有空格：拒");
    CHECK(mc_code_normalize("MT:y.k9042", out, sizeof(out)) == 0, "QR 小写：拒（Base38 只有大写）");
    CHECK(mc_code_normalize("MT:", out, sizeof(out)) == 0, "只有前缀：拒");
    CHECK(mc_code_normalize("MT:A:B", out, sizeof(out)) == 0, "第二个冒号：拒");
    CHECK(mc_code_normalize("MT:AAAA*BBBB", out, sizeof(out)) == MC_CODE_QR, "多设备分隔符 *");
    char big[200]; memset(big, 'A', sizeof(big)); memcpy(big, "MT:", 3); big[199] = 0;
    CHECK(mc_code_normalize(big, out, sizeof(out)) == 0, "QR 太长：拒");
    CHECK(mc_code_normalize("34970112332", out, 5) == 0, "输出缓冲太小：拒");
    CHECK(mc_code_normalize(NULL, out, sizeof(out)) == 0 && mc_code_normalize("1", NULL, 4) == 0, "空指针");

    CHECK(mc_code_disc_match(false, 960, "960"), "长 discriminator 相等");
    CHECK(!mc_code_disc_match(false, 960, "961"), "长的差 1");
    CHECK(mc_code_disc_match(true, 960 >> 8, "960") && mc_code_disc_match(true, 3, "1023"), "短的比高 4 位（960 → 3，1023 → 3）");
    CHECK(!mc_code_disc_match(true, 3, "1024"), "短的：1024 的高 4 位是 4");
    CHECK(!mc_code_disc_match(false, 1, "") && !mc_code_disc_match(false, 1, NULL) && !mc_code_disc_match(false, 1, "1x"), "D 解析不了");
    CHECK(!mc_code_disc_match(false, 4096 & 0xFFF, "4096"), "D 超过 12 位：不认");
}

int main(void)
{
    test_nodes();
    test_code();
    printf("mc_nodes / mc_code: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
