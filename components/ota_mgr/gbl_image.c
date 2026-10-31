#include "gbl_image.h"

#include <string.h>

/* Little-endian helpers — the GBL is LE throughout, the host running the
   tests may not be. */
static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint32_t gbl_crc32(const uint8_t *data, size_t len, uint32_t crc)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    return crc;
}

bool gbl_crc32_selftest(void)
{
    /* Standard check value for CRC-32 over "123456789" is 0xCBF43926 after
       the final inversion; the raw register is its complement. */
    return gbl_crc32((const uint8_t *)"123456789", 9, GBL_CRC32_START)
           == 0x340BC6D9UL;
}

/*
 * Walk the tag chain from offset 0. True when every record fits, the last
 * one is the END tag and it ends exactly at `len`. Reports the signature
 * tags seen so the callers can apply the signing rules without a second walk.
 */
typedef struct {
    unsigned sig_count;
    size_t   first_sig_off;
    uint32_t first_sig_len;
    bool     sig_is_last;     /* the tag right before END is a signature tag */
} walk_result_t;

static bool walk_tags(const uint8_t *img, size_t len, walk_result_t *r,
                      const char **why)
{
    memset(r, 0, sizeof(*r));

    size_t   off     = 0;
    uint32_t prev_id = 0;
    while (off < len) {
        if (len - off < GBL_TAG_HDR_LEN) {
            *why = "truncated tag header";
            return false;
        }
        const uint32_t id = rd_le32(img + off);
        const uint32_t tl = rd_le32(img + off + 4);
        /* len - off - 8 cannot underflow: the check above guarantees >= 8.
           Comparing as size_t handles a hostile 0xFFFFFFFF length. */
        if ((size_t)tl > len - off - GBL_TAG_HDR_LEN) {
            *why = "tag length overruns image";
            return false;
        }

        if (id == GBL_TAG_ID_END) {
            if (tl != 4) {
                *why = "END tag length is not 4";
                return false;
            }
            if (off + GBL_TAG_HDR_LEN + tl != len) {
                *why = "data after END tag";
                return false;
            }
            r->sig_is_last = (prev_id == GBL_TAG_ID_SIGNATURE_ECDSA_P256);
            return true;
        }

        if (id == GBL_TAG_ID_SIGNATURE_ECDSA_P256) {
            if (r->sig_count == 0) {
                r->first_sig_off = off;
                r->first_sig_len = tl;
            }
            r->sig_count++;
        }

        prev_id = id;
        off += GBL_TAG_HDR_LEN + tl;
    }

    *why = "no END tag";
    return false;
}

bool gbl_validate(const uint8_t *img, size_t len, const char **why)
{
    const char *dummy;
    if (!why) why = &dummy;
    *why = "ok";

    if (!img) {
        *why = "no image";
        return false;
    }
    if (len < GBL_MIN_LEN) {
        *why = "image too small";
        return false;
    }
    if (rd_le32(img) != GBL_TAG_ID_HEADER_V3) {
        *why = "not a GBL container (bad magic)";
        return false;
    }
    if (rd_le32(img + 4) != 8) {
        *why = "GBL header tag length is not 8";
        return false;
    }

    const uint8_t *end_tag = img + len - GBL_END_TAG_LEN;
    if (rd_le32(end_tag) != GBL_TAG_ID_END || rd_le32(end_tag + 4) != 4) {
        *why = "missing GBL end tag — truncated or trailing garbage";
        return false;
    }

    if (gbl_crc32(img, len, GBL_CRC32_START) != GBL_CRC32_END) {
        *why = "GBL CRC32 mismatch — image corrupted in transit";
        return false;
    }

    /* The three checks above are what the bootloader's own parser ends up
       enforcing; the walk additionally guarantees the chain is consistent so
       gbl_locate_signature() / gbl_strip_signature() never run on a file
       whose tag lengths do not add up. */
    walk_result_t r;
    if (!walk_tags(img, len, &r, why)) {
        return false;
    }

    *why = "ok";
    return true;
}

/* ---- NCP build tag (T9.25) ---- */

