#pragma once
#include <stdint.h>
/* config_mgr.h 里只有网络配置结构体用到这些类型；主机测试只碰 zigbee 段。 */
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { uint32_t ip; uint32_t netmask; uint32_t gw; } esp_netif_ip_info_t;
