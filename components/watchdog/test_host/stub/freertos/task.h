#pragma once
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
/* 测试里任务不真的跑：被测函数由测试直接调用，时间靠 g_fake_us 推进。 */
static inline void vTaskDelay(TickType_t t) { (void)t; }
static inline BaseType_t xTaskCreate(TaskFunction_t f, const char *n, uint32_t s,
                                     void *p, UBaseType_t pr, TaskHandle_t *h)
{ (void)f; (void)n; (void)s; (void)p; (void)pr; if (h) *h = (TaskHandle_t)1; return pdPASS; }
extern TaskHandle_t g_fake_current_task;
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return g_fake_current_task; }
static inline BaseType_t xPortInIsrContext(void) { return 0; }
extern uint32_t g_fake_stack_headroom;
static inline uint32_t uxTaskGetStackHighWaterMark(TaskHandle_t h) { (void)h; return g_fake_stack_headroom; }
