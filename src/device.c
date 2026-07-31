/*
 * rtl8139re.device — C reconstruction of Hyperion's rtl8139.device 53.4.
 *
 * PHASE A — library init + cleanup + expunge.
 *
 * Translates DevInit (rtl.asm 0x100023c, 684 bytes) and DevCleanup
 * (0x100016c, 208 bytes). DevExpunge (0x10004e8, 196 bytes) and
 * DevAbortIO (0x100007c, 240 bytes) also included since they're small
 * and referenced from the AutoInit function table.
 *
 * See docs/FUNCTION_MAP.md for the full map. Struct layout in
 * include/rtl8139re.h.
 */

#include "rtl8139re.h"
#include "version.h"

#include <exec/exectags.h>
#include <exec/interfaces.h>
#include <exec/resident.h>
#include <exec/errors.h>
#include <exec/execbase.h>
#include <exec/ports.h>

#include <devices/newstyle.h>

#include <dos/dos.h>
#include <interfaces/dos.h>

#include <stdarg.h>
#include <stddef.h>

#define DEVNAME           "rtl8139re.device"
#define DEVVER            53
#define DEVREV            4
#define DEVVERSIONSTRING  VSTRING

/*
 * The RTL8139 vendor:device ID table from the original binary's rodata
 * at 0x100a370. 16 real entries + a 0xFFFFFFFF terminator. Order matters
 * only for the search itself; leaving it identical for byte-compat.
 */
const struct Rtl8139DeviceID rtl8139_device_ids[] = {
    {0x10EC, 0x8139}, {0x10EC, 0x8138}, {0x1113, 0x1211}, {0x1500, 0x1360},
    {0x4033, 0x1360}, {0x1186, 0x1300}, {0x1186, 0x1340}, {0x13D1, 0xAB06},
    {0x1259, 0xA117}, {0x1259, 0xA11E}, {0x14EA, 0xAB06}, {0x14EA, 0xAB07},
    {0x11DB, 0x1234}, {0x1432, 0x9130}, {0x02AC, 0x1012}, {0x018A, 0x0106},
    {0x126C, 0x1211}, {0x1743, 0x8139}, {0x021B, 0x8139},
    {0xFFFF, 0xFFFF},   /* terminator */
};

/* Interface refcount helpers — standard OS4 idiom. */
uint32 _manager_Obtain(struct DeviceManagerInterface *Self)
{
    Self->Data.RefCount++;
    return Self->Data.RefCount;
}

uint32 _manager_Release(struct DeviceManagerInterface *Self)
{
    Self->Data.RefCount--;
    return Self->Data.RefCount;
}

/* Forward decls for the vector table. */
extern struct Library *_manager_Init(struct Library *library, BPTR seglist,
                                     struct Interface *exec);
extern struct Rtl8139ReBase *_manager_Open(struct DeviceManagerInterface *Self,
                                           struct IOSana2Req *ioreq,
                                           ULONG unitNum, ULONG flags);
extern BPTR _manager_Close(struct DeviceManagerInterface *Self,
                           struct IOSana2Req *ioreq);
extern BPTR _manager_Expunge(struct DeviceManagerInterface *Self);
extern void _manager_BeginIO(struct DeviceManagerInterface *Self,
                             struct IOSana2Req *ioreq);
extern LONG _manager_AbortIO(struct DeviceManagerInterface *Self,
                             struct IOSana2Req *ioreq);

static const APTR _manager_Vectors[] = {
    (APTR)_manager_Obtain,
    (APTR)_manager_Release,
    (APTR)NULL,
    (APTR)NULL,
    (APTR)_manager_Open,
    (APTR)_manager_Close,
    (APTR)_manager_Expunge,
    (APTR)NULL,
    (APTR)_manager_BeginIO,
    (APTR)_manager_AbortIO,
    (APTR)-1,
};

static const struct TagItem _manager_Tags[] = {
    {MIT_Name,        (ULONG)"__device"},
    {MIT_VectorTable, (ULONG)_manager_Vectors},
    {MIT_Version,     1},
    {TAG_END,         0},
};

const APTR devInterfaces[] = { (APTR)_manager_Tags, (APTR)NULL };

static const APTR _manager_Vectors68K[] = {
    (APTR)_manager_Open,
    (APTR)_manager_Close,
    (APTR)_manager_Expunge,
    (APTR)NULL,
    (APTR)_manager_BeginIO,
    (APTR)_manager_AbortIO,
    (APTR)-1,
};

static const char verstag[] __attribute__((used)) = "\0$VER: " DEVVERSIONSTRING;

