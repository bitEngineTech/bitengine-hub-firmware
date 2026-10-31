/* hub_mcap（Matter 能力 / 类别，9.6）的主机测试：两台真设备的访谈结果（Eve Energy、Aqara 门窗 P2，G0-5 / M7 读到的），
 * 加上表里每一行。 bash run.sh */
#include <stdio.h>
#include <string.h>
#include "hub_mcap.h"
#include "hub_model.h"
#include "../../matter_ctrl/mc_iv_core.h"

static int pass, fail;
#define CHECK(c, what) do { if (c) pass++; else { fail++; printf("❌ %s（%s:%d）\n", what, __FILE__, __LINE__); } } while (0)

static mc_iv_t mk(uint32_t dt, const uint16_t *cl, int ncl)
{
    mc_iv_t iv;
    memset(&iv, 0, sizeof(iv));
    iv.flags = MC_IV_F_DONE;
    mc_iv_ep_t *e0 = mc_iv_ep(&iv, 0);
    mc_iv_add_dt(&iv, e0, 0x0016);
    mc_iv_add_cl(&iv, e0, 0x0028); mc_iv_add_cl(&iv, e0, 0x0006);   /* 根节点上的簇不算能力 */
    mc_iv_ep_t *e1 = mc_iv_ep(&iv, 1);
    mc_iv_add_dt(&iv, e1, dt);
    for (int i = 0; i < ncl; i++) mc_iv_add_cl(&iv, e1, cl[i]);
    return iv;
}

