/* mc_nodes_core —— 网关 fabric 上的节点表（docs/15 设计 9.3"新设备的节点号""节点表"；M7 第三步）。纯逻辑，不依赖 IDF / CHIP（test_host/ 原样编）。
 *
 * 节点号从 2 起递增、**永不回收**（1 是控制器自己）：删掉的节点号再发给新设备，旧设备（若还留着我们的 fabric）的 CASE 会话、
 * 订阅、ACL 会和新设备混在一起。分配 = next_node++ 并把这一条记成 PENDING —— 调用方**落盘读回确认之后**才拿这个号去 PairDevice
 * （CHECKLIST MC12：防抖窗口里断电，设备拿着一个网关已经忘了的号）。配网成功改 OK；失败删掉这一条（号不还）。
 *
 * blob v1（小端，存 hubdb mctrl/nodes）：next_node u64 | n u8 | n × 记录 16 B：node_id u64 | vid u16 | pid u16 | state u8 | flags u8 | 保留 u16。
 * 格式版本是 hub_store 的 ver 字节（MC_NODES_VER）。长度必须正好对上；记录里 node_id 重复、< 2、≥ next_node、state 不认识都不认。 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MC_NODES_VER       1
#define MC_NODES_CAP       128         /* 结构容量；实际上限是 mc_nodes_t.max（Kconfig BITENGINE_MATTER_MAX_NODES，≤ 128）。blob 9 + 128 × 16 = 2057 B，hub_store 单块 4000 B 内 */
#define MC_NODE_FIRST      2u          /* 1 是控制器自己（9.3） */
#define MC_NODES_REC_LEN   16
#define MC_NODES_BLOB_MAX  (8 + 1 + MC_NODES_CAP * MC_NODES_REC_LEN)
_Static_assert(MC_NODES_BLOB_MAX <= 4000, "节点表要放进 hub_store 的一个 blob（HSC_BLOB_MAX 4000）");

typedef enum {
    MC_NODE_PENDING = 1,               /* 号已分配、正在配网（开机读回时看到它 = 上次配到一半断了电：当作失败删掉） */
    MC_NODE_OK      = 2,               /* 配网完成（CommissioningComplete 回了成功） */
} mc_node_state_t;

typedef struct {
    uint64_t node_id;
    uint16_t vid, pid;                 /* 配网码里带的（长码 / QR），没有就 0；访谈（第四步）读 Basic Information 再补 */
    uint8_t  state;                    /* mc_node_state_t */
    uint8_t  flags;
} mc_node_t;

typedef struct {
    uint64_t  next_node;               /* 下一个要发的号（≥ MC_NODE_FIRST） */
    uint8_t   n, max;
    mc_node_t nodes[MC_NODES_CAP];
} mc_nodes_t;

typedef enum {
    MC_NODES_OK = 0,
    MC_NODES_EARG,
    MC_NODES_ELEN,                     /* blob 长度对不上 / 条数超过 max */
    MC_NODES_EBAD,                     /* 记录内容不合法（号重复 / 越界、state 不认识、next_node < 2） */
    MC_NODES_EFULL,                    /* 已经 max 台 */
    MC_NODES_ENOTFOUND,
} mc_nodes_err_t;

void mc_nodes_init(mc_nodes_t *t, uint8_t max);
int mc_nodes_encode(const mc_nodes_t *t, uint8_t *buf, size_t cap);           /* 成功返回字节数，失败 -mc_nodes_err_t */
mc_nodes_err_t mc_nodes_decode(const uint8_t *buf, size_t len, uint8_t max, mc_nodes_t *out);   /* 失败时 *out 是空表（next_node = 2） */
/* 分配一个号、记一条 PENDING（vid / pid 可为 0）。*out_node 是新号 */
mc_nodes_err_t mc_nodes_alloc(mc_nodes_t *t, uint16_t vid, uint16_t pid, uint64_t *out_node);
mc_node_t *mc_nodes_find(mc_nodes_t *t, uint64_t node_id);
mc_nodes_err_t mc_nodes_set_state(mc_nodes_t *t, uint64_t node_id, mc_node_state_t st);
mc_nodes_err_t mc_nodes_remove(mc_nodes_t *t, uint64_t node_id);               /* 号不还：next_node 不动 */
/* 开机读回之后：删掉所有 PENDING（上次配到一半），返回删了几条 */
unsigned mc_nodes_drop_pending(mc_nodes_t *t);

#ifdef __cplusplus
}
#endif
