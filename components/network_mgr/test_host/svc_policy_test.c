/* svc_policy 的主机侧测试 —— 编的是产品源文件本身（../svc_policy.c）。
 *
 * 这几条判据决定"网关会不会去占一个可能已经属于别人的 IP"，写错的两种后果在板子上
 * 都极难归因：该接管的不接管 = ZHA 断到网线插回（T9.20 本来要修的现象）；不该接管的
 * 接管了 = 局域网 IP 冲突，坏的是别人的设备。所以每条拒绝理由都要有一条用例。 */
#include "../svc_policy.h"

#include <stdio.h>
#include <string.h>

static int pass = 0, fail = 0;

static void check(const char *name, int cond)
{
    if (cond) { printf("✅ %s\n", name); pass++; }
    else      { printf("❌ %s\n", name); fail++; }
}

#define IP4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define M24 IP4(255, 255, 255, 0)

static const int64_t NOW = 1791330000LL;   /* 2026-10-06 前后，> SVC_EPOCH_MIN */

static svc_rec_t rec_dhcp(int64_t saved_at, int64_t expiry, uint32_t lease_s)
{
    svc_rec_t r;
    memset(&r, 0, sizeof(r));
    r.ver = SVC_REC_VERSION;
    r.ip = IP4(192, 168, 10, 34);
    r.mask = M24;
    r.gw = IP4(192, 168, 10, 1);
    r.lease_s = lease_s;
    r.saved_at = saved_at;
    r.expiry = expiry;
    r.ssid_hash = svc_ssid_hash("HomeNet-5G");
    return r;
}

static void test_ip_sane(void)
{
    check("ip_sane: 普通私网地址", svc_ip_sane(IP4(192, 168, 10, 34), M24));
    check("ip_sane: /30 也行", svc_ip_sane(IP4(10, 0, 0, 1), IP4(255, 255, 255, 252)));
    check("ip_sane: 拒 0.0.0.0", !svc_ip_sane(0, M24));
    check("ip_sane: 拒掩码 0", !svc_ip_sane(IP4(192, 168, 10, 34), 0));
    check("ip_sane: 拒不连续掩码", !svc_ip_sane(IP4(192, 168, 10, 34), IP4(255, 0, 255, 0)));
    check("ip_sane: 拒 /32", !svc_ip_sane(IP4(192, 168, 10, 34), 0xFFFFFFFFu));
    check("ip_sane: 拒 /31", !svc_ip_sane(IP4(192, 168, 10, 34), 0xFFFFFFFEu));
    check("ip_sane: 拒 127/8", !svc_ip_sane(IP4(127, 0, 0, 1), IP4(255, 0, 0, 0)));
    check("ip_sane: 拒 169.254/16", !svc_ip_sane(IP4(169, 254, 3, 4), IP4(255, 255, 0, 0)));
    check("ip_sane: 拒组播", !svc_ip_sane(IP4(224, 0, 0, 251), M24));
    check("ip_sane: 拒 255.255.255.255", !svc_ip_sane(0xFFFFFFFFu, M24));
    check("ip_sane: 拒网络号", !svc_ip_sane(IP4(192, 168, 10, 0), M24));
    check("ip_sane: 拒本网段广播", !svc_ip_sane(IP4(192, 168, 10, 255), M24));
    check("ip_sane: 拒 0/8", !svc_ip_sane(IP4(0, 1, 2, 3), IP4(255, 0, 0, 0)));
}

static void test_subnet(void)
{
    check("subnet: 同 /24", svc_same_subnet(IP4(192, 168, 10, 34), IP4(192, 168, 10, 36), M24));
    check("subnet: 不同 /24", !svc_same_subnet(IP4(192, 168, 1, 34), IP4(192, 168, 10, 36), M24));
    check("subnet: WiFi 掩码未知", !svc_same_subnet(IP4(192, 168, 10, 34), IP4(192, 168, 10, 36), 0));
    check("subnet: WiFi 没地址", !svc_same_subnet(IP4(192, 168, 10, 34), 0, M24));
    check("subnet: 按 WiFi 的 /16 算", svc_same_subnet(IP4(10, 1, 9, 9), IP4(10, 1, 200, 7), IP4(255, 255, 0, 0)));
}

static void test_ssid_hash(void)
{
    check("ssid_hash: NULL = 0", svc_ssid_hash(NULL) == 0);
    check("ssid_hash: 空串 = 0", svc_ssid_hash("") == 0);
    check("ssid_hash: 非空串非 0", svc_ssid_hash("a") != 0);
    check("ssid_hash: 同串同值", svc_ssid_hash("HomeNet-5G") == svc_ssid_hash("HomeNet-5G"));
    check("ssid_hash: 大小写不同就不同", svc_ssid_hash("HomeNet") != svc_ssid_hash("homenet"));
    /* FNV-1a 32 的已知值，钉住算法（改算法 = 已存记录全部判成"换了网络"） */
    check("ssid_hash: FNV-1a(\"a\") = 0xe40c292c", svc_ssid_hash("a") == 0xe40c292cu);
}

