/* zh_host_core —— zhub 的编排层（接口、规则与理由见 include/zh_host_core.h）。 */
#include "zh_host_core.h"

#include <string.h>

/* UP 之后的序列：tag 的高位带步号，序列重新开始之后迟到的结果认得出来 */
enum { TAG_NET_INIT = 1, TAG_NET_STATE = 3, TAG_INIT = 4, TAG_UP = 5, TAG_WIN = 6, TAG_APP = 7 };
#define TAG_KIND(t) ((t) & 0xFFu)
#define TAG_STEP(t) ((t) >> 8)

/* 初始化序列：握手之后、networkInit 之前（规则与每一项的出处见 zh_host_core.h 文件头和常量旁的注释）。顺序有讲究：协议参数在最前
   （NCP 分配内存的配置值必须先于别的命令），端点在最后、紧挨 networkInit（之后网络一起来就不许再加）。 */
enum { STEP_CFG = 0, STEP_POLICY, STEP_ENDPOINT };
/* opt = 被拒也接着走（只记 st.init_opt_rejects）：只给"有更好、没有也能跑"的那几项 —— 别让一项优化把整个 Zigbee 拖进复位循环（第七步 b 第二轮审计） */
static const struct { uint8_t kind; uint8_t id; uint16_t value; bool opt; } k_init[] = {
    { STEP_CFG,      ZH_EZSP_CFG_STACK_PROFILE,                   ZH_HOST_STACK_PROFILE, false },
    { STEP_CFG,      ZH_EZSP_CFG_SECURITY_LEVEL,                  ZH_HOST_SECURITY_LEVEL, false },
    { STEP_CFG,      ZH_EZSP_CFG_INDIRECT_TRANSMISSION_TIMEOUT,   ZH_HOST_INDIRECT_TX_TIMEOUT_MS, true },
    { STEP_POLICY,   ZH_EZSP_POLICY_TRUST_CENTER,                 ZH_HOST_TC_POLICY_CLOSED, false },
    { STEP_POLICY,   ZH_EZSP_POLICY_TC_REJOINS_USING_WELL_KNOWN_KEY, ZH_HOST_TC_WELL_KNOWN_REJOINS, false },
    { STEP_POLICY,   ZH_EZSP_POLICY_TC_KEY_REQUEST,               ZH_EZSP_DECISION_ALLOW_TC_KEY_REQUESTS_AND_SEND_CURRENT_KEY, false },
    { STEP_POLICY,   ZH_EZSP_POLICY_APP_KEY_REQUEST,              ZH_EZSP_DECISION_DENY_APP_KEY_REQUESTS, false },
    { STEP_POLICY,   ZH_EZSP_POLICY_BINDING_MODIFICATION,         ZH_EZSP_DECISION_DISALLOW_BINDING_MODIFICATION, false },
    { STEP_POLICY,   ZH_EZSP_POLICY_MESSAGE_CONTENTS_IN_CALLBACK, ZH_EZSP_DECISION_MESSAGE_TAG_ONLY_IN_CALLBACK, false },
    { STEP_POLICY,   ZH_EZSP_POLICY_POLL_HANDLER,                 ZH_EZSP_DECISION_POLL_HANDLER_IGNORE, false },
    { STEP_ENDPOINT, ZH_HOST_ENDPOINT,                            0, false },
};
#define K_INIT_N ((uint8_t)(sizeof(k_init) / sizeof(k_init[0])))

/* 端点 1 的簇表（设计表 8.5-3；簇 ID 是 ZCL 的）。输入 = 网关当服务端，输出 = 网关当客户端（收上报、发控制、IAS CIE）。
   OTA 0x0019 暂不登记：它得放进输入簇（网关当 OTA 服务端才能回"没有镜像"），等第七步真能回了再加 —— 先登记了，设备会来问、却等不到回答。 */
static const uint16_t k_ep_in[] = { 0x0000 /* Basic */, 0x0003 /* Identify */, 0x000A /* Time */ };
static const uint16_t k_ep_out[] = {
    0x0003 /* Identify */, 0x0006 /* OnOff */, 0x0008 /* Level */, 0x0300 /* Color */, 0x0101 /* DoorLock */,
    0x0102 /* WindowCovering */, 0x0201 /* Thermostat */, 0x0500 /* IAS Zone */, 0x0502 /* IAS WD */,
};
#define K_EP_IN_N  ((uint8_t)(sizeof(k_ep_in) / sizeof(k_ep_in[0])))
/* 入网窗口的开 / 关序列（zh_host_core.h 文件头"入网窗口"一段）。tag 的步号里 0x10 位 = 关的序列：序列换了之后迟到的结果认得出来 */
enum { WO_KEY = 0, WO_POLICY, WO_PERMIT, WO_BCAST, WO_N };
enum { WC_PERMIT0 = 0, WC_BCAST0, WC_POLICY_CLOSED, WC_CLEAR_KEYS, WC_N };
#define WIN_CLOSING_BIT 0x10u
#define K_EP_OUT_N ((uint8_t)(sizeof(k_ep_out) / sizeof(k_ep_out[0])))

/* 第 k 次连续失败之后等多久再 ASH RST（k 从 1 起；超出表尾按最后一项）。8.13："NCP 不回 RSTACK → 5 / 30 / 60 s 退避"；
   第 1 次立即重来：运行了很久之后偶发一次保活超时，没必要让 Zigbee 多停 5 s。 */
static const uint32_t k_backoff_ms[] = { 0, 5000, 30000, 60000 };

static uint32_t backoff_ms(uint32_t fails)
{
    const uint32_t n = (uint32_t)(sizeof(k_backoff_ms) / sizeof(k_backoff_ms[0]));
    if (fails == 0) return 0;
    return k_backoff_ms[fails - 1 < n ? fails - 1 : n - 1];
}

