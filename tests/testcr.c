/*
 * testcr — dump the Init-time CR readback trace.
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

    if (IExec->OpenDevice("rtl8139re.device", 0,
                          (struct IORequest *)req, 0)) {
        IDOS->Printf("OpenDevice failed\n");
        return 20;
    }

    req->ios2_Req.io_Command = 0xE002;   /* DBG_CRTRACE */
    IExec->DoIO((struct IORequest *)req);
    ULONG trace = req->ios2_DataLength;
    IDOS->Printf("CR trace: init=%02lx afterTE=%02lx afterTE|RE=%02lx re-write=%02lx\n",
                 (trace >> 24) & 0xFF,
                 (trace >> 16) & 0xFF,
                 (trace >>  8) & 0xFF,
                 (trace      ) & 0xFF);

    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);
    IDOS->Printf("DONE\n");
    return 0;
}
