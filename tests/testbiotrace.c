/*
 * testbiotrace — read DBG_BIOTRACE (0xE004): total BeginIO count + ring
 * of last 8 command codes.
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
        IDOS->Printf("Open failed\n"); return 20;
    }

    req->ios2_Req.io_Command = 0xE004;   /* DBG_BIOTRACE */
    IExec->DoIO((struct IORequest *)req);

    ULONG count = req->ios2_DataLength;
    ULONG p01 = req->ios2_WireError;
    ULONG p23 = req->ios2_PacketType;
    ULONG p45 = (ULONG)req->ios2_Data;
    ULONG p67 = (ULONG)req->ios2_StatData;

    IDOS->Printf("beginio_count=%lu\n", count);
    IDOS->Printf("last 8 cmds (oldest -> newest):\n");
    IDOS->Printf("  %04lx %04lx %04lx %04lx %04lx %04lx %04lx %04lx\n",
                 (p01 >> 16) & 0xFFFF, p01 & 0xFFFF,
                 (p23 >> 16) & 0xFFFF, p23 & 0xFFFF,
                 (p45 >> 16) & 0xFFFF, p45 & 0xFFFF,
                 (p67 >> 16) & 0xFFFF, p67 & 0xFFFF);

    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);
    IDOS->Printf("DONE\n");
    return 0;
}
