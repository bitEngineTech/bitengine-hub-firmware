/* 层 E 恢复引擎状态机的主机侧测试
 *
 * 这是全模块分支最多的地方：升级/降级、冷却窗口、fail_streak、策略封顶、
 * 安全模式抑制。在板子上每验一条要等一个冷却周期（60/300/900s）并且往往
 * 伴随重启，主机上是几微秒。
 *
 * 最该被锁住的不变量：**MG24（PERIPH）永远不得升到 L3/L4**。AM1 板上硬件
 * 复位会带走整板且可能丢配网，这条策略一旦被手滑改掉，后果是用户重新配网。
 *
 * 第二条（2026-10-07，T9.18 方案 A）：**Thread 是 L2 → L4，永远不走 L3**，且级别/冷却与同类的
 * 网络分开存。整栈重启正常态实测会 panic（components/webui/otbr_api.c 末尾），谁把 L3 接回来，
 * test_thread_l2_then_reboot 会红。通用的"子系统类"行为改用 "network" 来测。
 */

#include "stub/test_common.h"
WD_TEST_GLOBALS

#include "../wd_recovery.c"
/* do_reboot 填异常开机事件的 cause（T9.30 ④）。两个文件各有一个 static TAG，单编译单元里改个名 */
#define TAG WD_BOOT_TAG
#include "../wd_boot_reason.c"
#undef TAG

/* 整机重启前等升级时会 kick 自己的槽位（T9.30 ①）；注册表不在本测试里 */
void wd_kick(int slot) { (void)slot; }

/* ---- 假的取证层：只观测调用，不碰 NVS ---- */
static struct { int count; wd_fault_class_t cls; wd_level_t lvl; char detail[32]; } g_rec;
static uint8_t g_self_heal;
static int     g_safe_mode_flag;
static int     g_clear_called;

void wd_forensics_record(wd_fault_class_t cls, wd_level_t lvl, const char *detail)
{
    g_rec.count++; g_rec.cls = cls; g_rec.lvl = lvl;
    snprintf(g_rec.detail, sizeof(g_rec.detail), "%s", detail ? detail : "");
    if (lvl >= WD_LEVEL_REBOOT) g_self_heal++;
}
bool        wd_forensics_safe_mode(void)        { return g_safe_mode_flag; }
uint8_t     wd_forensics_self_heal_count(void)  { return g_self_heal; }
const char *wd_forensics_last_detail(void)      { return "test"; }
void        wd_forensics_clear_safe_mode(void)  { g_clear_called++; g_safe_mode_flag = 0; }

/* 注册表：恢复引擎会给自己注册一个槽 */
int wd_register(const wd_task_spec_t *spec) { (void)spec; return 7; }

/* ---- 可控的恢复回调 ---- */
/* 回调被调用那一刻取证层里有几条、最后一条是几级 —— 锁住"先记后做"：
   恢复动作自己崩了（2026-10-07 httpd L3 栈溢出），history 也得留下线索 */
static int g_rec_at_call; static wd_level_t g_lvl_at_call;

static int g_recover_calls;
static esp_err_t g_recover_ret = ESP_OK;
static esp_err_t fake_recover(void)
{
    g_recover_calls++; g_rec_at_call = g_rec.count; g_lvl_at_call = g_rec.lvl;
    return g_recover_ret;
}

static int g_subsys_calls;
static esp_err_t g_subsys_ret = ESP_OK;
static esp_err_t fake_subsys(void)
{
    g_subsys_calls++; g_rec_at_call = g_rec.count; g_lvl_at_call = g_rec.lvl;
    return g_subsys_ret;
}

static void reset(void)
{
    memset(s_state, 0, sizeof(s_state));
    memset(s_ov_state, 0, sizeof(s_ov_state));
    memset(s_recovery, 0, sizeof(s_recovery));
    s_recovery_count = 0;
    s_last_action = "none"; s_last_action_ts = 0;
    s_safe_mode = false;
    memset(&g_rec, 0, sizeof(g_rec));
    g_self_heal = 0; g_safe_mode_flag = 0; g_clear_called = 0;
    g_recover_calls = g_subsys_calls = 0;
    g_rec_at_call = 0; g_lvl_at_call = WD_LEVEL_NONE;
    g_recover_ret = g_subsys_ret = ESP_OK;
    g_restart_count = 0; g_event_count = 0;
    SET_MS(100000);
}

