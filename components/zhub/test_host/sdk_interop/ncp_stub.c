/* 主机上跑 SDK 的 NCP 侧 ASH（ash-ncp.c + ash-common.c + legacy_hal crc.c）用的环境替身。
 * 和那三份 SDK 源文件一起编成 libsdkncp.so；测试（ncp_interop.c）dlopen 它，收到 RST 时 NCP 会调 halReboot() ——
 * 这里 longjmp 回 ncp_poll()，由测试 dlclose / dlopen 重新加载，相当于真重启（SDK 的 ashStart() 不清自己的静态变量，
 * 见 ash-ncp.c:234 的 #if 0，靠的就是上电清 RAM）。
 *
 * 只实现 ash-ncp.c 用到的那几个函数，语义按它的用法来：
 *   - 缓冲 = 池里的一项，数据连续存放；"链接缓冲"按偏移直接取（PACKET_BUFFER_SIZE 只影响随机化分段，结果一样）。
 *   - 队列 FIFO：head 是最早进的（ashMessageBufferQueueNthEntry 从 head 往后数，N=0 是 frmReTxHead 那帧）。
 *   - hold / release 计引用；间接缓冲（ashAllocateStaticBuffers 那块）引用到 0 也不回收。
 *   - 应答**原地写回命令所在的那块缓冲**再 ashSend（同真 NCP 的 em260 serial-interface-uart.c：命令缓冲就是应答缓冲）。
 *     ash-ncp.c 在下一帧开始时就把那块标成空闲（ashReleaseRxDataBuffer），主机此时重传命令会覆盖还没发完的应答 ——
 *     NCP 会把主机自己的命令当应答发回来。这是真 NCP 的行为，EZSP 层要扛（第一轮审计；设计 8.4）。回调用另外的缓冲。 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#include "stack/include/sl_zigbee.h"
#include "hal/hal.h"
#include "serial/serial.h"
#include "app/util/ezsp/ezsp-protocol.h"   /* SL_ZIGBEE_EZSP_* 状态码（ezsp-enum.h） */
#include "ash-ncp.h"                       /* ashStart / ashSendExec / ashSend / ashReceive */

#define POOL 32
typedef struct {
    bool     used, indirect;
    int      ref;
    uint8_t  len;
    uint8_t *data;
    uint8_t  own[256];
    sli_buffer_manager_buffer_t qnext;
} ent_t;
static ent_t s_pool[POOL];

static uint8_t  s_rx[1 << 16];
static size_t   s_rx_r, s_rx_w;
static uint8_t  s_tx[1 << 16];
static size_t   s_tx_n;
static uint16_t s_now;
static uint8_t  s_reset_reason = 2;
static jmp_buf  s_jmp;
static bool     s_in_poll;
static uint8_t  s_cmd[256];
static size_t   s_cmd_len;
static uint32_t s_cmds;
static bool     s_booting;      /* ashStart() 里会忙等"重启延时"定时器：只在这期间让时钟自己走 */
static sli_buffer_manager_buffer_t s_held = SL_ZIGBEE_NULL_MESSAGE_BUFFER;   /* 刚收到的命令所在的缓冲：应答原地写回它 */
static bool     s_inplace = true;   /* false：应答另开缓冲（理想化的 NCP 应用层，只给 k>1 的流水线测试用：em260 一次只能处理一条命令） */

static ent_t *E(sli_buffer_manager_buffer_t b)
{
    if (b >= POOL || !s_pool[b].used) { fprintf(stderr, "ncp_stub: 坏缓冲 %u\n", b); abort(); }
    return &s_pool[b];
}

static sli_buffer_manager_buffer_t alloc_ent(void)
{
    for (sli_buffer_manager_buffer_t i = 0; i < POOL; i++) {
        if (!s_pool[i].used) {
            memset(&s_pool[i], 0, sizeof(s_pool[i]));
            s_pool[i].used = true;
            s_pool[i].ref = 1;
            s_pool[i].data = s_pool[i].own;
            s_pool[i].qnext = SL_ZIGBEE_NULL_MESSAGE_BUFFER;
            return i;
        }
    }
    fprintf(stderr, "ncp_stub: 缓冲池满\n");
    abort();
}

