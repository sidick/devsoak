/*
 * worker.c - devsoak concurrent soak workers (implementation brief S6/S7).
 *
 * N plain Exec tasks (amiga.lib CreateTask(), never a Process: workers
 * must not call dos.library, directly or indirectly -- all reporting goes
 * through out_task_printf()/ring_log(), never out_printf(), and there is
 * no DateStamp() call anywhere in this file). Each worker owns its own
 * MsgPort, a pool of cfg.qdepth IOExtTDs cloning dev.io's already-open
 * io_Device/io_Unit (the standard multi-request idiom -- no OpenDevice()
 * here, main owns the open/close), and one guarded buffer per slot,
 * rotating alignment/memory variants across slots (S7/S13).
 *
 * Concurrency story: a worker never blocks waiting for a stripe
 * (stripes_attempt() is non-blocking, all-or-nothing); on contention it
 * just repicks a different op. Stripes nest per-task, so a worker must
 * also check its own in-flight slot ranges before attempting stripes --
 * see devsoak.h's stripe.c comment. Everything that the watchdog (main)
 * or another task can read cross-task (struct WorkerSlot fields, the
 * stop/done/dead flags) is written under Forbid()/Permit().
 *
 * Task lifetime: CreateTask() can't pass arguments, so a worker's private
 * context is handed over with a short main<->child signal handshake
 * (see workers_start()/worker_entry()), one worker at a time (sequential
 * spawn -- a single set of handshake statics is enough, matching the
 * existing auditor_start()/auditor_entry() pattern in audit.c). A worker
 * ends by setting pub.done = 1 and returning: falling off the end of a
 * CreateTask() entry function is the documented safe way to end the task
 * (CreateTask()'s internal trampoline calls RemTask() for us and its
 * tc_MemEntry list frees the TCB/stack automatically) -- see audit.c's
 * auditor_entry() for the same pattern already relied on in this tree.
 *
 * Error handling (S9.5): a worker that hits a real driver bug (I/O error
 * outside expectations, a guard overrun, a content mismatch, an abort
 * that leaves mixed old/new generations) reports full diagnostics via
 * out_task_printf(), marks itself pub.dead, and (unless -e/cfg.stoponerror,
 * which also stops every other worker) simply stops starting new ops and
 * drains what is already in flight -- other workers are unaffected. It
 * does NOT call ring_dump(): like audit.c, ring_dump() is built on
 * out_printf(), which only main may call. engine.c is expected to call
 * ring_dump() itself after workers_wait_done() when workers_dead() > 0;
 * see the final report for this division of responsibility.
 */

#include "devsoak.h"

#include <proto/exec.h>
#include <proto/alib.h>   /* CreatePort/CreateExtIO/DeleteExtIO/DeletePort/
                              CreateTask/BeginIO */
#include <exec/memory.h>  /* MEMF_PUBLIC */
#include <exec/errors.h>  /* IOERR_NOCMD, IOERR_ABORTED */
#include <string.h>       /* memset */

extern struct ExecBase *SysBase;

/* ---- pick-attempt budget for a contended R/W op (S6/S7) ---- */
#define OP_PICK_ATTEMPTS  8

/* ---- worker task stack: diagnose_mismatch() keeps a sector-sized (up
 * to 4 KB) expected[] buffer on the stack, and a 68000 has no guard
 * pages, so give tasks that can run it a comfortable margin ---- */
#define WORKER_STACK  16384UL

/* ---- private per-worker context: struct WorkerCtx MUST be first ---- */

struct WorkerPriv {
    struct WorkerCtx pub;              /* public/shared part -- FIRST */

    struct MsgPort   *port;
    struct IOExtTD   *io[MAX_QDEPTH];
    struct TestBuf    buf[MAX_QDEPTH];

    /* per-slot bookkeeping not visible outside worker.c (WorkerSlot only
       carries what the watchdog needs) */
    U64    byteoff[MAX_QDEPTH];
    ULONG  bytes[MAX_QDEPTH];
    ULONG  submit_u[MAX_QDEPTH];       /* micros; submit_secs lives in
                                           pub.slots[].submit_secs already */
};

static struct WorkerPriv workers[MAX_WORKERS];
static ULONG g_num_workers;            /* clamped cfg.workers */
static ULONG g_qdepth;                 /* clamped cfg.qdepth */
static ULONG n_started;                /* workers actually spawned */

/* ---- housekeeping command set + "returns IOERR_NOCMD, stop trying"
 * mask. Shared across all workers (a NOCMD result is a fact about the
 * driver, not about which worker asked); a benign race disabling it
 * twice is harmless, so no lock is needed. */
static const UWORD hk_cmds[5] = {
    CMD_UPDATE, CMD_CLEAR, TD_CHANGENUM, TD_CHANGESTATE, TD_PROTSTATUS
};
static UBYTE hk_disabled[5];

/* ---- CreateTask() handshake statics (sequential spawn: one set is
 * enough -- same pattern as audit.c's auditor_start()/auditor_entry()) */
static volatile struct WorkerPriv *hs_spawn;
static struct Task   *hs_main_task;
static ULONG           hs_signum = (ULONG)-1;

/* ---- small helpers ---- */

/* RawDoFmt has no 64-bit support; render decimal by hand (every module
   that needs this keeps its own tiny copy -- see engine.c/audit.c). */
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

/* Mirrors ops.c's private elapsed_usec() -- that one is static to ops.c,
   so this file keeps its own copy for post-hoc (message-arrived) latency
   computation. */
