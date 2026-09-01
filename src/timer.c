/*
 * timer.c - devsoak timing helper (implementation brief S13).
 *
 * Wall-clock time is needed from many tasks at once (main, workers, the
 * auditor), so timer_gettime() must be task-safe without ever doing a
 * DoIO() on a request another task might touch: DoIO()'s internal Wait()
 * implicitly Permits while the caller is suspended, which is exactly the
 * window where a second task could issue its own command on the *same*
 * timerequest and corrupt it. A Forbid() around the DoIO(), as this file
 * used to have, does not close that window -- it only looks like it does.
 *
 * V36+ (2.0 and later): timer.device exports GetSysTime() as an ordinary
 * *library* call (LVO -66, TR_GETSYSTIME's non-IORequest twin), usable
 * from any task with no IORequest and no Forbid() at all. TimerBase (a
 * "struct Device *", per timer.device convention, even though GetSysTime
 * is declared against "struct Library *") is captured once in
 * timer_init() from the already-open timerequest's io_Device and never
 * touched again, so every later timer_gettime() call is just a call
 * through TimerBase -- safe from any number of concurrent tasks.
 *
 * <V36 (Kickstart 1.3): timer.device has no GetSysTime() library call, so
 * there is no task-safe way to read a live clock without a private
 * IORequest per caller. DateStamp() would work, but it is a dos.library
 * call and worker/auditor tasks in devsoak are plain Exec tasks with no
 * Process -- calling dos.library from them is unsafe/undefined. DECISION:
 * on <V36, timer_gettime() calls DateStamp() only when the calling task
 * is actually a Process (tc_Node.ln_Type == NT_PROCESS), and caches the
 * result (cached_s/cached_u) under Forbid(); non-Process callers (the
 * worker/auditor tasks) just read that cache under Forbid(). This makes
 * <V36 timestamps second-granular and up to ~1s stale for non-Process
 * callers instead of tick-accurate, which is an accepted trade-off: on
 * 1.3, worker/ring/watchdog timestamps only need "roughly when", and the
 * alternative (no safe clock at all for those tasks) is worse. main and
 * the auditor, when run as/with a Process, keep tick-accurate stamps and
 * refresh the shared cache for everyone else.
 *
 * timer_delay_ms() is unchanged: main-task-only by convention (not
 * enforced), still uses the shared UNIT_VBLANK timerequest via
 * TR_ADDREQUEST/DoIO(). It must not be called from worker/auditor tasks.
 */

#include "devsoak.h"

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>  /* GetSysTime() -- needs the TimerBase global below */
#include <proto/alib.h>   /* CreatePort/CreateExtIO/DeletePort/DeleteExtIO */
#include <exec/execbase.h>
#include <dos/dos.h>

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

/* proto/timer.h's GetSysTime() macro calls through a global literally
 * named TimerBase (TIMER_BASE_NAME, unset here so it defaults to
 * "TimerBase"). Declaring it as a real global (not just extern) is what
 * the brief calls for; it is populated once in timer_init(). */
struct Device *TimerBase;

#define TIMER_UNIT  UNIT_VBLANK

static struct MsgPort     *timer_port;
static struct timerequest *timer_io;
static UBYTE                timer_open;
static UBYTE                have_v36;

/* <V36 fallback cache -- see the file comment. Guarded by Forbid(). */
static ULONG cached_s;
static ULONG cached_u;

LONG timer_init(void)
{
    timer_port = NULL;
    timer_io = NULL;
    timer_open = 0;

    have_v36 = (SysBase->LibNode.lib_Version >= 36) ? 1 : 0;

    timer_port = CreatePort(NULL, 0);
    if (timer_port == NULL)
        return 1;

    timer_io = (struct timerequest *)CreateExtIO(timer_port, sizeof(struct timerequest));
    if (timer_io == NULL) {
        DeletePort(timer_port);
        timer_port = NULL;
        return 1;
    }

    if (OpenDevice((STRPTR)TIMERNAME, TIMER_UNIT, (struct IORequest *)timer_io, 0) != 0) {
        DeleteExtIO((struct IORequest *)timer_io);
        timer_io = NULL;
        DeletePort(timer_port);
        timer_port = NULL;
        return 1;
    }
    timer_open = 1;

    /* Capture TimerBase for the V36+ GetSysTime() library call. Even
       though every NDK header spells this "struct Device *", it is really
       just a base register value to GetSysTime() -- no different from any
       other library base. */
    TimerBase = (struct Device *)timer_io->tr_node.io_Device;

    cached_s = 0;
    cached_u = 0;

    return 0;
}

void timer_cleanup(void)
{
    if (timer_open) {
        CloseDevice((struct IORequest *)timer_io);
        timer_open = 0;
    }
    if (timer_io != NULL) {
        DeleteExtIO((struct IORequest *)timer_io);
        timer_io = NULL;
    }
    if (timer_port != NULL) {
        DeletePort(timer_port);
        timer_port = NULL;
    }

    TimerBase = NULL;
}

void timer_gettime(ULONG *secs, ULONG *micros)
{
    if (have_v36 && TimerBase != NULL) {
        /* Ordinary library call, no IORequest, no Forbid() needed --
           task-safe for any number of concurrent callers. */
        struct timeval tv;
        GetSysTime(&tv);
        *secs = tv.tv_secs;
        *micros = tv.tv_micro;
        return;
    }

    /* <V36: see the file comment for why this is a Process-only
       DateStamp() plus a Forbid()-guarded cache for everyone else. */
    if (((struct Task *)FindTask(NULL))->tc_Node.ln_Type == NT_PROCESS) {
        struct DateStamp ds;
        ULONG s, u;

        DateStamp(&ds);
        s = (ULONG)ds.ds_Days * 86400UL
          + (ULONG)ds.ds_Minute * 60UL
          + (ULONG)(ds.ds_Tick / 50);
        u = (ULONG)(ds.ds_Tick % 50) * 20000UL;

        Forbid();
        cached_s = s;
        cached_u = u;
        Permit();

        *secs = s;
        *micros = u;
    } else {
        Forbid();
        *secs = cached_s;
        *micros = cached_u;
        Permit();
    }
}

ULONG timer_elapsed_ms(ULONG s0, ULONG u0)
{
    ULONG s1, u1;
    LONG ds, du;

    timer_gettime(&s1, &u1);

    ds = (LONG)(s1 - s0);
    du = (LONG)u1 - (LONG)u0;
    if (du < 0) {
        du += 1000000L;
        ds -= 1;
    }
    if (ds < 0)
        ds = 0;   /* clock went backwards (V36 sys time changed) -- clamp */

    return (ULONG)ds * 1000UL + (ULONG)(du / 1000L);
}

void timer_delay_ms(ULONG ms)
{
    if (timer_io == NULL)
        return;

    timer_io->tr_node.io_Command = TR_ADDREQUEST;
    timer_io->tr_node.io_Flags = 0;
    timer_io->tr_time.tv_secs = ms / 1000UL;
    timer_io->tr_time.tv_micro = (ms % 1000UL) * 1000UL;
    DoIO((struct IORequest *)timer_io);
}
