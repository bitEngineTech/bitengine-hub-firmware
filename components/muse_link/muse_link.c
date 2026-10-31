/* muse_link —— Muse 会话的驱动（见 include/muse_link.h；docs/15_muse_direct_design_v1.md 4.1–4.13）。
 *
 * 结构：会话任务 muse_sess 独自持有状态机（muse_link_core）。阻塞的活（fetch_vms、刷新令牌、跑会话）在任务主循环里一件件做，
 * 做完把结果翻译成事件喂回状态机；会话期间 muse_session 通过钩子（muse_link_priv.h）回到这里：建立 → 发注册；来消息 →
 * 判注册应答 / 分派调用 / 处理解绑事件；每圈轮询 → 处理外部事件、心跳、注册超时、看门狗、推送；请求流回包 → 推送的结果。
 * 外部事件只投队列（S2）。
 *
 * 线程：除 muse_link_init/start/get_status/set_* 之外的函数都只在会话任务里调；状态快照 s_st 由 s_st_lock 保护。
 * 本任务的栈在 PSRAM：不碰 flash（凭据经 hub_store 的队列落盘），不调可能写 NVS 的东西（psram_task.h 规则 1）。 */
#include "muse_link.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "sdkconfig.h"

#include "ble_window.h"
#include "hub_audit.h"
#include "hub_call.h"
#include "hub_cmds.h"
#include "hub_events.h"
#include "hub_jobs.h"
#include "hub_json.h"
#include "hub_params.h"
#include "hub_policy.h"
#include "hub_registry.h"
#include "hub_sanitize.h"
#include "muse_cfg.h"
#include "muse_cfg_core.h"
#include "muse_link_core.h"
#include "muse_link_priv.h"
#include "muse_proto.h"
#include "muse_push_core.h"
#include "muse_session.h"
#include "network_mgr.h"
#include "vm_api.h"
#include "vm_connect.h"
#include "watchdog.h"
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
#include "matter_ctrl.h"      /* 后端就绪位：控制器在跑才注册 hub.matter.commission */
#endif
#if CONFIG_BITENGINE_ENABLE_ZHUB
#include "zhub.h"           /* current_backends：zhub_started()（C17） */
#endif

static const char *TAG = "muse_link";

#define SESS_STACK          12288                   /* 4.6：PSRAM 栈 */
#define SESS_PRIO           4
#define EVQ_LEN             16
#define WD_DEADLINE_MS      180000                  /* 4.6：刷新最坏约 127 s，取 ≥ 180 s */
#define FLUSH_TMO_MS        2000
#define HEARTBEAT_MS        (24ULL * 3600 * 1000)   /* link.heartbeat 24 h（4.6） */
#define TOKEN_PERIOD_MS     (30ULL * 24 * 3600 * 1000)   /* 令牌 30 天主动刷新一次（4.2 注 4；按开机时长算，同上游） */
#define MAX_WAIT_MS         10000                   /* 主循环最多等这么久就转一圈（kick 看门狗） */
#define RESYNC_MS           10000                   /* 按现值补一次外部状态的周期（见 resync） */
#define BOOT_RETRY_MS       1000                    /* 开机读开关 / 配对状态读不到时的重试间隔（10 次后放宽到 RESYNC_MS） */
#define CFG_RETRY_MS        5000                    /* 配置值一时读不到时，多久再读一次（会话轮询 100 ms 一圈，别每圈都等存储） */
#define BEARER_MAX          4096
#define PUSH_PATH           "/chat/stream"
#define PUSH_TIMEOUT_MS     60000                   /* 一条推送等回包最多这么久（同上游：ESP32 REPLY_TIMEOUT_US、Linux SDK REQUEST_TIMEOUT_S 都是 60 s） */
#define PUSH_RETRY_MS       30000                   /* 5xx / 超时 / 没发出去之后，隔这么久再发下一条（不对 api 形成快速重试） */
#define PUSH_BODY_MAX       2048                    /* 请求体：512 B 句子转义后最多约 1 KB + 字段 */
#define PUSH_REPLY_MAX      1024                    /* 只为日志里报 message_id；更长的回包截断，不影响判定 */

/* 投给会话任务的消息：状态机事件，或"配置变了" */
typedef struct {
    uint8_t kind;          /* MSG_EVENT / MSG_CFG */
    uint8_t ev;            /* mle_t */
} ml_msg_t;
enum { MSG_EVENT = 0, MSG_CFG = 1 };

/* 大块的状态放 PSRAM（内部常驻增量 ≤ 2 KB，4.12；本构建没开外部内存的 .bss 段，所以在 init 里一次分配）。
   下面的宏让其余代码照常写 s_st / s_mlc …… */
typedef struct {
    muse_status_t st;                               /* 状态快照：别的任务经 s_st_lock 读 */
    mlc_t         mlc;                              /* 以下只在会话任务里 */
    hub_rl_t      rl;
    char          reg_id[MP_UUID_LEN + 1];
    char          vm_id[128];
    char          host[MUSE_URL_MAX + 1];
    char          node_id[24];
    char          version[32];
    mpq_t         mpq;                              /* 推送队列（4.9） */
    char          chat_sid[MUSE_CHAT_SID_LEN + 1];  /* 侧边会话 id（4.4），第一次推侧边会话时取 / 生成 */
    /* 结论晚到的调用（作业，6.6；hub.matter.commission）：作业 id → link.invoke 的 id。作业结束时在会话任务里发 link.result；
       会话断了就作废（新会话里那个 id 没人认） */
    struct { uint32_t job; char inv_id[64]; } pend[4];
    hjc_job_t     jobv;                             /* 取结论用的拷贝（约 650 B，别放栈上） */
} ml_big_t;
static ml_big_t *s_big;
#define s_st      (s_big->st)
#define s_mlc     (s_big->mlc)
#define s_rl      (s_big->rl)
#define s_reg_id  (s_big->reg_id)
#define s_vm_id   (s_big->vm_id)
#define s_host    (s_big->host)
#define s_node_id (s_big->node_id)
#define s_version (s_big->version)
#define s_mpq     (s_big->mpq)
#define s_chat_sid (s_big->chat_sid)

/* ---- 别的任务也会碰的 ---- */
static QueueHandle_t     s_q;
static SemaphoreHandle_t s_st_lock;
static bool              s_inited, s_started;
static volatile bool     s_cfg_dirty;              /* config_changed 置位、会话任务清：不靠队列，满了也丢不掉 */
static volatile bool     s_rereg_dirty;            /* reregister 置位、会话任务清（同上） */
static volatile bool     s_test_push;              /* test_push 置位、会话任务清（同上） */
static volatile bool     s_test_push_main;         /* 这条测试推送进主会话（test_push 置、会话任务读） */
/* 推送一律进主会话（设计 6.7 去向一列，2026-10-23 用户定）：上板对照实验 —— 推进侧边会话（chat_sid）的在 Muse App 里找不到，
   推进主会话的看得到；原先只有 ALARM 进主会话，门磁 / 人体、离线、入网、网关故障、低电量用户一条都收不到。侧边会话的路径留着
   （测试推送 ?main=0 还能走），等 Meta 那边弄清侧边会话怎么显示再说 */
#define MUSE_PUSH_MAIN_CHAT true
static volatile bool     s_wake_pending;           /* 已投了一条叫醒、会话任务还没处理：别再投（16 格的事件队列不让叫醒挤满） */

/* ---- 只在会话任务里 ---- */
static int       s_wd = -1;
static int64_t   s_timer_deadline_us;               /* 0 = 没有定时器 */
static bool      s_end_session;                     /* 钩子里决定结束会话 */
static bool      s_do_fetch, s_do_refresh, s_do_connect;
static bool      s_bg_refresh_due;                  /* 在线时到了 30 天：会话结束后、下次取 VM 前顺手刷新（见 do_fetch） */
static char     *s_reg_buf;                         /* MP_REGISTER_MAX + 1，PSRAM */
static char     *s_res_buf;                         /* MP_RESULT_MAX，PSRAM */
static uint8_t   s_reg_perm, s_reg_backends;
static bool      s_hb_ok, s_hb_sent;
static uint64_t  s_hb_last_ms;
static uint64_t  s_tok_ms;                          /* 令牌上次刷新（或开机）的时刻，开机毫秒 */
static uint64_t  s_resync_ms;
static uint64_t  s_cfg_retry_ms;                    /* 配置值上次读失败的时刻 */
static bool      s_cfg_loaded;                      /* SDK token / API 地址 / noise_host 这次开机确定读到过一次 */
static bool      s_clear_pending;                   /* 清配对凭据失败了，resync 接着清 */
static bool      s_sdk_tok_set;                     /* 配置里确实有 SDK token（setup_vm_api 读到的） */
static uint64_t  s_rereg_at_ms;                     /* 注册时权限位读不准：到这个时刻重注册一次（0 = 没排） */
/* 推送（4.9）：同一时刻最多一条在途 */
static char     *s_push_body;                       /* PUSH_BODY_MAX，PSRAM */
static char     *s_px_buf;                          /* PUSH_REPLY_MAX + 1，PSRAM：在途那条的回包 */
static int64_t   s_px_id;                           /* 在途推送的请求流 id；0 = 没有 */
static int       s_px_idx;                          /* 它在 s_mpq 里的槽 */
static int       s_px_status;                       /* 应答头的 HTTP 状态；0 = 还没到，-1 = 流被复位 */
static size_t    s_px_len;
static uint64_t  s_px_deadline_ms;
static uint64_t  s_push_not_before_ms;              /* 失败后到这个时刻才发下一条 */
static uint8_t   s_sc_seen;                         /* BLE 窗口后的 Thread 自检：0 没看过，1 这次开机看见它在跑，2 处理完了 */
static char     *s_pend_acc, *s_pend_ref;           /* 刷新拿到、但还没落盘成功的新令牌（PSRAM）：先用它们，resync 接着存 */
static char     *s_bearer;                          /* 当前 VM 的 vm_auth_token（PSRAM，只放内存，4.5） */

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

