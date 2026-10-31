#pragma once
#include "esp_err.h"
#include <stdint.h>

esp_err_t mdns_svc_init(void);
esp_err_t mdns_svc_refresh(void);  /* re-announce our own services only (not SRP-proxied ones) */

/* _meshcop._udp 的 xa / nn / xp（以及其余 meshcop 项）由 IDF 的 BR 库发布并维护：角色、网络名、扩展 PAN ID、
   活动数据集等一变它就重发（esp_openthread_state.c 的 ESP_OPENTHREAD_BORDER_ROUTER_FLAG_OF_INTEREST），
   填的是原始字节（HA 要的格式）。我们只覆盖 vn/mn 品牌（下面）。2026-10-22 删掉了自己写 xa/nn/xp 的
   mdns_svc_update_meshcop_txt()：开机那次在服务发布前调，必然失败，每次开机三条 E 级
   "Service doesn't exist"；推数据集后那次只是重复库已经写好的值。 */

/* 把 BR 库发布的 _meshcop._udp 的 vn/mn 覆盖成本产品品牌（库填的是
   OpenThread / BorderRouter，HA 的 Thread 面板拿它当厂商/型号显示）。
   服务尚未发布时返回 ESP_ERR_NOT_FOUND —— 调用方重试，别猜时间。 */
esp_err_t mdns_svc_brand_meshcop(void);
