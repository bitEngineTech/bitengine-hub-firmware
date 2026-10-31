/* srp_readvertise 的跟踪表与缓冲区分配单元测试。
 *
 * 跟踪表决定"哪个服务已经重新宣告过"。它错了的后果是静默的：要么某个设备
 * 永远不被处理（控制器一直等超时），要么每轮都重复处理（订阅反复重建）。
 * 这两种都不会报错，只能靠断言锁住。
 *
 * 另外验 PSRAM 分配：缓冲区曾经是静态数组，占 2.8KB 内部 SRAM，现改为
 * 启动时从 PSRAM 取。这里锁住"只分配一次"和"PSRAM 取不到要回退且仍能工作"。
 */

#include "stub/test_common.h"
SRP_TEST_GLOBALS

#include "../srp_readvertise.c"

/* 每个用例前把模块恢复到未初始化状态 */
static void reset_module(void)
{
    free(s_track); free(s_svc); free(s_orphan); free(s_txt_items); free(s_txt_scratch); free(s_idx);
    s_track = NULL; s_svc = NULL; s_orphan = NULL; s_idx = NULL; s_idx_n = 0; s_free_hint = 0;
    s_txt_items = NULL; s_txt_scratch = NULL;
    s_task = NULL; s_task_run = false; s_force = false;
    s_orphan_valid = false; s_ot_gate = NULL;
    s_stat_readv = s_stat_fail = 0;
#if CONFIG_BITENGINE_SRP_READV_FORCE_BYE
    /* 强制 bye 于 2026-08-15 改为可选且默认关（487386d），这个时间戳随之进了
       条件编译。run.sh 会把本文件在开/关两种构型下各编一遍 —— 不然默认关的
       构型下，FORCE_BYE 那 70 余行分支连编译器都看不到。 */
    s_last_add_us = 0;
#endif
    g_fake_spiram_fail = 0;
    g_fake_task_create_ok = 1;
}

static void test_alloc(void)
{
    CASE("start 后缓冲区全部就位");
    reset_module();
    CHECK(srp_readv_start() == ESP_OK, "start 应成功");
    CHECK(s_track && s_svc && s_orphan && s_txt_items && s_txt_scratch,
          "五块缓冲区都应分配");
    CHECK(s_ot_gate != NULL, "闸门应创建");

    CASE("重复 start 幂等，不重复分配");
    void *first = s_track;
    CHECK(srp_readv_start() == ESP_OK, "重复 start 应返回 OK");
    CHECK(s_track == first, "不应重新分配（否则每次 start 泄漏一份）");

    CASE("PSRAM 取不到时回退到默认堆，功能不退化");
    reset_module();
    g_fake_spiram_fail = 1;
    CHECK(srp_readv_start() == ESP_OK, "PSRAM 失败也应能启动");
    CHECK(s_track && s_txt_scratch, "回退后缓冲区仍应可用");

    CASE("建任务失败时 start 报错");
    reset_module();
    g_fake_task_create_ok = 0;
    CHECK(srp_readv_start() == ESP_ERR_NO_MEM, "任务建不起来必须报错，不能假装成功");

    CASE("统计接口容忍 NULL");
    reset_module();
    srp_readv_get_stats(NULL, NULL, NULL);
    uint32_t a = 99, b = 99, c = 99;
    srp_readv_get_stats(&a, &b, &c);
    CHECK(a == 0 && b == 0 && c == 0, "未启动时应全 0，得到 %u/%u/%u",
          (unsigned)a, (unsigned)b, (unsigned)c);
}

