/* hub_ti_core 的真值表（docs/15 设计第七节；TC-TI01–04 的主机侧）。编的是产品源文件 ../hub_ti_core.c、../hub_sanitize.c、../hub_json.c，
 * 结果用 cJSON 解析回来比。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "hub_ti_core.h"

static int s_fail, s_pass;
#define CHECK(cond, ...) do { if (cond) { s_pass++; } else { s_fail++; printf("❌ %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static ti_raw_t s_raw;
static ti_list_t s_list;
static ti_prev_t s_prev;

static ti_ip6_t ip(uint8_t tag, uint8_t n)
{
    ti_ip6_t a;
    memset(&a, 0, sizeof(a));
    a.b[0] = 0xfd;
    a.b[1] = tag;
    a.b[15] = n;
    return a;
}

static ti_host_t *host(const char *name)
{
    ti_host_t *h = &s_raw.hosts[s_raw.n_hosts++];
    memset(h, 0, sizeof(*h));
    snprintf(h->name, sizeof(h->name), "%s", name);
    h->lease_remaining_s = 7000;
    return h;
}

static ti_child_t *child(uint8_t ext_last, uint16_t rloc16)
{
    ti_child_t *c = &s_raw.children[s_raw.n_children++];
    memset(c, 0, sizeof(*c));
    for (int i = 0; i < 8; i++) c->ext[i] = (uint8_t)(0x10 + i);
    c->ext[7] = ext_last;
    c->rloc16 = rloc16;
    c->rssi_avg = -70;
    c->rssi_last = -68;
    c->age_s = 12;
    c->timeout_s = 240;
    c->connected_s = 3600;
    return c;
}

static ti_router_t *router(uint8_t ext_last, uint16_t rloc16, bool neighbor)
{
    ti_router_t *r = &s_raw.routers[s_raw.n_routers++];
    memset(r, 0, sizeof(*r));
    for (int i = 0; i < 8; i++) r->ext[i] = (uint8_t)(0xa0 + i);
    r->ext[7] = ext_last;
    r->rloc16 = rloc16;
    r->is_neighbor = neighbor;
    r->rssi_avg = neighbor ? -60 : 127;
    r->age_s = 5;
    return r;
}

static void reset(void)
{
    memset(&s_raw, 0, sizeof(s_raw));
    s_raw.my_rloc16 = 0x6c00;
    s_raw.noise_floor = -120;
}

static cJSON *entry_json(const ti_entry_t *e, bool full)
{
    static char buf[4096];
    hj_t w;
    hj_init(&w, buf, sizeof(buf));
    ti_write_entry(&w, &s_raw, e, full);
    return hj_finish(&w) > 0 ? cJSON_Parse(buf) : NULL;
}

static const char *jstr(const cJSON *o, const char *path)
{
    char p[64];
    snprintf(p, sizeof(p), "%s", path);
    const cJSON *cur = o;
    for (char *tok = strtok(p, "."); tok && cur; tok = strtok(NULL, ".")) cur = cJSON_GetObjectItemCaseSensitive(cur, tok);
    return cJSON_IsString(cur) ? cur->valuestring : NULL;
}

static int jnum(const cJSON *o, const char *path, int dflt)
{
    char p[64];
    snprintf(p, sizeof(p), "%s", path);
    const cJSON *cur = o;
    for (char *tok = strtok(p, "."); tok && cur; tok = strtok(NULL, ".")) cur = cJSON_GetObjectItemCaseSensitive(cur, tok);
    return cJSON_IsNumber(cur) ? cur->valueint : dflt;
}

/* ---------------------------------------------------------------- 解析（TC-TI03） */

static void test_parse(void)
{
    char cfid[17], node[17];
    CHECK(ti_parse_matter_instance("a1b2c3d4e5f60718-0000000000000012", cfid, node)
          && strcmp(cfid, "A1B2C3D4E5F60718") == 0 && strcmp(node, "0000000000000012") == 0, "Matter 实例名拆开、统一大写");
    CHECK(!ti_parse_matter_instance("A1B2C3D4E5F60718_0000000000000012", cfid, node), "分隔符不是 - 拒");
    CHECK(!ti_parse_matter_instance("A1B2C3D4E5F6071-0000000000000012", cfid, node), "15 位拒");
    CHECK(!ti_parse_matter_instance("G1B2C3D4E5F60718-0000000000000012", cfid, node), "非十六进制拒");
    CHECK(!ti_parse_matter_instance("A1B2C3D4E5F60718-00000000000000123", cfid, node), "超长拒");
    CHECK(!ti_parse_matter_instance(NULL, cfid, node), "NULL");

    CHECK(ti_service_is("_matter._tcp.default.service.arpa.", "_matter", "_tcp"), "服务名前两段");
    CHECK(!ti_service_is("_matterc._udp.default.service.arpa.", "_matter", "_tcp"), "另一种服务");
    CHECK(ti_service_is("_matterc._udp.default.service.arpa.", "_matterc", "_udp"), "可配网服务");
    CHECK(!ti_service_is("_matter", "_matter", "_tcp"), "残缺");
    char lbl[TI_HOSTNAME_MAX];
    CHECK(ti_host_label("1A2B3C4D5E6F7788.default.service.arpa.", lbl, sizeof(lbl)) && strcmp(lbl, "1A2B3C4D5E6F7788") == 0, "主机名第一段");
    CHECK(!ti_host_label(".default.service.arpa.", lbl, sizeof(lbl)), "空的第一段");

    /* _matterc TXT：VP / DN，越界条目丢掉 */
    ti_host_t h;
    memset(&h, 0, sizeof(h));
    uint8_t t2[64];
    size_t n = 0;
    const char *items[] = { "D=840", "VP=65521+32769", "DN=Kitchen Plug", "CM=1" };
    for (int i = 0; i < 4; i++) {
        t2[n++] = (uint8_t)strlen(items[i]);
        memcpy(t2 + n, items[i], strlen(items[i]));
        n += strlen(items[i]);
    }
    ti_parse_matterc_txt(t2, (uint16_t)n, &h);
    CHECK(h.has_vp && h.vid == 65521 && h.pid == 32769, "VP 拆成 VID / PID");
    CHECK(strcmp(h.dn, "Kitchen Plug") == 0, "DN 原样");
    memset(&h, 0, sizeof(h));
    t2[0] = 60;                                                 /* 第一条长度越界 */
    ti_parse_matterc_txt(t2, (uint16_t)n, &h);
    CHECK(!h.has_vp && h.dn[0] == 0, "越界的条目和后面的都不要");
    memset(&h, 0, sizeof(h));
    const char *bad[] = { "VP=99999+1", "DN=0123456789012345678901234567890123456789" };
    n = 0;
    for (int i = 0; i < 2; i++) {
        t2[n++] = (uint8_t)strlen(bad[i]);
        memcpy(t2 + n, bad[i], strlen(bad[i]));
        n += strlen(bad[i]);
    }
    ti_parse_matterc_txt(t2, (uint16_t)n, &h);
    CHECK(!h.has_vp, "VID 超过 65535 拒");
    CHECK(strlen(h.dn) == TI_DN_MAX - 1, "DN 截到 32 字节");
    ti_parse_matterc_txt(NULL, 3, &h);
    CHECK(h.dn[0] == 0 && !h.has_vp, "NULL TXT 清空");
}

