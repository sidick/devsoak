/*
 * removable.c - devsoak change-interrupt phase (implementation brief S8
 * "Change interrupts (-R removable mode)").
 *
 * One-shot phase, main task, run after the initial audit and before any
 * worker/auditor/invariant task exists (engine.c calls this from
 * engine_run() with nothing else touching dev.io or the range). That
 * means dev.io/dev.port (main's own persistent request/port) are free to
 * reuse here for simple ops, exactly like engine.c's setup() does for
 * TD_CHANGENUM.
 *
 * Three TD_ADDCHANGEINT requests are installed on a private port (their
 * own IOExtTDs, cloned io_Device/io_Unit from dev.io -- the standard
 * multi-request idiom used throughout the tree, see audit.c). Per the
 * rkrm-devices ADDCHANGEINT contract, each such request is SendIO'd and
 * then never completes on its own: it sits pending in the driver until a
 * matching TD_REMCHANGEINT (same io_Data pointer) is issued, at which
 * point the ADDCHANGEINT request completes and REMCHANGEINT itself
 * returns. Every path through this file -- including every early-FAIL
 * return -- must therefore either REMCHANGEINT+WaitIO each pending
 * request or, if the driver completed one on its own (a rejection),
 * WaitIO it once to reap the message already sitting at the port. See
 * teardown() below, which is the single funnel all paths go through.
 *
 * The interrupt handler itself runs in the driver's Cause()/interrupt
 * context with is_Data in A1 (rkrm-devices): it must be tiny and must
 * never call anything that assumes Forbid() is not held. A C function
 * with a `register ... asm("a1")` parameter is fragile for an *indirect*
 * call under this toolchain (the compiler is free to pick a different
 * register-allocation strategy for an address taken via is_Code, since
 * nothing enforces the parameter's register at the call site -- there is
 * no call site in our own code, the driver does the equivalent of
 * `jsr (is_Code)` with A1 preloaded, and the calling convention is
 * whatever the *driver* was told to expect, i.e. none at all beyond "A1
 * holds is_Data"). So, exactly like output.c's putch_code trick, each
 * handler is a two-instruction machine code stub with no C calling
 * convention involved whatsoever: `addq.l #1,(a1) ; rts`. is_Data points
 * directly at the ULONG counter, so the stub increments it in place.
 *
 * addq.l #1,(a1) encodes (68000 PRM 4-9, ADDQ, pattern
 * 0101 qqq 0 ss mmmrrr, sz=10 for .l, mode=010/reg=001 for (A1)):
 *   1010  001   0  10  010     001
 *   ADDQ  q=1  op  .L  (An)    An=A1
 * bits:  0101 0010 1001 0001 = 0x5291, followed by RTS = 0x4E75.
 * Three independent counters (three stub/data pairs) so each of the
 * three installed interrupts is provably firing on its own.
 */

#include "devsoak.h"

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/alib.h>   /* CreatePort/CreateExtIO/DeleteExtIO/DeletePort */
#include <exec/interrupts.h>
#include <exec/memory.h>
#include <exec/errors.h>
#include <string.h>        /* memset */

extern struct ExecBase *SysBase;

#define N_INT 3

/* addq.l #1,(a1) ; rts -- see file header for the encoding derivation.
 * Called by the driver with is_Data (the matching counter's address) in
 * A1; no other register/stack assumption is made, so no C calling
 * convention risk exists at all. */
static const UWORD chg_stub[2] = { 0x5291, 0x4E75 };

static volatile ULONG chg_count[N_INT];

struct removable_state {
    struct MsgPort   *port;
    UBYTE             have_port;

    struct IOExtTD   *aio[N_INT];    /* the three ADDCHANGEINT requests */
    UBYTE             created[N_INT];/* CreateExtIO'd */
    UBYTE             sent[N_INT];   /* SendIO'd */
    UBYTE             completed[N_INT]; /* driver completed it on its own
                                            (rejection or spurious) --
                                            already WaitIO'd, no REMCHANGEINT
                                            needed */
    UBYTE             remdone[N_INT];/* TD_REMCHANGEINT already issued and
                                         the pending request reaped */
    struct Interrupt  ints[N_INT];

    struct IOExtTD   *rio;           /* fourth request: ETD_READ probe /
                                         the post-eject stale-count read */
    UBYTE             have_rio;
    UBYTE             rio_pending;

    UBYTE            *readbuf;
    ULONG             readbuf_len;

    ULONG             failures;
};

