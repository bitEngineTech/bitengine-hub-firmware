#pragma once
/* zh_ash_core —— ASH（EZSP 的 UART 链路层）的主机侧，纯逻辑（docs/15_muse_direct_design_v1.md 8.3）。不依赖 ESP-IDF：test_host/ 原样编它。
 *
 * 语义的正本是本机 SDK（Simplicity SDK 2025.6.3，~/src/mg24-fw/sdk）里的主机参考实现 protocol/zigbee/app/ezsp-host/ash/ash-host.c
 * 与公共编解码 platform/service/legacy_common_ash/src/ash-common.c（Silicon Labs MSLA）。代码是重写的、没有复制粘贴，但**按 ash-host.c 的结构
 * 逐段对照**写成（标志位、帧号变量、收帧与发送优先级的分支顺序都对应）—— 结构上是衍生的，开源前要处理，见 ../UPSTREAM.md。
 * SDK 源码只在 test_host/ 里从本机路径编来做互通测试，不进仓库。
 *
 * H1（首次使用协议，写代码前先答清楚；出处都是上面两份 + ash-protocol.h）：
 * 1. 每个字段的精确含义：
 *    - 帧 = 控制字节 + 数据域 + CRC（2 B，高字节先）+ FLAG。CRC 覆盖控制字节与数据域（**随机化之后的**数据），不含 FLAG、不含转义
 *      （ash-common.c:75-109 ashEncodeByte：先算 CRC 再 stuff）。
 *    - 控制字节（ash-protocol.h:67-82）：DATA = 0 frmNum(3) rFlag ackNum(3)；ACK = 100 pF nF ackNum(3)；NAK = 101 pF nF ackNum(3)；
 *      RST 0xC0、RSTACK 0xC1、ERROR 0xC2。ackNum = **下一个期望收到的** frmNum。
 *    - RSTACK / ERROR 的数据域 2 B：{ASH 版本（必须 2），复位原因 / 错误码}（ash-host.c:325-347、ash-ncp.c:546-551）。
 *    - 只有 DATA 帧的数据域随机化（LFSR seed 0x42 poly 0xB8，ash-common.c:204-215），收发同一个函数（XOR 自逆）。
 *    - 数据域 3..220 字节（EZSP_MIN/MAX_FRAME_LENGTH，ezsp-protocol.h:36、42）。
 *    - 转义集合：7E 7D 11 13 18 1A → 7D, b^0x20（ash-common.c:111-125）。0xFF 不转义：帧与帧之间是 WAKE，帧内是普通数据。
 *    - 收到 CAN：丢弃当前帧（不算错）；SUB：这一帧有 UART 错，到 FLAG 时整帧作废（ash-common.c:127-200）。
 *    - RTS/CTS 流控下 XON / XOFF 直接忽略（ash-common.c:169-180 主机分支；数据里的 11 / 13 都转义过）。
 * 2. 必须实现的回调 / 释放：本模块不分配内存。收到的 DATA 帧经 on_frame 回调交出去（指针只在回调期间有效，回调里拷走）；
 *    链路事件经 on_event 交出去。调用方负责：UART 收到的字节喂 zh_ash_rx()；周期调 zh_ash_tx() 拿要写的字节（同时跑定时器）。
 * 3. 获取 / 释放的配对：zh_ash_init() 只清零；zh_ash_reset() 发 CAN + RST，等到合格的 RSTACK 才算连上（ZH_ASH_EV_CONNECTED）。
 *    连上之后 NCP 自己重启（再来一个 RSTACK）、ERROR 帧、ACK 连续超时到上限 —— 都报事件、回到"断开"，由上层再调 zh_ash_reset()
 *    并重新初始化 EZSP（8.3、8.13）。断开期间 zh_ash_send() 一律拒收。
 * 4. 字节级格式：见第 1 条；帧号都是 3 位、模 8，"在窗口内"用 within_range(lo, n, hi) = (n-lo)&7 <= (hi-lo)&7（ash-protocol.h:126-129）。
 *
 * 与设计 8.3 原文的出入（2026-10-22，以 SDK 主机参考实现为准；设计写时以为本机只有 NCP 侧源码）：
 * - ACK 定时器默认 800 ms、自适应夹在 400–2400 ms，连续超时上限 6，等 RSTACK 5000 ms（ash-host.c:86-89 ASH_HOST_CONFIG_DEFAULT、ash-host.h:31）。
 *   设计原文的 1600 / 400–3200 ms、4 次、2500 ms 出自 UG101 / bellows / ncp_version.c（bellows 另取 5 次）；NCP 侧自己用 1600 / 400–3200、3 次
 *   （ash-ncp.h:237-242）。两套都能用（周期是自适应的），我们跟 SDK 主机参考实现；都在 zh_ash_cfg_t 里，正本是 zh_ash_cfg_default()。
 * - 发送窗口按设计取 1（停等）；参考实现默认 3（ash-host.c:84），bellows 取 1。NCP 只有一块接收缓冲、命令缓冲就是应答缓冲，窗口 1 正合适（H1）。
 *   1..3 都支持，互通测试里 k=3 流水线也跑。
 * - 重传前也发一个 CAN（同参考实现 ash-host.c:752-760、470-473：空闲时照样写）：清掉 NCP 解码器里可能残留的半截帧，噪声下 ACK 超时更少。
 * - 不实现接收端的"没准备好"（nFlag）：收到的帧同步交给回调，主机侧总是准备好的。NCP 的 nFlag 照参考实现忽略、只计数。
 *
 * 留给上层（EZSP 层、zh_main）的硬要求（2026-10-22 第一轮审计复现过）：
 * - NCP 自己连续超时会进 ERROR，之后只在**收到帧时**才回 ERROR；那一帧 ERROR 若被噪声吃掉，本层队列空、定时器停，会一直以为连着。
 *   上层要有 EZSP 应答超时 → 复位，并在空闲时定期发点什么（EZSP nop）保活（设计 8.13）。
 * - CRC-CCITT 不做终异或：CRC 低字节为 0x00 的帧若恰好丢了这个字节，会变成"少一个数据字节、CRC 仍对"的合法帧（约 1/256 × 丢字节率）。
 *   EZSP 层要按帧 ID 严格校验应答长度（设计 8.4）。
 * - 真 NCP 把应答写回命令所在的同一块缓冲（em260），主机重传命令时可能把它覆盖 —— NCP 会把**主机自己的命令**当应答发回来。
 *   EZSP 层必须检查 frame control 的方向位，丢掉回显、超时重发（设计 8.4）。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- 常量（出处见行尾）---- */
