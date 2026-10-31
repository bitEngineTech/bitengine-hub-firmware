#pragma once
/* muse_cfg —— hubdb 命名空间 muse 里的设置与凭据（docs/15_muse_direct_design_v1.md 4.4；只在 BITENGINE_ENABLE_MUSE_LINK 下编）。
 *
 * 落盘一律经 hub_store（hubdb 的唯一写入者）。秘密项（sdk_tok、acc_tok、ref_tok）：
 *  - 对外只报 set / unset，不进日志（日志只打前缀和长度，如 "mgst_… (len 48)"），不进配置导出；
 *  - 读出来的缓冲用完调用方自己清零（mbedtls_platform_zeroize 之类）。
 * 线程：可从任何任务调（hub_store 的规则），不能在 ISR / esp_timer 回调里调（红线 10）。
 */
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "hub_model.h"
#include "muse_cfg_core.h"

/* 写 SDK token：格式不对 ESP_ERR_INVALID_ARG（不落盘）；NULL 或空串 = 删掉。 */
esp_err_t muse_cfg_set_sdk_token(const char *token);
/* 读 SDK token 到 out（cap ≥ MUSE_SDK_TOKEN_LEN + 1）。没有 ESP_ERR_NOT_FOUND；存着的不合法也当没有（打 W）。 */
esp_err_t muse_cfg_get_sdk_token(char *out, size_t cap);
bool      muse_cfg_sdk_token_set(void);

esp_err_t muse_cfg_set_enabled(bool en);
bool      muse_cfg_enabled(void);

/* 显示名：按用户名字的规矩净化（UTF-8、去控制字符、拒绝换行、≤ 32 字节）；不合格 ESP_ERR_INVALID_ARG。
   NULL 或空串 = 恢复默认（删键）。读：没设过时给默认名（"bitEngine Hub " + 序列号后 4 位；没有出厂身份时用 MAC 后 4 位）。 */
esp_err_t muse_cfg_set_display_name(const char *name);
void      muse_cfg_get_display_name(char *out, size_t cap);

/* 配对结果落盘（设计 5.5 第 4 步）：四个键各 put 一次，然后 flush、逐个读回比对。任何一步不对都返回错误，
   并把已写的删掉（不留半套凭据）。在 link_pairing_commit_provisioning() 的回调里同步调。 */
typedef struct {
    const char *access_token;    /* 必填，≤ MUSE_ACCESS_TOKEN_MAX */
    const char *refresh_token;   /* 必填，≤ MUSE_REFRESH_TOKEN_MAX */
    const char *api_url_v2;      /* 可空；非空必须是 https:// 开头 */
    const char *noise_host;      /* 可空 */
} muse_cfg_pairing_t;
esp_err_t muse_cfg_save_pairing(const muse_cfg_pairing_t *p, uint32_t flush_timeout_ms);
bool      muse_cfg_paired(void);  /* 有 access token（显示用：读不到也算 false） */
/* 会话任务开机用：两个标志都**确定**读到了才返回 ESP_OK（没存过 = false，也算确定）；hub_store 没就绪 / 超时 / 缺内存
   返回那个错误、不改 *enabled / *paired —— 不能把"一时读不到"当成"没开 / 没配对"。 */
esp_err_t muse_cfg_load_flags(bool *enabled, bool *paired);

/* ---- 会话层用（M3，设计 4.4–4.6、4.10、4.13） ---- */

/* 读设备令牌到 out（cap ≥ 对应上限 + 1）。用完调用方清零。
   ESP_ERR_NOT_FOUND **只**表示确实没有可用的令牌（没存、版本比本固件新、超长、内容不合格）—— 调用方据此走"凭据没了"；
   其余错误（ESP_ERR_TIMEOUT、ESP_ERR_INVALID_STATE（hub_store 没起来）、ESP_ERR_NO_MEM……）是一时读不到，按可重试的
   故障处理，**不许**据此清凭据（第四步审计 ❌：存储超时一次就把配对删掉）。 */
