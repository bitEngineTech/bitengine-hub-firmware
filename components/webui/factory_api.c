/* factory_api.c — 产线自检：一次调用，一个判定
 *
 * ── 为什么不是再加十个接口 ────────────────────────────────────────────
 *
 * 单项数据本来就够多了（/status、/diag/heap、/diag/uart、/zigbee/status、
 * /poe/status、/led/test…）。产线缺的不是数据，是**判定**：让操作员或夹具
 * 去拼十个 JSON、各自记住阈值，等于把判据散在治具脚本里——阈值一改就没人知道
 * 产线还在按老的判。所以判据集中在固件里，接口只回 pass/fail/manual。
 *
 * ── 三种结论，不允许第四种 ────────────────────────────────────────────
 *
 *   pass    自动判定通过
 *   fail    自动判定不通过（detail 里必须说清是什么值不满足什么阈值）
 *   manual  **固件判不了**，需要人眼或治具。detail 给出判断所需的原始值。
 *
 * 关键是 manual 不能被算成 pass。整机 verdict 只有在"零个 fail 且零个 manual"
 * 时才是 pass，否则是 fail 或 incomplete —— 产线不该出现"没测但看着像过了"。
 * 本项目吃过这个亏：恢复出厂的检查脚本 7/7 全绿，而 Thread 密钥其实已经没了，
 * 因为那 7 项恰好都不覆盖密钥。
 *
 * ── manual 必须有办法被消掉（2026-08-17 补） ──────────────────────────
 *
 * 上面那条规则写对了，但**没有提供消 manual 的手段**：led_visual 无条件写死
 * "manual"，于是 verdict 的 pass 分支是**死分支**，tools/factory_test.sh 每次
 * 必然 exit 1。操作员在 shell 里回答了"四颗全白"，那个答案却从不回写设备 ——
 * 产线最后一道机器判定门永久红灯，等于没有门。
 *
 * 现在由 POST /api/v1/factory/ack 回写：
 *     {"item":"led_visual","result":"pass"}   人工判定通过
 *     {"item":"led_visual","result":"fail"}   人工判定不通过 → 计入 n_fail
 *     {"clear":true}                          清空（同一块板重测时用）
 *
 * 三条约束，别放宽：
 *   1. **只作用于 manual**。ack 永远不能把自动判定的 fail 洗成 pass ——
 *      否则产线多了一个"一键放行"的后门，这比没有门更糟。
 *   2. **只存内存**，不落 NVS。断电即失效，确认不会跨板残留。
 *   3. **白名单**。只有真会出现 manual 的三项可 ack；写错名字直接 400，
 *      而不是静默无效（静默无效 = 操作员以为消了，实际没消）。
 *
 * ── 按键为什么单独处理 ────────────────────────────────────────────────
 *
 * 按键是整机唯一一个产线不能随便碰的输入：短按重启、>10s 擦配置。安全的只有
 * 2-10 秒那一档（落在保护带，不触发动作）。所以流程是：读一次 press_count →
 * 让操作员按住约 5 秒 → 再读，count +1 且 last_action == "none"。
 * 一次按压就覆盖 GPIO、上拉、去抖、计时、分档全链路。见 button.h。
 */

#include "webui.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdarg.h>

#include "cJSON.h"

#include "esp_log.h"
#include "esp_chip_info.h"
#include "device_identity.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "ping/ping_sock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_secure_boot.h"    /* app_signed 自检项 —— 首烧必须是签名版 */
#include "esp_wifi_types.h"     /* wifi_ap_record_t —— wifi_scan 自检项 */
#include "esp_timer.h"          /* flash_rw 的每次不同的写入值 */

#include "network_mgr.h"
#include "watchdog.h"
#include "ezsp_bridge.h"
#include "ncp_version.h"    /* ncp_tables 自检项：整板上电时读的 NCP 表容量 */
#include "config_mgr.h"
#include "led_ctrl.h"
#include "button.h"
#include "poe.h"
#include "nvs.h"

static const char *TAG = "factory";

/* 内部 SRAM 的产线下限。本板内部堆只有 175KB，启动完就见底；实测运行时低水位
   1-3KB，低于 1KB 时 WiFi/LWIP 缓冲区会开始分配失败（每秒数次）。定在 1024 是
   "还能跑但已经该警惕"的线 —— 比它低就是这块板的 PSRAM 没起来或有东西泄漏。 */
#define MIN_INTERNAL_FREE   1024

/* MG24 的 EZSP 链路：产线上只要有过双向字节就算通。不设更高的门槛，因为
   ncp_version 读取本身会软复位 MG24，产线不该为了测试去复位它。 */
#define MIN_UART_BYTES      64
/* ncp_tables：量产 NCP 的直连子设备编译值。正本是 mg24/ncp/bitengine_ncp.slcp 的
   SL_ZIGBEE_MAX_END_DEVICE_CHILDREN（tools/check_capacity_docs.py 盯着两边一致）。 */