#define GBL_TAG_ID_PROG             0xFE0101FEUL   /* btl_gbl_format.h：PROG */
#define GBL_TAG_ID_ERASEPROG        0xFD0303FDUL   /* commander 默认出这个 */
#define GBL_TAG_ID_PROG_LZ4         0xFD0505FDUL
#define GBL_TAG_ID_PROG_LZMA        0xFD0707FDUL

static const char BUILD_PREFIX[] = "bitEngine-xncp/";

/* 一个空格分隔的词是不是 `word`（整词）；`val` 非 NULL 时按 "word=<十进制>" 解析值 */
static bool tag_token(const char *tag, const char *word, int32_t *val)
{
    const size_t wl = strlen(word);
    for (const char *p = tag; *p; ) {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && *e != ' ') e++;
        const size_t tl = (size_t)(e - p);
        if (!val && tl == wl && memcmp(p, word, wl) == 0) return true;
        if (val && tl > wl + 1 && memcmp(p, word, wl) == 0 && p[wl] == '=') {
            int64_t v = 0;
            const char *d = p + wl + 1;
            for (; d < e && *d >= '0' && *d <= '9'; d++) {
                v = v * 10 + (*d - '0');
                if (v > 1000000) return false;          /* 不像缓存条数 */
            }
            if (d != e) return false;                   /* 混了非数字：不认 */
            *val = (int32_t)v;
            return true;
        }
        p = e;
    }
    return false;
}

bool gbl_build_info(const uint8_t *img, size_t len, gbl_build_info_t *out)
{
    if (!img || !out) return false;
    memset(out, 0, sizeof(*out));
    out->nvm3_cache = -1;

    /* 按字节流式匹配：前缀里 'b' 只出现在开头，失配时只需看当前字节是不是 'b'，不会漏 */
    const size_t pl = sizeof(BUILD_PREFIX) - 1;
    size_t match = 0, n_tag = 0;
    bool capturing = false, done = false;

    size_t off = 0;
    while (!done && off + GBL_TAG_HDR_LEN <= len) {
        const uint32_t id = rd_le32(img + off);
        const uint32_t tl = rd_le32(img + off + 4);
        if ((size_t)tl > len - off - GBL_TAG_HDR_LEN) break;      /* gbl_validate 已挡过，防御一下 */
        if (id == GBL_TAG_ID_END) break;
        if (id == GBL_TAG_ID_PROG_LZ4 || id == GBL_TAG_ID_PROG_LZMA) {
            out->compressed = true;
        } else if ((id == GBL_TAG_ID_PROG || id == GBL_TAG_ID_ERASEPROG) && tl > 4) {
            const uint8_t *p = img + off + GBL_TAG_HDR_LEN + 4;   /* 跳过 4 字节 Flash 地址 */
            for (uint32_t i = 0; i < tl - 4 && !done; i++) {
                const char c = (char)p[i];
                if (capturing) {
                    if (c == '\0' || n_tag == GBL_BUILD_TAG_MAX) {
                        /* 与 ncp_image_check.py 的 [0-9.]+ 对应：前缀后面不是版本号（别处的同名字符串，
                           比如格式串）就不算，接着往后找 */
                        if (n_tag > 0 && ((out->tag[0] >= '0' && out->tag[0] <= '9') || out->tag[0] == '.')) {
                            done = true;
                            break;
                        }
                        capturing = false;
                        out->has_build_tag = false;
                        n_tag = 0;
                        match = (c == BUILD_PREFIX[0]) ? 1 : 0;
                        continue;
                    }
                    out->tag[n_tag++] = c;
                } else if (c == BUILD_PREFIX[match]) {
                    if (++match == pl) { capturing = true; out->has_build_tag = true; }
                } else {
                    match = (c == BUILD_PREFIX[0]) ? 1 : 0;
                }
            }
        }
        off += GBL_TAG_HDR_LEN + tl;
    }
    out->tag[n_tag] = '\0';
    if (out->has_build_tag && !done &&
        !(n_tag > 0 && ((out->tag[0] >= '0' && out->tag[0] <= '9') || out->tag[0] == '.'))) {
        out->has_build_tag = false;                       /* 数据到头还在抓、且不是版本号开头 */
        out->tag[0] = '\0';
    }
    if (out->has_build_tag) {
        /* 第一个词是版本号（"1.0"），与 ncp_image_check.py 的 [0-9.]+ 对应；标记在其后 */
        out->wipe = tag_token(out->tag, "WIPE", NULL);
        int32_t v;
        if (tag_token(out->tag, "nvm3c", &v)) out->nvm3_cache = v;
    }
    return true;
}

