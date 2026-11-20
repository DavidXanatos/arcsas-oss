/*
 * src_comm.c - AAC comm area bring-up (TYPE2 / init rev 7).
 *
 * SrcCommAlloc:  allocate the DMA-coherent comm area (StorPortGetUncachedExtension)
 *                and carve it into comm header/queues, adapter FIB pool, host RRQ,
 *                and the AAC_INIT struct. Called from HwFindAdapter (needs ConfigInfo).
 * SrcCommStart:  fill the AAC_INIT r7 struct and hand its physical base to the firmware
 *                via the INIT_STRUCT_BASE_ADDRESS sync command. Called from HwInitialize.
 *
 * Spec: docs/comm-init-protocol.md. The 32-bit init-struct fields require the comm area
 * to be below 4 GB; we check and report if it isn't.
 */
#include "srcraid.h"

#define SRC_PAGE            0x1000
#define SRC_ALIGN_UP(v,a)   (((v) + ((a)-1)) & ~((ULONG)(a)-1))
#define SRC_BRINGUP_FIBS    512           /* host FIB count we provision (<= outstanding) */

ULONG
SrcCommAlloc(
    _In_ PDEVICE_EXTENSION dev,
    _In_ PPORT_CONFIGURATION_INFORMATION ConfigInfo
    )
{
    ULONG hdrSize    = NUMBER_OF_COMM_QUEUES * 2 * sizeof(ULONG);   /* 64  */
    ULONG queuesSize = TOTAL_QUEUE_ENTRIES * sizeof(AAC_QENTRY);     /* 8448 */
    ULONG rA, rB, rC, rD, rE, total, off, basePhys, lenRet;
    STOR_PHYSICAL_ADDRESS pa;

    dev->NumFibs         = SRC_BRINGUP_FIBS;
    dev->AdapterFibsSize = dev->NumFibs * SRC_FIB_SIZE;
    dev->HostRrqEntries  = dev->NumFibs;

    rA = SRC_ALIGN_UP(hdrSize + queuesSize, SRC_PAGE);
    rB = SRC_ALIGN_UP(dev->AdapterFibsSize, SRC_PAGE);
    rC = SRC_ALIGN_UP(dev->HostRrqEntries * sizeof(ULONG), SRC_PAGE);
    rD = SRC_ALIGN_UP(sizeof(AAC_INIT_R7), SRC_PAGE);
    rE = SRC_ALIGN_UP(SRC_RAWBOUNCE_COUNT * SRC_RAWBOUNCE_SIZE, SRC_PAGE);  /* raw-SRB >1KB pool */
    total = rA + rB + rC + rD + rE;

    dev->CommArea = (PUCHAR)StorPortGetUncachedExtension(dev, ConfigInfo, total);
    if (dev->CommArea == NULL) {
        SRCDBG("srcraid: StorPortGetUncachedExtension(0x%x) FAILED\n", total);
        return SRC_BRINGUP_NOMEM;
    }
    dev->CommAreaLength = total;

    pa = StorPortGetPhysicalAddress(dev, NULL, dev->CommArea, &lenRet);
    dev->CommAreaPhys = pa;
    if (pa.HighPart != 0) {
        SRCDBG("srcraid: comm area phys above 4GB (0x%x:%08x) - 32-bit init fields "
               "can't address it\n", pa.HighPart, pa.LowPart);
        return SRC_BRINGUP_ADDR;
    }
    basePhys = pa.LowPart;

    RtlZeroMemory(dev->CommArea, total);

    off = 0;
    dev->CommHdr        = dev->CommArea + off;  dev->CommHdrPhys     = basePhys + off;  off += rA;
    dev->AdapterFibs    = dev->CommArea + off;  dev->AdapterFibsPhys = basePhys + off;  off += rB;
    dev->HostRrq = (PULONG)(dev->CommArea + off); dev->HostRrqPhys   = basePhys + off;  off += rC;
    dev->Init = (PAAC_INIT_R7)(dev->CommArea + off); dev->InitPhys    = basePhys + off; off += rD;
    dev->RawBounce = dev->CommArea + off;  dev->RawBouncePhys = basePhys + off;  off += rE;
    dev->RawBounceBusy = 0;
    /* safe INTx default (single region == whole ring); the real vector split is set at the
     * init-struct build below, once dev->MsiEnabled is known. */
    dev->NumVectors = 1;
    dev->VectorCap  = dev->HostRrqEntries;
    dev->DrainVec   = -1;   /* drain all until an ISR narrows it (0 would mean "region 0 only") */
    RtlZeroMemory(dev->HostRrqIdx, sizeof(dev->HostRrqIdx));

    SRCDBG("srcraid: commArea va=%p pa=0x%08x len=0x%x | hdr=0x%08x fibs=0x%08x(sz=0x%x) "
           "rrq=0x%08x init=0x%08x\n",
           dev->CommArea, basePhys, total, dev->CommHdrPhys, dev->AdapterFibsPhys,
           dev->AdapterFibsSize, dev->HostRrqPhys, dev->InitPhys);
    return SRC_BRINGUP_OK;
}

