#pragma once
/* zh_host_core —— zhub 的编排层，纯逻辑（docs/15_muse_direct_design_v1.md 8.2 合并后的单任务、8.5 开头一段、8.13；CHECKLIST Z02 Z03 Z04
 * Z40 Z43 Z45 Z58 Z60 C3 C4）。不依赖 ESP-IDF、不分配内存：test_host/ 原样编它。
 *
 * 一个对象装下 ASH（zh_ash_core）与会话（zh_ezsp_ses），对外只有"字节进、字节出、定时器"：
 *
 *   UART 收到的字节 ─▶ zh_host_rx ─▶ zh_ash_rx ─▶ zh_ses_rx ─▶ 结果 / 回调 / 事件（本层记账）
 *   zh_host_tick：处理记下的链路事件（复位、退避、握手）→ 交下一条初始化命令 → zh_ses_tick
 *   zh_host_tx ─▶ zh_ash_tx（同时跑 ASH 的定时器）─▶ 写 UART 的字节
 *
 * ⚠️ 线程契约（S2，CHECKLIST Z60 定案）：zh_ash 与 zh_main 合并成**一个任务**（components/zhub/zhub.c 的 "zhub"），本对象的所有入口都在
 *    那个任务里串行调用。zh_ash_core 与 zh_ezsp_ses 自己都没有锁，它们的线程契约由此成立；别的任务只能经 zhub.c 的请求接口（挂起 / 恢复 /
 *    立即复位 / 读快照）间接影响它。
 *
 * 管的事：
 *  - 启动 / 恢复（zh_host_start）：ASH RST → RSTACK → 会话握手（version 0x12）→ 初始化序列（k_init，见下）→ networkInit →
 *    等 stackStatusHandler(NETWORK_UP) → RUNNING → UP 之后的序列（见下）。之后会话自己保活（nop）、取回调。
 *  - 初始化序列（networkInit 之前，zh_host_core.c 的 k_init 是正本；设计 8.5，M4 第五步）：
 *    ① 两个协议参数 STACK_PROFILE = 2、SECURITY_LEVEL = 5（表 8.5-1 第一行）。⚠️ STACK_PROFILE 省不得：NCP 出厂是 0，网络是按 profile 2 组的，
 *       不先写 networkInit 恒回 NOT_JOINED —— 看着像网络丢了（mg24/ncp_probe.py 文件头，2026-10-06 栽过；2026-10-22 第四步首次上板又栽一次）。
 *       **不写**表容量类配置值（红线 4、Z14：NCP 不被写就用编译值，NVM3 里的表不受影响）。
 *    ② 7 条策略（表 8.5-2，取值见下面的常量）。信任中心策略在窗口外是"只许重新入网"（ZH_HOST_TC_POLICY_CLOSED）：NCP 上电默认是
 *       ALLOW_JOINS | ALLOW_UNSECURED_REJOINS（em260-common.c:39），不写的话谁家路由器开着入网，新设备就能拿到网络密钥。入网窗口是第六步。
 *    ③ 端点 1（表 8.5-3）。addEndpoint 必须在 NETWORK_UP 之前（之后 NCP 回 INVALID_CALL，command-prototypes.h:113-122）；NCP 复位就忘，
 *       所以每次 NCP 启动都写一遍 —— 我们每一轮都从 ASH RST 开始，正好。
 *    任何一步被拒 / 结果说不清都按 networkInit 失败处理（NET_INIT，code 见下）：复位重来、退避限速、连续失败亮 failing。
 *    networkInit 回 NOT_JOINED 只记"没有网络"（ZH_NET_NONE），**绝不自动建网**——建网会换掉网络、所有设备掉线（8.5.1 / 8.12 另做）。
 *  - UP 之后的序列（每次进入 UP 都从头走一遍；步骤的正本是下面的 zh_host_up_step_t，每步怎么编、怎么解在 zh_host_core.c 的
 *    submit_want / up_result —— 加一步三处都要动）：getNetworkParameters（信道、PAN、扩展 PAN）→ getEui64 →
 *    setConcentrator（HIGH_RAM 集中器）→ setSourceRouteDiscoveryMode(RESCHEDULE)。多对一路由按 SDK 在 NETWORK_UP 之后开（herdsman 同）。
 *    **尽力而为**：哪一步被拒 / 没读到只记账（st.up_bad），过 ZH_HOST_UP_RETRY_MS 再试同一步，不复位链路 —— 少的是一份读数或一个
 *    集中器设置，Zigbee 照常收发（bellows 设集中器失败也只打警告）；为它复位只会让整张网停得更久。
 *  - networkInit **每次 NCP 启动只发一次**（只在握手完成时要；同一次启动里再发，真协议栈回 INVALID_STATE，它也不幂等，Z59）。
 *    之后网络起不来 / 掉了（networkInit 说 OK 而 NETWORK_UP 等不到、运行中报 NETWORK_DOWN）：先用幂等的 networkState 查一次
 *    （回调丢了也认得出网络其实在），ZH_HOST_NET_DOWN_MS 内还没回来就记一次失败（NET_DOWN）、复位重来 —— 每次重来都是 NCP 的新一次启动，
 *    networkInit 在那次启动里再发一次；退避表限速，连续失败照样亮 failing。2026-10-22 第一轮审计：原先"同一次启动里 60 s 重试 networkInit"
 *    永远拿 INVALID_STATE，而保活照常有应答，Zigbee 停着却没有任何告警。
 *  - 失败（ASH：RSTACK 等不到 / 版本不对 / NCP 自己重启 / ERROR 帧 / ACK 连续超时；会话：RESET_LINK / 协议号不对；networkInit 的结果不干净）
 *    一律走同一条路：会话收摊（在途命令了结）→ ASH 静音（zh_ash_stop）→ 按连续失败次数退避 → ASH RST 重来。
 *    退避表的正本是 zh_host_core.c 的 k_backoff_ms（第 1 次失败立即重来，之后逐档加长到 8.13 说的 60 s 封顶）；
 *    连续 RUNNING 满 ZH_HOST_STABLE_MS 才把连续失败次数清零 —— "RSTACK 正常、保活反复失败"的复位循环也被这张表拖慢（Z41 的担心）。
 *    连续失败 ≥ ZH_HOST_FAILING_AT 次在快照里置 failing（灯 / 推送的出口随 Z41、Z54）。
 *  - 入网窗口（zh_host_permit_join，设计 8.6，M4 第六步）：开 = 导入临时链路密钥（通配 EUI64 + 公开密钥 ZigBeeAlliance09，SDK 框架开网那一步
 *    network-creator-security.c:299-308；2026-10-23 补：迁移来的网是 bellows 用散列信任中心密钥建的，不导这把，老设备（小米）解不开
 *    Transport Key、反复重新入网后放弃 —— 抓包实锤）→ 信任中心策略换成"放新设备"（ZH_HOST_TC_POLICY_OPEN）→ permitJoining(s) →
 *    向 0xFFFC 广播 ZDO Mgmt_Permit_Joining_req（让路由器也放行；SDK 框架 af-common.c:1003-1012、1328-1342 同此顺序与格式）；关（到期 / seconds 0 /
 *    网络掉了）= permitJoining(0) → 广播 0 → 信任中心策略写回窗口外的取值 → clearTransientLinkKeys（SDK 框架关窗口那几步，
 *    network-creator-security.c:326-329）。开的某一步失败：走关窗把已改的复原，记一次失败。关的步骤里只有"写回信任中心策略"必须成功
 *    （被拒就过一会儿重试，连着 ZH_HOST_WIN_POLICY_TRIES 次就复位链路 —— NCP 重启后 k_init 写回；安全相关，不许跳过）；permitJoining(0) / 广播在网络掉了时会失败、跳过（路由器的窗口到期自己关；临时链路密钥 300 s 自己过期，
 *    sl_zigbee_configuration_defaults.h:582）。链路一复位窗口就回 CLOSED：NCP 重启了，permitJoining 没了，k_init 也把信任中心写回窗口外。
 *    窗口命令和别的初始化命令共用会话的单在途：want 槽空着时才交窗口那一步。
 *    总时长上限：滚动 60 min 里最多开 ZH_HOST_PERMIT_HOUR_CAP_S；剩的不够 ZH_HOST_PERMIT_TRIM_MIN_S 不开，够就开、截到剩的；早关的只算真开了的时长。
 *  - 协议号不对（会话报 VERSION_MISMATCH）与设计 8.4 的出入：不"停住"，按普通失败走退避表重试 —— 可能是 NCP 正在被升级、或回包的是 bootloader，
 *    会自己好；last_fail = EZSP_VERSION、last_fail_code = NCP 回的协议号，就是 8.4 说的 ncp_ezsp_mismatch（CHECKLIST Z08）。
 *  - C3 定案：本板上 MG24 的软件复位**只有 ASH RST 这一条**（IO23 / RS706 禁用，CLAUDE.md；nBOOT 是进 bootloader 不是复位），
 *    8.13 说的"3 次后走软复位路径"就是按退避表继续发 ASH RST + 快照里报 failing，没有另一条路可走。
 *  - 链路级的转移（复位 ASH、link_up）**只在 zh_host_tick 里做**：ASH / 会话的回调里只记账（会话不许回调里 link_up；在 ASH 自己的
 *    调用栈里复位它也不干净）。所以 zh_host_rx / zh_host_tx 之后要调一次 zh_host_tick，zh_host_next_timeout_ms 在有事待办时回 0。
 *  - 自己收摊（zh_ses_link_down）时会话交回的 UNKNOWN / LINK_DOWN 不再算一次失败（teardown 标志）。
 *
 * 时间都是调用方给的单调毫秒（uint32_t，回绕安全：只比差值）。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zh_ash_core.h"
#include "zh_ezsp_core.h"
#include "zh_ezsp_ses.h"

/* sl_status_t（~/src/mg24-fw/sdk/platform/common/inc/sl_status.h）：EZSP v18 的状态都是它 */
#define ZH_SL_OK              0x0000u  /* sl_status.h:85 */
#define ZH_SL_NETWORK_UP      0x0015u  /* sl_status.h:108：stackStatusHandler 报"网络起来了"（af-common.c:352） */
#define ZH_SL_NETWORK_DOWN    0x0016u  /* sl_status.h:109 */
#define ZH_SL_NOT_JOINED      0x0017u  /* sl_status.h:110：networkInit —— NVM3 里没有网络（network-formation.h:71-81） */
#define ZH_SL_TC_SWAP_CHANGED 0x0C10u  /* sl_status.h:449：信任中心换机后重新入网，同样意味着 NETWORK_UP（af-common.c:353） */
#define ZH_SL_TC_SWAP_SAME    0x0C11u  /* sl_status.h:450：同上（af-common.c:354） */
#define ZH_SL_NETWORK_OPENED  0x0C18u  /* sl_status.h:457：stackStatusHandler 报"网络开放入网了"（NCP 那边的窗口真开了） */
#define ZH_SL_NETWORK_CLOSED  0x0C19u  /* sl_status.h:458：同上，关了 */
/* networkInit 之前必须写的两个协议参数（配置 ID 出自 ezsp-enum.h；取值：设计表 8.5-1、bellows 的默认配置、SDK 的 SL_ZIGBEE_STACK_PROFILE 2 档） */
#define ZH_EZSP_CFG_STACK_PROFILE  0x0Cu  /* ezsp-enum.h:43 SL_ZIGBEE_EZSP_CONFIG_STACK_PROFILE */
#define ZH_EZSP_CFG_SECURITY_LEVEL 0x0Du  /* ezsp-enum.h:47 SL_ZIGBEE_EZSP_CONFIG_SECURITY_LEVEL */
#define ZH_HOST_STACK_PROFILE      2u     /* ZigBee PRO */
#define ZH_HOST_SECURITY_LEVEL     5u     /* sl_zigbee_configuration_defaults.h:147（profile 2 档的取值） */
/* MAC 间接发送超时（父节点替休眠子节点缓存一帧多久）：NCP 编译时没定协议档，默认只有 3000 ms（sl_zigbee_configuration_defaults.h:334-335）；
   SDK 主机在协议 2 档显式写 7680（下面的 af-host.c 行，前一行是配置名），bellows 也写 7680。不写的话直连在网关下的休眠设备轮询间隔超过 3 s 就收不到
   访谈请求（第七步 b H1）。它是运行时配置、不是表容量（红线 4 不涉及），NCP 每次启动都要重写 */
