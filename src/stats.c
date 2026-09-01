/*
 * stats.c - semaphore-guarded stats + latency histogram (implementation
 * brief S6).
 *
 * A single static struct holds per-class op counts, byte counts, a
 * running error count and a log2(usec) latency histogram, all guarded by
 * one SignalSemaphore so stats_record() is safe from any task (workers,
 * the auditor, main). stats_snapshot() is documented as main-only but
 * still takes the semaphore -- cheap insurance, and it means the
 * "main-only" rule is a convention rather than something a future caller
 * could get wrong silently.
 *
 * Nothing in this file prints; stats_snapshot()'s caller owns reporting.
 */

#include "devsoak.h"

#include <proto/exec.h>
#include <exec/semaphores.h>

extern struct ExecBase *SysBase;

struct Stats {
    struct SignalSemaphore sem;
    ULONG ops[CLASS_COUNT];
    U64   bytes[CLASS_COUNT];
    ULONG errors;
    ULONG lat[CLASS_COUNT][LAT_BUCKETS];
};

static struct Stats stats;

/* Position of the highest set bit of v (0 if v == 0), i.e. floor(log2(v))
   with a 0 fallback for the zero case -- matches "bucket 0" being the
   finest-grained (sub-2us) bucket. */
static ULONG highbit(ULONG v)
{
    ULONG b = 0;

    if (v == 0)
        return 0;

    while (v >>= 1)
        b++;

    return b;
}

void stats_init(void)
{
    ULONG c, b;

    InitSemaphore(&stats.sem);

    for (c = 0; c < CLASS_COUNT; c++) {
        stats.ops[c] = 0;
        stats.bytes[c] = 0;
        for (b = 0; b < LAT_BUCKETS; b++)
            stats.lat[c][b] = 0;
    }
    stats.errors = 0;
}

void stats_record(ULONG cls, ULONG bytes, ULONG usec, LONG err)
{
    ULONG bucket;

    if (cls >= CLASS_COUNT)
        return;

    bucket = highbit(usec);
    if (bucket > LAT_BUCKETS - 1)
        bucket = LAT_BUCKETS - 1;

    ObtainSemaphore(&stats.sem);

    stats.ops[cls]++;
    stats.bytes[cls] += bytes;
    stats.lat[cls][bucket]++;
    if (err)
        stats.errors++;

    ReleaseSemaphore(&stats.sem);
}

/* Walk the histogram for one class, returning the representative usec
   value (1UL << bucket) for the bucket at which the cumulative count
   first reaches 'pct' percent of that class's total ops; 0 if the class
   has no ops at all. Called with stats.sem already held. */
static ULONG percentile_usec(ULONG cls, ULONG pct)
{
    ULONG total, target, cum, b;

    total = stats.ops[cls];
    if (total == 0)
        return 0;

    target = (total * pct + 99) / 100;   /* ceiling, so pct=100 needs 'total' */
    if (target == 0)
        target = 1;

    cum = 0;
    for (b = 0; b < LAT_BUCKETS; b++) {
        cum += stats.lat[cls][b];
        if (cum >= target)
            return (1UL << b);
    }

    /* Every recorded op falls in some bucket, so this is unreachable, but
       keep a safe fallback rather than an unresolved read. */
    return (1UL << (LAT_BUCKETS - 1));
}

void stats_snapshot(struct StatsSnap *out)
{
    ULONG c;

    ObtainSemaphore(&stats.sem);

    for (c = 0; c < CLASS_COUNT; c++) {
        out->ops[c] = stats.ops[c];
        out->bytes[c] = stats.bytes[c];
        out->p50_usec[c] = percentile_usec(c, 50);
        out->p99_usec[c] = percentile_usec(c, 99);
    }
    out->errors = stats.errors;

    ReleaseSemaphore(&stats.sem);
}
