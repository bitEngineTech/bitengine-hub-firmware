/* zh_host_core 的真值表（docs/15 设计 8.2、8.5 开头一段、8.13）。编的是产品源文件 ../zh_host_core.c（连同 ASH、EZSP 编解码、会话）。
 * 这里的"NCP"是测试里手搓的：ASH 层每收一帧 DATA 就确认（像真 NCP 的 ASH），EZSP 应答由各用例按需回。与真 NCP 状态机（SDK ash-ncp.c）的
 * 端到端在 sdk_interop/host_interop.c。
 *   T1  初始 / NULL / IDLE 不收不发            T2  上电到 RUNNING（UP 在应答之后）：状态序列、networkInit 位图、读回信道 / PAN
 *   T3  NETWORK_UP 先于应答到                   T4  NOT_JOINED：记"没有网络"，绝不建网（主机只发过 version / networkInit / nop / callback）
 *   T5  networkInit 回错误码：这次启动里不重试，记失败、复位   T6  等 UP 超时 → 查 networkState（JOINED 就 UP；不在就 down 计时、到期记 NET_DOWN）
 *   T7  运行中 NETWORK_DOWN：60 s 内回来就取消；没回来查一次再复位；全程不在同一次启动里补发 networkInit
 *   T8  协议号不对 → 复位                        T9  networkInit 回显（非幂等，结果未知）→ 复位
 *   T10 networkInit 被 invalidCommand → 复位     T11 NCP 不回：0 / 5 / 30 / 60 / 60 s 退避的确切时刻，退避期间零字节，第 4 次起 failing
 *   T12 连续 RUNNING 满 60 s 连续失败清零         T13 运行中 NCP 自己重启（RSTACK）/ ERROR 帧 → 复位
 *   T14 挂起：IDLE、不收不发、在途命令了结不算失败；恢复从头来、连续失败清零
 *   T15 立即复位（restart）：跳过退避、不计失败；IDLE 时无效
 *   T16 保活无应答 → RESET_LINK(KEEPALIVE) → 复位，时刻 = 空闲 10 s + 应答超时 15 s
 *   T17 回调先记账再交上层；网络状态变化交 on_net
 *   T18 读参数被拒 / 没读到：不复位，5 s 后再读，读到为止
 *   T19 自己收摊交回的结果不算第二次失败   T20 会话失败进退避：NCP 来帧也不回 ACK
 *   T21 应答之前 UP 又 DOWN：应答 OK 后照常等 UP、查 networkState   T22 TC 换机两种状态算 UP   T23 同一拍两个失败只记第一个
 *   T24 证据比计数：跨 2^31 / 2^32 回绕都对；每毫秒的不变量：退避 / 挂起时 ASH 与会话都是 DOWN
 *   T25 网络一直起不来：失败攒起来、5/30/60 s 退避、failing   T26 VERSION_NOT_SET 归会话   T27 NOT_JOINED 不误复位、不卡 NET_INIT
 *   T28 从 down 回来，稳定期重新起算   T29 初始化序列（协议参数 / 策略 / 端点）逐字节对、任何一步被拒都不发 networkInit
 *   T30 UP 之后：读参数 → EUI64 → 集中器 → MTORR，逐字节对   T31 集中器被拒：不复位、5 s 后重试、MTORR 等它
 *   T32 同一次启动里网络掉了又回来：UP 之后的序列重走，初始化序列不重发   T33 序列重新开始后迟到的结果不算数
 *   T34 序列没走完网络就掉了：停下（不对 down 的网络设集中器、重试也不试），回来再从头走；快照里的集中器跟着清
 *   T35 UP 之后的序列遇到截断 / 回显（UNKNOWN）/ 短一字节：只记账、5 s 后重试同一步、不复位；解不开的 EUI64 不盖掉上一次读到的
 *   T36 入网窗口开：策略 → permitJoining → ZDO 广播，逐字节；开着不重开   T37 到期关：permitJoining(0) → 广播 0 → 策略写回 → 清临时链路密钥
 *   T38 主动关、开的路上要关、关的路上要开   T39 开的某一步被拒：关窗复原、记失败   T40 关窗时策略写回被拒重试、别的步骤被拒跳过
 *   T41 链路复位 / 网络掉了：窗口回 CLOSED / 走关窗   T42 不在 RUNNING / UP、超过 254 s：拒
 *   T43 开的那一步在途时网络掉了：它迟到的 OK 不当成关窗的一步，关窗四条一条不少
 *   T44 UP 之后的序列没走完就要开窗：等 want 槽空了再交；广播回显（非幂等，UNKNOWN）：开失败、关窗复原
 *   T45 关窗写回策略连着被拒 6 次：复位链路（NCP 重启后 k_init 写回），记 WIN_POLICY
 *   T47 应用命令队列：不在 RUNNING 拒、优先级最低（want 槽 / 窗口先）、结果带原 tag 回、满了拒、链路复位丢掉排队的；会话不收的帧丢掉回 INVALID
 *   T46 总时长上限：滚动一小时 600 s —— 254 + 254 之后第三次截到 92 s、再开回 QUOTA 带等待时长、过一小时又能开；早关 / 开失败只算真开的 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "zh_host_core.h"

static int s_fail, s_pass;
#define CHECK(cond, ...) do { if (cond) { s_pass++; } else { s_fail++; printf("❌ %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------------------------------------------------------- 线上字节（测试自己的编解码，独立于被测代码） */

typedef struct { uint8_t b[ZH_ASH_RAW_MAX + 4]; size_t n; } rawf_t;
static int split(const uint8_t *w, size_t n, rawf_t *out, int max)
{
    int k = 0, esc = 0;
    size_t cur = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t b = w[i];
        if (b == ZH_ASH_CAN) { cur = 0; continue; }
        if (b == ZH_ASH_FLAG) { if (cur && k < max) { out[k].n = cur; k++; } cur = 0; continue; }
        if (b == ZH_ASH_ESC) { esc = 1; continue; }
        if (k < max && cur < sizeof(out[k].b)) out[k].b[cur++] = esc ? (uint8_t)(b ^ 0x20) : b;
        esc = 0;
    }
    return k;
}
static uint16_t crc_ref(const uint8_t *p, size_t n)
{
    uint16_t c = 0xFFFF;
    while (n--) {
        uint8_t x = (uint8_t)((c >> 8) ^ *p++);
        x ^= (uint8_t)(x >> 4);
        c = (uint16_t)((c << 8) ^ ((uint16_t)x << 12) ^ ((uint16_t)x << 5) ^ (uint16_t)x);
    }
    return c;
}
static void lfsr(uint8_t *d, size_t len)
{
    uint8_t r = 0x42;
    for (size_t i = 0; i < len; i++) { d[i] ^= r; r = (r & 1) ? (uint8_t)((r >> 1) ^ 0xB8) : (uint8_t)(r >> 1); }
}
static size_t ncp_frame(uint8_t ctrl, const uint8_t *data, size_t len, uint8_t *out)
{
    uint8_t raw[ZH_ASH_RAW_MAX];
    raw[0] = ctrl;
    if (len) memcpy(raw + 1, data, len);
    if ((ctrl & 0x80) == 0) lfsr(raw + 1, len);
    const uint16_t c = crc_ref(raw, 1 + len);
    raw[1 + len] = (uint8_t)(c >> 8);
    raw[2 + len] = (uint8_t)c;
    size_t n = 0;
    for (size_t i = 0; i < 3 + len; i++) {
        const uint8_t b = raw[i];
        if (b == 0x7E || b == 0x7D || b == 0x11 || b == 0x13 || b == 0x18 || b == 0x1A) { out[n++] = 0x7D; out[n++] = (uint8_t)(b ^ 0x20); }
        else out[n++] = b;
    }
    out[n++] = 0x7E;
    return n;
}

/* ---------------------------------------------------------------- 手搓的 NCP */

static zh_host_t H;
static uint32_t  T;
static bool      s_dead;            /* NCP 整个不响应（连 ASH 都不回） */
static bool      s_auto_nop;        /* 自动回保活 nop */
static bool      s_auto_params;     /* 自动回 getNetworkParameters */
static bool      s_auto_up;         /* 自动回 UP 之后的其余几步：getEui64、setConcentrator（回 s_conc_status）、setSourceRouteDiscoveryMode */
static bool      s_hold_conc;       /* setConcentrator 先不回 */
static bool      s_hold_eui, s_hold_mtorr;   /* getEui64 / setSourceRouteDiscoveryMode 先不回 */
static bool      s_auto_win;        /* RUNNING 里自动回入网窗口的命令：setPolicy / permitJoining / sendBroadcast / clearTransientLinkKeys */
static bool      s_hold_pj;         /* permitJoining 先不回 */
static bool      s_hold_pol;        /* RUNNING 里的 setPolicy 先不回 */
static uint32_t  s_pol_status, s_pj_status, s_bc_status, s_key_status;   /* 回的状态 */
static uint32_t  s_conc_status;
static bool      s_auto_ncp;        /* 自动扮完整的 NCP：收到 RST 回 RSTACK、回 version、networkInit 回 OK（从不报 UP）、networkState 回 s_auto_ns */
static uint8_t   s_auto_ns;
static uint8_t   s_ncp_frm, s_rx_exp;   /* NCP 下一个 DATA 的帧号 / 期望收到的主机帧号（= 要回的 ackNum） */
static uint32_t  s_tx_bytes, s_rst;
typedef struct { uint8_t e[ZH_ASH_DATA_MAX]; size_t n; } cmd_t;
static cmd_t     s_cmd[64];
static int       s_ncmd;            /* 主机发来的新 DATA 帧（去随机化后的 EZSP 帧），不含重传 */
static int       s_by_id[0x100];    /* 按帧 ID 低字节计数 */
static int       s_net_events[4], s_cbs;

static uint16_t cmd_id(const cmd_t *c) { return (c->e[2] & 0x03) != 0x01 ? c->e[2] : (uint16_t)(c->e[3] | (c->e[4] << 8)); }
static const cmd_t *last_cmd(void) { return s_ncmd ? &s_cmd[(s_ncmd - 1) % 64] : NULL; }

static void feed(uint8_t ctrl, const uint8_t *d, size_t n)
{
    uint8_t w[600];
    const size_t m = ncp_frame(ctrl, d, n, w);
    zh_host_rx(&H, w, m, T);
    zh_host_tick(&H, T);
}
static void ncp_rstack(uint8_t reason)
{
    const uint8_t d[2] = { 2, reason };
    s_ncp_frm = s_rx_exp = 0;
    feed(ZH_ASH_CTRL_RSTACK, d, 2);
}
static void ncp_data(const uint8_t *e, size_t n)
{
    const uint8_t ctrl = (uint8_t)((s_ncp_frm << 4) | s_rx_exp);
    s_ncp_frm = (uint8_t)((s_ncp_frm + 1) & 7);
    feed(ctrl, e, n);
}
/* 扩展头应答：[seq][0x80][0x01][id lo][id hi][参数] */
static void ncp_rsp(uint8_t seq, uint16_t id, const uint8_t *p, size_t n)
{
    uint8_t e[ZH_ASH_DATA_MAX];
    e[0] = seq; e[1] = 0x80; e[2] = 0x01; e[3] = (uint8_t)id; e[4] = (uint8_t)(id >> 8);
    if (n) memcpy(e + 5, p, n);
    ncp_data(e, 5 + n);
}
static void ncp_rsp_status(uint8_t seq, uint16_t id, uint32_t st)
{
    const uint8_t p[4] = { (uint8_t)st, (uint8_t)(st >> 8), (uint8_t)(st >> 16), (uint8_t)(st >> 24) };
    ncp_rsp(seq, id, p, 4);
}
/* 异步回调 stackStatusHandler(st)：fc 0x90 */
static void ncp_stack_status(uint32_t st)
{
    const uint8_t e[] = { 0x00, 0x90, 0x01, (uint8_t)ZH_EZSP_ID_STACK_STATUS_HANDLER, 0x00,
                          (uint8_t)st, (uint8_t)(st >> 8), (uint8_t)(st >> 16), (uint8_t)(st >> 24) };
    ncp_data(e, sizeof(e));
}
static const uint8_t k_eui[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
static void ncp_params(uint8_t seq)
{
    uint8_t p[4 + 1 + 20] = { 0 };
    p[4] = 0x01;                                     /* 协调器 */
    for (int i = 0; i < 8; i++) p[5 + i] = (uint8_t)(0xA0 + i);   /* 扩展 PAN */
    p[13] = 0x34; p[14] = 0x12;                      /* PAN 0x1234 */
    p[15] = 8;                                       /* 发射功率 */
    p[16] = 20;                                      /* 信道 */
    ncp_rsp(seq, ZH_EZSP_ID_GET_NETWORK_PARAMETERS, p, sizeof(p));
}

/* 跑一轮主机 tx：收下新 DATA 帧（去随机化）、ASH 层立即确认、按需自动回 nop / 参数。tx 之后 tick；next_timeout 回 0（tx 里判了失败、
   要立刻复位）就同一毫秒里再来一轮 —— 同 zhub.c 的任务循环 */
static void pump(void)
{
    uint8_t w[4096];
    size_t n;
    for (int round = 0; round < 4; round++) {
    while ((n = zh_host_tx(&H, T, w, sizeof(w))) > 0) {
        s_tx_bytes += (uint32_t)n;
        rawf_t f[16];
        const int k = split(w, n, f, 16);
        for (int i = 0; i < k; i++) {
            if (f[i].n < 3) continue;
            const uint16_t c = crc_ref(f[i].b, f[i].n - 2);
            if (f[i].b[f[i].n - 2] != (uint8_t)(c >> 8) || f[i].b[f[i].n - 1] != (uint8_t)c) continue;
            const uint8_t ctrl = f[i].b[0];
            if (ctrl == ZH_ASH_CTRL_RST) { s_rst++; if (s_auto_ncp && !s_dead) ncp_rstack(ZH_ASH_RESET_SOFTWARE); continue; }
            if (s_dead || (ctrl & 0x80)) continue;       /* NCP 不响应 / 主机的 ACK / NAK */
            const uint8_t frm = (uint8_t)((ctrl >> 4) & 7);
            if (frm != s_rx_exp) continue;               /* 重传 */
            s_rx_exp = (uint8_t)((s_rx_exp + 1) & 7);
            cmd_t *cm = &s_cmd[s_ncmd % 64];
            cm->n = f[i].n - 3;
            memcpy(cm->e, f[i].b + 1, cm->n);
            lfsr(cm->e, cm->n);
            s_ncmd++;
            s_by_id[cmd_id(cm) & 0xFF]++;
            feed((uint8_t)(0x80 | s_rx_exp), NULL, 0);   /* ASH 确认 */
            if (s_auto_nop && cmd_id(cm) == ZH_EZSP_ID_NOP) ncp_rsp(cm->e[0], ZH_EZSP_ID_NOP, NULL, 0);
            if (s_auto_params && cmd_id(cm) == ZH_EZSP_ID_GET_NETWORK_PARAMETERS) ncp_params(cm->e[0]);
            if (s_auto_up && !s_hold_eui && cmd_id(cm) == ZH_EZSP_ID_GET_EUI64) ncp_rsp(cm->e[0], ZH_EZSP_ID_GET_EUI64, k_eui, 8);
            if (s_auto_up && !s_hold_conc && cmd_id(cm) == ZH_EZSP_ID_SET_CONCENTRATOR) ncp_rsp_status(cm->e[0], ZH_EZSP_ID_SET_CONCENTRATOR, s_conc_status);
            if (s_auto_up && !s_hold_mtorr && cmd_id(cm) == ZH_EZSP_ID_SET_SOURCE_ROUTE_DISCOVERY_MODE) {
                const uint8_t rem[4] = { 0x60, 0xEA, 0x00, 0x00 };   /* 60000 ms */
                ncp_rsp(cm->e[0], ZH_EZSP_ID_SET_SOURCE_ROUTE_DISCOVERY_MODE, rem, 4);
            }
            if (s_auto_ncp && cmd_id(cm) == ZH_EZSP_ID_VERSION && (cm->e[2] & 0x03) != 0x01) {
                const uint8_t vr[] = { cm->e[0], 0x80, 0x00, 0x12, 0x02, 0x34, 0x12 };
                ncp_data(vr, sizeof(vr));
            }
            if (s_auto_ncp && cmd_id(cm) == ZH_EZSP_ID_SET_CONFIGURATION_VALUE) ncp_rsp_status(cm->e[0], ZH_EZSP_ID_SET_CONFIGURATION_VALUE, ZH_SL_OK);
            if (s_auto_ncp && cmd_id(cm) == ZH_EZSP_ID_SET_POLICY) ncp_rsp_status(cm->e[0], ZH_EZSP_ID_SET_POLICY, ZH_SL_OK);
            if (s_auto_win && !s_auto_ncp && H.state == ZH_HOST_RUNNING) {
                const uint16_t wid = cmd_id(cm);
                if (wid == ZH_EZSP_ID_SET_POLICY && !s_hold_pol) ncp_rsp_status(cm->e[0], wid, s_pol_status);
                if (wid == ZH_EZSP_ID_PERMIT_JOINING && !s_hold_pj) ncp_rsp_status(cm->e[0], wid, s_pj_status);
                if (wid == ZH_EZSP_ID_CLEAR_TRANSIENT_LINK_KEYS) ncp_rsp(cm->e[0], wid, NULL, 0);
                if (wid == ZH_EZSP_ID_SEC_MAN_IMPORT_TRANSIENT_KEY) ncp_rsp_status(cm->e[0], wid, s_key_status);
                if (wid == ZH_EZSP_ID_SEND_BROADCAST) {
                    const uint8_t bp[5] = { (uint8_t)s_bc_status, (uint8_t)(s_bc_status >> 8), 0, 0, 0x42 };   /* 状态 + APS 序号 */
                    ncp_rsp(cm->e[0], wid, bp, 5);
                }
            }
            if (s_auto_ncp && cmd_id(cm) == ZH_EZSP_ID_ADD_ENDPOINT) ncp_rsp_status(cm->e[0], ZH_EZSP_ID_ADD_ENDPOINT, ZH_SL_OK);
            if (s_auto_ncp && cmd_id(cm) == ZH_EZSP_ID_NETWORK_INIT) ncp_rsp_status(cm->e[0], ZH_EZSP_ID_NETWORK_INIT, ZH_SL_OK);
            if (s_auto_ncp && cmd_id(cm) == ZH_EZSP_ID_NETWORK_STATE) ncp_rsp(cm->e[0], ZH_EZSP_ID_NETWORK_STATE, &s_auto_ns, 1);
        }
    }
    zh_host_tick(&H, T);
    if (zh_host_next_timeout_ms(&H, T) != 0) break;
    }
}
/* 每毫秒查一条不变量：退避 / 挂起时 ASH 与会话都必须是 DOWN（第一轮审计建议：退避期间 ASH 还连着就会回 ACK、会重传） */
static uint32_t s_inv_bad;
static void advance(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        T++;
        zh_host_tick(&H, T);
        pump();
        if ((H.state == ZH_HOST_BACKOFF || H.state == ZH_HOST_IDLE) && (H.ash.state != ZH_ASH_DOWN || H.ses.state != ZH_SES_DOWN)) s_inv_bad++;
    }
}

