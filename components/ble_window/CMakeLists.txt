# ble_window —— 直连型里 BLE 的唯一入口（docs/15_muse_direct_design_v1.md 第三节）。眼下只有策略 C（隔离重启）。
# ⚠️ 只属于直连型 + Muse 开关（BITENGINE_ENABLE_MUSE_LINK）：HA 型里不编任何源文件，只剩头文件目录（架构 5.8 R-3；
#    tools/check_profiles.py 查 libble_window.a 在 HA 型里没有字节）。
# ⚠️ 依赖无条件声明，按配置变的只有 SRCS（同 hub_core/CMakeLists.txt 的说明）。
set(_blew_srcs "")
if(CONFIG_BITENGINE_ENABLE_MUSE_LINK)
    set(_blew_srcs "ble_window.c"
                   "blew_boot.c"
                   "blew_selfcheck.c"   # BLE 窗口之后的 Thread 自检（设计 3.3）
                   "blew_core.c")      # 纯判定，不依赖 IDF（test_host/ 原样编它）
endif()

idf_component_register(
    SRCS ${_blew_srcs}
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES bt esp_timer esp_hw_support esp_system freertos heap log watchdog thread_br hub_core config_mgr
)
