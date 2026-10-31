cmake_minimum_required(VERSION 3.22)

# ---- 固件签名私钥：没有就生成一把开发钥匙 ----
# 本项目开着 OTA 固件签名验证（sdkconfig.defaults 末尾），而私钥不入库。
# 若不做这一步，任何人 `git clone && idf.py build` 会直接失败：
#   "Secure Boot Signing Key secure_boot_signing_key.pem does not exist"
# 对一个要开源的仓库，这是最容易被当成「开源洗白」截图的失败模式。
#
# 之所以是「自动生成」而不是「关掉签名」：信任锚是**运行中 app 自己的签名块**，
# 不是 eFuse，所以每人一把自己的钥匙天然自洽（自建→自刷→自己的 OTA 也能装），
# 出货固件仍用官方钥匙签，安全性零回退。同构先例：Home Assistant OS。
#
# ⚠️ 脚本对「软链接悬空」会**大声失败且不生成** —— 那意味着这台机器本该有
# 一把真钥匙（卷没挂载/备份没恢复），此时生成会顺着悬空链接把冒牌货写到
# 真钥匙的规范路径上，而构建全程绿灯。理由详见脚本内注释。
execute_process(
    COMMAND bash ${CMAKE_CURRENT_LIST_DIR}/tools/signing_key.sh ensure
    RESULT_VARIABLE _key_rc)
if(NOT _key_rc EQUAL 0)
    message(FATAL_ERROR "签名私钥不可用，见上。")
endif()


# ---- 两种产品配置：工程名（docs/15 设计 2.7、架构 5.8 R-1 / R-9）----
# HA 型 = bitengine（现有产品，什么都不传就是它，产物名、路径一概不变）；直连型 = bitengine-direct。
# 工程名就是镜像 esp_app_desc_t.project_name，OTA 写入前的防串刷比对认的正是它
# （components/ota_cloud/ota_profile.c）。直连型这样编：
#   idf.py -B build-direct -DSDKCONFIG=sdkconfig.direct \
#          -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.direct" -DBITENGINE_PROFILE=direct build
# 或 bash tools/build_profiles.sh。
set(BITENGINE_PROFILE "" CACHE STRING "Product profile: empty or ha = HA (bitengine), direct = Direct (bitengine-direct)")
string(TOLOWER "${BITENGINE_PROFILE}" _bitengine_profile)
if(_bitengine_profile STREQUAL "direct")
    set(_bitengine_project bitengine-direct)
elseif(_bitengine_profile STREQUAL "" OR _bitengine_profile STREQUAL "ha")
    set(_bitengine_project bitengine)
else()
    message(FATAL_ERROR "BITENGINE_PROFILE=${BITENGINE_PROFILE}：只认 ha（默认，可不传）或 direct")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 直连型的托管依赖用自己的锁文件（docs/15 设计 9.2；M7 第一步）：esp_matter 只在直连型里解算
# （components/matter_ctrl/idf_component.yml 的条件），两种配置共用一个 dependencies.lock 的话，每换一种配置构建就把它改写一遍。
# HA 型不设，照旧用 dependencies.lock —— HA 型的解算结果与产物不变。
if(_bitengine_profile STREQUAL "direct")
    idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.direct.lock")
endif()

project(${_bitengine_project})

# 工程名与 sdkconfig 里的总开关必须一致，不一致就停：防止拿 HA 型的 sdkconfig 编出一个叫 bitengine-direct 的
# 固件（防串刷认它是直连型，内容却是 HA 型），或者反过来（附录 E29）。IDF 的 project() 宏链已经在这一层 include 过
# sdkconfig.cmake（build.cmake 的 __build_process_project_includes；IDF 自带的 examples/system/ota/partitions_ota
# 也在 project() 之后直接用 CONFIG_），这里显式再 include 一次，不依赖那个实现细节。
idf_build_get_property(_bitengine_sdkconfig_cmake SDKCONFIG_CMAKE)
include(${_bitengine_sdkconfig_cmake})
set(_bitengine_how_direct "直连型请用：idf.py -B build-direct -DSDKCONFIG=sdkconfig.direct -DSDKCONFIG_DEFAULTS=\"sdkconfig.defaults;sdkconfig.defaults.direct\" -DBITENGINE_PROFILE=direct build（或 bash tools/build_profiles.sh --only direct）")
if(CONFIG_BITENGINE_PROFILE_DIRECT AND NOT _bitengine_profile STREQUAL "direct")
    message(FATAL_ERROR "sdkconfig 选的是直连型（CONFIG_BITENGINE_PROFILE_DIRECT=y），却在按 HA 型（bitengine）构建。"
                        "${_bitengine_how_direct}。HA 型的 sdkconfig 不该选直连型：别在 menuconfig 里切总开关。")
