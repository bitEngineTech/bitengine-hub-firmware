/* zh_dev —— zhub 的设备层（规则与理由见 include/zh_dev.h）。 */
#include "zh_dev.h"

#include <math.h>
#include <string.h>

#define NWK_COORD      0x0000u
#define NWK_RESERVED   0xFFF8u   /* 0xFFF8–0xFFFF：广播 / 保留，不是设备 */
#define NWK_UNKNOWN    0xFFFFu   /* 记录里的短地址作废（被别的设备占了，等它下次宣告 / 入网再填） */

static bool expired(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

/* 全 0 / 全 FF 的 IEEE 不是真地址（NCP 地址表空位、坏帧） */
static bool eui_valid(const uint8_t e[8])
{
    bool zero = true, ff = true;
    for (int i = 0; i < 8; i++) {
        if (e[i] != 0x00) zero = false;
        if (e[i] != 0xFF) ff = false;
    }
    return !zero && !ff;
}

void zh_dev_init(zh_dev_core_t *d, const zh_dev_ops_t *ops)
{
    if (!d) return;
    memset(d, 0, sizeof(*d));
    if (ops) d->ops = *ops;
}

int zh_dev_find_ieee(const zh_dev_core_t *d, const uint8_t ieee[8])
{
    if (!d || !ieee) return -1;
    for (unsigned i = 0; i < ZH_DEV_MAX; i++)
        if (d->dev[i].used && memcmp(d->dev[i].rec.ieee, ieee, 8) == 0) return (int)i;
    return -1;
}

/* 只找没标 left 的：left 设备的短地址可能已经被新设备占了，同一个 nwk 发来的消息要靠被动学习认 IEEE */
int zh_dev_find_nwk(const zh_dev_core_t *d, uint16_t nwk)
{
    if (!d || nwk == NWK_UNKNOWN) return -1;
    for (unsigned i = 0; i < ZH_DEV_MAX; i++) {
        const zh_dev_t *x = &d->dev[i];
        if (x->used && x->rec.nwk == nwk && !(x->rec.flags & ZH_DEVF_LEFT)) return (int)i;
    }
    return -1;
}

/* 还有能分配给新设备的格子（没占用、没保留） */
static bool has_free(const zh_dev_core_t *d)
{
    for (unsigned i = 0; i < ZH_DEV_MAX; i++)
        if (!d->dev[i].used && !d->dev[i].reserved) return true;
    return false;
}

unsigned zh_dev_count(const zh_dev_core_t *d)
{
    unsigned n = 0;
    for (unsigned i = 0; d && i < ZH_DEV_MAX; i++) n += d->dev[i].used;
    return n;
}

/* 结构变了：落盘（唯一调 ops.save 的地方；收到消息只改内存里的 last_seen / 信号，8.9） */
static void dev_save(zh_dev_core_t *d, unsigned idx)
{
    d->dirty = true;
    if (d->ops.save) d->ops.save(d->ops.ctx, idx, &d->dev[idx].rec);
}

static void dev_event(zh_dev_core_t *d, zh_dev_ev_t ev, zh_dev_src_t src, unsigned idx)
{
    if (d->ops.event) d->ops.event(d->ops.ctx, ev, src, idx, &d->dev[idx].rec);
}

static void mark_seen(zh_dev_t *x, int8_t rssi, uint8_t lqi, bool have_link, uint32_t now)
{
    x->seen = true;
    x->t_seen = now;
    if (have_link) {
        x->rssi = rssi;
        x->lqi = lqi;
    }
}

static void iv_start(zh_dev_core_t *d, unsigned idx, bool is_new, uint32_t now);
static bool ias_enroll_rsp(zh_dev_core_t *d, unsigned idx, uint8_t dst_ep);

/* CONFIGURE 第二步的表（设计表 8.6-1；初值参照 zigbee-herdsman-converters 的常用默认）：只列 zhub 解得开读数的簇（st_apply）。
   类型照 XML（与 st_apply 收读数时认的类型一致）；变化量只给模拟量类型（bool / bitmap 是离散的，reporting.c:461-473） */
typedef struct {
    uint16_t cl, attr;
    uint8_t  type;
    uint16_t min_s, max_s;
    uint8_t  chg_len;
    uint32_t chg;
} cfg_row_t;
static const cfg_row_t k_cfg[] = {
    { ZH_CL_ONOFF,       ZH_ATTR_ON_OFF,            ZH_ZCL_T_BOOL,    0,    300,   0, 0 },
    { ZH_CL_LEVEL,       ZH_ATTR_CURRENT_LEVEL,     ZH_ZCL_T_UINT8,   1,    300,   1, 1 },
    { ZH_CL_TEMPERATURE, ZH_ATTR_TEMP_MEASURED,     ZH_ZCL_T_INT16,   30,   600,   2, 10 },    /* 0.1 °C */
    { ZH_CL_HUMIDITY,    ZH_ATTR_HUMIDITY_MEASURED, ZH_ZCL_T_UINT16,  30,   600,   2, 100 },   /* 1 % */
    { ZH_CL_ILLUMINANCE, ZH_ATTR_ILLUM_MEASURED,    ZH_ZCL_T_UINT16,  10,   600,   2, 500 },
    { ZH_CL_PRESSURE,    ZH_ATTR_PRESSURE_MEASURED, ZH_ZCL_T_INT16,   30,   600,   2, 1 },
    { ZH_CL_OCCUPANCY,   ZH_ATTR_OCCUPANCY,         ZH_ZCL_T_BITMAP8, 0,    600,   0, 0 },
    { ZH_CL_POWER_CFG,   ZH_ATTR_BATTERY_PCT2,      ZH_ZCL_T_UINT8,   3600, 43200, 1, 2 },     /* 电量 %（0.5 % 一格）：只给非路由器 */
};
#define K_CFG_N (sizeof(k_cfg) / sizeof(k_cfg[0]))
_Static_assert(K_CFG_N <= 255u, "行号存在 uint8_t 里");
static void cie_resume(zh_dev_core_t *d, unsigned idx, uint32_t gap_ms);
static void iv_to_config(zh_dev_core_t *d, unsigned idx);

/* 知道了 nwk ↔ IEEE：建记录或更新（短地址变了、标过 left 的回来了）。返回下标，没建成 -1。
   caps：新建时就写进去的节点类型 / 标志（Device_annce 带着能力字节：一次写好，别先存一条"不知道"再改一次）；NULL = 不知道 */
typedef struct { uint8_t node_type, flags; } learn_caps_t;
static int learn(zh_dev_core_t *d, uint16_t nwk, const uint8_t eui[8], zh_dev_src_t src, const learn_caps_t *caps, uint32_t now)
{
    if (!eui_valid(eui)) {
        d->st.bad_eui++;
        return -1;
    }
    int idx = zh_dev_find_ieee(d, eui);
    /* 别的记录还挂着这个短地址：那台设备换过地址了，它的短地址作废（等它下次宣告 / 入网再填） */
    const int other = zh_dev_find_nwk(d, nwk);
    if (other >= 0 && other != idx) {
        d->dev[other].rec.nwk = NWK_UNKNOWN;
        dev_save(d, (unsigned)other);
    }
    if (idx >= 0) {
        zh_dev_t *x = &d->dev[idx];
        bool changed = false;
        if (x->rec.nwk != nwk) {
            x->rec.nwk = nwk;
            changed = true;
            d->st.nwk_changes++;
            dev_event(d, ZH_DEV_EV_NWK, src, (unsigned)idx);
        }
        if (x->rec.flags & ZH_DEVF_LEFT) {
            x->rec.flags &= (uint8_t)~ZH_DEVF_LEFT;
            changed = true;
            d->st.back++;
            dev_event(d, ZH_DEV_EV_BACK, src, (unsigned)idx);
            if (!(x->rec.flags & ZH_DEVF_INTERVIEWED)) {   /* 离开前没访谈完：回来了再来（它刚回来：失败了从 30 s 那档算） */
                x->iv_fails = 0;
                iv_start(d, (unsigned)idx, false, now);
            }
        }
        if (changed) {
            d->st.updated++;
            dev_save(d, (unsigned)idx);
        }
        if (src == ZH_DEV_SRC_REJOIN) cie_resume(d, (unsigned)idx, ZH_CIE_RESUME_GAP_MS);   /* 加密重新入网：设备醒着，欠着 CIE / 登记的补上 */
        return idx;
    }
    for (unsigned i = 0; i < ZH_DEV_MAX; i++) {
        if (d->dev[i].used || d->dev[i].reserved) continue;
        zh_dev_t *x = &d->dev[i];
        memset(x, 0, sizeof(*x));
        x->used = true;
        x->rec.ver = ZH_DEV_REC_VER;
        memcpy(x->rec.ieee, eui, 8);
        x->rec.nwk = nwk;
        x->rec.node_type = caps ? caps->node_type : (uint8_t)ZH_NODE_UNKNOWN;
        if (caps) x->rec.flags = caps->flags;
        x->t_seen = now;                           /* 可用性从现在起算 */
        x->rec.zone_type = ZH_ZONE_TYPE_UNKNOWN;
        d->st.added++;
        dev_save(d, i);
        dev_event(d, ZH_DEV_EV_ADDED, src, i);
        iv_start(d, i, src == ZH_DEV_SRC_JOIN, now);
        return (int)i;
    }
    d->st.full++;
    return -1;
}

/* ---------------------------------------------------------------- 被动学习 */

static zh_dev_look_t *look_find(zh_dev_core_t *d, uint16_t nwk)
{
    for (unsigned i = 0; i < ZH_DEV_LOOK_N; i++)
        if (d->look[i].stage && d->look[i].nwk == nwk) return &d->look[i];
    return NULL;
}

static bool gap_recent(const zh_dev_core_t *d, uint16_t nwk, uint32_t now)
{
    for (unsigned i = 0; i < ZH_DEV_LOOK_N; i++)
        if (d->gap[i].t && d->gap[i].nwk == nwk && now - d->gap[i].t < ZH_DEV_LOOKUP_GAP_MS) return true;
    return false;
}

static void gap_note(zh_dev_core_t *d, uint16_t nwk, uint32_t now)
{
    d->gap[d->gap_head].nwk = nwk;
    d->gap[d->gap_head].t = now ? now : 1u;        /* 0 当"空" */
    d->gap_head = (uint8_t)((d->gap_head + 1u) % ZH_DEV_LOOK_N);
}

static uint32_t dev_tag(const zh_dev_core_t *d, uint32_t kind, unsigned slot) { return kind | ((uint32_t)slot << 4) | ((uint32_t)d->look[slot].gen << 8); }

static bool send_lookup(zh_dev_core_t *d, unsigned slot)
{
    uint8_t b[16];
    const size_t n = zh_ezsp_enc_lookup_eui64(b, sizeof(b), 0, d->look[slot].nwk);
    return n && d->ops.send && d->ops.send(d->ops.ctx, b, n, dev_tag(d, ZH_DEV_TAG_KIND_LOOKUP, slot), true);   /* 读 NCP 的表：幂等 */
}

/* ZDO IEEE_addr_req 单播给它：[序号][nwk:2][请求类型 0 = 只要它自己][起始下标 0]（zigbee-device-host.c:47-61 sl_zigbee_ieee_address_request） */
static bool send_ieee_req(zh_dev_core_t *d, unsigned slot)
{
    zh_dev_look_t *L = &d->look[slot];
    L->zdo_seq = d->ops.zdo_seq ? d->ops.zdo_seq(d->ops.ctx) : 0;
    const uint8_t pl[5] = { L->zdo_seq, (uint8_t)L->nwk, (uint8_t)(L->nwk >> 8), 0, 0 };
    const zh_ezsp_aps_t aps = { .profile_id = 0x0000, .cluster_id = ZH_ZDO_IEEE_ADDR_REQ, .src_ep = 0, .dst_ep = 0,
                                .options = ZH_APS_OPTS_UNICAST, .group_id = 0, .sequence = 0 };
    uint8_t b[48];
    const size_t n = zh_ezsp_enc_send_unicast(b, sizeof(b), 0, ZH_OUTGOING_DIRECT, L->nwk, &aps, L->zdo_seq, sizeof(pl), pl);
    return n && d->ops.send && d->ops.send(d->ops.ctx, b, n, dev_tag(d, ZH_DEV_TAG_KIND_IEEE, slot), false);   /* sendUnicast 不幂等 */
}

static void start_learn(zh_dev_core_t *d, uint16_t nwk, int8_t rssi, uint8_t lqi, uint32_t now)
{
    if (nwk == NWK_COORD || nwk >= NWK_RESERVED) return;
    if (look_find(d, nwk) || gap_recent(d, nwk, now)) return;
    if (d->gap[d->gap_head].t && now - d->gap[d->gap_head].t < ZH_DEV_LOOKUP_GAP_MS) {
        d->st.deferred++;                          /* 10 s 内已开了 ZH_DEV_LOOK_N 个：再开就要盖掉一条还没满 10 s 的间隔记录 */
        return;
    }
    unsigned slot = ZH_DEV_LOOK_N;
    for (unsigned i = 0; i < ZH_DEV_LOOK_N; i++) {
        if (!d->look[i].stage) {
            slot = i;
            break;
        }
    }
    if (slot == ZH_DEV_LOOK_N) return;             /* 同时在查的满了：等它下次说话 */
    gap_note(d, nwk, now);
    d->look[slot].nwk = nwk;
    d->look[slot].rssi = rssi;
    d->look[slot].lqi = lqi;
    d->look[slot].gen = d->gen++;
    d->look[slot].stage = 1;
    d->look[slot].t_start = now;
    d->st.lookups++;
    if (!send_lookup(d, slot)) {
        d->st.send_busy++;
        d->look[slot].stage = 0;                   /* 交不出去：放掉，间隔到了它再说话时重来 */
    }
}

/* ---------------------------------------------------------------- 主动发现（8.12 ④；M6 第一步，规则见 include/zh_dev.h 文件头） */

static bool disc_queued(const zh_disc_t *s, uint16_t nwk)
{
    for (unsigned i = 0; i < s->q_n; i++)
        if (s->q[i].nwk == nwk) return true;
    return false;
}

static void disc_enqueue(zh_dev_core_t *d, uint16_t nwk, uint8_t depth)
{
    zh_disc_t *s = &d->disc;
    if (disc_queued(s, nwk)) return;
    if (s->q_n >= ZH_DISC_Q_MAX) {                 /* 漏爬的要看得出来（审计 ⚠️） */
        d->st.disc_q_full++;
        return;
    }
    s->q[s->q_n].nwk = nwk;
    s->q[s->q_n].depth = depth;
    s->q_n++;
}

/* 起点：协调器（发给自己：NCP 的 ZDO 会应答，zigpy 默认也这样扫协调器）+ 设备表里已知的路由器（回环要是不通，照样能往下爬） */
static void disc_seed(zh_dev_core_t *d)
{
    zh_disc_t *s = &d->disc;
    s->q_n = s->q_i = 0;
    s->start_idx = s->pages = s->tries = 0;
    s->inflight = false;
    disc_enqueue(d, NWK_COORD, 0);
    for (unsigned i = 0; i < ZH_DEV_MAX; i++) {
        const zh_dev_t *x = &d->dev[i];
        if (x->used && x->rec.node_type == ZH_NODE_ROUTER && !(x->rec.flags & ZH_DEVF_LEFT) && x->rec.nwk != NWK_COORD && x->rec.nwk < NWK_RESERVED)
            disc_enqueue(d, x->rec.nwk, 1);
    }
}

/* 这一台读完了（或放弃 / 不支持）：换下一台 */
static void disc_next(zh_disc_t *s)
{
    s->q_i++;
    s->start_idx = s->pages = s->tries = 0;
}

/* 这一页的这一次没成（超时 / 交不出去 / NCP 拒了）：次数用完就放弃这一台 */
static void disc_try_failed(zh_dev_core_t *d, uint32_t now)
{
    zh_disc_t *s = &d->disc;
    s->inflight = false;
    if (++s->tries >= ZH_DISC_TRIES) {
        d->st.disc_gave_up++;
        disc_next(s);
    }
    s->t_next = now + ZH_DISC_GAP_MS;
}

static void disc_send(zh_dev_core_t *d, uint32_t now)
{
    zh_disc_t *s = &d->disc;
    const uint16_t nwk = s->q[s->q_i].nwk;
    s->zdo_seq = d->ops.zdo_seq ? d->ops.zdo_seq(d->ops.ctx) : 0;
    s->gen++;
    const uint8_t pl[2] = { s->zdo_seq, s->start_idx };
    /* APS 只开 RETRY：SDK 的先例 zdo-cli.c:296-298 zdoMgmtLqiCommand（sl_zigbee_lqi_table_request(…, SL_ZIGBEE_APS_OPTION_RETRY)）；
       表里 IEEE 不认的路由器也要问，开地址发现帮不上 */
    const zh_ezsp_aps_t aps = { .profile_id = 0x0000, .cluster_id = ZH_ZDO_LQI_TABLE_REQ, .src_ep = 0, .dst_ep = 0,
                                .options = ZH_APS_OPTS_UNICAST, .group_id = 0, .sequence = 0 };
    uint8_t b[48];
    const size_t n = zh_ezsp_enc_send_unicast(b, sizeof(b), 0, ZH_OUTGOING_DIRECT, nwk, &aps, s->zdo_seq, sizeof(pl), pl);
    const uint32_t tag = ZH_DEV_TAG_KIND_DISC | ((uint32_t)s->gen << 8);
    if (!n) {                                      /* 编不出来（不会发生）：算用掉一次，别一直卡在这台 */
        d->st.disc_send_fail++;
        disc_try_failed(d, now);
        return;
    }
    if (!d->ops.send || !d->ops.send(d->ops.ctx, b, n, tag, false)) {   /* sendUnicast 不幂等 */
        d->st.send_busy++;                         /* 应用命令队列满：下一拍再试，不占次数（同可用性 / 补读的惯例；审计 ⚠️） */
        s->t_next = now + ZH_DISC_GAP_MS;
        return;
    }
    s->inflight = true;
    s->t_req = now;
    d->st.disc_reqs++;
}

/* 一条邻居条目（22 B，布局见 ZH_ZDO_LQI_ENTRY_LEN）。depth：这张表的主人在第几层 */
static void disc_entry(zh_dev_core_t *d, const uint8_t *e, uint8_t depth)
{
    const uint8_t *ieee = e + 8;
    const uint16_t nwk = (uint16_t)(e[16] | (e[17] << 8));
    const uint8_t type = e[18] & ZH_LQI_TYPE_MASK;
    const uint8_t rx = (uint8_t)((e[18] >> ZH_LQI_RX_SHIFT) & 0x03u);
    const uint8_t rel = (uint8_t)((e[18] >> ZH_LQI_REL_SHIFT) & 0x07u);
    d->st.disc_entries++;
    if (d->have_xpan && memcmp(e, d->xpan, 8) != 0) {   /* 别的网的 */
        d->st.disc_xpan++;
        return;
    }
    if (rel >= ZH_LQI_REL_PREV_CHILD || nwk == NWK_COORD || nwk >= NWK_RESERVED) {
        d->st.disc_skipped++;                      /* 旧条目（前子节点）/ 协调器自己 / 广播保留段 */
        return;
    }
    /* 路由器排进下一层（不管认不认识它：认识的路由器的表里也可能有不认识的设备） */
    if (type == ZH_LQI_TYPE_ROUTER && depth + 1u <= ZH_DISC_DEPTH_MAX) disc_enqueue(d, nwk, (uint8_t)(depth + 1u));
    if (!eui_valid(ieee)) {                        /* 有的协议栈对不知道 IEEE 的邻居填全 0 / 全 FF：等它说话时被动学习 */
        d->st.bad_eui++;
        return;
    }
    if (zh_dev_find_ieee(d, ieee) >= 0) {          /* 认识的不改：短地址 / 能力以宣告、被动学习、访谈为准（表可能是旧的） */
        d->st.disc_known++;
        return;
    }
    if (zh_dev_find_nwk(d, nwk) >= 0) {            /* 短地址挂在别的记录上：表和记录有一边是旧的，别拿表去作废那条记录 */
        d->st.disc_conflict++;
        return;
    }
    if (!has_free(d)) {
        d->st.full++;
        return;
    }
    learn_caps_t caps = { .node_type = ZH_NODE_UNKNOWN, .flags = rx == 1u ? ZH_DEVF_RX_ON : 0 };
    if (type == ZH_LQI_TYPE_ROUTER) caps = (learn_caps_t){ .node_type = ZH_NODE_ROUTER, .flags = ZH_DEVF_RX_ON };
    else if (type == ZH_LQI_TYPE_END_DEVICE) caps.node_type = ZH_NODE_END_DEVICE;
    const int idx = learn(d, nwk, ieee, ZH_DEV_SRC_DISCOVERY, &caps, d->t_now);
    if (idx < 0) return;
    d->dev[idx].disc = (uint8_t)(ZH_DISC_F_NEW | (rx == 1u ? ZH_DISC_F_RX : 0u));
    d->st.disc_added++;
}

/* LQI 表应答（ZH_ZDO_LQI_TABLE_RSP）。只认当前这台、序号对上的 */
static void disc_on_rsp(zh_dev_core_t *d, uint16_t sender, const uint8_t *pl, size_t len, uint32_t now)
{
    zh_disc_t *s = &d->disc;
    if (s->state != ZH_DISC_RUN || !s->inflight || s->q_i >= s->q_n || len < 2) return;
    if (sender != s->q[s->q_i].nwk || pl[0] != s->zdo_seq) return;   /* 迟到的 / 别人的 */
    s->inflight = false;
    s->t_next = now + ZH_DISC_GAP_MS;
    if (pl[1] != ZH_ZDP_SUCCESS) {                 /* 不支持 / 拒了：跳过这台（zigpy 同） */
        d->st.disc_unsupported++;
        disc_next(s);
        return;
    }
    const unsigned cnt = len >= 5 ? pl[4] : 0;
    if (len < 5 || len < 5u + cnt * ZH_ZDO_LQI_ENTRY_LEN) {
        d->st.disc_bad++;
        disc_next(s);                              /* 坏应答重发也是坏的 */
        return;
    }
    d->st.disc_rsps++;
    const uint8_t depth = s->q[s->q_i].depth;
    for (unsigned i = 0; i < cnt; i++) disc_entry(d, pl + 5u + i * ZH_ZDO_LQI_ENTRY_LEN, depth);
    s->pages++;
    const unsigned next = (unsigned)pl[3] + cnt;
    if (cnt == 0 || next >= pl[2] || next > 0xFFu || s->pages >= ZH_DISC_PAGES_MAX) {
        d->st.disc_routers++;
        disc_next(s);
        return;
    }
    s->start_idx = (uint8_t)next;
    s->tries = 0;
}

static void disc_tick(zh_dev_core_t *d, uint32_t now)
{
    zh_disc_t *s = &d->disc;
    if (s->state == ZH_DISC_DONE) return;
    if (!d->net_up) {                              /* 暂停：在途的那条作废（链路复位时应用命令的结果会被吞掉），不算次数 */
        s->inflight = false;
        if (s->state == ZH_DISC_WAIT) s->state = ZH_DISC_IDLE;   /* 还没开始就掉了：回来后重新等满 60 s（别正好撞上 NCP 复位后那一波，审计 ⚠️） */
        return;
    }
    if (s->state == ZH_DISC_IDLE) {                /* 这次开机网第一次 UP：等开机那一波过去 */
        s->state = ZH_DISC_WAIT;
        s->t_start = now + ZH_DISC_START_MS;
        return;
    }
    if (s->state == ZH_DISC_WAIT) {
        if (!expired(now, s->t_start) || !d->have_xpan) return;   /* 不知道本网的扩展 PAN 就先不开始（过滤不了别的网的条目） */
        disc_seed(d);
        s->state = ZH_DISC_RUN;
        s->t_next = now;
    }
    if (s->inflight) {
        if (!expired(now, s->t_req + ZH_DISC_WAIT_MS)) return;
        d->st.disc_timeouts++;
        disc_try_failed(d, now);
        return;
    }
    if (!expired(now, s->t_next)) return;
    if (s->q_i >= s->q_n) {
        s->state = ZH_DISC_DONE;
        d->st.disc_rounds++;
        return;
    }
    disc_send(d, now);
}

/* ---------------------------------------------------------------- 访谈（8.6；第七步 b，规则见 include/zh_dev.h 文件头） */

/* SWBuildID 的指纹：CRC-32（IEEE 802.3，反射多项式 0xEDB88320）。0 留给"没读到" */
static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1u));
    }
    c = ~c;
    return c ? c : 1u;
}

