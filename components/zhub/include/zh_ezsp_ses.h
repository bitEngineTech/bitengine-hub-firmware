#pragma once
/* zh_ezsp_ses —— EZSP 会话层，纯逻辑（docs/15_muse_direct_design_v1.md 8.4 收发规则、8.13；CHECKLIST Z08 Z57 Z58 Z59 C15）。
 * 不依赖 ESP-IDF、不分配内存：test_host/ 原样编它。夹在 zh_ash_core（链路）与 zh_main（命令的使用者）之间：
 *
 *   zh_main ──submit──▶ zh_ezsp_ses ──ops.send──▶ zh_ash（zh_ash_send）
 *          ◀─on_result / on_callback / on_event── ◀──zh_ses_rx── zh_ash 的 on_frame
 *
 * ⚠️ 线程契约（S2）：本模块没有锁。zh_ses_* 的所有入口、ops 的所有回调、以及 ops.send / ops.unacked 背后的 zh_ash_* 调用，
 *    必须在**同一个任务里串行**发生。CHECKLIST Z60 已定（2026-10-22）：设计 8.2 原来的 zh_ash 与 zh_main 合并成一个任务（zhub.c 的
 *    "zhub"），会话由编排层 zh_host_core 在那个任务里驱动，下面 H1 第 2 条"unacked 变 0 = 确认"照原样成立。
 *
 * 管的事：
 *  - 握手：链路连上（zh_ses_link_up）后会话自己发首条 version（旧式头，ezsp.c:525-528），应答的协议号是 0x12 才 READY；
 *    不是就停住、报 ZH_SES_EV_VERSION_MISMATCH（设计 8.4 的 ncp_ezsp_mismatch）。旧式头的应答只在这一条命令在途时接受（Z08）。
 *  - 同时只有一条命令在途；每次发送（含重发）换新 seq，应答按 seq 配；对不上的（迟到的旧应答）记 stale 丢掉。
 *  - C15（两个方向）：主机→NCP，应答计时从 **ASH 确认 NCP 收到这条命令之后**才起算（ops.unacked 变 0），不和主机侧 ASH 的纠错时间
 *    赛跑，久不确认 / 久交不出去另有兜底 ack_guard_ms；NCP→主机，应答超时要长过 **NCP 侧** ASH 重传应答的耗时，而那没有严格上界，
 *    所以**应答超时一次就复位链路**、不重发（会话若先放弃又发下一条，NCP 会把新命令写进还等着重传旧应答的那块缓冲 —— em260 原地应答的
 *    又一种形态）。数值与推导在 zh_ses_cfg_default()。
 *  - Z59：回显（方向是"命令"的同 seq 同 ID 帧）说明 NCP 已经执行过、应答被重传的命令覆盖了（em260 原地应答，zh_ash_core.h 末段）。
 *    幂等命令重发（max_retries 次）；非幂等命令不重发，回 ZH_SES_R_UNKNOWN。哪些算幂等见 zh_ses_idempotent()。
 *  - 保活（Z58、8.13）：空闲 keepalive_ms 发一次 nop，超时一次就判链路。NCP 静默进 ERROR、或应用层挂死而 ASH 照样确认时，
 *    ASH 自己发现不了，只能靠这个（空闲时最迟 keepalive + 应答超时发现；有命令在途时就是那条命令的应答超时）。
 *  - 回调：异步回调（fc 0x10）直接交 on_callback（先于"是不是在途应答"判断）；应答带"还有回调待取"（fc 0x04）且开了 poll_callbacks，
 *    空闲时发 callback 命令取（NCP 用同步回调时，E13）。callback 的应答是那条回调（fc 0x08）或 noCallbacks（不交上层）。
 *    回调待取与上层排队的命令轮流发，谁也饿不死。
 *  - 判断链路不可用时报 ZH_SES_EV_RESET_LINK 并停住（应答超时、握手失败、ASH 久不确认、内部命令回显用完、invalidCommand 原因 VERSION_NOT_SET）；
 *    上层复位 ASH（zh_ash_reset），连上后再 zh_ses_link_up。在途 / 排队的上层命令在停住时一并了结，不会悬着。
 *  - 命令了结后会话里那份命令帧整块清零（setInitialSecurityState 带网络密钥，Z17）；了结时不把整块命令拷到栈上。
 *  - 正向证据（Z47）：zh_ses_alive_evidence() 只在收到与在途命令配上的应答、或一条合法回调时更新 —— 别拿 t_idle 当证据（建链、超时了结都会刷它）。
 *
 * 结果的确切含义（上层按 Z59 处理）：
 *  - R_OK 只保证"头、seq、帧 ID 对得上"；参数还要用 zh_ezsp_dec_* 解，解不过（比如少一个字节、CRC 仍对的应答）时：
 *    幂等命令可重新 submit，非幂等命令按 UNKNOWN 处理（它已经执行了），并计入 UART 错误率（Z41）。
 *  - R_LINK_DOWN 只给**从没交给 ASH 过**的命令 —— 确定没执行；交出去过的在链路断 / 会话停住时回 R_UNKNOWN 或 R_TIMEOUT。
 *
 * H1（本层用到的下层接口，写代码前答清楚）：
 * 1. zh_ash_send(frame, len)：排进 ASH 发送队列（随机化在 ASH 里做），没连上 / 队列满回 false —— 我们下一拍再试，不丢命令。
 * 2. zh_ash_queued()：已发未确认 + 等着发的帧数。窗口 1、本层同时只有一条在途，所以它变 0 = NCP 确认收到了我们这条。
 *    （重传状态下确认到了也要等下一次 zh_ash_tx 才清队列，所以它可能晚一拍变 0；会话收应答不依赖它，只影响应答计时的起点。）
 * 3. zh_ash 的 on_frame 回调里交来的帧指针只在回调期间有效：zh_ses_rx 同步处理完，交给上层的 zh_ezsp_frame_t 也只在回调期间有效。
 * 4. 回调重入：on_result / on_callback / on_event 里可以调 zh_ses_submit / zh_ses_link_down（会话在调回调前已经把自己的状态收拾好、
 *    状态已是新的；回调里 submit 的命令等回调返回才发）。**不许在回调里 zh_ses_link_up**（会被忽略、记 misuse）：link_up 只在 zh_ash 复位后
 *    报 CONNECTED 时调，而复位要等 RSTACK 往返；在没复位的链路上握手会撞 NCP 还在重传的应答缓冲。RESET_LINK 总是会报（上层在结果回调里
 *    已 link_down 过也报，再 link_down 一次是幂等的）—— 复位 ASH 这一步不能被省。
 *
 * 时间都是调用方给的单调毫秒（uint32_t，回绕安全：只比差值）。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zh_ezsp_core.h"

#define ZH_SES_INVALID_VERSION_NOT_SET 0x30   /* invalidCommand 的原因码：NCP 收到命令时还没握过手（ezsp-enum.h:616 SL_ZIGBEE_EZSP_ERROR_VERSION_NOT_SET） */
#define ZH_SES_MAX_RETRIES_CAP         3      /* cfg.max_retries 的上限（防"回显就重发"无限循环） */
#define ZH_SES_POLL_MS                 20     /* 没有事件可等时（等 ASH 收 / 等确认）回来看的间隔 */