#define ZH_EZSP_CFG_INDIRECT_TRANSMISSION_TIMEOUT 0x12u  /* ezsp-enum.h:54 SL_ZIGBEE_EZSP_CONFIG_INDIRECT_TRANSMISSION_TIMEOUT */
#define ZH_HOST_INDIRECT_TX_TIMEOUT_MS 7680u  /* af-host.c:402（7.68 s = 802.15.4 macTransactionPersistenceTime 默认值） */
#define ZH_NET_STATUS_JOINED  2u       /* sl_zigbee_types.h:945 SL_ZIGBEE_JOINED_NETWORK：networkState 的"已在网络里"（0 = SL_ZIGBEE_NO_NETWORK，:941） */

/* 策略（表 8.5-2）。setPolicy 的 ID 与取值都出自 ezsp-enum.h（行号由 test_host/run.sh 对着 SDK 核）。三条有出处的选择：
   - TC_KEY_REQUEST 允许并回当前密钥、APP_KEY_REQUEST 拒绝：bellows 与 zigbee-herdsman ember 一致（Zigbee 3.0 设备入网后要申请唯一链路密钥）；
   - 信任中心窗口外 ALLOW_UNSECURED_REJOINS（只许重新入网）：SDK 框架关窗口就设这个（network-creator-security.c:328 →
     af-trust-center.c:227-228 "Allow rejoins only"），herdsman permitJoin(0) 同。bellows 窗口外留着 ALLOW_JOINS（0x01），与设计
     "只在窗口内允许新设备加入"相反，不照搬。有网络密钥的重新入网不发密钥、不看这条策略（SDK SoC 分支
     af-trust-center.c:284 SECURED_REJOIN 一律 NO_ACTION），它管的是"不安全的（TC）重新入网"：错过换密钥的设备靠它回来；
   - 用公开的 ZigBeeAlliance09 链路密钥做 TC 重新入网：不许（0）。上一条放行了"不安全的重新入网"，这一条为假它才安全：为真时冒用
     一个已知 EUI64 做 TC 重新入网的设备，就能拿到用公开密钥加密的网络密钥（第二轮审计）。NCP 默认就是不许、允许了也会超时自动关回
     （network-formation.h:482-490），bellows / herdsman 都不写这条：写死 0 = 这张网一直以来的行为，只是不再依赖默认；
   - 其余三条（不许远端改绑定表、messageSent 只带 tag、子设备轮询不报）都是 NCP 默认值，写出来是为了不依赖默认。 */