/* 直接驱动状态机，绕过队列（队列本身在 test_queue 里单独验）。上报时刻 = 现在。 */
static void feed(wd_fault_class_t cls, wd_severity_t sev, const char *detail,
                 esp_err_t (*recover)(void))
{
    wd_action_msg_t m = { cls, sev, detail, recover, now_s(), false };
    handle(&m);
}

/* 把队列里积压的消息按 wd_action 任务的同一份分发逻辑处理完 */
static void drain(void)
{
    wd_action_msg_t m;
    while (xQueueReceive(s_queue, &m, 0) == pdTRUE) dispatch(&m);
}

// ============================================================

static void test_observe_stays_l1(void)
{
    CASE("OBSERVE 永远停在 L1，重复上报也不升级");
    reset();
    for (int i = 0; i < 5; i++) {
        feed(WD_FAULT_TASK_STALL, WD_SEV_OBSERVE, "led_task", NULL);
        ADVANCE_MS(60000);                  /* 每次都让冷却过掉 */
    }
    CHECK(s_state[WD_FAULT_TASK_STALL].level == WD_LEVEL_LOG,
          "应停在 L1，实际 L%d", s_state[WD_FAULT_TASK_STALL].level);
    CHECK(g_restart_count == 0, "OBSERVE 绝不应重启");
    CHECK(g_subsys_calls == 0, "OBSERVE 不应触发子系统重启");
}

static void test_recover_starts_at_l2(void)
{
    CASE("RECOVER 起点是 L2 而不是 L1（否则单次故障永远轮不到恢复）");
    reset();
    feed(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "network", fake_recover);
    CHECK(s_state[WD_FAULT_SUBSYS_DEAD].level == WD_LEVEL_TARGET,
          "首次 RECOVER 应到 L2，实际 L%d", s_state[WD_FAULT_SUBSYS_DEAD].level);
    CHECK(g_recover_calls == 1, "应调用定向恢复回调");
    CHECK(g_rec_at_call == 1 && g_lvl_at_call == WD_LEVEL_TARGET,
          "L2 取证必须在调回调之前写（调用时 %d 条、L%d）", g_rec_at_call, g_lvl_at_call);
}

static void test_no_l2_handler_jumps_to_l3(void)
{
    CASE("没有 L2 定向手段时直接进 L3（TC-W07 的 httpd 就是这种）");
    reset();
    wd_register_recovery("httpd", fake_subsys);
    feed(WD_FAULT_TASK_STALL, WD_SEV_RECOVER, "httpd", NULL);
    CHECK(s_state[WD_FAULT_TASK_STALL].level == WD_LEVEL_SUBSYS,
          "应直接到 L3，实际 L%d", s_state[WD_FAULT_TASK_STALL].level);
    CHECK(g_subsys_calls == 1, "应调用登记的子系统重启入口");
    CHECK(g_rec_at_call == 1 && g_lvl_at_call == WD_LEVEL_SUBSYS,
          "L3 取证必须在调恢复入口之前写（调用时 %d 条、L%d）", g_rec_at_call, g_lvl_at_call);
}

static void test_fatal_reboots_immediately(void)
{
    CASE("FATAL 直接 L4，且重启前必须先写取证");
    reset();
    feed(WD_FAULT_TASK_STALL, WD_SEV_FATAL, "health", NULL);
    CHECK(s_state[WD_FAULT_TASK_STALL].level == WD_LEVEL_REBOOT, "应到 L4");
    CHECK(g_restart_count == 1, "应重启一次");
    CHECK(g_rec.count >= 1 && g_rec.lvl == WD_LEVEL_REBOOT, "应写 L4 取证");
    CHECK(strcmp(g_rec.detail, "health") == 0, "取证应带 detail，实际 %s", g_rec.detail);
}

