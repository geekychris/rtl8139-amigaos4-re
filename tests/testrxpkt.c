/*
 * testrxpkt — post a CMD_READ for ARP (0x0806), wait up to 8s for
 * the driver to deliver a received ARP frame. Prints src/dst MAC
 * plus first few payload bytes.
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
    struct IOSana2Req *req = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
        ASOIOR_ReplyPort, port,
        ASOIOR_Size, sizeof(struct IOSana2Req),
        TAG_END);
    if (!port || !req) return 20;

    if (IExec->OpenDevice("rtl8139re.device", 0,
                          (struct IORequest *)req, 0)) {
        IDOS->Printf("Open failed\n");
        return 20;
    }

    req->ios2_Req.io_Command = S2_ONLINE;
    IExec->DoIO((struct IORequest *)req);

    UBYTE buf[1600];
    for (unsigned i = 0; i < sizeof(buf); i++) buf[i] = 0;
    req->ios2_Req.io_Command = CMD_READ;
    req->ios2_Req.io_Error   = 0;
    req->ios2_Data           = buf;
    req->ios2_DataLength     = sizeof(buf);
    req->ios2_PacketType     = 0x0806;   /* ARP */
    IExec->SendIO((struct IORequest *)req);
    IDOS->Printf("SendIO CMD_READ (ARP), waiting up to 8s...\n");

    /* Wait signal or timeout — poll every 400ms. */
    for (int t = 0; t < 20; t++) {
        IDOS->Delay(20);   /* 20/50 s = 400ms */
        struct Message *m = IExec->GetMsg(port);
        if (m) {
            IDOS->Printf("GOT packet! err=%ld len=%ld type=0x%04lx\n"
                         "  DST=%02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n"
                         "  SRC=%02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n"
                         "  bytes 0..15: %02lx %02lx %02lx %02lx %02lx %02lx %02lx %02lx %02lx %02lx %02lx %02lx %02lx %02lx %02lx %02lx\n",
                         (long)req->ios2_Req.io_Error,
                         (long)req->ios2_DataLength,
                         (long)req->ios2_PacketType,
                         (long)req->ios2_DstAddr[0], (long)req->ios2_DstAddr[1],
                         (long)req->ios2_DstAddr[2], (long)req->ios2_DstAddr[3],
                         (long)req->ios2_DstAddr[4], (long)req->ios2_DstAddr[5],
                         (long)req->ios2_SrcAddr[0], (long)req->ios2_SrcAddr[1],
                         (long)req->ios2_SrcAddr[2], (long)req->ios2_SrcAddr[3],
                         (long)req->ios2_SrcAddr[4], (long)req->ios2_SrcAddr[5],
                         (long)buf[0], (long)buf[1], (long)buf[2], (long)buf[3],
                         (long)buf[4], (long)buf[5], (long)buf[6], (long)buf[7],
                         (long)buf[8], (long)buf[9], (long)buf[10], (long)buf[11],
                         (long)buf[12], (long)buf[13], (long)buf[14], (long)buf[15]);
            goto done;
        }
    }
    IDOS->Printf("TIMEOUT — no packet delivered in 8s. Aborting.\n");
    IExec->AbortIO((struct IORequest *)req);
    IExec->WaitIO((struct IORequest *)req);
done:
    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);
    IDOS->Printf("DONE\n");
    return 0;
}
