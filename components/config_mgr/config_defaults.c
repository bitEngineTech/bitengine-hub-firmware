#include "config_mgr.h"
#include <string.h>

void config_set_defaults(gateway_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->version = CONFIG_VERSION_CURRENT;

    // sys
    strlcpy(cfg->sys.hostname, "bitengine", sizeof(cfg->sys.hostname));
    /* memset 已经把它清零了；显式写出来是为了让读默认值的人知道它存在，
       以及"出厂不放行任何额外 Host"是一个决定，不是遗漏。 */
    cfg->sys.extra_host[0] = '\0';
    cfg->sys.bridge_mode = 0;  // BRIDGE_MODE_LAN
    cfg->sys.radio_region[0] = '\0';  // 未设置：NCP 上限 10 dBm、Thread 10 dBm（docs/14 Q4/Q5）
    cfg->sys.password_hash[0] = '\0';  // no password on first boot

    // wifi — empty = no credentials
    cfg->wifi.has_credentials = false;

    // eth
    cfg->eth.dhcp = true;

    // zigbee
    /* 出厂不预设信道：它是用户声明值，我们没有任何依据去猜。写 15 的老做法
       会被当成"用户说了 15"，而多数人用 Z2M（默认 11）。未声明就报 unknown。 */
    cfg->zigbee.channel          = 0;
    cfg->zigbee.channel_declared = false;
    /* 没有发射功率默认值：功率归 Z2M/ZHA 管（见 config_mgr.h 的说明） */

    // thread
    cfg->thread.enabled = true;    // Thread BR default ON
    cfg->thread.channel = THREAD_CHANNEL_DEFAULT;
    cfg->thread.rest_auth = false; // OTBR REST 默认不认证，理由见 config_mgr.h
    cfg->thread.otbr_token[0] = '\0';  // 出厂不设 token（同上，默认开放）

    // mqtt
    cfg->mqtt.enabled = false;
    cfg->mqtt.port = 1883;
    strlcpy(cfg->mqtt.topic_prefix, "bitengine", sizeof(cfg->mqtt.topic_prefix));
    cfg->mqtt.publish_interval = 30;
    cfg->mqtt.tls_enabled = false;

    // webhook
    cfg->webhook.enabled = false;
    cfg->webhook.backup_interval   = 86400;  // 24 hours
    cfg->webhook.event_zigbee_crash = true;
    cfg->webhook.event_network_down = true;
    cfg->webhook.event_interference = true;
    cfg->webhook.event_ota          = true;
    cfg->webhook.event_temp_high    = true;
    cfg->webhook.event_memory_low   = true;

    // led
    cfg->led.brightness          = 128;
    cfg->led.night_mode_enabled  = false;
    cfg->led.night_start_hour    = 22;
    cfg->led.night_start_min     = 0;
    cfg->led.night_end_hour      = 7;
    cfg->led.night_end_min       = 0;
    cfg->led.color_booting       = 0xFFFF00;  // yellow
    cfg->led.color_normal        = 0x00FF00;  // green
    cfg->led.color_zigbee        = 0x0000FF;  // blue
    cfg->led.color_error         = 0xFF0000;  // red
    cfg->led.color_ota           = 0xFF00FF;  // purple

    // whitelist
    cfg->whitelist.enabled = false;
    cfg->whitelist.count   = 0;

    // ntp
    strlcpy(cfg->ntp.server, "pool.ntp.org", sizeof(cfg->ntp.server));
    strlcpy(cfg->ntp.timezone, NTP_TIMEZONE_DEFAULT, sizeof(cfg->ntp.timezone));
    cfg->ntp.sync_interval = 3600;  // 1 hour
    cfg->ntp.tz_declared   = false; // 出厂 UTC+0 不算"用户设过"
}
