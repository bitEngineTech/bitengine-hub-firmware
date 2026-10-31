/* 往 JSON 里放外来字符串（UTF-8 安全）。规则、理由与怎么选见 json_escape.h；
 * UTF-8 判据本身在 components/utf8。只依赖 libc + utf8 —— test_host 直接编这两个文件。 */
#include <stdbool.h>
#include <string.h>

#include "json_escape.h"
#include "utf8.h"

size_t webui_json_escape(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return 0;

    static const char hex[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *)(src ? src : "");
    size_t w = 0;

    while (*p) {
        char ctl[6];
        const char *out;
        size_t out_len, consumed = 1;

        switch (*p) {
        case '"':  out = "\\\""; out_len = 2; break;
        case '\\': out = "\\\\"; out_len = 2; break;
        case '\n': out = "\\n";  out_len = 2; break;
        case '\r': out = "\\r";  out_len = 2; break;
        case '\t': out = "\\t";  out_len = 2; break;
        default:
            if (*p < 0x20) {                       /* 其余控制字符走 \u00XX */
                memcpy(ctl, "\\u00", 4);
                ctl[4] = hex[*p >> 4];
                ctl[5] = hex[*p & 0x0F];
                out = ctl; out_len = 6;
            } else if (*p < 0x80) {
                out = (const char *)p; out_len = 1;
            } else {
                size_t len = utf8_scan(p, &consumed);
                if (len) { out = (const char *)p; out_len = len; }
                else     { out = "\\ufffd";       out_len = 6; }
            }
            break;
        }

        if (w + out_len >= dst_size) break;       /* 宁可截断，不可写半个序列 */
        memcpy(dst + w, out, out_len);
        w += out_len;
        p += consumed;
    }
    dst[w] = '\0';
    return w;
}

/* 剥离式：" \ 与控制字符直接丢掉，其余交给 utf8_repair_copy 修 UTF-8。 */
static bool json_unsafe(unsigned char c)
{
    return c == '"' || c == '\\' || c < 0x20;
}

void json_sanitize_copy(char *dst, const char *src, size_t dst_size)
{
    utf8_repair_copy(dst, dst_size, src, json_unsafe);
}