static void test_rec_valid(void)
{
    svc_rec_t r = rec_dhcp(NOW, NOW + 3600, 7200);
    check("rec: 正常记录可用", svc_rec_valid(&r, sizeof(r)));
    check("rec: 长度不对", !svc_rec_valid(&r, sizeof(r) - 1));
    check("rec: NULL", !svc_rec_valid(NULL, sizeof(r)));

    svc_rec_t x = r; x.ver = 2;
    check("rec: 版本不认识", !svc_rec_valid(&x, sizeof(x)));
    x = r; x.flags = 0x80;
    check("rec: 不认识的标志位", !svc_rec_valid(&x, sizeof(x)));
    x = r; x.ip = IP4(169, 254, 1, 1); x.mask = IP4(255, 255, 0, 0); x.gw = 0;
    check("rec: 地址不合理", !svc_rec_valid(&x, sizeof(x)));
    x = r; x.gw = IP4(10, 0, 0, 1);
    check("rec: 网关不在本网段", !svc_rec_valid(&x, sizeof(x)));
    x = r; x.gw = 0;
    check("rec: 没网关也算可用", svc_rec_valid(&x, sizeof(x)));
    x = r; x.saved_at = 1000; x.expiry = 2000;
    check("rec: 写入时间早于 2025（墙钟没对过）", !svc_rec_valid(&x, sizeof(x)));
    x = r; x.expiry = x.saved_at;
    check("rec: 写入时就已过期", !svc_rec_valid(&x, sizeof(x)));
    x = r; x.expiry = x.saved_at + 7200 + 61;
    check("rec: 剩余比整条租约还长", !svc_rec_valid(&x, sizeof(x)));
    x = r; x.lease_s = 0;
    check("rec: 租约长度 0", !svc_rec_valid(&x, sizeof(x)));
    x = r; x.flags = SVC_REC_F_INFINITE; x.expiry = 0;
    check("rec: 无限租期", svc_rec_valid(&x, sizeof(x)));
    x = r; x.flags = SVC_REC_F_INFINITE; x.expiry = NOW + 10;
    check("rec: 无限租期却带到期时刻", !svc_rec_valid(&x, sizeof(x)));
    x = r; x.reserved = 1;
    check("rec: 保留字段非 0", !svc_rec_valid(&x, sizeof(x)));
    x = r; x.ssid_hash = 0;
    check("rec: 写的时候没配 WiFi（产线出货板）→ 不存不读", !svc_rec_valid(&x, sizeof(x)));
    x = r; x.lease_s = 2u * 366 * 24 * 3600; x.expiry = x.saved_at + 3600;
    check("rec: 两年的超长租约也认（不再静默存不进去）", svc_rec_valid(&x, sizeof(x)));
    x = r; x.lease_s = 11u * 366 * 24 * 3600; x.expiry = x.saved_at + 3600;
    check("rec: 十年以上当垃圾值", !svc_rec_valid(&x, sizeof(x)));
}

static void test_decide(void)
{
    const uint32_t wip = IP4(192, 168, 10, 36);
    const uint32_t H = svc_ssid_hash("HomeNet-5G");
    svc_rec_t r = rec_dhcp(NOW - 600, NOW + 3600, 14400);

    check("decide: 没记录", svc_decide(NULL, wip, M24, H, true, NOW, 300) == SVC_DENY_NO_RECORD);
    check("decide: 正常 → ok", svc_decide(&r, wip, M24, H, true, NOW, 300) == SVC_OK);
    check("decide: 时钟不可信 → 等对时", svc_decide(&r, wip, M24, H, false, 0, 300) == SVC_WAIT_CLOCK);
    check("decide: 网段不同（换了网络）", svc_decide(&r, IP4(192, 168, 1, 50), M24, H, true, NOW, 300) == SVC_DENY_SUBNET);
    check("decide: 网段比对先于时钟", svc_decide(&r, IP4(192, 168, 1, 50), M24, H, false, 0, 300) == SVC_DENY_SUBNET);
    svc_rec_t b23 = r; b23.ip = IP4(192, 168, 10, 255); b23.mask = IP4(255, 255, 254, 0); b23.gw = IP4(192, 168, 10, 1);
    check("decide: 有线 /23 的合法地址，按 WiFi 的 /24 是广播号 → 拒（探测会永远 ERR_ARG）",
          svc_decide(&b23, wip, M24, H, true, NOW, 300) == SVC_DENY_SUBNET);
    check("decide: WiFi 拿到的就是它", svc_decide(&r, r.ip, M24, H, true, NOW, 300) == SVC_DENY_SAME_AS_WIFI);
    check("decide: 换了 WiFi（SSID 不同）", svc_decide(&r, wip, M24, svc_ssid_hash("Neighbor"), true, NOW, 300) == SVC_DENY_NETWORK_CHANGED);
    check("decide: 现在没配 WiFi SSID", svc_decide(&r, wip, M24, 0, true, NOW, 300) == SVC_DENY_NETWORK_CHANGED);
    svc_rec_t z = r; z.ssid_hash = 0;
    check("decide: 写记录时没配 WiFi → 无从比对，不接管", svc_decide(&z, wip, M24, H, true, NOW, 300) == SVC_DENY_NETWORK_CHANGED);
    check("decide: 换网络先于时钟判定", svc_decide(&r, wip, M24, svc_ssid_hash("Neighbor"), false, 0, 300) == SVC_DENY_NETWORK_CHANGED);

    svc_rec_t e = rec_dhcp(NOW - 7200, NOW + 299, 14400);
    check("decide: 只剩余量以内 → 过期", svc_decide(&e, wip, M24, H, true, NOW, 300) == SVC_DENY_EXPIRED);
    e.expiry = NOW + 301;
    check("decide: 余量外 1 s → ok", svc_decide(&e, wip, M24, H, true, NOW, 300) == SVC_OK);
    e.expiry = NOW;
    check("decide: 正好到期", svc_decide(&e, wip, M24, H, true, NOW, 0) == SVC_DENY_EXPIRED);
    e.expiry = NOW - 5;
    check("decide: 早已过期", svc_decide(&e, wip, M24, H, true, NOW, 300) == SVC_DENY_EXPIRED);
    e.expiry = NOW + 1;
    check("decide: 负余量按 0", svc_decide(&e, wip, M24, H, true, NOW, -50) == SVC_OK);

    svc_rec_t b = rec_dhcp(NOW + 100, NOW + 3700, 14400);
    check("decide: 现在早于写入时刻（时钟倒退）", svc_decide(&b, wip, M24, H, true, NOW, 300) == SVC_DENY_CLOCK_BACKWARDS);

    svc_rec_t inf = rec_dhcp(NOW - 600, 0, 0);
    inf.flags = SVC_REC_F_INFINITE;
    check("decide: 无限租期不看时钟", svc_decide(&inf, wip, M24, H, false, 0, 300) == SVC_OK);
    check("decide: 无限租期也要同网段", svc_decide(&inf, IP4(10, 0, 0, 5), IP4(255, 0, 0, 0), H, false, 0, 300) == SVC_DENY_SUBNET);

    /* 每个结论都有稳定的名字，/status 与日志靠它 */
    int names_ok = 1;
    for (int v = SVC_OK; v <= SVC_DENY_NETWORK_CHANGED; v++) {
        if (strcmp(svc_verdict_name((svc_verdict_t)v), "unknown") == 0) names_ok = 0;
    }
    check("verdict_name: 每个枚举值都有名字", names_ok);
    check("verdict_name: 越界给 unknown", strcmp(svc_verdict_name((svc_verdict_t)99), "unknown") == 0);
}