#define ZH_ASH_FLAG          0x7E   /* ash-protocol.h:39 */
#define ZH_ASH_ESC           0x7D   /* ash-protocol.h:40 */
#define ZH_ASH_XON           0x11   /* ash-protocol.h:41 */
#define ZH_ASH_XOFF          0x13   /* ash-protocol.h:42 */
#define ZH_ASH_SUB           0x18   /* ash-protocol.h:43 */
#define ZH_ASH_CAN           0x1A   /* ash-protocol.h:44 */
#define ZH_ASH_WAKE          0xFF   /* ash-protocol.h:48：只在帧与帧之间有特殊含义 */
#define ZH_ASH_FLIP          0x20   /* ash-protocol.h:51 */
#define ZH_ASH_CTRL_ACK      0x80   /* ash-protocol.h:93，+ ackNum，nFlag 0x08 */
#define ZH_ASH_CTRL_NAK      0xA0   /* ash-protocol.h:94 */
#define ZH_ASH_CTRL_RST      0xC0   /* ash-protocol.h:95 */
#define ZH_ASH_CTRL_RSTACK   0xC1   /* ash-protocol.h:96 */
#define ZH_ASH_CTRL_ERROR    0xC2   /* ash-protocol.h:97 */
#define ZH_ASH_RFLAG         0x08   /* ash-protocol.h:101：DATA 帧的重传标志 */
#define ZH_ASH_NFLAG         0x08   /* ash-protocol.h:103：ACK / NAK 的"没准备好" */
#define ZH_ASH_LFSR_SEED     0x42   /* ash-common.c:38 */
#define ZH_ASH_LFSR_POLY     0xB8   /* ash-common.c:37 */
#define ZH_ASH_VERSION       2      /* ash-protocol.h:35：RSTACK 里的 ASH 版本必须是它 */
#define ZH_ASH_DATA_MIN      3      /* ezsp-protocol.h:42 EZSP_MIN_FRAME_LENGTH */
#define ZH_ASH_DATA_MAX      220    /* ezsp-protocol.h:36 EZSP_MAX_FRAME_LENGTH */
#define ZH_ASH_RESET_SOFTWARE 11    /* em2xx-reset-defs.h:33 EM2XX_RESET_SOFTWARE：我们发 RST 导致的复位只报这个原因 */

