/* hub_zigbee —— hub_core 这一侧的 Zigbee 设备（接口与规则见 include/hub_zigbee.h）。 */
#include "hub_zigbee.h"

#if CONFIG_BITENGINE_ENABLE_ZHUB

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hub_events.h"
#include "hub_meta.h"
#include "hub_model.h"
#include "hub_sanitize.h"
#include "hub_zcap.h"
#include "hub_store.h"
#include "zh_dev.h"
#include "zhub.h"

static const char *TAG = "hub_zigbee";

#define LOAD_WAIT_MS   500                      /* 开机加载每条先等 hub_store 这么久（另加投递最多 1 s；zhub.c 每条之前喂一次看门狗） */
#define LOAD_RETRY_MS  3000                     /* 超时了用这个期限再试一次（hub_store 开机时可能正忙着别的，500 ms 偏短；第一轮审计） */
#define LOAD_BUDGET_US (20LL * 1000 * 1000)     /* 整个开机加载最多花这么久：超了剩下的格子一律保留（不读、不分配），zhub 别为等存储迟迟不接管网络 */

static void key_of(unsigned idx, char key[8]) { snprintf(key, 8, "d%02u", idx); }

static void id_of(const uint8_t ieee[8], char id[24])
{
    snprintf(id, 24, "zb-%02x%02x%02x%02x%02x%02x%02x%02x", ieee[7], ieee[6], ieee[5], ieee[4], ieee[3], ieee[2], ieee[1], ieee[0]);
}

/* ---------------------------------------------------------------- 钩子（都在 zhub 任务里调） */

static bool s_store_down;                          /* hub_store 这次开机没起来：开机加载只报一次，别 64 条各报一行 */
static bool s_store_stuck;                         /* 重试也超时 / 超了预算：剩下的格子直接保留，不再一条条等 */
static int64_t s_t_load0;                          /* 开机加载第一条的时刻 */

/* 三态（第二轮审计 ❌）："读不出来"不是"没有" —— 当成空格的话，新学到的设备会落进来、把存储里那条盖掉 */
static zhub_load_t hk_load(unsigned idx, zh_dev_rec_t *out)
{
    if (s_store_down) return ZHUB_LOAD_EMPTY;
    if (!s_t_load0) s_t_load0 = esp_timer_get_time();
    if (!s_store_stuck && esp_timer_get_time() - s_t_load0 > LOAD_BUDGET_US) {
        s_store_stuck = true;
        ESP_LOGE(TAG, "开机加载超过 %lld s：从 d%02u 起的格子这次开机保留（不读、不分配给新设备）", LOAD_BUDGET_US / 1000000, idx);
    }
    if (s_store_stuck) return ZHUB_LOAD_ERR;
    char key[8];
    key_of(idx, key);
    uint8_t ver = 0;
    size_t len = sizeof(*out);
    esp_err_t e = hub_store_get(HUB_STORE_NS_ZHUB, key, ZH_DEV_REC_VER, &ver, out, &len, LOAD_WAIT_MS);
    if (e == ESP_ERR_TIMEOUT) {
        len = sizeof(*out);
        e = hub_store_get(HUB_STORE_NS_ZHUB, key, ZH_DEV_REC_VER, &ver, out, &len, LOAD_RETRY_MS);
        if (e == ESP_ERR_TIMEOUT) {
            s_store_stuck = true;
            ESP_LOGE(TAG, "hub_store 读 %s 重试也超时：从这一格起这次开机保留（不读、不分配给新设备）", key);
            return ZHUB_LOAD_ERR;
        }
    }
    if (e == ESP_ERR_NOT_FOUND) return ZHUB_LOAD_EMPTY;
    if (e == ESP_ERR_INVALID_STATE) {              /* 存储整个没起来：写也写不进去，不会盖掉什么，不必保留 */
        s_store_down = true;
        ESP_LOGE(TAG, "hub_store 没起来：设备表这次开机不读回（设备靠被动学习再认出来），改动也落不了盘");
        return ZHUB_LOAD_EMPTY;
    }
    if (e == ESP_OK && ver == ZH_DEV_REC_VER && len == sizeof(*out)) return ZHUB_LOAD_OK;
    if (e == ESP_OK && ver == ZH_DEV_REC_VER) {    /* 版本对、长度不对：坏记录，盖掉无妨 */
        ESP_LOGW(TAG, "设备记录 %s 长度不对（%u B），当空格用", key, (unsigned)len);
        return ZHUB_LOAD_EMPTY;
    }
    /* 超时 / 读错 / 存的格式比固件新（OTA 回滚后，hub_store 回 INVALID_VERSION）/ 更旧的格式（迁移随第一次改格式）：保留 */
    ESP_LOGW(TAG, "设备记录 %s 读不出来（%s、版本 %u）：这次开机保留这一格", key, esp_err_to_name(e), (unsigned)ver);
    return ZHUB_LOAD_ERR;
}

