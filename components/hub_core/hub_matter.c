/* hub_matter —— hub_core 这一侧的 Matter 控制器接线（docs/15 设计第九节；M7 第二步起）。
 *
 * matter_ctrl 不依赖 hub_core（同 zhub）：它定义钩子，这里用 hub_store 实现、在 matter_ctrl_start 之前登记（hub_svc_start）。
 * 第二步：fabric 身份的存取（hubdb/mctrl 的 ident）。第三步：节点表的存取（mctrl/nodes）、hub.matter.commission 的处理函数、
 * 本地移除（/api/v1/hub/device/remove 的 mt- 设备）—— 都是作业（hub_jobs.h）：matter_ctrl 在 CHIP 任务里回调完成，这里结束作业。
 * 以后的 mt- 设备列表项与事件也放这里。
 *
 * ⚠️ 身份 blob 里是根 CA 私钥与 IPK（设计第十三节"秘密"）：不打日志、不进 GET / 导出 / Muse；读回比对用的临时缓冲用完清零。
 * 钩子在 matter_ctrl 的启动任务 / CHIP 任务里调（都是内部栈），可以同步等 hub_store；不许在 esp_timer 回调里调（红线 10）。 */
#include "sdkconfig.h"
#if CONFIG_BITENGINE_ENABLE_MATTER_CTRL

#include <stdlib.h>
#include <string.h>

#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "hub_events.h"
#include "hub_jobs.h"
#include "hub_json.h"
#include "hub_meta.h"
#include "hub_sanitize.h"
#include "esp_heap_caps.h"
#include "hub_mcap.h"
#include "hub_model.h"
#include "hub_store.h"
#include "hub_ti_core.h"
#include "matter_ctrl.h"
#include "matter_ctrl_iv.h"
#include "matter_ctrl_live.h"
#include "esp_timer.h"
#include <strings.h>
#include "hub_matter.h"
#include "hub_registry.h"

static const char *TAG = "hub_matter";

#define IDENT_KEY      "ident"
#define NODES_KEY      "nodes"
#define IDENT_WAIT_MS  3000      /* 开机时 hub_store 可能正忙着别人的开机加载（hub_zigbee 的 LOAD_RETRY_MS 同理） */
#define IDENT_CAP      1024      /* ≥ MC_IDENT_BLOB_MAX（matter_ctrl 内部的上限，约 750 B） */
#define SAVE_CAP       4000      /* 落盘读回比对的缓冲 = hub_store 单块上限（HSC_BLOB_MAX）：节点表 128 台 2057 B（Matter 32 → 128 时原来的 2048 不够了） */
/* 访谈存取 / 事件钩子在 CHIP 任务里调（配网、CASE 的消息处理都在这个任务上）：只短等（同 zhub 的 META_LOCK_ZHUB_MS，审计）。
   身份 / 节点表那两个钩子照旧等 IDENT_WAIT_MS —— 它们要落盘读回确认，宁可慢。hub_event_emit 自己最多等 100 ms（只护 memcpy） */
#define CHIP_WAIT_MS   20
#define HUB_MT_CTL_CAPS (HUB_CAP_ONOFF | HUB_CAP_LEVEL)   /* 控制路径驱动得了的（M7 第六步）；色温 / 颜色 / 窗帘 / 门锁 / 温控随后 */
#define MC_LIST_MAX    128       /* ≥ CONFIG_BITENGINE_MATTER_MAX_NODES（Kconfig range 1..128）；列表 / 详情每次在 PSRAM 拷一份节点表 */
_Static_assert(CONFIG_BITENGINE_MATTER_MAX_NODES <= MC_LIST_MAX, "list buffer smaller than the node table");

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

static matter_ctrl_load_t hk_ident_load(uint8_t max_ver, void *buf, size_t *len_inout)
{
    uint8_t ver = 0;
    const esp_err_t e = hub_store_get(HUB_STORE_NS_MCTRL, IDENT_KEY, max_ver, &ver, buf, len_inout, IDENT_WAIT_MS);
    if (e == ESP_OK) return MATTER_CTRL_LOAD_OK;
    if (e == ESP_ERR_NOT_FOUND) return MATTER_CTRL_LOAD_EMPTY;
    /* 超时 / hub_store 没起来 / 存的格式比固件新（OTA 回滚后）：都不是"没有" —— matter_ctrl 不会因此新建身份 */
    ESP_LOGE(TAG, "读 fabric 身份失败：%s —— Matter 控制器这次开机不起（不新建，免得之前的设备成孤儿）", esp_err_to_name(e));
    return MATTER_CTRL_LOAD_ERR;
}

/* 写 → 立即落盘 → 读回逐字节比对。只有比对一致才回 ESP_OK：matter_ctrl 拿到 OK 之前不签任何证书 / 不拿新节点号去配 */
static esp_err_t save_confirmed(const char *key, uint8_t ver, const void *buf, size_t len)
{
    if (!buf || len == 0 || len > SAVE_CAP) return ESP_ERR_INVALID_ARG;
    esp_err_t e = hub_store_put(HUB_STORE_NS_MCTRL, key, ver, buf, len);
    if (e == ESP_OK) e = hub_store_flush(IDENT_WAIT_MS);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "mctrl/%s 没落盘：%s", key, esp_err_to_name(e));
        return e;
    }
    uint8_t *back = calloc(1, SAVE_CAP);
    if (!back) return ESP_ERR_NO_MEM;
    size_t n = SAVE_CAP;
    uint8_t got_ver = 0;
    e = hub_store_get(HUB_STORE_NS_MCTRL, key, ver, &got_ver, back, &n, IDENT_WAIT_MS);
    if (e == ESP_OK && (got_ver != ver || n != len || memcmp(back, buf, len) != 0)) e = ESP_ERR_INVALID_CRC;
    wipe(back, SAVE_CAP);
    free(back);
    if (e != ESP_OK) ESP_LOGE(TAG, "mctrl/%s 读回对不上：%s", key, esp_err_to_name(e));
    return e;
}

