/* mc_sub_core —— 订阅的纯逻辑（见 mc_sub_core.h）。换算照 Zigbee 那一边（zh_dev.c），同一个读数两边给同一个数。 */
#include "mc_sub_core.h"

#include <math.h>
#include <string.h>

/* 簇 / 属性（Matter 1.4 应用簇规范；H1 记在 matter_ctrl_live.h 的字段表里） */
enum {
    CL_ONOFF = 0x0006, CL_LEVEL = 0x0008, CL_BOOLEAN_STATE = 0x0045, CL_POWER_SOURCE = 0x002F, CL_SMOKE_CO = 0x005C,
    CL_EPM = 0x0090, CL_EEM = 0x0091, CL_ILLUMINANCE = 0x0400, CL_TEMPERATURE = 0x0402, CL_PRESSURE = 0x0403,
    CL_HUMIDITY = 0x0405, CL_OCCUPANCY = 0x0406, CL_BASIC = 0x0028,
};
enum { DT_CONTACT = 0x0015, DT_WATER_LEAK = 0x0043 };

static bool has_cl(const mc_iv_ep_t *e, uint16_t cl)
{
    for (unsigned k = 0; k < e->n_cl && k < MC_IV_CL_MAX; k++) if (e->cl[k] == cl) return true;
    return false;
}

static bool has_dt(const mc_iv_ep_t *e, uint32_t dt)
{
    for (unsigned k = 0; k < e->n_dt && k < MC_IV_DT_MAX; k++) if (e->dt[k] == dt) return true;
    return false;
}

static int add(mc_path_t *out, int n, int max, uint16_t ep, uint16_t cl, uint16_t at, uint8_t field)
{
    if (n >= max) return n;
    out[n].ep = ep;
    out[n].cl = cl;
    out[n].at = at;
    out[n].field = field;
    return n + 1;
}

bool mc_sub_paths_eq(const mc_path_t *a, const mc_path_t *b, int n)
{
    if (!a || !b) return false;
    for (int i = 0; i < n; i++)
        if (a[i].ep != b[i].ep || a[i].cl != b[i].cl || a[i].at != b[i].at || a[i].field != b[i].field) return false;
    return true;
}

/* 第一个有这个簇的非根端点（BooleanState 还要设备类型对得上）；没有 -1 */
static int first_ep(const mc_iv_t *iv, uint16_t cl, uint32_t need_dt)
{
    for (unsigned i = 0; i < iv->n_ep && i < MC_IV_EP_MAX; i++) {
        const mc_iv_ep_t *e = &iv->ep[i];
        if (e->ep == 0 || !has_cl(e, cl)) continue;
        if (need_dt && !has_dt(e, need_dt)) continue;
        return (int)e->ep;
    }
    return -1;
}

int mc_sub_first_ep(const mc_iv_t *iv, uint16_t cl) { return iv ? first_ep(iv, cl, 0) : -1; }

int mc_sub_paths(const mc_iv_t *iv, mc_path_t *out, int max)
{
    if (!iv || !out || max <= 0) return 0;
    memset(out, 0, sizeof(*out) * (size_t)max);
    static const struct { uint16_t cl, at; uint32_t dt; uint8_t field; } k_order[] = {
        { CL_ONOFF, 0x0000, 0, MC_ST_ONOFF },
        { CL_LEVEL, 0x0000, 0, MC_ST_LEVEL },
        { CL_BOOLEAN_STATE, 0x0000, DT_CONTACT, MC_ST_CONTACT },
        { CL_BOOLEAN_STATE, 0x0000, DT_WATER_LEAK, MC_ST_LEAK },
        { CL_SMOKE_CO, 0x0001, 0, MC_ST_SMOKE },
        { CL_OCCUPANCY, 0x0000, 0, MC_ST_MOTION },
        { CL_TEMPERATURE, 0x0000, 0, MC_ST_TEMP },
        { CL_HUMIDITY, 0x0000, 0, MC_ST_HUM },
        { CL_PRESSURE, 0x0000, 0, MC_ST_PRESS },
        { CL_ILLUMINANCE, 0x0000, 0, MC_ST_LUX },
        { CL_EPM, 0x0008, 0, MC_ST_POWER },
    };
    int n = 0;
    for (unsigned k = 0; k < sizeof(k_order) / sizeof(k_order[0]); k++) {
        const int ep = first_ep(iv, k_order[k].cl, k_order[k].dt);
        if (ep >= 0) n = add(out, n, max, (uint16_t)ep, k_order[k].cl, k_order[k].at, k_order[k].field);
    }
    if (iv->flags & MC_IV_F_BATTERY) {
        /* 访谈只记了"有电池"、没记是哪个 PowerSource：每个都订（一般一两个），哪个报出电量就用哪个；根端点也认 */
        for (unsigned i = 0; i < iv->n_ep && i < MC_IV_EP_MAX; i++)
            if (has_cl(&iv->ep[i], CL_POWER_SOURCE)) n = add(out, n, max, iv->ep[i].ep, CL_POWER_SOURCE, 0x000C, MC_ST_BATTERY);
    }
    const int eem = first_ep(iv, CL_EEM, 0);
    if (eem >= 0) n = add(out, n, max, (uint16_t)eem, CL_EEM, 0x0001, MC_ST_ENERGY);
    if (n == 0) n = add(out, n, max, 0, CL_BASIC, 0x0005, MC_FIELD_NONE);   /* 只为知道它在不在 */
    return n;
}

