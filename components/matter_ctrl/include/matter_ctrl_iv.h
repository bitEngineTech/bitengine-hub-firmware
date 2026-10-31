/* matter_ctrl_iv —— 一台 Matter 设备的访谈结果（docs/15 设计 9.4 图末段"读 Basic Information、Descriptor → 映射能力"；M7 第四步）。
 * 纯类型，不依赖 IDF / CHIP：hub_core 的能力映射（hub_mcap.c）与主机测试都用它。存储格式与编解码在 mc_iv_core.h。
 * 字符串是设备自报的原样字节（UTF-8，可能不合法），输出前一律按 6.8 净化（hub_sanitize_device_str）。 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define MC_IV_EP_MAX   6      /* 记几个端点（端点 0 = 根节点也算）。多出来的置 MC_IV_F_EP_TRUNC */
#define MC_IV_DT_MAX   3      /* 每个端点记几个设备类型 */
#define MC_IV_CL_MAX   24     /* 每个端点记几个服务端簇（只记标准簇 ≤ 0x7FFF；厂商簇不记） */
#define MC_IV_STR_MAX  32     /* VendorName / ProductName / NodeLabel 的上限（规范上限就是 32） */

#define MC_IV_F_DONE      0x01u   /* 访谈完整读完（没有它 = 读到一半 / 失败过） */
#define MC_IV_F_ICD       0x02u   /* 根节点有 IcdManagement 簇（间歇连接设备：订阅按它自己的间隔，9.5） */
#define MC_IV_F_EP_TRUNC  0x04u
#define MC_IV_F_CL_TRUNC  0x08u
#define MC_IV_F_BATTERY   0x10u   /* 有端点的 PowerSource 簇特性位带 BAT（0x2）：电池供电，报 battery 能力（9.6） */

typedef struct {
    uint16_t ep;
    uint8_t  n_dt, n_cl;
    uint32_t dt[MC_IV_DT_MAX];        /* 设备类型 ID（Matter Device Library） */
    uint16_t cl[MC_IV_CL_MAX];        /* 服务端簇 */
} mc_iv_ep_t;

typedef struct mc_iv_s {
    uint64_t   node_id;
    uint16_t   vid, pid;
    uint8_t    flags;
    uint8_t    n_ep;
    char       vendor[MC_IV_STR_MAX + 1];
    char       product[MC_IV_STR_MAX + 1];
    char       label[MC_IV_STR_MAX + 1];
    mc_iv_ep_t ep[MC_IV_EP_MAX];
} mc_iv_t;
