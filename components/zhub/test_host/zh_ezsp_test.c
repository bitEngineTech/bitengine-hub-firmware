/* zh_ezsp_core 的真值表（docs/15 设计 8.4）。编的是产品源文件 ../zh_ezsp_core.c。
 * 这里只测"拒什么"：头的分类、每个解码器的严格长度 / 帧 ID、编码器的参数校验与 220 字节边界。
 * "收什么、编出什么"与 SDK 逐字节 / 逐字段一致，由 sdk_interop/ezsp_oracle.c 拿 SDK 的 ezsp.c 对照。 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "zh_ezsp_core.h"

static int s_fail, s_pass;
#define CHECK(cond, ...) do { if (cond) { s_pass++; } else { s_fail++; printf("❌ %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t s_rng = 0x2545F491u;
static uint8_t rnd8(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (uint8_t)s_rng;
}

/* 拼一帧 NCP → 主机的扩展头回包：[seq][fc][0x01][id lo][id hi] + 参数 */
static size_t mk(uint8_t *f, uint8_t fc, uint16_t id, const uint8_t *par, size_t n)
{
    f[0] = 0x42;
    f[1] = fc;
    f[2] = 0x01;
    f[3] = (uint8_t)id;
    f[4] = (uint8_t)(id >> 8);
    if (n) memcpy(f + 5, par, n);
    return 5 + n;
}

/* ---- 每个解码器一个桩：null = true 时所有输出给 NULL（不要的字段） ---- */
typedef int (*dec_fn)(const zh_ezsp_frame_t *fr, bool null);
#define O(p) (null ? NULL : (p))
static int d_version(const zh_ezsp_frame_t *f, bool null) { uint8_t a, b; uint16_t c; return zh_ezsp_dec_version(f, O(&a), O(&b), O(&c)); }
static int d_get_config(const zh_ezsp_frame_t *f, bool null) { uint32_t s; uint16_t v; return zh_ezsp_dec_get_config(f, O(&s), O(&v)); }
static int d_get_value(const zh_ezsp_frame_t *f, bool null) { uint32_t s; uint8_t l; const uint8_t *v; return zh_ezsp_dec_get_value(f, O(&s), O(&l), O(&v)); }
static int d_custom_frame(const zh_ezsp_frame_t *f, bool null) { uint32_t s; uint8_t l; const uint8_t *v; return zh_ezsp_dec_custom_frame(f, O(&s), O(&l), O(&v)); }
static int d_get_eui64(const zh_ezsp_frame_t *f, bool null) { uint8_t e[8]; return zh_ezsp_dec_get_eui64(f, O(e)); }
static int d_get_node_id(const zh_ezsp_frame_t *f, bool null) { uint16_t v; return zh_ezsp_dec_get_node_id(f, O(&v)); }
static int d_network_state(const zh_ezsp_frame_t *f, bool null) { uint8_t v; return zh_ezsp_dec_network_state(f, O(&v)); }
static int d_srd_mode(const zh_ezsp_frame_t *f, bool null) { uint32_t v; return zh_ezsp_dec_source_route_discovery_mode(f, O(&v)); }
static int d_get_net_params(const zh_ezsp_frame_t *f, bool null) { uint32_t s; uint8_t t; zh_ezsp_net_params_t p; return zh_ezsp_dec_get_network_parameters(f, O(&s), O(&t), O(&p)); }
static int d_send_uni(const zh_ezsp_frame_t *f, bool null) { uint32_t s; uint8_t q; return zh_ezsp_dec_send(f, ZH_EZSP_ID_SEND_UNICAST, O(&s), O(&q)); }
static int d_send_bc(const zh_ezsp_frame_t *f, bool null) { uint32_t s; uint8_t q; return zh_ezsp_dec_send(f, ZH_EZSP_ID_SEND_BROADCAST, O(&s), O(&q)); }
static int d_lookup_eui64(const zh_ezsp_frame_t *f, bool null) { uint32_t s; uint8_t e[8]; return zh_ezsp_dec_lookup_eui64(f, O(&s), O(e)); }
static int d_lookup_node_id(const zh_ezsp_frame_t *f, bool null) { uint32_t s; uint16_t v; return zh_ezsp_dec_lookup_node_id(f, O(&s), O(&v)); }
static int d_cur_sec(const zh_ezsp_frame_t *f, bool null) { uint32_t s; uint16_t b; uint8_t e[8]; return zh_ezsp_dec_get_current_security_state(f, O(&s), O(&b), O(e)); }
static int d_stack_status(const zh_ezsp_frame_t *f, bool null) { uint32_t s; return zh_ezsp_dec_stack_status(f, O(&s)); }
static int d_energy(const zh_ezsp_frame_t *f, bool null) { uint8_t c; int8_t r; return zh_ezsp_dec_energy_scan_result(f, O(&c), O(&r)); }
static int d_scan_complete(const zh_ezsp_frame_t *f, bool null) { uint8_t c; uint32_t s; return zh_ezsp_dec_scan_complete(f, O(&c), O(&s)); }
static int d_child_join(const zh_ezsp_frame_t *f, bool null) { uint8_t i, e[8], t; bool j; uint16_t c; return zh_ezsp_dec_child_join(f, O(&i), O(&j), O(&c), O(e), O(&t)); }
static int d_tc_post_join(const zh_ezsp_frame_t *f, bool null) { uint16_t n, p; uint8_t e[8], s, d; return zh_ezsp_dec_tc_post_join(f, O(&n), O(e), O(&s), O(&d), O(&p)); }
static int d_msg_sent(const zh_ezsp_frame_t *f, bool null)
{
    uint32_t s; uint8_t t, l; uint16_t d, g; zh_ezsp_aps_t a; const uint8_t *m;
    return zh_ezsp_dec_message_sent(f, O(&s), O(&t), O(&d), O(&a), O(&g), O(&l), O(&m));
}
static int d_incoming(const zh_ezsp_frame_t *f, bool null)
{
    uint8_t t, l; zh_ezsp_aps_t a; zh_ezsp_rx_info_t ri; const uint8_t *m;
    uint8_t ov;
    return zh_ezsp_dec_incoming_message(f, O(&t), O(&a), O(&ri), O(&l), O(&m), O(&ov));
}
static int d_route_record(const zh_ezsp_frame_t *f, bool null)
{
    uint16_t s; uint8_t e[8], q, c; int8_t r; const uint8_t *rl;
    return zh_ezsp_dec_incoming_route_record(f, O(&s), O(e), O(&q), O(&r), O(&c), O(&rl));
}
static int d_id_conflict(const zh_ezsp_frame_t *f, bool null) { uint16_t v; return zh_ezsp_dec_id_conflict(f, O(&v)); }
static int d_cf_handler(const zh_ezsp_frame_t *f, bool null) { uint8_t l; const uint8_t *p; return zh_ezsp_dec_custom_frame_handler(f, O(&l), O(&p)); }
#define DSTAT(name, ID) static int name(const zh_ezsp_frame_t *f, bool null) { uint32_t s; return zh_ezsp_dec_status(f, ID, O(&s)); }
DSTAT(d_st_set_config, ZH_EZSP_ID_SET_CONFIGURATION_VALUE)
DSTAT(d_st_add_ep, ZH_EZSP_ID_ADD_ENDPOINT)
DSTAT(d_st_set_policy, ZH_EZSP_ID_SET_POLICY)
DSTAT(d_st_set_value, ZH_EZSP_ID_SET_VALUE)
DSTAT(d_st_net_init, ZH_EZSP_ID_NETWORK_INIT)
DSTAT(d_st_start_scan, ZH_EZSP_ID_START_SCAN)
DSTAT(d_st_form, ZH_EZSP_ID_FORM_NETWORK)
DSTAT(d_st_leave, ZH_EZSP_ID_LEAVE_NETWORK)
DSTAT(d_st_permit, ZH_EZSP_ID_PERMIT_JOINING)
DSTAT(d_st_radio_power, ZH_EZSP_ID_SET_RADIO_POWER)
DSTAT(d_st_concentrator, ZH_EZSP_ID_SET_CONCENTRATOR)
DSTAT(d_st_init_sec, ZH_EZSP_ID_SET_INITIAL_SECURITY_STATE)
static int d_e_nop(const zh_ezsp_frame_t *f, bool null) { (void)null; return zh_ezsp_dec_empty(f, ZH_EZSP_ID_NOP); }
static int d_e_clear(const zh_ezsp_frame_t *f, bool null) { (void)null; return zh_ezsp_dec_empty(f, ZH_EZSP_ID_CLEAR_TRANSIENT_LINK_KEYS); }
static int d_e_nocb(const zh_ezsp_frame_t *f, bool null) { (void)null; return zh_ezsp_dec_empty(f, ZH_EZSP_ID_NO_CALLBACKS); }