static ULONG
elapsed_usec(ULONG s0, ULONG u0, ULONG s1, ULONG u1)
{
    LONG ds = (LONG)(s1 - s0);
    LONG du = (LONG)u1 - (LONG)u0;
    LONG total;

    if (du < 0) {
        du += 1000000L;
        ds -= 1;
    }
    if (ds < 0)
        return 0;

    total = ds * 1000000L + du;
    if (total < 0)
        total = 0;

    return (ULONG)total;
}

/* actual command number a dialect/direction pair will issue -- used to
   check quirk_cmd_skipped() against the real wire command, not just the
   dialect as a whole (asymmetric skips like "skip TD_WRITE64 only"). */
static UWORD
dialect_cmd(ULONG dialect, UBYTE is_write)
{
    switch (dialect) {
    case DIALECT_CMD:      return is_write ? CMD_WRITE : CMD_READ;
    case DIALECT_ETD:      return is_write ? ETD_WRITE : ETD_READ;
    case DIALECT_TD64:     return is_write ? TD_WRITE64 : TD_READ64;
    case DIALECT_NSD64:    return is_write ? NSCMD_TD_WRITE64
                                            : NSCMD_TD_READ64;
    case DIALECT_NSDETD64: return is_write ? NSCMD_ETD_WRITE64
                                            : NSCMD_ETD_READ64;
    default:               return CMD_READ;
    }
}

static void
report_failure(struct WorkerPriv *w, const char *what, UWORD cmd,
               U64 byteoff, ULONG len, LONG err, ULONG actual)
{
    char offbuf[24];

    u64_to_str(byteoff, offbuf);
    out_task_printf("devsoak: worker %ld: %s FAILED cmd %s offset %s len %ld "
                     "err %ld actual %ld",
                     (LONG)w->pub.id, what, op_cmd_name(cmd), offbuf,
                     (LONG)len, (LONG)err, (LONG)actual);
}

static void
mark_dead(struct WorkerPriv *w)
{
    w->pub.dead = 1;
    if (cfg.stoponerror) {
        ULONG i;
        for (i = 0; i < n_started; i++)
            workers[i].pub.stop = 1;
    }
}

/* ---- S9 mismatch diagnosis --------------------------------------------
 * Single-buffer simplification, exactly like audit.c's diagnose(): the
 * sector's stripe is still held by the caller, so nothing else can touch
 * it while we re-read into the SAME slot buffer (prefilled 0x5A first).
 * That means read1's raw payload bytes are gone by the time we print
 * (only its header survives, decoded before the re-read), so this is an
 * expected/read2 report, not the three-way expected/read1/read2 report
 * engine.c's M2-only diagnoser can afford with a dedicated third buffer.
 */
static void
diagnose_mismatch(struct WorkerPriv *w, ULONG qi, U64 chunk_byteoff,
                   ULONG chunk_len, U64 lba, ULONG idx_in_chunk,
                   ULONG expect_gen, LONG cvclass)
{
    UBYTE *sect1 = w->buf[qi].data + idx_in_chunk * dev.sector_size;
    UBYTE *sect2;
    struct SectorHdr hdr;
    struct Result res;
    ULONG  chk;
    ULONG  diffs = 0;
    ULONG  i;
    char   lbabuf[24], claimbuf[24], expbuf[24];
    /* dev.sector_size is validated <= 4096 at open time (main.c); see
       audit.c's identical stack-buffer note. */
    UBYTE  expected[4096];

    u64_to_str(lba, lbabuf);
    out_task_printf("devsoak: worker %ld: DATA MISMATCH sector %s class %s",
                     (LONG)w->pub.id, lbabuf, content_class_name(cvclass));

    content_decode_hdr(sect1, &hdr);
    chk = hdr.magic ^ hdr.seed ^ hdr.sector_hi ^ hdr.sector_lo
        ^ hdr.generation ^ hdr.writer ^ hdr.xfer_len;

    {
        U64 claimed_lba = ((U64)hdr.sector_hi << 32) | (U64)hdr.sector_lo;

        u64_to_str(claimed_lba, claimbuf);
        u64_to_str(lba, expbuf);
        out_task_printf("devsoak: worker %ld: hdr claims sector %s (expected %s)",
                         (LONG)w->pub.id, claimbuf, expbuf);
        out_task_printf("devsoak: worker %ld: hdr generation %ld (expected %ld), "
                         "writer %ld, xfer_len %ld",
                         (LONG)w->pub.id, (LONG)hdr.generation, (LONG)expect_gen,
                         (LONG)hdr.writer, (LONG)hdr.xfer_len);
        out_task_printf("devsoak: worker %ld: hdr checksum %s",
                         (LONG)w->pub.id, (chk == hdr.hdr_check) ? "ok" : "bad");
    }

    buf_prefill(&w->buf[qi], 0x5A);
    op_build_rw(w->io[qi], DIALECT_CMD, 0, chunk_byteoff, w->buf[qi].data,
                chunk_len, g_changenum);
    op_do_sync(w->io[qi], SUBMIT_DOIO, &res);
    {
        struct RingEntry e;

        timer_gettime(&e.secs, &e.micros);
        e.worker = (UWORD)w->pub.id;
        e.cmd    = w->io[qi]->iotd_Req.io_Command;
        e.off_hi = (ULONG)(chunk_byteoff >> 32);
        e.off_lo = (ULONG)(chunk_byteoff & 0xFFFFFFFFUL);
        e.length = chunk_len;
        e.data   = w->buf[qi].data;
        e.err    = res.err;
        e.actual = res.actual;
        ring_log(&e);
    }

    if (res.err != 0 || res.actual != chunk_len) {
        out_task_printf("devsoak: worker %ld: diagnostic re-read failed too, "
                         "err %ld actual %ld",
                         (LONG)w->pub.id, (LONG)res.err, (LONG)res.actual);
        return;
    }

    sect2 = w->buf[qi].data + idx_in_chunk * dev.sector_size;

    if (chk == hdr.hdr_check)
        content_build(expected, lba, expect_gen, hdr.writer, hdr.xfer_len);
    else
        content_build(expected, lba, expect_gen, 0, chunk_len);

    out_task_printf("devsoak: worker %ld: first differing bytes (expected/read2):",
                     (LONG)w->pub.id);
    for (i = 0; i < dev.sector_size && diffs < 16; i++) {
        if (expected[i] != sect2[i]) {
            out_task_printf("devsoak: worker %ld:  +0x%lx exp %02lx read2 %02lx",
                             (LONG)w->pub.id, (ULONG)i, (ULONG)expected[i],
                             (ULONG)sect2[i]);
            diffs++;
        }
    }
    if (diffs == 0) {
        out_task_printf("devsoak: worker %ld: re-read matches expected "
                         "(transient first read?)", (LONG)w->pub.id);
    }
}

