#pragma once
/* hub_zigbee —— hub_core 这一侧的 Zigbee 设备（docs/15_muse_direct_design_v1.md 6.1、8.9；M4 第七步 a；只在直连型、开了 zhub 时编）。
 *  - 给 zhub 登记钩子：设备记录存进 hubdb（命名空间 zhub，键 dNN，hub_store 2 s 防抖）、设备事件进事件环（hub_event_emit）。
 *    zhub 不依赖 hub_core：两个组件只在这一处接上（zhub_set_hooks，要在 zhub_start 之前）。
 *  - 把 zhub 的设备表写成 hub.devices.list / hub.device.get 的列表项（附录 A 的格式；id = "zb-" + 16 位小写十六进制 IEEE，高字节在前）。
 *    kind / 能力按访谈记下的簇推（hub_zcap，8.8），读数在 state、年龄在 state_age_s（列表与详情都有）；不可控（控制随 M5 后续）；名字 / 房间 / notify / sensitive 来自 hub_meta（hub.device.update，M5 第二步）。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hub_json.h"

/* 登记钩子。hub_svc_start 里调（main.c 里它在 zhub_start 之前）。 */
void hub_zigbee_init(void);

enum {
    HUB_ZB_BUSY = -1,         /* 设备表这会儿拿不到（内存 / zhub 的锁忙）：调用方回 unavailable，别说成没有设备 */
    HUB_ZB_DONE = 0,          /* 写完了 / 找到了 */
    HUB_ZB_TRUNCATED = 1,     /* 列表写不下：回滚了半条，*next = 下一条的序号 */
    HUB_ZB_NOT_FOUND = 2,     /* hub.device.get：没有这个 id */
};
/* 列表：把第 start 条起的 Zigbee 设备写成数组元素（调用方已开好 devices 数组）。cap_filter 非 0 = 只要有这些能力之一的（HUB_CAP_*）；
   room 非 NULL = 只要房间与它相同的（不分大小写；调用方先按用户名规则净化过）；
   start / *next / n_out 都按过滤后的序号算，n_out = 过滤后的总数（给调用方算 Thread 那部分的起点）。 */
int hub_zigbee_write_list(hj_t *w, unsigned long start, unsigned long *next, unsigned long *n_out, uint32_t cap_filter, const char *room);
/* hub.device.get：id 是 "zb-…" 才找；找到写一个对象。w 为 NULL = 只问有没有（hub.device.update 先确认设备在） */
int hub_zigbee_write_device(hj_t *w, const char *id);
/* 控制（8.11，M5 第七步）：查设备的 IEEE / 能力位 / 控制路径现在真能驱动的能力位（hub_zcap_ctl_caps，M5 第八步）/ 常电 / 敏感标记；
   回 HUB_ZB_DONE / NOT_FOUND / BUSY。参数可为 NULL */
int hub_zigbee_ctl_info(const char *id, uint8_t ieee[8], uint32_t *caps, uint32_t *ctl_caps, bool *rx_on, bool *sensitive);
/* 控制之后回读：在 w 的当前位置写 "state" 键与对象（调用方已开好外层对象） */
int hub_zigbee_write_state(hj_t *w, const char *id);
/* Zigbee 设备总数（zhub 没起为 0；拿不到 -1） */
long hub_zigbee_count(void);
