/*
 * dosdev.c - partition mode: resolve a DOS device name (DH1:) to the exec
 * device/unit/geometry it is mounted on, and inhibit/uninhibit the handler
 * for the duration of the run.
 *
 * Runs in main (a DOS Process), after out_init() -- unlike args.c this file
 * is allowed to print. All of the DosList structures below are BCPL: BPTR
 * fields are longword addresses shifted right 2 (BADDR() undoes that) and
 * BSTR fields are BPTRs to a length-prefixed (not NUL-terminated) string.
 * struct DosList is the same layout whether reached via LockDosList()
 * (V36+) or by hand-walking DOSBase->dl_Root->rn_Info->di_DevInfo under
 * Forbid() (Kickstart 1.3); only the list-walking API differs.
 */

#include "devsoak.h"

#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <string.h>

extern struct ExecBase   *SysBase;
extern struct DosLibrary *DOSBase;

/* ---- hand-rolled LVO calls for the V36+ DosList API ----
 * -mcrt=nix13 (see README "Building") picks up bebbo gcc's *ndk13-include*
 * tree ahead of the full NDK on the include search path -- a curated
 * Kickstart-1.3-safe subset that deliberately omits LockDosList(),
 * UnLockDosList(), FindDosEntry(), NextDosEntry() and DoPkt() (their
 * structs/#defines are all still present; only these five V36+ calls'
 * proto/inline glue is missing). Called only under the lib_Version >= 36
 * gate below, so this is exactly output.c's RawPutChar situation: no
 * header provides the call, so it is hand-rolled as a direct LVO jsr with
 * pinned argument registers (dos.library's documented register
 * assignments, from the ndk-include inline/dos.h this build does not use).
 */

static struct DosList *call_LockDosList(ULONG flags)
{
    register struct DosLibrary *a6 asm("a6") = DOSBase;
    register ULONG               d1 asm("d1") = flags;
    register struct DosList     *d0 asm("d0");

    asm volatile ("jsr -654(a6)"
                  : "=r" (d0)
                  : "r" (a6), "r" (d1)
                  : "a0", "a1", "cc", "memory");
    return d0;
}

static void call_UnLockDosList(ULONG flags)
{
    register struct DosLibrary *a6 asm("a6") = DOSBase;
    register ULONG               d1 asm("d1") = flags;

    asm volatile ("jsr -660(a6)"
                  :
                  : "r" (a6), "r" (d1)
                  : "d0", "a0", "a1", "cc", "memory");
}

static struct DosList *call_FindDosEntry(struct DosList *dlist,
                                          CONST_STRPTR name, ULONG flags)
{
    register struct DosLibrary *a6 asm("a6") = DOSBase;
    register struct DosList     *d1 asm("d1") = dlist;
    register CONST_STRPTR        d2 asm("d2") = name;
    register ULONG               d3 asm("d3") = flags;
    register struct DosList     *d0 asm("d0");

    asm volatile ("jsr -684(a6)"
                  : "=r" (d0)
                  : "r" (a6), "r" (d1), "r" (d2), "r" (d3)
                  : "a0", "a1", "cc", "memory");
    return d0;
}

static struct DosList *call_NextDosEntry(struct DosList *dlist, ULONG flags)
{
    register struct DosLibrary *a6 asm("a6") = DOSBase;
    register struct DosList     *d1 asm("d1") = dlist;
    register ULONG               d2 asm("d2") = flags;
    register struct DosList     *d0 asm("d0");

    asm volatile ("jsr -690(a6)"
                  : "=r" (d0)
                  : "r" (a6), "r" (d1), "r" (d2)
                  : "a0", "a1", "cc", "memory");
    return d0;
}

static LONG call_DoPkt(struct MsgPort *port, LONG action, LONG arg1,
                        LONG arg2, LONG arg3, LONG arg4, LONG arg5)
{
    register struct DosLibrary *a6 asm("a6") = DOSBase;
    register struct MsgPort    *d1 asm("d1") = port;
    register LONG                d2 asm("d2") = action;
    register LONG                d3 asm("d3") = arg1;
    register LONG                d4 asm("d4") = arg2;
    register LONG                d5 asm("d5") = arg3;
    register LONG                d6 asm("d6") = arg4;
    register LONG                d7 asm("d7") = arg5;
    register LONG                d0 asm("d0");

    asm volatile ("jsr -240(a6)"
                  : "=r" (d0)
                  : "r" (a6), "r" (d1), "r" (d2), "r" (d3), "r" (d4),
                    "r" (d5), "r" (d6), "r" (d7)
                  : "a0", "a1", "cc", "memory");
    return d0;
}

/* ---- tiny hand-rolled string helpers (no strcasecmp on this toolchain;
 * see quirks.c for the same pattern) ---- */

static int my_tolower(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 'a';
    return c;
}

/* case-insensitive compare of a BSTR (BCPL length-prefixed) against a
 * NUL-terminated C string. b == 0 never matches. */
