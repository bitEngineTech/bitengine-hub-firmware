/* mc_nodes_core —— 节点表（规则与格式见 mc_nodes_core.h）。纯逻辑，test_host/ 原样编。 */
#include "mc_nodes_core.h"

#include <string.h>

static void put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

void mc_nodes_init(mc_nodes_t *t, uint8_t max)
{
    if (!t) return;
    memset(t, 0, sizeof(*t));
    t->next_node = MC_NODE_FIRST;
    t->max = (max == 0 || max > MC_NODES_CAP) ? MC_NODES_CAP : max;
}

int mc_nodes_encode(const mc_nodes_t *t, uint8_t *buf, size_t cap)
{
    if (!t || !buf) return -MC_NODES_EARG;
    if (t->n > t->max || t->n > MC_NODES_CAP || t->next_node < MC_NODE_FIRST) return -MC_NODES_EBAD;
    const size_t need = 8 + 1 + (size_t)t->n * MC_NODES_REC_LEN;
    if (cap < need) return -MC_NODES_EARG;
    uint8_t *p = buf;
    put_u64(p, t->next_node); p += 8;
    *p++ = t->n;
    for (unsigned i = 0; i < t->n; i++) {
        const mc_node_t *r = &t->nodes[i];
        put_u64(p, r->node_id);
        p[8] = (uint8_t)r->vid; p[9] = (uint8_t)(r->vid >> 8);
        p[10] = (uint8_t)r->pid; p[11] = (uint8_t)(r->pid >> 8);
        p[12] = r->state; p[13] = r->flags; p[14] = 0; p[15] = 0;
        p += MC_NODES_REC_LEN;
    }
    return (int)(p - buf);
}

mc_nodes_err_t mc_nodes_decode(const uint8_t *buf, size_t len, uint8_t max, mc_nodes_t *out)
{
    if (!out) return MC_NODES_EARG;
    mc_nodes_init(out, max);
    if (!buf) return MC_NODES_EARG;
    if (len < 9) return MC_NODES_ELEN;
    const uint8_t n = buf[8];
    if (n > out->max || len != 9 + (size_t)n * MC_NODES_REC_LEN) return MC_NODES_ELEN;
    const uint64_t next = get_u64(buf);
    if (next < MC_NODE_FIRST) return MC_NODES_EBAD;
    mc_nodes_t t;
    mc_nodes_init(&t, out->max);
    t.next_node = next;
    const uint8_t *p = buf + 9;
    for (unsigned i = 0; i < n; i++, p += MC_NODES_REC_LEN) {
        mc_node_t r = { 0 };
        r.node_id = get_u64(p);
        r.vid = (uint16_t)(p[8] | (p[9] << 8));
        r.pid = (uint16_t)(p[10] | (p[11] << 8));
        r.state = p[12];
        r.flags = p[13];
        if (r.node_id < MC_NODE_FIRST || r.node_id >= next) return MC_NODES_EBAD;
        if (r.state != MC_NODE_PENDING && r.state != MC_NODE_OK) return MC_NODES_EBAD;
        if (mc_nodes_find(&t, r.node_id)) return MC_NODES_EBAD;
        t.nodes[t.n++] = r;
    }
    *out = t;
    return MC_NODES_OK;
}

mc_node_t *mc_nodes_find(mc_nodes_t *t, uint64_t node_id)
{
    if (!t) return NULL;
    for (unsigned i = 0; i < t->n; i++) if (t->nodes[i].node_id == node_id) return &t->nodes[i];
    return NULL;
}

mc_nodes_err_t mc_nodes_alloc(mc_nodes_t *t, uint16_t vid, uint16_t pid, uint64_t *out_node)
{
    if (!t || !out_node) return MC_NODES_EARG;
    if (t->n >= t->max) return MC_NODES_EFULL;
    if (t->next_node < MC_NODE_FIRST || t->next_node == UINT64_MAX) return MC_NODES_EBAD;
    mc_node_t *r = &t->nodes[t->n++];
    memset(r, 0, sizeof(*r));
    r->node_id = t->next_node++;
    r->vid = vid;
    r->pid = pid;
    r->state = MC_NODE_PENDING;
    *out_node = r->node_id;
    return MC_NODES_OK;
}

mc_nodes_err_t mc_nodes_set_state(mc_nodes_t *t, uint64_t node_id, mc_node_state_t st)
{
    mc_node_t *r = mc_nodes_find(t, node_id);
    if (!r) return MC_NODES_ENOTFOUND;
    if (st != MC_NODE_PENDING && st != MC_NODE_OK) return MC_NODES_EARG;
    r->state = (uint8_t)st;
    return MC_NODES_OK;
}

mc_nodes_err_t mc_nodes_remove(mc_nodes_t *t, uint64_t node_id)
{
    if (!t) return MC_NODES_EARG;
    for (unsigned i = 0; i < t->n; i++) {
        if (t->nodes[i].node_id != node_id) continue;
        memmove(&t->nodes[i], &t->nodes[i + 1], (size_t)(t->n - i - 1) * sizeof(t->nodes[0]));
        t->n--;
        memset(&t->nodes[t->n], 0, sizeof(t->nodes[0]));
        return MC_NODES_OK;
    }
    return MC_NODES_ENOTFOUND;
}

unsigned mc_nodes_drop_pending(mc_nodes_t *t)
{
    if (!t) return 0;
    unsigned dropped = 0;
    for (unsigned i = 0; i < t->n;) {
        if (t->nodes[i].state == MC_NODE_PENDING) {
            (void)mc_nodes_remove(t, t->nodes[i].node_id);
            dropped++;
        } else {
            i++;
        }
    }
    return dropped;
}
