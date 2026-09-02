/*
 * quirks.c - devsoak known-driver-behaviour database (implementation brief
 * S16, S10 -Q/-k/-K/-P/--resume).
 *
 * quirks_load()/quirks_cleanup()/quirks_report()/crumb_*()/
 * quirks_resume_report() run in main (a DOS Process): dos.library
 * Open/Read/Write/Seek/Close and exec AllocMem/FreeMem are used here, and
 * only here. Everything else in this file (the quirk_*() query functions)
 * is a read-only lookup over static tables built once by quirks_load()
 * before any worker/invariant/audit task starts, so they are safe to call
 * from any task: no DOS, no allocation, no locking needed.
 *
 * File format (S16.2): plain text, one "quirk" block per entry, keywords
 * ("quirk", "match", "because", "action", "status") at column 0, '#'
 * comments and blank lines ignored. A "because" block's free text may wrap
 * onto further lines; this parser does not attempt to store that text --
 * any line that starts with whitespace is treated as a continuation of
 * whatever came before it and simply skipped, which is sufent since every
 * real keyword line in the documented format starts at column 0.
 *
 * All static tables are fixed-size (no AllocMem for the parsed structures,
 * only for the raw file buffer, which is freed before quirks_load()
 * returns): MAX_QUIRK_ENTRIES entries, MAX_COND_PER_ENTRY match
 * conditions, MAX_ACTION_PER_ENTRY actions per entry (MAX_ACTIONS_TOTAL
 * across the whole file), MAX_SKIP_CMDS distinct skipped commands,
 * MAX_EXPECTS/MAX_WARNS/MAX_SKIPTESTS distinct per-test/per-command
 * actions. Anything past a limit is warned about once and ignored; no
 * hostile or oversized input can write outside these arrays.
 */

#include "devsoak.h"

#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/execbase.h>
#include <exec/errors.h>
#include <dos/dos.h>
#include <string.h>

extern struct ExecBase *SysBase;

/* ---- S16.4 cross-task crumb handshake storage (declared in devsoak.h) --
 * The invariant task publishes a breadcrumb line here and waits for main
 * to persist it via crumb_write() from its status loop; that hand-off
 * itself is main.c's/invariant.c's responsibility, this file only owns
 * the storage since it is documented alongside the rest of the quirks.c
 * API in devsoak.h. */
volatile UBYTE g_crumb_pending = 0;
char           g_crumb_text[200];

/* ---- static limits ---- */

#define MAX_QUIRK_ENTRIES   32
#define ID_LEN              40
#define TESTNAME_LEN        24
#define MAX_COND_PER_ENTRY  6
#define MAX_ACTION_PER_ENTRY 6
#define MAX_ACTIONS_TOTAL   128
#define MAX_SKIP_PER_ACTION 24
#define MAX_SKIP_CMDS       64
#define MAX_EXPECTS         16
#define MAX_WARNS           16
#define MAX_SKIPTESTS       16
#define FILEBUF_MAX         16384UL

/* ---- match conditions ---- */

#define MK_NAME     0
#define MK_VERSION  1
#define MK_REVISION 2
#define MK_ID       3
#define MK_KICK     4
#define MK_UNKNOWN  5

#define OP_EQ     0
#define OP_LT     1
#define OP_LE     2
#define OP_GT     3
#define OP_GE     4
#define OP_SUBSTR 5

struct MatchCond {
    UBYTE key;
    UBYTE op;
    LONG  num;
    char  str[ID_LEN];
};

/* ---- actions ---- */

#define ACT_SKIP        0
#define ACT_EXPECT      1
#define ACT_WARN        2
#define ACT_SKIPTEST    3
#define ACT_ALIGN       4
#define ACT_NOCHIP      5
#define ACT_MAXINFLIGHT 6
#define ACT_MAXXFER     7
#define ACT_NORANDOMCMD 8
#define ACT_TIER        9

struct Action {
    UBYTE verb;
    UWORD skipcmds[MAX_SKIP_PER_ACTION];
    ULONG nskip;
    UWORD expect_cmd;
    LONG  expect_err;
    char  testname[TESTNAME_LEN];
    ULONG numval;   /* align / maxinflight / maxxfer */
    LONG  tierval;
};

/* ---- status ---- */

#define ST_CONFIRMED 0
#define ST_REPORTED  1
#define ST_SUSPECTED 2

struct QuirkEntry {
    char   id[ID_LEN];
    struct MatchCond conds[MAX_COND_PER_ENTRY];
    ULONG  nconds;
    UBYTE  unmatchable;           /* saw an unknown match key */
    UBYTE  cond_overflow_warned;
    struct Action actions[MAX_ACTION_PER_ENTRY];
    ULONG  nactions;
    UBYTE  action_overflow_warned;
    UBYTE  status;
    UBYTE  matched;
    UBYTE  applied;
};

static struct QuirkEntry entries[MAX_QUIRK_ENTRIES];
static ULONG              n_entries;
static ULONG              applied_count;
static UBYTE              entries_full_warned;
static ULONG              g_total_actions;
static UBYTE              g_actions_full_warned;
static UBYTE              skip_overflow_warned;

/* ---- merged, flat query state -----------------------------------------
 * Built once by quirks_load(); every quirk_*() query below only reads
 * this. */

struct SkipRec { UWORD cmd; ULONG entry_idx; };
static struct SkipRec skip_cmds[MAX_SKIP_CMDS];
static ULONG           n_skip;

