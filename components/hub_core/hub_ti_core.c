/* hub_ti_core —— Thread 网络层清单的纯逻辑（见 include/hub_ti_core.h）。纯 C，不依赖 ESP-IDF。 */
#include "hub_ti_core.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "hub_sanitize.h"

#define RSSI_INVALID 127   /* OT_RADIO_RSSI_INVALID */

/* ---------------------------------------------------------------- 解析 */

static bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

bool ti_parse_matter_instance(const char *label, char cfid[17], char node[17])
{
    if (!label || strlen(label) != 33 || label[16] != '-') return false;
    for (int i = 0; i < 33; i++) {
        if (i != 16 && !is_hex(label[i])) return false;
    }
    for (int i = 0; i < 16; i++) {
        cfid[i] = (char)toupper((unsigned char)label[i]);
        node[i] = (char)toupper((unsigned char)label[17 + i]);
    }
    cfid[16] = node[16] = '\0';
    return true;
}

/* 十进制 0–65535，整串都是数字 */
static bool parse_u16(const char *s, size_t n, uint16_t *out)
{
    if (n == 0 || n > 5) return false;
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10u + (uint32_t)(s[i] - '0');
    }
    if (v > 65535u) return false;
    *out = (uint16_t)v;
    return true;
}

void ti_parse_matterc_txt(const uint8_t *txt, uint16_t len, ti_host_t *h)
{
    if (!h) return;
    h->has_vp = false;
    h->dn[0] = '\0';
    if (!txt) return;
    uint16_t i = 0;
    while (i < len) {
        const uint8_t elen = txt[i++];
        if (elen == 0) continue;
        if ((uint32_t)i + elen > len) break;                  /* 越界的条目：剩下的不要 */
        const char *e = (const char *)&txt[i];
        const char *eq = memchr(e, '=', elen);
        if (eq) {
            const size_t kl = (size_t)(eq - e), vl = (size_t)elen - kl - 1;
            const char *v = eq + 1;
            if (kl == 2 && memcmp(e, "VP", 2) == 0) {
                const char *plus = memchr(v, '+', vl);
                uint16_t vid = 0, pid = 0;
                if (plus && parse_u16(v, (size_t)(plus - v), &vid) && parse_u16(plus + 1, vl - (size_t)(plus - v) - 1, &pid)) {
                    h->vid = vid;
                    h->pid = pid;
                    h->has_vp = true;
                } else if (!plus && parse_u16(v, vl, &vid)) {     /* 只有 VID 也是合法的（规范：PID 可省） */
                    h->vid = vid;
                    h->pid = 0;
                    h->has_vp = true;
                }
            } else if (kl == 2 && memcmp(e, "DN", 2) == 0) {
                const size_t n = vl < TI_DN_MAX - 1 ? vl : TI_DN_MAX - 1;   /* 原样截到 32 字节；输出前再净化（UTF-8 截断由净化修好） */
                memcpy(h->dn, v, n);
                h->dn[n] = '\0';
            }
        }
        i = (uint16_t)(i + elen);
    }
}

/* 第一段（到第一个 '.' 为止）拷进 out；空、太长、没有 '.' 都不合格 */
static bool first_label(const char *s, char *out, size_t cap)
{
    if (!s || !out || cap == 0) return false;
    const char *dot = strchr(s, '.');
    const size_t n = dot ? (size_t)(dot - s) : strlen(s);
    if (n == 0 || n >= cap) return false;
    memcpy(out, s, n);
    out[n] = '\0';
    return true;
}

bool ti_service_is(const char *full_name, const char *type, const char *proto)
{
    char t[32], p[8];
    if (!first_label(full_name, t, sizeof(t))) return false;
    const char *dot = strchr(full_name, '.');
    if (!dot || !first_label(dot + 1, p, sizeof(p))) return false;
    return strcmp(t, type) == 0 && strcmp(p, proto) == 0;
}

bool ti_host_label(const char *full_name, char *out, size_t cap)
{
    return first_label(full_name, out, cap);
}

/* ---------------------------------------------------------------- id */