/* 参数布局（command-functions.h 的 fetch 顺序，按字节数手算，不看被测代码）：
   fixed = 定长部分（含长度字节）；len_at = 长度字节在参数里的偏移（-1 = 定长）；unit = 每单位字节数；tail = 变长部分之后还跟几个字节 */
typedef struct { const char *name; uint16_t id; dec_fn dec; size_t fixed; int len_at; size_t unit; size_t tail; } layout_t;
static const layout_t k_lay[] = {
    { "version", ZH_EZSP_ID_VERSION, d_version, 4, -1, 0, 0 },                          /* u8 u8 u16 */
    { "get_config", ZH_EZSP_ID_GET_CONFIGURATION_VALUE, d_get_config, 6, -1, 0, 0 },    /* u32 u16 */
    { "get_value", ZH_EZSP_ID_GET_VALUE, d_get_value, 5, 4, 1, 0 },                     /* u32 len bytes */
    { "custom_frame", ZH_EZSP_ID_CUSTOM_FRAME, d_custom_frame, 5, 4, 1, 0 },
    { "get_eui64", ZH_EZSP_ID_GET_EUI64, d_get_eui64, 8, -1, 0, 0 },
    { "get_node_id", ZH_EZSP_ID_GET_NODE_ID, d_get_node_id, 2, -1, 0, 0 },
    { "network_state", ZH_EZSP_ID_NETWORK_STATE, d_network_state, 1, -1, 0, 0 },
    { "set_source_route_discovery_mode", ZH_EZSP_ID_SET_SOURCE_ROUTE_DISCOVERY_MODE, d_srd_mode, 4, -1, 0, 0 },   /* u32 剩余毫秒 */
    { "get_network_parameters", ZH_EZSP_ID_GET_NETWORK_PARAMETERS, d_get_net_params, 25, -1, 0, 0 },   /* u32 u8 + 8+2+1+1+1+2+1+4 */
    { "send_unicast", ZH_EZSP_ID_SEND_UNICAST, d_send_uni, 5, -1, 0, 0 },
    { "send_broadcast", ZH_EZSP_ID_SEND_BROADCAST, d_send_bc, 5, -1, 0, 0 },
    { "lookup_eui64", ZH_EZSP_ID_LOOKUP_EUI64_BY_NODE_ID, d_lookup_eui64, 12, -1, 0, 0 },
    { "lookup_node_id", ZH_EZSP_ID_LOOKUP_NODE_ID_BY_EUI64, d_lookup_node_id, 6, -1, 0, 0 },
    { "get_current_security_state", ZH_EZSP_ID_GET_CURRENT_SECURITY_STATE, d_cur_sec, 14, -1, 0, 0 },   /* u32 u16 eui */
    { "stack_status", ZH_EZSP_ID_STACK_STATUS_HANDLER, d_stack_status, 4, -1, 0, 0 },
    { "energy_scan_result", ZH_EZSP_ID_ENERGY_SCAN_RESULT_HANDLER, d_energy, 2, -1, 0, 0 },
    { "scan_complete", ZH_EZSP_ID_SCAN_COMPLETE_HANDLER, d_scan_complete, 5, -1, 0, 0 },
    { "child_join", ZH_EZSP_ID_CHILD_JOIN_HANDLER, d_child_join, 13, -1, 0, 0 },          /* u8 u8 u16 eui u8 */
    { "tc_post_join", ZH_EZSP_ID_TRUST_CENTER_POST_JOIN_HANDLER, d_tc_post_join, 14, -1, 0, 0 },   /* u16 eui u8 u8 u16 */
    { "message_sent", ZH_EZSP_ID_MESSAGE_SENT_HANDLER, d_msg_sent, 21, 20, 1, 0 },        /* u32 u8 u16 aps(11) u16 len bytes */
    { "incoming_message", ZH_EZSP_ID_INCOMING_MESSAGE_HANDLER, d_incoming, 31, 30, 1, 1 },   /* u8 aps(11) rxinfo(18) len bytes 开销(1) */
    { "incoming_route_record", ZH_EZSP_ID_INCOMING_ROUTE_RECORD_HANDLER, d_route_record, 13, 12, 2, 0 },   /* u16 eui u8 i8 cnt u16×cnt */
    { "id_conflict", ZH_EZSP_ID_ID_CONFLICT_HANDLER, d_id_conflict, 2, -1, 0, 0 },
    { "custom_frame_handler", ZH_EZSP_ID_CUSTOM_FRAME_HANDLER, d_cf_handler, 1, 0, 1, 0 },
    { "st set_config", ZH_EZSP_ID_SET_CONFIGURATION_VALUE, d_st_set_config, 4, -1, 0, 0 },
    { "st add_endpoint", ZH_EZSP_ID_ADD_ENDPOINT, d_st_add_ep, 4, -1, 0, 0 },
    { "st set_policy", ZH_EZSP_ID_SET_POLICY, d_st_set_policy, 4, -1, 0, 0 },
    { "st set_value", ZH_EZSP_ID_SET_VALUE, d_st_set_value, 4, -1, 0, 0 },
    { "st network_init", ZH_EZSP_ID_NETWORK_INIT, d_st_net_init, 4, -1, 0, 0 },
    { "st start_scan", ZH_EZSP_ID_START_SCAN, d_st_start_scan, 4, -1, 0, 0 },
    { "st form_network", ZH_EZSP_ID_FORM_NETWORK, d_st_form, 4, -1, 0, 0 },
    { "st leave_network", ZH_EZSP_ID_LEAVE_NETWORK, d_st_leave, 4, -1, 0, 0 },
    { "st permit_joining", ZH_EZSP_ID_PERMIT_JOINING, d_st_permit, 4, -1, 0, 0 },
    { "st set_radio_power", ZH_EZSP_ID_SET_RADIO_POWER, d_st_radio_power, 4, -1, 0, 0 },
    { "st set_concentrator", ZH_EZSP_ID_SET_CONCENTRATOR, d_st_concentrator, 4, -1, 0, 0 },
    { "st set_initial_security_state", ZH_EZSP_ID_SET_INITIAL_SECURITY_STATE, d_st_init_sec, 4, -1, 0, 0 },
    { "empty nop", ZH_EZSP_ID_NOP, d_e_nop, 0, -1, 0, 0 },
    { "empty clear_transient_link_keys", ZH_EZSP_ID_CLEAR_TRANSIENT_LINK_KEYS, d_e_clear, 0, -1, 0, 0 },
    { "empty noCallbacks", ZH_EZSP_ID_NO_CALLBACKS, d_e_nocb, 0, -1, 0, 0 },
};
#define NLAY (sizeof(k_lay) / sizeof(k_lay[0]))

