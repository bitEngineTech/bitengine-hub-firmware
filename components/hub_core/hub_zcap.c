/* hub_zcap —— Zigbee 设备的能力与设备类（规则见 include/hub_zcap.h）。 */
#include "hub_zcap.h"

#include <stdbool.h>

#include "hub_model.h"

#if CONFIG_BITENGINE_ENABLE_ZHUB

/* 记录里有没有这个输入簇 / 输出簇（cl[]：先输入后输出，见 ZH_DEV_EP_CL） */
static bool has_cl(const zh_dev_rec_t *r, uint16_t cl, bool out)
{
    for (unsigned k = 0; k < r->n_ep && k < ZH_DEV_EP_REC; k++) {
        const struct zh_dev_ep *e = &r->ep[k];
        const unsigned n_in = e->n_in < ZH_DEV_EP_CL ? e->n_in : ZH_DEV_EP_CL;
        const unsigned n_out = e->n_out < ZH_DEV_EP_CL - n_in ? e->n_out : ZH_DEV_EP_CL - n_in;
        const unsigned from = out ? n_in : 0, to = out ? n_in + n_out : n_in;
        for (unsigned j = from; j < to; j++)
            if (e->cl[j] == cl) return true;
    }
    return false;
}

static bool has_devid(const zh_dev_rec_t *r, uint16_t devid)
{
    for (unsigned k = 0; k < r->n_ep && k < ZH_DEV_EP_REC; k++)
        if (r->ep[k].profile == ZH_HA_PROFILE && r->ep[k].devid == devid) return true;
    return false;
}

/* 社区设备库导出的表（涂鸦 / 小米）里的字段 → 能力（报警类走 ZoneType） */
static const struct { uint8_t tf; uint32_t cap; } k_tbl[] = {
    { ZH_TF_ONOFF, HUB_CAP_ONOFF },          { ZH_TF_LEVEL, HUB_CAP_LEVEL },
    { ZH_TF_BATTERY_PCT, HUB_CAP_BATTERY },  { ZH_TF_BATTERY_LOW, HUB_CAP_BATTERY },
    { ZH_TF_TEMP, HUB_CAP_TEMPERATURE },     { ZH_TF_HUMIDITY, HUB_CAP_HUMIDITY },
    { ZH_TF_LUX, HUB_CAP_ILLUMINANCE },      { ZH_TF_OCCUPANCY, HUB_CAP_MOTION },
    { ZH_TF_VOLTAGE, HUB_CAP_BATTERY },      { ZH_TF_PRESSURE, HUB_CAP_PRESSURE },
};
static uint32_t tbl_caps(uint32_t tf)
{
    uint32_t caps = 0;
    for (unsigned k = 0; k < sizeof(k_tbl) / sizeof(k_tbl[0]); k++)
        if (tf & (1u << k_tbl[k].tf)) caps |= k_tbl[k].cap;
    return caps;
}

uint32_t hub_zcap_caps(const zh_dev_rec_t *r)
{
    if (!r) return 0;
    /* 小米（M5 第十步）：访谈常常走不完（休眠设备几乎不应），型号在小米表里就按表定 —— 没访谈完的只用表、不看簇 */
    const int ld = zh_dev_lumi_dev(r);
    const bool iv = (r->flags & ZH_DEVF_INTERVIEWED) != 0;
    if (!iv && ld < 0) return 0;
    static const struct { uint16_t cl; uint32_t cap; } k_in[] = {
        { ZH_CL_ONOFF, HUB_CAP_ONOFF },          { ZH_CL_LEVEL, HUB_CAP_LEVEL },
        { 0x0102, HUB_CAP_COVER },               /* Window Covering */
        { 0x0101, HUB_CAP_LOCK },                /* Door Lock */
        { 0x0201, HUB_CAP_THERMOSTAT },          /* Thermostat */
        { ZH_CL_TEMPERATURE, HUB_CAP_TEMPERATURE }, { ZH_CL_HUMIDITY, HUB_CAP_HUMIDITY },
        { ZH_CL_ILLUMINANCE, HUB_CAP_ILLUMINANCE }, { ZH_CL_PRESSURE, HUB_CAP_PRESSURE },
        { ZH_CL_OCCUPANCY, HUB_CAP_MOTION },
        { 0x0B04, HUB_CAP_POWER },               /* Electrical Measurement */
        { 0x0702, HUB_CAP_ENERGY },              /* Metering */
        /* Identify（0x0003）不当能力：几乎每台都有，附录 A 的能力 / 过滤列表里也没有（两轮审计） */
    };
    uint32_t caps = 0;
    for (unsigned k = 0; k < sizeof(k_in) / sizeof(k_in[0]); k++)   /* 小米表改写了的簇（门磁的 OnOff 是开合）不按通用规则 */
        if (iv && has_cl(r, k_in[k].cl, false) && !(ld >= 0 && zh_lumi_has_cl(ld, k_in[k].cl))) caps |= k_in[k].cap;
    switch (zh_dev_zone_type(r)) {               /* IAS 设备看 ZoneType；涂鸦设备看表里的主报警字段（zh_dev_zone_type 一处算） */
    case ZH_ZONE_MOTION:    caps |= HUB_CAP_MOTION; break;
    case ZH_ZONE_CONTACT:   caps |= HUB_CAP_CONTACT; break;
    case ZH_ZONE_FIRE:      caps |= HUB_CAP_SMOKE; break;
    case ZH_ZONE_WATER:     caps |= HUB_CAP_WATER_LEAK; break;
    case ZH_ZONE_VIBRATION: caps |= HUB_CAP_VIBRATION; break;
    case ZH_ZONE_GAS:       break;               /* 燃气：能力表里没有对应项（只定设备类，见 hub_zcap_kind） */
    case ZH_ZONE_CO:        break;               /* CO：同上；别当成 smoke —— 用户问"有没有烟"时会被误导 */
    default:                break;
    }
    /* 涂鸦私有簇的设备（M5 第八步）与小米（第十步）：社区设备库导出的表说它报哪些字段（报警类走上面的 ZoneType） */
    caps |= tbl_caps(zh_tuya_fields(zh_dev_tuya_dev(r))) | tbl_caps(zh_lumi_fields(ld));
    if (iv && has_cl(r, ZH_CL_POWER_CFG, false) && r->node_type != ZH_NODE_ROUTER) caps |= HUB_CAP_BATTERY;
    if (iv && !(caps & HUB_CAP_ONOFF) && (has_cl(r, ZH_CL_ONOFF, true) || has_cl(r, ZH_CL_LEVEL, true) || has_cl(r, 0x0005 /* Scenes */, true))) caps |= HUB_CAP_BUTTON;
    return caps;
}

