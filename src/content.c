/*
 * content.c - devsoak sector content model (implementation brief S5).
 *
 * Every sector devsoak writes carries a 32-byte header (struct SectorHdr,
 * devsoak.h) followed by an xorshift32 payload stream seeded from
 * seed ^ (ULONG)lba ^ (generation << 16). Verification is stateless: given
 * (lba, generation) the expected bytes are fully recomputable, so no copy
 * of what was written needs to be kept anywhere but generation[].
 *
 * Odd alignment: buf_alloc() (buf.c) deliberately hands out odd-aligned
 * and word-aligned test buffers (ALIGN_ODD, ALIGN_WORD) to catch DMA byte
 * lane bugs in the driver under test. A 68000 takes an Address Error
 * exception on any word/long access through an odd address, so this file
 * must never cast `sect` to a ULONG or UWORD pointer and dereference it --
 * every multi-byte read or write here goes through explicit byte
 * stores/loads (put_u32/get_u32 below), for both the header and payload.
 */

#include "devsoak.h"

/* ---- byte-wise big-endian access: safe at any address on 68000 ---- */

static void
put_u32(UBYTE *p, ULONG v)
{
    p[0] = (UBYTE)(v >> 24);
    p[1] = (UBYTE)(v >> 16);
    p[2] = (UBYTE)(v >> 8);
    p[3] = (UBYTE)v;
}

static ULONG
get_u32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16)
         | ((ULONG)p[2] << 8)  | (ULONG)p[3];
}

/* Compares the 4 bytes at p against the big-endian encoding of `expected`.
 * Returns the offset (0..3) of the first differing byte, or 4 if all four
 * match. Used both to detect a mismatch and to report where in the field
 * it starts. */
static ULONG
byte_diff4(const UBYTE *p, ULONG expected)
{
    UBYTE eb[4];
    ULONG i;

    eb[0] = (UBYTE)(expected >> 24);
    eb[1] = (UBYTE)(expected >> 16);
    eb[2] = (UBYTE)(expected >> 8);
    eb[3] = (UBYTE)expected;

    for (i = 0; i < 4; i++)
        if (p[i] != eb[i])
            return i;
    return 4;
}

void
content_build(UBYTE *sect, U64 lba, ULONG generation, ULONG writer,
              ULONG xfer_len)
{
    ULONG magic     = DSOK_MAGIC;
    ULONG seed      = cfg.seed;
    ULONG sector_hi = (ULONG)(lba >> 32);
    ULONG sector_lo = (ULONG)lba;
    ULONG hdr_check;
    ULONG state;
    ULONG nwords, i;

    hdr_check = magic ^ seed ^ sector_hi ^ sector_lo
              ^ generation ^ writer ^ xfer_len;

    put_u32(sect + 0,  magic);
    put_u32(sect + 4,  seed);
    put_u32(sect + 8,  sector_hi);
    put_u32(sect + 12, sector_lo);
    put_u32(sect + 16, generation);
    put_u32(sect + 20, writer);
    put_u32(sect + 24, xfer_len);
    put_u32(sect + 28, hdr_check);

    state = XS32_SEED(cfg.seed ^ (ULONG)lba ^ (generation << 16));

    /* dev.sector_size is always a multiple of 4 (512/1024/2048/4096) so
     * the payload region is a whole number of longwords. */
    nwords = (dev.sector_size - 32) / 4;
    for (i = 0; i < nwords; i++)
        put_u32(sect + 32 + i * 4, xs32(&state));
}

LONG
content_verify(const UBYTE *sect, U64 lba, ULONG expect_gen,
               ULONG *first_diff)
{
    ULONG magic      = get_u32(sect + 0);
    ULONG seed       = get_u32(sect + 4);
    ULONG sector_hi  = get_u32(sect + 8);
    ULONG sector_lo  = get_u32(sect + 12);
    ULONG generation = get_u32(sect + 16);
    ULONG writer     = get_u32(sect + 20);
    ULONG xfer_len   = get_u32(sect + 24);
    ULONG hdr_check  = get_u32(sect + 28);
    ULONG calc_check = magic ^ seed ^ sector_hi ^ sector_lo
                      ^ generation ^ writer ^ xfer_len;
    ULONG exp_hi, exp_lo;
    ULONG state, nwords, i;

    /* (1) magic, seed, hdr_check -> CV_HDR_CORRUPT */
    if (magic != DSOK_MAGIC) {
        *first_diff = 0 + byte_diff4(sect + 0, DSOK_MAGIC);
        return CV_HDR_CORRUPT;
    }
    if (seed != cfg.seed) {
        *first_diff = 4 + byte_diff4(sect + 4, cfg.seed);
        return CV_HDR_CORRUPT;
    }
    if (hdr_check != calc_check) {
        *first_diff = 28 + byte_diff4(sect + 28, calc_check);
        return CV_HDR_CORRUPT;
    }

    /* (2) sector_hi/lo vs lba -> CV_WRONG_SECTOR */
    exp_hi = (ULONG)(lba >> 32);
    exp_lo = (ULONG)lba;
    if (sector_hi != exp_hi) {
        *first_diff = 8 + byte_diff4(sect + 8, exp_hi);
        return CV_WRONG_SECTOR;
    }
    if (sector_lo != exp_lo) {
        *first_diff = 12 + byte_diff4(sect + 12, exp_lo);
        return CV_WRONG_SECTOR;
    }

    /* (3) generation vs expect_gen -> CV_STALE_GEN */
    if (generation != expect_gen) {
        *first_diff = 16 + byte_diff4(sect + 16, expect_gen);
        return CV_STALE_GEN;
    }

    /* (4) payload: regenerate using the caller's (lba, expect_gen) */
    state = XS32_SEED(cfg.seed ^ (ULONG)lba ^ (expect_gen << 16));
    nwords = (dev.sector_size - 32) / 4;
    for (i = 0; i < nwords; i++) {
        ULONG expected = xs32(&state);
        ULONG off = 32 + i * 4;
        ULONG d = byte_diff4(sect + off, expected);

        if (d < 4) {
            *first_diff = off + d;
            return CV_PAYLOAD;
        }
    }

    return CV_OK;
}

void
content_decode_hdr(const UBYTE *sect, struct SectorHdr *out)
{
    out->magic      = get_u32(sect + 0);
    out->seed       = get_u32(sect + 4);
    out->sector_hi  = get_u32(sect + 8);
    out->sector_lo  = get_u32(sect + 12);
    out->generation = get_u32(sect + 16);
    out->writer     = get_u32(sect + 20);
    out->xfer_len   = get_u32(sect + 24);
    out->hdr_check  = get_u32(sect + 28);
}

const char *
content_class_name(LONG cv)
{
    switch (cv) {
    case CV_OK:           return "ok";
    case CV_HDR_CORRUPT:  return "header corrupt (not devsoak data)";
    case CV_WRONG_SECTOR: return "wrong sector (offset bug)";
    case CV_STALE_GEN:    return "stale generation (lost write)";
    case CV_PAYLOAD:      return "payload mismatch (partial transfer)";
    case CV_FOREIGN:      return "foreign data";
    default:              return "unknown verify class";
    }
}
