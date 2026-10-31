/* 层 E — 恢复引擎（docs/04_watchdog_design_v2.md 第八节）
 *
 * 所有故障上报汇聚到这里，由策略表决定做什么。三条原则：
 *
 *   P3 最小恢复 —— 能重启一个子系统就不重启整机；能重启整机就不动硬件复位。
 *   P4 恢复有界 —— 每级有冷却、有次数上限；超限进安全模式而不是无限重启。
 *   P7 不伤业务 —— 恢复动作不得破坏 Zigbee 配网、Thread 数据集、进行中的 OTA。
 *
 * ⚠️ 上报路径（wd_report）必须是非阻塞的：它会被 pri-22 的扫描任务调用，
 *    在那里执行 webui_httpd_restart() 这种秒级操作会连喂狗一起卡死。所以
 *    wd_report 只入队，真正的动作在 wd_action 任务里跑。
 *
 * ⚠️ wd_action 必须用**内部栈**（普通 xTaskCreate，不能用 psram_task_create）：
 *    它会调 wd_forensics_record() 写 NVS，而擦写 Flash 期间 cache 关闭，
 *    栈在 PSRAM 上会直接挂掉（见 psram_task.h 的约束 1）。
 *
 * L5（IO27→MR 硬件复位）本版本不实现，等 AM2。
 */

#include "watchdog.h"
#include "config_mgr.h"    /* config_mgr_flush：重启前把防抖中的配置写进去 */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_system.h"

static const char *TAG = "wd_recovery";

/* ---- 冷却（秒），逐级 5 倍 ---- */
static const uint32_t s_cooldown_s[] = {
    [WD_LEVEL_NONE]   = 0,
    [WD_LEVEL_LOG]    = 0,
    [WD_LEVEL_TARGET] = 60,
    [WD_LEVEL_SUBSYS] = 300,
    [WD_LEVEL_REBOOT] = 900,
};

#define SELF_HEAL_MAX 3      /* 与 wd_forensics.c 保持一致，VERIFY.sh 断言此常量 */

/* ---- 策略表：每个故障类允许升到的最高级别 ----
 *
 * MG24 那一行是刻意的例外：AM1 板上硬件复位会把整板带走（IO23 经 R113 反灌
 * RS706 RESET，再经 R58 拖垮 C5 的 EN），而 ASH RST 会重建 Zigbee 网络导致
 * 配网丢失。宁可让 Zigbee 一直报离线让用户看见，也不自动做可能丢配网的动作。
 * no_auto_reset —— 这一行不要在没有 AM2 硬件的情况下改。
 */
typedef struct {
    const char *name;
    wd_level_t  max_level;
} wd_policy_t;

static const wd_policy_t s_policy[WD_FAULT_CLASS_MAX] = {
    [WD_FAULT_NONE]        = { "none",     WD_LEVEL_NONE   },
    [WD_FAULT_TASK_STALL]  = { "task",     WD_LEVEL_REBOOT },
    [WD_FAULT_SUBSYS_DEAD] = { "subsys",   WD_LEVEL_SUBSYS },
    [WD_FAULT_RESOURCE]    = { "resource", WD_LEVEL_REBOOT },
    [WD_FAULT_PERIPH_DEAD] = { "periph",   WD_LEVEL_TARGET },  /* MG24: no_auto_reset */
};

#if CONFIG_BITENGINE_PROFILE_DIRECT
/* 恢复引擎此刻在做什么：它停转被应急重启时写进 NVS 历史（detail 是 "wd_action <阶段> <上报者>"）。2026-10-24 15:23 它停转 120 s 后
   应急重启、板子卡进下载模式，断电救回来以后 L1 / L2 的记录（只在 RTC）和最后的日志全没了，查不出卡在哪 */
static const char *volatile s_doing = "idle";
static const char *volatile s_doing_det = "";
#define DOING(what, det) do { s_doing = (what); s_doing_det = (det) ? (det) : ""; } while (0)
#else
#define DOING(what, det) do { } while (0)
#endif

const char *wd_fault_class_name(wd_fault_class_t cls)
{
#if CONFIG_BITENGINE_PROFILE_DIRECT
    if (cls == WD_FAULT_PLANNED) return "planned";
#endif
    return ((unsigned)cls < WD_FAULT_CLASS_MAX) ? s_policy[cls].name : "unknown";
}

/* ---- 运行状态 ---- */
typedef struct {
    wd_level_t level;
    uint32_t   last_action_s;
    uint8_t    fail_streak;     /* 本级恢复动作连续失败次数 */
} wd_class_state_t;

