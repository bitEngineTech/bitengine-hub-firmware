#pragma once
/* zh_dev —— zhub 的设备层，纯逻辑（docs/15_muse_direct_design_v1.md 8.6 入网回调、8.9 设备库、8.12 ③ 被动学习；M4 第七步 a）。
 * 不依赖 ESP-IDF、不分配内存：test_host/ 原样编它。
 *
 * 管一张设备表（最多 ZH_DEV_MAX 台，下标就是存储键 dNN 的 NN）：
 *  - 建 / 改记录的来源：信任中心放行的入网与重新入网（trustCenterPostJoinHandler；**拒绝的不建** —— 信任中心每个决定都回调，第六步两轮审计 ❌）、
 *    ZDO Device_annce（nwk / IEEE / 能力字节）、被动学习。离开（DEVICE_LEFT）只标 left，不删记录（8.6）。
 *  - 被动学习（8.12 ③）：收到不认识的 nwk 发来的消息 → 先问 NCP 的地址表（lookupEui64ByNodeId，本地、快）→ 查不到再给它发 ZDO IEEE_addr_req
 *    （单播，休眠设备由父节点缓存到它下次轮询）→ 等 IEEE_addr_rsp。同一个 nwk 两次查询至少隔 ZH_DEV_LOOKUP_GAP_MS；同时在查的最多 ZH_DEV_LOOK_N 个。
 *    现网设备（从 HA 型换固件过来）就是这样"一说话就出现"的。
 *  - 只在结构变化时落盘（8.9：入网、nwk 变了、能力字节变了、离开 / 回来、访谈完成）；last_seen / 信号只在内存（开机后靠上报重建）。
 *  - 访谈（8.6；第七步 b）：NODE_DESC → ACTIVE_EP → 前 ZH_DEV_EP_REC 个端点各一条 SIMPLE_DESC → 读 Basic（厂商 / 型号 / 供电 / 软件版本）→ 有 IAS Zone 的再读 ZoneType。
 *    同时只有一条访谈请求在途（各设备的进度各自记，轮流推进）；休眠设备只在它刚发过消息之后（ZH_IV_AWAKE_MS 内）发下一条；每步最多 ZH_IV_TRIES 次、
 *    每次等 ZH_IV_STEP_TIMEOUT_MS；从第一条请求起 ZH_IV_TOTAL_MS 还没完就算失败（设备照样列出；它重新宣告、或失败 ZH_IV_RETRY_MS 之后再说话时重来）。
 *    网络不是 UP 时不推进；请求没发出去（sendUnicast 被拒）也照样等到这一次的期限再重发，不连发。
 *    Basic 两个属性一组读（一次读四个最坏 103 B，超过 APS 载荷上限；zigpy endpoint.py:188-197 同法），缺了的 / 回 0x89 的改成一个一个读。
 *    MAP（能力映射 8.8）在 hub_core（hub_zcap）。CONFIGURE 第一步（M5 第四步）：有 IAS Zone 的，读完 ZoneType 再写 IAS_CIE_ADDRESS = 网关自己的 EUI64
 *    （ZH_IV_CIE，照 SDK ias-zone-client.c 的 setCieAddress），写成了进 ZH_IV_ENROLL：发登记应答（自动登记）并回读 ZoneState，读到"已登记"才算完，
 *    否则连同登记应答一起重发（只认自动登记应答的设备，这条应答是它唯一的登记途径，丢了不能不管 —— 第二轮审计 ❌）。不写的话经 zhub 新入网的 IAS 设备
 *    没登记、不报 Zone Status。写不成 / 不回 / 没确认 / 还不知道自己的 EUI64 都不算访谈失败：记 ZH_DEVF_CIE_OWED，之后在它重新宣告、重新入网、
 *    发来登记请求（这三种距上次没成至少 ZH_CIE_RESUME_GAP_MS）、距上次 ZH_IV_RETRY_MS 后再说话、或网关读到自己的 EUI64 时只补这两步
 *    （不重新访谈、不出事件；第一轮审计 ❌）。设备明确回失败（如只读）不再补。
 *    CONFIGURE 第二步（M5 第五步）：IAS 那几步之后，逐个簇绑定到网关（ZH_IV_BIND）、配属性上报（ZH_IV_REPORT），簇与间隔按设计表 8.6-1，
 *    只配 zhub 解得开读数的那几个簇（zh_dev.c 的 k_cfg）。**只对这次开机经入网窗口新加 / 重新配对的设备做**：迁移来的设备里早有 ZHA / Z2M
 *    配好的绑定与上报（存在设备自己的 NVM 里），不去改它的上报节奏、也不在迁移后头一次开机一下子发几百条。尽力而为：失败 / 不回 /
 *    30 min 期限到都不算访谈失败，只计数（bind_* / report_*），不补做。**先算访谈完、推送 IDENTIFIED，再配**（配置是不出事件的后续阶段，第二轮审计 ❌）；
 *    新入网的设备只要还在说话，"当醒着"的 ZH_IV_JOIN_AWAKE_MS 就随它最近一条消息顺延。
 *  - 开机补读电量（M5 第六步，用户 10-23 定）：访谈完、不是路由器、有电源配置簇、这次开机还没有电量读数的设备，在它刚说完话还醒着时
 *    读一次电量 % 与电压（应答走读数路径）。迁移来的设备上报间隔由前主机定，常常几小时一次，重启后列表里会长时间没有电量。
 *    每次开机最多 ZH_BATT_READ_TRIES 次、两次至少隔 ZH_BATT_READ_GAP_MS；一拍最多发一条；网络不是 UP 不发；交不出去不占次数。
 *  - 状态（M5 第一步，只读）：属性上报 / 读属性应答 / IAS 的 Zone Status Change 通知更新内存里的读数（zh_state_t，不落盘，开机后靠上报重建）；
 *    报警类字段（IAS 的 alarm1 / 防拆 / 电量低、占用）翻转时发 ZH_DEV_EV_STATE，st_ev 说发生了什么（hub_zigbee 按 ZoneType 渲染成句子）。
 *    IAS 设备发 Zone Enroll Request 就回 Enroll Response（成功、zone id = 下标；设计 8.6 IAS 第 2 条）：迁移过来的设备重新登记时得有人应。
 *  - 涂鸦私有簇 0xEF00（M5 第八步，设计 8.8 注记"社区设备库"）：访谈完、厂商名 + 型号在社区设备库导出的表里（zh_tuya）的设备，
 *    数据点按表换成读数，复用上面同一套读数与事件：开关 / 亮度 / 温湿度 / 照度直接进读数；电量 % 走 st_batt_pct（降到 15 % 报一次、回到 20 % 才重新算）；
 *    占用走 st_occ；门磁 / 漏水 / 烟雾 / 燃气 / CO / 振动里的主报警字段（zh_tbl_alarm_field）连同防拆、电量低合成 IAS ZoneStatus 走 st_zone_from。
 *    有效 ZoneType 由 zh_dev_zone_type 一处算（IAS 设备看记录、涂鸦设备看表）。主报警还没报过时，电量低 / 防拆只出事件、不置 ZONE 读数（不假装门是关着的）。
 *  - 小米 / Aqara（M5 第十步）：小米休眠设备几乎不应访谈，型号从它主动发的 Basic 上报（0x0004 / 0x0005）学进记录（st_learn_str；
 *    访谈只在重新配对时清厂家 / 型号）。型号在社区设备库导出的小米表里（zh_lumi）的：Basic 0xFF01 与 0xFCC0 簇（0x00F7 的 TLV、别的属性）按表换成读数，
 *    标准簇的改写（门磁的 OnOff 是开合、人体的照度直接是 lux、灯的 Occupancy 丢掉）在 st_apply 开头先查表；读数与报警合成和涂鸦走同一套
 *    （st_tbl_val / st_tbl_zone）；电池电压按表里的曲线换成电量 %（st_batt_mv）；人体只报"有人"的，表给的秒数内没再报就自动报没人（t_occ_clear）。
 *    每次开机每台打一轮招呼（tuya_tick）：magic packet（一条读 Basic 0x0004/0x0000/0x0001/0x0005/0x0007/0xFFFE，同 zhc 的 configureMagicPacket，
 *    有的设备收到它才开始报数）+ dataQuery（让它把所有数据点报一遍：重启后不用等它下次变化）；规矩同开机补读电量（休眠的只在它刚说过话时、一拍一条）。
 *
 *  - 主动发现（设计 8.12 ④；M6 第一步）：每次开机网 UP 之后 ZH_DISC_START_MS 起爬一轮。从协调器（发给自己 0x0000）和设备表里已知的路由器开始，
 *    逐台读邻居表（ZDO LQI 表请求，分页），按广度优先往下爬路由器（最多 ZH_DISC_DEPTH_MAX 层）。表里 IEEE 和短地址都不认识的设备直接建记录
 *    （两样都在表里，不用再问）：路由器标成路由器、马上访谈（iv_can_talk 认路由器），终端设备等它说话再访谈（休眠设备推迟）。
 *    一次一条、两条至少隔 ZH_DISC_GAP_MS（设计"每秒 1 个"；zigpy topology.py 的 REQUEST_DELAY 1.0–1.5 s）；每页最多 ZH_DISC_TRIES 次、每次等
 *    ZH_DISC_WAIT_MS；应答状态不是成功的那台跳过（zigpy 同：当它不支持）；本地队列满交不出去不占次数，下一拍再试。网不是 UP 时暂停，
 *    在途的那条作废、不算次数；还在等那 60 s 时掉了网，回来重新等。不知道本网的扩展 PAN 不开始。
 *    邻居表说空闲时也收的终端（常电终端）也马上访谈（ZH_DISC_F_RX）；主动发现建的记录一次都没听到过就判离线时静默、不推送（ZH_DISC_F_NEW）。
 *    **不改已有记录**（短地址变了、能力不同都归宣告 / 被动学习 / 访谈管：邻居表可能是旧的）；短地址被别的记录占着的条目也不建（不拿旧表去作废一条对的记录）；
 *    跳过前子节点（旧条目）和扩展 PAN 不是本网的。只用来建设备记录：不保存、不对外提供邻居表（docs/00 第九节 A.2 的直连型例外，MD-13）。
 *
 * 线程：所有入口都在 zhub 任务里串行调（同 zh_host_core 的线程契约）；存取 / 事件 / 发命令经 ops，由 zhub.c 接到 hub_core 登记的钩子与
 * zh_host 的应用命令队列上。时间都是调用方给的单调毫秒（uint32，只比差值）。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zh_ezsp_core.h"
#include "zh_ezsp_ses.h"
#include "zh_lumi.h"
#include "zh_tuya.h"
#include "zh_zcl.h"

#define ZH_DEV_MAX             64u      /* 设计 8.9："64 台 × 150 B ≈ 10 KB" */
#define ZH_DEV_REC_VER         1u       /* zh_dev_rec_t 的格式版本（hub_store 的 ver 字节；加载时按版本迁移） */
#define ZH_DEV_LOOKUP_GAP_MS   10000u   /* 同一个 nwk 两次地址查询的最短间隔（8.12 ③"每 10 s 最多 1 次"） */
#define ZH_DEV_IEEE_WAIT_MS    10000u   /* IEEE_addr_req 发出去等应答多久（休眠设备的父节点要等它轮询，几秒常见） */
#define ZH_DEV_LOOKUP_WAIT_MS  5000u    /* 问 NCP 地址表等结果多久：链路复位时在途 / 排队的应用命令不回结果（会话收摊时吞掉），不设期限这一格就永远占着 */
#define ZH_DEV_LOOK_N          8u       /* 同时在查的不认识的 nwk 最多几个 */
#define ZH_DEV_AGE_CAP_MS      1728000000u   /* 距最近一次收到最多记 20 天：时刻是 32 位毫秒，静默超过约 49.7 天会回绕成"刚收到"（又显示在线），zh_dev_tick 把更老的封在这里。
                                                必须 < 2^31：封顶时按有符号差值认出"t_seen 比 now 晚一点"（回调里取的时刻比 tick 的晚）并跳过 —— 修复复核 ❌：
                                                原先 30 天、无符号比较，刚上报的设备会被封成 30 天前、显示离线 */