void ti_id_from_ext(const uint8_t ext[8], char out[TI_ID_MAX])
{
    static const char hex[] = "0123456789abcdef";
    memcpy(out, "th-", 3);
    for (int i = 0; i < 8; i++) {
        out[3 + 2 * i] = hex[ext[i] >> 4];
        out[4 + 2 * i] = hex[ext[i] & 0x0F];
    }
    out[19] = '\0';
}

void ti_id_from_host(const char *host, char out[TI_ID_MAX])
{
    static const char hex[] = "0123456789abcdef";
    memcpy(out, "th-h", 4);
    bool all_hex = host && strlen(host) >= 15;
    for (int i = 0; all_hex && i < 15; i++) all_hex = is_hex(host[i]);
    if (all_hex) {
        for (int i = 0; i < 15; i++) out[4 + i] = (char)tolower((unsigned char)host[i]);
    } else {
        uint64_t h = 1469598103934665603ull;                   /* FNV-1a 64 */
        for (const unsigned char *p = (const unsigned char *)(host ? host : ""); *p; p++) {
            h ^= *p;
            h *= 1099511628211ull;
        }
        for (int i = 0; i < 15; i++) out[4 + i] = hex[(h >> (4 * (14 - i))) & 0x0F];
    }
    out[19] = '\0';
}

/* ---------------------------------------------------------------- 关联 */

static bool ip6_eq(const ti_ip6_t *a, const ti_ip6_t *b) { return memcmp(a->b, b->b, 16) == 0; }

/* 清单前 upto 项里有没有这个 id */
static bool id_taken(const ti_list_t *l, uint16_t upto, const char *id)
{
    for (uint16_t i = 0; i < upto && i < l->n; i++) {
        if (strcmp(l->e[i].id, id) == 0) return true;
    }
    return false;
}

static int find_router(const ti_raw_t *raw, uint16_t rloc16)
{
    for (int i = 0; i < raw->n_routers; i++) {
        if (raw->routers[i].rloc16 == rloc16) return i;
    }
    return -1;
}

/* 上一轮这个主机名用过 th-<ext> 吗（关联不上时沿用，id 不来回变） */
static const char *sticky_id(const ti_prev_t *prev, const char *host)
{
    if (!prev || !host[0]) return NULL;
    for (size_t i = 0; i < sizeof(prev->p) / sizeof(prev->p[0]); i++) {
        const ti_prev_entry_t *p = &prev->p[i];
        if (p->used && strcmp(p->host, host) == 0 && strncmp(p->id, "th-h", 4) != 0) return p->id;
    }
    return NULL;
}

static ti_entry_t *add(ti_list_t *out)
{
    if (out->n >= sizeof(out->e) / sizeof(out->e[0])) return NULL;
    ti_entry_t *e = &out->e[out->n++];
    memset(e, 0, sizeof(*e));
    e->host = e->child = e->router = -1;
    e->rloc16 = e->parent = TI_RLOC_NONE;
    return e;
}

static bool ext_known(const uint8_t ext[8])
{
    /* OT 把不相邻的路由器加进路由表时条目是清空的，扩展地址全 0（router_table.cpp AddRouter → Clear）：当作"不知道" */
    for (int i = 0; i < 8; i++) {
        if (ext[i]) return true;
    }
    return false;
}

static int find_child_by_rloc(const ti_raw_t *raw, uint16_t rloc16)
{
    for (uint16_t i = 0; i < raw->n_children; i++) {
        if (raw->children[i].rloc16 == rloc16) return i;
    }
    return -1;
}

static int find_child_by_addr(const ti_raw_t *raw, const ti_host_t *h, const bool *used)
{
    for (uint16_t c = 0; c < raw->n_children; c++) {
        if (used[c]) continue;
        const ti_child_t *C = &raw->children[c];
        for (uint8_t a = 0; a < h->n_addrs; a++) {
            for (uint8_t b = 0; b < C->n_addrs; b++) {
                if (ip6_eq(&h->addrs[a], &C->addrs[b])) return c;
            }
        }
    }
    return -1;
}