static void test_persist(void)
{
    const int64_t MIN_IV = 1800;
    svc_rec_t old = rec_dhcp(NOW - 4000, NOW + 3200, 14400);   /* 4 h 租约，上次写在 4000 s 前 */
    svc_rec_t neu = old;
    neu.saved_at = NOW;

    check("persist: 没写过 → 写", svc_persist_needed(NULL, &neu, NOW, MIN_IV));
    check("persist: neu=NULL → 不写", !svc_persist_needed(&old, NULL, NOW, MIN_IV));
    check("persist: 什么都没变 → 不写", !svc_persist_needed(&old, &neu, NOW, MIN_IV));

    svc_rec_t x = neu; x.ip = IP4(192, 168, 10, 35);
    check("persist: 地址变了 → 立刻写", svc_persist_needed(&old, &x, NOW, MIN_IV));
    x = neu; x.ssid_hash = svc_ssid_hash("Other");
    check("persist: WiFi SSID 变了 → 立刻写", svc_persist_needed(&old, &x, NOW, MIN_IV));
    x = neu; x.mask = IP4(255, 255, 0, 0);
    check("persist: 掩码变了 → 立刻写", svc_persist_needed(&old, &x, NOW, MIN_IV));

    x = neu; x.expiry = old.expiry + 7200;                      /* 续租：推后半个租期 */
    check("persist: 续租推后半个租期 → 写", svc_persist_needed(&old, &x, NOW, MIN_IV));
    x = neu; x.expiry = old.expiry + 3599;                      /* < lease/4 = 3600 */
    check("persist: 推后不足 lease/4 → 不写", !svc_persist_needed(&old, &x, NOW, MIN_IV));
    x = neu; x.expiry = old.expiry + 7200;
    check("persist: 推后够了但距上次写不足间隔 → 不写", !svc_persist_needed(&old, &x, old.saved_at + MIN_IV - 1, MIN_IV));
    x = neu; x.expiry = old.expiry - 30;
    check("persist: 提前 30 s（抖动）→ 不写", !svc_persist_needed(&old, &x, NOW, MIN_IV));
    x = neu; x.expiry = old.expiry - 61;
    check("persist: 提前超过 60 s（租期缩短/时钟回校）→ 立刻写", svc_persist_needed(&old, &x, old.saved_at + 1, MIN_IV));

    svc_rec_t s_old = rec_dhcp(NOW - 4000, NOW - 4000 + 120, 300);   /* 300 s 短租约 */
    svc_rec_t s_neu = s_old; s_neu.expiry = s_old.expiry + 150; s_neu.saved_at = NOW;
    check("persist: 短租约续租推后 150 s（≥ 步长下限 120）→ 写", svc_persist_needed(&s_old, &s_neu, NOW, 300));
    s_neu.expiry = s_old.expiry + 100;
    check("persist: 推后 100 s（< 步长下限 120，算抖动）→ 不写", !svc_persist_needed(&s_old, &s_neu, NOW, 300));

    svc_rec_t i_old = old; i_old.flags = SVC_REC_F_INFINITE; i_old.expiry = 0;
    svc_rec_t i_neu = i_old; i_neu.saved_at = NOW;
    check("persist: 无限租期没变 → 不写", !svc_persist_needed(&i_old, &i_neu, NOW, MIN_IV));
    check("persist: 有限 ↔ 无限 → 立刻写", svc_persist_needed(&old, &i_neu, NOW, MIN_IV));

    /* 写入次数上界：模拟 4 h 租约 + T1 续租 + 每 300 s 读一次，跑一天，数写盘次数 */
    svc_rec_t cur = rec_dhcp(NOW, NOW + 14400, 14400);
    int writes = 1;
    int64_t bind = NOW;
    for (int64_t t = NOW + 300; t <= NOW + 86400; t += 300) {
        if (t - bind >= 7200) bind = t;                         /* T1 = 50% 续租成功 */
        svc_rec_t rd = cur;
        rd.expiry = bind + 14400;
        rd.saved_at = t;
        if (svc_persist_needed(&cur, &rd, t, MIN_IV)) { cur = rd; writes++; }
    }
    char name[128];
    snprintf(name, sizeof(name), "persist: 4 h 租约一天写盘 %d 次（≤ 13：每次续租最多一次）", writes);
    check(name, writes <= 13 && writes >= 12);
    check("persist: 一天后 NVS 里的到期时刻不晚于真实到期时刻", cur.expiry <= bind + 14400);

    /* 30 分钟短租约：T1 = 900 s 续租，每 300 s 读一次，写盘间隔照 network_mgr.c 取 lease/4 夹 300–1800。
       审计 F10：以前固定 30 分钟间隔 + 600 s 步长，NVS 里的到期时刻会落后一整个租期、开机总判过期。 */
    const uint32_t L = 1800;
    int64_t iv = L / 4; if (iv < 300) iv = 300; if (iv > 1800) iv = 1800;
    svc_rec_t sc = rec_dhcp(NOW, NOW + L, L);
    int sw = 1; int64_t sbind = NOW; int64_t worst_lag = 0; int ahead = 0;
    for (int64_t t = NOW + 300; t <= NOW + 86400; t += 300) {
        if (t - sbind >= L / 2) sbind = t;
        svc_rec_t rd = sc; rd.expiry = sbind + L; rd.saved_at = t;
        if (svc_persist_needed(&sc, &rd, t, iv)) { sc = rd; sw++; }
        int64_t lag = (sbind + L) - sc.expiry;
        if (lag > worst_lag) worst_lag = lag;
        if (sc.expiry > sbind + L) ahead = 1;
    }
    snprintf(name, sizeof(name), "persist: 30 分钟租约一天写盘 %d 次（≤ 100）", sw);
    check(name, sw <= 100);
    snprintf(name, sizeof(name), "persist: 30 分钟租约 NVS 到期时刻最多落后 %lld s（≤ 半个租期 900）", (long long)worst_lag);
    check(name, worst_lag <= 900);
    check("persist: 30 分钟租约 NVS 到期时刻从不晚于真实值", !ahead);
}

