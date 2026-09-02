/*
 * invariant.c - devsoak §8/§16.3 continuous edge-case matrix.
 *
 * A single plain Exec task ("devsoak.invariant"), pri 0 (never lower --
 * see audit.c/worker.c for why: a lower-priority task is starved under an
 * unthrottled emulator), running the §8 matrix tiers 0 -> 1 -> 2 -> [3 if
 * cfg.risky] once per pass, forever, with a ~5 s sleep between passes
 * (5 x 1 s timer.device DoIO sleeps, checking the stop flag between each
 * one -- exactly audit.c's auditor_entry() pattern). Lifecycle tests
 * (§8 "Lifecycle") run on the first pass and again whenever a private
 * elapsed-seconds counter passes 1800 (~30 min).
 *
 * Own MsgPort + two IOExtTDs (io0/io1) cloning dev.io's already-open
 * io_Device/io_Unit -- the standard multi-request idiom -- plus a THIRD,
 * scratch IOExtTD used only for the lifecycle OpenDevice()/CloseDevice()
 * calls: OpenDevice() overwrites io_Device/io_Unit on the request it is
 * given, so reusing io0/io1 for it would corrupt the clone those two
 * carry for the rest of the matrix. All three are created here, in the
 * task's own context, so the port's mp_SigTask/mp_SigBit belong to this
 * task (see worker.c's identical note) -- a port created by main would
 * have every completion Signal() main while we sleep in DoIO()/WaitIO()
 * on a bit we never owned.
 *
 * This is a plain Exec task, not a Process: it must never call
 * dos.library, directly or indirectly. All reporting goes through
 * out_task_printf() (queued for main to Write()) or, for tier-2/3 crash
 * breadcrumbs (§16.4), out_serial_line() (always reaches the wire
 * immediately, regardless of -o mode). It cannot use timer_delay_ms()
 * (main-task-only per timer.c) -- it opens its own private timer.device
 * unit and DoIOs 1 s TR_ADDREQUEST sleeps.
 *
 * Stripe discipline: this task holds nothing else, so stripes_obtain()
 * would be safe, but every write-taking test here prefers the
 * non-blocking stripes_attempt() with a skip-this-pass fallback instead,
 * exactly as the brief asks -- a soak run should never let the matrix
 * task stall behind worker traffic; a skipped bounds/format write this
 * pass just tries again next pass a few seconds later.
 */

#include "devsoak.h"

#include <proto/exec.h>
#include <proto/alib.h>   /* CreatePort/CreateExtIO/DeleteExtIO/DeletePort/
                              CreateTask/SendIO/CheckIO/WaitIO */
#include <exec/memory.h>  /* MEMF_PUBLIC */
#include <exec/errors.h>  /* IOERR_* */
#include <string.h>       /* memcmp/memset/strcmp */

extern struct ExecBase *SysBase;

#define INV_STACK 16384UL

/* ---- pin table (§8 "pinned") ---- */

struct PinEntry {
    const char *name;
    UBYTE       set;
    LONG        value;
};

#define MAX_PINS 40

static struct PinEntry pins[MAX_PINS];
static ULONG            n_pins;

/* ---- tier-2/3 first-issue breadcrumb tracking (§16.4) ----
 * Keyed by io_Command; a command not yet seen gets one out_serial_line()
 * before its first DoIO/SendIO this run, then never again. Random command
 * numbers (test 21) are deliberately NOT tracked here -- see run_tier3()'s
 * comment -- so this table only ever holds the small, fixed set of real
 * tier-2/3 commands the matrix issues. */

struct Breadcrumb {
    UWORD cmd;
    UBYTE announced;
};

#define MAX_BREADCRUMB_CMDS 16

static struct Breadcrumb bc[MAX_BREADCRUMB_CMDS];
static ULONG              n_bc;

/* ---- task state (file statics: exactly one invariant task ever) ---- */

static struct Task    *inv_taskptr;
static volatile UBYTE  inv_stop;
static volatile UBYTE  inv_done;
static UBYTE            inv_active;
static ULONG            inv_errors;
static ULONG            inv_warnings;
static ULONG            inv_passes;

/* ---- §16.2 `skiptest`: print "SKIP <test> (quirk)" once per run per
 * test; quirks_report() covers the id-level detail, this is just the
 * once-only runtime notice. ---- */

struct SkipReport {
    const char *name;
};

#define MAX_SKIP_TESTS 32

static struct SkipReport skiprep[MAX_SKIP_TESTS];
static ULONG              n_skiprep;

/* handshake, exactly auditor_start()/auditor_entry()'s pattern */
static struct Task *hs_main_task;
static ULONG         hs_signum = (ULONG)-1;

/* set once at task start; some tests are gated on it */
static UBYTE g_can_maxtransfer;

/* small per-run "already handled, stop retrying" latches -- separate from
 * the pin table because they gate WHICH tests run, not an observed value */
static UBYTE stop_skip;    /* CMD_STOP returned IOERR_NOCMD */
static UBYTE motor_skip;   /* TD_MOTOR returned IOERR_NOCMD */

/* §8 "-B" 4 GB boundary tests (tests 26/27): once-per-run notices, separate
 * from test_skip_check()'s SKIP reporting because these aren't quirk-driven
 * skips -- they're "the option was given but the situation doesn't apply". */
static UBYTE bigdev_toosmall_notice;   /* -B given, device < 4 GB */
static UBYTE bigdev_norange_notice;    /* -r doesn't straddle/reach the
                                           boundary with 4 in-range sectors */

/* ---- small helpers ---- */

/* RawDoFmt has no 64-bit support; render decimal by hand (every module
   that needs this keeps its own tiny copy -- see engine.c/audit.c/worker.c). */
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

static void
matrix_fail_bump(void)
{
    inv_errors++;
    stats_record(CLASS_HK, 0, 0, -1);
}

/* Every §8 test failure (other than a pin violation -- see pin_check()'s
 * comment below for why those are never downgradable) goes through here
 * instead of the bare matrix_fail_bump(): a `warn TEST` quirk downgrades
 * it to a counted warning instead of a hard error. The FAIL detail line
 * is still printed by the caller before this is called. */
static void
matrix_fail(const char *test)
{
    if (quirk_test_warn(test)) {
        inv_warnings++;
        out_task_printf("devsoak: matrix: WARN (downgraded by quirk) %s",
                        test);
    } else {
        matrix_fail_bump();
    }
}

/* §16.2 `skiptest`: returns 1 (and prints the SKIP notice, once per run)
 * if the named test should not run at all this pass. */
static UBYTE
test_skip_check(const char *name)
{
    ULONG i;

    if (!quirk_test_skipped(name))
        return 0;

    for (i = 0; i < n_skiprep; i++) {
        if (strcmp(skiprep[i].name, name) == 0)
            return 1;   /* already reported this run */
    }

    if (n_skiprep < MAX_SKIP_TESTS) {
        skiprep[n_skiprep].name = name;
        n_skiprep++;
    }
    out_task_printf("devsoak: matrix: SKIP %s (quirk)", name);
    return 1;
}

/* pin_check(): first observation wins; every later call must match it.
 * Returns 0 if consistent (including "just pinned"), 1 on a violation
 * (already bumped the error counter). */
static LONG
pin_check(const char *name, LONG observed)
{
    ULONG i;

    for (i = 0; i < n_pins; i++) {
        if (strcmp(pins[i].name, name) == 0) {
            if (pins[i].value == observed)
                return 0;
            out_task_printf("devsoak: matrix: PIN VIOLATION %s: was %ld now %ld",
                             name, (LONG)pins[i].value, (LONG)observed);
            matrix_fail_bump();
            return 1;
        }
    }

    if (n_pins < MAX_PINS) {
        pins[n_pins].name = name;
        pins[n_pins].set = 1;
        pins[n_pins].value = observed;
        n_pins++;
        out_task_printf("devsoak: matrix: pinned %s = %ld", name, (LONG)observed);
    } else {
        out_task_printf("devsoak: matrix: pin table full, cannot pin %s (value %ld)",
                         name, (LONG)observed);
    }
    return 0;
}

/* forward decl: defined near the task entry point, needed by crumb_sync()
   below for the -P handshake's bounded wait. */
static void inv_sleep_1s(struct timerequest *tio);

/* Bounded strcat: appends as much of src as fits in dst's remaining
 * space (dstsize includes room for the NUL), never overruns. */