/* ---------------------------------------------------------------- id */

static void test_ids(void)
{
    const uint8_t ext[8] = { 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78, 0x87 };
    char id[TI_ID_MAX];
    ti_id_from_ext(ext, id);
    CHECK(strcmp(id, "th-1e2d3c4b5a697887") == 0, "扩展地址 → th-<16hex>（附录 A 的样例）");
    ti_id_from_host("1A2B3C4D5E6F7788", id);
    CHECK(strcmp(id, "th-h1a2b3c4d5e6f778") == 0, "十六进制主机名取前 15 位、小写");
    char id2[TI_ID_MAX];
    ti_id_from_host("HomePod-Living-Room", id);
    ti_id_from_host("HomePod-Living-Room", id2);
    CHECK(strncmp(id, "th-h", 4) == 0 && strlen(id) == 19 && strcmp(id, id2) == 0, "非十六进制主机名：哈希、稳定");
    CHECK(strstr(id, "HomePod") == NULL, "不带设备给的原文");
    ti_id_from_host("HomePod-Bedroom", id2);
    CHECK(strcmp(id, id2) != 0, "不同主机名不同 id");
}

/* ---------------------------------------------------------------- 关联（TC-TI01 / TC-TI02） */

static void test_associate(void)
{
    reset();
    /* H0：直连子节点（地址有交集） */
    ti_host_t *h0 = host("1A2B3C4D5E6F7788");
    h0->addrs[0] = ip(1, 1);
    h0->addrs[1] = ip(2, 1);
    h0->n_addrs = 2;
    ti_parse_matter_instance("A1B2C3D4E5F60718-0000000000000012", h0->fabrics[0].cfid, h0->fabrics[0].node);
    ti_parse_matter_instance("1111111111111111-00000000000000AB", h0->fabrics[1].cfid, h0->fabrics[1].node);
    h0->n_fabrics = 2;
    ti_child_t *c0 = child(0x01, 0x6c01);
    c0->addrs[0] = ip(9, 9);
    c0->addrs[1] = ip(2, 1);
    c0->n_addrs = 2;
    /* H1：挂在别的路由器下（EID 缓存 → 0x4801，子节点号 1） */
    ti_host_t *h1 = host("2222333344445555");
    h1->addrs[0] = ip(3, 1);
    h1->n_addrs = 1;
    s_raw.cache[s_raw.n_cache++] = (ti_cache_t){ ip(3, 1), 0x4801 };
    /* H2：它自己是路由器（缓存 → 0x4800）。主机名 = 它的扩展地址（Matter 的主机名就是扩展地址，2026-10-22 上板核实）；
       主机名是十六进制却对不上这台路由器的，是挂在它下面的设备（test_snooped_parent_rloc） */
    ti_host_t *h2 = host("A0A1A2A3A4A5A648");
    h2->addrs[0] = ip(4, 1);
    h2->n_addrs = 1;
    s_raw.cache[s_raw.n_cache++] = (ti_cache_t){ ip(4, 1), 0x4800 };
    router(0x48, 0x4800, true);
    /* H3：关联不上 */
    ti_host_t *h3 = host("lonely-host");
    h3->addrs[0] = ip(5, 1);
    h3->n_addrs = 1;
    h3->commissionable = true;
    snprintf(h3->dn, sizeof(h3->dn), "Desk\x1b[31mLamp");
    /* 没注册 SRP 的子节点、路由器、本 BR 自己 */
    child(0x02, 0x6c02)->rx_on_idle = true;
    router(0x99, 0x9c00, false);
    router(0x00, 0x6c00, true);                                 /* 本 BR：不列 */

    ti_associate(&s_raw, NULL, &s_list);
    CHECK(s_list.n == 6, "4 个主机 + 1 个没服务的子节点 + 1 个没服务的路由器（%u）", s_list.n);

    const ti_entry_t *e = ti_find(&s_list, "th-1011121314151601");
    CHECK(e && e->host == 0 && e->child == 0 && e->link == TI_LINK_CHILD && e->parent == 0x6c00, "TC-TI01：子节点按地址交集关联、id 用扩展地址");
    cJSON *j = e ? entry_json(e, false) : NULL;
    CHECK(j && strcmp(jstr(j, "thread.parent"), "0x6c00") == 0 && jnum(j, "thread.rssi_avg", 0) == -70 && jnum(j, "thread.link_margin", 0) == 50,
          "子节点：parent / rssi_avg / link_margin（OT 算法）");
    const cJSON *fab = j ? cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "matter"), "fabrics") : NULL;
    CHECK(cJSON_GetArraySize(fab) == 2 && strcmp(cJSON_GetArrayItem(fab, 1)->child->valuestring, "1111111111111111") == 0,
          "TC-TI03：多管理员的两个 fabric 都列出");
    CHECK(j && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(j, "controllable")) && strcmp(jstr(j, "name"), "") == 0
          && strcmp(jstr(j, "role"), "sleepy_end_device") == 0, "不归我们管：controllable false、没有名字");
    cJSON_Delete(j);

    /* M7 第四步：本网关 fabric 上的设备按 mt- 呈现 —— 认出它、按节点号找回它、thread 子对象单独写得出来（7.3） */
    {
        char nd[17] = "";
        const ti_entry_t *h0e = ti_find(&s_list, "th-1011121314151601");
        CHECK(h0e && ti_entry_on_fabric(&s_raw, h0e, "1111111111111111", nd) && strcmp(nd, "00000000000000AB") == 0, "认得出本 fabric 的实例、拆出节点号");
        CHECK(h0e && ti_entry_on_fabric(&s_raw, h0e, "a1b2c3d4e5f60718", NULL), "cfid 大小写不敏感");
        CHECK(h0e && !ti_entry_on_fabric(&s_raw, h0e, "2222222222222222", nd), "别的 fabric：不是");
        CHECK(h0e && !ti_entry_on_fabric(&s_raw, h0e, "", nd) && !ti_entry_on_fabric(NULL, h0e, "1111111111111111", nd), "空 cfid / 空指针：不是");
        CHECK(ti_find_fabric_node(&s_raw, &s_list, "1111111111111111", 0xAB) == h0e && !ti_find_fabric_node(&s_raw, &s_list, "1111111111111111", 0xAC),
              "按节点号找回那一条");
        const ti_entry_t *nohost = NULL;
        for (uint16_t i = 0; i < s_list.n; i++) if (s_list.e[i].host < 0) nohost = &s_list.e[i];
        CHECK(nohost && !ti_entry_on_fabric(&s_raw, nohost, "1111111111111111", nd), "没注册 SRP 的条目：不是");
        char tb[256];
        hj_t tw;
        hj_init(&tw, tb, sizeof(tb));
        hj_obj_begin(&tw);
        if (h0e) ti_write_thread_obj(&tw, &s_raw, h0e, false);
        hj_obj_end(&tw);
        cJSON *tj = hj_finish(&tw) > 0 ? cJSON_Parse(tb) : NULL;
        CHECK(tj && strcmp(jstr(tj, "thread.parent"), "0x6c00") == 0 && jnum(tj, "thread.link_margin", 0) == 50, "thread 子对象单独写：parent / link_margin");
        cJSON_Delete(tj);
    }

    e = ti_find(&s_list, "th-2222333344445555");
    CHECK(e && e->link == TI_LINK_VIA_ROUTER && e->rloc16 == 0x4801 && e->parent == 0x4800, "TC-TI02：挂在别的路由器下，父路由器 = rloc16 & 0xFC00");
    j = e ? entry_json(e, false) : NULL;
    CHECK(j && strcmp(jstr(j, "thread.parent"), "0x4800") == 0 && strcmp(jstr(j, "thread.link_quality"), "unknown") == 0
          && jnum(j, "thread.rssi_avg", 999) == 999, "链路质量标未知、没有 RSSI");
    cJSON_Delete(j);

    e = ti_find(&s_list, "th-a0a1a2a3a4a5a648");
    CHECK(e && e->link == TI_LINK_ROUTER && e->host == 2 && e->router == 0, "子节点号 0：它自己是路由器，id 用路由器的扩展地址");
    j = e ? entry_json(e, false) : NULL;
    CHECK(j && strcmp(jstr(j, "role"), "router") == 0 && jnum(j, "thread.link_margin", 0) == 60, "邻居路由器有 RSSI / link_margin");
    cJSON_Delete(j);

    char lid[TI_ID_MAX];
    ti_id_from_host("lonely-host", lid);
    e = ti_find(&s_list, lid);
    CHECK(e && e->link == TI_LINK_UNKNOWN, "关联不上的照样列出");
    j = e ? entry_json(e, true) : NULL;
    CHECK(j && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "ready_to_add")), "等待被添加");
    CHECK(j && strcmp(jstr(j, "name"), "") == 0, "name 是用户起的名字：Thread 成员恒为空（7.5）");
    CHECK(j && jstr(j, "advertised_name") && strstr(jstr(j, "advertised_name"), "\x1b") == NULL && strstr(jstr(j, "advertised_name"), "Desk") != NULL,
          "可配网设备自报的 DN 放专门的字段 advertised_name，净化掉控制字符（6.8）：%s", j ? jstr(j, "advertised_name") : "");
    CHECK(j && cJSON_GetObjectItemCaseSensitive(j, "vendor_id") == NULL, "不输出 VID / PID（等于说出类型，超出 7.5）");
    CHECK(j && strcmp(jstr(j, "hostname"), "lonely-host") == 0 && jnum(j, "lease_remaining_s", 0) == 7000, "详情带主机名与剩余租约");
    cJSON_Delete(j);

    e = ti_find(&s_list, "th-1011121314151602");
    CHECK(e && e->host < 0 && e->link == TI_LINK_CHILD, "没注册 SRP 服务的子节点也列出");
    j = e ? entry_json(e, true) : NULL;
    CHECK(j && strcmp(jstr(j, "role"), "minimal_end_device") == 0 && cJSON_GetObjectItemCaseSensitive(j, "ready_to_add") == NULL
          && strcmp(jstr(j, "thread.rloc16"), "0x6c02") == 0 && jnum(j, "thread.timeout_s", 0) == 240, "子节点详情");
    cJSON_Delete(j);

    CHECK(ti_find(&s_list, "th-a0a1a2a3a4a5a699") != NULL, "没注册 SRP 服务的路由器也列出");
    CHECK(ti_find(&s_list, "th-a0a1a2a3a4a5a600") == NULL, "本 BR 自己不列");
    CHECK(ti_find(&s_list, "TH-1011121314151601") != NULL, "按 id 找不区分大小写");
    CHECK(ti_find(&s_list, "th-0000000000000000") == NULL && ti_find(NULL, "x") == NULL, "找不到 / NULL");
}

