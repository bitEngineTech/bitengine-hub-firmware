/* components/idf_fixes/include/idf_fixes.h — 本地回合的 IDF 上游修复
 *
 * 出厂固件里这个组件没有对外接口：修复在链接期生效（crypto_reset_lock.c 的 --wrap）。
 * 下面只是诊断构建的压测接口。 */
#pragma once

#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>

#if CONFIG_BITENGINE_DIAG_CRYPTO_RACE
/* true = ECDSA 锁退回 v6.0.2 原样（不连带 SHA/AES 与 MPI 锁），压测对照用 */
void idf_fixes_diag_set_bypass(bool bypass);
/* 硬件 ECDSA 拿锁的累计次数 */
uint32_t idf_fixes_diag_ecdsa_locks(void);
/* 开机后延时跑一轮对照压测，结果打 W 级日志（/api/v1/logs 可读），见 crypto_race_diag.c */
void crypto_race_diag_start(void);
#endif