struct ExpectRec { UWORD cmd; LONG err; ULONG entry_idx; };
static struct ExpectRec expects[MAX_EXPECTS];
static ULONG              n_expects;

struct TestRec { char name[TESTNAME_LEN]; ULONG entry_idx; };
static struct TestRec skiptests[MAX_SKIPTESTS];
static ULONG            n_skiptests;
static struct TestRec warns[MAX_WARNS];
static ULONG            n_warns;

static ULONG g_align_min;      /* max of all `align N`; 0 = no floor */
static UBYTE g_nochip;
static ULONG g_maxinflight;    /* min of all nonzero `maxinflight N`; 0 = unlimited */
static ULONG g_maxxfer;        /* min of all nonzero `maxxfer N`; 0 = no cap */
static UBYTE g_norandomcmd;
static LONG  g_tier = -1;      /* min of all `tier N`; -1 = no override */

/* ---- tiny hand-rolled string helpers (no strcasecmp on this toolchain) */

static int my_tolower(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 'a';
    return c;
}

static UBYTE ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (my_tolower((unsigned char)*a) != my_tolower((unsigned char)*b))
            return 0;
        a++; b++;
    }
    return (*a == '\0' && *b == '\0') ? 1 : 0;
}

static UBYTE ci_prefix(const char *s, const char *prefix, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (s[i] == '\0')
            return 0;
        if (my_tolower((unsigned char)s[i]) != my_tolower((unsigned char)prefix[i]))
            return 0;
    }
    return 1;
}

static UBYTE ci_strstr(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);
    const char *h;

    if (nlen == 0)
        return 1;
    for (h = hay; *h; h++) {
        size_t i;
        for (i = 0; i < nlen; i++) {
            if (h[i] == '\0')
                return 0;
            if (my_tolower((unsigned char)h[i]) != my_tolower((unsigned char)needle[i]))
                break;
        }
        if (i == nlen)
            return 1;
    }
    return 0;
}

