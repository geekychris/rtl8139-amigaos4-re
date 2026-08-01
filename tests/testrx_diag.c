/*
 * testrx_diag — poll the driver's private RX-state DBG command 5x.
 * Prints RX ring registers so we can see NIC activity.
 *
 * NB: IDOS->Printf uses AmigaDOS RawDoFmt — %d reads a 16-bit WORD,
 * %ld reads a 32-bit LONG. Get this wrong and all subsequent args
 * shift on the stack. Use %ld for everything.
 */

#include <exec/errors.h>
#include <exec/io.h>
#include <devices/sana2.h>

#include <proto/exec.h>
#include <proto/dos.h>

#define DBG_RXSTATE  0xE001

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

    for (long i = 0; i < 5; i++) {
        req->ios2_Req.io_Command = DBG_RXSTATE;
        req->ios2_Req.io_Error   = 0;
        IExec->DoIO((struct IORequest *)req);
        /* driver packs CBR into ios2_DataLength (low 16 bits = CBA
         * = NIC write ptr; high 16 = IMR).
         * ISR into ios2_WireError low 16.
         * CR into ios2_PacketType low 8. */
        ULONG cbr_imr = req->ios2_DataLength;
        ULONG isr     = req->ios2_WireError;
        ULONG cr      = req->ios2_PacketType;
        ULONG irq_ct  = (ULONG)req->ios2_Data;
        ULONG last_isr= (ULONG)req->ios2_StatData;
        IDOS->Printf("iter %ld: CBA=%04lx IMR=%04lx  ISR=%04lx  CR=%02lx  irq_count=%lu last_isr=%04lx\n",
                     i,
                     cbr_imr & 0xFFFF,
                     (cbr_imr >> 16) & 0xFFFF,
                     isr & 0xFFFF,
                     cr & 0xFF,
                     irq_ct,
                     last_isr);
        IDOS->Delay(50);  /* 1 second */
    }

    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);
    return 0;
}