/* ---- 缓冲 ---- */
sli_buffer_manager_buffer_t sli_legacy_buffer_manager_allocate_indirect_buffer(uint8_t *contents, void *freePtr, uint16_t length)
{
    (void)freePtr;
    const sli_buffer_manager_buffer_t b = alloc_ent();
    s_pool[b].indirect = true;
    s_pool[b].data = contents;
    s_pool[b].len = (uint8_t)(length > 255 ? 255 : length);
    return b;
}
uint8_t  sl_legacy_buffer_manager_message_buffer_length(sli_buffer_manager_buffer_t b) { return E(b)->len; }
void     sl_legacy_buffer_manager_set_message_buffer_length(sli_buffer_manager_buffer_t b, uint8_t len) { E(b)->len = len; }
uint8_t *sl_legacy_buffer_manager_message_buffer_contents(sli_buffer_manager_buffer_t b) { return E(b)->data; }
void     sl_legacy_buffer_manager_copy_to_linked_buffers(const uint8_t *from, sli_buffer_manager_buffer_t b, uint8_t start, uint8_t n) { memcpy(E(b)->data + start, from, n); }
void     sl_legacy_buffer_manager_set_linked_buffers_byte(sli_buffer_manager_buffer_t b, uint8_t i, uint8_t v) { E(b)->data[i] = v; }
uint8_t  sl_legacy_buffer_manager_get_linked_buffers_byte(sli_buffer_manager_buffer_t b, uint8_t i) { return E(b)->data[i]; }
uint8_t *sl_legacy_buffer_manager_get_linked_buffers_pointer(sli_buffer_manager_buffer_t b, uint8_t i) { return E(b)->data + i; }
void     sl_legacy_buffer_manager_hold_message_buffer(sli_buffer_manager_buffer_t b) { E(b)->ref++; }
void     sl_legacy_buffer_manager_release_message_buffer(sli_buffer_manager_buffer_t b)
{
    ent_t *e = E(b);
    if (--e->ref <= 0 && !e->indirect) e->used = false;
}

/* ---- 队列（tail 指最新一项，tail->qnext 是最早一项：环形） ---- */
bool sli_legacy_packet_buffer_queue_is_empty(sli_zigbee_message_buffer_queue_t *q) { return q->tail == SL_ZIGBEE_NULL_MESSAGE_BUFFER; }
void sli_legacy_packet_buffer_queue_add(sli_zigbee_message_buffer_queue_t *q, sli_buffer_manager_buffer_t b)
{
    /* 同一块缓冲还挂在某个队列上又被入队 = em260 原地应答撞缓冲：主机在 NCP 的旧应答还没送到时发了新命令，新命令写进同一块静态
       接收缓冲，应答又原地写回、ashSend —— 队列的链接被弄乱（会话层的应答超时就是为了不让这件事发生，zh_ezsp_ses.c 的 cfg_default）。
       当场报错，别让它在后面以"坏缓冲"的样子出现 */
    if (E(b)->qnext != SL_ZIGBEE_NULL_MESSAGE_BUFFER) { fprintf(stderr, "ncp_stub: 缓冲 %u 还在队列里又被入队（原地应答撞缓冲）\n", b); abort(); }
    if (q->tail == SL_ZIGBEE_NULL_MESSAGE_BUFFER) {
        E(b)->qnext = b;
    } else {
        E(b)->qnext = E(q->tail)->qnext;
        E(q->tail)->qnext = b;
    }
    q->tail = b;
}
sli_buffer_manager_buffer_t sli_legacy_buffer_manager_buffer_queue_head(sli_zigbee_message_buffer_queue_t *q)
{
    return q->tail == SL_ZIGBEE_NULL_MESSAGE_BUFFER ? SL_ZIGBEE_NULL_MESSAGE_BUFFER : E(q->tail)->qnext;
}
sli_buffer_manager_buffer_t sli_legacy_buffer_manager_buffer_queue_next(sli_zigbee_message_buffer_queue_t *q, sli_buffer_manager_buffer_t b)
{
    return b == q->tail ? SL_ZIGBEE_NULL_MESSAGE_BUFFER : E(b)->qnext;
}
sli_buffer_manager_buffer_t sli_legacy_packet_buffer_queue_remove_head(sli_zigbee_message_buffer_queue_t *q)
{
    const sli_buffer_manager_buffer_t h = sli_legacy_buffer_manager_buffer_queue_head(q);
    if (h == SL_ZIGBEE_NULL_MESSAGE_BUFFER) return h;
    if (h == q->tail) q->tail = SL_ZIGBEE_NULL_MESSAGE_BUFFER;
    else E(q->tail)->qnext = E(h)->qnext;
    E(h)->qnext = SL_ZIGBEE_NULL_MESSAGE_BUFFER;
    return h;
}