static uint16_t cache_lookup(const ti_raw_t *raw, const ti_host_t *h)
{
    for (uint8_t a = 0; a < h->n_addrs; a++) {
        for (uint16_t k = 0; k < raw->n_cache; k++) {
            if (ip6_eq(&h->addrs[a], &raw->cache[k].addr)) return raw->cache[k].rloc16;
        }
    }
    return TI_RLOC_NONE;
}

/* 主机名本身是 16 位十六进制（Matter 的 MAC 派生主机名）时，它对应的扩展地址 id "th-<小写>"；不是返回 false */
static bool host_hex_id(const char *hn, char out[TI_ID_MAX])
{
    if (strlen(hn) != 16) return false;
    for (int k = 0; k < 16; k++) {
        if (!is_hex(hn[k])) return false;
    }
    memcpy(out, "th-", 3);
    for (int k = 0; k < 16; k++) out[3 + k] = (char)tolower((unsigned char)hn[k]);
    out[19] = '\0';
    return true;
}

/* 还没定 id 的主机条目是不是这台（上一轮粘连到这个 id、或主机名就是这个扩展地址）：是的话 ③④ 把子节点 / 路由器并进主机那条，
   而不是另列一条、第二遍再因撞车把主机那条（带 fabric、ready_to_add）丢掉（第六步第二轮审计 ❌） */
static ti_entry_t *host_entry_for(ti_list_t *out, const ti_raw_t *raw, const char *id)
{
    /* 主机名就是这个扩展地址的优先（确定的）；粘连（上一轮的关联可能是陈旧 EID 缓存配上被复用的 RLOC16 弄错的）其次 */
    for (int pass = 0; pass < 2; pass++) {
        for (uint16_t i = 0; i < out->n; i++) {
            ti_entry_t *x = &out->e[i];
            if (x->id[0] || x->host < 0) continue;
            char hx[TI_ID_MAX];
            const char *st = out->sticky[x->host];
            const bool hit = pass == 0 ? (host_hex_id(raw->hosts[x->host].name, hx) && strcmp(hx, id) == 0)
                                       : (st && strcmp(st, id) == 0);
            if (hit) {
                memcpy(x->id, id, TI_ID_MAX);
                return x;
            }
        }
    }
    return NULL;
}

static void set_child(ti_entry_t *e, const ti_raw_t *raw, int c)
{
    e->child = (int16_t)c;
    e->link = TI_LINK_CHILD;
    e->rloc16 = raw->children[c].rloc16;
    e->parent = raw->my_rloc16;
}

/* 两遍（第六步审计：一遍做的话，失效的旧主机名会先占住 th-<ext>，真正关联上的反而退回主机名 id）：
   第一遍只做"确定的"关联（子节点地址交集 / EID 缓存 → 本 BR 的子节点 / 路由器 / 挂在别的路由器下），能确定 id 的定下来；
   第二遍给关联不上的主机名分 id：上一轮用过的扩展地址 id（粘连）没人占就沿用，否则主机名派生。 */
/* 除 except 之外，有没有哪个主机的主机名（十六进制）就是这个扩展地址 id —— 有就是那台路由器自己的 Matter 主机在场 */
static bool other_host_is(const ti_raw_t *raw, uint16_t except, const char *rid)
{
    char hx[TI_ID_MAX];
    for (uint16_t k = 0; k < raw->n_hosts; k++) {
        if (k != except && host_hex_id(raw->hosts[k].name, hx) && strcmp(hx, rid) == 0) return true;
    }
    return false;
}

