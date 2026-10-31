/* zh_zcl —— ZCL 帧的最小编解码（规则与出处见 include/zh_zcl.h）。 */
#include "zh_zcl.h"

#include <string.h>

int zh_zcl_parse(const uint8_t *b, size_t n, zh_zcl_frame_t *f)
{
    if (!b || !f) return ZH_ZCL_E_ARG;
    memset(f, 0, sizeof(*f));
    if (n < 3) return ZH_ZCL_E_SHORT;
    f->fc = b[0];
    size_t i = 1;
    if (f->fc & ZH_ZCL_FC_MFG) {
        if (n < 5) return ZH_ZCL_E_SHORT;
        f->mfg = (uint16_t)(b[1] | (b[2] << 8));
        i = 3;
    }
    f->seq = b[i];
    f->cmd = b[i + 1];
    f->pl = b + i + 2;
    f->n = n - (i + 2);
    return ZH_ZCL_OK;
}

size_t zh_zcl_enc_read_attrs(uint8_t *out, size_t cap, uint8_t seq, const uint16_t *ids, size_t n_ids)
{
    if (!out || !ids || !n_ids) return 0;
    const size_t need = 3 + 2 * n_ids;
    if (need > cap) return 0;
    out[0] = ZH_ZCL_FC_DIS_DR;                    /* 全局命令、客户端 → 服务端、不带厂商码 */
    out[1] = seq;
    out[2] = ZH_ZCL_CMD_READ_ATTR;
    for (size_t k = 0; k < n_ids; k++) {
        out[3 + 2 * k] = (uint8_t)ids[k];
        out[4 + 2 * k] = (uint8_t)(ids[k] >> 8);
    }
    return need;
}

size_t zh_zcl_enc_write_eui64(uint8_t *out, size_t cap, uint8_t seq, uint16_t attr, const uint8_t eui[8])
{
    if (!out || !eui || cap < 14) return 0;
    out[0] = 0x00;                                /* 全局命令、客户端 → 服务端、不带厂商码、不禁默认应答（同 SDK） */
    out[1] = seq;
    out[2] = ZH_ZCL_CMD_WRITE_ATTR;
    out[3] = (uint8_t)attr;
    out[4] = (uint8_t)(attr >> 8);
    out[5] = ZH_ZCL_T_EUI64;
    memcpy(out + 6, eui, 8);                       /* 空中小端；EUI64 数组本来就是小端（SDK 直接拷 sl_zigbee_af_get_eui64 的结果） */
    return 14;
}

size_t zh_zcl_enc_cfg_report(uint8_t *out, size_t cap, uint8_t seq, uint16_t attr, uint8_t type, uint16_t min_s, uint16_t max_s, uint32_t change, uint8_t chg_len)
{
    const size_t need = 3u + 8u + chg_len;
    if (!out || chg_len > 4 || need > cap) return 0;
    out[0] = 0x00;                                /* 全局命令、客户端 → 服务端、不带厂商码、不禁默认应答（同 SDK） */
    out[1] = seq;
    out[2] = ZH_ZCL_CMD_CFG_REPORT;
    out[3] = 0x00;                                /* 方向：设备上报（reporting.c:439 REPORTED） */
    out[4] = (uint8_t)attr;
    out[5] = (uint8_t)(attr >> 8);
    out[6] = type;
    out[7] = (uint8_t)min_s;
    out[8] = (uint8_t)(min_s >> 8);
    out[9] = (uint8_t)max_s;
    out[10] = (uint8_t)(max_s >> 8);
    for (uint8_t k = 0; k < chg_len; k++) out[11 + k] = (uint8_t)(change >> (8u * k));
    return need;
}

/* 定长类型的字节数；-1 = 不是定长类型（字符串单独算）或不认识（types.xml:19-75 的类型表） */
static int fixed_len(uint8_t t)
{
    if (t == 0x00 || t == 0xFF) return 0;                         /* no_data / unknown */
    if (t >= 0x08 && t <= 0x0F) return t - 0x07;                  /* data8 … data64 */
    if (t == 0x10) return 1;                                      /* boolean */
    if (t >= 0x18 && t <= 0x1F) return t - 0x17;                  /* bitmap8 … bitmap64 */
    if (t >= 0x20 && t <= 0x27) return t - 0x1F;                  /* int8u … int64u */
    if (t >= 0x28 && t <= 0x2F) return t - 0x27;                  /* int8s … int64s */
    if (t >= 0x30 && t <= 0x32) return t - 0x2F;                  /* enum8 / enum16 / enum24 */
    switch (t) {
    case 0x38: return 2;                                          /* float_semi */
    case 0x39: return 4;                                          /* float_single */
    case 0x3A: return 8;                                          /* float_double */
    case 0xE0: case 0xE1: case 0xE2: return 4;                    /* time_of_day / date / utc_time */
    case 0xE8: case 0xE9: return 2;                               /* cluster_id / attribute_id */
    case 0xEA: return 4;                                          /* bacnet_oid */
    case 0xF0: return 8;                                          /* ieee_address */
    case 0xF1: return 16;                                         /* security_key */
    default:   return -1;
    }
}

