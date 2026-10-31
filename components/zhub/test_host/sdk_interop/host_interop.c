/* zh_host_core（编排层，连同 zh_ash_core / zh_ezsp_core / zh_ezsp_ses，产品源文件原样编）对接 SDK 的真 NCP 侧 ASH（ash-ncp.c，经 libsdkncp.so）
 * 和测试里的 EZSP 应用层替身 —— docs/15 设计 8.2、8.5 开头一段、8.13；CHECKLIST Z02 Z03 Z04 Z40 Z58 Z60 C3。
 *
 * 替身像 em260：应答原地写回命令缓冲（ncp_stub.c），同一块缓冲重复入队当硬错误（原地应答撞缓冲）。它模拟一颗"NVM3 里有网络"的 NCP：
 * 每次（重新）启动后协议栈是未初始化的，第一条 networkInit 回 OK、随后报 stackStatusHandler(NETWORK_UP)；同一次启动里再来 networkInit
 * 回 INVALID_STATE（真协议栈的行为）并记一次"重复执行"；这次启动里主机没写 STACK_PROFILE = 2 时 networkInit 回 NOT_JOINED（真 NCP 出厂
 * profile 0 —— 首次上板就栽在这儿）。按长度严格收命令，不认识的命令记下来（主机只许发 version / nop / callback / 初始化序列（两个协议参数 /
 * 7 条策略 / addEndpoint）/ networkInit / networkState / UP 之后的 getNetworkParameters / getEui64 / setConcentrator / setSourceRouteDiscoveryMode）。
 * addEndpoint 像真 NCP：网络起来之后回 EZSP_ERROR（INVALID_CALL，command-prototypes.h:113-122），同一次启动里同一个端点再加也回错（INVALID_VALUE）。
 *   H1 上电：RUNNING、网络 UP、读回信道 / PAN；NCP 第一条收到旧式头 version、networkInit 恰好一次
 *   H2 NCP 里没有网络（NOT_JOINED）：RUNNING / NONE，之后 3 分钟只有保活，NCP 没收到任何别的命令（绝不建网）
 *   H3 NCP 先死 40 s 再活：按退避表重试、退避期间主机一个字节都不发；活过来后恢复
 *   H4 应用层挂死、ASH 照样确认：保活超时 → 复位 → NCP 重启 → 重新 networkInit → RUNNING
 *   H5 运行中 NCP 自己重启：软件重启（RSTACK）、看门狗复位（ERROR 帧）两路，主机都发现 → 复位 → 恢复
 *   H6 挂起 30 s（UART 交给别人）：一个字节都不发；恢复后从头走一遍
 *   H7 同步回调（stackStatusHandler 走 pending 位 + callback 取）：照样到 UP
 *   H8 噪声信道（每字节 1%）+ 应用层随机不回 + 每 20 s NCP 自己重启一次，共 3 分钟：始终有结论、不悬着；每次 NCP 启动里 networkInit
 *      至多执行一次（Z59）；噪声停了恢复 RUNNING
 *   H9 2% 噪声下连做 15 次从头开始：每次都走到 UP，networkInit 每次启动至多执行一次
 *   H10 运行中网络掉了：不回来就期限后复位（新启动里 networkInit 一次）；期限内回来就不复位、集中器重设
 *   H11 入网窗口：开 30 s → NCP 的信任中心策略、permitJoining、ZDO 广播都对，到期 NCP 自己关、主机写回策略清临时密钥；噪声里开关 6 轮，
 *       每轮关完 NCP 的信任中心都是窗口外的取值；窗口开着时 NCP 重启：主机窗口回 CLOSED、新启动的信任中心是窗口外的取值
 *   每次 NCP 启动都核：7 条策略与端点 1 都在 networkInit 之前到、端点只加一次；集中器只在网络起来之后设（H1 / H8 / H9 全程）
 * 用法：host_interop <libsdkncp.so 的路径> [随机种子] */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zh_host_core.h"

static int s_fail, s_pass;
#define CHECK(cond, ...) do { if (cond) { s_pass++; } else { s_fail++; printf("❌ %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- libsdkncp.so（同 ses_interop.c） ---- */
static const char *s_path;
static void *s_so;
static void     (*ncp_boot)(uint8_t);
static void     (*ncp_set_time)(uint32_t);
static void     (*ncp_feed)(const uint8_t *, size_t);
static size_t   (*ncp_drain)(uint8_t *, size_t);
static int      (*ncp_poll)(void);
static size_t   (*ncp_take_cmd)(uint8_t *);
static void     (*ncp_send)(const uint8_t *, size_t, bool);
static void     (*ncp_reply)(const uint8_t *, size_t);
static void     (*ncp_set_inplace)(bool);
static uint32_t (*ncp_cmd_count)(void);

static void *sym(const char *n)
{
    void *p = dlsym(s_so, n);
    if (!p) { printf("❌ libsdkncp.so 里没有 %s\n", n); exit(1); }
    return p;
}
static void load(void)
{
    if (s_so) dlclose(s_so);
    s_so = dlopen(s_path, RTLD_NOW | RTLD_LOCAL);
    if (!s_so) { printf("❌ dlopen %s: %s\n", s_path, dlerror()); exit(1); }
    ncp_boot = sym("ncp_boot"); ncp_set_time = sym("ncp_set_time"); ncp_feed = sym("ncp_feed"); ncp_drain = sym("ncp_drain");
    ncp_poll = sym("ncp_poll"); ncp_take_cmd = sym("ncp_take_cmd"); ncp_send = sym("ncp_send"); ncp_reply = sym("ncp_reply");
    ncp_set_inplace = sym("ncp_set_inplace"); ncp_cmd_count = sym("ncp_cmd_count");
    if (ncp_cmd_count() != 0) { printf("❌ 重新加载后 NCP 的静态变量没清零\n"); exit(1); }
}

static uint32_t s_rng = 0x13579BDF;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

/* ---------------------------------------------------------------- 主机：zh_host */