#define FACTORY_NCP_END_DEVICE_CHILDREN 64
/* eth_ping：从以太网口 ping 网关。3 发、每发 500ms 超时、间隔 200ms，最坏约 2.1s；
   ≥1 个应答即 pass —— 产线要拦的是"链路起来了但一个包都发不出去/收不回来"
   （PHY 与 C5 之间 SPI 数据线虚焊、MAC 地址没写、DM9051 中断线问题都长这样），
   不是测丢包率。 */
#define ETH_PING_COUNT      3
#define ETH_PING_TIMEOUT_MS 500
#define ETH_PING_INTERVAL_MS 200

/* ---- 人工确认（ack）表 ------------------------------------------------
   只存内存：断电即失效，确认不会跨板残留。httpd 是单任务串行的（同 selftest
   里那块 scratch 的理由），所以裸静态数组不需要锁。 */
#define ACK_NAME_LEN    24
#define ACK_MAX         8

/* 白名单 —— 只有这几项在自动判定下可能落到 manual。
   led_visual 恒 manual；wifi 在产线不配 WiFi 时 manual；button 未按过时 manual；
   wifi_scan 在产线没有 5GHz AP 环境时 manual；poe 在用 USB 供电时 manual。

   ⚠️ 新增一个**可能落到 manual** 的自检项时，必须同时加进这张白名单。
      un-ackable 的 manual 会让 verdict 永久卡在 incomplete —— 产线拿到的是一个
      消不掉的黄灯，而不是一个能修的红灯（identity 那一项之所以用 fail，就是
      为了避开这件事）。ACK_MAX 必须 >= 本数组长度，否则第 N+1 个 ack 存不进去，
      表现为"确认了但 verdict 没变"。
   ⚠️ 加进来之后，tools/factory_test.sh 与 .ps1 **两边**都要有对应的处理，
      否则那一项在产线上永远消不掉。tools/check_factory_test_parity.py 会验这件事。 */
static const char *const ACKABLE[] = {
    "led_visual", "wifi", "button", "wifi_scan", "poe",
};
_Static_assert(sizeof(ACKABLE) / sizeof(ACKABLE[0]) <= ACK_MAX,
               "ACK_MAX 装不下白名单：会出现确认了却消不掉的 manual");

static struct {
    char name[ACK_NAME_LEN];
    bool pass;
} s_acks[ACK_MAX];
static int s_ack_count;

static bool ack_is_allowed(const char *name)
{
    for (size_t i = 0; i < sizeof(ACKABLE) / sizeof(ACKABLE[0]); i++)
        if (strcmp(name, ACKABLE[i]) == 0) return true;
    return false;
}

/* 找到返回 true，并把操作员的判定写进 *pass */
static bool ack_lookup(const char *name, bool *pass)
{
    for (int i = 0; i < s_ack_count; i++) {
        if (strcmp(s_acks[i].name, name) == 0) { *pass = s_acks[i].pass; return true; }
    }
    return false;
}

typedef struct {
    char   buf[2816];   /* 每项约 100 B（2026-10-07 实测整份 1.8 KB），留足余量：溢出会让整次自检回 500 */
    int    off;
    int    n_fail;
    int    n_manual;
    int    n_acked;
    bool   overflow;
} rep_t;

/* 扫描结果条数上限。判据只是"有没有 >=1 个 5GHz AP"，不需要完整列表；
   wifi_ap_record_t 每条 200+ 字节，多要几条只是白占 PSRAM。 */
#define SELFTEST_SCAN_MAX   8

/* 报告结构与输出缓冲合成一块，一次分配一次释放：返回路径越少，漏 free 的机会越少。
   扫描结果也放进来：wifi_ap_record_t 挺大，绝不能放栈上（httpd 任务栈有限），
   而这块本来就优先落在 PSRAM。 */
typedef struct {
    rep_t r;
    wifi_ap_record_t aps[SELFTEST_SCAN_MAX];
    char  out[3328];
} selftest_scratch_t;

/* ---- flash_rw：容量对 != 写得进去（T2.2）--------------------------------
 *
 * 原来的 `flash` 项只调 esp_flash_get_size()，那读的是 SPI Flash 的**ID/容量**，
 * 一颗焊虚了、写不进去的片子照样报 8 MB。真正会在产线出现的故障是"配置存不住"：
 * 用户设完密码重启回到出厂态，而自检全绿。
 *
 * 判据是一个完整的写-读-擦回合，而不是只写不读：
 *   1. 写一个**每次都不同**的值（含 esp_timer 的微秒数）——写死常量的话，
 *      上一次残留的键会让"根本没写进去"也读成功，这个自检就退化成恒真
 *   2. 关掉句柄重开再读 —— 不重开只能证明 NVS 的内存缓存，证明不了落盘
 *   3. 读完擦掉并 commit —— 不留垃圾，重复自检也不会把 NVS 撑大
 *
 * ⚠️ **绝不能碰出厂身份那个分区**。身份写在独立分区里，擦掉 = 设备永久失去身份，
 *    而且外部零症状（见 components/device_identity）。这里用的是默认 "nvs" 分区上
 *    的 selftest 命名空间：nvs_open() 只作用于默认分区，够不到它 —— 但仍然写死
 *    这条注释，因为下一个人很可能想"顺手验证一下那个分区能不能写"。那个念头本身
 *    就是错的，固件侧只有 nvs_get_*。
 *
 * ⚠️ 每调一次自检就是一次 NVS 写+擦。接口挂着 AUTH_REQUIRED，且产线一块板只跑
 *    一两次，磨损可以忽略；但别把它接进任何轮询或看板刷新里。
 */
