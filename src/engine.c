/*
 * engine.c - devsoak M2 sequential test engine (implementation brief S5,
 * S7 subset, S9).
 *
 * M2 scope: strictly sequential, single request in flight, main task only,
 * using dev.io/dev.port set up by main.c. Fill pass, initial verify sweep,
 * a randomised soak loop drawing from the mix in S7 (operation/length/
 * dialect/submission only -- addressing is always uniform-random within
 * the range, housekeeping ops and the S8 invariant matrix are out of
 * scope until the worker/invariant milestones), final verify sweep.
 *
 * All output goes through out_printf() (S9.1): %ld/%lx/%lu/%s only, every
 * argument cast to LONG/ULONG. RawDoFmt has no 64-bit support, so every
 * U64 value (byte offsets, LBAs, MB counters) is rendered with the local
 * u64_to_str() helper below and passed through as %s.
 */

#include "devsoak.h"

#include <proto/exec.h>
#include <exec/memory.h>
#include <exec/errors.h>    /* IOERR_NOCMD */
#include <dos/dos.h>        /* SIGBREAKF_CTRL_C */

/* ---- module state (M2 is single-task/sequential: file statics are fine;
 * a later milestone that spawns workers must not reuse this file as-is) */

static struct TestBuf wbuf, rbuf, r2buf;
static UWORD *generation;
static ULONG  range_len_u32;
static ULONG  sector_size;
static ULONG  chunk_bytes, chunk_sectors;
static ULONG  changenum;
static UBYTE  have_etd;
static UBYTE  ring_ready;

static ULONG  enabled_list[DIALECT_COUNT];
static ULONG  n_enabled;
static ULONG  enabled64_list[DIALECT_COUNT];
static ULONG  n_enabled64;

static ULONG  rng;

static ULONG  stat_total_ops, stat_writes, stat_reads;
static U64    stat_bytes_written, stat_bytes_read;

/* ---- forward decls ---- */

static void  u64_to_str(U64 v, char *buf);
static UBYTE nsd_has_cmd(UWORD cmd);
static const char *dialect_name(ULONG d);
static void  log_ring(UWORD worker, UWORD cmd, U64 byteoff, ULONG length,
                      APTR data, const struct Result *res);
static void  report_io_failure(const char *what, UWORD cmd, U64 byteoff,
                               ULONG len, LONG err, ULONG actual);
static void  report_guard_hit(LONG g, UWORD cmd, U64 byteoff, ULONG len);
static void  print_status(ULONG start_s, ULONG start_u);
static void  print_summary(void);
static void  diagnose_mismatch(U64 chunk_byteoff, ULONG chunk_len, U64 lba,
                               ULONG idx_in_chunk, ULONG expect_gen, LONG cvclass);
static LONG  setup(void);
static void  cleanup(void);
static LONG  fill_pass(void);
static LONG  verify_sweep(const char *label);
static LONG  soak_loop(void);

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

static void
report_guard_hit(LONG g, UWORD cmd, U64 byteoff, ULONG len)
{
    char offbuf[24];

    u64_to_str(byteoff, offbuf);
    if (g > 0) {
        out_printf("devsoak: GUARD OVERRUN (pre) at +%ld, cmd %s offset %s len %ld",
                    (LONG)(g - 1), op_cmd_name(cmd), offbuf, (LONG)len);
    } else {
        out_printf("devsoak: GUARD OVERRUN (post) at +%ld, cmd %s offset %s len %ld",
                    (LONG)(-g - 1), op_cmd_name(cmd), offbuf, (LONG)len);
    }
}

