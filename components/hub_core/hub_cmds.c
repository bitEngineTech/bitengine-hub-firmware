/* hub_cmds —— 命令处理函数（见 include/hub_cmds.h）。只在直连型里编，在调用方的任务里同步执行（M3 的读命令都只读缓存
 * 或持 OT 锁拷一份快照，毫秒级；控制类命令的异步作业层随 6.6）。
 *
 * H1（2026-10-22 对照 IDF v6.0.2 的 openthread/include）：
 *   - OT 的 API 一律在 esp_openthread_lock_acquire() 里调、拷进局部 / PSRAM 快照后马上放锁（7.1：持锁 < 20 ms）；
 *   - otThreadGetChildInfoByIndex(inst, i, &info)：i < otThreadGetMaxAllowedChildren()，空槽返回 OT_ERROR_NOT_FOUND；
 *   - hub.devices.list / hub.device.get 的 Thread 成员来自 thread_inventory（第七节；关联、id、link_margin 的算法在 hub_ti_core.c）；
 *   - otThreadGetRouterInfo() 对不存在的 router id 返回 OT_ERROR_NOT_FOUND，router id 从 0 到 otThreadGetMaxRouterId()（含）；
 *   - otBorderRoutingGetOmrPrefix()：边界路由没初始化时返回 OT_ERROR_INVALID_STATE。
 * 安全：不输出任何网络密钥（附录 A hub.thread.network 的说明里承诺了）；不输出局域网 IP（出站数据清单里没有，N5）。 */
#include "hub_cmds.h"
#include "hub_matter.h"
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
#include "matter_ctrl.h"
#include "matter_ctrl_live.h"   /* MATTER_CTRL_ACT_*（hub.device.control 的 Matter 列，M7 第六步） */
#endif

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hub_events.h"
#include "hub_meta.h"
#include "hub_params.h"
#include "hub_sanitize.h"
#include "hub_ti_core.h"
#include "hub_zigbee.h"
#include "network_mgr.h"
#include "openthread/border_routing.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"
#include "thread_br.h"
#include "thread_inventory.h"
#if CONFIG_BITENGINE_ENABLE_ZHUB
#include "watchdog.h"       /* MG24_HEARTBEAT_TIMEOUT_S：与看门狗判 MG24 死用同一个阈值 */
#include "zh_dev.h"         /* zh_ctl_act_t / zh_ctl_err_t / zh_ctl_result_t（控制，M5 第七步） */
#include "zh_host_core.h"   /* zh_pj_result_t */
#include "zhub.h"           /* device.health 的 zigbee：芯片有没有应答、网络起没起来（M4 第四步起 zhub 在跑）；入网窗口 */
#endif

static const char *TAG = "hub_cmds";

#define OT_LOCK_TICKS   pdMS_TO_TICKS(500)   /* 7.1 */

static int fail(hub_call_t *call, const char *code, const char *msg)
{
    call->err_code = code;
    snprintf(call->err_msg, sizeof(call->err_msg), "%s", msg);
    return -1;
}

static const char *role_str(otDeviceRole r)
{
    switch (r) {
    case OT_DEVICE_ROLE_DISABLED: return "disabled";
    case OT_DEVICE_ROLE_DETACHED: return "detached";
    case OT_DEVICE_ROLE_CHILD:    return "child";
    case OT_DEVICE_ROLE_ROUTER:   return "router";
    case OT_DEVICE_ROLE_LEADER:   return "leader";
    default:                      return "unknown";
    }
}

static bool net_online(void)
{
    const net_state_t s = network_mgr_get_state();
    return s == NET_STATE_ETH_ONLINE || s == NET_STATE_WIFI_ONLINE || s == NET_STATE_BOTH_ONLINE;
}

static const char *uplink_str(void)
{
    network_status_t st;
    if (network_mgr_get_status(&st) != ESP_OK || !net_online()) return "none";
    return st.active_interface == NET_IF_ETH ? "ethernet" : "wifi";
}

/* ---------------- Thread 快照 ---------------- */

typedef struct {
    bool          running;
    otDeviceRole  role;
    char          name[OT_NETWORK_NAME_MAX_SIZE + 1];
    uint8_t       channel;
    uint16_t      panid;
    uint16_t      rloc16;
    uint16_t      routers;
    bool          omr_ok;
    char          omr[OT_IP6_PREFIX_STRING_SIZE];
    uint16_t      n_children;
} ot_snap_t;

/* 本 BR 的网络概况（device.health、hub.thread.network）；成员清单在 thread_inventory */
static bool ot_snapshot(ot_snap_t *s)
{
    memset(s, 0, sizeof(*s));
    s->running = thread_br_is_running();
    if (!s->running) return true;
    if (!esp_openthread_lock_acquire(OT_LOCK_TICKS)) return false;
    otInstance *inst = esp_openthread_get_instance();
    if (!inst) {
        esp_openthread_lock_release();
        s->running = false;
        return true;
    }
    s->role = otThreadGetDeviceRole(inst);
    const char *nn = otThreadGetNetworkName(inst);
    snprintf(s->name, sizeof(s->name), "%s", nn ? nn : "");
    s->channel = otLinkGetChannel(inst);
    s->panid = otLinkGetPanId(inst);
    s->rloc16 = otThreadGetRloc16(inst);
    const uint8_t max_rid = otThreadGetMaxRouterId(inst);
    for (uint16_t rid = 0; rid <= max_rid; rid++) {
        otRouterInfo ri;
        if (otThreadGetRouterInfo(inst, rid, &ri) == OT_ERROR_NONE) s->routers++;
    }
    otIp6Prefix pfx;
    if (otBorderRoutingGetOmrPrefix(inst, &pfx) == OT_ERROR_NONE) {
        otIp6PrefixToString(&pfx, s->omr, sizeof(s->omr));
        s->omr_ok = true;
    }
    const uint16_t max_c = otThreadGetMaxAllowedChildren(inst);
    for (uint16_t i = 0; i < max_c; i++) {
        otChildInfo ci;
        if (otThreadGetChildInfoByIndex(inst, i, &ci) == OT_ERROR_NONE) s->n_children++;
    }
    esp_openthread_lock_release();
    return true;
}