static bool iv_pending(const zh_dev_t *x) { return x->used && x->iv >= ZH_IV_NODE && x->iv <= ZH_IV_REPORT; }

static void iv_release(zh_dev_core_t *d, unsigned idx)
{
    if (d->iv_busy == idx + 1u) d->iv_busy = 0;   /* 在途那条的结果之后再来也不认：下一条发出时代号会变 */
}

static void iv_set_step(zh_dev_t *x, uint8_t step)
{
    x->iv = step;
    x->iv_tries = 0;
    x->iv_n_seq = 0;
    x->iv_er_sent = false;
}

/* 开始（或重新开始）访谈。is_new：这次开机经入网窗口新加的（访谈完推送）。记录里访谈填的字段先清掉：第七步 a 建的记录 zone_type 是 0
   （0x0000 是合法的 ZoneType），重新配对的设备读不到的属性也不能留着旧值（第二轮审计）。只改内存：盘上的旧结果到访谈完才覆盖 */
static void iv_start(zh_dev_core_t *d, unsigned idx, bool is_new, uint32_t now)
{
    zh_dev_t *x = &d->dev[idx];
    zh_dev_rec_t *r = &x->rec;
    iv_release(d, idx);
    iv_set_step(x, ZH_IV_NODE);
    x->iv_ep_i = x->iv_n_eps = 0;
    x->iv_want = 0;
    x->iv_single = false;
    x->iv_new = is_new;
    x->iv_t_new = now;
    x->iv_t0 = 0;
    x->iv_resume = false;                          /* 整套重新访谈：顺带重写 CIE、重新登记 */
    if (is_new) x->iv_fails = 0;                   /* 重新配对：自己重来的间隔从第一档算 */
    r->flags &= (uint8_t)~ZH_DEVF_CIE_OWED;        /* 欠着的随这一轮重新算（重新配对成没有 IAS Zone 的设备时别留着） */
    r->flags &= (uint8_t)~(ZH_DEVF_NODE_DESC | ZH_DEVF_POWER);
    x->tuya_k = 0;                                 /* 厂商名 / 型号要重读：涂鸦表的查找结果作废 */
    r->manuf_code = 0;
    if (is_new) {                                  /* 重新配对：读不到的不能留着旧值。别的重来（失败重试、离开后回来）不清：小米的型号多半是从它主动上报学来的，
                                                      访谈常常走不完，清了就要等它下次按键才知道是什么（M5 第十步） */
        memset(r->manuf, 0, sizeof(r->manuf));
        memset(r->model, 0, sizeof(r->model));
        x->lumi_k = 0;
    }
    r->sw_build_crc = 0;
    r->n_ep = 0;
    memset(r->ep, 0, sizeof(r->ep));
    r->zone_type = ZH_ZONE_TYPE_UNKNOWN;
}

/* 访谈眼里"随时可以发"的：能力已知且空闲时也收、路由器（主动发现建的记录只知道类型；与可用性 / 控制的判据一致）、
   主动发现时邻居表说它空闲时也收的（访谈完节点描述符就有了 ANNCED，之后走第一条） */
static bool iv_always_on(const zh_dev_t *x)
{
    return (x->rec.flags & (ZH_DEVF_ANNCED | ZH_DEVF_RX_ON)) == (ZH_DEVF_ANNCED | ZH_DEVF_RX_ON) || x->rec.node_type == ZH_NODE_ROUTER ||
           ((x->disc & ZH_DISC_F_RX) && (x->rec.flags & ZH_DEVF_RX_ON));
}

/* 随时听得见的设备第 n 次访谈失败后，过多久自己重来 */
static uint32_t iv_on_retry_gap(uint8_t fails)
{
    return fails <= 1u ? ZH_IV_ON_RETRY1_MS : fails == 2u ? ZH_IV_ON_RETRY2_MS : fails == 3u ? ZH_IV_ON_RETRY3_MS : ZH_IV_RETRY_MS;
}

/* 现在能不能给它发：已知空闲时也收的随时可以；休眠的 / 还不知道的只在它最近一条消息之后 ZH_IV_AWAKE_MS 内（刚发完消息多半在快速轮询） */
static bool iv_can_talk(const zh_dev_t *x, uint32_t now)
{
    if ((x->rec.flags & ZH_DEVF_LEFT) || x->rec.nwk == NWK_COORD || x->rec.nwk >= NWK_RESERVED) return false;   /* 短地址作废（0xFFFF）时发出去就是广播 */
    if (iv_always_on(x)) return true;
    if (x->iv_new && (int32_t)(now - x->iv_t_new) < (int32_t)ZH_IV_JOIN_AWAKE_MS) return true;   /* 刚入网（或刚入网后一直在说话）：还在快速轮询 */
    const int32_t age = (int32_t)(now - x->t_seen);   /* < 0：回调里写的时刻比 tick 的晚，是刚收到 */
    return x->seen && age < (int32_t)ZH_IV_AWAKE_MS;
}

/* 认出来了：访谈结果落盘、出 IDENTIFIED。调用方先把 x->iv 设好（NONE，或要接着做的 CONFIGURE 第二步）：事件之前发布的快照里要是"已访谈完" */
static void iv_identified(zh_dev_core_t *d, unsigned idx)
{
    zh_dev_t *x = &d->dev[idx];
    x->rec.flags |= ZH_DEVF_INTERVIEWED;
    x->iv_fails = 0;                               /* 认出来了：之后再失败，自己重来的间隔从第一档算（iv_done 与 CONFIGURE 那条路都经过这里） */
    x->tuya_k = 0;                                 /* 厂商名 / 型号刚读回：涂鸦表重新查（访谈中途可能按清空的名字查过） */
    x->lumi_k = 0;
    d->st.iv_done++;
    dev_save(d, idx);                              /* 访谈结果一次落盘（结构变化，8.9）；也置 dirty：事件之前快照要能发布 */
    /* 新入网的（或新入网时访谈失败过、后来重试成功的）推送：Muse 之前收到过"认不出来"，成功了也要知道 */
    dev_event(d, ZH_DEV_EV_IDENTIFIED, (x->iv_new || x->iv_owe_push) ? ZH_DEV_SRC_JOIN : ZH_DEV_SRC_PASSIVE, idx);
}

static void iv_done(zh_dev_core_t *d, unsigned idx)
{
    zh_dev_t *x = &d->dev[idx];
    iv_release(d, idx);
    x->iv = ZH_IV_NONE;
    iv_identified(d, idx);
    x->iv_new = false;
    x->iv_owe_push = false;
}

/* CONFIGURE 第二步收尾（配完 / 期限到）：访谈早在 iv_identified 时算完、推送过了，这里不再出事件 */
static void cfg_end(zh_dev_core_t *d, unsigned idx)
{
    zh_dev_t *x = &d->dev[idx];
    iv_release(d, idx);
    x->iv = ZH_IV_NONE;
    x->iv_new = false;
    x->iv_owe_push = false;
    d->dirty = true;
}

static void iv_fail(zh_dev_core_t *d, unsigned idx)
{
    zh_dev_t *x = &d->dev[idx];
    iv_release(d, idx);
    x->iv = ZH_IV_FAILED;                          /* 不落盘：它重新宣告 / 过 ZH_IV_RETRY_MS 再说话 / 重新入网 / 下次开机时重来；随时听得见的还到点自己重来（iv_tick） */
    x->iv_t_fail = d->t_now;
    const bool first = x->iv_fails == 0;           /* 这一轮连着失败的第一次（重新宣告 / 回来 / 重新配对 / 访谈完时清零） */
    if (x->iv_fails < UINT8_MAX) x->iv_fails++;
    d->st.iv_failed++;
    d->dirty = true;                               /* 事件之前置：zhub.c 发事件前发布快照，dirty 没置就发布不出去（修复复核 ❌） */
    if (x->iv_new) x->iv_owe_push = true;          /* 重试成功时还要推一次"认出来了" */
    /* 失败只推第一次（重试的不再是 iv_new）；事件也只记一轮里的第一次：随时听得见的到点自己重来，不在了的设备每小时一条会冲掉 64 条的事件环（审计 ⚠️） */
    if (x->iv_new || first) dev_event(d, ZH_DEV_EV_IV_FAILED, x->iv_new ? ZH_DEV_SRC_JOIN : ZH_DEV_SRC_PASSIVE, idx);
    x->iv_new = false;
}

/* 记录里第一个输入簇含 cl 的端点下标；没有 -1 */
static int ep_with_in(const zh_dev_rec_t *r, uint16_t cl)
{
    for (unsigned k = 0; k < r->n_ep && k < ZH_DEV_EP_REC; k++) {
        const unsigned n_in = r->ep[k].n_in < ZH_DEV_EP_CL ? r->ep[k].n_in : ZH_DEV_EP_CL;
        for (unsigned j = 0; j < n_in; j++)
            if (r->ep[k].cl[j] == cl) return (int)k;
    }
    return -1;
}

static void iv_to_basic(zh_dev_core_t *d, unsigned idx)
{
    zh_dev_t *x = &d->dev[idx];
    if (x->rec.n_ep == 0) {                        /* 一个端点都没有：没有可读的 */
        iv_done(d, idx);
        return;
    }
    iv_set_step(x, ZH_IV_BASIC);
    x->iv_want = ZH_IV_W_MANUF | ZH_IV_W_MODEL | ZH_IV_W_POWER | ZH_IV_W_SWB;
    x->iv_single = false;
}

static void iv_after_basic(zh_dev_core_t *d, unsigned idx)
{
    zh_dev_t *x = &d->dev[idx];
    if (ep_with_in(&x->rec, ZH_CL_IAS_ZONE) < 0) {
        iv_to_config(d, idx);
        return;
    }
    iv_set_step(x, ZH_IV_ZONE);
}

/* CIE / 登记这两步的结局（M5 第四步）：DONE = 登记确认了 / 设备明确拒了（再补也一样）；OWED = 没成，记 ZH_DEVF_CIE_OWED 之后补。
   正常访谈里走到这里就是访谈完（iv_done：落盘、出事件）；补做的那一轮（iv_resume）只改标志，不出事件 */
enum { CIE_DONE, CIE_OWED };
static void cie_finish(zh_dev_core_t *d, unsigned idx, int outcome)
{
    zh_dev_t *x = &d->dev[idx];
    const uint8_t f0 = x->rec.flags;
    if (outcome == CIE_OWED) {
        x->rec.flags |= ZH_DEVF_CIE_OWED;
        x->iv_t_fail = d->t_now;                   /* 距这一刻 ZH_IV_RETRY_MS 后它再说话就补 */
    } else {
        x->rec.flags &= (uint8_t)~ZH_DEVF_CIE_OWED;
    }
    if (!x->iv_resume) {
        iv_to_config(d, idx);                      /* 接着 CONFIGURE 第二步（不配的直接访谈完）；标志随访谈完那一次落盘 */
        return;
    }
    iv_release(d, idx);
    x->iv = ZH_IV_NONE;
    x->iv_resume = false;
    if (x->rec.flags != f0) dev_save(d, idx);
    else d->dirty = true;
}

/* 欠着 CIE / 登记的（ZH_DEVF_CIE_OWED）：只补这两步，从写 CIE 地址开始（不重新访谈、结束时不出事件）。正在访谈 / 补做中、离开了、
   还不知道网关自己的 EUI64、没有 IAS Zone 的不动。触发：重新宣告、加密重新入网、发来登记请求、距上次 ZH_IV_RETRY_MS 后再说话、网关读到自己的 EUI64 */
static void cie_resume(zh_dev_core_t *d, unsigned idx, uint32_t gap_ms)
{
    zh_dev_t *x = &d->dev[idx];
    if (!(x->rec.flags & ZH_DEVF_CIE_OWED) || !(x->rec.flags & ZH_DEVF_INTERVIEWED) || (x->rec.flags & ZH_DEVF_LEFT) || x->iv != ZH_IV_NONE ||
        !d->have_own_eui || ep_with_in(&x->rec, ZH_CL_IAS_ZONE) < 0) return;
    /* 距上次没成还不够久（gap_ms = 0：读到自己的 EUI64，这是头一回能写；iv_t_fail = 0：这次开机还没试过 —— 开机读回的欠着的头一回不等） */
    if (gap_ms && x->iv_t_fail && d->t_now - x->iv_t_fail < gap_ms) return;
    iv_set_step(x, ZH_IV_CIE);
    x->iv_resume = true;
    x->iv_t0 = 0;                                  /* 整体期限从这一轮第一条请求起算 */
    d->st.cie_resumes++;
}

/* CONFIGURE 第二步（设计表 8.6-1）：只对这次开机经入网窗口新加 / 重新配对的（iv_new；新入网时访谈失败过、这回重试的 iv_owe_push）——
   迁移来的设备里早有 ZHA / Z2M 配好的绑定与上报，不改它的上报节奏。计划 = 前 ZH_DEV_EP_REC 个端点里输入簇在 k_cfg 的
   （电源配置只给非路由器：市电路由器也常带这个簇），最多 ZH_CFG_MAX 条。期限已过 / 不用配：直接访谈完 */
static int lumi_of(zh_dev_t *x);

static void iv_to_config(zh_dev_core_t *d, unsigned idx)
{
    zh_dev_t *x = &d->dev[idx];
    x->cfg_i = x->cfg_n = 0;
    if (!(x->iv_new || x->iv_owe_push)) {
        iv_done(d, idx);
        return;
    }
    unsigned over = 0;
    for (unsigned k = 0; k < x->rec.n_ep && k < ZH_DEV_EP_REC; k++) {
        const struct zh_dev_ep *e = &x->rec.ep[k];
        const unsigned n_in = e->n_in < ZH_DEV_EP_CL ? e->n_in : ZH_DEV_EP_CL;
        for (unsigned r = 0; r < K_CFG_N; r++) {
            if (k_cfg[r].cl == ZH_CL_POWER_CFG && x->rec.node_type == ZH_NODE_ROUTER) continue;
            for (unsigned j = 0; j < n_in; j++) {
                if (e->cl[j] != k_cfg[r].cl) continue;
                if (x->cfg_n < ZH_CFG_MAX) {
                    x->cfg[x->cfg_n][0] = (uint8_t)k;
                    x->cfg[x->cfg_n][1] = (uint8_t)r;
                    x->cfg_n++;
                } else {
                    over++;
                }
                break;
            }
        }
    }
    d->st.cfg_skipped += over;                     /* 超出 ZH_CFG_MAX 的不配（多端点的大设备） */
    if (!x->cfg_n) {
        iv_done(d, idx);
        return;
    }
    /* 小米 / Aqara 的电池设备（型号在小米表里、不是路由器）：不接受标准的上报配置，按自己的节奏报（0xFF01 心跳 + 变化时上报）；
       2026-10-23 上板 lumi.weather 三条上报配置全回失败。Z2M 的小米电池设备也不配。绑定对它也没用 —— 一并跳过，计 cfg_skipped */
    if (x->rec.node_type != ZH_NODE_ROUTER && lumi_of(x) >= 0) {
        d->st.cfg_skipped += x->cfg_n;
        iv_done(d, idx);
        return;
    }
    /* 绑定的目标就是网关自己的 EUI64：不知道就配不了；30 min 期限已过（IAS 那几步拖到了期限）：不配了。尽力而为，都不补 */
    if (!d->have_own_eui || (x->iv_t0 && d->t_now - x->iv_t0 > ZH_IV_TOTAL_MS)) {
        d->st.cfg_skipped += x->cfg_n;
        iv_done(d, idx);
        return;
    }
    /* 先算访谈完、推送"认出来了"，再配：配置是不出事件的后续阶段 —— 别让几台休眠设备同时入网时 Muse 等半小时才收到"已入网"（第二轮审计 ❌） */
    iv_release(d, idx);
    iv_set_step(x, ZH_IV_BIND);
    iv_identified(d, idx);
}

/* 这一条配完了（成没成都算）：下一条，或者收尾 */
static void iv_cfg_next(zh_dev_core_t *d, unsigned idx)
{
    zh_dev_t *x = &d->dev[idx];
    iv_release(d, idx);
    if (++x->cfg_i >= x->cfg_n) {
        cfg_end(d, idx);
        return;
    }
    iv_set_step(x, ZH_IV_BIND);
}

/* ZoneType 读完（或读不到）之后：写 CIE 地址（CONFIGURE 第一步，设计 8.6 IAS 第 1 条）。还不知道网关自己的 EUI64：记欠着（读到后补），照常算访谈完 */
static void iv_after_zone(zh_dev_core_t *d, unsigned idx)
{
    zh_dev_t *x = &d->dev[idx];
    if (!d->have_own_eui) {
        d->st.cie_skipped++;
        cie_finish(d, idx, CIE_OWED);
        return;
    }
    iv_set_step(x, ZH_IV_CIE);
}

