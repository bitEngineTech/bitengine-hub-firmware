/* hub_policy —— 动作表、执行时的三道检查、限速（见 include/hub_policy.h）。纯 C，不依赖 ESP-IDF。 */
#include "hub_policy.h"

#include <string.h>

/* 6.2 的动作表。顺序与 hub_action_t 一致（hub_action_info 按下标取，主机测试钉着）。
 * 敏感性：unlock 永远敏感；lock 永远不敏感（锁上是安全方向）；其余动作在设备被标为敏感时都敏感 —— 只拦开关会被
 * set_brightness（MoveToLevelWithOnOff，0 关、> 0 开）绕过，IAS 警报器"所有动作"也要拦（6.2；2026-10-22 两轮审计）。 */
static const hub_action_info_t k_actions[HUB_ACT_COUNT] = {
    { "turn_on",              HUB_ACT_TURN_ON,              HUB_CAP_ONOFF,      HUB_VAL_NONE,    HUB_SENS_IF_DEVICE },
    { "turn_off",             HUB_ACT_TURN_OFF,             HUB_CAP_ONOFF,      HUB_VAL_NONE,    HUB_SENS_IF_DEVICE },
    { "toggle",               HUB_ACT_TOGGLE,               HUB_CAP_ONOFF,      HUB_VAL_NONE,    HUB_SENS_IF_DEVICE },
    { "set_brightness",       HUB_ACT_SET_BRIGHTNESS,       HUB_CAP_LEVEL,      HUB_VAL_PERCENT, HUB_SENS_IF_DEVICE },
    { "set_color_temp",       HUB_ACT_SET_COLOR_TEMP,       HUB_CAP_COLOR_TEMP, HUB_VAL_KELVIN,  HUB_SENS_IF_DEVICE },
    { "set_color",            HUB_ACT_SET_COLOR,            HUB_CAP_COLOR,      HUB_VAL_RGB,     HUB_SENS_IF_DEVICE },
    { "open",                 HUB_ACT_OPEN,                 HUB_CAP_COVER,      HUB_VAL_NONE,    HUB_SENS_IF_DEVICE },
    { "close",                HUB_ACT_CLOSE,                HUB_CAP_COVER,      HUB_VAL_NONE,    HUB_SENS_IF_DEVICE },
    { "stop",                 HUB_ACT_STOP,                 HUB_CAP_COVER,      HUB_VAL_NONE,    HUB_SENS_IF_DEVICE },
    { "set_position",         HUB_ACT_SET_POSITION,         HUB_CAP_COVER,      HUB_VAL_PERCENT, HUB_SENS_IF_DEVICE },
    { "lock",                 HUB_ACT_LOCK,                 HUB_CAP_LOCK,       HUB_VAL_NONE,    HUB_SENS_NEVER },
    { "unlock",               HUB_ACT_UNLOCK,               HUB_CAP_LOCK,       HUB_VAL_NONE,    HUB_SENS_ALWAYS },
    { "set_heating_setpoint", HUB_ACT_SET_HEATING_SETPOINT, HUB_CAP_THERMOSTAT, HUB_VAL_CELSIUS, HUB_SENS_IF_DEVICE },
    { "identify",             HUB_ACT_IDENTIFY,             HUB_CAP_IDENTIFY,   HUB_VAL_SECONDS, HUB_SENS_IF_DEVICE },
};

const hub_action_info_t *hub_action_lookup(const char *name)
{
    if (!name) return NULL;
    for (unsigned i = 0; i < HUB_ACT_COUNT; i++) {
        if (strcmp(name, k_actions[i].name) == 0) return &k_actions[i];
    }
    return NULL;
}

const hub_action_info_t *hub_action_info(hub_action_t act)
{
    return ((unsigned)act < HUB_ACT_COUNT) ? &k_actions[act] : NULL;
}

/* ---------------- 值解析 ---------------- */

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* 去掉首尾空格后的 [b, e) */
static void trim(const char *s, const char **b, const char **e)
{
    *b = skip_ws(s);
    *e = *b + strlen(*b);
    while (*e > *b && ((*e)[-1] == ' ' || (*e)[-1] == '\t')) (*e)--;
}

/* 十进制整数（可带一个正负号），最多 9 位数字（不溢出 int32） */
static bool parse_int(const char *b, const char *e, int32_t *out)
{
    bool neg = false;
    if (b < e && (*b == '+' || *b == '-')) {
        neg = (*b == '-');
        b++;
    }
    if (b == e || e - b > 9) return false;
    int32_t v = 0;
    for (const char *p = b; p < e; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (*p - '0');
    }
    *out = neg ? -v : v;
    return true;
}

