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

#include <devices/newstyle.h>
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

/* ISR — RTL8139 interrupt handler. Runs in interrupt context.
 * Read ISR (BAR+0x3E), ACK by writing the same bits back, bump
 * counters. Keep it minimal; deferred work happens in the unit
 * task (Phase G). Returns 1 if the interrupt was "ours" (any bit
 * set in ISR after we ACK) so the OS4 int dispatcher can move on. */
static uint32 rtl_isr(struct ExecBase *sysbase, struct Rtl8139ReBase *base)
{
    (void)sysbase;
    if (!base || !base->pciDevice || !base->bar_io) return 0;
    ULONG bar = base->bar_io;
    struct PCIDevice *pd = base->pciDevice;
    /* ISR/IMR are 16-bit registers at BAR+0x3E / BAR+0x3C. Use
     * InWord/OutWord — OutLong here would zero TCR (0x40-0x41).
     *
     * PCI IRQs on sam460ex are level-triggered and shared. To avoid
     * re-entering while our ACK's write is still in flight to the
     * chip, mask IMR first, then ACK ISR, then unmask. */
    pd->OutWord(bar + RTL_IMR, 0);
    UWORD isr = pd->InWord(bar + RTL_ISR);
    if (!isr) {
        pd->OutWord(bar + RTL_IMR, (UWORD)RTL_IMR_DEFAULT);
        return 0;   /* not our interrupt */
    }
    base->irq_count++;
    base->irq_last_isr = isr;
    pd->OutWord(bar + RTL_ISR, isr);   /* W1C — clears the bits we saw */
    pd->OutWord(bar + RTL_IMR, (UWORD)RTL_IMR_DEFAULT);
    /* Wake unit task so it can drain the RX ring at task priority.
     * Signal is only meaningful once unit_task + rx_sigmask are set. */
    if (base->unit_task && base->rx_sigmask) {
        base->IExec->Signal(base->unit_task, base->rx_sigmask);
    }
    return 1;   /* claimed */
}

/* Forward declaration — rtl_rx_drain is defined below. */
static void rtl_rx_drain(struct Rtl8139ReBase *base);

/* Unit task entry — runs at task priority (not interrupt).
 * Waits on rx_sigmask (set by ISR on any interrupt) OR CTRL_C
 * (set by driver teardown to ask task to exit), drains RX, loops.
 *
 * Base pointer passed via AT_Param1 → arrives as first arg on PPC. */
static void rtl_unit_task_entry(struct Rtl8139ReBase *base)
{
    if (!base) return;
    struct ExecIFace *iexec = base->IExec;

    ULONG stop_mask = SIGBREAKF_CTRL_C;
    ULONG wait_mask = base->rx_sigmask | stop_mask;
    while (!base->unit_task_stop) {
        ULONG sigs = iexec->Wait(wait_mask);
        if (sigs & stop_mask) break;
        if (sigs & base->rx_sigmask) rtl_rx_drain(base);
    }
    iexec->Forbid();
    base->unit_task = NULL;
    iexec->Permit();
}

/* CRC-32 (Ethernet polynomial 0xEDB88320) for the 6-byte multicast MAC.
 * RTL8139 hashes into MAR0..7 (64 bits) using the top 6 bits of the
 * CRC. Reference: Linux 8139too driver ether_crc(). */
static ULONG rtl_ether_crc(const UBYTE *mac)
{
    ULONG crc = 0xFFFFFFFFUL;
    for (int i = 0; i < 6; i++) {
        crc ^= mac[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320UL : 0);
    }
    return crc;
}

/* Push the software MAR mirror to the chip. Called after add/del. */
static void rtl_mar_flush(struct Rtl8139ReBase *base)
{
    if (!base->pciDevice || !base->bar_io) return;
    /* MAR0-7 are 8 individual bytes at BAR+0x08..0x0F. Program via
     * two OutLong writes (0x08 and 0x0C) — the chip accepts either
     * byte or long access on these. Long is faster + atomic. */
    ULONG lo = ((ULONG)base->mar[0]) | ((ULONG)base->mar[1] << 8) |
               ((ULONG)base->mar[2] << 16) | ((ULONG)base->mar[3] << 24);
    ULONG hi = ((ULONG)base->mar[4]) | ((ULONG)base->mar[5] << 8) |
               ((ULONG)base->mar[6] << 16) | ((ULONG)base->mar[7] << 24);
    base->pciDevice->OutLong(base->bar_io + RTL_MAR0, lo);
    base->pciDevice->OutLong(base->bar_io + RTL_MAR0 + 4, hi);
}

/* Walk the RX ring and dispatch received packets to matching openers.
 * Called from BeginIO after any client-initiated command (client-
 * driven drain — no unit task yet). Non-reentrant; assumes caller is
 * in task context, NOT interrupt context.
 *
 * Ring layout (RTL8139 datasheet ch 6):
 *   - 8 KB circular buffer + 16-byte pad (RCR_WRAP=1 -> chip pads
 *     overflow into +16 area so a packet at ring end is contiguous).
 *   - Each packet is prefixed by a 4-byte header:
 *       byte 0-1: status word (bit 0 = ROK)
 *       byte 2-3: length including 4-byte FCS
 *   - CBR (0x3A) = chip's write pointer (offset into ring)
 *   - CAPR (0x38) = driver's read pointer, biased by -16. So driver
 *     reads from (CAPR + 16) & (RING_SIZE-1).
 */