static UBYTE bstr_ci_eq(BSTR b, const char *cname)
{
    UBYTE *p;
    UBYTE len, i;

    if (b == 0)
        return 0;
    p = (UBYTE *)BADDR(b);
    len = p[0];
    if ((size_t)len != strlen(cname))
        return 0;
    for (i = 0; i < len; i++) {
        if (my_tolower(p[1 + i]) != my_tolower((unsigned char)cname[i]))
            return 0;
    }
    return 1;
}

/* copy a BSTR into a NUL-terminated C buffer, truncating to fit */
static void bstr_to_c(BSTR b, char *out, size_t outsize)
{
    UBYTE *p;
    UBYTE len, i;

    if (outsize == 0)
        return;
    if (b == 0) {
        out[0] = '\0';
        return;
    }
    p = (UBYTE *)BADDR(b);
    len = p[0];
    if ((size_t)len >= outsize)
        len = (UBYTE)(outsize - 1);
    for (i = 0; i < len; i++)
        out[i] = (char)p[1 + i];
    out[len] = '\0';
}

/* A BPTR field is "plausible" if it is nonzero and BADDR()s to a nonzero,
 * even address -- BADDR always produces a multiple of 4 when raw != 0, so
 * this really only rules out raw == 0, but it documents the invariant we
 * are relying on before dereferencing driver/filesystem-supplied data. */
static UBYTE plausible_bptr(ULONG raw, APTR *outptr)
{
    APTR p;

    if (raw == 0)
        return 0;
    p = (APTR)BADDR(raw);
    if (p == NULL || (((ULONG)p) & 1) != 0)
        return 0;
    *outptr = p;
    return 1;
}

/* Validate a DLT_DEVICE DosList node hard before trusting any of the
 * filesystem-startup data it points to, then extract the fields
 * partition mode needs. 0 on success; on failure prints the one-line
 * reason and returns -1. */
static LONG validate_and_extract(struct DosList *node, struct PartInfo *out,
                                  const char *origname)
{
    APTR startupptr, environptr;
    struct FileSysStartupMsg *fssm;
    struct DosEnvec *de;
    UBYTE *devbstr;
    UBYTE devlen;

    if (node->dol_Type != DLT_DEVICE) {
        out_printf("devsoak: %s: not a DOS device list entry", origname);
        return -1;
    }

    if (!plausible_bptr(node->dol_misc.dol_handler.dol_Startup, &startupptr)) {
        out_printf("devsoak: %s has no filesystem startup (not a disk "
                   "partition)", origname);
        return -1;
    }
    fssm = (struct FileSysStartupMsg *)startupptr;

    if (fssm->fssm_Device == 0) {
        out_printf("devsoak: %s: filesystem startup has no device name",
                   origname);
        return -1;
    }
    devbstr = (UBYTE *)BADDR(fssm->fssm_Device);
    devlen = devbstr[0];
    if (devlen < 1) {
        out_printf("devsoak: %s: filesystem startup device name is empty",
                   origname);
        return -1;
    }

    if (!plausible_bptr((ULONG)fssm->fssm_Environ, &environptr)) {
        out_printf("devsoak: %s: filesystem startup has no environment "
                   "table", origname);
        return -1;
    }
    de = (struct DosEnvec *)environptr;

    if (de->de_TableSize < DE_UPPERCYL) {
        out_printf("devsoak: %s: environment table too short (size %ld, "
                   "need >= %ld)", origname, (LONG)de->de_TableSize,
                   (LONG)DE_UPPERCYL);
        return -1;
    }
    if (de->de_SizeBlock == 0 || de->de_Surfaces == 0 ||
        de->de_BlocksPerTrack == 0) {
        out_printf("devsoak: %s: implausible geometry in environment table "
                   "(zero SizeBlock/Surfaces/BlocksPerTrack)", origname);
        return -1;
    }
    if (de->de_HighCyl < de->de_LowCyl) {
        out_printf("devsoak: %s: environment table has HighCyl < LowCyl",
                   origname);
        return -1;
    }

    bstr_to_c(fssm->fssm_Device, out->devname, sizeof(out->devname));
    out->unit = (LONG)fssm->fssm_Unit;
    out->opendevice_flags = fssm->fssm_Flags;
    out->sizeblock = de->de_SizeBlock;
    out->surfaces = de->de_Surfaces;
    out->blockspertrack = de->de_BlocksPerTrack;
    out->lowcyl = de->de_LowCyl;
    out->highcyl = de->de_HighCyl;
    out->handler = node->dol_Task;

    return 0;
}

