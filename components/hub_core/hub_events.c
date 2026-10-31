/* hub_events —— 事件管线的入口（见 include/hub_events.h）。只在直连型里编。
 *
 * H1（2026-10-22 对照 IDF v6.0.2）：
 *   - xQueueCreateWithCaps(len, item, MALLOC_CAP_SPIRAM)：队列存储与控制块都按 caps 分配（freertos/idf_additions.h）；
 *     只在任务里收发（不在 ISR 里），PSRAM 里的队列存储没问题；删除要配 vQueueDeleteWithCaps（本模块常驻，不删）。
 *   - WATCHDOG_EVENT_ABNORMAL_BOOT 的载荷是 wd_abnormal_boot_t，health_monitor 在开机 ≥ 60 s 后补发（watchdog.h），
 *     cause 只含 [a-z0-9_:/.-]；本模块在 hub_svc_start（main.c，hub_store 之后）里订阅，早于补发。
 *   - otThreadGetDeviceRole() 在 esp_openthread_lock_acquire() 里调（同 hub_cmds.c）。 */
#include "hub_events.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "config_mgr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_timer.h"
#include "hub_meta.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "openthread/thread.h"
#include "thread_br.h"
#include "watchdog.h"
#if CONFIG_BITENGINE_ENABLE_ZHUB
#include "zhub.h"          /* 入网窗口与入网观察（M4 第六步）：读快照，不碰 zhub 的状态 */
#endif

static const char *TAG = "hub_events";

#define PUSH_Q_LEN     8
#define LOCK_TICKS     pdMS_TO_TICKS(100)
#define OT_LOCK_TICKS  pdMS_TO_TICKS(500)

static hev_ring_t        *s_ring;          /* PSRAM */
static SemaphoreHandle_t  s_lock;
static QueueHandle_t      s_pushq;         /* hev_t，PSRAM */
static void             (*s_wake)(void);
static volatile uint16_t  s_notify = HUB_NOTIFY_DEFAULT;   /* 单写（Muse 会话任务读进推送类别）多读，16 位读写是原子的 */
static hub_events_stats_t s_stats;         /* 计数在 s_lock 里改 */
static bool               s_inited;

/* 看门狗回调（系统事件任务）只记到这里，hub_events_poll 生成事件：一次开机最多一份，单写单读 */
static char               s_ab_cause[WD_ABNORMAL_CAUSE_LEN];
static volatile bool      s_ab_pending;

/* Thread 监视的状态：只在 hub_svc 任务里读写（hub_events_poll 的唯一调用者） */
static bool    s_th_seen_up;               /* 这次开机起来过 */
static bool    s_th_down_reported;
static uint8_t s_th_bad;                   /* 连续几次看到掉网 */

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

/* 对过时（年份 ≥ 2024）给本地 "HH:MM"，否则空串（6.7：没对时不写错误的钟点）；审计环也用（hub_audit.c） */
void hub_events_local_clock(char out[6])
{
    out[0] = '\0';
    const time_t t = time(NULL);
    struct tm tm;
    if (!localtime_r(&t, &tm) || tm.tm_year + 1900 < 2024) return;
    const unsigned h = (unsigned)tm.tm_hour % 24u, m = (unsigned)tm.tm_min % 60u;   /* 逐位写：不经 snprintf，长度确定 */
    out[0] = (char)('0' + h / 10u);
    out[1] = (char)('0' + h % 10u);
    out[2] = ':';
    out[3] = (char)('0' + m / 10u);
    out[4] = (char)('0' + m % 10u);
    out[5] = '\0';
}