int main(void)
{
    /* Eve Energy 20EBU4101：端点 1 插座（0x010A，OnOff），端点 2 电量传感器（0x0510，EPM / EEM） */
    const uint16_t eve1[] = { 0x0003, 0x0004, 0x0006 };
    mc_iv_t eve = mk(0x010A, eve1, 3);
    mc_iv_ep_t *e2 = mc_iv_ep(&eve, 2);
    mc_iv_add_dt(&eve, e2, 0x0510); mc_iv_add_cl(&eve, e2, 0x0090); mc_iv_add_cl(&eve, e2, 0x0091);
    uint32_t c = hub_mcap_caps(&eve);
    CHECK(c == (HUB_CAP_ONOFF | HUB_CAP_POWER | HUB_CAP_ENERGY), "Eve Energy：onoff + power + energy（Identify 不算能力，同 hub_zcap）");
    CHECK(!strcmp(hub_mcap_kind(&eve, c), "plug"), "Eve Energy 是 plug");

    /* Aqara 门窗 P2：端点 1 门磁（0x0015，BooleanState），端点 2 电源（0x0011，PowerSource，BAT） */
    const uint16_t aq1[] = { 0x0003, 0x0045 };
    mc_iv_t aq = mk(0x0015, aq1, 2);
    mc_iv_ep_t *a2 = mc_iv_ep(&aq, 2);
    mc_iv_add_dt(&aq, a2, 0x0011); mc_iv_add_cl(&aq, a2, 0x002F);
    aq.flags |= MC_IV_F_BATTERY | MC_IV_F_ICD;
    c = hub_mcap_caps(&aq);
    CHECK(c == (HUB_CAP_CONTACT | HUB_CAP_BATTERY), "Aqara 门窗：contact + battery（Identify 不算）");
    CHECK(!strcmp(hub_mcap_kind(&aq, c), "contact_sensor"), "Aqara 门窗是 contact_sensor");
    aq.flags &= (uint8_t)~MC_IV_F_BATTERY;
    CHECK(!(hub_mcap_caps(&aq) & HUB_CAP_BATTERY), "没有 BAT 特性位就不报 battery（常电设备也有 PowerSource 簇）");

    /* 表里的其余行 */
    const uint16_t leak[] = { 0x0045 };
    mc_iv_t lk = mk(0x0043, leak, 1);
    c = hub_mcap_caps(&lk);
    CHECK(c == HUB_CAP_WATER_LEAK && !strcmp(hub_mcap_kind(&lk, c), "leak_sensor"), "BooleanState + 漏水检测 → water_leak / leak_sensor");
    mc_iv_t rain = mk(0x0044, leak, 1);
    CHECK(hub_mcap_caps(&rain) == 0, "BooleanState + 别的设备类型（雨量）不报能力");
    const uint16_t dim[] = { 0x0006, 0x0008 };
    mc_iv_t dl = mk(0x0101, dim, 2);
    c = hub_mcap_caps(&dl);
    CHECK(c == (HUB_CAP_ONOFF | HUB_CAP_LEVEL) && !strcmp(hub_mcap_kind(&dl, c), "dimmable_light"), "调光灯");
    const uint16_t col[] = { 0x0006, 0x0008, 0x0300 };
    mc_iv_t ct = mk(0x010C, col, 3);
    c = hub_mcap_caps(&ct);
    CHECK(c == (HUB_CAP_ONOFF | HUB_CAP_LEVEL | HUB_CAP_COLOR_TEMP) && !strcmp(hub_mcap_kind(&ct, c), "color_light"), "色温灯：只报色温");
    mc_iv_t ec = mk(0x010D, col, 3);
    c = hub_mcap_caps(&ec);
    CHECK((c & HUB_CAP_COLOR) && (c & HUB_CAP_COLOR_TEMP) && !strcmp(hub_mcap_kind(&ec, c), "color_light"), "彩灯：颜色 + 色温");
    const uint16_t lock[] = { 0x0101 };
    mc_iv_t dl2 = mk(0x000A, lock, 1);
    CHECK(hub_mcap_caps(&dl2) == HUB_CAP_LOCK && !strcmp(hub_mcap_kind(&dl2, HUB_CAP_LOCK), "lock"), "门锁");
    const uint16_t cov[] = { 0x0102 };
    mc_iv_t wc = mk(0x0202, cov, 1);
    CHECK(hub_mcap_caps(&wc) == HUB_CAP_COVER && !strcmp(hub_mcap_kind(&wc, HUB_CAP_COVER), "cover"), "窗帘");
    const uint16_t th[] = { 0x0201, 0x0402 };
    mc_iv_t ts = mk(0x0301, th, 2);
    c = hub_mcap_caps(&ts);
    CHECK((c & HUB_CAP_THERMOSTAT) && !strcmp(hub_mcap_kind(&ts, c), "thermostat"), "温控器");
    const uint16_t clim[] = { 0x0402, 0x0405, 0x0403 };
    mc_iv_t cs = mk(0x0302, clim, 3);
    c = hub_mcap_caps(&cs);
    CHECK(c == (HUB_CAP_TEMPERATURE | HUB_CAP_HUMIDITY | HUB_CAP_PRESSURE) && !strcmp(hub_mcap_kind(&cs, c), "climate_sensor"), "温湿度气压");
    const uint16_t occ[] = { 0x0406 };
    mc_iv_t os = mk(0x0107, occ, 1);
    CHECK(hub_mcap_caps(&os) == HUB_CAP_MOTION && !strcmp(hub_mcap_kind(&os, HUB_CAP_MOTION), "motion_sensor"), "占用 → motion");
    const uint16_t ill[] = { 0x0400 };
    mc_iv_t ls = mk(0x0106, ill, 1);
    CHECK(!strcmp(hub_mcap_kind(&ls, hub_mcap_caps(&ls)), "light_sensor"), "照度");
    const uint16_t smk[] = { 0x005C };
    mc_iv_t sm = mk(0x0076, smk, 1);
    CHECK(hub_mcap_caps(&sm) == HUB_CAP_SMOKE && !strcmp(hub_mcap_kind(&sm, HUB_CAP_SMOKE), "smoke_sensor"), "烟雾 / CO");
    const uint16_t sw[] = { 0x003B };
    mc_iv_t gs = mk(0x000F, sw, 1);
    CHECK(hub_mcap_caps(&gs) == HUB_CAP_BUTTON && !strcmp(hub_mcap_kind(&gs, HUB_CAP_BUTTON), "button"), "通用开关（Switch 簇）→ button");
    const uint16_t onoff[] = { 0x0006 };
    mc_iv_t ol = mk(0x0100, onoff, 1);
    CHECK(!strcmp(hub_mcap_kind(&ol, hub_mcap_caps(&ol)), "switch"), "只会开关的灯 → switch（同 Zigbee）");
    mc_iv_t pend = eve; pend.flags = 0;
    CHECK(!strcmp(hub_mcap_kind(&pend, hub_mcap_caps(&pend)), "unknown") && hub_mcap_caps(&pend) != 0, "访谈没完：kind unknown，能力照算已读到的");
    CHECK(hub_mcap_caps(NULL) == 0 && !strcmp(hub_mcap_kind(NULL, 0), "unknown"), "NULL");

    printf("hub_mcap: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
