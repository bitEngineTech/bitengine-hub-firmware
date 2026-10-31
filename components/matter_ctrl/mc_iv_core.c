/* mc_iv_core —— 访谈结果的存储格式与组装（见 mc_iv_core.h）。纯逻辑，test_host/ 原样编。 */
#include "mc_iv_core.h"

#include <stdbool.h>
#include <string.h>

static void put16(uint8_t **p, uint16_t v) { (*p)[0] = (uint8_t)v; (*p)[1] = (uint8_t)(v >> 8); *p += 2; }
static void put32(uint8_t **p, uint32_t v) { for (int i = 0; i < 4; i++) (*p)[i] = (uint8_t)(v >> (8 * i)); *p += 4; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static size_t bounded_len(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

int mc_iv_encode(const mc_iv_t *iv, uint8_t *buf, size_t cap)
{
    if (!iv || !buf || iv->n_ep > MC_IV_EP_MAX) return -1;
    size_t need = 14;
    const char *strs[3] = { iv->vendor, iv->product, iv->label };
    for (int i = 0; i < 3; i++) {
        const size_t n = bounded_len(strs[i], MC_IV_STR_MAX + 1);
        if (n > MC_IV_STR_MAX) return -1;
        need += 1 + n;
    }
    for (unsigned i = 0; i < iv->n_ep; i++) {
        if (iv->ep[i].n_dt > MC_IV_DT_MAX || iv->ep[i].n_cl > MC_IV_CL_MAX) return -1;
        need += 4 + iv->ep[i].n_dt * 4u + iv->ep[i].n_cl * 2u;
    }
    if (cap < need) return -1;
    uint8_t *p = buf;
    for (int i = 0; i < 8; i++) *p++ = (uint8_t)(iv->node_id >> (8 * i));
    put16(&p, iv->vid);
    put16(&p, iv->pid);
    *p++ = iv->flags;
    *p++ = iv->n_ep;
    for (int i = 0; i < 3; i++) {
        const size_t n = bounded_len(strs[i], MC_IV_STR_MAX);
        *p++ = (uint8_t)n;
        memcpy(p, strs[i], n);
        p += n;
    }
    for (unsigned i = 0; i < iv->n_ep; i++) {
        const mc_iv_ep_t *e = &iv->ep[i];
        put16(&p, e->ep);
        *p++ = e->n_dt;
        *p++ = e->n_cl;
        for (unsigned k = 0; k < e->n_dt; k++) put32(&p, e->dt[k]);
        for (unsigned k = 0; k < e->n_cl; k++) put16(&p, e->cl[k]);
    }
    return (int)(p - buf);
}

bool mc_iv_decode(const uint8_t *buf, size_t len, mc_iv_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!buf || len < 14) return false;
    mc_iv_t t;
    memset(&t, 0, sizeof(t));
    const uint8_t *p = buf, *end = buf + len;
    for (int i = 0; i < 8; i++) t.node_id |= (uint64_t)p[i] << (8 * i);
    p += 8;
    t.vid = get16(p); p += 2;
    t.pid = get16(p); p += 2;
    t.flags = *p++;
    t.n_ep = *p++;
    if (t.n_ep > MC_IV_EP_MAX) return false;
    char *strs[3] = { t.vendor, t.product, t.label };
    for (int i = 0; i < 3; i++) {
        if (p >= end) return false;
        const uint8_t n = *p++;
        if (n > MC_IV_STR_MAX || (size_t)(end - p) < n) return false;
        memcpy(strs[i], p, n);
        strs[i][n] = '\0';
        if (memchr(strs[i], '\0', n)) return false;
        p += n;
    }
    for (unsigned i = 0; i < t.n_ep; i++) {
        if ((size_t)(end - p) < 4) return false;
        mc_iv_ep_t *e = &t.ep[i];
        e->ep = get16(p); p += 2;
        e->n_dt = *p++;
        e->n_cl = *p++;
        if (e->n_dt > MC_IV_DT_MAX || e->n_cl > MC_IV_CL_MAX) return false;
        if ((size_t)(end - p) < e->n_dt * 4u + e->n_cl * 2u) return false;
        for (unsigned k = 0; k < e->n_dt; k++, p += 4) e->dt[k] = get32(p);
        for (unsigned k = 0; k < e->n_cl; k++, p += 2) e->cl[k] = get16(p);
        for (unsigned j = 0; j < i; j++) if (t.ep[j].ep == e->ep) return false;
    }
    if (p != end) return false;
    *out = t;
    return true;
}

mc_iv_ep_t *mc_iv_ep(mc_iv_t *iv, uint16_t ep)
{
    if (!iv) return NULL;
    for (unsigned i = 0; i < iv->n_ep; i++) if (iv->ep[i].ep == ep) return &iv->ep[i];
    if (iv->n_ep >= MC_IV_EP_MAX) {
        iv->flags |= MC_IV_F_EP_TRUNC;
        return NULL;
    }
    mc_iv_ep_t *e = &iv->ep[iv->n_ep++];
    memset(e, 0, sizeof(*e));
    e->ep = ep;
    return e;
}

bool mc_iv_complete(const mc_iv_t *iv)
{
    if (!iv) return false;
    for (unsigned i = 0; i < iv->n_ep && i < MC_IV_EP_MAX; i++)
        if (iv->ep[i].ep == 0 && iv->ep[i].n_cl > 0) return true;
    return false;
}

void mc_iv_add_dt(mc_iv_t *iv, mc_iv_ep_t *e, uint32_t dt)
{
    if (!iv || !e) return;
    for (unsigned k = 0; k < e->n_dt; k++) if (e->dt[k] == dt) return;
    if (e->n_dt >= MC_IV_DT_MAX) { iv->flags |= MC_IV_F_CL_TRUNC; return; }
    e->dt[e->n_dt++] = dt;
}

void mc_iv_add_cl(mc_iv_t *iv, mc_iv_ep_t *e, uint32_t cl)
{
    if (!iv || !e || cl > 0x7FFFu) return;
    for (unsigned k = 0; k < e->n_cl; k++) if (e->cl[k] == cl) return;
    if (e->n_cl >= MC_IV_CL_MAX) { iv->flags |= MC_IV_F_CL_TRUNC; return; }
    e->cl[e->n_cl++] = (uint16_t)cl;
}

void mc_iv_set_str(char dst[MC_IV_STR_MAX + 1], const char *src, size_t len)
{
    if (!dst) return;
    dst[0] = '\0';
    if (!src) return;
    size_t n = len;
    for (size_t i = 0; i < len; i++) if (src[i] == '\0') { n = i; break; }
    if (n > MC_IV_STR_MAX) {
        n = MC_IV_STR_MAX;
        while (n > 0 && ((unsigned char)src[n] & 0xC0u) == 0x80u) n--;   /* 退到字符边界 */
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

bool mc_iv_has_cluster(const mc_iv_t *iv, uint16_t cl)
{
    for (unsigned i = 0; iv && i < iv->n_ep; i++)
        for (unsigned k = 0; k < iv->ep[i].n_cl; k++) if (iv->ep[i].cl[k] == cl) return true;
    return false;
}