static void
bounded_strcat(char *dst, ULONG dstsize, const char *src)
{
    ULONG dlen = strlen(dst);
    ULONG slen;
    ULONG avail;

    if (dstsize == 0 || dlen >= dstsize - 1)
        return;
    avail = dstsize - 1 - dlen;
    slen = strlen(src);
    if (slen > avail)
        slen = avail;
    memcpy(dst + dlen, src, slen);
    dst[dlen + slen] = '\0';
}

/* Assemble "about to send <name> len=<n> off=<o>" by hand -- no RawDoFmt
 * (this text goes to the -P crumb file via crumb_write(), not through the
 * output layer). */
static void
build_crumb_line(char *out, ULONG outsize, UWORD cmd, ULONG len, U64 off)
{
    char numbuf[24];

    if (outsize == 0)
        return;
    out[0] = '\0';
    bounded_strcat(out, outsize, "about to send ");
    bounded_strcat(out, outsize, op_cmd_name(cmd));
    bounded_strcat(out, outsize, " len=");
    u64_to_str((U64)len, numbuf);
    bounded_strcat(out, outsize, numbuf);
    bounded_strcat(out, outsize, " off=");
    u64_to_str(off, numbuf);
    bounded_strcat(out, outsize, numbuf);
}

/* §16.4 -P handshake: publish the crumb line and wait (bounded) for main
 * to persist it via crumb_write() -- main services this from its status
 * loop every 250 ms, so normally one 1 s sleep is more than enough; cap
 * at 5 so a main that is itself stuck (or -P unset) never wedges this
 * task. No-op when -P is unset. */
static void
crumb_sync(struct timerequest *tio, const char *line)
{
    ULONG n;
    ULONG len;

    if (cfg.crumbfile == NULL)
        return;

    len = strlen(line);
    if (len > sizeof(g_crumb_text) - 1)
        len = sizeof(g_crumb_text) - 1;
    memcpy(g_crumb_text, line, len);
    g_crumb_text[len] = '\0';
    g_crumb_pending = 1;

    for (n = 0; n < 5 && g_crumb_pending && !inv_stop; n++)
        inv_sleep_1s(tio);
}

/* §16.4: before the FIRST issue of a distinct tier-2/3 command this run,
 * announce it on the serial sink unconditionally, and (with -P) hand it
 * to main for the crumb file. off is rendered via u64_to_str() since
 * RawDoFmt (which out_serial_line uses) has no 64-bit support. */
static void
breadcrumb(struct timerequest *tio, UWORD cmd, ULONG len, U64 off, APTR data)
{
    ULONG i;
    char  offbuf[24];
    char  crumbline[200];

    for (i = 0; i < n_bc; i++) {
        if (bc[i].cmd == cmd) {
            if (bc[i].announced)
                return;
            bc[i].announced = 1;
            u64_to_str(off, offbuf);
            out_serial_line("devsoak: about to send %s(0x%lx) len=%ld off=%s data=0x%lx",
                             op_cmd_name(cmd), (ULONG)cmd, (LONG)len, offbuf,
                             (ULONG)data);
            build_crumb_line(crumbline, sizeof(crumbline), cmd, len, off);
            crumb_sync(tio, crumbline);
            return;
        }
    }

    if (n_bc < MAX_BREADCRUMB_CMDS) {
        bc[n_bc].cmd = cmd;
        bc[n_bc].announced = 1;
        n_bc++;
        u64_to_str(off, offbuf);
        out_serial_line("devsoak: about to send %s(0x%lx) len=%ld off=%s data=0x%lx",
                         op_cmd_name(cmd), (ULONG)cmd, (LONG)len, offbuf,
                         (ULONG)data);
        build_crumb_line(crumbline, sizeof(crumbline), cmd, len, off);
        crumb_sync(tio, crumbline);
    }
    /* table full: extremely unlikely given the small fixed set of real
       tier-2/3 commands issued below -- silently skip the breadcrumb
       rather than fail the matrix over a logging nicety. */
}

static UBYTE
dialect_enabled(ULONG d)
{
    ULONG i;

    for (i = 0; i < g_n_enabled; i++)
        if (g_enabled_dialects[i] == d)
            return 1;
    return 0;
}

static UBYTE
nsd_list_has(UWORD cmd)
{
    ULONG i;

    for (i = 0; i < dev.nsd_ncmds; i++)
        if (dev.nsd_cmds[i] == cmd)
            return 1;
    return 0;
}

/* ---- tier 0: benign ------------------------------------------------- */

static void
run_tier0(struct IOExtTD *io0, struct IOExtTD *io1, struct TestBuf *buf)
{
    ULONG ss = dev.sector_size;

    /* 1 + 7: read-last-sector / actual-eq-length / io_Error stale-value
     * catch (io_Error prefilled 0xa5 by op_build_rw). */
    if (dev.have_geom && !test_skip_check("read-last-sector")) {
        U64 off = (dev.total_sectors - 1) * (U64)ss;
        struct Result res;

        /* >4GB offsets need a 64-bit dialect (op_build_rw upgrades
         * automatically); if the driver has none, a truncated 32-bit
         * read would be a false failure -- skip. (Thanks LIV2.) */
        if (op_dialect_for(off, ss) != DIALECT_COUNT) {
            op_build_rw(io0, DIALECT_CMD, 0, off, buf->data, ss,
                        g_changenum);
            op_do_sync(io0, SUBMIT_DOIO, &res);
            if (res.err != 0 || res.actual != ss) {
                out_task_printf("devsoak: matrix: FAIL read-last-sector: "
                                 "cmd %s off_lo %ld len %ld err %ld "
                                 "actual %ld",
                                 op_cmd_name(io0->iotd_Req.io_Command),
                                 (LONG)(off & 0xFFFFFFFFUL), (LONG)ss,
                                 (LONG)res.err, (LONG)res.actual);
                matrix_fail("read-last-sector");
            }
        }
    }

    /* 2: geometry-stable */
    if (dev.have_geom && !test_skip_check("geometry-stable")) {
        struct DriveGeometry g2;

        memset(&g2, 0, sizeof(g2));
        io0->iotd_Req.io_Command = TD_GETGEOMETRY;
        io0->iotd_Req.io_Data = (APTR)&g2;
        io0->iotd_Req.io_Length = sizeof(g2);
        io0->iotd_Req.io_Offset = 0;
        io0->iotd_Req.io_Flags = 0;
        io0->iotd_Req.io_Actual = 0xa5a5a5a5UL;
        io0->iotd_Req.io_Error = (BYTE)0xa5;
        DoIO((struct IORequest *)io0);

        if (io0->iotd_Req.io_Error != 0) {
            out_task_printf("devsoak: matrix: FAIL geometry-stable: "
                             "TD_GETGEOMETRY err %ld",
                             (LONG)io0->iotd_Req.io_Error);
            matrix_fail("geometry-stable");
        } else if (memcmp(&g2, &dev.geom, sizeof(g2)) != 0) {
            out_task_printf("devsoak: matrix: FAIL geometry-stable: "
                             "snapshot changed since startup");
            matrix_fail("geometry-stable");
        }
    }

    /* 3: changestate-constant */
    if (!test_skip_check("changestate-constant")) {
        struct Result res;

        op_simple(io0, TD_CHANGESTATE, &res);
        if (res.err == 0)
            pin_check("changestate", (LONG)res.actual);
    }

    /* 4: protstatus-constant (+ write-protect sub-test) */
    if (!test_skip_check("protstatus-constant")) {
        struct Result res;

        op_simple(io0, TD_PROTSTATUS, &res);
        if (res.err == 0) {
            LONG viol = pin_check("protstatus", (LONG)res.actual);

            if (!viol && res.actual != 0) {
                if (stripes_attempt(0, 1)) {
                    struct Result wres;
                    U64 off = cfg.range_start * (U64)ss;

                    content_build(buf->data, cfg.range_start,
                                  (ULONG)g_generation[0] + 1, 0, ss);
                    op_build_rw(io1, DIALECT_CMD, 1, off, buf->data, ss,
                                g_changenum);
                    op_do_sync(io1, SUBMIT_DOIO, &wres);
                    if (wres.err != TDERR_WriteProt) {
                        out_task_printf("devsoak: matrix: FAIL "
                                        "protstatus-constant: write to "
                                        "protected device err %ld "
                                        "(expected TDERR_WriteProt)",
                                        (LONG)wres.err);
                        matrix_fail("protstatus-constant");
                    }
                    stripes_release(0, 1);
                }
            }
        }
    }

    /* 5: nsd-query-stable */
    if (dev.have_nsd && !test_skip_check("nsd-query-stable")) {
        struct NSDQueryResult nsdq;
        ULONG n = 0;

        memset(&nsdq, 0, sizeof(nsdq));
        io0->iotd_Req.io_Command = NSCMD_DEVICEQUERY;
        io0->iotd_Req.io_Data = (APTR)&nsdq;
        io0->iotd_Req.io_Length = sizeof(nsdq);
        io0->iotd_Req.io_Offset = 0;
        io0->iotd_Req.io_Flags = 0;
        io0->iotd_Req.io_Actual = 0xa5a5a5a5UL;
        io0->iotd_Req.io_Error = (BYTE)0xa5;
        DoIO((struct IORequest *)io0);

        if (io0->iotd_Req.io_Error != 0) {
            out_task_printf("devsoak: matrix: FAIL nsd-query-stable: "
                             "DEVICEQUERY err %ld",
                             (LONG)io0->iotd_Req.io_Error);
            matrix_fail("nsd-query-stable");
        } else {
            if (nsdq.SupportedCommands != NULL) {
                while (n < 63 && nsdq.SupportedCommands[n] != 0)
                    n++;
            }
            if (nsdq.DeviceType != dev.nsd_devtype ||
                (dev.nsd_devtype == NSDEVTYPE_TRACKDISK &&
                 nsdq.DeviceType != NSDEVTYPE_TRACKDISK) ||
                n != dev.nsd_ncmds) {
                out_task_printf("devsoak: matrix: FAIL nsd-query-stable: "
                                 "devtype %ld (was %ld) ncmds %ld (was %ld)",
                                 (LONG)nsdq.DeviceType, (LONG)dev.nsd_devtype,
                                 (LONG)n, (LONG)dev.nsd_ncmds);
                matrix_fail("nsd-query-stable");
            }
        }
    }

    /* 6: quick-flag */
    if (!test_skip_check("quick-flag") && stripes_attempt(0, 1)) {
        struct Result res;

        buf_prefill(buf, 0xA5);
        op_build_rw(io0, DIALECT_CMD, 0, cfg.range_start * (U64)ss,
                    buf->data, ss, g_changenum);
        op_do_sync(io0, SUBMIT_QUICK, &res);
        pin_check("iof-quick", (res.flags & IOF_QUICK) ? 1 : 0);
        stripes_release(0, 1);
    }
}

