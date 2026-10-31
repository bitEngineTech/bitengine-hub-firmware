#pragma once
/* muse_proto —— Muse 会话里控制面报文的编解码（docs/15_muse_direct_design_v1.md 4.2 注 3、4.7、4.8、4.13）。
 * 纯 C，不依赖 ESP-IDF（用 cJSON 解析入站报文）：test_host/ 原样编它。会话任务（M3 muse_session）调用。
 *
 * 报文格式照上游 esp32/main/noise_control.cpp（e6fe517；H1 已逐字段对照，行号见各函数注释）：
 *   注册   {"type":"req","id":<uuid>,"method":"link.register","params":{node_id, display_name, platform, version,
 *           device_family, model_id, is_wakeup_supported, metadata, commands_v2}}            —— :1366-1559
 *   应答   {"type":"res","id":<同上>,"result":{"status":"registered",…}} 或带 "error"         —— :1929-1955
 *   调用   {"method":"link.invoke","id":…,"command":…,"params":{…}}                            —— :1708-1740
 *   结果   {"method":"link.result","id":…,"ok":bool,"payload":{…},"error":"<一句话>"}         —— :1819-1860
 *   事件   {"type":"event","event":"link.unpaired" | "node.unpaired"}                          —— :1714-1724
 *   心跳   {"method":"link.heartbeat"}                                                          —— :2290
 * 与上游 ESP32 的有意差异：注册被拒还认 "ok":false（Linux SDK link_client.py:536 的判法；ESP32 的 898f846 只看 error，
 * 我们取两者的并集），协议错误还看 error.code（"unsupported_version" 这种不带 message 的也认）；错误一句话统一写成
 * "<code>: <面向用户的解释>"（4.8）；不发 metadata（不送 SSID，4.7 —— 上游 ESP32 没有 SSID 时整个键都不发，Linux SDK
 * 从来不发，G0-2 用的正是 Linux SDK，空对象反而没人在真服务端上验过）。
 * 注册被拒时 reason 里是服务端给的文字：不可信，进 last_error / 网页前要过 hub_sanitize 再转义。
 * 入站报文进 cJSON 之前必须先过 mp_json_shape_ok（见下）。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "hub_json.h"
#include "hub_registry.h"

#define MP_UUID_LEN          36      /* 不含 NUL */
#define MP_REGISTER_MAX      7168    /* 整条注册 ≤ 7 KB（4.7：附录 A 九条全开约 5.5 KB，留余量；上游缓冲 8 KB） */
#define MP_INVOKE_MAX        4096    /* 入站调用报文上限（4.8）：超过的不进 cJSON，只扫出 id 回 invalid_params */
#define MP_RESULT_MAX        65536   /* 结果上限（4.8） */
#define MP_JSON_MAX_DEPTH    16      /* 入站报文进 cJSON 前的形状上限（见 mp_json_shape_ok） */
#define MP_JSON_MAX_VALUES   64      /* cJSON 每个节点（32 位约 40 B）与每个键 / 字符串都是一次小分配，按 SPIRAM_MALLOC_ALWAYSINTERNAL
                                        全落内部 RAM：256 个值最坏约 12–17 KB（第四步第二轮审计实测）。附录 A 的命令参数最多 4 个，
                                        真实调用报文不到 20 个值，64 留够余量，最坏约 8 KB（节点 + 4 KB 内的字符串），同一时刻只有一条 */

/* 16 字节随机数 → v4 UUID 文本（同上游 make_uuid 的版本位与变体位）。out 至少 MP_UUID_LEN + 1。 */
void mp_format_uuid(const uint8_t rnd[16], char *out);

/* ---------------- 注册（4.7） ---------------- */

typedef struct {
    const char *node_id;         /* "homelink-xxxxxx"（4.3） */
    const char *display_name;    /* ≤ 32 字节，已净化 */
    const char *version;         /* app 版本 */
    const char *device_family;   /* Kconfig 常量（MD-4） */
    const char *model_id;        /* Kconfig 常量 */
} mp_reg_info_t;

/* 写整条注册报文。命令表按 perm × backends 过滤（hub_registry_emit_commands_v2）。
   返回长度；参数缺失、放不下（宁可不注册也不发半张表）返回 -1。cap 超过 MP_REGISTER_MAX 时按 MP_REGISTER_MAX 算。 */
int mp_build_register(char *buf, size_t cap, const char *req_id, const mp_reg_info_t *info, uint8_t perm, uint8_t backends);

typedef enum {
    MP_REG_NOT_MINE = 0,         /* 不是这次注册的应答（id 不符 / 不是对象） */
    MP_REG_ACKED,                /* 注册成功 */
    MP_REG_REJECTED,             /* 被拒，退避后重试（60 s 起逐步拉长到 30 min，muse_link_core） */
    MP_REG_REJECTED_PROTOCOL,    /* 被拒且原因带 "unsupported" / "version"：协议变了，进 PROTOCOL_ERROR */
} mp_reg_reply_t;

