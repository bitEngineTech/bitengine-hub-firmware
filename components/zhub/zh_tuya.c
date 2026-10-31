/* zh_tuya —— 涂鸦私有簇 0xEF00 的读数（规则见 include/zh_tuya.h）。 */
#include "zh_tuya.h"

#include <string.h>

#include "zh_zcl.h"

bool zh_tuya_is_data_cmd(uint8_t cmd)
{
    return cmd == ZH_TUYA_CMD_DATA_RSP || cmd == ZH_TUYA_CMD_DATA_REPORT || cmd == ZH_TUYA_CMD_STATUS_ALT || cmd == ZH_TUYA_CMD_STATUS;
}

bool zh_tuya_iter_init(zh_tuya_iter_t *it, const uint8_t *pl, size_t n)
{
    if (!it || !pl || n < 2) return false;
    it->p = pl;
    it->n = n;
    it->off = 2;                                   /* seq */
    return true;
}

int zh_tuya_next(zh_tuya_iter_t *it, zh_tuya_dp_t *out)
{
    if (!it || !out) return -1;
    if (it->off >= it->n) return 0;
    if (it->n - it->off < 4) {                     /* 头都不全 */
        it->off = it->n;
        return -1;
    }
    const uint8_t *h = it->p + it->off;
    const uint16_t len = (uint16_t)((h[2] << 8) | h[3]);   /* 大端（buffaloZcl.js:605-607） */
    if (it->n - it->off - 4 < len) {
        it->off = it->n;
        return -1;
    }
    out->dp = h[0];
    out->type = h[1];
    out->len = len;
    out->data = h + 4;
    it->off += 4u + len;
    return 1;
}

bool zh_tuya_dp_int(const zh_tuya_dp_t *dp, int32_t *out)
{
    if (!dp || !out || (dp->len && !dp->data)) return false;
    switch (dp->type) {
    case ZH_TUYA_T_BOOL:
        if (dp->len < 1) return false;
        *out = dp->data[0] == 1u ? 1 : 0;          /* tuya.js:199：只有 1 算真 */
        return true;
    case ZH_TUYA_T_ENUM:
        if (dp->len < 1) return false;
        *out = dp->data[0];
        return true;
    case ZH_TUYA_T_NUMBER:
    case ZH_TUYA_T_BITMAP: {
        if (dp->len < 1 || dp->len > 4) return false;
        uint32_t v = 0;
        for (unsigned i = 0; i < dp->len; i++) v = (v << 8) | dp->data[i];
        *out = (int32_t)v;                         /* 4 字节：int32 补码；不足 4 字节：高位是 0，就是无符号值（tuya.js:177-183） */
        return true;
    }
    default:
        return false;                              /* raw / string / 不认识的类型：表里不会有 */
    }
}

static const char *pool_str(uint16_t off)
{
    return off < zh_tdb_str_len ? zh_tdb_str + off : "";
}

/* 按（厂商名，型号）二分；找到回指纹下标，没有 -1 */
static int fp_find(const char *mfr, const char *model)
{
    unsigned lo = 0, hi = zh_tdb_n_fps;
    while (lo < hi) {
        const unsigned mid = lo + (hi - lo) / 2u;
        int c = strcmp(pool_str(zh_tdb_fps[mid].mfr), mfr);
        if (c == 0) c = strcmp(pool_str(zh_tdb_fps[mid].model), model);
        if (c == 0) return (int)mid;
        if (c < 0) lo = mid + 1u;
        else hi = mid;
    }
    return -1;
}

int zh_tuya_find(const char *mfr, const char *model)
{
    if (!mfr) mfr = "";
    if (!model) model = "";
    int f = -1;
    if (mfr[0] && model[0]) f = fp_find(mfr, model);
    if (f < 0 && mfr[0]) f = fp_find(mfr, "");
    if (f < 0 && model[0]) f = fp_find("", model);
    if (f < 0) return -1;
    const uint16_t dev = zh_tdb_fps[f].dev;
    return dev < zh_tdb_n_devs ? (int)dev : -1;
}

uint32_t zh_tuya_fields(int dev)
{
    return dev >= 0 && (unsigned)dev < zh_tdb_n_devs ? zh_tdb_devs[dev].fields : 0u;
}

static const zh_tdb_ctl_t *ctl_of(int dev)
{
    if (dev < 0 || (unsigned)dev >= zh_tdb_n_devs) return NULL;
    const uint16_t c = zh_tdb_devs[dev].ctl;
    return c < zh_tdb_n_ctls ? &zh_tdb_ctls[c] : NULL;
}