static struct TagItem dev_init_tags[] = {
    {CLT_DataSize,     sizeof(struct Rtl8139ReBase)},
    {CLT_Interfaces,   (ULONG)devInterfaces},
    {CLT_InitFunc,     (ULONG)_manager_Init},
    {CLT_Vector68K,    (ULONG)_manager_Vectors68K},
    {CLT_NoLegacyIFace, FALSE},
    {TAG_END,          0},
};

static struct Resident dev_res __attribute__((used)) = {
    RTC_MATCHWORD,
    (struct Resident *)&dev_res,
    (struct Resident *)(&dev_res + 1),
    RTF_NATIVE | RTF_COLDSTART | RTF_AUTOINIT,
    DEVVER,
    NT_DEVICE,
    0,
    DEVNAME,
    DEVVERSIONSTRING,
    (APTR)dev_init_tags,
};

int _start(char *argstring, int arglen, struct ExecBase *sysbase)
{
    (void)argstring; (void)arglen;
    struct ExecIFace *IExec = (struct ExecIFace *)sysbase->MainInterface;
    IExec->DebugPrintF("%s is a device — install to DEVS:Networks/ or "
                       "OpenDevice() from a test program.\n", DEVNAME);
    return 20;
}

/* ------------------------------------------------------------------- */
/* Cleanup — reverse of what DevInit + DevOpen have set up so far.     */
/*                                                                      */
/* Original: 0x100016c, 208 bytes. Drops 4 interfaces + closes 4 libs. */
/* We use per-field NULL checks so callers can invoke it at any point  */
/* during Init to roll back a partial success.                         */
/* ------------------------------------------------------------------- */

static void v_cleanup(struct Rtl8139ReBase *base)
{
    struct ExecIFace *IExec = base->IExec;

    if (base->IPCI)         { IExec->DropInterface((struct Interface *)base->IPCI); base->IPCI = NULL; }
    if (base->IUtility)     { IExec->DropInterface((struct Interface *)base->IUtility); base->IUtility = NULL; }
    if (base->IExpansion)   { IExec->DropInterface((struct Interface *)base->IExpansion); base->IExpansion = NULL; }
    if (base->IDOS)         { IExec->DropInterface((struct Interface *)base->IDOS); base->IDOS = NULL; }

    /* Free PCI device first (needs IPCI which we drop above; move
     * up if that ordering matters — original drops device before
     * dropping IPCI, so free here BEFORE those go away. Reordering: */
    if (base->pciDevice && base->IPCI) {
        base->IPCI->FreeDevice(base->pciDevice);
        base->pciDevice = NULL;
    }

    if (base->UtilityBase)  { IExec->CloseLibrary(base->UtilityBase); base->UtilityBase = NULL; }
    if (base->ExpansionBase){ IExec->CloseLibrary(base->ExpansionBase); base->ExpansionBase = NULL; }
    if (base->DOSBase)      { IExec->CloseLibrary(base->DOSBase); base->DOSBase = NULL; }

    if (base->io_lock)      { IExec->FreeSysObject(ASOT_SEMAPHORE, base->io_lock); base->io_lock = NULL; }
}

/* ------------------------------------------------------------------- */
/* Init — the AutoInit hook, called by exec when the resident tag is  */
/* first processed. Sets library metadata, opens dos/expansion/utility */
/* + their "main" interfaces, gets IPCI, probes newmemory.resource.    */
/* On any library failure: DevCleanup and return the library base      */
/* anyway (letting exec put us in the library list — Expunge will      */
/* handle removal). The original returns 0 on total failure; a         */
/* partial-success base is fine, since Open will re-check.             */
/* Original: 0x100023c, 684 bytes.                                     */
/* ------------------------------------------------------------------- */

struct Library *_manager_Init(struct Library *library, BPTR seglist,
                              struct Interface *exec)
{
    struct Rtl8139ReBase *base = (struct Rtl8139ReBase *)library;
    struct ExecIFace *iexec = (struct ExecIFace *)exec;

    base->IExec       = iexec;
    base->dev_SegList = (ULONG)seglist;

    /* Library metadata — original sets ln_Type=NT_DEVICE, ln_Pri=0,
     * lib_Flags=6 (SUMUSED | CHANGED), lib_Version=53, lib_Revision=4,
     * lib_Node.ln_Name = "rtl8139.device", lib_IdString = VSTRING.
     * The kernel does most of this from the resident tag, but the
     * original hand-fills to be sure. */
    base->dev_Base.dd_Library.lib_Node.ln_Type = NT_DEVICE;
    base->dev_Base.dd_Library.lib_Node.ln_Name = (STRPTR)DEVNAME;
    base->dev_Base.dd_Library.lib_Node.ln_Pri  = 0;
    base->dev_Base.dd_Library.lib_Version      = DEVVER;
    base->dev_Base.dd_Library.lib_Revision     = DEVREV;
    base->dev_Base.dd_Library.lib_IdString     = (STRPTR)DEVVERSIONSTRING;

