idf_component_register(
    SRCS "wd_feed.c"
         "health_monitor.c"
         "mg24_reset.c"
         "wd_registry.c"
         "wd_resource.c"
         "wd_probes.c"
         "wd_recovery.c"
         "wd_forensics.c"
         "wd_fault_inject.c"
         "wd_heaptrace.c"
         "wd_boot_reason.c"
    INCLUDE_DIRS "include"
    # zhub 的头文件给路径而不是 REQUIRES（zhub 已经 REQUIRES watchdog，反向会成环）：wd_probes.c 的弱符号要与 zhub.h 的声明对上类型，
    # 符号在最终链接时由 zhub.c 的强定义覆盖（直连型）；#include 本身在 #if CONFIG_BITENGINE_ENABLE_ZHUB 里，HA 型不读这个头
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/components/zhub/include"
    REQUIRES ezsp_bridge esp_event
    PRIV_REQUIRES esp_timer esp_driver_gpio esp_driver_tsens network_mgr config_mgr
                  freertos esp_system esp_hw_support esp_wifi nvs_flash
                  esp_eth spi_flash   # wd_heaptrace.c（诊断固件专用）：ETH_EVENT、spi_flash_cache_enabled
)

if(CONFIG_BITENGINE_DIAG_HEAP_TRACE)
    # 诊断固件专用：realloc 同堆搬家时给旧块补记释放，见 wd_heaptrace.c 的 __wrap_heap_caps_realloc_base
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=heap_caps_realloc_base")
endif()
