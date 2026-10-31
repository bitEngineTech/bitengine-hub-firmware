#include "ezsp_bridge.h"
#include "esp_timer.h"
#include <string.h>

static bridge_stats_t s_stats;
static int64_t        s_start_time;

void bridge_stats_init(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
    s_start_time = esp_timer_get_time();
}

bridge_stats_t *bridge_stats_get(void)
{
    s_stats.uptime_seconds = (uint32_t)((esp_timer_get_time() - s_start_time) / 1000000);
    /* 有客户端就把"最近见到协调器"推到现在。放在 get() 里而不是 tcp_server 的
       连接/断开两处：断开那一处只有在**最后一个**客户端走掉时才该定格，
       两个客户端轮流断开时按事件记会把时间点记错。 */
    if (s_stats.active_clients > 0) {
        s_stats.last_client_seen_s = s_stats.uptime_seconds;
    }
    return &s_stats;
}

void bridge_stats_reset(void)
{
    uint8_t clients = s_stats.active_clients;
    /* last_client_seen_s 跟 uptime 同一时基，而下面会把 s_start_time 也归零，
       所以这里不能保留旧值（那是另一个时基的数）。清成 0 = "本次计数周期内
       还没见过协调器"，与开机初值一致。 */
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.active_clients = clients;
    s_start_time = esp_timer_get_time();
}