#define ZH_EZSP_POLICY_TRUST_CENTER                0x00u  /* ezsp-enum.h:396 SL_ZIGBEE_EZSP_TRUST_CENTER_POLICY */
#define ZH_EZSP_POLICY_BINDING_MODIFICATION        0x01u  /* ezsp-enum.h:398 SL_ZIGBEE_EZSP_BINDING_MODIFICATION_POLICY */
#define ZH_EZSP_POLICY_POLL_HANDLER                0x03u  /* ezsp-enum.h:402 SL_ZIGBEE_EZSP_POLL_HANDLER_POLICY */
#define ZH_EZSP_POLICY_MESSAGE_CONTENTS_IN_CALLBACK 0x04u /* ezsp-enum.h:405 SL_ZIGBEE_EZSP_MESSAGE_CONTENTS_IN_CALLBACK_POLICY */
#define ZH_EZSP_POLICY_TC_KEY_REQUEST              0x05u  /* ezsp-enum.h:408 SL_ZIGBEE_EZSP_TC_KEY_REQUEST_POLICY */
#define ZH_EZSP_POLICY_APP_KEY_REQUEST             0x06u  /* ezsp-enum.h:411 SL_ZIGBEE_EZSP_APP_KEY_REQUEST_POLICY */
#define ZH_EZSP_POLICY_TC_REJOINS_USING_WELL_KNOWN_KEY 0x09u /* ezsp-enum.h:421 SL_ZIGBEE_EZSP_TC_REJOINS_USING_WELL_KNOWN_KEY_POLICY */
#define ZH_EZSP_DECISION_ALLOW_UNSECURED_REJOINS   0x02u  /* ezsp-enum.h:437 SL_ZIGBEE_EZSP_DECISION_ALLOW_UNSECURED_REJOINS（信任中心策略的位图） */
#define ZH_EZSP_DECISION_DISALLOW_BINDING_MODIFICATION 0x10u /* ezsp-enum.h:456 SL_ZIGBEE_EZSP_DISALLOW_BINDING_MODIFICATION */
#define ZH_EZSP_DECISION_POLL_HANDLER_IGNORE       0x30u  /* ezsp-enum.h:473 SL_ZIGBEE_EZSP_POLL_HANDLER_IGNORE */
#define ZH_EZSP_DECISION_MESSAGE_TAG_ONLY_IN_CALLBACK 0x40u /* ezsp-enum.h:479 SL_ZIGBEE_EZSP_MESSAGE_TAG_ONLY_IN_CALLBACK */
#define ZH_EZSP_DECISION_ALLOW_TC_KEY_REQUESTS_AND_SEND_CURRENT_KEY 0x51u /* ezsp-enum.h:490 SL_ZIGBEE_EZSP_ALLOW_TC_KEY_REQUESTS_AND_SEND_CURRENT_KEY */
#define ZH_EZSP_DECISION_DENY_APP_KEY_REQUESTS     0x60u  /* ezsp-enum.h:499 SL_ZIGBEE_EZSP_DENY_APP_KEY_REQUESTS */
#define ZH_HOST_TC_POLICY_CLOSED ZH_EZSP_DECISION_ALLOW_UNSECURED_REJOINS   /* 入网窗口外的信任中心策略 */
#define ZH_EZSP_DECISION_ALLOW_JOINS               0x01u  /* ezsp-enum.h:435 SL_ZIGBEE_EZSP_DECISION_ALLOW_JOINS —— 只在窗口里 */
/* 入网窗口里的信任中心策略：放新设备 + 不安全的重新入网。SDK 框架开窗（USE_PRECONFIGURED_KEY → af-trust-center.c:221-222、
   network-creator-security.c:393）与 herdsman 开窗（emberSetJoinPolicy(USE_PRECONFIGURED_KEY)）都是这一组 */