/* ---------------------------------------------------------------- 增减（TC-TI04）与 id 粘连 */

static int s_joined, s_left;
static char s_last[TI_ID_MAX];
static void on_change(void *ctx, const char *id, bool joined)
{
    (void)ctx;
    if (joined) s_joined++;
    else s_left++;
    snprintf(s_last, sizeof(s_last), "%s", id);
}

static uint64_t s_now;
static void round_(void)
{
    s_now += 60000;                                             /* 60 s 一轮（7.4） */
    ti_associate(&s_raw, &s_prev, &s_list);
    ti_diff(&s_prev, &s_raw, &s_list, s_now, on_change, NULL);
}

static void test_diff(void)
{
    memset(&s_prev, 0, sizeof(s_prev));
    s_joined = s_left = 0;
    reset();
    ti_host_t *h = host("1A2B3C4D5E6F7788");
    h->addrs[0] = ip(1, 1);
    h->n_addrs = 1;
    ti_child_t *c = child(0x01, 0x6c01);
    c->addrs[0] = ip(1, 1);
    c->n_addrs = 1;
    host("2222333344445555");
    round_();
    CHECK(s_joined == 0 && s_left == 0, "第一轮只建基线，不报（开机不刷一屏「新设备」）");
    round_();
    CHECK(s_joined == 0 && s_left == 0, "没变化不报");

    host("8888777766665555");                                   /* 建基线后 5 分钟内新出现的：只并入基线 */
    round_();
    CHECK(s_joined == 0, "静默期里新出现的不报（SRP 主机陆续重新注册，第二轮审计 ❌）");
    s_now += TI_QUIET_MS;

    host("9999888877776666");
    round_();
    CHECK(s_joined == 1 && strcmp(s_last, "th-9999888877776666") == 0, "新出现的报 joined（16 位十六进制主机名就是扩展地址：th-<小写>）");

    /* 子节点超时离开（7.4：子节点消失超过 child timeout）——SRP 主机还在，关联不上了：id 粘连，不报一对 left / joined */
    s_raw.n_children = 0;
    round_();
    CHECK(s_joined == 1 && s_left == 0, "子节点没了但主机还在：沿用 th-<ext>，不算一走一来");
    CHECK(ti_find(&s_list, "th-1011121314151601") != NULL, "id 粘连");

    /* TC-TI04：租约过期（主机被删）—— 连续两轮没看到才算离开 */
    s_raw.n_hosts = 3;                                          /* 去掉第 4 个（9999…） */
    round_();
    CHECK(s_left == 0, "缺一轮不算（SED 的一次缺席）");
    round_();
    CHECK(s_left == 1 && strcmp(s_last, "th-9999888877776666") == 0, "连续两轮没看到：报 left");
    round_();
    CHECK(s_left == 1, "报过就不再报");

    /* 缺一轮又回来（8888… 缺席一轮）：不报 */
    s_raw.n_hosts = 2;
    round_();
    s_raw.n_hosts = 3;
    round_();
    CHECK(s_left == 1 && s_joined == 1, "缺一轮又回来：既不报离开也不报新来");
}

