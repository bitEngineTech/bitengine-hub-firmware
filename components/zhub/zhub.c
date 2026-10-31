/* zhub —— 直连型 Zigbee 主机的任务外壳（接口见 include/zhub.h；规则与数值在 include/zh_host_core.h）。
 *
 * H1（本文件用到的驱动 / 桥接口，写代码前答清楚）：
 * 1. uart_bridge_read(buf, len, timeout_ms) 就是 uart_read_bytes：要读满 len 字节或等到超时才返回。所以先阻塞读 1 字节（timeout = 编排层
 *    给的等待上限），读到了再按 uart_bridge_get_rx_buffered() 把已缓冲的一次取走（timeout 0，不阻塞）。它顺带清 UART 的错误事件队列
 *    （溢出时清空接收、计 uart_rx_errors、发 BRIDGE_EVENT_UART_ERROR）。
 * 2. uart_bridge_write(buf, len, timeout_ms)：TX 缓冲为 0，用 uart_tx_chars 非阻塞地往 FIFO 填、超时返回已写字节数 —— 流控卡死
 *    （CTS 不放）不会把任务挂住。写不全的那一帧 NCP 收到的是半截（CRC 不过，它丢掉），ASH 的重传会补上；我们计 uart_tx_short。
 * 3. UART 归属：ezsp_bridge_zhub_claim() 是 RUNNING → ZHUB 的 CAS，ezsp_bridge_zhub_release() 反过来；别人的 ezsp_bridge_ota_claim() 在直连型里先调
 *    这里登记的 zhub_suspend（同步等我们归还），release 后调 zhub_resume。
 * 4. 任务栈在 PSRAM（xTaskCreateWithCaps）：这个任务不写 flash / NVS，cache 关着的时候不会跑到它。上下文（zh_host_t 约 2 KB + 收发缓冲）
 *    也在 PSRAM（Z56）：内部 RAM 紧，而这里的访问量是 UART 速率级的。
 *
 * S2：编排层对象只在本任务里碰（zh_host_core.h 的线程契约）。别的任务经原子量投递请求（挂起计数、立即复位），读的是加锁拷贝的快照。 */
#include "zhub.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ezsp_bridge.h"
#include "led_ctrl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "uart_bridge.h"
#include "watchdog.h"
#include "zh_dev.h"
#include "zh_host_core.h"

static const char *TAG = "zhub";

#define ZHUB_STACK          8192    /* PSRAM：设计 8.2 给 zh_main 的 8 KB，合并后 ASH 也在里面（ASH 本身不吃栈，帧都在上下文里） */
/* 优先级：原设计 zh_ash 21（与桥的数据任务同级）、zh_main 9。合并后上层逻辑（ZDO / ZCL / 设备库）以后也跑在这个任务里，放 20 以上会压住
   lwIP（tcpip 18）；ASH 的确认时限是几百毫秒级（NCP 的 ACK 定时器自适应、下限 400 ms），12 足够，仍高于 httpd、muse_sess、hub_svc。 */
#define ZHUB_PRIO           12
#define ZHUB_LOOP_MAX_MS    100     /* 每圈最长阻塞：挂起请求与看门狗心跳的响应上限 */
#define ZHUB_TX_TIMEOUT_MS  200     /* 最长一帧约 450 B，460800 下约 10 ms；流控卡住时别把任务挂死 */
#define ZHUB_WD_DEADLINE_MS 30000   /* 设计 8.2：与原 EZSP 数据通路同级（FATAL 30 s）。心跳每圈都打，不绑 NCP 应答（Z45 / C5） */
#define ZHUB_SPIN_LIMIT     20      /* 连续这么多圈"不等待"就强制让出 1 ms：编排层万一一直回 0，别让 12 级的任务饿死低优先级（含 IDLE） */
#define ZHUB_RESTART_MIN_MS 10000   /* 外部要求的立即复位（/zigbee/restart、看门狗 L2）最短间隔：它跳过退避，别让人连点把 NCP 反复复位（HA 型同类端点有 429 冷却） */
#define ZHUB_CLAIM_WARN     50      /* 连续这么多圈认领不到 UART（约 5 s，又没人挂起我们）打一条日志：桥停在 ERROR 时 zhub 就一直空转 */

#define ZHUB_PJ_WAIT_MS     500     /* zhub_permit_join 等任务处理的上限：任务每圈最多睡 ZHUB_LOOP_MAX_MS，留足余量 */

#define BIT_IDLE            BIT0    /* zhub 没持有 UART，而且在有人挂起它期间不会去拿 */
#define BIT_PJ_DONE         BIT1    /* 入网窗口请求处理完了 */
#define BIT_CTL_DONE        BIT2    /* 控制命令有结论了（或没发出去） */
#define ZHUB_CTL_WAIT_MS    (ZH_CTL_DEADLINE_MS + 2000u)   /* zhub_control 等结论的上限：zh_dev 最迟 ZH_CTL_DEADLINE_MS 给结论，加任务一圈的余量 */
#define ZHUB_DEV_PUB_MS     500     /* 设备表快照最多多久发布一次（每条上报都会改 last_seen，不必每条都拷） */
#define ZHUB_LED_ACT_MS     400     /* ZIGBEE 灯的蓝色短闪最多这么久一次（同 HA 型桥那一侧的 400 ms 节流，led_ctrl.c 的 ZB_ACTIVITY_HOLD_MS 说明） */

typedef struct {
    zh_host_t      h;
    zh_dev_core_t  dev;
    zhub_dev_view_t view[ZH_DEV_MAX];   /* 发布给别的任务的快照（s_dev_lock 护着） */
    size_t         view_n;
    uint8_t        rx[512];
    uint8_t        tx[2 * ZH_ASH_WIRE_MAX];
} zhub_ctx_t;