static void rtl_rx_drain(struct Rtl8139ReBase *base)
{
    if (!base->is_online || !base->rx_ring || !base->pciDevice) return;
    struct PCIDevice *pd = base->pciDevice;
    ULONG bar = base->bar_io;
    UBYTE *ring = (UBYTE *)base->rx_ring;

    UWORD cbr = pd->InWord(bar + RTL_CBR);
    UWORD capr = pd->InWord(bar + RTL_CAPR);
    UWORD read_off = (UWORD)((capr + 16) & (RTL_RX_RING_SIZE - 1));

    /* Cap iterations to avoid infinite loop on malformed ring. */
    for (int i = 0; i < 32 && read_off != cbr; i++) {
        UBYTE *pkt = ring + read_off;
        UWORD status = (UWORD)pkt[0] | ((UWORD)pkt[1] << 8);
        UWORD length = (UWORD)pkt[2] | ((UWORD)pkt[3] << 8);

        if (!(status & 0x0001) || length < 18 || length > 1600) {
            /* Bad packet — drop everything, hope next drain resyncs. */
            base->stats.BadData++;
            break;
        }
        UBYTE *body = pkt + 4;
        UWORD body_len = (UWORD)(length - 4);  /* strip FCS */
        UWORD ethertype = ((UWORD)body[12] << 8) | body[13];

        /* Match against first opener whose packet_type matches, else
         * fall back to any pending S2_READORPHAN. */
        base->IExec->ObtainSemaphore(base->io_lock);
        struct IOSana2Req *target = NULL;
        struct Rtl8139Opener *chosen = NULL;
        struct Rtl8139Opener *op = (struct Rtl8139Opener *)base->openers.mlh_Head;
        while (op && op->node.mln_Succ) {
            struct Rtl8139Opener *next =
                (struct Rtl8139Opener *)op->node.mln_Succ;
            if (op->packet_type == ethertype) {
                struct Node *rn = base->IExec->RemHead(&op->pending_reads);
                if (rn) { target = (struct IOSana2Req *)rn; chosen = op; break; }
            }
            op = next;
        }
        if (!target) {
            struct Node *rn = base->IExec->RemHead(&base->orphan_reads);
            if (rn) target = (struct IOSana2Req *)rn;
        }
        base->IExec->ReleaseSemaphore(base->io_lock);

        if (target) {
            /* Matched-opener delivery = COOKED (strip 14-byte ETH hdr).
             * Orphan-queue delivery = RAW (full frame) since orphan
             * consumers may want raw. */
            UBYTE *deliver_from;
            UWORD  deliver_len;
            if (chosen) {
                deliver_from = body + 14;
                deliver_len  = body_len > 14 ? (UWORD)(body_len - 14) : 0;
            } else {
                deliver_from = body;
                deliver_len  = body_len;
            }
            UWORD copy = deliver_len;
            if (target->ios2_DataLength > 0 && copy > target->ios2_DataLength)
                copy = (UWORD)target->ios2_DataLength;
            /* Dispatch by copy_to_tag — classic direct-call vs Hook*
             * via CallHookPkt. See CMD_WRITE for details. */
            BOOL copy_ok = TRUE;
            if (chosen && chosen->copy_to_buff) {
                if (chosen->copy_to_tag == S2_CopyToBuff) {
                    Rtl8139CopyFn fn = (Rtl8139CopyFn)chosen->copy_to_buff;
                    copy_ok = fn(target->ios2_Data, deliver_from, copy);
                } else {
                    struct SANA2CopyHookMsg msg;
                    msg.schm_Method  = chosen->copy_to_tag;
                    msg.schm_MsgSize = sizeof(msg);
                    msg.schm_To      = target->ios2_Data;
                    msg.schm_From    = deliver_from;
                    msg.schm_Size    = copy;
                    copy_ok = (BOOL)(ULONG)base->IUtility->CallHookPkt(
                        (struct Hook *)chosen->copy_to_buff, target, &msg);
                }
            } else {
                UBYTE *dst = (UBYTE *)target->ios2_Data;
                for (UWORD b = 0; b < copy; b++) dst[b] = deliver_from[b];
            }
            if (!copy_ok) {
                target->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
                target->ios2_WireError    = S2WERR_BUFF_ERROR;
                if (chosen) chosen->stat_dropped++;
            }
            target->ios2_DataLength = copy;
            for (int a = 0; a < 6; a++) {
                target->ios2_DstAddr[a] = body[a];       /* from ETH header */
                target->ios2_SrcAddr[a] = body[6 + a];
            }
            target->ios2_PacketType   = ethertype;
            if (!target->ios2_Req.io_Error) target->ios2_Req.io_Error = 0;
            base->IExec->ReplyMsg((struct Message *)target);
            base->stats.PacketsReceived++;
            if (chosen) {
                chosen->stat_rx_pkts++;
                chosen->stat_rx_bytes += deliver_len;
            }
        } else {
            base->stats.UnknownTypesReceived++;
        }

        /* Advance: pkt + length + 4 header, 4-byte aligned. */
        read_off = (UWORD)((read_off + length + 4 + 3) & ~3);
        read_off = (UWORD)(read_off & (RTL_RX_RING_SIZE - 1));
    }

    /* Update CAPR = new read position - 16 (biasing). */
    UWORD new_capr = (UWORD)((read_off - 16) & (RTL_RX_RING_SIZE - 1));
    pd->OutWord(bar + RTL_CAPR, new_capr);
}

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
    /* Stop unit task BEFORE removing IRQ or freeing anything the
     * task might touch. Signal CTRL_C so its Wait returns; poll for
     * the task nulling itself. Cap at ~1s via short IDOS->Delay. */
    if (base->unit_task) {
        base->unit_task_stop = 1;
        IExec->Signal(base->unit_task, SIGBREAKF_CTRL_C);
        for (int i = 0; i < 100 && base->unit_task; i++) {
            if (base->IDOS) base->IDOS->Delay(1);   /* 1/50 s = 20ms */
            else break;
        }
        base->unit_task = NULL;
    }

    /* IRQ off FIRST — before we drop the PCI resources or free the
     * buffers the ISR references. Also mask IMR so no spurious late
     * interrupts fire during teardown. */
    if (base->pciDevice && base->bar_io) {
        base->pciDevice->OutWord(base->bar_io + RTL_IMR, 0);
    }
    if (base->irq_installed && base->irq_vector != 0) {
        IExec->RemIntServer(base->irq_vector, &base->irq_node);
        base->irq_installed = FALSE;
        base->irq_vector = 0;
    }
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
    /* Openers list — empty until first OpenDevice runs. */
    iexec->NewMinList(&base->openers);
    iexec->NewList(&base->orphan_reads);

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