static void test_track_table(void)
{
    reset_module();
    g_fake_task_create_ok = 1;
    CHECK(srp_readv_start() == ESP_OK, "start");

    CASE("新增后能找回");
    track_t *e = track_add("inst1", "_matter", "_tcp");
    CHECK(e != NULL, "应能新增");
    CHECK(track_find("inst1", "_matter", "_tcp") == e, "应找回同一条");

    CASE("三元组任一不同即视为不同服务");
    CHECK(track_find("inst1", "_matter", "_udp") == NULL, "proto 不同不应命中");
    CHECK(track_find("inst1", "_meshcop", "_tcp") == NULL, "type 不同不应命中");
    CHECK(track_find("inst2", "_matter", "_tcp") == NULL, "instance 不同不应命中");

    CASE("新条目初始状态：未处理、计数归零");
    CHECK(e->used && !e->done && e->rounds == 0 && e->misses == 0 && !e->warned,
          "used=%d done=%d rounds=%u misses=%u", e->used, e->done, e->rounds, e->misses);

    CASE("表满时返回 NULL 而不是覆盖别人");
    reset_module();
    CHECK(srp_readv_start() == ESP_OK, "start");
    for (int i = 0; i < MAX_SERVICES; i++) {
        char name[LABEL_LEN];
        snprintf(name, sizeof(name), "inst%02d", i);
        CHECK(track_add(name, "_matter", "_tcp") != NULL, "第 %d 条应能加入", i);
    }
    CHECK(track_add("overflow", "_matter", "_tcp") == NULL, "超出容量必须返回 NULL");
    CHECK(track_find("inst00", "_matter", "_tcp") != NULL, "已有条目不能被挤掉");

    CASE("回收后槽位可复用（设备重新入网要能再次触发）");
    track_t *slot = track_find("inst00", "_matter", "_tcp");
    slot->used = false;                                  /* 模拟本轮没见到被回收 */
    CHECK(track_add("newcomer", "_matter", "_tcp") != NULL, "空出的槽位应可复用");
    CHECK(track_find("newcomer", "_matter", "_tcp") != NULL, "新条目应能找回");
    CHECK(track_find("inst00", "_matter", "_tcp") == NULL, "旧条目不应再被找到");

    CASE("哈希索引：满表时每一条都找得回（2026-10-07 线性查找改成开放寻址）");
    reset_module();
    CHECK(srp_readv_start() == ESP_OK, "start");
    for (int i = 0; i < MAX_SERVICES; i++) {
        char name[LABEL_LEN];
        snprintf(name, sizeof(name), "088E927C6F07E0AC-BE57%012X", i);
        CHECK(track_add(name, "_matter", "_tcp") != NULL, "第 %d 条应能加入", i);
    }
    int found = 0;
    for (int i = 0; i < MAX_SERVICES; i++) {
        char name[LABEL_LEN];
        snprintf(name, sizeof(name), "088E927C6F07E0AC-BE57%012X", i);
        track_t *e = track_find(name, "_matter", "_tcp");
        if (e && strcmp(e->instance, name) == 0) found++;
    }
    CHECK(found == MAX_SERVICES, "满表应全部命中，实际 %d/%d", found, MAX_SERVICES);
    CHECK(track_find("088E927C6F07E0AC-BE57FFFFFFFFFFFF", "_matter", "_tcp") == NULL, "不存在的不应命中");

    CASE("哈希索引：反复回收 / 新增之后过期项不堆积、查找仍然正确");
    /* 模拟 50 轮"一半设备离网、新设备入网"：每轮回收偶数位、补进同样多的新名字，再按扫描那样重建一次 */
    int next = MAX_SERVICES, live_ok = 1, dead_ok = 1;
    for (int round = 0; round < 50; round++) {
        for (int i = 0; i < MAX_SERVICES; i += 2) track_release(i);
        for (int i = 0; i < MAX_SERVICES; i += 2) {
            char name[LABEL_LEN];
            snprintf(name, sizeof(name), "n%06d", next++);
            if (!track_add(name, "_matter", "_tcp")) live_ok = 0;
        }
        CHECK(s_idx_n <= MAX_SERVICES, "第 %d 轮：两次重建之间插入项 %d 应 ≤ %d（装填率 ≤ 1/2）", round, s_idx_n, MAX_SERVICES);
        if (round % 7 == 0) idx_rebuild();
    }
    for (int i = 0; i < MAX_SERVICES; i++) {
        if (!s_track[i].used || track_find(s_track[i].instance, s_track[i].type, s_track[i].proto) != &s_track[i]) live_ok = 0;
    }
    char gone[LABEL_LEN];
    snprintf(gone, sizeof(gone), "n%06d", MAX_SERVICES);   /* 第一轮补进来、之后早被回收的名字 */
    if (track_find(gone, "_matter", "_tcp")) dead_ok = 0;
    CHECK(live_ok, "在用的条目都应找得回且指向自己");
    CHECK(dead_ok, "回收过的名字不应再命中");

    CASE("哈希索引：三段拼接不混淆（\"ab\"+\"c\" ≠ \"a\"+\"bc\"）");
    reset_module();
    CHECK(srp_readv_start() == ESP_OK, "start");
    track_t *x = track_add("ab", "c", "_tcp"), *y = track_add("a", "bc", "_tcp");
    CHECK(x && y && x != y, "两条都应加入");
    CHECK(track_find("ab", "c", "_tcp") == x && track_find("a", "bc", "_tcp") == y, "各自找回各自");

    CASE("超长名字被 snprintf 截断后仍是合法字符串");
    reset_module();
    CHECK(srp_readv_start() == ESP_OK, "start");
    char huge[256];
    memset(huge, 'z', sizeof(huge) - 1); huge[sizeof(huge) - 1] = '\0';
    track_t *t = track_add(huge, "_matter", "_tcp");
    CHECK(t != NULL, "应能加入");
    CHECK(strlen(t->instance) == LABEL_LEN - 1, "应截断到 %d，实际 %zu",
          LABEL_LEN - 1, strlen(t->instance));
}

static void test_settle_constants(void)
{
    CASE("晾够的时间必须覆盖实测 3.2s 的 PCB 探测窗口");
    /* 2026-08-10 在板抓包：服务加入后 PCB 走 PROBE_1..3 → ANNOUNCE_1..3 → RUNNING
       约 3.2s，期间 mdns_priv_pcb_send_bye_service() 直接 return，bye 静默丢失。
       第一版就栽在这里，所以把两个常数钉死在测试里。 */
    CHECK(SETTLE_ROUNDS * POLL_MS > 3200,
          "SETTLE_ROUNDS(%d) * POLL_MS(%d) = %d ms，必须 > 3200",
          SETTLE_ROUNDS, POLL_MS, SETTLE_ROUNDS * POLL_MS);
    CHECK(SETTLE_MS > 3200, "SETTLE_MS = %d ms，必须 > 3200", SETTLE_MS);

    CASE("bye 与重新宣告的间隔不得为 0");
    /* zeroconf 收到 TTL=0 立即移除缓存并回调 Removed；matter-server 还有 0.5s
       事件去抖。两个动作贴在一起会退化成一次 Updated，等于什么都没做。 */
    CHECK(BYE_GAP_MS >= 1000, "BYE_GAP_MS = %d ms，必须 >= 1000", BYE_GAP_MS);
}

int main(void)
{
    printf("=== 缓冲区分配 ===\n"); test_alloc();
    printf("=== 跟踪表 ===\n");     test_track_table();
    printf("=== 时序常数 ===\n");   test_settle_constants();
    reset_module();
    return srp_test_summary("track");
}
