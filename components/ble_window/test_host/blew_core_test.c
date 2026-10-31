/* blew_core 的真值表（docs/15 设计 3.1、3.2）。编的是产品源文件 ../blew_core.c。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blew_core.h"

static int s_fail, s_pass;
#define CHECK(cond, ...) do { if (cond) { s_pass++; } else { s_fail++; printf("❌ %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_boot_decide(void)
{
    uint32_t m = 0, inv = 0;
    blew_boot_arm(&m, &inv);
    CHECK(m == BLEW_PAIR_MAGIC && inv == (uint32_t)~BLEW_PAIR_MAGIC, "arm 写出 magic 与取反");

    CHECK(blew_boot_decide(m, inv, true, false), "合法魔数 + 软件复位 + 非安全模式 → 配对剖面");
    CHECK(!blew_boot_decide(m, inv, false, false), "合法魔数但不是软件复位（断电 / panic / 看门狗）→ 正常启动（TC-MU04）");
    CHECK(!blew_boot_decide(m, inv, true, true), "安全模式优先：有安全模式标志就不进配对剖面");
    CHECK(!blew_boot_decide(0, 0, true, false), "RTC 清零 → 正常");
    CHECK(!blew_boot_decide(m, m, true, false), "取反不对 → 正常（上电随机值）");
    CHECK(!blew_boot_decide(~m, inv, true, false), "magic 不对 → 正常");
    CHECK(!blew_boot_decide(m, inv ^ 1u, true, false), "取反差一位 → 正常");
    CHECK(!blew_boot_decide(0xFFFFFFFFu, 0x00000000u, true, false), "全 1 / 全 0 → 正常");
    blew_boot_arm(NULL, &inv);   /* 不崩 */
    blew_boot_arm(&m, NULL);
}

static void test_req_check(void)
{
    const int MP = BLEW_CORE_OWNER_MUSE_PAIR, MT = BLEW_CORE_OWNER_MATTER;
    const int PER = BLEW_CORE_ROLE_PERIPHERAL, CEN = BLEW_CORE_ROLE_CENTRAL;

    CHECK(blew_req_check(MP, PER, 180000, false, true) == BLEW_REQ_OK, "配对剖面里开 Muse 外设窗口 → OK");
    CHECK(blew_req_check(MP, PER, BLEW_MUSE_MAX_MS, false, true) == BLEW_REQ_OK, "正好 600 s → OK");
    CHECK(blew_req_check(MP, PER, BLEW_MUSE_MAX_MS + 1, false, true) == BLEW_REQ_BAD_ARG, "超过 600 s → BAD_ARG");
    CHECK(blew_req_check(MP, PER, 0, false, true) == BLEW_REQ_BAD_ARG, "max_ms = 0 → BAD_ARG");
    CHECK(blew_req_check(0, PER, 1000, false, true) == BLEW_REQ_BAD_ARG, "owner 0 → BAD_ARG");
    CHECK(blew_req_check(3, PER, 1000, false, true) == BLEW_REQ_BAD_ARG, "owner 3 → BAD_ARG");
    CHECK(blew_req_check(-1, PER, 1000, false, true) == BLEW_REQ_BAD_ARG, "owner -1 → BAD_ARG");
    CHECK(blew_req_check(MP, 2, 1000, false, true) == BLEW_REQ_BAD_ARG, "role 2 → BAD_ARG");
    CHECK(blew_req_check(MP, -1, 1000, false, true) == BLEW_REQ_BAD_ARG, "role -1 → BAD_ARG");
    CHECK(blew_req_check(MP, PER, 180000, true, true) == BLEW_REQ_BUSY, "已有窗口 → BUSY");
    CHECK(blew_req_check(MP, PER, 180000, false, false) == BLEW_REQ_NOT_SUPPORTED,
          "策略 C 下正常开机开窗口 → NOT_SUPPORTED（BT 内存已释放）");
    CHECK(blew_req_check(MP, CEN, 1000, false, true) == BLEW_REQ_NOT_SUPPORTED, "中心角色 → NOT_SUPPORTED（P3）");
    CHECK(blew_req_check(MT, CEN, 60000, false, true) == BLEW_REQ_NOT_SUPPORTED, "Matter → NOT_SUPPORTED（P3）");
    CHECK(blew_req_check(MT, CEN, BLEW_MATTER_MAX_MS + 1, false, true) == BLEW_REQ_BAD_ARG,
          "Matter 超过 120 s → BAD_ARG（参数先于能力判）");
    CHECK(blew_req_check(MP, PER, 180000, true, false) == BLEW_REQ_BUSY, "BUSY 先于 NOT_SUPPORTED");
}

