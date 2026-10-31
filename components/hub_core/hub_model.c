/* hub_model —— 能力位与对外名字（见 include/hub_model.h）。纯 C，不依赖 ESP-IDF。
 * 名字与附录 A hub.devices.list 的 capability 参数说明一致（那里列了除 identify 外的 20 个；identify 只用于动作检查，
 * 不出现在过滤参数里）。 */
#include "hub_model.h"

#include <string.h>

static const char *const k_cap_names[HUB_CAP_COUNT] = {
    "onoff", "level", "color_temp", "color", "cover", "lock", "thermostat",
    "temperature", "humidity", "illuminance", "pressure", "contact", "motion",
    "water_leak", "smoke", "vibration", "battery", "power", "energy", "button", "identify",
};

const char *hub_cap_name(uint32_t cap_bit)
{
    if (cap_bit == 0 || (cap_bit & (cap_bit - 1)) != 0) return NULL;   /* 不是恰好一位 */
    for (unsigned i = 0; i < HUB_CAP_COUNT; i++) {
        if (cap_bit == (1u << i)) return k_cap_names[i];
    }
    return NULL;
}

uint32_t hub_cap_from_name(const char *name)
{
    if (!name) return 0;
    for (unsigned i = 0; i < HUB_CAP_COUNT; i++) {
        if (strcmp(name, k_cap_names[i]) == 0) return 1u << i;
    }
    return 0;
}
