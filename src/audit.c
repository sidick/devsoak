/*
 * audit.c - devsoak periodic and start/end full-range audit sweeps
 * (implementation brief S6, S8 lifecycle note, S9).
 *
 * audit_sweep() is the shared engine: main calls it directly (initial and
 * final audits), and a plain Exec task ("devsoak.audit") spawned by
 * auditor_start() calls the same sweep repeatedly, cfg.audit_min minutes
 * apart, until asked to stop.
 *
 * Two contexts, so CALLER-LOCAL resources only: a private MsgPort/IOExtTD
 * pair (cloning io_Device/io_Unit from dev.io -- the standard multi-request
 * idiom, no second OpenDevice) and a private buf_alloc'd buffer, both
 * created at entry and freed at exit. Nothing here is shared mutable state
 * except g_generation and the stripe semaphores, both of which already
 * have their own concurrency story (stripe locks guard every access).
 *
 * The auditor task is a plain amiga.lib CreateTask() task, not a Process:
 * it must never call dos.library, directly or indirectly. That means it
 * can log only through out_task_printf()/ring_log() (never out_printf(),
 * which is main-task-only because it owns the CLI's Output() handle) and
 * it cannot use timer_delay_ms() (main-task-only per timer.c) -- it opens
 * its own timer.device unit and DoIOs 1-second TR_ADDREQUEST sleeps,
 * checking the stop flag between each one.
 *
 * NOTE: unlike engine.c's M2 diagnose_mismatch (and unlike the final
 * shutdown-path ring dumps in engine.c), a mismatch found here does NOT
 * call ring_dump(): ring_dump() is built on out_printf() (see ring.c),
 * which is only safe to call from the main task. Since this file's sweep
 * runs from the auditor task as often as from main, calling ring_dump()
 * from here would be a real console-ownership violation whenever the
 * auditor found the mismatch. See the final report for this reasoning;
 * engine.c's own shutdown-path ring_dump() calls are all made from main,
 * where they are safe.
 */

#include "devsoak.h"

#include <proto/exec.h>
#include <proto/alib.h>   /* CreatePort/CreateExtIO/DeleteExtIO/DeletePort/CreateTask */
#include <exec/memory.h>  /* MEMF_PUBLIC */

extern struct ExecBase *SysBase;

/* ---- auditor task state (file statics: exactly one auditor ever) ---- */

static struct Task    *auditor_taskptr;
static volatile UBYTE  auditor_stop;
static volatile UBYTE  auditor_done;
static UBYTE            auditor_active;   /* auditor_start() actually spawned it */

/* handshake signal, set up by auditor_start() before CreateTask(), used
 * once by the new task to confirm it is alive and has opened its timer
 * unit, then never touched again. */
static struct Task *hs_main_task;
static ULONG         hs_signum = (ULONG)-1;

/* ---- small helpers ---- */

/* RawDoFmt (via out_task_printf) has no 64-bit support; render decimal by
 * hand, exactly as engine.c's own copy does (this file may run on a task
 * with no shared state with engine.c, so it keeps its own copy rather than
 * exporting one). */
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

/* ---- mismatch diagnosis: single-buffer simplification of engine.c's M2
 * diagnose_mismatch (S9). Runs with the sector's stripe still held (the
 * caller only releases after this returns), so nothing else can touch the
 * sector while we re-read it: that is what makes it safe to overwrite the
 * same buffer instead of allocating a second one. It also means the
 * report has expected/read2 pairs only, never expected/read1/read2 --
 * read1 (the buffer contents that failed content_verify()) is decoded for
 * its header before being overwritten, but the raw payload bytes are
 * gone by the time we print, so no read1 payload column is possible. */