/* ---------------- device.health ---------------- */

#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
typedef struct {
    const hub_mt_own_t *own;
    unsigned long       n;
} fab_cnt_t;

static void fab_cnt_cb(const ti_raw_t *raw, const ti_list_t *l, void *arg)
{
    fab_cnt_t *c = arg;
    for (unsigned long i = 0; l && i < l->n; i++)
        if (hub_matter_own_has(c->own, raw, &l->e[i], NULL)) c->n++;   /* 同列表：按 mt- 列了的那些 */
}
#endif

#if CONFIG_BITENGINE_ENABLE_ZHUB
/* zhub 快照 → device.health 的 zigbee.radio（固定英文词表，Muse 原样转述）。"有没有应答"与看门狗判 MG24 死用同一个阈值，
   不另写一个数（复核：原先写死 60 s，与看门狗的判据各说各话） */
static const char *zigbee_radio_str(const zhub_status_t *z)
{
    if (!z->started) return "not_running";                   /* 直连型里 zhub 没起来（启动失败）= 故障，不是"不用" */
    if (z->suspended || !z->holding_uart) return "busy";      /* UART 借给了 MG24 升级 / 基准测试 / bootloader */
    if (z->failing) return "not_answering";                  /* 连续失败到 ZH_HOST_FAILING_AT 次 */
    if (z->evidence_age_ms > MG24_HEARTBEAT_TIMEOUT_S * 1000u) return "not_answering";   /* 看门狗此时也判 MG24 死了 */
    if (z->have_evidence) return "answering";
    return "starting";                                       /* 刚接管、还没有第一次应答 */
}
#endif