#define SELFTEST_NS     "selftest"
#define SELFTEST_KEY    "rw_probe"

static bool flash_rw_check(char *detail, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SELFTEST_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        snprintf(detail, len, "nvs_open 失败：%s", esp_err_to_name(err));
        return false;
    }

    /* 每次不同 —— 否则残留的旧键会让"没写进去"也读成功 */
    uint64_t want = ((uint64_t)0xBE17 << 48) | (uint64_t)esp_timer_get_time();

    err = nvs_set_u64(h, SELFTEST_KEY, want);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        snprintf(detail, len, "写入失败：%s", esp_err_to_name(err));
        return false;
    }

    /* 重开句柄再读：不重开读到的可能只是内存里的缓存，证明不了落盘 */
    err = nvs_open(SELFTEST_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        snprintf(detail, len, "回读时 nvs_open 失败：%s", esp_err_to_name(err));
        return false;
    }
    uint64_t got = 0;
    err = nvs_get_u64(h, SELFTEST_KEY, &got);

    /* 不管读得对不对都要擦干净：失败路径留下垃圾，下一轮自检就带着脏数据跑 */
    esp_err_t eerr = nvs_erase_key(h, SELFTEST_KEY);
    if (eerr == ESP_OK) eerr = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        snprintf(detail, len, "回读失败：%s", esp_err_to_name(err));
        return false;
    }
    if (got != want) {
        snprintf(detail, len, "回读不一致：写 %016llX 读 %016llX",
                 (unsigned long long)want, (unsigned long long)got);
        return false;
    }
    if (eerr != ESP_OK) {
        /* 写读都对但擦不掉：Flash 已经在退化，别报成 pass */
        snprintf(detail, len, "写读一致但擦除失败：%s", esp_err_to_name(eerr));
        return false;
    }

    /* ⚠️ 这句话里**不要**出现那个分区的名字：VERIFY 的反向不变量连字符串字面量
       一起查（nvs_open("…") 的证据就在字面量里），写进去会让检查恒红。 */
    snprintf(detail, len, "写-读-擦一个回合通过（命名空间 %s，未触碰出厂身份分区）",
             SELFTEST_NS);
    return true;
}

/* ---- eth_ping：以太网口真的通不通 ----
   `ethernet` 那项**只看 link up**（不看 IP，见下方 item 行），一块 PHY↔C5 之间 SPI
   数据线虚焊、或 MAC 没初始化好的板子，link 灯照亮、DHCP 拿不到，产线看不见。
   这里从**以太网口**（不是默认路由，那可能是 WiFi）向网关发 3 个 ICMP echo。

   H1 验证过的 API 语义（ping_sock.h，IDF v6.0.2）：
   - esp_ping_config_t.interface 是 lwIP netif index（0 = NETIF_NO_INDEX = 走默认路由）；
     esp_netif_get_netif_impl_index() 返回的正是这个 index，用它把包钉在 ETH 口。
   - esp_ping_start() 起一个内部任务后立即返回；会话结束由 on_ping_end 回调通知
     （回调跑在 ping 任务里，**不能**在回调里 esp_ping_delete_session）。
   - esp_ping_get_profile(ESP_PING_PROF_REPLY) 读到的是本会话收到的应答数。
   返回 true = pass。detail 写清目标、应答数、最后一次 RTT。 */
static void eth_ping_on_end(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    xSemaphoreGive((SemaphoreHandle_t)args);
}