void ti_associate(const ti_raw_t *raw, const ti_prev_t *prev, ti_list_t *out)
{
    if (!out) return;
    out->n = 0;
    if (!raw) return;
    bool child_used[TI_MAX_CHILDREN] = { false };
    bool router_used[TI_MAX_ROUTERS] = { false };
    /* 每个主机的粘连 id 先算一次（第六步核实审计：在 ③④ 里对每个子节点 / 路由器 × 每个主机现查，最坏一千多万次比较、C5 上约 1 s） */
    for (uint16_t h = 0; h < raw->n_hosts; h++) out->sticky[h] = sticky_id(prev, raw->hosts[h].name);

    /* 第一遍：每个 SRP 主机一条，能确定的关联与 id 定下来 */
    for (uint16_t h = 0; h < raw->n_hosts; h++) {
        const ti_host_t *H = &raw->hosts[h];
        ti_entry_t *e = add(out);
        if (!e) break;
        e->host = (int16_t)h;
        e->link = TI_LINK_UNKNOWN;
        int c = find_child_by_addr(raw, H, child_used);          /* ① 直连子节点：地址交集（MTD 子节点向父节点登记地址） */
        if (c < 0) {
            const uint16_t r = cache_lookup(raw, H);             /* ② EID 缓存 → RLOC16 */
            if (r != TI_RLOC_NONE && r != raw->my_rloc16) {
                if ((raw->my_rloc16 & 0x01FFu) == 0 && (r & 0xFC00u) == (raw->my_rloc16 & 0xFC00u)) {   /* 本 BR 是路由器时才有子节点 */
                    /* 本 BR 的子节点（FTD 子节点不向父节点登记地址，① 必然落空：mle.cpp 的 FTD 分支），按 RLOC16 在子节点表里找 */
                    const int cc = find_child_by_rloc(raw, r);
                    if (cc >= 0 && !child_used[cc]) c = cc;
                } else if ((r & 0x01FFu) == 0) {                  /* 子节点号 0：多半是路由器自己 —— 但也可能是转发它的父路由器（见下） */
                    const int ri = find_router(raw, r);
                    char hx[TI_ID_MAX], rid[TI_ID_MAX] = "";
                    const bool hex16 = host_hex_id(H->name, hx);
                    if (ri >= 0 && ext_known(raw->routers[ri].ext)) ti_id_from_ext(raw->routers[ri].ext, rid);
                    /* 判"挂在它下面"：主机名是十六进制（= 自己的扩展地址）却对不上这台路由器；或主机名不是十六进制、而这台路由器自己的
                       Matter 主机（主机名 = 它的扩展地址）在场 —— 那这个主机就不是它（复核：非 Matter 子设备排在前面时同样会把路由器挤掉） */
                    if (rid[0] && (hex16 ? strcmp(hx, rid) != 0 : other_host_is(raw, h, rid))) {
                        /* EID 缓存里的 RLOC16 不一定是它自己的：OpenThread 嗅探收到的报文时记的是上一跳，子节点的报文由父路由器转发一跳到
                           本 BR，记下的就是父路由器的 RLOC16（子节点号 0）。主机名本身就是扩展地址（Matter，7.2 的 H1，2026-10-22 上板核实）
                           却对不上这台路由器 = 它是挂在这台路由器下面的设备，不是路由器自己。2026-10-22 上板：Eve 插座（路由器 0x9000）下的
                           Aqara 门窗传感器（0x9005）被当成 Eve，Eve 自己的主机因 id 冲突被丢、传感器被判"离开"并推给了 Muse */
                        e->link = TI_LINK_VIA_ROUTER;
                        e->rloc16 = TI_RLOC_NONE;            /* 它自己的 RLOC16 不知道：缓存里记的是父路由器的（不然 hub.device.get 报出与路由器相同的 RLOC16） */
                        e->parent = r;
                    } else {
                        e->link = TI_LINK_ROUTER;
                        e->rloc16 = r;
                        if (ri >= 0 && !router_used[ri]) {
                            e->router = (int16_t)ri;
                            router_used[ri] = true;
                            if (rid[0]) memcpy(e->id, rid, sizeof(rid));
                        }
                    }
                } else {
                    e->link = TI_LINK_VIA_ROUTER;
                    e->rloc16 = r;
                    e->parent = (uint16_t)(r & 0xFC00u);
                }
            }
        }
        if (c >= 0) {
            child_used[c] = true;
            set_child(e, raw, c);
            ti_id_from_ext(raw->children[c].ext, e->id);
        }
        /* 关联不上（或路由器的扩展地址不知道）的留给第二遍定 id。
           第一遍里两个主机定到同一个 id（一台路由器注册了几个主机名）：后来的也留给第二遍 */
        if (e->id[0] && id_taken(out, (uint16_t)(out->n - 1), e->id)) e->id[0] = '\0';
    }

    /* ③ 没注册 SRP 服务的子节点。id 已被某个主机占了、或某个还没定 id 的主机上一轮就是它（粘连）= 就是它：并进那条，不另列
       （第六步审计：子节点重新登记了地址、暂时对不上时，不然会列两条同 id 的） */
    for (uint16_t c = 0; c < raw->n_children; c++) {
        if (child_used[c]) continue;
        char id[TI_ID_MAX];
        ti_id_from_ext(raw->children[c].ext, id);
        ti_entry_t *dup = NULL;
        for (uint16_t i = 0; i < out->n && !dup; i++) {
            if (strcmp(out->e[i].id, id) == 0) dup = &out->e[i];
        }
        if (!dup) dup = host_entry_for(out, raw, id);
        if (dup) {
            if (dup->child < 0) set_child(dup, raw, c);
            child_used[c] = true;
            continue;
        }
        ti_entry_t *e = add(out);
        if (!e) break;
        child_used[c] = true;
        set_child(e, raw, c);
        memcpy(e->id, id, sizeof(id));
    }
    /* ④ 没注册 SRP 服务的路由器（不含本 BR 自己；扩展地址不知道的不列：没有稳定的 id） */
    for (uint8_t r = 0; r < raw->n_routers; r++) {
        if (router_used[r] || raw->routers[r].rloc16 == raw->my_rloc16 || !ext_known(raw->routers[r].ext)) continue;
        char id[TI_ID_MAX];
        ti_id_from_ext(raw->routers[r].ext, id);
        if (id_taken(out, out->n, id)) continue;
        ti_entry_t *h = host_entry_for(out, raw, id);    /* 这台路由器注册了 SRP（EID 缓存里没它）：并进主机那条 */
        if (h) {
            h->router = (int16_t)r;
            h->link = TI_LINK_ROUTER;
            h->rloc16 = raw->routers[r].rloc16;
            h->parent = TI_RLOC_NONE;                    /* 它就是路由器：第一遍若按陈旧缓存记成"挂在别的路由器下"，父节点要清掉 */
            router_used[r] = true;
            continue;
        }
        ti_entry_t *e = add(out);
        if (!e) break;
        e->router = (int16_t)r;
        e->link = TI_LINK_ROUTER;
        e->rloc16 = raw->routers[r].rloc16;
        memcpy(e->id, id, sizeof(id));
    }

    /* 第二遍：还没有 id 的主机名 —— 粘连的扩展地址 id 没人占就沿用，否则主机名派生；那也撞了就丢掉（免得 get 找错设备） */
    uint16_t w = 0;
    for (uint16_t i = 0; i < out->n; i++) {
        ti_entry_t *e = &out->e[i];
        if (!e->id[0] && e->host >= 0) {
            const char *hn = raw->hosts[e->host].name;
            const char *s = out->sticky[e->host];
            char hx[TI_ID_MAX];
            const bool hex16 = host_hex_id(hn, hx);
            if (s && !id_taken(out, out->n, s)) {
                snprintf(e->id, sizeof(e->id), "%s", s);      /* 上一轮关联到过的扩展地址：沿用，id 不来回变 */
            } else if (hex16) {
                /* 16 位十六进制主机名（Matter 的 MAC 派生主机名）就是扩展地址：th-<小写>，与按扩展地址生成的 id 一致（7.2 的 H1） */
                memcpy(e->id, hx, sizeof(hx));
            } else {
                ti_id_from_host(hn, e->id);
            }
            /* 只跟已定下的比（自己之前的、和后面已有 id 的） */
            bool clash = false;
            for (uint16_t k = 0; k < out->n && !clash; k++) clash = k != i && out->e[k].id[0] && strcmp(out->e[k].id, e->id) == 0;
            if (clash) continue;                                  /* 丢掉：不拷进 w */
        }
        if (w != i) out->e[w] = *e;
        w++;
    }
    out->n = w;
}

