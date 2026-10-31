/* zh_tuya_test —— zh_tuya.c（涂鸦私有簇 0xEF00）与生成的 zh_tuya_db.c 的主机测试。
 *
 * T1–T4 线上格式（迭代器、解码）的真值表；T5 表本身（有序、偏移不越界、查找优先级）；
 * T6 金样例：zh_tuya_gold.inc 是导出脚本拿同一批报文喂 zigbee-herdsman-converters 得到的输出（目标单位），这里把报文按线上字节拼出来、
 *    走一遍迭代器 + 解码 + 查表 + 换算，逐条对照 —— 证明固件读出来的数和 Zigbee2MQTT 一样（只差取整；LIN16 的负值比库更准一格，见 T6 注释）；
 * T7 主报警字段；T8 手核的几台（不经导出脚本，照库的源码算）；T9 模糊。
 * 写方向（M5 第九步）：T10 写方向金样例 zh_tuya_gold_ctl.inc（库的 toZigbee 对开 / 关 / 亮度发出的数据点）逐条对照 zh_tuya_enc_ctl 编出的负载；
 * T11 编码的边界（亮度 0 = 关、先开再设亮度、封顶、seq 小端、sendData、没有写法 / 写不下）。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zh_tuya.h"
#include "zh_zcl.h"

static int s_pass, s_fail;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { const char *mfr, *model; int dp, type, raw, field, want; } gold_t;
static const gold_t k_gold[] = {
#include "zh_tuya_gold.inc"
};
typedef struct { const char *mfr, *model; int act, pct, cmd, dp, type, raw, tol; } gold_ctl_t;
static const gold_ctl_t k_gold_ctl[] = {
#include "zh_tuya_gold_ctl.inc"
};

/* 拼一条数据点：[dp][类型][长度 大端][数据] */
static size_t put_dp(uint8_t *b, int dp, int type, int raw)
{
    b[0] = (uint8_t)dp;
    b[1] = (uint8_t)type;
    size_t n;
    if (type == ZH_TUYA_T_NUMBER) {
        const uint32_t u = (uint32_t)raw;
        b[4] = (uint8_t)(u >> 24); b[5] = (uint8_t)(u >> 16); b[6] = (uint8_t)(u >> 8); b[7] = (uint8_t)u;
        n = 4;
    } else {
        b[4] = (uint8_t)raw;
        n = 1;
    }
    b[2] = 0;
    b[3] = (uint8_t)n;
    return 4 + n;
}

static void t1_cmds(void)
{
    CHECK(zh_tuya_is_data_cmd(0x01) && zh_tuya_is_data_cmd(0x02) && zh_tuya_is_data_cmd(0x05) && zh_tuya_is_data_cmd(0x06), "T1 四条带读数的命令");
    CHECK(!zh_tuya_is_data_cmd(0x00) && !zh_tuya_is_data_cmd(0x03) && !zh_tuya_is_data_cmd(0x11) && !zh_tuya_is_data_cmd(0x24), "T1 别的命令（dataRequest / dataQuery / MCU 版本 / 对时）不是读数");
}