_Static_assert(ZH_DEV_AGE_CAP_MS + 2u * 60000u < 0x80000000u, "封顶上限加两次检查间隔要小于 2^31（有符号差值判先后）");
#define ZH_DEV_AGE_CHK_MS      60000u   /* 封顶多久查一次 */

/* ZDO（zigbee-device-stack.h；行号由 test_host/run.sh 核） */
#define ZH_ZDO_IEEE_ADDR_REQ   0x0001u  /* zigbee-device-stack.h:260 IEEE_ADDRESS_REQUEST */
#define ZH_ZDO_IEEE_ADDR_RSP   0x8001u  /* zigbee-device-stack.h:261 IEEE_ADDRESS_RESPONSE */
#define ZH_APS_OPTS_UNICAST    0x0040u  /* device-table-tracking.c:124 handleUnknownDevice SL_ZIGBEE_APS_OPTION_RETRY（SDK 里同一件事 ——"不认识的设备发 IEEE_addr_req"—— 的先例，service-discovery-common.c:276 同；第一轮审计） */
#define ZH_OUTGOING_DIRECT     0u       /* sl_zigbee_types.h:816 SL_ZIGBEE_OUTGOING_DIRECT（sendUnicast 的 type；枚举第一个） */
/* 访谈用的 ZDO（布局：zigbee-device-stack.h 簇号上方的注释；节点描述符各字段位宽在 :273-286 —— 整包第 4 字节低 3 位是逻辑类型、第 6 字节 MAC 能力、第 7–8 字节厂商码） */
#define ZH_ZDO_NODE_DESC_REQ   0x0002u  /* zigbee-device-stack.h:289 NODE_DESCRIPTOR_REQUEST */
#define ZH_ZDO_NODE_DESC_RSP   0x8002u  /* zigbee-device-stack.h:290 NODE_DESCRIPTOR_RESPONSE */
#define ZH_ZDO_NODE_RSP_MIN    17u      /* zigbee-device-stack.h:292 NODE_DESCRIPTOR_RESPONSE_MINIMUM_LENGTH（序号 + 状态 + nwk + 13 B 描述符） */
#define ZH_ZDO_SIMPLE_DESC_REQ 0x0004u  /* zigbee-device-stack.h:322 SIMPLE_DESCRIPTOR_REQUEST */
#define ZH_ZDO_SIMPLE_DESC_RSP 0x8004u  /* zigbee-device-stack.h:323 SIMPLE_DESCRIPTOR_RESPONSE */
#define ZH_ZDO_ACTIVE_EP_REQ   0x0005u  /* zigbee-device-stack.h:334 ACTIVE_ENDPOINTS_REQUEST */
#define ZH_ZDO_ACTIVE_EP_RSP   0x8005u  /* zigbee-device-stack.h:335 ACTIVE_ENDPOINTS_RESPONSE */
/* 绑定（CONFIGURE 第二步）：[seq][源 EUI64][源端点][簇 2 B][0x03][目标 EUI64][目标端点]；应答 [seq][状态]（zigbee-device-stack.h:451-458 的格式说明） */
#define ZH_ZDO_BIND_REQ        0x0021u  /* zigbee-device-stack.h:464 BIND_REQUEST */
#define ZH_ZDO_BIND_RSP        0x8021u  /* zigbee-device-stack.h:465 BIND_RESPONSE */
#define ZH_ZDO_UNICAST_BINDING 0x03u    /* zigbee-device-stack.h:460 UNICAST_BINDING */
/* 主动发现（8.12 ④）：LQI 表 = 邻居表 */
#define ZH_ZDO_LQI_TABLE_REQ   0x0031u  /* zigbee-device-stack.h:517 LQI_TABLE_REQUEST：[序号][起始下标] */
#define ZH_ZDO_LQI_TABLE_RSP   0x8031u  /* zigbee-device-stack.h:518 LQI_TABLE_RESPONSE：[序号][状态][表总条数][起始下标][本页条数][条目 × 22] */
#define ZH_ZDO_LQI_ENTRY_LEN   22u      /* zigbee-device-stack.h:478：[扩展 PAN:8][EUI64:8][nwk:2][类型 | rx | 关系:1][允许入网:1][深度:1][LQI:1] */
#define ZH_LQI_TYPE_MASK       0x03u    /* zigbee-device-stack.h:489-492：0 协调器 1 路由器 2 终端 3 不知道 */
#define ZH_LQI_TYPE_ROUTER     0x01u
#define ZH_LQI_TYPE_END_DEVICE 0x02u
#define ZH_LQI_RX_SHIFT        2u       /* zigbee-device-stack.h:494-496：0x0C，0 空闲时关 1 开 2 不知道 */
#define ZH_LQI_REL_SHIFT       4u       /* zigbee-device-stack.h:498-502：0x70，0 父 1 子 2 兄弟 3 别的 4 前子节点 */
#define ZH_LQI_REL_PREV_CHILD  4u
#define ZH_DISC_START_MS       60000u   /* 网 UP 之后多久开始（开机那一波宣告 / 上报 / 被动学习先过去） */
#define ZH_DISC_GAP_MS         1000u    /* 两条请求至少隔多久 */
#define ZH_DISC_WAIT_MS        5000u    /* 一页等多久 */
#define ZH_DISC_TRIES          3u       /* 每页最多发几次（zigpy retries=2 → 共 3 次） */
#define ZH_DISC_DEPTH_MAX      5u       /* 最多爬几层（协调器是第 0 层） */
#define ZH_DISC_Q_MAX          (ZH_DEV_MAX + 1u)   /* 待爬的：协调器 + 最多一张设备表那么多路由器 */
#define ZH_DISC_PAGES_MAX      16u      /* 一台最多读几页（对方报的总条数离谱时别一直读下去） */
typedef enum { ZH_DISC_IDLE = 0, ZH_DISC_WAIT, ZH_DISC_RUN, ZH_DISC_DONE } zh_disc_state_t;
typedef struct {
    uint8_t  state;               /* zh_disc_state_t */
    uint8_t  q_n, q_i;            /* 队列里几台 / 正在爬第几台（队列只进不出，兼作"爬过的"） */
    struct { uint16_t nwk; uint8_t depth; } q[ZH_DISC_Q_MAX];
    uint8_t  start_idx, pages, tries;
    bool     inflight;
    uint8_t  zdo_seq;
    uint16_t gen;                 /* 每发一条换一个（编进 tag：迟到的应用命令结果对不上就不认） */
    uint32_t t_start, t_req, t_next;
} zh_disc_t;

