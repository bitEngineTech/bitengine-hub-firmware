/* hub_zcap 的真值表（docs/15 设计 8.8；M5 第一步）。编的是产品源文件 ../hub_zcap.c。
 *   Z1 没访谈完：没能力、unknown   Z2 IAS 按 ZoneType（人体 / 门磁 / 漏水 / 烟雾 / 振动；燃气 / CO 只定设备类、不当 smoke）
 *   Z3 灯 / 插座 / 开关（HA 设备号优先）   Z4 按钮（输出簇有 OnOff、输入簇没有）   Z5 传感器类、电池只给非路由器   Z6 锁 / 温控 / 窗帘
 *   Z7 涂鸦私有簇的设备（M5 第八步）：按社区设备库导出的表加能力、报警类按表推出的 ZoneType；有写法的开关 / 亮度进 ctl_caps（M5 第九步）
 *   Z8 小米 / Aqara（M5 第十步）：没访谈完也按表定能力与设备类；表改写了的标准簇不按通用规则（门磁的 OnOff 不算开关） */
#include <stdio.h>
#include <string.h>

#include "hub_model.h"
#include "hub_zcap.h"

static int s_fail, s_pass;
#define CHECK(cond, ...) do { if (cond) { s_pass++; } else { s_fail++; printf("❌ %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* 一个端点：输入簇 in[]、输出簇 out[] */
static zh_dev_rec_t rec1(uint16_t devid, const uint16_t *in, unsigned nin, const uint16_t *out, unsigned nout, uint16_t zone, uint8_t node_type)
{
    zh_dev_rec_t r;
    memset(&r, 0, sizeof(r));
    r.flags = ZH_DEVF_INTERVIEWED;
    r.node_type = node_type;
    r.zone_type = zone;
    r.n_ep = 1;
    r.ep[0].ep = 1;
    r.ep[0].profile = ZH_HA_PROFILE;
    r.ep[0].devid = devid;
    r.ep[0].n_in = (uint8_t)nin;
    r.ep[0].n_out = (uint8_t)nout;
    unsigned c = 0;
    for (unsigned j = 0; j < nin && c < ZH_DEV_EP_CL; j++) r.ep[0].cl[c++] = in[j];
    for (unsigned j = 0; j < nout && c < ZH_DEV_EP_CL; j++) r.ep[0].cl[c++] = out[j];
    return r;
}
#define REC(devid, IN, OUT, zone, nt) rec1(devid, IN, sizeof(IN) / sizeof(IN[0]), OUT, sizeof(OUT) / sizeof(OUT[0]), zone, nt)