static void
print_status(ULONG start_s, ULONG start_u)
{
    ULONG elapsed_ms = timer_elapsed_ms(start_s, start_u);
    U64   mbw = stat_bytes_written / 1048576ULL;
    U64   mbr = stat_bytes_read   / 1048576ULL;
    char  mbwbuf[24], mbrbuf[24];

    u64_to_str(mbw, mbwbuf);
    u64_to_str(mbr, mbrbuf);

    out_printf("devsoak: status: %ld s, ops %ld (w %ld r %ld), written %s MB, read %s MB",
                (LONG)(elapsed_ms / 1000UL), (LONG)stat_total_ops,
                (LONG)stat_writes, (LONG)stat_reads, mbwbuf, mbrbuf);
}

static void
print_summary(void)
{
    U64  mbw = stat_bytes_written / 1048576ULL;
    U64  mbr = stat_bytes_read   / 1048576ULL;
    char mbwbuf[24], mbrbuf[24];

    u64_to_str(mbw, mbwbuf);
    u64_to_str(mbr, mbrbuf);

    out_printf("devsoak: summary: %ld ops total (%ld writes, %ld reads)",
                (LONG)stat_total_ops, (LONG)stat_writes, (LONG)stat_reads);
    out_printf("devsoak: summary: %s MB written, %s MB read", mbwbuf, mbrbuf);
}

/* ---- S9 mismatch diagnoser ----
 * Always operates on the module's rbuf (the buffer every read/verify in
 * this sequential engine uses) and r2buf (the diagnostic re-read buffer),
 * and uses wbuf as scratch space to regenerate the expected sector -- safe
 * because M2 never has a write in flight while diagnosing a read. */
