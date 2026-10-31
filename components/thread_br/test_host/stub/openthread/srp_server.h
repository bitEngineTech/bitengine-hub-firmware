#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "instance.h"
/* SRP server 的桩：本轮测试只覆盖不依赖它的纯逻辑（名字解析、TXT 解析、
   跟踪表），这些函数只需存在以便 srp_readvertise.c 能编译链接。
   注册表遍历本身要真设备/真 OT 实例，属于在板用例（见 docs 里 TC-S01~S03）。*/
typedef struct otSrpServerHost    otSrpServerHost;
typedef struct otSrpServerService otSrpServerService;

static inline const otSrpServerHost *otSrpServerGetNextHost(otInstance *i, const otSrpServerHost *h)
{ (void)i; (void)h; return 0; }
static inline bool otSrpServerHostIsDeleted(const otSrpServerHost *h) { (void)h; return false; }
static inline const char *otSrpServerHostGetFullName(const otSrpServerHost *h) { (void)h; return ""; }
static inline const otSrpServerService *otSrpServerHostGetNextService(const otSrpServerHost *h,
                                                                     const otSrpServerService *s)
{ (void)h; (void)s; return 0; }
static inline bool otSrpServerServiceIsDeleted(const otSrpServerService *s) { (void)s; return false; }
static inline const char *otSrpServerServiceGetInstanceLabel(const otSrpServerService *s) { (void)s; return ""; }
static inline const char *otSrpServerServiceGetServiceName(const otSrpServerService *s) { (void)s; return ""; }
static inline uint16_t otSrpServerServiceGetPort(const otSrpServerService *s) { (void)s; return 0; }
static inline const uint8_t *otSrpServerServiceGetTxtData(const otSrpServerService *s, uint16_t *len)
{ (void)s; if (len) *len = 0; return 0; }
static inline uint16_t otSrpServerServiceGetNumberOfSubTypes(const otSrpServerService *s) { (void)s; return 0; }
static inline const char *otSrpServerServiceGetSubTypeServiceNameAt(const otSrpServerService *s, uint16_t i)
{ (void)s; (void)i; return 0; }
static inline otError otSrpServerParseSubTypeServiceName(const char *n, char *label, uint8_t size)
{ (void)n; (void)label; (void)size; return OT_ERROR_FAILED; }