static void test_cooldown_blocks_escalation(void)
{
    CASE("冷却窗口内复发不升级");
    reset();
    wd_register_recovery("network", fake_subsys);
    feed(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "network", fake_recover);
    CHECK(s_state[WD_FAULT_SUBSYS_DEAD].level == WD_LEVEL_TARGET, "首次到 L2");

    ADVANCE_MS(30000);                      /* L2 冷却是 60s，只过了一半 */
    feed(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "network", fake_recover);
    CHECK(s_state[WD_FAULT_SUBSYS_DEAD].level == WD_LEVEL_TARGET,
          "冷却中不应升级，实际 L%d", s_state[WD_FAULT_SUBSYS_DEAD].level);
    CHECK(g_subsys_calls == 0, "冷却中不应执行 L3");

    ADVANCE_MS(31000);                      /* 冷却过了 */
    feed(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "network", fake_recover);
    CHECK(s_state[WD_FAULT_SUBSYS_DEAD].level == WD_LEVEL_SUBSYS, "冷却过后应升到 L3");
    CHECK(g_subsys_calls == 1, "应执行一次 L3");
}

static void test_mg24_never_escalates_past_l2(void)
{
    CASE("⚠️ MG24(PERIPH) 封顶 L2：绝不自动整机/硬件复位（会丢配网）");
    reset();
    wd_register_recovery("mg24", fake_subsys);
    for (int i = 0; i < 10; i++) {
        feed(WD_FAULT_PERIPH_DEAD, WD_SEV_RECOVER, "mg24", fake_recover);
        ADVANCE_MS(1000000);                /* 每次都让冷却彻底过掉 */
    }
    CHECK(s_state[WD_FAULT_PERIPH_DEAD].level == WD_LEVEL_TARGET,
          "PERIPH 必须封顶在 L2，实际 L%d", s_state[WD_FAULT_PERIPH_DEAD].level);
    CHECK(g_restart_count == 0, "MG24 故障绝不应触发整机重启");
    CHECK(g_subsys_calls == 0, "MG24 故障绝不应触发子系统重启");

    /* 连 FATAL 上报也必须被策略压回 L2 */
    feed(WD_FAULT_PERIPH_DEAD, WD_SEV_FATAL, "mg24", NULL);
    CHECK(g_restart_count == 0, "即使以 FATAL 上报，PERIPH 也不得重启");
}

static void test_network_still_capped_at_l3(void)
{
    CASE("SUBSYS 类（network）仍封顶 L3：Thread 的覆盖不许连带放开同类");
    reset();
    wd_register_recovery("network", fake_subsys);
    g_subsys_ret = ESP_FAIL;                /* 一直失败 */
    for (int i = 0; i < 6; i++) {
        feed(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "network", NULL);
        ADVANCE_MS(1000000);
    }
    CHECK(s_state[WD_FAULT_SUBSYS_DEAD].level == WD_LEVEL_SUBSYS, "应封顶 L3");
    CHECK(g_restart_count == 0, "SUBSYS 策略不允许 L4");
}

static void test_thread_l2_then_reboot(void)
{
    CASE("⚠️ Thread：L2 → 冷却后仍停摆 → L4 整机重启，绝不走 L3（就算有人把 L3 入口登记回来）");
    reset();
    wd_register_recovery("thread", fake_subsys);   /* 模拟有人把整栈重启接回来 */
    feed(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "thread", fake_recover);
    CHECK(s_ov_state[0].level == WD_LEVEL_TARGET, "首次应到 L2，实际 L%d", s_ov_state[0].level);
    CHECK(g_recover_calls == 1, "应调射频重启（L2）");

    ADVANCE_MS(30000);                              /* L2 冷却 60 s 内 */
    feed(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "thread", fake_recover);
    CHECK(g_restart_count == 0, "冷却内不应重启");
    CHECK(s_ov_state[0].level == WD_LEVEL_TARGET, "冷却内应维持 L2");

    ADVANCE_MS(31000);                              /* 冷却过了，探针再报 */
    feed(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "thread", fake_recover);
    CHECK(g_subsys_calls == 0, "Thread 绝不应走 L3（整栈重启正常态会 panic）");
    CHECK(s_ov_state[0].level == WD_LEVEL_REBOOT, "应升到 L4，实际 L%d", s_ov_state[0].level);
    CHECK(g_restart_count == 1, "应整机重启一次，实际 %d", g_restart_count);
    CHECK(g_rec.lvl == WD_LEVEL_REBOOT && strcmp(g_rec.detail, "thread") == 0,
          "重启前应写 L4 取证（计入自愈次数），实际 L%d/%s", g_rec.lvl, g_rec.detail);
    CHECK(g_self_heal == 1, "整机重启应计 1 次自愈");
}