#ifdef RTL_ENABLE_RX
            /* Allocate RX ring — 8 KB + 16-byte wrap padding + slack.
             * MEMF_KICK|MEMF_CLEAR + 32-byte manual alignment. */
            ULONG rx_total = RTL_RX_RING_SIZE + RTL_RX_RING_PAD + 64;
            base->rx_ring_raw = iexec->AllocMem(rx_total, MEMF_KICK | MEMF_CLEAR);
            base->rx_ring_raw_size = rx_total;
            base->rx_ring = base->rx_ring_raw
                ? (APTR)(((ULONG)base->rx_ring_raw + 31UL) & ~31UL) : NULL;
            if (base->rx_ring) {
                base->rx_ring_phys = (ULONG)base->rx_ring;
                iexec->DebugPrintF("[rtl8139re] rx_ring: cpu=%p phys=%08lx size=%lu\n",
                                   base->rx_ring,
                                   (unsigned long)base->rx_ring_phys,
                                   (unsigned long)RTL_RX_RING_SIZE);
                base->pciDevice->OutLong(base->bar_io + RTL_RBSTART,
                                         base->rx_ring_phys);
                base->pciDevice->OutLong(base->bar_io + RTL_RCR,
                                         RTL_RCR_DEFAULT);
            } else {
                iexec->DebugPrintF("[rtl8139re] rx_ring alloc FAILED\n");
            }
#endif

            /* Enable TE + optionally RE in ChipCmd. Also program TCR to
             * sensible defaults (16 retries, standard IFG). Do this LAST
             * so ring registers are programmed before the NIC starts
             * looking at them.
             *
             * RTL_ENABLE_RX gate: currently disabled while we chase down
             * why enabling RE hangs the guest under testtx. */
            base->pciDevice->OutLong(base->bar_io + RTL_TCR, 0x03000700);
#ifdef RTL_ENABLE_RX
            /* Diagnostic: read CR at each write step. On QEMU sam460ex
             * the RE bit has been observed not to stick after a
             * single CR=TE|RE write. Try TE first, then TE|RE. */
            {
                ULONG a = base->pciDevice->InLong(base->bar_io + RTL_CR);
                base->pciDevice->OutByte(base->bar_io + RTL_CR, RTL_CR_TE);
                ULONG b = base->pciDevice->InLong(base->bar_io + RTL_CR);
                base->pciDevice->OutByte(base->bar_io + RTL_CR,
                                         RTL_CR_TE | RTL_CR_RE);
                ULONG c = base->pciDevice->InLong(base->bar_io + RTL_CR);
                /* Second write attempt in case RE needs a re-latch. */
                base->pciDevice->OutByte(base->bar_io + RTL_CR,
                                         RTL_CR_TE | RTL_CR_RE);
                ULONG d = base->pciDevice->InLong(base->bar_io + RTL_CR);
                base->cr_after_init       = (UBYTE)(a & 0xFF);
                base->cr_after_te         = (UBYTE)(b & 0xFF);
                base->cr_after_te_re      = (UBYTE)(c & 0xFF);
                base->cr_after_re_rewrite = (UBYTE)(d & 0xFF);
                iexec->DebugPrintF("[rtl8139re] CR steps: init=%02x afterTE=%02x afterTE|RE=%02x re-write=%02x\n",
                                   (unsigned)(a & 0xFF),
                                   (unsigned)(b & 0xFF),
                                   (unsigned)(c & 0xFF),
                                   (unsigned)(d & 0xFF));
            }
#else
            base->pciDevice->OutByte(base->bar_io + RTL_CR, RTL_CR_TE);
#endif

            /* Ensure IRQs are masked at chip regardless of whether we
             * install a handler. This guards against a partial-init
             * scenario leaving IMR non-zero from a prior driver load. */
            base->pciDevice->OutWord(base->bar_io + RTL_IMR, 0);

