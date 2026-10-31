/* components/network_mgr/lwip_hooks/bitengine_lwip_hooks.h — 挂进 lwIP 的钩子头文件（T9.37 服务身份）
 *
 * 顶层 CMakeLists.txt 用 ESP_IDF_LWIP_HOOK_FILENAME 把它注入 lwip 组件（同 IDF 示例 examples/network/vlan_support）。
 * lwip_default_hooks.h 开头就 include 本文件；dhcp.c 先经 opt.h → lwipopts.h 定义了默认的
 * LWIP_HOOK_DHCP_APPEND_OPTIONS，之后才 include LWIP_HOOK_FILENAME —— 所以这里 #undef / #define 能生效
 * （核过的出处见 docs/16_service_identity_dual_float_design_v1.md §11）。
 *
 * 只要 IDF 默认的 option 61 关着（CONFIG_LWIP_DHCP_DISABLE_CLIENT_ID=y，sdkconfig.defaults 无条件打开）就接管：
 * T9.37 服务身份打开时每张卡报哪个身份由 svc_cid.c 的表决定；关着时表不绑定，一律报本卡 MAC —— 与 IDF 默认逐字节相同。
 * 2026-10-07 第二轮审计：以前只在服务身份打开时接管，而 Kconfig 那一行是无条件的，关掉服务身份的构建会**完全不带** client-id，
 * 有的服务器会给两张卡都换新地址。 */
#pragma once

#include "sdkconfig.h"

#if CONFIG_BITENGINE_SVC_IDENTITY && !ESP_DHCP_DISABLE_CLIENT_ID
/* 两份都开着报文里会有两个 client-id，哪个算数看服务器 —— 交接时的哑身份可能不起作用，RELEASE 会退掉真租约 */
#error "T9.37 服务身份需要 CONFIG_LWIP_DHCP_DISABLE_CLIENT_ID=y（option 61 由 svc_cid.c 的钩子按网卡附）"
#endif

#if ESP_DHCP_DISABLE_CLIENT_ID
#include "lwip/arch.h"

struct netif;
struct dhcp;
struct dhcp_msg;

/* lwIP 线程。实现在 components/network_mgr/svc_cid.c */
void bitengine_dhcp_append_options(struct netif *netif, struct dhcp *dhcp, u8_t state,
                                   struct dhcp_msg *msg, u8_t msg_type, u16_t *len);

#undef LWIP_HOOK_DHCP_APPEND_OPTIONS
#define LWIP_HOOK_DHCP_APPEND_OPTIONS(netif, dhcp, state, msg, msg_type, options_len_ptr) \
        bitengine_dhcp_append_options(netif, dhcp, state, msg, msg_type, options_len_ptr)
#endif /* ESP_DHCP_DISABLE_CLIENT_ID */