#define ZH_HOST_TC_POLICY_OPEN (ZH_EZSP_DECISION_ALLOW_JOINS | ZH_EZSP_DECISION_ALLOW_UNSECURED_REJOINS)
/* 入网窗口导入的临时链路密钥：通配 EUI64（谁来都用它）+ Zigbee 3.0 集中式安全的公开密钥（SDK util.h:66-70 ZIGBEE_3_CENTRALIZED_SECURITY_LINK_KEY）。
   不是秘密（规范公开），窗口关时 clearTransientLinkKeys 清掉，NCP 自己也 300 s 过期（sl_zigbee_configuration_defaults.h:582，长于窗口上限） */
#define ZH_HOST_TRANSIENT_EUI64     { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }
#define ZH_HOST_WELL_KNOWN_LINK_KEY { 0x5A, 0x69, 0x67, 0x42, 0x65, 0x65, 0x41, 0x6C, 0x6C, 0x69, 0x61, 0x6E, 0x63, 0x65, 0x30, 0x39 }
#define ZH_HOST_TC_WELL_KNOWN_REJOINS 0u    /* 这条策略的决策值就是 bool allow（SDK 主机侧 ezsp.c:847-849），0 = 不许 */

/* 端点 1（表 8.5-3；簇表在 zh_host_core.c 的 k_ep_in / k_ep_out）。设备类型 IAS CIE：网关在 IAS 里就是 CIE（8.6），zigpy 也注册成它
   （zigpy/application.py register_endpoints，DeviceType.IAS_CONTROL = 0x0400，版本 0）；herdsman 用的 0x0065 它自己注释"?"。 */
#define ZH_HOST_ENDPOINT    1u
#define ZH_HOST_EP_PROFILE  0x0104u   /* Home Automation */
#define ZH_HOST_EP_DEVICE   0x0400u   /* IAS Control and Indicating Equipment */
#define ZH_HOST_EP_VERSION  0u

/* 多对一路由（集中器）。类型 HIGH_RAM：NCP 自己有源路由表（mg24/ncp/bitengine_ncp.slcp 的 zigbee_source_route），能记下整张网的路由；
   bellows 与 herdsman 都用 HIGH_RAM。时间与阈值取 SDK 集中器插件的默认值（concentrator-config.h；herdsman 注释里 Silicon Labs 的 zigpc
   也是这组，herdsman 只把最短间隔改成 5 s）。⚠️ 不照 bellows 的 60 / 3600 / 8 / 8：zigpy 默认根本不开集中器（source_routing 默认
   False → bellows 发 setConcentrator(off)），那组值在这张网上从没生效过。
   发现模式 RESCHEDULE：立刻发一次 MTORR 再按周期（sl_zigbee_types.h:3043）—— NCP 刚启动源路由表是空的（herdsman 同；bellows 用 ON）。 */
#define ZH_HOST_CONC_TYPE          0xFFF9u  /* sl_zigbee_types.h:1032 SL_ZIGBEE_HIGH_RAM_CONCENTRATOR */
#define ZH_HOST_CONC_MIN_S         10u      /* concentrator-config.h:32 MIN_TIME_BETWEEN_BROADCASTS_SECONDS */
#define ZH_HOST_CONC_MAX_S         60u      /* concentrator-config.h:37 MAX_TIME_BETWEEN_BROADCASTS_SECONDS */
#define ZH_HOST_CONC_ROUTE_ERR     3u       /* concentrator-config.h:42 ROUTE_ERROR_THRESHOLD */
#define ZH_HOST_CONC_DELIVERY_FAIL 1u       /* concentrator-config.h:47 DELIVERY_FAILURE_THRESHOLD */
#define ZH_HOST_CONC_MAX_HOPS      0u       /* concentrator-config.h:52 MAX_HOPS（0 = 协议栈按网络深度算） */
#define ZH_HOST_MTORR_MODE         0x02u    /* sl_zigbee_types.h:3058 SL_ZIGBEE_SOURCE_ROUTE_DISCOVERY_RESCHEDULE */

/* networkInit 的位图：PARENT_INFO_IN_TOKEN 0x0001 | END_DEVICE_REJOIN_ON_REBOOT 0x0002（sl_zigbee_types.h:642、644；设计 8.5，
   照 zigbee-herdsman ember）。两位都只对终端设备有意义，协调器上无副作用。 */
