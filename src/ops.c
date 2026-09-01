/*
 * ops.c - devsoak trackdisk request builders and submission (implementation
 * brief S13).
 *
 * Pure mechanism: builds IOExtTD requests for every read/write dialect
 * (S7), submits them via the three submission styles, and normalises the
 * result (including a best-effort latency stamp via timer_gettime()).
 * No output.c calls here -- callers report, this module only does I/O.
 *
 * 64-bit dialects (TD64/NSD64/NSDETD64) carry the high 32 bits of the
 * byte offset in io_Actual (devtest convention noted in devsoak.h), so
 * io_Actual must NOT be pre-filled with the 0xa5a5a5a5 sentinel for those
 * builds -- only the plain CMD/ETD dialects get the sentinel, since their
 * io_Actual is purely a result field until completion.
 */

#include "devsoak.h"

#include <proto/exec.h>
#include <proto/alib.h>   /* BeginIO */

/* ---- latency helper ----
 * Mirrors timer_elapsed_ms()'s (secs,micros) delta logic (timer.c) but at
 * microsecond resolution. Best effort only: on Kickstart 1.3 the DateStamp
 * fallback in timer_gettime() has 20 ms granularity, so short ops will
 * often read back as 0 or as a multiple of 20000.
 */
static ULONG elapsed_usec(ULONG s0, ULONG u0, ULONG s1, ULONG u1)
{
    LONG ds = (LONG)(s1 - s0);
    LONG du = (LONG)u1 - (LONG)u0;
    LONG total;

    if (du < 0) {
        du += 1000000L;
        ds -= 1;
    }
    if (ds < 0)
        return 0;   /* clock went backwards -- clamp, don't report garbage */

    total = ds * 1000000L + du;
    if (total < 0)
        total = 0;  /* overflow guard for implausibly long ops */

    return (ULONG)total;
}

static void fill_result(struct IOExtTD *io, struct Result *res,
                         ULONG s0, ULONG u0, ULONG s1, ULONG u1)
{
    res->err = (LONG)(BYTE)io->iotd_Req.io_Error;  /* sign-extend the BYTE */
    res->actual = io->iotd_Req.io_Actual;
    res->flags = (ULONG)io->iotd_Req.io_Flags;
    res->usec = elapsed_usec(s0, u0, s1, u1);
}

void op_build_rw(struct IOExtTD *io, ULONG dialect, ULONG is_write,
                  U64 byteoff, APTR data, ULONG len, ULONG changenum)
{
    /* A 32-bit dialect cannot express a transfer reaching past 4 GB --
     * the (ULONG) cast below would silently land it ~4 GB low, OUTSIDE
     * the test range. Upgrade to an enabled 64-bit dialect (ETD prefers
     * NSCMD_ETD to keep the change-count semantics). Callers that
     * deliberately want the raw 32-bit behaviour (the 4 GB wrap probe)
     * must build the request by hand. */
    if ((dialect == DIALECT_CMD || dialect == DIALECT_ETD) &&
        byteoff + (U64)len > 0x100000000ULL) {
        ULONG i, up = DIALECT_COUNT;

        for (i = 0; i < g_n_enabled; i++) {
            ULONG d = g_enabled_dialects[i];

            if (dialect == DIALECT_ETD && d == DIALECT_NSDETD64) {
                up = d;
                break;
            }
            if (up == DIALECT_COUNT &&
                (d == DIALECT_TD64 || d == DIALECT_NSD64 ||
                 d == DIALECT_NSDETD64))
                up = d;
        }
        if (up != DIALECT_COUNT)
            dialect = up;
    }

    io->iotd_Req.io_Data = data;
    io->iotd_Req.io_Length = len;
    io->iotd_Req.io_Flags = 0;
    io->iotd_Req.io_Error = (BYTE)0xa5;
    io->iotd_Count = 0;

    switch (dialect) {

    case DIALECT_ETD:
        io->iotd_Req.io_Command = is_write ? ETD_WRITE : ETD_READ;
        io->iotd_Req.io_Offset = (ULONG)byteoff;
        io->iotd_Req.io_Actual = 0xa5a5a5a5UL;
        io->iotd_Count = changenum;
        break;

    case DIALECT_TD64:
        io->iotd_Req.io_Command = is_write ? TD_WRITE64 : TD_READ64;
        io->iotd_Req.io_Offset = (ULONG)byteoff;
        io->iotd_Req.io_Actual = (ULONG)(byteoff >> 32);
        break;

    case DIALECT_NSD64:
        io->iotd_Req.io_Command =
            is_write ? NSCMD_TD_WRITE64 : NSCMD_TD_READ64;
        io->iotd_Req.io_Offset = (ULONG)byteoff;
        io->iotd_Req.io_Actual = (ULONG)(byteoff >> 32);
        break;

    case DIALECT_NSDETD64:
        io->iotd_Req.io_Command =
            is_write ? NSCMD_ETD_WRITE64 : NSCMD_ETD_READ64;
        io->iotd_Req.io_Offset = (ULONG)byteoff;
        io->iotd_Req.io_Actual = (ULONG)(byteoff >> 32);
        io->iotd_Count = changenum;
        break;

    case DIALECT_CMD:
    default:
        io->iotd_Req.io_Command = is_write ? CMD_WRITE : CMD_READ;
        io->iotd_Req.io_Offset = (ULONG)byteoff;
        io->iotd_Req.io_Actual = 0xa5a5a5a5UL;
        break;
    }
}