static void
diagnose_mismatch(U64 chunk_byteoff, ULONG chunk_len, U64 lba,
                   ULONG idx_in_chunk, ULONG expect_gen, LONG cvclass)
{
    char   lbabuf[24];
    struct Result res;
    struct SectorHdr hdr;
    UBYTE *sect1 = rbuf.data + (ULONG)idx_in_chunk * sector_size;
    UBYTE *sect2;
    UBYTE *expected;
    ULONG  diffs = 0;
    ULONG  i;
    ULONG  chk;
    UBYTE  untouched = 1;

    u64_to_str(lba, lbabuf);
    out_printf("devsoak: DATA MISMATCH sector %s class %s",
                lbabuf, content_class_name(cvclass));

    /* (b) re-read the same chunk into r2buf, prefilled 0x5A (rbuf/sect1
     * was prefilled 0xA5 before its own read) */
    buf_prefill(&r2buf, 0x5A);
    op_build_rw(dev.io, DIALECT_CMD, 0, chunk_byteoff, r2buf.data, chunk_len,
                changenum);
    op_do_sync(dev.io, SUBMIT_DOIO, &res);
    log_ring(0, dev.io->iotd_Req.io_Command, chunk_byteoff, chunk_len,
             r2buf.data, &res);
    if (res.err != 0 || res.actual != chunk_len) {
        out_printf("devsoak: diagnostic re-read failed too, err %ld actual %ld",
                    (LONG)res.err, (LONG)res.actual);
    }
    sect2 = r2buf.data + (ULONG)idx_in_chunk * sector_size;

    /* (c) regenerate the expected sector into scratch (wbuf) and diff.
     * writer/xfer_len are per-write values verify does not check; when the
     * on-disk header's checksum is intact, borrow its own writer/xfer_len
     * so those bytes do not show up as phantom diffs. */
    content_decode_hdr(sect1, &hdr);
    chk = hdr.magic ^ hdr.seed ^ hdr.sector_hi ^ hdr.sector_lo
        ^ hdr.generation ^ hdr.writer ^ hdr.xfer_len;
    expected = wbuf.data;
    if (chk == hdr.hdr_check)
        content_build(expected, lba, expect_gen, hdr.writer, hdr.xfer_len);
    else
        content_build(expected, lba, expect_gen, 0, chunk_len);

    out_printf("devsoak: first differing bytes (expected/read1/read2):");
    for (i = 0; i < sector_size && diffs < 16; i++) {
        if (expected[i] != sect1[i]) {
            out_printf("  +0x%lx exp %02lx read1 %02lx read2 %02lx",
                        (ULONG)i, (ULONG)expected[i], (ULONG)sect1[i],
                        (ULONG)sect2[i]);
            if (!(sect1[i] == 0xA5 && sect2[i] == 0x5A))
                untouched = 0;
            diffs++;
        }
    }
    if (diffs == 0) {
        out_printf("devsoak: payload identical to expected (header-level mismatch only)");
    } else if (untouched) {
        out_printf("devsoak: buffer untouched by driver (DMA never landed)");
    }

    /* (d) header decode: claimed vs expected (hdr/chk from step (c)) */
    {
        char claimbuf[24], expbuf[24];
        U64  claimed_lba = ((U64)hdr.sector_hi << 32) | (U64)hdr.sector_lo;

        u64_to_str(claimed_lba, claimbuf);
        u64_to_str(lba, expbuf);
        out_printf("devsoak: hdr claims sector %s (expected %s)", claimbuf, expbuf);
        out_printf("devsoak: hdr generation %ld (expected %ld), writer %ld, xfer_len %ld",
                    (LONG)hdr.generation, (LONG)expect_gen, (LONG)hdr.writer,
                    (LONG)hdr.xfer_len);
        out_printf("devsoak: hdr checksum %s", (chk == hdr.hdr_check) ? "ok" : "bad");
    }

    /* (e) ring dump */
    ring_dump(4096);
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

    generation = AllocMem(bytes, MEMF_PUBLIC | MEMF_CLEAR);
    if (generation == NULL) {
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
        changenum = res.actual;
    } else {
        have_etd = 0;
        changenum = 0;
        out_printf("devsoak: TD_CHANGENUM unavailable, ETD dialects disabled");
    }

    /* chunk size: cfg.maxxfer rounded down to a sector multiple, halving
     * (and re-rounding) on allocation failure, down to one sector */
    try_bytes = cfg.maxxfer - (cfg.maxxfer % sector_size);
    if (try_bytes < sector_size)
        try_bytes = sector_size;

    for (;;) {
        LONG ok = 1;

        if (buf_alloc(&wbuf, try_bytes, ALIGN_LONG, MEMF_PUBLIC) != 0)
            ok = 0;
        if (ok && buf_alloc(&rbuf, try_bytes, ALIGN_LONG, MEMF_PUBLIC) != 0)
            ok = 0;
        if (ok && buf_alloc(&r2buf, try_bytes, ALIGN_LONG, MEMF_PUBLIC) != 0)
            ok = 0;

        if (ok) {
            chunk_bytes = try_bytes;
            break;
        }

        if (wbuf.base != NULL) buf_free(&wbuf);
        if (rbuf.base != NULL) buf_free(&rbuf);
        if (r2buf.base != NULL) buf_free(&r2buf);

        if (try_bytes <= sector_size) {
            out_printf("devsoak: could not allocate I/O buffers, not even one sector");
            return RC_FATAL;
        }
        try_bytes /= 2;
        try_bytes -= (try_bytes % sector_size);
        if (try_bytes < sector_size)
            try_bytes = sector_size;
    }
    chunk_sectors = chunk_bytes / sector_size;
    out_printf("devsoak: chunk size %ld sectors (%ld bytes)",
                (LONG)chunk_sectors, (LONG)chunk_bytes);

    /* usable dialect set */
    /* Candidate dialects, then a 1-sector read probe of each: the NSD
     * command list is advisory (and absent pre-NSD), and a dialect can be
     * unimplemented even when its prerequisites look present -- e.g.
     * scsi.device 47.4 answers TD_CHANGENUM but NOCMDs ETD_READ/WRITE.
     * The probe is authoritative here; a disagreement with the NSD list
     * is reported (the full listed<->implemented cross-check is the M4
     * invariant matrix). */
    n_enabled = 0;
    enabled_list[n_enabled++] = DIALECT_CMD;
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
                        rbuf.data, sector_size, changenum);
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
            enabled_list[n_enabled++] = cand[c].dialect;
        }
    }

    n_enabled64 = 0;
    {
        ULONG i;
        for (i = 0; i < n_enabled; i++) {
            if (enabled_list[i] == DIALECT_TD64 ||
                enabled_list[i] == DIALECT_NSD64 ||
                enabled_list[i] == DIALECT_NSDETD64) {
                enabled64_list[n_enabled64++] = enabled_list[i];
            }
        }
    }

    {
        ULONG i;
        for (i = 0; i < n_enabled; i++)
            out_printf("devsoak: dialect enabled: %s", dialect_name(enabled_list[i]));
    }

    rng = XS32_SEED(cfg.seed);

    stat_total_ops = 0;
    stat_writes = 0;
    stat_reads = 0;
    stat_bytes_written = 0;
    stat_bytes_read = 0;

    return 0;
}