/* ---- small helpers ---- */

static UBYTE
etd_enabled(void)
{
    ULONG i;

    for (i = 0; i < g_n_enabled; i++) {
        if (g_enabled_dialects[i] == DIALECT_ETD)
            return 1;
    }
    return 0;
}

/* peek+reap: if the request has completed, WaitIO it (returns immediately
 * since it is already done) so its reply message leaves the port. */
static UBYTE
reap_if_complete(struct IOExtTD *io)
{
    if (CheckIO((struct IORequest *)io) == NULL)
        return 0;
    WaitIO((struct IORequest *)io);
    return 1;
}

/* Opportunistic scan of the three ADDCHANGEINT requests, called from every
 * poll loop: catches a driver that completes one on its own mid-phase
 * (error, or a spurious "rejection" after the fact) instead of only ever
 * completing it in response to TD_REMCHANGEINT. Any such completion is a
 * phase failure, but not fatal to the rest of the sequence -- later steps
 * that depend on that interrupt will simply also observe the stuck
 * counter and report that too. */
static void
scan_addchangeint(struct removable_state *st)
{
    ULONG i;

    for (i = 0; i < N_INT; i++) {
        if (!st->created[i] || !st->sent[i] || st->completed[i] || st->remdone[i])
            continue;
        if (reap_if_complete(st->aio[i])) {
            st->completed[i] = 1;
            out_printf("devsoak: removable: FAIL TD_ADDCHANGEINT %ld completed "
                       "on its own (err %ld) before TD_REMCHANGEINT",
                       (LONG)i, (LONG)(BYTE)st->aio[i]->iotd_Req.io_Error);
            st->failures++;
        }
    }
}

/* Issue TD_REMCHANGEINT. The V47 autodoc is explicit: it must go through
 * "The same IO request used for TD_ADDCHANGEINT" -- the still-pending
 * request itself gets its io_Command rewritten and re-DoIO'd; the device
 * recognises its own queued request, unlinks the handler and completes
 * it. Sending TD_REMCHANGEINT on a different request (as an earlier
 * version of this file did, via dev.io) wedges trackdisk.device 47.
 * DoIO() doubles as the reap: when it returns, the request is ours
 * again. */
static void
remchangeint_one(struct removable_state *st, ULONG i)
{
    LONG err;

    st->aio[i]->iotd_Req.io_Command = TD_REMCHANGEINT;
    st->aio[i]->iotd_Req.io_Data    = (APTR)&st->ints[i];
    st->aio[i]->iotd_Req.io_Length  = sizeof(struct Interrupt);
    st->aio[i]->iotd_Req.io_Flags   = 0;

    DoIO((struct IORequest *)st->aio[i]);
    err = (LONG)(BYTE)st->aio[i]->iotd_Req.io_Error;
    if (err != 0) {
        out_printf("devsoak: removable: FAIL TD_REMCHANGEINT %ld error %ld",
                   (LONG)i, err);
        st->failures++;
    }
    st->remdone[i] = 1;
}

/* Single funnel for cleanup on every path (brief S8 requirement): reaps
 * any still-pending ADDCHANGEINT requests (via REMCHANGEINT, unless the
 * driver already completed them on its own), frees the requests/port/
 * buffers. Idempotent-by-construction via the created[]/sent[]/
 * completed[]/remdone[] flags -- safe to call once at the end of every
 * return path, however far setup got. */
static void
teardown(struct removable_state *st)
{
    ULONG i;

    for (i = 0; i < N_INT; i++) {
        if (!st->created[i])
            continue;
        if (st->sent[i] && !st->completed[i] && !st->remdone[i])
            remchangeint_one(st, i);
        DeleteExtIO((struct IORequest *)st->aio[i]);
        st->aio[i] = NULL;
        st->created[i] = 0;
    }

    if (st->have_rio) {
        if (st->rio_pending) {
            AbortIO((struct IORequest *)st->rio);
            WaitIO((struct IORequest *)st->rio);
            st->rio_pending = 0;
        }
        DeleteExtIO((struct IORequest *)st->rio);
        st->rio = NULL;
        st->have_rio = 0;
    }

    if (st->have_port) {
        DeletePort(st->port);
        st->port = NULL;
        st->have_port = 0;
    }

    if (st->readbuf != NULL) {
        FreeMem(st->readbuf, st->readbuf_len);
        st->readbuf = NULL;
    }
}