/* 设备的 notify 位（hub_meta）。zhub 任务也会调 emit（它还跑着 ASH）：查表只等 DEV_META_LOCK_MS，拿不到按关（hub_meta 计 lookup_busy） */
#define DEV_META_LOCK_MS 20u
static bool dev_notify(const char *dev_id)
{
    uint8_t proto, id8[8];
    hub_meta_ent_t m;
    if (!dev_id || !hub_meta_parse_id(dev_id, &proto, id8)) return false;   /* 网关自己 / Thread 成员：没有设备位 */
    return hub_meta_lookup(proto, id8, &m, DEV_META_LOCK_MS) && (m.flags & HUB_META_F_NOTIFY);
}

esp_err_t hub_event_emit(hub_evt_class_t cls, const char *dev_id, const char *kind, const char *text, unsigned flags)
{
    if ((unsigned)cls >= HUB_EVC_COUNT || !kind || !text || !text[0]) return ESP_ERR_INVALID_ARG;
    if (strlen(kind) >= HEV_KIND_MAX || strlen(text) >= HEV_TEXT_MAX || (dev_id && strlen(dev_id) >= HEV_DEV_MAX)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    hev_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.cls = cls;
    ev.mono_ms = now_ms();
    hub_events_local_clock(ev.clock);
    snprintf(ev.dev_id, sizeof(ev.dev_id), "%s", dev_id ? dev_id : "");
    snprintf(ev.kind, sizeof(ev.kind), "%s", kind);
    snprintf(ev.text, sizeof(ev.text), "%s", text);
    /* 推不推在这一处判（设计第十七节"M4 之前"：别让每个事件源各算各的）：调用方说要推（HEV_F_PUSH）、并且这一类开着（ALARM 恒推）；
       SECURITY（门磁 / 人体 / 振动）还要这台设备的 notify 位开着（6.7：类别位 AND 设备位；设备位由 hub.device.update 设，默认关） */
    bool push = (flags & HEV_F_PUSH) && (cls == HUB_EVC_ALARM || (s_notify & (1u << cls)));
    if (push && cls == HUB_EVC_SECURITY) push = dev_notify(dev_id);
    ev.flags = (uint8_t)(push ? flags : (flags & ~HEV_F_PUSH));
    if (xSemaphoreTake(s_lock, LOCK_TICKS) != pdTRUE) {
        s_stats.ring_busy++;                       /* 没拿到锁时的计数不精确也无妨（只给诊断看） */
        return ESP_ERR_TIMEOUT;
    }
    ev.seq = hev_ring_add(s_ring, &ev);
    if (ev.seq) s_stats.emitted++;
    xSemaphoreGive(s_lock);
    if (!ev.seq) return ESP_ERR_INVALID_ARG;       /* kind 不合格之类：hev_ring_add 拒了 */
    if (push) {
        const bool ok = xQueueSend(s_pushq, &ev, 0) == pdTRUE;
        if (xSemaphoreTake(s_lock, LOCK_TICKS) == pdTRUE) {
            if (ok) s_stats.push_queued++;
            else s_stats.push_overflow++;
            xSemaphoreGive(s_lock);
        }
        void (*wake)(void) = s_wake;
        if (ok && wake) wake();
    }
    ESP_LOGI(TAG, "%s/%s%s%s", hev_class_name(cls), kind, dev_id && dev_id[0] ? " " : "", dev_id ? dev_id : "");
    return ESP_OK;
}

void hub_events_set_notify(uint16_t mask) { s_notify = (uint16_t)(mask & HUB_NOTIFY_ALL); }
uint16_t hub_events_get_notify(void) { return s_notify; }

bool hub_events_take(hev_t *out)
{
    return s_inited && out && xQueueReceive(s_pushq, out, 0) == pdTRUE;
}

void hub_events_set_wake(void (*fn)(void)) { s_wake = fn; }

bool hub_events_write_recent(hj_t *w, uint32_t limit, uint32_t since_s)
{
    if (!w || !s_inited || xSemaphoreTake(s_lock, LOCK_TICKS) != pdTRUE) return false;
    hev_write_recent(w, s_ring, now_ms(), limit, since_s);   /* 只在锁里拷进 JSON 缓冲，毫秒级 */
    xSemaphoreGive(s_lock);
    return true;
}

void hub_events_get_stats(hub_events_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (s_inited && xSemaphoreTake(s_lock, LOCK_TICKS) == pdTRUE) {
        *out = s_stats;
        xSemaphoreGive(s_lock);
    }
}

/* ---------------------------------------------------------------- 生产者：看门狗 */

static void on_wd_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != WATCHDOG_EVENT_ABNORMAL_BOOT || !data || s_ab_pending) return;
    const wd_abnormal_boot_t *ab = (const wd_abnormal_boot_t *)data;
    /* wd_recovery 在整机重启前抢发的那份 reset_reason 是 0：重启后 health_monitor 会带真原因再补发一次，只记那次 */
    if (ab->reset_reason == 0) return;
    memcpy(s_ab_cause, ab->cause, sizeof(s_ab_cause));
    s_ab_cause[sizeof(s_ab_cause) - 1] = '\0';
    s_ab_pending = true;                           /* 只记下来：渲染与入环在 hub_svc 任务里（这里是 2304 B 栈的系统事件任务） */
}