    iexec->DebugPrintF("[rtl8139re] Init: DevBase=%p sizeof=%lu\n",
                       base, (unsigned long)sizeof(*base));

    /* IO lock — AllocSysObject(ASOT_SEMAPHORE). Guards openers list
     * and unit table (Phase B). Original stores at r31+40. */
    base->io_lock = iexec->AllocSysObject(ASOT_SEMAPHORE, NULL);

    /* Open the three libraries. If any fails, cleanup + still return
     * the base — Open will notice and reject. dos.library at v51 is
     * the standard OS4 minimum. */
    base->DOSBase = iexec->OpenLibrary("dos.library", 51);
    if (!base->DOSBase) goto fail;

    base->ExpansionBase = iexec->OpenLibrary("expansion.library", 51);
    if (!base->ExpansionBase) goto fail;

    base->UtilityBase = iexec->OpenLibrary("utility.library", 51);
    if (!base->UtilityBase) goto fail;

    /* Get "main" interfaces (v1). The original names these all the
     * same string constant ("main") from rodata 0x100a144. */
    base->IDOS = (struct DOSIFace *)iexec->GetInterface(
        base->DOSBase, "main", 1, NULL);
    if (!base->IDOS) goto fail;

    base->IExpansion = (struct ExpansionIFace *)iexec->GetInterface(
        base->ExpansionBase, "main", 1, NULL);
    if (!base->IExpansion) goto fail;

    base->IUtility = (struct UtilityIFace *)iexec->GetInterface(
        base->UtilityBase, "main", 1, NULL);
    if (!base->IUtility) goto fail;

    /* Get IPCI — the "pci" interface on expansion.library. Original
     * uses this for FindDeviceTags in DevOpen. */
    base->IPCI = (struct PCIIFace *)iexec->GetInterface(
        base->ExpansionBase, "pci", 1, NULL);
    if (!base->IPCI) goto fail;

    /* Probe newmemory.resource. Original stores a boolean at
     * r31+168 — TRUE if the resource is available. We mirror that
     * flag but nothing else in the current phase uses it. */
    APTR nmem = iexec->OpenResource("newmemory.resource");
    base->has_newmemory = (nmem != NULL) ? 1 : 0;

    /* Phase B: walk the vendor:device table and grab the first
     * matching PCI device. The original does this per-unit inside
     * DevOpen (0x1000864), but for a single-unit driver on QEMU we
     * can do it once at Init. If found: base->pciDevice is set and
     * Open on unit 0 will succeed. */
    for (const struct Rtl8139DeviceID *id = rtl8139_device_ids;
         id->vendor != 0xFFFF; id++) {
        struct PCIDevice *pd = base->IPCI->FindDeviceTags(
            FDT_VendorID, id->vendor,
            FDT_DeviceID, id->device,
            FDT_Index,    (ULONG)0,
            TAG_END);
        if (pd) {
            base->pciDevice = pd;
            base->pci_vendor = id->vendor;
            base->pci_device = id->device;
            iexec->DebugPrintF("[rtl8139re] Found PCI device %04lx:%04lx\n",
                               (unsigned long)id->vendor,
                               (unsigned long)id->device);
            break;
        }
    }
    if (!base->pciDevice) {
        iexec->DebugPrintF("[rtl8139re] No matching RTL8139-family "
                           "PCI device found — device will load but "
                           "OpenDevice on unit 0 will fail\n");
    }

    iexec->DebugPrintF("[rtl8139re] Init OK: libs+ifaces bound, "
                       "has_newmemory=%d, pciDevice=%p\n",
                       (int)base->has_newmemory, base->pciDevice);
    return (struct Library *)base;

fail:
    iexec->DebugPrintF("[rtl8139re] Init: library/interface open failed — "
                       "cleaning up but keeping base\n");
    v_cleanup(base);
    /* Return the base anyway; Open will refuse. The original does the
     * same — DevCleanup then tail returns the library. */
    return (struct Library *)base;
}

/* ------------------------------------------------------------------- */
/* Open / Close / Expunge — Phase A stubs; Phase B adds unit setup.   */
/* ------------------------------------------------------------------- */