#ifdef RTL_ENABLE_IRQ
            /* IRQ install. MapInterrupt tells us the OS4-side vector;
             * AddIntServer hooks our handler. Then program IMR to
             * un-mask the causes we care about. */
            base->irq_vector = base->pciDevice->MapInterrupt();
            iexec->DebugPrintF("[rtl8139re] MapInterrupt: vector=%lu\n",
                               (unsigned long)base->irq_vector);
            if (base->irq_vector != 0) {
                base->irq_node.is_Node.ln_Type = NT_INTERRUPT;
                base->irq_node.is_Node.ln_Pri  = 0;
                base->irq_node.is_Node.ln_Name = (STRPTR)DEVNAME;
                base->irq_node.is_Data         = (APTR)base;
                base->irq_node.is_Code         = (void (*)())rtl_isr;
                BOOL ok = iexec->AddIntServer(base->irq_vector,
                                              &base->irq_node);
                base->irq_installed = ok;
                iexec->DebugPrintF("[rtl8139re] AddIntServer(%lu) = %s\n",
                                   (unsigned long)base->irq_vector,
                                   ok ? "OK" : "FAIL");
                base->pciDevice->OutWord(base->bar_io + RTL_IMR,
                                         (UWORD)RTL_IMR_DEFAULT);
                iexec->DebugPrintF("[rtl8139re] IMR = %04x\n",
                                   (unsigned)RTL_IMR_DEFAULT);
            }
#else
            iexec->DebugPrintF("[rtl8139re] IRQ install DISABLED (build-time)\n");
#endif
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
    base->open_count++;
    base->open_last_err = 0;

    /* Refuse if Init didn't complete (any required libs/ifaces missing). */
    if (!base->IDOS || !base->IExpansion || !base->IUtility || !base->IPCI) {
        ioreq->ios2_Req.io_Error = IOERR_OPENFAIL;
        base->open_last_err = IOERR_OPENFAIL;
        return NULL;
    }

    /* Reject unit > 7 (arbitrary max — real hardware never has that
     * many RTL8139s in one system). */
    if (unitNum > 7) {
        ioreq->ios2_Req.io_Error = IOERR_OPENFAIL;
        base->open_last_err = IOERR_OPENFAIL;
        return NULL;
    }

    /* Refuse if no PCI device was bound at Init. */
    if (!base->pciDevice) {
        ioreq->ios2_Req.io_Error = IOERR_OPENFAIL;
        base->open_last_err = IOERR_OPENFAIL;
        return NULL;
    }

    /* Allocate per-opener state. Each OpenDevice gets its own so
     * different clients can request different PacketTypes on RX
     * without stepping on each other. */
    struct Rtl8139Opener *op = base->IExec->AllocVecTags(
        sizeof(*op),
        AVT_ClearWithValue, 0,
        TAG_END);
    if (!op) {
        ioreq->ios2_Req.io_Error = IOERR_OPENFAIL;
        base->open_last_err = IOERR_OPENFAIL;
        return NULL;
    }
    op->base        = base;
    op->reply_port  = ioreq->ios2_Req.io_Message.mn_ReplyPort;
    op->packet_type = 0;
    op->copy_to_buff   = NULL;
    op->copy_from_buff = NULL;
    base->IExec->NewList(&op->pending_reads);

    /* Parse SANA-II buffer-management tags. Roadshow supplies
     * ios2_BufferManagement as a pointer to a TagItem array holding
     * one or more of:
     *   S2_CopyToBuff32   Hook* preferred (byte or 32-bit aligned)
     *   S2_CopyToBuff16   Hook* fallback (16-bit aligned)
     *   S2_CopyToBuff     classic fn ptr fallback
     * (and same for CopyFromBuff). We prefer the Hook-style variants
     * because classic-vs-Hook ABIs are incompatible — calling a
     * Hook* as a fn pointer jumps 8 bytes into the struct → DSI. */
    if (ioreq->ios2_BufferManagement && base->IUtility) {
        struct TagItem *tags =
            (struct TagItem *)ioreq->ios2_BufferManagement;
        struct UtilityIFace *iu = base->IUtility;

        op->copy_to_buff = (APTR)iu->GetTagData(S2_CopyToBuff32, 0, tags);
        op->copy_to_tag  = op->copy_to_buff ? S2_CopyToBuff32 : 0;
        if (!op->copy_to_buff) {
            op->copy_to_buff = (APTR)iu->GetTagData(S2_CopyToBuff16, 0, tags);
            op->copy_to_tag  = op->copy_to_buff ? S2_CopyToBuff16 : 0;
        }
        if (!op->copy_to_buff) {
            op->copy_to_buff = (APTR)iu->GetTagData(S2_CopyToBuff, 0, tags);
            op->copy_to_tag  = op->copy_to_buff ? S2_CopyToBuff : 0;
        }

        op->copy_from_buff = (APTR)iu->GetTagData(S2_CopyFromBuff32, 0, tags);
        op->copy_from_tag  = op->copy_from_buff ? S2_CopyFromBuff32 : 0;
        if (!op->copy_from_buff) {
            op->copy_from_buff = (APTR)iu->GetTagData(S2_CopyFromBuff16, 0, tags);
            op->copy_from_tag  = op->copy_from_buff ? S2_CopyFromBuff16 : 0;
        }
        if (!op->copy_from_buff) {
            op->copy_from_buff = (APTR)iu->GetTagData(S2_CopyFromBuff, 0, tags);
            op->copy_from_tag  = op->copy_from_buff ? S2_CopyFromBuff : 0;
        }
    }

    base->IExec->ObtainSemaphore(base->io_lock);
    base->IExec->AddTail((struct List *)&base->openers, (struct Node *)&op->node);
    base->IExec->ReleaseSemaphore(base->io_lock);

    base->dev_Base.dd_Library.lib_OpenCnt++;
    base->dev_Base.dd_Library.lib_Flags &= ~LIBF_DELEXP;
    ioreq->ios2_Req.io_Error = 0;
    /* SANA-II convention: io_Unit points at per-opener state so
     * BeginIO can retrieve the opener without walking the list. */
    ioreq->ios2_Req.io_Unit  = (struct Unit *)op;
    return base;
}

