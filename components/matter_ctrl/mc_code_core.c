/* mc_code_core —— 配网码预处理与 discriminator 比对（见 mc_code_core.h）。纯逻辑，test_host/ 原样编。 */
#include "mc_code_core.h"

#include <string.h>

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

int mc_code_normalize(const char *in, char *out, size_t cap)
{
    if (!in || !out || cap == 0) return 0;
    out[0] = '\0';
    size_t b = 0, e = strlen(in);
    while (b < e && is_space(in[b])) b++;
    while (e > b && is_space(in[e - 1])) e--;
    if (e - b >= 3 && in[b] == 'M' && in[b + 1] == 'T' && in[b + 2] == ':') {
        if (e - b + 1 > cap || e - b > MC_CODE_MAX || e - b == 3) return 0;
        for (size_t i = b; i < e; i++) {
            const char c = in[i];
            /* Base38 字母表：0-9 A-Z - . 加上多设备分隔符 *；":" 只许是前缀那一个 */
            const int ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || c == '-' || c == '.' || c == '*' || (i == b + 2 && c == ':');
            if (!ok) return 0;
        }
        memcpy(out, in + b, e - b);
        out[e - b] = '\0';
        return MC_CODE_QR;
    }
    size_t n = 0;
    for (size_t i = b; i < e; i++) {
        const char c = in[i];
        if (c == ' ' || c == '-') continue;
        if (c < '0' || c > '9') { out[0] = '\0'; return 0; }
        if (n + 1 >= cap || n >= 21) { out[0] = '\0'; return 0; }
        out[n++] = c;
    }
    out[n] = '\0';
    if (n != 11 && n != 21) { out[0] = '\0'; return 0; }
    return MC_CODE_MANUAL;
}

bool mc_code_disc_match(bool is_short, uint16_t disc, const char *txt_d)
{
    if (!txt_d || !*txt_d) return false;
    unsigned v = 0;
    for (const char *p = txt_d; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (unsigned)(*p - '0');
        if (v > 0xFFF) return false;
    }
    return is_short ? ((v >> 8) & 0xF) == (disc & 0xF) : v == (disc & 0xFFF);
}