static void
diagnose(const char *label, struct IOExtTD *io, struct TestBuf *buf,
         U64 chunk_byteoff, ULONG chunk_len, U64 lba, ULONG idx_in_chunk,
         ULONG expect_gen, LONG cvclass)
{
    UBYTE *sect1 = buf->data + (ULONG)idx_in_chunk * dev.sector_size;
    UBYTE *sect2;
    struct SectorHdr hdr;
    struct Result res;
    ULONG  chk;
    ULONG  diffs = 0;
    ULONG  i;
    char   lbabuf[24], claimbuf[24], expbuf[24];
    /* dev.sector_size is validated to be <= 4096 (512/1024/2048/4096) at
     * open time in main.c; a stack buffer this size is fine for either
     * calling context (main's stack, or the auditor's 8192-byte stack --
     * plus the rest of this function's locals it is comfortably clear of
     * that limit since nothing else here is more than a few dozen bytes). */
    UBYTE  expected[4096];

    u64_to_str(lba, lbabuf);
    out_task_printf("devsoak: audit(%s): DATA MISMATCH sector %s class %s",
                     label, lbabuf, content_class_name(cvclass));

    content_decode_hdr(sect1, &hdr);
    chk = hdr.magic ^ hdr.seed ^ hdr.sector_hi ^ hdr.sector_lo
        ^ hdr.generation ^ hdr.writer ^ hdr.xfer_len;

    {
        U64 claimed_lba = ((U64)hdr.sector_hi << 32) | (U64)hdr.sector_lo;

        u64_to_str(claimed_lba, claimbuf);
        u64_to_str(lba, expbuf);
        out_task_printf("devsoak: audit(%s): hdr claims sector %s (expected %s)",
                         label, claimbuf, expbuf);
        out_task_printf("devsoak: audit(%s): hdr generation %ld (expected %ld), writer %ld, xfer_len %ld",
                         label, (LONG)hdr.generation, (LONG)expect_gen,
                         (LONG)hdr.writer, (LONG)hdr.xfer_len);
        out_task_printf("devsoak: audit(%s): hdr checksum %s",
                         label, (chk == hdr.hdr_check) ? "ok" : "bad");
    }

    /* single re-read into the SAME buffer -- the noted simplification
     * versus engine.c's M2 diagnoser, which had a dedicated second
     * buffer. Prefill 0x5A first so an untouched-by-DMA re-read is still
     * distinguishable from a payload mismatch. */
    buf_prefill(buf, 0x5A);
    op_build_rw(io, DIALECT_CMD, 0, chunk_byteoff, buf->data, chunk_len,
                g_changenum);
    op_do_sync(io, SUBMIT_DOIO, &res);
    {
        struct RingEntry e;

        timer_gettime(&e.secs, &e.micros);
        e.worker = 0;
        e.cmd    = io->iotd_Req.io_Command;
        e.off_hi = (ULONG)(chunk_byteoff >> 32);
        e.off_lo = (ULONG)(chunk_byteoff & 0xFFFFFFFFUL);
        e.length = chunk_len;
        e.data   = buf->data;
        e.err    = res.err;
        e.actual = res.actual;
        ring_log(&e);
    }

    if (res.err != 0 || res.actual != chunk_len) {
        out_task_printf("devsoak: audit(%s): diagnostic re-read failed too, err %ld actual %ld",
                         label, (LONG)res.err, (LONG)res.actual);
        return;
    }

    sect2 = buf->data + (ULONG)idx_in_chunk * dev.sector_size;

    /* Regenerate the expected sector, borrowing the on-disk writer/xfer_len
     * when the header's own checksum is intact (those two fields are not
     * otherwise checked by content_verify()), exactly as engine.c's M2
     * diagnoser does. */
    if (chk == hdr.hdr_check)
        content_build(expected, lba, expect_gen, hdr.writer, hdr.xfer_len);
    else
        content_build(expected, lba, expect_gen, 0, chunk_len);

    out_task_printf("devsoak: audit(%s): first differing bytes (expected/read2):", label);
    for (i = 0; i < dev.sector_size && diffs < 16; i++) {
        if (expected[i] != sect2[i]) {
            out_task_printf("devsoak: audit(%s):  +0x%lx exp %02lx read2 %02lx",
                             label, (ULONG)i, (ULONG)expected[i], (ULONG)sect2[i]);
            diffs++;
        }
    }
    if (diffs == 0) {
        out_task_printf("devsoak: audit(%s): re-read matches expected (transient first read?)",
                         label);
    }
}

/* ---- the sweep itself ---- */