static zhub_ctx_t        *s_z;
static TaskHandle_t       s_task;
static EventGroupHandle_t s_ev;
static atomic_int         s_suspend_cnt;
static atomic_bool        s_restart_req;
/* NCP 侧诊断（zhub.h）：别的任务置 s_diag_req；本任务读、结果写进 s_diag（s_mux 护） */
static atomic_bool        s_diag_req, s_diag_busy;
static zhub_ncp_diag_t    s_diag;
static uint64_t           s_diag_t_done;        /* s_mux 护 */
static struct { bool active, inflight; uint8_t step; uint32_t t_sub; zhub_ncp_diag_t w; } s_dg;   /* 本任务独占 */
#if CONFIG_BITENGINE_FAULT_INJECTION
static atomic_bool        s_fault_rst;          /* 故障注入（TC-ZH07）：下一圈背着 ASH 写一帧 RST */
#endif
static atomic_bool        s_holding;            /* 只有本任务写；别的任务读（zhub_request_restart 判断） */
static uint64_t           s_t_claimed;          /* 本任务读写：接管 UART 的时刻（64 位毫秒，持有多久都不回绕） */
static uint32_t           s_ev_mark;            /* 本任务读写：接管时的正向证据计数（zh_host_evidence_mark）—— "接管以来的证据"比计数不比时间戳 */
static atomic_uint        s_t_restart;          /* 上一次被接受的外部立即复位（32 位毫秒，只比差值） */
static atomic_bool        s_restarted_once;
static uint32_t           s_tx_short;
/* 本任务读写（on_host_callback 在 zh_host_rx 的调用栈里、也就是本任务里跑）：设备发来的消息，snapshot 拷进快照 */
static uint32_t           s_rx_msgs, s_rx_bad;
static int8_t             s_bad_err;            /* 最近一条解不开的：错误码、帧控制、参数长度、参数前 31 字节（上板定位用） */
static uint8_t            s_bad_fc, s_bad_fc_hi, s_bad_n, s_bad_head_n, s_bad_head[31];
static bool               s_have_last_rx;
static uint64_t           s_t_last_rx;
static zh_ezsp_aps_t      s_last_aps;
static zh_ezsp_rx_info_t  s_last_info;
static int                s_wd = -1;
/* 入网窗口请求：调用方持 s_pj_lock 投一个（s_pj_req = 秒数 + 1，0 = 没有），任务处理完写 s_pj_res 再置 BIT_PJ_DONE */
static SemaphoreHandle_t  s_pj_lock;
static SemaphoreHandle_t  s_dev_lock;           /* 护 s_z->view / view_n：zhub 任务写、别的任务读 */
static zhub_hooks_t       s_hooks;              /* zhub_start 之前登记，之后只读 */
static uint32_t           s_t_dev_pub;
static atomic_uint        s_pj_req;
static zhub_pj_t          s_pj_res;
/* 控制命令（M5 第七步）：调用方持 s_ctl_lock 投一条（s_ctl_req，s_mux 护），任务发出去、等 zh_dev 给结论，写 s_ctl_res（带请求号）再置 BIT_CTL_DONE */
static SemaphoreHandle_t  s_ctl_lock;
static struct { bool pending; uint32_t id; uint8_t ieee[8]; uint8_t act, level; } s_ctl_req;
static zhub_ctl_t         s_ctl_res;
static uint32_t           s_ctl_id_seq;         /* 调用方分配请求号（持 s_ctl_lock） */
static uint32_t           s_ctl_cur_id;         /* 本任务：在途那条的请求号 */
/* 本任务读写：入网观察（on_host_callback） */
static uint32_t           s_joined, s_rejoined, s_left, s_denied, s_annces, s_tc_bad;
static bool               s_have_join;
static uint16_t           s_join_node, s_join_parent;
static uint8_t            s_join_eui[8], s_join_status, s_join_decision;
static uint64_t           s_t_join;
static portMUX_TYPE       s_mux = portMUX_INITIALIZER_UNLOCKED;
static zhub_status_t      s_snap;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint64_t now_ms64(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

/* ---------------------------------------------------------------- 别的任务调的 */

esp_err_t zhub_suspend(uint32_t timeout_ms)
{
    atomic_fetch_add(&s_suspend_cnt, 1);
    if (!s_task) return ESP_OK;                       /* 没起：UART 本来就不在我们手里 */
    if (xTaskGetCurrentTaskHandle() == s_task) {      /* 自己等自己归还 = 死锁 */
        atomic_fetch_sub(&s_suspend_cnt, 1);
        return ESP_ERR_INVALID_STATE;
    }
    const EventBits_t b = xEventGroupWaitBits(s_ev, BIT_IDLE, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    if (!(b & BIT_IDLE)) {
        atomic_fetch_sub(&s_suspend_cnt, 1);          /* 撤回：调用方不用再 resume */
        ESP_LOGW(TAG, "让出 UART 超时（%u ms）", (unsigned)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void zhub_resume(void)
{
    int c = atomic_load(&s_suspend_cnt);
    while (c > 0 && !atomic_compare_exchange_weak(&s_suspend_cnt, &c, c - 1)) { }
    if (c <= 0) ESP_LOGW(TAG, "resume 没有配对的 suspend（忽略）");
}

esp_err_t zhub_request_restart(void)
{
    if (!s_task || !atomic_load(&s_holding) || atomic_load(&s_suspend_cnt) > 0) return ESP_ERR_INVALID_STATE;
    const uint32_t now = now_ms();
    unsigned last = atomic_load(&s_t_restart);
    if (atomic_load(&s_restarted_once) && now - last < ZHUB_RESTART_MIN_MS) return ESP_ERR_NOT_ALLOWED;
    if (!atomic_compare_exchange_strong(&s_t_restart, &last, now)) return ESP_ERR_NOT_ALLOWED;   /* 同一时刻另一个请求抢先了 */
    atomic_store(&s_restarted_once, true);
    atomic_store(&s_restart_req, true);
    return ESP_OK;
}

esp_err_t zhub_control(const uint8_t ieee[8], uint8_t act, uint8_t level_pct, zhub_ctl_t *out)
{
    if (!out || !ieee || act > (uint8_t)ZH_CTL_LEVEL || level_pct > 100u) return ESP_ERR_INVALID_ARG;
    if (!s_task || !s_ctl_lock || xTaskGetCurrentTaskHandle() == s_task) return ESP_ERR_INVALID_STATE;
    if (!atomic_load(&s_holding) || atomic_load(&s_suspend_cnt) > 0) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_ctl_lock, pdMS_TO_TICKS(ZHUB_PJ_WAIT_MS)) != pdTRUE) return ESP_ERR_TIMEOUT;
    const uint32_t id = ++s_ctl_id_seq ? s_ctl_id_seq : ++s_ctl_id_seq;   /* 0 不用 */
    xEventGroupClearBits(s_ev, BIT_CTL_DONE);
    taskENTER_CRITICAL(&s_mux);
    s_ctl_req.pending = true;
    s_ctl_req.id = id;
    memcpy(s_ctl_req.ieee, ieee, 8);
    s_ctl_req.act = act;
    s_ctl_req.level = level_pct;
    taskEXIT_CRITICAL(&s_mux);
    esp_err_t e = ESP_ERR_TIMEOUT;
    const int64_t t_end = esp_timer_get_time() + (int64_t)ZHUB_CTL_WAIT_MS * 1000;
    for (;;) {                                     /* 只认自己这条的结论：上一位等超时走了，它那条迟到的结论别安到我头上 */
        const int64_t left_us = t_end - esp_timer_get_time();
        if (left_us <= 0) break;
        const EventBits_t b = xEventGroupWaitBits(s_ev, BIT_CTL_DONE, pdTRUE, pdTRUE, pdMS_TO_TICKS((uint32_t)(left_us / 1000) + 1u));
        if (!(b & BIT_CTL_DONE)) break;
        taskENTER_CRITICAL(&s_mux);
        const bool mine = s_ctl_res.id == id;
        if (mine) *out = s_ctl_res;
        taskEXIT_CRITICAL(&s_mux);
        if (mine) {
            e = ESP_OK;
            break;
        }
    }
    if (e != ESP_OK) {
        taskENTER_CRITICAL(&s_mux);
        const bool taken = !(s_ctl_req.pending && s_ctl_req.id == id);
        if (!taken) s_ctl_req.pending = false;     /* 撤回还没被取走的 */
        taskEXIT_CRITICAL(&s_mux);
        if (taken) e = ESP_ERR_NOT_FINISHED;       /* 已经被取走（多半已经发出去了）却等不到结论：调用方别让人重发（切换会切两次；审计） */
    }
    xSemaphoreGive(s_ctl_lock);
    return e;
}

esp_err_t zhub_permit_join(uint32_t seconds, zhub_pj_t *out)
{
    if (!out || seconds > ZH_HOST_PERMIT_MAX_S) return ESP_ERR_INVALID_ARG;
    if (!s_task || !s_pj_lock || xTaskGetCurrentTaskHandle() == s_task) return ESP_ERR_INVALID_STATE;
    if (!atomic_load(&s_holding) || atomic_load(&s_suspend_cnt) > 0) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_pj_lock, pdMS_TO_TICKS(ZHUB_PJ_WAIT_MS)) != pdTRUE) return ESP_ERR_TIMEOUT;
    xEventGroupClearBits(s_ev, BIT_PJ_DONE);
    atomic_store(&s_pj_req, seconds + 1u);
    const EventBits_t b = xEventGroupWaitBits(s_ev, BIT_PJ_DONE, pdTRUE, pdTRUE, pdMS_TO_TICKS(ZHUB_PJ_WAIT_MS));
    esp_err_t e = ESP_ERR_TIMEOUT;
    if (b & BIT_PJ_DONE) {
        taskENTER_CRITICAL(&s_mux);
        *out = s_pj_res;
        taskEXIT_CRITICAL(&s_mux);
        e = ESP_OK;
    } else {
        unsigned mine = seconds + 1u;              /* 撤回还没被取走的请求；已经取走的照样会被处理（结果只是没人等） */
        (void)atomic_compare_exchange_strong(&s_pj_req, &mine, 0u);
    }
    xSemaphoreGive(s_pj_lock);
    return e;
}

void zhub_set_hooks(const zhub_hooks_t *hooks)
{
    if (s_task) {                                  /* 任务起来以后再改钩子：任务里正在用，拒 */
        ESP_LOGW(TAG, "zhub_set_hooks 要在 zhub_start 之前调（忽略）");
        return;
    }
    if (hooks) s_hooks = *hooks;
    else memset(&s_hooks, 0, sizeof(s_hooks));
}

int zhub_dev_list(zhub_dev_view_t *out, size_t max)
{
    if (!out || !max || !s_z || !s_dev_lock) return 0;
    if (xSemaphoreTake(s_dev_lock, pdMS_TO_TICKS(100)) != pdTRUE) return -1;
    const size_t n = s_z->view_n < max ? s_z->view_n : max;
    memcpy(out, s_z->view, n * sizeof(*out));
    xSemaphoreGive(s_dev_lock);
    /* 年龄在这里算：没人说话时快照不重发，发布时算好的年龄会停住（第二轮审计 ❌：唯一一台设备的 last_seen 永远是 0、永远在线）。
       zh_dev_tick（每圈都跑，挂起时也跑）把 t_seen 封在 ZH_DEV_AGE_CAP_MS 以内并重发快照，所以这里的减法不会回绕 */
    const uint32_t now = now_ms();
    for (size_t i = 0; i < n; i++) {
        out[i].age_ms = now - out[i].t_seen;
        for (unsigned f = 0; f < ZH_ST_N; f++) out[i].st_age_ms[f] = now - out[i].st.t[f];   /* 读数的时刻也由 zh_dev_tick 封顶 */
    }
    return (int)n;
}

#if CONFIG_BITENGINE_FAULT_INJECTION
esp_err_t zhub_fault_ash_rst(void)
{
    if (!s_task || !atomic_load(&s_holding)) return ESP_ERR_INVALID_STATE;
    atomic_store(&s_fault_rst, true);
    return ESP_OK;
}
#endif

_Static_assert(ZHUB_NCP_CTR_N == ZH_EZSP_COUNTER_N, "zhub.h 的计数器个数要等于 readCounters 回的个数");
_Static_assert(ZHUB_NCP_CHILD_SCAN + 1u <= 255u, "步号在 tag 的 8–15 位：0 计数器、1 安全状态、2… 子设备表");
static const char *const k_ctr_name[ZHUB_NCP_CTR_N] = {   /* sl_zigbee_types.h:1479-1708 的顺序 */
    "mac_rx_bcast", "mac_tx_bcast", "mac_rx_ucast", "mac_tx_ucast_ok", "mac_tx_ucast_retry", "mac_tx_ucast_fail",
    "aps_rx_bcast", "aps_tx_bcast", "aps_rx_ucast", "aps_tx_ucast_ok", "aps_tx_ucast_retry", "aps_tx_ucast_fail",
    "route_disc", "nbr_added", "nbr_removed", "nbr_stale", "join_ind", "child_removed",
    "ash_overflow", "ash_framing", "ash_overrun", "nwk_fc_fail", "aps_fc_fail", "ash_xoff",
    "aps_key_unauth", "nwk_decrypt_fail", "aps_decrypt_fail", "pkt_buf_fail", "relayed_ucast", "phy_mac_q_limit",
    "pkt_validate_drop", "nwk_retry_overflow", "phy_cca_fail", "bcast_table_full", "pta_lo_req", "pta_hi_req",
    "pta_lo_denied", "pta_hi_denied", "pta_lo_aborted", "pta_hi_aborted", "addr_conflict_sent", "csl_rx_sched_fail",
};

const char *zhub_ncp_counter_name(unsigned i)
{
    return i < ZHUB_NCP_CTR_N ? k_ctr_name[i] : NULL;
}

esp_err_t zhub_ncp_diag_request(void)
{
    if (!s_task || !atomic_load(&s_holding)) return ESP_ERR_INVALID_STATE;
    atomic_store(&s_diag_req, true);
    return ESP_OK;
}

void zhub_ncp_diag_get(zhub_ncp_diag_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_mux);
    *out = s_diag;
    const uint64_t t = s_diag_t_done;
    taskEXIT_CRITICAL(&s_mux);
    const uint64_t now = now_ms64();                /* 出了临界区再取：先取的话，中间换上的那份 t 会比 now 晚、差值回绕成 49 天（审计 ❌） */
    out->busy = atomic_load(&s_diag_busy);
    out->age_ms = out->valid ? (uint32_t)((now - t) > UINT32_MAX ? UINT32_MAX : (now - t)) : 0;
}