/* ---------------------------------------------------------------- 增减 */

static ti_prev_entry_t *prev_find(ti_prev_t *prev, const char *id)
{
    for (size_t i = 0; i < sizeof(prev->p) / sizeof(prev->p[0]); i++) {
        if (prev->p[i].used && strcmp(prev->p[i].id, id) == 0) return &prev->p[i];
    }
    return NULL;
}

static ti_prev_entry_t *prev_free(ti_prev_t *prev)
{
    for (size_t i = 0; i < sizeof(prev->p) / sizeof(prev->p[0]); i++) {
        if (!prev->p[i].used) return &prev->p[i];
    }
    return NULL;
}

static bool in_list(const ti_list_t *l, const char *id)
{
    for (uint16_t i = 0; i < l->n; i++) {
        if (strcmp(l->e[i].id, id) == 0) return true;
    }
    return false;
}

void ti_diff(ti_prev_t *prev, const ti_raw_t *raw, const ti_list_t *cur, uint64_t now_ms, ti_change_fn fn, void *ctx)
{
    if (!prev || !cur) return;
    if (!prev->have_baseline) prev->baseline_ms = now_ms;
    /* 建基线后的静默期里新出现的只并入基线、不报"新来"：开机 / 掉网恢复 / 重开 Thread 之后 SRP 主机要陆续重新注册（成批约 10 s，
       休眠设备几分钟），不然会报一大串假的"新来"、把事件环冲掉（第六步第二轮审计 ❌）。离开照报（基线里有、后来没了才算） */
    const bool report = prev->have_baseline;
    const bool report_join = report && now_ms >= prev->baseline_ms && now_ms - prev->baseline_ms >= TI_QUIET_MS;
    bool seen[sizeof(prev->p) / sizeof(prev->p[0])] = { false };
    for (uint16_t i = 0; i < cur->n; i++) {
        const ti_entry_t *e = &cur->e[i];
        const char *hname = (raw && e->host >= 0) ? raw->hosts[e->host].name : "";
        ti_prev_entry_t *p = prev_find(prev, e->id);
        if (!p && hname[0]) {
            /* 同一个主机名上一轮用的是别的 id、而那个 id 这一轮不在了：是同一台设备换了 id，原地改名 */
            for (size_t k = 0; k < sizeof(prev->p) / sizeof(prev->p[0]) && !p; k++) {
                ti_prev_entry_t *q = &prev->p[k];
                if (q->used && !seen[k] && strcmp(q->host, hname) == 0 && !in_list(cur, q->id)) {
                    snprintf(q->id, sizeof(q->id), "%s", e->id);
                    p = q;
                }
            }
        }
        if (p && hname[0] && !p->host[0]) {
            /* 命中的是上一轮没主机名的那条（子节点 / 路由器先单独列过）：同一台设备上一轮可能还以主机名 id 列着另一条（关联不上时），
               那条这一轮不在了 —— 是并成一条了，不是离开：悄悄退役（第六步核实审计：否则两轮后报假离开） */
            for (size_t k = 0; k < sizeof(prev->p) / sizeof(prev->p[0]); k++) {
                ti_prev_entry_t *q = &prev->p[k];
                if (q != p && q->used && !seen[k] && strcmp(q->host, hname) == 0 && !in_list(cur, q->id)) q->used = false;
            }
        }
        if (!p) {
            p = prev_free(prev);
            if (!p) continue;                                   /* 表满：不跟踪（不可能：表与清单一样大） */
            memset(p, 0, sizeof(*p));
            snprintf(p->id, sizeof(p->id), "%s", e->id);
            p->used = true;
            if (report_join && fn) fn(ctx, e->id, true);
        }
        p->miss = 0;
        p->last_seen_ms = now_ms;
        if (hname[0]) snprintf(p->host, sizeof(p->host), "%s", hname);
        seen[p - prev->p] = true;
    }
    /* 没看到的：连续 TI_LEFT_AFTER 轮、且距上次看到满 TI_LEFT_MIN_MS 才算离开 */
    for (size_t i = 0; i < sizeof(prev->p) / sizeof(prev->p[0]); i++) {
        ti_prev_entry_t *p = &prev->p[i];
        if (!p->used || seen[i]) continue;
        if (p->last_seen_ms < prev->baseline_ms) {
            /* 重建基线之前就有、之后一次都没看到（掉网 / SRP 停过，设备还没重新注册）：不报离开（它的"上次看到"是掉网前的，110 s
               的保护形同虚设，会成批推假离开）；静默期过了还没回来就悄悄退役（第六步核实审计 ❌） */
            if (now_ms >= prev->baseline_ms && now_ms - prev->baseline_ms >= TI_QUIET_MS) p->used = false;
            continue;
        }
        if (p->miss < UINT8_MAX) p->miss++;
        if (p->miss >= TI_LEFT_AFTER && now_ms >= p->last_seen_ms && now_ms - p->last_seen_ms >= TI_LEFT_MIN_MS) {
            if (report && fn) fn(ctx, p->id, false);
            p->used = false;
        }
    }
    prev->have_baseline = true;
}