/* ---- resource setup / teardown ---------------------------------------- */

/* Main frees ONLY the buffers: the MsgPort and IOExtTDs are created (and
 * on shutdown deleted) inside the worker task itself, because a MsgPort's
 * signal bit and mp_SigTask belong to the task that CreatePort()s it --
 * a port made by main would have completions Signal() main while the
 * worker sleeps forever in WaitPort() on a bit it never owned. */
static void
worker_free_resources(struct WorkerPriv *w)
{
    ULONG j;

    for (j = 0; j < g_qdepth; j++)
        buf_free(&w->buf[j]);
}

/* Buffers + bookkeeping only (no task-affine resources); called from the
 * calling (main) task before the task itself is spawned. Returns 0 on
 * success. */
static LONG
worker_init_resources(ULONG idx)
{
    struct WorkerPriv *w = &workers[idx];
    ULONG j;

    memset(w, 0, sizeof(*w));
    w->pub.id = idx + 1;
    w->pub.rng = XS32_SEED(cfg.seed ^ ((ULONG)(idx + 1) * 0x9E3779B9UL));

    for (j = 0; j < g_qdepth; j++) {
        ULONG alignsel = j % 4;
        ULONG minalign = quirk_min_align();
        LONG  bufrc;

        /* honour `align N`: never use a variant weaker than the floor.
           ALIGN_CROSS4K is longword-aligned already, so it is never
           remapped. */
        if (minalign >= 4) {
            if (alignsel == ALIGN_ODD || alignsel == ALIGN_WORD)
                alignsel = ALIGN_LONG;
        } else if (minalign >= 2) {
            if (alignsel == ALIGN_ODD)
                alignsel = ALIGN_LONG;
        }

        if (j == 1 && g_qdepth > 1 && dev.have_geom &&
            dev.geom.dg_BufMemType != 0 &&
            !(quirk_nochip() && (dev.geom.dg_BufMemType & MEMF_CHIP))) {
            ULONG special = dev.geom.dg_BufMemType | MEMF_PUBLIC;

            bufrc = buf_alloc(&w->buf[j], g_chunk_bytes, alignsel, special);
            if (bufrc != 0)
                bufrc = buf_alloc(&w->buf[j], g_chunk_bytes, alignsel,
                                  MEMF_PUBLIC);
        } else {
            bufrc = buf_alloc(&w->buf[j], g_chunk_bytes, alignsel,
                              MEMF_PUBLIC);
        }
        if (bufrc != 0)
            return 1;
    }

    return 0;
}

/* Worker-context half of resource setup: the port and the request pool.
 * Returns 0 on success; on failure everything partial is freed here (own
 * context) and the worker reports + dies before ever starting an op. */
static LONG
worker_make_port_ios(struct WorkerPriv *w)
{
    ULONG j;

    w->port = CreatePort(NULL, 0);
    if (w->port == NULL)
        return 1;

    for (j = 0; j < g_qdepth; j++) {
        w->io[j] = (struct IOExtTD *)CreateExtIO(w->port,
                                                 sizeof(struct IOExtTD));
        if (w->io[j] == NULL)
            goto fail;

        /* standard multi-request idiom: clone the already-open
           device/unit, never OpenDevice() again here */
        w->io[j]->iotd_Req.io_Device = dev.io->iotd_Req.io_Device;
        w->io[j]->iotd_Req.io_Unit   = dev.io->iotd_Req.io_Unit;
    }
    return 0;

fail:
    while (j > 0) {
        j--;
        DeleteExtIO((struct IORequest *)w->io[j]);
        w->io[j] = NULL;
    }
    DeletePort(w->port);
    w->port = NULL;
    return 1;
}

/* Worker-context teardown of what worker_make_port_ios() built; runs
 * after the in-flight drain, so every io is idle. */
static void
worker_delete_port_ios(struct WorkerPriv *w)
{
    ULONG j;

    for (j = 0; j < g_qdepth; j++) {
        if (w->io[j] != NULL) {
            DeleteExtIO((struct IORequest *)w->io[j]);
            w->io[j] = NULL;
        }
    }
    if (w->port != NULL) {
        DeletePort(w->port);
        w->port = NULL;
    }
}

/* ---- housekeeping op (S7) ---------------------------------------------- */