static void strncpy_safe(char *dst, const char *src, size_t dstsize)
{
    size_t n = strlen(src);
    if (n >= dstsize)
        n = dstsize - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void trim_trailing(char *s)
{
    size_t n = strlen(s);
    while (n > 0 &&
           (s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static LONG parse_dec_signed(const char *s)
{
    LONG sign = 1;
    LONG v = 0;

    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (LONG)(*s - '0');
        s++;
    }
    return v * sign;
}

static ULONG parse_hex_or_dec(const char *s)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        ULONG v = 0;
        s += 2;
        while (*s) {
            int d;
            if (*s >= '0' && *s <= '9') d = *s - '0';
            else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
            else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
            else break;
            v = (v << 4) + (ULONG)d;
            s++;
        }
        return v;
    }
    return (ULONG)parse_dec_signed(s);
}

static const char *status_name(UBYTE st)
{
    switch (st) {
    case ST_CONFIRMED: return "confirmed";
    case ST_REPORTED:  return "reported";
    default:           return "suspected";
    }
}

/* ---- command name table (mirrors ops.c's op_cmd_name() exactly) ------- */

struct CmdName { const char *name; UWORD val; };

static const struct CmdName cmd_names[] = {
    { "CMD_READ",           CMD_READ },
    { "CMD_WRITE",          CMD_WRITE },
    { "CMD_UPDATE",         CMD_UPDATE },
    { "CMD_CLEAR",          CMD_CLEAR },
    { "CMD_STOP",           CMD_STOP },
    { "CMD_START",          CMD_START },
    { "CMD_FLUSH",          CMD_FLUSH },
    { "CMD_INVALID",        CMD_INVALID },
    { "CMD_RESET",          CMD_RESET },

    { "TD_MOTOR",           TD_MOTOR },
    { "TD_SEEK",            TD_SEEK },
    { "TD_FORMAT",          TD_FORMAT },
    { "TD_REMOVE",          TD_REMOVE },
    { "TD_CHANGENUM",       TD_CHANGENUM },
    { "TD_CHANGESTATE",     TD_CHANGESTATE },
    { "TD_PROTSTATUS",      TD_PROTSTATUS },
    { "TD_RAWREAD",         TD_RAWREAD },
    { "TD_RAWWRITE",        TD_RAWWRITE },
    { "TD_GETDRIVETYPE",    TD_GETDRIVETYPE },
    { "TD_GETNUMTRACKS",    TD_GETNUMTRACKS },
    { "TD_ADDCHANGEINT",    TD_ADDCHANGEINT },
    { "TD_REMCHANGEINT",    TD_REMCHANGEINT },
    { "TD_GETGEOMETRY",     TD_GETGEOMETRY },
    { "TD_EJECT",           TD_EJECT },

    { "TD_READ64",          TD_READ64 },
    { "TD_WRITE64",         TD_WRITE64 },
    { "TD_SEEK64",          TD_SEEK64 },
    { "TD_FORMAT64",        TD_FORMAT64 },

    { "ETD_READ",           ETD_READ },
    { "ETD_WRITE",          ETD_WRITE },
    { "ETD_UPDATE",         (UWORD)(CMD_UPDATE      | TDF_EXTCOM) },
    { "ETD_CLEAR",          (UWORD)(CMD_CLEAR       | TDF_EXTCOM) },
    { "ETD_STOP",           (UWORD)(CMD_STOP        | TDF_EXTCOM) },
    { "ETD_START",          (UWORD)(CMD_START       | TDF_EXTCOM) },
    { "ETD_FLUSH",          (UWORD)(CMD_FLUSH       | TDF_EXTCOM) },
    { "ETD_INVALID",        (UWORD)(CMD_INVALID     | TDF_EXTCOM) },
    { "ETD_RESET",          (UWORD)(CMD_RESET       | TDF_EXTCOM) },
    { "ETD_MOTOR",          (UWORD)(TD_MOTOR        | TDF_EXTCOM) },
    { "ETD_SEEK",           (UWORD)(TD_SEEK         | TDF_EXTCOM) },
    { "ETD_FORMAT",         (UWORD)(TD_FORMAT       | TDF_EXTCOM) },
    { "ETD_REMOVE",         (UWORD)(TD_REMOVE       | TDF_EXTCOM) },
    { "ETD_CHANGENUM",      (UWORD)(TD_CHANGENUM    | TDF_EXTCOM) },
    { "ETD_CHANGESTATE",    (UWORD)(TD_CHANGESTATE  | TDF_EXTCOM) },
    { "ETD_PROTSTATUS",     (UWORD)(TD_PROTSTATUS   | TDF_EXTCOM) },
    { "ETD_RAWREAD",        (UWORD)(TD_RAWREAD      | TDF_EXTCOM) },
    { "ETD_RAWWRITE",       (UWORD)(TD_RAWWRITE     | TDF_EXTCOM) },
    { "ETD_GETDRIVETYPE",   (UWORD)(TD_GETDRIVETYPE | TDF_EXTCOM) },
    { "ETD_GETNUMTRACKS",   (UWORD)(TD_GETNUMTRACKS | TDF_EXTCOM) },
    { "ETD_ADDCHANGEINT",   (UWORD)(TD_ADDCHANGEINT | TDF_EXTCOM) },
    { "ETD_REMCHANGEINT",   (UWORD)(TD_REMCHANGEINT | TDF_EXTCOM) },
    { "ETD_GETGEOMETRY",    (UWORD)(TD_GETGEOMETRY  | TDF_EXTCOM) },
    { "ETD_EJECT",          (UWORD)(TD_EJECT        | TDF_EXTCOM) },

    { "NSCMD_DEVICEQUERY",   NSCMD_DEVICEQUERY },
    { "NSCMD_TD_READ64",     NSCMD_TD_READ64 },
    { "NSCMD_TD_WRITE64",    NSCMD_TD_WRITE64 },
    { "NSCMD_TD_SEEK64",     NSCMD_TD_SEEK64 },
    { "NSCMD_TD_FORMAT64",   NSCMD_TD_FORMAT64 },
    { "NSCMD_ETD_READ64",    NSCMD_ETD_READ64 },
    { "NSCMD_ETD_WRITE64",   NSCMD_ETD_WRITE64 },
    { "NSCMD_ETD_SEEK64",    NSCMD_ETD_SEEK64 },
    { "NSCMD_ETD_FORMAT64",  NSCMD_ETD_FORMAT64 },
};
#define N_CMD_NAMES (sizeof(cmd_names) / sizeof(cmd_names[0]))

static UBYTE cmdname_lookup(const char *tok, UWORD *out)
{
    ULONG i;
    for (i = 0; i < N_CMD_NAMES; i++) {
        if (ci_eq(tok, cmd_names[i].name)) {
            *out = cmd_names[i].val;
            return 1;
        }
    }
    return 0;
}

/* ---- symbolic error-code table ---- */

struct ErrName { const char *name; LONG val; };

static const struct ErrName err_names[] = {
    { "IOERR_NOCMD",        IOERR_NOCMD },
    { "IOERR_BADLENGTH",    IOERR_BADLENGTH },
    { "IOERR_BADADDRESS",   IOERR_BADADDRESS },
    { "IOERR_OPENFAIL",     IOERR_OPENFAIL },
    { "IOERR_ABORTED",      IOERR_ABORTED },
    { "TDERR_DiskChanged",  TDERR_DiskChanged },
    { "TDERR_WriteProt",    TDERR_WriteProt },
    { "TDERR_SeekError",    TDERR_SeekError },
    { "TDERR_BadUnitNum",   TDERR_BadUnitNum },
};
#define N_ERR_NAMES (sizeof(err_names) / sizeof(err_names[0]))

static UBYTE errname_lookup(const char *tok, LONG *out)
{
    ULONG i;
    const char *s;
    UBYTE any;

    for (i = 0; i < N_ERR_NAMES; i++) {
        if (ci_eq(tok, err_names[i].name)) {
            *out = err_names[i].val;
            return 1;
        }
    }

    s = tok;
    any = 0;
    if (*s == '-') s++;
    while (*s >= '0' && *s <= '9') { any = 1; s++; }
    if (any && *s == '\0') {
        *out = parse_dec_signed(tok);
        return 1;
    }
    return 0;
}

/* ---- match evaluation --------------------------------------------------- */

static UBYTE cmp_num(LONG a, UBYTE op, LONG b)
{
    switch (op) {
    case OP_EQ: return (a == b) ? 1 : 0;
    case OP_LT: return (a <  b) ? 1 : 0;
    case OP_LE: return (a <= b) ? 1 : 0;
    case OP_GT: return (a >  b) ? 1 : 0;
    case OP_GE: return (a >= b) ? 1 : 0;
    default:    return 0;
    }
}

static UBYTE eval_cond(const struct MatchCond *c)
{
    switch (c->key) {
    case MK_NAME:
        return ci_eq(dev.dev_name, c->str);
    case MK_VERSION:
        return cmp_num((LONG)dev.dev_version, c->op, c->num);
    case MK_REVISION:
        return cmp_num((LONG)dev.dev_revision, c->op, c->num);
    case MK_ID:
        return ci_strstr(dev.dev_idstring, c->str);
    case MK_KICK:
        return cmp_num((LONG)SysBase->LibNode.lib_Version, c->op, c->num);
    default:
        return 0;
    }
}

static UBYTE eval_match(const struct QuirkEntry *e)
{
    ULONG i;

    if (e->unmatchable)
        return 0;
    if (e->nconds == 0)
        return 0;   /* a quirk with no evaluable condition never matches */

    for (i = 0; i < e->nconds; i++) {
        if (!eval_cond(&e->conds[i]))
            return 0;
    }
    return 1;
}

/* ---- parsing: match line ------------------------------------------------- */

static void parse_match_token(struct QuirkEntry *e, const char *tok)
{
    char key[16];
    ULONG ki = 0;
    const char *s = tok;
    UBYTE op;
    const char *val;
    struct MatchCond *c;

    while (*s && *s != '=' && *s != '<' && *s != '>' && *s != '~') {
        if (ki < sizeof(key) - 1)
            key[ki++] = *s;
        s++;
    }
    key[ki] = '\0';

    if (*s == '\0') {
        out_printf("devsoak: quirks: %s: malformed match token '%s' ignored",
                   e->id, tok);
        return;
    }

    if (*s == '~') {
        op = OP_SUBSTR; s++;
    } else if (*s == '<') {
        s++;
        if (*s == '=') { op = OP_LE; s++; } else op = OP_LT;
    } else if (*s == '>') {
        s++;
        if (*s == '=') { op = OP_GE; s++; } else op = OP_GT;
    } else {
        op = OP_EQ; s++;
    }
    val = s;

    if (e->nconds >= MAX_COND_PER_ENTRY) {
        if (!e->cond_overflow_warned) {
            out_printf("devsoak: quirks: %s: too many match conditions, "
                       "ignoring the rest", e->id);
            e->cond_overflow_warned = 1;
        }
        return;
    }

    c = &e->conds[e->nconds];
    memset(c, 0, sizeof(*c));

    if (ci_eq(key, "name")) {
        c->key = MK_NAME; c->op = op;
        strncpy_safe(c->str, val, sizeof(c->str));
    } else if (ci_eq(key, "version")) {
        c->key = MK_VERSION; c->op = op; c->num = parse_dec_signed(val);
    } else if (ci_eq(key, "revision")) {
        c->key = MK_REVISION; c->op = op; c->num = parse_dec_signed(val);
    } else if (ci_eq(key, "id") && op == OP_SUBSTR) {
        c->key = MK_ID; c->op = op;
        strncpy_safe(c->str, val, sizeof(c->str));
    } else if (ci_eq(key, "kick")) {
        c->key = MK_KICK; c->op = op; c->num = parse_dec_signed(val);
    } else {
        c->key = MK_UNKNOWN;
        e->unmatchable = 1;
        out_printf("devsoak: quirks: %s: unknown match key '%s', this "
                   "entry can never match", e->id, key);
    }
    e->nconds++;
}

static void parse_match_line(struct QuirkEntry *e, char *rest)
{
    char *p = rest;

    while (*p) {
        char *start;
        UBYTE had_sep;

        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        had_sep = (*p != '\0');
        if (had_sep) *p = '\0';
        parse_match_token(e, start);
        if (had_sep) p++;
    }
}

/* ---- parsing: action line ------------------------------------------------ */

static void add_skip_tokens(struct Action *a, const char *tok, const char *id)
{
    size_t tl = strlen(tok);

    if (tl > 0 && tok[tl - 1] == '*') {
        char prefix[ID_LEN];
        size_t pl = tl - 1;
        ULONG i, found = 0;

        if (pl >= sizeof(prefix)) pl = sizeof(prefix) - 1;
        memcpy(prefix, tok, pl);
        prefix[pl] = '\0';

        for (i = 0; i < N_CMD_NAMES; i++) {
            if (ci_prefix(cmd_names[i].name, prefix, pl)) {
                if (a->nskip < MAX_SKIP_PER_ACTION) {
                    a->skipcmds[a->nskip++] = cmd_names[i].val;
                    found++;
                }
            }
        }
        if (!found) {
            out_printf("devsoak: quirks: %s: wildcard '%s' matched no "
                       "known commands", id, tok);
        }
    } else {
        UWORD cmd;
        if (cmdname_lookup(tok, &cmd)) {
            if (a->nskip < MAX_SKIP_PER_ACTION)
                a->skipcmds[a->nskip++] = cmd;
        } else {
            out_printf("devsoak: quirks: %s: unknown command '%s' in "
                       "skip action, ignored", id, tok);
        }
    }
}

/* splits "TOK1 TOK2" (whitespace separated) in place; returns 1 if both
 * tokens were found. Anything after the second token is ignored. */
static UBYTE split2(char *s, char *out1, size_t n1, char *out2, size_t n2)
{
    char *p = s;
    char *t1, *t2;

    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return 0;
    t1 = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) { *p = '\0'; p++; }

    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return 0;
    t2 = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p = '\0';

    strncpy_safe(out1, t1, n1);
    strncpy_safe(out2, t2, n2);
    return 1;
}

