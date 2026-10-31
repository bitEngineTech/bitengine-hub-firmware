idf_component_register(
    SRCS "main.c" "psram_selftest.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}"
    PRIV_REQUIRES nvs_flash esp_event esp_timer app_update esp_driver_gpio esp_mm device_identity
                  watchdog config_mgr network_mgr mdns_svc ezsp_bridge webui
                  spectrum thread_br
                  mqtt_client webhook ble_proxy matter_bridge
                  led_ctrl button scheduler log_store poe
                  hub_core      # 直连型才有内容；HA 型里是空组件（docs/15 设计 2.7、架构 5.8 R-3）
                  ble_window muse_pair muse_link   # 同上，只在 BITENGINE_ENABLE_MUSE_LINK 下有内容
                  zhub          # 同上，只在 BITENGINE_ENABLE_ZHUB 下有内容
                  matter_ctrl   # 同上，只在 BITENGINE_ENABLE_MATTER_CTRL 下有内容（esp_matter 只在直连型里拉）
                  idf_fixes     # 本地回合的 IDF 上游修复（链接期 wrap，两种配置都要；T9.47）
)