uint32_t hub_zcap_ctl_caps(const zh_dev_rec_t *r)
{
    if (!r || !(r->flags & ZH_DEVF_INTERVIEWED)) return 0;
    uint32_t c = 0;
    const int ld = zh_dev_lumi_dev(r);              /* 小米表改写了的（门磁 / 振动的 OnOff 是开合）不算能控制（审计 ⚠️） */
    if (has_cl(r, ZH_CL_ONOFF, false) && !(ld >= 0 && zh_lumi_has_cl(ld, ZH_CL_ONOFF))) c |= HUB_CAP_ONOFF;
    if (has_cl(r, ZH_CL_LEVEL, false) && !(ld >= 0 && zh_lumi_has_cl(ld, ZH_CL_LEVEL))) c |= HUB_CAP_LEVEL;
    const uint32_t tf = zh_tuya_ctl_fields(zh_dev_tuya_dev(r));   /* 涂鸦：社区设备库导出的写法（M5 第九步；只导读得回来的 dp） */
    if (tf & (1u << ZH_TF_ONOFF)) c |= HUB_CAP_ONOFF;
    if (tf & (1u << ZH_TF_LEVEL)) c |= HUB_CAP_LEVEL;
    return c;
}

const char *hub_zcap_kind(const zh_dev_rec_t *r, uint32_t caps)
{
    if (!r || (!(r->flags & ZH_DEVF_INTERVIEWED) && zh_dev_lumi_dev(r) < 0)) return "unknown";   /* 小米没访谈完也按表（见 hub_zcap_caps） */
    switch (zh_dev_zone_type(r)) {               /* IAS 设备 / 涂鸦的报警类设备：ZoneType 说了算 */
    case ZH_ZONE_FIRE:      return "smoke_sensor";
    case ZH_ZONE_WATER:     return "leak_sensor";
    case ZH_ZONE_GAS:       return "gas_sensor";
    case ZH_ZONE_CO:        return "co_sensor";
    case ZH_ZONE_CONTACT:   return "contact_sensor";
    case ZH_ZONE_MOTION:    return "motion_sensor";
    case ZH_ZONE_VIBRATION: return "vibration_sensor";
    default:                break;
    }
    if (caps & HUB_CAP_LOCK) return "lock";
    if (caps & HUB_CAP_THERMOSTAT) return "thermostat";
    if (caps & HUB_CAP_COVER) return "cover";
    if (caps & HUB_CAP_ONOFF) {
        /* HA 设备号：0x0009 市电插座、0x0051 智能插座、0x010A / 0x010B 插入式开关单元 —— 有它们就是插座，不看有没有调光 */
        if (has_devid(r, 0x0009) || has_devid(r, 0x0051) || has_devid(r, 0x010A) || has_devid(r, 0x010B)) return "plug";
        if (has_cl(r, 0x0300, false)) return "color_light";
        if (caps & HUB_CAP_LEVEL) return "dimmable_light";
        if (caps & (HUB_CAP_POWER | HUB_CAP_ENERGY)) return "plug";
        return "switch";
    }
    if (caps & HUB_CAP_MOTION) return "motion_sensor";   /* 占用传感器（0x0406） */
    if (caps & HUB_CAP_BUTTON) return "button";
    if (caps & (HUB_CAP_TEMPERATURE | HUB_CAP_HUMIDITY | HUB_CAP_PRESSURE)) return "climate_sensor";
    if (caps & HUB_CAP_ILLUMINANCE) return "light_sensor";
    return "unknown";
}

#endif /* CONFIG_BITENGINE_ENABLE_ZHUB */