static bool net_online(void)
{
    const net_state_t s = network_mgr_get_state();
    return s == NET_STATE_ETH_ONLINE || s == NET_STATE_WIFI_ONLINE || s == NET_STATE_BOTH_ONLINE;
}

/* 内存闸门（4.6、N2） */
static bool mem_ok(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= CONFIG_BITENGINE_MUSE_MIN_INTERNAL_FREE
           && heap_caps_get_free_size(MALLOC_CAP_DMA) >= CONFIG_BITENGINE_MUSE_MIN_DMA_FREE;
}

/* 在跑的后端与就绪位（6.3 的 needs；CHECKLIST C17）。zhub 任务在跑就报 ZHUB —— 注册 hub.device.update（名字 / 房间 / notify 只靠 zhub 的设备表与 hub_meta）；
   连同 CONTROL（8.11 控制路径，M5 第七步：常电设备的开 / 关 / 切换 / 亮度）—— 注册 hub.device.control。
   ZB_JOIN（Z21：容量口径 C10 还没定）不报：报了就把前提没满足的 hub.zigbee.permit_join 注册出去。matter_ctrl 随 M7 */
static uint8_t current_backends(void)
{
    uint8_t b = 0;
#if CONFIG_BITENGINE_ENABLE_ZHUB
    if (zhub_started()) b |= HUB_BACKEND_ZHUB | HUB_BACKEND_CONTROL;   /* 只读一位（C17 审计）；控制路径 M5 第七步做了：注册 hub.device.control */
#endif
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
    /* matter_ctrl 起来了（开机后约 40 s，晚于 Muse 第一次注册 —— 位一变 resync 就重注册）：注册 hub.matter.commission（M7 第三步）；
       Matter 设备的控制路径 M7 第六步做了：也报 CONTROL（zhub 没起时 hub.device.control 照样注册） */
    matter_ctrl_status_t mc;
    matter_ctrl_get_status(&mc);
    if (mc.state == MATTER_CTRL_RUNNING) b |= HUB_BACKEND_MCTRL | HUB_BACKEND_CONTROL;
#endif
    return b;
}

static void clear_bearer(void)
{
    if (s_bearer) {
        mbedtls_platform_zeroize(s_bearer, BEARER_MAX);
        heap_caps_free(s_bearer);
        s_bearer = NULL;
    }
}

static void free_secret(char **p)
{
    if (*p) {
        mbedtls_platform_zeroize(*p, strlen(*p));
        heap_caps_free(*p);
        *p = NULL;
    }
}

static char *dup_secret(const char *s)
{
    const size_t n = strlen(s) + 1;
    char *d = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (d) memcpy(d, s, n);
    return d;
}

/* vm_api.c 是上游原样，它的 free 不清零：令牌字符串先在这里抹掉 */
static void zero_str(char *p)
{
    if (p) mbedtls_platform_zeroize(p, strlen(p));
}

static void tokens_free(vm_device_tokens_t *t)
{
    zero_str(t->access_token);
    zero_str(t->refresh_token);
    vm_device_tokens_free(t);
}

static void vms_free(vm_info_t *vms, int n)
{
    for (int i = 0; i < n; i++) zero_str(vms[i].vm_auth_token);
    vm_list_free(vms, n);
}

/* ---------------------------------------------------------------- 状态快照 */

