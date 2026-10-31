/*
 * Zigbee / Thread 信道共存规则的主机侧测试。
 *
 * 编的是真源码 components/thread_br/thread_br_channel.c，不是副本。
 *
 * 为什么需要它：VERIFY.sh 里那几条 grep 只能证明"代码长成那个样子"
 * （候选集写着 {15,20,25}、ch26 被排除），证明不了**结果**对。而这类规则出错
 * 的典型方式恰恰是结果错而形状对：阈值边界写成 >= 而不是 >、候选集里某一格
 * 其实无解却静默返回了一个太近的信道。这些在板子上的表现是"偶尔丢包"，
 * 几乎不可能被定位回信道选择。
 *
 * 覆盖：挑信道全表（11-26 逐格）、二级放宽路径、阈值边界、未知输入。
 * 不覆盖：实际建网是否用上了这个结果（那要看 thread_br.c 的 dataset 分支，
 * 需要真 OT 实例），以及两颗射频靠近之后到底掉不掉包（只能上板）。
 */

#include "test_common.h"
#include "thread_br.h"
#include "config_mgr.h"

/* ---- 可控桩：被测代码通过这两个入口读外部状态 ---- */
static zigbee_config_t s_fake_zb;
static esp_err_t       s_fake_active_ret = ESP_OK;
static uint8_t         s_fake_active_ch  = 25;

const zigbee_config_t *config_mgr_get_zigbee(void) { return &s_fake_zb; }

esp_err_t thread_br_get_active_channel(uint8_t *out)
{
    if (s_fake_active_ret != ESP_OK) return s_fake_active_ret;
    if (out) *out = s_fake_active_ch;
    return ESP_OK;
}

/* valid/joined 两个旧参数合并成"用户是否声明过" —— Zigbee 信道现在是声明值，
   不是实测值（实测拿不到，见 ncp_version.h）。保留两个参数是为了让老用例
   逐条对应：只有 declared=true 且信道合法才算已知。 */
static void set_zigbee(bool declared, bool joined, uint8_t ch, bool stale)
{
    (void)stale;
    memset(&s_fake_zb, 0, sizeof(s_fake_zb));
    s_fake_zb.channel_declared = declared && joined;
    s_fake_zb.channel = ch;
}

static int distance(int a, int b) { return a > b ? a - b : b - a; }

/* ------------------------------------------------------------------ */

/* 首要不变量：无论 Zigbee 落在哪一格，选出来的 Thread 信道都必须真的够远。
   这条如果破了，"不要挨太近"这个需求就是没做到，无论代码看起来多对。 */
static void test_every_zigbee_channel_gets_distance(void)
{
    CASE("11-26 逐格：选出的信道间隔必须 > COEX_TOO_CLOSE_MAX");
    for (int zb = 11; zb <= 26; zb++) {
        set_zigbee(true, true, (uint8_t)zb, false);
        uint8_t th = thread_br_pick_channel((uint8_t)zb, true, 25);
        int d = distance(th, zb);
        CHECK(d > COEX_TOO_CLOSE_MAX,
              "zigbee ch%d -> thread ch%u，间隔只有 %d（要求 >%d）",
              zb, th, d, COEX_TOO_CLOSE_MAX);
        CHECK(th >= 11 && th <= 25, "zigbee ch%d 选出越界信道 ch%u", zb, th);
    }
}

/* ch26 贴频段上边沿、发射功率降额，任何情况下都不该被选中。 */
static void test_ch26_never_selected(void)
{
    CASE("ch26 永不入选");
    for (int zb = 11; zb <= 26; zb++) {
        uint8_t th = thread_br_pick_channel((uint8_t)zb, true, 25);
        CHECK(th != 26, "zigbee ch%d 选中了被排除的 ch26", zb);
    }
}

/* ch20 是候选集 {15,20,25} 里唯一无解的一格（15:5 20:0 25:5 全部 <=5），
   必须走二级放宽。其余每一格都应停在 WiFi 空隙信道上。
   这条锁住的是"二级放宽真的会被触发"——它是最容易写成死代码的分支。 */
