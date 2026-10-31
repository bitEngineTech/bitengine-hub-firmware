idf_component_register(
    SRCS "scheduler.c"
    INCLUDE_DIRS "include"
    # watchdog 反向依赖会成环，只取头文件路径，符号最终链接时解析
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/components/watchdog/include"
                      "${CMAKE_SOURCE_DIR}"   # psram_task.h
    REQUIRES config_mgr led_ctrl webhook spectrum
    PRIV_REQUIRES freertos esp_timer
)