/* ---- tier 1: error paths --------------------------------------------- */

static void
run_tier1(struct IOExtTD *io0, struct IOExtTD *io1, struct TestBuf *buf)
{
    ULONG ss = dev.sector_size;

    (void)io1;

    /* 8: read-past-end */
    if (dev.have_geom && !test_skip_check("read-past-end")) {
        U64 off = dev.total_sectors * (U64)ss;
        struct Result res;

        /* >4GB device end with no 64-bit dialect: a truncated 32-bit
         * probe would be a false failure -- skip. (Thanks LIV2.) */
        if (op_dialect_for(off, ss) != DIALECT_COUNT) {
            op_build_rw(io0, DIALECT_CMD, 0, off, buf->data, ss,
                        g_changenum);
            op_do_sync(io0, SUBMIT_DOIO, &res);
            if (res.err == 0) {
                out_task_printf("devsoak: matrix: FAIL read-past-end: "
                                 "silently succeeded, off_lo %ld",
                                 (LONG)(off & 0xFFFFFFFFUL));
                matrix_fail("read-past-end");
            } else {
                pin_check("past-end-err", (LONG)res.err);
            }
        }
    }

    /* 9: straddle-end (read-only, per brief: those sectors are normally
     * outside the range so no write variant is attempted) */
    if (dev.have_geom && dev.total_sectors >= 4 &&
        !test_skip_check("straddle-end")) {
        U64   off = (dev.total_sectors - 2) * (U64)ss;
        ULONG len = 4 * ss;
        struct Result res;

        /* same >4GB/no-64-bit skip as test 8 */
        if (op_dialect_for(off, len) != DIALECT_COUNT) {
            op_build_rw(io0, DIALECT_CMD, 0, off, buf->data, len,
                        g_changenum);
            op_do_sync(io0, SUBMIT_DOIO, &res);
            if (res.err == 0) {
                out_task_printf("devsoak: matrix: FAIL straddle-end: "
                                 "silently succeeded");
                matrix_fail("straddle-end");
            } else if (res.actual > 2 * ss) {
                out_task_printf("devsoak: matrix: FAIL straddle-end: "
                                 "io_Actual not clamped, actual %ld "
                                 "expected <= %ld",
                                 (LONG)res.actual, (LONG)(2 * ss));
                matrix_fail("straddle-end");
            }
        }
    }

    /* 10: zero-length */
    if (!test_skip_check("zero-length")) {
        struct Result res;
        UBYTE ok = 1;
        ULONG i;

        buf_prefill(buf, 0xA5);
        op_build_rw(io0, DIALECT_CMD, 0, cfg.range_start * (U64)ss,
                    buf->data, 0, g_changenum);
        op_do_sync(io0, SUBMIT_DOIO, &res);
        /* The brief expects success with io_Actual 0, and scsi.device
         * 47.4 does that -- but lide.device 40.12 rejects io_Length 0
         * with IOERR_BADLENGTH, also defensible. Pin the error code
         * (0 = accepted); a successful zero-length op reporting a
         * nonzero io_Actual is still a hard failure, and the
         * buffer-untouched check below applies either way. */
        pin_check("zero-length-err", (LONG)res.err);
        if (res.err == 0) {
            /* trackdisk.device 47.14 rounds a zero-length read UP: it
             * reports io_Actual = one sector and really transfers that
             * sector. Odd, but self-consistent -- pin the actual. The
             * hard invariant is that nothing is touched BEYOND what
             * io_Actual claims (and the guards). */
            pin_check("zero-length-actual", (LONG)res.actual);
        }
        if (ok) {
            ULONG from = (res.err == 0 && res.actual <= buf->len)
                         ? res.actual : 0;

            for (i = from; i < buf->len; i++) {
                if (buf->data[i] != 0xA5) {
                    out_task_printf("devsoak: matrix: FAIL zero-length: "
                                     "buffer touched at +%ld (beyond "
                                     "io_Actual %ld)", (LONG)i,
                                     (LONG)res.actual);
                    matrix_fail("zero-length");
                    break;
                }
            }
            if (buf_check_guards(buf) != 0) {
                out_task_printf("devsoak: matrix: FAIL zero-length: guard "
                                 "overrun");
                matrix_fail("zero-length");
            }
        }
    }

    /* 11: unaligned-length + unaligned-offset */
    if (!test_skip_check("unaligned-length")) {
        struct Result res;

        op_build_rw(io0, DIALECT_CMD, 0, cfg.range_start * (U64)ss,
                    buf->data, ss + 1, g_changenum);
        op_do_sync(io0, SUBMIT_DOIO, &res);
        /* Brief expects IOERR_BADLENGTH; scsi.device 47.4 rejects (-4)
         * but lide.device 40.12 services byte-granular lengths -- both
         * defensible for a READ, so pinned rather than hard-failed
         * (0 = accepted). Writes never use unaligned lengths here. */
        pin_check("unaligned-len-err", (LONG)res.err);
    }
    if (!test_skip_check("unaligned-offset")) {
        struct Result res;
        U64 off = cfg.range_start * (U64)ss + 1;

        op_build_rw(io0, DIALECT_CMD, 0, off, buf->data, ss, g_changenum);
        op_do_sync(io0, SUBMIT_DOIO, &res);
        /* The brief expects an error here, but scsi.device 47.4 (the
         * reference driver) services byte-granular offsets fine -- so
         * this is a pinned behaviour, not a hard expectation: 0 means
         * "accepted"; any change during the run is still a violation. */
        pin_check("unaligned-off-err", (LONG)res.err);
    }

    /* 12: maxtransfer -- only when the task buffer is big enough to read
     * one sector past MaxTransfer (g_can_maxtransfer, set once at task
     * start; a "skipped, no buffer" note is printed once there too). */
    if (g_can_maxtransfer && !test_skip_check("maxtransfer")) {
        ULONG len = g_chunk_bytes + ss;             /* one sector over MaxTransfer */
        ULONG range_bytes = (ULONG)cfg.range_len * ss;
        struct Result res;

        if (len > range_bytes)
            len = range_bytes - (range_bytes % ss);
        if (len > buf->len)
            len = buf->len - (buf->len % ss);

        if (len >= ss) {
            op_build_rw(io0, DIALECT_CMD, 0, cfg.range_start * (U64)ss,
                        buf->data, len, g_changenum);
            op_do_sync(io0, SUBMIT_DOIO, &res);

            if (res.err == 0 && res.actual == len) {
                pin_check("maxxfer-over", 1);   /* honoured */
            } else if (res.err == 0 && res.actual > 0 && res.actual < len) {
                out_task_printf("devsoak: matrix: FAIL maxtransfer: silent "
                                 "partial, err 0 actual %ld of %ld",
                                 (LONG)res.actual, (LONG)len);
                matrix_fail("maxtransfer");
            } else {
                pin_check("maxxfer-over", 0);    /* rejected (cleanly or not) */
            }
        }
    }

    /* 13: unlisted-nocmd (+ NSD cross-check for these five) */
    if (!test_skip_check("unlisted-nocmd")) {
        static const UWORD cmds5[5] = {
            CMD_INVALID, TD_GETDRIVETYPE, TD_GETNUMTRACKS, TD_RAWREAD,
            TD_RAWWRITE
        };
        ULONG k;

        for (k = 0; k < 5; k++) {
            UWORD cmd = cmds5[k];
            struct Result res;
            UBYTE listed;

            if (quirk_cmd_skipped(cmd))
                continue;

            op_simple(io0, cmd, &res);
            listed = dev.have_nsd && nsd_list_has(cmd);

            if (cmd == CMD_INVALID) {
                /* command 0 must never be dispatched */
                if (res.err == 0) {
                    out_task_printf("devsoak: matrix: FAIL unlisted-nocmd: "
                                     "%s succeeded (expected IOERR_NOCMD)",
                                     op_cmd_name(cmd));
                    matrix_fail("unlisted-nocmd");
                }
            } else {
                /* §15 expects NOCMD for RAWREAD/RAWWRITE on non-floppy
                 * drivers, but trackdisk.device itself implements them
                 * natively -- pinned, like GETDRIVETYPE/GETNUMTRACKS. */
                pin_check(cmd == TD_GETDRIVETYPE ? "drivetype-err"
                          : cmd == TD_GETNUMTRACKS ? "numtracks-err"
                          : cmd == TD_RAWREAD ? "rawread-err"
                                               : "rawwrite-err",
                          (LONG)res.err);
            }

            if (dev.have_nsd) {
                if (listed && res.err == IOERR_NOCMD) {
                    out_task_printf("devsoak: matrix: FAIL unlisted-nocmd: "
                                     "%s listed but NOCMD", op_cmd_name(cmd));
                    matrix_fail("unlisted-nocmd");
                } else if (!listed && res.err == 0) {
                    out_task_printf("devsoak: matrix: FAIL unlisted-nocmd: "
                                     "%s not listed but implemented",
                                     op_cmd_name(cmd));
                    matrix_fail("unlisted-nocmd");
                }
            }
        }
    }

    /* 14: hk-benign */
    if (!test_skip_check("hk-benign")) {
        struct Result res;

        if (!quirk_cmd_skipped(CMD_UPDATE)) {
            op_simple(io0, CMD_UPDATE, &res);
            if (res.err != 0 && res.err != IOERR_NOCMD) {
                out_task_printf("devsoak: matrix: FAIL hk-benign: CMD_UPDATE "
                                 "err %ld", (LONG)res.err);
                matrix_fail("hk-benign");
            } else {
                pin_check("update-err", (LONG)res.err);
            }
        }

        if (!quirk_cmd_skipped(CMD_CLEAR)) {
            op_simple(io0, CMD_CLEAR, &res);
            if (res.err != 0 && res.err != IOERR_NOCMD) {
                out_task_printf("devsoak: matrix: FAIL hk-benign: CMD_CLEAR "
                                 "err %ld", (LONG)res.err);
                matrix_fail("hk-benign");
            } else {
                pin_check("clear-err", (LONG)res.err);
            }
        }
    }

    /* 15: etd-stale */
    if (dialect_enabled(DIALECT_ETD) && !test_skip_check("etd-stale")) {
        struct Result res;
        ULONG stale = (g_changenum == 0) ? 0xFFFFFFFFUL : g_changenum - 1;

        op_build_rw(io0, DIALECT_ETD, 0, cfg.range_start * (U64)ss,
                    buf->data, ss, stale);
        op_do_sync(io0, SUBMIT_DOIO, &res);
        if (res.err == 0) {
            out_task_printf("devsoak: matrix: FAIL etd-stale: stale ETD "
                             "count accepted");
            matrix_fail("etd-stale");
        } else {
            pin_check("etd-stale-err", (LONG)res.err);
        }
    }

    /* 16: nsd-undersized */
    if (dev.have_nsd && !test_skip_check("nsd-undersized")) {
        UBYTE overrun = 0;
        ULONG i;

        buf_prefill(buf, 0xA5);
        io0->iotd_Req.io_Command = NSCMD_DEVICEQUERY;
        io0->iotd_Req.io_Data = buf->data;
        io0->iotd_Req.io_Length = 8;
        io0->iotd_Req.io_Offset = 0;
        io0->iotd_Req.io_Flags = 0;
        io0->iotd_Req.io_Actual = 0xa5a5a5a5UL;
        io0->iotd_Req.io_Error = (BYTE)0xa5;
        DoIO((struct IORequest *)io0);

        if (buf_check_guards(buf) != 0)
            overrun = 1;
        for (i = 8; !overrun && i < buf->len && i < 64; i++) {
            if (buf->data[i] != 0xA5)
                overrun = 1;
        }

        if (overrun) {
            out_task_printf("devsoak: matrix: FAIL nsd-undersized: overran "
                             "undersized buffer");
            matrix_fail("nsd-undersized");
        } else {
            pin_check("nsd-undersized-err",
                      (LONG)(BYTE)io0->iotd_Req.io_Error);
        }
    }
}

