#include "eth_mgr.h"
#include "esp_log.h"
#include "config_mgr.h"

static const char *TAG = "eth_mgr";

#ifdef CONFIG_BITENGINE_HAS_ETHERNET

#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "devkit_pins.h"
#include "ethernet_init.h"

/* ── DM9051 引脚：把两份真源绑死（2026-08-17）───────────────────────────
 *
 * devkit_pins.h 里那组 ETH_SPI_* 宏写得很像权威映射，注释还逐条标了
 * "FSPICLK → DM9051 SPI_CK"，**但它们 8 个里有 7 个零引用** —— 真正驱动
 * DM9051 的是 managed_components 的 ethernet_init，它读的是 sdkconfig 的
 * CONFIG_ETHERNET_SPI_*。改 devkit_pins.h 不会改变任何行为。
 *
 * 两份值目前恰好一致，但此前没有任何东西把它们绑在一起 —— 这正是 CLAUDE.md
 * 里 RTS/CTS 写反那个教训的同款结构：**错值长期无症状**，直到某天有人照着
 * devkit_pins.h 改引脚却发现板子行为不变，或反过来改了 sdkconfig 而
 * devkit_pins.h 上的注释开始骗人。
 *
 * 下面的静态断言让"只改一边"变成编译错误。要真的换引脚：两边一起改。
 * （不把宏删掉是因为它们是**给人看的**接线表，注释里的网标信息在原理图核对时
 *   有用；CLAUDE.md 也要求 GPIO 走 devkit_pins.h 而不是散落的数字。）
 *
 * ⚠️ 断言消息用英文：GCC 把中文字符串按八进制转义打出来，中文消息在编译错误里
 *    是一串 \37777777747 完全没法读 —— 第一版就是这么写的，白搭。
 */
_Static_assert(ETH_SPI_CLK_PIN   == CONFIG_ETHERNET_SPI_SCLK_GPIO,
               "ETH_SPI_CLK_PIN (devkit_pins.h) != CONFIG_ETHERNET_SPI_SCLK_GPIO (sdkconfig) -- change BOTH");
_Static_assert(ETH_SPI_MOSI_PIN  == CONFIG_ETHERNET_SPI_MOSI_GPIO,
               "ETH_SPI_MOSI_PIN (devkit_pins.h) != CONFIG_ETHERNET_SPI_MOSI_GPIO (sdkconfig) -- change BOTH");
_Static_assert(ETH_SPI_MISO_PIN  == CONFIG_ETHERNET_SPI_MISO_GPIO,
               "ETH_SPI_MISO_PIN (devkit_pins.h) != CONFIG_ETHERNET_SPI_MISO_GPIO (sdkconfig) -- change BOTH");
_Static_assert(ETH_SPI_CS_PIN    == CONFIG_ETHERNET_SPI_CS0_GPIO,
               "ETH_SPI_CS_PIN (devkit_pins.h) != CONFIG_ETHERNET_SPI_CS0_GPIO (sdkconfig) -- change BOTH");
_Static_assert(ETH_SPI_INT_PIN   == CONFIG_ETHERNET_SPI_INT0_GPIO,
               "ETH_SPI_INT_PIN (devkit_pins.h) != CONFIG_ETHERNET_SPI_INT0_GPIO (sdkconfig) -- change BOTH");
_Static_assert(ETH_SPI_RST_PIN   == CONFIG_ETHERNET_SPI_PHY_RST0_GPIO,
               "ETH_SPI_RST_PIN (devkit_pins.h) != CONFIG_ETHERNET_SPI_PHY_RST0_GPIO (sdkconfig) -- change BOTH");
_Static_assert(ETH_SPI_CLOCK_MHZ == CONFIG_ETHERNET_SPI_CLOCK_MHZ,
               "ETH_SPI_CLOCK_MHZ (devkit_pins.h) != CONFIG_ETHERNET_SPI_CLOCK_MHZ (sdkconfig) -- change BOTH");
_Static_assert(ETH_SPI_HOST      == CONFIG_ETHERNET_SPI_HOST,
               "ETH_SPI_HOST (devkit_pins.h) != CONFIG_ETHERNET_SPI_HOST (sdkconfig) -- change BOTH");

static esp_netif_t      *s_eth_netif  = NULL;
static esp_eth_handle_t  s_eth_handle = NULL;
static volatile bool     s_link_up    = false;
static char              s_eth_diag[256] = "not initialized";
char g_spi_diag[128] = "";
volatile uint8_t g_phycfg_voted = 0;
volatile uint32_t g_phycfg_vote_count = 0;

static void eth_link_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *data)
{
    if (event_id == ETHERNET_EVENT_CONNECTED) {
        s_link_up = true;
        ESP_LOGI(TAG, "ETH link up");
    } else if (event_id == ETHERNET_EVENT_DISCONNECTED) {
        s_link_up = false;
        ESP_LOGW(TAG, "ETH link down");
    }
}

const char *eth_mgr_get_diag(void) {
    if (s_eth_handle) {
        // Link state is maintained by ETHERNET_EVENT_CONNECTED/DISCONNECTED
        // (PHY-agnostic via esp_eth), so DM9051 works the same as any SPI PHY.
        snprintf(s_eth_diag, sizeof(s_eth_diag), "link=%d voted=0x%02x polls=%lu",
                 s_link_up, g_phycfg_voted, (unsigned long)g_phycfg_vote_count);
    }
    return s_eth_diag;
}

