/* 层 B — 任务心跳注册表（docs/04_watchdog_design_v2.md 第五节）
 *
 * 对付 F2「任务停转」：系统整体在跑，但某个任务不再推进。芯片级 Task WDT
 * 看不见这类故障（idle 照跑），而它今天真实发生过——WebUI 的 httpd 被滞留
 * 连接堵了两分钟，五层看门狗一层都没报。
 *
 * 设计约束（三条都不能破）：
 *   1. 零动态分配。看门狗自身绝不能因为内存不足而失效——而"内存不足"正是
 *      它要监视的故障之一。
 *   2. wd_kick() 是 O(1) 无锁的单次 32 位存储。RISC-V 上对齐的 32 位读写
 *      天然原子，不需要临界区，因此可以在任何上下文（含 ISR）里调用。
 *   3. 扫描跑在 pri-22（喂狗任务）里，保证被饿死的是别人不是它。
 */

#include "watchdog.h"

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "wd_registry";

#define WD_DEADLINE_MIN_MS  3000

typedef struct {
    const char   *name;
    uint32_t      deadline_ms;
    wd_severity_t severity;
    esp_err_t   (*recover)(void);

    volatile uint32_t last_kick_ms;      /* wd_kick 唯一写入的字段 */
    volatile uint32_t suspend_until_ms;
    uint16_t      miss_count;            /* 只由扫描任务读写 */
    bool          reported;              /* 已上报，避免重复刷屏 */
    /* T9.36（2026-10-08）：已上报停转的槽被**复用**（L3 重启后新实例重新注册）或标成无主时，"reported → 又被 kick"这一跳变
       就没了，"任务恢复"永远发不出去，恢复引擎里这一类的级别停在 L3 —— 冷却之后同类任务再停转直接 L4 整机重启。
       所以清 reported 的同时记"待确认恢复"，并记下当时的 last_kick_ms：注册时那个是代填的，**新实例真的 kick 过**
       （last_kick_ms 变了）才发"任务恢复"—— 恢复由真实心跳证明，不靠 L3 回调的返回值。 */
    bool          recover_pending;
    uint32_t      recover_mark_ms;

    /* 栈余量：由任务**自己**在 wd_kick 里采样后缓存，这里存的是值不是句柄。
     *
     * ⚠️ 这里原本存的是 TaskHandle_t（首次 kick 时捕获），层 D 事后拿去调
     * uxTaskGetStackHighWaterMark()。那是 2026-08-11 整板重启的第二个真因：
     * 句柄只在**槽位被重新注册**时才清空，而 EZSP 桥切到 DISABLED 会让两个
     * 任务自删（进 bootloader 时桥更是一直不起来），中间这段窗口里句柄指着
     * 已释放的 TCB，health_task 每个 tick 都会去解引用它 —— 读到已释放 TCB 里
     * 的 pxStack 是 0，于是在地址 0 上 Load access fault。
     *
     * 修法不是"记得在自删前清句柄"——那是同一套纪律的第三次应用，而它在本模块
     * 里已经失效两次（esp_task_wdt 订阅没退、这里的句柄没清）。改成只存值之后
     * **没有指针可悬空**，这个缺陷类在这里变成不可表达的。
     */
    volatile uint32_t stack_left;        /* 字节；0 = 还没采过（含"从没被 kick 过"） */
    volatile uint32_t stack_sample_ms;   /* 上次采样时刻，用于节流 */

    /* 拥有它的任务已按计划结束，扫描应跳过。见 wd_slot_offline()。 */
    volatile bool offline;
} wd_slot_t;

/* 采样节流：uxTaskGetStackHighWaterMark 要扫栈找 0xa5 填充，最坏 O(栈大小)，
   而 wd_kick 在桥的收发循环里每秒能调几十次。高水位线是**单调不增**的，
   慢采样只会让发现晚不到一秒，而层 D 本来查的就是缓慢的栈耗尽。 */
#define WD_STACK_SAMPLE_INTERVAL_MS  1000

static wd_slot_t s_slots[WD_MAX_TASKS];
static int       s_slot_count;           /* 只增不减；注册在启动阶段完成 */

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ---- 注册与心跳 ----

/* 清 reported 之前调（复用槽位 / 标成无主）：它若正处于"已上报停转"，改记待确认恢复（见 recover_pending 的注释）。
   调用时 last_kick_ms 必须已经是"这一刻的值"（注册路径先刷新它再调）。 */
static void mark_recover_pending(wd_slot_t *s)
{
    if (!s->reported) return;
    s->recover_pending = true;
    s->recover_mark_ms = s->last_kick_ms;
}