static void on_net(void *ctx, zh_host_net_t n) { (void)ctx; if (n < 4) s_net_events[n]++; }
static void on_cb(void *ctx, const zh_ezsp_frame_t *fr) { (void)ctx; (void)fr; s_cbs++; }
static uint32_t s_app_tag[16];
static int      s_app_r[16], s_app_n;
static void on_app(void *ctx, uint32_t tag, zh_ses_result_t r, const zh_ezsp_frame_t *fr) { (void)ctx; (void)fr; s_app_tag[s_app_n % 16] = tag; s_app_r[s_app_n % 16] = r; s_app_n++; }

static void fresh(void)
{
    T = 1000;
    s_dead = false;
    s_auto_nop = s_auto_params = s_auto_up = true;
    s_hold_conc = s_hold_eui = s_hold_mtorr = false;
    s_conc_status = ZH_SL_OK;
    s_auto_win = true;
    s_hold_pj = s_hold_pol = false;
    s_pol_status = s_pj_status = s_bc_status = s_key_status = ZH_SL_OK;
    s_auto_ncp = false;
    s_auto_ns = 0;
    s_ncp_frm = s_rx_exp = 0;
    s_tx_bytes = s_rst = 0;
    s_ncmd = 0;
    s_cbs = 0;
    memset(s_by_id, 0, sizeof(s_by_id));
    memset(s_net_events, 0, sizeof(s_net_events));
    s_app_n = 0;
    const zh_host_ops_t ops = { on_net, on_cb, NULL, on_app };
    zh_host_init(&H, &ops);
}

/* 握手之后的初始化序列：主机发的每一条都要和这张表逐字节相同（参数部分，帧头之后），各回 OK。这张表按设计 8.5 与 SDK 枚举手写，
   不读被测代码的常量：两个协议参数（表 8.5-1）→ 7 条策略（表 8.5-2）→ 端点 1（表 8.5-3） */
typedef struct { uint16_t id; uint8_t n; uint8_t p[40]; } want_cmd_t;
static const want_cmd_t k_want_init[] = {
    { ZH_EZSP_ID_SET_CONFIGURATION_VALUE, 3, { 0x0C, 0x02, 0x00 } },   /* STACK_PROFILE = 2 */
    { ZH_EZSP_ID_SET_CONFIGURATION_VALUE, 3, { 0x0D, 0x05, 0x00 } },   /* SECURITY_LEVEL = 5 */
    { ZH_EZSP_ID_SET_CONFIGURATION_VALUE, 3, { 0x12, 0x00, 0x1E } },   /* INDIRECT_TRANSMISSION_TIMEOUT = 7680（0x1E00）：第七步 b */
    { ZH_EZSP_ID_SET_POLICY, 2, { 0x00, 0x02 } },                      /* 信任中心：只许重新入网（窗口外） */
    { ZH_EZSP_ID_SET_POLICY, 2, { 0x09, 0x00 } },                      /* 用公开密钥的 TC 重新入网：不许 */
    { ZH_EZSP_ID_SET_POLICY, 2, { 0x05, 0x51 } },                      /* TC 链路密钥申请：允许，回当前密钥 */
    { ZH_EZSP_ID_SET_POLICY, 2, { 0x06, 0x60 } },                      /* 应用链路密钥申请：拒绝 */
    { ZH_EZSP_ID_SET_POLICY, 2, { 0x01, 0x10 } },                      /* 远端改绑定表：不许 */
    { ZH_EZSP_ID_SET_POLICY, 2, { 0x04, 0x40 } },                      /* messageSent 只带 tag */
    { ZH_EZSP_ID_SET_POLICY, 2, { 0x03, 0x30 } },                      /* 子设备轮询：不报 */
    { ZH_EZSP_ID_ADD_ENDPOINT, 32, { 0x01, 0x04, 0x01, 0x00, 0x04, 0x00, 0x03, 0x09,          /* ep 1、HA、IAS CIE、版本 0、3 进 9 出 */
                                     0x00, 0x00, 0x03, 0x00, 0x0A, 0x00,                      /* Basic、Identify、Time */
                                     0x03, 0x00, 0x06, 0x00, 0x08, 0x00, 0x00, 0x03, 0x01, 0x01, 0x02, 0x01, 0x01, 0x02, 0x00, 0x05, 0x02, 0x05 } },
};
#define K_WANT_INIT_N ((int)(sizeof(k_want_init) / sizeof(k_want_init[0])))
static int s_init_bad;
static bool same_cmd_as(const cmd_t *c, const want_cmd_t *w)
{
    return c && cmd_id(c) == w->id && c->n == 5u + w->n && !memcmp(c->e + 5, w->p, w->n);
}
/* 回到第 upto 步（不含）为止，各回 OK；upto = K_WANT_INIT_N 就是整个序列 */
static bool answer_init_upto(int upto)
{
    for (int i = 0; i < upto; i++) {
        const cmd_t *c = last_cmd();
        if (!same_cmd_as(c, &k_want_init[i])) {
            s_init_bad++;
            printf("   初始化第 %d 步不对：帧 ID 0x%04X、%u 字节\n", i, c ? cmd_id(c) : 0xFFFF, c ? (unsigned)c->n : 0);
            return false;
        }
        ncp_rsp_status(c->e[0], k_want_init[i].id, ZH_SL_OK);   /* 应答帧 ID = 命令帧 ID */
        pump();
    }
    return true;
}
static bool answer_init(void) { return answer_init_upto(K_WANT_INIT_N); }

/* 从 start 走到"握手完成、初始化序列第一条已交出去"（失败返回 false） */
static bool to_ready(void)
{
    zh_host_start(&H, T);
    pump();
    if (H.state != ZH_HOST_RESETTING || s_rst == 0) return false;
    ncp_rstack(ZH_ASH_RESET_SOFTWARE);
    pump();
    const cmd_t *v = last_cmd();
    if (H.state != ZH_HOST_HANDSHAKE || !v || cmd_id(v) != ZH_EZSP_ID_VERSION) return false;
    const uint8_t vr[] = { v->e[0], 0x80, 0x00, 0x12, 0x02, 0x34, 0x12 };
    ncp_data(vr, sizeof(vr));
    pump();
    return H.state == ZH_HOST_NET_INIT;
}

/* 从 start 走到"networkInit 已交出去"：返回 networkInit 那条命令的 seq（失败返回 -1） */
static int to_net_init(void)
{
    zh_host_start(&H, T);
    pump();
    if (H.state != ZH_HOST_RESETTING || s_rst == 0) return -1;
    ncp_rstack(ZH_ASH_RESET_SOFTWARE);
    pump();
    const cmd_t *v = last_cmd();
    if (H.state != ZH_HOST_HANDSHAKE || !v || cmd_id(v) != ZH_EZSP_ID_VERSION) return -1;
    const uint8_t vr[] = { v->e[0], 0x80, 0x00, 0x12, 0x02, 0x34, 0x12 };   /* 旧式头的 version 应答：协议 0x12、stackType 2、版本 0x1234 */
    ncp_data(vr, sizeof(vr));
    pump();
    if (!answer_init()) return -1;
    const cmd_t *ni = last_cmd();
    if (H.state != ZH_HOST_NET_INIT || !ni || cmd_id(ni) != ZH_EZSP_ID_NETWORK_INIT) return -1;
    return ni->e[0];
}
static bool to_running_up(void)
{
    const int seq = to_net_init();
    if (seq < 0) return false;
    ncp_rsp_status((uint8_t)seq, ZH_EZSP_ID_NETWORK_INIT, ZH_SL_OK);
    pump();
    ncp_stack_status(ZH_SL_NETWORK_UP);
    pump();
    return H.state == ZH_HOST_RUNNING && H.net == ZH_NET_UP;
}

