/*
 * devsoak - entry point (milestone M1: args, device open, geometry,
 * NSD query, confirmation). Workers/invariants/audit are later milestones.
 */

#include "devsoak.h"
#include "version.h"

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/alib.h>
#include <exec/errors.h>
#include <dos/dos.h>
#include <string.h>

/* globals declared extern in devsoak.h */
struct Config       cfg;
struct DevUnderTest dev;

DEVSOAK_VERSTAG

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

/* case-insensitive compare (no strcasecmp on this toolchain; same pattern
 * as quirks.c's ci_eq()) -- used only for the partition-mode volume-name
 * confirmation below. */
static int
main_tolower(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    return c;
}

static UBYTE
main_ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (main_tolower((unsigned char)*a) != main_tolower((unsigned char)*b))
            return 0;
        a++; b++;
    }
    return (*a == '\0' && *b == '\0') ? 1 : 0;
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
    /* partition mode (§ dosdev.c) */
    struct PartInfo pinfo;
    UBYTE have_partinfo = 0;
    UBYTE inhibited = 0;
    UBYTE have_sig = 0;
    char  signame[8];
    U64   part_boot_lba = 0;

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

    /* partition mode: resolve NAME: to the exec device/unit/geometry it
     * is mounted on before opening anything. dosdev_resolve() prints its
     * own one-line failure reason. */
    if (cfg.partition) {
        if (dosdev_resolve(cfg.dosdev, &pinfo) != 0) {
            rc = RC_FATAL;
            goto cleanup_close;
        }
        have_partinfo = 1;
        cfg.device = pinfo.devname;
        cfg.unit = pinfo.unit;
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
                       (struct IORequest *)io,
                       cfg.partition ? pinfo.opendevice_flags : 0);
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

    /* partition mode: turn the mounted partition's extent (envec blocks,
     * de_LowCyl/de_HighCyl) into a device-sector range, exactly as -r
     * would express it, absolute from LBA 0. envec blocks are
     * de_SizeBlock*4 bytes; devsoak works in dev.sector_size units, so a
     * clean integer scale between the two is required. */
    if (cfg.partition) {
        ULONG envec_bytes = pinfo.sizeblock * 4;
        ULONG scale;
        U64   blocks_per_cyl, part_start, part_len;

        if (envec_bytes == 0 || dev.sector_size == 0 ||
            envec_bytes % dev.sector_size != 0) {
            out_printf("devsoak: %s: envec block size %ld bytes is not a "
                       "multiple of the device sector size %ld bytes",
                       cfg.dosdev, (LONG)envec_bytes, (LONG)dev.sector_size);
            rc = RC_FATAL;
            goto cleanup_close;
        }
        scale = envec_bytes / dev.sector_size;

        blocks_per_cyl = (U64)pinfo.surfaces * (U64)pinfo.blockspertrack;
        part_start = (U64)pinfo.lowcyl * blocks_per_cyl * (U64)scale;
        part_len = ((U64)pinfo.highcyl - (U64)pinfo.lowcyl + 1) *
                   blocks_per_cyl * (U64)scale;
        part_boot_lba = part_start;

        if (cfg.have_range) {
            /* -r was also given: a relative sub-range within the
             * partition, for low-RAM machines (§ README "Partition
             * mode"). */
            if (cfg.range_start + cfg.range_len > part_len) {
                u64_to_str(part_len, numbuf1);
                out_printf("devsoak: -r sub-range exceeds the partition "
                           "(%s sectors)", numbuf1);
                rc = RC_FATAL;
                goto cleanup_close;
            }
            cfg.range_start = part_start + cfg.range_start;
        } else {
            cfg.range_start = part_start;
            cfg.range_len = part_len;
        }
        cfg.have_range = 1;

        {
            U64 endsec = cfg.range_start + cfg.range_len;
            U64 mb = (cfg.range_len * (U64)dev.sector_size) / (1024 * 1024);

            u64_to_str(cfg.range_start, numbuf1);
            u64_to_str(endsec, numbuf2);
            out_printf("devsoak: %s = %s unit %ld, partition sectors %s..%s",
                        cfg.dosdev, cfg.device, (LONG)cfg.unit,
                        numbuf1, numbuf2);
            u64_to_str(mb, numbuf1);
            out_printf("devsoak: %s size: %s MB", cfg.dosdev, numbuf1);
        }

        /* contents warning (§ README "Partition mode"): read the
         * partition's boot block and look for a known filesystem
         * signature in its first 4 bytes. Best-effort only -- a
         * partition starting at or past the 4 GB boundary can't be
         * addressed with a plain CMD_READ here (this runs before the
         * engine's dialect probing), so the check is skipped for it and
         * the confirmation falls back to the plain y/n tier. */
        if (part_boot_lba * (U64)dev.sector_size < 0x100000000ULL) {
            UBYTE *scratch = AllocMem(dev.sector_size,
                                       MEMF_PUBLIC | MEMF_CLEAR);
            if (scratch != NULL) {
                io->iotd_Req.io_Command = CMD_READ;
                io->iotd_Req.io_Data = scratch;
                io->iotd_Req.io_Length = dev.sector_size;
                io->iotd_Req.io_Offset =
                    (ULONG)(part_boot_lba * (U64)dev.sector_size);
                io->iotd_Req.io_Flags = 0;
                DoIO((struct IORequest *)io);

                if (io->iotd_Req.io_Error == 0 &&
                    io->iotd_Req.io_Actual >= 4) {
                    if (memcmp(scratch, "muFS", 4) == 0) {
                        strcpy(signame, "muFS");
                        have_sig = 1;
                    } else if (memcmp(scratch, "DOS", 3) == 0 ||
                               memcmp(scratch, "PFS", 3) == 0 ||
                               memcmp(scratch, "PDS", 3) == 0 ||
                               memcmp(scratch, "SFS", 3) == 0) {
                        signame[0] = (char)scratch[0];
                        signame[1] = (char)scratch[1];
                        signame[2] = (char)scratch[2];
                        signame[3] = '\\'; signame[4] = 'x';
                        {
                            UBYTE v = scratch[3];
                            static const char hex[] = "0123456789abcdef";
                            signame[5] = hex[(v >> 4) & 0xF];
                            signame[6] = hex[v & 0xF];
                        }
                        signame[7] = '\0';
                        have_sig = 1;
                    }
                }
                FreeMem(scratch, dev.sector_size);
            }
        }
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

    /* confirmation. Partition mode escalates in three tiers (§ README
     * "Partition mode"): no signature/no volume behaves exactly like the
     * plain -r form below; a filesystem signature with no live volume
     * adds a warning line ahead of the same y/n; a live mounted volume
     * requires typing the volume name back and -y does not bypass it --
     * this is destroying a filesystem someone still has open. */
    if (cfg.partition && have_partinfo && pinfo.have_volume) {
        if ((SetSignal(0, 0) & SIGBREAKF_CTRL_C) != 0) {
            out_printf("devsoak: aborted (break)");
            rc = RC_FATAL;
            goto cleanup_close;
        }

        /* policy: see PR discussion -- -y is deliberately not honoured
         * for a live mounted volume, unlike every other confirmation
         * tier; this is under review. */
        if (cfg.yes) {
            out_printf("devsoak: %s: volume \"%s:\" is live-mounted; -y "
                       "does not bypass this confirmation. Run "
                       "interactively.", cfg.dosdev, pinfo.volname);
            rc = RC_FATAL;
            goto cleanup_close;
        }

        out_printf("devsoak: %s: volume \"%s:\" is live-mounted on this "
                   "partition.", cfg.dosdev, pinfo.volname);
        out_printf("This will DESTROY it. Type the volume name (%s) to "
                   "continue:", pinfo.volname);
        {
            BPTR cin = Input();
            char ansbuf[80];
            LONG n = Read(cin, ansbuf, (LONG)sizeof(ansbuf) - 1);

            if (n > 0) {
                while (n > 0 && (ansbuf[n - 1] == '\n' || ansbuf[n - 1] == '\r'))
                    n--;
                ansbuf[n] = '\0';
            } else {
                ansbuf[0] = '\0';
            }
            if (!main_ci_eq(ansbuf, pinfo.volname)) {
                out_printf("devsoak: aborted (volume name did not match)");
                rc = RC_FATAL;
                goto cleanup_close;
            }
        }
    } else if (!cfg.yes) {
        if ((SetSignal(0, 0) & SIGBREAKF_CTRL_C) != 0) {
            out_printf("devsoak: aborted (break)");
            rc = RC_FATAL;
            goto cleanup_close;
        }

        if (cfg.partition && have_sig) {
            out_printf("devsoak: %s: partition contains a %s filesystem "
                       "(no live volume mounted).", cfg.dosdev, signame);
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

    /* partition mode: inhibit the handler before any test traffic so the
     * filesystem doesn't fight it (§ README "Partition mode"). Every exit
     * path from here on falls through to cleanup_close, which uninhibits
     * unconditionally (a no-op unless dosdev_inhibit() actually set
     * `inhibited`). */
    if (cfg.partition && have_partinfo) {
        if (dosdev_inhibit(pinfo.handler, &inhibited) != 0) {
            crumb_close();
            quirks_cleanup();
            rc = RC_FATAL;
            goto cleanup_close;
        }
    }

    rc = engine_run();

    crumb_close();
    quirks_cleanup();

cleanup_close:
    if (cfg.partition && have_partinfo)
        dosdev_uninhibit(pinfo.handler, &inhibited);

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