const char *gbl_ncp_guard(const gbl_build_info_t *bi, int32_t floor, bool bypass, int32_t *eff_out)
{
    int32_t eff = -1;
    const char *why = NULL;
    if (!bi || bi->compressed) {
        why = "ncp_image_unreadable";
    } else {
        eff = (bi->has_build_tag && bi->nvm3_cache >= 0) ? bi->nvm3_cache : GBL_NVM3C_LEGACY;
        if (bi->wipe)        why = "ncp_image_wipe";
        else if (eff < floor) why = "ncp_image_downgrade";
    }
    if (eff_out) *eff_out = eff;
    return bypass ? NULL : why;
}

bool gbl_locate_signature(const uint8_t *img, size_t len,
                          gbl_sig_info_t *out, const char **why)
{
    const char *dummy;
    if (!why) why = &dummy;
    *why = "ok";

    if (!img || !out) {
        *why = "no image";
        return false;
    }
    memset(out, 0, sizeof(*out));

    walk_result_t r;
    if (!walk_tags(img, len, &r, why)) {
        return false;
    }

    if (r.sig_count == 0) {
        return true;                       /* plain container */
    }
    if (r.sig_count > 1) {
        *why = "more than one signature tag";
        return false;
    }
    if (r.first_sig_len != GBL_SIGNATURE_LEN) {
        *why = "signature tag length is not 64";
        return false;
    }
    if (!r.sig_is_last) {
        /* The bootloader stops parsing after the signature; anything in
           between would be silently ignored on flash — treat as forged. */
        *why = "data between signature and END tag";
        return false;
    }

    out->has_signature = true;
    out->sig_off       = r.first_sig_off;
    out->signed_len    = r.first_sig_off;
    return true;
}

size_t gbl_strip_signature(uint8_t *img, size_t len, const gbl_sig_info_t *sig)
{
    if (!img || !sig || !sig->has_signature) return 0;
    if (len < GBL_MIN_LEN) return 0;

    /* The signature tag must be exactly { hdr, 64 bytes } followed by the
       END tag at the very end — gbl_locate_signature() guarantees it, and the
       arithmetic below is only valid in that shape. */
    const size_t sig_tag_len = GBL_TAG_HDR_LEN + GBL_SIGNATURE_LEN;
    if (sig->sig_off < GBL_HEADER_TAG_LEN) return 0;
    if (sig->sig_off + sig_tag_len + GBL_END_TAG_LEN != len) return 0;
    if (rd_le32(img + sig->sig_off) != GBL_TAG_ID_SIGNATURE_ECDSA_P256) return 0;
    if (rd_le32(img + sig->sig_off + 4) != GBL_SIGNATURE_LEN) return 0;

    /* 1. header: clear the "signed" type bit (offset 12: id 0-3, length 4-7,
          version 8-11, type 12-15) */
    wr_le32(img + 12, rd_le32(img + 12) & ~(uint32_t)GBL_TYPE_SIGNATURE_ECDSA);

    /* 2. drop the signature tag: slide the END tag down over it */
    memmove(img + sig->sig_off, img + sig->sig_off + sig_tag_len, GBL_END_TAG_LEN);
    const size_t new_len = len - sig_tag_len;

    /* 3. END tag CRC over everything before the CRC field itself */
    const uint32_t crc = gbl_crc32(img, new_len - 4, GBL_CRC32_START) ^ 0xFFFFFFFFUL;
    wr_le32(img + new_len - 4, crc);

    return new_len;
}