#define ZH_HOST_NET_INIT_BITMASK 0x0003u   /* sl_zigbee_types.h:642、644 */

#define ZH_HOST_STACK_UP_WAIT_MS 10000u   /* networkInit 回 OK 之后等 stackStatusHandler(NETWORK_UP) 多久（设计 8.4："最长 10 s"） */
#define ZH_HOST_STABLE_MS        60000u   /* 连续 RUNNING 这么久，连续失败次数清零 */
#define ZH_HOST_NET_DOWN_MS      60000u   /* 网络 down（等不到 UP / 报 NETWORK_DOWN）多久还没回来就记失败、复位重来（8.13"网络 down：等 NCP 自己恢复"） */
#define ZH_HOST_FAILING_AT       4u       /* 连续失败到这么多次（= 立即重来 + 3 次退避都没好）快照里置 failing */
#define ZH_HOST_UP_RETRY_MS      5000u    /* UP 之后的某一步没成（回显用完 / 被拒 / 解不开）多久再试同一步：尽力而为，不复位链路 */
#define ZH_HOST_WIN_RETRY_MS     5000u    /* 关窗时"写回信任中心策略"被拒：多久再试（这一步必须成功） */

/* 入网窗口与 ZDO（设计 8.6；出处都是 SDK 头文件，行号由 test_host/run.sh 核） */
#define ZH_HOST_PERMIT_MAX_S       254u     /* af-types.h:2027 SL_ZIGBEE_AF_PERMIT_JOIN_MAX_TIMEOUT（255 = 永久开放，编码器拒；SDK 框架同样夹到 254，af-common.c:739-742） */
#define ZH_ZDO_PROFILE             0x0000u  /* sl_zigbee_types.h:425 SL_ZIGBEE_ZDO_PROFILE_ID */
#define ZH_ZDO_ENDPOINT            0u       /* sl_zigbee_types.h:410 SL_ZIGBEE_ZDO_ENDPOINT */
#define ZH_ZDO_BCAST_ROUTERS       0xFFFCu  /* sl_zigbee_types.h:549 SL_ZIGBEE_BROADCAST_ADDRESS（全部路由器与协调器） */
#define ZH_NULL_NODE_ID            0xFFFFu  /* sl_zigbee_types.h:346 SL_ZIGBEE_NULL_NODE_ID（sendBroadcast 的 alias：不用别名） */
#define ZH_ZDO_PERMIT_JOINING_REQ  0x0036u  /* zigbee-device-stack.h:604 PERMIT_JOINING_REQUEST */
#define ZH_ZDO_DEVICE_ANNCE        0x0013u  /* zigbee-device-stack.h:361 END_DEVICE_ANNOUNCE */
#define ZH_ZDO_RADIUS              255u     /* zigbee-device-common.c:24 zigDevRequestRadius（SDK 主机发 ZDO 广播用的半径） */
#define ZH_ZDO_TC_SIGNIFICANCE     1u       /* af-common.c:1333：Mgmt_Permit_Joining_req 的 TC significance，SDK 框架"always 1" */
#define ZH_ZDO_APP_SEQ_MASK        0x7Fu    /* zigbee-device-common.c:30 APPLICATION_ZDO_SEQUENCE_MASK：应用的 ZDO 序号用 0–127，128–255 归协议栈 */
#define ZH_HOST_PERMIT_HOUR_CAP_S  600u     /* 总时长上限（用户 2026-10-22 定按默认做）：滚动 ZH_HOST_PERMIT_HOUR_MS 里窗口最多开这么久 —— 254 s 一到期就能
                                               再开，智能体循环调用能让网络一直开放入网（第六步审计）；10 分钟够配好几台设备 */
#define ZH_HOST_PERMIT_HOUR_MS     3600000u
#define ZH_HOST_PERMIT_TRIM_MIN_S  30u      /* 剩的额度不够这么久就不开（回 ZH_PJ_QUOTA）；够就开、截到剩的 */
#define ZH_HOST_WIN_HIST           24u      /* 开窗记录的环：一小时里最多开这么多次（早关 / 开失败的记录只有几秒，光按时长算环会转满、把一小时内的
                                               大记录盖掉 —— 审计 ❌：那样一小时能开到约 858 s；所以要盖的那条还在一小时内就回 QUOTA） */
#define ZH_HOST_WIN_POLICY_TRIES   6u       /* 关窗时写回信任中心策略连着被拒这么多次（约 30 s）：复位链路 —— NCP 重启、k_init 把策略写回窗口外 */

typedef enum { ZH_WIN_CLOSED = 0, ZH_WIN_OPENING, ZH_WIN_OPEN, ZH_WIN_CLOSING } zh_win_state_t;
/* zh_host_permit_join 的结果 */
typedef enum {
    ZH_PJ_OK = 0,           /* 开始开窗 / 关窗了（结果看 win_state） */
    ZH_PJ_ALREADY_OPEN,     /* 已经开着（或正在开）：不重开、不续时（6.5 注记"窗口开着时不再重开"） */
    ZH_PJ_ALREADY_CLOSED,   /* 要关，本来就关着 / 正在关 */
    ZH_PJ_NOT_READY,        /* 不在 RUNNING 或网络不是 UP */
    ZH_PJ_BUSY,             /* 正在关窗，关完才能再开 */
    ZH_PJ_INVALID,          /* seconds > ZH_HOST_PERMIT_MAX_S */
    ZH_PJ_QUOTA,            /* 滚动一小时里开够了 ZH_HOST_PERMIT_HOUR_CAP_S（剩的不够 ZH_HOST_PERMIT_TRIM_MIN_S）：zh_host_quota_wait_s 说还要等多久 */
} zh_pj_result_t;
typedef struct { uint32_t t_start; uint16_t secs; bool valid; } zh_win_rec_t;   /* 一次开窗：开始时刻、开了多久（早关的改成真开的时长） */

