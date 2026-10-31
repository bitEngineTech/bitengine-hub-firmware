# hub_core —— 直连型的设备层与存储（docs/15_muse_direct_design_v1.md 第六节、2.5）。M1 有 hub_store，M3 加命令层。
# ⚠️ 只属于直连型：HA 型里不编任何源文件，只剩一个头文件目录（架构 5.8 R-3；tools/check_profiles.py 查
#    libhub_core.a 在 HA 型里没有字节）。
# ⚠️ 依赖必须无条件声明：IDF 在"预展开"阶段收集组件依赖，那时还读不到 CONFIG_*。依赖写进 if 里，
#    预展开会把它们记成空，直连型编译时就找不到 esp_event.h（2026-10-21 踩过）。按配置变的只能是 SRCS。
set(_hub_core_srcs "")
if(CONFIG_BITENGINE_PROFILE_DIRECT)
    set(_hub_core_srcs "hub_store.c"
                       "hub_store_core.c"      # 纯逻辑，不依赖 IDF（test_host/ 主机侧测试原样编它）
                       # 命令层（6.2–6.5、6.8、6.9；M3 第二步），全是纯逻辑，test_host/ 原样编：
                       "hub_json.c"            # 流式 JSON 写入器
                       "hub_sanitize.c"        # 字符串卫生
                       "hub_model.c"           # 能力位与对外名字
                       "hub_registry.c"        # 命令注册表（文字与附录 A 逐字一致，主机测试比对）
                       "hub_policy.c"          # 动作表、执行时的三道检查、限速
                       "hub_params.c"          # 命令参数按注册表校验（Muse 与本地 REST 共用；M3 第七步从 muse_proto 挪来）
                       "hub_cmds.c"            # 命令处理函数（读网络 / Thread / 内存，依赖 IDF；M3）
                       # 事件管线（6.7；M3 第五步）：
                       "hub_evlog.c"           # 最近事件环，纯逻辑，test_host/ 原样编
                       "hub_events.c"          # 事件入口 + 推送队列 + 网关自己的事件源（看门狗、Thread 起落），依赖 IDF
                       # Thread 网络层清单（第七节；M3 第六步）。设计写在 thread_br 里，放这里是为了不动 HA 型共用的 thread_br：
                       "hub_ti_core.c"         # 关联 / Matter 识别 / id / 增减 / 列表项 JSON，纯逻辑，test_host/ 原样编
                       "thread_inventory.c"    # 持 OT 锁拷 SRP / 子节点 / 路由器 / EID 缓存，依赖 IDF
                       # 审计（6.10；M3 第七步）：
                       "hub_audit_core.c"      # 审计环，纯逻辑，test_host/ 原样编
                       "hub_audit.c"           # 入口：加锁、取 id / action、被拒的写 log_store
                       # 常驻服务（第十七节"M4 之前"）：上面三样的初始化与 10 s / 60 s 轮询，不再挂在 Muse 会话任务上：
                       "hub_svc.c"
                       # Zigbee 设备（M4 第七步 a）：给 zhub 登记存取 / 事件钩子、写列表项（只在开了 zhub 时有内容）
                       "hub_zigbee.c"
                       "hub_zcap.c"          # Zigbee 设备的能力与设备类（8.8；M5 第一步），纯逻辑，test_host/hub_zcap_test
                       # 设备元数据（6.1：名字 / 房间 / 推送开关；M5 第二步）：
                       "hub_meta_core.c"     # 表与分页序列化，纯逻辑，test_host/hub_meta_test
                       "hub_meta.c"          # 加锁、开机读回、改完写回 hubdb
                       # Matter 控制器（M7 第二步起）：给 matter_ctrl 登记 fabric 身份 / 节点表的存取钩子、配网与移除的作业（只在开了 matter_ctrl 时有内容）
                       "hub_matter.c"
                       "hub_mcap.c"          # Matter 设备的能力与设备类（9.6；M7 第四步），纯逻辑，test_host/hub_mcap_test
                       # 作业表（6.6；M7 第三步）：耗时命令的结论晚到
                       "hub_jobs_core.c"     # 纯逻辑，test_host/hub_jobs_test
                       "hub_jobs.c")         # 加锁、过期、完成通知
endif()

idf_component_register(
    SRCS ${_hub_core_srcs}
    INCLUDE_DIRS "include"
    REQUIRES esp_common espressif__cjson        # hub_call.h 的接口里有 cJSON
    PRIV_REQUIRES nvs_flash esp_event esp_timer freertos heap log config_mgr watchdog zhub matter_ctrl
                  network_mgr thread_br openthread esp_app_format   # hub_cmds.c
)