/* ---------------------------------------------------------------- 输出 */

/* link_margin（附录 A）照 OT 自己的算法（core/thread/link_quality.cpp ComputeLinkMargin）：平均 RSSI − 噪声底，负数或 RSSI 无效记 0；
   噪声底 = otPlatRadioGetReceiveSensitivity()（core/mac/sub_mac.cpp:694 GetNoiseFloor 就是它；ESP 端口固定 −120 dBm），
   所以和 OT CLI / 网络诊断报的 link margin 是一个数。这里是这个算法的正本。 */
static unsigned link_margin(int8_t noise_floor, int8_t rss)
{
    const int m = (int)rss - (int)noise_floor;
    return (rss == RSSI_INVALID || m < 0) ? 0u : (unsigned)m;
}

const char *ti_role_str(const ti_raw_t *raw, const ti_entry_t *e)
{
    if (e->link == TI_LINK_ROUTER) return "router";
    if (e->child >= 0) {
        const ti_child_t *c = &raw->children[e->child];
        if (!c->rx_on_idle) return "sleepy_end_device";
        return c->ftd ? "full_end_device" : "minimal_end_device";
    }
    return "unknown";
}

static void kstr_rloc(hj_t *w, const char *k, uint16_t r)
{
    char s[8];
    snprintf(s, sizeof(s), "0x%04x", r);
    hj_kstr(w, k, s);
}

