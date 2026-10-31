/* mc_paa —— 内置的 PAA 信任库（docs/15 设计 9.7；TODO T9.53）。证书在 paa_snapshot.c（tools/fetch_paa.py 生成）。 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *der;
    size_t len;
} mc_paa_cert_t;

extern const mc_paa_cert_t mc_paa_certs[];
extern const unsigned mc_paa_count;
extern const char mc_paa_snapshot_date[];

/* 装成 CHIP 的信任库（mc_paa.cpp；CHIP 任务里、setup_commissioner 之前）。失败返回 false：控制器照起，配网的设备认证会失败 */
bool mc_paa_install(void);
/* "允许测试设备"（mc_paa.cpp 文件头）：set / get 任何任务；apply 在 CHIP 任务里（setup_commissioner 之后、每次切换后），设 CD 测试密钥 */
void mc_paa_set_allow_test(bool on);
bool mc_paa_allow_test(void);
void mc_paa_apply_test(void);

#ifdef __cplusplus
}
#endif