int wd_register(const wd_task_spec_t *spec)
{
    if (!spec || !spec->name) return -1;
    if (s_slot_count >= WD_MAX_TASKS) {
        ESP_LOGE(TAG, "注册表已满(%d)，%s 未被监视", WD_MAX_TASKS, spec->name);
        return -1;
    }

    /* 幂等：同名槽直接复用。子系统重启（L3）会再次走 init 路径重新注册，
       每次新建一个槽的话，旧槽从此没人 kick，必然被判停转，于是触发下一轮
       恢复——自己把自己送进重启循环。TC-W07 实测到过这条路径。 */
    for (int i = 0; i < s_slot_count; i++) {
        if (s_slots[i].name && !strcmp(s_slots[i].name, spec->name)) {
            s_slots[i].last_kick_ms = now_ms();
            s_slots[i].miss_count   = 0;
            mark_recover_pending(&s_slots[i]);
            s_slots[i].reported     = false;
            s_slots[i].offline      = false;   /* 新任务接手，恢复监视 */
            /* 新任务的栈余量与旧任务无关，清掉重新采 */
            s_slots[i].stack_left      = 0;
            s_slots[i].stack_sample_ms = 0;
            ESP_LOGI(TAG, "复用心跳槽 %s (slot %d)", spec->name, i);
            return i;
        }
    }

    int id = s_slot_count;
    wd_slot_t *s = &s_slots[id];
    s->name        = spec->name;
    /* deadline 下限：比扫描周期(500ms)大得多，避免调度抖动造成误判 */
    s->deadline_ms = spec->deadline_ms < WD_DEADLINE_MIN_MS
                     ? WD_DEADLINE_MIN_MS : spec->deadline_ms;
    s->severity    = spec->severity;
    s->recover     = spec->recover;
    s->last_kick_ms = now_ms();   /* 注册即视为刚活过一次，避免立刻超时 */
    s->suspend_until_ms = 0;
    s->miss_count  = 0;
    s->reported    = false;
    s->recover_pending = false;
    s->recover_mark_ms = 0;
    s->stack_left      = 0;
    s->stack_sample_ms = 0;
    s->offline         = false;

    /* 最后一步才让扫描看见这个槽：wd_feed 的扫描任务在注册期间就已经在跑了，
       屏障保证它看到 s_slot_count 增加时，槽内字段已经写完。 */
    __atomic_store_n(&s_slot_count, id + 1, __ATOMIC_RELEASE);

    ESP_LOGI(TAG, "监视 %-12s deadline=%"PRIu32"ms sev=%d",
             s->name, s->deadline_ms, (int)s->severity);
    return id;
}

void wd_kick(int slot)
{
    if (slot < 0 || slot >= s_slot_count) return;
    uint32_t now = now_ms();
    s_slots[slot].last_kick_ms = now;
    /* 任务在 kick，就是活着 —— 无论之前是否被标记过无主 */
    s_slots[slot].offline = false;

    /* 栈余量在**任务自己的上下文**里采（传 NULL = 查当前任务），采完只留一个整数。
       返回的是"历史最小剩余"，IDF 上单位是字节（StackType_t = uint8_t，
       portmacro.h:95）。ISR 里跳过：那里量到的是被打断的任务，不是本槽的任务。 */
    if (xPortInIsrContext()) return;
    if (s_slots[slot].stack_left == 0 ||
        (now - s_slots[slot].stack_sample_ms) >= WD_STACK_SAMPLE_INTERVAL_MS) {
        s_slots[slot].stack_left      = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
        s_slots[slot].stack_sample_ms = now;
    }
}

void wd_suspend(int slot, uint32_t grace_ms)
{
    if (slot < 0 || slot >= s_slot_count) return;
    s_slots[slot].suspend_until_ms = now_ms() + grace_ms;
}

/* 槽位当前无主：拥有它的任务已经**按计划**结束了，不该再被当成停转。
 *
 * 与 wd_suspend() 的区别是"有没有上界"。suspend 适合已知时长的长操作
 * （OTA、UART 基准测试），到点自动恢复监视；而这里针对的是时长由外部决定、
 * 无上界的情形：EZSP 桥停在 MG24 bootloader 时，什么时候退出由用户决定，
 * 用固定宽限期就得反复续期，忘了续就是一次误判重启。
 *
 * 2026-08-11 实测：停在 bootloader 超过 30s（两个桥槽的 deadline），
 * 停转检测把"桥被故意停掉"当成"任务卡死"，触发 L4 整机重启。
 *
 * 不需要配对的"上线"调用：任何一次 wd_kick() 都证明任务活着，会自动清除；
 * wd_register() 复用槽位时也会清。所以漏调只会让监视早一点恢复，不会漏判。
 */