/* 按布局拼参数：长度字节填 L，其余随机 */
static size_t mk_params(const layout_t *l, uint8_t L, uint8_t *par)
{
    const size_t n = l->fixed + (l->len_at >= 0 ? (size_t)L * l->unit : 0) + l->tail;
    for (size_t i = 0; i < n; i++) par[i] = rnd8();
    if (l->len_at >= 0) par[l->len_at] = L;
    return n;
}

static int dec_bytes(const layout_t *l, const uint8_t *par, size_t n, bool null)
{
    uint8_t f[300];
    zh_ezsp_frame_t fr;
    const size_t fn = mk(f, 0x80, l->id, par, n);
    if (zh_ezsp_parse(f, fn, &fr) != ZH_EZSP_OK) return 99;
    return l->dec(&fr, null);
}

static void t_decoders_strict(void)
{
    int bad_ok = 0, bad_short = 0, bad_long = 0, bad_claim = 0, bad_id = 0, bad_null = 0, bad_trunc0 = 0;
    for (size_t k = 0; k < NLAY; k++) {
        const layout_t *l = &k_lay[k];
        for (int it = 0; it < 40; it++) {
            uint8_t par[300];
            const uint8_t L = l->len_at >= 0 ? (uint8_t)(it == 0 ? 0 : rnd8() % (l->unit == 2 ? 80 : 160)) : 0;
            const size_t n = mk_params(l, L, par);
            if (dec_bytes(l, par, n, false) != ZH_EZSP_OK) { bad_ok++; printf("   %s ok L=%u\n", l->name, L); }
            if (dec_bytes(l, par, n, true) != ZH_EZSP_OK) bad_null++;
            /* 少一个字节（丢了尾字节、CRC 仍对的那种帧）、多一个字节 */
            if (n && dec_bytes(l, par, n - 1, false) != ZH_EZSP_E_LENGTH) { bad_short++; printf("   %s short\n", l->name); }
            par[n] = rnd8();
            if (dec_bytes(l, par, n + 1, false) != ZH_EZSP_E_LENGTH) { bad_long++; printf("   %s long\n", l->name); }
            /* 长度字节说的比帧里实际多：不许越界读（ASan 盯着），回 E_LENGTH */
            if (l->len_at >= 0 && L < 255) {
                par[l->len_at] = (uint8_t)(L + 1);
                if (dec_bytes(l, par, n, false) != ZH_EZSP_E_LENGTH) { bad_claim++; printf("   %s claim\n", l->name); }
                par[l->len_at] = L;
            }
            /* 参数一个字节都没有：定长 > 0 的都要拒 */
            if (l->fixed && dec_bytes(l, par, 0, false) != ZH_EZSP_E_LENGTH) bad_trunc0++;
            /* 帧 ID 不对 */
            uint8_t f[300];
            zh_ezsp_frame_t fr;
            const size_t fn = mk(f, 0x80, (uint16_t)(l->id ^ 0x0100), par, n);
            if (zh_ezsp_parse(f, fn, &fr) != ZH_EZSP_OK || l->dec(&fr, false) != ZH_EZSP_E_ID) bad_id++;
        }
    }
    CHECK(bad_ok == 0, "%zu 个解码器 × 40：布局恰好时都收（%d 次不收）", NLAY, bad_ok);
    CHECK(bad_null == 0, "输出全给 NULL 也能解（%d 次失败）", bad_null);
    CHECK(bad_short == 0, "少一个字节都拒 E_LENGTH（%d 次没拒）", bad_short);
    CHECK(bad_long == 0, "多一个字节都拒 E_LENGTH（%d 次没拒）", bad_long);
    CHECK(bad_claim == 0, "长度字节虚报多 1：拒 E_LENGTH、不越界（%d 次没拒）", bad_claim);
    CHECK(bad_trunc0 == 0, "参数为空：定长的都拒（%d 次没拒）", bad_trunc0);
    CHECK(bad_id == 0, "帧 ID 不对回 E_ID（%d 次没回）", bad_id);
    CHECK(NLAY == 39, "布局表覆盖 24 种回包 / 回调 + 12 个只回状态的命令 + 3 个空回包（实为 %zu）", NLAY);
}