static void hk_save(unsigned idx, const zh_dev_rec_t *rec)
{
    char key[8];
    key_of(idx, key);
    /* 不等队列：zhub 任务里还跑着 ASH，hub_store 卡住时别把它也拖住（队列 16 条，满了说明 hub_store 本身出了事） */
    const esp_err_t e = hub_store_put_wait(HUB_STORE_NS_ZHUB, key, ZH_DEV_REC_VER, rec, sizeof(*rec), 0);
    if (e != ESP_OK) ESP_LOGW(TAG, "设备记录 %s 没排上写（%s）：内存里照常，重启后要靠被动学习再认出来", key, esp_err_to_name(e));
}

/* 设备事件。推送只有一种：这次开机经入网窗口新加的设备访谈完了（或访谈失败）—— 附录 A："New devices are reported when they finish joining"，
   这时说得出它是什么（Muse 再用 hub.device.get 看厂家 / 型号）。其余只进事件环：被动学习认出的现网设备不推（换固件后第一次开机会一下子认出好几台），
   新记录本身也不推（还说不出是什么）。类别：新记录 / 访谈是 LIFECYCLE，离开 / 回来是 AVAILABILITY（设计 8.6 的 DEVICE_OFFLINE 归 6.7 的 AVAILABILITY，
   与 thread_member_left 同类；但那条推送、这条暂不推）。事件文字里绝不放设备自报的字符串（6.8：厂家 / 型号只进专门的字段） */
/* 设备的元数据（名字 / 房间 / 推送开关，hub.device.update；M5 第二步）。zhub 任务里只等 META_LOCK_ZHUB_MS（它还跑着 ASH；拿不到按没名字，hub_meta 计 lookup_busy） */
#define META_LOCK_ZHUB_MS 20u
static void meta_of(const uint8_t ieee[8], hub_meta_ent_t *m, uint32_t lock_ms)
{
    uint8_t id8[8];
    hub_meta_id8_from_ieee(ieee, id8);
    (void)hub_meta_lookup(HUB_META_PROTO_ZIGBEE, id8, m, lock_ms);   /* 没有 / 拿不到：*m 清零 = 没名字 */
}

/* 事件句子里的设备名（6.7 模板，规则与真值表在 hub_meta_core 的 hub_meta_event_name）：起了名字 "名字" (房间)，没起名 unnamed <设备类> <末四位>。
   不放设备自报的字符串（6.8） */
static void name_of(const zhub_dev_view_t *v, const hub_meta_ent_t *m, char *out, size_t cap)
{
    const char *kind = hub_zcap_kind(&v->rec, hub_zcap_caps(&v->rec));
    (void)hub_meta_event_name(m, HUB_META_PROTO_ZIGBEE, kind, (uint16_t)(v->rec.ieee[1] << 8 | v->rec.ieee[0]), out, cap);
}

/* 句首用：没起名的 "unnamed …" 首字母大写（起了名字的以引号开头，不动） */
static void cap_first(char *s)
{
    if (s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 'a' + 'A');
}

static void emit_ev(hub_evt_class_t cls, const char *id, const char *kind, const char *text, unsigned flags)
{
    const esp_err_t e = hub_event_emit(cls, id, kind, text, flags);
    if (e != ESP_OK) ESP_LOGW(TAG, "事件 %s 没进事件环（%s）", kind, esp_err_to_name(e));
}

/* 报警类读数翻转（M5 第一步）。类别照 6.7：漏水 / 烟雾 / 燃气 / CO 是 ALARM（恒推，检测到与解除成对）；ZoneType 不认识 / 没读到的 IAS 报警
   也按 ALARM（6.7 把"IAS 报警"列在 ALARM：漏水传感器访谈时没读到 ZoneType，漏水不能只进环 —— 两轮审计 ❌）；防拆是 ALARM（只有 0 → 1）；
   人体 / 门磁 / 振动是 SECURITY（照常带推送标志；推不推 = 类别位 AND 设备 notify 位，两样都在 hub_event_emit 里判 —— 设备位由 hub.device.update 设，默认关只进环）；电量低是 BATTERY。
   人体 / 振动只报"检测到"（每次都报，2026-10-24 起不限间隔）；门磁开、关都报 */
