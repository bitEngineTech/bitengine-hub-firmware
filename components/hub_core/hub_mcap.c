/* hub_mcap —— Matter 设备的能力与设备类别（见 include/hub_mcap.h）。纯逻辑，test_host/hub_mcap_test 原样编。 */
#include "hub_mcap.h"

#include <stdbool.h>
#include "hub_model.h"

/* 设备类型（Matter 1.4 Device Library） */
enum {
    DT_CONTACT = 0x0015, DT_WATER_LEAK = 0x0043, DT_RAIN = 0x0044, DT_SMOKE_CO = 0x0076,
    DT_ONOFF_LIGHT = 0x0100, DT_DIMMABLE_LIGHT = 0x0101, DT_CT_LIGHT = 0x010C, DT_EXT_COLOR_LIGHT = 0x010D,
    DT_ONOFF_PLUG = 0x010A, DT_DIMMABLE_PLUG = 0x010B, DT_MOUNTED_ONOFF = 0x010F, DT_MOUNTED_DIMMABLE = 0x0110,
    DT_LIGHT_SWITCH = 0x0103, DT_DIMMER_SWITCH = 0x0104, DT_COLOR_DIMMER_SWITCH = 0x0105, DT_GENERIC_SWITCH = 0x000F,
    DT_DOOR_LOCK = 0x000A, DT_WINDOW_COVERING = 0x0202, DT_THERMOSTAT = 0x0301,
};
/* 簇（Matter 应用簇规范） */
enum {
    CL_ONOFF = 0x0006, CL_LEVEL = 0x0008, CL_BOOLEAN_STATE = 0x0045, CL_POWER_SOURCE = 0x002F,
    CL_SWITCH = 0x003B, CL_SMOKE_CO = 0x005C, CL_EPM = 0x0090, CL_EEM = 0x0091, CL_DOOR_LOCK = 0x0101,
    CL_WINDOW_COVERING = 0x0102, CL_THERMOSTAT = 0x0201, CL_COLOR = 0x0300, CL_ILLUMINANCE = 0x0400, CL_TEMPERATURE = 0x0402,
    CL_PRESSURE = 0x0403, CL_HUMIDITY = 0x0405, CL_OCCUPANCY = 0x0406,
};

static bool ep_has_cl(const mc_iv_ep_t *e, uint16_t cl)
{
    for (unsigned k = 0; k < e->n_cl; k++) if (e->cl[k] == cl) return true;
    return false;
}

static bool ep_has_dt(const mc_iv_ep_t *e, uint32_t dt)
{
    for (unsigned k = 0; k < e->n_dt; k++) if (e->dt[k] == dt) return true;
    return false;
}

static bool any_dt(const mc_iv_t *iv, uint32_t dt)
{
    for (unsigned i = 0; iv && i < iv->n_ep; i++) if (ep_has_dt(&iv->ep[i], dt)) return true;
    return false;
}

