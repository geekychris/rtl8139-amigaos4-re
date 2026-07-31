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

    /* PCI plumbing — Phase C will populate these once the RTL8139 is
     * enumerated. NULL until then. */
    struct PCIDevice  *pciDevice;
    ULONG              pci_vendor;
    ULONG              pci_device;
};

/* Vendor:Device pairs the driver matches. Sourced from the rodata table
 * at 0x100a370 of the original binary — 16 entries + 0xFFFFFFFF
 * terminator. Order matches original. */
struct Rtl8139DeviceID {
    UWORD vendor;
    UWORD device;
};
extern const struct Rtl8139DeviceID rtl8139_device_ids[];

#endif
