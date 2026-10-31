idf_component_register(
    SRCS "poe.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}"   # devkit_pins.h 在仓库根
    PRIV_REQUIRES esp_driver_gpio freertos
)
