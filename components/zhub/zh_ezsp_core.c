/* zh_ezsp_core —— EZSP v18 帧的编码与解析（见 include/zh_ezsp_core.h，那里有 H1 的出处）。纯 C，不依赖 ESP-IDF，不分配内存。 */
#include "zh_ezsp_core.h"

#include <string.h>

/* ---------------------------------------------------------------- 写 */

void zh_ezsp_w_begin(zh_ezsp_w_t *w, uint8_t *buf, size_t cap, uint8_t seq, uint16_t frame_id, uint8_t net_idx)
{
    if (!w) return;
    w->p = buf;
    w->cap = buf ? cap : 0;
    w->n = 0;
    w->ovf = net_idx > (ZH_EZSP_FC_NETIDX_MASK >> ZH_EZSP_FC_NETIDX_SHIFT);   /* 网络号只有 2 位：越界不悄悄截掉，整帧作废 */
    zh_ezsp_w_u8(w, seq);
    /* fc 低字节：命令方向（0）、网络号、睡眠模式 idle（ezsp.c:621-626）；高字节：扩展格式版本 1（ezsp.c:628-633） */
    zh_ezsp_w_u8(w, (uint8_t)((net_idx << ZH_EZSP_FC_NETIDX_SHIFT) & ZH_EZSP_FC_NETIDX_MASK));
    zh_ezsp_w_u8(w, ZH_EZSP_FCH_FORMAT_V1);
    zh_ezsp_w_u16(w, frame_id);
}

void zh_ezsp_w_u8(zh_ezsp_w_t *w, uint8_t v)
{
    if (!w) return;
    if (w->n < w->cap && w->n < ZH_EZSP_MAX) w->p[w->n] = v;
    else w->ovf = true;
    w->n++;
}

void zh_ezsp_w_u16(zh_ezsp_w_t *w, uint16_t v)
{
    zh_ezsp_w_u8(w, (uint8_t)(v & 0xFF));             /* 小端（ezsp-frame-utilities.c:56-60） */
    zh_ezsp_w_u8(w, (uint8_t)(v >> 8));
}

void zh_ezsp_w_u32(zh_ezsp_w_t *w, uint32_t v)
{
    zh_ezsp_w_u16(w, (uint16_t)(v & 0xFFFF));
    zh_ezsp_w_u16(w, (uint16_t)(v >> 16));
}

void zh_ezsp_w_bytes(zh_ezsp_w_t *w, const uint8_t *b, size_t n)
{
    if (!w) return;
    if (n && !b) {
        w->ovf = true;
        return;
    }
    for (size_t i = 0; i < n; i++) zh_ezsp_w_u8(w, b[i]);
}

size_t zh_ezsp_w_end(zh_ezsp_w_t *w)
{
    if (!w || w->ovf || w->n > w->cap || w->n > ZH_EZSP_MAX) return 0;
    return w->n;
}

/* ---------------------------------------------------------------- 读 */

uint8_t zh_ezsp_r_u8(zh_ezsp_r_t *r)
{
    if (!r || r->off >= r->n) {
        if (r) r->err = true;
        return 0;
    }
    return r->p[r->off++];
}

uint16_t zh_ezsp_r_u16(zh_ezsp_r_t *r)
{
    const uint16_t lo = zh_ezsp_r_u8(r);
    const uint16_t hi = zh_ezsp_r_u8(r);
    return (uint16_t)(lo | (uint16_t)(hi << 8));
}

uint32_t zh_ezsp_r_u32(zh_ezsp_r_t *r)
{
    const uint32_t lo = zh_ezsp_r_u16(r);
    const uint32_t hi = zh_ezsp_r_u16(r);
    return lo | (hi << 16);
}

void zh_ezsp_r_bytes(zh_ezsp_r_t *r, uint8_t *out, size_t n)
{
    const uint8_t *p = zh_ezsp_r_ptr(r, n);
    if (out) {
        if (p) memcpy(out, p, n);
        else memset(out, 0, n);
    }
}

const uint8_t *zh_ezsp_r_ptr(zh_ezsp_r_t *r, size_t n)
{
    if (!r || r->off > r->n || n > r->n - r->off) {
        if (r) r->err = true;
        return NULL;
    }
    const uint8_t *p = r->p + r->off;
    r->off += n;
    return p;
}

/* ---------------------------------------------------------------- 解析头 */