ULONG op_dialect_for(U64 byteoff, ULONG len)
{
    ULONG i;

    if (byteoff + (U64)len <= 0x100000000ULL)
        return DIALECT_CMD;

    for (i = 0; i < g_n_enabled; i++) {
        ULONG d = g_enabled_dialects[i];

        if (d == DIALECT_TD64 || d == DIALECT_NSD64 ||
            d == DIALECT_NSDETD64)
            return d;
    }
    return DIALECT_COUNT;
}

void op_do_sync(struct IOExtTD *io, ULONG submit, struct Result *res)
{
    ULONG s0, u0, s1, u1;

    timer_gettime(&s0, &u0);

    switch (submit) {

    case SUBMIT_SENDIO:
        SendIO((struct IORequest *)io);
        WaitIO((struct IORequest *)io);
        break;

    case SUBMIT_QUICK:
        io->iotd_Req.io_Flags |= IOF_QUICK;
        BeginIO((struct IORequest *)io);
        /* If the driver kept IOF_QUICK set, it completed inline inside
           BeginIO and posted no message -- WaitIO/GetMsg must not be
           called. If it cleared IOF_QUICK it decided to complete async
           and did post a reply, so WaitIO is required. */
        if ((io->iotd_Req.io_Flags & IOF_QUICK) == 0)
            WaitIO((struct IORequest *)io);
        break;

    case SUBMIT_DOIO:
    default:
        DoIO((struct IORequest *)io);
        break;
    }

    timer_gettime(&s1, &u1);

    fill_result(io, res, s0, u0, s1, u1);
}

void op_simple(struct IOExtTD *io, UWORD cmd, struct Result *res)
{
    ULONG s0, u0, s1, u1;

    io->iotd_Req.io_Command = cmd;
    io->iotd_Req.io_Data = NULL;
    io->iotd_Req.io_Length = 0;
    io->iotd_Req.io_Offset = 0;
    io->iotd_Req.io_Flags = 0;
    io->iotd_Req.io_Actual = 0xa5a5a5a5UL;
    io->iotd_Req.io_Error = (BYTE)0xa5;
    io->iotd_Count = 0;

    timer_gettime(&s0, &u0);
    DoIO((struct IORequest *)io);
    timer_gettime(&s1, &u1);

    fill_result(io, res, s0, u0, s1, u1);
}