static void state_event(const zhub_dev_view_t *v, const char *id)
{
    hub_meta_ent_t m;
    meta_of(v->rec.ieee, &m, META_LOCK_ZHUB_MS);
    char name[HUB_META_EVNAME_MAX + 1], when[16] = "", hhmm[6], t[160];
    name_of(v, &m, name, sizeof(name));
    char cname[sizeof(name)];                      /* 句首用 */
    snprintf(cname, sizeof(cname), "%s", name);
    cap_first(cname);
    hub_events_local_clock(hhmm);
    if (hhmm[0]) snprintf(when, sizeof(when), " at %s", hhmm);
    const unsigned sec_flags = HEV_F_PUSH;         /* 设备 notify 位在 hub_event_emit 里判（默认关 = 只进环） */
    const unsigned alarm_flags = HEV_F_PUSH;
    const uint8_t ev = v->st_ev;
    const uint16_t zt = zh_dev_zone_type(&v->rec);   /* IAS 看记录、涂鸦看表（M5 第八步） */
    if (ev & (ZH_STEV_ALARM_ON | ZH_STEV_ALARM_OFF)) {
        const bool on = (ev & ZH_STEV_ALARM_ON) != 0;
        switch (zt) {
        case ZH_ZONE_MOTION:
            if (on) {
                snprintf(t, sizeof(t), "Motion detected by %s%s.", name, when);
                emit_ev(HUB_EVC_SECURITY, id, "motion_detected", t, sec_flags);
            }
            break;
        case ZH_ZONE_CONTACT:
            snprintf(t, sizeof(t), "%s %s%s.", cname, on ? "opened" : "closed", when);
            emit_ev(HUB_EVC_SECURITY, id, on ? "contact_open" : "contact_closed", t, sec_flags);
            break;
        case ZH_ZONE_WATER:
            snprintf(t, sizeof(t), on ? "Water leak detected by %s%s." : "Water leak cleared on %s%s.", name, when);
            emit_ev(HUB_EVC_ALARM, id, on ? "water_leak" : "water_leak_cleared", t, alarm_flags);
            break;
        case ZH_ZONE_FIRE:
            snprintf(t, sizeof(t), on ? "Smoke detected by %s%s." : "Smoke alarm cleared on %s%s.", name, when);
            emit_ev(HUB_EVC_ALARM, id, on ? "smoke_detected" : "smoke_cleared", t, alarm_flags);
            break;
        case ZH_ZONE_GAS:
            snprintf(t, sizeof(t), on ? "Gas detected by %s%s." : "Gas alarm cleared on %s%s.", name, when);
            emit_ev(HUB_EVC_ALARM, id, on ? "gas_detected" : "gas_cleared", t, alarm_flags);
            break;
        case ZH_ZONE_CO:
            snprintf(t, sizeof(t), on ? "Carbon monoxide detected by %s%s." : "Carbon monoxide alarm cleared on %s%s.", name, when);
            emit_ev(HUB_EVC_ALARM, id, on ? "co_detected" : "co_cleared", t, alarm_flags);
            break;
        case ZH_ZONE_VIBRATION:
            if (on) {
                snprintf(t, sizeof(t), "Vibration detected by %s%s.", name, when);
                emit_ev(HUB_EVC_SECURITY, id, "vibration", t, sec_flags);
            }
            break;
        default:                                   /* ZoneType 不认识 / 没读到：只说有报警，按 ALARM 推 */
            if (on) {
                snprintf(t, sizeof(t), "%s raised an alarm%s.", cname, when);
                emit_ev(HUB_EVC_ALARM, id, "sensor_alarm", t, HEV_F_PUSH);
            }
            break;
        }
    }
    if ((ev & ZH_STEV_OCC_ON) && zt != ZH_ZONE_MOTION) {   /* 同一台既有 IAS 人体又有占用：只报 IAS 那条 */
        snprintf(t, sizeof(t), "Motion detected by %s%s.", name, when);
        emit_ev(HUB_EVC_SECURITY, id, "motion_detected", t, sec_flags);
    }
    if (ev & ZH_STEV_TAMPER) {
        snprintf(t, sizeof(t), "Tamper alarm on %s%s.", name, when);
        emit_ev(HUB_EVC_ALARM, id, "tamper", t, HEV_F_PUSH);   /* 只有 0 → 1、没有解除 */
    }
    if (ev & ZH_STEV_BATTERY_LOW) {
        snprintf(t, sizeof(t), "Low battery on %s.", name);
        emit_ev(HUB_EVC_BATTERY, id, "battery_low", t, HEV_F_PUSH);
    }
}

