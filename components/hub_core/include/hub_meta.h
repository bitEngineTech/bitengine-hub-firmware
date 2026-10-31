#pragma once
/* hub_meta —— 设备元数据（用户起的名字、房间、推送开关、敏感标记）的存取（docs/15_muse_direct_design_v1.md 6.1、附录 A hub.device.update；
 * M5 第二步）。只在直连型里编。表与序列化的规则在 hub_meta_core.h（纯逻辑、主机测试原样编）；这里管加锁、开机读回、改完写回 hubdb。
 *
 * 存储：hubdb 的 HUB_STORE_NS_HUB 命名空间，键 meta0…meta3（每页一个 blob，格式版本 HUB_META_VER）。开机逐页读：
 *  - 没有 → 空页；
 *  - 格式不对（版本对、结构坏）→ 空页、可写（盖掉无妨，打 W 日志）；单条名字过不了现在的净化规则只丢那一条；
 *  - 读不出来（超时 / 读错 / 存的格式比固件新：OTA 回滚后）→ 这一页这次开机只读，**新条目这次开机一律不建**（回 STORE）：
 *    看不见那一页，就证明不了这台设备不在上面（两轮审计 ❌）。一页超时后后面的页不再等，也按只读。判定的真值表在 hub_meta_core。
 *  读完去一遍跨页重复（存储被改坏才会有）。
 * 并发：hub_meta_update 可能同时来自本地 REST（httpd）与 Muse 会话任务 —— 一把写锁让它们排队（打包与投递的顺序一致，后改的不会被先改的盖回去）；
 * 表本身另一把锁，只在内存里拷几十字节时持有，所以 zhub 任务里查名字（事件句子）等不久。 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "hub_meta_core.h"

/* 分配表（PSRAM）、开机读回。main.c 里 hub_store_start 之后、zhub_start 之前（hub_svc_start 调）。幂等 */
esp_err_t hub_meta_init(void);

/* 查一台设备的元数据。没有条目 / 表没起来 / 锁一时拿不到都回 false、*out 清零（= 默认：没名字、没房间、不推送）。
   lock_ms：zhub 任务里用小值（它还跑着 ASH），其余调用方用 HUB_META_LOOKUP_MS */
#define HUB_META_LOOKUP_MS 200u
bool hub_meta_lookup(uint8_t proto, const uint8_t id8[8], hub_meta_ent_t *out, uint32_t lock_ms);
/* 同上，但分得清"确实没有条目"与"查不了"（表没起来 / 锁没拿到）：放行方向的判定（敏感标记）要用它 —— 查不了别当成"不敏感"（M5 第七步审计） */
enum { HUB_META_LK_UNKNOWN = -1, HUB_META_LK_NONE = 0, HUB_META_LK_FOUND = 1 };
int  hub_meta_lookup_ex(uint8_t proto, const uint8_t id8[8], hub_meta_ent_t *out, uint32_t lock_ms);

/* Zigbee 的 IEEE（小端，zh_dev_rec_t.ieee）→ id8（高字节在前） */
void hub_meta_id8_from_ieee(const uint8_t ieee[8], uint8_t id8[8]);

/* 改一条并排上写（hub_store 2 s 防抖）。id 是 "zb-…" / "mt-…"（是否真有这台设备由调用方先判）。成功时 *out = 改完的样子。
   失败：hub_meta_core.h 的那些；STORE = 表没起来 / 有页这次开机读不出来 / hub_store 没起来（重启网关才可能好）；
   BUSY = 锁拿不到 / 写队列满（过一会儿再试）。写没排上时内存里的改动已撤回 */
hub_meta_res_t hub_meta_update(const char *id, const hub_meta_change_t *c, hub_meta_ent_t *out);

typedef struct {
    bool     ready;
    uint8_t  pages_ro;         /* 第 i 位 = 第 i 页这次开机只读 */
    uint16_t used;             /* 占用的条目数 */
    uint32_t updates, update_fail;
    uint32_t lookup_busy;      /* 查名字时锁没拿到（那条事件按没名字、不推送走了） */
    uint32_t dropped;          /* 开机读回时因名字过不了净化规则丢掉的条数 */
} hub_meta_status_t;
void hub_meta_get_status(hub_meta_status_t *out);
/* 删掉某个协议的全部条目并落盘（重置 Matter 控制器，10.1）。有页这次开机读不出来：ESP_ERR_INVALID_STATE、什么都不动（重启后再来）；
   排不上写：ESP_ERR_TIMEOUT，内存里那一页撤回（别留没落盘的删除）。每页都重写，重试幂等。任何任务里可调，最多等约 1 s / 页 */
esp_err_t hub_meta_forget_proto(uint8_t proto);
