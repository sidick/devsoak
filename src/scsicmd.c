/*
 * scsicmd.c - devsoak HD_SCSICMD checks (implementation brief S8, "-X").
 *
 * One-shot phase, run by main (a DOS Process) after the initial audit and
 * before any worker/auditor/invariant task exists. Nothing else is running
 * at this point, so this file uses dev.io/dev.port directly (no cloning,
 * no stripe locks) and prints with out_printf() (never out_task_printf() --
 * there is no cross-task queue to drain yet and main is the only task).
 *
 * If the driver does not implement HD_SCSICMD at all, the very first
 * probe (INQUIRY) comes back IOERR_NOCMD; that is reported
 * and treated as a clean, not-applicable result (RC_CLEAN), not a failure
 * -- the option asked for tests the driver cannot take part in.
 */

#include "devsoak.h"

#include <proto/exec.h>
#include <proto/alib.h>
#include <exec/errors.h>
#include <exec/memory.h>
#include <devices/scsidisk.h>

#include <string.h>   /* memcmp -- header/byte compare only, no libc I/O */

/* ---- small local helpers ---- */

/* RawDoFmt has no 64-bit support; render decimal by hand, same idiom as
 * audit.c's own copy (this file has no shared state with audit.c either,
 * and each module keeps its own tiny copy rather than exporting one). */
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

/* Copy n bytes from an INQUIRY response field into a NUL-terminated C
 * string, then strip trailing spaces (SCSI ASCII fields are space-padded,
 * not NUL-padded). dst must have room for n+1 bytes. */
static void
field_copy_strip(const UBYTE *src, ULONG n, char *dst)
{
    ULONG i;

    for (i = 0; i < n; i++)
        dst[i] = (char)src[i];
    dst[n] = '\0';

    while (n > 0 && dst[n - 1] == ' ') {
        n--;
        dst[n] = '\0';
    }
}

/* Issue one SCSI command through HD_SCSICMD on dev.io. Fills sc, submits,
 * returns the sign-extended io_Error -- exactly the fill_result() idiom
 * ops.c uses for trackdisk requests, applied to the SCSICmd wrapper. */
static LONG
do_scsi(UBYTE *cdb, ULONG cdblen, APTR data, ULONG datalen, UBYTE flags,
        struct SCSICmd *sc, UBYTE *sense, ULONG senselen)
{
    sc->scsi_Data        = (UWORD *)data;
    sc->scsi_Length      = datalen;
    sc->scsi_Actual      = 0xA5A5A5A5UL;   /* sentinel: catches drivers that
                                               never fill this field */
    sc->scsi_Command     = cdb;
    sc->scsi_CmdLength   = (UWORD)cdblen;
    sc->scsi_CmdActual   = 0;
    sc->scsi_Flags       = flags;
    sc->scsi_Status      = 0xFF;           /* sentinel */
    sc->scsi_SenseData   = sense;
    sc->scsi_SenseLength = (UWORD)senselen;
    sc->scsi_SenseActual = 0;

    dev.io->iotd_Req.io_Command = HD_SCSICMD;
    dev.io->iotd_Req.io_Data    = (APTR)sc;
    dev.io->iotd_Req.io_Length  = sizeof(struct SCSICmd);
    dev.io->iotd_Req.io_Offset  = 0;       /* unused for HD_SCSICMD */
    dev.io->iotd_Req.io_Actual  = 0;       /* unused for HD_SCSICMD */
    dev.io->iotd_Req.io_Flags   = 0;
    dev.io->iotd_Req.io_Error   = (BYTE)0xa5;
    dev.io->iotd_Count          = 0;

    DoIO((struct IORequest *)dev.io);

    return (LONG)(BYTE)dev.io->iotd_Req.io_Error;
}