static void hk_event(zh_dev_ev_t ev, zh_dev_src_t src, const zhub_dev_view_t *v)
{
    if (ev == ZH_DEV_EV_STATE) {
        char sid[24];
        id_of(v->rec.ieee, sid);
        state_event(v, sid);
        return;
    }
    if (ev != ZH_DEV_EV_ADDED && ev != ZH_DEV_EV_LEFT && ev != ZH_DEV_EV_BACK && ev != ZH_DEV_EV_IDENTIFIED && ev != ZH_DEV_EV_IV_FAILED &&
        ev != ZH_DEV_EV_OFFLINE && ev != ZH_DEV_EV_ONLINE) {
        return;                                    /* 短地址变了：不是用户关心的事 */
    }
    char id[24], t[HEV_TEXT_MAX];
    id_of(v->rec.ieee, id);
    /* 句子里的设备名照 6.7 模板（M5 第二步，第一轮审计）：起了名字用名字，新入网的还没名字就是 unnamed <设备类> <末四位>（访谈完才知道设备类） */
    hub_meta_ent_t m;
    meta_of(v->rec.ieee, &m, META_LOCK_ZHUB_MS);
    char name[HUB_META_EVNAME_MAX + 1], cname[HUB_META_EVNAME_MAX + 1];
    name_of(v, &m, name, sizeof(name));
    snprintf(cname, sizeof(cname), "%s", name);
    cap_first(cname);
    const bool join = src == ZH_DEV_SRC_JOIN;      /* 只有信任中心放行的新入网才算"新加的"；宣告 / 重新入网 / 被动学习建出来的都是已在网的（新入网时入网回调先于宣告到） */
    const char *kind = NULL;
    hub_evt_class_t cls = HUB_EVC_LIFECYCLE;
    unsigned flags = 0;
    switch (ev) {
    case ZH_DEV_EV_ADDED:
        kind = join ? "zigbee_device_added" : "zigbee_device_found";
        if (join) snprintf(t, sizeof(t), "New Zigbee device joined: %s (not identified yet).", name);
        else snprintf(t, sizeof(t), "Found a Zigbee device already on the network: %s (not identified yet).", name);
        break;
    case ZH_DEV_EV_LEFT:
        kind = "zigbee_device_left";
        cls = HUB_EVC_AVAILABILITY;
        snprintf(t, sizeof(t), "%s left the Zigbee network.", cname);
        break;
    case ZH_DEV_EV_BACK:
        kind = "zigbee_device_back";
        cls = HUB_EVC_AVAILABILITY;
        snprintf(t, sizeof(t), "%s is back on the Zigbee network.", cname);
        break;
    case ZH_DEV_EV_OFFLINE: {                      /* 8.10：常电 3 次 ping 没回 / 电池 25 h 没消息；模板照 6.7（"… has been offline for 2 h (last seen 12:01)."） */
        kind = "zigbee_offline";
        cls = HUB_EVC_AVAILABILITY;
        flags = HEV_F_PUSH;
        const uint32_t min = v->age_ms / 60000u;
        if (min >= 120u) snprintf(t, sizeof(t), "%s is offline: nothing heard from it for %u h.", cname, (unsigned)(min / 60u));
        else snprintf(t, sizeof(t), "%s is offline: it stopped answering (nothing heard for %u min).", cname, (unsigned)(min ? min : 1u));
        break;
    }
    case ZH_DEV_EV_ONLINE:
        kind = "zigbee_online";
        cls = HUB_EVC_AVAILABILITY;
        flags = HEV_F_PUSH;
        snprintf(t, sizeof(t), "%s is back online.", cname);
        break;
    case ZH_DEV_EV_IDENTIFIED:
        kind = "zigbee_identified";               /* 事件名 < HEV_KIND_MAX（24）：超了 hub_event_emit 整条拒收（修复复核 ❌） */
        if (join) snprintf(t, sizeof(t), "New Zigbee device paired and identified: %s. hub.device.get shows what it reported about itself.", name);
        else snprintf(t, sizeof(t), "Identified a Zigbee device already on the network: %s. hub.device.get shows what it reported about itself.", name);
        if (join) flags = HEV_F_PUSH;
        break;
    case ZH_DEV_EV_IV_FAILED:
    default:
        kind = "zigbee_unidentified";
        if (join) snprintf(t, sizeof(t), "New Zigbee device joined but could not be identified yet: %s. It stays listed; the hub will try again later.", name);
        else snprintf(t, sizeof(t), "Could not identify a Zigbee device already on the network: %s. It stays listed; the hub will try again later.", name);
        if (join) flags = HEV_F_PUSH;
        break;
    }
    emit_ev(cls, id, kind, t, flags);
}

void hub_zigbee_init(void)
{
    static const zhub_hooks_t hooks = { .load = hk_load, .save = hk_save, .event = hk_event };
    zhub_set_hooks(&hooks);
}

/* ---------------------------------------------------------------- 列表项 */

static const char *node_type_str(uint8_t t)
{
    switch (t) {
    case ZH_NODE_ROUTER:     return "router";
    case ZH_NODE_END_DEVICE: return "end_device";
    default:                 return "unknown";
    }
}

