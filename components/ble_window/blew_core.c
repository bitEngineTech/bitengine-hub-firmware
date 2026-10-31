/* blew_core —— ble_window 的纯判定（见 blew_core.h）。不依赖 ESP-IDF。 */
#include "blew_core.h"

bool blew_boot_decide(uint32_t magic, uint32_t magic_inv, bool sw_reset, bool safe_mode)
{
    if (!sw_reset || safe_mode) return false;
    return magic == BLEW_PAIR_MAGIC && magic_inv == (uint32_t)~BLEW_PAIR_MAGIC;
}

void blew_boot_arm(uint32_t *magic, uint32_t *magic_inv)
{
    if (!magic || !magic_inv) return;
    *magic = BLEW_PAIR_MAGIC;
    *magic_inv = (uint32_t)~BLEW_PAIR_MAGIC;
}

blew_req_verdict_t blew_req_check(int owner, int role, uint32_t max_ms, bool window_open, bool pair_boot)
{
    uint32_t limit;
    if (owner == BLEW_CORE_OWNER_MUSE_PAIR) {
        limit = BLEW_MUSE_MAX_MS;
    } else if (owner == BLEW_CORE_OWNER_MATTER) {
        limit = BLEW_MATTER_MAX_MS;
    } else {
        return BLEW_REQ_BAD_ARG;
    }
    if (role != BLEW_CORE_ROLE_PERIPHERAL && role != BLEW_CORE_ROLE_CENTRAL) return BLEW_REQ_BAD_ARG;
    if (max_ms == 0 || max_ms > limit) return BLEW_REQ_BAD_ARG;
    if (window_open) return BLEW_REQ_BUSY;
    /* 策略 C：BT 只在配对剖面里起（正常开机已经 esp_bt_mem_release，起不来）；Matter 与中心角色是 P3 */
    if (owner != BLEW_CORE_OWNER_MUSE_PAIR || role != BLEW_CORE_ROLE_PERIPHERAL || !pair_boot) {
        return BLEW_REQ_NOT_SUPPORTED;
    }
    return BLEW_REQ_OK;
}

bool blew_mem_gate(uint32_t internal_free, uint32_t dma_free, uint32_t need_internal, uint32_t need_dma)
{
    return internal_free >= need_internal && dma_free >= need_dma;
}

/* ---------------- Thread 自检 ---------------- */

uint16_t blew_permille(uint32_t num, uint32_t den)
{
    if (den == 0) return 0;
    if (num >= den) return 1000;
    return (uint16_t)(((uint64_t)num * 1000u) / den);
}

blew_sc_result_t blew_sc_verdict(const blew_sc_input_t *in)
{
    if (!in) return BLEW_SC_INCONCLUSIVE;
    const bool ack_judged = in->after_req >= BLEW_SC_MIN_FRAMES;
    bool ack_ok = true;
    if (ack_judged) {
        uint32_t thr = 500;                                          /* 绝对下限 0.5 */
        if (in->base_req >= BLEW_SC_MIN_FRAMES) {
            const uint32_t rel = (uint32_t)blew_permille(in->base_acked, in->base_req) * 8u / 10u;   /* 0.8 × 基线 */
            if (rel > thr) thr = rel;
        }
        ack_ok = blew_permille(in->after_acked, in->after_req) >= thr;
    }
    const bool children_judged = in->judge_children && in->children_before > 0;
    const bool children_ok = !children_judged
                             || (uint32_t)in->children_after * 10u >= (uint32_t)in->children_before * 9u;
    if (!ack_judged && !children_judged) return BLEW_SC_INCONCLUSIVE;
    if (!ack_ok && !children_ok) return BLEW_SC_DEGRADED_BOTH;
    if (!ack_ok) return BLEW_SC_DEGRADED_ACK;
    if (!children_ok) return BLEW_SC_DEGRADED_CHILDREN;
    return BLEW_SC_PASS;
}

const char *blew_sc_str(blew_sc_result_t r)
{
    switch (r) {
    case BLEW_SC_NONE:              return "none";
    case BLEW_SC_PASS:              return "pass";
    case BLEW_SC_DEGRADED_ACK:      return "degraded_ack";
    case BLEW_SC_DEGRADED_CHILDREN: return "degraded_children";
    case BLEW_SC_DEGRADED_BOTH:     return "degraded_both";
    case BLEW_SC_INCONCLUSIVE:      return "inconclusive";
    case BLEW_SC_NO_THREAD:         return "no_thread";
    case BLEW_SC_INTERRUPTED:       return "interrupted";
    case BLEW_SC_MAX:               break;
    }
    return "unknown";
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

void blew_res_encode(const blew_res_t *r, uint8_t out[BLEW_RES_BLOB_LEN])
{
    if (!r || !out) return;
    out[0] = r->last;
    out[1] = r->consecutive_degraded;
    put16(out + 2, r->total);
    put16(out + 4, r->before_pm);
    put16(out + 6, r->after_pm);
    out[8] = r->children_before;
    out[9] = r->children_after;
    out[10] = (uint8_t)r->epoch; out[11] = (uint8_t)(r->epoch >> 8);
    out[12] = (uint8_t)(r->epoch >> 16); out[13] = (uint8_t)(r->epoch >> 24);
}

bool blew_res_decode(const uint8_t *in, size_t len, blew_res_t *r)
{
    if (!in || !r || len < BLEW_RES_BLOB_LEN || in[0] >= BLEW_SC_MAX) return false;
    r->last = in[0];
    r->consecutive_degraded = in[1];
    r->total = get16(in + 2);
    r->before_pm = get16(in + 4);
    r->after_pm = get16(in + 6);
    r->children_before = in[8];
    r->children_after = in[9];
    r->epoch = (uint32_t)in[10] | ((uint32_t)in[11] << 8) | ((uint32_t)in[12] << 16) | ((uint32_t)in[13] << 24);
    return true;
}

void blew_res_update(blew_res_t *out, const blew_res_t *prev, blew_sc_result_t r, const blew_sc_input_t *in, uint32_t epoch)
{
    if (!out) return;
    blew_res_t n = {0};
    if (prev) n = *prev;
    n.last = (uint8_t)r;
    if (n.total < 0xFFFF) n.total++;
    if (r == BLEW_SC_PASS) {
        n.consecutive_degraded = 0;
    } else if (r == BLEW_SC_DEGRADED_ACK || r == BLEW_SC_DEGRADED_CHILDREN || r == BLEW_SC_DEGRADED_BOTH) {
        if (n.consecutive_degraded < 0xFF) n.consecutive_degraded++;
    }   /* INCONCLUSIVE / NO_THREAD / INTERRUPTED：连续计数不动 */
    const bool judged = r == BLEW_SC_PASS || r == BLEW_SC_DEGRADED_ACK || r == BLEW_SC_DEGRADED_CHILDREN
                        || r == BLEW_SC_DEGRADED_BOTH;
    if (in && judged) {
        n.before_pm = blew_permille(in->base_acked, in->base_req);
        n.after_pm = blew_permille(in->after_acked, in->after_req);
        n.children_before = in->children_before;
        n.children_after = in->children_after;
    }
    n.epoch = epoch;
    *out = n;
}