/* ---------------------------------------------------------------- 撞车 */

static void test_collisions(void)
{
    reset();
    /* 两个主机名前 15 位相同：第二个丢掉（免得 get 找错设备） */
    host("1A2B3C4D5E6F778A-x");
    host("1A2B3C4D5E6F778A-y");
    ti_associate(&s_raw, NULL, &s_list);
    CHECK(s_list.n == 1, "主机名派生的 id 撞了：只留一个（%u）", s_list.n);

    /* 一台路由器注册了两个主机名（非 Matter：主机名不是扩展地址）：第一个用路由器 id，第二个退回主机名派生 */
    reset();
    ti_host_t *a = host("router-svc-a"), *b = host("router-svc-b");
    a->addrs[0] = ip(4, 1);
    a->n_addrs = 1;
    b->addrs[0] = ip(4, 2);
    b->n_addrs = 1;
    s_raw.cache[s_raw.n_cache++] = (ti_cache_t){ ip(4, 1), 0x4800 };
    s_raw.cache[s_raw.n_cache++] = (ti_cache_t){ ip(4, 2), 0x4800 };
    router(0x48, 0x4800, false);
    ti_associate(&s_raw, NULL, &s_list);
    char bid[TI_ID_MAX];
    ti_id_from_host("router-svc-b", bid);
    CHECK(s_list.n == 2 && ti_find(&s_list, "th-a0a1a2a3a4a5a648") && ti_find(&s_list, bid), "同一台路由器的第二个主机名不抢路由器 id（用它自己的主机名派生）");

    /* 关联到本 BR 自己的 RLOC：当作关联不上 */
    reset();
    ti_host_t *s = host("selfhost");
    s->addrs[0] = ip(7, 1);
    s->n_addrs = 1;
    s_raw.cache[s_raw.n_cache++] = (ti_cache_t){ ip(7, 1), 0x6c00 };
    ti_associate(&s_raw, NULL, &s_list);
    CHECK(s_list.n == 1 && s_list.e[0].link == TI_LINK_UNKNOWN, "缓存指向本 BR：不当成路由器");

    ti_associate(NULL, NULL, &s_list);
    CHECK(s_list.n == 0, "raw NULL：空清单");
}

