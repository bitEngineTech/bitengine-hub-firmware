/* zh_dev 的真值表（docs/15 设计 8.6、8.9、8.12 ③；M4 第七步 a）。编的是产品源文件 ../zh_dev.c（连同 EZSP 编解码）。
 * ops 是假的：send 把帧记下来（字节逐个核）、save / event 计数。
 *   D1 加载：版本 / IEEE / 越界 / 重复都拒      D2 被动学习：不认识的 nwk → 问 NCP 地址表（帧逐字节）→ 问到了建记录；10 s 内不重问
 *   D3 NCP 表里没有 → IEEE_addr_req（帧逐字节）→ 应答序号对上才认；超时放掉   D4 入网：放行的建、拒绝的不建；重新入网换了短地址；离开 / 回来
 *   D5 设备宣告：建记录、能力字节进标志；一样的不重写   D6 短地址冲突：旧记录的短地址作废   D7 表满 / 坏 IEEE   D8 交不出去 / 查询期限
 *   D9 同时查的上限   D10 别人的 tag 不认   D11 变异测试存活的几处（拒绝的重新入网、应答状态、地址表超时、入网取消在查的）
 *   D12 读不出来的格子保留、表满不发空口查询、年龄封顶（第二轮审计）
 *   D13 查询限速、宣告里的短地址、重新入网算发现、宣告建记录一次写好（第一轮审计）
 *   访谈（第七步 b）：D14 休眠设备完整走一遍（帧逐字节、结果落盘一次、事件）   D15 休眠设备没醒不发、醒了才发   D16 超时重试到失败
 *   D17 Basic 回 0x89 / 缺属性 → 改单读   D18 默认应答拒读   D19 空闲接收的设备随时可问   D20 30 min 总期限   D21 短地址作废不发
 *   D22 已访谈过的设备重新配对：再访谈、算新入网   D23 迟到 / 不对号的应答、sendUnicast 没发出去   D24 坏的 Simple_Desc_rsp   D25 开机加载的
 *   D26 变异测试存活的几处（节点描述符被拒、端点 / 输出簇不对、Basic 读不到失败、有进展清次数、截 23 B、电池、方向位、序号）
 *   D27 太短的 Node / Active 应答、ZoneType 读不到
 *   D28 两轮审计补的（网络不是 UP、失败后重来、旧记录复位、重新配对落盘、入网 60 s 当醒着、改单读清次数、醒着的休眠设备优先、ZLL 用 HA profile）
 *   D29 新入网失败后重试成功照样推、失败只推一次、发事件时表已 dirty
 *   读数（M5 第一步）：D30 IAS 通知   D31 属性上报的换算与无效值   D32 占用   D33 IAS 登记应答
 *   D34 两轮审计补的（只收规定类型、电量低 / 人体每次翻转都报（2026-10-24 起不限间隔）、登记补 ZoneType / 限速、重新配对清读数、访谈顺带读 ZoneStatus、读数时刻封顶）
 *   D35 随机 ZCL 帧穿过 zh_dev_on_zcl 的模糊（UBSan）
 *   D36 写 CIE 地址（CONFIGURE 第一步，设计 8.6 IAS 第 1、2 条）：帧照 SDK setCieAddress、成了补发登记应答、失败 / 默认应答 / 超时 / 不知道自己的 EUI64 都不算访谈失败
 *   D37 欠着 CIE / 登记的补做   D38 绑定 + 上报配置（CONFIGURE 第二步，表 8.6-1）：只对新入网的、帧格式、失败 / 期限不算访谈失败
 *   D39 开机补读电量：只在刚说过话时、次数与间隔封顶、收到就停、路由器 / 没电源簇不读
 *   D40 控制（8.11）：帧格式、只给常电、结论（默认应答 / 上报 / messageSent / 期限）、迟到的不改结论、一条在途
 *   D41 涂鸦私有簇的读数（M5 第八步）：查表、换算进读数、复用 IAS / 占用 / 电量事件、主报警没报过时不置 ZONE、不在表里 / 没访谈完 / 表里没有的 dp 只计数
 *   D42 涂鸦设备的开机招呼：magic packet + dataQuery（帧逐字节）、一台一轮、休眠的只在刚说过话时、交不出去不占次数
 *   D43 涂鸦设备的控制（M5 第九步）：写数据点（帧逐字节、禁默认应答）、确认看回报的那个 dp、亮度带开、切换按读数取反、ZCL 优先、没有写法 / 休眠
 *   D44 小米 / Aqara（M5 第十步）：从主动上报学型号（落盘）、0xFF01 的 TLV 换算、电量曲线、门磁的 OnOff 改写、人体 90 s 清零、0xFCC0（厂商专用帧）、
 *       没访谈完也按表、漏水的 IAS 报警靠表认、重新配对才清型号
 *   D45 可用性（8.10；M5 第十一步）：常电静默 10 min 后 ping（帧、10 s 一次）、3 次没回判离线、回了作罢；电池 25 h；收到消息恢复；
 *       网不是 UP 不判、恢复后从那刻起算；交不出去不占次数；一拍一条；离开了的不判
 *   D46 休眠设备的控制排队（8.11 第 6 步；M5 第十二步）：立刻 PENDING、新命令顶掉旧的、做不到的当场回；醒来 3 s 内补发（帧同同步路）、
 *       同一个醒着的窗口不连发、送达清掉 / 没送达留着 / messageSent 一直不来也放开、30 min 作废、涂鸦切换按补发时的读数
 *   D47 主动发现（8.12 ④；M6 第一步）：网 UP 60 s 后从协调器（发给自己）+ 已知路由器开始爬（帧逐字节）；分页；只建两样都不认识的、
 *       路由器马上访谈、终端等它说话；跳过前子节点 / 别的网 / 地址不认；已有记录不改、短地址被占不建；超时重试到放弃；不支持跳过；
 *       坏长度；迟到 / 别人的不认；网掉暂停不算次数；NCP 拒了不干等；层数 / 页数封顶；一次开机一轮；网没 UP 不开始
 *   D48 访谈失败、随时听得见的（2026-10-24 Sengled 灯）：不等它说话，30 s / 2 min / 10 min / 之后每小时自己重来（重开那一拍就发）；
 *       休眠的不自己重来；网不是 UP / 短地址作废不重来；一轮只出一次失败事件；重新宣告 / 回来 / 访谈完 / 重新配对后间隔从第一档算；
 *       发现建的从没听到过的不每小时问；重新配对失败后到点重来成功照样推"认出来了"；一拍只重开一台 */
#include <stdio.h>
#include <string.h>

#include "zh_dev.h"

static int s_fail, s_pass;
#define CHECK(cond, ...) do { if (cond) { s_pass++; } else { s_fail++; printf("❌ %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static zh_dev_core_t D;
static uint8_t  s_frame[64][64];
static size_t   s_frame_n[64];
static uint32_t s_tag[64];
static bool     s_idem[64];
static int      s_nsent, s_saves, s_ev[16], s_ev_src[8];   /* s_ev 按 zh_dev_ev_t 编号、s_ev_src 按 zh_dev_src_t 编号；加种类时这里要够大 */
static zh_dev_src_t s_last_src;
static bool     s_dirty_at_ev;                 /* 发事件那一刻表是不是 dirty（zhub.c 发事件前要先发布快照） */
static bool     s_send_ok = true;
static uint8_t  s_seq;

static zh_dev_src_t s_last_ident_src;                /* 最近一次 IDENTIFIED 的来源（D38） */
static void op_save(void *c, unsigned idx, const zh_dev_rec_t *r) { (void)c; (void)idx; (void)r; s_saves++; }
static void op_event(void *c, zh_dev_ev_t ev, zh_dev_src_t src, unsigned idx, const zh_dev_rec_t *r)
{
    (void)c; (void)idx; (void)r;
    if ((unsigned)ev < sizeof(s_ev) / sizeof(s_ev[0])) s_ev[ev]++;
    s_last_src = src;
    if (ev == ZH_DEV_EV_IDENTIFIED) s_last_ident_src = src;
    s_dirty_at_ev = D.dirty;
    if (ev == ZH_DEV_EV_ADDED && (unsigned)src < sizeof(s_ev_src) / sizeof(s_ev_src[0])) s_ev_src[src]++;
}
static int s_send_budget = -1;                     /* 应用命令队列还剩几个空位（-1 = 不限；D37 模拟一拍只剩一个空位） */
static bool op_send(void *c, const uint8_t *f, size_t n, uint32_t tag, bool idem)
{
    (void)c;
    if (!s_send_ok || s_send_budget == 0) return false;
    if (s_send_budget > 0) s_send_budget--;
    const int k = s_nsent++ % 64;
    memcpy(s_frame[k], f, n);
    s_frame_n[k] = n;
    s_tag[k] = tag;
    s_idem[k] = idem;
    return true;
}
static uint8_t op_seq(void *c) { (void)c; return ++s_seq; }
/* 从第 from 条起、tag 种类是 kind 的帧有几条 */
static int count_kind(int from, uint32_t kind)
{
    int c = 0;
    for (int j = from; j < s_nsent; j++) if ((s_tag[j % 64] & 0xFu) == kind) c++;
    return c;
}

static void fresh(void)
{
    s_nsent = s_saves = 0;
    memset(s_ev, 0, sizeof(s_ev));
    memset(s_ev_src, 0, sizeof(s_ev_src));
    s_send_ok = true;
    s_send_budget = -1;
    s_seq = 0x20;
    const zh_dev_ops_t ops = { op_save, op_event, op_send, op_seq, NULL };
    zh_dev_init(&D, &ops);
    zh_dev_set_net_up(&D, true);
    D.disc.state = ZH_DISC_DONE;                   /* 主动发现（D47）默认关着：别的用例的模拟时间常常过了 60 s，它的请求会混进帧计数。D47 自己放开 */
}

static const uint8_t EA[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
static const uint8_t EB[8] = { 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8 };

/* 回包帧：[seq][fc 0x80][0x01][id lo][id hi][参数] */
static zh_ezsp_frame_t mkrsp(uint16_t id, const uint8_t *p, size_t n, uint8_t *buf)
{
    buf[0] = 0; buf[1] = 0x80; buf[2] = 0x01; buf[3] = (uint8_t)id; buf[4] = (uint8_t)(id >> 8);
    if (n) memcpy(buf + 5, p, n);
    zh_ezsp_frame_t fr;
    zh_ezsp_parse(buf, 5 + n, &fr);
    return fr;
}
static void lookup_rsp(uint32_t tag, uint32_t status, const uint8_t eui[8], uint32_t now)
{
    uint8_t p[12], buf[32];
    p[0] = (uint8_t)status; p[1] = (uint8_t)(status >> 8); p[2] = (uint8_t)(status >> 16); p[3] = (uint8_t)(status >> 24);
    memcpy(p + 4, eui, 8);
    const zh_ezsp_frame_t fr = mkrsp(ZH_EZSP_ID_LOOKUP_EUI64_BY_NODE_ID, p, 12, buf);
    zh_dev_on_app_result(&D, tag, ZH_SES_R_OK, &fr, now);
}
static void send_rsp(uint32_t tag, uint32_t status, uint32_t now)
{
    uint8_t p[5] = { (uint8_t)status, (uint8_t)(status >> 8), 0, 0, 0x42 }, buf[16];
    const zh_ezsp_frame_t fr = mkrsp(ZH_EZSP_ID_SEND_UNICAST, p, 5, buf);
    zh_dev_on_app_result(&D, tag, ZH_SES_R_OK, &fr, now);
}
static void ieee_rsp(uint8_t seq, uint8_t status, const uint8_t eui[8], uint16_t nwk, uint32_t now)
{
    uint8_t pl[12] = { seq, status };
    memcpy(pl + 2, eui, 8);
    pl[10] = (uint8_t)nwk; pl[11] = (uint8_t)(nwk >> 8);
    zh_dev_on_zdo(&D, nwk, ZH_ZDO_IEEE_ADDR_RSP, pl, sizeof(pl), now);
}
static void annce(uint16_t nwk, const uint8_t eui[8], uint8_t cap, uint32_t now)
{
    uint8_t pl[12] = { 0x77, (uint8_t)nwk, (uint8_t)(nwk >> 8) };
    memcpy(pl + 3, eui, 8);
    pl[11] = cap;
    zh_dev_on_zdo(&D, nwk, 0x0013, pl, sizeof(pl), now);
}

/* ---- 访谈用的 ---- */
/* 发出去的 sendUnicast 帧：[EZSP 头 5][type][dest:2][APS 11：profile 2、cluster 2、src 1、dst 1、options 2、group 2、seq 1][messageTag:2][len][载荷] */
static uint16_t fr_cluster(int k) { return (uint16_t)(s_frame[k % 64][10] | (s_frame[k % 64][11] << 8)); }
static uint16_t fr_profile(int k) { return (uint16_t)(s_frame[k % 64][8] | (s_frame[k % 64][9] << 8)); }
static uint16_t fr_dest(int k) { return (uint16_t)(s_frame[k % 64][6] | (s_frame[k % 64][7] << 8)); }
static uint16_t fr_opts(int k) { return (uint16_t)(s_frame[k % 64][14] | (s_frame[k % 64][15] << 8)); }
static uint8_t  fr_dst_ep(int k) { return s_frame[k % 64][13]; }
static const uint8_t *fr_pl(int k) { return s_frame[k % 64] + 22; }
static uint8_t  fr_pl_n(int k) { return s_frame[k % 64][21]; }
static int last(void) { return s_nsent - 1; }
/* 最近一条是 ZDO：序号在载荷第 0 字节；是 ZCL：在第 1 字节 */
static uint8_t last_seq(void) { return fr_profile(last()) == 0 ? fr_pl(last())[0] : fr_pl(last())[1]; }

static void node_rsp(uint8_t seq, uint8_t status, uint16_t nwk, uint8_t logical, uint8_t cap, uint16_t manuf, uint32_t now)
{
    uint8_t pl[17] = { seq, status, (uint8_t)nwk, (uint8_t)(nwk >> 8), logical, 0x40, cap, (uint8_t)manuf, (uint8_t)(manuf >> 8), 0x52, 0x80, 0x00, 0x00, 0x2C, 0x80, 0x00, 0x00 };
    zh_dev_on_zdo(&D, nwk, ZH_ZDO_NODE_DESC_RSP, pl, status ? 4 : sizeof(pl), now);
}
static void active_rsp(uint8_t seq, uint8_t status, uint16_t nwk, const uint8_t *eps, uint8_t n, uint32_t now)
{
    uint8_t pl[16] = { seq, status, (uint8_t)nwk, (uint8_t)(nwk >> 8), n };
    memcpy(pl + 5, eps, n);
    zh_dev_on_zdo(&D, nwk, ZH_ZDO_ACTIVE_EP_RSP, pl, 5u + n, now);
}
static void simple_rsp(uint8_t seq, uint8_t status, uint16_t nwk, uint8_t ep, uint16_t prof, uint16_t devid,
                       const uint16_t *in, uint8_t nin, const uint16_t *out, uint8_t nout, uint32_t now)
{
    uint8_t pl[96] = { seq, status, (uint8_t)nwk, (uint8_t)(nwk >> 8) };
    size_t k = 5;
    if (status == 0) {
        pl[k++] = ep; pl[k++] = (uint8_t)prof; pl[k++] = (uint8_t)(prof >> 8); pl[k++] = (uint8_t)devid; pl[k++] = (uint8_t)(devid >> 8); pl[k++] = 0x01;
        pl[k++] = nin;
        for (uint8_t j = 0; j < nin; j++) { pl[k++] = (uint8_t)in[j]; pl[k++] = (uint8_t)(in[j] >> 8); }
        pl[k++] = nout;
        for (uint8_t j = 0; j < nout; j++) { pl[k++] = (uint8_t)out[j]; pl[k++] = (uint8_t)(out[j] >> 8); }
    }
    pl[4] = (uint8_t)(k - 5);
    zh_dev_on_zdo(&D, nwk, ZH_ZDO_SIMPLE_DESC_RSP, pl, k, now);
}
/* ZCL 读属性应答：recs 是已经编好的记录字节 */
static void zcl_rsp(uint16_t nwk, uint16_t cluster, uint8_t seq, const uint8_t *recs, size_t n, uint32_t now)
{
    uint8_t b[96] = { ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR, seq, ZH_ZCL_CMD_READ_ATTR_RSP };
    memcpy(b + 3, recs, n);
    zh_dev_on_zcl(&D, nwk, ZH_HA_PROFILE, cluster, 1, b, 3 + n, now);
}
static void zcl_default_rsp(uint16_t nwk, uint16_t cluster, uint8_t seq, uint8_t status, uint32_t now)
{
    const uint8_t b[5] = { ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR, seq, ZH_ZCL_CMD_DEFAULT_RSP, ZH_ZCL_CMD_READ_ATTR, status };
    zh_dev_on_zcl(&D, nwk, ZH_HA_PROFILE, cluster, 1, b, sizeof(b), now);
}
/* 任意 ZCL 帧（读数测试用）：[fc][seq][cmd][载荷] */
static void zcl_frame(uint16_t nwk, uint16_t cluster, uint8_t fc, uint8_t cmd, const uint8_t *p, size_t n, uint8_t src_ep, uint32_t now)
{
    uint8_t b[64] = { fc, 0x5A, cmd };
    memcpy(b + 3, p, n);
    zh_dev_on_zcl(&D, nwk, ZH_HA_PROFILE, cluster, src_ep, b, 3 + n, now);
}
static void zone_notify(uint16_t nwk, uint16_t status, uint32_t now)
{
    const uint8_t p[6] = { (uint8_t)status, (uint8_t)(status >> 8), 0x00, 0x00, 0x00, 0x00 };
    zcl_frame(nwk, ZH_CL_IAS_ZONE, ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_S2C, ZH_IAS_CMD_STATUS_CHANGE, p, sizeof(p), 1, now);
}
static void report(uint16_t nwk, uint16_t cluster, const uint8_t *recs, size_t n, uint32_t now)
{
    zcl_frame(nwk, cluster, ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR, ZH_ZCL_CMD_REPORT_ATTR, recs, n, 1, now);
}

/* D38 用：最近一次 IDENTIFIED 是不是按新入网推的；计划第 k 条的簇 */
static bool s_ev_src_last_identified_join(void) { return s_last_ident_src == ZH_DEV_SRC_JOIN; }
static uint16_t k_cfg_cl_of(int idx, int k);

/* 设备说了一句（应答之后 zhub.c 也会调 on_rx） */
static void spoke(uint16_t nwk, uint32_t now) { zh_dev_on_rx(&D, nwk, -40, 200, now); }

/* ---- 主动发现用的（D47）：一条 22 B 的邻居条目、一页 LQI 表应答 ---- */
static const uint8_t XPAN[8] = { 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD };
static const uint8_t XPAN_OTHER[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
typedef struct { uint8_t b[22]; } lqi_ent_t;
static lqi_ent_t lqi_ent(const uint8_t xpan[8], const uint8_t eui[8], uint16_t nwk, uint8_t type, uint8_t rx, uint8_t rel, uint8_t depth)
{
    lqi_ent_t e;
    memcpy(e.b, xpan, 8);
    memcpy(e.b + 8, eui, 8);
    e.b[16] = (uint8_t)nwk; e.b[17] = (uint8_t)(nwk >> 8);
    e.b[18] = (uint8_t)(type | (rx << 2) | (rel << 4));
    e.b[19] = 0x02; e.b[20] = depth; e.b[21] = 0xB4;
    return e;
}
static void lqi_rsp(uint16_t sender, uint8_t seq, uint8_t status, uint8_t total, uint8_t start, const lqi_ent_t *ents, uint8_t n, uint32_t now)
{
    uint8_t pl[5 + 22 * 4] = { seq, status, total, start, n };
    for (uint8_t i = 0; i < n; i++) memcpy(pl + 5 + 22 * i, ents[i].b, 22);
    zh_dev_on_zdo(&D, sender, ZH_ZDO_LQI_TABLE_RSP, pl, status ? 2u : 5u + 22u * n, now);
}
/* 最近一条是不是发给 nwk 的 LQI 表请求、起始下标 start */
static bool last_is_lqi(uint16_t nwk, uint8_t start)
{
    const int k = last();
    return s_nsent > 0 && (s_tag[k % 64] & 0xFu) == ZH_DEV_TAG_KIND_DISC && fr_profile(k) == 0 && fr_cluster(k) == ZH_ZDO_LQI_TABLE_REQ &&
           fr_dest(k) == nwk && fr_pl_n(k) == 2 && fr_pl(k)[1] == start && !s_idem[k % 64];
}
/* 打开主动发现（fresh() 默认关着），网 UP 的那一拍是 t */
static void disc_open(uint32_t t)
{
    D.disc.state = ZH_DISC_IDLE;
    zh_dev_set_ext_pan(&D, XPAN);
    zh_dev_tick(&D, t);
}

/* 自动应答：按最近一条请求回成功，直到访谈走到 stop_at（ZH_IV_NONE = 走完）/ 失败 / 不再发 */
static void auto_answer(int idx, uint16_t nwk, uint32_t *t, bool ias, uint8_t stop_at)
{
    for (int guard = 0; guard < 48 && D.dev[idx].iv >= ZH_IV_NODE && D.dev[idx].iv <= ZH_IV_REPORT && D.dev[idx].iv != stop_at; guard++) {
        spoke(nwk, *t);
        const int before = s_nsent;
        zh_dev_tick(&D, *t);
        if (s_nsent == before) break;
        const int k = last();
        const uint8_t sq = last_seq();
        *t += 50;
        switch (fr_cluster(k)) {
        case ZH_ZDO_NODE_DESC_REQ: node_rsp(sq, 0, nwk, 0x01, 0x8E, 0x1234, *t); break;
        case ZH_ZDO_ACTIVE_EP_REQ: active_rsp(sq, 0, nwk, (const uint8_t[1]){ 1 }, 1, *t); break;
        case ZH_ZDO_BIND_REQ: zh_dev_on_zdo(&D, nwk, ZH_ZDO_BIND_RSP, (const uint8_t[2]){ sq, 0 }, 2, *t); break;
        case ZH_ZDO_SIMPLE_DESC_REQ:
            if (ias) simple_rsp(sq, 0, nwk, 1, 0x0104, 0x0402, (const uint16_t[2]){ 0x0000, 0x0500 }, 2, NULL, 0, *t);
            else simple_rsp(sq, 0, nwk, 1, 0x0104, 0x0100, (const uint16_t[2]){ 0x0000, 0x0006 }, 2, NULL, 0, *t);
            break;
        default: {                                       /* ZCL 读属性：问的每个都回成功；写属性（CIE 地址）回成功 */
            if (fr_pl(k)[2] == ZH_ZCL_CMD_CFG_REPORT) {
                const uint8_t cb[4] = { ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR, sq, ZH_ZCL_CMD_CFG_REPORT_RSP, ZH_ZCL_ST_SUCCESS };
                zh_dev_on_zcl(&D, nwk, ZH_HA_PROFILE, fr_cluster(k), 1, cb, sizeof(cb), *t);
                break;
            }
            if (fr_pl(k)[2] == ZH_ZCL_CMD_WRITE_ATTR) {
                const uint8_t wb[4] = { ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR, sq, ZH_ZCL_CMD_WRITE_ATTR_RSP, ZH_ZCL_ST_SUCCESS };
                zh_dev_on_zcl(&D, nwk, ZH_HA_PROFILE, fr_cluster(k), 1, wb, sizeof(wb), *t);
                break;
            }
            uint8_t recs[64];
            size_t n = 0;
            const uint8_t *p = fr_pl(k);
            const uint8_t pn = fr_pl_n(k);
            for (uint8_t j = 3; j + 1 < pn; j += 2) {
                const uint16_t id = (uint16_t)(p[j] | (p[j + 1] << 8));
                recs[n++] = p[j]; recs[n++] = p[j + 1]; recs[n++] = 0;
                if (id == ZH_BASIC_POWER) { recs[n++] = 0x30; recs[n++] = 0x01; }
                else if (id == ZH_IAS_ZONE_TYPE) { recs[n++] = 0x31; recs[n++] = 0x15; recs[n++] = 0x00; }
                else if (id == ZH_IAS_ZONE_STATUS) { recs[n++] = 0x19; recs[n++] = 0x00; recs[n++] = 0x00; }
                else if (id == ZH_IAS_ZONE_STATE) { recs[n++] = ZH_ZCL_T_ENUM8; recs[n++] = ZH_IAS_STATE_ENROLLED; }
                else { recs[n++] = 0x42; recs[n++] = 2; recs[n++] = 'A'; recs[n++] = 'B'; }
            }
            zcl_rsp(nwk, fr_cluster(k), sq, recs, n, *t);
        }
        }
    }
}

/* 第 from 条起有没有发往 nwk 的 Node_Desc_req（D48：常电设备静默 10 min 后可用性 ping 也会发帧，只数访谈的） */
static bool node_desc_since(int from, uint16_t nwk)
{
    for (int k = from; k < s_nsent; k++)
        if (fr_profile(k) == 0 && fr_cluster(k) == ZH_ZDO_NODE_DESC_REQ && fr_dest(k) == nwk) return true;
    return false;
}

/* 第 n 次失败后自己重来的间隔（D48；与 zh_dev.c 的 iv_on_retry_gap 同表） */
static uint32_t iv_gap_of(int n) { return n <= 1 ? ZH_IV_ON_RETRY1_MS : n == 2 ? ZH_IV_ON_RETRY2_MS : n == 3 ? ZH_IV_ON_RETRY3_MS : ZH_IV_RETRY_MS; }

/* 随时听得见的设备：一直不回，直到判访谈失败（D48）。*t 停在判失败的那一拍 */
static void iv_fail_now(int idx, uint32_t *t)
{
    for (int guard = 0; guard < 32 && D.dev[idx].iv != ZH_IV_FAILED; guard++) {
        *t += ZH_IV_STEP_TIMEOUT_MS;
        zh_dev_tick(&D, *t);
    }
}

int main(void)
{
    /* ---- D1 ---- */
    fresh();
    zh_dev_rec_t r;
    memset(&r, 0, sizeof(r));
    r.ver = ZH_DEV_REC_VER;
    memcpy(r.ieee, EA, 8);
    r.nwk = 0x1234;
    CHECK(zh_dev_load(&D, 3, &r, 1000) && zh_dev_count(&D) == 1 && zh_dev_find_ieee(&D, EA) == 3 && zh_dev_find_nwk(&D, 0x1234) == 3, "D1 加载到下标 3");
    CHECK(!zh_dev_load(&D, 4, &r, 1000), "D1 同一个 IEEE 再加载：拒");
    r.ver = 2;
    memcpy(r.ieee, EB, 8);
    CHECK(!zh_dev_load(&D, 5, &r, 1000), "D1 版本不认：拒");
    r.ver = ZH_DEV_REC_VER;
    memset(r.ieee, 0, 8);
    CHECK(!zh_dev_load(&D, 5, &r, 1000), "D1 全 0 IEEE：拒");
    memcpy(r.ieee, EB, 8);
    CHECK(!zh_dev_load(&D, ZH_DEV_MAX, &r, 1000) && !zh_dev_load(&D, 3, &r, 1000), "D1 越界 / 下标占了：拒");
    CHECK(s_saves == 0 && s_ev[ZH_DEV_EV_ADDED] == 0, "D1 加载不落盘、不发事件");
    CHECK(!D.dev[3].seen && D.dev[3].t_seen == 1000, "D1 加载的记录：没见过、可用性从加载时刻起算");

    /* ---- D2 ---- */
    fresh();
    zh_dev_on_rx(&D, 0xCAF5, -27, 255, 1000);
    CHECK(s_nsent == 1 && D.st.unknown_rx == 1 && D.st.lookups == 1, "D2 不认识的 0xCAF5：发了一条");
    {
        const uint8_t want[7] = { 0x00, 0x00, 0x01, 0x61, 0x00, 0xF5, 0xCA };   /* lookupEui64ByNodeId(0xCAF5) */
        CHECK(s_frame_n[0] == 7 && !memcmp(s_frame[0] + 1, want + 1, 6) && s_idem[0] && (s_tag[0] & 0xF) == ZH_DEV_TAG_KIND_LOOKUP,
              "D2 问 NCP 地址表：帧逐字节对、幂等、tag 种类对");
    }
    zh_dev_on_rx(&D, 0xCAF5, -27, 255, 5000);
    zh_dev_on_rx(&D, 0xCAF5, -27, 255, 10999);
    CHECK(s_nsent == 1, "D2 结果没回 / 10 s 内：不重问");
    lookup_rsp(s_tag[0], 0, EA, 11000);
    CHECK(zh_dev_count(&D) == 1 && zh_dev_find_nwk(&D, 0xCAF5) >= 0 && s_saves == 1 && s_ev[ZH_DEV_EV_ADDED] == 1 && s_ev_src[ZH_DEV_SRC_PASSIVE] == 1 &&
          D.st.lookup_hits == 1, "D2 问到了：建记录、落盘一次、事件 ADDED（被动学习）");
    const int ia = zh_dev_find_ieee(&D, EA);
    CHECK(D.dev[ia].seen && D.dev[ia].rssi == -27 && D.dev[ia].lqi == 255 && D.dev[ia].rx == 1 && D.dev[ia].t_seen == 11000,
          "D2 触发查询的那条消息记进记录（信号 -27 / 255、计数 1）");
    zh_dev_on_rx(&D, 0xCAF5, -40, 200, 12000);
    CHECK(s_saves == 1 && D.dev[ia].seen && D.dev[ia].t_seen == 12000 && D.dev[ia].rssi == -40 && D.dev[ia].lqi == 200 && D.dev[ia].rx == 2,
          "D2 之后的消息：只更新内存（last_seen / 信号），不落盘");

    /* ---- D3 ---- */
    fresh();
    zh_dev_on_rx(&D, 0x4321, -50, 180, 1000);
    lookup_rsp(s_tag[0], 0x0C1Eu, (const uint8_t[8]){ 0 }, 1010);      /* NCP 表里没有 */
    CHECK(s_nsent == 2 && D.st.ieee_reqs == 1 && !s_idem[1] && (s_tag[1] & 0xF) == ZH_DEV_TAG_KIND_IEEE, "D3 表里没有：发 IEEE_addr_req（不幂等）");
    {
        /* sendUnicast：type 0、目的 0x4321、APS{0, 0x0001, 0→0, 选项 0x0040（RETRY，SDK device-table-tracking.c:124 同类先例）, 组 0, 序号 0}、
           tag = ZDO 序号 0x21、5 字节 [0x21, 21 43, 0, 0] */
        const uint8_t want[] = { 0x00, 0x21, 0x43, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x21, 0x00, 0x05, 0x21, 0x21, 0x43, 0x00, 0x00 };
        CHECK(s_frame_n[1] == 5 + sizeof(want) && !memcmp(s_frame[1] + 5, want, sizeof(want)) && s_frame[1][3] == 0x34, "D3 IEEE_addr_req 的 sendUnicast 逐字节对");
    }
    send_rsp(s_tag[1], 0, 1020);
    ieee_rsp(0x22, 0, EB, 0x4321, 1500);                               /* 序号不对 */
    CHECK(zh_dev_count(&D) == 0, "D3 序号对不上的应答不认");
    ieee_rsp(0x21, 0, EB, 0x4321, 1600);
    CHECK(zh_dev_count(&D) == 1 && zh_dev_find_ieee(&D, EB) >= 0 && D.st.ieee_rsps == 1 && D.dev[zh_dev_find_ieee(&D, EB)].seen, "D3 应答对上：建记录");
    fresh();
    zh_dev_on_rx(&D, 0x5555, 0, 0, 1000);
    lookup_rsp(s_tag[0], 0x0C1Eu, (const uint8_t[8]){ 0 }, 1010);
    send_rsp(s_tag[1], 0, 1020);
    zh_dev_tick(&D, 1010 + ZH_DEV_IEEE_WAIT_MS - 1);
    CHECK(D.look[0].stage == 2, "D3 等应答：还没到期");
    zh_dev_tick(&D, 1010 + ZH_DEV_IEEE_WAIT_MS);
    CHECK(D.look[0].stage == 0 && D.st.learn_fails == 1 && zh_dev_count(&D) == 0, "D3 %u ms 没应答：放掉、记一次没学到", ZH_DEV_IEEE_WAIT_MS);
    zh_dev_on_rx(&D, 0x5555, 0, 0, 1000 + ZH_DEV_LOOKUP_GAP_MS + 1);
    CHECK(s_nsent == 3, "D3 间隔过了它再说话：重新问");
    fresh();
    zh_dev_on_rx(&D, 0x6666, 0, 0, 1000);
    lookup_rsp(s_tag[0], 0x0C1Eu, (const uint8_t[8]){ 0 }, 1010);
    send_rsp(s_tag[1], 0x0002, 1020);                                   /* sendUnicast 被拒 */
    CHECK(D.look[0].stage == 0 && D.st.learn_fails == 1, "D3 IEEE_addr_req 没发出去：放掉");

    /* ---- D4 ---- */
    fresh();
    zh_dev_on_tc_join(&D, 0x7001, EA, ZH_JOIN_UNSECURED_JOIN, ZH_JOIN_DENY, 0x0000, 1000);
    CHECK(zh_dev_count(&D) == 0 && s_saves == 0, "D4 拒绝的入网：不建");
    zh_dev_on_tc_join(&D, 0x7001, EA, ZH_JOIN_UNSECURED_JOIN, ZH_JOIN_NO_ACTION, 0x0000, 1000);
    CHECK(zh_dev_count(&D) == 0, "D4 新入网却 NO_ACTION：不算放行、不建");
    zh_dev_on_tc_join(&D, 0x7001, EA, ZH_JOIN_UNSECURED_JOIN, ZH_JOIN_USE_PRECONFIGURED_KEY, 0x0000, 1000);
    const int i4 = zh_dev_find_ieee(&D, EA);
    CHECK(i4 >= 0 && D.dev[i4].rec.nwk == 0x7001 && D.dev[i4].seen && s_ev_src[ZH_DEV_SRC_JOIN] == 1 && s_saves == 1, "D4 放行的入网：建记录（来源 JOIN）");
    zh_dev_on_tc_join(&D, 0x7002, EA, ZH_JOIN_SECURED_REJOIN, ZH_JOIN_NO_ACTION, 0x0000, 2000);
    CHECK(D.dev[i4].rec.nwk == 0x7002 && s_ev[ZH_DEV_EV_NWK] == 1 && s_saves == 2 && zh_dev_find_nwk(&D, 0x7001) < 0, "D4 重新入网换了短地址：更新、落盘、事件 NWK");
    zh_dev_on_tc_join(&D, 0x7002, EA, ZH_JOIN_DEVICE_LEFT, ZH_JOIN_NO_ACTION, 0x0000, 3000);
    CHECK((D.dev[i4].rec.flags & ZH_DEVF_LEFT) && s_ev[ZH_DEV_EV_LEFT] == 1 && s_saves == 3 && zh_dev_count(&D) == 1, "D4 离开：标 left、记录还在");
    zh_dev_on_tc_join(&D, 0x7002, EA, ZH_JOIN_DEVICE_LEFT, ZH_JOIN_NO_ACTION, 0x0000, 3100);
    CHECK(s_ev[ZH_DEV_EV_LEFT] == 1 && s_saves == 3, "D4 再报离开：不重复");
    const int n4 = s_nsent;
    zh_dev_on_rx(&D, 0x7002, -60, 150, 4000);                          /* 标过 left 的旧地址发来消息：不直接认，问它是谁 */
    CHECK(s_nsent == n4 + 1, "D4 left 设备的旧地址发来消息：去查");
    lookup_rsp(s_tag[n4], 0, EA, 4010);
    CHECK(!(D.dev[i4].rec.flags & ZH_DEVF_LEFT) && s_ev[ZH_DEV_EV_BACK] == 1 && zh_dev_count(&D) == 1, "D4 查出来还是它：回来了（清 left、事件 BACK）");

    /* ---- D5 ---- */
    fresh();
    annce(0x8001, EB, ZH_CAP_MAINS | ZH_CAP_RX_ON_IDLE | ZH_CAP_FFD, 1000);
    const int i5 = zh_dev_find_ieee(&D, EB);
    CHECK(i5 >= 0 && (D.dev[i5].rec.flags & (ZH_DEVF_MAINS | ZH_DEVF_RX_ON | ZH_DEVF_ANNCED)) == (ZH_DEVF_MAINS | ZH_DEVF_RX_ON | ZH_DEVF_ANNCED) &&
          D.dev[i5].rec.node_type == ZH_NODE_ROUTER && s_ev_src[ZH_DEV_SRC_ANNCE] == 1 && s_nsent == 0, "D5 宣告：建记录、能力进标志、路由器，不用去查");
    const int sv = s_saves;
    annce(0x8001, EB, ZH_CAP_MAINS | ZH_CAP_RX_ON_IDLE | ZH_CAP_FFD, 2000);
    CHECK(s_saves == sv, "D5 一样的宣告：不重写");
    annce(0x8001, EB, 0x80, 3000);                                      /* 变成休眠终端 */
    CHECK(s_saves == sv + 1 && !(D.dev[i5].rec.flags & (ZH_DEVF_MAINS | ZH_DEVF_RX_ON)) && D.dev[i5].rec.node_type == ZH_NODE_END_DEVICE, "D5 能力变了：重写");
    annce(0x8001, EB, ZH_CAP_MAINS, 3100);
    CHECK((D.dev[i5].rec.flags & (ZH_DEVF_MAINS | ZH_DEVF_RX_ON)) == ZH_DEVF_MAINS, "D5 只有市电位：MAINS、不带 RX_ON");
    annce(0x8001, EB, ZH_CAP_RX_ON_IDLE, 3200);
    CHECK((D.dev[i5].rec.flags & (ZH_DEVF_MAINS | ZH_DEVF_RX_ON)) == ZH_DEVF_RX_ON, "D5 只有空闲接收位：RX_ON、不带 MAINS");
    zh_dev_on_rx(&D, 0x9001, 0, 0, 1000);
    const int n5 = s_nsent;
    annce(0x9001, EA, 0x80, 1100);                                      /* 正在查的那个 nwk 自己宣告了 */
    lookup_rsp(s_tag[n5 - 1], 0, EA, 1200);
    CHECK(zh_dev_count(&D) == 2 && D.st.lookup_hits == 0, "D5 查到一半它宣告了：直接认，查询结果作废");

    /* ---- D6 ---- */
    fresh();
    annce(0x1111, EA, 0x80, 1000);
    annce(0x1111, EB, 0x80, 2000);                                      /* B 拿到了 A 以前的短地址 */
    CHECK(D.dev[zh_dev_find_ieee(&D, EA)].rec.nwk == 0xFFFF && D.dev[zh_dev_find_ieee(&D, EB)].rec.nwk == 0x1111 && zh_dev_find_nwk(&D, 0x1111) == zh_dev_find_ieee(&D, EB),
          "D6 短地址冲突：旧的那条作废（0xFFFF），新的拿到");

    /* ---- D7 ---- */
    fresh();
    for (unsigned k = 0; k < ZH_DEV_MAX; k++) {
        uint8_t e[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, (uint8_t)(k >> 8), (uint8_t)(k + 1) };
        annce((uint16_t)(0x2000 + k), e, 0x80, 1000);
    }
    CHECK(zh_dev_count(&D) == ZH_DEV_MAX && D.st.full == 0, "D7 装满 %u 台", ZH_DEV_MAX);
    annce(0x3000, EB, 0x80, 1000);
    CHECK(zh_dev_count(&D) == ZH_DEV_MAX && D.st.full == 1 && zh_dev_find_ieee(&D, EB) < 0, "D7 满了：不建、记一笔");
    fresh();
    annce(0x3000, (const uint8_t[8]){ 0 }, 0x80, 1000);
    annce(0x3001, (const uint8_t[8]){ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }, 0x80, 1000);
    zh_dev_on_tc_join(&D, 0x3002, (const uint8_t[8]){ 0 }, ZH_JOIN_UNSECURED_JOIN, 0, 0, 1000);
    CHECK(zh_dev_count(&D) == 0 && D.st.bad_eui == 3, "D7 全 0 / 全 FF 的 IEEE：不认");
    zh_dev_on_rx(&D, 0x0000, 0, 0, 1000);
    zh_dev_on_rx(&D, 0xFFFD, 0, 0, 1000);
    CHECK(s_nsent == 0, "D7 协调器 / 广播地址：不查");

    /* ---- D8 ---- */
    fresh();
    s_send_ok = false;
    zh_dev_on_rx(&D, 0x4444, 0, 0, 1000);
    CHECK(D.st.send_busy == 1 && D.look[0].stage == 0, "D8 应用命令队列满：放掉这一格");
    s_send_ok = true;
    zh_dev_on_rx(&D, 0x4444, 0, 0, 2000);
    CHECK(s_nsent == 0, "D8 间隔没到：不重问（下次它说话再来）");
    zh_dev_on_rx(&D, 0x4445, 0, 0, 3000);
    zh_dev_tick(&D, 3000 + ZH_DEV_LOOKUP_WAIT_MS - 1);
    CHECK(D.look[0].stage == 1, "D8 问 NCP 地址表的结果没回：还没到期");
    zh_dev_tick(&D, 3000 + ZH_DEV_LOOKUP_WAIT_MS);
    CHECK(D.look[0].stage == 0 && D.st.learn_fails == 1, "D8 %u ms 结果没回（链路复位时被吞）：放掉这一格", ZH_DEV_LOOKUP_WAIT_MS);
    lookup_rsp(s_tag[0], 0, EA, 9000);
    CHECK(zh_dev_count(&D) == 0, "D8 过期之后才回的结果：不认");
    /* 应用命令排在队尾，5 s 后才执行：这一格早被下一个 nwk 占了。上一轮迟到的结果不能安到新 nwk 头上（审计前自查发现） */
    fresh();
    zh_dev_on_rx(&D, 0x4446, 0, 0, 1000);
    zh_dev_tick(&D, 1000 + ZH_DEV_LOOKUP_WAIT_MS);
    zh_dev_on_rx(&D, 0x4447, 0, 0, 1000 + ZH_DEV_LOOKUP_WAIT_MS + 1);
    CHECK(s_nsent == 2 && (s_tag[0] & 0xF0) == (s_tag[1] & 0xF0) && s_tag[0] != s_tag[1] && D.look[0].stage == 1 && D.look[0].nwk == 0x4447,
          "D8 同一格换了 nwk：tag 的代号不同");
    lookup_rsp(s_tag[0], 0, EA, 7000);                                  /* 0x4446 那一轮的结果（EA）迟到 */
    CHECK(zh_dev_count(&D) == 0 && D.look[0].stage == 1, "D8 上一轮迟到的结果：不安到新 nwk 上，这一格接着等自己的");
    lookup_rsp(s_tag[1], 0, EB, 7010);
    CHECK(zh_dev_count(&D) == 1 && zh_dev_find_nwk(&D, 0x4447) >= 0 && !memcmp(D.dev[zh_dev_find_nwk(&D, 0x4447)].rec.ieee, EB, 8),
          "D8 这一轮自己的结果：照常认");

    /* ---- D9 ---- */
    fresh();
    for (unsigned k = 0; k < ZH_DEV_LOOK_N + 3; k++) zh_dev_on_rx(&D, (uint16_t)(0x5000 + k), 0, 0, 1000);
    CHECK(s_nsent == (int)ZH_DEV_LOOK_N, "D9 同时查的最多 %u 个（来了 %u 个不认识的）", ZH_DEV_LOOK_N, ZH_DEV_LOOK_N + 3);

    /* ---- D10 ---- */
    fresh();
    {
        uint8_t buf[16];
        const zh_ezsp_frame_t fr = mkrsp(ZH_EZSP_ID_NOP, NULL, 0, buf);
        CHECK(!zh_dev_on_app_result(&D, 0x8, ZH_SES_R_OK, &fr, 1000) && !zh_dev_on_app_result(&D, ZH_DEV_TAG_KIND_LOOKUP | (ZH_DEV_LOOK_N << 4), ZH_SES_R_OK, &fr, 1000),
              "D10 不是自己的 tag / 槽号越界：回 false");
    }
    zh_dev_init(NULL, NULL);
    CHECK(zh_dev_find_ieee(NULL, EA) == -1 && zh_dev_count(NULL) == 0, "D10 NULL 不崩");

    /* ---- D11 ---- 变异测试存活的几处补的 */
    fresh();
    zh_dev_on_tc_join(&D, 0x7301, EA, ZH_JOIN_SECURED_REJOIN, ZH_JOIN_DENY, 0x0000, 1000);
    zh_dev_on_tc_join(&D, 0x7301, EA, ZH_JOIN_UNSECURED_REJOIN, ZH_JOIN_DENY, 0x0000, 1000);
    CHECK(zh_dev_count(&D) == 0, "D11 拒绝的重新入网：也不建");
    fresh();
    zh_dev_on_rx(&D, 0x7401, 0, 0, 1000);
    lookup_rsp(s_tag[0], 0x0C1Eu, (const uint8_t[8]){ 0 }, 1010);
    send_rsp(s_tag[1], 0, 1020);
    ieee_rsp(0x21, 0x81, EB, 0x7401, 1100);                            /* 状态 DEVICE_NOT_FOUND，地址字段却像样 */
    CHECK(zh_dev_count(&D) == 0 && D.look[0].stage == 0 && D.st.learn_fails == 1, "D11 IEEE_addr_rsp 状态非 0：不认、放掉");
    fresh();
    zh_dev_on_rx(&D, 0x7501, 0, 0, 1000);
    zh_dev_on_app_result(&D, s_tag[0], ZH_SES_R_TIMEOUT, NULL, 1010);
    zh_dev_on_rx(&D, 0x7502, 0, 0, 1000);
    zh_dev_on_app_result(&D, s_tag[1], ZH_SES_R_UNKNOWN, NULL, 1010);
    CHECK(s_nsent == 2 && D.look[0].stage == 0 && D.look[1].stage == 0 && D.st.ieee_reqs == 0 && D.st.learn_fails == 2,
          "D11 问 NCP 地址表超时 / 结果未知：放掉，不去发 IEEE_addr_req");
    fresh();
    zh_dev_on_rx(&D, 0x7601, 0, 0, 1000);
    zh_dev_on_tc_join(&D, 0x7601, EA, ZH_JOIN_SECURED_REJOIN, ZH_JOIN_NO_ACTION, 0x0000, 1100);
    CHECK(D.look[0].stage == 0, "D11 正在查的 nwk 入网了：这一格放掉");
    lookup_rsp(s_tag[0], 0, EB, 1200);                                 /* 迟到的结果说是 EB */
    CHECK(zh_dev_count(&D) == 1 && zh_dev_find_ieee(&D, EB) < 0 && D.st.lookup_hits == 0, "D11 入网后迟到的查询结果：作废");
    fresh();
    zh_dev_on_rx(&D, 0x7701, 0, 0, 1000);
    lookup_rsp(s_tag[0], 0x0C1Eu, EA, 1010);                           /* 状态说没有、地址字段却像样 */
    CHECK(zh_dev_count(&D) == 0 && s_nsent == 2 && D.st.ieee_reqs == 1, "D11 地址表状态非 0：不认它带的地址，去问本人");

    /* ---- D12 ---- 第二轮审计补的：读不出来的格子保留、表满不发空口查询、年龄封顶 */
    fresh();
    CHECK(zh_dev_reserve(&D, 0) && zh_dev_reserve(&D, 1) && !zh_dev_reserve(&D, 1) && !zh_dev_reserve(&D, ZH_DEV_MAX) && D.st.reserved == 2,
          "D12 保留第 0、1 格（重复 / 越界拒）");
    memset(&r, 0, sizeof(r));
    r.ver = ZH_DEV_REC_VER;
    memcpy(r.ieee, EB, 8);
    CHECK(!zh_dev_load(&D, 0, &r, 1000), "D12 保留的格子不加载");
    annce(0x1201, EA, 0x80, 1000);
    CHECK(zh_dev_find_ieee(&D, EA) == 2 && zh_dev_count(&D) == 1, "D12 新设备落在第 2 格（不进保留的 0、1）");
    fresh();
    for (unsigned k = 0; k < ZH_DEV_MAX - 1; k++) {
        uint8_t e[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x07, (uint8_t)(k >> 8), (uint8_t)(k + 1) };
        annce((uint16_t)(0x2000 + k), e, 0x80, 1000);
    }
    zh_dev_reserve(&D, ZH_DEV_MAX - 1);            /* 最后一格读不出来：表实际满了 */
    zh_dev_on_rx(&D, 0x3333, 0, 0, 1000);
    lookup_rsp(s_tag[0], 0x0C1Eu, (const uint8_t[8]){ 0 }, 1010);      /* NCP 表里没有 */
    CHECK(s_nsent == 1 && D.st.ieee_reqs == 0 && D.st.full == 1 && D.look[0].stage == 0, "D12 表满：不发空口 IEEE_addr_req、记一笔 full");
    zh_dev_on_rx(&D, 0x3334, 0, 0, 1000);
    lookup_rsp(s_tag[1], 0, EB, 1010);                                  /* 地址表查到了，可没地方放 */
    CHECK(zh_dev_count(&D) == ZH_DEV_MAX - 1 && zh_dev_find_ieee(&D, EB) < 0 && D.st.full == 2, "D12 表满：地址表查到了也不建（保留格不动）");
    fresh();
    annce(0x1301, EA, 0x80, 1000);
    const int i12 = zh_dev_find_ieee(&D, EA);
    D.dirty = false;
    zh_dev_tick(&D, 1000 + ZH_DEV_AGE_CAP_MS);
    CHECK(D.dev[i12].t_seen == 1000 && !D.dirty, "D12 正好到上限：不动");
    zh_dev_tick(&D, 1000 + ZH_DEV_AGE_CAP_MS + ZH_DEV_AGE_CHK_MS + 5);
    CHECK((uint32_t)(1000 + ZH_DEV_AGE_CAP_MS + ZH_DEV_AGE_CHK_MS + 5 - D.dev[i12].t_seen) == ZH_DEV_AGE_CAP_MS && D.dirty, "D12 超过上限：封在上限、要重发快照");
    {
        const uint32_t ts = D.dev[i12].t_seen;
        zh_dev_tick(&D, 1000 + ZH_DEV_AGE_CAP_MS + ZH_DEV_AGE_CHK_MS + 5 + 1000);
        CHECK(D.dev[i12].t_seen == ts, "D12 封顶检查 %u ms 才做一次（zhub 每圈都调 tick，别每圈扫 64 条）", ZH_DEV_AGE_CHK_MS);
    }
    {   /* 修复复核 ❌：zhub 主循环的时刻取得早，回调里写 t_seen 用的更晚 —— t_seen 比 tick 的 now 晚 1 ms，不能被当成"很老"封掉 */
        zh_dev_core_t D2;
        const zh_dev_ops_t o2 = { op_save, op_event, op_send, op_seq, NULL };
        zh_dev_init(&D2, &o2);
        uint8_t pl[12] = { 0x77, 0x01, 0x14 };
        memcpy(pl + 3, EB, 8);
        pl[11] = 0x80;
        zh_dev_on_zdo(&D2, 0x1401, 0x0013, pl, sizeof(pl), 200001);          /* 回调里取的时刻：200001 */
        zh_dev_tick(&D2, 200000);                                              /* 这一圈开头取的：200000，正赶上封顶检查 */
        const int j = zh_dev_find_ieee(&D2, EB);
        CHECK(j >= 0 && D2.dev[j].t_seen == 200001, "D12 t_seen 比 tick 的时刻晚 1 ms：是刚收到，不封顶（原先会被封成 30 天前、显示离线）");
    }
    {
        uint32_t t = 1000 + ZH_DEV_AGE_CAP_MS + ZH_DEV_AGE_CHK_MS + 5, worst = 0;
        for (int k = 0; k < 4000; k++) {                               /* 往后再走约 66 天（跨过 32 位毫秒的回绕） */
            t += 1440000u;
            zh_dev_tick(&D, t);
            const uint32_t age = t - D.dev[i12].t_seen;
            if (age > worst) worst = age;
        }
        CHECK(worst <= ZH_DEV_AGE_CAP_MS + 1440000u, "D12 静默约 86 天、跨过回绕：年龄一直封在上限附近（最大 %u ms）", (unsigned)worst);
    }

    /* ---- D13 ---- 第一轮审计补的：查询限速、宣告里的短地址、重新入网算"发现"、宣告建记录一次写好 */
    fresh();
    for (unsigned k = 0; k < ZH_DEV_LOOK_N; k++) {                      /* 8 个都很快放掉（地址表超时），间隔记录还都不满 10 s */
        zh_dev_on_rx(&D, (uint16_t)(0x6000 + k), 0, 0, 1000);
        zh_dev_on_app_result(&D, s_tag[k], ZH_SES_R_TIMEOUT, NULL, 1000);
    }
    zh_dev_on_rx(&D, 0x6100, 0, 0, 2000);
    CHECK(s_nsent == (int)ZH_DEV_LOOK_N && D.st.deferred == 1, "D13 10 s 内已开 %u 个查询：第 %u 个先不查（不盖掉还没满 10 s 的间隔记录）", ZH_DEV_LOOK_N, ZH_DEV_LOOK_N + 1);
    zh_dev_on_rx(&D, 0x6000, 0, 0, 5000);
    CHECK(s_nsent == (int)ZH_DEV_LOOK_N, "D13 头一个 nwk 10 s 内再说话：仍不重查");
    zh_dev_on_rx(&D, 0x6100, 0, 0, 1000 + ZH_DEV_LOOKUP_GAP_MS);
    CHECK(s_nsent == (int)ZH_DEV_LOOK_N + 1, "D13 最早那条间隔记录满 10 s：可以开新查询了");
    fresh();
    annce(0x0000, EA, 0x80, 1000);
    annce(0xFFFC, EB, 0x80, 1000);
    CHECK(zh_dev_count(&D) == 0 && D.st.bad_nwk == 2, "D13 宣告里的短地址是协调器 / 广播保留段：不认");
    fresh();
    zh_dev_on_tc_join(&D, 0x7801, EA, ZH_JOIN_SECURED_REJOIN, ZH_JOIN_NO_ACTION, 0x0000, 1000);
    CHECK(zh_dev_count(&D) == 1 && s_ev_src[ZH_DEV_SRC_REJOIN] == 1 && s_ev_src[ZH_DEV_SRC_JOIN] == 0, "D13 不认识的设备重新入网：来源 REJOIN（已在网的，不是新加的）");
    fresh();
    annce(0x7901, EB, ZH_CAP_FFD | ZH_CAP_MAINS | ZH_CAP_RX_ON_IDLE, 1000);
    {
        const int i = zh_dev_find_ieee(&D, EB);
        CHECK(i >= 0 && s_saves == 1 && D.dev[i].rec.node_type == ZH_NODE_ROUTER && (D.dev[i].rec.flags & ZH_DEVF_ANNCED), "D13 宣告建的新记录：一次写好（节点类型 / 能力），只落盘一次");
    }

    /* ================= 访谈（第七步 b） ================= */
    /* ---- D14 ---- 休眠设备完整走一遍 */
    fresh();
    annce(0x1401, EA, 0x80, 1000);                                     /* 休眠终端：能力字节已知、空闲不收 */
    const int i14 = zh_dev_find_ieee(&D, EA);
    const int sv14 = s_saves;
    zh_dev_tick(&D, 1000);
    CHECK(s_nsent == 1 && fr_cluster(0) == ZH_ZDO_NODE_DESC_REQ && fr_profile(0) == 0 && fr_dest(0) == 0x1401 && fr_opts(0) == ZH_APS_OPTS_DEFAULT &&
          fr_pl_n(0) == 3 && fr_pl(0)[1] == 0x01 && fr_pl(0)[2] == 0x14 && (s_tag[0] & 0xF) == ZH_DEV_TAG_KIND_IV && !s_idem[0] && D.dev[i14].iv == ZH_IV_NODE,
          "D14 先问节点描述符：ZDO 单播给 0x1401、[序号][nwk]、APS 0x1040、不幂等");
    zh_dev_tick(&D, 1010);
    CHECK(s_nsent == 1, "D14 一条在途：不发第二条");
    send_rsp(s_tag[0], 0, 1010);
    node_rsp(last_seq(), 0, 0x1401, 0x02, 0x80, 0x115F, 1100);
    spoke(0x1401, 1100);
    CHECK(D.dev[i14].iv == ZH_IV_ACTIVE && D.dev[i14].rec.manuf_code == 0x115F && D.dev[i14].rec.node_type == ZH_NODE_END_DEVICE && D.iv_busy == 0 &&
          (D.dev[i14].rec.flags & ZH_DEVF_NODE_DESC), "D14 节点描述符：厂商码（标\"拿到了\"）、终端；下一步问端点");
    zh_dev_tick(&D, 1100);
    CHECK(s_nsent == 2 && fr_cluster(1) == ZH_ZDO_ACTIVE_EP_REQ, "D14 问活动端点");
    active_rsp(last_seq(), 0, 0x1401, (const uint8_t[3]){ 1, 242, 2 }, 3, 1200);
    spoke(0x1401, 1200);
    CHECK(D.dev[i14].iv == ZH_IV_SIMPLE && D.dev[i14].iv_n_eps == 2 && D.dev[i14].iv_eps[1] == 2, "D14 端点 1、2（242 Green Power 不问）");
    zh_dev_tick(&D, 1200);
    CHECK(s_nsent == 3 && fr_cluster(2) == ZH_ZDO_SIMPLE_DESC_REQ && fr_pl_n(2) == 4 && fr_pl(2)[3] == 1, "D14 问端点 1 的简单描述符：[序号][nwk][端点]");
    simple_rsp(last_seq(), 0, 0x1401, 1, 0x0104, 0x0402, (const uint16_t[4]){ 0x0000, 0x0001, 0x0003, 0x0500 }, 4, (const uint16_t[1]){ 0x0019 }, 1, 1300);
    spoke(0x1401, 1300);
    zh_dev_tick(&D, 1300);
    CHECK(s_nsent == 4 && fr_cluster(3) == ZH_ZDO_SIMPLE_DESC_REQ && fr_pl(3)[3] == 2, "D14 再问端点 2");
    simple_rsp(last_seq(), 0x83, 0x1401, 2, 0, 0, NULL, 0, NULL, 0, 1400);   /* NOT_ACTIVE：跳过 */
    spoke(0x1401, 1400);
    {
        const zh_dev_rec_t *r = &D.dev[i14].rec;
        CHECK(r->n_ep == 1 && r->ep[0].ep == 1 && r->ep[0].profile == 0x0104 && r->ep[0].devid == 0x0402 && r->ep[0].n_in == 4 && r->ep[0].n_out == 1 &&
              r->ep[0].cl[3] == 0x0500 && r->ep[0].cl[4] == 0x0019 && D.dev[i14].iv == ZH_IV_BASIC,
              "D14 端点 1 记下（簇先输入后输出），端点 2 状态非 0 跳过；下一步读 Basic");
    }
    zh_dev_tick(&D, 1400);
    {
        const uint8_t want[] = { 0x10, 0x00, 0x00, 0x04, 0x00, 0x05, 0x00 };   /* ZCL 序号从 0 起 */
        CHECK(s_nsent == 5 && fr_profile(4) == ZH_HA_PROFILE && fr_cluster(4) == ZH_CL_BASIC && fr_dst_ep(4) == 1 && s_frame[4][12] == ZH_DEV_SRC_EP &&
              fr_opts(4) == ZH_APS_OPTS_DEFAULT && fr_pl_n(4) == sizeof(want) && !memcmp(fr_pl(4), want, sizeof(want)),
              "D14 读 Basic 厂商 + 型号（两个一组）：HA、端点 1→1、帧逐字节");
        const uint8_t recs[] = { 0x04, 0x00, 0x00, 0x42, 0x04, 'L', 'U', 'M', 'I',
                                 0x05, 0x00, 0x00, 0x42, 0x16, 'l', 'u', 'm', 'i', '.', 's', 'e', 'n', 's', 'o', 'r', '_', 'm', 'o', 't', 'i', 'o', 'n', '.', 'a', 'q', '2' };
        zcl_rsp(0x1401, ZH_CL_BASIC, last_seq(), recs, sizeof(recs), 1500);
        spoke(0x1401, 1500);
    }
    CHECK(!strcmp(D.dev[i14].rec.manuf, "LUMI") && !strcmp(D.dev[i14].rec.model, "lumi.sensor_motion.aq2") && D.dev[i14].iv_want == (ZH_IV_W_POWER | ZH_IV_W_SWB),
          "D14 厂商 / 型号记下");
    zh_dev_tick(&D, 1500);
    CHECK(s_nsent == 6 && fr_pl(5)[3] == 0x07 && fr_pl(5)[4] == 0x00 && fr_pl(5)[5] == 0x00 && fr_pl(5)[6] == 0x40, "D14 再读供电 + 软件版本");
    {
        const uint8_t recs[] = { 0x07, 0x00, 0x00, 0x30, 0x03, 0x00, 0x40, 0x00, 0x42, 0x03, '1', '.', '2' };
        zcl_rsp(0x1401, ZH_CL_BASIC, last_seq(), recs, sizeof(recs), 1600);
        spoke(0x1401, 1600);
    }
    CHECK(D.dev[i14].iv == ZH_IV_ZONE && !(D.dev[i14].rec.flags & ZH_DEVF_MAINS) && D.dev[i14].rec.sw_build_crc != 0, "D14 电池供电、软件版本指纹；有 IAS Zone：读 ZoneType");
    zh_dev_tick(&D, 1600);
    CHECK(s_nsent == 7 && fr_cluster(6) == ZH_CL_IAS_ZONE && fr_pl(6)[3] == 0x01 && fr_pl(6)[4] == 0x00, "D14 读 IAS Zone 的 ZoneType");
    zcl_rsp(0x1401, ZH_CL_IAS_ZONE, last_seq(), (const uint8_t[6]){ 0x01, 0x00, 0x00, 0x31, 0x0D, 0x00 }, 6, 1700);
    CHECK((D.dev[i14].rec.flags & ZH_DEVF_INTERVIEWED) && D.dev[i14].rec.zone_type == 0x000D && D.dev[i14].iv == ZH_IV_NONE && s_saves == sv14 + 1 &&
          s_ev[ZH_DEV_EV_IDENTIFIED] == 1 && s_last_src == ZH_DEV_SRC_PASSIVE && D.st.iv_done == 1 && D.iv_busy == 0,
          "D14 访谈完：标 INTERVIEWED、ZoneType 0x000D（人体）、落盘一次、事件 IDENTIFIED（不是新入网的）");
    spoke(0x1401, 1700);
    zh_dev_tick(&D, 1700);
    CHECK(s_nsent == 8 && fr_cluster(7) == ZH_CL_POWER_CFG && fr_pl(7)[2] == ZH_ZCL_CMD_READ_ATTR, "D14 访谈完：不再发访谈请求（只补读一次电量，M5 第六步）");

    /* ---- D15 ---- 休眠设备没醒不发 */
    fresh();
    annce(0x1501, EA, 0x80, 1000);
    zh_dev_tick(&D, 1000 + ZH_IV_AWAKE_MS);
    CHECK(s_nsent == 0, "D15 休眠设备静默 %u ms 之后：不发（它听不见）", ZH_IV_AWAKE_MS);
    spoke(0x1501, 9000);
    zh_dev_tick(&D, 9000);
    CHECK(s_nsent == 1 && fr_cluster(0) == ZH_ZDO_NODE_DESC_REQ, "D15 它说话了：马上发");

    /* ---- D16 ---- 超时重试到失败 */
    fresh();
    annce(0x1601, EA, 0x80, 1000);
    const int i16 = zh_dev_find_ieee(&D, EA);
    const int sv16 = s_saves;
    uint32_t t = 1000;
    for (int k = 0; k < (int)ZH_IV_TRIES; k++) {
        spoke(0x1601, t);
        zh_dev_tick(&D, t);
        CHECK(s_nsent == k + 1, "D16 第 %d 次发", k + 1);
        t += ZH_IV_STEP_TIMEOUT_MS;
        zh_dev_tick(&D, t);                                            /* 超时（它 5 s 前说的话，现在听不见：这一拍不重发） */
        CHECK(s_nsent == k + 1, "D16 第 %d 次超时：不当场重发", k + 1);
    }
    CHECK(D.dev[i16].iv == ZH_IV_FAILED && s_ev[ZH_DEV_EV_IV_FAILED] == 1 && D.st.iv_timeouts == ZH_IV_TRIES && !(D.dev[i16].rec.flags & ZH_DEVF_INTERVIEWED) &&
          s_saves == sv16, "D16 %u 次都没回：失败、事件、不落盘", ZH_IV_TRIES);
    spoke(0x1601, t + 10);
    zh_dev_tick(&D, t + 10);
    CHECK(s_nsent == (int)ZH_IV_TRIES, "D16 失败了这次开机不再问");

    /* ---- D17 ---- Basic 回 0x89 / 缺属性 → 改单读 */
    fresh();
    annce(0x1701, EA, 0x80, 1000);
    const int i17 = zh_dev_find_ieee(&D, EA);
    t = 1000;
    auto_answer(i17, 0x1701, &t, false, ZH_IV_BASIC);
    CHECK(D.dev[i17].iv == ZH_IV_BASIC, "D17 走到 Basic");
    spoke(0x1701, t);
    zh_dev_tick(&D, t);
    zcl_rsp(0x1701, ZH_CL_BASIC, last_seq(), (const uint8_t[]){ 0x04, 0x00, 0x00, 0x42, 0x04, 'A', 'C', 'M', 'E', 0x05, 0x00, 0x89 }, 12, t + 10);
    CHECK(D.dev[i17].iv_single && D.dev[i17].iv_want == (ZH_IV_W_MODEL | ZH_IV_W_POWER | ZH_IV_W_SWB) && !strcmp(D.dev[i17].rec.manuf, "ACME"),
          "D17 型号回 0x89（装不下）：厂商记下、改单读");
    spoke(0x1701, t + 10);
    zh_dev_tick(&D, t + 10);
    CHECK(fr_pl_n(last()) == 5 && fr_pl(last())[3] == 0x05, "D17 单读型号");
    zcl_rsp(0x1701, ZH_CL_BASIC, last_seq(), (const uint8_t[]){ 0x05, 0x00, 0x00, 0x42, 0x02, 'X', '1' }, 7, t + 20);
    spoke(0x1701, t + 20);
    zh_dev_tick(&D, t + 20);
    CHECK(fr_pl_n(last()) == 5 && fr_pl(last())[3] == 0x07 && !strcmp(D.dev[i17].rec.model, "X1"), "D17 单读供电");
    zcl_rsp(0x1701, ZH_CL_BASIC, last_seq(), (const uint8_t[]){ 0x07, 0x00, 0x86 }, 3, t + 30);
    spoke(0x1701, t + 30);
    zh_dev_tick(&D, t + 30);
    CHECK(fr_pl_n(last()) == 5 && fr_pl(last())[3] == 0x00 && fr_pl(last())[4] == 0x40, "D17 供电不支持（0x86）不是错：单读软件版本");
    zcl_rsp(0x1701, ZH_CL_BASIC, last_seq(), (const uint8_t[]){ 0x00, 0x40, 0x86 }, 3, t + 40);
    CHECK((D.dev[i17].rec.flags & ZH_DEVF_INTERVIEWED) && D.dev[i17].rec.zone_type == ZH_ZONE_TYPE_UNKNOWN && D.dev[i17].rec.sw_build_crc == 0,
          "D17 没有 IAS Zone：读完 Basic 就完成，ZoneType 留未知");
    fresh();
    annce(0x1702, EB, 0x80, 1000);
    const int i17b = zh_dev_find_ieee(&D, EB);
    t = 1000;
    auto_answer(i17b, 0x1702, &t, false, ZH_IV_BASIC);
    spoke(0x1702, t);
    zh_dev_tick(&D, t);
    zcl_rsp(0x1702, ZH_CL_BASIC, last_seq(), (const uint8_t[]){ 0x04, 0x00, 0x00, 0x42, 0x01, 'Z' }, 6, t + 10);   /* 型号问了没回 */
    CHECK(D.dev[i17b].iv_single && (D.dev[i17b].iv_want & ZH_IV_W_MODEL) && !(D.dev[i17b].iv_want & ZH_IV_W_MANUF), "D17 问了的型号没在应答里：改单读");

    /* ---- D18 ---- 默认应答拒读 */
    fresh();
    annce(0x1801, EA, 0x80, 1000);
    const int i18 = zh_dev_find_ieee(&D, EA);
    t = 1000;
    auto_answer(i18, 0x1801, &t, false, ZH_IV_BASIC);
    spoke(0x1801, t);
    zh_dev_tick(&D, t);
    zcl_default_rsp(0x1801, ZH_CL_BASIC, last_seq(), 0xC3, t + 10);   /* UNSUPPORTED_CLUSTER */
    CHECK(D.dev[i18].iv_single && D.dev[i18].iv_want == (ZH_IV_W_MANUF | ZH_IV_W_MODEL | ZH_IV_W_POWER | ZH_IV_W_SWB) && D.iv_busy == 0, "D18 两个一组被默认应答拒：改单读");
    spoke(0x1801, t + 10);
    zh_dev_tick(&D, t + 10);
    zcl_default_rsp(0x1801, ZH_CL_BASIC, last_seq(), 0xC3, t + 20);
    CHECK(D.dev[i18].iv_want == (ZH_IV_W_MODEL | ZH_IV_W_POWER | ZH_IV_W_SWB), "D18 单读也被拒：放弃这一个（厂商），接着问下一个");
    zcl_default_rsp(0x1801, ZH_CL_BASIC, 0x77, 0xC3, t + 25);                                      /* 序号不对：不理 */
    CHECK(D.dev[i18].iv_want == (ZH_IV_W_MODEL | ZH_IV_W_POWER | ZH_IV_W_SWB), "D18 序号对不上的默认应答：不理");

    /* ---- D19 ---- 空闲接收的设备随时可问 */
    fresh();
    annce(0x1901, EA, ZH_CAP_FFD | ZH_CAP_MAINS | ZH_CAP_RX_ON_IDLE, 1000);
    zh_dev_tick(&D, 1000 + 600000);
    CHECK(s_nsent == 1 && fr_cluster(0) == ZH_ZDO_NODE_DESC_REQ, "D19 路由器 / 空闲也收的：10 min 没说话也照问");
    fresh();
    zh_dev_on_rx(&D, 0x1902, 0, 0, 1000);                              /* 被动学习：能力未知 */
    lookup_rsp(s_tag[0], 0, EB, 1010);
    zh_dev_tick(&D, 1010);
    CHECK(s_nsent == 2 && fr_cluster(1) == ZH_ZDO_NODE_DESC_REQ, "D19 被动学习刚认出来（它刚说过话）：马上访谈");
    fresh();
    zh_dev_on_rx(&D, 0x1903, 0, 0, 1000);
    lookup_rsp(s_tag[0], 0, EB, 1010);
    zh_dev_tick(&D, 1010 + ZH_IV_AWAKE_MS + 100);
    CHECK(s_nsent == 1, "D19 能力未知（可能是休眠设备）：当休眠的对待，静默了就不发");

    /* ---- D20 ---- 30 min 总期限 */
    fresh();
    annce(0x2001, EA, 0x80, 1000);
    const int i20 = zh_dev_find_ieee(&D, EA);
    zh_dev_tick(&D, 1000);
    node_rsp(last_seq(), 0, 0x2001, 0x02, 0x80, 0x1111, 1100);
    CHECK(D.dev[i20].iv == ZH_IV_ACTIVE, "D20 第一步成了");
    spoke(0x2001, 1000 + ZH_IV_TOTAL_MS + 1);
    const int n20 = s_nsent;
    zh_dev_tick(&D, 1000 + ZH_IV_TOTAL_MS + 1);
    CHECK(D.dev[i20].iv == ZH_IV_FAILED && s_nsent == n20 && s_ev[ZH_DEV_EV_IV_FAILED] == 1, "D20 从第一条请求起超过 30 min 还没完：失败、不再发");

    /* ---- D21 ---- 短地址作废的不发 */
    fresh();
    annce(0x2101, EA, ZH_CAP_FFD | ZH_CAP_RX_ON_IDLE, 1000);
    annce(0x2101, EB, ZH_CAP_FFD | ZH_CAP_RX_ON_IDLE, 1001);          /* EA 的短地址作废（0xFFFF） */
    for (uint32_t k = 0; k < 20; k++) zh_dev_tick(&D, 1001 + k * ZH_IV_STEP_TIMEOUT_MS);
    {
        bool bcast = false;
        for (int k = 0; k < s_nsent && k < 64; k++) bcast |= fr_dest(k) >= 0xFFF8;
        CHECK(s_nsent > 0 && !bcast, "D21 短地址作废（0xFFFF）的设备不访谈：发出去的 %d 条没有一条是广播地址", s_nsent);
    }

    /* ---- D22 ---- 访谈过的设备重新配对：再访谈、算新入网 */
    fresh();
    annce(0x2201, EA, ZH_CAP_FFD | ZH_CAP_RX_ON_IDLE, 1000);
    const int i22 = zh_dev_find_ieee(&D, EA);
    t = 1000;
    auto_answer(i22, 0x2201, &t, false, ZH_IV_NONE);
    CHECK((D.dev[i22].rec.flags & ZH_DEVF_INTERVIEWED) && s_ev[ZH_DEV_EV_IDENTIFIED] == 1 && s_last_src == ZH_DEV_SRC_PASSIVE, "D22 先访谈完一次");
    zh_dev_on_tc_join(&D, 0x2201, EA, ZH_JOIN_SECURED_REJOIN, ZH_JOIN_NO_ACTION, 0, t);
    CHECK(D.dev[i22].iv == ZH_IV_NONE, "D22 重新入网（rejoin）：不重新访谈");
    zh_dev_on_tc_join(&D, 0x2201, EA, ZH_JOIN_UNSECURED_JOIN, ZH_JOIN_USE_PRECONFIGURED_KEY, 0, t);
    CHECK(D.dev[i22].iv == ZH_IV_NODE && D.dev[i22].iv_new && !(D.dev[i22].rec.flags & ZH_DEVF_INTERVIEWED), "D22 重新配对（新入网）：清访谈标志、重新访谈");
    auto_answer(i22, 0x2201, &t, false, ZH_IV_NONE);
    CHECK((D.dev[i22].rec.flags & ZH_DEVF_INTERVIEWED) && s_ev[ZH_DEV_EV_IDENTIFIED] == 2 && s_last_src == ZH_DEV_SRC_JOIN, "D22 再访谈完：事件来源 JOIN（推送给 Muse）");
    fresh();
    zh_dev_on_tc_join(&D, 0x2202, EB, ZH_JOIN_UNSECURED_JOIN, ZH_JOIN_USE_PRECONFIGURED_KEY, 0, 1000);
    const int i22b = zh_dev_find_ieee(&D, EB);
    t = 1000;
    auto_answer(i22b, 0x2202, &t, true, ZH_IV_NONE);
    CHECK((D.dev[i22b].rec.flags & ZH_DEVF_INTERVIEWED) && s_last_src == ZH_DEV_SRC_JOIN && D.dev[i22b].rec.zone_type == 0x0015,
          "D22 新入网的设备访谈完：事件来源 JOIN；IAS 门磁 ZoneType 0x0015");

    /* ---- D23 ---- 迟到 / 不对号的应答、sendUnicast 没发出去 */
    fresh();
    annce(0x2301, EA, 0x80, 1000);
    const int i23 = zh_dev_find_ieee(&D, EA);
    zh_dev_tick(&D, 1000);
    const uint8_t s1 = last_seq();
    const uint32_t tag1 = s_tag[last()];
    zh_dev_tick(&D, 1000 + ZH_IV_STEP_TIMEOUT_MS);                     /* 第一次超时 */
    spoke(0x2301, 7000);
    zh_dev_tick(&D, 7000);
    CHECK(s_nsent == 2 && D.dev[i23].iv_tries == 2, "D23 重发一次");
    const int busy23 = D.iv_busy;
    send_rsp(tag1, 0x0002, 7010);                                       /* 第一次那条的发送结果迟到（还是失败的）：不认 */
    CHECK(D.iv_busy == busy23 && D.dev[i23].iv_tries == 2, "D23 上一条的发送结果迟到：不认（代号对不上）");
    node_rsp((uint8_t)(s1 + 50), 0, 0x2301, 0x02, 0x80, 0x2222, 7020);  /* 没发过的序号 */
    CHECK(D.dev[i23].iv == ZH_IV_NODE, "D23 没发过的序号：不理");
    node_rsp(s1, 0, 0x2301, 0x02, 0x80, 0x2222, 7030);                 /* 第一次那条的应答迟到 */
    CHECK(D.dev[i23].iv == ZH_IV_ACTIVE && D.dev[i23].rec.manuf_code == 0x2222 && D.iv_busy == 0, "D23 上一次那条的应答迟到：照认（休眠设备的父节点缓存着）");
    spoke(0x2301, 7030);
    zh_dev_tick(&D, 7030);
    send_rsp(s_tag[last()], 0x0002, 7040);                              /* sendUnicast 没发出去 */
    CHECK(D.iv_busy != 0 && D.dev[i23].iv_tries == 1 && D.st.iv_send_fail == 1 && D.dev[i23].iv == ZH_IV_ACTIVE,
          "D23 这一条没发出去：算用了一次，但等到期限才重发（不连发：常电设备几百毫秒就会把次数用完，第二轮审计）");
    spoke(0x2301, 7050);
    zh_dev_tick(&D, 7050);
    zcl_rsp(0x2301, ZH_CL_BASIC, last_seq(), (const uint8_t[]){ 0x04, 0x00, 0x86 }, 3, 7060);   /* 不在 Basic 这一步：ZCL 应答不理 */
    CHECK(D.dev[i23].iv == ZH_IV_ACTIVE && D.iv_busy != 0, "D23 不在读属性这一步时来的 ZCL 应答：不理");

    /* ---- D24 ---- 坏的 Simple_Desc_rsp */
    fresh();
    annce(0x2401, EA, 0x80, 1000);
    const int i24 = zh_dev_find_ieee(&D, EA);
    t = 1000;
    auto_answer(i24, 0x2401, &t, false, ZH_IV_SIMPLE);
    spoke(0x2401, t);
    zh_dev_tick(&D, t);
    {
        const uint8_t bad1[] = { last_seq(), 0x00, 0x01, 0x24, 0x14, 0x01, 0x04, 0x01, 0x00, 0x01, 0x01, 0x05 };   /* 长度说 20 B、实际 7 B */
        zh_dev_on_zdo(&D, 0x2401, ZH_ZDO_SIMPLE_DESC_RSP, bad1, sizeof(bad1), t + 10);
        CHECK(D.dev[i24].iv == ZH_IV_SIMPLE && D.st.iv_bad == 1 && D.iv_busy == 0 && D.dev[i24].rec.n_ep == 0, "D24 长度越界：不认、记一笔、这一步再来");
        spoke(0x2401, t + 10);
        zh_dev_tick(&D, t + 10);
        const uint8_t bad2[] = { last_seq(), 0x00, 0x01, 0x24, 0x0A, 0x01, 0x04, 0x01, 0x00, 0x01, 0x01, 0x05, 0x00, 0x00, 0x00 };   /* 输入数 5、只有 2 B */
        zh_dev_on_zdo(&D, 0x2401, ZH_ZDO_SIMPLE_DESC_RSP, bad2, sizeof(bad2), t + 20);
        CHECK(D.st.iv_bad == 2 && D.dev[i24].rec.n_ep == 0, "D24 簇数越界：不认");
        const uint8_t bad3[] = { 0x00, 0x00, 0x01 };
        zh_dev_on_zdo(&D, 0x2401, ZH_ZDO_NODE_DESC_RSP, bad3, sizeof(bad3), t + 30);
        CHECK(D.st.iv_bad == 3, "D24 应答连 [序号][状态][nwk] 都不够：记一笔");
    }

    /* ---- D25 ---- 开机加载的 */
    fresh();
    memset(&r, 0, sizeof(r));
    r.ver = ZH_DEV_REC_VER;
    memcpy(r.ieee, EA, 8);
    r.nwk = 0x2501;
    zh_dev_load(&D, 0, &r, 1000);
    r.flags = ZH_DEVF_INTERVIEWED;
    memcpy(r.ieee, EB, 8);
    r.nwk = 0x2502;
    zh_dev_load(&D, 1, &r, 1000);
    CHECK(D.dev[0].iv == ZH_IV_NODE && D.dev[1].iv == ZH_IV_NONE, "D25 加载的：没访谈过的排上访谈，访谈过的不再访谈");
    zh_dev_tick(&D, 1000);
    CHECK(s_nsent == 0, "D25 开机后它还没说过话：不发（不知道它醒没醒）");
    spoke(0x2502, 2000);
    zh_dev_tick(&D, 2000);
    CHECK(s_nsent == 0, "D25 访谈过的说话：不访谈");
    spoke(0x2501, 3000);
    zh_dev_tick(&D, 3000);
    CHECK(s_nsent == 1 && fr_dest(0) == 0x2501, "D25 没访谈过的说话了：开始访谈");

    /* ---- D26 ---- 变异测试存活的几处补的 */
    fresh();
    annce(0x2601, EA, 0x80, 1000);
    const int i26 = zh_dev_find_ieee(&D, EA);
    t = 1000;
    spoke(0x2601, t);
    zh_dev_tick(&D, t);
    node_rsp(last_seq(), 0x84, 0x2601, 0, 0, 0, t + 10);              /* NOT_SUPPORTED */
    CHECK(D.dev[i26].iv == ZH_IV_ACTIVE && !(D.dev[i26].rec.flags & ZH_DEVF_NODE_DESC), "D26 节点描述符被拒：只是补充信息，接着问端点（厂商码不标\"拿到了\"）");
    auto_answer(i26, 0x2601, &t, false, ZH_IV_SIMPLE);
    spoke(0x2601, t);
    zh_dev_tick(&D, t);
    {
        const uint8_t sq = last_seq();
        simple_rsp(sq, 0, 0x2601, 7, 0x0104, 0x0100, (const uint16_t[1]){ 0x0000 }, 1, NULL, 0, t + 10);   /* 回的是没问的端点 7 */
        CHECK(D.dev[i26].rec.n_ep == 0 && D.st.iv_bad == 1, "D26 Simple_Desc_rsp 的端点不是问的那个：不认");
        const uint8_t bad[] = { sq, 0x00, 0x01, 0x26, 0x0C, 0x01, 0x04, 0x01, 0x00, 0x01, 0x01, 0x01, 0x00, 0x00, 0x03, 0x06, 0x00, 0x08 };   /* 输出数 3、只有 1.5 个 */
        zh_dev_on_zdo(&D, 0x2601, ZH_ZDO_SIMPLE_DESC_RSP, bad, sizeof(bad), t + 20);
        CHECK(D.dev[i26].rec.n_ep == 0 && D.st.iv_bad == 2, "D26 输出簇数越界：不认");
    }
    /* Basic：一直不回 → 先两个一组 3 次、再单读 3 次，厂商 / 型号都没拿到 → 失败 */
    fresh();
    annce(0x2602, EA, 0x80, 1000);
    const int i26b = zh_dev_find_ieee(&D, EA);
    t = 1000;
    auto_answer(i26b, 0x2602, &t, false, ZH_IV_BASIC);
    for (int k = 0; k < 2 * (int)ZH_IV_TRIES; k++) {
        spoke(0x2602, t);
        zh_dev_tick(&D, t);
        t += ZH_IV_STEP_TIMEOUT_MS;
        zh_dev_tick(&D, t);
    }
    CHECK(D.dev[i26b].iv == ZH_IV_FAILED && D.dev[i26b].iv_single, "D26 Basic 两个一组 %u 次、单读 %u 次都没回：失败（厂商 / 型号没拿到）", ZH_IV_TRIES, ZH_IV_TRIES);
    /* 有进展清次数：单读接连成功后来一次超时，不能算到第 3 次 */
    fresh();
    annce(0x2603, EA, ZH_CAP_MAINS, 1000);                             /* 能力字节说市电（不收空闲）：供电属性说电池 → 清掉 */
    const int i26c = zh_dev_find_ieee(&D, EA);
    t = 1000;
    auto_answer(i26c, 0x2603, &t, false, ZH_IV_BASIC);
    CHECK(D.dev[i26c].rec.flags & ZH_DEVF_MAINS, "D26 节点描述符说市电");
    spoke(0x2603, t);
    zh_dev_tick(&D, t);
    zcl_rsp(0x2603, ZH_CL_BASIC, last_seq(), (const uint8_t[]){ 0x04, 0x00, 0x00, 0x42, 0x01, 'M', 0x05, 0x00, 0x89 }, 9, t + 10);
    spoke(0x2603, t + 10);
    zh_dev_tick(&D, t + 10);
    {   /* 型号：30 字节长串，只存 23 字节 */
        uint8_t rec30[5 + 30] = { 0x05, 0x00, 0x00, 0x42, 30 };
        memset(rec30 + 5, 'm', 30);
        zcl_rsp(0x2603, ZH_CL_BASIC, last_seq(), rec30, sizeof(rec30), t + 20);
    }
    CHECK(strlen(D.dev[i26c].rec.model) == 23, "D26 型号 30 B：只存 23 B（记录里 24 B 含结尾 0）");
    spoke(0x2603, t + 20);
    zh_dev_tick(&D, t + 20);
    zcl_rsp(0x2603, ZH_CL_BASIC, last_seq(), (const uint8_t[]){ 0x07, 0x00, 0x00, 0x30, 0x03 }, 5, t + 30);
    CHECK(!(D.dev[i26c].rec.flags & ZH_DEVF_MAINS), "D26 供电属性 3（电池）：清掉市电标志");
    spoke(0x2603, t + 30);
    zh_dev_tick(&D, t + 30);                                           /* 单读软件版本：这已是第 4 条请求 */
    zh_dev_tick(&D, t + 30 + ZH_IV_STEP_TIMEOUT_MS);                   /* 超时一次 */
    CHECK(D.dev[i26c].iv == ZH_IV_BASIC, "D26 有进展就重新计次数：之后超时一次不算第 3 次，接着读");
    spoke(0x2603, t + 40 + ZH_IV_STEP_TIMEOUT_MS);
    zh_dev_tick(&D, t + 40 + ZH_IV_STEP_TIMEOUT_MS);
    {
        const uint8_t sq = last_seq();
        uint8_t nos2c[] = { ZH_ZCL_FC_DIS_DR, sq, ZH_ZCL_CMD_READ_ATTR_RSP, 0x00, 0x40, 0x86 };   /* 方向位不是服务端 → 客户端 */
        zh_dev_on_zcl(&D, 0x2603, ZH_HA_PROFILE, ZH_CL_BASIC, 1, nos2c, sizeof(nos2c), t + 50 + ZH_IV_STEP_TIMEOUT_MS);
        CHECK(D.dev[i26c].iv == ZH_IV_BASIC && (D.dev[i26c].iv_want & ZH_IV_W_SWB), "D26 方向位不对的 ZCL：不理");
        zcl_rsp(0x2603, ZH_CL_BASIC, (uint8_t)(sq + 7), (const uint8_t[]){ 0x00, 0x40, 0x86 }, 3, t + 60 + ZH_IV_STEP_TIMEOUT_MS);
        CHECK(D.dev[i26c].iv == ZH_IV_BASIC && (D.dev[i26c].iv_want & ZH_IV_W_SWB), "D26 序号对不上的读属性应答：不理");
        zcl_rsp(0x2603, ZH_CL_BASIC, sq, (const uint8_t[]){ 0x00, 0x40, 0x86 }, 3, t + 70 + ZH_IV_STEP_TIMEOUT_MS);
        CHECK(D.dev[i26c].rec.flags & ZH_DEVF_INTERVIEWED, "D26 对上的那条：完成");
    }

    /* ---- D27 ---- 太短的 Node / Active 应答、ZoneType 读不到 */
    fresh();
    annce(0x2701, EA, 0x80, 1000);
    const int i27 = zh_dev_find_ieee(&D, EA);
    spoke(0x2701, 1000);
    zh_dev_tick(&D, 1000);
    {
        const uint8_t shortn[10] = { last_seq(), 0x00, 0x01, 0x27, 0x02, 0x40, 0x80, 0x11, 0x11, 0x52 };   /* 状态 0、描述符只有 6 B */
        zh_dev_on_zdo(&D, 0x2701, ZH_ZDO_NODE_DESC_RSP, shortn, sizeof(shortn), 1010);
        CHECK(D.dev[i27].iv == ZH_IV_NODE && D.st.iv_bad == 1 && D.dev[i27].rec.manuf_code == 0, "D27 节点描述符不到 %u B：不认", ZH_ZDO_NODE_RSP_MIN);
        node_rsp(last_seq(), 0, 0x2701, 0x02, 0x80, 0x3333, 1020);
        spoke(0x2701, 1020);
        zh_dev_tick(&D, 1020);
        const uint8_t shorta[7] = { last_seq(), 0x00, 0x01, 0x27, 0x04, 0x01, 0x02 };   /* 说 4 个端点、只有 2 个 */
        zh_dev_on_zdo(&D, 0x2701, ZH_ZDO_ACTIVE_EP_RSP, shorta, sizeof(shorta), 1030);
        CHECK(D.dev[i27].iv == ZH_IV_ACTIVE && D.st.iv_bad == 2, "D27 活动端点数越界：不认");
    }
    fresh();
    annce(0x2702, EA, 0x80, 1000);
    const int i27b = zh_dev_find_ieee(&D, EA);
    t = 1000;
    auto_answer(i27b, 0x2702, &t, true, ZH_IV_ZONE);
    CHECK(D.dev[i27b].iv == ZH_IV_ZONE, "D27 走到读 ZoneType");
    for (int k = 0; k < (int)ZH_IV_TRIES; k++) {
        spoke(0x2702, t);
        zh_dev_tick(&D, t);
        t += ZH_IV_STEP_TIMEOUT_MS;
        zh_dev_tick(&D, t);
    }
    CHECK((D.dev[i27b].rec.flags & ZH_DEVF_INTERVIEWED) && D.dev[i27b].rec.zone_type == ZH_ZONE_TYPE_UNKNOWN && s_ev[ZH_DEV_EV_IV_FAILED] == 0,
          "D27 ZoneType %u 次都没回：照样完成（留未知），不算访谈失败", ZH_IV_TRIES);

    /* ---- D28 ---- 两轮审计补的 */
    fresh();                                                            /* 网络不是 UP：不推进 */
    zh_dev_set_net_up(&D, false);
    annce(0x2801, EA, ZH_CAP_FFD | ZH_CAP_RX_ON_IDLE, 1000);
    zh_dev_tick(&D, 1000);
    CHECK(s_nsent == 0, "D28 网络不是 UP：不发访谈请求");
    zh_dev_set_net_up(&D, true);
    zh_dev_tick(&D, 1100);
    CHECK(s_nsent == 1, "D28 网络回来了：发");
    fresh();                                                            /* 失败后：重新宣告马上重来；一小时后再说话也重来 */
    annce(0x2802, EA, 0x80, 1000);
    const int i28 = zh_dev_find_ieee(&D, EA);
    t = 1000;
    for (int k = 0; k < (int)ZH_IV_TRIES; k++) {
        spoke(0x2802, t);
        zh_dev_tick(&D, t);
        t += ZH_IV_STEP_TIMEOUT_MS;
        zh_dev_tick(&D, t);
    }
    CHECK(D.dev[i28].iv == ZH_IV_FAILED, "D28 先失败一次");
    spoke(0x2802, t + 1000);
    CHECK(D.dev[i28].iv == ZH_IV_FAILED, "D28 失败后不到一小时说话：不重来");
    annce(0x2802, EA, 0x80, t + 2000);
    CHECK(D.dev[i28].iv == ZH_IV_NODE && D.dev[i28].iv_tries == 0, "D28 失败过的重新宣告（多半刚上电）：马上重新访谈");
    for (int k = 0; k < (int)ZH_IV_TRIES; k++) {
        spoke(0x2802, t);
        zh_dev_tick(&D, t);
        t += ZH_IV_STEP_TIMEOUT_MS;
        zh_dev_tick(&D, t);
    }
    CHECK(D.dev[i28].iv == ZH_IV_FAILED, "D28 又失败一次");
    spoke(0x2802, t + ZH_IV_RETRY_MS);
    CHECK(D.dev[i28].iv == ZH_IV_NODE, "D28 失败 %u ms 之后它说话：重新访谈", ZH_IV_RETRY_MS);
    fresh();                                                            /* 第七步 a 的旧记录：zone_type 0 要复位成未知 */
    memset(&r, 0, sizeof(r));
    r.ver = ZH_DEV_REC_VER;
    memcpy(r.ieee, EA, 8);
    r.nwk = 0x2803;
    r.zone_type = 0;
    strcpy(r.model, "OLD");
    zh_dev_load(&D, 0, &r, 1000);
    CHECK(D.dev[0].rec.zone_type == ZH_ZONE_TYPE_UNKNOWN && !strcmp(D.dev[0].rec.model, "OLD"), "D28 没访谈过的旧记录：开始访谈时 zone_type 0 → 未知（0x0000 是合法的 ZoneType）；"
          "型号不清（不是重新配对：小米的型号多半是从主动上报学来的，M5 第十步；重新配对清，见 D44）");
    fresh();                                                            /* 重新配对：清掉的"已访谈"立刻落盘 */
    annce(0x2804, EA, ZH_CAP_FFD | ZH_CAP_RX_ON_IDLE, 1000);
    const int i28d = zh_dev_find_ieee(&D, EA);
    t = 1000;
    auto_answer(i28d, 0x2804, &t, false, ZH_IV_NONE);
    const int sv28 = s_saves;
    zh_dev_on_tc_join(&D, 0x2804, EA, ZH_JOIN_UNSECURED_JOIN, ZH_JOIN_USE_PRECONFIGURED_KEY, 0, t);
    CHECK(s_saves == sv28 + 1 && !(D.dev[i28d].rec.flags & ZH_DEVF_INTERVIEWED), "D28 重新配对：\"没访谈过\"立刻落盘（这次访谈失败、下次开机还会再访谈）");
    fresh();                                                            /* 刚入网的休眠设备：60 s 内当醒着 */
    zh_dev_on_tc_join(&D, 0x2805, EB, ZH_JOIN_UNSECURED_JOIN, ZH_JOIN_USE_PRECONFIGURED_KEY, 0, 1000);
    zh_dev_tick(&D, 1000);
    zh_dev_tick(&D, 1000 + ZH_IV_STEP_TIMEOUT_MS);                     /* 第一条丢了 */
    zh_dev_tick(&D, 1000 + ZH_IV_STEP_TIMEOUT_MS + 10);
    CHECK(s_nsent == 2, "D28 刚入网 %u ms 内：它没再说话也照样重发（入网后还在快速轮询）", ZH_IV_JOIN_AWAKE_MS);
    zh_dev_tick(&D, 1000 + ZH_IV_JOIN_AWAKE_MS + 10);                  /* 第二条也超时了，窗口也过了 */
    zh_dev_tick(&D, 1000 + ZH_IV_JOIN_AWAKE_MS + 20);
    CHECK(s_nsent == 2, "D28 入网窗口过了：当休眠设备，等它说话");
    fresh();                                                            /* 默认应答改单读：次数重新算 */
    annce(0x2806, EA, 0x80, 1000);
    const int i28f = zh_dev_find_ieee(&D, EA);
    t = 1000;
    auto_answer(i28f, 0x2806, &t, false, ZH_IV_BASIC);
    for (int k = 0; k < 2; k++) {                                       /* 两个一组超时两次 */
        spoke(0x2806, t);
        zh_dev_tick(&D, t);
        t += ZH_IV_STEP_TIMEOUT_MS;
        zh_dev_tick(&D, t);
    }
    spoke(0x2806, t);
    zh_dev_tick(&D, t);
    zcl_default_rsp(0x2806, ZH_CL_BASIC, last_seq(), 0xC3, t + 10);     /* 第 3 次才收到拒绝 */
    CHECK(D.dev[i28f].iv_single && D.dev[i28f].iv_tries == 0 && D.dev[i28f].iv == ZH_IV_BASIC, "D28 第 3 次被默认应答拒、改单读：次数清零（单读有完整的 %u 次）", ZH_IV_TRIES);
    {
        const uint8_t old = last_seq();
        spoke(0x2806, t + 20);
        zh_dev_tick(&D, t + 20);                                       /* 单读厂商 */
        zcl_default_rsp(0x2806, ZH_CL_BASIC, old, 0xC3, t + 30);        /* 上一条（两个一组那条）的拒绝又来一次（迟到 / 重复） */
        CHECK(D.dev[i28f].iv_want == (ZH_IV_W_MANUF | ZH_IV_W_MODEL | ZH_IV_W_POWER | ZH_IV_W_SWB), "D28 单读阶段迟到的旧拒绝：不当成这次单读被拒（不放弃厂商）");
        const uint8_t s_manuf = last_seq();
        zcl_rsp(0x2806, ZH_CL_BASIC, s_manuf, (const uint8_t[]){ 0x04, 0x00, 0x00, 0x42, 0x01, 'Q' }, 6, t + 40);
        spoke(0x2806, t + 40);
        zh_dev_tick(&D, t + 40);                                       /* 单读型号 */
        CHECK(fr_pl(last())[3] == 0x05, "D28 接着单读型号");
        zcl_default_rsp(0x2806, ZH_CL_BASIC, s_manuf, 0xC3, t + 50);    /* 厂商那条的拒绝迟到（它其实已经回过成功） */
        CHECK((D.dev[i28f].iv_want & ZH_IV_W_MODEL) && D.iv_busy != 0, "D28 迟到的拒绝对的是上一条单读（厂商）：不放弃这次在问的型号、在途那条照等");
    }
    fresh();                                                            /* 醒着的休眠设备优先 */
    annce(0x2807, EA, ZH_CAP_FFD | ZH_CAP_RX_ON_IDLE, 1000);            /* 路由器（下标 0，轮到它在前） */
    annce(0x2808, EB, 0x80, 1000);                                     /* 休眠终端，刚说过话 */
    zh_dev_tick(&D, 1000);
    CHECK(s_nsent == 1 && fr_dest(0) == 0x2808, "D28 醒着的休眠设备先问（它只在这几秒听得见），路由器等下一条");
    fresh();                                                            /* 对方端点报 ZLL 0xC05E：照样用 HA profile 读 */
    annce(0x2809, EA, ZH_CAP_FFD | ZH_CAP_RX_ON_IDLE, 1000);
    const int i28h = zh_dev_find_ieee(&D, EA);
    zh_dev_tick(&D, 1000);
    node_rsp(last_seq(), 0, 0x2809, 0x01, 0x8E, 0x100B, 1010);
    zh_dev_tick(&D, 1010);
    active_rsp(last_seq(), 0, 0x2809, (const uint8_t[1]){ 11 }, 1, 1020);
    zh_dev_tick(&D, 1020);
    simple_rsp(last_seq(), 0, 0x2809, 11, 0xC05E, 0x0220, (const uint16_t[2]){ 0x0000, 0x0006 }, 2, NULL, 0, 1030);
    zh_dev_tick(&D, 1030);
    CHECK(D.dev[i28h].iv == ZH_IV_BASIC && fr_cluster(last()) == ZH_CL_BASIC && fr_profile(last()) == ZH_HA_PROFILE && fr_dst_ep(last()) == 11,
          "D28 对方端点报 ZLL 0xC05E：读 Basic 照样用 HA profile（SDK 取本地源端点的 profile）、发往端点 11");

    /* ---- D29 ---- 修复复核补的：新入网的失败一次、重新宣告后成功 → 照样推"认出来了"；失败只推第一次；发事件时表已 dirty */
    fresh();
    zh_dev_on_tc_join(&D, 0x2901, EA, ZH_JOIN_UNSECURED_JOIN, ZH_JOIN_USE_PRECONFIGURED_KEY, 0, 1000);
    const int i29 = zh_dev_find_ieee(&D, EA);
    t = 1000;
    for (int k = 0; k < (int)ZH_IV_TRIES; k++) {                       /* 入网窗口里 3 次都没回 */
        zh_dev_tick(&D, t);
        t += ZH_IV_STEP_TIMEOUT_MS;
        D.dirty = false;                                               /* zhub.c 发布过快照：看失败那一刻 zh_dev 自己有没有置 dirty */
        zh_dev_tick(&D, t);
    }
    CHECK(D.dev[i29].iv == ZH_IV_FAILED && s_ev[ZH_DEV_EV_IV_FAILED] == 1 && s_last_src == ZH_DEV_SRC_JOIN && s_dirty_at_ev,
          "D29 新入网的访谈失败：推一次（来源 JOIN），发事件时表已 dirty");
    annce(0x2901, EA, 0x80, t + 100);                                  /* 它重新宣告：重来 */
    CHECK(D.dev[i29].iv == ZH_IV_NODE && !D.dev[i29].iv_new, "D29 重新宣告：重新访谈（不再算新入网）");
    for (int k = 0; k < (int)ZH_IV_TRIES; k++) {
        spoke(0x2901, t + 100);
        zh_dev_tick(&D, t + 100);
        t += ZH_IV_STEP_TIMEOUT_MS;
        zh_dev_tick(&D, t + 100);
    }
    CHECK(s_ev[ZH_DEV_EV_IV_FAILED] == 2 && s_last_src == ZH_DEV_SRC_PASSIVE, "D29 重试又失败：这次不推（来源不是 JOIN）");
    annce(0x2901, EA, ZH_CAP_FFD | ZH_CAP_RX_ON_IDLE, t + 200);
    t += 200;
    auto_answer(i29, 0x2901, &t, false, ZH_IV_NONE);
    CHECK((D.dev[i29].rec.flags & ZH_DEVF_INTERVIEWED) && s_ev[ZH_DEV_EV_IDENTIFIED] == 1 && s_last_src == ZH_DEV_SRC_JOIN,
          "D29 后来成功了：推\"认出来了\"（Muse 之前收到过认不出来）");

    /* ================= 读数（M5 第一步） ================= */
    /* ---- D30 ---- IAS 通知：报警开 / 关、防拆、重复的不报 */
    fresh();
    annce(0x3001, EA, 0x80, 1000);
    const int i30 = zh_dev_find_ieee(&D, EA);
    const int sv30 = s_saves;
    D.dirty = false;                                                   /* zhub.c 发布过快照：看事件那一刻 zh_dev 自己置没置 dirty */
    zone_notify(0x3001, ZH_IAS_ST_ALARM1, 2000);
    CHECK(D.dev[i30].st.zone_status == ZH_IAS_ST_ALARM1 && (D.dev[i30].st.have & (1u << ZH_ST_ZONE)) && D.dev[i30].st.t[ZH_ST_ZONE] == 2000 &&
          s_ev[ZH_DEV_EV_STATE] == 1 && D.dev[i30].st_ev == ZH_STEV_ALARM_ON && s_dirty_at_ev, "D30 alarm1 0 → 1：事件 STATE（ALARM_ON），发事件时表已 dirty");
    zone_notify(0x3001, ZH_IAS_ST_ALARM1, 2100);
    CHECK(s_ev[ZH_DEV_EV_STATE] == 1 && D.dev[i30].st.t[ZH_ST_ZONE] == 2100, "D30 一样的状态再来：只更新时刻，不再报");
    zone_notify(0x3001, 0, 2200);
    CHECK(s_ev[ZH_DEV_EV_STATE] == 2 && D.dev[i30].st_ev == ZH_STEV_ALARM_OFF, "D30 alarm1 1 → 0：ALARM_OFF");
    zone_notify(0x3001, ZH_IAS_ST_TAMPER | ZH_IAS_ST_BATTERY, 2300);
    CHECK(s_ev[ZH_DEV_EV_STATE] == 3 && D.dev[i30].st_ev == (ZH_STEV_TAMPER | ZH_STEV_BATTERY_LOW), "D30 防拆 + 电量低一起来：一条事件两个位");
    zone_notify(0x3001, ZH_IAS_ST_TAMPER | ZH_IAS_ST_BATTERY, 2350);
    CHECK(s_ev[ZH_DEV_EV_STATE] == 3, "D30 防拆 / 电量低还亮着、再来一遍：不再报");
    CHECK(s_saves == sv30, "D30 读数不落盘");
    {
        const uint8_t p1[1] = { 0x01 };
        zcl_frame(0x3001, ZH_CL_IAS_ZONE, ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_S2C, ZH_IAS_CMD_STATUS_CHANGE, p1, 1, 1, 2400);
        CHECK(D.st.report_bad == 1 && D.dev[i30].st.t[ZH_ST_ZONE] == 2350, "D30 通知不到 2 B：不认、记一笔");
        zcl_frame(0x3001, ZH_CL_IAS_ZONE, ZH_ZCL_FC_CLUSTER, ZH_IAS_CMD_STATUS_CHANGE, (const uint8_t[2]){ 0x01, 0x00 }, 2, 1, 2500);   /* 方向是客户端 → 服务端 */
        CHECK(D.dev[i30].st.t[ZH_ST_ZONE] == 2350, "D30 方向不对的不理");
        zcl_frame(0x3001, ZH_CL_IAS_ZONE, ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_S2C | ZH_ZCL_FC_MFG, 0x11, (const uint8_t[3]){ 0x00, 0x01, 0x00 }, 3, 1, 2600);
        /* 厂商专用位置位、厂商码 0x1234 之后正好是"序号 + 命令 0x00 + 状态 0x0001"：照字节看像一条报警通知，但它是厂商私有的 */
        zcl_frame(0x3001, ZH_CL_IAS_ZONE, ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_S2C | ZH_ZCL_FC_MFG, 0x12, (const uint8_t[6]){ 0x5B, 0x00, 0x01, 0x00, 0x00, 0x00 }, 6, 1, 2650);
        CHECK(D.dev[i30].st.t[ZH_ST_ZONE] == 2350, "D30 厂商专用帧不理");
        zone_notify(0x7777, ZH_IAS_ST_ALARM1, 2700);
        CHECK(s_ev[ZH_DEV_EV_STATE] == 3, "D30 表里没有的短地址：不理");
    }
    fresh();                                                            /* 这次开机头一条就是报警：照样报 */
    annce(0x3002, EA, 0x80, 1000);
    zone_notify(0x3002, ZH_IAS_ST_ALARM1, 1100);
    CHECK(s_ev[ZH_DEV_EV_STATE] == 1 && D.dev[zh_dev_find_ieee(&D, EA)].st_ev == ZH_STEV_ALARM_ON, "D30 开机后头一条就是报警：照样报（之前当 0）");
    fresh();
    annce(0x3003, EA, 0x80, 1000);
    zone_notify(0x3003, 0, 1100);
    CHECK(s_ev[ZH_DEV_EV_STATE] == 0, "D30 开机后头一条是平静：不报");

    /* ---- D31 ---- 属性上报：单位换算与无效值 */
    fresh();
    annce(0x3101, EA, 0x80, 1000);
    const int i31 = zh_dev_find_ieee(&D, EA);
    report(0x3101, ZH_CL_TEMPERATURE, (const uint8_t[]){ 0x00, 0x00, 0x29, 0x00, 0xFE }, 5, 2000);
    report(0x3101, ZH_CL_HUMIDITY, (const uint8_t[]){ 0x00, 0x00, 0x21, 0xD7, 0x11 }, 5, 2000);
    report(0x3101, ZH_CL_PRESSURE, (const uint8_t[]){ 0x00, 0x00, 0x29, 0xF5, 0x03 }, 5, 2000);
    report(0x3101, ZH_CL_ILLUMINANCE, (const uint8_t[]){ 0x00, 0x00, 0x21, 0x11, 0x27 }, 5, 2000);   /* 10001 → 10 lux */
    report(0x3101, ZH_CL_POWER_CFG, (const uint8_t[]){ 0x21, 0x00, 0x20, 0x96, 0x20, 0x00, 0x20, 0x1E }, 8, 2000);   /* 150 → 75 %（单位 0.5 %）；30 → 3000 mV */
    report(0x3101, ZH_CL_ONOFF, (const uint8_t[]){ 0x00, 0x00, 0x10, 0x01 }, 4, 2000);
    report(0x3101, ZH_CL_LEVEL, (const uint8_t[]){ 0x00, 0x00, 0x20, 0x7F }, 4, 2000);   /* 127 → 50 % */
    {
        const zh_state_t *st = &D.dev[i31].st;
        CHECK(st->temp_c100 == -512 && st->hum_c100 == 4567 && st->pressure_hpa10 == 10130 && st->lux == 10 && st->battery_pct == 75 && st->battery_mv == 3000 &&
              st->onoff == 1 && st->level_pct == 50 && D.st.reports == 8 && s_ev[ZH_DEV_EV_STATE] == 0,
              "D31 温度 -5.12 °C、湿度 45.67 %%、气压 1013.0 hPa、照度 10 lux、电量 75 %% / 3000 mV、开、亮度 50 %%；不发事件");
        CHECK(st->have == ((1u << ZH_ST_TEMP) | (1u << ZH_ST_HUMIDITY) | (1u << ZH_ST_PRESSURE) | (1u << ZH_ST_LUX) | (1u << ZH_ST_BATTERY_PCT) |
                           (1u << ZH_ST_BATTERY_MV) | (1u << ZH_ST_ONOFF) | (1u << ZH_ST_LEVEL)), "D31 每个读数都标了有值");
    }
    report(0x3101, ZH_CL_TEMPERATURE, (const uint8_t[]){ 0x00, 0x00, 0x29, 0x00, 0x80 }, 5, 3000);   /* 0x8000 无效 */
    report(0x3101, ZH_CL_HUMIDITY, (const uint8_t[]){ 0x00, 0x00, 0x21, 0xFF, 0xFF }, 5, 3000);
    report(0x3101, ZH_CL_ILLUMINANCE, (const uint8_t[]){ 0x00, 0x00, 0x21, 0xFF, 0xFF }, 5, 3000);
    report(0x3101, ZH_CL_POWER_CFG, (const uint8_t[]){ 0x21, 0x00, 0x20, 0xFF }, 4, 3000);
    report(0x3101, ZH_CL_ONOFF, (const uint8_t[]){ 0x00, 0x00, 0x10, 0xFF }, 4, 3000);
    report(0x3101, ZH_CL_LEVEL, (const uint8_t[]){ 0x00, 0x00, 0x20, 0xFF }, 4, 3000);
    report(0x3101, ZH_CL_TEMPERATURE, (const uint8_t[]){ 0x00, 0x00, 0x21, 0x10, 0x00 }, 5, 3000);   /* 类型不对（uint16） */
    {
        const zh_state_t *st = &D.dev[i31].st;
        CHECK(st->temp_c100 == -512 && st->hum_c100 == 4567 && st->lux == 10 && st->battery_pct == 75 && st->onoff == 1 && st->level_pct == 50 &&
              st->t[ZH_ST_TEMP] == 2000 && D.st.report_bad == 1, "D31 无效值（0x8000 / 0xFFFF / 0xFF，量不出）不收也不算坏；类型不对的不收、记 1 笔（%u）", (unsigned)D.st.report_bad);
    }
    report(0x3101, ZH_CL_POWER_CFG, (const uint8_t[]){ 0x21, 0x00, 0x20, 0xFE }, 4, 3050);   /* 254 → 127 → 封顶 100 */
    CHECK(D.dev[i31].st.battery_pct == 100, "D31 电量超过 100 %%：封顶");
    report(0x3101, ZH_CL_ILLUMINANCE, (const uint8_t[]){ 0x00, 0x00, 0x21, 0x00, 0x00 }, 5, 3100);
    CHECK(D.dev[i31].st.lux == 0 && D.dev[i31].st.t[ZH_ST_LUX] == 3100, "D31 照度 0 = 太暗量不出：记 0 lux");
    report(0x3101, ZH_CL_TEMPERATURE, (const uint8_t[]){ 0x00, 0x00, 0x29, 0x34, 0x08, 0x00, 0x00, 0x42, 0x09 }, 9, 3200);   /* 第二条坏了 */
    CHECK(D.dev[i31].st.temp_c100 == 2100 && D.st.report_bad == 2, "D31 一帧里前一条好、后一条坏：前一条照收，坏的停、记一笔");
    zcl_frame(0x3101, ZH_CL_TEMPERATURE, ZH_ZCL_FC_S2C, ZH_ZCL_CMD_READ_ATTR_RSP, (const uint8_t[]){ 0x00, 0x00, 0x00, 0x29, 0xC4, 0x09 }, 6, 1, 3300);
    CHECK(D.dev[i31].st.temp_c100 == 2500, "D31 读属性应答也进读数（25.00 °C）");

    /* ---- D32 ---- 占用传感器 */
    fresh();
    annce(0x3201, EA, 0x80, 1000);
    const int i32 = zh_dev_find_ieee(&D, EA);
    report(0x3201, ZH_CL_OCCUPANCY, (const uint8_t[]){ 0x00, 0x00, 0x18, 0x01 }, 4, 2000);
    CHECK(s_ev[ZH_DEV_EV_STATE] == 1 && D.dev[i32].st_ev == ZH_STEV_OCC_ON && D.dev[i32].st.occupancy == 1, "D32 占用 0 → 1：OCC_ON");
    report(0x3201, ZH_CL_OCCUPANCY, (const uint8_t[]){ 0x00, 0x00, 0x18, 0x01 }, 4, 2100);
    CHECK(s_ev[ZH_DEV_EV_STATE] == 1, "D32 一样的不报");
    report(0x3201, ZH_CL_OCCUPANCY, (const uint8_t[]){ 0x00, 0x00, 0x18, 0x00 }, 4, 2200);
    CHECK(s_ev[ZH_DEV_EV_STATE] == 2 && D.dev[i32].st_ev == ZH_STEV_OCC_OFF, "D32 1 → 0：OCC_OFF");
    report(0x3201, ZH_CL_IAS_ZONE, (const uint8_t[]){ 0x02, 0x00, 0x19, 0x01, 0x00 }, 5, 2300);   /* ZONE_STATUS 属性上报 */
    CHECK(s_ev[ZH_DEV_EV_STATE] == 3 && D.dev[i32].st_ev == ZH_STEV_ALARM_ON, "D32 ZONE_STATUS 属性上报：同 IAS 通知");

    /* ---- D33 ---- IAS 登记请求 → 登记应答 */
    fresh();
    annce(0x33F0, EB, 0x80, 900);                                      /* 先占掉下标 0：zone id 要是真下标，不是凑巧的 0 */
    annce(0x3301, EA, 0x80, 1000);
    const int i33 = zh_dev_find_ieee(&D, EA);
    CHECK(i33 == 1, "D33 设备在下标 1");
    zcl_frame(0x3301, ZH_CL_IAS_ZONE, ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_S2C, ZH_IAS_CMD_ENROLL_REQ, (const uint8_t[4]){ 0x0D, 0x00, 0x33, 0x12 }, 4, 2, 2000);
    {
        const int k = last();
        CHECK(s_nsent >= 1 && fr_cluster(k) == ZH_CL_IAS_ZONE && fr_profile(k) == ZH_HA_PROFILE && fr_dest(k) == 0x3301 && fr_dst_ep(k) == 2 &&
              s_frame[k % 64][12] == ZH_DEV_SRC_EP && fr_opts(k) == (ZH_APS_OPTS_DEFAULT | ZH_APS_OPT_SOURCE_EUI64) && (s_tag[k % 64] & 0xF) == ZH_DEV_TAG_KIND_MISC &&
              !s_idem[k % 64], "D33 登记应答：IAS Zone 簇、HA、发回请求来的端点 2、APS 0x1440（带源 EUI64：设备拿它比 CIE 地址）、不等结果");
        const uint8_t *pp = fr_pl(k);
        CHECK(fr_pl_n(k) == 5 && pp[0] == (ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_DIS_DR) && pp[2] == ZH_IAS_CMD_ENROLL_RSP && pp[3] == ZH_IAS_ENROLL_SUCCESS &&
              pp[4] == (uint8_t)i33 && D.st.enroll_reqs == 1 && D.st.enroll_rsps == 1, "D33 [簇专用、客户端 → 服务端、不要默认应答][序号][0x00][成功][zone id = 下标 %d]", i33);
        uint8_t buf[16];
        const zh_ezsp_frame_t fr = mkrsp(ZH_EZSP_ID_SEND_UNICAST, (const uint8_t[5]){ 0, 0, 0, 0, 1 }, 5, buf);
        CHECK(zh_dev_on_app_result(&D, s_tag[k % 64], ZH_SES_R_OK, &fr, 2010), "D33 登记应答的发送结果：认作自己的、不处理");
    }

    /* ---- D34 ---- 两轮审计补的：只收规定类型、电量低 / 人体每次翻转都报（2026-10-24 起不限间隔）、登记补 ZoneType 与限速、重新配对清读数、访谈顺带读 ZoneStatus、读数时刻封顶 */
    fresh();
    annce(0x3401, EA, 0x80, 1000);
    const int i34 = zh_dev_find_ieee(&D, EA);
    report(0x3401, ZH_CL_PRESSURE, (const uint8_t[]){ 0x00, 0x00, 0x2B, 0xFF, 0xFF, 0xFF, 0x7F }, 7, 2000);    /* int32 0x7FFFFFFF：×10 会溢出 */
    report(0x3401, ZH_CL_POWER_CFG, (const uint8_t[]){ 0x20, 0x00, 0x21, 0x10, 0x27 }, 5, 2000);               /* 电压按 uint16 报 */
    report(0x3401, ZH_CL_ILLUMINANCE, (const uint8_t[]){ 0x00, 0x00, 0x23, 0xFF, 0xFF, 0xFF, 0xFF }, 7, 2000); /* 照度 uint32 */
    report(0x3401, ZH_CL_TEMPERATURE, (const uint8_t[]){ 0x00, 0x00, 0x2B, 0x34, 0x08, 0x00, 0x00 }, 7, 2000); /* 温度 int32 */
    CHECK(D.dev[i34].st.have == 0 && D.st.report_bad == 4, "D34 不是规定类型的（int32 气压 / uint16 电压 / uint32 照度 / int32 温度）：一律不收、各记一笔");
    report(0x3401, ZH_CL_PRESSURE, (const uint8_t[]){ 0x00, 0x00, 0x29, 0x00, 0x80 }, 5, 2100);
    report(0x3401, ZH_CL_POWER_CFG, (const uint8_t[]){ 0x20, 0x00, 0x20, 0xFF }, 4, 2100);
    report(0x3401, ZH_CL_HUMIDITY, (const uint8_t[]){ 0x00, 0x00, 0x21, 0x11, 0x27 }, 5, 2100);   /* 10001 > 100 % */
    report(0x3401, ZH_CL_POWER_CFG, (const uint8_t[]){ 0x3E, 0x00, 0x1B, 0x00, 0x00, 0x00, 0x00 }, 7, 2100);   /* 认识的簇、不认识的属性 0x003E */
    CHECK(D.dev[i34].st.have == 0 && D.st.report_bad == 4, "D34 气压 0x8000 / 电压 0xFF / 湿度超 100 %%：不收也不算坏；不认识的属性不理");
    report(0x3401, ZH_CL_LEVEL, (const uint8_t[]){ 0x00, 0x00, 0x20, 0x01 }, 4, 2200);
    CHECK(D.dev[i34].st.level_pct == 1, "D34 亮度 1：1 %%，不舍成 0（亮着）");
    report(0x3401, ZH_CL_POWER_CFG, (const uint8_t[]){ 0x21, 0x00, 0x20, 0x28 }, 4, 3000);      /* 20 % */
    CHECK(s_ev[ZH_DEV_EV_STATE] == 0, "D34 电量 20 %%：不报");
    report(0x3401, ZH_CL_POWER_CFG, (const uint8_t[]){ 0x21, 0x00, 0x20, 0x1E }, 4, 4000);      /* 15 % */
    CHECK(s_ev[ZH_DEV_EV_STATE] == 1 && D.dev[i34].st_ev == ZH_STEV_BATTERY_LOW && D.dev[i34].st.battery_pct == 15, "D34 降到 15 %%：电量低");
    report(0x3401, ZH_CL_POWER_CFG, (const uint8_t[]){ 0x21, 0x00, 0x20, 0x28 }, 4, 5000);
    report(0x3401, ZH_CL_POWER_CFG, (const uint8_t[]){ 0x21, 0x00, 0x20, 0x14 }, 4, 6000);      /* 回到 20 % 又掉到 10 % */
    CHECK(s_ev[ZH_DEV_EV_STATE] == 2 && D.dev[i34].st_ev == ZH_STEV_BATTERY_LOW, "D34 回到 20 %% 又掉到 10 %%：再报（不再\"每天一条\"：网关不做人为干预，2026-10-24）");
    report(0x3401, ZH_CL_POWER_CFG, (const uint8_t[]){ 0x21, 0x00, 0x20, 0x20 }, 4, 6100);      /* 16 % */
    report(0x3401, ZH_CL_POWER_CFG, (const uint8_t[]){ 0x21, 0x00, 0x20, 0x1E }, 4, 6200);      /* 又 15 % */
    report(0x3401, ZH_CL_POWER_CFG, (const uint8_t[]){ 0x21, 0x00, 0x20, 0x26 }, 4, 6300);      /* 19 % */
    report(0x3401, ZH_CL_POWER_CFG, (const uint8_t[]){ 0x21, 0x00, 0x20, 0x1C }, 4, 6400);      /* 14 % */
    CHECK(s_ev[ZH_DEV_EV_STATE] == 2, "D34 在 15 %% 上下抖（16 / 15 / 19 / 14）：没回到 20 %% 不算恢复，不再报（回差，不是限次数）");
    zone_notify(0x3401, ZH_IAS_ST_BATTERY, 7000);
    CHECK(s_ev[ZH_DEV_EV_STATE] == 3 && D.dev[i34].st_ev == ZH_STEV_BATTERY_LOW, "D34 IAS 电量位 0 → 1：也报");
    zone_notify(0x3401, 0, 8000);
    zone_notify(0x3401, ZH_IAS_ST_BATTERY, 9000);
    CHECK(s_ev[ZH_DEV_EV_STATE] == 4 && D.dev[i34].st_ev == ZH_STEV_BATTERY_LOW, "D34 电量位又翻一次：再报");
    fresh();                                                            /* 人体：每次检测到都报（不再 5 min 一条） */
    annce(0x3402, EA, 0x80, 1000);
    const int i34b = zh_dev_find_ieee(&D, EA);
    D.dev[i34b].rec.zone_type = ZH_ZONE_MOTION;
    zone_notify(0x3402, ZH_IAS_ST_ALARM1, 2000);
    zone_notify(0x3402, 0, 2100);
    zone_notify(0x3402, ZH_IAS_ST_ALARM1, 60000);
    CHECK(s_ev[ZH_DEV_EV_STATE] == 3 && D.dev[i34b].st_ev == ZH_STEV_ALARM_ON && D.dev[i34b].st.t[ZH_ST_ZONE] == 60000,
          "D34 人体 1 分钟内又触发：照报“检测到”（网关不做人为干预，2026-10-24）");
    zone_notify(0x3402, 0, 60100);
    zone_notify(0x3402, ZH_IAS_ST_ALARM1, 60200);
    CHECK(s_ev[ZH_DEV_EV_STATE] == 5 && D.dev[i34b].st_ev == ZH_STEV_ALARM_ON, "D34 解除后 0.1 s 又有人：照报");
    report(0x3402, ZH_CL_IAS_ZONE, (const uint8_t[]){ 0x02, 0x00, 0x21, 0x00, 0x00 }, 5, 400000);   /* ZONE_STATUS 按 uint16 报：类型不对 */
    CHECK(D.dev[i34b].st.zone_status == ZH_IAS_ST_ALARM1 && D.st.report_bad == 1, "D34 ZONE_STATUS 不是 bitmap16：不收、记一笔");
    fresh();                                                            /* 占用传感器同样每次都报 */
    annce(0x3407, EA, 0x80, 1000);
    report(0x3407, ZH_CL_OCCUPANCY, (const uint8_t[]){ 0x00, 0x00, 0x18, 0x01 }, 4, 2000);
    report(0x3407, ZH_CL_OCCUPANCY, (const uint8_t[]){ 0x00, 0x00, 0x18, 0x00 }, 4, 3000);
    report(0x3407, ZH_CL_OCCUPANCY, (const uint8_t[]){ 0x00, 0x00, 0x18, 0x01 }, 4, 4000);
    CHECK(s_ev[ZH_DEV_EV_STATE] == 3 && D.dev[zh_dev_find_ieee(&D, EA)].st.occupancy == 1 && D.dev[zh_dev_find_ieee(&D, EA)].st_ev == ZH_STEV_OCC_ON,
          "D34 占用 1 s 后又有人：照报有人");
    fresh();                                                            /* 既有 IAS 人体又有占用（Develco / Frient 类）：同一次有人只出一条，走 IAS 那路 */
    annce(0x3408, EA, 0x80, 1000);
    const int i34g = zh_dev_find_ieee(&D, EA);
    D.dev[i34g].rec.zone_type = ZH_ZONE_MOTION;
    report(0x3408, ZH_CL_OCCUPANCY, (const uint8_t[]){ 0x00, 0x00, 0x18, 0x01 }, 4, 2000);
    CHECK(s_ev[ZH_DEV_EV_STATE] == 0 && D.dev[i34g].st.occupancy == 1, "D34 有 IAS 人体的设备：占用只更新读数，不发事件");
    zone_notify(0x3408, ZH_IAS_ST_ALARM1, 2050);
    CHECK(s_ev[ZH_DEV_EV_STATE] == 1 && D.dev[i34g].st_ev == ZH_STEV_ALARM_ON, "D34 随后到的 IAS 报警：照报");
    fresh();                                                            /* 登记应答间隔的时刻封顶 */
    annce(0x3409, EA, 0x80, 1000);
    const int i34h = zh_dev_find_ieee(&D, EA);
    D.dev[i34h].t_enroll = 1000;                                       /* 刚回过一次登记应答的样子 */
    zh_dev_tick(&D, 1000 + ZH_DEV_AGE_CAP_MS + ZH_DEV_AGE_CHK_MS + 5);
    CHECK((uint32_t)(1000 + ZH_DEV_AGE_CAP_MS + ZH_DEV_AGE_CHK_MS + 5 - D.dev[i34h].t_enroll) == ZH_DEV_AGE_CAP_MS,
          "D34 登记应答的时刻也封顶（49.7 天回绕后不被当成刚回过）");
    fresh();                                                            /* 登记请求：补 ZoneType、限速 */
    annce(0x3403, EA, ZH_CAP_FFD | ZH_CAP_RX_ON_IDLE, 1000);
    const int i34c = zh_dev_find_ieee(&D, EA);
    t = 1000;
    auto_answer(i34c, 0x3403, &t, false, ZH_IV_NONE);                   /* 没有 IAS 簇：ZoneType 留未知 */
    const int sv34 = s_saves, n34 = s_nsent;
    zcl_frame(0x3403, ZH_CL_IAS_ZONE, ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_S2C, ZH_IAS_CMD_ENROLL_REQ, (const uint8_t[4]){ 0x2A, 0x00, 0x00, 0x00 }, 4, 1, t + 10);
    CHECK(D.dev[i34c].rec.zone_type == ZH_ZONE_WATER && s_saves == sv34 + 1 && s_nsent == n34 + 1, "D34 访谈没读到 ZoneType：登记请求里的 0x002A（漏水）补进记录、落盘、回登记应答");
    zcl_frame(0x3403, ZH_CL_IAS_ZONE, ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_S2C, ZH_IAS_CMD_ENROLL_REQ, (const uint8_t[4]){ 0x2A, 0x00, 0x00, 0x00 }, 4, 1, t + 20);
    CHECK(s_nsent == n34 + 1 && D.st.enroll_reqs == 2, "D34 10 s 内又来登记请求：不再回（限速）");
    zcl_frame(0x3403, ZH_CL_IAS_ZONE, ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_S2C, ZH_IAS_CMD_ENROLL_REQ, (const uint8_t[4]){ 0x2A, 0x00, 0x00, 0x00 }, 4, 1, t + 10 + ZH_IAS_ENROLL_GAP_MS);
    CHECK(s_nsent == n34 + 2, "D34 过了 %u ms：再回", ZH_IAS_ENROLL_GAP_MS);
    fresh();                                                            /* 重新配对清读数 */
    annce(0x3404, EA, ZH_CAP_FFD | ZH_CAP_RX_ON_IDLE, 1000);
    const int i34d = zh_dev_find_ieee(&D, EA);
    zone_notify(0x3404, ZH_IAS_ST_ALARM1, 1100);
    zh_dev_on_tc_join(&D, 0x3404, EA, ZH_JOIN_UNSECURED_JOIN, ZH_JOIN_USE_PRECONFIGURED_KEY, 0, 1200);
    CHECK(D.dev[i34d].st.have == 0, "D34 重新配对：读数清掉（旧的报警状态会吞掉新设备的头一次报警）");
    fresh();                                                            /* 访谈读 ZoneType 时顺带读 ZoneStatus */
    annce(0x3405, EA, 0x80, 1000);
    const int i34e = zh_dev_find_ieee(&D, EA);
    t = 1000;
    auto_answer(i34e, 0x3405, &t, true, ZH_IV_ZONE);
    spoke(0x3405, t);
    zh_dev_tick(&D, t);
    CHECK(fr_cluster(last()) == ZH_CL_IAS_ZONE && fr_pl_n(last()) == 7 && fr_pl(last())[3] == 0x01 && fr_pl(last())[5] == 0x02, "D34 读 ZoneType 与 ZoneStatus（一次两个）");
    zcl_rsp(0x3405, ZH_CL_IAS_ZONE, last_seq(), (const uint8_t[]){ 0x01, 0x00, 0x00, 0x31, 0x2A, 0x00, 0x02, 0x00, 0x00, 0x19, 0x01, 0x00 }, 12, t + 10);
    CHECK((D.dev[i34e].rec.flags & ZH_DEVF_INTERVIEWED) && D.dev[i34e].rec.zone_type == ZH_ZONE_WATER && D.dev[i34e].st.zone_status == ZH_IAS_ST_ALARM1 &&
          s_ev[ZH_DEV_EV_STATE] == 1, "D34 访谈时正在漏水：ZoneStatus 进读数、出事件（迁移设备的头一条报警不被吞）");
    fresh();                                                            /* 读数时刻封顶 */
    annce(0x3406, EA, 0x80, 1000);
    const int i34f = zh_dev_find_ieee(&D, EA);
    report(0x3406, ZH_CL_TEMPERATURE, (const uint8_t[]){ 0x00, 0x00, 0x29, 0x34, 0x08 }, 5, 1000);
    zh_dev_tick(&D, 1000 + ZH_DEV_AGE_CAP_MS + ZH_DEV_AGE_CHK_MS + 5);
    CHECK((uint32_t)(1000 + ZH_DEV_AGE_CAP_MS + ZH_DEV_AGE_CHK_MS + 5 - D.dev[i34f].st.t[ZH_ST_TEMP]) == ZH_DEV_AGE_CAP_MS && D.dev[i34f].st.t[ZH_ST_LUX] == 0,
          "D34 读数的时刻同样封顶（没有的读数不碰）");

    /* ---- D35 ---- 模糊：随机 ZCL 帧穿过 zh_dev_on_zcl（读数解析 + 访谈应答），UBSan 下不许溢出 / 越界 */
    fresh();
    for (unsigned k = 0; k < 6; k++) {
        uint8_t e[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x09, 0x00, (uint8_t)(k + 1) };
        annce((uint16_t)(0x3500 + k), e, (uint8_t)(k & 1 ? 0x8E : 0x80), 1000);
        D.dev[k].rec.zone_type = (uint16_t)(k == 0 ? ZH_ZONE_MOTION : k == 1 ? ZH_ZONE_CONTACT : k == 2 ? ZH_ZONE_WATER : ZH_ZONE_TYPE_UNKNOWN);
    }
    {
        static const uint16_t cls[] = { ZH_CL_POWER_CFG, ZH_CL_ONOFF, ZH_CL_LEVEL, ZH_CL_ILLUMINANCE, ZH_CL_TEMPERATURE, ZH_CL_PRESSURE, ZH_CL_HUMIDITY,
                                        ZH_CL_OCCUPANCY, ZH_CL_IAS_ZONE, ZH_CL_BASIC, 0x0300 };
        uint32_t rs = 0x2468ACE1u;
        for (int k = 0; k < 200000; k++) {
            rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
            uint8_t b[40];
            const size_t n = rs % sizeof(b);
            uint32_t r2 = rs;
            for (size_t j = 0; j < n; j++) { r2 = r2 * 1103515245u + 12345u; b[j] = (uint8_t)(r2 >> 16); }
            if (n > 2 && (k & 3) == 0) b[2] = ZH_ZCL_CMD_REPORT_ATTR;   /* 多走上报路径 */
            if (n > 0 && (k & 7) == 1) b[0] = ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_S2C;
            zh_dev_on_zcl(&D, (uint16_t)(0x3500 + (rs >> 8) % 6), ZH_HA_PROFILE, cls[(rs >> 12) % (sizeof(cls) / sizeof(cls[0]))], 1, b, n, 2000u + (uint32_t)k);
        }
        const uint32_t bad_rand = D.st.report_bad;
        /* 第二段：合法的上报帧做底（类型对），值随机 —— 让换算分支都被压到（int16 气压 ×10、照度 pow、电量 /2、亮度…） */
        static const struct { uint16_t cl, attr; uint8_t type, len; } tpl[] = {
            { ZH_CL_POWER_CFG, 0x0021, 0x20, 1 }, { ZH_CL_POWER_CFG, 0x0020, 0x20, 1 }, { ZH_CL_ONOFF, 0x0000, 0x10, 1 }, { ZH_CL_LEVEL, 0x0000, 0x20, 1 },
            { ZH_CL_ILLUMINANCE, 0x0000, 0x21, 2 }, { ZH_CL_TEMPERATURE, 0x0000, 0x29, 2 }, { ZH_CL_PRESSURE, 0x0000, 0x29, 2 }, { ZH_CL_HUMIDITY, 0x0000, 0x21, 2 },
            { ZH_CL_OCCUPANCY, 0x0000, 0x18, 1 }, { ZH_CL_IAS_ZONE, 0x0002, 0x19, 2 } };
        const uint32_t rep0 = D.st.reports;
        for (int k = 0; k < 200000; k++) {
            rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
            const unsigned j = (rs >> 4) % (sizeof(tpl) / sizeof(tpl[0]));
            const uint8_t rec[5] = { (uint8_t)tpl[j].attr, (uint8_t)(tpl[j].attr >> 8), tpl[j].type, (uint8_t)(rs >> 16), (uint8_t)(rs >> 24) };
            report((uint16_t)(0x3500 + (rs >> 8) % 6), tpl[j].cl, rec, 3u + tpl[j].len, 500000u + (uint32_t)k);
        }
        CHECK(D.st.reports - rep0 > 150000 && bad_rand > 1000, "D35 模糊：随机帧 %u 条坏的、合法帧随机值收下 %u 条，没溢出没越界（UBSan / ASan）",
              (unsigned)bad_rand, (unsigned)(D.st.reports - rep0));
    }

    /* ---- D36 ---- 写 CIE 地址 + 自动登记（ENROLL 回读确认） */
    {
        static const uint8_t OWN[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
        /* 写属性应答：[fc S2C|DIS_DR][seq][0x04][载荷] */
        #define WRITE_RSP(nwk, sq, p, n, now) do { uint8_t wb_[16] = { ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR, (sq), ZH_ZCL_CMD_WRITE_ATTR_RSP }; \
            memcpy(wb_ + 3, (p), (n)); zh_dev_on_zcl(&D, (nwk), ZH_HA_PROFILE, ZH_CL_IAS_ZONE, 1, wb_, 3 + (n), (now)); } while (0)
        /* 回读 ZoneState 的应答 */
        #define STATE_RSP(nwk, sq, st, now) zcl_rsp((nwk), ZH_CL_IAS_ZONE, (sq), (const uint8_t[]){ 0x00, 0x00, 0x00, ZH_ZCL_T_ENUM8, (st) }, 5, (now))
        fresh();
        zh_dev_set_own_eui(&D, OWN);
        annce(0x3601, EA, 0x80, 1000);
        const int i36 = zh_dev_find_ieee(&D, EA);
        t = 1000;
        auto_answer(i36, 0x3601, &t, true, ZH_IV_CIE);
        CHECK(D.dev[i36].iv == ZH_IV_CIE && D.dev[i36].rec.zone_type == 0x0015 && !(D.dev[i36].rec.flags & ZH_DEVF_INTERVIEWED),
              "D36 IAS 设备读完 ZoneType 进写 CIE 地址这一步（还没算访谈完）");
        spoke(0x3601, t);
        zh_dev_tick(&D, t);
        const int kw = last();
        static const uint8_t want_pl[11] = { ZH_ZCL_CMD_WRITE_ATTR, 0x10, 0x00, ZH_ZCL_T_EUI64, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 };
        CHECK(fr_cluster(kw) == ZH_CL_IAS_ZONE && fr_profile(kw) == ZH_HA_PROFILE && fr_dst_ep(kw) == 1 && fr_pl_n(kw) == 14 && fr_pl(kw)[0] == 0x00 &&
              !memcmp(fr_pl(kw) + 2, want_pl, sizeof(want_pl)) && fr_pl(kw)[13] == 0x08 && fr_opts(kw) == ZH_APS_OPTS_DEFAULT,
              "D36 写属性帧照 SDK setCieAddress：帧控制 0x00、全局写、0x0010、类型 0xF0、网关 EUI64 小端（14 B）");
        const int n36 = s_nsent;
        WRITE_RSP(0x3601, (uint8_t)(last_seq() + 1), (const uint8_t[]){ ZH_ZCL_ST_SUCCESS }, 1, t + 10);
        CHECK(D.dev[i36].iv == ZH_IV_CIE && s_nsent == n36, "D36 序号对不上的写属性应答：不认");
        WRITE_RSP(0x3601, last_seq(), (const uint8_t[]){ 0 }, 0, t + 12);
        CHECK(D.dev[i36].iv == ZH_IV_CIE, "D36 空的写属性应答：不认");
        {
            const uint8_t drs[5] = { ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR, last_seq(), ZH_ZCL_CMD_DEFAULT_RSP, ZH_ZCL_CMD_WRITE_ATTR, ZH_ZCL_ST_SUCCESS };
            zh_dev_on_zcl(&D, 0x3601, ZH_HA_PROFILE, ZH_CL_IAS_ZONE, 1, drs, sizeof(drs), t + 14);
            CHECK(D.dev[i36].iv == ZH_IV_CIE, "D36 状态为 SUCCESS 的默认应答：不当成写失败");
            const uint8_t mfg[6] = { ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR | ZH_ZCL_FC_MFG, 0x12, 0x34, last_seq(), ZH_ZCL_CMD_WRITE_ATTR_RSP, ZH_ZCL_ST_SUCCESS };
            zh_dev_on_zcl(&D, 0x3601, ZH_HA_PROFILE, ZH_CL_IAS_ZONE, 1, mfg, sizeof(mfg), t + 15);
            const uint8_t c2s[4] = { ZH_ZCL_FC_DIS_DR, last_seq(), ZH_ZCL_CMD_WRITE_ATTR_RSP, ZH_ZCL_ST_SUCCESS };
            zh_dev_on_zcl(&D, 0x3601, ZH_HA_PROFILE, ZH_CL_IAS_ZONE, 1, c2s, sizeof(c2s), t + 16);
            CHECK(D.dev[i36].iv == ZH_IV_CIE, "D36 厂商专用 / 方向反了的写属性应答：不认");
        }
        const uint32_t rsp0 = D.st.enroll_rsps;
        WRITE_RSP(0x3601, last_seq(), (const uint8_t[]){ ZH_ZCL_ST_SUCCESS }, 1, t + 20);
        CHECK(D.st.cie_ok == 1 && D.dev[i36].iv == ZH_IV_ENROLL && !(D.dev[i36].rec.flags & ZH_DEVF_INTERVIEWED), "D36 写成了：进 ENROLL（还没算访谈完）");
        spoke(0x3601, t + 30);
        zh_dev_tick(&D, t + 30);
        CHECK(s_nsent == n36 + 2 && D.st.enroll_rsps == rsp0 + 1 && fr_pl(last() - 1)[2] == ZH_IAS_CMD_ENROLL_RSP && fr_pl(last() - 1)[0] == (ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_DIS_DR) &&
              fr_pl(last() - 1)[3] == ZH_IAS_ENROLL_SUCCESS && fr_pl(last() - 1)[4] == (uint8_t)i36 && (fr_opts(last() - 1) & ZH_APS_OPT_SOURCE_EUI64) &&
              fr_cluster(last()) == ZH_CL_IAS_ZONE && fr_pl(last())[2] == ZH_ZCL_CMD_READ_ATTR && fr_pl_n(last()) == 5 && fr_pl(last())[3] == 0x00 && fr_pl(last())[4] == 0x00,
              "D36 ENROLL：先发登记应答（成功、zone id = 下标、带源 EUI64），紧跟着回读 ZoneState");
        STATE_RSP(0x3601, last_seq(), 0x00, t + 40);
        CHECK(D.dev[i36].iv == ZH_IV_ENROLL && D.st.enroll_ok == 0, "D36 回读还没登记上：留在 ENROLL，下一次连登记应答一起重发");
        spoke(0x3601, t + 50);
        zh_dev_tick(&D, t + 50);
        CHECK(D.st.enroll_rsps == rsp0 + 2 && fr_pl(last())[2] == ZH_ZCL_CMD_READ_ATTR, "D36 重发：登记应答 + 回读");
        STATE_RSP(0x3601, last_seq(), ZH_IAS_STATE_ENROLLED, t + 60);
        CHECK(D.st.enroll_ok == 1 && (D.dev[i36].rec.flags & ZH_DEVF_INTERVIEWED) && !(D.dev[i36].rec.flags & ZH_DEVF_CIE_OWED) && D.dev[i36].iv == ZH_IV_NONE &&
              s_ev[ZH_DEV_EV_IDENTIFIED] == 1, "D36 读到已登记：访谈完、不欠、IDENTIFIED 一次");
        const uint8_t ereq[4] = { 0x15, 0x00, 0x00, 0x00 };   /* auto-enroll-request 的设备紧接着发的登记请求：照样回 */
        zcl_frame(0x3601, ZH_CL_IAS_ZONE, ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_S2C, ZH_IAS_CMD_ENROLL_REQ, ereq, sizeof(ereq), 1, t + 70);
        CHECK(D.st.enroll_rsps == rsp0 + 3 && D.dev[i36].iv == ZH_IV_NONE, "D36 之后的登记请求照样回、不欠就不补做");

        fresh();                                                       /* ENROLL 回读三次都不是已登记：记欠着、不算访谈失败 */
        zh_dev_set_own_eui(&D, OWN);
        annce(0x360C, EA, ZH_CAP_FFD | ZH_CAP_RX_ON_IDLE, 1000);
        const int i36l = zh_dev_find_ieee(&D, EA);
        t = 1000;
        auto_answer(i36l, 0x360C, &t, true, ZH_IV_ENROLL);
        for (int k = 0; k < 6 && D.dev[i36l].iv == ZH_IV_ENROLL; k++) {
            zh_dev_tick(&D, t);
            STATE_RSP(0x360C, last_seq(), 0x00, t + 10);
            t += 100;
        }
        CHECK(D.st.enroll_unconfirmed == 1 && (D.dev[i36l].rec.flags & ZH_DEVF_INTERVIEWED) && (D.dev[i36l].rec.flags & ZH_DEVF_CIE_OWED) &&
              s_ev[ZH_DEV_EV_IV_FAILED] == 0, "D36 回读三次都没登记上：enroll_unconfirmed、访谈完、记欠着（之后补）");

        fresh();                                                       /* 设备回失败（只读）：不再补、不算访谈失败 */
        zh_dev_set_own_eui(&D, OWN);
        annce(0x3602, EA, 0x80, 1000);
        const int i36b = zh_dev_find_ieee(&D, EA);
        t = 1000;
        auto_answer(i36b, 0x3602, &t, true, ZH_IV_CIE);
        spoke(0x3602, t);
        zh_dev_tick(&D, t);
        const int n36b = s_nsent;
        WRITE_RSP(0x3602, last_seq(), ((const uint8_t[]){ 0x88, 0x10, 0x00 }), 3, t + 10);
        CHECK(D.st.cie_fail == 1 && D.st.cie_ok == 0 && (D.dev[i36b].rec.flags & ZH_DEVF_INTERVIEWED) && !(D.dev[i36b].rec.flags & ZH_DEVF_CIE_OWED) &&
              s_ev[ZH_DEV_EV_IV_FAILED] == 0 && s_nsent == n36b, "D36 写属性应答报失败（0x88 只读）：cie_fail、访谈完、不欠（补也一样）、不发登记应答");

        fresh();                                                       /* 默认应答拒了这次写 */
        zh_dev_set_own_eui(&D, OWN);
        annce(0x3603, EA, 0x80, 1000);
        const int i36c = zh_dev_find_ieee(&D, EA);
        t = 1000;
        auto_answer(i36c, 0x3603, &t, true, ZH_IV_CIE);
        spoke(0x3603, t);
        zh_dev_tick(&D, t);
        {
            const uint8_t dr_read[5] = { ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR, last_seq(), ZH_ZCL_CMD_DEFAULT_RSP, ZH_ZCL_CMD_READ_ATTR, 0x86 };
            zh_dev_on_zcl(&D, 0x3603, ZH_HA_PROFILE, ZH_CL_IAS_ZONE, 1, dr_read, sizeof(dr_read), t + 5);
            CHECK(D.dev[i36c].iv == ZH_IV_CIE, "D36 拒的是别的命令（读）的默认应答：不认");
            const uint8_t dr[5] = { ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR, last_seq(), ZH_ZCL_CMD_DEFAULT_RSP, ZH_ZCL_CMD_WRITE_ATTR, 0x81 };
            zh_dev_on_zcl(&D, 0x3603, ZH_HA_PROFILE, ZH_CL_IAS_ZONE, 1, dr, sizeof(dr), t + 10);
        }
        CHECK(D.st.cie_fail == 1 && (D.dev[i36c].rec.flags & ZH_DEVF_INTERVIEWED) && !(D.dev[i36c].rec.flags & ZH_DEVF_CIE_OWED) && s_ev[ZH_DEV_EV_IV_FAILED] == 0,
              "D36 写被默认应答拒（0x81）：cie_fail、访谈完、不欠");

        fresh();                                                       /* 三次都不回：不算访谈失败、记欠着 */
        zh_dev_set_own_eui(&D, OWN);
        annce(0x3604, EA, ZH_CAP_FFD | ZH_CAP_RX_ON_IDLE, 1000);      /* 常电：随时可发 */
        const int i36d = zh_dev_find_ieee(&D, EA);
        t = 1000;
        auto_answer(i36d, 0x3604, &t, true, ZH_IV_CIE);
        int writes = 0;
        for (int k = 0; k < 8 && D.dev[i36d].iv == ZH_IV_CIE; k++) {
            const int before = s_nsent;
            zh_dev_tick(&D, t);
            if (s_nsent > before && fr_cluster(last()) == ZH_CL_IAS_ZONE && fr_pl(last())[2] == ZH_ZCL_CMD_WRITE_ATTR) writes++;
            t += ZH_IV_STEP_TIMEOUT_MS + 1;
        }
        CHECK(writes == (int)ZH_IV_TRIES && D.st.cie_fail == 1 && (D.dev[i36d].rec.flags & ZH_DEVF_INTERVIEWED) && (D.dev[i36d].rec.flags & ZH_DEVF_CIE_OWED) &&
              s_ev[ZH_DEV_EV_IV_FAILED] == 0 && s_saves > 0, "D36 写了 %d 次都没回：cie_fail、访谈完（不推\"认不出来\"）、记欠着并落盘", writes);

        fresh();                                                       /* 还不知道网关自己的 EUI64：不写、记欠着；知道了以后补 */
        annce(0x3605, EA, ZH_CAP_FFD | ZH_CAP_RX_ON_IDLE, 1000);
        const int i36e = zh_dev_find_ieee(&D, EA);
        t = 1000;
        auto_answer(i36e, 0x3605, &t, true, ZH_IV_NONE);
        CHECK(D.st.cie_skipped == 1 && (D.dev[i36e].rec.flags & ZH_DEVF_INTERVIEWED) && (D.dev[i36e].rec.flags & ZH_DEVF_CIE_OWED) && D.dev[i36e].iv == ZH_IV_NONE,
              "D36 不知道自己的 EUI64：不写 CIE 地址、访谈完、记欠着");
        const int ev36e = s_ev[ZH_DEV_EV_IDENTIFIED];
        zh_dev_set_own_eui(&D, OWN);
        CHECK(D.dev[i36e].iv == ZH_IV_CIE && D.dev[i36e].iv_resume && D.st.cie_resumes == 1, "D36 读到自己的 EUI64：欠着的排上补做（从写 CIE 开始）");
        auto_answer(i36e, 0x3605, &t, true, ZH_IV_NONE);
        CHECK(D.st.enroll_ok == 1 && !(D.dev[i36e].rec.flags & ZH_DEVF_CIE_OWED) && D.dev[i36e].iv == ZH_IV_NONE && s_ev[ZH_DEV_EV_IDENTIFIED] == ev36e,
              "D36 补做完：登记确认、不欠了，不再出 IDENTIFIED");
        zh_dev_set_own_eui(&D, (const uint8_t[8]){ 0 });
        CHECK(!D.have_own_eui, "D36 全 0 的 EUI64 不认（写进设备就是让它往不存在的地址报）");
        zh_dev_set_own_eui(&D, (const uint8_t[8]){ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF });
        CHECK(!D.have_own_eui, "D36 全 FF 的 EUI64 不认");
        zh_dev_set_own_eui(&D, NULL);
        CHECK(!D.have_own_eui, "D36 NULL：撤掉");

        fresh();                                                       /* 不是 IAS 设备：没有这两步 */
        zh_dev_set_own_eui(&D, OWN);
        annce(0x3606, EB, 0x80, 1000);
        const int i36f = zh_dev_find_ieee(&D, EB);
        t = 1000;
        auto_answer(i36f, 0x3606, &t, false, ZH_IV_NONE);
        CHECK((D.dev[i36f].rec.flags & ZH_DEVF_INTERVIEWED) && D.st.cie_ok + D.st.cie_fail + D.st.cie_skipped == 0, "D36 没有 IAS Zone 的设备：不写 CIE 地址");

        fresh();                                                       /* ZoneType 被默认应答拒、或三次没回：照样写 CIE */
        zh_dev_set_own_eui(&D, OWN);
        annce(0x3608, EA, ZH_CAP_FFD | ZH_CAP_RX_ON_IDLE, 1000);
        const int i36h = zh_dev_find_ieee(&D, EA);
        t = 1000;
        auto_answer(i36h, 0x3608, &t, true, ZH_IV_ZONE);
        zh_dev_tick(&D, t);
        zcl_default_rsp(0x3608, ZH_CL_IAS_ZONE, last_seq(), 0x86, t + 5);
        CHECK(D.dev[i36h].iv == ZH_IV_CIE, "D36 读 ZoneType 被默认应答拒：照样进写 CIE");
        fresh();
        zh_dev_set_own_eui(&D, OWN);
        annce(0x3609, EA, ZH_CAP_FFD | ZH_CAP_RX_ON_IDLE, 1000);
        const int i36i = zh_dev_find_ieee(&D, EA);
        t = 1000;
        auto_answer(i36i, 0x3609, &t, true, ZH_IV_ZONE);
        for (int k = 0; k < 6 && D.dev[i36i].iv == ZH_IV_ZONE; k++) { zh_dev_tick(&D, t); t += ZH_IV_STEP_TIMEOUT_MS + 1; }
        CHECK(D.dev[i36i].iv == ZH_IV_CIE, "D36 读 ZoneType 三次没回：照样进写 CIE");

        fresh();                                                       /* IAS 在第二个端点：写发到那个端点 */
        zh_dev_set_own_eui(&D, OWN);
        annce(0x360A, EA, ZH_CAP_FFD | ZH_CAP_RX_ON_IDLE, 1000);
        const int i36j = zh_dev_find_ieee(&D, EA);
        D.dev[i36j].rec.n_ep = 2;
        D.dev[i36j].rec.ep[0].ep = 1; D.dev[i36j].rec.ep[0].n_in = 1; D.dev[i36j].rec.ep[0].cl[0] = 0x0000;
        D.dev[i36j].rec.ep[1].ep = 7; D.dev[i36j].rec.ep[1].n_in = 2; D.dev[i36j].rec.ep[1].cl[0] = 0x0000; D.dev[i36j].rec.ep[1].cl[1] = ZH_CL_IAS_ZONE;
        D.dev[i36j].iv = ZH_IV_CIE; D.dev[i36j].iv_tries = 0; D.dev[i36j].iv_n_seq = 0;
        zh_dev_tick(&D, 2000);
        CHECK(fr_cluster(last()) == ZH_CL_IAS_ZONE && fr_dst_ep(last()) == 7 && fr_pl(last())[2] == ZH_ZCL_CMD_WRITE_ATTR, "D36 IAS Zone 在端点 7：写发到端点 7");

        fresh();                                                       /* 30 min 总期限落在写 CIE / 登记这一步：不算访谈失败，记欠着 */
        zh_dev_set_own_eui(&D, OWN);
        annce(0x360B, EA, 0x80, 1000);                                 /* 休眠：只在刚说过话时发 */
        const int i36k = zh_dev_find_ieee(&D, EA);
        t = 1000;
        auto_answer(i36k, 0x360B, &t, true, ZH_IV_CIE);
        spoke(0x360B, t);
        zh_dev_tick(&D, t);                                            /* 发出一次写，之后它不再说话 */
        zh_dev_tick(&D, t + ZH_IV_TOTAL_MS + 10);
        CHECK(D.dev[i36k].iv == ZH_IV_NONE && (D.dev[i36k].rec.flags & ZH_DEVF_INTERVIEWED) && (D.dev[i36k].rec.flags & ZH_DEVF_CIE_OWED) &&
              s_ev[ZH_DEV_EV_IV_FAILED] == 0 && s_ev[ZH_DEV_EV_IDENTIFIED] == 1, "D36 总期限到在写 CIE：访谈完（厂家 / 型号照样落盘）、记欠着、不推访谈失败");

        fresh();                                                       /* 写 CIE / 登记这两步里的随机帧（ASan / UBSan） */
        zh_dev_set_own_eui(&D, OWN);
        uint32_t rnd = 0x13579BDFu;
        unsigned rounds = 0, in_enroll = 0;
        int ig = -1;
        for (int k = 0; k < 20000; k++) {
            if (ig < 0 || (D.dev[ig].iv != ZH_IV_CIE && D.dev[ig].iv != ZH_IV_ENROLL)) {   /* 走完了：重新来一轮 */
                fresh();
                zh_dev_set_own_eui(&D, OWN);
                annce(0x3607, EA, ZH_CAP_FFD | ZH_CAP_RX_ON_IDLE, 1000);
                t = 1000;
                ig = zh_dev_find_ieee(&D, EA);
                auto_answer(ig, 0x3607, &t, true, (rounds & 1u) ? ZH_IV_CIE : ZH_IV_ENROLL);   /* 按轮数奇偶（LCG 的最低位每轮末恒为 1，修复复核 ❌） */
                rounds++;
            }
            zh_dev_tick(&D, t);
            if (D.dev[ig].iv == ZH_IV_ENROLL) in_enroll++;
            uint8_t fb[12];
            const size_t fl = 1 + (rnd = rnd * 1103515245u + 12345u) % sizeof(fb);
            for (size_t j = 0; j < fl; j++) fb[j] = (uint8_t)((rnd = rnd * 1103515245u + 12345u) >> 16);
            if (fl >= 3 && ((rnd >> 20) & 3u)) { fb[0] = (uint8_t)(fb[0] & ~ZH_ZCL_FC_MFG) | ZH_ZCL_FC_S2C; fb[1] = last_seq(); }
            zh_dev_on_zcl(&D, 0x3607, ZH_HA_PROFILE, ZH_CL_IAS_ZONE, 1, fb, fl, t);
            t += 500;                                                    /* 不回对的那几轮要等三次超时才走完 */
        }
        CHECK(rounds > 100 && in_enroll > 1000, "D36 写 CIE / 登记两步里灌随机帧：走完 %u 轮（%u 帧落在 ENROLL）、没溢出没越界（ASan / UBSan）", rounds, in_enroll);
    }
    /* ---- D37 ---- 欠着 CIE / 登记的补做：触发、只补两步、不出事件；补做里交不出去不占次数 */
    {
        static const uint8_t OWN[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
        zh_dev_rec_t rr;
        memset(&rr, 0, sizeof(rr));
        rr.ver = ZH_DEV_REC_VER;
        memcpy(rr.ieee, EA, 8);
        rr.nwk = 0x3701;
        rr.flags = ZH_DEVF_INTERVIEWED | ZH_DEVF_CIE_OWED;             /* 开机读回：访谈完、欠着 */
        rr.n_ep = 1; rr.ep[0].ep = 1; rr.ep[0].profile = ZH_HA_PROFILE; rr.ep[0].n_in = 2; rr.ep[0].cl[0] = 0x0000; rr.ep[0].cl[1] = ZH_CL_IAS_ZONE;
        rr.zone_type = ZH_ZONE_WATER;
        fresh();
        CHECK(zh_dev_load(&D, 3, &rr, 1000) && D.dev[3].iv == ZH_IV_NONE, "D37 开机读回欠着的：还不知道自己的 EUI64 时不动");
        zh_dev_set_own_eui(&D, OWN);
        CHECK(D.dev[3].iv == ZH_IV_CIE && D.dev[3].iv_resume, "D37 读到 EUI64：欠着的排上");
        s_send_ok = false;                                             /* 写交不出去：不占次数 */
        spoke(0x3701, 2000);
        zh_dev_tick(&D, 2000);
        zh_dev_tick(&D, 2001);
        CHECK(D.dev[3].iv == ZH_IV_CIE && D.dev[3].iv_tries == 0, "D37 交不出去：留在这一步、不占次数");
        s_send_ok = true;
        uint32_t tt = 2100;
        auto_answer(3, 0x3701, &tt, true, ZH_IV_ENROLL);
        CHECK(D.dev[3].iv == ZH_IV_ENROLL, "D37 补做里写成了：进 ENROLL");
        s_send_ok = false;                                             /* 登记应答交不出去：回读也不发、不占次数 */
        const int nb = s_nsent;
        spoke(0x3701, tt);
        zh_dev_tick(&D, tt);
        CHECK(D.dev[3].iv == ZH_IV_ENROLL && D.dev[3].iv_tries == 0 && s_nsent == nb, "D37 登记应答交不出去：不发回读、不占次数");
        s_send_ok = true;
        auto_answer(3, 0x3701, &tt, true, ZH_IV_NONE);
        CHECK(!(D.dev[3].rec.flags & ZH_DEVF_CIE_OWED) && D.dev[3].iv == ZH_IV_NONE && !D.dev[3].iv_resume && s_ev[ZH_DEV_EV_IDENTIFIED] == 0 && s_saves > 0,
              "D37 补做完：不欠了（落盘）、不出事件");

        fresh();                                                       /* 触发：重新宣告 / 加密重新入网 / 登记请求 / 一小时后再说话 */
        zh_dev_set_own_eui(&D, OWN);
        rr.nwk = 0x3702;
        zh_dev_load(&D, 4, &rr, 1000);                                 /* EUI64 先知道、后加载：加载时不排（等触发） */
        CHECK(D.dev[4].iv == ZH_IV_NONE, "D37 读回时 EUI64 已知：不立刻排（等它醒着的时候）");
        annce(0x3702, EA, 0x80, 1500);
        CHECK(D.dev[4].iv == ZH_IV_CIE && D.st.cie_resumes == 1, "D37 重新宣告：排上补做");
        D.dev[4].iv = ZH_IV_NONE; D.dev[4].iv_resume = false;
        zh_dev_on_tc_join(&D, 0x3702, EA, ZH_JOIN_SECURED_REJOIN, ZH_JOIN_NO_ACTION, 0, 1600);
        CHECK(D.dev[4].iv == ZH_IV_CIE && D.st.cie_resumes == 2 && (D.dev[4].rec.flags & ZH_DEVF_INTERVIEWED), "D37 加密重新入网：排上补做（不重新访谈）");
        D.dev[4].iv = ZH_IV_NONE; D.dev[4].iv_resume = false;
        const uint8_t ereq37[4] = { 0x2A, 0x00, 0x00, 0x00 };
        zcl_frame(0x3702, ZH_CL_IAS_ZONE, ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_S2C, ZH_IAS_CMD_ENROLL_REQ, ereq37, sizeof(ereq37), 1, 1700);
        CHECK(D.dev[4].iv == ZH_IV_CIE && D.st.cie_resumes == 3, "D37 发来登记请求：回应答、排上补做");
        D.dev[4].iv = ZH_IV_NONE; D.dev[4].iv_resume = false;
        D.dev[4].iv_t_fail = 1800;
        spoke(0x3702, 1800 + ZH_IV_RETRY_MS - 1);
        CHECK(D.dev[4].iv == ZH_IV_NONE, "D37 距上次不到一小时说话：不补");
        spoke(0x3702, 1800 + ZH_IV_RETRY_MS);
        CHECK(D.dev[4].iv == ZH_IV_CIE && D.st.cie_resumes == 4, "D37 一小时后再说话：补");
        D.dev[4].iv = ZH_IV_NONE; D.dev[4].iv_resume = false;
        D.dev[4].rec.flags &= (uint8_t)~ZH_DEVF_CIE_OWED;
        annce(0x3702, EA, 0x80, 1900 + ZH_IV_RETRY_MS);
        CHECK(D.dev[4].iv == ZH_IV_NONE && D.st.cie_resumes == 4, "D37 不欠的：重新宣告不补");
        D.dev[4].rec.flags |= ZH_DEVF_CIE_OWED | ZH_DEVF_LEFT;
        zh_dev_set_own_eui(&D, NULL);
        zh_dev_set_own_eui(&D, OWN);
        CHECK(D.dev[4].iv == ZH_IV_NONE, "D37 离开了的：不补");

        fresh();                                                       /* 触发的间隔：刚没成的，一分钟内的登记请求 / 宣告不引出新一轮 */
        zh_dev_set_own_eui(&D, OWN);
        rr.nwk = 0x3703;
        zh_dev_load(&D, 5, &rr, 1000);
        D.dev[5].iv_t_fail = 5000;                                     /* 刚没成 */
        zcl_frame(0x3703, ZH_CL_IAS_ZONE, ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_S2C, ZH_IAS_CMD_ENROLL_REQ, ereq37, sizeof(ereq37), 1, 5000 + ZH_CIE_RESUME_GAP_MS - 1);
        annce(0x3703, EA, 0x80, 5000 + ZH_CIE_RESUME_GAP_MS - 1);
        CHECK(D.dev[5].iv == ZH_IV_NONE && D.st.cie_resumes == 0, "D37 距上次没成不到一分钟：登记请求 / 宣告都不引出补做（反复发请求的设备别刷屏）");
        zcl_frame(0x3703, ZH_CL_IAS_ZONE, ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_S2C, ZH_IAS_CMD_ENROLL_REQ, ereq37, sizeof(ereq37), 1, 5000 + ZH_CIE_RESUME_GAP_MS);
        CHECK(D.dev[5].iv == ZH_IV_CIE && D.st.cie_resumes == 1, "D37 满一分钟：补");
        D.dev[5].iv = ZH_IV_NONE; D.dev[5].iv_resume = false;
        D.dev[5].iv_t_fail = 9000;                                     /* 又没成 */
        annce(0x3703, EA, 0x80, 9000 + ZH_CIE_RESUME_GAP_MS);
        CHECK(D.dev[5].iv == ZH_IV_CIE && D.st.cie_resumes == 2, "D37 宣告：没成满一分钟（不到一小时）就补");
        D.dev[5].iv = ZH_IV_NONE; D.dev[5].iv_resume = false;
        D.dev[5].iv_t_fail = 13000;
        zh_dev_on_tc_join(&D, 0x3703, EA, ZH_JOIN_SECURED_REJOIN, ZH_JOIN_NO_ACTION, 0, 13000 + ZH_CIE_RESUME_GAP_MS - 1);
        CHECK(D.dev[5].iv == ZH_IV_NONE, "D37 加密重新入网：不到一分钟不补");
        zh_dev_on_tc_join(&D, 0x3703, EA, ZH_JOIN_SECURED_REJOIN, ZH_JOIN_NO_ACTION, 0, 13000 + ZH_CIE_RESUME_GAP_MS);
        CHECK(D.dev[5].iv == ZH_IV_CIE && D.st.cie_resumes == 3, "D37 加密重新入网：满一分钟补");

        fresh();                                                       /* ENROLL 一半交出去：登记应答交了、回读没交 → 下一拍只补回读，不刷登记应答 */
        zh_dev_set_own_eui(&D, OWN);
        annce(0x3704, EA, ZH_CAP_FFD | ZH_CAP_RX_ON_IDLE, 1000);
        const int i37h = zh_dev_find_ieee(&D, EA);
        uint32_t th = 1000;
        auto_answer(i37h, 0x3704, &th, true, ZH_IV_ENROLL);
        s_send_budget = 1;                                             /* 这一拍只剩一个队列空位 */
        const uint32_t er0 = D.st.enroll_rsps;
        for (int k = 0; k < 20; k++) { s_send_budget = 1; zh_dev_tick(&D, th + (uint32_t)k); }
        CHECK(D.st.enroll_rsps == er0 + 1 && D.dev[i37h].iv_tries == 1, "D37 每拍只剩一个空位：登记应答只发一条，下一拍补上回读（%u 条）", (unsigned)(D.st.enroll_rsps - er0));
        s_send_budget = -1;

        fresh();                                                       /* ZoneState 报成 UINT8：也认（SDK 跳过类型字节） */
        zh_dev_set_own_eui(&D, OWN);
        annce(0x3705, EA, ZH_CAP_FFD | ZH_CAP_RX_ON_IDLE, 1000);
        const int i37i = zh_dev_find_ieee(&D, EA);
        uint32_t ti = 1000;
        auto_answer(i37i, 0x3705, &ti, true, ZH_IV_ENROLL);
        zh_dev_tick(&D, ti);
        zcl_rsp(0x3705, ZH_CL_IAS_ZONE, last_seq(), (const uint8_t[]){ 0x00, 0x00, 0x00, ZH_ZCL_T_UINT8, 0x01 }, 5, ti + 10);
        CHECK(D.st.enroll_ok == 1 && D.dev[i37i].iv == ZH_IV_NONE, "D37 ZoneState 报成 UINT8 = 1：认作已登记");

        fresh();                                                       /* 重新配对：欠着的标志随整套访谈重算 */
        zh_dev_set_own_eui(&D, OWN);
        rr.nwk = 0x3706;
        zh_dev_load(&D, 6, &rr, 1000);
        D.dev[6].iv = ZH_IV_NONE; D.dev[6].iv_resume = false;
        zh_dev_on_tc_join(&D, 0x3706, EA, ZH_JOIN_UNSECURED_JOIN, ZH_JOIN_USE_PRECONFIGURED_KEY, 0, 2000);
        CHECK(!(D.dev[6].rec.flags & ZH_DEVF_CIE_OWED) && D.dev[6].iv == ZH_IV_NODE && !D.dev[6].iv_resume, "D37 重新配对：欠着的标志清掉、整套重新访谈");
        #undef WRITE_RSP
        #undef STATE_RSP
    }
    /* ---- D38 ---- 绑定 + 上报配置（CONFIGURE 第二步） */
    {
        static const uint8_t OWN[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
        /* 温湿度传感器（终端、电池）：Basic、电源配置、温度、湿度 */
        /* ⑥ 用：直接让 ENROLL 这一步读到"已登记"（序号塞进它的等待表） */
        #define STATE_RSP_NOW(ix, nwk) do { D.dev[(ix)].iv_seq[0] = 0x77; D.dev[(ix)].iv_n_seq = 1; \
            zcl_rsp((nwk), ZH_CL_IAS_ZONE, 0x77, (const uint8_t[]){ 0x00, 0x00, 0x00, ZH_ZCL_T_ENUM8, 0x01 }, 5, D.t_now); } while (0)
        #define TH_SIMPLE(sq, nwk, now) simple_rsp((sq), 0, (nwk), 1, 0x0104, 0x0302, (const uint16_t[4]){ 0x0000, 0x0001, 0x0402, 0x0405 }, 4, NULL, 0, (now))
        /* 新入网（信任中心放行），走到 BASIC 读完 */
        #define JOIN_TO_CFG(nwk, eui, tt) do { \
            zh_dev_on_tc_join(&D, (nwk), (eui), ZH_JOIN_UNSECURED_JOIN, ZH_JOIN_USE_PRECONFIGURED_KEY, 0, (tt)); \
            DRIVE_TO_CFG(nwk, eui, tt); } while (0)
        /* 已经在访谈里的，答到 BASIC 读完 */
        #define DRIVE_TO_CFG(nwk, eui, tt) do { \
            const int ix_ = zh_dev_find_ieee(&D, (eui)); \
            for (int g_ = 0; g_ < 20 && D.dev[ix_].iv >= ZH_IV_NODE && D.dev[ix_].iv <= ZH_IV_BASIC; g_++) { \
                zh_dev_tick(&D, (tt)); if (s_nsent == 0) break; \
                const int k_ = last(); const uint8_t s_ = last_seq(); (tt) += 50; \
                if (fr_cluster(k_) == ZH_ZDO_NODE_DESC_REQ) node_rsp(s_, 0, (nwk), 0x02, 0x80, 0x1234, (tt)); \
                else if (fr_cluster(k_) == ZH_ZDO_ACTIVE_EP_REQ) active_rsp(s_, 0, (nwk), (const uint8_t[1]){ 1 }, 1, (tt)); \
                else if (fr_cluster(k_) == ZH_ZDO_SIMPLE_DESC_REQ) TH_SIMPLE(s_, (nwk), (tt)); \
                else { uint8_t rc_[64]; size_t rn_ = 0; const uint8_t *p_ = fr_pl(k_); \
                       for (uint8_t j_ = 3; j_ + 1 < fr_pl_n(k_); j_ += 2) { rc_[rn_++] = p_[j_]; rc_[rn_++] = p_[j_ + 1]; rc_[rn_++] = 0x86; } \
                       zcl_rsp((nwk), fr_cluster(k_), s_, rc_, rn_, (tt)); } \
            } } while (0)
        fresh();
        zh_dev_set_own_eui(&D, OWN);
        uint32_t t8 = 1000;
        JOIN_TO_CFG(0x3801, EA, t8);
        const int i38 = zh_dev_find_ieee(&D, EA);
        CHECK(D.dev[i38].iv == ZH_IV_BIND && D.dev[i38].cfg_n == 3 && D.dev[i38].cfg_i == 0 && k_cfg_cl_of(i38, 0) == ZH_CL_TEMPERATURE &&
              k_cfg_cl_of(i38, 1) == ZH_CL_HUMIDITY && k_cfg_cl_of(i38, 2) == ZH_CL_POWER_CFG, "D38 新入网的温湿度传感器：计划 3 条（温度 / 湿度 / 电量，按表 8.6-1 的顺序），先绑定");
        CHECK((D.dev[i38].rec.flags & ZH_DEVF_INTERVIEWED) && s_ev[ZH_DEV_EV_IDENTIFIED] == 1 && s_ev_src_last_identified_join() && s_saves > 0,
              "D38 配置之前就算访谈完：落盘、推送\"认出来了\"（配置不拖推送，第二轮审计 ❌）");
        zh_dev_tick(&D, t8);
        const int kb = last();
        CHECK(fr_profile(kb) == 0 && fr_cluster(kb) == ZH_ZDO_BIND_REQ && fr_dst_ep(kb) == 0 && fr_pl_n(kb) == 22 && !memcmp(fr_pl(kb) + 1, EA, 8) && fr_pl(kb)[9] == 1 &&
              fr_pl(kb)[10] == 0x02 && fr_pl(kb)[11] == 0x04 && fr_pl(kb)[12] == ZH_ZDO_UNICAST_BINDING && !memcmp(fr_pl(kb) + 13, OWN, 8) && fr_pl(kb)[21] == ZH_DEV_SRC_EP,
              "D38 绑定请求：ZDO 0x0021、[seq][设备 EUI64][端点 1][簇 0x0402][0x03][网关 EUI64][端点 1]（22 B）");
        zh_dev_on_zdo(&D, 0x3801, ZH_ZDO_BIND_RSP, (const uint8_t[2]){ (uint8_t)(fr_pl(kb)[0] + 1), 0 }, 2, t8 + 5);
        CHECK(D.dev[i38].iv == ZH_IV_BIND, "D38 序号对不上的绑定应答：不认");
        zh_dev_on_zdo(&D, 0x3802, ZH_ZDO_BIND_RSP, (const uint8_t[2]){ fr_pl(kb)[0], 0 }, 2, t8 + 6);
        CHECK(D.dev[i38].iv == ZH_IV_BIND, "D38 别的设备发来的绑定应答：不认");
        zh_dev_on_zdo(&D, 0x3801, ZH_ZDO_BIND_RSP, (const uint8_t[2]){ fr_pl(kb)[0], 0 }, 2, t8 + 10);
        CHECK(D.dev[i38].iv == ZH_IV_REPORT && D.st.bind_ok == 1, "D38 绑定成了：配上报");
        zh_dev_tick(&D, t8 + 20);
        static const uint8_t want_tmp[11] = { ZH_ZCL_CMD_CFG_REPORT, 0x00, 0x00, 0x00, ZH_ZCL_T_INT16, 0x1E, 0x00, 0x58, 0x02, 0x0A, 0x00 };
        CHECK(fr_profile(last()) == ZH_HA_PROFILE && fr_cluster(last()) == ZH_CL_TEMPERATURE && fr_dst_ep(last()) == 1 && fr_pl_n(last()) == 13 &&
              fr_pl(last())[0] == 0x00 && !memcmp(fr_pl(last()) + 2, want_tmp, 11), "D38 温度上报配置：帧控制 0x00、全局 0x06、方向 0、INT16、30 / 600 s、变化量 10（2 B；共 13 B）");
        {
            const uint8_t cr[4] = { ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR, (uint8_t)(last_seq() + 1), ZH_ZCL_CMD_CFG_REPORT_RSP, 0 };
            zh_dev_on_zcl(&D, 0x3801, ZH_HA_PROFILE, ZH_CL_TEMPERATURE, 1, cr, sizeof(cr), t8 + 25);
            CHECK(D.dev[i38].iv == ZH_IV_REPORT && D.st.report_ok == 0, "D38 序号对不上的上报配置应答：不认");
            const uint8_t cw[4] = { ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR, last_seq(), ZH_ZCL_CMD_CFG_REPORT_RSP, 0 };
            zh_dev_on_zcl(&D, 0x3801, ZH_HA_PROFILE, ZH_CL_POWER_CFG, 1, cw, sizeof(cw), t8 + 26);
            CHECK(D.dev[i38].iv == ZH_IV_REPORT && D.st.report_ok == 0, "D38 别的簇来的上报配置应答：不认");
            zh_dev_on_zcl(&D, 0x3801, ZH_HA_PROFILE, ZH_CL_TEMPERATURE, 1, cw, sizeof(cw), t8 + 30);
        }
        CHECK(D.st.report_ok == 1 && D.dev[i38].iv == ZH_IV_BIND && D.dev[i38].cfg_i == 1, "D38 上报配好了：下一条（湿度）先绑定");
        zh_dev_tick(&D, t8 + 40);
        zh_dev_on_zdo(&D, 0x3801, ZH_ZDO_BIND_RSP, (const uint8_t[2]){ last_seq(), 0x8C }, 2, t8 + 45);   /* 表满 */
        CHECK(D.st.bind_fail == 1 && D.dev[i38].iv == ZH_IV_REPORT, "D38 绑定表满（0x8C）：照样配上报");
        zh_dev_tick(&D, t8 + 50);
        static const uint8_t want_hum[11] = { ZH_ZCL_CMD_CFG_REPORT, 0x00, 0x00, 0x00, ZH_ZCL_T_UINT16, 0x1E, 0x00, 0x58, 0x02, 0x64, 0x00 };
        CHECK(fr_cluster(last()) == ZH_CL_HUMIDITY && fr_pl_n(last()) == 13 && !memcmp(fr_pl(last()) + 2, want_hum, 11), "D38 湿度：UINT16、30 / 600 s、变化量 100");
        {
            const uint8_t cf[7] = { ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR, last_seq(), ZH_ZCL_CMD_CFG_REPORT_RSP, 0x8D, 0x00, 0x00, 0x00 };   /* 失败：[状态][方向][属性] */
            zh_dev_on_zcl(&D, 0x3801, ZH_HA_PROFILE, ZH_CL_HUMIDITY, 1, cf, sizeof(cf), t8 + 55);
        }
        CHECK(D.st.report_fail == 1 && D.dev[i38].iv == ZH_IV_BIND && D.dev[i38].cfg_i == 2, "D38 上报配置失败：下一条（电量）");
        zh_dev_tick(&D, t8 + 60);
        zh_dev_on_zdo(&D, 0x3801, ZH_ZDO_BIND_RSP, (const uint8_t[2]){ last_seq(), 0 }, 2, t8 + 65);
        zh_dev_tick(&D, t8 + 70);
        static const uint8_t want_bat[10] = { ZH_ZCL_CMD_CFG_REPORT, 0x00, 0x21, 0x00, ZH_ZCL_T_UINT8, 0x10, 0x0E, 0xC0, 0xA8, 0x02 };
        CHECK(fr_cluster(last()) == ZH_CL_POWER_CFG && fr_pl_n(last()) == 12 && !memcmp(fr_pl(last()) + 2, want_bat, 10),
              "D38 电量 %% 上报配置：0x0021、UINT8、3600 / 43200 s、变化量 2（1 B）");
        {
            const uint8_t ok8[4] = { ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR, last_seq(), ZH_ZCL_CMD_CFG_REPORT_RSP, 0 };
            zh_dev_on_zcl(&D, 0x3801, ZH_HA_PROFILE, ZH_CL_POWER_CFG, 1, ok8, sizeof(ok8), t8 + 75);
        }
        CHECK((D.dev[i38].rec.flags & ZH_DEVF_INTERVIEWED) && D.dev[i38].iv == ZH_IV_NONE && D.st.bind_ok == 2 && D.st.bind_fail == 1 && D.st.report_ok == 2 &&
              D.st.report_fail == 1 && s_ev[ZH_DEV_EV_IDENTIFIED] == 1 && !D.dev[i38].iv_new, "D38 三条配完：收尾、不再出 IDENTIFIED（只在配置前推过一次）");

        fresh();                                                       /* 被动学习认出的（迁移来的）：不配 */
        zh_dev_set_own_eui(&D, OWN);
        annce(0x3803, EB, 0x80, 1000);
        const int i38b = zh_dev_find_ieee(&D, EB);
        uint32_t tb = 1000;
        auto_answer(i38b, 0x3803, &tb, false, ZH_IV_NONE);
        CHECK((D.dev[i38b].rec.flags & ZH_DEVF_INTERVIEWED) && D.st.bind_ok + D.st.bind_fail + D.st.report_ok + D.st.report_fail + D.st.cfg_skipped == 0,
              "D38 宣告认出的设备（迁移来的）：不绑定、不改上报");

        fresh();                                                       /* 不知道自己的 EUI64：不配，计 cfg_skipped */
        uint32_t tc = 1000;
        JOIN_TO_CFG(0x3804, EA, tc);
        const int i38c = zh_dev_find_ieee(&D, EA);
        CHECK((D.dev[i38c].rec.flags & ZH_DEVF_INTERVIEWED) && D.st.cfg_skipped == 3 && D.dev[i38c].iv == ZH_IV_NONE, "D38 不知道自己的 EUI64：三条都不配、访谈完、计 cfg_skipped");

        fresh();                                                       /* 小米 / Aqara 的电池设备：不绑、不配上报（2026-10-23 上板 lumi.weather 三条全失败；Z2M 同） */
        zh_dev_set_own_eui(&D, OWN);
        uint32_t tl = 1000;
        zh_dev_on_tc_join(&D, 0x3809, EA, ZH_JOIN_UNSECURED_JOIN, ZH_JOIN_USE_PRECONFIGURED_KEY, 0, tl);
        {
            const int il0 = zh_dev_find_ieee(&D, EA);
            strcpy(D.dev[il0].rec.model, "lumi.weather");             /* 小米入网时自己报的型号（访谈的 Basic 读这里答不支持） */
            D.dev[il0].lumi_k = 0;
        }
        const int b0 = s_nsent;
        DRIVE_TO_CFG(0x3809, EA, tl);
        const int il = zh_dev_find_ieee(&D, EA);
        zh_dev_tick(&D, tl);
        CHECK((D.dev[il].rec.flags & ZH_DEVF_INTERVIEWED) && D.dev[il].iv == ZH_IV_NONE && D.st.cfg_skipped == 3 && D.st.bind_ok + D.st.bind_fail == 0,
              "D38 小米温湿度计（电池）：三条都不配、不绑、访谈完、计 cfg_skipped");
        {
            int binds = 0;
            for (int j = b0; j < s_nsent; j++) if (fr_profile(j) == 0 && fr_cluster(j) == ZH_ZDO_BIND_REQ) binds++;
            CHECK(binds == 0, "D38 小米温湿度计：一条绑定请求都没发");
        }

        fresh();                                                       /* 绑定 / 上报几次都不回：不算访谈失败 */
        zh_dev_set_own_eui(&D, OWN);
        uint32_t td = 1000;
        JOIN_TO_CFG(0x3805, EA, td);
        const int i38d = zh_dev_find_ieee(&D, EA);
        for (int k = 0; k < 60 && D.dev[i38d].iv != ZH_IV_NONE; k++) { spoke(0x3805, td); zh_dev_tick(&D, td); td += ZH_IV_STEP_TIMEOUT_MS + 1; }
        CHECK((D.dev[i38d].rec.flags & ZH_DEVF_INTERVIEWED) && D.st.bind_fail == 3 && D.st.report_fail == 3 && s_ev[ZH_DEV_EV_IV_FAILED] == 0,
              "D38 几次都不回：bind_fail 3、report_fail 3、访谈照样完（不推\"认不出来\"）");

        fresh();                                                       /* 期限到在配置中：剩下的不配、访谈完 */
        zh_dev_set_own_eui(&D, OWN);
        uint32_t te = 1000;
        JOIN_TO_CFG(0x3806, EA, te);
        const int i38e = zh_dev_find_ieee(&D, EA);
        zh_dev_tick(&D, te);                                           /* 发出第一条绑定 */
        zh_dev_tick(&D, D.dev[i38e].iv_t0 + ZH_IV_TOTAL_MS + 1);
        CHECK(D.dev[i38e].iv == ZH_IV_NONE && (D.dev[i38e].rec.flags & ZH_DEVF_INTERVIEWED) && D.st.cfg_skipped == 3 && s_ev[ZH_DEV_EV_IV_FAILED] == 0 &&
              s_ev[ZH_DEV_EV_IDENTIFIED] == 1 && !(D.dev[i38e].rec.flags & ZH_DEVF_CIE_OWED) && D.st.cie_fail == 0,
              "D38 30 min 期限到在配置中：剩下 3 条不配、不再推送、不当成 CIE 没写成");

        fresh();                                                       /* 路由器（市电）不配电源配置 */
        zh_dev_set_own_eui(&D, OWN);
        zh_dev_on_tc_join(&D, 0x3807, EA, ZH_JOIN_UNSECURED_JOIN, ZH_JOIN_USE_PRECONFIGURED_KEY, 0, 1000);
        const int i38f = zh_dev_find_ieee(&D, EA);
        uint32_t tf = 1000;
        for (int g = 0; g < 20 && D.dev[i38f].iv >= ZH_IV_NODE && D.dev[i38f].iv <= ZH_IV_BASIC; g++) {
            zh_dev_tick(&D, tf);
            const int k = last(); const uint8_t sq = last_seq(); tf += 50;
            if (fr_cluster(k) == ZH_ZDO_NODE_DESC_REQ) node_rsp(sq, 0, 0x3807, 0x01, 0x8E, 0x1234, tf);   /* 路由器 */
            else if (fr_cluster(k) == ZH_ZDO_ACTIVE_EP_REQ) active_rsp(sq, 0, 0x3807, (const uint8_t[1]){ 1 }, 1, tf);
            else if (fr_cluster(k) == ZH_ZDO_SIMPLE_DESC_REQ)
                simple_rsp(sq, 0, 0x3807, 1, 0x0104, 0x0100, (const uint16_t[4]){ 0x0000, 0x0001, 0x0006, 0x0008 }, 4, NULL, 0, tf);
            else { uint8_t rc[16]; size_t rn = 0; const uint8_t *p = fr_pl(k);
                   for (uint8_t j = 3; j + 1 < fr_pl_n(k); j += 2) { rc[rn++] = p[j]; rc[rn++] = p[j + 1]; rc[rn++] = 0x86; }
                   zcl_rsp(0x3807, fr_cluster(k), sq, rc, rn, tf); }
        }
        CHECK(D.dev[i38f].iv == ZH_IV_BIND && D.dev[i38f].cfg_n == 2 && k_cfg_cl_of(i38f, 0) == ZH_CL_ONOFF && k_cfg_cl_of(i38f, 1) == ZH_CL_LEVEL,
              "D38 路由器：只配开关 / 亮度（电源配置不配）");
        zh_dev_tick(&D, tf);
        zh_dev_on_zdo(&D, 0x3807, ZH_ZDO_BIND_RSP, (const uint8_t[2]){ last_seq(), 0 }, 2, tf + 5);
        zh_dev_tick(&D, tf + 10);
        CHECK(fr_cluster(last()) == ZH_CL_ONOFF && fr_pl_n(last()) == 11 && fr_pl(last())[6] == ZH_ZCL_T_BOOL, "D38 开关：BOOLEAN、不带变化量（离散类型，11 B）");
        /* 第二轮审计列出的漏网变异 ①–⑦ 与随机帧灌入 */
        fresh();                                                       /* ① IAS 新设备：ENROLL 之后接配置 */
        zh_dev_set_own_eui(&D, OWN);
        zh_dev_on_tc_join(&D, 0x3808, EA, ZH_JOIN_UNSECURED_JOIN, ZH_JOIN_USE_PRECONFIGURED_KEY, 0, 1000);
        const int i38g = zh_dev_find_ieee(&D, EA);
        uint32_t tg = 1000;
        for (int g = 0; g < 40 && D.dev[i38g].iv >= ZH_IV_NODE && D.dev[i38g].iv <= ZH_IV_ENROLL; g++) {
            zh_dev_tick(&D, tg);
            const int k = last(); const uint8_t sq = last_seq(); tg += 50;
            if (fr_cluster(k) == ZH_ZDO_NODE_DESC_REQ) node_rsp(sq, 0, 0x3808, 0x02, 0x80, 0x1234, tg);
            else if (fr_cluster(k) == ZH_ZDO_ACTIVE_EP_REQ) active_rsp(sq, 0, 0x3808, (const uint8_t[1]){ 1 }, 1, tg);
            else if (fr_cluster(k) == ZH_ZDO_SIMPLE_DESC_REQ)
                simple_rsp(sq, 0, 0x3808, 1, 0x0104, 0x0402, (const uint16_t[3]){ 0x0000, 0x0001, 0x0500 }, 3, NULL, 0, tg);
            else if (fr_pl(k)[2] == ZH_ZCL_CMD_WRITE_ATTR) {
                const uint8_t wb[4] = { ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR, sq, ZH_ZCL_CMD_WRITE_ATTR_RSP, 0 };
                zh_dev_on_zcl(&D, 0x3808, ZH_HA_PROFILE, ZH_CL_IAS_ZONE, 1, wb, sizeof(wb), tg);
            } else if (fr_cluster(k) == ZH_CL_IAS_ZONE && D.dev[i38g].iv == ZH_IV_ENROLL) {
                zcl_rsp(0x3808, ZH_CL_IAS_ZONE, sq, (const uint8_t[]){ 0x00, 0x00, 0x00, ZH_ZCL_T_ENUM8, 0x01 }, 5, tg);
            } else if (fr_cluster(k) == ZH_CL_IAS_ZONE) {
                zcl_rsp(0x3808, ZH_CL_IAS_ZONE, sq, (const uint8_t[]){ 0x01, 0x00, 0x00, 0x31, 0x2A, 0x00 }, 6, tg);
            } else { uint8_t rc[16]; size_t rn = 0; const uint8_t *p = fr_pl(k);
                   for (uint8_t j = 3; j + 1 < fr_pl_n(k); j += 2) { rc[rn++] = p[j]; rc[rn++] = p[j + 1]; rc[rn++] = 0x86; }
                   zcl_rsp(0x3808, fr_cluster(k), sq, rc, rn, tg); }
        }
        CHECK(D.dev[i38g].iv == ZH_IV_BIND && D.st.enroll_ok == 1 && k_cfg_cl_of(i38g, 0) == ZH_CL_POWER_CFG && (D.dev[i38g].rec.flags & ZH_DEVF_INTERVIEWED) &&
              s_ev[ZH_DEV_EV_IDENTIFIED] == 1, "D38 ① IAS 新设备：登记确认后接配置（电量），配置前已推送");

        fresh();                                                       /* ② 新入网时访谈失败过、这回重试的（iv_owe_push）：也配 */
        zh_dev_set_own_eui(&D, OWN);
        uint32_t th2 = 1000;
        zh_dev_on_tc_join(&D, 0x3809, EA, ZH_JOIN_UNSECURED_JOIN, ZH_JOIN_USE_PRECONFIGURED_KEY, 0, th2);
        const int i38h = zh_dev_find_ieee(&D, EA);
        D.dev[i38h].iv_new = false;
        D.dev[i38h].iv_owe_push = true;
        DRIVE_TO_CFG(0x3809, EA, th2);
        CHECK(D.dev[i38h].iv == ZH_IV_BIND && D.dev[i38h].cfg_n == 3, "D38 ② 重试的新设备（iv_owe_push）：也配");

        fresh();                                                       /* ③ 三个端点各 7 个簇：只配前 8 条，其余计 cfg_skipped */
        zh_dev_set_own_eui(&D, OWN);
        zh_dev_on_tc_join(&D, 0x380A, EA, ZH_JOIN_UNSECURED_JOIN, ZH_JOIN_USE_PRECONFIGURED_KEY, 0, 1000);
        const int i38i = zh_dev_find_ieee(&D, EA);
        uint32_t ti3 = 1000;
        static const uint16_t many[8] = { 0x0000, 0x0006, 0x0008, 0x0402, 0x0405, 0x0400, 0x0403, 0x0406 };
        for (int g = 0; g < 40 && D.dev[i38i].iv >= ZH_IV_NODE && D.dev[i38i].iv <= ZH_IV_BASIC; g++) {
            zh_dev_tick(&D, ti3);
            const int k = last(); const uint8_t sq = last_seq(); ti3 += 50;
            if (fr_cluster(k) == ZH_ZDO_NODE_DESC_REQ) node_rsp(sq, 0, 0x380A, 0x01, 0x8E, 0x1234, ti3);
            else if (fr_cluster(k) == ZH_ZDO_ACTIVE_EP_REQ) active_rsp(sq, 0, 0x380A, (const uint8_t[3]){ 1, 2, 3 }, 3, ti3);
            else if (fr_cluster(k) == ZH_ZDO_SIMPLE_DESC_REQ) simple_rsp(sq, 0, 0x380A, fr_pl(k)[3], 0x0104, 0x0100, many, 8, NULL, 0, ti3);
            else { uint8_t rc[16]; size_t rn = 0; const uint8_t *p = fr_pl(k);
                   for (uint8_t j = 3; j + 1 < fr_pl_n(k); j += 2) { rc[rn++] = p[j]; rc[rn++] = p[j + 1]; rc[rn++] = 0x86; }
                   zcl_rsp(0x380A, fr_cluster(k), sq, rc, rn, ti3); }
        }
        CHECK(D.dev[i38i].iv == ZH_IV_BIND && D.dev[i38i].cfg_n == ZH_CFG_MAX && D.st.cfg_skipped == 3u * 7u - ZH_CFG_MAX,
              "D38 ③ 3 个端点 × 7 簇 = 21 条：只配 %u 条，其余 %u 条计 cfg_skipped", ZH_CFG_MAX, (unsigned)D.st.cfg_skipped);
        zh_dev_tick(&D, ti3);
        const uint32_t bad0 = D.st.iv_bad;
        zh_dev_on_zdo(&D, 0x380A, ZH_ZDO_BIND_RSP, (const uint8_t[1]){ last_seq() }, 1, ti3 + 5);
        CHECK(D.st.iv_bad == bad0 + 1 && D.dev[i38i].iv == ZH_IV_BIND, "D38 ④ 绑定应答不到 2 字节：记坏帧、不推进");
        zh_dev_on_zdo(&D, 0x380A, ZH_ZDO_BIND_RSP, (const uint8_t[2]){ last_seq(), 0 }, 2, ti3 + 6);
        zh_dev_tick(&D, ti3 + 10);
        const uint8_t rsq = last_seq();
        zh_dev_on_zdo(&D, 0x380A, ZH_ZDO_BIND_RSP, (const uint8_t[2]){ rsq, 0 }, 2, ti3 + 12);
        CHECK(D.dev[i38i].iv == ZH_IV_REPORT && D.dev[i38i].cfg_i == 0 && D.st.bind_ok == 1, "D38 ⑦ 不在绑定这一步时来的绑定应答：不认");
        {
            const uint8_t dr[5] = { ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR, rsq, ZH_ZCL_CMD_DEFAULT_RSP, ZH_ZCL_CMD_CFG_REPORT, 0x86 };
            zh_dev_on_zcl(&D, 0x380A, ZH_HA_PROFILE, ZH_CL_ONOFF, 1, dr, sizeof(dr), ti3 + 15);
        }
        CHECK(D.st.report_fail == 1 && D.dev[i38i].cfg_i == 1 && D.dev[i38i].iv == ZH_IV_BIND, "D38 ⑤ 上报配置被默认应答拒（0x86）：report_fail、下一条");

        fresh();                                                       /* ⑥ IAS 那几步拖到了 30 min 期限：配置整个不做、计 cfg_skipped */
        zh_dev_set_own_eui(&D, OWN);
        uint32_t t6 = 1000;
        JOIN_TO_CFG(0x380B, EA, t6);                                   /* 走到 BIND 之前已推送 */
        const int i38j = zh_dev_find_ieee(&D, EA);
        D.dev[i38j].iv = ZH_IV_ENROLL;                                 /* 假装还在 IAS 的最后一步、期限早过了 */
        D.dev[i38j].rec.flags &= (uint8_t)~ZH_DEVF_INTERVIEWED;
        D.dev[i38j].iv_t0 = 1;
        D.t_now = 1 + ZH_IV_TOTAL_MS + 100;
        const uint32_t sk0 = D.st.cfg_skipped;
        STATE_RSP_NOW(i38j, 0x380B);
        CHECK(D.dev[i38j].iv == ZH_IV_NONE && D.st.cfg_skipped == sk0 + 3 && (D.dev[i38j].rec.flags & ZH_DEVF_INTERVIEWED), "D38 ⑥ 登记确认时已过 30 min：3 条都不配、计 cfg_skipped、访谈完");

        fresh();                                                       /* 醒着的窗口随它说话顺延：60 s 之后只要它一直在说话，仍当醒着 */
        zh_dev_set_own_eui(&D, OWN);
        uint32_t t7 = 1000;
        JOIN_TO_CFG(0x380C, EA, t7);
        const int i38k = zh_dev_find_ieee(&D, EA);
        for (uint32_t tt = t7; tt < t7 + 3u * ZH_IV_JOIN_AWAKE_MS; tt += 20000) spoke(0x380C, tt);
        const int before7 = s_nsent;
        zh_dev_tick(&D, t7 + 3u * ZH_IV_JOIN_AWAKE_MS - 20000 + ZH_IV_AWAKE_MS + 100);   /* 距最近一条消息超过 3 s，但在顺延的 60 s 里 */
        CHECK(s_nsent == before7 + 1 && fr_cluster(last()) == ZH_ZDO_BIND_REQ, "D38 新入网的一直在说话：入网 60 s 之后也当醒着（休眠设备配置排得开）");
        (void)i38k;

        fresh();                                                       /* S1：BIND / REPORT 两步里灌随机帧（ASan / UBSan） */
        zh_dev_set_own_eui(&D, OWN);
        uint32_t rz = 0x2468ACE1u, tz = 1000;
        unsigned zr = 0, in_cfg = 0;
        int iz = -1;
        for (int k = 0; k < 30000; k++) {
            if (iz < 0 || (D.dev[iz].iv != ZH_IV_BIND && D.dev[iz].iv != ZH_IV_REPORT)) {
                fresh();
                zh_dev_set_own_eui(&D, OWN);
                tz = 1000;
                JOIN_TO_CFG(0x380D, EA, tz);
                iz = zh_dev_find_ieee(&D, EA);
                zr++;
            }
            zh_dev_tick(&D, tz);
            if (D.dev[iz].iv == ZH_IV_BIND || D.dev[iz].iv == ZH_IV_REPORT) in_cfg++;
            uint8_t fb[16];
            const size_t fl = 1 + (rz = rz * 1103515245u + 12345u) % sizeof(fb);
            for (size_t j = 0; j < fl; j++) fb[j] = (uint8_t)((rz = rz * 1103515245u + 12345u) >> 16);
            const unsigned pick = (rz >> 20) & 3u;
            const uint16_t snd = (pick == 3) ? (uint16_t)(rz >> 8) : 0x380D;
            if (pick == 0) { fb[0] = last_seq(); zh_dev_on_zdo(&D, snd, ZH_ZDO_BIND_RSP, fb, fl, tz); }
            else {
                if (fl >= 3) { fb[0] = (uint8_t)(fb[0] & ~ZH_ZCL_FC_MFG) | ZH_ZCL_FC_S2C; fb[1] = last_seq(); }
                static const uint16_t cls[4] = { 0x0402, 0x0405, 0x0001, 0x0006 };
                zh_dev_on_zcl(&D, snd, ZH_HA_PROFILE, cls[(rz >> 24) & 3u], 1, fb, fl, tz);
            }
            tz += 700;
        }
        CHECK(zr > 50 && in_cfg > 5000, "D38 BIND / REPORT 两步里灌随机帧：走完 %u 轮（%u 拍在配置中）、没溢出没越界（ASan / UBSan）", zr, in_cfg);
        #undef TH_SIMPLE
        #undef JOIN_TO_CFG
        #undef DRIVE_TO_CFG
        #undef STATE_RSP_NOW
    }
    /* ---- D39 ---- 开机补读电量（M5 第六步） */
    {
        zh_dev_rec_t rb;
        memset(&rb, 0, sizeof(rb));
        rb.ver = ZH_DEV_REC_VER;
        memcpy(rb.ieee, EA, 8);
        rb.nwk = 0x3901;
        rb.node_type = ZH_NODE_END_DEVICE;
        rb.flags = ZH_DEVF_INTERVIEWED | ZH_DEVF_ANNCED;
        rb.n_ep = 1; rb.ep[0].ep = 2; rb.ep[0].profile = ZH_HA_PROFILE; rb.ep[0].n_in = 3;
        rb.ep[0].cl[0] = 0x0000; rb.ep[0].cl[1] = ZH_CL_POWER_CFG; rb.ep[0].cl[2] = ZH_CL_IAS_ZONE;
        rb.zone_type = ZH_ZONE_MOTION;
        fresh();
        zh_dev_load(&D, 0, &rb, 1000);
        zh_dev_tick(&D, 2000);
        CHECK(s_nsent == 0, "D39 开机后还没听到它：不读");
        spoke(0x3901, 5000);
        zh_dev_tick(&D, 5000 + ZH_IV_AWAKE_MS);
        CHECK(s_nsent == 0, "D39 说话已经过去 3 s：不读（它多半又睡了）");
        spoke(0x3901, 6000);
        zh_dev_tick(&D, 6100);
        CHECK(s_nsent == 1 && fr_cluster(last()) == ZH_CL_POWER_CFG && fr_profile(last()) == ZH_HA_PROFILE && fr_dst_ep(last()) == 2 && fr_pl_n(last()) == 7 &&
              fr_pl(last())[2] == ZH_ZCL_CMD_READ_ATTR && fr_pl(last())[3] == 0x21 && fr_pl(last())[5] == 0x20 && D.st.batt_reads == 1,
              "D39 刚说过话：读电量 %%（0x0021）与电压（0x0020），发到电源配置簇所在的端点");
        spoke(0x3901, 6200);
        zh_dev_tick(&D, 6300);
        CHECK(s_nsent == 1, "D39 不到 10 min：不再读");
        spoke(0x3901, 6100 + ZH_BATT_READ_GAP_MS);
        zh_dev_tick(&D, 6150 + ZH_BATT_READ_GAP_MS);
        CHECK(s_nsent == 2 && D.dev[0].batt_reads == 2, "D39 读了没回、满 10 min 又说话：再读");
        report(0x3901, ZH_CL_POWER_CFG, (const uint8_t[]){ 0x21, 0x00, ZH_ZCL_T_UINT8, 0xB4 }, 4, 6200 + ZH_BATT_READ_GAP_MS);
        spoke(0x3901, 6200 + 2u * ZH_BATT_READ_GAP_MS);
        zh_dev_tick(&D, 6250 + 2u * ZH_BATT_READ_GAP_MS);
        CHECK(s_nsent == 2 && (D.dev[0].st.have & (1u << ZH_ST_BATTERY_PCT)) && D.dev[0].st.battery_pct == 90, "D39 收到电量（90 %%）：不再读");

        fresh();                                                       /* 最多 3 次 */
        zh_dev_load(&D, 0, &rb, 1000);
        for (unsigned k = 0; k < 6; k++) { spoke(0x3901, 2000 + k * (ZH_BATT_READ_GAP_MS + 1)); zh_dev_tick(&D, 2010 + k * (ZH_BATT_READ_GAP_MS + 1)); }
        CHECK(s_nsent == (int)ZH_BATT_READ_TRIES && D.dev[0].batt_reads == ZH_BATT_READ_TRIES, "D39 一直没回：每次开机最多读 %u 次", ZH_BATT_READ_TRIES);

        fresh();                                                       /* 交不出去不占次数；网络不是 UP 不发 */
        zh_dev_load(&D, 0, &rb, 1000);
        s_send_ok = false;
        spoke(0x3901, 2000);
        zh_dev_tick(&D, 2010);
        CHECK(D.dev[0].batt_reads == 0 && D.st.batt_reads == 0, "D39 交不出去：不占次数");
        s_send_ok = true;
        zh_dev_set_net_up(&D, false);
        zh_dev_tick(&D, 2020);
        CHECK(s_nsent == 0, "D39 网络不是 UP：不发");
        zh_dev_set_net_up(&D, true);
        zh_dev_tick(&D, 2030);
        CHECK(s_nsent == 1, "D39 网络回来、它还醒着：发");

        fresh();                                                       /* 路由器 / 没有电源配置簇 / 没访谈完 / 离开了：不读 */
        zh_dev_rec_t r2 = rb;
        r2.node_type = ZH_NODE_ROUTER;
        zh_dev_load(&D, 0, &r2, 1000);
        r2 = rb; memcpy(r2.ieee, EB, 8); r2.nwk = 0x3902; r2.ep[0].cl[1] = 0x0006;
        zh_dev_load(&D, 1, &r2, 1000);
        r2 = rb; r2.ieee[0] = 0x99; r2.nwk = 0x3903; r2.flags = ZH_DEVF_INTERVIEWED | ZH_DEVF_LEFT;
        zh_dev_load(&D, 2, &r2, 1000);
        spoke(0x3901, 2000); spoke(0x3902, 2000); spoke(0x3903, 2000);
        for (int k = 0; k < 3; k++) zh_dev_tick(&D, 2010 + (uint32_t)k);
        bool any_pc = false;
        for (int k = 0; k < s_nsent; k++) any_pc |= (s_tag[k % 64] & 0xFu) == ZH_DEV_TAG_KIND_MISC && fr_cluster(k) == ZH_CL_POWER_CFG;   /* 只看 sendUnicast 帧：查询帧短，缓冲里是旧字节 */
        CHECK(!any_pc && D.st.batt_reads == 0, "D39 路由器 / 没有电源配置簇 / 离开了的：不读（离开了的说话会触发被动学习查询，那不是电量读）");
        fresh();                                                       /* 正在补 CIE / 配置的：不读（那几秒归那边用） */
        zh_dev_load(&D, 0, &rb, 1000);
        D.dev[0].iv = ZH_IV_REPORT;
        spoke(0x3901, 2000);
        zh_dev_tick(&D, 2010);
        CHECK(D.st.batt_reads == 0, "D39 正在配置 / 补做中的：不读");

        fresh();                                                       /* 一拍一条：两台同时醒着，两拍各一条 */
        zh_dev_load(&D, 0, &rb, 1000);
        r2 = rb; memcpy(r2.ieee, EB, 8); r2.nwk = 0x3904;
        zh_dev_load(&D, 1, &r2, 1000);
        spoke(0x3901, 2000); spoke(0x3904, 2000);
        zh_dev_tick(&D, 2010);
        CHECK(s_nsent == 1, "D39 两台同时醒着：这一拍只发一条");
        zh_dev_tick(&D, 2020);
        CHECK(s_nsent == 2 && D.st.batt_reads == 2, "D39 下一拍发另一台");
    }
    /* ---- D40 ---- 控制（设计 8.11；M5 第七步） */
    {
        zh_dev_rec_t rl;                                               /* 调光灯：路由器、OnOff + Level，端点 11 */
        memset(&rl, 0, sizeof(rl));
        rl.ver = ZH_DEV_REC_VER;
        memcpy(rl.ieee, EA, 8);
        rl.nwk = 0x4001;
        rl.node_type = ZH_NODE_ROUTER;
        rl.flags = ZH_DEVF_INTERVIEWED | ZH_DEVF_ANNCED | ZH_DEVF_RX_ON | ZH_DEVF_MAINS;
        rl.n_ep = 1; rl.ep[0].ep = 11; rl.ep[0].profile = ZH_HA_PROFILE; rl.ep[0].n_in = 3;
        rl.ep[0].cl[0] = 0x0000; rl.ep[0].cl[1] = ZH_CL_ONOFF; rl.ep[0].cl[2] = ZH_CL_LEVEL;
        #define DR(nwk, cl, sq, cmd, st, now) do { const uint8_t b_[5] = { ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR, (sq), ZH_ZCL_CMD_DEFAULT_RSP, (cmd), (st) }; \
            zh_dev_on_zcl(&D, (nwk), ZH_HA_PROFILE, (cl), 11, b_, sizeof(b_), (now)); } while (0)
        fresh();
        zh_dev_load(&D, 0, &rl, 1000);
        DR(0x4001, ZH_CL_ONOFF, 0, ZH_CL_CMD_TOGGLE, 0, 1500);   /* 没有在途的：不认（下面的切换测"读数不知道就不猜"） */
        CHECK(zh_dev_ctl_start(&D, EA, ZH_CTL_TOGGLE, 0, 1600) == ZH_CTL_E_OK, "D40 开关状态还不知道时切换：照发（ZCL 的 Toggle 设备自己知道）");
        DR(0x4001, ZH_CL_ONOFF, last_seq(), ZH_CL_CMD_TOGGLE, 0, 1650);
        CHECK(D.ctl.result == ZH_CTL_R_CONFIRMED && !(D.dev[0].st.have & (1u << ZH_ST_ONOFF)), "D40 切换确认了、但之前不知道开关：读数不猜");
        zh_dev_ctl_clear(&D);
        CHECK(zh_dev_ctl_start(&D, EA, ZH_CTL_ON, 0, 2000) == ZH_CTL_E_OK && D.ctl.active && s_nsent == 2, "D40 开灯：发出去了");
        const int k1 = last();
        CHECK(fr_profile(k1) == ZH_HA_PROFILE && fr_cluster(k1) == ZH_CL_ONOFF && fr_dst_ep(k1) == 11 && fr_dest(k1) == 0x4001 && fr_opts(k1) == ZH_APS_OPTS_DEFAULT &&
              fr_pl_n(k1) == 3 && fr_pl(k1)[0] == ZH_ZCL_FC_CLUSTER && fr_pl(k1)[2] == ZH_CL_CMD_ON && (s_tag[k1 % 64] & 0xFu) == ZH_DEV_TAG_KIND_CTL,
              "D40 帧：HA、OnOff 簇、端点 11、簇专用不禁默认应答（fc 0x01）、命令 On");
        const uint16_t mt = (uint16_t)(s_frame[k1 % 64][19] | (s_frame[k1 % 64][20] << 8));
        CHECK(mt == D.ctl.msg_tag && (mt & 0x8000u), "D40 messageTag = 0x8000 | 代号（不和访谈用的序号撞）");
        CHECK(zh_dev_ctl_start(&D, EA, ZH_CTL_OFF, 0, 2001) == ZH_CTL_E_BUSY, "D40 上一条在途：BUSY");
        DR(0x4001, ZH_CL_ONOFF, (uint8_t)(fr_pl(k1)[1] + 1), ZH_CL_CMD_ON, 0, 2100);
        DR(0x4001, ZH_CL_ONOFF, fr_pl(k1)[1], ZH_CL_CMD_OFF, 0, 2100);
        DR(0x4002, ZH_CL_ONOFF, fr_pl(k1)[1], ZH_CL_CMD_ON, 0, 2100);
        DR(0x4001, ZH_CL_LEVEL, fr_pl(k1)[1], ZH_CL_CMD_ON, 0, 2100);
        CHECK(D.ctl.result == ZH_CTL_R_NONE, "D40 序号 / 命令号 / 发送方 / 簇对不上的默认应答：不认");
        DR(0x4001, ZH_CL_ONOFF, fr_pl(k1)[1], ZH_CL_CMD_ON, 0, 2150);
        CHECK(D.ctl.result == ZH_CTL_R_CONFIRMED && D.ctl.t_done - D.ctl.t_sent == 150 && D.st.ctl_confirmed == 2, "D40 成功的默认应答：confirmed（150 ms）");
        CHECK((D.dev[0].st.have & (1u << ZH_ST_ONOFF)) && D.dev[0].st.onoff == 1 && D.dev[0].st.t[ZH_ST_ONOFF] == 2150 && D.dirty,
              "D40 默认应答成功：读数乐观写成开（回包不再带命令之前的缓存，2026-10-23 灯泡上板）");
        zh_dev_on_msg_sent(&D, D.ctl.msg_tag, 0x66, 2200);
        CHECK(D.ctl.result == ZH_CTL_R_CONFIRMED, "D40 有了结论之后迟到的 messageSent（哪怕报失败）：不改");
        zh_dev_ctl_clear(&D);
        CHECK(!D.ctl.active, "D40 取走结论：清掉");

        CHECK(zh_dev_ctl_start(&D, EA, ZH_CTL_LEVEL, 40, 3000) == ZH_CTL_E_OK, "D40 亮度 40 %%：发出去了");
        CHECK(fr_cluster(last()) == ZH_CL_LEVEL && fr_pl_n(last()) == 6 && fr_pl(last())[2] == ZH_CL_CMD_MOVE_TO_LEVEL_ONOFF && fr_pl(last())[3] == 102 &&
              fr_pl(last())[4] == 0 && fr_pl(last())[5] == 0, "D40 亮度：Level 簇、MoveToLevelWithOnOff、40 %% → 102（/254）、渐变 0");
        DR(0x4001, ZH_CL_LEVEL, last_seq(), ZH_CL_CMD_MOVE_TO_LEVEL_ONOFF, 0x81, 3100);
        CHECK(D.ctl.result == ZH_CTL_R_FAILED && D.ctl.zcl_status == 0x81 && !(D.dev[0].st.have & (1u << ZH_ST_LEVEL)),
              "D40 设备回错误（0x81）：failed、带 ZCL 状态、读数不动");
        zh_dev_ctl_clear(&D);
        zh_dev_ctl_start(&D, EA, ZH_CTL_LEVEL, 100, 3200);
        CHECK(fr_pl(last())[3] == 254, "D40 100 %% → 254");
        zh_dev_ctl_clear(&D);
        zh_dev_ctl_start(&D, EA, ZH_CTL_LEVEL, 0, 3300);
        CHECK(fr_pl(last())[3] == 0, "D40 0 %% → 0（带 OnOff 的命令：关）");
        zh_dev_ctl_clear(&D);

        zh_dev_ctl_start(&D, EA, ZH_CTL_TOGGLE, 0, 4000);              /* 只送达、没确认：窗口过了算 delivered */
        CHECK(fr_pl(last())[2] == ZH_CL_CMD_TOGGLE, "D40 切换：Toggle");
        zh_dev_on_msg_sent(&D, (uint16_t)(D.ctl.msg_tag ^ 1u), 0, 4100);
        CHECK(!D.ctl.delivered, "D40 别的 messageTag：不认");
        zh_dev_on_msg_sent(&D, D.ctl.msg_tag, 0, 4100);
        zh_dev_tick(&D, 4000 + ZH_CTL_CONFIRM_MS - 1);
        CHECK(D.ctl.delivered && D.ctl.result == ZH_CTL_R_NONE, "D40 送达了、还在确认窗口里：等");
        zh_dev_tick(&D, 4000 + ZH_CTL_CONFIRM_MS);
        CHECK(D.ctl.result == ZH_CTL_R_DELIVERED && D.st.ctl_delivered == 1, "D40 窗口过了只送达：delivered");
        zh_dev_ctl_clear(&D);

        zh_dev_ctl_start(&D, EA, ZH_CTL_OFF, 0, 5000);                 /* 窗口内来了这个簇的上报：confirmed */
        report(0x4001, ZH_CL_ONOFF, (const uint8_t[]){ 0x00, 0x00, ZH_ZCL_T_BOOL, 0x00 }, 4, 5000 + ZH_CTL_CONFIRM_MS - 1);
        CHECK(D.ctl.result == ZH_CTL_R_CONFIRMED && (D.dev[0].st.have & (1u << ZH_ST_ONOFF)) && D.dev[0].st.onoff == 0,
              "D40 命令后很快来了开关上报：confirmed，读数也更新成关");
        zh_dev_ctl_clear(&D);
        /* 乐观更新：切换按当时读数取反、亮度 > 0 = 开到这个亮度、亮度 0 = 关且亮度不变 */
        zh_dev_ctl_start(&D, EA, ZH_CTL_TOGGLE, 0, 5500);
        DR(0x4001, ZH_CL_ONOFF, last_seq(), ZH_CL_CMD_TOGGLE, 0, 5530);
        CHECK(D.dev[0].st.onoff == 1, "D40 切换确认：关 → 开");
        zh_dev_ctl_clear(&D);
        zh_dev_ctl_start(&D, EA, ZH_CTL_OFF, 0, 5600);
        DR(0x4001, ZH_CL_ONOFF, last_seq(), ZH_CL_CMD_OFF, 0, 5630);
        CHECK(D.dev[0].st.onoff == 0, "D40 关确认：关");
        zh_dev_ctl_clear(&D);
        zh_dev_ctl_start(&D, EA, ZH_CTL_LEVEL, 30, 5700);
        DR(0x4001, ZH_CL_LEVEL, last_seq(), ZH_CL_CMD_MOVE_TO_LEVEL_ONOFF, 0, 5730);
        CHECK(D.dev[0].st.onoff == 1 && (D.dev[0].st.have & (1u << ZH_ST_LEVEL)) && D.dev[0].st.level_pct == 30 && D.dev[0].st.t[ZH_ST_LEVEL] == 5730,
              "D40 亮度 30 %% 确认：开、30 %%");
        zh_dev_ctl_clear(&D);
        zh_dev_ctl_start(&D, EA, ZH_CTL_LEVEL, 0, 5800);
        DR(0x4001, ZH_CL_LEVEL, last_seq(), ZH_CL_CMD_MOVE_TO_LEVEL_ONOFF, 0, 5830);
        CHECK(D.dev[0].st.onoff == 0 && D.dev[0].st.level_pct == 30, "D40 亮度 0 确认：关、亮度留着 30 %%");
        zh_dev_ctl_clear(&D);
        {                                                              /* 只有亮度簇、没有开关簇的调光器：确认后只写亮度，开关位不写（审计补） */
            static const uint8_t EL[8] = { 0x4C, 0x40, 0, 0, 0, 0, 0, 0x40 };
            zh_dev_rec_t rd = rl;
            memcpy(rd.ieee, EL, 8);
            rd.nwk = 0x4011;
            rd.ep[0].n_in = 2; rd.ep[0].cl[1] = ZH_CL_LEVEL;
            zh_dev_load(&D, 5, &rd, 5900);
            CHECK(zh_dev_ctl_start(&D, EL, ZH_CTL_LEVEL, 70, 5900) == ZH_CTL_E_OK, "D40 只调光的：发出去了");
            DR(0x4011, ZH_CL_LEVEL, last_seq(), ZH_CL_CMD_MOVE_TO_LEVEL_ONOFF, 0, 5930);
            CHECK(D.ctl.result == ZH_CTL_R_CONFIRMED && D.dev[5].st.level_pct == 70 && (D.dev[5].st.have & (1u << ZH_ST_LEVEL)) &&
                  !(D.dev[5].st.have & (1u << ZH_ST_ONOFF)), "D40 只调光的确认：亮度 70 %%、开关位不写");
            zh_dev_ctl_clear(&D);
        }
        zh_dev_ctl_start(&D, EA, ZH_CTL_ON, 0, 6000);
        report(0x4001, ZH_CL_ONOFF, (const uint8_t[]){ 0x00, 0x00, ZH_ZCL_T_BOOL, 0x01 }, 4, 6000 + ZH_CTL_CONFIRM_MS);
        CHECK(D.ctl.result == ZH_CTL_R_NONE, "D40 窗口过了才来的上报：不当确认");
        zh_dev_ctl_clear(&D);

        zh_dev_ctl_start(&D, EA, ZH_CTL_ON, 0, 7000);                  /* 没送达 */
        zh_dev_on_msg_sent(&D, D.ctl.msg_tag, 0x66, 7100);
        CHECK(D.ctl.result == ZH_CTL_R_FAILED && D.ctl.status == 0x66, "D40 messageSent 报没送达：failed、带状态");
        zh_dev_ctl_clear(&D);
        zh_dev_ctl_start(&D, EA, ZH_CTL_ON, 0, 8000);                  /* sendUnicast 被 NCP 拒 */
        send_rsp(s_tag[last() % 64], 0x0072, 8010);
        CHECK(D.ctl.result == ZH_CTL_R_FAILED && D.ctl.status == 0x0072, "D40 sendUnicast 被拒：failed");
        zh_dev_ctl_clear(&D);
        zh_dev_ctl_start(&D, EA, ZH_CTL_ON, 0, 9000);                  /* NCP 收下了：等 */
        send_rsp(s_tag[last() % 64], 0, 9010);
        CHECK(D.ctl.result == ZH_CTL_R_NONE, "D40 sendUnicast 收下了：接着等");
        zh_dev_tick(&D, 9000 + ZH_CTL_DEADLINE_MS - 1);
        CHECK(D.ctl.result == ZH_CTL_R_NONE, "D40 一点回音没有、还没到期限：等");
        zh_dev_tick(&D, 9000 + ZH_CTL_DEADLINE_MS);
        CHECK(D.ctl.result == ZH_CTL_R_FAILED && D.ctl.status == 0, "D40 到期限一点回音都没有：failed");
        const uint32_t f0 = D.st.ctl_failed;
        send_rsp((uint32_t)ZH_DEV_TAG_KIND_CTL | ((uint32_t)D.ctl.gen << 8), 0x0072, 9100 + ZH_CTL_DEADLINE_MS);
        CHECK(D.st.ctl_failed == f0, "D40 有结论之后迟到的应用命令结果：不重复计");
        zh_dev_ctl_clear(&D);

        s_send_ok = false;                                             /* 交不出去 */
        CHECK(zh_dev_ctl_start(&D, EA, ZH_CTL_ON, 0, 10000) == ZH_CTL_E_SEND && !D.ctl.active, "D40 交不出去：SEND、不留在途");
        s_send_ok = true;
        zh_dev_set_net_up(&D, false);
        CHECK(zh_dev_ctl_start(&D, EA, ZH_CTL_ON, 0, 10000) == ZH_CTL_E_NET, "D40 网络不是 UP：NET");
        zh_dev_set_net_up(&D, true);
        CHECK(zh_dev_ctl_start(&D, EB, ZH_CTL_ON, 0, 10000) == ZH_CTL_E_NOT_FOUND && zh_dev_ctl_start(&D, EA, (zh_ctl_act_t)9, 0, 10000) == ZH_CTL_E_ARG &&
              zh_dev_ctl_start(&D, EA, ZH_CTL_LEVEL, 101, 10000) == ZH_CTL_E_ARG && zh_dev_ctl_start(NULL, EA, ZH_CTL_ON, 0, 10000) == ZH_CTL_E_ARG,
              "D40 没有这台 / 动作越界 / 亮度 > 100 / NULL");

        fresh();                                                       /* 只有 OnOff 的插座：亮度没有这个簇 */
        zh_dev_rec_t rp = rl;
        rp.ep[0].n_in = 2;
        zh_dev_load(&D, 0, &rp, 1000);
        CHECK(zh_dev_ctl_start(&D, EA, ZH_CTL_LEVEL, 50, 2000) == ZH_CTL_E_NO_CLUSTER && zh_dev_ctl_start(&D, EA, ZH_CTL_ON, 0, 2000) == ZH_CTL_E_OK,
              "D40 插座：亮度 NO_CLUSTER、开关照发");
        fresh();                                                       /* 休眠设备 / 离开了的 */
        zh_dev_rec_t rs = rl;
        rs.node_type = ZH_NODE_END_DEVICE;
        rs.flags = ZH_DEVF_INTERVIEWED | ZH_DEVF_ANNCED;
        zh_dev_load(&D, 0, &rs, 1000);
        CHECK(zh_dev_ctl_start(&D, EA, ZH_CTL_ON, 0, 2000) == ZH_CTL_E_OK && D.ctl.result == ZH_CTL_R_PENDING && s_nsent == 0, "D40 休眠设备：排进待发、立刻 PENDING（M5 第十二步，细节见 D46）");
        zh_dev_ctl_clear(&D);
        fresh();
        rs = rl;
        rs.flags |= ZH_DEVF_LEFT;
        zh_dev_load(&D, 0, &rs, 1000);
        CHECK(zh_dev_ctl_start(&D, EA, ZH_CTL_ON, 0, 2000) == ZH_CTL_E_LEFT, "D40 离开了的：LEFT");
        #undef DR
    }

    /* ---- D41 ---- 涂鸦私有簇的读数（M5 第八步） */
    {
        #define TUYA(nwk, t, ...) do { const uint8_t tp_[] = { 0x01, 0x00, __VA_ARGS__ }; \
            zcl_frame((nwk), ZH_CL_TUYA, ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR, ZH_TUYA_CMD_DATA_REPORT, tp_, sizeof(tp_), 1, (t)); } while (0)
        #define HAS(i, f) ((D.dev[(i)].st.have & (1u << (f))) != 0)
        static const uint8_t E1[8] = { 1, 0x41, 0, 0, 0, 0, 0, 0x41 }, E2[8] = { 2, 0x41, 0, 0, 0, 0, 0, 0x41 }, E3[8] = { 3, 0x41, 0, 0, 0, 0, 0, 0x41 },
                             E4[8] = { 4, 0x41, 0, 0, 0, 0, 0, 0x41 }, E5[8] = { 5, 0x41, 0, 0, 0, 0, 0, 0x41 };
        zh_dev_rec_t rt;
        memset(&rt, 0, sizeof(rt));
        rt.ver = ZH_DEV_REC_VER;
        rt.node_type = ZH_NODE_END_DEVICE;
        rt.flags = ZH_DEVF_INTERVIEWED | ZH_DEVF_ANNCED;
        rt.n_ep = 1; rt.ep[0].ep = 1; rt.ep[0].profile = ZH_HA_PROFILE; rt.ep[0].n_in = 2; rt.ep[0].cl[0] = 0x0000; rt.ep[0].cl[1] = ZH_CL_TUYA;
        rt.zone_type = ZH_ZONE_TYPE_UNKNOWN;
        strcpy(rt.model, "TS0601");
        zh_dev_rec_t r1 = rt, r2 = rt, r3 = rt, r4 = rt, r5 = rt;
        memcpy(r1.ieee, E1, 8); r1.nwk = 0x4101; strcpy(r1.manuf, "_TZE200_bjawzodf");   /* 温湿度（老式手写转换器） */
        memcpy(r2.ieee, E2, 8); r2.nwk = 0x4102; strcpy(r2.manuf, "_TZE200_pay2byax");   /* 门磁 */
        memcpy(r3.ieee, E3, 8); r3.nwk = 0x4103; strcpy(r3.manuf, "_TZE200_ntcy3xu1");   /* 烟雾 + 防拆 + 电量状态 */
        memcpy(r4.ieee, E4, 8); r4.nwk = 0x4104; strcpy(r4.manuf, "_TZE200_3towulqd");   /* 人体（占用） */
        memcpy(r5.ieee, E5, 8); r5.nwk = 0x4105; strcpy(r5.manuf, "_TZE200_zzzzzzzz");   /* 不在表里 */
        fresh();
        CHECK(zh_dev_load(&D, 0, &r1, 1000) && zh_dev_load(&D, 1, &r2, 1000) && zh_dev_load(&D, 2, &r3, 1000) && zh_dev_load(&D, 3, &r4, 1000) && zh_dev_load(&D, 4, &r5, 1000), "D41 加载");
        CHECK(zh_dev_tuya_dev(&r1) >= 0 && zh_dev_tuya_dev(&r5) < 0, "D41 查表：bjawzodf 在、zzzzzzzz 不在");
        CHECK(zh_dev_zone_type(&r1) == ZH_ZONE_TYPE_UNKNOWN && zh_dev_zone_type(&r2) == ZH_ZONE_CONTACT && zh_dev_zone_type(&r3) == ZH_ZONE_FIRE &&
              zh_dev_zone_type(&r4) == ZH_ZONE_TYPE_UNKNOWN && zh_dev_zone_type(&r5) == ZH_ZONE_TYPE_UNKNOWN, "D41 有效 ZoneType：温湿度 / 人体没有、门磁 CONTACT、烟雾 FIRE");
        zh_dev_rec_t ri = r2;
        ri.ep[0].cl[1] = ZH_CL_IAS_ZONE; ri.zone_type = ZH_ZONE_WATER;
        CHECK(zh_dev_zone_type(&ri) == ZH_ZONE_WATER, "D41 有 IAS Zone 输入簇的看记录（哪怕厂商名在涂鸦表里）");
        zh_dev_rec_t rz = r1;
        rz.zone_type = ZH_ZONE_MOTION;
        CHECK(zh_dev_zone_type(&rz) == ZH_ZONE_MOTION, "D41 不是涂鸦报警类、记录里有 ZoneType（IAS 簇被截掉 / 登记请求补的）：照记录");
        ri.flags = 0;
        CHECK(zh_dev_tuya_dev(&ri) < 0, "D41 没访谈完的记录不查表");

        /* 温湿度：dp1 温度 21.5 °C、dp2 湿度 45.5 %（这一家 ÷10）、dp4 电量 87 % */
        const uint32_t rep0 = D.st.reports;
        TUYA(0x4101, 2000, 1, ZH_TUYA_T_NUMBER, 0, 4, 0, 0, 0, 215, 2, ZH_TUYA_T_NUMBER, 0, 4, 0, 0, 0x01, 0xC7, 4, ZH_TUYA_T_NUMBER, 0, 4, 0, 0, 0, 87);
        CHECK(D.dev[0].st.temp_c100 == 2150 && D.dev[0].st.hum_c100 == 4550 && D.dev[0].st.battery_pct == 87 && HAS(0, ZH_ST_TEMP) && HAS(0, ZH_ST_HUMIDITY) &&
              HAS(0, ZH_ST_BATTERY_PCT) && D.st.reports == rep0 + 3 && D.st.tuya_frames == 1 && s_ev[ZH_DEV_EV_STATE] == 0, "D41 温湿度：三个读数进 zh_state、不出事件");
        TUYA(0x4101, 2100, 1, ZH_TUYA_T_NUMBER, 0, 4, 0, 0, 0xFF, 0xF6);
        CHECK(D.dev[0].st.temp_c100 == -100, "D41 负温度（低 16 位补码）−1.0 °C");
        TUYA(0x4101, 2150, 1, ZH_TUYA_T_NUMBER, 0, 4, 0, 0, 0x7F, 0xFF, 2, ZH_TUYA_T_NUMBER, 0, 4, 0, 0, 0x05, 0xDC);
        CHECK(D.dev[0].st.temp_c100 == 32767 && D.dev[0].st.hum_c100 == 10000, "D41 越界的读数封顶（温度 3276.7 °C → int16 上限、湿度 150 %% → 100 %%）");
        TUYA(0x4101, 2160, 1, ZH_TUYA_T_NUMBER, 0, 4, 0xFF, 0xFF, 0x80, 0x00);
        CHECK(D.dev[0].st.temp_c100 == -27315, "D41 低于绝对零度的封在 −273.15 °C");
        /* 电量状态 dp3 = 0（低）：这台没有主报警字段 → 只出事件，不置 ZONE 读数 */
        TUYA(0x4101, 2200, 3, ZH_TUYA_T_ENUM, 0, 1, 0);
        CHECK(s_ev[ZH_DEV_EV_STATE] == 1 && D.dev[0].st_ev == ZH_STEV_BATTERY_LOW && !HAS(0, ZH_ST_ZONE), "D41 电量低：出事件、不置 ZONE 读数");
        TUYA(0x4101, 2300, 3, ZH_TUYA_T_ENUM, 0, 1, 0);
        CHECK(s_ev[ZH_DEV_EV_STATE] == 1, "D41 还是低：不重复出");
        TUYA(0x4101, 2400, 3, ZH_TUYA_T_ENUM, 0, 1, 2);
        TUYA(0x4101, 2500, 3, ZH_TUYA_T_ENUM, 0, 1, 0);
        CHECK(s_ev[ZH_DEV_EV_STATE] == 2 && D.dev[0].st_ev == ZH_STEV_BATTERY_LOW, "D41 恢复后又低：再出（不再\"每天一条\"：网关不做人为干预，2026-10-24）");

        /* 门磁：头一帧只有电量 → 不置 ZONE（不假装是关着的） */
        TUYA(0x4102, 3000, 2, ZH_TUYA_T_NUMBER, 0, 4, 0, 0, 0, 50);
        CHECK(D.dev[1].st.battery_pct == 50 && !HAS(1, ZH_ST_ZONE), "D41 门磁头一帧只有电量：不置 ZONE");
        TUYA(0x4102, 3100, 1, ZH_TUYA_T_BOOL, 0, 1, 1);
        CHECK(HAS(1, ZH_ST_ZONE) && (D.dev[1].st.zone_status & ZH_IAS_ST_ALARM1) && s_ev[ZH_DEV_EV_STATE] == 3 && D.dev[1].st_ev == ZH_STEV_ALARM_ON, "D41 门开：ZONE alarm1、出 ALARM_ON");
        TUYA(0x4102, 3200, 1, ZH_TUYA_T_BOOL, 0, 1, 0);
        CHECK(!(D.dev[1].st.zone_status & ZH_IAS_ST_ALARM1) && s_ev[ZH_DEV_EV_STATE] == 4 && D.dev[1].st_ev == ZH_STEV_ALARM_OFF, "D41 门关：ALARM_OFF");
        TUYA(0x4102, 3300, 2, ZH_TUYA_T_NUMBER, 0, 4, 0, 0, 0, 10);
        CHECK(s_ev[ZH_DEV_EV_STATE] == 5 && D.dev[1].st_ev == ZH_STEV_BATTERY_LOW && D.dev[1].st.battery_pct == 10, "D41 电量 %% 降到 15 以下：走 st_batt_pct 出电量低");
        const uint32_t tz = D.dev[1].st.t[ZH_ST_ZONE], rp = D.st.reports;
        TUYA(0x4102, 3400, 2, ZH_TUYA_T_NUMBER, 0, 4, 0, 0, 0x01, 0x2C);
        CHECK(D.dev[1].st.battery_pct == 100, "D41 电量 %% 超过 100（300）封在 100");
        CHECK(D.dev[1].st.t[ZH_ST_ZONE] == tz && D.st.reports == rp + 1, "D41 只有电量 %% 的帧：不结算 ZoneStatus（ZONE 的时刻不动、只算一条读数）");

        /* 烟雾：先报电量低（只出事件）→ 有烟（置 ZONE；电量位带上但不再出一次电量低）→ 防拆 */
        TUYA(0x4103, 4000, 14, ZH_TUYA_T_ENUM, 0, 1, 0);
        CHECK(s_ev[ZH_DEV_EV_STATE] == 6 && D.dev[2].st_ev == ZH_STEV_BATTERY_LOW && !HAS(2, ZH_ST_ZONE), "D41 烟雾头一帧电量低：只出事件");
        /* 防拆没有每天一条的间隔：先报过的防拆，主报警头一回进 ZONE 时不能再出一次 */
        {
            static const uint8_t E7[8] = { 7, 0x41, 0, 0, 0, 0, 0, 0x41 };
            zh_dev_rec_t r7 = r3;
            memcpy(r7.ieee, E7, 8); r7.nwk = 0x4107;
            CHECK(zh_dev_load(&D, 6, &r7, 1000), "D41 第二台烟雾");
            const int ev0 = s_ev[ZH_DEV_EV_STATE];
            TUYA(0x4107, 4050, 4, ZH_TUYA_T_BOOL, 0, 1, 1);
            CHECK(s_ev[ZH_DEV_EV_STATE] == ev0 + 1 && D.dev[6].st_ev == ZH_STEV_TAMPER && !HAS(6, ZH_ST_ZONE), "D41 先报防拆：只出 TAMPER");
            TUYA(0x4107, 4060, 1, ZH_TUYA_T_ENUM, 0, 1, 1);
            CHECK(s_ev[ZH_DEV_EV_STATE] == ev0 + 1 && HAS(6, ZH_ST_ZONE) && D.dev[6].st.zone_status == ZH_IAS_ST_TAMPER,
                  "D41 随后报\"没有烟\"：ZONE = 防拆位，不再出一次 TAMPER（之前按合成值）");
            s_ev[ZH_DEV_EV_STATE] = ev0;
        }
        TUYA(0x4103, 4100, 1, ZH_TUYA_T_ENUM, 0, 1, 0);
        CHECK(HAS(2, ZH_ST_ZONE) && D.dev[2].st.zone_status == (ZH_IAS_ST_ALARM1 | ZH_IAS_ST_BATTERY) && s_ev[ZH_DEV_EV_STATE] == 7 && D.dev[2].st_ev == ZH_STEV_ALARM_ON,
              "D41 有烟（枚举 0）：ZONE = alarm1 + 电量位，只出 ALARM_ON（电量低不再出一次）");
        TUYA(0x4103, 4200, 4, ZH_TUYA_T_BOOL, 0, 1, 1);
        CHECK(s_ev[ZH_DEV_EV_STATE] == 8 && D.dev[2].st_ev == ZH_STEV_TAMPER && (D.dev[2].st.zone_status & ZH_IAS_ST_TAMPER) && (D.dev[2].st.zone_status & ZH_IAS_ST_ALARM1),
              "D41 防拆：TAMPER 事件，alarm1 保持（只改这一位）");
        TUYA(0x4103, 4300, 1, ZH_TUYA_T_ENUM, 0, 1, 1, 4, ZH_TUYA_T_BOOL, 0, 1, 0);
        CHECK(s_ev[ZH_DEV_EV_STATE] == 9 && D.dev[2].st_ev == ZH_STEV_ALARM_OFF && D.dev[2].st.zone_status == ZH_IAS_ST_BATTERY, "D41 一帧两条：烟散了、防拆复位 → 一个 ALARM_OFF");

        /* 门磁 + 振动二合一（_TZE200_jfw0a4aa：dp1 振动、dp101 门磁）：主报警是门磁，振动不进 alarm1 */
        {
            static const uint8_t E6[8] = { 6, 0x41, 0, 0, 0, 0, 0, 0x41 };
            zh_dev_rec_t r6 = rt;
            memcpy(r6.ieee, E6, 8); r6.nwk = 0x4106; strcpy(r6.manuf, "_TZE200_jfw0a4aa");
            CHECK(zh_dev_load(&D, 5, &r6, 1000) && zh_dev_zone_type(&r6) == ZH_ZONE_CONTACT, "D41 二合一：有效 ZoneType 是门磁");
            const int ev0 = s_ev[ZH_DEV_EV_STATE];
            TUYA(0x4106, 4500, 1, ZH_TUYA_T_BOOL, 0, 1, 1);
            CHECK(s_ev[ZH_DEV_EV_STATE] == ev0 && !HAS(5, ZH_ST_ZONE), "D41 二合一的振动：不进 alarm1、不出事件（不会被报成门开）");
            TUYA(0x4106, 4600, 101, ZH_TUYA_T_BOOL, 0, 1, 1);
            CHECK(s_ev[ZH_DEV_EV_STATE] == ev0 + 1 && D.dev[5].st_ev == ZH_STEV_ALARM_ON && HAS(5, ZH_ST_ZONE), "D41 二合一的门磁：门开");
            TUYA(0x4106, 4700, 101, ZH_TUYA_T_BOOL, 0, 1, 0, 1, ZH_TUYA_T_BOOL, 0, 1, 1);
            CHECK(s_ev[ZH_DEV_EV_STATE] == ev0 + 2 && D.dev[5].st_ev == ZH_STEV_ALARM_OFF && !(D.dev[5].st.zone_status & ZH_IAS_ST_ALARM1),
                  "D41 同一帧先门关、再振动：门关（振动不会把 alarm1 又置上）");
            s_ev[ZH_DEV_EV_STATE] = ev0;                 /* 后面的计数照旧 */
        }
        /* 同时有 IAS 簇的涂鸦设备：ZoneStatus 归 IAS 那路，涂鸦帧里的报警类 / 电量低不合成（审计 ⚠️） */
        {
            static const uint8_t E8[8] = { 8, 0x41, 0, 0, 0, 0, 0, 0x41 };
            zh_dev_rec_t r8 = r2;
            memcpy(r8.ieee, E8, 8); r8.nwk = 0x4108;
            r8.ep[0].n_in = 3; r8.ep[0].cl[2] = ZH_CL_IAS_ZONE; r8.zone_type = ZH_ZONE_TYPE_UNKNOWN;
            CHECK(zh_dev_zone_type(&r8) == ZH_ZONE_CONTACT, "D41 有 IAS 簇但 ZoneType 还不知道：退回查表");
            r8.zone_type = ZH_ZONE_WATER;
            CHECK(zh_dev_zone_type(&r8) == ZH_ZONE_WATER, "D41 有 IAS 簇、ZoneType 知道：照记录");
            CHECK(zh_dev_load(&D, 7, &r8, 1000), "D41 混合设备加载");
            const int ev0 = s_ev[ZH_DEV_EV_STATE];
            TUYA(0x4108, 4800, 1, ZH_TUYA_T_BOOL, 0, 1, 1, 2, ZH_TUYA_T_NUMBER, 0, 4, 0, 0, 0, 33);
            CHECK(s_ev[ZH_DEV_EV_STATE] == ev0 && !HAS(7, ZH_ST_ZONE) && D.dev[7].st.battery_pct == 33, "D41 混合设备的涂鸦帧：报警类不进 ZONE、不出事件，电量 %% 照收");
            zone_notify(0x4108, ZH_IAS_ST_ALARM1, 4900);
            CHECK(s_ev[ZH_DEV_EV_STATE] == ev0 + 1 && D.dev[7].st_ev == ZH_STEV_ALARM_ON, "D41 混合设备的 IAS 通知照常");
            TUYA(0x4108, 5000, 1, ZH_TUYA_T_BOOL, 0, 1, 0);
            CHECK(s_ev[ZH_DEV_EV_STATE] == ev0 + 1 && (D.dev[7].st.zone_status & ZH_IAS_ST_ALARM1), "D41 涂鸦帧不会把 IAS 的报警盖掉");
            s_ev[ZH_DEV_EV_STATE] = ev0;
        }
        /* 人体：占用走 st_occ（每次有人都出） */
        TUYA(0x4104, 5000, 1, ZH_TUYA_T_ENUM, 0, 1, 0);
        CHECK(D.dev[3].st.occupancy == 1 && s_ev[ZH_DEV_EV_STATE] == 10 && D.dev[3].st_ev == ZH_STEV_OCC_ON, "D41 有人（枚举 0）：OCC_ON");
        TUYA(0x4104, 5100, 1, ZH_TUYA_T_ENUM, 0, 1, 1);
        CHECK(D.dev[3].st.occupancy == 0 && s_ev[ZH_DEV_EV_STATE] == 11 && D.dev[3].st_ev == ZH_STEV_OCC_OFF, "D41 没人：OCC_OFF");
        TUYA(0x4104, 5200, 1, ZH_TUYA_T_ENUM, 0, 1, 0);
        CHECK(D.dev[3].st.occupancy == 1 && s_ev[ZH_DEV_EV_STATE] == 12 && D.dev[3].st_ev == ZH_STEV_OCC_ON, "D41 0.1 s 后又有人：照出 OCC_ON");

        /* 不在表里 / 表里没有的 dp / 截断 / 不是读数的命令 / 带厂商位 / 全局命令 */
        const uint32_t unk0 = D.st.tuya_unknown, unm0 = D.st.tuya_unmapped, fr0 = D.st.tuya_frames, bad0 = D.st.report_bad;
        TUYA(0x4105, 6000, 1, ZH_TUYA_T_NUMBER, 0, 4, 0, 0, 0, 1);
        CHECK(D.st.tuya_unknown == unk0 + 1 && D.dev[4].st.have == 0, "D41 不在表里：只计数");
        TUYA(0x4101, 6100, 200, ZH_TUYA_T_NUMBER, 0, 4, 0, 0, 0, 1, 1, ZH_TUYA_T_STRING, 0, 2, 'a', 'b');
        CHECK(D.st.tuya_unmapped == unm0 + 2, "D41 表里没有的 dp / 解不成整数的类型：计 unmapped");
        TUYA(0x4101, 6200, 4, ZH_TUYA_T_NUMBER, 0, 4, 0, 0, 0, 55, 1, ZH_TUYA_T_NUMBER, 0, 4, 0);
        CHECK(D.dev[0].st.battery_pct == 55 && D.st.report_bad == bad0 + 1, "D41 最后一条截断：前面的照收、计 report_bad");
        const uint8_t sync[] = { 0x00, 0x01 };
        zcl_frame(0x4101, ZH_CL_TUYA, ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_S2C, 0x24, sync, sizeof(sync), 1, 6300);
        zcl_frame(0x4101, ZH_CL_TUYA, ZH_ZCL_FC_CLUSTER, ZH_TUYA_CMD_DATA_REPORT, sync, sizeof(sync), 1, 6300);   /* 方向不对 */
        const uint8_t dr[] = { ZH_TUYA_CMD_DATA_REPORT, 0x00 };
        zcl_frame(0x4101, ZH_CL_TUYA, ZH_ZCL_FC_S2C, ZH_ZCL_CMD_DEFAULT_RSP, dr, sizeof(dr), 1, 6300);
        CHECK(D.st.tuya_frames == fr0 + 3, "D41 对时请求 / 客户端方向 / 全局的默认应答：不是读数（tuya_frames 不动）");
        const uint8_t mfg[] = { 0x02, 0x10, 0x5A, ZH_TUYA_CMD_DATA_REPORT, 0x01, 0x00, 4, ZH_TUYA_T_NUMBER, 0, 4, 0, 0, 0, 66 };   /* fc 带厂商位 + 厂商码 0x1002 */
        uint8_t mb[32] = { ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_S2C | ZH_ZCL_FC_MFG };
        memcpy(mb + 1, mfg, sizeof(mfg));
        zh_dev_on_zcl(&D, 0x4101, ZH_HA_PROFILE, ZH_CL_TUYA, 1, mb, 1 + sizeof(mfg), 6400);
        CHECK(D.dev[0].st.battery_pct == 66, "D41 带厂商位的涂鸦帧也照收");
        /* 没访谈完的：不查表、不缓存（访谈完了再查） */
        D.dev[0].rec.flags &= (uint8_t)~ZH_DEVF_INTERVIEWED;
        D.dev[0].tuya_k = 0;
        TUYA(0x4101, 6500, 4, ZH_TUYA_T_NUMBER, 0, 4, 0, 0, 0, 77);
        CHECK(D.dev[0].st.battery_pct == 66 && D.dev[0].tuya_k == 0, "D41 没访谈完：不收、不缓存查找结果");
        D.dev[0].rec.flags |= ZH_DEVF_INTERVIEWED;
        TUYA(0x4101, 6600, 4, ZH_TUYA_T_NUMBER, 0, 4, 0, 0, 0, 77);
        CHECK(D.dev[0].st.battery_pct == 77 && D.dev[0].tuya_k >= 2, "D41 访谈完：查表、缓存");
        /* 重新配对：访谈重来，查找结果、合成的 ZoneStatus、招呼都清 */
        D.dev[2].tuya_hello = 2;
        zh_dev_on_tc_join(&D, 0x4103, E3, ZH_JOIN_UNSECURED_JOIN, ZH_JOIN_USE_PRECONFIGURED_KEY, 0, 7000);
        CHECK(D.dev[2].tuya_k == 0 && D.dev[2].tbl_zs == 0 && D.dev[2].tuya_hello == 0 && D.dev[2].st.have == 0, "D41 重新配对：查找缓存 / 合成 ZoneStatus / 招呼 / 读数都清");
    }

    /* ---- D42 ---- 涂鸦设备的开机招呼 */
    {
        static const uint8_t E1[8] = { 1, 0x42, 0, 0, 0, 0, 0, 0x42 }, E2[8] = { 2, 0x42, 0, 0, 0, 0, 0, 0x42 }, E3[8] = { 3, 0x42, 0, 0, 0, 0, 0, 0x42 };
        zh_dev_rec_t rt;
        memset(&rt, 0, sizeof(rt));
        rt.ver = ZH_DEV_REC_VER;
        rt.node_type = ZH_NODE_END_DEVICE;
        rt.flags = ZH_DEVF_INTERVIEWED | ZH_DEVF_ANNCED;
        rt.n_ep = 1; rt.ep[0].ep = 1; rt.ep[0].profile = ZH_HA_PROFILE; rt.ep[0].n_in = 2; rt.ep[0].cl[0] = 0x0000; rt.ep[0].cl[1] = ZH_CL_TUYA;
        rt.zone_type = ZH_ZONE_TYPE_UNKNOWN;
        strcpy(rt.model, "TS0601");
        zh_dev_rec_t r1 = rt, r2 = rt, r3 = rt;
        memcpy(r1.ieee, E1, 8); r1.nwk = 0x4201; strcpy(r1.manuf, "_TZE200_bjawzodf");
        memcpy(r2.ieee, E2, 8); r2.nwk = 0x4202; strcpy(r2.manuf, "_TZE200_zzzzzzzz");   /* 不在表里 */
        memcpy(r3.ieee, E3, 8); r3.nwk = 0x4203; strcpy(r3.manuf, "_TZE200_swaamsoy");   /* 调光器：路由器，常电 */
        r3.node_type = ZH_NODE_ROUTER; r3.flags |= ZH_DEVF_RX_ON | ZH_DEVF_MAINS;
        r3.ep[0].ep = 2;
        fresh();
        zh_dev_load(&D, 0, &r1, 1000);
        zh_dev_load(&D, 1, &r2, 1000);
        zh_dev_set_net_up(&D, false);
        zh_dev_tick(&D, 1500);
        CHECK(s_nsent == 0, "D42 网络不是 UP：不招呼");
        zh_dev_set_net_up(&D, true);
        zh_dev_tick(&D, 2000);
        CHECK(s_nsent == 0, "D42 休眠的还没说话：不招呼");
        spoke(0x4202, 2500);
        zh_dev_tick(&D, 2600);
        CHECK(s_nsent == 0, "D42 不在表里的：不招呼");
        spoke(0x4201, 3000);
        zh_dev_tick(&D, 3100);
        CHECK(s_nsent == 1 && fr_cluster(last()) == ZH_CL_BASIC && fr_dst_ep(last()) == 1 && fr_pl_n(last()) == 15 && fr_pl(last())[2] == ZH_ZCL_CMD_READ_ATTR &&
              memcmp(fr_pl(last()) + 3, (const uint8_t[]){ 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x05, 0x00, 0x07, 0x00, 0xFE, 0xFF }, 12) == 0 &&
              (s_tag[last() % 64] & 0x0F) == ZH_DEV_TAG_KIND_MISC && !s_idem[last() % 64], "D42 刚说过话：magic packet（读 Basic 0x0004/0000/0001/0005/0007/FFFE，第一个端点）");
        zh_dev_tick(&D, 3200);
        CHECK(s_nsent == 2 && fr_cluster(last()) == ZH_CL_TUYA && fr_dst_ep(last()) == 1 && fr_pl_n(last()) == 3 &&
              fr_pl(last())[0] == (ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_DIS_DR) && fr_pl(last())[2] == ZH_TUYA_CMD_DATA_QUERY && D.st.tuya_hellos == 2,
              "D42 下一拍：dataQuery（簇专用、客户端 → 服务端、不要默认应答、无负载）");
        spoke(0x4201, 4000);
        zh_dev_tick(&D, 4100);
        CHECK(s_nsent == 2, "D42 一轮打完：这次开机不再招呼");
        /* 常电的路由器：不用等它说话；交不出去不占次数；访谈 / 配置进行中不发 */
        fresh();
        zh_dev_load(&D, 0, &r3, 1000);
        s_send_ok = false;
        zh_dev_tick(&D, 2000);
        CHECK(D.dev[0].tuya_hello == 0 && D.st.tuya_hellos == 0, "D42 交不出去：不占次数");
        s_send_ok = true;
        D.dev[0].iv = ZH_IV_FAILED;
        zh_dev_tick(&D, 2100);
        CHECK(s_nsent == 0, "D42 访谈没完（这里是失败等重来）：不发");
        D.dev[0].iv = ZH_IV_NONE;
        zh_dev_tick(&D, 2200);
        CHECK(s_nsent == 1 && fr_cluster(last()) == ZH_CL_BASIC && fr_dst_ep(last()) == 2, "D42 常电的：不用等它说话就招呼（发到它的第一个端点）");
        zh_dev_tick(&D, 2300);
        CHECK(s_nsent == 2 && fr_cluster(last()) == ZH_CL_TUYA && fr_dst_ep(last()) == 2, "D42 dataQuery 发到有 0xEF00 的端点");
        #undef TUYA
        #undef HAS
    }

    /* ---- D43 ---- 涂鸦设备的控制（M5 第九步） */
    {
        static const uint8_t E1[8] = { 1, 0x43, 0, 0, 0, 0, 0, 0x43 };
        zh_dev_rec_t rd;                                               /* _TZE200_swaamsoy 调光器：路由器，端点 1 只有 Basic / 0xEF00 */
        memset(&rd, 0, sizeof(rd));
        rd.ver = ZH_DEV_REC_VER;
        memcpy(rd.ieee, E1, 8);
        rd.nwk = 0x4301;
        rd.node_type = ZH_NODE_ROUTER;
        rd.flags = ZH_DEVF_INTERVIEWED | ZH_DEVF_ANNCED | ZH_DEVF_RX_ON | ZH_DEVF_MAINS;
        rd.n_ep = 1; rd.ep[0].ep = 1; rd.ep[0].profile = ZH_HA_PROFILE; rd.ep[0].n_in = 2; rd.ep[0].cl[0] = 0x0000; rd.ep[0].cl[1] = ZH_CL_TUYA;
        rd.zone_type = ZH_ZONE_TYPE_UNKNOWN;
        strcpy(rd.manuf, "_TZE200_swaamsoy");
        strcpy(rd.model, "TS0601");
        #define TREP(t, ...) do { const uint8_t tp_[] = { 0x00, 0x00, __VA_ARGS__ }; \
            zcl_frame(0x4301, ZH_CL_TUYA, ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR, ZH_TUYA_CMD_DATA_RSP, tp_, sizeof(tp_), 1, (t)); } while (0)
        fresh();
        zh_dev_load(&D, 0, &rd, 1000);
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_ON, 0, 2000) == ZH_CTL_E_OK && s_nsent == 1 && D.ctl.cluster == ZH_CL_TUYA && D.ctl.tuya_dp == 1 && D.st.ctl_tuya == 1,
              "D43 开：走涂鸦路（没有 ZCL 的 OnOff 簇），确认看 dp1");
        int k = last();
        CHECK(fr_cluster(k) == ZH_CL_TUYA && fr_dst_ep(k) == 1 && fr_pl_n(k) == 10 && fr_pl(k)[0] == (ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_DIS_DR) && fr_pl(k)[2] == ZH_TUYA_CMD_DATA_REQUEST &&
              fr_pl(k)[3] == 0 && fr_pl(k)[4] == 0 && memcmp(fr_pl(k) + 5, (const uint8_t[]){ 1, ZH_TUYA_T_BOOL, 0, 1, 1 }, 5) == 0,
              "D43 帧：0xEF00、端点 1、fc 0x11（照 z2m 不要默认应答）、dataRequest、[seq 0][dp1 布尔 1]");
        TREP(2100, 2, ZH_TUYA_T_NUMBER, 0, 4, 0, 0, 0x01, 0xF4);
        CHECK(D.ctl.result == ZH_CTL_R_NONE && D.dev[0].st.level_pct == 50, "D43 回报的是别的 dp（亮度）：读数照收，但不算确认");
        { const uint8_t sy[] = { 0x00, 0x00, 1, ZH_TUYA_T_BOOL, 0, 1, 1 };   /* 对时请求 0x24，负载碰巧长得像 dp1 */
          zcl_frame(0x4301, ZH_CL_TUYA, ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_S2C, 0x24, sy, sizeof(sy), 1, 2120); }
        CHECK(D.ctl.result == ZH_CTL_R_NONE, "D43 不是读数的命令（对时请求）哪怕带着同一个 dp：不算确认");
        TREP(2150, 1, ZH_TUYA_T_BOOL, 0, 1, 1);
        CHECK(D.ctl.result == ZH_CTL_R_CONFIRMED && D.st.ctl_confirmed == 1 && D.dev[0].st.onoff == 1, "D43 回报了 dp1：confirmed，读数也更新");
        zh_dev_ctl_clear(&D);
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_LEVEL, 50, 3000) == ZH_CTL_E_OK && D.ctl.tuya_dp == 2, "D43 亮度 50 %%：确认看 dp2");
        k = last();
        CHECK(fr_pl_n(k) == 3 + 2 + 5 + 8 && fr_pl(k)[3] == 1 && fr_pl(k)[4] == 0 &&
              memcmp(fr_pl(k) + 5, (const uint8_t[]){ 1, ZH_TUYA_T_BOOL, 0, 1, 1, 2, ZH_TUYA_T_NUMBER, 0, 4, 0, 0, 0x01, 0xF4 }, 13) == 0,
              "D43 亮度帧：seq 递增到 1、同一帧先开（dp1）再设 dp2 = 500");
        TREP(3100, 1, ZH_TUYA_T_BOOL, 0, 1, 1);
        CHECK(D.ctl.result == ZH_CTL_R_NONE, "D43 只回报了开关：亮度那条还没确认");
        TREP(3150, 2, ZH_TUYA_T_NUMBER, 0, 4, 0, 0, 0x00, 0xC8);
        CHECK(D.ctl.result == ZH_CTL_R_NONE, "D43 回报了 dp2 但值是旧的 200（不是写的 500）：不算确认（审计 ⚠️：全量回报 / 旧亮度）");
        zh_dev_on_msg_sent(&D, D.ctl.msg_tag, 0, 3200);
        zh_dev_tick(&D, 3000 + ZH_CTL_CONFIRM_MS);
        CHECK(D.ctl.result == ZH_CTL_R_DELIVERED, "D43 只送达、窗口里没等到 dp2：delivered");
        zh_dev_ctl_clear(&D);
        zh_dev_ctl_start(&D, E1, ZH_CTL_LEVEL, 50, 4000);
        TREP(4000 + ZH_CTL_CONFIRM_MS, 2, ZH_TUYA_T_NUMBER, 0, 4, 0, 0, 0x01, 0xF4);
        CHECK(D.ctl.result == ZH_CTL_R_NONE, "D43 过了确认窗口才来的回报：不算确认");
        zh_dev_ctl_clear(&D);
        zh_dev_ctl_start(&D, E1, ZH_CTL_LEVEL, 0, 5000);
        k = last();
        CHECK(fr_pl_n(k) == 10 && fr_pl(k)[5] == 1 && fr_pl(k)[9] == 0 && D.ctl.tuya_dp == 1, "D43 亮度 0 %% = 关（dp1 = 0）");
        zh_dev_ctl_clear(&D);
        /* 切换：按读数取反；还没读到开关状态就拒 */
        D.dev[0].st.have &= (uint16_t)~(1u << ZH_ST_ONOFF);
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_TOGGLE, 0, 6000) == ZH_CTL_E_STATE && !D.ctl.active, "D43 切换、还不知道开没开：ZH_CTL_E_STATE（不发）");
        TREP(6100, 1, ZH_TUYA_T_BOOL, 0, 1, 1);
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_TOGGLE, 0, 6200) == ZH_CTL_E_OK && fr_pl(last())[9] == 0, "D43 切换、现在开着：发关");
        zh_dev_ctl_clear(&D);
        TREP(6300, 1, ZH_TUYA_T_BOOL, 0, 1, 0);
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_TOGGLE, 0, 6400) == ZH_CTL_E_OK && fr_pl(last())[9] == 1, "D43 切换、现在关着：发开");
        zh_dev_ctl_clear(&D);
        /* 设备万一回了默认应答（我们没要）：照常认（报错就 failed） */
        zh_dev_ctl_start(&D, E1, ZH_CTL_ON, 0, 7000);
        { const uint8_t b_[5] = { ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR, fr_pl(last())[1], ZH_ZCL_CMD_DEFAULT_RSP, ZH_TUYA_CMD_DATA_REQUEST, 0x81 };
          zh_dev_on_zcl(&D, 0x4301, ZH_HA_PROFILE, ZH_CL_TUYA, 1, b_, sizeof(b_), 7050); }
        CHECK(D.ctl.result == ZH_CTL_R_FAILED && D.ctl.zcl_status == 0x81, "D43 设备回了报错的默认应答：failed");
        zh_dev_ctl_clear(&D);
        /* 有 ZCL 的 OnOff 簇：ZCL 优先 */
        fresh();
        zh_dev_rec_t rz = rd;
        rz.ep[0].n_in = 3; rz.ep[0].cl[2] = ZH_CL_ONOFF;
        zh_dev_load(&D, 0, &rz, 1000);
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_ON, 0, 2000) == ZH_CTL_E_OK && fr_cluster(last()) == ZH_CL_ONOFF && D.st.ctl_tuya == 0, "D43 也有 ZCL OnOff 簇：走 ZCL");
        zh_dev_ctl_clear(&D);
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_LEVEL, 30, 2100) == ZH_CTL_E_OK && fr_cluster(last()) == ZH_CL_TUYA, "D43 没有 ZCL Level 簇的亮度：走涂鸦");
        zh_dev_ctl_clear(&D);
        /* 控制命令在途时不打招呼（dataQuery 引出的全量回报会搅乱确认） */
        fresh();
        zh_dev_load(&D, 0, &rd, 1000);
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_ON, 0, 2000) == ZH_CTL_E_OK && s_nsent == 1, "D43 在途");
        zh_dev_tick(&D, 2010);
        CHECK(s_nsent == 1 && D.st.tuya_hellos == 0, "D43 命令在途：这台不打招呼");
        zh_dev_ctl_clear(&D);
        zh_dev_tick(&D, 2020);
        CHECK(s_nsent == 2 && D.st.tuya_hellos == 1, "D43 结论取走后照常招呼");
        /* 只有亮度、没有开关 dp 的：亮度 0 %% 关不了 */
        fresh();
        zh_dev_rec_t rl0 = rd;
        strcpy(rl0.manuf, "_TZE204_djurk6p5");
        zh_dev_load(&D, 0, &rl0, 1000);
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_LEVEL, 0, 2000) == ZH_CTL_E_NO_OFF && s_nsent == 0, "D43 没有开关 dp 的亮度 0 %%：ZH_CTL_E_NO_OFF（不发）");
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_LEVEL, 40, 2100) == ZH_CTL_E_OK && fr_pl_n(last()) == 3 + 2 + 8, "D43 它的亮度 40 %%：只有一条亮度");
        zh_dev_ctl_clear(&D);
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_ON, 0, 2200) == ZH_CTL_E_NO_CLUSTER, "D43 它的开：没有开关写法，NO_CLUSTER");
        /* 没有写法 / 休眠 / 端点里没记下 0xEF00 */
        fresh();
        zh_dev_rec_t rt = rd;
        strcpy(rt.manuf, "_TZE200_bjawzodf");
        zh_dev_load(&D, 0, &rt, 1000);
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_ON, 0, 2000) == ZH_CTL_E_NO_CLUSTER && s_nsent == 0, "D43 涂鸦表里没有写法的（温湿度计）：NO_CLUSTER");
        fresh();
        rt = rd;
        rt.node_type = ZH_NODE_END_DEVICE; rt.flags = ZH_DEVF_INTERVIEWED | ZH_DEVF_ANNCED;
        zh_dev_load(&D, 0, &rt, 1000);
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_ON, 0, 2000) == ZH_CTL_E_OK && D.ctl.result == ZH_CTL_R_PENDING, "D43 休眠的涂鸦设备：排进待发（D46）");
        zh_dev_ctl_clear(&D);
        fresh();
        rt = rd;
        rt.ep[0].ep = 3; rt.ep[0].n_in = 1;                            /* 0xEF00 被截掉了：发第一个端点 */
        zh_dev_load(&D, 0, &rt, 1000);
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_OFF, 0, 2000) == ZH_CTL_E_OK && fr_dst_ep(last()) == 3, "D43 端点里没记下 0xEF00：发第一个端点");
        zh_dev_ctl_clear(&D);
        fresh();
        rt = rd;                                                       /* 0xEF00 在第二个端点：发到那个端点 */
        rt.n_ep = 2;
        rt.ep[0].ep = 242; rt.ep[0].n_in = 1; rt.ep[0].cl[0] = 0x0021;
        rt.ep[1].ep = 1; rt.ep[1].profile = ZH_HA_PROFILE; rt.ep[1].n_in = 2; rt.ep[1].cl[0] = 0x0000; rt.ep[1].cl[1] = ZH_CL_TUYA;
        zh_dev_load(&D, 0, &rt, 1000);
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_ON, 0, 2000) == ZH_CTL_E_OK && fr_dst_ep(last()) == 1, "D43 0xEF00 在第二个端点：发到端点 1，不是第一个端点 242");
        zh_dev_ctl_clear(&D);
        #undef TREP
    }

    /* ---- D44 ---- 小米 / Aqara（M5 第十步） */
    {
        static const uint8_t E1[8] = { 1, 0x44, 0, 0, 0, 0x8d, 0x15, 0x00 }, E2[8] = { 2, 0x44, 0, 0, 0, 0x8d, 0x15, 0x00 },
                             E3[8] = { 3, 0x44, 0, 0, 0, 0x8d, 0x15, 0x00 }, E4[8] = { 4, 0x44, 0, 0, 0, 0x8d, 0x15, 0x00 };
        zh_dev_rec_t rx;                                               /* 被动学来、访谈没走完的小米：没有端点、没有型号 */
        memset(&rx, 0, sizeof(rx));
        rx.ver = ZH_DEV_REC_VER;
        rx.node_type = ZH_NODE_END_DEVICE;
        rx.zone_type = ZH_ZONE_TYPE_UNKNOWN;
        zh_dev_rec_t r1 = rx, r2 = rx, r3 = rx, r4 = rx;
        memcpy(r1.ieee, E1, 8); r1.nwk = 0x4401;
        memcpy(r2.ieee, E2, 8); r2.nwk = 0x4402; strcpy(r2.model, "lumi.sensor_magnet.aq2");
        memcpy(r3.ieee, E3, 8); r3.nwk = 0x4403; strcpy(r3.model, "lumi.sensor_motion.aq2");
        memcpy(r4.ieee, E4, 8); r4.nwk = 0x4404; strcpy(r4.model, "lumi.sensor_wleak.aq1");
        fresh();
        zh_dev_load(&D, 0, &r1, 1000);
        zh_dev_load(&D, 1, &r2, 1000);
        zh_dev_load(&D, 2, &r3, 1000);
        zh_dev_load(&D, 3, &r4, 1000);
        D.dev[0].iv = ZH_IV_NONE; D.dev[1].iv = ZH_IV_NONE; D.dev[2].iv = ZH_IV_NONE; D.dev[3].iv = ZH_IV_NONE;   /* 访谈的进度这里不关心 */
        /* 温湿度计：先来一条 0xFF01（型号还不知道）→ 只计数；再来 Basic 的型号上报 → 学进记录、落盘 */
        const uint8_t ff01[] = { 0x01, 0xFF, 0x42, 12, 0x01, 0x21, 0xD1, 0x0B, 0x64, 0x29, 0x2C, 0x09, 0x65, 0x21, 0x10, 0x15 };
        report(0x4401, ZH_CL_BASIC, ff01, sizeof(ff01), 2000);
        CHECK(D.st.lumi_frames == 1 && D.st.lumi_unknown == 1 && D.dev[0].st.have == 0, "D44 型号还不知道：0xFF01 只计数");
        const int saves0 = s_saves;
        const uint8_t mdl[] = { 0x05, 0x00, 0x42, 12, 'l', 'u', 'm', 'i', '.', 'w', 'e', 'a', 't', 'h', 'e', 'r' };
        report(0x4401, ZH_CL_BASIC, mdl, sizeof(mdl), 2100);
        CHECK(!strcmp(D.dev[0].rec.model, "lumi.weather") && D.st.learned == 1 && s_saves == saves0 + 1 && zh_dev_lumi_dev(&D.dev[0].rec) >= 0, "D44 Basic 的型号上报：学进记录、落盘、查得到小米表");
        report(0x4401, ZH_CL_BASIC, mdl, sizeof(mdl), 2150);
        CHECK(D.st.learned == 1 && s_saves == saves0 + 1, "D44 一样的型号再报：不重复落盘");
        report(0x4401, ZH_CL_BASIC, ff01, sizeof(ff01), 2200);
        CHECK(D.dev[0].st.battery_mv == 3025 && D.dev[0].st.temp_c100 == 2348 && D.dev[0].st.hum_c100 == 0x1510 && D.dev[0].st.battery_pct == 100,
              "D44 0xFF01：电压 3025 mV、温度 23.48、湿度、电量按 2850–3000 线性封到 100 %%");
        const uint8_t ff01b[] = { 0x01, 0xFF, 0x42, 4, 0x01, 0x21, 0x6D, 0x0B };   /* 2925 mV */
        report(0x4401, ZH_CL_BASIC, ff01b, sizeof(ff01b), 2300);
        CHECK(D.dev[0].st.battery_pct == 50, "D44 2925 mV = 50 %%（同 zhc toPercentage）");
        const uint8_t ff01c[] = { 0x01, 0xFF, 0x42, 4, 0x01, 0x21, 0x40, 0x0B };   /* 2880 mV → 20 % */
        const int ev0 = s_ev[ZH_DEV_EV_STATE];
        report(0x4401, ZH_CL_BASIC, ff01c, sizeof(ff01c), 2400);
        const uint8_t ff01d[] = { 0x01, 0xFF, 0x42, 4, 0x01, 0x21, 0x2E, 0x0B };   /* 2862 mV → 8 % */
        report(0x4401, ZH_CL_BASIC, ff01d, sizeof(ff01d), 2500);
        CHECK(D.dev[0].st.battery_pct == 8 && s_ev[ZH_DEV_EV_STATE] == ev0 + 1 && D.dev[0].st_ev == ZH_STEV_BATTERY_LOW, "D44 电量降到 15 %% 以下：出电量低（st_batt_pct 那一套）");
        /* 门磁：OnOff 簇的 onOff = 开合（1 = 开），不进开关读数 */
        const uint8_t on1[] = { 0x00, 0x00, ZH_ZCL_T_BOOL, 0x01 };
        report(0x4402, ZH_CL_ONOFF, on1, sizeof(on1), 3000);
        CHECK(!(D.dev[1].st.have & (1u << ZH_ST_ONOFF)) && (D.dev[1].st.have & (1u << ZH_ST_ZONE)) && (D.dev[1].st.zone_status & ZH_IAS_ST_ALARM1) &&
              D.dev[1].st_ev == ZH_STEV_ALARM_ON && zh_dev_zone_type(&D.dev[1].rec) == ZH_ZONE_CONTACT, "D44 门磁 onOff 1：门开（ZONE alarm1、ALARM_ON），不当成开关");
        const uint8_t on0[] = { 0x00, 0x00, ZH_ZCL_T_BOOL, 0x00 };
        report(0x4402, ZH_CL_ONOFF, on0, sizeof(on0), 3100);
        CHECK(!(D.dev[1].st.zone_status & ZH_IAS_ST_ALARM1) && D.dev[1].st_ev == ZH_STEV_ALARM_OFF, "D44 门磁 onOff 0：门关");
        /* 人体：只报"有人"，90 s 没再报就自动报没人；照度直接是 lux */
        const uint8_t occ1[] = { 0x00, 0x00, ZH_ZCL_T_BITMAP8, 0x01 };
        report(0x4403, ZH_CL_OCCUPANCY, occ1, sizeof(occ1), 4000);
        CHECK(D.dev[2].st.occupancy == 1 && D.dev[2].st_ev == ZH_STEV_OCC_ON && D.dev[2].t_occ_clear != 0, "D44 人体有人：OCC_ON，记下清零时刻");
        zh_dev_tick(&D, 4000 + 89000);
        CHECK(D.dev[2].st.occupancy == 1, "D44 89 s：还算有人");
        report(0x4403, ZH_CL_OCCUPANCY, occ1, sizeof(occ1), 4000 + 89500);
        zh_dev_tick(&D, 4000 + 91000);
        CHECK(D.dev[2].st.occupancy == 1, "D44 89.5 s 又报了一次有人：清零时刻顺延");
        zh_dev_tick(&D, 4000 + 89500 + 90000);
        CHECK(D.dev[2].st.occupancy == 0 && D.dev[2].st_ev == ZH_STEV_OCC_OFF && D.dev[2].t_occ_clear == 0, "D44 再过 90 s 没报：自动报没人（OCC_OFF）");
        const uint8_t lux[] = { 0x00, 0x00, ZH_ZCL_T_UINT16, 123, 0 };
        report(0x4403, ZH_CL_ILLUMINANCE, lux, sizeof(lux), 200000);
        CHECK(D.dev[2].st.lux == 123, "D44 人体的照度 123 = 123 lux（不按对数）");
        const uint8_t luxbad[] = { 0x00, 0x00, ZH_ZCL_T_UINT16, 0xFF, 0xFF };
        report(0x4403, ZH_CL_ILLUMINANCE, luxbad, sizeof(luxbad), 200050);
        CHECK(D.dev[2].st.lux == 123, "D44 照度 0xFFFF（ZCL 的无效值）：改写路也不收");
        /* 0xFCC0（厂商专用帧，厂商码 0x115F）的 0x00F7：TLV 照样进表 */
        const uint8_t f7[] = { 0x5F, 0x11, 0x5A, ZH_ZCL_CMD_REPORT_ATTR, 0xF7, 0x00, 0x41, 4, 0x01, 0x21, 0x6D, 0x0B };
        uint8_t fb[32] = { ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR | ZH_ZCL_FC_MFG };
        memcpy(fb + 1, f7, sizeof(f7));
        const uint32_t lf0 = D.st.lumi_frames;
        zh_dev_on_zcl(&D, 0x4403, ZH_HA_PROFILE, ZH_CL_LUMI, 1, fb, 1 + sizeof(f7), 200100);
        CHECK(D.st.lumi_frames == lf0 + 1 && D.dev[2].st.battery_mv == 2925 && D.dev[2].st.battery_pct == 50, "D44 0xFCC0 的 0x00F7（厂商专用帧）：电压 2925 mV、电量 50 %%");
        /* 漏水：访谈没走完（记录里没有 IAS 簇、没有 ZoneType），IAS 通知来了也认得是漏水 */
        CHECK(zh_dev_zone_type(&D.dev[3].rec) == ZH_ZONE_WATER, "D44 漏水传感器没访谈完：有效 ZoneType 靠表 = WATER");
        zone_notify(0x4404, ZH_IAS_ST_ALARM1, 5000);
        CHECK(D.dev[3].st_ev == ZH_STEV_ALARM_ON && (D.dev[3].st.zone_status & ZH_IAS_ST_ALARM1), "D44 漏水的 IAS 通知：ALARM_ON（hub 按 WATER 说成漏水）");
        /* 访谈重来（不是重新配对）：学来的型号留着；重新配对：清掉 */
        D.dev[0].iv = ZH_IV_FAILED;
        D.dev[0].iv_t_fail = 1000;
        spoke(0x4401, 1000 + ZH_IV_RETRY_MS);
        CHECK(D.dev[0].iv == ZH_IV_NODE && !strcmp(D.dev[0].rec.model, "lumi.weather"), "D44 访谈失败后重来：学来的型号留着");
        zh_dev_on_tc_join(&D, 0x4401, E1, ZH_JOIN_UNSECURED_JOIN, ZH_JOIN_USE_PRECONFIGURED_KEY, 0, 1000 + ZH_IV_RETRY_MS + 10);
        CHECK(D.dev[0].rec.model[0] == 0 && D.dev[0].lumi_k == 0, "D44 重新配对：型号清掉、查表缓存清掉");
        /* 真设备的 0xFF01 是厂商专用帧（帧控制带厂商位，厂商码 0x115F）：配对时型号和 0xFF01 常在同一帧 —— 照收（审计 ❌） */
        {
            static const uint8_t E6[8] = { 6, 0x44, 0, 0, 0, 0x8d, 0x15, 0x00 };
            zh_dev_rec_t r6 = rx;
            memcpy(r6.ieee, E6, 8); r6.nwk = 0x4406;
            zh_dev_load(&D, 5, &r6, 1000);
            const uint8_t body[] = { 0x5F, 0x11, 0x33, ZH_ZCL_CMD_REPORT_ATTR,
                                     0x05, 0x00, 0x42, 12, 'l', 'u', 'm', 'i', '.', 'w', 'e', 'a', 't', 'h', 'e', 'r',
                                     0x01, 0xFF, 0x42, 8, 0x01, 0x21, 0x6D, 0x0B, 0x64, 0x29, 0x2C, 0x09,
                                     0x06, 0x00, ZH_ZCL_T_UINT8, 0x07 };   /* 0x0006 在厂商专用帧里：不是标准的日期码，不认 */
            uint8_t mb[64] = { ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR | ZH_ZCL_FC_MFG };
            memcpy(mb + 1, body, sizeof(body));
            zh_dev_on_zcl(&D, 0x4406, ZH_HA_PROFILE, ZH_CL_BASIC, 1, mb, 1 + sizeof(body), 5500);
            CHECK(!strcmp(D.dev[5].rec.model, "lumi.weather") && D.dev[5].st.battery_mv == 2925 && D.dev[5].st.temp_c100 == 2348 && D.dev[5].st.battery_pct == 50,
                  "D44 厂商专用帧里的型号 + 0xFF01：学型号、同一帧的 TLV 按新型号解");
        }
        /* 不在小米表里的型号：0xFCC0 帧只计数 */
        zh_dev_rec_t r5 = rx;
        static const uint8_t E5[8] = { 5, 0x44, 0, 0, 0, 0x8d, 0x15, 0x00 };
        memcpy(r5.ieee, E5, 8); r5.nwk = 0x4405; strcpy(r5.model, "lumi.no_such");
        zh_dev_load(&D, 4, &r5, 1000);
        const uint32_t lu0 = D.st.lumi_unknown;
        zh_dev_on_zcl(&D, 0x4405, ZH_HA_PROFILE, ZH_CL_LUMI, 1, fb, 1 + sizeof(f7), 6000);
        CHECK(D.st.lumi_unknown == lu0 + 1 && D.dev[4].st.have == 0, "D44 型号不在表里：只计数");
        /* 它后来报了真型号：查表缓存（之前记成"不在表里"）要作废，下一帧就按表 */
        const uint8_t mdl5[] = { 0x05, 0x00, 0x42, 12, 'l', 'u', 'm', 'i', '.', 'w', 'e', 'a', 't', 'h', 'e', 'r' };
        report(0x4405, ZH_CL_BASIC, mdl5, sizeof(mdl5), 6100);
        report(0x4405, ZH_CL_BASIC, ff01b, sizeof(ff01b), 6200);
        CHECK(D.dev[4].st.battery_mv == 2925 && D.dev[4].st.battery_pct == 50, "D44 型号改了：查表缓存作废，0xFF01 按新型号的表解");
    }

    /* ---- D45 ---- 可用性（设计 8.10；M5 第十一步） */
    {
        static const uint8_t E1[8] = { 1, 0x45, 0, 0, 0, 0, 0, 0x45 }, E2[8] = { 2, 0x45, 0, 0, 0, 0, 0, 0x45 }, E3[8] = { 3, 0x45, 0, 0, 0, 0, 0, 0x45 };
        zh_dev_rec_t rm;                                               /* 常电的插座：路由器、端点 3 有 Basic */
        memset(&rm, 0, sizeof(rm));
        rm.ver = ZH_DEV_REC_VER;
        rm.node_type = ZH_NODE_ROUTER;
        rm.flags = ZH_DEVF_INTERVIEWED | ZH_DEVF_ANNCED | ZH_DEVF_RX_ON | ZH_DEVF_MAINS;
        rm.n_ep = 1; rm.ep[0].ep = 3; rm.ep[0].profile = ZH_HA_PROFILE; rm.ep[0].n_in = 2; rm.ep[0].cl[0] = 0x0000; rm.ep[0].cl[1] = ZH_CL_ONOFF;
        rm.zone_type = ZH_ZONE_TYPE_UNKNOWN;
        zh_dev_rec_t r1 = rm, r2 = rm, r3 = rm;
        memcpy(r1.ieee, E1, 8); r1.nwk = 0x4501;
        memcpy(r2.ieee, E2, 8); r2.nwk = 0x4502; r2.node_type = ZH_NODE_END_DEVICE; r2.flags = ZH_DEVF_INTERVIEWED | ZH_DEVF_ANNCED;   /* 电池的 */
        memcpy(r3.ieee, E3, 8); r3.nwk = 0x4503; r3.flags |= ZH_DEVF_LEFT;                                                           /* 离开了的 */
        fresh();
        zh_dev_load(&D, 0, &r1, 1000);
        zh_dev_load(&D, 1, &r2, 1000);
        zh_dev_load(&D, 2, &r3, 1000);
        const uint32_t t0 = 1000;
        zh_dev_tick(&D, t0);                                       /* 网恢复的那一拍：从这里起算 */
        spoke(0x4502, t0);                                         /* 电池的这次开机说过话（没说过的不判，见下） */
        zh_dev_tick(&D, t0 + ZH_AV_MAINS_SILENT_MS - 1);
        CHECK(s_nsent == 0 && D.st.av_pings == 0, "D45 静默不到 10 min：不 ping");
        zh_dev_tick(&D, t0 + ZH_AV_MAINS_SILENT_MS);
        CHECK(s_nsent == 1 && D.st.av_pings == 1 && fr_cluster(last()) == ZH_CL_BASIC && fr_dst_ep(last()) == 3 && fr_pl_n(last()) == 5 &&
              fr_pl(last())[2] == ZH_ZCL_CMD_READ_ATTR && fr_pl(last())[3] == 0x00 && fr_pl(last())[4] == 0x00 && (s_tag[last() % 64] & 0xFu) == ZH_DEV_TAG_KIND_MISC,
              "D45 静默满 10 min：ping = 读 Basic 0x0000，发到有 Basic 的端点");
        zh_dev_tick(&D, t0 + ZH_AV_MAINS_SILENT_MS + 5000);
        CHECK(s_nsent == 1, "D45 10 s 内：等回音，不再发");
        spoke(0x4501, t0 + ZH_AV_MAINS_SILENT_MS + 6000);          /* 回了 */
        zh_dev_tick(&D, t0 + ZH_AV_MAINS_SILENT_MS + 20000);
        CHECK(s_nsent == 1 && !D.dev[0].offline && D.dev[0].av_miss == 0, "D45 回了：这一轮作罢，静默重新起算");
        /* 再静默 10 min：3 次都不回 → 离线 */
        const uint32_t t1 = t0 + ZH_AV_MAINS_SILENT_MS + 6000 + ZH_AV_MAINS_SILENT_MS;
        const int ev0 = s_ev[ZH_DEV_EV_OFFLINE];
        zh_dev_tick(&D, t1);
        zh_dev_tick(&D, t1 + ZH_AV_PING_GAP_MS);
        zh_dev_tick(&D, t1 + 2 * ZH_AV_PING_GAP_MS);
        CHECK(D.st.av_pings == 4 && !D.dev[0].offline, "D45 第三次 ping 出去了、还在等");
        zh_dev_tick(&D, t1 + 3 * ZH_AV_PING_GAP_MS);
        CHECK(D.dev[0].offline && s_ev[ZH_DEV_EV_OFFLINE] == ev0 + 1 && D.st.av_offline == 1 && s_dirty_at_ev, "D45 3 次、每次 10 s 都没回：判离线、出 OFFLINE（事件之前表已 dirty）");
        zh_dev_tick(&D, t1 + 3 * ZH_AV_PING_GAP_MS + ZH_AV_MAINS_SILENT_MS * 2);
        CHECK(D.st.av_pings == 4 && s_ev[ZH_DEV_EV_OFFLINE] == ev0 + 1, "D45 离线了：不再 ping、不再出离线");
        spoke(0x4501, t1 + 3 * ZH_AV_PING_GAP_MS + ZH_AV_MAINS_SILENT_MS * 2 + 10);
        CHECK(!D.dev[0].offline && s_ev[ZH_DEV_EV_ONLINE] == 1 && D.st.av_online == 1, "D45 又说话了：恢复、出 ONLINE");
        /* 电池的：25 h 没消息才离线，不 ping */
        CHECK(D.st.av_pings == 4, "D45 电池设备从不 ping");
        zh_dev_tick(&D, t0 + ZH_AV_BATT_SILENT_MS - 1);
        CHECK(!D.dev[1].offline, "D45 电池不到 25 h：在线");
        zh_dev_tick(&D, t0 + ZH_AV_BATT_SILENT_MS);
        CHECK(D.dev[1].offline && s_ev[ZH_DEV_EV_OFFLINE] == ev0 + 2, "D45 电池 25 h 没消息：离线");
        CHECK(!D.dev[2].offline, "D45 离开了的：不判");
        /* 电池设备这次开机没听到过：开机满 25 h 静默判离线（不出事件、不推送；2026-10-23，原先永远不判）、说话了静默恢复；遥控器类：不判（只在按的时候说话） */
        fresh();
        zh_dev_rec_t rb = r2, rr = r2;
        rr.nwk = 0x4505; rr.ieee[0] = 5;
        rr.n_ep = 1; rr.ep[0].ep = 1; rr.ep[0].profile = ZH_HA_PROFILE; rr.ep[0].n_in = 1; rr.ep[0].n_out = 2; rr.ep[0].cl[0] = 0x0000; rr.ep[0].cl[1] = ZH_CL_ONOFF; rr.ep[0].cl[2] = ZH_CL_LEVEL;
        zh_dev_load(&D, 0, &rb, 1000);
        zh_dev_load(&D, 1, &rr, 1000);
        zh_dev_tick(&D, 1000);
        const int evo = s_ev[ZH_DEV_EV_OFFLINE], evn = s_ev[ZH_DEV_EV_ONLINE];
        zh_dev_tick(&D, 1000 + ZH_AV_BATT_SILENT_MS - 10);
        CHECK(!D.dev[0].offline, "D45 没听到过的电池设备、网 UP 不到 25 h：在线");
        zh_dev_tick(&D, 1000 + ZH_AV_BATT_SILENT_MS + 10);
        CHECK(D.dev[0].offline && D.dev[0].av_quiet && D.st.av_quiet == 1 && D.st.av_offline == 0 && s_ev[ZH_DEV_EV_OFFLINE] == evo && !D.dev[1].offline,
              "D45 没听到过的电池设备、网 UP 满 25 h：静默离线（不出事件）；遥控器类不判");
        const uint32_t tq = 1000 + ZH_AV_BATT_SILENT_MS + 100;
        spoke(0x4502, tq);
        CHECK(!D.dev[0].offline && !D.dev[0].av_quiet && D.st.av_online == 0 && s_ev[ZH_DEV_EV_ONLINE] == evn, "D45 静默离线的说话了：静默恢复（没推过离线，就不推恢复）");
        spoke(0x4505, tq);
        zh_dev_tick(&D, tq + ZH_AV_BATT_SILENT_MS);
        CHECK(D.dev[0].offline && !D.dev[0].av_quiet && D.st.av_offline == 1 && s_ev[ZH_DEV_EV_OFFLINE] == evo + 1 && !D.dev[1].offline,
              "D45 听到过的电池设备 25 h：照常离线、出事件；遥控器类（输出簇有开关 / 亮度）不判");
        /* 网连着 UP 超过 2^31 ms：t_net_up 封顶，照样判得出（审计 ❌：原先 24.8 天后谁都判不出离线） */
        fresh();
        zh_dev_load(&D, 0, &r1, 1000);
        zh_dev_tick(&D, 1000);
        for (uint32_t k = 1; k <= 25; k++) {                       /* 网连续 UP 一天天过去（插座每天都说话） */
            spoke(0x4501, 1000u + k * 86400000u);
            zh_dev_tick(&D, 1000u + k * 86400000u + 1);
        }
        const uint32_t tw = 1000u + 0x80000000u + 5000u;            /* 网连续 UP 24.8 天之后 */
        spoke(0x4501, tw);
        zh_dev_tick(&D, tw + 1);
        zh_dev_tick(&D, tw + ZH_AV_MAINS_SILENT_MS);
        CHECK(D.st.av_pings == 1, "D45 网连续 UP 超过 24.8 天：静默 10 min 照样 ping");

        /* 网断了：不判；恢复后从恢复那刻起算 */
        fresh();
        zh_dev_load(&D, 0, &r1, 1000);
        zh_dev_tick(&D, 1000);
        zh_dev_set_net_up(&D, false);
        zh_dev_tick(&D, 1000 + ZH_AV_MAINS_SILENT_MS * 3);
        CHECK(s_nsent == 0 && !D.dev[0].offline, "D45 网不是 UP：不 ping、不判");
        zh_dev_set_net_up(&D, true);
        const uint32_t tu = 1000 + ZH_AV_MAINS_SILENT_MS * 3 + 100;
        zh_dev_tick(&D, tu);
        zh_dev_tick(&D, tu + ZH_AV_MAINS_SILENT_MS - 1);
        CHECK(s_nsent == 0, "D45 网恢复：从恢复那刻重新起算（不会马上 ping 一片）");
        zh_dev_tick(&D, tu + ZH_AV_MAINS_SILENT_MS);
        CHECK(s_nsent == 1, "D45 恢复后静默满 10 min：照常 ping");
        /* 交不出去不占次数；一拍一条 */
        fresh();
        zh_dev_load(&D, 0, &r1, 1000);
        zh_dev_rec_t r4 = rm;
        static const uint8_t E4[8] = { 4, 0x45, 0, 0, 0, 0, 0, 0x45 };
        memcpy(r4.ieee, E4, 8); r4.nwk = 0x4504;
        zh_dev_load(&D, 1, &r4, 1000);
        zh_dev_tick(&D, 1000);
        s_send_ok = false;
        zh_dev_tick(&D, 1000 + ZH_AV_MAINS_SILENT_MS);
        CHECK(D.dev[0].av_miss == 0 && D.dev[1].av_miss == 0 && D.st.av_pings == 0, "D45 交不出去：不占次数");
        s_send_ok = true;
        zh_dev_tick(&D, 1000 + ZH_AV_MAINS_SILENT_MS + 1);
        CHECK(s_nsent == 1 && D.dev[0].av_miss + D.dev[1].av_miss == 1, "D45 两台都该 ping：一拍只发一条");
        zh_dev_tick(&D, 1000 + ZH_AV_MAINS_SILENT_MS + 2);
        CHECK(s_nsent == 2 && D.dev[0].av_miss == 1 && D.dev[1].av_miss == 1, "D45 下一拍发另一台");
    }

    /* ---- D46 ---- 休眠设备的控制排队（8.11 第 6 步；M5 第十二步） */
    {
        static const uint8_t E1[8] = { 1, 0x46, 0, 0, 0, 0, 0, 0x46 }, E2[8] = { 2, 0x46, 0, 0, 0, 0, 0, 0x46 };
        zh_dev_rec_t rs;                                               /* 电池的阀门：OnOff 输入簇、端点 2 */
        memset(&rs, 0, sizeof(rs));
        rs.ver = ZH_DEV_REC_VER;
        rs.node_type = ZH_NODE_END_DEVICE;
        rs.flags = ZH_DEVF_INTERVIEWED | ZH_DEVF_ANNCED;
        rs.n_ep = 1; rs.ep[0].ep = 2; rs.ep[0].profile = ZH_HA_PROFILE; rs.ep[0].n_in = 2; rs.ep[0].cl[0] = 0x0000; rs.ep[0].cl[1] = ZH_CL_ONOFF;
        rs.zone_type = ZH_ZONE_TYPE_UNKNOWN;
        zh_dev_rec_t r1 = rs, r2 = rs;
        memcpy(r1.ieee, E1, 8); r1.nwk = 0x4601;
        memcpy(r2.ieee, E2, 8); r2.nwk = 0x4602;                       /* 涂鸦调光器、但是电池供电（测涂鸦的排队） */
        r2.ep[0].ep = 1; r2.ep[0].cl[1] = ZH_CL_TUYA; strcpy(r2.manuf, "_TZE200_swaamsoy"); strcpy(r2.model, "TS0601");
        fresh();
        zh_dev_load(&D, 0, &r1, 1000);
        zh_dev_load(&D, 1, &r2, 1000);
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_ON, 0, 2000) == ZH_CTL_E_OK && D.ctl.result == ZH_CTL_R_PENDING && s_nsent == 0 && D.dev[0].cq.on && D.st.ctlq_queued == 1,
              "D46 休眠设备开：排进待发、立刻 PENDING、不发");
        zh_dev_ctl_clear(&D);
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_OFF, 0, 2100) == ZH_CTL_E_OK && D.dev[0].cq.act == ZH_CTL_OFF && D.st.ctlq_replaced == 1, "D46 又来一条关：顶掉开（要的是最后那个状态）");
        zh_dev_ctl_clear(&D);
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_LEVEL, 50, 2150) == ZH_CTL_E_NO_CLUSTER && D.dev[0].cq.act == ZH_CTL_OFF, "D46 做不到的（没有 Level 簇）：当场回、不顶掉");
        zh_dev_tick(&D, 2200);
        CHECK(s_nsent == 0, "D46 它还没说话：不发");
        spoke(0x4601, 5000);
        zh_dev_tick(&D, 5000 + ZH_IV_AWAKE_MS);
        CHECK(s_nsent == 0, "D46 说话已经过了 3 s：不发（多半又睡了）");
        spoke(0x4601, 6000);
        zh_dev_tick(&D, 6100);
        int k = last();
        CHECK(s_nsent == 1 && fr_cluster(k) == ZH_CL_ONOFF && fr_dst_ep(k) == 2 && fr_pl_n(k) == 3 && fr_pl(k)[0] == ZH_ZCL_FC_CLUSTER && fr_pl(k)[2] == ZH_CL_CMD_OFF &&
              (s_tag[k % 64] & 0xFu) == ZH_DEV_TAG_KIND_QCTL && D.dev[0].cq.inflight && D.st.ctlq_sent == 1, "D46 刚说过话：补发（帧同同步路：OnOff、端点 2、命令 Off）");
        const uint16_t mt = (uint16_t)(s_frame[k % 64][19] | (s_frame[k % 64][20] << 8));
        CHECK((mt & 0xC000u) == 0x4000u && ((mt >> 8) & 0x3Fu) == 0, "D46 messageTag = 0x4000 | 下标 << 8 | 代号");
        zh_dev_on_msg_sent(&D, mt, 0x66, 6200);
        CHECK(D.dev[0].cq.on && !D.dev[0].cq.inflight && D.st.ctlq_failed == 1, "D46 没送达：留着");
        zh_dev_tick(&D, 6300);
        CHECK(s_nsent == 1, "D46 同一个醒着的窗口（补发之后它没再说话）：不连发");
        spoke(0x4601, 9000);
        zh_dev_tick(&D, 9100);
        k = last();
        CHECK(s_nsent == 2 && D.dev[0].cq.tries == 2, "D46 它又说话了：再补发");
        zh_dev_on_msg_sent(&D, (uint16_t)(s_frame[k % 64][19] | (s_frame[k % 64][20] << 8)), 0, 9200);
        CHECK(!D.dev[0].cq.on && D.st.ctlq_delivered == 1, "D46 送达：清掉");
        spoke(0x4601, 12000);
        zh_dev_tick(&D, 12100);
        CHECK(s_nsent == 2, "D46 清掉之后：不再发");
        /* messageSent 一直不来：过了期限放开，下次醒来再发（换代号）；迟到的回执：失败不认、送达照样清槽 */
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_ON, 0, 20000) == ZH_CTL_E_OK, "D46 再排一条");
        zh_dev_ctl_clear(&D);
        spoke(0x4601, 20100);
        zh_dev_tick(&D, 20150);
        k = last();
        const uint16_t mt3 = (uint16_t)(s_frame[k % 64][19] | (s_frame[k % 64][20] << 8));
        CHECK(s_nsent == 3 && D.dev[0].cq.inflight, "D46 补发出去了");
        zh_dev_tick(&D, 20150 + ZH_CTL_DEADLINE_MS);
        CHECK(D.dev[0].cq.inflight, "D46 过了常电的 8 s 还在等（经父节点间接发 + APS 重试要二十几秒，审计 ❌）");
        zh_dev_tick(&D, 20150 + ZH_CTLQ_SENT_MS);
        CHECK(D.dev[0].cq.on && !D.dev[0].cq.inflight, "D46 messageSent 一直不来（链路复位吞了）：期限一到放开");
        zh_dev_on_msg_sent(&D, mt3, 0x66, 20150 + ZH_CTLQ_SENT_MS + 10);
        CHECK(D.dev[0].cq.on && D.st.ctlq_failed == 1, "D46 期限过了才来的失败：早当没送到了、不另计");
        spoke(0x4601, 60000);
        zh_dev_tick(&D, 60010);
        k = last();
        const uint16_t mt4 = (uint16_t)(s_frame[k % 64][19] | (s_frame[k % 64][20] << 8));
        CHECK(s_nsent == 4 && (uint8_t)mt4 != (uint8_t)mt3 && ((mt4 >> 8) & 0x3Fu) == 0, "D46 下次醒来再发、换了代号");
        zh_dev_on_msg_sent(&D, mt3, 0x66, 60100);
        CHECK(D.dev[0].cq.inflight && D.st.ctlq_failed == 1, "D46 上一发迟到的失败回执：安不到这一发头上（审计 ❌）");
        zh_dev_tick(&D, 60010 + ZH_CTLQ_SENT_MS);
        CHECK(!D.dev[0].cq.inflight, "D46 这一发也过了期限：放开");
        const uint32_t dl1 = D.st.ctlq_delivered;
        zh_dev_on_msg_sent(&D, mt4, 0, 60010 + ZH_CTLQ_SENT_MS + 500);
        CHECK(!D.dev[0].cq.on && D.st.ctlq_delivered == dl1 + 1, "D46 期限过了才来的送达：照样清槽（审计 ❌：不然下次醒来再发，切换会被撤销）");
        spoke(0x4601, 100000);
        zh_dev_tick(&D, 100010);
        CHECK(s_nsent == 4, "D46 清掉之后：不再发");
        /* 30 min 没送到：作废 */
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_ON, 0, 110000) == ZH_CTL_E_OK, "D46 再排一条（它一直不说话）");
        zh_dev_ctl_clear(&D);
        zh_dev_tick(&D, 110000 + ZH_CTLQ_MAX_MS - 10);
        CHECK(D.dev[0].cq.on, "D46 不到 30 min：还排着");
        zh_dev_tick(&D, 110000 + ZH_CTLQ_MAX_MS);
        CHECK(!D.dev[0].cq.on && D.st.ctlq_expired == 1, "D46 排了 30 min：作废");
        /* 涂鸦（电池的）：切换在补发时按当时的读数取反 */
        const uint8_t on1[] = { 0x00, 0x00, 1, ZH_TUYA_T_BOOL, 0, 1, 1 };
        zcl_frame(0x4602, ZH_CL_TUYA, ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR, ZH_TUYA_CMD_DATA_REPORT, on1, sizeof(on1), 1, 40000);
        CHECK(zh_dev_ctl_start(&D, E2, ZH_CTL_TOGGLE, 0, 40100) == ZH_CTL_E_OK && D.ctl.result == ZH_CTL_R_PENDING, "D46 电池涂鸦的切换：排进待发");
        zh_dev_ctl_clear(&D);
        const uint8_t off1[] = { 0x00, 0x00, 1, ZH_TUYA_T_BOOL, 0, 1, 0 };   /* 补发前它自己报了"关" */
        zcl_frame(0x4602, ZH_CL_TUYA, ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_S2C | ZH_ZCL_FC_DIS_DR, ZH_TUYA_CMD_DATA_REPORT, off1, sizeof(off1), 1, 41000);
        spoke(0x4602, 41000);
        zh_dev_tick(&D, 41050);
        k = last();
        CHECK(fr_cluster(k) == ZH_CL_TUYA && fr_pl(k)[0] == (ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_DIS_DR) && fr_pl(k)[5] == 1 && fr_pl(k)[9] == 1,
              "D46 补发时它是关着的：切换 = 开（按补发那一刻的读数，不是排队时的）");
        const uint16_t mt2 = (uint16_t)(s_frame[k % 64][19] | (s_frame[k % 64][20] << 8));
        CHECK(((mt2 >> 8) & 0x3Fu) == 1, "D46 第二台的 messageTag 里是下标 1（messageSent 才认得回到哪台）");
        zh_dev_on_msg_sent(&D, mt2, 0, 41100);
        CHECK(!D.dev[1].cq.on, "D46 第二台送达：清它自己的槽");
        /* 两台同时醒着、都有待发：一拍只发一条 */
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_ON, 0, 42000) == ZH_CTL_E_OK, "D46 第一台排一条");
        zh_dev_ctl_clear(&D);
        CHECK(zh_dev_ctl_start(&D, E2, ZH_CTL_OFF, 0, 42000) == ZH_CTL_E_OK, "D46 第二台排一条");
        zh_dev_ctl_clear(&D);
        const int n0 = s_nsent;
        spoke(0x4601, 43000);
        spoke(0x4602, 43000);
        zh_dev_tick(&D, 43010);
        CHECK(count_kind(n0, ZH_DEV_TAG_KIND_QCTL) == 1, "D46 两台都该补发：一拍只发一条（只数补发帧：涂鸦的招呼帧同拍也会发）");
        zh_dev_tick(&D, 43020);
        CHECK(count_kind(n0, ZH_DEV_TAG_KIND_QCTL) == 2 && D.dev[0].cq.inflight && D.dev[1].cq.inflight, "D46 下一拍发另一台");
        /* 在途时又来一条：顶掉之后是一条全新的待发（不继承旧的在途 / 补发时刻），还醒着就马上发；旧那条的 messageSent 不认 */
        int k0 = -1;
        for (int j = n0; j < s_nsent; j++) if ((s_tag[j % 64] & 0xFu) == ZH_DEV_TAG_KIND_QCTL && ((s_tag[j % 64] >> 8) & 0xFFu) == 0) k0 = j;
        CHECK(k0 >= 0, "D46 找到第一台那条补发");
        const uint16_t mt_old = (uint16_t)(s_frame[k0 % 64][19] | (s_frame[k0 % 64][20] << 8));
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_OFF, 0, 43100) == ZH_CTL_E_OK && D.dev[0].cq.on && !D.dev[0].cq.inflight && D.dev[0].cq.t_sent == 0,
              "D46 在途时顶掉：新的一条不在途、没补发过");
        zh_dev_ctl_clear(&D);
        const int n1 = s_nsent;
        zh_dev_tick(&D, 43150);
        k = last();
        CHECK(count_kind(n1, ZH_DEV_TAG_KIND_QCTL) == 1 && fr_cluster(k) == ZH_CL_ONOFF && fr_pl(k)[2] == ZH_CL_CMD_OFF && D.dev[0].cq.inflight,
              "D46 它还醒着：马上发新的那条（关）");
        const uint32_t dl0 = D.st.ctlq_delivered;
        zh_dev_on_msg_sent(&D, mt_old, 0, 43200);
        CHECK(D.dev[0].cq.on && D.dev[0].cq.inflight && D.st.ctlq_delivered == dl0, "D46 旧那条的送达回执：不清新的");
        /* 网不是 UP：不排（当场回 NET） */
        zh_dev_set_net_up(&D, false);
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_ON, 0, 50000) == ZH_CTL_E_NET, "D46 网不是 UP：不排，回 NET");
        zh_dev_set_net_up(&D, true);
        /* 排着旧的、后来定型成常电：同步发出去的新命令顶掉它（审计 ❌） */
        CHECK(D.dev[0].cq.on, "D46 第一台还排着一条");
        const uint32_t rp0 = D.st.ctlq_replaced;
        D.dev[0].rec.flags |= ZH_DEVF_RX_ON;
        zh_dev_ctl_clear(&D);
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_ON, 0, 50100) == ZH_CTL_E_OK && D.ctl.result == ZH_CTL_R_NONE && !D.dev[0].cq.on && D.st.ctlq_replaced == rp0 + 1,
              "D46 定型成常电后同步发出：排着的旧命令清掉");
        zh_dev_ctl_clear(&D);
        D.dev[0].rec.flags &= (uint8_t)~ZH_DEVF_RX_ON;
        /* 离网 / 重新配对：排着的作废（审计 ⚠️） */
        const uint32_t dr0 = D.st.ctlq_dropped;
        CHECK(D.dev[1].cq.on, "D46 第二台还排着一条");
        zh_dev_on_tc_join(&D, 0x4602, E2, ZH_JOIN_DEVICE_LEFT, ZH_JOIN_NO_ACTION, 0x0000, 50200);
        CHECK(!D.dev[1].cq.on && D.st.ctlq_dropped == dr0 + 1, "D46 离网：排着的作废");
        CHECK(zh_dev_ctl_start(&D, E1, ZH_CTL_OFF, 0, 50300) == ZH_CTL_E_OK && D.dev[0].cq.on, "D46 第一台再排一条");
        zh_dev_ctl_clear(&D);
        zh_dev_on_tc_join(&D, 0x4601, E1, ZH_JOIN_UNSECURED_JOIN, ZH_JOIN_USE_PRECONFIGURED_KEY, 0x0000, 50400);
        CHECK(!D.dev[0].cq.on && D.st.ctlq_dropped == dr0 + 2, "D46 重新配对（恢复出厂过）：排着的是给旧那台的，作废");
    }
    /* ---- D47 主动发现（8.12 ④；M6 第一步）---- */
    {
        static const uint8_t R1[8] = { 0x01, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11 };   /* 不认识的路由器 */
        static const uint8_t T1[8] = { 0x02, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22 };   /* 不认识的终端（休眠） */
        static const uint8_t P1[8] = { 0x03, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33 };   /* 前子节点 */
        static const uint8_t FF[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        fresh();
        annce(0x4444, EA, ZH_CAP_FFD | ZH_CAP_MAINS | ZH_CAP_RX_ON_IDLE, 100);   /* 已知的路由器（宣告来的）：也是起点 */
        const int ka = zh_dev_find_ieee(&D, EA);
        D.dev[ka].rec.flags |= ZH_DEVF_INTERVIEWED;                            /* 它不再访谈：下面数帧只数发现的 */
        D.dev[ka].iv = ZH_IV_NONE;
        disc_open(1000);
        int n0 = s_nsent;
        zh_dev_tick(&D, 60999);
        CHECK(s_nsent == n0 && D.disc.state == ZH_DISC_WAIT, "D47 网 UP 后 60 s 内不发");
        zh_dev_tick(&D, 61000);
        CHECK(s_nsent == n0 + 1 && last_is_lqi(0x0000, 0) && D.st.disc_reqs == 1, "D47 第一条：发给协调器自己、起始 0、不幂等");
        CHECK(fr_opts(last()) == ZH_APS_OPTS_UNICAST && fr_dst_ep(last()) == 0 && fr_pl(last())[0] == s_seq, "D47 帧：ZDO 端点 0、APS RETRY、序号在载荷第 0 字节");
        CHECK(D.disc.q_n == 2 && D.disc.q[1].nwk == 0x4444 && D.disc.q[1].depth == 1, "D47 已知路由器也排进起点（第 1 层）");
        const uint8_t seq1 = s_seq;
        zh_dev_tick(&D, 62000);
        CHECK(s_nsent == n0 + 1, "D47 一次一条：在途时不发下一条");
        /* 第一页：路由器、终端、前子节点 */
        const lqi_ent_t p1[3] = { lqi_ent(XPAN, R1, 0x1111, ZH_LQI_TYPE_ROUTER, 1, 1, 1), lqi_ent(XPAN, T1, 0x2222, ZH_LQI_TYPE_END_DEVICE, 0, 1, 1),
                                  lqi_ent(XPAN, P1, 0x3333, ZH_LQI_TYPE_END_DEVICE, 0, ZH_LQI_REL_PREV_CHILD, 1) };
        lqi_rsp(0x0000, (uint8_t)(seq1 + 1), 0, 5, 0, p1, 3, 62500);
        CHECK(D.st.disc_rsps == 0 && D.disc.inflight, "D47 序号不对的不认");
        lqi_rsp(0x4444, seq1, 0, 5, 0, p1, 3, 62500);
        CHECK(D.st.disc_rsps == 0 && D.disc.inflight, "D47 别人发来的不认");
        const int ev0 = s_ev_src[ZH_DEV_SRC_DISCOVERY];
        lqi_rsp(0x0000, seq1, 0, 5, 0, p1, 3, 62600);
        const int kr = zh_dev_find_ieee(&D, R1), kt = zh_dev_find_ieee(&D, T1);
        CHECK(kr >= 0 && D.dev[kr].rec.nwk == 0x1111 && D.dev[kr].rec.node_type == ZH_NODE_ROUTER && (D.dev[kr].rec.flags & ZH_DEVF_RX_ON) &&
              !(D.dev[kr].rec.flags & ZH_DEVF_ANNCED), "D47 不认识的路由器：建记录、标路由器 + RX_ON（不冒充\"能力已知\"）");
        CHECK(kt >= 0 && D.dev[kt].rec.nwk == 0x2222 && D.dev[kt].rec.node_type == ZH_NODE_END_DEVICE && D.dev[kt].rec.flags == 0, "D47 不认识的终端：建记录、休眠");
        CHECK(zh_dev_find_ieee(&D, P1) < 0 && D.st.disc_skipped == 1, "D47 前子节点：跳过");
        CHECK(s_ev_src[ZH_DEV_SRC_DISCOVERY] == ev0 + 2 && D.st.disc_added == 2 && D.st.disc_entries == 3, "D47 两条 ADDED、来源是主动发现");
        CHECK(D.disc.q_n == 3 && D.disc.q[2].nwk == 0x1111 && D.disc.q[2].depth == 1, "D47 路由器排进下一层");
        /* 路由器马上访谈（它没说过话）；终端不访谈 */
        n0 = s_nsent;
        zh_dev_tick(&D, 62700);
        bool iv_r = false, iv_t = false;
        for (int j = n0; j < s_nsent; j++) {
            if (fr_cluster(j) == ZH_ZDO_NODE_DESC_REQ && fr_dest(j) == 0x1111) iv_r = true;
            if (fr_cluster(j) == ZH_ZDO_NODE_DESC_REQ && fr_dest(j) == 0x2222) iv_t = true;
        }
        CHECK(iv_r && !iv_t, "D47 路由器马上访谈（iv_can_talk 认路由器）、休眠终端等它说话");
        CHECK(count_kind(n0, ZH_DEV_TAG_KIND_DISC) == 0, "D47 两条请求至少隔 1 s");
        /* 第二页：已认识的、别的网的、全 FF 的路由器、短地址被占的 */
        zh_dev_tick(&D, 63600);
        CHECK(last_is_lqi(0x0000, 3), "D47 第二页：起始下标 = 上页起始 + 条数");
        const uint8_t seq2 = fr_pl(last())[0];
        const lqi_ent_t p2[4] = { lqi_ent(XPAN, EA, 0x4545, ZH_LQI_TYPE_ROUTER, 1, 2, 1), lqi_ent(XPAN_OTHER, EB, 0x5555, ZH_LQI_TYPE_ROUTER, 1, 2, 1),
                                  lqi_ent(XPAN, FF, 0x6666, ZH_LQI_TYPE_ROUTER, 1, 2, 1), lqi_ent(XPAN, EB, 0x2222, ZH_LQI_TYPE_END_DEVICE, 0, 1, 1) };
        lqi_rsp(0x0000, seq2, 0, 5, 3, p2, 4, 63700);
        CHECK(D.dev[ka].rec.nwk == 0x4444 && D.st.disc_known == 1, "D47 已认识的不改（表里的短地址可能是旧的）");
        CHECK(zh_dev_find_ieee(&D, EB) < 0 && D.st.disc_xpan == 1 && D.st.disc_conflict == 1 && D.dev[kt].rec.nwk == 0x2222,
              "D47 别的网的不建；短地址被占的不建、也不作废那条记录");
        CHECK(D.st.bad_eui >= 1 && D.disc.q_n == 5 && D.disc.q[4].nwk == 0x6666, "D47 IEEE 不认的路由器：不建，但照样往下爬");
        CHECK(D.st.disc_routers == 1 && D.disc.q_i == 1, "D47 读到总条数：这一台完了、换下一台");
        /* 下一台（已知路由器 0x4444）：不支持 */
        zh_dev_tick(&D, 64700);
        CHECK(last_is_lqi(0x4444, 0), "D47 下一台从起始 0 开始");
        lqi_rsp(0x4444, fr_pl(last())[0], 0x84, 0, 0, NULL, 0, 64800);
        CHECK(D.st.disc_unsupported == 1 && D.disc.q_i == 2, "D47 应答状态不是成功：跳过这台");
        /* 0x1111：超时三次放弃 */
        zh_dev_tick(&D, 65800);
        CHECK(last_is_lqi(0x1111, 0), "D47 发给新发现的路由器");
        const uint8_t seqa = fr_pl(last())[0];
        zh_dev_tick(&D, 70799);
        CHECK(D.st.disc_timeouts == 0, "D47 5 s 内不算超时");
        zh_dev_tick(&D, 70800);
        CHECK(D.st.disc_timeouts == 1 && D.disc.tries == 1 && !D.disc.inflight, "D47 5 s 没回：算一次");
        zh_dev_tick(&D, 71800);
        CHECK(last_is_lqi(0x1111, 0) && fr_pl(last())[0] != seqa, "D47 重发同一页、换序号");
        lqi_rsp(0x1111, seqa, 0, 1, 0, p1, 1, 71900);
        CHECK(D.st.disc_rsps == 2, "D47 上一次的迟到应答不认");
        zh_dev_tick(&D, 76800); zh_dev_tick(&D, 77800); zh_dev_tick(&D, 82800);
        CHECK(D.st.disc_gave_up == 1 && D.disc.q_i == 3, "D47 三次都没回：放弃这台");
        /* 0x4545：已认识的那台在表里的旧短地址也照样排进来爬（白等也只是一台的时间）；它回不支持 */
        zh_dev_tick(&D, 83800);
        CHECK(last_is_lqi(0x4545, 0), "D47 表里旧短地址上的路由器也爬");
        lqi_rsp(0x4545, fr_pl(last())[0], 0x84, 0, 0, NULL, 0, 83900);
        CHECK(D.st.disc_unsupported == 2 && D.disc.q_i == 4, "D47 再跳过一台");
        /* 0x6666：页数封顶（对方报的总条数离谱） */
        uint32_t t = 84900;
        for (int pg = 0; pg < (int)ZH_DISC_PAGES_MAX; pg++) {
            zh_dev_tick(&D, t);
            const lqi_ent_t one = lqi_ent(XPAN, T1, 0x2222, ZH_LQI_TYPE_END_DEVICE, 0, 1, 2);
            lqi_rsp(0x6666, fr_pl(last())[0], 0, 255, D.disc.start_idx, &one, 1, t + 10);
            t += 1100;                             /* 应答之后至少隔 1 s 才发下一条 */
        }
        CHECK(D.st.disc_routers == 2 && D.disc.q_i == 5, "D47 一台最多读 %u 页", ZH_DISC_PAGES_MAX);
        zh_dev_tick(&D, t);
        CHECK(D.disc.state == ZH_DISC_DONE && D.st.disc_rounds == 1, "D47 队列爬完：这一轮结束");
        n0 = s_nsent;
        for (uint32_t tt = t; tt < t + 3600000u; tt += 60000u) zh_dev_tick(&D, tt);
        CHECK(count_kind(n0, ZH_DEV_TAG_KIND_DISC) == 0, "D47 一次开机只爬一轮");
    }
    {   /* 网掉暂停（在途的作废、不算次数）、NCP 拒了不干等、交不出去算一次、坏长度、层数封顶 */
        fresh();
        disc_open(0);
        zh_dev_tick(&D, ZH_DISC_START_MS);
        CHECK(last_is_lqi(0x0000, 0) && D.disc.inflight, "D47b 开始");
        zh_dev_set_net_up(&D, false);
        zh_dev_tick(&D, ZH_DISC_START_MS + 100);
        CHECK(!D.disc.inflight && D.disc.tries == 0 && D.st.disc_timeouts == 0, "D47b 网掉：在途作废、不算次数");
        int n0 = s_nsent;
        zh_dev_tick(&D, ZH_DISC_START_MS + 20000);
        CHECK(s_nsent == n0, "D47b 网不是 UP：不发");
        zh_dev_set_net_up(&D, true);
        zh_dev_tick(&D, ZH_DISC_START_MS + 20100);
        CHECK(last_is_lqi(0x0000, 0) && D.disc.tries == 0, "D47b 网回来：重发同一页");
        const uint32_t tag = s_tag[last() % 64];
        send_rsp(tag, 0x74, ZH_DISC_START_MS + 20150);   /* NCP 拒了（缺缓冲） */
        CHECK(!D.disc.inflight && D.disc.tries == 1 && D.st.disc_send_fail == 1, "D47b NCP 拒了：当场算一次，不干等 5 s");
        send_rsp(tag ^ 0x100u, 0, ZH_DISC_START_MS + 20160);
        CHECK(D.disc.tries == 1, "D47b 别的代号的结果不认");
        s_send_ok = false;
        const uint32_t busy0 = D.st.send_busy;
        zh_dev_tick(&D, ZH_DISC_START_MS + 21200);
        CHECK(D.disc.tries == 1 && D.st.disc_send_fail == 1 && D.st.send_busy == busy0 + 1 && !D.disc.inflight,
              "D47b 本地队列满交不出去：不占次数（审计 ⚠️），下一拍再试");
        s_send_ok = true;
        zh_dev_tick(&D, ZH_DISC_START_MS + 22300);
        CHECK(last_is_lqi(0x0000, 0), "D47b 再发");
        const uint8_t pl_bad[7] = { fr_pl(last())[0], 0, 3, 0, 1, 0xAA, 0xBB };   /* 说有 1 条，只带 2 字节 */
        zh_dev_on_zdo(&D, 0x0000, ZH_ZDO_LQI_TABLE_RSP, pl_bad, sizeof(pl_bad), ZH_DISC_START_MS + 22400);
        CHECK(D.st.disc_bad == 1 && D.disc.q_i == 1 && D.disc.state == ZH_DISC_RUN, "D47b 长度不对：坏应答，换下一台");
        /* 层数封顶：第 5 层的路由器表里列出的路由器不再往下排 */
        D.disc.q_i = 0; D.disc.q_n = 1; D.disc.q[0].nwk = 0x0000; D.disc.q[0].depth = ZH_DISC_DEPTH_MAX;
        zh_dev_tick(&D, ZH_DISC_START_MS + 23500);
        static const uint8_t RX[8] = { 0x09, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99 };
        const lqi_ent_t deep = lqi_ent(XPAN, RX, 0x7777, ZH_LQI_TYPE_ROUTER, 1, 1, ZH_DISC_DEPTH_MAX + 1);
        lqi_rsp(0x0000, fr_pl(last())[0], 0, 1, 0, &deep, 1, ZH_DISC_START_MS + 23600);
        CHECK(D.disc.q_n == 1 && zh_dev_find_ieee(&D, RX) >= 0, "D47b 第 %u 层的表里的路由器：建记录，但不再往下爬", ZH_DISC_DEPTH_MAX);
    }
    {   /* 网从没 UP：不开始；不知道扩展 PAN 不开始；等待期掉网回来重新等满 60 s */
        fresh();
        D.disc.state = ZH_DISC_IDLE;
        zh_dev_set_net_up(&D, false);
        for (uint32_t tt = 0; tt < 600000u; tt += 5000u) zh_dev_tick(&D, tt);
        CHECK(s_nsent == 0 && D.disc.state == ZH_DISC_IDLE, "D47c 网没 UP：不开始");
        zh_dev_set_net_up(&D, true);
        zh_dev_tick(&D, 600000u);
        zh_dev_tick(&D, 600000u + ZH_DISC_START_MS + 30000u);
        CHECK(s_nsent == 0 && D.disc.state == ZH_DISC_WAIT, "D47c 不知道本网扩展 PAN：先不开始（过滤不了别的网的条目）");
        const uint8_t zero[8] = { 0 };
        zh_dev_set_ext_pan(&D, zero);
        CHECK(!D.have_xpan, "D47c 全 0 的扩展 PAN 当没读到");
        zh_dev_set_ext_pan(&D, XPAN);
        zh_dev_tick(&D, 600000u + ZH_DISC_START_MS + 30100u);
        CHECK(last_is_lqi(0x0000, 0), "D47c 扩展 PAN 读到了：开始");
        /* 等待期掉网 */
        fresh();
        disc_open(1000);
        zh_dev_tick(&D, 50000);
        zh_dev_set_net_up(&D, false);
        zh_dev_tick(&D, 55000);
        CHECK(D.disc.state == ZH_DISC_IDLE, "D47c 还在等的时候掉了网：回到没开始");
        zh_dev_set_net_up(&D, true);
        zh_dev_tick(&D, 62000);
        zh_dev_tick(&D, 62000 + ZH_DISC_START_MS - 1);
        CHECK(s_nsent == 0, "D47c 网回来：从回来那一刻重新等满 60 s（审计 ⚠️）");
        zh_dev_tick(&D, 62000 + ZH_DISC_START_MS);
        CHECK(last_is_lqi(0x0000, 0), "D47c 等满了：开始");
    }
    {   /* 常电终端马上访谈；发现建的、没听到过的判离线静默不推；队列满计数 */
        static const uint8_t TM[8] = { 0x0B, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB };   /* 常电终端（rx 开） */
        static const uint8_t RG[8] = { 0x0C, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };   /* 表里的旧路由器：不在了 */
        fresh();
        disc_open(0);
        zh_dev_tick(&D, ZH_DISC_START_MS);
        const lqi_ent_t ee[2] = { lqi_ent(XPAN, TM, 0x5151, ZH_LQI_TYPE_END_DEVICE, 1, 1, 1), lqi_ent(XPAN, RG, 0x5252, ZH_LQI_TYPE_ROUTER, 1, 2, 1) };
        lqi_rsp(0x0000, fr_pl(last())[0], 0, 2, 0, ee, 2, ZH_DISC_START_MS + 10);
        const int km = zh_dev_find_ieee(&D, TM), kg = zh_dev_find_ieee(&D, RG);
        CHECK(km >= 0 && (D.dev[km].disc & ZH_DISC_F_RX) && (D.dev[km].disc & ZH_DISC_F_NEW) && D.dev[km].rec.node_type == ZH_NODE_END_DEVICE,
              "D47d 常电终端：标 RX、NEW");
        int n0 = s_nsent;
        bool iv_m = false;
        for (uint32_t tt = ZH_DISC_START_MS + 20; tt < ZH_DISC_START_MS + 400; tt += 50) zh_dev_tick(&D, tt);
        for (int j = n0; j < s_nsent; j++) if (fr_cluster(j) == ZH_ZDO_NODE_DESC_REQ && fr_dest(j) == 0x5151) iv_m = true;
        CHECK(iv_m, "D47d 常电终端没说过话也马上访谈（8.12 ④\"常电设备和路由器 → 立即访谈\"，审计 ⚠️）");
        /* 旧路由器：访谈不回、ping 不回 → 判离线，静默 */
        D.dev[kg].iv = ZH_IV_NONE;                 /* 只看可用性 */
        D.dev[km].iv = ZH_IV_NONE;
        const int off0 = s_ev[ZH_DEV_EV_OFFLINE];
        const uint32_t q0 = D.st.av_quiet;
        for (uint32_t tt = ZH_DISC_START_MS + 1000; tt < ZH_DISC_START_MS + ZH_AV_MAINS_SILENT_MS + 5u * ZH_AV_PING_GAP_MS; tt += 1000) zh_dev_tick(&D, tt);
        CHECK(D.dev[kg].offline && s_ev[ZH_DEV_EV_OFFLINE] == off0 && D.st.av_quiet == q0 + 1,
              "D47d 发现建的旧路由器 ping 不回：判离线但静默、不推（审计 ⚠️）");
        /* 队列满：一张表那么多已知路由器占满起点队列 */
        fresh();
        for (unsigned i = 0; i < ZH_DEV_MAX; i++) {
            uint8_t eu[8] = { 0x40, (uint8_t)i, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46 };
            annce((uint16_t)(0x1000 + i), eu, ZH_CAP_FFD | ZH_CAP_MAINS | ZH_CAP_RX_ON_IDLE, 10);
        }
        for (unsigned i = 0; i < ZH_DEV_MAX; i++) { D.dev[i].rec.flags |= ZH_DEVF_INTERVIEWED; D.dev[i].iv = ZH_IV_NONE; }
        disc_open(100);
        zh_dev_tick(&D, 100 + ZH_DISC_START_MS);
        CHECK(D.disc.q_n == ZH_DISC_Q_MAX && D.st.disc_q_full == 0, "D47d 协调器 + %u 台已知路由器正好排满", ZH_DEV_MAX);
        const lqi_ent_t nr = lqi_ent(XPAN, RG, 0x6060, ZH_LQI_TYPE_ROUTER, 1, 2, 1);
        lqi_rsp(0x0000, fr_pl(last())[0], 0, 1, 0, &nr, 1, 200 + ZH_DISC_START_MS);
        CHECK(D.st.disc_q_full == 1 && D.disc.q_n == ZH_DISC_Q_MAX, "D47d 排不下的路由器要计数（审计 ⚠️）");
    }
    /* ---- D48 ---- 访谈失败、随时听得见的：到点自己重来（2026-10-24：Sengled 灯重新入网那一刻被访谈，15 s 三问不回判失败；
       它从不主动说话、也不宣告，"失败一小时后再说话"永远等不来，直到网关重启） */
    {
        fresh();
        annce(0x4801, EA, ZH_CAP_MAINS | ZH_CAP_RX_ON_IDLE, 1000);     /* 常电终端（不是路由器）：Sengled 灯就是这样 */
        const int i = zh_dev_find_ieee(&D, EA);
        uint32_t t = 1000;
        zh_dev_tick(&D, t);
        const int ev0 = s_ev[ZH_DEV_EV_IV_FAILED];
        iv_fail_now(i, &t);
        CHECK(D.dev[i].iv == ZH_IV_FAILED && D.dev[i].iv_fails == 1 && s_ev[ZH_DEV_EV_IV_FAILED] == ev0 + 1, "D48 常电终端三问不回：访谈失败、记第 1 次、出一次事件");
        static const uint32_t gaps[5] = { ZH_IV_ON_RETRY1_MS, ZH_IV_ON_RETRY2_MS, ZH_IV_ON_RETRY3_MS, ZH_IV_RETRY_MS, ZH_IV_RETRY_MS };
        for (int k = 0; k < 5; k++) {
            const uint32_t tf = D.dev[i].iv_t_fail;
            const int n0 = s_nsent;
            const uint32_t r0 = D.st.iv_retries;
            zh_dev_tick(&D, tf + gaps[k] - 1);
            CHECK(D.dev[i].iv == ZH_IV_FAILED && !node_desc_since(n0, 0x4801), "D48 第 %d 次失败后不到 %u ms：不重来、不发", k + 1, gaps[k]);
            const int n1 = s_nsent;
            zh_dev_tick(&D, tf + gaps[k]);
            CHECK(D.dev[i].iv == ZH_IV_NODE && D.st.iv_retries == r0 + 1 && node_desc_since(n1, 0x4801),
                  "D48 第 %d 次失败后满 %u ms：它没说话也自己重来，重开那一拍就发 Node_Desc_req", k + 1, gaps[k]);
            t = tf + gaps[k];
            iv_fail_now(i, &t);
            CHECK(D.dev[i].iv == ZH_IV_FAILED && D.dev[i].iv_fails == k + 2, "D48 又失败：记第 %d 次", k + 2);
        }
        CHECK(s_ev[ZH_DEV_EV_IV_FAILED] == ev0 + 1, "D48 到点重来又失败的不再出事件（一轮只记第一次：不在了的设备每小时一条会冲掉事件环，审计 ⚠️）");
        /* 已经在每小时那档时它重新宣告（刚上电）：马上重来；这一下又赶在它就绪之前失败了 —— 从 30 s 那档算，不是再等一小时（审计 ⚠️） */
        t = D.dev[i].iv_t_fail + 1000;
        annce(0x4801, EA, ZH_CAP_MAINS | ZH_CAP_RX_ON_IDLE, t);
        CHECK(D.dev[i].iv == ZH_IV_NODE && D.dev[i].iv_fails == 0, "D48 重新宣告：马上重来、失败次数清零");
        zh_dev_tick(&D, t);
        iv_fail_now(i, &t);
        CHECK(s_ev[ZH_DEV_EV_IV_FAILED] == ev0 + 2, "D48 重新宣告之后的那轮失败：照样记一次事件");
        zh_dev_tick(&D, D.dev[i].iv_t_fail + ZH_IV_ON_RETRY1_MS - 1);
        CHECK(D.dev[i].iv == ZH_IV_FAILED, "D48 宣告后再失败：不到 30 s 不重来");
        zh_dev_tick(&D, D.dev[i].iv_t_fail + ZH_IV_ON_RETRY1_MS);
        CHECK(D.dev[i].iv == ZH_IV_NODE, "D48 宣告后再失败：30 s 那档");
        iv_fail_now(i, &t);
        /* 这次回了：访谈完，间隔清回第一档 */
        t = D.dev[i].iv_t_fail + ZH_IV_RETRY_MS;
        const uint32_t r6 = D.st.iv_retries;
        zh_dev_tick(&D, t);
        CHECK(D.dev[i].iv == ZH_IV_NODE && D.st.iv_retries == r6 + 1, "D48 到点重来");
        t += 50;
        node_rsp(last_seq(), 0, 0x4801, 0x02, 0x8C, 0x1160, t);         /* 回在途那条，后面的交给 auto_answer */
        auto_answer(i, 0x4801, &t, false, ZH_IV_NONE);
        CHECK((D.dev[i].rec.flags & ZH_DEVF_INTERVIEWED) && D.dev[i].iv_fails == 0, "D48 这次回了：访谈完、失败次数清零");
    }
    {   /* 短地址作废的（被别的设备占了，等它下次说话再学）：不重来 —— 发出去就是广播，iv_t0 也不起算、会一直挂着 */
        fresh();
        annce(0x4808, EA, ZH_CAP_MAINS | ZH_CAP_RX_ON_IDLE, 1000);
        const int i = zh_dev_find_ieee(&D, EA);
        uint32_t t = 1000;
        zh_dev_tick(&D, t);
        iv_fail_now(i, &t);
        D.dev[i].rec.nwk = 0xFFFF;                                     /* learn() 里被占时的样子 */
        const int n0 = s_nsent;
        zh_dev_tick(&D, D.dev[i].iv_t_fail + ZH_IV_ON_RETRY1_MS);
        CHECK(D.dev[i].iv == ZH_IV_FAILED && D.st.iv_retries == 0 && s_nsent == n0, "D48 短地址作废：不重来、不发");
        D.dev[i].rec.nwk = 0x4808;                                     /* 25 天后被动学习拿回地址（这条路不触发重来） */
        zh_dev_set_net_up(&D, false);                                  /* 期间别的逻辑别动它：只看重来的时间比较 */
        zh_dev_set_net_up(&D, true);
        zh_dev_tick(&D, D.dev[i].iv_t_fail + 25u * 86400000u);
        CHECK(D.dev[i].iv == ZH_IV_NODE && D.st.iv_retries == 1, "D48 失败 25 天后（差值过了 2^31 ms）：照样到点重来（无符号差，审计 ⚠️）");
    }
    {   /* 主动发现建的、这次开机一次都没听到过（表里的旧条目，多半根本不在了）：30 s / 2 min / 10 min 试过就不再每小时问；听到过的照旧 */
        for (int heard = 0; heard < 2; heard++) {
            fresh();
            annce(0x4809, EA, ZH_CAP_FFD | ZH_CAP_MAINS | ZH_CAP_RX_ON_IDLE, 1000);
            const int i = zh_dev_find_ieee(&D, EA);
            D.dev[i].disc |= ZH_DISC_F_NEW;                            /* 发现建的样子：没宣告过、没听到过 */
            D.dev[i].seen = heard != 0;
            uint32_t t = 1000;
            zh_dev_tick(&D, t);
            iv_fail_now(i, &t);
            for (int k = 0; k < 3; k++) {
                zh_dev_tick(&D, D.dev[i].iv_t_fail + iv_gap_of(k + 1));
                t = D.dev[i].iv_t_fail + iv_gap_of(k + 1);
                iv_fail_now(i, &t);
            }
            const uint32_t r0 = D.st.iv_retries;
            CHECK(r0 == 3 && D.dev[i].iv_fails == 4, "D48 前三档都试了（heard=%d）", heard);
            zh_dev_tick(&D, D.dev[i].iv_t_fail + ZH_IV_RETRY_MS);
            if (heard) CHECK(D.dev[i].iv == ZH_IV_NODE && D.st.iv_retries == r0 + 1, "D48 听到过的：每小时照旧重来");
            else CHECK(D.dev[i].iv == ZH_IV_FAILED && D.st.iv_retries == r0, "D48 发现建的、从没听到过：不再每小时问");
        }
    }
    {   /* 离开前没访谈完、回来了：重来，失败次数清零 */
        fresh();
        annce(0x480A, EA, ZH_CAP_MAINS | ZH_CAP_RX_ON_IDLE, 1000);
        const int i = zh_dev_find_ieee(&D, EA);
        uint32_t t = 1000;
        zh_dev_tick(&D, t);
        iv_fail_now(i, &t);
        for (int k = 0; k < 3; k++) { t = D.dev[i].iv_t_fail + iv_gap_of(k + 1); zh_dev_tick(&D, t); iv_fail_now(i, &t); }
        CHECK(D.dev[i].iv_fails == 4, "D48 已经在每小时那档");
        zh_dev_on_tc_join(&D, 0x480A, EA, ZH_JOIN_DEVICE_LEFT, ZH_JOIN_USE_PRECONFIGURED_KEY, 0, t + 100);
        CHECK(D.dev[i].rec.flags & ZH_DEVF_LEFT, "D48 离开了");
        zh_dev_on_tc_join(&D, 0x480B, EA, ZH_JOIN_SECURED_REJOIN, ZH_JOIN_NO_ACTION, 0, t + 200);
        CHECK(!(D.dev[i].rec.flags & ZH_DEVF_LEFT) && D.dev[i].iv == ZH_IV_NODE && D.dev[i].iv_fails == 0, "D48 加密重新入网回来：重来、失败次数清零");
    }
    {   /* 休眠的（不知道空闲时收不收）：不自己重来，照旧等它说话 */
        fresh();
        annce(0x4802, EA, 0x80, 1000);
        const int i = zh_dev_find_ieee(&D, EA);
        uint32_t t = 1000;
        for (int k = 0; k < (int)ZH_IV_TRIES; k++) {
            spoke(0x4802, t);
            zh_dev_tick(&D, t);
            t += ZH_IV_STEP_TIMEOUT_MS;
            zh_dev_tick(&D, t);
        }
        CHECK(D.dev[i].iv == ZH_IV_FAILED, "D48 休眠终端先失败一次");
        const uint32_t tf = D.dev[i].iv_t_fail;
        spoke(0x4802, tf + ZH_IV_ON_RETRY1_MS + 100);                  /* 它 30 s 后又说话（醒着）：照旧要满一小时，不按常电的档 */
        zh_dev_tick(&D, tf + ZH_IV_ON_RETRY1_MS + 100);
        CHECK(D.dev[i].iv == ZH_IV_FAILED && D.st.iv_retries == 0, "D48 休眠终端 30 s 后说话：不重来（休眠的照旧满 %u ms 再说话才重来）", ZH_IV_RETRY_MS);
        const int n0 = s_nsent;
        for (uint32_t tt = t; tt <= t + ZH_IV_RETRY_MS + 60000u; tt += 30000u) zh_dev_tick(&D, tt);
        CHECK(D.dev[i].iv == ZH_IV_FAILED && D.st.iv_retries == 0 && s_nsent == n0, "D48 休眠终端：一小时多不说话，不自己重来、不发");
    }
    {   /* 网不是 UP：不重来；回来了再重来 */
        fresh();
        annce(0x4803, EA, ZH_CAP_FFD | ZH_CAP_MAINS | ZH_CAP_RX_ON_IDLE, 1000);
        const int i = zh_dev_find_ieee(&D, EA);
        uint32_t t = 1000;
        zh_dev_tick(&D, t);
        iv_fail_now(i, &t);
        zh_dev_set_net_up(&D, false);
        zh_dev_tick(&D, D.dev[i].iv_t_fail + ZH_IV_ON_RETRY1_MS + 1000);
        CHECK(D.dev[i].iv == ZH_IV_FAILED && D.st.iv_retries == 0, "D48 网不是 UP：不重来");
        zh_dev_set_net_up(&D, true);
        zh_dev_tick(&D, D.dev[i].iv_t_fail + ZH_IV_ON_RETRY1_MS + 2000);
        CHECK(D.dev[i].iv == ZH_IV_NODE && D.st.iv_retries == 1, "D48 网回来：重来");
    }
    {   /* 已有记录的常电灯重新配对（就是 10-24 那盏）：失败次数清零；到点重来成功，照样推"认出来了"（来源 JOIN） */
        fresh();
        annce(0x4804, EA, ZH_CAP_MAINS | ZH_CAP_RX_ON_IDLE, 1000);
        const int i = zh_dev_find_ieee(&D, EA);
        uint32_t t = 1000;
        auto_answer(i, 0x4804, &t, false, ZH_IV_NONE);
        CHECK(D.dev[i].rec.flags & ZH_DEVF_INTERVIEWED, "D48 灯先访谈完");
        D.dev[i].iv_fails = 3;                                         /* 之前失败过好几次（不该带进新的一次配对） */
        const int id0 = s_ev[ZH_DEV_EV_IDENTIFIED], f0 = s_ev[ZH_DEV_EV_IV_FAILED];
        t += 1000;
        zh_dev_on_tc_join(&D, 0x4805, EA, ZH_JOIN_UNSECURED_JOIN, ZH_JOIN_USE_PRECONFIGURED_KEY, 0, t);
        CHECK(D.dev[i].iv == ZH_IV_NODE && D.dev[i].iv_new && D.dev[i].iv_fails == 0, "D48 重新配对：重新访谈、失败次数清零");
        zh_dev_tick(&D, t);
        iv_fail_now(i, &t);
        CHECK(D.dev[i].iv == ZH_IV_FAILED && s_ev[ZH_DEV_EV_IV_FAILED] == f0 + 1 && s_last_src == ZH_DEV_SRC_JOIN, "D48 入网那一刻访谈不回：失败、推一次\"认不出来\"");
        t = D.dev[i].iv_t_fail + ZH_IV_ON_RETRY1_MS;
        const int n5 = s_nsent;
        zh_dev_tick(&D, t);
        CHECK(D.dev[i].iv == ZH_IV_NODE && node_desc_since(n5, 0x4805), "D48 30 s 后自己重来（发往新地址）");
        t += 50;
        node_rsp(last_seq(), 0, 0x4805, 0x02, 0x8C, 0x1160, t);         /* 回在途那条（常电终端），后面的交给 auto_answer */
        auto_answer(i, 0x4805, &t, false, ZH_IV_NONE);
        CHECK((D.dev[i].rec.flags & ZH_DEVF_INTERVIEWED) && s_ev[ZH_DEV_EV_IDENTIFIED] == id0 + 1 && s_ev_src_last_identified_join(),
              "D48 重来成功：推\"认出来了\"（来源 JOIN：Muse 之前收到过认不出来）");
    }
    {   /* 一拍只重开一台：两台同时到点，先重开一台，它那条完了再轮另一台 */
        fresh();
        annce(0x4806, EA, ZH_CAP_MAINS | ZH_CAP_RX_ON_IDLE, 1000);
        annce(0x4807, EB, ZH_CAP_MAINS | ZH_CAP_RX_ON_IDLE, 1000);
        const int ia = zh_dev_find_ieee(&D, EA), ib = zh_dev_find_ieee(&D, EB);
        uint32_t t = 1000;
        for (int g = 0; g < 64 && !(D.dev[ia].iv == ZH_IV_FAILED && D.dev[ib].iv == ZH_IV_FAILED); g++) { zh_dev_tick(&D, t); t += ZH_IV_STEP_TIMEOUT_MS; }
        CHECK(D.dev[ia].iv == ZH_IV_FAILED && D.dev[ib].iv == ZH_IV_FAILED, "D48 两台都失败");
        const uint32_t tl = D.dev[ia].iv_t_fail > D.dev[ib].iv_t_fail ? D.dev[ia].iv_t_fail : D.dev[ib].iv_t_fail;
        zh_dev_tick(&D, tl + ZH_IV_ON_RETRY1_MS);
        CHECK(D.st.iv_retries == 1 && (D.dev[ia].iv == ZH_IV_NODE) != (D.dev[ib].iv == ZH_IV_NODE), "D48 两台同时到点：这一拍只重开一台");
        zh_dev_tick(&D, tl + ZH_IV_ON_RETRY1_MS + ZH_IV_STEP_TIMEOUT_MS);
        CHECK(D.st.iv_retries == 2 && D.dev[ia].iv == ZH_IV_NODE && D.dev[ib].iv == ZH_IV_NODE, "D48 前一台那条完了：轮到另一台");
    }
    printf("zh_dev: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}

/* 计划第 k 条是哪个簇：k_cfg 是 zh_dev.c 的私有表，这里照着取行号对上表 8.6-1 的顺序（开关 0、亮度 1、…） */
static uint16_t k_cfg_cl_of(int idx, int k)
{
    static const uint16_t rows[] = { ZH_CL_ONOFF, ZH_CL_LEVEL, ZH_CL_TEMPERATURE, ZH_CL_HUMIDITY, ZH_CL_ILLUMINANCE, ZH_CL_PRESSURE, ZH_CL_OCCUPANCY, ZH_CL_POWER_CFG };
    const unsigned r = D.dev[idx].cfg[k][1];
    return r < sizeof(rows) / sizeof(rows[0]) ? rows[r] : 0xFFFF;
}