static void t2_iter(void)
{
    uint8_t b[64] = { 0x34, 0x12 };                /* seq */
    size_t n = 2;
    n += put_dp(b + n, 1, ZH_TUYA_T_NUMBER, 215);
    n += put_dp(b + n, 3, ZH_TUYA_T_ENUM, 2);
    n += put_dp(b + n, 9, ZH_TUYA_T_BOOL, 1);
    zh_tuya_iter_t it;
    zh_tuya_dp_t dp;
    CHECK(zh_tuya_iter_init(&it, b, n), "T2 init");
    CHECK(zh_tuya_next(&it, &dp) == 1 && dp.dp == 1 && dp.type == 2 && dp.len == 4 && dp.data == b + 6, "T2 第一条");
    CHECK(zh_tuya_next(&it, &dp) == 1 && dp.dp == 3 && dp.type == 4 && dp.len == 1 && dp.data[0] == 2, "T2 第二条");
    CHECK(zh_tuya_next(&it, &dp) == 1 && dp.dp == 9 && dp.type == 1 && dp.data[0] == 1, "T2 第三条");
    CHECK(zh_tuya_next(&it, &dp) == 0 && zh_tuya_next(&it, &dp) == 0, "T2 正好完，再要还是 0");
    CHECK(zh_tuya_iter_init(&it, b, 2) && zh_tuya_next(&it, &dp) == 0, "T2 只有 seq：0 条");
    CHECK(!zh_tuya_iter_init(&it, b, 1) && !zh_tuya_iter_init(&it, b, 0) && !zh_tuya_iter_init(&it, NULL, 8) && !zh_tuya_iter_init(NULL, b, 8), "T2 不到 2 字节 / 空指针");
    /* 零长度的数据点（raw / string 可以是空的）：拿得到，解不成整数 */
    const uint8_t z[] = { 0, 0, 7, ZH_TUYA_T_RAW, 0, 0, 8, ZH_TUYA_T_ENUM, 0, 1, 5 };
    int32_t v;
    CHECK(zh_tuya_iter_init(&it, z, sizeof(z)) && zh_tuya_next(&it, &dp) == 1 && dp.dp == 7 && dp.len == 0 && !zh_tuya_dp_int(&dp, &v), "T2 零长度 raw");
    CHECK(zh_tuya_next(&it, &dp) == 1 && dp.dp == 8 && zh_tuya_dp_int(&dp, &v) && v == 5 && zh_tuya_next(&it, &dp) == 0, "T2 零长度之后的照常");
}

static void t3_truncated(void)
{
    uint8_t b[64] = { 0, 0 };
    size_t n = 2;
    n += put_dp(b + n, 1, ZH_TUYA_T_NUMBER, 7);
    const size_t good = n;
    n += put_dp(b + n, 2, ZH_TUYA_T_NUMBER, 9);
    zh_tuya_iter_t it;
    zh_tuya_dp_t dp;
    for (size_t cut = good + 1; cut < n; cut++) {   /* 第二条截在头里 / 数据里 */
        CHECK(zh_tuya_iter_init(&it, b, cut) && zh_tuya_next(&it, &dp) == 1 && dp.dp == 1, "T3 截断在 %zu：前一条照收", cut);
        CHECK(zh_tuya_next(&it, &dp) == -1 && zh_tuya_next(&it, &dp) == 0, "T3 截断在 %zu：最后一条丢掉、之后是 0", cut);
    }
    const uint8_t big[] = { 0, 0, 1, ZH_TUYA_T_RAW, 0xFF, 0xFF, 1, 2, 3 };   /* 长度 65535 */
    CHECK(zh_tuya_iter_init(&it, big, sizeof(big)) && zh_tuya_next(&it, &dp) == -1, "T3 长度远超剩下的");
}

static void t4_decode(void)
{
    int32_t v = 99;
    zh_tuya_dp_t d = { 1, ZH_TUYA_T_BOOL, 1, (const uint8_t *)"\x01" };
    CHECK(zh_tuya_dp_int(&d, &v) && v == 1, "T4 bool 1");
    d.data = (const uint8_t *)"\x00";
    CHECK(zh_tuya_dp_int(&d, &v) && v == 0, "T4 bool 0");
    d.data = (const uint8_t *)"\x02";
    CHECK(zh_tuya_dp_int(&d, &v) && v == 0, "T4 bool 2：只有 1 算真（tuya.js:199）");
    d.type = ZH_TUYA_T_ENUM;
    d.data = (const uint8_t *)"\xFE";
    CHECK(zh_tuya_dp_int(&d, &v) && v == 254, "T4 enum");
    d.type = ZH_TUYA_T_NUMBER; d.len = 4; d.data = (const uint8_t *)"\xFF\xFF\xFF\xF6";
    CHECK(zh_tuya_dp_int(&d, &v) && v == -10, "T4 4 字节数值是 int32 补码");
    d.data = (const uint8_t *)"\x00\x00\xFF\xF6";
    CHECK(zh_tuya_dp_int(&d, &v) && v == 65526, "T4 低 16 位的负数（解码层不管，LIN16 行才管）");
    d.len = 2; d.data = (const uint8_t *)"\xFF\xF6";
    CHECK(zh_tuya_dp_int(&d, &v) && v == 65526, "T4 不足 4 字节是无符号（JS 的 << 只在第 4 字节进符号位）");
    d.type = ZH_TUYA_T_BITMAP; d.len = 1; d.data = (const uint8_t *)"\x81";
    CHECK(zh_tuya_dp_int(&d, &v) && v == 0x81, "T4 bitmap");
    d.type = ZH_TUYA_T_NUMBER; d.len = 5; d.data = (const uint8_t *)"\0\0\0\0\1";
    CHECK(!zh_tuya_dp_int(&d, &v), "T4 数值超过 4 字节不解");
    d.len = 0;
    CHECK(!zh_tuya_dp_int(&d, &v), "T4 数值 0 字节不解");
    d.type = ZH_TUYA_T_STRING; d.len = 2; d.data = (const uint8_t *)"ab";
    CHECK(!zh_tuya_dp_int(&d, &v), "T4 string 不解");
    d.type = ZH_TUYA_T_RAW;
    CHECK(!zh_tuya_dp_int(&d, &v), "T4 raw 不解");
    d.type = 9;
    CHECK(!zh_tuya_dp_int(&d, &v), "T4 不认识的类型");
    CHECK(!zh_tuya_dp_int(NULL, &v) && !zh_tuya_dp_int(&d, NULL), "T4 空指针");
}

