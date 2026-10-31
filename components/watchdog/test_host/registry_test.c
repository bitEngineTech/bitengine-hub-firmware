/* 层 B 任务心跳注册表的主机侧测试
 *
 * 直接 #include 被测 .c，这样能看到 static 状态并逐条断言——注册表的行为
 * （deadline 边界、豁免、幂等、时间回绕）在板子上要么难触发、要么要等很久。
 */

#include "stub/test_common.h"
WD_TEST_GLOBALS

#include "../wd_registry.c"

/* ---- 被测文件依赖的外部符号，这里用可观测的假实现 ---- */

static struct {
    int count;
    wd_fault_class_t cls;
    wd_severity_t    sev;
    char             detail[32];
} g_report;

static int g_recovered_count;
static int g_emergency_count;
static int g_action_slot = -1;

void wd_report(wd_fault_class_t cls, wd_severity_t sev, const char *detail,
               esp_err_t (*recover)(void))
{
    (void)recover;
    g_report.count++;
    g_report.cls = cls;
    g_report.sev = sev;
    snprintf(g_report.detail, sizeof(g_report.detail), "%s", detail ? detail : "");
}

void wd_report_recovered(wd_fault_class_t cls, const char *detail)
{
    (void)cls; (void)detail;
    g_recovered_count++;
}

int  wd_recovery_action_slot(void) { return g_action_slot; }
void wd_recovery_emergency(const char *d) { (void)d; g_emergency_count++; }

/* ---- 每个用例前把注册表清干净 ---- */
static void reset(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    s_slot_count = 0;
    memset(&g_report, 0, sizeof(g_report));
    g_recovered_count = g_emergency_count = 0;
    g_action_slot = -1;
    SET_MS(10000);           /* 不从 0 开始，免得掩盖"未初始化即 0"的 bug */
}

// ============================================================

static void test_basic_kick(void)
{
    CASE("注册后按时 kick，扫描判健康");
    reset();
    static const wd_task_spec_t spec = { "t1", 5000, WD_SEV_FATAL, NULL };
    int s = wd_register(&spec);
    CHECK(s == 0, "首个槽应为 0，实际 %d", s);

    ADVANCE_MS(3000);
    wd_kick(s);
    ADVANCE_MS(3000);
    CHECK(wd_registry_scan() == true, "3s < 5s deadline，应判健康");
    CHECK(g_report.count == 0, "健康时不应上报");
}

static void test_deadline_edge(void)
{
    CASE("deadline 边界：恰好等于不算超时，超过才算");
    reset();
    static const wd_task_spec_t spec = { "t1", 5000, WD_SEV_FATAL, NULL };
    int s = wd_register(&spec);

    ADVANCE_MS(5000);                       /* age == deadline */
    CHECK(wd_registry_scan() == true, "age==deadline 不应判停转（用的是 > 而非 >=）");
    CHECK(g_report.count == 0, "边界上不应上报");

    ADVANCE_MS(1);                          /* age > deadline */
    CHECK(wd_registry_scan() == false, "age>deadline 应判停转");
    CHECK(g_report.count == 1, "应上报一次");
    (void)s;
}

static void test_report_once(void)
{
    CASE("持续停转只上报一次（500ms 扫描不能刷 120 条/分钟）");
    reset();
    static const wd_task_spec_t spec = { "t1", 3000, WD_SEV_FATAL, NULL };
    wd_register(&spec);

    ADVANCE_MS(10000);
    for (int i = 0; i < 20; i++) { wd_registry_scan(); ADVANCE_MS(500); }
    CHECK(g_report.count == 1, "20 次扫描只应上报 1 次，实际 %d", g_report.count);
}

static void test_recover_resets(void)
{
    CASE("恢复后清计数并上报恢复，可再次检出");
    reset();
    static const wd_task_spec_t spec = { "t1", 3000, WD_SEV_RECOVER, NULL };
    int s = wd_register(&spec);

    ADVANCE_MS(5000);
    wd_registry_scan();
    CHECK(g_report.count == 1, "第一次停转应上报");

    wd_kick(s);
    wd_registry_scan();
    CHECK(g_recovered_count == 1, "恢复应上报一次");
    CHECK(s_slots[s].miss_count == 0, "恢复后 miss 应清零");

    ADVANCE_MS(5000);
    wd_registry_scan();
    CHECK(g_report.count == 2, "再次停转应能再次上报（reported 已复位）");
}