#define ZH_ZDP_SUCCESS         0x00u    /* sl_zigbee_types.h:2764 SL_ZIGBEE_ZDP_SUCCESS */
/* 访谈的 ZDO 与 ZCL 单播用 SDK 框架的默认 APS 选项（集中器那一支：集中器靠 MTORR、不开路由发现；SDK 的 service-discovery 与 ZCL 发送都用它；
   设计 8.11 写的 0x0140 是 bellows 不开集中器时的取法，第七步 b H1） */
#define ZH_APS_OPTS_DEFAULT    0x1040u  /* config.h:306-309 SL_ZIGBEE_AF_DEFAULT_APS_OPTIONS（RETRY | ENABLE_ADDRESS_DISCOVERY） */
#define ZH_DEV_SRC_EP          1u       /* 网关自己的端点（zhub.c 静态断言它等于 ZH_HOST_ENDPOINT） */
/* IAS 登记应答要带网关的 EUI64：设备拿它和自己记的 CIE 地址比，对上了才认（SDK ias-zone-client.c:332-334 的注释与做法；第一轮审计） */
#define ZH_APS_OPT_SOURCE_EUI64 0x0400u /* sl_zigbee_types.h:757 SL_ZIGBEE_APS_OPTION_SOURCE_EUI64 */
#define ZH_ST_BATT_LOW_PCT     15u      /* 电量 ≤ 15 % 算低（6.7） */
#define ZH_ST_BATT_REARM_PCT   20u      /* 报过电量低之后回到这个值及以上才算恢复、下次再降才再报：读数在 15 % 上下抖不算新的翻转（审计 ⚠️，2026-10-24；不是限次数） */
#define ZH_IAS_ENROLL_GAP_MS   10000u   /* 同一台的登记应答最多 10 s 回一次（登记请求刷屏时别跟着刷） */
/* 可用性（设计 8.10；M5 第十一步）：常电设备静默 10 min 后 ping（读 Basic 0x0000），10 s 一次，连续 3 次没回判离线（≈ 30 s）；
   电池设备（含不知道是不是休眠的）静默 25 h 判离线、不 ping —— 只判这次开机听到过的、不判遥控器类（只在按的时候说话）。
   收到它任何消息即恢复。网不是 UP 时不判，网恢复后从恢复那刻起算 */
#define ZH_AV_MAINS_SILENT_MS  600000u
#define ZH_AV_PING_GAP_MS      10000u
#define ZH_AV_PING_TRIES       3u
#define ZH_AV_BATT_SILENT_MS   90000000u    /* 25 h = Z2M 的 passive timeout 1500 min */
_Static_assert(ZH_AV_BATT_SILENT_MS < ZH_DEV_AGE_CAP_MS, "电池判离线的阈值要小于年龄封顶（不然封顶后永远够不着）");
#define ZH_CTLQ_MAX_MS         1800000u /* 8.11 第 6 步：休眠设备的命令最多排 30 min（没送到就作废） */
#define ZH_CTLQ_SENT_MS        30000u   /* 补发之后等 messageSent 的期限：经父节点间接发，一次最多缓存 7.68 s（zh_host_core.h 的
                                           ZH_HOST_INDIRECT_TX_TIMEOUT_MS），APS 重试 3 次约 23 s，再加余量。别用常电的 ZH_CTL_DEADLINE_MS（8 s，审计 ❌：
                                           回执晚到被丢、下次醒来再发一遍，切换会被撤销） */
_Static_assert(ZH_DEV_MAX <= 64u, "补发的 messageTag 里下标只有 6 位（cq_mtag）");
/* 休眠设备的待发槽（一台一个；新命令顶掉旧的）。补发时机与结论见 zh_dev.c 的 ctlq_tick / zh_dev_on_msg_sent */
typedef struct {
    bool     on;
    bool     inflight;                /* 交出去了、等 messageSent */
    uint8_t  act, level, gen, tries;  /* gen：每发一次换一个（迟到的回执只认得自己那一发） */
    uint32_t t_q, t_sent;             /* 排进来的时刻 / 最近一次补发的时刻 */
} zh_ctlq_t;

/* 访谈（设计 8.6） */
#define ZH_IV_TRIES            3u       /* 每步最多几次 */
#define ZH_IV_STEP_TIMEOUT_MS  5000u    /* 每次等应答多久 */
#define ZH_IV_TOTAL_MS         1800000u /* 从第一条请求起最多 30 min */
#define ZH_IV_AWAKE_MS         3000u    /* 休眠（或还不知道是不是休眠）的设备：距它最近一条消息这么久之内才发下一条（它刚发完消息多半还在快速轮询；
                                           父节点替它缓存 7.68 s，见 zh_host_core.h 的 ZH_HOST_INDIRECT_TX_TIMEOUT_MS） */