static void
do_housekeeping(struct WorkerPriv *w, ULONG qi)
{
    ULONG tries;
    ULONG pick = 2;             /* falls back to TD_CHANGENUM's index */
    UWORD cmd = TD_CHANGENUM;
    struct Result res;
    ULONG dummy_u;
    UBYTE any_enabled = 0;
    ULONG k;

    for (k = 0; k < 5; k++)
        if (!hk_disabled[k] && !quirk_cmd_skipped(hk_cmds[k]))
            any_enabled = 1;
    if (!any_enabled)
        return;   /* every hk command NOCMD'd/quirk-skipped -- nothing safe
                     left to send */

    for (tries = 0; tries < 5; tries++) {
        pick = xs32(&w->pub.rng) % 5;
        if (!hk_disabled[pick] && !quirk_cmd_skipped(hk_cmds[pick])) {
            cmd = hk_cmds[pick];
            break;
        }
    }
    if (hk_disabled[pick] || quirk_cmd_skipped(hk_cmds[pick]))
        return;   /* picked disabled/skipped 5 times running -- skip this
                     round */

    Forbid();
    w->pub.slots[qi].active = 1;
    w->pub.slots[qi].is_write = 0;
    w->pub.slots[qi].cmd = cmd;
    timer_gettime(&w->pub.slots[qi].submit_secs, &dummy_u);
    w->pub.slots[qi].start_idx = 0;
    w->pub.slots[qi].nsect = 0;
    Permit();

    op_simple(w->io[qi], cmd, &res);

    if (res.err == 0) {
        if (cmd == TD_CHANGENUM && res.actual < g_changenum) {
            report_failure(w, "TD_CHANGENUM went backwards", cmd, 0, 0,
                           (LONG)res.actual, res.actual);
            stats_record(CLASS_HK, 0, res.usec, -1);
            mark_dead(w);
        } else {
            if (cmd == TD_CHANGENUM && res.actual > g_changenum)
                g_changenum = res.actual;   /* advisory only: workers all
                                                read this, nobody but the
                                                engine writes it otherwise;
                                                a torn read/write race here
                                                only widens the "stale ETD"
                                                tolerance window slightly */
            stats_record(CLASS_HK, 0, res.usec, 0);
        }
    } else if (res.err == IOERR_NOCMD) {
        if (!hk_disabled[pick]) {
            hk_disabled[pick] = 1;
            out_task_printf("devsoak: worker %ld: %s returns IOERR_NOCMD, "
                             "disabling it for this run",
                             (LONG)w->pub.id, op_cmd_name(cmd));
        }
        stats_record(CLASS_HK, 0, res.usec, res.err);
    } else {
        report_failure(w, "housekeeping", cmd, 0, 0, res.err, res.actual);
        stats_record(CLASS_HK, 0, res.usec, res.err);
        mark_dead(w);
    }

    Forbid();
    w->pub.slots[qi].active = 0;
    Permit();
}

/* ---- length/addressing pick (S7) --------------------------------------- */

static ULONG
pick_nsect(struct WorkerPriv *w, ULONG cap)
{
    ULONG bucket = xs32(&w->pub.rng) % 4;
    ULONG n;

    if (cap < 1)
        cap = 1;

    switch (bucket) {
    case 0:
        n = 1;
        break;
    case 1:
        n = 2 + (xs32(&w->pub.rng) % 15);          /* 2..16 */
        break;
    case 2:
        n = 17 + (xs32(&w->pub.rng) % 112);        /* 17..128 */
        break;
    default:
        if (cap > 129)
            n = 129 + (xs32(&w->pub.rng) % (cap - 129 + 1)); /* 129..cap */
        else
            n = cap;
        break;
    }
    if (n > cap)
        n = cap;
    if (n < 1)
        n = 1;
    return n;
}

static ULONG
pick_start(struct WorkerPriv *w, ULONG nsect, ULONG range_len)
{
    ULONG span;

    if (nsect >= range_len)
        return 0;
    span = range_len - nsect;
    return xs32(&w->pub.rng) % (span + 1);
}

/* ---- abort-probe verification (S7 "AbortIO on a random in-flight
 * request roughly once per 1000 ops" bullet) ----------------------------
 * Called from process_completion() when the completed request's io_Error
 * is IOERR_ABORTED. For a write: the affected sectors must read back as
 * either fully old or fully new generation, never a mix. For a read:
 * nothing to check (buffer contents are undefined). */
