/*
 * testopen — probes rtl8139re.device.
 *   1. OpenDevice on unit 0 (expected: rc=0 IF Init found a PCI device)
 *   2. NSCMD_DEVICEQUERY (expected in Phase A/B: IOERR_NOCMD = -3)
 *   3. Read PCI ident from device's config space via the PCI iface,
 *      so we can cross-check what the driver bound.
 */

#include <exec/errors.h>
#include <exec/io.h>
#include <devices/sana2.h>
#include <interfaces/expansion.h>
#include <expansion/pci.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>

/* RTL8139 family IDs we expect the driver to bind. */
static const struct {
    UWORD vendor;
    UWORD device;
    const char *name;
} known[] = {
    {0x10EC, 0x8139, "Realtek RTL8139"},
    {0x10EC, 0x8138, "Realtek RTL8138"},
    {0xFFFF, 0xFFFF, NULL}
};

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* First — probe from userspace that a PCI device exists at all. */
    struct Library *ExpBase = IExec->OpenLibrary("expansion.library", 51);
    struct PCIIFace *IPCI = ExpBase
        ? (struct PCIIFace *)IExec->GetInterface(ExpBase, "pci", 1, NULL)
        : NULL;
    if (IPCI) {
        for (int i = 0; known[i].name; i++) {
            struct PCIDevice *pd = IPCI->FindDeviceTags(
                FDT_VendorID, known[i].vendor,
                FDT_DeviceID, known[i].device,
                FDT_Index,    (ULONG)0,
                TAG_END);
            if (pd) {
                IDOS->Printf("PCI probe: found %s (%04lx:%04lx) at %p\n",
                             known[i].name,
                             (unsigned long)known[i].vendor,
                             (unsigned long)known[i].device,
                             pd);
                IPCI->FreeDevice(pd);
            }
        }
        IExec->DropInterface((struct Interface *)IPCI);
    }
    if (ExpBase) IExec->CloseLibrary(ExpBase);

    /* Now open the device we're testing. */
    struct MsgPort *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (!port) return 20;
    struct IOSana2Req *req = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
        ASOIOR_ReplyPort, port,
        ASOIOR_Size, sizeof(struct IOSana2Req),
        TAG_END);
    if (!req) { IExec->FreeSysObject(ASOT_PORT, port); return 20; }

    LONG err = IExec->OpenDevice("rtl8139re.device", 0,
                                 (struct IORequest *)req, 0);
    IDOS->Printf("OpenDevice(rtl8139re.device, unit=0): rc=%ld\n", (long)err);
    if (err) {
        IDOS->Printf("  -> IOERR_OPENFAIL (%d): driver rejected the Open — "
                     "either init failed or PCI device wasn't found\n",
                     IOERR_OPENFAIL);
        IExec->FreeSysObject(ASOT_IOREQUEST, req);
        IExec->FreeSysObject(ASOT_PORT, port);
        return 20;
    }

    req->ios2_Req.io_Command = 0x4000;   /* NSCMD_DEVICEQUERY */
    LONG rc = IExec->DoIO((struct IORequest *)req);
    IDOS->Printf("NSCMD_DEVICEQUERY: DoIO=%ld io_Error=%ld\n",
                 (long)rc, (long)req->ios2_Req.io_Error);

    /* S2_GETSTATIONADDRESS: retrieve the MAC our driver read from
     * IDR0/IDR4. Fills ios2_SrcAddr[0..5] (HW MAC) and _DstAddr.
     * Pre-set SrcAddr[0..5] to a MARKER (0xAA) so if driver doesn't
     * write, we see 0xAA (not 0). Distinguishes "not written" from
     * "written but cleared". */
    for (int i = 0; i < 6; i++) {
        req->ios2_SrcAddr[i] = 0xAA;
        req->ios2_DstAddr[i] = 0xBB;
    }
    req->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
    req->ios2_Req.io_Error   = 0;
    LONG rc2 = IExec->DoIO((struct IORequest *)req);
    IDOS->Printf("S2_GETSTATIONADDRESS: DoIO=%ld io_Error=%ld\n",
                 (long)rc2, (long)req->ios2_Req.io_Error);
    /* Report MAC via the ULONG workaround (SrcAddr/DstAddr writes get
     * lost on this OS4 setup — see driver notes). */
    ULONG w0 = req->ios2_DataLength;
    ULONG w1 = req->ios2_PacketType;
    IDOS->Printf("  MAC (via ULONG workaround) = %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
                 (w0 >> 24) & 0xFF, (w0 >> 16) & 0xFF,
                 (w0 >>  8) & 0xFF, (w0      ) & 0xFF,
                 (w1 >> 24) & 0xFF, (w1 >> 16) & 0xFF);

    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);
    IDOS->Printf("OK\n");
    return 0;
}