typedef enum {
    ZH_SES_DOWN = 0,     /* 链路没连上 */
    ZH_SES_HANDSHAKE,    /* 连上了，首条 version 在途 */
    ZH_SES_READY,        /* 可以 submit */
    ZH_SES_STOPPED,      /* 报过 RESET_LINK / VERSION_MISMATCH：什么都不发，等上层 link_down → 复位 ASH → link_up */
} zh_ses_state_t;

typedef enum {
    ZH_SES_R_OK = 0,     /* 应答的头、seq、帧 ID 都对；fr 指向应答（参数用 zh_ezsp_dec_* 解，解不过的处置见文件头） */
    ZH_SES_R_INVALID,    /* NCP 回 invalidCommand；原因码在 fr->invalid_reason */
    ZH_SES_R_TRUNCATED,  /* NCP 说应答被截断了 */
    ZH_SES_R_TIMEOUT,    /* 交出去之后没等到应答（会话随即复位链路）：执行没执行不知道 */
    ZH_SES_R_UNKNOWN,    /* 执行了或可能执行了、结果未知：回显（非幂等不重发；幂等的重发用完仍回显也回它），或交出去之后链路断了 / 会话停住了 */
    ZH_SES_R_LINK_DOWN,  /* 从没交给 ASH 就断了 / 停住了：确定没执行，可以照常重发 */
} zh_ses_result_t;