static int cmd_device_health(hub_call_t *call)
{
    ot_snap_t ts;
    const bool ot_ok = ot_snapshot(&ts);
    const esp_app_desc_t *d = esp_app_get_description();
    hj_t *w = call->out;
    hj_obj_begin(w);
    hj_kstr(w, "firmware", d ? d->version : "unknown");
    hj_kstr(w, "profile", "direct");
    hj_kuint(w, "uptime_s", (uint64_t)(esp_timer_get_time() / 1000000));
    hj_key(w, "network");
    hj_obj_begin(w);
    hj_kbool(w, "online", net_online());
    hj_kstr(w, "uplink", uplink_str());
    hj_obj_end(w);
    hj_key(w, "zigbee");
    hj_obj_begin(w);
#if CONFIG_BITENGINE_ENABLE_ZHUB
    /* 附录 A 的说明是"whether the Zigbee radio answers"：读 zhub 的快照（2026-10-22 上板：原先写死 not_in_use，Muse 据此说"Zigbee 还不工作"）。
       只报状态词，不报信道 / PAN（第十三节的出站清单里 device.health 只有网关运行状态） */
    zhub_status_t z;
    zhub_get_status(&z);
    hj_kstr(w, "radio", zigbee_radio_str(&z));
    hj_kstr(w, "network", z.started && z.net ? z.net : "unknown");
#else
    hj_kstr(w, "radio", "not_in_use");
#endif
    hj_obj_end(w);
    hj_key(w, "thread");
    hj_obj_begin(w);
    hj_kbool(w, "running", ts.running);
    if (ts.running && ot_ok) {
        hj_kstr(w, "role", role_str(ts.role));
        hj_kuint(w, "routers", ts.routers);
        hj_kuint(w, "children", ts.n_children);
    } else if (ts.running) {
        hj_kstr(w, "role", "busy");               /* OT 锁 500 ms 没拿到 */
    }
    hj_obj_end(w);
    hj_key(w, "devices");
    hj_obj_begin(w);
    thread_inventory_stats_t tis;                 /* 网络里的 Thread 成员（hub.devices.list 列的那些；上一轮清单，不现刷） */
    thread_inventory_get_stats(&tis);
    unsigned long thn = ts.running && ot_ok ? tis.entries : 0;   /* Thread 成员 */
    bool partial = false;
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
    /* 本网关配好的 Matter 设备在列表里是 mt-、不再按 th- 列（7.3）：Thread 成员里扣掉它们（只从 Thread 那一份里扣，审计），
       再按节点表数（不在 Thread 上的也算）。同一份上一轮快照（max_age 0 = 不现刷），与上面的 tis.entries 一致 */
    hub_mt_own_t *own = thn ? hub_matter_own_get() : NULL;
    fab_cnt_t fc = { own, 0 };
    if (own && thread_inventory_with(0, fab_cnt_cb, &fc) == ESP_OK) thn -= fc.n < thn ? fc.n : thn;
    hub_matter_own_free(own);
    const int mn = hub_matter_count();
    partial = mn < 0;
#endif
    unsigned long total = thn;
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
    if (mn > 0) total += (unsigned long)mn;
#endif
#if CONFIG_BITENGINE_ENABLE_ZHUB
    /* 列表里的 Zigbee 设备也算（附录 A："how many devices it manages"；第二轮审计 ❌：原先只数 Thread）。设备表这会儿拿不到就只数 Thread */
    const long zn = hub_zigbee_count();
    if (zn > 0) total += (unsigned long)zn;
    partial = partial || zn < 0;                  /* Zigbee 设备表这会儿拿不到：total 只数了别的，明说（"忙"不等于"没有"） */
#endif
    hj_kuint(w, "total", total);
    if (partial) hj_kbool(w, "partial", true);
    hj_obj_end(w);
    hj_key(w, "memory");
    hj_obj_begin(w);
    hj_kuint(w, "internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    hj_kuint(w, "internal_min_free", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    hj_kuint(w, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    hj_obj_end(w);
    hj_obj_end(w);
    return 0;
}

/* ---------------- hub.thread.network ---------------- */

static int cmd_thread_network(hub_call_t *call)
{
    ot_snap_t ts;
    if (!ot_snapshot(&ts)) return fail(call, "unavailable", "the Thread stack is busy, try again in a moment");
    if (!ts.running) return fail(call, "unavailable", "the hub's Thread network is not running");
    char pan[8];
    snprintf(pan, sizeof(pan), "0x%04x", ts.panid);
    char name[OT_NETWORK_NAME_MAX_SIZE + 1];       /* 网络名能从外部设置（数据集）：过一遍设备字符串卫生（6.8）再给模型看 */
    hub_sanitize_device_str(ts.name, strlen(ts.name), name, sizeof(name), sizeof(name) - 1);
    hj_t *w = call->out;
    hj_obj_begin(w);
    hj_kstr(w, "network_name", name);
    hj_kuint(w, "channel", ts.channel);
    hj_kstr(w, "pan_id", pan);
    hj_kstr(w, "role", role_str(ts.role));
    hj_kuint(w, "routers", ts.routers);
    hj_kuint(w, "children", ts.n_children);
    if (ts.omr_ok) hj_kstr(w, "omr_prefix", ts.omr);
    hj_obj_end(w);
    return 0;
}

/* ---------------- hub.devices.list / hub.device.get（M3：Thread 网络层清单 thread_inventory，第七节） ---------------- */

static const char *param_str(const hub_call_t *call, const char *key)
{
    const cJSON *v = call->params ? cJSON_GetObjectItemCaseSensitive(call->params, key) : NULL;
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : NULL;
}

/* 清单快照最多旧多少就先刷新一次（thread_inventory 平时 60 s 一轮；agent 问到时给新一点的） */
#define TI_FRESH_MS 15000

#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
#define MATTER_ON true
#else
#define MATTER_ON false
#endif

#define LIST_RC_BUSY (-2)                         /* Zigbee 设备表拿不到（list_cb 填，调用方回 unavailable） */

typedef struct {
    hub_call_t   *call;
    bool          want_thread;
    bool          want_zigbee;
    bool          want_matter;                    /* 本网关 fabric 上的 Matter 设备（mt-，M7 第四步）：有能力、有房间 */
    uint32_t      cap;                            /* capability 过滤（HUB_CAP_*；0 = 不过滤）：Zigbee 设备有能力（M5 第一步），Thread 成员没有 */
    const char   *room;                           /* room 过滤（已净化；NULL = 不过滤）：Zigbee 设备有房间（M5 第二步），Thread 成员没有 */
    unsigned long start;
    int           rc;
} list_ctx_t;

static void list_cb(const ti_raw_t *raw, const ti_list_t *l, void *arg)
{
    list_ctx_t *c = arg;
    hj_t *w = c->call->out;
    hj_set_reserve(w, 48);                        /* ,"truncated":true,"next_cursor":"NNNNNN" */
    hj_obj_begin(w);
    hj_key(w, "devices");
    hj_arr_begin(w);
    bool truncated = false;
    unsigned long next = 0;
    /* 顺序：先 Zigbee 设备（按设备表下标），再 Thread 成员；cursor 是合在一起的序号 */
    unsigned long nz = 0;
#if CONFIG_BITENGINE_ENABLE_ZHUB
    if (c->want_zigbee) {
        const int zr = hub_zigbee_write_list(w, c->start, &next, &nz, c->cap, c->room);
        if (zr == HUB_ZB_BUSY) {                   /* 设备表这会儿拿不到：回"忙"，别交一个缺了 Zigbee 的列表（游标也会错位） */
            c->rc = LIST_RC_BUSY;
            return;
        }
        truncated = zr == HUB_ZB_TRUNCATED;
    }
#endif
    /* 再 Matter 设备（按节点表顺序；Thread 信息从这份清单里按 fabric + 节点号找来挂上，7.3） */
    unsigned long nm = 0;
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
    if (c->want_matter && !truncated) {
        const unsigned long ms = c->start > nz ? c->start - nz : 0;
        unsigned long mnext = 0;
        const int mr = hub_matter_write_list(w, ms, &mnext, &nm, c->cap, c->room, raw, l);
        if (mr == HUB_MT_BUSY) {
            c->rc = LIST_RC_BUSY;
            return;
        }
        if (mr == HUB_MT_TRUNCATED) {
            truncated = true;
            next = nz + mnext;
        }
    }
#endif
    const unsigned long t0 = c->start > nz + nm ? c->start - nz - nm : 0;
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
    hub_mt_own_t *own = c->want_thread && l ? hub_matter_own_get() : NULL;   /* 拿不到（控制器没在跑）：一个都不跳，重复好过消失 */
#endif
    for (unsigned long i = t0; !truncated && c->want_thread && l && i < l->n; i++) {
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
        if (hub_matter_own_has(own, raw, &l->e[i], NULL)) continue;   /* 本网关配好的 Matter 设备：已按 mt- 列过（序号照占，游标不乱） */
#endif
        hj_mark_t m = hj_mark(w);
        ti_write_entry(w, raw, &l->e[i], false);
        if (w->truncated) {
            hj_rollback(w, m);
            truncated = true;
            next = nz + nm + i;
            break;
        }
    }
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
    hub_matter_own_free(own);
#endif
    hj_arr_end(w);
    hj_set_reserve(w, 0);
    if (truncated) {
        char cur[12];
        snprintf(cur, sizeof(cur), "%lu", next);
        hj_kbool(w, "truncated", true);
        hj_kstr(w, "next_cursor", cur);
    }
    hj_obj_end(w);
    c->rc = 0;
}

static int cmd_devices_list(hub_call_t *call)
{
    const char *proto = param_str(call, "protocol");
    const char *room = param_str(call, "room");
    const char *cap = param_str(call, "capability");
    const char *cursor = param_str(call, "cursor");
    if (proto && strcmp(proto, "all") && strcmp(proto, "thread") && strcmp(proto, "zigbee") && strcmp(proto, "matter")) {
        return fail(call, "invalid_params", "protocol must be zigbee, matter, thread or all");
    }
    if (cap && !hub_cap_from_name(cap)) return fail(call, "invalid_params", "unknown capability (see the command description for the list)");
    unsigned long start = 0;
    if (cursor && *cursor) {
        char *end = NULL;
        start = strtoul(cursor, &end, 10);
        if (!end || *end || start > 100000) return fail(call, "invalid_params", "cursor must be the next_cursor value from the previous call");
    }
    /* 房间：Zigbee 设备按 hub.device.update 设的房间过滤（M5 第二步）；Thread 成员没有房间（带了 room 就不列）。过滤值按用户名规则净化
       （与存的时候同一套：首尾空白、连续空白、格式字符），空串当作没给。能力：Thread 成员没有（带了 capability 就不列）；
       Zigbee 设备按访谈推出的能力过滤（hub_zcap，M5 第一步）；Matter 设备按访谈结果（hub_mcap，M7 第四步） */
    char room_c[HUB_ROOM_MAX + 1] = "";
    if (room) {
        const hub_name_verdict_t rv = hub_sanitize_user_name(room, room_c, sizeof(room_c), HUB_ROOM_MAX);
        if (rv == HUB_NAME_NEWLINE) return fail(call, "invalid_params", "room must be a single line");
        if (rv != HUB_NAME_OK) return fail(call, "invalid_params", "room is at most 24 bytes in UTF-8");
        if (!room_c[0]) room = NULL;
    }
    const bool all = !proto || !strcmp(proto, "all");
    list_ctx_t c = { call, (all || !strcmp(proto, "thread")) && !room && !cap, (all || !strcmp(proto, "zigbee")),
                     MATTER_ON && (all || !strcmp(proto, "matter")),
                     cap ? hub_cap_from_name(cap) : 0, room ? room_c : NULL, start, -1 };
    /* Thread 成员要清单（先刷到 15 s 内）；Matter 设备只是从清单里取 Thread 信息（上一轮的就行、不现刷）；只要 Zigbee 不碰清单。
       不要 Thread 成员时清单拿不到（锁忙）照样回：Matter 条目只是不挂 thread（审计：别让房间 / 能力过滤被清单锁拖成 unavailable） */
    esp_err_t ie = ESP_ERR_NOT_FOUND;
    if (c.want_thread || c.want_matter) ie = thread_inventory_with(c.want_thread ? TI_FRESH_MS : 0, list_cb, &c);
    if (ie != ESP_OK && !c.want_thread) list_cb(NULL, NULL, &c);
    if ((ie != ESP_OK && c.want_thread) || c.rc != 0) return fail(call, "unavailable", "the device list is busy, try again in a moment");
    return 0;
}

typedef struct {
    hub_call_t *call;
    const char *id;
    bool        found;
    bool        moved;                            /* 这个 th- 是本网关 fabric 上的 Matter 设备：按 mt[] 查 */
    char        mt[20];
} get_ctx_t;

static void get_cb(const ti_raw_t *raw, const ti_list_t *l, void *arg)
{
    get_ctx_t *c = arg;
    const ti_entry_t *e = ti_find(l, c->id);
    if (!e) return;
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
    hub_mt_own_t *own = hub_matter_own_get();
    c->moved = hub_matter_own_has(own, raw, e, c->mt);
    hub_matter_own_free(own);
    if (c->moved) return;          /* 本网关配好的 Matter 设备（7.3）：它在列表里是 mt-，这个 th- 只是它的 Thread 网络层；指过去，别给两份说法 */
#endif
    ti_write_entry(c->call->out, raw, e, true);
    c->found = true;
}

#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
typedef struct {
    hub_call_t *call;
    const char *id;
    int         rc;
} mget_ctx_t;

static void mget_cb(const ti_raw_t *raw, const ti_list_t *l, void *arg)
{
    mget_ctx_t *c = arg;
    c->rc = hub_matter_write_device(c->call->out, c->id, raw, l);
}
#endif

static int cmd_device_get(hub_call_t *call)
{
    const char *id = param_str(call, "id");
    if (!id) return fail(call, "invalid_params", "id is required");
#if CONFIG_BITENGINE_ENABLE_ZHUB
    if (strncasecmp(id, "zb-", 3) == 0) {
        const int zr = hub_zigbee_write_device(call->out, id);
        if (zr == HUB_ZB_BUSY) return fail(call, "unavailable", "the device list is busy, try again in a moment");
        if (zr != HUB_ZB_DONE) return fail(call, "not_found", "no device with that id on this hub (ids come from hub.devices.list)");
        return 0;
    }
#endif
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
    if (strncasecmp(id, "mt-", 3) == 0) {
        mget_ctx_t mc = { call, id, HUB_MT_NOT_FOUND };
        if (thread_inventory_with(TI_FRESH_MS, mget_cb, &mc) != ESP_OK) mget_cb(NULL, NULL, &mc);   /* 清单拿不到：照样回，只是不挂 Thread 信息 */
        if (mc.rc == HUB_MT_BUSY) return fail(call, "unavailable", "the device list is busy, try again in a moment");
        if (mc.rc != HUB_MT_DONE) return fail(call, "not_found", "no device with that id on this hub (ids come from hub.devices.list)");
        return 0;
    }
#endif
    if (strncasecmp(id, "th-", 3) != 0 || strlen(id) != 19) {
        return fail(call, "not_found", "no device with that id on this hub (ids come from hub.devices.list)");
    }
    get_ctx_t c = { call, id, false, false, "" };
    if (thread_inventory_with(TI_FRESH_MS, get_cb, &c) != ESP_OK) {
        return fail(call, "unavailable", "the device list is busy, try again in a moment");
    }
    if (c.moved) {
        char msg[120];
        snprintf(msg, sizeof(msg), "this Thread member is the hub's own Matter device %s; use that id", c.mt);
        return fail(call, "not_found", msg);
    }
    if (!c.found) return fail(call, "not_found", "no device with that id on this hub (it may have left the network)");
    return 0;
}

/* ---------------- hub.events.recent（6.7 最近事件环） ---------------- */

/* 可选的整数参数：没给用默认；给了要是 [lo, hi] 里的整数（mp_check_params 只验过类型是 number） */
static bool param_uint(const hub_call_t *call, const char *key, uint32_t lo, uint32_t hi, uint32_t dflt, uint32_t *out)
{
    const cJSON *v = call->params ? cJSON_GetObjectItemCaseSensitive(call->params, key) : NULL;
    if (!v) {
        *out = dflt;
        return true;
    }
    if (!cJSON_IsNumber(v)) return false;
    const double d = v->valuedouble;
    if (d < (double)lo || d > (double)hi || d != (double)(uint32_t)d) return false;
    *out = (uint32_t)d;
    return true;
}

static int cmd_events_recent(hub_call_t *call)
{
    uint32_t limit = 20, since = 0;
    if (!param_uint(call, "limit", 1, 50, 20, &limit)) return fail(call, "invalid_params", "limit must be an integer from 1 to 50");
    /* since_s：0 或不给 = 不限时间；超过 7 天按 7 天（环只有 64 条，更早的本来也不在了），附录 A 没写上限，别拒收 */
    if (!param_uint(call, "since_s", 0, UINT32_MAX, 0, &since)) return fail(call, "invalid_params", "since_s must be a non-negative integer");
    if (since > 7u * 86400u) since = 7u * 86400u;
    if (!hub_events_write_recent(call->out, limit, since)) {
        return fail(call, "unavailable", "the hub's event log is busy, try again in a moment");   /* 别回"空列表"：会被当成什么都没发生 */
    }
    return 0;
}

#if CONFIG_BITENGINE_ENABLE_ZHUB
/* hub.zigbee.permit_join（附录 A：seconds 30–254，默认 120，0 = 关窗）。受理由 zhub 任务说了算（zhub_permit_join 投过去，最坏约 1 s），
   之后再等窗口真开好 / 真关好（每 50 ms 看一次快照，最多 PJ_SETTLE_MS；连受理最坏约 2 s，httpd / Muse 会话任务里能接受）：只在 NCP 那边窗口开了才回 open:true（第二轮审计：原先"正在开"就回开了，
   开失败 Muse 也会说开好了）。窗口开着时不重开、不续时（6.5 注记），回剩余秒数。
   ⚠️ 这一步（M4 第六步）只经本地 REST 用：Muse 那边要等入网的设备能被报上去（第七步，Z22）才注册，见 CHECKLIST Z21 */
#define PJ_SETTLE_MS 1000

static bool win_is(const zhub_status_t *z, const char *name) { return z->win_state && strcmp(z->win_state, name) == 0; }

static int cmd_zigbee_permit_join(hub_call_t *call)
{
    uint32_t secs = 120;
    if (!param_uint(call, "seconds", 0, HUB_PJ_MAX_S, 120, &secs) || (secs != 0 && secs < HUB_PJ_MIN_S)) {
        return fail(call, "invalid_params", "seconds must be 0 (close the window) or 30-254");
    }
    zhub_pj_t r;
    const esp_err_t e = zhub_permit_join(secs, &r);
    if (e == ESP_ERR_TIMEOUT) return fail(call, "busy", "the hub's Zigbee radio is busy, try again in a moment");
    if (e != ESP_OK) return fail(call, "unavailable", "the hub's Zigbee network is not running right now; try again in a minute");
    switch ((zh_pj_result_t)r.result) {
    case ZH_PJ_NOT_READY: return fail(call, "unavailable", "the hub's Zigbee network is not up right now; try again in a minute");
    case ZH_PJ_BUSY:      return fail(call, "busy", "the Zigbee join window is still closing; try again in a few seconds");
    case ZH_PJ_INVALID:   return fail(call, "invalid_params", "seconds must be 0 (close the window) or 30-254");
    case ZH_PJ_QUOTA: {                            /* 总时长上限（滚动一小时 10 分钟，zh_host_core.h） */
        char m[160];
        const unsigned wait_min = (unsigned)((r.retry_s + 59u) / 60u);
        snprintf(m, sizeof(m), "Zigbee pairing has already been open for its limit of %u minutes in the past hour; try again in about %u minutes",
                 (unsigned)(ZH_HOST_PERMIT_HOUR_CAP_S / 60u), wait_min ? wait_min : 1u);
        return fail(call, "rate_limited", m);
    }
    default:              break;
    }
    /* 等它落定：开的等到 open（或没开成回到 closing / closed），关的等到 closed。第一眼用受理时带回来的状态（任务也先刷了快照） */
    zhub_status_t z;
    zhub_get_status(&z);
    z.win_state = r.win_state;
    z.win_left_s = r.left_s;
    for (int waited = 0; waited < PJ_SETTLE_MS && (win_is(&z, "opening") || (secs == 0 && !win_is(&z, "closed"))); waited += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
        zhub_get_status(&z);
    }
    const bool open = win_is(&z, "open");
    if (secs != 0 && !open && !win_is(&z, "opening")) {
        return fail(call, "unavailable", "the hub could not open Zigbee pairing; try again in a minute");
    }
    hj_obj_begin(call->out);
    hj_kbool(call->out, "open", open);
    if (open) hj_kuint(call->out, "seconds_left", z.win_left_s);
    if (open && r.result == ZH_PJ_OK && r.seconds < secs) hj_kbool(call->out, "shortened", true);   /* 被总时长上限截短了 */
    if (r.result == ZH_PJ_ALREADY_OPEN) hj_kbool(call->out, "already_open", true);
    if (secs != 0 && !open) hj_kbool(call->out, "opening", true);            /* 1 s 里还没落定：多半是在途的命令慢，过一会儿看 */
    if (secs == 0 && !win_is(&z, "closed")) hj_kbool(call->out, "closing", true);   /* 1 s 里还没关完（写回策略在重试、或开的那一步还在途） */
    hj_kstr(call->out, "message", open ? (r.result == ZH_PJ_ALREADY_OPEN ? "Zigbee pairing is already open."
                                                                        : "Zigbee pairing is open; put the new device into pairing mode now.")
                                  : secs != 0 ? "Zigbee pairing is being opened; check again in a few seconds."
                                  : win_is(&z, "closed") ? "Zigbee pairing is closed." : "Zigbee pairing is closing.");
    hj_obj_end(call->out);
    return 0;
}
#endif

/* ---------------- hub.device.update（6.1 元数据、附录 A；M5 第二步） ---------------- */

/* 名字 / 房间 / 推送开关。只认这个网关管着的设备（Zigbee，与本网关 fabric 上的 Matter 设备 —— M7 第四步；Thread 成员不可管 —— 附录 A 的列表说明）。
   附录 A 的参数里没有敏感标记：Muse 改不了它（6.2），这里也不读那个键 */
static int cmd_device_update(hub_call_t *call)
{
    const char *id = param_str(call, "id");
    if (!id) return fail(call, "invalid_params", "id is required");
    const char *name = param_str(call, "name");
    const char *room = param_str(call, "room");
    const cJSON *nv = call->params ? cJSON_GetObjectItemCaseSensitive(call->params, "notify") : NULL;
    /* 按字段名初始化：没写的那一项（敏感标记）是 0 = 不改 —— Muse 改不了它（6.2），只有本地 /api/v1/hub/device/sensitive 设 */
    const hub_meta_change_t ch = { .name = name, .room = room, .notify = cJSON_IsBool(nv) ? (cJSON_IsTrue(nv) ? 1 : 0) : -1 };
    if (!ch.name && !ch.room && ch.notify < 0) return fail(call, "invalid_params", "give at least one of name, room or notify");
    bool known = false;
#if CONFIG_BITENGINE_ENABLE_ZHUB
    if (strncasecmp(id, "zb-", 3) == 0) {
        const int zr = hub_zigbee_write_device(NULL, id);
        if (zr == HUB_ZB_BUSY) return fail(call, "unavailable", "the device list is busy, try again in a moment");
        known = zr == HUB_ZB_DONE;
    }
#endif
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
    if (strncasecmp(id, "mt-", 3) == 0) {
        const int mr = hub_matter_write_device(NULL, id, NULL, NULL);   /* 只问在不在（配好了的才算） */
        if (mr == HUB_MT_BUSY) return fail(call, "unavailable", "the device list is busy, try again in a moment");
        known = mr == HUB_MT_DONE;
    }
#endif
    if (!known) {
        if (strncasecmp(id, "th-", 3) == 0) return fail(call, "invalid_params", "Thread network members are not managed by this hub and cannot be renamed");
        return fail(call, "not_found", "no device with that id on this hub (ids come from hub.devices.list)");
    }
    hub_meta_ent_t e;
    switch (hub_meta_update(id, &ch, &e)) {
    case HUB_META_OK:           break;
    case HUB_META_NAME_NEWLINE: return fail(call, "invalid_params", "name must be a single line");
    case HUB_META_NAME_LONG:    return fail(call, "invalid_params", "name is at most 32 bytes in UTF-8");
    case HUB_META_ROOM_NEWLINE: return fail(call, "invalid_params", "room must be a single line");
    case HUB_META_ROOM_LONG:    return fail(call, "invalid_params", "room is at most 24 bytes in UTF-8");
    case HUB_META_BAD_ID:       return fail(call, "not_found", "no device with that id on this hub (ids come from hub.devices.list)");
    case HUB_META_NOTHING:      return fail(call, "invalid_params", "give at least one of name, room or notify");
    case HUB_META_FULL:         return fail(call, "unavailable", "the hub has no room for more device settings; clear the name, room and notify setting of another device first "
                                                         "(or have the owner remove its sensitive mark on the hub's web page)");
    case HUB_META_STORE:        return fail(call, "unavailable", "the hub could not read its saved device names when it started; restart the hub, then try again");
    case HUB_META_BUSY:
    default:                    return fail(call, "unavailable", "the hub could not save the change right now; try again in a minute");
    }
    /* 回读：存进去的样子（名字 / 房间是净化后的；id 按列表里的写法：小写） */
    char cid[24];
    snprintf(cid, sizeof(cid), "%s-%02x%02x%02x%02x%02x%02x%02x%02x", e.proto == HUB_META_PROTO_MATTER ? "mt" : "zb",
             e.id8[0], e.id8[1], e.id8[2], e.id8[3], e.id8[4], e.id8[5], e.id8[6], e.id8[7]);
    /* notify 打开了、可"门窗与人体"这一类推送在网页上关着（推不推 = 类别位 AND 设备位，6.7）：照实说，别让 agent 答应了"开门告诉你"却永远收不到（C17 第二轮审计） */
    const bool notify = (e.flags & HUB_META_F_NOTIFY) != 0;
    const bool cls_on = (hub_events_get_notify() & (1u << HUB_EVC_SECURITY)) != 0;
    hj_obj_begin(call->out);
    hj_kstr(call->out, "id", cid);
    hj_kstr(call->out, "name", e.name);
    hj_kstr(call->out, "room", e.room);
    hj_kbool(call->out, "notify", notify);
    hj_kbool(call->out, "notify_active", notify && cls_on);
    if (notify && !cls_on) {
        hj_kstr(call->out, "message", "Door, window and motion notifications are turned off on the hub's web page, so door, window and "
                                      "motion changes from this device will not be reported until the owner turns them back on. Alarms "
                                      "are still reported.");
    }
    hj_obj_end(call->out);
    return 0;
}

/* ---------------- hub.device.control（8.11 控制路径、6.2 动作表、6.5 执行时的检查；M5 第七步、M7 第六步） ---------------- */

#if CONFIG_BITENGINE_ENABLE_ZHUB || CONFIG_BITENGINE_ENABLE_MATTER_CTRL
/* 第 3 道检查（hub_policy_check_action：Muse 看权限位与敏感、两个出口都看能力）的结论 → 回给调用方的错误。0 = 放行 */
static int check_action(hub_call_t *call, const hub_action_info_t *a, bool sensitive, uint32_t caps)
{
    switch (hub_policy_check_action(a, call->perm, call->src, sensitive, caps)) {
    case HUB_CHK_OK:        return 0;
    case HUB_CHK_NO_PERM:   return fail(call, "forbidden", "the hub's owner has not allowed controlling devices");
    case HUB_CHK_SENSITIVE:                        /* 分清是哪种敏感：agent 照着说才不会去猜"权限开关关了 / 固件收紧了"（2026-10-23 上板） */
        if (a->sens != HUB_SENS_ALWAYS && sensitive)
            return fail(call, "forbidden", "the hub's owner marked this device as sensitive, and controlling sensitive devices is not allowed for you; "
                                           "tell the user it was refused for that reason instead of retrying");
        return fail(call, "forbidden", "this is a security-sensitive action that the hub's owner has not allowed; tell the user instead of retrying");
    case HUB_CHK_NO_CAP:    return fail(call, "invalid_params", "this device cannot do that action (check its capabilities in hub.devices.list)");
    default:                return fail(call, "internal", "could not check the action");
    }
}
#endif

#if CONFIG_BITENGINE_ENABLE_ZHUB
static int zb_control(hub_call_t *call, const hub_action_info_t *a, int32_t v, const char *id)
{
    uint8_t ieee[8];
    uint32_t caps = 0, ctl_caps = 0;
    bool rx_on = false, sensitive = false;
    const int zr = hub_zigbee_ctl_info(id, ieee, &caps, &ctl_caps, &rx_on, &sensitive);
    if (zr == HUB_ZB_BUSY) return fail(call, "unavailable", "the device list is busy, try again in a moment");
    if (zr != HUB_ZB_DONE) return fail(call, "not_found", "no device with that id on this hub (ids come from hub.devices.list)");
    if (check_action(call, a, sensitive, caps) != 0) return -1;
    zh_ctl_act_t act;
    uint8_t lvl = 0;
    switch (a->act) {
    case HUB_ACT_TURN_ON:        act = ZH_CTL_ON; break;
    case HUB_ACT_TURN_OFF:       act = ZH_CTL_OFF; break;
    case HUB_ACT_TOGGLE:         act = ZH_CTL_TOGGLE; break;
    case HUB_ACT_SET_BRIGHTNESS: act = ZH_CTL_LEVEL; lvl = (uint8_t)v; break;
    default:                     return fail(call, "unsupported", "this action is not available on this hub's firmware yet");
    }
    /* 列表里有这个能力、控制路径还驱动不了（例如涂鸦表里只推出了读法、没推出写法的）：说是固件还不支持，别说"这台做不了"（审计 ❌） */
    if ((ctl_caps & a->cap) != a->cap) return fail(call, "unsupported", "this hub's firmware cannot control this device yet (it can only read its state); tell the user instead of retrying");
    (void)rx_on;                                   /* 休眠设备也能控制了：zhub 排进待发、回 pending（8.11 第 6 步，M5 第十二步） */
    zhub_ctl_t r;
    const esp_err_t e = zhub_control(ieee, (uint8_t)act, lvl, &r);
    if (e == ESP_ERR_TIMEOUT) return fail(call, "busy", "the hub's Zigbee radio is busy, try again in a moment");
    if (e == ESP_ERR_NOT_FINISHED) {               /* 可能已经发出去了：别让 agent 重发（切换会切两次） */
        return fail(call, "unavailable", "the hub sent the command but could not get the result; check the device state with hub.device.get instead of resending");
    }
    if (e != ESP_OK) return fail(call, "unavailable", "the hub's Zigbee network is not running right now; try again in a minute");
    switch ((zh_ctl_err_t)r.err) {
    case ZH_CTL_E_OK:         break;
    case ZH_CTL_E_BUSY:
    case ZH_CTL_E_SEND:       return fail(call, "busy", "the hub's Zigbee radio is busy, try again in a moment");
    case ZH_CTL_E_NET:        return fail(call, "unavailable", "the hub's Zigbee network is not up right now; try again in a minute");
    case ZH_CTL_E_LEFT:       return fail(call, "unavailable", "the device has left the network or its address is not known yet");
    case ZH_CTL_E_SLEEPY:     return fail(call, "unsupported", "battery-powered devices cannot be controlled by this hub yet");
    case ZH_CTL_E_NO_CLUSTER: return fail(call, "invalid_params", "this device cannot do that action (check its capabilities in hub.devices.list)");
    case ZH_CTL_E_STATE:      return fail(call, "unsupported", "this device has not reported whether it is on or off yet, so it cannot be toggled; use turn_on or turn_off");
    case ZH_CTL_E_NO_OFF:     return fail(call, "unsupported", "this device has no on/off switch, so brightness 0 cannot turn it off; set 1-100 instead");
    default:                  return fail(call, "not_found", "no device with that id on this hub (ids come from hub.devices.list)");
    }
    const char *res = r.result == ZH_CTL_R_CONFIRMED ? "confirmed" : r.result == ZH_CTL_R_DELIVERED ? "delivered" : r.result == ZH_CTL_R_PENDING ? "pending" : "failed";
    char detail[96];
    if (r.result == ZH_CTL_R_PENDING) snprintf(detail, sizeof(detail), "Battery device: queued until the device wakes; do not resend.");   /* 8.11 第 6 步的原话 */
    else if (r.result == ZH_CTL_R_CONFIRMED) snprintf(detail, sizeof(detail), "The device confirmed the command (%u ms).", (unsigned)r.ms);
    else if (r.result == ZH_CTL_R_DELIVERED) snprintf(detail, sizeof(detail), "The command reached the device, but it did not confirm the change.");
    else if (r.zcl_status) snprintf(detail, sizeof(detail), "The device refused the command (ZCL status 0x%02X).", (unsigned)r.zcl_status);
    else snprintf(detail, sizeof(detail), "The command did not reach the device; it may be unpowered or out of range.");
    hj_obj_begin(call->out);
    hj_kstr(call->out, "result", res);
    (void)hub_zigbee_write_state(call->out, id);   /* 当前缓存的读数（设备回了上报就是新的；没有就是命令之前的） */
    hj_kstr(call->out, "detail", detail);
    hj_obj_end(call->out);
    return 0;
}
#endif

#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
/* Matter（M7 第六步，9.5「命令」）：开 / 关 / 切换 / 亮度，matter_ctrl_control 同步等结论（≤ 约 13 s，在命令的 15 s 之内）。
   结论口径同 Zigbee：confirmed（设备回了成功、3 s 内订阅报回目标值）/ delivered（只回了成功）/ failed */
static int mt_control(hub_call_t *call, const hub_action_info_t *a, int32_t v, const char *id)
{
    uint64_t node = 0;
    uint32_t caps = 0, ctl_caps = 0;
    bool sensitive = false;
    const int mr = hub_matter_ctl_info(id, &node, &caps, &ctl_caps, &sensitive);
    if (mr == HUB_MT_BUSY) return fail(call, "unavailable", "the device list is busy, try again in a moment");
    if (mr != HUB_MT_DONE) return fail(call, "not_found", "no device with that id on this hub (ids come from hub.devices.list)");
    if (check_action(call, a, sensitive, caps) != 0) return -1;
    int act;
    switch (a->act) {
    case HUB_ACT_TURN_ON:        act = MATTER_CTRL_ACT_ON; break;
    case HUB_ACT_TURN_OFF:       act = MATTER_CTRL_ACT_OFF; break;
    case HUB_ACT_TOGGLE:         act = MATTER_CTRL_ACT_TOGGLE; break;
    case HUB_ACT_SET_BRIGHTNESS: act = MATTER_CTRL_ACT_LEVEL; break;
    default:                     return fail(call, "unsupported", "this action is not available on this hub's firmware yet");
    }
    if ((ctl_caps & a->cap) != a->cap) return fail(call, "unsupported", "this hub's firmware cannot control this device yet (it can only read its state); tell the user instead of retrying");
    matter_ctrl_ctl_t r;
    const esp_err_t e = matter_ctrl_control(node, act, act == MATTER_CTRL_ACT_LEVEL ? (uint8_t)v : 0, &r);
    if (e == ESP_ERR_TIMEOUT) return fail(call, "busy", "the hub is controlling another Matter device right now, try again in a moment");
    if (e != ESP_OK) return fail(call, "unavailable", "the hub's Matter controller is not running right now; try again in a minute");
    switch ((matter_ctrl_ctl_err_t)r.err) {
    case MATTER_CTRL_CTL_E_OK:          break;
    case MATTER_CTRL_CTL_E_NO_NODE:     return fail(call, "unavailable", "the hub has not finished identifying this device yet; try again in a minute");
    case MATTER_CTRL_CTL_E_NO_CLUSTER:
        if (act == MATTER_CTRL_ACT_LEVEL && v == 0) return fail(call, "unsupported", "this device has no on/off switch, so brightness 0 cannot turn it off; set 1-100 instead");
        return fail(call, "invalid_params", "this device cannot do that action (check its capabilities in hub.devices.list)");
    case MATTER_CTRL_CTL_E_UNREACHABLE: return fail(call, "unavailable", "the hub could not reach the device; it may be unpowered or out of range");
    case MATTER_CTRL_CTL_E_NO_ANSWER:   /* 可能已经执行：别让 agent 重发（切换会切两次） */
        return fail(call, "unavailable", "the hub sent the command but could not get the result; check the device state with hub.device.get instead of resending");
    case MATTER_CTRL_CTL_E_REFUSED:
    default:                            break;
    }
    const char *res = r.result == MATTER_CTRL_CTL_CONFIRMED ? "confirmed" : r.result == MATTER_CTRL_CTL_DELIVERED ? "delivered" : "failed";
    char detail[96];
    if (r.result == MATTER_CTRL_CTL_CONFIRMED) snprintf(detail, sizeof(detail), "The device confirmed the command (%u ms).", (unsigned)r.ms);
    else if (r.result == MATTER_CTRL_CTL_DELIVERED) snprintf(detail, sizeof(detail), "The command reached the device, but it did not confirm the change.");
    else snprintf(detail, sizeof(detail), "The device refused the command (Matter status 0x%02X).", (unsigned)r.im_status);
    hj_obj_begin(call->out);
    hj_kstr(call->out, "result", res);
    (void)hub_matter_write_state(call->out, id);   /* 当前的读数（订阅报回来的就是新的） */
    hj_kstr(call->out, "detail", detail);
    hj_obj_end(call->out);
    return 0;
}
#endif

#if CONFIG_BITENGINE_ENABLE_ZHUB || CONFIG_BITENGINE_ENABLE_MATTER_CTRL
/* 只开放开 / 关 / 切换 / 亮度（Zigbee 常电设备与休眠设备、Matter 设备）；色温 / 颜色 / 窗帘 / 门锁 / 温控 / 识别随后（附录 A 的说明照列全集，没做的回 unsupported）。
   顺序：动作认不认识 → 值 → 设备（有没有、能力、敏感）→ 第 3 道检查 → 发、同步等结论 → 回 confirmed / delivered / pending / failed 与当前读数 */
static int cmd_device_control(hub_call_t *call)
{
    const char *id = param_str(call, "id");
    const char *action = param_str(call, "action");
    const char *value = param_str(call, "value");
    if (!id || !action) return fail(call, "invalid_params", "id and action are required");
    const hub_action_info_t *a = hub_action_lookup(action);
    if (!a) return fail(call, "invalid_params", "unknown action (see the command description for the list)");
    int32_t v = 0;
    switch (hub_val_parse(a->val, value, &v)) {
    case HUB_VAL_OK:      break;
    case HUB_VAL_MISSING: return fail(call, "invalid_params", "this action needs a value");
    case HUB_VAL_RANGE:   return fail(call, "invalid_params", a->val == HUB_VAL_PERCENT ? "value must be 0-100" : "value is out of range");
    default:              return fail(call, "invalid_params", "value is not in the expected format");
    }
#if CONFIG_BITENGINE_ENABLE_ZHUB
    if (strncasecmp(id, "zb-", 3) == 0) return zb_control(call, a, v, id);
#endif
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
    if (strncasecmp(id, "mt-", 3) == 0) return mt_control(call, a, v, id);
#endif
    return fail(call, "not_found", "no controllable device with that id on this hub (ids come from hub.devices.list)");
}
#endif

/* ---------------- 表 ---------------- */

static const struct { const char *name; hub_cmd_fn fn; } k_impl[] = {
    { "device.health",      cmd_device_health },
    { "hub.thread.network", cmd_thread_network },
    { "hub.devices.list",   cmd_devices_list },
    { "hub.device.get",     cmd_device_get },
    { "hub.events.recent",  cmd_events_recent },
    { "hub.device.update",  cmd_device_update },
#if CONFIG_BITENGINE_ENABLE_ZHUB || CONFIG_BITENGINE_ENABLE_MATTER_CTRL
    { "hub.device.control", cmd_device_control },
#endif
#if CONFIG_BITENGINE_ENABLE_ZHUB
    { "hub.zigbee.permit_join", cmd_zigbee_permit_join },
#endif
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL
    { "hub.matter.commission", hub_matter_cmd_commission },   /* 作业：结论晚到（hub_matter.c，6.6） */
#endif
};

hub_cmd_fn hub_cmds_find(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(k_impl) / sizeof(k_impl[0]); i++) {
        if (strcmp(k_impl[i].name, name) == 0) return k_impl[i].fn;
    }
    (void)TAG;
    return NULL;
}