static void test_severity_affects_hard_ok(void)
{
    CASE("OBSERVE 不影响 all_hard_ok，RECOVER/FATAL 影响");
    reset();
    static const wd_task_spec_t obs = { "obs", 3000, WD_SEV_OBSERVE, NULL };
    wd_register(&obs);
    ADVANCE_MS(5000);
    CHECK(wd_registry_scan() == true, "只有 OBSERVE 停转时 all_hard_ok 应仍为 true");

    reset();
    static const wd_task_spec_t rec = { "rec", 3000, WD_SEV_RECOVER, NULL };
    wd_register(&rec);
    ADVANCE_MS(5000);
    CHECK(wd_registry_scan() == false, "RECOVER 停转应让 all_hard_ok 为 false");
}

static void test_suspend(void)
{
    CASE("wd_suspend 豁免（OTA/UART 基准测试期间不误判）");
    reset();
    static const wd_task_spec_t spec = { "t1", 3000, WD_SEV_FATAL, NULL };
    int s = wd_register(&spec);

    wd_suspend(s, 30000);
    ADVANCE_MS(20000);
    CHECK(wd_registry_scan() == true, "豁免期内不应判停转");
    CHECK(g_report.count == 0, "豁免期内不应上报");

    ADVANCE_MS(15000);                      /* 豁免已过期，且早已超 deadline */
    CHECK(wd_registry_scan() == false, "豁免过期后应判停转");
    CHECK(g_report.count == 1, "豁免过期后应上报");
}

static void test_idempotent_register(void)
{
    CASE("同名注册幂等（L3 重启子系统会让 init 重跑）");
    reset();
    static const wd_task_spec_t spec = { "httpd", 5000, WD_SEV_RECOVER, NULL };
    int a = wd_register(&spec);

    ADVANCE_MS(9000);
    wd_registry_scan();                     /* 让它进入 reported/miss 状态 */
    CHECK(s_slots[a].reported == true, "应已标记上报");

    int b = wd_register(&spec);             /* 子系统重启后再次注册 */
    CHECK(a == b, "同名应复用同一个槽，实际 %d vs %d", a, b);
    CHECK(s_slot_count == 1, "不应新增槽，实际 %d 个", s_slot_count);
    CHECK(s_slots[a].reported == false, "复用时应清 reported");
    CHECK(s_slots[a].miss_count == 0, "复用时应清 miss");
    CHECK(s_slots[a].recover_pending == true, "复用一个已上报停转的槽，应记待确认恢复（T9.36）");
    CHECK(wd_registry_scan() == true, "复用后 last_kick 刷新，应判健康");
}

static void test_reuse_reports_recovered(void)
{
    CASE("T9.36：L3 重启后新实例真 kick 过才发一次'任务恢复'，注册本身不算");
    reset();
    static const wd_task_spec_t spec = { "httpd", 5000, WD_SEV_RECOVER, NULL };
    int a = wd_register(&spec);
    ADVANCE_MS(9000);
    wd_registry_scan();
    CHECK(g_report.count == 1, "先判一次停转");

    wd_register(&spec);                     /* L3：新实例重新注册 */
    wd_registry_scan();
    CHECK(g_recovered_count == 0, "只注册、还没 kick：不能发恢复（注册时的 last_kick 是代填的），实际 %d", g_recovered_count);
    ADVANCE_MS(1000);
    wd_registry_scan();
    CHECK(g_recovered_count == 0, "还是没 kick：仍不发，实际 %d", g_recovered_count);

    wd_kick(a);                             /* 新实例第一次心跳 */
    wd_registry_scan();
    CHECK(g_recovered_count == 1, "新实例 kick 之后应发一次恢复，实际 %d", g_recovered_count);
    CHECK(s_slots[a].recover_pending == false, "发完应清待确认");
    wd_kick(a);
    wd_registry_scan();
    CHECK(g_recovered_count == 1, "不应重复发，实际 %d", g_recovered_count);

    ADVANCE_MS(9000);
    wd_registry_scan();
    CHECK(g_report.count == 2, "之后再停转照常上报，实际 %d", g_report.count);
}

