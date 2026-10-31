#define _GNU_SOURCE   /* memmem */
/* Host test for mg24_bl_entry.c — the UART sequence that gets the MG24 into
   its bootloader, against a scripted fake MG24 in four states:

     NCP        resets on ASH RST
     BOOTLOADER reprints its menu on any byte that is not '1'/'2'/'3'
     RAILTEST   ignores ASH RST; line CLI, `reset` reboots it
     DEAD       says nothing

   The fake models the one thing that bit the real hardware before: bytes
   only mean something at the rate the MG24 is listening at, and the C5 only
   reads text sent at its own current rate. */

#include "mg24_bl_entry.h"

#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, msg) do { if (cond) g_pass++; else { g_fail++; printf("  ❌ %s (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)

typedef enum { NCP, BOOTLOADER, RAILTEST, DEAD } mg_state_t;

typedef struct {
    mg_state_t state;
    int  app_baud, bl_baud;    /* MG24 side */
    int  c5_baud;              /* C5 side, moved by set_baud */
    char line[160];            /* RAILtest CLI line buffer */
    size_t line_len;
    char out[256];             /* MG24 output not yet read by the C5 */
    int  out_baud;
    char sent[512];            /* everything the C5 wrote, for assertions */
    size_t sent_len;
    int  sent_reset_baud;      /* rate "reset" went out at, 0 = never */
    int  fail_send_at;         /* 1-based send call that fails, 0 = never */
    int  send_calls;
    bool fail_set_baud;
    int  fail_set_baud_to;     /* fail only switches to this rate, 0 = n/a */
} fake_t;

static int mg_rate(const fake_t *f)
{
    return f->state == BOOTLOADER ? f->bl_baud : f->app_baud;
}

static void emit(fake_t *f, const char *text, int baud)
{
    strncat(f->out, text, sizeof(f->out) - strlen(f->out) - 1);
    f->out_baud = baud;
}

static void enter_bl(fake_t *f)
{
    f->state = BOOTLOADER;
    f->out[0] = '\0';
    emit(f, "\r\nGecko Bootloader v3.1.3\r\n1. upload gbl\r\n2. run\r\n3. ebl info\r\n" MG24_BL_PROMPT, f->bl_baud);
}

static void rx_byte(fake_t *f, uint8_t c, bool right_rate)
{
    switch (f->state) {
    case NCP:
        break;   /* framed protocol; the whole-frame check happens in fake_send */
    case BOOTLOADER:
        /* Wrong-rate bytes are framing errors, not characters. */
        if (right_rate && c != '1' && c != '2' && c != '3') {
            f->out[0] = '\0';
            emit(f, "\r\n1. upload gbl\r\n2. run\r\n3. ebl info\r\n" MG24_BL_PROMPT, f->bl_baud);
        }
        break;
    case RAILTEST:
        if (!right_rate) break;
        if (c == '\r' || c == '\n') {
            f->line[f->line_len] = '\0';
            if (strcmp(f->line, "reset") == 0) {
                enter_bl(f);   /* nBOOT is held low by the caller */
            } else if (f->line_len) {
                emit(f, "\r\nCommand not recognized\r\n> ", f->app_baud);
            } else {
                emit(f, "\r\n> ", f->app_baud);
            }
            f->line_len = 0;
        } else if (f->line_len + 1 < sizeof(f->line)) {
            f->line[f->line_len++] = (char)c;
        }
        break;
    case DEAD:
        break;
    }
}

