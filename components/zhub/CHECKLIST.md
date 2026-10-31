# zhub 交叉需求清单（CLAUDE.md P2，2026-10-22）

> 这是**索引**，不是正本：每一行只写"要做什么 + 正本在哪一节"，具体数值一律看出处（CLAUDE.md 7.6：抄过来的数值迟早和正本对不上）。
> 引用设计文档用**节号 / 表名**不用行号（行号一改就错位）。做完的在"状态"列标上提交号；进度的正本仍是设计第十七节的 M4 / M5 / M6 行。
> 由子代理扫 `docs/00`、`docs/01`、`docs/04`、`docs/15` 两份、CLAUDE.md 与现有代码生成；全项目的交叉清单是
> `docs/15_muse_direct_cross_requirements.md`（A-xx 指它）。缩写：设计 = `docs/15_muse_direct_design_v1.md`，架构 = `docs/15_muse_direct_architecture_v1.md`。

## 需求

| # | 要做什么 | 正本 | 层 | 里程碑 | 状态 |
|---|---|---|---|---|---|
| Z01 | ASH 帧的收发规则（分帧、取消、替换字节、CRC、去随机化、ACK/NAK、重复帧补 ACK） | 设计 8.3 | ASH | M4 | ✅ M4 第一步（zh_ash_core） |
| Z02 | 发送窗口、ACK 定时器自适应、连续超时判链路断 → 复位 → 重新初始化 | 设计 8.3、8.13 | ASH | M4 | ✅ ASH 部分（参数见 zh_ash_core.h）；重新初始化 ✅ M4 第四步（zh_host_core：失败一律收摊 → 退避 → ASH RST → 握手 → networkInit） |
| Z03 | 启动 CAN+RST 等 RSTACK；不请自来的 RSTACK → 事件 + 重新初始化 | 设计 8.3 | ASH | M4 | ✅ ASH 部分（ZH_ASH_EV_NCP_RESET）；重新初始化 ✅ 第四步（zh_host_core，host_interop H5）；HUB_EVENT_NET 事件出口随 Z41 |
| Z04 | NCP 的 ERROR 帧怎么处置（设计只列了常量） | 设计 8.3 | ASH | M4 | ✅ ASH 部分（ZH_ASH_EV_NCP_ERROR）；上层复位 ✅ 第四步（zh_host_core；SDK 的 NCP 在看门狗 / 断言复位后报的就是 ERROR 而不是 RSTACK，ash-ncp.c:266-281，host_interop H5 两路都测） |
| Z05 | 速率与硬件流控两侧成对；UART 写可能写不全，要计数（tx_blocked 的实时信号） | CLAUDE.md 硬件环境；`uart_bridge.c` 的写函数 | ASH | M4 | ✅ 第四步（zhub.c）：速率与流控沿用 uart_bridge_init（成对的前提不变）；写不全计 uart_tx_short（快照）与 bridge 的 uart_tx_timeouts，写有超时、流控卡死不挂任务 |
| Z06 | UART 归属仲裁 + zhub_suspend / resume | 设计 8.1；架构 5.8 R-5 | ASH | M4 | ✅ 第四步：zhub 运行时桥是 BRIDGE_STATE_ZHUB（不借 OTA 状态：exit_bootloader 拿 state == OTA 判"已经是我的"）；ezsp_bridge_ota_claim 在直连型里先经钩子调 zhub_suspend（同步、计数、超时自己撤回），ota_release 后 zhub_resume —— 调用方一行不改。suspend = zh_host_stop（会话收摊：交出去过的回 UNKNOWN、没交出去的回 LINK_DOWN；ASH 静音）；resume = 重新认领 → zh_host_start（ASH RST、等 CONNECTED 才 link_up，link_up 只在 tick 里，VERIFY 钉住）。⏳ "排空"（先停 submit、等在途了结）随上层有命令时再看要不要 |
| Z07 | 仲裁漏掉的 UART 占用者：`/zigbee/bootloader`、`/radio/region` 走的 ncp_resync、mg24_do_reset 的软复位分支、wd_probes 的恢复与确认探测 | `rest_api.c`、`mg24_reset.c`、`wd_probes.c` | ASH | M4 | ✅ 第四步：四个都经 ezsp_bridge_ota_claim，一并被仲裁 —— /zigbee/bootloader（ota_mg24_enter_bootloader）、/radio/region 的 ncp_resync（直连型里现在会 zhub 让出 → 读版本发地区 → zhub 重新接管、networkInit，Zigbee 断几秒；正式做法是 Z11 的在线下发）、mg24_do_reset 直连型禁用（已有判据）、wd_probes 直连型不探测（probe_mg24_zhub）；health_probe 在直连型一律拒发（K5） |
| Z08 | EZSP 版本握手、首帧旧式头、状态码宽度、单在途按 seq 匹配、回调派发、命令超时 | 设计 8.4 | EZSP | M4 | ◐ 第二步：首帧旧式头、4 B 状态、头的分类、回调解码。第三步（zh_ezsp_ses）：握手（协议号不对停住报 mismatch）、单在途、按 seq 配、回调派发与 callback 轮询、超时（C15 两个方向）、旧式头只在握手时收、VERSION_NOT_SET → 复位。剩下随 zh_main：getValue(VERSION_INFO) 回填缓存、mismatch 进状态、各计数接出去；第四步：握手的 stackType / 版本进 /zigbee/status 的 hub 对象；getValue(VERSION_INFO) 回填缓存仍 ⏳（开机读 NCP 版本照旧在整板上电时做）；⚠️ 与 8.4 的偏离（第四步，第一轮审计提醒登记）：协议号不对没有"停住"，按普通失败走退避表重试（60 s 封顶）—— 可能是 NCP 正在被升级 / 回包的是 bootloader，自己会好；快照 last_fail=ezsp_version、last_fail_code=NCP 回的协议号，就是 8.4 说的 ncp_ezsp_mismatch |
| Z09 | 帧 ID 常量带 `ezsp-enum.h` 出处；编码金标由脚本从 `command-functions.h` 生成 | 设计 8.4、14.1、14.2 | EZSP | M4 | ✅ M4 第二步：常量带行号（VERIFY）；金标改为直接拿 SDK 的 `ezsp.c`（含 `command-functions.h`）当预言机逐字节对照，比脚本生成更强 |
| Z10 | 回填 ncp_version 缓存要补齐 XNCP build_string 与 txp；别用写入后的值盖掉上电读的表容量 | 设计 8.4、8.5；`ota_cloud.c` 的 NCP 比对；`factory_api.c` 的 ncp_tables | EZSP | M4 | ⏳；EZSP 审计补：内容级解析还没有——`VERSION_INFO` 严格 7 B、stack_version 半字节拆分、表容量的 8 个配置 ID（带 ezsp-enum.h 出处）、`ncp_ezsp_mismatch`；严格长度的思路延续到值的内容 |
| Z11 | 地区在线下发（XNCP + setRadioPower），不走 ncp_resync；功率不加开关 | 设计 8.1、8.5；CLAUDE.md 红线 13 | EZSP | M4 | ⏳；XNCP 信封与 11 B 状态的编解码直接对 `mg24/ncp/test_host/vectors.txt` 的黄金字节测（HA 侧与 zhub 共用一份判据，7.6 规矩 3）；⚠️ 第四步之后直连型改地区会走 ncp_resync：zhub 让出 UART → ASH RST 读版本、发地区 → zhub 重新接管、networkInit（Zigbee 断几秒、不再是"下次开机生效"）。在线下发不复位仍是正式做法 |
| Z12 | 网络启动序列、策略表、端点表 | 设计 8.5（表 8.5-2、8.5-3） | 网络启动 | M4 | ✅ 第五步（2026-10-22）：networkInit 之前的初始化序列（两个协议参数 → 表 8.5-2 的 7 条策略 → 端点 1；任何一步被拒按 NET_INIT 复位，code 带步号）与 UP 之后的序列（读参数 → EUI64 → 集中器 → MTORR，尽力而为；掉网停、回来重走），见设计 8.5 第五步注记；正本 zh_host_core.h（初始化序列是 .c 的 k_init 表，UP 之后的步骤是 zh_host_up_step_t + submit_want / up_result）。networkInit 每次 NCP 启动只发一次、NOT_JOINED 绝不自动建网照旧。setRadioPower ⏳ 随 Z11；表 8.5-1 其余各行 ⏳ 随 Z13 |
| Z13 | 表容量写入值 = NCP 编译值，开机读容量核对（A-06） | 设计 8.5（表 8.5-1） | 网络启动 | M4 | ◐ 第七步 b：表 8.5-1 里的 `INDIRECT_TRANSMISSION_TIMEOUT` 已写（7680，k_init；NCP 默认只有 3000）。⏳ 表容量类与开机核对 |
| Z14 | NCP 拒改的项不写或容忍失败；落 NVM3 的表绝不写成非编译值（红线 4） | `mg24/README.md`「NCP 表容量」 | 网络启动 | M4 | ◐ 第五步：仍然一个表容量类配置值都不写（k_init 只有两个协议参数，VERIFY 钉表内容）；Z13 动手时再按 C8 处理 NCP 拒改的项 |
| Z15 | 建网：能量扫描与信道偏好、排除 Thread 信道、PAN / 扩展 PAN / 网络密钥随机、预配置密钥 | 设计 8.5.1 | 网络启动 | M4 | ⏳ |
| Z16 | 建网尊重 `zigbee.channel`（定为信道偏好） | 设计 2.8 表 | 网络启动 | M4 | ⏳（见 C9） |
| Z17 | 网络密钥不进日志、REST、Muse、导出 | 设计 8.5、第十三节；架构 N6 | 网络启动 | M4 | ◐ 拷贝与负责人：① 会话层 cur / pend —— ✅ 第三步，了结即整块清零、不拷到栈上（真值表 key_in_session 钉住）；② zh_ash_core 的发送队列 q[].data，以及接收缓冲 rx_raw（原地去随机化后交帧：setInitialSecurityState 的回显会把明文密钥留在里面）—— ⏳ zh_ash 那一步（确认后清掉队列那一项、交帧后清 rx_raw）；③ zh_main 编码用的缓冲 —— ⏳ zh_main；④ VERIFY 已禁 zhub 里整帧转储（`ESP_LOG_BUFFER_HEX`）与密钥字段进日志 / hj_str；第四步定：② 放到建网（Z15）那一步一起做 —— 在那之前没有任何带密钥的帧经过 ASH（第五步只发 version / 两个协议参数 / 6 条策略 / addEndpoint / networkInit / networkState / nop / callback / getNetworkParameters / getEui64 / setConcentrator / setSourceRouteDiscoveryMode，VERIFY 钉着"不发建网 / 密钥类命令"） |
| Z18 | 状态读数：`/zigbee/status` 实读值、`zigbee_tx_power_source`、共存告警用实读值、仪表盘 / `/ws`、device.health 报芯片应答 | 设计 2.8、第十五节；A-08 / A-09 / A-22 | 状态 | M4 | ◐ 第四步：/zigbee/status 末尾加 hub 对象（直连型）：状态、网络、连续失败与原因、stackType / 版本、实际信道 / PAN / 扩展 PAN / 功率（getNetworkParameters 读回）、证据年龄、ASH 与会话计数。zigbee_tx_power_source、共存告警用实读值、仪表盘 / ws ⏳；2026-10-22 上板后：device.health 的 zigbee 改读 zhub 快照（radio / network，判据在 hub_cmds.c 的 zigbee_radio_str）—— Muse 原先据写死的 not_in_use 说"Zigbee 还不工作"；第五步：hub 对象加 eui64、concentrator、up_bad、incoming / incoming_bad、incoming_bad_last（最近一条解不开的：错误码、参数长度、前 31 字节消息头 —— 首次上板就靠它查出源路由开销字节）、last_rx（最近一条设备消息的来源与信号，载荷不出网关），直连型缓冲 2560 → 3072 |
| Z19 | 改 Thread 信道后核对与 Zigbee 实际信道的间隔，只警告 | 设计 2.8 | 网络启动 | M6 | ⏳ |
| Z20 | 入网窗口：permitJoining + 广播、JOIN_WINDOW 事件、灯、关窗后清临时链路密钥 | 设计 8.6 | 入网访谈 | M4 | ◐ 第六步（2026-10-22）：入网窗口状态机在 zh_host_core（正本是它文件头"入网窗口"一段）；JOIN_WINDOW 进事件环（不推送）。灯 ⏳ 随 Z54；⚠️ 2026-10-23 上板首个真实新入网（小米温湿度计）失败：开窗少了"导入临时链路密钥"（通配 EUI64 + ZigBeeAlliance09，SDK network-creator-security.c:299-308）—— 迁移来的网是 bellows 用散列信任中心密钥建的，老设备解不开 Transport Key、重新入网两次后放弃（C5 抓包实锤）；已补成开窗第 0 步（T36 / T39b、host_interop H11） |
| Z21 | `hub.zigbee.permit_join` zhub 起来后才注册；限速与总时长上限 | 设计 6.4、6.5 | 入网访谈 | M4 | ◐ 第六步：处理函数 cmd_zigbee_permit_join（hub_cmds.c，参数 0 或 30–254；等窗口真开好 / 关好再回，连受理最坏约 2 s）+ zhub_permit_join；关窗不进限速桶（hub_params_rl_exempt，两个出口共用；也不扣合计桶）；窗口开着不重开。⏳ Muse 注册，前提五条：① Z22 入网设备能报上去（M5 第四步审计订正：IAS 设备靠写 CIE + 登记已能报；温湿度 / 占用 / 插座这类的属性上报只发往绑定表 —— ✅ M5 第五步 Z24 给新入网的配了绑定与上报；残余：只对新入网的、只配解得开读数的簇（功率 / 颜色 / 窗帘 / 温控 / 门锁随控制路径）、按钮类的输出簇不绑、尽力而为不补做）；~~② muse_link 的 current_backends() 报 HUB_BACKEND_ZHUB 时 HUB_NEEDS_ANY_BACKEND 的 hub.device.control / update 会跟着注册，处理函数要先有（M5），或者把它们的 needs 换成设备层就绪的标志；~~ ✅ C17（M5 第三步）拆开了就绪位；~~③ 定每小时总时长上限做不做~~ ✅ 用户 2026-10-22 定按默认做：滚动一小时 600 s（zh_host_core.h，T46）；~~④（M5 第一步审计加）CONFIGURE 至少写 CIE 地址并自动登记~~ ✅ M5 第四步（Z24） —— 不然经窗口新入网的 IAS 设备（漏水 / 门磁）不发 Zone Status，Muse 说"加好了"却永远不报警；⑤ 容量口径（C10）；M5 第三步起 Muse 注册还要 `HUB_BACKEND_ZB_JOIN`（`current_backends()` 等这里的前提都满足了才报这一位） |
| Z22 | 入网回调与 Device_annce 建立 / 刷新记录；离网只标 left | 设计 8.6 | 入网访谈 | M4 | ◐ 第六步：trustCenterPostJoinHandler（入网 / 重新入网 / 离开 / 拒绝计数、最近一次的节点 / IEEE / 父节点）与 ZDO Device_annce（计数）在 zhub.c 的回调里观察，/zigbee/status 的 hub.joins 看得到；✅ 第七步 a（2026-10-22）：放行的入网 / 重新入网与 Device_annce 建立 / 刷新记录（zh_dev；拒绝的不建，D4 / D11），离开只标 left（D4），被动学习现网设备（8.12 ③，D2 / D3）；新记录（LIFECYCLE）/ 离开 / 回来（AVAILABILITY）进事件环，一台设备一条、带 zb- id（第六步按入网计数出的那两条已删，第二轮审计）；不推送 —— 推送等访谈完说得出它是什么，第七步 b。⏳ `DEVICE_OFFLINE` 的推送随可用性（Z32） |
| Z23 | 访谈状态机（重试、休眠设备、失败事件） | 设计 8.6 | 入网访谈 | M4 | ✅ 第七步 b（2026-10-23）：zh_dev 的访谈（D14–D27，变异 40 处全抓到）；每步 3 次 × 5 s、总 30 min、休眠设备只在刚说过话时推进、同时一条在途；失败发 LIFECYCLE、不落盘，它重新宣告 / 失败一小时后再说话时重来（两轮审计 ❌ 已修：旧记录 ZoneType、失败不重试、发送失败连发、刚入网的休眠设备卡住、事件文字不分来源、可选配置被拒复位）。⏳ "重新访谈"入口随前端；厂家 / 型号只存 23 B（6.8 要 32 B，改格式要升 REC_VER，M5 怪癖表按型号匹配前定） |
| Z24 | 绑定与上报表；IAS 登记与 ZoneType 解释 | 设计 8.6（表 8.6-1） | 入网访谈 | M4–M5 | ◐ 第七步 b：ZoneType 读进记录（`ias_zone_type`）；M5 第一步：登记请求回登记应答、Zone Status 进读数并按 ZoneType 解释（hub_zcap / state_event）。✅ M5 第四步：写 CIE 地址 + 自动登记、回读 ZoneState 确认、没成记欠着之后补（`ZH_IV_CIE` / `ZH_IV_ENROLL` / `ZH_DEVF_CIE_OWED`，zh_dev_test D36–D37）。✅ M5 第五步：绑定 + 上报配置（`ZH_IV_BIND` / `ZH_IV_REPORT`，只对新入网的、只配解得开读数的簇；D38）。⏳ 颜色 / 功率 / 计量 / 门锁 / 窗帘 / 温控的上报随控制路径 |
| Z25 | 设备库满了之后再有设备入网 / 被动学习到怎么办 | 设计 8.9 | 设备库 | M4 | ◐ 第七步 a：满了不建、不挤掉旧的、不再发空口 IEEE_addr_req（问到了也放不下），计数进 `/zigbee/status` 的 `hub.devices.full`（D7 / D12）；开机读不出来的格子保留不分配（`hub.devices.reserved`）。⏳ 用户能看到的提示 / 事件、容量定多少（C10） |
| Z26 | ZCL 帧头、全局命令、数据类型、换算 | 设计 8.7 | ZCL | M5 | ◐ 第七步 b：`zh_zcl` 帧头（含厂商码）、读属性请求 / 应答、默认应答、全部定长与字符串类型的长度（对着 SDK types.xml 逐个核）；M5 第一步：属性上报、有符号取值、读数的换算与无效值哨兵（zh_dev 的 st_apply）。⏳ 其余全局命令、复合类型、颜色 / 功率等换算 |
| Z27 | 空口报文按恶意输入：越界检查 + 模糊测试；不认识的类型丢本帧剩余并计数 | 设计 8.7、14.1；CLAUDE.md S1 | ZCL | M5 | ◐ 第七步 b：`zh_zcl` 每读一个字节前核剩余、不认识的类型停并丢本帧剩余（访谈计 `hub.devices.iv_bad`）、20 万帧模糊（ASan）；访谈的 ZDO 应答逐字段核长度（D24、D27）。M5 第一步：上报路径同样过（zh_zcl C8 + zh_dev D35 穿过 zh_dev_on_zcl 的模糊，UBSan；读数只收 XML 规定的类型，审计 ❌：int32 气压 ×10 溢出）。⏳ 控制路径的应答解析 |
| Z28 | 按簇定能力、判按钮；怪癖表数据驱动 | 设计 8.8 | 能力 | M5 | ◐ M5 第一步（2026-10-23）：`hub_zcap`（纯逻辑，hub_zcap_test 26 项、变异 11/12，剩的是无实际意义的锁 / 温控先后）。✅ M5 第八步（2026-10-23）：涂鸦 0xEF00 读数，表从 zigbee-herdsman-converters 26.123.0 导出（`tools/zhc_export/regen.sh`；1119 条指纹，金样例 1496 条逐条对上库的输出；zh_dev D41–D42、hub_zcap Z7；变异 30/30）。✅ M5 第九步：涂鸦开关 / 亮度写数据点（289 条指纹，写方向金样例 172 条；zh_dev D43、变异 21/23，剩两处防御性的）。✅ M5 第十步：小米 / Aqara 读数（TLV、0xFCC0、标准簇改写、IAS 报警、电量曲线、人体清零；151 个型号，金样例 430 条；zh_dev D44、hub_zcap Z8），型号从主动上报学、没访谈完也按表。⏳ 颜色能力（读 ColorCapabilities）、按钮事件、小米控制 / 按键事件、表放独立分区云端更新（第四阶段）、回涂鸦对时请求 0x24 |
| Z29 | 动作表的 Zigbee 列；只给语义动作，不注册原始 ZCL（MD-17 的 VERIFY 目前只写了 Matter） | 设计 6.2、14.2；架构 MD-17 | 能力 | M5 | ⏳ |
| Z30 | 设备模型的链路质量由 incomingMessageHandler 的 lastHop 值填 | 架构 F4；设计 6.1 | 能力 | M5 | ◐ 第七步 a：列表项的 `link`（lqi / rssi）取最近一条消息的 lastHop 值（zh_dev_on_rx；被动学习时触发查询的那条也算，D2）。⏳ 随能力 / 状态（M5）再看要不要平滑 |
| Z31 | 设备库记录格式、键空间、开机建索引、写入时机、只经 hub_store | 设计 8.9 | 设备库 | M5 | ◐ 第七步 a 提前做了：格式 / 键空间 / 写入时机正本 `zh_dev.h`，只经 hub_store（`hub_zigbee.c` 的钩子），开机顺序读回（没建索引，64 条线性扫）。⏳ 访谈字段（第七步 b）、按版本迁移的代码（等第一次改格式） |
| Z32 | 可用性判定（常电 / 电池）与去抖 | 设计 8.10 | 可用性 | M5 | ✅ M5 第十一步（2026-10-23）：常电静默 10 min 后 ping（读 Basic 0x0000）、3 次 × 10 s 没回判离线，电池 25 h；收到消息恢复；网不是 UP 不判、恢复后重新起算（原 ⚠️ 已处理）；离线 / 恢复出 AVAILABILITY 事件、列表 online 跟它（zh_dev D45）。⏳ 可用性不落盘（开机当在线）；2026-10-23 补：开机后一次都没听到过的电池设备网 UP 满 25 h 静默离线（不出事件，hub.devices.av_quiet） |
| Z33 | 控制路径的结果分类、休眠设备排队、时延目标 | 设计 8.11 | 控制 | M5 | ⏳；⚠️ 第三步审计：会话层最坏时延（应答超时，正本 zh_ses_cfg_default）大于作业截止（6.6；hub_registry 的控制 / permit_join 截止），链路不健康时上层会先报超时、结论后到——作业层要能撤回还没交给 ASH 的命令，或把"结论晚到"当正常情形；8.11 的时延目标只在链路正常时成立；✅ M5 第七步第一版：常电设备的开 / 关 / 切换 / 亮度，结论 confirmed / delivered / failed（zh_dev_test D40）；第九步加涂鸦（写 0xEF00 数据点，D43）；第十二步休眠设备排队（pending、醒来补发、30 min 作废，D46）。⏳ 色温 / 颜色 / 窗帘 / 门锁 / 温控；✅ TC-ZH04 已测（10-23 Sengled：设备确认 p50 35 / p95 37 ms）；默认应答成功乐观更新读数（ctl_optimistic） |
| Z34 | 作业层多在途 vs zhub 单在途：访谈与控制挤一条道，要排队优先级 | 设计 6.6、8.4 | 控制 | M5 | ⏳；会话层同时只收上层一条（在途或排队），排队优先级在作业层 |
| Z35 | 敏感动作默认禁止；TC-MU12 | 设计 6.5、第十七节 | 控制 | M5 | ⏳ |
| Z36 | 返回兑现命令说明承诺的字段；错误串写成用户能照做的句子；写了就生效、读回新值 | 架构第六节（G0-2 实测） | 控制 | M5 | ⏳ |
| Z37 | 迁移：接管现网、被动学习、拓扑遍历、名字导入、反向迁移、导出不含密钥、Z2M GENERATE 提示 | 设计 8.12 | 迁移 | M6 | ◐ 接管现网 / 被动学习（M4）；拓扑遍历 = 主动发现（M6 第一步，8.12 ④）。还剩名字导入、反向迁移、导出、GENERATE 提示 |
| Z38 | 迁移脚本搬身份、先擦、刷完完全断电（IO28） | 设计 8.12 | 迁移 | 已落地 | ✅ `migrate_profile.py` |
| Z39 | **MD-13**：zhub 接进固件的那个提交同时改 `docs/00` 第九节的 zHub 口径 | 架构 MD-13；设计第十六节 | 文档同步 | M4 | ✅ 第四步（同一个提交）：docs/00 文首作用域 + 第九节（zHub 单列一行、A 表前三行与 A.1–A.3、"实际信道""主动探测"两行标作用域）；check_scope_claims.py 的 zHub 判据改成"components/zhub/ 以外的每处引用都在 #if CONFIG_BITENGINE_ENABLE_ZHUB 里 + HA 型前端不许出现 + 第九节那一行写明作用域"（变异实测） |
| Z40 | NCP 不回 RSTACK 时的退避与软复位，绝不碰 IO23 / mg24_do_reset | 设计 8.13 | 失败模式 | M4 | ✅ 第四步：退避表的正本是 `zh_host_core.c` 的 k_backoff_ms（第 1 次失败立即重来，之后逐档加长），连续 RUNNING 满 ZH_HOST_STABLE_MS 才清零；C3 定案见 zh_host_core.h 文件头。不碰 IO23（VERIFY） |
| Z41 | 网络 down 的恢复；复用 webhook / MQTT 的故障事件；帧级 UART 错误率告警 | 设计 8.13；`docs/01` 健康指标；`docs/04` | 失败模式 | M4–M5 | ⏳；EZSP 审计补：错误率分子要加上 EZSP 层的拒收（长度错 / 短帧 / 保留位）——CRC 漏洞正是这类帧；告警出口映射（哪些 RESET_LINK 原因对应 MQTT zigbee_restart / webhook zigbee_crash、data.reason 填什么）随 zh_main 定；⚠️ RSTACK 正常但保活反复失败的无限复位循环要升级到红灯 + GATEWAY 推送（配 C3 / C4 冷却），不能只有日志；会话层的计数在 zh_ses_stats_t，溢出位只有计数没有事件；第四步：失败按原因计数（zh_host_stats_t.fails）、日志、快照里 failing（连续 ≥ ZH_HOST_FAILING_AT），复位循环被退避表拖慢到最慢 60 s 一次；红灯 / GATEWAY 推送 / webhook / MQTT 出口仍 ⏳；第四步补：网络 down 期限内没回来记 NET_DOWN（也走退避、也亮 failing）；✅ M5 第十三步（TC-ZH07）：NCP 自己重启（故障注入构建背着 ASH 发 RST）上板实测自愈，立即档 2.6 s、退避档 7.7 / 32.9 s（`tools/test_zhub_ncp_reset.py`） |
| Z42 | 安全模式下 zhub 不起；开机读 NCP 版本时已发过 ASH RST → 安全模式期间 Zigbee 停摆 | 架构安全模式段；设计 2.6；`ezsp_bridge.c` 开机读版本 | 失败模式 | M4 | ⏳（见 C14）。第四步之后 zhub 真在跑了，现状要写明：安全模式不起 zhub（main.c 里 zhub_start 在安全模式出口之后），整板上电进安全模式时开机读 NCP 版本已经发过一次 ASH RST，之后没人把协议栈拉起来 —— 直连型安全模式期间 Zigbee 是停的。暂不改的理由：安全模式是"自愈重启 1 小时 ≥ 3 次"后的降级，起因可能就在 zhub 或它依赖的组件里，在那时再起 zhub 有把降级态也拖进重启循环的风险；要不要起、起到哪一步随 C14 定 |
| Z43 | zh_ash、zh_main 的看门狗级别与期限；wd_register 与 wd_kick 成对 | 设计 8.2；`docs/04` 层 B | 看门狗 | M4 | ✅ 第四步：合并后只有一个任务 "zhub"（Z60），看门狗级别与期限的正本是 `zhub.c` 的 ZHUB_WD_*；wd_register 在任务开头、wd_kick 每圈无条件打（VERIFY） |
| Z44 | 直连型 WD_MAX_TASKS、HEAP_MARK_MAX 提高（`#if DIRECT`） | 设计第十五节 A-24、第十七节 | 看门狗 | M4 之前 | ✅ 2026-10-22：直连型 20 / 32（HA 型仍 16 / 24）；watchdog 主机测试按两种配置各编一次，VERIFY 钉住 |
| Z45 | zh_ash 心跳不能绑在 NCP 应答上（MG24 无响应封顶 L2、禁 L4）；zhub_suspend 期间暂停或照常 kick | `docs/04` MG24 恢复规则 | 看门狗 | M4 | ✅ 第四步：心跳每圈都打、不看 NCP（任务每圈最多阻塞 ZHUB_LOOP_MAX_MS，写有超时）；挂起期间照常 kick（任务不停，只是不碰 UART）；zh_host_next_timeout_ms 在 IDLE 回 UINT32_MAX，外壳自己夹上限；另有"连续 20 圈不等待就让出 1 ms"的防空转 |
| Z46 | zh_ash（高优先级）要不要订阅 Task WDT（04 对同级的桥任务做了） | `docs/04` 层 A | 看门狗 | M4 | ✅ 第四步定：不订阅 Task WDT —— 合并后的任务不在原 zh_ash 的高优先级上（取值与理由见 zhub.c 文件头），每圈都阻塞在 UART 读上，外壳另有防空转；CPU 被占死时 IDF 的 TWDT 盯着 IDLE 任务本来就会响 |
| Z47 | wd_probes 的 MG24 证据改由 zhub 自报，`/diag/watchdog` 照报三态；恢复改走 zhub_restart | 设计 8.1；`docs/04` §6；`wd_probes.c` | 看门狗 | M4 | ✅ 第四步：wd_probes.c 直连型走 probe_mg24_zhub（zhub_mg24_evidence：证据只认这次接管 UART 以来的 —— 比正向证据计数、不比时间戳，持有多久都不回绕（第一轮审计 ❌1）；没有证据时年龄从接管起算，64 位），/diag/watchdog 照报三态；挂起时不判定、也不把 mg24_last_ok_s 刷成"现在"（docs/04 禁止的伪造）；注入 mg24_mute 在直连型同样生效（TC-W04）；判死的定向恢复是 zhub_wd_recover（请 zhub 立即复位，PERIPH 封顶 L2） |
| Z48 | `/zigbee/restart` 从 501 改为 zhub 软重启 | 设计 2.8 | 状态 | M4 | ✅ 第四步：直连型 /zigbee/restart → zhub_request_restart（立即 ASH RST + 重新 networkInit，跳过退避、不计失败）；最短间隔见 zhub.c 的 ZHUB_RESTART_MIN_MS，冷却期内 429 cooldown_active（同 HA 型），zhub 没持有 UART 时 409 zhub_not_running |
| Z49 | 产测 ezsp_uart 项靠 zh_ash 累加收发字节；直连型产线套件 `.sh` 与 `.ps1` 同改（红线 14） | 设计 2.8；`factory_api.c` | 状态 | M4 | ◐ 第四步：zhub 收发字节经 ezsp_bridge_note_uart 计入 bridge_stats（产测 ezsp_uart 项、/status、health_monitor 错误率的分母都有了）；直连型产线套件 .sh / .ps1 仍 ⏳ |
| Z50 | 新端点守卫在 AUTH 前；"清空设备库""离开 / 重置 Zigbee 网络"还没有端点 | 设计 10.1、第十二节 | 状态 | M5–M6 | ⏳ |
| Z51 | 红线 4 的验收（`ncp_probe.py` 要连 :6638）直连型用不了：zhub 给出"已入网 + PAN 不变"的读数 | `mg24/README.md` | 状态 | M6 | ◐ 第五步：`/zigbee/status` 的 hub 对象已有 eui64、pan_id、ext_pan_id、net（第二轮审计），"PAN 不变"的比对（与迁移前记下的值）⏳ 随 M6 迁移 |
| Z52 | test_api.sh 的生效断言：改地区 → 功率变；permit_join → 剩余秒数；改名 → 读回新值 | CLAUDE.md 7.5；设计 14.4 | 状态 | M4–M5 | ⏳ |
| Z53 | CLAUDE.md 关键决策 1、6 与红线 13 标"只约束 HA 型"；04 / 01 相关节加"直连型见 docs/15 第八节"指针 | CLAUDE.md；`docs/04`；`docs/01` | 文档同步 | M4 | ✅ 第四步（与 MD-13 同一个提交）：CLAUDE.md 关键决策 1、6 标"只约束 HA 型"；红线 13 写明**两种配置都不加功率开关**、只是"归 Z2M/ZHA"的理由只对 HA 型成立（架构 5.7）；docs/04 §6.1、docs/01 透传一节加指针；watchdog/VERIFY 的 probe_mg24 判据精确到 HA 型那支（前缀匹配会撞上 probe_mg24_zhub），直连型那支由 zhub/VERIFY 钉住"不调 health_probe、只读 zhub 的证据" |
| Z54 | ZIGBEE 灯的 Hub 语义与活动闪；check_led_docs.py 按配置分列；check_capacity_docs.py 写直连型口径 | 设计第十一节、第十六节 | 文档同步 | M4 / M6 | ◐ 灯 2026-10-24 用户定、做了（check_led_docs.py 按配置分列、check_capacity_docs.py 直连型口径还没做）：看 zhub 的状态 —— 网络 up 绿、入网窗口开着 / 建网中黄慢闪、芯片不应答红、设备消息蓝色短闪（`zhub.c` 的 `led_push`、`led_ctrl.c` 的 ENABLE_ZHUB 一支；设计第十一节注记）；以下是当初的记录 —— ⚠️ 2026-10-22 上板：直连型桥恒为 DISABLED，ZIGBEE 灯按现有逻辑恒灭（zhub 在跑、网络 up 也灭） |
| Z55 | 编码前先写 VERIFY.sh、关掉 E12–E15；主机侧测试 ①–⑦ | 设计 14.1、14.2、附录 E | 文档同步 | M4 | ◐ VERIFY、①②、E12 已做（第一步）；③ 已做（第二步）；会话层真值表 + 端到端（第三步）；E13：两种回调方式会话层都支持了，只剩上板确认 |
| Z56 | `zh_ash_t`（约 1.3 KB）与发送缓冲（≥ `ZH_ASH_WIRE_MAX`）放内部 RAM 还是 PSRAM；或把发送队列缩到 2（EZSP 同时只有一条在途） | 设计 8.2 的内存账 | ASH | M4 | ✅ 第四步定：上下文（zh_host_t 约 2 KB + 收发缓冲）与任务栈都放 PSRAM（任务不写 flash）；发送队列不缩（窗口 1 时只用到 1 项，缩了省的是 PSRAM） |
| Z57 | EZSP 层：检查 frame control 方向位（丢 em260 原地应答被覆盖后的命令回显）、按帧 ID 严格校验应答长度（CRC 结构性漏洞）、命令超时后重发再复位 | 设计 8.4 的 ⚠️ 条；`zh_ash_core.h` 末段 | EZSP | M4 | ✅ 方向位、严格长度（第二步）；第三步（zh_ezsp_ses）：回显后幂等才重发，**应答超时一次就复位链路、不重发**（NCP 可能还有应答在重传），保活失败同样复位。复位本身随 zh_main |
| Z58 | 空闲保活：连接中定期发 EZSP `nop`；EZSP 应答超时 → 复位链路（NCP 静默进 ERROR 时 ASH 层自己发现不了） | 设计 8.13 新加的那行 | 失败模式 | M4 | ✅ 逻辑（第三步，zh_ezsp_ses：空闲时定期发 nop、超时即报 RESET_LINK；ses_interop E5 实测应用层挂死时会话自己发现并恢复，时限见那条判据）；接进任务随 zh_main |
| Z59 | **回显后自动重发 = NCP 执行两次**：回显说明 NCP 已经处理完、应答缓冲被重传的命令覆盖。只有幂等命令可以自动重发；`sendUnicast` / `sendBroadcast` 超时或收到回显记"结果未知"，靠 `messageSentHandler` 的 tag 确认；`formNetwork`、`setInitialSecurityState`、`leaveNetwork`、`addEndpoint` 一律不自动重发（toggle 翻两次 = 灯没变却报成功） | 设计 8.4 的 ⚠️ 条；`zh_ash_core.h` 末段 | EZSP / 控制 | M4–M5 | ✅ 会话层（第三步：只有回显后才重发、只给幂等命令；应答超时一律不重发而是复位；非幂等回 UNKNOWN / TIMEOUT；ses_interop 的噪声场景、确定性回显、2 s 超时对照都证明非幂等命令没被执行两次）。⏳ 上层对 UNKNOWN / TIMEOUT 的 sendUnicast 靠 messageSentHandler 的 tag 确认（Z33） |
| Z60 | **会话层与 zh_ash 的跨任务模型**：zh_ezsp_ses 与 zh_ash_core 都没有锁，会话的入口、ops（zh_ash_send / zh_ash_queued）与 zh_ash_tx / zh_ash_rx 必须同一任务串行。设计 8.2 把 zh_ash（4 KB、优先级 21）与 zh_main 分开、中间一条帧队列——二选一：会话与 zh_ash 同一任务（结果 / 回调再经队列交 zh_main，上层回调不跑在 zh_ash 的小栈上），或经队列转交的 ops（那时"unacked 变 0 = 确认"要重新论证） | `zh_ezsp_ses.h` 线程契约；设计 8.2 | ASH / EZSP | M4 | ✅ 2026-10-22 用户定：**合并成一个任务**（zhub.c 的 "zhub"）。会话、ASH、编排同一任务串行，zh_ezsp_ses.h 的线程契约就此成立；别的任务只经 zhub.h 的请求接口（挂起 / 恢复 / 立即复位 / 读快照） |
## 互相矛盾的地方（要在动到对应层时定下来，只写一处正本）

