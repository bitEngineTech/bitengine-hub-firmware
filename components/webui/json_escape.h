/* 往 JSON 里放外来字符串，输出都保证是合法 UTF-8（T9.24 / T9.32）。
 *
 * 为什么要校验 UTF-8：转义 / 剥离只管 JSON 语法，挡不住非法字节。局域网里任何人不用登录
 * 就能发一个带 \xff 的 Host 头，跨站守卫会把它原样写进 W 日志；周围 AP 的 SSID 可以是
 * 任意字节（GBK 编码的中文名就不是 UTF-8）。回包一旦不合法，浏览器把坏字节换成替换字符，
 * 看不出问题；脚本（Python json.load，tools/test_api.sh 就是这么读的）直接报错。
 *
 * 三者共同的规则：
 *   · 合法 UTF-8 序列整段原样拷贝（RFC 3629：不收过长编码、代理区、> U+10FFFF）；
 *   · 非法字节按 Unicode 的"最大子部分"规则一段换一个 U+FFFD
 *     —— 与 Python 的 bytes.decode('utf-8', 'replace') 逐字一致，主机侧测试拿它做对照；
 *   · 空间不够时整段停下：绝不写出半个转义序列、半个 UTF-8 字符；
 *   · src 为 NULL 按空串处理；dst_size 为 0 时一个字节都不写。
 *
 * 选哪个：
 *   webui_json_escape   —— 必须原样呈现的正文（日志、OTBR 网络名）：" \ 与控制字符**转义**，
 *                          U+FFFD 写成 \ufffd。输出最多是输入的 6 倍。
 *   json_sanitize_copy  —— 拼进手写 JSON（snprintf）的短字段：" \ 与控制字符直接**剥掉**，
 *                          U+FFFD 写成原样 3 字节。剥会改动内容，所以别用在日志上。
 *   utf8_repair_copy(…, NULL)（components/utf8）—— 交给 cJSON 的字符串：cJSON 自己会转义，
 *                          只需修 UTF-8（扫描到的 SSID）。
 *
 * 返回写入的字节数（不含结尾 NUL）。 */
#pragma once

#include <stddef.h>

size_t webui_json_escape(char *dst, size_t dst_size, const char *src);

/* 参数顺序沿用老接口（dst, src, size），全组件几十处调用不必改。 */
void   json_sanitize_copy(char *dst, const char *src, size_t dst_size);
