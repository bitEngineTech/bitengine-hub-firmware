/* hub_meta_core —— 设备元数据的表与分页序列化（规则见 include/hub_meta_core.h）。 */
#include "hub_meta_core.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void hub_meta_core_init(hub_meta_core_t *m)
{
    if (m) memset(m, 0, sizeof(*m));
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hub_meta_parse_id(const char *id, uint8_t *proto, uint8_t id8[8])
{
    if (!id || !proto || !id8 || strlen(id) != 19 || id[2] != '-') return false;
    if ((id[0] == 'z' || id[0] == 'Z') && (id[1] == 'b' || id[1] == 'B')) *proto = HUB_META_PROTO_ZIGBEE;
    else if ((id[0] == 'm' || id[0] == 'M') && (id[1] == 't' || id[1] == 'T')) *proto = HUB_META_PROTO_MATTER;
    else return false;
    for (unsigned k = 0; k < 8; k++) {
        const int hi = hexval(id[3 + 2 * k]), lo = hexval(id[4 + 2 * k]);
        if (hi < 0 || lo < 0) return false;
        id8[k] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

int hub_meta_find(const hub_meta_core_t *m, uint8_t proto, const uint8_t id8[8])
{
    if (!m || !id8 || proto == HUB_META_PROTO_NONE) return -1;
    for (unsigned i = 0; i < HUB_META_MAX; i++)
        if (m->e[i].proto == proto && !memcmp(m->e[i].id8, id8, 8)) return (int)i;
    return -1;
}

static bool is_default(const hub_meta_ent_t *e) { return !e->name[0] && !e->room[0] && !e->flags; }

hub_meta_res_t hub_meta_apply(hub_meta_core_t *m, uint8_t proto, const uint8_t id8[8], const hub_meta_change_t *c, uint32_t page_ok, int *slot_out)
{
    if (slot_out) *slot_out = -1;
    if (!m || !id8 || !c || (proto != HUB_META_PROTO_ZIGBEE && proto != HUB_META_PROTO_MATTER)) return HUB_META_BAD_ID;
    if (!c->name && !c->room && c->notify < 0 && c->sensitive == HUB_META_SENS_KEEP) return HUB_META_NOTHING;
    if (c->sensitive > HUB_META_SENS_ON) return HUB_META_NOTHING;
    char name[HUB_NAME_MAX + 1] = "", room[HUB_ROOM_MAX + 1] = "";
    if (c->name) {
        const hub_name_verdict_t v = hub_sanitize_user_name(c->name, name, sizeof(name), HUB_NAME_MAX);
        if (v == HUB_NAME_NEWLINE) return HUB_META_NAME_NEWLINE;
        if (v != HUB_NAME_OK) return HUB_META_NAME_LONG;
    }
    if (c->room) {
        const hub_name_verdict_t v = hub_sanitize_user_name(c->room, room, sizeof(room), HUB_ROOM_MAX);
        if (v == HUB_NAME_NEWLINE) return HUB_META_ROOM_NEWLINE;
        if (v != HUB_NAME_OK) return HUB_META_ROOM_LONG;
    }
    int slot = hub_meta_find(m, proto, id8);
    if (slot >= 0 && !(page_ok & (1u << ((unsigned)slot / HUB_META_PER_PAGE)))) return HUB_META_STORE;
    if (slot < 0) {
        /* 有页读不出来：看不见它，就证明不了这台设备不在那一页上 —— 连"设成默认值"也不能答 OK（那一页里的旧值还在） */
        if ((page_ok & HUB_META_ALL_PAGES) != HUB_META_ALL_PAGES) return HUB_META_STORE;
        /* 新条目：只会变成"有内容"的才占位（只是把不存在的设成默认值，不用分配） */
        const bool becomes_empty = (!c->name || !name[0]) && (!c->room || !room[0]) && c->notify <= 0 && c->sensitive != HUB_META_SENS_ON;
        if (becomes_empty) return HUB_META_OK;
        for (unsigned i = 0; i < HUB_META_MAX && slot < 0; i++)
            if (m->e[i].proto == HUB_META_PROTO_NONE) slot = (int)i;
        if (slot < 0) return HUB_META_FULL;
        memset(&m->e[slot], 0, sizeof(m->e[slot]));
        m->e[slot].proto = proto;
        memcpy(m->e[slot].id8, id8, 8);
    }
    hub_meta_ent_t *e = &m->e[slot];
    if (c->name) memcpy(e->name, name, sizeof(e->name));
    if (c->room) memcpy(e->room, room, sizeof(e->room));
    if (c->notify >= 0) e->flags = (uint8_t)(c->notify ? (e->flags | HUB_META_F_NOTIFY) : (e->flags & ~HUB_META_F_NOTIFY));
    if (c->sensitive == HUB_META_SENS_ON) e->flags = (uint8_t)(e->flags | HUB_META_F_SENSITIVE);
    else if (c->sensitive == HUB_META_SENS_OFF) e->flags = (uint8_t)(e->flags & ~HUB_META_F_SENSITIVE);
    if (is_default(e)) memset(e, 0, sizeof(*e));   /* 回到默认：腾出位置（那一页照样要重写） */
    if (slot_out) *slot_out = slot;
    return HUB_META_OK;
}

unsigned hub_meta_clear_proto(hub_meta_core_t *m, uint8_t proto, unsigned page)
{
    if (!m || proto == HUB_META_PROTO_NONE || page >= HUB_META_PAGES) return 0;
    unsigned n = 0;
    for (unsigned i = page * HUB_META_PER_PAGE; i < (page + 1) * HUB_META_PER_PAGE; i++) {
        if (m->e[i].proto != proto) continue;
        memset(&m->e[i], 0, sizeof(m->e[i]));
        n++;
    }
    return n;
}

size_t hub_meta_pack(const hub_meta_core_t *m, unsigned page, uint8_t *buf, size_t cap)
{
    if (!m || !buf || page >= HUB_META_PAGES || cap < 1) return 0;
    size_t n = 1;
    uint8_t cnt = 0;
    for (unsigned k = 0; k < HUB_META_PER_PAGE; k++) {
        const hub_meta_ent_t *e = &m->e[page * HUB_META_PER_PAGE + k];
        if (e->proto == HUB_META_PROTO_NONE) continue;
        if (n + HUB_META_ENT_BYTES > cap) return 0;
        buf[n++] = (uint8_t)k;
        buf[n++] = e->proto;
        memcpy(buf + n, e->id8, 8);
        n += 8;
        buf[n++] = e->flags;
        memcpy(buf + n, e->name, sizeof(e->name));
        n += sizeof(e->name);
        memcpy(buf + n, e->room, sizeof(e->room));
        n += sizeof(e->room);
        cnt++;
    }
    buf[0] = cnt;
    return n;
}

int hub_meta_unpack(hub_meta_core_t *m, unsigned page, const uint8_t *buf, size_t len)
{
    if (!m || !buf || page >= HUB_META_PAGES || len < 1) return -1;
    const unsigned cnt = buf[0];
    if (cnt > HUB_META_PER_PAGE || len != 1u + cnt * HUB_META_ENT_BYTES) return -1;
    /* 两遍：先核对结构（不对就整页不动），再写 —— 不在栈上开一整页的临时表（约 3 KB，调用方的任务栈未必宽裕） */
    uint64_t seen = 0;
    const uint8_t *p = buf + 1;
    for (unsigned k = 0; k < cnt; k++, p += HUB_META_ENT_BYTES) {
        const unsigned slot = p[0];
        const uint8_t proto = p[1];
        if (slot >= HUB_META_PER_PAGE || (seen & (1ull << slot)) || (proto != HUB_META_PROTO_ZIGBEE && proto != HUB_META_PROTO_MATTER)) return -1;
        seen |= 1ull << slot;
        const uint8_t *nm = p + 11, *rm = p + 11 + (HUB_NAME_MAX + 1u);
        if (!memchr(nm, 0, HUB_NAME_MAX + 1u) || !memchr(rm, 0, HUB_ROOM_MAX + 1u)) return -1;
    }
    memset(&m->e[page * HUB_META_PER_PAGE], 0, HUB_META_PER_PAGE * sizeof(hub_meta_ent_t));
    int dropped = 0;
    p = buf + 1;
    for (unsigned k = 0; k < cnt; k++, p += HUB_META_ENT_BYTES) {
        hub_meta_ent_t *e = &m->e[page * HUB_META_PER_PAGE + p[0]];
        /* 存储是自己写的，但照样再过一遍净化：换了规则 / 存储被改坏都不让脏字符串流到 Muse 那边；过不了的那条丢掉 */
        if (hub_sanitize_user_name((const char *)(p + 11), e->name, sizeof(e->name), HUB_NAME_MAX) != HUB_NAME_OK ||
            hub_sanitize_user_name((const char *)(p + 11 + (HUB_NAME_MAX + 1u)), e->room, sizeof(e->room), HUB_ROOM_MAX) != HUB_NAME_OK) {
            memset(e, 0, sizeof(*e));
            dropped++;
            continue;
        }
        e->proto = p[1];
        memcpy(e->id8, p + 2, 8);
        e->flags = (uint8_t)(p[10] & (HUB_META_F_NOTIFY | HUB_META_F_SENSITIVE));
        if (is_default(e)) memset(e, 0, sizeof(*e));   /* 存的是默认值（不该有）：不占位 */
    }
    return dropped;
}

hub_meta_ld_t hub_meta_load_page(hub_meta_core_t *m, unsigned page, hub_meta_rd_t rd, uint8_t ver, const uint8_t *buf, size_t len, unsigned *dropped)
{
    if (dropped) *dropped = 0;
    switch (rd) {
    case HUB_META_RD_NONE:
    case HUB_META_RD_DOWN:
        return HUB_META_LD_EMPTY;
    case HUB_META_RD_OK:
        if (ver != HUB_META_VER) return HUB_META_LD_READONLY;   /* 更旧的格式：迁移随第一次改格式；更新的 hub_store 已回 INVALID_VERSION */
        {
            const int d = hub_meta_unpack(m, page, buf, len);
            if (d < 0) return HUB_META_LD_CORRUPT;
            if (dropped) *dropped = (unsigned)d;
            return HUB_META_LD_LOADED;
        }
    case HUB_META_RD_TIMEOUT:
    case HUB_META_RD_ERR:
    default:
        return HUB_META_LD_READONLY;
    }
}

unsigned hub_meta_dedup(hub_meta_core_t *m, uint32_t *pages)
{
    if (pages) *pages = 0;
    if (!m) return 0;
    unsigned n = 0;
    for (unsigned i = 0; i < HUB_META_MAX; i++) {
        if (m->e[i].proto == HUB_META_PROTO_NONE) continue;
        for (unsigned j = i + 1; j < HUB_META_MAX; j++) {
            if (m->e[j].proto == m->e[i].proto && !memcmp(m->e[j].id8, m->e[i].id8, 8)) {
                memset(&m->e[j], 0, sizeof(m->e[j]));
                if (pages) *pages |= 1u << (j / HUB_META_PER_PAGE);
                n++;
            }
        }
    }
    return n;
}

/* 拷一个用户字符串进事件句子：" → '、[ → (、] → )；同形的 Unicode 引号 / 方括号也换（名字可能是 Muse 的 agent 写的，
   全角 ［zb-…］ 或弯引号同样能伪造设备 id、冒充关引号 —— C17 第二轮审计）。替换后只会变短，dst 与 src 同容量即可 */
static void ev_copy(char *dst, size_t cap, const char *src)
{
    static const struct { uint8_t b[3]; char to; } k_alias[] = {
        { { 0xE2, 0x80, 0x9C }, '\'' }, { { 0xE2, 0x80, 0x9D }, '\'' },   /* “ ” */
        { { 0xE2, 0x80, 0x9E }, '\'' }, { { 0xE2, 0x80, 0x9F }, '\'' },   /* „ ‟ */
        { { 0xEF, 0xBC, 0x82 }, '\'' },                                    /* ＂ */
        { { 0xEF, 0xBC, 0xBB }, '(' },  { { 0xEF, 0xBC, 0xBD }, ')' },     /* ［ ］ */
        { { 0xE3, 0x80, 0x90 }, '(' },  { { 0xE3, 0x80, 0x91 }, ')' },     /* 【 】 */
        { { 0xE3, 0x80, 0x9D }, '\'' }, { { 0xE3, 0x80, 0x9E }, '\'' }, { { 0xE3, 0x80, 0x9F }, '\'' },   /* 〝 〞 〟 */
        { { 0xE2, 0x9F, 0xA6 }, '(' },  { { 0xE2, 0x9F, 0xA7 }, ')' },     /* ⟦ ⟧ */
        { { 0xE3, 0x80, 0x94 }, '(' },  { { 0xE3, 0x80, 0x95 }, ')' },     /* 〔 〕 */
        { { 0xEF, 0xB9, 0x9D }, '(' },  { { 0xEF, 0xB9, 0x9E }, ')' },     /* ﹝ ﹞ */
    };
    size_t i = 0, k = 0;
    while (src[i] && k + 1 < cap) {
        const char c = src[i];
        bool hit = false;
        for (unsigned a = 0; a < sizeof(k_alias) / sizeof(k_alias[0]) && !hit; a++) {
            if ((uint8_t)src[i] == k_alias[a].b[0] && (uint8_t)src[i + 1] == k_alias[a].b[1] && src[i + 1] && (uint8_t)src[i + 2] == k_alias[a].b[2]) {
                dst[k++] = k_alias[a].to;
                i += 3;
                hit = true;
            }
        }
        if (hit) continue;
        dst[k++] = c == '"' ? '\'' : c == '[' ? '(' : c == ']' ? ')' : c;
        i++;
    }
    dst[k] = '\0';
}

size_t hub_meta_event_name(const hub_meta_ent_t *m, uint8_t proto, const char *kind, uint16_t tail, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    int n;
    if (m && m->name[0]) {
        char nm[sizeof(m->name)], rm[sizeof(m->room)];
        ev_copy(nm, sizeof(nm), m->name);
        ev_copy(rm, sizeof(rm), m->room);
        n = rm[0] ? snprintf(out, cap, "\"%s\" (%s)", nm, rm) : snprintf(out, cap, "\"%s\"", nm);
    } else {
        char k[24];
        if (!kind || !strcmp(kind, "unknown")) snprintf(k, sizeof(k), "%s", proto == HUB_META_PROTO_MATTER ? "Matter device" : "Zigbee device");
        else {
            snprintf(k, sizeof(k), "%s", kind);
            for (char *c = k; *c; c++)
                if (*c == '_') *c = ' ';
        }
        n = snprintf(out, cap, "unnamed %s %04x", k, (unsigned)tail);
    }
    if (n < 0) return 0;
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

const char *hub_meta_res_str(hub_meta_res_t r)
{
    switch (r) {
    case HUB_META_OK:           return "ok";
    case HUB_META_BAD_ID:       return "bad id";
    case HUB_META_NOTHING:      return "nothing to change";
    case HUB_META_NAME_NEWLINE: return "name has a line break";
    case HUB_META_NAME_LONG:    return "name too long";
    case HUB_META_ROOM_NEWLINE: return "room has a line break";
    case HUB_META_ROOM_LONG:    return "room too long";
    case HUB_META_FULL:         return "table full";
    case HUB_META_STORE:        return "storage unreadable";
    case HUB_META_BUSY:         return "busy";
    default:                    return "?";
    }
}
