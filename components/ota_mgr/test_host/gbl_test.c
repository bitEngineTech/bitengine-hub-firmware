/*
 * Host test for gbl_image.c — container validation, signature location,
 * ECDSA verification (real, via host_crypto.c) and signature stripping.
 *
 *   gbl_test <vector dir>                          vectors from make_vectors.py
 *   gbl_test golden <signed.gbl> <unsigned.gbl> <pub.xy>
 *                                                  commander-made pair: verify
 *                                                  signed, strip it, must equal
 *                                                  unsigned byte-for-byte
 */

#include "gbl_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;

#define CHECK(cond, ...) do {                                   \
        if (cond) { g_pass++; }                                 \
        else { g_fail++; printf("  ❌ "); printf(__VA_ARGS__);   \
               printf("  (%s:%d)\n", __FILE__, __LINE__); }      \
    } while (0)

/* dir == NULL: name is already a full path (golden mode) */
static uint8_t *load(const char *dir, const char *name, size_t *len)
{
    char path[512];
    if (dir) snprintf(path, sizeof(path), "%s/%s", dir, name);
    else     snprintf(path, sizeof(path), "%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { printf("read %s failed\n", path); exit(2); }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static bool validates(const uint8_t *img, size_t len, const char **why)
{
    return gbl_validate(img, len, why);
}

static void expect_invalid(const char *dir, const char *name)
{
    size_t n; uint8_t *v = load(dir, name, &n);
    const char *why = NULL;
    CHECK(!validates(v, n, &why), "%s must fail gbl_validate (got ok)\n", name);
    CHECK(why && strcmp(why, "ok") != 0, "%s: why must be set\n", name);
    free(v);
}

static void expect_malformed_signing(const char *dir, const char *name)
{
    size_t n; uint8_t *v = load(dir, name, &n);
    const char *why = NULL;
    CHECK(validates(v, n, &why), "%s must pass gbl_validate: %s\n", name, why);
    gbl_sig_info_t si;
    CHECK(!gbl_locate_signature(v, n, &si, &why), "%s must fail gbl_locate_signature\n", name);
    CHECK(why && strcmp(why, "ok") != 0, "%s: why must be set\n", name);
    free(v);
}

static void run_vectors(const char *dir)
{
    const char *why = NULL;
    size_t n_good, n_uns, n_pub, n_oth;
    uint8_t *good = load(dir, "good.gbl", &n_good);
    uint8_t *uns  = load(dir, "unsigned.gbl", &n_uns);
    uint8_t *pub  = load(dir, "pub.xy", &n_pub);
    uint8_t *oth  = load(dir, "other.xy", &n_oth);
    CHECK(n_pub == 64 && n_oth == 64, "key files must be 64 bytes\n");

    printf("-- crc\n");
    CHECK(gbl_crc32_selftest(), "crc32 self-test\n");

    printf("-- validate\n");
    CHECK(validates(good, n_good, &why), "good.gbl validates: %s\n", why);
    CHECK(validates(uns, n_uns, &why), "unsigned.gbl validates: %s\n", why);
    CHECK(!gbl_validate(NULL, 100, &why), "NULL image rejected\n");
    CHECK(!gbl_validate(good, GBL_MIN_LEN - 1, &why), "short image rejected\n");
    CHECK(gbl_validate(good, n_good, NULL), "why may be NULL\n");
    expect_invalid(dir, "truncated.gbl");
    expect_invalid(dir, "badcrc.gbl");
    expect_invalid(dir, "after-end.gbl");
    expect_invalid(dir, "overrun.gbl");
    expect_invalid(dir, "badhdrlen.gbl");

    printf("-- locate\n");
    gbl_sig_info_t si;
    CHECK(gbl_locate_signature(good, n_good, &si, &why) && si.has_signature, "good.gbl has a signature: %s\n", why);
    CHECK(si.sig_off == n_good - (GBL_TAG_HDR_LEN + GBL_SIGNATURE_LEN + GBL_END_TAG_LEN), "signature sits right before END\n");
    CHECK(si.signed_len == si.sig_off, "signed_len == sig_off\n");
    gbl_sig_info_t su;
    CHECK(gbl_locate_signature(uns, n_uns, &su, &why) && !su.has_signature, "unsigned.gbl: no signature, not an error\n");
    {
        size_t n; uint8_t *v = load(dir, "unsigned-flagged.gbl", &n);
        gbl_sig_info_t s2;
        CHECK(gbl_validate(v, n, &why) && gbl_locate_signature(v, n, &s2, &why) && !s2.has_signature,
              "header flag without tag = unsigned\n");
        free(v);
    }
    expect_malformed_signing(dir, "twosig.gbl");
    expect_malformed_signing(dir, "sig-not-last.gbl");
    expect_malformed_signing(dir, "badsiglen.gbl");
    CHECK(!gbl_locate_signature(NULL, n_good, &si, &why), "locate: NULL image rejected\n");
    CHECK(!gbl_locate_signature(good, n_good, NULL, &why), "locate: NULL out rejected\n");

    printf("-- verify (real ECDSA-P256 via OpenSSL)\n");
    CHECK(gbl_locate_signature(good, n_good, &si, &why), "relocate good\n");
    const uint8_t *sig = good + si.sig_off + GBL_TAG_HDR_LEN;
    CHECK(gbl_sig_verify(good, si.signed_len, sig, pub) == 0, "good.gbl verifies with pub\n");
    CHECK(gbl_sig_verify(good, si.signed_len, sig, oth) == 1, "good.gbl does NOT verify with other key\n");
    CHECK(gbl_sig_verify(good, si.signed_len - 1, sig, pub) == 1, "hash range is exactly [0, sig_off)\n");
    CHECK(gbl_sig_verify(good, si.signed_len + 1, sig, pub) == 1, "hash must not include the tag header\n");
    {
        size_t n; uint8_t *v = load(dir, "wrongkey.gbl", &n);
        gbl_sig_info_t s2;
        CHECK(gbl_validate(v, n, &why) && gbl_locate_signature(v, n, &s2, &why) && s2.has_signature, "wrongkey.gbl shape ok\n");
        CHECK(gbl_sig_verify(v, s2.signed_len, v + s2.sig_off + GBL_TAG_HDR_LEN, pub) == 1, "wrongkey.gbl rejected\n");
        free(v);
        v = load(dir, "tampered.gbl", &n);
        CHECK(gbl_validate(v, n, &why), "tampered.gbl passes the container check (CRC was refixed): %s\n", why);
        CHECK(gbl_locate_signature(v, n, &s2, &why) && s2.has_signature, "tampered.gbl shape ok\n");
        CHECK(gbl_sig_verify(v, s2.signed_len, v + s2.sig_off + GBL_TAG_HDR_LEN, pub) == 1, "tampered.gbl rejected by the signature\n");
        free(v);
    }
    {
        uint8_t bad_pub[64];
        memset(bad_pub, 0xFF, sizeof(bad_pub));   /* not on the curve */
        CHECK(gbl_sig_verify(good, si.signed_len, sig, bad_pub) == -1, "off-curve key is an internal error, not 'valid'\n");
        CHECK(gbl_sig_verify(NULL, 1, sig, pub) == -1, "verify: NULL msg\n");
    }

    printf("-- strip\n");
    {
        uint8_t *w = malloc(n_good);
        memcpy(w, good, n_good);
        size_t nl = gbl_strip_signature(w, n_good, &si);
        CHECK(nl == n_good - (GBL_TAG_HDR_LEN + GBL_SIGNATURE_LEN), "stripped length\n");
        CHECK(nl == n_uns && memcmp(w, uns, n_uns) == 0, "strip(good.gbl) == unsigned.gbl byte-for-byte\n");
        CHECK(gbl_validate(w, nl, &why), "stripped image validates: %s\n", why);
        gbl_sig_info_t s3;
        CHECK(gbl_locate_signature(w, nl, &s3, &why) && !s3.has_signature, "stripped image has no signature\n");
        CHECK((w[12] & 0x01) == 0 && (w[13] & 0x01) == 0, "header signed bit cleared\n");
        CHECK(gbl_strip_signature(w, nl, &s3) == 0, "strip on an unsigned image returns 0\n");
        free(w);

        w = malloc(n_good);
        memcpy(w, good, n_good);
        gbl_sig_info_t wrong = si;
        wrong.sig_off += 1;
        CHECK(gbl_strip_signature(w, n_good, &wrong) == 0, "strip with a wrong offset returns 0\n");
        CHECK(memcmp(w, good, n_good) == 0, "…and leaves the image untouched\n");
        CHECK(gbl_strip_signature(w, n_good - 1, &si) == 0, "strip with inconsistent length returns 0\n");
        CHECK(gbl_strip_signature(NULL, n_good, &si) == 0, "strip: NULL image\n");
        CHECK(gbl_strip_signature(w, n_good, NULL) == 0, "strip: NULL info\n");
        free(w);
    }

    free(good); free(uns); free(pub); free(oth);
}

/* ---- T9.25：构建串 / 防降级（合成镜像，不需要 CRC 对 —— gbl_build_info 只沿标签链读程序数据）---- */

static size_t put_tag(uint8_t *b, size_t off, uint32_t id, const uint8_t *body, uint32_t n)
{
    b[off] = id; b[off + 1] = id >> 8; b[off + 2] = id >> 16; b[off + 3] = id >> 24;
    b[off + 4] = n; b[off + 5] = n >> 8; b[off + 6] = n >> 16; b[off + 7] = n >> 24;
    if (n) memcpy(b + off + 8, body, n);
    return off + 8 + n;
}

/* 头 + 若干程序块（每块前 4 字节是 Flash 地址）+ END。`split` 处把程序数据切成两块；prog_id 指定块类型 */
static size_t synth(uint8_t *b, const char *prog, size_t plen, size_t split, uint32_t prog_id)
{
    static const uint8_t hdr[8] = { 0 };
    uint8_t chunk[600];
    size_t off = put_tag(b, 0, GBL_TAG_ID_HEADER_V3, hdr, 8);
    if (split > plen) split = plen;
    memset(chunk, 0, 4);
    memcpy(chunk + 4, prog, split);
    off = put_tag(b, off, prog_id, chunk, (uint32_t)(4 + split));
    if (plen > split) {
        memcpy(chunk + 4, prog + split, plen - split);
        off = put_tag(b, off, prog_id, chunk, (uint32_t)(4 + plen - split));
    }
    static const uint8_t crc[4] = { 0 };
    return put_tag(b, off, GBL_TAG_ID_END, crc, 4);
}

static void run_build_tag_tests(void)
{
    printf("-- build tag / downgrade guard (T9.25)\n");
    static const char REL[] = "\x12\x34code..bitEngine-xncp/1.0 v0.5.0-418-gc9ac5d1 2026-10-07 nvm3c=400\0tail";
    uint8_t b[2048];
    gbl_build_info_t bi;
    int32_t eff;

    size_t n = synth(b, REL, sizeof(REL) - 1, sizeof(REL) - 1, 0xFD0303FDUL);
    CHECK(gbl_build_info(b, n, &bi) && bi.has_build_tag && bi.nvm3_cache == 400 && !bi.wipe && !bi.compressed,
          "release-style tag in one ERASEPROG: nvm3c=400 (got tag=%d c=%d)\n", bi.has_build_tag, (int)bi.nvm3_cache);
    CHECK(strcmp(bi.tag, "1.0 v0.5.0-418-gc9ac5d1 2026-10-07 nvm3c=400") == 0, "tag text captured to the NUL: [%s]\n", bi.tag);
    CHECK(gbl_ncp_guard(&bi, 400, false, &eff) == NULL && eff == 400, "400 onto a 400 chip: allowed\n");
    CHECK(gbl_ncp_guard(&bi, 600, false, &eff) && strcmp(gbl_ncp_guard(&bi, 600, false, &eff), "ncp_image_downgrade") == 0,
          "400 onto a chip that used 600: downgrade\n");

    for (size_t cut = 8; cut < 40; cut += 3) {      /* 切在前缀里、版本里、nvm3c 里 */
        n = synth(b, REL, sizeof(REL) - 1, cut, 0xFE0101FEUL);
        CHECK(gbl_build_info(b, n, &bi) && bi.nvm3_cache == 400, "tag split across two PROG tags at %u: still 400\n", (unsigned)cut);
    }
    n = synth(b, REL, sizeof(REL) - 1, 62, 0xFE0101FEUL);
    CHECK(gbl_build_info(b, n, &bi) && bi.nvm3_cache == 400, "split inside nvm3c=: still 400\n");

    static const char OLD[] = "xx bitEngine-xncp/1.0 v0.5.0-414-ga8deee9 2026-10-06\0";
    n = synth(b, OLD, sizeof(OLD) - 1, sizeof(OLD) - 1, 0xFD0303FDUL);
    CHECK(gbl_build_info(b, n, &bi) && bi.has_build_tag && bi.nvm3_cache == -1, "tag without nvm3c=: -1\n");
    CHECK(gbl_ncp_guard(&bi, 400, false, &eff) && eff == GBL_NVM3C_LEGACY, "…counts as legacy 200 and is refused\n");

    static const char NONE[] = "an XNCP-less NCP has no build tag at all\0";
    n = synth(b, NONE, sizeof(NONE) - 1, sizeof(NONE) - 1, 0xFD0303FDUL);
    CHECK(gbl_build_info(b, n, &bi) && !bi.has_build_tag, "no build tag\n");
    CHECK(gbl_ncp_guard(&bi, 400, false, &eff) && strcmp(gbl_ncp_guard(&bi, 400, false, &eff), "ncp_image_downgrade") == 0
          && eff == GBL_NVM3C_LEGACY, "no build tag: legacy 200, downgrade\n");
    CHECK(gbl_ncp_guard(&bi, 400, true, &eff) == NULL, "developer unlock bypasses\n");

    static const char WIPE[] = "bitEngine-xncp/1.0 v0.5.0-416 2026-10-06 nvm3c=400 WIPE\0";
    n = synth(b, WIPE, sizeof(WIPE) - 1, sizeof(WIPE) - 1, 0xFD0303FDUL);
    CHECK(gbl_build_info(b, n, &bi) && bi.wipe && bi.nvm3_cache == 400, "WIPE token seen\n");
    CHECK(gbl_ncp_guard(&bi, 400, false, NULL) && strcmp(gbl_ncp_guard(&bi, 400, false, NULL), "ncp_image_wipe") == 0, "WIPE refused\n");
    static const char NOWIPE[] = "bitEngine-xncp/1.0 v0.5.0-WIPED 2026 nvm3c=400\0";
    n = synth(b, NOWIPE, sizeof(NOWIPE) - 1, sizeof(NOWIPE) - 1, 0xFD0303FDUL);
    CHECK(gbl_build_info(b, n, &bi) && !bi.wipe, "WIPE must be a whole word (\"v0.5.0-WIPED\" is not)\n");

    static const char DECOY[] = "fmt bitEngine-xncp/%s %s\0 ... bitEngine-xncp/1.0 v0.5.0-418 2026-10-07 nvm3c=400\0";
    n = synth(b, DECOY, sizeof(DECOY) - 1, sizeof(DECOY) - 1, 0xFD0303FDUL);
    CHECK(gbl_build_info(b, n, &bi) && bi.nvm3_cache == 400, "a decoy prefix without a version is skipped\n");

    static const char BADNUM[] = "bitEngine-xncp/1.0 v0.5 nvm3c=40x nvm3cache=999\0";
    n = synth(b, BADNUM, sizeof(BADNUM) - 1, sizeof(BADNUM) - 1, 0xFD0303FDUL);
    CHECK(gbl_build_info(b, n, &bi) && bi.nvm3_cache == -1, "nvm3c=40x / nvm3cache= are not nvm3c\n");

    n = synth(b, REL, sizeof(REL) - 1, sizeof(REL) - 1, 0xFD0505FDUL);   /* LZ4 */
    CHECK(gbl_build_info(b, n, &bi) && bi.compressed, "compressed program data flagged\n");
    CHECK(gbl_ncp_guard(&bi, 400, false, &eff) && strcmp(gbl_ncp_guard(&bi, 400, false, &eff), "ncp_image_unreadable") == 0
          && eff == -1, "compressed: unreadable\n");

    CHECK(!gbl_build_info(NULL, 10, &bi) && !gbl_build_info(b, n, NULL), "NULL args\n");
    CHECK(gbl_ncp_guard(NULL, 400, false, &eff) && eff == -1, "guard: NULL info refused\n");
}

/* 真实镜像：bash run.sh 从 mg24/release/ 与 git 历史各取一份传进来 */
static void run_build_tag_real(const char *path, int32_t expect_nvm3c, const char *expect_guard)
{
    size_t n;
    uint8_t *v = load(NULL, path, &n);
    const char *why = NULL;
    gbl_build_info_t bi;
    CHECK(gbl_validate(v, n, &why), "%s validates: %s\n", path, why);
    CHECK(gbl_build_info(v, n, &bi), "%s: build info\n", path);
    CHECK(bi.nvm3_cache == expect_nvm3c, "%s: nvm3c %d (expect %d), tag [%s]\n", path, (int)bi.nvm3_cache, (int)expect_nvm3c, bi.tag);
    const char *g = gbl_ncp_guard(&bi, 400, false, NULL);
    CHECK((g == NULL && expect_guard == NULL) || (g && expect_guard && strcmp(g, expect_guard) == 0),
          "%s: guard %s (expect %s)\n", path, g ? g : "allow", expect_guard ? expect_guard : "allow");
    free(v);
}

static void run_golden(const char *signed_path, const char *unsigned_path, const char *xy_path)
{
    const char *why = NULL;
    size_t ns, nu, nk;
    uint8_t *s = load(NULL, signed_path, &ns);
    uint8_t *u = load(NULL, unsigned_path, &nu);
    uint8_t *k = load(NULL, xy_path, &nk);
    CHECK(nk == 64, "golden: key is 64 bytes\n");
    CHECK(gbl_validate(s, ns, &why), "golden: signed validates: %s\n", why);
    CHECK(gbl_validate(u, nu, &why), "golden: unsigned validates: %s\n", why);
    gbl_sig_info_t si;
    CHECK(gbl_locate_signature(s, ns, &si, &why) && si.has_signature, "golden: signature located: %s\n", why);
    CHECK(ns == nu + GBL_TAG_HDR_LEN + GBL_SIGNATURE_LEN, "golden: signed is exactly 72 bytes longer\n");
    CHECK(gbl_sig_verify(s, si.signed_len, s + si.sig_off + GBL_TAG_HDR_LEN, k) == 0, "golden: commander signature verifies\n");
    size_t nl = gbl_strip_signature(s, ns, &si);
    CHECK(nl == nu && memcmp(s, u, nu) == 0, "golden: strip(signed) == commander's unsigned output\n");
    free(s); free(u); free(k);
}

int main(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[1], "golden") == 0) {
        run_golden(argv[2], argv[3], argv[4]);
    } else if (argc == 5 && strcmp(argv[1], "buildtag") == 0) {
        run_build_tag_real(argv[2], (int32_t)atoi(argv[3]), strcmp(argv[4], "allow") ? argv[4] : NULL);
    } else if (argc == 2) {
        run_vectors(argv[1]);
        run_build_tag_tests();
    } else {
        fprintf(stderr, "usage: %s <vector dir> | golden <signed.gbl> <unsigned.gbl> <pub.xy>\n", argv[0]);
        return 2;
    }
    printf("Result: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
