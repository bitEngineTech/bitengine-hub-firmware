/* muse_cfg_core —— 见 muse_cfg_core.h。只依赖 libc 与 components/utf8。 */
#include "muse_cfg_core.h"

#include <stdio.h>
#include <string.h>

#include "utf8.h"

static bool b64url_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool muse_sdk_token_valid(const char *token)
{
    if (!token || strlen(token) != MUSE_SDK_TOKEN_LEN || strncmp(token, "mgst_", 5) != 0) return false;
    for (const char *c = token + 5; *c; c++) {
        if (!b64url_char(*c)) return false;
    }
    return strchr("AEIMQUYcgkosw048", token[MUSE_SDK_TOKEN_LEN - 1]) != NULL;
}

void muse_secret_hint(const char *secret, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) return;
    if (!secret || !*secret) {
        snprintf(out, out_cap, "(unset)");
        return;
    }
    const bool sdk = strncmp(secret, "mgst_", 5) == 0;
    snprintf(out, out_cap, "%s… (len %u)", sdk ? "mgst_" : "", (unsigned)strlen(secret));
}

bool muse_display_name_valid(const char *name)
{
    if (!name || !*name) return false;
    const size_t n = strlen(name);
    if (n > MUSE_DISPLAY_NAME_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7F) return false;
    }
    return utf8_valid(name);
}

static bool printable_no_space_quote(const char *s)
{
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c <= 0x20 || c >= 0x7F || c == '"' || c == '\'' || c == '\\') return false;
    }
    return true;
}

bool muse_api_url_valid(const char *url)
{
    if (!url) return false;
    if (!*url) return true;
    const size_t n = strlen(url);
    if (n > MUSE_URL_MAX || n <= 8 || strncmp(url, "https://", 8) != 0) return false;
    return printable_no_space_quote(url);
}

bool muse_noise_host_valid(const char *host)
{
    if (!host) return false;
    if (!*host) return true;
    const size_t n = strlen(host);
    if (n > MUSE_URL_MAX || host[0] == '.' || host[0] == '-') return false;
    for (const char *c = host; *c; c++) {
        const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')
                        || *c == '.' || *c == '-';
        if (!ok) return false;
    }
    return true;
}

void muse_identity_node_id(const uint8_t mac[6], char *out, size_t cap)
{
    static const uint8_t zero[6] = {0};
    const uint8_t *m = mac ? mac : zero;
    if (out && cap) snprintf(out, cap, "homelink-%02x%02x%02x", m[3], m[4], m[5]);
}

bool muse_vm_id_valid(const char *id)
{
    if (!id || !*id) return false;
    size_t n = 0;
    for (const char *c = id; *c; c++, n++) {
        if (n >= MUSE_VM_ID_MAX) return false;
        const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-';
        if (!ok) return false;
    }
    return true;
}

bool muse_chat_sid_valid(const char *s)
{
    if (!s || strlen(s) != MUSE_CHAT_SID_LEN) return false;
    for (size_t i = 0; i < MUSE_CHAT_SID_LEN; i++) {
        const char c = s[i];
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool muse_token_valid(const char *tok, size_t max_len)
{
    if (!tok || !*tok) return false;
    if (strlen(tok) > max_len) return false;
    for (const char *c = tok; *c; c++) {
        const unsigned char u = (unsigned char)*c;
        if (u < 0x21 || u > 0x7E) return false;
    }
    return true;
}
