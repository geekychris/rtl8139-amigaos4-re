/*
 * testtx — send a broadcast ARP frame via rtl8139re CMD_WRITE.
 *
 * On success: /tmp/qemu-n3.pcap should contain a real broadcast ARP
 * packet, not zeros. That validates the whole TX path we just built.
 *
 * No S2_CONFIGINTERFACE / S2_ONLINE — the driver enables TE at Init
 * time so CMD_WRITE works immediately after OpenDevice.
 */

#include <exec/errors.h>
#include <exec/io.h>
#include <devices/sana2.h>

#include <proto/exec.h>
#include <proto/dos.h>

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct MsgPort *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (!port) return 20;
    struct IOSana2Req *req = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
        ASOIOR_ReplyPort, port,
        ASOIOR_Size, sizeof(struct IOSana2Req),
        TAG_END);
    if (!req) { IExec->FreeSysObject(ASOT_PORT, port); return 20; }

    LONG err = IExec->OpenDevice("rtl8139re.device", 0,
                                 (struct IORequest *)req, 0);
    IDOS->Printf("OpenDevice: rc=%ld\n", (long)err);
    if (err) {
        IExec->FreeSysObject(ASOT_IOREQUEST, req);
        IExec->FreeSysObject(ASOT_PORT, port);
        return 20;
    }

    /* Cooked-mode ARP request: payload only (no ETH header — driver
     * builds that from ios2_DstAddr + ios2_PacketType).
     * ARP body: HW type=1 (Ethernet), Proto=0800, HW/Proto len=6/4,
     *           Op=1 (request), then sender+target MAC/IP fields. */
    UBYTE arp[28];
    for (int i = 0; i < 28; i++) arp[i] = 0;
    arp[0]  = 0x00; arp[1]  = 0x01;   /* HW=Ethernet */
    arp[2]  = 0x08; arp[3]  = 0x00;   /* Proto=IPv4 */
    arp[4]  = 0x06; arp[5]  = 0x04;   /* len 6/4 */
    arp[6]  = 0x00; arp[7]  = 0x01;   /* op=request */
    /* Sender MAC bytes 8..13 = our MAC (rtl8139re's), 14..17 = 0.0.0.0
     * Target MAC 18..23 = 0, IP 24..27 = 0.0.0.1 (arbitrary). */
    arp[8]  = 0x52; arp[9]  = 0x54;
    arp[10] = 0x00; arp[11] = 0x12;
    arp[12] = 0x34; arp[13] = 0x59;
    arp[27] = 0x01;

    /* Broadcast MAC in DstAddr; ARP ethertype in PacketType. */
    for (int i = 0; i < 6; i++) req->ios2_DstAddr[i] = 0xFF;
    req->ios2_PacketType     = 0x0806;   /* ARP */
    req->ios2_Req.io_Command = CMD_WRITE;
    req->ios2_Req.io_Error   = 0;
    req->ios2_DataLength     = 28;
    req->ios2_Data           = (APTR)arp;

    LONG rc = IExec->DoIO((struct IORequest *)req);
    IDOS->Printf("CMD_WRITE: DoIO=%ld io_Error=%ld wire=0x%lx\n",
                 (long)rc, (long)req->ios2_Req.io_Error,
                 (unsigned long)req->ios2_WireError);
    if (rc == 0) {
        IDOS->Printf("  -> RESULT: driver accepted the frame; "
                     "check /tmp/qemu-n3.pcap on host for the packet\n");
    }

    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);
    IDOS->Printf("OK\n");
    return 0;
}
