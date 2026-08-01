/*
 * testread — SendIO a CMD_READ, verify it stays pending (no reply),
 * then AbortIO + CloseDevice cleanly. Confirms the opener list +
 * pending-read queueing works without touching the (broken) RX path.
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

    LONG err = IExec->OpenDevice("rtl8139re.device", 0,
                                 (struct IORequest *)req, 0);
    IDOS->Printf("OpenDevice: rc=%ld unit=%p\n",
                 (long)err, req->ios2_Req.io_Unit);
    if (err) goto out;

    /* Bring online. */
    req->ios2_Req.io_Command = S2_ONLINE;
    IExec->DoIO((struct IORequest *)req);

    /* Set a payload buffer + packet type for the READ. */
    UBYTE buf[1600];
    for (unsigned i = 0; i < sizeof(buf); i++) buf[i] = 0;
    req->ios2_Req.io_Command = CMD_READ;
    req->ios2_Req.io_Error   = 0;
    req->ios2_Data           = buf;
    req->ios2_DataLength     = sizeof(buf);
    req->ios2_PacketType     = 0x0800; /* IPv4 */

    IExec->SendIO((struct IORequest *)req);
    IDOS->Printf("SendIO(CMD_READ, PacketType=0x0800) fired\n");

    /* Give the driver a moment; poll msgport to see if it's replied. */
    IDOS->Delay(50);
    struct Message *m = IExec->GetMsg(port);
    IDOS->Printf("GetMsg after 1s: %s (want 'no msg' since RX not fired)\n",
                 m ? "GOT REPLY (unexpected)" : "no msg — read is pending");
    if (m) {
        IDOS->Printf("  io_Error=%ld\n", (long)req->ios2_Req.io_Error);
    }

    /* AbortIO so CloseDevice doesn't get stuck. */
    IExec->AbortIO((struct IORequest *)req);
    IExec->WaitIO((struct IORequest *)req);
    IDOS->Printf("AbortIO+WaitIO: io_Error=%ld (want -2 IOERR_ABORTED)\n",
                 (long)req->ios2_Req.io_Error);

    /* Verify a second CMD_READ can be queued (single opener holds many). */
    req->ios2_Req.io_Command = CMD_READ;
    req->ios2_Req.io_Error   = 0;
    req->ios2_Data           = buf;
    req->ios2_DataLength     = sizeof(buf);
    req->ios2_PacketType     = 0x0806; /* ARP */
    IExec->SendIO((struct IORequest *)req);
    IExec->AbortIO((struct IORequest *)req);
    IExec->WaitIO((struct IORequest *)req);
    IDOS->Printf("Second CMD_READ (ARP) abort: err=%ld\n",
                 (long)req->ios2_Req.io_Error);

    IExec->CloseDevice((struct IORequest *)req);
    IDOS->Printf("CloseDevice OK\n");
out:
    if (req)  IExec->FreeSysObject(ASOT_IOREQUEST, req);
    if (port) IExec->FreeSysObject(ASOT_PORT, port);
    IDOS->Printf("DONE\n");
    return 0;
}