esp_err_t zhub_wd_recover(void)
{
    const esp_err_t e = zhub_request_restart();
    if (e == ESP_OK) ESP_LOGW(TAG, "看门狗定向恢复：立即复位链路");
    return e;
}

void zhub_get_status(zhub_status_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_mux);
    *out = s_snap;
    taskEXIT_CRITICAL(&s_mux);
}

bool zhub_started(void)
{
    taskENTER_CRITICAL(&s_mux);
    const bool on = s_snap.started;
    taskEXIT_CRITICAL(&s_mux);
    return on;
}

bool zhub_mg24_evidence(zhub_mg24_view_t *out)
{
    if (!out) return false;
    zhub_status_t s;
    zhub_get_status(&s);
    out->active = s.started && s.holding_uart && !s.suspended;
    out->evidence = s.have_evidence;
    out->age_ms = s.evidence_age_ms;
    return s.started;
}

/* ---------------------------------------------------------------- 任务 */

/* ---------------------------------------------------------------- 设备层的接线（都在本任务里） */

static void make_view(const zh_dev_t *x, uint32_t now, zhub_dev_view_t *v)
{
    v->rec = x->rec;
    v->iv = x->iv;
    v->iv_started = x->iv_t0 != 0;
    v->seen = x->seen;
    v->offline = x->offline;
    v->t_seen = x->t_seen;
    v->age_ms = now - x->t_seen;
    v->rssi = x->rssi;
    v->lqi = x->lqi;
    v->rx = x->rx;
    v->st = x->st;
    v->st_ev = x->st_ev;
    for (unsigned f = 0; f < ZH_ST_N; f++) v->st_age_ms[f] = now - x->st.t[f];
}

/* 设备表变了：最多每 ZHUB_DEV_PUB_MS 发布一次快照（force = 结构变化，立即发） */
static void publish_devices(bool force)
{
    const uint32_t now = now_ms();
    if (!s_z->dev.dirty || (!force && now - s_t_dev_pub < ZHUB_DEV_PUB_MS)) return;
    if (xSemaphoreTake(s_dev_lock, pdMS_TO_TICKS(20)) != pdTRUE) return;   /* 读的人拿着：下一圈再发 */
    size_t n = 0;
    for (unsigned i = 0; i < ZH_DEV_MAX; i++)
        if (s_z->dev.dev[i].used) make_view(&s_z->dev.dev[i], now, &s_z->view[n++]);
    s_z->view_n = n;
    xSemaphoreGive(s_dev_lock);
    s_z->dev.dirty = false;
    s_t_dev_pub = now;
}

static void dev_save(void *ctx, unsigned idx, const zh_dev_rec_t *rec)
{
    (void)ctx;
    if (s_hooks.save) s_hooks.save(idx, rec);
}

