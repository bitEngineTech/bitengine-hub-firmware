/* 层 C 探针的主机侧测试 —— 目前只测 Thread 射频探针的上报节奏
 *
 * 为什么存在：2026-10-06 TC-W06 发现 probe_thread() 只上报**一次**，而恢复引擎只在"冷却期过后
 * 又收到上报"时升级，于是 L2 救不回时永远停在 L2，Thread 死到人工重启 —— 整个模块的 VERIFY 与
 * 引擎测试都是绿的，因为它们都没把"探针 × 引擎"连起来看。这里把 wd_probes.c 单独编译、与本文件
 * 链接：本文件的强符号 thread_br_get_health / wd_report 盖掉它的 weak 默认实现与真实引擎，
 * 用可控时钟驱动 wd_probes_tick()，断言上报的时刻与次数。
 */

#include "stub/test_common.h"
#include "watchdog.h"
#include "ezsp_bridge.h"
#include "network_mgr.h"
WD_TEST_GLOBALS

/* ---- 可控的 Thread 健康 ---- */
static bool g_running = true, g_stalled = false;
bool thread_br_get_health(bool *running, bool *stalled)
{
    if (running) *running = g_running;
    if (stalled) *stalled = g_stalled;
    return true;
}
esp_err_t thread_br_radio_recover(void) { return ESP_OK; }

/* ---- 观测上报 ---- */
static int  g_reports, g_recovered;
static char g_last_detail[16];
static esp_err_t (*g_last_recover)(void);
static int64_t g_report_us[16];

static int g_mg24_reports, g_mg24_recovered;

void wd_report(wd_fault_class_t cls, wd_severity_t sev, const char *detail, esp_err_t (*recover)(void))
{
    (void)sev;
    if (cls == WD_FAULT_PERIPH_DEAD) { g_mg24_reports++; return; }
    if (cls != WD_FAULT_SUBSYS_DEAD) return;
    if (g_reports < 16) g_report_us[g_reports] = g_fake_us;
    g_reports++;
    snprintf(g_last_detail, sizeof(g_last_detail), "%s", detail ? detail : "");
    g_last_recover = recover;
}
void wd_report_recovered(wd_fault_class_t cls, const char *detail)
{
    if (cls == WD_FAULT_PERIPH_DEAD) { g_mg24_recovered++; return; }
    if (cls == WD_FAULT_SUBSYS_DEAD && detail && strcmp(detail, "thread") == 0) g_recovered++;
}

/* ---- MG24 探针的可控依赖（T9.31）。默认桥"未运行"，Thread 那几条测试里 MG24 探针走早退分支 ---- */
static bool           g_bridge_running;
static bridge_stats_t g_bstats;
static bool           g_line_absent, g_probe_ok = true;
static int            g_probe_calls;
bridge_state_t        ezsp_bridge_get_state(void) { return g_bridge_running ? BRIDGE_STATE_RUNNING : BRIDGE_STATE_STOPPED; }
const bridge_stats_t *ezsp_bridge_get_stats(void) { return g_bridge_running ? &g_bstats : NULL; }
bool                  ezsp_bridge_health_probe(int timeout_ms) { (void)timeout_ms; g_probe_calls++; return g_probe_ok; }
bool                  uart_bridge_line_says_absent(int window_ms) { (void)window_ms; return g_line_absent; }
esp_err_t             network_mgr_get_status(network_status_t *out) { (void)out; return ESP_FAIL; }
size_t                wd_registry_snapshot(wd_task_view_t *out, size_t max) { (void)out; (void)max; return 0; }

static void reset(void)
{
    g_running = true; g_stalled = false;
    g_reports = g_recovered = 0; g_last_detail[0] = 0; g_last_recover = NULL;
    SET_MS(1000000);
    wd_probes_tick();                       /* 把探针里的 s_reported 带回"没报过" */
    g_reports = g_recovered = 0;
}

/* 健康任务 10 s 一拍（wd_probes.h：由 health_task 每 10s 调用） */
static void tick_for(int seconds)
{
    for (int s = 0; s < seconds; s += 10) { ADVANCE_MS(10000); wd_probes_tick(); }
}

