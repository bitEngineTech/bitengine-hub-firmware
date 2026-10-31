/* 层 C — 子系统存活探针（docs/04_watchdog_design_v2.md 第六节）
 *
 * 对付 F3「子系统假死」和 F5「外设失联」。原则：判定"活着"必须有**正向证据**
 * ——计数器在变、探测拿到回应。"没有报错"不算证据，v1 就是栽在这里：LAN 模式
 * 无客户端时它直接 `alive = true`，2026-08-10 那 4 小时里 Zigbee 早就没人管了，
 * 网关一直报 mg24_alive=true。
 */

#include "watchdog.h"
#include "ezsp_bridge.h"

#include "network_mgr.h"

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "wd_probes";

/* 层 H 的两个注入开关此前**没有任何探针去读** —— wd_fault_active() 全仓库零调用点。
 * POST /api/v1/diag/fault {"kind":"mg24_mute"} 会返回 200，设备行为却完全不变，
 * 于是 TC-W04/W05/W06 验的是空气。而 F5（外设失联 / MG24 卡死）和 F3（802.15.4
 * 射频静默死亡）恰恰是本项目现场真实发生过的两类故障，它们的检测与恢复路径
 * 至今没被证明过会响 —— 这正是 wd_fault_inject.c 开篇说要补的那一课，
 * 它自己却漏了这两种。
 *
 * 注入点选在"探针取证据"的那一行，不选在上报处：这样注入出来的现象与真故障
 * 在探针眼里完全一致，检测判据、超时、上报、恢复阶梯会整条走一遍。 */
#if CONFIG_BITENGINE_FAULT_INJECTION
#define FAULT_ACTIVE(k)  wd_fault_active(k)
#else
#define FAULT_ACTIVE(k)  (false)
#endif

/* thread_br 的两个入口用 weak 默认实现，而不是 CMake 依赖：
   thread_br → led_ctrl → watchdog 已经成链，watchdog 再反向依赖 thread_br
   就成环了。thread_br.c 里的强符号在链接时覆盖这里的弱实现；没编入
   thread_br 的构型下探针自动降级为"不判定"。 */