static const char *iv_str(const zhub_dev_view_t *v)
{
    /* 认出来了就是 done：之后的补 CIE / 登记（M5 第四步）与绑定 / 上报配置（第五步）都是认出来之后的事，登记进度另看 zigbee.ias_enrollment */
    if ((v->rec.flags & ZH_DEVF_INTERVIEWED) && v->iv != ZH_IV_FAILED) return "done";
    if (v->iv == ZH_IV_FAILED) return "failed";
    if (v->iv >= ZH_IV_NODE && v->iv <= ZH_IV_REPORT) return v->iv_started ? "running" : "pending";
    return "pending";                              /* 离开了、访谈停着 */
}

/* 设备自报的字符串（6.8）：净化后只放进专门的字段；空的不写 */
static void put_dev_str(hj_t *w, const char *key, const char *raw)
{
    char clean[HUB_DEV_STR_MAX + 1];
    const size_t n = hub_sanitize_device_str(raw, strnlen(raw, 24), clean, sizeof(clean), HUB_DEV_STR_MAX);
    if (n) hj_kstr(w, key, clean);
}

/* 有端点登记了 IAS Zone 输入簇（ZoneType 只对它有意义；第七步 a 的旧记录 zone_type 是 0，第二轮审计） */
static bool has_ias(const zh_dev_rec_t *r)
{
    for (unsigned k = 0; k < r->n_ep && k < ZH_DEV_EP_REC; k++) {
        const unsigned n_in = r->ep[k].n_in < ZH_DEV_EP_CL ? r->ep[k].n_in : ZH_DEV_EP_CL;
        for (unsigned j = 0; j < n_in; j++)
            if (r->ep[k].cl[j] == ZH_CL_IAS_ZONE) return true;
    }
    return false;
}

static void put_hex16(hj_t *w, const char *key, uint16_t v)
{
    char b[8];
    snprintf(b, sizeof(b), "0x%04X", (unsigned)v);
    hj_kstr(w, key, b);
}

/* 读数（M5 第一步）。键名照附录 A 的例子（contact "open" / "closed"、onoff、level_pct、battery_pct），量值带单位后缀。
   ages = true 时写同样的键、值是年龄（秒），放在 state_age_s 里。IAS 的那一个读数按 ZoneType 起名 */
static const char *zone_key(uint16_t zt)
{
    switch (zt) {
    case ZH_ZONE_MOTION:    return "motion";
    case ZH_ZONE_CONTACT:   return "contact";
    case ZH_ZONE_WATER:     return "water_leak";
    case ZH_ZONE_FIRE:      return "smoke";
    case ZH_ZONE_GAS:       return "gas";
    case ZH_ZONE_CO:        return "co";
    case ZH_ZONE_VIBRATION: return "vibration";
    default:                return "alarm";
    }
}

static void write_state(hj_t *w, const zhub_dev_view_t *v, bool ages)
{
    const zh_state_t *s = &v->st;
    const uint16_t zt = zh_dev_zone_type(&v->rec);   /* IAS 看记录、涂鸦看表（M5 第八步） */
#define ST_HAS(f) ((s->have & (1u << (f))) != 0)
#define ST_AGE(k, f) hj_kuint(w, (k), v->st_age_ms[(f)] / 1000u)
    hj_key(w, ages ? "state_age_s" : "state");
    hj_obj_begin(w);
    bool motion_done = false;
    if (ST_HAS(ZH_ST_ZONE)) {
        const char *k = zone_key(zt);
        const bool a1 = (s->zone_status & ZH_IAS_ST_ALARM1) != 0;
        if (ages) ST_AGE(k, ZH_ST_ZONE);
        else if (zt == ZH_ZONE_CONTACT) hj_kstr(w, "contact", a1 ? "open" : "closed");
        else hj_kbool(w, k, a1);
        motion_done = zt == ZH_ZONE_MOTION;
        if (s->zone_status & ZH_IAS_ST_TAMPER) {
            if (ages) ST_AGE("tamper", ZH_ST_ZONE);
            else hj_kbool(w, "tamper", true);
        }
        if (s->zone_status & ZH_IAS_ST_BATTERY) {
            if (ages) ST_AGE("battery_low", ZH_ST_ZONE);
            else hj_kbool(w, "battery_low", true);
        }
    }
    if (ST_HAS(ZH_ST_OCCUPANCY) && !motion_done) {   /* 占用传感器的"有人"也叫 motion（别和 IAS 人体重复出一个键） */
        if (ages) ST_AGE("motion", ZH_ST_OCCUPANCY);
        else hj_kbool(w, "motion", s->occupancy != 0);
    }
    if (ST_HAS(ZH_ST_BATTERY_PCT)) { if (ages) ST_AGE("battery_pct", ZH_ST_BATTERY_PCT); else hj_kuint(w, "battery_pct", s->battery_pct); }
    if (ST_HAS(ZH_ST_BATTERY_MV)) { if (ages) ST_AGE("battery_mv", ZH_ST_BATTERY_MV); else hj_kuint(w, "battery_mv", s->battery_mv); }
    if (ST_HAS(ZH_ST_TEMP)) { if (ages) ST_AGE("temperature_c", ZH_ST_TEMP); else hj_kfixed(w, "temperature_c", s->temp_c100, 2); }
    if (ST_HAS(ZH_ST_HUMIDITY)) { if (ages) ST_AGE("humidity_pct", ZH_ST_HUMIDITY); else hj_kfixed(w, "humidity_pct", s->hum_c100, 2); }
    if (ST_HAS(ZH_ST_PRESSURE)) { if (ages) ST_AGE("pressure_hpa", ZH_ST_PRESSURE); else hj_kfixed(w, "pressure_hpa", s->pressure_hpa10, 1); }
    if (ST_HAS(ZH_ST_LUX)) { if (ages) ST_AGE("illuminance_lux", ZH_ST_LUX); else hj_kuint(w, "illuminance_lux", s->lux); }
    if (ST_HAS(ZH_ST_ONOFF)) { if (ages) ST_AGE("onoff", ZH_ST_ONOFF); else hj_kbool(w, "onoff", s->onoff != 0); }
    if (ST_HAS(ZH_ST_LEVEL)) { if (ages) ST_AGE("level_pct", ZH_ST_LEVEL); else hj_kuint(w, "level_pct", s->level_pct); }
#undef ST_HAS
#undef ST_AGE
    hj_obj_end(w);
}

