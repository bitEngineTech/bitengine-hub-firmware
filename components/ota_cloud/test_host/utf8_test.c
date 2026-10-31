/* utf8_safe_copy 的主机侧测试 —— 编的是产品源文件本身（../utf8_trunc.c）。
 *
 * 判据的核心只有一条：**输出永远是合法 UTF-8**，无论输入多长、切在哪。
 * 这正是 2026-09-15 那次线上事故的形状：一条 164 字节的中文 notes 被按字节
 * 砍在第 159 字节，设备回包里留下半个字符。 */
#include "../utf8_trunc.h"

#include <stdio.h>
#include <string.h>

static int pass = 0, fail = 0;

/* 独立的 UTF-8 合法性校验（**不复用**被测代码的判断逻辑，否则同错同对）。 */
static int is_valid_utf8(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        size_t need;
        if      (*p < 0x80)          need = 0;
        else if ((*p & 0xE0) == 0xC0) need = 1;
        else if ((*p & 0xF0) == 0xE0) need = 2;
        else if ((*p & 0xF8) == 0xF0) need = 3;
        else return 0;                       /* 不是合法首字节 */
        p++;
        for (size_t i = 0; i < need; i++) {
            if ((*p & 0xC0) != 0x80) return 0;
            p++;
        }
    }
    return 1;
}

static void check(const char *name, int cond, const char *detail)
{
    if (cond) { printf("✅ %s\n", name); pass++; }
    else      { printf("❌ %s%s%s\n", name, detail ? " — " : "", detail ? detail : ""); fail++; }
}

int main(void)
{
    char buf[160];

    /* 就是线上那一条，164 字节 */
    const char *real =
        "修正 Zigbee 网络「找不到」的误报横幅；Zigbee 页可重新声明信道；"
        "协调器断开不再复位 Zigbee 芯片；长时间无协调器时会提示";
    utf8_safe_copy(buf, sizeof(buf), real);
    check("线上那条 164 字节中文：输出是合法 UTF-8", is_valid_utf8(buf), buf);
    check("确实发生了截断（不是悄悄放行）", strlen(buf) < strlen(real), NULL);
    check("输出是原串的前缀（只截不改）", memcmp(buf, real, strlen(buf)) == 0, buf);
    check("切点落在字符边界（原串的下一个字节是首字节或结尾）",
          real[strlen(buf)] == '\0' || ((unsigned char)real[strlen(buf)] & 0xC0) != 0x80, NULL);

    /* 逐个长度扫一遍：任何切点都必须产出合法 UTF-8 */
    int bad = 0;
    for (size_t cap = 1; cap <= sizeof(buf); cap++) {
        char small[160];
        utf8_safe_copy(small, cap, real);
        if (!is_valid_utf8(small)) { bad++; }
        if (strlen(small) >= cap) { bad++; }      /* 必须留 NUL */
    }
    check("160 个切点逐个扫：全部合法且不越界", bad == 0, NULL);

    /* 不该截断时一个字节都不能动 */
    const char *fits = "短说明 OK";
    utf8_safe_copy(buf, sizeof(buf), fits);
    check("放得下时原样复制", strcmp(buf, fits) == 0, buf);

    /* 纯 ASCII 边界：刚好放满 */
    char tiny[4];
    utf8_safe_copy(tiny, sizeof(tiny), "abcdef");
    check("ASCII 截断到 dst_sz-1", strcmp(tiny, "abc") == 0, tiny);

    /* 4 字节字符（emoji）不能被切开 */
    utf8_safe_copy(tiny, sizeof(tiny), "😀x");     /* 😀 是 4 字节，放不下 */
    check("4 字节字符放不下时整个丢掉", tiny[0] == '\0' && is_valid_utf8(tiny), tiny);

    /* 退化输入不能崩、不能死循环 */
    utf8_safe_copy(buf, sizeof(buf), "");
    check("空串", buf[0] == '\0', NULL);
    utf8_safe_copy(buf, sizeof(buf), NULL);
    check("NULL 源", buf[0] == '\0', NULL);
    utf8_safe_copy(buf, 0, real);
    check("dst_sz=0 不写越界", 1, NULL);
    const char junk[] = { (char)0x80, (char)0x80, (char)0x80, 0 };
    utf8_safe_copy(buf, sizeof(buf), junk);
    check("全是续字节的畸形输入不死循环", 1, NULL);

    printf("\nResult: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
