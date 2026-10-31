/* zh_lumi_test —— zh_lumi.c（小米 / Aqara）与生成的 zh_lumi_db.c 的主机测试（M5 第十步）。
 *
 * L1 TLV 迭代（[标签][类型][值]、末尾多一个字节、坏类型、超长）；L2 取整数（各宽度、符号扩展、装不下的 u32）；L3 表本身（有序、偏移、查找）；
 * L4 金样例：zh_lumi_gold.inc 是导出脚本拿同一批 TLV / 属性喂 zigbee-herdsman-converters 的输出，逐条对照 zh_lumi_map；电量曲线那几行对照 zh_lumi_batt_pct；
 * L5 电量曲线三种（照 zhc utils.js 手算）；L6 手核的几台（温湿度计 / 门磁 / 人体 / 漏水）；L7 模糊。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zh_lumi.h"

static int s_pass, s_fail;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { const char *model; unsigned cl; int id, raw, field, want; } gold_t;
static const gold_t k_gold[] = {
#include "zh_lumi_gold.inc"
};

static void l1_tlv(void)
{
    /* 温湿度计一条 0xFF01 的典型内容：01 21 d1 0b（电压 3025 mV）、04 21 a8 13、05 21 0a 00、06 24 …（40 位，跳过）、64 29 2c 09（温度 23.48）、65 21 ……（湿度）、0a 21 00 00 */
    const uint8_t b[] = { 0x01, 0x21, 0xD1, 0x0B, 0x64, 0x29, 0x2C, 0x09, 0x06, 0x24, 1, 2, 3, 4, 5, 0x65, 0x21, 0x10, 0x15, 0x03, 0x28, 0xE7, 0x0A, 0x10, 0x01, 0x00 };
    zh_lumi_tlv_t it;
    uint8_t tag;
    int32_t v;
    bool num;
    zh_lumi_tlv_init(&it, b, sizeof(b));
    CHECK(zh_lumi_tlv_next(&it, &tag, &v, &num) == 1 && tag == 1 && num && v == 3025, "L1 电压 3025 mV（u16 小端）");
    CHECK(zh_lumi_tlv_next(&it, &tag, &v, &num) == 1 && tag == 100 && num && v == 2348, "L1 温度 int16 2348");
    CHECK(zh_lumi_tlv_next(&it, &tag, &v, &num) == 1 && tag == 6 && !num, "L1 40 位无符号：拿到但不取整数");
    CHECK(zh_lumi_tlv_next(&it, &tag, &v, &num) == 1 && tag == 101 && num && v == 0x1510, "L1 湿度");
    CHECK(zh_lumi_tlv_next(&it, &tag, &v, &num) == 1 && tag == 3 && num && v == -25, "L1 int8 −25（符号扩展）");
    CHECK(zh_lumi_tlv_next(&it, &tag, &v, &num) == 1 && tag == 10 && num && v == 1, "L1 布尔");
    CHECK(zh_lumi_tlv_next(&it, &tag, &v, &num) == 0 && zh_lumi_tlv_next(&it, &tag, &v, &num) == 0, "L1 末尾多一个字节：当完了（同 readMiStruct）");
    const uint8_t bad[] = { 0x01, 0x05, 0x00, 0x02, 0x20, 0x01 };   /* 0x05 没定义（0xFF 是合法的 unknown 类型，长度 0） */
    zh_lumi_tlv_init(&it, bad, sizeof(bad));
    CHECK(zh_lumi_tlv_next(&it, &tag, &v, &num) == -1 && zh_lumi_tlv_next(&it, &tag, &v, &num) == 0, "L1 不认识的类型：坏了、停");
    const uint8_t sh[] = { 0x01, 0x23, 0x00, 0x01 };
    zh_lumi_tlv_init(&it, sh, sizeof(sh));
    CHECK(zh_lumi_tlv_next(&it, &tag, &v, &num) == -1, "L1 值超出剩余：坏了");
    const uint8_t str[] = { 0x08, 0x42, 0x02, 'a', 'b', 0x0b, 0x20, 0x07 };
    zh_lumi_tlv_init(&it, str, sizeof(str));
    CHECK(zh_lumi_tlv_next(&it, &tag, &v, &num) == 1 && tag == 8 && !num && zh_lumi_tlv_next(&it, &tag, &v, &num) == 1 && tag == 11 && num && v == 7, "L1 字符串跳过、后面照常");
    zh_lumi_tlv_init(&it, NULL, 5);
    CHECK(zh_lumi_tlv_next(&it, &tag, &v, &num) == 0 && zh_lumi_tlv_next(NULL, &tag, &v, &num) == -1, "L1 空指针");
}