#define ZH_ASH_IS_DATA(c)    (((c) & 0x80) == 0)

/* 一帧最大的原始长度（控制 + 数据 + CRC）与写 UART 时最坏的长度（每字节都转义 + FLAG + 前导 CAN） */
#define ZH_ASH_RAW_MAX       (1 + ZH_ASH_DATA_MAX + 2)
#define ZH_ASH_WIRE_MAX      (2 * ZH_ASH_RAW_MAX + 2)
#define ZH_ASH_TXQ           4      /* 已发未确认 + 等着发的帧（窗口 ≤ ZH_ASH_TXQ - 1） */

typedef struct {
    uint16_t ack_init_ms;     /* 800（ash-host.c:86） */
    uint16_t ack_min_ms;      /* 400 */
    uint16_t ack_max_ms;      /* 2400 */
    uint16_t rst_timeout_ms;  /* 5000（ash-host.c:89） */
    uint8_t  max_timeouts;    /* 6（ash-host.h:31）：连续这么多次 ACK 超时就判链路断 */
    uint8_t  tx_k;            /* 发送窗口，1..ZH_ASH_TXQ-1；设计 8.3 取 1 */
} zh_ash_cfg_t;

typedef enum {
    ZH_ASH_EV_CONNECTED = 0,  /* 收到合格的 RSTACK：可以发 EZSP 了 */
    ZH_ASH_EV_RESET_FAILED,   /* rst_timeout_ms 内没等到合格的 RSTACK */
    ZH_ASH_EV_VERSION,        /* RSTACK 的 ASH 版本不是 2：这颗 NCP 说的不是我们认识的 ASH */
    ZH_ASH_EV_NCP_RESET,      /* 连上之后又收到 RSTACK：NCP 自己重启了（code = 复位原因） */
    ZH_ASH_EV_NCP_ERROR,      /* ERROR 帧（code = NCP 的错误码） */
    ZH_ASH_EV_TIMEOUTS,       /* ACK 连续超时到 max_timeouts：链路断了 */
} zh_ash_event_t;

typedef void (*zh_ash_frame_cb)(void *ctx, const uint8_t *data, size_t len);
typedef void (*zh_ash_event_cb)(void *ctx, zh_ash_event_t ev, uint8_t code);

typedef struct {
    uint32_t tx_frames, tx_data, tx_redata, tx_ack, tx_nak, tx_rst;
    uint32_t rx_frames, rx_data, rx_ack, rx_nak, rx_dup, rx_oos, rx_cancelled, rx_nflag;
    uint32_t rx_crc, rx_comm, rx_short, rx_long, rx_bad_ctrl, rx_bad_len, rx_bad_ack;
    uint32_t ack_timeouts, resets, ncp_resets, ncp_errors;
} zh_ash_stats_t;

typedef enum { ZH_ASH_DOWN = 0, ZH_ASH_RESETTING, ZH_ASH_CONNECTED } zh_ash_state_t;

typedef struct {
    uint8_t len;
    uint8_t data[ZH_ASH_DATA_MAX];
} zh_ash_qent_t;