LONG
scsicmd_phase(void)
{
    struct SCSICmd sc;
    UBYTE  cdb[10];
    UBYTE  sense[32];
    UBYTE *inqbuf;
    UBYTE *rcbuf;
    UBYTE *secA;
    UBYTE *secB;
    LONG   err;
    LONG   failures = 0;
    ULONG  ss;

    out_printf("devsoak: scsi: HD_SCSICMD checks starting");

    ss = dev.sector_size;
    if (ss == 0)
        ss = 512;   /* defensive only: main.c already validated this at
                       open time before any phase runs */

    /* ---- 1. INQUIRY (also the NOCMD probe) ---- */

    inqbuf = AllocMem(36, MEMF_PUBLIC | MEMF_CLEAR);
    if (inqbuf == NULL) {
        out_printf("devsoak: scsi: FAIL AllocMem(36) for INQUIRY failed");
        return RC_ERROR;
    }

    cdb[0] = 0x12; cdb[1] = 0; cdb[2] = 0; cdb[3] = 0; cdb[4] = 36; cdb[5] = 0;
    err = do_scsi(cdb, 6, inqbuf, 36, SCSIF_READ, &sc, NULL, 0);

    if (err == IOERR_NOCMD) {
        out_printf("devsoak: scsi: HD_SCSICMD not supported (IOERR_NOCMD), "
                    "-X tests skipped");
        FreeMem(inqbuf, 36);
        return RC_CLEAN;
    }

    if (err != 0) {
        out_printf("devsoak: scsi: FAIL INQUIRY io_Error %ld", (LONG)err);
        failures++;
    } else if (sc.scsi_Status != 0) {
        out_printf("devsoak: scsi: FAIL INQUIRY scsi_Status %ld (expected 0)",
                    (LONG)sc.scsi_Status);
        failures++;
    } else if (sc.scsi_Actual == 0xA5A5A5A5UL) {
        out_printf("devsoak: scsi: FAIL scsi_Actual not filled (INQUIRY)");
        failures++;
    } else if (sc.scsi_Actual < 8) {
        out_printf("devsoak: scsi: FAIL INQUIRY scsi_Actual %lu too short "
                    "(need >= 8)", (ULONG)sc.scsi_Actual);
        failures++;
    } else {
        char vendor[9];
        char product[17];

        field_copy_strip(inqbuf + 8, 8, vendor);
        field_copy_strip(inqbuf + 16, 16, product);
        out_printf("devsoak: scsi: INQUIRY vendor '%s' product '%s' "
                    "devtype %ld", vendor, product,
                    (LONG)(inqbuf[0] & 0x1F));
    }

    FreeMem(inqbuf, 36);

    /* ---- 2. TEST UNIT READY ---- */

    cdb[0] = 0x00; cdb[1] = 0; cdb[2] = 0; cdb[3] = 0; cdb[4] = 0; cdb[5] = 0;
    err = do_scsi(cdb, 6, NULL, 0, SCSIF_READ, &sc, NULL, 0);

    if (err != 0 || sc.scsi_Status != 0) {
        out_printf("devsoak: scsi: FAIL TEST UNIT READY io_Error %ld "
                    "scsi_Status %ld (expected 0/0 -- fixed disk should be "
                    "ready)", (LONG)err, (LONG)sc.scsi_Status);
        failures++;
    }

    /* ---- 3. READ CAPACITY(10) vs TD_GETGEOMETRY ---- */

    rcbuf = AllocMem(8, MEMF_PUBLIC | MEMF_CLEAR);
    if (rcbuf == NULL) {
        out_printf("devsoak: scsi: FAIL AllocMem(8) for READ CAPACITY failed");
        failures++;
    } else {
        cdb[0] = 0x25;
        cdb[1] = 0; cdb[2] = 0; cdb[3] = 0; cdb[4] = 0;
        cdb[5] = 0; cdb[6] = 0; cdb[7] = 0; cdb[8] = 0; cdb[9] = 0;

        err = do_scsi(cdb, 10, rcbuf, 8, SCSIF_READ, &sc, NULL, 0);

        if (err != 0) {
            out_printf("devsoak: scsi: FAIL READ CAPACITY(10) io_Error %ld",
                        (LONG)err);
            failures++;
        } else if (sc.scsi_Status != 0) {
            out_printf("devsoak: scsi: FAIL READ CAPACITY(10) scsi_Status %ld",
                        (LONG)sc.scsi_Status);
            failures++;
        } else if (sc.scsi_Actual == 0xA5A5A5A5UL) {
            out_printf("devsoak: scsi: FAIL scsi_Actual not filled "
                        "(READ CAPACITY)");
            failures++;
        } else {
            ULONG lastlba, blksize;
            U64   rc_total;

            lastlba = ((ULONG)rcbuf[0] << 24) | ((ULONG)rcbuf[1] << 16)
                    | ((ULONG)rcbuf[2] << 8)  |  (ULONG)rcbuf[3];
            blksize = ((ULONG)rcbuf[4] << 24) | ((ULONG)rcbuf[5] << 16)
                    | ((ULONG)rcbuf[6] << 8)  |  (ULONG)rcbuf[7];
            rc_total = (U64)lastlba + 1ULL;

            if (dev.have_geom) {
                if (rc_total != dev.total_sectors || blksize != dev.sector_size) {
                    char a[24], b[24];

                    u64_to_str(rc_total, a);
                    u64_to_str(dev.total_sectors, b);
                    out_printf("devsoak: scsi: FAIL READ CAPACITY disagrees "
                                "with TD_GETGEOMETRY (capacity total %s "
                                "blksize %ld vs geometry total %s "
                                "blksize %ld)",
                                a, (LONG)blksize, b, (LONG)dev.sector_size);
                    failures++;
                }
            }
        }
        FreeMem(rcbuf, 8);
    }

    /* ---- 4. READ(10) vs CMD_READ byte-compare, one sector at
     * cfg.range_start (inside the test range, already filled by the fill
     * pass, so a mismatch here is meaningful either way this runs before
     * or after traffic). ---- */

    secA = AllocMem(ss, MEMF_PUBLIC);
    secB = AllocMem(ss, MEMF_PUBLIC);
    if (secA == NULL || secB == NULL) {
        out_printf("devsoak: scsi: FAIL AllocMem for READ(10)/CMD_READ "
                    "compare failed");
        failures++;
    } else {
        ULONG lba = (ULONG)cfg.range_start;   /* range starts fit 32 bits in
                                                  every realistic test range */
        U64   byteoff = cfg.range_start * (U64)ss;
        struct Result res;

        cdb[0] = 0x28; cdb[1] = 0;
        cdb[2] = (UBYTE)(lba >> 24); cdb[3] = (UBYTE)(lba >> 16);
        cdb[4] = (UBYTE)(lba >> 8);  cdb[5] = (UBYTE)lba;
        cdb[6] = 0;
        cdb[7] = 0; cdb[8] = 1;     /* transfer length: 1 block, BE */
        cdb[9] = 0;

        err = do_scsi(cdb, 10, secA, ss, SCSIF_READ, &sc, NULL, 0);

        if (err != 0) {
            out_printf("devsoak: scsi: FAIL READ(10) io_Error %ld", (LONG)err);
            failures++;
        } else if (sc.scsi_Status != 0) {
            out_printf("devsoak: scsi: FAIL READ(10) scsi_Status %ld",
                        (LONG)sc.scsi_Status);
            failures++;
        } else if (sc.scsi_Actual == 0xA5A5A5A5UL) {
            out_printf("devsoak: scsi: FAIL scsi_Actual not filled (READ(10))");
            failures++;
        } else if (sc.scsi_Actual != ss) {
            out_printf("devsoak: scsi: FAIL READ(10) scsi_Actual %lu != "
                        "sector size %ld", (ULONG)sc.scsi_Actual, (LONG)ss);
            failures++;
        } else {
            op_build_rw(dev.io, DIALECT_CMD, 0, byteoff, secB, ss, g_changenum);
            op_do_sync(dev.io, SUBMIT_DOIO, &res);

            if (res.err != 0 || res.actual != ss) {
                out_printf("devsoak: scsi: FAIL CMD_READ compare-read err %ld "
                            "actual %ld (expected 0/%ld)",
                            (LONG)res.err, (LONG)res.actual, (LONG)ss);
                failures++;
            } else if (memcmp(secA, secB, ss) != 0) {
                ULONG i;
                ULONG first = 0;

                for (i = 0; i < ss; i++) {
                    if (secA[i] != secB[i]) {
                        first = i;
                        break;
                    }
                }
                out_printf("devsoak: scsi: FAIL READ(10) differs from "
                            "CMD_READ at +0x%lx", (ULONG)first);
                failures++;
            }
        }
    }
    if (secA != NULL)
        FreeMem(secA, ss);
    if (secB != NULL)
        FreeMem(secB, ss);

    /* ---- 5. Unsupported opcode with autosense ---- */

    {
        ULONG i;

        for (i = 0; i < sizeof(sense); i++)
            sense[i] = 0x00;

        cdb[0] = 0xFF; cdb[1] = 0; cdb[2] = 0; cdb[3] = 0; cdb[4] = 0; cdb[5] = 0;

        err = do_scsi(cdb, 6, NULL, 0, (UBYTE)(SCSIF_READ | SCSIF_AUTOSENSE),
                      &sc, sense, sizeof(sense));

        if (err == 0 && sc.scsi_Status == 0) {
            out_printf("devsoak: scsi: FAIL unsupported opcode 0xFF claims "
                        "success (io_Error 0, scsi_Status 0)");
            failures++;
        } else if (err == 0 && sc.scsi_Status != 2) {
            /* Neither the expected error path nor a clean success: the
             * command completed but with a status we did not ask for.
             * Not explicitly listed as fatal by the brief, but it is not
             * the "CHECK CONDITION or io_Error" contract either. */
            out_printf("devsoak: scsi: FAIL unsupported opcode 0xFF: "
                        "io_Error 0, unexpected scsi_Status %ld "
                        "(expected CHECK CONDITION, status 2)",
                        (LONG)sc.scsi_Status);
            failures++;
        }

        if (sc.scsi_Status == 2) {
            if (sc.scsi_SenseActual == 0) {
                out_printf("devsoak: scsi: FAIL autosense not filled "
                            "(unsupported opcode, SCSIF_AUTOSENSE set)");
                failures++;
            } else if ((sense[2] & 0x0F) != 5) {
                out_printf("devsoak: scsi: WARN unsupported opcode sense "
                            "key %ld (expected 5, ILLEGAL REQUEST)",
                            (LONG)(sense[2] & 0x0F));
            }
        }
    }

    out_printf("devsoak: scsi: phase complete, %ld failure(s)",
                (LONG)failures);

    return (failures != 0) ? RC_ERROR : RC_CLEAN;
}