static void test_thread_l2_failure_still_waits(void)
{
    CASE("Thread：L2 返回失败也不立即重启，等冷却后的下一次上报");
    reset();
    g_recover_ret = ESP_FAIL;
    feed(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "thread", fake_recover);
    CHECK(g_restart_count == 0, "L2 失败本身不应触发重启");
    ADVANCE_MS(61000);
    feed(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "thread", fake_recover);
    CHECK(g_restart_count == 1, "冷却后再报应重启");
}

static void test_thread_independent_of_network(void)
{
    CASE("Thread 与 network 同属 SUBSYS 类，但级别/冷却各算各的，互不打断");
    reset();
    wd_register_recovery("network", fake_subsys);
    feed(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "thread", fake_recover);   /* thread L2 */
    ADVANCE_MS(10000);
    feed(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "network", NULL);          /* network 直接 L3 */
    CHECK(g_subsys_calls == 1, "network 不应被 thread 的 L2 冷却吞掉");
    CHECK(s_state[WD_FAULT_SUBSYS_DEAD].level == WD_LEVEL_SUBSYS, "network 应在 L3");

    wd_report_recovered(WD_FAULT_SUBSYS_DEAD, "network");
    CHECK(s_ov_state[0].level == WD_LEVEL_TARGET, "network 恢复不应清掉 thread 的级别");

    ADVANCE_MS(51000);                                                    /* thread 冷却过了 */
    feed(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "thread", fake_recover);
    CHECK(g_restart_count == 1, "thread 应照常升到 L4");
}

static void test_thread_recovered_resets_own_level(void)
{
    CASE("Thread 恢复后只清自己的级别，下次停摆重新从 L2 开始");
    reset();
    feed(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "thread", fake_recover);
    wd_report_recovered(WD_FAULT_SUBSYS_DEAD, "thread");
    CHECK(s_ov_state[0].level == WD_LEVEL_NONE, "应归零");
    ADVANCE_MS(1000);
    feed(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "thread", fake_recover);
    CHECK(s_ov_state[0].level == WD_LEVEL_TARGET, "应重新从 L2 开始，实际 L%d", s_ov_state[0].level);
    CHECK(g_restart_count == 0, "不应重启");
}

static void test_thread_no_l2_means_reboot(void)
{
    CASE("Thread 不带 L2 手段上报（OT 主循环意外退出）→ 直接 L4");
    reset();
    feed(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "thread", NULL);
    CHECK(g_restart_count == 1, "应直接整机重启，实际 %d", g_restart_count);
    CHECK(g_subsys_calls == 0 && g_recover_calls == 0, "不应调任何 L2/L3 回调");
}

static void test_recovered_is_queued_in_order(void)
{
    CASE("⚠️ 「已恢复」与上报走同一队列、按先后生效（同步改会错序，级别残留 → 下一轮越过 L2）");
    reset();
    s_queue = xQueueCreate(8, sizeof(wd_action_msg_t));
    wd_report(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "thread", fake_recover);   /* wd_action 正忙，先排着 */
    wd_report_recovered(WD_FAULT_SUBSYS_DEAD, "thread");                         /* 停摆很快自愈 */
    CHECK(s_queue->count == 2, "两条都应入队，实际 %d", s_queue->count);
    drain();
    CHECK(s_ov_state[0].level == WD_LEVEL_NONE, "按先后：先 L2 再归零，实际 L%d", s_ov_state[0].level);

    ADVANCE_MS(3600000);                    /* 很久以后另一次独立的停摆 */
    wd_report(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "thread", fake_recover);
    drain();
    CHECK(g_restart_count == 0, "新一轮应从 L2 开始，不应直接重启");
    CHECK(s_ov_state[0].level == WD_LEVEL_TARGET, "应在 L2，实际 L%d", s_ov_state[0].level);
    vQueueDelete(s_queue); s_queue = NULL;
}