/* ---------------------------------------------------------------- 第六步审计的四个 ❌ 的回归 */

static void test_audit_regressions(void)
{
    /* ❌1：主机名先关联不上（th-h…）、后来关联上（th-<ext>）——同一台设备换了 id，不能报假离开 */
    memset(&s_prev, 0, sizeof(s_prev));
    s_joined = s_left = 0;
    s_now = 0;
    reset();
    ti_host_t *h = host("matter-node-1");
    h->addrs[0] = ip(1, 1);
    h->n_addrs = 1;
    round_();                                                   /* 基线：关联不上 → th-h<哈希> */
    char hid[TI_ID_MAX];
    ti_id_from_host("matter-node-1", hid);
    CHECK(ti_find(&s_list, hid) != NULL, "先是主机名 id");
    ti_child_t *c = child(0x55, 0x6c05);
    c->addrs[0] = ip(1, 1);
    c->n_addrs = 1;
    round_();                                                   /* 关联上 → th-<ext> */
    round_();
    round_();
    CHECK(ti_find(&s_list, "th-1011121314151655") != NULL && ti_find(&s_list, hid) == NULL, "换成扩展地址 id");
    CHECK(s_left == 0 && s_joined == 0, "换 id 是原地改名：不报离开也不报新来（%d / %d）", s_left, s_joined);
    s_raw.n_children = 0;                                       /* 子节点超时、主机还在：粘连回 th-<ext> */
    round_();
    round_();
    round_();
    CHECK(ti_find(&s_list, "th-1011121314151655") != NULL && s_left == 0 && s_joined == 0, "子节点没了：沿用 th-<ext>，仍不报");

    /* ❌2：本 BR 的 FTD 子节点不向父节点登记地址：地址交集落空，EID 缓存指到本 BR 的子节点 RLOC → 按 RLOC 在子节点表里找 */
    reset();
    h = host("ftd-device");
    h->addrs[0] = ip(2, 1);
    h->n_addrs = 1;
    c = child(0x66, 0x6c03);
    c->ftd = true;
    c->rx_on_idle = true;
    s_raw.cache[s_raw.n_cache++] = (ti_cache_t){ ip(2, 1), 0x6c03 };
    ti_associate(&s_raw, NULL, &s_list);
    const ti_entry_t *e = ti_find(&s_list, "th-1011121314151666");
    CHECK(s_list.n == 1 && e && e->host == 0 && e->child == 0 && e->link == TI_LINK_CHILD && e->parent == 0x6c00,
          "FTD 子节点：按 RLOC 关联、只列一次（%u）", s_list.n);

    /* ❌3：不相邻路由器的扩展地址全 0：当作不知道 —— 不拿它当 id、没 SRP 的不列、主机退回自己的 id */
    reset();
    for (int i = 0; i < 3; i++) {
        ti_router_t *r = router(0, (uint16_t)(0x0400 * (i + 1)), false);
        memset(r->ext, 0, 8);
    }
    h = host("router-host-x");
    h->addrs[0] = ip(3, 1);
    h->n_addrs = 1;
    s_raw.cache[s_raw.n_cache++] = (ti_cache_t){ ip(3, 1), 0x0400 };
    ti_associate(&s_raw, NULL, &s_list);
    ti_id_from_host("router-host-x", hid);
    CHECK(s_list.n == 1 && ti_find(&s_list, "th-0000000000000000") == NULL && ti_find(&s_list, hid) != NULL
          && s_list.e[0].link == TI_LINK_ROUTER, "全 0 扩展地址不当 id；没 SRP 的不列（%u）", s_list.n);

    /* ❌4：主机沿用旧的 th-<ext>，同一个子节点还在表里、地址暂时对不上：第 ③ 步并进那条，不出现两个相同 id */
    memset(&s_prev, 0, sizeof(s_prev));
    s_now = 0;
    reset();
    h = host("sleepy-sensor");
    h->addrs[0] = ip(4, 1);
    h->n_addrs = 1;
    c = child(0x77, 0x6c07);
    c->addrs[0] = ip(4, 1);
    c->n_addrs = 1;
    round_();                                                   /* 关联上：th-…77 */
    c->addrs[0] = ip(4, 9);                                     /* 子节点重新登记了地址，暂时对不上 */
    round_();
    int same = 0;
    for (uint16_t i = 0; i < s_list.n; i++) same += strcmp(s_list.e[i].id, "th-1011121314151677") == 0;
    CHECK(same == 1 && s_list.n == 1, "同一个 id 只出现一次（%d 次、共 %u 条）", same, s_list.n);
    e = ti_find(&s_list, "th-1011121314151677");
    CHECK(e && e->host == 0 && e->child == 0, "并成一条：主机 + 子节点");

    /* ⚠️：离开按时间算 —— agent 调用的现刷（15 s 内连刷）不能把"两轮"缩成 15 s */
    memset(&s_prev, 0, sizeof(s_prev));
    s_joined = s_left = 0;
    s_now = 0;
    reset();
    host("aaaa1111bbbb2222");
    round_();
    s_raw.n_hosts = 0;
    for (int i = 0; i < 4; i++) {                               /* 4 次现刷，相隔 5 s */
        s_now += 5000;
        ti_associate(&s_raw, &s_prev, &s_list);
        ti_diff(&s_prev, &s_raw, &s_list, s_now, on_change, NULL);
    }
    CHECK(s_left == 0, "缺席 20 s、刷了 4 次：还不算离开");
    s_now += 100000;
    ti_associate(&s_raw, &s_prev, &s_list);
    ti_diff(&s_prev, &s_raw, &s_list, s_now, on_change, NULL);
    CHECK(s_left == 1, "缺席满 110 s：离开");

    /* ⚠️：子节点还没有 RSSI 样本时 link_margin 报 0（附录 A） */
    reset();
    c = child(0x88, 0x6c08);
    c->rssi_avg = 127;
    ti_associate(&s_raw, NULL, &s_list);
    cJSON *j = entry_json(&s_list.e[0], false);
    CHECK(j && jnum(j, "thread.link_margin", 99) == 0 && jnum(j, "thread.rssi_avg", 999) == 999, "没有样本：link_margin 0、不报 rssi_avg");
    cJSON_Delete(j);
    /* 不相邻的路由器：不报 last_seen_s、链路质量未知 */
    reset();
    router(0x42, 0x4400, false);
    ti_associate(&s_raw, NULL, &s_list);
    j = entry_json(&s_list.e[0], false);
    CHECK(j && jnum(j, "last_seen_s", -1) == -1 && strcmp(jstr(j, "thread.link_quality"), "unknown") == 0, "不相邻路由器：没有 last_seen_s、链路质量未知");
    cJSON_Delete(j);
}