static void t_decoder_misuse(void)
{
    uint8_t f[32], par[8] = { 0x12, 0x02, 0x34, 0x12 };
    zh_ezsp_frame_t fr;
    /* version 的回包恰好 4 B：dec_status 不许借道把它解成状态 */
    size_t n = mk(f, 0x80, ZH_EZSP_ID_VERSION, par, 4);
    CHECK(zh_ezsp_parse(f, n, &fr) == ZH_EZSP_OK, "version 回包能解析");
    uint32_t st = 0;
    CHECK(zh_ezsp_dec_status(&fr, ZH_EZSP_ID_VERSION, &st) == ZH_EZSP_E_ARG, "dec_status 只认 12 个只回状态的命令");
    CHECK(zh_ezsp_dec_empty(&fr, ZH_EZSP_ID_VERSION) == ZH_EZSP_E_ARG, "dec_empty 只认 nop / clear_transient_link_keys");
    CHECK(zh_ezsp_dec_send(&fr, ZH_EZSP_ID_VERSION, &st, NULL) == ZH_EZSP_E_ARG, "dec_send 只认 unicast / broadcast");
    CHECK(zh_ezsp_dec_version(NULL, NULL, NULL, NULL) == ZH_EZSP_E_ARG, "解码器 fr = NULL 回 E_ARG");
    /* 解码不改动 fr（同一帧可以被不同解码器各试一次） */
    uint8_t p = 0, t = 0;
    uint16_t v = 0;
    CHECK(zh_ezsp_dec_version(&fr, &p, &t, &v) == ZH_EZSP_OK && p == 0x12 && t == 0x02 && v == 0x1234, "version：协议 0x12、栈类型 2、栈版本小端 0x1234");
    CHECK(zh_ezsp_dec_version(&fr, &p, &t, &v) == ZH_EZSP_OK && fr.params.off == 0, "解码只读 fr 的副本，可以重复解");

    /* 上层漏看 parse 的返回值：解码器自己也拒回显 / 截断 / 保留位（nop 保活不能把回显当应答） */
    const uint8_t echo[] = { 0x11, 0x00, 0x01, 0x05, 0x00 };
    CHECK(zh_ezsp_parse(echo, 5, &fr) == ZH_EZSP_E_DIRECTION && zh_ezsp_dec_empty(&fr, ZH_EZSP_ID_NOP) == ZH_EZSP_E_DIRECTION,
          "nop 的回显：parse 拒了，dec_empty 也拒");
    const uint8_t trunc[] = { 0x11, 0x82, 0x01, 0x05, 0x00 };
    CHECK(zh_ezsp_parse(trunc, 5, &fr) == ZH_EZSP_E_TRUNCATED && zh_ezsp_dec_empty(&fr, ZH_EZSP_ID_NOP) == ZH_EZSP_E_TRUNCATED, "截断帧：解码器也拒");
    const uint8_t resv[] = { 0x11, 0x80, 0x05, 0x05, 0x00 };
    CHECK(zh_ezsp_parse(resv, 5, &fr) == ZH_EZSP_E_RESERVED && zh_ezsp_dec_empty(&fr, ZH_EZSP_ID_NOP) == ZH_EZSP_E_RESERVED, "保留位：解码器也拒");
    /* 旧式头只给 version：第 2 字节 0x02（格式位 0）会被解析成旧式头的 addEndpoint，不许当真 */
    const uint8_t lg_ae[] = { 0x11, 0x80, 0x02, 0x00, 0x00, 0x00, 0x00 };
    CHECK(zh_ezsp_parse(lg_ae, 7, &fr) == ZH_EZSP_OK && fr.legacy && fr.id == ZH_EZSP_ID_ADD_ENDPOINT &&
          zh_ezsp_dec_status(&fr, ZH_EZSP_ID_ADD_ENDPOINT, &st) == ZH_EZSP_E_ID, "旧式头的 addEndpoint：解码器回 E_ID");
    const uint8_t lg_v[] = { 0x00, 0x80, 0x00, 0x12, 0x02, 0x34, 0x12 };
    CHECK(zh_ezsp_parse(lg_v, 7, &fr) == ZH_EZSP_OK && zh_ezsp_dec_version(&fr, &p, &t, &v) == ZH_EZSP_OK && p == 0x12, "旧式头的 version：照常解");
}

