/*
 * rtl8139re.device — C reconstruction of Hyperion's rtl8139.device 53.4.
 *
 * PHASE 0 — SKELETON.
 *
 * Enough of an OS4 SANA-II device to load, be OpenDevice'd, and reply
 * IOERR_NOCMD to every request. No PCI, no hardware, no dispatch — just
 * the resident-tag glue so we can prove the docker → push → load cycle.
 *
 * Structure lifted from virte1000/src/device.c (same author's e1000 driver
 * work) which had the OS4-native resident-tag layout, 68k jump table,
 * DeviceManagerInterface vector table, and RTF_AUTOINIT semantics all
 * ironed out. Each subsequent phase (A, B, C, ...) will pull in one
 * chunk of translated behavior from the RE'd rtl.asm at
 * /Users/chris/tmp_bsd/rtl.asm.
 */

#include "rtl8139re.h"
#include "version.h"

#include <exec/exectags.h>
#include <exec/interfaces.h>
#include <exec/resident.h>
#include <exec/errors.h>
#include <exec/execbase.h>

#include <devices/newstyle.h>

#include <dos/dos.h>
#include <interfaces/dos.h>

#include <exec/ports.h>

#include <stdarg.h>
#include <stddef.h>

#define DEVNAME           "rtl8139re.device"
#define DEVVER            0
#define DEVREV            1
#define DEVVERSIONSTRING  VSTRING

/*
 * Manager Obtain/Release — reference count on the interface. Standard
 * OS4 idiom; every device driver's manager interface has this.
 */
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

/* OS4 vector table for DeviceManagerInterface. Order and terminator are
 * mandated by exec/interfaces.h — do not reorder. */