static void l2_int(void)
{
    int32_t v;
    CHECK(zh_lumi_val_int(0x23, (const uint8_t *)"\x01\x02\x03\x04", 4, &v) && v == 0x04030201, "L2 u32 小端");
    CHECK(!zh_lumi_val_int(0x23, (const uint8_t *)"\xFF\xFF\xFF\xFF", 4, &v), "L2 u32 装不进 int32：不取");
    CHECK(zh_lumi_val_int(0x2B, (const uint8_t *)"\xFF\xFF\xFF\xFF", 4, &v) && v == -1, "L2 int32 −1");
    CHECK(zh_lumi_val_int(0x2A, (const uint8_t *)"\xFE\xFF\xFF", 3, &v) && v == -2, "L2 int24 符号扩展");
    CHECK(zh_lumi_val_int(0x22, (const uint8_t *)"\xFE\xFF\xFF", 3, &v) && v == 0xFFFFFE, "L2 u24 不扩展");
    CHECK(zh_lumi_val_int(0x31, (const uint8_t *)"\x34\x12", 2, &v) && v == 0x1234 && zh_lumi_val_int(0x18, (const uint8_t *)"\x81", 1, &v) && v == 0x81, "L2 enum16 / bitmap8");
    CHECK(!zh_lumi_val_int(0x39, (const uint8_t *)"\0\0\0\0", 4, &v) && !zh_lumi_val_int(0x21, (const uint8_t *)"\x01", 1, &v) && !zh_lumi_val_int(0x21, NULL, 2, &v), "L2 浮点 / 不够长 / 空指针：不取");
}

static void l3_table(void)
{
    CHECK(zh_ldb_n_fps > 100 && zh_ldb_n_devs > 20 && strncmp(zh_ldb_source, "zigbee-herdsman-converters ", 27) == 0, "L3 规模（型号 %u / 组合 %u / 行 %u）", zh_ldb_n_fps, zh_ldb_n_devs, zh_ldb_n_rows);
    unsigned bad = 0, unsorted = 0, miss = 0;
    for (unsigned i = 0; i < zh_ldb_n_fps; i++) {
        if (zh_ldb_fps[i].model >= zh_ldb_str_len || zh_ldb_fps[i].dev >= zh_ldb_n_devs) { bad++; continue; }
        if (i && strcmp(zh_ldb_str + zh_ldb_fps[i - 1].model, zh_ldb_str + zh_ldb_fps[i].model) >= 0) unsorted++;
        if (zh_lumi_find(zh_ldb_str + zh_ldb_fps[i].model) != zh_ldb_fps[i].dev) miss++;
    }
    for (unsigned i = 0; i < zh_ldb_n_devs; i++) {
        const zh_ldb_dev_t *d = &zh_ldb_devs[i];
        if ((unsigned)d->row + d->n_rows > zh_ldb_n_rows || d->batt > ZH_LDB_BATT_3V_1500_2800) bad++;
        for (unsigned k = 0; k < d->n_rows && (unsigned)d->row + k < zh_ldb_n_rows; k++) {
            const zh_ldb_row_t *r = &zh_ldb_rows[d->row + k];
            if (r->field >= ZH_TF_COUNT || (r->field && (r->kind < ZH_TDB_LIN || r->kind > ZH_TDB_ANY))) bad++;
            if (r->field && !(d->fields & (1u << r->field))) bad++;
        }
    }
    CHECK(!bad && !unsorted && !miss, "L3 偏移 / 字段合法、型号严格有序、每个型号查得到自己（坏 %u / 乱序 %u / 查不到 %u）", bad, unsorted, miss);
    CHECK(zh_lumi_find("lumi.no_such") == -1 && zh_lumi_find("") == -1 && zh_lumi_find(NULL) == -1, "L3 不在表里 / 空");
    zh_tbl_val_t o[4];
    bool dr;
    CHECK(zh_lumi_fields(-1) == 0 && zh_lumi_occ_s(99999) == 0 && !zh_lumi_has_cl(-1, 6) && zh_lumi_map(-1, 6, 0, 1, o, 4, &dr) == 0 && zh_lumi_batt_pct(-1, 3000) == -1, "L3 下标非法");
}