static int fp_cmp(const zh_tdb_fp_t *a, const zh_tdb_fp_t *b)
{
    int c = strcmp(zh_tdb_str + a->mfr, zh_tdb_str + b->mfr);
    return c ? c : strcmp(zh_tdb_str + a->model, zh_tdb_str + b->model);
}

static void t5_table(void)
{
    CHECK(zh_tdb_n_fps > 1000 && zh_tdb_n_devs > 100 && zh_tdb_n_rows > 300, "T5 表的规模（指纹 %u / 组合 %u / 行 %u）", zh_tdb_n_fps, zh_tdb_n_devs, zh_tdb_n_rows);
    CHECK(strncmp(zh_tdb_source, "zigbee-herdsman-converters ", 27) == 0, "T5 来源串");
    CHECK(zh_tdb_str[zh_tdb_str_len - 1] == 0, "T5 字符串池以 0 结尾");
    unsigned bad_off = 0, bad_dev = 0, unsorted = 0, bad_row = 0, bad_fields = 0, miss = 0;
    for (unsigned i = 0; i < zh_tdb_n_fps; i++) {
        const zh_tdb_fp_t *f = &zh_tdb_fps[i];
        if (f->mfr >= zh_tdb_str_len || f->model >= zh_tdb_str_len) { bad_off++; continue; }
        if (f->dev >= zh_tdb_n_devs) bad_dev++;
        if (i && fp_cmp(&zh_tdb_fps[i - 1], f) >= 0) unsorted++;   /* 严格递增：也就没有重复 */
        const char *m = zh_tdb_str + f->mfr, *o = zh_tdb_str + f->model;
        if (!m[0] && !o[0]) bad_off++;
        /* 每条指纹都查得到自己（厂商名 + 型号齐的按精确；只有一边的按那一边） */
        if (m[0] && o[0] && zh_tuya_find(m, o) != f->dev) miss++;
        if (m[0] && !o[0] && zh_tuya_find(m, "ZZ-NO-SUCH-MODEL") != f->dev) miss++;
        if (!m[0] && o[0] && zh_tuya_find("_ZZ_NO_SUCH_", o) != f->dev) miss++;
    }
    for (unsigned i = 0; i < zh_tdb_n_devs; i++) {
        const zh_tdb_dev_t *d = &zh_tdb_devs[i];
        if (!d->n_rows || (unsigned)d->row + d->n_rows > zh_tdb_n_rows) { bad_row++; continue; }
        uint32_t f = 0;
        for (unsigned k = 0; k < d->n_rows; k++) {
            const zh_tdb_row_t *r = &zh_tdb_rows[d->row + k];
            f |= 1u << r->field;
            if (r->field == ZH_TF_NONE || r->field >= ZH_TF_COUNT || r->kind < ZH_TDB_LIN || r->kind > ZH_TDB_LIN16) bad_row++;
            if (k && (zh_tdb_rows[d->row + k - 1].dp > r->dp)) bad_row++;
        }
        if (f != d->fields) bad_fields++;
    }
    CHECK(!bad_off && !bad_dev, "T5 偏移 / 设备下标不越界（%u / %u）", bad_off, bad_dev);
    CHECK(!unsorted, "T5 指纹按（厂商名，型号）严格 sorted（二分的前提）：乱序 %u 处", unsorted);
    CHECK(!bad_row && !bad_fields, "T5 行范围 / 字段 / 换算种类合法、按 dp 排好，fields 位与行一致（%u / %u）", bad_row, bad_fields);
    CHECK(!miss, "T5 每条指纹都查得到自己：%u 条没查到", miss);
    CHECK(zh_tuya_find("_ZZ_NO_SUCH_", "ZZ-NO-SUCH") == -1 && zh_tuya_find("", "") == -1 && zh_tuya_find(NULL, NULL) == -1, "T5 不在表里 / 空串 / 空指针 = -1");
    /* 优先级：厂商名 + 任意型号（_TZE200_f1pvdgoh 的指纹没写型号）先于只按型号（0yu2xgi 只写了 zigbeeModel）——同 z2m 先认指纹再认型号 */
    const int by_mfr = zh_tuya_find("_TZE200_f1pvdgoh", "ZZ-NO-SUCH"), by_model = zh_tuya_find("_ZZ_NO_SUCH_", "0yu2xgi");
    CHECK(by_mfr >= 0 && by_model >= 0 && by_mfr != by_model && zh_tuya_find("_TZE200_f1pvdgoh", "0yu2xgi") == by_mfr, "T5 两边都能匹配时厂商名那条优先");
    CHECK(zh_tuya_fields(-1) == 0 && zh_tuya_fields((int)zh_tdb_n_devs) == 0, "T5 fields 下标非法 = 0");
    zh_tuya_val_t o[4];
    CHECK(zh_tuya_map(-1, 1, 0, o, 4) == 0 && zh_tuya_map((int)zh_tdb_n_devs, 1, 0, o, 4) == 0 && zh_tuya_map(0, 1, 0, NULL, 4) == 0, "T5 map 下标非法 / 空指针 = 0");
}