static void dev_event(void *ctx, zh_dev_ev_t ev, zh_dev_src_t src, unsigned idx, const zh_dev_rec_t *rec)
{
    (void)ctx;
    (void)rec;
    const zh_dev_t *x = &s_z->dev.dev[idx];
    ESP_LOGI(TAG, "设备 %02X%02X%02X%02X%02X%02X%02X%02X（0x%04X）：%s（来源 %d）", x->rec.ieee[7], x->rec.ieee[6], x->rec.ieee[5], x->rec.ieee[4],
             x->rec.ieee[3], x->rec.ieee[2], x->rec.ieee[1], x->rec.ieee[0], (unsigned)x->rec.nwk,
             ev == ZH_DEV_EV_ADDED ? "新记录" : ev == ZH_DEV_EV_LEFT ? "离开" : ev == ZH_DEV_EV_BACK ? "回来了" : ev == ZH_DEV_EV_NWK ? "短地址变了" :
             ev == ZH_DEV_EV_IDENTIFIED ? "访谈完成" : ev == ZH_DEV_EV_IV_FAILED ? "访谈失败" : ev == ZH_DEV_EV_OFFLINE ? "判离线" :
             ev == ZH_DEV_EV_ONLINE ? "恢复在线" : "读数报警翻转", (int)src);
    /* 访谈完 / 失败、报警类读数翻转、离线 / 恢复的推送发出去之前先发布快照：Muse 收到推送马上来 hub.device.get，别读到 500 ms 前的旧表
       （两次第一轮审计；离线 / 恢复是 M5 第十一步审计 ❌） */
    if (ev == ZH_DEV_EV_IDENTIFIED || ev == ZH_DEV_EV_IV_FAILED || ev == ZH_DEV_EV_STATE || ev == ZH_DEV_EV_OFFLINE || ev == ZH_DEV_EV_ONLINE) publish_devices(true);
    if (!s_hooks.event) return;
    zhub_dev_view_t v;
    make_view(x, now_ms(), &v);
    s_hooks.event(ev, src, &v);
}

_Static_assert(ZH_DEV_TAG_MAX <= ZH_HOST_APP_TAG_MAX, "设备层的 tag 要能原样经 zh_host 的应用命令队列交回");
_Static_assert(ZH_DEV_SRC_EP == ZH_HOST_ENDPOINT, "访谈的 ZCL 请求从网关注册的端点发");
_Static_assert(ZH_HA_PROFILE == ZH_HOST_EP_PROFILE, "网关端点的 profile 就是 HA");

static bool dev_send(void *ctx, const uint8_t *frame, size_t n, uint32_t tag, bool idempotent)
{
    (void)ctx;
    return zh_host_app_submit(&s_z->h, frame, n, tag, idempotent);
}

static uint8_t dev_zdo_seq(void *ctx)
{
    (void)ctx;
    return zh_host_next_zdo_seq(&s_z->h);
}

#define ZHUB_TAG_KIND_DIAG 0xFu                     /* 设备层只用 1–7（zh_dev.h ZH_DEV_TAG_KIND_*），不认的种类它回 false */
_Static_assert(ZHUB_TAG_KIND_DIAG > ZH_DEV_TAG_KIND_DISC, "诊断的 tag 种类别和设备层的撞");
#define ZHUB_DIAG_RSP_MS   3000u                    /* 一条诊断命令的结果最多等这么久（会话自己的应答超时更短，这里只是兜底） */

static void diag_abort(void)
{
    s_dg.active = s_dg.inflight = false;
    atomic_store(&s_diag_busy, false);
    taskENTER_CRITICAL(&s_mux);
    s_diag.fails++;
    taskEXIT_CRITICAL(&s_mux);
}

static void diag_result(uint8_t step, zh_ses_result_t r, const zh_ezsp_frame_t *fr)
{
    if (!s_dg.active || !s_dg.inflight || step != s_dg.step) return;   /* 作废之后才回来的 */
    s_dg.inflight = false;
    if (r != ZH_SES_R_OK || !fr) {
        diag_abort();
        return;
    }
    if (step == 0) {
        if (zh_ezsp_dec_read_counters(fr, s_dg.w.ctr) != ZH_EZSP_OK) {
            diag_abort();
            return;
        }
    } else if (step == 1) {
        uint32_t st = 1;
        uint8_t tc[8];
        if (zh_ezsp_dec_get_current_security_state(fr, &st, &s_dg.w.sec_bitmask, tc) != ZH_EZSP_OK || st != 0) {
            diag_abort();
            return;
        }
    } else {
        uint32_t st = 1;
        zh_ezsp_child_t c;
        if (zh_ezsp_dec_child_data(fr, &st, &c) != ZH_EZSP_OK) {
            diag_abort();
            return;
        }
        if (st == 0) {                             /* 这一格有子设备（NOT_JOINED = 空格） */
            if (s_dg.w.n_child < ZHUB_NCP_CHILD_MAX) {
                zhub_ncp_child_t *o = &s_dg.w.child[s_dg.w.n_child++];
                memcpy(o->eui64, c.eui64, 8);
                o->id = c.id;
                o->type = c.type;
                o->timeout = c.timeout;
                o->remaining = c.remaining;
            } else {
                s_dg.w.child_more = true;
            }
        }
    }
    if (++s_dg.step <= ZHUB_NCP_CHILD_SCAN + 1u) return;
    s_dg.active = false;                           /* 读完：整份换上去 */
    taskENTER_CRITICAL(&s_mux);
    const uint32_t reads = s_diag.reads + 1u, fails = s_diag.fails;
    s_diag = s_dg.w;
    s_diag.valid = true;
    s_diag.reads = reads;
    s_diag.fails = fails;
    s_diag_t_done = now_ms64();
    taskEXIT_CRITICAL(&s_mux);
    atomic_store(&s_diag_busy, false);
}

/* 本任务里每圈调：接一个请求、交下一条（一条在途），结果在 diag_result */
static void diag_service(uint32_t now)
{
    if (atomic_exchange(&s_diag_req, false) && !s_dg.active) {
        memset(&s_dg, 0, sizeof(s_dg));
        s_dg.active = true;
        atomic_store(&s_diag_busy, true);
    }
    if (!s_dg.active) return;
    if (s_z->h.state != ZH_HOST_RUNNING) {         /* 链路复位了：这一份作废 */
        diag_abort();
        return;
    }
    if (s_dg.inflight) {
        if ((int32_t)(now - s_dg.t_sub) >= (int32_t)ZHUB_DIAG_RSP_MS) diag_abort();   /* 结果一直没回来（应用命令队列在复位时清空、不回结果） */
        return;
    }
    uint8_t b[16];
    const size_t n = s_dg.step == 0 ? zh_ezsp_enc_read_counters(b, sizeof(b), 0)
                   : s_dg.step == 1 ? zh_ezsp_enc_get_current_security_state(b, sizeof(b), 0)
                                    : zh_ezsp_enc_get_child_data(b, sizeof(b), 0, (uint8_t)(s_dg.step - 2u));
    if (!n) {
        diag_abort();
        return;
    }
    if (!zh_host_app_submit(&s_z->h, b, n, ZHUB_TAG_KIND_DIAG | ((uint32_t)s_dg.step << 8), false)) return;   /* 队列满：下一圈再交 */
    s_dg.inflight = true;
    s_dg.t_sub = now;
}

static void on_app_result(void *ctx, uint32_t tag, zh_ses_result_t r, const zh_ezsp_frame_t *fr)
{
    (void)ctx;
    if (zh_dev_on_app_result(&s_z->dev, tag, r, fr, now_ms())) return;
    if ((tag & 0x0Fu) == ZHUB_TAG_KIND_DIAG) diag_result((uint8_t)(tag >> 8), r, fr);
}

/* 开机从 hubdb 读回设备表（任务起来、认领 UART 之前；hub_store 的 get 是同步的，经它的队列） */
static void load_devices(void)
{
    if (!s_hooks.load) return;
    unsigned n = 0;
    for (unsigned i = 0; i < ZH_DEV_MAX; i++) {
        zh_dev_rec_t rec;
        wd_kick(s_wd);                             /* 每条最多等 hub_store 一会儿（钩子里定），64 条加起来别撞上看门狗 30 s */
        const zhub_load_t r = s_hooks.load(i, &rec);
        if (r == ZHUB_LOAD_EMPTY) continue;
        /* 读不出来的不是"没有"：这次开机保留这一格，别让新学到的设备落进来、把存储里那条盖掉（第二轮审计 ❌） */
        if (r == ZHUB_LOAD_ERR) {
            (void)zh_dev_reserve(&s_z->dev, i);
            continue;
        }
        if (zh_dev_load(&s_z->dev, i, &rec, now_ms())) n++;
        else ESP_LOGW(TAG, "设备记录 d%02u 是坏的（IEEE 全 0 / 全 FF，或与前面的重复），当空格用", i);   /* 坏记录盖掉无妨；读不出来的才保留 */
    }
    publish_devices(true);
    ESP_LOGI(TAG, "设备表：读回 %u 台%s", n, s_z->dev.st.reserved ? "（有读不出来的格子，保留着）" : "");
}