static void l4_gold(void)
{
    unsigned n = 0, bad = 0, nf = 0;
    for (size_t i = 0; i < sizeof(k_gold) / sizeof(k_gold[0]); i++) {
        const gold_t *g = &k_gold[i];
        const int dev = zh_lumi_find(g->model);
        if (dev < 0) { nf++; continue; }
        if (g->field < 0) {                        /* 电量曲线 */
            const int p = zh_lumi_batt_pct(dev, g->raw);
            if (p != g->want) { bad++; if (bad < 8) printf("  L4 %s 电量 %d mV：要 %d %%，得 %d\n", g->model, g->raw, g->want, p); }
            n++;
            continue;
        }
        zh_tbl_val_t o[4];
        bool dr;
        const unsigned m = zh_lumi_map(dev, (uint16_t)g->cl, (uint16_t)g->id, g->raw, o, 4, &dr);
        const zh_tbl_val_t *hit = NULL;
        for (unsigned k = 0; k < m; k++) if (o[k].field == g->field) hit = &o[k];
        const long tol = (g->field == ZH_TF_ONOFF || g->field == ZH_TF_OCCUPANCY || (g->field >= ZH_TF_CONTACT && g->field <= ZH_TF_VIBRATION) || g->field == ZH_TF_BATTERY_LOW) ? 0 : 1;
        if (!hit || labs((long)hit->val - g->want) > tol) {
            bad++;
            if (bad < 8) printf("  L4 %s 簇 0x%04X id %d 原始 %d 字段 %d：要 %d，得 %s%ld\n", g->model, g->cl, g->id, g->raw, g->field, g->want, hit ? "" : "（没有）", hit ? (long)hit->val : 0L);
        }
        n++;
    }
    CHECK(nf == 0, "L4 金样例的型号都查得到（%u 条查不到）", nf);
    CHECK(bad == 0 && n >= 100, "L4 金样例 %u 条：固件的换算 / 电量曲线与库的输出一致（不一致 %u 条）", n, bad);
}

static void l5_curves(void)
{
    /* 线性 2850–3000（小米大多数）：照 toPercentage 手算 */
    CHECK(zh_lumi_batt_curve(ZH_LDB_BATT_LINEAR, 2850, 3000, 0, 2900) == 33 && zh_lumi_batt_curve(ZH_LDB_BATT_LINEAR, 2850, 3000, 0, 2925) == 50 &&
          zh_lumi_batt_curve(ZH_LDB_BATT_LINEAR, 2850, 3000, 0, 2700) == 0 && zh_lumi_batt_curve(ZH_LDB_BATT_LINEAR, 2850, 3000, 0, 3200) == 100, "L5 线性：33 / 50 / 钳 0 / 钳 100");
    CHECK(zh_lumi_batt_curve(ZH_LDB_BATT_LINEAR, 2850, 3000, 100, 2800) == 33, "L5 线性带 vOffset（+100 mV）");
    CHECK(zh_lumi_batt_curve(ZH_LDB_BATT_LINEAR, 3000, 3000, 0, 3000) == -1, "L5 min = max：不算");
    /* 3V_2100：2800 → 42 − (100·24)/160 = 27；2500 → 18 − (240·12)/300 = 8.4 → 8；2950 → 100 − 29 = 71；2000 → 0；3100 → 100 */
    CHECK(zh_lumi_batt_curve(ZH_LDB_BATT_3V_2100, 0, 0, 0, 2800) == 27 && zh_lumi_batt_curve(ZH_LDB_BATT_3V_2100, 0, 0, 0, 2500) == 8 &&
          zh_lumi_batt_curve(ZH_LDB_BATT_3V_2100, 0, 0, 0, 2950) == 71 && zh_lumi_batt_curve(ZH_LDB_BATT_3V_2100, 0, 0, 0, 2000) == 0 &&
          zh_lumi_batt_curve(ZH_LDB_BATT_3V_2100, 0, 0, 0, 3100) == 100, "L5 3V_2100 分段");
    /* 3V_1500_2800：2500 → 235 − 370000/2501 = 87.06 → 87；1500 → −11.6 → 钳 0；3000 → 111.7 → 钳 100 */
    CHECK(zh_lumi_batt_curve(ZH_LDB_BATT_3V_1500_2800, 0, 0, 0, 2500) == 87 && zh_lumi_batt_curve(ZH_LDB_BATT_3V_1500_2800, 0, 0, 0, 1500) == 0 &&
          zh_lumi_batt_curve(ZH_LDB_BATT_3V_1500_2800, 0, 0, 0, 3000) == 100, "L5 3V_1500_2800");
    CHECK(zh_lumi_batt_curve(9, 0, 0, 0, 3000) == -1 && zh_lumi_batt_curve(ZH_LDB_BATT_NONE, 0, 0, 0, 3000) == -1, "L5 没有曲线 / 不认识");
    CHECK(zh_lumi_batt_curve(ZH_LDB_BATT_3V_1500_2800, 0, 0, 0, -1) == 0 && zh_lumi_batt_curve(ZH_LDB_BATT_3V_1500_2800, 0, 0, 0, INT32_MAX) == 100 &&
          zh_lumi_batt_curve(ZH_LDB_BATT_LINEAR, 2850, 3000, 0, INT32_MIN) == 0, "L5 坏帧的毫伏（−1 / INT32_MAX / INT32_MIN）：不除零、不溢出");
}