static void test_reuse_then_stall_again(void)
{
    CASE("T9.36：复用后新实例一次都没 kick 就又停转 —— 照常再报，之后恢复只发一次");
    reset();
    static const wd_task_spec_t spec = { "httpd", 5000, WD_SEV_RECOVER, NULL };
    int a = wd_register(&spec);
    ADVANCE_MS(9000);
    wd_registry_scan();
    wd_register(&spec);
    ADVANCE_MS(9000);                       /* 新实例也卡住了 */
    wd_registry_scan();
    CHECK(g_report.count == 2, "应再报一次停转（升级由恢复引擎决定），实际 %d", g_report.count);
    CHECK(g_recovered_count == 0, "没恢复过，实际 %d", g_recovered_count);
    wd_kick(a);
    wd_registry_scan();
    wd_registry_scan();
    CHECK(g_recovered_count == 1, "恢复只发一次（reported 跳变那条），实际 %d", g_recovered_count);
}

static void test_offline_reported_then_back(void)
{
    CASE("T9.36：已上报停转的槽被标成无主，任务回来 kick 了要发恢复");
    reset();
    static const wd_task_spec_t spec = { "bridge", 30000, WD_SEV_FATAL, NULL };
    int s = wd_register(&spec);
    ADVANCE_MS(40000);
    wd_registry_scan();
    CHECK(g_report.count == 1, "先判停转");
    wd_slot_offline(s);
    ADVANCE_MS(5000);
    wd_registry_scan();
    CHECK(g_recovered_count == 0, "无主期间不发，实际 %d", g_recovered_count);
    wd_kick(s);
    wd_registry_scan();
    CHECK(g_recovered_count == 1, "回来 kick 了应发恢复，实际 %d", g_recovered_count);
}

static void test_fresh_slot_no_pending(void)
{
    CASE("T9.36：没上报过停转的槽被复用 / 标成无主，不应凭空发恢复");
    reset();
    static const wd_task_spec_t spec = { "mqtt_pub", 5000, WD_SEV_RECOVER, NULL };
    int s = wd_register(&spec);
    wd_kick(s);
    wd_register(&spec);
    wd_slot_offline(s);
    wd_kick(s);
    wd_registry_scan();
    CHECK(g_recovered_count == 0, "从没停转过：不该有恢复，实际 %d", g_recovered_count);
}

static void test_registry_full(void)
{
    CASE("注册表满返回 -1，不越界");
    reset();
    char names[WD_MAX_TASKS + 4][8];
    for (int i = 0; i < WD_MAX_TASKS + 4; i++) {
        snprintf(names[i], sizeof(names[i]), "t%d", i);
        wd_task_spec_t sp = { names[i], 5000, WD_SEV_OBSERVE, NULL };
        int r = wd_register(&sp);
        if (i < WD_MAX_TASKS) CHECK(r == i, "第 %d 个应成功", i);
        else                  CHECK(r == -1, "第 %d 个应失败", i);
    }
    CHECK(s_slot_count == WD_MAX_TASKS, "槽数应封顶在 %d", WD_MAX_TASKS);
    /* 上限按配置分开（docs/15 设计第十五节 A-24）：run.sh 两种配置各编一次 */
#if CONFIG_BITENGINE_PROFILE_DIRECT
    CHECK(WD_MAX_TASKS == 20 && HEAP_MARK_MAX == 32, "直连型：20 个槽、32 个堆水位标记");
#else
    CHECK(WD_MAX_TASKS == 16 && HEAP_MARK_MAX == 24, "HA 型：仍是 16 个槽、24 个堆水位标记");
#endif
}