typedef enum {
    ZH_HOST_IDLE = 0,    /* 没启动 / 已挂起：不收不发（UART 不归我们） */
    ZH_HOST_RESETTING,   /* ASH RST 已排，等 RSTACK */
    ZH_HOST_HANDSHAKE,   /* ASH 连上，会话握手（version） */
    ZH_HOST_NET_INIT,    /* networkInit 在途 / 等 NETWORK_UP */
    ZH_HOST_RUNNING,     /* 运行：会话保活、取回调 */
    ZH_HOST_BACKOFF,     /* 失败后等退避到期（ASH 已静音） */
} zh_host_state_t;

typedef enum {
    ZH_NET_UNKNOWN = 0,  /* 还没走到 networkInit（复位后 NCP 的协议栈是未初始化的） */
    ZH_NET_UP,           /* stackStatusHandler 报过 NETWORK_UP */
    ZH_NET_DOWN,         /* networkInit 失败、等不到 NETWORK_UP、或运行中报 NETWORK_DOWN */
    ZH_NET_NONE,         /* networkInit 回 NOT_JOINED：NCP 里没有网络（不自动建网） */
} zh_host_net_t;

typedef enum {
    ZH_HOST_FAIL_NONE = 0,
    ZH_HOST_FAIL_NO_RSTACK,     /* ASH：等不到合格的 RSTACK（code 0） */
    ZH_HOST_FAIL_ASH_VERSION,   /* ASH：RSTACK 的版本不是 2（code = 版本） */
    ZH_HOST_FAIL_NCP_RESET,     /* ASH：连上后 NCP 自己重启了（code = 复位原因） */
    ZH_HOST_FAIL_NCP_ERROR,     /* ASH：ERROR 帧（code = 错误码） */
    ZH_HOST_FAIL_ASH_TIMEOUTS,  /* ASH：ACK 连续超时到上限 */
    ZH_HOST_FAIL_SESSION,       /* 会话：RESET_LINK（code = zh_ses_why_t） */
    ZH_HOST_FAIL_EZSP_VERSION,  /* 会话：协议号不是 0x12（code = NCP 回的协议号） */
    ZH_HOST_FAIL_NET_INIT,      /* networkInit 或它之前的初始化序列没成（code：zh_ses_result_t；0x100 + 解码错误码的绝对值；networkInit 回错误码为
                                   0x10000 | sl_status_t；初始化序列某一步被拒为 0x20000 | 步号 << 8 | sl_status_t 低字节，步号 = k_init 的下标） */
    ZH_HOST_FAIL_NET_DOWN,      /* 网络 ZH_HOST_NET_DOWN_MS 内没起来 / 没回来（code = networkState 的值，查不到为 0xFF） */
    ZH_HOST_FAIL_WIN_POLICY,    /* 关入网窗口时写回信任中心策略连着被拒 ZH_HOST_WIN_POLICY_TRIES 次（code = NCP 回的 sl_status_t 低 16 位）：复位让 NCP 重启 */
    ZH_HOST_FAIL_MAX,
} zh_host_fail_t;

typedef struct {
    void (*on_net)(void *ctx, zh_host_net_t net);                /* 网络状态变了（可为 NULL；事件出口随 Z41） */
    void (*on_callback)(void *ctx, const zh_ezsp_frame_t *fr);   /* 每条回调（本层先记账再交；可为 NULL：上层设备层随后几步接） */
    void *ctx;
    /* 应用命令（zh_host_app_submit 交的）的结果：tag 是调用方给的那个；fr 只在 r == OK / INVALID 时可能有，其余为 NULL（会话不收的帧回 INVALID、fr 为 NULL）。
       会话自己判链路不可用时，在途的回 TIMEOUT / UNKNOWN / LINK_DOWN；编排层主动收摊（失败复位、挂起、外部重启）时排队的与在途的都不回 —— 调用方两种都要能处理、自己设期限 */
    void (*on_app_result)(void *ctx, uint32_t tag, zh_ses_result_t r, const zh_ezsp_frame_t *fr);
} zh_host_ops_t;

#define ZH_HOST_APP_Q        6u          /* 应用命令队列（设备层的地址查询、ZDO，以后的 ZCL）：want 槽与入网窗口都空着才交，优先级最低 */
#define ZH_HOST_APP_TAG_MAX  0x00FFFFFFu /* 调用方的 tag 只能用低 24 位（高 8 位以上本层占来区分命令种类） */
typedef struct { uint8_t f[ZH_EZSP_MAX]; uint8_t n; bool idem; uint32_t tag; } zh_host_app_t;

/* UP 之后的序列（尽力而为，见文件头）：下标也是 st.up_ok / up_bad 的下标 */
typedef enum {
    ZH_HOST_UP_PARAMS = 0,    /* getNetworkParameters */
    ZH_HOST_UP_EUI64,         /* getEui64 */
    ZH_HOST_UP_CONCENTRATOR,  /* setConcentrator */
    ZH_HOST_UP_MTORR,         /* setSourceRouteDiscoveryMode */
    ZH_HOST_UP_N,
} zh_host_up_step_t;

