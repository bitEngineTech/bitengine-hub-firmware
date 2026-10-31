# components/matter_ctrl —— 直连型的 Matter 控制器（docs/15_muse_direct_design_v1.md 第九节；M7）。
# ⚠️ 只属于直连型：HA 型里不编任何源文件、不拉 esp_matter（架构 5.8 R-3；tools/check_profiles.py 查 libmatter_ctrl.a 与
#    libespressif__esp_matter.a 在 HA 型里没有字节）。esp_matter 由 idf_component.yml 的条件依赖带进来，是组件管理器注入的私有依赖。
# ⚠️ 依赖必须无条件声明（预展开读不到 CONFIG_*，见 hub_core/CMakeLists.txt 头注），所以这里只列两种配置都有的组件；
#    esp_matter 不能写进 PRIV_REQUIRES —— HA 型里没有它，写了 HA 型就配置失败。按配置变的只有 SRCS 与链接片段。
set(_mc_srcs "")
set(_mc_lf "")
if(CONFIG_BITENGINE_ENABLE_MATTER_CTRL)
    set(_mc_srcs "matter_ctrl.cpp"
                 "mc_issuer.cpp"        # 自己的凭据签发器（M7 第二步）
                 "mc_ident_core.c"      # fabric 身份的存储格式，纯逻辑（test_host/ 原样编）
                 "mc_comm.cpp"          # 节点表、在网配网、移除（M7 第三步）
                 "mc_nodes_core.c"      # 节点表格式与分配，纯逻辑（test_host/）
                 "mc_code_core.c"       # 配网码规整、discriminator 比对，纯逻辑（test_host/）
                 "mc_iv.cpp"            # 访谈：Basic Information / Descriptor（M7 第四步）
                 "mc_iv_core.c"         # 访谈结果的存储格式，纯逻辑（test_host/）
                 "mc_sub.cpp"           # 订阅：在线与读数（M7 第五步）
                 "mc_sub_core.c"        # 订阅的纯逻辑：路径、换算、翻转事件、离线判定（test_host/）
                 "mc_ctl.cpp"           # 控制：开 / 关 / 切换 / 亮度（M7 第六步）
                 "mc_paa.cpp"           # 内置 PAA 快照装成设备认证的信任库（T9.53）
                 "paa_snapshot.c")      # PAA 证书本身：tools/fetch_paa.py 生成，别手改
    set(_mc_lf "linker.lf")
endif()

idf_component_register(
    SRCS ${_mc_srcs}
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES nvs_flash esp_timer esp_hw_support freertos log heap watchdog espressif__mdns
    LDFRAGMENTS ${_mc_lf}
)

if(CONFIG_BITENGINE_ENABLE_MATTER_CTRL)
    # 照 #159：CHIP 按 esp-matter 自己的方式 -Os 编（工程的 -O2 下 GCC 在 CHIP 里报 stringop，CHIP 又是 -Werror）
    idf_component_get_property(_em_lib espressif__esp_matter COMPONENT_LIB)
    target_compile_options(${_em_lib} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:-Wno-error=unused-result;-Wno-overloaded-virtual>"
        "-Wno-format-nonliteral;-Wno-format-security" "-Os")
    idf_component_get_property(_em_dir espressif__esp_matter COMPONENT_DIR)
    # esp-matter 1.6.0 的编译缺陷：PeerAddress 没带命名空间（9.8a 第 9 条）
    set_source_files_properties(
        "${_em_dir}/components/esp_matter_controller/commands/esp_matter_controller_pairing_command.cpp"
        TARGET_DIRECTORY ${_em_lib}
        PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/esp_matter_compat.h")
    # CONFIG_ENABLE_ETHERNET_TELEMETRY（CHIP 的以太网支持，平台 DNS-SD 要它）会把以太网的配网驱动也编进来：那个驱动用 ESP32 片内
    # EMAC（esp_eth_mac_new_esp32），C5 没有，编不过；真跑起来还会另建一套以太网驱动、和 DM9051 冲突。它只在
    # CONFIG_ETHERNET_NETWORK_COMMISSIONING_DRIVER（server 端）下被引用，我们不带 server：不编它（9.8a 第 5 条）
    set_source_files_properties(
        "${_em_dir}/connectedhomeip/connectedhomeip/src/platform/ESP32/NetworkCommissioningDriver_Ethernet.cpp"
        TARGET_DIRECTORY ${_em_lib}
        PROPERTIES HEADER_FILE_ONLY ON)
    # esp_matter 的 CMakeLists.txt 在"没有 Wi-Fi 站点 / 热点"时把平台 mDNS 的实现 ESP32DnssdImpl.cpp 排除掉 —— 没想到纯以太网；
    # 而 DnssdImpl.cpp 里调它的条件是 WIFI || ETHERNET（CHIPDevicePlatformConfig.h）。纯以太网 + 平台 mDNS 就缺了实现、链接失败（9.8a 第 4 条）。加回来
    target_sources(${_em_lib} PRIVATE "${_em_dir}/connectedhomeip/connectedhomeip/src/platform/ESP32/ESP32DnssdImpl.cpp")
endif()
