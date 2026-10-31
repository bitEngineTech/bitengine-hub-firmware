#pragma once
#include "FreeRTOS.h"
typedef void *SemaphoreHandle_t;
static int g_fake_mutex_obj;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &g_fake_mutex_obj; }
static inline int xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdTRUE; }
static inline int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
