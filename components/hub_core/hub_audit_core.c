/* hub_audit_core —— 审计环（见 include/hub_audit_core.h）。纯 C，不依赖 ESP-IDF。 */
#include "hub_audit_core.h"

#include <string.h>

void hau_init(hau_ring_t *r)
{
    if (!r) return;
    memset(r, 0, sizeof(*r));
    r->next_seq = 1;
}

void hau_token(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    size_t n = 0;
    for (const char *p = src ? src : ""; *p && n + 1 < cap; p++) {
        const char c = *p;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        dst[n++] = ok ? c : '?';
    }
    dst[n] = '\0';
}

static bool clock_ok(const char *c)
{
    if (!c[0]) return true;
    if (strlen(c) != 5 || c[2] != ':') return false;
    for (int i = 0; i < 5; i++) {
        if (i != 2 && (c[i] < '0' || c[i] > '9')) return false;
    }
    return (c[0] - '0') * 10 + (c[1] - '0') <= 23 && c[3] <= '5';
}

static const hau_entry_t *newest(const hau_ring_t *r, uint32_t n);

uint32_t hau_add(hau_ring_t *r, const hau_entry_t *in, bool *merged)
{
    if (merged) *merged = false;
    if (!r || !in) return 0;
    hau_entry_t e;
    memset(&e, 0, sizeof(e));
    /* 输入的字段可能没有 NUL（调用方的缓冲）：按各自容量读 */
    char tmp[HAU_TOKEN_MAX + 1];
    memcpy(tmp, in->command, HAU_TOKEN_MAX);
    tmp[HAU_TOKEN_MAX] = '\0';
    hau_token(e.command, sizeof(e.command), tmp);
    if (!e.command[0]) return 0;
    memcpy(tmp, in->device, HAU_DEV_MAX);
    tmp[HAU_DEV_MAX] = '\0';
    hau_token(e.device, sizeof(e.device), tmp);
    memcpy(tmp, in->action, HAU_TOKEN_MAX);
    tmp[HAU_TOKEN_MAX] = '\0';
    hau_token(e.action, sizeof(e.action), tmp);
    memcpy(tmp, in->result, HAU_TOKEN_MAX);
    tmp[HAU_TOKEN_MAX] = '\0';
    hau_token(e.result, sizeof(e.result), tmp);
    memcpy(tmp, in->clock, sizeof(in->clock));
    tmp[sizeof(in->clock)] = '\0';
    if (clock_ok(tmp)) memcpy(e.clock, tmp, sizeof(e.clock));
    e.mono_ms = in->mono_ms;
    e.src = in->src == HAU_SRC_REST ? HAU_SRC_REST : HAU_SRC_MUSE;
    e.dur_ms = in->dur_ms;
    if (r->next_seq == 0) r->next_seq = 1;
    /* 最近 HAU_MERGE_SCAN 条里有相同的、相隔 ≤ HAU_MERGE_MS（时间倒退的不算）：合并进去、挪到最前 */
    for (uint32_t n = 0; n < HAU_MERGE_SCAN; n++) {
        const hau_entry_t *c = newest(r, n);
        if (!c) break;
        if (c->src != e.src || e.mono_ms < c->mono_ms || e.mono_ms - c->mono_ms > HAU_MERGE_MS || strcmp(c->command, e.command)
            || strcmp(c->device, e.device) || strcmp(c->action, e.action) || strcmp(c->result, e.result)) continue;
        hau_entry_t m = *c;
        if (m.repeat < UINT16_MAX) m.repeat++;
        m.mono_ms = e.mono_ms;
        memcpy(m.clock, e.clock, sizeof(m.clock));
        m.dur_ms = e.dur_ms;
        m.seq = r->next_seq++;
        for (uint32_t k = n; k > 0; k--) r->e[(r->head + HAU_CAP - 1u - k) % HAU_CAP] = r->e[(r->head + HAU_CAP - k) % HAU_CAP];
        r->e[(r->head + HAU_CAP - 1u) % HAU_CAP] = m;
        if (merged) *merged = true;
        return m.seq;
    }
    e.repeat = 1;
    e.seq = r->next_seq++;
    r->e[r->head] = e;
    r->head = (r->head + 1) % HAU_CAP;
    if (r->count < HAU_CAP) r->count++;
    return e.seq;
}

static const hau_entry_t *newest(const hau_ring_t *r, uint32_t n)
{
    if (n >= r->count) return NULL;
    return &r->e[(r->head + HAU_CAP - 1u - n) % HAU_CAP];
}

void hau_write(hj_t *w, const hau_ring_t *r, uint64_t now_ms, uint32_t limit)
{
    if (!w || !r) return;
    if (limit < 1) limit = 1;
    if (limit > HAU_CAP) limit = HAU_CAP;
    hj_set_reserve(w, 32);                         /* ],"count":NNN,"truncated":true} */
    hj_obj_begin(w);
    hj_key(w, "entries");
    hj_arr_begin(w);
    uint32_t n = 0;
    bool truncated = false;
    for (uint32_t i = 0; n < limit; i++) {
        const hau_entry_t *e = newest(r, i);
        if (!e) break;
        hj_mark_t m = hj_mark(w);
        hj_obj_begin(w);
        hj_kuint(w, "seq", e->seq);
        hj_kuint(w, "age_s", now_ms >= e->mono_ms ? (now_ms - e->mono_ms) / 1000u : 0u);
        if (e->clock[0]) hj_kstr(w, "time", e->clock);
        hj_kstr(w, "source", e->src == HAU_SRC_REST ? "rest" : "muse");
        hj_kstr(w, "command", e->command);
        if (e->device[0]) hj_kstr(w, "device", e->device);
        if (e->action[0]) hj_kstr(w, "action", e->action);
        hj_kstr(w, "result", e->result);
        hj_kuint(w, "ms", e->dur_ms);
        if (e->repeat > 1) hj_kuint(w, "repeat", e->repeat);
        hj_obj_end(w);
        if (w->truncated) {
            hj_rollback(w, m);
            truncated = true;
            break;
        }
        n++;
    }
    hj_arr_end(w);
    hj_set_reserve(w, 0);
    hj_kuint(w, "count", n);
    if (truncated) hj_kbool(w, "truncated", true);
    hj_obj_end(w);
}