endif()
if(NOT CONFIG_BITENGINE_PROFILE_DIRECT AND _bitengine_profile STREQUAL "direct")
    message(FATAL_ERROR "按直连型（bitengine-direct）构建，sdkconfig 却没选直连型。"
                        "若是误传了 -DBITENGINE_PROFILE=direct：它是缓存变量，会一直留在这个构建目录里，"
                        "用 -DBITENGINE_PROFILE=ha 改回 HA 型。${_bitengine_how_direct}。")
endif()
# 两种配置各用各的构建目录：在 build/ 里编出直连型，会让 build/ 同时躺着两种固件的产物，HA 型那份还是旧的
get_filename_component(_bitengine_bdir "${CMAKE_BINARY_DIR}" NAME)
if(_bitengine_profile STREQUAL "direct" AND _bitengine_bdir STREQUAL "build")
    message(FATAL_ERROR "直连型不要编进 build/（那是 HA 型的）。${_bitengine_how_direct}。")
endif()
if(NOT _bitengine_profile STREQUAL "direct" AND _bitengine_bdir STREQUAL "build-direct")
    message(FATAL_ERROR "build-direct/ 是直连型的构建目录，HA 型请用 build/（idf.py build）。")
endif()

# ---- mdns 多包应答补丁 ----
# 组件把一次应答的全部记录塞进一个 1460 字节缓冲，放不下就**静默丢弃**：
# 不置 TC 位、不报错、ANCOUNT 只数塞进去的那些。实测 187 字节/设备，
# 第 8 个 Matter 设备起浏览应答就不完整了 —— 而 CONFIG_MDNS_MAX_SERVICES 是 32，
# 也就是撞到那个上限之前很久，控制器就已经「查无此人」。
# 之所以是构建期打补丁而不是 fork：managed_components/ 被 .gitignore 挡着
# （.gitignore:3），改在那里不入库、fullclean 即失。理由详见脚本头部。
# ⚠️ 用原文匹配，上游一改就报错停下，不会打歪。
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env python3
            ${CMAKE_CURRENT_LIST_DIR}/tools/patch_mdns.py
            --component-dir ${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__mdns
    RESULT_VARIABLE _mdns_patch_rc
    OUTPUT_VARIABLE _mdns_patch_out
    ERROR_VARIABLE  _mdns_patch_err)
message(STATUS "${_mdns_patch_out}${_mdns_patch_err}")
if(NOT _mdns_patch_rc EQUAL 0)
    message(FATAL_ERROR "mdns 多包补丁失败，见上。绕过它等于放任浏览应答被静默截断。")
endif()

# ---- esp-mqtt：mqtt_task 的栈放 PSRAM（T9.39）----
# 开着 MQTT 时内部 SRAM 少约 10 KB、跌破水位告警，其中 6 KB 是这个任务的栈；组件没有配置项能改。
# 同 mdns：构建期按原文打补丁，上游一改就报错停下。理由与安全前提见 tools/patch_mqtt.py 头注。
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env python3
            ${CMAKE_CURRENT_LIST_DIR}/tools/patch_mqtt.py
            --component-dir ${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__mqtt
    RESULT_VARIABLE _mqtt_patch_rc
    OUTPUT_VARIABLE _mqtt_patch_out
    ERROR_VARIABLE  _mqtt_patch_err)
message(STATUS "${_mqtt_patch_out}${_mqtt_patch_err}")
if(NOT _mqtt_patch_rc EQUAL 0)
    message(FATAL_ERROR "esp-mqtt 补丁失败，见上。绕过它 = mqtt_task 的 6 KB 栈回到内部 SRAM。")
endif()

