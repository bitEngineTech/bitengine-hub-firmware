/* mc_ident_core —— 网关 fabric 身份的存储格式（格式见 mc_ident_core.h）。纯逻辑，test_host/ 原样编。 */
#include "mc_ident_core.h"

#include <string.h>

static void put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

void mc_ident_wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (p && n--) *v++ = 0;
}

int mc_ident_encode(const mc_ident_t *id, uint8_t *buf, size_t cap)
{
    if (!id || !buf) return -MC_IDENT_EARG;
    if (id->fabric_id == 0 || id->rcac_id == 0) return -MC_IDENT_EZERO;
    if (id->rcac_len == 0 || id->rcac_len > MC_IDENT_RCAC_MAX) return -MC_IDENT_ELEN;
    const size_t need = 8 + 8 + 1 + MC_IDENT_KP_LEN + MC_IDENT_IPK_LEN + 2 + (size_t)id->rcac_len;
    if (cap < need) return -MC_IDENT_EARG;
    uint8_t *p = buf;
    put_u64(p, id->fabric_id); p += 8;
    put_u64(p, id->rcac_id);   p += 8;
    *p++ = MC_IDENT_KP_LEN;
    memcpy(p, id->kp, MC_IDENT_KP_LEN);    p += MC_IDENT_KP_LEN;
    memcpy(p, id->ipk, MC_IDENT_IPK_LEN);  p += MC_IDENT_IPK_LEN;
    *p++ = (uint8_t)id->rcac_len;
    *p++ = (uint8_t)(id->rcac_len >> 8);
    memcpy(p, id->rcac, id->rcac_len);     p += id->rcac_len;
    return (int)(p - buf);
}

mc_ident_err_t mc_ident_decode(const uint8_t *buf, size_t len, mc_ident_t *out)
{
    if (!out) return MC_IDENT_EARG;
    memset(out, 0, sizeof(*out));
    if (!buf) return MC_IDENT_EARG;
    const size_t fixed = 8 + 8 + 1 + MC_IDENT_KP_LEN + MC_IDENT_IPK_LEN + 2;
    if (len < fixed) return MC_IDENT_ELEN;
    mc_ident_err_t rc = MC_IDENT_OK;
    const uint8_t *p = buf;
    out->fabric_id = get_u64(p); p += 8;
    out->rcac_id = get_u64(p);   p += 8;
    if (*p++ != MC_IDENT_KP_LEN) { rc = MC_IDENT_EKP; goto bad; }
    memcpy(out->kp, p, MC_IDENT_KP_LEN);   p += MC_IDENT_KP_LEN;
    memcpy(out->ipk, p, MC_IDENT_IPK_LEN); p += MC_IDENT_IPK_LEN;
    out->rcac_len = (uint16_t)(p[0] | (p[1] << 8)); p += 2;
    if (out->rcac_len == 0 || out->rcac_len > MC_IDENT_RCAC_MAX || len != fixed + out->rcac_len) { rc = MC_IDENT_ELEN; goto bad; }
    memcpy(out->rcac, p, out->rcac_len);
    if (out->fabric_id == 0 || out->rcac_id == 0) { rc = MC_IDENT_EZERO; goto bad; }
    return MC_IDENT_OK;
bad:
    mc_ident_wipe(out, sizeof(*out));
    return rc;
}