/* ---- §8 "-B": 4 GB boundary tests (tests 26/27) ----------------------- */

/* test 26: straddle-4gb-<dialect>. One dialect table entry per 64-bit
 * dialect; only entries whose dialect is in g_enabled_dialects are tried. */
struct Straddle4gEntry {
    ULONG       dialect;
    UWORD       cmd_write;
    UWORD       cmd_read;
    const char *name;
};

static const struct Straddle4gEntry s4g_table[3] = {
    { DIALECT_TD64,     TD_WRITE64,        TD_READ64,        "straddle-4gb-td64" },
    { DIALECT_NSD64,    NSCMD_TD_WRITE64,  NSCMD_TD_READ64,  "straddle-4gb-nsd64" },
    { DIALECT_NSDETD64, NSCMD_ETD_WRITE64, NSCMD_ETD_READ64, "straddle-4gb-nsdetd64" },
};

static void
run_test26_straddle4gb(struct IOExtTD *io0, struct TestBuf *buf,
                        struct timerequest *tio)
{
    ULONG ss = dev.sector_size;
    U64   boundary_sector = 0x100000000ULL / (U64)ss;
    U64   start_sector;
    ULONG idx;
    ULONG d;
    UBYTE straddles;
    UBYTE inrange;

    straddles = (cfg.range_start < boundary_sector) &&
                ((cfg.range_start + cfg.range_len) > boundary_sector);

    start_sector = boundary_sector - 2;
    inrange = straddles &&
              (start_sector >= cfg.range_start) &&
              ((start_sector + 4) <= (cfg.range_start + cfg.range_len));

    if (!inrange) {
        if (!bigdev_norange_notice) {
            out_task_printf("devsoak: matrix: range does not straddle the "
                             "4 GB boundary, straddle-4gb tests skipped "
                             "(place -r across it)");
            bigdev_norange_notice = 1;
        }
        return;
    }

    idx = (ULONG)(start_sector - cfg.range_start);