LONG dosdev_resolve(const char *name, struct PartInfo *out)
{
    char   namebuf[32];
    size_t n;
    struct DosList *found = NULL;
    UBYTE  have_v36;
    LONG   rc = -1;

    memset(out, 0, sizeof(*out));

    n = strlen(name);
    if (n > 0 && name[n - 1] == ':')
        n--;
    if (n == 0 || n >= sizeof(namebuf)) {
        out_printf("devsoak: %s: bad DOS device name", name);
        return -1;
    }
    memcpy(namebuf, name, n);
    namebuf[n] = '\0';

    have_v36 = (SysBase->LibNode.lib_Version >= 36) ? 1 : 0;

    if (have_v36) {
        struct DosList *dl, *v;

        dl = call_LockDosList(LDF_DEVICES | LDF_VOLUMES | LDF_READ);
        found = call_FindDosEntry(dl, (STRPTR)namebuf, LDF_DEVICES);
        if (found == NULL) {
            out_printf("devsoak: %s: not found in the DOS device list", name);
        } else if (validate_and_extract(found, out, name) == 0) {
            rc = 0;
            if (out->handler != NULL) {
                v = dl;
                while ((v = call_NextDosEntry(v, LDF_VOLUMES)) != NULL) {
                    if (v->dol_Task == out->handler) {
                        bstr_to_c(v->dol_Name, out->volname,
                                  sizeof(out->volname));
                        out->have_volume = 1;
                        break;
                    }
                }
            }
        }
        call_UnLockDosList(LDF_DEVICES | LDF_VOLUMES | LDF_READ);
    } else {
        struct RootNode *root;
        struct DosInfo  *info;
        struct DosList  *node;

        Forbid();

        root = ((struct DosLibrary *)DOSBase)->dl_Root;
        info = (struct DosInfo *)BADDR(root->rn_Info);

        for (node = (struct DosList *)BADDR(info->di_DevInfo); node != NULL;
             node = (struct DosList *)BADDR(node->dol_Next)) {
            if (node->dol_Type == DLT_DEVICE && bstr_ci_eq(node->dol_Name, namebuf)) {
                found = node;
                break;
            }
        }

        if (found == NULL) {
            out_printf("devsoak: %s: not found in the DOS device list", name);
        } else if (validate_and_extract(found, out, name) == 0) {
            rc = 0;
            if (out->handler != NULL) {
                for (node = (struct DosList *)BADDR(info->di_DevInfo);
                     node != NULL;
                     node = (struct DosList *)BADDR(node->dol_Next)) {
                    if (node->dol_Type == DLT_VOLUME &&
                        node->dol_Task == out->handler) {
                        bstr_to_c(node->dol_Name, out->volname,
                                  sizeof(out->volname));
                        out->have_volume = 1;
                        break;
                    }
                }
            }
        }

        Permit();
    }

    return rc;
}

/* ---- ACTION_INHIBIT (Kickstart 1.3 has no DoPkt(); hand-roll the
 * packet the way DoPkt() itself does internally on V36+) ---- */

static LONG send_inhibit_pre36(struct MsgPort *handler, LONG arg1)
{
    struct StandardPacket sp;
    struct MsgPort *replyport = &((struct Process *)FindTask(NULL))->pr_MsgPort;
    struct Message *reply;

    memset(&sp, 0, sizeof(sp));
    sp.sp_Msg.mn_Node.ln_Name = (char *)&sp.sp_Pkt;
    sp.sp_Msg.mn_Length = sizeof(struct DosPacket);
    sp.sp_Msg.mn_ReplyPort = replyport;
    sp.sp_Pkt.dp_Link = &sp.sp_Msg;
    sp.sp_Pkt.dp_Port = replyport;
    sp.sp_Pkt.dp_Type = ACTION_INHIBIT;
    sp.sp_Pkt.dp_Arg1 = arg1;

    PutMsg(handler, &sp.sp_Msg);
    do {
        WaitPort(replyport);
        reply = GetMsg(replyport);
    } while (reply != &sp.sp_Msg);

    return sp.sp_Pkt.dp_Res1;
}

static LONG send_inhibit(struct MsgPort *handler, LONG arg1)
{
    if (SysBase->LibNode.lib_Version >= 36)
        return call_DoPkt(handler, ACTION_INHIBIT, arg1, 0, 0, 0, 0);
    return send_inhibit_pre36(handler, arg1);
}

LONG dosdev_inhibit(struct MsgPort *handler, UBYTE *inhibited)
{
    *inhibited = 0;

    if (handler == NULL) {
        out_printf("devsoak: handler not started; nothing to inhibit");
        return 0;
    }

    if (send_inhibit(handler, DOSTRUE) == 0) {
        out_printf("devsoak: ACTION_INHIBIT failed; the filesystem would "
                   "fight the test traffic, refusing to run");
        return -1;
    }

    *inhibited = 1;
    out_printf("devsoak: partition inhibited (filesystem access blocked "
               "for the run)");
    return 0;
}

void dosdev_uninhibit(struct MsgPort *handler, UBYTE *inhibited)
{
    if (!*inhibited || handler == NULL)
        return;

    if (send_inhibit(handler, DOSFALSE) == 0) {
        out_printf("devsoak: warning: ACTION_INHIBIT (uninhibit) failed; "
                   "the filesystem may still be inhibited");
    }
    *inhibited = 0;
}
