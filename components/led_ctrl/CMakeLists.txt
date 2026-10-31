idf_component_register(
    SRCS "led_ctrl.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}"  # psram_task.h 在仓库根
    REQUIRES config_mgr network_mgr watchdog esp_event ezsp_bridge
    PRIV_REQUIRES freertos esp_driver_gpio espressif__led_strip
)