/* 这一步 ZH_IV_TRIES 次都没成 */
static void iv_step_failed(zh_dev_core_t *d, unsigned idx)
{
    zh_dev_t *x = &d->dev[idx];
    iv_release(d, idx);
    switch (x->iv) {
    case ZH_IV_BASIC:
        if (!x->iv_single) {                       /* 两个一组都没回：有的设备一次只认一个属性（zigpy endpoint.py:188-197），再来一轮单读 */
            x->iv_single = true;
            x->iv_tries = 0;
            x->iv_n_seq = 0;
        } else if (!(x->iv_want & (ZH_IV_W_MANUF | ZH_IV_W_MODEL))) {
            iv_after_basic(d, idx);                /* 厂商 / 型号有了：供电、软件版本读不到不要紧 */
        } else {
            iv_fail(d, idx);
        }
        break;
    case ZH_IV_ZONE:
        iv_after_zone(d, idx);                     /* ZoneType 读不到：留 ZH_ZONE_TYPE_UNKNOWN，不算访谈失败；CIE 地址照样要写 */
        break;
    case ZH_IV_CIE:
        d->st.cie_fail++;                          /* 几次都没回：不算访谈失败，记欠着（重新宣告 / 重新入网 / 登记请求 / 一小时后再说话时补） */
        cie_finish(d, idx, CIE_OWED);
        break;
    case ZH_IV_ENROLL:
        d->st.enroll_unconfirmed++;                /* 回读几次都不是"已登记"：记欠着，之后连写 CIE 一起补 */
        cie_finish(d, idx, CIE_OWED);
        break;
    case ZH_IV_BIND:
        d->st.bind_fail++;                         /* 绑定几次都没回：照样配上报（有的设备不看绑定表、直接报给协调器） */
        iv_set_step(x, ZH_IV_REPORT);
        break;
    case ZH_IV_REPORT:
        d->st.report_fail++;                       /* 尽力而为：下一条 */
        iv_cfg_next(d, idx);
        break;
    default:
        iv_fail(d, idx);
        break;
    }
}

/* 发这一步的下一条请求。false = 现在交不出去（应用命令队列满），这一拍不再发 */
static bool iv_issue(zh_dev_core_t *d, unsigned idx, uint32_t now)
{
    zh_dev_t *x = &d->dev[idx];
    uint8_t pl[32];                                /* 最长的是绑定请求 22 B */
    size_t n = 0;
    uint16_t profile = 0, cluster = 0;
    uint8_t dst_ep = 0, seq = 0;
    switch (x->iv) {
    case ZH_IV_NODE:
    case ZH_IV_ACTIVE:
    case ZH_IV_SIMPLE:
        /* ZDO：[序号][nwk:2]，Simple 再加 [端点]（zigbee-device-stack.h:313-314、330） */
        seq = d->ops.zdo_seq ? d->ops.zdo_seq(d->ops.ctx) : 0;
        pl[0] = seq;
        pl[1] = (uint8_t)x->rec.nwk;
        pl[2] = (uint8_t)(x->rec.nwk >> 8);
        n = 3;
        cluster = x->iv == ZH_IV_NODE ? ZH_ZDO_NODE_DESC_REQ : x->iv == ZH_IV_ACTIVE ? ZH_ZDO_ACTIVE_EP_REQ : ZH_ZDO_SIMPLE_DESC_REQ;
        if (x->iv == ZH_IV_SIMPLE) pl[n++] = x->iv_eps[x->iv_ep_i];
        break;
    case ZH_IV_BASIC:
    case ZH_IV_ZONE: {
        uint16_t ids[2];
        size_t k = 0;
        int e;
        if (x->iv == ZH_IV_BASIC) {
            static const struct { uint8_t bit; uint16_t id; } order[] = {
                { ZH_IV_W_MANUF, ZH_BASIC_MANUF }, { ZH_IV_W_MODEL, ZH_BASIC_MODEL }, { ZH_IV_W_POWER, ZH_BASIC_POWER }, { ZH_IV_W_SWB, ZH_BASIC_SW_BUILD } };
            const size_t max = x->iv_single ? 1u : 2u;
            x->iv_req = 0;
            for (size_t j = 0; j < 4 && k < max; j++)
                if (x->iv_want & order[j].bit) {
                    ids[k++] = order[j].id;
                    x->iv_req |= order[j].bit;
                }
            e = ep_with_in(&x->rec, ZH_CL_BASIC);
            if (e < 0) e = 0;                      /* 没登记 Basic 的也问第一个端点（Basic 是必备簇） */
            cluster = ZH_CL_BASIC;
        } else {
            ids[k++] = ZH_IAS_ZONE_TYPE;
            ids[k++] = ZH_IAS_ZONE_STATUS;         /* 顺带读当前状态：迁移来的设备访谈时正在报警也认得出来（第二轮审计） */
            e = ep_with_in(&x->rec, ZH_CL_IAS_ZONE);
            cluster = ZH_CL_IAS_ZONE;
        }
        if (k == 0 || e < 0) return true;          /* 不会发生（want 空时已转下一步）：别卡住调度 */
        dst_ep = x->rec.ep[e].ep;
        profile = ZH_HA_PROFILE;                   /* 取网关自己源端点的 profile（SDK af-common.c:1194-1199 同；zigpy 也把 ZLL 改成 HA）：对方报 0xC05E 的老 ZLL 设备也认 HA（第一轮审计 ❌） */
        seq = d->zcl_seq++;
        n = zh_zcl_enc_read_attrs(pl, sizeof(pl), seq, ids, k);
        break;
    }
    case ZH_IV_CIE: {
        /* 写 IAS_CIE_ADDRESS = 网关自己的 EUI64（SDK ias-zone-client.c:395-404 的 setCieAddress：全局写属性、类型 ieee_address、小端） */
        const int e = ep_with_in(&x->rec, ZH_CL_IAS_ZONE);
        if (e < 0 || !d->have_own_eui) {           /* 不会发生（进这一步前判过）：别卡住调度 */
            cie_finish(d, idx, e < 0 ? CIE_DONE : CIE_OWED);
            return true;
        }
        dst_ep = x->rec.ep[e].ep;
        profile = ZH_HA_PROFILE;
        cluster = ZH_CL_IAS_ZONE;
        seq = d->zcl_seq++;
        n = zh_zcl_enc_write_eui64(pl, sizeof(pl), seq, ZH_IAS_CIE_ADDR, d->own_eui);
        break;
    }
    case ZH_IV_BIND: {
        /* 绑定请求（ZDO，zigbee-device-stack.h:451-458）：[seq][源 = 设备 EUI64][源端点][簇][0x03 单播][目标 = 网关 EUI64][目标端点 = 网关的源端点] */
        const struct zh_dev_ep *e = &x->rec.ep[x->cfg[x->cfg_i][0]];
        const uint16_t cl = k_cfg[x->cfg[x->cfg_i][1]].cl;
        seq = d->ops.zdo_seq ? d->ops.zdo_seq(d->ops.ctx) : 0;
        pl[0] = seq;
        memcpy(pl + 1, x->rec.ieee, 8);
        pl[9] = e->ep;
        pl[10] = (uint8_t)cl;
        pl[11] = (uint8_t)(cl >> 8);
        pl[12] = ZH_ZDO_UNICAST_BINDING;
        memcpy(pl + 13, d->own_eui, 8);
        pl[21] = ZH_DEV_SRC_EP;
        n = 22;
        cluster = ZH_ZDO_BIND_REQ;
        break;
    }
    case ZH_IV_REPORT: {
        const struct zh_dev_ep *e = &x->rec.ep[x->cfg[x->cfg_i][0]];
        const cfg_row_t *c = &k_cfg[x->cfg[x->cfg_i][1]];
        dst_ep = e->ep;
        profile = ZH_HA_PROFILE;
        cluster = c->cl;
        seq = d->zcl_seq++;
        n = zh_zcl_enc_cfg_report(pl, sizeof(pl), seq, c->attr, c->type, c->min_s, c->max_s, c->chg, c->chg_len);
        break;
    }
    case ZH_IV_ENROLL: {
        /* 自动登记（设计 8.6 IAS 第 2 条后半）：先发登记应答，紧跟着回读 ZoneState —— 应答本身没有回音，读到"已登记"才知道它收到了 */
        const int e = ep_with_in(&x->rec, ZH_CL_IAS_ZONE);
        if (e < 0) {
            cie_finish(d, idx, CIE_DONE);
            return true;
        }
        dst_ep = x->rec.ep[e].ep;
        if (!x->iv_er_sent) {                      /* 上一拍登记应答交出去了、回读没交出去：这一拍只补回读 */
            if (!ias_enroll_rsp(d, idx, dst_ep)) return false;   /* 交不出去：这一拍不发回读，下一拍连同登记应答一起再来（不占次数） */
            x->iv_er_sent = true;
        }
        const uint16_t id = ZH_IAS_ZONE_STATE;
        profile = ZH_HA_PROFILE;
        cluster = ZH_CL_IAS_ZONE;
        seq = d->zcl_seq++;
        n = zh_zcl_enc_read_attrs(pl, sizeof(pl), seq, &id, 1);
        break;
    }
    default:
        return true;
    }
    const zh_ezsp_aps_t aps = { .profile_id = profile, .cluster_id = cluster, .src_ep = profile ? ZH_DEV_SRC_EP : 0, .dst_ep = dst_ep,
                                .options = ZH_APS_OPTS_DEFAULT, .group_id = 0, .sequence = 0 };
    uint8_t b[64];
    const size_t len = n ? zh_ezsp_enc_send_unicast(b, sizeof(b), 0, ZH_OUTGOING_DIRECT, x->rec.nwk, &aps, seq, (uint8_t)n, pl) : 0;
    const uint16_t gen = (uint16_t)(d->iv_gen + 1u);
    if (!len || !d->ops.send || !d->ops.send(d->ops.ctx, b, len, ZH_DEV_TAG_KIND_IV | ((uint32_t)gen << 8), false)) return false;
    d->iv_gen = gen;
    x->iv_er_sent = false;                         /* ENROLL：这一次的回读也交出去了，下一次连登记应答一起重发 */
    x->iv_tries++;
    if (x->iv_n_seq < ZH_IV_TRIES) x->iv_seq[x->iv_n_seq++] = seq;
    else {                                         /* 单读时进度会把次数清零：只留最近三条的序号 */
        memmove(x->iv_seq, x->iv_seq + 1, ZH_IV_TRIES - 1u);
        x->iv_seq[ZH_IV_TRIES - 1u] = seq;
    }
    if (!x->iv_t0) {
        x->iv_t0 = now ? now : 1u;
        d->dirty = true;                           /* 列表里的访谈状态从"等着"变"进行中" */
    }
    d->iv_busy = (uint8_t)(idx + 1u);
    d->t_iv_req = now;
    d->st.iv_reqs++;
    return true;
}

static bool iv_seq_ok(const zh_dev_t *x, uint8_t seq)
{
    for (unsigned k = 0; k < x->iv_n_seq; k++)
        if (x->iv_seq[k] == seq) return true;
    return false;
}

/* 应答对上了：在途那条完了。没进展、次数又用完了就算这一步失败 */
static void iv_no_progress(zh_dev_core_t *d, unsigned idx)
{
    iv_release(d, idx);
    if (d->dev[idx].iv_tries >= ZH_IV_TRIES) iv_step_failed(d, idx);
}

/* 访谈的三种 ZDO 应答。true = 是访谈的应答（不管对没对上） */
static bool iv_on_zdo(zh_dev_core_t *d, uint16_t sender, uint16_t cluster, const uint8_t *pl, size_t len)
{
    if (cluster == ZH_ZDO_BIND_RSP) {              /* [序号][状态]：没有 nwk，按发送方找（绑定表在设备自己身上，父节点不代答） */
        const int bi = zh_dev_find_nwk(d, sender);
        if (bi < 0) return true;
        zh_dev_t *bx = &d->dev[bi];
        if (len < 2) {
            d->st.iv_bad++;
            return true;
        }
        if (bx->iv != ZH_IV_BIND || !iv_seq_ok(bx, pl[0])) return true;
        if (pl[1] == ZH_ZDP_SUCCESS) d->st.bind_ok++;
        else d->st.bind_fail++;                    /* 表满 / 不支持：照样配上报（有的设备不看绑定表、直接报给协调器） */
        iv_release(d, (unsigned)bi);
        iv_set_step(bx, ZH_IV_REPORT);
        return true;
    }
    uint8_t want;
    if (cluster == ZH_ZDO_NODE_DESC_RSP) want = ZH_IV_NODE;
    else if (cluster == ZH_ZDO_ACTIVE_EP_RSP) want = ZH_IV_ACTIVE;
    else if (cluster == ZH_ZDO_SIMPLE_DESC_RSP) want = ZH_IV_SIMPLE;
    else return false;
    if (len < 4) {                                 /* [序号][状态][nwk:2] 都不够 */
        d->st.iv_bad++;
        return true;
    }
    const uint8_t seq = pl[0], status = pl[1];
    const uint16_t nwk = (uint16_t)(pl[2] | (pl[3] << 8));   /* 应答里的 nwk 是被问的那台（休眠设备可能由父节点代答，所以不看发送方） */
    const int i = zh_dev_find_nwk(d, nwk);
    if (i < 0) return true;
    const unsigned idx = (unsigned)i;
    zh_dev_t *x = &d->dev[idx];
    if (x->iv != want || !iv_seq_ok(x, seq)) return true;   /* 不是在等的（重复的 / 别的主机问的）：不理 */
    if (want == ZH_IV_NODE) {
        if (status != ZH_ZDP_SUCCESS) {            /* 节点描述符只是补充信息：拿不到也接着问端点 */
            iv_release(d, idx);
            iv_set_step(x, ZH_IV_ACTIVE);
            return true;
        }
        if (len < ZH_ZDO_NODE_RSP_MIN) {
            d->st.iv_bad++;
            iv_no_progress(d, idx);
            return true;
        }
        const uint8_t logical = pl[4] & 0x07u, cap = pl[6];
        if (logical <= 1u) x->rec.node_type = ZH_NODE_ROUTER;   /* 0 协调器（不会是它）/ 1 路由器 */
        else if (logical == 2u) x->rec.node_type = ZH_NODE_END_DEVICE;
        x->rec.manuf_code = (uint16_t)(pl[7] | (pl[8] << 8));
        x->rec.flags = (uint8_t)((x->rec.flags & ~(ZH_DEVF_RX_ON | ZH_DEVF_MAINS)) | ZH_DEVF_ANNCED | ZH_DEVF_NODE_DESC | ((cap & ZH_CAP_RX_ON_IDLE) ? ZH_DEVF_RX_ON : 0) |
                                 ((cap & ZH_CAP_MAINS) ? ZH_DEVF_MAINS : 0));
        iv_release(d, idx);
        iv_set_step(x, ZH_IV_ACTIVE);
        return true;
    }
    if (want == ZH_IV_ACTIVE) {
        if (status != ZH_ZDP_SUCCESS) {            /* 不告诉端点就什么都问不了 */
            iv_no_progress(d, idx);
            return true;
        }
        if (len < 5 || len < 5u + pl[4]) {
            d->st.iv_bad++;
            iv_no_progress(d, idx);
            return true;
        }
        x->iv_n_eps = 0;
        for (unsigned k = 0; k < pl[4] && x->iv_n_eps < ZH_DEV_EP_REC; k++) {
            const uint8_t ep = pl[5 + k];
            if (ep >= 1u && ep <= 240u) x->iv_eps[x->iv_n_eps++] = ep;   /* 0 是 ZDO、241–254 保留（242 是 Green Power）、255 广播 */
        }
        x->rec.n_ep = 0;
        memset(x->rec.ep, 0, sizeof(x->rec.ep));
        iv_release(d, idx);
        if (x->iv_n_eps == 0) {
            iv_to_basic(d, idx);                   /* n_ep 为 0：直接完成 */
            return true;
        }
        iv_set_step(x, ZH_IV_SIMPLE);
        x->iv_ep_i = 0;
        return true;
    }
    /* SIMPLE：[序号][状态][nwk:2][长度][端点][profile:2][devid:2][版本][输入数][输入簇:2×n][输出数][输出簇:2×n]（zigbee-device-stack.h:315-320） */
    if (status == ZH_ZDP_SUCCESS) {
        const size_t dl = len >= 5 ? pl[4] : 0;
        const uint8_t *p = pl + 5;
        bool ok = len >= 5 && dl >= 8 && len >= 5 + dl && p[0] == x->iv_eps[x->iv_ep_i];
        size_t n_in = 0, n_out = 0;
        if (ok) {
            n_in = p[6];
            ok = 7 + 2 * n_in + 1 <= dl;           /* 输入簇之后还得有输出数那一个字节 */
        }
        if (ok) {
            n_out = p[7 + 2 * n_in];
            ok = 8 + 2 * n_in + 2 * n_out <= dl;
        }
        if (!ok) {
            d->st.iv_bad++;
            iv_no_progress(d, idx);
            return true;
        }
        if (x->rec.n_ep < ZH_DEV_EP_REC) {
            struct zh_dev_ep *e = &x->rec.ep[x->rec.n_ep++];
            e->ep = p[0];
            e->profile = (uint16_t)(p[1] | (p[2] << 8));
            e->devid = (uint16_t)(p[3] | (p[4] << 8));
            e->n_in = (uint8_t)n_in;
            e->n_out = (uint8_t)n_out;
            unsigned c = 0;
            for (size_t j = 0; j < n_in && c < ZH_DEV_EP_CL; j++) e->cl[c++] = (uint16_t)(p[7 + 2 * j] | (p[8 + 2 * j] << 8));
            for (size_t j = 0; j < n_out && c < ZH_DEV_EP_CL; j++) e->cl[c++] = (uint16_t)(p[8 + 2 * n_in + 2 * j] | (p[9 + 2 * n_in + 2 * j] << 8));
        }
    }
    /* 状态不是 0（端点不活跃之类）：这个端点跳过 */
    iv_release(d, idx);
    if (x->iv_ep_i + 1u < x->iv_n_eps) {
        x->iv_ep_i++;
        iv_set_step(x, ZH_IV_SIMPLE);
    } else {
        iv_to_basic(d, idx);
    }
    return true;
}

/* Basic 属性号 → want 位 */
static uint8_t basic_bit(uint16_t id)
{
    switch (id) {
    case ZH_BASIC_MANUF:    return ZH_IV_W_MANUF;
    case ZH_BASIC_MODEL:    return ZH_IV_W_MODEL;
    case ZH_BASIC_POWER:    return ZH_IV_W_POWER;
    case ZH_BASIC_SW_BUILD: return ZH_IV_W_SWB;
    default:                return 0;
    }
}

static void copy_str(char out[24], const uint8_t *s, size_t n)
{
    size_t k = 0;
    while (k < n && k < 23 && s[k] != 0) {         /* 原样存（不可信，输出时净化）；遇 0 截断，最多 23 B */
        out[k] = (char)s[k];
        k++;
    }
    out[k] = '\0';
}