typedef struct {
    uint32_t starts;                     /* zh_host_start 次数（开机 + 每次恢复） */
    uint32_t resets;                     /* 发起的 ASH 复位 */
    uint32_t readies;                    /* 握手完成 */
    uint32_t fails[ZH_HOST_FAIL_MAX];    /* 按原因 */
    uint32_t net_ups, net_downs;         /* 进入 UP / DOWN 的次数 */
    uint32_t net_inits;                  /* 交出去的 networkInit（每次 NCP 启动至多一次） */
    uint32_t net_states;                 /* 交出去的 networkState */
    uint32_t callbacks, stack_status;    /* 收到的回调 / 其中 stackStatusHandler */
    uint32_t bad_callbacks;              /* 解不开的 stackStatusHandler */
    uint32_t up_ok[ZH_HOST_UP_N];        /* UP 之后的序列：每步成功 / 没成（被拒、没读到、结果说不清） */
    uint32_t up_bad[ZH_HOST_UP_N];
    uint32_t win_opens, win_open_fails;  /* 入网窗口：开成的 / 开的某一步没成（随即关窗复原） */
    uint32_t win_closes, win_expired;    /* 关完的 / 其中到期关的 */
    uint32_t win_close_retries;          /* 关窗时写回信任中心策略被拒、重试的次数 */
    uint32_t win_skipped;                /* 关窗时 permitJoining(0) / 广播 / 清密钥没成、跳过的步数 */
    uint32_t app_submits, app_full;      /* 应用命令：收下的 / 队列满或不在 RUNNING 拒收的 */
    uint32_t app_bad;                    /* 会话不收的帧（丢掉、回 ZH_SES_R_INVALID 且 fr 为 NULL） */
    uint32_t init_opt_rejects;           /* k_init 里可选项被 NCP 拒的次数（MAC 间接发送超时等；0 = 都收下了） */
    uint32_t init_opt_last;              /* 最近一次被拒的：步号 << 8 | 状态低字节 */
    uint32_t restarts;                   /* 外部要求的立即复位（zh_host_restart） */
} zh_host_stats_t;

enum { ZH_HOST_WANT_NONE = 0, ZH_HOST_WANT_NET_INIT, ZH_HOST_WANT_NET_STATE, ZH_HOST_WANT_INIT, ZH_HOST_WANT_UP };

typedef struct {
    zh_ash_t        ash;
    zh_ses_t        ses;
    zh_host_ops_t   ops;
    zh_host_state_t state;
    zh_host_net_t   net;
    uint32_t        t_now;             /* 最近一次入口给的时刻（ASH 的收帧回调不带时间，会话要用） */
    /* 回调里记下、tick 里处理 */
    bool            pend_connected;
    bool            pend_fail;
    zh_host_fail_t  pend_why;
    uint32_t        pend_code;
    bool            teardown;          /* 自己调 zh_ses_link_down 期间：交回的结果不算失败 */
    uint8_t         want;              /* 下一条要交的初始化命令（ZH_HOST_WANT_*） */
    uint8_t         init_step;         /* 初始化序列走到第几步（k_init 的下标） */
    uint8_t         up_step;           /* UP 之后的序列走到第几步（zh_host_up_step_t；= ZH_HOST_UP_N 走完了） */
    bool            got_up;            /* 这一轮链路里见过 NETWORK_UP（可能先于 networkInit 的应答到） */
    bool            wait_up;           /* networkInit 回了 OK，正在等 NETWORK_UP */
    bool            timer_on;          /* t_timer 的含义随状态：BACKOFF 到期 / NET_INIT 等 UP 到期 / RUNNING 网络 down 的期限 */
    bool            down_check;        /* 这次 networkState 是"down 期限到了"的那一查：还没在网络里就记 NET_DOWN */
    uint32_t        t_timer;
    bool            up_retry;          /* UP 之后的某一步没成：t_up 到了再试同一步 */
    uint32_t        t_up;
    uint32_t        t_running;         /* 进入 RUNNING 的时刻（稳定计时） */
    uint32_t        fails;             /* 连续失败次数 */
    zh_host_fail_t  last_fail;
    uint32_t        last_fail_code;
    uint32_t        last_net_status;   /* 最近一次 networkInit / stackStatusHandler 的状态码 */
    bool            have_version;
    uint32_t        version_code;      /* 会话 READY 的 code：stackType << 16 | stackVersion */
    bool            have_params;
    uint8_t         node_type;
    zh_ezsp_net_params_t params;
    bool            have_eui64;
    uint8_t         eui64[8];          /* 协调器自己的 EUI64（小端，同线上字节序） */
    bool            concentrator;      /* 这次 UP 之后 setConcentrator 回了 OK */
    bool            mtorr;             /* 这次 UP 之后 setSourceRouteDiscoveryMode 有应答 */
    uint32_t        mtorr_next_ms;     /* 它回的"距下一次 MTORR 广播的毫秒数" */
    /* 入网窗口（文件头"入网窗口"一段） */
    uint8_t         win_state;         /* zh_win_state_t */
    uint8_t         win_step;          /* 开 / 关序列走到第几步 */
    uint8_t         win_s;             /* 这次开多少秒 */
    bool            win_want;          /* 下一步待交（want 槽空着时交） */
    bool            win_close_pending; /* 正在开的时候有人要关：开完马上关 */
    bool            win_retry;         /* 关窗时写回信任中心策略被拒：t_win_retry 再试 */
    uint8_t         win_policy_tries;  /* 这次关窗写回策略被拒了几次（到 ZH_HOST_WIN_POLICY_TRIES 复位链路） */
    bool            ncp_open;          /* NCP 报过 NETWORK_OPENED、还没报 NETWORK_CLOSED */
    uint8_t         zdo_seq;           /* ZDO 请求的序号（帧里第 0 字节，也当 messageTag） */
    uint32_t        t_win_end, t_win_retry;
    zh_win_rec_t    win_hist[ZH_HOST_WIN_HIST];   /* 开窗记录（总时长上限用；链路复位不清，zh_host_init 才清） */
    uint8_t         win_hist_head;     /* 下一条写哪 */
    uint8_t         win_rec_cur;       /* 这次窗口的记录（0xFF = 没有） */
    zh_host_app_t   app[ZH_HOST_APP_Q];/* 应用命令队列（环） */
    uint8_t         app_head, app_n;
    uint32_t        win_last_err;      /* 最近一次开窗失败：步号 << 24 | zh_ses_result_t << 16 | sl_status_t 低 16 位（解不开为 0xFFFF） */
    zh_host_stats_t st;
} zh_host_t;