static void
handle_abort(struct WorkerPriv *w, ULONG qi, UWORD cmd, U64 byteoff,
             ULONG bytes, ULONG start_idx, ULONG nsect, UBYTE is_write,
             const struct Result *res)
{
    if (is_write) {
        struct Result rres;
        LONG   state = -1;      /* -1 unknown, 0 = old, 1 = new */
        UBYTE  bad = 0;
        ULONG  k;

        buf_prefill(&w->buf[qi], 0xA5);
        op_build_rw(w->io[qi], DIALECT_CMD, 0, byteoff, w->buf[qi].data, bytes,
                    g_changenum);
        op_do_sync(w->io[qi], SUBMIT_DOIO, &rres);

        if (rres.err != 0 || rres.actual != bytes) {
            report_failure(w, "abort readback", cmd, byteoff, bytes, rres.err,
                           rres.actual);
            stats_record(CLASS_WRITE, 0, rres.usec, -1);
            mark_dead(w);
            bad = 1;
        }

        for (k = 0; !bad && k < nsect; k++) {
            ULONG gidx = start_idx + k;
            U64   lba = cfg.range_start + gidx;
            ULONG oldgen = g_generation[gidx];
            ULONG newgen = oldgen + 1;
            UBYTE *sect = w->buf[qi].data + k * dev.sector_size;
            ULONG diff;
            LONG  cv;
            LONG  this_state;

            cv = content_verify(sect, lba, newgen, &diff);
            if (cv == CV_OK) {
                this_state = 1;
            } else if (oldgen != 0 &&
                      content_verify(sect, lba, oldgen, &diff) == CV_OK) {
                this_state = 0;
            } else {
                diagnose_mismatch(w, qi, byteoff, bytes, lba, k, newgen, cv);
                stats_record(CLASS_WRITE, 0, rres.usec, -1);
                mark_dead(w);
                bad = 1;
                break;
            }

            if (state == -1) {
                state = this_state;
            } else if (state != this_state) {
                report_failure(w, "abort left mixed old/new generations",
                               cmd, byteoff, bytes, 0, 0);
                stats_record(CLASS_WRITE, 0, rres.usec, -1);
                mark_dead(w);
                bad = 1;
                break;
            }
        }

        if (!bad) {
            if (state == 1) {
                for (k = 0; k < nsect; k++)
                    g_generation[start_idx + k]++;
            }
            stats_record(CLASS_WRITE, bytes, res->usec, 0);
        }

        {
            struct RingEntry e;

            timer_gettime(&e.secs, &e.micros);
            e.worker = (UWORD)w->pub.id;
            e.cmd    = cmd;
            e.off_hi = (ULONG)(byteoff >> 32);
            e.off_lo = (ULONG)(byteoff & 0xFFFFFFFFUL);
            e.length = bytes;
            e.data   = w->buf[qi].data;
            e.err    = res->err;      /* the real IOERR_ABORTED, not 0 */
            e.actual = res->actual;
            ring_log(&e);
        }
    } else {
        struct RingEntry e;

        timer_gettime(&e.secs, &e.micros);
        e.worker = (UWORD)w->pub.id;
        e.cmd    = cmd;
        e.off_hi = (ULONG)(byteoff >> 32);
        e.off_lo = (ULONG)(byteoff & 0xFFFFFFFFUL);
        e.length = bytes;
        e.data   = w->buf[qi].data;
        e.err    = res->err;
        e.actual = res->actual;
        ring_log(&e);

        stats_record(CLASS_READ, bytes, res->usec, 0);
    }
}

/* ---- op completion ------------------------------------------------------ */

static void
log_ring(struct WorkerPriv *w, UWORD cmd, U64 byteoff, ULONG len,
         APTR data, const struct Result *res)
{
    struct RingEntry e;

    timer_gettime(&e.secs, &e.micros);
    e.worker = (UWORD)w->pub.id;
    e.cmd    = cmd;
    e.off_hi = (ULONG)(byteoff >> 32);
    e.off_lo = (ULONG)(byteoff & 0xFFFFFFFFUL);
    e.length = len;
    e.data   = data;
    e.err    = res->err;
    e.actual = res->actual;
    ring_log(&e);
}

static void
process_completion(struct WorkerPriv *w, ULONG qi)
{
    struct IOExtTD *io = w->io[qi];
    ULONG  start_idx = w->pub.slots[qi].start_idx;
    ULONG  nsect     = w->pub.slots[qi].nsect;
    UBYTE  is_write  = w->pub.slots[qi].is_write;
    UWORD  cmd       = w->pub.slots[qi].cmd;
    U64    byteoff   = w->byteoff[qi];
    ULONG  bytes     = w->bytes[qi];
    struct Result res;
    ULONG  s1, u1;

    timer_gettime(&s1, &u1);
    res.err    = (LONG)(BYTE)io->iotd_Req.io_Error;
    res.actual = io->iotd_Req.io_Actual;
    res.flags  = (ULONG)io->iotd_Req.io_Flags;
    res.usec   = elapsed_usec(w->pub.slots[qi].submit_secs, w->submit_u[qi],
                              s1, u1);

    {
        LONG e;

        if (res.err != 0 && quirk_expected_err(cmd, &e) && res.err == e) {
            /* The driver correctly refused this op: a benign no-op, not a
               bug -- don't touch g_generation, don't verify, don't
               report. */
            log_ring(w, cmd, byteoff, bytes, io->iotd_Req.io_Data, &res);
            stats_record(is_write ? CLASS_WRITE : CLASS_READ, 0, res.usec, 0);
            stripes_release(start_idx, nsect);
            Forbid();
            w->pub.slots[qi].active = 0;
            Permit();
            w->pub.inflight--;
            return;
        }
    }

    if (res.err == IOERR_ABORTED) {
        handle_abort(w, qi, cmd, byteoff, bytes, start_idx, nsect, is_write,
                    &res);
    } else if (is_write) {
        if (res.err == 0 && res.actual == bytes) {
            ULONG k;
            LONG  g;

            for (k = 0; k < nsect; k++)
                g_generation[start_idx + k]++;

            g = buf_check_guards(&w->buf[qi]);
            if (g != 0) {
                report_failure(w, "guard overrun after write", cmd, byteoff,
                               bytes, g, res.actual);
                stats_record(CLASS_WRITE, bytes, res.usec, -1);
                mark_dead(w);
            } else {
                stats_record(CLASS_WRITE, bytes, res.usec, 0);
            }
        } else {
            report_failure(w, "write", cmd, byteoff, bytes, res.err,
                           res.actual);
            stats_record(CLASS_WRITE, bytes, res.usec, res.err ? res.err : -1);
            mark_dead(w);
        }
        log_ring(w, cmd, byteoff, bytes, io->iotd_Req.io_Data, &res);
    } else {
        if (res.err == 0 && res.actual == bytes) {
            LONG  mismatch_cv = -1;
            U64   mismatch_lba = 0;
            ULONG mismatch_idx = 0;
            ULONG mismatch_gen = 0;
            ULONG k;

            for (k = 0; k < nsect; k++) {
                ULONG gidx = start_idx + k;
                ULONG gen = g_generation[gidx];
                ULONG diff;
                LONG  cv;

                if (gen == 0)
                    continue;   /* never written -- not verifiable yet */

                cv = content_verify(w->buf[qi].data + k * dev.sector_size,
                                    cfg.range_start + gidx, gen, &diff);
                if (cv != CV_OK) {
                    mismatch_cv = cv;
                    mismatch_lba = cfg.range_start + gidx;
                    mismatch_idx = k;
                    mismatch_gen = gen;
                    break;
                }
            }

            if (mismatch_cv >= 0) {
                diagnose_mismatch(w, qi, byteoff, bytes, mismatch_lba,
                                  mismatch_idx, mismatch_gen, mismatch_cv);
                stats_record(CLASS_READ, bytes, res.usec, -1);
                mark_dead(w);
            } else {
                LONG g = buf_check_guards(&w->buf[qi]);

                if (g != 0) {
                    report_failure(w, "guard overrun after read", cmd,
                                   byteoff, bytes, g, res.actual);
                    stats_record(CLASS_READ, bytes, res.usec, -1);
                    mark_dead(w);
                } else {
                    stats_record(CLASS_READ, bytes, res.usec, 0);
                }
            }
        } else {
            report_failure(w, "read", cmd, byteoff, bytes, res.err,
                           res.actual);
            stats_record(CLASS_READ, bytes, res.usec, res.err ? res.err : -1);
            mark_dead(w);
        }
        log_ring(w, cmd, byteoff, bytes, io->iotd_Req.io_Data, &res);
    }

    /* release before stopping (S9.5): always release the stripe(s) and
       free the slot, even on a fatal mismatch -- mark_dead() only sets
       flags, it never leaves anything held. */
    stripes_release(start_idx, nsect);
    Forbid();
    w->pub.slots[qi].active = 0;
    Permit();
    w->pub.inflight--;
}