int zh_ezsp_parse(const uint8_t *f, size_t n, zh_ezsp_frame_t *out)
{
    if (!f || !out) return ZH_EZSP_E_ARG;
    memset(out, 0, sizeof(*out));
    if (n < ZH_EZSP_LEGACY_HDR) return ZH_EZSP_E_SHORT;
    size_t hdr;
    out->seq = f[0];
    out->fc = f[1];
    /* 扩展头看 fc 高字节（第 2 字节）的格式位（ezsp.c:552-553）；否则是旧式头（只有第一条 version 的回包）。
       格式只按这一字节判，与 SDK 一致；判成扩展却不够 5 字节就是短帧（SDK 在这里会读到共用缓冲里刚发出去的命令的字节） */
    if ((f[2] & ZH_EZSP_FCH_FORMAT_MASK) == ZH_EZSP_FCH_FORMAT_V1) {
        if (n < ZH_EZSP_EXT_HDR) return ZH_EZSP_E_SHORT;
        out->fc_hi = f[2];
        out->id = (uint16_t)(f[3] | (f[4] << 8));
        hdr = ZH_EZSP_EXT_HDR;
        if (out->fc_hi & ZH_EZSP_FCH_RESERVED_MASK) return ZH_EZSP_E_RESERVED;   /* ezsp.c:558-561 */
    } else {
        out->legacy = true;
        out->id = f[2];
        hdr = ZH_EZSP_LEGACY_HDR;
    }
    out->params.p = f + hdr;
    out->params.n = n - hdr;
    out->params.off = 0;
    out->params.err = false;
    /* ezsp.c:569-585 判的是同几件事，但那里后写的覆盖先写的（溢出会把错误洗成成功）；这里先判先拒，溢出只是提示（见头文件） */
    if (out->id == ZH_EZSP_ID_INVALID_COMMAND) {
        out->invalid_reason = out->params.n ? out->params.p[0] : 0;
        return ZH_EZSP_E_INVALID_CMD;
    }
    if ((out->fc & ZH_EZSP_FC_RESPONSE) == 0) return ZH_EZSP_E_DIRECTION;   /* 主机自己的命令被回显 */
    if (out->fc & ZH_EZSP_FC_TRUNCATED) return ZH_EZSP_E_TRUNCATED;
    return ZH_EZSP_OK;
}

/* ---------------------------------------------------------------- 编码 */

#define ENC_BEGIN(id)                                                         \
    zh_ezsp_w_t w;                                                            \
    zh_ezsp_w_begin(&w, buf, cap, seq, (id), 0)

size_t zh_ezsp_enc_version_legacy(uint8_t *buf, size_t cap, uint8_t seq, uint8_t desired_protocol)
{
    /* 旧式头 [seq][fc=0][帧 ID 低字节] + desiredProtocolVersion（ezsp.c:525-528、628-633：第一条 version 不写 fc 高字节） */
    if (!buf || cap < ZH_EZSP_LEGACY_HDR + 1) return 0;
    buf[0] = seq;
    buf[1] = 0x00;
    buf[2] = (uint8_t)ZH_EZSP_ID_VERSION;
    buf[3] = desired_protocol;
    return ZH_EZSP_LEGACY_HDR + 1;
}

