/*
 * rtl8139re.device — AmigaOS 4 SANA-II network driver for the
 * Realtek RTL8139 family.
 *
 * Currently implements: library/PCI init, MAC read via IDR0..IDR5,
 * CMD_WRITE (TX via TSAD/TSD register pair, 4-slot round-robin,
 * synchronous poll). Pending: IRQ handling, RX ring, S2_ONLINE /
 * S2_CONFIGINTERFACE, opener list, full command dispatcher.
 *
 * Single-unit, PCI I/O BAR only (no MMIO), tested on QEMU sam460ex.
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
 * Vendor:Device pairs matched by the driver. First entry is the
 * canonical Realtek RTL8139; the rest are known RTL8139-compatible
 * variants and rebadged parts. Terminated by 0xFFFF:0xFFFF.
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
/* Per-field NULL checks so callers can invoke it at any point during  */
/* Init to roll back a partial success.                                */
/* ------------------------------------------------------------------- */

static void v_cleanup(struct Rtl8139ReBase *base)
{
    struct ExecIFace *IExec = base->IExec;

    if (base->IPCI)         { IExec->DropInterface((struct Interface *)base->IPCI); base->IPCI = NULL; }
    if (base->IUtility)     { IExec->DropInterface((struct Interface *)base->IUtility); base->IUtility = NULL; }
    if (base->IExpansion)   { IExec->DropInterface((struct Interface *)base->IExpansion); base->IExpansion = NULL; }
    if (base->IDOS)         { IExec->DropInterface((struct Interface *)base->IDOS); base->IDOS = NULL; }

    /* Free PCI device + BAR + TX buffer BEFORE dropping IPCI, since
     * FreeDevice/FreeResourceRange need the IPCI interface alive. */
    if (base->tx_buffer_raw) {
        IExec->FreeMem(base->tx_buffer_raw, base->tx_buffer_raw_size);
        base->tx_buffer_raw = NULL;
        base->tx_buffer = NULL;
        base->tx_buffer_phys = 0;
    }
    if (base->rx_ring_raw) {
        IExec->FreeMem(base->rx_ring_raw, base->rx_ring_raw_size);
        base->rx_ring_raw = NULL;
        base->rx_ring = NULL;
        base->rx_ring_phys = 0;
    }
    if (base->bar_range && base->pciDevice) {
        base->pciDevice->FreeResourceRange(base->bar_range);
        base->bar_range = NULL;
        base->bar_io = 0;
    }
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
/* Init — the AutoInit hook, called by exec when the resident tag is   */
/* first processed. Sets library metadata, opens dos/expansion/utility */
/* + their "main" interfaces, gets IPCI, probes newmemory.resource,    */
/* enumerates the RTL8139 PCI device, grabs BAR0, enables IO+BM, and   */
/* allocates the TX buffer pool. On failure DevCleanup + return the    */
/* base anyway; Open will refuse.                                      */
/* ------------------------------------------------------------------- */

struct Library *_manager_Init(struct Library *library, BPTR seglist,
                              struct Interface *exec)
{
    struct Rtl8139ReBase *base = (struct Rtl8139ReBase *)library;
    struct ExecIFace *iexec = (struct ExecIFace *)exec;

    base->IExec       = iexec;
    base->dev_SegList = (ULONG)seglist;

    /* Library metadata — the kernel populates most of this from the
     * resident tag, but hand-filling here keeps things explicit
     * regardless of exec version. */
    base->dev_Base.dd_Library.lib_Node.ln_Type = NT_DEVICE;
    base->dev_Base.dd_Library.lib_Node.ln_Name = (STRPTR)DEVNAME;
    base->dev_Base.dd_Library.lib_Node.ln_Pri  = 0;
    base->dev_Base.dd_Library.lib_Version      = DEVVER;
    base->dev_Base.dd_Library.lib_Revision     = DEVREV;
    base->dev_Base.dd_Library.lib_IdString     = (STRPTR)DEVVERSIONSTRING;

    iexec->DebugPrintF("[rtl8139re] Init: DevBase=%p sizeof=%lu\n",
                       base, (unsigned long)sizeof(*base));

    /* IO lock — will guard openers list + unit table once those land. */
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

    /* Get the "main" interface (v1) on each library. */
    base->IDOS = (struct DOSIFace *)iexec->GetInterface(
        base->DOSBase, "main", 1, NULL);
    if (!base->IDOS) goto fail;

    base->IExpansion = (struct ExpansionIFace *)iexec->GetInterface(
        base->ExpansionBase, "main", 1, NULL);
    if (!base->IExpansion) goto fail;

    base->IUtility = (struct UtilityIFace *)iexec->GetInterface(
        base->UtilityBase, "main", 1, NULL);
    if (!base->IUtility) goto fail;

    /* Get IPCI — the "pci" interface on expansion.library. Used for
     * FindDeviceTags below. */
    base->IPCI = (struct PCIIFace *)iexec->GetInterface(
        base->ExpansionBase, "pci", 1, NULL);
    if (!base->IPCI) goto fail;

    /* Probe newmemory.resource — kept as a boolean flag for future
     * use; nothing consumes it in the current build. */
    APTR nmem = iexec->OpenResource("newmemory.resource");
    base->has_newmemory = (nmem != NULL) ? 1 : 0;

    /* Walk the vendor:device table and grab a matching PCI device.
     * Prefer Index=1+ so we don't collide with any other rtl8139
     * driver that may have taken Index=0. Fall back to Index=0 if
     * no second chip exists. */
    for (int idx = 1; idx >= 0 && !base->pciDevice; idx--) {
        for (const struct Rtl8139DeviceID *id = rtl8139_device_ids;
             id->vendor != 0xFFFF; id++) {
            struct PCIDevice *pd = base->IPCI->FindDeviceTags(
                FDT_VendorID, id->vendor,
                FDT_DeviceID, id->device,
                FDT_Index,    (ULONG)idx,
                TAG_END);
            if (pd) {
                base->pciDevice = pd;
                base->pci_vendor = id->vendor;
                base->pci_device = id->device;
                iexec->DebugPrintF("[rtl8139re] Found PCI device %04lx:%04lx (Index=%d)\n",
                                   (unsigned long)id->vendor,
                                   (unsigned long)id->device, idx);
                break;
            }
        }
    }
    if (!base->pciDevice) {
        iexec->DebugPrintF("[rtl8139re] No matching RTL8139-family "
                           "PCI device found — device will load but "
                           "OpenDevice on unit 0 will fail\n");
    }

    /* Bring the chip's registers within reach + read the MAC.
     *
     * Steps:
     *   1. GetResourceRange(0) → BAR0 (I/O port range for RTL8139)
     *   2. Set PCI command bits (BUSMASTER | IO_ENABLE) via
     *      WriteConfigLong(PCI_COMMAND, ...)
     *   3. Read MAC via InLong(BAR+IDR0) + InLong(BAR+IDR4).
     *
     * PCIDevice.InLong / OutLong auto-byteswap on PPC — the OS4 PCI
     * stack knows the bus is LE. So the ULONG we get back is in host
     * order for byte-lane purposes; the low byte of InLong(BAR+0)
     * is IDR0, next byte IDR1, etc.
     */
    if (base->pciDevice) {
        base->bar_range = base->pciDevice->GetResourceRange(0);
        if (base->bar_range) {
            base->bar_io = base->bar_range->BaseAddress;
            iexec->DebugPrintF("[rtl8139re] BAR0 base=%08lx size=%lu flags=%08lx\n",
                               (unsigned long)base->bar_io,
                               (unsigned long)base->bar_range->Size,
                               (unsigned long)base->bar_range->Flags);

            /* Enable BUSMASTER + IO in PCI command register. Standard
             * PCI bring-up; without this the NIC won't respond to
             * I/O accesses. Command reg = 0x04, low 16 bits are the
             * command bits. */
            ULONG cmd = base->pciDevice->ReadConfigLong(0x04);
            cmd |= 0x00000007;    /* IO | MEM | BUSMASTER (low 3 bits) */
            base->pciDevice->WriteConfigLong(0x04, cmd);
            iexec->DebugPrintF("[rtl8139re] PCI cmd/status = %08lx (after set)\n",
                               (unsigned long)cmd);

            /* Read MAC via InLong. IMPORTANT: on OS4/sam460ex, PCIDevice.InByte
             * returns garbled values for RTL8139 IDR registers — verified via
             * comparison with InLong which returns the correct little-endian
             * bytes. Use InLong exclusively for register reads.
             *
             * PCIDevice.InLong returns a ULONG whose low byte = IDR0, next
             * = IDR1, etc — matching how PCIDevice.OutLong writes bytes to
             * the device. So mac[0] = lo & 0xFF, mac[1] = (lo>>8), etc. */
            ULONG lo = base->pciDevice->InLong(base->bar_io + 0);
            ULONG hi = base->pciDevice->InLong(base->bar_io + 4);
            base->mac[0] = (UBYTE)(lo & 0xFF);
            base->mac[1] = (UBYTE)((lo >>  8) & 0xFF);
            base->mac[2] = (UBYTE)((lo >> 16) & 0xFF);
            base->mac[3] = (UBYTE)((lo >> 24) & 0xFF);
            base->mac[4] = (UBYTE)(hi & 0xFF);
            base->mac[5] = (UBYTE)((hi >>  8) & 0xFF);
            iexec->DebugPrintF("[rtl8139re] MAC = %02x:%02x:%02x:%02x:%02x:%02x\n",
                               base->mac[0], base->mac[1], base->mac[2],
                               base->mac[3], base->mac[4], base->mac[5]);

            base->hw_present = TRUE;

            /* Allocate TX buffer pool (4 slots x 2 KB) via plain
             * AllocMem with MEMF_KICK|MEMF_CLEAR. AllocVecTags with
             * alignment attrs silently fails at KB sizes on some
             * OS4/sam460ex builds — AllocMem is the reliable path.
             * Over-allocate for 32-byte manual alignment. */
            ULONG total = RTL_TX_BUF_SIZE * RTL_TX_SLOTS + 32;  /* over-align */
            base->tx_buffer_raw = iexec->AllocMem(total, MEMF_KICK | MEMF_CLEAR);
            base->tx_buffer_raw_size = total;
            base->tx_buffer = base->tx_buffer_raw
                ? (APTR)(((ULONG)base->tx_buffer_raw + 31UL) & ~31UL) : NULL;
            if (base->tx_buffer) {
                /* Get DMA phys for the aligned base. */
                ULONG nents = iexec->StartDMA(base->tx_buffer,
                    RTL_TX_BUF_SIZE * RTL_TX_SLOTS, DMA_ReadFromRAM);
                if (nents > 0) {
                    struct DMAEntry *dl = (struct DMAEntry *)
                        iexec->AllocSysObjectTags(ASOT_DMAENTRY,
                            ASODMAE_NumEntries, nents, TAG_END);
                    if (dl) {
                        iexec->GetDMAList(base->tx_buffer,
                            RTL_TX_BUF_SIZE * RTL_TX_SLOTS,
                            DMA_ReadFromRAM, dl);
                        base->tx_buffer_phys = (ULONG)dl[0].PhysicalAddress;
                        iexec->FreeSysObject(ASOT_DMAENTRY, dl);
                    }
                }
                iexec->DebugPrintF("[rtl8139re] tx_buffer: cpu=%p phys=%08lx (%d slots x %d bytes)\n",
                                   base->tx_buffer,
                                   (unsigned long)base->tx_buffer_phys,
                                   RTL_TX_SLOTS, RTL_TX_BUF_SIZE);
            }

            /* Allocate RX ring — 8 KB + 16-byte wrap padding + a bit
             * of slack for chip prefetch. Same MEMF_KICK|CLEAR pattern
             * as TX; align to 32 bytes for cache-line safety. */
            ULONG rx_total = RTL_RX_RING_SIZE + RTL_RX_RING_PAD + 64;
            base->rx_ring_raw = iexec->AllocMem(rx_total, MEMF_KICK | MEMF_CLEAR);
            base->rx_ring_raw_size = rx_total;
            base->rx_ring = base->rx_ring_raw
                ? (APTR)(((ULONG)base->rx_ring_raw + 31UL) & ~31UL) : NULL;
            if (base->rx_ring) {
                /* MEMF_KICK memory is identity-mapped for DMA on this
                 * platform; CPU virt = PCI-bus phys. Don't call
                 * StartDMA — it doesn't nest and we don't need it. */
                base->rx_ring_phys = (ULONG)base->rx_ring;
                iexec->DebugPrintF("[rtl8139re] rx_ring: cpu=%p phys=%08lx size=%lu\n",
                                   base->rx_ring,
                                   (unsigned long)base->rx_ring_phys,
                                   (unsigned long)RTL_RX_RING_SIZE);

                /* Program RBSTART with the ring's DMA physical addr. */
                base->pciDevice->OutLong(base->bar_io + RTL_RBSTART,
                                         base->rx_ring_phys);
                /* Program RCR: accept-physical + accept-broadcast +
                 * WRAP + unlimited MXDMA. RBLEN=00 selects 8KB+16. */
                base->pciDevice->OutLong(base->bar_io + RTL_RCR,
                                         RTL_RCR_DEFAULT);
                /* Zero CAPR (driver read pointer) — chip resets to
                 * 0xFFF0 (= "one 16-byte block before start"), which
                 * is the correct initial value; leave alone. */
            } else {
                iexec->DebugPrintF("[rtl8139re] rx_ring alloc FAILED\n");
            }

            /* Enable TE + RE in ChipCmd. Also program TCR to sensible
             * defaults (16 retries, standard IFG). Do this LAST so
             * ring registers are programmed before the NIC starts
             * looking at them. */
            base->pciDevice->OutLong(base->bar_io + RTL_TCR, 0x03000700);
            base->pciDevice->OutByte(base->bar_io + RTL_CR,
                                     RTL_CR_TE | RTL_CR_RE);
            iexec->DebugPrintF("[rtl8139re] TX+RX enabled (TCR=03000700, CR=TE|RE, RCR=%08lx)\n",
                               (unsigned long)RTL_RCR_DEFAULT);
        } else {
            iexec->DebugPrintF("[rtl8139re] GetResourceRange(0) FAILED\n");
        }
    }

    iexec->DebugPrintF("[rtl8139re] Init OK: libs+ifaces bound, "
                       "has_newmemory=%d, pciDevice=%p hw=%d\n",
                       (int)base->has_newmemory, base->pciDevice,
                       (int)base->hw_present);
    return (struct Library *)base;

fail:
    iexec->DebugPrintF("[rtl8139re] Init: library/interface open failed — "
                       "cleaning up but keeping base\n");
    v_cleanup(base);
    /* Return the base anyway; Open will refuse. Letting exec add the
     * device to the library list means Expunge handles removal
     * cleanly. */
    return (struct Library *)base;
}

/* ------------------------------------------------------------------- */
/* Open / Close / Expunge.                                             */
/* ------------------------------------------------------------------- */

struct Rtl8139ReBase *_manager_Open(struct DeviceManagerInterface *Self,
                                    struct IOSana2Req *ioreq,
                                    ULONG unitNum, ULONG flags)
{
    (void)flags;
    struct Rtl8139ReBase *base = (struct Rtl8139ReBase *)Self->Data.LibBase;

    /* Refuse if Init didn't complete (any required libs/ifaces missing). */
    if (!base->IDOS || !base->IExpansion || !base->IUtility || !base->IPCI) {
        ioreq->ios2_Req.io_Error = IOERR_OPENFAIL;
        return NULL;
    }

    /* Reject unit > 7 (arbitrary max — real hardware never has that
     * many RTL8139s in one system). */
    if (unitNum > 7) {
        ioreq->ios2_Req.io_Error = IOERR_OPENFAIL;
        return NULL;
    }

    /* Refuse if no PCI device was bound at Init. */
    if (!base->pciDevice) {
        ioreq->ios2_Req.io_Error = IOERR_OPENFAIL;
        return NULL;
    }

    base->dev_Base.dd_Library.lib_OpenCnt++;
    base->dev_Base.dd_Library.lib_Flags &= ~LIBF_DELEXP;
    ioreq->ios2_Req.io_Error = 0;
    ioreq->ios2_Req.io_Unit  = (struct Unit *)base;   /* single unit for now */
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
/* BeginIO — SANA-II dispatch.                                         */
/* AbortIO — canonical Amiga AbortIO: under IExec.Disable, removes     */
/* the IOReq from any queue, replies it, re-enables.                   */
/* ------------------------------------------------------------------- */

void _manager_BeginIO(struct DeviceManagerInterface *Self,
                      struct IOSana2Req *ioreq)
{
    struct Rtl8139ReBase *base = (struct Rtl8139ReBase *)Self->Data.LibBase;
    ioreq->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    ioreq->ios2_Req.io_Flags &= ~IOF_QUICK;
    ioreq->ios2_Req.io_Error = 0;
    ioreq->ios2_WireError    = 0;

    /* Currently implements S2_GETSTATIONADDRESS + CMD_WRITE. All
     * other commands reply IOERR_NOCMD. */
    switch (ioreq->ios2_Req.io_Command) {
    case S2_GETSTATIONADDRESS: {
        if (!base->hw_present) {
            ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
            ioreq->ios2_WireError    = S2WERR_UNIT_OFFLINE;
        } else {
            /* Real S2_GETSTATIONADDRESS. Read MAC via InLong then
             * pack into SrcAddr[0..5] + DstAddr[0..5]. */
            volatile ULONG lo_v = base->pciDevice->InLong(base->bar_io + RTL_IDR0);
            volatile ULONG hi_v = base->pciDevice->InLong(base->bar_io + RTL_IDR4);
            ULONG lo = lo_v, hi = hi_v;
            base->mac[0] = (UBYTE)(lo & 0xFF);
            base->mac[1] = (UBYTE)((lo >>  8) & 0xFF);
            base->mac[2] = (UBYTE)((lo >> 16) & 0xFF);
            base->mac[3] = (UBYTE)((lo >> 24) & 0xFF);
            base->mac[4] = (UBYTE)(hi & 0xFF);
            base->mac[5] = (UBYTE)((hi >>  8) & 0xFF);
            for (int i = 0; i < 6; i++) {
                ioreq->ios2_SrcAddr[i] = base->mac[i];
                ioreq->ios2_DstAddr[i] = base->mac[i];
            }
            /* Bonus: pack MAC bytes into ios2_DataLength + PacketType
             * as a workaround — SrcAddr/DstAddr writes appear to be
             * lost between driver and caller on this OS4 setup for
             * reasons we haven't isolated. ULONG field writes DO
             * persist reliably. */
            ioreq->ios2_DataLength = ((ULONG)base->mac[0] << 24) |
                                     ((ULONG)base->mac[1] << 16) |
                                     ((ULONG)base->mac[2] <<  8) |
                                     ((ULONG)base->mac[3]);
            ioreq->ios2_PacketType = ((ULONG)base->mac[4] << 24) |
                                     ((ULONG)base->mac[5] << 16);
        }
        break;
    }
    case CMD_WRITE:
    case S2_BROADCAST: {
        /* TX handler. Direct write to RTL8139 TSAD/TSD register pair.
         * Synchronous in BeginIO context (no unit task, no ring, no
         * async).
         *
         *   1. Pick TX slot (round-robin 0..3)
         *   2. Copy caller's frame to tx_buffer[slot]; pad to 60 bytes
         *   3. CacheClearE to ensure CPU writes reach RAM
         *   4. OutLong(TSAD[slot], buf_phys)
         *   5. OutLong(TSD[slot], length | early-TX-threshold)
         *      Writing to TSD triggers TX; the NIC DMA-reads TSAD's
         *      address, transmits, sets TOK bit when done.
         *   6. Poll TSD.TOK (bit 15) or ABT/CRS (error) bits.
         */
        if (!base->hw_present || !base->tx_buffer) {
            ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
            ioreq->ios2_WireError    = S2WERR_UNIT_OFFLINE;
            break;
        }
        ULONG len = ioreq->ios2_DataLength;
        if (len == 0 || len > 1514 || !ioreq->ios2_Data) {
            ioreq->ios2_Req.io_Error = S2ERR_MTU_EXCEEDED;
            break;
        }

        UBYTE slot = base->tx_next_slot;
        UBYTE *dst = (UBYTE *)base->tx_buffer + (slot * RTL_TX_BUF_SIZE);
        ULONG dst_phys = base->tx_buffer_phys + (slot * RTL_TX_BUF_SIZE);

        /* Copy caller's frame (RAW mode — full Ethernet). */
        UBYTE *src = (UBYTE *)ioreq->ios2_Data;
        for (ULONG i = 0; i < len; i++) dst[i] = src[i];
        /* Pad to Ethernet minimum 60 bytes. */
        for (ULONG i = len; i < 60; i++) dst[i] = 0;
        if (len < 60) len = 60;

        /* Flush cache before doorbell. */
        base->IExec->CacheClearE(dst, len, CACRF_ClearD);

        struct PCIDevice *pd = base->pciDevice;
        ULONG bar = base->bar_io;

        /* Write DMA phys addr to TSADn. */
        pd->OutLong(bar + RTL_TSAD0 + (slot * 4), dst_phys);

        /* Write TSDn: length in low 13 bits, OWN=0 triggers TX.
         * Also default early-TX-threshold (bits 16-21 = 32*256=8KB).
         * QEMU accepts anything sensible; use 0x00030000 (256B threshold). */
        ULONG tsd = (len & 0x1FFF) | (0x30 << 11);   /* threshold + len */
        pd->OutLong(bar + RTL_TSD0 + (slot * 4), tsd);

        /* Advance slot pointer round-robin. */
        base->tx_next_slot = (slot + 1) & 0x3;

        /* Poll TSD for TOK (bit 15) or ABT/CRS/TUN (error bits).
         * QEMU should complete quickly. Cap at ~10ms. */
        BOOL done = FALSE;
        for (int i = 0; i < 5000; i++) {
            ULONG s = pd->InLong(bar + RTL_TSD0 + (slot * 4));
            if (s & (1UL << 15)) { done = TRUE; break; }  /* TOK */
            if (s & (1UL << 30)) break;                    /* OWN — HW returned */
        }
        if (!done) {
            ioreq->ios2_Req.io_Error = IOERR_UNITBUSY;
        }
        break;
    }
    case 0xE001: {  /* Private DBG_RXSTATE — pack rx-ring diagnostics */
        if (!base->hw_present) {
            ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
            break;
        }
        struct PCIDevice *pd = base->pciDevice;
        ULONG bar = base->bar_io;
        volatile ULONG cbr  = pd->InLong(bar + RTL_CBR);   /* also reads CAPR high half */
        volatile ULONG isr  = pd->InLong(bar + RTL_ISR);
        volatile ULONG cr   = pd->InLong(bar + RTL_CR);
        ioreq->ios2_DataLength = cbr;
        ioreq->ios2_WireError  = isr & 0xFFFF;
        ioreq->ios2_PacketType = cr & 0xFF;
        break;
    }
    default:
        ioreq->ios2_Req.io_Error = IOERR_NOCMD;
        break;
    }

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