/* 推送给用户的是看得懂的话，不带内部原因串（会被 Muse 原样转述；原始原因在 /api/v1/diag/watchdog 的历史里） */
static const char *restart_phrase(const char *cause)
{
    if (strncmp(cause, "panic", 5) == 0) return "after a software fault";
    if (strstr(cause, "wdt") || strncmp(cause, "watchdog", 8) == 0) return "after a component stopped responding";
    if (strstr(cause, "brownout")) return "after a power dip";
    return "after an unexpected fault";
}

static void emit_pending_restart(void)
{
    if (!s_ab_pending) return;
    char text[HEV_TEXT_MAX];
    snprintf(text, sizeof(text), "Hub: restarted on its own %s; it is running again.", restart_phrase(s_ab_cause));
    (void)hub_event_emit(HUB_EVC_GATEWAY, NULL, "hub_restarted", text, HEV_F_PUSH);
    s_ab_pending = false;                          /* health_monitor 每次开机只补发一次；单写单读（C5 单核），不用锁 */
}

/* ---------------------------------------------------------------- 生产者：Thread 监视 */

#if CONFIG_BITENGINE_ENABLE_ZHUB
/* 入网窗口开 / 关 / 没开成：比快照里的计数（每 10 s 一次，同一个间隔里来了几次合成一条）。只进事件环（hub.events.recent 看得到），不推送。
   设备的增 / 离 / 回来不在这里：第七步 a 起由设备层出（hub_zigbee.c，一台设备一条、带 zb- id）—— 原先这里按入网观察的计数也出一条，
   一台设备离开事件环里有两条（第二轮审计 ❌）。zhub 不直接调这里：两个组件不互相依赖 */
static void poll_zigbee(void)
{
    /* 基线是 0：这几个计数在 zhub.c 里每次开机从 0 起（链路复位不清）。原先"第一次轮询只记基线"会把开机后头 10 s 里开的窗吞掉 ——
       第一次轮询正好落在开窗之后、关窗之前时只报"关了"（M5 第一步上板 test_api ZH10h 撞上） */
    static uint32_t s_opens, s_closes, s_fails;
    zhub_status_t z;
    zhub_get_status(&z);
    if (!z.started) return;
    char t[HEV_TEXT_MAX];
    if (z.win_opens != s_opens) {
        snprintf(t, sizeof(t), "Zigbee pairing opened for %u seconds; new Zigbee devices can join now.", (unsigned)z.win_s);
        (void)hub_event_emit(HUB_EVC_LIFECYCLE, NULL, "zigbee_pairing_open", t, 0);
    }
    if (z.win_open_fails != s_fails) {
        (void)hub_event_emit(HUB_EVC_LIFECYCLE, NULL, "zigbee_pairing_failed", "Zigbee pairing could not be opened; try again in a minute.", 0);
    }
    if (z.win_closes != s_closes) {
        (void)hub_event_emit(HUB_EVC_LIFECYCLE, NULL, "zigbee_pairing_closed", "Zigbee pairing closed.", 0);
    }
    s_opens = z.win_opens; s_closes = z.win_closes; s_fails = z.win_open_fails;
}
#endif

