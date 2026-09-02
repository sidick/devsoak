/*
 * devsoak - entry point (milestone M1: args, device open, geometry,
 * NSD query, confirmation). Workers/invariants/audit are later milestones.
 */

#include "devsoak.h"

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/alib.h>
#include <exec/errors.h>
#include <dos/dos.h>

/* globals declared extern in devsoak.h */
struct Config       cfg;
struct DevUnderTest dev;

static const char verstag[] __attribute__((used)) =
    "$VER: devsoak 0.1 (01.09.2026)";

/* args.c, deliberately not declared in devsoak.h (internal to this program) */
extern LONG args_parse(int argc, char **argv);
extern const char *args_error(void);
extern void args_usage(void);

static void
bcopy_str(char *dst, ULONG dstsize, const char *src)
{
    ULONG i = 0;

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    /* stop at CR/LF too: lib_IdString conventionally ends "\r\n" */
    while (src[i] != '\0' && src[i] != '\r' && src[i] != '\n'
           && i < dstsize - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* RawDoFmt has no 64-bit support; render a U64 as decimal ourselves. */
static void
u64_to_str(U64 v, char *buf)
{
    char tmp[24];
    int i = 0;
    int j;

    if (v == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    while (v > 0) {
        tmp[i++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    for (j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
}

int
main(int argc, char **argv)
{
    LONG rc = RC_CLEAN;
    struct MsgPort *port = NULL;
    struct IOExtTD *io = NULL;
    UBYTE opened = 0;
    LONG operr;
    ULONG s0, u0;
    char numbuf1[24];
    char numbuf2[24];

    if (args_parse(argc, argv) != 0) {
        out_init(OUT_CON);
        out_printf("devsoak: %s", args_error());
        args_usage();
        out_cleanup();
        return RC_FATAL;
    }

    out_init(cfg.outmode);

    if (timer_init() != 0) {
        out_printf("devsoak: timer_init failed");
        out_cleanup();
        return RC_FATAL;
    }

    /* --resume (§16.4): report the crash suspect from -P FILE, no run */
    if (cfg.resume) {
        rc = quirks_resume_report();
        timer_cleanup();
        out_cleanup();
        return rc;
    }

    if (!cfg.seed_given) {
        timer_gettime(&s0, &u0);
        cfg.seed = s0 ^ u0;
    }

    port = CreatePort(NULL, 0);
    if (port == NULL) {
        out_printf("devsoak: CreatePort failed");
        rc = RC_FATAL;
        goto cleanup_close;
    }

    io = (struct IOExtTD *)CreateExtIO(port, sizeof(struct IOExtTD));
    if (io == NULL) {
        out_printf("devsoak: CreateExtIO failed");
        rc = RC_FATAL;
        goto cleanup_close;
    }

    operr = OpenDevice((CONST_STRPTR)cfg.device, cfg.unit,
                       (struct IORequest *)io, 0);
    if (operr != 0) {
        out_printf("devsoak: OpenDevice(%s,%ld) failed, io_Error %ld",
                    cfg.device, cfg.unit, (LONG)io->iotd_Req.io_Error);
        rc = RC_FATAL;
        goto cleanup_close;
    }
    opened = 1;
    dev.port = port;
    dev.io = io;
    dev.opened = 1;

    {
        struct Library *lib = &io->iotd_Req.io_Device->dd_Library;
        bcopy_str(dev.dev_name, sizeof(dev.dev_name), lib->lib_Node.ln_Name);
        dev.dev_version = lib->lib_Version;
        dev.dev_revision = lib->lib_Revision;
        bcopy_str(dev.dev_idstring, sizeof(dev.dev_idstring),
                  (const char *)lib->lib_IdString);
    }

    /* TD_GETGEOMETRY */
    {
        io->iotd_Req.io_Command = TD_GETGEOMETRY;
        io->iotd_Req.io_Data = (APTR)&dev.geom;
        io->iotd_Req.io_Length = sizeof(struct DriveGeometry);
        io->iotd_Req.io_Offset = 0;
        io->iotd_Req.io_Flags = 0;
        DoIO((struct IORequest *)io);

        if (io->iotd_Req.io_Error == 0) {
            ULONG ss = dev.geom.dg_SectorSize;
            if (ss != 512 && ss != 1024 && ss != 2048 && ss != 4096) {
                out_printf("devsoak: unsupported sector size %ld", (LONG)ss);
                rc = RC_FATAL;
                goto cleanup_close;
            }
            dev.have_geom = 1;
            dev.sector_size = ss;
            dev.total_sectors = (U64)dev.geom.dg_TotalSectors;
        } else {
            /* Pre-V36 drivers predate TD_GETGEOMETRY entirely; they
             * answer IOERR_NOCMD or whatever their dispatcher falls
             * into (A2091 7.0: -1). Either way: no geometry, fall back
             * to the range end as the device size (§16.5). */
            dev.have_geom = 0;
            dev.sector_size = 512;
            out_printf("devsoak: warning: TD_GETGEOMETRY failed (io_Error "
                       "%ld); assuming 512-byte sectors and using the "
                       "range end as device size",
                       (LONG)(BYTE)io->iotd_Req.io_Error);
        }
    }

    /* NSCMD_DEVICEQUERY */
    {
        struct NSDQueryResult nsdq;

        nsdq.DevQueryFormat = 0;
        nsdq.SizeAvailable = 0;
        nsdq.DeviceType = 0;
        nsdq.DeviceSubType = 0;
        nsdq.SupportedCommands = NULL;

        io->iotd_Req.io_Command = NSCMD_DEVICEQUERY;
        io->iotd_Req.io_Data = (APTR)&nsdq;
        io->iotd_Req.io_Length = sizeof(nsdq);
        io->iotd_Req.io_Offset = 0;
        io->iotd_Req.io_Flags = 0;
        DoIO((struct IORequest *)io);

        if (io->iotd_Req.io_Error == 0 && nsdq.DevQueryFormat == 0 &&
            nsdq.SizeAvailable >= 16 && nsdq.SizeAvailable <= sizeof(nsdq)) {
            ULONG n = 0;

            dev.have_nsd = 1;
            dev.nsd_devtype = nsdq.DeviceType;
            if (nsdq.SupportedCommands != NULL) {
                while (n < 63 && nsdq.SupportedCommands[n] != 0) {
                    dev.nsd_cmds[n] = nsdq.SupportedCommands[n];
                    n++;
                }
            }
            dev.nsd_cmds[n] = 0;
            dev.nsd_ncmds = n;
            out_printf("devsoak: NSCMD_DEVICEQUERY: devtype=%ld, %ld commands listed",
                        (LONG)dev.nsd_devtype, (LONG)dev.nsd_ncmds);
        } else if (io->iotd_Req.io_Error == IOERR_NOCMD) {
            dev.have_nsd = 0;
            out_printf("devsoak: NSCMD_DEVICEQUERY unsupported (pre-NSD driver)");
        } else {
            dev.have_nsd = 0;
            out_printf("devsoak: NSCMD_DEVICEQUERY returned an unexpected result, io_Error %ld",
                        (LONG)io->iotd_Req.io_Error);
        }
    }

    /* quirks (§16): needs the driver identity captured above */
    if (quirks_load() != 0) {
        rc = RC_FATAL;
        goto cleanup_close;
    }

    /* validate range against device size */
    if (cfg.range_len == 0) {
        out_printf("devsoak: -r LEN must be nonzero");
        rc = RC_FATAL;
        goto cleanup_close;
    }
    if (dev.have_geom) {
        if (cfg.range_start + cfg.range_len > dev.total_sectors) {
            u64_to_str(dev.total_sectors, numbuf1);
            out_printf("devsoak: range exceeds device size (%s total sectors)", numbuf1);
            rc = RC_FATAL;
            goto cleanup_close;
        }
    } else {
        dev.total_sectors = cfg.range_start + cfg.range_len;
    }

    /* run banner */
    out_printf("devsoak: device %s unit %ld", cfg.device, cfg.unit);
    out_printf("devsoak: driver %s version %ld.%ld",
                dev.dev_name, (LONG)dev.dev_version, (LONG)dev.dev_revision);
    if (dev.dev_idstring[0] != '\0') {
        out_printf("devsoak: idstring: %s", dev.dev_idstring);
    }
    if (dev.have_geom) {
        u64_to_str(dev.total_sectors, numbuf1);
        out_printf("devsoak: geometry: sector size %ld, total sectors %s, cyl %ld, heads %ld, sec/track %ld",
                    (LONG)dev.sector_size, numbuf1,
                    (LONG)dev.geom.dg_Cylinders, (LONG)dev.geom.dg_Heads,
                    (LONG)dev.geom.dg_TrackSectors);
    } else {
        out_printf("devsoak: geometry: unavailable, assuming sector size %ld",
                    (LONG)dev.sector_size);
    }
    {
        U64 endsec = cfg.range_start + cfg.range_len;
        U64 mb = (cfg.range_len * (U64)dev.sector_size) / (1024 * 1024);
        u64_to_str(cfg.range_start, numbuf1);
        u64_to_str(endsec, numbuf2);
        out_printf("devsoak: range: sectors %s..%s", numbuf1, numbuf2);
        u64_to_str(mb, numbuf1);
        out_printf("devsoak: range size: %s MB", numbuf1);
    }
    out_printf("devsoak: seed %ld%s", (LONG)cfg.seed, cfg.seed_given ? "" : " (from clock)");
    out_printf("devsoak: workers %ld, qdepth %ld, duration %ld s",
                (LONG)cfg.workers, (LONG)cfg.qdepth, (LONG)cfg.duration_s);

    /* confirmation (skipped with -y) */
    if (!cfg.yes) {
        if ((SetSignal(0, 0) & SIGBREAKF_CTRL_C) != 0) {
            out_printf("devsoak: aborted (break)");
            rc = RC_FATAL;
            goto cleanup_close;
        }

        out_printf("This will DESTROY data in the above range. Continue? (y/N)");
        {
            BPTR cin = Input();
            char ansbuf[8];
            LONG n = Read(cin, ansbuf, (LONG)sizeof(ansbuf) - 1);
            UBYTE ans = 0;

            if (n > 0) ans = (UBYTE)ansbuf[0];
            if (ans != 'y' && ans != 'Y') {
                out_printf("devsoak: aborted");
                rc = RC_FATAL;
                goto cleanup_close;
            }
        }
    }

    if (crumb_open() != 0) {
        out_printf("devsoak: cannot open -P file %s", cfg.crumbfile);
        rc = RC_FATAL;
        goto cleanup_close;
    }

    rc = engine_run();

    crumb_close();
    quirks_cleanup();

cleanup_close:
    if (opened) {
        CloseDevice((struct IORequest *)io);
        opened = 0;
        dev.opened = 0;

        /* §8 lifecycle: after everything has closed, the device must
         * open again (catches wrong open counts / premature expunge).
         * Only meaningful after a full run, but harmless otherwise. */
        if (OpenDevice((CONST_STRPTR)cfg.device, cfg.unit,
                       (struct IORequest *)io, 0) == 0) {
            CloseDevice((struct IORequest *)io);
            out_printf("devsoak: lifecycle: reopen after close ok");
        } else {
            out_printf("devsoak: lifecycle: REOPEN AFTER CLOSE FAILED, "
                       "io_Error %ld (wrong open count / premature "
                       "expunge?)", (LONG)io->iotd_Req.io_Error);
            if (rc == RC_CLEAN || rc == RC_WARN) {
                rc = RC_ERROR;
                /* engine_run() already printed its RESULT line; issue a
                 * corrected final verdict so CI grepping the last RESULT
                 * sees the failure */
                out_printf("devsoak: RESULT FAIL rc=%ld (lifecycle)",
                           (LONG)rc);
            }
        }
    }
    if (io != NULL) DeleteExtIO((struct IORequest *)io);
    if (port != NULL) DeletePort(port);
    timer_cleanup();
    out_cleanup();
    return rc;
}
