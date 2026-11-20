/* srctrace.exe - read srcraid's in-memory mgmt-FIB trace ring via the "SRCDBG" IOCTL.
 * Usage: srctrace reset   (clear the ring)
 *        srctrace dump    (print the ring; default)
 * Scans \\.\Scsi0..15:, finds the srcraid controller (magic 'SRCT' in the reply). */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#pragma pack(push,1)
typedef struct { ULONG HeaderLength; UCHAR Signature[8]; ULONG Timeout;
                 ULONG ControlCode; ULONG ReturnCode; ULONG Length; } SRB_IO_CONTROL;
typedef struct { ULONG kind, a, b, c, d0, d1, d2, d3; } ENT;
#pragma pack(pop)

#define IOCTL_SCSI_MINIPORT 0x0004D008
#define BUFSZ (sizeof(SRB_IO_CONTROL) + 16 + 1024 * sizeof(ENT))

static const char* kindname(ULONG k) {
    switch (k) { case 1: return "MGMT>>"; case 2: return "MGMT<<";
                 case 3: return "RAWSRB"; case 5: return "AIF";
                 case 6: return "PHYS>>"; case 7: return "PHYS<<";
                 case 10: return "CISS-F"; case 11: return "CISS-R";
                 case 12: return "CTUID "; case 13: return "FLUSH "; case 20: return "MGMTIN";
                 default: return "?"; }
}

int main(int argc, char** argv) {
    ULONG code = (argc > 1 && _stricmp(argv[1], "reset") == 0) ? 0 : 1;
    char* buf = (char*)malloc(BUFSZ);
    int n, found = 0;
    for (n = 0; n < 16 && !found; n++) {
        char path[32]; HANDLE h; DWORD ret = 0; BOOL ok;
        SRB_IO_CONTROL* ioc = (SRB_IO_CONTROL*)buf;
        sprintf(path, "\\\\.\\Scsi%d:", n);
        h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        memset(buf, 0, BUFSZ);
        ioc->HeaderLength = sizeof(SRB_IO_CONTROL);
        memcpy(ioc->Signature, "SRCDBG\0\0", 8);
        ioc->Timeout = 10;
        ioc->ControlCode = code;
        ioc->Length = BUFSZ - sizeof(SRB_IO_CONTROL);
        ok = DeviceIoControl(h, IOCTL_SCSI_MINIPORT, buf, BUFSZ, buf, BUFSZ, &ret, NULL);
        if (ok) {
            ULONG* hdr = (ULONG*)(buf + sizeof(SRB_IO_CONTROL));
            if (code == 0) {
                /* reset: can't verify magic (no dump), but srcraid accepted it. Try next too. */
                printf("Scsi%d: reset sent (rc=%u)\n", n, ioc->ReturnCode);
                /* keep going: only srcraid actually reset; harmless to others */
            } else if (hdr[0] == 0x54435253u) {   /* 'SRCT' */
                ULONG total = hdr[1], ciss = hdr[2], irq = hdr[3], i, navail;
                ENT* e = (ENT*)(buf + sizeof(SRB_IO_CONTROL) + 16);
                navail = total < 1024 ? total : 1024;
                printf("=== Scsi%d srcraid trace: total=%u shown=%u IRQ=%s optHi=0x%02x V8=%s [MSIXdiag maxMsgId=%u isrFires=%u] CISS avail=%u mgmt=(%u,%u,%u) ===\n",
                       n, total, navail, (irq & 1) ? "MSI-X" : "INTx",
                       (irq>>24)&0xff, ((irq & 0x40000000u) ? "YES" : "no"),
                       (irq>>8)&0xff, (irq>>16)&0xff,
                       ciss & 1, (ciss>>8)&0xff, (ciss>>16)&0xff, (ciss>>24)&0xff);
                for (i = 0; i < navail; i++)
                    printf("%4u %s cmd/a=%-5u sz/b=%-5u st/c=0x%08x d=%08x %08x %08x %08x\n",
                           i, kindname(e[i].kind), e[i].a, e[i].b, e[i].c,
                           e[i].d0, e[i].d1, e[i].d2, e[i].d3);
                found = 1;
            }
        }
        CloseHandle(h);
    }
    if (code == 1 && !found) { printf("srcraid trace device not found\n"); return 1; }
    return 0;
}