/* ---- 头的分类 ---- */
static void t_parse(void)
{
    uint8_t f[64];
    zh_ezsp_frame_t fr;
    CHECK(zh_ezsp_parse(NULL, 5, &fr) == ZH_EZSP_E_ARG && zh_ezsp_parse(f, 5, NULL) == ZH_EZSP_E_ARG, "NULL 回 E_ARG");
    f[0] = 1; f[1] = 0x80; f[2] = 0x00;
    CHECK(zh_ezsp_parse(f, 2, &fr) == ZH_EZSP_E_SHORT, "不到 3 字节是短帧");
    CHECK(zh_ezsp_parse(f, 3, &fr) == ZH_EZSP_OK && fr.legacy && fr.id == 0 && fr.params.n == 0, "第 2 字节格式位不是 1 → 旧式 3 字节头");
    f[2] = 0x01; f[3] = 0x05;
    CHECK(zh_ezsp_parse(f, 3, &fr) == ZH_EZSP_E_SHORT && zh_ezsp_parse(f, 4, &fr) == ZH_EZSP_E_SHORT,
          "第 2 字节说是扩展头、不到 5 字节 → 短帧（不能退回旧式头去解）");
    f[4] = 0x00;
    CHECK(zh_ezsp_parse(f, 5, &fr) == ZH_EZSP_OK && !fr.legacy && fr.id == ZH_EZSP_ID_NOP && fr.seq == 1 && fr.fc_hi == 0x01,
          "扩展头：seq、fc_hi、帧 ID 小端");
    /* 保留位：0x04 0x08 0x10 0x20 各自单独置位都拒；0x40 填充、0x80 加密不在保留位里 */
    int bad = 0;
    for (int b = 2; b < 6; b++) {
        f[2] = (uint8_t)(0x01 | (1 << b));
        if (zh_ezsp_parse(f, 5, &fr) != ZH_EZSP_E_RESERVED) bad++;
    }
    CHECK(bad == 0, "fc_hi 保留位 0x3C 任一位置了 → E_RESERVED");
    f[2] = 0x41;
    CHECK(zh_ezsp_parse(f, 5, &fr) == ZH_EZSP_OK, "fc_hi 0x40（填充）不是保留位");
    f[2] = 0x01;
    f[1] = 0x00;
    CHECK(zh_ezsp_parse(f, 5, &fr) == ZH_EZSP_E_DIRECTION && fr.seq == 1 && fr.id == ZH_EZSP_ID_NOP,
          "方向位是命令（主机自己的命令被回显）→ E_DIRECTION，seq / id 仍填好");
    f[1] = 0x82;
    CHECK(zh_ezsp_parse(f, 5, &fr) == ZH_EZSP_E_TRUNCATED, "截断位 → E_TRUNCATED");
    f[1] = 0x83;
    CHECK(zh_ezsp_parse(f, 5, &fr) == ZH_EZSP_E_TRUNCATED, "截断 + 溢出：溢出不洗白截断（SDK 会当成功，见头文件）");
    f[1] = 0x81;
    CHECK(zh_ezsp_parse(f, 5, &fr) == ZH_EZSP_OK && zh_ezsp_overflow(&fr), "只有溢出位：照收，overflow() 为真");
    f[1] = 0x84;
    CHECK(zh_ezsp_parse(f, 5, &fr) == ZH_EZSP_OK && zh_ezsp_pending_cb(&fr) && !zh_ezsp_is_callback(&fr), "还有回调待取：pending_cb()");
    f[1] = 0x90;
    CHECK(zh_ezsp_parse(f, 5, &fr) == ZH_EZSP_OK && zh_ezsp_is_callback(&fr), "异步回调位：is_callback()");
    f[1] = 0x88;
    CHECK(zh_ezsp_parse(f, 5, &fr) == ZH_EZSP_OK && zh_ezsp_is_callback(&fr), "同步回调位：is_callback()");
    /* invalidCommand：原因码；原因码 0 也拒（SDK 会当成功）；扩展头与旧式头（0x58 的格式位是 0）都认 */
    f[1] = 0x80; f[3] = 0x58; f[4] = 0x00; f[5] = 0x31;
    CHECK(zh_ezsp_parse(f, 6, &fr) == ZH_EZSP_E_INVALID_CMD && fr.invalid_reason == 0x31, "invalidCommand → E_INVALID_CMD，原因码 0x31");
    f[5] = 0x00;
    CHECK(zh_ezsp_parse(f, 6, &fr) == ZH_EZSP_E_INVALID_CMD, "invalidCommand 原因码 0 也拒");
    f[5] = 0x34;
    CHECK(zh_ezsp_parse(f, 6, &fr) == ZH_EZSP_E_INVALID_CMD && fr.invalid_reason == 0x34, "invalidCommand 原因码 0x34（= OVERFLOW 的值）也拒");
    CHECK(zh_ezsp_parse(f, 5, &fr) == ZH_EZSP_E_INVALID_CMD && fr.invalid_reason == 0, "invalidCommand 没带原因码也拒、不越界");
    const uint8_t lg[] = { 7, 0x80, 0x58, 0x31 };
    CHECK(zh_ezsp_parse(lg, 4, &fr) == ZH_EZSP_E_INVALID_CMD && fr.legacy && fr.invalid_reason == 0x31, "旧式头的 invalidCommand 也认");
    /* 网络号在 fc 0x60 */
    const uint8_t ni[] = { 9, 0xE0, 0x01, 0x05, 0x00 };
    CHECK(zh_ezsp_parse(ni, 5, &fr) == ZH_EZSP_OK && ((fr.fc & ZH_EZSP_FC_NETIDX_MASK) >> ZH_EZSP_FC_NETIDX_SHIFT) == 3, "网络号 3");
    /* 参数视图指向原帧、不拷贝 */
    const uint8_t pv[] = { 1, 0x80, 0x01, 0x27, 0x00, 0x34, 0x12 };
    CHECK(zh_ezsp_parse(pv, sizeof(pv), &fr) == ZH_EZSP_OK && fr.params.p == pv + 5 && fr.params.n == 2, "params 指向原帧第 5 字节起");
}