static bool eth_ping_check(const network_status_t *ns, char *detail, size_t len)
{
    char gw[16];
    snprintf(gw, sizeof(gw), IPSTR, IP2STR(&ns->eth_gateway));

    if (!ns->eth_link_up) {
        snprintf(detail, len, "link down，跳过 ping（先过 ethernet 项）");
        return false;
    }
    if (ns->eth_gateway.addr == 0) {
        /* 2026-09-13 真板插回网线时抓到过这条：link 已起、DHCP 还没回，ip 也是 0.0.0.0，
           几秒后自己好。产线上稳定停在这里才是故障（DHCP 不通 / 静态配置漏网关） */
        snprintf(detail, len, "以太网 ip=" IPSTR " 没有网关地址（DHCP 还没给或不通？静态配置漏了网关？）",
                 IP2STR(&ns->eth_ip));
        return false;
    }
    /* ETH_DEF 是 ESP_NETIF_DEFAULT_ETH() 的 if_key（eth_mgr.c 用的就是它） */
    esp_netif_t *eth = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (!eth) {
        snprintf(detail, len, "找不到以太网 netif（ETH_DEF）");
        return false;
    }
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        snprintf(detail, len, "内存不足，建不出信号量");
        return false;
    }

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count       = ETH_PING_COUNT;
    cfg.interval_ms = ETH_PING_INTERVAL_MS;
    cfg.timeout_ms  = ETH_PING_TIMEOUT_MS;
    cfg.data_size   = 32;
    cfg.interface   = esp_netif_get_netif_impl_index(eth);
    cfg.target_addr.type = IPADDR_TYPE_V4;
    cfg.target_addr.u_addr.ip4.addr = ns->eth_gateway.addr;

    esp_ping_callbacks_t cbs = { .cb_args = done, .on_ping_end = eth_ping_on_end };
    esp_ping_handle_t hdl = NULL;
    esp_err_t err = esp_ping_new_session(&cfg, &cbs, &hdl);
    if (err != ESP_OK) {
        vSemaphoreDelete(done);
        snprintf(detail, len, "建 ping 会话失败：%s", esp_err_to_name(err));
        return false;
    }
    err = esp_ping_start(hdl);
    if (err != ESP_OK) {
        esp_ping_delete_session(hdl);
        vSemaphoreDelete(done);
        snprintf(detail, len, "ping 启动失败：%s", esp_err_to_name(err));
        return false;
    }
    /* 最坏 count×(timeout+interval)，再给 1s 余量；等不到就按 0 应答算 —— 别让自检挂死 */
    const TickType_t budget = pdMS_TO_TICKS(ETH_PING_COUNT * (ETH_PING_TIMEOUT_MS + ETH_PING_INTERVAL_MS) + 1000);
    bool ended = xSemaphoreTake(done, budget) == pdTRUE;
    if (!ended) esp_ping_stop(hdl);

    uint32_t sent = 0, got = 0, rtt = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY,   &got,  sizeof(got));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &rtt,  sizeof(rtt));
    esp_ping_delete_session(hdl);
    vSemaphoreDelete(done);

    snprintf(detail, len, "ETH → %s：应答 %"PRIu32"/%"PRIu32"%s%s",
             gw, got, sent, got ? "，最后一次 " : "", got ? "" : "（要求 ≥ 1）");
    if (got) {
        size_t n = strlen(detail);
        /* 局域网一跳通常不到 1ms，TIMEGAP 取整后是 0 —— 打成 "0 ms" 像个 bug */
        if (rtt == 0) snprintf(detail + n, len > n ? len - n : 0, "<1 ms");
        else          snprintf(detail + n, len > n ? len - n : 0, "%"PRIu32" ms", rtt);
    }
    return got >= 1;
}

static void item(rep_t *r, const char *name, const char *state, const char *fmt, ...)
{
    if (r->overflow) return;
    va_list ap;
    char detail[192];
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);

    /* 人工确认只在 state == "manual" 时生效 —— 见文件头约束 1：
       ack 绝不能把自动判定的 fail 洗成 pass。 */
    if (strcmp(state, "manual") == 0) {
        bool ack_pass;
        if (ack_lookup(name, &ack_pass)) {
            state = ack_pass ? "pass" : "fail";
            r->n_acked++;
            strlcat(detail, ack_pass ? "｜操作员确认：通过" : "｜操作员确认：不通过",
                    sizeof(detail));
        }
    }

    /* detail 里可能带引号（比如版本串），必须转义，否则整个 JSON 报废而
       治具只会看到"解析失败"，查不到是哪一项。 */
    char esc[sizeof(detail) * 2];
    int e = 0;
    for (int i = 0; detail[i] && e < (int)sizeof(esc) - 2; i++) {
        if (detail[i] == '"' || detail[i] == '\\') esc[e++] = '\\';
        esc[e++] = detail[i];
    }
    esc[e] = '\0';

    int w = snprintf(r->buf + r->off, sizeof(r->buf) - r->off,
                     "%s{\"name\":\"%s\",\"state\":\"%s\",\"detail\":\"%s\"}",
                     r->off ? "," : "", name, state, esc);
    if (w < 0 || w >= (int)sizeof(r->buf) - r->off) { r->overflow = true; return; }
    r->off += w;

    if (strcmp(state, "fail") == 0)        r->n_fail++;
    else if (strcmp(state, "manual") == 0) r->n_manual++;
}

