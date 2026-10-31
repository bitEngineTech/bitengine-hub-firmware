/* components/thread_br/thread_br_diag.c — 诊断构建专用，两块各自一个 Kconfig、出厂都是 n（出厂固件里这个文件编出来是空的）：
 *   上半：802.15.4 帧挂起表压测（CONFIG_BITENGINE_DIAG_THREAD_PENDING）
 *   下半：SRP UPDATE 注入（CONFIG_BITENGINE_DIAG_SRP_INJECT）：单条一来一回 + 注册潮（成批按时间表发），说明在下半段开头
 *
 * 为什么有它（2026-10-06）：挂起表扩到 ≥ 子设备上限之后，每个要 ACK 的帧都在 802.15.4 中断里线性
 * 扫满整张表（OT 端口用 ENHANCED 模式，不只 data poll）。现网只有一个休眠子设备，表几乎总是空的 ——
 * 不往里塞东西，就量不到"满表时 ACK 还赶不赶得上"。做法：塞入 router ID 63 段（RLOC16
 * 0xFC00–0xFFFF，OT 永不分配，kInvalidRouterId）的假短地址，真实帧不可能命中。
 * 步骤、判据与 10-06 的结果只写在 docs/test_cases_scale_baseline.md「802.15.4 帧挂起表满表压测」一处。
 *
 * 读数怎么用（别读过头）：
 *   - MAC "收到重复帧"只能说明 ACK 有没有发出去；ACK 里的挂起位（FP）是否及时，要看对休眠子设备的
 *     端到端 ping —— 日志里的 ack-fp 是软件算出的值，空口没赶上也照样显示，不能当判据；
 *   - scan_miss_cycles 是任务上下文里 clear(不存在的地址) 的耗时，循环与中断里的查表同构，但不含
 *     中断路径上的其余工作，所以只是整个 RX-done 中断开销的下界；
 *   - 只塞短地址：长地址表每个占用格要 memcmp 8 字节，满表的最坏扫描没测。
 *
 * ⚠️ 绝不调 esp_ieee802154_reset_pending_table()：那会连真子设备的条目一起清掉，而 OT 以为还在 ——
 *    等于人为制造"下行卡死"。只逐条加/删自己的假地址。thread_br/VERIFY.sh 全仓禁止那个调用。
 * ⚠️ 必须持 OT 锁：add 是"找第一个空位再写"，与 OT 自己的加表并发会抢同一格、覆盖真条目。
 *    中断只读表；add 先写地址后置位、clear 只清位，与中断并发是安全的。
 * ⚠️ 与 thread_br_stop() 之间不互斥（先查 is_running 再取 OT 锁，与 otbr_api 的读路径同形），诊断专用、可以接受。
 */
#include "sdkconfig.h"
#include "thread_br.h"
#include "thread_br_diag.h"

#if CONFIG_BITENGINE_DIAG_THREAD_PENDING
#include <string.h>
#include "esp_cpu.h"
#include "esp_ieee802154.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "freertos/FreeRTOS.h"
#include "openthread/link.h"
#include "openthread/thread_ftd.h"

#define FAKE_BASE      0xFD01u   /* router ID 63 段，永不分配 */
#define FAKE_PROBE     0xFDFFu   /* 计时用的"不存在的地址"：从不加进表（FAKE_BASE + 254，table_size ≤ 100 碰不到） */
#define TIMING_ROUNDS  200
#define OT_LOCK_MS     2000

