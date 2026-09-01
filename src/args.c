/*
 * devsoak - command line parsing (see implementation brief section 10)
 *
 * args_parse() only fills cfg and returns 0/nonzero; it must never print,
 * because the output layer is not initialised until the outmode is known.
 * Callers use args_error() and args_usage() (via out_printf) afterwards.
 */

#include "devsoak.h"
#include <string.h>

static char errbuf[160];
static const char *errmsg = NULL;

static void
seterr(const char *msg, const char *extra)
{
    strcpy(errbuf, msg);
    if (extra != NULL) {
        size_t n = strlen(errbuf);
        size_t room = sizeof(errbuf) - n - 1;
        size_t elen = strlen(extra);
        if (elen > room) elen = room;
        memcpy(errbuf + n, extra, elen);
        errbuf[n + elen] = '\0';
    }
    errmsg = errbuf;
}

const char *
args_error(void)
{
    return errmsg != NULL ? errmsg : "unknown error";
}

static int
ishexprefix(const char *s)
{
    return s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
}

/* parses decimal, or 0x-prefixed hex, from *pp; advances *pp past digits.
 * returns 0 on success (>=1 digit consumed), -1 if no digits found. */
static LONG
parse_u64_auto(const char **pp, U64 *out)
{
    const char *s = *pp;
    U64 v = 0;
    int any = 0;

    if (ishexprefix(s)) {
        s += 2;
        while (*s) {
            int d;
            if (*s >= '0' && *s <= '9') d = *s - '0';
            else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
            else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
            else break;
            v = (v << 4) + (U64)d;
            any = 1;
            s++;
        }
    } else {
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (U64)(*s - '0');
            any = 1;
            s++;
        }
    }
    if (!any) return -1;
    *pp = s;
    *out = v;
    return 0;
}

/* always-hex parse, 0x prefix optional (for -m ADDR) */
static LONG
parse_hex_forced(const char **pp, U64 *out)
{
    const char *s = *pp;
    U64 v = 0;
    int any = 0;

    if (ishexprefix(s)) s += 2;
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        v = (v << 4) + (U64)d;
        any = 1;
        s++;
    }
    if (!any) return -1;
    *pp = s;
    *out = v;
    return 0;
}

/* plain decimal or 0x hex, whole string, no suffix (-w/-q/-S/-A/-W/-s/unit) */
static LONG
parse_ulong(const char *s, ULONG *out)
{
    const char *p = s;
    U64 v;
    if (parse_u64_auto(&p, &v) != 0) return -1;
    if (*p != '\0') return -1;
    *out = (ULONG)v;
    return 0;
}

/* hex address, whole string (-m ADDR) */
static LONG
parse_addr(const char *s, ULONG *out)
{
    const char *p = s;
    U64 v;
    if (parse_hex_forced(&p, &v) != 0) return -1;
    if (*p != '\0') return -1;
    *out = (ULONG)v;
    return 0;
}

/* decimal or 0x hex, whole string (-M BYTES) */
static LONG
parse_bytes(const char *s, ULONG *out)
{
    return parse_ulong(s, out);
}

/* number with optional K/M/G suffix (x1024), whole string (sector counts) */
static LONG
parse_sectors(const char *s, U64 *out)
{
    const char *p = s;
    U64 v;

    if (parse_u64_auto(&p, &v) != 0) return -1;
    if (*p == 'K' || *p == 'k') { v *= 1024ULL; p++; }
    else if (*p == 'M' || *p == 'm') { v *= 1024ULL * 1024ULL; p++; }
    else if (*p == 'G' || *p == 'g') { v *= 1024ULL * 1024ULL * 1024ULL; p++; }
    if (*p != '\0') return -1;
    *out = v;
    return 0;
}

/* -r START,LEN */
static LONG
parse_range(const char *s, U64 *start, U64 *len)
{
    char buf[64];
    char *comma;
    size_t n = strlen(s);

    if (n >= sizeof(buf)) return -1;
    strcpy(buf, s);
    comma = strchr(buf, ',');
    if (comma == NULL) return -1;
    *comma = '\0';
    if (parse_sectors(buf, start) != 0) return -1;
    if (parse_sectors(comma + 1, len) != 0) return -1;
    return 0;
}

/* number with s/m/h suffix (x1/60/3600), whole string, default unit seconds */
static LONG
parse_duration(const char *s, ULONG *out)
{
    const char *p = s;
    U64 v;

    if (parse_u64_auto(&p, &v) != 0) return -1;
    if (*p == 's' || *p == 'S') { p++; }
    else if (*p == 'm' || *p == 'M') { v *= 60ULL; p++; }
    else if (*p == 'h' || *p == 'H') { v *= 3600ULL; p++; }
    if (*p != '\0') return -1;
    *out = (ULONG)v;
    return 0;
}

