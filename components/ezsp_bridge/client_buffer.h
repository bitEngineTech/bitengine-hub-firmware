#pragma once

#include <stdint.h>
#include "ezsp_bridge.h"

typedef struct {
    uint8_t  buf[CLIENT_TX_BUF_SIZE];
    uint16_t head;
    uint16_t tail;
    uint16_t count;
} ring_buffer_t;

void ring_buffer_reset(ring_buffer_t *rb);
int  ring_buffer_free(const ring_buffer_t *rb);
static inline int ring_buffer_pending(const ring_buffer_t *rb) { return rb->count; }
int  ring_buffer_write(ring_buffer_t *rb, const uint8_t *data, int len);
int  ring_buffer_peek(const ring_buffer_t *rb, uint8_t *data, int max_len);
void ring_buffer_consume(ring_buffer_t *rb, int len);
