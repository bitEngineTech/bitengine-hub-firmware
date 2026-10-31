#pragma once
#include "freertos/FreeRTOS.h"
#include <string.h>
#include <stdlib.h>
/* 极简队列：够 wd_recovery 用（定长、单生产单消费、非阻塞语义由测试控制）。 */
typedef struct { char *buf; int isz, cap, head, tail, count; } wd_fake_queue_t;
typedef wd_fake_queue_t *QueueHandle_t;
static inline QueueHandle_t xQueueCreate(int n, int isz)
{ wd_fake_queue_t *q = calloc(1, sizeof(*q)); q->buf = calloc(n, isz);
  q->isz = isz; q->cap = n; return q; }
static inline void vQueueDelete(QueueHandle_t q) { if (q) { free(q->buf); free(q); } }
static inline BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t t)
{ (void)t; if (!q || q->count == q->cap) return pdFALSE;
  memcpy(q->buf + q->head * q->isz, item, q->isz);
  q->head = (q->head + 1) % q->cap; q->count++; return pdTRUE; }
static inline BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t t)
{ (void)t; if (!q || q->count == 0) return pdFALSE;
  memcpy(out, q->buf + q->tail * q->isz, q->isz);
  q->tail = (q->tail + 1) % q->cap; q->count--; return pdTRUE; }