BPTR _manager_Close(struct DeviceManagerInterface *Self,
                    struct IOSana2Req *ioreq)
{
    struct Rtl8139ReBase *base = (struct Rtl8139ReBase *)Self->Data.LibBase;
    struct Rtl8139Opener *op = (struct Rtl8139Opener *)ioreq->ios2_Req.io_Unit;
    base->close_count++;

    /* Detach the opener + abort any pending CMD_READs so callers
     * unblock. Then free. Guard the list mutation with io_lock. */
    if (op) {
        base->IExec->ObtainSemaphore(base->io_lock);
        base->IExec->Remove((struct Node *)&op->node);
        struct Node *rn;
        while ((rn = base->IExec->RemHead(&op->pending_reads)) != NULL) {
            struct IOSana2Req *r = (struct IOSana2Req *)rn;
            r->ios2_Req.io_Error = IOERR_ABORTED;
            base->IExec->ReplyMsg((struct Message *)r);
        }
        base->IExec->ReleaseSemaphore(base->io_lock);
        base->IExec->FreeVec(op);
    }

    ioreq->ios2_Req.io_Unit   = (struct Unit *)-1;
    ioreq->ios2_Req.io_Device = (struct Device *)-1;
    base->dev_Base.dd_Library.lib_OpenCnt--;
    /* Reset SANA-II per-Open state when the last opener closes so
     * the next OpenDevice + S2_CONFIGINTERFACE cycle starts clean.
     * Without this, Roadshow's NetInterface remove+add sequence sees
     * S2ERR_BAD_STATE on re-config and gives up silently. */
    if (base->dev_Base.dd_Library.lib_OpenCnt == 0) {
        base->is_configured = FALSE;
        base->is_online     = FALSE;
    }
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

    /* Trace every BeginIO invocation — bumps count + records the
     * cmd code in a ring. Diagnoses whether Roadshow calls us. */
    base->beginio_count++;
    base->beginio_last_cmds[base->beginio_ring_head] = ioreq->ios2_Req.io_Command;
    base->beginio_ring_head = (UBYTE)((base->beginio_ring_head + 1) & 0xF);

    /* Client-driven RX drain — process any pending frames before we
     * handle this command. Later this should move to a unit task
     * signalled from the ISR. */
    rtl_rx_drain(base);

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
        /* Debug capture: every CMD_WRITE / S2_BROADCAST call, record
         * the key inputs into base so DBG_TXTRACE (0xE003) can dump
         * them without needing sashimi. */
        base->tx_call_count++;
        base->tx_last_len   = ioreq->ios2_DataLength;
        base->tx_last_ptype = ioreq->ios2_PacketType;
        base->tx_last_cmd   = ioreq->ios2_Req.io_Command;
        base->tx_last_err   = 0;
        base->tx_last_wire  = 0;
        for (int i = 0; i < 6; i++) base->tx_last_dst[i] = ioreq->ios2_DstAddr[i];
        if (ioreq->ios2_Data) {
            UBYTE *sp = (UBYTE *)ioreq->ios2_Data;
            ULONG cap = ioreq->ios2_DataLength;
            if (cap > 16) cap = 16;
            for (ULONG i = 0; i < cap; i++) base->tx_data0_16[i] = sp[i];
            for (ULONG i = cap; i < 16; i++) base->tx_data0_16[i] = 0;
        }

        ULONG len = ioreq->ios2_DataLength;
        if (len == 0 || len > 1514 || !ioreq->ios2_Data) {
            ioreq->ios2_Req.io_Error = S2ERR_MTU_EXCEEDED;
            base->tx_last_err = S2ERR_MTU_EXCEEDED;
            break;
        }

        UBYTE slot = base->tx_next_slot;
        UBYTE *dst = (UBYTE *)base->tx_buffer + (slot * RTL_TX_BUF_SIZE);
        ULONG dst_phys = base->tx_buffer_phys + (slot * RTL_TX_BUF_SIZE);

        /* SANA-II cooked TX: client supplies payload + ios2_DstAddr +
         * ios2_PacketType. Driver builds Ethernet header.
         * S2_BROADCAST forces dst = FF:FF:FF:FF:FF:FF. Prior attempt
         * at raw/cooked heuristic (nonzero PacketType => cooked) was
         * fragile; standard SANA-II clients (Roadshow, bsdsocket) are
         * always cooked, so make that the default. */
        struct Rtl8139Opener *tx_op = (struct Rtl8139Opener *)
            ioreq->ios2_Req.io_Unit;
        if (ioreq->ios2_Req.io_Command == S2_BROADCAST) {
            for (int i = 0; i < 6; i++) dst[i] = 0xFF;
        } else {
            /* If ios2_DstAddr is all-zeros, treat as broadcast — this
             * matches shipping rtl8139.device behavior on Roadshow's
             * pre-ARP-resolution CMD_WRITE calls. Without this the
             * SLIRP gateway drops our packets and never replies with
             * ARP, so Roadshow never gets to resolve the real MAC. */
            BOOL all_zero = TRUE;
            for (int i = 0; i < 6; i++) {
                if (ioreq->ios2_DstAddr[i]) { all_zero = FALSE; break; }
            }
            if (all_zero) {
                for (int i = 0; i < 6; i++) dst[i] = 0xFF;
            } else {
                for (int i = 0; i < 6; i++) dst[i] = ioreq->ios2_DstAddr[i];
            }
        }
        for (int i = 0; i < 6; i++) dst[6 + i] = base->mac[i];
        dst[12] = (UBYTE)((ioreq->ios2_PacketType >> 8) & 0xFF);
        dst[13] = (UBYTE)( ioreq->ios2_PacketType       & 0xFF);
        UBYTE *payload_dst = dst + 14;
        ULONG frame_len    = 14 + len;

