#pragma once
/* hub_params —— 命令参数按注册表校验（docs/15_muse_direct_design_v1.md 4.8 第 2 条、6.3）。纯 C，不依赖 ESP-IDF：test_host/ 原样编。
 * Muse 的调用（muse_link）和本地 REST 的 /api/v1/hub/invoke（MD-9 一张表两个出口）共用这一份：必填齐、类型对（string / boolean /
 * integer：整数要在 int32 里、不能带小数），可选的给了也要类型对；范围与枚举由各处理函数自己判（它知道语义）。
 * 原在 muse_proto.c（mp_check_params），M3 第七步挪到这里。 */
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "hub_registry.h"

/* 合格返回 true；不合格 false 并把给模型 / 管理员看的英文原因写进 err（"missing required parameter 'id'" 之类） */
bool hub_params_check(const hub_cmd_t *c, const cJSON *params, char *err, size_t err_cap);

/* hub.zigbee.permit_join 的 seconds：0 = 关窗，否则 HUB_PJ_MIN_S..HUB_PJ_MAX_S（附录 A；254 = SDK 的上限，255 是永久开放）。处理函数与下面的豁免共用 */
#define HUB_PJ_MIN_S  30u
#define HUB_PJ_MAX_S  254u

/* 这次调用不扣限速令牌：hub.zigbee.permit_join 的 seconds:0（关窗 —— 被限速挡住关不了窗是安全问题，6.5 注记），以及处理函数注定按参数错拒掉的
   seconds（不是整数、不在 0 / 30–254）—— 不然一次写错的调用占掉 60 s 一次的名额，改对了再调回 429（第六步上板 ZH10 抓到）。参数已过 hub_params_check。
   Muse（muse_link）与本地 REST（hub_api）两个出口都用这一个判断 */
bool hub_params_rl_exempt(const hub_cmd_t *c, const cJSON *params);

/* 进 cJSON 之前的形状预检（不分配内存）：嵌套 ≤ max_depth、值 ≤ max_values、字符串闭合、括号配对 —— 4 KB 以内也能写出约 990 层
   嵌套，cJSON 递归解析会打爆任务栈（会话任务 12 KB、httpd 8 KB）。原在 muse_proto.c（mp_json_shape_ok），M3 第七步挪来 */
bool hub_json_shape_ok(const char *json, size_t len, unsigned max_depth, unsigned max_values);
