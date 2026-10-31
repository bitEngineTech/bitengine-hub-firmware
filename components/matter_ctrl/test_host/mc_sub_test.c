/* mc_sub_core 的主机测试：订哪些路径、报告值的换算、翻转事件、离线判定、退避（M7 第五步）。 */
#include <stdio.h>
#include <string.h>

#include "../mc_iv_core.h"
#include "../mc_sub_core.h"

static int pass, fail;
#define CHECK(c, what) do { if (c) pass++; else { fail++; printf("❌ %s（%s:%d）\n", what, __FILE__, __LINE__); } } while (0)

/* 板上两台的访谈结果（10-24 device.get 读回的） */
static mc_iv_t eve(void)
{
    mc_iv_t iv;
    memset(&iv, 0, sizeof(iv));
    iv.flags = MC_IV_F_DONE;
    mc_iv_ep_t *e0 = mc_iv_ep(&iv, 0);
    mc_iv_add_dt(&iv, e0, 0x0016);
    mc_iv_add_cl(&iv, e0, 0x001D); mc_iv_add_cl(&iv, e0, 0x0028); mc_iv_add_cl(&iv, e0, 0x002F);
    mc_iv_ep_t *e1 = mc_iv_ep(&iv, 1);
    mc_iv_add_dt(&iv, e1, 0x010A);
    mc_iv_add_cl(&iv, e1, 0x0003); mc_iv_add_cl(&iv, e1, 0x0004); mc_iv_add_cl(&iv, e1, 0x0006); mc_iv_add_cl(&iv, e1, 0x001D);
    mc_iv_ep_t *e2 = mc_iv_ep(&iv, 2);
    mc_iv_add_dt(&iv, e2, 0x0510);
    mc_iv_add_cl(&iv, e2, 0x0090); mc_iv_add_cl(&iv, e2, 0x0091);
    return iv;
}

static mc_iv_t aqara(void)
{
    mc_iv_t iv;
    memset(&iv, 0, sizeof(iv));
    iv.flags = MC_IV_F_DONE | MC_IV_F_BATTERY | MC_IV_F_ICD;
    mc_iv_ep_t *e0 = mc_iv_ep(&iv, 0);
    mc_iv_add_dt(&iv, e0, 0x0012); mc_iv_add_dt(&iv, e0, 0x0016);
    mc_iv_add_cl(&iv, e0, 0x001D); mc_iv_add_cl(&iv, e0, 0x0028); mc_iv_add_cl(&iv, e0, 0x0046);
    mc_iv_ep_t *e1 = mc_iv_ep(&iv, 1);
    mc_iv_add_dt(&iv, e1, 0x0015);
    mc_iv_add_cl(&iv, e1, 0x0003); mc_iv_add_cl(&iv, e1, 0x001D); mc_iv_add_cl(&iv, e1, 0x0045); mc_iv_add_cl(&iv, e1, 0x0080);
    mc_iv_ep_t *e2 = mc_iv_ep(&iv, 2);
    mc_iv_add_dt(&iv, e2, 0x0011);
    mc_iv_add_cl(&iv, e2, 0x001D); mc_iv_add_cl(&iv, e2, 0x002F);
    return iv;
}

static mc_val_t vb(int b) { return (mc_val_t){ MC_V_BOOL, (uint64_t)(b != 0), 0 }; }
static mc_val_t vu(uint64_t u) { return (mc_val_t){ MC_V_UINT, u, 0 }; }
static mc_val_t vi(int64_t i) { return (mc_val_t){ MC_V_INT, 0, i }; }
static const mc_val_t VNULL = { MC_V_NULL, 0, 0 };