/* 第六步第二轮审计 ❌1：FTD 子节点 / 路由器注册了 SRP、EID 缓存里没它、主机名就是扩展地址 —— 并成一条，fabric 不丢 */
static void test_hex_host_merge(void)
{
    reset();
    ti_host_t *h = host("1011121314151699");                    /* = 下面子节点的扩展地址 */
    h->addrs[0] = ip(6, 1);
    h->n_addrs = 1;
    ti_parse_matter_instance("A1B2C3D4E5F60718-0000000000000012", h->fabrics[0].cfid, h->fabrics[0].node);
    h->n_fabrics = 1;
    ti_child_t *c = child(0x99, 0x6c09);
    c->ftd = true;
    c->rx_on_idle = true;                                       /* FTD：不登记地址、缓存里也没它 */
    ti_host_t *g = host("A0A1A2A3A4A5A677");                    /* = 下面路由器的扩展地址 */
    g->addrs[0] = ip(6, 2);
    g->n_addrs = 1;
    ti_parse_matter_instance("1111111111111111-0000000000000002", g->fabrics[0].cfid, g->fabrics[0].node);
    g->n_fabrics = 1;
    router(0x77, 0x5000, true);
    ti_associate(&s_raw, NULL, &s_list);
    CHECK(s_list.n == 2, "两台各一条（%u）", s_list.n);
    const ti_entry_t *e = ti_find(&s_list, "th-1011121314151699");
    CHECK(e && e->host == 0 && e->child == 0 && e->link == TI_LINK_CHILD, "FTD 子节点并进主机那条");
    cJSON *j = e ? entry_json(e, false) : NULL;
    CHECK(j && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "matter"), "fabrics")) == 1,
          "fabric 还在");
    cJSON_Delete(j);
    e = ti_find(&s_list, "th-a0a1a2a3a4a5a677");
    CHECK(e && e->host == 1 && e->router == 0 && e->link == TI_LINK_ROUTER, "路由器并进主机那条");
}

/* 第六步核实审计：掉网 / SRP 停过之后重建基线，还没重新注册的不能报假离开；非十六进制主机名的设备并成一条时旧条目悄悄退役 */
static void test_verify_regressions(void)
{
    /* 场景 A：5 台主机；掉网一轮（ESP 层不比较）；回来后分两轮陆续重新注册 —— 一条离开都不报 */
    memset(&s_prev, 0, sizeof(s_prev));
    s_joined = s_left = 0;
    s_now = 0;
    reset();
    const char *names[5] = { "dev-a", "dev-b", "dev-c", "dev-d", "dev-e" };
    for (int i = 0; i < 5; i++) host(names[i]);
    round_();
    round_();
    s_now += 60000;                                             /* 掉网那一轮：不比较 */
    s_prev.have_baseline = false;                               /* thread_inventory 回来后重建基线 */
    s_raw.n_hosts = 2;
    round_();
    s_raw.n_hosts = 4;
    round_();
    round_();
    CHECK(s_left == 0 && s_joined == 0, "掉网回来、陆续重新注册：不报离开也不报新来（%d / %d）", s_left, s_joined);
    s_raw.n_hosts = 4;                                          /* dev-e 再也没回来 */
    for (int i = 0; i < 6; i++) round_();
    CHECK(s_left == 0, "重建基线前就有、之后一次都没出现的：静默期后悄悄退役，不推离开");
    char id[TI_ID_MAX];
    ti_id_from_host("dev-e", id);
    bool still = false;
    for (size_t i = 0; i < sizeof(s_prev.p) / sizeof(s_prev.p[0]); i++) still = still || (s_prev.p[i].used && !strcmp(s_prev.p[i].id, id));
    CHECK(!still, "已从跟踪表退役");
    s_raw.n_hosts = 3;                                          /* 基线之后见过的再消失：照常报离开 */
    round_();
    round_();
    CHECK(s_left == 1, "基线之后见过的再消失：照常报离开");

    /* 场景 B：非十六进制主机名的路由器 —— 先列成两条（th-h 主机 + th-<ext> 路由器），EID 缓存出现后并成一条：不报假离开 */
    memset(&s_prev, 0, sizeof(s_prev));
    s_joined = s_left = 0;
    s_now = 0;
    reset();
    ti_host_t *h = host("HomePod-Living");
    h->addrs[0] = ip(8, 1);
    h->n_addrs = 1;
    router(0x31, 0x3000, true);
    round_();
    CHECK(s_list.n == 2, "先是两条（%u）", s_list.n);
    s_raw.cache[s_raw.n_cache++] = (ti_cache_t){ ip(8, 1), 0x3000 };
    round_();
    CHECK(s_list.n == 1 && ti_find(&s_list, "th-a0a1a2a3a4a5a631") && s_list.e[0].host == 0, "并成一条");
    for (int i = 0; i < 4; i++) round_();
    CHECK(s_left == 0 && s_joined == 0, "并成一条不是一走一来（%d / %d）", s_left, s_joined);

    /* 本 BR 自己是子节点角色（短暂）时：父路由器的 RLOC 不当成"本 BR 的子节点" */
    reset();
    s_raw.my_rloc16 = 0x4801;
    h = host("parent-router");
    h->addrs[0] = ip(9, 1);
    h->n_addrs = 1;
    s_raw.cache[s_raw.n_cache++] = (ti_cache_t){ ip(9, 1), 0x4800 };
    router(0x48, 0x4800, true);
    ti_associate(&s_raw, NULL, &s_list);
    CHECK(s_list.n == 1 && s_list.e[0].link == TI_LINK_ROUTER, "本 BR 是子节点时，父路由器按路由器关联");

    /* 粘连与"主机名就是扩展地址"冲突时，后者优先 */
    memset(&s_prev, 0, sizeof(s_prev));
    snprintf(s_prev.p[0].id, sizeof(s_prev.p[0].id), "th-1011121314151633");
    snprintf(s_prev.p[0].host, sizeof(s_prev.p[0].host), "stale-host");   /* 上一轮的错误关联 */
    s_prev.p[0].used = true;
    reset();
    host("stale-host");
    ti_host_t *real = host("1011121314151633");
    ti_parse_matter_instance("A1B2C3D4E5F60718-0000000000000012", real->fabrics[0].cfid, real->fabrics[0].node);
    real->n_fabrics = 1;
    child(0x33, 0x6c33)->ftd = true;
    ti_associate(&s_raw, &s_prev, &s_list);
    const ti_entry_t *e = ti_find(&s_list, "th-1011121314151633");
    CHECK(e && e->host == 1 && e->child == 0, "主机名就是扩展地址的那台拿到 th-<ext>（带着 fabric），不被陈旧粘连挤掉");
}

