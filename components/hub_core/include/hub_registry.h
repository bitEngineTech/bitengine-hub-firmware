#pragma once
/* hub_registry —— 与协议无关的命令注册表（docs/15_muse_direct_design_v1.md 6.3、6.4、附录 A）。纯 C，不依赖 ESP-IDF。
 *
 * 一张表，两个出口（架构 MD-9）：Muse 的 link.register 用 hub_registry_emit_commands_v2 生成命令表；本地 REST
 * （POST /api/v1/hub/invoke）走同一个 handler。命令说明全是编译期常量，**禁止**插入任何运行时数据（6.8；VERIFY 查
 * 说明里没有 '%'）。命令的文字与附录 A 逐字一致，主机测试把生成结果和附录 A 比（test_host/check_commands_v2.py）。
 *
 * 不在这张表里的：Muse 服务端 / App 发起的 4 个管理命令（device.unpair / list_vms / set_vm / reset_vm，归 muse_link，
 * 只处理不注册，4.13），以及只给本地开放的破坏性操作（单独的 REST 端点，十）。永不注册：device.ota、device.discover、
 * 上游的 ble.*、任何移除 / 复位 / 读密钥 / 原始簇命令（6.4）。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hub_json.h"

typedef enum { HUB_PERM_READ = 0, HUB_PERM_CONTROL = 1, HUB_PERM_MANAGE = 2 } hub_perm_t;  /* 敏感由 6.5 另判 */

/* muse/perm 的位（6.5）。READ 永远有。 */
#define HUB_PERMBIT_CONTROL    0x01u
#define HUB_PERMBIT_MANAGE     0x02u
#define HUB_PERMBIT_SENSITIVE  0x04u
#define HUB_PERMBIT_ALL        0x07u
#define HUB_PERM_DEFAULT       (HUB_PERMBIT_CONTROL | HUB_PERMBIT_MANAGE)   /* SENSITIVE 出厂为 0 */

/* 命令要求哪些后端在跑（"已启用的组件"，6.3）。没有"模式"字段：Muse 只在直连型里，那里永远是 Hub 模式。
 * 后端在跑 ≠ 它的每条命令都能用了：控制路径、Zigbee 入网对 Muse 的前提各有自己的就绪位（CHECKLIST C17；M5 第三步拆开），
 * 调用方（muse_link 的 current_backends）只在对应功能真做好时才报，没报的命令就不注册。本地 REST 不看 needs（只看有没有处理函数）。 */
#define HUB_BACKEND_ZHUB       0x01u   /* zhub 在跑（M4 起） */
#define HUB_BACKEND_MCTRL      0x02u   /* matter_ctrl 在跑（M7 起） */
#define HUB_BACKEND_CONTROL    0x04u   /* 有后端真能控制设备（设计 8.11 / 9.6 的控制路径做好之后才报） */
#define HUB_BACKEND_ZB_JOIN    0x08u   /* Zigbee 入网对 Muse 就绪（CHECKLIST Z21 的前提：入网后能被报上去、CONFIGURE / 写 CIE、容量口径） */
#define HUB_NEEDS_NONE         0x00u
#define HUB_NEEDS_ZHUB         0x01u
#define HUB_NEEDS_MCTRL        0x02u
#define HUB_NEEDS_CONTROL      0x04u
#define HUB_NEEDS_ZB_JOIN      0x08u
#define HUB_NEEDS_ANY_BACKEND  0x80u   /* zhub 与 matter_ctrl 至少一个：控制、改名只对它们管的设备有意义（M3 只有 Thread 成员，不可控） */
#define HUB_BACKEND_ALL        (HUB_BACKEND_ZHUB | HUB_BACKEND_MCTRL | HUB_BACKEND_CONTROL | HUB_BACKEND_ZB_JOIN)

/* 限速类别（6.5 的表）：每条命令显式选一个，新加命令不会因为名字没被认出来而落进宽松的那档 */
typedef enum {
    HUB_RL_READ,          /* 读类：每秒 2 次，容量 5 */
    HUB_RL_CONTROL,       /* hub.device.control：每秒 5 次，容量 10 */
    HUB_RL_UPDATE,        /* hub.device.update：每分钟 10 次 */
    HUB_RL_PERMIT_JOIN,   /* hub.zigbee.permit_join：每 60 s 1 次 */
    HUB_RL_COMMISSION,    /* hub.matter.commission：每 60 s 1 次 */
    HUB_RL_CLASS_COUNT
} hub_rl_class_t;

typedef struct { const char *name, *type, *description; } hub_param_t;  /* type: "string" / "integer" / "boolean" */

struct hub_call;                                   /* 调用上下文，随作业层（6.6）定义 */
typedef int (*hub_cmd_fn)(struct hub_call *call);

typedef struct {
    const char        *name;            /* "hub.device.control" */
    const char        *description;     /* 静态英文，禁止插入运行时数据（6.8） */
    const hub_param_t *required; uint8_t n_required;
    const hub_param_t *optional; uint8_t n_optional;
    hub_perm_t         perm;
    uint32_t           timeout_ms;
    uint8_t            needs;           /* HUB_NEEDS_* */
    hub_rl_class_t     rl;              /* 限速类别 */
    hub_cmd_fn         handler;         /* 在作业里执行（6.6）；NULL = 本版还没实现，调用回 unsupported */
} hub_cmd_t;

size_t hub_registry_count(void);
const hub_cmd_t *hub_registry_at(size_t i);
const hub_cmd_t *hub_registry_find(const char *name);     /* 名字不认识返回 NULL */

/* 这条命令在当前权限位 × 在跑的后端下注册不注册（6.3：不满足 perm、needs 的根本不注册） */
bool hub_cmd_available(const hub_cmd_t *c, uint8_t perm_bits, uint8_t backends);

/* 把 commands_v2 的值（一个对象）写进 w：只含 hub_cmd_available 的命令，顺序同表。调用方先写好键 "commands_v2"。 */
void hub_registry_emit_commands_v2(hj_t *w, uint8_t perm_bits, uint8_t backends);
