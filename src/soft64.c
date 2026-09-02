/*
 * soft64.c - pure-software 64-bit multiply/divide/modulo helpers.
 *
 * bebbo amiga-gcc's libgcc implements __muldi3/__udivdi3/__umoddi3 for
 * plain 68000 via utility.library (UMult64 and friends, V36+), which
 * silently breaks the Kickstart 1.3 requirement (§3): the binary then
 * refuses to start with "utility.library failed to load". Defining the
 * same symbols here wins over libgcc at link time (our objects precede
 * -lgcc), keeping every U64 * / % in the codebase ROM-independent.
 *
 * Performance is irrelevant: these run in banners, progress percentages
 * and diagnostics, never per-sector in the I/O paths.
 */

#include "devsoak.h"

/* 32x32 -> 64 via 16-bit limbs (no 64-bit multiply anywhere) */
static U64 umul32(ULONG a, ULONG b)
{
    ULONG al = a & 0xFFFF, ah = a >> 16;
    ULONG bl = b & 0xFFFF, bh = b >> 16;
    U64 r = (U64)(al * bl);

    r += (U64)(al * bh) << 16;
    r += (U64)(ah * bl) << 16;
    r += (U64)(ah * bh) << 32;
    return r;
}

U64 __muldi3(U64 a, U64 b)
{
    ULONG al = (ULONG)a, ah = (ULONG)(a >> 32);
    ULONG bl = (ULONG)b, bh = (ULONG)(b >> 32);
    U64 r = umul32(al, bl);

    r += (U64)(al * bh + ah * bl) << 32;
    return r;
}

/* shift-subtract long division: only shifts/compares/subtracts */
static U64 udivmod64(U64 n, U64 d, U64 *rem)
{
    U64 q = 0, r = 0;
    int i;

    if (d != 0) {
        for (i = 63; i >= 0; i--) {
            r = (r << 1) | ((n >> i) & 1);
            if (r >= d) {
                r -= d;
                q |= (U64)1 << i;
            }
        }
    }
    if (rem != NULL)
        *rem = r;
    return q;
}

U64 __udivdi3(U64 n, U64 d)
{
    return udivmod64(n, d, NULL);
}

U64 __umoddi3(U64 n, U64 d)
{
    U64 r;

    udivmod64(n, d, &r);
    return r;
}

long long __divdi3(long long n, long long d)
{
    UBYTE neg = 0;
    U64 un, ud, q;

    if (n < 0) { un = (U64)-n; neg ^= 1; } else un = (U64)n;
    if (d < 0) { ud = (U64)-d; neg ^= 1; } else ud = (U64)d;
    q = udivmod64(un, ud, NULL);
    return neg ? -(long long)q : (long long)q;
}

long long __moddi3(long long n, long long d)
{
    UBYTE neg = 0;
    U64 un, ud, r;

    if (n < 0) { un = (U64)-n; neg = 1; } else un = (U64)n;
    if (d < 0) ud = (U64)-d; else ud = (U64)d;
    udivmod64(un, ud, &r);
    return neg ? -(long long)r : (long long)r;
}