/* 访谈的 ZCL 应答（读属性应答 / 默认应答） */
static void iv_on_zcl(zh_dev_core_t *d, uint16_t sender, uint16_t cluster, const uint8_t *pl, size_t len)
{
    const int i = zh_dev_find_nwk(d, sender);
    if (i < 0) return;
    const unsigned idx = (unsigned)i;
    zh_dev_t *x = &d->dev[idx];
    if (!((x->iv == ZH_IV_BASIC && cluster == ZH_CL_BASIC) || ((x->iv == ZH_IV_ZONE || x->iv == ZH_IV_CIE || x->iv == ZH_IV_ENROLL) && cluster == ZH_CL_IAS_ZONE) ||
          (x->iv == ZH_IV_REPORT && cluster == k_cfg[x->cfg[x->cfg_i][1]].cl))) return;
    zh_zcl_frame_t f;
    if (zh_zcl_parse(pl, len, &f) != ZH_ZCL_OK) {
        d->st.iv_bad++;
        return;
    }
    if ((f.fc & ZH_ZCL_FC_TYPE_MASK) != 0 || !(f.fc & ZH_ZCL_FC_S2C) || (f.fc & ZH_ZCL_FC_MFG) || !iv_seq_ok(x, f.seq)) return;
    if (x->iv == ZH_IV_REPORT) {
        /* 上报配置应答：全成 = 一个 SUCCESS 字节，否则只列失败的（reporting.c:531-549；一条请求只配一个属性，看第一个字节就够）；或者被默认应答拒了 */
        bool ok;
        if (f.cmd == ZH_ZCL_CMD_CFG_REPORT_RSP && f.n >= 1) ok = f.pl[0] == ZH_ZCL_ST_SUCCESS;
        else if (f.cmd == ZH_ZCL_CMD_DEFAULT_RSP && f.n >= 2 && f.pl[0] == ZH_ZCL_CMD_CFG_REPORT && f.pl[1] != ZH_ZCL_ST_SUCCESS) ok = false;
        else return;
        if (ok) d->st.report_ok++;
        else d->st.report_fail++;                  /* 尽力而为：下一条 */
        iv_cfg_next(d, idx);
        return;
    }
    if (x->iv == ZH_IV_CIE) {
        /* 写属性应答：全成 = 一个 SUCCESS 字节，否则只列失败的（SDK ias-zone-client.c:513-516 只看第一个字节）；或者被默认应答拒了 */
        bool ok;
        if (f.cmd == ZH_ZCL_CMD_WRITE_ATTR_RSP && f.n >= 1) ok = f.pl[0] == ZH_ZCL_ST_SUCCESS;
        else if (f.cmd == ZH_ZCL_CMD_DEFAULT_RSP && f.n >= 2 && f.pl[0] == ZH_ZCL_CMD_WRITE_ATTR && f.pl[1] != ZH_ZCL_ST_SUCCESS) ok = false;
        else return;
        iv_release(d, idx);
        if (ok) {
            d->st.cie_ok++;
            iv_set_step(x, ZH_IV_ENROLL);          /* 下一拍发登记应答 + 回读 ZoneState（登记应答不占 10 s 间隔：auto-enroll-request 的设备紧接着发的登记请求照样回） */
        } else {
            d->st.cie_fail++;                      /* 设备明确拒了（只读 / 不认）：补也一样，不记欠着；不算访谈失败 */
            cie_finish(d, idx, CIE_DONE);
        }
        return;
    }
    if (f.cmd == ZH_ZCL_CMD_DEFAULT_RSP) {         /* [命令号][状态]：设备拒了这次读（不支持这个簇之类） */
        if (f.n < 2 || f.pl[0] != ZH_ZCL_CMD_READ_ATTR || f.pl[1] == ZH_ZCL_ST_SUCCESS) return;
        const bool latest = x->iv_n_seq && f.seq == x->iv_seq[x->iv_n_seq - 1];
        if (x->iv_single && !latest) return;       /* 单读阶段迟到的旧拒绝：在途那条还在等，别放掉它（不然同时两条在途，修复复核） */
        iv_release(d, idx);
        if (x->iv == ZH_IV_ZONE) iv_after_zone(d, idx);
        else if (x->iv == ZH_IV_ENROLL) {          /* 读不了 ZoneState：确认不了，登记应答已经发了 —— 不再补（补也读不了） */
            d->st.enroll_unconfirmed++;
            cie_finish(d, idx, CIE_DONE);
        } else if (!x->iv_single) {
            x->iv_single = true;                   /* 改单读：次数重新算（第一轮审计） */
            x->iv_tries = 0;
            x->iv_n_seq = 0;
        } else {                                   /* 单读、最近一条的拒绝（上面已挡掉迟到的） */
            x->iv_want &= (uint8_t)~x->iv_req;     /* 单读也被拒：放弃这一个 */
            x->iv_tries = 0;
            if (!x->iv_want) iv_after_basic(d, idx);
        }
        return;
    }
    if (f.cmd != ZH_ZCL_CMD_READ_ATTR_RSP) return;
    zh_zcl_iter_t it;
    zh_zcl_attr_t a;
    zh_zcl_iter_init(&it, &f);
    int r;
    if (x->iv == ZH_IV_ENROLL) {
        int state = -1;                            /* -1 没回这个属性；-2 设备没有这个属性 */
        while ((r = zh_zcl_next_rsp_rec(&it, &a)) == 1) {
            uint32_t v;
            if (a.id != ZH_IAS_ZONE_STATE) continue;
            /* 规定是 ENUM8；SDK 客户端（ias-zone-client.c:541）干脆跳过类型字节 —— 一字节的整数类型都认，别让类型报错的设备永远确认不了（修复复核） */
            if (a.status == ZH_ZCL_ST_SUCCESS && (a.type == ZH_ZCL_T_ENUM8 || a.type == ZH_ZCL_T_UINT8 || a.type == ZH_ZCL_T_BITMAP8) && zh_zcl_attr_uint(&a, &v))
                state = (int)v;
            else if (a.status == ZH_ZCL_ST_UNSUP_ATTR) state = -2;
        }
        if (r < 0) d->st.iv_bad++;
        if (state == ZH_IAS_STATE_ENROLLED) {
            d->st.enroll_ok++;
            cie_finish(d, idx, CIE_DONE);
        } else if (state == -2) {                  /* 没有 ZoneState（必备属性，不该发生）：确认不了，不再补 */
            d->st.enroll_unconfirmed++;
            cie_finish(d, idx, CIE_DONE);
        } else {
            iv_no_progress(d, idx);                /* 还没登记上（应答可能还在路上）：次数用完前下一次连登记应答一起重发 */
        }
        return;
    }
    if (x->iv == ZH_IV_ZONE) {
        bool got = false;
        while ((r = zh_zcl_next_rsp_rec(&it, &a)) == 1) {
            uint32_t v;
            if (a.id != ZH_IAS_ZONE_TYPE) continue;
            if (a.status == ZH_ZCL_ST_SUCCESS && a.type == ZH_ZCL_T_ENUM16 && zh_zcl_attr_uint(&a, &v)) x->rec.zone_type = (uint16_t)v;
            got = a.status == ZH_ZCL_ST_SUCCESS || a.status == ZH_ZCL_ST_UNSUP_ATTR;
        }
        if (r < 0) d->st.iv_bad++;
        if (got) {
            iv_release(d, idx);
            iv_after_zone(d, idx);
        } else {
            iv_no_progress(d, idx);
        }
        return;
    }
    uint8_t seen = 0, cleared = 0;
    while ((r = zh_zcl_next_rsp_rec(&it, &a)) == 1) {
        const uint8_t bit = basic_bit(a.id);
        if (!bit) continue;
        seen |= bit;
        if (!(x->iv_want & bit)) continue;
        if (a.status == ZH_ZCL_ST_SUCCESS) {
            const uint8_t *s;
            size_t sl;
            uint32_t v;
            if (bit == ZH_IV_W_MANUF && zh_zcl_attr_str(&a, &s, &sl)) copy_str(x->rec.manuf, s, sl);
            else if (bit == ZH_IV_W_MODEL && zh_zcl_attr_str(&a, &s, &sl)) copy_str(x->rec.model, s, sl);
            else if (bit == ZH_IV_W_SWB && zh_zcl_attr_str(&a, &s, &sl)) x->rec.sw_build_crc = crc32_of(s, sl);
            else if (bit == ZH_IV_W_POWER && zh_zcl_attr_uint(&a, &v)) {
                const uint32_t src = v & 0x7Fu;    /* 位 7 = 另有备用电池（types.xml:264-272 的取值） */
                if (src == 1 || src == 2 || src == 4 || src == 5 || src == 6) x->rec.flags |= ZH_DEVF_MAINS | ZH_DEVF_POWER;
                else if (src == 3) x->rec.flags = (uint8_t)((x->rec.flags & ~ZH_DEVF_MAINS) | ZH_DEVF_POWER);
            }
            x->iv_want &= (uint8_t)~bit;           /* 类型不对也不再问：问几次都一样 */
            cleared |= bit;
        } else if (a.status == ZH_ZCL_ST_UNSUP_ATTR) {
            x->iv_want &= (uint8_t)~bit;           /* 没有这个属性：不是错 */
            cleared |= bit;
        } else {
            x->iv_single = true;                   /* 0x89 装不下等：拆开一个一个问 */
        }
    }
    if (r < 0) {
        d->st.iv_bad++;
        x->iv_single = true;
    }
    if (x->iv_want & x->iv_req & (uint8_t)~seen) x->iv_single = true;   /* 问了的没回（应答方装不下就停在那里，attribute-table.c:587-590） */
    iv_release(d, idx);
    if (!x->iv_want) {
        iv_after_basic(d, idx);
        return;
    }
    if (cleared) {
        x->iv_tries = 0;                           /* 有进展：次数重新算 */
        return;
    }
    if (x->iv_tries >= ZH_IV_TRIES) iv_step_failed(d, idx);
}

static void iv_tick(zh_dev_core_t *d, uint32_t now)
{
    if (d->iv_busy) {
        const unsigned i = d->iv_busy - 1u;
        if ((int32_t)(now - d->t_iv_req) >= (int32_t)ZH_IV_STEP_TIMEOUT_MS) {
            d->iv_busy = 0;
            d->st.iv_timeouts++;
            if (iv_pending(&d->dev[i]) && d->dev[i].iv_tries >= ZH_IV_TRIES) iv_step_failed(d, i);
        }
    }
    for (unsigned i = 0; i < ZH_DEV_MAX; i++) {    /* 整体期限：从第一条请求起 */
        zh_dev_t *x = &d->dev[i];
        if (!iv_pending(x) || !x->iv_t0 || now - x->iv_t0 <= ZH_IV_TOTAL_MS) continue;
        if (x->iv == ZH_IV_BIND || x->iv == ZH_IV_REPORT) {   /* 配到一半期限到了：剩下的不配（访谈早就算完了，尽力而为） */
            d->st.cfg_skipped += (uint32_t)(x->cfg_n - x->cfg_i);
            cfg_end(d, i);
        } else if (x->iv >= ZH_IV_ZONE) {                 /* 厂家 / 型号已经有了：不算访谈失败（第二轮审计）；CIE / 登记记欠着之后补 */
            if (x->iv == ZH_IV_ENROLL) d->st.enroll_unconfirmed++;
            else d->st.cie_fail++;                 /* ZONE / CIE：CIE 地址没写成（ZONE 时还没开始写） */
            cie_finish(d, i, CIE_OWED);
        } else {
            iv_fail(d, i);
        }
    }
    if (d->iv_busy || !d->net_up) return;          /* 一条在途；网络不是 UP 时发出去也只是白白用掉次数 */
    /* 访谈失败、随时听得见的：不等它说话，到点自己重来（间隔见 ZH_IV_ON_RETRY1_MS 的说明）。一拍最多重开一台：重开的这一拍接着在下面发第一条 */
    for (unsigned i = 0; i < ZH_DEV_MAX; i++) {
        zh_dev_t *x = &d->dev[i];
        if (!x->used || x->iv != ZH_IV_FAILED || !iv_always_on(x) || !iv_can_talk(x, now)) continue;
        if (now - x->iv_t_fail < iv_on_retry_gap(x->iv_fails)) continue;   /* 无符号差：iv_t_fail 不在开机时间戳的封顶里，有符号差过 24.8 天会变号（审计 ⚠️） */
        if (x->iv_fails > 3u && !x->seen && (x->disc & ZH_DISC_F_NEW)) continue;   /* 主动发现建的、这次开机一次都没听到过（表里的旧条目）：前三档试过就不再每小时问 */
        d->st.iv_retries++;
        iv_start(d, i, false, now);
        break;
    }
    /* 两遍：先给醒着的休眠设备（它只在刚说过话的几秒里听得见，错过了要等它下一条消息），再轮空闲时也收的；每遍从上次那台的下一台起 */
    for (int pass = 0; pass < 2; pass++) {
        for (unsigned k = 0; k < ZH_DEV_MAX; k++) {
            const unsigned i = (d->iv_rr + k) % ZH_DEV_MAX;
            zh_dev_t *x = &d->dev[i];
            if (!iv_pending(x) || !iv_can_talk(x, now)) continue;
            const bool rx_on = iv_always_on(x);    /* 与 iv_can_talk 同一个判据（审计 ⚠️：发现来的路由器别和刚醒的休眠设备抢第 0 遍） */
            if (pass == 0 && rx_on) continue;
            if (iv_issue(d, i, now)) d->iv_rr = (uint8_t)((i + 1u) % ZH_DEV_MAX);
            return;                                /* 发出去了（只许一条在途），或队列满（这一拍不再试） */
        }
    }
}

/* ---------------------------------------------------------------- 读数（M5 第一步：只读；规则见 include/zh_dev.h 文件头） */

static void st_set(zh_dev_t *x, unsigned field, uint32_t now)
{
    x->st.have = (uint16_t)(x->st.have | (1u << field));
    x->st.t[field] = now;
}

/* 报警类读数翻转：唯一发 ZH_DEV_EV_STATE 的地方 */
static void st_emit(zh_dev_core_t *d, unsigned idx, uint8_t ev)
{
    if (!ev) return;
    d->dev[idx].st_ev = ev;
    d->dirty = true;                               /* 事件之前置：zhub.c 发事件前发布快照 */
    dev_event(d, ZH_DEV_EV_STATE, ZH_DEV_SRC_PASSIVE, idx);
}

/* 同一台、同一类事件的最短间隔：到了（或这次开机还没发过）就记下现在、返回 true */
static bool st_gap_ok(uint32_t *t_last, uint32_t gap_ms, uint32_t now)
{
    if (*t_last && now - *t_last < gap_ms) return false;
    *t_last = now ? now : 1u;
    return true;
}

/* ---- 涂鸦表的查找（M5 第八步）：放在 st_zone 前面，它的人体类间隔要看有效 ZoneType ---- */

int zh_dev_tuya_dev(const zh_dev_rec_t *r)
{
    if (!r || !(r->flags & ZH_DEVF_INTERVIEWED)) return -1;
    char m[sizeof(r->manuf) + 1], o[sizeof(r->model) + 1];
    memcpy(m, r->manuf, sizeof(r->manuf));         /* 盘上读回的记录不一定以 0 结尾 */
    m[sizeof(r->manuf)] = 0;
    memcpy(o, r->model, sizeof(r->model));
    o[sizeof(r->model)] = 0;
    return zh_tuya_find(m, o);
}

uint16_t zh_dev_zone_type(const zh_dev_rec_t *r)
{
    if (!r) return ZH_ZONE_TYPE_UNKNOWN;
    if (ep_with_in(r, ZH_CL_IAS_ZONE) >= 0 && r->zone_type != ZH_ZONE_TYPE_UNKNOWN) return r->zone_type;
    uint16_t tz = zh_tbl_zone_type(zh_tuya_fields(zh_dev_tuya_dev(r)));
    if (tz == ZH_ZONE_TYPE_UNKNOWN) tz = zh_tbl_zone_type(zh_lumi_fields(zh_dev_lumi_dev(r)));   /* 小米：访谈走不完、读不到 ZoneType 的漏水 / 烟雾靠表 */
    return tz != ZH_ZONE_TYPE_UNKNOWN ? tz : r->zone_type;   /* 不是涂鸦报警类的照记录：IAS 簇可能被截掉了（每端点只存 12 个簇、只存 3 个端点），登记请求补的 ZoneType 照样算 */
}

int zh_dev_lumi_dev(const zh_dev_rec_t *r)
{
    if (!r || !r->model[0]) return -1;
    char o[sizeof(r->model) + 1];
    memcpy(o, r->model, sizeof(r->model));         /* 盘上读回的记录不一定以 0 结尾 */
    o[sizeof(r->model)] = 0;
    return zh_lumi_find(o);
}

/* zh_dev_lumi_dev 带缓存（x->lumi_k）。还没有型号不缓存 */
static int lumi_of(zh_dev_t *x)
{
    if (!x->rec.model[0]) return -1;
    if (!x->lumi_k) {
        const int ld = zh_dev_lumi_dev(&x->rec);
        x->lumi_k = (uint16_t)(ld < 0 ? 1 : ld + 2);
    }
    return x->lumi_k >= 2 ? (int)x->lumi_k - 2 : -1;
}

/* zh_dev_tuya_dev 带缓存（x->tuya_k）。没访谈完不缓存：厂商名 / 型号还没有 */
static int tuya_of(zh_dev_t *x)
{
    if (!(x->rec.flags & ZH_DEVF_INTERVIEWED)) return -1;
    if (!x->tuya_k) {
        const int td = zh_dev_tuya_dev(&x->rec);
        x->tuya_k = (uint16_t)(td < 0 ? 1 : td + 2);
    }
    return x->tuya_k >= 2 ? (int)x->tuya_k - 2 : -1;
}

/* ZoneStatus 从 prev 变成 status 该出的事件：每次翻转都出 —— 人体 / 振动不再"5 min 一条"、电量低不再"每天一条"
   （网关不做人为干预，交给 Muse；2026-10-24 用户定） */
static uint8_t zone_ev(uint16_t prev, uint16_t status)
{
    uint8_t ev = 0;
    if ((status & ZH_IAS_ST_ALARM1) && !(prev & ZH_IAS_ST_ALARM1)) ev |= ZH_STEV_ALARM_ON;
    if (!(status & ZH_IAS_ST_ALARM1) && (prev & ZH_IAS_ST_ALARM1)) ev |= ZH_STEV_ALARM_OFF;
    if ((status & ZH_IAS_ST_TAMPER) && !(prev & ZH_IAS_ST_TAMPER)) ev |= ZH_STEV_TAMPER;
    if ((status & ZH_IAS_ST_BATTERY) && !(prev & ZH_IAS_ST_BATTERY)) ev |= ZH_STEV_BATTERY_LOW;
    return ev;
}

/* ZoneStatus 进读数并出事件；prev = 上一次的（调用方定：IAS 是读数里的，涂鸦另有算法，见 st_tuya_zone） */
static void st_zone_from(zh_dev_core_t *d, unsigned idx, uint16_t prev, uint16_t status, uint32_t now)
{
    zh_dev_t *x = &d->dev[idx];
    x->st.zone_status = status;
    st_set(x, ZH_ST_ZONE, now);
    d->st.reports++;
    d->dirty = true;
    st_emit(d, idx, zone_ev(prev, status));
}

/* IAS ZoneStatus（Zone Status Change 通知或 ZONE_STATUS 属性）。这次开机头一回收到时把"之前"当 0：开着的报警照样报一次 */
static void st_zone(zh_dev_core_t *d, unsigned idx, uint16_t status, uint32_t now)
{
    const zh_dev_t *x = &d->dev[idx];
    st_zone_from(d, idx, (x->st.have & (1u << ZH_ST_ZONE)) ? x->st.zone_status : 0, status, now);
}

/* 电量 %（0–100）进读数；降到 15 % 及以下出电量低（6.7），之后回到 20 % 及以上才重新算（ZH_ST_BATT_REARM_PCT） */
static void st_batt_pct(zh_dev_core_t *d, unsigned idx, uint8_t pct, uint32_t now)
{
    zh_dev_t *x = &d->dev[idx];
    x->st.battery_pct = pct > 100u ? 100u : pct;
    st_set(x, ZH_ST_BATTERY_PCT, now);
    d->st.reports++;
    d->dirty = true;
    if (x->st.battery_pct >= ZH_ST_BATT_REARM_PCT) x->batt_low = false;
    else if (x->st.battery_pct <= ZH_ST_BATT_LOW_PCT && !x->batt_low) {   /* 降到门槛：报一次，回到 20 % 及以上才重新算（门槛附近抖不算翻转） */
        x->batt_low = true;
        st_emit(d, idx, ZH_STEV_BATTERY_LOW);
    }
}

/* 电池电压（毫伏）进读数；小米表给了曲线的再换成电量 %（同 zhc batteryVoltageToPercentage）走 st_batt_pct */
static void st_batt_mv(zh_dev_core_t *d, unsigned idx, int32_t mv, uint32_t now)
{
    zh_dev_t *x = &d->dev[idx];
    x->st.battery_mv = (uint16_t)(mv < 0 ? 0 : mv > 0xFFFF ? 0xFFFF : mv);
    st_set(x, ZH_ST_BATTERY_MV, now);
    d->st.reports++;
    d->dirty = true;
    const int p = zh_lumi_batt_pct(lumi_of(x), mv);
    if (p >= 0) st_batt_pct(d, idx, (uint8_t)p, now);
}

/* 占用 0 / 1 进读数；0 → 1 出 OCC_ON、1 → 0 出 OCC_OFF。小米人体只报"有人"：表给了秒数的，有人时记下清零时刻（occ_tick 到时报没人） */
static void st_occ(zh_dev_core_t *d, unsigned idx, uint8_t occ, uint32_t now)
{
    zh_dev_t *x = &d->dev[idx];
    const uint8_t prev = (x->st.have & (1u << ZH_ST_OCCUPANCY)) ? x->st.occupancy : 0;
    x->st.occupancy = occ ? 1u : 0u;
    const uint16_t os = x->st.occupancy ? zh_lumi_occ_s(lumi_of(x)) : 0;
    x->t_occ_clear = os ? (now + os * 1000u ? now + os * 1000u : 1u) : 0;   /* 0 表示"不用"：碰巧算出 0 记成 1 */
    st_set(x, ZH_ST_OCCUPANCY, now);
    d->st.reports++;
    d->dirty = true;
    uint8_t ev = (uint8_t)(x->st.occupancy && !prev ? ZH_STEV_OCC_ON : !x->st.occupancy && prev ? ZH_STEV_OCC_OFF : 0);
    if (zh_dev_zone_type(&x->rec) == ZH_ZONE_MOTION) ev = 0;   /* 同一台还有 IAS 人体：同一次有人只出一条，事件走 IAS 那路 */
    st_emit(d, idx, ev);
}

