/*
 * testopen — minimal probe that opens rtl8139re.device and reports.
 * If Phase 0 skeleton is correct: OpenDevice returns 0, CloseDevice
 * runs cleanly, and IOERR_NOCMD is what any command replies with.
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

    /* Fire NSCMD_DEVICEQUERY — should reply IOERR_NOCMD in Phase 0. */
    req->ios2_Req.io_Command = 0x4000;  /* NSCMD_DEVICEQUERY */
    LONG rc = IExec->DoIO((struct IORequest *)req);
    IDOS->Printf("NSCMD_DEVICEQUERY: DoIO=%ld io_Error=%ld\n",
                 (long)rc, (long)req->ios2_Req.io_Error);

    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);
    IDOS->Printf("OK\n");
    return 0;
}
