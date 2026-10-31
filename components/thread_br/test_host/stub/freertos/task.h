#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
extern int g_fake_task_create_ok;   /* 置 0 模拟建任务失败 */
extern int g_delay_total_ms;        /* 累计 vTaskDelay，用于断言等待时长 */

/* 不真的起线程：测试只覆盖纯逻辑，任务体不执行。 */
static inline int xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                              void *arg, uint32_t prio, TaskHandle_t *out)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio;
    if (!g_fake_task_create_ok) return pdFAIL;
    if (out) *out = (TaskHandle_t)0x1234;
    return pdPASS;
}
static inline void vTaskDelay(TickType_t t) { g_delay_total_ms += (int)t; }
static inline void vTaskDelete(TaskHandle_t h) { (void)h; }
static inline void xTaskNotifyGive(TaskHandle_t h) { (void)h; }
static inline uint32_t ulTaskNotifyTake(int clear, TickType_t t) { (void)clear; (void)t; return 0; }