/* splits "ID[,ID...]" in place (mutates s) into cfg.forcequirk[] */
static LONG
parse_quirklist(char *s)
{
    char *p = s;

    cfg.nforcequirks = 0;
    while (p != NULL && *p != '\0') {
        char *comma = strchr(p, ',');
        if (comma != NULL) *comma = '\0';
        if (cfg.nforcequirks >= MAX_FORCEQUIRKS) return -1;
        cfg.forcequirk[cfg.nforcequirks++] = p;
        p = (comma != NULL) ? comma + 1 : NULL;
    }
    return 0;
}

LONG
args_parse(int argc, char **argv)
{
    int i;
    ULONG u;

    memset(&cfg, 0, sizeof(cfg));
    cfg.duration_s = 60;
    cfg.workers = 4;
    cfg.qdepth = 4;
    cfg.stripe = 256;
    cfg.audit_min = 10;
    cfg.maxxfer = 0x1FE00;
    cfg.watchdog_s = 5;
    cfg.outmode = OUT_CON;

    if (argc < 3) {
        seterr("usage: devsoak DEVICE UNIT -d -r START,LEN [options]", NULL);
        return 1;
    }

    cfg.device = argv[1];
    if (parse_ulong(argv[2], &u) != 0) {
        seterr("bad UNIT: ", argv[2]);
        return 1;
    }
    cfg.unit = (LONG)u;

    for (i = 3; i < argc; i++) {
        char *arg = argv[i];

        if (strcmp(arg, "-d") == 0) {
            cfg.destructive = 1;
        } else if (strcmp(arg, "-r") == 0) {
            if (++i >= argc) { seterr("-r requires START,LEN", NULL); return 1; }
            if (parse_range(argv[i], &cfg.range_start, &cfg.range_len) != 0) {
                seterr("bad -r value: ", argv[i]);
                return 1;
            }
            cfg.have_range = 1;
        } else if (strcmp(arg, "-t") == 0) {
            if (++i >= argc) { seterr("-t requires a duration", NULL); return 1; }
            if (parse_duration(argv[i], &cfg.duration_s) != 0) {
                seterr("bad -t value: ", argv[i]);
                return 1;
            }
        } else if (strcmp(arg, "-w") == 0) {
            if (++i >= argc) { seterr("-w requires N", NULL); return 1; }
            if (parse_ulong(argv[i], &cfg.workers) != 0) {
                seterr("bad -w value: ", argv[i]);
                return 1;
            }
        } else if (strcmp(arg, "-q") == 0) {
            if (++i >= argc) { seterr("-q requires N", NULL); return 1; }
            if (parse_ulong(argv[i], &cfg.qdepth) != 0) {
                seterr("bad -q value: ", argv[i]);
                return 1;
            }
        } else if (strcmp(arg, "-S") == 0) {
            if (++i >= argc) { seterr("-S requires N", NULL); return 1; }
            if (parse_ulong(argv[i], &cfg.stripe) != 0) {
                seterr("bad -S value: ", argv[i]);
                return 1;
            }
        } else if (strcmp(arg, "-A") == 0) {
            if (++i >= argc) { seterr("-A requires MIN", NULL); return 1; }
            if (parse_ulong(argv[i], &cfg.audit_min) != 0) {
                seterr("bad -A value: ", argv[i]);
                return 1;
            }
        } else if (strcmp(arg, "-M") == 0) {
            if (++i >= argc) { seterr("-M requires BYTES", NULL); return 1; }
            if (parse_bytes(argv[i], &cfg.maxxfer) != 0) {
                seterr("bad -M value: ", argv[i]);
                return 1;
            }
        } else if (strcmp(arg, "-m") == 0) {
            ULONG addr;
            if (++i >= argc) { seterr("-m requires ADDR", NULL); return 1; }
            if (parse_addr(argv[i], &addr) != 0) {
                seterr("bad -m value: ", argv[i]);
                return 1;
            }
            if (cfg.nmemregions >= MAX_MEMREGIONS) {
                seterr("too many -m regions", NULL);
                return 1;
            }
            cfg.memregion[cfg.nmemregions++] = (APTR)addr;
        } else if (strcmp(arg, "-s") == 0) {
            if (++i >= argc) { seterr("-s requires SEED", NULL); return 1; }
            if (parse_ulong(argv[i], &cfg.seed) != 0) {
                seterr("bad -s value: ", argv[i]);
                return 1;
            }
            cfg.seed_given = 1;
        } else if (strcmp(arg, "-e") == 0) {
            cfg.stoponerror = 1;
        } else if (strcmp(arg, "-y") == 0) {
            cfg.yes = 1;
        } else if (strcmp(arg, "-B") == 0) {
            cfg.bigdev = 1;
        } else if (strcmp(arg, "-R") == 0) {
            cfg.removable = 1;
        } else if (strcmp(arg, "-H") == 0) {
            if (++i >= argc) { seterr("-H requires CMD", NULL); return 1; }
            cfg.hookcmd = argv[i];
        } else if (strcmp(arg, "-X") == 0) {
            cfg.scsicmd = 1;
        } else if (strcmp(arg, "-W") == 0) {
            if (++i >= argc) { seterr("-W requires SEC", NULL); return 1; }
            if (parse_ulong(argv[i], &cfg.watchdog_s) != 0) {
                seterr("bad -W value: ", argv[i]);
                return 1;
            }
        } else if (strcmp(arg, "-o") == 0) {
            if (++i >= argc) { seterr("-o requires con|ser|both", NULL); return 1; }
            if (strcmp(argv[i], "con") == 0) cfg.outmode = OUT_CON;
            else if (strcmp(argv[i], "ser") == 0) cfg.outmode = OUT_SER;
            else if (strcmp(argv[i], "both") == 0) cfg.outmode = OUT_BOTH;
            else { seterr("bad -o value: ", argv[i]); return 1; }
        } else if (strcmp(arg, "-Q") == 0) {
            if (++i >= argc) { seterr("-Q requires FILE", NULL); return 1; }
            cfg.quirksfile = argv[i];
        } else if (strcmp(arg, "-k") == 0) {
            if (++i >= argc) { seterr("-k requires ID[,ID]", NULL); return 1; }
            if (parse_quirklist(argv[i]) != 0) {
                seterr("too many -k ids: ", argv[i]);
                return 1;
            }
        } else if (strcmp(arg, "-K") == 0) {
            cfg.noquirks = 1;
        } else if (strcmp(arg, "-Z") == 0) {
            cfg.risky = 1;
        } else if (strcmp(arg, "-v") == 0) {
            cfg.verbose = 1;
        } else if (strcmp(arg, "-P") == 0) {
            if (++i >= argc) { seterr("-P requires FILE", NULL); return 1; }
            cfg.crumbfile = argv[i];
        } else if (strcmp(arg, "--resume") == 0) {
            cfg.resume = 1;
        } else {
            seterr("unknown option: ", arg);
            return 1;
        }
    }

    if (cfg.resume && cfg.crumbfile != NULL) {
        /* --resume with -P: -d/-r not required */
    } else {
        if (!cfg.destructive) {
            seterr("-d is required (devsoak is destructive by design)", NULL);
            return 1;
        }
        if (!cfg.have_range) {
            seterr("-r START,LEN is required", NULL);
            return 1;
        }
    }

    return 0;
}