static void test_gate(void)
{
    svc_gate_in_t in = {
        .state = SVC_BT_WAIT, .eth_link_up = false, .eth_got_ip_this_boot = false,
        .wifi_has_own_ip = true, .since_init_ms = 30000, .grace_ms = 20000,
    };
    check("gate: 条件都满足 → decide", svc_gate_step(&in) == SVC_GATE_DECIDE);

    svc_gate_in_t x = in; x.state = SVC_BT_NONE;
    check("gate: 没候选 → 不动", svc_gate_step(&x) == SVC_GATE_NOTHING);
    x = in; x.state = SVC_BT_TAKEN;
    check("gate: 已接管 → 不动（归运行中浮动管）", svc_gate_step(&x) == SVC_GATE_NOTHING);
    x = in; x.state = SVC_BT_DENIED;
    check("gate: 已拒绝 → 本次开机不再试", svc_gate_step(&x) == SVC_GATE_NOTHING);

    x = in; x.eth_got_ip_this_boot = true;
    check("gate: 以太网本次拿到过地址 → 退场", svc_gate_step(&x) == SVC_GATE_RETIRE);
    x.state = SVC_BT_PROBING;
    check("gate: 探测中以太网拿到地址 → 也退场", svc_gate_step(&x) == SVC_GATE_RETIRE);

    x = in; x.eth_link_up = true;
    check("gate: 网线插着（还没拿到地址）→ 等", svc_gate_step(&x) == SVC_GATE_HOLD);
    x.state = SVC_BT_PROBING;
    check("gate: 探测中插上网线 → 作废探测", svc_gate_step(&x) == SVC_GATE_ABORT);

    x = in; x.wifi_has_own_ip = false;
    check("gate: WiFi 没地址 → 等", svc_gate_step(&x) == SVC_GATE_HOLD);
    x.state = SVC_BT_PROBING;
    check("gate: 探测中 WiFi 掉线 → 作废探测", svc_gate_step(&x) == SVC_GATE_ABORT);

    x = in; x.since_init_ms = 5000;
    check("gate: 宽限期内 → 定时再看", svc_gate_step(&x) == SVC_GATE_ARM_GRACE);
    x.since_init_ms = 19999;
    check("gate: 宽限期最后 1 ms → 仍等", svc_gate_step(&x) == SVC_GATE_ARM_GRACE);
    x.since_init_ms = 20000;
    check("gate: 宽限期刚到 → decide", svc_gate_step(&x) == SVC_GATE_DECIDE);

    x = in; x.state = SVC_BT_PROBING;
    check("gate: 探测在途、条件仍满足 → 等结果", svc_gate_step(&x) == SVC_GATE_NOTHING);
    check("gate: NULL", svc_gate_step(NULL) == SVC_GATE_NOTHING);
}

static void test_mdns(void)
{
    /* 基线：开机 5 s，以太网在等 DHCP —— 一个个改字段看结论 */
    const svc_mdns_in_t base = { .since_init_ms = 5000, .hold_cap_ms = 60000 };
    svc_mdns_in_t x;

    x = base; x.eth_has_ip = true; x.svc_ip_valid = true;
    check("mdns: 以太网有地址 → 只宣告服务 IP", svc_mdns_decide(&x) == SVC_MDNS_FIXED);
    x = base; x.svc_on_wifi = true; x.svc_ip_valid = true;
    check("mdns: 服务 IP 浮在 WiFi 上 → 仍只宣告服务 IP", svc_mdns_decide(&x) == SVC_MDNS_FIXED);
    x = base; x.svc_ip_valid = true; x.eth_link_up = true; x.eth_dhcp_pending = true; x.since_init_ms = 3600000;
    check("mdns: 插回网线等 DHCP（运行中）→ 继续宣告服务 IP，不换 WiFi 地址", svc_mdns_decide(&x) == SVC_MDNS_FIXED);
    x = base; x.eth_link_up = true; x.eth_dhcp_pending = true;
    check("mdns: 插线开机、以太网还在拿地址 → 先不给", svc_mdns_decide(&x) == SVC_MDNS_HOLD);
    x = base; x.eth_link_up = true; x.since_init_ms = 15000;
    check("mdns: 插线开机、DHCP 慢过 10 s 超时 → 仍不给（看链路不看 DHCP 超时）", svc_mdns_decide(&x) == SVC_MDNS_HOLD);
    x = base; x.eth_link_up = true; x.since_init_ms = 60000;
    check("mdns: 插着没有 DHCP 的口、到上限 → 给 WiFi 自己的地址", svc_mdns_decide(&x) == SVC_MDNS_DEFAULT);
    x = base; x.eth_link_up = true; x.svc_ip_valid = true; x.since_init_ms = 3600000;
    check("mdns: 插回网线后 DHCP 失败（超时已过）→ 给 WiFi 自己的地址", svc_mdns_decide(&x) == SVC_MDNS_DEFAULT);
    x = base; x.bt_pending = true;
    check("mdns: 拔线开机、开机接管在判 → 先不给", svc_mdns_decide(&x) == SVC_MDNS_HOLD);
    x = base; x.bt_pending = true; x.since_init_ms = 59999;
    check("mdns: 上限前 1 ms → 仍不给", svc_mdns_decide(&x) == SVC_MDNS_HOLD);
    x = base; x.bt_pending = true; x.since_init_ms = 60000;
    check("mdns: 到上限（等对时拖住）→ 先给 WiFi 自己的地址", svc_mdns_decide(&x) == SVC_MDNS_DEFAULT);
    x = base; x.eth_link_up = true; x.eth_dhcp_pending = true; x.since_init_ms = 3600000;
    check("mdns: 运行中第一次插上网线（还没服务 IP）→ 不憋，照旧给 WiFi 地址", svc_mdns_decide(&x) == SVC_MDNS_DEFAULT);
    x = base;
    check("mdns: 开机、没网线也没候选（只用 WiFi）→ 直接给 WiFi 地址", svc_mdns_decide(&x) == SVC_MDNS_DEFAULT);
    x = base; x.svc_ip_valid = true; x.since_init_ms = 3600000;
    check("mdns: 有快照但网线断着、没浮过去（SSID 锁 / 网段变了）→ WiFi 地址", svc_mdns_decide(&x) == SVC_MDNS_DEFAULT);
    x = base; x.eth_has_ip = true; x.svc_ip_valid = true; x.bt_pending = true;
    check("mdns: 以太网有地址优先于开机等待", svc_mdns_decide(&x) == SVC_MDNS_FIXED);
    check("mdns: NULL", svc_mdns_decide(NULL) == SVC_MDNS_DEFAULT);
    /* T9.37：服务身份生效时只看 id_* */
    x = base; x.id_active = true; x.id_known = true; x.since_init_ms = 3600000;
    check("mdns/id: 知道 A_S → 宣告它（不管以太网有没有地址）", svc_mdns_decide(&x) == SVC_MDNS_FIXED);
    x = base; x.id_active = true; x.id_known = true; x.eth_link_up = true; x.eth_dhcp_pending = true;
    check("mdns/id: 交接 / 开机空档里也宣告 A_S", svc_mdns_decide(&x) == SVC_MDNS_FIXED);
    x = base; x.id_active = true; x.since_init_ms = 5000;
    check("mdns/id: 首次安装还在定 S → 先不给", svc_mdns_decide(&x) == SVC_MDNS_HOLD);
    x = base; x.id_active = true; x.since_init_ms = 60000;
    check("mdns/id: 到上限还没定 → 给 WiFi 自己的地址", svc_mdns_decide(&x) == SVC_MDNS_DEFAULT);
    x = base; x.id_active = true; x.eth_has_ip = true; x.since_init_ms = 5000;
    check("mdns/id: 身份机制下不看以太网地址（它可能是 other(S) 的地址）", svc_mdns_decide(&x) == SVC_MDNS_HOLD);
}