| # | 矛盾 | 处置 |
|---|---|---|
| C1 | ASH 主机侧参数（ACK 定时器、超时次数、等 RSTACK 的时限）在设计 8.3 / 8.13 / 附录 E12、`ncp_version.c`、`watchdog.h`、`uart_bench.c`、`docs/04` 里各写一套 | ✅ M4 第一步：主机侧跟 SDK 主机参考实现，正本是 `zh_ash_cfg_default()`；设计 8.3 / 8.13 / E12 改成指针 |
| C2 | MG24 判死阈值在 `docs/04`、`watchdog.h`、`docs/01` 各写一套 | ✅ 第四步：阈值只认代码（`watchdog.h` 的 MG24_HEARTBEAT_TIMEOUT_S），docs/04 常量表那两行改成指针（原写 60 / FAIL_THRESHOLD 2，都没实施过）；直连型的 probe_mg24_zhub 用同一个阈值 |
| C3 | 复位策略：01 还是 GPIO23 硬复位；04 是 L2 = ASH RST、禁 L4；8.13 的"另写一条软复位"没定义 | ✅ 第四步定：本板 MG24 的软件复位只有 ASH RST（IO23 / RS706 禁用；nBOOT 是进 bootloader）。退避与"持续不正常"的判据在 zh_host_core.h，8.13 那一行改成指针 |
| C4 | 复位冷却：`mg24_reset` 的冷却 vs 8.13 的退避 | ✅ 第四步：直连型只有 zh_host_core 的退避（mg24_reset 的冷却在直连型不编进调用路径：mg24_do_reset / watchdog_reset_mg24 直连型禁用） |
| C5 | zh_ash 设 FATAL vs 04"MG24 宁可报离线也不整机重启"；`/zigbee/bootloader` 占 UART、或链路判断 + 复位 + 退避的最坏耗时接近 FATAL 期限时会误触发 | ✅ 第四步：心跳与 NCP 应答解耦（每圈都打）+ 挂起期间任务照转照打；bootloader 占 UART 期间 zhub 挂着、不判定 |
| C6 | 看门狗槽位账 | ✅ 设计 8.2 / A-24 已改；直连型 WD_MAX_TASKS 20 已落地（Z44） |
| C7 | `tx_blocked`：`/api/v1/diag/uart` 是要独占 UART 的基准测试，不是持续信号 | ✅ 第四步：zhub 自己计写不全（uart_tx_short），/diag/uart 仍是独占的基准测试（经 ota_claim 让 zhub 让出） |
| C8 | 表 8.5-1 要写 KEY_TABLE_SIZE、PACKET_BUFFER_COUNT，但 NCP 实测拒改 | 随网络启动那一步（Z14） |
| C9 | 建网选信道只按能量 vs 2.8 的信道偏好 | 同上（Z16） |
| C10 | "64"：直连终端（路由器不占）vs 设备库总容量；从 HA 型迁来的网可能超 64 | 随设备库（Z25） |
| C11 | TC-ZH02 / TC-ZH03 是 M4 出口，但迁移在 M6、设备库与 ZCL 在 M5 | 重排里程碑判据 |
| C12 | UART 错误率：health_monitor 按字节、8.13 按帧；直连型不累加字节数会误报 100% | ◐ 第四步：直连型的字节计数有了（分母不再是 0，"一个错误就 100%"的误报没了）；按帧算、加上 EZSP 层拒收的分子 ⏳（Z41） |
| C13 | 01:1241"CRC 错不重传、交给 Z2M/ZHA"只对桥成立 | ✅ zhub 自己 NAK / 重传（zh_ash_core）；01 的作用域标注随 Z53 |
| C14 | 直连型安全模式不起 zhub vs T9.31"主机走开网络也该照跑" | 随失败模式（Z42） |
| C15 | EZSP 普通命令的超时（设计 8.4）短于 ASH 一次重传可能花的时间：ASH 还在纠错，EZSP 已经判命令超时 | ✅ 第三步定，两个方向：主机→NCP 计时从 ASH 确认之后起算；NCP→主机，NCP 侧重传应答没有严格上界（单帧有上界，发送窗口里排着回调时部分确认会把超时计数清零），所以应答超时取得比单帧最坏长、而且**超时一次就复位链路**（ASH RST 清空 NCP 发送队列），不重发。数值与推导的正本是 `zh_ses_cfg_default()` 的注释（真值表 t_defaults 钉住） |
| C16 | M5 第一步已知局限（后续步骤）：① 读数按设备存一份，多端点设备（双路开关）的值互相覆盖；② 小米怪癖（门磁走 OnOff 簇会被判成 switch、0x0406 人体只报 1 不报 0）要怪癖表；③ 常电灯 / 插座重启后 state 一直空到下次上报（8.9 说"靠上报和读取重建"，读取还没做）；④ zhub 不回 ZCL 默认应答（有的设备会因此重发，H1 待核）；⑤ kind 词表只在 hub_zcap.c，Matter（9.6）要共用 | 随 M5 后续 |
| C17 | M5 第二步：`hub.device.update`（名字 / 房间 / notify，`hub_core/hub_meta*`）与还没实现的 `hub.device.control` 共用 `HUB_NEEDS_ANY_BACKEND`，Muse 一打开两条一起注册。敏感标记存着、还没有网页入口（恒 false） | ✅ M5 第三步：就绪位拆开（`HUB_BACKEND_CONTROL` / `HUB_BACKEND_ZB_JOIN`），`current_backends()` 只报 zhub → Muse 注册 update、不注册 control / permit_join；后端变了在线重注册（`/muse/status` 的 `reg_backends`，test_api M22b）。⏳ 网页设备页 |