static bool fake_send(void *ctx, const uint8_t *data, int len)
{
    fake_t *f = ctx;
    if (f->fail_send_at && ++f->send_calls == f->fail_send_at) return false;
    if (!f->fail_send_at) f->send_calls++;

    if (f->sent_len + (size_t)len < sizeof(f->sent)) {
        memcpy(f->sent + f->sent_len, data, (size_t)len);
        f->sent_len += (size_t)len;
    }
    if (len >= 5 && memmem(data, (size_t)len, "reset", 5)) f->sent_reset_baud = f->c5_baud;

    const bool right_rate = (f->c5_baud == mg_rate(f));
    static const uint8_t ash_rst[] = {0x1A, 0xC0, 0x38, 0xBC, 0x7E};
    if (f->state == NCP && right_rate && len == 5 && memcmp(data, ash_rst, 5) == 0) {
        enter_bl(f);
        return true;
    }
    for (int i = 0; i < len; i++) rx_byte(f, data[i], right_rate);
    return true;
}

static bool fake_wait(void *ctx, const char *needle, int timeout_ms, char *sink, size_t sink_size)
{
    fake_t *f = ctx;
    (void)timeout_ms;
    if (sink && sink_size) sink[0] = '\0';
    if (!f->out[0]) return false;
    const bool readable = (f->out_baud == f->c5_baud);
    const bool hit = readable && strstr(f->out, needle) != NULL;
    if (readable && sink && sink_size) {
        strncpy(sink, f->out, sink_size - 1);
        sink[sink_size - 1] = '\0';
    }
    f->out[0] = '\0';   /* consumed (or garbage at the wrong rate) */
    return hit;
}

static int  fake_get_baud(void *ctx) { return ((fake_t *)ctx)->c5_baud; }
static bool fake_set_baud(void *ctx, int b)
{
    fake_t *f = ctx;
    if (f->fail_set_baud || (f->fail_set_baud_to && b == f->fail_set_baud_to)) return false;
    f->c5_baud = b;
    return true;
}
static void fake_flush(void *ctx) { ((fake_t *)ctx)->out[0] = '\0'; }

static mg24_bl_route_t run(fake_t *f, mg_state_t st, int app, int bl, char *detail, size_t dsz)
{
    f->state = st;
    f->app_baud = app;
    f->bl_baud = bl;
    f->c5_baud = app;   /* the bridge runs at the application rate */
    if (st == BOOTLOADER) f->out[0] = '\0';   /* banner was printed before we listened */
    mg24_bl_io_t io = {
        .ctx = f, .send = fake_send, .wait_text = fake_wait,
        .get_baud = fake_get_baud, .set_baud = fake_set_baud, .flush_input = fake_flush,
    };
    return mg24_bl_entry_run(&io, app, bl, 3000, detail, dsz);
}

static bool sent_contains(const fake_t *f, const char *s)
{
    return memmem(f->sent, f->sent_len, s, strlen(s)) != NULL;
}