static zh_host_t H;
static uint32_t  s_now;
static int       s_net_ev[4];
static void on_net(void *ctx, zh_host_net_t n) { (void)ctx; if (n < 4) s_net_ev[n]++; }

/* ---------------------------------------------------------------- NCP：SDK ash-ncp + EZSP 应用层替身 */

static double   s_noise, s_app_drop;
static bool     s_app_hung, s_ncp_dead, s_sync_cb, s_has_network = true;
static int      s_ncp_boots;
static uint32_t s_cmds_boot;                   /* 这次启动收到的命令数 */
static uint32_t s_first_legacy;                /* 有几次启动的第一条命令是旧式头 version */
static uint32_t s_ni_boot, s_ni_total, s_ni_dup;   /* networkInit：这次启动执行了几次 / 总共 / 同一次启动里重复执行 */
static uint32_t s_params, s_nops, s_unexpected, s_malformed, s_nstates;
static bool     s_net_up;                      /* 这次启动里协议栈的网络起来了（networkInit 成功之后）；H10 让它掉下去 */
static bool     s_profile2;                    /* 这次启动里主机写过 STACK_PROFILE = 2：真 NCP 出厂 profile 0，不写 networkInit 恒回 NOT_JOINED */
static uint32_t s_cfgs;
/* 第五步：初始化序列与 UP 之后的序列 */
static uint8_t  s_pol[10];                     /* 这次启动里各策略最后写成的取值（0xFF = 没写） */
static uint32_t s_pol_boot;                    /* 这次启动写了几条策略 */
static uint32_t s_ep_boot, s_ep_dup, s_ep_late;    /* addEndpoint：这次启动成功的 / 重复加的 / 网络起来之后才来的 */
static uint32_t s_init_late;                   /* 策略 / 端点 / 协议参数来得比 networkInit 晚（这次启动里 networkInit 之后） */
static uint32_t s_init_short;                  /* networkInit 来的时候这次启动的策略不满 6 条或端点没加 */
static uint32_t s_conc_boot, s_conc_total, s_conc_early, s_conc_bad;   /* setConcentrator：这次启动 / 总共 / 网络没起来就来 / 参数不对 */
static uint32_t s_mtorr_total, s_mtorr_bad, s_eui_total;
/* 第六步：入网窗口。NCP 的 permitJoining 计时按模拟时间走，到期报 NETWORK_CLOSED（真 NCP 同样自己关） */
static uint32_t s_permit_until;                /* 0 = 关着 */
static uint32_t s_permit_cnt, s_permit_bad, s_bcast_cnt, s_bcast_bad, s_clear_cnt, s_key_cnt, s_key_bad;
static uint8_t  s_bcast_last[3];
static uint16_t s_cbq[64];
static int      s_cbq_h, s_cbq_t;
static uint32_t s_rx_host_bytes;               /* 主机发出的字节（判"退避 / 挂起期间不发"） */

static size_t channel(uint8_t *b, size_t n)
{
    if (s_noise <= 0) return n;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        const double r = (double)(rnd() % 1000000) / 1e6;
        if (r < s_noise / 2) continue;
        b[o] = b[i];
        if (r < s_noise) b[o] ^= (uint8_t)(1u << (rnd() % 8));
        o++;
    }
    return o;
}

static void push_stack_status(uint16_t st)
{
    if (s_sync_cb) { s_cbq[s_cbq_t++ % 64] = st; return; }
    const uint8_t f[] = { 0x00, 0x90, 0x01, (uint8_t)ZH_EZSP_ID_STACK_STATUS_HANDLER, 0x00, (uint8_t)st, (uint8_t)(st >> 8), 0, 0 };
    ncp_send(f, sizeof(f), false);
}

