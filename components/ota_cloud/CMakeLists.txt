idf_component_register(
    SRCS "ota_cloud.c" "version_cmp.c" "utf8_trunc.c" "ota_profile.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_https_ota
    PRIV_REQUIRES esp_http_client esp-tls mbedtls app_update bootloader_support freertos espressif__cjson ota_mgr ezsp_bridge heap webhook
)
