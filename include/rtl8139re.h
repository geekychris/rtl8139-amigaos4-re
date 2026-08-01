#ifndef RTL8139RE_H
#define RTL8139RE_H

/*
 * rtl8139re.device — AmigaOS 4 SANA-II network driver for the
 * Realtek RTL8139 family (and RTL8139-compatible variants).
 *
 * Targets QEMU sam460ex + real sam460ex hardware. Single-unit, uses
 * PCI I/O BAR for register access (no MMIO), 4-slot TX ring in
 * MEMF_KICK memory, direct-register DMA (TSAD0-3 addressed).
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
    struct Device      dev_Base;
    ULONG              dev_SegList;
    APTR               io_lock;             /* ASOT_SEMAPHORE */
    struct ExecIFace  *IExec;

    /* Libraries + their "main" interfaces. NULL until Init acquires
     * them; DevCleanup releases whatever is non-NULL, so partial
     * Init failure still tears down cleanly. */
    struct Library     *DOSBase;
    struct DOSIFace    *IDOS;
    struct Library     *ExpansionBase;
    struct ExpansionIFace *IExpansion;
    struct PCIIFace    *IPCI;
    struct Library     *UtilityBase;
    struct UtilityIFace *IUtility;

    UBYTE              has_newmemory;       /* newmemory.resource probe result */
    UBYTE              _pad_1[3];

    /* PCI plumbing populated in Init once the RTL8139 is enumerated. */
    struct PCIDevice  *pciDevice;
    ULONG              pci_vendor;
    ULONG              pci_device;

    /* BAR0 is the RTL8139 I/O-port register file. bar_io holds the
     * port base; PCIDevice.InLong/OutLong to bar_io+offset. */
    ULONG              bar_io;
    struct PCIResourceRange *bar_range;

    /* MAC address, fetched from IDR0..IDR5 by S2_GETSTATIONADDRESS. */
    UBYTE              mac[6];
    UBYTE              _pad_mac[2];
    BOOL               hw_present;

    /* TX pool: 4 slots × 2 KB in MEMF_KICK|MEMF_CLEAR memory. CMD_WRITE
     * copies packet, flushes cache, publishes DMA phys to TSAD[slot],
     * kicks TSD[slot], polls TSD.TOK. Round-robin slot picker. */
    APTR               tx_buffer_raw;
    ULONG              tx_buffer_raw_size;
    APTR               tx_buffer;
    ULONG              tx_buffer_phys;
    UBYTE              tx_next_slot;
    UBYTE              _pad_tx[3];

    /* RX ring: single 8KB circular buffer + 16-byte no-wrap padding
     * + a few extra bytes for chip prefetch. NIC writes incoming
     * packets into it starting at CBA offset, driver reads and
     * advances CAPR. Each packet is prefixed by a 4-byte header
     * (u16 status | u16 length). */
    APTR               rx_ring_raw;
    ULONG              rx_ring_raw_size;
    APTR               rx_ring;
    ULONG              rx_ring_phys;

    /* IRQ handler state. irq_installed guards Expunge so we RemIntServer
     * only if AddIntServer succeeded. Counters are volatile since the
     * ISR bumps them from interrupt context. */
    struct Interrupt   irq_node;
    ULONG              irq_vector;
    BOOL               irq_installed;
    volatile ULONG     irq_count;         /* total ISR fires */
    volatile ULONG     irq_last_isr;      /* last ISR value seen */
};

#define RTL_RX_RING_SIZE  (8 * 1024)      /* RBLEN=00 → 8 KB */
#define RTL_RX_RING_PAD   16              /* WRAP=1 padding */

/* RTL8139 RCR (Receive Configuration) bits */
#define RTL_RCR_AAP   0x0001   /* accept all physical (promiscuous) */
#define RTL_RCR_APM   0x0002   /* accept physical match */
#define RTL_RCR_AM    0x0004   /* accept multicast */
#define RTL_RCR_AB    0x0008   /* accept broadcast */
#define RTL_RCR_WRAP  0x0080   /* 1 = pad overflow to +16, no wrap in ring */
#define RTL_RCR_MXDMA_UNLIMITED 0x0700   /* MXDMA=111 */
#define RTL_RCR_RXFTH_NONE     0xE000   /* wait — layout differs across revisions,
                                         * safe conservative: no threshold */

/* Sensible default RCR: broadcast + physical match + wrap-pad-16 +
 * unlimited DMA burst. Add AAP for promiscuous / debugging. */
#define RTL_RCR_DEFAULT   (RTL_RCR_APM | RTL_RCR_AB | RTL_RCR_WRAP | RTL_RCR_MXDMA_UNLIMITED)

/* RTL8139 ISR / IMR bits (both registers share layout, at BAR+0x3C
 * and BAR+0x3E respectively — u16). Writing 1 to an ISR bit clears
 * it. Writing 1 to an IMR bit enables that interrupt cause. */
#define RTL_ISR_ROK    0x0001   /* RX OK */
#define RTL_ISR_RER    0x0002   /* RX Error */
#define RTL_ISR_TOK    0x0004   /* TX OK */
#define RTL_ISR_TER    0x0008   /* TX Error */
#define RTL_ISR_RXOVW  0x0010   /* RX buffer overflow */
#define RTL_ISR_PUN    0x0020   /* Packet Underrun / Link Change */
#define RTL_ISR_FOVW   0x0040   /* RX FIFO overflow */
#define RTL_ISR_SERR   0x8000   /* System Error */

#define RTL_IMR_DEFAULT   (RTL_ISR_ROK | RTL_ISR_RER | RTL_ISR_TOK | RTL_ISR_TER | RTL_ISR_RXOVW | RTL_ISR_PUN | RTL_ISR_FOVW | RTL_ISR_SERR)

/* RTL8139 register offsets from BAR I/O base. */
#define RTL_IDR0   0x00
#define RTL_IDR4   0x04
#define RTL_MAR0   0x08
#define RTL_TSD0   0x10   /* TX Status of Descriptor 0..3 */
#define RTL_TSAD0  0x20   /* TX Start Address of Descriptor 0..3 */
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

#define RTL_CR_TE  0x04
#define RTL_CR_RE  0x08
#define RTL_CR_RST 0x10

#define RTL_TX_BUF_SIZE   2048
#define RTL_TX_SLOTS      4

/* Vendor:Device pairs the driver matches (RTL8139 family + rebadges). */
struct Rtl8139DeviceID {
    UWORD vendor;
    UWORD device;
};
extern const struct Rtl8139DeviceID rtl8139_device_ids[];

#endif