/* ZCL 定长整数类型的"无效值"（全 1 的无符号、最小的有符号、布尔 0xFF；ZCL spec 2.6.2 各类型的 Non-value） */
static bool zcl_invalid(const zh_zcl_attr_t *a)
{
    uint32_t u;
    int32_t sv;
    if (a->type == ZH_ZCL_T_BOOL) return a->len == 1 && a->val && a->val[0] == 0xFF;
    if (a->type >= 0x20 && a->type <= 0x23 && zh_zcl_attr_uint(a, &u)) {
        const unsigned w = (unsigned)(a->type - 0x1F);
        return u == (w == 4 ? 0xFFFFFFFFu : (1u << (8 * w)) - 1u);
    }
    if (a->type >= 0x28 && a->type <= 0x2B && zh_zcl_attr_int(a, &sv)) {
        const unsigned w = (unsigned)(a->type - 0x27);
        return sv == (w == 4 ? INT32_MIN : -(int32_t)(1u << (8 * w - 1)));
    }
    return false;
}

/* 小米（M5 第十步）：一批 (cl, id, 值) 读数（同一帧里的），定义在涂鸦那节后面 */
typedef struct { uint16_t cl, id; int32_t raw; } lumi_in_t;
#define LUMI_IN_MAX 32u                            /* 一帧最多收几条（0xFF01 一般十来条） */
static void st_lumi_apply(zh_dev_core_t *d, unsigned idx, int ld, const lumi_in_t *in, unsigned n, uint32_t now);
static void st_lumi_tlv(zh_dev_core_t *d, unsigned idx, const uint8_t *s, size_t n, uint32_t now);
static void st_learn_str(zh_dev_core_t *d, unsigned idx, const zh_zcl_attr_t *a);

/* 一条属性记录进读数。每个属性只收 SDK XML 规定的那一个类型（别的宽度不收：int32 气压 ×10 会有符号溢出、uint32 照度让 pow 溢出 —— 第一轮审计 ❌）；
   无效值哨兵（"量不出"）不收但也不算坏；认识的簇里不认识的属性不理（访谈读 ZoneType 的应答也会走到这里）。单位见 zh_zcl.h 各属性旁 */
static void st_apply(zh_dev_core_t *d, unsigned idx, uint16_t cluster, const zh_zcl_attr_t *a, uint32_t now)
{
    zh_dev_t *x = &d->dev[idx];
    uint32_t u = 0;
    int32_t sv = 0;
    const bool u8 = a->type == ZH_ZCL_T_UINT8 && zh_zcl_attr_uint(a, &u), u16 = a->type == ZH_ZCL_T_UINT16 && zh_zcl_attr_uint(a, &u);
    const bool s16 = a->type == ZH_ZCL_T_INT16 && zh_zcl_attr_int(a, &sv);
    int field = -1;                                /* 收下的读数；-1 = 没收 */
    bool known = true;                             /* 认识的属性（类型不对才算坏） */
    bool typed = false;
    /* 小米表改写了这一路（门磁的 OnOff 是开合、人体的照度直接是 lux、灯的 Occupancy 丢掉……）：照表，不走通用解析 */
    const int ld = lumi_of(x);
    if (ld >= 0 && zh_lumi_has_cl(ld, cluster)) {
        if (zcl_invalid(a)) return;                /* ZCL 的"无效值"（温度 0x8000、照度 0xFFFF……）：通用解析不收，改写路也不收（审计 ⚠️） */
        int32_t raw;
        zh_tbl_val_t v[ZH_TUYA_DP_FIELDS_MAX];
        bool dropped = false;
        if (zh_lumi_val_int(a->type, a->val, a->len, &raw) && (zh_lumi_map(ld, cluster, a->id, raw, v, ZH_TUYA_DP_FIELDS_MAX, &dropped) || dropped)) {
            const lumi_in_t in = { cluster, a->id, raw };
            st_lumi_apply(d, idx, ld, &in, 1, now);
            return;
        }
    }
    switch (cluster) {
    case ZH_CL_POWER_CFG:
        if (a->id == ZH_ATTR_BATTERY_PCT2) {
            typed = u8;
            if (u8 && u != 0xFF) {
                st_batt_pct(d, idx, (uint8_t)(u / 2u > 100u ? 100u : u / 2u), now);   /* 单位 0.5 % */
                return;
            }
        } else if (a->id == ZH_ATTR_BATTERY_MV100) {
            typed = u8;
            if (u8 && u != 0xFF) {
                st_batt_mv(d, idx, (int32_t)(u * 100u), now);   /* 小米表给了曲线的顺带算电量 % */
                return;
            }
        } else known = false;
        break;
    case ZH_CL_ONOFF:
        if (a->id == ZH_ATTR_ON_OFF) {
            typed = a->type == ZH_ZCL_T_BOOL && a->len == 1;
            if (typed && a->val[0] <= 1) {         /* boolean：0xFF = 无效 */
                x->st.onoff = a->val[0];
                field = ZH_ST_ONOFF;
            }
        } else known = false;
        break;
    case ZH_CL_LEVEL:
        if (a->id == ZH_ATTR_CURRENT_LEVEL) {
            typed = u8;
            if (u8 && u <= 254u) {
                const uint32_t pct = (u * 100u + 127u) / 254u;
                x->st.level_pct = (uint8_t)(u && pct == 0 ? 1u : pct);   /* 1 不舍成 0 %：亮着就不是 0 */
                field = ZH_ST_LEVEL;
            }
        } else known = false;
        break;
    case ZH_CL_ILLUMINANCE:
        if (a->id == ZH_ATTR_ILLUM_MEASURED) {
            typed = u16;
            if (u16 && u != 0xFFFF) {
                x->st.lux = u == 0 ? 0u : (uint32_t)lround(pow(10.0, ((double)u - 1.0) / 10000.0));   /* 10000·log10(lux)+1；0 = 太暗量不出；最大约 3.5e6 */
                field = ZH_ST_LUX;
            }
        } else known = false;
        break;
    case ZH_CL_TEMPERATURE:
        if (a->id == ZH_ATTR_TEMP_MEASURED) {
            typed = s16;
            if (s16 && sv != -0x8000) {
                x->st.temp_c100 = (int16_t)sv;
                field = ZH_ST_TEMP;
            }
        } else known = false;
        break;
    case ZH_CL_PRESSURE:
        if (a->id == ZH_ATTR_PRESSURE_MEASURED) {
            typed = s16;
            if (s16 && sv != -0x8000) {            /* 0x8000 无效 */
                x->st.pressure_hpa10 = sv * 10;    /* 0.1 kPa = 1 hPa；int16 × 10 不会溢出 int32 */
                field = ZH_ST_PRESSURE;
            }
        } else known = false;
        break;
    case ZH_CL_HUMIDITY:
        if (a->id == ZH_ATTR_HUMIDITY_MEASURED) {
            typed = u16;
            if (u16 && u != 0xFFFF && u <= 10000u) {
                x->st.hum_c100 = (uint16_t)u;
                field = ZH_ST_HUMIDITY;
            }
        } else known = false;
        break;
    case ZH_CL_OCCUPANCY:
        if (a->id == ZH_ATTR_OCCUPANCY) {
            if (a->type != ZH_ZCL_T_BITMAP8 || !zh_zcl_attr_uint(a, &u)) {
                d->st.report_bad++;
                return;
            }
            st_occ(d, idx, (uint8_t)(u & 1u), now);
            return;
        }
        return;
    case ZH_CL_IAS_ZONE:
        if (a->id == ZH_IAS_ZONE_STATUS) {
            if (a->type != ZH_ZCL_T_BITMAP16 || !zh_zcl_attr_uint(a, &u)) {
                d->st.report_bad++;
                return;
            }
            st_zone(d, idx, (uint16_t)u, now);
        }
        return;                                    /* ZoneType（访谈读的）等别的属性：不是读数 */
    case ZH_CL_BASIC:                              /* 厂家 / 型号的主动上报学进记录；小米的 0xFF01 结构进 TLV（M5 第十步） */
        if (a->id == ZH_BASIC_MANUF || a->id == ZH_BASIC_MODEL) {
            st_learn_str(d, idx, a);
        } else if (a->id == ZH_ATTR_LUMI_FF01) {
            const uint8_t *s;
            size_t n;
            if (zh_zcl_attr_str(a, &s, &n)) st_lumi_tlv(d, idx, s, n, now);
        }
        return;
    default:
        return;                                    /* 别的簇：不是读数 */
    }
    if (!known) return;                            /* 认识的簇、不认识的属性：不理（不算坏） */
    if (!typed) {
        d->st.report_bad++;                        /* 类型不对：不收 */
        return;
    }
    if (field < 0) return;                         /* 无效值哨兵（量不出）：不收、也不算坏 */
    st_set(x, (unsigned)field, now);
    d->st.reports++;
    d->dirty = true;
}

/* IAS 登记应答（簇专用、客户端 → 服务端）：[成功][zone id = 下标]（ha.xml:1146）。返回交没交出去（应用命令队列满回 false）。
   不等送达结果：回登记请求的那条丢了设备会再发请求；访谈 / 补做里主动发的那条靠回读 ZoneState 确认（ZH_IV_ENROLL） */
static bool ias_enroll_rsp(zh_dev_core_t *d, unsigned idx, uint8_t dst_ep)
{
    zh_dev_t *x = &d->dev[idx];
    const uint8_t seq = d->zcl_seq++;
    const uint8_t pl[5] = { ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_DIS_DR, seq, ZH_IAS_CMD_ENROLL_RSP, ZH_IAS_ENROLL_SUCCESS, (uint8_t)idx };
    const zh_ezsp_aps_t aps = { .profile_id = ZH_HA_PROFILE, .cluster_id = ZH_CL_IAS_ZONE, .src_ep = ZH_DEV_SRC_EP, .dst_ep = dst_ep,
                                .options = ZH_APS_OPTS_DEFAULT | ZH_APS_OPT_SOURCE_EUI64, .group_id = 0, .sequence = 0 };
    uint8_t b[48];
    const size_t n = zh_ezsp_enc_send_unicast(b, sizeof(b), 0, ZH_OUTGOING_DIRECT, x->rec.nwk, &aps, seq, sizeof(pl), pl);
    if (!n || !d->ops.send || !d->ops.send(d->ops.ctx, b, n, ZH_DEV_TAG_KIND_MISC, false)) return false;
    d->st.enroll_rsps++;
    return true;
}

/* ---------------------------------------------------------------- 控制（M5 第七步，规则见 zh_dev.h 的控制一节） */

static void ctl_finish(zh_dev_core_t *d, zh_ctl_result_t r, uint32_t now)
{
    zh_ctl_t *c = &d->ctl;
    if (!c->active || c->result != ZH_CTL_R_NONE) return;   /* 已经有结论了：后到的不改（默认应答先到、messageSent 后到是常态） */
    c->result = r;
    c->t_done = now ? now : 1u;
    if (r == ZH_CTL_R_CONFIRMED) d->st.ctl_confirmed++;
    else if (r == ZH_CTL_R_DELIVERED) d->st.ctl_delivered++;
    else d->st.ctl_failed++;
}

/* 一条控制命令的 ZCL 负载与去向（同步发与休眠排队共用，M5 第十二步）。ZCL 的开关 / 亮度簇优先，没有就按涂鸦表写数据点（M5 第九步）。
   用掉一个 ZCL 序号；涂鸦的 seq 由调用方发成功后再加 */
typedef struct {
    uint16_t cl;
    uint8_t  ep, cmd, seq, tdp;
    int32_t  traw;
    bool     tuya;
    uint8_t  pl[3 + ZH_TUYA_CTL_PL_MAX];
    size_t   n;
} ctl_fr_t;

static zh_ctl_err_t ctl_frame(zh_dev_core_t *d, zh_dev_t *x, zh_ctl_act_t act, uint8_t level_pct, ctl_fr_t *f)
{
    memset(f, 0, sizeof(*f));
    uint16_t cl = act == ZH_CTL_LEVEL ? ZH_CL_LEVEL : ZH_CL_ONOFF;
    int e = ep_with_in(&x->rec, cl);
    int td = -1;
    unsigned tact = ZH_TUYA_ACT_ON;
    if (e < 0) {                                   /* 没有 ZCL 的簇：涂鸦表里有写法就写数据点（ZCL 优先） */
        td = tuya_of(x);
        if (!(zh_tuya_ctl_fields(td) & (1u << (act == ZH_CTL_LEVEL ? ZH_TF_LEVEL : ZH_TF_ONOFF)))) return ZH_CTL_E_NO_CLUSTER;
        if (act == ZH_CTL_TOGGLE) {                /* 涂鸦没有切换命令：按当前读数取反（排队的在补发时才取反） */
            if (!(x->st.have & (1u << ZH_ST_ONOFF))) return ZH_CTL_E_STATE;
            tact = x->st.onoff ? ZH_TUYA_ACT_OFF : ZH_TUYA_ACT_ON;
        } else {
            tact = act == ZH_CTL_ON ? ZH_TUYA_ACT_ON : act == ZH_CTL_OFF ? ZH_TUYA_ACT_OFF : ZH_TUYA_ACT_LEVEL;
        }
        if (tact == ZH_TUYA_ACT_LEVEL && level_pct == 0 && !(zh_tuya_ctl_fields(td) & (1u << ZH_TF_ONOFF))) return ZH_CTL_E_NO_OFF;
        if (x->rec.n_ep == 0) return ZH_CTL_E_NO_CLUSTER;
        cl = ZH_CL_TUYA;
        e = ep_with_in(&x->rec, ZH_CL_TUYA);
        if (e < 0) e = 0;                          /* 访谈没记下 0xEF00（端点簇被截掉）：发第一个端点 */
    }
    f->cl = cl;
    f->ep = x->rec.ep[e].ep;
    f->seq = d->zcl_seq++;
    f->pl[0] = ZH_ZCL_FC_CLUSTER;                  /* 簇专用、客户端 → 服务端、不禁默认应答：设备回的默认应答就是确认（8.11 第 5 步） */
    f->pl[1] = f->seq;
    if (td >= 0) {
        f->tuya = true;
        f->pl[0] = ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_DIS_DR;   /* 照 z2m 不要默认应答（tuya.js:258）：确认看设备回报的 dp */
        const size_t k = zh_tuya_enc_ctl(td, tact, level_pct, d->tuya_seq, f->pl + 3, sizeof(f->pl) - 3, &f->cmd, &f->tdp, &f->traw);
        if (!k) return ZH_CTL_E_NO_CLUSTER;
        f->pl[2] = f->cmd;
        f->n = 3 + k;
    } else if (act == ZH_CTL_LEVEL) {
        f->cmd = ZH_CL_CMD_MOVE_TO_LEVEL_ONOFF;
        f->pl[2] = f->cmd;
        f->pl[3] = (uint8_t)(((uint32_t)level_pct * 254u + 50u) / 100u);   /* 0–100 % → 0–254（0 = 关：带 OnOff 的那个命令） */
        f->pl[4] = 0;                              /* 渐变时间 0（立即；0.1 s 一格） */
        f->pl[5] = 0;
        f->n = 6;
    } else {
        f->cmd = act == ZH_CTL_ON ? ZH_CL_CMD_ON : act == ZH_CTL_OFF ? ZH_CL_CMD_OFF : ZH_CL_CMD_TOGGLE;
        f->pl[2] = f->cmd;
        f->n = 3;
    }
    return ZH_CTL_E_OK;
}

/* 把编好的负载交给应用命令队列 */
static bool ctl_send(zh_dev_core_t *d, const zh_dev_t *x, const ctl_fr_t *f, uint16_t mtag, uint32_t app_tag)
{
    const zh_ezsp_aps_t aps = { .profile_id = ZH_HA_PROFILE, .cluster_id = f->cl, .src_ep = ZH_DEV_SRC_EP, .dst_ep = f->ep,
                                .options = ZH_APS_OPTS_DEFAULT, .group_id = 0, .sequence = 0 };
    uint8_t b[64];
    const size_t len = zh_ezsp_enc_send_unicast(b, sizeof(b), 0, ZH_OUTGOING_DIRECT, x->rec.nwk, &aps, mtag, (uint8_t)f->n, f->pl);
    return len && d->ops.send && d->ops.send(d->ops.ctx, b, len, app_tag, false);
}

zh_ctl_err_t zh_dev_ctl_start(zh_dev_core_t *d, const uint8_t ieee[8], zh_ctl_act_t act, uint8_t level_pct, uint32_t now_ms)
{
    if (!d || !ieee || (unsigned)act > (unsigned)ZH_CTL_LEVEL || level_pct > 100u) return ZH_CTL_E_ARG;
    d->t_now = now_ms;
    if (d->ctl.active) return ZH_CTL_E_BUSY;
    const int i = zh_dev_find_ieee(d, ieee);
    if (i < 0) return ZH_CTL_E_NOT_FOUND;
    zh_dev_t *x = &d->dev[i];
    if ((x->rec.flags & ZH_DEVF_LEFT) || x->rec.nwk == NWK_COORD || x->rec.nwk >= NWK_RESERVED) return ZH_CTL_E_LEFT;   /* 短地址作废时发出去就是广播 */
    const bool rx_on = (x->rec.flags & (ZH_DEVF_ANNCED | ZH_DEVF_RX_ON)) == (ZH_DEVF_ANNCED | ZH_DEVF_RX_ON) || x->rec.node_type == ZH_NODE_ROUTER;
    ctl_fr_t f;
    const zh_ctl_err_t fe = ctl_frame(d, x, act, level_pct, &f);   /* 排队的也先编一遍：做不到的（没有簇 / 不知道开没开 / 关不了）当场回 */
    if (fe != ZH_CTL_E_OK) return fe;
    if (!d->net_up) return ZH_CTL_E_NET;
    if (!rx_on) {                                  /* 休眠设备（8.11 第 6 步）：进这台的待发槽（新命令顶掉旧的：要的是最后那个状态），它下次说话时补发 */
        zh_ctlq_t *q = &x->cq;
        if (q->on) d->st.ctlq_replaced++;
        memset(q, 0, sizeof(*q));
        q->on = true;
        q->act = (uint8_t)act;
        q->level = level_pct;
        q->gen = (uint8_t)(++d->cq_gen);
        q->t_q = now_ms ? now_ms : 1u;
        d->st.ctlq_queued++;
        memset(&d->ctl, 0, sizeof(d->ctl));        /* 同步调用方立刻拿到 PENDING */
        d->ctl.active = true;
        d->ctl.idx = (uint8_t)i;
        d->ctl.result = ZH_CTL_R_PENDING;
        d->ctl.t_sent = d->ctl.t_done = q->t_q;
        return ZH_CTL_E_OK;
    }
    const uint16_t gen = (uint16_t)((d->ctl_gen + 1u) & 0x7FFFu);
    const uint16_t mtag = (uint16_t)(0x8000u | gen);
    if (!ctl_send(d, x, &f, mtag, ZH_DEV_TAG_KIND_CTL | ((uint32_t)gen << 8))) return ZH_CTL_E_SEND;
    d->ctl_gen = gen;
    if (x->cq.on) {                                /* 排着一条旧的（当时还不知道它常电：访谈后才定型）：新命令顶掉它，不然它下次说话时被补发（审计 ❌） */
        x->cq.on = false;
        d->st.ctlq_replaced++;
    }
    if (f.tuya) {
        d->tuya_seq++;
        d->st.ctl_tuya++;
    }
    memset(&d->ctl, 0, sizeof(d->ctl));
    d->ctl.active = true;
    d->ctl.idx = (uint8_t)i;
    d->ctl.cmd = f.cmd;
    d->ctl.level_pct = act == ZH_CTL_LEVEL ? level_pct : 0;
    d->ctl.zcl_seq = f.seq;
    d->ctl.cluster = f.cl;
    d->ctl.msg_tag = mtag;
    d->ctl.gen = gen;
    d->ctl.t_sent = now_ms ? now_ms : 1u;
    d->ctl.tuya_dp = f.tdp;
    d->ctl.tuya_raw = f.traw;
    d->st.ctl_sent++;
    return ZH_CTL_E_OK;
}

/* 休眠设备的待发（8.11 第 6 步；M5 第十二步）。messageTag = 0x4000 | 设备下标 << 8 | 代号（访谈 / 补读用序号 < 0x100、同步控制 0x8000 | 代号，碰不上） */
static uint16_t cq_mtag(unsigned idx, uint8_t gen) { return (uint16_t)(0x4000u | ((idx & 0x3Fu) << 8) | gen); }