    for (d = 0; d < 3; d++) {
        const struct Straddle4gEntry *e = &s4g_table[d];
        struct Result res;
        U64 off;
        ULONG k;
        UBYTE writeok;

        if (test_skip_check(e->name))
            continue;
        if (!dialect_enabled(e->dialect))
            continue;

        off = start_sector * (U64)ss;

        breadcrumb(tio, e->cmd_write, 4 * ss, off, buf->data);

        if (!stripes_attempt(idx, 4))
            continue;   /* worker traffic holds these stripes -- try next pass */

        for (k = 0; k < 4; k++) {
            content_build(buf->data + k * ss, start_sector + k,
                          (ULONG)g_generation[idx + k] + 1, 0, 4 * ss);
        }
        op_build_rw(io0, e->dialect, 1, off, buf->data, 4 * ss, g_changenum);
        op_do_sync(io0, SUBMIT_DOIO, &res);

        writeok = (res.err == 0 && res.actual == 4 * ss);

        if (res.err != 0) {
            out_task_printf("devsoak: matrix: FAIL %s: straddling write "
                             "failed err %ld", e->name, (LONG)res.err);
            matrix_fail(e->name);
        } else if (res.actual != 4 * ss) {
            out_task_printf("devsoak: matrix: FAIL %s: short straddling "
                             "write, actual %ld of %ld", e->name,
                             (LONG)res.actual, (LONG)(4 * ss));
            matrix_fail(e->name);
        }

        if (writeok) {
            struct Result rres;
            UBYTE failed = 0;

            for (k = 0; k < 4; k++)
                g_generation[idx + k]++;

            breadcrumb(tio, e->cmd_read, 4 * ss, off, buf->data);
            buf_prefill(buf, 0xA5);
            op_build_rw(io0, e->dialect, 0, off, buf->data, 4 * ss,
                        g_changenum);
            op_do_sync(io0, SUBMIT_DOIO, &rres);

            if (rres.err != 0) {
                out_task_printf("devsoak: matrix: FAIL %s: readback failed "
                                 "err %ld", e->name, (LONG)rres.err);
                matrix_fail(e->name);
                failed = 1;
            } else if (rres.actual != 4 * ss) {
                out_task_printf("devsoak: matrix: FAIL %s: short readback, "
                                 "actual %ld of %ld", e->name,
                                 (LONG)rres.actual, (LONG)(4 * ss));
                matrix_fail(e->name);
                failed = 1;
            }

            for (k = 0; !failed && k < 4; k++) {
                LONG  cv;
                ULONG diff = 0;

                cv = content_verify(buf->data + k * ss, start_sector + k,
                                     g_generation[idx + k], &diff);
                if (cv != CV_OK) {
                    if (cv == CV_WRONG_SECTOR) {
                        out_task_printf("devsoak: matrix: FAIL %s: sector "
                                         "%ld verify %s at +%ld (high word "
                                         "ignored across 4 GB?)", e->name,
                                         (LONG)k, content_class_name(cv),
                                         (LONG)diff);
                    } else {
                        out_task_printf("devsoak: matrix: FAIL %s: sector "
                                         "%ld verify %s at +%ld", e->name,
                                         (LONG)k, content_class_name(cv),
                                         (LONG)diff);
                    }
                    matrix_fail(e->name);
                    break;
                }
            }
        }

        stripes_release(idx, 4);
    }
}

/* test 27: cmd-read-4gb-wrap. A plain 32-bit CMD_READ whose io_Offset +
 * io_Length crosses 2^32 -- the transfer cannot be expressed without
 * wrapping, so the driver must fail it rather than clamp silently past the
 * boundary or (the classic bug) wrap the offset back to a low LBA. */
static void
run_test27_cmdread4gbwrap(struct IOExtTD *io0, struct TestBuf *buf,
                           struct timerequest *tio)
{
    ULONG ss = dev.sector_size;
    ULONG len = 4 * ss;
    U64   off;
    struct Result res;

    if (test_skip_check("cmd-read-4gb-wrap"))
        return;

    /* 0x100000000 - 2*ss is already sector aligned since ss divides
       0x100000000 (ss is a power of two <= 4096) and 2*ss is a multiple
       of ss. Cast to ULONG (op_build_rw does this) wraps io_Offset to
       0xFFFFFFFF - 2*ss + 1, i.e. the last two sectors below 4 GB; the
       4-sector length then runs 2 sectors past the 32-bit wrap point. */
    off = 0x100000000ULL - (U64)len / 2;

    breadcrumb(tio, CMD_READ, len, off, buf->data);
    buf_prefill(buf, 0xA5);
    /* Hand-built: op_build_rw() would auto-upgrade a >4GB CMD transfer
     * to a 64-bit dialect, but this test exists to see what the RAW
     * 32-bit command does at the boundary. */
    io0->iotd_Req.io_Command = CMD_READ;
    io0->iotd_Req.io_Data = buf->data;
    io0->iotd_Req.io_Length = len;
    io0->iotd_Req.io_Offset = (ULONG)off;
    io0->iotd_Req.io_Flags = 0;
    io0->iotd_Req.io_Actual = 0xa5a5a5a5UL;
    io0->iotd_Req.io_Error = (BYTE)0xa5;
    io0->iotd_Count = 0;
    op_do_sync(io0, SUBMIT_DOIO, &res);

    if (res.err != 0) {
        pin_check("cmd-read-4gb-err", res.err);
        return;
    }

    if (res.actual <= len / 2) {
        /* clamped cleanly at the boundary */
        pin_check("cmd-read-4gb-err", 0);
        pin_check("cmd-read-4gb-clamped", 1);
        return;
    }

    if (res.actual == len) {
        struct SectorHdr hdr;
        UBYTE *last2 = buf->data + (len / 2);
        ULONG  calc_check;

        content_decode_hdr(last2, &hdr);
        calc_check = hdr.magic ^ hdr.seed ^ hdr.sector_hi ^ hdr.sector_lo
                   ^ hdr.generation ^ hdr.writer ^ hdr.xfer_len;

        if (hdr.magic == DSOK_MAGIC && hdr.seed == cfg.seed &&
            hdr.hdr_check == calc_check &&
            (((U64)hdr.sector_hi << 32) | (U64)hdr.sector_lo) < 4ULL) {
            /* the last two sectors of this "4 GB+" transfer decode as our
             * own header for a sector < 4 -- the driver wrapped the 32-bit
             * offset back to LBA 0 instead of failing. Classic ignored-
             * high-word bug, just seen from the CMD_READ side. */
            out_task_printf("devsoak: matrix: FAIL cmd-read-4gb-wrap: "
                             "32-bit read wrapped past 4 GB to LBA 0");
            matrix_fail("cmd-read-4gb-wrap");
        } else {
            /* Transferred 4 sectors and the tail doesn't decode as our
             * data at LBA 0..3 -- whatever the driver did (serviced a
             * genuinely different range, returned stale/foreign data,
             * whatever), it isn't the wrap bug this test hunts for. That
             * behaviour is still pinnable: a driver that is consistently
             * "wrong but not wrapping" every pass is not a regression,
             * only a driver that starts wrapping later is. */
            pin_check("cmd-read-4gb-err", 0);
        }
        return;
    }

    /* err 0 but actual is neither a clean clamp nor the full 4 sectors:
       a silent partial transfer across the boundary is its own bug. */
    out_task_printf("devsoak: matrix: FAIL cmd-read-4gb-wrap: err 0 but "
                     "actual %ld (neither clamped nor full)", (LONG)res.actual);
    matrix_fail("cmd-read-4gb-wrap");
}

/* ---- tier 2: 64-bit and probing edges --------------------------------- */

static void
run_tier2(struct IOExtTD *io0, struct IOExtTD *io1, struct TestBuf *buf,
          struct timerequest *tio)
{
    ULONG ss = dev.sector_size;

