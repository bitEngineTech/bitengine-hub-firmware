#pragma once
/* pair_ble —— Muse 社区配对 v5 的 GATT 外设（docs/15 设计 5.2；改写自上游 esp32/main/ble_server.c，见 pair_ble.c 文件头）。
 * 组件内部用（muse_pair.c），不对外。BT 的起停归 ble_window：这里只提供服务表、广播与收发。
 *
 * 线程：GATT / GAP 回调都在 NimBLE host 任务里。照上游，握手本身在 host 任务里做（client_hello 的 ECDH、解密、回协议错误码，
 * 会拿 TX 锁发送，最长等 15 s）；要花时间的业务（device_info、wifi_scan、确认提示、provision）一律交给 muse_pair 的任务
 * （cb 必须 0 等待地投递，投不进去就丢、不在 host 任务里补发）。pair_ble_disconnect_pairing_session 不能在 host 任务里调。
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

struct ble_gatt_svc_def;

/* provision_v2 的参数（已校验）。所有权交给回调方，用完 pair_prov_free()（会把字符串清零再释放）。 */
typedef struct {
    char    *access_token;
    char    *refresh_token;
    char    *api_url_v2;     /* 可为 NULL */
    char    *noise_host;     /* 可为 NULL */
    uint32_t generation;     /* link_pairing_mark_provisioning_active() 给的会话代号 */
} pair_prov_t;

void pair_prov_free(pair_prov_t *p);

/* 都在 NimBLE host 任务里调，实现必须 0 等待地投递、不阻塞 */
typedef struct {
    void (*on_client_connected)(void);                 /* 手机订阅了 TX 通知 */
    void (*on_client_disconnected)(void);
    void (*on_get_device_info)(void);
    void (*on_wifi_scan)(uint32_t generation);         /* 已确认的会话 */
    void (*on_pairing_client_finished)(uint32_t generation);
    bool (*on_provision)(pair_prov_t *p);              /* 返回 false = 没接住（调用方会释放 p 并回 operation_in_progress） */
    void (*on_decrypt_failed)(void);                   /* 解密失败（设计 5.6：记一笔，上游随后断开） */
} pair_ble_cb_t;

/* 建锁、记下名字与回调。device_name 必须一直有效。 */
esp_err_t pair_ble_init(const char *device_name, const pair_ble_cb_t *cb);
const struct ble_gatt_svc_def *pair_ble_svcs(void);

/* 给 ble_window 的 host 回调 */
void pair_ble_on_sync(void);          /* 开广播 */
void pair_ble_on_reset(int reason);

/* 窗口关之前调（ble_window_close 之前、在 owner 自己的任务里）：等在途发送完、停广播、断开连接，此后发送与断开投递都不再碰 NimBLE。
   幂等；mark_dead 过就不再调 ble_gap_*，但照样 link_pairing_reset（等在途的 commit）。 */
void pair_ble_shutdown(void);
/* ble_window 强制拆 BT 之前（on_force_close 里同步调）：只置标志、不碰 NimBLE。可阻塞 ≤ 15 s（等在途发送）。 */
void pair_ble_mark_dead(void);

bool pair_ble_has_connection(void);
bool pair_ble_send_status(const char *status);                          /* 会话有密钥就加密，否则只有少数错误码能明文发 */
bool pair_ble_send_pairing_status(const char *status, uint32_t generation);
bool pair_ble_send_chunked(const char *json);                           /* 明文分片（device_info、client_hello 应答） */
bool pair_ble_send_encrypted_json(const char *json, uint32_t generation);
void pair_ble_disconnect_pairing_session(uint32_t generation);          /* 投到 host 队列里做，作废会话密钥并断开；不能在 host 任务里调 */
void pair_ble_device_info_done(void);   /* muse_pair 发完 device_info（或放弃）后调：下一条 get_device_info 才会再投递 */
void pair_ble_delayed_disconnect(uint32_t ms);
