/*
 * devsoak - trackdisk-style block device driver soak tester
 *
 * Shared types, command ids and module interfaces.
 *
 * Hard constraints (see implementation brief §3):
 *  - Must run on Kickstart 1.3 and AROS m68k: exec.library, timer.device,
 *    dos.library (output only), amiga.lib CreatePort/CreateExtIO/CreateTask.
 *    No ReadArgs, no tag-based calls, no utility.library.
 *  - Built with bebbo amiga-gcc, -noixemul -m68000 -Os. No C stdio.
 *  - All formatting via exec RawDoFmt: sizes are 16-bit by default, so
 *    always %ld/%lx/%lu, never bare %d. No %b (V36+).
 */

#ifndef DEVSOAK_H
#define DEVSOAK_H

#include <exec/types.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <exec/devices.h>
#include <devices/trackdisk.h>
#include <devices/timer.h>

/* 64-bit LBAs/byte offsets. bebbo gcc supports long long on -m68000. */
typedef unsigned long long U64;

/* ---- command numbers not present in all NDK headers ---- */

#ifndef TD_GETGEOMETRY
#define TD_GETGEOMETRY  (CMD_NONSTD + 13)   /* 22 */
#endif
#ifndef TD_EJECT
#define TD_EJECT        (CMD_NONSTD + 14)   /* 23 */
#endif

/* TD64 dialect (Commodore-numbered 64-bit commands) */
#ifndef TD_READ64
#define TD_READ64       24
#define TD_WRITE64      25
#define TD_SEEK64       26
#define TD_FORMAT64     27
#endif

/* New Style Device dialect */
#ifndef NSCMD_DEVICEQUERY
#define NSCMD_DEVICEQUERY   0x4000
#endif
#ifndef NSCMD_TD_READ64
#define NSCMD_TD_READ64     0xC000
#define NSCMD_TD_WRITE64    0xC001
#define NSCMD_TD_SEEK64     0xC002
#define NSCMD_TD_FORMAT64   0xC003
#endif
#ifndef NSCMD_ETD_READ64
#define NSCMD_ETD_READ64    0xE000
#define NSCMD_ETD_WRITE64   0xE001
#define NSCMD_ETD_SEEK64    0xE002
#define NSCMD_ETD_FORMAT64  0xE003
#endif
#ifndef NSDEVTYPE_TRACKDISK
#define NSDEVTYPE_TRACKDISK 5
#endif

/* NSCMD_DEVICEQUERY result (devices/newstyle.h may be absent) */
struct NSDQueryResult {
    ULONG  DevQueryFormat;      /* 0 */
    ULONG  SizeAvailable;       /* bytes actually filled in */
    UWORD  DeviceType;
    UWORD  DeviceSubType;
    UWORD *SupportedCommands;   /* 0-terminated list */
};

/* ---- exit codes (§3) ---- */

#define RC_CLEAN    0
#define RC_WARN     5
#define RC_ERROR    10
#define RC_FATAL    20

/* ---- configuration from the command line (§10) ---- */

#define OUT_CON     0
#define OUT_SER     1
#define OUT_BOTH    2

#define MAX_MEMREGIONS  4
#define MAX_FORCEQUIRKS 8

