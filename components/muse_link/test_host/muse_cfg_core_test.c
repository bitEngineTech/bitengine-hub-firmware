/* muse_cfg_core 的真值表（docs/15 设计 4.4、5.5）。编的是产品源文件 ../muse_cfg_core.c 与 components/utf8。 */
#include <stdio.h>
#include <string.h>
#include "muse_cfg_core.h"
#include "vm_connect.h"     /* 上游原样（header-only）：device.set_vm 与选 VM 都用它的 vm_derive_id 取标签 */

static int s_fail, s_pass;
#define CHECK(cond, ...) do { if (cond) { s_pass++; } else { s_fail++; printf("❌ %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* 测试用的假 token：结构合法（mgst_ + 43 个 base64url，末位在允许集合里），不是任何真实的 token */
#define FAKE_OK "mgst_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"

static void test_sdk_token(void)
{
    char t[64];
    CHECK(strlen(FAKE_OK) == 48, "假 token 长 48");
    CHECK(muse_sdk_token_valid(FAKE_OK), "结构合法 → 通过");
    CHECK(!muse_sdk_token_valid(NULL), "NULL");
    CHECK(!muse_sdk_token_valid(""), "空串");
    snprintf(t, sizeof(t), "%s", FAKE_OK); t[47] = 'B';
    CHECK(!muse_sdk_token_valid(t), "末位 B 不在 AEIMQUYcgkosw048 → 拒（32 字节编码后末位只带 4 位）");
    const char *allowed = "AEIMQUYcgkosw048";
    for (const char *c = allowed; *c; c++) {
        snprintf(t, sizeof(t), "%s", FAKE_OK); t[47] = *c;
        CHECK(muse_sdk_token_valid(t), "末位 %c 允许", *c);
    }
    snprintf(t, sizeof(t), "%s", FAKE_OK); t[0] = 'M';
    CHECK(!muse_sdk_token_valid(t), "前缀不是 mgst_");
    snprintf(t, sizeof(t), "%sA", FAKE_OK);
    CHECK(!muse_sdk_token_valid(t), "49 字符");
    snprintf(t, sizeof(t), "%.47s", FAKE_OK);
    CHECK(!muse_sdk_token_valid(t), "47 字符");
    snprintf(t, sizeof(t), "%s", FAKE_OK); t[10] = '+';
    CHECK(!muse_sdk_token_valid(t), "'+' 不是 base64url");
    snprintf(t, sizeof(t), "%s", FAKE_OK); t[10] = '=';
    CHECK(!muse_sdk_token_valid(t), "'=' 填充不允许");
    snprintf(t, sizeof(t), "%s", FAKE_OK); t[20] = '_'; t[21] = '-';
    CHECK(muse_sdk_token_valid(t), "'_' '-' 允许");
    snprintf(t, sizeof(t), "%s", FAKE_OK); t[30] = (char)0xC3;
    CHECK(!muse_sdk_token_valid(t), "高位字节");
    snprintf(t, sizeof(t), "%s", FAKE_OK); t[5] = ' ';
    CHECK(!muse_sdk_token_valid(t), "空格");
}

static void test_hint(void)
{
    char h[32];
    muse_secret_hint(FAKE_OK, h, sizeof(h));
    CHECK(strcmp(h, "mgst_… (len 48)") == 0, "SDK token 提示只带前缀和长度：%s", h);
    CHECK(strstr(h, "AAAA") == NULL, "提示里没有 mgst_ 之后的字符");
    muse_secret_hint("eyJhbGciOi.xyz", h, sizeof(h));
    CHECK(strcmp(h, "… (len 14)") == 0, "非 SDK token 一个字符都不带：%s", h);
    CHECK(strstr(h, "eyJ") == NULL, "设备令牌一个字符都不带");
    muse_secret_hint(NULL, h, sizeof(h));
    CHECK(strcmp(h, "(unset)") == 0, "NULL");
    muse_secret_hint("", h, sizeof(h));
    CHECK(strcmp(h, "(unset)") == 0, "空串");
    char tiny[4];
    muse_secret_hint(FAKE_OK, tiny, sizeof(tiny));
    CHECK(strlen(tiny) < sizeof(tiny), "小缓冲不越界");
    muse_secret_hint(FAKE_OK, NULL, 10);   /* 不崩 */
}