static void test_second_tier_only_for_ch20(void)
{
    CASE("ch20 走二级放宽 -> ch11，其余留在 {15,20,25}");
    for (int zb = 11; zb <= 26; zb++) {
        uint8_t th = thread_br_pick_channel((uint8_t)zb, true, 25);
        bool in_gap_set = (th == 15 || th == 20 || th == 25);
        if (zb == 20) {
            CHECK(th == 11, "zigbee ch20 应放宽到 ch11，实得 ch%u", th);
        } else {
            CHECK(in_gap_set, "zigbee ch%d 不该放宽，却选了 ch%u", zb, th);
        }
    }
}

/* 出货行为回归锁：当前默认 Zigbee ch15 / Thread ch25，这套规则不能把它改掉。 */
static void test_default_pairing_unchanged(void)
{
    CASE("zigbee ch15 仍然选 ch25（现行出货默认值不变）");
    CHECK(thread_br_pick_channel(15, true, 25) == 25, "默认组合被改变了");
}

/* 没有要躲的东西时不许擅自改变出厂行为，而且必须原样透传配置值
   （硬编码 25 也能让上一条通过，所以这里换个配置值验证）。 */
static void test_unknown_zigbee_returns_configured(void)
{
    CASE("Zigbee 未知 / 越界 -> 原样返回配置值");
    CHECK(thread_br_pick_channel(0,  false, 25) == 25, "未知时没返回配置值");
    CHECK(thread_br_pick_channel(20, false, 25) == 25, "known=false 仍被采信");
    CHECK(thread_br_pick_channel(99, true,  25) == 25, "越界 99 未被拒");
    CHECK(thread_br_pick_channel(10, true,  25) == 25, "越界 10 未被拒");
    CHECK(thread_br_pick_channel(27, true,  25) == 25, "越界 27 未被拒");
    /* 配置值是透传的，不是写死的 25 */
    CHECK(thread_br_pick_channel(0, false, 20) == 20, "配置值没有被透传");
}

/* 阈值边界。间隔正好等于 COEX_TOO_CLOSE_MAX 必须算"太近"，正好多 1 必须算合格。
   < 与 <= 写反是这类代码最典型的失误，且在板上完全看不出来。 */
static void test_too_close_boundary(void)
{
    CASE("间隔边界：==limit 算太近，==limit+1 算合格");
    const uint8_t zb = 15;
    set_zigbee(true, true, zb, false);

    uint8_t got = 0;
    CHECK(thread_br_channel_too_close(zb + COEX_TOO_CLOSE_MAX, &got),
          "间隔 %d 应判太近", COEX_TOO_CLOSE_MAX);
    CHECK(got == zb, "没有回填 Zigbee 信道（实得 %u）", got);
    CHECK(!thread_br_channel_too_close(zb + COEX_TOO_CLOSE_MAX + 1, NULL),
          "间隔 %d 应判合格", COEX_TOO_CLOSE_MAX + 1);
    /* 反方向同样成立 */
    CHECK(thread_br_channel_too_close(zb - COEX_TOO_CLOSE_MAX, NULL),
          "负方向间隔 %d 应判太近", COEX_TOO_CLOSE_MAX);
    CHECK(!thread_br_channel_too_close(zb - COEX_TOO_CLOSE_MAX - 1, NULL),
          "负方向间隔 %d 应判合格", COEX_TOO_CLOSE_MAX + 1);
    /* 同信道当然太近 */
    CHECK(thread_br_channel_too_close(zb, NULL), "同信道未判太近");
}

/* 未知 ≠ 有问题。读不到 Zigbee 信道时不许报告"太近"——那是凭空造告警。 */
static void test_unknown_is_not_a_problem(void)
{
    CASE("Zigbee 读不到 / 未建网 -> 不报太近");
    set_zigbee(false, false, 0, false);
    CHECK(!thread_br_channel_too_close(15, NULL), "读不到时报了太近");
    set_zigbee(true, false, 0, false);          /* 读到了，但 NCP 没建网 */
    CHECK(!thread_br_channel_too_close(15, NULL), "未建网时报了太近");
}