    /* 17: high-word-garbage */
    if (!test_skip_check("high-word-garbage")) {
        ULONG di;

        for (di = 0; di < g_n_enabled; di++) {
            ULONG dialect = g_enabled_dialects[di];
            UWORD cmd;
            U64   off;
            struct Result res;

            if (dialect != DIALECT_TD64 && dialect != DIALECT_NSD64)
                continue;

            cmd = (dialect == DIALECT_TD64) ? TD_READ64 : NSCMD_TD_READ64;
            if (quirk_cmd_skipped(cmd))
                continue;
            off = cfg.range_start * (U64)ss;

            breadcrumb(tio, cmd, ss, off, buf->data);
            op_build_rw(io0, dialect, 0, off, buf->data, ss, g_changenum);
            io0->iotd_Req.io_Actual = 0xDEADBEEFUL;   /* poison high word */
            op_do_sync(io0, SUBMIT_DOIO, &res);

            if (res.err == 0) {
                out_task_printf("devsoak: matrix: FAIL high-word-garbage: "
                                 "%s accepted garbage high word (0x%lx)",
                                 op_cmd_name(cmd), 0xDEADBEEFUL);
                matrix_fail("high-word-garbage");
            } else {
                pin_check(dialect == DIALECT_TD64 ? "td64-hiword-err"
                                                   : "nsd64-hiword-err",
                          (LONG)res.err);
            }
        }
    }

    /* 18: hi-word-past-end -- only on a device known to be < 4 GB */
    if (dev.have_geom &&
        (dev.total_sectors * (U64)ss) < 0x100000000ULL &&
        !test_skip_check("hi-word-past-end")) {
        ULONG di;

        for (di = 0; di < g_n_enabled; di++) {
            ULONG dialect = g_enabled_dialects[di];
            UWORD cmd;
            U64   off;
            struct Result res;

            if (dialect != DIALECT_TD64 && dialect != DIALECT_NSD64)
                continue;

            cmd = (dialect == DIALECT_TD64) ? TD_READ64 : NSCMD_TD_READ64;
            if (quirk_cmd_skipped(cmd))
                continue;
            off = 0x100000000ULL + cfg.range_start * (U64)ss;

            breadcrumb(tio, cmd, ss, off, buf->data);
            op_build_rw(io0, dialect, 0, off, buf->data, ss, g_changenum);
            op_do_sync(io0, SUBMIT_DOIO, &res);

            if (res.err == 0) {
                out_task_printf("devsoak: matrix: FAIL hi-word-past-end: "
                                 "%s accepted a 4GB+ offset on a sub-4GB "
                                 "device", op_cmd_name(cmd));
                matrix_fail("hi-word-past-end");
            }
        }
    }

    /* 19: stop-start. Gated on cfg.risky: CMD_STOP can physically stop or
     * (on removable media) eject/spin down a drive we know nothing about
     * yet, so it only runs under -Z in this implementation -- see the
     * final report for the "pinned-working" simplification taken here
     * (this codebase has no cross-run quirks/pin persistence to fall back
     * on within a single process, so "already known safe" can only ever
     * mean "we are in a -Z run and it worked earlier this run", which
     * cfg.risky already covers). */
    if (cfg.risky && !stop_skip && !test_skip_check("stop-start")) {
        struct Result sres;

        if (quirk_cmd_skipped(CMD_STOP)) {
            stop_skip = 1;
        } else {
            breadcrumb(tio, CMD_STOP, 0, 0, NULL);
            op_simple(io0, CMD_STOP, &sres);

            if (sres.err == IOERR_NOCMD) {
                pin_check("stop-err", (LONG)IOERR_NOCMD);
                stop_skip = 1;
            } else if (sres.err != 0) {
                out_task_printf("devsoak: matrix: FAIL stop-start: CMD_STOP "
                                 "err %ld", (LONG)sres.err);
                matrix_fail("stop-start");
            } else {
                pin_check("stop-err", 0);

                if (stripes_attempt(0, 1)) {
                    U64  off = cfg.range_start * (U64)ss;
                    UBYTE done_before_start;
                    struct Result rres;

                    op_build_rw(io1, DIALECT_CMD, 0, off, buf->data, ss,
                                g_changenum);
                    SendIO((struct IORequest *)io1);
                    done_before_start =
                        (CheckIO((struct IORequest *)io1) != NULL);
                    pin_check("stop-gates", done_before_start ? 0 : 1);

                    if (!quirk_cmd_skipped(CMD_START)) {
                        breadcrumb(tio, CMD_START, 0, 0, NULL);
                        op_simple(io0, CMD_START, &sres);
                        if (sres.err != 0) {
                            out_task_printf("devsoak: matrix: FAIL "
                                            "stop-start: CMD_START err %ld",
                                            (LONG)sres.err);
                            matrix_fail("stop-start");
                        }
                    }

                    WaitIO((struct IORequest *)io1);
                    rres.err = (LONG)(BYTE)io1->iotd_Req.io_Error;
                    rres.actual = io1->iotd_Req.io_Actual;
                    if (rres.err != 0) {
                        out_task_printf("devsoak: matrix: FAIL stop-start: "
                                        "read after CMD_START err %ld",
                                        (LONG)rres.err);
                        matrix_fail("stop-start");
                    }

                    stripes_release(0, 1);
                }
            }
        }
    }

    /* 20: motor */
    if (!motor_skip && !test_skip_check("motor")) {
      if (quirk_cmd_skipped(TD_MOTOR)) {
        motor_skip = 1;
      } else {
        LONG errcode;

        breadcrumb(tio, TD_MOTOR, 0, 0, NULL);
        io0->iotd_Req.io_Command = TD_MOTOR;
        io0->iotd_Req.io_Data = NULL;
        io0->iotd_Req.io_Length = 1;      /* motor on */
        io0->iotd_Req.io_Offset = 0;
        io0->iotd_Req.io_Flags = 0;
        io0->iotd_Req.io_Actual = 0xa5a5a5a5UL;
        io0->iotd_Req.io_Error = (BYTE)0xa5;
        DoIO((struct IORequest *)io0);

        errcode = (LONG)(BYTE)io0->iotd_Req.io_Error;
        pin_check("motor-err", errcode);

        if (errcode == IOERR_NOCMD) {
            motor_skip = 1;
        } else if (errcode != 0) {
            out_task_printf("devsoak: matrix: FAIL motor: TD_MOTOR(on) "
                             "err %ld", (LONG)errcode);
            matrix_fail("motor");
        } else {
            ULONG prevstate = io0->iotd_Req.io_Actual;

            io0->iotd_Req.io_Length = 0;   /* motor off */
            io0->iotd_Req.io_Actual = 0xa5a5a5a5UL;
            io0->iotd_Req.io_Error = (BYTE)0xa5;
            DoIO((struct IORequest *)io0);

            if ((LONG)(BYTE)io0->iotd_Req.io_Error != 0) {
                out_task_printf("devsoak: matrix: FAIL motor: TD_MOTOR(off) "
                                 "err %ld",
                                 (LONG)(BYTE)io0->iotd_Req.io_Error);
                matrix_fail("motor");
            } else {
                /* The brief expects io_Actual = previous motor state (1
                 * here, we just turned it on), but scsi.device 47.4 stubs
                 * motor tracking and always answers 0 -- pin whatever the
                 * driver does; a change during the run is a violation. */
                pin_check("motor-off-actual",
                          (LONG)io0->iotd_Req.io_Actual);
            }
            (void)prevstate;

            /* restore motor on, harmless on non-floppy drivers */
            io0->iotd_Req.io_Length = 1;
            io0->iotd_Req.io_Actual = 0xa5a5a5a5UL;
            io0->iotd_Req.io_Error = (BYTE)0xa5;
            DoIO((struct IORequest *)io0);
        }
      }
    }

    /* 26/27: -B big-device 4 GB boundary tests. Gated on the device
     * actually being >= 4 GB (by geometry, not by the -r range) -- the
     * option can be given against a small test unit for a scripted CI run
     * that also exercises other units, so this must degrade to a one-time
     * notice rather than a hard failure. */
    if (cfg.bigdev) {
        UBYTE big_enough = dev.have_geom &&
            (dev.total_sectors * (U64)ss) > 0x100000000ULL;

        if (!big_enough) {
            if (!bigdev_toosmall_notice) {
                out_task_printf("devsoak: matrix: -B given but device < "
                                 "4 GB, boundary tests skipped");
                bigdev_toosmall_notice = 1;
            }
        } else {
            run_test26_straddle4gb(io0, buf, tio);
            run_test27_cmdread4gbwrap(io0, buf, tio);
        }
    }
}

/* ---- tier 3: risky (-Z only) ------------------------------------------ */