/* disk-present test via TD_CHANGESTATE: io_Actual == 0 means present. */
static UBYTE
disk_present(void)
{
    struct Result res;

    op_simple(dev.io, TD_CHANGESTATE, &res);
    return (res.actual == 0) ? 1 : 0;
}

/* Ask for eject or insert -- run the -H hook if given, else prompt on the
 * console. Runs from main (a DOS Process), so dos.library's Execute() is
 * safe here. */
static void
request_transition(struct removable_state *st, const char *what)
{
    (void)st;

    if (cfg.hookcmd != NULL) {
        out_printf("devsoak: removable: running %s hook", what);
        Execute((CONST_STRPTR)cfg.hookcmd, 0, 0);
    } else {
        out_printf("devsoak: removable: %s the disk now", what);
    }
}

/* Poll up to 60 s for the disk to reach `want_present`; scans the three
 * ADDCHANGEINT requests each iteration (S8: catch a driver completing one
 * mid-phase). Returns 0 on success, -1 on timeout. */
static LONG
poll_for_disk(struct removable_state *st, UBYTE want_present)
{
    ULONG waited_ms = 0;
    ULONG next_nudge_ms = 2000;

    for (;;) {
        if (disk_present() == want_present)
            return 0;

        /* Nudge: TD_CHANGESTATE reports the driver's cached view, which
         * some drivers refresh only on real I/O or on their own poll
         * cadence. A harmless 1-sector read forces the driver to touch
         * the hardware; on an ejected disk it fails TDERR_DiskChanged,
         * which the next TD_CHANGESTATE then reflects. */
        if (waited_ms >= next_nudge_ms && st->readbuf != NULL) {
            struct Result res;
            /* Alternate between two tracks: trackdisk.device serves
             * repeat reads of the SAME track from its in-RAM track
             * buffer without touching the hardware, so a fixed-sector
             * nudge can read "successfully" from an ejected drive
             * forever. A track-cache miss forces real I/O, which is
             * when the driver sees /DSKCHANGE. */
            ULONG tsects = (dev.have_geom && dev.geom.dg_TrackSectors)
                           ? dev.geom.dg_TrackSectors : 16;
            U64   sec = cfg.range_start +
                        (((waited_ms / 2000UL) & 1) ? (U64)(2 * tsects) : 0);

            next_nudge_ms += 2000;
            if (sec >= cfg.range_start + cfg.range_len)
                sec = cfg.range_start;
            op_build_rw(dev.io, DIALECT_CMD, 0, sec * (U64)dev.sector_size,
                        st->readbuf, dev.sector_size, g_changenum);
            op_do_sync(dev.io, SUBMIT_DOIO, &res);
            out_printf("devsoak: removable: nudge read err %ld "
                       "(changestate %ld)", (LONG)res.err,
                       (LONG)(disk_present() ? 0L : 1L));
        }

        scan_addchangeint(st);

        if (st->have_rio && st->rio_pending)
            st->rio_pending = !reap_if_complete(st->rio);

        if (waited_ms >= 60000UL)
            return -1;

        timer_delay_ms(250);
        waited_ms += 250;
    }
}

/* ---- the phase ---- */