/* 共存结论：两边都得是实测值才 known，任一侧缺失就不下结论。 */
static void test_coex_status(void)
{
    coex_status_t c;

    CASE("两边都读到 -> known，且 separation / too_close 正确");
    set_zigbee(true, true, 15, false);
    s_fake_active_ret = ESP_OK; s_fake_active_ch = 25;
    thread_br_get_coex_status(&c);
    CHECK(c.known, "两边都有却 known=false");
    CHECK(c.zigbee_channel == 15 && c.thread_channel == 25, "信道回填错误");
    CHECK(c.separation == 10, "separation 应为 10，实得 %u", c.separation);
    CHECK(!c.too_close, "间隔 10 被判太近");

    CASE("靠太近时 too_close 置位");
    s_fake_active_ch = 19;                       /* 与 15 相距 4 */
    thread_br_get_coex_status(&c);
    CHECK(c.known && c.too_close, "间隔 4 未判太近");
    CHECK(c.separation == 4, "separation 应为 4，实得 %u", c.separation);

    CASE("Thread 读不到 -> 不下结论");
    s_fake_active_ret = ESP_ERR_TIMEOUT;
    thread_br_get_coex_status(&c);
    CHECK(!c.known, "Thread 信道读不到却给了结论");
    CHECK(!c.too_close, "未知却报太近");

    CASE("Zigbee 读不到 -> 不下结论");
    s_fake_active_ret = ESP_OK;
    set_zigbee(false, false, 0, false);
    thread_br_get_coex_status(&c);
    CHECK(!c.known, "Zigbee 信道读不到却给了结论");

    CASE("NULL 参数不崩溃");
    thread_br_get_coex_status(NULL);
}

/* 开机重选：只在"从未接入过设备"时才允许动网络。
   had_child 这一票是否决票，任何其他条件都不能推翻它 —— 写反了就会把用户
   正在用的 Thread 网络搬走，而这在板子上表现为"设备毫无征兆全部掉线"。 */
static void test_should_repick(void)
{
    uint8_t nch;

    CASE("空网络 + 靠太近 -> 重选，且新信道真的够远");
    set_zigbee(true, true, 25, false);
    nch = 0;
    CHECK(thread_br_should_repick(25, false, &nch), "空网络撞车却不重选");
    CHECK(nch == 15, "应选 ch15，实得 ch%u", nch);
    CHECK(distance(nch, 25) > COEX_TOO_CLOSE_MAX, "重选后仍然太近");

    CASE("接入过设备 -> 一律不动（否决票）");
    CHECK(!thread_br_should_repick(25, true, NULL), "有过设备仍然重选了");

    CASE("间隔够 -> 不动");
    set_zigbee(true, true, 11, false);
    CHECK(!thread_br_should_repick(25, false, NULL), "间隔 14 却重选");

    CASE("边界：间隔 ==limit 要动，==limit+1 不动");
    set_zigbee(true, true, 15, false);
    CHECK(thread_br_should_repick(15 + COEX_TOO_CLOSE_MAX, false, NULL),
          "间隔 %d 未触发重选", COEX_TOO_CLOSE_MAX);
    CHECK(!thread_br_should_repick(15 + COEX_TOO_CLOSE_MAX + 1, false, NULL),
          "间隔 %d 不该重选", COEX_TOO_CLOSE_MAX + 1);

    CASE("Zigbee 未知 / 未建网 -> 不动（产线首烧后的常态，别瞎搬）");
    set_zigbee(false, false, 0, false);
    CHECK(!thread_br_should_repick(25, false, NULL), "Zigbee 未知却重选");
    set_zigbee(true, false, 0, false);
    CHECK(!thread_br_should_repick(25, false, NULL), "Zigbee 未建网却重选");

    CASE("NULL 输出参数不崩溃");
    set_zigbee(true, true, 25, false);
    CHECK(thread_br_should_repick(25, false, NULL), "NULL 时结论不该改变");

    CASE("重选结果对每一格都成立且不自我循环");
    for (int zb = 11; zb <= 26; zb++) {
        set_zigbee(true, true, (uint8_t)zb, false);
        for (int cur = 11; cur <= 26; cur++) {
            nch = 0;
            if (!thread_br_should_repick((uint8_t)cur, false, &nch)) continue;
            CHECK(nch != cur, "zigbee ch%d：重选到了原信道 ch%u", zb, nch);
            CHECK(distance(nch, zb) > COEX_TOO_CLOSE_MAX,
                  "zigbee ch%d：从 ch%d 重选到 ch%u 仍然太近", zb, cur, nch);
        }
    }
}