static void test_display_name(void)
{
    CHECK(muse_display_name_valid("bitEngine Hub 1A2B"), "普通名字");
    CHECK(muse_display_name_valid("客厅网关"), "中文（UTF-8）");
    CHECK(!muse_display_name_valid(""), "空");
    CHECK(!muse_display_name_valid(NULL), "NULL");
    CHECK(!muse_display_name_valid("a\nb"), "换行");
    CHECK(!muse_display_name_valid("a\tb"), "TAB（控制字符）");
    CHECK(!muse_display_name_valid("a\x7f"), "DEL");
    CHECK(muse_display_name_valid("12345678901234567890123456789012"), "正好 32 字节");
    CHECK(!muse_display_name_valid("123456789012345678901234567890123"), "33 字节");
    CHECK(!muse_display_name_valid("\xC3\x28"), "非法 UTF-8");
    CHECK(!muse_display_name_valid("\xE5\xAE"), "截断的 UTF-8");
    CHECK(!muse_display_name_valid("\xC0\xAF"), "超长编码");
}

static void test_urls(void)
{
    CHECK(muse_api_url_valid(""), "空 = 用默认");
    CHECK(muse_api_url_valid("https://api.muse.ai"), "https");
    CHECK(muse_api_url_valid("https://api.muse.ai/v2/x"), "带路径");
    CHECK(!muse_api_url_valid("http://api.muse.ai"), "http 拒（令牌会明文出站）");
    CHECK(!muse_api_url_valid("https://"), "只有前缀");
    CHECK(!muse_api_url_valid("HTTPS://api.muse.ai"), "大写前缀拒（照上游 startswith）");
    CHECK(!muse_api_url_valid("https://a b"), "空白");
    CHECK(!muse_api_url_valid("https://a\"b"), "引号");
    CHECK(!muse_api_url_valid("https://a\r\nX: y"), "CRLF 注入");
    CHECK(!muse_api_url_valid(NULL), "NULL");
    char longurl[200];
    memset(longurl, 'a', sizeof(longurl)); memcpy(longurl, "https://", 8); longurl[129] = '\0';
    CHECK(!muse_api_url_valid(longurl), "129 字节");
    longurl[128] = '\0';
    CHECK(muse_api_url_valid(longurl), "128 字节");

    CHECK(muse_noise_host_valid(""), "noise 空");
    CHECK(muse_noise_host_valid("hatch.metaaivm.com"), "主机名");
    CHECK(!muse_noise_host_valid("hatch.metaaivm.com:443"), "带端口拒");
    CHECK(!muse_noise_host_valid("-x.com"), "- 开头");
    CHECK(!muse_noise_host_valid(".x.com"), ". 开头");
    /* node_id（4.3）：与配对时的格式一致 */
    {
        char nid[24];
        const uint8_t mac[6] = { 0x10, 0x20, 0x30, 0xA1, 0xB2, 0xC3 };
        muse_identity_node_id(mac, nid, sizeof(nid));
        CHECK(strcmp(nid, "homelink-a1b2c3") == 0, "node_id 格式");
        muse_identity_node_id(NULL, nid, sizeof(nid));
        CHECK(strcmp(nid, "homelink-000000") == 0, "node_id 无 MAC 全零");
    }
    /* vm_id（4.13 device.set_vm；进 WS 升级的查询参数） */
    CHECK(muse_vm_id_valid("vm-abc123"), "vm_id 合法");
    CHECK(!muse_vm_id_valid(""), "vm_id 空拒");
    CHECK(!muse_vm_id_valid(NULL), "vm_id NULL 拒");
    CHECK(!muse_vm_id_valid("vm abc"), "vm_id 带空格拒");
    CHECK(!muse_vm_id_valid("vm/../x"), "vm_id 带 / . 拒");
    CHECK(!muse_vm_id_valid("vm\r\nHost: x"), "vm_id 带 CRLF 拒");
    {
        char id[80];
        memset(id, 'a', 64); id[64] = 0;
        CHECK(muse_vm_id_valid(id), "vm_id 64 字节");
        memset(id, 'a', 65); id[65] = 0;
        CHECK(!muse_vm_id_valid(id), "vm_id 65 字节拒");
    }
    CHECK(!muse_noise_host_valid("a/b"), "斜杠");
    CHECK(!muse_noise_host_valid(NULL), "NULL");
}