static void test_recovered_applies_in_safe_mode(void)
{
    CASE("安全模式下恢复动作被忽略，但「已恢复」照样清级别");
    reset();
    s_queue = xQueueCreate(8, sizeof(wd_action_msg_t));
    feed(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "thread", fake_recover);         /* 先到 L2 */
    s_safe_mode = true;
    ADVANCE_MS(61000);
    wd_report(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "thread", fake_recover);
    drain();
    CHECK(g_restart_count == 0, "安全模式下不应重启");
    wd_report_recovered(WD_FAULT_SUBSYS_DEAD, "thread");
    drain();
    CHECK(s_ov_state[0].level == WD_LEVEL_NONE, "「已恢复」应生效，实际 L%d", s_ov_state[0].level);
    vQueueDelete(s_queue); s_queue = NULL;
}

static void test_cooldown_uses_report_time(void)
{
    CASE("冷却按上报时刻算：wd_action 忙了几秒，准点的再报照样升级");
    reset();
    s_queue = xQueueCreate(8, sizeof(wd_action_msg_t));
    wd_report(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "thread", fake_recover);   /* T */
    ADVANCE_MS(3000);                       /* 3 s 后才轮到处理 */
    drain();
    CHECK(s_ov_state[0].level == WD_LEVEL_TARGET, "应到 L2");
    ADVANCE_MS(57000);                      /* T+60：探针准点再报 */
    wd_report(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "thread", fake_recover);
    ADVANCE_MS(3000);                       /* 处理又晚 3 s */
    drain();
    CHECK(g_restart_count == 1, "按上报时刻已满 60 s，应升到 L4，实际重启 %d 次", g_restart_count);

    reset();
    s_queue = xQueueCreate(8, sizeof(wd_action_msg_t));
    wd_report(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "thread", fake_recover);   /* T */
    drain();
    ADVANCE_MS(59000);                      /* T+59：还在冷却里 */
    wd_report(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "thread", fake_recover);
    ADVANCE_MS(5000);                       /* 处理时已过 60 s，但按上报时刻没到 */
    drain();
    CHECK(g_restart_count == 0, "按上报时刻只过了 59 s，不应升级");
    vQueueDelete(s_queue); s_queue = NULL;
}

static void test_view_shows_thread_level(void)
{
    CASE("/diag/watchdog 的 levels.subsys 看得见 Thread 单独存的级别");
    reset();
    feed(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "thread", fake_recover);
    wd_recovery_view_t v;
    wd_recovery_get(&v);
    CHECK(v.level[WD_FAULT_SUBSYS_DEAD] == WD_LEVEL_TARGET, "视图应显示 L2，实际 L%d", v.level[WD_FAULT_SUBSYS_DEAD]);
}

static void test_l3_double_failure_reboots(void)
{
    CASE("TASK/RESOURCE 类：连续 2 次 L3 无效 → L4");
    reset();
    wd_register_recovery("httpd", fake_subsys);
    g_subsys_ret = ESP_FAIL;

    feed(WD_FAULT_TASK_STALL, WD_SEV_RECOVER, "httpd", NULL);   /* L3 第一次失败 */
    CHECK(g_restart_count == 0, "第一次失败不应重启");
    CHECK(s_state[WD_FAULT_TASK_STALL].fail_streak == 1, "streak 应为 1");

    ADVANCE_MS(400000);                     /* 过 L3 的 300s 冷却 */
    feed(WD_FAULT_TASK_STALL, WD_SEV_RECOVER, "httpd", NULL);
    CHECK(g_restart_count == 1, "第二次失败应触发 L4");
}

static void test_recovered_resets_level(void)
{
    CASE("故障消失后级别归零，下次从头开始");
    reset();
    feed(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "network", fake_recover);
    CHECK(s_state[WD_FAULT_SUBSYS_DEAD].level == WD_LEVEL_TARGET, "先到 L2");

    wd_report_recovered(WD_FAULT_SUBSYS_DEAD, "network");
    CHECK(s_state[WD_FAULT_SUBSYS_DEAD].level == WD_LEVEL_NONE, "恢复后应归零");
    CHECK(s_state[WD_FAULT_SUBSYS_DEAD].fail_streak == 0, "streak 也应归零");

    ADVANCE_MS(1000);
    feed(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "network", fake_recover);
    CHECK(s_state[WD_FAULT_SUBSYS_DEAD].level == WD_LEVEL_TARGET, "应重新从 L2 开始");
}