esp_err_t thread_br_diag_pending(int want, thread_br_diag_pending_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (want > CONFIG_IEEE802154_PENDING_TABLE_SIZE) return ESP_ERR_INVALID_ARG;
    if (!thread_br_is_running()) return ESP_ERR_INVALID_STATE;
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(OT_LOCK_MS))) return ESP_ERR_TIMEOUT;

    memset(out, 0, sizeof(*out));
    esp_err_t err = ESP_OK;
    out->fakes = -1;
    if (want >= 0) {
        /* 无状态：前 want 个都 add（已在表里的 add 也回 ESP_OK，幂等），其余都 clear（不在表里就回失败，忽略）。
           这样 fakes 数的是这一次确认在表里的个数，不依赖上一次记了多少 —— Thread 重启清过表也照样对。 */
        int present = 0;
        for (int i = 0; i < CONFIG_IEEE802154_PENDING_TABLE_SIZE; i++) {
            /* 与 OT 端口同形：uint16_t 按本机字节序传（esp_openthread_radio.c 的 AddSrcMatchShortEntry） */
            uint16_t a = (uint16_t)(FAKE_BASE + i);
            if (i < want) {
                if (esp_ieee802154_add_pending_addr((const uint8_t *)&a, true) == ESP_OK) present++;
                else err = ESP_ERR_NO_MEM;     /* 表满：剩下的格子被真子设备占着 */
            } else {
                (void)esp_ieee802154_clear_pending_addr((const uint8_t *)&a, true);
            }
        }
        out->fakes = present;
    }

    /* 扫描耗时下界：clear(不存在的地址) 的循环与中断里的查表同构，找不到就什么都不改 */
    uint32_t best = UINT32_MAX;
    const uint16_t probe = FAKE_PROBE;
    for (int i = 0; i < TIMING_ROUNDS; i++) {
        uint32_t c0 = esp_cpu_get_cycle_count();
        (void)esp_ieee802154_clear_pending_addr((const uint8_t *)&probe, true);
        uint32_t c = esp_cpu_get_cycle_count() - c0;
        if (c < best) best = c;
    }
    out->scan_miss_cycles = best;
    out->pending_mode     = (int)esp_ieee802154_get_pending_mode();

    otInstance *inst = esp_openthread_get_instance();
    const otMacCounters *mc = otLinkGetCounters(inst);
    out->rx_total              = mc->mRxTotal;
    out->rx_duplicated         = mc->mRxDuplicated;
    out->tx_total              = mc->mTxTotal;
    out->tx_retry              = mc->mTxRetry;
    out->tx_err_abort          = mc->mTxErrAbort;
    out->tx_direct_max_retry   = mc->mTxDirectMaxRetryExpiry;
    out->tx_indirect_max_retry = mc->mTxIndirectMaxRetryExpiry;

    uint16_t max = otThreadGetMaxAllowedChildren(inst);
    for (uint16_t i = 0; i < max; i++) {
        otChildInfo ci;
        if (otThreadGetChildInfoByIndex(inst, i, &ci) != OT_ERROR_NONE) continue;
        out->children_total++;
        if (out->n_children >= THREAD_BR_DIAG_MAX_CHILDREN) continue;   /* 列表满了只计数，回包里有 children_total */
        thread_br_diag_child_t *c = &out->child[out->n_children++];
        c->rloc16     = ci.mRloc16;
        c->rx_on_idle = ci.mRxOnWhenIdle;
        c->csl_synced = ci.mIsCslSynced;   /* CSL 同步的子设备收下行不经过挂起表，压测要排除它 */
        c->queued     = ci.mQueuedMessageCnt;
        c->frame_err  = ci.mFrameErrorRate;
        c->msg_err    = ci.mMessageErrorRate;
        c->age        = ci.mAge;
        c->timeout    = ci.mTimeout;
    }

    esp_openthread_lock_release();
    return err;
}
#endif /* CONFIG_BITENGINE_DIAG_THREAD_PENDING */