/* 清零、装好 ASH 与会话（默认参数），状态 IDLE。ops 可为 NULL。 */
void zh_host_init(zh_host_t *h, const zh_host_ops_t *ops);
/* 开始 / 恢复：连续失败清零，立即 ASH RST 走完整初始化。任何状态都可以调（包括正在运行 —— 那等于从头来）。 */
void zh_host_start(zh_host_t *h, uint32_t now_ms);
/* 停下（挂起、交出 UART 之前）：会话收摊（在途命令按"结果的确切含义"了结）、ASH 静音，状态 IDLE。之后不收不发，直到 zh_host_start。 */
void zh_host_stop(zh_host_t *h);
/* 外部要求立即复位（看门狗的定向恢复、将来的 /zigbee/restart）：跳过退避、不计失败。IDLE 时无效。 */
void zh_host_restart(zh_host_t *h, uint32_t now_ms);
/* UART 收到的字节（IDLE 时丢弃） */
void zh_host_rx(zh_host_t *h, const uint8_t *buf, size_t n, uint32_t now_ms);
/* 处理记下的链路事件、定时器，交下一条初始化命令，再跑会话的 tick。每次 rx / tx 之后都调。 */
void zh_host_tick(zh_host_t *h, uint32_t now_ms);
/* 要写 UART 的字节（同 zh_ash_tx：cap 至少 ZH_ASH_WIRE_MAX；IDLE / 退避时回 0）。可能触发 ASH 事件 → 之后要 tick。 */
size_t zh_host_tx(zh_host_t *h, uint32_t now_ms, uint8_t *out, size_t cap);
/* 最多多久之后该再 rx / tick / tx 一轮；有事待办回 0；IDLE 回 UINT32_MAX（调用方自己夹上限，看门狗心跳别绑在这上面） */
uint32_t zh_host_next_timeout_ms(const zh_host_t *h, uint32_t now_ms);
/* NCP 应用层还活着的正向证据（= zh_ses_alive_evidence：配上的应答 / 合法回调；Z47） */
bool zh_host_alive_evidence(const zh_host_t *h, uint32_t *t_ms);
/* "某个时刻以来"的证据：那一刻先记 mark = zh_host_evidence_mark()，之后用 zh_host_evidence_since(mark) 问。比的是会话的正向证据计数，
   **不比时间戳**（持有 UART 满 2^31 ms 后两个时间戳的差值会变号 —— 第一轮审计复现：约 24.8 天后看门狗误判 MG24 已死）。
   有新证据时返回 true，age_ms = 距最近一次证据（证据是新的，差值远小于 2^31，回绕安全）。 */
static inline uint32_t zh_host_evidence_mark(const zh_host_t *h) { return h->ses.st.responses; }
bool zh_host_evidence_since(const zh_host_t *h, uint32_t mark, uint32_t now_ms, uint32_t *age_ms);
/* 应用命令（上层设备层用）：frame 是编好的整帧（seq 字节随便，会话发的时候改写），tag ≤ ZH_HOST_APP_TAG_MAX。只在 RUNNING 收；
   队列满 / 不在 RUNNING / 帧太长回 false（调用方过一会儿再试）。结果经 ops.on_app_result 回。 */
bool zh_host_app_submit(zh_host_t *h, const uint8_t *frame, size_t n, uint32_t tag, bool idempotent);   /* idempotent 与 zh_ses_idempotent(帧 ID) 取与：哪些命令能重发只有会话层那一处正本 */
/* 取一个 ZDO 序号（入网窗口的广播与设备层的 ZDO 请求共用一个计数；应用侧 0–127，ZH_ZDO_APP_SEQ_MASK） */
uint8_t zh_host_next_zdo_seq(zh_host_t *h);
/* 入网窗口：seconds 0 = 关，1..ZH_HOST_PERMIT_MAX_S = 开这么多秒（上层按附录 A 再限 30 起）。只改状态、交命令在 tick 里 */
zh_pj_result_t zh_host_permit_join(zh_host_t *h, uint32_t seconds, uint32_t now_ms);
/* 总时长上限：滚动一小时里还能开多少秒 / 还要等多少秒才能再开（够 ZH_HOST_PERMIT_TRIM_MIN_S 就回 0） */
uint32_t zh_host_quota_left_s(const zh_host_t *h, uint32_t now_ms);
uint32_t zh_host_quota_wait_s(const zh_host_t *h, uint32_t now_ms);
/* 把满一小时的开窗记录作废。zhub 任务每圈都调（挂起时也调）：记录的时刻是 32 位毫秒，约 49.7 天回绕一次，不作废的老记录会"返老还童"、
   被当成一小时内的再算一遍（审计 ❌）。只要每 49 天里看到过它一次就够 */
void zh_host_expire_records(zh_host_t *h, uint32_t now_ms);
/* 窗口开着时还剩多少秒（向上取整）；不在 OPEN 回 0 */
uint32_t zh_host_window_left_s(const zh_host_t *h, uint32_t now_ms);
const char *zh_host_win_name(zh_win_state_t w);
/* 连续失败到 ZH_HOST_FAILING_AT 次了 */
static inline bool zh_host_failing(const zh_host_t *h) { return h->fails >= ZH_HOST_FAILING_AT; }
const char *zh_host_state_name(zh_host_state_t s);
const char *zh_host_net_name(zh_host_net_t n);
const char *zh_host_fail_name(zh_host_fail_t f);
