/* zh_lumi —— 小米 / Aqara 设备的读数（规则见 include/zh_lumi.h）。 */
#include "zh_lumi.h"

#include <string.h>

#include "zh_zcl.h"

void zh_lumi_tlv_init(zh_lumi_tlv_t *it, const uint8_t *p, size_t n)
{
    if (!it) return;
    it->p = p;
    it->n = p ? n : 0;
    it->off = 0;
}

bool zh_lumi_val_int(uint8_t type, const uint8_t *v, size_t len, int32_t *out)
{
    if (!v || !out) return false;
    size_t w;
    bool sign = false;
    if (type == 0x10 || type == 0x18 || type == 0x20 || type == 0x30) w = 1;   /* 布尔 / 位图 8 / 无符号 8 / 枚举 8 */
    else if (type == 0x19 || type == 0x21 || type == 0x31) w = 2;
    else if (type == 0x1A || type == 0x22) w = 3;
    else if (type == 0x1B || type == 0x23) w = 4;
    else if (type >= 0x28 && type <= 0x2B) { w = (size_t)(type - 0x27); sign = true; }   /* 有符号 8–32 */
    else return false;
    if (len < w) return false;
    uint32_t u = 0;
    for (size_t i = 0; i < w; i++) u |= (uint32_t)v[i] << (8 * i);   /* 小端 */
    if (sign && w < 4 && (u & (1u << (8 * w - 1)))) u |= ~0u << (8 * w);   /* 符号扩展 */
    if (!sign && w == 4 && u > INT32_MAX) return false;                  /* 装不进 int32 的无符号 32 位：不取 */
    *out = (int32_t)u;
    return true;
}

int zh_lumi_tlv_next(zh_lumi_tlv_t *it, uint8_t *tag, int32_t *val, bool *numeric)
{
    if (!it || !tag || !val || !numeric) return -1;
    if (it->off + 2 > it->n) return 0;              /* 不到 2 字节：完了（readMiStruct 跳过末尾那一个字节） */
    const uint8_t t = it->p[it->off], type = it->p[it->off + 1];
    size_t len;
    if (zh_zcl_type_len(type, it->p + it->off + 2, it->n - it->off - 2, &len) != ZH_ZCL_OK) {
        it->off = it->n;
        return -1;
    }
    *tag = t;
    *numeric = zh_lumi_val_int(type, it->p + it->off + 2, len, val);
    it->off += 2 + len;
    return 1;
}

static const char *pool_str(uint16_t off)
{
    return off < zh_ldb_str_len ? zh_ldb_str + off : "";
}

int zh_lumi_find(const char *model)
{
    if (!model || !model[0]) return -1;
    unsigned lo = 0, hi = zh_ldb_n_fps;
    while (lo < hi) {
        const unsigned mid = lo + (hi - lo) / 2u;
        const int c = strcmp(pool_str(zh_ldb_fps[mid].model), model);
        if (c == 0) return zh_ldb_fps[mid].dev < zh_ldb_n_devs ? (int)zh_ldb_fps[mid].dev : -1;
        if (c < 0) lo = mid + 1u;
        else hi = mid;
    }
    return -1;
}

static const zh_ldb_dev_t *dev_of(int dev)
{
    if (dev < 0 || (unsigned)dev >= zh_ldb_n_devs) return NULL;
    const zh_ldb_dev_t *d = &zh_ldb_devs[dev];
    return (unsigned)d->row + d->n_rows <= zh_ldb_n_rows ? d : NULL;
}

uint32_t zh_lumi_fields(int dev)
{
    const zh_ldb_dev_t *d = dev_of(dev);
    return d ? d->fields : 0u;
}

uint16_t zh_lumi_occ_s(int dev)
{
    const zh_ldb_dev_t *d = dev_of(dev);
    return d ? d->occ_s : 0u;
}

bool zh_lumi_has_cl(int dev, uint16_t cl)
{
    const zh_ldb_dev_t *d = dev_of(dev);
    if (!d || cl == ZH_LDB_CL_TLV) return false;
    for (unsigned i = 0; i < d->n_rows; i++)
        if (zh_ldb_rows[d->row + i].cl == cl) return true;
    return false;
}

unsigned zh_lumi_map(int dev, uint16_t cl, uint16_t id, int32_t raw, zh_tbl_val_t *out, unsigned max, bool *dropped)
{
    if (dropped) *dropped = false;
    const zh_ldb_dev_t *d = dev_of(dev);
    if (!d || !out) return 0;
    unsigned n = 0;
    for (unsigned i = 0; i < d->n_rows && n < max; i++) {
        const zh_ldb_row_t *r = &zh_ldb_rows[d->row + i];
        if (r->cl != cl || r->id != id) continue;
        if (r->field == ZH_TF_NONE) {               /* 库对这一路什么都不出：丢掉 */
            if (dropped) *dropped = true;
            continue;
        }
        if (r->field >= ZH_TF_COUNT || !zh_tbl_conv(r->kind, r->mask, r->mul, r->off, raw, &out[n].val)) continue;
        out[n].field = r->field;
        n++;
    }
    return n;
}

static int round_half_up(double x) { return x >= 0 ? (int)(x + 0.5) : -(int)(-x + 0.5); }

int zh_lumi_batt_curve(uint8_t type, int32_t min, int32_t max, int32_t off, int32_t mv)
{
    switch (type) {
    case ZH_LDB_BATT_LINEAR: {                      /* toPercentage(mV + vOffset, min, max)：先钳位，再 Math.round（utils.js:272-280、359-363） */
        if (max <= min) return -1;
        double v = (double)mv + off;
        if (v > max) v = max;
        if (v < min) v = min;
        return round_half_up((v - min) / (double)(max - min) * 100.0);
    }
    case ZH_LDB_BATT_3V_2100: {                     /* utils.js:336-354 */
        double p = 100;
        if (mv < 2100) p = 0;
        else if (mv < 2440) p = 6 - ((2440 - mv) * 6) / 340.0;
        else if (mv < 2740) p = 18 - ((2740 - mv) * 12) / 300.0;
        else if (mv < 2900) p = 42 - ((2900 - mv) * 24) / 160.0;
        else if (mv < 3000) p = 100 - ((3000 - mv) * 58) / 100.0;
        return round_half_up(p);
    }
    case ZH_LDB_BATT_3V_1500_2800: {                /* utils.js:355-358；坏帧的 mV ≤ 0 别除零 / 溢出（审计 ⚠️） */
        if (mv <= 0) return 0;
        const int p = round_half_up(235 - 370000.0 / ((double)mv + 1.0));
        return p < 0 ? 0 : p > 100 ? 100 : p;
    }
    default:
        return -1;
    }
}

int zh_lumi_batt_pct(int dev, int32_t mv)
{
    const zh_ldb_dev_t *d = dev_of(dev);
    return d ? zh_lumi_batt_curve(d->batt, d->batt_min, d->batt_max, d->batt_off, mv) : -1;
}