// ---- GET /api/v1/factory/selftest ----
static esp_err_t handle_factory_selftest(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    /* 这两块曾是函数内 static，合计 4624 字节常驻 .bss —— 本项目自有代码最大的
       一笔内部 SRAM 占用，而它服务的是**产线自检**，出厂后几乎不再调用。
       2026-08-17 实测 internal min_free 只剩 12 字节，改为按需分配、优先 PSRAM。
       httpd 是单任务串行的，不存在两次自检并发，所以一块 scratch 够用。 */
    selftest_scratch_t *s = heap_caps_malloc(sizeof(*s),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s) s = malloc(sizeof(*s));      /* PSRAM 拿不到就退回内部堆 */
    if (!s) return send_error(req, 500, "no_mem");
    memset(s, 0, sizeof(*s));
    rep_t *rp = &s->r;
    esp_err_t ret;

    /* ---- 身份：SN 必须存在且格式合法 ----
       2026-09-11 之前这项是**无条件 pass**，只把 MAC 打出来给产线贴标。
       现在它是产线上唯一能拦住"忘了写出厂身份就出货"的地方：身份写在 fctry
       分区，固件只读，写漏了外部零症状 —— 一切功能照常，直到几个月后售后要查
       SN 才发现，那时已经无从恢复。

       ⚠️ 用 fail 不用 manual：ACKABLE 白名单里没有 identity（也不该加，见文件头
          约束 1），un-ackable 的 manual 会让 verdict 永久卡在 incomplete，
          产线得到的是一个消不掉的黄灯而不是一个能修的红灯。
       ⚠️ 这意味着**所有 fctry 未写的板子从此 identity 报 fail**，包括开发板和
          产线上还没走到写身份那步的板子。这是刻意的：工位顺序是"烧固件 →
          写身份（tools/generate_nvs.py --flash）→ 验收"，验收时没身份就该红。 */
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    /* ⚠️ **不要**用 esp_efuse_mac_get_default(uint8_t[6])（2026-09-11 修）。
       ESP32-C5 支持 802.15.4，那个 API 在这种芯片上返回的是 **8 字节 EUI-64**
       （esp_mac.h:104「length: 6 bytes/8 bytes … 8 bytes for EUI-64」）：
       mac_addr.c 的 insert_mac_ext_into_mac() 把 2 字节 MAC_EXT 插到中间，
       再 memcpy(&mac[5], tmp, 3) —— 对一个 6 字节数组来说，这是**写越界 2 字节**。

       后果不只是踩栈，打出来的值本身就是错的：它是 EUI-64 的前 6 字节
       = 基址前 3 字节 + MAC_EXT + 基址第 4 字节。本板实测打出 3844BEFEFFA3，
       而真实 MAC 是 3844BE123456（FEFF 是 MAC_EXT）。产线一直照着这个值贴标，
       而它在网络上根本不存在。

       ESP_MAC_EFUSE_FACTORY 才是「乐鑫出厂烧进 eFuse 的那 6 字节」，
       也正是 esptool read-mac 报的值 —— 产线追溯要的就是它，
       tools/generate_nvs.py 派生序列号用的也是它。 */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);

    /* 身份那半截先拼好再交给 item()：两个分支写成两个格式串的话，参数个数和
       类型必须手工对齐，改一边就是栈上乱读。 */
    char id_part[96];
    bool has_id = device_identity_valid();
    if (has_id) {
        snprintf(id_part, sizeof(id_part), "SN=%s hw=%s 产期=%s",
                 device_identity_serial(),
                 device_identity_hw_revision()[0] ? device_identity_hw_revision() : "?",
                 device_identity_mfg_date()[0]    ? device_identity_mfg_date()    : "?");
    } else {
        strlcpy(id_part, "无出厂身份 —— 先跑 tools/generate_nvs.py --port <口> --flash",
                sizeof(id_part));
    }
    item(rp, "identity", has_id ? "pass" : "fail",
         "%s · ESP32-C%d rev%d.%d mac=%02X%02X%02X%02X%02X%02X",
         id_part,
         chip.model == CHIP_ESP32C5 ? 5 : 0, chip.revision / 100, chip.revision % 100,
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    const esp_app_desc_t *app = esp_app_get_description();
    item(rp, "firmware", "pass", "%s", app ? app->version : "unknown");

    /* 首烧的必须是**签名版**镜像。OTA 的信任锚是运行中 app 自己的签名块
       （CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT，见 sdkconfig.defaults 末尾），
       所以刷成 build/bitengine-unsigned.bin 的整机会永久拒绝一切 OTA，
       而且外部零症状 —— 用户第一次点升级才发现，那时已经出货。
       这是产线唯一能拦住它的地方，故为 fail 而非 manual。 */
    esp_image_sig_public_key_digests_t sig = {0};
    bool signed_ok = (esp_secure_boot_get_signature_blocks_for_running_app(false, &sig) == ESP_OK
                      && sig.num_digests > 0);
    item(rp, "app_signed", signed_ok ? "pass" : "fail",
         signed_ok ? "%u signature block(s)"
                   : "无签名块 —— 本机永久无法 OTA，刷成 unsigned 版了？(%u)",
         (unsigned)sig.num_digests);

    /* ---- Flash / PSRAM / 内部 SRAM ---- */
    uint32_t fsz = 0;
    esp_flash_get_size(NULL, &fsz);
    item(rp, "flash", fsz >= 8 * 1024 * 1024 ? "pass" : "fail",
         "%"PRIu32" MB（要求 >= 8）", fsz / (1024 * 1024));

    char frw[96];
    bool frw_ok = flash_rw_check(frw, sizeof(frw));
    item(rp, "flash_rw", frw_ok ? "pass" : "fail", "%s", frw);

    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    item(rp, "psram", psram_free > 1024 * 1024 ? "pass" : "fail",
         "空闲 %u KB（要求 > 1024）", (unsigned)(psram_free / 1024));

    size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t int_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    item(rp, "internal_ram", int_min >= MIN_INTERNAL_FREE ? "pass" : "fail",
         "当前 %u B / 低水位 %u B（要求低水位 >= %d）",
         (unsigned)int_free, (unsigned)int_min, MIN_INTERNAL_FREE);

    /* ---- MG24 / EZSP 链路 ---- */
    const health_status_t *hs = watchdog_get_status();
    const bridge_stats_t  *bs = ezsp_bridge_get_stats();
    bool mg24_ok = hs && hs->mg24_alive;
    item(rp, "mg24_ncp", mg24_ok ? "pass" : "fail",
         "alive=%d resets=%"PRIu32, mg24_ok, hs ? hs->mg24_total_resets : 0);

    uint32_t urx = bs ? bs->uart_rx_bytes : 0, utx = bs ? bs->uart_tx_bytes : 0;
    item(rp, "ezsp_uart", (urx >= MIN_UART_BYTES && utx >= MIN_UART_BYTES) ? "pass" : "fail",
         "rx=%"PRIu32" tx=%"PRIu32"（各要求 >= %d）", urx, utx, MIN_UART_BYTES);

    /* 工位 1 烧的是不是当前版 NCP（2026-10-07，TODO T9.12②）。量产只用直连子设备 64 + NVM3 缓存 400 那版；
       老版是 32 + 缓存 200，而跑过 64 的芯片再刷老版会开机即崩（mg24/README.md「直连子设备 64：根因实测」）。
       上面两项只证明"活着、串口通"，分不出版本 —— 这是唯一的机器门。认直连子设备而不认源路由：
       10-06 白天那版缓存 200 的源路由也是 254。读的是整板上电开机时独占 UART 的上电值（ncp_version.c），
       产线上没有上位机连着，读到的就是编译值。没读到算 fail 不算 manual：manual 能被 ack 消掉。
       C5 单独重启时不读、沿用上次的读数（T9.31 ②）—— 工位 1 改烧过 MG24 却没断电，那份就是旧芯片的，
       所以沿用的读数一律不认，要求断电重上电（产测指南本来就要求写完身份后断电重上电）。 */
    const ncp_version_t *nv = ncp_version_get();
    if (!nv || !nv->cfg.valid) {
        item(rp, "ncp_tables", "fail", "没读到 NCP 表容量（MG24 开机没应答，或开机时 6638 已有上位机连着）");
    } else if (nv->from_cache) {
        item(rp, "ncp_tables", "fail", "本次开机没读 Zigbee 芯片（沿用上次的读数）—— 断电重上电后再测");
    } else {
        item(rp, "ncp_tables", nv->cfg.max_end_device_children == FACTORY_NCP_END_DEVICE_CHILDREN ? "pass" : "fail",
             "直连子设备 %u（要求 %d；不等 = 工位 1 烧的不是当前版 NCP，回工位 1 重烧）",
             (unsigned)nv->cfg.max_end_device_children, FACTORY_NCP_END_DEVICE_CHILDREN);
    }

    /* ---- 网络 ---- */
    network_status_t ns;
    network_mgr_get_status(&ns);
    char eip[16];
    snprintf(eip, sizeof(eip), IPSTR, IP2STR(&ns.eth_ip));
    item(rp, "ethernet", ns.eth_link_up ? "pass" : "fail",
         "link=%d ip=%s（产测必须插网线）", ns.eth_link_up, eip);
    /* 18②（2026-09-13）：link up 不等于通。从以太网口 ping 网关，见 eth_ping_check 头注 */
    char epd[128];
    bool ep_ok = eth_ping_check(&ns, epd, sizeof(epd));
    item(rp, "eth_ping", ep_ok ? "pass" : "fail", "%s", epd);
    item(rp, "wifi", ns.wifi_connected ? "pass" : "manual",
         ns.wifi_connected ? "已连 \"%s\" rssi=%d" : "未连接（若产线不配 WiFi 则跳过）",
         ns.wifi_ssid, ns.wifi_rssi);

    /* ---- WiFi 射频自证（T2.2）----
       `wifi` 那项只看"连上了没"，产线通常**没有** 5GHz AP 可连，于是它恒为
       manual —— 也就是说此前产线根本没有任何一项证明过 WiFi 射频是好的，
       一块 5GHz 射频虚焊的板子能一路走到出货。

       ⚠️ 已连上时**不扫描**：连接本身已经证明了射频，而扫描很贵 ——
          2026-09-11 在本板实测一次 /api/v1/wifi/scan 要 **14.5 秒**（不是想当然的
          1-2 秒；共存仲裁器激活时驱动用的是它自己的驻留参数，见 wifi_mgr.c）。
          webui 是单任务，这 14 秒里整个 Web 界面卡死（同 /ota/check 的道理，
          那个才 6 秒就已经被判定为"只能挂在用户点击上"）；且 802.15.4 与 WiFi
          共存，扫描期间 Thread 会被饿到。在役设备上按一次自检就把网页和 Thread
          一起按住 14 秒，代价方向反了。
       ⚠️ 扫描参数一个都不要填：共存仲裁器激活时驱动会拒绝自定义的 active
          min/max，静默返回 0 个 AP（见 wifi_mgr.c 的 scan_time 那段注释）。
          network_mgr_scan_wifi() 已经按那条教训写好，直接用它，别在这里另起一套。 */
    if (ns.wifi_connected) {
        item(rp, "wifi_scan", "pass",
             "已连上 5GHz AP（ch%d），射频已自证，跳过扫描", ns.wifi_channel);
    } else {
        uint16_t n_ap = SELFTEST_SCAN_MAX;
        esp_err_t serr = network_mgr_scan_wifi(s->aps, &n_ap);
        if (serr != ESP_OK) {
            item(rp, "wifi_scan", "manual",
                 "扫描发不出去：%s（WiFi 未启动？）", esp_err_to_name(serr));
        } else if (n_ap > 0) {
            /* 两个分支各自一个格式串 —— 合成一个的话参数个数和类型要手工对齐，
               改一边就是栈上乱读（同 identity 那一项的理由）。 */
            item(rp, "wifi_scan", "pass", "扫到 %u 个 5GHz AP，最强 \"%s\" rssi=%d",
                 (unsigned)n_ap, (const char *)s->aps[0].ssid, s->aps[0].rssi);
        } else {
            item(rp, "wifi_scan", "manual",
                 "扫到 0 个 5GHz AP —— 产线没有 5GHz 环境时属正常；"
                 "若现场确有 5GHz AP，那就是这块板的 WiFi 射频有问题");
        }
    }

    /* ---- PoE 供电 ----
       固件分不出"这块板没有 PoE 供电"和"分压电路没上件"（两者数字读数相同，
       见 poe.h 的实测表），所以没检测到时只能给 manual，由产线脚本按实际供电
       方式消掉：PoE 供电却读不到 = 不合格，USB 供电 = 跳过。

       ⚠️ 用 poe_is_present() 不用 poe_probe()：probe 会把引脚依次配成上拉/下拉
          各读一次，期间 /status 与 WS 推送读到的是被拉住的电平（poe.h 记的那个
          几十毫秒竞态窗口）。判定本来就只看 no_pull 那一格，没必要为此去抖动
          一个正在被别人读的引脚。 */
    bool poe_on = poe_is_present();
    item(rp, "poe", poe_on ? "pass" : "manual",
         poe_on ? "PoE 供电（IO0 为高）"
                : "未检测到 PoE（IO0 为低）—— USB 供电时属正常；"
                  "若产线按 PoE 供电则此项不合格");

    /* ---- 温度 ---- */
    float t = hs ? hs->temperature : 0.0f;
    item(rp, "temperature", (t > 0.0f && t < 85.0f) ? "pass" : "fail",
         "%.1f °C（要求 0-85）", t);

    /* ---- LED：固件只能证明"写进去了"，亮不亮必须人眼 ---- */
    uint8_t px[8][3];
    int npx = led_ctrl_get_pixels(px, 8);
    item(rp, "led_pixels", npx == led_ctrl_count() ? "pass" : "fail",
         "%d/%d 颗可寻址", npx, led_ctrl_count());
    item(rp, "led_visual", "manual",
         "先 POST /api/v1/led/test {\"index\":-1,\"color\":\"FFFFFF\",\"hold_s\":3}，"
         "四颗必须**全白**——单色看不出丢字节（X5）");

    /* ---- 按键：固件判不了"按下去有没有反应"，只能给出留痕 ---- */
    button_stats_t btn;
    button_get_stats(&btn);
    item(rp, "button", btn.press_count > 0 ? "pass" : "manual",
         btn.press_count > 0
             ? "已按 %"PRIu32" 次，最后一次 %"PRIu32" ms → %s"
             : "尚未按过。请按住约 5 秒后松开（落在 2-10s 保护带，不触发任何动作），"
               "再查本接口：count 应 +1、action 应为 none",
         btn.press_count, btn.last_duration_ms, btn.last_action);

    if (rp->overflow) { ret = send_error(req, 500, "report_overflow"); goto done; }

    const char *verdict = rp->n_fail  ? "fail"
                        : rp->n_manual ? "incomplete"
                                     : "pass";

    int n = snprintf(s->out, sizeof(s->out),
        "{\"verdict\":\"%s\",\"fail\":%d,\"manual\":%d,\"acked\":%d,\"items\":[%s]}",
        verdict, rp->n_fail, rp->n_manual, rp->n_acked, rp->buf);
    if (n < 0 || n >= (int)sizeof(s->out)) {
        ret = send_error(req, 500, "json_overflow"); goto done;
    }

    ESP_LOGI(TAG, "selftest: %s (fail=%d manual=%d acked=%d)",
             verdict, rp->n_fail, rp->n_manual, rp->n_acked);
    ret = send_json_response(req, s->out);

    /* 三条返回路径统一在这里收尾 —— 漏掉任何一条就是每次自检漏 4.6KB。 */
done:
    free(s);
    return ret;
}