/* 十进制小数 → × 100（最多两位小数，整数部分最多 6 位） */
static bool parse_c100(const char *b, const char *e, int32_t *out)
{
    bool neg = false;
    if (b < e && (*b == '+' || *b == '-')) {
        neg = (*b == '-');
        b++;
    }
    const char *dot = memchr(b, '.', (size_t)(e - b));
    const char *ie = dot ? dot : e;
    if (ie == b || ie - b > 6) return false;            /* 要有整数部分（".5" 不收） */
    int32_t v = 0;
    for (const char *p = b; p < ie; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (*p - '0');
    }
    int32_t frac = 0;
    if (dot) {
        const char *fb = dot + 1;
        if (fb == e || e - fb > 2) return false;          /* "21." 与三位小数都不收 */
        for (const char *p = fb; p < e; p++) {
            if (*p < '0' || *p > '9') return false;
            frac = frac * 10 + (*p - '0');
        }
        if (e - fb == 1) frac *= 10;
    }
    v = v * 100 + frac;
    *out = neg ? -v : v;
    return true;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

hub_val_verdict_t hub_val_parse(hub_val_kind_t kind, const char *value, int32_t *out)
{
    if (!out) return HUB_VAL_SYNTAX;
    const char *b = NULL, *e = NULL;
    const bool given = value != NULL && (trim(value, &b, &e), b != e);
    int32_t v = 0;
    switch (kind) {
    case HUB_VAL_NONE:
        *out = 0;
        return HUB_VAL_OK;                               /* 带了也忽略（附录 A：只对要值的动作给 value） */
    case HUB_VAL_SECONDS:
        if (!given) {
            *out = 10;                                   /* identify 默认 10 s（6.2） */
            return HUB_VAL_OK;
        }
        if (!parse_int(b, e, &v)) return HUB_VAL_SYNTAX;
        if (v < 1 || v > 60) return HUB_VAL_RANGE;
        break;
    case HUB_VAL_PERCENT:
        if (!given) return HUB_VAL_MISSING;
        if (!parse_int(b, e, &v)) return HUB_VAL_SYNTAX;
        if (v < 0 || v > 100) return HUB_VAL_RANGE;
        break;
    case HUB_VAL_KELVIN:
        if (!given) return HUB_VAL_MISSING;
        if (!parse_int(b, e, &v)) return HUB_VAL_SYNTAX;
        if (v < 1500 || v > 9000) return HUB_VAL_RANGE;
        break;
    case HUB_VAL_RGB:
        if (!given) return HUB_VAL_MISSING;
        if (e - b != 7 || *b != '#') return HUB_VAL_SYNTAX;
        for (int i = 1; i < 7; i++) {
            const int h = hexval(b[i]);
            if (h < 0) return HUB_VAL_SYNTAX;
            v = (v << 4) | h;
        }
        break;
    case HUB_VAL_CELSIUS:
        if (!given) return HUB_VAL_MISSING;
        if (!parse_c100(b, e, &v)) return HUB_VAL_SYNTAX;
        if (v < 500 || v > 3000) return HUB_VAL_RANGE;
        break;
    default:
        return HUB_VAL_SYNTAX;
    }
    *out = v;
    return HUB_VAL_OK;
}

/* ---------------- 三道检查 ---------------- */

hub_chk_t hub_policy_check_cmd(const hub_cmd_t *c, uint8_t perm_bits, hub_src_t src)
{
    if (!c || (unsigned)src >= HUB_SRC_COUNT) return HUB_CHK_BAD_ARG;
    if (src == HUB_SRC_REST) return HUB_CHK_OK;          /* 本地管理员不受 Muse 权限位约束（6.3） */
    switch (c->perm) {
    case HUB_PERM_READ:    return HUB_CHK_OK;
    case HUB_PERM_CONTROL: return (perm_bits & HUB_PERMBIT_CONTROL) ? HUB_CHK_OK : HUB_CHK_NO_PERM;
    case HUB_PERM_MANAGE:  return (perm_bits & HUB_PERMBIT_MANAGE) ? HUB_CHK_OK : HUB_CHK_NO_PERM;
    default:               return HUB_CHK_NO_PERM;       /* 不认识的权限级别：拒 */
    }
}

bool hub_action_is_sensitive(const hub_action_info_t *a, bool device_sensitive)
{
    if (!a) return true;                                 /* 宁可多拦 */
    switch (a->sens) {
    case HUB_SENS_NEVER:     return false;
    case HUB_SENS_IF_DEVICE: return device_sensitive;
    case HUB_SENS_ALWAYS:    return true;
    default:                 return true;
    }
}

hub_chk_t hub_policy_check_action(const hub_action_info_t *a, uint8_t perm_bits, hub_src_t src,
                                  bool device_sensitive, uint32_t device_caps)
{
    if (!a || (unsigned)src >= HUB_SRC_COUNT) return HUB_CHK_BAD_ARG;
    if (src == HUB_SRC_MUSE) {
        if (!(perm_bits & HUB_PERMBIT_CONTROL)) return HUB_CHK_NO_PERM;
        if (hub_action_is_sensitive(a, device_sensitive) && !(perm_bits & HUB_PERMBIT_SENSITIVE)) return HUB_CHK_SENSITIVE;
    }
    if ((device_caps & a->cap) != a->cap) return HUB_CHK_NO_CAP;
    return HUB_CHK_OK;
}

const char *hub_chk_str(hub_chk_t c)
{
    switch (c) {
    case HUB_CHK_OK:        return "ok";
    case HUB_CHK_NO_PERM:   return "no_perm";
    case HUB_CHK_SENSITIVE: return "sensitive";
    case HUB_CHK_NO_CAP:    return "no_capability";
    case HUB_CHK_BAD_ARG:   return "bad_arg";
    }
    return "unknown";
}

/* ---------------- 限速 ---------------- */

/* 6.5 的表：容量、每补一个令牌的毫秒数 */
static const hub_rl_spec_t k_class_spec[HUB_RL_CLASS_COUNT] = {
    [HUB_RL_READ]        = { 5,  500 },     /* 每秒 2 次，容量 5 */
    [HUB_RL_CONTROL]     = { 10, 200 },     /* 每秒 5 次，容量 10 */
    [HUB_RL_UPDATE]      = { 10, 6000 },    /* 每分钟 10 次 */
    [HUB_RL_PERMIT_JOIN] = { 1,  60000 },   /* 每 60 s 1 次 */
    [HUB_RL_COMMISSION]  = { 1,  60000 },   /* 每 60 s 1 次 */
};
static const hub_rl_spec_t k_total_spec = { 10, 100 };   /* 全部调用合计每秒 10 次 */

hub_rl_spec_t hub_rl_class_spec(hub_rl_class_t c)
{
    if ((unsigned)c >= HUB_RL_CLASS_COUNT) return (hub_rl_spec_t){ 0, 0 };
    return k_class_spec[c];
}

hub_rl_spec_t hub_rl_total_spec(void)
{
    return k_total_spec;
}

static void bucket_fill(hub_bucket_t *b, hub_rl_spec_t spec, uint64_t now_ms)
{
    b->tokens = spec.capacity;
    b->ref_ms = now_ms;
}

/* 按整周期补令牌，余下不足一个周期的时间留在 ref_ms 里（频繁调用也不会把零头吃掉）；满了参考点跟着当前时刻走 */
static void bucket_refill(hub_bucket_t *b, hub_rl_spec_t spec, uint64_t now_ms)
{
    if (now_ms < b->ref_ms) return;                      /* 时间倒退（esp_timer 单调，不该发生）：不补、参考点也不动，时间回来时不多给 */
    if (b->tokens >= spec.capacity) {
        b->tokens = spec.capacity;
        b->ref_ms = now_ms;
        return;
    }
    const uint64_t n = (now_ms - b->ref_ms) / spec.period_ms;
    if (n == 0) return;
    if (n >= (uint64_t)(spec.capacity - b->tokens)) {
        b->tokens = spec.capacity;
        b->ref_ms = now_ms;
    } else {
        b->tokens = (uint16_t)(b->tokens + n);
        b->ref_ms += n * spec.period_ms;
    }
}

void hub_rl_init(hub_rl_t *rl, uint64_t now_ms)
{
    if (!rl) return;
    for (unsigned i = 0; i < HUB_RL_CLASS_COUNT; i++) bucket_fill(&rl->cls[i], k_class_spec[i], now_ms);
    bucket_fill(&rl->total, k_total_spec, now_ms);
}

void hub_rl_refund(hub_rl_t *rl, hub_rl_class_t c, uint64_t now_ms)
{
    if (!rl || (unsigned)c >= HUB_RL_CLASS_COUNT) return;
    hub_bucket_t *cb = &rl->cls[c];
    bucket_refill(cb, k_class_spec[c], now_ms);           /* 先补到现在：别让退还和补令牌叠出超过容量的数 */
    bucket_refill(&rl->total, k_total_spec, now_ms);
    if (cb->tokens < k_class_spec[c].capacity) cb->tokens++;
    if (rl->total.tokens < k_total_spec.capacity) rl->total.tokens++;
}

bool hub_rl_take(hub_rl_t *rl, hub_rl_class_t c, uint64_t now_ms)
{
    if (!rl || (unsigned)c >= HUB_RL_CLASS_COUNT) return false;
    hub_bucket_t *cb = &rl->cls[c];
    bucket_refill(cb, k_class_spec[c], now_ms);
    bucket_refill(&rl->total, k_total_spec, now_ms);
    if (cb->tokens == 0 || rl->total.tokens == 0) return false;   /* 被拒不扣 */
    cb->tokens--;
    rl->total.tokens--;
    return true;
}