static void test_bad_args(void)
{
    CASE("非法参数不崩");
    reset();
    CHECK(wd_register(NULL) == -1, "NULL spec 应返回 -1");
    wd_task_spec_t noname = { NULL, 5000, WD_SEV_OBSERVE, NULL };
    CHECK(wd_register(&noname) == -1, "无名字应返回 -1");

    wd_kick(-1); wd_kick(999); wd_suspend(-1, 100); wd_suspend(999, 100);
    CHECK(1, "越界 slot 不应崩");

    static const wd_task_spec_t spec = { "t1", 10, WD_SEV_OBSERVE, NULL };
    int s = wd_register(&spec);
    CHECK(s_slots[s].deadline_ms == 3000, "deadline 应被抬到下限 3000，实际 %u",
          s_slots[s].deadline_ms);
}

static void test_time_wraparound(void)
{
    CASE("32 位毫秒时间戳回绕（49.7 天）不误判");
    reset();
    /* 把时钟推到 32 位毫秒即将回绕处 */
    SET_MS(0xFFFFF000u);
    static const wd_task_spec_t spec = { "t1", 5000, WD_SEV_FATAL, NULL };
    int s = wd_register(&spec);

    ADVANCE_MS(2000);                       /* 跨过回绕点 */
    wd_kick(s);
    ADVANCE_MS(2000);
    CHECK(wd_registry_scan() == true, "跨回绕点后 2s 应仍判健康（无符号减法）");

    ADVANCE_MS(10000);
    CHECK(wd_registry_scan() == false, "跨回绕点后超时应正常检出");
}

static void test_emergency_path(void)
{
    CASE("恢复引擎自己停转时走应急路径，不经过队列");
    reset();
    static const wd_task_spec_t spec = { "wd_action", 5000, WD_SEV_FATAL, NULL };
    int s = wd_register(&spec);
    g_action_slot = s;

    ADVANCE_MS(10000);
    wd_registry_scan();
    CHECK(g_emergency_count == 1, "应调用 wd_recovery_emergency");
    CHECK(g_report.count == 0, "不应走 wd_report（那条路要经过它自己的队列）");
}

static void test_stack_low(void)
{
    CASE("栈余量取各任务自采值中的最小值（不存句柄）");
    reset();
    static const wd_task_spec_t a = { "a", 5000, WD_SEV_OBSERVE, NULL };
    static const wd_task_spec_t b = { "b", 5000, WD_SEV_OBSERVE, NULL };
    int sa = wd_register(&a), sb = wd_register(&b);

    const char *name = (const char *)1; uint32_t bytes = 1;
    wd_registry_stack_low(&name, &bytes);
    CHECK(name == NULL && bytes == 0, "没有任务 kick 过时应返回 NULL/0");

    g_fake_stack_headroom = 700;
    wd_kick(sa);                            /* 首次 kick 即采样 */
    wd_kick(sb);
    wd_registry_stack_low(&name, &bytes);
    CHECK(name != NULL && bytes == 700, "应返回 700，实际 %u", bytes);
    g_fake_stack_headroom = 4096;
}


static void test_slot_offline(void)
{
    CASE("无主槽位不判停转，下次 kick 自动恢复监视");
    reset();
    static const wd_task_spec_t o = { "bridge", 30000, WD_SEV_FATAL, NULL };
    int s = wd_register(&o);
    wd_kick(s);

    /* 任务按计划结束 —— 标记无主 */
    wd_slot_offline(s);
    ADVANCE_MS(120000);                 /* 远超 30s deadline */
    wd_registry_scan();
    CHECK(g_report.count == 0, "无主期间不应上报停转，实际 %d 次", g_report.count);

    /* 任务回来了：一次 kick 就该恢复监视 */
    wd_kick(s);
    ADVANCE_MS(40000);                  /* 又超了 deadline */
    wd_registry_scan();
    CHECK(g_report.count == 1, "恢复监视后应正常判停转，实际 %d 次", g_report.count);
}

int main(void)
{
    printf("=== 层 B 任务心跳注册表 ===\n");
    test_basic_kick();
    test_deadline_edge();
    test_report_once();
    test_recover_resets();
    test_severity_affects_hard_ok();
    test_suspend();
    test_idempotent_register();
    test_reuse_reports_recovered();
    test_reuse_then_stall_again();
    test_offline_reported_then_back();
    test_fresh_slot_no_pending();
    test_registry_full();
    test_bad_args();
    test_time_wraparound();
    test_emergency_path();
    test_stack_low();
    test_slot_offline();
    return wd_test_summary("registry");
}
