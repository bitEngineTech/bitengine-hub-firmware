#pragma once

/* The UART half of "get the MG24 into its Gecko bootloader", kept free of
   ESP-IDF so test_host/ can drive it against a scripted fake MG24.

   The caller owns nBOOT: it must already be held low when this runs, because
   the bootloader samples it on every reset, whatever caused the reset. This
   module only decides what to say on the UART to make the MG24 reset:

     1. ASH RST           — a running EZSP NCP resets itself
     2. '\r'              — already parked in the bootloader (it has no idle
                            timeout and printed its menu before we listened)
     3. "\rreset\r"       — a RAILtest image (docs/test_cases_rf_bench.md): it
                            ignores ASH RST, but its CLI has a `reset` command
                            (debug_ci.c resetChip → NVIC_SystemReset)

   Each later step only runs when the earlier ones produced no prompt, so an
   NCP that honoured ASH RST never sees the RAILtest bytes. If it reset but
   came back as the application anyway (nBOOT not latched), it does get them:
   they carry no 0x7E flag, and ASH drops them at the next flag or cancel
   byte the host sends. The CLI step proves only that a prompt appeared after
   `reset`, not that the image was RAILtest. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Gecko bootloader menu prompt (btl_comm_xmodem_common.c). */
#define MG24_BL_PROMPT "BL > "

typedef struct {
    void *ctx;
    /* Write all bytes and wait until they have left the wire. */
    bool (*send)(void *ctx, const uint8_t *data, int len);
    /* Read until `needle` appears or timeout. Text seen goes to sink. */
    bool (*wait_text)(void *ctx, const char *needle, int timeout_ms,
                      char *sink, size_t sink_size);
    int  (*get_baud)(void *ctx);
    bool (*set_baud)(void *ctx, int baud);
    void (*flush_input)(void *ctx);
} mg24_bl_io_t;

typedef enum {
    MG24_BL_VIA_ASH_RST,    /* the NCP honoured ASH RST */
    MG24_BL_VIA_NUDGE,      /* it was already in the bootloader */
    MG24_BL_VIA_CLI_RESET,  /* a CLI image (RAILtest) reset on command */
    MG24_BL_NO_PROMPT,      /* none of the three worked */
    MG24_BL_IO_ERROR,       /* UART write or baud switch failed */
} mg24_bl_route_t;

/* `app_baud` is the rate the running image talks at (NCP and RAILtest share
   one UART config), `bl_baud` the bootloader's. On return the UART is left at
   bl_baud for every outcome except MG24_BL_IO_ERROR; the caller restores
   app_baud on failure as before. `detail` gets a human-readable reason. */
mg24_bl_route_t mg24_bl_entry_run(const mg24_bl_io_t *io, int app_baud,
                                  int bl_baud, int timeout_ms,
                                  char *detail, size_t detail_size);

#ifdef __cplusplus
}
#endif