struct Config {
    char   *device;             /* device name, e.g. "uaehf.device" */
    LONG    unit;
    U64     range_start;        /* -r, in sectors */
    U64     range_len;          /* -r, in sectors */
    ULONG   duration_s;         /* -t, seconds (default 60) */
    ULONG   workers;            /* -w (default 4) */
    ULONG   qdepth;             /* -q (default 4) */
    ULONG   stripe;             /* -S sectors (default 256) */
    ULONG   audit_min;          /* -A minutes (default 10, 0 = start/end only) */
    ULONG   maxxfer;            /* -M bytes (default 0x1FE00) */
    APTR    memregion[MAX_MEMREGIONS];  /* -m, extra buffer regions */
    ULONG   nmemregions;
    ULONG   seed;               /* -s (default from clock, printed) */
    ULONG   watchdog_s;         /* -W seconds (default 5) */
    UBYTE   outmode;            /* -o: OUT_CON/OUT_SER/OUT_BOTH */
    char   *quirksfile;         /* -Q (NULL = devsoak.quirks next to binary) */
    char   *forcequirk[MAX_FORCEQUIRKS]; /* -k ids */
    ULONG   nforcequirks;
    char   *hookcmd;            /* -H, eject/insert shell command */
    char   *crumbfile;          /* -P, breadcrumb file */
    UBYTE   destructive;        /* -d given (required) */
    UBYTE   have_range;         /* -r given (required) */
    UBYTE   yes;                /* -y skip confirmation */
    UBYTE   bigdev;             /* -B 4GB boundary tests */
    UBYTE   removable;          /* -R removable semantics */
    UBYTE   scsicmd;            /* -X HD_SCSICMD tests */
    UBYTE   risky;              /* -Z tier 3 */
    UBYTE   noquirks;           /* -K ignore quirks file */
    UBYTE   stoponerror;        /* -e */
    UBYTE   verbose;            /* -v */
    UBYTE   resume;             /* --resume (with -P) */
    UBYTE   seed_given;         /* -s was on the command line */
};

/* ---- device under test, shared read-only state after init ----
 * (Not "DevInfo": dos/dosextens.h owns that name for the DOS device list.) */

struct DevUnderTest {
    struct MsgPort   *port;      /* main task's port */
    struct IOExtTD   *io;        /* main task's request */
    UBYTE             opened;
    /* geometry snapshot (§8: must not change during the run) */
    struct DriveGeometry geom;
    UBYTE             have_geom;      /* TD_GETGEOMETRY worked */
    ULONG             sector_size;    /* 512/1024/2048/4096 */
    U64               total_sectors;
    /* driver identity, for quirks matching */
    char              dev_name[64];   /* ln_Name */
    UWORD             dev_version;
    UWORD             dev_revision;
    char              dev_idstring[128];
    /* NSCMD_DEVICEQUERY snapshot */
    UBYTE             have_nsd;
    UWORD             nsd_devtype;
    UWORD             nsd_cmds[64];   /* 0-terminated copy */
    ULONG             nsd_ncmds;
};

extern struct Config       cfg;
extern struct DevUnderTest dev;

/* ---- PRNG: xorshift32, one state per task (§13) ---- */