#define ZH_IV_JOIN_AWAKE_MS    60000u   /* 刚经入网窗口加入的设备：这么久之内都当醒着（入网后休眠设备会快速轮询一阵；第一条请求丢了不用等它下一条应用消息 —— 门磁可能几小时，第二轮审计） */
#define ZH_CIE_RESUME_GAP_MS   60000u   /* 欠着 CIE / 登记的设备：重新宣告 / 重新入网 / 登记请求触发补做，距上次没成至少隔这么久（反复发登记请求、
                                           又一直确认不了的设备别每条请求都引出一整轮 8 帧 —— 修复复核） */
#define ZH_BATT_READ_TRIES     3u       /* 开机补读电量：每次开机每台最多读几次（读了没回 = 它多半已经睡了，下次说话再试） */
#define ZH_BATT_READ_GAP_MS    600000u  /* 两次补读至少隔这么久 */
#define ZH_CFG_MAX             8u       /* CONFIGURE 第二步一台设备最多配几条（端点 × 簇；ZH_DEV_EP_REC 个端点、k_cfg 8 行） */
#define ZH_IV_RETRY_MS         3600000u /* 访谈失败的设备：距失败这么久之后它再说话就重新访谈（它重新宣告时马上重来）。网关常年不重启，不能只靠"下次开机"（第二轮审计） */
/* 访谈失败、随时听得见的（iv_always_on：已知空闲时也收 / 路由器）：不等它说话，距失败这么久自己重来 —— 第 1、2、3 次失败后用这三档，之后每 ZH_IV_RETRY_MS。
   2026-10-24：Sengled 灯重新入网的同一刻就被访谈，它（或 NCP 那边）还没就绪，15 s 三问不回判失败；这种灯从不主动说话、也不宣告，
   "失败一小时后再说话"永远等不来，直到网关重启才重新访谈上 */
#define ZH_IV_ON_RETRY1_MS     30000u
#define ZH_IV_ON_RETRY2_MS     120000u
#define ZH_IV_ON_RETRY3_MS     600000u
#define ZH_DEV_EP_REC          3u       /* 记录里存几个端点（= zh_dev_rec_t.ep 的大小；只访谈这么多个，多了也存不下） */
#define ZH_DEV_EP_CL           12u      /* 每个端点存几个簇（先输入后输出，放不下的截掉；n_in / n_out 是实际个数） */
#define ZH_ZONE_TYPE_UNKNOWN   0xFFFFu  /* 记录里的 zone_type：不是 IAS 设备 / 没读到 */
typedef enum {
    ZH_IV_NONE = 0,               /* 不用访谈（没开始 / 已完成就看 flags 的 INTERVIEWED） */
    ZH_IV_NODE,                   /* 下一条（或在途的）是 Node_Desc_req */
    ZH_IV_ACTIVE,
    ZH_IV_SIMPLE,
    ZH_IV_BASIC,
    ZH_IV_ZONE,
    ZH_IV_CIE,                    /* 写 IAS_CIE_ADDRESS（CONFIGURE 第一步）：等写属性应答 */
    ZH_IV_ENROLL,                 /* 发登记应答 + 回读 ZoneState：读到"已登记"才算完 */
    ZH_IV_BIND,                   /* CONFIGURE 第二步：把第 cfg_i 条计划的簇绑定到网关 */
    ZH_IV_REPORT,                 /* CONFIGURE 第二步：给第 cfg_i 条计划配属性上报 */
    ZH_IV_FAILED,                 /* 这次开机不再试（重新入网 / 下次开机再访谈） */
} zh_iv_step_t;
/* Basic 要读的属性（iv_want 的位） */
#define ZH_IV_W_MANUF          0x01u
#define ZH_IV_W_MODEL          0x02u
#define ZH_IV_W_POWER          0x04u
#define ZH_IV_W_SWB            0x08u

/* trustCenterPostJoinHandler 的 status / decision（sl_zigbee_types.h:1107-1110、1072-1081） */
enum { ZH_JOIN_SECURED_REJOIN = 0, ZH_JOIN_UNSECURED_JOIN = 1, ZH_JOIN_DEVICE_LEFT = 2, ZH_JOIN_UNSECURED_REJOIN = 3 };
enum { ZH_JOIN_USE_PRECONFIGURED_KEY = 0, ZH_JOIN_SEND_KEY_IN_CLEAR = 1, ZH_JOIN_DENY = 2, ZH_JOIN_NO_ACTION = 3 };

/* Device_annce 的能力字节（MAC capability flags；SDK 头文件里没有具名常量，取值照 zigpy/zdo/types.py:68-76 MACCapabilityFlags）。
   Device_annce = [序号][nwk:2][IEEE:8][能力:1]（zigbee-device-stack.h:361-364）；IEEE_addr_rsp = [序号][状态][IEEE:8][nwk:2]…（同文件 :250-256） */
#define ZH_CAP_FFD             0x02u    /* 全功能设备（路由器） */
#define ZH_CAP_MAINS           0x04u    /* 市电供电 */
#define ZH_CAP_RX_ON_IDLE      0x08u    /* 空闲时也收（不是休眠设备） */

/* 记录标志 */
#define ZH_DEVF_RX_ON          0x01u
#define ZH_DEVF_MAINS          0x02u
#define ZH_DEVF_INTERVIEWED    0x04u
#define ZH_DEVF_LEFT           0x08u
#define ZH_DEVF_ANNCED         0x10u    /* 能力字节已知（来自 Device_annce 或访谈的节点描述符：RX_ON / MAINS 可信）；被动学习来的记录没有 */
#define ZH_DEVF_NODE_DESC      0x20u    /* 访谈拿到了节点描述符（manuf_code 可信；0x0000 本身也是个厂商码） */
#define ZH_DEVF_POWER          0x40u    /* 访谈读到了 Basic 的供电属性（MAINS 可信，哪怕能力字节不知道） */
#define ZH_DEVF_CIE_OWED       0x80u    /* IAS 设备的 CIE 地址 / 登记还欠着（M5 第四步）：见文件头。这一位以前恒为 0，旧记录读出来就是"不欠"，格式版本不用升 */

/* 节点类型 */
enum { ZH_NODE_UNKNOWN = 0, ZH_NODE_ROUTER = 1, ZH_NODE_END_DEVICE = 2 };

/* 设计 8.9 的 zdev_rec_t（落盘格式，hubdb 命名空间 zhub、键 dNN）。访谈填的字段（manuf_code / manuf / model / sw_build_crc / ep / zone_type）开始访谈时清空、访谈完一次落盘；
   caps / quirk_id 随 M5 的能力映射 */
typedef struct __attribute__((packed)) {
    uint8_t  ver;                 /* = ZH_DEV_REC_VER */
    uint8_t  ieee[8];             /* 小端（= 线上字节序） */
    uint16_t nwk;
    uint8_t  node_type;           /* ZH_NODE_* */
    uint8_t  flags;               /* ZH_DEVF_* */
    uint16_t manuf_code;
    char     manuf[24], model[24];/* 访谈读回的原样字节（最多 23 B、以 0 结尾；遇到 0 字节截断）。不可信：输出时由 hub_zigbee 按 6.8 净化 */
    uint32_t sw_build_crc;        /* SWBuildID 字符串的 CRC-32（0 = 没读到）：设备换了固件就变 */
    uint8_t  n_ep;
    struct __attribute__((packed)) zh_dev_ep { uint8_t ep; uint16_t profile, devid; uint8_t n_in, n_out; uint16_t cl[12]; } ep[3];   /* cl：先输入后输出，见 ZH_DEV_EP_CL */
    uint32_t caps;
    uint16_t zone_type;
    uint8_t  quirk_id;
} zh_dev_rec_t;
_Static_assert(sizeof(zh_dev_rec_t) == 168, "zh_dev_rec_t 是落盘格式：大小变了要升 ZH_DEV_REC_VER 并写迁移");   /* 设计 8.9 估"约 150 B"，实际 168；64 台约 10.8 KB */
_Static_assert(sizeof(((zh_dev_rec_t *)0)->ep) / sizeof(((zh_dev_rec_t *)0)->ep[0]) == ZH_DEV_EP_REC, "ZH_DEV_EP_REC = 记录里的端点数");
_Static_assert(sizeof(((zh_dev_rec_t *)0)->ep[0].cl) / sizeof(uint16_t) == ZH_DEV_EP_CL, "ZH_DEV_EP_CL = 每个端点存的簇数");

