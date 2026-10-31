# zhub —— 直连型的 Zigbee 主机（docs/15_muse_direct_design_v1.md 第八节；M4 起）。
# ⚠️ 只属于直连型（CONFIG_BITENGINE_ENABLE_ZHUB 跟总开关）：HA 型里不编任何源文件，只剩头文件目录（架构 5.8 R-3；
#    tools/check_profiles.py 查 libzhub.a 在 HA 型里没有字节）。依赖要无条件写（IDF 预展开阶段读不到 CONFIG_*），按配置变的只能是 SRCS。
set(_zhub_srcs "")
if(CONFIG_BITENGINE_ENABLE_ZHUB)
    set(_zhub_srcs "zh_ash_core.c"      # ASH 链路层主机侧，纯逻辑（8.3；M4 第一步）。test_host/ 原样编它并与 SDK 互通
                   "zh_ezsp_core.c"     # EZSP v18 帧的编码与解析，纯逻辑（8.4；M4 第二步）。test_host/ 拿 SDK 的 ezsp.c 当预言机
                   "zh_ezsp_ses.c"      # EZSP 会话层：握手、单在途、超时与重发、保活，纯逻辑（8.4、8.13；M4 第三步）
                   "zh_host_core.c"     # 编排层：复位 / 握手 / networkInit / 退避 / 挂起，纯逻辑（8.2、8.5、8.13；M4 第四步）
                   "zh_dev.c"           # 设备层：设备表、入网回调、被动学习，纯逻辑（8.6、8.9、8.12 ③；M4 第七步 a）
                   "zh_zcl.c"           # ZCL 最小编解码（读属性），纯逻辑（8.7；M4 第七步 b 访谈）。test_host/ 原样编它，类型表对着 SDK 的 types.xml 核
                   "zh_tbl.c"           # 社区设备库导出的表共用的字段 / 换算（M5 第十步；涂鸦与小米共用）
                   "zh_tuya.c"          # 涂鸦私有簇 0xEF00 的解码与换算，纯逻辑（8.8 注记"社区设备库"；M5 第八步）
                   "zh_tuya_db.c"       # 涂鸦数据点表：tools/zhc_export/regen.sh 从 zigbee-herdsman-converters（MIT）生成，别手改
                   "zh_lumi.c"          # 小米 / Aqara 的 TLV 与表的换算，纯逻辑（M5 第十步）
                   "zh_lumi_db.c"       # 小米表：同上，regen.sh 生成，别手改
                   "zhub.c")            # 任务外壳：单任务（Z60）、UART 认领与让出、看门狗、状态快照（8.1、8.2）
endif()

idf_component_register(
    SRCS ${_zhub_srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../ezsp_bridge"   # uart_bridge.h（桥的 UART 读写，同 ota_mgr）
    REQUIRES ezsp_bridge
    PRIV_REQUIRES watchdog freertos esp_timer esp_driver_uart led_ctrl   # led_ctrl：直连型 ZIGBEE 灯（CHECKLIST Z54）
)
