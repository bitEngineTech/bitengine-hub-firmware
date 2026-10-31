/* thread_inventory —— Thread 网络层清单的 ESP 那一层（见 include/thread_inventory.h）。只在直连型里编。
 *
 * H1（2026-10-22 对照 IDF v6.0.2 的 openthread/include；符号在本构建的 libopenthread.a 里都有定义，nm 查过）：
 *   - 全部 OT 调用在 esp_openthread_lock_acquire(500 ms) 里，拷进 PSRAM 后马上放锁（7.1）；
 *   - otSrpServerGetNextHost(inst, prev)：prev = NULL 取第一个；otSrpServerHostIsDeleted 的跳过（删了但还留着 key lease 的）；
 *     otSrpServerHostGetFullName → "<host>.default.service.arpa."；otSrpServerHostGetAddresses(host, &num) 返回数组；
 *     otSrpServerHostGetLeaseInfo 的四个字段单位是**毫秒**；
 *   - otSrpServerHostGetNextService(host, prev) 同上；otSrpServerServiceGetServiceName → "_matter._tcp.default.service.arpa."（完整服务名，
 *     不含实例）；otSrpServerServiceGetInstanceLabel → 实例名第一段；otSrpServerServiceGetTxtData(svc, &len) 是 DNS-SD TXT 线格式；
 *   - otThreadGetChildInfoByIndex(inst, i, &ci)：i < otThreadGetMaxAllowedChildren，空槽 OT_ERROR_NOT_FOUND；
 *     otThreadGetChildNextIp6Address(inst, i, &iter, &addr)：同一个下标 i，iter 从 OT_CHILD_IP6_ADDRESS_ITERATOR_INIT 起；
 *   - otThreadGetRouterInfo(inst, rid, &ri)：rid 0..otThreadGetMaxRouterId()，没有这个 router 回 OT_ERROR_NOT_FOUND；
 *   - otThreadGetNextNeighborInfo(inst, &iter, &ni)：iter 从 OT_NEIGHBOR_INFO_ITERATOR_INIT 起；mIsChild 区分子节点 / 路由器邻居；
 *   - otThreadGetNextCacheEntry(inst, &ce, &iter)：iter 先整个清零；只有 OT_CACHE_ENTRY_STATE_CACHED 的 EID→RLOC 是确定的；
 *   - otPlatRadioGetReceiveSensitivity(inst) = OT 的噪声底（link_margin 的参照；算法正本在 hub_ti_core.c）；
 *   - otChildInfo.mIsStateRestoring：BR 重启后从 NVM 恢复、还没重新联系上的子节点（child_table.cpp），不算在网，跳过。 */
#include "thread_inventory.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "config_mgr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hub_events.h"
#include "openthread/platform/radio.h"
#include "openthread/srp_server.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"
#include "sdkconfig.h"
#include "thread_br.h"

static const char *TAG = "thread_inv";

#define LOCK_TICKS     pdMS_TO_TICKS(500)

_Static_assert(CONFIG_OPENTHREAD_MLE_MAX_CHILDREN <= TI_MAX_CHILDREN, "子节点表比清单的子节点上限大：TI_MAX_CHILDREN 要跟着改");
/* SRP 代播的上限跟着 CONFIG_MDNS_MAX_SERVICES 走（减去网关自用的 4 条，srp_readvertise.c 头注）：清单的主机上限不能比它小 */
_Static_assert(TI_MAX_HOSTS >= CONFIG_MDNS_MAX_SERVICES - 4, "SRP 可代播的比清单能装的多：TI_MAX_HOSTS 要跟着 CONFIG_MDNS_MAX_SERVICES 改");
#define OT_LOCK_TICKS  pdMS_TO_TICKS(500)

static ti_raw_t          *s_raw;          /* PSRAM：快照的原始数据 */
static ti_list_t         *s_list;         /* PSRAM：关联好的清单 */
static ti_prev_t         *s_prev;         /* PSRAM：上一轮的 id（增减、粘连） */
static SemaphoreHandle_t  s_lock;
static bool               s_inited;
static bool               s_outage;       /* BR 掉网 / 没在跑：回来后重建基线，不把这段时间的进出报成事件 */
static int64_t            s_refreshed_us; /* 0 = 还没成功刷新过 */
static thread_inventory_stats_t s_stats;  /* 在 s_lock 里改 */

/* ---------------------------------------------------------------- 从 OT 拷数据（持 OT 锁） */

static void ip6_copy(ti_ip6_t *dst, const otIp6Address *src) { memcpy(dst->b, src->mFields.m8, 16); }