/* ---- 编码：金标（按 ezsp.c 的头格式手写，不看被测代码）与参数校验 ---- */
static void t_encode(void)
{
    uint8_t b[300];
    const uint8_t legacy[] = { 0x00, 0x00, 0x00, 0x12 };
    CHECK(zh_ezsp_enc_version_legacy(b, sizeof(b), 0, ZH_EZSP_PROTOCOL_VERSION) == 4 && !memcmp(b, legacy, 4), "首条 version：00 00 00 12（旧式头）");
    const uint8_t ver[] = { 0x01, 0x00, 0x01, 0x00, 0x00, 0x12 };
    CHECK(zh_ezsp_enc_version(b, sizeof(b), 1, 0x12) == 6 && !memcmp(b, ver, 6), "其后的 version：01 00 01 00 00 12（扩展头）");
    const uint8_t nop[] = { 0x7F, 0x00, 0x01, 0x05, 0x00 };
    CHECK(zh_ezsp_enc_nop(b, sizeof(b), 0x7F) == 5 && !memcmp(b, nop, 5), "nop：7F 00 01 05 00");
    const uint8_t sc[] = { 0x02, 0x00, 0x01, 0x53, 0x00, 0x2D, 0x34, 0x12 };
    CHECK(zh_ezsp_enc_set_config(b, sizeof(b), 2, 0x2D, 0x1234) == 8 && !memcmp(b, sc, 8), "set_config：u8 + u16 小端");
    const uint8_t ss[] = { 0x03, 0x00, 0x01, 0x1A, 0x00, 0x01, 0x00, 0xF8, 0xFF, 0x07, 0x04 };
    CHECK(zh_ezsp_enc_start_scan(b, sizeof(b), 3, 1, 0x07FFF800u, 4) == 11 && !memcmp(b, ss, 11), "start_scan：u8 + u32 小端 + u8");
    const uint8_t rp[] = { 0x04, 0x00, 0x01, 0x99, 0x00, 0xF6 };
    CHECK(zh_ezsp_enc_set_radio_power(b, sizeof(b), 4, -10) == 6 && !memcmp(b, rp, 6), "set_radio_power(-10)：按原字节 0xF6");
    const uint8_t cb[] = { 0x05, 0x00, 0x01, 0x06, 0x00 };
    CHECK(zh_ezsp_enc_callback(b, sizeof(b), 5) == 5 && !memcmp(b, cb, 5), "callback：05 00 01 06 00");
    const uint8_t pj[] = { 0x06, 0x00, 0x01, 0x22, 0x00, 0xFE };
    CHECK(zh_ezsp_enc_permit_joining(b, sizeof(b), 6, 254) == 6 && !memcmp(b, pj, 6), "permitJoining(254)：最长的有期限窗口");
    CHECK(zh_ezsp_enc_permit_joining(b, sizeof(b), 6, 255) == 0, "permitJoining(255) = 永久开放：拒编");

    /* 网络号 */
    zh_ezsp_w_t w;
    zh_ezsp_w_begin(&w, b, sizeof(b), 5, ZH_EZSP_ID_NOP, 2);
    CHECK(zh_ezsp_w_end(&w) == 5 && b[1] == 0x40, "w_begin 网络号 2 → fc 0x40");
    zh_ezsp_w_begin(&w, b, sizeof(b), 5, ZH_EZSP_ID_NOP, 4);
    CHECK(zh_ezsp_w_end(&w) == 0, "网络号 4 越界 → 整帧作废");

    /* 220 字节边界：set_value 头 5 + id 1 + len 1 + L */
    uint8_t v[255];
    memset(v, 0xAB, sizeof(v));
    CHECK(zh_ezsp_enc_set_value(b, sizeof(b), 6, 1, 213, v) == 220, "set_value 213 B → 恰好 220");
    CHECK(zh_ezsp_enc_set_value(b, sizeof(b), 6, 1, 214, v) == 0, "set_value 214 B → 221 超长回 0");
    CHECK(zh_ezsp_enc_custom_frame(b, sizeof(b), 6, 214, v) == 220 && zh_ezsp_enc_custom_frame(b, sizeof(b), 6, 215, v) == 0, "custom_frame 214 / 215");
    /* cap 不够：差一个字节就回 0、不越界写（ASan 盯着） */
    uint8_t small[8];
    CHECK(zh_ezsp_enc_set_config(small, 7, 2, 0x2D, 0x1234) == 0 && zh_ezsp_enc_set_config(small, 8, 2, 0x2D, 0x1234) == 8, "cap 7 放不下 8 字节帧、cap 8 刚好");
    CHECK(zh_ezsp_enc_version_legacy(small, 3, 0, 0x12) == 0, "旧式 version cap 3 放不下");
    CHECK(zh_ezsp_enc_nop(NULL, 100, 0) == 0, "buf = NULL 回 0");
    /* 指针参数：长度非 0 却给 NULL 都拒 */
    CHECK(zh_ezsp_enc_set_value(b, sizeof(b), 0, 1, 3, NULL) == 0 && zh_ezsp_enc_set_value(b, sizeof(b), 0, 1, 0, NULL) == 7, "set_value：len>0 且 NULL 拒；len 0 可以 NULL");
    CHECK(zh_ezsp_enc_custom_frame(b, sizeof(b), 0, 3, NULL) == 0, "custom_frame：len>0 且 NULL 拒");
    CHECK(zh_ezsp_enc_add_endpoint(b, sizeof(b), 0, 1, 0x104, 5, 0, 2, 0, NULL, NULL) == 0, "add_endpoint：in_count>0 且 NULL 拒");
    zh_ezsp_aps_t aps = { 0 };
    CHECK(zh_ezsp_enc_send_unicast(b, sizeof(b), 0, 0, 0, NULL, 0, 0, NULL) == 0 && zh_ezsp_enc_send_unicast(b, sizeof(b), 0, 0, 0, &aps, 0, 2, NULL) == 0,
          "send_unicast：aps NULL / 报文 NULL 拒");
    CHECK(zh_ezsp_enc_send_broadcast(b, sizeof(b), 0, 0, 0xFFFC, 0, NULL, 0, 0, 0, NULL) == 0, "send_broadcast：aps NULL 拒");
    CHECK(zh_ezsp_enc_form_network(b, sizeof(b), 0, NULL) == 0 && zh_ezsp_enc_set_initial_security_state(b, sizeof(b), 0, NULL) == 0 &&
          zh_ezsp_enc_lookup_node_id(b, sizeof(b), 0, NULL) == 0, "form_network / set_initial_security_state / lookup_node_id：NULL 拒");
    /* 报文长度 0 合法 */
    CHECK(zh_ezsp_enc_send_unicast(b, sizeof(b), 0, 0, 0x1234, &aps, 7, 0, NULL) == 5 + 1 + 2 + 11 + 2 + 1, "send_unicast 空报文 22 B");
}