static void ncp_app_on_cmd(void)
{
    uint8_t c[256], r[64];
    const size_t n = ncp_take_cmd(c);
    if (n < 3) return;
    const bool legacy = (c[2] & 0x03) != 0x01;
    const uint16_t id = legacy ? c[2] : (n >= 5 ? (uint16_t)(c[3] | (c[4] << 8)) : 0xFFFF);
    if (s_cmds_boot++ == 0 && legacy && id == ZH_EZSP_ID_VERSION) s_first_legacy++;
    if (s_app_hung) return;
    bool good;
    if (legacy) good = id == ZH_EZSP_ID_VERSION && n == 4;
    else if (id == ZH_EZSP_ID_NOP || id == ZH_EZSP_ID_CALLBACK || id == ZH_EZSP_ID_GET_NETWORK_PARAMETERS || id == ZH_EZSP_ID_NETWORK_STATE) good = n == 5;
    else if (id == ZH_EZSP_ID_NETWORK_INIT) good = n == 7;
    else if (id == ZH_EZSP_ID_SET_CONFIGURATION_VALUE) good = n == 8 && (c[5] == ZH_EZSP_CFG_STACK_PROFILE || c[5] == ZH_EZSP_CFG_SECURITY_LEVEL ||
                                                            c[5] == ZH_EZSP_CFG_INDIRECT_TRANSMISSION_TIMEOUT);
    else if (id == ZH_EZSP_ID_SET_POLICY) good = n == 7 && c[5] < sizeof(s_pol);
    else if (id == ZH_EZSP_ID_ADD_ENDPOINT) good = n >= 13 && n == 13u + 2u * (c[11] + c[12]);
    else if (id == ZH_EZSP_ID_GET_EUI64) good = n == 5;
    else if (id == ZH_EZSP_ID_SET_CONCENTRATOR) good = n == 15;
    else if (id == ZH_EZSP_ID_SET_SOURCE_ROUTE_DISCOVERY_MODE) good = n == 6;
    else if (id == ZH_EZSP_ID_PERMIT_JOINING) good = n == 6;
    else if (id == ZH_EZSP_ID_SEND_BROADCAST) good = n >= 25 && n == 25u + c[24];   /* 头 5 + alias 2 + 目的 2 + nwk 序号 1 + APS 11 + 半径 1 + tag 2 + 长度 1 */
    else if (id == ZH_EZSP_ID_CLEAR_TRANSIENT_LINK_KEYS) good = n == 5;
    else if (id == ZH_EZSP_ID_SEC_MAN_IMPORT_TRANSIENT_KEY) good = n == 29;   /* 头 5 + EUI64 8 + 密钥 16 */
    else { s_unexpected++; good = false; }
    if (!good) {
        if (id == ZH_EZSP_ID_NOP || id == ZH_EZSP_ID_CALLBACK || id == ZH_EZSP_ID_GET_NETWORK_PARAMETERS || id == ZH_EZSP_ID_NETWORK_INIT ||
            id == ZH_EZSP_ID_NETWORK_STATE) s_malformed++;
        if (legacy || n < 5) return;
        const uint8_t inv[] = { c[0], 0x80, 0x01, (uint8_t)ZH_EZSP_ID_INVALID_COMMAND, 0x00, 0x36 };
        ncp_reply(inv, sizeof(inv));
        return;
    }
    /* 执行（计数在"回不回"之前：不回 = 执行了但应答没出来） */
    uint32_t ni_status = 0, st = ZH_SL_OK;
    /* networkInit 之后只许入网窗口改信任中心策略（ID 0）；别的策略、配置值、端点来了都算"初始化晚到" */
    if (((id == ZH_EZSP_ID_SET_POLICY && n == 7 && c[5] != 0x00) || id == ZH_EZSP_ID_ADD_ENDPOINT || id == ZH_EZSP_ID_SET_CONFIGURATION_VALUE) && s_ni_boot)
        s_init_late++;
    if (id == ZH_EZSP_ID_SET_POLICY) {
        s_pol[c[5]] = c[6];
        s_pol_boot++;
    }
    if (id == ZH_EZSP_ID_ADD_ENDPOINT) {
        if (s_net_up) { s_ep_late++; st = 0x0C1E; }                 /* SL_STATUS_ZIGBEE_EZSP_ERROR（INVALID_CALL） */
        else if (s_ep_boot) { s_ep_dup++; st = 0x0C1E; }            /* 同一个端点再加（INVALID_VALUE） */
        else s_ep_boot++;
    }
    if (id == ZH_EZSP_ID_GET_EUI64) s_eui_total++;
    bool opened = false;
    if (id == ZH_EZSP_ID_PERMIT_JOINING) {
        s_permit_cnt++;
        if (!s_net_up) { st = 0x0002; s_permit_bad++; }           /* 没有网络：INVALID_STATE */
        else if (c[5] == 0) s_permit_until = 0;
        else { s_permit_until = s_now + 1000u * c[5]; opened = true; }
    }
    if (id == ZH_EZSP_ID_SEND_BROADCAST) {
        s_bcast_cnt++;
        /* 参数从 c[5] 起：alias FFFF、目的 FFFC、nwk 序号、APS（profile 0、cluster 0x0036、端点 0→0、选项、组、序号）、半径 255、tag = ZDO 序号、
           长度 3、[ZDO 序号, 时长, TC significance 1] */
        const bool fmt = c[5] == 0xFF && c[6] == 0xFF && c[7] == 0xFC && c[8] == 0xFF && c[10] == 0 && c[11] == 0 && c[12] == 0x36 && c[13] == 0 &&
                         c[14] == 0 && c[15] == 0 && c[21] == 0xFF && c[22] == c[25] && c[23] == 0 && c[24] == 3 && c[27] == 1;
        if (!fmt) s_bcast_bad++;
        if (n >= 28) memcpy(s_bcast_last, c + 25, 3);
        if (!s_net_up) st = 0x0002;
    }
    if (id == ZH_EZSP_ID_CLEAR_TRANSIENT_LINK_KEYS) s_clear_cnt++;
    if (id == ZH_EZSP_ID_SEC_MAN_IMPORT_TRANSIENT_KEY) {             /* 通配 EUI64 + 公开密钥（SDK util.h:66-70），逐字节对手写的值 */
        static const uint8_t want[24] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                          'Z', 'i', 'g', 'B', 'e', 'e', 'A', 'l', 'l', 'i', 'a', 'n', 'c', 'e', '0', '9' };
        s_key_cnt++;
        if (memcmp(c + 5, want, 24)) s_key_bad++;
    }
    if (id == ZH_EZSP_ID_SET_CONCENTRATOR) {
        s_conc_total++;
        if (!s_net_up) s_conc_early++;
        else s_conc_boot++;
        /* on=1、HIGH_RAM 0xFFF9、10 s / 60 s、3、1、0（SDK 集中器插件默认值） */
        static const uint8_t want[10] = { 0x01, 0xF9, 0xFF, 0x0A, 0x00, 0x3C, 0x00, 0x03, 0x01, 0x00 };
        if (memcmp(c + 5, want, 10)) s_conc_bad++;
    }
    if (id == ZH_EZSP_ID_SET_SOURCE_ROUTE_DISCOVERY_MODE) {
        s_mtorr_total++;
        if (c[5] != 0x02 || !s_net_up) s_mtorr_bad++;
    }
    if (id == ZH_EZSP_ID_NETWORK_INIT) {
        if (s_pol_boot < 7 || !s_ep_boot) s_init_short++;
        s_ni_total++;
        if (s_ni_boot++) { s_ni_dup++; ni_status = 0x0002; }       /* 同一次启动里再来：INVALID_STATE */
        else ni_status = (s_has_network && s_profile2) ? ZH_SL_OK : ZH_SL_NOT_JOINED;   /* 同真 NCP：没写 profile 2，现网也认不出 */
        if (ni_status == ZH_SL_OK) s_net_up = true;
    }
    if (id == ZH_EZSP_ID_NETWORK_STATE) s_nstates++;
    if (id == ZH_EZSP_ID_SET_CONFIGURATION_VALUE) {
        s_cfgs++;
        if (c[5] == ZH_EZSP_CFG_STACK_PROFILE) s_profile2 = (c[6] | (c[7] << 8)) == ZH_HOST_STACK_PROFILE;
    }
    if (id == ZH_EZSP_ID_NOP) s_nops++;
    if (id == ZH_EZSP_ID_GET_NETWORK_PARAMETERS) s_params++;
    const bool drop = id != ZH_EZSP_ID_VERSION && s_app_drop > 0 && (double)(rnd() % 1000000) / 1e6 < s_app_drop;
    if (!drop) {
        size_t m;
        const uint8_t fc = (uint8_t)(0x80 | ((s_sync_cb && s_cbq_h != s_cbq_t) ? 0x04 : 0));
        if (legacy) {
            r[0] = c[0]; r[1] = 0x80; r[2] = 0x00; r[3] = 0x12; r[4] = 0x02; r[5] = 0x00; r[6] = 0x08;
            m = 7;
        } else {
            r[0] = c[0]; r[1] = fc; r[2] = 0x01; r[3] = c[3]; r[4] = c[4];
            m = 5;
            if (id == ZH_EZSP_ID_NETWORK_INIT) {
                r[5] = (uint8_t)ni_status; r[6] = (uint8_t)(ni_status >> 8); r[7] = r[8] = 0;
                m = 9;
                if (s_sync_cb && ni_status == ZH_SL_OK) r[1] |= 0x04;   /* UP 马上要排进队列 */
            } else if (id == ZH_EZSP_ID_GET_NETWORK_PARAMETERS) {
                memset(r + 5, 0, 25);
                r[9] = 0x01;                                            /* 协调器 */
                for (int i = 0; i < 8; i++) r[10 + i] = (uint8_t)(0x11 * (i + 1));
                r[18] = 0xEF; r[19] = 0xBE;                             /* PAN 0xBEEF */
                r[20] = 8; r[21] = 15;                                  /* 功率 8、信道 15 */
                m = 30;
            } else if (id == ZH_EZSP_ID_SET_CONFIGURATION_VALUE || id == ZH_EZSP_ID_SET_POLICY || id == ZH_EZSP_ID_ADD_ENDPOINT ||
                       id == ZH_EZSP_ID_SET_CONCENTRATOR || id == ZH_EZSP_ID_PERMIT_JOINING || id == ZH_EZSP_ID_SEC_MAN_IMPORT_TRANSIENT_KEY) {
                r[5] = (uint8_t)st; r[6] = (uint8_t)(st >> 8); r[7] = r[8] = 0;
                m = 9;
            } else if (id == ZH_EZSP_ID_SEND_BROADCAST) {
                r[5] = (uint8_t)st; r[6] = (uint8_t)(st >> 8); r[7] = r[8] = 0; r[9] = 0x42;   /* 状态 + APS 序号 */
                m = 10;
            } else if (id == ZH_EZSP_ID_GET_EUI64) {
                for (int i = 0; i < 8; i++) r[5 + i] = (uint8_t)(0xE0 + i);
                m = 13;
            } else if (id == ZH_EZSP_ID_SET_SOURCE_ROUTE_DISCOVERY_MODE) {
                r[5] = 0x10; r[6] = 0x27; r[7] = r[8] = 0;            /* 10000 ms 后下一次 MTORR */
                m = 9;
            } else if (id == ZH_EZSP_ID_NETWORK_STATE) {
                r[5] = s_net_up ? ZH_NET_STATUS_JOINED : 0;              /* JOINED_NETWORK / NO_NETWORK */
                m = 6;
            } else if (id == ZH_EZSP_ID_CALLBACK) {
                r[1] = 0x88;
                if (s_cbq_h != s_cbq_t) {
                    const uint16_t v = s_cbq[s_cbq_h++ % 64];
                    r[3] = (uint8_t)ZH_EZSP_ID_STACK_STATUS_HANDLER; r[4] = 0;
                    r[5] = (uint8_t)v; r[6] = (uint8_t)(v >> 8); r[7] = r[8] = 0;
                    if (s_cbq_h != s_cbq_t) r[1] |= 0x04;
                    m = 9;
                } else {
                    r[3] = (uint8_t)ZH_EZSP_ID_NO_CALLBACKS; r[4] = 0;
                }
            }
        }
        ncp_reply(r, m);
    }
    if (id == ZH_EZSP_ID_NETWORK_INIT && ni_status == ZH_SL_OK) push_stack_status(ZH_SL_NETWORK_UP);   /* 协议栈起来了（应答之后） */
    if (opened) push_stack_status(ZH_SL_NETWORK_OPENED);
}

