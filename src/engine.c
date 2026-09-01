/*
 * engine.c - devsoak M3 concurrent soak orchestrator (implementation brief
 * S6, S9).
 *
 * M3 scope: setup()/fill_pass()/cleanup() are carried over from M2 almost
 * unchanged -- they run single-threaded, sequentially, on dev.io, before
 * any worker or the auditor exists, exactly like M2. Everything downstream
 * of the initial audit is new: engine_run() now starts the stripe locks,
 * the shared stats block, the worker pool and the auditor task, then
 * drives a status/watchdog loop until the run's duration elapses, Ctrl-C
 * is seen, or the workload itself decides to stop (stoponerror, or every
 * worker having died on an error). M2's verify_sweep/soak_loop/
 * diagnose_mismatch are gone: audit.c's audit_sweep() replaces
 * verify_sweep() for both the initial and final audits, worker.c owns the
 * concurrent mix and its own per-op diagnosis, and there is no longer a
 * single-task soak loop to diagnose from here.
 *
 * All output goes through out_printf() (S9.1): %ld/%lx/%lu/%s only, every
 * argument cast to LONG/ULONG. RawDoFmt has no 64-bit support, so every
 * U64 value is rendered with the local u64_to_str() helper below and
 * passed through as %s.
 */

#include "devsoak.h"

#include <proto/exec.h>
#include <exec/memory.h>
#include <exec/errors.h>    /* IOERR_NOCMD */
#include <dos/dos.h>        /* SIGBREAKF_CTRL_C */

/* ---- globals declared extern in devsoak.h, defined here (S6/S13) ---- */

UWORD *g_generation;
ULONG  g_changenum;
ULONG  g_enabled_dialects[DIALECT_COUNT];
ULONG  g_n_enabled;
ULONG  g_chunk_bytes;

/* ---- module state: setup()/fill_pass()/cleanup() only run single-task,
 * before stripes/workers/the auditor exist, so file statics are fine for
 * them -- exactly the M2 rule, just narrowed to this smaller set now that
 * verify_sweep/soak_loop (and their rbuf/r2buf/rng) are gone. */

static struct TestBuf wbuf;
static ULONG  range_len_u32;
static ULONG  sector_size;
static ULONG  chunk_sectors;
static UBYTE  have_etd;
static UBYTE  ring_ready;

/* ---- forward decls ---- */

static void  u64_to_str(U64 v, char *buf);
static UBYTE nsd_has_cmd(UWORD cmd);
static const char *dialect_name(ULONG d);
static void  log_ring(UWORD worker, UWORD cmd, U64 byteoff, ULONG length,
                      APTR data, const struct Result *res);
static void  report_io_failure(const char *what, UWORD cmd, U64 byteoff,
                               ULONG len, LONG err, ULONG actual);
static void  print_status_line(const struct StatsSnap *snap, ULONG ops_delta,
                               ULONG bytes_delta_mb_x1000, ULONG elapsed_s);
static LONG  setup(void);
static void  cleanup(void);
static LONG  fill_pass(void);

/* ---- small helpers ---- */