typedef struct {
    zh_ash_cfg_t    cfg;
    zh_ash_frame_cb on_frame;
    zh_ash_event_cb on_event;
    void           *ctx;
    zh_ash_state_t  state;
    uint16_t        flags;
    /* 帧号（模 8）：frm_tx 下一个要发的新帧、ack_rx 对方确认到哪、frm_retx 重传到哪、frm_rx 下一个期望收的、ack_tx 最后发出去的 ackNum */
    uint8_t         frm_tx, ack_rx, frm_retx, frm_rx, ack_tx, frm_head;
    uint8_t         timeouts;
    /* 发送队列：q_start 起依次是帧号 frm_head、frm_head+1 …（先是已发未确认的，后是等着发的） */
    zh_ash_qent_t   q[ZH_ASH_TXQ];
    uint8_t         q_start, q_len;
    /* ACK / RSTACK 定时器 */
    bool            ack_running;
    uint32_t        ack_start_ms;
    uint16_t        ack_period;
    /* 接收解码 */
    uint8_t         rx_raw[ZH_ASH_RAW_MAX];
    uint16_t        rx_len;          /* 已收的原始字节数（含 CRC；超长时继续计数、不再存） */
    bool            rx_esc, rx_sub;
    uint8_t         rx_last_ctrl;    /* 上一个坏帧 / 好帧的控制字节：决定要不要回 NAK（只对 DATA） */
    zh_ash_stats_t  st;
} zh_ash_t;

void zh_ash_cfg_default(zh_ash_cfg_t *c);
/* 清零并装配置 / 回调（cfg 为 NULL 用默认）。之后先 zh_ash_reset() 才能用。 */
void zh_ash_init(zh_ash_t *a, const zh_ash_cfg_t *cfg, zh_ash_frame_cb on_frame, zh_ash_event_cb on_event, void *ctx);
/* 复位链路：清掉帧号、队列、定时器，下一次 zh_ash_tx() 发 CAN + RST，等 RSTACK（rst_timeout_ms）。 */
void zh_ash_reset(zh_ash_t *a);
/* 停下：清掉帧号、队列、定时器、半截的接收，回到 DOWN —— 不报事件、不清统计、什么都不发（上层退避期间让链路安静，
   之后再 zh_ash_reset）。 */
void zh_ash_stop(zh_ash_t *a);
/* 排一个 EZSP 帧（明文，3..220 字节；随机化在这里做）。没连上 / 队列满 / 长度不对返回 false。 */
bool zh_ash_send(zh_ash_t *a, const uint8_t *data, size_t len);
/* UART 收到的字节。DATA 帧去随机化后经 on_frame 交出；事件经 on_event。 */
void zh_ash_rx(zh_ash_t *a, const uint8_t *buf, size_t n, uint32_t now_ms);
/* 先跑定时器（ACK 超时 → 重传 / 判断链路；RSTACK 超时），再把要写 UART 的字节写进 out（只写整帧；cap 至少 ZH_ASH_WIRE_MAX
   才保证能写出一帧）。返回写了多少字节，0 = 现在没东西要发。 */
size_t zh_ash_tx(zh_ash_t *a, uint32_t now_ms, uint8_t *out, size_t cap);
/* 有没有现在就该写 UART 的东西（RST / CAN / ACK / NAK / 重传 / 窗口内待发的 DATA） */
bool     zh_ash_tx_pending(const zh_ash_t *a);
/* 调用方最多能阻塞多久就该再调一次 zh_ash_tx()：有待发的返回 0；否则是定时器剩余的毫秒；都没有返回 UINT32_MAX */
uint32_t zh_ash_next_timeout_ms(const zh_ash_t *a, uint32_t now_ms);
/* 已发未确认 + 等着发的帧数（上层判断能不能再排） */
static inline unsigned zh_ash_queued(const zh_ash_t *a) { return a->q_len; }
static inline bool zh_ash_connected(const zh_ash_t *a) { return a->state == ZH_ASH_CONNECTED; }

/* 单独的编解码工具（主机测试也用）：CRC-CCITT、随机化、把一帧编成线上字节（含 FLAG，不含前导 CAN） */
uint16_t zh_ash_crc16(const uint8_t *buf, size_t len);
void     zh_ash_randomize(uint8_t *buf, size_t len);
size_t   zh_ash_encode(uint8_t ctrl, const uint8_t *data, size_t len, uint8_t *out, size_t cap);
