#pragma once
/* muse_push_core —— 推送队列（docs/15_muse_direct_design_v1.md 4.9）。纯 C，不依赖 ESP-IDF：test_host/ 原样编它。
 *
 * 会话任务持有一个 mpq_t（PSRAM），事件管线（6.7，hub_core）把渲染好的英文句子交进来；在线时会话任务每圈问一次
 * "下一条该发谁"，发出去（POST /chat/stream）成功才算数。
 *
 * **网关不对推送做人为干预（2026-10-24 用户定）**：每个事件来了就排、排上就发 —— 不合并、不去重、不限配额，
 * 怎么归并、要不要提醒交给 Muse。以前的 10 s 合并窗口让每条门磁 / 人体推送固定晚 10 s，去重 / 配额会把来回开关门、
 * 连着有人经过的后几条吞掉。只剩资源与送达上的硬边界：
 *   - 队列 MPQ_CAP（64）条；满了丢**最低优先级里最旧的**那条（新来的若就是唯一最低的，丢它自己）；
 *   - 过了 max_age_s 还没发出去就丢（ALARM 1800 s，其余 600 s：断线太久，回来不补发旧事）；
 *   - 服务端明确拒收（4xx）的那条丢掉、不重试（mpq_drop），5xx / 超时 / 断线留着重试到过期。
 * 不是线程安全的：只在会话任务里调。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hub_json.h"
#include "hub_model.h"

#define MPQ_CAP            64         /* 不合并以后一个事件占一格：16 格在断线几分钟、门磁人体一阵连发时会挤掉报警（审计 ⚠️）；PSRAM 里约 34 KB */
#define MPQ_TEXT_MAX       512
/* 推送正文前的来源前缀（2026-10-23 用户定）：Meta 的推送接口把它当"来自这台设备的用户消息"，App 里画在用户那一侧、像用户自己发的；
   前缀让用户一眼认出是网关发的，也让 agent 知道这是网关的通知、该转告。只在拼请求体时加 */
#define MUSE_PUSH_PREFIX   "[bitEngine Hub] "
#define MPQ_ALARM_AGE_S    1800u
#define MPQ_OTHER_AGE_S    600u

typedef struct {
    hub_evt_class_t cls;
    bool            main_chat;               /* true 进主会话；false 进侧边会话（chat_sid） */
    char            text[MPQ_TEXT_MAX];      /* 已渲染、已净化的英文句子（6.7 模板） */
} muse_push_t;

typedef struct {
    struct { muse_push_t msg; uint64_t enq_ms; bool used, inflight; } slot[MPQ_CAP];
    uint32_t     sent, dropped_full, dropped_expired, dropped_rejected, dropped_cleared;
} mpq_t;

typedef enum {
    MPQ_QUEUED = 0,
    MPQ_QUEUED_EVICTED,      /* 排进去了，挤掉了一条更低优先级（或同级更旧）的 */
    MPQ_DROPPED_FULL,        /* 队列满，新来的这条就是最低优先级里最旧的 */
    MPQ_BAD_ARG,
} mpq_result_t;

void mpq_init(mpq_t *q, uint64_t now_ms);
/* 优先级：ALARM 最高，其余按 hub_evt_class_t 的顺序递减 */
uint8_t mpq_prio(hub_evt_class_t c);
uint32_t mpq_max_age_s(hub_evt_class_t c);
mpq_result_t mpq_enqueue(mpq_t *q, const muse_push_t *m, uint64_t now_ms);
/* 下一条该发的（优先级最高、同级最旧、没过期）；顺手丢掉已过期的。没有返回 -1。 */
int mpq_next(mpq_t *q, uint64_t now_ms);
/* 发出去、等回包期间：这一槽不会被挤掉、不会过期、mpq_next 不再给它（回包到了才按结果处理，不会记到别的消息头上）。
   同一时刻最多一条在途由调用方保证。 */
void mpq_begin(mpq_t *q, int idx);
/* 没成（5xx、超时、断线）：取消在途标记，留在队列里等下次（过期照算，从入队时刻起） */
void mpq_abort(mpq_t *q, int idx);
/* 发出去了（2xx）：腾出槽位、计 sent */
void mpq_mark_sent(mpq_t *q, int idx, uint64_t now_ms);
/* 服务端明确拒收（4xx）：丢掉这条，计 dropped_rejected */
void mpq_drop(mpq_t *q, int idx);
/* 关掉 / 解绑：丢掉排着的（含在途的），计 dropped_cleared；计数保留（状态页计数不倒退） */
void mpq_clear_queue(mpq_t *q);
/* 丢掉的总数（满、过期、被拒、被清），给状态页的 pushes_dropped */
uint32_t mpq_dropped_total(const mpq_t *q);
const muse_push_t *mpq_get(const mpq_t *q, int idx);
size_t mpq_count(const mpq_t *q);

/* /chat/stream 回包的处理（status：应答头的 HTTP 状态；-1 = 流被复位或会话断了；0 = 超时还没回包） */
typedef enum {
    MPQ_REPLY_SENT = 0,      /* 2xx：发出去了（Linux SDK 的判据 200 ≤ status < 300；没有 message_id 也算，免得重发出两条） */
    MPQ_REPLY_REJECTED,      /* 4xx（408、429 除外）：服务端明确不收，丢掉不重试 */
    MPQ_REPLY_RETRY,         /* 408 / 429 / 5xx / 复位 / 超时 / 其他：留着重试到过期 */
} mpq_reply_t;
mpq_reply_t mpq_classify_status(int status);

/* /chat/stream 的请求体（4.9、附录 B ④）：{"message":…,"output_modality":"text","device_id":<node_id>,"session_id":<chat_sid>}；
   主会话时省略 session_id。返回长度或 -1。 */
int mpq_build_body(char *buf, size_t cap, const muse_push_t *m, const char *node_id, const char *chat_sid);
