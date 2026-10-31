#pragma once

/*
 * Developer unlock for unsigned MG24 (.gbl) images — the pure state machine.
 *
 * Product decision (docs/产品化TODO_2026-09-09.md T1.2, 2026-09-14): the
 * gateway only flashes NCP images signed with the official key, but a person
 * standing at the device may override that for ten minutes:
 *
 *   1. an authenticated REST call ARMS a 30 s confirmation window;
 *   2. within that window the physical button is held 2-10 s and released
 *      (the band that otherwise does nothing — button.c's guard band);
 *   3. unsigned images are then accepted for NCP_UNLOCK_WINDOW_S seconds,
 *      after which the gateway locks itself again. A reboot always locks.
 *
 * Why two steps: the button alone must never mean anything (a stray press
 * must not open the device), and the password alone must never be enough
 * (the NCP holds the Zigbee network key; whoever can flash arbitrary NCP
 * firmware owns that network). Remote attacker with the password: cannot
 * press. Person with the button but no password: cannot arm.
 *
 * No ESP-IDF here so test_host/unlock_test.c can drive it with a fake clock.
 * Time is int64 seconds from any monotonic source; 0 is a valid "now".
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NCP_UNLOCK_ARM_S      30     /* press the button within this */
#define NCP_UNLOCK_WINDOW_S   600    /* unsigned images accepted for this */

typedef struct {
    int64_t arm_until;      /* 0 = not armed; otherwise the confirmation deadline */
    int64_t unlock_until;   /* 0 = locked; otherwise unsigned images accepted until then */
} ncp_unlock_t;

/** Open (or re-open) the confirmation window. Does not touch an active unlock. */
void ncp_unlock_arm(ncp_unlock_t *u, int64_t now);

/**
 * Button confirmation. True when a window was open: the window is consumed
 * and the unlock runs NCP_UNLOCK_WINDOW_S from @p now (re-confirming during
 * an active unlock restarts the ten minutes). False when nothing was armed
 * or the window had expired — the press then has no effect at all.
 */
bool ncp_unlock_confirm(ncp_unlock_t *u, int64_t now);

/** Cancel the window and any active unlock. */
void ncp_unlock_lock(ncp_unlock_t *u);

bool ncp_unlock_armed(const ncp_unlock_t *u, int64_t now);
bool ncp_unlock_active(const ncp_unlock_t *u, int64_t now);

/** Seconds left in the confirmation window / the unlock; 0 when not open. */
int ncp_unlock_arm_left(const ncp_unlock_t *u, int64_t now);
int ncp_unlock_left(const ncp_unlock_t *u, int64_t now);

#ifdef __cplusplus
}
#endif