/* T6 金样例。LIN 允许差 1（库是 Math.round(y × 单位)，我们是整数乘数 × 原始值再四舍五入）；LIN16 行的 0x8000–0xFFFF 允许差一个原始单位：
   库里老式转换器写的是 value − 0xffff（−1.0 °C 解成 −0.9 °C），我们按 16 位补码解（−1.0 °C） */
static void t6_gold(void)
{
    unsigned n = 0, bad = 0, nf = 0;
    for (size_t i = 0; i < sizeof(k_gold) / sizeof(k_gold[0]); i++) {
        const gold_t *g = &k_gold[i];
        const int dev = zh_tuya_find(g->mfr, g->model);
        if (dev < 0) { nf++; if (nf < 5) printf("  T6 查不到 %s / %s\n", g->mfr, g->model); continue; }
        uint8_t b[16] = { 0x01, 0x00 };
        const size_t len = 2 + put_dp(b + 2, g->dp, g->type, g->raw);
        zh_tuya_iter_t it;
        zh_tuya_dp_t dp;
        int32_t raw;
        if (!zh_tuya_iter_init(&it, b, len) || zh_tuya_next(&it, &dp) != 1 || !zh_tuya_dp_int(&dp, &raw)) { bad++; continue; }
        zh_tuya_val_t o[4];
        const unsigned m = zh_tuya_map(dev, dp.dp, raw, o, 4);
        const zh_tuya_val_t *hit = NULL;
        for (unsigned k = 0; k < m; k++) if (o[k].field == g->field) hit = &o[k];
        long tol = 0;
        if (g->field == ZH_TF_ONOFF || g->field == ZH_TF_BATTERY_LOW || g->field == ZH_TF_OCCUPANCY || g->field >= ZH_TF_CONTACT) tol = 0;
        else {
            tol = 1;
            for (unsigned k = 0; k < zh_tdb_devs[dev].n_rows; k++) {
                const zh_tdb_row_t *r = &zh_tdb_rows[zh_tdb_devs[dev].row + k];
                if (r->dp == g->dp && r->field == g->field && r->kind == ZH_TDB_LIN16 && g->raw >= 0x8000) tol = labs((long)r->mul) / 10000 + 1;
            }
        }
        if (!hit || labs((long)hit->val - g->want) > tol) {
            bad++;
            if (bad < 8) printf("  T6 %s/%s dp%d 类型 %d 原始 %d 字段 %d：要 %d，得 %s%ld\n", g->mfr, g->model, g->dp, g->type, g->raw, g->field, g->want, hit ? "" : "（没有）", hit ? (long)hit->val : 0L);
        }
        n++;
    }
    CHECK(nf == 0, "T6 金样例的指纹都查得到（%u 条查不到）", nf);
    CHECK(bad == 0 && n >= 1000, "T6 金样例 %u 条：固件的解码 + 换算与库的输出一致（不一致 %u 条）", n, bad);
}