static void test_tokens(void)
{
    CHECK(muse_token_valid("abc.def-ghi_JKL", 2040), "普通");
    CHECK(!muse_token_valid("", 2040), "空");
    CHECK(!muse_token_valid(NULL, 2040), "NULL");
    CHECK(!muse_token_valid("a b", 2040), "空格（进 HTTP 头）");
    CHECK(!muse_token_valid("a\r\nX-Evil: 1", 2040), "CRLF 头注入");
    char big[2100];
    memset(big, 'a', sizeof(big)); big[2040] = '\0';
    CHECK(muse_token_valid(big, MUSE_ACCESS_TOKEN_MAX), "access 正好 2040");
    big[2040] = 'a'; big[2041] = '\0';
    CHECK(!muse_token_valid(big, MUSE_ACCESS_TOKEN_MAX), "access 2041");
    big[2026] = '\0';
    CHECK(muse_token_valid(big, MUSE_REFRESH_TOKEN_MAX), "refresh 正好 2026");
    big[2026] = 'a'; big[2027] = '\0';
    CHECK(!muse_token_valid(big, MUSE_REFRESH_TOKEN_MAX), "refresh 2027");
}

/* device.set_vm 存的标签与选 VM 时比的标签必须出自同一函数、同一缓冲长度（muse_link.c pick_vm / handle_mgmt）：
   这里钉住边界 —— 64 字节的标签取得出来且合法，65 字节取不出来（不截断：截断的 id 可能指向别的 VM） */
static void test_vm_url_label(void)
{
    char id[MUSE_VM_ID_MAX + 1];
    CHECK(vm_derive_id("wss://abc-1.vm.example/", id, sizeof(id)) == 5 && strcmp(id, "abc-1") == 0 && muse_vm_id_valid(id),
          "普通 VM url → 标签 abc-1");
    char url[160], label[80];
    memset(label, 'a', 64); label[64] = 0;
    snprintf(url, sizeof(url), "wss://%s.vm.example/", label);
    CHECK(vm_derive_id(url, id, sizeof(id)) == 64 && muse_vm_id_valid(id), "64 字节标签取得出、合法");
    memset(label, 'a', 65); label[65] = 0;
    snprintf(url, sizeof(url), "wss://%s.vm.example/", label);
    CHECK(vm_derive_id(url, id, sizeof(id)) == 0, "65 字节标签取不出（不截断）");
    CHECK(vm_derive_id("wss://abc_1.vm.example/", id, sizeof(id)) == 0, "'_' 不是主机名字符");
    CHECK(vm_derive_id("wss://abc/x.y", id, sizeof(id)) == 0, "点在路径里：取出 abc/x，带 / 拒");
    CHECK(vm_derive_id("wss://abc", id, sizeof(id)) == 0, "没有点");
    CHECK(vm_derive_id("wss://.vm.example/", id, sizeof(id)) == 0, "空标签");
    CHECK(vm_derive_id("abc.vm.example", id, sizeof(id)) == 0, "没有 scheme");
    CHECK(vm_derive_id(NULL, id, sizeof(id)) == 0, "NULL url（列表项可能没有 vm_url）");
}

static void test_chat_sid(void)
{
    CHECK(muse_chat_sid_valid("6b1f0a2c-0d3e-4f50-8a6b-7c8d9e0f1a2b"), "合法 UUID");
    CHECK(!muse_chat_sid_valid("6B1F0A2C-0D3E-4F50-8A6B-7C8D9E0F1A2B"), "大写拒（我们只生成小写）");
    CHECK(!muse_chat_sid_valid("6b1f0a2c0d3e4f508a6b7c8d9e0f1a2b"), "没有连字符拒");
    CHECK(!muse_chat_sid_valid("6b1f0a2c-0d3e-4f50-8a6b-7c8d9e0f1a2"), "35 字符拒");
    CHECK(!muse_chat_sid_valid("6b1f0a2c-0d3e-4f50-8a6b-7c8d9e0f1a2bb"), "37 字符拒");
    CHECK(!muse_chat_sid_valid("6b1f0a2c-0d3e-4f50-8a6b-7c8d9e0f1a2\""), "引号拒（它原样进 JSON）");
    CHECK(!muse_chat_sid_valid("6b1f0a2c-0d3e-4f50-8a6b_7c8d9e0f1a2b"), "连字符位置不对拒");
    CHECK(!muse_chat_sid_valid(NULL) && !muse_chat_sid_valid(""), "NULL / 空");
}

int main(void)
{
    test_sdk_token();
    test_hint();
    test_vm_url_label();
    test_chat_sid();
    test_display_name();
    test_urls();
    test_tokens();
    printf("%s muse_cfg_core：%d 通过，%d 失败\n", s_fail ? "❌" : "✅", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
