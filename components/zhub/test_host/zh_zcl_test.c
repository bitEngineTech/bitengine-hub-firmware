/* zh_zcl 的真值表（docs/15 设计 8.7；M4 第七步 b）。编的是产品源文件 ../zh_zcl.c。
 *   C1 帧头（有 / 无厂商码、太短）   C2 读属性请求逐字节   C3 读属性应答：成功 / 不支持 / 装不下、字符串 / 枚举、无效字符串 0xFF
 *   C4 每个类型的长度对着 SDK 的 types.xml 核（argv[1]；没给就跳过并算失败；--no-sdk = 明说没有 SDK，跳过不算失败）   C5 坏帧：半条记录、值越界、不认识的类型都停、不越界
 *   C6 模糊：随机字节 20 万帧，解析 + 逐条取，ASan / UBSan 下不许越界   C7 属性上报、有符号取值   C8 上报路径的模糊 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zh_zcl.h"

static int s_fail, s_pass;
#define CHECK(cond, ...) do { if (cond) { s_pass++; } else { s_fail++; printf("❌ %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t s_rnd = 0x1234567u;
static uint32_t rnd(void) { s_rnd ^= s_rnd << 13; s_rnd ^= s_rnd >> 17; s_rnd ^= s_rnd << 5; return s_rnd; }

int main(int argc, char **argv)
{
    zh_zcl_frame_t f;
    /* ---- C1 ---- */
    {
        const uint8_t b[] = { 0x18, 0x42, 0x01, 0xAA };
        CHECK(zh_zcl_parse(b, sizeof(b), &f) == ZH_ZCL_OK && f.fc == 0x18 && f.seq == 0x42 && f.cmd == 0x01 && f.n == 1 && f.pl == b + 3 && f.mfg == 0,
              "C1 不带厂商码：头 3 B");
        const uint8_t m[] = { 0x1C, 0x5F, 0x11, 0x07, 0x0A, 0xBB };
        CHECK(zh_zcl_parse(m, sizeof(m), &f) == ZH_ZCL_OK && f.mfg == 0x115F && f.seq == 0x07 && f.cmd == 0x0A && f.n == 1 && f.pl == m + 5,
              "C1 带厂商码：头 5 B、厂商码小端");
        CHECK(zh_zcl_parse(b, 2, &f) == ZH_ZCL_E_SHORT && zh_zcl_parse(m, 4, &f) == ZH_ZCL_E_SHORT, "C1 太短");
    }
    /* ---- C2 ---- */
    {
        uint8_t o[16];
        const uint16_t ids[2] = { ZH_BASIC_MANUF, ZH_BASIC_MODEL };
        const uint8_t want[] = { 0x10, 0x33, 0x00, 0x04, 0x00, 0x05, 0x00 };
        CHECK(zh_zcl_enc_read_attrs(o, sizeof(o), 0x33, ids, 2) == sizeof(want) && !memcmp(o, want, sizeof(want)),
              "C2 读属性请求：全局、客户端→服务端、不要默认应答、属性号小端");
        CHECK(zh_zcl_enc_read_attrs(o, 6, 0x33, ids, 2) == 0 && zh_zcl_enc_read_attrs(o, sizeof(o), 1, ids, 0) == 0, "C2 写不下 / 没属性：0");
    }
    /* ---- C3 ---- */
    {
        /* 0x0004 成功 "LUMI"、0x0005 成功空串、0x0007 enum8 3、0x4000 不支持、0x0001 字符串无效 0xFF */
        const uint8_t b[] = { 0x18, 0x09, 0x01,
                              0x04, 0x00, 0x00, 0x42, 0x04, 'L', 'U', 'M', 'I',
                              0x05, 0x00, 0x00, 0x42, 0x00,
                              0x07, 0x00, 0x00, 0x30, 0x03,
                              0x00, 0x40, 0x86,
                              0x01, 0x00, 0x00, 0x42, 0xFF };
        CHECK(zh_zcl_parse(b, sizeof(b), &f) == ZH_ZCL_OK, "C3 帧头");
        zh_zcl_iter_t it;
        zh_zcl_iter_init(&it, &f);
        zh_zcl_attr_t a;
        const uint8_t *s;
        size_t sl;
        uint32_t v;
        CHECK(zh_zcl_next_rsp_rec(&it, &a) == 1 && a.id == 0x0004 && zh_zcl_attr_str(&a, &s, &sl) && sl == 4 && !memcmp(s, "LUMI", 4), "C3 厂商串");
        CHECK(zh_zcl_next_rsp_rec(&it, &a) == 1 && a.id == 0x0005 && zh_zcl_attr_str(&a, &s, &sl) && sl == 0, "C3 空串");
        CHECK(zh_zcl_next_rsp_rec(&it, &a) == 1 && a.id == 0x0007 && zh_zcl_attr_uint(&a, &v) && v == 3 && !zh_zcl_attr_str(&a, &s, &sl), "C3 enum8 取值、不当字符串");
        CHECK(zh_zcl_next_rsp_rec(&it, &a) == 1 && a.id == 0x4000 && a.status == ZH_ZCL_ST_UNSUP_ATTR && !a.val && !zh_zcl_attr_uint(&a, &v), "C3 不支持：只有 3 B");
        CHECK(zh_zcl_next_rsp_rec(&it, &a) == 1 && a.id == 0x0001 && !zh_zcl_attr_str(&a, &s, &sl) && a.len == 1, "C3 长度 0xFF = 无效字符串：占 1 B、取不到内容");
        CHECK(zh_zcl_next_rsp_rec(&it, &a) == 0, "C3 读完");
        const uint8_t z[] = { 0x18, 0x0A, 0x01, 0x01, 0x00, 0x00, 0x31, 0x0D, 0x00 };   /* ZoneType enum16 0x000D */
        zh_zcl_parse(z, sizeof(z), &f);
        zh_zcl_iter_init(&it, &f);
        CHECK(zh_zcl_next_rsp_rec(&it, &a) == 1 && a.type == ZH_ZCL_T_ENUM16 && zh_zcl_attr_uint(&a, &v) && v == 0x000D, "C3 enum16 小端");
        const uint8_t i89[] = { 0x18, 0x0B, 0x01, 0x04, 0x00, 0x00, 0x42, 0x01, 'X', 0x05, 0x00, 0x89 };
        zh_zcl_parse(i89, sizeof(i89), &f);
        zh_zcl_iter_init(&it, &f);
        CHECK(zh_zcl_next_rsp_rec(&it, &a) == 1 && zh_zcl_next_rsp_rec(&it, &a) == 1 && a.status == ZH_ZCL_ST_INSUF_SPACE && zh_zcl_next_rsp_rec(&it, &a) == 0,
              "C3 装不下（0x89）：记录只有 3 B");
    }
    /* ---- C4 ---- 每个类型的长度对着 SDK types.xml */
    {
        int checked = 0, wrong = 0;
        const bool no_sdk = argc > 1 && strcmp(argv[1], "--no-sdk") == 0;
        FILE *fp = argc > 1 && !no_sdk ? fopen(argv[1], "r") : NULL;
        char line[512];
        while (fp && fgets(line, sizeof(line), fp)) {
            unsigned id;
            const char *p = strstr(line, "<type id=\"0x");
            if (!p || sscanf(p, "<type id=\"0x%x\"", &id) != 1) continue;
            const char *sz = strstr(line, " size=\"");
            const bool str = strstr(line, "string=\"true\"") != NULL, lng = strstr(line, "long=\"true\"") != NULL;
            uint8_t v[20] = { 3, 0, 1, 2, 3 };
            size_t len = 0;
            const int r = zh_zcl_type_len((uint8_t)id, v, sizeof(v), &len);
            checked++;
            if (sz) {
                if (r != ZH_ZCL_OK || len != (size_t)atoi(sz + 7)) { wrong++; printf("   类型 0x%02X：types.xml 说 %d B，我们 %d / %zu\n", id, atoi(sz + 7), r, len); }
            } else if (str) {
                const size_t want = lng ? 2 + 3 : 1 + 3;          /* 长度前缀 + 内容 3 B（v[0] = 3，long 时 v[0..1] = 3） */
                if (r != ZH_ZCL_OK || len != want) { wrong++; printf("   字符串类型 0x%02X：我们 %d / %zu\n", id, r, len); }
            } else if (r != ZH_ZCL_E_TYPE) {                      /* 复合类型：当不支持 */
                wrong++;
                printf("   复合类型 0x%02X 应当不支持\n", id);
            }
        }
        if (fp) fclose(fp);
        if (no_sdk) printf("   C4 跳过：没有 SDK 的 types.xml\n");
        else CHECK(fp && checked == 57 && wrong == 0, "C4 types.xml 里 %d 个类型的长度逐个对上（错 %d）", checked, wrong);
        size_t len = 0;
        CHECK(zh_zcl_type_len(0x03, (const uint8_t[1]){ 0 }, 1, &len) == ZH_ZCL_E_TYPE && zh_zcl_type_len(0x60, (const uint8_t[1]){ 0 }, 1, &len) == ZH_ZCL_E_TYPE,
              "C4 types.xml 里没有的类型：不认识");
        CHECK(zh_zcl_type_len(0x44, (const uint8_t[2]){ 0xFF, 0xFF }, 2, &len) == ZH_ZCL_OK && len == 2, "C4 long 字符串长度 0xFFFF = 无效：占 2 B");
    }
    /* ---- C5 ---- 坏帧 */
    {
        zh_zcl_iter_t it;
        zh_zcl_attr_t a;
        const uint8_t half[] = { 0x18, 0x01, 0x01, 0x04, 0x00 };                          /* 半条记录 */
        zh_zcl_parse(half, sizeof(half), &f);
        zh_zcl_iter_init(&it, &f);
        CHECK(zh_zcl_next_rsp_rec(&it, &a) == ZH_ZCL_E_SHORT && zh_zcl_next_rsp_rec(&it, &a) == 0, "C5 半条记录：停、之后读完");
        const uint8_t over[] = { 0x18, 0x01, 0x01, 0x04, 0x00, 0x00, 0x42, 0x20, 'A', 'B' };   /* 说 32 B、只有 2 B */
        zh_zcl_parse(over, sizeof(over), &f);
        zh_zcl_iter_init(&it, &f);
        CHECK(zh_zcl_next_rsp_rec(&it, &a) == ZH_ZCL_E_SHORT, "C5 字符串长度越界：停");
        const uint8_t unk[] = { 0x18, 0x01, 0x01, 0x04, 0x00, 0x00, 0x48, 0x20, 0x01, 0x00, 0x05, 0x00, 0x86 };   /* array 之后还有一条 */
        zh_zcl_parse(unk, sizeof(unk), &f);
        zh_zcl_iter_init(&it, &f);
        CHECK(zh_zcl_next_rsp_rec(&it, &a) == ZH_ZCL_E_TYPE && zh_zcl_next_rsp_rec(&it, &a) == 0, "C5 复合类型：停，后面那条丢掉");
        const uint8_t nostatus[] = { 0x18, 0x01, 0x01, 0x04, 0x00, 0x00 };               /* 成功却没类型字节 */
        zh_zcl_parse(nostatus, sizeof(nostatus), &f);
        zh_zcl_iter_init(&it, &f);
        CHECK(zh_zcl_next_rsp_rec(&it, &a) == ZH_ZCL_E_SHORT, "C5 状态 0 却没有类型字节：停");
    }
    /* ---- C6 ---- 模糊 */
    {
        long recs = 0, stops = 0;
        for (int k = 0; k < 200000; k++) {
            const size_t n = rnd() % 48;
            uint8_t *b = malloc(n ? n : 1);                       /* 正好 n 字节：越界一个字节 ASan 就报 */
            for (size_t i = 0; i < n; i++) b[i] = (uint8_t)rnd();
            if (k & 1 && n > 3) b[3] = 0x00 + (uint8_t)(rnd() % 2) * 0x42;   /* 让一部分记录走到成功分支 */
            if (zh_zcl_parse(b, n, &f) == ZH_ZCL_OK) {
                zh_zcl_iter_t it;
                zh_zcl_attr_t a;
                zh_zcl_iter_init(&it, &f);
                int r;
                int guard = 0;
                while ((r = zh_zcl_next_rsp_rec(&it, &a)) == 1 && guard++ < 64) {
                    recs++;
                    const uint8_t *s;
                    size_t sl;
                    uint32_t v;
                    if (zh_zcl_attr_str(&a, &s, &sl) && sl) { volatile uint8_t last = s[sl - 1]; (void)last; }
                    (void)zh_zcl_attr_uint(&a, &v);
                }
                if (r < 0) stops++;
            }
            free(b);
        }
        CHECK(recs > 10000 && stops > 10000, "C6 模糊 20 万帧：取到 %ld 条记录、%ld 次停，没越界（ASan）", recs, stops);
    }
    /* ---- C7 ---- 属性上报（没有状态字节）、有符号取值 */
    {
        /* 温度 int16 -512（-5.12 °C）、湿度 uint16 4567、占用 bitmap8 1 */
        const uint8_t b[] = { 0x18, 0x21, 0x0A, 0x00, 0x00, 0x29, 0x00, 0xFE, 0x01, 0x00, 0x21, 0xD7, 0x11, 0x00, 0x00, 0x18, 0x01 };
        CHECK(zh_zcl_parse(b, sizeof(b), &f) == ZH_ZCL_OK && f.cmd == ZH_ZCL_CMD_REPORT_ATTR, "C7 上报帧头");
        zh_zcl_iter_t it;
        zh_zcl_attr_t a;
        int32_t sv;
        uint32_t v;
        zh_zcl_iter_init(&it, &f);
        CHECK(zh_zcl_next_report_rec(&it, &a) == 1 && a.id == 0x0000 && a.type == 0x29 && zh_zcl_attr_int(&a, &sv) && sv == -512 && !zh_zcl_attr_uint(&a, &v),
              "C7 int16 符号扩展（-512），不当无符号");
        CHECK(zh_zcl_next_report_rec(&it, &a) == 1 && a.id == 0x0001 && zh_zcl_attr_uint(&a, &v) && v == 4567 && !zh_zcl_attr_int(&a, &sv), "C7 uint16、不当有符号");
        CHECK(zh_zcl_next_report_rec(&it, &a) == 1 && a.type == 0x18 && zh_zcl_attr_uint(&a, &v) && v == 1, "C7 bitmap8");
        CHECK(zh_zcl_next_report_rec(&it, &a) == 0, "C7 读完");
        const uint8_t i8[] = { 0x80 }, i24[] = { 0xFF, 0xFF, 0x7F }, i32[] = { 0x00, 0x00, 0x00, 0x80 };
        zh_zcl_attr_t x = { .status = 0, .type = 0x28, .val = i8, .len = 1 };
        CHECK(zh_zcl_attr_int(&x, &sv) && sv == -128, "C7 int8 -128");
        x.type = 0x2A; x.val = i24; x.len = 3;
        CHECK(zh_zcl_attr_int(&x, &sv) && sv == 0x7FFFFF, "C7 int24 正数不扩展");
        x.type = 0x2B; x.val = i32; x.len = 4;
        CHECK(zh_zcl_attr_int(&x, &sv) && sv == INT32_MIN, "C7 int32 最小值");
        const uint8_t bad[] = { 0x18, 0x21, 0x0A, 0x00, 0x00, 0x42, 0x09, 'a' };   /* 字符串说 9 B、只有 1 B */
        zh_zcl_parse(bad, sizeof(bad), &f);
        zh_zcl_iter_init(&it, &f);
        CHECK(zh_zcl_next_report_rec(&it, &a) == ZH_ZCL_E_SHORT && zh_zcl_next_report_rec(&it, &a) == 0, "C7 上报的值越界：停");
        const uint8_t two[] = { 0x18, 0x21, 0x0A, 0x00, 0x00 };
        zh_zcl_parse(two, sizeof(two), &f);
        zh_zcl_iter_init(&it, &f);
        CHECK(zh_zcl_next_report_rec(&it, &a) == ZH_ZCL_E_SHORT, "C7 只有属性号没类型：停");
    }
    /* ---- C8 ---- 上报路径的模糊 */
    {
        long recs = 0, stops = 0;
        for (int k = 0; k < 200000; k++) {
            const size_t n = rnd() % 48;
            uint8_t *b = malloc(n ? n : 1);
            for (size_t i = 0; i < n; i++) b[i] = (uint8_t)rnd();
            if (zh_zcl_parse(b, n, &f) == ZH_ZCL_OK) {
                zh_zcl_iter_t it;
                zh_zcl_attr_t a;
                zh_zcl_iter_init(&it, &f);
                int r, guard = 0;
                while ((r = zh_zcl_next_report_rec(&it, &a)) == 1 && guard++ < 64) {
                    recs++;
                    int32_t sv;
                    uint32_t v;
                    const uint8_t *str;
                    size_t sl;
                    (void)zh_zcl_attr_int(&a, &sv);
                    (void)zh_zcl_attr_uint(&a, &v);
                    if (zh_zcl_attr_str(&a, &str, &sl) && sl) { volatile uint8_t last = str[sl - 1]; (void)last; }
                }
                if (r < 0) stops++;
            }
            free(b);
        }
        CHECK(recs > 10000 && stops > 10000, "C8 上报路径模糊 20 万帧：取到 %ld 条、%ld 次停，没越界（ASan）", recs, stops);
    }
    /* C9 写一个 EUI64 属性（CONFIGURE 第一步写 CIE 地址；SDK ias-zone-client.c:395-404） */
    {
        const uint8_t eui[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
        uint8_t wb[16];
        static const uint8_t want[14] = { 0x00, 0x33, ZH_ZCL_CMD_WRITE_ATTR, 0x10, 0x00, ZH_ZCL_T_EUI64, 1, 2, 3, 4, 5, 6, 7, 8 };
        CHECK(zh_zcl_enc_write_eui64(wb, sizeof(wb), 0x33, ZH_IAS_CIE_ADDR, eui) == 14 && !memcmp(wb, want, 14), "C9 写 CIE 地址：帧控制 0x00（同 SDK）、14 B、小端");
        CHECK(zh_zcl_enc_write_eui64(wb, 13, 0x33, ZH_IAS_CIE_ADDR, eui) == 0 && zh_zcl_enc_write_eui64(NULL, 16, 0, 0, eui) == 0 &&
              zh_zcl_enc_write_eui64(wb, 16, 0, 0, NULL) == 0, "C9 写不下 / NULL：0");
        zh_zcl_frame_t f;
        CHECK(zh_zcl_parse(wb, 14, &f) == ZH_ZCL_OK && f.cmd == ZH_ZCL_CMD_WRITE_ATTR && f.n == 11, "C9 自己编的帧自己解得开");
    }
    /* C10 上报配置（CONFIGURE 第二步；reporting.c:422-473 的记录格式） */
    {
        uint8_t cb[24];
        static const uint8_t want[13] = { 0x00, 0x44, ZH_ZCL_CMD_CFG_REPORT, 0x00, 0x00, 0x00, ZH_ZCL_T_INT16, 0x1E, 0x00, 0x58, 0x02, 0x0A, 0x00 };
        CHECK(zh_zcl_enc_cfg_report(cb, sizeof(cb), 0x44, 0x0000, ZH_ZCL_T_INT16, 30, 600, 10, 2) == 13 && !memcmp(cb, want, 13), "C10 温度：13 B、变化量 2 B 小端");
        CHECK(zh_zcl_enc_cfg_report(cb, sizeof(cb), 0x44, 0x0000, ZH_ZCL_T_BOOL, 0, 300, 0, 0) == 11 && cb[6] == ZH_ZCL_T_BOOL, "C10 离散类型：不带变化量（11 B）");
        CHECK(zh_zcl_enc_cfg_report(cb, sizeof(cb), 0, 0, ZH_ZCL_T_UINT8, 0, 0, 0x01020304u, 4) == 15 && cb[11] == 0x04 && cb[14] == 0x01, "C10 变化量 4 B：小端");
        CHECK(zh_zcl_enc_cfg_report(cb, sizeof(cb), 0, 0, 0, 0, 0, 0, 5) == 0 && zh_zcl_enc_cfg_report(cb, 12, 0, 0, 0, 0, 0, 0, 2) == 0 &&
              zh_zcl_enc_cfg_report(NULL, 24, 0, 0, 0, 0, 0, 0, 0) == 0, "C10 变化量 > 4 B / 写不下 / NULL：0");
    }
    printf("zh_zcl: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