size_t zh_ezsp_enc_version(uint8_t *buf, size_t cap, uint8_t seq, uint8_t desired_protocol)
{
    ENC_BEGIN(ZH_EZSP_ID_VERSION);
    zh_ezsp_w_u8(&w, desired_protocol);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_get_config(uint8_t *buf, size_t cap, uint8_t seq, uint8_t config_id)
{
    ENC_BEGIN(ZH_EZSP_ID_GET_CONFIGURATION_VALUE);
    zh_ezsp_w_u8(&w, config_id);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_set_config(uint8_t *buf, size_t cap, uint8_t seq, uint8_t config_id, uint16_t value)
{
    ENC_BEGIN(ZH_EZSP_ID_SET_CONFIGURATION_VALUE);
    zh_ezsp_w_u8(&w, config_id);
    zh_ezsp_w_u16(&w, value);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_add_endpoint(uint8_t *buf, size_t cap, uint8_t seq, uint8_t endpoint, uint16_t profile_id, uint16_t device_id, uint8_t device_version,
                                uint8_t in_count, uint8_t out_count, const uint16_t *in_clusters, const uint16_t *out_clusters)
{
    if ((in_count && !in_clusters) || (out_count && !out_clusters)) return 0;
    ENC_BEGIN(ZH_EZSP_ID_ADD_ENDPOINT);
    zh_ezsp_w_u8(&w, endpoint);
    zh_ezsp_w_u16(&w, profile_id);
    zh_ezsp_w_u16(&w, device_id);
    zh_ezsp_w_u8(&w, device_version);
    zh_ezsp_w_u8(&w, in_count);
    zh_ezsp_w_u8(&w, out_count);
    for (uint8_t i = 0; i < in_count; i++) zh_ezsp_w_u16(&w, in_clusters[i]);
    for (uint8_t i = 0; i < out_count; i++) zh_ezsp_w_u16(&w, out_clusters[i]);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_set_policy(uint8_t *buf, size_t cap, uint8_t seq, uint8_t policy_id, uint8_t decision_id)
{
    ENC_BEGIN(ZH_EZSP_ID_SET_POLICY);
    zh_ezsp_w_u8(&w, policy_id);
    zh_ezsp_w_u8(&w, decision_id);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_get_value(uint8_t *buf, size_t cap, uint8_t seq, uint8_t value_id)
{
    ENC_BEGIN(ZH_EZSP_ID_GET_VALUE);
    zh_ezsp_w_u8(&w, value_id);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_set_value(uint8_t *buf, size_t cap, uint8_t seq, uint8_t value_id, uint8_t len, const uint8_t *value)
{
    if (len && !value) return 0;
    ENC_BEGIN(ZH_EZSP_ID_SET_VALUE);
    zh_ezsp_w_u8(&w, value_id);
    zh_ezsp_w_u8(&w, len);
    zh_ezsp_w_bytes(&w, value, len);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_nop(uint8_t *buf, size_t cap, uint8_t seq)
{
    ENC_BEGIN(ZH_EZSP_ID_NOP);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_custom_frame(uint8_t *buf, size_t cap, uint8_t seq, uint8_t len, const uint8_t *payload)
{
    if (len && !payload) return 0;
    ENC_BEGIN(ZH_EZSP_ID_CUSTOM_FRAME);
    zh_ezsp_w_u8(&w, len);
    zh_ezsp_w_bytes(&w, payload, len);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_callback(uint8_t *buf, size_t cap, uint8_t seq)
{
    ENC_BEGIN(ZH_EZSP_ID_CALLBACK);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_get_eui64(uint8_t *buf, size_t cap, uint8_t seq)
{
    ENC_BEGIN(ZH_EZSP_ID_GET_EUI64);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_get_node_id(uint8_t *buf, size_t cap, uint8_t seq)
{
    ENC_BEGIN(ZH_EZSP_ID_GET_NODE_ID);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_network_init(uint8_t *buf, size_t cap, uint8_t seq, uint16_t bitmask)
{
    ENC_BEGIN(ZH_EZSP_ID_NETWORK_INIT);
    zh_ezsp_w_u16(&w, bitmask);                        /* sl_zigbee_network_init_struct_t 只有 bitmask（ezsp-frame-utilities.c:301-304） */
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_network_state(uint8_t *buf, size_t cap, uint8_t seq)
{
    ENC_BEGIN(ZH_EZSP_ID_NETWORK_STATE);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_start_scan(uint8_t *buf, size_t cap, uint8_t seq, uint8_t scan_type, uint32_t channel_mask, uint8_t duration)
{
    ENC_BEGIN(ZH_EZSP_ID_START_SCAN);
    zh_ezsp_w_u8(&w, scan_type);
    zh_ezsp_w_u32(&w, channel_mask);
    zh_ezsp_w_u8(&w, duration);
    return zh_ezsp_w_end(&w);
}

static void w_net_params(zh_ezsp_w_t *w, const zh_ezsp_net_params_t *p)
{
    /* ezsp-frame-utilities.h:68-78 append_sl_zigbee_network_parameters_t */
    zh_ezsp_w_bytes(w, p->ext_pan_id, 8);
    zh_ezsp_w_u16(w, p->pan_id);
    zh_ezsp_w_u8(w, (uint8_t)p->tx_power);
    zh_ezsp_w_u8(w, p->channel);
    zh_ezsp_w_u8(w, p->join_method);
    zh_ezsp_w_u16(w, p->nwk_manager_id);
    zh_ezsp_w_u8(w, p->nwk_update_id);
    zh_ezsp_w_u32(w, p->channels);
}

size_t zh_ezsp_enc_form_network(uint8_t *buf, size_t cap, uint8_t seq, const zh_ezsp_net_params_t *p)
{
    if (!p) return 0;
    ENC_BEGIN(ZH_EZSP_ID_FORM_NETWORK);
    w_net_params(&w, p);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_leave_network(uint8_t *buf, size_t cap, uint8_t seq, uint8_t options)
{
    ENC_BEGIN(ZH_EZSP_ID_LEAVE_NETWORK);
    zh_ezsp_w_u8(&w, options);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_permit_joining(uint8_t *buf, size_t cap, uint8_t seq, uint8_t duration)
{
    if (duration == 0xFF) return 0;                    /* 0xFF = 永久开放，入网窗口必须有期限 */
    ENC_BEGIN(ZH_EZSP_ID_PERMIT_JOINING);
    zh_ezsp_w_u8(&w, duration);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_get_network_parameters(uint8_t *buf, size_t cap, uint8_t seq)
{
    ENC_BEGIN(ZH_EZSP_ID_GET_NETWORK_PARAMETERS);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_set_radio_power(uint8_t *buf, size_t cap, uint8_t seq, int8_t power)
{
    ENC_BEGIN(ZH_EZSP_ID_SET_RADIO_POWER);
    zh_ezsp_w_u8(&w, (uint8_t)power);                 /* appendInt8 = appendInt8u（ezsp-frame-utilities.h:61） */
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_set_concentrator(uint8_t *buf, size_t cap, uint8_t seq, bool on, uint16_t type, uint16_t min_time, uint16_t max_time,
                                    uint8_t route_error_threshold, uint8_t delivery_failure_threshold, uint8_t max_hops)
{
    ENC_BEGIN(ZH_EZSP_ID_SET_CONCENTRATOR);
    zh_ezsp_w_u8(&w, on ? 1 : 0);
    zh_ezsp_w_u16(&w, type);
    zh_ezsp_w_u16(&w, min_time);
    zh_ezsp_w_u16(&w, max_time);
    zh_ezsp_w_u8(&w, route_error_threshold);
    zh_ezsp_w_u8(&w, delivery_failure_threshold);
    zh_ezsp_w_u8(&w, max_hops);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_set_source_route_discovery_mode(uint8_t *buf, size_t cap, uint8_t seq, uint8_t mode)
{
    ENC_BEGIN(ZH_EZSP_ID_SET_SOURCE_ROUTE_DISCOVERY_MODE);
    zh_ezsp_w_u8(&w, mode);                            /* command-functions.h:2389 appendInt8u(mode) */
    return zh_ezsp_w_end(&w);
}

static void w_aps(zh_ezsp_w_t *w, const zh_ezsp_aps_t *a)
{
    /* ezsp-frame-utilities.c:140-149 append_sl_zigbee_aps_frame_t */
    zh_ezsp_w_u16(w, a->profile_id);
    zh_ezsp_w_u16(w, a->cluster_id);
    zh_ezsp_w_u8(w, a->src_ep);
    zh_ezsp_w_u8(w, a->dst_ep);
    zh_ezsp_w_u16(w, a->options);
    zh_ezsp_w_u16(w, a->group_id);
    zh_ezsp_w_u8(w, a->sequence);
}

size_t zh_ezsp_enc_send_unicast(uint8_t *buf, size_t cap, uint8_t seq, uint8_t type, uint16_t index_or_dest, const zh_ezsp_aps_t *aps,
                                uint16_t tag, uint8_t len, const uint8_t *msg)
{
    if (!aps || (len && !msg)) return 0;
    ENC_BEGIN(ZH_EZSP_ID_SEND_UNICAST);
    zh_ezsp_w_u8(&w, type);
    zh_ezsp_w_u16(&w, index_or_dest);
    w_aps(&w, aps);
    zh_ezsp_w_u16(&w, tag);
    zh_ezsp_w_u8(&w, len);
    zh_ezsp_w_bytes(&w, msg, len);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_send_broadcast(uint8_t *buf, size_t cap, uint8_t seq, uint16_t alias, uint16_t dest, uint8_t nwk_seq, const zh_ezsp_aps_t *aps,
                                  uint8_t radius, uint16_t tag, uint8_t len, const uint8_t *msg)
{
    if (!aps || (len && !msg)) return 0;
    ENC_BEGIN(ZH_EZSP_ID_SEND_BROADCAST);
    zh_ezsp_w_u16(&w, alias);                          /* v18 多了 alias / nwkSequence（command-functions.h sendBroadcast） */
    zh_ezsp_w_u16(&w, dest);
    zh_ezsp_w_u8(&w, nwk_seq);
    w_aps(&w, aps);
    zh_ezsp_w_u8(&w, radius);
    zh_ezsp_w_u16(&w, tag);
    zh_ezsp_w_u8(&w, len);
    zh_ezsp_w_bytes(&w, msg, len);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_lookup_eui64(uint8_t *buf, size_t cap, uint8_t seq, uint16_t node_id)
{
    ENC_BEGIN(ZH_EZSP_ID_LOOKUP_EUI64_BY_NODE_ID);
    zh_ezsp_w_u16(&w, node_id);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_lookup_node_id(uint8_t *buf, size_t cap, uint8_t seq, const uint8_t eui64[8])
{
    if (!eui64) return 0;
    ENC_BEGIN(ZH_EZSP_ID_LOOKUP_NODE_ID_BY_EUI64);
    zh_ezsp_w_bytes(&w, eui64, 8);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_set_initial_security_state(uint8_t *buf, size_t cap, uint8_t seq, const zh_ezsp_init_sec_t *s)
{
    if (!s) return 0;
    ENC_BEGIN(ZH_EZSP_ID_SET_INITIAL_SECURITY_STATE);
    /* ezsp-frame-utilities.c:228-235：bitmask、预配置密钥 16、网络密钥 16、密钥序号、TC 的 EUI64 */
    zh_ezsp_w_u16(&w, s->bitmask);
    zh_ezsp_w_bytes(&w, s->preconfigured_key, 16);
    zh_ezsp_w_bytes(&w, s->network_key, 16);
    zh_ezsp_w_u8(&w, s->network_key_seq);
    zh_ezsp_w_bytes(&w, s->tc_eui64, 8);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_get_current_security_state(uint8_t *buf, size_t cap, uint8_t seq)
{
    ENC_BEGIN(ZH_EZSP_ID_GET_CURRENT_SECURITY_STATE);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_clear_transient_link_keys(uint8_t *buf, size_t cap, uint8_t seq)
{
    ENC_BEGIN(ZH_EZSP_ID_CLEAR_TRANSIENT_LINK_KEYS);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_import_transient_key(uint8_t *buf, size_t cap, uint8_t seq, const uint8_t eui64[8], const uint8_t key[16])
{
    if (!eui64 || !key) return 0;
    ENC_BEGIN(ZH_EZSP_ID_SEC_MAN_IMPORT_TRANSIENT_KEY);
    zh_ezsp_w_bytes(&w, eui64, 8);
    zh_ezsp_w_bytes(&w, key, 16);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_read_counters(uint8_t *buf, size_t cap, uint8_t seq)
{
    ENC_BEGIN(ZH_EZSP_ID_READ_COUNTERS);
    return zh_ezsp_w_end(&w);
}

size_t zh_ezsp_enc_get_child_data(uint8_t *buf, size_t cap, uint8_t seq, uint8_t index)
{
    ENC_BEGIN(ZH_EZSP_ID_GET_CHILD_DATA);
    zh_ezsp_w_u8(&w, index);
    return zh_ezsp_w_end(&w);
}

/* ---------------------------------------------------------------- 解码 */

/* 拿一个参数读取器的副本：头要是一条正常的应答 / 回调，帧 ID 要对 */
static int take(const zh_ezsp_frame_t *fr, uint16_t id, zh_ezsp_r_t *r)
{
    if (!fr || !r) return ZH_EZSP_E_ARG;
    /* parse 已经拒过这些；再查一遍是给漏看 parse 返回值的调用方兜底（回显的 nop 不能被当成保活应答） */
    if ((fr->fc & ZH_EZSP_FC_RESPONSE) == 0) return ZH_EZSP_E_DIRECTION;
    if (fr->fc & ZH_EZSP_FC_TRUNCATED) return ZH_EZSP_E_TRUNCATED;
    if (!fr->legacy && (fr->fc_hi & ZH_EZSP_FCH_RESERVED_MASK)) return ZH_EZSP_E_RESERVED;
    if (fr->legacy && id != ZH_EZSP_ID_VERSION) return ZH_EZSP_E_ID;   /* 旧式头只给首条 version 的应答 */
    if (fr->id != id) return ZH_EZSP_E_ID;
    *r = fr->params;
    r->off = 0;
    r->err = false;
    return ZH_EZSP_OK;
}

/* 参数必须恰好读完：多一个少一个都拒（CRC 结构性漏洞，见头文件） */
static int fin(const zh_ezsp_r_t *r)
{
    return (!r->err && r->off == r->n) ? ZH_EZSP_OK : ZH_EZSP_E_LENGTH;
}

#define DEC_BEGIN(id)                                                         \
    zh_ezsp_r_t r;                                                            \
    int _e = take(fr, (id), &r);                                              \
    if (_e != ZH_EZSP_OK) return _e

int zh_ezsp_dec_version(const zh_ezsp_frame_t *fr, uint8_t *protocol, uint8_t *stack_type, uint16_t *stack_version)
{
    DEC_BEGIN(ZH_EZSP_ID_VERSION);
    const uint8_t p = zh_ezsp_r_u8(&r);
    const uint8_t t = zh_ezsp_r_u8(&r);
    const uint16_t v = zh_ezsp_r_u16(&r);
    if (protocol) *protocol = p;
    if (stack_type) *stack_type = t;
    if (stack_version) *stack_version = v;
    return fin(&r);
}

int zh_ezsp_dec_get_config(const zh_ezsp_frame_t *fr, uint32_t *status, uint16_t *value)
{
    DEC_BEGIN(ZH_EZSP_ID_GET_CONFIGURATION_VALUE);
    const uint32_t s = zh_ezsp_r_u32(&r);
    const uint16_t v = zh_ezsp_r_u16(&r);
    if (status) *status = s;
    if (value) *value = v;
    return fin(&r);
}

/* 回包恰好只有一个 4 B 状态的命令（command-functions.h）。不在表里的 ID 不许借道：version 的回包也恰好 4 B */
static const uint16_t k_status_only[] = {
    ZH_EZSP_ID_SET_CONFIGURATION_VALUE, ZH_EZSP_ID_ADD_ENDPOINT, ZH_EZSP_ID_SET_POLICY, ZH_EZSP_ID_SET_VALUE,
    ZH_EZSP_ID_NETWORK_INIT, ZH_EZSP_ID_START_SCAN, ZH_EZSP_ID_FORM_NETWORK, ZH_EZSP_ID_LEAVE_NETWORK,
    ZH_EZSP_ID_PERMIT_JOINING, ZH_EZSP_ID_SET_RADIO_POWER, ZH_EZSP_ID_SET_CONCENTRATOR, ZH_EZSP_ID_SET_INITIAL_SECURITY_STATE,
    ZH_EZSP_ID_SEC_MAN_IMPORT_TRANSIENT_KEY,
};
static const uint16_t k_empty[] = { ZH_EZSP_ID_NOP, ZH_EZSP_ID_CLEAR_TRANSIENT_LINK_KEYS, ZH_EZSP_ID_NO_CALLBACKS };

static bool in_list(uint16_t id, const uint16_t *l, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (l[i] == id) return true;
    return false;
}

int zh_ezsp_dec_status(const zh_ezsp_frame_t *fr, uint16_t id, uint32_t *status)
{
    if (!in_list(id, k_status_only, sizeof(k_status_only) / sizeof(k_status_only[0]))) return ZH_EZSP_E_ARG;
    DEC_BEGIN(id);
    const uint32_t s = zh_ezsp_r_u32(&r);
    if (status) *status = s;
    return fin(&r);
}

int zh_ezsp_dec_empty(const zh_ezsp_frame_t *fr, uint16_t id)
{
    if (!in_list(id, k_empty, sizeof(k_empty) / sizeof(k_empty[0]))) return ZH_EZSP_E_ARG;
    DEC_BEGIN(id);
    return fin(&r);
}

int zh_ezsp_dec_get_value(const zh_ezsp_frame_t *fr, uint32_t *status, uint8_t *len, const uint8_t **value)
{
    DEC_BEGIN(ZH_EZSP_ID_GET_VALUE);
    const uint32_t s = zh_ezsp_r_u32(&r);
    const uint8_t l = zh_ezsp_r_u8(&r);
    const uint8_t *v = zh_ezsp_r_ptr(&r, l);
    if (status) *status = s;
    if (len) *len = l;
    if (value) *value = v;
    return fin(&r);
}

int zh_ezsp_dec_custom_frame(const zh_ezsp_frame_t *fr, uint32_t *status, uint8_t *len, const uint8_t **reply)
{
    DEC_BEGIN(ZH_EZSP_ID_CUSTOM_FRAME);
    const uint32_t s = zh_ezsp_r_u32(&r);
    const uint8_t l = zh_ezsp_r_u8(&r);
    const uint8_t *v = zh_ezsp_r_ptr(&r, l);
    if (status) *status = s;
    if (len) *len = l;
    if (reply) *reply = v;
    return fin(&r);
}

int zh_ezsp_dec_get_eui64(const zh_ezsp_frame_t *fr, uint8_t eui64[8])
{
    DEC_BEGIN(ZH_EZSP_ID_GET_EUI64);
    zh_ezsp_r_bytes(&r, eui64, 8);
    return fin(&r);
}

int zh_ezsp_dec_get_node_id(const zh_ezsp_frame_t *fr, uint16_t *node_id)
{
    DEC_BEGIN(ZH_EZSP_ID_GET_NODE_ID);
    const uint16_t v = zh_ezsp_r_u16(&r);
    if (node_id) *node_id = v;
    return fin(&r);
}

int zh_ezsp_dec_network_state(const zh_ezsp_frame_t *fr, uint8_t *state)
{
    DEC_BEGIN(ZH_EZSP_ID_NETWORK_STATE);
    const uint8_t v = zh_ezsp_r_u8(&r);
    if (state) *state = v;
    return fin(&r);
}

int zh_ezsp_dec_source_route_discovery_mode(const zh_ezsp_frame_t *fr, uint32_t *remaining_ms)
{
    DEC_BEGIN(ZH_EZSP_ID_SET_SOURCE_ROUTE_DISCOVERY_MODE);
    const uint32_t v = zh_ezsp_r_u32(&r);               /* command-functions.h:2393 fetchInt32u() */
    if (remaining_ms) *remaining_ms = v;
    return fin(&r);
}

static void r_net_params(zh_ezsp_r_t *r, zh_ezsp_net_params_t *p)
{
    /* ezsp-frame-utilities.c:121-131 fetch_sl_zigbee_network_parameters_t */
    zh_ezsp_net_params_t t;
    zh_ezsp_r_bytes(r, t.ext_pan_id, 8);
    t.pan_id = zh_ezsp_r_u16(r);
    t.tx_power = (int8_t)zh_ezsp_r_u8(r);
    t.channel = zh_ezsp_r_u8(r);
    t.join_method = zh_ezsp_r_u8(r);
    t.nwk_manager_id = zh_ezsp_r_u16(r);
    t.nwk_update_id = zh_ezsp_r_u8(r);
    t.channels = zh_ezsp_r_u32(r);
    if (p) *p = t;
}

int zh_ezsp_dec_get_network_parameters(const zh_ezsp_frame_t *fr, uint32_t *status, uint8_t *node_type, zh_ezsp_net_params_t *p)
{
    DEC_BEGIN(ZH_EZSP_ID_GET_NETWORK_PARAMETERS);
    const uint32_t s = zh_ezsp_r_u32(&r);
    const uint8_t t = zh_ezsp_r_u8(&r);
    r_net_params(&r, p);
    if (status) *status = s;
    if (node_type) *node_type = t;
    return fin(&r);
}

int zh_ezsp_dec_send(const zh_ezsp_frame_t *fr, uint16_t id, uint32_t *status, uint8_t *aps_seq)
{
    if (id != ZH_EZSP_ID_SEND_UNICAST && id != ZH_EZSP_ID_SEND_BROADCAST) return ZH_EZSP_E_ARG;
    DEC_BEGIN(id);
    const uint32_t s = zh_ezsp_r_u32(&r);
    const uint8_t q = zh_ezsp_r_u8(&r);
    if (status) *status = s;
    if (aps_seq) *aps_seq = q;
    return fin(&r);
}

int zh_ezsp_dec_lookup_eui64(const zh_ezsp_frame_t *fr, uint32_t *status, uint8_t eui64[8])
{
    DEC_BEGIN(ZH_EZSP_ID_LOOKUP_EUI64_BY_NODE_ID);
    const uint32_t s = zh_ezsp_r_u32(&r);
    zh_ezsp_r_bytes(&r, eui64, 8);
    if (status) *status = s;
    return fin(&r);
}

int zh_ezsp_dec_lookup_node_id(const zh_ezsp_frame_t *fr, uint32_t *status, uint16_t *node_id)
{
    DEC_BEGIN(ZH_EZSP_ID_LOOKUP_NODE_ID_BY_EUI64);
    const uint32_t s = zh_ezsp_r_u32(&r);
    const uint16_t v = zh_ezsp_r_u16(&r);
    if (status) *status = s;
    if (node_id) *node_id = v;
    return fin(&r);
}

int zh_ezsp_dec_get_current_security_state(const zh_ezsp_frame_t *fr, uint32_t *status, uint16_t *bitmask, uint8_t tc_eui64[8])
{
    DEC_BEGIN(ZH_EZSP_ID_GET_CURRENT_SECURITY_STATE);
    const uint32_t s = zh_ezsp_r_u32(&r);
    const uint16_t b = zh_ezsp_r_u16(&r);          /* ezsp-frame-utilities.c:237-241：bitmask + TC 长地址，不含任何密钥 */
    zh_ezsp_r_bytes(&r, tc_eui64, 8);
    if (status) *status = s;
    if (bitmask) *bitmask = b;
    return fin(&r);
}

/* ---- 回调 ---- */

int zh_ezsp_dec_stack_status(const zh_ezsp_frame_t *fr, uint32_t *status)
{
    DEC_BEGIN(ZH_EZSP_ID_STACK_STATUS_HANDLER);
    const uint32_t s = zh_ezsp_r_u32(&r);
    if (status) *status = s;
    return fin(&r);
}

int zh_ezsp_dec_energy_scan_result(const zh_ezsp_frame_t *fr, uint8_t *channel, int8_t *max_rssi)
{
    DEC_BEGIN(ZH_EZSP_ID_ENERGY_SCAN_RESULT_HANDLER);
    const uint8_t c = zh_ezsp_r_u8(&r);
    const int8_t v = (int8_t)zh_ezsp_r_u8(&r);
    if (channel) *channel = c;
    if (max_rssi) *max_rssi = v;
    return fin(&r);
}

int zh_ezsp_dec_scan_complete(const zh_ezsp_frame_t *fr, uint8_t *channel, uint32_t *status)
{
    DEC_BEGIN(ZH_EZSP_ID_SCAN_COMPLETE_HANDLER);
    const uint8_t c = zh_ezsp_r_u8(&r);
    const uint32_t s = zh_ezsp_r_u32(&r);
    if (channel) *channel = c;
    if (status) *status = s;
    return fin(&r);
}

int zh_ezsp_dec_child_join(const zh_ezsp_frame_t *fr, uint8_t *index, bool *joining, uint16_t *child_id, uint8_t child_eui64[8], uint8_t *child_type)
{
    DEC_BEGIN(ZH_EZSP_ID_CHILD_JOIN_HANDLER);
    const uint8_t i = zh_ezsp_r_u8(&r);
    const uint8_t j = zh_ezsp_r_u8(&r);
    const uint16_t c = zh_ezsp_r_u16(&r);
    zh_ezsp_r_bytes(&r, child_eui64, 8);
    const uint8_t t = zh_ezsp_r_u8(&r);
    if (index) *index = i;
    if (joining) *joining = j != 0;
    if (child_id) *child_id = c;
    if (child_type) *child_type = t;
    return fin(&r);
}

int zh_ezsp_dec_tc_post_join(const zh_ezsp_frame_t *fr, uint16_t *node_id, uint8_t eui64[8], uint8_t *status, uint8_t *policy_decision,
                             uint16_t *parent_id)
{
    DEC_BEGIN(ZH_EZSP_ID_TRUST_CENTER_POST_JOIN_HANDLER);
    const uint16_t n = zh_ezsp_r_u16(&r);
    zh_ezsp_r_bytes(&r, eui64, 8);
    const uint8_t s = zh_ezsp_r_u8(&r);            /* 这里的 status 是 1 B（sl_zigbee_device_update_t），不是 sl_status_t */
    const uint8_t d = zh_ezsp_r_u8(&r);
    const uint16_t p = zh_ezsp_r_u16(&r);
    if (node_id) *node_id = n;
    if (status) *status = s;
    if (policy_decision) *policy_decision = d;
    if (parent_id) *parent_id = p;
    return fin(&r);
}

static void r_aps(zh_ezsp_r_t *r, zh_ezsp_aps_t *a)
{
    /* ezsp-frame-utilities.c:151-160 fetch_sl_zigbee_aps_frame_t */
    zh_ezsp_aps_t t;
    t.profile_id = zh_ezsp_r_u16(r);
    t.cluster_id = zh_ezsp_r_u16(r);
    t.src_ep = zh_ezsp_r_u8(r);
    t.dst_ep = zh_ezsp_r_u8(r);
    t.options = zh_ezsp_r_u16(r);
    t.group_id = zh_ezsp_r_u16(r);
    t.sequence = zh_ezsp_r_u8(r);
    if (a) *a = t;
}

int zh_ezsp_dec_message_sent(const zh_ezsp_frame_t *fr, uint32_t *status, uint8_t *type, uint16_t *index_or_dest, zh_ezsp_aps_t *aps,
                             uint16_t *tag, uint8_t *len, const uint8_t **msg)
{
    DEC_BEGIN(ZH_EZSP_ID_MESSAGE_SENT_HANDLER);
    const uint32_t s = zh_ezsp_r_u32(&r);
    const uint8_t t = zh_ezsp_r_u8(&r);
    const uint16_t d = zh_ezsp_r_u16(&r);
    r_aps(&r, aps);
    const uint16_t g = zh_ezsp_r_u16(&r);
    const uint8_t l = zh_ezsp_r_u8(&r);
    const uint8_t *m = zh_ezsp_r_ptr(&r, l);
    if (status) *status = s;
    if (type) *type = t;
    if (index_or_dest) *index_or_dest = d;
    if (tag) *tag = g;
    if (len) *len = l;
    if (msg) *msg = m;
    return fin(&r);
}

int zh_ezsp_dec_incoming_message(const zh_ezsp_frame_t *fr, uint8_t *type, zh_ezsp_aps_t *aps, zh_ezsp_rx_info_t *info, uint8_t *len,
                                 const uint8_t **msg, uint8_t *src_route_overhead)
{
    DEC_BEGIN(ZH_EZSP_ID_INCOMING_MESSAGE_HANDLER);
    const uint8_t t = zh_ezsp_r_u8(&r);
    r_aps(&r, aps);
    /* ezsp-frame-utilities.c:162-171 fetch_sl_zigbee_rx_packet_info_t */
    zh_ezsp_rx_info_t ri;
    ri.sender_short = zh_ezsp_r_u16(&r);
    zh_ezsp_r_bytes(&r, ri.sender_long, 8);
    ri.binding_index = zh_ezsp_r_u8(&r);
    ri.address_index = zh_ezsp_r_u8(&r);
    ri.last_hop_lqi = zh_ezsp_r_u8(&r);
    ri.last_hop_rssi = (int8_t)zh_ezsp_r_u8(&r);
    ri.last_hop_timestamp = zh_ezsp_r_u32(&r);
    const uint8_t l = zh_ezsp_r_u8(&r);
    const uint8_t *m = zh_ezsp_r_ptr(&r, l);
    const uint8_t ov = zh_ezsp_r_u8(&r);              /* 源路由开销：分发函数之外的那个字节（头文件，ezsp.c:752-757） */
    if (type) *type = t;
    if (info) *info = ri;
    if (len) *len = l;
    if (msg) *msg = m;
    if (src_route_overhead) *src_route_overhead = ov;
    return fin(&r);
}

int zh_ezsp_dec_incoming_route_record(const zh_ezsp_frame_t *fr, uint16_t *source, uint8_t source_eui[8], uint8_t *lqi, int8_t *rssi,
                                      uint8_t *relay_count, const uint8_t **relays)
{
    DEC_BEGIN(ZH_EZSP_ID_INCOMING_ROUTE_RECORD_HANDLER);
    const uint16_t s = zh_ezsp_r_u16(&r);
    zh_ezsp_r_bytes(&r, source_eui, 8);
    const uint8_t q = zh_ezsp_r_u8(&r);
    const int8_t v = (int8_t)zh_ezsp_r_u8(&r);
    const uint8_t c = zh_ezsp_r_u8(&r);
    const uint8_t *rl = zh_ezsp_r_ptr(&r, (size_t)c * 2);
    if (source) *source = s;
    if (lqi) *lqi = q;
    if (rssi) *rssi = v;
    if (relay_count) *relay_count = c;
    if (relays) *relays = rl;
    return fin(&r);
}

int zh_ezsp_dec_id_conflict(const zh_ezsp_frame_t *fr, uint16_t *id)
{
    DEC_BEGIN(ZH_EZSP_ID_ID_CONFLICT_HANDLER);
    const uint16_t v = zh_ezsp_r_u16(&r);
    if (id) *id = v;
    return fin(&r);
}

int zh_ezsp_dec_custom_frame_handler(const zh_ezsp_frame_t *fr, uint8_t *len, const uint8_t **payload)
{
    DEC_BEGIN(ZH_EZSP_ID_CUSTOM_FRAME_HANDLER);
    const uint8_t l = zh_ezsp_r_u8(&r);
    const uint8_t *p = zh_ezsp_r_ptr(&r, l);
    if (len) *len = l;
    if (payload) *payload = p;
    return fin(&r);
}

int zh_ezsp_dec_read_counters(const zh_ezsp_frame_t *fr, uint16_t v[ZH_EZSP_COUNTER_N])
{
    DEC_BEGIN(ZH_EZSP_ID_READ_COUNTERS);
    for (unsigned i = 0; i < ZH_EZSP_COUNTER_N; i++) {   /* fetchInt16uArray(SL_ZIGBEE_COUNTER_TYPE_COUNT, …)：没有长度前缀 */
        const uint16_t x = zh_ezsp_r_u16(&r);
        if (v) v[i] = x;
    }
    return fin(&r);
}

int zh_ezsp_dec_child_data(const zh_ezsp_frame_t *fr, uint32_t *status, zh_ezsp_child_t *c)
{
    DEC_BEGIN(ZH_EZSP_ID_GET_CHILD_DATA);
    const uint32_t s = zh_ezsp_r_u32(&r);
    zh_ezsp_child_t t;
    zh_ezsp_r_bytes(&r, t.eui64, 8);
    t.type = zh_ezsp_r_u8(&r);
    t.id = zh_ezsp_r_u16(&r);
    t.phy = zh_ezsp_r_u8(&r);
    t.power = zh_ezsp_r_u8(&r);
    t.timeout = zh_ezsp_r_u8(&r);
    t.remaining = zh_ezsp_r_u32(&r);
    if (status) *status = s;
    if (c) *c = t;
    return fin(&r);
}