/* 2026-10-22 上板复现：Eve 插座是路由器（0x9000，主机名 = 它的扩展地址），Aqara 门窗传感器是它的休眠子节点（0x9005，主机名 = 它自己的
   扩展地址）。传感器的报文由 Eve 转发一跳到本 BR，OpenThread 嗅探记下的是上一跳 Eve 的 RLOC16（子节点号 0）—— EID 缓存在 0x9005 与 0x9000
   之间来回变。原先一指到 0x9000 就把传感器当成 Eve：Eve 自己的主机因 id 冲突被丢、传感器被判"离开"并推给了 Muse。
   要求：始终两条、id 不变、各带各的 fabric，不报离开 / 新来 */
static void test_snooped_parent_rloc(void)
{
    memset(&s_prev, 0, sizeof(s_prev));
    s_joined = s_left = 0;
    s_now = 0;
    reset();
    s_raw.my_rloc16 = 0xF000;
    router(0xFB, 0x9000, true);                                  /* Eve：扩展地址 a0a1a2a3a4a5a6fb */
    ti_host_t *eve = host("A0A1A2A3A4A5A6FB");
    eve->addrs[0] = ip(1, 1);
    eve->n_addrs = 1;
    ti_parse_matter_instance("088E927C6F07E0AC-0000000000000011", eve->fabrics[0].cfid, eve->fabrics[0].node);
    eve->n_fabrics = 1;
    ti_host_t *sensor = host("920E02140E0077C1");
    sensor->addrs[0] = ip(2, 1);
    sensor->n_addrs = 1;
    ti_parse_matter_instance("088E927C6F07E0AC-0000000000000002", sensor->fabrics[0].cfid, sensor->fabrics[0].node);
    sensor->n_fabrics = 1;
    s_raw.cache[s_raw.n_cache++] = (ti_cache_t){ ip(1, 1), 0x9000 };
    s_raw.cache[s_raw.n_cache++] = (ti_cache_t){ ip(2, 1), 0x9005 };
    const uint16_t flips[] = { 0x9005, 0x9000, 0x9000, 0x9005, 0x9000, 0x9005, 0x9005, 0x9000 };
    int bad = 0;
    for (size_t i = 0; i < sizeof(flips) / sizeof(flips[0]); i++) {
        s_raw.cache[1].rloc16 = flips[i];
        round_();
        const ti_entry_t *e = ti_find(&s_list, "th-a0a1a2a3a4a5a6fb");
        const ti_entry_t *t = ti_find(&s_list, "th-920e02140e0077c1");
        const bool ok = s_list.n == 2 && e && t && e->host == 0 && e->link == TI_LINK_ROUTER && t->host == 1 && t->link == TI_LINK_VIA_ROUTER &&
                        t->parent == 0x9000;
        if (!ok) {
            bad++;
            printf("   第 %zu 轮（传感器缓存 0x%04X）：%u 条，Eve %s，传感器 %s\n", i, flips[i], s_list.n, e ? "在" : "没", t ? "在" : "没");
        }
    }
    CHECK(bad == 0, "EID 缓存在子节点 RLOC 与父路由器 RLOC 之间来回变：始终两条、各带各的主机（%d 轮不对）", bad);
    CHECK(s_left == 0 && s_joined == 0, "不报离开 / 新来（%d / %d）", s_left, s_joined);
    s_raw.cache[1].rloc16 = 0x9000;
    ti_associate(&s_raw, &s_prev, &s_list);
    const ti_entry_t *ts = ti_find(&s_list, "th-920e02140e0077c1");
    CHECK(ts && ts->rloc16 == TI_RLOC_NONE && ts->parent == 0x9000, "缓存只给了父路由器的 RLOC：传感器自己的 RLOC16 报不知道（不报 0x9000，免得与 Eve 撞）");

    /* 板上的实际顺序：传感器的主机排在 Eve 前面（复核：Eve 在前时旧代码只错 link、不报假离开，复现不了板上的故障） */
    memset(&s_prev, 0, sizeof(s_prev));
    s_joined = s_left = 0;
    s_now = 0;
    reset();
    s_raw.my_rloc16 = 0xF000;
    router(0xFB, 0x9000, true);
    ti_host_t *sensor2 = host("920E02140E0077C1");
    sensor2->addrs[0] = ip(2, 1);
    sensor2->n_addrs = 1;
    ti_host_t *eve2 = host("A0A1A2A3A4A5A6FB");
    eve2->addrs[0] = ip(1, 1);
    eve2->n_addrs = 1;
    s_raw.cache[s_raw.n_cache++] = (ti_cache_t){ ip(1, 1), 0x9000 };
    s_raw.cache[s_raw.n_cache++] = (ti_cache_t){ ip(2, 1), 0x9005 };
    bad = 0;
    for (size_t i = 0; i < sizeof(flips) / sizeof(flips[0]); i++) {
        s_raw.cache[1].rloc16 = flips[i];
        round_();
        const ti_entry_t *e = ti_find(&s_list, "th-a0a1a2a3a4a5a6fb");
        const ti_entry_t *t = ti_find(&s_list, "th-920e02140e0077c1");
        if (!(s_list.n == 2 && e && t && e->host == 1 && e->link == TI_LINK_ROUTER && t->host == 0 && t->link == TI_LINK_VIA_ROUTER)) bad++;
    }
    CHECK(bad == 0 && s_left == 0 && s_joined == 0, "传感器排在前面：同样始终两条、不报增减（%d 轮不对，离开 %d / 新来 %d）", bad, s_left, s_joined);

    /* 子设备的主机名不是十六进制（非 Matter，比如 HomeKit）、排在前面，缓存指到 Eve：Eve 自己的 Matter 主机在场，它就不是 Eve */
    memset(&s_prev, 0, sizeof(s_prev));
    s_joined = s_left = 0;
    s_now = 0;
    reset();
    s_raw.my_rloc16 = 0xF000;
    router(0xFB, 0x9000, true);
    ti_host_t *hk = host("Eve-Door-HAP-1234");
    hk->addrs[0] = ip(3, 1);
    hk->n_addrs = 1;
    ti_host_t *eve3 = host("A0A1A2A3A4A5A6FB");
    eve3->addrs[0] = ip(1, 1);
    eve3->n_addrs = 1;
    s_raw.cache[s_raw.n_cache++] = (ti_cache_t){ ip(1, 1), 0x9000 };
    s_raw.cache[s_raw.n_cache++] = (ti_cache_t){ ip(3, 1), 0x9003 };
    bad = 0;
    for (size_t i = 0; i < sizeof(flips) / sizeof(flips[0]); i++) {
        s_raw.cache[1].rloc16 = (uint16_t)(flips[i] == 0x9000 ? 0x9000 : 0x9003);
        round_();
        const ti_entry_t *e = ti_find(&s_list, "th-a0a1a2a3a4a5a6fb");
        if (!(s_list.n == 2 && e && e->host == 1 && e->link == TI_LINK_ROUTER)) bad++;
    }
    CHECK(bad == 0 && s_left == 0 && s_joined == 0, "非十六进制主机名的子设备：不把 Eve 挤掉、不报增减（%d 轮不对，离开 %d / 新来 %d）", bad, s_left, s_joined);
    /* 主机名不是十六进制（非 Matter）的照旧：EID 缓存指到路由器 = 就是那台路由器 */
    reset();
    s_raw.my_rloc16 = 0xF000;
    router(0x31, 0x3000, true);
    ti_host_t *hp = host("HomePod-Living");
    hp->addrs[0] = ip(8, 1);
    hp->n_addrs = 1;
    s_raw.cache[s_raw.n_cache++] = (ti_cache_t){ ip(8, 1), 0x3000 };
    ti_associate(&s_raw, NULL, &s_list);
    CHECK(s_list.n == 1 && s_list.e[0].link == TI_LINK_ROUTER && ti_find(&s_list, "th-a0a1a2a3a4a5a631"), "非十六进制主机名：仍按路由器关联");
}