/* 常电（随时收）：宣告 / 节点描述符说空闲时也收，或者是路由器（与 zh_dev_ctl_start 的判据一致） */
static bool hub_zigbee_rx_on(const zh_dev_rec_t *r)
{
    return (r->flags & (ZH_DEVF_ANNCED | ZH_DEVF_RX_ON)) == (ZH_DEVF_ANNCED | ZH_DEVF_RX_ON) || r->node_type == ZH_NODE_ROUTER;
}

static void write_entry(hj_t *w, const zhub_dev_view_t *v, const hub_meta_ent_t *meta, bool full)
{
    const zh_dev_rec_t *r = &v->rec;
    char id[24], nwk[8];
    id_of(r->ieee, id);
    snprintf(nwk, sizeof(nwk), "0x%04X", (unsigned)r->nwk);
    const bool left = (r->flags & ZH_DEVF_LEFT) != 0;
    const bool identified = (r->flags & ZH_DEVF_INTERVIEWED) != 0;
    const uint32_t age_s = v->age_ms / 1000u;
    hj_obj_begin(w);
    hj_kstr(w, "id", id);
    hj_kstr(w, "protocol", "zigbee");
    hj_kstr(w, "name", meta->name);                /* 用户起的（hub.device.update；存的时候净化过、读回时又过一遍，6.8） */
    hj_kstr(w, "room", meta->room);
    const uint32_t caps = hub_zcap_caps(r);
    hj_kstr(w, "kind", hub_zcap_kind(r, caps));    /* 8.8：按访谈记下的簇与 ZoneType 推（hub_zcap） */
    if (identified || zh_dev_lumi_dev(r) >= 0) {   /* 小米没访谈完也有型号（从主动上报学的，M5 第十步） */
        put_dev_str(w, "manufacturer", r->manuf);
        put_dev_str(w, "model", r->model);
    }
    hj_key(w, "capabilities");
    hj_arr_begin(w);
    for (unsigned b = 0; b < HUB_CAP_COUNT; b++)
        if (caps & (1u << b)) hj_str(w, hub_cap_name(1u << b));
    hj_arr_end(w);
    /* 控制路径（8.11，M5 第七步）：控制路径驱动得了开关 / 亮度（ZCL 的簇，或涂鸦表里有写法，M5 第九步）就算可控；
       休眠设备也算（M5 第十二步：排进待发，它醒来时补发，回 pending） */
    hj_kbool(w, "controllable", (hub_zcap_ctl_caps(r) & (HUB_CAP_ONOFF | HUB_CAP_LEVEL)) != 0);
    hj_kbool(w, "online", !left && !v->offline);   /* 可用性由 zhub 判（8.10：常电 ping、电池 25 h；M5 第十一步） */
    if (v->seen) hj_kuint(w, "last_seen_s", age_s);
    write_state(w, v, false);
    write_state(w, v, true);                       /* 每个读数的年龄：缺的键 = 这次开机还没听到，别当成 false / 0（第二轮审计；get 的说明"every reading with its age"） */
    if (v->seen && v->rx) {
        hj_key(w, "link");
        hj_obj_begin(w);
        hj_kuint(w, "lqi", v->lqi);
        hj_kint(w, "rssi", v->rssi);
        hj_obj_end(w);
    }
    hj_kbool(w, "notify", (meta->flags & HUB_META_F_NOTIFY) != 0);
    hj_kbool(w, "sensitive", (meta->flags & HUB_META_F_SENSITIVE) != 0);
    hj_key(w, "zigbee");
    hj_obj_begin(w);
    hj_kstr(w, "nwk", nwk);
    hj_kstr(w, "node_type", node_type_str(r->node_type));
    if (r->flags & ZH_DEVF_ANNCED) hj_kbool(w, "sleepy", !(r->flags & ZH_DEVF_RX_ON));
    hj_kbool(w, "identified", identified);
    hj_kstr(w, "interview", iv_str(v));
    if (left) hj_kbool(w, "left", true);
    if (full) {
        if (r->flags & (ZH_DEVF_ANNCED | ZH_DEVF_POWER)) hj_kbool(w, "mains_powered", (r->flags & ZH_DEVF_MAINS) != 0);
        hj_kuint(w, "messages", v->rx);
        if (identified) {
            if (r->flags & ZH_DEVF_NODE_DESC) put_hex16(w, "manufacturer_code", r->manuf_code);   /* 节点描述符没拿到时没有（0x0000 本身也是个厂商码） */
            if (r->zone_type != ZH_ZONE_TYPE_UNKNOWN && has_ias(r)) put_hex16(w, "ias_zone_type", r->zone_type);
            /* IAS 登记（M5 第四步）：pending = CIE 地址 / 登记还欠着或正在补（设备没登记上不会报警）；done = 这台网关确认过、或设备明确拒了、或访谈时没走这一步（旧固件下访谈的） */
            if (has_ias(r)) hj_kstr(w, "ias_enrollment", ((r->flags & ZH_DEVF_CIE_OWED) || v->iv == ZH_IV_CIE || v->iv == ZH_IV_ENROLL) ? "pending" : "done");
            hj_key(w, "endpoints");
            hj_arr_begin(w);
            for (unsigned k = 0; k < r->n_ep && k < ZH_DEV_EP_REC; k++) {
                const struct zh_dev_ep *e = &r->ep[k];
                const unsigned n_in = e->n_in < ZH_DEV_EP_CL ? e->n_in : ZH_DEV_EP_CL;
                const unsigned n_out = e->n_out < ZH_DEV_EP_CL - n_in ? e->n_out : ZH_DEV_EP_CL - n_in;
                hj_obj_begin(w);
                hj_kuint(w, "endpoint", e->ep);
                put_hex16(w, "profile", e->profile);
                put_hex16(w, "device_id", e->devid);
                hj_key(w, "in_clusters");
                hj_arr_begin(w);
                for (unsigned j = 0; j < n_in; j++) {
                    char b[8];
                    snprintf(b, sizeof(b), "0x%04X", (unsigned)e->cl[j]);
                    hj_str(w, b);
                }
                hj_arr_end(w);
                hj_key(w, "out_clusters");
                hj_arr_begin(w);
                for (unsigned j = 0; j < n_out; j++) {
                    char b[8];
                    snprintf(b, sizeof(b), "0x%04X", (unsigned)e->cl[n_in + j]);
                    hj_str(w, b);
                }
                hj_arr_end(w);
                if (e->n_in + e->n_out > ZH_DEV_EP_CL) hj_kbool(w, "clusters_truncated", true);
                hj_obj_end(w);
            }
            hj_arr_end(w);
        }
    }
    hj_obj_end(w);
    hj_obj_end(w);
}