typedef enum {
    ZH_SES_EV_READY = 0,        /* 握手完成；code = NCP 的 stackVersion（高 16 位放 stackType） */
    ZH_SES_EV_VERSION_MISMATCH, /* 协议号不是 0x12；code = NCP 回的协议号。会话停住 */
    ZH_SES_EV_RESET_LINK,       /* 会话判断链路不可用；code = zh_ses_why_t。会话停住 */
} zh_ses_event_t;

typedef enum {
    ZH_SES_WHY_KEEPALIVE = 1,   /* 保活 nop 无应答 */
    ZH_SES_WHY_RSP_TIMEOUT,     /* 上层命令（或 callback 取回调）交出去之后没等到应答 */
    ZH_SES_WHY_HANDSHAKE,       /* 首条 version 无应答 / 被拒 / 被截断 */
    ZH_SES_WHY_ACK_GUARD,       /* 交给 ASH 之后 ack_guard_ms 还没被确认，或这么久都交不出去 */
    ZH_SES_WHY_VERSION_NOT_SET, /* NCP 说还没握过手（它自己重启过、我们不知道） */
    ZH_SES_WHY_ECHOES,          /* 内部命令（version / nop / callback）重发用完了还是回显 */
} zh_ses_why_t;

typedef struct {
    bool (*send)(void *ctx, const uint8_t *frame, size_t len);   /* = zh_ash_send */
    unsigned (*unacked)(void *ctx);                               /* = zh_ash_queued */
    void (*on_result)(void *ctx, uint32_t tag, zh_ses_result_t r, const zh_ezsp_frame_t *fr);   /* fr 只在 R_OK / R_INVALID / R_TRUNCATED 时非 NULL */
    void (*on_callback)(void *ctx, const zh_ezsp_frame_t *fr);
    void (*on_event)(void *ctx, zh_ses_event_t ev, uint32_t code);
    void *ctx;
} zh_ses_ops_t;

typedef struct {
    uint32_t rsp_timeout_ms;   /* NCP 确认收到之后等应答多久；超时即复位链路（0 = 用默认） */
    uint32_t ack_guard_ms;     /* 交给 ASH 之后多久还没被确认 / 多久都交不出去就判链路（0 = 用默认） */
    uint32_t keepalive_ms;     /* 空闲多久发一次 nop（0 = 不保活，只给测试） */
    uint8_t  max_retries;      /* 幂等命令回显后最多重发几次（上限 ZH_SES_MAX_RETRIES_CAP） */
    bool     poll_callbacks;   /* NCP 说还有回调待取时，空闲就发 callback 取 */
} zh_ses_cfg_t;

typedef struct {
    uint32_t rx_frames;      /* zh_ses_rx 收到的帧 */
    uint32_t responses;      /* 正向证据：配上的应答 + 合法回调 */
    uint32_t rejected;       /* 头解析不过（短帧 / 保留位 / 不该出现的旧式头）、应答长度不对——也算进 UART 错误率的分子（Z41） */
    uint32_t echoes;         /* 回显 */
    uint32_t stale;          /* seq 对不上的应答（迟到的旧应答、没有在途时来的应答） */
    uint32_t id_mismatch;    /* seq 对得上、帧 ID 对不上 */
    uint32_t timeouts;       /* 应答超时（每次都随之复位链路） */
    uint32_t retries;        /* 回显后的重发 */
    uint32_t invalid;        /* invalidCommand */
    uint32_t truncated;      /* 截断（应答或回调） */
    uint32_t overflow;       /* 帧上带溢出位（NCP 缓冲紧张，丢过回调） */
    uint32_t callbacks;      /* 交出的回调 */
    uint32_t lost_callbacks; /* callback 命令的应答被回显吞掉：那条回调丢了 */
    uint32_t keepalives;     /* 发出的保活 nop */
    uint32_t unknown;        /* 回 ZH_SES_R_UNKNOWN 的次数 */
    uint32_t misuse;         /* 调用方用错了接口（回调里 link_up）：被忽略的次数 */
} zh_ses_stats_t;

typedef struct {
    bool     used;
    bool     internal;       /* 会话自己发的（version / nop / callback）：结果不交上层 */
    bool     idempotent;
    bool     handed;         /* 已交给 ASH */
    bool     acked;          /* ASH 确认 NCP 收到了 */
    uint8_t  tries;          /* 已发了几次 */
    uint8_t  seq;            /* 最近一次发送用的 seq */
    uint16_t id;
    uint32_t tag;
    uint32_t t_queued;       /* 进入"待发"的时刻（ack_guard 也管交不出去） */
    uint32_t t_handed, t_acked;
    uint8_t  len;
    uint8_t  buf[ZH_EZSP_MAX];
} zh_ses_cmd_t;