static void test_mem_gate(void)
{
    CHECK(blew_mem_gate(44032, 4096, 44032, 4096), "正好够 → 开");
    CHECK(!blew_mem_gate(44031, 4096, 44032, 4096), "内部差 1 字节 → 不开");
    CHECK(!blew_mem_gate(100000, 4095, 44032, 4096), "DMA 差 1 字节 → 不开");
    CHECK(!blew_mem_gate(0, 0, 1, 1), "全空 → 不开");
}

static blew_sc_input_t sc(uint32_t br, uint32_t ba, uint8_t cb, uint32_t ar, uint32_t aa, uint8_t ca)
{
    blew_sc_input_t in = { .base_req = br, .base_acked = ba, .children_before = cb,
                           .after_req = ar, .after_acked = aa, .children_after = ca, .judge_children = true };
    return in;
}

static void test_selfcheck(void)
{
    blew_sc_input_t in;
    in = sc(1000, 950, 4, 100, 90, 4);
    CHECK(blew_sc_verdict(&in) == BLEW_SC_PASS, "基线 0.95、之后 0.90（≥ 0.76）、子节点回齐 → PASS");
    in = sc(1000, 950, 4, 100, 75, 4);
    CHECK(blew_sc_verdict(&in) == BLEW_SC_DEGRADED_ACK, "之后 0.75 < 0.8×0.95=0.76 → ACK 退化");
    in = sc(1000, 950, 4, 100, 76, 4);
    CHECK(blew_sc_verdict(&in) == BLEW_SC_PASS, "正好 0.76 → PASS（边界含等号）");
    in = sc(1000, 400, 0, 100, 50, 0);
    CHECK(blew_sc_verdict(&in) == BLEW_SC_PASS, "基线本来就差（0.4）：门槛取绝对下限 0.5，之后 0.5 → PASS");
    in = sc(1000, 400, 0, 100, 49, 0);
    CHECK(blew_sc_verdict(&in) == BLEW_SC_DEGRADED_ACK, "之后 0.49 < 0.5 → 退化（不因基线差就放水）");
    in = sc(5, 5, 0, 100, 60, 0);
    CHECK(blew_sc_verdict(&in) == BLEW_SC_PASS, "基线帧数不够：门槛 0.5，之后 0.6 → PASS");
    in = sc(1000, 950, 3, 5, 0, 3);
    CHECK(blew_sc_verdict(&in) == BLEW_SC_PASS, "观察窗帧数不够：不判 ACK，只看子节点（回齐）→ PASS");
    in = sc(1000, 950, 10, 100, 95, 8);
    CHECK(blew_sc_verdict(&in) == BLEW_SC_DEGRADED_CHILDREN, "子节点 8/10 < 90%% → 子节点退化");
    in = sc(1000, 950, 10, 100, 95, 9);
    CHECK(blew_sc_verdict(&in) == BLEW_SC_PASS, "子节点 9/10 = 90%% → PASS");
    in = sc(1000, 950, 3, 100, 95, 2);
    CHECK(blew_sc_verdict(&in) == BLEW_SC_DEGRADED_CHILDREN, "子节点 2/3（67%%）→ 退化（3 个时要全回来）");
    in = sc(1000, 950, 10, 100, 10, 5);
    CHECK(blew_sc_verdict(&in) == BLEW_SC_DEGRADED_BOTH, "两样都差 → BOTH");
    in = sc(0, 0, 0, 3, 3, 0);
    CHECK(blew_sc_verdict(&in) == BLEW_SC_INCONCLUSIVE, "没流量也没子节点 → 判不了");
    CHECK(blew_sc_verdict(NULL) == BLEW_SC_INCONCLUSIVE, "NULL");
    in = sc(0xFFFFFFFFu, 0xFFFFFFF0u, 1, 0xFFFFFFFFu, 0xFFFFFFFFu, 1);
    CHECK(blew_sc_verdict(&in) == BLEW_SC_PASS, "大数不溢出（乘 1000 走 64 位）");
    CHECK(blew_permille(1, 0) == 0 && blew_permille(5, 4) == 1000 && blew_permille(1, 3) == 333, "千分数");
    for (int i = 0; i < BLEW_SC_MAX; i++) CHECK(strcmp(blew_sc_str((blew_sc_result_t)i), "unknown") != 0, "结果 %d 有名字", i);
    CHECK(BLEW_SC_PASS == 1 && BLEW_SC_NO_THREAD == 6 && BLEW_SC_INTERRUPTED == 7, "编号钉死（进 flash，只许追加）");
    /* 边界：最少帧数 20 两侧 */
    in = sc(1000, 950, 0, 19, 0, 0);
    CHECK(blew_sc_verdict(&in) == BLEW_SC_INCONCLUSIVE, "观察窗 19 帧：不判 ACK（没子节点 → 判不了）");
    in = sc(1000, 950, 0, 20, 0, 0);
    CHECK(blew_sc_verdict(&in) == BLEW_SC_DEGRADED_ACK, "观察窗 20 帧：判 ACK（0/20 → 退化）");
    in = sc(19, 19, 0, 100, 51, 0);
    CHECK(blew_sc_verdict(&in) == BLEW_SC_PASS, "基线 19 帧：门槛 0.5（0.51 过）");
    in = sc(20, 20, 0, 100, 51, 0);
    CHECK(blew_sc_verdict(&in) == BLEW_SC_DEGRADED_ACK, "基线 20 帧：门槛 0.8（0.51 不过）");
    in = sc(1000, 950, 5, 5, 0, 2);
    CHECK(blew_sc_verdict(&in) == BLEW_SC_DEGRADED_CHILDREN, "ACK 判不了、子节点退化 → 子节点退化");
    in = sc(1000, 950, 1, 100, 95, 0);
    CHECK(blew_sc_verdict(&in) == BLEW_SC_DEGRADED_CHILDREN, "1 个子节点没回来 → 退化");
    /* 策略 C：子节点只记录不判 */
    in = sc(1000, 950, 10, 100, 95, 0); in.judge_children = false;
    CHECK(blew_sc_verdict(&in) == BLEW_SC_PASS, "策略 C：子节点 0/10（改挂别的路由器）不算退化，ACK 正常就过");
    in = sc(1000, 950, 10, 100, 10, 0); in.judge_children = false;
    CHECK(blew_sc_verdict(&in) == BLEW_SC_DEGRADED_ACK, "策略 C：ACK 掉了照样判退化");
    in = sc(1000, 950, 10, 5, 0, 0); in.judge_children = false;
    CHECK(blew_sc_verdict(&in) == BLEW_SC_INCONCLUSIVE, "策略 C：没流量 → 判不了（子节点不顶替）");
}