static const APTR _manager_Vectors[] = {
    (APTR)_manager_Obtain,
    (APTR)_manager_Release,
    (APTR)NULL,             /* Expunge-slot on Interface — unused */
    (APTR)NULL,             /* Clone — unused */
    (APTR)_manager_Open,
    (APTR)_manager_Close,
    (APTR)_manager_Expunge,
    (APTR)NULL,             /* Reserved */
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

/* 68k-compat jump table. Every shipping OS4 .device includes this to
 * support classic-Amiga callers that go through negative library
 * offsets. Costs nothing to include. */
static const APTR _manager_Vectors68K[] = {
    (APTR)_manager_Open,     /* -6  */
    (APTR)_manager_Close,    /* -12 */
    (APTR)_manager_Expunge,  /* -18 */
    (APTR)NULL,              /* -24 Reserved */
    (APTR)_manager_BeginIO,  /* -30 */
    (APTR)_manager_AbortIO,  /* -36 */
    (APTR)-1,
};

/* Version cookie for the AmigaDOS `Version` command. */
static const char verstag[] __attribute__((used)) = "\0$VER: " DEVVERSIONSTRING;

/* Init tag list — CLT_DataSize tells the kernel how big our libBase is. */
static struct TagItem dev_init_tags[] = {
    {CLT_DataSize,     sizeof(struct Rtl8139ReBase)},
    {CLT_Interfaces,   (ULONG)devInterfaces},
    {CLT_InitFunc,     (ULONG)_manager_Init},
    {CLT_Vector68K,    (ULONG)_manager_Vectors68K},
    {CLT_NoLegacyIFace, FALSE},
    {TAG_END,          0},
};

/* Resident struct — NOT const. Every working OS4 kickstart device
 * places this in writable .data because the kernel/DOS may patch
 * fields during boot binding. */
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

/* Shell entry point. Not runnable — print a hint and exit. */
int _start(char *argstring, int arglen, struct ExecBase *sysbase)
{
    (void)argstring; (void)arglen;
    struct ExecIFace *IExec = (struct ExecIFace *)sysbase->MainInterface;
    IExec->DebugPrintF("%s is a device — install to DEVS:Networks/ or "
                       "OpenDevice() from a test program.\n", DEVNAME);
    return 20;
}

/* ------------------------------------------------------------------- */
/* Init / Open / Close / Expunge / BeginIO / AbortIO — Phase 0 stubs   */
/* ------------------------------------------------------------------- */

struct Library *_manager_Init(struct Library *library, BPTR seglist,
                              struct Interface *exec)
{
    struct Rtl8139ReBase *devBase = (struct Rtl8139ReBase *)library;
    struct ExecIFace *iexec = (struct ExecIFace *)exec;

    devBase->IExec       = iexec;
    devBase->dev_SegList = (ULONG)seglist;

    iexec->DebugPrintF("[rtl8139re] Init: DevBase=%p sizeof=%lu\n",
                       devBase, (unsigned long)sizeof(*devBase));

    /* Real init (Phase A onwards) will open dos/expansion/utility, get
     * their interfaces, then find the RTL8139 PCI device. For Phase 0
     * we just return so OpenDevice can succeed. */
    return (struct Library *)devBase;
}

struct Rtl8139ReBase *_manager_Open(struct DeviceManagerInterface *Self,
                                    struct IOSana2Req *ioreq,
                                    ULONG unitNum, ULONG flags)
{
    (void)flags; (void)unitNum;
    struct Rtl8139ReBase *devBase = (struct Rtl8139ReBase *)Self->Data.LibBase;
    devBase->dev_Base.dd_Library.lib_OpenCnt++;
    devBase->dev_Base.dd_Library.lib_Flags &= ~LIBF_DELEXP;
    ioreq->ios2_Req.io_Error = 0;
    ioreq->ios2_Req.io_Unit  = (struct Unit *)devBase;
    return devBase;
}

BPTR _manager_Close(struct DeviceManagerInterface *Self,
                    struct IOSana2Req *ioreq)
{
    struct Rtl8139ReBase *devBase = (struct Rtl8139ReBase *)Self->Data.LibBase;
    ioreq->ios2_Req.io_Unit   = (struct Unit *)-1;
    ioreq->ios2_Req.io_Device = (struct Device *)-1;
    devBase->dev_Base.dd_Library.lib_OpenCnt--;
    if (devBase->dev_Base.dd_Library.lib_OpenCnt == 0 &&
        (devBase->dev_Base.dd_Library.lib_Flags & LIBF_DELEXP)) {
        return _manager_Expunge(Self);
    }
    return (BPTR)0;
}

BPTR _manager_Expunge(struct DeviceManagerInterface *Self)
{
    struct Rtl8139ReBase *devBase = (struct Rtl8139ReBase *)Self->Data.LibBase;
    if (devBase->dev_Base.dd_Library.lib_OpenCnt) {
        devBase->dev_Base.dd_Library.lib_Flags |= LIBF_DELEXP;
        return (BPTR)0;
    }
    BPTR seg = (BPTR)devBase->dev_SegList;
    struct ExecIFace *IExec = devBase->IExec;
    IExec->Remove((struct Node *)&devBase->dev_Base.dd_Library.lib_Node);
    IExec->DeleteLibrary((struct Library *)devBase);
    return seg;
}

void _manager_BeginIO(struct DeviceManagerInterface *Self,
                      struct IOSana2Req *ioreq)
{
    (void)Self;
    /* Phase 0 stub — reply IOERR_NOCMD for everything. Sana-II handlers
     * arrive in later phases (C onwards). */
    ioreq->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    ioreq->ios2_Req.io_Flags &= ~IOF_QUICK;
    ioreq->ios2_Req.io_Error = IOERR_NOCMD;
    ioreq->ios2_WireError    = 0;
    /* Reply immediately since no unit task exists yet. */
    struct Rtl8139ReBase *devBase = (struct Rtl8139ReBase *)Self->Data.LibBase;
    devBase->IExec->ReplyMsg((struct Message *)ioreq);
}

LONG _manager_AbortIO(struct DeviceManagerInterface *Self,
                      struct IOSana2Req *ioreq)
{
    (void)Self; (void)ioreq;
    return IOERR_NOCMD;
}