static void copy_hosts(otInstance *inst, ti_raw_t *raw)
{
    const otSrpServerHost *host = NULL;
    while ((host = otSrpServerGetNextHost(inst, host)) != NULL) {
        if (otSrpServerHostIsDeleted(host)) continue;
        if (raw->n_hosts >= TI_MAX_HOSTS) {
            raw->hosts_truncated = true;
            break;
        }
        ti_host_t *h = &raw->hosts[raw->n_hosts];
        memset(h, 0, sizeof(*h));
        if (!ti_host_label(otSrpServerHostGetFullName(host), h->name, sizeof(h->name))) continue;
        uint8_t na = 0;
        const otIp6Address *addrs = otSrpServerHostGetAddresses(host, &na);
        for (uint8_t i = 0; addrs && i < na && h->n_addrs < TI_HOST_ADDRS; i++) ip6_copy(&h->addrs[h->n_addrs++], &addrs[i]);
        otSrpServerLeaseInfo li;
        otSrpServerHostGetLeaseInfo(host, &li);
        h->lease_remaining_s = li.mRemainingLease / 1000u;      /* 毫秒 → 秒 */
        const otSrpServerService *svc = NULL;
        while ((svc = otSrpServerHostGetNextService(host, svc)) != NULL) {
            if (otSrpServerServiceIsDeleted(svc)) continue;
            const char *sname = otSrpServerServiceGetServiceName(svc);
            if (ti_service_is(sname, "_matter", "_tcp")) {
                if (h->n_fabrics >= TI_FABRICS) continue;
                char cfid[17], node[17];
                if (!ti_parse_matter_instance(otSrpServerServiceGetInstanceLabel(svc), cfid, node)) continue;
                memcpy(h->fabrics[h->n_fabrics].cfid, cfid, sizeof(cfid));
                memcpy(h->fabrics[h->n_fabrics].node, node, sizeof(node));
                h->n_fabrics++;
            } else if (ti_service_is(sname, "_matterc", "_udp")) {
                uint16_t tl = 0;
                const uint8_t *txt = otSrpServerServiceGetTxtData(svc, &tl);
                h->commissionable = true;
                ti_parse_matterc_txt(txt, tl, h);
            }
        }
        raw->n_hosts++;
    }
}

static void copy_children(otInstance *inst, ti_raw_t *raw)
{
    const uint16_t max_c = otThreadGetMaxAllowedChildren(inst);
    for (uint16_t i = 0; i < max_c && raw->n_children < TI_MAX_CHILDREN; i++) {
        otChildInfo ci;
        if (otThreadGetChildInfoByIndex(inst, i, &ci) != OT_ERROR_NONE || ci.mIsStateRestoring) continue;
        ti_child_t *c = &raw->children[raw->n_children++];
        memset(c, 0, sizeof(*c));
        memcpy(c->ext, ci.mExtAddress.m8, 8);
        c->rloc16 = ci.mRloc16;
        c->rssi_avg = ci.mAverageRssi;
        c->rssi_last = ci.mLastRssi;
        c->rx_on_idle = ci.mRxOnWhenIdle;
        c->ftd = ci.mFullThreadDevice;
        c->age_s = ci.mAge;
        c->timeout_s = ci.mTimeout;
        c->connected_s = ci.mConnectionTime;
        otChildIp6AddressIterator it = OT_CHILD_IP6_ADDRESS_ITERATOR_INIT;
        otIp6Address a;
        while (c->n_addrs < TI_CHILD_ADDRS && otThreadGetChildNextIp6Address(inst, i, &it, &a) == OT_ERROR_NONE) {
            ip6_copy(&c->addrs[c->n_addrs++], &a);
        }
    }
}

static void copy_routers(otInstance *inst, ti_raw_t *raw)
{
    const uint8_t max_rid = otThreadGetMaxRouterId(inst);
    for (uint16_t rid = 0; rid <= max_rid && raw->n_routers < TI_MAX_ROUTERS; rid++) {
        otRouterInfo ri;
        if (otThreadGetRouterInfo(inst, rid, &ri) != OT_ERROR_NONE || !ri.mAllocated) continue;
        ti_router_t *r = &raw->routers[raw->n_routers++];
        memset(r, 0, sizeof(*r));
        memcpy(r->ext, ri.mExtAddress.m8, 8);
        r->rloc16 = ri.mRloc16;
        r->age_s = ri.mAge;
        r->rssi_avg = OT_RADIO_RSSI_INVALID;
    }
    otNeighborInfoIterator it = OT_NEIGHBOR_INFO_ITERATOR_INIT;
    otNeighborInfo ni;
    while (otThreadGetNextNeighborInfo(inst, &it, &ni) == OT_ERROR_NONE) {
        if (ni.mIsChild) continue;
        for (uint8_t i = 0; i < raw->n_routers; i++) {
            if (raw->routers[i].rloc16 == ni.mRloc16) {
                raw->routers[i].is_neighbor = true;
                raw->routers[i].rssi_avg = ni.mAverageRssi;
                break;
            }
        }
    }
}