__attribute__((weak)) bool thread_br_get_health(bool *running, bool *stalled)
{
    (void)running; (void)stalled;
    return false;
}
__attribute__((weak)) esp_err_t thread_br_radio_recover(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

/* 没有客户端时**不再主动探测**（T9.31，2026-10-08 用户拍板）。
   唯一的探测手段是 ASH RST，而它会把 NCP 的 Zigbee 协议栈打回未初始化 —— 我们没有能力把它拉起来
   （那需要 write_config，等于造 EZSP 主机，违反 docs/02 决策 1）。普通 USB 协调器的主机离开时网络照跑，
   我们以前却会把它停掉：

     · 2026-08-12：ZHA 卡死约 8 小时，期间 60 s 一次的空闲探测让一只 Aqara 温湿度掉网；
     · 2026-09-15：改成"协调器走后宽限 30 分钟，之后先高频再退避"——只是推迟，不是不停；
     · 2026-10-07：Sengled 灯在网络停了约 11.5 h 之后清掉网络凭据回出厂态，断电重启也回不来
       （C5 旁听抓包 + ZHA 调试日志，docs/guide/Zigbee全掉线排查.md）。

   现在无客户端时只看线路电平（不花钱、只能判死，见下），其余时间显式"不判定"（mg24_judged=false）。
   代价（登记在案）：没有客户端时 MG24 CPU 卡死、USART 还配着的情形看不出来；但那时没人在用它，
   客户端一连上，下面"有客户端"那条证据路径就会判出来。 */
#define MG24_CONFIRM_INTERVAL_S       60    /* 判死后、无客户端时，确认芯片回来的探测间隔 */
#define NET_EVIDENCE_TIMEOUT_S      180

/* 线路电平采样窗口（D）。实测 MG24 运行时 ctsn 恒 0（34000 个采样点零抬起），
   复位那一下只有 63ms，所以 300ms 整窗全高已经远离正常抖动；再要求**连续两个
   tick**（相隔 10s）才下结论，任何合法复位都不可能持续那么久。 */
#define MG24_LINE_SAMPLE_MS          300
#define MG24_LINE_ABSENT_TICKS         2

static wd_probe_view_t s_view = {
    .mg24_ok = true, .mg24_judged = false,
    .thread_ok = true, .network_ok = true, .httpd_ok = true,
};

/* 探测窗口标志。tcp_server.c 在 accept 前查询：为 true 时延迟接受连接，
   避免探测的 ASH RST 打断正在建立的 Z2M 会话。上界 = 探测超时 2.5s。 */
static volatile bool s_mg24_probe_active;

bool wd_mg24_probe_active(void) { return s_mg24_probe_active; }

static inline uint32_t now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

// ---- MG24 ----

#if CONFIG_BITENGINE_ENABLE_ZHUB
#include "zhub.h"

/* 直连型（docs/15 设计 8.1、CHECKLIST K6 / Z47）：网关自己就是 Zigbee 主机，没有"客户端"，也绝不能自己发 ASH RST（会打断 zhub 的会话）。
   证据改由 zhub 自报：配上的 EZSP 应答 / 合法回调（zh_ses_alive_evidence，空闲时有 10 s 一次的保活 nop 撑着）。
   弱符号：zhub 没编进来（不该发生，ENABLE_ZHUB 下它总在）时探针降级为"不判定"。 */
__attribute__((weak)) bool zhub_mg24_evidence(zhub_mg24_view_t *out)
{
    (void)out;
    return false;
}
__attribute__((weak)) esp_err_t zhub_wd_recover(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static void probe_mg24_zhub(void)
{
    static bool     s_reported;
    static uint32_t s_mute_since;                     /* 注入 mg24_mute 从何时起（0 = 没在注入） */
    zhub_mg24_view_t v = { 0 };
    if (!zhub_mg24_evidence(&v) || !v.active) {
        /* zhub 没起 / 挂起中（UART 交给了 MG24 升级、基准测试、bootloader）：取不到证据，judged=false 如实说"这段时间没判"。
           不刷 mg24_last_ok_s：这支探针的判死计时用 zhub 给的证据年龄（接管时从零起算），不读 last_ok；刷成"现在"只会让
           /diag/watchdog 在 MG24 停在 bootloader 几小时的时候也显示刚刚活过 —— docs/04 禁止的伪造形状（第一轮审计） */
        s_view.mg24_judged = false;
        return;
    }
    const uint32_t t = now_s();
    /* 注入 mg24_mute（TC-W04）：抹掉 zhub 的证据，年龄从注入开始时的那份往上长 —— 判死、上报、恢复整条路径与真故障同步 */
    if (FAULT_ACTIVE("mg24_mute")) {
        if (!s_mute_since) s_mute_since = t ? t : 1;
        const uint32_t muted_ms = (t - s_mute_since) * 1000u;
        v.evidence = false;
        if (muted_ms > v.age_ms) v.age_ms = muted_ms;
        ESP_LOGW(TAG, "注入生效: mg24_mute —— 忽略 zhub 的存活证据");
    } else {
        s_mute_since = 0;
    }
    const uint32_t age_s = v.age_ms / 1000;
    if (v.evidence && age_s <= MG24_HEARTBEAT_TIMEOUT_S) {
        s_view.mg24_ok = true;
        s_view.mg24_judged = true;
        s_view.mg24_last_ok_s = t - age_s;            /* 证据的真实时刻，不是"现在" */
        if (s_reported) {
            s_reported = false;
            wd_report_recovered(WD_FAULT_PERIPH_DEAD, "mg24");
        }
        return;
    }
    if (age_s > MG24_HEARTBEAT_TIMEOUT_S) {           /* 计时从"最近一次证据"与"这次接管 UART"里较晚的那个起（zhub 算好给 age） */
        s_view.mg24_ok = false;
        s_view.mg24_judged = true;
        if (!s_reported) {
            s_reported = true;
            ESP_LOGE(TAG, "MG24 已 %"PRIu32"s 没有存活证据（zhub 自报）", age_s);
            /* 定向恢复只是请 zhub 立即复位链路（它本来就在按退避表重试）；PERIPH 封顶 L2，绝不整机重启（docs/04 §6） */
            wd_report(WD_FAULT_PERIPH_DEAD, WD_SEV_RECOVER, "mg24", zhub_wd_recover);
        }
        return;
    }
    s_view.mg24_judged = false;                       /* 接管不久、还没有证据：不判定 */
}
#else
/* 定向恢复（L2）：无客户端时发一帧 ASH RST 让 NCP 自己重来。
   有客户端时拒绝——那会打断 Z2M 的 ASH 会话。整机/硬件复位一律不做，
   AM1 上硬件复位会带走整板且可能丢配网（策略表里 PERIPH 封顶 L2）。 */
static esp_err_t mg24_recover(void)
{
    const bridge_stats_t *st = ezsp_bridge_get_stats();
    if (st && st->active_clients > 0) {
        ESP_LOGW(TAG, "MG24 恢复跳过：%d 个客户端持有 ASH 会话",
                 st->active_clients);
        return ESP_ERR_INVALID_STATE;
    }
    s_mg24_probe_active = true;
    bool ok = ezsp_bridge_health_probe(MG24_EZSP_QUERY_TIMEOUT_MS);
    s_mg24_probe_active = false;
    return ok ? ESP_OK : ESP_FAIL;
}

static void probe_mg24(void)
{
    static uint32_t s_prev_uart_rx;
    static bool     s_reported;
    static uint32_t s_line_absent;      /* 连续几个 tick 线路显示未驱动（D） */
    static uint32_t s_client_since_s;   /* 这一段"有客户端"从何时开始；0 = 现在没有 */
    static uint32_t s_last_confirm_s;   /* 上次"判死后确认"探测的时刻 */
    static bool     s_dead_by_line;     /* 这次判死是线路电平给的（芯片复位过 / 掉过电） */

    if (ezsp_bridge_get_state() != BRIDGE_STATE_RUNNING) {
        /* OTA/模式切换中：UART 归别人用，取不到证据。
           ⚠️ 这里刷 last_ok 是**保住判死计时**，不是证据 —— 不刷的话一次 15s 的
           MG24 OTA 结束后 last_ok 已经陈旧，下一个 tick 直接判死并复位，正好打断
           刚装完的固件。所以 mg24_ok 维持原值而把 judged 置 false：对外如实说
           "这段时间没判"，别让 /status 把它读成"已确认存活"。 */
        s_view.mg24_judged = false;
        s_view.mg24_last_ok_s = now_s();
        return;
    }

    const bridge_stats_t *st = ezsp_bridge_get_stats();
    if (!st) return;

    uint32_t t = now_s();
    bool evidence = false;

    if (st->active_clients > 0) {
        /* 有客户端：UART 收字节在涨就是最好的证据，不需要打扰会话 */
        if (!s_client_since_s) s_client_since_s = t ? t : 1;
        s_line_absent = 0;
        if (st->uart_rx_bytes != s_prev_uart_rx) {
            s_prev_uart_rx = st->uart_rx_bytes;
            evidence = true;
        }
    } else {
        s_client_since_s = 0;

        /* ---- 无客户端：只要一份不花钱的负面证据 ----
           读 UART_STATUS_REG 的 ctsn，不发一个字节、不碰协议。整窗解除断言且
           连续两个 tick 都如此 = MG24 的 USART 压根没配起来（复位中/掉电）。
           这一条是**判死**用的，不能反过来当"活着"的证据：CPU 死循环时 USART
           外设照样把 RTS 摁着。 */
        if (uart_bridge_line_says_absent(MG24_LINE_SAMPLE_MS)) {
            if (++s_line_absent < MG24_LINE_ABSENT_TICKS) {
                ESP_LOGW(TAG, "MG24 线路电平显示未驱动（%u/%u tick）",
                         (unsigned)s_line_absent, MG24_LINE_ABSENT_TICKS);
                return;             /* 先不下结论，下一个 tick 再看 */
            }
            ESP_LOGE(TAG, "MG24 线路电平连续 %u 个 tick 显示未驱动 —— 判死",
                     (unsigned)s_line_absent);
            s_view.mg24_ok = false;
            s_view.mg24_judged = true;
            s_dead_by_line = true;
            if (!s_reported) {
                s_reported = true;
                wd_report(WD_FAULT_PERIPH_DEAD, WD_SEV_RECOVER, "mg24", mg24_recover);
            }
            return;
        }
        s_line_absent = 0;

        if (s_view.mg24_ok || !s_dead_by_line) {
            /* 不探就不判：既不宣布活，也不宣布死。⚠️ 别把 mg24_last_ok_s 刷成现在 —— 那是伪造证据，
               正是本文件开篇痛批的 v1 罪状；客户端回来后的判死计时由 s_client_since_s 起算（见下）。 */
            s_view.mg24_judged = false;
            return;
        }

        /* 线路电平判过死（芯片复位过 / 掉过电）：它的协议栈本来就没了，这时发 ASH RST 不会再打掉
           任何东西。拿它确认芯片回来没有 —— 否则 MG24 自己复位一次之后，没有客户端时 mg24_ok 会
           一直停在 false，直到有人连上来。限频，别在芯片真坏时每拍都发。
           ⚠️ 只认线路给的判死（第一轮审计 #12）：有客户端时"长时间收不到字节"判的死证明不了协议栈
           没了（ZHA连接不自愈.md 记过 tcp_clients=1 但 rx 不涨、NCP 好好的），那种客户端一走就发
           RST，正是 T9.31 要避免的。那种情况走上面的"不判定"分支：mg24_ok 留在 false，等客户端回来由
           证据路径翻案。 */
        if (s_last_confirm_s && t - s_last_confirm_s < MG24_CONFIRM_INTERVAL_S) {
            return;                         /* 维持"已判死" */
        }
        s_last_confirm_s = t;
        s_mg24_probe_active = true;
        evidence = ezsp_bridge_health_probe(MG24_EZSP_QUERY_TIMEOUT_MS);
        s_mg24_probe_active = false;
        if (!evidence) {
            ESP_LOGW(TAG, "MG24 判死后的确认探测仍无 RSTACK");
            s_view.mg24_judged = true;
            return;
        }
        ESP_LOGI(TAG, "MG24 判死后的确认探测有 RSTACK —— 芯片回来了");
    }

    /* 注入 mg24_mute：抹掉本轮取到的存活证据。放在这里而不是函数开头，是为了
       保留原有的节奏（判死后的确认探测仍按 MG24_CONFIRM_INTERVAL_S 走），
       这样判死所需的 MG24_HEARTBEAT_TIMEOUT_S 与真故障完全同步。
       ⚠️ 无客户端时没有证据可抹（T9.31 起不再主动探测），所以注入只在有客户端时有效（TC-W04）。 */
    if (evidence && FAULT_ACTIVE("mg24_mute")) {
        evidence = false;
        ESP_LOGW(TAG, "注入生效: mg24_mute —— 忽略本轮存活证据");
    }

    if (evidence) {
        s_view.mg24_ok = true;
        s_view.mg24_judged = true;
        s_view.mg24_last_ok_s = t;
        s_dead_by_line = false;
        if (s_reported) {
            s_reported = false;
            wd_report_recovered(WD_FAULT_PERIPH_DEAD, "mg24");
        }
        return;
    }

    /* 无证据（只会出现在有客户端时）：超过心跳超时才判死。计时从"上次证据"与"这一段客户端开始"
       里较晚的那个算 —— 没有客户端的那几个小时不算数，否则 ZHA 一回来、第一拍还没收到字节就判死。 */
    uint32_t base = s_view.mg24_last_ok_s > s_client_since_s ? s_view.mg24_last_ok_s
                                                              : s_client_since_s;
    if (base && (t - base) > MG24_HEARTBEAT_TIMEOUT_S) {
        s_view.mg24_ok = false;
        s_view.mg24_judged = true;
        if (!s_reported) {
            s_reported = true;
            ESP_LOGE(TAG, "MG24 已 %"PRIu32"s 没有存活证据（有客户端期间）", t - base);
            wd_report(WD_FAULT_PERIPH_DEAD, WD_SEV_RECOVER, "mg24", mg24_recover);
        }
    }
}
#endif /* CONFIG_BITENGINE_ENABLE_ZHUB */

// ---- Thread 射频 ----

/* 持续停摆时多久再报一次。恢复引擎只在"冷却期过后又收到上报"时升级，所以只报一次 = 永远停在 L2
   （2026-10-06 TC-W06 实测：L2 救不回，Thread 死到有人手动重启）。不能短于 L2 的冷却（60 s，
   wd_recovery.c 的 s_cooldown_s）：冷却里的上报只换来一条"冷却中"日志。VERIFY 钉着这两个数。
   探针 10 s 一拍，所以实际间隔是 60–70 s；第二次上报就升到 L4 整机重启（Thread 没有 L3，T9.18）。 */
#define THREAD_REREPORT_S 60

/* thread_br 的 2s 定时器已经在持 OT 锁时采样 MAC 计数器（child_stats_timer_cb），
   这里只读它的缓存结论，不再自己取锁——在 OT mainloop 之外抢锁会拖慢射频。 */
static void probe_thread(void)
{
    static bool     s_reported;
    static uint32_t s_last_report_s;

    bool running = false, stalled = false;
    if (!thread_br_get_health(&running, &stalled)) {
        return;    /* thread_br 未编入或未初始化 */
    }

    /* 注入 thread_radio_freeze：等价于 thread_br 的 MAC 计数器不再增长。
       注意要放在 !running 的早退**之前**判 —— 真实的射频静默死亡正是
       "OT 协议栈还活着（running=true）、只有射频哑了"，见 memory 里那条
       "802.15.4 射频会静默死掉而 OT 协议栈还活着"。 */
    if (FAULT_ACTIVE("thread_radio_freeze")) {
        stalled = true;
        ESP_LOGW(TAG, "注入生效: thread_radio_freeze —— 判为射频停摆");
    }

    s_view.thread_running = running;
    s_view.thread_ok = !stalled;

    if (!running) {           /* 没跑就没什么可判的（SoftAP 配网期会暂停） */
        /* 报过停摆的话要告诉引擎这一轮结束了：只清自己的标志，引擎里 Thread 的级别就停在 L2，
           几小时后另一次独立的停摆第一报就越过 L2 直接整机重启（2026-10-07 审计）。
           主循环意外退出也会走到这里 —— 它自己已经报过 L4，这条 recovered 排在后面、不影响。 */
        if (s_reported) {
            s_reported = false;
            wd_report_recovered(WD_FAULT_SUBSYS_DEAD, "thread");
        }
        return;
    }

    uint32_t t = now_s();
    if (stalled && (!s_reported || t - s_last_report_s >= THREAD_REREPORT_S)) {
        if (!s_reported) {
            ESP_LOGE(TAG, "Thread 射频停摆：MAC 计数器长时间不动");
        } else {
            ESP_LOGW(TAG, "Thread 射频仍停摆（距上次上报 %"PRIu32" s）—— 再报一次，引擎据此升级",
                     t - s_last_report_s);
        }
        s_reported = true;
        s_last_report_s = t;
        /* 第一次：L2 射频重启（thread_br 提供）。冷却后仍停摆：L4 整机重启 ——
           Thread 没有 L3，整栈重启正常态实测不可用（wd_recovery.c 的 s_subsys_override） */
        wd_report(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "thread",
                  thread_br_radio_recover);
    } else if (!stalled && s_reported) {
        s_reported = false;
        wd_report_recovered(WD_FAULT_SUBSYS_DEAD, "thread");
    }
}

// ---- 网络 ----

static void probe_network(void)
{
    static uint32_t s_last_ok_s;
    static bool     s_reported;

    network_status_t ns;
    if (network_mgr_get_status(&ns) != ESP_OK) return;

    uint32_t t = now_s();

    /* 正向证据：活动接口拿到了 IP。只看 link up 不够——DM9051 出过
       "link 报 up 但不出包"的情况（以太网路径系统性未验证那一轮）。
       SoftAP 兜底态也算活着：用户至少能连上来配网。 */
    bool ok = (ns.active_interface == NET_IF_ETH  && ns.eth_has_ip)  ||
              (ns.active_interface == NET_IF_WIFI && ns.wifi_has_ip) ||
              ns.softap_active;

    s_view.network_ok = ok;
    if (ok) {
        s_last_ok_s = t;
        if (s_reported) {
            s_reported = false;
            wd_report_recovered(WD_FAULT_SUBSYS_DEAD, "network");
        }
        return;
    }

    if (s_last_ok_s && (t - s_last_ok_s) > NET_EVIDENCE_TIMEOUT_S && !s_reported) {
        s_reported = true;
        ESP_LOGE(TAG, "网络已 %"PRIu32"s 无可用地址", t - s_last_ok_s);
        /* network_mgr 的状态机自己会重试，这里只升级到 L3 让它整体重来 */
        wd_report(WD_FAULT_SUBSYS_DEAD, WD_SEV_RECOVER, "network", NULL);
    }
}

// ---- 汇总 ----

void wd_probes_tick(void)
{
#if CONFIG_BITENGINE_ENABLE_ZHUB
    probe_mg24_zhub();
#else
    probe_mg24();
#endif
    probe_thread();
    probe_network();

    /* httpd 的存活由层 B 的心跳槽负责（ws_push 把 kick 投进 httpd 工作队列），
       这里只把结论镜像到探针视图，供 /api/v1/diag/watchdog 一处读全。 */
    wd_task_view_t tv[WD_MAX_TASKS];
    size_t n = wd_registry_snapshot(tv, WD_MAX_TASKS);
    s_view.httpd_ok = true;
    for (size_t i = 0; i < n; i++) {
        if (tv[i].name && strstr(tv[i].name, "httpd") && tv[i].miss_count > 0) {
            s_view.httpd_ok = false;
        }
    }
}

void wd_probes_get(wd_probe_view_t *out)
{
    if (out) *out = s_view;
}