/* 它刚说过话（还在快速轮询）的 3 s 里补发；一拍一条；交不出去下一拍再试；30 min 没送达就作废。送达靠 messageSent，没送达留到下次醒来 */
static void ctlq_tick(zh_dev_core_t *d, uint32_t now)
{
    bool sent = false;
    for (unsigned i = 0; i < ZH_DEV_MAX; i++) {
        zh_dev_t *x = &d->dev[i];
        zh_ctlq_t *q = &x->cq;
        if (!x->used || !q->on) continue;
        if ((int32_t)(now - q->t_q) >= (int32_t)ZH_CTLQ_MAX_MS) {   /* 排了 30 min 还没送到 */
            q->on = false;
            d->st.ctlq_expired++;
            continue;
        }
        if (q->inflight) {                         /* 等 messageSent；一直没来（链路复位吞了）就当没送到、下次醒来再发 */
            if ((int32_t)(now - q->t_sent) >= (int32_t)ZH_CTLQ_SENT_MS) q->inflight = false;
            continue;
        }
        if (sent || !d->net_up || !x->seen || (x->rec.flags & ZH_DEVF_LEFT) || x->rec.nwk == NWK_COORD || x->rec.nwk >= NWK_RESERVED) continue;
        const int32_t age = (int32_t)(now - x->t_seen);
        if (age >= (int32_t)ZH_IV_AWAKE_MS) continue;   /* 没醒（< 0：回调里的时刻比 tick 晚，是刚收到） */
        if (q->t_sent && (int32_t)(x->t_seen - q->t_sent) <= 0) continue;   /* 上次补发之后它还没再说过话：同一个醒着的窗口里别连发 */
        ctl_fr_t f;
        if (ctl_frame(d, x, (zh_ctl_act_t)q->act, q->level, &f) != ZH_CTL_E_OK) {   /* 现在做不到了（例如涂鸦切换还不知道开没开）：作废 */
            q->on = false;
            d->st.ctlq_dropped++;
            continue;
        }
        q->gen = (uint8_t)(++d->cq_gen);           /* 每发一次换代号：上一发迟到的失败回执别安到这一发头上（审计 ❌） */
        if (!ctl_send(d, x, &f, cq_mtag(i, q->gen), ZH_DEV_TAG_KIND_QCTL | ((uint32_t)i << 8) | ((uint32_t)q->gen << 16))) continue;
        sent = true;
        if (f.tuya) d->tuya_seq++;
        q->inflight = true;
        q->t_sent = now ? now : 1u;
        q->tries++;
        d->st.ctlq_sent++;
    }
}

void zh_dev_ctl_clear(zh_dev_core_t *d)
{
    if (d) memset(&d->ctl, 0, sizeof(d->ctl));
}

void zh_dev_on_msg_sent(zh_dev_core_t *d, uint16_t msg_tag, uint32_t status, uint32_t now_ms)
{
    if (!d) return;
    d->t_now = now_ms;
    if ((msg_tag & 0xC000u) == 0x4000u) {          /* 休眠设备的补发：送达就清掉，没送达留到下次醒来 */
        const unsigned idx = (msg_tag >> 8) & 0x3Fu;
        if (idx >= ZH_DEV_MAX) return;
        zh_ctlq_t *q = &d->dev[idx].cq;
        if (!q->on || q->gen != (uint8_t)msg_tag) return;
        if (!q->inflight && status != 0) return;   /* 期限过了才来的失败：早就当没送到了 */
        q->inflight = false;                       /* 期限过了才来的送达照样清槽：不然下次醒来再发一遍（切换会被撤销，审计 ❌） */
        if (status == 0) {
            q->on = false;
            d->st.ctlq_delivered++;
        } else {
            d->st.ctlq_failed++;
        }
        return;
    }
    zh_ctl_t *c = &d->ctl;
    if (!c->active || c->result != ZH_CTL_R_NONE || msg_tag != c->msg_tag) return;
    if (status == 0) {
        c->delivered = true;                       /* 收到 APS ACK：等默认应答 / 上报到 ZH_CTL_CONFIRM_MS，没等到就算 delivered */
        return;
    }
    c->status = status;                            /* 没送达（路由不通 / 设备没 ACK） */
    ctl_finish(d, ZH_CTL_R_FAILED, now_ms);
}

/* 默认应答成功 = 设备照做了：按命令把读数写成新状态（Z2M 同此"乐观更新"）。不写的话 hub.device.control 回包带的是命令之前的缓存
   ——"关了、已确认、状态：开"（2026-10-23 灯泡上板）；设备随后的上报照样覆盖。上报确认的那条路读数已经是新的，涂鸦确认看的就是回报的 dp，都不走这里 */
static void ctl_optimistic(zh_dev_core_t *d, uint32_t now)
{
    const zh_ctl_t *c = &d->ctl;
    zh_dev_t *x = &d->dev[c->idx];
    if (c->cluster == ZH_CL_ONOFF) {
        if (c->cmd == ZH_CL_CMD_ON) x->st.onoff = 1;
        else if (c->cmd == ZH_CL_CMD_OFF) x->st.onoff = 0;
        else if (c->cmd == ZH_CL_CMD_TOGGLE && (x->st.have & (1u << ZH_ST_ONOFF))) x->st.onoff = (uint8_t)!x->st.onoff;
        else return;                               /* 不知道切换之前是开是关：不猜 */
        st_set(x, ZH_ST_ONOFF, now);
    } else if (c->cluster == ZH_CL_LEVEL && c->cmd == ZH_CL_CMD_MOVE_TO_LEVEL_ONOFF) {
        if (c->level_pct) {                        /* MoveToLevelWithOnOff：> 0 是开到这个亮度 */
            x->st.level_pct = c->level_pct;
            st_set(x, ZH_ST_LEVEL, now);
            x->st.onoff = 1;
        } else {
            x->st.onoff = 0;                       /* 0 = 关，亮度不变（设备记着关之前的亮度） */
        }
        if (ep_with_in(&x->rec, ZH_CL_ONOFF) >= 0) st_set(x, ZH_ST_ONOFF, now);   /* 没有开关簇的只调光设备：开关读数没意义，不写 */
    } else {
        return;
    }
    d->dirty = true;
}

/* 控制命令的确认：在途那台设备、那个簇、那个序号的默认应答（[命令号][状态]），或者这个簇的属性上报（设备状态变了会报，绑过的灯就是这样） */
static void ctl_on_zcl(zh_dev_core_t *d, uint16_t sender, uint16_t cluster, const uint8_t *pl, size_t len, uint32_t now)
{
    zh_ctl_t *c = &d->ctl;
    if (!c->active || c->result != ZH_CTL_R_NONE || cluster != c->cluster || d->dev[c->idx].rec.nwk != sender) return;
    zh_zcl_frame_t f;
    if (zh_zcl_parse(pl, len, &f) != ZH_ZCL_OK || !(f.fc & ZH_ZCL_FC_S2C)) return;
    if (c->cluster == ZH_CL_TUYA && (f.fc & ZH_ZCL_FC_TYPE_MASK) == ZH_ZCL_FC_CLUSTER) {   /* 涂鸦（厂商位带不带都收）：窗口内回报了写的那个 dp */
        if (!zh_tuya_is_data_cmd(f.cmd) || now - c->t_sent >= ZH_CTL_CONFIRM_MS) return;
        zh_tuya_iter_t it;
        zh_tuya_dp_t dp;
        if (!zh_tuya_iter_init(&it, f.pl, f.n)) return;
        int32_t v;
        while (zh_tuya_next(&it, &dp) == 1)
            if (dp.dp == c->tuya_dp && zh_tuya_dp_int(&dp, &v) && v == c->tuya_raw) {   /* 值也要对上：别被全量回报 / 上一条的旧值顶掉 */
                ctl_finish(d, ZH_CTL_R_CONFIRMED, now);
                return;
            }
        return;
    }
    if ((f.fc & ZH_ZCL_FC_MFG) || (f.fc & ZH_ZCL_FC_TYPE_MASK) != 0) return;
    if (f.cmd == ZH_ZCL_CMD_DEFAULT_RSP) {
        if (f.seq != c->zcl_seq || f.n < 2 || f.pl[0] != c->cmd) return;
        if (f.pl[1] == ZH_ZCL_ST_SUCCESS) {
            ctl_optimistic(d, now);
            ctl_finish(d, ZH_CTL_R_CONFIRMED, now);
        } else {
            c->zcl_status = f.pl[1];               /* 设备拒了（不支持这个命令之类） */
            ctl_finish(d, ZH_CTL_R_FAILED, now);
        }
    } else if (f.cmd == ZH_ZCL_CMD_REPORT_ATTR && now - c->t_sent < ZH_CTL_CONFIRM_MS) {
        ctl_finish(d, ZH_CTL_R_CONFIRMED, now);    /* 命令发出后很快来了这个簇的上报：状态变了 */
    }
}

/* 到期给结论：窗口过了只送达的 → DELIVERED；一点回音都没有 → FAILED */
static void ctl_tick(zh_dev_core_t *d, uint32_t now)
{
    zh_ctl_t *c = &d->ctl;
    if (!c->active || c->result != ZH_CTL_R_NONE) return;
    const uint32_t age = now - c->t_sent;
    if (c->delivered && age >= ZH_CTL_CONFIRM_MS) ctl_finish(d, ZH_CTL_R_DELIVERED, now);
    else if (age >= ZH_CTL_DEADLINE_MS) ctl_finish(d, ZH_CTL_R_FAILED, now);
}

/* 开机补读电量（M5 第六步）：见 zh_dev.h 文件头。读电量 % 与电压（general.xml:219-220），不等结果 —— 应答走 st_on_zcl → st_apply，
   读了没回就等它下次说话（间隔与次数封顶）。一拍最多一条：别和访谈 / 配置抢应用命令队列 */
static void batt_tick(zh_dev_core_t *d, uint32_t now)
{
    if (!d->net_up) return;
    for (unsigned i = 0; i < ZH_DEV_MAX; i++) {
        zh_dev_t *x = &d->dev[i];
        if (!x->used || !(x->rec.flags & ZH_DEVF_INTERVIEWED) || (x->rec.flags & ZH_DEVF_LEFT) || x->rec.node_type == ZH_NODE_ROUTER) continue;
        if (x->iv != ZH_IV_NONE) continue;         /* 正在配置 / 补 CIE：它这几秒归那边用（配置阶段本来就会配电量上报） */
        if (x->st.have & ((1u << ZH_ST_BATTERY_PCT) | (1u << ZH_ST_BATTERY_MV))) continue;   /* 已经有了 */
        if (x->batt_reads >= ZH_BATT_READ_TRIES || (x->batt_reads && now - x->t_batt_read < ZH_BATT_READ_GAP_MS)) continue;
        if (!x->seen || (int32_t)(now - x->t_seen) >= (int32_t)ZH_IV_AWAKE_MS) continue;   /* 只在它刚说过话的几秒里（< 0：回调里的时刻比 tick 晚，是刚收到） */
        if (x->rec.nwk == NWK_COORD || x->rec.nwk >= NWK_RESERVED) continue;               /* 短地址作废时发出去就是广播 */
        const int e = ep_with_in(&x->rec, ZH_CL_POWER_CFG);
        if (e < 0) continue;
        static const uint16_t ids[2] = { ZH_ATTR_BATTERY_PCT2, ZH_ATTR_BATTERY_MV100 };
        uint8_t pl[8];
        const uint8_t seq = d->zcl_seq++;
        const size_t n = zh_zcl_enc_read_attrs(pl, sizeof(pl), seq, ids, 2);
        const zh_ezsp_aps_t aps = { .profile_id = ZH_HA_PROFILE, .cluster_id = ZH_CL_POWER_CFG, .src_ep = ZH_DEV_SRC_EP, .dst_ep = x->rec.ep[e].ep,
                                    .options = ZH_APS_OPTS_DEFAULT, .group_id = 0, .sequence = 0 };
        uint8_t b[48];
        const size_t len = n ? zh_ezsp_enc_send_unicast(b, sizeof(b), 0, ZH_OUTGOING_DIRECT, x->rec.nwk, &aps, seq, (uint8_t)n, pl) : 0;
        if (!len || !d->ops.send || !d->ops.send(d->ops.ctx, b, len, ZH_DEV_TAG_KIND_MISC, false)) return;   /* 交不出去：下一拍再试，不占次数 */
        x->batt_reads++;
        x->t_batt_read = now ? now : 1u;
        d->st.batt_reads++;
        return;                                    /* 一拍一条 */
    }
}

/* ---------------------------------------------------------------- 涂鸦私有簇 0xEF00（M5 第八步；规则见 include/zh_dev.h 文件头） */

static uint8_t pct_u8(int32_t v) { return (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v); }

/* 一条表读数（涂鸦 / 小米共用）。报警类（主报警 / 防拆 / 电量低）只改合成的 ZoneStatus（*zs），整帧处理完由 st_tbl_zone 一起结算 */
static void st_tbl_val(zh_dev_core_t *d, unsigned idx, const zh_tuya_val_t *v, unsigned alarm, uint16_t *zs, bool *alarm_seen, uint32_t now)
{
    zh_dev_t *x = &d->dev[idx];
    const int32_t val = v->val;
    unsigned field;
    switch (v->field) {
    case ZH_TF_ONOFF:       x->st.onoff = val ? 1u : 0u; field = ZH_ST_ONOFF; break;
    case ZH_TF_LEVEL:       x->st.level_pct = pct_u8(val); field = ZH_ST_LEVEL; break;
    case ZH_TF_TEMP:        x->st.temp_c100 = (int16_t)(val < -27315 ? -27315 : val > 32767 ? 32767 : val); field = ZH_ST_TEMP; break;   /* 不低于绝对零度；int16 的 −32768 是 ZCL 的"无效" */
    case ZH_TF_HUMIDITY:    x->st.hum_c100 = (uint16_t)(val < 0 ? 0 : val > 10000 ? 10000 : val); field = ZH_ST_HUMIDITY; break;
    case ZH_TF_LUX:         x->st.lux = (uint32_t)(val < 0 ? 0 : val); field = ZH_ST_LUX; break;
    case ZH_TF_VOLTAGE:     st_batt_mv(d, idx, val, now); return;   /* 小米表给了曲线的顺带算电量 % */
    case ZH_TF_PRESSURE:    x->st.pressure_hpa10 = val; field = ZH_ST_PRESSURE; break;
    case ZH_TF_BATTERY_PCT: st_batt_pct(d, idx, pct_u8(val), now); return;
    case ZH_TF_OCCUPANCY:   st_occ(d, idx, val ? 1u : 0u, now); return;
    case ZH_TF_BATTERY_LOW: *zs = (uint16_t)(val ? (*zs | ZH_IAS_ST_BATTERY) : (*zs & ~ZH_IAS_ST_BATTERY)); return;
    case ZH_TF_TAMPER:      *zs = (uint16_t)(val ? (*zs | ZH_IAS_ST_TAMPER) : (*zs & ~ZH_IAS_ST_TAMPER)); return;
    default:                                       /* 门磁 / 漏水 / 烟雾 / 燃气 / CO / 振动：只认主报警字段（二合一的另一个不理） */
        if (v->field == alarm) {
            *zs = (uint16_t)(val ? (*zs | ZH_IAS_ST_ALARM1) : (*zs & ~ZH_IAS_ST_ALARM1));
            *alarm_seen = true;
        }
        return;
    }
    st_set(x, field, now);
    d->st.reports++;
    d->dirty = true;
}

/* 一帧里的报警类读数一起结算。主报警报过（这一帧或之前）：整个 ZoneStatus 进读数、出事件（st_zone_from）——
   alarm1 的"之前"按读数（头一回当 0：开着的报警照样报一次），防拆 / 电量低的"之前"按合成值（之前只出过事件的不再出一次）。
   主报警还没报过（或这台根本没有主报警字段）：只出防拆 / 电量低的事件，不置 ZONE 读数 —— 不然门磁头一帧只报电量时会显示"关着" */
static void st_tbl_zone(zh_dev_core_t *d, unsigned idx, uint16_t zs, bool alarm_seen, uint32_t now)
{
    zh_dev_t *x = &d->dev[idx];
    const uint16_t old = x->tbl_zs;
    x->tbl_zs = zs;
    const bool have_zone = (x->st.have & (1u << ZH_ST_ZONE)) != 0;
    if (alarm_seen || have_zone) {
        const uint16_t prev = (uint16_t)((have_zone ? (x->st.zone_status & ZH_IAS_ST_ALARM1) : 0u) | (old & (ZH_IAS_ST_TAMPER | ZH_IAS_ST_BATTERY)));
        st_zone_from(d, idx, prev, zs, now);
    } else {
        st_emit(d, idx, (uint8_t)(zone_ev(old, zs) & (ZH_STEV_TAMPER | ZH_STEV_BATTERY_LOW)));
    }
}

/* 一帧涂鸦命令（簇专用、服务端 → 客户端）。不在表里的设备 / 表里没有的数据点只计数 */
static void st_tuya(zh_dev_core_t *d, unsigned idx, const zh_zcl_frame_t *f, uint32_t now)
{
    if ((f->fc & ZH_ZCL_FC_TYPE_MASK) != ZH_ZCL_FC_CLUSTER || !(f->fc & ZH_ZCL_FC_S2C) || !zh_tuya_is_data_cmd(f->cmd)) return;   /* 对时请求、MCU 版本、默认应答等：不是读数 */
    zh_dev_t *x = &d->dev[idx];
    d->st.tuya_frames++;
    const int td = tuya_of(x);
    if (td < 0) {
        d->st.tuya_unknown++;
        return;
    }
    zh_tuya_iter_t it;
    if (!zh_tuya_iter_init(&it, f->pl, f->n)) {
        d->st.report_bad++;
        return;
    }
    const bool ias = ep_with_in(&x->rec, ZH_CL_IAS_ZONE) >= 0;   /* 同时有 IAS 簇的：ZoneStatus 归 IAS 那路（两边合成会互相覆盖、各出一次假的报警），涂鸦帧里报警类 / 防拆 / 电量低不理 */
    const unsigned alarm = ias ? ZH_TF_NONE : zh_tbl_alarm_field(zh_tuya_fields(td));
    uint16_t zs = x->tbl_zs;
    bool touched = false, alarm_seen = false;
    zh_tuya_dp_t dp;
    int r;
    while ((r = zh_tuya_next(&it, &dp)) == 1) {
        int32_t raw;
        zh_tuya_val_t v[ZH_TUYA_DP_FIELDS_MAX];
        const unsigned n = zh_tuya_dp_int(&dp, &raw) ? zh_tuya_map(td, dp.dp, raw, v, ZH_TUYA_DP_FIELDS_MAX) : 0;
        if (!n) {
            d->st.tuya_unmapped++;
            continue;
        }
        for (unsigned k = 0; k < n; k++) {
            if (ias && (v[k].field == ZH_TF_BATTERY_LOW || v[k].field == ZH_TF_TAMPER)) continue;
            if (v[k].field == ZH_TF_BATTERY_LOW || v[k].field == ZH_TF_TAMPER || v[k].field == alarm) touched = true;
            st_tbl_val(d, idx, &v[k], alarm, &zs, &alarm_seen, now);
        }
    }
    if (r < 0) d->st.report_bad++;
    if (touched) st_tbl_zone(d, idx, zs, alarm_seen, now);
}

/* ---------------------------------------------------------------- 小米 / Aqara（M5 第十步；规则见 include/zh_dev.h 文件头） */

/* 厂家 / 型号的主动上报（或读属性应答）学进记录：变了才落盘，表的查找缓存作废 */
static void st_learn_str(zh_dev_core_t *d, unsigned idx, const zh_zcl_attr_t *a)
{
    zh_dev_t *x = &d->dev[idx];
    const uint8_t *s;
    size_t n;
    if (!zh_zcl_attr_str(a, &s, &n) || !n) return;
    char v[sizeof(x->rec.model)] = { 0 };         /* copy_str 只写到结尾的 0：后面要是零，下面按整块比才对 */
    copy_str(v, s, n);
    if (!v[0]) return;
    char *dst = a->id == ZH_BASIC_MODEL ? x->rec.model : x->rec.manuf;
    if (!memcmp(dst, v, sizeof(v))) return;
    memcpy(dst, v, sizeof(v));
    x->tuya_k = 0;
    x->lumi_k = 0;
    d->st.learned++;
    dev_save(d, idx);                              /* 落盘：重启后照样认得（小米要等下次按键才会再报型号） */
}

/* 一批小米读数（同一帧）：按表换算，读数 / 报警合成同涂鸦（st_tbl_val / st_tbl_zone）。同时有 IAS 簇的，ZoneStatus 归 IAS 那路 */
static void st_lumi_apply(zh_dev_core_t *d, unsigned idx, int ld, const lumi_in_t *in, unsigned n, uint32_t now)
{
    zh_dev_t *x = &d->dev[idx];
    const bool ias = ep_with_in(&x->rec, ZH_CL_IAS_ZONE) >= 0;
    const unsigned alarm = ias ? ZH_TF_NONE : zh_tbl_alarm_field(zh_lumi_fields(ld));
    uint16_t zs = x->tbl_zs;
    bool touched = false, alarm_seen = false;
    for (unsigned i = 0; i < n; i++) {
        zh_tbl_val_t v[ZH_TUYA_DP_FIELDS_MAX];
        bool dropped;
        const unsigned m = zh_lumi_map(ld, in[i].cl, in[i].id, in[i].raw, v, ZH_TUYA_DP_FIELDS_MAX, &dropped);
        for (unsigned k = 0; k < m; k++) {
            if (ias && (v[k].field == ZH_TF_BATTERY_LOW || v[k].field == ZH_TF_TAMPER)) continue;
            if (v[k].field == ZH_TF_BATTERY_LOW || v[k].field == ZH_TF_TAMPER || v[k].field == alarm) touched = true;
            st_tbl_val(d, idx, &v[k], alarm, &zs, &alarm_seen, now);
        }
    }
    if (touched) st_tbl_zone(d, idx, zs, alarm_seen, now);
}

