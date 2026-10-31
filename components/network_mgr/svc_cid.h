/* components/network_mgr/svc_cid.h — 每张卡的 DHCP client-id（T9.37 服务身份，组件私有）
 *
 * 一张两项的小表：网口、WiFi 各报哪个身份（svc_cid_t：本卡 / 对方 / 哑身份）。network_mgr.c 改表，
 * lwIP 线程里的钩子（bitengine_dhcp_append_options）每发一个 DHCP 报文读一次。改了表之后**要重启那张卡的 DHCP**
 * 才生效到新的一轮申请上 —— 交接的完整步骤在 network_mgr.c 的 svc_id_handover（设计 docs/16 §5）。 */
#pragma once

#include "esp_netif.h"
#include "svc_policy.h"

/* network_mgr_init 里调一次（两张卡的 netif 都建好之后）。没绑之前、或不认识的口，钩子一律报本卡 MAC，
   与 IDF 默认的 option 61 逐字节相同 —— 现有租约不受影响。eth 可为 NULL（以太网初始化失败）。 */
void svc_cid_bind(esp_netif_t *eth, esp_netif_t *sta);

/* 任意任务：设某张卡报哪个身份。临界区里改一个字节，不阻塞。 */
void svc_cid_set(svc_nic_t nic, svc_cid_t kind);
svc_cid_t svc_cid_get(svc_nic_t nic);

/* 这张卡自上次清除以来发过 DHCPDECLINE 没有（钩子在 lwIP 线程里置）。交接时清、判地址时读：
   发过 DECLINE 说明 DHCP 的 ARP 检查撞上了别人（地址冲突、AP 代答），那一轮拿到的地址不能拿来判"路由器只认 MAC"。 */
bool svc_cid_declined(svc_nic_t nic);
void svc_cid_clear_declined(svc_nic_t nic);

/* 这张卡最近一个 DHCPREQUEST（换来 ACK、拿到地址的那个报文）实际带的身份（钩子在 lwIP 线程里记）。判"交换着拿到的"
   一律用它，不用表：表可能在 DHCP 已经发出去之后才改（2026-10-08 隔离网段实测：网口链路先于 network_mgr 初始化起来，
   IDF 用默认表发了第一个 DISCOVER / REQUEST，判定却按改过的表以为是交换着拿的）。没发过时是 SELF。 */
svc_cid_t svc_cid_sent(svc_nic_t nic);

/* 这张卡开机以来在 RENEWING / REBINDING 态发出的 DHCPREQUEST 个数（钩子在 lwIP 线程里数）。续租在 T1 时单播给原服务器，
   被 ACK 就不会走到重绑；重绑计数在涨 = T1 的续租没被接受（T9.37 长时间续租测试的判据之一，测试 6e R5）。 */
void svc_cid_counts(svc_nic_t nic, uint32_t *renew_reqs, uint32_t *rebind_reqs);
