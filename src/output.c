/*
 * output.c - devsoak output layer (implementation brief S9.1).
 *
 * Every line goes through RawDoFmt() so the console and serial sinks are
 * driven by the same format strings. Callers must only use %ld/%lx/%lu/%s
 * (RawDoFmt args are 16-bit by default; this module never uses bare %d).
 *
 * RawDoFmt formats by calling a PutChProc once per output character with
 * the character in d0 and PutChData in a3 (classic RKM calling convention,
 * not a normal C call). To fill a linear buffer we hand RawDoFmt the
 * canonical two-instruction stub:
 *     move.b d0,(a3)+ ; rts
 * PutChData is the buffer pointer; each call stores the byte and advances
 * a3. RawDoFmt always appends a trailing NUL once formatting is done, so
 * the buffer is a plain C string afterwards.
 *
 * That stub has no bounds check, so a format producing more output than
 * the buffer would overrun it. Mitigation: every format string used by
 * devsoak is a static string literal under our control and kept well
 * under the buffer size; the buffers are sized generously (512 bytes)
 * against a hard 255-char post-format truncation, not as an enforced
 * limit during RawDoFmt itself. See the final report for this risk.
 *
 * The serial sink emits via the ROM debug entry point RawPutChar
 * (exec LVO -516): private but stable from Kickstart 1.3 through 3.x and
 * on AROS m68k. It needs only SysBase -- no device open, no DOS -- so it
 * is safe to call from any task, including before dos.library is usable.
 */

#include "devsoak.h"

#include <proto/exec.h>
#include <proto/dos.h>
#include <stdarg.h>

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;  /* opened by startup code; see report */

/* move.b d0,(a3)+ ; rts -- RawDoFmt PutChProc that stores into (a3) */
static const UWORD putch_code[2] = { 0x16C0, 0x4E75 };

#define LINE_BUF_SIZE   512
#define LINE_MAX_CHARS  255

static UBYTE out_mode;
static UBYTE con_buf[LINE_BUF_SIZE];
static UBYTE ser_buf[LINE_BUF_SIZE];

static void rawputchar(LONG c)
{
    register struct ExecBase *a6 asm("a6") = SysBase;
    register LONG d0 asm("d0") = c;
    /* d0 is in-out: exec calls may trash d0/d1/a0/a1. */
    asm volatile ("jsr -516(a6)"
                  : "+r" (d0)
                  : "r" (a6)
                  : "d1", "a0", "a1", "cc", "memory");
}

static ULONG line_len(const UBYTE *buf, ULONG max)
{
    ULONG i = 0;
    while (i < max && buf[i] != 0)
        i++;
    return i;
}

/* Format fmt/ap into buf via RawDoFmt, truncate to LINE_MAX_CHARS, return
   the resulting length (not counting any terminator). */
static ULONG format_line(UBYTE *buf, const char *fmt, va_list ap)
{
    ULONG len;

    RawDoFmt((CONST_STRPTR)fmt, (APTR)ap,
              (void (*)(void))putch_code, (APTR)buf);

    len = line_len(buf, LINE_BUF_SIZE - 1);
    if (len > LINE_MAX_CHARS)
        len = LINE_MAX_CHARS;
    return len;
}

/* Console sink: format into con_buf, append LF, one Write() per line.
   Forbid()/Permit() guards the shared buffer end-to-end; today only the
   main task calls out_printf(), a later milestone adds a cross-task
   queue and this guard is what keeps it safe then too. Write() may
   block inside DoIO/Wait(), which is fine under Forbid() -- Wait()
   implicitly Permits while suspended and re-Forbids on return. */
static void console_emit(const char *fmt, va_list ap)
{
    ULONG len;

    Forbid();
    len = format_line(con_buf, fmt, ap);
    con_buf[len] = '\n';
    Write(Output(), con_buf, len + 1);
    Permit();
}

/* Serial sink: format into ser_buf, emit every character unbuffered via
   RawPutChar, CR LF ending. Own buffer and own Forbid()/Permit() guard
   so it never depends on the console path or on dos.library. */
static void serial_emit(const char *fmt, va_list ap)
{
    ULONG len, i;

    Forbid();
    len = format_line(ser_buf, fmt, ap);
    for (i = 0; i < len; i++)
        rawputchar((LONG)(UBYTE)ser_buf[i]);
    rawputchar((LONG)'\r');
    rawputchar((LONG)'\n');
    Permit();
}

void out_init(UBYTE mode)
{
    out_mode = mode;
}

void out_cleanup(void)
{
    /* Nothing to flush: the console sink Write()s per line and never
       holds a private handle (Output() is the CLI's own), and the
       serial sink is unbuffered by design. */
}

void out_printf(const char *fmt, ...)
{
    va_list ap;

    if (out_mode == OUT_CON || out_mode == OUT_BOTH) {
        va_start(ap, fmt);
        console_emit(fmt, ap);
        va_end(ap);
    }

    if (out_mode == OUT_SER || out_mode == OUT_BOTH) {
        va_start(ap, fmt);
        serial_emit(fmt, ap);
        va_end(ap);
    }
}

/* Serial-only, unbuffered, callable from any task at any time (crash
 * breadcrumbs, watchdog): always emits to serial regardless of mode,
 * since serial is the only sink guaranteed to survive a hang/crash.
 * NOTE: devsoak.h's comment describes a 'force' parameter that does not
 * exist in its own prototype; this implementation always emits, which
 * is the only behaviour consistent with the documented use case (crash
 * breadcrumbs need to reach the wire even in OUT_CON mode). The header
 * comment should be corrected to say so plainly. */
void out_serial_line(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    serial_emit(fmt, ap);
    va_end(ap);
}
