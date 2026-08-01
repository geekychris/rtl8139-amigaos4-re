#include <devices/sana2.h>
#include <stddef.h>
#include <proto/exec.h>
#include <proto/dos.h>
int main() {
    IDOS->Printf("sizeof(IOSana2Req) = %ld\n", (long)sizeof(struct IOSana2Req));
    IDOS->Printf("offset ios2_Req       = %ld\n", (long)offsetof(struct IOSana2Req, ios2_Req));
    IDOS->Printf("offset ios2_WireError = %ld\n", (long)offsetof(struct IOSana2Req, ios2_WireError));
    IDOS->Printf("offset ios2_PacketType= %ld\n", (long)offsetof(struct IOSana2Req, ios2_PacketType));
    IDOS->Printf("offset ios2_SrcAddr   = %ld\n", (long)offsetof(struct IOSana2Req, ios2_SrcAddr));
    IDOS->Printf("offset ios2_DstAddr   = %ld\n", (long)offsetof(struct IOSana2Req, ios2_DstAddr));
    IDOS->Printf("offset ios2_DataLength= %ld\n", (long)offsetof(struct IOSana2Req, ios2_DataLength));
    return 0;
}
