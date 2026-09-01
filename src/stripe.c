/*
 * stripe.c - per-stripe SignalSemaphores (implementation brief S6).
 *
 * The range under test is divided into fixed-size stripes of cfg.stripe
 * sectors each; one SignalSemaphore per stripe. Workers never block on a
 * stripe: stripes_attempt() is all-or-nothing and non-blocking, so a
 * worker that can't get every stripe its op touches just picks a
 * different op instead of risking deadlock. Only a task holding no
 * stripes at all (the auditor, main) may use the blocking
 * stripes_obtain(), always ascending, for the same reason lock ordering
 * prevents deadlock anywhere else: consistent ascending acquisition order
 * everywhere means no cycle is possible.
 *
 * Nothing in this file prints; callers own error reporting.
 */

#include "devsoak.h"

#include <proto/exec.h>
#include <exec/semaphores.h>

extern struct ExecBase *SysBase;

static struct SignalSemaphore *stripe_sems;
static ULONG                   n_stripes;
static ULONG                   stripe_size;   /* sectors per stripe */

static ULONG stripe_of(ULONG rel_sector)
{
    return rel_sector / stripe_size;
}

LONG stripes_init(void)
{
    ULONG i;

    stripe_size = cfg.stripe;
    if (stripe_size == 0)
        stripe_size = 256;

    /* cfg.range_len is a U64 sector count but the whole point of striping
       is that a stripe is a small, addressable chunk of it; the driver
       under test caps transfers far below 2^32 stripes' worth of range,
       so this narrows safely for any range devsoak can actually be
       pointed at. */
    n_stripes = (ULONG)((cfg.range_len + stripe_size - 1) / stripe_size);
    if (n_stripes == 0)
        n_stripes = 1;

    stripe_sems = (struct SignalSemaphore *)
        AllocMem(n_stripes * sizeof(struct SignalSemaphore),
                 MEMF_PUBLIC | MEMF_CLEAR);
    if (stripe_sems == NULL) {
        n_stripes = 0;
        return 1;
    }

    for (i = 0; i < n_stripes; i++)
        InitSemaphore(&stripe_sems[i]);

    return 0;
}

void stripes_cleanup(void)
{
    if (stripe_sems != NULL) {
        FreeMem(stripe_sems, n_stripes * sizeof(struct SignalSemaphore));
        stripe_sems = NULL;
    }
    n_stripes = 0;
}

LONG stripes_attempt(ULONG start, ULONG n)
{
    ULONG first, last, i;

    if (n == 0)
        return 1;

    first = stripe_of(start);
    last = stripe_of(start + n - 1);

    for (i = first; i <= last; i++) {
        if (!AttemptSemaphore(&stripe_sems[i])) {
            /* back off everything already taken in this attempt */
            while (i > first) {
                i--;
                ReleaseSemaphore(&stripe_sems[i]);
            }
            return 0;
        }
    }

    return 1;
}

void stripes_release(ULONG start, ULONG n)
{
    ULONG first, last, i;

    if (n == 0)
        return;

    first = stripe_of(start);
    last = stripe_of(start + n - 1);

    for (i = first; i <= last; i++)
        ReleaseSemaphore(&stripe_sems[i]);
}

void stripes_obtain(ULONG start, ULONG n)
{
    ULONG first, last, i;

    if (n == 0)
        return;

    first = stripe_of(start);
    last = stripe_of(start + n - 1);

    for (i = first; i <= last; i++)
        ObtainSemaphore(&stripe_sems[i]);
}