/* "thread" 子对象（7.5 的 parent / rssi_avg / link_margin，full 时加 rloc16 与子节点的连接时长）。ti_write_entry 与 mt- 设备
   （本网关 fabric 上的 Matter 设备按 mt- 呈现，Thread 信息挂在它下面，7.3；M7 第四步）共用 */
void ti_write_thread_obj(hj_t *w, const ti_raw_t *raw, const ti_entry_t *e, bool full)
{
    const ti_child_t *c = e->child >= 0 ? &raw->children[e->child] : NULL;
    const ti_router_t *r = e->router >= 0 ? &raw->routers[e->router] : NULL;
    hj_key(w, "thread");
    hj_obj_begin(w);
    if (e->parent != TI_RLOC_NONE) kstr_rloc(w, "parent", e->parent);
    int8_t rssi = RSSI_INVALID;
    if (c) rssi = c->rssi_avg;
    else if (r && r->is_neighbor) rssi = r->rssi_avg;
    if (rssi != RSSI_INVALID) hj_kint(w, "rssi_avg", rssi);
    if (c || (r && r->is_neighbor)) {
        hj_kuint(w, "link_margin", link_margin(raw->noise_floor, rssi));   /* 还没有样本时为 0（附录 A） */
    } else {
        hj_kstr(w, "link_quality", "unknown");                  /* 挂在别的路由器下 / 关联不上 / 不相邻的路由器（7.2 ②④） */
    }
    if (full) {
        if (e->rloc16 != TI_RLOC_NONE) kstr_rloc(w, "rloc16", e->rloc16);
        if (c) {
            if (c->rssi_last != RSSI_INVALID) hj_kint(w, "rssi_last", c->rssi_last);
            hj_kuint(w, "connected_s", c->connected_s);
            hj_kuint(w, "timeout_s", c->timeout_s);
        }
    }
    hj_obj_end(w);
}