static void
reap_all(struct WorkerPriv *w)
{
    struct Message *msg;
    ULONG j;

    while ((msg = GetMsg(w->port)) != NULL) {
        for (j = 0; j < g_qdepth; j++) {
            if ((struct Message *)w->io[j] == msg) {
                process_completion(w, j);
                break;
            }
        }
    }
}

/* ---- starting one op (S7) ----------------------------------------------- */

static LONG
start_one_op(struct WorkerPriv *w)
{
    ULONG qi = g_qdepth;
    ULONG j;
    ULONG kind;

    for (j = 0; j < g_qdepth; j++) {
        if (!w->pub.slots[j].active) {
            qi = j;
            break;
        }
    }
    if (qi == g_qdepth)
        return 0;   /* caller only gets here when inflight < qdepth */

    kind = xs32(&w->pub.rng) % 100;

    if (kind >= 90) {
        do_housekeeping(w, qi);
        return 1;
    }

    {
        UBYTE  is_write = (kind < 45) ? 1 : 0;
        ULONG  range_len = (ULONG)cfg.range_len;
        ULONG  chunk_sectors = g_chunk_bytes / dev.sector_size;
        ULONG  cap = (chunk_sectors < range_len) ? chunk_sectors : range_len;
        ULONG  nsect = 1, start_idx = 0;
        ULONG  attempts;
        LONG   got = 0;

        for (attempts = 0; attempts < OP_PICK_ATTEMPTS; attempts++) {
            UBYTE overlap = 0;
            ULONG k;

            nsect = pick_nsect(w, cap);
            start_idx = pick_start(w, nsect, range_len);

            for (k = 0; k < g_qdepth; k++) {
                struct WorkerSlot *s = &w->pub.slots[k];

                if (s->active && s->nsect > 0 &&
                    start_idx < s->start_idx + s->nsect &&
                    s->start_idx < start_idx + nsect) {
                    overlap = 1;
                    break;
                }
            }
            if (overlap)
                continue;

            if (stripes_attempt(start_idx, nsect)) {
                got = 1;
                break;
            }
        }
        if (!got)
            return 0;

        {
            U64   byteoff = ((U64)cfg.range_start + (U64)start_idx) *
                           (U64)dev.sector_size;
            ULONG bytes = nsect * dev.sector_size;
            ULONG dsel, dialect;
            UBYTE is32;

            dsel = xs32(&w->pub.rng) % g_n_enabled;
            dialect = g_enabled_dialects[dsel];
            is32 = (dialect == DIALECT_CMD || dialect == DIALECT_ETD) ? 1 : 0;

            if (is32 && (byteoff + (U64)bytes) > 0x100000000ULL) {
                ULONG d2;
                UBYTE found = 0;

                for (d2 = 0; d2 < g_n_enabled; d2++) {
                    ULONG cand = g_enabled_dialects[d2];

                    if (cand == DIALECT_TD64 || cand == DIALECT_NSD64 ||
                        cand == DIALECT_NSDETD64) {
                        dialect = cand;
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    stripes_release(start_idx, nsect);
                    return 0;
                }
            }

            /* Belt-and-braces: engine.c already excludes skipped dialects
               from g_enabled_dialects at probe time, but that check is
               keyed on the READ command per dialect (S16.4/engine.c
               setup()), so an asymmetric skip (e.g. "skip TD_WRITE64
               only") would still let a write through here. Re-pick a
               dialect that isn't skipped for THIS op's actual command. */
            if (quirk_cmd_skipped(dialect_cmd(dialect, is_write))) {
                ULONG d3;
                UBYTE found2 = 0;

                for (d3 = 0; d3 < g_n_enabled; d3++) {
                    ULONG cand2 = g_enabled_dialects[d3];
                    UBYTE cand_is32 = (cand2 == DIALECT_CMD ||
                                       cand2 == DIALECT_ETD) ? 1 : 0;

                    if (cand_is32 &&
                        (byteoff + (U64)bytes) > 0x100000000ULL)
                        continue;
                    if (quirk_cmd_skipped(dialect_cmd(cand2, is_write)))
                        continue;
                    dialect = cand2;
                    found2 = 1;
                    break;
                }
                if (!found2) {
                    stripes_release(start_idx, nsect);
                    return 0;
                }
            }

            {
                ULONG k;
                UBYTE *base = w->buf[qi].data;

                if (is_write) {
                    for (k = 0; k < nsect; k++) {
                        U64   lba = cfg.range_start + start_idx + k;
                        ULONG curgen = g_generation[start_idx + k];

                        content_build(base + k * dev.sector_size, lba,
                                     curgen + 1, w->pub.id, bytes);
                    }
                } else {
                    buf_prefill(&w->buf[qi], 0xA5);
                }

                op_build_rw(w->io[qi], dialect, is_write, byteoff, base,
                           bytes, g_changenum);

                Forbid();
                w->pub.slots[qi].active = 1;
                w->pub.slots[qi].is_write = is_write;
                w->pub.slots[qi].cmd = w->io[qi]->iotd_Req.io_Command;
                timer_gettime(&w->pub.slots[qi].submit_secs, &w->submit_u[qi]);
                w->pub.slots[qi].start_idx = start_idx;
                w->pub.slots[qi].nsect = nsect;
                Permit();

                w->byteoff[qi] = byteoff;
                w->bytes[qi] = bytes;

                /* Simplification vs. the full S7 submission mix: every
                   async op here goes via SendIO -- the plain-DoIO and
                   IOF_QUICK submission styles are exercised by the S8
                   invariant matrix (M4) instead. Batching q SendIO()s
                   before draining is exactly what filling the queue up
                   to qdepth before WaitPort()ing already does. See the
                   final report. */
                SendIO((struct IORequest *)w->io[qi]);
                w->pub.inflight++;

                /* ~1/1000 ops: AbortIO() probe on a random ACTIVE slot */
                if ((xs32(&w->pub.rng) % 1024) == 0) {
                    ULONG cnt = 0, pick, m;

                    for (m = 0; m < g_qdepth; m++)
                        if (w->pub.slots[m].active)
                            cnt++;
                    if (cnt > 0) {
                        pick = xs32(&w->pub.rng) % cnt;
                        for (m = 0; m < g_qdepth; m++) {
                            if (w->pub.slots[m].active) {
                                if (pick == 0) {
                                    AbortIO((struct IORequest *)w->io[m]);
                                    break;
                                }
                                pick--;
                            }
                        }
                    }
                }
            }
        }
    }

    return 1;
}

/* ---- worker main loop ---------------------------------------------------- */

static void
worker_run(struct WorkerPriv *w)
{
    for (;;) {
        if (w->pub.stop || w->pub.dead)
            break;

        while (w->pub.inflight < g_qdepth && !w->pub.stop && !w->pub.dead) {
            if (!start_one_op(w))
                break;   /* contended or nothing to do -- don't spin */
        }

        if (w->pub.inflight > 0) {
            WaitPort(w->port);
            reap_all(w);
        } else if (!w->pub.stop && !w->pub.dead) {
            /* Nothing in flight and nothing startable (stripe
               contention with zero pending ops should be rare, since
               every repick tries a fresh location): yield to any other
               task at the same priority without a real sleep -- workers
               are plain Exec tasks and have no safe way to Delay()
               (timer_delay_ms() is main-task-only per timer.c, and
               there is no Process to call dos.library Delay() from).
               Forbid()/Permit() with nothing in between forces a
               reschedule point on return without actually blocking. */
            Forbid();
            Permit();
        }
    }

    /* stop/dead: stop starting new ops, drain what's already in flight
       (reap only -- stripes_release()/slot-clear already happens inside
       process_completion()). */
    while (w->pub.inflight > 0) {
        WaitPort(w->port);
        reap_all(w);
    }
    /* pub.done is set by worker_entry() after the port/request teardown;
       falling off the end of the entry is the documented-safe way to end
       a CreateTask() task (internal trampoline RemTask()s us). */
}

static void
worker_entry(void)
{
    struct WorkerPriv *w = (struct WorkerPriv *)hs_spawn;
    struct Task       *maintask = hs_main_task;
    ULONG              signum = hs_signum;

    /* Port + request pool MUST be created here, in the worker's own
       context, so mp_SigTask/mp_SigBit belong to this task (see
       worker_free_resources()'s comment). Then tell main -- which checks
       pub.dead after the handshake -- whether we are viable. */
    if (worker_make_port_ios(w) != 0) {
        out_task_printf("devsoak: worker %ld: port/request allocation failed",
                        (LONG)w->pub.id);
        w->pub.dead = 1;
        w->pub.done = 1;
        Signal(maintask, 1UL << signum);
        return;
    }

    Signal(maintask, 1UL << signum);

    worker_run(w);

    /* own-context teardown before the final done handshake */
    worker_delete_port_ios(w);
    w->pub.done = 1;
}

/* ---- public API (devsoak.h) ---------------------------------------------- */

LONG
workers_start(void)
{
    ULONG want_workers = cfg.workers;
    ULONG want_qdepth  = cfg.qdepth;
    ULONG i, k;

    n_started = 0;
    for (k = 0; k < 5; k++)
        hk_disabled[k] = 0;

    if (want_workers == 0)
        want_workers = 1;
    if (want_qdepth == 0)
        want_qdepth = 1;

    if (want_workers > MAX_WORKERS) {
        out_task_printf("devsoak: -w %ld clamped to %ld (MAX_WORKERS)",
                        (LONG)want_workers, (LONG)MAX_WORKERS);
        want_workers = MAX_WORKERS;
    }
    if (want_qdepth > MAX_QDEPTH) {
        out_task_printf("devsoak: -q %ld clamped to %ld (MAX_QDEPTH)",
                        (LONG)want_qdepth, (LONG)MAX_QDEPTH);
        want_qdepth = MAX_QDEPTH;
    }

    {
        ULONG cap = quirk_maxinflight();

        if (cap != 0 && want_workers * want_qdepth > cap) {
            ULONG orig_w = want_workers, orig_q = want_qdepth;

            while (want_qdepth > 1 && want_workers * want_qdepth > cap)
                want_qdepth--;
            while (want_workers > 1 && want_workers * want_qdepth > cap)
                want_workers--;
            out_task_printf("devsoak: quirk caps total inflight to %ld: "
                            "workers %ld->%ld qdepth %ld->%ld",
                            (LONG)cap, (LONG)orig_w, (LONG)want_workers,
                            (LONG)orig_q, (LONG)want_qdepth);
        }
    }

    g_num_workers = want_workers;
    g_qdepth = want_qdepth;

    hs_signum = (ULONG)AllocSignal(-1);
    if ((LONG)hs_signum == -1) {
        out_task_printf("devsoak: workers: AllocSignal failed");
        return 1;
    }
    hs_main_task = FindTask(NULL);

    for (i = 0; i < g_num_workers; i++) {
        struct Task *t;

        if (worker_init_resources(i) != 0) {
            out_task_printf("devsoak: worker %ld: resource allocation failed",
                            (LONG)(i + 1));
            worker_free_resources(&workers[i]);
            goto fail_rollback;
        }

        hs_spawn = &workers[i];

        t = CreateTask((CONST_STRPTR)"devsoak.worker", 0L,
                       (APTR)worker_entry, WORKER_STACK);
        if (t == NULL) {
            out_task_printf("devsoak: worker %ld: CreateTask failed",
                            (LONG)(i + 1));
            worker_free_resources(&workers[i]);
            goto fail_rollback;
        }
        workers[i].pub.task = t;

        Wait(1UL << hs_signum);
        n_started = i + 1;

        if (workers[i].pub.dead) {
            /* worker couldn't build its port/request pool in its own
               context; it already reported and exited */
            goto fail_rollback;
        }
    }

    FreeSignal((LONG)hs_signum);
    hs_signum = (ULONG)-1;
    return 0;

fail_rollback:
    FreeSignal((LONG)hs_signum);
    hs_signum = (ULONG)-1;
    if (n_started > 0) {
        workers_request_stop();
        workers_wait_done();
        workers_cleanup();
    }
    return 1;
}

void
workers_request_stop(void)
{
    ULONG i;

    for (i = 0; i < n_started; i++)
        workers[i].pub.stop = 1;
}

void
workers_wait_done(void)
{
    UBYTE alldone;
    ULONG i;

    if (n_started == 0)
        return;

    do {
        alldone = 1;
        for (i = 0; i < n_started; i++) {
            if (!workers[i].pub.done) {
                alldone = 0;
                break;
            }
        }
        if (!alldone)
            timer_delay_ms(250);   /* main-task-only, per timer.c */
    } while (!alldone);

    /* short grace period for CreateTask()'s trampoline to actually
       RemTask() each worker after it set pub.done -- see the file header
       comment: done=1 is set just before falling off the entry function,
       not after the task has fully unwound. */
    timer_delay_ms(100);
}

void
workers_cleanup(void)
{
    ULONG i;

    for (i = 0; i < n_started; i++)
        worker_free_resources(&workers[i]);

    n_started = 0;
}

ULONG
workers_inflight(void)
{
    ULONG i, total = 0;

    Forbid();
    for (i = 0; i < n_started; i++)
        total += workers[i].pub.inflight;
    Permit();

    return total;
}

ULONG
workers_dead(void)
{
    ULONG i, total = 0;

    Forbid();
    for (i = 0; i < n_started; i++)
        if (workers[i].pub.dead)
            total++;
    Permit();

    return total;
}

ULONG
workers_oldest_secs(ULONG *worker, ULONG *cmd)
{
    ULONG i, j;
    ULONG now_s, now_u;
    ULONG oldest = 0, ow = 0, ocmd = 0;

    timer_gettime(&now_s, &now_u);

    Forbid();
    for (i = 0; i < n_started; i++) {
        for (j = 0; j < g_qdepth; j++) {
            struct WorkerSlot *s = &workers[i].pub.slots[j];

            if (s->active) {
                ULONG age = (now_s >= s->submit_secs)
                          ? (now_s - s->submit_secs) : 0;

                if (age >= oldest) {
                    oldest = age;
                    ow = workers[i].pub.id;
                    ocmd = s->cmd;
                }
            }
        }
    }
    Permit();

    if (worker != NULL)
        *worker = ow;
    if (cmd != NULL)
        *cmd = ocmd;

    return oldest;
}
