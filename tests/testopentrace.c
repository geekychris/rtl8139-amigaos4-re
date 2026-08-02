/* testopentrace — DBG_OPENTRACE (0xE005): Open/Close counters. */
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
        IDOS->Printf("Open failed\n"); return 20;
    }
    req->ios2_Req.io_Command = 0xE005;
    IExec->DoIO((struct IORequest *)req);
    IDOS->Printf("open_count=%lu close_count=%lu open_last_err=%ld\n",
                 (unsigned long)req->ios2_DataLength,
                 (unsigned long)req->ios2_WireError,
                 (long)req->ios2_PacketType);
    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);
    IDOS->Printf("DONE\n");
    return 0;
}
