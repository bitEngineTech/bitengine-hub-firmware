idf_component_register(
    SRCS "config_mgr.c"
         "config_defaults.c"
         "config_export.c"
         "config_migrate.c"
         "radio_region.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event nvs_flash esp_netif
    PRIV_REQUIRES freertos mbedtls esp_system esp_timer utf8
)