static int32_t map1(const char *model, unsigned cl, int id, int32_t raw, unsigned field, int *found)
{
    zh_tbl_val_t o[4];
    bool dr;
    const unsigned m = zh_lumi_map(zh_lumi_find(model), (uint16_t)cl, (uint16_t)id, raw, o, 4, &dr);
    for (unsigned k = 0; k < m; k++) if (o[k].field == field) { *found = 1; return o[k].val; }
    *found = 0;
    return 0;
}

static void l6_known(void)
{
    int f;
    /* lumi.weather（WSDCGQ11LM）：TLV 1 电压、100 温度 /100、101 湿度 /100、102 气压 /100 hPa；电量 2850–3000 线性 */
    const int w = zh_lumi_find("lumi.weather");
    CHECK(w >= 0 && (zh_lumi_fields(w) & (1u << ZH_TF_TEMP)) && (zh_lumi_fields(w) & (1u << ZH_TF_PRESSURE)) && (zh_lumi_fields(w) & (1u << ZH_TF_BATTERY_PCT)), "L6 温湿度计：温度 / 气压 / 电量");
    CHECK(map1("lumi.weather", ZH_LDB_CL_TLV, 1, 3025, ZH_TF_VOLTAGE, &f) == 3025 && f, "L6 TLV 1 = 电池 3025 mV");
    CHECK(map1("lumi.weather", ZH_LDB_CL_TLV, 100, 2348, ZH_TF_TEMP, &f) == 2348 && f, "L6 TLV 100 = 23.48 °C");
    CHECK(map1("lumi.weather", ZH_LDB_CL_TLV, 102, 100950, ZH_TF_PRESSURE, &f) == 10095 && f, "L6 TLV 102 = 1009.5 hPa（0.1 hPa 单位）");
    CHECK(zh_lumi_batt_pct(w, 2925) == 50, "L6 电量 2925 mV = 50 %%");
    /* 门磁 aq2：OnOff 簇的 onOff 是开合（1 = 开），这台整个 OnOff 簇不按开关算 */
    const int m = zh_lumi_find("lumi.sensor_magnet.aq2");
    CHECK(m >= 0 && zh_lumi_has_cl(m, 0x0006) && !zh_lumi_has_cl(m, 0x0402), "L6 门磁改写了 OnOff 簇");
    CHECK(!zh_lumi_has_cl(w, ZH_LDB_CL_TLV) && !zh_lumi_has_cl(m, ZH_LDB_CL_TLV), "L6 TLV 伪簇不算\"改写了标准簇\"（温湿度计全是 TLV 行）");
    CHECK(map1("lumi.sensor_magnet.aq2", 0x0006, 0, 1, ZH_TF_CONTACT, &f) == 1 && f && map1("lumi.sensor_magnet.aq2", 0x0006, 0, 0, ZH_TF_CONTACT, &f) == 0, "L6 门磁 onOff 1 = 开");
    /* 人体 aq2：90 s 清零、照度直接是 lux */
    const int mo = zh_lumi_find("lumi.sensor_motion.aq2");
    CHECK(mo >= 0 && zh_lumi_occ_s(mo) == 90 && (zh_lumi_fields(mo) & (1u << ZH_TF_OCCUPANCY)), "L6 人体：90 s 清零");
    CHECK(map1("lumi.sensor_motion.aq2", 0x0400, 0, 123, ZH_TF_LUX, &f) == 123 && f, "L6 人体的照度 123 = 123 lux（不按对数）");
    /* 新式 Aqara 人体（P1）：0xFCC0 的 illuminance（0x0112）来一条就是有人（ANY）、照度 = 值 − 65536 */
    CHECK(map1("lumi.motion.ac02", 0xFCC0, 0x0112, 65536 + 80, ZH_TF_OCCUPANCY, &f) == 1 && f && map1("lumi.motion.ac02", 0xFCC0, 0x0112, 65536 + 80, ZH_TF_LUX, &f) == 80 && f,
          "L6 P1 人体：0x0112 一条 = 有人 + 照度 80 lux");
    CHECK(zh_lumi_occ_s(zh_lumi_find("lumi.motion.ac02")) == 90, "L6 P1 人体也是 90 s 清零");
    /* 漏水：IAS 报警是漏水（表只记字段位） */
    CHECK((zh_lumi_fields(zh_lumi_find("lumi.sensor_wleak.aq1")) & (1u << ZH_TF_WATER)) != 0, "L6 漏水传感器的报警字段是 WATER");
    /* 丢掉的一路 */
    unsigned drops = 0;
    for (unsigned i = 0; i < zh_ldb_n_rows; i++) if (zh_ldb_rows[i].field == ZH_TF_NONE) drops++;
    bool dr = false;
    zh_tbl_val_t o[4];
    for (unsigned i = 0; i < zh_ldb_n_devs && !dr; i++)
        for (unsigned k = 0; k < zh_ldb_devs[i].n_rows; k++) {
            const zh_ldb_row_t *r = &zh_ldb_rows[zh_ldb_devs[i].row + k];
            if (r->field == ZH_TF_NONE) { CHECK(zh_lumi_map((int)i, r->cl, r->id, 1, o, 4, &dr) == 0 && dr, "L6 丢掉的那一路：0 条、dropped"); break; }
        }
    CHECK(drops > 0 && dr, "L6 表里有丢掉的行（%u 条）", drops);
}

