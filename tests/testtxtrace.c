/*
 * testtxtrace — read DBG_TXTRACE, print the last CMD_WRITE state.
 * Useful for diagnosing whether Roadshow is calling CMD_WRITE.
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

    req->ios2_Req.io_Command = 0xE003;   /* DBG_TXTRACE */
    IExec->DoIO((struct IORequest *)req);

    ULONG count = req->ios2_DataLength;
    ULONG last_len = req->ios2_WireError;
    ULONG ptype    = req->ios2_PacketType;
    ULONG cmd      = (ULONG)req->ios2_Data;
    ULONG err      = (ULONG)req->ios2_StatData;

    IDOS->Printf("tx_call_count=%lu last_len=%lu last_cmd=0x%lx last_ptype=0x%04lx last_err=%ld\n",
                 count, last_len, cmd, ptype, err);
    IDOS->Printf("last_dst=%02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
                 (long)req->ios2_DstAddr[0], (long)req->ios2_DstAddr[1],
                 (long)req->ios2_DstAddr[2], (long)req->ios2_DstAddr[3],
                 (long)req->ios2_DstAddr[4], (long)req->ios2_DstAddr[5]);
    IDOS->Printf("data[0..5]=%02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
                 (long)req->ios2_SrcAddr[0], (long)req->ios2_SrcAddr[1],
                 (long)req->ios2_SrcAddr[2], (long)req->ios2_SrcAddr[3],
                 (long)req->ios2_SrcAddr[4], (long)req->ios2_SrcAddr[5]);

    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);
    IDOS->Printf("DONE\n");
    return 0;
}