/* 读数（只在内存）。have 的位 = ZH_ST_* 有值；t[] = 每个读数最近更新的时刻（ms） */
enum { ZH_ST_ZONE = 0, ZH_ST_OCCUPANCY, ZH_ST_BATTERY_PCT, ZH_ST_BATTERY_MV, ZH_ST_TEMP, ZH_ST_HUMIDITY, ZH_ST_LUX, ZH_ST_PRESSURE,
       ZH_ST_ONOFF, ZH_ST_LEVEL, ZH_ST_N };
typedef struct {
    uint16_t have;
    uint16_t zone_status;         /* IAS ZoneStatus 原值（ZH_IAS_ST_*） */
    uint8_t  occupancy;           /* 0 / 1 */
    uint8_t  battery_pct;         /* 0–100 */
    uint16_t battery_mv;
    int16_t  temp_c100;           /* 0.01 °C */
    uint16_t hum_c100;            /* 0.01 % */
    uint32_t lux;
    int32_t  pressure_hpa10;      /* 0.1 hPa */
    uint8_t  onoff;
    uint8_t  level_pct;
    uint32_t t[ZH_ST_N];
} zh_state_t;
/* ZH_DEV_EV_STATE 时"发生了什么"（st_ev 的位） */
#define ZH_STEV_ALARM_ON       0x01u    /* IAS alarm1 0 → 1（人体有人 / 门开 / 漏水 / 烟…，看 ZoneType） */
#define ZH_STEV_ALARM_OFF      0x02u    /* alarm1 1 → 0 */
#define ZH_STEV_TAMPER         0x04u    /* 防拆 0 → 1 */
#define ZH_STEV_BATTERY_LOW    0x08u    /* 电量低：IAS 电量位 0 → 1，或电量 % 降到 ZH_ST_BATT_LOW_PCT 及以下（回到 ZH_ST_BATT_REARM_PCT 以上之前只报一次） */
#define ZH_STEV_OCC_ON         0x10u    /* 占用传感器 0 → 1 */
#define ZH_STEV_OCC_OFF        0x20u

typedef struct {
    bool         used;
    bool         reserved;        /* 开机时这一格读不出来（超时 / 读错 / 存的格式比固件新）：这次开机不分配给新设备，免得盖掉存储里那条 */
    zh_dev_rec_t rec;
    bool         seen;            /* 这次开机以来收到过它的消息 */
    uint32_t     t_seen;          /* 最近一次收到（seen = false 时是加载 / 建记录的时刻：可用性从开机起算） */
    int8_t       rssi;
    uint8_t      lqi;
    uint32_t     rx;              /* 这次开机以来收到的消息数 */
    /* 访谈进度（只在内存；结果落盘） */
    uint8_t      iv;              /* zh_iv_step_t */
    uint8_t      iv_tries;        /* 这一步已经发了几次 */
    uint8_t      iv_ep_i;         /* SIMPLE：在问 iv_eps 的第几个 */
    uint8_t      iv_n_eps;        /* ACTIVE 回的端点里要访谈的（最多 ZH_DEV_EP_REC 个） */
    uint8_t      iv_eps[ZH_DEV_EP_REC];
    uint8_t      iv_seq[ZH_IV_TRIES]; /* 这一步各次请求的序号（ZDO / ZCL）：上一次超时后迟到的应答也认 */
    uint8_t      iv_n_seq;
    uint8_t      iv_want;         /* BASIC：还没有结果的属性（ZH_IV_W_*） */
    uint8_t      iv_req;          /* BASIC：最近一条请求问了哪些（ZH_IV_W_*）：问了没回的要拆开单读；单读被默认应答拒了放弃的就是它 */
    bool         iv_single;       /* BASIC：改成一次读一个 */
    bool         iv_new;          /* 这次开机经入网窗口新加的：访谈完推送给 Muse（附录 A："New devices are reported when they finish joining"） */
    uint32_t     iv_t_new;        /* 入网的时刻（iv_new 时；ZH_IV_JOIN_AWAKE_MS 内当醒着） */
    bool         iv_owe_push;     /* 新入网时访谈失败过（推过"认不出来"）：之后重试成功也要推"认出来了" */
    bool         iv_resume;       /* 这一轮只是补 CIE / 登记（设备早就访谈完了）：结束时不出事件、不算访谈 */
    bool         iv_er_sent;      /* ENROLL：这一次的登记应答已经交出去了，回读还没交出去（下一拍只补回读，别把登记应答一拍一条地刷 —— 修复复核） */
    uint8_t      batt_reads;      /* 这次开机补读电量的次数（不落盘） */
    bool         batt_low;        /* 电量 % 已报过低、还没回到 ZH_ST_BATT_REARM_PCT（只在内存：开机第一报就低照样报一次） */
    uint32_t     t_batt_read;     /* 最近一次补读的时刻 */
    uint8_t      cfg_i, cfg_n;    /* CONFIGURE 第二步：计划里第几条 / 共几条 */
    uint8_t      cfg[ZH_CFG_MAX][2]; /* 每条：[端点在 rec.ep 里的下标][k_cfg 的行号] */
    uint32_t     iv_t_fail;       /* 访谈失败的时刻（ZH_IV_RETRY_MS 之后它再说话就重来） */
    uint8_t      iv_fails;        /* 连着失败了几次（只在内存；访谈完 / 重新配对清零）：随时听得见的按它选自己重来的间隔（ZH_IV_ON_RETRY*_MS） */
    uint32_t     iv_t0;           /* 第一条请求的时刻（0 = 还没发过） */
    zh_state_t   st;              /* 读数（M5 第一步） */
    uint8_t      st_ev;           /* 最近一次 ZH_DEV_EV_STATE 的 ZH_STEV_* */
    uint32_t     t_enroll;        /* 上次回登记应答的时刻（0 = 这次开机还没有）。人体 / 电量低事件没有间隔：每次翻转都出（2026-10-24 用户定） */
    uint16_t     tuya_k;          /* 涂鸦表的查找结果缓存：0 = 还没查；1 = 不在表里；k ≥ 2 = 设备下标 k − 2（访谈重来 / 访谈完时清回 0） */
    uint16_t     tbl_zs;          /* 涂鸦 / 小米表合成的 IAS ZoneStatus（alarm1 / 防拆 / 电量低；只在内存） */
    uint16_t     lumi_k;          /* 小米表的查找结果缓存：0 = 还没查；1 = 不在表里；k ≥ 2 = 设备下标 k − 2（型号变了 / 重新配对时清回 0） */
    uint32_t     t_occ_clear;     /* 小米人体：到这个时刻还没再报"有人"就当没人（0 = 不用；zh_lumi_occ_s） */
    bool         offline;         /* 可用性（8.10）：判过离线、还没再收到它的消息（只在内存；开机当在线） */
    bool         av_quiet;        /* 这次离线是静默判的（这次开机一次都没听到过的电池设备，开机 / 网恢复满 25 h）：不出离线事件，恢复也不出 */
    zh_ctlq_t    cq;              /* 休眠设备的待发控制命令（M5 第十二步） */
    uint8_t      av_miss;         /* 常电设备这一轮静默里发出去、还没回的 ping 数 */
    uint32_t     t_av_ping;       /* 最近一次 ping 的时刻（0 = 这一轮还没 ping） */
    uint8_t      tuya_hello;      /* 这次开机的招呼：0 没打；1 magic packet 交出去了；2 dataQuery 也交出去了 */
    uint8_t      disc;            /* 主动发现（M6 第一步，只在内存）：ZH_DISC_F_* */
} zh_dev_t;
#define ZH_DISC_F_NEW          0x01u    /* 这条记录是主动发现建的：没听到过它之前判离线不推送（表里的旧条目可能根本不在了，用户也没见过它） */
#define ZH_DISC_F_RX           0x02u    /* 邻居表说它空闲时也收（常电终端）：马上访谈（设计 8.12 ④"常电设备和路由器 → 立即访谈"） */