/* 一串 TLV（Basic 0xFF01 / 0xFCC0 的 0x00F7 的内容）→ 读数。型号不在表里只计数 */
static unsigned tlv_collect(const uint8_t *s, size_t n, lumi_in_t *in, unsigned have, unsigned max, bool *bad)
{
    zh_lumi_tlv_t it;
    uint8_t tag;
    int32_t v;
    bool num;
    int r;
    zh_lumi_tlv_init(&it, s, n);
    while ((r = zh_lumi_tlv_next(&it, &tag, &v, &num)) == 1 && have < max)
        if (num) in[have++] = (lumi_in_t){ ZH_LDB_CL_TLV, tag, v };
    if (r < 0) *bad = true;
    return have;
}

static void st_lumi_tlv(zh_dev_core_t *d, unsigned idx, const uint8_t *s, size_t n, uint32_t now)
{
    zh_dev_t *x = &d->dev[idx];
    d->st.lumi_frames++;
    const int ld = lumi_of(x);
    if (ld < 0) {
        d->st.lumi_unknown++;
        return;
    }
    lumi_in_t in[LUMI_IN_MAX];
    bool bad = false;
    const unsigned k = tlv_collect(s, n, in, 0, LUMI_IN_MAX, &bad);
    if (bad) d->st.report_bad++;
    st_lumi_apply(d, idx, ld, in, k, now);
}

/* 0xFCC0 簇（Aqara 私有，厂商专用帧）：属性上报 / 读属性应答。0x00F7 是一串 TLV，别的属性按属性号查表 */
static void st_lumi(zh_dev_core_t *d, unsigned idx, const zh_zcl_frame_t *f, uint32_t now)
{
    if ((f->fc & ZH_ZCL_FC_TYPE_MASK) != 0 || !(f->fc & ZH_ZCL_FC_S2C) || (f->cmd != ZH_ZCL_CMD_REPORT_ATTR && f->cmd != ZH_ZCL_CMD_READ_ATTR_RSP)) return;
    zh_dev_t *x = &d->dev[idx];
    d->st.lumi_frames++;
    const int ld = lumi_of(x);
    if (ld < 0) {
        d->st.lumi_unknown++;
        return;
    }
    lumi_in_t in[LUMI_IN_MAX];
    unsigned k = 0;
    bool bad = false;
    zh_zcl_iter_t it;
    zh_zcl_attr_t a;
    int r;
    zh_zcl_iter_init(&it, f);
    while ((r = f->cmd == ZH_ZCL_CMD_REPORT_ATTR ? zh_zcl_next_report_rec(&it, &a) : zh_zcl_next_rsp_rec(&it, &a)) == 1) {
        if (a.status != ZH_ZCL_ST_SUCCESS) continue;
        if (a.id == ZH_ATTR_LUMI_F7 && (a.type == 0x41 || a.type == 0x42) && a.len >= 1 && a.val[0] != 0xFF) {
            k = tlv_collect(a.val + 1, a.val[0] <= a.len - 1 ? a.val[0] : a.len - 1, in, k, LUMI_IN_MAX, &bad);
        } else {
            int32_t v;
            if (k < LUMI_IN_MAX && zh_lumi_val_int(a.type, a.val, a.len, &v)) in[k++] = (lumi_in_t){ ZH_CL_LUMI, a.id, v };
        }
    }
    if (r < 0 || bad) d->st.report_bad++;
    st_lumi_apply(d, idx, ld, in, k, now);
}

/* 小米人体：到了清零时刻还没再报"有人"就报没人（同 z2m occupancy_timeout） */
static void occ_tick(zh_dev_core_t *d, uint32_t now)
{
    for (unsigned i = 0; i < ZH_DEV_MAX; i++) {
        zh_dev_t *x = &d->dev[i];
        if (!x->used || !x->t_occ_clear || !expired(now, x->t_occ_clear)) continue;
        x->t_occ_clear = 0;
        st_occ(d, i, 0, now);
    }
}

/* ---------------------------------------------------------------- 可用性（设计 8.10；M5 第十一步；常量与规则见 zh_dev.h） */

/* 这次开机一次都没听到过的电池设备，网 UP 满 25 h 还没听到：列表改成离线，但不出事件、不推送（2026-10-23）。
   原先这种设备永远不判 —— 开机前就没电的传感器会一直显示在线；而照常出事件又会在开机 25 h 后成批推送（8.10 注记 ②）。
   "掉线"其实发生在开机之前、我们没看到它发生，所以静默改；它之后说话了也静默恢复（av_quiet） */
static void av_offline_quiet(zh_dev_core_t *d, unsigned idx)
{
    zh_dev_t *x = &d->dev[idx];
    x->offline = true;
    x->av_quiet = true;
    d->st.av_quiet++;
    d->dirty = true;
}

static void av_offline(zh_dev_core_t *d, unsigned idx)
{
    zh_dev_t *x = &d->dev[idx];
    x->offline = true;
    x->av_quiet = false;
    x->av_miss = 0;
    x->t_av_ping = 0;
    d->st.av_offline++;
    d->dirty = true;                               /* 事件之前置：zhub.c 发事件前发布快照 */
    dev_event(d, ZH_DEV_EV_OFFLINE, ZH_DEV_SRC_PASSIVE, idx);
}

/* 记录里第一个输出簇含 cl 的端点下标；没有 -1 */
static int ep_with_out(const zh_dev_rec_t *r, uint16_t cl)
{
    for (unsigned k = 0; k < r->n_ep && k < ZH_DEV_EP_REC; k++) {
        const unsigned n_in = r->ep[k].n_in < ZH_DEV_EP_CL ? r->ep[k].n_in : ZH_DEV_EP_CL;
        const unsigned n_out = r->ep[k].n_out < ZH_DEV_EP_CL - n_in ? r->ep[k].n_out : ZH_DEV_EP_CL - n_in;
        for (unsigned j = n_in; j < n_in + n_out; j++)
            if (r->ep[k].cl[j] == cl) return (int)k;
    }
    return -1;
}

/* 遥控器 / 按键类：输出簇里有开关 / 亮度 / 场景、输入簇没有开关（同 hub_zcap 判 button 的规则） */
static bool av_remote(const zh_dev_rec_t *r)
{
    return ep_with_in(r, ZH_CL_ONOFF) < 0 && (ep_with_out(r, ZH_CL_ONOFF) >= 0 || ep_with_out(r, ZH_CL_LEVEL) >= 0 || ep_with_out(r, 0x0005) >= 0);
}

/* 读 Basic 0x0000（ZCL 版本）当 ping：发到有 Basic 的端点（没访谈完的发端点 1）。应答 / 默认应答都会经 zh_dev_on_rx 刷新 t_seen */
static bool av_ping(zh_dev_core_t *d, zh_dev_t *x)
{
    const int e = ep_with_in(&x->rec, ZH_CL_BASIC);
    const uint8_t ep = e >= 0 ? x->rec.ep[e].ep : x->rec.n_ep ? x->rec.ep[0].ep : 1u;
    static const uint16_t ids[1] = { 0x0000 };
    uint8_t pl[8];
    const uint8_t seq = d->zcl_seq++;
    const size_t n = zh_zcl_enc_read_attrs(pl, sizeof(pl), seq, ids, 1);
    const zh_ezsp_aps_t aps = { .profile_id = ZH_HA_PROFILE, .cluster_id = ZH_CL_BASIC, .src_ep = ZH_DEV_SRC_EP, .dst_ep = ep,
                                .options = ZH_APS_OPTS_DEFAULT, .group_id = 0, .sequence = 0 };
    uint8_t b[48];
    const size_t len = n ? zh_ezsp_enc_send_unicast(b, sizeof(b), 0, ZH_OUTGOING_DIRECT, x->rec.nwk, &aps, seq, (uint8_t)n, pl) : 0;
    return len && d->ops.send && d->ops.send(d->ops.ctx, b, len, ZH_DEV_TAG_KIND_MISC, false);
}

static void av_tick(zh_dev_core_t *d, uint32_t now)
{
    if (!d->net_up) {                              /* 链路复位 / NCP 重启 / 网没起：听不到谁都不说明它掉了（不然常电设备会被成批误判离线 —— Z32 ⚠️） */
        d->av_paused = true;
        return;
    }
    if (d->av_paused || !d->t_net_up) {            /* 网刚恢复：从这一刻重新起算 */
        d->av_paused = false;
        d->t_net_up = now ? now : 1u;
    }
    /* t_net_up 只在恢复那一拍写：网连着 UP 超过 2^31 ms（24.8 天）后，32 位差值变号、谁都判不出离线（审计 ❌）。
       恢复的宽限只管 25 h，更早的封在 now − 2 × 25 h（同 ZH_DEV_AGE_CAP_MS 的做法） */
    if ((int32_t)(now - d->t_net_up) > (int32_t)(2u * ZH_AV_BATT_SILENT_MS)) d->t_net_up = now - 2u * ZH_AV_BATT_SILENT_MS;
    bool sent = false;
    for (unsigned i = 0; i < ZH_DEV_MAX; i++) {
        zh_dev_t *x = &d->dev[i];
        if (!x->used || x->offline || (x->rec.flags & ZH_DEVF_LEFT)) continue;
        const uint32_t base = (int32_t)(x->t_seen - d->t_net_up) > 0 ? x->t_seen : d->t_net_up;
        const int32_t age = (int32_t)(now - base);  /* < 0：回调里写的时刻比 tick 的晚，是刚收到 */
        const bool rx_on = (x->rec.flags & (ZH_DEVF_ANNCED | ZH_DEVF_RX_ON)) == (ZH_DEVF_ANNCED | ZH_DEVF_RX_ON) || x->rec.node_type == ZH_NODE_ROUTER;
        if (!rx_on) {                              /* 电池 / 不知道是不是休眠的：只看静默多久，不 ping */
            /* 这次开机还没听到过的不判（开机 / 网恢复 25 h 后会成批离线、冲掉事件环 —— 审计 ⚠️）；遥控器类（输出簇有开关 / 亮度 / 场景、
               输入簇没有开关）只在按的时候才说话，MAC 轮询到不了主机，不按就"离线"是误报 —— Z2M 默认关可用性也是因为这个 */
            if (!av_remote(&x->rec) && age >= (int32_t)ZH_AV_BATT_SILENT_MS) {
                if (x->seen) av_offline(d, i);
                else av_offline_quiet(d, i);        /* 这次开机一次都没听到过：静默（上面的注释） */
            }
            continue;
        }
        if (age < (int32_t)ZH_AV_MAINS_SILENT_MS) {  /* 最近说过话（含回了 ping）：这一轮作罢 */
            x->av_miss = 0;
            x->t_av_ping = 0;
            continue;
        }
        if (x->t_av_ping && (int32_t)(now - x->t_av_ping) < (int32_t)ZH_AV_PING_GAP_MS) continue;   /* 等这次 ping 的回音 */
        if (x->av_miss >= ZH_AV_PING_TRIES) {      /* 连发 3 次、每次等 10 s 都没回 */
            if (!x->seen && (x->disc & ZH_DISC_F_NEW)) av_offline_quiet(d, i);   /* 主动发现建的、一次都没听到过（表里的旧条目）：静默，不推"离线"（审计 ⚠️） */
            else av_offline(d, i);
            continue;
        }
        if (sent || x->rec.nwk == NWK_COORD || x->rec.nwk >= NWK_RESERVED) continue;   /* 一拍一条；短地址作废时发出去就是广播 */
        if (!av_ping(d, x)) continue;              /* 交不出去：下一拍再试，不占次数 */
        sent = true;
        x->av_miss++;
        x->t_av_ping = now ? now : 1u;
        d->st.av_pings++;
    }
}

/* 每次开机每台涂鸦设备打一轮招呼：magic packet（读 Basic，同 zhc lib/tuya.js:811-815 configureMagicPacket，从第一个端点读），
   然后 dataQuery（同 :817-820 configureQuery，发给端点 1 —— 有 0xEF00 输入簇的端点优先）。一拍一条；交不出去下一拍再试，不占次数 */
static void tuya_tick(zh_dev_core_t *d, uint32_t now)
{
    if (!d->net_up) return;
    for (unsigned i = 0; i < ZH_DEV_MAX; i++) {
        zh_dev_t *x = &d->dev[i];
        if (!x->used || x->tuya_hello >= 2 || !(x->rec.flags & ZH_DEVF_INTERVIEWED) || (x->rec.flags & ZH_DEVF_LEFT) || x->rec.n_ep == 0) continue;
        if (x->iv != ZH_IV_NONE) continue;         /* 正在配置 / 补 CIE：它这几秒归那边用 */
        if (d->ctl.active && d->ctl.idx == i) continue;   /* 控制命令在途：dataQuery 引出的全量回报会搅乱确认（审计 ⚠️） */
        if (tuya_of(x) < 0) continue;              /* 表里没有的：招呼了也解不了它的数据点 */
        if (x->rec.nwk == NWK_COORD || x->rec.nwk >= NWK_RESERVED) continue;               /* 短地址作废时发出去就是广播 */
        const bool rx_on = (x->rec.flags & (ZH_DEVF_ANNCED | ZH_DEVF_RX_ON)) == (ZH_DEVF_ANNCED | ZH_DEVF_RX_ON) || x->rec.node_type == ZH_NODE_ROUTER;
        if (!rx_on && (!x->seen || (int32_t)(now - x->t_seen) >= (int32_t)ZH_IV_AWAKE_MS)) continue;   /* 休眠的：只在它刚说过话的几秒里 */
        uint8_t pl[24];
        size_t n;
        uint16_t cl;
        uint8_t ep;
        const uint8_t seq = d->zcl_seq++;
        if (x->tuya_hello == 0) {
            static const uint16_t ids[6] = { 0x0004, 0x0000, 0x0001, 0x0005, 0x0007, 0xFFFE };   /* manufacturerName, zclVersion, appVersion, modelId, powerSource, 0xfffe */
            n = zh_zcl_enc_read_attrs(pl, sizeof(pl), seq, ids, 6);
            cl = ZH_CL_BASIC;
            ep = x->rec.ep[0].ep;
        } else {
            const int e = ep_with_in(&x->rec, ZH_CL_TUYA);
            pl[0] = ZH_ZCL_FC_CLUSTER | ZH_ZCL_FC_DIS_DR;   /* 簇专用、客户端 → 服务端、不要默认应答；无负载 */
            pl[1] = seq;
            pl[2] = ZH_TUYA_CMD_DATA_QUERY;
            n = 3;
            cl = ZH_CL_TUYA;
            ep = e >= 0 ? x->rec.ep[e].ep : 1u;
        }
        const zh_ezsp_aps_t aps = { .profile_id = ZH_HA_PROFILE, .cluster_id = cl, .src_ep = ZH_DEV_SRC_EP, .dst_ep = ep,
                                    .options = ZH_APS_OPTS_DEFAULT, .group_id = 0, .sequence = 0 };
        uint8_t b[64];
        const size_t len = n ? zh_ezsp_enc_send_unicast(b, sizeof(b), 0, ZH_OUTGOING_DIRECT, x->rec.nwk, &aps, seq, (uint8_t)n, pl) : 0;
        if (!len || !d->ops.send || !d->ops.send(d->ops.ctx, b, len, ZH_DEV_TAG_KIND_MISC, false)) return;
        x->tuya_hello++;
        d->st.tuya_hellos++;
        return;                                    /* 一拍一条 */
    }
}

/* ZCL 消息进读数：属性上报 / 读属性应答、IAS 的 Zone Status Change 通知与登记请求 */
static void st_on_zcl(zh_dev_core_t *d, uint16_t sender, uint16_t cluster, uint8_t src_ep, const uint8_t *pl, size_t len, uint32_t now)
{
    const int i = zh_dev_find_nwk(d, sender);
    if (i < 0) return;
    const unsigned idx = (unsigned)i;
    zh_zcl_frame_t f;
    if (zh_zcl_parse(pl, len, &f) != ZH_ZCL_OK) {
        d->st.report_bad++;
        return;
    }
    if (cluster == ZH_CL_TUYA) {                   /* 涂鸦私有簇：不带厂商位，也有带的 —— 都照收（在"厂商专用的不认"之前分流） */
        st_tuya(d, idx, &f, now);
        return;
    }
    if (cluster == ZH_CL_LUMI) {                   /* Aqara 私有簇：厂商专用帧（0x115F），同样在"厂商专用的不认"之前分流 */
        st_lumi(d, idx, &f, now);
        return;
    }
    const bool global = (f.fc & ZH_ZCL_FC_TYPE_MASK) == 0;
    if (f.fc & ZH_ZCL_FC_MFG) {                    /* 厂商专用的：只认小米在 Basic 上发的那种（0xFF01 心跳、配对时的型号；帧控制带厂商位、厂商码 0x115F / 0x1234 —— 审计 ❌：
                                                      之前整帧丢，真设备上 TLV 与学型号都不生效），别的不认识 */
        if (cluster != ZH_CL_BASIC || !global || (f.cmd != ZH_ZCL_CMD_REPORT_ATTR && f.cmd != ZH_ZCL_CMD_READ_ATTR_RSP)) return;
        zh_zcl_iter_t it;
        zh_zcl_attr_t a;
        int r;
        zh_zcl_iter_init(&it, &f);
        while ((r = f.cmd == ZH_ZCL_CMD_REPORT_ATTR ? zh_zcl_next_report_rec(&it, &a) : zh_zcl_next_rsp_rec(&it, &a)) == 1)
            if (a.status == ZH_ZCL_ST_SUCCESS && (a.id == ZH_BASIC_MANUF || a.id == ZH_BASIC_MODEL || a.id == ZH_ATTR_LUMI_FF01)) st_apply(d, idx, cluster, &a, now);
        if (r < 0) d->st.report_bad++;
        return;
    }
    if (global && (f.cmd == ZH_ZCL_CMD_REPORT_ATTR || f.cmd == ZH_ZCL_CMD_READ_ATTR_RSP)) {
        zh_zcl_iter_t it;
        zh_zcl_attr_t a;
        int r;
        zh_zcl_iter_init(&it, &f);
        while ((r = f.cmd == ZH_ZCL_CMD_REPORT_ATTR ? zh_zcl_next_report_rec(&it, &a) : zh_zcl_next_rsp_rec(&it, &a)) == 1)
            if (a.status == ZH_ZCL_ST_SUCCESS) st_apply(d, idx, cluster, &a, now);
        if (r < 0) d->st.report_bad++;
        return;
    }
    if (global || cluster != ZH_CL_IAS_ZONE || !(f.fc & ZH_ZCL_FC_S2C)) return;
    if (f.cmd == ZH_IAS_CMD_STATUS_CHANGE) {       /* [状态 2 B][扩展状态][zone id][延时 2 B]：只要前两个字节 */
        if (f.n < 2) {
            d->st.report_bad++;
            return;
        }
        st_zone(d, idx, (uint16_t)(f.pl[0] | (f.pl[1] << 8)), now);
    } else if (f.cmd == ZH_IAS_CMD_ENROLL_REQ) {   /* [ZoneType 2 B][厂商码 2 B] */
        zh_dev_t *x = &d->dev[idx];
        d->st.enroll_reqs++;
        const uint16_t zt = f.n >= 2 ? (uint16_t)(f.pl[0] | (f.pl[1] << 8)) : ZH_ZONE_TYPE_UNKNOWN;
        if (zt != ZH_ZONE_TYPE_UNKNOWN && x->rec.zone_type == ZH_ZONE_TYPE_UNKNOWN && (x->rec.flags & ZH_DEVF_INTERVIEWED)) {
            x->rec.zone_type = zt;                 /* 访谈没读到 ZoneType 的：登记请求里有，补上（漏水 / 烟雾才认得出来该推） */
            dev_save(d, idx);
        }
        if (st_gap_ok(&x->t_enroll, ZH_IAS_ENROLL_GAP_MS, now)) (void)ias_enroll_rsp(d, idx, src_ep);   /* 丢了它会再发请求 */
        cie_resume(d, idx, ZH_CIE_RESUME_GAP_MS);  /* 欠着 CIE / 登记的：它正醒着想登记，补上 */
    }
}

/* ---------------------------------------------------------------- 入口 */