/* ---- 串口 / 时间 / 复位 ---- */
sl_status_t sli_legacy_serial_write_byte(uint8_t port, uint8_t b)
{
    (void)port;
    if (s_tx_n < sizeof(s_tx)) s_tx[s_tx_n++] = b;
    return SL_STATUS_OK;
}
sl_status_t sli_legacy_serial_read_byte(uint8_t port, uint8_t *b)
{
    (void)port;
    if (s_rx_r == s_rx_w) return SL_STATUS_EMPTY;
    *b = s_rx[s_rx_r++ % sizeof(s_rx)];
    return SL_STATUS_OK;
}
uint16_t sli_legacy_serial_read_available(uint8_t port) { (void)port; return (uint16_t)(s_rx_w - s_rx_r); }
uint16_t halCommonGetInt16uMillisecondTick(void) { return s_now; }
uint32_t halCommonGetInt32uMillisecondTick(void) { return s_now; }
uint8_t  halGetEm2xxResetInfo(void) { return s_reset_reason; }
void     halResetWatchdog(void) { }
void     simulatedTimePasses(void) { if (s_booting) s_now++; }
bool     callbackPending(void) { return false; }
void     halReboot(void)
{
    if (s_in_poll) longjmp(s_jmp, 1);
    fprintf(stderr, "ncp_stub: halReboot 不在 ncp_poll 里\n");
    abort();
}

/* ---- 给测试的接口（dlsym 取） ---- */
void ncp_boot(uint8_t reset_reason)
{
    s_reset_reason = reset_reason;
    s_booting = true;                /* ashStart() 等 rebootDelay（ash-ncp.c:255-258）：忙等里时间得往前走，不然永远等不完 */
    ashStart();
    s_booting = false;
}
void ncp_set_time(uint32_t ms) { s_now = (uint16_t)ms; }
void ncp_feed(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) s_rx[s_rx_w++ % sizeof(s_rx)] = b[i];
}
size_t ncp_drain(uint8_t *out, size_t cap)
{
    const size_t n = s_tx_n < cap ? s_tx_n : cap;
    memcpy(out, s_tx, n);
    memmove(s_tx, s_tx + n, s_tx_n - n);
    s_tx_n -= n;
    return n;
}
/* 跑一轮：先发、再收。返回 0 = 什么都没收到，1 = 收到一条命令（ncp_take_cmd 取），2 = NCP 要重启（收到了 RST） */
int ncp_poll(void)
{
    s_in_poll = true;
    if (setjmp(s_jmp)) {
        s_in_poll = false;
        return 2;
    }
    ashSendExec();
    sli_buffer_manager_buffer_t b = SL_ZIGBEE_NULL_MESSAGE_BUFFER;
    const sl_zigbee_ezsp_status_t st = ashReceive(&b);
    int ret = 0;
    if (st == SL_ZIGBEE_EZSP_SUCCESS && b != SL_ZIGBEE_NULL_MESSAGE_BUFFER) {
        s_cmd_len = E(b)->len;
        memcpy(s_cmd, E(b)->data, s_cmd_len);
        if (s_held != SL_ZIGBEE_NULL_MESSAGE_BUFFER) sl_legacy_buffer_manager_release_message_buffer(s_held);   /* 上一条没应答的 */
        s_held = SL_ZIGBEE_NULL_MESSAGE_BUFFER;
        if (s_inplace) s_held = b;                             /* ashReceiveFrame 已 hold；应答时原地写回、ashSend，确认后 scrub 释放 */
        else sl_legacy_buffer_manager_release_message_buffer(b);   /* 命令已拷走：平衡那次 hold */
        s_cmds++;
        ret = 1;
    }
    ashSendExec();
    s_in_poll = false;
    return ret;
}
size_t ncp_take_cmd(uint8_t *out) { memcpy(out, s_cmd, s_cmd_len); return s_cmd_len; }
/* 命令应答：原地写回命令那块缓冲再 ashSend（同 em260 serialResponseReady；ashSend 会就地随机化） */
void ncp_reply(const uint8_t *data, size_t len)
{
    if (s_held == SL_ZIGBEE_NULL_MESSAGE_BUFFER) { fprintf(stderr, "ncp_stub: 没有可写回的命令缓冲\n"); abort(); }
    memcpy(E(s_held)->data, data, len);
    E(s_held)->len = (uint8_t)len;
    ashSend(s_held, false);
    s_held = SL_ZIGBEE_NULL_MESSAGE_BUFFER;
}
/* NCP 主动发的一帧（回调）：拷进自己的缓冲再交给 ASH */
void ncp_send(const uint8_t *data, size_t len, bool high)
{
    const sli_buffer_manager_buffer_t b = alloc_ent();
    memcpy(s_pool[b].own, data, len);
    s_pool[b].len = (uint8_t)len;
    ashSend(b, high);
}
uint32_t ncp_cmd_count(void) { return s_cmds; }
void ncp_set_inplace(bool on) { s_inplace = on; }
