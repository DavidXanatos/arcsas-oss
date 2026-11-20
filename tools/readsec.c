/* readsec.exe N - read sector 0 of \\.\PhysicalDriveN and report signature + nonzero count.
 * Tests srcraid's physical pass-through READ path (does it return the disk's real data?). */
#include <windows.h>
#include <stdio.h>

int main(int argc, char** argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 2;
    char path[64];
    unsigned char buf[512];
    DWORD got = 0;
    HANDLE h;
    int i, nz = 0;
    sprintf(path, "\\\\.\\PhysicalDrive%d", n);
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { printf("open %s failed err=%lu\n", path, GetLastError()); return 1; }
    if (!ReadFile(h, buf, 512, &got, NULL)) { printf("read failed err=%lu\n", GetLastError()); CloseHandle(h); return 1; }
    CloseHandle(h);
    for (i = 0; i < 512; i++) if (buf[i]) nz++;
    printf("PhysicalDrive%d: read=%lu bytes  sig[510,511]=%02X%02X  nonzero=%d/512\n",
           n, got, buf[510], buf[511], nz);
    printf("first32:  ");  for (i = 0; i < 32; i++) printf("%02X ", buf[i]);  printf("\n");
    printf("mbr[440..463]: "); for (i = 440; i < 464; i++) printf("%02X ", buf[i]); printf("\n");
    /* GPT protective MBR / EFI PART check at LBA1 would need another read; sig 55AA => MBR present */
    return 0;
}
