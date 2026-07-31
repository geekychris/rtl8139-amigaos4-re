#ifndef RTL8139RE_H
#define RTL8139RE_H

/*
 * rtl8139re.device — C reconstruction of Hyperion's rtl8139.device 53.4.
 *
 * Structure derived from RE'd disassembly at /Users/chris/tmp_bsd/rtl.asm
 * (9491 lines, ELF32-PPC-AmigaOS binary at /Users/chris/tmp_bsd/rtl8139.device).
 *
 * Function-offset tables for IExec (offset 0x2c in DevBase) and
 * PCIDevice (offset 0x48 in DevBase) verified via probe programs
 * compiled with the walkero cross-toolchain — see memory
 * [[rtl8139-dma-pattern]] for the full table.
 *
 * DevBase field layout matches the original where field offsets are
 * observable from the disassembly (r31+0x2c, r31+0x30, r31+0x48, etc.).
 * Fields for which we haven't RE'd the exact offset are added at the
 * tail so nothing shifts.
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

/* Structure layout observed from rtl.asm register indirections. The
 * "@N" comments give the offset from which the driver's compiled code
 * accesses each field (r31+N). Init sequence at 0x1000320+ fills these:
 *
 *   r31+0x2c  = IExec           (used everywhere for library calls)
 *   r31+0x30  = pciDevice       (used everywhere for PCI I/O)
 *   r31+0x38  = ExpansionBase   (openlib result, main->interface fetched)
 *   r31+0x40  = IUtility        (interface pointer)
 *   r31+0x44  = DOSBase
 *   r31+0x48  = IExpansion
 *   r31+0x4C  = ?
 *   r31+0x54  = IPCI-ish?  (used with FindDevice etc)
 *   r31+0xA8  = has_newmemory (byte flag)
 *
 * These are BEST GUESSES until the function map agent finishes. Adjust
 * as we translate each function.
 */

struct Rtl8139ReBase
{
    struct Device      dev_Base;            /* @0  - required first field */
    ULONG              dev_SegList;         /* @32 (approx) */
    APTR               _pad_38;             /* @40 */
    struct ExecIFace  *IExec;               /* @44 (0x2c) - confirmed */
    struct PCIDevice  *pciDevice;           /* @48 (0x30) - confirmed */
    APTR               _pad_52;             /* @52 (0x34) */
    APTR               _pad_56;             /* @56 (0x38) - IUtility? */
    APTR               _pad_60;             /* @60 (0x3c) */
    struct Library    *DOSBase;             /* @64 (0x40) */
    APTR               _pad_68;             /* @68 (0x44) */
    struct PCIIFace   *IPCI;                /* @72 (0x48) - confirmed  */
    /* Padding to reach observed offsets; refined as RE proceeds. */
    UBYTE              _pad_76[168 - 76];
    UBYTE              has_newmemory;       /* @168 (0xa8) - probe result flag */
    UBYTE              _pad_169[512 - 169];
};

#endif