static void test_safe_mode_suppresses(void)
{
    CASE("安全模式下不再自愈，只记录");
    reset();
    s_safe_mode = true;
    /* 走队列路径，因为抑制逻辑在 wd_action_task 里；这里模拟它的判断 */
    CHECK(wd_safe_mode() == true, "安全模式标志应生效");

    /* 直接 feed 会绕过抑制，所以验证的是引擎对外暴露的状态 */
    wd_recovery_view_t v;
    wd_recovery_get(&v);
    CHECK(v.safe_mode == true, "视图应反映安全模式");
}

static void test_safe_mode_exit(void)
{
    CASE("退出安全模式：清记录并重启；未进入时拒绝");
    reset();
    CHECK(wd_safe_mode_exit() == ESP_ERR_INVALID_STATE, "未进入安全模式应拒绝");
    CHECK(g_clear_called == 0, "拒绝时不应清记录");

    s_safe_mode = true; g_safe_mode_flag = 1;
    wd_safe_mode_exit();
    CHECK(g_clear_called == 1, "应清安全模式记录");
    CHECK(g_restart_count == 1, "应重启");
    CHECK(s_safe_mode == false, "内存标志也应清掉");
}

static void test_recovery_registry(void)
{
    CASE("恢复回调表：子串匹配、满了拒绝、非法参数");
    reset();
    CHECK(wd_register_recovery(NULL, fake_subsys) == ESP_ERR_INVALID_ARG, "NULL key");
    CHECK(wd_register_recovery("x", NULL) == ESP_ERR_INVALID_ARG, "NULL fn");

    wd_register_recovery("httpd", fake_subsys);
    CHECK(subsys_restart("task httpd stalled") == ESP_OK, "子串应能匹配");
    CHECK(subsys_restart("unknown") == ESP_ERR_NOT_FOUND, "匹配不上应返回 NOT_FOUND");

    for (int i = 0; i < WD_RECOVERY_MAX + 2; i++) {
        static char keys[WD_RECOVERY_MAX + 2][8];
        snprintf(keys[i], sizeof(keys[i]), "k%d", i);
        wd_register_recovery(keys[i], fake_subsys);
    }
    CHECK(s_recovery_count == WD_RECOVERY_MAX, "应封顶在 %d，实际 %d",
          WD_RECOVERY_MAX, s_recovery_count);
}

static void test_bad_class(void)
{
    CASE("非法故障类被丢弃，不越界");
    reset();
    wd_report(WD_FAULT_NONE, WD_SEV_FATAL, "x", NULL);
    wd_report((wd_fault_class_t)99, WD_SEV_FATAL, "x", NULL);
    wd_report_recovered(WD_FAULT_NONE, "x");
    wd_report_recovered((wd_fault_class_t)99, "x");
    CHECK(g_restart_count == 0, "非法类不应产生任何动作");
}

static void test_queue_full_does_not_block(void)
{
    CASE("动作队列满时丢弃而不是阻塞（调用方是 pri-22 扫描任务）");
    reset();
    s_queue = xQueueCreate(2, sizeof(wd_action_msg_t));
    for (int i = 0; i < 10; i++) {
        wd_report(WD_FAULT_TASK_STALL, WD_SEV_OBSERVE, "t", NULL);
    }
    CHECK(s_queue->count == 2, "队列应满于 2，实际 %d", s_queue->count);
    vQueueDelete(s_queue); s_queue = NULL;
    CHECK(1, "满队列下 wd_report 应立即返回");
}

/* ---- T9.27：真复位原因 ---- */
static void reset_latch(void) { s_latched = false; s_reason = ESP_RST_UNKNOWN; }