typedef enum {
    ZH_DEV_EV_ADDED = 0,          /* 新建了一条记录（入网 / 设备宣告 / 被动学习） */
    ZH_DEV_EV_LEFT,               /* 离开网络（标 left，记录还在） */
    ZH_DEV_EV_BACK,               /* 标过 left 的又出现了（重新入网 / 发消息） */
    ZH_DEV_EV_NWK,                /* 短地址变了 */
    ZH_DEV_EV_IDENTIFIED,         /* 访谈完成（记录里有了厂商 / 型号 / 端点）；src = JOIN 表示这次开机新入网的 */
    ZH_DEV_EV_IV_FAILED,          /* 访谈失败（设备照样列出）；src 同上 */
    ZH_DEV_EV_STATE,              /* 报警类读数翻转（x->st_ev 说是哪种） */
    ZH_DEV_EV_OFFLINE,            /* 判离线（8.10；常电 3 次 ping 没回 / 电池 25 h 没消息） */
    ZH_DEV_EV_ONLINE,             /* 判过离线的又收到它的消息 */
} zh_dev_ev_t;

typedef enum {
    ZH_DEV_SRC_JOIN = 0,          /* 新设备入网（信任中心放行） */
    ZH_DEV_SRC_ANNCE,             /* Device_annce */
    ZH_DEV_SRC_PASSIVE,           /* 被动学习 */
    ZH_DEV_SRC_REJOIN,            /* 重新入网：已在网的设备（换固件迁移过来的就是这样被认出来的），不是"新加的" */
    ZH_DEV_SRC_DISCOVERY,         /* 主动发现（8.12 ④）：路由器的邻居表里列着、我们还不认识的现网设备 */
} zh_dev_src_t;

typedef struct {
    void (*save)(void *ctx, unsigned idx, const zh_dev_rec_t *rec);                    /* 结构变了：落盘（调用方防抖） */
    void (*event)(void *ctx, zh_dev_ev_t ev, zh_dev_src_t src, unsigned idx, const zh_dev_rec_t *rec);
    bool (*send)(void *ctx, const uint8_t *frame, size_t n, uint32_t tag, bool idempotent);   /* 交给 zh_host 的应用命令队列；false = 现在交不了 */
    uint8_t (*zdo_seq)(void *ctx);                                                      /* 取一个 ZDO 序号（与 zh_host 共用一个计数，0–127） */
    void *ctx;
} zh_dev_ops_t;

typedef struct {
    uint32_t added, updated, left, back, nwk_changes;
    uint32_t full;                /* 表满：没建成的，或因此没去空口问 IEEE 的 */
    uint32_t reserved;            /* 开机时读不出来、这次开机保留着的格子 */
    uint32_t unknown_rx;          /* 不认识的 nwk 发来的消息 */
    uint32_t lookups, lookup_hits;/* 问 NCP 地址表 / 问到了 */
    uint32_t ieee_reqs, ieee_rsps;/* 发 IEEE_addr_req / 收到合格的应答 */
    uint32_t learn_fails;         /* 两样都没问到（超时 / 被拒），等下次它说话再查 */
    uint32_t send_busy;           /* 应用命令队列满，这次没交出去 */
    uint32_t bad_eui;             /* 全 0 / 全 FF 的 IEEE，不认 */
    uint32_t bad_nwk;             /* 宣告里的短地址是协调器 / 广播保留段，不认 */
    uint32_t deferred;            /* 查询限速：10 s 内要开的新查询超过 ZH_DEV_LOOK_N 个（间隔环会被盖掉），这次不查、等它下次说话 */
    uint32_t iv_reqs, iv_timeouts;/* 访谈：发出的请求 / 等应答超时 */
    uint32_t iv_done, iv_failed;
    uint32_t iv_bad;              /* 访谈应答解不开 / 对不上（ZDO 长度不对、ZCL 记录坏了） */
    uint32_t iv_send_fail;        /* 访谈请求 sendUnicast 被拒（等到期限再重发） */
    uint32_t iv_retries;          /* 访谈失败的随时听得见的设备：到点自己重来的次数（ZH_IV_ON_RETRY*_MS） */
    uint32_t reports;             /* 收下的读数（上报 / 读属性应答 / IAS 通知的每条记录） */
    uint32_t report_bad;          /* 上报帧解不开 / 记录坏了 / 类型对不上 */
    uint32_t enroll_reqs, enroll_rsps;   /* IAS 登记请求 / 回出去的登记应答（交出去算） */
    uint32_t cie_ok, cie_fail;    /* 写 CIE 地址：设备回了成功 / 回了失败或几次都没回（不算访谈失败） */
    uint32_t cie_skipped;         /* 该写 CIE 地址时还不知道网关自己的 EUI64（zhub.c 读到之前）：记欠着，读到后补 */
    uint32_t enroll_ok;           /* 回读 ZoneState 确认登记上了 */
    uint32_t enroll_unconfirmed;  /* 发了登记应答、几次回读都不是"已登记"（或读不了） */
    uint32_t cie_resumes;         /* 欠着 CIE / 登记的设备补做的次数 */
    uint32_t bind_ok, bind_fail;  /* CONFIGURE 第二步：绑定应答成功 / 失败或几次都没回 */
    uint32_t report_ok, report_fail;     /* 上报配置应答成功 / 失败或几次都没回 */
    uint32_t batt_reads;          /* 开机补读电量发出去的条数（M5 第六步） */
    uint32_t ctl_sent, ctl_confirmed, ctl_delivered, ctl_failed;   /* 控制命令（M5 第七步） */
    uint32_t cfg_skipped;         /* 该配的时候没配完：还不知道自己的 EUI64、或 30 min 期限到了（剩下的条数照计） */
    uint32_t tuya_frames;         /* 收到的涂鸦带读数的帧（M5 第八步） */
    uint32_t tuya_unknown;        /* 其中发送方不在表里（或还没访谈完）的 */
    uint32_t tuya_unmapped;       /* 表里有这台、但这个数据点不在表里（不是 zhub 收的字段）的条数 */
    uint32_t tuya_hellos;         /* 交出去的招呼帧（magic packet + dataQuery） */
    uint32_t ctl_tuya;            /* 走涂鸦路（写数据点）的控制命令（M5 第九步；也计在 ctl_sent 里） */
    uint32_t lumi_frames;         /* 收到的小米私有数据（Basic 0xFF01 / 0xFCC0 簇的上报；M5 第十步） */
    uint32_t lumi_unknown;        /* 其中型号不在小米表里（或还不知道型号）的 */
    uint32_t learned;             /* 从主动上报里学到 / 改了厂家或型号的次数（小米休眠设备几乎不应访谈） */
    uint32_t av_pings, av_offline, av_online;   /* 可用性：交出去的 ping、判离线、恢复（M5 第十一步） */
    uint32_t av_quiet;            /* 静默判离线的（开机后一次都没听到过的电池设备，2026-10-23） */
    uint32_t ctlq_queued, ctlq_replaced, ctlq_sent, ctlq_delivered, ctlq_failed, ctlq_expired, ctlq_dropped;   /* 休眠设备的排队（M5 第十二步） */
    /* 主动发现（M6 第一步）：每次开机一轮 */
    uint32_t disc_reqs, disc_rsps;       /* 发出的 LQI 表请求 / 收下的应答页 */
    uint32_t disc_timeouts;              /* 一页等应答超时（一次算一次） */
    uint32_t disc_send_fail;             /* 请求交不出去 / NCP 拒了（算用掉一次） */
    uint32_t disc_routers;               /* 读完了邻居表的路由器（含协调器） */
    uint32_t disc_unsupported;           /* 应答状态不是成功的（跳过这台） */
    uint32_t disc_gave_up;               /* 一页几次都没回、放弃的 */
    uint32_t disc_entries;               /* 收下的邻居条目 */
    uint32_t disc_added;                 /* 因此新建的记录 */
    uint32_t disc_known;                 /* 已认识的（不改） */
    uint32_t disc_conflict;              /* 短地址被别的记录占着（不建） */
    uint32_t disc_xpan;                  /* 扩展 PAN 不是本网的（上板时它等于 disc_entries = 字节序对不上，查 zh_dev_set_ext_pan 的来源） */
    uint32_t disc_skipped;               /* 前子节点 / 协调器自己 / 广播保留段 */
    uint32_t disc_bad;                   /* 应答长度不对 */
    uint32_t disc_rounds;                /* 爬完的轮数 */
    uint32_t disc_q_full;                /* 待爬的队列满了、没排进来的路由器 */
} zh_dev_stats_t;