/* 设备表快照（PSRAM；64 × 约 300 B ≈ 19 KB：每条带整条记录与读数）。拿不到（内存 / zhub 的锁忙）回 -1：调用方回"忙"，别说成没有设备（第二轮审计） */
static int snapshot(zhub_dev_view_t **out)
{
    *out = heap_caps_malloc(ZH_DEV_MAX * sizeof(zhub_dev_view_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!*out) return -1;
    const int n = zhub_dev_list(*out, ZH_DEV_MAX);
    if (n < 0) {
        heap_caps_free(*out);
        *out = NULL;
    }
    return n;
}

long hub_zigbee_count(void)
{
    zhub_dev_view_t *v = NULL;
    const int n = snapshot(&v);
    heap_caps_free(v);
    return n;
}

int hub_zigbee_write_list(hj_t *w, unsigned long start, unsigned long *next, unsigned long *n_out, uint32_t cap_filter, const char *room)
{
    zhub_dev_view_t *v = NULL;
    const int n = snapshot(&v);
    if (n < 0) return HUB_ZB_BUSY;
    int rc = HUB_ZB_DONE;
    unsigned long k = 0;                           /* 过滤后的序号（游标按它算） */
    for (int i = 0; i < n; i++) {
        if (cap_filter && !(hub_zcap_caps(&v[i].rec) & cap_filter)) continue;
        hub_meta_ent_t meta;
        meta_of(v[i].rec.ieee, &meta, HUB_META_LOOKUP_MS);
        if (room && strcasecmp(meta.room, room) != 0) continue;   /* 附录 A："case-insensitive"（只折叠 ASCII 字母） */
        if (k++ < start || rc != HUB_ZB_DONE) continue;   /* 写不下以后接着数完，n_out 要的是总数 */
        hj_mark_t m = hj_mark(w);
        write_entry(w, &v[i], &meta, false);
        if (w->truncated) {
            hj_rollback(w, m);
            if (next) *next = k - 1;
            rc = HUB_ZB_TRUNCATED;
        }
    }
    if (n_out) *n_out = k;
    heap_caps_free(v);
    return rc;
}

int hub_zigbee_write_device(hj_t *w, const char *id)
{
    if (!id || strncasecmp(id, "zb-", 3) != 0 || strlen(id) != 19) return HUB_ZB_NOT_FOUND;
    zhub_dev_view_t *v = NULL;
    const int n = snapshot(&v);
    if (n < 0) return HUB_ZB_BUSY;
    int rc = HUB_ZB_NOT_FOUND;
    for (int i = 0; i < n && rc == HUB_ZB_NOT_FOUND; i++) {
        char vid[24];
        id_of(v[i].rec.ieee, vid);
        if (strcasecmp(vid, id) == 0) {
            if (w) {
                hub_meta_ent_t meta;
                meta_of(v[i].rec.ieee, &meta, HUB_META_LOOKUP_MS);
                write_entry(w, &v[i], &meta, true);
            }
            rc = HUB_ZB_DONE;
        }
    }
    heap_caps_free(v);
    return rc;
}

/* 控制前查设备（M5 第七步）：IEEE、能力、常电、敏感标记（hub_meta，6.2：只给网页改）。找到回 HUB_ZB_DONE */
int hub_zigbee_ctl_info(const char *id, uint8_t ieee[8], uint32_t *caps, uint32_t *ctl_caps, bool *rx_on, bool *sensitive)
{
    if (!id || strncasecmp(id, "zb-", 3) != 0 || strlen(id) != 19) return HUB_ZB_NOT_FOUND;
    zhub_dev_view_t *v = NULL;
    const int n = snapshot(&v);
    if (n < 0) return HUB_ZB_BUSY;
    int rc = HUB_ZB_NOT_FOUND;
    for (int i = 0; i < n && rc == HUB_ZB_NOT_FOUND; i++) {
        char vid[24];
        id_of(v[i].rec.ieee, vid);
        if (strcasecmp(vid, id) != 0) continue;
        if (ieee) memcpy(ieee, v[i].rec.ieee, 8);
        if (caps) *caps = hub_zcap_caps(&v[i].rec);         /* 列表里报的能力：权限 / 能力检查照它（报错要和列表说的一致） */
        if (ctl_caps) *ctl_caps = hub_zcap_ctl_caps(&v[i].rec);   /* 控制路径现在真能驱动的（涂鸦的开关 / 亮度随第二阶段） */
        if (rx_on) *rx_on = hub_zigbee_rx_on(&v[i].rec);
        rc = HUB_ZB_DONE;
        if (sensitive) {
            /* 查不了（表没起来 / 锁忙）别当成"不敏感"放行：回忙，让调用方过会儿再试（审计：放行方向的失败） */
            uint8_t id8[8];
            hub_meta_ent_t m;
            hub_meta_id8_from_ieee(v[i].rec.ieee, id8);
            const int lk = hub_meta_lookup_ex(HUB_META_PROTO_ZIGBEE, id8, &m, HUB_META_LOOKUP_MS);
            if (lk == HUB_META_LK_UNKNOWN) rc = HUB_ZB_BUSY;
            *sensitive = lk == HUB_META_LK_FOUND && (m.flags & HUB_META_F_SENSITIVE) != 0;
        }
    }
    heap_caps_free(v);
    return rc;
}

/* 控制之后回读：写 "state" 对象（同列表项的读数键）。找到回 HUB_ZB_DONE */
int hub_zigbee_write_state(hj_t *w, const char *id)
{
    if (!w || !id || strncasecmp(id, "zb-", 3) != 0 || strlen(id) != 19) return HUB_ZB_NOT_FOUND;
    zhub_dev_view_t *v = NULL;
    const int n = snapshot(&v);
    if (n < 0) return HUB_ZB_BUSY;
    int rc = HUB_ZB_NOT_FOUND;
    for (int i = 0; i < n && rc == HUB_ZB_NOT_FOUND; i++) {
        char vid[24];
        id_of(v[i].rec.ieee, vid);
        if (strcasecmp(vid, id) != 0) continue;
        write_state(w, &v[i], false);
        rc = HUB_ZB_DONE;
    }
    heap_caps_free(v);
    return rc;
}

#endif /* CONFIG_BITENGINE_ENABLE_ZHUB */
