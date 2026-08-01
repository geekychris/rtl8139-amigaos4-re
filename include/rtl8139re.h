#ifndef RTL8139RE_H
#define RTL8139RE_H

/*
 * rtl8139re.device — C reconstruction of Hyperion's rtl8139.device 53.4.
 *
 * Structure derived from RE'd disassembly at /Users/chris/tmp_bsd/rtl.asm
 * (9491 lines) and the function map in docs/FUNCTION_MAP.md.
 *
 * Struct field layout follows what the disassembly OBSERVABLY accesses,
 * where the exact byte offset is knowable from r31+N loads/stores in the
 * asm. Where the offset is arbitrary (compiler chose it), we use natural
 * C ordering — the C struct doesn't have to be byte-identical to the
 * original, only functionally equivalent.
 *
 * Observed offsets on the driver-base (r31 in the asm):
 *   r31+36 (0x24) = dev_SegList   (BPTR from CLT_InitFunc arg 2)
 *   r31+40 (0x28) = io_lock       (AllocSysObject(ASOT_SEMAPHORE) result)
 *   r31+44 (0x2C) = IExec         (CLT_InitFunc arg 3; confirmed everywhere)
 *   r31+168 (0xA8)= has_newmemory (byte flag; probe of newmemory.resource)
 */

#include <exec/devices.h>
#include <exec/interfaces.h>
#include <exec/interrupts.h>
#include <exec/io.h>
#include <exec/libraries.h>
#include <exec/semaphores.h>
#include <exec/types.h>
#include <dos/dos.h>
#include <devices/sana2.h>
#include <expansion/pci.h>
#include <interfaces/expansion.h>
#include <utility/tagitem.h>
#include <utility/hooks.h>
#include <interfaces/utility.h>

#include <proto/exec.h>

struct DOSIFace;

struct Rtl8139ReBase
{
    struct Device      dev_Base;         /* @0-35 */
    ULONG              dev_SegList;      /* @36 */
    APTR               io_lock;          /* @40 - ASOT_SEMAPHORE */
    struct ExecIFace  *IExec;            /* @44 - confirmed */

    /* Libraries + their "main" interfaces. Offsets past @44 are the C
     * compiler's choice — not tied to any specific asm offset. Everything
     * NULL initially; each Init step fills its slot; DevCleanup releases
     * whatever is non-NULL, so a partially-completed Init cleans up
     * safely. */
    struct Library     *DOSBase;
    struct DOSIFace    *IDOS;
    struct Library     *ExpansionBase;
    struct ExpansionIFace *IExpansion;
    struct PCIIFace    *IPCI;            /* GetInterface(ExpansionBase, "pci", 1) */
    struct Library     *UtilityBase;
    struct UtilityIFace *IUtility;

    UBYTE              has_newmemory;    /* @168 originally - kept as flag */
    UBYTE              _pad_1[3];

    /* PCI plumbing — Phase B populates pciDevice + vendor/device. */
    struct PCIDevice  *pciDevice;
    ULONG              pci_vendor;
    ULONG              pci_device;

    /* Phase C: BAR + MAC. RTL8139 exposes BAR0 as an I/O-port range
     * (16 bytes wide) and BAR1 as memory-mapped mirror of the same
     * registers. The Hyperion binary uses BAR0 (I/O) via
     * PCIDevice.InLong/OutLong, which auto-byteswaps on PPC. We do
     * the same. Original stores bar_io at unit+192 in the per-unit
     * struct; we hang it off the driver base for now since we're
     * still single-unit. */
    ULONG              bar_io;              /* io_base for PCIDevice.InLong/OutLong */
    struct PCIResourceRange *bar_range;     /* keep so we can FreeResourceRange */

    /* MAC address read from IDR0-5 at Init time. Serves
     * S2_GETSTATIONADDRESS + fills the outgoing frame src MAC. */
    UBYTE              mac[6];
    UBYTE              _pad_mac[2];
    BOOL               hw_present;          /* TRUE iff BAR + MAC read OK */

    /* Phase D: TX buffers — 4 slots (matches RTL8139 hardware). Each
     * is a small MEMF_KICK|MEMF_CLEAR block that CPU writes to and the
     * NIC DMA-reads. Buffers stored contiguously so we can allocate
     * ONE block and slice. Original driver has ONE buffer per active
     * TX request; we do simpler — 4 preallocated slots. */
    APTR               tx_buffer_raw;      /* pre-alignment ptr for FreeMem */
    ULONG              tx_buffer_raw_size;
    APTR               tx_buffer;          /* aligned base of TX pool */
    ULONG              tx_buffer_phys;     /* DMA phys of tx_buffer */
    UBYTE              tx_next_slot;       /* round-robin 0..3 */
    UBYTE              _pad_tx[3];
};

#define RTL_TX_BUF_SIZE   2048  /* per-slot; RTL8139 max frame is 1518 */
#define RTL_TX_SLOTS      4

/* RTL8139 register offsets (from BAR I/O base). Full spec in the
 * Realtek datasheet; we replicate what the original binary uses. */
#define RTL_IDR0   0x00
#define RTL_IDR4   0x04
#define RTL_MAR0   0x08
#define RTL_TSD0   0x10   /* Transmit Status of Descriptor 0..3 */
#define RTL_TSAD0  0x20   /* Transmit Start Addr of Descriptor 0..3 */
#define RTL_RBSTART 0x30
#define RTL_ERBCR  0x34
#define RTL_CR     0x37   /* Command Register (byte) */
#define RTL_CAPR   0x38
#define RTL_CBR    0x3A
#define RTL_IMR    0x3C   /* Interrupt Mask (word) */
#define RTL_ISR    0x3E   /* Interrupt Status (word) */
#define RTL_TCR    0x40   /* Transmit Config */
#define RTL_RCR    0x44   /* Receive Config */
#define RTL_9346CR 0x50   /* EEPROM Command */

/* Command Register bits (write byte to CR) */
#define RTL_CR_TE  0x04
#define RTL_CR_RE  0x08
#define RTL_CR_RST 0x10

/* Vendor:Device pairs the driver matches. Sourced from the rodata table
 * at 0x100a370 of the original binary — 16 entries + 0xFFFFFFFF
 * terminator. Order matches original. */
struct Rtl8139DeviceID {
    UWORD vendor;
    UWORD device;
};
extern const struct Rtl8139DeviceID rtl8139_device_ids[];

#endif