int main(void)
{
    static const int rates[][2] = { {460800, 460800}, {460800, 115200} };
    char detail[80];   /* what both callers in ota_mg24.c use */

    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        const int app = rates[r][0], bl = rates[r][1];
        printf("-- app %d / bootloader %d\n", app, bl);

        fake_t f;
        memset(&f, 0, sizeof(f));
        mg24_bl_route_t rt = run(&f, NCP, app, bl, detail, sizeof(detail));
        CHECK(rt == MG24_BL_VIA_ASH_RST, "healthy NCP enters on ASH RST");
        CHECK(!sent_contains(&f, "reset"), "healthy NCP never sees the RAILtest reset");
        CHECK(f.sent_len == 5, "healthy NCP gets exactly the ASH RST frame, no nudge");
        CHECK(f.c5_baud == bl && f.state == BOOTLOADER, "left at bootloader rate, MG24 in bootloader");

        memset(&f, 0, sizeof(f));
        rt = run(&f, BOOTLOADER, app, bl, detail, sizeof(detail));
        CHECK(rt == MG24_BL_VIA_ASH_RST || rt == MG24_BL_VIA_NUDGE, "already-parked bootloader is found");
        CHECK(app == bl || rt == MG24_BL_VIA_NUDGE, "different rates: found by the nudge, not the ASH RST");
        CHECK(!sent_contains(&f, "reset"), "parked bootloader never gets the RAILtest reset");

        memset(&f, 0, sizeof(f));
        rt = run(&f, RAILTEST, app, bl, detail, sizeof(detail));
        CHECK(rt == MG24_BL_VIA_CLI_RESET, "RAILtest enters via its CLI reset");
        CHECK(f.sent_reset_baud == app, "reset is spoken at the application rate");
        CHECK(f.c5_baud == bl && f.state == BOOTLOADER, "left at bootloader rate, MG24 in bootloader");

        memset(&f, 0, sizeof(f));
        rt = run(&f, DEAD, app, bl, detail, sizeof(detail));
        CHECK(rt == MG24_BL_NO_PROMPT, "dead MG24 reports no prompt");
        CHECK(strstr(detail, "J-Link") != NULL, "…and the detail points at J-Link, even in an 80-byte buffer");
        CHECK(f.c5_baud == bl, "…and leaves the UART at bootloader rate for the caller to restore");
    }

    printf("-- RAILtest with junk already in its line buffer\n");
    {
        fake_t f;
        memset(&f, 0, sizeof(f));
        strcpy(f.line, "setTxTo");   /* half-typed command from a nc session */
        f.line_len = strlen(f.line);
        mg24_bl_route_t rt = run(&f, RAILTEST, 460800, 115200, detail, sizeof(detail));
        CHECK(rt == MG24_BL_VIA_CLI_RESET, "leading CR flushes the stale line before reset");
    }

    printf("-- I/O failures\n");
    {
        fake_t f;
        memset(&f, 0, sizeof(f));
        f.fail_send_at = 1;
        CHECK(run(&f, NCP, 460800, 460800, detail, sizeof(detail)) == MG24_BL_IO_ERROR, "ASH RST write failure is an I/O error");
        CHECK(strstr(detail, "ASH RST") != NULL, "…named in the detail");

        memset(&f, 0, sizeof(f));
        f.fail_send_at = 2;
        CHECK(run(&f, DEAD, 460800, 460800, detail, sizeof(detail)) == MG24_BL_IO_ERROR, "nudge write failure is an I/O error");

        memset(&f, 0, sizeof(f));
        f.fail_set_baud_to = 460800;   /* back to the application rate for the CLI step */
        CHECK(run(&f, RAILTEST, 460800, 115200, detail, sizeof(detail)) == MG24_BL_IO_ERROR, "switch back to application rate failing is an I/O error");
        CHECK(strstr(detail, "application rate") != NULL, "…named in the detail");

        memset(&f, 0, sizeof(f));
        f.fail_send_at = 3;
        CHECK(run(&f, RAILTEST, 460800, 460800, detail, sizeof(detail)) == MG24_BL_IO_ERROR, "CLI reset write failure is an I/O error");

        memset(&f, 0, sizeof(f));
        f.fail_set_baud = true;
        CHECK(run(&f, NCP, 460800, 115200, detail, sizeof(detail)) == MG24_BL_IO_ERROR, "baud switch failure is an I/O error");
        memset(&f, 0, sizeof(f));
        f.fail_set_baud = true;
        CHECK(run(&f, NCP, 460800, 460800, detail, sizeof(detail)) == MG24_BL_VIA_ASH_RST, "equal rates never call set_baud");
    }

    printf("-- bad arguments\n");
    {
        mg24_bl_io_t io;
        memset(&io, 0, sizeof(io));
        CHECK(mg24_bl_entry_run(NULL, 460800, 460800, 3000, detail, sizeof(detail)) == MG24_BL_IO_ERROR, "NULL io");
        CHECK(mg24_bl_entry_run(&io, 460800, 460800, 3000, detail, sizeof(detail)) == MG24_BL_IO_ERROR, "io without callbacks");
        CHECK(mg24_bl_entry_run(&io, 460800, 460800, 3000, NULL, 0) == MG24_BL_IO_ERROR, "NULL detail is tolerated");
    }

    printf("\nbl_entry: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