static void ncp_reboot(uint8_t reason)
{
    load();
    ncp_set_inplace(true);
    ncp_set_time(s_now);
    ncp_boot(reason);
    s_ncp_boots++;
    s_cmds_boot = 0;
    s_ni_boot = 0;
    s_net_up = false;
    s_profile2 = false;
    memset(s_pol, 0xFF, sizeof(s_pol));
    s_pol_boot = s_ep_boot = s_conc_boot = 0;
    s_permit_until = 0;
    s_app_hung = false;
    s_cbq_h = s_cbq_t = 0;
}

/* 一毫秒：主机 tick → 主机发（经信道）→ NCP 收、处理 → NCP 发（经信道）→ 主机收 → tick。同 zhub.c 的任务循环：next_timeout 回 0 就再来一轮 */
static void step(void)
{
    static uint8_t buf[8192];
    s_now++;
    if (s_permit_until && !s_ncp_dead && (int32_t)(s_now - s_permit_until) >= 0) {   /* NCP 的窗口到期：自己关、报 NETWORK_CLOSED */
        s_permit_until = 0;
        push_stack_status(ZH_SL_NETWORK_CLOSED);
    }
    zh_host_tick(&H, s_now);
    for (int round = 0; round < 4; round++) {
        size_t n = zh_host_tx(&H, s_now, buf, sizeof(buf));
        s_rx_host_bytes += (uint32_t)n;
        n = channel(buf, n);
        if (!s_ncp_dead) {
            ncp_set_time(s_now);
            if (n) ncp_feed(buf, n);
            const int r = ncp_poll();
            if (r == 2) ncp_reboot(ZH_ASH_RESET_SOFTWARE);
            else if (r == 1) ncp_app_on_cmd();
            n = ncp_drain(buf, sizeof(buf));
            n = channel(buf, n);
            if (n) zh_host_rx(&H, buf, n, s_now);
        }
        zh_host_tick(&H, s_now);
        if (zh_host_next_timeout_ms(&H, s_now) != 0) break;
    }
}
static bool run_until(bool (*cond)(void), uint32_t max_ms)
{
    for (uint32_t i = 0; i < max_ms; i++) {
        if (cond()) return true;
        step();
    }
    return cond();
}
static bool is_running_up(void) { return H.state == ZH_HOST_RUNNING && H.net == ZH_NET_UP && H.have_params; }
static bool is_all_up(void) { return is_running_up() && H.up_step == ZH_HOST_UP_N && H.concentrator && H.mtorr && H.have_eui64; }
/* 第五步的每启动不变量：初始化序列都在 networkInit 之前、端点只加一次且没晚到、集中器 / MTORR 只在网络起来之后、参数对 */
static bool step5_invariants(void)
{
    return s_init_late == 0 && s_init_short == 0 && s_ep_dup == 0 && s_ep_late == 0 && s_conc_early == 0 && s_conc_bad == 0 && s_mtorr_bad == 0;
}
static bool is_running(void) { return H.state == ZH_HOST_RUNNING; }