int main(void)
{
    static const uint16_t none[1] = { 0xFFFF };
    /* ---- Z1 ---- */
    {
        static const uint16_t in[] = { 0x0000, 0x0500 };
        zh_dev_rec_t r = REC(0x0402, in, none, ZH_ZONE_MOTION, ZH_NODE_END_DEVICE);
        r.flags = 0;
        CHECK(hub_zcap_caps(&r) == 0 && !strcmp(hub_zcap_kind(&r, 0), "unknown"), "Z1 没访谈完：没能力、unknown");
        CHECK(hub_zcap_caps(NULL) == 0 && !strcmp(hub_zcap_kind(NULL, 0), "unknown"), "Z1 NULL 不崩");
    }
    /* ---- Z2 ---- 板上那台 Third Reality 3RMS16BZ：输入 Basic / 0xFF01 / 0xFF00 / 电源 / IAS，输出 OTA，ZoneType 0x000D */
    {
        static const uint16_t in[] = { 0x0000, 0xFF01, 0xFF00, 0x0001, 0x0500 }, out[] = { 0x0019 };
        zh_dev_rec_t r = REC(0x0402, in, out, ZH_ZONE_MOTION, ZH_NODE_END_DEVICE);
        const uint32_t c = hub_zcap_caps(&r);
        CHECK(c == (HUB_CAP_MOTION | HUB_CAP_BATTERY) && !strcmp(hub_zcap_kind(&r, c), "motion_sensor"), "Z2 3RMS16BZ：motion + battery、motion_sensor（0x%X）", (unsigned)c);
        static const struct { uint16_t zone; uint32_t cap; const char *kind; } t[] = {
            { ZH_ZONE_CONTACT, HUB_CAP_CONTACT, "contact_sensor" }, { ZH_ZONE_WATER, HUB_CAP_WATER_LEAK, "leak_sensor" },
            { ZH_ZONE_FIRE, HUB_CAP_SMOKE, "smoke_sensor" },        { ZH_ZONE_VIBRATION, HUB_CAP_VIBRATION, "vibration_sensor" },
            { ZH_ZONE_GAS, 0, "gas_sensor" },                       { ZH_ZONE_CO, 0, "co_sensor" },
            { 0x0225, 0, "unknown" },                                { ZH_ZONE_TYPE_UNKNOWN, 0, "unknown" },
        };
        for (unsigned k = 0; k < sizeof(t) / sizeof(t[0]); k++) {
            r.zone_type = t[k].zone;
            const uint32_t ck = hub_zcap_caps(&r);
            CHECK(ck == (t[k].cap | HUB_CAP_BATTERY) && !strcmp(hub_zcap_kind(&r, ck), t[k].kind), "Z2 ZoneType 0x%04X → %s（能力 0x%X）", t[k].zone, t[k].kind, (unsigned)ck);
        }
        CHECK(!(hub_zcap_caps(&r) & HUB_CAP_SMOKE), "Z2 燃气 / CO 不当成 smoke");
    }
    /* ---- Z3 ---- */
    {
        static const uint16_t onoff[] = { 0x0000, 0x0003, 0x0006 }, dim[] = { 0x0000, 0x0003, 0x0006, 0x0008 }, color[] = { 0x0000, 0x0006, 0x0008, 0x0300 },
                              meter[] = { 0x0000, 0x0006, 0x0B04, 0x0702 };
        zh_dev_rec_t r = REC(0x0100, onoff, none, ZH_ZONE_TYPE_UNKNOWN, ZH_NODE_ROUTER);
        uint32_t c = hub_zcap_caps(&r);
        CHECK(c == HUB_CAP_ONOFF && !strcmp(hub_zcap_kind(&r, c), "switch"), "Z3 只有 OnOff：switch（Identify 不当能力）");
        r = REC(0x0101, dim, none, ZH_ZONE_TYPE_UNKNOWN, ZH_NODE_ROUTER);
        c = hub_zcap_caps(&r);
        CHECK(c == (HUB_CAP_ONOFF | HUB_CAP_LEVEL) && !strcmp(hub_zcap_kind(&r, c), "dimmable_light"), "Z3 OnOff + Level：dimmable_light");
        r = REC(0x0102, color, none, ZH_ZONE_TYPE_UNKNOWN, ZH_NODE_ROUTER);
        c = hub_zcap_caps(&r);
        CHECK(!(c & (HUB_CAP_COLOR | HUB_CAP_COLOR_TEMP)) && !strcmp(hub_zcap_kind(&r, c), "color_light"), "Z3 有 0x0300：color_light（颜色能力要读 ColorCapabilities，这一步不定）");
        r = REC(0x0051, dim, none, ZH_ZONE_TYPE_UNKNOWN, ZH_NODE_ROUTER);
        CHECK(!strcmp(hub_zcap_kind(&r, hub_zcap_caps(&r)), "plug"), "Z3 HA 设备号 0x0051（智能插座）：plug，哪怕能调光");
        r = REC(0x0100, meter, none, ZH_ZONE_TYPE_UNKNOWN, ZH_NODE_ROUTER);
        c = hub_zcap_caps(&r);
        CHECK((c & (HUB_CAP_POWER | HUB_CAP_ENERGY)) == (HUB_CAP_POWER | HUB_CAP_ENERGY) && !strcmp(hub_zcap_kind(&r, c), "plug"), "Z3 OnOff + 电量计量：plug");
    }
    /* ---- Z4 ---- */
    {
        static const uint16_t in[] = { 0x0000, 0x0001, 0x0003 }, out[] = { 0x0003, 0x0006, 0x0008 };
        zh_dev_rec_t r = REC(0x0820, in, out, ZH_ZONE_TYPE_UNKNOWN, ZH_NODE_END_DEVICE);
        const uint32_t c = hub_zcap_caps(&r);
        CHECK((c & HUB_CAP_BUTTON) && (c & HUB_CAP_BATTERY) && !(c & HUB_CAP_ONOFF) && !strcmp(hub_zcap_kind(&r, c), "button"), "Z4 输出簇有 OnOff / Level：button");
        static const uint16_t in2[] = { 0x0000, 0x0006 }, out2[] = { 0x0006 };
        r = REC(0x0100, in2, out2, ZH_ZONE_TYPE_UNKNOWN, ZH_NODE_ROUTER);
        CHECK(!(hub_zcap_caps(&r) & HUB_CAP_BUTTON), "Z4 输入簇也有 OnOff（开关自己能被控）：不算 button");
    }
    /* ---- Z5 ---- */
    {
        static const uint16_t in[] = { 0x0000, 0x0001, 0x0402, 0x0405, 0x0403 };
        zh_dev_rec_t r = REC(0x0302, in, none, ZH_ZONE_TYPE_UNKNOWN, ZH_NODE_END_DEVICE);
        uint32_t c = hub_zcap_caps(&r);
        CHECK(c == (HUB_CAP_TEMPERATURE | HUB_CAP_HUMIDITY | HUB_CAP_PRESSURE | HUB_CAP_BATTERY) && !strcmp(hub_zcap_kind(&r, c), "climate_sensor"), "Z5 温湿度气压：climate_sensor");
        static const uint16_t lux[] = { 0x0000, 0x0400 }, occ[] = { 0x0000, 0x0406, 0x0400 }, rpow[] = { 0x0000, 0x0001, 0x0006 };
        r = REC(0x0106, lux, none, ZH_ZONE_TYPE_UNKNOWN, ZH_NODE_END_DEVICE);
        CHECK(!strcmp(hub_zcap_kind(&r, hub_zcap_caps(&r)), "light_sensor"), "Z5 只有照度：light_sensor");
        r = REC(0x0107, occ, none, ZH_ZONE_TYPE_UNKNOWN, ZH_NODE_END_DEVICE);
        c = hub_zcap_caps(&r);
        CHECK((c & HUB_CAP_MOTION) && !strcmp(hub_zcap_kind(&r, c), "motion_sensor"), "Z5 占用传感器（0x0406）：motion_sensor");
        r = REC(0x0100, rpow, none, ZH_ZONE_TYPE_UNKNOWN, ZH_NODE_ROUTER);
        CHECK(!(hub_zcap_caps(&r) & HUB_CAP_BATTERY), "Z5 路由器带 Power Configuration：不算 battery（市电的也常带这个簇）");
    }
    /* ---- Z6 ---- */
    {
        static const uint16_t lock[] = { 0x0000, 0x0001, 0x0101 }, th[] = { 0x0000, 0x0201, 0x0402 }, cov[] = { 0x0000, 0x0102 };
        zh_dev_rec_t r = REC(0x000A, lock, none, ZH_ZONE_TYPE_UNKNOWN, ZH_NODE_END_DEVICE);
        CHECK(!strcmp(hub_zcap_kind(&r, hub_zcap_caps(&r)), "lock"), "Z6 lock");
        r = REC(0x0301, th, none, ZH_ZONE_TYPE_UNKNOWN, ZH_NODE_ROUTER);
        CHECK(!strcmp(hub_zcap_kind(&r, hub_zcap_caps(&r)), "thermostat"), "Z6 thermostat（温度簇也在，温控优先）");
        r = REC(0x0202, cov, none, ZH_ZONE_TYPE_UNKNOWN, ZH_NODE_ROUTER);
        CHECK(!strcmp(hub_zcap_kind(&r, hub_zcap_caps(&r)), "cover"), "Z6 cover");
    }
    /* ---- Z7 ---- 涂鸦私有簇（M5 第八步）：输入簇只有 Basic / 0xEF00，能力全靠表 */
    {
        static const uint16_t in[] = { 0x0000, 0xEF00 };
        zh_dev_rec_t r = REC(0x0051, in, none, ZH_ZONE_TYPE_UNKNOWN, ZH_NODE_END_DEVICE);
        strcpy(r.model, "TS0601");
        strcpy(r.manuf, "_TZE200_pay2byax");                     /* 门磁：dp1 contact、dp2 电量、dp101 照度 */
        uint32_t c = hub_zcap_caps(&r);
        CHECK(c == (HUB_CAP_CONTACT | HUB_CAP_BATTERY | HUB_CAP_ILLUMINANCE) && !strcmp(hub_zcap_kind(&r, c), "contact_sensor"), "Z7 涂鸦门磁：contact + battery + illuminance、contact_sensor（0x%X）", (unsigned)c);
        strcpy(r.manuf, "_TZE200_bjawzodf");                     /* 温湿度 */
        c = hub_zcap_caps(&r);
        CHECK(c == (HUB_CAP_TEMPERATURE | HUB_CAP_HUMIDITY | HUB_CAP_BATTERY) && !strcmp(hub_zcap_kind(&r, c), "climate_sensor"), "Z7 涂鸦温湿度：climate_sensor（0x%X）", (unsigned)c);
        strcpy(r.manuf, "_TZE200_ntcy3xu1");                     /* 烟雾 */
        c = hub_zcap_caps(&r);
        CHECK((c & HUB_CAP_SMOKE) && !strcmp(hub_zcap_kind(&r, c), "smoke_sensor"), "Z7 涂鸦烟雾：smoke、smoke_sensor（0x%X）", (unsigned)c);
        strcpy(r.manuf, "_TZE200_3towulqd");                     /* 人体（占用） */
        c = hub_zcap_caps(&r);
        CHECK((c & HUB_CAP_MOTION) && !strcmp(hub_zcap_kind(&r, c), "motion_sensor"), "Z7 涂鸦人体：motion_sensor（0x%X）", (unsigned)c);
        strcpy(r.manuf, "_TZE200_swaamsoy");                     /* 调光器：开关 + 亮度，devid 0x0051 也不当插座以外的 —— 只看能力怎么推 */
        r.node_type = ZH_NODE_ROUTER;
        r.ep[0].devid = 0x0100;
        c = hub_zcap_caps(&r);
        CHECK(c == (HUB_CAP_ONOFF | HUB_CAP_LEVEL) && !strcmp(hub_zcap_kind(&r, c), "dimmable_light") && hub_zcap_ctl_caps(&r) == (HUB_CAP_ONOFF | HUB_CAP_LEVEL),
              "Z7 涂鸦调光器：onoff + level、dimmable_light，ctl_caps 也是 onoff + level（M5 第九步写数据点）");
        strcpy(r.manuf, "_TZE200_bjawzodf");
        CHECK(hub_zcap_ctl_caps(&r) == 0, "Z7 涂鸦温湿度计：没有写法，ctl_caps = 0");
        strcpy(r.manuf, "_TZE200_zzzzzzzz");
        CHECK(hub_zcap_caps(&r) == 0 && !strcmp(hub_zcap_kind(&r, 0), "unknown"), "Z7 不在表里的 TS0601：没能力、unknown");
        r.flags = 0;
        strcpy(r.manuf, "_TZE200_pay2byax");
        CHECK(hub_zcap_caps(&r) == 0, "Z7 没访谈完：不查表");
        /* ctl_caps 只认 ZCL 输入簇 */
        static const uint16_t lin[] = { 0x0000, 0x0006, 0x0008 };
        zh_dev_rec_t l = REC(0x0101, lin, none, ZH_ZONE_TYPE_UNKNOWN, ZH_NODE_ROUTER);
        CHECK(hub_zcap_ctl_caps(&l) == (HUB_CAP_ONOFF | HUB_CAP_LEVEL) && hub_zcap_ctl_caps(NULL) == 0, "Z7 ZCL 灯：ctl_caps = onoff + level");
        l.flags = 0;
        CHECK(hub_zcap_ctl_caps(&l) == 0, "Z7 没访谈完：ctl_caps = 0");
    }
    /* ---- Z8 ---- 小米 / Aqara（M5 第十步） */
    {
        zh_dev_rec_t r;
        memset(&r, 0, sizeof(r));                                  /* 访谈没走完：没有端点，只有从上报学来的型号 */
        r.node_type = ZH_NODE_END_DEVICE;
        r.zone_type = ZH_ZONE_TYPE_UNKNOWN;
        strcpy(r.model, "lumi.weather");
        uint32_t c = hub_zcap_caps(&r);
        CHECK(c == (HUB_CAP_TEMPERATURE | HUB_CAP_HUMIDITY | HUB_CAP_PRESSURE | HUB_CAP_BATTERY) && !strcmp(hub_zcap_kind(&r, c), "climate_sensor"),
              "Z8 没访谈完的小米温湿度计：温度 / 湿度 / 气压 / 电量、climate_sensor（0x%X）", (unsigned)c);
        strcpy(r.model, "lumi.sensor_wleak.aq1");
        c = hub_zcap_caps(&r);
        CHECK((c & HUB_CAP_WATER_LEAK) && !strcmp(hub_zcap_kind(&r, c), "leak_sensor"), "Z8 没访谈完的小米漏水：leak_sensor（IAS 报警靠表认，0x%X）", (unsigned)c);
        strcpy(r.model, "lumi.sensor_motion.aq2");
        c = hub_zcap_caps(&r);
        CHECK((c & HUB_CAP_MOTION) && (c & HUB_CAP_ILLUMINANCE) && !strcmp(hub_zcap_kind(&r, c), "motion_sensor"), "Z8 小米人体：motion + illuminance");
        strcpy(r.model, "lumi.no_such");
        CHECK(hub_zcap_caps(&r) == 0 && !strcmp(hub_zcap_kind(&r, 0), "unknown"), "Z8 不在小米表里、没访谈完：没能力");
        /* 访谈完的小米门磁：端点上有 OnOff 输入簇，但表说它是开合 —— 不算开关、不可控的能力 */
        static const uint16_t in[] = { 0x0000, 0x0003, 0xFFFF, 0x0006 };
        zh_dev_rec_t m = REC(0x0104, in, none, ZH_ZONE_TYPE_UNKNOWN, ZH_NODE_END_DEVICE);
        strcpy(m.model, "lumi.sensor_magnet.aq2");
        c = hub_zcap_caps(&m);
        CHECK(!(c & HUB_CAP_ONOFF) && (c & HUB_CAP_CONTACT) && !strcmp(hub_zcap_kind(&m, c), "contact_sensor"), "Z8 小米门磁：OnOff 簇是开合（contact，不是 onoff），contact_sensor（0x%X）", (unsigned)c);
        CHECK(hub_zcap_ctl_caps(&m) == 0, "Z8 小米门磁的 OnOff 簇不算能控制");
        strcpy(m.model, "");
        c = hub_zcap_caps(&m);
        CHECK((c & HUB_CAP_ONOFF) && !(c & HUB_CAP_CONTACT), "Z8 同样的簇、不知道型号：照通用规则是开关");
    }
    printf("hub_zcap: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