/* ---- T9.37 服务身份（设计 docs/16 §5 / §7 的每一行都有一条用例）---- */
static svc_id_rec_t idrec(uint8_t s_nic, uint32_t ip)
{
    svc_id_rec_t r;
    memset(&r, 0, sizeof(r));
    r.ver = SVC_ID_REC_VERSION;
    r.s_nic = s_nic;
    r.net_ssid = svc_ssid_hash("HomeNet-5G");
    if (ip) { r.ip = ip; r.mask = M24; r.gw = IP4(192, 168, 10, 1); }
    return r;
}

static void test_id_rec(void)
{
    svc_id_rec_t r = idrec(SVC_NIC_ETH, IP4(192, 168, 10, 34));
    check("idrec: 正常记录", svc_id_rec_valid(&r, sizeof(r)));
    check("idrec: 长度不对", !svc_id_rec_valid(&r, sizeof(r) - 1));
    check("idrec: NULL", !svc_id_rec_valid(NULL, sizeof(r)));
    r = idrec(SVC_NIC_WIFI, 0);
    check("idrec: 还不知道 A_S（全 0）也合法", svc_id_rec_valid(&r, sizeof(r)));
    r.mask = M24;
    check("idrec: A_S 为 0 却有掩码 → 坏", !svc_id_rec_valid(&r, sizeof(r)));
    r = idrec(SVC_NIC_NONE, IP4(192, 168, 10, 34));
    check("idrec: S 不是网口 / WiFi → 坏", !svc_id_rec_valid(&r, sizeof(r)));
    r = idrec(SVC_NIC_ETH, IP4(192, 168, 10, 34)); r.ver = 9;
    check("idrec: 版本不认识", !svc_id_rec_valid(&r, sizeof(r)));
    r = idrec(SVC_NIC_ETH, IP4(192, 168, 10, 34)); r.mode = 3;
    check("idrec: mode 越界", !svc_id_rec_valid(&r, sizeof(r)));
    r = idrec(SVC_NIC_ETH, IP4(192, 168, 10, 34)); r.flags = SVC_ID_F_AS_NATIVE;
    check("idrec: 带 AS_NATIVE 标志", svc_id_rec_valid(&r, sizeof(r)));
    r = idrec(SVC_NIC_ETH, IP4(192, 168, 10, 34)); r.flags = 0x80;
    check("idrec: 不认识的标志位", !svc_id_rec_valid(&r, sizeof(r)));
    r = idrec(SVC_NIC_ETH, IP4(192, 168, 10, 34)); r.net_ssid = 0;
    check("idrec: 没配 WiFi 时定的 S 不存（产线）", !svc_id_rec_valid(&r, sizeof(r)));
    r = idrec(SVC_NIC_ETH, IP4(192, 168, 10, 255));
    check("idrec: A_S 是广播号", !svc_id_rec_valid(&r, sizeof(r)));
    r = idrec(SVC_NIC_ETH, IP4(192, 168, 10, 34)); r.gw = IP4(10, 0, 0, 1);
    check("idrec: 网关不在网段里", !svc_id_rec_valid(&r, sizeof(r)));
    r = idrec(SVC_NIC_ETH, IP4(192, 168, 10, 34)); r.mode = SVC_IDMODE_MAC_ONLY; r.mode_ts = 1800000000u;
    check("idrec: 只认 MAC 带计时", svc_id_rec_valid(&r, sizeof(r)));
    r = idrec(SVC_NIC_ETH, IP4(192, 168, 10, 34)); r.mode = SVC_IDMODE_CID; r.mode_ts = 1800000000u;
    check("idrec: 不是只认 MAC 却带计时 → 坏", !svc_id_rec_valid(&r, sizeof(r)));
    check("idrec: v3 是 28 字节（v2 24 字节 + mode_ts）", sizeof(svc_id_rec_t) == 28 && SVC_ID_REC_V2_LEN == 24);
}