/* ---- 读写器的边界 ---- */
static void t_rw(void)
{
    const uint8_t d[] = { 1, 2, 3 };
    zh_ezsp_r_t r = { d, 3, 0, false };
    CHECK(zh_ezsp_r_u16(&r) == 0x0201 && !r.err && r.off == 2, "r_u16 小端");
    (void)zh_ezsp_r_u16(&r);
    CHECK(r.err, "跨尾读 u16 置 err");
    zh_ezsp_r_t r2 = { d, 3, 0, false };
    CHECK(zh_ezsp_r_ptr(&r2, 4) == NULL && r2.err && r2.off == 0, "r_ptr 超长回 NULL、不前进");
    zh_ezsp_r_t r3 = { d, 3, 0, false };
    uint8_t out[4] = { 9, 9, 9, 9 };
    zh_ezsp_r_bytes(&r3, out, 4);
    CHECK(r3.err && out[0] == 0 && out[3] == 0, "r_bytes 超长：置 err、输出清零（不留半截）");
    zh_ezsp_r_t r4 = { d, 3, 0, false };
    (void)zh_ezsp_r_u32(&r4);
    CHECK(r4.err, "r_u32 跨尾置 err");
    CHECK(zh_ezsp_r_u8(NULL) == 0 && zh_ezsp_r_ptr(NULL, 0) == NULL, "读器 NULL 不崩");
    uint8_t b[4];
    zh_ezsp_w_t w = { b, 4, 0, false };
    zh_ezsp_w_u32(&w, 0x04030201u);
    CHECK(zh_ezsp_w_end(&w) == 4 && b[0] == 1 && b[3] == 4, "w_u32 小端、恰好写满");
    zh_ezsp_w_u8(&w, 5);
    CHECK(zh_ezsp_w_end(&w) == 0 && w.ovf, "写满再写一个 → ovf、w_end 回 0");
    zh_ezsp_w_t w2 = { b, 4, 0, false };
    zh_ezsp_w_u8(&w2, 1);
    zh_ezsp_w_bytes(&w2, NULL, 2);
    CHECK(zh_ezsp_w_end(&w2) == 0 && w2.ovf, "w_bytes(NULL, 2) → 整帧作废（不是悄悄少写两个字节）");
    zh_ezsp_w_u8(NULL, 1);
    CHECK(zh_ezsp_w_end(NULL) == 0, "写器 NULL 不崩");
}