static void copy_cache(otInstance *inst, ti_raw_t *raw)
{
    otCacheEntryIterator it;
    memset(&it, 0, sizeof(it));
    otCacheEntryInfo ce;
    while (raw->n_cache < TI_MAX_CACHE && otThreadGetNextCacheEntry(inst, &ce, &it) == OT_ERROR_NONE) {
        if (ce.mState != OT_CACHE_ENTRY_STATE_CACHED) continue;
        ti_cache_t *k = &raw->cache[raw->n_cache++];
        ip6_copy(&k->addr, &ce.mTarget);
        k->rloc16 = ce.mRloc16;
    }
}

typedef enum {
    COPY_OK = 0,
    COPY_NO_SRP,     /* 网络在、SRP 服务没在跑（自动启停模式下路由管理器一停它就停，Stop 会删光所有主机）：只有子节点 / 路由器，不比较 */
    COPY_DOWN,
    COPY_BUSY,
} copy_result_t;

static void raw_clear(ti_raw_t *raw)
{
    raw->n_hosts = raw->n_children = raw->n_cache = 0;
    raw->n_routers = 0;
    raw->hosts_truncated = false;
}

static copy_result_t copy_raw(ti_raw_t *raw)
{
    if (!thread_br_is_running()) {
        raw_clear(raw);
        return COPY_DOWN;
    }
    if (!esp_openthread_lock_acquire(OT_LOCK_TICKS)) return COPY_BUSY;   /* 忙：上一轮的数据原封不动（清单还指着它） */
    raw_clear(raw);
    const int64_t t0 = esp_timer_get_time();
    otInstance *inst = esp_openthread_get_instance();
    copy_result_t r = COPY_OK;
    if (!inst) {
        r = COPY_DOWN;
    } else {
        const otDeviceRole role = otThreadGetDeviceRole(inst);
        if (role == OT_DEVICE_ROLE_DISABLED || role == OT_DEVICE_ROLE_DETACHED) {
            r = COPY_DOWN;
        } else {
            raw->my_rloc16 = otThreadGetRloc16(inst);
            raw->noise_floor = otPlatRadioGetReceiveSensitivity(inst);
            if (otSrpServerGetState(inst) == OT_SRP_SERVER_STATE_RUNNING) copy_hosts(inst, raw);
            else r = COPY_NO_SRP;
            copy_children(inst, raw);
            copy_routers(inst, raw);
            copy_cache(inst, raw);
        }
    }
    const uint32_t held = (uint32_t)(esp_timer_get_time() - t0);
    esp_openthread_lock_release();
    s_stats.last_lock_us = held;
    if (held > s_stats.max_lock_us) s_stats.max_lock_us = held;
    return r;
}

/* ---------------------------------------------------------------- 增减 → 事件 */

static void on_change(void *ctx, const char *id, bool joined)
{
    (void)ctx;
    if (joined) {
        s_stats.joined++;
        (void)hub_event_emit(HUB_EVC_LIFECYCLE, id, "thread_member_joined", "A new device joined the hub's Thread network.", 0);
    } else {
        s_stats.left++;
        /* 只说看到的事实（MD-10）：没注册 SRP 的休眠子节点换到别的路由器下也会"看不到"，不能断言它离开了网络 */
        (void)hub_event_emit(HUB_EVC_AVAILABILITY, id, "thread_member_left",
                             "A device is no longer seen on the hub's Thread network.", HEV_F_PUSH);
    }
}

/* ---------------------------------------------------------------- 对外 */