/* 判 link.register 的应答（4.2 注 3）：非空、非 false 的 "error"（字符串，或带 "message" / "code" 的对象），或 "ok":false，
   都是被拒。原因（message，其次 code）里带 "unsupported" / "version" 的（不分大小写）判为协议变了。
   *heartbeat：type 为 "res"、result.status 为 "registered"、而且回包里根本没有 "error" 键（照上游 :1956-1960，
   连 "error":null 也不发心跳）。reason 收被拒的原因（可为 NULL）。 */
mp_reg_reply_t mp_classify_register_reply(const cJSON *msg, const char *req_id, bool *heartbeat, char *reason, size_t reason_cap);

/* ---------------- 入站（4.8、4.10、4.13） ---------------- */

typedef enum {
    MP_IN_IGNORE = 0,            /* 不认识的消息（含注册应答之外的 res）：不回 */
    MP_IN_UNPAIRED,              /* link.unpaired / node.unpaired 事件（4.10） */
    MP_IN_INVOKE,                /* link.invoke，id 与 command 都在 */
    MP_IN_INVOKE_BAD,            /* link.invoke 但缺 id 以外的东西（command 不是字符串、params 不是对象）：回 invalid_params */
} mp_in_kind_t;

typedef struct {
    const char  *id;             /* 指向 msg 里的字符串（msg 释放前有效） */
    const char  *command;
    const cJSON *params;         /* 没带 params 时为 NULL */
} mp_invoke_t;

/* 没有 id 的 link.invoke 回不了：当 IGNORE（同上游）。 */
mp_in_kind_t mp_classify_inbound(const cJSON *msg, mp_invoke_t *inv);

/* **调用方义务**：任何入站报文进 cJSON_Parse 之前先过这一关（2026-10-22 第二轮审计 a5）。4 KB 以内照样能写出 ~990 层
   嵌套 —— cJSON 递归解析每层约 80 B 栈，muse_sess 的 12 KB 栈直接溢出、整机重启（违反 N7）；扁平的 2000 个值又会因为
   SPIRAM_MALLOC_ALWAYSINTERNAL 把小节点全落进内部 RAM（瞬时几十 KB）。线性扫描、不分配内存：嵌套深度 ≤ max_depth、
   值的个数（大致 = 左括号数 + 逗号数 + 1）≤ max_values、字符串都闭合，才返回 true。 */
bool mp_json_shape_ok(const char *json, size_t len, unsigned max_depth, unsigned max_values);

/* 不分配内存地扫出顶层对象里某个键的字符串值（只认不含转义的字符串）。给超过 MP_INVOKE_MAX 的报文用：
   不进 cJSON 也能拿到 id 回 invalid_params。找到返回 true。 */
bool mp_scan_top_string(const char *json, size_t len, const char *key, char *out, size_t cap);

/* ---------------- 调用分派（4.8、4.13） ---------------- */

typedef enum { MP_MGMT_NONE = 0, MP_MGMT_UNPAIR, MP_MGMT_LIST_VMS, MP_MGMT_SET_VM, MP_MGMT_RESET_VM } mp_mgmt_t;
mp_mgmt_t mp_mgmt_lookup(const char *command);   /* 4 个只处理、不注册的管理命令 */

typedef enum {
    MP_DISP_UNSUPPORTED = 0,     /* 不在本次注册的命令表里（含 device.ota 等永不注册的） */
    MP_DISP_REGISTRY,            /* 交给 hub_core 的命令表 */
    MP_DISP_MGMT,                /* 管理命令 */
} mp_disp_t;

/* reg_perm / reg_backends 是**本次注册时**用的值：没注册过的命令一律 unsupported（4.8 第 2 步），哪怕之后权限变了 */
mp_disp_t mp_dispatch(const char *command, uint8_t reg_perm, uint8_t reg_backends, const hub_cmd_t **cmd, mp_mgmt_t *mgmt);

/* 必填参数都在、已知参数类型对（"string" / "integer" / "boolean"）；不认识的参数忽略。
   不过时把面向用户的解释写进 err（如 "missing required parameter 'id'"）。params 可为 NULL（= 空对象）。 */
bool mp_check_params(const hub_cmd_t *c, const cJSON *params, char *err, size_t err_cap);

/* ---------------- 结果（4.8） ---------------- */

/* 成功：先 mp_result_begin（写到 "payload": 为止），调用方在 w 里写 payload 的值（一个对象），再 mp_result_end。
   w 由调用方 hj_init 在结果缓冲上（≤ MP_RESULT_MAX）。 */
void mp_result_begin(hj_t *w, const char *req_id);
void mp_result_end(hj_t *w);

/* 失败：{"method":"link.result","id":…,"ok":false,"error":"<code>: <message>"}。返回长度或 -1。 */
int mp_build_result_error(char *buf, size_t cap, const char *req_id, const char *code, const char *message);

/* 心跳报文（固定串） */
const char *mp_heartbeat_json(void);