static esp_err_t hk_ident_save(uint8_t ver, const void *buf, size_t len)
{
    if (len > IDENT_CAP) return ESP_ERR_INVALID_ARG;
    return save_confirmed(IDENT_KEY, ver, buf, len);
}

/* 节点表（≤ 9 + 128 × 16 = 2057 B，SAVE_CAP 装得下） */
static matter_ctrl_load_t hk_nodes_load(uint8_t max_ver, void *buf, size_t *len_inout)
{
    uint8_t ver = 0;
    const esp_err_t e = hub_store_get(HUB_STORE_NS_MCTRL, NODES_KEY, max_ver, &ver, buf, len_inout, IDENT_WAIT_MS);
    if (e == ESP_OK) return MATTER_CTRL_LOAD_OK;
    if (e == ESP_ERR_NOT_FOUND) return MATTER_CTRL_LOAD_EMPTY;
    ESP_LOGE(TAG, "读 Matter 节点表失败：%s —— 这次开机不配网、不移除（不覆盖：节点号会重发）", esp_err_to_name(e));
    return MATTER_CTRL_LOAD_ERR;
}

static esp_err_t hk_nodes_save(uint8_t ver, const void *buf, size_t len) { return save_confirmed(NODES_KEY, ver, buf, len); }

/* ---------------------------------------------------------------- 访谈结果的存取与设备事件（M7 第四步） */

/* "允许测试设备"（mctrl/opts，1 字节：bit0）。读在启动任务里（可以等）；恢复出厂全清、重置 Matter 控制器连同 mctrl 一起清掉 = 回到关 */
#define OPTS_KEY       HUB_MATTER_OPTS_KEY
#define OPTS_VER       1
#define OPT_ALLOW_TEST 0x01u

static bool hk_allow_test_load(void)
{
    uint8_t v = 0, ver = 0;
    size_t n = sizeof(v);
    const esp_err_t e = hub_store_get(HUB_STORE_NS_MCTRL, OPTS_KEY, OPTS_VER, &ver, &v, &n, IDENT_WAIT_MS);
    if (e != ESP_OK && e != ESP_ERR_NOT_FOUND) ESP_LOGW(TAG, "读 Matter 选项失败（%s）：测试设备按默认拒绝", esp_err_to_name(e));
    return e == ESP_OK && n == 1 && (v & OPT_ALLOW_TEST);
}

esp_err_t hub_matter_set_allow_test(bool on)
{
    const uint8_t v = on ? OPT_ALLOW_TEST : 0;
    if (!on) {
        /* 关：先生效再存（安全的一边；审计）—— 存不上照样报，说明重启后可能又回到开 */
        const esp_err_t ae = matter_ctrl_set_allow_test_devices(false);
        const esp_err_t se = save_confirmed(OPTS_KEY, OPTS_VER, &v, 1);
        return se != ESP_OK ? se : ae == ESP_OK ? ESP_OK : ESP_ERR_NOT_FINISHED;
    }
    /* 开：落盘读回确认之后才生效（别让"开着"只活在内存里、重启又悄悄变回去） */
    const esp_err_t se = save_confirmed(OPTS_KEY, OPTS_VER, &v, 1);
    if (se != ESP_OK) return se;
    return matter_ctrl_set_allow_test_devices(true) == ESP_OK ? ESP_OK : ESP_ERR_NOT_FINISHED;   /* 存上了、CHIP 任务没排上：重启后完整生效 */
}

/* 重置 Matter 控制器（设计 10.1、第十二节；2026-10-24 用户定：只在身份丢了 —— ident_lost —— 时给）。顺序保证中途失败都能再来：
   ① 清 Matter 设备的名字 / 房间 / 敏感标记（节点号会从 2 重新发，旧的不能落到新设备头上）；② 清 hubdb 的 mctrl（节点表、访谈、选项）；
   ③ 最后删 matter 分区的绑定记号。③ 之前失败：记号还在，下次开机照样停在 ident_lost，可以再按一次。成功后调用方重启：没有身份、没有记号
   = 新网关，建新身份、擦掉 CHIP 的旧状态（matter_rebound） */
