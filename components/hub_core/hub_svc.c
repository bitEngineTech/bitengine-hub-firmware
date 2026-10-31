/* hub_svc —— 直连型设备层的常驻服务（见 include/hub_svc.h）。只在直连型里编。 */
#include "hub_svc.h"
#include "hub_zigbee.h"
#include "hub_matter.h"
#include "hub_jobs.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hub_audit.h"
#include "hub_events.h"
#include "hub_meta.h"
#include "thread_inventory.h"
#include "watchdog.h"

static const char *TAG = "hub_svc";

#define HUB_SVC_STACK   6144      /* PSRAM；按 objdump 估最深约 2.3 KB（清单刷新 → ti_diff → emit → 日志），上板看 stack_min_free 再定 */
#define HUB_SVC_PRIO    3         /* 低于 muse_sess（4）：事件与清单晚几百毫秒无妨 */
#define TICK_MS         10000     /* 同原来 muse_link 的 resync 周期 */
#define INVENTORY_MS    60000     /* 清单刷新周期（7.4） */
#define INVENTORY_ROUNDS (INVENTORY_MS / TICK_MS)   /* 按轮数计：固定周期下每 6 轮一次，不受相对延时累积的影响 */
#define WD_DEADLINE_MS  30000     /* 一轮最坏：poll 的 OT 锁 ≤ 500 ms + emit 锁 + 清单锁 ≤ 500 ms + OT 锁 ≤ 500 ms + 关联，约 2 s；30 s 没喂就是卡住了 */

static volatile bool     s_started;
static volatile bool     s_running;
static volatile uint32_t s_rounds;
static volatile uint32_t s_stack_min_free;

static void svc_task(void *arg)
{
    (void)arg;
    const wd_task_spec_t spec = {
        .name = "hub_svc",
        .deadline_ms = WD_DEADLINE_MS,
        .severity = WD_SEV_OBSERVE,      /* 停了只影响事件与清单：记下来，不升级到整机重启（N7） */
        .recover = NULL,
    };
    const int wd = wd_register(&spec);   /* 槽满返回 -1：wd_kick(-1) 什么都不做，任务照跑 */
    if (wd < 0) ESP_LOGW(TAG, "看门狗槽位满了：hub_svc 不受看门狗观察");
    s_running = true;
    TickType_t wake = xTaskGetTickCount();
    for (uint32_t round = 0;; round++) {
        hub_events_poll();               /* 看门狗记下的异常重启、Thread 网络起落 → GATEWAY 事件（6.7） */
        hub_jobs_tick();                 /* 过了期限还没结束的作业改 TIMEOUT 并通知（6.6） */
        if (round % INVENTORY_ROUNDS == 0) {   /* 第一轮就刷，之后每 INVENTORY_MS 一次 */
            (void)thread_inventory_refresh();   /* 第七节：成员进出 → 事件（OT 锁忙这轮跳过；Thread 关着 / 掉网它自己处理） */
        } else {
            thread_inventory_watch();    /* 两轮之间看一眼网络 / SRP 停过没有（停过就下次重建基线） */
        }
        s_rounds = round + 1;
        s_stack_min_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
        wd_kick(wd);
        xTaskDelayUntil(&wake, pdMS_TO_TICKS(TICK_MS));   /* 固定周期：一轮的耗时不累积进间隔 */
    }
}

esp_err_t hub_svc_start(void)
{
    if (s_started) return ESP_OK;
    /* 三样各自起不来只影响自己：事件环 / 推送、审计、hub.devices.list（处理函数在没 init 时回 unavailable） */
    if (hub_events_init() != ESP_OK) ESP_LOGE(TAG, "事件管线没起来：这次开机没有事件环、不推送");
    if (hub_audit_init() != ESP_OK) ESP_LOGE(TAG, "审计环没起来：这次开机不记审计");
    if (thread_inventory_init() != ESP_OK) ESP_LOGE(TAG, "Thread 清单没起来：hub.devices.list 这次开机回 unavailable");
    /* 设备名字 / 房间（6.1）：在 zhub_start 之前读回，事件句子里第一条就用得上用户起的名字。起不来：名字一律空、hub.device.update 回 unavailable */
    if (hub_meta_init() != ESP_OK) ESP_LOGE(TAG, "设备元数据没起来：这次开机没有名字 / 房间");
    /* 作业表（6.6；M7 第三步：hub.matter.commission、本地移除）：起不来时耗时命令一律回 busy */
    if (hub_jobs_init() != ESP_OK) ESP_LOGE(TAG, "作业表没起来：这次开机耗时命令（Matter 配网 / 移除）回 busy");
#if CONFIG_BITENGINE_ENABLE_ZHUB
    hub_zigbee_init();                             /* 给 zhub 登记设备存取 / 事件钩子：main.c 里 hub_svc 在 zhub_start 之前 */
#endif
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
    hub_matter_init();                             /* 给 matter_ctrl 登记 fabric 身份的存取钩子：main.c 里 hub_svc 在 matter_ctrl_start 之前 */
#endif
    /* 栈在 PSRAM：这个任务不写 flash（只读内存、拿 OT 锁、往 PSRAM 环里拷），cache 关着时不会跑到它 */
    if (xTaskCreateWithCaps(svc_task, "hub_svc", HUB_SVC_STACK, NULL, HUB_SVC_PRIO, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "任务建不起来（PSRAM 栈）：事件与清单这次开机不轮询");
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

void hub_svc_get_status(hub_svc_status_t *out)
{
    if (!out) return;
    out->running = s_running;
    out->rounds = s_rounds;
    out->stack_min_free = s_stack_min_free;
}