static bool expired(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

static void set_net(zh_host_t *h, zh_host_net_t n)
{
    if (h->net == n) return;
    h->net = n;
    if (n == ZH_NET_UP) h->st.net_ups++;
    else if (n == ZH_NET_DOWN) h->st.net_downs++;
    if (h->ops.on_net) h->ops.on_net(h->ops.ctx, n);
}

/* 回调里只记账：第一个原因算数，同一轮里随后到的（比如 UNKNOWN 之后的 RESET_LINK）不再叠一次 */
static void fail_later(zh_host_t *h, zh_host_fail_t why, uint32_t code)
{
    if (h->state == ZH_HOST_IDLE || h->pend_fail) return;
    h->pend_fail = true;
    h->pend_why = why;
    h->pend_code = code;
}

/* 会话收摊：在途 / 排队的命令按"结果的确切含义"了结；这期间交回的结果是我们自己造成的，不算失败 */
static void teardown_session(zh_host_t *h)
{
    h->teardown = true;
    zh_ses_link_down(&h->ses);
    h->teardown = false;
}

static void win_rec_close(zh_host_t *h);

/* 一轮链路的状态清零（复位 / 退避 / 停下都要） */
static void clear_round(zh_host_t *h)
{
    h->pend_connected = false;
    h->want = ZH_HOST_WANT_NONE;
    h->got_up = h->wait_up = false;
    h->timer_on = false;
    h->down_check = false;
    h->up_step = ZH_HOST_UP_N;
    h->up_retry = false;
    h->have_params = false;
    h->have_eui64 = false;
    h->concentrator = h->mtorr = false;
    win_rec_close(h);
    h->win_state = ZH_WIN_CLOSED;                  /* NCP 重启了：permitJoining 没了，k_init 又会把信任中心写回窗口外（第五步第二轮审计） */
    h->win_want = h->win_retry = h->win_close_pending = false;
    h->ncp_open = false;
    h->app_n = 0;                                  /* 排队的应用命令作废（不回结果：调用方自己有期限） */
    set_net(h, ZH_NET_UNKNOWN);
}

static void do_reset(zh_host_t *h, uint32_t now)
{
    (void)now;
    teardown_session(h);
    clear_round(h);
    zh_ash_reset(&h->ash);
    h->state = ZH_HOST_RESETTING;
    h->st.resets++;
}

static void handle_fail(zh_host_t *h, uint32_t now)
{
    const zh_host_fail_t why = h->pend_why;
    h->pend_fail = false;
    h->last_fail = why;
    h->last_fail_code = h->pend_code;
    if (why < ZH_HOST_FAIL_MAX) h->st.fails[why]++;
    if (h->fails < UINT32_MAX) h->fails++;
    const uint32_t d = backoff_ms(h->fails);
    if (d == 0) {
        do_reset(h, now);
        return;
    }
    teardown_session(h);
    clear_round(h);
    zh_ash_stop(&h->ash);              /* 退避期间什么都不发：不重传、不回 ACK */
    h->state = ZH_HOST_BACKOFF;
    h->timer_on = true;
    h->t_timer = now + d;
}

/* UP 之后的序列从头走（每次进入 UP 都走：NCP 的集中器设置在它复位后就没了，网络回来了信道 / PAN 也要重读） */
static void start_up_steps(zh_host_t *h)
{
    h->up_step = 0;
    h->up_retry = false;
    h->concentrator = h->mtorr = false;
    h->want = ZH_HOST_WANT_UP;
}

/* 网络掉了 / 没了：UP 之后的序列停下（还没交的不交、重试不试；在途那条的结果因为步号对不上而作废），网络回来时 net_up 从头走。
   不停的话会对着网络已经 down 的 NCP 设集中器（host_interop H10 抓到的） */
static void stop_up_steps(zh_host_t *h)
{
    h->up_step = ZH_HOST_UP_N;
    h->up_retry = false;
    h->concentrator = h->mtorr = false;            /* 快照里的"这次 UP 之后设好了"：网络不在了就不算（第一轮审计 ⚠️：原先掉网后仍报 true） */
    if (h->want == ZH_HOST_WANT_UP) h->want = ZH_HOST_WANT_NONE;
}

/* ---------------------------------------------------------------- 入网窗口 */

/* 总时长上限的记录：开始开时记一条（按请求的时长），一关就改成真开了的时长（向上取整到秒） */
static void win_rec_open(zh_host_t *h, uint8_t seconds)
{
    zh_win_rec_t *r = &h->win_hist[h->win_hist_head];
    r->t_start = h->t_now;
    r->secs = seconds;
    r->valid = true;
    h->win_rec_cur = h->win_hist_head;
    h->win_hist_head = (uint8_t)((h->win_hist_head + 1u) % ZH_HOST_WIN_HIST);
}

static void win_rec_close(zh_host_t *h)
{
    if (h->win_rec_cur >= ZH_HOST_WIN_HIST) return;
    zh_win_rec_t *r = &h->win_hist[h->win_rec_cur];
    const uint32_t used = (h->t_now - r->t_start + 999u) / 1000u;
    if (used < r->secs) r->secs = (uint16_t)used;
    h->win_rec_cur = 0xFF;
}

static uint32_t win_quota_used_s(const zh_host_t *h, uint32_t now)
{
    uint32_t used = 0;
    for (unsigned i = 0; i < ZH_HOST_WIN_HIST; i++) {
        const zh_win_rec_t *r = &h->win_hist[i];
        if (r->valid && now - r->t_start < ZH_HOST_PERMIT_HOUR_MS) used += r->secs;
    }
    return used;
}

/* 要写的那一格还是一小时内的记录：环满了（一小时里开了 ZH_HOST_WIN_HIST 次） */
static bool win_hist_full(const zh_host_t *h, uint32_t now)
{
    const zh_win_rec_t *r = &h->win_hist[h->win_hist_head];
    return r->valid && now - r->t_start < ZH_HOST_PERMIT_HOUR_MS;
}

void zh_host_expire_records(zh_host_t *h, uint32_t now_ms)
{
    if (!h) return;
    for (unsigned i = 0; i < ZH_HOST_WIN_HIST; i++) {
        zh_win_rec_t *r = &h->win_hist[i];
        if (r->valid && i != h->win_rec_cur && now_ms - r->t_start >= ZH_HOST_PERMIT_HOUR_MS) r->valid = false;
    }
}

static uint32_t win_quota_left_s(const zh_host_t *h, uint32_t now)
{
    const uint32_t used = win_quota_used_s(h, now);
    return used >= ZH_HOST_PERMIT_HOUR_CAP_S ? 0 : ZH_HOST_PERMIT_HOUR_CAP_S - used;
}

uint32_t zh_host_quota_left_s(const zh_host_t *h, uint32_t now_ms) { return h ? win_quota_left_s(h, now_ms) : 0; }

uint32_t zh_host_quota_wait_s(const zh_host_t *h, uint32_t now_ms)
{
    if (!h) return 0;
    if (win_hist_full(h, now_ms)) {                /* 环满：等要写的那一格（最旧的一条）满一小时；那时额度也至少回来了那一条 */
        const zh_win_rec_t *r = &h->win_hist[h->win_hist_head];
        const uint32_t w = (ZH_HOST_PERMIT_HOUR_MS - (now_ms - r->t_start) + 999u) / 1000u;
        if (win_quota_left_s(h, now_ms) >= ZH_HOST_PERMIT_TRIM_MIN_S) return w ? w : 1u;
    }
    if (win_quota_left_s(h, now_ms) >= ZH_HOST_PERMIT_TRIM_MIN_S) return 0;
    /* 从最早的记录起一条条过期（环里按时间先后），过到剩的够 ZH_HOST_PERMIT_TRIM_MIN_S 为止 */
    uint32_t left = win_quota_left_s(h, now_ms);
    for (unsigned k = 0; k < ZH_HOST_WIN_HIST; k++) {
        const zh_win_rec_t *r = &h->win_hist[(h->win_hist_head + k) % ZH_HOST_WIN_HIST];
        if (!r->valid || now_ms - r->t_start >= ZH_HOST_PERMIT_HOUR_MS) continue;
        left += r->secs;
        if (left >= ZH_HOST_PERMIT_TRIM_MIN_S) {
            const uint32_t w = (ZH_HOST_PERMIT_HOUR_MS - (now_ms - r->t_start) + 999u) / 1000u;
            return w ? w : 1u;
        }
    }
    return ZH_HOST_PERMIT_HOUR_MS / 1000u;         /* 不会走到：记录全过期时剩的就是满额 */
}

static void win_start_open(zh_host_t *h, uint8_t seconds)
{
    win_rec_open(h, seconds);
    h->win_state = ZH_WIN_OPENING;
    h->win_step = 0;
    h->win_s = seconds;
    h->win_want = true;
    h->win_retry = h->win_close_pending = false;
}

static void win_start_close(zh_host_t *h)
{
    win_rec_close(h);                              /* 不管为什么关（主动 / 到期 / 掉网 / 开失败）：记录改成真开了的时长 */
    h->win_state = ZH_WIN_CLOSING;
    h->win_step = 0;
    h->win_want = true;
    h->win_retry = h->win_close_pending = false;
    h->win_policy_tries = 0;
}

/* 网络掉了 / 没了：开着或正在开的窗口走关窗（permitJoining(0) / 广播多半失败、跳过；写回信任中心策略不靠网络，照样必须成功）。
   正在开的那一步在途：它的结果因为序列换了而作废 */
static void win_net_gone(zh_host_t *h)
{
    if (h->win_state == ZH_WIN_OPENING || h->win_state == ZH_WIN_OPEN) win_start_close(h);
}

static void enter_running(zh_host_t *h, uint32_t now)
{
    h->state = ZH_HOST_RUNNING;
    h->t_running = now;
    h->wait_up = false;
    h->timer_on = false;
    if (h->net == ZH_NET_UP) start_up_steps(h);
}

/* 网络 down：给 NCP ZH_HOST_NET_DOWN_MS 自己恢复（8.13）；已经在计时就不重新起算（反复报 DOWN 不许把期限一直往后推） */
static void arm_net_down(zh_host_t *h, uint32_t now)
{
    if (h->timer_on) return;
    h->timer_on = true;
    h->t_timer = now + ZH_HOST_NET_DOWN_MS;
}

/* 网络起来了（NETWORK_UP / networkState 回 JOINED） */
static void net_up(zh_host_t *h)
{
    h->got_up = true;
    h->down_check = false;
    set_net(h, ZH_NET_UP);
    if (h->state == ZH_HOST_NET_INIT) {
        enter_running(h, h->t_now);                /* 顺带走 UP 之后的序列 */
    } else if (h->state == ZH_HOST_RUNNING) {
        if (h->timer_on) h->t_running = h->t_now;  /* 从 down 里回来：稳定期从现在重新起算 */
        h->timer_on = false;                       /* 不用再等它恢复了 */
        start_up_steps(h);                         /* 网络回来了：信道 / PAN 重读、集中器重设 */
    }
}

/* 网络没起来 / 掉了：进 RUNNING（会话照常保活），开始 down 计时 */
static void net_down(zh_host_t *h)
{
    h->got_up = false;
    stop_up_steps(h);
    win_net_gone(h);
    set_net(h, ZH_NET_DOWN);
    if (h->state == ZH_HOST_NET_INIT) enter_running(h, h->t_now);
    if (h->state == ZH_HOST_RUNNING) arm_net_down(h, h->t_now);
}

/* ---------------------------------------------------------------- ASH 的回调（在 zh_ash_rx / zh_ash_tx 的调用栈里：只记账） */

static void on_ash_frame(void *ctx, const uint8_t *d, size_t n)
{
    zh_host_t *h = ctx;
    zh_ses_rx(&h->ses, d, n, h->t_now);
}

static void on_ash_event(void *ctx, zh_ash_event_t ev, uint8_t code)
{
    zh_host_t *h = ctx;
    switch (ev) {
    case ZH_ASH_EV_CONNECTED:    h->pend_connected = true; break;
    case ZH_ASH_EV_RESET_FAILED: fail_later(h, ZH_HOST_FAIL_NO_RSTACK, code); break;
    case ZH_ASH_EV_VERSION:      fail_later(h, ZH_HOST_FAIL_ASH_VERSION, code); break;
    case ZH_ASH_EV_NCP_RESET:    fail_later(h, ZH_HOST_FAIL_NCP_RESET, code); break;
    case ZH_ASH_EV_NCP_ERROR:    fail_later(h, ZH_HOST_FAIL_NCP_ERROR, code); break;
    case ZH_ASH_EV_TIMEOUTS:     fail_later(h, ZH_HOST_FAIL_ASH_TIMEOUTS, code); break;
    }
}

/* ---------------------------------------------------------------- 会话的回调（在 zh_ses_* 的调用栈里：可以 submit、不许 link_up） */

static bool ses_send(void *ctx, const uint8_t *f, size_t n) { return zh_ash_send(&((zh_host_t *)ctx)->ash, f, n); }
static unsigned ses_unacked(void *ctx) { return zh_ash_queued(&((zh_host_t *)ctx)->ash); }

/* 初始化序列某一步的结果：OK 就走下一步，走完了才要 networkInit（networkInit 只从这里要 —— 每次 NCP 启动一次）。被拒 / 说不清就复位重来 */
static void init_result(zh_host_t *h, zh_ses_result_t r, const zh_ezsp_frame_t *fr)
{
    static const uint16_t k_rsp_id[] = { [STEP_CFG] = ZH_EZSP_ID_SET_CONFIGURATION_VALUE, [STEP_POLICY] = ZH_EZSP_ID_SET_POLICY,
                                         [STEP_ENDPOINT] = ZH_EZSP_ID_ADD_ENDPOINT };
    if (r == ZH_SES_R_TIMEOUT || r == ZH_SES_R_LINK_DOWN) return;   /* 会话随后报 RESET_LINK */
    if (r == ZH_SES_R_INVALID && fr && fr->invalid_reason == ZH_SES_INVALID_VERSION_NOT_SET) return;
    uint32_t st = 1;
    if (r != ZH_SES_R_OK || h->init_step >= K_INIT_N) {
        fail_later(h, ZH_HOST_FAIL_NET_INIT, (uint32_t)r);
        return;
    }
    const int d = zh_ezsp_dec_status(fr, k_rsp_id[k_init[h->init_step].kind], &st);
    if (d != ZH_EZSP_OK) {
        fail_later(h, ZH_HOST_FAIL_NET_INIT, 0x100u + (uint32_t)(-d));
        return;
    }
    if (st != ZH_SL_OK && k_init[h->init_step].opt) {
        h->st.init_opt_rejects++;                  /* 可选项被拒：记下，接着走（NCP 照它的默认值跑） */
        h->st.init_opt_last = (uint32_t)h->init_step << 8 | (st & 0xFFu);
    } else if (st != ZH_SL_OK) {
        fail_later(h, ZH_HOST_FAIL_NET_INIT, 0x20000u | ((uint32_t)h->init_step << 8) | (st & 0xFFu));
        return;
    }
    h->init_step++;
    h->want = h->init_step < K_INIT_N ? ZH_HOST_WANT_INIT : ZH_HOST_WANT_NET_INIT;
}

static void net_init_result(zh_host_t *h, zh_ses_result_t r, const zh_ezsp_frame_t *fr)
{
    /* TIMEOUT / LINK_DOWN：会话随后报 RESET_LINK（停住时排队的命令回 LINK_DOWN、在途的回 TIMEOUT），那边记失败 */
    if (r == ZH_SES_R_TIMEOUT || r == ZH_SES_R_LINK_DOWN) return;
    /* NCP 说还没握过手：会话随后报 RESET_LINK(VERSION_NOT_SET)，原因让它记（第一轮审计：原先记成 NET_INIT，诊断会被带偏） */
    if (r == ZH_SES_R_INVALID && fr && fr->invalid_reason == ZH_SES_INVALID_VERSION_NOT_SET) return;
    /* UNKNOWN（回显：networkInit 不幂等、不重发）、INVALID、TRUNCATED：结果说不清 —— 复位重来比猜强 */
    if (r != ZH_SES_R_OK) {
        fail_later(h, ZH_HOST_FAIL_NET_INIT, (uint32_t)r);
        return;
    }
    uint32_t st = 0;
    const int d = zh_ezsp_dec_status(fr, ZH_EZSP_ID_NETWORK_INIT, &st);
    if (d != ZH_EZSP_OK) {                         /* 少一个字节、CRC 仍对的应答（zh_ezsp_ses.h"结果的确切含义"） */
        fail_later(h, ZH_HOST_FAIL_NET_INIT, 0x100u + (uint32_t)(-d));
        return;
    }
    h->last_net_status = st;
    if (st == ZH_SL_OK) {
        if (h->got_up) {                           /* NETWORK_UP 先于应答到了 */
            enter_running(h, h->t_now);
        } else {
            h->wait_up = true;
            h->timer_on = true;
            h->t_timer = h->t_now + ZH_HOST_STACK_UP_WAIT_MS;
        }
    } else if (st == ZH_SL_NOT_JOINED) {
        set_net(h, ZH_NET_NONE);                   /* 没有网络：只记，不建网 */
        enter_running(h, h->t_now);
    } else {
        /* 刚复位过的 NCP 起网络都失败：在这次启动里重试没用（networkInit 每次启动只发一次），复位重来、退避限速 */
        fail_later(h, ZH_HOST_FAIL_NET_INIT, 0x10000u | (st & 0xFFFFu));
    }
}

/* networkState（幂等读）：down 期限到了的那一查（down_check）还没在网络里就记 NET_DOWN；等 UP 超时的那一查只决定 UP / 开始 down 计时 */
static void net_state_result(zh_host_t *h, zh_ses_result_t r, const zh_ezsp_frame_t *fr)
{
    if (r == ZH_SES_R_TIMEOUT || r == ZH_SES_R_LINK_DOWN) return;   /* 会话随后报 RESET_LINK */
    if (r == ZH_SES_R_INVALID && fr && fr->invalid_reason == ZH_SES_INVALID_VERSION_NOT_SET) return;   /* 同 net_init_result：原因让 RESET_LINK 记 */
    if (h->net == ZH_NET_NONE) {                   /* 查的路上来了 NOT_JOINED：没有网络就没有可恢复的，不记失败 */
        h->down_check = false;
        if (h->state == ZH_HOST_NET_INIT) enter_running(h, h->t_now);   /* "没有网络"是稳定态，别停在 NET_INIT */
        return;
    }
    uint8_t ns = 0xFF;
    const bool ok = r == ZH_SES_R_OK && zh_ezsp_dec_network_state(fr, &ns) == ZH_EZSP_OK;
    if (ok && ns == ZH_NET_STATUS_JOINED) {
        net_up(h);
        return;
    }
    if (h->down_check) {
        h->down_check = false;
        fail_later(h, ZH_HOST_FAIL_NET_DOWN, ns);
        return;
    }
    net_down(h);
}

/* UP 之后的某一步没成：过一会儿再试同一步。不复位链路 —— 少的只是一份读数或集中器设置（zh_host_core.h 文件头"尽力而为"）。
   2026-10-22 host_interop 的噪声种子扫出来过：读参数回显用完回 UNKNOWN 之后就再也不读，网络是 UP、快照里却一直没有信道 / PAN */
static void up_later(zh_host_t *h, uint32_t step)
{
    h->st.up_bad[step]++;
    h->up_retry = true;
    h->t_up = h->t_now + ZH_HOST_UP_RETRY_MS;
}

static void up_result(zh_host_t *h, uint32_t step, zh_ses_result_t r, const zh_ezsp_frame_t *fr)
{
    if (step != h->up_step || step >= ZH_HOST_UP_N) return;   /* 序列在它在途时重新开始了：迟到的结果不算数 */
    if (r == ZH_SES_R_INVALID && fr && fr->invalid_reason == ZH_SES_INVALID_VERSION_NOT_SET) return;   /* 会话随后报 RESET_LINK，整轮重来 */
    if (r == ZH_SES_R_TIMEOUT || r == ZH_SES_R_LINK_DOWN) return;   /* 链路的问题由会话 / ASH 报，复位之后整轮重来 */
    if (r != ZH_SES_R_OK) {                        /* INVALID / TRUNCATED / UNKNOWN（回显用完：setSourceRouteDiscoveryMode 再发一次只是多一次 MTORR） */
        up_later(h, step);
        return;
    }
    bool ok = false;
    uint32_t st = 1;
    if (step == ZH_HOST_UP_PARAMS) {
        uint8_t type = 0;
        zh_ezsp_net_params_t p;
        if (zh_ezsp_dec_get_network_parameters(fr, &st, &type, &p) == ZH_EZSP_OK && st == ZH_SL_OK) {
            h->params = p;
            h->node_type = type;
            h->have_params = ok = true;
        }
    } else if (step == ZH_HOST_UP_EUI64) {
        uint8_t e[8];                              /* 先解到局部：解码失败时可能写了一半，别把上一次读到的盖成半截（第一轮审计 ⚠️） */
        if (zh_ezsp_dec_get_eui64(fr, e) == ZH_EZSP_OK) {
            memcpy(h->eui64, e, sizeof(e));
            h->have_eui64 = ok = true;
        }
    } else if (step == ZH_HOST_UP_CONCENTRATOR) {
        if (zh_ezsp_dec_status(fr, ZH_EZSP_ID_SET_CONCENTRATOR, &st) == ZH_EZSP_OK && st == ZH_SL_OK) h->concentrator = ok = true;
    } else {
        uint32_t ms = 0;
        if (zh_ezsp_dec_source_route_discovery_mode(fr, &ms) == ZH_EZSP_OK) {
            h->mtorr_next_ms = ms;
            h->mtorr = ok = true;
        }
    }
    if (!ok) {
        up_later(h, step);
        return;
    }
    h->st.up_ok[step]++;
    h->up_step++;
    if (h->up_step < ZH_HOST_UP_N) h->want = ZH_HOST_WANT_UP;
}

/* 入网窗口某一步的结果。开的序列：哪步没成就走关窗把已改的复原（记失败）；关的序列：写回信任中心策略没成就过一会儿重试（这一步不许跳过），
   别的步骤没成跳过（文件头"入网窗口"一段） */
static void win_result(zh_host_t *h, uint32_t code, zh_ses_result_t r, const zh_ezsp_frame_t *fr)
{
    const bool closing = (code & WIN_CLOSING_BIT) != 0;
    const uint8_t step = (uint8_t)(code & 0x0Fu);
    const bool live = closing ? h->win_state == ZH_WIN_CLOSING : h->win_state == ZH_WIN_OPENING;
    if (!live || step != h->win_step) return;      /* 序列换过了（网络掉了转关窗、链路复位）：迟到的结果不算数 */
    if (r == ZH_SES_R_TIMEOUT || r == ZH_SES_R_LINK_DOWN) return;   /* 会话随后报 RESET_LINK，链路复位时窗口回 CLOSED */
    if (r == ZH_SES_R_INVALID && fr && fr->invalid_reason == ZH_SES_INVALID_VERSION_NOT_SET) return;
    uint32_t st = 0xFFFFu;
    bool ok = false;
    if (r == ZH_SES_R_OK) {
        const bool policy = closing ? step == WC_POLICY_CLOSED : step == WO_POLICY;
        const bool permit = closing ? step == WC_PERMIT0 : step == WO_PERMIT;
        const bool bcast = closing ? step == WC_BCAST0 : step == WO_BCAST;
        const bool key = !closing && step == WO_KEY;
        if (key) ok = zh_ezsp_dec_status(fr, ZH_EZSP_ID_SEC_MAN_IMPORT_TRANSIENT_KEY, &st) == ZH_EZSP_OK && st == ZH_SL_OK;
        else if (policy) ok = zh_ezsp_dec_status(fr, ZH_EZSP_ID_SET_POLICY, &st) == ZH_EZSP_OK && st == ZH_SL_OK;
        else if (permit) ok = zh_ezsp_dec_status(fr, ZH_EZSP_ID_PERMIT_JOINING, &st) == ZH_EZSP_OK && st == ZH_SL_OK;
        else if (bcast) ok = zh_ezsp_dec_send(fr, ZH_EZSP_ID_SEND_BROADCAST, &st, NULL) == ZH_EZSP_OK && st == ZH_SL_OK;
        else ok = zh_ezsp_dec_empty(fr, ZH_EZSP_ID_CLEAR_TRANSIENT_LINK_KEYS) == ZH_EZSP_OK;   /* 没有状态：有应答就算 */
    }
    if (!closing) {
        if (!ok) {
            h->st.win_open_fails++;
            h->win_last_err = ((uint32_t)step << 24) | (((uint32_t)r & 0xFFu) << 16) | (st & 0xFFFFu);
            win_start_close(h);                    /* 已经改了的（策略、NCP 的窗口）复原 */
            return;
        }
        if (++h->win_step < WO_N) {
            h->win_want = true;
            return;
        }
        h->st.win_opens++;                         /* NCP 那边窗口开了（哪怕马上要关：事件环里要有"开了"） */
        if (h->win_close_pending) {                /* 开的时候有人要关 */
            win_start_close(h);
            return;
        }
        h->win_state = ZH_WIN_OPEN;
        h->t_win_end = h->t_now + (uint32_t)h->win_s * 1000u;
        return;
    }
    if (!ok) {
        if (step == WC_POLICY_CLOSED) {            /* 信任中心策略必须写回：过一会儿再试同一步；老是不成就复位让 NCP 重启（k_init 写回） */
            h->st.win_close_retries++;
            if (++h->win_policy_tries >= ZH_HOST_WIN_POLICY_TRIES) {
                fail_later(h, ZH_HOST_FAIL_WIN_POLICY, st & 0xFFFFu);
                return;
            }
            h->win_retry = true;
            h->t_win_retry = h->t_now + ZH_HOST_WIN_RETRY_MS;
            return;
        }
        h->st.win_skipped++;                       /* permitJoining(0) / 广播 / 清临时链路密钥：跳过（文件头） */
    }
    if (++h->win_step < WC_N) {
        h->win_want = true;
        return;
    }
    h->win_state = ZH_WIN_CLOSED;
    h->st.win_closes++;
}

static void on_ses_result(void *ctx, uint32_t tag, zh_ses_result_t r, const zh_ezsp_frame_t *fr)
{
    zh_host_t *h = ctx;
    if (h->teardown) return;
    if (tag == TAG_NET_INIT) net_init_result(h, r, fr);
    else if (tag == TAG_NET_STATE) net_state_result(h, r, fr);
    else if (tag == TAG_INIT) init_result(h, r, fr);
    else if (TAG_KIND(tag) == TAG_UP) up_result(h, TAG_STEP(tag), r, fr);
    else if (TAG_KIND(tag) == TAG_WIN) win_result(h, TAG_STEP(tag), r, fr);
    else if (TAG_KIND(tag) == TAG_APP && h->ops.on_app_result) h->ops.on_app_result(h->ops.ctx, TAG_STEP(tag), r, fr);
}

static void stack_status(zh_host_t *h, uint32_t st)
{
    h->last_net_status = st;
    if (st == ZH_SL_NETWORK_UP || st == ZH_SL_TC_SWAP_CHANGED || st == ZH_SL_TC_SWAP_SAME) {
        if (h->state == ZH_HOST_NET_INIT && !h->wait_up) {   /* 先于 networkInit 的应答到：记下，应答到了再进 RUNNING */
            h->got_up = true;
            set_net(h, ZH_NET_UP);
        } else {
            net_up(h);
        }
    } else if (st == ZH_SL_NETWORK_DOWN) {
        if (h->state == ZH_HOST_NET_INIT && !h->wait_up) {   /* networkInit 还没应答：先记，应答 OK 后照常等 UP（等不到会查 networkState） */
            h->got_up = false;
            set_net(h, ZH_NET_DOWN);
        } else {
            h->wait_up = false;
            net_down(h);
        }
    } else if (st == ZH_SL_NOT_JOINED) {
        set_net(h, ZH_NET_NONE);                   /* 没有网络了（比如离网）：没有可恢复的，不计时、不建网 */
        h->got_up = false;
        stop_up_steps(h);
        win_net_gone(h);
        h->down_check = false;
        if (h->state == ZH_HOST_RUNNING) h->timer_on = false;   /* RUNNING 里它是 down 期限 */
        else if (h->state == ZH_HOST_NET_INIT && h->wait_up) enter_running(h, h->t_now);   /* 等 UP 等来了"没有网络"：不用再等、也不用查 */
    } else if (st == ZH_SL_NETWORK_OPENED) {
        h->ncp_open = true;                        /* NCP 那边的窗口真开了（快照里给上板核对用；状态机不靠它） */
    } else if (st == ZH_SL_NETWORK_CLOSED) {
        h->ncp_open = false;
    }
    /* 其余（入网失败的各种原因）只记在 last_net_status */
}

static void on_ses_callback(void *ctx, const zh_ezsp_frame_t *fr)
{
    zh_host_t *h = ctx;
    h->st.callbacks++;
    if (fr->id == ZH_EZSP_ID_STACK_STATUS_HANDLER) {
        uint32_t st = 0;
        if (zh_ezsp_dec_stack_status(fr, &st) == ZH_EZSP_OK) {
            h->st.stack_status++;
            stack_status(h, st);
        } else {
            h->st.bad_callbacks++;
        }
    }
    if (h->ops.on_callback) h->ops.on_callback(h->ops.ctx, fr);
}

static void on_ses_event(void *ctx, zh_ses_event_t ev, uint32_t code)
{
    zh_host_t *h = ctx;
    switch (ev) {
    case ZH_SES_EV_READY:
        h->st.readies++;
        h->have_version = true;
        h->version_code = code;
        if (h->state == ZH_HOST_HANDSHAKE) {
            h->state = ZH_HOST_NET_INIT;
            h->init_step = 0;
            h->want = ZH_HOST_WANT_INIT;           /* 先走初始化序列，走完了 init_result 再要 networkInit */
        }
        break;
    case ZH_SES_EV_VERSION_MISMATCH: fail_later(h, ZH_HOST_FAIL_EZSP_VERSION, code); break;
    case ZH_SES_EV_RESET_LINK:       fail_later(h, ZH_HOST_FAIL_SESSION, code); break;
    }
}

/* ---------------------------------------------------------------- 入口 */

void zh_host_init(zh_host_t *h, const zh_host_ops_t *ops)
{
    if (!h) return;
    memset(h, 0, sizeof(*h));
    h->win_rec_cur = 0xFF;
    if (ops) h->ops = *ops;
    zh_ash_init(&h->ash, NULL, on_ash_frame, on_ash_event, h);
    const zh_ses_ops_t so = { ses_send, ses_unacked, on_ses_result, on_ses_callback, on_ses_event, h };
    zh_ses_init(&h->ses, NULL, &so);
    h->state = ZH_HOST_IDLE;
}

void zh_host_start(zh_host_t *h, uint32_t now_ms)
{
    if (!h) return;
    h->t_now = now_ms;
    h->fails = 0;
    h->pend_fail = false;
    h->st.starts++;
    do_reset(h, now_ms);
}

void zh_host_stop(zh_host_t *h)
{
    if (!h) return;
    teardown_session(h);
    clear_round(h);
    h->pend_fail = false;
    zh_ash_stop(&h->ash);
    h->state = ZH_HOST_IDLE;
}

void zh_host_restart(zh_host_t *h, uint32_t now_ms)
{
    if (!h || h->state == ZH_HOST_IDLE) return;
    h->t_now = now_ms;
    h->pend_fail = false;
    h->st.restarts++;
    do_reset(h, now_ms);
}

void zh_host_rx(zh_host_t *h, const uint8_t *buf, size_t n, uint32_t now_ms)
{
    if (!h || !buf || !n || h->state == ZH_HOST_IDLE) return;
    h->t_now = now_ms;
    zh_ash_rx(&h->ash, buf, n, now_ms);
}

static void submit_want(zh_host_t *h, uint32_t now)
{
    uint8_t b[48];                                 /* 最长的是 addEndpoint：5 + 8 + 2 × 12 = 37 B */
    size_t n = 0;
    uint32_t tag = 0;
    uint16_t id = 0;
    if (h->want == ZH_HOST_WANT_NET_INIT) {
        n = zh_ezsp_enc_network_init(b, sizeof(b), 0, ZH_HOST_NET_INIT_BITMASK);
        tag = TAG_NET_INIT;
        id = ZH_EZSP_ID_NETWORK_INIT;
    } else if (h->want == ZH_HOST_WANT_NET_STATE) {
        n = zh_ezsp_enc_network_state(b, sizeof(b), 0);
        tag = TAG_NET_STATE;
        id = ZH_EZSP_ID_NETWORK_STATE;
    } else if (h->want == ZH_HOST_WANT_INIT && h->init_step < K_INIT_N) {
        const uint8_t kind = k_init[h->init_step].kind, iid = k_init[h->init_step].id;
        const uint16_t v = k_init[h->init_step].value;
        tag = TAG_INIT;
        if (kind == STEP_CFG) {
            n = zh_ezsp_enc_set_config(b, sizeof(b), 0, iid, v);
            id = ZH_EZSP_ID_SET_CONFIGURATION_VALUE;
        } else if (kind == STEP_POLICY) {
            n = zh_ezsp_enc_set_policy(b, sizeof(b), 0, iid, (uint8_t)v);
            id = ZH_EZSP_ID_SET_POLICY;
        } else {
            n = zh_ezsp_enc_add_endpoint(b, sizeof(b), 0, iid, ZH_HOST_EP_PROFILE, ZH_HOST_EP_DEVICE, ZH_HOST_EP_VERSION, K_EP_IN_N, K_EP_OUT_N,
                                         k_ep_in, k_ep_out);
            id = ZH_EZSP_ID_ADD_ENDPOINT;
        }
    } else if (h->want == ZH_HOST_WANT_UP && h->up_step < ZH_HOST_UP_N) {
        tag = TAG_UP | ((uint32_t)h->up_step << 8);
        if (h->up_step == ZH_HOST_UP_PARAMS) {
            n = zh_ezsp_enc_get_network_parameters(b, sizeof(b), 0);
            id = ZH_EZSP_ID_GET_NETWORK_PARAMETERS;
        } else if (h->up_step == ZH_HOST_UP_EUI64) {
            n = zh_ezsp_enc_get_eui64(b, sizeof(b), 0);
            id = ZH_EZSP_ID_GET_EUI64;
        } else if (h->up_step == ZH_HOST_UP_CONCENTRATOR) {
            n = zh_ezsp_enc_set_concentrator(b, sizeof(b), 0, true, ZH_HOST_CONC_TYPE, ZH_HOST_CONC_MIN_S, ZH_HOST_CONC_MAX_S,
                                             ZH_HOST_CONC_ROUTE_ERR, ZH_HOST_CONC_DELIVERY_FAIL, ZH_HOST_CONC_MAX_HOPS);
            id = ZH_EZSP_ID_SET_CONCENTRATOR;
        } else {
            n = zh_ezsp_enc_set_source_route_discovery_mode(b, sizeof(b), 0, ZH_HOST_MTORR_MODE);
            id = ZH_EZSP_ID_SET_SOURCE_ROUTE_DISCOVERY_MODE;
        }
    }
    if (!n) {                                      /* 编不出来（不会发生）：别卡在这儿每拍重试 */
        h->want = ZH_HOST_WANT_NONE;
        return;
    }
    if (zh_ses_submit(&h->ses, b, n, tag, zh_ses_idempotent(id), now) == ZH_SES_OK) {
        if (tag == TAG_NET_INIT) h->st.net_inits++;
        if (tag == TAG_NET_STATE) h->st.net_states++;
        h->want = ZH_HOST_WANT_NONE;
    }                                              /* 忙 / 没就绪：留着下一拍再交 */
}

/* 交入网窗口的下一步（want 槽空着、会话能收时才调） */
static void submit_win(zh_host_t *h, uint32_t now)
{
    uint8_t b[48];
    size_t n = 0;
    uint16_t id = 0;
    const bool closing = h->win_state == ZH_WIN_CLOSING;
    const uint8_t step = h->win_step;
    uint8_t secs = h->win_s;
    bool zdo = false;
    if (!closing && h->win_state != ZH_WIN_OPENING) {
        h->win_want = false;
        return;
    }
    if (closing) {
        secs = 0;
        if (step == WC_PERMIT0) { n = zh_ezsp_enc_permit_joining(b, sizeof(b), 0, 0); id = ZH_EZSP_ID_PERMIT_JOINING; }
        else if (step == WC_BCAST0) zdo = true;
        else if (step == WC_POLICY_CLOSED) {
            n = zh_ezsp_enc_set_policy(b, sizeof(b), 0, ZH_EZSP_POLICY_TRUST_CENTER, (uint8_t)ZH_HOST_TC_POLICY_CLOSED);
            id = ZH_EZSP_ID_SET_POLICY;
        } else if (step == WC_CLEAR_KEYS) { n = zh_ezsp_enc_clear_transient_link_keys(b, sizeof(b), 0); id = ZH_EZSP_ID_CLEAR_TRANSIENT_LINK_KEYS; }
    } else {
        if (step == WO_KEY) {                      /* 先导临时链路密钥（文件头"入网窗口"） */
            static const uint8_t eui[8] = ZH_HOST_TRANSIENT_EUI64, key[16] = ZH_HOST_WELL_KNOWN_LINK_KEY;
            n = zh_ezsp_enc_import_transient_key(b, sizeof(b), 0, eui, key);
            id = ZH_EZSP_ID_SEC_MAN_IMPORT_TRANSIENT_KEY;
        } else if (step == WO_POLICY) {
            n = zh_ezsp_enc_set_policy(b, sizeof(b), 0, ZH_EZSP_POLICY_TRUST_CENTER, (uint8_t)ZH_HOST_TC_POLICY_OPEN);
            id = ZH_EZSP_ID_SET_POLICY;
        } else if (step == WO_PERMIT) { n = zh_ezsp_enc_permit_joining(b, sizeof(b), 0, secs); id = ZH_EZSP_ID_PERMIT_JOINING; }
        else if (step == WO_BCAST) zdo = true;
    }
    if (zdo) {
        /* ZDO Mgmt_Permit_Joining_req 广播给全部路由器：[序号][时长][TC significance]（af-common.c:1328-1342；APS 与半径照 SDK 主机的
           sendZigDevRequestBuffer，zigbee-device-host.c:147-171） */
        const uint8_t seq = zh_host_next_zdo_seq(h);
        const uint8_t pl[3] = { seq, secs, ZH_ZDO_TC_SIGNIFICANCE };
        const zh_ezsp_aps_t aps = { .profile_id = ZH_ZDO_PROFILE, .cluster_id = ZH_ZDO_PERMIT_JOINING_REQ, .src_ep = ZH_ZDO_ENDPOINT,
                                    .dst_ep = ZH_ZDO_ENDPOINT, .options = 0, .group_id = 0, .sequence = 0 };
        n = zh_ezsp_enc_send_broadcast(b, sizeof(b), 0, ZH_NULL_NODE_ID, ZH_ZDO_BCAST_ROUTERS, 0, &aps, ZH_ZDO_RADIUS, seq, sizeof(pl), pl);
        id = ZH_EZSP_ID_SEND_BROADCAST;
    }
    if (!n) {                                      /* 编不出来（不会发生）：别卡在这儿每拍重试 */
        h->win_want = false;
        return;
    }
    const uint32_t tag = TAG_WIN | ((uint32_t)((closing ? WIN_CLOSING_BIT : 0u) | step) << 8);
    if (zh_ses_submit(&h->ses, b, n, tag, zh_ses_idempotent(id), now) == ZH_SES_OK) h->win_want = false;
}

bool zh_host_app_submit(zh_host_t *h, const uint8_t *frame, size_t n, uint32_t tag, bool idempotent)
{
    if (!h || !frame || n < ZH_EZSP_EXT_HDR || n > ZH_EZSP_MAX || tag > ZH_HOST_APP_TAG_MAX || h->state != ZH_HOST_RUNNING ||
        h->app_n >= ZH_HOST_APP_Q) {
        if (h) h->st.app_full++;
        return false;
    }
    zh_host_app_t *a = &h->app[(h->app_head + h->app_n) % ZH_HOST_APP_Q];
    memcpy(a->f, frame, n);
    a->n = (uint8_t)n;
    a->idem = idempotent && zh_ses_idempotent((uint16_t)(frame[3] | (frame[4] << 8)));   /* 调用方说幂等也要在白名单里（第一轮审计；submit_win 同） */
    a->tag = tag;
    h->app_n++;
    h->st.app_submits++;
    return true;
}

uint8_t zh_host_next_zdo_seq(zh_host_t *h)
{
    if (!h) return 0;
    h->zdo_seq = (uint8_t)((h->zdo_seq + 1u) & ZH_ZDO_APP_SEQ_MASK);
    return h->zdo_seq;
}

/* 交应用命令队列的第一条（want 槽与入网窗口都空着、会话能收时才调） */
static void submit_app(zh_host_t *h, uint32_t now)
{
    zh_host_app_t *a = &h->app[h->app_head];
    const int rc = zh_ses_submit(&h->ses, a->f, a->n, TAG_APP | (a->tag << 8), a->idem, now);
    if (rc != ZH_SES_OK && rc != ZH_SES_E_ARG) return;   /* 忙 / 没就绪：下一拍再交 */
    const uint32_t tag = a->tag;
    h->app_head = (uint8_t)((h->app_head + 1u) % ZH_HOST_APP_Q);
    h->app_n--;
    if (rc == ZH_SES_E_ARG) {                      /* 会话永远不收这一帧（不是扩展头命令帧 / 会话自己的帧 ID）：丢掉回 INVALID，别堵住后面排队的 */
        h->st.app_bad++;
        if (h->ops.on_app_result) h->ops.on_app_result(h->ops.ctx, tag, ZH_SES_R_INVALID, NULL);
    }
}

zh_pj_result_t zh_host_permit_join(zh_host_t *h, uint32_t seconds, uint32_t now_ms)
{
    if (!h) return ZH_PJ_NOT_READY;
    if (seconds > ZH_HOST_PERMIT_MAX_S) return ZH_PJ_INVALID;
    h->t_now = now_ms;
    if (seconds == 0) {
        switch (h->win_state) {
        case ZH_WIN_OPEN:    win_start_close(h); return ZH_PJ_OK;
        case ZH_WIN_OPENING: h->win_close_pending = true; return ZH_PJ_OK;
        default:             return ZH_PJ_ALREADY_CLOSED;   /* 关着 / 正在关 */
        }
    }
    if (h->state != ZH_HOST_RUNNING || h->net != ZH_NET_UP) return ZH_PJ_NOT_READY;
    switch (h->win_state) {
    case ZH_WIN_OPEN:
    case ZH_WIN_OPENING:
        if (h->win_state == ZH_WIN_OPENING && h->win_close_pending) return ZH_PJ_BUSY;   /* 开完马上要关：等它关完 */
        return ZH_PJ_ALREADY_OPEN;
    case ZH_WIN_CLOSING: return ZH_PJ_BUSY;
    default:             break;
    }
    const uint32_t left = win_quota_left_s(h, now_ms);   /* 总时长上限：不够最短的就不开，够就截到剩的；一小时里开满 ZH_HOST_WIN_HIST 次也不开 */
    if (left < ZH_HOST_PERMIT_TRIM_MIN_S || win_hist_full(h, now_ms)) return ZH_PJ_QUOTA;
    win_start_open(h, (uint8_t)(seconds < left ? seconds : left));
    return ZH_PJ_OK;
}

uint32_t zh_host_window_left_s(const zh_host_t *h, uint32_t now_ms)
{
    if (!h || h->win_state != ZH_WIN_OPEN || expired(now_ms, h->t_win_end)) return 0;
    return (h->t_win_end - now_ms + 999u) / 1000u;
}

void zh_host_tick(zh_host_t *h, uint32_t now_ms)
{
    if (!h || h->state == ZH_HOST_IDLE) return;
    h->t_now = now_ms;
    if (h->pend_fail) handle_fail(h, now_ms);
    if (h->pend_connected) {
        h->pend_connected = false;
        if (h->state == ZH_HOST_RESETTING) {
            h->state = ZH_HOST_HANDSHAKE;
            zh_ses_link_up(&h->ses, now_ms);       /* 会话自己发首条 version */
        }
    }
    switch (h->state) {
    case ZH_HOST_BACKOFF:
        if (h->timer_on && expired(now_ms, h->t_timer)) do_reset(h, now_ms);
        break;
    case ZH_HOST_NET_INIT:
        if (h->wait_up && h->timer_on && expired(now_ms, h->t_timer)) {
            /* networkInit 说 OK，NETWORK_UP 却没来：问一下 networkState（回调丢了也认得出），不再发 networkInit */
            h->wait_up = false;
            h->timer_on = false;
            h->want = ZH_HOST_WANT_NET_STATE;
        }
        break;
    case ZH_HOST_RUNNING:
        /* 稳定 = 网络没在 down（down 期间保活照样有应答，不算数 —— 复核审计：不然 down 期限与稳定期同时到，先清零再记 NET_DOWN，
           失败数永远是 1、不退避、不亮 failing，每约 70 s 悄悄复位一次 NCP） */
        if (h->fails && h->net != ZH_NET_DOWN && !h->down_check && now_ms - h->t_running >= ZH_HOST_STABLE_MS) h->fails = 0;
        if (h->timer_on && expired(now_ms, h->t_timer)) {
            h->timer_on = false;
            if (h->net != ZH_NET_UP) {             /* down 期限到了：最后查一次，还不在网络里就复位重来 */
                h->down_check = true;
                h->want = ZH_HOST_WANT_NET_STATE;
            }
        }
        if (h->win_state == ZH_WIN_OPEN && expired(now_ms, h->t_win_end)) {
            h->st.win_expired++;                   /* 到期：NCP 与路由器的窗口自己关了，信任中心策略与临时链路密钥要我们收拾 */
            win_start_close(h);
        }
        if (h->win_retry && expired(now_ms, h->t_win_retry)) {
            h->win_retry = false;
            if (h->win_state == ZH_WIN_CLOSING) h->win_want = true;
        }
        if (h->up_retry && expired(now_ms, h->t_up)) {
            h->up_retry = false;                   /* up_step 没走完 ⇒ 网络是 UP：掉网 / 没网时 stop_up_steps 把它置成走完，回来时 net_up 从头走 */
            if (h->up_step < ZH_HOST_UP_N && h->want == ZH_HOST_WANT_NONE) h->want = ZH_HOST_WANT_UP;
        }
        break;
    default:
        break;
    }
    if (zh_ses_can_submit(&h->ses)) {
        if (h->want != ZH_HOST_WANT_NONE) submit_want(h, now_ms);
        else if (h->win_want) submit_win(h, now_ms);   /* win_want 只在 RUNNING 里置；离开 RUNNING 只经复位 / 挂起，clear_round 清它 */
        else if (h->app_n) submit_app(h, now_ms);      /* 优先级最低：设备层的查询不跟初始化 / 窗口抢；app_n 同样只在 RUNNING 里涨 */
    }
    zh_ses_tick(&h->ses, now_ms);
    if (h->pend_fail) handle_fail(h, now_ms);      /* 会话的 tick 刚判了链路（保活 / 应答超时）：这一拍就开始复位 */
}

size_t zh_host_tx(zh_host_t *h, uint32_t now_ms, uint8_t *out, size_t cap)
{
    if (!h || !out || h->state == ZH_HOST_IDLE) return 0;
    h->t_now = now_ms;
    return zh_ash_tx(&h->ash, now_ms, out, cap);
}

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

uint32_t zh_host_next_timeout_ms(const zh_host_t *h, uint32_t now_ms)
{
    if (!h || h->state == ZH_HOST_IDLE) return UINT32_MAX;
    if (h->pend_fail || h->pend_connected) return 0;
    if ((h->want != ZH_HOST_WANT_NONE || h->win_want || h->app_n) && zh_ses_can_submit(&h->ses)) return 0;
    uint32_t t = min_u32(zh_ash_next_timeout_ms(&h->ash, now_ms), zh_ses_next_timeout_ms(&h->ses, now_ms));
    if (h->timer_on) t = min_u32(t, expired(now_ms, h->t_timer) ? 0 : h->t_timer - now_ms);
    if (h->up_retry && h->state == ZH_HOST_RUNNING) t = min_u32(t, expired(now_ms, h->t_up) ? 0 : h->t_up - now_ms);
    if (h->state == ZH_HOST_RUNNING && h->win_state == ZH_WIN_OPEN) t = min_u32(t, expired(now_ms, h->t_win_end) ? 0 : h->t_win_end - now_ms);
    if (h->state == ZH_HOST_RUNNING && h->win_retry) t = min_u32(t, expired(now_ms, h->t_win_retry) ? 0 : h->t_win_retry - now_ms);
    if (h->state == ZH_HOST_RUNNING && h->fails) {
        const uint32_t ran = now_ms - h->t_running;
        t = min_u32(t, ran >= ZH_HOST_STABLE_MS ? 0 : ZH_HOST_STABLE_MS - ran);
    }
    return t;
}

bool zh_host_alive_evidence(const zh_host_t *h, uint32_t *t_ms)
{
    return h && zh_ses_alive_evidence(&h->ses, t_ms);
}

bool zh_host_evidence_since(const zh_host_t *h, uint32_t mark, uint32_t now_ms, uint32_t *age_ms)
{
    uint32_t t = 0;
    if (!h || h->ses.st.responses == mark || !zh_ses_alive_evidence(&h->ses, &t)) return false;
    if (age_ms) *age_ms = now_ms - t;
    return true;
}

const char *zh_host_state_name(zh_host_state_t s)
{
    switch (s) {
    case ZH_HOST_IDLE:      return "idle";
    case ZH_HOST_RESETTING: return "resetting";
    case ZH_HOST_HANDSHAKE: return "handshake";
    case ZH_HOST_NET_INIT:  return "net_init";
    case ZH_HOST_RUNNING:   return "running";
    case ZH_HOST_BACKOFF:   return "backoff";
    }
    return "?";
}

const char *zh_host_net_name(zh_host_net_t n)
{
    switch (n) {
    case ZH_NET_UNKNOWN: return "unknown";
    case ZH_NET_UP:      return "up";
    case ZH_NET_DOWN:    return "down";
    case ZH_NET_NONE:    return "none";
    }
    return "?";
}

const char *zh_host_win_name(zh_win_state_t w)
{
    switch (w) {
    case ZH_WIN_CLOSED:  return "closed";
    case ZH_WIN_OPENING: return "opening";
    case ZH_WIN_OPEN:    return "open";
    case ZH_WIN_CLOSING: return "closing";
    }
    return "?";
}

const char *zh_host_fail_name(zh_host_fail_t f)
{
    switch (f) {
    case ZH_HOST_FAIL_NONE:         return "none";
    case ZH_HOST_FAIL_NO_RSTACK:    return "no_rstack";
    case ZH_HOST_FAIL_ASH_VERSION:  return "ash_version";
    case ZH_HOST_FAIL_NCP_RESET:    return "ncp_reset";
    case ZH_HOST_FAIL_NCP_ERROR:    return "ncp_error";
    case ZH_HOST_FAIL_ASH_TIMEOUTS: return "ash_timeouts";
    case ZH_HOST_FAIL_SESSION:      return "session";
    case ZH_HOST_FAIL_EZSP_VERSION: return "ezsp_version";
    case ZH_HOST_FAIL_NET_INIT:     return "net_init";
    case ZH_HOST_FAIL_NET_DOWN:     return "net_down";
    case ZH_HOST_FAIL_WIN_POLICY:   return "win_policy";
    case ZH_HOST_FAIL_MAX:          break;
    }
    return "?";
}