/* 满表：252 个主机 + 64 个子节点 + 62 个路由器，清单装得下、JSON 合法 */
static void test_full(void)
{
    reset();
    for (int i = 0; i < TI_MAX_HOSTS; i++) {
        char n[24];
        snprintf(n, sizeof(n), "%015XF", 0x1000 + i);   /* 前 15 位各不相同（否则按设计会撞 id、被丢掉） */
        ti_host_t *h = host(n);
        h->addrs[0] = ip(0x20 + (uint8_t)(i / 200), (uint8_t)i);
        h->n_addrs = 1;
    }
    for (int i = 0; i < TI_MAX_CHILDREN; i++) child((uint8_t)i, (uint16_t)(0x6c01 + i));
    for (int i = 0; i < 62; i++) router((uint8_t)i, (uint16_t)((i + 1) << 10), i % 2);
    ti_associate(&s_raw, NULL, &s_list);
    CHECK(s_list.n == TI_MAX_HOSTS + TI_MAX_CHILDREN + 61, "满表全列出（62 个路由器里 0x6c00 是本 BR，不列；%u）", s_list.n);
    int ok = 0;
    for (uint16_t i = 0; i < s_list.n; i++) {
        cJSON *j = entry_json(&s_list.e[i], true);
        ok += j != NULL;
        cJSON_Delete(j);
    }
    CHECK(ok == s_list.n, "每一项的详情 JSON 都合法");
}

int main(void)
{
    test_parse();
    test_ids();
    test_associate();
    test_diff();
    test_collisions();
    test_audit_regressions();
    test_hex_host_merge();
    test_verify_regressions();
    test_snooped_parent_rloc();
    test_full();
    printf("hub_ti_core: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