static void
run_tier3(struct IOExtTD *io0, struct TestBuf *buf, struct timerequest *tio)
{
    ULONG ss = dev.sector_size;
    static ULONG rng;

    /* 21: random-cmd. Deliberately NOT run through breadcrumb()'s
     * "already announced" table: the command number is different almost
     * every pass (65 279 possibilities), so tracking it would either miss
     * the point (a command we send once is, in practice, always its own
     * "first issue") or grow the breadcrumb table unboundedly. Announce
     * every issue instead -- the whole point of §16.4 is "what was in
     * flight when it died", and for a command chosen at random that is
     * every issue, not just the first. */
    if (!quirk_norandomcmd() && !test_skip_check("random-cmd")) {
        UWORD cmd;
        struct Result res;
        char offbuf[24];

        if (rng == 0)
            rng = XS32_SEED(cfg.seed ^ 0xC0FFEEUL);
        cmd = (UWORD)(0x0100UL + (xs32(&rng) % (0x7FFFUL - 0x0100UL + 1UL)));

        u64_to_str(0, offbuf);
        out_serial_line("devsoak: about to send %s(0x%lx) len=%ld off=%s data=0x%lx",
                         op_cmd_name(cmd), (ULONG)cmd, 0L, offbuf, 0UL);

        op_simple(io0, cmd, &res);
        if (res.err == 0) {
            out_task_printf("devsoak: matrix: FAIL random-cmd: cmd 0x%lx "
                             "succeeded (expected IOERR_NOCMD)", (ULONG)cmd);
            matrix_fail("random-cmd");
        }
        /* any nonzero err is tolerated -- no pin, too many possible codes */
    }

    /* 22: unlisted-64 */
    if (!test_skip_check("unlisted-64")) {
        static const ULONG all64[3] = {
            DIALECT_TD64, DIALECT_NSD64, DIALECT_NSDETD64
        };
        ULONG d;

        for (d = 0; d < 3; d++) {
            ULONG dialect = all64[d];
            UWORD cmd;
            U64   off;
            struct Result res;

            if (dialect_enabled(dialect))
                continue;

            cmd = (dialect == DIALECT_TD64) ? TD_READ64
                : (dialect == DIALECT_NSD64) ? NSCMD_TD_READ64
                                              : NSCMD_ETD_READ64;
            if (quirk_cmd_skipped(cmd))
                continue;
            off = cfg.range_start * (U64)ss;

            breadcrumb(tio, cmd, ss, off, buf->data);
            op_build_rw(io0, dialect, 0, off, buf->data, ss, g_changenum);
            op_do_sync(io0, SUBMIT_DOIO, &res);

            if (res.err == 0) {
                pin_check("unlisted64-works", 1);
                out_task_printf("devsoak: matrix: WARN unlisted-64: %s "
                                 "works without being advertised",
                                 op_cmd_name(cmd));
            }
            /* any nonzero err tolerated */
        }
    }

    /* 23: format (+ FORMAT64/NSCMD_TD_FORMAT64 if enabled).
     * TD_FORMAT's io_Offset/io_Length must be whole-track aligned and
     * sized (lide.device 40.12 rejects a single-sector format with
     * IOERR_BADADDRESS, correctly) -- format exactly one track, at the
     * first track boundary inside the range. */
    if (!test_skip_check("format")) {
        ULONG tsects = (dev.have_geom && dev.geom.dg_TrackSectors != 0)
                       ? dev.geom.dg_TrackSectors : 1;
        ULONG tsize = tsects * ss;
        U64   ta = ((cfg.range_start + tsects - 1) / tsects) * tsects;
        ULONG idx0 = (ULONG)(ta - cfg.range_start);
        UBYTE viable = (tsize <= buf->len) &&
                       (ta + tsects <= cfg.range_start + cfg.range_len);
        U64   off = ta * (U64)ss;
        ULONG d;

        for (d = 0; d < 3 && viable; d++) {
            ULONG dialect;
            UWORD cmd;
            const char *pname;

            if (d == 0) {
                dialect = DIALECT_CMD;  /* plain TD_FORMAT: always tried */
                cmd = TD_FORMAT;
                pname = "format-err";
            } else if (d == 1) {
                dialect = DIALECT_TD64;
                cmd = TD_FORMAT64;
                pname = "format64-err";
            } else {
                dialect = DIALECT_NSD64;
                cmd = NSCMD_TD_FORMAT64;
                pname = "nsdformat64-err";
            }
            if (d > 0 && !dialect_enabled(dialect))
                continue;
            if (quirk_cmd_skipped(cmd))
                continue;

            breadcrumb(tio, cmd, tsize, off, buf->data);
            if (stripes_attempt(idx0, tsects)) {
                struct Result res;
                ULONG k;

                for (k = 0; k < tsects; k++) {
                    content_build(buf->data + k * ss, ta + k,
                                  (ULONG)g_generation[idx0 + k] + 1, 0,
                                  tsize);
                }
                io0->iotd_Req.io_Command = cmd;
                io0->iotd_Req.io_Data = buf->data;
                io0->iotd_Req.io_Length = tsize;
                io0->iotd_Req.io_Offset = (ULONG)off;
                io0->iotd_Req.io_Actual = (d == 0) ? 0xa5a5a5a5UL
                                                    : (ULONG)(off >> 32);
                io0->iotd_Req.io_Flags = 0;
                io0->iotd_Req.io_Error = (BYTE)0xa5;
                DoIO((struct IORequest *)io0);

                res.err = (LONG)(BYTE)io0->iotd_Req.io_Error;
                res.actual = io0->iotd_Req.io_Actual;

                if (res.err == 0 && res.actual == tsize) {
                    for (k = 0; k < tsects; k++)
                        g_generation[idx0 + k]++;
                    pin_check(pname, 0);
                } else if (res.err == 0) {
                    out_task_printf("devsoak: matrix: FAIL format: %s "
                                     "silent partial, err 0 actual %ld "
                                     "of %ld", op_cmd_name(cmd),
                                     (LONG)res.actual, (LONG)tsize);
                    matrix_fail("format");
                } else {
                    /* any error code is a pinned behaviour: lide.device
                     * 40.12 rejects a valid plain TD_FORMAT with
                     * IOERR_BADADDRESS while accepting the same track
                     * via TD_FORMAT64/NSCMD_TD_FORMAT64. */
                    pin_check(pname, (LONG)res.err);
                }
                stripes_release(idx0, tsects);
            }
        }
    }
}

/* ---- lifecycle (first pass + every ~30 min) --------------------------- */

static void
run_lifecycle(struct IOExtTD *scratch)
{
    LONG err;

    /* 24: open-bad-unit */
    if (!test_skip_check("open-bad-unit")) {
        err = OpenDevice((CONST_STRPTR)cfg.device, 999,
                         (struct IORequest *)scratch, 0);
        if (err == 0) {
            out_task_printf("devsoak: matrix: FAIL open-bad-unit: "
                             "OpenDevice(unit 999) succeeded");
            matrix_fail("open-bad-unit");
            CloseDevice((struct IORequest *)scratch);
        } else {
            pin_check("bad-unit-err", (LONG)scratch->iotd_Req.io_Error);
        }
    }

    /* 25: extra-open */
    if (!test_skip_check("extra-open")) {
        err = OpenDevice((CONST_STRPTR)cfg.device, cfg.unit,
                         (struct IORequest *)scratch, 0);
        if (err != 0) {
            out_task_printf("devsoak: matrix: FAIL extra-open: "
                             "OpenDevice(unit %ld) failed, err %ld",
                             (LONG)cfg.unit,
                             (LONG)scratch->iotd_Req.io_Error);
            matrix_fail("extra-open");
        } else {
            CloseDevice((struct IORequest *)scratch);
        }
    }
}

/* ---- task entry -------------------------------------------------------- */

static void
inv_sleep_1s(struct timerequest *tio)
{
    tio->tr_node.io_Command = TR_ADDREQUEST;
    tio->tr_node.io_Flags = 0;
    tio->tr_time.tv_secs = 1;
    tio->tr_time.tv_micro = 0;
    DoIO((struct IORequest *)tio);
}

