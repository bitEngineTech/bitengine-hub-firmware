#pragma once
/*
 * 把常驻任务的栈放到 PSRAM，省下稀缺的内部 SRAM。
 *
 * 背景：C5 内部堆只有 175KB，本固件启动完就见底，DMA 可用内存一度只剩几百字节，
 * WiFi/LWIP 缓冲区分配持续失败（2026-08-10 实测 ≈4 次/秒）。任务栈默认全部来自
 * 内部 DMA 内存，是最大的一块可迁移开销。诊断入口见 /api/v1/diag/heap。
 *
 * ⚠️ 使用前提（IDF v6.0.2 核对，改动前先重读，用错是随机崩溃而不是编译错误）：
 *
 * 1. 该任务**绝不能自己调用 flash / NVS 操作**。
 *    Kconfig FREERTOS_TASK_CREATE_ALLOW_EXT_MEM 原文："should only be used for
 *    tasks where the stack is never accessed while the cache is disabled"。
 *    擦写 flash 期间 cache 关闭，而 PSRAM 就在 cache 后面——此时访问自己的栈
 *    直接挂掉。单核上 flash 操作会挂起调度器，所以只有**执行 flash 操作的那个
 *    任务自己**有风险，其他任务不会被调度到。
 *    因此排除：button（长按恢复出厂）、ot_main（OpenThread 数据集落 NVS）、
 *    mg24_ota / ota_* 全家。
 *    ⚠️ scheduler 曾因「自动备份写 NVS」被排除，该理由 2026-08-17 复核已不成立
 *    （check_auto_backup 只发 webhook 通知，全组件 grep nvs_ 零命中），已迁 PSRAM。
 *    若将来 scheduler.c 那行 "OTA check would go here" 接上云端 OTA 并写 NVS
 *    记录检查时间，必须改回 xTaskCreate —— components/scheduler/VERIFY.sh 有断言盯着。
 *
 * 2. 该任务**必须常驻**，不能 vTaskDelete(NULL) 自删。
 *    xTaskCreateWithCaps 的栈要用 vTaskDeleteWithCaps 释放，自删会漏掉这块栈。
 *    因此排除：client_watchdog、ot_br_init、uart_bench、tcp_accept/uart_to_tcp/
 *    tcp_to_uart（模式切换时会停）、ws_push（停止路径自删）。
 *
 * 已迁清单（2026-08-17 起）：mqtt_pub / health / stats_log / webhook / net_status /
 * led_task / zb_check / scheduler / wifi_keepalive（2026-09-15 加入，3072 —— 它在
 * net_init 区间内，而以太网主用时该任务常态空转，理由见 wifi_mgr.c 的注释）/
 * mdns_refresh（2026-10-05 加入，4096 —— 开机 / Thread 重启后 5 分钟的 mDNS 刷新
 * 从 ot_br_init 拆出，只调 mdns_svc_*，理由见 thread_br.c 的注释）/
 * esp-mqtt 的 mqtt_task（2026-10-08 加入，6144 —— 第三方组件里建的任务，不走本函数，
 * 由 tools/patch_mqtt.py 构建期改成 WithCaps；它会自删，补丁让它用 vTaskDeleteWithCaps，T9.39）。
 *
 * 3. TCB 不受影响：IDF 用 pvPortMalloc 强制把 TCB 放内部内存
 *    （freertos/esp_additions/idf_additions.c:44-46），无需我们处理。
 *
 * 4. 延迟敏感的高优先级任务不要迁：PSRAM 访问有 cache miss 抖动。
 *    因此排除 wdt_feed（喂 RS706）和 EZSP 数据通路那几个 20+ 优先级任务。
 *
 * 分配失败时自动退回内部内存，不会因为 PSRAM 满了就少一个任务。
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

/* stack_bytes 单位是**字节**：IDF 的 RISC-V port 里 StackType_t 是 uint8_t
   (portmacro.h:95)，所以 xTaskCreate / xTaskCreateWithCaps 的栈深度都按字节算。 */
static inline BaseType_t psram_task_create(TaskFunction_t fn, const char *name,
                                           uint32_t stack_bytes, void *arg,
                                           UBaseType_t prio, TaskHandle_t *out)
{
#if defined(CONFIG_SPIRAM) && defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM)
    BaseType_t r = xTaskCreateWithCaps(fn, name, stack_bytes, arg, prio, out,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (r == pdPASS) {
        return r;
    }
    ESP_LOGW("psram_task", "%s: no PSRAM for stack, falling back to internal", name);
#endif
    return xTaskCreate(fn, name, stack_bytes, arg, prio, out);
}