static void
cleanup(void)
{
    if (wbuf.base != NULL)  buf_free(&wbuf);
    if (rbuf.base != NULL)  buf_free(&rbuf);
    if (r2buf.base != NULL) buf_free(&r2buf);

    if (generation != NULL) {
        FreeMem(generation, range_len_u32 * (ULONG)sizeof(UWORD));
        generation = NULL;
    }

    if (ring_ready) {
        ring_cleanup();
        ring_ready = 0;
    }
}

/* ---- fill pass (S5/S9 step 2) ---- */

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
                    changenum);
        op_do_sync(dev.io, SUBMIT_DOIO, &res);
        log_ring(0, dev.io->iotd_Req.io_Command, byteoff, piece_bytes,
                 wbuf.data, &res);

        if (res.err != 0 || res.actual != piece_bytes) {
            report_io_failure("fill", dev.io->iotd_Req.io_Command, byteoff,
                               piece_bytes, res.err, res.actual);
            return RC_ERROR;
        }

        for (i = 0; i < piece_sectors; i++)
            generation[(ULONG)(cur - cfg.range_start) + i] = 1;

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

/* ---- verify sweep (S9 steps 3 and 5: identical, reused for both) ---- */

static LONG
verify_sweep(const char *label)
{
    U64   cur = cfg.range_start;
    U64   remaining = cfg.range_len;
    ULONG done = 0;
    ULONG next_pct = 25;
    struct Result res;

    out_printf("devsoak: %s: starting", label);

    while (remaining > 0) {
        ULONG piece_sectors = (remaining > (U64)chunk_sectors)
                               ? chunk_sectors : (ULONG)remaining;
        ULONG piece_bytes = piece_sectors * sector_size;
        U64   byteoff = cur * (U64)sector_size;
        ULONG i;
        LONG  g;

        buf_prefill(&rbuf, 0xA5);
        op_build_rw(dev.io, DIALECT_CMD, 0, byteoff, rbuf.data, piece_bytes,
                    changenum);
        op_do_sync(dev.io, SUBMIT_DOIO, &res);
        log_ring(0, dev.io->iotd_Req.io_Command, byteoff, piece_bytes,
                 rbuf.data, &res);

        if (res.err != 0 || res.actual != piece_bytes) {
            report_io_failure(label, dev.io->iotd_Req.io_Command, byteoff,
                               piece_bytes, res.err, res.actual);
            return RC_ERROR;
        }

        for (i = 0; i < piece_sectors; i++) {
            U64   lba = cur + i;
            ULONG idx = (ULONG)(lba - cfg.range_start);

            if (generation[idx] != 0) {
                ULONG fd;
                LONG  cv = content_verify(rbuf.data + (ULONG)i * sector_size,
                                          lba, (ULONG)generation[idx], &fd);
                if (cv != CV_OK) {
                    diagnose_mismatch(byteoff, piece_bytes, lba, i,
                                       (ULONG)generation[idx], cv);
                    return RC_ERROR;
                }
            }
        }

        g = buf_check_guards(&rbuf);
        if (g != 0) {
            report_guard_hit(g, dev.io->iotd_Req.io_Command, byteoff, piece_bytes);
            return RC_ERROR;
        }

        cur += piece_sectors;
        remaining -= piece_sectors;
        done += piece_sectors;

        {
            U64 pct = ((U64)done * 100ULL) / (U64)range_len_u32;
            if (next_pct <= 100 && pct >= (U64)next_pct) {
                out_printf("devsoak: %s: %ld%%", label, (LONG)next_pct);
                while (next_pct <= 100 && pct >= (U64)next_pct)
                    next_pct += 25;
            }
        }

        if ((SetSignal(0, 0) & SIGBREAKF_CTRL_C) != 0) {
            SetSignal(0, SIGBREAKF_CTRL_C);
            out_printf("devsoak: %s: aborted by user (incomplete)", label);
            return RC_WARN;
        }
    }

    out_printf("devsoak: %s: clean (%ld sectors verified)", label, (LONG)done);
    return 0;
}