        /* Dispatch by tag flavor: classic S2_CopyFromBuff = direct fn
         * call; 16/32 variants = CallHookPkt with SANA2CopyHookMsg.
         * NULL = fall back to memcpy (RAW clients like testtx). */
        BOOL copy_ok = TRUE;
        if (tx_op && tx_op->copy_from_buff) {
            if (tx_op->copy_from_tag == S2_CopyFromBuff) {
                Rtl8139CopyFn fn = (Rtl8139CopyFn)tx_op->copy_from_buff;
                copy_ok = fn(payload_dst, ioreq->ios2_Data, len);
            } else {
                struct SANA2CopyHookMsg msg;
                msg.schm_Method  = tx_op->copy_from_tag;
                msg.schm_MsgSize = sizeof(msg);
                msg.schm_To      = payload_dst;
                msg.schm_From    = ioreq->ios2_Data;
                msg.schm_Size    = len;
                copy_ok = (BOOL)(ULONG)base->IUtility->CallHookPkt(
                    (struct Hook *)tx_op->copy_from_buff, ioreq, &msg);
            }
        } else {
            UBYTE *src = (UBYTE *)ioreq->ios2_Data;
            for (ULONG i = 0; i < len; i++) payload_dst[i] = src[i];
        }
        if (!copy_ok) {
            ioreq->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
            ioreq->ios2_WireError    = S2WERR_BUFF_ERROR;
            break;
        }
        for (ULONG i = frame_len; i < 60; i++) dst[i] = 0;
        if (frame_len < 60) frame_len = 60;
        len = frame_len;   /* wire length that goes to the chip */
        if (tx_op) {
            tx_op->stat_tx_pkts++;
            tx_op->stat_tx_bytes += len;
        }
        base->stats.PacketsSent++;

        /* Flush cache before doorbell. */
        base->IExec->CacheClearE(dst, len, CACRF_ClearD);

        struct PCIDevice *pd = base->pciDevice;
        ULONG bar = base->bar_io;

        /* Write DMA phys addr to TSADn. */
        pd->OutLong(bar + RTL_TSAD0 + (slot * 4), dst_phys);

        /* Write TSDn: length in low 13 bits, OWN=0 triggers TX,
         * ERTXTH=0x30 (48*32=1536 bytes early threshold) in bits 16-21. */
        ULONG tsd = (len & 0x1FFF) | (0x30 << 16);
        pd->OutLong(bar + RTL_TSD0 + (slot * 4), tsd);

        base->tx_next_slot = (slot + 1) & 0x3;