static uint32_t s_lcg = 777;
static uint32_t rnd(void) { s_lcg = s_lcg * 1103515245u + 12345u; return s_lcg >> 8; }

static void l7_fuzz(void)
{
    for (int round = 0; round < 20000; round++) {
        uint8_t b[32];
        const size_t n = rnd() % sizeof(b);
        for (size_t i = 0; i < n; i++) b[i] = (uint8_t)rnd();
        if (round & 1) for (size_t i = 1; i < n; i += 4) b[i] = (uint8_t)(0x20 + rnd() % 12);   /* 一半让类型像样 */
        uint8_t *p = malloc(n ? n : 1);
        memcpy(p, b, n);
        zh_lumi_tlv_t it;
        uint8_t tag;
        int32_t v;
        bool num;
        zh_lumi_tlv_init(&it, p, n);
        int r, k = 0;
        while ((r = zh_lumi_tlv_next(&it, &tag, &v, &num)) == 1 && k++ < 64) {
            zh_tbl_val_t o[4];
            bool dr;
            (void)zh_lumi_map((int)(rnd() % (zh_ldb_n_devs + 2)) - 1, ZH_LDB_CL_TLV, tag, v, o, 4, &dr);
        }
        CHECK(r != 1, "L7 迭代会结束");
        free(p);
    }
}

int main(void)
{
    l1_tlv();
    l2_int();
    l3_table();
    l4_gold();
    l5_curves();
    l6_known();
    l7_fuzz();
    printf("zh_lumi: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
