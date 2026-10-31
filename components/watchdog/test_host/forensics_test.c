/* 层 G 取证层的主机侧测试
 *
 * 自愈滑动窗口是 3600 秒——在板子上验一遍要跑一小时且需要真重启三次。
 * 这里时间是变量，几微秒验完，还能覆盖"窗口刚好越界"这种边界。
 */

#include "stub/test_common.h"
WD_TEST_GLOBALS

#include "../wd_forensics.c"

/* wd_forensics.c 会读分配失败计数（由 wd_resource.c 提供），这里给个假的 */
static uint32_t g_fake_fails = 0;
void watchdog_get_alloc_failures(uint32_t *c, uint32_t *s, uint32_t *p)
{
    if (c) *c = g_fake_fails;
    if (s) *s = 0;
    if (p) *p = 0;
}

static void reset(void)
{
    memset(&s_rtc, 0, sizeof(s_rtc));
    memset(&s_hist, 0, sizeof(s_hist));
    s_last_was_self_heal = false;
    g_nvs_len = 0; g_nvs_fail = 0; g_fake_fails = 0;
    memset(g_nvs_blob, 0, sizeof(g_nvs_blob));
    SET_MS(0);
}

// ============================================================

static void test_cold_boot(void)
{
    CASE("上电首次：RTC 是随机值，应被识别并清零");
    reset();
    s_rtc.magic = 0xDEADBEEF;               /* 模拟未初始化的 RTC RAM */
    s_rtc.self_heal_count = 99;
    s_rtc.safe_mode = 1;

    wd_forensics_init();
    CHECK(s_rtc.magic == WD_RTC_MAGIC, "应写入 magic");
    CHECK(s_rtc.self_heal_count == 0, "应清零自愈计数，实际 %u", s_rtc.self_heal_count);
    CHECK(wd_forensics_safe_mode() == false, "冷启动不应处于安全模式");
    CHECK(wd_forensics_last_was_self_heal(NULL) == false, "冷启动不应报自愈");
}

static void test_self_heal_detection(void)
{
    CASE("软复位后能识别出上次是自愈重启，且只报一次");
    reset();
    wd_forensics_init();
    SET_MS(40000);
    wd_forensics_record(WD_FAULT_TASK_STALL, WD_LEVEL_REBOOT, "health");

    /* 模拟软复位：RTC 保留，静态标志重置 */
    s_last_was_self_heal = false;
    wd_forensics_init();

    wd_forensic_rec_t rec;
    CHECK(wd_forensics_last_was_self_heal(&rec) == true, "应识别为自愈重启");
    CHECK(strcmp(rec.detail, "health") == 0, "应带上 detail，实际 %s", rec.detail);
    CHECK(rec.uptime_s == 40, "应记录事发 uptime，实际 %u", rec.uptime_s);

    /* 再软复位一次，但这次不是自愈 */
    s_last_was_self_heal = false;
    wd_forensics_init();
    CHECK(wd_forensics_last_was_self_heal(NULL) == false,
          "标记应已被消费，不能连报两次");
}

static void test_window_accumulates(void)
{
    CASE("自愈窗口累加运行时长，未越界时计数递增");
    reset();
    wd_forensics_init();

    SET_MS(600 * 1000);                     /* 运行 600s 后自愈 */
    wd_forensics_record(WD_FAULT_TASK_STALL, WD_LEVEL_REBOOT, "a");
    CHECK(s_rtc.self_heal_count == 1, "第 1 次，实际 %u", s_rtc.self_heal_count);
    CHECK(s_rtc.window_runtime_s == 600, "窗口累计 600s，实际 %u", s_rtc.window_runtime_s);

    SET_MS(300 * 1000);                     /* 重启后又跑 300s */
    wd_forensics_record(WD_FAULT_TASK_STALL, WD_LEVEL_REBOOT, "b");
    CHECK(s_rtc.self_heal_count == 2, "第 2 次");
    CHECK(s_rtc.window_runtime_s == 900, "窗口累计 900s，实际 %u", s_rtc.window_runtime_s);
    CHECK(wd_forensics_safe_mode() == false, "2 次还不该进安全模式");
}