/* ============================================================================================
 * SRP UPDATE 注入（CONFIG_BITENGINE_DIAG_SRP_INJECT，默认 n）—— 扩容实验 D 方案的传输层。
 *
 * 为什么有它（2026-10-07）：Matter-over-Thread 的设备数被 mDNS 代播条数卡着，要抬上限就得在几百台的规模上验
 * "SRP server → advertising proxy → mDNS → 局域网"这条真实链路（含代播跟踪任务那一轮扫描的耗时）。
 * 之前的合成负载（mdns_load）绕过了 SRP；而从局域网发往网关自身 OMR 地址的 SRP 包进不了 OT 的 UDP 栈
 * （tools/srp_load.py 头注里的证据链）。OT 把"目的是本机单播地址"的包直接交给本机 socket
 * （ip6.cpp DetermineAction：HasUnicastAddress(dst) → aReceive），所以从 OT 内部发给自己的 SRP 端口就能进去。
 *
 * H1（openthread/udp.h、message.h，ESP-IDF v6.0.2 自带的 OT）：
 *   1. otUdpSend 成功即接管 message，失败时调用方负责 otMessageFree；otMessageInfo 只填 mPeerAddr/mPeerPort，
 *      源地址留空由 OT 选（本机 ML-EID）。
 *   2. 回调 otUdpReceive(ctx, msg, info) 在 OT 任务里、持 OT 锁调用；msg 归 OT 所有，回调里只读不释放。
 *      UDP 载荷从 otMessageGetOffset(msg) 开始，长度 = otMessageGetLength − offset。
 *   3. otUdpOpen ↔ otUdpClose 成对。这里开一次、跟着 OT 实例活：Thread 重启后实例里的 socket 链表是空的，
 *      otUdpIsOpen 回 false，下次调用时重开（旧的那份随旧实例作废，不能也不必 close）。
 *      绑 OT_NETIF_THREAD_INTERNAL：不走平台 UDP，只在 OT 内部收发。
 *   4. DNS 报文头 12 字节，前 2 字节是 ID（大端）；SRP 应答回同一个 ID —— 拿它把应答和请求对上号，
 *      上一条超时后才到的旧应答就丢掉。
 *
 * S2：调用方是 httpd 任务。s_inj_waiting / s_inj_id 只在持 OT 锁时读写（回调本身就持锁），应答缓冲在回调里写完
 *     再 give 信号量，httpd 侧 take 到之后才读 —— 有先后。等应答时**不持** OT 锁（否则 SRP server 根本跑不了）。
 * S3：信号量第一次调用时建、永不删（诊断构建、只建一次）；PSRAM 里的缓冲由调用方给。
 * ⚠️ 与 thread_br_stop() 之间只做了"先查 is_running 再取锁"（同上半段、同 otbr_api 的读路径），诊断专用、可以接受。
 * ============================================================================================ */
#if CONFIG_BITENGINE_DIAG_SRP_INJECT
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "esp_freertos_hooks.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psram_task.h"
#include "openthread/message.h"
#include "openthread/srp_server.h"
#include "openthread/thread.h"
#include "openthread/udp.h"

#define INJ_OT_LOCK_MS 2000

static const char *TAG_INJ = "srp_inject";

static otUdpSocket       s_inj_sock;
static SemaphoreHandle_t s_inj_mutex;      /* 一次只注入一条（注册潮不拿它，见 burst_task 的注释） */
static SemaphoreHandle_t s_inj_done;       /* 应答到了 */
static bool              s_inj_waiting;    /* 持 OT 锁读写 */
static uint16_t          s_inj_id;         /* 持 OT 锁读写 */
static uint8_t           s_inj_rsp[THREAD_BR_DIAG_SRP_MAX];
static size_t            s_inj_rsp_len;

/* ---- 注册潮的状态（下面 burst_* 那一段）。回调要用，所以声明在前 ---- */
typedef struct {
    uint32_t       at_ms;               /* 计划送出时刻 */
    int32_t        sent_ms, rsp_ms;     /* -1 = 没有 */
    int8_t         rcode;               /* -1 = 没应答 */
    uint16_t       len;
    const uint8_t *msg;                 /* 指进 s_b.rec；发完整轮后 rec 释放，此指针作废、不再用 */
} burst_slot_t;
static struct {
    volatile bool  running;             /* 任务置、任务清；GET 只读 */
    bool           active;              /* 回调按注册潮匹配应答；持 OT 锁读写 */
    uint8_t       *rec;                 /* 接管来的记录缓冲（PSRAM） */
    burst_slot_t  *slot;                /* PSRAM，保留到下一轮开始 */
    uint32_t       n, timeout_ms;
    uint16_t       id0;
    int64_t        t0_us;
    volatile uint32_t sent, send_fail, retries, responded;
    volatile uint32_t send_lag_max_ms, ot_lock_wait_max_ms, elapsed_ms;
    uint32_t       rcode[16];
    uint32_t       lat_p50_ms, lat_p95_ms, lat_max_ms;
} s_b;