int main(void)
{
    /* ---- T1 ---- */
    fresh();
    CHECK(H.state == ZH_HOST_IDLE && H.net == ZH_NET_UNKNOWN, "T1 初始 IDLE / UNKNOWN");
    CHECK(zh_host_next_timeout_ms(&H, T) == UINT32_MAX, "T1 IDLE 没有定时器");
    uint8_t w[600];
    CHECK(zh_host_tx(&H, T, w, sizeof(w)) == 0, "T1 IDLE 不发");
    feed(ZH_ASH_CTRL_RSTACK, (const uint8_t[]){ 2, ZH_ASH_RESET_SOFTWARE }, 2);
    CHECK(H.state == ZH_HOST_IDLE && H.ash.st.rx_frames == 0, "T1 IDLE 不收");
    zh_host_init(NULL, NULL); zh_host_start(NULL, 0); zh_host_stop(NULL); zh_host_restart(NULL, 0);
    zh_host_rx(NULL, w, 1, 0); zh_host_tick(NULL, 0);
    CHECK(zh_host_tx(NULL, 0, w, sizeof(w)) == 0 && zh_host_next_timeout_ms(NULL, 0) == UINT32_MAX && !zh_host_alive_evidence(NULL, NULL), "T1 NULL 不崩");
    CHECK(!strcmp(zh_host_state_name(ZH_HOST_RUNNING), "running") && !strcmp(zh_host_net_name(ZH_NET_NONE), "none")
          && !strcmp(zh_host_fail_name(ZH_HOST_FAIL_NET_INIT), "net_init") && !strcmp(zh_host_state_name((zh_host_state_t)99), "?"), "T1 名字");

    /* ---- T2 ---- */
    fresh();
    int seq = to_net_init();
    CHECK(seq >= 0, "T2 RST → RSTACK → version → networkInit");
    const cmd_t *ni = last_cmd();
    CHECK(ni && ni->n == 7 && ni->e[5] == 0x03 && ni->e[6] == 0x00, "T2 networkInit 位图 0x0003（%02X %02X）", ni ? ni->e[5] : 0, ni ? ni->e[6] : 0);
    CHECK(H.have_version && H.version_code == ((2u << 16) | 0x1234u) && H.st.readies == 1, "T2 握手记下 stackType / 版本");
    ncp_rsp_status((uint8_t)seq, ZH_EZSP_ID_NETWORK_INIT, ZH_SL_OK);
    pump();
    CHECK(H.state == ZH_HOST_NET_INIT && H.wait_up, "T2 OK 之后等 NETWORK_UP");
    CHECK(zh_host_next_timeout_ms(&H, T) <= ZH_HOST_STACK_UP_WAIT_MS, "T2 等 UP 有时限");
    ncp_stack_status(ZH_SL_NETWORK_UP);
    pump();
    CHECK(H.state == ZH_HOST_RUNNING && H.net == ZH_NET_UP, "T2 UP → RUNNING");
    CHECK(H.have_params && H.params.channel == 20 && H.params.pan_id == 0x1234 && H.params.ext_pan_id[0] == 0xA0 && H.node_type == 1,
          "T2 读回信道 / PAN / 扩展 PAN");
    CHECK(H.st.resets == 1 && H.fails == 0 && H.st.net_inits == 1 && H.st.stack_status == 1, "T2 计数");
    uint32_t ta = 0;
    CHECK(zh_host_alive_evidence(&H, &ta) && ta == T, "T2 正向证据");
    CHECK(s_net_events[ZH_NET_UP] == 1 && s_cbs == 1, "T2 on_net(UP) 一次、回调交上层一次");
    advance(65000);                                  /* 保活自动回：一直 RUNNING */
    CHECK(H.state == ZH_HOST_RUNNING && s_by_id[ZH_EZSP_ID_NOP] >= 5 && H.st.resets == 1, "T2 保活 65 s：%d 条 nop、没复位", s_by_id[ZH_EZSP_ID_NOP]);

    /* ---- T3 ---- */
    fresh();
    seq = to_net_init();
    ncp_stack_status(ZH_SL_NETWORK_UP);
    pump();
    CHECK(H.state == ZH_HOST_NET_INIT && H.net == ZH_NET_UP, "T3 UP 先到：还在等应答");
    ncp_rsp_status((uint8_t)seq, ZH_EZSP_ID_NETWORK_INIT, ZH_SL_OK);
    pump();
    CHECK(H.state == ZH_HOST_RUNNING && H.have_params, "T3 应答一到就 RUNNING、读参数");

    /* ---- T4 ---- */
    fresh();
    seq = to_net_init();
    ncp_rsp_status((uint8_t)seq, ZH_EZSP_ID_NETWORK_INIT, ZH_SL_NOT_JOINED);
    pump();
    CHECK(H.state == ZH_HOST_RUNNING && H.net == ZH_NET_NONE && !H.have_params, "T4 NOT_JOINED：RUNNING / NONE、不读参数");
    advance(200000);
    {
        int others = 0;
        for (int i = 0; i < 0x100; i++)
            if (i != ZH_EZSP_ID_VERSION && i != ZH_EZSP_ID_NETWORK_INIT && i != ZH_EZSP_ID_NOP && i != ZH_EZSP_ID_CALLBACK &&
                i != ZH_EZSP_ID_SET_CONFIGURATION_VALUE && i != ZH_EZSP_ID_SET_POLICY && i != ZH_EZSP_ID_ADD_ENDPOINT) others += s_by_id[i];
        CHECK(others == 0 && s_by_id[ZH_EZSP_ID_NETWORK_INIT] == 1 && s_by_id[ZH_EZSP_ID_SET_CONFIGURATION_VALUE] == 3 && s_by_id[ZH_EZSP_ID_SET_POLICY] == 7
              && s_by_id[ZH_EZSP_ID_ADD_ENDPOINT] == 1,
              "T4 200 s 里只发过 version / 初始化序列 / networkInit(1 次) / nop：绝不建网、没有网络不开集中器（%d 条别的）", others);
    }

    /* ---- T5：刚复位过的 NCP networkInit 回错误码：这次启动里不再重试，记 NET_INIT 失败、复位重来 ---- */
    fresh();
    seq = to_net_init();
    s_rst = 0;
    ncp_rsp_status((uint8_t)seq, ZH_EZSP_ID_NETWORK_INIT, 0x0002);
    pump();
    CHECK(H.last_fail == ZH_HOST_FAIL_NET_INIT && H.last_fail_code == (0x10000u | 0x0002u) && H.state == ZH_HOST_RESETTING && s_rst == 1 && H.fails == 1,
          "T5 错误码：记 NET_INIT（code 0x%X）、立即复位", (unsigned)H.last_fail_code);
    CHECK(s_by_id[ZH_EZSP_ID_NETWORK_INIT] == 1, "T5 同一次启动里没有第二条 networkInit");

    /* ---- T6：networkInit 说 OK、NETWORK_UP 等不到：10 s 后查 networkState，不重发 networkInit ---- */
    fresh();
    seq = to_net_init();
    ncp_rsp_status((uint8_t)seq, ZH_EZSP_ID_NETWORK_INIT, ZH_SL_OK);
    pump();
    const uint32_t t6 = T;
    while (s_by_id[ZH_EZSP_ID_NETWORK_STATE] == 0 && T - t6 < 20000) advance(1);
    CHECK(s_by_id[ZH_EZSP_ID_NETWORK_STATE] == 1 && T - t6 == ZH_HOST_STACK_UP_WAIT_MS && s_by_id[ZH_EZSP_ID_NETWORK_INIT] == 1,
          "T6 等 UP %u ms 超时 → 问 networkState（不重发 networkInit）", (unsigned)(T - t6));
    {
        const uint8_t joined[1] = { ZH_NET_STATUS_JOINED };
        ncp_rsp(last_cmd()->e[0], ZH_EZSP_ID_NETWORK_STATE, joined, 1);
        pump();
    }
    CHECK(H.state == ZH_HOST_RUNNING && H.net == ZH_NET_UP && H.have_params && H.fails == 0, "T6 回 JOINED：回调丢了而网络其实在 → RUNNING / UP、读参数");
    /* T6b：networkState 回"不在网络里" → RUNNING / DOWN，60 s 期限到了再查一次，还不在就记 NET_DOWN、复位 */
    fresh();
    seq = to_net_init();
    ncp_rsp_status((uint8_t)seq, ZH_EZSP_ID_NETWORK_INIT, ZH_SL_OK);
    pump();
    for (int g = 0; g < 20000 && s_by_id[ZH_EZSP_ID_NETWORK_STATE] == 0; g++) advance(1);
    {
        const uint8_t none[1] = { 0 };
        ncp_rsp(last_cmd()->e[0], ZH_EZSP_ID_NETWORK_STATE, none, 1);
        pump();
        CHECK(H.state == ZH_HOST_RUNNING && H.net == ZH_NET_DOWN && H.timer_on && H.fails == 0, "T6b 不在网络里：RUNNING / DOWN、开始 down 计时");
        const uint32_t t0 = T;
        while (s_by_id[ZH_EZSP_ID_NETWORK_STATE] < 2 && T - t0 < 70000) advance(1);
        CHECK(T - t0 == ZH_HOST_NET_DOWN_MS && s_by_id[ZH_EZSP_ID_NETWORK_INIT] == 1, "T6b %u ms 后再查（期间不发 networkInit）", (unsigned)(T - t0));
        s_rst = 0;
        ncp_rsp(last_cmd()->e[0], ZH_EZSP_ID_NETWORK_STATE, none, 1);
        pump();
        CHECK(H.last_fail == ZH_HOST_FAIL_NET_DOWN && H.last_fail_code == 0 && H.state == ZH_HOST_RESETTING && s_rst == 1 && H.fails == 1,
              "T6b 还不在：记 NET_DOWN、复位重来（NCP 的新一次启动里再 networkInit）");
    }

    /* ---- T7：运行中 NETWORK_DOWN：给 NCP 60 s 自己恢复；期间 UP 回来就取消；没回来查一次再复位。全程不在这次启动里重发 networkInit ---- */
    fresh();
    CHECK(to_running_up(), "T7 起来");
    ncp_stack_status(ZH_SL_NETWORK_DOWN);
    pump();
    CHECK(H.net == ZH_NET_DOWN && H.state == ZH_HOST_RUNNING && H.timer_on, "T7 NETWORK_DOWN：RUNNING / DOWN、开始 down 计时");
    advance(30000);
    ncp_stack_status(ZH_SL_NETWORK_DOWN);            /* 反复报 DOWN 不把期限往后推 */
    pump();
    ncp_stack_status(ZH_SL_NETWORK_UP);
    pump();
    advance(40000);
    CHECK(H.net == ZH_NET_UP && !H.timer_on && s_by_id[ZH_EZSP_ID_NETWORK_STATE] == 0 && s_by_id[ZH_EZSP_ID_NETWORK_INIT] == 1 && H.st.resets == 1,
          "T7 60 s 内回来了：取消计时，不查不复位");
    ncp_stack_status(ZH_SL_NETWORK_DOWN);
    pump();
    {
        const uint32_t t0 = T;
        advance(20000);
        ncp_stack_status(ZH_SL_NETWORK_DOWN);
        pump();
        while (s_by_id[ZH_EZSP_ID_NETWORK_STATE] == 0 && T - t0 < 70000) advance(1);
        CHECK(T - t0 == ZH_HOST_NET_DOWN_MS, "T7 期限从第一次 DOWN 起算（%u ms，中途又报 DOWN 不重新计时）", (unsigned)(T - t0));
        const uint8_t joined[1] = { ZH_NET_STATUS_JOINED };
        ncp_rsp(last_cmd()->e[0], ZH_EZSP_ID_NETWORK_STATE, joined, 1);
        pump();
        CHECK(H.net == ZH_NET_UP && H.state == ZH_HOST_RUNNING && H.fails == 0 && H.st.resets == 1, "T7 期限到了一查是 JOINED（UP 回调丢了）：不复位");
    }
    ncp_stack_status(ZH_SL_NETWORK_DOWN);
    pump();
    for (int g = 0; g < 70000 && s_by_id[ZH_EZSP_ID_NETWORK_STATE] < 2; g++) advance(1);
    s_rst = 0;
    {
        const uint8_t leaving[1] = { 4 };
        ncp_rsp(last_cmd()->e[0], ZH_EZSP_ID_NETWORK_STATE, leaving, 1);
        pump();
    }
    CHECK(H.last_fail == ZH_HOST_FAIL_NET_DOWN && H.last_fail_code == 4 && s_rst == 1 && s_by_id[ZH_EZSP_ID_NETWORK_INIT] == 1,
          "T7 没回来：记 NET_DOWN（networkState %u）、复位；这次启动里一条 networkInit 都没补发", (unsigned)H.last_fail_code);

    /* ---- T8 ---- */
    fresh();
    zh_host_start(&H, T);
    pump();
    ncp_rstack(ZH_ASH_RESET_SOFTWARE);
    pump();
    {
        const cmd_t *v = last_cmd();
        const uint8_t vr[] = { v->e[0], 0x80, 0x00, 0x0D, 0x02, 0x34, 0x12 };   /* 协议 13 */
        ncp_data(vr, sizeof(vr));
        pump();
    }
    CHECK(H.last_fail == ZH_HOST_FAIL_EZSP_VERSION && H.last_fail_code == 0x0D && H.state == ZH_HOST_RESETTING && s_rst == 2 && H.fails == 1,
          "T8 协议号不对：第 1 次失败立即复位（RST %u 次）", (unsigned)s_rst);

    /* ---- T9 ---- */
    fresh();
    seq = to_net_init();
    {
        const cmd_t *c = last_cmd();
        ncp_data(c->e, c->n);                        /* NCP 把主机的命令原样发回来（em260 原地应答被覆盖） */
        pump();
    }
    CHECK(H.last_fail == ZH_HOST_FAIL_NET_INIT && H.last_fail_code == ZH_SES_R_UNKNOWN && H.state == ZH_HOST_RESETTING
          && s_by_id[ZH_EZSP_ID_NETWORK_INIT] == 1 && H.st.fails[ZH_HOST_FAIL_NET_INIT] == 1,
          "T9 networkInit 回显：不重发（非幂等）、结果未知 → 复位");

    /* ---- T10 ---- */
    fresh();
    seq = to_net_init();
    {
        const uint8_t inv[] = { 0x36 };
        ncp_rsp((uint8_t)seq, ZH_EZSP_ID_INVALID_COMMAND, inv, 1);
        pump();
    }
    CHECK(H.last_fail == ZH_HOST_FAIL_NET_INIT && H.last_fail_code == ZH_SES_R_INVALID && H.state == ZH_HOST_RESETTING, "T10 invalidCommand → 复位");
    {
        fresh();
        seq = to_net_init();
        const uint8_t shortp[3] = { 0, 0, 0 };       /* 少一个字节、CRC 仍对的应答 */
        ncp_rsp((uint8_t)seq, ZH_EZSP_ID_NETWORK_INIT, shortp, 3);
        pump();
        CHECK(H.last_fail == ZH_HOST_FAIL_NET_INIT && H.last_fail_code > 0x100 && H.state == ZH_HOST_RESETTING, "T10 应答短一个字节 → 复位");
    }

    /* ---- T11 ---- */
    fresh();
    s_dead = true;
    zh_host_start(&H, T);
    pump();
    {
        const uint32_t expect_gap[] = { 0, 5000, 30000, 60000, 60000 };   /* 第 k 次失败之后的等待 */
        uint32_t t_fail = 0;
        for (int k = 0; k < 5; k++) {
            const uint32_t f0 = H.fails;
            const uint32_t t0 = T;
            while (H.fails == f0 && T - t0 < 10000) advance(1);
            CHECK(H.fails == f0 + 1 && H.last_fail == ZH_HOST_FAIL_NO_RSTACK && T - t0 == 5000, "T11 第 %d 次：等 RSTACK 5 s 判失败（%u ms）",
                  k + 1, (unsigned)(T - t0));
            t_fail = T;
            const uint32_t bytes0 = s_tx_bytes;
            while (H.state != ZH_HOST_RESETTING && T - t_fail < 70000) advance(1);
            CHECK(T - t_fail == expect_gap[k], "T11 第 %d 次失败之后等 %u ms（应为 %u）", k + 1, (unsigned)(T - t_fail), (unsigned)expect_gap[k]);
            if (expect_gap[k]) CHECK(s_tx_bytes - bytes0 <= 8, "T11 退避期间不发（只有到期那一拍的 CAN + RST：%u 字节）", (unsigned)(s_tx_bytes - bytes0));
            CHECK(zh_host_failing(&H) == (k + 1 >= (int)ZH_HOST_FAILING_AT), "T11 第 %d 次 failing=%d", k + 1, zh_host_failing(&H));
        }
    }
    {
        /* 退避中 next_timeout = 剩余时间 */
        for (int g = 0; g < 200000 && H.state != ZH_HOST_BACKOFF; g++) advance(1);
        const uint32_t nt = zh_host_next_timeout_ms(&H, T);
        CHECK(nt > 0 && nt <= 60000 && nt == H.t_timer - T, "T11 退避中 next_timeout = 剩余 %u ms", (unsigned)nt);
    }

    /* ---- T12 ---- */
    s_dead = false;
    for (int g = 0; g < 200000 && H.state != ZH_HOST_RESETTING; g++) advance(1);
    {
        const uint32_t f = H.fails;
        ncp_rstack(ZH_ASH_RESET_SOFTWARE);
        pump();
        const cmd_t *v = last_cmd();
        const uint8_t vr[] = { v->e[0], 0x80, 0x00, 0x12, 0x02, 0x34, 0x12 };
        ncp_data(vr, sizeof(vr));
        pump();
        answer_init();
        ncp_rsp_status(last_cmd()->e[0], ZH_EZSP_ID_NETWORK_INIT, ZH_SL_OK);
        pump();
        ncp_stack_status(ZH_SL_NETWORK_UP);
        pump();
        CHECK(H.state == ZH_HOST_RUNNING && H.fails == f && f >= 5, "T12 恢复后连续失败先不清（%u）", (unsigned)H.fails);
        advance(ZH_HOST_STABLE_MS - 10);
        CHECK(H.fails == f, "T12 不满 60 s 不清");
        advance(20);
        CHECK(H.fails == 0 && !zh_host_failing(&H), "T12 满 60 s 清零");
    }

    /* ---- T13 ---- */
    fresh();
    CHECK(to_running_up(), "T13 起来");
    s_rst = 0;
    ncp_rstack(0x02);                                /* 运行中 NCP 自己重启了 */
    pump();
    CHECK(H.last_fail == ZH_HOST_FAIL_NCP_RESET && H.last_fail_code == 0x02 && H.state == ZH_HOST_RESETTING && s_rst == 1 && H.net == ZH_NET_UNKNOWN,
          "T13 NCP 自己重启 → 立即复位、网络状态回 UNKNOWN");
    fresh();
    CHECK(to_running_up(), "T13 再起来");
    {
        const uint8_t e[2] = { 2, 0x51 };
        feed(ZH_ASH_CTRL_ERROR, e, 2);
        pump();
    }
    CHECK(H.last_fail == ZH_HOST_FAIL_NCP_ERROR && H.last_fail_code == 0x51 && H.state == ZH_HOST_RESETTING, "T13 ERROR 帧 → 复位");

    /* ---- T14 ---- */
    fresh();
    seq = to_net_init();                             /* networkInit 在途 */
    zh_host_stop(&H);
    CHECK(H.state == ZH_HOST_IDLE && H.net == ZH_NET_UNKNOWN && H.fails == 0 && !H.pend_fail, "T14 挂起：IDLE、在途命令了结不算失败");
    {
        uint32_t sum = 0;
        for (int i = 0; i < ZH_HOST_FAIL_MAX; i++) sum += H.st.fails[i];
        CHECK(sum == 0, "T14 失败计数为 0");
    }
    const uint32_t b14 = s_tx_bytes;
    advance(100000);
    CHECK(s_tx_bytes == b14 && H.state == ZH_HOST_IDLE, "T14 挂起期间一个字节都不发、不自己醒");
    ncp_stack_status(ZH_SL_NETWORK_UP);
    CHECK(H.net == ZH_NET_UNKNOWN, "T14 挂起时不收");
    fresh();
    s_dead = true;
    zh_host_start(&H, T);
    advance(5000 + 5000 + 100);                      /* 两次失败，进 5 s 退避 */
    CHECK(H.state == ZH_HOST_BACKOFF && H.fails == 2, "T14 进退避");
    zh_host_stop(&H);
    s_dead = false;
    zh_host_start(&H, T);
    pump();
    CHECK(H.state == ZH_HOST_RESETTING && H.fails == 0 && H.st.starts == 2, "T14 恢复：从头来、连续失败清零");

    /* ---- T15 ---- */
    fresh();
    s_dead = true;
    zh_host_start(&H, T);
    advance(5000 + 5000 + 100);
    CHECK(H.state == ZH_HOST_BACKOFF, "T15 退避中");
    s_rst = 0;
    zh_host_restart(&H, T);
    pump();
    CHECK(H.state == ZH_HOST_RESETTING && H.fails == 2 && H.st.restarts == 1 && s_rst == 1, "T15 立即复位：跳过退避、不计失败");
    zh_host_stop(&H);
    zh_host_restart(&H, T);
    CHECK(H.state == ZH_HOST_IDLE && H.st.restarts == 1, "T15 IDLE 时无效");

    /* ---- T16 ---- */
    fresh();
    CHECK(to_running_up(), "T16 起来");
    s_auto_nop = false;
    const uint32_t t16 = T;
    while (H.state == ZH_HOST_RUNNING && T - t16 < 40000) advance(1);
    CHECK(H.last_fail == ZH_HOST_FAIL_SESSION && H.last_fail_code == ZH_SES_WHY_KEEPALIVE && H.state == ZH_HOST_RESETTING,
          "T16 保活无应答 → RESET_LINK(KEEPALIVE) → 复位");
    CHECK(T - t16 >= 25000 && T - t16 <= 25100, "T16 时刻：空闲 10 s + 应答超时 15 s（%u ms）", (unsigned)(T - t16));

    /* ---- T17 ---- */
    fresh();
    CHECK(to_running_up(), "T17 起来");
    const int cb0 = s_cbs;
    ncp_stack_status(ZH_SL_TC_SWAP_SAME);
    pump();
    ncp_stack_status(0x0C18);                        /* 入网窗口打开：只记 */
    pump();
    CHECK(s_cbs == cb0 + 2 && H.st.callbacks == 3 && H.last_net_status == 0x0C18 && H.net == ZH_NET_UP, "T17 回调都交上层、只认网络起落");
    {
        const uint8_t bad[] = { 0x00, 0x90, 0x01, (uint8_t)ZH_EZSP_ID_STACK_STATUS_HANDLER, 0x00, 0x15, 0x00 };   /* 状态只有 2 B */
        ncp_data(bad, sizeof(bad));
        pump();
    }
    CHECK(H.st.bad_callbacks == 1 && H.st.callbacks == 4 && H.net == ZH_NET_UP, "T17 状态短了的 stackStatusHandler：交上层、记坏回调、不改网络状态（%u）", (unsigned)H.st.bad_callbacks);
    CHECK(s_net_events[ZH_NET_UP] == 1 && s_net_events[ZH_NET_DOWN] == 0, "T17 on_net 只在变化时调");

    /* ---- T18 ---- */
    fresh();
    s_auto_params = false;
    CHECK(to_running_up(), "T18 起来（参数不自动回）");
    {
        const cmd_t *p = last_cmd();
        CHECK(p && cmd_id(p) == ZH_EZSP_ID_GET_NETWORK_PARAMETERS, "T18 UP 之后读参数");
        const uint8_t inv[] = { 0x36 };
        ncp_rsp(p->e[0], ZH_EZSP_ID_INVALID_COMMAND, inv, 1);
        pump();
        CHECK(H.st.up_bad[ZH_HOST_UP_PARAMS] == 1 && !H.have_params && H.state == ZH_HOST_RUNNING && H.fails == 0, "T18 被拒：只记一笔、不复位");
        const int n0 = s_by_id[ZH_EZSP_ID_GET_NETWORK_PARAMETERS];
        advance(ZH_HOST_UP_RETRY_MS - 1);
        CHECK(s_by_id[ZH_EZSP_ID_GET_NETWORK_PARAMETERS] == n0, "T18 不到 5 s 不重读");
        advance(1);
        CHECK(s_by_id[ZH_EZSP_ID_GET_NETWORK_PARAMETERS] == n0 + 1, "T18 5 s 后重读");
        ncp_params(last_cmd()->e[0]);
        pump();
        CHECK(H.have_params && H.params.channel == 20, "T18 读到了");
        advance(30000);
        CHECK(s_by_id[ZH_EZSP_ID_GET_NETWORK_PARAMETERS] == n0 + 1, "T18 读到之后不再读");
    }

    /* ---- T19：命令在途时自己收摊（restart / NCP 自己重启）交回的结果不算第二次失败（teardown 标志；变异 M2 曾存活） ---- */
    fresh();
    seq = to_net_init();                             /* networkInit 在途 */
    s_rst = 0;
    zh_host_restart(&H, T);
    pump();
    advance(50);
    {
        uint32_t sum = 0;
        for (int i = 0; i < ZH_HOST_FAIL_MAX; i++) sum += H.st.fails[i];
        CHECK(sum == 0 && H.fails == 0 && H.st.resets == 2 && s_rst == 1 && H.state == ZH_HOST_RESETTING,
              "T19 NET_INIT 里 restart：只复位一次、不记失败（失败 %u、复位 %u、RST %u）", sum, H.st.resets, (unsigned)s_rst);
    }
    fresh();
    seq = to_net_init();
    s_rst = 0;
    ncp_rstack(0x02);                                /* networkInit 在途时 NCP 自己重启 */
    pump();
    advance(50);
    CHECK(H.st.fails[ZH_HOST_FAIL_NCP_RESET] == 1 && H.st.fails[ZH_HOST_FAIL_NET_INIT] == 0 && H.fails == 1 && s_rst == 1,
          "T19 networkInit 在途时 NCP 重启：只记一次 NCP_RESET、只复位一次（NET_INIT %u、RST %u）", H.st.fails[ZH_HOST_FAIL_NET_INIT], (unsigned)s_rst);

    /* ---- T20：会话层失败（ASH 还连着）进退避：退避期间 NCP 来帧也不回 ACK、不交上去（zh_ash_stop；变异 M7 曾存活） ---- */
    fresh();
    CHECK(to_running_up(), "T20 起来");
    s_auto_nop = false;
    {
        uint32_t t0 = T;
        while (H.fails < 1 && T - t0 < 40000) advance(1);          /* 第 1 次保活失败：立即重来 */
        ncp_rstack(ZH_ASH_RESET_SOFTWARE);
        pump();
        const cmd_t *v = last_cmd();
        const uint8_t vr[] = { v->e[0], 0x80, 0x00, 0x12, 0x02, 0x34, 0x12 };
        ncp_data(vr, sizeof(vr));
        pump();
        answer_init();
        ncp_rsp_status(last_cmd()->e[0], ZH_EZSP_ID_NETWORK_INIT, ZH_SL_OK);
        pump();
        ncp_stack_status(ZH_SL_NETWORK_UP);
        pump();
        t0 = T;
        while (H.fails < 2 && T - t0 < 40000) advance(1);          /* 第 2 次：退避 5 s */
        CHECK(H.state == ZH_HOST_BACKOFF && H.last_fail == ZH_HOST_FAIL_SESSION && H.fails == 2, "T20 第二次会话失败进退避（%s）", zh_host_state_name(H.state));
        const uint32_t b0 = s_tx_bytes, rx0 = H.ash.st.rx_data, cb0 = H.st.callbacks;
        ncp_stack_status(ZH_SL_NETWORK_UP);                       /* NCP 还在发（它不知道我们放弃了） */
        advance(100);
        CHECK(s_tx_bytes == b0 && H.st.callbacks == cb0 && H.ash.st.rx_data == rx0 && H.state == ZH_HOST_BACKOFF,
              "T20 退避期间 NCP 来帧：不回 ACK、不交上去（发了 %u 字节）", (unsigned)(s_tx_bytes - b0));
    }

    /* ---- T21：networkInit 应答之前先来 NETWORK_DOWN、应答 OK 之后 UP 一直没来：照常走"等 UP → 查 → down 计时"，不被更早的状态带偏（变异 M10 曾存活） ---- */
    fresh();
    seq = to_net_init();
    ncp_stack_status(ZH_SL_NETWORK_UP);              /* 先报 UP（记下 got_up）…… */
    pump();
    ncp_stack_status(ZH_SL_NETWORK_DOWN);            /* ……又报 DOWN：got_up 要清掉 */
    pump();
    ncp_rsp_status((uint8_t)seq, ZH_EZSP_ID_NETWORK_INIT, ZH_SL_OK);
    pump();
    CHECK(H.state == ZH_HOST_NET_INIT && H.wait_up, "T21 应答 OK 时不被早先的 UP 冒充：还在等 UP（%s）", zh_host_state_name(H.state));
    advance(ZH_HOST_STACK_UP_WAIT_MS + 5);
    CHECK(s_by_id[ZH_EZSP_ID_NETWORK_STATE] == 1 && s_by_id[ZH_EZSP_ID_NETWORK_INIT] == 1, "T21 等 UP 超时：查 networkState，不重发 networkInit");

    /* ---- T22：信任中心换机后的两种状态也算网络起来了（af-common.c:352-354；变异 M16 曾存活） ---- */
    fresh();
    seq = to_net_init();
    ncp_rsp_status((uint8_t)seq, ZH_EZSP_ID_NETWORK_INIT, ZH_SL_OK);
    pump();
    ncp_stack_status(ZH_SL_TC_SWAP_CHANGED);
    pump();
    CHECK(H.state == ZH_HOST_RUNNING && H.net == ZH_NET_UP && H.have_params, "T22 TC_SWAP_CHANGED 也算 NETWORK_UP");

    /* ---- T23：同一拍里先后两个失败，只记第一个（变异 M17 曾存活）：networkInit 被拒与 NCP 的 ERROR 帧在同一批字节里 ---- */
    fresh();
    seq = to_net_init();
    {
        uint8_t w2[1200];
        size_t n2 = 0;
        const uint8_t inv[] = { (uint8_t)seq, 0x80, 0x01, (uint8_t)ZH_EZSP_ID_INVALID_COMMAND, 0x00, 0x36 };
        n2 += ncp_frame((uint8_t)((s_ncp_frm << 4) | s_rx_exp), inv, sizeof(inv), w2 + n2);
        s_ncp_frm = (uint8_t)((s_ncp_frm + 1) & 7);
        const uint8_t er[2] = { 2, 0x51 };
        n2 += ncp_frame(ZH_ASH_CTRL_ERROR, er, 2, w2 + n2);
        zh_host_rx(&H, w2, n2, T);
        zh_host_tick(&H, T);
        pump();
    }
    CHECK(H.last_fail == ZH_HOST_FAIL_NET_INIT && H.st.fails[ZH_HOST_FAIL_NCP_ERROR] == 0 && H.fails == 1,
          "T23 同一拍两个失败：只记先到的 NET_INIT（last=%s）", zh_host_fail_name(H.last_fail));

    /* ---- T25：网络一直起不来（networkInit 说 OK、UP 永远不来、networkState 一直回"不在"）：连续失败要攒起来 —— 依次退避、亮 failing
       （复核审计 ❌1：down 期限与稳定期同时到，原先先清零再记 NET_DOWN，失败数永远是 1、每约 70 s 悄悄复位一次） ---- */
    fresh();
    s_auto_ncp = true;
    s_auto_ns = 0;
    zh_host_start(&H, T);
    pump();
    {
        uint32_t max_fails = 0;
        bool seen[4] = { false, false, false, false };         /* 见过的退避档：5 / 30 / 60 s（立即重来那一档不进 BACKOFF） */
        zh_host_state_t prev = H.state;
        for (int i = 0; i < 20 * 60 * 1000; i++) {
            advance(1);
            if (H.fails > max_fails) max_fails = H.fails;
            if (H.state == ZH_HOST_BACKOFF && prev != ZH_HOST_BACKOFF) {
                const uint32_t d = H.t_timer - T;
                if (d == 5000) seen[1] = true;
                else if (d == 30000) seen[2] = true;
                else if (d == 60000) seen[3] = true;
            }
            prev = H.state;
        }
        CHECK(max_fails >= ZH_HOST_FAILING_AT && zh_host_failing(&H) && seen[1] && seen[2] && seen[3] && H.st.fails[ZH_HOST_FAIL_NET_DOWN] >= 4,
              "T25 网络 20 分钟起不来：连续失败攒到 %u、退避 5/30/60 s 都出现（%d%d%d）、failing=%d、NET_DOWN %u 次", (unsigned)max_fails,
              seen[1], seen[2], seen[3], zh_host_failing(&H), H.st.fails[ZH_HOST_FAIL_NET_DOWN]);
        CHECK(H.st.net_inits == H.st.readies, "T25 networkInit 次数 = 握手次数（每次 NCP 启动一条，%u / %u）", H.st.net_inits, H.st.readies);
    }

    /* ---- T26：networkInit / 期限到时的 networkState 撞上 invalidCommand(VERSION_NOT_SET)：失败原因归会话（RESET_LINK(VERSION_NOT_SET)），不记成网络的 ---- */
    fresh();
    seq = to_net_init();
    {
        const uint8_t inv[] = { ZH_SES_INVALID_VERSION_NOT_SET };
        ncp_rsp((uint8_t)seq, ZH_EZSP_ID_INVALID_COMMAND, inv, 1);
        pump();
    }
    CHECK(H.last_fail == ZH_HOST_FAIL_SESSION && H.last_fail_code == ZH_SES_WHY_VERSION_NOT_SET && H.st.fails[ZH_HOST_FAIL_NET_INIT] == 0,
          "T26 networkInit 撞上 VERSION_NOT_SET：记 SESSION / VERSION_NOT_SET（last=%s code %u）", zh_host_fail_name(H.last_fail), (unsigned)H.last_fail_code);
    fresh();
    CHECK(to_running_up(), "T26 起来");
    ncp_stack_status(ZH_SL_NETWORK_DOWN);
    pump();
    for (int g = 0; g < 70000 && s_by_id[ZH_EZSP_ID_NETWORK_STATE] == 0; g++) advance(1);
    {
        const uint8_t inv[] = { ZH_SES_INVALID_VERSION_NOT_SET };
        ncp_rsp(last_cmd()->e[0], ZH_EZSP_ID_INVALID_COMMAND, inv, 1);
        pump();
    }
    CHECK(H.last_fail == ZH_HOST_FAIL_SESSION && H.last_fail_code == ZH_SES_WHY_VERSION_NOT_SET && H.st.fails[ZH_HOST_FAIL_NET_DOWN] == 0,
          "T26 期限那一查撞上 VERSION_NOT_SET：记 SESSION，不记 NET_DOWN（last=%s）", zh_host_fail_name(H.last_fail));

    /* ---- T27：NOT_JOINED 回调：down 期间来 → 记"没有网络"、不复位；等 UP 期间来 → 直接 RUNNING / NONE，不卡在 NET_INIT ---- */
    fresh();
    CHECK(to_running_up(), "T27 起来");
    ncp_stack_status(ZH_SL_NETWORK_DOWN);
    pump();
    for (int g = 0; g < 70000 && s_by_id[ZH_EZSP_ID_NETWORK_STATE] == 0; g++) advance(1);
    ncp_stack_status(ZH_SL_NOT_JOINED);              /* 期限那一查的应答之前 */
    pump();
    {
        const uint8_t none[1] = { 0 };
        s_rst = 0;
        ncp_rsp(last_cmd()->e[0], ZH_EZSP_ID_NETWORK_STATE, none, 1);
        pump();
        advance(200000);
        CHECK(H.net == ZH_NET_NONE && H.state == ZH_HOST_RUNNING && s_rst == 0 && H.st.fails[ZH_HOST_FAIL_NET_DOWN] == 0,
              "T27 down 期间来 NOT_JOINED：没有网络、不复位（RST %u）", (unsigned)s_rst);
    }
    fresh();
    seq = to_net_init();
    ncp_rsp_status((uint8_t)seq, ZH_EZSP_ID_NETWORK_INIT, ZH_SL_OK);
    pump();
    ncp_stack_status(ZH_SL_NOT_JOINED);
    pump();
    advance(ZH_HOST_STACK_UP_WAIT_MS + 100);
    CHECK(H.state == ZH_HOST_RUNNING && H.net == ZH_NET_NONE && s_by_id[ZH_EZSP_ID_NETWORK_STATE] == 0, "T27 等 UP 期间来 NOT_JOINED：直接 RUNNING / NONE（%s）",
          zh_host_state_name(H.state));

    /* ---- T28：从 down 里回来，稳定期从回来那一刻重新起算（down 期间保活照样有应答，不能把它算进"稳定"） ---- */
    fresh();
    CHECK(to_running_up(), "T28 起来");
    s_auto_nop = false;
    for (int g = 0; g < 40000 && H.fails < 1; g++) advance(1);   /* 一次保活失败：连续失败 1、立即重来 */
    s_auto_nop = true;
    ncp_rstack(ZH_ASH_RESET_SOFTWARE);
    pump();
    {
        const cmd_t *v = last_cmd();
        const uint8_t vr[] = { v->e[0], 0x80, 0x00, 0x12, 0x02, 0x34, 0x12 };
        ncp_data(vr, sizeof(vr));
        pump();
        answer_init();
        ncp_rsp_status(last_cmd()->e[0], ZH_EZSP_ID_NETWORK_INIT, ZH_SL_OK);
        pump();
        ncp_stack_status(ZH_SL_NETWORK_UP);
        pump();
    }
    CHECK(H.state == ZH_HOST_RUNNING && H.fails == 1, "T28 恢复运行、连续失败 1");
    advance(50000);
    ncp_stack_status(ZH_SL_NETWORK_DOWN);
    pump();
    advance(30000);
    ncp_stack_status(ZH_SL_NETWORK_UP);
    pump();
    advance(1000);
    CHECK(H.fails == 1, "T28 刚从 down 回来：不清零（进 RUNNING 已 81 s，但其中 30 s 网络是 down 的）");
    advance(ZH_HOST_STABLE_MS);
    CHECK(H.fails == 0, "T28 回来满 60 s：清零");

    /* ---- T29：初始化序列（设计 8.5）：协议参数（真 NCP 出厂 profile 0，不写 networkInit 恒回 NOT_JOINED —— 首次上板栽过）→ 策略 → 端点，
       逐字节和手写的表相同；任何一步被拒：不发后面的、不发 networkInit，记 NET_INIT（code = 0x20000 | 步号 << 8 | 状态低字节）、复位 ---- */
    fresh();
    CHECK(to_net_init() >= 0 && s_init_bad == 0, "T29 握手 → STACK_PROFILE 2 → SECURITY_LEVEL 5 → 6 条策略 → 端点 1 → networkInit（顺序、取值、端点描述逐字节都对）");
    {
        static const struct { int step; uint32_t st; } rej[] = { { 0, 0x02 }, { 4, 0x21 }, { 9, 0x0C } };   /* STACK_PROFILE / TC_KEY 策略 / addEndpoint */
        for (int k = 0; k < 3; k++) {
            fresh();
            CHECK(to_ready() && answer_init_upto(rej[k].step), "T29 拒第 %d 步：走到它", rej[k].step);
            const cmd_t *c = last_cmd();
            CHECK(same_cmd_as(c, &k_want_init[rej[k].step]), "T29 第 %d 步的命令对", rej[k].step);
            s_rst = 0;
            const int n_after0 = s_ncmd;
            ncp_rsp_status(c->e[0], k_want_init[rej[k].step].id, rej[k].st);
            pump();
            const uint32_t want_code = 0x20000u | ((uint32_t)rej[k].step << 8) | rej[k].st;
            CHECK(H.last_fail == ZH_HOST_FAIL_NET_INIT && H.last_fail_code == want_code && s_by_id[ZH_EZSP_ID_NETWORK_INIT] == 0 && s_rst == 1 && s_ncmd == n_after0,
                  "T29 第 %d 步被拒：后面的一条都不发、不发 networkInit、记 NET_INIT（code 0x%X，应为 0x%X）、复位", rej[k].step,
                  (unsigned)H.last_fail_code, (unsigned)want_code);
        }
    }
    {
        /* 端点在 NETWORK_UP 之后不许加（command-prototypes.h:113-122）：每次 NCP 启动恰好一条 addEndpoint，而且在 networkInit 之前 */
        fresh();
        CHECK(to_running_up(), "T29 起来");
        int ep_at = -1, ni_at = -1;
        for (int i = 0; i < s_ncmd && i < 64; i++) {
            if (cmd_id(&s_cmd[i]) == ZH_EZSP_ID_ADD_ENDPOINT && ep_at < 0) ep_at = i;
            if (cmd_id(&s_cmd[i]) == ZH_EZSP_ID_NETWORK_INIT && ni_at < 0) ni_at = i;
        }
        CHECK(ep_at >= 0 && ni_at == ep_at + 1 && s_by_id[ZH_EZSP_ID_ADD_ENDPOINT] == 1, "T29 addEndpoint 恰好一条、紧挨在 networkInit 之前（第 %d / %d 条）", ep_at, ni_at);
    }

    /* ---- T30：UP 之后的序列：getNetworkParameters → getEui64 → setConcentrator(HIGH_RAM 0xFFF9, 10, 60, 3, 1, 0) → setSourceRouteDiscoveryMode(2)，
       逐字节和手写的表相同（SDK 集中器插件默认值 concentrator-config.h、发现模式 RESCHEDULE sl_zigbee_types.h:3058） ---- */
    fresh();
    {
        CHECK(to_running_up(), "T30 起来");
        advance(10);
        static const want_cmd_t k_want_up[] = {
            { ZH_EZSP_ID_GET_NETWORK_PARAMETERS, 0, { 0 } },
            { ZH_EZSP_ID_GET_EUI64, 0, { 0 } },
            { ZH_EZSP_ID_SET_CONCENTRATOR, 10, { 0x01, 0xF9, 0xFF, 0x0A, 0x00, 0x3C, 0x00, 0x03, 0x01, 0x00 } },
            { ZH_EZSP_ID_SET_SOURCE_ROUTE_DISCOVERY_MODE, 1, { 0x02 } },
        };
        int ni_at = -1;
        for (int i = 0; i < s_ncmd && i < 64; i++)
            if (cmd_id(&s_cmd[i]) == ZH_EZSP_ID_NETWORK_INIT) ni_at = i;
        bool seq_ok = ni_at >= 0;
        int k = 0;
        for (int i = ni_at + 1; seq_ok && i < s_ncmd && i < 64 && k < 4; i++) {
            const uint16_t id = cmd_id(&s_cmd[i]);
            if (id == ZH_EZSP_ID_NOP || id == ZH_EZSP_ID_CALLBACK) continue;
            if (!same_cmd_as(&s_cmd[i], &k_want_up[k])) seq_ok = false;
            k++;
        }
        CHECK(seq_ok && k == 4, "T30 UP 之后四条命令的顺序与字节都对（对上 %d 条）", k);
        CHECK(H.have_params && H.have_eui64 && !memcmp(H.eui64, k_eui, 8) && H.concentrator && H.mtorr && H.mtorr_next_ms == 60000 && H.up_step == ZH_HOST_UP_N,
              "T30 快照：参数、EUI64、集中器、MTORR 剩余 60000 ms 都有");
        CHECK(H.st.up_ok[ZH_HOST_UP_PARAMS] == 1 && H.st.up_ok[ZH_HOST_UP_EUI64] == 1 && H.st.up_ok[ZH_HOST_UP_CONCENTRATOR] == 1 && H.st.up_ok[ZH_HOST_UP_MTORR] == 1,
              "T30 每步各成功一次");
        const int c0 = s_by_id[ZH_EZSP_ID_SET_CONCENTRATOR];
        advance(120000);
        CHECK(s_by_id[ZH_EZSP_ID_SET_CONCENTRATOR] == c0 && H.st.resets == 1, "T30 之后 120 s 不再重发、不复位");
    }

    /* ---- T31：集中器被拒：只记账、不复位，5 s 后重试同一步；它没成之前不发 MTORR ---- */
    fresh();
    s_conc_status = 0x0021;                          /* SL_STATUS_INVALID_PARAMETER */
    CHECK(to_running_up(), "T31 起来");
    {
        const uint32_t rs0 = H.st.resets;
        CHECK(!H.concentrator && H.st.up_bad[ZH_HOST_UP_CONCENTRATOR] == 1 && s_by_id[ZH_EZSP_ID_SET_SOURCE_ROUTE_DISCOVERY_MODE] == 0 && H.fails == 0,
              "T31 被拒：记一笔、MTORR 不发、不算失败");
        advance(ZH_HOST_UP_RETRY_MS - 1);
        CHECK(s_by_id[ZH_EZSP_ID_SET_CONCENTRATOR] == 1, "T31 不到 5 s 不重试");
        advance(1);
        CHECK(s_by_id[ZH_EZSP_ID_SET_CONCENTRATOR] == 2 && H.st.up_bad[ZH_HOST_UP_CONCENTRATOR] == 2, "T31 5 s 后重试同一步（又被拒）");
        s_conc_status = ZH_SL_OK;
        advance(ZH_HOST_UP_RETRY_MS);
        CHECK(H.concentrator && H.mtorr && s_by_id[ZH_EZSP_ID_SET_SOURCE_ROUTE_DISCOVERY_MODE] == 1, "T31 这次成了，接着发 MTORR");
        advance(70000);
        CHECK(H.st.resets == rs0 && H.state == ZH_HOST_RUNNING && H.fails == 0 && s_by_id[ZH_EZSP_ID_GET_NETWORK_PARAMETERS] == 1,
              "T31 全程没复位、没重读参数（被拒的那一步不连累前面的）");
    }

    /* ---- T32：同一次 NCP 启动里网络掉了又回来：UP 之后的序列重走（集中器重设、参数重读），初始化序列（策略 / 端点）一条都不重发 ---- */
    fresh();
    CHECK(to_running_up(), "T32 起来");
    ncp_stack_status(ZH_SL_NETWORK_DOWN);
    pump();
    advance(5000);
    ncp_stack_status(ZH_SL_NETWORK_UP);
    pump();
    advance(100);
    CHECK(H.concentrator && H.mtorr && s_by_id[ZH_EZSP_ID_SET_CONCENTRATOR] == 2 && s_by_id[ZH_EZSP_ID_GET_NETWORK_PARAMETERS] == 2
          && s_by_id[ZH_EZSP_ID_ADD_ENDPOINT] == 1 && s_by_id[ZH_EZSP_ID_SET_POLICY] == 7 && s_by_id[ZH_EZSP_ID_NETWORK_INIT] == 1 && H.st.resets == 1,
          "T32 网络回来：集中器 %d 次、参数 %d 次；端点 %d 条、策略 %d 条、networkInit %d 条（不重发）", s_by_id[ZH_EZSP_ID_SET_CONCENTRATOR],
          s_by_id[ZH_EZSP_ID_GET_NETWORK_PARAMETERS], s_by_id[ZH_EZSP_ID_ADD_ENDPOINT], s_by_id[ZH_EZSP_ID_SET_POLICY], s_by_id[ZH_EZSP_ID_NETWORK_INIT]);

    /* ---- T33：setConcentrator 在途时网络掉了又回来（序列从头开始）：它迟到的 OK 不算数，集中器要按新序列重设一遍 ---- */
    fresh();
    s_hold_conc = true;
    CHECK(to_running_up(), "T33 起来");
    {
        const cmd_t *c = last_cmd();
        CHECK(c && cmd_id(c) == ZH_EZSP_ID_SET_CONCENTRATOR && H.up_step == ZH_HOST_UP_CONCENTRATOR, "T33 集中器在途");
        const uint8_t cseq = c->e[0];
        ncp_stack_status(ZH_SL_NETWORK_DOWN);
        pump();
        ncp_stack_status(ZH_SL_NETWORK_UP);
        pump();
        CHECK(H.up_step == ZH_HOST_UP_PARAMS && !H.concentrator, "T33 网络回来：序列回到第一步");
        s_hold_conc = false;                         /* 新序列紧接着就会在同一个 pump 里走到集中器 */
        const uint32_t ok0 = H.st.up_ok[ZH_HOST_UP_PARAMS];
        ncp_rsp_status(cseq, ZH_EZSP_ID_SET_CONCENTRATOR, ZH_SL_OK);   /* 旧的那条迟到的 OK */
        CHECK(!H.concentrator && H.st.up_ok[ZH_HOST_UP_CONCENTRATOR] == 0 && H.st.up_ok[ZH_HOST_UP_PARAMS] == ok0, "T33 迟到的 OK 不算数（序列还在第一步）");
        pump();
        advance(100);
        CHECK(H.concentrator && H.mtorr && s_by_id[ZH_EZSP_ID_SET_CONCENTRATOR] == 2 && H.up_step == ZH_HOST_UP_N, "T33 新序列把集中器重设了一遍");
    }

    /* ---- T34：UP 之后的序列没走完网络就掉了（NETWORK_DOWN / NOT_JOINED）：停下，不对 down 的网络设集中器；被拒排着的重试也不试。回来从头走 ---- */
    for (int v = 0; v < 2; v++) {
        fresh();
        s_hold_conc = true;
        CHECK(to_running_up() && H.up_step == ZH_HOST_UP_CONCENTRATOR, "T34[%d] 集中器在途", v);
        const cmd_t *c = last_cmd();
        const uint8_t cseq = c->e[0];
        ncp_stack_status(v ? ZH_SL_NOT_JOINED : ZH_SL_NETWORK_DOWN);
        pump();
        s_hold_conc = false;
        ncp_rsp_status(cseq, ZH_EZSP_ID_SET_CONCENTRATOR, ZH_SL_OK);   /* 在途那条的 OK：网络已经不在了，不算数 */
        pump();
        advance(20000);
        CHECK(!H.concentrator && !H.mtorr && s_by_id[ZH_EZSP_ID_SET_SOURCE_ROUTE_DISCOVERY_MODE] == 0 && s_by_id[ZH_EZSP_ID_SET_CONCENTRATOR] == 1,
              "T34[%d] 网络%s：序列停下，MTORR 不发、集中器不再发", v, v ? "没了" : "掉了");
        if (!v) {
            ncp_stack_status(ZH_SL_NETWORK_UP);
            pump();
            advance(100);
            CHECK(H.concentrator && H.mtorr && s_by_id[ZH_EZSP_ID_SET_CONCENTRATOR] == 2, "T34 网络回来：从头走完");
        }
    }
    fresh();
    s_conc_status = 0x0021;
    CHECK(to_running_up() && H.up_retry, "T34 集中器被拒、重试排着");
    ncp_stack_status(ZH_SL_NETWORK_DOWN);
    pump();
    CHECK(!H.up_retry && H.up_step == ZH_HOST_UP_N, "T34 网络掉了：排着的重试当场取消（不为它白醒一次）");
    advance(ZH_HOST_UP_RETRY_MS * 3);
    CHECK(s_by_id[ZH_EZSP_ID_SET_CONCENTRATOR] == 1, "T34 之后也没再发集中器（只发过 %d 次）", s_by_id[ZH_EZSP_ID_SET_CONCENTRATOR]);

    /* T34 补：集中器设好之后掉网，快照里的 concentrator / mtorr 跟着清（第一轮审计 ⚠️：原先掉网后仍报 true，ZH09 在 net=down 时会误判） */
    fresh();
    CHECK(to_running_up() && H.concentrator && H.mtorr, "T34 集中器设好了");
    ncp_stack_status(ZH_SL_NETWORK_DOWN);
    pump();
    CHECK(!H.concentrator && !H.mtorr, "T34 掉网：快照里的集中器清掉");

    /* ---- T35：UP 之后的序列遇到截断（fc 0x02）、回显（非幂等的 setSourceRouteDiscoveryMode 回 UNKNOWN）、短一个字节的应答：
       都只记账、5 s 后重试同一步，不复位；解不开的 getEui64 应答不盖掉同一次启动里上一次读到的 EUI64 ---- */
    fresh();
    s_hold_eui = true;
    CHECK(to_running_up() && H.up_step == ZH_HOST_UP_EUI64, "T35 getEui64 在途");
    {
        const cmd_t *c = last_cmd();
        uint8_t e[5 + 8] = { c->e[0], 0x82, 0x01, (uint8_t)ZH_EZSP_ID_GET_EUI64, 0x00 };   /* fc 0x82：NCP 说截断了 */
        memcpy(e + 5, k_eui, 8);
        ncp_data(e, sizeof(e));
        pump();
        CHECK(H.st.up_bad[ZH_HOST_UP_EUI64] == 1 && !H.have_eui64 && H.st.resets == 1 && H.fails == 0 && H.up_retry, "T35 截断：记一笔、排重试、不复位");
        s_hold_eui = false;
        advance(ZH_HOST_UP_RETRY_MS);
        CHECK(H.have_eui64 && !memcmp(H.eui64, k_eui, 8) && H.up_step > ZH_HOST_UP_EUI64, "T35 5 s 后重读到了");
    }
    fresh();
    s_hold_mtorr = true;
    CHECK(to_running_up() && H.up_step == ZH_HOST_UP_MTORR, "T35 MTORR 在途");
    {
        const cmd_t *c = last_cmd();
        ncp_data(c->e, c->n);                        /* 原样回显：非幂等命令不自动重发，结果 UNKNOWN */
        pump();
        CHECK(H.st.up_bad[ZH_HOST_UP_MTORR] == 1 && !H.mtorr && H.st.resets == 1 && H.fails == 0, "T35 回显（UNKNOWN）：记一笔、不复位");
        s_hold_mtorr = false;
        advance(ZH_HOST_UP_RETRY_MS);
        CHECK(H.mtorr && s_by_id[ZH_EZSP_ID_SET_SOURCE_ROUTE_DISCOVERY_MODE] == 2, "T35 5 s 后重发一次、成了（多一次 MTORR 无害）");
    }
    fresh();
    CHECK(to_running_up() && H.have_eui64, "T35 EUI64 读到了");
    s_hold_eui = true;
    ncp_stack_status(ZH_SL_NETWORK_DOWN);
    pump();
    ncp_stack_status(ZH_SL_NETWORK_UP);
    pump();
    advance(10);
    CHECK(H.up_step == ZH_HOST_UP_EUI64, "T35 同一次启动里第二次 UP：getEui64 在途");
    {
        const cmd_t *c = last_cmd();
        const uint8_t shortp[7] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD, 0xBE };   /* 少一个字节、CRC 仍对 */
        ncp_rsp(c->e[0], ZH_EZSP_ID_GET_EUI64, shortp, sizeof(shortp));
        pump();
        CHECK(H.st.up_bad[ZH_HOST_UP_EUI64] == 1 && H.have_eui64 && !memcmp(H.eui64, k_eui, 8), "T35 短一个字节：不盖掉上一次读到的 EUI64");
    }

    /* ---- T36：入网窗口开（设计 8.6；SDK 框架 af-common.c:1003-1012、1328-1342）：策略换成"放新设备" → permitJoining(s) → 向 0xFFFC 广播
       Mgmt_Permit_Joining_req [序号, s, TC significance 1]，逐字节对手写的表；开着时再开不重开、不续时 ---- */
    fresh();
    CHECK(to_running_up(), "T36 起来");
    advance(10);
    {
        const int n0 = s_ncmd;
        CHECK(zh_host_permit_join(&H, 120, T) == ZH_PJ_OK && H.win_state == ZH_WIN_OPENING, "T36 开 120 s：受理");
        pump();
        advance(5);
        static const want_cmd_t k_open[] = {
            { ZH_EZSP_ID_SEC_MAN_IMPORT_TRANSIENT_KEY, 24, { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 通配 EUI64 */
                                                             'Z', 'i', 'g', 'B', 'e', 'e', 'A', 'l', 'l', 'i', 'a', 'n', 'c', 'e', '0', '9' } },   /* 公开密钥（SDK util.h:66-70） */
            { ZH_EZSP_ID_SET_POLICY, 2, { 0x00, 0x03 } },                                       /* 信任中心：ALLOW_JOINS | ALLOW_UNSECURED_REJOINS */
            { ZH_EZSP_ID_PERMIT_JOINING, 1, { 120 } },
            { ZH_EZSP_ID_SEND_BROADCAST, 23, { 0xFF, 0xFF, 0xFC, 0xFF, 0x00,                     /* alias 无、0xFFFC、nwk 序号 0 */
                                               0x00, 0x00, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* ZDO 0x0036、端点 0→0 */
                                               0xFF, 0x01, 0x00, 0x03, 0x01, 120, 0x01 } },        /* 半径 255、tag 1、3 字节：[1, 120, 1] */
        };
        int k = 0, bad = 0;
        for (int i = n0; i < s_ncmd && k < 4; i++) {
            const cmd_t *c = &s_cmd[i % 64];
            if (cmd_id(c) == ZH_EZSP_ID_NOP || cmd_id(c) == ZH_EZSP_ID_CALLBACK) continue;
            if (!same_cmd_as(c, &k_open[k])) { bad++; printf("   T36 第 %d 条不对：0x%04X %u B\n", k, cmd_id(c), (unsigned)c->n); }
            k++;
        }
        CHECK(k == 4 && bad == 0, "T36 四条命令的顺序与字节都对：先导临时链路密钥（2026-10-23 小米抓包）（对上 %d 条、错 %d 条）", k, bad);
        CHECK(H.win_state == ZH_WIN_OPEN && zh_host_window_left_s(&H, T) == 120 && H.st.win_opens == 1, "T36 开了，剩 %u s",
              (unsigned)zh_host_window_left_s(&H, T));
        const int n1 = s_ncmd;
        CHECK(zh_host_permit_join(&H, 60, T) == ZH_PJ_ALREADY_OPEN && H.win_state == ZH_WIN_OPEN, "T36 开着再开：ALREADY_OPEN");
        advance(100);
        CHECK(s_by_id[ZH_EZSP_ID_PERMIT_JOINING] == 1 && s_ncmd - n1 <= 1 && zh_host_window_left_s(&H, T) <= 120, "T36 不重开、不续时");

        /* ---- T37：到期关：permitJoining(0) → 广播 0 → 策略写回窗口外 → 清临时链路密钥（SDK 关窗口那几步，network-creator-security.c:326-329） ---- */
        const int n2 = s_ncmd;
        const uint32_t t_end = H.t_win_end;
        for (int g = 0; g < 130000 && H.win_state == ZH_WIN_OPEN; g++) advance(1);
        CHECK(H.st.win_expired == 1 && (int32_t)(T - t_end) >= 0 && T - t_end <= 1, "T37 到期那一刻开始关（%d ms）", (int)(T - t_end));
        advance(10);
        static const want_cmd_t k_close[] = {
            { ZH_EZSP_ID_PERMIT_JOINING, 1, { 0 } },
            { ZH_EZSP_ID_SEND_BROADCAST, 23, { 0xFF, 0xFF, 0xFC, 0xFF, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                               0xFF, 0x02, 0x00, 0x03, 0x02, 0x00, 0x01 } },        /* tag 2、[2, 0, 1] */
            { ZH_EZSP_ID_SET_POLICY, 2, { 0x00, 0x02 } },                                       /* 写回 ALLOW_UNSECURED_REJOINS */
            { ZH_EZSP_ID_CLEAR_TRANSIENT_LINK_KEYS, 0, { 0 } },
        };
        k = 0; bad = 0;
        for (int i = n2; i < s_ncmd && k < 4; i++) {
            const cmd_t *c = &s_cmd[i % 64];
            if (cmd_id(c) == ZH_EZSP_ID_NOP || cmd_id(c) == ZH_EZSP_ID_CALLBACK) continue;
            if (!same_cmd_as(c, &k_close[k])) { bad++; printf("   T37 第 %d 条不对：0x%04X %u B\n", k, cmd_id(c), (unsigned)c->n); }
            k++;
        }
        CHECK(k == 4 && bad == 0, "T37 关窗四条命令的顺序与字节都对（对上 %d 条、错 %d 条）", k, bad);
        CHECK(H.win_state == ZH_WIN_CLOSED && H.st.win_closes == 1 && H.st.win_skipped == 0 && H.fails == 0 && H.st.resets == 1, "T37 关完、没复位链路");
    }

    /* ---- T38：主动关；关着时关；开的路上要关（开完马上关）；关的路上要开（BUSY） ---- */
    fresh();
    CHECK(to_running_up(), "T38 起来");
    advance(10);
    CHECK(zh_host_permit_join(&H, 0, T) == ZH_PJ_ALREADY_CLOSED, "T38 关着时关：ALREADY_CLOSED");
    zh_host_permit_join(&H, 60, T);
    advance(10);
    CHECK(H.win_state == ZH_WIN_OPEN, "T38 开了");
    CHECK(zh_host_permit_join(&H, 0, T) == ZH_PJ_OK && H.win_state == ZH_WIN_CLOSING, "T38 主动关：受理、进 CLOSING");
    CHECK(zh_host_permit_join(&H, 60, T) == ZH_PJ_BUSY, "T38 关的路上要开：BUSY");
    advance(10);
    CHECK(H.win_state == ZH_WIN_CLOSED && H.st.win_closes == 1 && H.st.win_expired == 0, "T38 关完（不是到期关的）");
    s_hold_pj = true;
    zh_host_permit_join(&H, 60, T);
    advance(10);
    CHECK(H.win_state == ZH_WIN_OPENING && H.win_step == 2, "T38 开到 permitJoining 在途（第 0 步导密钥、第 1 步策略）");
    CHECK(zh_host_permit_join(&H, 0, T) == ZH_PJ_OK && H.win_close_pending, "T38 开的路上要关：记下");
    CHECK(zh_host_permit_join(&H, 60, T) == ZH_PJ_BUSY, "T38 记下要关之后再要开：BUSY");
    s_hold_pj = false;
    {
        const cmd_t *c = last_cmd();
        ncp_rsp_status(c->e[0], ZH_EZSP_ID_PERMIT_JOINING, ZH_SL_OK);
        pump();
    }
    advance(20);
    CHECK(H.win_state == ZH_WIN_CLOSED && H.st.win_closes == 2 && H.st.win_opens == 2, "T38 开完那一步马上关（NCP 那边开过：算开过，事件环里有\"开了\"）");

    /* ---- T39：开的某一步被拒（permitJoining 回 INVALID_STATE）：走关窗把策略写回，记一次失败，带步号 ---- */
    fresh();
    CHECK(to_running_up(), "T39 起来");
    advance(10);
    s_pj_status = 0x0002;
    {
        const int pol0 = s_by_id[ZH_EZSP_ID_SET_POLICY];
        zh_host_permit_join(&H, 60, T);
        advance(20);
        CHECK(H.st.win_open_fails == 1 && H.win_state == ZH_WIN_CLOSED && H.st.win_opens == 0 && (H.win_last_err >> 24) == 2 &&
              (H.win_last_err & 0xFFFF) == 0x0002, "T39 失败记在 permitJoining 那一步（0x%08X）、关回去", (unsigned)H.win_last_err);
        CHECK(s_by_id[ZH_EZSP_ID_SET_POLICY] == pol0 + 2 && H.st.win_closes == 1 && H.fails == 0, "T39 策略开过又写回（setPolicy 两次）、不复位链路");
    }
    /* T39b：导临时链路密钥被拒（第 0 步）：策略一次都没换成"放新设备"、NCP 窗口没开；关窗序列照走（写回策略、清密钥），记在第 0 步 */
    s_pj_status = ZH_SL_OK;
    s_key_status = 0x0002;
    {
        const int pol0 = s_by_id[ZH_EZSP_ID_SET_POLICY], pj0 = s_by_id[ZH_EZSP_ID_PERMIT_JOINING], f0 = (int)H.st.win_open_fails;
        const int cl0 = s_by_id[ZH_EZSP_ID_CLEAR_TRANSIENT_LINK_KEYS];
        CHECK(zh_host_permit_join(&H, 60, T) == ZH_PJ_OK, "T39b 受理");
        advance(30);
        CHECK((int)H.st.win_open_fails == f0 + 1 && H.win_state == ZH_WIN_CLOSED && (H.win_last_err >> 24) == 0 && (H.win_last_err & 0xFFFF) == 0x0002,
              "T39b 密钥被拒：开失败记在第 0 步（0x%08X）", (unsigned)H.win_last_err);
        CHECK(s_by_id[ZH_EZSP_ID_SET_POLICY] == pol0 + 1 && s_by_id[ZH_EZSP_ID_PERMIT_JOINING] == pj0 + 1 &&
              s_by_id[ZH_EZSP_ID_CLEAR_TRANSIENT_LINK_KEYS] == cl0 + 1 && H.fails == 0,
              "T39b 只有关窗的写回策略、permitJoining(0)、清密钥各一条（没开过）");
    }
    s_key_status = ZH_SL_OK;

    /* ---- T40：关窗时写回策略被拒：5 s 后重试同一步，成了才算关完；permitJoining(0) 被拒：跳过、照样往下 ---- */
    fresh();
    CHECK(to_running_up(), "T40 起来");
    advance(10);
    zh_host_permit_join(&H, 60, T);
    advance(10);
    s_pol_status = 0x0021;
    s_pj_status = 0x0002;
    zh_host_permit_join(&H, 0, T);
    advance(20);
    CHECK(H.win_state == ZH_WIN_CLOSING && H.win_step == 2 && H.st.win_close_retries == 1 && H.st.win_skipped == 1 && H.win_retry,
          "T40 permitJoining(0) 被拒跳过、写回策略被拒排重试");
    advance(ZH_HOST_WIN_RETRY_MS);
    CHECK(H.st.win_close_retries == 2 && H.win_state == ZH_WIN_CLOSING, "T40 5 s 后重试（又被拒）");
    s_pol_status = ZH_SL_OK;
    advance(ZH_HOST_WIN_RETRY_MS + 10);
    CHECK(H.win_state == ZH_WIN_CLOSED && H.st.win_closes == 1 && H.fails == 0, "T40 这次成了、关完；全程不复位链路");

    /* ---- T41：窗口开着时链路复位：窗口当场回 CLOSED（NCP 重启后 permitJoining 没了），新一轮的 k_init 又把信任中心写成窗口外；
       窗口开着时网络掉了：走关窗 ---- */
    fresh();
    CHECK(to_running_up(), "T41 起来");
    advance(10);
    zh_host_permit_join(&H, 120, T);
    advance(10);
    CHECK(H.win_state == ZH_WIN_OPEN, "T41 开着");
    zh_host_restart(&H, T);
    CHECK(H.win_state == ZH_WIN_CLOSED && !H.win_want, "T41 链路复位：窗口回 CLOSED");
    {
        ncp_rstack(ZH_ASH_RESET_SOFTWARE);
        pump();
        const cmd_t *v = last_cmd();
        const uint8_t vr[] = { v->e[0], 0x80, 0x00, 0x12, 0x02, 0x34, 0x12 };
        ncp_data(vr, sizeof(vr));
        pump();
        CHECK(answer_init_upto(3), "T41 新一轮的初始化序列照常，第三条把信任中心写回窗口外（0x02）");
    }
    fresh();
    CHECK(to_running_up(), "T41 再起来");
    advance(10);
    zh_host_permit_join(&H, 120, T);
    advance(10);
    ncp_stack_status(ZH_SL_NETWORK_DOWN);
    pump();
    advance(20);
    CHECK(H.win_state == ZH_WIN_CLOSED && H.st.win_closes == 1 && H.st.win_expired == 0, "T41 网络掉了：走关窗、关完");

    /* ---- T42：不在 RUNNING / UP 不开；超过 254 s 拒（255 = 永久开放） ---- */
    fresh();
    CHECK(zh_host_permit_join(&H, 60, T) == ZH_PJ_NOT_READY && zh_host_permit_join(&H, 0, T) == ZH_PJ_ALREADY_CLOSED, "T42 没起来：NOT_READY；关：ALREADY_CLOSED");
    CHECK(zh_host_permit_join(NULL, 60, T) == ZH_PJ_NOT_READY && zh_host_permit_join(&H, 255, T) == ZH_PJ_INVALID, "T42 NULL / 255");
    seq = to_net_init();
    ncp_rsp_status((uint8_t)seq, ZH_EZSP_ID_NETWORK_INIT, ZH_SL_NOT_JOINED);
    pump();
    CHECK(H.state == ZH_HOST_RUNNING && zh_host_permit_join(&H, 60, T) == ZH_PJ_NOT_READY, "T42 没有网络（NONE）：NOT_READY");
    {
        const uint8_t on = 1;
        (void)on;
        ncp_stack_status(ZH_SL_NETWORK_OPENED);
        pump();
        CHECK(H.ncp_open, "T42 NCP 报 NETWORK_OPENED：快照里记下");
        ncp_stack_status(ZH_SL_NETWORK_CLOSED);
        pump();
        CHECK(!H.ncp_open, "T42 NETWORK_CLOSED：清掉");
    }

    /* ---- T43：开的第一步（setPolicy 放新设备）在途时网络掉了：转关窗；那条迟到的 OK 不许被当成关窗的第 0 步（步号相同），
       关窗的 permitJoining(0) / 广播 / 写回策略 / 清密钥一条不少（变异"结果不核开 / 关序列"曾存活） ---- */
    fresh();
    CHECK(to_running_up(), "T43 起来");
    advance(10);
    s_hold_pol = true;
    zh_host_permit_join(&H, 60, T);
    advance(5);
    {
        const cmd_t *c = last_cmd();
        CHECK(c && cmd_id(c) == ZH_EZSP_ID_SET_POLICY && H.win_state == ZH_WIN_OPENING && H.win_step == 1, "T43 开窗的 setPolicy 在途（第 1 步，和关窗的第 1 步同号）");
        const uint8_t pseq = c->e[0];
        const int pj0 = s_by_id[ZH_EZSP_ID_PERMIT_JOINING], cl0 = s_by_id[ZH_EZSP_ID_CLEAR_TRANSIENT_LINK_KEYS];
        ncp_stack_status(ZH_SL_NETWORK_DOWN);
        pump();
        CHECK(H.win_state == ZH_WIN_CLOSING && H.win_step == 0, "T43 网络掉了：转关窗、从第 0 步开始");
        s_hold_pol = false;
        ncp_rsp_status(pseq, ZH_EZSP_ID_SET_POLICY, ZH_SL_OK);   /* 开窗那条迟到的 OK */
        pump();
        advance(30);
        CHECK(H.win_state == ZH_WIN_CLOSED && s_by_id[ZH_EZSP_ID_PERMIT_JOINING] == pj0 + 1 && s_by_id[ZH_EZSP_ID_CLEAR_TRANSIENT_LINK_KEYS] == cl0 + 1,
              "T43 关窗四条都发了（permitJoining(0) %d 条、清密钥 %d 条）", s_by_id[ZH_EZSP_ID_PERMIT_JOINING] - pj0,
              s_by_id[ZH_EZSP_ID_CLEAR_TRANSIENT_LINK_KEYS] - cl0);
    }

    /* ---- T44：UP 之后的序列（读参数等）还在走就要开窗：窗口命令等 want 槽空了才交、两边都走完；
       开窗的广播被原样回显（sendBroadcast 不幂等，会话不重发，回 UNKNOWN）：开失败、走关窗复原 ---- */
    fresh();
    s_hold_eui = true;
    CHECK(to_running_up() && H.up_step == ZH_HOST_UP_EUI64, "T44 UP 之后的序列停在 getEui64");
    CHECK(zh_host_permit_join(&H, 60, T) == ZH_PJ_OK, "T44 这时要开窗：受理");
    advance(20);
    CHECK(H.win_state == ZH_WIN_OPENING && H.win_step == 0 && s_by_id[ZH_EZSP_ID_SET_POLICY] == 7, "T44 want 槽占着：窗口的第一条还没交");
    s_hold_eui = false;
    {
        const cmd_t *c = last_cmd();
        ncp_rsp(c->e[0], ZH_EZSP_ID_GET_EUI64, k_eui, 8);
        pump();
    }
    advance(30);
    CHECK(H.up_step == ZH_HOST_UP_N && H.concentrator && H.win_state == ZH_WIN_OPEN, "T44 两边都走完：集中器设好、窗口开了");
    fresh();
    CHECK(to_running_up(), "T44 再起来");
    advance(10);
    s_auto_win = false;                              /* 自己回：广播那条原样回显 */
    zh_host_permit_join(&H, 60, T);
    pump();
    {
        const cmd_t *c = last_cmd();
        ncp_rsp_status(c->e[0], ZH_EZSP_ID_SEC_MAN_IMPORT_TRANSIENT_KEY, ZH_SL_OK);
        pump();
        c = last_cmd();
        ncp_rsp_status(c->e[0], ZH_EZSP_ID_SET_POLICY, ZH_SL_OK);
        pump();
        c = last_cmd();
        ncp_rsp_status(c->e[0], ZH_EZSP_ID_PERMIT_JOINING, ZH_SL_OK);
        pump();
        c = last_cmd();
        CHECK(cmd_id(c) == ZH_EZSP_ID_SEND_BROADCAST, "T44 到了广播那一步");
        s_auto_win = true;                           /* 开失败之后关窗那几条照常自动回 */
        ncp_data(c->e, c->n);                        /* 原样回显 */
        pump();
    }
    advance(30);
    CHECK(H.st.win_open_fails == 1 && (H.win_last_err >> 24) == 3 && ((H.win_last_err >> 16) & 0xFF) == ZH_SES_R_UNKNOWN && H.win_state == ZH_WIN_CLOSED &&
          s_by_id[ZH_EZSP_ID_SEND_BROADCAST] == 2, "T44 广播回显：开失败（步号 2、UNKNOWN）、不重发广播、关窗复原（关窗那条广播照发）");

    /* ---- T45：关窗时写回信任中心策略连着被拒 ZH_HOST_WIN_POLICY_TRIES 次：不再无限重试，复位链路（NCP 重启，k_init 把策略写回窗口外） ---- */
    fresh();
    CHECK(to_running_up(), "T45 起来");
    advance(10);
    zh_host_permit_join(&H, 60, T);
    advance(10);
    s_pol_status = 0x0021;
    s_rst = 0;
    zh_host_permit_join(&H, 0, T);
    for (int g = 0; g < 60000 && H.st.fails[ZH_HOST_FAIL_WIN_POLICY] == 0; g++) advance(1);
    CHECK(H.st.fails[ZH_HOST_FAIL_WIN_POLICY] == 1 && H.st.win_close_retries == ZH_HOST_WIN_POLICY_TRIES && H.last_fail_code == 0x0021 && s_rst == 1 &&
          H.win_state == ZH_WIN_CLOSED, "T45 被拒 %u 次后复位链路（记 WIN_POLICY、code 0x%X）、窗口回 CLOSED", (unsigned)H.st.win_close_retries,
          (unsigned)H.last_fail_code);

    /* ---- T46：入网窗口总时长上限（滚动 60 min 最多 ZH_HOST_PERMIT_HOUR_CAP_S；用户 2026-10-22 定按默认做） ---- */
    fresh();
    CHECK(to_running_up(), "T46 起来");
    advance(10);
    {
        const uint32_t t0 = T;
        CHECK(zh_host_quota_left_s(&H, T) == ZH_HOST_PERMIT_HOUR_CAP_S && zh_host_quota_wait_s(&H, T) == 0, "T46 一开始满额 %u s", (unsigned)zh_host_quota_left_s(&H, T));
        for (int k = 0; k < 2; k++) {
            CHECK(zh_host_permit_join(&H, 254, T) == ZH_PJ_OK && H.win_s == 254, "T46 第 %d 次开 254 s", k + 1);
            for (int g = 0; g < 300000 && H.win_state != ZH_WIN_CLOSED; g++) advance(1);
            advance(61000);                            /* 让 REST 那一层的 60 s 限速与这里无关：这里直接调编排层 */
        }
        CHECK(zh_host_quota_left_s(&H, T) == ZH_HOST_PERMIT_HOUR_CAP_S - 508, "T46 用了 508 s，剩 %u s", (unsigned)zh_host_quota_left_s(&H, T));
        CHECK(zh_host_permit_join(&H, 120, T) == ZH_PJ_OK && H.win_s == 92, "T46 第三次要 120 s：截到剩的 92 s（%u）", (unsigned)H.win_s);
        for (int g = 0; g < 200000 && H.win_state != ZH_WIN_CLOSED; g++) advance(1);
        CHECK(zh_host_quota_left_s(&H, T) == 0 && zh_host_permit_join(&H, 60, T) == ZH_PJ_QUOTA && H.win_state == ZH_WIN_CLOSED, "T46 用满：再开回 QUOTA、不开");
        const uint32_t w = zh_host_quota_wait_s(&H, T);
        const uint32_t expect = (ZH_HOST_PERMIT_HOUR_MS - (T - t0)) / 1000u;   /* 第一条记录从 t0 起算，一小时后过期 */
        CHECK(w >= expect && w <= expect + 2, "T46 要等 %u s（约 %u s：第一次开窗满一小时）", (unsigned)w, (unsigned)expect);
        advance(w * 1000u + 10);
        CHECK(zh_host_quota_left_s(&H, T) >= 254 && zh_host_permit_join(&H, 120, T) == ZH_PJ_OK && H.win_s == 120, "T46 第一条过期：又能开满 120 s");
        advance(5000);
        zh_host_permit_join(&H, 0, T);
        advance(50);
    }
    fresh();
    CHECK(to_running_up(), "T46 再起来");
    advance(10);
    zh_host_permit_join(&H, 254, T);
    advance(10000);
    zh_host_permit_join(&H, 0, T);
    advance(50);
    CHECK(zh_host_quota_left_s(&H, T) >= ZH_HOST_PERMIT_HOUR_CAP_S - 11 && zh_host_quota_left_s(&H, T) <= ZH_HOST_PERMIT_HOUR_CAP_S - 10,
          "T46 开 254 s 第 10 s 主动关：只算 10 s 左右（剩 %u s）", (unsigned)zh_host_quota_left_s(&H, T));
    s_pj_status = 0x0002;
    const uint32_t q0 = zh_host_quota_left_s(&H, T);
    zh_host_permit_join(&H, 120, T);
    advance(50);
    CHECK(H.st.win_open_fails == 1 && zh_host_quota_left_s(&H, T) + 1 >= q0, "T46 开失败：几乎不占额度（%u → %u）", (unsigned)q0, (unsigned)zh_host_quota_left_s(&H, T));
    s_pj_status = ZH_SL_OK;

    /* T46b：一小时里开满 ZH_HOST_WIN_HIST 次（每次开了马上关，时长几乎为 0）：第 25 次回 QUOTA，不许把一小时内的记录盖掉（审计 ❌：
       盖掉大记录后一小时能开到约 858 s）；要等的是最旧那一条满一小时 */
    fresh();
    CHECK(to_running_up(), "T46b 起来");
    advance(10);
    {
        const uint32_t t0 = T;
        int opened = 0;
        for (unsigned k = 0; k < ZH_HOST_WIN_HIST; k++) {
            if (zh_host_permit_join(&H, 30, T) == ZH_PJ_OK) opened++;
            advance(20);
            zh_host_permit_join(&H, 0, T);
            advance(1000);
        }
        CHECK(opened == (int)ZH_HOST_WIN_HIST && zh_host_quota_left_s(&H, T) >= ZH_HOST_PERMIT_HOUR_CAP_S - 2 * ZH_HOST_WIN_HIST, "T46b 开关 %d 次、额度几乎没动（剩 %u s）",
              opened, (unsigned)zh_host_quota_left_s(&H, T));
        CHECK(zh_host_permit_join(&H, 254, T) == ZH_PJ_QUOTA, "T46b 第 %u 次：环满、回 QUOTA", (unsigned)ZH_HOST_WIN_HIST + 1);
        const uint32_t w = zh_host_quota_wait_s(&H, T), expect = (ZH_HOST_PERMIT_HOUR_MS - (T - t0)) / 1000u;
        CHECK(w >= expect && w <= expect + 2, "T46b 要等最旧那条满一小时（%u s，约 %u s）", (unsigned)w, (unsigned)expect);
    }

    /* T46c：32 位毫秒约 49.7 天回绕：作废过的老记录不许"返老还童"（审计 ❌）。直接拨时钟：满一小时后作废，再往后跳 2^32 ms，额度应是满的 */
    fresh();
    CHECK(to_running_up(), "T46c 起来");
    advance(10);
    zh_host_permit_join(&H, 254, T);
    for (int g = 0; g < 300000 && H.win_state != ZH_WIN_CLOSED; g++) advance(1);
    {
        const uint32_t t1 = T;
        CHECK(zh_host_quota_left_s(&H, t1) == ZH_HOST_PERMIT_HOUR_CAP_S - 254, "T46c 用了 254 s");
        zh_host_expire_records(&H, t1 + ZH_HOST_PERMIT_HOUR_MS + 1000u);    /* zhub 任务每圈都调：满一小时的那一刻作废 */
        const uint32_t wrapped = t1 + 1000u;                                 /* = t1 + 2^32 + 1000 ms（uint32 回绕后与 t1 相差 1 s） */
        CHECK(zh_host_quota_left_s(&H, wrapped) == ZH_HOST_PERMIT_HOUR_CAP_S, "T46c 回绕之后：作废过的记录不再算（剩 %u s）", (unsigned)zh_host_quota_left_s(&H, wrapped));
    }

    /* ---- T48：k_init 的可选项被拒（第七步 b 第二轮审计）：MAC 间接发送超时被拒 → 记一笔、接着走到 networkInit；必需项被拒照旧复位 ---- */
    fresh();
    {
        CHECK(to_ready(), "T48 握手完成");
        CHECK(answer_init_upto(2), "T48 前两项回 OK");
        const cmd_t *c = last_cmd();
        CHECK(c && same_cmd_as(c, &k_want_init[2]), "T48 第三项是 MAC 间接发送超时");
        ncp_rsp_status(c->e[0], ZH_EZSP_ID_SET_CONFIGURATION_VALUE, 0x0021);   /* SL_STATUS_INVALID_PARAMETER */
        pump();
        CHECK(H.st.init_opt_rejects == 1 && H.st.init_opt_last == (2u << 8 | 0x21u) && H.state != ZH_HOST_BACKOFF && H.st.resets == 1,
              "T48 可选项被拒：记一笔（步号 2、状态 0x21），不复位");
        const cmd_t *n = last_cmd();
        CHECK(n && same_cmd_as(n, &k_want_init[3]), "T48 接着发下一项（信任中心策略）");
    }
    fresh();
    {
        CHECK(to_ready() && answer_init_upto(1), "T48b 第一项回 OK");
        const cmd_t *c = last_cmd();
        ncp_rsp_status(c->e[0], ZH_EZSP_ID_SET_CONFIGURATION_VALUE, 0x0021);   /* SECURITY_LEVEL 被拒：必需项 */
        pump();
        advance(10);
        CHECK(H.st.init_opt_rejects == 0 && H.last_fail == ZH_HOST_FAIL_NET_INIT, "T48b 必需项被拒：照旧当 networkInit 失败复位");
    }

    /* ---- T47：应用命令队列（设备层用；M4 第七步 a） ---- */
    fresh();
    {
        uint8_t f[8];
        const size_t fn = zh_ezsp_enc_lookup_eui64(f, sizeof(f), 0, 0xCAF5);
        CHECK(!zh_host_app_submit(&H, f, fn, 0x123, true), "T47 不在 RUNNING：拒");
        s_hold_eui = true;
        CHECK(to_running_up() && H.up_step == ZH_HOST_UP_EUI64, "T47 起来，UP 之后的序列停在 getEui64（want 槽占着）");
        CHECK(zh_host_app_submit(&H, f, fn, 0x123, true) && zh_host_app_submit(&H, f, fn, 0xABCDEF, true) && H.app_n == 2, "T47 收下两条");
        CHECK(!zh_host_app_submit(&H, f, fn, ZH_HOST_APP_TAG_MAX + 1u, true) && !zh_host_app_submit(&H, f, 3, 1, true), "T47 tag 超 24 位 / 帧太短：拒");
        advance(20);
        CHECK(H.app_n == 2 && s_by_id[ZH_EZSP_ID_LOOKUP_EUI64_BY_NODE_ID] == 0, "T47 want 槽占着：应用命令不交");
        s_hold_eui = false;
        const cmd_t *c = last_cmd();
        ncp_rsp(c->e[0], ZH_EZSP_ID_GET_EUI64, k_eui, 8);
        pump();
        advance(20);                               /* 集中器 / MTORR 自动回，走完了才轮到应用命令 */
        CHECK(H.up_step == ZH_HOST_UP_N && s_by_id[ZH_EZSP_ID_LOOKUP_EUI64_BY_NODE_ID] == 1 && H.app_n == 1, "T47 UP 之后的序列走完才交第一条");
        c = last_cmd();
        CHECK(cmd_id(c) == ZH_EZSP_ID_LOOKUP_EUI64_BY_NODE_ID && c->e[5] == 0xF5 && c->e[6] == 0xCA, "T47 交的就是收下的那一帧");
        uint8_t p[12] = { 0 };
        memcpy(p + 4, k_eui, 8);
        ncp_rsp(c->e[0], ZH_EZSP_ID_LOOKUP_EUI64_BY_NODE_ID, p, 12);
        pump();
        advance(5);
        CHECK(s_app_n >= 1 && s_app_tag[0] == 0x123 && s_app_r[0] == ZH_SES_R_OK, "T47 结果带原 tag 0x123 回给 on_app_result");
        c = last_cmd();
        ncp_rsp(c->e[0], ZH_EZSP_ID_LOOKUP_EUI64_BY_NODE_ID, p, 12);
        pump();
        CHECK(s_app_n == 2 && s_app_tag[1] == 0xABCDEF, "T47 第二条：24 位 tag 原样回");
        for (unsigned k = 0; k < ZH_HOST_APP_Q; k++) zh_host_app_submit(&H, f, fn, k, true);
        CHECK(!zh_host_app_submit(&H, f, fn, 99, true) && H.st.app_full >= 3, "T47 队列满：拒");
        zh_host_restart(&H, T);
        CHECK(H.app_n == 0, "T47 链路复位：排队的丢掉（不回结果，调用方自己有期限）");
    }
    /* T47b：变异测试存活的几处补的 */
    fresh();
    {
        uint8_t f[8], bad[8];
        const size_t fn = zh_ezsp_enc_lookup_eui64(f, sizeof(f), 0, 0xCAF5);
        CHECK(to_running_up() && H.up_step == ZH_HOST_UP_N, "T47b 起来、UP 之后的序列走完");
        advance(5);
        const int a0 = s_app_n;
        CHECK(zh_host_app_submit(&H, f, fn, 0x55, false) && zh_host_next_timeout_ms(&H, T) == 0, "T47b 队列里有、会话能收：下次超时 0（马上来交）");
        zh_host_tick(&H, T);
        const zh_ses_cmd_t *sc = (H.ses.cur.used && !H.ses.cur.internal) ? &H.ses.cur : &H.ses.pend;
        CHECK(sc->used && sc->tag == (7u | (0x55u << 8)) && !sc->idempotent, "T47b 交给会话的那条：tag 编进 TAG_APP、不幂等的标志原样带过去");
        {
            uint8_t u[48];
            const zh_ezsp_aps_t ua = { .profile_id = 0, .cluster_id = 1, .options = 0x40 };
            const uint8_t up[1] = { 0 };
            const size_t un = zh_ezsp_enc_send_unicast(u, sizeof(u), 0, 0, 0x1234, &ua, 1, sizeof(up), up);
            const bool queued = un && zh_host_app_submit(&H, u, un, 0x56, true);
            CHECK(queued && !H.app[(H.app_head + H.app_n - 1) % ZH_HOST_APP_Q].idem,
                  "T47b 调用方把 sendUnicast 说成幂等：不在会话层白名单里，按不幂等交（哪些能重发只有一处正本）");
            if (queued) H.app_n--;                 /* 只看入队时的标志：从队尾撤掉，不让它真发出去 */
        }
        pump();
        const cmd_t *c = last_cmd();
        ncp_rsp(c->e[0], ZH_EZSP_ID_LOOKUP_EUI64_BY_NODE_ID, (const uint8_t[12]){ 0 }, 12);
        pump();
        CHECK(s_app_n == a0 + 1 && s_app_tag[a0 % 16] == 0x55, "T47b 结果回来了");
        memcpy(bad, f, fn);
        bad[3] = (uint8_t)ZH_EZSP_ID_CALLBACK;                   /* 会话自己的帧 ID：会话永远不收 */
        bad[4] = (uint8_t)(ZH_EZSP_ID_CALLBACK >> 8);
        const int nl = s_by_id[ZH_EZSP_ID_LOOKUP_EUI64_BY_NODE_ID];
        CHECK(zh_host_app_submit(&H, bad, fn, 0x66, true) && zh_host_app_submit(&H, f, fn, 0x67, true), "T47b 收下一条会话不收的、一条正常的");
        advance(5);
        CHECK(s_app_n >= a0 + 2 && s_app_tag[(a0 + 1) % 16] == 0x66 && s_app_r[(a0 + 1) % 16] == ZH_SES_R_INVALID && H.st.app_bad == 1 &&
              s_by_id[ZH_EZSP_ID_LOOKUP_EUI64_BY_NODE_ID] == nl + 1, "T47b 会话不收的那条：丢掉回 INVALID，不堵后面那条");
        c = last_cmd();
        ncp_rsp(c->e[0], ZH_EZSP_ID_LOOKUP_EUI64_BY_NODE_ID, (const uint8_t[12]){ 0 }, 12);
        pump();
        /* 入网窗口的命令比应用命令先：同一拍里两样都有，先交 setPolicy */
        const int n0 = s_ncmd;
        CHECK(zh_host_app_submit(&H, f, fn, 0x68, true) && zh_host_permit_join(&H, 60, T) == ZH_PJ_OK, "T47b 同一拍里排一条应用命令、开窗口");
        advance(1);
        CHECK(s_ncmd > n0 && cmd_id(&s_cmd[n0 % 64]) == ZH_EZSP_ID_SEC_MAN_IMPORT_TRANSIENT_KEY, "T47b 窗口的第一步（导临时链路密钥）先交（交的第一条是 0x%04X）", (unsigned)cmd_id(&s_cmd[n0 % 64]));
        bool in_range = true, wrapped = false;
        uint8_t prev = zh_host_next_zdo_seq(&H);
        for (int k = 0; k < 300; k++) {
            const uint8_t q = zh_host_next_zdo_seq(&H);
            if (q > ZH_ZDO_APP_SEQ_MASK) in_range = false;
            if (prev == ZH_ZDO_APP_SEQ_MASK && q == 0) wrapped = true;
            prev = q;
        }
        CHECK(in_range && wrapped, "T47b ZDO 序号只在 0..%u 里转、到顶回 0", ZH_ZDO_APP_SEQ_MASK);
    }

    /* ---- T24：证据比的是计数：时间轴跨过 2^31 / 2^32 两个回绕点照样判得对。原 bug（第一轮审计 ❌1，zhub.c 比两个 32 位时间戳，
       持有约 24.8 天后变号）靠的是"比计数不比时间戳"这个结构，VERIFY 钉着 zhub.c 不许再出现那种比较；这里钉住计数接口本身 ---- */
    for (int w = 0; w < 2; w++) {
        fresh();
        T = w ? 0xFFFF0000u : 0x7FFF0000u;           /* 起点离回绕点约 65 s */
        const uint32_t mark = zh_host_evidence_mark(&H);
        CHECK(!zh_host_evidence_since(&H, mark, T, NULL), "T24 接管时还没有证据");
        CHECK(to_running_up(), "T24 起来（起点 0x%08X）", (unsigned)T);
        uint32_t worst = 0, missing = 0;
        for (int i = 0; i < 200; i++) {
            advance(1000);
            uint32_t age = 0;
            if (!zh_host_evidence_since(&H, mark, T, &age)) missing++;
            else if (age > worst) worst = age;
        }
        CHECK(missing == 0 && worst <= 11000, "T24 跨回绕点 200 s：一直有证据、最旧 %u ms（保活 10 s 一次）", (unsigned)worst);
        const uint32_t mark2 = zh_host_evidence_mark(&H);
        CHECK(!zh_host_evidence_since(&H, mark2, T, NULL), "T24 新的 mark 之后没有新应答：不算");
    }
    CHECK(s_inv_bad == 0, "不变量：退避 / 挂起时 ASH 与会话都是 DOWN（违反 %u 次）", (unsigned)s_inv_bad);

    printf("zh_host: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