static __inline ULONG xs32(ULONG *s)
{
    ULONG x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

/* xorshift32 must never be seeded 0 (it would stick there) */
#define XS32_SEED(v) ((v) ? (v) : 0x9E3779B9UL)

/* ---- content.c (§5): sector content model ----
 * Every sector devsoak writes carries a 32-byte header followed by an
 * xorshift32 payload seeded  seed ^ (ULONG)sector ^ (generation << 16).
 * All verification is recomputable from (sector, generation).
 */

#define DSOK_MAGIC 0x44534F4BUL     /* 'DSOK' */

struct SectorHdr {                  /* big-endian on target = native */
    ULONG magic;                    /* 'DSOK' */
    ULONG seed;                     /* run seed */
    ULONG sector_hi;                /* absolute 64-bit LBA written to */
    ULONG sector_lo;
    ULONG generation;               /* per-sector write count, from 1 */
    ULONG writer;                   /* worker id (0 = auditor/main) */
    ULONG xfer_len;                 /* io_Length of the owning transfer */
    ULONG hdr_check;                /* XOR of the previous 7 longwords */
};

/* content_verify() result classes; each maps to a driver-bug class (§5) */
#define CV_OK            0
#define CV_HDR_CORRUPT   1  /* magic/seed/checksum wrong: not our data */
#define CV_WRONG_SECTOR  2  /* offset arithmetic / high word / off-by-one */
#define CV_STALE_GEN     3  /* lost write, reordering, cache not flushed */
#define CV_PAYLOAD       4  /* header right, payload wrong: partial DMA */
#define CV_FOREIGN       5  /* writer/xfer_len from another request */

void content_build(UBYTE *sect, U64 lba, ULONG generation, ULONG writer,
                   ULONG xfer_len);
/* expect_gen: value from generation[]; first_diff always gets the first
 * differing byte offset within the sector (0..31 for header-level
 * failures, >= 32 for payload mismatches). */
LONG content_verify(const UBYTE *sect, U64 lba, ULONG expect_gen,
                    ULONG *first_diff);
void content_decode_hdr(const UBYTE *sect, struct SectorHdr *out);
const char *content_class_name(LONG cv);

/* ---- ring.c (§9): op ring buffer ---- */

struct RingEntry {
    ULONG secs, micros;             /* timer_gettime stamp */
    UWORD worker;                   /* 0 = main/auditor */
    UWORD cmd;                      /* io_Command */
    ULONG off_hi, off_lo;           /* byte offset */
    ULONG length;
    APTR  data;
    LONG  err;                      /* io_Error (LONG for -1 sentinel) */
    ULONG actual;
};

LONG  ring_init(ULONG entries);     /* tries 'entries', halves on alloc
                                       failure; 0 on success */
void  ring_cleanup(void);
void  ring_log(const struct RingEntry *e);      /* task-safe */
void  ring_dump(ULONG maxn);        /* newest-last, via out_printf */

/* ---- buf.c: guarded, alignment/memtype-variant I/O buffers ---- */

#define GUARD_BYTES 64
#define GUARD_FILL  0xCC

#define ALIGN_LONG    0             /* longword aligned */
#define ALIGN_WORD    1             /* +2 */
#define ALIGN_ODD     2             /* +1 */
#define ALIGN_CROSS4K 3             /* crosses a 4 KB boundary */

struct TestBuf {
    UBYTE *base;                    /* AllocMem'd block */
    ULONG  basesize;
    UBYTE *data;                    /* io_Data: aligned per variant,
                                       GUARD_BYTES of 0xCC either side */
    ULONG  len;                     /* usable data length */
};

LONG  buf_alloc(struct TestBuf *b, ULONG len, ULONG alignsel, ULONG memflags);
void  buf_free(struct TestBuf *b);
LONG  buf_check_guards(const struct TestBuf *b);  /* 0 ok, else guard
                                                     offset +1 (pre) or
                                                     -(offset+1) (post) */
void  buf_prefill(struct TestBuf *b, UBYTE pattern);

/* ---- ops.c (§13): request builders, normalised result ---- */

struct Result {
    LONG  err;                      /* io_Error, normalised */
    ULONG actual;                   /* io_Actual after completion */
    ULONG flags;                    /* io_Flags after completion */
    ULONG usec;                     /* wall latency (best effort) */
};

#define DIALECT_CMD      0          /* CMD_READ/CMD_WRITE */
#define DIALECT_ETD      1          /* ETD_READ/ETD_WRITE (iotd_Count) */
#define DIALECT_TD64     2          /* TD_READ64/TD_WRITE64 (24/25) */
#define DIALECT_NSD64    3          /* NSCMD_TD_READ64/WRITE64 */
#define DIALECT_NSDETD64 4          /* NSCMD_ETD_READ64/WRITE64 */
#define DIALECT_COUNT    5

#define SUBMIT_DOIO      0
#define SUBMIT_SENDIO    1          /* SendIO + WaitIO */
#define SUBMIT_QUICK     2          /* BeginIO with IOF_QUICK */

/* Build a read/write. 64-bit dialects put the offset high word in
 * io_Actual (devtest convention); io_Actual/io_Error are pre-filled 0xa5
 * so a driver leaving them untouched is caught. changenum only matters
 * for ETD dialects. */
void  op_build_rw(struct IOExtTD *io, ULONG dialect, ULONG is_write,
                  U64 byteoff, APTR data, ULONG len, ULONG changenum);
/* Submit + wait synchronously, fill res (including latency). */
void  op_do_sync(struct IOExtTD *io, ULONG submit, struct Result *res);
/* Housekeeping/no-data command via DoIO. */
void  op_simple(struct IOExtTD *io, UWORD cmd, struct Result *res);
const char *op_cmd_name(UWORD cmd);

/* ---- engine.c: orchestrator ---- */

LONG  engine_run(void);             /* returns RC_CLEAN/RC_ERROR/RC_FATAL */

/* ---- M3 concurrency ---- */

#define MAX_WORKERS  8
#define MAX_QDEPTH   8

/* Shared per-sector write-count table (cfg.range_len UWORD entries,
 * indexed range-relative). Guarded by the stripe semaphores: touch an
 * entry only while holding its stripe. Owned by engine.c. */
extern UWORD *g_generation;

/* engine.c setup results shared with worker/audit tasks (read-only
 * after workers start) */
extern ULONG g_changenum;           /* ETD dialects' change count */
extern ULONG g_enabled_dialects[DIALECT_COUNT];
extern ULONG g_n_enabled;           /* probe-verified dialect set */
extern ULONG g_chunk_bytes;         /* per-request transfer cap */

/* ---- stripe.c (§6): one SignalSemaphore per -S sectors ----
 * Deadlock rule: workers NEVER block on stripes -- stripes_attempt()
 * only (ascending, all-or-nothing); on failure they pick a different
 * op. Only a task holding no stripes at all (the auditor, main) may use
 * the blocking stripes_obtain(), also ascending. NOTE SignalSemaphores
 * nest per-task: a worker's own two in-flight ops would both "acquire"
 * the same stripe, so workers must additionally avoid overlapping their
 * own in-flight sector ranges. */

LONG  stripes_init(void);           /* from cfg.stripe / cfg.range_len */
void  stripes_cleanup(void);
LONG  stripes_attempt(ULONG start, ULONG n);  /* range-relative sectors;
                                                 1 = all acquired */
void  stripes_release(ULONG start, ULONG n);
void  stripes_obtain(ULONG start, ULONG n);   /* blocking; hold nothing */

/* ---- stats.c (§6): semaphore-guarded stats + latency histogram ---- */

#define CLASS_READ   0
#define CLASS_WRITE  1
#define CLASS_HK     2              /* housekeeping */
#define CLASS_COUNT  3

#define LAT_BUCKETS  24             /* log2(usec) buckets */

struct StatsSnap {
    ULONG ops[CLASS_COUNT];
    U64   bytes[CLASS_COUNT];
    ULONG errors;
    ULONG p50_usec[CLASS_COUNT];
    ULONG p99_usec[CLASS_COUNT];
};

void  stats_init(void);
void  stats_record(ULONG cls, ULONG bytes, ULONG usec, LONG err);
void  stats_snapshot(struct StatsSnap *out);

/* ---- worker.c (§6/§7): N worker tasks, q requests in flight each ---- */

struct WorkerSlot {                 /* watchdog-visible, update under
                                       Forbid() */
    UBYTE  active;
    UBYTE  is_write;
    UWORD  cmd;
    ULONG  submit_secs;             /* timer_gettime at SendIO */
    ULONG  start_idx;               /* range-relative first sector */
    ULONG  nsect;
};

struct WorkerCtx {
    ULONG            id;            /* 1..N (0 = main/auditor) */
    struct Task     *task;
    volatile UBYTE   stop;          /* main -> worker: finish up */
    volatile UBYTE   done;          /* worker -> main: exited cleanly */
    volatile UBYTE   dead;          /* worker stopped itself on error */
    ULONG            rng;
    ULONG            inflight;
    struct WorkerSlot slots[MAX_QDEPTH];
    /* opaque to everyone but worker.c beyond here (ports, IOExtTDs,
       buffers) -- worker.c defines the rest via its own private struct
       that embeds this one. */
};

LONG  workers_start(void);          /* spawn cfg.workers tasks; 0 = ok */
void  workers_request_stop(void);
void  workers_wait_done(void);      /* returns once all done/dead */
void  workers_cleanup(void);
ULONG workers_inflight(void);       /* total, for the status line */
ULONG workers_dead(void);           /* count of error-stopped workers */
/* watchdog scan: oldest outstanding request age in seconds, and its
 * worker/cmd if wanted (may pass NULL) */
ULONG workers_oldest_secs(ULONG *worker, ULONG *cmd);

/* ---- invariant.c (§8/§16.3): continuous edge-case matrix ----
 * A dedicated task (pri 0, own port/requests/buffer) runs the named §8
 * tests in risk-tier order (0 -> 1 -> 2, tier 3 only with -Z), a full
 * pass every few seconds for the whole soak. "Pinned" behaviours record
 * the first observation; any later deviation is an error. Tier-2/3
 * commands emit an "about to send" serial breadcrumb before their FIRST
 * issue (§16.4). Matrix failures are folded into the stats error count
 * AND tracked separately for the verdict/summary. */

LONG  invariant_start(void);
void  invariant_request_stop(void);
void  invariant_wait_done(void);
void  invariant_cleanup(void);
ULONG invariant_errors(void);       /* matrix failures so far */
ULONG invariant_passes(void);       /* completed full matrix passes */
void  invariant_print_pins(void);   /* main-only: end-of-run pin summary */

/* ---- audit.c (§6): periodic full-range sweeps ---- */

LONG  auditor_start(void);          /* task sweeping every cfg.audit_min */
void  auditor_request_stop(void);
void  auditor_wait_done(void);
void  auditor_cleanup(void);
/* One full sweep in the calling task's context (used by main for the
 * initial/final audits; takes stripe locks; returns RC_*). */
LONG  audit_sweep(const char *label);

/* ---- output.c (§9.1) ----
 * Every line devsoak prints goes through out_printf().  RawDoFmt formats;
 * the console sink line-buffers and Write()s on the CLI handle (LF endings,
 * main task only for now -- M3 adds the cross-task hand-off queue); the
 * serial sink emits each character via RawPutChar (LVO -516) immediately,
 * CR LF endings, callable from any task.
 */
void out_init(UBYTE mode);       /* OUT_CON/OUT_SER/OUT_BOTH */
void out_cleanup(void);
void out_printf(const char *fmt, ...);   /* main task only (console) */
/* Any-task printf: serial sink emitted immediately (unbuffered, as
 * out_serial_line); console copy is queued for main, which must call
 * out_drain() regularly. Queue overflow drops the console copy (a
 * "... N console lines dropped" marker is emitted) -- the serial copy
 * always got out first. */
void out_task_printf(const char *fmt, ...);
void out_drain(void);            /* main: flush queued console lines */
/* Serial-only, unbuffered, any-task: crash breadcrumbs (§16.4). Always
 * emits to the serial sink regardless of the -o mode: serial is the only
 * sink guaranteed to survive a hang or crash. */
void out_serial_line(const char *fmt, ...);

/* ---- timer.c (§13) ----
 * TR_GETSYSTIME on V36+, DateStamp fallback on 1.3 (tick granularity).
 */
LONG  timer_init(void);          /* 0 on success */
void  timer_cleanup(void);
void  timer_gettime(ULONG *secs, ULONG *micros);
ULONG timer_elapsed_ms(ULONG s0, ULONG u0);  /* ms since (s0,u0) */
void  timer_delay_ms(ULONG ms);  /* independent of dos Delay() ticks */

#endif /* DEVSOAK_H */
