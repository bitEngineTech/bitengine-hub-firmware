#include "config_mgr.h"
#include "esp_log.h"

static const char *TAG = "config_migrate";

// Migration function prototypes (add as versions increase):
// static void migrate_v1_to_v2(void);
// static void migrate_v2_to_v3(void);

void config_run_migrations(uint8_t stored_version)
{
    ESP_LOGI(TAG, "Running migrations from v%d to v%d",
             stored_version, CONFIG_VERSION_CURRENT);

    for (uint8_t v = stored_version; v < CONFIG_VERSION_CURRENT; v++) {
        switch (v) {
        // case 1: migrate_v1_to_v2(); break;
        // case 2: migrate_v2_to_v3(); break;
        default:
            ESP_LOGW(TAG, "No migration handler for v%d -> v%d", v, v + 1);
            break;
        }
    }

    ESP_LOGI(TAG, "Migrations complete");
}