static void test_safe_mode_threshold(void)
{
    CASE("窗口内第 3 次自愈 → 进安全模式");
    reset();
    wd_forensics_init();
    for (int i = 0; i < 3; i++) {
        SET_MS(100 * 1000);
        wd_forensics_record(WD_FAULT_TASK_STALL, WD_LEVEL_REBOOT, "health");
    }
    CHECK(s_rtc.self_heal_count == 3, "应累计 3 次，实际 %u", s_rtc.self_heal_count);
    CHECK(wd_forensics_safe_mode() == true, "第 3 次应触发安全模式");
}

static void test_window_rollover(void)
{
    CASE("累计运行超过 1 小时 → 开新窗口，计数重置（偶发故障不该攒成安全模式）");
    reset();
    wd_forensics_init();

    SET_MS(1800 * 1000);
    wd_forensics_record(WD_FAULT_TASK_STALL, WD_LEVEL_REBOOT, "a");
    CHECK(s_rtc.self_heal_count == 1 && s_rtc.window_runtime_s == 1800, "第 1 次");

    SET_MS(1700 * 1000);                    /* 1800+1700=3500 < 3600，仍在窗口内 */
    wd_forensics_record(WD_FAULT_TASK_STALL, WD_LEVEL_REBOOT, "b");
    CHECK(s_rtc.self_heal_count == 2 && s_rtc.window_runtime_s == 3500,
          "3500s 仍在窗口内，实际 count=%u runtime=%u",
          s_rtc.self_heal_count, s_rtc.window_runtime_s);

    SET_MS(200 * 1000);                     /* 3500+200=3700 > 3600 → 开新窗口 */
    wd_forensics_record(WD_FAULT_TASK_STALL, WD_LEVEL_REBOOT, "c");
    CHECK(s_rtc.self_heal_count == 1, "越界应重置为 1，实际 %u", s_rtc.self_heal_count);
    CHECK(s_rtc.window_runtime_s == 0, "越界应重置窗口，实际 %u", s_rtc.window_runtime_s);
    CHECK(wd_forensics_safe_mode() == false, "跨窗口的偶发故障不应进安全模式");
}

static void test_history_only_l3_plus(void)
{
    CASE("只有 L3/L4 写历史（L1 告警可能每分钟一条，会磨损 Flash）");
    reset();
    wd_forensics_init();
    wd_forensics_record(WD_FAULT_RESOURCE, WD_LEVEL_LOG, "warn");
    wd_forensics_record(WD_FAULT_RESOURCE, WD_LEVEL_TARGET, "l2");
    CHECK(s_hist.count == 0, "L1/L2 不应写历史，实际 %u 条", s_hist.count);

    wd_forensics_record(WD_FAULT_RESOURCE, WD_LEVEL_SUBSYS, "l3");
    CHECK(s_hist.count == 1, "L3 应写历史");
    wd_forensics_record(WD_FAULT_RESOURCE, WD_LEVEL_REBOOT, "l4");
    CHECK(s_hist.count == 2, "L4 应写历史");
}

static void test_history_ring_wrap(void)
{
    CASE("环形历史绕回：保留最新 N 条，最新在前");
    reset();
    wd_forensics_init();
    char names[WD_HISTORY_MAX + 3][8];
    for (int i = 0; i < WD_HISTORY_MAX + 3; i++) {
        snprintf(names[i], sizeof(names[i]), "r%d", i);
        SET_MS((i + 1) * 1000);
        wd_forensics_record(WD_FAULT_TASK_STALL, WD_LEVEL_SUBSYS, names[i]);
    }
    CHECK(s_hist.count == WD_HISTORY_MAX, "应封顶在 %d 条，实际 %u",
          WD_HISTORY_MAX, s_hist.count);

    wd_forensic_rec_t out[WD_HISTORY_MAX];
    size_t n = wd_forensics_history(out, WD_HISTORY_MAX);
    CHECK(n == WD_HISTORY_MAX, "应返回 %d 条", WD_HISTORY_MAX);
    CHECK(strcmp(out[0].detail, "r10") == 0, "最新一条应是 r10，实际 %s", out[0].detail);
    CHECK(out[0].uptime_s > out[1].uptime_s, "应按时间倒序");
    CHECK(strcmp(out[WD_HISTORY_MAX - 1].detail, "r3") == 0,
          "最老一条应是 r3，实际 %s", out[WD_HISTORY_MAX - 1].detail);
}