static void test_id_rec_load(void)
{
    svc_id_rec_t out, r = idrec(SVC_NIC_WIFI, IP4(192, 168, 10, 36));
    r.mode = SVC_IDMODE_CID; r.flags = SVC_ID_F_AS_NATIVE; r.mode_ssid = r.net_ssid;
    check("load: v3 原样", svc_id_rec_load(&r, sizeof(r), &out) && !memcmp(&out, &r, sizeof(r)));

    /* v2：同样的前 24 字节、ver = 2（10-07 板上那条就是这种） */
    uint8_t v2[SVC_ID_REC_V2_LEN];
    memcpy(v2, &r, sizeof(v2));
    v2[0] = 2;
    check("load: v2 升成 v3", svc_id_rec_load(v2, sizeof(v2), &out) && out.ver == SVC_ID_REC_VERSION &&
          out.s_nic == SVC_NIC_WIFI && out.ip == IP4(192, 168, 10, 36) && out.mode == SVC_IDMODE_CID &&
          out.flags == SVC_ID_F_AS_NATIVE && out.net_ssid == r.net_ssid && out.mode_ts == 0);
    v2[0] = 1;
    check("load: 24 字节但版本不是 2 → 不认", !svc_id_rec_load(v2, sizeof(v2), &out));
    v2[0] = 2; v2[1] = 7;
    check("load: v2 字段不合理 → 不认", !svc_id_rec_load(v2, sizeof(v2), &out));
    check("load: 20 字节（v1）→ 不认", !svc_id_rec_load(&r, 20, &out));
    r.ver = 2;
    check("load: 28 字节却写着 v2 → 不认", !svc_id_rec_load(&r, sizeof(r), &out));
    check("load: NULL", !svc_id_rec_load(NULL, sizeof(r), &out) && !svc_id_rec_load(&r, sizeof(r), NULL));
}

static void test_id_mac_only_age(void)
{
    const uint32_t P = SVC_ID_MAC_ONLY_RETRY_S, T0 = 1800000000u;
    check("maconly: 7 天", P == 604800u);
    check("maconly: 墙钟不可信 → 不动", svc_id_mac_only_age(0, false, T0, P) == SVC_MACONLY_KEEP);
    check("maconly: 墙钟不可信、早就到期 → 也不动", svc_id_mac_only_age(T0, false, (int64_t)T0 + 30 * 86400, P) == SVC_MACONLY_KEEP);
    check("maconly: 还没开始计 → 开始", svc_id_mac_only_age(0, true, T0, P) == SVC_MACONLY_START);
    check("maconly: 刚记下 → 不动", svc_id_mac_only_age(T0, true, T0 + 60, P) == SVC_MACONLY_KEEP);
    check("maconly: 差 1 秒到 7 天 → 不动", svc_id_mac_only_age(T0, true, (int64_t)T0 + P - 1, P) == SVC_MACONLY_KEEP);
    check("maconly: 满 7 天 → 重试", svc_id_mac_only_age(T0, true, (int64_t)T0 + P, P) == SVC_MACONLY_RETRY);
    check("maconly: 墙钟比记的还早（回拨）→ 重新开始计", svc_id_mac_only_age(T0, true, T0 - 3600, P) == SVC_MACONLY_START);
    check("maconly: 墙钟是 0 → 不动", svc_id_mac_only_age(0, true, 0, P) == SVC_MACONLY_KEEP);
}

static void test_id_degrade(void)
{
    check("degrade: S = WiFi → 钉在 WiFi 上", svc_id_degrade_mode(SVC_NIC_WIFI) == SVC_DEGRADE_PIN_WIFI);
    check("degrade: S = 网口 → 旧规则", svc_id_degrade_mode(SVC_NIC_ETH) == SVC_DEGRADE_LEGACY);
    check("degrade: S 没定 → 旧规则", svc_id_degrade_mode(SVC_NIC_NONE) == SVC_DEGRADE_LEGACY);
}

static void test_id_cid(void)
{
    check("cid: S 没定 → 自然身份", svc_id_cid_for(SVC_NIC_ETH, SVC_NIC_ETH, SVC_NIC_NONE, SVC_IDMODE_UNKNOWN) == SVC_CID_SELF);
    check("cid: 开机待定 → 自然身份", svc_id_cid_for(SVC_NIC_WIFI, SVC_NIC_NONE, SVC_NIC_ETH, SVC_IDMODE_CID) == SVC_CID_SELF);
    check("cid: 只认 MAC → 自然身份（即使持有者不是 S 的本卡）", svc_id_cid_for(SVC_NIC_ETH, SVC_NIC_ETH, SVC_NIC_WIFI, SVC_IDMODE_MAC_ONLY) == SVC_CID_SELF);
    check("cid: 先网口装、网口持有 → 网口自然", svc_id_cid_for(SVC_NIC_ETH, SVC_NIC_ETH, SVC_NIC_ETH, SVC_IDMODE_UNKNOWN) == SVC_CID_SELF);
    check("cid: 先网口装、网口持有 → WiFi 自然", svc_id_cid_for(SVC_NIC_WIFI, SVC_NIC_ETH, SVC_NIC_ETH, SVC_IDMODE_UNKNOWN) == SVC_CID_SELF);
    check("cid: 先网口装、拔线 WiFi 持有 → WiFi 用网口的身份", svc_id_cid_for(SVC_NIC_WIFI, SVC_NIC_WIFI, SVC_NIC_ETH, SVC_IDMODE_CID) == SVC_CID_PEER);
    check("cid: 先 WiFi 装、插线网口持有 → 网口用 WiFi 的身份", svc_id_cid_for(SVC_NIC_ETH, SVC_NIC_ETH, SVC_NIC_WIFI, SVC_IDMODE_UNKNOWN) == SVC_CID_PEER);
    check("cid: 先 WiFi 装、插线网口持有 → WiFi 用网口的身份", svc_id_cid_for(SVC_NIC_WIFI, SVC_NIC_ETH, SVC_NIC_WIFI, SVC_IDMODE_UNKNOWN) == SVC_CID_PEER);
    check("cid: 先 WiFi 装、WiFi 持有 → WiFi 自然", svc_id_cid_for(SVC_NIC_WIFI, SVC_NIC_WIFI, SVC_NIC_WIFI, SVC_IDMODE_CID) == SVC_CID_SELF);
    check("cid: 不认识的卡 → 自然", svc_id_cid_for(SVC_NIC_NONE, SVC_NIC_ETH, SVC_NIC_WIFI, SVC_IDMODE_CID) == SVC_CID_SELF);
}