static bool as_bool(const mc_val_t *v, bool *out)
{
    if (v->t != MC_V_BOOL || v->u > 1) return false;
    *out = v->u != 0;
    return true;
}

static bool as_uint(const mc_val_t *v, uint64_t max, uint64_t *out)
{
    if (v->t != MC_V_UINT || v->u > max) return false;
    *out = v->u;
    return true;
}

static bool as_int(const mc_val_t *v, int64_t lo, int64_t hi, int64_t *out)
{
    int64_t x;
    if (v->t == MC_V_INT) x = v->i;
    else if (v->t == MC_V_UINT && v->u <= (uint64_t)INT64_MAX) x = (int64_t)v->u;   /* 正数可能按无符号编码 */
    else return false;
    if (x < lo || x > hi) return false;
    *out = x;
    return true;
}

bool mc_sub_apply(mc_state_t *st, uint8_t field, const mc_val_t *v, int64_t now_ms, unsigned *sev)
{
    unsigned ev = 0;
    if (sev) *sev = 0;
    if (!st || !v || field >= MC_ST_COUNT) return false;
    const uint32_t bit = 1u << field;
    const bool had = (st->have & bit) != 0;
    if (v->t == MC_V_NULL) {                       /* 设备说"现在不知道"：这一项不报（别留着旧值冒充新的） */
        st->have &= ~bit;
        return true;
    }
    bool b;
    uint64_t u;
    int64_t i;
    switch (field) {
    case MC_ST_ONOFF:
        if (!as_bool(v, &b)) return false;
        st->onoff = b;
        break;
    case MC_ST_LEVEL: {
        if (!as_uint(v, 254, &u)) return false;
        const uint32_t pct = (uint32_t)((u * 100u + 127u) / 254u);
        st->level_pct = (uint8_t)(u && pct == 0 ? 1u : pct);      /* 同 Zigbee：亮着就不是 0 % */
        break;
    }
    case MC_ST_CONTACT: {
        if (!as_bool(v, &b)) return false;
        const uint8_t open = b ? 0 : 1;              /* Contact Sensor 的 StateValue：true = 合上 */
        if (open != (had ? st->contact_open : 0)) ev |= MC_SEV_CONTACT;   /* 没收到过按合上算（同 Zigbee IAS 的"之前是 0"） */
        st->contact_open = open;
        break;
    }
    case MC_ST_LEAK:
        if (!as_bool(v, &b)) return false;
        if ((uint8_t)b != (had ? st->leak : 0)) ev |= MC_SEV_LEAK;
        st->leak = b;
        break;
    case MC_ST_SMOKE: {
        if (!as_uint(v, 2, &u)) return false;        /* SmokeState：0 正常 / 1 警告 / 2 严重 */
        const uint8_t on = u != 0;
        if (on != (had ? st->smoke : 0)) ev |= MC_SEV_SMOKE;
        st->smoke = on;
        break;
    }
    case MC_ST_MOTION: {
        if (!as_uint(v, 0xFF, &u)) return false;     /* Occupancy 位图：bit0 有人 */
        const uint8_t on = (u & 1u) != 0;
        if (on && !(had ? st->motion : 0)) ev |= MC_SEV_MOTION_ON;   /* 每次没人 → 有人都出（不限间隔：网关不做人为干预，2026-10-24） */
        st->motion = on;
        break;
    }
    case MC_ST_BATTERY: {
        if (!as_uint(v, 200, &u)) return false;      /* 单位 0.5 % */
        const uint8_t pct = (uint8_t)(u / 2u);
        if (pct >= MC_BATT_REARM_PCT) st->batt_low = 0;
        else if (pct <= MC_BATT_LOW_PCT && !st->batt_low) {   /* 降到门槛报一次，回到 20 % 及以上才重新算（同 Zigbee；门槛附近抖不算翻转） */
            st->batt_low = 1;
            ev |= MC_SEV_BATT_LOW;
        }
        st->battery_pct = pct;
        break;
    }
    case MC_ST_TEMP:
        if (!as_int(v, -27315, 32767, &i)) return false;
        st->temp_c100 = (int16_t)i;
        break;
    case MC_ST_HUM:
        if (!as_uint(v, 10000, &u)) return false;
        st->hum_c100 = (uint16_t)u;
        break;
    case MC_ST_PRESS:
        if (!as_int(v, -32767, 32767, &i)) return false;      /* 0.1 kPa = 1 hPa */
        if (i * 10 < -32767 || i * 10 > 32767) return false;
        st->press_hpa10 = (int16_t)(i * 10);
        break;
    case MC_ST_LUX:
        if (!as_uint(v, 0xFFFE, &u)) return false;
        st->lux = u == 0 ? 0u : (uint32_t)lround(pow(10.0, ((double)u - 1.0) / 10000.0));   /* 同 Zigbee：0 = 太暗量不出 */
        break;
    case MC_ST_POWER:
        if (!as_int(v, -4611686018427387904LL, 4611686018427387904LL, &i)) return false;   /* int64 mW（规范范围 ±2^62） */
        st->power_mw = i;
        break;
    case MC_ST_ENERGY:
        if (!as_int(v, 0, 4611686018427387904LL, &i)) return false;   /* 累计 mWh，只增不减 */
        st->energy_mwh = i;
        break;
    default:
        return false;
    }
    st->have |= bit;
    st->at_ms[field] = now_ms;
    if (sev) *sev = ev;
    return true;
}