static void st_update(const char *detail)
{
    if (!s_st_lock || xSemaphoreTake(s_st_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;
    if (s_st.state != s_mlc.st) {
        s_st.state = s_mlc.st;
        s_st.state_since_s = (uint32_t)(esp_timer_get_time() / 1000000);
    }
    s_st.registrations = s_mlc.registrations;
    s_st.reg_backends = s_reg_backends;
    s_st.enabled = s_mlc.enabled;
    s_st.paired = s_mlc.paired;
    s_st.heartbeat = s_hb_ok;
    const char *why = mlc_reason_str(s_mlc.reason);
    if (detail && why[0]) {
        snprintf(s_st.last_error, sizeof(s_st.last_error), "%s: %s", why, detail);
    } else if (detail) {
        snprintf(s_st.last_error, sizeof(s_st.last_error), "%s", detail);
    } else {
        snprintf(s_st.last_error, sizeof(s_st.last_error), "%s", mlc_reason_str(s_mlc.reason));
    }
    xSemaphoreGive(s_st_lock);
}

/* 推送过期丢弃记审计（4.9）：一批记一条，结果码带条数 */
static void audit_expired(void)
{
    static uint32_t seen;
    if (s_mpq.dropped_expired <= seen) return;
    char res[24];
    snprintf(res, sizeof(res), "expired_%u", (unsigned)(s_mpq.dropped_expired - seen));
    seen = s_mpq.dropped_expired;
    hub_audit_record(HAU_SRC_MUSE, "push.expired", NULL, res, 0);
}

/* 采样进状态快照：会话任务栈用得最深时还剩多少（上板确认 4.6 的栈尺寸；esp_http_client + mbedTLS 握手没法静态量）、推送计数 */
static void st_sample(void)
{
    const uint32_t hwm = (uint32_t)uxTaskGetStackHighWaterMark(NULL);   /* ESP-IDF 的 FreeRTOS 栈单位是字节 */
    hub_events_stats_t es;
    hub_events_get_stats(&es);
    audit_expired();
    if (s_st_lock && xSemaphoreTake(s_st_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_st.stack_min_free = hwm;
        s_st.pushes_sent = s_mpq.sent;
        s_st.pushes_dropped = mpq_dropped_total(&s_mpq) + es.push_overflow;   /* 队列满 / 过期 / 被拒 / 被清 + 管线入口溢出 */
        s_st.pushes_queued = (uint32_t)mpq_count(&s_mpq);
        xSemaphoreGive(s_st_lock);
    }
}

static void st_count(uint32_t *field)
{
    if (s_st_lock && xSemaphoreTake(s_st_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        (*field)++;
        xSemaphoreGive(s_st_lock);
    }
}

/* ---------------------------------------------------------------- 状态机驱动 */

static void send_register(void);
static void push_session_ended(void);
static void push_clear(void);

static void apply(mlc_out_t o, mle_t ev)
{
    if (o.unexpected) {
        ESP_LOGW(TAG, "状态 %s 收到不该来的事件 %s，丢弃", mlc_state_str(o.from), mlc_event_str(ev));
        return;
    }
    if (o.from != o.to) ESP_LOGI(TAG, "%s --%s--> %s", mlc_state_str(o.from), mlc_event_str(ev), mlc_state_str(o.to));
    if (o.timer_ms > 0) s_timer_deadline_us = esp_timer_get_time() + (int64_t)o.timer_ms * 1000;
    else if (o.timer_ms == 0) s_timer_deadline_us = 0;
    if (o.act & MLA_CLOSE) s_end_session = true;
    if (o.to == MUSE_ST_DISABLED || o.to == MUSE_ST_UNPAIRED) {
        clear_bearer();                               /* 关掉 / 解绑后不留 VM 凭据在内存里 */
        if (o.from != o.to) push_clear();             /* 也不留没推出去的事件：重新打开 / 换账号配对后不该再推（N5，第五步审计） */
    }
    if (o.act & MLA_CLEAR_CREDS) {
        clear_bearer();
        free_secret(&s_pend_acc);
        free_secret(&s_pend_ref);
        s_clear_pending = muse_cfg_clear_pairing(FLUSH_TMO_MS) != ESP_OK;
        if (s_clear_pending) ESP_LOGE(TAG, "清配对凭据失败：resync 每 10 s 接着清（清掉之前重启会按旧令牌重新上线）");
    }
    if (o.act & MLA_FETCH_VM) s_do_fetch = true;
    if (o.act & MLA_REFRESH) s_do_refresh = true;
    if (o.act & MLA_CONNECT) s_do_connect = true;
    if (o.act & MLA_REGISTER) send_register();
    if (o.act & MLA_BG_REFRESH) s_bg_refresh_due = true;
    st_update(s_clear_pending ? "credentials not deleted yet, retrying" : NULL);
}

/* 每一步之前都把网络与内存的现状告诉状态机（不止 TIMER）：事件可能丢（队列满）或晚到，enter_fetch 的内存闸门要看现值；
   NET_* / MEM_* 事件本身在 mlc_step 里再覆盖一次，语义不变 */
static mlc_out_t step(mle_t ev)
{
    mlc_set_env(&s_mlc, net_online(), mem_ok());
    mlc_out_t o = mlc_step(&s_mlc, ev, now_ms());
    apply(o, ev);
    return o;
}

/* ---------------------------------------------------------------- VM 与令牌 */

/* 读 SDK token / API 地址 / noise_host 喂给 vm_api。一时读不到的那一样**保留上一次的值**、s_cfg_dirty 留着过会儿再读；
   三样都确定读到过一次（s_cfg_loaded）之前不取 VM —— 拿 Kconfig 默认值顶，配对下发的地址和默认不同时会 401 →
   刷新被拒 → 清凭据（第四步两轮审计 ❌）。vm_api 的 setter 写静态缓冲不加锁：只在本任务里调（UPSTREAM.md 调用方义务）。 */
static bool setup_vm_api(void)
{
    bool ok = true;
    char tok[MUSE_SDK_TOKEN_LEN + 1];
    const esp_err_t te = muse_cfg_get_sdk_token(tok, sizeof(tok));
    if (te == ESP_OK || te == ESP_ERR_NOT_FOUND) {
        vm_api_set_sdk_token(te == ESP_OK ? tok : "");
        s_sdk_tok_set = te == ESP_OK;
    } else {
        ok = false;
    }
    mbedtls_platform_zeroize(tok, sizeof(tok));
    char url[MUSE_URL_MAX + 1];
    if (muse_cfg_get_api_url(url, sizeof(url)) == ESP_OK) vm_api_set_base_url(url);
    else ok = false;
    char host[MUSE_URL_MAX + 1];
    if (muse_cfg_get_noise_host(host, sizeof(host)) == ESP_OK) memcpy(s_host, host, sizeof(s_host));
    else ok = false;
    /* 推送类别开关（6.7）：不挡取 VM；一时读不到就沿用现值、过会儿再读（不回落成默认） */
    uint16_t nm;
    bool notify_ok = muse_cfg_read_notify(&nm) == ESP_OK;
    if (notify_ok) hub_events_set_notify(nm);
    if (ok) s_cfg_loaded = true;
    if (!ok || !notify_ok) {
        s_cfg_dirty = true;
        s_cfg_retry_ms = now_ms();
        ESP_LOGW(TAG, "一时读不到 Muse 的配置值：沿用上一次的，%d s 后再读", CFG_RETRY_MS / 1000);
    }
    return ok;
}

/* 令牌：刷新拿到但没落盘成功的新令牌优先（服务端可能已经作废了旧的 refresh token，丢了新的就只能重新配对） */
static esp_err_t load_pending(const char *p, char *out, size_t cap)
{
    const size_t n = strlen(p);
    if (n >= cap) return ESP_ERR_INVALID_SIZE;        /* keep_tokens 已经按上限校验过，不会走到；走到也不能截断着用 */
    memcpy(out, p, n + 1);
    return ESP_OK;
}

static esp_err_t load_access(char *out, size_t cap)
{
    return s_pend_acc ? load_pending(s_pend_acc, out, cap) : muse_cfg_get_access_token(out, cap);
}

static esp_err_t load_refresh(char *out, size_t cap)
{
    return s_pend_ref ? load_pending(s_pend_ref, out, cap) : muse_cfg_get_refresh_token(out, cap);
}

/* 刷新拿到的一对新令牌：落盘；落不下就留在内存里（resync 接着存）。两样都没办到（落盘失败且 PSRAM 拿不到）返回 false。 */
static bool keep_tokens(const char *acc, const char *ref)
{
    /* 服务端给的令牌超长 / 带不可见字符：存不进、也不能截断着用（截断的令牌 401 → 当成被拒 → 清凭据）。按可重试的错处理 */
    if (!muse_token_valid(acc, MUSE_ACCESS_TOKEN_MAX) || !muse_token_valid(ref, MUSE_REFRESH_TOKEN_MAX)) {
        ESP_LOGE(TAG, "刷新拿到的令牌不合格（超长或带不可见字符）：不用");
        return false;
    }
    if (muse_cfg_save_tokens(acc, ref, FLUSH_TMO_MS) == ESP_OK) {
        free_secret(&s_pend_acc);
        free_secret(&s_pend_ref);
        return true;
    }
    char *a = dup_secret(acc), *r = dup_secret(ref);
    if (!a || !r) {
        free_secret(&a);
        free_secret(&r);
        ESP_LOGE(TAG, "新令牌落盘失败、内存也放不下：这一对丢了");
        return false;
    }
    free_secret(&s_pend_acc);
    free_secret(&s_pend_ref);
    s_pend_acc = a;
    s_pend_ref = r;
    ESP_LOGE(TAG, "新令牌落盘失败：先放内存里用，resync 接着存");
    return true;
}

/* 选 VM（4.5）：用户指定的 → is_default → 第一台。
   "指定的"存的是 device.set_vm 的 url 经 vm_derive_id 取出的主机名标签，所以这里也拿每台的 vm_url 用**同一个函数**取标签来比
   （不用 vm_connect_params 给的 id：列表项带 vm_id 字段时它优先用那个，和 url 的标签不一定是一种东西）。
   上游存的是整条 url、按 url 全等找（app.c:1046-1051 vm_find_by_url）；我们只存标签（4.4 的 vm_id 键），效果相同。 */
static esp_err_t pick_vm(const vm_info_t *vms, int n, const vm_info_t **out)
{
    *out = NULL;
    char want[MUSE_VM_ID_MAX + 1];
    const esp_err_t e = muse_cfg_get_vm_id(want, sizeof(want));
    if (e != ESP_OK && e != ESP_ERR_NOT_FOUND) return e;   /* 一时读不到：别悄悄连到默认 VM 上，按可重试的错退避 */
    if (e == ESP_OK) {
        for (int i = 0; i < n; i++) {
            char id[MUSE_VM_ID_MAX + 1];
            if (vm_derive_id(vms[i].vm_url, id, sizeof(id)) > 0 && strcmp(id, want) == 0) {
                *out = &vms[i];
                return ESP_OK;
            }
        }
        ESP_LOGW(TAG, "指定的 VM 不在列表里了，回默认 VM");
    }
    const vm_info_t *d = vm_find_default(vms, n);
    *out = d ? d : (n > 0 ? &vms[0] : NULL);
    return ESP_OK;
}

static void do_fetch(void)
{
    if (!s_cfg_loaded && !setup_vm_api()) {
        step(MLE_VM_ERR);                             /* 配置值还没确定读到过：不拿默认值去取 VM，退避再来 */
        st_update("storage busy");
        return;
    }
    if (!s_sdk_tok_set) {
        /* 用户删了 SDK token：取 VM / 刷新都会 401，vm_api 把它和 403 一样当鉴权失败，一路走到"刷新被拒 → 撤销 → 清凭据"。
           删 token 不该等于解绑：不取 VM，退避等用户把 token 设回来（核实审计） */
        step(MLE_VM_ERR);
        st_update("SDK token not set");
        return;
    }
    if (s_bg_refresh_due) {
        /* 在线时到了 30 天的主动刷新：在这里（会话外）做，被拒保留旧令牌（上游 allow_stale=true）。照上游 app.c:909-928：
           记"试过"的时刻而不是"成功"的时刻（失败不急，旧令牌还能用，每次重连都重试会把端点打爆）；access token 传
           **NULL** —— 闲置一阵后再出示它，服务端回 200 却铸出一个所有端点都拒的新令牌，下次就会被当成撤销、清凭据
           （第四步第一轮审计 ❌）。 */
        s_bg_refresh_due = false;
        s_tok_ms = now_ms();
        char *ref = heap_caps_calloc(1, MUSE_REFRESH_TOKEN_MAX + 1, MALLOC_CAP_SPIRAM);
        const esp_err_t re = ref ? load_refresh(ref, MUSE_REFRESH_TOKEN_MAX + 1) : ESP_ERR_NO_MEM;
        if (re != ESP_OK) ESP_LOGW(TAG, "定期刷新这次跳过：读不到 refresh token（%s），30 天后再试", esp_err_to_name(re));
        if (re == ESP_OK) {
            vm_device_tokens_t t = {0};
            vm_token_refresh_method_t m;
            if (vm_api_refresh_device_token(NULL, ref, s_node_id, &t, &m) == 0) {
                (void)keep_tokens(t.access_token, t.refresh_token);
            } else {
                ESP_LOGW(TAG, "定期刷新没成功，继续用旧令牌");
            }
            tokens_free(&t);
        }
        if (ref) { mbedtls_platform_zeroize(ref, MUSE_REFRESH_TOKEN_MAX + 1); heap_caps_free(ref); }
        wd_kick(s_wd);                                /* 刷新最坏约 127 s、下面的 fetch_vms 约 63 s：两段之间喂一次 */
    }

    char *acc = heap_caps_calloc(1, MUSE_ACCESS_TOKEN_MAX + 1, MALLOC_CAP_SPIRAM);
    if (!acc) {
        step(MLE_MEM_LOW);
        return;
    }
    const esp_err_t ge = load_access(acc, MUSE_ACCESS_TOKEN_MAX + 1);
    if (ge != ESP_OK) {
        mbedtls_platform_zeroize(acc, MUSE_ACCESS_TOKEN_MAX + 1);
        heap_caps_free(acc);
        if (ge == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "没有 access token：走刷新");
            step(MLE_VM_AUTH);
        } else {
            ESP_LOGW(TAG, "一时读不到 access token（%s）：按可重试的错退避", esp_err_to_name(ge));   /* 不能走刷新 → 撤销 */
            step(MLE_VM_ERR);
            st_update("storage busy");
        }
        return;
    }
    vm_info_t vms[VM_API_MAX_VMS];
    memset(vms, 0, sizeof(vms));
#if CONFIG_BITENGINE_DIAG_MUSE_FAULT
    /* TC-MU09 故障注入（诊断构建；注入点在 fetch_vms：WS 升级的 401 进不了 REFRESHING）。方式 1：第一次取 VM 当 401（刷新那步也注入
       被拒 → 撤销）；方式 2：头两次取 VM 当 401（中间的刷新真去发、成功）→ "新凭据也不认"，退避、保留配对（设计 14.4） */
    static int s_fault_fetches;
    int n;
    if (s_fault_fetches < CONFIG_BITENGINE_MUSE_FAULT_MODE) {
        s_fault_fetches++;
        ESP_LOGW(TAG, "【故障注入】fetch_vms 当 401（TC-MU09 方式 %d，第 %d 次）", CONFIG_BITENGINE_MUSE_FAULT_MODE, s_fault_fetches);
        n = VM_API_ERR_AUTH;
    } else {
        n = vm_api_fetch_vms(acc, vms, VM_API_MAX_VMS);
    }
#else
    const int n = vm_api_fetch_vms(acc, vms, VM_API_MAX_VMS);
#endif
    mbedtls_platform_zeroize(acc, MUSE_ACCESS_TOKEN_MAX + 1);
    heap_caps_free(acc);
    if (n == VM_API_ERR_AUTH) {
        step(MLE_VM_AUTH);
        return;
    }
    if (n <= 0) {
        ESP_LOGW(TAG, "fetch_vms 失败（%d）", n);
        step(MLE_VM_ERR);
        return;
    }
    const vm_info_t *vm = NULL;
    if (pick_vm(vms, n, &vm) != ESP_OK) {
        vms_free(vms, n);
        ESP_LOGW(TAG, "一时读不到选中的 VM：退避重试");
        step(MLE_VM_ERR);
        return;
    }
    const char *tok = NULL;
    bool ok = vm && vm_connect_params(vm, s_vm_id, sizeof(s_vm_id), &tok) && tok && strlen(tok) < BEARER_MAX;
    if (ok) {
        clear_bearer();
        s_bearer = heap_caps_calloc(1, BEARER_MAX, MALLOC_CAP_SPIRAM);
        ok = s_bearer != NULL;
        if (ok) memcpy(s_bearer, tok, strlen(tok) + 1);
        if (s_st_lock && xSemaphoreTake(s_st_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            hub_sanitize_device_str(vm->vm_name ? vm->vm_name : "", vm->vm_name ? strlen(vm->vm_name) : 0,
                                    s_st.vm_name, sizeof(s_st.vm_name), sizeof(s_st.vm_name) - 1);
            xSemaphoreGive(s_st_lock);
        }
    }
    vms_free(vms, n);
    if (!ok) {
        ESP_LOGE(TAG, "选中的 VM 没有可用的连接参数");
        step(MLE_VM_ERR);
        return;
    }
    step(MLE_VM_OK);
}

/* REFRESHING：fetch_vms 刚 401 —— access token 传 NULL，直接用 refresh token（上游 app.c:918-927：再出示被拒的那个反而会
   让服务端铸出一个所有端点都拒的新令牌）；device_id 参数照上游传 node_id */
static void do_refresh(void)
{
    char *ref = heap_caps_calloc(1, MUSE_REFRESH_TOKEN_MAX + 1, MALLOC_CAP_SPIRAM);
    if (!ref) {
        step(MLE_REFRESH_ERR);
        return;
    }
    const esp_err_t ge = load_refresh(ref, MUSE_REFRESH_TOKEN_MAX + 1);
    if (ge != ESP_OK) {
        mbedtls_platform_zeroize(ref, MUSE_REFRESH_TOKEN_MAX + 1);
        heap_caps_free(ref);
        if (ge == ESP_ERR_NOT_FOUND) {
            step(MLE_REFRESH_REJECTED);               /* 确实没有 refresh token = 凭据没了 */
        } else {
            /* 一时读不到（hub_store 超时 / 没就绪）：按可重试的错退避。**不能**当成被拒 —— 那会清凭据、逼用户重新配对 */
            ESP_LOGW(TAG, "一时读不到 refresh token（%s）：退避重试", esp_err_to_name(ge));
            step(MLE_REFRESH_ERR);
        }
        return;
    }
    vm_device_tokens_t t = {0};
    vm_token_refresh_method_t m = VM_TOKEN_REFRESH_NONE;
#if CONFIG_BITENGINE_DIAG_MUSE_FAULT && CONFIG_BITENGINE_MUSE_FAULT_MODE == 1
    static bool s_fault_refresh_done;
    int rc;
    if (!s_fault_refresh_done) {
        s_fault_refresh_done = true;
        ESP_LOGW(TAG, "【故障注入】刷新当被拒（TC-MU09 方式 1：应进 UNPAIRED、清令牌）");
        rc = VM_API_ERR_AUTH;
    } else {
        rc = vm_api_refresh_device_token(NULL, ref, s_node_id, &t, &m);
    }
#else
    const int rc = vm_api_refresh_device_token(NULL, ref, s_node_id, &t, &m);
#endif
    mbedtls_platform_zeroize(ref, MUSE_REFRESH_TOKEN_MAX + 1);
    heap_caps_free(ref);
    if (rc == 0) {
        const bool kept = keep_tokens(t.access_token, t.refresh_token);   /* 落盘失败就先放内存里用 */
        tokens_free(&t);
        if (kept) {
            s_tok_ms = now_ms();
            step(MLE_REFRESH_OK);
        } else {
            step(MLE_REFRESH_ERR);
        }
        return;
    }
    tokens_free(&t);
    step(rc == VM_API_ERR_AUTH ? MLE_REFRESH_REJECTED : MLE_REFRESH_ERR);
}

static void do_connect(void)
{
    if (!s_bearer || !s_vm_id[0]) {
        ESP_LOGE(TAG, "没有 VM 凭据就要连：回去取");
        step(MLE_WS_ERR);
        return;
    }
    const msess_conn_t c = { s_host, s_vm_id, s_bearer };
    s_end_session = false;
    bool established = false;
    const msess_result_t r = msess_run(&c, &established);
    ESP_LOGI(TAG, "会话结束：%d（%s）", (int)r, established ? "建立过" : "没建立");
    push_session_ended();
    switch (r) {
    case MSESS_STOPPED:
        /* 正常情况下结束它的那一步已经转好了状态（离开会话态，或 CLOSE+CONNECT 回 CONNECTING 并排好重连）。
           兜底：还停在会话态又没排重连 = 没有哪一步接手，当断线处理，别卡在"会话已关、状态还说在线" */
        if (s_mlc.st == MUSE_ST_REGISTERING || s_mlc.st == MUSE_ST_ONLINE
            || (s_mlc.st == MUSE_ST_CONNECTING && !s_do_connect)) {
            ESP_LOGW(TAG, "会话结束了但状态还在 %s：按断线处理", mlc_state_str(s_mlc.st));
            step(MLE_WS_ERR);
        }
        break;
    case MSESS_AUTH_REJECTED: step(MLE_WS_AUTH); break;
    case MSESS_NO_MEM:        step(MLE_MEM_LOW); break;
    case MSESS_FAILED:
    default:                  step(MLE_WS_ERR); break;
    }
}

/* ---------------------------------------------------------------- 会话里的收发 */

static void reply_error(const char *id, const char *code, const char *msg)
{
    const int n = mp_build_result_error(s_res_buf, MP_RESULT_MAX, id, code, msg);
    if (n > 0 && !msess_send(s_res_buf, (size_t)n)) s_end_session = true;
}

static void send_register(void)
{
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    mp_format_uuid(rnd, s_reg_id);
    if (muse_cfg_read_perm(&s_reg_perm) != ESP_OK) {
        s_rereg_at_ms = now_ms() + CFG_RETRY_MS;      /* 读不准就按只读注册（N4），过会儿重注册一次，别让只读命令表粘住整个会话 */
    }
    s_reg_backends = current_backends();
    char name[MUSE_DISPLAY_NAME_MAX + 1];
    muse_cfg_get_display_name(name, sizeof(name));
    const mp_reg_info_t info = { s_node_id, name, s_version, CONFIG_BITENGINE_MUSE_DEVICE_FAMILY, CONFIG_BITENGINE_MUSE_MODEL_ID };
    const int n = mp_build_register(s_reg_buf, MP_REGISTER_MAX + 1, s_reg_id, &info, s_reg_perm, s_reg_backends);
    if (n < 0) {
        ESP_LOGE(TAG, "注册报文放不下 / 参数缺失，不注册");
        step(MLE_REG_ERR);
        return;
    }
    s_hb_ok = false;
    if (!msess_send(s_reg_buf, (size_t)n)) s_end_session = true;
    else ESP_LOGI(TAG, "已发 link.register（%d 字节，权限 0x%02x）", n, s_reg_perm);
}

/* 4 个管理命令（4.13）：只处理、不注册，不受权限位约束；同一时刻只有一个（会话任务串行，天然满足） */
static const char *handle_mgmt(mp_mgmt_t mg, const mp_invoke_t *inv)
{
    if (mg == MP_MGMT_UNPAIR) {
        /* 先删凭据并读回确认，确认后才回 accepted；再投 UNPAIRED_EVT（CLOSE 在先，先投会把回包一起关掉） */
        if (muse_cfg_clear_pairing(FLUSH_TMO_MS) != ESP_OK) {
            reply_error(inv->id, "storage_error", "the hub could not delete its credentials; try again");
            return "storage_error";
        }
        hj_t w;
        hj_init(&w, s_res_buf, MP_RESULT_MAX);
        mp_result_begin(&w, inv->id);
        hj_obj_begin(&w); hj_kbool(&w, "accepted", true); hj_obj_end(&w);
        mp_result_end(&w);
        const int n = hj_finish(&w);
        if (n > 0) (void)msess_send(s_res_buf, (size_t)n);
        step(MLE_UNPAIRED_EVT);
        return "ok";
    }
    if (mg == MP_MGMT_RESET_VM || mg == MP_MGMT_SET_VM) {
        char id[MUSE_VM_ID_MAX + 1] = "";
        if (mg == MP_MGMT_SET_VM) {
            const cJSON *u = inv->params ? cJSON_GetObjectItemCaseSensitive(inv->params, "url") : NULL;
            if (!cJSON_IsString(u) || !u->valuestring || !u->valuestring[0]) {
                reply_error(inv->id, "invalid_params", "device.set_vm requires url");
                return "invalid_params";
            }
            if (vm_derive_id(u->valuestring, id, sizeof(id)) == 0 || !muse_vm_id_valid(id)) {
                reply_error(inv->id, "invalid_params", "url is not a Muse VM address (wss://<vm>.<domain>/)");
                return "invalid_params";
            }
        }
        if (muse_cfg_set_vm_id(mg == MP_MGMT_SET_VM ? id : NULL) != ESP_OK) {
            reply_error(inv->id, "storage_error", "the hub could not save the VM choice");
            return "storage_error";
        }
        hj_t w;
        hj_init(&w, s_res_buf, MP_RESULT_MAX);
        mp_result_begin(&w, inv->id);
        hj_obj_begin(&w); hj_kbool(&w, "accepted", true); hj_obj_end(&w);
        mp_result_end(&w);
        const int n = hj_finish(&w);
        if (n > 0) (void)msess_send(s_res_buf, (size_t)n);
        step(MLE_VM_SWITCH);                          /* 关会话、按新选择重取 VM（上游 300 ms 后由工作任务做，这里回包已发出） */
        return "ok";
    }
    /* device.list_vms：要同步调 fetch_vms（最坏约 63 s，会话这期间不收发）。M3 先回 unavailable，留到工作任务（4.6 的待办） */
    reply_error(inv->id, "unavailable", "listing VMs from the hub is not supported yet");
    return "unavailable";
}

/* ---------------------------------------------------------------- 结论晚到的调用（作业，6.6） */

static bool pend_add(uint32_t job, const char *inv_id)
{
    if (!inv_id || strlen(inv_id) >= sizeof(s_big->pend[0].inv_id)) return false;
    for (size_t i = 0; i < sizeof(s_big->pend) / sizeof(s_big->pend[0]); i++) {
        if (s_big->pend[i].job) continue;
        s_big->pend[i].job = job;
        strcpy(s_big->pend[i].inv_id, inv_id);
        return true;
    }
    return false;
}

static void pend_clear(void) { memset(s_big->pend, 0, sizeof(s_big->pend)); }

/* 会话任务里：结束了的作业发 link.result（成功：作业的结果体原样作 payload；失败 / 超时：错误码 + 句子） */
static void pend_flush(void)
{
    for (size_t i = 0; i < sizeof(s_big->pend) / sizeof(s_big->pend[0]) && !s_end_session; i++) {
        if (!s_big->pend[i].job) continue;
        hjc_job_t *j = &s_big->jobv;
        if (!hub_job_get(s_big->pend[i].job, j)) {
            reply_error(s_big->pend[i].inv_id, "internal", "the hub lost track of this request; check the device list");
        } else if (j->state == HJC_RUNNING) {
            continue;
        } else if (j->state == HJC_DONE) {
            hj_t w;
            hj_init(&w, s_res_buf, MP_RESULT_MAX);
            mp_result_begin(&w, s_big->pend[i].inv_id);
            hj_raw(&w, j->result);
            mp_result_end(&w);
            const int n = hj_finish(&w);
            if (n < 0) reply_error(s_big->pend[i].inv_id, "internal", "the result did not fit");
            else if (!msess_send(s_res_buf, (size_t)n)) s_end_session = true;
        } else {
            reply_error(s_big->pend[i].inv_id, j->err_code[0] ? j->err_code : "internal", j->err_msg[0] ? j->err_msg : "the hub could not do that");
        }
        memset(&s_big->pend[i], 0, sizeof(s_big->pend[i]));
    }
}

static volatile bool s_job_wake;
/* hub_jobs 的监听者：结束作业的那个任务里调（CHIP 任务 / hub_svc），只置位、投一条叫醒 */
static void job_listener(uint32_t id)
{
    (void)id;
    s_job_wake = true;
    if (s_q) {
        const ml_msg_t m = { MSG_CFG, 0 };
        (void)xQueueSend(s_q, &m, 0);
    }
}

/* 一次调用：回包（结果或错误）发掉，返回给审计的结果码。顺序：认不认识 → 参数 → 限速 → 权限 → 处理函数。
   限速在权限之前（6.5 注记）：被拒的调用每次都要写审计 + log_store，放在后面会被一个刷屏的客户端把审计环冲掉 */
static const char *dispatch_inner(const mp_invoke_t *inv)
{
    const hub_cmd_t *cmd = NULL;
    mp_mgmt_t mg = MP_MGMT_NONE;
    const mp_disp_t d = mp_dispatch(inv->command, s_reg_perm, s_reg_backends, &cmd, &mg);
    if (d == MP_DISP_MGMT) return handle_mgmt(mg, inv);      /* 4.13：不受权限位约束，但每次都记审计 */
    if (d == MP_DISP_UNSUPPORTED) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%.48s is not a command of this hub", inv->command);
        reply_error(inv->id, "unsupported", msg);
        st_count(&s_st.invokes_failed);
        return "unsupported";
    }
    char err[128];
    if (!mp_check_params(cmd, inv->params, err, sizeof(err))) {
        reply_error(inv->id, "invalid_params", err);
        st_count(&s_st.invokes_failed);
        return "invalid_params";
    }
    const bool rl_exempt = hub_params_rl_exempt(cmd, inv->params);
    if (!rl_exempt && !hub_rl_take(&s_rl, cmd->rl, now_ms())) {
        reply_error(inv->id, "rate_limited", "too many requests to the hub; wait a few seconds and try again");
        st_count(&s_st.invokes_denied);
        return "rate_limited";
    }
    if (hub_policy_check_cmd(cmd, muse_cfg_get_perm(), HUB_SRC_MUSE) != HUB_CHK_OK) {   /* 纵深防御：权限可能在注册后变了 */
        reply_error(inv->id, "forbidden", "the hub's owner has not allowed this kind of command");
        st_count(&s_st.invokes_denied);
        return "forbidden";
    }
    const hub_cmd_fn fn = hub_cmds_find(cmd->name);
    if (!fn) {
        reply_error(inv->id, "unsupported", "this command is not available in this hub's firmware yet");
        st_count(&s_st.invokes_failed);
        return "unsupported";
    }
    hj_t w;
    hj_init(&w, s_res_buf, MP_RESULT_MAX);
    mp_result_begin(&w, inv->id);
    hub_call_t call = { .src = HUB_SRC_MUSE, .perm = muse_cfg_get_perm(), .params = inv->params, .out = &w, .err_code = NULL };
    if (fn(&call) != 0) {
        if (call.rl_refund && !rl_exempt) hub_rl_refund(&s_rl, cmd->rl, now_ms());   /* 当场拒了、没真去做：令牌还回去（MX21；本地出口 hub_api.c 同样） */
        const char *code = call.err_code ? call.err_code : "internal";
        reply_error(inv->id, code, call.err_msg[0] ? call.err_msg : "the hub could not do that");
        /* 处理函数里的权限拒（敏感动作 / 敏感设备，M5）也算"被拒"，和上面分发前的权限检查一个口径（2026-10-23 上板：原来记成 failed） */
        st_count(!strcmp(code, "forbidden") || !strcmp(code, "rate_limited") ? &s_st.invokes_denied : &s_st.invokes_failed);
        return code;
    }
    /* 结论晚到（作业，6.6）：现在不回包，作业结束时 pend_flush 发 link.result；审计里这一条记 accepted，结束时 hub_jobs 再记一条最终结论。
       表满 / id 太长存不下就当场把 {job, state} 回出去（Muse 至少知道在做，结论要靠事件或再查） */
    if (call.job && pend_add(call.job, inv->id)) return "accepted";
    mp_result_end(&w);
    const int n = hj_finish(&w);
    if (n < 0) {
        reply_error(inv->id, "internal", "the result did not fit; try a narrower request");
        st_count(&s_st.invokes_failed);
        return "internal";
    }
    if (!msess_send(s_res_buf, (size_t)n)) s_end_session = true;
    return "ok";
}

static void dispatch(const mp_invoke_t *inv)
{
    const int64_t t0 = esp_timer_get_time();
    st_count(&s_st.invokes_total);
    const char *res = dispatch_inner(inv);
    hub_audit_record(HAU_SRC_MUSE, inv->command, inv->params, res, (uint32_t)((esp_timer_get_time() - t0) / 1000));   /* 6.10 */
}

/* ---------------------------------------------------------------- 推送（4.9、6.7） */

/* hub_events 有新推送时叫醒会话任务（任何任务里调；只投、不等） */
static void push_wake(void)
{
    if (s_wake_pending || !s_q) return;               /* 已有一条在排：会话任务处理它时会把管线一次取空 */
    s_wake_pending = true;
    const ml_msg_t m = { MSG_CFG, 0 };
    if (xQueueSend(s_q, &m, 0) != pdTRUE) s_wake_pending = false;   /* 没投进去：下次再投；主循环最多 10 s 也会自己转一圈 */
}

/* 关掉 / 解绑：排着的、在途的都不要了（在途那条的回包晚到会因 id 对不上被丢掉） */
static void push_clear(void)
{
    const size_t n = mpq_count(&s_mpq);
    mpq_clear_queue(&s_mpq);                          /* 只丢排着的：计数保留（状态页计数不倒退） */
    s_px_id = 0;
    if (n) ESP_LOGI(TAG, "丢掉 %u 条没推出去的事件", (unsigned)n);
}

/* 管线 → 队列：一个事件一条推送，来了就排（不合并、不去重、不限配额 —— 网关不做人为干预，交给 Muse；2026-10-24 用户定，
   muse_push_core.h 头注）。会话外也调：过期照常算，上线后直接发队列里的 */
static void push_collect(uint64_t now)
{
    s_wake_pending = false;                           /* 这一趟会把管线取空 */
    /* 关着 / 没配对时事件只进环（hub_events 已记），不进推送队列：重新打开或换账号配对后不该把那段时间的事推出去（N5） */
    const bool accept = s_mlc.st != MUSE_ST_DISABLED && s_mlc.st != MUSE_ST_UNPAIRED;
    hev_t ev;
    while (hub_events_take(&ev)) {
        if (!accept) continue;
        /* 再按推送类别复核一次：事件由 hub_svc 产生，开机早期可能在本任务读进用户设置（setup_vm_api）之前就按默认类别进了队 ——
           到这里时设置已经读进来了（主循环在 setup_vm_api 之后），用户关掉的类别别推出去（第八步审计）。报警恒推 */
        if (ev.cls != HUB_EVC_ALARM && !(hub_events_get_notify() & (1u << ev.cls))) continue;
        muse_push_t m;
        memset(&m, 0, sizeof(m));
        m.cls = ev.cls;
        m.main_chat = MUSE_PUSH_MAIN_CHAT;
        /* 带设备 id 方便 agent 接着操作（6.7 模板）；截断是确定的，句子在前、id 在尾 */
        const int a = ev.dev_id[0] ? snprintf(m.text, sizeof(m.text), "%s [%s]", ev.text, ev.dev_id) : snprintf(m.text, sizeof(m.text), "%s", ev.text);
        if (a < 0) continue;
        const mpq_result_t r = mpq_enqueue(&s_mpq, &m, now);   /* 满了挤掉最低优先级里最旧的（计进 pushes_dropped） */
        if (r != MPQ_QUEUED && r != MPQ_QUEUED_EVICTED) ESP_LOGW(TAG, "一条 %s 推送没排进队列（%d）", hev_class_name(m.cls), (int)r);
    }
    if (s_test_push && !accept) s_test_push = false;   /* 点完马上关掉：不留到重新打开再发 */
    if (s_test_push) {
        s_test_push = false;
        static const char k_test[] = "Test message from your bitEngine Hub, sent from its web page.";
        /* 也记进事件环：Muse 收到推送后常会调 hub.events.recent 核对（G0-2 观察 4），核对不到会让它困惑 */
        (void)hub_event_emit(HUB_EVC_GATEWAY, NULL, "test_push", k_test, 0);
        muse_push_t t;
        memset(&t, 0, sizeof(t));
        t.cls = HUB_EVC_GATEWAY;
        t.main_chat = s_test_push_main;
        snprintf(t.text, sizeof(t.text), "%s", k_test);
        const mpq_result_t r = mpq_enqueue(&s_mpq, &t, now);   /* 用户在网页上点的，要马上看到 */
        ESP_LOGI(TAG, "测试推送：%s", r == MPQ_QUEUED || r == MPQ_QUEUED_EVICTED ? "已排队" : "没排进去");
    }
}

/* 侧边会话 id：存过的；确实没有就生成一个存下；一时读不到返回 false（这次不推侧边会话，别生成新的 —— 会另开一个侧边会话） */
static bool chat_sid_ready(void)
{
    if (s_chat_sid[0]) return true;
    const esp_err_t e = muse_cfg_get_chat_sid(s_chat_sid, sizeof(s_chat_sid));
    if (e == ESP_OK) return true;
    s_chat_sid[0] = '\0';
    if (e != ESP_ERR_NOT_FOUND) return false;
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    mp_format_uuid(rnd, s_chat_sid);
    if (muse_cfg_set_chat_sid(s_chat_sid) != ESP_OK) {
        s_chat_sid[0] = '\0';
        return false;
    }
    ESP_LOGI(TAG, "生成侧边会话 id");
    return true;
}

static void push_finish(int status, uint64_t now)
{
    switch (mpq_classify_status(status)) {
    case MPQ_REPLY_SENT:
        mpq_mark_sent(&s_mpq, s_px_idx, now);
        ESP_LOGI(TAG, "推送已送达（%d）", status);
        break;
    case MPQ_REPLY_REJECTED:
        mpq_drop(&s_mpq, s_px_idx);                   /* 服务端明确不收：丢掉，不重试 */
        ESP_LOGW(TAG, "推送被拒（%d），丢掉", status);
        break;
    default:
        mpq_abort(&s_mpq, s_px_idx);                  /* 留着，过一会儿再发，过期为止 */
        s_push_not_before_ms = now + PUSH_RETRY_MS;
        ESP_LOGW(TAG, "推送没成（%d），%d s 后重试", status, PUSH_RETRY_MS / 1000);
        break;
    }
    s_px_id = 0;
    st_sample();
}

/* 会话结束：在途那条没有回包了，留在队列里等下次 */
static void push_session_ended(void)
{
    if (!s_px_id) return;
    /* 应答头已经到了就按它判（2xx 算送达、免得重连后重发出两条；4xx 丢；5xx 重试）；没到按断线算：留在队列，30 s 后再发 */
    push_finish(s_px_status > 0 ? s_px_status : -1, now_ms());
}

/* 在线时（会话轮询里）：在途的看超时；没有在途就发下一条 */
static void push_pump(uint64_t now)
{
    if (s_px_id) {
        if (now >= s_px_deadline_ms) {
            (void)msess_cancel(s_px_id);
            push_finish(s_px_status > 0 ? s_px_status : 0, now);   /* 头已到按头判（2xx 算送达）；没到 = 超时、可重试 */
        }
        return;
    }
    if (now < s_push_not_before_ms) return;
    const int idx = mpq_next(&s_mpq, now);
    if (idx < 0) return;
    const muse_push_t *m = mpq_get(&s_mpq, idx);
    if (!m->main_chat && !chat_sid_ready()) {
        s_push_not_before_ms = now + PUSH_RETRY_MS;
        return;
    }
    const int n = mpq_build_body(s_push_body, PUSH_BODY_MAX, m, s_node_id, s_chat_sid);
    if (n < 0) {
        ESP_LOGE(TAG, "推送请求体放不下，丢掉这条");
        mpq_drop(&s_mpq, idx);
        return;
    }
    const int64_t id = msess_post(PUSH_PATH, s_push_body, (size_t)n);
    if (!id) {
        s_push_not_before_ms = now + PUSH_RETRY_MS;   /* 写失败的话 msess 已标记，会话随即结束 */
        return;
    }
    mpq_begin(&s_mpq, idx);
    s_px_id = id;
    s_px_idx = idx;
    s_px_status = 0;
    s_px_len = 0;
    s_px_deadline_ms = now + PUSH_TIMEOUT_MS;
}

/* ---------------------------------------------------------------- 钩子（muse_link_priv.h） */

bool ml_session_established(void)
{
    s_end_session = false;
    pend_clear();                                     /* 上一个会话里没等到结论的调用：新会话里那些 id 没人认 */
    step(MLE_WS_UP);                                  /* → REGISTERING，动作 REGISTER → send_register() */
    return !s_end_session;
}

bool ml_session_message(const char *json, size_t len)
{
    char id[64];
    if (len > MP_INVOKE_MAX || !mp_json_shape_ok(json, len, MP_JSON_MAX_DEPTH, MP_JSON_MAX_VALUES)) {
        /* 不进 cJSON（4.8；嵌套 / 节点数的闸门见 mp_json_shape_ok）：是调用就扫出 id 回 invalid_params */
        char method[24];
        if (mp_scan_top_string(json, len, "method", method, sizeof(method)) && strcmp(method, "link.invoke") == 0
            && mp_scan_top_string(json, len, "id", id, sizeof(id))) {
            reply_error(id, "invalid_params", len > MP_INVOKE_MAX ? "request too large for this hub (limit 4096 bytes)"
                                                                  : "request too deeply nested or too long for this hub");
            hub_audit_record(HAU_SRC_MUSE, "invalid_request", NULL, len > MP_INVOKE_MAX ? "too_large" : "too_nested", 0);   /* 6.10：回了错的都记 */
        } else {
            ESP_LOGW(TAG, "丢弃一条 %u 字节、不能解析的控制消息", (unsigned)len);
        }
        return !s_end_session;
    }
    cJSON *msg = cJSON_ParseWithLength(json, len);
    if (!msg) {
        ESP_LOGW(TAG, "控制消息不是 JSON（%u 字节）", (unsigned)len);
        return !s_end_session;
    }
    if (s_mlc.st == MUSE_ST_REGISTERING) {
        bool hb = false;
        char why[96];
        const mp_reg_reply_t r = mp_classify_register_reply(msg, s_reg_id, &hb, why, sizeof(why));
        if (r != MP_REG_NOT_MINE) {
            cJSON_Delete(msg);
            if (r == MP_REG_ACKED) {
                s_hb_ok = hb;
                s_hb_sent = false;
                s_tok_ms = s_tok_ms ? s_tok_ms : now_ms();
                step(MLE_REG_OK);
                ESP_LOGI(TAG, "注册成功（心跳 %s）", hb ? "开" : "关");
            } else {
                char clean[64];                       /* 服务端给的原因不可信：净化后才进状态 / 日志 */
                hub_sanitize_device_str(why, strlen(why), clean, sizeof(clean), sizeof(clean) - 1);
                ESP_LOGW(TAG, "注册被拒：%s", clean);
                step(r == MP_REG_REJECTED_PROTOCOL ? MLE_REG_PROTO : MLE_REG_ERR);
                st_update(clean);
            }
            return !s_end_session;
        }
    }
    mp_invoke_t inv;
    switch (mp_classify_inbound(msg, &inv)) {
    case MP_IN_UNPAIRED:
        ESP_LOGW(TAG, "服务端说这台设备已在 Muse App 里删除");
        hub_audit_note(HAU_SRC_MUSE, "muse.unpaired_in_app", "", "ok");
        step(MLE_UNPAIRED_EVT);
        break;
    case MP_IN_INVOKE_BAD:
        reply_error(inv.id, "invalid_params", "command must be a string and params an object");
        hub_audit_record(HAU_SRC_MUSE, "invalid_request", NULL, "invalid_params", 0);
        break;
    case MP_IN_INVOKE: {
        const mlc_out_t o = step(MLE_INVOKE);
        if (o.act & MLA_DISPATCH) dispatch(&inv);
        else if (o.act & MLA_REPLY_BUSY) {
            reply_error(inv.id, "busy", "the hub is still connecting; try again in a moment");
            hub_audit_record(HAU_SRC_MUSE, inv.command, inv.params, "busy", 0);
        }
        break;
    }
    case MP_IN_IGNORE:
    default:
        break;
    }
    cJSON_Delete(msg);
    return !s_end_session;
}

void ml_session_skipped(size_t len)
{
    ESP_LOGW(TAG, "一条 %u 字节的控制消息超过重组缓冲，整条跳过", (unsigned)len);
}

void ml_session_response(int64_t id, int status, const uint8_t *data, size_t len, bool end)
{
    if (!s_px_id || id != s_px_id) return;            /* 取消过的 / 过期的流 */
    if (status > 0 || (status < 0 && s_px_status <= 0)) s_px_status = status;   /* 应答头到了之后的复位不覆盖它（2xx 后复位仍算送达） */
    if (data && len && s_px_len < PUSH_REPLY_MAX) {
        const size_t n = len < PUSH_REPLY_MAX - s_px_len ? len : PUSH_REPLY_MAX - s_px_len;
        memcpy(s_px_buf + s_px_len, data, n);
        s_px_len += n;
    }
    if (!end && status >= 0) return;
    s_px_buf[s_px_len] = '\0';
    if (s_px_status >= 200 && s_px_status < 300 && !strstr(s_px_buf, "message_id")) {
        ESP_LOGW(TAG, "推送回包没有 message_id（照样算送达，免得重发出两条）");
    }
    push_finish(s_px_status == 0 ? -1 : s_px_status, now_ms());   /* 结束了却没有应答头：按复位算；头之后被复位按头判 */
}

/* 配置变了（SDK token / 显示名 / 服务地址）：重新喂给 vm_api。看标志而不是看队列里的 MSG_CFG —— 那条只是叫醒 */
static void take_cfg(void)
{
    if (s_rereg_at_ms && now_ms() >= s_rereg_at_ms) {
        s_rereg_at_ms = 0;
        s_rereg_dirty = true;
    }
    if (s_rereg_dirty) {
        s_rereg_dirty = false;
        step(MLE_REREG);
    }
    if (!s_cfg_dirty || (s_cfg_retry_ms && now_ms() - s_cfg_retry_ms < CFG_RETRY_MS)) return;
    s_cfg_dirty = false;
    s_cfg_retry_ms = 0;
    (void)setup_vm_api();                             /* 读不到会重新置 dirty、记重试时刻 */
}

/* 在线时的内存复查：WATCHDOG_EVENT_HEAP_CRITICAL 只在看门狗判 FATAL（马上整机重启）时才发，指望不上（第二轮审计）。
   门槛取建连闸门的一半：建连后会话本身就要吃掉一些，用同一个门槛会连上就断、断了又连 */
static bool mem_critical(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < CONFIG_BITENGINE_MUSE_MIN_INTERNAL_FREE / 2
           || heap_caps_get_free_size(MALLOC_CAP_DMA) < CONFIG_BITENGINE_MUSE_MIN_DMA_FREE / 2;
}

/* 兜底（每 RESYNC_MS 一次）：外部事件经队列来，回调里只许投、不许等（S2），队列满就丢。丢了 NET_UP 会永远停在
   WAIT_NET，丢了 ENABLE / DISABLE 会和网页上存着的开关对不上 —— 这里按现值补。开关以 hubdb 里存着的为准：
   只有**确定**读到才动（读不到什么都不做）；网络只补 NET_UP（NET_DOWN 丢了，会话自己会断，补它反而会在切换的瞬间误杀在线会话）。
   将来 4.11 的"连续 3 次恢复失败进 DISABLED"若做成不改存储，要在这里排除，否则会被拉回来。 */
static void resync(void)
{
    const uint64_t t = now_ms();
    if (t - s_resync_ms < RESYNC_MS) return;
    s_resync_ms = t;
    bool en = false, paired = false;
    if (muse_cfg_load_flags(&en, &paired) == ESP_OK) {
        if (en && s_mlc.st == MUSE_ST_DISABLED) {
            ESP_LOGW(TAG, "存着的开关是开、状态是关（ENABLE 事件丢了？）：补一次");
            step(MLE_ENABLE);
        } else if (!en && s_mlc.st != MUSE_ST_DISABLED) {
            ESP_LOGW(TAG, "存着的开关是关、状态还在跑（DISABLE 事件丢了？）：补一次");
            step(MLE_DISABLE);
        }
    }
    if (!s_end_session && s_mlc.st == MUSE_ST_WAIT_NET && net_online()) {
        ESP_LOGW(TAG, "网络在线但还在等网（NET_UP 事件丢了？）：补一次");
        step(MLE_NET_UP);
    }
    /* 后端变了（zhub 比 Muse 晚起：started 只会 false → true 一次）：重注册，命令表跟着变（C17）。只看在线时：其余状态下次注册自然取新值。
       ⚠️ 以后的 CONTROL / ZB_JOIN 别绑到会来回翻的运行状态上：每翻一次就断一次 Muse（重注册 = 断开重连） */
    const uint8_t be_now = current_backends();
    if (!s_end_session && s_mlc.st == MUSE_ST_ONLINE && be_now != s_reg_backends) {
        ESP_LOGI(TAG, "后端就绪位变了（0x%02x → 0x%02x）：重新注册", (unsigned)s_reg_backends, (unsigned)be_now);
        s_rereg_dirty = true;
    }
    if (!s_end_session && (s_mlc.st == MUSE_ST_REGISTERING || s_mlc.st == MUSE_ST_ONLINE) && mem_critical()) {
        ESP_LOGW(TAG, "在线期间内存跌到建连门槛的一半以下：主动断开");
        step(MLE_MEM_LOW);
    }
    if (s_clear_pending) {
        s_clear_pending = muse_cfg_clear_pairing(FLUSH_TMO_MS) != ESP_OK;
        if (!s_clear_pending) ESP_LOGI(TAG, "配对凭据补清掉了");
        st_update(s_clear_pending ? "credentials not deleted yet, retrying" : NULL);
    }
    st_sample();                                   /* 会话里分派命令时栈最深，在线时也要采 */
    if (s_sc_seen < 2) {
        /* BLE 窗口之后的 Thread 自检（3.3）：只报这次开机亲眼看着它跑完的；开机第一眼就不是 pending 的是上次存的结果，不再报 */
        blew_status_t bs;
        ble_window_get_status(&bs);
        const char *sc = bs.selfcheck ? bs.selfcheck : "";
        if (strcmp(sc, "pending") == 0) {
            s_sc_seen = 1;
        } else if (s_sc_seen == 1 && strncmp(sc, "degraded", 8) == 0) {
            s_sc_seen = 2;
            /* 只写事实、不写指令（MD-10）；"断电重上电"的提示由前端在连续两次退化时给（3.3） */
            (void)hub_event_emit(HUB_EVC_GATEWAY, NULL, "thread_degraded",
                                 "Hub: after the last Muse pairing, Thread devices are answering the hub less reliably than before.",
                                 HEV_F_PUSH);
        } else {
            s_sc_seen = 2;
        }
    }
    if (s_pend_acc && s_pend_ref && muse_cfg_save_tokens(s_pend_acc, s_pend_ref, FLUSH_TMO_MS) == ESP_OK) {
        free_secret(&s_pend_acc);
        free_secret(&s_pend_ref);
        ESP_LOGI(TAG, "内存里的新令牌补存上了");
    }
}

static void handle_msg(const ml_msg_t *m)
{
    if (m->kind == MSG_CFG) return;                   /* 只是叫醒；take_cfg 看标志 */
    if (m->ev >= MLE_COUNT) return;
    step((mle_t)m->ev);
}

bool ml_session_poll(void)
{
    wd_kick(s_wd);
    ml_msg_t m;
    while (!s_end_session && xQueueReceive(s_q, &m, 0) == pdTRUE) handle_msg(&m);
    take_cfg();
    if (!s_end_session && s_job_wake) {
        s_job_wake = false;
        pend_flush();
    }
    if (!s_end_session) resync();
    const uint64_t t = now_ms();
    if (!s_end_session && s_timer_deadline_us && esp_timer_get_time() >= s_timer_deadline_us) {
        s_timer_deadline_us = 0;
        step(MLE_TIMER);                              /* REGISTERING 的 30 s 注册超时走这里（step 里先刷新环境） */
    }
    if (!s_end_session && s_mlc.st == MUSE_ST_ONLINE) {
        if (s_hb_ok && (!s_hb_sent || t - s_hb_last_ms >= HEARTBEAT_MS)) {
            if (msess_send(mp_heartbeat_json(), strlen(mp_heartbeat_json()))) {
                s_hb_sent = true;
                s_hb_last_ms = t;
            } else {
                s_end_session = true;
            }
        }
        if (!s_bg_refresh_due && s_tok_ms && t - s_tok_ms >= TOKEN_PERIOD_MS) step(MLE_TOKEN_DUE);
    }
    push_collect(t);
    if (!s_end_session && s_mlc.st == MUSE_ST_ONLINE) push_pump(t);   /* 只在注册成功之后推（4.9） */
    return !s_end_session;
}

/* ---------------------------------------------------------------- 任务 */

static void sess_task(void *arg)
{
    (void)arg;
    const wd_task_spec_t spec = {
        .name = "muse_sess",
        .deadline_ms = WD_DEADLINE_MS,
        /* 先只观察：4.11 的"恢复 = 重启 muse_link、连续 3 次失败进 DISABLED"还没做，按默认策略 RECOVER 会升到整机重启（违反 N7） */
        .severity = WD_SEV_OBSERVE,
        .recover = NULL,
    };
    s_wd = wd_register(&spec);
    hub_rl_init(&s_rl, now_ms());
    /* 开关与配对状态要**确定**读到才起状态机：hub_store 一时没答（开机早、忙）就当成"没开 / 没配对"，会让一台配好的
       网关在这次开机里一直离线（第四步审计 ❌ 的同一类）。读不到就等，等的时候状态页显示原因 */
    bool en = false, paired = false;
    for (uint32_t tries = 0;; tries++) {
        const esp_err_t e = muse_cfg_load_flags(&en, &paired);
        if (e == ESP_OK) break;
        if (tries % 30 == 0) {
            ESP_LOGW(TAG, "读不到 Muse 的开关 / 配对状态（%s）：等 hub_store", esp_err_to_name(e));
            st_update("storage not ready");
        }
        wd_kick(s_wd);
        vTaskDelay(pdMS_TO_TICKS(tries < 10 ? BOOT_RETRY_MS : RESYNC_MS));
    }
    s_cfg_dirty = false;
    (void)setup_vm_api();                             /* 存储刚确定能读：读不到的那样会置 dirty，取 VM 前还会再读 */
    s_tok_ms = now_ms();
    s_resync_ms = now_ms();
    mlc_out_t o = mlc_init(&s_mlc, en, paired, net_online(), mem_ok());
    apply(o, MLE_ENABLE);
    ESP_LOGI(TAG, "会话任务起来了：%s（node %s）", mlc_state_str(s_mlc.st), s_node_id);
    for (;;) {
        wd_kick(s_wd);
        ml_msg_t m;
        /* 先把排着的事件处理掉、再做阻塞的活：DISABLE / 解绑不必等一次取 VM 或建连（最长约一分钟）做完才生效；
           动作标志按现在的状态再核一次（期间状态可能已经变了） */
        s_end_session = false;                        /* 会话外它没有意义；留着会挡住 resync 补 NET_UP（核实审计） */
        while (xQueueReceive(s_q, &m, 0) == pdTRUE) handle_msg(&m);
        take_cfg();
        push_collect(now_ms());
        st_sample();
        if (s_do_fetch) { s_do_fetch = false; if (s_mlc.st == MUSE_ST_FETCH_VM) do_fetch(); continue; }
        if (s_do_refresh) { s_do_refresh = false; if (s_mlc.st == MUSE_ST_REFRESHING) do_refresh(); continue; }
        if (s_do_connect) { s_do_connect = false; if (s_mlc.st == MUSE_ST_CONNECTING) do_connect(); continue; }
        TickType_t wait = pdMS_TO_TICKS(MAX_WAIT_MS);
        if (s_timer_deadline_us) {
            const int64_t left_us = s_timer_deadline_us - esp_timer_get_time();
            if (left_us <= 0) wait = 0;
            else if (left_us / 1000 < MAX_WAIT_MS) wait = pdMS_TO_TICKS(left_us / 1000) + 1;
        }
        if (xQueueReceive(s_q, &m, wait) == pdTRUE) {
            handle_msg(&m);
        } else if (s_timer_deadline_us && esp_timer_get_time() >= s_timer_deadline_us) {
            s_timer_deadline_us = 0;
            step(MLE_TIMER);                          /* 状态机约定：喂 TIMER 前刷新网络与内存（step 里做） */
        }
        resync();                                     /* 每圈都调（它自己 10 s 一次）：只在等超时才调会被持续的叫醒消息饿死（核实审计） */
    }
}

/* ---------------------------------------------------------------- 事件订阅（回调里只投队列，S2） */

/* 只投、不等（回调与 httpd 都会调到）。满了返回 ESP_ERR_TIMEOUT —— 开关与网络有 resync 兜底，其余调用方自己决定怎么报 */
static esp_err_t post_event(mle_t ev)
{
    if (!s_q || !s_started) return ESP_ERR_INVALID_STATE;          /* 任务没起来（start 失败）：没人收，别塞满队列 */
    const ml_msg_t m = { MSG_EVENT, (uint8_t)ev };
    if (xQueueSend(s_q, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "事件队列满，丢掉 %s", mlc_event_str(ev));
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == NETWORK_EVENT) {
        if (id == NETWORK_EVENT_ONLINE) (void)post_event(MLE_NET_UP);          /* 丢了由 resync 补 */
        else if (id == NETWORK_EVENT_OFFLINE) (void)post_event(MLE_NET_DOWN);
    } else if (base == WATCHDOG_EVENT && id == WATCHDOG_EVENT_HEAP_CRITICAL) {
        (void)post_event(MLE_MEM_LOW);                /* 4.6：堆告急时主动断开；丢了下一步 step 也会按现值看内存闸门 */
    }
}

esp_err_t muse_link_init(void)
{
    if (s_inited) return ESP_OK;
    s_big = heap_caps_calloc(1, sizeof(ml_big_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_big) return ESP_ERR_NO_MEM;
    s_q = xQueueCreate(EVQ_LEN, sizeof(ml_msg_t));
    s_st_lock = xSemaphoreCreateMutex();
    s_reg_buf = heap_caps_malloc(MP_REGISTER_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_res_buf = heap_caps_malloc(MP_RESULT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_push_body = heap_caps_malloc(PUSH_BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_px_buf = heap_caps_malloc(PUSH_REPLY_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_q || !s_st_lock || !s_reg_buf || !s_res_buf || !s_push_body || !s_px_buf) {
        ESP_LOGE(TAG, "初始化分配失败");
        return ESP_ERR_NO_MEM;
    }
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) ESP_LOGW(TAG, "esp_read_mac 失败，身份按全零");   /* 不用 esp_efuse_mac_get_default */
    muse_identity_node_id(mac, s_node_id, sizeof(s_node_id));
    const esp_app_desc_t *d = esp_app_get_description();
    snprintf(s_version, sizeof(s_version), "%s", d ? d->version : "unknown");
    memset(&s_st, 0, sizeof(s_st));
    mpq_init(&s_mpq, (uint64_t)(esp_timer_get_time() / 1000));
    /* 事件管线、Thread 清单、审计环由 hub_svc 起、由它轮询（main.c 在本模块之前调 hub_svc_start；第十七节"M4 之前"）。
       这里只认领推送：要推的事件进队后叫醒会话任务 */
    hub_events_set_wake(push_wake);
    hub_jobs_set_listener(job_listener);
    snprintf(s_st.node_id, sizeof(s_st.node_id), "%s", s_node_id);
    muse_cfg_get_display_name(s_st.display_name, sizeof(s_st.display_name));
    s_st.enabled = muse_cfg_enabled();
    s_st.paired = muse_cfg_paired();
    s_st.sdk_token_set = muse_cfg_sdk_token_set();
    esp_err_t err = esp_event_handler_register(NETWORK_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    if (err == ESP_OK) err = esp_event_handler_register(WATCHDOG_EVENT, WATCHDOG_EVENT_HEAP_CRITICAL, on_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "订阅事件失败：%s", esp_err_to_name(err));
        return err;
    }
    s_inited = true;
    return ESP_OK;
}

esp_err_t muse_link_start(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (s_started) return ESP_OK;
    /* 栈必须在 PSRAM（4.6、14.2）：拿不到就不起，不退回内部内存 */
    if (xTaskCreateWithCaps(sess_task, "muse_sess", SESS_STACK, NULL, SESS_PRIO, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "会话任务建不起来（PSRAM 栈）");
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

esp_err_t muse_link_get_status(muse_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_inited || !s_started) return ESP_ERR_INVALID_STATE;     /* 任务没起来：网页显示 not_running，不显示假的 disabled */
    if (xSemaphoreTake(s_st_lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    *out = s_st;
    xSemaphoreGive(s_st_lock);
    return ESP_OK;
}

esp_err_t muse_link_set_enabled(bool en)
{
    if (!s_inited || !s_started) return ESP_ERR_INVALID_STATE;
    return post_event(en ? MLE_ENABLE : MLE_DISABLE);
}

esp_err_t muse_link_unpair_local(void)
{
    if (!s_inited || !s_started) return ESP_ERR_INVALID_STATE;
    return post_event(MLE_LOCAL_UNPAIR);
}

esp_err_t muse_link_reregister(void)
{
    if (!s_inited || !s_started) return ESP_ERR_INVALID_STATE;
    s_rereg_dirty = true;                             /* 走标志：队列满了也丢不掉（第一轮审计） */
    const ml_msg_t m = { MSG_CFG, 0 };
    (void)xQueueSend(s_q, &m, 0);                     /* 只是叫醒 */
    return ESP_OK;
}

esp_err_t muse_link_test_push(bool main_chat)
{
    if (!s_inited || !s_started) return ESP_ERR_INVALID_STATE;
    muse_state_t st = MUSE_ST_DISABLED;
    if (xSemaphoreTake(s_st_lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    st = s_st.state;
    xSemaphoreGive(s_st_lock);
    if (st != MUSE_ST_ONLINE) return ESP_ERR_NOT_ALLOWED;
    s_test_push_main = main_chat;
    s_test_push = true;                               /* 走标志：丢不掉 */
    push_wake();
    return ESP_OK;
}

esp_err_t muse_link_config_changed(void)
{
    if (!s_inited || !s_started || !s_q) return ESP_ERR_INVALID_STATE;
    s_cfg_dirty = true;
    const ml_msg_t m = { MSG_CFG, 0 };
    (void)xQueueSend(s_q, &m, 0);                     /* 只是叫醒会话任务；满了也不要紧，它每圈都看标志 */
    /* 先读（hub_store 最多各等 500 ms）、后拿锁拷：别拿着状态锁等存储，把 get_status 堵住 */
    const bool tok_set = muse_cfg_sdk_token_set();
    char name[sizeof(s_st.display_name)];
    muse_cfg_get_display_name(name, sizeof(name));
    if (s_st_lock && xSemaphoreTake(s_st_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_st.sdk_token_set = tok_set;
        memcpy(s_st.display_name, name, sizeof(name));
        xSemaphoreGive(s_st_lock);
    }
    return ESP_OK;
}