/* 给 WebUI 用的"可用 Zigbee 信道"列表。规则必须和挑信道/告警是同一条 —— 这份
   列表要显示给用户照着去配 Z2M，多一格少一格都会把他引到冲突信道上。 */
static void test_safe_zigbee_list(void)
{
    uint8_t out[16];

    CASE("列表里每一格都真的够远，且不漏不重");
    for (int th = 11; th <= 26; th++) {
        uint8_t n = thread_br_list_safe_zigbee_channels((uint8_t)th, out, sizeof(out));
        int expect = 0;
        for (int c = 11; c <= 26; c++) if (distance(c, th) > COEX_TOO_CLOSE_MAX) expect++;
        CHECK(n == expect, "thread ch%d：应有 %d 个可用信道，实得 %u", th, expect, n);
        for (uint8_t i = 0; i < n; i++) {
            CHECK(distance(out[i], th) > COEX_TOO_CLOSE_MAX,
                  "thread ch%d：列出了太近的 ch%u", th, out[i]);
            CHECK(out[i] >= 11 && out[i] <= 26, "列出了越界信道 ch%u", out[i]);
            if (i) CHECK(out[i] > out[i - 1], "列表未严格递增");
        }
    }

    CASE("与 too_close 判定完全一致（同一条规则的两个出口）");
    for (int th = 11; th <= 26; th++) {
        uint8_t n = thread_br_list_safe_zigbee_channels((uint8_t)th, out, sizeof(out));
        for (int c = 11; c <= 26; c++) {
            bool in_list = false;
            for (uint8_t i = 0; i < n; i++) if (out[i] == c) in_list = true;
            set_zigbee(true, true, (uint8_t)c, false);
            bool too_close = thread_br_channel_too_close((uint8_t)th, NULL);
            CHECK(in_list == !too_close,
                  "thread ch%d / zigbee ch%d：列表说%s，too_close 说%s",
                  th, c, in_list ? "可用" : "不可用", too_close ? "太近" : "不近");
        }
    }

    CASE("ch26 不因 Thread 的取舍被排除（Zigbee 信道归 Z2M 决定）");
    uint8_t n = thread_br_list_safe_zigbee_channels(11, out, sizeof(out));
    bool has26 = false;
    for (uint8_t i = 0; i < n; i++) if (out[i] == 26) has26 = true;
    CHECK(has26, "thread ch11 时 ch26 间隔 15，不该被剔除");

    CASE("非法输入与缓冲区上限");
    CHECK(thread_br_list_safe_zigbee_channels(10, out, sizeof(out)) == 0, "越界 10 未拒");
    CHECK(thread_br_list_safe_zigbee_channels(27, out, sizeof(out)) == 0, "越界 27 未拒");
    CHECK(thread_br_list_safe_zigbee_channels(25, NULL, 16) == 0, "NULL 未拒");
    CHECK(thread_br_list_safe_zigbee_channels(25, out, 0) == 0, "max=0 未拒");
    CHECK(thread_br_list_safe_zigbee_channels(25, out, 3) == 3, "未遵守 max 上限");
}

int main(void)
{
    printf("=== 信道选择 ===\n");
    test_every_zigbee_channel_gets_distance();
    test_ch26_never_selected();
    test_second_tier_only_for_ch20();
    test_default_pairing_unchanged();
    test_unknown_zigbee_returns_configured();

    printf("=== 共存判定 ===\n");
    test_too_close_boundary();
    test_unknown_is_not_a_problem();
    test_coex_status();

    printf("=== 开机重选 ===\n");
    test_should_repick();

    printf("=== 可用 Zigbee 信道列表 ===\n");
    test_safe_zigbee_list();

    return srp_test_summary("channel");
}