const char *op_cmd_name(UWORD cmd)
{
    switch (cmd) {

    /* -- plain CMD_ -- */
    case CMD_READ:          return "CMD_READ";
    case CMD_WRITE:         return "CMD_WRITE";
    case CMD_UPDATE:        return "CMD_UPDATE";
    case CMD_CLEAR:         return "CMD_CLEAR";
    case CMD_STOP:          return "CMD_STOP";
    case CMD_START:         return "CMD_START";
    case CMD_FLUSH:         return "CMD_FLUSH";
    case CMD_INVALID:       return "CMD_INVALID";
    case CMD_RESET:         return "CMD_RESET";

    /* -- plain TD_ (trackdisk CMD_NONSTD range) -- */
    case TD_MOTOR:          return "TD_MOTOR";
    case TD_SEEK:           return "TD_SEEK";
    case TD_FORMAT:         return "TD_FORMAT";
    case TD_REMOVE:         return "TD_REMOVE";
    case TD_CHANGENUM:      return "TD_CHANGENUM";
    case TD_CHANGESTATE:    return "TD_CHANGESTATE";
    case TD_PROTSTATUS:     return "TD_PROTSTATUS";
    case TD_RAWREAD:        return "TD_RAWREAD";
    case TD_RAWWRITE:       return "TD_RAWWRITE";
    case TD_GETDRIVETYPE:   return "TD_GETDRIVETYPE";
    case TD_GETNUMTRACKS:   return "TD_GETNUMTRACKS";
    case TD_ADDCHANGEINT:   return "TD_ADDCHANGEINT";
    case TD_REMCHANGEINT:   return "TD_REMCHANGEINT";
    case TD_GETGEOMETRY:    return "TD_GETGEOMETRY";
    case TD_EJECT:          return "TD_EJECT";

    /* -- TD64 (Commodore-numbered 64-bit dialect) --
       NOTE: devices/trackdisk.h #defines TD_READ64 to the same numeric
       value as TD_LASTCOMM (CMD_NONSTD+15); TD_LASTCOMM is not a real
       command and is deliberately not given a case here. */
    case TD_READ64:         return "TD_READ64";
    case TD_WRITE64:        return "TD_WRITE64";
    case TD_SEEK64:         return "TD_SEEK64";
    case TD_FORMAT64:       return "TD_FORMAT64";

    /* -- ETD_ (TDF_EXTCOM set): read/write are the header's own macros,
       the rest are the same TD_/CMD_ numbers with TDF_EXTCOM or'd in,
       since trackdisk.h does not name all of them itself. -- */
    case ETD_READ:          return "ETD_READ";
    case ETD_WRITE:         return "ETD_WRITE";
    case (CMD_UPDATE | TDF_EXTCOM):     return "ETD_UPDATE";
    case (CMD_CLEAR  | TDF_EXTCOM):     return "ETD_CLEAR";
    case (CMD_STOP   | TDF_EXTCOM):     return "ETD_STOP";
    case (CMD_START  | TDF_EXTCOM):     return "ETD_START";
    case (CMD_FLUSH  | TDF_EXTCOM):     return "ETD_FLUSH";
    case (CMD_INVALID| TDF_EXTCOM):     return "ETD_INVALID";
    case (CMD_RESET  | TDF_EXTCOM):     return "ETD_RESET";
    case (TD_MOTOR   | TDF_EXTCOM):     return "ETD_MOTOR";
    case (TD_SEEK    | TDF_EXTCOM):     return "ETD_SEEK";
    case (TD_FORMAT  | TDF_EXTCOM):     return "ETD_FORMAT";
    case (TD_REMOVE  | TDF_EXTCOM):     return "ETD_REMOVE";
    case (TD_CHANGENUM   | TDF_EXTCOM): return "ETD_CHANGENUM";
    case (TD_CHANGESTATE | TDF_EXTCOM): return "ETD_CHANGESTATE";
    case (TD_PROTSTATUS  | TDF_EXTCOM): return "ETD_PROTSTATUS";
    case (TD_RAWREAD  | TDF_EXTCOM):    return "ETD_RAWREAD";
    case (TD_RAWWRITE | TDF_EXTCOM):    return "ETD_RAWWRITE";
    case (TD_GETDRIVETYPE | TDF_EXTCOM):return "ETD_GETDRIVETYPE";
    case (TD_GETNUMTRACKS | TDF_EXTCOM):return "ETD_GETNUMTRACKS";
    case (TD_ADDCHANGEINT | TDF_EXTCOM):return "ETD_ADDCHANGEINT";
    case (TD_REMCHANGEINT | TDF_EXTCOM):return "ETD_REMCHANGEINT";
    case (TD_GETGEOMETRY  | TDF_EXTCOM):return "ETD_GETGEOMETRY";
    case (TD_EJECT        | TDF_EXTCOM):return "ETD_EJECT";

    /* -- New Style Device dialect --
       These all have bit 15 set too (0x4000/0xC00x/0xE00x), but they are
       distinct literal values from every TD_/CMD_/ETD_ case above (whose
       largest value is TD_EJECT|TDF_EXTCOM = 0x8017), so there is no
       ambiguity in this switch. */
    case NSCMD_DEVICEQUERY:  return "NSCMD_DEVICEQUERY";
    case NSCMD_TD_READ64:    return "NSCMD_TD_READ64";
    case NSCMD_TD_WRITE64:   return "NSCMD_TD_WRITE64";
    case NSCMD_TD_SEEK64:    return "NSCMD_TD_SEEK64";
    case NSCMD_TD_FORMAT64:  return "NSCMD_TD_FORMAT64";
    case NSCMD_ETD_READ64:   return "NSCMD_ETD_READ64";
    case NSCMD_ETD_WRITE64:  return "NSCMD_ETD_WRITE64";
    case NSCMD_ETD_SEEK64:   return "NSCMD_ETD_SEEK64";
    case NSCMD_ETD_FORMAT64: return "NSCMD_ETD_FORMAT64";

    default:
        return "?";
    }
}