/* 编排层交上来的每条回调（本任务里）：入网回调与 incomingMessageHandler —— 计数、记最后一条，再交设备层（zh_dev：记录、被动学习、访谈；
   上报解析随 M5）。不打日志、不留载荷：设备每次上报都会走到这里，载荷归上层解析 */
static void on_host_callback(void *ctx, const zh_ezsp_frame_t *fr)
{
    (void)ctx;
    if (fr->id == ZH_EZSP_ID_TRUST_CENTER_POST_JOIN_HANDLER) {
        /* 信任中心看到的入网 / 重新入网 / 离开（status：sl_zigbee_types.h:1107-1110；decision：:1072-1081）。记录与访谈交设备层 */
        uint16_t node = 0, parent = 0;
        uint8_t eui[8], status = 0, decision = 0;
        if (zh_ezsp_dec_tc_post_join(fr, &node, eui, &status, &decision, &parent) != ZH_EZSP_OK) {
            s_tc_bad++;
            return;
        }
        /* 信任中心每做一次决定都回调这里，拒绝的也算（trust-center.h:227-230）：只有放行的（USE_PRECONFIGURED_KEY 0 / SEND_KEY_IN_THE_CLEAR 1，
           重新入网另有 NO_ACTION 3）才算入网 / 重新入网。两轮审计 ❌：原先 status 1 就算新设备入网，窗口外被拒的也进了事件环 */
        if (decision == 2) s_denied++;             /* DENY_JOIN */
        else if (status == 1 && decision <= 1) s_joined++;          /* STANDARD_SECURITY_UNSECURED_JOIN：新设备 */
        else if (status == 0 || status == 3) s_rejoined++;          /* SECURED / UNSECURED_REJOIN */
        else if (status == 2) s_left++;            /* DEVICE_LEFT */
        s_join_node = node;
        s_join_parent = parent;
        memcpy(s_join_eui, eui, sizeof(s_join_eui));
        s_join_status = status;
        s_join_decision = decision;
        s_t_join = now_ms64();
        s_have_join = true;
        zh_dev_on_tc_join(&s_z->dev, node, eui, status, decision, parent, now_ms());
        return;
    }
    if (fr->id == ZH_EZSP_ID_MESSAGE_SENT_HANDLER) {   /* 控制命令的送达结果（M5 第七步；访谈 / 补读不看它） */
        uint32_t st = 0;
        uint8_t mtype = 0, mlen = 0;
        uint16_t dst = 0, mtag = 0;
        zh_ezsp_aps_t maps;
        const uint8_t *msg = NULL;
        if (zh_ezsp_dec_message_sent(fr, &st, &mtype, &dst, &maps, &mtag, &mlen, &msg) == ZH_EZSP_OK) zh_dev_on_msg_sent(&s_z->dev, mtag, st, now_ms());
        return;
    }
    if (fr->id != ZH_EZSP_ID_INCOMING_MESSAGE_HANDLER) return;
    uint8_t type = 0, len = 0, overhead = 0;
    zh_ezsp_aps_t aps;
    zh_ezsp_rx_info_t info;
    const uint8_t *payload = NULL;
    const int d = zh_ezsp_dec_incoming_message(fr, &type, &aps, &info, &len, &payload, &overhead);
    if (d != ZH_EZSP_OK) {
        s_rx_bad++;
        s_bad_err = (int8_t)d;
        s_bad_fc = fr->fc;
        s_bad_fc_hi = fr->fc_hi;
        s_bad_n = fr->params.n > 255 ? 255 : (uint8_t)fr->params.n;
        s_bad_head_n = fr->params.n < sizeof(s_bad_head) ? (uint8_t)fr->params.n : (uint8_t)sizeof(s_bad_head);
        if (fr->params.p) memcpy(s_bad_head, fr->params.p, s_bad_head_n);
        return;
    }
    s_rx_msgs++;
    {                                              /* ZIGBEE 灯：设备发来消息蓝色短闪（Z54；节流在调用方，同 HA 型的桥） */
        static uint32_t s_t_led_act;
        const uint32_t t = now_ms();
        if (t - s_t_led_act >= ZHUB_LED_ACT_MS) {
            s_t_led_act = t;
            led_ctrl_notify_zigbee_activity();
        }
    }
    if (aps.profile_id == ZH_ZDO_PROFILE && aps.cluster_id == ZH_ZDO_DEVICE_ANNCE) s_annces++;
    /* ZDO（设备宣告、IEEE_addr_rsp、访谈应答）先交设备层：宣告里就有 IEEE，先认出来，下面的 on_rx 就不用再去查它是谁；
       其它 profile 是 ZCL（访谈的读属性应答；上报解析随 M5） */
    if (aps.profile_id == ZH_ZDO_PROFILE) zh_dev_on_zdo(&s_z->dev, info.sender_short, aps.cluster_id, payload, len, now_ms());
    else zh_dev_on_zcl(&s_z->dev, info.sender_short, aps.profile_id, aps.cluster_id, aps.src_ep, payload, len, now_ms());
    zh_dev_on_rx(&s_z->dev, info.sender_short, info.last_hop_rssi, info.last_hop_lqi, now_ms());
    s_last_aps = aps;
    s_last_info = info;
    s_t_last_rx = now_ms64();
    s_have_last_rx = true;
}