esp_err_t thread_inventory_init(void)
{
    if (s_inited) return ESP_OK;
    s_raw = heap_caps_calloc(1, sizeof(ti_raw_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_list = heap_caps_calloc(1, sizeof(ti_list_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_prev = heap_caps_calloc(1, sizeof(ti_prev_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_lock = xSemaphoreCreateMutex();
    if (!s_raw || !s_list || !s_prev || !s_lock) {
        ESP_LOGE(TAG, "分配失败（raw %u B、list %u B、prev %u B）", (unsigned)sizeof(ti_raw_t), (unsigned)sizeof(ti_list_t),
                 (unsigned)sizeof(ti_prev_t));
        heap_caps_free(s_raw);                        /* 都放掉：再调一次 init 不泄漏 */
        heap_caps_free(s_list);
        heap_caps_free(s_prev);
        if (s_lock) vSemaphoreDelete(s_lock);
        s_raw = NULL;
        s_list = NULL;
        s_prev = NULL;
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_inited = true;
    return ESP_OK;
}

void thread_inventory_watch(void)
{
    if (!s_inited || !thread_br_is_running()) return;          /* BR 没在跑由刷新的 COPY_DOWN 处理 */
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(50))) return;
    bool ok = false;
    otInstance *inst = esp_openthread_get_instance();
    if (inst) {
        const otDeviceRole role = otThreadGetDeviceRole(inst);
        ok = role != OT_DEVICE_ROLE_DISABLED && role != OT_DEVICE_ROLE_DETACHED && otSrpServerGetState(inst) == OT_SRP_SERVER_STATE_RUNNING;
    }
    esp_openthread_lock_release();
    if (!ok && xSemaphoreTake(s_lock, LOCK_TICKS) == pdTRUE) {
        s_outage = true;                                        /* 下次刷新重建基线（静默期） */
        xSemaphoreGive(s_lock);
    }
}

esp_err_t thread_inventory_refresh(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_lock, LOCK_TICKS) != pdTRUE) return ESP_ERR_TIMEOUT;
    const int64_t t0 = esp_timer_get_time();
    esp_err_t ret = ESP_OK;
    const thread_config_t *tc = config_mgr_get_thread();
    if (!tc || !tc->enabled) {
        /* 用户关了 Thread：清单里不再有 Thread 成员，基线重来（重新打开后不把所有设备报成"新来的"） */
        s_list->n = 0;
        raw_clear(s_raw);                             /* 统计也归零（否则状态页显示旧的主机 / 子节点数） */
        memset(s_prev, 0, sizeof(*s_prev));
        s_outage = false;
        s_refreshed_us = esp_timer_get_time();
    } else {
        switch (copy_raw(s_raw)) {
        case COPY_BUSY:
            s_stats.ot_busy++;
            ret = ESP_ERR_TIMEOUT;                    /* 快照保持上一轮的 */
            break;
        case COPY_DOWN:
            s_list->n = 0;                            /* 掉网期间不说"在网"；thread_down 事件已经报了 */
            s_outage = true;
            s_refreshed_us = esp_timer_get_time();
            break;
        case COPY_NO_SRP:
            /* SRP 主机全没了不等于设备都走了：列出子节点 / 路由器，但不比较（不推一串假"离开"），SRP 回来后重建基线 */
            ti_associate(s_raw, s_prev, s_list);
            s_outage = true;
            s_refreshed_us = esp_timer_get_time();
            break;
        case COPY_OK:
        default:
            ti_associate(s_raw, s_prev, s_list);
            if (s_outage) {
                s_prev->have_baseline = false;        /* 掉网回来：这一轮只建基线 */
                s_outage = false;
            }
            ti_diff(s_prev, s_raw, s_list, (uint64_t)(esp_timer_get_time() / 1000), on_change, NULL);
            s_refreshed_us = esp_timer_get_time();
            s_stats.refreshes++;
            break;
        }
    }
    s_stats.entries = s_list->n;
    s_stats.hosts = s_raw->n_hosts;
    s_stats.children = s_raw->n_children;
    s_stats.routers = s_raw->n_routers;
    s_stats.hosts_truncated = s_raw->hosts_truncated;
    s_stats.last_refresh_us = (uint32_t)(esp_timer_get_time() - t0);
    if (s_stats.last_refresh_us > s_stats.max_refresh_us) s_stats.max_refresh_us = s_stats.last_refresh_us;
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t thread_inventory_with(uint32_t max_age_ms, void (*fn)(const ti_raw_t *raw, const ti_list_t *list, void *ctx), void *ctx)
{
    if (!s_inited || !fn) return ESP_ERR_INVALID_STATE;
    if (max_age_ms && (!s_refreshed_us || esp_timer_get_time() - s_refreshed_us > (int64_t)max_age_ms * 1000)) {
        (void)thread_inventory_refresh();             /* OT 锁忙也没关系：用上一轮的 */
    }
    if (xSemaphoreTake(s_lock, LOCK_TICKS) != pdTRUE) return ESP_ERR_TIMEOUT;
    fn(s_raw, s_list, ctx);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void thread_inventory_get_stats(thread_inventory_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (s_inited && xSemaphoreTake(s_lock, LOCK_TICKS) == pdTRUE) {
        *out = s_stats;
        xSemaphoreGive(s_lock);
    }
}