typedef struct {
    uint16_t nwk;
    uint8_t  stage;               /* 0 空；1 问 NCP 地址表在途；2 IEEE_addr_req 在等应答 */
    uint8_t  zdo_seq;
    int8_t   rssi;                /* 触发这次查询的那条消息的信号（问 NCP 地址表查到时记进记录：那条消息也是它发的） */
    uint8_t  lqi;
    uint16_t gen;                 /* 这一格这一轮的代号（编进 tag）：过期放掉的格子被下一个 nwk 占了，上一轮迟到的结果对不上代号，不会被安到新 nwk 头上 */
    uint32_t t_start;             /* 这一段开始的时刻（两段的超时都用） */
} zh_dev_look_t;

/* ---------------------------------------------------------------- 控制（设计 8.11；M5 第七步）
 * 同时只有一条在途（hub_cmds 同步调、zhub.c 投一条等一条）。只给常电设备（RX_ON / 路由器）：休眠设备的主机侧排队（8.11 第 6 步）随后。
 * ZCL 簇专用、客户端 → 服务端、**不禁默认应答**：设备回的默认应答就是确认。结论（8.11 第 4、5 步）：
 *   成功的默认应答、或 ZH_CTL_CONFIRM_MS 内来了这个簇的属性上报 → CONFIRMED；默认应答报错 → FAILED（zcl_status）；
 *   sendUnicast 被拒 / messageSentHandler 报没送达 → FAILED（status）；只有 messageSent 报送达、窗口里没等到确认 → DELIVERED；
 *   ZH_CTL_DEADLINE_MS 还没有任何回音 → FAILED。messageTag = 0x8000 | 代号（访谈 / 补读用的 messageTag 是 ZCL / ZDO 序号，不超 0xFF，碰不上）
 * 涂鸦设备（M5 第九步）：设备有 ZCL 的开关 / 亮度簇就照上面走（ZCL 优先）；没有、而社区设备库导出的表里有写法的，写 0xEF00 数据点
 *   （zh_tuya_enc_ctl；照 z2m **禁默认应答**），确认 = ZH_CTL_CONFIRM_MS 内设备回报了写的那个 dp（涂鸦设备改完状态会立刻回报）；
 *   切换：涂鸦没有切换命令，按当前读数取反 —— 这次开机还没读到开关状态就回 ZH_CTL_E_STATE（让调用方用开 / 关）。 */
typedef enum { ZH_CTL_ON = 0, ZH_CTL_OFF, ZH_CTL_TOGGLE, ZH_CTL_LEVEL } zh_ctl_act_t;
typedef enum { ZH_CTL_R_NONE = 0, ZH_CTL_R_CONFIRMED, ZH_CTL_R_DELIVERED, ZH_CTL_R_FAILED, ZH_CTL_R_PENDING } zh_ctl_result_t;   /* PENDING：休眠设备，排进待发（8.11 第 6 步） */
typedef enum {
    ZH_CTL_E_OK = 0,
    ZH_CTL_E_NOT_FOUND,           /* 设备表里没有 */
    ZH_CTL_E_LEFT,                /* 离开了 / 短地址作废 */
    ZH_CTL_E_NO_CLUSTER,          /* 没有这个动作要的输入簇 */
    ZH_CTL_E_SLEEPY,              /* 休眠设备：还不支持 */
    ZH_CTL_E_BUSY,                /* 上一条还在途 */
    ZH_CTL_E_SEND,                /* 应用命令队列满 / 编码失败 */
    ZH_CTL_E_NET,                 /* 网络不是 UP */
    ZH_CTL_E_ARG,
    ZH_CTL_E_STATE,               /* 涂鸦设备切换：还不知道它现在开着还是关着（M5 第九步） */
    ZH_CTL_E_NO_OFF,              /* 涂鸦设备亮度 0 %（= 关）而它没有开关 dp：关不了，只能调 1–100 %（审计 ⚠️） */
} zh_ctl_err_t;
#define ZH_CTL_CONFIRM_MS      3000u    /* 8.11 第 5 步：3 s 内收到默认应答 / 上报 = confirmed；之后只送达了的算 delivered */
#define ZH_CTL_DEADLINE_MS     8000u    /* 一点回音都没有（连 messageSent 都没回）就算 failed：APS 重试最坏也就几秒 */
typedef struct {
    bool            active;       /* 有一条在途 */
    uint8_t         idx;          /* 设备下标 */
    uint8_t         cmd;          /* 发出去的 ZCL 命令号（默认应答里带回来） */
    uint8_t         level_pct;    /* 亮度命令的目标（%）：默认应答成功时乐观写进读数 */
    uint8_t         zcl_seq;
    uint16_t        cluster;
    uint16_t        msg_tag;      /* sendUnicast 的 messageTag */
    uint16_t        gen;          /* 应用命令 tag 里的代号 */
    bool            delivered;    /* messageSent 报送达了 */
    uint32_t        t_sent;
    zh_ctl_result_t result;       /* NONE = 还没结论 */
    uint8_t         zcl_status;   /* 设备默认应答里的状态（FAILED 时） */
    uint32_t        status;       /* sendUnicast / messageSent 的状态（FAILED 时） */
    uint32_t        t_done;       /* 有结论的时刻（算时延） */
    uint8_t         tuya_dp;      /* 走涂鸦路时：确认看设备回报的哪个 dp（cluster = ZH_CL_TUYA）…… */
    int32_t         tuya_raw;     /* ……回报的值要等于写的值（只看 dp 会被全量回报 / 旧亮度顶掉，审计 ⚠️） */
} zh_ctl_t;

