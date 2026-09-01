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

/* ---- output.c (§9.1) ----
 * Every line devsoak prints goes through out_printf().  RawDoFmt formats;
 * the console sink line-buffers and Write()s on the CLI handle (LF endings,
 * main task only for now -- M3 adds the cross-task hand-off queue); the
 * serial sink emits each character via RawPutChar (LVO -516) immediately,
 * CR LF endings, callable from any task.
 */
void out_init(UBYTE mode);       /* OUT_CON/OUT_SER/OUT_BOTH */
void out_cleanup(void);
void out_printf(const char *fmt, ...);
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
