# muse_link —— Muse 的身份、凭据、VM API、会话、注册、调用、推送（docs/15_muse_direct_design_v1.md 第四节、附录 C）。
# M2：vm_api（原样取自上游 muse-gadget-sdk，提交与哈希见 UPSTREAM.md）+ muse_cfg（hubdb 里的设置与凭据）；
# M3：muse_proto（控制面报文）、muse_link_core（会话状态机）、muse_frame（分帧）、muse_session（会话）、muse_link（会话任务）、
#     muse_push_core（推送队列与合并）。
# ⚠️ 只属于直连型（BITENGINE_ENABLE_MUSE_LINK）：HA 型里不编任何源文件（架构 5.8 R-3）。
# ⚠️ 依赖无条件声明，按配置变的只有 SRCS（同 hub_core/CMakeLists.txt 的说明）。
set(_muse_link_srcs "")
if(CONFIG_BITENGINE_ENABLE_MUSE_LINK)
    set(_muse_link_srcs "vm_api.c"          # 上游原样
                        "muse_cfg.c"        # hubdb 命名空间 muse 的读写
                        "muse_cfg_core.c"   # 纯判据，不依赖 IDF（test_host/ 原样编它）
                        "muse_proto.c"      # 控制面报文编解码（M3），纯 C + cJSON（test_host/ 原样编它）
                        "muse_link_core.c"  # 会话状态机与退避（M3），纯 C（test_host/ 原样编它）
                        "muse_frame.c"      # WebSocket / 控制流分帧（M3），纯 C（test_host/ 原样编它）
                        "muse_push_core.c"  # 推送队列、去重、配额、同类合并（M3 第五步），纯 C（test_host/ 原样编它）
                        "muse_session.cpp"  # 一次会话：TLS + WS 升级 + Noise + /link-control（M3，改写自上游 noise_control.cpp）
                        "muse_link.c"       # 会话任务：驱动状态机、取 VM、刷新令牌、注册、分派调用（M3）
                        "muse_hold.c")      # 安全模式退出时"先关掉 Muse"的一次性留言（附录 E33）
endif()

idf_component_register(
    SRCS ${_muse_link_srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_client mbedtls espressif__cjson freertos log
                  hub_core device_identity utf8 esp_hw_support
                  esp-tls esp_event esp_timer esp_app_format lwip network_mgr watchdog noise_core   # M3 会话
                  ble_window                                                                    # M3 推送：BLE 窗口后的 Thread 自检结果
                  zhub                                                                          # M5 第三步：注册的后端就绪位（zhub 在跑才注册 hub.device.update）
                  matter_ctrl                                                                   # M7 第三步：后端就绪位（matter_ctrl 在跑才注册 hub.matter.commission）
                  nvs_flash                                                                     # muse_hold：主 nvs 里的留言（E33）
)

if(CONFIG_BITENGINE_ENABLE_MUSE_LINK)
    # 上游 vm_api.c 以 INFO 打 SDK token 前 12 个字符（"refresh carries SDK token %.12s"）：编译期压到 WARN，
    # 理由同 muse_pair/CMakeLists.txt 的 link_pairing.c（UPSTREAM.md "调用方义务"）。
    set_source_files_properties(vm_api.c PROPERTIES COMPILE_DEFINITIONS "LOG_LOCAL_LEVEL=ESP_LOG_WARN")
endif()