static LONG
sweep(const char *label, volatile UBYTE *stop)
{
    struct MsgPort *port;
    struct IOExtTD *io;
    struct TestBuf  buf;
    ULONG range_len_u32, chunk_sectors, cur, done;
    LONG  rc;

    buf.base = NULL;

    port = CreatePort(NULL, 0);
    if (port == NULL) {
        out_task_printf("devsoak: audit(%s): CreatePort failed", label);
        return RC_FATAL;
    }

    io = (struct IOExtTD *)CreateExtIO(port, sizeof(struct IOExtTD));
    if (io == NULL) {
        DeletePort(port);
        out_task_printf("devsoak: audit(%s): CreateExtIO failed", label);
        return RC_FATAL;
    }

    /* standard multi-request idiom: clone the already-open device/unit
     * rather than OpenDevice() again. */
    io->iotd_Req.io_Device = dev.io->iotd_Req.io_Device;
    io->iotd_Req.io_Unit   = dev.io->iotd_Req.io_Unit;

    if (buf_alloc(&buf, g_chunk_bytes, ALIGN_LONG, g_bufmem) != 0) {
        out_task_printf("devsoak: audit(%s): buffer allocation failed", label);
        DeleteExtIO((struct IORequest *)io);
        DeletePort(port);
        return RC_FATAL;
    }

    range_len_u32 = (ULONG)cfg.range_len;   /* safe: engine.c's setup()
                                                already validated this fits
                                                the UWORD generation table */
    chunk_sectors = g_chunk_bytes / dev.sector_size;
    if (chunk_sectors == 0)
        chunk_sectors = 1;

    cur = 0;
    done = 0;
    rc = RC_CLEAN;

    while (cur < range_len_u32) {
        ULONG piece_sectors = ((range_len_u32 - cur) > chunk_sectors)
                              ? chunk_sectors : (range_len_u32 - cur);
        ULONG piece_bytes = piece_sectors * dev.sector_size;
        U64   abs_start = cfg.range_start + (U64)cur;
        U64   byteoff = abs_start * (U64)dev.sector_size;
        struct Result res;
        ULONG i;
        LONG  g;

        stripes_obtain(cur, piece_sectors);   /* blocking is safe: audit
                                                  holds no other stripes */

        buf_prefill(&buf, 0xA5);
        op_build_rw(io, DIALECT_CMD, 0, byteoff, buf.data, piece_bytes,
                    g_changenum);
        op_do_sync(io, SUBMIT_DOIO, &res);
        {
            struct RingEntry e;

            timer_gettime(&e.secs, &e.micros);
            e.worker = 0;
            e.cmd    = io->iotd_Req.io_Command;
            e.off_hi = (ULONG)(byteoff >> 32);
            e.off_lo = (ULONG)(byteoff & 0xFFFFFFFFUL);
            e.length = piece_bytes;
            e.data   = buf.data;
            e.err    = res.err;
            e.actual = res.actual;
            ring_log(&e);
        }

        if (res.err != 0 || res.actual != piece_bytes) {
            out_task_printf("devsoak: audit(%s): FAILED cmd %s off_hi %ld off_lo %ld "
                             "len %ld err %ld actual %ld",
                             label, op_cmd_name(io->iotd_Req.io_Command),
                             (LONG)(byteoff >> 32),
                             (LONG)(byteoff & 0xFFFFFFFFUL),
                             (LONG)piece_bytes, (LONG)res.err, (LONG)res.actual);
            stripes_release(cur, piece_sectors);
            rc = RC_ERROR;
            goto out;
        }

        /* Verify every sector while STILL holding the stripe: read the
         * generation value and content_verify() it in the same held
         * section, so a worker cannot sneak a write in between reading
         * generation[] and checking the buffer against it (which would
         * otherwise look like a stale-generation false positive). */
        for (i = 0; i < piece_sectors; i++) {
            ULONG idx = cur + i;

            if (g_generation[idx] != 0) {
                ULONG fd;
                LONG  cv = content_verify(buf.data + (ULONG)i * dev.sector_size,
                                          abs_start + i, (ULONG)g_generation[idx],
                                          &fd);
                if (cv != CV_OK) {
                    diagnose(label, io, &buf, byteoff, piece_bytes,
                             abs_start + i, i, (ULONG)g_generation[idx], cv);
                    stripes_release(cur, piece_sectors);
                    rc = RC_ERROR;
                    goto out;
                }
            }
        }

        g = buf_check_guards(&buf);
        if (g != 0) {
            out_task_printf("devsoak: audit(%s): GUARD OVERRUN (%s) at +%ld, "
                             "cmd %s off_hi %ld off_lo %ld len %ld",
                             label, (g > 0) ? "pre" : "post",
                             (LONG)((g > 0) ? (g - 1) : (-g - 1)),
                             op_cmd_name(io->iotd_Req.io_Command),
                             (LONG)(byteoff >> 32),
                             (LONG)(byteoff & 0xFFFFFFFFUL), (LONG)piece_bytes);
            stripes_release(cur, piece_sectors);
            rc = RC_ERROR;
            goto out;
        }

        stripes_release(cur, piece_sectors);

        cur += piece_sectors;
        done += piece_sectors;

        if (stop != NULL && *stop)
            break;   /* asked to stop mid-sweep: not a failure, just partial */
    }

    if (rc == RC_CLEAN) {
        out_task_printf("devsoak: audit(%s): clean (%ld sectors)",
                         label, (LONG)done);
    }

out:
    buf_free(&buf);
    DeleteExtIO((struct IORequest *)io);
    DeletePort(port);
    return rc;
}

LONG
audit_sweep(const char *label)
{
    return sweep(label, NULL);
}

/* ---- auditor task ---- */