static void test_reports_once_when_stall_starts(void)
{
    CASE("停摆一出现就上报一次，带 L2 手段（射频重启）");
    reset();
    g_stalled = true;
    tick_for(10);
    CHECK(g_reports == 1, "应上报 1 次，实际 %d", g_reports);
    CHECK(strcmp(g_last_detail, "thread") == 0, "detail 应为 thread，实际 %s", g_last_detail);
    CHECK(g_last_recover == thread_br_radio_recover, "第一次上报应带 L2 手段");
}

static void test_rereports_while_stalled(void)
{
    CASE("⚠️ 持续停摆时每 ≥60 s 再报一次（只报一次 = 永远升不过 L2，TC-W06 的老毛病）");
    reset();
    g_stalled = true;
    tick_for(10);                           /* 第一次 */
    tick_for(50);                           /* 第一次之后 50 s：还在 L2 冷却里，不该再报 */
    CHECK(g_reports == 1, "60 s 内不应再报，实际 %d", g_reports);
    tick_for(20);                           /* 过了 60 s */
    CHECK(g_reports == 2, "过了 60 s 应再报一次，实际 %d", g_reports);
    int64_t gap = g_report_us[1] - g_report_us[0];
    CHECK(gap >= 60 * 1000000LL && gap <= 70 * 1000000LL, "两次上报应相隔 60–70 s，实际 %lld us", (long long)gap);
    tick_for(60);
    CHECK(g_reports == 3, "继续停摆应继续每 60 s 报，实际 %d", g_reports);
}

static void test_recovery_clears_and_restarts_cycle(void)
{
    CASE("恢复后报一次 recovered，下次停摆重新从第一次报起");
    reset();
    g_stalled = true;  tick_for(10);
    g_stalled = false; tick_for(10);
    CHECK(g_recovered == 1, "应报 recovered 1 次，实际 %d", g_recovered);
    int before = g_reports;
    tick_for(120);
    CHECK(g_reports == before, "正常运行不应上报，实际多了 %d 次", g_reports - before);
    g_stalled = true;  tick_for(10);
    CHECK(g_reports == before + 1, "再次停摆应立刻上报，实际多了 %d 次", g_reports - before);
}

static void test_stops_running_after_stall_reports_recovered(void)
{
    CASE("⚠️ 报过停摆后 Thread 停了（SoftAP 暂停等）：要告诉引擎这一轮结束，否则级别残留、下一轮越过 L2");
    reset();
    g_stalled = true;  tick_for(10);
    CHECK(g_reports == 1, "应先报停摆");
    g_running = false; tick_for(10);
    CHECK(g_recovered == 1, "停了应报 recovered 1 次，实际 %d", g_recovered);
    tick_for(60);
    CHECK(g_recovered == 1, "不应重复报 recovered，实际 %d", g_recovered);
    g_running = true;  tick_for(10);    /* 回来时仍停摆：重新从第一报开始 */
    CHECK(g_reports == 2, "回来仍停摆应重新上报，实际 %d", g_reports);
}

static void test_not_running_never_reports(void)
{
    CASE("Thread 没在跑（如 SoftAP 配网暂停）不判、不报");
    reset();
    g_running = false; g_stalled = true;
    tick_for(300);
    CHECK(g_reports == 0, "没在跑不应上报，实际 %d", g_reports);
}

/* ---- T9.31 ①：MG24 探针 —— 没有客户端时不发 ASH RST ----
   探针的静态状态跨用例保留，所以按一条时间线走完，每段断言一件事。 */