/*
 * Hold the DM9051 in reset on the way out of esp_restart().
 *
 * ⚠️ Mitigation, not a fix. The real problem is hardware: PHY_INT shares IO28
 * with the ESP32-C5's BOOT strapping pin (pull-up only 4.7K). If the PHY is
 * asserting its open-drain interrupt at the instant the C5 resets, the ROM
 * reads IO28 low and drops into serial download mode — no second-stage
 * bootloader, no application, no console output at all. It looks exactly like
 * a bricked board and only a full power cycle clears it. See
 * `docs/hardware/硬件缺陷档案.md` X6 / A3.
 *
 * Asserting PHY reset first makes the DM9051 release INT, so IO28 rises back
 * to its pull-up before the CPU reset latches the strapping pins.
 *
 * What this does NOT cover — do not treat A3 as optional because of it:
 *   - panic / abort / task WDT: shutdown handlers do not run on those paths
 *   - RS706 hardware watchdog: yanks EN directly, no code executes
 *   - brownout, power glitch, manual reset
 *   - mg24_do_reset()'s IO23 pulse, which resets the whole board through
 *     R113/R58 without running anything afterwards
 * Those are precisely the unattended-fault paths, i.e. the ones where the
 * board most needs to come back on its own.
 */
static void eth_phy_hold_reset_on_reboot(void)
{
    /* Active low — esp_eth_phy_802_3.c:443 asserts with 0, releases with 1.
       Latch the output register before switching direction so the pin cannot
       glitch high on its way to being driven (same rule as mg24_reset_init). */
    gpio_set_level(ETH_SPI_RST_PIN, 0);
    gpio_set_direction(ETH_SPI_RST_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(ETH_SPI_RST_PIN, 0);

    /* Busy-wait, not vTaskDelay: shutdown handlers run while the system is
       being torn down and must not depend on the scheduler. 5 ms is far more
       than the DM9051 needs to drop INT. */
    esp_rom_delay_us(5000);
}

esp_err_t eth_mgr_init(void)
{
    /* Registered before the driver starts: an interrupt can only be pending
       once the PHY is running, but registering here keeps the handler in
       place even if ethernet_init_all() below fails. */
    esp_err_t reg = esp_register_shutdown_handler(eth_phy_hold_reset_on_reboot);
    if (reg != ESP_OK && reg != ESP_ERR_INVALID_STATE) {   /* already registered */
        ESP_LOGW(TAG, "PHY-reset-on-reboot hook not registered (%s) — a restart "
                 "may strand the board in download mode (X6)", esp_err_to_name(reg));
    }

    // Use Espressif's official ethernet_init component (handles SPI bus, DM9051 driver, etc.)
    esp_eth_handle_t *eth_handles = NULL;
    uint8_t eth_cnt = 0;

    esp_err_t err = ethernet_init_all(&eth_handles, &eth_cnt);
    ESP_LOGI(TAG, "ethernet_init_all: err=%s cnt=%d handles=%p", esp_err_to_name(err), eth_cnt, eth_handles);
    if (err != ESP_OK || eth_cnt == 0 || !eth_handles) {
        snprintf(s_eth_diag, sizeof(s_eth_diag), "FAIL: init=%s cnt=%d hdl=%d",
                 esp_err_to_name(err), eth_cnt, eth_handles != NULL);
        ESP_LOGW(TAG, "%s", s_eth_diag);
        return ESP_OK;  // non-fatal
    }

    s_eth_handle = eth_handles[0];

    // Create netif and attach
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(s_eth_handle));
    /* DHCP 主机名（option 12）。不设的话路由器客户端列表里显示 IDF 默认的
       "espressif" —— 家里有几个 ESP 设备就分不清哪个是网关，用户想做
       DHCP 保留都找不到目标。而 DHCP 保留正是"IP 变来变去"的正解。
       必须在 DHCP 启动前设，否则要等下一次续租才生效。 */
    esp_netif_set_hostname(s_eth_netif, config_mgr_get_sys()->hostname);

    // Register link handler
    esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                        eth_link_handler, NULL, NULL);

    // Start Ethernet
    err = esp_eth_start(s_eth_handle);
    if (err != ESP_OK) {
        snprintf(s_eth_diag, sizeof(s_eth_diag), "FAIL: start=%s", esp_err_to_name(err));
        ESP_LOGW(TAG, "%s", s_eth_diag);
        return ESP_OK;  // non-fatal
    }

    eth_dev_info_t info = ethernet_init_get_dev_info(s_eth_handle);
    snprintf(s_eth_diag, sizeof(s_eth_diag), "OK: %s CS=%d INT=%d",
             info.name, info.pin.eth_spi_cs, info.pin.eth_spi_int);
    ESP_LOGI(TAG, "Ethernet: %s", s_eth_diag);
    return ESP_OK;
}

bool eth_mgr_link_is_up(void)       { return s_link_up; }
esp_netif_t *eth_mgr_get_netif(void) { return s_eth_netif; }

#else

esp_err_t eth_mgr_init(void)
{
    ESP_LOGI(TAG, "Ethernet disabled (no hardware)");
    return ESP_OK;
}

bool eth_mgr_link_is_up(void)        { return false; }
esp_netif_t *eth_mgr_get_netif(void) { return NULL; }
const char *eth_mgr_get_diag(void)   { return "disabled"; }

#endif