static wd_class_state_t s_state[WD_FAULT_CLASS_MAX];

/* ---- 按子系统覆盖：同属一个故障类、阶梯却不一样的子系统 ----
 *
 * 策略表按故障类封顶，而"子系统假死"这一类里各家的 L3 并不一样安全。覆盖项按 detail 子串匹配，
 * 有自己的级别与冷却（s_ov_state）—— 不与同类共用一份：共用的话，Thread 在 L2 冷却期间网络来报故障
 * 会被当成"复发、冷却中"吞掉，网络一恢复又把 Thread 的级别清零，两边互相打断。
 *
 * thread —— L2（射频 sleep/enable）之后直接 L4，没有 L3（T9.18，2026-10-07 用户拍板方案 A）。
 *   原来的 L3 是 thread_br_stop() → start() 整栈重启，正常态实测不可用：IDF 侧三个缺陷，其一会 panic
 *   （记录在 components/webui/otbr_api.c 末尾）。整机重启是唯一实测干净的恢复。误判的代价由
 *   "1 小时内自愈 3 次进安全模式"兜住（安全模式不起 thread_br，docs/04 §8.3）；判据本身是 MAC 收发
 *   计数器 180 s 全不动，而孤立单节点也每 ≤32 s 发一次 MLE 广播（docs/04 §6.2）。 */
typedef struct {
    const char *key;          /* detail 子串 */
    wd_level_t  max_level;    /* 取代该故障类的封顶 */
    bool        skip_subsys;  /* 没有可用的 L3：该进 L3 时直接进 L4 */
} wd_subsys_override_t;

static const wd_subsys_override_t s_subsys_override[] = {
    { "thread", WD_LEVEL_REBOOT, true },
};
#define WD_OVERRIDE_N (sizeof(s_subsys_override) / sizeof(s_subsys_override[0]))

static wd_class_state_t s_ov_state[WD_OVERRIDE_N];

/* 只有"子系统假死"类有覆盖。返回下标，没有返回 -1。 */
static int override_of(wd_fault_class_t cls, const char *detail)
{
    if (cls != WD_FAULT_SUBSYS_DEAD || !detail) return -1;
    for (size_t i = 0; i < WD_OVERRIDE_N; i++) {
        if (strstr(detail, s_subsys_override[i].key)) return (int)i;
    }
    return -1;
}
static const char      *s_last_action = "none";
static uint32_t         s_last_action_ts;
static bool             s_safe_mode;
static char             s_safe_reason[48];

/* ---- 动作队列 ----
 * 上报与"已恢复"都走这一条队列，级别状态只在 wd_action 任务里改：两条路并行改的话，wd_action 正忙
 * （mg24_recover 最长 2.5 s、httpd 重启更久）时会错序 —— 恢复先生效、排在后面的上报又把级别设回 L2
 * 并残留下来，Thread 下一轮停摆就跳过 L2 直接整机重启（2026-10-07 审计）。 */
typedef struct {
    wd_fault_class_t cls;
    wd_severity_t    sev;
    const char      *detail;          /* 字符串字面量 */
    esp_err_t      (*recover)(void);
    uint32_t         t;               /* 上报时刻（秒）。冷却按它算，不按处理时刻：wd_action 正忙时
                                         处理会晚几秒，按处理时刻算会把准点的再报当成"冷却中"吞掉 */
    bool             recovered;       /* true = wd_report_recovered() */
} wd_action_msg_t;

static QueueHandle_t s_queue;
static int           s_action_slot = -1;   /* 恢复引擎自己的心跳槽 */

static inline uint32_t now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

// ============================================================
// 上报入口（非阻塞，可从任何任务调用）
// ============================================================

void wd_report(wd_fault_class_t cls, wd_severity_t sev, const char *detail,
               esp_err_t (*recover)(void))
{
    if (cls <= WD_FAULT_NONE || cls >= WD_FAULT_CLASS_MAX) return;
    if (!s_queue) return;          /* 引擎还没起来：丢弃，日志已在上报方打过 */

    wd_action_msg_t m = { cls, sev, detail ? detail : "?", recover, now_s(), false };
    /* 0 超时：宁可丢一条上报，也不能阻塞 pri-22 的扫描任务 */
    if (xQueueSend(s_queue, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "动作队列满，丢弃上报 %s/%s", s_policy[cls].name, m.detail);
    }
}