bool zh_dev_load(zh_dev_core_t *d, unsigned idx, const zh_dev_rec_t *rec, uint32_t now_ms)
{
    if (!d || !rec || idx >= ZH_DEV_MAX || d->dev[idx].used || d->dev[idx].reserved) return false;
    if (rec->ver != ZH_DEV_REC_VER || !eui_valid(rec->ieee) || zh_dev_find_ieee(d, rec->ieee) >= 0) return false;
    zh_dev_t *x = &d->dev[idx];
    memset(x, 0, sizeof(*x));
    x->used = true;
    x->rec = *rec;
    x->t_seen = now_ms;                            /* 状态不落盘：可用性从开机起算（8.9） */
    if (!(x->rec.flags & ZH_DEVF_INTERVIEWED) && !(x->rec.flags & ZH_DEVF_LEFT)) iv_start(d, idx, false, now_ms);
    d->dirty = true;
    return true;
}

bool zh_dev_reserve(zh_dev_core_t *d, unsigned idx)
{
    if (!d || idx >= ZH_DEV_MAX || d->dev[idx].used || d->dev[idx].reserved) return false;
    d->dev[idx].reserved = true;
    d->st.reserved++;
    return true;
}

void zh_dev_set_net_up(zh_dev_core_t *d, bool up)
{
    if (d) d->net_up = up;
}

void zh_dev_set_ext_pan(zh_dev_core_t *d, const uint8_t xpan[8])
{
    if (!d) return;
    static const uint8_t zero[8];
    d->have_xpan = xpan && memcmp(xpan, zero, 8) != 0;   /* 全 0 = 没读到 */
    if (d->have_xpan) memcpy(d->xpan, xpan, 8);
}

void zh_dev_set_own_eui(zh_dev_core_t *d, const uint8_t eui[8])
{
    if (!d) return;
    const bool had = d->have_own_eui;
    d->have_own_eui = eui && eui_valid(eui);       /* 全 0 / 全 FF 不认（写进设备就是让它往一个不存在的地址报） */
    if (d->have_own_eui) memcpy(d->own_eui, eui, 8);
    if (!had && d->have_own_eui)                   /* 刚知道：之前因为不知道而欠着的（含开机读回的欠着的）都排上 */
        for (unsigned i = 0; i < ZH_DEV_MAX; i++)
            if (d->dev[i].used) cie_resume(d, i, 0);
}

void zh_dev_on_rx(zh_dev_core_t *d, uint16_t sender, int8_t rssi, uint8_t lqi, uint32_t now_ms)
{
    if (!d) return;
    d->t_now = now_ms;
    const int idx = zh_dev_find_nwk(d, sender);
    if (idx >= 0) {
        zh_dev_t *x = &d->dev[idx];
        mark_seen(x, rssi, lqi, true, now_ms);
        x->rx++;
        x->av_miss = 0;
        x->t_av_ping = 0;
        if (x->offline) {                          /* 判过离线的又说话了：恢复（8.10） */
            x->offline = false;
            d->dirty = true;
            if (x->av_quiet) {
                x->av_quiet = false;               /* 静默判的离线：静默恢复（没推过"离线"，推"恢复"会没头没尾） */
            } else {
                d->st.av_online++;
                dev_event(d, ZH_DEV_EV_ONLINE, ZH_DEV_SRC_PASSIVE, (unsigned)idx);
            }
        }
        if (x->iv_new) x->iv_t_new = now_ms;       /* 刚入网的还在说话 = 还在快速轮询：醒着的窗口顺延（几台同时入网时访谈 / 配置排得开 —— 第二轮审计） */
        if (x->iv == ZH_IV_FAILED && now_ms - x->iv_t_fail >= ZH_IV_RETRY_MS) iv_start(d, (unsigned)idx, false, now_ms);   /* 失败够久了：再试 */
        else if (x->rec.flags & ZH_DEVF_CIE_OWED) cie_resume(d, (unsigned)idx, ZH_IV_RETRY_MS);   /* 欠着 CIE / 登记够久了：它正醒着，补 */
        d->dirty = true;
        return;
    }
    d->st.unknown_rx++;
    start_learn(d, sender, rssi, lqi, now_ms);     /* 不认识（或标过 left 的旧地址）：问它是谁 */
}

void zh_dev_on_tc_join(zh_dev_core_t *d, uint16_t node, const uint8_t eui[8], uint8_t status, uint8_t decision, uint16_t parent, uint32_t now_ms)
{
    (void)parent;
    if (!d || !eui) return;
    d->t_now = now_ms;
    if (decision == ZH_JOIN_DENY) return;          /* 信任中心拒了：不建记录（第六步两轮审计 ❌ 的判据） */
    if (status == ZH_JOIN_DEVICE_LEFT) {
        const int idx = zh_dev_find_ieee(d, eui);
        if (idx < 0 || (d->dev[idx].rec.flags & ZH_DEVF_LEFT)) return;
        d->dev[idx].rec.flags |= ZH_DEVF_LEFT;     /* 只标 left，不删（8.6） */
        iv_release(d, (unsigned)idx);
        d->dev[idx].iv = ZH_IV_NONE;               /* 访谈停下；回来了（BACK）没访谈完的再来 */
        d->dev[idx].iv_resume = false;             /* 补做中离开：欠着的标志还在，回来（重新入网）时再补 */
        if (d->dev[idx].cq.on) d->st.ctlq_dropped++;
        memset(&d->dev[idx].cq, 0, sizeof(d->dev[idx].cq));   /* 排着的命令作废：离开的多半是恢复出厂，别让它重新配对时收到旧命令 */
        d->st.left++;
        dev_save(d, (unsigned)idx);
        dev_event(d, ZH_DEV_EV_LEFT, ZH_DEV_SRC_JOIN, (unsigned)idx);
        return;
    }
    const bool join = status == ZH_JOIN_UNSECURED_JOIN && decision <= ZH_JOIN_SEND_KEY_IN_CLEAR;
    const bool rejoin = status == ZH_JOIN_SECURED_REJOIN || status == ZH_JOIN_UNSECURED_REJOIN;
    if (!join && !rejoin) return;
    zh_dev_look_t *L = look_find(d, node);
    if (L) L->stage = 0;                           /* 正在查这个 nwk：不用查了 */
    const uint32_t added = d->st.added;
    const int idx = learn(d, node, eui, join ? ZH_DEV_SRC_JOIN : ZH_DEV_SRC_REJOIN, NULL, now_ms);
    if (idx < 0) return;
    mark_seen(&d->dev[idx], 0, 0, false, now_ms);
    if (join && d->st.added == added) {            /* 已有记录的设备重新配对（恢复出厂过、可能换了固件）：重新访谈 */
        d->dev[idx].rec.flags &= (uint8_t)~ZH_DEVF_INTERVIEWED;
        memset(&d->dev[idx].st, 0, sizeof(d->dev[idx].st));   /* 读数也清：旧的 zone_status 会吞掉新设备的头一次报警 */
        d->dev[idx].batt_low = false;              /* 电量低的回差同理：换了电池重新配对的，再低要再报 */
        d->dev[idx].tbl_zs = 0;                   /* 涂鸦合成的 ZoneStatus 同理；重新配对的要重新打招呼（magic packet 就是配对时要的） */
        d->dev[idx].t_occ_clear = 0;
        d->dev[idx].tuya_hello = 0;
        if (d->dev[idx].cq.on) d->st.ctlq_dropped++;
        memset(&d->dev[idx].cq, 0, sizeof(d->dev[idx].cq));   /* 排着的命令是给旧那台的 */
        iv_start(d, (unsigned)idx, true, now_ms);
        dev_save(d, (unsigned)idx);                /* "没访谈过"立刻落盘：这次访谈失败、下次开机才会再访谈（不然读回的还是旧的"已访谈"，第一轮审计） */
    }
}

void zh_dev_on_zdo(zh_dev_core_t *d, uint16_t sender, uint16_t cluster, const uint8_t *pl, size_t len, uint32_t now_ms)
{
    if (!d || !pl) return;
    d->t_now = now_ms;
    if (cluster == ZH_ZDO_LQI_TABLE_RSP) {
        disc_on_rsp(d, sender, pl, len, now_ms);
        return;
    }
    if (iv_on_zdo(d, sender, cluster, pl, len)) return;
    if (cluster == 0x0013u && len >= 12) {         /* Device_annce [序号][nwk:2][IEEE:8][能力:1] */
        const uint16_t nwk = (uint16_t)(pl[1] | (pl[2] << 8));
        const uint8_t cap = pl[11];
        if (nwk == NWK_COORD || nwk >= NWK_RESERVED) {   /* 空口来的：协调器自己 / 广播保留段不可能是宣告的设备（S1，第一轮审计） */
            d->st.bad_nwk++;
            return;
        }
        zh_dev_look_t *L = look_find(d, nwk);
        if (L) L->stage = 0;
        const uint8_t capf = (uint8_t)(ZH_DEVF_ANNCED | ((cap & ZH_CAP_RX_ON_IDLE) ? ZH_DEVF_RX_ON : 0) | ((cap & ZH_CAP_MAINS) ? ZH_DEVF_MAINS : 0));
        const uint8_t type = (cap & ZH_CAP_FFD) ? ZH_NODE_ROUTER : ZH_NODE_END_DEVICE;
        const learn_caps_t lc = { type, capf };
        const int idx = learn(d, nwk, pl + 3, ZH_DEV_SRC_ANNCE, &lc, now_ms);
        if (idx < 0) return;
        zh_dev_t *x = &d->dev[idx];
        mark_seen(x, 0, 0, false, now_ms);
        if (x->iv == ZH_IV_FAILED) {               /* 访谈失败过的重新宣告（多半刚上电）：马上重来；这一下多半又赶在它就绪之前，失败了从 30 s 那档算（审计 ⚠️） */
            x->iv_fails = 0;
            iv_start(d, (unsigned)idx, false, now_ms);
        }
        else cie_resume(d, (unsigned)idx, ZH_CIE_RESUME_GAP_MS);   /* 欠着 CIE / 登记的：它刚宣告、多半醒着，补上 */
        const uint8_t flags = (uint8_t)((x->rec.flags & ~(ZH_DEVF_RX_ON | ZH_DEVF_MAINS)) | capf);
        if (flags != x->rec.flags || type != x->rec.node_type) {
            x->rec.flags = flags;
            x->rec.node_type = type;
            d->st.updated++;
            dev_save(d, (unsigned)idx);
        }
        return;
    }
    if (cluster == ZH_ZDO_IEEE_ADDR_RSP && len >= 12) {   /* [序号][状态][IEEE:8][nwk:2]… */
        const uint8_t seq = pl[0], status = pl[1];
        const uint16_t nwk = (uint16_t)(pl[10] | (pl[11] << 8));
        for (unsigned i = 0; i < ZH_DEV_LOOK_N; i++) {
            zh_dev_look_t *L = &d->look[i];
            if (L->stage != 2 || L->zdo_seq != seq || L->nwk != nwk) continue;
            L->stage = 0;
            if (status != 0 || !eui_valid(pl + 2)) {
                d->st.learn_fails++;
                return;
            }
            d->st.ieee_rsps++;
            const int idx = learn(d, nwk, pl + 2, ZH_DEV_SRC_PASSIVE, NULL, now_ms);
            if (idx >= 0) mark_seen(&d->dev[idx], 0, 0, false, now_ms);
            return;
        }
    }
    (void)sender;
}

void zh_dev_on_zcl(zh_dev_core_t *d, uint16_t sender, uint16_t profile, uint16_t cluster, uint8_t src_ep, const uint8_t *pl, size_t len, uint32_t now_ms)
{
    if (!d || !pl || profile == 0) return;
    d->t_now = now_ms;
    iv_on_zcl(d, sender, cluster, pl, len);        /* 访谈的应答按发送方 + 簇 + ZCL 序号配，端点不看（有的设备从别的端点回） */
    ctl_on_zcl(d, sender, cluster, pl, len, now_ms);   /* 控制命令的默认应答 / 状态上报（上报照样往下进读数） */
    st_on_zcl(d, sender, cluster, src_ep, pl, len, now_ms);
}

bool zh_dev_on_app_result(zh_dev_core_t *d, uint32_t tag, zh_ses_result_t r, const zh_ezsp_frame_t *fr, uint32_t now_ms)
{
    if (!d) return false;
    d->t_now = now_ms;
    const uint32_t kind = tag & 0x0Fu;
    if (kind == ZH_DEV_TAG_KIND_MISC && tag <= ZH_DEV_TAG_MAX) return true;   /* 不等结果的（IAS 登记应答、补读电量） */
    if (kind == ZH_DEV_TAG_KIND_DISC && tag <= ZH_DEV_TAG_MAX) {   /* 主动发现：NCP 拒了（缺缓冲 / 网络刚掉）就别干等 5 s */
        zh_disc_t *s = &d->disc;
        if (!s->inflight || (uint16_t)(tag >> 8) != s->gen) return true;   /* 不是在途那条的 */
        uint32_t st = 1;
        if (r == ZH_SES_R_OK && zh_ezsp_dec_send(fr, ZH_EZSP_ID_SEND_UNICAST, &st, NULL) == ZH_EZSP_OK && st == 0) return true;   /* 发出去了：等应答 */
        d->st.disc_send_fail++;
        disc_try_failed(d, now_ms);
        return true;
    }
    if (kind == ZH_DEV_TAG_KIND_QCTL && tag <= ZH_DEV_TAG_MAX) {   /* 休眠设备的补发：sendUnicast 被拒 = 没发出去，下次醒来再发 */
        const unsigned idx = (tag >> 8) & 0xFFu;
        if (idx >= ZH_DEV_MAX) return true;
        zh_ctlq_t *q = &d->dev[idx].cq;
        uint32_t st = 1;
        if (q->on && q->inflight && q->gen == (uint8_t)(tag >> 16) &&
            !(r == ZH_SES_R_OK && zh_ezsp_dec_send(fr, ZH_EZSP_ID_SEND_UNICAST, &st, NULL) == ZH_EZSP_OK && st == 0)) q->inflight = false;
        return true;
    }
    if (kind == ZH_DEV_TAG_KIND_CTL && tag <= ZH_DEV_TAG_MAX) {
        zh_ctl_t *c = &d->ctl;
        if (!c->active || c->result != ZH_CTL_R_NONE || (uint16_t)(tag >> 8) != c->gen) return true;   /* 不是在途那条的 */
        uint32_t st = 1;
        if (r == ZH_SES_R_OK && zh_ezsp_dec_send(fr, ZH_EZSP_ID_SEND_UNICAST, &st, NULL) == ZH_EZSP_OK && st == 0) return true;   /* NCP 收下了：等 messageSent / 默认应答 */
        c->status = r == ZH_SES_R_OK ? st : 0xFFFFFFFFu;   /* NCP 拒了（缺缓冲 / 网络刚掉），或链路出了事结果未知 */
        ctl_finish(d, ZH_CTL_R_FAILED, now_ms);
        return true;
    }
    if (kind == ZH_DEV_TAG_KIND_IV && tag <= ZH_DEV_TAG_MAX) {
        if ((uint16_t)(tag >> 8) != d->iv_gen || !d->iv_busy) return true;   /* 不是在途那条的（之后又发了新的 / 已经超时放掉了） */
        uint32_t st = 1;
        if (r == ZH_SES_R_OK && zh_ezsp_dec_send(fr, ZH_EZSP_ID_SEND_UNICAST, &st, NULL) == ZH_EZSP_OK && st == 0) return true;   /* 发出去了：等应答 */
        d->st.iv_send_fail++;                      /* 没发出去（NCP 缺缓冲 / 网络刚掉）：这一次算用掉了，但照样等到期限再重发 —— 当场重发几百毫秒就把次数用完（第二轮审计） */
        return true;
    }
    const unsigned slot = (unsigned)((tag >> 4) & 0x0Fu);
    if ((kind != ZH_DEV_TAG_KIND_LOOKUP && kind != ZH_DEV_TAG_KIND_IEEE) || slot >= ZH_DEV_LOOK_N || tag > ZH_DEV_TAG_MAX) return false;
    zh_dev_look_t *L = &d->look[slot];
    if ((uint16_t)(tag >> 8) != L->gen) return true;   /* 上一轮的：这一格到期放掉后被别的 nwk 占了（应用命令排在队尾，可能比 5 s 还久才执行） */
    if (kind == ZH_DEV_TAG_KIND_LOOKUP) {
        if (L->stage != 1) return true;            /* 已经被入网 / 宣告认出来了，或链路复位后作废 */
        if (r != ZH_SES_R_OK) {                    /* 超时 / 断了 / 结果未知（链路多半正在复位）/ NCP 不认：放掉，它下次说话再查 */
            L->stage = 0;
            d->st.learn_fails++;
            return true;
        }
        uint32_t st = 1;
        uint8_t eui[8];
        if (zh_ezsp_dec_lookup_eui64(fr, &st, eui) == ZH_EZSP_OK && st == 0 && eui_valid(eui)) {
            d->st.lookup_hits++;
            L->stage = 0;
            const int idx = learn(d, L->nwk, eui, ZH_DEV_SRC_PASSIVE, NULL, now_ms);
            if (idx >= 0) {                        /* 触发查询的那条消息也是它发的：信号与计数记上（不然只有一台设备时列表里一直没有 link） */
                mark_seen(&d->dev[idx], L->rssi, L->lqi, true, now_ms);
                d->dev[idx].rx++;
            }
            return true;
        }
        /* NCP 的地址表里没有（或回的地址不认）：问它本人 —— 表满了就不问（问到了也建不了，空口白发一条） */
        if (!has_free(d)) {
            d->st.full++;
            L->stage = 0;
            return true;
        }
        if (send_ieee_req(d, slot)) {
            L->stage = 2;
            L->t_start = now_ms;
            d->st.ieee_reqs++;
        } else {
            d->st.send_busy++;
            d->st.learn_fails++;
            L->stage = 0;
        }
        return true;
    }
    if (L->stage != 2) return true;
    uint32_t st = 1;
    if (r != ZH_SES_R_OK || zh_ezsp_dec_send(fr, ZH_EZSP_ID_SEND_UNICAST, &st, NULL) != ZH_EZSP_OK || st != 0) {
        d->st.learn_fails++;                       /* 没发出去：放掉 */
        L->stage = 0;
    }
    return true;                                   /* 发出去了：等 IEEE_addr_rsp（zh_dev_on_zdo）或超时（zh_dev_tick） */
}

void zh_dev_tick(zh_dev_core_t *d, uint32_t now_ms)
{
    if (!d) return;
    d->t_now = now_ms;
    iv_tick(d, now_ms);
    batt_tick(d, now_ms);
    tuya_tick(d, now_ms);
    occ_tick(d, now_ms);
    av_tick(d, now_ms);
    ctlq_tick(d, now_ms);
    ctl_tick(d, now_ms);
    disc_tick(d, now_ms);
    if (now_ms - d->t_age_chk >= ZH_DEV_AGE_CHK_MS) {
        d->t_age_chk = now_ms;
        for (unsigned i = 0; i < ZH_DEV_MAX; i++) {
            zh_dev_t *x = &d->dev[i];
            const int32_t age = (int32_t)(now_ms - x->t_seen);   /* < 0：t_seen 比 now 晚（回调里取的时刻更新），是刚收到，不是很老 */
            if (x->used && age > (int32_t)ZH_DEV_AGE_CAP_MS) {
                x->t_seen = now_ms - ZH_DEV_AGE_CAP_MS;   /* 封顶：再老也只说上限那么久，别回绕成刚收到 */
                d->dirty = true;
            }
            /* 登记应答间隔的时刻也封顶：连着跑 49.7 天回绕后别把很久以前当成刚回过 */
            if (x->used && x->t_enroll && (int32_t)(now_ms - x->t_enroll) > (int32_t)ZH_DEV_AGE_CAP_MS) x->t_enroll = now_ms - ZH_DEV_AGE_CAP_MS;
            for (unsigned f = 0; x->used && f < ZH_ST_N; f++) {   /* 读数的时刻同样封顶（列表里的 state_age_s） */
                if (!(x->st.have & (1u << f)) || (int32_t)(now_ms - x->st.t[f]) <= (int32_t)ZH_DEV_AGE_CAP_MS) continue;
                x->st.t[f] = now_ms - ZH_DEV_AGE_CAP_MS;
                d->dirty = true;
            }
        }
    }
    for (unsigned i = 0; i < ZH_DEV_LOOK_N; i++) {
        zh_dev_look_t *L = &d->look[i];
        if (!L->stage) continue;
        const uint32_t wait = L->stage == 1 ? ZH_DEV_LOOKUP_WAIT_MS : ZH_DEV_IEEE_WAIT_MS;
        if (expired(now_ms, L->t_start + wait)) {
            L->stage = 0;
            d->st.learn_fails++;
        }
    }
}