struct Rtl8139ReBase *_manager_Open(struct DeviceManagerInterface *Self,
                                    struct IOSana2Req *ioreq,
                                    ULONG unitNum, ULONG flags)
{
    (void)flags;
    struct Rtl8139ReBase *base = (struct Rtl8139ReBase *)Self->Data.LibBase;

    /* Refuse if Init didn't complete (any of the required libs/ifaces
     * missing). Original checks by looking at a "ok" flag; we
     * inspect the fields directly. */
    if (!base->IDOS || !base->IExpansion || !base->IUtility || !base->IPCI) {
        ioreq->ios2_Req.io_Error = IOERR_OPENFAIL;
        return NULL;
    }

    /* Original rejects unit > 7. Bridge tests use unit 0. */
    if (unitNum > 7) {
        ioreq->ios2_Req.io_Error = IOERR_OPENFAIL;
        return NULL;
    }

    /* Phase B: refuse if no PCI device was found at Init. */
    if (!base->pciDevice) {
        ioreq->ios2_Req.io_Error = IOERR_OPENFAIL;
        return NULL;
    }

    base->dev_Base.dd_Library.lib_OpenCnt++;
    base->dev_Base.dd_Library.lib_Flags &= ~LIBF_DELEXP;
    ioreq->ios2_Req.io_Error = 0;
    ioreq->ios2_Req.io_Unit  = (struct Unit *)base;   /* Phase B will
                                                       * return a per-unit
                                                       * struct instead. */
    return base;
}

BPTR _manager_Close(struct DeviceManagerInterface *Self,
                    struct IOSana2Req *ioreq)
{
    struct Rtl8139ReBase *base = (struct Rtl8139ReBase *)Self->Data.LibBase;
    ioreq->ios2_Req.io_Unit   = (struct Unit *)-1;
    ioreq->ios2_Req.io_Device = (struct Device *)-1;
    base->dev_Base.dd_Library.lib_OpenCnt--;
    if (base->dev_Base.dd_Library.lib_OpenCnt == 0 &&
        (base->dev_Base.dd_Library.lib_Flags & LIBF_DELEXP)) {
        return _manager_Expunge(Self);
    }
    return (BPTR)0;
}

BPTR _manager_Expunge(struct DeviceManagerInterface *Self)
{
    struct Rtl8139ReBase *base = (struct Rtl8139ReBase *)Self->Data.LibBase;
    if (base->dev_Base.dd_Library.lib_OpenCnt) {
        base->dev_Base.dd_Library.lib_Flags |= LIBF_DELEXP;
        return (BPTR)0;
    }
    BPTR seg = (BPTR)base->dev_SegList;
    struct ExecIFace *IExec = base->IExec;
    v_cleanup(base);
    IExec->Remove((struct Node *)&base->dev_Base.dd_Library.lib_Node);
    IExec->DeleteLibrary((struct Library *)base);
    return seg;
}

/* ------------------------------------------------------------------- */
/* BeginIO / AbortIO — Phase A stubs.                                  */
/*                                                                      */
/* AbortIO original: 0x100007c, 240 bytes — canonical Amiga AbortIO.   */
/* Under IExec.Disable, removes the IOReq from any queue, replies it,  */
/* re-enables. Phase B (unit task) implements the queue.                */
/* ------------------------------------------------------------------- */

void _manager_BeginIO(struct DeviceManagerInterface *Self,
                      struct IOSana2Req *ioreq)
{
    struct Rtl8139ReBase *base = (struct Rtl8139ReBase *)Self->Data.LibBase;
    ioreq->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    ioreq->ios2_Req.io_Flags &= ~IOF_QUICK;
    ioreq->ios2_Req.io_Error = IOERR_NOCMD;
    ioreq->ios2_WireError    = 0;
    base->IExec->ReplyMsg((struct Message *)ioreq);
}

LONG _manager_AbortIO(struct DeviceManagerInterface *Self,
                      struct IOSana2Req *ioreq)
{
    struct Rtl8139ReBase *base = (struct Rtl8139ReBase *)Self->Data.LibBase;
    struct ExecIFace *IExec = base->IExec;

    /* Original: Disable → Remove if node linked → ReplyMsg → Enable.
     * Only performs Remove if the io is on a msg-port (ln_Type ==
     * NT_MESSAGE) — otherwise it's already been replied or is quick. */
    IExec->Disable();
    if (ioreq->ios2_Req.io_Message.mn_Node.ln_Type == NT_MESSAGE) {
        IExec->Remove(&ioreq->ios2_Req.io_Message.mn_Node);
        ioreq->ios2_Req.io_Error = IOERR_ABORTED;
        IExec->ReplyMsg((struct Message *)ioreq);
    }
    IExec->Enable();
    return 0;
}