static void recovered_apply(wd_fault_class_t cls, const char *detail)
{
    int ov = override_of(cls, detail);
    wd_class_state_t *st = (ov >= 0) ? &s_ov_state[ov] : &s_state[cls];
    if (st->level == WD_LEVEL_NONE) return;

    ESP_LOGI(TAG, "%s/%s 恢复正常，级别 L%d → L0",
             s_policy[cls].name, detail ? detail : "?", (int)st->level);
    st->level       = WD_LEVEL_NONE;
    st->fail_streak = 0;
}

void wd_report_recovered(wd_fault_class_t cls, const char *detail)
{
    if (cls <= WD_FAULT_NONE || cls >= WD_FAULT_CLASS_MAX) return;
    /* 入队，与上报保持先后顺序（见 wd_action_msg_t 的头注）。引擎没起来或队列满时才就地改 ——
       队列满时丢掉"已恢复"比丢上报更糟：级别会残留，下一轮直接越级。 */
    wd_action_msg_t m = { cls, WD_SEV_OBSERVE, detail ? detail : "?", NULL, now_s(), true };
    if (s_queue && xQueueSend(s_queue, &m, 0) == pdTRUE) return;
    recovered_apply(cls, m.detail);
}

// ============================================================
// 动作执行（wd_action 任务上下文，可以阻塞）
// ============================================================

/* ---- L3 子系统重启回调表 ----
 *
 * 不直接调 webui_httpd_restart() 等：webui 等组件已经
 * REQUIRES watchdog，反向依赖会形成 CMake 循环。改由各子系统在自己的 init
 * 里登记恢复入口，watchdog 只按 key 匹配。key 用子串匹配 detail，所以
 * detail 里带上子系统名即可（如任务名 "ot_main" 对应 key "ot_main"）。
 */
#define WD_RECOVERY_MAX 8

typedef struct {
    const char *key;
    esp_err_t (*fn)(void);
} wd_recovery_entry_t;

static wd_recovery_entry_t s_recovery[WD_RECOVERY_MAX];
static int                 s_recovery_count;

esp_err_t wd_register_recovery(const char *key, esp_err_t (*fn)(void))
{
    if (!key || !fn) return ESP_ERR_INVALID_ARG;
    if (s_recovery_count >= WD_RECOVERY_MAX) {
        ESP_LOGE(TAG, "恢复回调表已满，%s 未登记", key);
        return ESP_ERR_NO_MEM;
    }
    s_recovery[s_recovery_count].key = key;
    s_recovery[s_recovery_count].fn  = fn;
    s_recovery_count++;
    ESP_LOGI(TAG, "登记恢复入口: %s", key);
    return ESP_OK;
}

static esp_err_t subsys_restart(const char *detail)
{
    if (!detail) return ESP_ERR_NOT_FOUND;
    for (int i = 0; i < s_recovery_count; i++) {
        if (strstr(detail, s_recovery[i].key)) {
            ESP_LOGW(TAG, "L3 → %s()", s_recovery[i].key);
            return s_recovery[i].fn();
        }
    }
    ESP_LOGW(TAG, "L3 无对应恢复入口: %s", detail);
    return ESP_ERR_NOT_FOUND;
}

/* ---- 整机重启前等升级写完（T9.30 ①）----
   Thread 停摆之类的 L4 恰好撞上升级时，以前会在写 flash 中途重启。后果可恢复（C5 有 A/B 分区，
   MG24 有"已在 bootloader"的重试路径），但 P7 写着"不得破坏进行中的 OTA"。
   在**写取证之前**等：网页手动上传写完会自己重启，那时 L4 的取证若已写下，下次开机就会被当成
   看门狗自愈、计进安全模式的窗口。等的期间照常 kick 自己的槽位（deadline 120 s），否则应急路径
   会因为"恢复引擎停转"直接重启，等于没等。 */
#define REBOOT_OTA_WAIT_MAX_S   600     /* 云端升级下载 app + www 再刷 NCP，也在几分钟内 */

static bool (*s_ota_busy)(void);

void wd_recovery_set_ota_busy_query(bool (*busy)(void))
{
    s_ota_busy = busy;
}

static void wait_for_ota(void)
{
    if (!s_ota_busy || !s_ota_busy()) return;
    DOING("ota", s_doing_det);
    ESP_LOGW(TAG, "L4 整机重启推迟：有升级正在写 flash（最多等 %d s）", REBOOT_OTA_WAIT_MAX_S);
    uint32_t waited = 0;
    while (s_ota_busy() && waited < REBOOT_OTA_WAIT_MAX_S) {
        wd_kick(s_action_slot);
        vTaskDelay(pdMS_TO_TICKS(1000));
        waited++;
    }
    if (s_ota_busy()) {
        ESP_LOGE(TAG, "等了 %"PRIu32" s 升级仍未结束 —— 照常重启", waited);
    } else {
        ESP_LOGW(TAG, "升级已结束（等了 %"PRIu32" s），继续整机重启", waited);
    }
}