typedef struct {
    zh_ses_cfg_t   cfg;
    zh_ses_ops_t   ops;
    zh_ses_state_t state;
    uint8_t        seq_next;
    zh_ses_cmd_t   cur;          /* 在途 */
    zh_ses_cmd_t   pend;         /* 上层的一条，等在途的内部命令完成（上层同时只能有一条：在途或排队） */
    bool           cb_wanted;    /* NCP 说还有回调待取 */
    bool           cb_turn;      /* 回调已轮过一次：下次与排队的上层命令同时想发时让上层先 */
    uint32_t       t_idle;       /* 上一次有往来（收到帧 / 命令了结 / 建链）的时刻：只管保活计时，**不是**活性证据 */
    bool           have_alive;
    uint32_t       t_alive;      /* 最近一次正向证据的时刻 */
    uint8_t        depth;        /* 正在调上层回调的层数：回调里 submit 的命令等回调返回再发 */
    zh_ses_stats_t st;
} zh_ses_t;

typedef enum {
    ZH_SES_OK = 0,
    ZH_SES_E_ARG = -1,        /* NULL、长度不对、不是扩展头的命令帧、会话自己用的帧 ID（callback / noCallbacks） */
    ZH_SES_E_NOT_READY = -2,  /* 还没握完手 / 停住了 / 链路没连上 */
    ZH_SES_E_BUSY = -3,       /* 上层已有一条在途或排队 */
} zh_ses_err_t;

void zh_ses_cfg_default(zh_ses_cfg_t *c);
/* 清零并装配置与回调（cfg 为 NULL 用默认；越界的配置会被夹住）。初始状态 DOWN。 */
void zh_ses_init(zh_ses_t *s, const zh_ses_cfg_t *cfg, const zh_ses_ops_t *ops);
/* zh_ash 复位后报 CONNECTED 时调（不许在会话的回调里调）：seq 从 0 重来，会话发首条 version（旧式头）。 */
void zh_ses_link_up(zh_ses_t *s, uint32_t now_ms);
/* ASH 断了（任何 zh_ash 事件）或上层要复位 / suspend：在途的上层命令回 UNKNOWN（交出去过）或 LINK_DOWN（没交出去过），排队的回 LINK_DOWN，回到 DOWN。 */
void zh_ses_link_down(zh_ses_t *s);
/* 上层发一条命令：frame 用 zh_ezsp_enc_* 编（seq 随便填，会话改写）。READY 之后才收；上层同时只能有一条。
   结果经 on_result(tag, …) 交回（不会在 submit 里同步回调）。idempotent 一般给 zh_ses_idempotent(帧 ID)。 */
int  zh_ses_submit(zh_ses_t *s, const uint8_t *frame, size_t len, uint32_t tag, bool idempotent, uint32_t now_ms);
/* zh_ash 的 on_frame 收到的 EZSP 帧 */
void zh_ses_rx(zh_ses_t *s, const uint8_t *frame, size_t len, uint32_t now_ms);
/* 跑定时器（确认 / 应答超时、兜底、保活）并把该发的交给 ASH。调用方每拍调。 */
void zh_ses_tick(zh_ses_t *s, uint32_t now_ms);
/* 最多多久之后该再 tick 一次（毫秒）；UINT32_MAX = 没有在等的定时器（DOWN / STOPPED：调用方自己夹一个上限，看门狗心跳别绑在这上面） */
uint32_t zh_ses_next_timeout_ms(const zh_ses_t *s, uint32_t now_ms);
/* 帧 ID 的默认幂等性（Z59）：读取类、"设成某个值"类是幂等的；会改变网络 / 发报文 / 加东西的不是；不认识的一律当非幂等 */
bool zh_ses_idempotent(uint16_t frame_id);
static inline bool zh_ses_ready(const zh_ses_t *s) { return s->state == ZH_SES_READY; }
/* 上层能不能 submit（READY 且没有自己的在途 / 排队） */
bool zh_ses_can_submit(const zh_ses_t *s);
/* NCP 应用层还活着的正向证据：有过就返回 true 并给出最近一次的时刻 */
bool zh_ses_alive_evidence(const zh_ses_t *s, uint32_t *t_ms);