/* RawDoFmt (via out_printf) has no 64-bit support; render decimal by hand. */
static void
u64_to_str(U64 v, char *buf)
{
    char tmp[24];
    int  i = 0;
    int  j;

    if (v == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    while (v > 0) {
        tmp[i++] = (char)('0' + (int)(v % 10ULL));
        v /= 10ULL;
    }
    for (j = 0; j < i; j++)
        buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
}

static UBYTE
nsd_has_cmd(UWORD cmd)
{
    ULONG i;

    for (i = 0; i < dev.nsd_ncmds; i++) {
        if (dev.nsd_cmds[i] == cmd)
            return 1;
    }
    return 0;
}

static const char *
dialect_name(ULONG d)
{
    switch (d) {
    case DIALECT_CMD:      return "CMD";
    case DIALECT_ETD:      return "ETD";
    case DIALECT_TD64:     return "TD64";
    case DIALECT_NSD64:    return "NSD64";
    case DIALECT_NSDETD64: return "NSDETD64";
    default:               return "?";
    }
}

static void
log_ring(UWORD worker, UWORD cmd, U64 byteoff, ULONG length, APTR data,
         const struct Result *res)
{
    struct RingEntry e;

    timer_gettime(&e.secs, &e.micros);
    e.worker = worker;
    e.cmd    = cmd;
    e.off_hi = (ULONG)(byteoff >> 32);
    e.off_lo = (ULONG)(byteoff & 0xFFFFFFFFULL);
    e.length = length;
    e.data   = data;
    e.err    = res->err;
    e.actual = res->actual;
    ring_log(&e);
}

static void
report_io_failure(const char *what, UWORD cmd, U64 byteoff, ULONG len,
                   LONG err, ULONG actual)
{
    char offbuf[24];

    u64_to_str(byteoff, offbuf);
    out_printf("devsoak: %s FAILED cmd %s offset %s len %ld err %ld actual %ld",
                what, op_cmd_name(cmd), offbuf, (LONG)len, (LONG)err,
                (LONG)actual);
}

/* elapsed, ops/s since last status, MB/s (x1000 for one decimal without
 * float), in-flight, errors, then a second line of p50/p99 per class. */
static void
print_status_line(const struct StatsSnap *snap, ULONG ops_delta,
                   ULONG bytes_delta_mb_x1000, ULONG elapsed_s)
{
    out_printf("devsoak: status: %ld s, ops/s %ld, MB/s %ld.%ld, inflight %ld, errors %ld",
                (LONG)elapsed_s, (LONG)ops_delta,
                (LONG)(bytes_delta_mb_x1000 / 1000UL),
                (LONG)(bytes_delta_mb_x1000 % 1000UL),
                (LONG)workers_inflight(), (LONG)snap->errors);
    out_printf("devsoak: lat us p50/p99: rd %ld/%ld wr %ld/%ld hk %ld/%ld",
                (LONG)snap->p50_usec[CLASS_READ], (LONG)snap->p99_usec[CLASS_READ],
                (LONG)snap->p50_usec[CLASS_WRITE], (LONG)snap->p99_usec[CLASS_WRITE],
                (LONG)snap->p50_usec[CLASS_HK], (LONG)snap->p99_usec[CLASS_HK]);
}

/* ---- setup / cleanup ---- */

static LONG
setup(void)
{
    ULONG bytes;
    ULONG try_bytes;
    struct Result res;

    sector_size = dev.sector_size;

    if (cfg.range_len > (U64)(0xFFFFFFFFUL / sizeof(UWORD))) {
        out_printf("devsoak: range too large for the generation table");
        return RC_FATAL;
    }
    range_len_u32 = (ULONG)cfg.range_len;
    bytes = range_len_u32 * (ULONG)sizeof(UWORD);

    g_generation = AllocMem(bytes, MEMF_PUBLIC | MEMF_CLEAR);
    if (g_generation == NULL) {
        out_printf("devsoak: AllocMem(%ld) failed for the generation table",
                    (LONG)bytes);
        return RC_FATAL;
    }

    if (ring_init(4096) != 0) {
        out_printf("devsoak: ring_init failed");
        return RC_FATAL;
    }
    ring_ready = 1;

    /* disk-change count, once, for ETD dialects */
    op_simple(dev.io, TD_CHANGENUM, &res);
    if (res.err == 0) {
        have_etd = 1;
        g_changenum = res.actual;
    } else {
        have_etd = 0;
        g_changenum = 0;
        out_printf("devsoak: TD_CHANGENUM unavailable, ETD dialects disabled");
    }

    /* chunk size: cfg.maxxfer rounded down to a sector multiple, halving
     * (and re-rounding) on allocation failure, down to one sector. Only
     * one buffer now (wbuf): M2's rbuf/r2buf belonged to verify_sweep and
     * the M2 diagnoser, both removed -- audit.c allocates its own private
     * buffer per sweep, and workers/the invariant task own theirs. */
    try_bytes = cfg.maxxfer - (cfg.maxxfer % sector_size);
    if (try_bytes < sector_size)
        try_bytes = sector_size;

    for (;;) {
        if (buf_alloc(&wbuf, try_bytes, ALIGN_LONG, MEMF_PUBLIC) == 0) {
            g_chunk_bytes = try_bytes;
            break;
        }

        if (try_bytes <= sector_size) {
            out_printf("devsoak: could not allocate I/O buffers, not even one sector");
            return RC_FATAL;
        }
        try_bytes /= 2;
        try_bytes -= (try_bytes % sector_size);
        if (try_bytes < sector_size)
            try_bytes = sector_size;
    }
    chunk_sectors = g_chunk_bytes / sector_size;
    out_printf("devsoak: chunk size %ld sectors (%ld bytes)",
                (LONG)chunk_sectors, (LONG)g_chunk_bytes);

    /* usable dialect set */
    /* Candidate dialects, then a 1-sector read probe of each: the NSD
     * command list is advisory (and absent pre-NSD), and a dialect can be
     * unimplemented even when its prerequisites look present -- e.g.
     * scsi.device 47.4 answers TD_CHANGENUM but NOCMDs ETD_READ/WRITE.
     * The probe is authoritative here; a disagreement with the NSD list
     * is reported (the full listed<->implemented cross-check is the M4
     * invariant matrix). Probing reads into wbuf -- fine, fill_pass()
     * hasn't put anything worth keeping in it yet. */
    g_n_enabled = 0;
    g_enabled_dialects[g_n_enabled++] = DIALECT_CMD;
    {
        struct {
            ULONG dialect;
            UBYTE candidate;
            UBYTE listed;
        } cand[4];
        ULONG c;

        cand[0].dialect = DIALECT_ETD;
        cand[0].listed = dev.have_nsd && nsd_has_cmd(ETD_READ);
        cand[0].candidate = have_etd;    /* probe decides */
        cand[1].dialect = DIALECT_TD64;
        cand[1].listed = dev.have_nsd && nsd_has_cmd(TD_READ64);
        cand[1].candidate = cand[1].listed;
        cand[2].dialect = DIALECT_NSD64;
        cand[2].listed = dev.have_nsd && nsd_has_cmd(NSCMD_TD_READ64);
        cand[2].candidate = cand[2].listed;
        cand[3].dialect = DIALECT_NSDETD64;
        cand[3].listed = have_etd && dev.have_nsd &&
                         nsd_has_cmd(NSCMD_ETD_READ64);
        cand[3].candidate = cand[3].listed;

        for (c = 0; c < 4; c++) {
            if (!cand[c].candidate)
                continue;
            op_build_rw(dev.io, cand[c].dialect, 0,
                        cfg.range_start * (U64)sector_size,
                        wbuf.data, sector_size, g_changenum);
            op_do_sync(dev.io, SUBMIT_DOIO, &res);
            if (res.err == IOERR_NOCMD) {
                if (cand[c].listed) {
                    out_printf("devsoak: warning: %s listed by "
                               "NSCMD_DEVICEQUERY but returns IOERR_NOCMD",
                               dialect_name(cand[c].dialect));
                }
                continue;
            }
            if (res.err != 0) {
                out_printf("devsoak: warning: %s probe failed err %ld, "
                           "dialect disabled",
                           dialect_name(cand[c].dialect), (LONG)res.err);
                continue;
            }
            g_enabled_dialects[g_n_enabled++] = cand[c].dialect;
        }
    }

    {
        ULONG i;
        for (i = 0; i < g_n_enabled; i++)
            out_printf("devsoak: dialect enabled: %s",
                       dialect_name(g_enabled_dialects[i]));
    }

    return 0;
}

static void
cleanup(void)
{
    if (wbuf.base != NULL)
        buf_free(&wbuf);

    if (g_generation != NULL) {
        FreeMem(g_generation, range_len_u32 * (ULONG)sizeof(UWORD));
        g_generation = NULL;
    }

    if (ring_ready) {
        ring_cleanup();
        ring_ready = 0;
    }
}

/* ---- fill pass (S5/S9 step 2): unchanged mechanics from M2 -- main task,
 * sequential, on dev.io, no stripe locks (nothing else touches the range
 * yet). ---- */

static LONG
fill_pass(void)
{
    U64   cur = cfg.range_start;
    U64   remaining = cfg.range_len;
    ULONG done = 0;
    ULONG next_pct = 25;
    struct Result res;

    out_printf("devsoak: fill: writing generation 1 over the range");

    while (remaining > 0) {
        ULONG piece_sectors = (remaining > (U64)chunk_sectors)
                               ? chunk_sectors : (ULONG)remaining;
        ULONG piece_bytes = piece_sectors * sector_size;
        U64   byteoff = cur * (U64)sector_size;
        ULONG i;

        for (i = 0; i < piece_sectors; i++) {
            content_build(wbuf.data + (ULONG)i * sector_size, cur + i, 1, 0,
                          piece_bytes);
        }

        op_build_rw(dev.io, DIALECT_CMD, 1, byteoff, wbuf.data, piece_bytes,
                    g_changenum);
        op_do_sync(dev.io, SUBMIT_DOIO, &res);
        log_ring(0, dev.io->iotd_Req.io_Command, byteoff, piece_bytes,
                 wbuf.data, &res);

        if (res.err != 0 || res.actual != piece_bytes) {
            report_io_failure("fill", dev.io->iotd_Req.io_Command, byteoff,
                               piece_bytes, res.err, res.actual);
            return RC_ERROR;
        }

        for (i = 0; i < piece_sectors; i++)
            g_generation[(ULONG)(cur - cfg.range_start) + i] = 1;

        cur += piece_sectors;
        remaining -= piece_sectors;
        done += piece_sectors;

        {
            U64 pct = ((U64)done * 100ULL) / (U64)range_len_u32;
            if (next_pct <= 100 && pct >= (U64)next_pct) {
                out_printf("devsoak: fill: %ld%%", (LONG)next_pct);
                while (next_pct <= 100 && pct >= (U64)next_pct)
                    next_pct += 25;
            }
        }

        if ((SetSignal(0, 0) & SIGBREAKF_CTRL_C) != 0) {
            SetSignal(0, SIGBREAKF_CTRL_C);
            out_printf("devsoak: fill: aborted by user (incomplete)");
            return RC_WARN;
        }
    }

    out_printf("devsoak: fill: complete (%ld sectors)", (LONG)done);
    return 0;
}

/* ---- entry point (M3) ---- */

LONG
engine_run(void)
{
    LONG  rc;
    UBYTE stripes_ready = 0;
    UBYTE ctrlc_break = 0;

    wbuf.base = NULL;
    g_generation = NULL;
    ring_ready = 0;

    rc = setup();
    if (rc != 0)
        goto done_minimal;

    if (stripes_init() != 0) {
        out_printf("devsoak: stripes_init failed");
        rc = RC_FATAL;
        goto done_minimal;
    }
    stripes_ready = 1;

    stats_init();

    rc = fill_pass();
    if (rc != 0)
        goto done_stripes;

    rc = audit_sweep("initial audit");
    out_drain();
    if (rc != RC_CLEAN)
        goto done_stripes;

    if (workers_start() != 0) {
        out_printf("devsoak: workers_start failed");
        rc = RC_FATAL;
        goto done_stripes;
    }

    /* auditor_start() intentionally not checked here: cfg.audit_min == 0
     * makes it a deliberate no-op (0), and a spawn failure just means
     * start/end audits only for this run -- already reported by
     * auditor_start() itself, not a reason to abort a soak that is
     * otherwise ready to go. */
    auditor_start();

    /* §8 matrix task; a spawn failure degrades the run (no matrix
     * coverage) but is not fatal -- it reports itself. */
    invariant_start();

    /* ---- main loop: status/watchdog until duration elapses, Ctrl-C, or
     * the workload stops itself (stoponerror / all workers dead). ---- */
    {
        ULONG loop_s, loop_u;
        ULONG mark_s, mark_u;
        struct StatsSnap prev, snap;
        ULONG prev_total_ops;
        U64   prev_total_bytes;

        timer_gettime(&loop_s, &loop_u);
        mark_s = loop_s;
        mark_u = loop_u;

        stats_snapshot(&prev);
        prev_total_ops = prev.ops[CLASS_READ] + prev.ops[CLASS_WRITE]
                       + prev.ops[CLASS_HK];
        prev_total_bytes = prev.bytes[CLASS_READ] + prev.bytes[CLASS_WRITE]
                          + prev.bytes[CLASS_HK];

        out_printf("devsoak: soak: running for %ld s", (LONG)cfg.duration_s);

        for (;;) {
            ULONG age, wid, wcmd;

            out_drain();
            timer_delay_ms(250);

            if ((SetSignal(0, 0) & SIGBREAKF_CTRL_C) != 0) {
                SetSignal(0, SIGBREAKF_CTRL_C);
                out_printf("devsoak: break: finishing with final audit");
                ctrlc_break = 1;
                break;
            }

            if (timer_elapsed_ms(loop_s, loop_u) >= cfg.duration_s * 1000UL)
                break;

            age = workers_oldest_secs(&wid, &wcmd);
            if (age > cfg.watchdog_s) {
                out_printf("devsoak: WATCHDOG: request outstanding %lu s: "
                           "worker %lu cmd %s(0x%lx)",
                           (ULONG)age, (ULONG)wid, op_cmd_name((UWORD)wcmd),
                           (ULONG)wcmd);
                out_printf("devsoak: WATCHDOG: a stuck request cannot be "
                           "safely torn down -- worker/auditor tasks and "
                           "their I/O buffers are being leaked deliberately "
                           "rather than risking a free of memory a hung "
                           "request might still touch; see the final "
                           "report for the residual risk this leaves in "
                           "main.c's CloseDevice() on exit");
                out_drain();
                ring_dump(4096);
                out_printf("devsoak: RESULT FAIL rc=%ld", (LONG)RC_ERROR);
                return RC_ERROR;   /* deliberately skip all further
                                      cleanup -- see the comment above and
                                      the final report */
            }

            if (cfg.stoponerror) {
                struct StatsSnap errsnap;
                stats_snapshot(&errsnap);
                if (errsnap.errors != 0) {
                    out_printf("devsoak: stop on first error (-e): %ld error(s)",
                                (LONG)errsnap.errors);
                    break;
                }
            }

            if (workers_dead() == cfg.workers) {
                out_printf("devsoak: all %ld workers have stopped on errors",
                            (LONG)cfg.workers);
                break;
            }

            if (timer_elapsed_ms(mark_s, mark_u) >= 10000UL) {
                ULONG elapsed_s = timer_elapsed_ms(loop_s, loop_u) / 1000UL;
                ULONG period_ms = timer_elapsed_ms(mark_s, mark_u);
                ULONG total_ops, ops_delta;
                U64   total_bytes, bytes_delta;
                ULONG mb_x1000;

                stats_snapshot(&snap);
                total_ops = snap.ops[CLASS_READ] + snap.ops[CLASS_WRITE]
                          + snap.ops[CLASS_HK];
                total_bytes = snap.bytes[CLASS_READ] + snap.bytes[CLASS_WRITE]
                            + snap.bytes[CLASS_HK];

                ops_delta = total_ops - prev_total_ops;
                bytes_delta = total_bytes - prev_total_bytes;
                if (period_ms == 0)
                    period_ms = 1;
                /* ops/s: ops_delta and period_ms are both small (10 s
                 * cadence), so this never overflows ULONG. */
                ops_delta = (ops_delta * 1000UL) / period_ms;
                /* bytes_delta fits comfortably in a ULONG for any
                 * realistic 10 s window (workers*qdepth*maxxfer is far
                 * below 4 GB/10s); downcast is safe here even though the
                 * running byte totals themselves are U64. */
                mb_x1000 = (ULONG)((bytes_delta * 1000ULL) / 1048576ULL);
                mb_x1000 = (mb_x1000 * 1000UL) / period_ms;

                print_status_line(&snap, ops_delta, mb_x1000, elapsed_s);

                prev_total_ops = total_ops;
                prev_total_bytes = total_bytes;
                mark_s = 0; mark_u = 0;
                timer_gettime(&mark_s, &mark_u);
            }
        }
    }

    /* ---- shutdown ---- */
    {
        ULONG dead;
        LONG  final_audit_rc;
        struct StatsSnap fsnap;

        invariant_request_stop();
        auditor_request_stop();
        workers_request_stop();
        workers_wait_done();
        invariant_wait_done();
        auditor_wait_done();
        out_drain();

        dead = workers_dead();
        if (dead > 0)
            ring_dump(4096);

        final_audit_rc = audit_sweep("final audit");
        out_drain();

        workers_cleanup();
        invariant_cleanup();
        auditor_cleanup();

        out_printf("devsoak: matrix: %ld full passes, %ld failures",
                    (LONG)invariant_passes(), (LONG)invariant_errors());
        invariant_print_pins();

        stats_snapshot(&fsnap);

        if (fsnap.errors != 0 || dead != 0 || final_audit_rc != RC_CLEAN ||
            invariant_errors() != 0)
            rc = RC_ERROR;
        else if (ctrlc_break)
            rc = RC_WARN;
        else
            rc = RC_CLEAN;
    }

done_stripes:
    if (stripes_ready)
        stripes_cleanup();

done_minimal:
    cleanup();
    /* One machine-greppable verdict line: the guest exit code does not
     * reach the host when running under an emulator's serial log. */
    if (rc == RC_CLEAN)
        out_printf("devsoak: RESULT PASS");
    else if (rc == RC_WARN)
        out_printf("devsoak: RESULT WARN (run cut short) rc=%ld", (LONG)rc);
    else
        out_printf("devsoak: RESULT FAIL rc=%ld", (LONG)rc);
    return rc;
}