static void
inv_entry(void)
{
    struct MsgPort *port = NULL;
    struct IOExtTD *io0 = NULL, *io1 = NULL, *scratch = NULL;
    struct MsgPort *tport = NULL;
    struct timerequest *tio = NULL;
    UBYTE  topen = 0;
    struct TestBuf buf;
    ULONG  ss = dev.sector_size;
    ULONG  want;
    UBYTE  have_buf = 0;
    ULONG  elapsed_since_lifecycle = 0;
    UBYTE  maxxfer_skip_reported = 0;

    buf.base = NULL;

    port = CreatePort(NULL, 0);
    if (port != NULL) {
        io0 = (struct IOExtTD *)CreateExtIO(port, sizeof(struct IOExtTD));
        io1 = (struct IOExtTD *)CreateExtIO(port, sizeof(struct IOExtTD));
        scratch = (struct IOExtTD *)CreateExtIO(port, sizeof(struct IOExtTD));
    }

    if (port == NULL || io0 == NULL || io1 == NULL || scratch == NULL) {
        out_task_printf("devsoak: matrix: port/request allocation failed, "
                         "invariant task exiting");
        if (io0 != NULL) DeleteExtIO((struct IORequest *)io0);
        if (io1 != NULL) DeleteExtIO((struct IORequest *)io1);
        if (scratch != NULL) DeleteExtIO((struct IORequest *)scratch);
        if (port != NULL) DeletePort(port);
        Signal(hs_main_task, 1UL << hs_signum);
        inv_done = 1;
        return;
    }

    /* standard multi-request idiom: clone the already-open device/unit;
     * scratch is left alone -- OpenDevice() fills in its io_Device/io_Unit
     * itself, on every lifecycle call. */
    io0->iotd_Req.io_Device = dev.io->iotd_Req.io_Device;
    io0->iotd_Req.io_Unit   = dev.io->iotd_Req.io_Unit;
    io1->iotd_Req.io_Device = dev.io->iotd_Req.io_Device;
    io1->iotd_Req.io_Unit   = dev.io->iotd_Req.io_Unit;

    tport = CreatePort(NULL, 0);
    if (tport != NULL)
        tio = (struct timerequest *)CreateExtIO(tport, sizeof(struct timerequest));
    if (tio != NULL) {
        if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK,
                       (struct IORequest *)tio, 0) == 0)
            topen = 1;
    }

    if (!topen) {
        out_task_printf("devsoak: matrix: could not open timer.device, "
                         "invariant task exiting without ever running");
        if (tio != NULL) DeleteExtIO((struct IORequest *)tio);
        if (tport != NULL) DeletePort(tport);
        DeleteExtIO((struct IORequest *)io0);
        DeleteExtIO((struct IORequest *)io1);
        DeleteExtIO((struct IORequest *)scratch);
        DeletePort(port);
        Signal(hs_main_task, 1UL << hs_signum);
        inv_done = 1;
        return;
    }

    /* buffer: g_chunk_bytes + one sector (rounded up), letting test 12
     * issue a read one sector longer than the run's chunk size / MaxTransfer
     * cap. Halve toward a 4-sector floor on allocation failure; below that,
     * the whole task is unviable (every straddle/bounds test needs at
     * least 4 sectors of buffer). */
    want = g_chunk_bytes + ss;
    if (want % ss != 0)
        want += ss - (want % ss);

    for (;;) {
        if (buf_alloc(&buf, want, ALIGN_LONG, MEMF_PUBLIC) == 0) {
            have_buf = 1;
            break;
        }
        if (want <= 4 * ss)
            break;
        want /= 2;
        want -= want % ss;
        if (want < 4 * ss)
            want = 4 * ss;
    }

    if (!have_buf) {
        out_task_printf("devsoak: matrix: buffer allocation failed, "
                         "invariant task exiting");
        CloseDevice((struct IORequest *)tio);
        DeleteExtIO((struct IORequest *)tio);
        DeletePort(tport);
        DeleteExtIO((struct IORequest *)io0);
        DeleteExtIO((struct IORequest *)io1);
        DeleteExtIO((struct IORequest *)scratch);
        DeletePort(port);
        Signal(hs_main_task, 1UL << hs_signum);
        inv_done = 1;
        return;
    }

    g_can_maxtransfer = (buf.len >= g_chunk_bytes + ss);
    if (!g_can_maxtransfer && !maxxfer_skip_reported) {
        out_task_printf("devsoak: matrix: maxtransfer test skipped (no buffer)");
        maxxfer_skip_reported = 1;
    }

    /* everything is ready: tell invariant_start() we're alive */
    Signal(hs_main_task, 1UL << hs_signum);

    while (!inv_stop) {
        LONG qt = quirk_tier();     /* -1 = no override */

        run_tier0(io0, io1, &buf);
        if (inv_stop) break;
        if (qt < 0 || qt >= 1) {
            run_tier1(io0, io1, &buf);
            if (inv_stop) break;
        }
        if (qt < 0 || qt >= 2) {
            run_tier2(io0, io1, &buf, tio);
            if (inv_stop) break;
        }
        if (cfg.risky && (qt < 0 || qt >= 3))
            run_tier3(io0, &buf, tio);

        if (inv_passes == 0 || elapsed_since_lifecycle >= 1800) {
            run_lifecycle(scratch);
            elapsed_since_lifecycle = 0;
        }

        inv_passes++;

        {
            ULONG i;

            for (i = 0; i < 5 && !inv_stop; i++) {
                inv_sleep_1s(tio);
                elapsed_since_lifecycle++;
            }
        }
    }

    CloseDevice((struct IORequest *)tio);
    DeleteExtIO((struct IORequest *)tio);
    DeletePort(tport);

    buf_free(&buf);

    DeleteExtIO((struct IORequest *)io0);
    DeleteExtIO((struct IORequest *)io1);
    DeleteExtIO((struct IORequest *)scratch);
    DeletePort(port);

    inv_done = 1;
    /* falling off the end is the documented-safe way to end a
     * CreateTask() task: the internal trampoline RemTask()s us and its
     * tc_MemEntry list frees the TCB/stack automatically -- see
     * audit.c/worker.c for the same reliance already in this tree. */
}

/* ---- public API (devsoak.h) -------------------------------------------- */

LONG
invariant_start(void)
{
    hs_main_task = FindTask(NULL);
    hs_signum = (ULONG)AllocSignal(-1);
    if ((LONG)hs_signum == -1) {
        out_printf("devsoak: matrix: AllocSignal failed, invariant matrix "
                   "not started (degraded: no §8 coverage this run)");
        return 0;
    }

    inv_stop = 0;
    inv_done = 0;
    inv_errors = 0;
    inv_warnings = 0;
    inv_passes = 0;
    n_pins = 0;
    n_bc = 0;
    n_skiprep = 0;
    stop_skip = 0;
    motor_skip = 0;
    bigdev_toosmall_notice = 0;
    bigdev_norange_notice = 0;

    inv_taskptr = CreateTask((STRPTR)"devsoak.invariant", 0,
                             (APTR)inv_entry, INV_STACK);
    if (inv_taskptr == NULL) {
        FreeSignal((LONG)hs_signum);
        hs_signum = (ULONG)-1;
        out_printf("devsoak: matrix: CreateTask failed, invariant matrix "
                   "not started (degraded: no §8 coverage this run)");
        return 0;
    }

    Wait(1UL << hs_signum);
    FreeSignal((LONG)hs_signum);
    hs_signum = (ULONG)-1;

    inv_active = 1;
    out_printf("devsoak: matrix: started");
    return 0;
}

void
invariant_request_stop(void)
{
    if (inv_active)
        inv_stop = 1;
}

void
invariant_wait_done(void)
{
    if (!inv_active)
        return;

    while (!inv_done)
        timer_delay_ms(100);   /* main-task-only call, per timer.c */
}

void
invariant_cleanup(void)
{
    /* Nothing to free here: the invariant task frees its own port,
     * IOExtTDs, timer.device unit and buffer (and its own TCB/stack via
     * the default task finalizer) before setting inv_done. Idempotent by
     * construction -- there is no state left to double-free. */
    inv_active = 0;
}

ULONG
invariant_errors(void)
{
    return inv_errors;
}

ULONG
invariant_warnings(void)
{
    return inv_warnings;
}

ULONG
invariant_passes(void)
{
    return inv_passes;
}

void
invariant_print_pins(void)
{
    ULONG i;

    for (i = 0; i < n_pins; i++) {
        if (pins[i].set) {
            out_printf("devsoak: matrix: pin %s = %ld",
                       pins[i].name, (LONG)pins[i].value);
        }
    }
}