static void snapshot(void)
{
    const zh_host_t *h = &s_z->h;
    zhub_status_t s = { 0 };
    s.started = true;
    s.holding_uart = atomic_load(&s_holding);
    s.suspended = atomic_load(&s_suspend_cnt) > 0;
    s.state = zh_host_state_name(h->state);
    s.net = zh_host_net_name(h->net);
    s.last_fail = zh_host_fail_name(h->last_fail);
    s.last_fail_code = h->last_fail_code;
    s.fails = h->fails;
    s.failing = zh_host_failing(h);
    s.last_net_status = h->last_net_status;
    s.have_version = h->have_version;
    s.stack_type = (uint8_t)(h->version_code >> 16);
    s.stack_version = (uint16_t)h->version_code;
    s.have_params = h->have_params;
    if (h->have_params) {
        s.channel = h->params.channel;
        s.pan_id = h->params.pan_id;
        s.tx_power = h->params.tx_power;
        memcpy(s.ext_pan_id, h->params.ext_pan_id, sizeof(s.ext_pan_id));
        s.node_type = h->node_type;
    }
    s.have_eui64 = h->have_eui64;
    if (h->have_eui64) memcpy(s.eui64, h->eui64, sizeof(s.eui64));
    s.concentrator = h->concentrator && h->mtorr;
    for (int i = 0; i < ZH_HOST_UP_N; i++) s.up_bad += h->st.up_bad[i];
    s.init_opt_rejects = h->st.init_opt_rejects;
    s.win_state = zh_host_win_name((zh_win_state_t)h->win_state);
    s.win_left_s = zh_host_window_left_s(h, now_ms());
    s.win_s = h->win_s;
    s.win_quota_left_s = zh_host_quota_left_s(h, now_ms());
    s.ncp_open = h->ncp_open;
    s.win_opens = h->st.win_opens;
    s.win_open_fails = h->st.win_open_fails;
    s.win_closes = h->st.win_closes;
    s.win_expired = h->st.win_expired;
    s.win_close_retries = h->st.win_close_retries;
    s.win_skipped = h->st.win_skipped;
    s.win_last_err = h->win_last_err;
    s.joined = s_joined;
    s.rejoined = s_rejoined;
    s.left = s_left;
    s.denied = s_denied;
    s.annces = s_annces;
    s.tc_bad = s_tc_bad;
    s.have_last_join = s_have_join;
    if (s_have_join) {
        s.last_join_node = s_join_node;
        s.last_join_parent = s_join_parent;
        memcpy(s.last_join_eui64, s_join_eui, sizeof(s.last_join_eui64));
        s.last_join_status = s_join_status;
        s.last_join_decision = s_join_decision;
        const uint64_t a = now_ms64() - s_t_join;
        s.last_join_age_ms = a > UINT32_MAX ? UINT32_MAX : (uint32_t)a;
    }
    s.incoming = s_rx_msgs;
    s.devices = zh_dev_count(&s_z->dev);
    s.dev_st = s_z->dev.st;
    s.disc_state = s_z->dev.disc.state;
    s.disc_q_n = s_z->dev.disc.q_n;
    s.disc_q_i = s_z->dev.disc.q_i;
    s.incoming_bad = s_rx_bad;
    s.bad_err = s_bad_err;
    s.bad_fc = s_bad_fc;
    s.bad_fc_hi = s_bad_fc_hi;
    s.bad_n = s_bad_n;
    s.bad_head_n = s_bad_head_n;
    memcpy(s.bad_head, s_bad_head, sizeof(s.bad_head));
    s.have_last_rx = s_have_last_rx;
    if (s_have_last_rx) {
        s.last_rx_node = s_last_info.sender_short;
        s.last_rx_profile = s_last_aps.profile_id;
        s.last_rx_cluster = s_last_aps.cluster_id;
        s.last_rx_ep = s_last_aps.src_ep;
        s.last_rx_lqi = s_last_info.last_hop_lqi;
        s.last_rx_rssi = s_last_info.last_hop_rssi;
        const uint64_t a = now_ms64() - s_t_last_rx;
        s.last_rx_age_ms = a > UINT32_MAX ? UINT32_MAX : (uint32_t)a;
    }
    /* 证据只认这次接管以来的（挂起前的证据说明不了现在）。比计数不比时间戳：持有满 2^31 ms 后两个 32 位时间戳的差值会变号
       （第一轮审计复现：约 24.8 天后看门狗误判 MG24 已死）。没有证据时年龄从接管起算，用 64 位 */
    uint32_t age = 0;
    s.have_evidence = zh_host_evidence_since(h, s_ev_mark, now_ms(), &age);
    if (!s.have_evidence) {
        const uint64_t held = now_ms64() - s_t_claimed;
        age = held > UINT32_MAX ? UINT32_MAX : (uint32_t)held;
    }
    s.evidence_age_ms = age;
    s.resets = h->st.resets;
    s.readies = h->st.readies;
    s.net_inits = h->st.net_inits;
    s.callbacks = h->st.callbacks;
    s.restarts = h->st.restarts;
    s.ash_tx_frames = h->ash.st.tx_frames;
    s.ash_rx_frames = h->ash.st.rx_frames;
    s.ash_retx = h->ash.st.tx_redata;
    s.ash_crc = h->ash.st.rx_crc;
    s.ash_ack_timeouts = h->ash.st.ack_timeouts;
    s.ash_ncp_resets = h->ash.st.ncp_resets;
    s.ash_ncp_errors = h->ash.st.ncp_errors;
    s.ses_timeouts = h->ses.st.timeouts;
    s.ses_echoes = h->ses.st.echoes;
    s.ses_rejected = h->ses.st.rejected;
    s.ses_overflow = h->ses.st.overflow;
    s.ses_lost_callbacks = h->ses.st.lost_callbacks;
    s.ses_keepalives = h->ses.st.keepalives;
    s.uart_tx_short = s_tx_short;
    taskENTER_CRITICAL(&s_mux);
    s_snap = s;
    taskEXIT_CRITICAL(&s_mux);
}

/* 状态 / 网络 / 失败有变化才打日志（退避本身已经把失败拉开了间隔，不会刷屏） */
static void log_changes(void)
{
    static zh_host_state_t s_state = ZH_HOST_IDLE;
    static zh_host_net_t   s_net = ZH_NET_UNKNOWN;
    static uint32_t        s_fail_total;
    const zh_host_t *h = &s_z->h;
    uint32_t total = 0;
    for (int i = 0; i < ZH_HOST_FAIL_MAX; i++) total += h->st.fails[i];
    if (total != s_fail_total) {
        s_fail_total = total;
        ESP_LOGW(TAG, "链路失败：%s（code 0x%"PRIx32"），连续第 %"PRIu32" 次%s", zh_host_fail_name(h->last_fail), h->last_fail_code, h->fails,
                 zh_host_failing(h) ? " —— NCP 持续不正常" : "");
    }
    if (h->state != s_state) {
        if (h->state == ZH_HOST_RUNNING && h->have_version) {
            ESP_LOGI(TAG, "运行中：EZSP 协议 0x12，NCP stackType %u、版本 0x%04X", (unsigned)(h->version_code >> 16), (unsigned)(h->version_code & 0xFFFF));
        } else if (h->state == ZH_HOST_BACKOFF) {
            ESP_LOGW(TAG, "退避 %"PRIu32" ms 后再复位", h->t_timer - now_ms());
        } else {
            ESP_LOGI(TAG, "状态 %s → %s", zh_host_state_name(s_state), zh_host_state_name(h->state));
        }
        s_state = h->state;
    }
    if (h->net != s_net) {
        if (h->net == ZH_NET_NONE) ESP_LOGW(TAG, "NCP 里没有 Zigbee 网络（networkInit 回 NOT_JOINED）—— 不自动建网");
        else ESP_LOGI(TAG, "网络 %s → %s（sl_status 0x%04"PRIX32"）", zh_host_net_name(s_net), zh_host_net_name(h->net), h->last_net_status);
        s_net = h->net;
    }
    static uint8_t s_win = ZH_WIN_CLOSED;
    static uint32_t s_win_fails, s_joins_seen, s_win_closes;
    if (h->win_state != s_win) {
        if (h->win_state == ZH_WIN_OPEN) ESP_LOGI(TAG, "入网窗口开了：%u s", (unsigned)h->win_s);
        else if (h->win_state == ZH_WIN_CLOSED && s_win != ZH_WIN_CLOSED) {
            if (h->st.win_closes != s_win_closes) ESP_LOGI(TAG, "入网窗口关了（信任中心策略已写回）");
            else ESP_LOGW(TAG, "入网窗口随链路复位作废（NCP 重启后信任中心策略回到窗口外）");
        }
        s_win = h->win_state;
        s_win_closes = h->st.win_closes;
    }
    if (h->st.win_open_fails != s_win_fails) {
        s_win_fails = h->st.win_open_fails;
        ESP_LOGW(TAG, "入网窗口没开成（0x%08"PRIX32"：步号 / 会话结果 / sl_status），已复原", h->win_last_err);
    }
    if (s_joined + s_rejoined + s_left != s_joins_seen) {
        s_joins_seen = s_joined + s_rejoined + s_left;
        ESP_LOGI(TAG, "信任中心：节点 0x%04X（%02X%02X%02X%02X%02X%02X%02X%02X）status %u decision %u，父节点 0x%04X", (unsigned)s_join_node,
                 s_join_eui[7], s_join_eui[6], s_join_eui[5], s_join_eui[4], s_join_eui[3], s_join_eui[2], s_join_eui[1], s_join_eui[0],
                 (unsigned)s_join_status, (unsigned)s_join_decision, (unsigned)s_join_parent);
    }
    static bool s_conc;
    const bool conc = h->concentrator && h->mtorr;
    if (conc != s_conc) {
        s_conc = conc;
        if (conc) ESP_LOGI(TAG, "多对一路由已开（HIGH_RAM 集中器），%"PRIu32" ms 后第一次 MTORR；信道 %u、PAN 0x%04X、EUI64 %02X%02X%02X%02X%02X%02X%02X%02X",
                           h->mtorr_next_ms, (unsigned)h->params.channel, (unsigned)h->params.pan_id, h->eui64[7], h->eui64[6], h->eui64[5], h->eui64[4],
                           h->eui64[3], h->eui64[2], h->eui64[1], h->eui64[0]);
    }
}

