#pragma once
/* hub_events —— 事件管线的入口（docs/15_muse_direct_design_v1.md 6.7；只在直连型里编）。
 *
 * 生产者调 hub_event_emit()：事件进最近事件环（hub_evlog，PSRAM，64 条，**所有事件都进**），要推送的另外拷一份进推送队列
 * （PSRAM，8 条），由消费者（muse_link 的会话任务）取走、合并、限额、发出去。消费者登记一个"叫醒"回调，有新的推送就调它。
 * 初始化与 hub_events_poll 的轮询归 hub_svc（常驻任务，不随 Muse，见 hub_svc.h）。
 *
 * 与 6.7 草图的出入：没有新开 esp_event 的 HUB_EVENT base —— 默认事件循环的队列在内部 RAM、载荷上限 64 B（6.7 自己定的），
 * 装不下渲染好的句子；改成这里的两个 PSRAM 结构，生产者照样不阻塞（S2）。设备层（M4 zhub / M5 matter_ctrl）来了以后
 * 它们直接调 hub_event_emit。
 *
 * 线程：hub_event_emit / hub_events_write_recent 任何任务都能调（锁只护 memcpy，等 ≤ 100 ms，拿不到就丢这条、计数）；
 * **不能**在 ISR、esp_timer 回调里调（红线 10）。栈也要掂量：emit 连同它自己那行日志约 1.5 KB 栈，系统事件任务只有 2304 B，所以看门狗事件
 * 的回调里只记下原因，事件在 hub_events_poll（hub_svc 任务）里才生成（第五步审计）。hub_events_poll 只给 hub_svc 一个任务调，
 * hub_events_take 只给推送的消费者（muse_link 的会话任务）一个任务调。 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "hub_evlog.h"
#include "hub_json.h"
#include "hub_model.h"

typedef struct {
    uint32_t emitted;          /* 进了环的 */
    uint32_t ring_busy;        /* 锁拿不到、没进环的 */
    uint32_t push_queued;      /* 拷进推送队列的 */
    uint32_t push_overflow;    /* 推送队列满、没拷进去的（消费者没起来 / 跟不上） */
} hub_events_stats_t;

/* 分配环与推送队列（PSRAM）、订阅看门狗的异常重启事件。幂等。 */
esp_err_t hub_events_init(void);

#define HEV_F_PUSH      0x01u   /* 这类事件要推送；推不推最后在 emit 里按类别开关判（hub_events_set_notify，ALARM 恒推）。SECURITY 类还要设备的 notify 位开着（emit 查 hub_meta；生产者照常带这一位） */

/* 记一条事件。dev_id 为 NULL / "" = 网关自己；kind 只许 [a-z0-9_]；text 已渲染、已净化（≤ HEV_TEXT_MAX - 1）。
   返回 ESP_OK = 进了环；推送队列满不算失败（计 push_overflow）。 */
esp_err_t hub_event_emit(hub_evt_class_t cls, const char *dev_id, const char *kind, const char *text, unsigned flags);

/* 推送类别开关（HUB_NOTIFY_*，hub_model.h）：推不推在 emit 这一处判（第五步审计：别让每个事件源各算各的）。
   ALARM 恒推；关掉的类别照样进事件环（hub.events.recent 看得到，14.5）。默认 HUB_NOTIFY_DEFAULT。 */
void     hub_events_set_notify(uint16_t mask);
uint16_t hub_events_get_notify(void);

/* 消费者：取一条要推送的（不阻塞）。没有返回 false。 */
bool hub_events_take(hev_t *out);
/* 消费者登记"有新推送"的回调（只投事件、不阻塞；NULL 取消）。 */
void hub_events_set_wake(void (*fn)(void));

/* hub.events.recent 的结果体（见 hev_write_recent）。管线没起来或 100 ms 拿不到锁返回 false、什么都不写 ——
   调用方回错误，别回"空列表"（Muse 会据此告诉用户什么都没发生）。 */
bool hub_events_write_recent(hj_t *w, uint32_t limit, uint32_t since_s);

/* hub_svc 每 10 s 调一次（只此一处调，内部状态不加锁）：
   - 生成看门狗回调记下的异常重启事件；
   - Thread 网络起落：要拿 OT 锁 ≤ 500 ms 读设备角色。起来过（leader / router / child）之后**连续**两次看到掉网（detached / disabled，
     或开着却没在跑）记一条 GATEWAY thread_down，回来记 thread_up；OT 锁忙的那次不算数、连续计数清零；用户关了 Thread 不算掉网。
     ⚠️ 诊断用的 zb_sniff 暂停射频超过 20 s 会被当成掉网报一次（thread_br 没有可查的"暂停中"状态；BLE 策略 B 上线时要排除）。
   - Zigbee 入网窗口开 / 关 / 没开成（直连型，比 zhub 快照里的计数；只进事件环、不推送，M4 第六步）。设备的增 / 离 / 访谈 / 读数事件由 hub_zigbee.c 出。 */
void hub_events_poll(void);

void hub_events_get_stats(hub_events_stats_t *out);
/* 本地时间 "HH:MM"（已对时，年份 ≥ 2024）；没对时写空串。事件环与审计环共用 */
void hub_events_local_clock(char out[6]);
