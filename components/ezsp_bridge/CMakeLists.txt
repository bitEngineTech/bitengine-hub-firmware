# 直连型（CONFIG_BITENGINE_ENABLE_ZHUB）不带桥：不编 :6638 的 TCP 服务、它的发送环形缓冲、经 USB 的桥
# （docs/15 架构 5.8 R-2；tools/check_profile_rules.py --only bridge、check_profiles.py 的 R-2 钉着）。
# ⚠️ 依赖照旧无条件声明：IDF 预展开时读不到 CONFIG_*，按配置变的只能是 SRCS（hub_core/CMakeLists.txt 同理）。
set(_ezsp_bridge_srcs "ezsp_bridge.c" "uart_bridge.c" "bridge_stats.c" "ncp_version.c" "ncp_cache.c")
if(NOT CONFIG_BITENGINE_ENABLE_ZHUB)
    list(APPEND _ezsp_bridge_srcs "tcp_server.c" "usb_bridge.c" "client_buffer.c")
endif()

idf_component_register(
    SRCS ${_ezsp_bridge_srcs}
    INCLUDE_DIRS "include" "${CMAKE_SOURCE_DIR}"
    # watchdog 的头文件直接给路径而不是 REQUIRES：watchdog 已经 REQUIRES
    # ezsp_bridge（MG24 探针要读桥的统计），反向加依赖会形成 CMake 环。
    # 符号在最终链接时解析（main 链入 watchdog），这里只缺头文件路径。
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/components/watchdog/include"
    REQUIRES config_mgr
    # led_ctrl：T4.4 起 UART 收发路径会置 Zigbee 灯的"有流量"位（节流 100ms）。
    # 放 PRIV_REQUIRES 而不是 REQUIRES：只有 .c 用它，头文件不暴露给下游。
    PRIV_REQUIRES esp_driver_uart esp_driver_usb_serial_jtag led_ctrl
                  esp_event esp_netif esp_timer lwip freertos mdns_svc nvs_flash
)
