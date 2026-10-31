/* 版本比较的真值表 —— 主机侧，不用板子。
 *
 * 为什么单独测这个：ota_cloud_version_cmp() 是防降级那道闸的全部实现。
 * 它写错的表现是"该拦的没拦住"或"该升的升不了"，两种在板子上都极难归因：
 * 前者要等到有人真的推一个旧固件才暴露，后者会被当成网络问题。
 *
 *   bash components/ota_cloud/test_host/run.sh
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* 直接编译**产品源文件本身**，不是副本。version_cmp.c 刻意不依赖 ESP-IDF
   就是为了这一步 —— 测副本只能证明副本是对的。 */
#include "../version_cmp.c"

static int fails = 0;

static void expect(const char *a, const char *b, int want, const char *why)
{
    int got = ota_cloud_version_cmp(a, b);
    int norm = got > 0 ? 1 : (got < 0 ? -1 : 0);
    if (norm != want) {
        printf("❌ cmp(\"%s\", \"%s\") = %d, 期望 %d   —— %s\n", a, b, norm, want, why);
        fails++;
    } else {
        printf("✅ cmp(\"%s\", \"%s\") = %d   %s\n", a, b, norm, why);
    }
}

int main(void)
{
    /* ---- 基本序 ---- */
    expect("v0.6.0", "v0.5.0", 1, "minor 更大");
    expect("v0.5.0", "v0.6.0", -1, "minor 更小");
    expect("v1.0.0", "v0.99.99", 1, "major 压倒一切");
    expect("v0.5.1", "v0.5.0", 1, "patch 更大");
    expect("v0.5.0", "v0.5.0", 0, "完全相同");

    /* ---- git describe 后缀：commits 计数参与排序 ---- */
    expect("v0.5.0-165-g1aac8a0", "v0.5.0-164-gf795da4", 1, "同 tag，commits 更多 = 更新");
    expect("v0.5.0-164-gf795da4", "v0.5.0-165-g1aac8a0", -1, "反向");
    expect("v0.5.0-1-gabc0000", "v0.5.0", 1, "带 commits 比裸 tag 新");
    expect("v0.5.0", "v0.5.0", 0, "裸 tag 自比");

    /* ---- 关键陷阱：-g<sha> 里的 sha 绝不能被当成数字 ----
       "v0.5.0-g1aac8a0" 的 g 后面是十六进制。若解析器把 '-' 后面无脑读数字，
       "1aac8a0" 会被读成 1，于是它会莫名其妙地"比裸 tag 新"。 */
    expect("v0.5.0-g1aac8a0", "v0.5.0", 0, "【陷阱】-g<sha> 不是 commits 计数");
    expect("v0.5.0-gf795da4", "v0.5.0-g1aac8a0", 0, "【陷阱】两个 sha 都不参与比较");

    /* ---- dirty 后缀 ---- */
    expect("v0.5.0-165-g1aac8a0-dirty", "v0.5.0-165-g1aac8a0", 0, "-dirty 不影响序");

    /* ---- 前缀与空白 ---- */
    expect("0.6.0", "v0.6.0", 0, "有没有 v 前缀等价");
    expect("V0.6.0", "v0.6.0", 0, "大写 V 等价");

    /* ---- 退化输入（manifest 是不可信输入，不能崩）---- */
    expect("", "v0.5.0", -1, "空串当 0.0.0.0");
    expect("v0.5.0", "", 1, "反向");
    expect("", "", 0, "两个空串");
    expect("garbage", "v0.5.0", -1, "非数字开头当 0");
    expect("v0.5", "v0.5.0", 0, "缺 patch 段按 0");
    expect("v0", "v0.0.0", 0, "只有 major");
    if (ota_cloud_version_cmp(NULL, "v0.5.0") >= 0) { printf("❌ NULL 应视为最旧\n"); fails++; }
    else printf("✅ NULL 视为最旧，未崩溃\n");

    /* ---- 溢出截断（网络输入可以是任意长数字串）---- */
    expect("v999999999999.0.0", "v1.0.0", 1, "超长数字截断后仍更大，且不溢出");

    /* ---- 防降级判据的实际形态：install() 用的是 cmp(latest, current) > 0 ---- */
    expect("v0.5.0-164-gf795da4", "v0.5.0-165-g1aac8a0", -1,
           "【防降级】旧固件对当前 = 负，install 会拒绝");

    /* ---- T9.19 ⑦：NCP 要不要提示更新 ---- */
    #define NB "bitEngine-xncp/1.0 v0.5.0-418-gc9ac5d1 2026-10-07 nvm3c=400"
    #define OB "bitEngine-xncp/1.0 v0.5.0-400-gaaaaaaa 2026-10-06 nvm3c=400"
    #define VS "8.2.2.0 build 532"
    struct { const char *rs, *cb, *cv, *ms, *mb, *mv; bool want; const char *why; } nc[] = {
        { "ABCDEF", NB, VS, "abcdef", OB, VS, false, "有指纹只看指纹（大小写不敏感），构建串不同也不提示" },
        { "abcdef", NB, VS, "123456", NB, VS, true,  "指纹不同就提示" },
        { "",       NB, VS, "123456", NB, VS, false, "【J-Link 首烧】无指纹、构建串相同 → 不提示" },
        { "",       OB, VS, "123456", NB, VS, true,  "【J-Link 首烧】无指纹、构建串不同 → 提示（以前比版本串永远相同）" },
        { NULL,     NB, VS, "123456", "", VS, false, "清单没有 build（老清单）→ 退回比版本串" },
        { "",       "", VS, "123456", NB, VS, false, "NCP 没报构建串（老 NCP）→ 退回比版本串" },
        { "",       "", "unknown", "x", NB, VS, true, "版本也读不到 → 提示，给重刷的机会" },
        { "",       "", "8.2.1.0", "x", "", VS, true,  "版本串不同 → 提示" },
    };
    for (unsigned i = 0; i < sizeof(nc) / sizeof(nc[0]); i++) {
        bool got = ota_cloud_ncp_differs(nc[i].rs, nc[i].cb, nc[i].cv, nc[i].ms, nc[i].mb, nc[i].mv);
        printf("%s ncp_differs = %d   %s\n", got == nc[i].want ? "✅" : "❌", got, nc[i].why);
        if (got != nc[i].want) fails++;
    }

    printf("\n%s\n", fails ? "有失败" : "全部通过");
    return fails ? 1 : 0;
}