/* 收：先阻塞读 1 字节（最多 wait_ms），读到了再把已缓冲的一次取走 */
static int uart_rx(uint32_t wait_ms)
{
    int n = uart_bridge_read(s_z->rx, 1, (int)wait_ms);
    if (n != 1) return 0;
    size_t more = 0;
    if (uart_bridge_get_rx_buffered(&more) == ESP_OK && more) {
        if (more > sizeof(s_z->rx) - 1) more = sizeof(s_z->rx) - 1;
        const int m = uart_bridge_read(s_z->rx + 1, (int)more, 0);
        if (m > 0) n += m;
    }
    return n;
}

/* 发：编排层吐多少写多少（每次 zh_host_tx 只给整帧）；写不全就停，下一圈再说 */
static void uart_tx(uint32_t now)
{
    for (int i = 0; i < 8; i++) {
        const size_t m = zh_host_tx(&s_z->h, now, s_z->tx, sizeof(s_z->tx));
        if (!m) return;
        const int w = uart_bridge_write(s_z->tx, (int)m, ZHUB_TX_TIMEOUT_MS);
        const bool short_write = w != (int)m;
        if (short_write) s_tx_short++;
        ezsp_bridge_note_uart(0, w > 0 ? (uint32_t)w : 0, short_write);
        if (short_write) return;
    }
}

/* 控制命令（M5 第七步）：取一条请求发出去；在途那条有结论了就交回调用方（没人等也照样收尾）。本任务里每圈调 */
static void ctl_service(void)
{
    taskENTER_CRITICAL(&s_mux);
    const bool pend = s_ctl_req.pending && !s_z->dev.ctl.active;
    uint8_t ieee[8], act = 0, lvl = 0;
    uint32_t id = 0;
    if (pend) {
        memcpy(ieee, s_ctl_req.ieee, 8);
        act = s_ctl_req.act;
        lvl = s_ctl_req.level;
        id = s_ctl_req.id;
        s_ctl_req.pending = false;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (pend) {
        const zh_ctl_err_t err = zh_dev_ctl_start(&s_z->dev, ieee, (zh_ctl_act_t)act, lvl, now_ms());
        if (err != ZH_CTL_E_OK) {                  /* 没发出去：当场回 */
            taskENTER_CRITICAL(&s_mux);
            memset(&s_ctl_res, 0, sizeof(s_ctl_res));
            s_ctl_res.id = id;
            s_ctl_res.err = (int)err;
            taskEXIT_CRITICAL(&s_mux);
            xEventGroupSetBits(s_ev, BIT_CTL_DONE);
        } else {
            s_ctl_cur_id = id;
        }
    }
    const zh_ctl_t *c = &s_z->dev.ctl;
    if (c->active && c->result != ZH_CTL_R_NONE) {
        taskENTER_CRITICAL(&s_mux);
        memset(&s_ctl_res, 0, sizeof(s_ctl_res));
        s_ctl_res.id = s_ctl_cur_id;
        s_ctl_res.err = (int)ZH_CTL_E_OK;
        s_ctl_res.result = (uint8_t)c->result;
        s_ctl_res.zcl_status = c->zcl_status;
        s_ctl_res.status = c->status;
        s_ctl_res.ms = c->t_done - c->t_sent;
        taskEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "控制命令：结论 %u（%u ms，ZCL 状态 0x%02X、发送状态 0x%lX）", (unsigned)c->result, (unsigned)(c->t_done - c->t_sent),
                 (unsigned)c->zcl_status, (unsigned long)c->status);
        zh_dev_ctl_clear(&s_z->dev);
        snapshot();
        publish_devices(true);                     /* 设备读数视图有 500 ms 节流：强刷，调用方醒来回读的是确认之后的读数（审计） */
        xEventGroupSetBits(s_ev, BIT_CTL_DONE);
    }
}

/* ZIGBEE 灯（Z54，2026-10-24 用户定）：每圈算一次 zhub 的状态，变了才推给 led_ctrl。
   网络 up 先判（恢复后 fails 要稳定 60 s 才清零，别让在线的芯片还红着 —— 审计 ⚠️）；让出 UART（MG24 升级 / 基准测试）与链路没起来算"进行中"；
   连着失败（zh_host_failing）或没人挂起却一直认领不到 UART（claim_stuck）算芯片不应答。MG24 里没有网络时停在黄（"等操作"，同另三颗灯的黄） */
static void led_push(bool claim_stuck)
{
    static int s_last = -1;
    led_zb_hub_t st;
    const bool holding = atomic_load(&s_holding);
    if (holding && s_z->h.state == ZH_HOST_RUNNING && s_z->h.net == ZH_NET_UP)
        st = (s_z->h.win_state == ZH_WIN_OPENING || s_z->h.win_state == ZH_WIN_OPEN) ? LED_ZB_HUB_JOIN : LED_ZB_HUB_UP;
    else if ((holding && zh_host_failing(&s_z->h)) || (!holding && claim_stuck)) st = LED_ZB_HUB_FAIL;
    else st = LED_ZB_HUB_STARTING;
    if ((int)st == s_last) return;
    s_last = (int)st;
    led_ctrl_set_zigbee_hub(st);
}