static void test_id_holder(void)
{
    svc_id_holder_in_t b = { .cur = SVC_NIC_NONE, .wifi_usable = true, .eth_ip_in_svc_net = true,
                             .since_init_ms = 0, .grace_ms = 10000 };
    svc_id_holder_in_t x;
    /* 开机待定 */
    x = b; x.eth_link_up = true;
    check("holder: 开机网口链路在 → 推定网口", svc_id_next_holder(&x) == SVC_NIC_ETH);
    x = b; x.since_init_ms = 9999;
    check("holder: 开机没链路、宽限内 → 继续待定", svc_id_next_holder(&x) == SVC_NIC_NONE);
    x = b; x.since_init_ms = 10000;
    check("holder: 开机没链路、宽限到 → WiFi", svc_id_next_holder(&x) == SVC_NIC_WIFI);
    x = b; x.since_init_ms = 10000; x.wifi_usable = false;
    check("holder: 宽限到但 WiFi 当不了 → 继续待定", svc_id_next_holder(&x) == SVC_NIC_NONE);
    /* 网口持有 */
    b.cur = SVC_NIC_ETH;
    x = b; x.eth_link_up = true; x.eth_has_ip = true;
    check("holder: 网口持有、正常 → 网口", svc_id_next_holder(&x) == SVC_NIC_ETH);
    x = b; x.eth_link_up = false;
    check("holder: 网口持有、链路断 → WiFi", svc_id_next_holder(&x) == SVC_NIC_WIFI);
    x = b; x.eth_link_up = true; x.eth_dhcp_expired = true;
    check("holder: 网口持有、10 s 拿不到地址 → WiFi", svc_id_next_holder(&x) == SVC_NIC_WIFI);
    x = b; x.eth_link_up = true; x.eth_dhcp_expired = true; x.eth_has_ip = true;
    check("holder: 超时标志在但其实已有地址 → 网口", svc_id_next_holder(&x) == SVC_NIC_ETH);
    x = b; x.eth_link_up = true;
    check("holder: 网口持有、还在等 DHCP（没超时）→ 网口", svc_id_next_holder(&x) == SVC_NIC_ETH);
    x = b; x.eth_link_up = false; x.wifi_usable = false;
    check("holder: 网口断但 WiFi 当不了 → 留在网口", svc_id_next_holder(&x) == SVC_NIC_ETH);
    x = b; x.eth_link_up = true; x.eth_has_ip = true; x.eth_ip_in_svc_net = false;
    check("holder: 网口持有、拿到别的网段 → 交回 WiFi", svc_id_next_holder(&x) == SVC_NIC_WIFI);
    x = b; x.eth_link_up = true; x.eth_has_ip = true; x.eth_ip_in_svc_net = false; x.wifi_usable = false;
    check("holder: 网口在别的网段但 WiFi 当不了 → 留在网口", svc_id_next_holder(&x) == SVC_NIC_ETH);
    /* WiFi 持有 */
    b.cur = SVC_NIC_WIFI;
    x = b;
    check("holder: WiFi 持有、没网线 → WiFi", svc_id_next_holder(&x) == SVC_NIC_WIFI);
    x = b; x.eth_link_up = true;
    check("holder: 运行中插线、网口还没地址 → 不推定，WiFi", svc_id_next_holder(&x) == SVC_NIC_WIFI);
    x = b; x.eth_link_up = true; x.eth_has_ip = true;
    check("holder: 网口用 other(S) 拿到同网段地址 → 网口", svc_id_next_holder(&x) == SVC_NIC_ETH);
    x = b; x.eth_link_up = true; x.eth_has_ip = true; x.eth_ip_in_svc_net = false;
    check("holder: 网口在另一张网上 → 仍是 WiFi", svc_id_next_holder(&x) == SVC_NIC_WIFI);
    x = b; x.eth_link_up = true; x.wifi_usable = false;
    check("holder: WiFi 当不了持有者、网口一有链路就接", svc_id_next_holder(&x) == SVC_NIC_ETH);
    check("holder: NULL", svc_id_next_holder(NULL) == SVC_NIC_NONE);
}