static void fresh(void)
{
    s_now = 0;
    s_noise = s_app_drop = 0;
    s_app_hung = s_ncp_dead = s_sync_cb = false;
    s_has_network = true;
    s_ncp_boots = 0;
    s_first_legacy = s_ni_total = s_ni_dup = s_params = s_nops = s_unexpected = s_malformed = s_nstates = s_cfgs = 0;
    s_ep_dup = s_ep_late = s_init_late = s_init_short = s_conc_total = s_conc_early = s_conc_bad = s_mtorr_total = s_mtorr_bad = s_eui_total = 0;
    s_permit_cnt = s_permit_bad = s_bcast_cnt = s_bcast_bad = s_clear_cnt = s_key_cnt = s_key_bad = 0;
    s_rx_host_bytes = 0;
    memset(s_net_ev, 0, sizeof(s_net_ev));
    const zh_host_ops_t ops = { on_net, NULL, NULL, NULL };
    zh_host_init(&H, &ops);
    ncp_reboot(0x01);                 /* 上电：NCP 先报一次 RSTACK（POWERON），主机 RST 后它再软重启 */
    zh_host_start(&H, s_now);
}

static void h1_h2(void)
{
    fresh();
    CHECK(run_until(is_running_up, 20000), "H1 上电 → RUNNING / UP（%u ms，状态 %s）", s_now, zh_host_state_name(H.state));
    CHECK(H.params.channel == 15 && H.params.pan_id == 0xBEEF && H.params.ext_pan_id[7] == 0x88 && H.node_type == 1, "H1 读回信道 / PAN / 扩展 PAN");
    CHECK(s_first_legacy == (uint32_t)s_ncp_boots - 1 && s_ni_total == 1 && s_ni_dup == 0 && s_unexpected == 0 && s_malformed == 0,
          "H1 NCP：第一条是旧式头 version（%u/%d 次启动）、networkInit 恰好一次、没有别的命令", s_first_legacy, s_ncp_boots - 1);
    CHECK(H.have_version && (H.version_code >> 16) == 2 && H.fails == 0 && H.st.resets == 1, "H1 一次复位就起来、没有失败");
    CHECK(run_until(is_all_up, 2000) && H.eui64[0] == 0xE0 && H.mtorr_next_ms == 10000, "H1 UP 之后：EUI64、集中器、MTORR 都设好");
    CHECK(s_pol[0] == 0x02 && s_pol[1] == 0x10 && s_pol[3] == 0x30 && s_pol[4] == 0x40 && s_pol[5] == 0x51 && s_pol[6] == 0x60 && s_pol[9] == 0x00
          && s_pol_boot == 7,
          "H1 NCP 收到的 7 条策略取值都对（TC %02X、绑定 %02X、轮询 %02X、messageSent %02X、TC 密钥 %02X、应用密钥 %02X、公开密钥 TC 重新入网 %02X）",
          s_pol[0], s_pol[1], s_pol[3], s_pol[4], s_pol[5], s_pol[6], s_pol[9]);
    CHECK(step5_invariants() && s_ep_boot == 1 && s_conc_boot == 1 && s_mtorr_total == 1,
          "H1 策略与端点都在 networkInit 之前、端点一次、集中器在网络起来之后一次（晚到 %u、端点重复 %u / 晚到 %u、集中器提早 %u / 参数错 %u）",
          s_init_late, s_ep_dup, s_ep_late, s_conc_early, s_conc_bad);
    for (int i = 0; i < 60000; i++) step();
    CHECK(is_running_up() && s_nops >= 5 && H.st.resets == 1, "H1 空闲 60 s 保活（%u 条 nop），一直 RUNNING", s_nops);

    fresh();
    s_has_network = false;
    CHECK(run_until(is_running, 20000) && H.net == ZH_NET_NONE, "H2 NOT_JOINED → RUNNING / NONE");
    for (int i = 0; i < 180000; i++) step();
    CHECK(H.state == ZH_HOST_RUNNING && s_ni_total == 1 && s_unexpected == 0 && s_params == 0 && s_conc_total == 0 && s_eui_total == 0,
          "H2 3 分钟：networkInit 只一次、NCP 没收到别的命令（绝不建网、没有网络不开集中器）");
}

