#pragma once
/* pair_core —— muse_pair 胶水层里不依赖 ESP-IDF 的判据（docs/15 设计第五节）。test_host 原样编进去测。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 配对结果（hubdb muse/pair_res，设计 4.4、5.6；TC-MU01–03 靠它判对错）。数值进 flash，只许追加、不许改号。 */
typedef enum {
    PAIR_RES_NONE = 0,
    PAIR_RES_OK = 1,
    PAIR_RES_WINDOW_TIMEOUT = 2,     /* 窗口到时没配完（含 App 找不到设备） */
    PAIR_RES_CANCELLED = 3,          /* 用户在网页上取消 */
    PAIR_RES_DECRYPT_FAILED = 4,     /* 解密失败（中间人或版本不匹配） */
    PAIR_RES_CONFIRM_TIMEOUT = 5,    /* 60 s 内没按键确认 / 确认后 120 s 没收到凭据 */
    PAIR_RES_AUTH_FAILED = 6,        /* fetch_vms 验证不过（SDK token 无效或已撤销？） */
    PAIR_RES_STORAGE_FAILED = 7,     /* 落盘失败 */
    PAIR_RES_NO_SDK_TOKEN = 8,       /* 配对剖面里读不到 SDK token */
    PAIR_RES_API_UNREACHABLE = 9,    /* 开窗前连不上 api.muse.ai */
    PAIR_RES_BT_FAILED = 10,         /* BLE 窗口开不起来 / BT 出错 */
    PAIR_RES_NO_NETWORK = 11,        /* 配对剖面里没网 */
    PAIR_RES_INTERNAL = 12,          /* 内存不足、任务起不来之类 */
    PAIR_RES_INTERRUPTED = 13,       /* 配对剖面开始时先写这一条：没走到收尾（panic / 断电 / 看门狗）就停在这里 */
    PAIR_RES_MAX
} pair_res_t;

const char *pair_res_str(pair_res_t r);

/* pair_res 的 blob（不含 hub_store 的格式版本字节）：code(1) + 结束时的墙钟 epoch 秒（4，小端；没对时为 0）。 */
#define PAIR_RES_BLOB_LEN 5
void pair_res_encode(pair_res_t r, uint32_t epoch_s, uint8_t out[PAIR_RES_BLOB_LEN]);
bool pair_res_decode(const uint8_t *in, size_t len, pair_res_t *r, uint32_t *epoch_s);

/* provision_v2 校验（照上游 Linux SDK ble_setup.py：ssid / password 只要求是字符串、内容忽略 —— 开放网络时密码为空；
   access / refresh token 非空；token_type 必须是 "device"）。另加令牌长度上限（设计 4.4："超限报 error_missing_credentials"）。
   *_is_str 为调用方从 JSON 判出的"这个字段是字符串"。 */
bool pair_provision_ok(bool ssid_is_str, bool password_is_str, const char *access_token,
                       const char *refresh_token, const char *token_type);

/* 照抄上游 ble_server.c 的 command_json_depth_valid（≤ 32 层）：cJSON 本项目的嵌套上限是 1000，明文帧与解密后的帧都要先过这一道。 */
bool pair_json_depth_valid(const uint8_t *data, size_t len);

/* 身份（设计 4.3，照上游 identity.c）：取 Wi-Fi STA MAC。缓冲至少 32 / 32 / 18 / 48 字节。 */
void pair_identity_format(const uint8_t mac[6], char *node_id, size_t node_cap, char *ble_name, size_t ble_cap,
                          char *mac_str, size_t mac_cap, char *device_id, size_t dev_cap);

/* wifi_scan 的回包（设计 5.5，照 Linux SDK）：在线时只有一条"当前连接"、标成开放网络（App 就不要密码），离线时空列表。
   返回写入长度，装不下返回 0。 */
size_t pair_wifi_scan_json(bool online, char *out, size_t cap);

/* 下发的 api_url_v2 怎么用：合法的 https:// 地址照用，否则当作没下发（用默认地址；照 Linux SDK _verify_and_save）。 */
const char *pair_effective_api_url(const char *api_url_v2);