LONG
removable_phase(void)
{
    struct removable_state st;
    ULONG i;
    ULONG base_cn, cn;
    ULONG snap_a[N_INT], snap_b[N_INT];
    struct Result res;
    UBYTE have_etd;
    LONG  rc;

    memset(&st, 0, sizeof(st));
    for (i = 0; i < N_INT; i++)
        chg_count[i] = 0;

    out_printf("devsoak: removable: phase starting");

    /* ---- step 1: install three TD_ADDCHANGEINT handlers ---- */

    st.port = CreatePort(NULL, 0);
    if (st.port == NULL) {
        out_printf("devsoak: removable: FAIL CreatePort failed");
        st.failures++;
        teardown(&st);
        return RC_ERROR;
    }
    st.have_port = 1;

    for (i = 0; i < N_INT; i++) {
        static const char *names[N_INT] = {
            "devsoak.chg0", "devsoak.chg1", "devsoak.chg2"
        };

        st.aio[i] = (struct IOExtTD *)CreateExtIO(st.port, sizeof(struct IOExtTD));
        if (st.aio[i] == NULL) {
            out_printf("devsoak: removable: FAIL CreateExtIO failed for interrupt %ld",
                       (LONG)i);
            st.failures++;
            teardown(&st);
            return RC_ERROR;
        }
        st.created[i] = 1;

        st.aio[i]->iotd_Req.io_Device = dev.io->iotd_Req.io_Device;
        st.aio[i]->iotd_Req.io_Unit   = dev.io->iotd_Req.io_Unit;

        memset(&st.ints[i], 0, sizeof(st.ints[i]));
        st.ints[i].is_Node.ln_Type = NT_INTERRUPT;
        st.ints[i].is_Node.ln_Pri  = 0;
        st.ints[i].is_Node.ln_Name = (char *)names[i];
        st.ints[i].is_Data = (APTR)&chg_count[i];
        st.ints[i].is_Code = (VOID (*)())chg_stub;

        st.aio[i]->iotd_Req.io_Command = TD_ADDCHANGEINT;
        st.aio[i]->iotd_Req.io_Data    = (APTR)&st.ints[i];
        st.aio[i]->iotd_Req.io_Length  = sizeof(struct Interrupt);
        st.aio[i]->iotd_Req.io_Offset  = 0;
        st.aio[i]->iotd_Req.io_Flags   = 0;

        out_printf("devsoak: removable: installing change interrupt %ld", (LONG)i);
        SendIO((struct IORequest *)st.aio[i]);
        st.sent[i] = 1;
    }

    /* ADDCHANGEINT does not complete on its own; give a misbehaving driver
     * a brief moment to reject it (complete it early with an error), then
     * check. */
    timer_delay_ms(100);
    {
        UBYTE any_bad = 0;

        for (i = 0; i < N_INT; i++) {
            if (reap_if_complete(st.aio[i])) {
                out_printf("devsoak: removable: FAIL TD_ADDCHANGEINT rejected "
                           "err %ld (interrupt %ld)",
                           (LONG)(BYTE)st.aio[i]->iotd_Req.io_Error, (LONG)i);
                st.completed[i] = 1;
                st.failures++;
                any_bad = 1;
            }
        }
        if (any_bad) {
            teardown(&st);
            return RC_ERROR;
        }
    }

    /* ---- step 2: baseline ---- */

    out_printf("devsoak: removable: recording baseline TD_CHANGENUM/TD_CHANGESTATE");

    op_simple(dev.io, TD_CHANGENUM, &res);
    base_cn = res.actual;

    if (!disk_present()) {
        out_printf("devsoak: removable: FAIL no disk present at start of phase");
        st.failures++;
        teardown(&st);
        return RC_ERROR;
    }

    /* ---- step 3: optional in-flight ETD_READ before the eject ---- */

    have_etd = etd_enabled();
    if (have_etd) {
        st.readbuf_len = dev.sector_size;
        st.readbuf = AllocMem(st.readbuf_len, MEMF_PUBLIC);
        if (st.readbuf == NULL) {
            out_printf("devsoak: removable: FAIL AllocMem failed for ETD probe buffer");
            st.failures++;
            teardown(&st);
            return RC_ERROR;
        }

        st.rio = (struct IOExtTD *)CreateExtIO(st.port, sizeof(struct IOExtTD));
        if (st.rio == NULL) {
            out_printf("devsoak: removable: FAIL CreateExtIO failed for ETD probe request");
            st.failures++;
            teardown(&st);
            return RC_ERROR;
        }
        st.have_rio = 1;
        st.rio->iotd_Req.io_Device = dev.io->iotd_Req.io_Device;
        st.rio->iotd_Req.io_Unit   = dev.io->iotd_Req.io_Unit;

        out_printf("devsoak: removable: starting in-flight ETD_READ before eject");
        op_build_rw(st.rio, DIALECT_ETD, 0,
                    cfg.range_start * (U64)dev.sector_size,
                    st.readbuf, dev.sector_size, g_changenum);
        SendIO((struct IORequest *)st.rio);
        st.rio_pending = 1;
    } else {
        out_printf("devsoak: removable: ETD dialect not enabled, skipping "
                   "in-flight-read and stale-count checks");
    }

    /* ---- step 4: request eject, poll for no disk ---- */

    request_transition(&st, "eject");

    if (poll_for_disk(&st, 0) != 0) {
        out_printf("devsoak: removable: FAIL no eject seen within 60 s");
        st.failures++;
        teardown(&st);
        return RC_ERROR;
    }
    out_printf("devsoak: removable: eject detected");

    if (st.have_rio && st.rio_pending) {
        /* fast device: it may not have completed yet even after an eject
         * that took a while to arrive; wait for it now, result unused. */
        WaitIO((struct IORequest *)st.rio);
        st.rio_pending = 0;
    }

    /* ---- step 5: post-eject checks ---- */

    for (i = 0; i < N_INT; i++) {
        if (chg_count[i] == 0) {
            out_printf("devsoak: removable: FAIL change interrupt %ld did not fire",
                       (LONG)i);
            st.failures++;
        }
    }

    if (have_etd) {
        out_printf("devsoak: removable: issuing stale-count ETD_READ, expect TDERR_DiskChanged");
        op_build_rw(st.rio, DIALECT_ETD, 0,
                    cfg.range_start * (U64)dev.sector_size,
                    st.readbuf, dev.sector_size, base_cn);
        op_do_sync(st.rio, SUBMIT_DOIO, &res);
        if (res.err == 0) {
            out_printf("devsoak: removable: FAIL stale ETD accepted with no disk");
            st.failures++;
        } else if (res.err != TDERR_DiskChanged) {
            out_printf("devsoak: removable: FAIL stale ETD returned err %ld, "
                       "expected TDERR_DiskChanged (%ld)",
                       (LONG)res.err, (LONG)TDERR_DiskChanged);
            st.failures++;
        }
    }

    op_simple(dev.io, TD_CHANGENUM, &res);
    cn = res.actual;
    if (cn <= base_cn) {
        out_printf("devsoak: removable: FAIL TD_CHANGENUM not monotonic after "
                   "eject (was %ld, now %ld)", (LONG)base_cn, (LONG)cn);
        st.failures++;
    }

    /* ---- step 6: request insert, poll for disk present ---- */

    for (i = 0; i < N_INT; i++)
        snap_a[i] = chg_count[i];

    request_transition(&st, "insert");

    if (poll_for_disk(&st, 1) != 0) {
        out_printf("devsoak: removable: FAIL no insert seen within 60 s");
        st.failures++;
        teardown(&st);
        return RC_ERROR;
    }
    out_printf("devsoak: removable: insert detected");

    /* ---- step 7: post-insert checks ---- */

    for (i = 0; i < N_INT; i++) {
        if (chg_count[i] <= snap_a[i]) {
            out_printf("devsoak: removable: FAIL change interrupt %ld did not "
                       "fire on insert", (LONG)i);
            st.failures++;
        }
    }

    op_simple(dev.io, TD_CHANGENUM, &res);
    cn = res.actual;
    if (cn < base_cn + 2UL) {
        out_printf("devsoak: removable: FAIL TD_CHANGENUM not monotonic across "
                   "full cycle (baseline %ld, now %ld, expected >= %ld)",
                   (LONG)base_cn, (LONG)cn, (LONG)(base_cn + 2UL));
        st.failures++;
    }

    /* ---- step 8: TD_REMCHANGEINT for all three ---- */

    out_printf("devsoak: removable: issuing TD_REMCHANGEINT for all three interrupts");
    for (i = 0; i < N_INT; i++) {
        if (st.created[i] && st.sent[i] && !st.completed[i] && !st.remdone[i])
            remchangeint_one(&st, i);
    }

    /* ---- step 9: prove delivery has stopped ---- */

    for (i = 0; i < N_INT; i++)
        snap_b[i] = chg_count[i];

    request_transition(&st, "eject");

    if (poll_for_disk(&st, 0) != 0) {
        out_printf("devsoak: removable: FAIL no eject (post-REMCHANGEINT) "
                   "seen within 60 s");
        st.failures++;
        teardown(&st);
        return RC_ERROR;
    }

    for (i = 0; i < N_INT; i++) {
        if (chg_count[i] != snap_b[i]) {
            out_printf("devsoak: removable: FAIL change interrupt %ld fired "
                       "after TD_REMCHANGEINT", (LONG)i);
            st.failures++;
        }
    }

    request_transition(&st, "insert");

    if (poll_for_disk(&st, 1) != 0) {
        out_printf("devsoak: removable: FAIL no insert (post-REMCHANGEINT) "
                   "seen within 60 s");
        st.failures++;
        teardown(&st);
        return RC_ERROR;
    }

    /* ---- step 10: refresh globals, cleanup ---- */

    op_simple(dev.io, TD_CHANGENUM, &res);
    g_changenum = res.actual;

    if (g_generation != NULL)
        memset(g_generation, 0, (ULONG)cfg.range_len * sizeof(UWORD));

    teardown(&st);

    rc = (st.failures != 0) ? RC_ERROR : RC_CLEAN;
    out_printf("devsoak: removable: phase complete, %ld failure(s)",
               (LONG)st.failures);
    return rc;
}