static void h3(void)
{
    fresh();
    s_ncp_dead = true;
    uint32_t bytes_in_backoff = 0, backoff_ms = 0;
    for (int i = 0; i < 40000; i++) {
        const bool bo = H.state == ZH_HOST_BACKOFF;
        const uint32_t b0 = s_rx_host_bytes;
        step();
        if (bo && H.state == ZH_HOST_BACKOFF) { bytes_in_backoff += s_rx_host_bytes - b0; backoff_ms++; }
    }
    /* 0 s 起：5 s 判第 1 次 → 立即重来 → 10 s 第 2 次 → 退避 5 s → 20 s 第 3 次 → 退避 30 s（到 50 s）：40 s 时退避共约 25 s */
    CHECK(H.fails == 3 && H.st.fails[ZH_HOST_FAIL_NO_RSTACK] == 3 && backoff_ms >= 24900 && bytes_in_backoff == 0 && H.state == ZH_HOST_BACKOFF,
          "H3 NCP 死 40 s：失败 %u 次、退避 %u ms 里主机发了 %u 字节", H.fails, backoff_ms, bytes_in_backoff);
    s_ncp_dead = false;
    ncp_reboot(0x01);
    CHECK(run_until(is_running_up, 70000), "H3 活过来后 70 s 内（最长一档退避 60 s + 复位握手）恢复（%s）", zh_host_state_name(H.state));
}

static void h4_h5(void)
{
    fresh();
    run_until(is_all_up, 20000);                 /* 等 UP 之后的序列走完：不然挂死时在途的那条先应答超时，原因就不是保活了 */
    const int boots0 = s_ncp_boots;
    const uint32_t ni0 = s_ni_total;
    s_app_hung = true;
    const uint32_t t0 = s_now;
    for (uint32_t i = 0; i < 60000 && !(s_ncp_boots > boots0 && is_running_up()); i++) step();
    CHECK(H.st.fails[ZH_HOST_FAIL_SESSION] == 1 && H.last_fail_code == ZH_SES_WHY_KEEPALIVE && s_ncp_boots > boots0 && is_running_up(),
          "H4 应用层挂死：保活超时 → 复位 → NCP 重启 → RUNNING");
    CHECK(s_ni_total == ni0 + 1 && s_ni_dup == 0, "H4 重启后重新 networkInit 一次");
    CHECK(s_now - t0 <= 10000 + 15000 + 8000, "H4 从挂死到恢复 ≤ 保活 + 应答超时 + 8 s（%u ms）", s_now - t0);

    /* H5：运行中 NCP 自己重启。SDK 的 ashStart() 按复位原因分两路（ash-ncp.c:266-281）：上电 / 软件 / bootloader → RSTACK；
       看门狗 / 断言 / 不明 → ERROR 帧（code = 复位原因）。两路主机都要发现并恢复 */
    const int boots1 = s_ncp_boots;
    ncp_reboot(ZH_ASH_RESET_SOFTWARE);
    for (uint32_t i = 0; i < 70000 && !(H.st.fails[ZH_HOST_FAIL_NCP_RESET] && is_running_up()); i++) step();
    CHECK(H.st.fails[ZH_HOST_FAIL_NCP_RESET] == 1 && H.last_fail_code == ZH_ASH_RESET_SOFTWARE && is_running_up() && s_ncp_boots >= boots1 + 2,
          "H5 NCP 软件重启（RSTACK）：主机发现 → 复位 → 恢复");
    const int boots2 = s_ncp_boots;
    ncp_reboot(0x03);                 /* EM2XX_RESET_WATCHDOG */
    for (uint32_t i = 0; i < 70000 && !(H.st.fails[ZH_HOST_FAIL_NCP_ERROR] && is_running_up()); i++) step();
    CHECK(H.st.fails[ZH_HOST_FAIL_NCP_ERROR] == 1 && H.last_fail_code == 0x03 && is_running_up() && s_ncp_boots >= boots2 + 2,
          "H5 NCP 看门狗复位（ERROR 帧，原因 %u）：主机发现 → 复位 → 恢复", (unsigned)H.last_fail_code);
}

static void h6_h7(void)
{
    fresh();
    run_until(is_running_up, 20000);
    zh_host_stop(&H);
    const uint32_t b0 = s_rx_host_bytes, ni0 = s_ni_total;
    for (int i = 0; i < 30000; i++) step();
    CHECK(s_rx_host_bytes == b0 && H.state == ZH_HOST_IDLE && H.net == ZH_NET_UNKNOWN, "H6 挂起 30 s：一个字节都不发");
    zh_host_start(&H, s_now);
    CHECK(run_until(is_running_up, 20000) && s_ni_total == ni0 + 1 && H.st.starts == 2, "H6 恢复：从头走一遍（networkInit 又一次）");

    fresh();
    s_sync_cb = true;
    CHECK(run_until(is_running_up, 20000) && H.ses.st.callbacks >= 1, "H7 同步回调：经 callback 取到 NETWORK_UP（%u 条回调）", H.ses.st.callbacks);
}