static void test_boot_reason_survives_extra_restart(void)
{
    CASE("非干净复位后补重启：第二次开机报真原因，且只报一次");
    reset_latch(); s_rtc_noted = 0;
    g_reset_reason = ESP_RST_TASK_WDT;
    wd_boot_reason_note(ESP_RST_TASK_WDT);        /* main.c 补重启前 */
    g_reset_reason = ESP_RST_SW;                   /* 补的那次 esp_restart */
    wd_boot_reason_latch();
    CHECK(wd_boot_reason_get() == ESP_RST_TASK_WDT, "应还原成 TASK_WDT，实际 %d", (int)wd_boot_reason_get());
    reset_latch();                                 /* 再一次正常的软重启 */
    wd_boot_reason_latch();
    CHECK(wd_boot_reason_get() == ESP_RST_SW, "记录只能用一次，实际 %d", (int)wd_boot_reason_get());
}

static void test_boot_reason_ignores_garbage(void)
{
    CASE("掉电后 RTC 是随机值：没有魔数、或这次不是软复位，都不采信");
    reset_latch(); s_rtc_noted = 0x12340004;       /* 低位像 PANIC，但没魔数 */
    g_reset_reason = ESP_RST_SW;
    wd_boot_reason_latch();
    CHECK(wd_boot_reason_get() == ESP_RST_SW, "无魔数应报原值，实际 %d", (int)wd_boot_reason_get());
    reset_latch(); wd_boot_reason_note(ESP_RST_PANIC);
    g_reset_reason = ESP_RST_POWERON;              /* 记下之后断了电 */
    wd_boot_reason_latch();
    CHECK(wd_boot_reason_get() == ESP_RST_POWERON, "上电开机应报 POWERON，实际 %d", (int)wd_boot_reason_get());
    CHECK(s_rtc_noted == 0, "latch 后记录应清掉");
}

static void test_abnormal_cause_strings(void)
{
    CASE("异常开机 cause：看门狗带类与子系统，非法字符被换掉、不溢出");
    wd_abnormal_boot_t ev;
    wd_abnormal_cause_watchdog(&ev, WD_FAULT_SUBSYS_DEAD, "thread");
    CHECK(strcmp(ev.cause, "watchdog:subsys/thread") == 0, "实际 %s", ev.cause);
    wd_abnormal_cause_watchdog(&ev, WD_FAULT_TASK_STALL, "Bad\"name}");
    CHECK(strcmp(ev.cause, "watchdog:task/bad_name_") == 0, "实际 %s", ev.cause);
    wd_abnormal_cause_watchdog(&ev, WD_FAULT_RESOURCE,
                               "a-very-long-detail-string-that-overflows-the-buffer");
    CHECK(strlen(ev.cause) == sizeof(ev.cause) - 1, "应截断到 %u，实际 %u",
          (unsigned)(sizeof(ev.cause) - 1), (unsigned)strlen(ev.cause));
    wd_abnormal_cause_watchdog(&ev, (wd_fault_class_t)99, NULL);
    CHECK(strcmp(ev.cause, "watchdog:unknown/unknown") == 0, "实际 %s", ev.cause);
    wd_abnormal_cause_reset(&ev, ESP_RST_PANIC);
    CHECK(strcmp(ev.cause, "panic") == 0, "实际 %s", ev.cause);
}

/* ---- T9.30 ①：整机重启前等升级写完 ---- */
static int g_busy_left, g_busy_calls, g_rec_count_when_free;
static bool fake_ota_busy(void)
{
    g_busy_calls++;
    if (g_busy_left > 0) { g_busy_left--; g_rec_count_when_free = g_rec.count; return true; }
    return false;
}

static void test_reboot_waits_for_ota(void)
{
    CASE("L4 撞上升级：先等升级结束、等的期间不写取证，结束后才记录并重启");
    reset();
    wd_recovery_set_ota_busy_query(fake_ota_busy);
    g_busy_left = 3; g_busy_calls = 0; g_rec_count_when_free = -1;
    feed(WD_FAULT_TASK_STALL, WD_SEV_FATAL, "health", NULL);
    CHECK(g_restart_count == 1, "应重启一次，实际 %d", g_restart_count);
    CHECK(g_busy_calls >= 4, "应反复询问直到不忙，实际 %d 次", g_busy_calls);
    CHECK(g_rec_count_when_free == 0, "等待期间不应写取证（实际已写 %d 条）", g_rec_count_when_free);
    CHECK(g_rec.count >= 1 && g_rec.lvl == WD_LEVEL_REBOOT, "升级结束后应写 L4 取证");
    wd_recovery_set_ota_busy_query(NULL);
}

