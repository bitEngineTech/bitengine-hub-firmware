/* hub_jobs_core（6.6 作业表）的主机测试：开 / 结束 / 超时、在途上限、复用最早结束的格、迟到的完成不覆盖、id 不回到 0。 bash run.sh */
#include <stdio.h>
#include <string.h>
#include "hub_jobs_core.h"

static int pass, fail;
#define CHECK(c, what) do { if (c) pass++; else { fail++; printf("❌ %s（%s:%d）\n", what, __FILE__, __LINE__); } } while (0)

int main(void)
{
    hjc_table_t t;
    hjc_init(&t);
    const uint32_t a = hjc_start(&t, "hub.matter.commission", 0, 1000, 180000);
    CHECK(a == 1 && hjc_get(&t, a)->state == HJC_RUNNING && !strcmp(hjc_get(&t, a)->cmd, "hub.matter.commission"), "开第一个：id 1、RUNNING");
    CHECK(hjc_inflight(&t) == 1, "在途 1");
    CHECK(hjc_finish(&t, a, 5000, NULL, NULL, "{\"id\":\"mt-0000000000000002\"}") && hjc_get(&t, a)->state == HJC_DONE &&
          !strcmp(hjc_get(&t, a)->result, "{\"id\":\"mt-0000000000000002\"}"), "成功结束带结果体");
    CHECK(!hjc_finish(&t, a, 6000, "internal", "x", NULL) && hjc_get(&t, a)->state == HJC_DONE, "结束过的不能再结束");
    const uint32_t b = hjc_start(&t, "x", 1, 6000, 1000);
    CHECK(hjc_finish(&t, b, 6100, NULL, NULL, NULL) && !strcmp(hjc_get(&t, b)->result, "{}"), "没结果体 = {}");
    const uint32_t c = hjc_start(&t, "y", 1, 7000, 1000);
    CHECK(hjc_finish(&t, c, 7100, "not_found", "The hub could not find the device.", NULL) && hjc_get(&t, c)->state == HJC_FAILED &&
          !strcmp(hjc_get(&t, c)->err_code, "not_found") && hjc_get(&t, c)->result[0] == 0, "失败带码与句子、没有结果体");

    /* 超时：过期限的改 TIMEOUT，迟到的完成不覆盖 */
    const uint32_t d = hjc_start(&t, "z", 0, 10000, 5000);
    uint32_t ids[4];
    CHECK(hjc_expire(&t, 14999, ids, 4) == 0, "期限前不超时");
    CHECK(hjc_expire(&t, 15000, ids, 4) == 1 && ids[0] == d && hjc_get(&t, d)->state == HJC_TIMEOUT && !strcmp(hjc_get(&t, d)->err_code, "timeout"),
          "到期限改 TIMEOUT、报 id");
    CHECK(!hjc_finish(&t, d, 15100, NULL, NULL, "{}") && hjc_get(&t, d)->state == HJC_TIMEOUT, "超时之后迟到的完成不覆盖");
    CHECK(hjc_expire(&t, 99999, ids, 4) == 0, "超时只报一次");

    /* 在途上限 4 */
    hjc_init(&t);
    uint32_t r[5];
    for (int i = 0; i < 4; i++) r[i] = hjc_start(&t, "c", 0, 100, 1000);
    CHECK(r[0] && r[1] && r[2] && r[3] && hjc_inflight(&t) == 4, "在途 4 个");
    CHECK(hjc_start(&t, "c", 0, 100, 1000) == 0, "第 5 个在途：拒");
    CHECK(hjc_finish(&t, r[0], 200, NULL, NULL, NULL), "结束一个");
    r[4] = hjc_start(&t, "c", 0, 300, 1000);
    CHECK(r[4] != 0, "结束一个之后能再开");

    /* 表满（6 格）时复用最早结束的那一格 */
    hjc_init(&t);
    uint32_t q[6];
    for (int i = 0; i < 6; i++) { q[i] = hjc_start(&t, "q", 0, (uint32_t)(100 * i), 1000); hjc_finish(&t, q[i], (uint32_t)(100 * i + 10), NULL, NULL, NULL); }
    const uint32_t q7 = hjc_start(&t, "q", 0, 1000, 1000);
    CHECK(q7 != 0 && hjc_get(&t, q[0]) == NULL && hjc_get(&t, q[1]) != NULL, "满表复用最早结束的（q0 被挤掉、q1 还在）");

    /* 时钟回绕：期限按差值算 */
    hjc_init(&t);
    const uint32_t w = hjc_start(&t, "w", 0, 0xFFFFFF00u, 0x200);
    CHECK(hjc_expire(&t, 0x50, ids, 4) == 0 && hjc_expire(&t, 0x100, ids, 4) == 1 && ids[0] == w, "毫秒计数回绕时期限照算");

    /* id 不回到 0 */
    hjc_init(&t);
    t.next_id = 0xFFFFFFFFu;
    const uint32_t m = hjc_start(&t, "m", 0, 0, 10), m2 = hjc_start(&t, "m", 0, 0, 10);
    CHECK(m == 0xFFFFFFFFu && m2 == 1, "id 回绕跳过 0");

    /* 放弃：格子空出来、不留结论；只能放弃在跑的 */
    hjc_init(&t);
    const uint32_t ab = hjc_start(&t, "a", 0, 0, 1000);
    CHECK(hjc_abandon(&t, ab) && hjc_get(&t, ab) == NULL && hjc_inflight(&t) == 0, "放弃：那一格回到空闲、查不到");
    CHECK(!hjc_abandon(&t, ab), "放弃过的不能再放弃");
    const uint32_t ab2 = hjc_start(&t, "a", 0, 0, 1000);
    hjc_finish(&t, ab2, 1, NULL, NULL, NULL);
    CHECK(!hjc_abandon(&t, ab2) && hjc_get(&t, ab2)->state == HJC_DONE, "结束了的不能放弃（结论留着）");
    hjc_init(&t);
    uint32_t keep[6];
    for (int i = 0; i < 6; i++) { keep[i] = hjc_start(&t, "k", 0, (uint32_t)i, 1000); hjc_finish(&t, keep[i], (uint32_t)i + 1, NULL, NULL, NULL); }
    const uint32_t tmp = hjc_start(&t, "x", 0, 100, 1000);
    hjc_abandon(&t, tmp);
    CHECK(hjc_get(&t, keep[1]) != NULL && hjc_get(&t, keep[0]) == NULL, "放弃的那次挤掉了最旧的一格（开的时候就挤了）");
    const uint32_t tmp2 = hjc_start(&t, "x", 0, 101, 1000);
    CHECK(tmp2 && hjc_get(&t, keep[1]) != NULL, "再开一个用的是放弃空出来的格子，不再挤别人");

    /* 参数 / 截短 */
    CHECK(hjc_start(&t, "", 0, 0, 10) == 0 && hjc_start(&t, NULL, 0, 0, 10) == 0 && hjc_start(&t, "x", 0, 0, 0) == 0, "坏参数：拒");
    hjc_init(&t);
    char longs[600]; memset(longs, 'a', sizeof(longs)); longs[599] = 0;
    const uint32_t l = hjc_start(&t, longs, 0, 0, 10);
    CHECK(l && strlen(hjc_get(&t, l)->cmd) == HJC_CMD_MAX - 1, "命令名截短");
    CHECK(hjc_finish(&t, l, 1, "internal", longs, NULL) && strlen(hjc_get(&t, l)->err_msg) == HJC_MSG_MAX - 1, "句子截短");
    CHECK(hjc_get(&t, 0) == NULL && hjc_get(&t, 12345) == NULL, "id 0 / 不存在：NULL");
    CHECK(!strcmp(hjc_state_str(HJC_RUNNING), "running") && !strcmp(hjc_state_str(HJC_TIMEOUT), "timeout"), "状态名");

    printf("hub_jobs: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