uint32_t zh_tuya_ctl_fields(int dev)
{
    const zh_tdb_ctl_t *c = ctl_of(dev);
    if (!c) return 0;
    return (c->onoff_dp ? 1u << ZH_TF_ONOFF : 0u) | (c->level_dp ? 1u << ZH_TF_LEVEL : 0u);
}

/* 一条数据点：[dp][类型][长度 u16 大端][数据]；number 4 字节大端、bool / enum 1 字节（tuya.js:261-269）。写不下 / 类型不认回 0 */
static size_t put_dp(uint8_t *out, size_t cap, uint8_t dp, uint8_t type, int32_t raw)
{
    const size_t n = type == ZH_TUYA_T_NUMBER ? 4u : (type == ZH_TUYA_T_BOOL || type == ZH_TUYA_T_ENUM) ? 1u : 0u;
    if (!n || cap < 4u + n) return 0;
    out[0] = dp;
    out[1] = type;
    out[2] = 0;
    out[3] = (uint8_t)n;
    const uint32_t u = (uint32_t)raw;
    if (n == 4) {
        out[4] = (uint8_t)(u >> 24); out[5] = (uint8_t)(u >> 16); out[6] = (uint8_t)(u >> 8); out[7] = (uint8_t)u;
    } else {
        out[4] = type == ZH_TUYA_T_BOOL ? (uint8_t)(raw ? 1u : 0u) : (uint8_t)u;
    }
    return 4u + n;
}

size_t zh_tuya_enc_ctl(int dev, unsigned act, uint8_t pct, uint16_t tseq, uint8_t *out, size_t cap, uint8_t *cmd, uint8_t *confirm_dp, int32_t *confirm_raw)
{
    const zh_tdb_ctl_t *c = ctl_of(dev);
    if (!c || !out || !cmd || !confirm_dp || !confirm_raw || cap < 2 || pct > 100u || act > ZH_TUYA_ACT_LEVEL) return 0;
    out[0] = (uint8_t)tseq;                        /* seq：ZCL 的 uint16，小端 */
    out[1] = (uint8_t)(tseq >> 8);
    size_t n = 2, k;
    const bool off = act == ZH_TUYA_ACT_OFF || (act == ZH_TUYA_ACT_LEVEL && pct == 0);
    if (act != ZH_TUYA_ACT_LEVEL || off) {
        if (!c->onoff_dp) return 0;
        const int32_t v = off ? c->off_raw : c->on_raw;
        if (!(k = put_dp(out + n, cap - n, c->onoff_dp, c->onoff_type, v))) return 0;
        n += k;
        *confirm_dp = c->onoff_dp;
        *confirm_raw = c->onoff_type == ZH_TUYA_T_BOOL ? (v ? 1 : 0) : v;   /* 回报解成整数后的样子（zh_tuya_dp_int：布尔是 0 / 1） */
    } else {
        if (!c->level_dp) return 0;
        if (c->onoff_dp) {                         /* 先开 */
            if (!(k = put_dp(out + n, cap - n, c->onoff_dp, c->onoff_type, c->on_raw))) return 0;
            n += k;
        }
        const int64_t t = (int64_t)pct * c->level_mul;
        int64_t raw = (t + 5000) / 10000 + c->level_off;   /* pct、mul 都不为负（导出脚本只认递增的） */
        if (raw < c->level_lo) raw = c->level_lo;
        if (raw > c->level_hi) raw = c->level_hi;
        if (!(k = put_dp(out + n, cap - n, c->level_dp, ZH_TUYA_T_NUMBER, (int32_t)raw))) return 0;
        n += k;
        *confirm_dp = c->level_dp;
        *confirm_raw = (int32_t)raw;
    }
    *cmd = c->cmd;
    return n;
}

unsigned zh_tuya_map(int dev, uint8_t dp, int32_t raw, zh_tuya_val_t *out, unsigned max)
{
    if (dev < 0 || (unsigned)dev >= zh_tdb_n_devs || !out) return 0;
    const zh_tdb_dev_t *d = &zh_tdb_devs[dev];
    if ((unsigned)d->row + d->n_rows > zh_tdb_n_rows) return 0;
    unsigned n = 0;
    for (unsigned i = 0; i < d->n_rows && n < max; i++) {
        const zh_tdb_row_t *r = &zh_tdb_rows[d->row + i];
        if (r->dp != dp || r->field == ZH_TF_NONE || r->field >= ZH_TF_COUNT) continue;
        if (!zh_tbl_conv(r->kind, r->mask, r->mul, r->off, raw, &out[n].val)) continue;
        out[n].field = r->field;
        n++;
    }
    return n;
}