        /* Poll TSD for TOK (bit 15) or OWN (bit 29). Cap ~5000 iters. */
        BOOL done = FALSE;
        for (int i = 0; i < 5000; i++) {
            ULONG s = pd->InLong(bar + RTL_TSD0 + (slot * 4));
            if (s & (1UL << 15)) { done = TRUE; break; }
            if (s & (1UL << 29)) break;
        }
        if (!done) {
            ioreq->ios2_Req.io_Error = IOERR_UNITBUSY;
            base->tx_last_err = IOERR_UNITBUSY;
        }
        base->tx_last_wire = len;
        break;
    }
    case 0xE005: {  /* Private DBG_OPENTRACE — Open/Close counters */
        ioreq->ios2_DataLength = base->open_count;
        ioreq->ios2_WireError  = base->close_count;
        ioreq->ios2_PacketType = base->open_last_err;
        break;
    }
    case 0xE004: {  /* Private DBG_BIOTRACE — dump BeginIO call trace */
        /* Dump last 8 cmd codes, oldest first. If count < 8, start
         * from index 0 (ring not full yet). If count >= 8, start at
         * (head - 8) & 0xF (last 8 entries of a 16-slot ring, though
         * we only expose 8 of the 16 tracked). */
        ioreq->ios2_DataLength = base->beginio_count;
        UWORD start;
        if (base->beginio_count < 16) {
            start = 0;   /* linear from index 0 */
        } else {
            start = (UWORD)((base->beginio_ring_head - 8) & 0xF);
        }
        UWORD cmds[8];
        for (int i = 0; i < 8; i++) {
            cmds[i] = base->beginio_last_cmds[(start + i) & 0xF];
        }
        ioreq->ios2_WireError  = ((ULONG)cmds[0] << 16) | cmds[1];
        ioreq->ios2_PacketType = ((ULONG)cmds[2] << 16) | cmds[3];
        ioreq->ios2_Data       = (APTR)(((ULONG)cmds[4] << 16) | cmds[5]);
        ioreq->ios2_StatData   = (APTR)(((ULONG)cmds[6] << 16) | cmds[7]);
        break;
    }
    case 0xE003: {  /* Private DBG_TXTRACE — dump last CMD_WRITE state */
        ioreq->ios2_DataLength = base->tx_call_count;
        ioreq->ios2_WireError  = base->tx_last_len;
        ioreq->ios2_PacketType = base->tx_last_ptype;
        ioreq->ios2_Data       = (APTR)base->tx_last_cmd;
        ioreq->ios2_StatData   = (APTR)base->tx_last_err;
        /* Pack first 6 bytes of dst into DstAddr[0..5], first 6 of
         * data0_16 into SrcAddr[0..5]. Return wire len via a
         * separate field abuse — use ios2_Req.io_Actual… actually
         * that doesn't exist on IORequest. Just squeeze into DstAddr's
         * unused-by-us upper bytes… too clever. Skip wire for now. */
        for (int i = 0; i < 6; i++) ioreq->ios2_DstAddr[i] = base->tx_last_dst[i];
        for (int i = 0; i < 6; i++) ioreq->ios2_SrcAddr[i] = base->tx_data0_16[i];
        break;
    }
    case 0xE001: {  /* Private DBG_RXSTATE — pack rx-ring diagnostics */
        if (!base->hw_present) {
            ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
            break;
        }
        struct PCIDevice *pd = base->pciDevice;
        ULONG bar = base->bar_io;
        volatile ULONG cbr  = pd->InLong(bar + RTL_CBR);
        volatile ULONG isr  = pd->InLong(bar + RTL_ISR);
        volatile ULONG cr   = pd->InLong(bar + RTL_CR);
        ioreq->ios2_DataLength = cbr;
        ioreq->ios2_WireError  = isr & 0xFFFF;
        ioreq->ios2_PacketType = cr & 0xFF;
        ioreq->ios2_Data = (APTR)(ULONG)base->irq_count;
        ioreq->ios2_StatData = (APTR)(ULONG)base->irq_last_isr;
        break;
    }
    case 0xE002: {  /* Private DBG_CRTRACE — Init-time CR readback trace */
        ioreq->ios2_DataLength = ((ULONG)base->cr_after_init << 24) |
                                 ((ULONG)base->cr_after_te << 16) |
                                 ((ULONG)base->cr_after_te_re << 8) |
                                 ((ULONG)base->cr_after_re_rewrite);
        break;
    }
    case S2_ONLINE:
        if (!base->hw_present) {
            ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
            ioreq->ios2_WireError    = S2WERR_UNIT_OFFLINE;
        } else {
            base->is_online = TRUE;
            base->stats.Reconfigurations++;
            /* Start unit task if not already running. Task drains RX
             * ring on ISR signal at task priority.
             *
             * Signal choice: use SIGBREAKF_CTRL_E as the RX-wake
             * signal (fixed, not AllocSignal — AllocSignal has to
             * run in the target task's context which complicates
             * cross-task setup). CTRL_C reserved for stop.
             * Cost: don't Break -e to rtl8139re.rx from the console. */
            if (!base->unit_task) {
                base->unit_task_stop = 0;
                base->rx_sigmask = SIGBREAKF_CTRL_E;
                base->unit_task = (struct Task *)
                    base->IExec->CreateTaskTags(
                        (STRPTR)"rtl8139re.rx",
                        1,                         /* priority */
                        (APTR)rtl_unit_task_entry,
                        8192,                       /* stack */
                        AT_Param1, (ULONG)base,
                        TAG_END);
            }
        }
        break;
    case S2_OFFLINE:
        base->is_online = FALSE;
        /* Do NOT tear down unit task here — allow later S2_ONLINE
         * to resume. Task is fully torn down in v_cleanup. */
        break;
    case S2_CONFIGINTERFACE:
        if (!base->hw_present) {
            ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
            ioreq->ios2_WireError    = S2WERR_UNIT_OFFLINE;
        } else if (base->is_configured) {
            ioreq->ios2_Req.io_Error = S2ERR_BAD_STATE;
            ioreq->ios2_WireError    = S2WERR_IS_CONFIGURED;
        } else {
            base->is_configured = TRUE;
            base->is_online     = TRUE;
            /* Return current MAC in DstAddr per SANA-II convention.
             * Caller may set SrcAddr to override; we ignore for now
             * (RTL8139 MAC change requires 9346CR programming). */
            for (int i = 0; i < 6; i++) {
                ioreq->ios2_DstAddr[i] = base->mac[i];
            }
        }
        break;
    case CMD_READ: {
        struct Rtl8139Opener *op = (struct Rtl8139Opener *)
            ioreq->ios2_Req.io_Unit;
        if (!op || !base->is_online) {
            ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
            ioreq->ios2_WireError    = S2WERR_UNIT_OFFLINE;
            break;
        }
        op->packet_type = ioreq->ios2_PacketType;
        base->IExec->ObtainSemaphore(base->io_lock);
        base->IExec->AddTail(&op->pending_reads,
                             (struct Node *)&ioreq->ios2_Req.io_Message.mn_Node);
        base->IExec->ReleaseSemaphore(base->io_lock);
        ioreq->ios2_Req.io_Flags &= ~IOF_QUICK;
        return;
    }
    case S2_READORPHAN: {
        /* Deliver any packet whose type isn't claimed by an opener.
         * Roadshow / packet-sniffer style clients use this. */
        if (!base->is_online) {
            ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
            ioreq->ios2_WireError    = S2WERR_UNIT_OFFLINE;
            break;
        }
        base->IExec->ObtainSemaphore(base->io_lock);
        base->IExec->AddTail(&base->orphan_reads,
                             (struct Node *)&ioreq->ios2_Req.io_Message.mn_Node);
        base->IExec->ReleaseSemaphore(base->io_lock);
        ioreq->ios2_Req.io_Flags &= ~IOF_QUICK;
        return;
    }
    case S2_DEVICEQUERY: {
        /* SANA-II lets the caller supply a partial buffer — some
         * older Roadshow versions allocate only the fields through
         * HardwareType (no RawMTU). Fill each field only if the
         * caller has room for it, and report SizeSupplied as MIN
         * of SizeAvailable and our full struct. Rejecting on
         * "too small" (as we used to) makes Roadshow close+abandon
         * the interface. */
        struct Sana2DeviceQuery *q =
            (struct Sana2DeviceQuery *)ioreq->ios2_StatData;
        if (!q) {
            ioreq->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
            ioreq->ios2_WireError    = S2WERR_NULL_POINTER;
            break;
        }
        ULONG avail = q->SizeAvailable;
        ULONG filled = 4;   /* SizeAvailable already there */
        if (avail >= 8)  { q->SizeSupplied   = 0; filled = 8; }
        if (avail >= 12) { q->DevQueryFormat = 0; filled = 12; }
        if (avail >= 16) { q->DeviceLevel    = 0; filled = 16; }
        if (avail >= 20) { q->AddrFieldSize  = 48; filled = 20; }
        if (avail >= 24) { q->MTU            = 1500; filled = 24; }
        if (avail >= 28) { q->BPS            = 100 * 1000 * 1000; filled = 28; }
        if (avail >= 32) { q->HardwareType   = S2WireType_Ethernet; filled = 32; }
        if (avail >= 36) { q->RawMTU         = 1514; filled = 36; }
        if (avail >= 8) q->SizeSupplied = filled;
        break;
    }
    case S2_GETGLOBALSTATS: {
        struct Sana2DeviceStats *s =
            (struct Sana2DeviceStats *)ioreq->ios2_StatData;
        if (!s) {
            ioreq->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
            ioreq->ios2_WireError    = S2WERR_NULL_POINTER;
        } else {
            /* Field-by-field copy — struct-assign would emit a memcpy
             * call which we can't link against in a resident .device. */
            s->PacketsReceived      = base->stats.PacketsReceived;
            s->PacketsSent          = base->stats.PacketsSent;
            s->BadData              = base->stats.BadData;
            s->Overruns             = base->stats.Overruns;
            s->Unused               = base->stats.Unused;
            s->UnknownTypesReceived = base->stats.UnknownTypesReceived;
            s->Reconfigurations     = base->stats.Reconfigurations;
            s->LastStart            = base->stats.LastStart;
        }
        break;
    }
    case S2_GETSPECIALSTATS: {
        struct Sana2SpecialStatHeader *h =
            (struct Sana2SpecialStatHeader *)ioreq->ios2_StatData;
        if (h) h->RecordCountSupplied = 0;
        break;
    }
    case S2_ADDMULTICASTADDRESS: {
        /* SrcAddr[0..5] = multicast MAC to accept. Hash via CRC-32
         * top 6 bits → index into 64-bit MAR table; bump refcount;
         * flush to chip if the bit's first ref. */
        ULONG crc = rtl_ether_crc(ioreq->ios2_SrcAddr);
        UBYTE idx = (UBYTE)(crc >> 26);          /* top 6 bits */
        base->IExec->ObtainSemaphore(base->io_lock);
        if (base->mar_refs[idx]++ == 0) {
            base->mar[idx >> 3] |= (UBYTE)(1 << (idx & 7));
            rtl_mar_flush(base);
        }
        base->IExec->ReleaseSemaphore(base->io_lock);
        break;
    }
    case S2_DELMULTICASTADDRESS: {
        ULONG crc = rtl_ether_crc(ioreq->ios2_SrcAddr);
        UBYTE idx = (UBYTE)(crc >> 26);
        base->IExec->ObtainSemaphore(base->io_lock);
        if (base->mar_refs[idx] > 0 && --base->mar_refs[idx] == 0) {
            base->mar[idx >> 3] &= (UBYTE)~(1 << (idx & 7));
            rtl_mar_flush(base);
        }
        base->IExec->ReleaseSemaphore(base->io_lock);
        break;
    }
    case NSCMD_DEVICEQUERY: {
        struct NSDeviceQueryResult *q =
            (struct NSDeviceQueryResult *)ioreq->ios2_Data;
        /* 0-terminated command list Roadshow scans. Must be static
         * so the pointer we return stays valid after we ReplyMsg. */
        static uint16 supported[] = {
            CMD_READ, CMD_WRITE,
            S2_DEVICEQUERY, S2_GETSTATIONADDRESS, S2_CONFIGINTERFACE,
            S2_BROADCAST, S2_ONLINE, S2_OFFLINE, S2_READORPHAN,
            S2_ADDMULTICASTADDRESS, S2_DELMULTICASTADDRESS,
            S2_GETGLOBALSTATS, S2_GETSPECIALSTATS,
            NSCMD_DEVICEQUERY,
            0
        };
        if (!q || ioreq->ios2_DataLength < sizeof(*q)) {
            ioreq->ios2_Req.io_Error = IOERR_BADLENGTH;
        } else {
            q->DevQueryFormat    = 0;
            q->SizeAvailable     = sizeof(*q);
            q->DeviceType        = NSDEVTYPE_SANA2;
            q->DeviceSubType     = 0;
            q->SupportedCommands = supported;
            ioreq->ios2_DataLength = sizeof(*q);
        }
        break;
    }
    case 0xC008:  /* S2_SANA2HOOK — Roadshow's fast-path hook install.
                   * MUST return IOERR_NOCMD; otherwise Roadshow thinks
                   * we support the fast path and installs a hook we
                   * never call, so packets go into the void. */
        ioreq->ios2_Req.io_Error = IOERR_NOCMD;
        break;
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