int zh_zcl_type_len(uint8_t type, const uint8_t *v, size_t avail, size_t *len)
{
    if (!len || (avail && !v)) return ZH_ZCL_E_ARG;
    size_t need;
    if (type == 0x41 || type == 0x42) {                           /* octet / char string：1 B 长度，0xFF = 无效（没有内容） */
        if (avail < 1) return ZH_ZCL_E_SHORT;
        need = 1 + (v[0] == 0xFF ? 0 : v[0]);
    } else if (type == 0x43 || type == 0x44) {                    /* long octet / long char string：2 B 长度，0xFFFF = 无效 */
        if (avail < 2) return ZH_ZCL_E_SHORT;
        const uint16_t l = (uint16_t)(v[0] | (v[1] << 8));
        need = 2 + (l == 0xFFFF ? 0 : l);
    } else {
        const int f = fixed_len(type);
        if (f < 0) return ZH_ZCL_E_TYPE;                          /* array / struct / set / bag 与没定义的类型 */
        need = (size_t)f;
    }
    if (need > avail) return ZH_ZCL_E_SHORT;
    *len = need;
    return ZH_ZCL_OK;
}

void zh_zcl_iter_init(zh_zcl_iter_t *it, const zh_zcl_frame_t *f)
{
    if (!it) return;
    it->p = f ? f->pl : NULL;
    it->n = f ? f->n : 0;
}

int zh_zcl_next_rsp_rec(zh_zcl_iter_t *it, zh_zcl_attr_t *a)
{
    if (!it || !a) return ZH_ZCL_E_ARG;
    memset(a, 0, sizeof(*a));
    if (it->n == 0) return 0;
    if (it->n < 3) {                                              /* 半条记录：剩下的丢掉 */
        it->n = 0;
        return ZH_ZCL_E_SHORT;
    }
    a->id = (uint16_t)(it->p[0] | (it->p[1] << 8));
    a->status = it->p[2];
    size_t used = 3;
    if (a->status == ZH_ZCL_ST_SUCCESS) {
        if (it->n < 4) {
            it->n = 0;
            return ZH_ZCL_E_SHORT;
        }
        a->type = it->p[3];
        size_t vl = 0;
        const int r = zh_zcl_type_len(a->type, it->p + 4, it->n - 4, &vl);
        if (r != ZH_ZCL_OK) {                                     /* 不认识的类型 / 值越界：不知道下一条从哪开始，停 */
            it->n = 0;
            return r;
        }
        a->val = it->p + 4;
        a->len = vl;
        used = 4 + vl;
    }
    it->p += used;
    it->n -= used;
    return 1;
}

int zh_zcl_next_report_rec(zh_zcl_iter_t *it, zh_zcl_attr_t *a)
{
    if (!it || !a) return ZH_ZCL_E_ARG;
    memset(a, 0, sizeof(*a));
    if (it->n == 0) return 0;
    if (it->n < 3) {                                              /* 属性号 + 类型都不够：剩下的丢掉 */
        it->n = 0;
        return ZH_ZCL_E_SHORT;
    }
    a->id = (uint16_t)(it->p[0] | (it->p[1] << 8));
    a->status = ZH_ZCL_ST_SUCCESS;
    a->type = it->p[2];
    size_t vl = 0;
    const int r = zh_zcl_type_len(a->type, it->p + 3, it->n - 3, &vl);
    if (r != ZH_ZCL_OK) {                                         /* 不认识的类型 / 值越界：不知道下一条从哪开始，停 */
        it->n = 0;
        return r;
    }
    a->val = it->p + 3;
    a->len = vl;
    it->p += 3 + vl;
    it->n -= 3 + vl;
    return 1;
}

bool zh_zcl_attr_int(const zh_zcl_attr_t *a, int32_t *v)
{
    if (!a || !v || a->status != ZH_ZCL_ST_SUCCESS || !a->val || a->type < 0x28 || a->type > 0x2B || a->len < 1 || a->len > 4) return false;
    uint32_t x = 0;
    for (size_t k = 0; k < a->len; k++) x |= (uint32_t)a->val[k] << (8 * k);
    const unsigned bits = (unsigned)(8 * a->len);
    if (bits < 32 && (x & (1u << (bits - 1)))) x |= ~0u << bits;   /* 符号扩展 */
    *v = (int32_t)x;
    return true;
}

bool zh_zcl_attr_str(const zh_zcl_attr_t *a, const uint8_t **s, size_t *len)
{
    if (!a || !s || !len || a->status != ZH_ZCL_ST_SUCCESS || a->type != ZH_ZCL_T_CHAR_STR || !a->val || a->len < 1) return false;
    if (a->val[0] == 0xFF) return false;
    *s = a->val + 1;
    *len = a->val[0];
    return true;
}

bool zh_zcl_attr_uint(const zh_zcl_attr_t *a, uint32_t *v)
{
    if (!a || !v || a->status != ZH_ZCL_ST_SUCCESS || !a->val) return false;
    const bool ok_type = (a->type >= 0x20 && a->type <= 0x23) || (a->type >= 0x30 && a->type <= 0x32) || (a->type >= 0x18 && a->type <= 0x1B) ||
                         (a->type >= 0x08 && a->type <= 0x0B);
    if (!ok_type || a->len < 1 || a->len > 4) return false;
    uint32_t x = 0;
    for (size_t k = 0; k < a->len; k++) x |= (uint32_t)a->val[k] << (8 * k);
    *v = x;
    return true;
}
