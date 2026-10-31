#pragma once
/* NCP 读数落盘（T9.31 ②）。见 ncp_cache.c。 */
#include <stdbool.h>
#include "ncp_version.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ncp_version_set_persist_hook() 的回调：读到了且内容变了才写 NVS；读失败（valid=false）则作废缓存 */
void ncp_cache_save(const ncp_version_t *v);

/* 读回上一次存的；没有、结构版本对不上、或读失败 → false */
bool ncp_cache_load(ncp_version_t *out);

/* 作废缓存：MG24 要被改写了（进了 bootloader）。之后只有新固件真答了话、重读成功那次才会再存。
   否则刷写失败 / 新固件起不来时用户软重启，开机会把旧读数当成现状（第一轮审计 #17）。 */
void ncp_cache_invalidate(void);

#ifdef __cplusplus
}
#endif
