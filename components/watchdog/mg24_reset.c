#include "watchdog.h"
#include "ezsp_bridge.h"

#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"

#ifdef CONFIG_BITENGINE_HAS_MG24_RESET_PIN
#include "driver/gpio.h"
#include "devkit_pins.h"
#endif

static const char *TAG = "mg24_reset";

static uint32_t s_last_reset_time = 0;  // seconds since boot
static uint32_t s_total_resets    = 0;

esp_err_t mg24_reset_init(void)
{
#ifdef CONFIG_BITENGINE_HAS_MG24_RESET_PIN
    /* Boot-step logs with a short flush delay before each GPIO action: on a
       board with a wiring fault (solder bridge into the RS706/EN network) a
       single gpio_set_level can cut EN and kill the chip before the UART
       FIFO drains — these delays guarantee the last-printed step IS the
       killing instruction. Costs ~80ms per boot; keep for field diagnosis. */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << MG24_RESET_PIN) | (1ULL << MG24_NBOOT_PIN),
        .mode         = GPIO_MODE_OUTPUT,
    };
    /* ⚠️ Glitch-free init: latch the output register HIGH *before* switching
       direction. gpio_config(OUTPUT) drives the out-register value at the
       instant the direction flips — and it defaults to 0. A low glitch on
       IO23 (even µs) back-feeds the RS706 RESET output via R113 and yanks
       C5's EN through R58 → instant power-off. This was the root cause of
       board 1 dying at watchdog init on EVERY firmware version. */
    gpio_set_level(MG24_RESET_PIN, 1);
    gpio_set_level(MG24_NBOOT_PIN, 1);
    ESP_LOGI(TAG, "step: gpio_config(nRESET=%d, nBOOT=%d) [out-reg pre-set high]",
             MG24_RESET_PIN, MG24_NBOOT_PIN);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_config(&io);
    /* Drive nBOOT high (bootloader-entry margin) and nRESET high (inactive).
       ⚠️ NEVER pulse IO23 low at boot on RS706-populated boards: the RS706
       RESET output feeds BOTH this net (via R113) and C5's EN (via R58).
       Driving ZIGBEE_RESET low overloads the supervisor's weak push-pull
       output, EN collapses and the C5 kills itself — infinite fast reset
       loop, USB never enumerates (v0.5.0-8 field failure, board 2).
       A boot pulse is also unnecessary: the Gecko bootloader reads nBOOT
       via gpioModeInputPull (pull-up for active-low), so a floating pin
       never traps the MG24 in bootloader mode. */
    ESP_LOGI(TAG, "step: drive nBOOT(%d) high", MG24_NBOOT_PIN);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(MG24_NBOOT_PIN, 1);
    ESP_LOGI(TAG, "step: drive nRESET(%d) high", MG24_RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(MG24_RESET_PIN, 1);
    ESP_LOGI(TAG, "MG24 nRESET pin %d + nBOOT pin %d configured (no boot pulse)",
             MG24_RESET_PIN, MG24_NBOOT_PIN);
#else
    ESP_LOGI(TAG, "No MG24 reset pin — will use ASH RST soft reset");
#endif
    return ESP_OK;
}

bool mg24_reset_takes_board_down(void)
{
    /* Only the GPIO path can drag EN down. The ASH-RST fallback is a message
       on the wire — it cannot touch the supervisor net at all. */
#if defined(CONFIG_BITENGINE_HAS_MG24_RESET_PIN) && \
    !defined(CONFIG_BITENGINE_MG24_RESET_ISOLATED)
    return true;
#else
    return false;
#endif
}

esp_err_t mg24_do_reset(void)
{
    // Check cooldown
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
    if (s_last_reset_time > 0 &&
        (now - s_last_reset_time) < MG24_RESET_COOLDOWN_S) {
        ESP_LOGW(TAG, "MG24 reset cooldown active (%"PRIu32"s remaining)",
                 MG24_RESET_COOLDOWN_S - (now - s_last_reset_time));
        return ESP_ERR_NOT_ALLOWED;
    }

#ifdef CONFIG_BITENGINE_HAS_MG24_RESET_PIN
    /* ⚠️ On AM1 boards this pulse collapses C5's EN through the R113/R58
       network (see mg24_reset_init) — the WHOLE BOARD resets, not just the
       MG24. Acceptable as last-resort recovery for a dead MG24 (both chips
       restart cleanly, network config survives in NVS), but the log below may
       never flush. A1 (R113 → diode + 10K pull-up) removes the back-feed;
       CONFIG_BITENGINE_MG24_RESET_ISOLATED then reflects that. */
    ESP_LOGW(TAG, "MG24 hard reset via GPIO%d (%s)", MG24_RESET_PIN,
             mg24_reset_takes_board_down() ? "AM1 board: FULL-BOARD reset"
                                           : "isolated: MG24 only");
    vTaskDelay(pdMS_TO_TICKS(100));  /* let the warning reach the console */
    gpio_set_level(MG24_RESET_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(MG24_RESET_PULSE_MS));
    gpio_set_level(MG24_RESET_PIN, 1);
#else
    // Soft reset: send ASH RST via bridge health probe (no GPIO available —
    // the probe sends ASH RST and waits for RSTACK). The probe refuses to
    // run while a TCP client holds the ASH session, and can time out: in
    // both cases no reset happened, so report failure instead of silently
    // claiming success (and don't burn the cooldown / counters / event).
    ESP_LOGW(TAG, "MG24 soft reset via ASH RST frame");
    const bridge_stats_t *st = ezsp_bridge_get_stats();
    if (st && st->active_clients > 0) {
        ESP_LOGW(TAG, "MG24 reset refused: %d TCP client(s) hold the ASH "
                 "session — disconnect them first", st->active_clients);
        return ESP_ERR_INVALID_STATE;
    }
    if (!ezsp_bridge_health_probe(MG24_EZSP_QUERY_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "MG24 soft reset failed: no RSTACK received");
        return ESP_FAIL;
    }
#endif

    vTaskDelay(pdMS_TO_TICKS(1000));  // Wait for MG24 to reboot

    s_last_reset_time = now;
    s_total_resets++;

    ESP_LOGW(TAG, "MG24 reset complete (total: %"PRIu32")", s_total_resets);

    esp_event_post(WATCHDOG_EVENT, WATCHDOG_EVENT_MG24_RESET,
                   NULL, 0, 0);

    return ESP_OK;
}

esp_err_t mg24_set_nboot(int level)
{
#ifdef CONFIG_BITENGINE_HAS_MG24_RESET_PIN
    /* Safe to drive in either direction: unlike IO23, nBOOT (IO24) is a
       private line to the MG24 — it does not touch the RS706/EN network, so
       a low level here cannot brown out the C5. */
    esp_err_t err = gpio_set_level(MG24_NBOOT_PIN, level ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nBOOT set %d failed: %s", level, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "nBOOT(IO%d) = %d%s", MG24_NBOOT_PIN, level ? 1 : 0,
             level ? "" : " (bootloader entry armed)");
    return ESP_OK;
#else
    ESP_LOGE(TAG, "nBOOT control unavailable (no MG24 reset/boot pins configured)");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

uint32_t mg24_get_last_reset_time(void) { return s_last_reset_time; }
uint32_t mg24_get_total_resets(void)    { return s_total_resets; }
