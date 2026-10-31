# 纯以太网剖面（docs/15 设计 2.2，只在直连型里能开）：不编 wifi_mgr.c / wifi_probe.c，换成"没有 Wi-Fi"的
# 空实现；其余直接调 esp_wifi_* 的地方都在 #if !CONFIG_BITENGINE_NET_ETH_ONLY 里，Wi-Fi 库因此一个字节都不链接
# （tools/check_profiles.py 按库字节数查）。HA 型走 else 分支，与以前完全一样。
if(CONFIG_BITENGINE_NET_ETH_ONLY)
    set(_bitengine_wifi_srcs "wifi_none.c")
else()
    set(_bitengine_wifi_srcs "wifi_mgr.c" "wifi_probe.c")
endif()

idf_component_register(
    SRCS "network_mgr.c"
         ${_bitengine_wifi_srcs}
         "eth_mgr.c"
         "net_events.c"
         "svc_policy.c"       # T9.20 开机接管 / T9.37 服务身份的纯判据（不依赖 IDF，test_host/ 主机侧测试）
         "svc_cid.c"          # T9.37 每张卡的 DHCP client-id + lwIP 钩子（钩子头文件见 lwip_hooks/，顶层 CMake 注入）
    INCLUDE_DIRS "include"
    # devkit_pins.h 在仓库根：PHY 复位脚（IO8）用于重启前释放 PHY_INT
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}"
                      # watchdog 反向依赖会成环，只取头文件路径，符号最终链接时解析
                      "${CMAKE_SOURCE_DIR}/components/watchdog/include"
    REQUIRES esp_wifi esp_event esp_netif config_mgr
    PRIV_REQUIRES lwip esp_timer nvs_flash freertos esp_eth driver esp_driver_gpio espressif__w5500 espressif__ethernet_init ezsp_bridge mdns
)