/* 真 NCP 发来的帧（2026-10-22 第五步首次上板，/zigbee/status 的 incoming_bad_last 抓到的）：人体传感器的 IAS Zone 上报。
   参数 41 字节 = 31 字节头 + 9 字节消息 + 1 字节源路由开销；只按生成的分发函数解（没有末尾那个字节）条条被拒 */
static void t_real_ncp_frames(void)
{
    static const uint8_t head[31] = { 0x00, 0x04, 0x01, 0x00, 0x05, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0xFC,   /* 单播、HA、0x0500、1→1、序号 0xFC */
                                      0xF5, 0xCA, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xE5,            /* 0xCAF5、长地址未知、LQI 255、RSSI -27 */
                                      0xA0, 0x41, 0x29, 0x07, 0x09 };                                       /* 时间戳、消息 9 字节 */
    uint8_t par[64], f[128];
    memcpy(par, head, 31);
    for (int i = 0; i < 9; i++) par[31 + i] = (uint8_t)(0x10 + i);       /* 载荷没抓（只留了头），随便填 */
    par[40] = 0xFF;                                                      /* 源路由开销：不知道 */
    zh_ezsp_frame_t fr;
    uint8_t t = 0, l = 0, ov = 0;
    zh_ezsp_aps_t a;
    zh_ezsp_rx_info_t ri;
    const uint8_t *m = NULL;
    size_t fn = mk(f, 0x90, ZH_EZSP_ID_INCOMING_MESSAGE_HANDLER, par, 41);
    CHECK(zh_ezsp_parse(f, fn, &fr) == ZH_EZSP_OK && zh_ezsp_dec_incoming_message(&fr, &t, &a, &ri, &l, &m, &ov) == ZH_EZSP_OK,
          "真 NCP 的 incomingMessageHandler（41 字节参数）能解");
    CHECK(t == 0 && a.profile_id == 0x0104 && a.cluster_id == 0x0500 && a.src_ep == 1 && a.dst_ep == 1 && ri.sender_short == 0xCAF5 &&
          ri.last_hop_rssi == -27 && l == 9 && m && m[0] == 0x10 && ov == 0xFF, "  字段：IAS Zone、0xCAF5、RSSI -27、消息 9 字节、开销 0xFF");
    fn = mk(f, 0x90, ZH_EZSP_ID_INCOMING_MESSAGE_HANDLER, par, 40);
    CHECK(zh_ezsp_parse(f, fn, &fr) == ZH_EZSP_OK && zh_ezsp_dec_incoming_message(&fr, &t, &a, &ri, &l, &m, &ov) == ZH_EZSP_E_LENGTH,
          "  少了开销字节（= 只按分发函数的布局）：拒 E_LENGTH，照样严格");
}

int main(void)
{
    t_parse();
    t_real_ncp_frames();
    t_decoders_strict();
    t_decoder_misuse();
    t_encode();
    t_rw();
    printf("zh_ezsp_test：%d 通过，%d 失败\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
