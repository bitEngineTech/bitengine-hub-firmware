/* 纯函数，无 ESP-IDF 依赖 —— 这样 test_host/ 能直接编译 version_cmp.c。
   公开原型同时在 include/ota_cloud.h 里；两处签名必须一致，
   ota_cloud.c 同时包含两者，不一致会编译报错。 */
#pragma once

/** >0 表示 a 比 b 新，0 相等，<0 更旧。NULL 视为最旧。 */
int ota_cloud_version_cmp(const char *a, const char *b);

#include <stdbool.h>

/* NCP 要不要提示更新（T9.19 ⑦）。依据按优先级：
     ① 本网关自己刷过的镜像指纹 rec_sha（有就只看它）；
     ② NCP 报的 XNCP 构建串 cur_build 与清单的 m_build（两边都有才比；J-Link 首烧的整机走这条）；
     ③ EZSP 版本串 cur_version 与 m_version（所有 build 都是 "8.2.2.0 build 532"，几乎比不出差别）；
     ④ 版本串也读不到（NCP 没应答）→ 提示，给用户一个重刷的机会。
   NULL 与 "" 同义。 */
bool ota_cloud_ncp_differs(const char *rec_sha, const char *cur_build, const char *cur_version,
                           const char *m_sha, const char *m_build, const char *m_version);