static void test_mg24_no_client_never_resets(void)
{
    wd_probe_view_t pv;
    g_bridge_running = true; g_bstats.active_clients = 0; g_bstats.uart_rx_bytes = 0;
    g_line_absent = false; g_probe_ok = true; g_probe_calls = 0;
    g_mg24_reports = g_mg24_recovered = 0;

    CASE("无客户端、线路正常：一小时里一次 ASH RST 都不发，显式不判定");
    tick_for(3600);
    wd_probes_get(&pv);
    CHECK(g_probe_calls == 0, "不该探测，实际 %d 次", g_probe_calls);
    CHECK(!pv.mg24_judged && pv.mg24_ok, "应为不判定且保持原值（judged=%d ok=%d）", pv.mg24_judged, pv.mg24_ok);
    CHECK(g_mg24_reports == 0, "不该判死上报");

    CASE("线路连续两拍没驱动 → 判死上报一次");
    g_line_absent = true;
    tick_for(20);
    wd_probes_get(&pv);
    CHECK(!pv.mg24_ok && pv.mg24_judged, "应已判死（judged=%d ok=%d）", pv.mg24_judged, pv.mg24_ok);
    CHECK(g_mg24_reports == 1, "应上报 1 次，实际 %d", g_mg24_reports);

    CASE("线路回来、仍无客户端：线路判的死才做一次确认探测，答了就翻案");
    g_line_absent = false;
    tick_for(10);
    wd_probes_get(&pv);
    CHECK(g_probe_calls == 1, "应确认探测 1 次，实际 %d", g_probe_calls);
    CHECK(pv.mg24_ok && pv.mg24_judged && g_mg24_recovered == 1, "应已翻案（ok=%d recovered=%d）",
          pv.mg24_ok, g_mg24_recovered);
    tick_for(1800);
    CHECK(g_probe_calls == 1, "翻案后无客户端不该再探，实际 %d", g_probe_calls);

    CASE("确认探测失败：限频重试，不是每拍都发");
    g_line_absent = true; tick_for(20);              /* 再判死一次 */
    g_line_absent = false; g_probe_ok = false; g_probe_calls = 0;
    tick_for(120);
    CHECK(g_probe_calls >= 2 && g_probe_calls <= 3, "120 s 里应探 2-3 次（60 s 一次），实际 %d", g_probe_calls);
    g_probe_ok = true; tick_for(60);
    wd_probes_get(&pv);
    CHECK(pv.mg24_ok, "芯片回来后应翻案");

    CASE("客户端回来：判死计时从这一段客户端开始算，不算没有客户端的几小时");
    tick_for(7200);
    g_bstats.active_clients = 1; g_probe_calls = 0; g_mg24_reports = 0;
    tick_for(170);                                   /* 一直收不到字节，但还没满 180 s */
    wd_probes_get(&pv);
    CHECK(pv.mg24_ok && g_mg24_reports == 0, "客户端刚连上 170 s，不该判死");
    tick_for(30);
    wd_probes_get(&pv);
    CHECK(!pv.mg24_ok && g_mg24_reports == 1, "满 180 s 无证据应判死（ok=%d reports=%d）", pv.mg24_ok, g_mg24_reports);

    CASE("这种判死（有客户端却不吭声）在客户端走后不发确认探测（第一轮审计 #12）");
    g_bstats.active_clients = 0;
    tick_for(600);
    wd_probes_get(&pv);
    CHECK(g_probe_calls == 0, "不该发 ASH RST，实际 %d 次", g_probe_calls);
    CHECK(!pv.mg24_ok && !pv.mg24_judged, "应保持 ok=false、不判定（ok=%d judged=%d）", pv.mg24_ok, pv.mg24_judged);

    CASE("客户端回来、收到字节 → 证据路径翻案");
    g_bstats.active_clients = 1; g_bstats.uart_rx_bytes += 100;
    tick_for(10);
    wd_probes_get(&pv);
    CHECK(pv.mg24_ok && pv.mg24_judged, "应翻案");
    CHECK(g_probe_calls == 0, "有客户端时从不探测");

    g_bridge_running = false;                        /* 还给后面的用例 */
}

int main(void)
{
    printf("=== 层 C 探针（Thread 射频）===\n");
    test_reports_once_when_stall_starts();
    test_rereports_while_stalled();
    test_recovery_clears_and_restarts_cycle();
    test_stops_running_after_stall_reports_recovered();
    test_not_running_never_reports();
    printf("=== 层 C 探针（MG24，T9.31）===\n");
    test_mg24_no_client_never_resets();
    return wd_test_summary("probes");
}