static void t7_alarm(void)
{
    CHECK(zh_tbl_alarm_field((1u << ZH_TF_SMOKE) | (1u << ZH_TF_CO)) == ZH_TF_SMOKE, "T7 烟雾 + CO：主报警是烟雾");
    CHECK(zh_tbl_alarm_field((1u << ZH_TF_CONTACT) | (1u << ZH_TF_VIBRATION) | (1u << ZH_TF_TAMPER)) == ZH_TF_CONTACT, "T7 门磁 + 振动 + 防拆：门磁");
    CHECK(zh_tbl_alarm_field((1u << ZH_TF_TEMP) | (1u << ZH_TF_BATTERY_LOW) | (1u << ZH_TF_TAMPER)) == ZH_TF_NONE, "T7 电量低 / 防拆不算主报警");
    CHECK(zh_tbl_zone_type(1u << ZH_TF_WATER) == ZH_ZONE_WATER && zh_tbl_zone_type(1u << ZH_TF_SMOKE) == ZH_ZONE_FIRE && zh_tbl_zone_type(1u << ZH_TF_GAS) == ZH_ZONE_GAS &&
          zh_tbl_zone_type(1u << ZH_TF_CO) == ZH_ZONE_CO && zh_tbl_zone_type(1u << ZH_TF_CONTACT) == ZH_ZONE_CONTACT && zh_tbl_zone_type(1u << ZH_TF_VIBRATION) == ZH_ZONE_VIBRATION,
          "T7 ZoneType 对照");
    CHECK(zh_tbl_zone_type(1u << ZH_TF_OCCUPANCY) == 0xFFFFu && zh_tbl_zone_type(0) == 0xFFFFu, "T7 占用不走 IAS（走占用那路）、没有报警字段 = 0xFFFF");
}

static int32_t map1(const char *mfr, const char *model, int dp, int32_t raw, unsigned field, int *found)
{
    const int dev = zh_tuya_find(mfr, model);
    zh_tuya_val_t o[4];
    const unsigned m = dev >= 0 ? zh_tuya_map(dev, (uint8_t)dp, raw, o, 4) : 0;
    for (unsigned k = 0; k < m; k++) if (o[k].field == field) { *found = 1; return o[k].val; }
    *found = 0;
    return 0;
}