static void test_history_survives_reboot(void)
{
    CASE("历史经 NVS 跨重启保留");
    reset();
    wd_forensics_init();
    SET_MS(5000);
    wd_forensics_record(WD_FAULT_RESOURCE, WD_LEVEL_SUBSYS, "persisted");

    memset(&s_hist, 0, sizeof(s_hist));     /* 模拟重启：RAM 没了，NVS 还在 */
    wd_forensics_init();
    CHECK(s_hist.count == 1, "重启后应从 NVS 读回，实际 %u 条", s_hist.count);

    wd_forensic_rec_t out[WD_HISTORY_MAX];
    wd_forensics_history(out, WD_HISTORY_MAX);
    CHECK(strcmp(out[0].detail, "persisted") == 0, "内容应一致，实际 %s", out[0].detail);
}

static void test_corrupt_nvs(void)
{
    CASE("NVS 内容损坏/长度不符时安全降级，不越界");
    reset();
    /* 长度对但 head 越界 */
    wd_hist_blob_t bad = {0};
    bad.head = 200; bad.count = 3;
    memcpy(g_nvs_blob, &bad, sizeof(bad));
    g_nvs_len = sizeof(bad);
    wd_forensics_init();
    CHECK(s_hist.head < WD_HISTORY_MAX, "损坏的 head 应被重置，实际 %u", s_hist.head);
    CHECK(s_hist.count == 0, "损坏时应整体清零");

    /* 长度不符（换过版本） */
    reset();
    g_nvs_len = 7;
    wd_forensics_init();
    CHECK(s_hist.count == 0, "长度不符应清零");

    /* NVS 完全不可用 */
    reset();
    g_nvs_fail = 1;
    wd_forensics_init();
    wd_forensics_record(WD_FAULT_TASK_STALL, WD_LEVEL_SUBSYS, "x");
    CHECK(s_hist.count == 1, "NVS 不可用时内存里仍应有记录（诊断不能因此瞎掉）");
}

static void test_record_before_init(void)
{
    CASE("init 之前上报不得污染 RTC 记录");
    reset();
    s_rtc.magic = 0x12345678;               /* 未初始化 */
    SET_MS(1000);
    wd_forensics_record(WD_FAULT_TASK_STALL, WD_LEVEL_REBOOT, "early");
    CHECK(s_rtc.magic == WD_RTC_MAGIC, "应就地补初始化");
    CHECK(s_rtc.self_heal_count == 1, "计数应从干净状态开始，实际 %u",
          s_rtc.self_heal_count);
}

static void test_snapshot_and_truncation(void)
{
    CASE("快照写入 + 超长 detail 截断不溢出");
    reset();
    wd_forensics_init();
    g_fake_internal_free = 12345; g_fake_dma_free = 6789; g_fake_fails = 42;
    SET_MS(9000);
    wd_forensics_record(WD_FAULT_RESOURCE, WD_LEVEL_SUBSYS,
                        "这是一个非常长的故障描述字符串用来验证截断行为");

    wd_forensic_rec_t out[1];
    wd_forensics_history(out, 1);
    CHECK(out[0].internal_free == 12345, "应记录内部堆快照，实际 %u", out[0].internal_free);
    CHECK(out[0].dma_free == 6789, "应记录 DMA 快照");
    CHECK(out[0].alloc_fails == 42, "应记录分配失败数");
    CHECK(strlen(out[0].detail) < sizeof(out[0].detail), "detail 必须以 NUL 结尾且不溢出");
    g_fake_internal_free = 20000; g_fake_dma_free = 15000;
}

static void test_null_args(void)
{
    CASE("空参数不崩");
    reset();
    wd_forensics_init();
    CHECK(wd_forensics_history(NULL, 5) == 0, "NULL out 应返回 0");
    wd_forensic_rec_t out[1];
    CHECK(wd_forensics_history(out, 0) == 0, "max=0 应返回 0");
    wd_forensics_record(WD_FAULT_TASK_STALL, WD_LEVEL_SUBSYS, NULL);
    CHECK(s_hist.count == 1, "NULL detail 应被替换而不是崩");
}

int main(void)
{
    printf("=== 层 G 取证 ===\n");
    test_cold_boot();
    test_self_heal_detection();
    test_window_accumulates();
    test_safe_mode_threshold();
    test_window_rollover();
    test_history_only_l3_plus();
    test_history_ring_wrap();
    test_history_survives_reboot();
    test_corrupt_nvs();
    test_record_before_init();
    test_snapshot_and_truncation();
    test_null_args();
    return wd_test_summary("forensics");
}