ULONG
SrcElapsedSeconds(_In_ PDEVICE_EXTENSION dev)
{
    LARGE_INTEGER t;
    UNREFERENCED_PARAMETER(dev);
    StorPortQuerySystemTime(&t);                 /* 100 ns since 1601 */
    return (ULONG)((t.QuadPart / 10000000ULL) - 11644473600ULL);  /* -> secs since 1970 */
}

ULONG
SrcCommStart(
    _In_ PDEVICE_EXTENSION dev
    )
{
    PAAC_INIT_R7 init = dev->Init;
    ULONG rc, st = 0;

    if (init == NULL) {
        return SRC_BRINGUP_NOMEM;
    }

    /* Vector config (must be set BEFORE pushing the init struct). INTx => 1 vector (one region
     * == the whole ring). MSI-X => N vectors (vendor uses SystemProcessorNumber; dev->NumMsix
     * holds our capped count), and the firmware splits the host RRQ into N regions of
     * HostRrqEntries/N each - see src_io.c SrcIoDrainRrq. */
    dev->NumVectors = dev->MsiEnabled ? dev->NumMsix : 1;
    if (dev->NumVectors < 1) dev->NumVectors = 1;
    if (dev->NumVectors > SRC_MAX_VECTORS) dev->NumVectors = SRC_MAX_VECTORS;
    /* keep regions evenly divisible: shrink N until it divides HostRrqEntries (512 => 1,2,4,8,16 ok) */
    while (dev->NumVectors > 1 && (dev->HostRrqEntries % dev->NumVectors) != 0) {
        dev->NumVectors--;
    }
    dev->VectorCap = dev->HostRrqEntries / dev->NumVectors;

    RtlZeroMemory(init, sizeof(*init));
    init->init_struct_revision        = ADAPTER_INIT_STRUCT_REVISION_7;
    init->no_of_msix_vectors          = dev->NumVectors;   /* 1 = INTx; N = MSI-X */
    init->fsrev                       = 0;
    init->comm_header_address         = dev->CommHdrPhys;
    init->fast_io_comm_area_address   = 0;
    init->adapter_fibs_physical_address = dev->AdapterFibsPhys;
    init->adapter_fibs_virtual_address  = 0;
    init->adapter_fibs_size           = dev->AdapterFibsSize;
    init->adapter_fib_align           = SRC_FIB_SIZE;
    init->printfbuf                   = 0;
    init->printfbufsiz                = 0;
    init->host_phys_mem_pages         = 0x000fffff;        /* cap (aacraid AAC_MAX_HOSTPHYSMEMPAGES) */
    init->host_elapsed_seconds        = SrcElapsedSeconds(dev);
    dev->LastFwTimeSync               = init->host_elapsed_seconds;  /* FW just got the time; re-sync in ~15min */
    init->init_flags                  = INITFLAGS_NEW_COMM_SUPPORTED |
                                        INITFLAGS_NEW_COMM_TYPE2_SUPPORTED |
                                        INITFLAGS_FAST_JBOD_SUPPORTED |
                                        INITFLAGS_DRIVER_USES_UTC_TIME |
                                        INITFLAGS_DRIVER_SUPPORTS_PM;
    init->max_io_commands             = dev->NumFibs;
    init->max_io_size                 = 256 * 1024;        /* 256 KB (maxSectors 512 << 9) */
    init->max_fib_size                = SRC_FIB_SIZE;
    init->max_num_aif                 = dev->MaxNumAif ? dev->MaxNumAif : 32;
    init->host_rrq_addr_low           = (ULONG)dev->HostRrqPhys;
    init->host_rrq_addr_high          = (ULONG)(dev->HostRrqPhys >> 32);

    /* reset each per-vector region's consumer index to its region base [v*VectorCap] */
    {
        ULONG v;
        for (v = 0; v < SRC_MAX_VECTORS; v++) {
            dev->HostRrqIdx[v] = (v < dev->NumVectors) ? (v * dev->VectorCap) : 0;
        }
    }
    SRCDBG("srcraid: vectors=%u cap=%u (MsiEnabled=%u NumMsix=%u) no_of_msix=%u\n",
           dev->NumVectors, dev->VectorCap, dev->MsiEnabled, dev->NumMsix, init->no_of_msix_vectors);

    /* hand the init struct's 32-bit physical base to the firmware (r7: p1 only) */
    rc = SrcSyncCommand(dev, INIT_STRUCT_BASE_ADDRESS,
                        dev->InitPhys, 0, 0, 0, 0, 0,
                        &st, NULL, NULL, NULL, NULL, 1000);
    SRCDBG("srcraid: INIT_STRUCT_BASE_ADDRESS rc=%u status=0x%08x -> OMR0=0x%08x\n",
           rc, st, SrcFwStatus(dev));

    if (rc != SRC_SYNC_OK) {
        return SRC_BRINGUP_INITFAIL;
    }
    dev->AdapterState = SRC_STATE_RUNNING;
    return SRC_BRINGUP_OK;
}