## 现有代码：继承或避开

| # | 代码 | 处置 |
|---|---|---|
| K1 | `ezsp_bridge/uart_bridge.c` | 继承：460800、流控按 Kconfig、RX 2048 / TX 0；写函数可能写不全，zhub 必须处理 |
| K2 | `uart_bridge_line_says_absent`、`/diag/ncp_line` | 继承：读 ctsn 的负面证据，只能判死 |
| K3 | `ezsp_bridge/ncp_version.c` | 继承常量与已核过的命令；它只备 16 字节发送、跑两个事务就停，zhub 是重写不是扩；直连型的帧编解码由 `zh_ezsp_core` 提供（M4 第二步，与它的请求字节逐字节相同、解析更严），它本身是 HA 代码不改 |
| K4 | `ncp_cache` + 开机读版本 | 读完 zhub 再 RST 一次（上电双复位，可省）；build_string 要保住 |
| K5 | `ezsp_bridge_health_probe` | ✅ 第四步：直连型不调（wd_probes 走 probe_mg24_zhub）；ezsp_bridge_health_probe 在直连型一律回 false、不发 RST（不按状态判：zhub 让出之后、别人认领之前那一下状态也是 RUNNING，查和用之间有竞态 —— 第一轮审计） |
| K6 | `watchdog/wd_probes.c` | ✅ 第四步：直连型走 probe_mg24_zhub，不发 RST、只读 zhub 自报的证据 |
| K7 | `watchdog/health_monitor.c` | ◐ 第四步：分母有了（zhub 计字节）；按帧（C12）⏳ |
| K8 | `watchdog/mg24_reset.c` | PCB 上只有拉 IO23 那一支，直连型禁用（已有判据） |
| K9 | `ota_mgr/ota_mg24.c` | ✅ 第四步：ota_mg24 一行不改 —— 它的 ezsp_bridge_ota_claim 在直连型里先请 zhub 让出，release 后 zhub 自己接回去（resume 排在它读完版本之后，因为读版本在 release 之前） |
| K10 | `ota_mgr/uart_bench.c`（`POST /diag/uart`） | ✅ 第四步：同 K9（uart_bench 也是 ota_claim / release） |
| K11 | `webui/rest_api.c`：`/zigbee/bootloader`、`/radio/region`、`/zigbee/restart` | ◐ 第四步：/zigbee/restart ✅（Z48）；/zigbee/bootloader 经 ota_claim ✅；/radio/region 暂走 ncp_resync（Z07 / Z11） |
| K12 | `rest_api.c:464` `zigbee_tx_power_source` | 改 |
| K13 | `bridge_stats` uart_rx/tx_bytes | ✅ 第四步：ezsp_bridge_note_uart（只在直连型声明） |
