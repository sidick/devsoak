/*
 * ring.c - devsoak op ring buffer (implementation brief S9, point 4).
 *
 * Every worker/invariant/audit op is logged here so that a data mismatch
 * or a hang can be diagnosed from "what else was in flight". The buffer
 * is a fixed-size circular array of struct RingEntry, allocated once at
 * startup and never grown; ring_log()/ring_dump() never call AllocMem, so
 * ring_dump() is safe to call from an error path or the watchdog even if
 * memory is tight.
 *
 * Single global ring for now: M2 has one invariant/audit task plus N
 * workers all logging into it, so ring_log() takes Forbid()/Permit()
 * around the copy-and-advance. That is cheap (a struct copy plus a
 * pointer bump) and keeps entries from tearing when two tasks log at
 * once; it is not a full multi-reader-safe structure, but ring_dump() is
 * only ever called from main (after quiescing workers) or from the
 * watchdog on a fatal path, so a Forbid()'d snapshot per entry is enough.
 */

#include "devsoak.h"

#include <exec/memory.h>
#include <proto/exec.h>

static struct RingEntry *ring;
static ULONG ring_size;      /* entries actually allocated */
static ULONG ring_next;      /* next slot to write */
static ULONG ring_wrapped;   /* nonzero once we've gone all the way round */
static ULONG ring_total;     /* total ops ever logged (for the header line) */

LONG
ring_init(ULONG entries)
{
    APTR mem;

    ring = NULL;
    ring_size = 0;
    ring_next = 0;
    ring_wrapped = 0;
    ring_total = 0;

    while (entries >= 64) {
        mem = AllocMem(entries * (ULONG)sizeof(struct RingEntry),
                        MEMF_PUBLIC | MEMF_CLEAR);
        if (mem != NULL) {
            ring = (struct RingEntry *)mem;
            ring_size = entries;
            return 0;
        }
        entries >>= 1;
    }

    return 1;
}

void
ring_cleanup(void)
{
    if (ring != NULL) {
        FreeMem(ring, ring_size * (ULONG)sizeof(struct RingEntry));
        ring = NULL;
    }
    ring_size = 0;
    ring_next = 0;
    ring_wrapped = 0;
}

void
ring_log(const struct RingEntry *e)
{
    if (ring == NULL || ring_size == 0)
        return;

    Forbid();
    ring[ring_next] = *e;
    ring_next++;
    if (ring_next >= ring_size) {
        ring_next = 0;
        ring_wrapped = 1;
    }
    ring_total++;
    Permit();
}

/* One compact (<200 char) line per entry: index, timestamp, worker,
 * command (hex + name), byte offset, length, data pointer, err, actual. */
static void
dump_line(ULONG idx, const struct RingEntry *e)
{
    if (e->off_hi != 0) {
        out_printf("%lu: %lu.%03lu w%lu cmd=0x%lx(%s) off=0x%lx%08lx "
                   "len=%lu data=0x%lx err=%ld act=%lu",
                   (ULONG)idx, e->secs, e->micros / 1000UL,
                   (ULONG)e->worker, (ULONG)e->cmd, op_cmd_name(e->cmd),
                   e->off_hi, e->off_lo, e->length, (ULONG)e->data,
                   (LONG)e->err, e->actual);
    } else {
        out_printf("%lu: %lu.%03lu w%lu cmd=0x%lx(%s) off=0x%lx "
                   "len=%lu data=0x%lx err=%ld act=%lu",
                   (ULONG)idx, e->secs, e->micros / 1000UL,
                   (ULONG)e->worker, (ULONG)e->cmd, op_cmd_name(e->cmd),
                   e->off_lo, e->length, (ULONG)e->data,
                   (LONG)e->err, e->actual);
    }
}

void
ring_dump(ULONG maxn)
{
    ULONG total, n, start, i, idx;
    struct RingEntry snap;

    if (ring == NULL || ring_size == 0) {
        out_printf("ring: empty (no ring buffer allocated)");
        return;
    }

    Forbid();
    total = ring_wrapped ? ring_size : ring_next;
    n = (maxn < total) ? maxn : total;
    start = ring_wrapped ? ((ring_next + ring_size - n) % ring_size)
                          : (ring_next - n);
    Permit();

    out_printf("ring: last %lu of %lu ops", n, ring_total);

    for (i = 0; i < n; i++) {
        idx = (start + i) % ring_size;

        Forbid();
        snap = ring[idx];
        Permit();

        dump_line(i, &snap);
    }
}
