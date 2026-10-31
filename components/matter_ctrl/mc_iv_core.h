/* mc_iv_core —— 访谈结果的存储格式（类型在 include/matter_ctrl_iv.h；M7 第四步）。纯逻辑，test_host/ 原样编。
 * blob v1（小端，hubdb mctrl/iv<十进制节点号>）：node u64 | vid u16 | pid u16 | flags u8 | n_ep u8 |
 *   3 × (len u8 + 字节)（vendor、product、label，len ≤ 32）| n_ep × (ep u16 | n_dt u8 | n_cl u8 | n_dt × u32 | n_cl × u16)。
 * 格式版本是 hub_store 的 ver 字节（MC_IV_VER）。长度必须正好对上；计数超上限、端点号重复都不认。 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "matter_ctrl_iv.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MC_IV_VER       1
#define MC_IV_BLOB_MAX  (8 + 2 + 2 + 1 + 1 + 3 * (1 + MC_IV_STR_MAX) + MC_IV_EP_MAX * (2 + 1 + 1 + MC_IV_DT_MAX * 4 + MC_IV_CL_MAX * 2))

int  mc_iv_encode(const mc_iv_t *iv, uint8_t *buf, size_t cap);          /* 成功返回字节数，失败 -1 */
bool mc_iv_decode(const uint8_t *buf, size_t len, mc_iv_t *out);          /* 失败时 *out 清零 */
/* 访谈时往里加（解析 Descriptor 用）：找 / 建一个端点，满了返回 NULL 并置 EP_TRUNC */
mc_iv_ep_t *mc_iv_ep(mc_iv_t *iv, uint16_t ep);
/* 读回来的够不够算"访谈完了"：至少有根端点 0 的簇表（Descriptor 读到了）。路径级的错误状态会让读请求成功、内容却是空的 */
bool mc_iv_complete(const mc_iv_t *iv);
void mc_iv_add_dt(mc_iv_t *iv, mc_iv_ep_t *e, uint32_t dt);
void mc_iv_add_cl(mc_iv_t *iv, mc_iv_ep_t *e, uint32_t cl);              /* 厂商簇（> 0x7FFF）不记 */
/* 字符串：最多 MC_IV_STR_MAX 字节，截在 UTF-8 字符边界上（截半个字符会让后面的净化把它换成 '?'） */
void mc_iv_set_str(char dst[MC_IV_STR_MAX + 1], const char *src, size_t len);
/* 有没有某个服务端簇（任一端点） */
bool mc_iv_has_cluster(const mc_iv_t *iv, uint16_t cl);

#ifdef __cplusplus
}
#endif
