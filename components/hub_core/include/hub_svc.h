#pragma once
/* hub_svc —— 直连型设备层的常驻服务（docs/15_muse_direct_design_v1.md 第十七节"M4 之前"；只在直连型里编）。
 *
 * 事件管线（6.7，hub_events）、审计环（6.10，hub_audit）、Thread 网络层清单（第七节，thread_inventory）原来都由 Muse 会话任务
 * 初始化、由它的 10 s resync 轮询 —— 编译关掉 Muse、或会话任务卡在 TLS 里时，事件环与清单就停了，zhub（M4）/ matter_ctrl（M7）
 * 也用不上。这里把它们挪到一个自己的任务里：main.c 在 hub_store 之后、muse_link 之前调 hub_svc_start()（安全模式不起）。
 *
 * 任务每 10 s：hub_events_poll()（看门狗记下的异常重启、Thread 网络起落 → GATEWAY 事件）；每 60 s 刷新一次清单
 * （thread_inventory_refresh，成员进出 → 事件），其余轮 thread_inventory_watch()。PSRAM 栈（不碰 flash：这几样都只读内存 / 拿 OT 锁），
 * 看门狗只观察（它停了只影响事件与清单，不该升到整机重启，N7）。
 * 推送（谁来发）仍归 muse_link：它注册 hub_events_set_wake、把推送类别读进 hub_events_set_notify。 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 初始化三样 + 设备元数据（hub_meta，6.1：只开机读回一次，不轮询）—— 各自起不来只记日志、其余照常 —— 并起任务。幂等。任务建不起来返回 ESP_ERR_NO_MEM（三样已初始化的照样能被按需调用）。 */
esp_err_t hub_svc_start(void);

typedef struct {
    bool     running;          /* 任务起来了 */
    uint32_t rounds;           /* 跑过几轮（10 s 一轮） */
    uint32_t stack_min_free;   /* 栈最深时还剩多少字节（上板确认 HUB_SVC_STACK 够不够） */
} hub_svc_status_t;

void hub_svc_get_status(hub_svc_status_t *out);