static void do_reboot(wd_fault_class_t cls, const char *detail)
{
    ESP_LOGE(TAG, "L4 整机重启：%s/%s", s_policy[cls].name, detail);

    /* 0. 有升级在写 flash 就先等它写完 */
    wait_for_ota();

    /* 1. 先写取证（RTC + NVS），否则重启后查不出原因 */
    wd_forensics_record(cls, WD_LEVEL_REBOOT, detail);

    /* 2. 给 webhook / MQTT 一个把消息发出去的机会（带上是哪一类、哪个子系统，T9.30 ④） */
    wd_abnormal_boot_t ev = { .reset_reason = 0 };
    wd_abnormal_cause_watchdog(&ev, cls, detail);
    esp_event_post(WATCHDOG_EVENT, WATCHDOG_EVENT_ABNORMAL_BOOT, &ev, sizeof(ev), 0);
    vTaskDelay(pdMS_TO_TICKS(300));

    /* 3. 防抖中的配置写进去（拿不到锁就算了：持锁的任务可能正是卡死的那个），再让日志刷出去 */
    (void)config_mgr_flush(500);
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_restart();
    /* L5（IO27→MR 硬件复位）等 AM2；在此之前 esp_restart 不生效就只能停在这里 */
}

static void handle(const wd_action_msg_t *m)
{
    int ov = override_of(m->cls, m->detail);
    wd_class_state_t *st = (ov >= 0) ? &s_ov_state[ov] : &s_state[m->cls];
    const wd_policy_t *pol = &s_policy[m->cls];
    uint32_t t = m->t;      /* 上报时刻，见 wd_action_msg_t */

    /* 级别语义（TC-W07 暴露后订正）：
         OBSERVE —— 永远停在 L1，只记录。想"只记录"就用这一级。
         RECOVER —— 第一次确认故障就要动手，起点是 L2 而不是 L1。之前从 L1
                    起步意味着单次故障只会打条日志，等故障自己消失级别又归零，
                    恢复动作永远轮不到（httpd 被堵 90 秒实测就是这样空转的）。
                    deadline 本身已经是"坏了这么久才算数"的门槛，不需要再多
                    观察一轮。
         FATAL   —— 直接 L4。 */
    wd_level_t want = (m->sev == WD_SEV_OBSERVE) ? WD_LEVEL_LOG
                    : (m->sev == WD_SEV_FATAL)   ? WD_LEVEL_REBOOT
                    : (st->level < WD_LEVEL_TARGET ? WD_LEVEL_TARGET
                                                   : (wd_level_t)(st->level + 1));
    if (want < WD_LEVEL_LOG)   want = WD_LEVEL_LOG;

    /* L2 只在真的有定向手段时才存在。没有 recover 回调却停在 L2，等于白白
       浪费一整个冷却周期什么也不做——TC-W07（httpd 被堵）就是这样卡住的：
       它有 L3 重启入口但没有 L2 手段，第一次上报停在 L2，等故障自己消失后
       级别归零，L3 永远轮不到。没有 L2 手段就直接进 L3。 */
    if (want == WD_LEVEL_TARGET && !m->recover) {
        want = WD_LEVEL_SUBSYS;
    }

    /* 覆盖项声明没有可用的 L3（如 thread）：该进 L3 时直接进 L4，仍受下面的封顶约束 */
    if (ov >= 0 && s_subsys_override[ov].skip_subsys && want == WD_LEVEL_SUBSYS) {
        want = WD_LEVEL_REBOOT;
    }

    wd_level_t max_level = (ov >= 0) ? s_subsys_override[ov].max_level : pol->max_level;
    if (want > max_level) want = max_level;

    /* 冷却：冷却期**过后**再次上报才升级；冷却期内的复发只记一条日志、维持当前级别 */
    if (st->level != WD_LEVEL_NONE && st->last_action_s &&
        (t - st->last_action_s) < s_cooldown_s[st->level]) {
        ESP_LOGW(TAG, "%s/%s 复发但 L%d 冷却中（剩 %"PRIu32"s），不升级",
                 pol->name, m->detail, (int)st->level,
                 s_cooldown_s[st->level] - (t - st->last_action_s));
        return;
    }

    st->level         = want;
    st->last_action_s = t;
    s_last_action_ts  = t;

    switch (want) {
    case WD_LEVEL_LOG:
        DOING("L1", m->detail);
        s_last_action = "L1 log";
        ESP_LOGW(TAG, "L1 %s/%s", pol->name, m->detail);
        wd_forensics_record(m->cls, WD_LEVEL_LOG, m->detail);
        break;

    case WD_LEVEL_TARGET: {
        DOING("L2", m->detail);
        s_last_action = "L2 targeted";
        ESP_LOGW(TAG, "L2 定向修复 %s/%s", pol->name, m->detail);
        wd_forensics_record(m->cls, WD_LEVEL_TARGET, m->detail);   /* 先记后做，理由见 L3 */
        esp_err_t err = m->recover ? m->recover() : ESP_ERR_NOT_SUPPORTED;
        if (err != ESP_OK) {
            st->fail_streak++;
            ESP_LOGW(TAG, "L2 失败(%s)，streak=%u", esp_err_to_name(err), st->fail_streak);
            /* 失败不立即升级：下一次上报到来时冷却已过，自然升到 L3 */
        } else {
            st->fail_streak = 0;
        }
        break;
    }

    case WD_LEVEL_SUBSYS: {
        DOING("L3", m->detail);
        s_last_action = "L3 subsystem restart";
        ESP_LOGE(TAG, "L3 子系统重启 %s/%s", pol->name, m->detail);
        /* 先记后做（同 do_reboot）：恢复动作自己崩了，history 里至少留着"死在哪个 L3 里"。
           2026-10-07 httpd 的 L3 在本任务栈上溢出 panic，那时记录在动作之后，history 一条没留，
           复位原因又报成 SOFTWARE —— 外部只看得到 uptime 归零。记录不含成败，挪前面不丢信息。 */
        wd_forensics_record(m->cls, WD_LEVEL_SUBSYS, m->detail);
        esp_err_t err = subsys_restart(m->detail);
        if (err != ESP_OK) {
            st->fail_streak++;
            ESP_LOGE(TAG, "L3 失败(%s)，streak=%u", esp_err_to_name(err), st->fail_streak);
            /* 连续两次 L3 无效 → 升到 L4（若策略允许） */
            if (st->fail_streak >= 2 && max_level >= WD_LEVEL_REBOOT) {
                do_reboot(m->cls, m->detail);
            }
        } else {
            st->fail_streak = 0;
        }
        break;
    }

    case WD_LEVEL_REBOOT:
        DOING("L4", m->detail);
        s_last_action = "L4 reboot";
        do_reboot(m->cls, m->detail);
        break;

    default:
        break;
    }
}

