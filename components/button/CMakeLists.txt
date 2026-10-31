idf_component_register(
    SRCS "button.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}"
    REQUIRES led_ctrl ezsp_bridge config_mgr watchdog esp_event
    PRIV_REQUIRES freertos esp_driver_gpio
)
