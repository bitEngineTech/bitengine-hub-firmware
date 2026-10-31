idf_component_register(
    SRCS "log_store.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer
    PRIV_REQUIRES freertos esp_psram
)