int main(void)
{
    mc_path_t p[MC_SUB_PATHS_MAX];
    /* ---- 路径 ---- */
    mc_iv_t e = eve();
    int n = mc_sub_paths(&e, p, MC_SUB_PATHS_MAX);
    CHECK(n == 3 && p[0].field == MC_ST_ONOFF && p[0].ep == 1 && p[0].cl == 0x0006 && p[0].at == 0
          && p[1].field == MC_ST_POWER && p[1].ep == 2 && p[1].cl == 0x0090 && p[1].at == 0x0008
          && p[2].field == MC_ST_ENERGY && p[2].ep == 2 && p[2].cl == 0x0091 && p[2].at == 0x0001,
          "Eve：开关（ep1）、功率、电能（ep2）；根端点的 PowerSource 没 BAT 位不订");
    mc_iv_t a = aqara();
    n = mc_sub_paths(&a, p, MC_SUB_PATHS_MAX);
    CHECK(n == 2 && p[0].field == MC_ST_CONTACT && p[0].ep == 1 && p[0].cl == 0x0045 && p[1].field == MC_ST_BATTERY && p[1].ep == 2
          && p[1].cl == 0x002F && p[1].at == 0x000C, "Aqara：门磁（ep1）+ 电量（ep2 的 PowerSource）");
    mc_iv_t lk = aqara();
    lk.ep[1].dt[0] = 0x0043;
    n = mc_sub_paths(&lk, p, MC_SUB_PATHS_MAX);
    CHECK(n == 2 && p[0].field == MC_ST_LEAK, "BooleanState 按设备类型：漏水传感器 → water_leak");
    mc_iv_t rain = aqara();
    rain.ep[1].dt[0] = 0x0044;
    rain.flags &= (uint8_t)~MC_IV_F_BATTERY;
    n = mc_sub_paths(&rain, p, MC_SUB_PATHS_MAX);
    CHECK(n == 1 && p[0].field == MC_FIELD_NONE && p[0].ep == 0 && p[0].cl == 0x0028 && p[0].at == 0x0005,
          "认不出的 BooleanState、也没别的：只订根端点 NodeLabel 保活");
    n = mc_sub_paths(&e, p, 2);
    CHECK(n == 2 && p[0].field == MC_ST_ONOFF && p[1].field == MC_ST_POWER, "上限截断：按优先级留前面的");
    CHECK(mc_sub_paths(NULL, p, 4) == 0 && mc_sub_paths(&e, p, 0) == 0, "坏参数 → 0");

    /* ---- 换算 ---- */
    mc_state_t st;
    memset(&st, 0, sizeof(st));
    unsigned ev = 0xFF;
    mc_val_t v = vb(1);
    CHECK(mc_sub_apply(&st, MC_ST_ONOFF, &v, 1000, &ev) && st.onoff == 1 && (st.have & (1u << MC_ST_ONOFF)) && st.at_ms[MC_ST_ONOFF] == 1000 && ev == 0,
          "开关：bool 进读数、记时刻");
    v = vu(1);
    CHECK(!mc_sub_apply(&st, MC_ST_ONOFF, &v, 1001, &ev) && st.at_ms[MC_ST_ONOFF] == 1000, "开关收到整数：类型不对丢掉");
    v = vu(254); (void)mc_sub_apply(&st, MC_ST_LEVEL, &v, 1, &ev);
    CHECK(st.level_pct == 100, "亮度 254 → 100 %");
    v = vu(1); (void)mc_sub_apply(&st, MC_ST_LEVEL, &v, 1, &ev);
    CHECK(st.level_pct == 1, "亮度 1 → 1 %（亮着就不是 0）");
    v = vu(127); (void)mc_sub_apply(&st, MC_ST_LEVEL, &v, 1, &ev);
    CHECK(st.level_pct == 50, "亮度 127 → 50 %");
    v = vu(255);
    CHECK(!mc_sub_apply(&st, MC_ST_LEVEL, &v, 1, &ev) && st.level_pct == 50, "亮度 255 超范围：丢掉");
    CHECK(mc_sub_apply(&st, MC_ST_LEVEL, &VNULL, 2, &ev) && !(st.have & (1u << MC_ST_LEVEL)), "null：这一项不报了");

    v = vb(1);
    CHECK(mc_sub_apply(&st, MC_ST_CONTACT, &v, 10, &ev) && st.contact_open == 0 && ev == 0, "门磁首报 true = 合上；首报不出事件");
    v = vb(0);
    CHECK(mc_sub_apply(&st, MC_ST_CONTACT, &v, 11, &ev) && st.contact_open == 1 && ev == MC_SEV_CONTACT, "合 → 开：出事件");
    CHECK(mc_sub_apply(&st, MC_ST_CONTACT, &v, 12, &ev) && ev == 0, "同值再报：不出");

    v = vu(1); (void)mc_sub_apply(&st, MC_ST_MOTION, &v, 1000, &ev);
    CHECK(st.motion == 1 && ev == MC_SEV_MOTION_ON, "人体首报有人：出（没收到过按无人算，同 Zigbee）");
    v = vu(0); (void)mc_sub_apply(&st, MC_ST_MOTION, &v, 2000, &ev);
    CHECK(st.motion == 0 && ev == 0, "有人 → 没人：不出");
    v = vu(3); (void)mc_sub_apply(&st, MC_ST_MOTION, &v, 3000, &ev);
    CHECK(st.motion == 1 && ev == MC_SEV_MOTION_ON, "1 s 后又有人：照出（不限间隔：网关不做人为干预，2026-10-24）");
    CHECK(mc_sub_apply(&st, MC_ST_MOTION, &v, 3500, &ev) && ev == 0, "有人时再报有人：不重复出（只认翻转）");
    v = vu(0); (void)mc_sub_apply(&st, MC_ST_MOTION, &v, 4000, &ev);
    v = vu(1); (void)mc_sub_apply(&st, MC_ST_MOTION, &v, 4100, &ev);
    CHECK(st.motion == 1 && ev == MC_SEV_MOTION_ON, "没人 → 有人（bit0）：再出");

    v = vb(0); (void)mc_sub_apply(&st, MC_ST_LEAK, &v, 1, &ev);
    CHECK(ev == 0, "漏水首报不漏：不出");
    v = vb(1); (void)mc_sub_apply(&st, MC_ST_LEAK, &v, 2, &ev);
    CHECK(st.leak == 1 && ev == MC_SEV_LEAK, "漏水：出");
    v = vb(0); (void)mc_sub_apply(&st, MC_ST_LEAK, &v, 3, &ev);
    CHECK(st.leak == 0 && ev == MC_SEV_LEAK, "漏水解除：也出");

    v = vu(0); (void)mc_sub_apply(&st, MC_ST_SMOKE, &v, 1, &ev);
    CHECK(ev == 0, "烟雾首报正常：不出");
    v = vu(2); (void)mc_sub_apply(&st, MC_ST_SMOKE, &v, 2, &ev);
    CHECK(st.smoke == 1 && ev == MC_SEV_SMOKE, "烟雾 正常 → 严重：出");
    v = vu(1); (void)mc_sub_apply(&st, MC_ST_SMOKE, &v, 3, &ev);
    CHECK(st.smoke == 1 && ev == 0, "严重 → 警告：还是有烟，不出");
    v = vu(3);
    CHECK(!mc_sub_apply(&st, MC_ST_SMOKE, &v, 4, &ev), "SmokeState 3 不认识：丢掉");

    memset(&st, 0, sizeof(st));
    v = vu(200); (void)mc_sub_apply(&st, MC_ST_BATTERY, &v, 1000, &ev);
    CHECK(st.battery_pct == 100 && ev == 0, "电量 200 → 100 %");
    v = vu(30); (void)mc_sub_apply(&st, MC_ST_BATTERY, &v, 2000, &ev);
    CHECK(st.battery_pct == 15 && ev == MC_SEV_BATT_LOW, "降到 15 %：电量低");
    v = vu(40); (void)mc_sub_apply(&st, MC_ST_BATTERY, &v, 3000, &ev);
    v = vu(20); (void)mc_sub_apply(&st, MC_ST_BATTERY, &v, 4000, &ev);
    CHECK(st.battery_pct == 10 && ev == MC_SEV_BATT_LOW, "回到 20 % 又降到 10 %：再报（不再每天一条）");
    v = vu(18); (void)mc_sub_apply(&st, MC_ST_BATTERY, &v, 5000, &ev);
    CHECK(st.battery_pct == 9 && ev == 0, "一直低着：不重复报（只认降过门槛那一下）");
    v = vu(32); (void)mc_sub_apply(&st, MC_ST_BATTERY, &v, 5100, &ev);
    v = vu(30); (void)mc_sub_apply(&st, MC_ST_BATTERY, &v, 5200, &ev);
    v = vu(38); (void)mc_sub_apply(&st, MC_ST_BATTERY, &v, 5300, &ev);
    v = vu(28); (void)mc_sub_apply(&st, MC_ST_BATTERY, &v, 5400, &ev);
    CHECK(st.battery_pct == 14 && ev == 0, "在 15 % 上下抖（16 / 15 / 19 / 14）：没回到 20 % 不算恢复，不再报（回差）");
    mc_state_t s2;
    memset(&s2, 0, sizeof(s2));
    v = vu(10); (void)mc_sub_apply(&s2, MC_ST_BATTERY, &v, 7, &ev);
    CHECK(ev == MC_SEV_BATT_LOW, "开机第一报就低：报一次（同 Zigbee）");
    v = vu(201);
    CHECK(!mc_sub_apply(&s2, MC_ST_BATTERY, &v, 8, &ev), "电量 201 超范围：丢掉");

    v = vi(2215); (void)mc_sub_apply(&st, MC_ST_TEMP, &v, 1, &ev);
    CHECK(st.temp_c100 == 2215, "温度 0.01 °C");
    v = vi(-27316);
    CHECK(!mc_sub_apply(&st, MC_ST_TEMP, &v, 1, &ev), "温度低于绝对零度：丢掉");
    v = vu(4500); (void)mc_sub_apply(&st, MC_ST_HUM, &v, 1, &ev);
    CHECK(st.hum_c100 == 4500, "湿度 0.01 %");
    v = vu(10001);
    CHECK(!mc_sub_apply(&st, MC_ST_HUM, &v, 1, &ev), "湿度 > 100 %：丢掉");
    v = vi(1013); (void)mc_sub_apply(&st, MC_ST_PRESS, &v, 1, &ev);
    CHECK(st.press_hpa10 == 10130, "气压 1013（0.1 kPa）→ 1013.0 hPa");
    v = vu(0); (void)mc_sub_apply(&st, MC_ST_LUX, &v, 1, &ev);
    CHECK(st.lux == 0, "照度 0 = 太暗");
    v = vu(40001); (void)mc_sub_apply(&st, MC_ST_LUX, &v, 1, &ev);
    CHECK(st.lux == 10000, "照度 40001 → 10000 lx");
    v = vu(1); (void)mc_sub_apply(&st, MC_ST_LUX, &v, 1, &ev);
    CHECK(st.lux == 1, "照度 1 → 1 lx");
    v = vi(-1500); (void)mc_sub_apply(&st, MC_ST_POWER, &v, 1, &ev);
    CHECK(st.power_mw == -1500, "功率可以是负的（回馈）");
    v = vu(123456789); (void)mc_sub_apply(&st, MC_ST_ENERGY, &v, 1, &ev);
    CHECK(st.energy_mwh == 123456789, "电能按无符号编码也收");
    v = vi(-1);
    CHECK(!mc_sub_apply(&st, MC_ST_ENERGY, &v, 1, &ev), "累计电能是负的：丢掉");
    CHECK(!mc_sub_apply(&st, MC_ST_COUNT, &v, 1, &ev) && !mc_sub_apply(NULL, 0, &v, 1, &ev) && !mc_sub_apply(&st, 0, NULL, 1, &ev),
          "坏字段 / NULL：丢掉");

    /* 首报就是报警态（审计 ❌：停电期间漏了水，网关回来首报 water_leak=true 必须推） */
    {
        mc_state_t f;
        memset(&f, 0, sizeof(f));
        v = vb(1); (void)mc_sub_apply(&f, MC_ST_LEAK, &v, 1, &ev);
        CHECK(ev == MC_SEV_LEAK, "首报漏水：出");
        v = vu(1); (void)mc_sub_apply(&f, MC_ST_SMOKE, &v, 1, &ev);
        CHECK(ev == MC_SEV_SMOKE, "首报有烟（警告）：出");
        v = vb(0); (void)mc_sub_apply(&f, MC_ST_CONTACT, &v, 1, &ev);
        CHECK(f.contact_open == 1 && ev == MC_SEV_CONTACT, "首报门开着：出（同 Zigbee IAS）");
    }

    /* 路径相同的判定（审计：结构体末尾有填充，memcmp 会把没变的路径判成变了） */
    {
        mc_path_t x[MC_SUB_PATHS_MAX], y[MC_SUB_PATHS_MAX];
        memset(x, 0xAA, sizeof(x));
        memset(y, 0x55, sizeof(y));
        const int nx = mc_sub_paths(&e, x, MC_SUB_PATHS_MAX), ny = mc_sub_paths(&e, y, MC_SUB_PATHS_MAX);
        CHECK(nx == ny && mc_sub_paths_eq(x, y, nx), "同一份访谈两次挑出的路径：判相同（不受旧内容 / 填充影响）");
        CHECK(memcmp(x, y, sizeof(x)) == 0, "挑路径前清零：连填充与没用的格子都一样");
        y[1].at = 0x0009;
        CHECK(!mc_sub_paths_eq(x, y, nx), "一个属性号不同：判不同");
        CHECK(!mc_sub_paths_eq(NULL, y, 1), "NULL：判不同");
    }

    /* ---- 在线判定 ---- */
    CHECK(mc_sub_judge(-1, true, 0, 300, 999999) == 1, "订阅建好：在线");
    CHECK(mc_sub_judge(-1, false, 1000, 300, 1000 + 600000) == -1, "没建好、还没满 2 × 最大间隔：不变");
    CHECK(mc_sub_judge(1, false, 1000, 300, 1000 + 600001) == 0, "超过 2 × 300 s：离线");
    CHECK(mc_sub_judge(1, false, 1000, 10, 1000 + 59999) == 1 && mc_sub_judge(1, false, 1000, 10, 1000 + 60001) == 0,
          "最大间隔很短：至少等 60 s");
    CHECK(mc_sub_judge(0, false, 1000, 0, 1000 + 600001) == 0, "最大间隔还不知道：按上限 300 s 算");

    /* ---- 退避 ---- */
    CHECK(mc_sub_backoff_ms(1) == 5000 && mc_sub_backoff_ms(2) == 30000 && mc_sub_backoff_ms(3) == 120000 && mc_sub_backoff_ms(40) == 120000
          && mc_sub_backoff_ms(0) == 5000, "退避 5 → 30 → 120 s，之后一直 120 s");
    {
        uint32_t lo = UINT32_MAX, hi = 0;
        for (uint32_t r = 0; r < 1000; r++) {
            const uint32_t j = mc_sub_jitter(30000, r * 2654435761u);
            if (j < lo) lo = j;
            if (j > hi) hi = j;
        }
        CHECK(lo == 24000 && hi == 36000, "抖动 ±20 %（30 s → 24–36 s）");
    }

    /* ---- 控制（M7 第六步） ---- */
    {
        int bad = 0;
        for (unsigned pct = 1; pct <= 100; pct++) {
            const uint8_t l = mc_ctl_level((uint8_t)pct);
            mc_state_t t;
            memset(&t, 0, sizeof(t));
            v = vu(l); (void)mc_sub_apply(&t, MC_ST_LEVEL, &v, 1, &ev);
            if (l < 1 || l > 254 || (unsigned)t.level_pct + 1u < pct || (unsigned)t.level_pct > pct + 1u) bad++;
        }
        CHECK(bad == 0 && mc_ctl_level(0) == 0 && mc_ctl_level(100) == 254 && mc_ctl_level(1) >= 1, "亮度 % → level → % 往返在 ±1 % 内；0 / 100 两端");
        CHECK(mc_sub_first_ep(&e, 0x0006) == 1 && mc_sub_first_ep(&e, 0x0008) == -1 && mc_sub_first_ep(NULL, 6) == -1, "控制发往第一个有该簇的非根端点");

        mc_state_t c;
        memset(&c, 0, sizeof(c));
        CHECK(!mc_ctl_already(&c, MATTER_CTRL_ACT_ON, 0) && !mc_ctl_reached(&c, MATTER_CTRL_ACT_ON, 1, 0, 100), "不知道开关状态：不算已经是、也不算到了");
        v = vb(1); (void)mc_sub_apply(&c, MC_ST_ONOFF, &v, 50, &ev);
        CHECK(mc_ctl_already(&c, MATTER_CTRL_ACT_ON, 0) && !mc_ctl_already(&c, MATTER_CTRL_ACT_OFF, 0) && !mc_ctl_already(&c, MATTER_CTRL_ACT_TOGGLE, 0),
              "开着：开 = 已经是；关 / 切换不是");
        CHECK(!mc_ctl_reached(&c, MATTER_CTRL_ACT_ON, 1, 0, 100), "开着、但报告早于发出：不算到了（要 3 s 内的新报告）");
        v = vb(0); (void)mc_sub_apply(&c, MC_ST_ONOFF, &v, 120, &ev);
        CHECK(mc_ctl_reached(&c, MATTER_CTRL_ACT_OFF, 0, 0, 100) && !mc_ctl_reached(&c, MATTER_CTRL_ACT_ON, 1, 0, 100), "发出后报了关：关到了、开没到");
        CHECK(mc_ctl_reached(&c, MATTER_CTRL_ACT_TOGGLE, 0, 0, 100) && !mc_ctl_reached(&c, MATTER_CTRL_ACT_TOGGLE, 1, 0, 100)
              && mc_ctl_reached(&c, MATTER_CTRL_ACT_TOGGLE, -1, 0, 100), "切换：知道目标按目标判；不知道之前的状态，报过就算");
        CHECK(mc_ctl_reached(&c, MATTER_CTRL_ACT_LEVEL, 0, 0, 100) && mc_ctl_already(&c, MATTER_CTRL_ACT_LEVEL, 0), "亮度 0 % 按关判");
        v = vb(1); (void)mc_sub_apply(&c, MC_ST_ONOFF, &v, 130, &ev);
        v = vu(127); (void)mc_sub_apply(&c, MC_ST_LEVEL, &v, 140, &ev);
        CHECK(mc_ctl_reached(&c, MATTER_CTRL_ACT_LEVEL, -1, 51, 100) && !mc_ctl_reached(&c, MATTER_CTRL_ACT_LEVEL, -1, 60, 100)
              && !mc_ctl_reached(&c, MATTER_CTRL_ACT_LEVEL, -1, 50, 150), "亮度：±1 % 内、报告晚于发出才算到");
        CHECK(mc_ctl_already(&c, MATTER_CTRL_ACT_LEVEL, 49) && !mc_ctl_already(&c, MATTER_CTRL_ACT_LEVEL, 70), "亮度已经是（亮着、±1 %）");
        /* 灯关着、亮度本来就是 50 %，"开到 50 %"：MoveToLevelWithOnOff 只翻开关，亮度不报（审计） */
        mc_state_t g;
        memset(&g, 0, sizeof(g));
        v = vb(0); (void)mc_sub_apply(&g, MC_ST_ONOFF, &v, 10, &ev);
        v = vu(127); (void)mc_sub_apply(&g, MC_ST_LEVEL, &v, 10, &ev);
        CHECK(!mc_ctl_already(&g, MATTER_CTRL_ACT_LEVEL, 50) && !mc_ctl_reached(&g, MATTER_CTRL_ACT_LEVEL, 1, 50, 100), "关着：还没到");
        v = vb(1); (void)mc_sub_apply(&g, MC_ST_ONOFF, &v, 150, &ev);
        CHECK(mc_ctl_reached(&g, MATTER_CTRL_ACT_LEVEL, 1, 50, 100), "发出后只报了开、亮度本来就是：算到了");
        CHECK(!mc_ctl_reached(&g, MATTER_CTRL_ACT_LEVEL, 1, 80, 100), "亮度对不上：没到");
        CHECK(!mc_ctl_reached(NULL, MATTER_CTRL_ACT_ON, 1, 0, 0) && !mc_ctl_reached(&c, 99, 1, 0, 0) && !mc_ctl_already(&c, 99, 0), "坏参数：否");
    }

    printf("mc_sub: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