static void zhub_task(void *arg)
{
    (void)arg;
    /* xTaskCreateWithCaps 在任务建好（而且可能已经跑起来）之后才写回句柄，而本任务优先级比 app_main 高、会先跑：
       这里先写上，zhub_suspend 的"没起"判断就不会在这个窗口里误判（第一轮审计） */
    s_task = xTaskGetCurrentTaskHandle();
    static const wd_task_spec_t spec = {
        .name = "zhub", .deadline_ms = ZHUB_WD_DEADLINE_MS, .severity = WD_SEV_FATAL, .recover = NULL,
    };
    s_wd = wd_register(&spec);                        /* 槽满返回 -1：wd_kick(-1) 什么都不做，任务照跑 */
    if (s_wd < 0) ESP_LOGW(TAG, "看门狗槽位满了：zhub 不受看门狗观察");
    load_devices();
    int spins = 0, claim_misses = 0;
    for (;;) {
        const bool want_suspend = atomic_load(&s_suspend_cnt) > 0;
        if (atomic_load(&s_holding) && want_suspend) {
            zh_host_stop(&s_z->h);                    /* 会话收摊（在途命令了结）、ASH 静音 */
            atomic_store(&s_holding, false);
            ezsp_bridge_zhub_release();
            ESP_LOGI(TAG, "让出 UART");
        }
        if (!atomic_load(&s_holding)) {
            if (want_suspend) {
                xEventGroupSetBits(s_ev, BIT_IDLE);
            } else {
                /* 先清 BIT_IDLE 再看计数：和 zhub_suspend（先加计数再等 BIT_IDLE）配成一对，谁先谁后都不会两边同时拿着 UART */
                xEventGroupClearBits(s_ev, BIT_IDLE);
                if (atomic_load(&s_suspend_cnt) == 0 && ezsp_bridge_zhub_claim() == ESP_OK) {
                    atomic_store(&s_holding, true);
                    atomic_store(&s_restart_req, false);
#if CONFIG_BITENGINE_FAULT_INJECTION
                    atomic_store(&s_fault_rst, false);
#endif
                    s_t_claimed = now_ms64();
                    s_ev_mark = zh_host_evidence_mark(&s_z->h);
                    claim_misses = 0;
                    uart_bridge_flush_input();       /* 别人用完留下的字节（bootloader 菜单、基准测试的回包）不是 ASH */
                    zh_host_start(&s_z->h, now_ms());
                    ESP_LOGI(TAG, "接管 UART：ASH 复位 → 握手 → networkInit");
                } else if (atomic_load(&s_suspend_cnt) == 0 && ++claim_misses == ZHUB_CLAIM_WARN) {
                    ESP_LOGW(TAG, "一直认领不到 MG24 的 UART（桥状态 %d）—— Zigbee 主机没在跑", (int)ezsp_bridge_get_state());
                }
            }
        }
        if (atomic_load(&s_holding)) {
            if (atomic_exchange(&s_restart_req, false)) zh_host_restart(&s_z->h, now_ms());
#if CONFIG_BITENGINE_FAULT_INJECTION
            if (atomic_exchange(&s_fault_rst, false)) {   /* TC-ZH07：背着 ASH 写 CAN + RST（ash-host.c 的线上字节同 zh_ash_core 的 RST），编排层不知道 */
                static const uint8_t rst[] = { ZH_ASH_CAN, ZH_ASH_CTRL_RST, 0x38, 0xBC, ZH_ASH_FLAG };
                const int w = uart_bridge_write(rst, (int)sizeof(rst), ZHUB_TX_TIMEOUT_MS);
                ESP_LOGW(TAG, "故障注入：背着 ASH 发了 RST（写出 %d 字节）—— 等 NCP 回 RSTACK、zhub 按 NCP 复位恢复", w);
            }
#endif
            const unsigned pj = atomic_exchange(&s_pj_req, 0u);
            if (pj) {
                const zh_pj_result_t r = zh_host_permit_join(&s_z->h, pj - 1u, now_ms());
                taskENTER_CRITICAL(&s_mux);
                s_pj_res.result = (int)r;
                s_pj_res.win_state = zh_host_win_name((zh_win_state_t)s_z->h.win_state);
                s_pj_res.left_s = zh_host_window_left_s(&s_z->h, now_ms());
                s_pj_res.seconds = s_z->h.win_s;
                s_pj_res.retry_s = r == ZH_PJ_QUOTA ? zh_host_quota_wait_s(&s_z->h, now_ms()) : 0;
                taskEXIT_CRITICAL(&s_mux);
                snapshot();                          /* 先刷快照再放调用方：它醒来就读快照等落定，读到旧的会把"正在开"当成"没开成"（修复复核 ❌） */
                xEventGroupSetBits(s_ev, BIT_PJ_DONE);
                if (r == ZH_PJ_OK) ESP_LOGI(TAG, "入网窗口：%s（%u s）", pj - 1u ? "开" : "关", pj - 1u);
            }
            ctl_service();
            diag_service(now_ms());
            uint32_t wait = zh_host_next_timeout_ms(&s_z->h, now_ms());
            if (wait > ZHUB_LOOP_MAX_MS) wait = ZHUB_LOOP_MAX_MS;
            spins = wait ? 0 : spins + 1;
            if (spins > ZHUB_SPIN_LIMIT) { wait = 1; spins = 0; }
            const int n = uart_rx(wait);
            const uint32_t now = now_ms();
            if (n > 0) {
                zh_host_rx(&s_z->h, s_z->rx, (size_t)n, now);
                ezsp_bridge_note_uart((uint32_t)n, 0, false);
            }
            zh_host_tick(&s_z->h, now);
            zh_host_tick(&s_z->h, now);                /* 上一次 tick 里交回的结果可能让设备层又排了一条（地址表没查到 → IEEE_addr_req）：这一拍就交出去 */
            uart_tx(now);
        } else {
            vTaskDelay(pdMS_TO_TICKS(ZHUB_LOOP_MAX_MS));
        }
        zh_host_expire_records(&s_z->h, now_ms());   /* 挂起时也作废满一小时的开窗记录（32 位毫秒约 49.7 天回绕，zh_host_core.h） */
        zh_dev_set_net_up(&s_z->dev, s_z->h.state == ZH_HOST_RUNNING && s_z->h.net == ZH_NET_UP);   /* 网络不是 UP 不推进访谈 */
        if (s_z->h.have_params) zh_dev_set_ext_pan(&s_z->dev, s_z->h.params.ext_pan_id);   /* 主动发现过滤别的网的邻居条目（M6 第一步）；NCP 复位清了 have 也不撤：
                                                                                          zhub 不建网，扩展 PAN 不会变（同 EUI64，审计 ⚠️：撤了过滤就有一段空窗） */
        if (s_z->h.have_eui64) zh_dev_set_own_eui(&s_z->dev, s_z->h.eui64);   /* 写 IAS 设备的 CIE 地址用（CONFIGURE 第一步）；同一颗芯片 EUI64 不变，链路复位清了 have 也不用撤 */
        zh_dev_tick(&s_z->dev, now_ms());          /* 查询期限、访谈、年龄封顶：挂起 / 没拿到 UART 时也要跑（封顶停了 zhub_dev_list 的减法会回绕）；时刻现取，别用这一圈开头的 */
        publish_devices(false);
        log_changes();
        snapshot();
        led_push(atomic_load(&s_suspend_cnt) == 0 && claim_misses >= ZHUB_CLAIM_WARN);
        wd_kick(s_wd);                                /* 每圈都打：MG24 不应答只会让编排层退避，不会让这里停（Z45） */
    }
}

esp_err_t zhub_start(void)
{
    if (s_task) return ESP_OK;
    s_z = heap_caps_calloc(1, sizeof(*s_z), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_z) {
        ESP_LOGE(TAG, "上下文分配失败（PSRAM %u B）", (unsigned)sizeof(*s_z));
        return ESP_ERR_NO_MEM;
    }
    s_ev = xEventGroupCreate();
    s_pj_lock = xSemaphoreCreateMutex();
    s_dev_lock = xSemaphoreCreateMutex();
    s_ctl_lock = xSemaphoreCreateMutex();
    if (!s_ev || !s_pj_lock || !s_dev_lock || !s_ctl_lock) {
        if (s_ev) vEventGroupDelete(s_ev);
        if (s_pj_lock) vSemaphoreDelete(s_pj_lock);
        if (s_dev_lock) vSemaphoreDelete(s_dev_lock);
        if (s_ctl_lock) vSemaphoreDelete(s_ctl_lock);
        s_ev = NULL;
        s_pj_lock = s_dev_lock = s_ctl_lock = NULL;
        heap_caps_free(s_z);
        s_z = NULL;
        return ESP_ERR_NO_MEM;
    }
    const zh_host_ops_t ops = { .on_net = NULL, .on_callback = on_host_callback, .ctx = NULL, .on_app_result = on_app_result };
    zh_host_init(&s_z->h, &ops);
    const zh_dev_ops_t dops = { .save = dev_save, .event = dev_event, .send = dev_send, .zdo_seq = dev_zdo_seq, .ctx = NULL };
    zh_dev_init(&s_z->dev, &dops);
    taskENTER_CRITICAL(&s_mux);
    s_snap.started = false;
    s_snap.state = zh_host_state_name(ZH_HOST_IDLE);
    s_snap.net = zh_host_net_name(ZH_NET_UNKNOWN);
    s_snap.last_fail = zh_host_fail_name(ZH_HOST_FAIL_NONE);
    s_snap.win_state = zh_host_win_name(ZH_WIN_CLOSED);
    taskEXIT_CRITICAL(&s_mux);
    /* 钩子先于任务登记：任务一起来就会认领 UART，之后任何 ota_claim 都要能请它让出 */
    ezsp_bridge_set_zhub_hooks(zhub_suspend, zhub_resume);
    if (xTaskCreateWithCaps(zhub_task, "zhub", ZHUB_STACK, NULL, ZHUB_PRIO, &s_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "任务建不起来（PSRAM 栈 %d B）", ZHUB_STACK);
        ezsp_bridge_set_zhub_hooks(NULL, NULL);
        vEventGroupDelete(s_ev);
        s_ev = NULL;
        vSemaphoreDelete(s_pj_lock);
        vSemaphoreDelete(s_dev_lock);
        vSemaphoreDelete(s_ctl_lock);
        s_pj_lock = s_dev_lock = s_ctl_lock = NULL;
        heap_caps_free(s_z);
        s_z = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