int8_t mc_sub_judge(int8_t cur, bool up, int64_t ref_ms, uint16_t max_iv_s, int64_t now_ms)
{
    if (up) return 1;
    const int64_t win = 2LL * (max_iv_s ? max_iv_s : MC_SUB_MAX_IV_S) * 1000LL;
    if (now_ms - ref_ms > (win < 60000 ? 60000 : win)) return 0;   /* 至少 60 s：最大间隔很短的设备别一抖就离线 */
    return cur;
}

uint32_t mc_sub_backoff_ms(uint32_t tries)
{
    return tries <= 1 ? 5000u : tries == 2 ? 30000u : 120000u;
}

uint8_t mc_ctl_level(uint8_t pct)
{
    if (pct == 0) return 0;
    if (pct >= 100) return 254;
    const uint32_t l = (pct * 254u + 50u) / 100u;
    return (uint8_t)(l < 1 ? 1 : l);
}

static bool level_near(uint8_t have, uint8_t want) { return have + 1 >= want && have <= want + 1; }

bool mc_ctl_reached(const mc_state_t *st, int act, int8_t want_on, uint8_t want_pct, int64_t sent_ms)
{
    if (!st) return false;
    const bool on_new = (st->have & (1u << MC_ST_ONOFF)) && st->at_ms[MC_ST_ONOFF] >= sent_ms;
    switch (act) {
    case MATTER_CTRL_ACT_ON:     return on_new && st->onoff;
    case MATTER_CTRL_ACT_OFF:    return on_new && !st->onoff;
    case MATTER_CTRL_ACT_TOGGLE: return on_new && (want_on < 0 || st->onoff == (uint8_t)want_on);
    case MATTER_CTRL_ACT_LEVEL:
        if (want_pct == 0) return on_new && !st->onoff;                 /* 0 % 发的是 Off */
        if (!(st->have & (1u << MC_ST_LEVEL)) || !level_near(st->level_pct, want_pct)) return false;
        /* 亮度报过新值；或者灯原来关着、亮度本来就是目标 —— MoveToLevelWithOnOff 只翻了开关，亮度没变化就不报（审计） */
        return st->at_ms[MC_ST_LEVEL] >= sent_ms || (on_new && st->onoff);
    default:                     return false;
    }
}

bool mc_ctl_already(const mc_state_t *st, int act, uint8_t want_pct)
{
    if (!st || !(st->have & (1u << MC_ST_ONOFF))) return false;
    switch (act) {
    case MATTER_CTRL_ACT_ON:  return st->onoff;
    case MATTER_CTRL_ACT_OFF: return !st->onoff;
    case MATTER_CTRL_ACT_LEVEL:
        if (want_pct == 0) return !st->onoff;
        return st->onoff && (st->have & (1u << MC_ST_LEVEL)) && level_near(st->level_pct, want_pct);
    default:                  return false;
    }
}

uint32_t mc_sub_jitter(uint32_t ms, uint32_t rnd)
{
    return (uint32_t)(((uint64_t)ms * (80u + rnd % 41u)) / 100u);
}
