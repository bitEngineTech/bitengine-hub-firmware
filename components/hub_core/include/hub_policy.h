#pragma once
/* hub_policy —— 动作表、执行时的三道检查、限速（docs/15_muse_direct_design_v1.md 6.2、6.5）。纯 C，不依赖 ESP-IDF。
 *
 * 注册时已经按权限位过滤过（hub_registry），这里是执行时的纵深防御（6.5）：
 *   1. 命令的权限位开着（READ 永远有）；
 *   2. hub.device.control 的动作落在敏感集合里时要 SENSITIVE 位：unlock 永远敏感，lock 永远不敏感（锁上是安全方向），
 *      其余动作在设备被标为敏感时都敏感（只拦开关会被 set_brightness 绕过；IAS 警报器的所有动作都要拦）；
 *   3. 设备有这个动作要的能力。
 * 本地 REST 的管理员不受 Muse 权限位约束（1、2 跳过），但能力、参数校验、限速、审计照常（6.3）。
 *
 * 限速（6.5）按来源（muse / rest）各一套令牌桶：每类命令一个桶，外加"全部调用合计"一个桶；两个桶都有令牌才放行，
 * 只扣放行的那次（被拒不扣）。不是线程安全的：调用方（作业层）持锁调用。时间用单调毫秒（esp_timer），跨重启不保留。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hub_model.h"
#include "hub_registry.h"

typedef enum { HUB_SRC_MUSE = 0, HUB_SRC_REST = 1, HUB_SRC_COUNT } hub_src_t;

typedef enum {
    HUB_ACT_TURN_ON, HUB_ACT_TURN_OFF, HUB_ACT_TOGGLE,
    HUB_ACT_SET_BRIGHTNESS, HUB_ACT_SET_COLOR_TEMP, HUB_ACT_SET_COLOR,
    HUB_ACT_OPEN, HUB_ACT_CLOSE, HUB_ACT_STOP, HUB_ACT_SET_POSITION,
    HUB_ACT_LOCK, HUB_ACT_UNLOCK,
    HUB_ACT_SET_HEATING_SETPOINT, HUB_ACT_IDENTIFY,
    HUB_ACT_COUNT
} hub_action_t;

typedef enum {
    HUB_VAL_NONE,        /* 不带值（带了也忽略） */
    HUB_VAL_PERCENT,     /* 0–100 整数 */
    HUB_VAL_KELVIN,      /* 1500–9000 整数；按设备范围钳位在后端做 */
    HUB_VAL_RGB,         /* "#RRGGBB" → 0xRRGGBB */
    HUB_VAL_CELSIUS,     /* 5.0–30.0，最多两位小数 → 摄氏度 × 100 */
    HUB_VAL_SECONDS,     /* 1–60 整数，不带值默认 10 */
} hub_val_kind_t;

typedef enum { HUB_SENS_NEVER = 0, HUB_SENS_IF_DEVICE = 1, HUB_SENS_ALWAYS = 2 } hub_sens_t;

typedef struct {
    const char     *name;     /* 附录 A hub.device.control 说明里的动作名 */
    hub_action_t    act;
    uint32_t        cap;      /* 需要的能力位 */
    hub_val_kind_t  val;
    hub_sens_t      sens;
} hub_action_info_t;

const hub_action_info_t *hub_action_lookup(const char *name);     /* 不认识返回 NULL */
const hub_action_info_t *hub_action_info(hub_action_t act);

typedef enum {
    HUB_VAL_OK = 0,
    HUB_VAL_MISSING,     /* 动作要值，没给 */
    HUB_VAL_SYNTAX,      /* 不是要的格式 */
    HUB_VAL_RANGE,       /* 格式对，超出范围 */
} hub_val_verdict_t;

/* value 可为 NULL（= 没给）。成功时 *out：百分数 / 开尔文 / 秒为整数，RGB 为 0xRRGGBB，摄氏度为 × 100。
   允许首尾空格；不接受正负号以外的多余字符、指数、十六进制数字。 */
hub_val_verdict_t hub_val_parse(hub_val_kind_t kind, const char *value, int32_t *out);

typedef enum {
    HUB_CHK_OK = 0,
    HUB_CHK_NO_PERM,      /* 命令的权限位关着 */
    HUB_CHK_SENSITIVE,    /* 敏感动作，SENSITIVE 位关着 */
    HUB_CHK_NO_CAP,       /* 设备没有这个动作要的能力 */
    HUB_CHK_BAD_ARG,
} hub_chk_t;

hub_chk_t hub_policy_check_cmd(const hub_cmd_t *c, uint8_t perm_bits, hub_src_t src);
bool hub_action_is_sensitive(const hub_action_info_t *a, bool device_sensitive);
hub_chk_t hub_policy_check_action(const hub_action_info_t *a, uint8_t perm_bits, hub_src_t src,
                                  bool device_sensitive, uint32_t device_caps);
const char *hub_chk_str(hub_chk_t c);

/* ---------------- 限速 ---------------- */

typedef struct {
    uint16_t tokens;
    uint64_t ref_ms;      /* 上次补到的时刻（补满时跟着当前时刻走） */
} hub_bucket_t;

typedef struct {
    hub_bucket_t cls[HUB_RL_CLASS_COUNT];
    hub_bucket_t total;
} hub_rl_t;

/* 每个桶的容量与"每补一个令牌要多少毫秒"（6.5 的表） */
typedef struct { uint16_t capacity; uint32_t period_ms; } hub_rl_spec_t;
hub_rl_spec_t hub_rl_class_spec(hub_rl_class_t c);
hub_rl_spec_t hub_rl_total_spec(void);

void hub_rl_init(hub_rl_t *rl, uint64_t now_ms);                  /* 所有桶装满 */
bool hub_rl_take(hub_rl_t *rl, hub_rl_class_t c, uint64_t now_ms); /* 放行返回 true 并扣令牌 */
/* 把刚扣的令牌还回去（类别桶与总桶各一个，不超过容量）：处理函数当场拒了、没真去做（CHECKLIST MX21：配网码打错一位不该罚一分钟）。
   只在 hub_rl_take 放行之后、同一次调用里用 */
void hub_rl_refund(hub_rl_t *rl, hub_rl_class_t c, uint64_t now_ms);