esp_err_t muse_cfg_get_access_token(char *out, size_t cap);
esp_err_t muse_cfg_get_refresh_token(char *out, size_t cap);
/* 刷新后换新令牌对：两个都 put → flush → 读回比对。失败返回错误（旧值可能已被部分覆盖：调用方当刷新失败，下轮再刷）。 */
esp_err_t muse_cfg_save_tokens(const char *access_token, const char *refresh_token, uint32_t flush_timeout_ms);
/* 解绑 / 撤销（4.10、4.13）：删 acc_tok、ref_tok、vm_id → flush → 读回确认都不在了。sdk_tok 与各项设置保留。
   device.unpair 要"先删后回"：这里返回 ESP_OK 才能回 accepted。 */
esp_err_t muse_cfg_clear_pairing(uint32_t flush_timeout_ms);

/* 生效的 API 地址：配对下发的 api_url_v2（已验 https://），确实没有（没存 / 版本更新 / 不合格）时 Kconfig 默认
   （BITENGINE_MUSE_API_URL），都返回 ESP_OK。一时读不到（超时、hub_store 没就绪……）返回那个错误、**不写 out** ——
   调用方要保留上一次的值、过会儿再读，不能拿默认值顶（配对下发的地址和默认不同时，用默认会 401 → 刷新被拒 → 清凭据；
   第四步第二轮审计 ❌）。 */
esp_err_t muse_cfg_get_api_url(char *out, size_t cap);
/* 生效的 noise_host：同上（默认 BITENGINE_MUSE_NOISE_HOST） */
esp_err_t muse_cfg_get_noise_host(char *out, size_t cap);

/* 用户在 App 里用 device.set_vm 选的 VM（4.13）；确实没有 ESP_ERR_NOT_FOUND，一时读不到原样返回错误。
   set 传 NULL / 空串 = 清掉（回默认 VM）。 */
esp_err_t muse_cfg_get_vm_id(char *out, size_t cap);
esp_err_t muse_cfg_set_vm_id(const char *vm_id);

/* 侧边会话 id（4.4 chat_sid，4.9）：第一次往侧边会话推送时生成并存下。确实没有 ESP_ERR_NOT_FOUND（调用方生成新的），
   一时读不到原样返回错误（调用方这次不推侧边会话，别生成新 id：会另开一个侧边会话）。out 至少 MUSE_CHAT_SID_LEN + 1。 */
esp_err_t muse_cfg_get_chat_sid(char *out, size_t cap);
esp_err_t muse_cfg_set_chat_sid(const char *sid);

/* 权限位（6.5，hub_registry.h 的 HUB_PERMBIT_*）：没设过返回出厂默认（CONTROL | MANAGE，SENSITIVE 关）；
   一时读不到、或存着的值带不认识的位，返回 0（只读）—— 读不准时往严里取（N4），不放开成默认 */
uint8_t   muse_cfg_get_perm(void);
/* 同上，但告诉调用方读得准不准：ESP_OK（含"没设过 → 出厂默认"）；一时读不到 / 存着的值不认识时 *out = 0 并返回错误
   （注册用它：读不准就按只读注册，过会儿重注册一次） */
esp_err_t muse_cfg_read_perm(uint8_t *out);
esp_err_t muse_cfg_set_perm(uint8_t perm);

/* 推送类别开关（4.4 muse/notify，位见 hub_model.h 的 HUB_NOTIFY_*）：没存过返回 HUB_NOTIFY_DEFAULT；一时读不到返回错误、不写 out
   （调用方沿用现值，不回落成默认 —— 用户关掉的类别不该因一次读失败又推起来）；带不认识的位只留认识的。 */
esp_err_t muse_cfg_read_notify(uint16_t *out);
esp_err_t muse_cfg_set_notify(uint16_t mask);
