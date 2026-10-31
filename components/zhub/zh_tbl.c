/* zh_tbl —— 社区设备库导出的表共用的换算（规则见 include/zh_tbl.h）。 */
#include "zh_tbl.h"

#include "zh_zcl.h"

bool zh_tbl_conv(uint8_t kind, uint8_t mask, int32_t mul, int32_t off, int32_t raw, int32_t *out)
{
    if (!out) return false;
    if (kind == ZH_TDB_ANY) {
        *out = 1;
        return true;
    }
    if (kind == ZH_TDB_MASK) {
        *out = raw >= 0 && raw <= 7 ? (mask >> raw) & 1 : 0;
        return true;
    }
    if (kind != ZH_TDB_LIN && kind != ZH_TDB_LIN16) return false;
    if (kind == ZH_TDB_LIN16 && raw >= 0x8000 && raw <= 0xFFFF) raw -= 0x10000;
    const int64_t t = (int64_t)raw * mul;
    int64_t q = t >= 0 ? (t + 5000) / 10000 : -((-t + 5000) / 10000);   /* 四舍五入（远离 0） */
    q += off;
    if (q > INT32_MAX) q = INT32_MAX;
    if (q < INT32_MIN) q = INT32_MIN;
    *out = (int32_t)q;
    return true;
}

unsigned zh_tbl_alarm_field(uint32_t fields)
{
    static const uint8_t k_prio[] = { ZH_TF_SMOKE, ZH_TF_CO, ZH_TF_GAS, ZH_TF_WATER, ZH_TF_CONTACT, ZH_TF_VIBRATION };
    for (unsigned i = 0; i < sizeof(k_prio); i++)
        if (fields & (1u << k_prio[i])) return k_prio[i];
    return ZH_TF_NONE;
}

uint16_t zh_tbl_zone_type(uint32_t fields)
{
    switch (zh_tbl_alarm_field(fields)) {
    case ZH_TF_SMOKE:     return ZH_ZONE_FIRE;
    case ZH_TF_CO:        return ZH_ZONE_CO;
    case ZH_TF_GAS:       return ZH_ZONE_GAS;
    case ZH_TF_WATER:     return ZH_ZONE_WATER;
    case ZH_TF_CONTACT:   return ZH_ZONE_CONTACT;
    case ZH_TF_VIBRATION: return ZH_ZONE_VIBRATION;
    default:              return 0xFFFFu;
    }
}