static void burst_on_rsp(uint16_t id, uint8_t flags_lo)
{
    uint16_t idx = (uint16_t)(id - s_b.id0);            /* 无符号回绕：比 id0 小的 ID 变成大数，被下面挡掉 */
    if (idx >= s_b.n) return;
    burst_slot_t *sl = &s_b.slot[idx];
    if (sl->sent_ms < 0 || sl->rsp_ms >= 0) return;     /* 没发过 / 重复应答 */
    sl->rsp_ms = (int32_t)((esp_timer_get_time() - s_b.t0_us) / 1000);
    sl->rcode  = (int8_t)(flags_lo & 0x0F);
    s_b.rcode[flags_lo & 0x0F]++;
    s_b.responded++;
}

static void srp_inject_rx(void *ctx, otMessage *msg, const otMessageInfo *info)
{
    (void)ctx; (void)info;
    uint16_t off = otMessageGetOffset(msg);
    uint16_t total = otMessageGetLength(msg);
    if (total < off + 12) return;                         /* 连 DNS 头都不够 */
    uint8_t hdr[4];
    if (otMessageRead(msg, off, hdr, sizeof(hdr)) != sizeof(hdr)) return;
    uint16_t id = (uint16_t)((hdr[0] << 8) | hdr[1]);
    if (s_b.active) { burst_on_rsp(id, hdr[3]); return; }
    if (!s_inj_waiting || id != s_inj_id) return;         /* 过期 / 不是我们等的 */
    uint16_t len = total - off;
    if (len > sizeof(s_inj_rsp)) len = sizeof(s_inj_rsp);
    s_inj_rsp_len = otMessageRead(msg, off, s_inj_rsp, len);
    s_inj_waiting = false;
    xSemaphoreGive(s_inj_done);
}