# ---- esp-matter（直连型的 Matter 控制器，docs/15 设计 9.8a 第 7 条、9.8b 第 11 条；M7 第一步）----
# 两处：CHIP 浏览上下文把 "_S2._sub._matterc" 截成 "_S2._sub"（本机代播的待配网设备永远查不到）、控制器宣告自己会改掉网关的
# mDNS 主机名。同 mdns：构建期按原文打补丁，上游一改就报错停下；理由见 tools/patch_esp_matter.py 头注。HA 型里没有 esp_matter。
if(CONFIG_BITENGINE_ENABLE_MATTER_CTRL)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env python3
                ${CMAKE_CURRENT_LIST_DIR}/tools/patch_esp_matter.py
                --component-dir ${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__esp_matter
        RESULT_VARIABLE _em_patch_rc
        OUTPUT_VARIABLE _em_patch_out
        ERROR_VARIABLE  _em_patch_err)
    message(STATUS "${_em_patch_out}${_em_patch_err}")
    if(NOT _em_patch_rc EQUAL 0)
        message(FATAL_ERROR "esp-matter 补丁失败，见上。绕过它 = 按配网码配网找不到本网关 Thread 上的设备、网关的 mDNS 主机名被改掉。")
    endif()
endif()

# ---- T9.37 服务身份：DHCP option 61 按网卡附 ----
# 服务地址由"某个 DHCP 身份（client-id）"持有而不是"以太网口"，持有者用这个身份做 DHCP、另一张卡用另一个
# （docs/16_service_identity_dual_float_design_v1.md）。option 61 由 components/network_mgr/svc_cid.c 的钩子附，
# 钩子头文件得进 lwip 组件的编译 —— 与 IDF 示例 examples/network/vlan_support 同一机制。头文件在
# CONFIG_BITENGINE_SVC_IDENTITY 关着时是空的，IDF 默认行为不变。
idf_component_get_property(_lwip lwip COMPONENT_LIB)
target_compile_options(${_lwip} PRIVATE "-I${CMAKE_CURRENT_LIST_DIR}/components/network_mgr/lwip_hooks")
target_compile_definitions(${_lwip} PRIVATE "-DESP_IDF_LWIP_HOOK_FILENAME=\"bitengine_lwip_hooks.h\"")

# 每次构建额外产出带版本号的固件副本（如 bitengine-v0.5.0-6-g1ae9451.bin；直连型是 bitengine-direct-v….bin），
# 供 OTA 分发/存档，避免多份 bitengine.bin 无法区分版本。按工程名取，HA 型的产物名不变。
idf_build_get_property(_build_dir BUILD_DIR)
idf_build_get_property(_ver PROJECT_VER)
add_custom_target(versioned_bin ALL
    COMMAND ${CMAKE_COMMAND} -E copy
            ${_build_dir}/${CMAKE_PROJECT_NAME}.bin
            ${_build_dir}/${CMAKE_PROJECT_NAME}-${_ver}.bin
    COMMENT "Versioned firmware: ${CMAKE_PROJECT_NAME}-${_ver}.bin"
    DEPENDS gen_project_binary)

# ---- www 分区（WebUI 前端）SPIFFS 镜像 ----
# 以前 www.bin 要手工跑 spiffsgen，结果烧录包里那份长期停在旧版本、且无从判断
# 对应哪一版前端。现在纳入构建：idf.py build 一并产出 build/www.bin，
# idf.py flash 一并烧到 0x620000。
#
# www_stage 目标（tools/stage_www.py）负责刷新前端并生成 version.json；
# 镜像内容不含时间戳，前端没变时 www.bin 逐字节稳定。
#
# 直连型的前端单独构建（docs/15 设计 2.7、A-29）：frontend/direct/build.sh 由 HA 型的打包派生，产物放 www-direct/；
# HA 型这一段的命令与以前完全相同（_www_profile_args 为空）。
idf_build_get_property(_python PYTHON)
if(CONFIG_BITENGINE_PROFILE_DIRECT)
    set(WWW_SRC_DIR       ${CMAKE_CURRENT_LIST_DIR}/www-direct)
    set(_www_profile_args --profile direct)
else()
    set(WWW_SRC_DIR       ${CMAKE_CURRENT_LIST_DIR}/www)
    set(_www_profile_args "")
endif()
set(WWW_STAGE_DIR ${_build_dir}/www_image)
file(MAKE_DIRECTORY ${WWW_STAGE_DIR})

add_custom_target(www_stage
    COMMAND ${_python} ${CMAKE_CURRENT_LIST_DIR}/tools/stage_www.py
            --project-dir ${CMAKE_CURRENT_LIST_DIR}
            --src         ${WWW_SRC_DIR}
            --stage       ${WWW_STAGE_DIR}
            ${_www_profile_args}
    COMMENT "Staging www SPIFFS content (frontend + version.json)"
    VERBATIM)

spiffs_create_partition_image(www ${WWW_STAGE_DIR} FLASH_IN_PROJECT DEPENDS www_stage)