void
args_usage(void)
{
    out_printf("devsoak DEVICE UNIT -d -r START,LEN [options]");
    out_printf("");
    out_printf("  -d            destructive (required)");
    out_printf("  -r START,LEN  test range in sectors (required); LEN may use K/M/G suffix");
    out_printf("  -t DURATION   e.g. 30s, 20m, 8h (default 60s)");
    out_printf("  -w N          worker tasks (default 4)");
    out_printf("  -q N          outstanding requests per worker (default 4)");
    out_printf("  -S N          stripe size in sectors (default 256)");
    out_printf("  -A MIN        full audit interval in minutes (default 10, 0 = only start/end)");
    out_printf("  -M BYTES      MaxTransfer (default 0x1FE00)");
    out_printf("  -m ADDR       extra buffer memory region (hex address), may repeat");
    out_printf("  -s SEED       PRNG seed (default: from clock, printed)");
    out_printf("  -e            stop on first error");
    out_printf("  -y            skip confirmation");
    out_printf("  -B            big device: include 4 GB boundary tests");
    out_printf("  -R            removable media semantics");
    out_printf("  -H CMD        shell command to trigger eject/insert (with -R)");
    out_printf("  -X            include HD_SCSICMD tests");
    out_printf("  -W SEC        watchdog timeout (default 5)");
    out_printf("  -o con|ser|both  output sink (default con)");
    out_printf("  -Q FILE       quirks file (default: devsoak.quirks next to the binary, if present)");
    out_printf("  -k ID[,ID]    apply these quirk ids regardless of the file's match rules");
    out_printf("  -K            ignore the quirks file entirely (driver-under-test mode)");
    out_printf("  -Z            include the \"risky\" test tier; off by default");
    out_printf("  -v            verbose (log every op to stdout - slow)");
    out_printf("  -P FILE       breadcrumb file for crash resume");
    out_printf("  --resume      resume from -P FILE after a crash/hang");
}