static void test_res_blob(void)
{
    blew_res_t a, b;
    blew_sc_input_t in = sc(1000, 950, 4, 100, 70, 4);
    blew_res_update(&a, NULL, BLEW_SC_DEGRADED_ACK, &in, 1761100000u);
    CHECK(a.total == 1 && a.consecutive_degraded == 1 && a.before_pm == 950 && a.after_pm == 700, "第一次：退化计 1");
    blew_res_update(&b, &a, BLEW_SC_DEGRADED_BOTH, &in, 0);
    CHECK(b.total == 2 && b.consecutive_degraded == 2, "连续两次退化（设计 3.3：连续两次提示改策略）");
    blew_res_update(&a, &b, BLEW_SC_INCONCLUSIVE, NULL, 0);
    CHECK(a.total == 3 && a.consecutive_degraded == 2, "判不了：连续计数不动");
    blew_res_update(&b, &a, BLEW_SC_PASS, &in, 0);
    CHECK(b.consecutive_degraded == 0 && b.last == BLEW_SC_PASS, "通过清零");
    blew_sc_input_t zero = sc(1000, 950, 4, 0, 0, 0);
    blew_res_update(&a, &b, BLEW_SC_NO_THREAD, &zero, 0);
    CHECK(a.after_pm == b.after_pm && a.children_after == b.children_after && a.last == BLEW_SC_NO_THREAD,
          "判不了的不覆盖上一次判过的数值（不显示成 950‰ → 0‰）");
    blew_res_update(&a, &b, BLEW_SC_INTERRUPTED, NULL, 0);
    CHECK(a.consecutive_degraded == b.consecutive_degraded && a.total == b.total + 1, "被打断：只计次数");
    uint8_t blob[BLEW_RES_BLOB_LEN];
    blew_res_t c;
    a.epoch = 0x01020304u; a.children_after = 7;
    blew_res_encode(&a, blob);
    CHECK(blew_res_decode(blob, sizeof(blob), &c) && memcmp(&a, &c, sizeof(a)) == 0, "编解码往返");
    CHECK(!blew_res_decode(blob, 13, &c), "短了");
    blob[0] = BLEW_SC_MAX;
    CHECK(!blew_res_decode(blob, sizeof(blob), &c), "不认识的新结果编号当作没有");
    CHECK(BLEW_RES_BLOB_LEN <= 63, "blob ≤ 64 B（含版本字节，设计 4.4）");
}

int main(void)
{
    test_selfcheck();
    test_res_blob();
    test_boot_decide();
    test_req_check();
    test_mem_gate();
    printf("%s blew_core：%d 通过，%d 失败\n", s_fail ? "❌" : "✅", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
