/* components/thread_br/include/thread_br_diag.h — 诊断构建专用：802.15.4 帧挂起表压测、SRP 注入 / 注册潮（thread_br_diag.c）。
 * 单独一个头：thread_br.h 要能在主机上编译（test_host/），不能依赖 sdkconfig.h。 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

#if CONFIG_BITENGINE_DIAG_THREAD_PENDING
#define THREAD_BR_DIAG_MAX_CHILDREN 4
typedef struct {
    uint16_t rloc16;
    bool     rx_on_idle;
    bool     csl_synced;     /* CSL 同步的子设备收下行不经过挂起表 */
    uint16_t queued;         /* OT 给它排着的间接消息数 */
    uint16_t frame_err;      /* 0xffff = 100%，OT 自己统计的发往它的帧错误率 */
    uint16_t msg_err;
    uint32_t age;            /* 秒：多久没听到它 */
    uint32_t timeout;        /* 秒：子设备超时 */
} thread_br_diag_child_t;
typedef struct {
    int      fakes;                /* 这次确认在表里的假条目数（add 对已在表里的地址也回成功，所以这是实数） */
    uint32_t scan_miss_cycles;     /* 未命中扫满一张表的 CPU 周期（200 次取最小，下界） */
    int      pending_mode;         /* esp_ieee802154_pending_mode_t */
    uint32_t rx_total, rx_duplicated, tx_total, tx_retry, tx_err_abort,
             tx_direct_max_retry, tx_indirect_max_retry;   /* otMacCounters 快照 */
    int      n_children;           /* child[] 里实际填了几个（≤ THREAD_BR_DIAG_MAX_CHILDREN） */
    int      children_total;       /* 子设备总数；> n_children 时回包里的列表被截断了 */
    thread_br_diag_child_t child[THREAD_BR_DIAG_MAX_CHILDREN];
} thread_br_diag_pending_t;
/* want < 0：只读（fakes 回 -1）；0..CONFIG_IEEE802154_PENDING_TABLE_SIZE：把假条目调到正好 want 个
   （无状态：每次都对前 want 个 add、其余 clear，不依赖上一次记了多少，Thread 重启清过表也照样对）。
   ESP_ERR_INVALID_STATE = Thread 没在跑；ESP_ERR_TIMEOUT = 抢不到 OT 锁；
   ESP_ERR_NO_MEM = 表满、没加够（out 照样填好）。 */
esp_err_t thread_br_diag_pending(int want, thread_br_diag_pending_t *out);
#endif

#if CONFIG_BITENGINE_DIAG_SRP_INJECT
/* SRP UPDATE 注入（扩容实验 D 方案的传输层；thread_br_diag.c 下半段）。
   把 req[0..len) 原样当 UDP 载荷，经 OT 内部 UDP 栈发给本机 SRP server（目的 = 本机 ML-EID : SRP 端口），
   等同 DNS 报文 ID 的应答，原样拷进 rsp（超过 rsp_cap 截断，*rsp_len 是拷进去的字节数）。
   一次只处理一条（内部互斥）；等应答时不持 OT 锁。
   ESP_ERR_INVALID_ARG   参数非法（len 不在 [12, THREAD_BR_DIAG_SRP_MAX]、指针为空、timeout_ms ≤ 0）
   ESP_ERR_INVALID_STATE Thread 没在跑、SRP server 不在 running 状态，或注册潮正在跑
   ESP_ERR_TIMEOUT       抢不到 OT 锁 / 等不到应答
   ESP_ERR_NO_MEM        OT 消息缓冲或信号量建不起来
   ESP_FAIL              socket 打不开或 otUdpSend 失败 */
#define THREAD_BR_DIAG_SRP_MAX 1280   /* IPv6 最小 MTU；srp_load.py 不做名字压缩，一条注册约 1 KB */
esp_err_t thread_br_diag_srp_inject(const uint8_t *req, size_t len,
                                    uint8_t *rsp, size_t rsp_cap, size_t *rsp_len, int timeout_ms);

/* 注册潮：按时间表把一批 SRP UPDATE 发给本机 SRP server，模拟网关重启后设备在 10 s 抖动窗口里
   成批重注册（OT SRP client 的 kMaxTxJitterOnServerRestart）。单条注入一来一回，CPU 有空档，
   看不到 OT 任务 / mdns 任务被连续占满 —— 而那正是 2026-10-07 任务看门狗 panic 的形状。
   rec 是若干条记录首尾相接：[u32 大端 计划发送时刻 ms，相对开始][u16 大端 长度][该条 UPDATE]；
   计划时刻不许倒退、不超过 600 s；各条的 DNS ID 必须是连续的 id0, id0+1, …（按 ID 直接定位，回调里 O(1)）。
   成功时接管 rec（PSRAM 缓冲，发完由本模块释放）；失败时 rec 仍归调用方。
   在后台常驻任务里跑，立即返回；进度与结果用 thread_br_diag_srp_burst_status() 读。
   ESP_ERR_INVALID_ARG 记录格式不对 / 条数超限；ESP_ERR_INVALID_STATE 上一轮还没完或 Thread 没在跑；
   ESP_ERR_NO_MEM 任务 / 结果表建不起来。 */