/* 谁来看着看门狗自己？
 *
 * L2/L3 的恢复动作是任意回调，可能阻塞任意久——TC-W07 实测 webui_httpd_restart()
 * 会卡死在 httpd_stop() 里，恢复引擎从此失效，后续所有故障入队无人处理，而
 * 外部完全看不出来（端点照样响应）。所以引擎自己也必须被心跳监视。
 *
 * 但它的停转不能走 wd_report——那条路正好要经过它自己的队列。这里提供一条
 * 直达路径，由 pri-22 的扫描任务直接调用。 */
int wd_recovery_action_slot(void) { return s_action_slot; }

void wd_recovery_emergency(const char *detail)
{
    ESP_LOGE(TAG, "恢复引擎自身停转（%s）——直接重启，不经过队列", detail);
    /* 在 pri-22 扫描任务上下文里写 NVS：该任务是内部栈，cache 关闭期间安全。 */
#if CONFIG_BITENGINE_PROFILE_DIRECT
    char what[24];                                  /* = wd_forensic_rec_t.detail；放不下的截掉 */
    wd_recovery_doing(what, sizeof(what));
    wd_forensics_record(WD_FAULT_TASK_STALL, WD_LEVEL_REBOOT, what);
#else
    wd_forensics_record(WD_FAULT_TASK_STALL, WD_LEVEL_REBOOT, "wd_action");
#endif
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

/* 队列里取出的一条消息怎么处理。单独成函数是为了让主机侧测试走同一份分发逻辑。 */
static void dispatch(const wd_action_msg_t *m)
{
    DOING(m->recovered ? "rec" : "msg", m->detail);
    if (m->recovered) {             /* 只改级别，安全模式下也照做 */
        recovered_apply(m->cls, m->detail);
        return;
    }
    if (s_safe_mode) {
        /* 安全模式下不再自愈：已经证明重启解决不了问题。（esp_restart 是 CPU 复位、不重采
           strap，本身不会卡进下载模式；AM1 上会卡的是芯片级复位，见硬件缺陷档案 X6。） */
        ESP_LOGW(TAG, "安全模式：忽略 %s/%s 的恢复动作",
                 s_policy[m->cls].name, m->detail);
        wd_forensics_record(m->cls, WD_LEVEL_LOG, m->detail);
        return;
    }
    handle(m);
}

static void wd_action_task(void *arg)
{
    (void)arg;
    wd_action_msg_t m;
    for (;;) {
        /* 定时唤醒而不是 portMAX_DELAY：空闲时也要 kick，否则"没有故障"和
           "卡死了"在心跳上无法区分。 */
        wd_kick(s_action_slot);
        DOING("idle", NULL);
        if (xQueueReceive(s_queue, &m, pdMS_TO_TICKS(5000)) == pdTRUE) {
            dispatch(&m);
        }
    }
}

// ============================================================
// 安全模式
// ============================================================

bool wd_safe_mode(void) { return s_safe_mode; }

const char *wd_safe_mode_reason(void)
{
    return s_safe_mode ? s_safe_reason : "";
}

esp_err_t wd_safe_mode_exit(void)
{
    if (!s_safe_mode) return ESP_ERR_INVALID_STATE;
    ESP_LOGW(TAG, "用户请求退出安全模式，清记录并重启");
    wd_forensics_clear_safe_mode();
    s_safe_mode = false;
    (void)config_mgr_flush(500);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;   /* 不会执行到 */
}

void wd_recovery_get(wd_recovery_view_t *out)
{
    if (!out) return;
    for (int i = 0; i < WD_FAULT_CLASS_MAX; i++) out->level[i] = s_state[i].level;
    /* 覆盖项（thread）的级别单独存，视图按类取高者：/diag/watchdog 的 levels.subsys 照样看得见它 */
    for (size_t i = 0; i < WD_OVERRIDE_N; i++) {
        if (s_ov_state[i].level > out->level[WD_FAULT_SUBSYS_DEAD]) {
            out->level[WD_FAULT_SUBSYS_DEAD] = s_ov_state[i].level;
        }
    }
    out->last_action     = s_last_action;
    out->last_action_ts  = s_last_action_ts;
    out->self_heal_count = wd_forensics_self_heal_count();
    out->safe_mode       = s_safe_mode;
}

// ============================================================
// 初始化
// ============================================================

void wd_recovery_init(void)
{
    s_safe_mode = wd_forensics_safe_mode();
    if (s_safe_mode) {
        snprintf(s_safe_reason, sizeof(s_safe_reason),
                 "自愈 %u 次(上限 %d): %s",
                 wd_forensics_self_heal_count(), SELF_HEAL_MAX,
                 wd_forensics_last_detail());
        ESP_LOGE(TAG, "===== 安全模式 =====");
        ESP_LOGE(TAG, "%s", s_safe_reason);
        ESP_LOGE(TAG, "只启动网络与 WebUI；请从 WebUI 显式退出");
    }

    s_queue = xQueueCreate(8, sizeof(wd_action_msg_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "动作队列创建失败——恢复引擎不可用");
        return;
    }

    /* deadline 120s：单个恢复动作（httpd 重启、mqtt 重启）秒级即可完成，
       120s 还没转回来就是卡死了。severity 无所谓——扫描侧对这个槽走专用路径。 */
    static const wd_task_spec_t action_spec = {
        .name = "wd_action", .deadline_ms = 120000, .severity = WD_SEV_FATAL,
    };
    s_action_slot = wd_register(&action_spec);

    /* ⚠️ 内部栈：wd_forensics_record() 会写 NVS，PSRAM 栈在 cache 关闭时会挂 */
    if (xTaskCreate(wd_action_task, "wd_action", 3072, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "wd_action 任务创建失败——恢复引擎不可用");
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
}

#if CONFIG_BITENGINE_PROFILE_DIRECT
void wd_recovery_doing(char *out, size_t n)
{
    if (!out || n == 0) return;
    const char *what = s_doing, *det = s_doing_det;  /* 别的任务正在改：各读一次，最多拼出新旧两半 —— 都是常量串，不会读坏 */
    snprintf(out, n, "wd_action %s%s%s", what ? what : "?", det && det[0] ? " " : "", det ? det : "");
}
#endif
