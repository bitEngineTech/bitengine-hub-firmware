#pragma once
/* hub_mcap —— Matter 设备的能力与设备类别（docs/15_muse_direct_design_v1.md 9.6；M7 第四步）。纯逻辑，test_host/ 原样编。
 * 输入是 matter_ctrl 的访谈结果（matter_ctrl_iv.h）。类别的词表与 Zigbee（hub_zcap）同一套。
 * 簇与设备类型 ID 取自 Matter 1.4 Device Library / 应用簇规范（9.6"设备类型与簇 ID 以 Matter 1.4 Device Library 为准"）。 */
#include <stdint.h>
#include "matter_ctrl_iv.h"

uint32_t    hub_mcap_caps(const mc_iv_t *iv);                    /* HUB_CAP_* 位；访谈没完成也按已读到的算 */
const char *hub_mcap_kind(const mc_iv_t *iv, uint32_t caps);     /* 访谈没完成回 "unknown" */