#define THREAD_BR_DIAG_BURST_MAX 1024
typedef struct {
    bool     running;
    uint32_t n, sent, send_fail, retries, responded;
    uint32_t rcode[16];             /* 按 RCODE 计数：0 成功，2 SERVFAIL（多半是 mdns 队列满），5 REFUSED */
    uint32_t elapsed_ms;            /* 从第一条开始算 */
    uint32_t lat_p50_ms, lat_p95_ms, lat_max_ms;   /* 送出 → 收到应答（结束后才算） */
    uint32_t send_lag_max_ms;       /* 实际送出比计划晚多少：OT 忙 / 缓冲满时发不出去 */
    uint32_t ot_lock_wait_max_ms;   /* 拿 OT 锁最多等了多久 ≈ OT 任务最长一次连续占用（卡在 mdns 里也算） */
    uint32_t idle_gap_max_ms;       /* IDLE 任务最长多久没被调度到；任务看门狗 5 s 就 panic */
} thread_br_diag_burst_t;
esp_err_t thread_br_diag_srp_burst_start(uint8_t *rec, size_t len, uint32_t timeout_ms);
void      thread_br_diag_srp_burst_status(thread_br_diag_burst_t *out);
/* 第 i 条（0 起）的计划 / 实际送出 / 收到应答时刻（ms，相对开始；-1 = 没有）与 RCODE（-1 = 没应答）。
   i 越界返回 false。结果保留到下一轮开始。 */
bool      thread_br_diag_srp_burst_slot(uint32_t i, int32_t *at_ms, int32_t *sent_ms, int32_t *rsp_ms, int *rcode);

/* SRP 代播调的 mdns 写接口各自的调用次数、累计 / 最长墙钟（srp_mdns_wrap.c，链接期 --wrap 截住）。
   注册潮开始时清零。回填了几项（≤ max）。 */
typedef struct {
    const char *name;
    uint32_t    n, total_us, max_us;
} thread_br_diag_mdns_call_t;
size_t thread_br_diag_mdns_calls(thread_br_diag_mdns_call_t *out, size_t max);
/* mdns 任务里各类动作（mdns_action_type_t）的次数、累计 / 最长耗时；与上面一起在注册潮开始时清零 */
size_t thread_br_diag_mdns_actions(thread_br_diag_mdns_call_t *out, size_t max);
void   thread_br_diag_mdns_calls_reset(void);
#endif

#if CONFIG_BITENGINE_DIAG_ZB_SNIFF
/* Zigbee 信道抓包（zb_sniff.c 头注）：录制期间 Thread 暂停。主机侧解码 tools/zb_sniff.py。 */
#define ZB_SNIFF_MAX          400     /* 帧数上限，PSRAM 约 54 KB */
#define ZB_SNIFF_MAX_SECONDS  60
typedef struct {
    uint32_t t_ms;        /* 相对录制开始 */
    int8_t   rssi;
    uint8_t  lqi;
    uint8_t  channel;
    uint8_t  len;         /* PSDU 长度，含末尾 2 字节 FCS */
    uint8_t  psdu[127];
} zb_sniff_frame_t;
typedef enum {
    THREAD_BR_ZB_SNIFF_IDLE = 0,
    THREAD_BR_ZB_SNIFF_RUNNING,
    THREAD_BR_ZB_SNIFF_DONE,
    THREAD_BR_ZB_SNIFF_ERROR,
} thread_br_zb_sniff_state_t;
typedef struct {
    thread_br_zb_sniff_state_t state;
    uint8_t   channel;
    uint16_t  seconds;
    uint16_t  count;
    uint32_t  dropped;
    uint32_t  tx_seen;
    char      err[40];
    const zb_sniff_frame_t *frames;   /* 录制结束后才非空 */
} thread_br_zb_sniff_status_t;
/* ESP_ERR_INVALID_ARG 信道/时长越界；ESP_ERR_INVALID_STATE Thread 从没起来过；ESP_ERR_NOT_FINISHED 正在录 */
esp_err_t thread_br_zb_sniff_start(uint8_t channel, uint16_t seconds);
void      thread_br_zb_sniff_status(thread_br_zb_sniff_status_t *out);
#endif