void hub_events_poll(void)
{
    if (!s_inited) return;
    emit_pending_restart();
#if CONFIG_BITENGINE_ENABLE_ZHUB
    poll_zigbee();
#endif
    const thread_config_t *tc = config_mgr_get_thread();
    if (!tc || !tc->enabled) {                     /* 用户关了 Thread：不算掉网，重新开始 */
        s_th_seen_up = s_th_down_reported = false;
        s_th_bad = 0;
        return;
    }
    bool up = false, known = false;
    if (thread_br_is_running() && esp_openthread_lock_acquire(OT_LOCK_TICKS)) {
        otInstance *inst = esp_openthread_get_instance();
        if (inst) {
            const otDeviceRole r = otThreadGetDeviceRole(inst);
            up = r == OT_DEVICE_ROLE_LEADER || r == OT_DEVICE_ROLE_ROUTER || r == OT_DEVICE_ROLE_CHILD;
            known = true;
        }
        esp_openthread_lock_release();
    } else if (!thread_br_is_running()) {
        known = true;                              /* 开着却没在跑：也是掉网 */
    }
    if (!known) {                                  /* OT 锁忙：这次不判，连续计数清零（要严格连续两次才报） */
        s_th_bad = 0;
        return;
    }
    if (up) {
        s_th_bad = 0;
        if (s_th_down_reported) {
            s_th_down_reported = false;
            (void)hub_event_emit(HUB_EVC_GATEWAY, NULL, "thread_up", "Hub: the Thread network is back; Thread devices can be reached again.",
                                 HEV_F_PUSH);
        }
        s_th_seen_up = true;
        return;
    }
    if (!s_th_seen_up || s_th_down_reported) return;   /* 开机还没起来过不算；报过了不重复 */
    if (++s_th_bad >= 2) {
        s_th_down_reported = true;
        (void)hub_event_emit(HUB_EVC_GATEWAY, NULL, "thread_down",
                             "Hub: the Thread network is down; Thread devices are unreachable until it recovers.", HEV_F_PUSH);
    }
}

/* ---------------------------------------------------------------- 初始化 */

esp_err_t hub_events_init(void)
{
    if (s_inited) return ESP_OK;
    s_ring = heap_caps_calloc(1, sizeof(hev_ring_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_lock = xSemaphoreCreateMutex();
    s_pushq = xQueueCreateWithCaps(PUSH_Q_LEN, sizeof(hev_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_err_t err = ESP_OK;
    if (!s_ring || !s_lock || !s_pushq) {
        ESP_LOGE(TAG, "分配失败（环 %p、锁 %p、队列 %p）", (void *)s_ring, (void *)s_lock, (void *)s_pushq);
        err = ESP_ERR_NO_MEM;
    } else {
        hev_ring_init(s_ring);
        err = esp_event_handler_register(WATCHDOG_EVENT, WATCHDOG_EVENT_ABNORMAL_BOOT, on_wd_event, NULL);
        if (err != ESP_OK) ESP_LOGE(TAG, "订阅看门狗事件失败：%s", esp_err_to_name(err));
    }
    if (err != ESP_OK) {
        /* 已分配的放掉、指针清零：hub_svc_start 失败后可以再调（同 hub_audit_init / thread_inventory_init；第八步审计） */
        heap_caps_free(s_ring);
        if (s_lock) vSemaphoreDelete(s_lock);
        if (s_pushq) vQueueDeleteWithCaps(s_pushq);
        s_ring = NULL;
        s_lock = NULL;
        s_pushq = NULL;
        return err;
    }
    s_inited = true;
    return ESP_OK;
}
