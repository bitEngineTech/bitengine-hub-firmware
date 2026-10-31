#include "client_buffer.h"
#include <string.h>

#define BUF_CAP  sizeof(((ring_buffer_t *)0)->buf)

void ring_buffer_reset(ring_buffer_t *rb)
{
    rb->head  = 0;
    rb->tail  = 0;
    rb->count = 0;
}

int ring_buffer_free(const ring_buffer_t *rb)
{
    return (int)(BUF_CAP - rb->count);
}

int ring_buffer_write(ring_buffer_t *rb, const uint8_t *data, int len)
{
    int space = ring_buffer_free(rb);
    if (space < len) {
        return 0;  // caller decides whether to drop client
    }
    for (int i = 0; i < len; i++) {
        rb->buf[rb->head] = data[i];
        rb->head = (rb->head + 1) % BUF_CAP;
    }
    rb->count += len;
    return len;
}

int ring_buffer_peek(const ring_buffer_t *rb, uint8_t *data, int max_len)
{
    int avail = rb->count;
    if (avail > max_len) avail = max_len;
    uint16_t pos = rb->tail;
    for (int i = 0; i < avail; i++) {
        data[i] = rb->buf[pos];
        pos = (pos + 1) % BUF_CAP;
    }
    return avail;
}

void ring_buffer_consume(ring_buffer_t *rb, int len)
{
    if (len > rb->count) len = rb->count;
    rb->tail = (rb->tail + len) % BUF_CAP;
    rb->count -= len;
}
