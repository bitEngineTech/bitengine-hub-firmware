/* srp_readvertise 的名字/TXT 解析单元测试 —— 不需要板子，只要 gcc。
 *
 * 为什么单独测这三个函数：它们是本模块唯一直接吃**空口数据**的地方。
 * TXT 内容与服务名都来自子设备的 SRP 注册，按 CLAUDE.md 的 S1 规则必须当作
 * 恶意构造来对待。越界判断写错的现场表现是随机崩溃，极难归因，而在这里
 * 一秒钟就能穷举完边界。
 *
 * 直接 #include 被测 .c，这样能看到 static 函数与静态状态。
 */

#include "stub/test_common.h"
SRP_TEST_GLOBALS

#include "../srp_readvertise.c"

/* ── first_label ── */

static void test_first_label(void)
{
    char out[LABEL_LEN];

    CASE("正常：取第一段");
    CHECK(first_label("920E02140E0077C1.default.service.arpa.", out, sizeof(out)),
          "应成功");
    CHECK(strcmp(out, "920E02140E0077C1") == 0, "得到 '%s'", out);

    CASE("无点：整串就是标签");
    CHECK(first_label("bitengine", out, sizeof(out)), "应成功");
    CHECK(strcmp(out, "bitengine") == 0, "得到 '%s'", out);

    CASE("空串与开头即点：拒绝");
    CHECK(!first_label("", out, sizeof(out)), "空串应失败");
    CHECK(!first_label(".foo", out, sizeof(out)), "开头是点应失败");
    CHECK(!first_label(NULL, out, sizeof(out)), "NULL 应失败");
    CHECK(!first_label("abc", NULL, sizeof(out)), "out 为 NULL 应失败");
    CHECK(!first_label("abc", out, 0), "长度 0 应失败");

    CASE("刚好放得下 / 差一字节");
    char small[5];
    CHECK(first_label("abcd.x", small, sizeof(small)), "4 字符 + NUL 应成功");
    CHECK(strcmp(small, "abcd") == 0, "得到 '%s'", small);
    CHECK(!first_label("abcde.x", small, sizeof(small)), "5 字符放不下应失败，不能截断");

    CASE("DNS 标签上限 63");
    char label63[64], input[80];
    memset(label63, 'a', 63); label63[63] = '\0';
    snprintf(input, sizeof(input), "%s.local", label63);
    CHECK(first_label(input, out, sizeof(out)), "63 字符标签应成功");
    CHECK(strlen(out) == 63, "长度 %zu", strlen(out));
}

/* ── split_service_name ── */

static void test_split_service_name(void)
{
    char type[TYPE_LEN], proto[PROTO_LEN];

    CASE("正常：_matter._tcp");
    CHECK(split_service_name("_matter._tcp.default.service.arpa.",
                             type, sizeof(type), proto, sizeof(proto)), "应成功");
    CHECK(strcmp(type, "_matter") == 0, "type='%s'", type);
    CHECK(strcmp(proto, "_tcp") == 0, "proto='%s'", proto);

    CASE("_udp 也接受（_meshcop 等）");
    CHECK(split_service_name("_meshcop._udp.default.service.arpa.",
                             type, sizeof(type), proto, sizeof(proto)), "应成功");
    CHECK(strcmp(proto, "_udp") == 0, "proto='%s'", proto);

    CASE("proto 不是 _tcp/_udp：拒绝");
    CHECK(!split_service_name("_matter._sctp.default.service.arpa.",
                              type, sizeof(type), proto, sizeof(proto)),
          "未知 proto 必须拒绝，否则会拿错误参数去调 mdns");

    CASE("结构不完整：拒绝");
    CHECK(!split_service_name("_matter", type, sizeof(type), proto, sizeof(proto)),
          "只有一段应失败");
    CHECK(!split_service_name("", type, sizeof(type), proto, sizeof(proto)),
          "空串应失败");
    CHECK(!split_service_name("_matter.", type, sizeof(type), proto, sizeof(proto)),
          "点后为空应失败");

    CASE("超长 type：拒绝而不是截断");
    char longname[128];
    memset(longname, 'x', 100); longname[100] = '\0';
    char full[160];
    snprintf(full, sizeof(full), "_%s._tcp.local.", longname);
    CHECK(!split_service_name(full, type, sizeof(type), proto, sizeof(proto)),
          "超过 TYPE_LEN 必须失败");
}

/* ── parse_txt ── */

/* 按 DNS-SD 线格式拼装：每条前面一个长度字节 */
static uint16_t build_txt(uint8_t *buf, const char **entries, int n)
{
    uint16_t off = 0;
    for (int i = 0; i < n; i++) {
        uint8_t len = (uint8_t)strlen(entries[i]);
        buf[off++] = len;
        memcpy(buf + off, entries[i], len);
        off += len;
    }
    return off;
}