static void h8(void)
{
    fresh();
    run_until(is_running_up, 20000);
    s_noise = 0.01;
    s_app_drop = 0.03;
    uint32_t stuck = 0, last_change = s_now;
    zh_host_state_t prev = H.state;
    const uint32_t ni0 = s_ni_total;
    /* NCP 每次重启时主机若正 RUNNING：在它复位之前，一条 EZSP 应答都不许再认（认了 = 对着重启过的 NCP 照常"运行"） */
    uint32_t reboots_running = 0, detected = 0, false_ok = 0, r0 = 0, resp0 = 0;
    bool watching = false;
    for (int i = 0; i < 180000; i++) {
        if (i % 20000 == 10000) {                                    /* 每 20 s NCP 自己重启一次：初始化在噪声下反复走 */
            watching = H.state == ZH_HOST_RUNNING;
            if (watching) { reboots_running++; r0 = H.st.resets; resp0 = H.ses.st.responses; }
            ncp_reboot(ZH_ASH_RESET_SOFTWARE);
        }
        step();
        if (watching && H.st.resets != r0) { watching = false; detected++; }
        else if (watching && H.ses.st.responses != resp0) { watching = false; false_ok++; }
        if (H.state != prev) { prev = H.state; last_change = s_now; }
        if (H.state != ZH_HOST_RUNNING && s_now - last_change > 70000) stuck++;
    }
    CHECK(stuck == 0, "H8 噪声 3 分钟：没有哪个非 RUNNING 状态停留超过 70 s");
    /* 只钉不变量（Z59）。原先还要求"噪声里至少走到过一次 networkInit"：第五步 networkInit 前多了十条命令，3% 丢应答 + 每 20 s 重启下
       每次尝试都可能在前面失败、退避又拉到 30 / 60 s —— seed 25 三分钟里 6 次尝试全没走到。"噪声里能走完初始化"由 H9 精确判（15 次都要到 UP） */
    CHECK(s_ni_dup == 0, "H8 每次 NCP 启动里 networkInit 至多执行一次（%u 次启动、噪声里 %u 次 networkInit）", s_ncp_boots, s_ni_total - ni0);
    CHECK(step5_invariants(), "H8 噪声里每次启动：初始化序列在 networkInit 之前、端点不重复、集中器不提早（晚到 %u / 不全 %u / 端点重复 %u / 集中器提早 %u）",
          s_init_late, s_init_short, s_ep_dup, s_conc_early);
    /* 原先断言 NCP_RESET ≥ 2 次：那是统计上的期望不是不变量 —— 重启撞上主机正在退避（噪声里的别的失败）就看不见，按设计如此；
       第五步每次启动多了十条命令、吃 3% 丢应答的机会更多，seed 31337 只剩 1 次。现在钉的是：这个场景真的让 NCP 在主机 RUNNING 时重启过
       （≥ 1 次，不是空跑；≥ 2 次同样是统计期望，seed 1 只有 1 次）、主机之后复位过，而且复位之前没认过一条应答。⚠️ 最后一条在这个替身里很难被违反：SDK 的 ash-ncp 重启后
       收到 RST 之前不回任何 DATA（变异"ASH 收到 RSTACK 照样当连着"它也抓不到）—— "发现 NCP 重启"本身由 H5 逐路径精确判 */
    CHECK(reboots_running >= 1 && detected >= 1 && false_ok == 0,
          "H8 NCP 在主机 RUNNING 时重启 %u 次：复位之前没认过一条应答（误认 %u 次，发现 %u 次；NCP_RESET %u 次）", reboots_running, false_ok, detected,
          H.st.fails[ZH_HOST_FAIL_NCP_RESET]);
    s_noise = s_app_drop = 0;
    CHECK(run_until(is_running_up, 90000), "H8 噪声停了 90 s 内恢复 RUNNING / UP（%s）", zh_host_state_name(H.state));
    printf("   H8：复位 %u 次，失败原因 rstack %u / ncp_reset %u / error %u / ash 超时 %u / 会话 %u / networkInit %u\n", H.st.resets,
           H.st.fails[ZH_HOST_FAIL_NO_RSTACK], H.st.fails[ZH_HOST_FAIL_NCP_RESET], H.st.fails[ZH_HOST_FAIL_NCP_ERROR],
           H.st.fails[ZH_HOST_FAIL_ASH_TIMEOUTS], H.st.fails[ZH_HOST_FAIL_SESSION], H.st.fails[ZH_HOST_FAIL_NET_INIT]);
}

/* H9：初始化本身在噪声里反复走 —— 2% 噪声下连做 15 次从头开始（zh_host_start：连续失败清零），每次都要走到 RUNNING / UP，
   而且 NCP 每次启动里 networkInit 至多执行一次（networkInit 不幂等：回显 / 超时都只能复位重来，不能重发）。
   不用 restart：它按设计不清连续失败次数，2% 噪声下 RST / RSTACK 常被打坏，跨轮攒下的失败会让每次都落进 60 s 那一档退避。
   时限 200 s ≥ 退避表走到 60 s 那一档（0 + 5 + 30 + 60）再加四次各 5 s 的 RSTACK 等待与一次初始化 */
static void h9(void)
{
    fresh();
    run_until(is_running_up, 20000);
    s_noise = 0.02;
    const uint32_t ni0 = s_ni_total;
    int ok = 0;
    uint32_t worst = 0;
    for (int k = 0; k < 15; k++) {
        zh_host_start(&H, s_now);
        const uint32_t t0 = s_now;
        step();
        if (run_until(is_running_up, 200000)) ok++;
        if (s_now - t0 > worst) worst = s_now - t0;
    }
    s_noise = 0;
    CHECK(ok == 15 && s_ni_dup == 0 && s_ni_total - ni0 >= 15, "H9 噪声里 15 次从头初始化：%d 次到 UP（最慢 %u ms）、networkInit %u 次、同一次启动里重复 %u 次",
          ok, worst, s_ni_total - ni0, s_ni_dup);
    CHECK(H.st.starts == 16, "H9 start 计数（fresh 1 + 15）");
    CHECK(step5_invariants() && run_until(is_all_up, 30000), "H9 噪声里每次初始化：第五步的不变量都守住，最后集中器也设好了");
}

/* H10：运行中网络掉了。① 掉了不回来（NCP 报 NETWORK_DOWN、networkState 也说不在）：主机给它 ZH_HOST_NET_DOWN_MS，查一次，复位 →
   NCP 的新一次启动里 networkInit 一次 → UP；② 掉了又在期限内回来：不复位。两种都不许在同一次启动里补发 networkInit */