static void test_reboot_ota_wait_is_bounded(void)
{
    CASE("升级一直不结束：等到上限照常重启");
    reset();
    wd_recovery_set_ota_busy_query(fake_ota_busy);
    g_busy_left = 1000000; g_busy_calls = 0;
    feed(WD_FAULT_TASK_STALL, WD_SEV_FATAL, "health", NULL);
    CHECK(g_restart_count == 1, "到上限应照常重启，实际 %d", g_restart_count);
    CHECK(g_busy_calls <= REBOOT_OTA_WAIT_MAX_S + 3, "等待应有上界，实际问了 %d 次", g_busy_calls);
    wd_recovery_set_ota_busy_query(NULL);
}

#if CONFIG_BITENGINE_PROFILE_DIRECT
/* 恢复引擎停转被应急重启时，历史里记下它当时卡在哪一步（2026-10-24：停转 120 s、板子卡进下载模式，断电后 RTC 里的线索全没了） */
static char g_doing_in_cb[32];
static esp_err_t doing_probe(void)
{
    wd_recovery_doing(g_doing_in_cb, sizeof(g_doing_in_cb));
    return ESP_OK;
}

static void test_emergency_says_what_it_was_doing(void)
{
    CASE("应急重启的历史写明恢复引擎卡在哪一步（直连型）");
    reset();
    feed(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "thread", doing_probe);
    CHECK(strcmp(g_doing_in_cb, "wd_action L2 thread") == 0, "L2 动作执行中：阶段 L2、上报者 thread");
    wd_recovery_emergency("wd_action");
    CHECK(g_rec.lvl == WD_LEVEL_REBOOT && strcmp(g_rec.detail, "wd_action L2 thread") == 0, "应急重启写进历史的是\"卡在哪\"，不是光秃秃的 wd_action");
    char b[24];
    reset();
    s_queue = xQueueCreate(8, sizeof(wd_action_msg_t));       /* 走队列：dispatch 才是 wd_action 的那条路 */
    wd_report_recovered(WD_FAULT_SUBSYS_DEAD, "network");
    drain();
    wd_recovery_doing(b, sizeof(b));
    CHECK(strcmp(b, "wd_action rec network") == 0, "处理\"已恢复\"时：阶段 rec");
    vQueueDelete(s_queue); s_queue = NULL;
    reset();
    feed(WD_FAULT_RESOURCE, WD_SEV_OBSERVE, "a_very_long_reporter_name", NULL);
    wd_recovery_doing(b, sizeof(b));
    CHECK(strlen(b) == sizeof(b) - 1 && strncmp(b, "wd_action L1 a_very", 19) == 0, "放不下的截到 23 字节（= 历史的 detail）");
}
#endif

int main(void)
{
    printf("=== 层 E 恢复引擎 ===\n");
    test_observe_stays_l1();
    test_recover_starts_at_l2();
    test_no_l2_handler_jumps_to_l3();
    test_fatal_reboots_immediately();
    test_cooldown_blocks_escalation();
    test_mg24_never_escalates_past_l2();
    test_network_still_capped_at_l3();
    test_thread_l2_then_reboot();
    test_thread_l2_failure_still_waits();
    test_thread_independent_of_network();
    test_thread_recovered_resets_own_level();
    test_view_shows_thread_level();
    test_thread_no_l2_means_reboot();
    test_recovered_is_queued_in_order();
    test_recovered_applies_in_safe_mode();
    test_cooldown_uses_report_time();
    test_l3_double_failure_reboots();
    test_recovered_resets_level();
    test_safe_mode_suppresses();
    test_safe_mode_exit();
    test_recovery_registry();
    test_bad_class();
    test_queue_full_does_not_block();
    test_reboot_waits_for_ota();
    test_reboot_ota_wait_is_bounded();
    test_boot_reason_survives_extra_restart();
    test_boot_reason_ignores_garbage();
    test_abnormal_cause_strings();
#if CONFIG_BITENGINE_PROFILE_DIRECT
    test_emergency_says_what_it_was_doing();
#endif
    return wd_test_summary("recovery");
}
