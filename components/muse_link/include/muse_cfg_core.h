#pragma once
/* muse_cfg_core —— muse_cfg 里不依赖 ESP-IDF 的判据（test_host 原样编进去测）。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MUSE_SDK_TOKEN_LEN     48      /* 上游 IDENTITY_SDK_TOKEN_LEN："mgst_" + 32 字节的 base64url（43 字符） */
#define MUSE_DISPLAY_NAME_MAX  32      /* 字节（设计 4.3） */
/* 鉴权头缓冲 2048（vm_api.c:272/394/447），减去 "Bearer " / "Bearer hatch_refresh:" 前缀（UPSTREAM.md 调用方义务） */
#define MUSE_ACCESS_TOKEN_MAX  2040
#define MUSE_REFRESH_TOKEN_MAX 2026
#define MUSE_URL_MAX           128     /* api_url2 / noise_host（设计 4.4） */
#define MUSE_VM_ID_MAX         64      /* vm_id（设计 4.4） */

/* 照上游 identity_sdk_token_valid()：正好 48 字符、"mgst_" 开头、其余是 base64url、末字符只能是 "AEIMQUYcgkosw048"
   之一（32 字节编码后末位只带 4 位）。ASCII 判断不经 isalnum（不受 locale 影响）。 */
bool muse_sdk_token_valid(const char *token);

/* 日志用的提示："mgst_… (len 48)"；永远不带 mgst_ 之后的字符。out_cap ≥ 24。 */
void muse_secret_hint(const char *secret, char *out, size_t out_cap);

/* 显示名校验：非空、≤ 32 字节、合法 UTF-8、不含任何 ASCII 控制字符（含换行）。 */
bool muse_display_name_valid(const char *name);

/* 服务端下发的 URL：空串合法（= 用默认）；非空必须 "https://" 开头、≤ MUSE_URL_MAX、只含可打印 ASCII、不含空白与引号。 */
bool muse_api_url_valid(const char *url);
/* noise_host：空串合法；非空是主机名（字母数字 . -，≤ MUSE_URL_MAX，不以 . / - 开头）。 */
bool muse_noise_host_valid(const char *host);

/* 令牌：非空、不超上限、只含可打印 ASCII（放进 HTTP 头，不许有 CR/LF）。 */
bool muse_token_valid(const char *tok, size_t max_len);
/* 侧边会话 id（chat_sid，4.4 / 4.9）：小写 v4 风格 UUID 文本，36 字符，8-4-4-4-12，只含 [0-9a-f-]（我们自己用 mp_format_uuid 生成） */
#define MUSE_CHAT_SID_LEN 36
bool muse_chat_sid_valid(const char *s);

/* VM id（device.set_vm 选中的那台，4.13；它会进 WS 升级请求的 vm_id 查询参数）：非空、≤ MUSE_VM_ID_MAX，
   只含字母数字与 '-'（同上游 vm_derive_id 认的字符集，vm_connect.h）。 */
bool muse_vm_id_valid(const char *id);

/* node_id（4.3）："homelink-" + STA MAC 后 3 字节的小写十六进制。必须与配对时发给 App 的完全一致
   （muse_pair/pair_core.c 的 pair_identity_format；muse_pair 依赖本组件，不能反过来调它 —— 两处的格式串由 VERIFY 钉成逐字相同）。
   mac 为 NULL 按全零。 */
void muse_identity_node_id(const uint8_t mac[6], char *out, size_t cap);
