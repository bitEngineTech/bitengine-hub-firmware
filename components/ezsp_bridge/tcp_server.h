#pragma once

#include "esp_err.h"
#include "ezsp_bridge.h"

esp_err_t tcp_server_start(void);
void      tcp_server_stop(void);
void      tcp_server_get_clients(tcp_client_info_t *out, int *out_count);
esp_err_t tcp_server_disconnect_client(int index);
/* 重启前干净断开所有客户端（发 FIN）。已挂 esp_register_shutdown_handler，
   覆盖全部 esp_restart() 路径，正常情况下不需要手工调用。 */
void tcp_server_close_all_clients(bool settle);
esp_err_t tcp_server_disconnect_by_ip(const char *ip, int *out_closed);
