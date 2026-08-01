/*
 * testg — exercise the Phase G SANA-II command surface:
 *   NSCMD_DEVICEQUERY, S2_DEVICEQUERY, S2_ONLINE, S2_OFFLINE,
 *   S2_GETGLOBALSTATS, S2_CONFIGINTERFACE.
 *
 * All %ld — RawDoFmt %d is 16-bit and would shift the stack.
 */

#include <exec/errors.h>
#include <exec/io.h>
#include <devices/sana2.h>
#include <devices/newstyle.h>

#include <proto/exec.h>
#include <proto/dos.h>

#define D(fmt, ...) IDOS->Printf((CONST_STRPTR)fmt, ##__VA_ARGS__)

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
    D("OpenDevice: rc=%ld\n", (long)err);
    if (err) goto out;

    /* 1. NSCMD_DEVICEQUERY */
    struct NSDeviceQueryResult nsq;
    for (unsigned i = 0; i < sizeof(nsq); i++) ((UBYTE *)&nsq)[i] = 0;
    nsq.SizeAvailable = sizeof(nsq);
    req->ios2_Req.io_Command = NSCMD_DEVICEQUERY;
    req->ios2_Req.io_Error   = 0;
    req->ios2_Data           = &nsq;
    req->ios2_DataLength     = sizeof(nsq);
    LONG rc = IExec->DoIO((struct IORequest *)req);
    D("NSCMD_DEVICEQUERY: rc=%ld err=%ld type=%ld sub=%ld cmds=%p\n",
      (long)rc, (long)req->ios2_Req.io_Error,
      (long)nsq.DeviceType, (long)nsq.DeviceSubType,
      nsq.SupportedCommands);
    if (nsq.SupportedCommands) {
        D("  Supported: ");
        for (uint16 *p = nsq.SupportedCommands; *p; p++) D("%04lx ", (long)*p);
        D("\n");
    }

    /* 2. S2_DEVICEQUERY */
    struct Sana2DeviceQuery sq;
    for (unsigned i = 0; i < sizeof(sq); i++) ((UBYTE *)&sq)[i] = 0;
    sq.SizeAvailable = sizeof(sq);
    req->ios2_Req.io_Command = S2_DEVICEQUERY;
    req->ios2_Req.io_Error   = 0;
    req->ios2_StatData       = &sq;
    rc = IExec->DoIO((struct IORequest *)req);
    D("S2_DEVICEQUERY: rc=%ld err=%ld MTU=%ld BPS=%ld HW=%ld AddrSz=%ld\n",
      (long)rc, (long)req->ios2_Req.io_Error,
      (long)sq.MTU, (long)sq.BPS, (long)sq.HardwareType,
      (long)sq.AddrFieldSize);

    /* 3. S2_CONFIGINTERFACE — sets is_configured + is_online */
    req->ios2_Req.io_Command = S2_CONFIGINTERFACE;
    req->ios2_Req.io_Error   = 0;
    for (int i = 0; i < 6; i++) req->ios2_SrcAddr[i] = 0;
    rc = IExec->DoIO((struct IORequest *)req);
    D("S2_CONFIGINTERFACE: rc=%ld err=%ld wire=0x%lx  "
      "DstMAC=%02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
      (long)rc, (long)req->ios2_Req.io_Error,
      (unsigned long)req->ios2_WireError,
      (long)req->ios2_DstAddr[0], (long)req->ios2_DstAddr[1],
      (long)req->ios2_DstAddr[2], (long)req->ios2_DstAddr[3],
      (long)req->ios2_DstAddr[4], (long)req->ios2_DstAddr[5]);

    /* 4. S2_ONLINE */
    req->ios2_Req.io_Command = S2_ONLINE;
    req->ios2_Req.io_Error   = 0;
    rc = IExec->DoIO((struct IORequest *)req);
    D("S2_ONLINE: rc=%ld err=%ld\n", (long)rc, (long)req->ios2_Req.io_Error);

    /* 5. S2_GETGLOBALSTATS */
    struct Sana2DeviceStats st;
    for (unsigned i = 0; i < sizeof(st); i++) ((UBYTE *)&st)[i] = 0;
    req->ios2_Req.io_Command = S2_GETGLOBALSTATS;
    req->ios2_Req.io_Error   = 0;
    req->ios2_StatData       = &st;
    rc = IExec->DoIO((struct IORequest *)req);
    D("S2_GETGLOBALSTATS: rc=%ld err=%ld TX=%ld RX=%ld Rcfg=%ld\n",
      (long)rc, (long)req->ios2_Req.io_Error,
      (long)st.PacketsSent, (long)st.PacketsReceived,
      (long)st.Reconfigurations);

    /* 6. S2_OFFLINE */
    req->ios2_Req.io_Command = S2_OFFLINE;
    req->ios2_Req.io_Error   = 0;
    rc = IExec->DoIO((struct IORequest *)req);
    D("S2_OFFLINE: rc=%ld err=%ld\n", (long)rc, (long)req->ios2_Req.io_Error);

    IExec->CloseDevice((struct IORequest *)req);
out:
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);
    D("DONE\n");
    return 0;
}
