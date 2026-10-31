/* UTF-8 校验与修复 —— 全仓唯一一份判据（T9.24 / T9.32 / T9.33）。
 *
 * 规则（RFC 3629 + Unicode 标准表 3-7）：不收过长编码（C0/C1、E0 80-9F、F0 80-8F）、
 * 代理区（ED A0-BF）、> U+10FFFF（F4 90+、F5-FF）。非法字节按"最大子部分"一段换一个
 * U+FFFD —— 与 Python 的 bytes.decode('utf-8', 'replace') 逐字一致。
 * 行为测试在 components/webui/test_host（对照 Python），config_mgr/test_host 测导入导出。
 *
 * 谁在用：
 *   webui/json_escape.c —— 往 JSON 里放外来字符串（转义 / 剥离 / 只修）；
 *   config_mgr —— 写配置的 setter 与配置导入拒收非法 UTF-8（WiFi SSID 除外），
 *                 导出时把非 UTF-8 的 SSID 另存成 ssid_hex。 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 从 p 起检查一个 UTF-8 序列。合法时返回其长度（1-4）；不合法返回 0。
   *consumed 是这次要吃掉的字节数：合法时等于长度；不合法时是"最大子部分"
   —— 能作为某个合法序列开头的最长前缀（至少 1 字节），它整体只换一个 U+FFFD。
   p 必须指向一个 NUL 结尾串里的非 NUL 字节；结尾的 NUL 不在任何续字节范围内，
   所以不会越过结尾去读。 */
size_t utf8_scan(const unsigned char *p, size_t *consumed);

/* 整串是否合法 UTF-8。NULL 按空串处理（合法）。 */
bool utf8_valid(const char *s);

/* 拷贝并修成合法 UTF-8：合法序列整段原样，非法字节换成原样 3 字节的 U+FFFD；
   drop 非空时，对它返回 true 的 ASCII 字节直接丢掉（不占空间）。
   空间不够时整段停下，绝不写出半个字符。src 为 NULL 按空串；dst_size 为 0 时一个字节都不写。
   返回写入的字节数（不含结尾 NUL）。 */
size_t utf8_repair_copy(char *dst, size_t dst_size, const char *src,
                        bool (*drop)(unsigned char c));
