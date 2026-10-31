#include "ncp_unlock.h"

void ncp_unlock_arm(ncp_unlock_t *u, int64_t now)
{
    if (!u) return;
    u->arm_until = now + NCP_UNLOCK_ARM_S;
}

bool ncp_unlock_confirm(ncp_unlock_t *u, int64_t now)
{
    if (!u) return false;
    if (u->arm_until == 0 || now > u->arm_until) {
        /* Not armed, or armed too long ago: a press means nothing. Clear a
           stale window so the next status read does not claim it is open. */
        u->arm_until = 0;
        return false;
    }
    u->arm_until    = 0;                          /* one confirmation per arm */
    u->unlock_until = now + NCP_UNLOCK_WINDOW_S;
    return true;
}

void ncp_unlock_lock(ncp_unlock_t *u)
{
    if (!u) return;
    u->arm_until    = 0;
    u->unlock_until = 0;
}

bool ncp_unlock_armed(const ncp_unlock_t *u, int64_t now)
{
    return u && u->arm_until != 0 && now <= u->arm_until;
}

bool ncp_unlock_active(const ncp_unlock_t *u, int64_t now)
{
    return u && u->unlock_until != 0 && now < u->unlock_until;
}

int ncp_unlock_arm_left(const ncp_unlock_t *u, int64_t now)
{
    if (!ncp_unlock_armed(u, now)) return 0;
    return (int)(u->arm_until - now);
}

int ncp_unlock_left(const ncp_unlock_t *u, int64_t now)
{
    if (!ncp_unlock_active(u, now)) return 0;
    return (int)(u->unlock_until - now);
}
