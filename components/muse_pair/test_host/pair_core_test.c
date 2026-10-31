/* pair_core 的真值表（docs/15 设计第五节）。编的是产品源文件 ../pair_core.c（及 muse_link 的 muse_cfg_core.c、utf8）。 */
#include <stdio.h>
#include <string.h>
#include "pair_core.h"

static int s_fail, s_pass;
#define CHECK(cond, ...) do { if (cond) { s_pass++; } else { s_fail++; printf("❌ %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_res(void)
{
    uint8_t b[PAIR_RES_BLOB_LEN];
    pair_res_t r = PAIR_RES_NONE; uint32_t t = 0;
    pair_res_encode(PAIR_RES_AUTH_FAILED, 0x6543A1B2u, b);
    CHECK(pair_res_decode(b, sizeof(b), &r, &t) && r == PAIR_RES_AUTH_FAILED && t == 0x6543A1B2u, "往返");
    CHECK(b[0] == 6 && b[1] == 0xB2 && b[4] == 0x65, "编号进 flash：auth_failed = 6、小端");
    CHECK(!pair_res_decode(b, 4, &r, &t), "短了");
    CHECK(!pair_res_decode(NULL, 5, &r, &t), "NULL");
    b[0] = PAIR_RES_NONE;
    CHECK(!pair_res_decode(b, 5, &r, &t), "NONE 不是结果");
    b[0] = PAIR_RES_MAX;
    CHECK(!pair_res_decode(b, 5, &r, &t), "不认识的新编号当作没有");
    CHECK(PAIR_RES_OK == 1 && PAIR_RES_WINDOW_TIMEOUT == 2 && PAIR_RES_CANCELLED == 3 && PAIR_RES_INTERNAL == 12
          && PAIR_RES_INTERRUPTED == 13, "编号钉死（进 flash，只许追加）");
    for (int i = 0; i < PAIR_RES_MAX; i++) {
        CHECK(strcmp(pair_res_str((pair_res_t)i), "unknown") != 0, "编号 %d 有名字", i);
    }
    CHECK(strcmp(pair_res_str(PAIR_RES_STORAGE_FAILED), "error_storage") == 0, "落盘失败用上游的串");
    CHECK(strcmp(pair_res_str((pair_res_t)99), "unknown") == 0, "越界");
}

static void test_provision(void)
{
    /* 密码空串（开放网络）在 pair_ble.c 里判成"是字符串"（cJSON_IsString）传进来，这里只看"是不是字符串"；
       "不要求非空"由 muse_pair/VERIFY.sh 钉在 pair_ble.c 上 */
    CHECK(pair_provision_ok(true, true, "acc", "ref", "device"), "正常（密码是不是空串这里不管）");
    CHECK(!pair_provision_ok(false, true, "acc", "ref", "device"), "ssid 不是字符串");
    CHECK(!pair_provision_ok(true, false, "acc", "ref", "device"), "password 不是字符串");
    CHECK(!pair_provision_ok(true, true, "", "ref", "device"), "access 空");
    CHECK(!pair_provision_ok(true, true, NULL, "ref", "device"), "access NULL");
    CHECK(!pair_provision_ok(true, true, "acc", "", "device"), "refresh 空");
    CHECK(!pair_provision_ok(true, true, "acc", "ref", "user"), "token_type 不是 device");
    CHECK(!pair_provision_ok(true, true, "acc", "ref", NULL), "token_type 缺");
    CHECK(!pair_provision_ok(true, true, "acc", "ref", "Device"), "大小写敏感");
    CHECK(!pair_provision_ok(true, true, "a\r\nb", "ref", "device"), "令牌带 CRLF（会进 HTTP 头）");
    static char big[2100];
    memset(big, 'a', sizeof(big) - 1);
    big[2041] = '\0';
    CHECK(!pair_provision_ok(true, true, big, "ref", "device"), "access 超 2040 → error_missing_credentials");
}

static void test_depth(void)
{
    const char *ok = "{\"a\":[{\"b\":1}]}";
    CHECK(pair_json_depth_valid((const uint8_t *)ok, strlen(ok)), "浅");
    char deep[100]; int n = 0;
    for (int i = 0; i < 33; i++) deep[n++] = '[';
    for (int i = 0; i < 33; i++) deep[n++] = ']';
    CHECK(!pair_json_depth_valid((const uint8_t *)deep, (size_t)n), "33 层拒");
    CHECK(pair_json_depth_valid((const uint8_t *)deep + 1, (size_t)n - 2), "31 层（去掉最外一对）通过");
    const char *q = "{\"x\":\"[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[\"}";
    CHECK(pair_json_depth_valid((const uint8_t *)q, strlen(q)), "字符串里的括号不算");
    const char *esc = "{\"x\":\"\\\"[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[\"}";
    CHECK(pair_json_depth_valid((const uint8_t *)esc, strlen(esc)), "转义引号不结束字符串");
    CHECK(!pair_json_depth_valid(NULL, 3), "NULL");
}

static void test_identity(void)
{
    const uint8_t mac[6] = {0x3c, 0x84, 0x27, 0xa1, 0xb2, 0xc3};
    char node[32], ble[32], ms[18], dev[48];
    pair_identity_format(mac, node, sizeof(node), ble, sizeof(ble), ms, sizeof(ms), dev, sizeof(dev));
    CHECK(strcmp(node, "homelink-a1b2c3") == 0, "node_id 小写：%s", node);
    CHECK(strcmp(ble, "MuseGadget-A1B2C3") == 0, "BLE 名大写，App 按这个前缀找：%s", ble);
    CHECK(strcmp(ms, "3c:84:27:a1:b2:c3") == 0, "mac：%s", ms);
    CHECK(strcmp(dev, "hatch-link:3c:84:27:a1:b2:c3") == 0, "device_id：%s", dev);
    char small[8];
    pair_identity_format(mac, small, sizeof(small), NULL, 0, NULL, 0, NULL, 0);
    CHECK(strlen(small) == 7, "小缓冲截断不越界");
    pair_identity_format(NULL, node, sizeof(node), NULL, 0, NULL, 0, NULL, 0);
    CHECK(strcmp(node, "homelink-000000") == 0, "MAC 读不到按全零");
}

static void test_scan_and_url(void)
{
    char j[200];
    CHECK(pair_wifi_scan_json(true, j, sizeof(j)) > 0 && strstr(j, "\"Use current connection\"")
          && strstr(j, "\"secure\":false") && strstr(j, "\"rssi\":-40"), "在线：一条当前连接、开放网络");
    CHECK(pair_wifi_scan_json(false, j, sizeof(j)) > 0 && strstr(j, "\"networks\":[]"), "离线：空列表");
    char tiny[10];
    CHECK(pair_wifi_scan_json(true, tiny, sizeof(tiny)) == 0 && tiny[0] == '\0', "装不下返回 0");
    CHECK(strcmp(pair_effective_api_url("https://x.muse.ai"), "https://x.muse.ai") == 0, "https 照用");
    CHECK(strcmp(pair_effective_api_url("http://x.muse.ai"), "") == 0, "http 当没下发（用默认）");
    CHECK(strcmp(pair_effective_api_url(NULL), "") == 0, "NULL");
    CHECK(strcmp(pair_effective_api_url(""), "") == 0, "空");
}

int main(void)
{
    test_res();
    test_provision();
    test_depth();
    test_identity();
    test_scan_and_url();
    printf("%s pair_core：%d 通过，%d 失败\n", s_fail ? "❌" : "✅", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