static void test_parse_txt(void)
{
    uint8_t buf[512];
    mdns_txt_item_t items[MAX_TXT_ITEMS];
    char scratch[SCRATCH_LEN];

    CASE("真实样本：Aqara P2 的 SII/SAI/SAT");
    const char *real[] = { "SII=7000", "SAI=2250", "SAT=5000" };
    uint16_t len = build_txt(buf, real, 3);
    int n = parse_txt(buf, len, items, MAX_TXT_ITEMS, scratch, sizeof(scratch));
    CHECK(n == 3, "应解出 3 条，实际 %d", n);
    CHECK(strcmp(items[0].key, "SII") == 0 && strcmp(items[0].value, "7000") == 0,
          "第 1 条 %s=%s", items[0].key, items[0].value);
    CHECK(strcmp(items[2].key, "SAT") == 0 && strcmp(items[2].value, "5000") == 0,
          "第 3 条 %s=%s", items[2].key, items[2].value);

    CASE("无 '=' 的键：value 为空串，不越界");
    const char *noeq[] = { "T", "SII=7000" };
    len = build_txt(buf, noeq, 2);
    n = parse_txt(buf, len, items, MAX_TXT_ITEMS, scratch, sizeof(scratch));
    CHECK(n == 2, "应解出 2 条，实际 %d", n);
    CHECK(strcmp(items[0].key, "T") == 0, "key='%s'", items[0].key);
    CHECK(items[0].value && items[0].value[0] == '\0', "value 应为空串");

    CASE("value 里含 '='：只在第一个处切分");
    const char *eq2[] = { "K=a=b" };
    len = build_txt(buf, eq2, 1);
    n = parse_txt(buf, len, items, MAX_TXT_ITEMS, scratch, sizeof(scratch));
    CHECK(n == 1 && strcmp(items[0].key, "K") == 0 && strcmp(items[0].value, "a=b") == 0,
          "得到 %s=%s", items[0].key, items[0].value);

    CASE("长度为 0 的条目被跳过（DNS-SD 允许填充）");
    buf[0] = 0; buf[1] = 8; memcpy(buf + 2, "SII=7000", 8);
    n = parse_txt(buf, 10, items, MAX_TXT_ITEMS, scratch, sizeof(scratch));
    CHECK(n == 1, "应只解出 1 条，实际 %d", n);
    CHECK(strcmp(items[0].key, "SII") == 0, "key='%s'", items[0].key);

    CASE("空输入");
    CHECK(parse_txt(buf, 0, items, MAX_TXT_ITEMS, scratch, sizeof(scratch)) == 0,
          "长度 0 应解出 0 条");

    CASE("长度字节越界：丢弃剩余，不读越界内存");
    buf[0] = 8; memcpy(buf + 1, "SII=700", 7);   /* 声称 8 字节，实际只有 7 */
    n = parse_txt(buf, 8, items, MAX_TXT_ITEMS, scratch, sizeof(scratch));
    CHECK(n == 0, "越界条目必须整条丢弃，实际解出 %d", n);

    CASE("最后一条越界：前面的仍然有效");
    len = build_txt(buf, real, 1);      /* SII=7000 */
    buf[len] = 200;                      /* 再接一条声称 200 字节的 */
    n = parse_txt(buf, (uint16_t)(len + 1), items, MAX_TXT_ITEMS, scratch, sizeof(scratch));
    CHECK(n == 1, "应保留前 1 条，实际 %d", n);

    CASE("条目数超过 MAX_TXT_ITEMS：截到上限，不溢出 items[]");
    uint16_t off = 0;
    for (int i = 0; i < MAX_TXT_ITEMS + 8; i++) {
        buf[off++] = 4;
        buf[off++] = (uint8_t)('A' + (i % 26));
        memcpy(buf + off, "=1", 2); off += 2;
        buf[off++] = 'x';
    }
    n = parse_txt(buf, off, items, MAX_TXT_ITEMS, scratch, sizeof(scratch));
    CHECK(n == MAX_TXT_ITEMS, "应截到 %d，实际 %d", MAX_TXT_ITEMS, n);

    CASE("scratch 放不下时停下，不越界写");
    char tiny[16];
    len = build_txt(buf, real, 3);
    n = parse_txt(buf, len, items, MAX_TXT_ITEMS, tiny, sizeof(tiny));
    CHECK(n >= 0 && n <= 2, "小 scratch 只能放下 1 条左右，实际 %d", n);
    for (int i = 0; i < n; i++) {
        CHECK(items[i].key >= tiny && items[i].key < tiny + sizeof(tiny),
              "第 %d 条的 key 必须落在 scratch 内", i);
    }

    CASE("整条 TXT 达到 MAX_TXT_BYTES 上限也不崩");
    off = 0;
    while (off + 32 < MAX_TXT_BYTES) {
        buf[off++] = 30;
        memset(buf + off, 'k', 30);
        buf[off + 2] = '=';
        off += 30;
    }
    n = parse_txt(buf, off, items, MAX_TXT_ITEMS, scratch, sizeof(scratch));
    CHECK(n <= MAX_TXT_ITEMS, "条目数不得超上限，实际 %d", n);
}

int main(void)
{
    printf("=== first_label ===\n");        test_first_label();
    printf("=== split_service_name ===\n"); test_split_service_name();
    printf("=== parse_txt ===\n");           test_parse_txt();
    return srp_test_summary("parse");
}
