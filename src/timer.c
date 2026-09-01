/*
 * timer.c - devsoak timing helper (implementation brief S13).
 *
 * Wall-clock time for the main task: TR_GETSYSTIME on Kickstart V36+
 * (exec/kickstart >= 2.0), DateStamp() fallback (1/50 s ticks) on 1.3.
 * Also provides timer_delay_ms() via TR_ADDREQUEST on UNIT_VBLANK, which
 * is independent of dos.library Delay() so it works before/without DOS.
 *
 * One timerequest is opened at UNIT_VBLANK and reused for both
 * TR_GETSYSTIME and TR_ADDREQUEST. For M1 (single-task use from main
 * only) this is safe; if a later milestone calls timer_gettime() or
 * timer_delay_ms() from worker tasks concurrently, this must move to a
 * request-per-task (or per-call) scheme -- see the Forbid()/Permit()
 * guard around the TR_GETSYSTIME DoIO below, which exists only to keep
 * the shared request self-consistent for M1, not as a real multi-task
 * solution.
 */

#include "devsoak.h"

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/alib.h>   /* CreatePort/CreateExtIO/DeletePort/DeleteExtIO */
#include <exec/execbase.h>
#include <dos/dos.h>

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

#define TIMER_UNIT  UNIT_VBLANK

static struct MsgPort     *timer_port;
static struct timerequest *timer_io;
static UBYTE                timer_open;
static UBYTE                have_v36;

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
}

void timer_gettime(ULONG *secs, ULONG *micros)
{
    if (have_v36 && timer_io != NULL) {
        /* Wait() (inside DoIO) implicitly Permits while blocked and
           re-Forbids on return, so this is safe even though DoIO can
           suspend the task -- it just keeps other tasks from touching
           timer_io between the request being issued and answered. */
        Forbid();
        timer_io->tr_node.io_Command = TR_GETSYSTIME;
        timer_io->tr_node.io_Flags = 0;
        DoIO((struct IORequest *)timer_io);
        *secs = timer_io->tr_time.tv_secs;
        *micros = timer_io->tr_time.tv_micro;
        Permit();
    } else {
        struct DateStamp ds;
        DateStamp(&ds);
        *secs = (ULONG)ds.ds_Days * 86400UL
              + (ULONG)ds.ds_Minute * 60UL
              + (ULONG)(ds.ds_Tick / 50);
        *micros = (ULONG)(ds.ds_Tick % 50) * 20000UL;
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