/* T8 手核：照库源码算（不经导出脚本），钉住几台最常见的 */
static void t8_known(void)
{
    int f;
    /* legacy.js tuya_temperature_humidity_sensor：dp1 温度 /10（value > 0x2000 当负）、dp2 湿度对 _TZE200_bjawzodf /10、dp4 电量 */
    CHECK(map1("_TZE200_bjawzodf", "TS0601", 1, 215, ZH_TF_TEMP, &f) == 2150 && f, "T8 bjawzodf 21.5 °C");
    CHECK(map1("_TZE200_bjawzodf", "TS0601", 1, 0xFFF6, ZH_TF_TEMP, &f) == -100 && f, "T8 bjawzodf −1.0 °C（低 16 位补码）");
    CHECK(map1("_TZE200_bjawzodf", "TS0601", 2, 455, ZH_TF_HUMIDITY, &f) == 4550 && f, "T8 bjawzodf 湿度 45.5 %%（这一家 ÷10）");
    CHECK(map1("_TZE200_bjawzodf", "TS0601", 4, 87, ZH_TF_BATTERY_PCT, &f) == 87 && f, "T8 bjawzodf 电量");
    /* 新式 _TZE200_ntcy3xu1 烟雾：dp1 trueFalse0（0 = 有烟）、dp4 防拆、dp14 battery_state（0 = low） */
    CHECK(map1("_TZE200_ntcy3xu1", "TS0601", 1, 0, ZH_TF_SMOKE, &f) == 1 && f && map1("_TZE200_ntcy3xu1", "TS0601", 1, 1, ZH_TF_SMOKE, &f) == 0, "T8 ntcy3xu1 烟雾 0 = 有烟");
    CHECK(map1("_TZE200_ntcy3xu1", "TS0601", 14, 0, ZH_TF_BATTERY_LOW, &f) == 1 && f && map1("_TZE200_ntcy3xu1", "TS0601", 14, 2, ZH_TF_BATTERY_LOW, &f) == 0, "T8 ntcy3xu1 电量低");
    /* 门磁 _TZE200_pay2byax：dp1 bool，true = 开（z2m contact = !v） */
    CHECK(map1("_TZE200_pay2byax", "TS0601", 1, 1, ZH_TF_CONTACT, &f) == 1 && f && map1("_TZE200_pay2byax", "TS0601", 1, 0, ZH_TF_CONTACT, &f) == 0, "T8 pay2byax 门开");
    /* 人体 _TZE200_3towulqd：dp1 trueFalse0（0 = 有人） */
    CHECK(map1("_TZE200_3towulqd", "TS0601", 1, 0, ZH_TF_OCCUPANCY, &f) == 1 && f && map1("_TZE200_3towulqd", "TS0601", 1, 1, ZH_TF_OCCUPANCY, &f) == 0, "T8 3towulqd 有人");
    /* 不在表里的 dp：没有读数 */
    CHECK(map1("_TZE200_bjawzodf", "TS0601", 200, 1, ZH_TF_TEMP, &f) == 0 && !f, "T8 表里没有的 dp");
}

static uint32_t s_lcg = 12345;
static uint32_t rnd(void) { s_lcg = s_lcg * 1103515245u + 12345u; return s_lcg >> 8; }

static void t9_fuzz(void)
{
    unsigned got = 0;
    for (int round = 0; round < 20000; round++) {
        uint8_t b[40];
        const size_t n = rnd() % sizeof(b);
        for (size_t i = 0; i < n; i++) b[i] = (uint8_t)rnd();
        if (round & 1 && n > 6) { b[4] = 0; b[5] = (uint8_t)(rnd() % 6); }   /* 一半让长度像样，走得进数据 */
        int fdev = (int)(rnd() % (zh_tdb_n_devs + 2)) - 1;
        if (round % 4 == 1 && n > 6 && fdev >= 0 && (unsigned)fdev < zh_tdb_n_devs) {                            /* 四分之一用这台设备表里真有的 dp：换算那条路要真走到 */
            const zh_tdb_dev_t *dv = &zh_tdb_devs[fdev];
            b[2] = zh_tdb_rows[dv->row + rnd() % dv->n_rows].dp;
        }
        uint8_t *p = malloc(n ? n : 1);            /* 正好 n 字节：越界读 ASan 一抓就是 */
        memcpy(p, b, n);
        zh_tuya_iter_t it;
        zh_tuya_dp_t dp;
        if (zh_tuya_iter_init(&it, p, n)) {
            int r, k = 0;
            while ((r = zh_tuya_next(&it, &dp)) == 1 && k++ < 64) {
                int32_t v;
                if (zh_tuya_dp_int(&dp, &v)) {
                    zh_tuya_val_t o[4];
                    got += zh_tuya_map(fdev, dp.dp, v, o, 4);
                }
            }
            CHECK(r != 1, "T9 迭代会结束");
        }
        free(p);
    }
    CHECK(got > 0, "T9 模糊里换算出过读数（%u 条）", got);
}


/* 负载里第 i 条数据点（0 起）；没有回 0 */
static int nth_dp(const uint8_t *pl, size_t n, unsigned i, zh_tuya_dp_t *out)
{
    zh_tuya_iter_t it;
    if (!zh_tuya_iter_init(&it, pl, n)) return 0;
    for (unsigned k = 0; zh_tuya_next(&it, out) == 1; k++) if (k == i) return 1;
    return 0;
}

