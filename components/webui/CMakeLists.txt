# SMLIGHT 兼容层（/api2/*、/ha_info…）是让 Z2M / ZHA 把网关当 SLZB-06 认的，只在 HA 型编（docs/15 设计 2.8 A-04、
# 架构 5.8 R-2；tools/check_profile_rules.py --only bridge 钉着）。依赖照旧无条件声明，只有 SRCS 按配置变。
set(_webui_srcs "webui_server.c" "cross_origin.c" "auth.c" "rest_api.c" "websocket.c"
                "otbr_api.c" "factory_api.c" "static_files.c" "json_escape.c")
if(NOT CONFIG_BITENGINE_ENABLE_ZHUB)
    list(APPEND _webui_srcs "smlight_compat.c")
endif()
if(CONFIG_BITENGINE_ENABLE_MUSE_LINK)
    list(APPEND _webui_srcs "muse_api.c")    # /api/v1/muse/* 与配对剖面的精简 WebUI（docs/15 设计 10.1、3.2）
    list(APPEND _webui_srcs "hub_api.c")     # /api/v1/hub/invoke、/hub/devices、/muse/audit（10.1、MD-9、6.10；M3 第七步）
endif()

idf_component_register(
    SRCS ${_webui_srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}"   # devkit_pins.h 在仓库根
    REQUIRES config_mgr network_mgr watchdog ezsp_bridge spectrum thread_br
             led_ctrl ota_mgr ota_cloud button esp_http_server esp_event esp_app_format
             spi_flash esp_psram
    PRIV_REQUIRES spiffs app_update freertos esp_wifi esp_timer openthread mdns_svc esp_coex
                  hub_core   # 直连型的 /api/v1/diag/hubdb；HA 型里是空组件
                  ble_window muse_pair muse_link   # 直连型 + Muse 才有内容；HA 型里是空组件
                  zhub       # 直连型的 /zigbee/status 的 hub 对象、/zigbee/restart；HA 型里是空组件
                  matter_ctrl   # 直连型的 /api/v1/matter/status（hub_api.c）；HA 型里是空组件
                  log_store poe device_identity nvs_flash utf8
                  mbedtls mqtt_client scheduler espcoredump
)