static void
auditor_entry(void)
{
    struct MsgPort     *tport;
    struct timerequest *tio;
    UBYTE                topen = 0;

    tport = CreatePort(NULL, 0);
    tio = (tport != NULL)
        ? (struct timerequest *)CreateExtIO(tport, sizeof(struct timerequest))
        : NULL;
    if (tio != NULL) {
        if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK,
                       (struct IORequest *)tio, 0) == 0)
            topen = 1;
    }

    /* Tell auditor_start() we are alive (and have made our one attempt at
     * opening a timer unit) regardless of whether that attempt succeeded
     * -- otherwise a timer.device open failure here would hang main
     * forever waiting for a signal that never comes. */
    Signal(hs_main_task, 1UL << hs_signum);

    if (!topen) {
        out_task_printf("devsoak: auditor: could not open timer.device, "
                         "auditor task exiting without ever sweeping");
        if (tio != NULL)
            DeleteExtIO((struct IORequest *)tio);
        if (tport != NULL)
            DeletePort(tport);
        auditor_done = 1;
        return;   /* default task finalizer (RemTask()) cleans us up */
    }

    while (!auditor_stop) {
        ULONG target_s = cfg.audit_min * 60UL;
        ULONG waited;

        for (waited = 0; waited < target_s && !auditor_stop; waited++) {
            tio->tr_node.io_Command = TR_ADDREQUEST;
            tio->tr_node.io_Flags = 0;
            tio->tr_time.tv_secs = 1;
            tio->tr_time.tv_micro = 0;
            DoIO((struct IORequest *)tio);
        }

        if (auditor_stop)
            break;

        if (sweep("periodic audit", &auditor_stop) == RC_ERROR) {
            /* No dedicated "auditor failed" plumbing in devsoak.h: fold
             * it into the shared error count instead, exactly like every
             * other error source. Engine's final verdict already keys
             * off stats errors (plus dead workers, plus its own final
             * sweep), so this is enough for the auditor's failures to
             * turn the run's exit code non-clean without adding a new
             * cross-module API. bytes/usec are 0: this is a bookkeeping
             * bump, not a real op. */
            stats_record(CLASS_READ, 0, 0, -1);
        }
    }

    CloseDevice((struct IORequest *)tio);
    DeleteExtIO((struct IORequest *)tio);
    DeletePort(tport);

    auditor_done = 1;
    /* returning here runs the default task finalizer, which RemTask()s
     * us; CreateTask() registered our TCB and stack on tc_MemEntry, so
     * that memory is freed automatically (see final report). */
}

LONG
auditor_start(void)
{
    if (cfg.audit_min == 0)
        return 0;   /* start/end audits only, per -A 0 */

    hs_main_task = FindTask(NULL);
    hs_signum = (ULONG)AllocSignal(-1);
    if ((LONG)hs_signum == -1) {
        out_printf("devsoak: auditor: AllocSignal failed, periodic auditor not started");
        return 1;
    }

    auditor_stop = 0;
    auditor_done = 0;

    /* pri 0, the same as main and the workers: anything lower is
     * permanently starved when a worker is CPU-bound, which is exactly
     * what happens under an unthrottled emulator where I/O completes
     * before the worker ever sleeps in WaitPort() -- a pri -1 auditor
     * then never even gets to run its entry. Round-robin at equal
     * priority gives the auditor its share while stripe locks keep its
     * sweeps from stalling the workers. */
    /* 16 KB stack: sweep() -> diagnose() keeps a sector-sized (up to
     * 4 KB) expected[] buffer on the stack; no guard pages on a 68000. */
    auditor_taskptr = CreateTask((STRPTR)"devsoak.audit", 0,
                                 (APTR)auditor_entry, 16384UL);
    if (auditor_taskptr == NULL) {
        FreeSignal((LONG)hs_signum);
        hs_signum = (ULONG)-1;
        out_printf("devsoak: auditor: CreateTask failed, periodic auditor not started");
        return 1;
    }

    Wait(1UL << hs_signum);
    FreeSignal((LONG)hs_signum);
    hs_signum = (ULONG)-1;

    auditor_active = 1;
    out_printf("devsoak: auditor: started, sweeping every %ld min",
                (LONG)cfg.audit_min);
    return 0;
}

void
auditor_request_stop(void)
{
    if (auditor_active)
        auditor_stop = 1;
}

void
auditor_wait_done(void)
{
    if (!auditor_active)
        return;

    while (!auditor_done)
        timer_delay_ms(100);   /* main-task-only call; auditor_wait_done()
                                   is documented as main-only in devsoak.h */
}

void
auditor_cleanup(void)
{
    /* Nothing to free here: the auditor task frees its own timer.device
     * resources and its own TCB/stack (via the default task finalizer's
     * RemTask()) before setting auditor_done. Idempotent by construction
     * -- there is no state left to double-free. */
    auditor_active = 0;
}