/* 持 OT 锁调用：socket 没开就开（绑 ::、临时端口，只走 Thread 内部 netif） */
static esp_err_t inj_sock_ensure_locked(otInstance *inst)
{
    if (otUdpIsOpen(inst, &s_inj_sock)) return ESP_OK;
    memset(&s_inj_sock, 0, sizeof(s_inj_sock));
    otSockAddr any;
    memset(&any, 0, sizeof(any));                         /* 地址 ::、端口 0 = OT 挑一个临时端口 */
    if (otUdpOpen(inst, &s_inj_sock, srp_inject_rx, NULL) != OT_ERROR_NONE) {
        ESP_LOGW(TAG_INJ, "otUdpOpen failed");
        return ESP_FAIL;
    }
    if (otUdpBind(inst, &s_inj_sock, &any, OT_NETIF_THREAD_INTERNAL) != OT_ERROR_NONE) {
        otUdpClose(inst, &s_inj_sock);
        ESP_LOGW(TAG_INJ, "otUdpBind failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* 持 OT 锁调用：把 buf 当一条 UDP 载荷发给本机 SRP server。ESP_ERR_NO_MEM = OT 消息缓冲不够 */
static esp_err_t inj_send_locked(otInstance *inst, const uint8_t *buf, size_t len)
{
    otMessage *m = otUdpNewMessage(inst, NULL);
    if (!m) return ESP_ERR_NO_MEM;
    if (otMessageAppend(m, buf, (uint16_t)len) != OT_ERROR_NONE) {
        otMessageFree(m);
        return ESP_ERR_NO_MEM;
    }
    otMessageInfo info;
    memset(&info, 0, sizeof(info));
    info.mPeerAddr = *otThreadGetMeshLocalEid(inst);
    info.mPeerPort = otSrpServerGetPort(inst);
    if (otUdpSend(inst, &s_inj_sock, m, &info) != OT_ERROR_NONE) {
        otMessageFree(m);                                 /* 失败时所有权还在我们这 */
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool inj_sync_init(void)
{
    /* 调用方只有 httpd（单任务）：首次建信号量不会并发 */
    if (!s_inj_mutex) s_inj_mutex = xSemaphoreCreateMutex();
    if (!s_inj_done)  s_inj_done  = xSemaphoreCreateBinary();
    return s_inj_mutex && s_inj_done;
}

esp_err_t thread_br_diag_srp_inject(const uint8_t *req, size_t len,
                                    uint8_t *rsp, size_t rsp_cap, size_t *rsp_len, int timeout_ms)
{
    if (!req || !rsp || !rsp_len || len < 12 || len > THREAD_BR_DIAG_SRP_MAX || timeout_ms <= 0)
        return ESP_ERR_INVALID_ARG;
    *rsp_len = 0;
    if (!thread_br_is_running()) return ESP_ERR_INVALID_STATE;
    if (!inj_sync_init()) return ESP_ERR_NO_MEM;

    /* 注册潮在跑：两边共用一个 socket / 回调，不能同时等应答 → 直接回"忙"。两者都只由 httpd（单任务）
       发起，所以"查完 running 之后注册潮才开始"不会发生。 */
    if (s_b.running) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_inj_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t ret = ESP_FAIL;

    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(INJ_OT_LOCK_MS))) { ret = ESP_ERR_TIMEOUT; goto out; }
    otInstance *inst = esp_openthread_get_instance();
    if (!inst || otSrpServerGetState(inst) != OT_SRP_SERVER_STATE_RUNNING) {
        esp_openthread_lock_release();
        ret = ESP_ERR_INVALID_STATE;
        goto out;
    }
    if (inj_sock_ensure_locked(inst) != ESP_OK) { esp_openthread_lock_release(); goto out; }

    s_inj_id = (uint16_t)((req[0] << 8) | req[1]);
    s_inj_waiting = true;
    (void)xSemaphoreTake(s_inj_done, 0);                      /* 清掉上一条超时后迟到的 give */
    ret = inj_send_locked(inst, req, len);
    if (ret != ESP_OK) {
        s_inj_waiting = false;
        esp_openthread_lock_release();
        if (ret != ESP_ERR_NO_MEM) ESP_LOGW(TAG_INJ, "otUdpSend failed");
        goto out;
    }
    esp_openthread_lock_release();                            /* 等应答时不持锁：SRP server 要在 OT 任务里跑 */

    if (xSemaphoreTake(s_inj_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        size_t n = s_inj_rsp_len < rsp_cap ? s_inj_rsp_len : rsp_cap;
        memcpy(rsp, s_inj_rsp, n);
        *rsp_len = n;
        ret = ESP_OK;
    } else {
        /* 撤销等待要持锁：回调可能正在 OT 任务里跑。拿不到锁也照撤 —— 最坏是一条迟到的应答被下一次
           的 ID 比对挡掉（ID 不同）或被 take(0) 清掉 */
        /* Thread 已经停了就别去拿锁（thread_br_stop 会把 OT 锁本身删掉）：OT 任务都没了，也不会有回调来抢 */
        bool locked = thread_br_is_running() && esp_openthread_lock_acquire(pdMS_TO_TICKS(INJ_OT_LOCK_MS));
        s_inj_waiting = false;
        if (locked) esp_openthread_lock_release();
        (void)xSemaphoreTake(s_inj_done, 0);
        ret = ESP_ERR_TIMEOUT;
    }
out:
    xSemaphoreGive(s_inj_mutex);
    return ret;
}

/* ================= 注册潮（thread_br_diag_srp_burst_*）=================
 *
 * 量什么：网关重启后设备在 10 s 内成批重注册时，① OT 任务最长被占住多久（拿 OT 锁的最长等待 ——
 * SRP 代播在 OT 任务里同步调 mdns，被 mdns 卡住时 OT 锁一直被持着）；② IDLE 最长多久没跑到
 * （任务看门狗只盯 IDLE，5 s 就 panic）；③ 每条从送出到应答多久、RCODE 分布（SERVFAIL 多半是
 * mdns 动作队列满，整台主机被丢掉）。
 *
 * 发送节奏按主机给的时间表（srp_load.py --burst 生成，默认 10 s 窗口内均匀随机）。OT 消息缓冲
 * 不够时等 10 ms 重试，最多 50 次，再不行记一次 send_fail 接着发下一条 —— 真设备被丢包后也是
 * 过一阵自己重试。
 *
 * 为什么是常驻后台任务而不是在 httpd 里同步跑：一轮要几十秒，httpd 被占满 60 s 会被看门狗判停转、
 * 触发 L3 重启 httpd（websocket.c 的 httpd_spec）；常驻是因为栈在 PSRAM，不能自删（psram_task.h 规则 2）。
 * 本任务只调 OT API、不碰 flash / NVS（规则 1）。 */
#define BURST_SEND_RETRIES    50
#define BURST_MAX_AT_MS       600000u     /* 时间表最晚 10 分钟 */
#define BURST_RETRY_GAP_MS    10
#define BURST_LOCK_WAIT_MS    10000

static TaskHandle_t s_burst_task;

/* ---- IDLE 最长被饿多久：idle hook 每轮 idle 都跑一次（空闲时 WFI 被 1 kHz 节拍唤醒，间隔 ≈ 1 ms）。
   两次之间的最大间隔 = IDLE 最长一次没被调度到。只在 CPU0 上挂（单核）。
   ⚠️ 用 32 位微秒：64 位读写在 32 位 RISC-V 上不是原子的，重置时 IDLE 可能读到半截值、报出假的
   超长间隔；32 位对齐访问是原子的，71 分钟回绕靠无符号减法照样对。0 当"刚重置、不算这一段"。 */
static volatile uint32_t s_idle_last_us, s_idle_gap_max_us;
static bool s_idle_hooked;

static bool idle_gap_hook(void)
{
    uint32_t now = (uint32_t)esp_timer_get_time();
    uint32_t last = s_idle_last_us;
    if (last && now - last > s_idle_gap_max_us) s_idle_gap_max_us = now - last;
    s_idle_last_us = now ? now : 1;
    return true;                                          /* 照常 WFI */
}

static void idle_gap_reset(void)
{
    s_idle_last_us = 0;                                   /* 先清起点：下一次 hook 只记时刻、不算间隔 */
    s_idle_gap_max_us = 0;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static void burst_finish(void)
{
    /* 停止匹配：持 OT 锁改，回调就不会再写 slot。Thread 已经停了就别去拿锁（锁本身会被删掉）。 */
    bool locked = thread_br_is_running() && esp_openthread_lock_acquire(pdMS_TO_TICKS(BURST_LOCK_WAIT_MS));
    s_b.active = false;
    if (locked) esp_openthread_lock_release();

    uint32_t *lat = heap_caps_malloc(sizeof(uint32_t) * (s_b.n ? s_b.n : 1), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t k = 0;
    for (uint32_t i = 0; lat && i < s_b.n; i++) {
        if (s_b.slot[i].rsp_ms >= 0 && s_b.slot[i].sent_ms >= 0)
            lat[k++] = (uint32_t)(s_b.slot[i].rsp_ms - s_b.slot[i].sent_ms);
    }
    if (k) {
        qsort(lat, k, sizeof(uint32_t), cmp_u32);
        s_b.lat_p50_ms = lat[(k - 1) * 50 / 100];
        s_b.lat_p95_ms = lat[(k - 1) * 95 / 100];
        s_b.lat_max_ms = lat[k - 1];
    }
    free(lat);
    free(s_b.rec);
    s_b.rec = NULL;
    for (uint32_t i = 0; i < s_b.n; i++) s_b.slot[i].msg = NULL;
    s_b.elapsed_ms = (uint32_t)((esp_timer_get_time() - s_b.t0_us) / 1000);
    ESP_LOGW(TAG_INJ, "注册潮结束：%"PRIu32" 条，送出 %"PRIu32"（失败 %"PRIu32"），应答 %"PRIu32
             "；OT 锁最长等 %"PRIu32" ms，IDLE 最长被饿 %"PRIu32" ms",
             s_b.n, s_b.sent, s_b.send_fail, s_b.responded, s_b.ot_lock_wait_max_ms,
             s_idle_gap_max_us / 1000);
}

static void burst_run(void)
{
    s_b.t0_us = esp_timer_get_time();
    idle_gap_reset();
    for (uint32_t i = 0; i < s_b.n; i++) {
        burst_slot_t *sl = &s_b.slot[i];
        int64_t due = s_b.t0_us + (int64_t)sl->at_ms * 1000;
        int64_t now = esp_timer_get_time();
        if (due > now) vTaskDelay(pdMS_TO_TICKS((uint32_t)((due - now + 999) / 1000)));

        for (int attempt = 0; ; attempt++) {
            int64_t w0 = esp_timer_get_time();
            if (!thread_br_is_running() || !esp_openthread_lock_acquire(pdMS_TO_TICKS(BURST_LOCK_WAIT_MS))) {
                s_b.send_fail++;
                break;
            }
            uint32_t waited = (uint32_t)((esp_timer_get_time() - w0) / 1000);
            if (waited > s_b.ot_lock_wait_max_ms) s_b.ot_lock_wait_max_ms = waited;
            otInstance *inst = esp_openthread_get_instance();
            esp_err_t e = ESP_ERR_INVALID_STATE;
            if (inst && otSrpServerGetState(inst) == OT_SRP_SERVER_STATE_RUNNING &&
                inj_sock_ensure_locked(inst) == ESP_OK) {
                sl->sent_ms = (int32_t)((esp_timer_get_time() - s_b.t0_us) / 1000);   /* 先记：回调要等我们放锁才能跑 */
                e = inj_send_locked(inst, sl->msg, sl->len);
                if (e != ESP_OK) sl->sent_ms = -1;
            }
            esp_openthread_lock_release();
            if (e == ESP_OK) {
                s_b.sent++;
                uint32_t lag = (uint32_t)(sl->sent_ms > (int32_t)sl->at_ms ? sl->sent_ms - (int32_t)sl->at_ms : 0);
                if (lag > s_b.send_lag_max_ms) s_b.send_lag_max_ms = lag;
                break;
            }
            if (e == ESP_ERR_NO_MEM && attempt < BURST_SEND_RETRIES) {
                s_b.retries++;
                vTaskDelay(pdMS_TO_TICKS(BURST_RETRY_GAP_MS));
                continue;
            }
            s_b.send_fail++;
            break;
        }
        s_b.elapsed_ms = (uint32_t)((esp_timer_get_time() - s_b.t0_us) / 1000);
    }
    /* 等应答：全到齐，或最后一条送出后 timeout_ms 还没到齐 */
    int64_t last_send = esp_timer_get_time();
    while (s_b.responded < s_b.sent &&
           esp_timer_get_time() - last_send < (int64_t)s_b.timeout_ms * 1000) {
        vTaskDelay(pdMS_TO_TICKS(100));
        s_b.elapsed_ms = (uint32_t)((esp_timer_get_time() - s_b.t0_us) / 1000);
    }
}

static void burst_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);          /* 等下一轮：这是"等活干"，不是等某个必然发生的事件 */
        /* ⚠️ 整轮期间**不持有**任何别的互斥锁（包括单条注入那把）：FreeRTOS 要等任务放掉**全部**互斥锁
           才撤销继承来的优先级。整轮持着一把的话，一旦为发送拿 OT 锁时 ot_main（16）来等，本任务就会
           一直顶着 16 跑完整轮 —— 2026-10-07 coredump 里 srp_burst 是 16/4，测量本身就偏了。
           与单条注入的互斥改由 s_b.running 表达（见 thread_br_diag_srp_inject）。 */
        bool locked = thread_br_is_running() && esp_openthread_lock_acquire(pdMS_TO_TICKS(BURST_LOCK_WAIT_MS));
        if (locked) {
            s_b.active = true;
            esp_openthread_lock_release();
            burst_run();
        } else {
            s_b.send_fail = s_b.n;
        }
        burst_finish();
        s_b.running = false;
    }
}

static uint32_t rd_be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

esp_err_t thread_br_diag_srp_burst_start(uint8_t *rec, size_t len, uint32_t timeout_ms)
{
    if (!rec || len < 6 + 12 || timeout_ms == 0) return ESP_ERR_INVALID_ARG;
    if (s_b.running) return ESP_ERR_INVALID_STATE;
    if (!thread_br_is_running()) return ESP_ERR_INVALID_STATE;
    if (!inj_sync_init()) return ESP_ERR_NO_MEM;

    /* 先数条数、验格式，不碰旧结果 */
    uint32_t n = 0, prev_at = 0;
    uint16_t id0 = 0;
    for (size_t off = 0; off < len; n++) {
        if (n >= THREAD_BR_DIAG_BURST_MAX || len - off < 6) return ESP_ERR_INVALID_ARG;
        uint32_t at = rd_be32(rec + off);
        uint16_t l  = (uint16_t)((rec[off + 4] << 8) | rec[off + 5]);
        /* 计划时刻设上限：填个极大值，任务会一直睡、running 一直为真，之后的注册潮和单条注入全回"忙"，只能重启 */
        if (l < 12 || l > THREAD_BR_DIAG_SRP_MAX || len - off - 6 < l || at < prev_at || at > BURST_MAX_AT_MS)
            return ESP_ERR_INVALID_ARG;
        uint16_t id = (uint16_t)((rec[off + 6] << 8) | rec[off + 7]);
        if (n == 0) id0 = id;
        else if (id != (uint16_t)(id0 + n)) return ESP_ERR_INVALID_ARG;
        prev_at = at;
        off += 6 + l;
    }

    burst_slot_t *slot = heap_caps_calloc(n, sizeof(burst_slot_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!slot) return ESP_ERR_NO_MEM;
    if (!s_idle_hooked) {
        if (esp_register_freertos_idle_hook_for_cpu(idle_gap_hook, 0) != ESP_OK) { free(slot); return ESP_ERR_NO_MEM; }
        s_idle_hooked = true;
    }
    if (!s_burst_task &&
        psram_task_create(burst_task, "srp_burst", 4096, NULL, 4, &s_burst_task) != pdPASS) {
        s_burst_task = NULL;
        free(slot);
        return ESP_ERR_NO_MEM;
    }

    size_t off = 0;
    for (uint32_t i = 0; i < n; i++) {
        slot[i].at_ms   = rd_be32(rec + off);
        slot[i].len     = (uint16_t)((rec[off + 4] << 8) | rec[off + 5]);
        slot[i].msg     = rec + off + 6;
        slot[i].sent_ms = slot[i].rsp_ms = -1;
        slot[i].rcode   = -1;
        off += 6 + slot[i].len;
    }
    /* 上一轮的结果到这里才丢（任务已空闲：running 为 false） */
    free(s_b.slot);
    memset(&s_b, 0, sizeof(s_b));
    s_b.slot = slot;
    s_b.rec = rec;
    s_b.n = n;
    s_b.id0 = id0;
    s_b.timeout_ms = timeout_ms;
    s_b.running = true;
    thread_br_diag_mdns_calls_reset();
    xTaskNotifyGive(s_burst_task);
    return ESP_OK;
}

void thread_br_diag_srp_burst_status(thread_br_diag_burst_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->running   = s_b.running;
    out->n         = s_b.n;
    out->sent      = s_b.sent;
    out->send_fail = s_b.send_fail;
    out->retries   = s_b.retries;
    out->responded = s_b.responded;
    memcpy(out->rcode, s_b.rcode, sizeof(out->rcode));
    out->elapsed_ms          = s_b.elapsed_ms;
    out->lat_p50_ms          = s_b.lat_p50_ms;
    out->lat_p95_ms          = s_b.lat_p95_ms;
    out->lat_max_ms          = s_b.lat_max_ms;
    out->send_lag_max_ms     = s_b.send_lag_max_ms;
    out->ot_lock_wait_max_ms = s_b.ot_lock_wait_max_ms;
    out->idle_gap_max_ms     = s_idle_gap_max_us / 1000;
}

bool thread_br_diag_srp_burst_slot(uint32_t i, int32_t *at_ms, int32_t *sent_ms, int32_t *rsp_ms, int *rcode)
{
    /* 跑着的时候也能读（只读，值可能半新半旧）；slot 表只在下一轮 start 时换掉，而那要求 running 为 false */
    if (!s_b.slot || i >= s_b.n) return false;
    const burst_slot_t *sl = &s_b.slot[i];
    if (at_ms)   *at_ms   = (int32_t)sl->at_ms;
    if (sent_ms) *sent_ms = sl->sent_ms;
    if (rsp_ms)  *rsp_ms  = sl->rsp_ms;
    if (rcode)   *rcode   = sl->rcode;
    return true;
}
#endif /* CONFIG_BITENGINE_DIAG_SRP_INJECT */
