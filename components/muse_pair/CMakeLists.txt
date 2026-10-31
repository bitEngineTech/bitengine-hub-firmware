# muse_pair —— Muse 社区配对 v5（docs/15_muse_direct_design_v1.md 第五节、附录 C）。
# link_pairing / pairing_transcript / pairing_signer_policy 原样取自上游 muse-gadget-sdk，提交与哈希见
# components/muse_link/UPSTREAM.md；pair_ble.c 改写自上游 ble_server.c；pair_core.c / muse_pair.c 本仓库自写。
# ⚠️ 只属于直连型（BITENGINE_ENABLE_MUSE_LINK）：HA 型里不编任何源文件（架构 5.8 R-3）。
# ⚠️ 依赖无条件声明，按配置变的只有 SRCS（同 hub_core/CMakeLists.txt 的说明）。
set(_muse_pair_srcs "")
if(CONFIG_BITENGINE_ENABLE_MUSE_LINK)
    set(_muse_pair_srcs "link_pairing.c" "pairing_transcript.c" "pairing_signer_policy.c"   # 上游原样
                        "pair_core.c"      # 纯判据，不依赖 IDF（test_host/ 原样编它）
                        "pair_ble.c"       # GATT 外设，改写自上游 ble_server.c
                        "muse_pair.c")     # 配对剖面里的状态机
endif()

idf_component_register(
    SRCS ${_muse_pair_srcs}
    INCLUDE_DIRS "include"
    REQUIRES espressif__cjson                       # link_pairing.h 的接口里有 cJSON
    PRIV_REQUIRES mbedtls efuse esp_timer esp_hw_support freertos log
                  bt lwip esp_event esp_app_format ble_window button config_mgr hub_core led_ctrl muse_link
                  network_mgr watchdog
)

if(CONFIG_BITENGINE_ENABLE_MUSE_LINK)
    # 上游 Kconfig 的 HOMEHUB_PAIRING_AUTH_EPOCH（range 1 1）。它只对厂商 eFuse 认证有意义，我们只用社区模式
    # （附录 C：不启用 eFuse 认证，所以也不定义 CONFIG_HOMEHUB_PAIRING_EFUSE_AUTH），这里给上游源码一个定值，
    # 不做成菜单项。
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_HOMEHUB_PAIRING_AUTH_EPOCH=1)
    # 上游 link_pairing.c 以 INFO 打 SDK token 前 12 个字符（"pairing_confirmed carries SDK token %.12s"）。
    # 本项目日志进 log_store、网页可看可导出，所以编译期把这个文件压到 WARN：INFO 分支整段编译不进去，
    # 源码不动（仍算原样引入），也不怕运行时 esp_log_level_set("*", …) 把按 TAG 设的级别清掉。
    # 调用方义务与理由见 components/muse_link/UPSTREAM.md；VERIFY.sh 查编译命令和目标文件里的字符串。
    set_source_files_properties(link_pairing.c PROPERTIES COMPILE_DEFINITIONS "LOG_LOCAL_LEVEL=ESP_LOG_WARN")
endif()