static void t10_gold_ctl(void)
{
    unsigned n = 0, bad = 0;
    for (size_t i = 0; i < sizeof(k_gold_ctl) / sizeof(k_gold_ctl[0]); i++) {
        const gold_ctl_t *g = &k_gold_ctl[i];
        const int dev = zh_tuya_find(g->mfr, g->model);
        uint8_t pl[ZH_TUYA_CTL_PL_MAX], cmd = 0xEE, cdp = 0;
        int32_t craw = -99;
        const unsigned act = g->act == 0 ? ZH_TUYA_ACT_ON : g->act == 1 ? ZH_TUYA_ACT_OFF : ZH_TUYA_ACT_LEVEL;
        const size_t len = zh_tuya_enc_ctl(dev, act, (uint8_t)g->pct, 0x1234, pl, sizeof(pl), &cmd, &cdp, &craw);
        /* 亮度那条：有开关 dp 时第一条是"开"，亮度是最后一条 */
        zh_tuya_dp_t dp;
        unsigned cnt = 0;
        while (len && nth_dp(pl, len, cnt, &dp)) cnt++;
        int32_t raw = 0;
        const int ok = len && cnt >= 1 && nth_dp(pl, len, cnt - 1, &dp) && zh_tuya_dp_int(&dp, &raw) && cmd == g->cmd && dp.dp == g->dp && dp.type == g->type &&
                       cdp == g->dp && craw == raw && labs((long)raw - g->raw) <= g->tol && pl[0] == 0x34 && pl[1] == 0x12;
        if (!ok) {
            bad++;
            if (bad < 8) printf("  T10 %s/%s 动作 %d %d%%：要 cmd %d dp%d 类型 %d 原始 %d（容差 %d），得 len %zu cmd %d dp%d 类型 %d 原始 %d\n", g->mfr, g->model, g->act, g->pct,
                                g->cmd, g->dp, g->type, g->raw, g->tol, len, cmd, dp.dp, dp.type, (int)raw);
        }
        n++;
    }
    CHECK(bad == 0 && n >= 100, "T10 写方向金样例 %u 条：固件编出的命令号 / dp / 类型 / 原始值与库的 toZigbee 一致（不一致 %u 条）", n, bad);
}

