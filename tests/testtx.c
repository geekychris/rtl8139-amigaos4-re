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

    /* Build a broadcast ARP request (60-byte minimum Ethernet frame).
     * Ethernet: dst=FF*6, src=52:54:00:12:34:59, type=0806 (ARP)
     * ARP: HW type=1 (Ethernet), Proto=0800, HW/Proto len=6/4,
     *      Op=1 (request), then sender+target MAC/IP fields. */
    UBYTE frame[60];
    for (int i = 0; i < 60; i++) frame[i] = 0;
    for (int i = 0; i < 6; i++) frame[i] = 0xFF;   /* dst broadcast */
    frame[6]  = 0x52; frame[7]  = 0x54;  /* src MAC */
    frame[8]  = 0x00; frame[9]  = 0x12;
    frame[10] = 0x34; frame[11] = 0x59;
    frame[12] = 0x08; frame[13] = 0x06;  /* Ethertype ARP */
    frame[14] = 0x00; frame[15] = 0x01;  /* HW=Ethernet */
    frame[16] = 0x08; frame[17] = 0x00;  /* Proto=IPv4 */
    frame[18] = 0x06; frame[19] = 0x04;  /* len 6/4 */
    frame[20] = 0x00; frame[21] = 0x01;  /* op=request */

    req->ios2_Req.io_Command = CMD_WRITE;
    req->ios2_Req.io_Error   = 0;
    req->ios2_DataLength     = 60;
    req->ios2_Data           = (APTR)frame;

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