static void test_id_judge(void)
{
    const uint32_t AS = IP4(192, 168, 10, 36), OWN = IP4(192, 168, 10, 34);
    svc_id_judge_in_t b = { .is_holder = true, .swapped = false, .mode = SVC_IDMODE_UNKNOWN,
                            .a = AS, .mask = M24, .a_s = AS, .other_nic_in_svc_net = false,
                            .as_native = true, .own_ip = 0, .declined = false };
    svc_id_judge_in_t x;
    x = b; x.is_holder = false;
    check("judge: 非持有者拿到地址 → 不管", svc_id_judge(&x) == SVC_JUDGE_NONE);
    x = b; x.a = 0;
    check("judge: 地址 0 → 不管", svc_id_judge(&x) == SVC_JUDGE_NONE);
    x = b; x.a_s = 0;
    check("judge: 还没有 A_S → 采用", svc_id_judge(&x) == SVC_JUDGE_ADOPT);
    x = b;
    check("judge: 拿到的就是 A_S（不交换）→ OK", svc_id_judge(&x) == SVC_JUDGE_OK);
    x = b; x.swapped = true;
    check("judge: 交换着拿到 A_S、mode 未知、A_S 是本卡拿的 → 证实认 client-id", svc_id_judge(&x) == SVC_JUDGE_CID_CONFIRMED);
    x = b; x.swapped = true; x.as_native = false;
    check("judge: 交换着拿到 A_S、但 A_S 本身是交换着拿的 → 证实不了，只是 OK", svc_id_judge(&x) == SVC_JUDGE_OK);
    x = b; x.swapped = true; x.mode = SVC_IDMODE_CID;
    check("judge: 交换着拿到 A_S、早已证实 → OK", svc_id_judge(&x) == SVC_JUDGE_OK);
    x = b; x.a = IP4(192, 168, 1, 50);
    check("judge: 不同网段、另一张卡不在旧网段 → 换了网络（重定 S）", svc_id_judge(&x) == SVC_JUDGE_NET_CHANGED);
    x = b; x.a = IP4(192, 168, 1, 50); x.other_nic_in_svc_net = true;
    check("judge: 不同网段、另一张卡还在 A_S 网段 → 这张卡接在另一张网上", svc_id_judge(&x) == SVC_JUDGE_OTHER_LAN);
    x = b; x.a = IP4(192, 168, 10, 50);
    check("judge: 自己的身份拿到同网段别的地址 → 租约丢过，采用", svc_id_judge(&x) == SVC_JUDGE_ADOPT);
    x = b; x.a = OWN; x.swapped = true; x.own_ip = OWN;
    check("judge: 交换着拿到的正是这张卡自己的地址 → 路由器只认 MAC", svc_id_judge(&x) == SVC_JUDGE_MAC_ONLY);
    x = b; x.a = OWN; x.swapped = true; x.own_ip = OWN; x.mode = SVC_IDMODE_CID;
    check("judge: 早已证实认 client-id、现在却拿到自己的地址（换了路由器）→ 照样判只认 MAC", svc_id_judge(&x) == SVC_JUDGE_MAC_ONLY);
    x = b; x.a = IP4(192, 168, 10, 77); x.swapped = true; x.own_ip = OWN;
    check("judge: 交换着拿到既不是 A_S 也不是自己的地址 → 不采用（可能是按 MAC 的保留），本次退回",
          svc_id_judge(&x) == SVC_JUDGE_INCONCLUSIVE);
    x = b; x.a = OWN; x.swapped = true; x.own_ip = OWN; x.declined = true;
    check("judge: 这一轮发过 DECLINE → 判不了（本次退回，不记）", svc_id_judge(&x) == SVC_JUDGE_INCONCLUSIVE);
    x = b; x.a = IP4(192, 168, 10, 77); x.swapped = true;
    check("judge: 不知道这张卡自己的地址、mode 未知 → 判不了", svc_id_judge(&x) == SVC_JUDGE_INCONCLUSIVE);
    x = b; x.a = IP4(192, 168, 10, 77); x.swapped = true; x.mode = SVC_IDMODE_CID;
    check("judge: 早已证实认 client-id、交换着拿到别的地址 → 照样不采用（之后才加的 MAC 保留）",
          svc_id_judge(&x) == SVC_JUDGE_INCONCLUSIVE);
    check("judge: NULL", svc_id_judge(NULL) == SVC_JUDGE_NONE);
}

static void test_id_wifi_can_hold(void)
{
    const uint32_t AS = IP4(192, 168, 10, 34);
    check("wifi_hold: 没凭据 → 不行", !svc_id_wifi_can_hold(false, AS, IP4(192, 168, 10, 36), M24, 0, 0));
    check("wifi_hold: 还不知道 A_S → 行", svc_id_wifi_can_hold(true, 0, IP4(10, 0, 0, 5), M24, 0, 0));
    check("wifi_hold: WiFi 还没地址 → 行（拿到后再判）", svc_id_wifi_can_hold(true, AS, 0, 0, IP4(192, 168, 10, 34), M24));
    check("wifi_hold: WiFi 在 A_S 网段 → 行", svc_id_wifi_can_hold(true, AS, IP4(192, 168, 10, 36), M24, 0, 0));
    check("wifi_hold: 两张网（网口在 A_S 网段、WiFi 在别处）→ 不行",
          !svc_id_wifi_can_hold(true, AS, IP4(192, 168, 1, 20), M24, IP4(192, 168, 10, 34), M24));
    check("wifi_hold: 网口不在、WiFi 在别的网段（换了网络）→ 行",
          svc_id_wifi_can_hold(true, AS, IP4(192, 168, 1, 20), M24, 0, 0));
    check("wifi_hold: 网口也在别的网段（整张网换了）→ 行",
          svc_id_wifi_can_hold(true, AS, IP4(192, 168, 1, 20), M24, IP4(192, 168, 1, 21), M24));
}

static void test_id_decide_s(void)
{
    check("decide_s: 网口持有、网口拿到 → S = 网口", svc_id_decide_s(SVC_NIC_ETH, SVC_NIC_ETH) == SVC_NIC_ETH);
    check("decide_s: WiFi 持有、WiFi 拿到 → S = WiFi", svc_id_decide_s(SVC_NIC_WIFI, SVC_NIC_WIFI) == SVC_NIC_WIFI);
    check("decide_s: 开机待定时 WiFi 先拿到 → 不定（等宽限）", svc_id_decide_s(SVC_NIC_NONE, SVC_NIC_WIFI) == SVC_NIC_NONE);
    check("decide_s: 网口持有时 WiFi 先拿到 → 不定（等网口）", svc_id_decide_s(SVC_NIC_ETH, SVC_NIC_WIFI) == SVC_NIC_NONE);
    check("names: 稳定短名", !strcmp(svc_nic_name(SVC_NIC_ETH), "eth") && !strcmp(svc_nic_name(SVC_NIC_WIFI), "wifi") &&
          !strcmp(svc_id_mode_name(SVC_IDMODE_CID), "client_id") && !strcmp(svc_id_mode_name(SVC_IDMODE_MAC_ONLY), "mac_only"));
}

int main(void)
{
    test_ip_sane();
    test_subnet();
    test_ssid_hash();
    test_rec_valid();
    test_decide();
    test_persist();
    test_gate();
    test_mdns();
    test_id_rec();
    test_id_rec_load();
    test_id_mac_only_age();
    test_id_degrade();
    test_id_cid();
    test_id_holder();
    test_id_judge();
    test_id_wifi_can_hold();
    test_id_decide_s();
    printf("\nResult: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