static void h10(void)
{
    fresh();
    run_until(is_all_up, 20000);
    const uint32_t ni0 = s_ni_total, rs0 = H.st.resets;
    const int boots0 = s_ncp_boots;
    s_net_up = false;
    push_stack_status(ZH_SL_NETWORK_DOWN);
    for (int i = 0; i < 1000; i++) step();
    CHECK(H.net == ZH_NET_DOWN && H.state == ZH_HOST_RUNNING, "H10 NETWORK_DOWN：RUNNING / DOWN");
    const uint32_t t0 = s_now;
    for (uint32_t i = 0; i < 90000 && !(H.st.fails[ZH_HOST_FAIL_NET_DOWN] && is_all_up()); i++) step();
    CHECK(H.st.fails[ZH_HOST_FAIL_NET_DOWN] == 1 && is_all_up() && H.st.resets == rs0 + 1 && s_ncp_boots == boots0 + 1,
          "H10 没回来：%u ms 后记 NET_DOWN、复位 → NCP 重启 → UP", s_now - t0);
    CHECK(s_ni_total == ni0 + 1 && s_ni_dup == 0 && s_nstates >= 1, "H10 新的一次启动里 networkInit 一次、同一次启动里没补发（networkState %u 次）", s_nstates);

    const uint32_t rs1 = H.st.resets, ni1 = s_ni_total;
    push_stack_status(ZH_SL_NETWORK_DOWN);
    for (int i = 0; i < 20000; i++) step();
    push_stack_status(ZH_SL_NETWORK_UP);
    for (int i = 0; i < 70000; i++) step();
    CHECK(is_running_up() && H.st.resets == rs1 && s_ni_total == ni1, "H10 20 s 后自己回来了：不复位、不发 networkInit");
    CHECK(is_all_up() && s_conc_boot == 2 && step5_invariants(), "H10 回来之后集中器在同一次启动里重设（%u 次）、端点没重加", s_conc_boot);
}

/* H11：入网窗口端到端 */
static bool is_win_closed(void) { return H.win_state == ZH_WIN_CLOSED; }
static bool is_win_open(void) { return H.win_state == ZH_WIN_OPEN; }
static void h11(void)
{
    fresh();
    run_until(is_all_up, 20000);
    CHECK(s_pol[0] == 0x02, "H11 开窗前 NCP 的信任中心是窗口外的取值（%02X）", s_pol[0]);
    CHECK(zh_host_permit_join(&H, 30, s_now) == ZH_PJ_OK && run_until(is_win_open, 2000), "H11 开 30 s：开了");
    CHECK(s_pol[0] == 0x03 && s_permit_until && s_bcast_cnt == 1 && s_bcast_bad == 0 && s_bcast_last[1] == 30 && s_bcast_last[2] == 1,
          "H11 NCP：信任中心 0x03、permitJoining 计时中、广播一条（[%u, %u, %u]、格式对）", s_bcast_last[0], s_bcast_last[1], s_bcast_last[2]);
    CHECK(s_key_cnt == 1 && s_key_bad == 0, "H11 NCP 收到临时链路密钥一条、通配 EUI64 + 公开密钥（%u 条、错 %u 条；2026-10-23 小米抓包）", s_key_cnt, s_key_bad);
    for (int i = 0; i < 200; i++) step();
    CHECK(H.ncp_open, "H11 NCP 报了 NETWORK_OPENED");
    const uint32_t t_open = s_now;
    CHECK(run_until(is_win_closed, 35000), "H11 到期关完（%u ms）", s_now - t_open);
    CHECK(s_pol[0] == 0x02 && !s_permit_until && s_clear_cnt == 1 && s_bcast_cnt == 2 && s_bcast_last[1] == 0 && !H.ncp_open && H.st.win_expired == 1,
          "H11 关完：NCP 信任中心写回 0x02、窗口关、清了临时链路密钥、广播 0、收到 NETWORK_CLOSED");

    /* 噪声里开关 6 轮（1% 字节噪声 + 3% 丢应答）：每轮都要有结论、关完 NCP 的信任中心都是窗口外的取值；链路被噪声打断复位的那几轮
       窗口回 CLOSED、NCP 新启动的 k_init 照样写成窗口外 */
    s_noise = 0.01;
    s_app_drop = 0.03;
    int rounds = 0, bad_after = 0, opened = 0;
    for (int k = 0; k < 6; k++) {
        if (!run_until(is_all_up, 120000)) continue;
        if (zh_host_permit_join(&H, 30, s_now) != ZH_PJ_OK) continue;
        rounds++;
        for (int i = 0; i < 3000 && !is_win_open() && H.win_state != ZH_WIN_CLOSED; i++) step();
        if (is_win_open()) opened++;
        if (k % 2) zh_host_permit_join(&H, 0, s_now);    /* 一半主动关、一半等到期 */
        run_until(is_win_closed, 60000);
        for (int i = 0; i < 2000; i++) step();         /* 让在途的都了结 */
        if (H.state == ZH_HOST_RUNNING && s_pol[0] != 0x02) bad_after++;
    }
    s_noise = s_app_drop = 0;
    CHECK(rounds >= 4 && bad_after == 0 && step5_invariants(), "H11 噪声里 %d 轮（开成 %d 轮）：每轮关完 NCP 的信任中心都是 0x02（不对 %d 轮）",
          rounds, opened, bad_after);
    printf("   H11：开 %u / 开失败 %u / 关 %u / 到期 %u / 写回策略重试 %u / 跳过 %u；链路复位 %u 次\n", H.st.win_opens, H.st.win_open_fails,
           H.st.win_closes, H.st.win_expired, H.st.win_close_retries, H.st.win_skipped, H.st.resets);

    /* 窗口开着时 NCP 自己重启 */
    run_until(is_all_up, 120000);
    zh_host_permit_join(&H, 120, s_now);
    run_until(is_win_open, 3000);
    CHECK(is_win_open() && s_pol[0] == 0x03, "H11 再开着");
    ncp_reboot(ZH_ASH_RESET_SOFTWARE);
    for (int i = 0; i < 200 && H.win_state != ZH_WIN_CLOSED; i++) step();
    CHECK(H.win_state == ZH_WIN_CLOSED, "H11 NCP 重启：主机窗口回 CLOSED");
    CHECK(run_until(is_all_up, 70000) && s_pol[0] == 0x02 && !s_permit_until, "H11 NCP 新启动的信任中心是窗口外的取值、没开窗");
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("用法：%s <libsdkncp.so>\n", argv[0]); return 2; }
    s_path = argv[1];
    if (argc >= 3) {
        s_rng = (uint32_t)strtoul(argv[2], NULL, 0);
        if (!s_rng) s_rng = 1;
    }
    h1_h2();
    h3();
    h4_h5();
    h6_h7();
    h8();
    h9();
    h10();
    h11();
    printf("host_interop：%d 通过，%d 失败\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