uint32_t hub_mcap_caps(const mc_iv_t *iv)
{
    if (!iv) return 0;
    uint32_t c = 0;
    for (unsigned i = 0; i < iv->n_ep; i++) {
        const mc_iv_ep_t *e = &iv->ep[i];
        if (e->ep == 0) continue;                              /* 根节点：Basic Information、Descriptor 这类，不是能力 */
        if (ep_has_cl(e, CL_ONOFF)) c |= HUB_CAP_ONOFF;
        if (ep_has_cl(e, CL_LEVEL)) c |= HUB_CAP_LEVEL;
        if (ep_has_cl(e, CL_COLOR)) {
            /* HS / XY 与色温看 ColorControl 的特性位（9.6）；访谈还没读特性位，先按设备类型：色温灯只报色温，彩灯两样都报 */
            if (ep_has_dt(e, DT_CT_LIGHT) || ep_has_dt(e, DT_EXT_COLOR_LIGHT)) c |= HUB_CAP_COLOR_TEMP;
            if (ep_has_dt(e, DT_EXT_COLOR_LIGHT)) c |= HUB_CAP_COLOR;
        }
        if (ep_has_cl(e, CL_WINDOW_COVERING)) c |= HUB_CAP_COVER;
        if (ep_has_cl(e, CL_DOOR_LOCK)) c |= HUB_CAP_LOCK;
        if (ep_has_cl(e, CL_THERMOSTAT)) c |= HUB_CAP_THERMOSTAT;
        if (ep_has_cl(e, CL_TEMPERATURE)) c |= HUB_CAP_TEMPERATURE;
        if (ep_has_cl(e, CL_HUMIDITY)) c |= HUB_CAP_HUMIDITY;
        if (ep_has_cl(e, CL_ILLUMINANCE)) c |= HUB_CAP_ILLUMINANCE;
        if (ep_has_cl(e, CL_PRESSURE)) c |= HUB_CAP_PRESSURE;
        if (ep_has_cl(e, CL_OCCUPANCY)) c |= HUB_CAP_MOTION;
        if (ep_has_cl(e, CL_BOOLEAN_STATE)) {
            /* BooleanState 本身不说是什么：按同一端点的设备类型分（9.6）；别的设备类型（雨量传感器……）不报 */
            if (ep_has_dt(e, DT_CONTACT)) c |= HUB_CAP_CONTACT;
            if (ep_has_dt(e, DT_WATER_LEAK)) c |= HUB_CAP_WATER_LEAK;
        }
        if (ep_has_cl(e, CL_SMOKE_CO)) c |= HUB_CAP_SMOKE;
        if (ep_has_cl(e, CL_EPM)) c |= HUB_CAP_POWER;
        if (ep_has_cl(e, CL_EEM)) c |= HUB_CAP_ENERGY;
        if (ep_has_cl(e, CL_SWITCH)) c |= HUB_CAP_BUTTON;
        /* Identify 0x0003 不当能力：9.6 的表与附录 A 的能力表都没有它，hub_zcap 那边也明确不报（审计）—— 几乎每台都有，报了只是噪声 */
    }
    if (iv->flags & MC_IV_F_BATTERY) c |= HUB_CAP_BATTERY;    /* PowerSource 的 BAT 特性位（访谈时读；常电设备也有这个簇） */
    return c;
}

const char *hub_mcap_kind(const mc_iv_t *iv, uint32_t caps)
{
    if (!iv || !(iv->flags & MC_IV_F_DONE)) return "unknown";
    if (any_dt(iv, DT_SMOKE_CO) || (caps & HUB_CAP_SMOKE)) return "smoke_sensor";
    if (any_dt(iv, DT_WATER_LEAK)) return "leak_sensor";
    if (any_dt(iv, DT_CONTACT)) return "contact_sensor";
    if (caps & HUB_CAP_LOCK) return "lock";
    if (caps & HUB_CAP_THERMOSTAT) return "thermostat";
    if (caps & HUB_CAP_COVER) return "cover";
    if (any_dt(iv, DT_ONOFF_PLUG) || any_dt(iv, DT_DIMMABLE_PLUG)) return "plug";
    if (any_dt(iv, DT_EXT_COLOR_LIGHT) || any_dt(iv, DT_CT_LIGHT)) return "color_light";
    if (any_dt(iv, DT_DIMMABLE_LIGHT) || any_dt(iv, DT_MOUNTED_DIMMABLE)) return "dimmable_light";
    if (any_dt(iv, DT_ONOFF_LIGHT) || any_dt(iv, DT_MOUNTED_ONOFF)) return "switch";          /* 同 Zigbee：只会开关的灯也叫 switch */
    if (any_dt(iv, DT_LIGHT_SWITCH) || any_dt(iv, DT_DIMMER_SWITCH) || any_dt(iv, DT_COLOR_DIMMER_SWITCH) ||
        any_dt(iv, DT_GENERIC_SWITCH) || (caps & HUB_CAP_BUTTON)) return "button";             /* 遥控类：发命令 / 事件，不是被控 */
    if (caps & HUB_CAP_MOTION) return "motion_sensor";
    if (caps & (HUB_CAP_TEMPERATURE | HUB_CAP_HUMIDITY | HUB_CAP_PRESSURE)) return "climate_sensor";
    if (caps & HUB_CAP_ILLUMINANCE) return "light_sensor";
    if (caps & HUB_CAP_ONOFF) return (caps & (HUB_CAP_POWER | HUB_CAP_ENERGY)) ? "plug" : "switch";
    (void)DT_RAIN;
    return "unknown";
}