static void parse_action_line(struct QuirkEntry *e, char *rest)
{
    char *p = rest;
    char *verb;
    char *args;
    struct Action *a;

    while (*p == ' ' || *p == '\t') p++;
    verb = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) { *p = '\0'; p++; }
    while (*p == ' ' || *p == '\t') p++;
    args = p;

    if (g_total_actions >= MAX_ACTIONS_TOTAL) {
        if (!g_actions_full_warned) {
            out_printf("devsoak: quirks: global action limit (%ld) "
                       "reached, ignoring further actions",
                       (LONG)MAX_ACTIONS_TOTAL);
            g_actions_full_warned = 1;
        }
        return;
    }
    if (e->nactions >= MAX_ACTION_PER_ENTRY) {
        if (!e->action_overflow_warned) {
            out_printf("devsoak: quirks: %s: too many actions, ignoring "
                       "the rest", e->id);
            e->action_overflow_warned = 1;
        }
        return;
    }

    a = &e->actions[e->nactions];
    memset(a, 0, sizeof(*a));

    if (ci_eq(verb, "skip")) {
        char *q = args;
        a->verb = ACT_SKIP;
        while (*q) {
            char *ts;
            UBYTE had_sep;
            while (*q == ' ' || *q == '\t') q++;
            if (!*q) break;
            ts = q;
            while (*q && *q != ' ' && *q != '\t') q++;
            had_sep = (*q != '\0');
            if (had_sep) *q = '\0';
            add_skip_tokens(a, ts, e->id);
            if (had_sep) q++;
        }
        if (a->nskip == 0) {
            out_printf("devsoak: quirks: %s: 'skip' action with no "
                       "recognised commands, ignored", e->id);
            return;
        }
    } else if (ci_eq(verb, "expect")) {
        char cmdtok[32], errtok[32];
        UWORD cmd;
        LONG err;

        if (!split2(args, cmdtok, sizeof(cmdtok), errtok, sizeof(errtok))) {
            out_printf("devsoak: quirks: %s: 'expect' needs CMD and "
                       "ERROR, ignored", e->id);
            return;
        }
        if (!cmdname_lookup(cmdtok, &cmd)) {
            out_printf("devsoak: quirks: %s: expect: unknown command "
                       "'%s', ignored", e->id, cmdtok);
            return;
        }
        if (!errname_lookup(errtok, &err)) {
            out_printf("devsoak: quirks: %s: expect: unknown error "
                       "'%s', ignored", e->id, errtok);
            return;
        }
        a->verb = ACT_EXPECT; a->expect_cmd = cmd; a->expect_err = err;
    } else if (ci_eq(verb, "warn")) {
        a->verb = ACT_WARN;
        strncpy_safe(a->testname, args, sizeof(a->testname));
    } else if (ci_eq(verb, "skiptest")) {
        a->verb = ACT_SKIPTEST;
        strncpy_safe(a->testname, args, sizeof(a->testname));
    } else if (ci_eq(verb, "align")) {
        a->verb = ACT_ALIGN; a->numval = (ULONG)parse_dec_signed(args);
    } else if (ci_eq(verb, "nochip")) {
        a->verb = ACT_NOCHIP;
    } else if (ci_eq(verb, "maxinflight")) {
        a->verb = ACT_MAXINFLIGHT; a->numval = (ULONG)parse_dec_signed(args);
    } else if (ci_eq(verb, "maxxfer")) {
        a->verb = ACT_MAXXFER; a->numval = parse_hex_or_dec(args);
    } else if (ci_eq(verb, "norandomcmd")) {
        a->verb = ACT_NORANDOMCMD;
    } else if (ci_eq(verb, "tier")) {
        a->verb = ACT_TIER; a->tierval = parse_dec_signed(args);
    } else {
        out_printf("devsoak: quirks: %s: unknown action verb '%s', "
                   "ignored", e->id, verb);
        return;
    }

    e->nactions++;
    g_total_actions++;
}

