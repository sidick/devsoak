/*
 * buf.c - devsoak guarded, alignment/memtype-variant I/O buffers
 * (implementation brief S8/S13).
 *
 * Every test buffer is one AllocMem() block containing, in order:
 *   [ optional front slack ][ GUARD_BYTES guard ][ len data bytes ]
 *   [ GUARD_BYTES guard ][ optional back slack ]
 * b->data always points at the start of the data region; the guard
 * regions are filled with GUARD_FILL and checked byte-for-byte by
 * buf_check_guards() to catch a driver that DMAs past the ends of the
 * buffer it was given (S8). Pure mechanism: no out_printf() here,
 * callers report.
 */

#include "devsoak.h"

#include <proto/exec.h>

/* Largest multiple of 4 strictly less than 4096, i.e. the largest
   longword-aligned residue (mod 4096) a buffer start can have and still
   leave room for a 4K boundary inside [data, data+len) once len > 4
   (see the len<=4 note below). */
#define CROSS4K_TARGET_RESIDUE  4092UL
#define CROSS4K_MODULUS         4096UL

/* buf_alloc() picks, for each alignment variant, the residue (mod
   'modulus') that b->data's address must have, then allocates enough
   slack in front of the GUARD_BYTES pre-guard to be able to hit that
   residue no matter what address AllocMem() happens to hand back
   (AllocMem() is guaranteed only to be long word aligned). */
LONG buf_alloc(struct TestBuf *b, ULONG len, ULONG alignsel, ULONG memflags)
{
    ULONG modulus, target;
    ULONG extra;
    ULONG basesize;
    UBYTE *base;
    ULONG candidate, rem, pad;
    UBYTE *data;
    UBYTE *p;
    ULONG i;

    switch (alignsel) {

    case ALIGN_WORD:
        modulus = 4;
        target = 2;
        extra = 4;
        break;

    case ALIGN_ODD:
        modulus = 2;
        target = 1;
        extra = 4;
        break;

    case ALIGN_CROSS4K:
        if (len > 4) {
            /* Need a longword-aligned start whose residue mod 4096 is
               high enough that a 4K boundary lands strictly inside the
               buffer. Worst case the block AllocMem() returns forces
               almost a full page of front slack to reach that residue,
               hence the ~4K extra (brief S8/S13). */
            modulus = CROSS4K_MODULUS;
            target = CROSS4K_TARGET_RESIDUE;
            extra = 4096UL + 8UL;
        } else {
            /* Too small for any longword-aligned start to have a 4K
               boundary strictly inside it (the nearest reachable
               residues are >4092 apart from the next page and len<=4
               can't bridge that gap) -- fall back to plain longword
               alignment and succeed anyway, per brief. */
            modulus = 4;
            target = 0;
            extra = 4;
        }
        break;

    case ALIGN_LONG:
    default:
        modulus = 4;
        target = 0;
        extra = 4;
        break;
    }

    basesize = len + 2UL * GUARD_BYTES + extra;

    base = (UBYTE *)AllocMem(basesize, memflags);
    if (base == NULL) {
        b->base = NULL;
        b->basesize = 0;
        b->data = NULL;
        b->len = 0;
        return 1;
    }

    /* Earliest address that still leaves GUARD_BYTES of room in front of
       it inside the allocation; slide forward by 'pad' (< modulus, and
       comfortably < extra) to hit the required residue. */
    candidate = (ULONG)(base + GUARD_BYTES);
    rem = candidate % modulus;
    pad = (target + modulus - rem) % modulus;
    data = base + GUARD_BYTES + pad;

    b->base = base;
    b->basesize = basesize;
    b->data = data;
    b->len = len;

    p = data - GUARD_BYTES;
    for (i = 0; i < GUARD_BYTES; i++)
        p[i] = GUARD_FILL;

    p = data + len;
    for (i = 0; i < GUARD_BYTES; i++)
        p[i] = GUARD_FILL;

    return 0;
}

void buf_free(struct TestBuf *b)
{
    if (b->base != NULL)
        FreeMem(b->base, b->basesize);

    b->base = NULL;
    b->basesize = 0;
    b->data = NULL;
    b->len = 0;
}

LONG buf_check_guards(const struct TestBuf *b)
{
    const UBYTE *p;
    ULONG i;

    if (b->data == NULL)
        return 0;   /* nothing allocated -- nothing to have corrupted */

    p = b->data - GUARD_BYTES;
    for (i = 0; i < GUARD_BYTES; i++) {
        if (p[i] != GUARD_FILL)
            return (LONG)(i + 1);
    }

    p = b->data + b->len;
    for (i = 0; i < GUARD_BYTES; i++) {
        if (p[i] != GUARD_FILL)
            return -(LONG)(i + 1);
    }

    return 0;
}

void buf_prefill(struct TestBuf *b, UBYTE pattern)
{
    ULONG i;

    for (i = 0; i < b->len; i++)
        b->data[i] = pattern;
}