esp_err_t hub_matter_reset(char *why, size_t why_len)
{
    matter_ctrl_status_t st;
    matter_ctrl_get_status(&st);
    if (st.state != MATTER_CTRL_FAILED || !st.fail_step || strcmp(st.fail_step, "ident_lost") != 0) {
        snprintf(why, why_len, "the Matter controller only needs a reset when its identity is lost; it is %s", matter_ctrl_state_str(st.state));
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t e = hub_meta_forget_proto(HUB_META_PROTO_MATTER);
    if (e != ESP_OK) {
        snprintf(why, why_len, e == ESP_ERR_INVALID_STATE ? "the hub could not read all of its saved device names when it started; restart the hub, then try again"
                                                          : "could not clear the Matter device names (%s); try again", esp_err_to_name(e));
        return e == ESP_ERR_INVALID_STATE ? ESP_FAIL : e;      /* INVALID_STATE 留给"状态不让做"（409）；这是"现在做不了"（503） */
    }
    e = hub_store_erase_ns(HUB_STORE_NS_MCTRL, IDENT_WAIT_MS);
    if (e == ESP_OK) e = hub_store_flush(IDENT_WAIT_MS);
    if (e != ESP_OK) {
        snprintf(why, why_len, "could not clear the Matter controller data (%s); try again", esp_err_to_name(e));
        return e;
    }
    e = matter_ctrl_forget_binding();
    if (e != ESP_OK) {
        snprintf(why, why_len, "could not clear the Matter controller binding (%s); try again", esp_err_to_name(e));
        return e;
    }
    ESP_LOGW(TAG, "Matter 控制器已重置（身份丢了之后的重建）：重启后建新身份，原来的设备要在各自的 App 里重新分享");
    return ESP_OK;
}

static void iv_key(uint64_t node, char out[16]) { snprintf(out, 16, "iv%llu", (unsigned long long)node); }

static matter_ctrl_load_t hk_iv_load(uint64_t node, uint8_t max_ver, void *buf, size_t *len_inout)
{
    char key[16];
    iv_key(node, key);
    uint8_t ver = 0;
    const esp_err_t e = hub_store_get(HUB_STORE_NS_MCTRL, key, max_ver, &ver, buf, len_inout, IDENT_WAIT_MS);
    if (e == ESP_OK) return MATTER_CTRL_LOAD_OK;
    return e == ESP_ERR_NOT_FOUND ? MATTER_CTRL_LOAD_EMPTY : MATTER_CTRL_LOAD_ERR;   /* 读不出来：下次再访谈一遍，不用另外报 */
}

static esp_err_t hk_iv_save(uint64_t node, uint8_t ver, const void *buf, size_t len)
{
    char key[16];
    iv_key(node, key);
    return hub_store_put_wait(HUB_STORE_NS_MCTRL, key, ver, buf, len, CHIP_WAIT_MS);   /* 防抖即可：丢了下次开机再访谈 */
}

static void hk_iv_erase(uint64_t node)
{
    char key[16];
    iv_key(node, key);
    (void)hub_store_erase(HUB_STORE_NS_MCTRL, key);
}

/* "mt-<16 位十六进制节点号>"：输出小写（同 zb- / th-，hub.device.update 的回读也是小写）；解析不分大小写 */
static void mt_id(uint64_t node, char out[20]) { snprintf(out, 20, "mt-%016" PRIx64, node); }

static bool mt_parse(const char *id, uint64_t *node)
{
    if (!id || strlen(id) != 19 || strncasecmp(id, "mt-", 3) != 0) return false;
    uint64_t v = 0;
    for (int i = 3; i < 19; i++) {
        const char ch = id[i];
        const int d = (ch >= '0' && ch <= '9') ? ch - '0' : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 : -1;
        if (d < 0) return false;
        v = (v << 4) | (uint64_t)d;
    }
    *node = v;
    return true;
}

static void id8_of(uint64_t node, uint8_t id8[8])
{
    for (int i = 0; i < 8; i++) id8[i] = (uint8_t)(node >> (56 - 8 * i));   /* hub_meta：高字节在前（同 "mt-" 后面的十六进制） */
}

static void cap_first(char *s)
{
    if (s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 'a' + 'A');
}

static void emit_ev(hub_evt_class_t cls, const char *id, const char *kind, const char *text, unsigned flags)
{
    const esp_err_t e = hub_event_emit(cls, id, kind, text, flags);
    if (e != ESP_OK) ESP_LOGW(TAG, "事件 %s 没进事件环（%s）", kind, esp_err_to_name(e));
}

/* 读数翻转（M7 第五步）：类别、措辞、推送标志照 Zigbee（hub_zigbee.c 的 state_event；6.7）。门磁 / 人体是 SECURITY（推不推 = 类别位 AND
   设备 notify 位，在 hub_event_emit 里判）；漏水 / 烟雾是 ALARM（恒推，检测到与解除成对）；电量低是 BATTERY */
static void state_event(const char *id, const char *name, unsigned bits, const mc_live_t *lv)
{
    char cname[HUB_META_EVNAME_MAX + 1], when[16] = "", hhmm[6], t[HEV_TEXT_MAX];
    snprintf(cname, sizeof(cname), "%s", name);
    cap_first(cname);
    hub_events_local_clock(hhmm);
    if (hhmm[0]) snprintf(when, sizeof(when), " at %s", hhmm);
    const unsigned alarm_flags = HEV_F_PUSH;
    if (bits & MC_SEV_CONTACT) {
        const bool open = lv->st.contact_open != 0;
        snprintf(t, sizeof(t), "%s %s%s.", cname, open ? "opened" : "closed", when);
        emit_ev(HUB_EVC_SECURITY, id, open ? "contact_open" : "contact_closed", t, HEV_F_PUSH);
    }
    if (bits & MC_SEV_MOTION_ON) {
        snprintf(t, sizeof(t), "Motion detected by %s%s.", name, when);
        emit_ev(HUB_EVC_SECURITY, id, "motion_detected", t, HEV_F_PUSH);
    }
    if (bits & MC_SEV_LEAK) {
        const bool on = lv->st.leak != 0;
        snprintf(t, sizeof(t), on ? "Water leak detected by %s%s." : "Water leak cleared on %s%s.", name, when);
        emit_ev(HUB_EVC_ALARM, id, on ? "water_leak" : "water_leak_cleared", t, alarm_flags);
    }
    if (bits & MC_SEV_SMOKE) {
        const bool on = lv->st.smoke != 0;
        snprintf(t, sizeof(t), on ? "Smoke detected by %s%s." : "Smoke alarm cleared on %s%s.", name, when);
        emit_ev(HUB_EVC_ALARM, id, on ? "smoke_detected" : "smoke_cleared", t, alarm_flags);
    }
    if (bits & MC_SEV_BATT_LOW) {
        snprintf(t, sizeof(t), "Low battery on %s.", name);
        emit_ev(HUB_EVC_BATTERY, id, "battery_low", t, HEV_F_PUSH);
    }
}

static void hk_event(uint64_t node, int ev, unsigned bits)
{
    mc_iv_t iv;
    const bool have = matter_ctrl_node_iv(node, &iv);
    char id[20], nm[HUB_META_EVNAME_MAX + 1], t[HEV_TEXT_MAX];
    mt_id(node, id);
    /* 称呼同 Zigbee 的事件：用户起的名字（带房间，引号 / 方括号已换掉，防伪造 [id]）> "unnamed <设备类> <节点号低 16 位>" */
    hub_meta_ent_t m;
    uint8_t id8[8];
    id8_of(node, id8);
    const bool named = hub_meta_lookup(HUB_META_PROTO_MATTER, id8, &m, CHIP_WAIT_MS);   /* 拿不到按没名字（hub_meta 计 lookup_busy） */
    (void)hub_meta_event_name(named ? &m : NULL, HUB_META_PROTO_MATTER, have ? hub_mcap_kind(&iv, hub_mcap_caps(&iv)) : NULL,
                              (uint16_t)node, nm, sizeof(nm));
    char cname[sizeof(nm)];
    snprintf(cname, sizeof(cname), "%s", nm);
    cap_first(cname);
    mc_live_t lv;
    switch (ev) {
    case MATTER_CTRL_EV_ADDED:
        /* 配进来、第一次访谈完：推（同 Zigbee 新入网的"paired and identified"；设计 6.7 LIFECYCLE） */
        snprintf(t, sizeof(t), "New Matter device added and identified: %s. hub.device.get shows what it reported about itself.", nm);
        emit_ev(HUB_EVC_LIFECYCLE, id, "matter_device_added", t, HEV_F_PUSH);
        break;
    case MATTER_CTRL_EV_INTERVIEWED:
        snprintf(t, sizeof(t), "Identified a Matter device again: %s.", nm);
        emit_ev(HUB_EVC_LIFECYCLE, id, "matter_identified", t, 0);
        break;
    case MATTER_CTRL_EV_OFFLINE: {                 /* 模板同 Zigbee（6.7）；推 */
        const int64_t now = esp_timer_get_time() / 1000;
        const bool got = matter_ctrl_node_live(node, &lv) && lv.last_ms;
        const uint32_t min = got ? (uint32_t)((now - lv.last_ms) / 60000) : 0;
        if (!got) snprintf(t, sizeof(t), "%s is offline: the hub could not reach it since it started.", cname);
        else if (min >= 120u) snprintf(t, sizeof(t), "%s is offline: nothing heard from it for %u h.", cname, (unsigned)(min / 60u));
        else snprintf(t, sizeof(t), "%s is offline: it stopped answering (nothing heard for %u min).", cname, (unsigned)(min ? min : 1u));
        emit_ev(HUB_EVC_AVAILABILITY, id, "matter_offline", t, HEV_F_PUSH);
        break;
    }
    case MATTER_CTRL_EV_ONLINE:
        snprintf(t, sizeof(t), "%s is back online.", cname);
        emit_ev(HUB_EVC_AVAILABILITY, id, "matter_online", t, HEV_F_PUSH);
        break;
    case MATTER_CTRL_EV_STATE:
        if (matter_ctrl_node_live(node, &lv)) state_event(id, nm, bits, &lv);
        break;
    default:
        ESP_LOGW(TAG, "不认识的 Matter 事件 %d（节点 %016" PRIx64 "）", ev, node);
        break;
    }
}

/* ---------------------------------------------------------------- 列表项（7.3：本网关 fabric 上的按 mt- 呈现；附录 A 的列表项格式） */

static void put_dev_str(hj_t *w, const char *key, const char *raw)
{
    char clean[HUB_DEV_STR_MAX + 1];
    const size_t n = hub_sanitize_device_str(raw, strlen(raw), clean, sizeof(clean), HUB_DEV_STR_MAX);
    if (n) hj_kstr(w, key, clean);
}

/* 四舍五入的整除（d > 0）：负数朝零的对称舍入 */
static int64_t div_round(int64_t x, int64_t d) { return x >= 0 ? (x + d / 2) / d : -((-x + d / 2) / d); }

/* 读数对象，键名同 Zigbee（hub_zigbee.c 的 write_state；功率 / 电能是 Matter 先有的：power_w 一位小数、energy_kwh 三位小数） */
static void write_state(hj_t *w, const mc_state_t *s, bool ages, int64_t now)
{
#define ST_HAS(f) ((s->have & (1u << (f))) != 0)
#define ST_AGE(k, f) hj_kuint(w, (k), (uint64_t)((now - s->at_ms[(f)]) / 1000))
    hj_key(w, ages ? "state_age_s" : "state");
    hj_obj_begin(w);
    if (ST_HAS(MC_ST_CONTACT)) { if (ages) ST_AGE("contact", MC_ST_CONTACT); else hj_kstr(w, "contact", s->contact_open ? "open" : "closed"); }
    if (ST_HAS(MC_ST_LEAK)) { if (ages) ST_AGE("water_leak", MC_ST_LEAK); else hj_kbool(w, "water_leak", s->leak != 0); }
    if (ST_HAS(MC_ST_SMOKE)) { if (ages) ST_AGE("smoke", MC_ST_SMOKE); else hj_kbool(w, "smoke", s->smoke != 0); }
    if (ST_HAS(MC_ST_MOTION)) { if (ages) ST_AGE("motion", MC_ST_MOTION); else hj_kbool(w, "motion", s->motion != 0); }
    if (ST_HAS(MC_ST_BATTERY)) { if (ages) ST_AGE("battery_pct", MC_ST_BATTERY); else hj_kuint(w, "battery_pct", s->battery_pct); }
    if (ST_HAS(MC_ST_TEMP)) { if (ages) ST_AGE("temperature_c", MC_ST_TEMP); else hj_kfixed(w, "temperature_c", s->temp_c100, 2); }
    if (ST_HAS(MC_ST_HUM)) { if (ages) ST_AGE("humidity_pct", MC_ST_HUM); else hj_kfixed(w, "humidity_pct", s->hum_c100, 2); }
    if (ST_HAS(MC_ST_PRESS)) { if (ages) ST_AGE("pressure_hpa", MC_ST_PRESS); else hj_kfixed(w, "pressure_hpa", s->press_hpa10, 1); }
    if (ST_HAS(MC_ST_LUX)) { if (ages) ST_AGE("illuminance_lux", MC_ST_LUX); else hj_kuint(w, "illuminance_lux", s->lux); }
    if (ST_HAS(MC_ST_ONOFF)) { if (ages) ST_AGE("onoff", MC_ST_ONOFF); else hj_kbool(w, "onoff", s->onoff != 0); }
    if (ST_HAS(MC_ST_LEVEL)) { if (ages) ST_AGE("level_pct", MC_ST_LEVEL); else hj_kuint(w, "level_pct", s->level_pct); }
    if (ST_HAS(MC_ST_POWER)) { if (ages) ST_AGE("power_w", MC_ST_POWER); else hj_kfixed(w, "power_w", div_round(s->power_mw, 100), 1); }
    if (ST_HAS(MC_ST_ENERGY)) { if (ages) ST_AGE("energy_kwh", MC_ST_ENERGY); else hj_kfixed(w, "energy_kwh", div_round(s->energy_mwh, 1000), 3); }
#undef ST_HAS
#undef ST_AGE
    hj_obj_end(w);
}

static void write_entry(hj_t *w, const matter_ctrl_node_t *nd, const mc_iv_t *iv, const hub_meta_ent_t *meta, bool full,
                        const ti_raw_t *raw, const ti_entry_t *te)
{
    char id[20], hex[20];
    mt_id(nd->node_id, id);
    const bool done = iv && (iv->flags & MC_IV_F_DONE);
    const uint32_t caps = iv ? hub_mcap_caps(iv) : 0;
    hj_obj_begin(w);
    hj_kstr(w, "id", id);
    hj_kstr(w, "protocol", "matter");
    hj_kstr(w, "name", meta->name);
    hj_kstr(w, "room", meta->room);
    hj_kstr(w, "kind", hub_mcap_kind(iv, caps));
    if (done) {
        put_dev_str(w, "manufacturer", iv->vendor);
        put_dev_str(w, "model", iv->product);
    }
    hj_key(w, "capabilities");
    hj_arr_begin(w);
    for (unsigned b = 0; b < HUB_CAP_COUNT; b++)
        if (caps & (1u << b)) hj_str(w, hub_cap_name(1u << b));
    hj_arr_end(w);
    hj_kbool(w, "controllable", (caps & HUB_MT_CTL_CAPS) != 0);   /* 控制路径（M7 第六步）：开关 / 亮度 */
    mc_live_t lv;
    if (matter_ctrl_node_live(nd->node_id, &lv)) {  /* 订阅（第五步）：在线、最后听到、读数；这次开机还不知道在不在线就不写 online */
        const int64_t now = esp_timer_get_time() / 1000;
        if (lv.online >= 0) hj_kbool(w, "online", lv.online == 1);
        if (lv.last_ms) hj_kuint(w, "last_seen_s", (uint64_t)((now - lv.last_ms) / 1000));
        write_state(w, &lv.st, false, now);
        write_state(w, &lv.st, true, now);         /* 每个读数的年龄：缺的键 = 这次开机还没收到（同 Zigbee） */
    }
    if (raw && te) ti_write_thread_obj(w, raw, te, full);   /* 设备在本网关的 Thread 上：Thread 信息挂在这里（7.3） */
    hj_kbool(w, "notify", (meta->flags & HUB_META_F_NOTIFY) != 0);
    hj_kbool(w, "sensitive", (meta->flags & HUB_META_F_SENSITIVE) != 0);
    hj_key(w, "matter");
    hj_obj_begin(w);
    snprintf(hex, sizeof(hex), "%016" PRIX64, nd->node_id);
    hj_kstr(w, "node_id", hex);
    const uint16_t vid = done ? iv->vid : nd->vid, pid = done ? iv->pid : nd->pid;
    if (vid) hj_kuint(w, "vendor_id", vid);
    if (pid) hj_kuint(w, "product_id", pid);
    hj_kstr(w, "interview", matter_ctrl_node_iv_busy(nd->node_id) ? "running" : done ? "done" : "pending");
    if (iv && (iv->flags & MC_IV_F_ICD)) hj_kbool(w, "sleepy", true);
    if (full && done) {
        if (iv->label[0]) put_dev_str(w, "label", iv->label);
        hj_key(w, "endpoints");
        hj_arr_begin(w);
        for (unsigned i = 0; i < iv->n_ep; i++) {
            const mc_iv_ep_t *e = &iv->ep[i];
            hj_obj_begin(w);
            hj_kuint(w, "endpoint", e->ep);
            hj_key(w, "device_types");
            hj_arr_begin(w);
            for (unsigned k = 0; k < e->n_dt; k++) {
                char b[12];
                snprintf(b, sizeof(b), "0x%04" PRIX32, e->dt[k]);
                hj_str(w, b);
            }
            hj_arr_end(w);
            hj_key(w, "clusters");
            hj_arr_begin(w);
            for (unsigned k = 0; k < e->n_cl; k++) {
                char b[8];
                snprintf(b, sizeof(b), "0x%04X", (unsigned)e->cl[k]);
                hj_str(w, b);
            }
            hj_arr_end(w);
            hj_obj_end(w);
        }
        hj_arr_end(w);
        if (iv->flags & (MC_IV_F_EP_TRUNC | MC_IV_F_CL_TRUNC)) hj_kbool(w, "endpoints_truncated", true);
    }
    hj_obj_end(w);
    hj_obj_end(w);
}

static void our_cfid(char out[17])
{
    matter_ctrl_status_t st;
    matter_ctrl_get_status(&st);
    if (st.state == MATTER_CTRL_RUNNING && st.compressed_fabric_id) snprintf(out, 17, "%016" PRIX64, st.compressed_fabric_id);
    else out[0] = '\0';
}

struct hub_mt_own_s {
    char     cfid[17];
    int      n;
    uint64_t node[MC_LIST_MAX];
};

hub_mt_own_t *hub_matter_own_get(void)
{
    char cfid[17];
    our_cfid(cfid);
    if (!cfid[0]) return NULL;
    hub_mt_own_t *o = heap_caps_malloc(sizeof(*o), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    matter_ctrl_node_t *nodes = heap_caps_malloc(MC_LIST_MAX * sizeof(*nodes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!o || !nodes) {
        heap_caps_free(o);
        heap_caps_free(nodes);
        return NULL;                                 /* 拿不到：不跳过任何 th-（重复好过消失） */
    }
    memcpy(o->cfid, cfid, sizeof(cfid));
    o->n = 0;
    const int n = matter_ctrl_nodes(nodes, MC_LIST_MAX);
    for (int i = 0; i < n; i++)
        if (nodes[i].commissioned) o->node[o->n++] = nodes[i].node_id;   /* 同列表：配好了的才会以 mt- 出现 */
    heap_caps_free(nodes);
    return o;
}

bool hub_matter_own_has(const hub_mt_own_t *o, const ti_raw_t *raw, const ti_entry_t *e, char mt_out[20])
{
    char hex[17], id[20];
    uint64_t node;
    if (!o || !ti_entry_on_fabric(raw, e, o->cfid, hex)) return false;
    snprintf(id, sizeof(id), "mt-%s", hex);
    if (!mt_parse(id, &node)) return false;
    for (int i = 0; i < o->n; i++) {
        if (o->node[i] != node) continue;
        if (mt_out) mt_id(node, mt_out);
        return true;
    }
    return false;                                    /* 本 fabric、可节点表里没有（强制移除了 / 正在配 / 表没读回）：照旧按 th- 列 */
}

void hub_matter_own_free(hub_mt_own_t *o) { heap_caps_free(o); }

int hub_matter_write_list(hj_t *w, unsigned long start, unsigned long *next, unsigned long *n_out, uint32_t cap_filter, const char *room,
                          const ti_raw_t *raw, const ti_list_t *l)
{
    matter_ctrl_node_t *nodes = heap_caps_malloc(MC_LIST_MAX * sizeof(*nodes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    mc_iv_t *iv = heap_caps_malloc(sizeof(*iv), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!nodes || !iv) {
        heap_caps_free(nodes);
        heap_caps_free(iv);
        return HUB_MT_BUSY;
    }
    const int n = matter_ctrl_nodes(nodes, MC_LIST_MAX);
    char cfid[17];
    our_cfid(cfid);
    int rc = HUB_MT_DONE;
    unsigned long k = 0;
    for (int i = 0; i < n; i++) {
        if (!nodes[i].commissioned) continue;          /* 正在配网的不列（配完由作业的结论 / 推送告诉用户） */
        const bool have = matter_ctrl_node_iv(nodes[i].node_id, iv);
        if (cap_filter && !(have && (hub_mcap_caps(iv) & cap_filter))) continue;
        hub_meta_ent_t meta;
        uint8_t id8[8];
        id8_of(nodes[i].node_id, id8);
        if (!hub_meta_lookup(HUB_META_PROTO_MATTER, id8, &meta, HUB_META_LOOKUP_MS)) memset(&meta, 0, sizeof(meta));
        if (room && strcasecmp(meta.room, room) != 0) continue;
        if (k++ < start || rc != HUB_MT_DONE) continue;
        const ti_entry_t *te = cfid[0] ? ti_find_fabric_node(raw, l, cfid, nodes[i].node_id) : NULL;
        hj_mark_t m = hj_mark(w);
        write_entry(w, &nodes[i], have ? iv : NULL, &meta, false, raw, te);
        if (w->truncated) {
            hj_rollback(w, m);
            if (next) *next = k - 1;
            rc = HUB_MT_TRUNCATED;
        }
    }
    if (n_out) *n_out = k;
    heap_caps_free(nodes);
    heap_caps_free(iv);
    return rc;
}

int hub_matter_ctl_info(const char *id, uint64_t *node, uint32_t *caps, uint32_t *ctl_caps, bool *sensitive)
{
    uint64_t n = 0;
    if (!mt_parse(id, &n)) return HUB_MT_NOT_FOUND;
    matter_ctrl_node_t *nodes = heap_caps_malloc(MC_LIST_MAX * sizeof(*nodes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    mc_iv_t *iv = heap_caps_malloc(sizeof(*iv), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!nodes || !iv) {
        heap_caps_free(nodes);
        heap_caps_free(iv);
        return HUB_MT_BUSY;
    }
    const int k = matter_ctrl_nodes(nodes, MC_LIST_MAX);
    int rc = HUB_MT_NOT_FOUND;
    for (int i = 0; i < k; i++) {
        if (nodes[i].node_id != n || !nodes[i].commissioned) continue;
        const uint32_t c = matter_ctrl_node_iv(n, iv) ? hub_mcap_caps(iv) : 0;
        if (node) *node = n;
        if (caps) *caps = c;
        if (ctl_caps) *ctl_caps = c & HUB_MT_CTL_CAPS;
        rc = HUB_MT_DONE;
        if (sensitive) {
            /* 查不了（表没起来 / 锁忙）别当成"不敏感"放行：回忙，让调用方过会儿再试（同 hub_zigbee_ctl_info；第六步审计 ❌：
               开放的四个动作都是"设备标了敏感才拦"，这个标记是第 3 道检查里唯一的闸） */
            hub_meta_ent_t m;
            uint8_t id8[8];
            id8_of(n, id8);
            const int lk = hub_meta_lookup_ex(HUB_META_PROTO_MATTER, id8, &m, HUB_META_LOOKUP_MS);
            if (lk == HUB_META_LK_UNKNOWN) rc = HUB_MT_BUSY;
            *sensitive = lk == HUB_META_LK_FOUND && (m.flags & HUB_META_F_SENSITIVE) != 0;
        }
        break;
    }
    heap_caps_free(nodes);
    heap_caps_free(iv);
    return rc;
}

int hub_matter_write_state(hj_t *w, const char *id)
{
    uint64_t n = 0;
    mc_live_t lv;
    if (!w || !mt_parse(id, &n) || !matter_ctrl_node_live(n, &lv)) return HUB_MT_NOT_FOUND;
    write_state(w, &lv.st, false, esp_timer_get_time() / 1000);
    return HUB_MT_DONE;
}

int hub_matter_count(void)
{
    matter_ctrl_node_t *nodes = heap_caps_malloc(MC_LIST_MAX * sizeof(*nodes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!nodes) return -1;
    const int n = matter_ctrl_nodes(nodes, MC_LIST_MAX);
    int k = 0;
    for (int i = 0; i < n; i++)
        if (nodes[i].commissioned) k++;                /* 同列表：只数配好了的 */
    heap_caps_free(nodes);
    return k;
}

int hub_matter_write_device(hj_t *w, const char *id, const ti_raw_t *raw, const ti_list_t *l)
{
    uint64_t node = 0;
    if (!mt_parse(id, &node)) return HUB_MT_NOT_FOUND;
    matter_ctrl_node_t *nodes = heap_caps_malloc(MC_LIST_MAX * sizeof(*nodes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    mc_iv_t *iv = heap_caps_malloc(sizeof(*iv), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!nodes || !iv) {
        heap_caps_free(nodes);
        heap_caps_free(iv);
        return HUB_MT_BUSY;
    }
    const int n = matter_ctrl_nodes(nodes, MC_LIST_MAX);
    int rc = HUB_MT_NOT_FOUND;
    for (int i = 0; i < n && rc == HUB_MT_NOT_FOUND; i++) {
        if (nodes[i].node_id != node || !nodes[i].commissioned) continue;
        const bool have = matter_ctrl_node_iv(node, iv);
        hub_meta_ent_t meta;
        uint8_t id8[8];
        id8_of(node, id8);
        if (!hub_meta_lookup(HUB_META_PROTO_MATTER, id8, &meta, HUB_META_LOOKUP_MS)) memset(&meta, 0, sizeof(meta));
        char cfid[17];
        our_cfid(cfid);
        const ti_entry_t *te = cfid[0] ? ti_find_fabric_node(raw, l, cfid, node) : NULL;
        if (w) write_entry(w, &nodes[i], have ? iv : NULL, &meta, true, raw, te);
        rc = HUB_MT_DONE;
    }
    heap_caps_free(nodes);
    heap_caps_free(iv);
    return rc;
}

/* ---------------------------------------------------------------- 配网与移除的作业（M7 第三步） */


/* 一次只配一台（matter_ctrl 自己也拦）：名字 / 房间等配完再写进 hub_meta。只在受理时（调用方任务）写、在完成回调（CHIP 任务）读 */
static struct {
    uint32_t job;
    bool     has_name, has_room;
    char     name[HUB_NAME_MAX + 1];
    char     room[HUB_ROOM_MAX + 1];
} s_comm;

static void comm_done(void *ctx, const matter_ctrl_result_t *r)
{
    const uint32_t job = (uint32_t)(uintptr_t)ctx;
    if (!r->ok) {
        (void)hub_job_finish(job, r->err_code ? r->err_code : "internal", r->msg, NULL);
        return;
    }
    char id[20];
    mt_id(r->node_id, id);
    bool meta_ok = true;
    if (job == s_comm.job && (s_comm.has_name || s_comm.has_room)) {
        const hub_meta_change_t c = { .name = s_comm.has_name ? s_comm.name : NULL, .room = s_comm.has_room ? s_comm.room : NULL, .notify = -1 };
        meta_ok = hub_meta_update(id, &c, NULL) == HUB_META_OK;
    }
    char text[96];
    snprintf(text, sizeof(text), "A Matter device was added (%s).", id);
    (void)hub_event_emit(HUB_EVC_LIFECYCLE, id, "added", text, 0);      /* 进事件环；结论已经回给发起者，不另推 */
    char res[HJC_RESULT_MAX];
    hj_t w;
    hj_init(&w, res, sizeof(res));
    hj_obj_begin(&w);
    hj_kstr(&w, "id", id);
    if (r->vid) hj_kint(&w, "vendor_id", r->vid);
    if (r->pid) hj_kint(&w, "product_id", r->pid);
    if (!meta_ok) hj_kstr(&w, "detail", "Added, but the name or room could not be saved; set it again with hub.device.update.");
    hj_obj_end(&w);
    (void)hub_job_finish(job, NULL, NULL, hj_finish(&w) > 0 ? res : "{}");
}

int hub_matter_cmd_commission(hub_call_t *call)
{
    call->rl_refund = true;                                      /* 下面每个当场拒都发生在真去配网之前：限速令牌还回去（MX21）；受理了才清掉 */
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(call->params, "setup_code");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(call->params, "name");
    const cJSON *room = cJSON_GetObjectItemCaseSensitive(call->params, "room");
    char nm[HUB_NAME_MAX + 1] = "", rm[HUB_ROOM_MAX + 1] = "";
    /* 名字 / 房间先按 hub_meta 的规则验（6.8）：配完再说"名字不合法"就晚了 */
    if (cJSON_IsString(name) && hub_sanitize_user_name(name->valuestring, nm, sizeof(nm), HUB_NAME_MAX) != HUB_NAME_OK) {
        call->err_code = "invalid_params";
        snprintf(call->err_msg, sizeof(call->err_msg), "name must be one line of at most %d bytes", HUB_NAME_MAX);
        return -1;
    }
    if (cJSON_IsString(room) && hub_sanitize_user_name(room->valuestring, rm, sizeof(rm), HUB_ROOM_MAX) != HUB_NAME_OK) {
        call->err_code = "invalid_params";
        snprintf(call->err_msg, sizeof(call->err_msg), "room must be one line of at most %d bytes", HUB_ROOM_MAX);
        return -1;
    }
    const hub_cmd_t *cmd = hub_registry_find("hub.matter.commission");
    const uint32_t job = hub_job_start("hub.matter.commission", call->src, cmd ? cmd->timeout_ms : 180000);
    if (!job) {
        call->err_code = "busy";
        snprintf(call->err_msg, sizeof(call->err_msg), "the hub is busy with other long tasks; try again in a minute");
        return -1;
    }
    char why[200] = "";
    const esp_err_t e = matter_ctrl_commission(cJSON_IsString(code) ? code->valuestring : "", comm_done, (void *)(uintptr_t)job, why, sizeof(why));
    if (e != ESP_OK) {
        hub_job_abandon(job);                                    /* 当场就拒了：调用方当场回错误，作业不留结论、不另记审计 */
        call->err_code = e == ESP_ERR_INVALID_ARG ? "invalid_params" : e == ESP_ERR_NOT_SUPPORTED ? "unsupported"
                       : e == ESP_ERR_NO_MEM ? "limit_reached" : "unavailable";
        snprintf(call->err_msg, sizeof(call->err_msg), "%.159s", why[0] ? why : "the hub could not start adding the device");
        return -1;
    }
    call->rl_refund = false;
    s_comm.job = job;
    s_comm.has_name = cJSON_IsString(name) && nm[0];
    s_comm.has_room = cJSON_IsString(room) && rm[0];
    memcpy(s_comm.name, nm, sizeof(nm));
    memcpy(s_comm.room, rm, sizeof(rm));
    call->job = job;
    char jid[12];
    snprintf(jid, sizeof(jid), "%lu", (unsigned long)job);
    hj_obj_begin(call->out);
    hj_kstr(call->out, "job", jid);
    hj_kstr(call->out, "state", "running");
    hj_kstr(call->out, "detail", "Adding the device; this takes up to two minutes.");
    hj_obj_end(call->out);
    return 0;
}

static void remove_done(void *ctx, const matter_ctrl_result_t *r)
{
    const uint32_t job = (uint32_t)(uintptr_t)ctx;
    if (!r->ok) {
        (void)hub_job_finish(job, r->err_code ? r->err_code : "internal", r->msg, NULL);
        return;
    }
    char id[20];
    mt_id(r->node_id, id);
    /* 名字 / 房间 / 推送开关一并清掉（MC37）：清成默认 = 条目释放 */
    const hub_meta_change_t c = { .name = "", .room = "", .notify = 0, .sensitive = HUB_META_SENS_OFF };
    (void)hub_meta_update(id, &c, NULL);
    char text[96];
    snprintf(text, sizeof(text), "A Matter device was removed (%s).", id);
    (void)hub_event_emit(HUB_EVC_LIFECYCLE, id, "removed", text, 0);
    char res[HJC_RESULT_MAX];
    hj_t w;
    hj_init(&w, res, sizeof(res));
    hj_obj_begin(&w);
    hj_kstr(&w, "id", id);
    hj_kstr(&w, "detail", r->msg);
    hj_obj_end(&w);
    (void)hub_job_finish(job, NULL, NULL, hj_finish(&w) > 0 ? res : "{}");
}

esp_err_t hub_matter_remove(const char *id, bool force, uint32_t *job_out, char *why, size_t why_len)
{
    *job_out = 0;
    uint64_t node = 0;
    if (!mt_parse(id, &node)) {
        snprintf(why, why_len, "not a Matter device id (mt-<16 hex digits>)");
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t job = hub_job_start("hub.device.remove", HUB_SRC_REST, 90000);
    if (!job) {
        snprintf(why, why_len, "the hub is busy with other long tasks; try again in a minute");
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t e = matter_ctrl_remove(node, force, remove_done, (void *)(uintptr_t)job, why, why_len);
    if (e != ESP_OK) {
        hub_job_abandon(job);
        return e;
    }
    *job_out = job;
    return ESP_OK;
}

void hub_matter_init(void)
{
    static const matter_ctrl_hooks_t hooks = { .ident_load = hk_ident_load, .ident_save = hk_ident_save,
                                               .nodes_load = hk_nodes_load, .nodes_save = hk_nodes_save,
                                               .iv_load = hk_iv_load, .iv_save = hk_iv_save, .iv_erase = hk_iv_erase,
                                               .event = hk_event, .allow_test_load = hk_allow_test_load };
    matter_ctrl_set_hooks(&hooks);
}

#endif /* CONFIG_BITENGINE_ENABLE_MATTER_CTRL */