/* ---- soak loop (S7 subset for M2: op kind/length/dialect/submission;
 * addressing is uniform-random in range; housekeeping and the S8 matrix
 * are out of scope until the worker/invariant milestones) ---- */

static LONG
soak_loop(void)
{
    ULONG start_s, start_u;
    ULONG mark_s, mark_u;
    struct Result res;

    timer_gettime(&start_s, &start_u);
    mark_s = start_s;
    mark_u = start_u;

    out_printf("devsoak: soak: running for %ld s", (LONG)cfg.duration_s);

    for (;;) {
        ULONG bucket, nsect, dialect, submit;
        UBYTE is_write;
        U64   span, start_sec, byteoff;
        ULONG bytes;
        ULONG idx0;
        ULONG i;

        if ((SetSignal(0, 0) & SIGBREAKF_CTRL_C) != 0) {
            SetSignal(0, SIGBREAKF_CTRL_C);
            out_printf("devsoak: break: finishing with final audit");
            return RC_WARN;
        }

        if (timer_elapsed_ms(start_s, start_u) >= cfg.duration_s * 1000UL)
            break;

        is_write = (UBYTE)((xs32(&rng) % 100) < 50);

        bucket = xs32(&rng) % 4;
        switch (bucket) {
        case 0:
            nsect = 1;
            break;
        case 1:
            nsect = 2 + xs32(&rng) % 15;
            break;
        case 2:
            nsect = 17 + xs32(&rng) % 112;
            break;
        default:
            if (chunk_sectors < 129)
                nsect = chunk_sectors;
            else
                nsect = 129 + xs32(&rng) % (chunk_sectors - 129 + 1);
            break;
        }
        if (nsect > chunk_sectors)
            nsect = chunk_sectors;
        if ((U64)nsect > cfg.range_len)
            nsect = range_len_u32;
        if (nsect == 0)
            nsect = 1;

        span = cfg.range_len - (U64)nsect + 1ULL;
        {
            ULONG span_u = (ULONG)span;
            ULONG off_in_range = (span_u <= 1) ? 0 : (xs32(&rng) % span_u);
            start_sec = cfg.range_start + (U64)off_in_range;
        }

        byteoff = start_sec * (U64)sector_size;
        bytes = nsect * sector_size;

        dialect = enabled_list[xs32(&rng) % n_enabled];
        if ((dialect == DIALECT_CMD || dialect == DIALECT_ETD) &&
            (byteoff + (U64)bytes) > 0xFFFFFFFFULL) {
            if (n_enabled64 > 0) {
                dialect = enabled64_list[xs32(&rng) % n_enabled64];
            } else {
                /* no dialect enabled can address this op; skip it */
                continue;
            }
        }

        submit = xs32(&rng) % 3;  /* SUBMIT_DOIO/SENDIO/QUICK == 0/1/2 */
        idx0 = (ULONG)(start_sec - cfg.range_start);

        if (is_write) {
            LONG g;

            for (i = 0; i < nsect; i++) {
                ULONG idx = idx0 + i;
                content_build(wbuf.data + (ULONG)i * sector_size,
                              start_sec + i, (ULONG)generation[idx] + 1, 0,
                              bytes);
            }

            op_build_rw(dev.io, dialect, 1, byteoff, wbuf.data, bytes, changenum);
            op_do_sync(dev.io, submit, &res);
            log_ring(0, dev.io->iotd_Req.io_Command, byteoff, bytes,
                     wbuf.data, &res);

            if (res.err != 0 || res.actual != bytes) {
                report_io_failure("soak write", dev.io->iotd_Req.io_Command,
                                   byteoff, bytes, res.err, res.actual);
                ring_dump(64);
                return RC_ERROR;
            }

            for (i = 0; i < nsect; i++)
                generation[idx0 + i] = (UWORD)(generation[idx0 + i] + 1);

            g = buf_check_guards(&wbuf);
            if (g != 0) {
                report_guard_hit(g, dev.io->iotd_Req.io_Command, byteoff, bytes);
                return RC_ERROR;
            }

            stat_writes++;
            stat_bytes_written += (U64)bytes;
        } else {
            LONG g;

            buf_prefill(&rbuf, 0xA5);
            op_build_rw(dev.io, dialect, 0, byteoff, rbuf.data, bytes, changenum);
            op_do_sync(dev.io, submit, &res);
            log_ring(0, dev.io->iotd_Req.io_Command, byteoff, bytes,
                     rbuf.data, &res);

            if (res.err != 0 || res.actual != bytes) {
                report_io_failure("soak read", dev.io->iotd_Req.io_Command,
                                   byteoff, bytes, res.err, res.actual);
                ring_dump(64);
                return RC_ERROR;
            }

            for (i = 0; i < nsect; i++) {
                ULONG idx = idx0 + i;

                if (generation[idx] != 0) {
                    ULONG fd;
                    LONG  cv = content_verify(rbuf.data + (ULONG)i * sector_size,
                                              start_sec + i, (ULONG)generation[idx],
                                              &fd);
                    if (cv != CV_OK) {
                        diagnose_mismatch(byteoff, bytes, start_sec + i, i,
                                           (ULONG)generation[idx], cv);
                        return RC_ERROR;
                    }
                }
            }

            g = buf_check_guards(&rbuf);
            if (g != 0) {
                report_guard_hit(g, dev.io->iotd_Req.io_Command, byteoff, bytes);
                return RC_ERROR;
            }

            stat_reads++;
            stat_bytes_read += (U64)bytes;
        }

        stat_total_ops++;

        if (timer_elapsed_ms(mark_s, mark_u) >= 10000UL) {
            print_status(start_s, start_u);
            timer_gettime(&mark_s, &mark_u);
        }
    }

    out_printf("devsoak: soak: duration elapsed");
    return 0;
}

/* ---- entry point ---- */

LONG
engine_run(void)
{
    LONG rc;
    LONG soak_rc;

    wbuf.base = NULL;
    rbuf.base = NULL;
    r2buf.base = NULL;
    generation = NULL;
    ring_ready = 0;

    rc = setup();
    if (rc != 0)
        goto done;

    rc = fill_pass();
    if (rc != 0)
        goto done;

    rc = verify_sweep("initial audit");
    if (rc != 0)
        goto done;

    soak_rc = soak_loop();
    if (soak_rc == RC_ERROR) {
        rc = RC_ERROR;
        goto done;
    }

    rc = verify_sweep("final audit");
    if (rc != 0)
        goto done;

    print_summary();
    rc = (soak_rc == RC_WARN) ? RC_WARN : RC_CLEAN;

done:
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