void wd_slot_offline(int slot)
{
    if (slot < 0 || slot >= s_slot_count) return;
    s_slots[slot].offline    = true;
    s_slots[slot].miss_count = 0;
    mark_recover_pending(&s_slots[slot]);
    s_slots[slot].reported   = false;
}

// ---- 扫描（由 wd_feed.c 的 pri-22 任务每 500ms 调用）----

bool wd_registry_scan(void)
{
    uint32_t t = now_ms();
    bool all_hard_ok = true;

    int count = __atomic_load_n(&s_slot_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < count; i++) {
        wd_slot_t *s = &s_slots[i];

        if (s->offline) {
            continue;    /* 拥有它的任务已按计划结束，等它回来（下次 kick 自动清） */
        }

        if (s->suspend_until_ms && (int32_t)(s->suspend_until_ms - t) > 0) {
            continue;    /* 已知长操作豁免中（OTA / UART 基准测试） */
        }

        uint32_t age = t - s->last_kick_ms;
        if (age > s->deadline_ms) {
            s->miss_count++;
            if (s->severity != WD_SEV_OBSERVE) all_hard_ok = false;

            /* 只在状态跳变时上报：扫描是 500ms 一次，持续停转会刷 120 条/分钟 */
            if (!s->reported) {
                s->reported = true;
                ESP_LOGE(TAG, "任务停转: %s 已 %"PRIu32"ms 未 kick (deadline %"PRIu32"ms)",
                         s->name, age, s->deadline_ms);
                /* 恢复引擎自己停转时不能走 wd_report——那条路要经过它的队列。 */
                if (i == wd_recovery_action_slot()) {
                    wd_recovery_emergency(s->name);   /* 不返回 */
                } else {
                    wd_report(WD_FAULT_TASK_STALL, s->severity, s->name, s->recover);
                }
            }
        } else if (s->reported) {
            s->reported   = false;
            s->recover_pending = false;   /* 这一次跳变就是恢复，别再补发 */
            s->miss_count = 0;
            ESP_LOGW(TAG, "任务恢复: %s", s->name);
            wd_report_recovered(WD_FAULT_TASK_STALL, s->name);
        } else if (s->recover_pending && s->last_kick_ms != s->recover_mark_ms) {
            /* 复用 / 无主之后，新实例真的 kick 过了（T9.36） */
            s->recover_pending = false;
            s->miss_count = 0;
            ESP_LOGW(TAG, "任务恢复: %s（重启后的新实例已在心跳）", s->name);
            wd_report_recovered(WD_FAULT_TASK_STALL, s->name);
        }
    }
    return all_hard_ok;
}

size_t wd_registry_snapshot(wd_task_view_t *out, size_t max)
{
    if (!out || max == 0) return 0;
    uint32_t t = now_ms();
    size_t n = (size_t)s_slot_count < max ? (size_t)s_slot_count : max;
    for (size_t i = 0; i < n; i++) {
        const wd_slot_t *s = &s_slots[i];
        out[i].name        = s->name;
        out[i].deadline_ms = s->deadline_ms;
        out[i].age_ms      = t - s->last_kick_ms;
        out[i].miss_count  = s->miss_count;
        out[i].severity    = s->severity;
        out[i].suspended   = s->suspend_until_ms &&
                             (int32_t)(s->suspend_until_ms - t) > 0;
        out[i].stack_left  = s->stack_left;
    }
    return n;
}

/* 层 D 用：在各任务自采的栈余量里取最小者。
   ⚠️ 这里**只读缓存值，不碰任何任务句柄** —— 本函数由 health_task 每 tick 调用，
   一旦持有已删除任务的句柄就会解引用已释放的 TCB（见 wd_slot_t 的说明）。 */
void wd_registry_stack_low(const char **name, uint32_t *bytes)
{
    const char *worst_name = NULL;
    uint32_t    worst = UINT32_MAX;

    for (int i = 0; i < s_slot_count; i++) {
        uint32_t left = s_slots[i].stack_left;   /* 0 = 该槽还没被 kick 过 */
        if (left == 0) continue;
        if (left < worst) {
            worst = left;
            worst_name = s_slots[i].name;
        }
    }

    if (name)  *name  = worst_name;
    if (bytes) *bytes = worst_name ? worst : 0;
}
