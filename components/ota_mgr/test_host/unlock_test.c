/* Host test for ncp_unlock.c — the developer-unlock state machine with a
   fake clock. Every rule the product decision depends on is a line here. */

#include "ncp_unlock.h"

#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, msg) do { if (cond) g_pass++; else { g_fail++; printf("  ❌ %s (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)

int main(void)
{
    ncp_unlock_t u;

    printf("-- fresh state (== after reboot)\n");
    memset(&u, 0, sizeof(u));
    CHECK(!ncp_unlock_armed(&u, 0) && !ncp_unlock_active(&u, 0), "boots locked and unarmed");
    CHECK(!ncp_unlock_confirm(&u, 5), "button press without arming does nothing");
    CHECK(!ncp_unlock_active(&u, 5), "…and does not unlock");

    printf("-- arm, then confirm inside the window\n");
    ncp_unlock_arm(&u, 100);
    CHECK(ncp_unlock_armed(&u, 100) && ncp_unlock_arm_left(&u, 100) == NCP_UNLOCK_ARM_S, "armed for 30 s");
    CHECK(!ncp_unlock_active(&u, 100), "arming alone does not unlock");
    CHECK(ncp_unlock_armed(&u, 130) && !ncp_unlock_armed(&u, 131), "window is inclusive at 30 s, closed at 31 s");
    CHECK(ncp_unlock_confirm(&u, 120), "press at 20 s confirms");
    CHECK(!ncp_unlock_armed(&u, 120), "window consumed by the confirmation");
    CHECK(ncp_unlock_active(&u, 120) && ncp_unlock_left(&u, 120) == NCP_UNLOCK_WINDOW_S, "unlocked for 600 s");
    CHECK(ncp_unlock_active(&u, 719) && !ncp_unlock_active(&u, 720), "unlock ends exactly after 600 s");
    CHECK(ncp_unlock_left(&u, 800) == 0, "left() is 0 once expired");
    CHECK(!ncp_unlock_confirm(&u, 200), "second press without re-arming is ignored");

    printf("-- arm, then miss the window\n");
    memset(&u, 0, sizeof(u));
    ncp_unlock_arm(&u, 1000);
    CHECK(!ncp_unlock_confirm(&u, 1031), "press at 31 s is too late");
    CHECK(!ncp_unlock_active(&u, 1031), "…and nothing is unlocked");
    CHECK(!ncp_unlock_armed(&u, 1031), "stale window cleared by the late press");

    printf("-- re-arm while unlocked extends; lock cancels everything\n");
    memset(&u, 0, sizeof(u));
    ncp_unlock_arm(&u, 0);
    CHECK(ncp_unlock_confirm(&u, 1), "confirm");
    ncp_unlock_arm(&u, 500);
    CHECK(ncp_unlock_active(&u, 500), "re-arming keeps the current unlock");
    CHECK(ncp_unlock_confirm(&u, 510), "re-confirm");
    CHECK(ncp_unlock_left(&u, 510) == NCP_UNLOCK_WINDOW_S, "re-confirm restarts the 600 s");
    ncp_unlock_lock(&u);
    CHECK(!ncp_unlock_active(&u, 511) && !ncp_unlock_armed(&u, 511), "lock() clears both");
    CHECK(!ncp_unlock_confirm(&u, 512), "press after lock() is ignored");

    printf("-- NULL safety\n");
    CHECK(!ncp_unlock_confirm(NULL, 0) && !ncp_unlock_armed(NULL, 0) && !ncp_unlock_active(NULL, 0), "NULL never unlocks");
    ncp_unlock_arm(NULL, 0);
    ncp_unlock_lock(NULL);
    CHECK(ncp_unlock_arm_left(NULL, 0) == 0 && ncp_unlock_left(NULL, 0) == 0, "NULL left() is 0");

    printf("Result: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
