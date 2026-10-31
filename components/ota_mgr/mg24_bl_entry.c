#include "mg24_bl_entry.h"

#include <string.h>

/* ASH RST — same frame ezsp_bridge_health_probe uses. */
static const uint8_t ASH_RST[] = {0x1A, 0xC0, 0x38, 0xBC, 0x7E};

/* RAILtest's CLI ends a line on '\r' or '\n' (sl_cli_input.c) and keeps up to
   128 bytes of it. The leading '\r' executes whatever junk the ASH RST bytes
   left in that buffer as its own (unknown) command, so `reset` starts on a
   clean line. To an EZSP NCP these bytes are noise with no 0x7E flag; ASH
   drops them at the next flag or cancel byte. To a bootloader menu that
   appeared late they are not '1'/'2'/'3', so it just reprints itself. */
static const uint8_t CLI_RESET[] = "\rreset\r";

static void put_detail(char *detail, size_t size, const char *msg)
{
    if (!detail || !size) return;
    strncpy(detail, msg, size - 1);
    detail[size - 1] = '\0';
}

static bool to_baud(const mg24_bl_io_t *io, int baud)
{
    return io->get_baud(io->ctx) == baud || io->set_baud(io->ctx, baud);
}

mg24_bl_route_t mg24_bl_entry_run(const mg24_bl_io_t *io, int app_baud,
                                  int bl_baud, int timeout_ms,
                                  char *detail, size_t detail_size)
{
    if (!io || !io->send || !io->wait_text || !io->get_baud || !io->set_baud
        || !io->flush_input || app_baud <= 0 || bl_baud <= 0 || timeout_ms <= 0) {
        put_detail(detail, detail_size, "bootloader entry: bad arguments");
        return MG24_BL_IO_ERROR;
    }

    /* 1. ASH RST at the application's rate. */
    io->flush_input(io->ctx);
    if (!io->send(io->ctx, ASH_RST, sizeof(ASH_RST))) {
        put_detail(detail, detail_size, "UART write failed sending ASH RST");
        return MG24_BL_IO_ERROR;
    }
    /* The banner comes back at the bootloader's rate. */
    if (!to_baud(io, bl_baud)) {
        put_detail(detail, detail_size, "baud switch to bootloader rate failed");
        return MG24_BL_IO_ERROR;
    }
    if (io->wait_text(io->ctx, MG24_BL_PROMPT, timeout_ms, detail, detail_size)) {
        return MG24_BL_VIA_ASH_RST;
    }

    /* 2. Already parked in the bootloader: any byte that is not '1'/'2'/'3'
       makes getAction() reprint the menu. */
    static const uint8_t nudge = '\r';
    if (!io->send(io->ctx, &nudge, 1)) {
        put_detail(detail, detail_size, "UART write failed sending nudge");
        return MG24_BL_IO_ERROR;
    }
    if (io->wait_text(io->ctx, MG24_BL_PROMPT, timeout_ms, detail, detail_size)) {
        return MG24_BL_VIA_NUDGE;
    }

    /* 3. A RAILtest image: speak its CLI at the application's rate, then
       listen for the banner at the bootloader's. */
    if (!to_baud(io, app_baud)) {
        put_detail(detail, detail_size, "baud switch to application rate failed");
        return MG24_BL_IO_ERROR;
    }
    io->flush_input(io->ctx);
    if (!io->send(io->ctx, CLI_RESET, (int)(sizeof(CLI_RESET) - 1))) {
        put_detail(detail, detail_size, "UART write failed sending CLI reset");
        return MG24_BL_IO_ERROR;
    }
    if (!to_baud(io, bl_baud)) {
        put_detail(detail, detail_size, "baud switch to bootloader rate failed");
        return MG24_BL_IO_ERROR;
    }
    if (io->wait_text(io->ctx, MG24_BL_PROMPT, timeout_ms, detail, detail_size)) {
        return MG24_BL_VIA_CLI_RESET;
    }

    /* ≤ 79 bytes: both callers and the OTA status keep an 80-byte detail. */
    put_detail(detail, detail_size,
               "no bootloader prompt after ASH RST, nudge or CLI reset; recover with J-Link");
    return MG24_BL_NO_PROMPT;
}