bool ti_entry_on_fabric(const ti_raw_t *raw, const ti_entry_t *e, const char *cfid, char node_out[17])
{
    if (!raw || !e || e->host < 0 || !cfid || !*cfid) return false;
    const ti_host_t *h = &raw->hosts[e->host];
    for (uint8_t i = 0; i < h->n_fabrics; i++) {
        if (strcasecmp(h->fabrics[i].cfid, cfid) != 0) continue;
        if (node_out) memcpy(node_out, h->fabrics[i].node, 17);
        return true;
    }
    return false;
}

const ti_entry_t *ti_find_fabric_node(const ti_raw_t *raw, const ti_list_t *l, const char *cfid, uint64_t node)
{
    if (!raw || !l) return NULL;
    char want[17];
    snprintf(want, sizeof(want), "%016llX", (unsigned long long)node);
    for (uint16_t i = 0; i < l->n; i++) {
        char n[17];
        if (ti_entry_on_fabric(raw, &l->e[i], cfid, n) && strcasecmp(n, want) == 0) return &l->e[i];
    }
    return NULL;
}

void ti_write_entry(hj_t *w, const ti_raw_t *raw, const ti_entry_t *e, bool full)
{
    const ti_host_t *h = e->host >= 0 ? &raw->hosts[e->host] : NULL;
    const ti_child_t *c = e->child >= 0 ? &raw->children[e->child] : NULL;
    const ti_router_t *r = e->router >= 0 ? &raw->routers[e->router] : NULL;
    hj_obj_begin(w);
    hj_kstr(w, "id", e->id);
    hj_kstr(w, "protocol", "thread");
    /* name 是用户起的名字（附录 A）：Thread 成员不归我们管，没有，恒为空串（7.5）。等待被添加的设备自己在 _matterc 里报的 DN
       放专门的字段 advertised_name（6.8：设备给的字符串只能放进专门的字段），净化后 */
    hj_kstr(w, "name", "");
    hj_kbool(w, "controllable", false);
    hj_kbool(w, "online", true);                                /* 在快照里 = 在网（离开的两轮后才从快照消失） */
    if (c) hj_kuint(w, "last_seen_s", c->age_s);
    else if (r && r->is_neighbor) hj_kuint(w, "last_seen_s", r->age_s);   /* 不相邻的路由器 OT 不知道上次听到它是什么时候（mAge 是截断的乱数） */
    hj_kstr(w, "role", ti_role_str(raw, e));
    ti_write_thread_obj(w, raw, e, full);
    if (h && h->n_fabrics) {
        hj_key(w, "matter");
        hj_obj_begin(w);
        hj_key(w, "fabrics");
        hj_arr_begin(w);
        for (uint8_t i = 0; i < h->n_fabrics; i++) {
            hj_obj_begin(w);
            hj_kstr(w, "cfid", h->fabrics[i].cfid);
            hj_kstr(w, "node_id", h->fabrics[i].node);
            hj_obj_end(w);
        }
        hj_arr_end(w);
        hj_obj_end(w);
    }
    if (h) hj_kbool(w, "ready_to_add", h->commissionable);
    if (h && h->commissionable && h->dn[0]) {
        char adv[TI_DN_MAX];
        hub_sanitize_device_str(h->dn, strlen(h->dn), adv, sizeof(adv), sizeof(adv) - 1);
        hj_kstr(w, "advertised_name", adv);
    }
    if (full && h) {
        char host[TI_HOSTNAME_MAX];
        hub_sanitize_device_str(h->name, strlen(h->name), host, sizeof(host), 32);   /* 6.8：设备给的字符串截到 32 字节 */
        hj_kstr(w, "hostname", host);
        hj_kuint(w, "lease_remaining_s", h->lease_remaining_s);
        /* VID / PID 不输出：等于说出了设备类型，超出 7.5 的"能说"（解析留着给 M5 matter_ctrl 配网用） */
    }
    hj_obj_end(w);
}

const ti_entry_t *ti_find(const ti_list_t *l, const char *id)
{
    if (!l || !id) return NULL;
    for (uint16_t i = 0; i < l->n; i++) {
        if (strcasecmp(l->e[i].id, id) == 0) return &l->e[i];
    }
    return NULL;
}
