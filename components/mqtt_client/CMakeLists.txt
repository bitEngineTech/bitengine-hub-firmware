idf_component_register(
    SRCS "bitengine_mqtt.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}"  # psram_task.h 在仓库根
    REQUIRES config_mgr watchdog network_mgr esp_event led_ctrl
    PRIV_REQUIRES freertos app_update mbedtls
)
