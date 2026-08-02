/*
 * testudpsend — bind UDP socket to rtl8139re's IP (192.168.102.15) so
 * outbound traffic MUST route via our interface. Send a UDP packet to
 * 192.168.102.2 (n3's SLIRP gateway) — if our driver transmits, we'll
 * see input packets increment via IFConfig / txtrace.
 *
 * Uses bsdsocket.library directly (Roadshow layer beneath).
 */
#include <exec/errors.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <proto/bsdsocket.h>

/* bsdsocket.library interface — auto-opened via -lauto. */
struct Library *SocketBase = NULL;

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    SocketBase = IExec->OpenLibrary("bsdsocket.library", 4);
    if (!SocketBase) {
        IDOS->Printf("bsdsocket.library not available\n");
        return 20;
    }

    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    IDOS->Printf("socket: fd=%ld errno=%ld\n", (long)s, (long)errno);
    if (s < 0) goto out;

    /* Bind to rtl8139re's IP so outbound routes via our interface. */
    struct sockaddr_in local;
    for (unsigned i = 0; i < sizeof(local); i++) ((UBYTE *)&local)[i] = 0;
    local.sin_family = AF_INET;
    local.sin_port   = 0;   /* ephemeral */
    /* 192.168.102.15 in network byte order */
    ULONG ip = (192UL << 24) | (168UL << 16) | (102UL << 8) | 15UL;
    local.sin_addr.s_addr = ip;   /* stored in network order */
    int br = bind(s, (struct sockaddr *)&local, sizeof(local));
    IDOS->Printf("bind to 192.168.102.15:0 = %ld errno=%ld\n",
                 (long)br, (long)errno);

    /* Send to 192.168.102.2:9 (discard port) */
    struct sockaddr_in dst;
    for (unsigned i = 0; i < sizeof(dst); i++) ((UBYTE *)&dst)[i] = 0;
    dst.sin_family = AF_INET;
    dst.sin_port   = 9 << 8;   /* port 9 in network order (low byte 0) */
    ULONG dstip = (192UL << 24) | (168UL << 16) | (102UL << 8) | 2UL;
    dst.sin_addr.s_addr = dstip;

    char *msg = "hello via rtl8139re\n";
    int sr = sendto(s, msg, 20, 0, (struct sockaddr *)&dst, sizeof(dst));
    IDOS->Printf("sendto 192.168.102.2:9 = %ld errno=%ld\n",
                 (long)sr, (long)errno);

    close(s);
out:
    if (SocketBase) IExec->CloseLibrary(SocketBase);
    IDOS->Printf("DONE\n");
    return 0;
}