/* 把当前 ack 表序列化进 out，形如 {"acked":2,"items":[{"name":"..","result":"pass"}]} */
static esp_err_t send_ack_state(httpd_req_t *req)
{
    char out[256];
    int n = snprintf(out, sizeof(out), "{\"acked\":%d,\"items\":[", s_ack_count);
    for (int i = 0; i < s_ack_count && n > 0 && n < (int)sizeof(out); i++) {
        n += snprintf(out + n, sizeof(out) - n, "%s{\"name\":\"%s\",\"result\":\"%s\"}",
                      i ? "," : "", s_acks[i].name, s_acks[i].pass ? "pass" : "fail");
    }
    if (n < 0 || n >= (int)sizeof(out) - 3) return send_error(req, 500, "json_overflow");
    snprintf(out + n, sizeof(out) - n, "]}");
    return send_json_response(req, out);
}

// ---- GET /api/v1/factory/ack ----
static esp_err_t handle_factory_ack_get(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);
    return send_ack_state(req);
}

// ---- POST /api/v1/factory/ack ----
static esp_err_t handle_factory_ack(httpd_req_t *req)
{
    CROSS_ORIGIN_GUARD(req);
    AUTH_REQUIRED(req);

    char body[192];
    if (req->content_len <= 0 || req->content_len >= (int)sizeof(body))
        return send_error(req, 400, "bad_body_length");

    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) return send_error(req, 400, "recv_failed");
        got += r;
    }
    body[got] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid_json");

    /* {"clear":true} —— 同一块板重测时把上一轮的确认清掉 */
    cJSON *clear = cJSON_GetObjectItem(root, "clear");
    if (clear && cJSON_IsTrue(clear)) {
        memset(s_acks, 0, sizeof(s_acks));
        s_ack_count = 0;
        cJSON_Delete(root);
        ESP_LOGI(TAG, "ack: cleared");
        return send_ack_state(req);
    }

    cJSON *jitem   = cJSON_GetObjectItem(root, "item");
    cJSON *jresult = cJSON_GetObjectItem(root, "result");

    if (!cJSON_IsString(jitem) || !jitem->valuestring[0]) {
        cJSON_Delete(root); return send_error(req, 400, "item_required");
    }
    /* result 不给默认值：产线工具漏填这个字段时必须报错，不能悄悄按"通过"处理。 */
    if (!cJSON_IsString(jresult) ||
        (strcmp(jresult->valuestring, "pass") != 0 &&
         strcmp(jresult->valuestring, "fail") != 0)) {
        cJSON_Delete(root); return send_error(req, 400, "result_must_be_pass_or_fail");
    }
    if (strlen(jitem->valuestring) >= ACK_NAME_LEN) {
        cJSON_Delete(root); return send_error(req, 400, "item_name_too_long");
    }
    /* 白名单外一律 400 —— 写错名字必须响，静默无效等于操作员以为消了实际没消。 */
    if (!ack_is_allowed(jitem->valuestring)) {
        cJSON_Delete(root); return send_error(req, 400, "item_not_ackable");
    }

    bool pass = (strcmp(jresult->valuestring, "pass") == 0);

    /* 同名覆盖：操作员改主意（先按了 y 又发现末端灯不亮）要能改回来。 */
    int slot = -1;
    for (int i = 0; i < s_ack_count; i++)
        if (strcmp(s_acks[i].name, jitem->valuestring) == 0) { slot = i; break; }
    if (slot < 0) {
        if (s_ack_count >= ACK_MAX) {
            cJSON_Delete(root); return send_error(req, 507, "ack_table_full");
        }
        slot = s_ack_count++;
    }
    strlcpy(s_acks[slot].name, jitem->valuestring, ACK_NAME_LEN);
    s_acks[slot].pass = pass;

    ESP_LOGI(TAG, "ack: %s = %s", s_acks[slot].name, pass ? "pass" : "fail");
    cJSON_Delete(root);
    return send_ack_state(req);
}

esp_err_t register_factory_handlers(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/v1/factory/selftest", .method = HTTP_GET,
          .handler = handle_factory_selftest },
        { .uri = "/api/v1/factory/ack",      .method = HTTP_POST,
          .handler = handle_factory_ack },
        { .uri = "/api/v1/factory/ack",      .method = HTTP_GET,
          .handler = handle_factory_ack_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &uris[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