typedef struct {
    zh_dev_t       dev[ZH_DEV_MAX];
    zh_dev_ops_t   ops;
    zh_dev_look_t  look[ZH_DEV_LOOK_N];
    struct { uint16_t nwk; uint32_t t; } gap[ZH_DEV_LOOK_N];   /* 最近查过的 nwk 与时刻（查询间隔用；环。要盖的那一格不满 10 s 就不开新查询，
                                                                  不然同一个 nwk 10 s 内会查第二次 —— 第一轮审计） */
    uint8_t        gap_head;
    uint16_t       gen;           /* 下一轮查询的代号 */
    uint32_t       t_age_chk;     /* 上次封顶检查的时刻 */
    uint8_t        iv_busy;       /* 有访谈请求在途的设备下标 + 1（0 = 没有）：同时只有一条 */
    uint8_t        iv_rr;         /* 轮到哪台（轮流推进，免得一台总在前面） */
    uint16_t       iv_gen;        /* 访谈请求的代号（编进 tag：迟到的应用命令结果对不上就不认） */
    uint32_t       t_iv_req;      /* 在途那条发出的时刻 */
    uint8_t        zcl_seq;
    bool           net_up;        /* 网络是 UP（zhub.c 每圈设）：不是 UP 时不推进访谈 —— 发出去也是白白用掉次数（第二轮审计） */
    bool           have_own_eui;  /* 网关自己的 EUI64（写 CIE 地址用；zhub.c 读到后设） */
    uint8_t        own_eui[8];    /* 小端（与 rec.ieee 同序） */
    uint32_t       t_now;         /* 最近一次入口调用给的时刻（访谈失败记时刻用） */
    bool           dirty;         /* 表变了（给 zhub.c 发布快照用；读完它清） */
    zh_ctl_t       ctl;           /* 在途的控制命令（M5 第七步） */
    uint16_t       ctl_gen;
    uint16_t       tuya_seq;      /* 涂鸦写命令负载里的 seq（同 zhc 的 sendDataPoints，各发各的计数） */
    bool           av_paused;     /* 可用性：上一拍网不是 UP（恢复的那一拍记 t_net_up） */
    uint8_t        cq_gen;        /* 待发槽的代号（编进 messageTag 的低 8 位） */
    uint32_t       t_net_up;      /* 网最近一次恢复成 UP 的时刻：可用性从这里和 t_seen 里晚的那个起算 */
    zh_disc_t      disc;          /* 主动发现（M6 第一步） */
    bool           have_xpan;     /* 本网的扩展 PAN（小端，= 线上字节序；zhub.c 读到网络参数后设）：过滤别的网的邻居条目 */
    uint8_t        xpan[8];
    zh_dev_stats_t st;
} zh_dev_core_t;

void zh_dev_init(zh_dev_core_t *d, const zh_dev_ops_t *ops);
/* 开机从存储加载一条（下标 = 键 dNN 的 NN）。版本 / IEEE 不对、下标越界、和已有的 IEEE 重复返回 false（不加载）。now：可用性从这里起算 */
bool zh_dev_load(zh_dev_core_t *d, unsigned idx, const zh_dev_rec_t *rec, uint32_t now_ms);
/* 开机时第 idx 格读不出来（不是"没有"）：这次开机保留，不分配给新设备。下标越界 / 已占用返回 false */
bool zh_dev_reserve(zh_dev_core_t *d, unsigned idx);
/* 每条 incomingMessageHandler：认识的更新 last_seen / 信号；不认识的（含标过 left 的旧短地址：可能已被别的设备占了）开始被动学习，查出来是它才算回来了 */
void zh_dev_on_rx(zh_dev_core_t *d, uint16_t sender, int8_t rssi, uint8_t lqi, uint32_t now_ms);
/* trustCenterPostJoinHandler */
void zh_dev_on_tc_join(zh_dev_core_t *d, uint16_t node, const uint8_t eui[8], uint8_t status, uint8_t decision, uint16_t parent, uint32_t now_ms);
/* ZDO 消息（profile 0）：Device_annce、IEEE_addr_rsp、访谈的三种应答；别的忽略 */
void zh_dev_on_zdo(zh_dev_core_t *d, uint16_t sender, uint16_t cluster, const uint8_t *pl, size_t len, uint32_t now_ms);
/* 其它 profile 的消息（ZCL）：访谈的读属性应答 / 默认应答；属性上报、IAS 通知 / 登记请求进读数（M5 第一步） */
void zh_dev_on_zcl(zh_dev_core_t *d, uint16_t sender, uint16_t profile, uint16_t cluster, uint8_t src_ep, const uint8_t *pl, size_t len, uint32_t now_ms);
/* 应用命令的结果（tag 是 zh_dev 自己编的；不是它的返回 false） */
bool zh_dev_on_app_result(zh_dev_core_t *d, uint32_t tag, zh_ses_result_t r, const zh_ezsp_frame_t *fr, uint32_t now_ms);
/* 网络是否 UP（zhub.c 每圈在 zh_dev_tick 之前设） */
void zh_dev_set_net_up(zh_dev_core_t *d, bool up);
/* 本网的扩展 PAN（小端；zhub.c 读到网络参数后设，NULL = 不知道 —— 不知道时不按它过滤） */
void zh_dev_set_ext_pan(zh_dev_core_t *d, const uint8_t xpan[8]);
/* 网关自己的 EUI64（小端；zhub.c 读到 getEui64 的结果后设，NULL = 不知道）：写 IAS 设备的 CIE 地址用 */
void zh_dev_set_own_eui(zh_dev_core_t *d, const uint8_t eui[8]);
/* 发一条控制命令（M5 第七步）。level_pct 只给 ZH_CTL_LEVEL（0–100；0 = 关）。返回 OK 后看 d->ctl.result（zh_dev_tick 到期限时给结论），
   zhub.c 取走结论后调 zh_dev_ctl_clear */
zh_ctl_err_t zh_dev_ctl_start(zh_dev_core_t *d, const uint8_t ieee[8], zh_ctl_act_t act, uint8_t level_pct, uint32_t now_ms);
void zh_dev_ctl_clear(zh_dev_core_t *d);
/* messageSentHandler（zhub.c 转来）：控制命令的送达结果 */
void zh_dev_on_msg_sent(zh_dev_core_t *d, uint16_t msg_tag, uint32_t status, uint32_t now_ms);
/* 定时：查询期限、访谈（超时、推进下一条）、年龄封顶（ZH_DEV_AGE_CAP_MS） */
void zh_dev_tick(zh_dev_core_t *d, uint32_t now_ms);
/* 有效 ZoneType：有 IAS Zone 输入簇、记录里有 ZoneType 的看记录；否则访谈完且是涂鸦表里报警类的按表（zh_tbl_zone_type）；别的照记录 */
uint16_t zh_dev_zone_type(const zh_dev_rec_t *r);
/* 访谈完的记录在涂鸦表里的设备下标（zh_tuya_find；厂商名 / 型号按 24 字节截断再比）；没访谈完 / 不在表里 -1 */
int  zh_dev_tuya_dev(const zh_dev_rec_t *r);
/* 记录的型号在小米表里的设备下标（zh_lumi_find）；**不要求访谈完**（小米的型号多半是从它主动上报学来的）；没有型号 / 不在表里 -1 */
int  zh_dev_lumi_dev(const zh_dev_rec_t *r);
/* 查找：返回下标，没有 -1 */
int  zh_dev_find_ieee(const zh_dev_core_t *d, const uint8_t ieee[8]);
int  zh_dev_find_nwk(const zh_dev_core_t *d, uint16_t nwk);
unsigned zh_dev_count(const zh_dev_core_t *d);

/* 应用命令的 tag（24 位，zh_host 原样交回，见 zh_host_core.h ZH_HOST_APP_TAG_MAX）：位 0–3 种类、4–7 槽号、8–23 这一格的代号 */
#define ZH_DEV_TAG_KIND_LOOKUP  1u
#define ZH_DEV_TAG_KIND_IEEE    2u
#define ZH_DEV_TAG_KIND_IV      3u      /* 访谈请求：槽号位不用，代号是 iv_gen */
#define ZH_DEV_TAG_KIND_MISC    4u      /* 不等结果的（IAS 登记应答、开机补读电量、涂鸦招呼） */
#define ZH_DEV_TAG_KIND_CTL     5u      /* 控制命令：代号是 ctl_gen */
#define ZH_DEV_TAG_KIND_QCTL    6u      /* 休眠设备的补发：位 8–15 设备下标、16–23 待发槽代号（M5 第十二步） */
#define ZH_DEV_TAG_KIND_DISC    7u      /* 主动发现的 LQI 表请求：代号是 disc.gen（M6 第一步） */
#define ZH_DEV_TAG_MAX          0x00FFFFFFu   /* zhub.c 静态断言它不超过 ZH_HOST_APP_TAG_MAX */
_Static_assert(ZH_DEV_LOOK_N <= 16u, "槽号在 tag 里只有 4 位");
