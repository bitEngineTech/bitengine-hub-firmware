/* hub_evlog —— 最近事件环（见 include/hub_evlog.h）。纯 C，不依赖 ESP-IDF。 */
#include "hub_evlog.h"

#include <string.h>

static const char *const k_class[HUB_EVC_COUNT] = {
    "alarm", "security", "availability", "lifecycle", "gateway", "battery",
};

const char *hev_class_name(hub_evt_class_t c)
{
    return (unsigned)c < HUB_EVC_COUNT ? k_class[c] : NULL;
}

void hev_ring_init(hev_ring_t *r)
{
    if (!r) return;
    memset(r, 0, sizeof(*r));
    r->next_seq = 1;
}

static bool terminated(const char *s, size_t cap) { return memchr(s, '\0', cap) != NULL; }

static bool kind_ok(const char *k)
{
    if (!k[0]) return false;
    for (const char *p = k; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    }
    return true;
}

static bool clock_ok(const char *c)
{
    if (!c[0]) return true;                        /* 没对时 */
    if (strlen(c) != 5 || c[2] != ':') return false;
    for (int i = 0; i < 5; i++) if (i != 2 && (c[i] < '0' || c[i] > '9')) return false;
    return (c[0] - '0') * 10 + (c[1] - '0') <= 23 && c[3] <= '5';
}

uint32_t hev_ring_add(hev_ring_t *r, const hev_t *ev)
{
    if (!r || !ev || (unsigned)ev->cls >= HUB_EVC_COUNT || !terminated(ev->dev_id, sizeof(ev->dev_id))
        || !terminated(ev->kind, sizeof(ev->kind)) || !terminated(ev->text, sizeof(ev->text))
        || !terminated(ev->clock, sizeof(ev->clock)) || !kind_ok(ev->kind) || !clock_ok(ev->clock) || !ev->text[0]) {
        return 0;
    }
    if (r->next_seq == 0) r->next_seq = 1;         /* 回绕（约 40 亿条）时跳过 0：0 是空槽 */
    hev_t *slot = &r->e[r->head];
    *slot = *ev;
    slot->seq = r->next_seq++;
    r->head = (r->head + 1) % HEV_RING_CAP;
    if (r->count < HEV_RING_CAP) r->count++;
    return slot->seq;
}

const hev_t *hev_ring_newest(const hev_ring_t *r, size_t n)
{
    if (!r || n >= r->count) return NULL;
    const uint32_t i = (r->head + HEV_RING_CAP - 1u - (uint32_t)n) % HEV_RING_CAP;
    return &r->e[i];
}

static void write_one(hj_t *w, const hev_t *e, uint64_t now_ms)
{
    hj_obj_begin(w);
    hj_kuint(w, "seq", e->seq);
    hj_kstr(w, "class", hev_class_name(e->cls));
    hj_kstr(w, "kind", e->kind);
    if (e->dev_id[0]) hj_kstr(w, "device", e->dev_id);
    hj_kuint(w, "age_s", now_ms >= e->mono_ms ? (now_ms - e->mono_ms) / 1000u : 0u);
    if (e->clock[0]) hj_kstr(w, "time", e->clock);
    hj_kstr(w, "text", e->text);
    hj_obj_end(w);
}

void hev_write_recent(hj_t *w, const hev_ring_t *r, uint64_t now_ms, uint32_t limit, uint32_t since_s)
{
    if (!w || !r) return;
    if (limit < 1) limit = 1;
    if (limit > 50) limit = 50;
    hj_set_reserve(w, 32);                         /* ,"count":NN,"truncated":true} */
    hj_obj_begin(w);
    hj_key(w, "events");
    hj_arr_begin(w);
    uint32_t n = 0;
    bool truncated = false;
    for (size_t i = 0; n < limit; i++) {
        const hev_t *e = hev_ring_newest(r, i);
        if (!e) break;
        if (since_s && (now_ms < e->mono_ms ? 0u : now_ms - e->mono_ms) > (uint64_t)since_s * 1000u) break;   /* 再往后只会更旧 */
        hj_mark_t m = hj_mark(w);
        write_one(w, e, now_ms);
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