/* ---- merge: apply one matched+eligible entry into the flat query state - */

static void add_testrec(struct TestRec *arr, ULONG *n, ULONG max,
                         const char *name, ULONG idx, const char *id,
                         const char *which)
{
    if (*n >= max) {
        out_printf("devsoak: quirks: %s table full (max %ld), '%s' from "
                   "'%s' not recorded", which, (LONG)max, name, id);
        return;
    }
    strncpy_safe(arr[*n].name, name, TESTNAME_LEN);
    arr[*n].entry_idx = idx;
    (*n)++;
}

static void apply_entry(ULONG idx)
{
    struct QuirkEntry *e = &entries[idx];
    ULONG i, j;

    for (i = 0; i < e->nactions; i++) {
        struct Action *a = &e->actions[i];

        switch (a->verb) {
        case ACT_SKIP:
            for (j = 0; j < a->nskip; j++) {
                if (n_skip < MAX_SKIP_CMDS) {
                    skip_cmds[n_skip].cmd = a->skipcmds[j];
                    skip_cmds[n_skip].entry_idx = idx;
                    n_skip++;
                } else if (!skip_overflow_warned) {
                    out_printf("devsoak: quirks: skip-command table full "
                               "(max %ld), some skips from '%s' not "
                               "recorded", (LONG)MAX_SKIP_CMDS, e->id);
                    skip_overflow_warned = 1;
                }
            }
            break;

        case ACT_EXPECT: {
            ULONG k;
            UBYTE found = 0;
            for (k = 0; k < n_expects; k++) {
                if (expects[k].cmd == a->expect_cmd) {
                    expects[k].err = a->expect_err;
                    expects[k].entry_idx = idx;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                if (n_expects < MAX_EXPECTS) {
                    expects[n_expects].cmd = a->expect_cmd;
                    expects[n_expects].err = a->expect_err;
                    expects[n_expects].entry_idx = idx;
                    n_expects++;
                } else {
                    out_printf("devsoak: quirks: expect table full (max "
                               "%ld), '%s' expect ignored",
                               (LONG)MAX_EXPECTS, e->id);
                }
            }
            break;
        }

        case ACT_WARN:
            add_testrec(warns, &n_warns, MAX_WARNS, a->testname, idx,
                        e->id, "warn");
            break;

        case ACT_SKIPTEST:
            add_testrec(skiptests, &n_skiptests, MAX_SKIPTESTS,
                        a->testname, idx, e->id, "skiptest");
            break;

        case ACT_ALIGN:
            if (a->numval > g_align_min)
                g_align_min = a->numval;
            break;

        case ACT_NOCHIP:
            g_nochip = 1;
            break;

        case ACT_MAXINFLIGHT:
            if (a->numval > 0)
                g_maxinflight = (g_maxinflight == 0 || a->numval < g_maxinflight)
                                ? a->numval : g_maxinflight;
            break;

        case ACT_MAXXFER:
            if (a->numval > 0)
                g_maxxfer = (g_maxxfer == 0 || a->numval < g_maxxfer)
                            ? a->numval : g_maxxfer;
            break;

        case ACT_NORANDOMCMD:
            g_norandomcmd = 1;
            break;

        case ACT_TIER:
            if (g_tier == -1 || a->tierval < g_tier)
                g_tier = a->tierval;
            break;

        default:
            break;
        }
    }
}

/* ---- top-level file parse ------------------------------------------------ */

static void parse_quirks_buffer(char *buf)
{
    char *p = buf;
    struct QuirkEntry *cur = NULL;

    while (*p) {
        char *line = p;
        char *nl = strchr(p, '\n');
        char *sp;
        char kwbuf[16];
        size_t klen;
        char *rest;

        if (nl) { *nl = '\0'; p = nl + 1; }
        else    { p += strlen(p); }

        trim_trailing(line);

        if (line[0] == '\0')
            continue;
        if (line[0] == ' ' || line[0] == '\t')
            continue;              /* continuation of a `because` block */
        if (line[0] == '#')
            continue;

        sp = line;
        while (*sp && *sp != ' ' && *sp != '\t') sp++;
        klen = (size_t)(sp - line);
        if (klen >= sizeof(kwbuf)) klen = sizeof(kwbuf) - 1;
        memcpy(kwbuf, line, klen);
        kwbuf[klen] = '\0';
        while (*sp == ' ' || *sp == '\t') sp++;
        rest = sp;

        if (strcmp(kwbuf, "quirk") == 0) {
            if (n_entries >= MAX_QUIRK_ENTRIES) {
                if (!entries_full_warned) {
                    out_printf("devsoak: quirks: too many quirk entries "
                               "(max %ld), ignoring '%s' and any after "
                               "it", (LONG)MAX_QUIRK_ENTRIES, rest);
                    entries_full_warned = 1;
                }
                cur = NULL;
                continue;
            }
            cur = &entries[n_entries++];
            memset(cur, 0, sizeof(*cur));
            strncpy_safe(cur->id, rest, ID_LEN);
            cur->status = ST_SUSPECTED;   /* conservative default */
        } else if (strcmp(kwbuf, "match") == 0) {
            if (cur == NULL) {
                out_printf("devsoak: quirks: 'match' with no preceding "
                           "'quirk', ignored");
                continue;
            }
            parse_match_line(cur, rest);
        } else if (strcmp(kwbuf, "because") == 0) {
            /* free text, deliberately not stored; its continuation lines
             * are skipped above via the leading-whitespace check. */
        } else if (strcmp(kwbuf, "action") == 0) {
            if (cur == NULL) {
                out_printf("devsoak: quirks: 'action' with no preceding "
                           "'quirk', ignored");
                continue;
            }
            parse_action_line(cur, rest);
        } else if (strcmp(kwbuf, "status") == 0) {
            if (cur == NULL)
                continue;
            if (ci_eq(rest, "confirmed")) cur->status = ST_CONFIRMED;
            else if (ci_eq(rest, "reported")) cur->status = ST_REPORTED;
            else if (ci_eq(rest, "suspected")) cur->status = ST_SUSPECTED;
            else
                out_printf("devsoak: quirks: %s: unknown status '%s', "
                           "treating as suspected", cur->id, rest);
        } else {
            out_printf("devsoak: quirks: unknown keyword '%s', line "
                       "ignored", kwbuf);
        }
    }
}

static UBYTE is_forced(const char *id)
{
    ULONG i;
    for (i = 0; i < cfg.nforcequirks; i++) {
        if (strcmp(cfg.forcequirk[i], id) == 0)
            return 1;
    }
    return 0;
}

static UBYTE id_exists(const char *id)
{
    ULONG i;
    for (i = 0; i < n_entries; i++) {
        if (strcmp(entries[i].id, id) == 0)
            return 1;
    }
    return 0;
}

/* ---- public API: load/apply --------------------------------------------- */

LONG quirks_load(void)
{
    BPTR  fh;
    char *path;
    ULONG i;

    n_entries = 0;
    applied_count = 0;
    entries_full_warned = 0;
    n_skip = 0; n_expects = 0; n_warns = 0; n_skiptests = 0;
    g_align_min = 0; g_nochip = 0; g_maxinflight = 0; g_maxxfer = 0;
    g_norandomcmd = 0; g_tier = -1;
    g_total_actions = 0; g_actions_full_warned = 0; skip_overflow_warned = 0;

    if (cfg.noquirks) {
        out_printf("devsoak: quirks: ignored (-K driver-under-test mode)");
        return 0;
    }

    fh = 0;
    path = NULL;

    if (cfg.quirksfile != NULL) {
        fh = Open((CONST_STRPTR)cfg.quirksfile, MODE_OLDFILE);
        if (fh == 0) {
            out_printf("devsoak: quirks: cannot open %s", cfg.quirksfile);
            return 0;
        }
        path = cfg.quirksfile;
    } else {
        fh = Open((CONST_STRPTR)"devsoak.quirks", MODE_OLDFILE);
        if (fh != 0) {
            path = "devsoak.quirks";
        } else {
            /* PROGDIR: is a 2.0+ assign; on 1.3 this Open() simply fails
             * and we fall through to "no quirks file", which is fine. */
            fh = Open((CONST_STRPTR)"PROGDIR:devsoak.quirks", MODE_OLDFILE);
            if (fh != 0)
                path = "PROGDIR:devsoak.quirks";
        }
        if (fh == 0) {
            out_printf("devsoak: quirks: no quirks file");
            return 0;
        }
    }

    {
        UBYTE *filebuf;
        LONG got, total = 0;

        filebuf = AllocMem(FILEBUF_MAX + 1, MEMF_PUBLIC);
        if (filebuf == NULL) {
            out_printf("devsoak: quirks: AllocMem failed, '%s' not loaded",
                       path);
            Close(fh);
            return 0;
        }

        for (;;) {
            got = Read(fh, filebuf + total, (LONG)(FILEBUF_MAX - (ULONG)total));
            if (got <= 0) break;
            total += got;
            if ((ULONG)total >= FILEBUF_MAX) break;
        }
        Close(fh);
        filebuf[total] = '\0';

        parse_quirks_buffer((char *)filebuf);

        FreeMem(filebuf, FILEBUF_MAX + 1);
    }

    for (i = 0; i < n_entries; i++) {
        struct QuirkEntry *e = &entries[i];
        UBYTE matched = eval_match(e);
        UBYTE forced = is_forced(e->id);

        e->matched = matched;

        if (matched && (e->status == ST_CONFIRMED || forced)) {
            apply_entry(i);
            e->applied = 1;
            applied_count++;
            out_printf("devsoak: quirks: applied %s", e->id);
        } else if (matched && !forced) {
            out_printf("devsoak: quirks: %s matches but is status %s, "
                       "not applied (-k %s to force)", e->id,
                       status_name(e->status), e->id);
        } else if (forced && !matched) {
            out_printf("devsoak: quirks: -k %s requested but its match "
                       "conditions do not hold; not applied", e->id);
        }
    }

    for (i = 0; i < cfg.nforcequirks; i++) {
        if (!id_exists(cfg.forcequirk[i])) {
            out_printf("devsoak: quirks: -k %s: no such quirk id in the "
                       "file", cfg.forcequirk[i]);
        }
    }

    return 0;
}

void quirks_cleanup(void)
{
    /* Nothing dynamic outlives quirks_load(): the file buffer is freed
     * before it returns, and every other table here is static storage. */
}

ULONG quirks_active(void)
{
    return applied_count;
}

void quirks_report(void)
{
    ULONG i;

    out_printf("devsoak: quirks: %lu applied", (ULONG)applied_count);

    for (i = 0; i < n_skip; i++) {
        out_printf("devsoak: SKIP cmd %s (quirk %s)",
                   op_cmd_name(skip_cmds[i].cmd),
                   entries[skip_cmds[i].entry_idx].id);
    }
    for (i = 0; i < n_skiptests; i++) {
        out_printf("devsoak: SKIP test %s (quirk %s)",
                   skiptests[i].name,
                   entries[skiptests[i].entry_idx].id);
    }
    for (i = 0; i < n_warns; i++) {
        out_printf("devsoak: WARN-downgrade test %s (quirk %s)",
                   warns[i].name,
                   entries[warns[i].entry_idx].id);
    }
}

/* ---- public API: queries (any task, read-only) -------------------------- */

ULONG quirk_cmd_skipped(UWORD cmd)
{
    ULONG i;
    for (i = 0; i < n_skip; i++) {
        if (skip_cmds[i].cmd == cmd)
            return 1;
    }
    return 0;
}

LONG quirk_expected_err(UWORD cmd, LONG *e)
{
    ULONG i;
    for (i = 0; i < n_expects; i++) {
        if (expects[i].cmd == cmd) {
            if (e != NULL) *e = expects[i].err;
            return 1;
        }
    }
    return 0;
}

ULONG quirk_test_skipped(const char *test)
{
    ULONG i;
    for (i = 0; i < n_skiptests; i++) {
        if (strcmp(skiptests[i].name, test) == 0)
            return 1;
    }
    return 0;
}

ULONG quirk_test_warn(const char *test)
{
    ULONG i;
    for (i = 0; i < n_warns; i++) {
        if (strcmp(warns[i].name, test) == 0)
            return 1;
    }
    return 0;
}

ULONG quirk_min_align(void)      { return g_align_min; }
ULONG quirk_nochip(void)         { return g_nochip; }
ULONG quirk_maxinflight(void)    { return g_maxinflight; }
ULONG quirk_maxxfer(void)        { return g_maxxfer; }
ULONG quirk_norandomcmd(void)    { return g_norandomcmd; }
LONG  quirk_tier(void)           { return g_tier; }

/* ---- S16.4 breadcrumb file (-P) ------------------------------------------
 * main-context only: Open() once (append-at-end semantics), a Write() per
 * breadcrumb line (unbuffered DOS handle -- each Write() is itself the
 * flush), Close() at shutdown. */

static BPTR crumb_fh;

LONG crumb_open(void)
{
    if (cfg.crumbfile == NULL)
        return 0;

    /* MODE_READWRITE: opens an existing file without truncating it, or
     * creates a new one -- exactly the "open for append" we want. */
    crumb_fh = Open((CONST_STRPTR)cfg.crumbfile, MODE_READWRITE);
    if (crumb_fh == 0) {
        out_printf("devsoak: crumb: cannot open %s", cfg.crumbfile);
        return -1;
    }
    Seek(crumb_fh, 0, OFFSET_END);
    return 0;
}

void crumb_write(const char *line)
{
    if (crumb_fh == 0)
        return;
    /* unbuffered DOS handle: each Write() IS the flush (Flush() itself
     * is V36+ and does not exist on Kickstart 1.3) */
    Write(crumb_fh, (APTR)line, (LONG)strlen(line));
    Write(crumb_fh, (APTR)"\n", 1);
}

void crumb_close(void)
{
    if (crumb_fh != 0) {
        Close(crumb_fh);
        crumb_fh = 0;
    }
}

/* ---- --resume: report the last breadcrumb and offer a quirk block ------- */

static UBYTE extract_cmdname(const char *line, char *out, size_t outsize)
{
    static const char marker[] = "about to send ";
    const char *p = strstr(line, marker);
    const char *start, *end;
    size_t n;

    if (p == NULL)
        return 0;
    start = p + (sizeof(marker) - 1);
    /* the name ends at '(' (serial breadcrumb format, "NAME(0x..)") or
     * at whitespace (-P file format, "NAME len=..") */
    end = start;
    while (*end != '\0' && *end != '(' && *end != ' ' && *end != '\t')
        end++;
    n = (size_t)(end - start);
    if (n == 0 || n >= outsize)
        return 0;
    memcpy(out, start, n);
    out[n] = '\0';
    return 1;
}

static void sanitise_id(const char *src, char *out, size_t outsize)
{
    size_t i = 0;

    if (src == NULL) src = "unknown";
    for (; *src != '\0' && i + 1 < outsize; src++) {
        char c = *src;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            c = '-';
        out[i++] = c;
    }
    out[i] = '\0';
    if (i == 0) {
        out[0] = 'x'; out[1] = '\0';
    }
}

LONG quirks_resume_report(void)
{
    BPTR   fh;
    UBYTE *filebuf;
    LONG   got, total = 0;
    char  *lastline;
    char   cmdname[40];
    UBYTE  have_cmd;
    char   idbuf[ID_LEN];

    if (cfg.crumbfile == NULL) {
        out_printf("devsoak: resume: no -P FILE given");
        return RC_FATAL;
    }

    fh = Open((CONST_STRPTR)cfg.crumbfile, MODE_OLDFILE);
    if (fh == 0) {
        out_printf("devsoak: resume: cannot open %s", cfg.crumbfile);
        return RC_FATAL;
    }

    filebuf = AllocMem(FILEBUF_MAX + 1, MEMF_PUBLIC);
    if (filebuf == NULL) {
        out_printf("devsoak: resume: AllocMem failed");
        Close(fh);
        return RC_FATAL;
    }

    for (;;) {
        got = Read(fh, filebuf + total, (LONG)(FILEBUF_MAX - (ULONG)total));
        if (got <= 0) break;
        total += got;
        if ((ULONG)total >= FILEBUF_MAX) break;
    }
    Close(fh);
    filebuf[total] = '\0';

    lastline = NULL;
    {
        char *p = (char *)filebuf;
        while (*p) {
            char *line = p;
            char *nl = strchr(p, '\n');
            if (nl) { *nl = '\0'; p = nl + 1; }
            else    { p += strlen(p); }
            trim_trailing(line);
            if (line[0] != '\0')
                lastline = line;
        }
    }

    if (lastline == NULL) {
        out_printf("devsoak: resume: %s is empty, nothing to resume from",
                   cfg.crumbfile);
        FreeMem(filebuf, FILEBUF_MAX + 1);
        return RC_FATAL;
    }

    out_printf("devsoak: resume: last breadcrumb before the crash was:");
    out_printf("%s", lastline);

    have_cmd = extract_cmdname(lastline, cmdname, sizeof(cmdname));
    sanitise_id(cfg.device, idbuf, sizeof(idbuf));

    out_printf("devsoak: resume: paste this into your quirks file after "
               "verifying it:");
    out_printf("quirk   suspected-crash-%s", idbuf);
    out_printf("match   name=%s", cfg.device != NULL ? cfg.device : "?");
    out_printf("because devsoak crashed/hung after this command was "
               "issued; see the");
    out_printf("        breadcrumb log %s.",
               cfg.crumbfile != NULL ? cfg.crumbfile : "?");
    if (have_cmd && strcmp(cmdname, "?") != 0) {
        out_printf("action  skip %s", cmdname);
    } else {
        out_printf("devsoak: resume: could not identify a command name "
                   "from the breadcrumb line; add the action by hand, "
                   "e.g. 'action skip <CMD>'.");
    }
    out_printf("status  suspected");

    FreeMem(filebuf, FILEBUF_MAX + 1);
    return RC_CLEAN;
}