static void t11_enc(void)
{
    uint8_t pl[ZH_TUYA_CTL_PL_MAX], cmd, cdp;
    int32_t craw = -99;
    const int dim = zh_tuya_find("_TZE200_swaamsoy", "TS0601");    /* 调光器：dp1 开关（布尔）、dp2 亮度 0–1000（量程下限 12） */
    CHECK(dim >= 0 && zh_tuya_ctl_fields(dim) == ((1u << ZH_TF_ONOFF) | (1u << ZH_TF_LEVEL)), "T11 调光器能写开关与亮度");
    size_t n = zh_tuya_enc_ctl(dim, ZH_TUYA_ACT_ON, 0, 0xABCD, pl, sizeof(pl), &cmd, &cdp, &craw);
    CHECK(n == 7 && pl[0] == 0xCD && pl[1] == 0xAB && memcmp(pl + 2, (const uint8_t[]){ 1, ZH_TUYA_T_BOOL, 0, 1, 1 }, 5) == 0 && cmd == ZH_TUYA_CMD_DATA_REQUEST && cdp == 1,
          "T11 开：[seq 小端][dp1 布尔 1]、dataRequest、确认看 dp1");
    CHECK(craw == 1, "T11 开：确认要的值是 1");
    n = zh_tuya_enc_ctl(dim, ZH_TUYA_ACT_LEVEL, 50, 1, pl, sizeof(pl), &cmd, &cdp, &craw);
    CHECK(n == 2 + 5 + 8 && memcmp(pl + 2, (const uint8_t[]){ 1, ZH_TUYA_T_BOOL, 0, 1, 1, 2, ZH_TUYA_T_NUMBER, 0, 4, 0, 0, 0x01, 0xF4 }, 13) == 0 && cdp == 2,
          "T11 亮度 50 %%：同一帧先开（dp1）、再设 dp2 = 500，确认看 dp2");
    CHECK(craw == 500, "T11 亮度 50 %%：确认要的值是 500");
    n = zh_tuya_enc_ctl(dim, ZH_TUYA_ACT_LEVEL, 0, 1, pl, sizeof(pl), &cmd, &cdp, &craw);
    CHECK(n == 7 && pl[2] == 1 && pl[6] == 0 && cdp == 1 && craw == 0, "T11 亮度 0 %% = 关（dp1 = 0，确认要 0）");
    n = zh_tuya_enc_ctl(dim, ZH_TUYA_ACT_LEVEL, 1, 1, pl, sizeof(pl), &cmd, &cdp, &craw);
    CHECK(n == 15 && pl[11] == 0 && pl[12] == 0 && pl[13] == 0 && pl[14] == 12, "T11 亮度 1 %%：封在库发过的下限 12");
    n = zh_tuya_enc_ctl(dim, ZH_TUYA_ACT_LEVEL, 100, 1, pl, sizeof(pl), &cmd, &cdp, &craw);
    CHECK(n == 15 && pl[13] == 0x03 && pl[14] == 0xE8, "T11 亮度 100 %%：1000（拟合算到量程外的封在上限）");
    CHECK(zh_tuya_enc_ctl(dim, ZH_TUYA_ACT_LEVEL, 101, 1, pl, sizeof(pl), &cmd, &cdp, &craw) == 0, "T11 超过 100 %% 不编");
    CHECK(zh_tuya_enc_ctl(dim, ZH_TUYA_ACT_LEVEL, 50, 1, pl, 14, &cmd, &cdp, &craw) == 0 && zh_tuya_enc_ctl(dim, ZH_TUYA_ACT_ON, 0, 1, pl, 6, &cmd, &cdp, &craw) == 0, "T11 写不下：0");
    CHECK(zh_tuya_enc_ctl(dim, 9, 0, 1, pl, sizeof(pl), &cmd, &cdp, &craw) == 0 && zh_tuya_enc_ctl(dim, ZH_TUYA_ACT_ON, 0, 1, NULL, 9, &cmd, &cdp, &craw) == 0 &&
          zh_tuya_enc_ctl(dim, ZH_TUYA_ACT_ON, 0, 1, pl, sizeof(pl), NULL, &cdp, &craw) == 0 && zh_tuya_enc_ctl(dim, ZH_TUYA_ACT_ON, 0, 1, pl, sizeof(pl), &cmd, &cdp, NULL) == 0,
          "T11 动作非法 / 空指针：0");
    const int th = zh_tuya_find("_TZE200_bjawzodf", "TS0601");
    CHECK(zh_tuya_ctl_fields(th) == 0 && zh_tuya_enc_ctl(th, ZH_TUYA_ACT_ON, 0, 1, pl, sizeof(pl), &cmd, &cdp, &craw) == 0 && zh_tuya_ctl_fields(-1) == 0,
          "T11 温湿度计没有写法：0");
    const int lv = zh_tuya_find("_TZE204_djurk6p5", "TS0601");         /* 只能写亮度、没有开关 dp */
    CHECK(lv >= 0 && zh_tuya_ctl_fields(lv) == (1u << ZH_TF_LEVEL), "T11 只有亮度写法的设备");
    CHECK(zh_tuya_enc_ctl(lv, ZH_TUYA_ACT_LEVEL, 0, 1, pl, sizeof(pl), &cmd, &cdp, &craw) == 0 && zh_tuya_enc_ctl(lv, ZH_TUYA_ACT_ON, 0, 1, pl, sizeof(pl), &cmd, &cdp, &craw) == 0,
          "T11 没有开关 dp：亮度 0 %% / 开都不编（不拿亮度冒充关）");
    n = zh_tuya_enc_ctl(lv, ZH_TUYA_ACT_LEVEL, 60, 1, pl, sizeof(pl), &cmd, &cdp, &craw);
    CHECK(n == 10, "T11 没有开关 dp 的亮度：只有一条");
    const int valve = zh_tuya_find("_ANY_", "TS0049");                  /* 只按型号认的水阀：sendData */
    n = valve >= 0 ? zh_tuya_enc_ctl(valve, ZH_TUYA_ACT_OFF, 0, 7, pl, sizeof(pl), &cmd, &cdp, &craw) : 0;
    CHECK(n > 2 && cmd == ZH_TUYA_CMD_SEND_DATA, "T11 定义里选了 sendData 的设备：命令号 0x04");
}

int main(void)
{
    t1_cmds();
    t2_iter();
    t3_truncated();
    t4_decode();
    t5_table();
    t6_gold();
    t7_alarm();
    t8_known();
    t9_fuzz();
    t10_gold_ctl();
    t11_enc();
    printf("zh_tuya: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
