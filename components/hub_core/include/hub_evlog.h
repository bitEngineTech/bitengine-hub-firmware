#pragma once
/* hub_evlog —— 最近事件环（docs/15_muse_direct_design_v1.md 6.7）。纯 C，不依赖 ESP-IDF：test_host/ 原样编它。
 *
 * 所有事件都进环（包括不推送的），64 条，满了覆盖最旧的；hub.events.recent 从这里读（新的在前）。
 * 事件的文字在进环之前就渲染、净化好（6.7 模板、6.8 卫生）—— 这里不碰内容，只存、只按时间与条数筛。
 * 不是线程安全的：调用方（hub_events.c）持锁调。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hub_json.h"
#include "hub_model.h"

#define HEV_RING_CAP   64
#define HEV_DEV_MAX    24      /* "zb-00158d0001a2b3c4"（19）、"th-…"（19）；"" = 网关自己 */
#define HEV_KIND_MAX   24      /* 机器可读的事件名："thread_down"、"water_leak"…：[a-z0-9_] */
#define HEV_TEXT_MAX   200     /* 一条事件的英文句子（已渲染、已净化，不带 [id] 尾巴） */

typedef struct {
    uint32_t        seq;                    /* 进环时分配，从 1 起递增；0 = 空槽 */
    hub_evt_class_t cls;
    uint64_t        mono_ms;                /* 开机毫秒：算 age_s 用 */
    char            clock[6];               /* 本地时间 "HH:MM"（发生时已对时）；没对时是空串 */
    uint8_t         flags;                  /* hub_events.h 的 HEV_F_*（推不推）；环本身不看它 */
    char            dev_id[HEV_DEV_MAX];
    char            kind[HEV_KIND_MAX];
    char            text[HEV_TEXT_MAX];
} hev_t;

typedef struct {
    hev_t    e[HEV_RING_CAP];
    uint32_t next_seq;                      /* 下一条的 seq */
    uint32_t head;                          /* 下一条写到哪个槽 */
    uint32_t count;                         /* 环里有几条（≤ CAP） */
} hev_ring_t;

void hev_ring_init(hev_ring_t *r);

/* 拷一条进环（seq 由这里分配，ev->seq 忽略）。参数不合格（类别越界、文字为空、字段没 NUL、kind 含 [a-z0-9_] 以外的
   字符）返回 0、不进环；否则返回分配的 seq。 */
uint32_t hev_ring_add(hev_ring_t *r, const hev_t *ev);

/* 第 n 新的那条（n = 0 最新）；没有返回 NULL */
const hev_t *hev_ring_newest(const hev_ring_t *r, size_t n);

/* 类别 → 对外名字（"alarm"…"battery"），越界 NULL */
const char *hev_class_name(hub_evt_class_t c);

/* hub.events.recent 的结果：{"events":[…],"count":N}，新的在前；limit 1–50（越界夹到边上），since_s = 0 表示不限时间。
   放不下时撤掉写了一半的那条、带 "truncated":true 收尾（hj_mark / hj_rollback）。 */
void hev_write_recent(hj_t *w, const hev_ring_t *r, uint64_t now_ms, uint32_t limit, uint32_t since_s);
