/*
 * src_io.c - asynchronous READ/WRITE path (Step B).
 *
 * Translate a SCSI READ/WRITE SRB into an aac_read64/aac_write64 FIB
 * (FIBCMD_ContainerCommand64), build a 64-bit scatter-gather list from the
 * SRB's buffer, and submit it to the SRC inbound queue with a per-request
 * slot handle. Completion arrives asynchronously: the firmware posts the slot
 * handle into the host RRQ ring and raises the outbound (response) interrupt;
 * SrcInterrupt -> SrcIoDrainRrq walks the ring, maps handle -> SRB, and
 * completes it.
 *
 * Slot handle scheme (verified during enumeration): submit Handle=(slot<<2)+1;
 * on completion slot = (rrq_entry & 0xffff) >> 2. A set bit 30 in the entry is
 * a "fast response" (status-only success); otherwise the reply status lives in
 * the FIB. Slots 1..SRC_NUM_IO_SLOTS-1 are used (0 is the sync/enum FIB).
 */
#include "srcraid.h"

/* rate-limited bring-up diagnostics (first few of each event) */
static LONG g_dbgSubmit = 0;
static LONG g_dbgIsr    = 0;
static LONG g_dbgDone   = 0;
#define DBG_LIMIT 12
/* mgmt FIB tracing now uses the in-memory ring (SrcTrace) read out via the "SRCDBG" IOCTL -
 * NOT DbgPrint (DbgPrint in the mgmt path crashes the multi-threaded maxView agent). */

/* ---- slot allocation (lock-free; StartIo allocs, ISR frees) ---- */

static ULONG
SrcIoAllocSlot(_In_ PDEVICE_EXTENSION dev, _In_ PSCSI_REQUEST_BLOCK Srb)
{
    ULONG start = (ULONG)dev->NextSlotHint;
    ULONG i, slot;

    for (i = SRC_IO_SLOT_MIN; i < SRC_NUM_IO_SLOTS; i++) {
        slot = SRC_IO_SLOT_MIN + ((start + i) % (SRC_NUM_IO_SLOTS - SRC_IO_SLOT_MIN));
        if (slot >= SRC_RSVD_SLOT_FIRST && slot <= SRC_RSVD_SLOT_LAST) {
            continue;   /* reserved band (100..104): AIF FIB + the fixed sync/scratch
                         * slots addressed by hard-coded index. Handing one of these to
                         * a dynamic I/O lets the two paths stomp the same FIB buffer and
                         * misroute completions (slot 103 -> SrcAifOnComplete, never the
                         * SRB) -> corruption/leak -> bugcheck. See SRC_RSVD_SLOT_*. */
        }
        if (InterlockedCompareExchangePointer(&dev->IoSlotSrb[slot], Srb, NULL) == NULL) {
            dev->NextSlotHint = (LONG)(slot + 1);
            return slot;
        }
    }
    return 0;   /* none free */
}

static PSCSI_REQUEST_BLOCK
SrcIoFreeSlot(_In_ PDEVICE_EXTENSION dev, _In_ ULONG slot)
{
    if (slot < SRC_IO_SLOT_MIN || slot >= SRC_NUM_IO_SLOTS) {
        return NULL;
    }
    return (PSCSI_REQUEST_BLOCK)InterlockedExchangePointer(&dev->IoSlotSrb[slot], NULL);
}

/* Build a fixed-format CHECK CONDITION with the given sense key + ASC, mirroring the vendor
 * (which synthesizes these so the Windows disk class driver classifies the failure - retry
 * policy, event log, chkdsk) instead of a bare SRB_STATUS_ERROR with no sense. Used for a RAID
 * media error (H2: key 3 MEDIUM ERROR / ASC 0x11) and an out-of-range LBA (M4: key 5 ILLEGAL
 * REQUEST / ASC 0x21). */
static VOID
SrcIoSetSense(_In_ PSCSI_REQUEST_BLOCK srb, _In_ UCHAR senseKey, _In_ UCHAR asc)
{
    srb->DataTransferLength = 0;
    srb->ScsiStatus = 0x02;                     /* CHECK CONDITION */
    srb->SrbStatus  = SRB_STATUS_ERROR;
    if (!(srb->SrbFlags & 0x00000020u) &&       /* SRB_FLAGS_DISABLE_AUTOSENSE */
        srb->SenseInfoBuffer != NULL && srb->SenseInfoBufferLength >= 14) {
        PUCHAR sns = (PUCHAR)srb->SenseInfoBuffer;
        RtlZeroMemory(sns, srb->SenseInfoBufferLength);
        sns[0]  = 0x70;                         /* current error, fixed format */
        sns[2]  = senseKey;
        sns[7]  = 0x0A;                         /* additional sense length */
        sns[12] = asc;
        srb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
    }
}

/*
 * AAC management-FIB passthrough (0x42260 / ACI2 0x42007), delivered ASYNC exactly
 * like the vendor driver: the user payload (at `p`, length
 * plSize) IS an AAC FIB. We copy it into a freshly-allocated I/O slot, PRESERVE the
 * caller's XferState and OR 0x30A0 (the vendor does `XferState |= 0x30A0`), set
 * StructType=5 (Support64BitFIB), Size+=header, submit on the inbound queue, and
 * return TRUE = the SRB is now PENDING. SrcIoDrainRrq (the ISR) later copies the whole
 * response FIB back to the user buffer and completes the SRB - NO busy-wait (the old
 * synchronous mailbox/RRQ-poll path raced the ISR and intermittently mis-delivered the
 * response, crashing maxView). Returns FALSE if it could not submit (no free slot).
 */
static ULONG SrcFibHandle(_In_ PDEVICE_EXTENSION dev, _In_ ULONG slot);  /* defined below */

BOOLEAN
SrcMgmtSubmit(_In_ PDEVICE_EXTENSION dev, _In_ PSCSI_REQUEST_BLOCK Srb,
              _In_ PUCHAR p, _In_ ULONG plSize)
{
    PAAC_FIBHDR ufib = (PAAC_FIBHDR)p;
    PAAC_FIBHDR afib;
    PSRB_EXTENSION ext;
    ULONG copyLen = plSize, slot, fibPhys, fibSize, fibSize128, iqLow;

    if (dev->AdapterFibs == NULL || plSize < sizeof(AAC_FIBHDR)) {
        return FALSE;
    }
    if (copyLen > SRC_FIB_SIZE) copyLen = SRC_FIB_SIZE;

    slot = SrcIoAllocSlot(dev, Srb);
    if (slot == 0) {
        return FALSE;                       /* no free slot; caller completes BUSY */
    }
    ext = (PSRB_EXTENSION)Srb->SrbExtension;
    if (ext != NULL) { ext->Slot = slot; ext->Kind = SRC_FIBK_MGMT; }

    afib    = (PAAC_FIBHDR)(dev->AdapterFibs + (SIZE_T)slot * SRC_FIB_SIZE);
    fibPhys = dev->AdapterFibsPhys + slot * SRC_FIB_SIZE;

    RtlZeroMemory(afib, SRC_FIB_SIZE);
    RtlCopyMemory(afib, ufib, copyLen);                 /* whole request FIB in */
    afib->Size             = (USHORT)(ufib->Size + sizeof(AAC_FIBHDR));
    /*
     * mgmt-FIB inbound-queue delivery. Three empirically-bracketed variants (the ONLY
     * two FIB-header fields that differ between them; everything else identical):
     *   mode 0 (pre-114): XferState=ufib|0x30A0, StructType=5  -> maxView ENUMERATES, but WEDGES
     *   mode 1 (0.1.0.114): XferState=0x830ad,   StructType=4  -> NO wedge, but maxView sees NO controller
     *   mode 2 (0.1.0.115): XferState=ufib|0x30A0, StructType=4  -> VENDOR-FAITHFUL candidate
     * Derived from the SRC interface: for NewCommInterfaceType>=2, non-SmartHBA, the vendor
     * ORs 0x30A0 into XferState (preserving the caller's bits -> fw FULLY processes the mgmt
     * command, filling the object graph maxView needs) and uses StructType=4 (5 only if the
     * device's Support64BitFIB cap is set), posting to the SAME inbound queue as block I/O
     * (DeviceBase0+0xC0). So 0x30A0 is NOT the wedge cause (the vendor uses it on the IQ without
     * wedging); mode 1 (114) accidentally fixed the wedge by switching StructType 5->4 but ALSO
     * dropped 0x30A0, which killed enumeration. mode 2 keeps 0x30A0 (enum) AND StructType=4
     * (the no-wedge value) = the overlap of both proven-good properties. */
#ifndef SRCRAID_MGMT_IQ_MODE
#define SRCRAID_MGMT_IQ_MODE 2
#endif
#if SRCRAID_MGMT_IQ_MODE == 1
    afib->XferState        = XS_SEND_SYNC | XS_Async;        /* 0x830ad */
    afib->StructType       = FIB_MAGIC2;                     /* 4 */
#elif SRCRAID_MGMT_IQ_MODE == 0
    afib->XferState        = ufib->XferState | 0x000030A0u;  /* vendor process bits */
    afib->StructType       = FIB_MAGIC2_64;                  /* 5 */
#else /* mode 2 - vendor-faithful: process bits + TYPE2 (non-64) */
    afib->XferState        = ufib->XferState | 0x000030A0u;  /* vendor driver: |= 0x30A0 */
    afib->StructType       = FIB_MAGIC2;                     /* 4 - vendor default StructType */
#endif
    afib->SenderSize       = (USHORT)SRC_FIB_SIZE;
    afib->SenderFibAddress = fibPhys;
    afib->ReceiverFibAddress = 0;                       /* FIB phys high (comm area < 4GB) */
    afib->Handle           = SrcFibHandle(dev, slot);  /* low16=slot, high16=completion vector */
    KeMemoryBarrier();

    fibSize = (ULONG)afib->Size;
    if (fibSize > SRC_FIB_SIZE)       fibSize = SRC_FIB_SIZE;
    if (fibSize < sizeof(AAC_FIBHDR)) fibSize = copyLen;
    fibSize128 = (fibSize + 127) / 128 - 1;
    if (fibSize128 > 31) fibSize128 = 31;
    iqLow = (fibPhys & ~0x7Fu) | fibSize128;

    SrcWrite32(dev, SRC_REG_IQ_H, 0);
    SrcWrite32(dev, SRC_REG_IQ_L, iqLow);
    {   /* DIAGNOSTIC in-memory trace: mgmt FIB request (kind 1). */
        PULONG rq = (PULONG)((PUCHAR)ufib + sizeof(AAC_FIBHDR));
        SrcTrace(dev, 1, (ULONG)ufib->Command, (ULONG)ufib->Size, slot,
                 rq[0], rq[1], rq[2], rq[3]);
    }
    return TRUE;                            /* pending; ISR completes it */
}

/* Build the FIB handle (== the vendor's SenderData field, same offset 24): low 16 bits are our
 * global slot id ((slot<<2)+1, decoded back in the ISR). The high 16 bits COULD carry a completion
 * vector (CPU % NumVectors) to steer the completion to a per-vector RRQ region + MSI-X message, as
 * the vendor does on V8 firmware (the SenderData field = (vector<<16) | (fib+1)). This Series 7
 * card is non-V8 and delivers all completions on vector 0, so we leave the
 * high 16 bits ZERO and drain all regions on vector 0 (see SrcFibHandle + the SRC_PERVEC_DRAIN notes
 * below). The RRQ decode masks the low 16 bits, so the slot id is unchanged either way. */
static ULONG
SrcFibHandle(_In_ PDEVICE_EXTENSION dev, _In_ ULONG slot)
{
    ULONG h = (slot << 2) + 1;
    /* Completion handle = (slot<<2)+1, NO per-vector routing. This Series 7 firmware is non-V8
     * (SupportedOptions bit 0x40000000 clear) and delivers EVERY completion on MSI-X vector 0
     * regardless of the FIB Handle's high16. Stamping the submitting CPU's vector there (the vendor's
     * SenderData trick) only works on V8 firmware with true per-vector completion queues;
     * on this card it funnels completions to vectors the host never gets an interrupt for -> hang
     * (proven: 0.1.0.205 hung boot; a route probe on hardware confirmed the firmware ignores the
     * vector). The ISR drains all per-vector RRQ regions on vector 0 - exactly what the vendor also
     * does on non-V8 hardware. Per-vector routing is deferred to real V8 hardware (none available). */
    UNREFERENCED_PARAMETER(dev);
    return h;
}

/* Raw-SRB >1KB bounce pool (lock-free bitmap). A maxView passthrough whose data exceeds the
 * per-slot FIB tail (SRC_RAWIOCTL_FIBTAIL = 1KB) borrows one of SRC_RAWBOUNCE_COUNT dedicated
 * DMA buffers so it stays ASYNC instead of falling to the DISPATCH sync mailbox. Returns the
 * buffer index, or -1 if all are busy (caller then uses the sync fallback). */
static LONG
SrcRawBounceAlloc(_In_ PDEVICE_EXTENSION dev)
{
    LONG i;
    for (i = 0; i < SRC_RAWBOUNCE_COUNT; i++) {
        for (;;) {
            LONG old = dev->RawBounceBusy;
            LONG bit = 1L << i;
            if (old & bit) {
                break;                      /* busy - try the next buffer */
            }
            if (InterlockedCompareExchange(&dev->RawBounceBusy, old | bit, old) == old) {
                return i;                   /* claimed */
            }
            /* lost the CAS to a concurrent alloc/free - re-read and retry this index */
        }
    }
    return -1;                              /* pool exhausted */
}
static VOID
SrcRawBounceFree(_In_ PDEVICE_EXTENSION dev, _In_ LONG idx)
{
    if (idx >= 0 && idx < SRC_RAWBOUNCE_COUNT) {
        InterlockedAnd(&dev->RawBounceBusy, ~(1L << idx));
    }
}

/*
 * 0x42264 / 0x4201B maxView raw-SRB passthrough (INQUIRY / VPD / ATA PASS-THROUGH for SMART),
 * submitted ASYNC on an I/O slot exactly like the vendor driver (and like SrcPhysExecute) -
 * NOT the DISPATCH-spinning sync mailbox that masked outbound interrupts and held MgmtBusy for
 * up to its timeout (which stalled block-I/O completions when a slow drive was probed under load).
 * The device data DMAs into a per-slot bounce region inside the slot's own 2048-byte FIB buffer
 * (SRC_RAWIOCTL_DATAOFF) - no shared scratch, no MgmtBusy, each request independent - and the ISR
 * (SRC_FIBK_RAWIOCTL) copies it back to the IOCTL buffer + writes the transfer length + ReturnCode.
 * p = IOCTL payload, have = its length. TRUE = pending; FALSE = fall back (no slot, or the transfer
 * is larger than the per-slot bounce - rare for mgmt INQUIRY/SMART).
 */
BOOLEAN
SrcRawSrbSubmit(_In_ PDEVICE_EXTENSION dev, _In_ PSCSI_REQUEST_BLOCK Srb,
                _In_ PUCHAR p, _In_ ULONG have)
{
    PSRB_EXTENSION ext = (PSRB_EXTENSION)Srb->SrbExtension;
    PAAC_FIBHDR fib;
    PAAC_SRB    srb;
    PUCHAR      dataBuf;
    LONG    bounceIdx;
    ULONG   slot, fibPhys, dataPhys, fibSize, fibSize128, iqLow;
    UCHAR   chan, id, lun, cdbLen;
    ULONG   flags, dataLen, dataOff;
    BOOLEAN dataOut;

    if (have < 0x58 || dev->AdapterFibs == NULL) {
        return FALSE;
    }
    chan    = p[0x05];
    id      = p[0x06];
    lun     = p[0x07];                       /* relayed LUN (vendor passes pSrb->Lun) */
    cdbLen  = p[0x0a];
    flags   = *(PULONG)(p + 0x0c);
    dataLen = *(PULONG)(p + 0x10);
    dataOff = ((ULONG)(*(PUSHORT)(p + 0x00)) + (ULONG)p[0x0b] + 7) & ~3u;
    dataOut = (flags & AAC_SRB_DataOut) ? TRUE : FALSE;

    if (cdbLen == 0 || cdbLen > 16) cdbLen = 16;
    if (dataLen > SRC_RAWIOCTL_DATAMAX) {
        return FALSE;                        /* too big for the per-slot bounce -> caller uses sync */
    }
    if (dataLen != 0 &&
        !(dataOff >= 0x58 && (ULONGLONG)dataOff + dataLen <= (ULONGLONG)have)) {
        return FALSE;                        /* data region not inside the IOCTL buffer */
    }

    slot = SrcIoAllocSlot(dev, Srb);
    if (slot == 0) {
        return FALSE;                        /* no free slot; caller completes BUSY */
    }

    fib      = (PAAC_FIBHDR)(dev->AdapterFibs + (SIZE_T)slot * SRC_FIB_SIZE);
    fibPhys  = dev->AdapterFibsPhys + slot * SRC_FIB_SIZE;
    /* <=1KB rides the per-slot FIB tail (no pool pressure); >1KB borrows a pool buffer so it
     * stays async. Pool exhausted => free the slot and fall back to sync (rare). */
    bounceIdx = -1;
    if (dataLen > SRC_RAWIOCTL_FIBTAIL) {
        if (ext == NULL) {                   /* no SRB ext to record the pool index -> use sync */
            SrcIoFreeSlot(dev, slot);
            return FALSE;
        }
        bounceIdx = SrcRawBounceAlloc(dev);
        if (bounceIdx < 0) {
            SrcIoFreeSlot(dev, slot);        /* pool exhausted -> caller uses sync */
            return FALSE;
        }
        dataBuf  = dev->RawBounce + (SIZE_T)bounceIdx * SRC_RAWBOUNCE_SIZE;
        dataPhys = dev->RawBouncePhys + (ULONG)bounceIdx * SRC_RAWBOUNCE_SIZE;
    } else {
        dataBuf  = (PUCHAR)fib + SRC_RAWIOCTL_DATAOFF;
        dataPhys = fibPhys + SRC_RAWIOCTL_DATAOFF;
    }
    if (ext != NULL) { ext->Slot = slot; ext->Kind = SRC_FIBK_RAWIOCTL; ext->BounceIdx = bounceIdx; }

    RtlZeroMemory(fib, SRC_FIB_SIZE);
    if (dataOut && dataLen) {
        RtlCopyMemory(dataBuf, p + dataOff, dataLen);       /* host -> device data */
    }

    srb = (PAAC_SRB)((PUCHAR)fib + sizeof(AAC_FIBHDR));
    srb->function   = SRBF_ExecuteScsi;
    srb->channel    = chan;
    srb->id         = id;
    srb->lun        = lun;
    srb->timeout    = Srb->TimeOutValue;
    srb->flags      = flags;
    srb->count      = dataLen;
    srb->retryLimit = 0;
    srb->cdbSize    = cdbLen;
    RtlCopyMemory(srb->cdb, p + 0x48, cdbLen);
    if (dataLen) {
        srb->sgCount        = 1;
        srb->sg[0].addrLow  = dataPhys;
        srb->sg[0].addrHigh = 0;
        srb->sg[0].len      = dataLen;
    } else {
        srb->sgCount = 0;
    }

    fibSize = sizeof(AAC_FIBHDR) + AAC_SRB_FIXED + (dataLen ? sizeof(AAC_SGENTRY64) : 0);
    fib->XferState        = XS_SEND_SYNC | XS_Async;
    fib->Command          = (USHORT)FIBCMD_ScsiPortCommand64;   /* 601 */
    fib->StructType       = FIB_MAGIC2;                         /* 4 - IQ delivery (as SrcPhysExecute) */
    fib->Size             = (USHORT)fibSize;
    fib->SenderSize       = (USHORT)SRC_FIB_SIZE;
    fib->SenderFibAddress = fibPhys;
    fib->Handle           = SrcFibHandle(dev, slot);   /* low16=slot, high16=completion vector */
    KeMemoryBarrier();

    fibSize128 = (fibSize + 127) / 128 - 1;
    if (fibSize128 > 31) fibSize128 = 31;
    iqLow = (fibPhys & ~0x7Fu) | fibSize128;
    SrcWrite32(dev, SRC_REG_IQ_H, 0);
    SrcWrite32(dev, SRC_REG_IQ_L, iqLow);

    /* trace kind 3 (raw-SRB), c=0xA5 marks the async-submit path (vs sync ret code) */
    SrcTrace(dev, 3, ((ULONG)chan << 8) | id, p[0x48], 0xA5, dataLen, slot, lun, 0);
    return TRUE;                             /* pending; ISR (SRC_FIBK_RAWIOCTL) completes it */
}

/* ---- CDB decode ---- */

static BOOLEAN
SrcIoDecodeCdb(_In_ PCDB cdb, _In_ UCHAR cdbLen, _Out_ PULONGLONG lba, _Out_ PULONG blocks)
{
    PUCHAR b = (PUCHAR)cdb;
    UNREFERENCED_PARAMETER(cdbLen);

    switch (b[0]) {
    case SCSIOP_READ6:
    case SCSIOP_WRITE6:
        *lba    = (((ULONG)(b[1] & 0x1f)) << 16) | ((ULONG)b[2] << 8) | b[3];
        *blocks = (b[4] == 0) ? 256 : b[4];
        return TRUE;
    case SCSIOP_READ:       /* READ(10)  */
    case SCSIOP_WRITE:      /* WRITE(10) */
        *lba    = ((ULONG)b[2] << 24) | ((ULONG)b[3] << 16) | ((ULONG)b[4] << 8) | b[5];
        *blocks = ((ULONG)b[7] << 8) | b[8];
        return TRUE;
    case SCSIOP_READ12:
    case SCSIOP_WRITE12:
        *lba    = ((ULONG)b[2] << 24) | ((ULONG)b[3] << 16) | ((ULONG)b[4] << 8) | b[5];
        *blocks = ((ULONG)b[6] << 24) | ((ULONG)b[7] << 16) | ((ULONG)b[8] << 8) | b[9];
        return TRUE;
    case SCSIOP_READ16:
    case SCSIOP_WRITE16:
        *lba    = ((ULONGLONG)b[2] << 56) | ((ULONGLONG)b[3] << 48) |
                  ((ULONGLONG)b[4] << 40) | ((ULONGLONG)b[5] << 32) |
                  ((ULONGLONG)b[6] << 24) | ((ULONGLONG)b[7] << 16) |
                  ((ULONGLONG)b[8] << 8)  |  (ULONGLONG)b[9];
        *blocks = ((ULONG)b[10] << 24) | ((ULONG)b[11] << 16) | ((ULONG)b[12] << 8) | b[13];
        return TRUE;
    default:
        return FALSE;
    }
}

/* ---- submit ---- */

BOOLEAN
SrcIoReadWrite(
    _In_ PDEVICE_EXTENSION dev,
    _In_ PSCSI_REQUEST_BLOCK Srb,
    _In_ PSRC_CONTAINER c
    )
{
    PSTOR_SCATTER_GATHER_LIST sgl;
    PAAC_FIBHDR fib;
    PAAC_RAW_IO2 rio;
    PSRB_EXTENSION ext = (PSRB_EXTENSION)Srb->SrbExtension;
    ULONGLONG lba = 0;
    ULONG blocks = 0, slot, i, n, fibPhys, fibSize, fibSize128, iqLow;
    USHORT rioFlags;
    BOOLEAN isWrite, conformant;

    if (!SrcIoDecodeCdb((PCDB)Srb->Cdb, Srb->CdbLength, &lba, &blocks)) {
        SRCDBG("srcraid: IO decode FAIL op=0x%02x\n", Srb->Cdb[0]);
        return FALSE;   /* unsupported op -> caller errors it */
    }
    if (blocks == 0) {              /* nothing to transfer: succeed immediately */
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        Srb->DataTransferLength = 0;
        return FALSE;               /* let caller complete with SUCCESS */
    }
    /* M4: reject a request past end-of-container (vendor BuildRawIoFib validates endLBA and
     * returns ILLEGAL REQUEST / LBA OUT OF RANGE). Guards against an out-of-range write
     * reaching the firmware as a bare error - or worse, a data hazard if advertised capacity
     * ever disagreed with the true geometry. (lba < SizeBlocks is checked first, so the
     * subtraction can't underflow.) */
    if (c != NULL && c->SizeBlocks != 0 &&
        (lba >= c->SizeBlocks || (ULONGLONG)blocks > c->SizeBlocks - lba)) {
        SRCDBG("srcraid: IO OUT-OF-RANGE lba=%I64u blocks=%u size=%I64u op=0x%02x\n",
               lba, blocks, c->SizeBlocks, Srb->Cdb[0]);
        SrcIoSetSense(Srb, 0x05, 0x21);     /* ILLEGAL REQUEST / LBA OUT OF RANGE */
        return FALSE;                       /* caller completes with CHECK CONDITION */
    }

    sgl = StorPortGetScatterGatherList(dev, Srb);
    if (sgl == NULL || sgl->NumberOfElements == 0) {
        SRCDBG("srcraid: IO no SG list (sgl=%p)\n", sgl);
        return FALSE;
    }
    n = sgl->NumberOfElements;

    slot = SrcIoAllocSlot(dev, Srb);
    if (slot == 0) {
        Srb->SrbStatus = SRB_STATUS_BUSY;   /* throttle: StorPort retries */
        return FALSE;
    }
    if (ext != NULL) {
        ext->Slot = slot;
        ext->Kind = SRC_FIBK_RAWIO2;
    }

    isWrite = (Srb->SrbFlags & SRB_FLAGS_DATA_OUT) ? TRUE : FALSE;

    fib     = (PAAC_FIBHDR)(dev->AdapterFibs + (SIZE_T)slot * SRC_FIB_SIZE);
    fibPhys = dev->AdapterFibsPhys + slot * SRC_FIB_SIZE;
    rio     = (PAAC_RAW_IO2)((PUCHAR)fib + sizeof(AAC_FIBHDR));

    /*
     * ContainerRawIo2 (503) - the block command this firmware accepts (verified
     * against the vendor driver's behavior; VM_CtHostRead64/501 gives ST_INVAL).
     * Payload aac_raw_io2: 64-bit LBA, byteCount in BYTES, cid, RIO2 flags,
     * IEEE1212 16-byte SGEs. SGL is "conformant" when every SGE but the last is
     * the same length as the first (firmware then uses sgeFirstSize).
     */
    conformant = TRUE;
    for (i = 0; i + 1 < n; i++) {
        if (sgl->List[i].Length != sgl->List[0].Length) { conformant = FALSE; break; }
    }
    rioFlags = (USHORT)((isWrite ? RIO2_IO_TYPE_WRITE : RIO2_IO_TYPE_READ) |
                        RIO2_SG_FORMAT_IEEE1212 |
                        (conformant ? RIO2_SGL_CONFORMANT : 0));

    RtlZeroMemory(fib, sizeof(AAC_FIBHDR) + AAC_RAWIO2_FIXED);
    rio->blockLow     = (ULONG)lba;
    rio->blockHigh    = (ULONG)(lba >> 32);
    rio->byteCount    = Srb->DataTransferLength;
    rio->cid          = (USHORT)Srb->TargetId;
    rio->flags        = rioFlags;
    rio->sgeFirstSize = sgl->List[0].Length;
    /* sgeNominalSize (vendor BuildRawIo2ScatterGather:1511) = the SECOND SGE's length - the size
     * the firmware assumes for every middle element of a CONFORMANT SGL. Leaving it 0 (as before)
     * is a latent corruption risk if the FW trusts it instead of the per-SGE IEEE1212 lengths.
     * Our conformant test requires List[0..n-2] all equal List[0], so when conformant List[1]==
     * List[0]==sgeFirstSize and this is consistent; harmless when non-conformant. */
    rio->sgeNvme      = (n > 1) ? sgl->List[1].Length : 0;   /* == sgeNominalSize */
    rio->sgeCnt       = n;
    rio->resv         = 0;
    for (i = 0; i < n; i++) {
        rio->sge[i].addrLow  = sgl->List[i].PhysicalAddress.LowPart;
        rio->sge[i].addrHigh = (ULONG)sgl->List[i].PhysicalAddress.HighPart;
        rio->sge[i].length   = sgl->List[i].Length;
        rio->sge[i].flags    = 0;
    }

    fibSize = sizeof(AAC_FIBHDR) + AAC_RAWIO2_FIXED + n * sizeof(AAC_SGE_IEEE1212);

    /* Completion is handle-based via the host RRQ. XferState 0x000830ad =
     * XS_SEND_SYNC|XS_Async (matches the captured vendor I/O FIB exactly). */
    fib->XferState        = XS_SEND_SYNC | XS_Async;
    fib->Command          = FIBCMD_ContainerRawIo2;
    fib->StructType       = FIB_MAGIC2;
    fib->Size             = (USHORT)fibSize;
    fib->SenderSize       = (USHORT)SRC_FIB_SIZE;
    fib->SenderFibAddress = fibPhys;
    fib->ReceiverFibAddress = 0;
    fib->Handle           = SrcFibHandle(dev, slot);   /* low16=slot, high16=completion vector */

    KeMemoryBarrier();

    fibSize128 = (fibSize + 127) / 128 - 1;
    if (fibSize128 > 31) {
        fibSize128 = 31;
    }
    iqLow = (fibPhys & ~0x7Fu) | fibSize128;

    SrcWrite32(dev, SRC_REG_IQ_H, 0);
    SrcWrite32(dev, SRC_REG_IQ_L, iqLow);

    if (InterlockedIncrement(&g_dbgSubmit) <= DBG_LIMIT) {
        SRCDBG("srcraid: IO submit slot=%u %s cid=%u lba=0x%I64x blks=%u nsg=%u "
               "xferLen=%u flags=0x%04x sg0=%08x:%08x len=%u fibSize=%u Cmd=%u\n",
               slot, isWrite ? "WR" : "RD", (ULONG)Srb->TargetId, lba,
               blocks, n, Srb->DataTransferLength, rioFlags,
               rio->sge[0].addrHigh, rio->sge[0].addrLow, rio->sge[0].length,
               fibSize, fib->Command);
    }
    return TRUE;    /* queued; completed in the ISR */
}

/*
 * SYNCHRONIZE CACHE -> flush the controller write-back cache for this container.
 * Builds a VM_ContainerConfig / CT_FLUSH_CACHE FIB (aacraid aac_synchronize) and
 * submits it ASYNC on the inbound queue; the ISR completes the SRB on the RRQ reply.
 * This replaces the old "fake SUCCESS without flushing", which risked losing
 * acknowledged writes on power loss when write-back cache is enabled.
 */
BOOLEAN
SrcIoFlush(
    _In_ PDEVICE_EXTENSION dev,
    _In_ PSCSI_REQUEST_BLOCK Srb
    )
{
    PAAC_FIBHDR fib;
    PULONG payload;
    PSRB_EXTENSION ext = (PSRB_EXTENSION)Srb->SrbExtension;
    ULONG slot, fibPhys, fibSize, fibSize128, iqLow;

    slot = SrcIoAllocSlot(dev, Srb);
    if (slot == 0) {
        Srb->SrbStatus = SRB_STATUS_BUSY;   /* throttle: StorPort retries */
        return FALSE;
    }
    if (ext != NULL) {
        ext->Slot = slot;
        ext->Kind = SRC_FIBK_FLUSH;
    }

    fib     = (PAAC_FIBHDR)(dev->AdapterFibs + (SIZE_T)slot * SRC_FIB_SIZE);
    fibPhys = dev->AdapterFibsPhys + slot * SRC_FIB_SIZE;
    payload = (PULONG)((PUCHAR)fib + sizeof(AAC_FIBHDR));

    /* aac_synchronize: {command=VM_ContainerConfig, type=CT_FLUSH_CACHE, cid, parm1..4=0,
     * count=sizeof(reply.data)=16}. 8 dwords. */
    RtlZeroMemory(fib, sizeof(AAC_FIBHDR) + 8 * sizeof(ULONG));
    payload[0] = FIBCMD_VM_ContainerConfig;     /* 2 */
    payload[1] = CT_FLUSH_CACHE;                /* 129 */
    payload[2] = (ULONG)Srb->TargetId;          /* cid */
    payload[7] = 16;                            /* count */

    fibSize = sizeof(AAC_FIBHDR) + 8 * sizeof(ULONG);
    fib->XferState        = XS_SEND_SYNC | XS_Async;
    fib->Command          = FIBCMD_ContainerCommand;    /* 500 */
    fib->StructType       = FIB_MAGIC2;                 /* 4 - new-comm inbound delivery */
    fib->Size             = (USHORT)fibSize;
    fib->SenderSize       = (USHORT)SRC_FIB_SIZE;
    fib->SenderFibAddress = fibPhys;
    fib->ReceiverFibAddress = 0;
    fib->Handle           = SrcFibHandle(dev, slot);   /* low16=slot, high16=completion vector */

    KeMemoryBarrier();

    fibSize128 = (fibSize + 127) / 128 - 1;
    if (fibSize128 > 31) {
        fibSize128 = 31;
    }
    iqLow = (fibPhys & ~0x7Fu) | fibSize128;
    SrcWrite32(dev, SRC_REG_IQ_H, 0);
    SrcWrite32(dev, SRC_REG_IQ_L, iqLow);

    if (InterlockedIncrement(&g_dbgSubmit) <= DBG_LIMIT) {
        SRCDBG("srcraid: FLUSH submit slot=%u cid=%u\n", slot, (ULONG)Srb->TargetId);
    }
    return TRUE;    /* queued; completed in the ISR */
}

/*
 * Physical pass-through: translate any SCSI SRB for a device on SRC_PHYSICAL_BUS
 * into an aac_srb FIB (ScsiPortCommand64=601, StructType=1) and submit it async
 * via the inbound queue. The firmware runs the CDB against the physical device
 * at (channel,id,lun) and posts the aac_srb_reply via the host RRQ. Returns TRUE
 * if queued (completed later in the ISR); FALSE if the caller must complete it.
 */
BOOLEAN
SrcPhysExecute(
    _In_ PDEVICE_EXTENSION dev,
    _In_ PSCSI_REQUEST_BLOCK Srb
    )
{
    PSTOR_SCATTER_GATHER_LIST sgl = NULL;
    PAAC_FIBHDR fib;
    PAAC_SRB srb;
    PSRB_EXTENSION ext = (PSRB_EXTENSION)Srb->SrbExtension;
    UCHAR relayChannel, relayId;
    ULONG slot, i, n = 0, fibPhys, fibSize, fibSize128, iqLow, flags;
    ULONG dataLen = Srb->DataTransferLength;

#if SRC_TRANSPARENT_RELAY
    /* Transparent relay (vendor model): NO driver-side sweep. StorPort
     * probes each (bus,target,lun) and we relay straight to the firmware. PathId SRC_PHYSICAL_BUS
     * -> channel 0; id = TargetId; lun = Lun. The firmware answers INQUIRY for present devices and
     * selection-times-out absent ones, so StorPort enumerates exactly the real set (no phantoms,
     * no sync-mailbox sweep) and re-probes on BusChangeDetected. Verified: the physical disks sit
     * at (ch 0, id 19) and (ch 0, id 36), so channel 0 reaches them. */
    if (Srb->TargetId > 254) {
        Srb->SrbStatus = SRB_STATUS_SELECTION_TIMEOUT;
        return FALSE;
    }
    relayChannel = 0;
    relayId      = (UCHAR)Srb->TargetId;
#else
    if (Srb->TargetId >= dev->NumPhysicals ||
        !dev->Physicals[Srb->TargetId].Valid) {
        Srb->SrbStatus = SRB_STATUS_SELECTION_TIMEOUT;
        return FALSE;
    }
    relayChannel = dev->Physicals[Srb->TargetId].Channel;
    relayId      = dev->Physicals[Srb->TargetId].Id;
#endif
    /* Multi-LUN (vendor model): our sweep discovers each device at
     * its base (channel,id) and presents it as one target; Storport then probes additional
     * LUNs on that target (ConfigInfo.MaximumNumberOfLogicalUnits), and we relay the
     * OS-requested LUN straight to the firmware at the same (channel,id) - exactly as the
     * vendor passes pSrb->Lun through. A real multi-LUN device (e.g. an enclosure exposing
     * SES as a separate LUN) answers its extra LUNs; a single-LUN device selection-times-out
     * LUN>0 at the firmware, so no phantom. (Earlier we hard-rejected LUN!=0; that predated
     * relaying it. The blind mailbox LUN-sweep that made phantoms is still disabled -
     * SRC_SWEEP_LUNS=1 - because THAT path can't see the device's REPORT LUNS, whereas this
     * Storport-driven path does.) */

    flags = 0;
    if (Srb->SrbFlags & SRB_FLAGS_DATA_IN)  flags |= AAC_SRB_DataIn;
    if (Srb->SrbFlags & SRB_FLAGS_DATA_OUT) flags |= AAC_SRB_DataOut;

    if (dataLen != 0 && flags != 0) {
        sgl = StorPortGetScatterGatherList(dev, Srb);
        if (sgl == NULL || sgl->NumberOfElements == 0) {
            Srb->SrbStatus = SRB_STATUS_ERROR;
            return FALSE;
        }
        n = sgl->NumberOfElements;
    }

    slot = SrcIoAllocSlot(dev, Srb);
    if (slot == 0) {
        Srb->SrbStatus = SRB_STATUS_BUSY;
        return FALSE;
    }
    if (ext != NULL) {
        ext->Slot = slot;
        ext->Kind = SRC_FIBK_SRB;
    }

    fib     = (PAAC_FIBHDR)(dev->AdapterFibs + (SIZE_T)slot * SRC_FIB_SIZE);
    fibPhys = dev->AdapterFibsPhys + slot * SRC_FIB_SIZE;
    srb     = (PAAC_SRB)((PUCHAR)fib + sizeof(AAC_FIBHDR));

    RtlZeroMemory(fib, sizeof(AAC_FIBHDR) + AAC_SRB_FIXED);
    srb->function   = SRBF_ExecuteScsi;
    srb->channel    = relayChannel;
    srb->id         = relayId;
    srb->lun        = (UCHAR)Srb->Lun;      /* relay the OS LUN (vendor passes pSrb->Lun) */
    srb->timeout    = Srb->TimeOutValue;
    srb->flags      = flags;
    srb->count      = dataLen;
    srb->retryLimit = 0;
    srb->cdbSize    = Srb->CdbLength;
    RtlCopyMemory(srb->cdb, Srb->Cdb, (Srb->CdbLength > 16) ? 16 : Srb->CdbLength);
    srb->sgCount    = n;
    for (i = 0; i < n; i++) {
        srb->sg[i].addrLow  = sgl->List[i].PhysicalAddress.LowPart;
        srb->sg[i].addrHigh = (ULONG)sgl->List[i].PhysicalAddress.HighPart;
        srb->sg[i].len      = sgl->List[i].Length;
    }

    fibSize = sizeof(AAC_FIBHDR) + AAC_SRB_FIXED + n * sizeof(AAC_SGENTRY64);
    fib->XferState        = XS_SEND_SYNC | XS_Async;
    fib->Command          = (USHORT)FIBCMD_ScsiPortCommand64;   /* 601 */
    /* new-comm inbound-queue delivery requires StructType=4 (the vendor driver
     * overwrites it to 4); the mailbox/sync path uses 1. See src_phys.c. */
    fib->StructType       = FIB_MAGIC2;                         /* 4 */
    fib->Size             = (USHORT)fibSize;
    fib->SenderSize       = (USHORT)SRC_FIB_SIZE;
    fib->SenderFibAddress = fibPhys;
    fib->Handle           = SrcFibHandle(dev, slot);   /* low16=slot, high16=completion vector */

    KeMemoryBarrier();

    fibSize128 = (fibSize + 127) / 128 - 1;
    if (fibSize128 > 31) {
        fibSize128 = 31;
    }
    iqLow = (fibPhys & ~0x7Fu) | fibSize128;
    SrcWrite32(dev, SRC_REG_IQ_H, 0);
    SrcWrite32(dev, SRC_REG_IQ_L, iqLow);

    if (InterlockedIncrement(&g_dbgSubmit) <= DBG_LIMIT) {
        SRCDBG("srcraid: PHYS submit slot=%u tgt=%u ch=%u id=%u op=0x%02x n=%u "
               "xfer=%u flags=0x%x fibSize=%u\n",
               slot, (ULONG)Srb->TargetId, relayChannel, relayId, Srb->Cdb[0],
               n, dataLen, flags, fibSize);
    }
    /* DIAGNOSTIC in-memory trace: PHYS submit (kind 6). a=chan<<8|id, b=cdb0, c=dataLen. */
    SrcTrace(dev, 6, ((ULONG)relayChannel << 8) | relayId, Srb->Cdb[0], dataLen,
             flags, n, Srb->CdbLength, (ULONG)Srb->TargetId);
    return TRUE;
}

/*
 * One-shot diagnostic: read cid 0 LBA 0 (1 sector) with the SG pointing into
 * our own comm area (a scratch FIB slot) - an address the firmware provably
 * DMAs to (FIBs/RRQ live there). Polls the RRQ (call during init, interrupts
 * off). If this succeeds where StorPort-SG reads fail, the problem is the
 * StorPort SG buffer's reachability; if it also fails, the read command/mode
 * is rejected regardless of SG.
 */
VOID
SrcIoSelfTest(_In_ PDEVICE_EXTENSION dev)
{
    const ULONG slot = 1, scratchSlot = 100;
    PAAC_FIBHDR fib = (PAAC_FIBHDR)(dev->AdapterFibs + (SIZE_T)slot * SRC_FIB_SIZE);
    PAAC_RAW_IO2 rio = (PAAC_RAW_IO2)((PUCHAR)fib + sizeof(AAC_FIBHDR));
    PUCHAR scratch = dev->AdapterFibs + (SIZE_T)scratchSlot * SRC_FIB_SIZE;
    ULONG  fibPhys = dev->AdapterFibsPhys + slot * SRC_FIB_SIZE;
    ULONG  scratchPhys = dev->AdapterFibsPhys + scratchSlot * SRC_FIB_SIZE;
    volatile ULONG *rrq = (volatile ULONG *)dev->HostRrq;
    PULONG reply = (PULONG)rio, s = (PULONG)scratch;
    ULONG idx = dev->HostRrqIdx[0], i, entry = 0, fibSize, fibSize128;

    RtlZeroMemory(scratch, 512);
    RtlZeroMemory(fib, sizeof(AAC_FIBHDR) + AAC_RAWIO2_FIXED);
    rio->blockLow     = 0;
    rio->blockHigh    = 0;
    rio->byteCount    = 512;
    rio->cid          = 0;
    rio->flags        = RIO2_IO_TYPE_READ | RIO2_SG_FORMAT_IEEE1212 | RIO2_SGL_CONFORMANT;
    rio->sgeFirstSize = 512;
    rio->sgeNvme      = 0;
    rio->sgeCnt       = 1;
    rio->resv         = 0;
    rio->sge[0].addrLow  = scratchPhys;
    rio->sge[0].addrHigh = 0;
    rio->sge[0].length   = 512;
    rio->sge[0].flags    = 0;
    fibSize = sizeof(AAC_FIBHDR) + AAC_RAWIO2_FIXED + sizeof(AAC_SGE_IEEE1212);
    fib->XferState        = XS_SEND_SYNC | XS_Async;
    fib->Command          = FIBCMD_ContainerRawIo2;
    fib->StructType       = FIB_MAGIC2;
    fib->Size             = (USHORT)fibSize;
    fib->SenderSize       = (USHORT)SRC_FIB_SIZE;
    fib->SenderFibAddress = fibPhys;
    fib->Handle           = SrcFibHandle(dev, slot);   /* low16=slot, high16=completion vector */
    KeMemoryBarrier();
    fibSize128 = (fibSize + 127) / 128 - 1;
    SrcWrite32(dev, SRC_REG_IQ_H, 0);
    SrcWrite32(dev, SRC_REG_IQ_L, (fibPhys & ~0x7Fu) | fibSize128);

    for (i = 0; i < 10000 && entry == 0; i++) {
        /* scan every per-vector region for the one selftest completion (it may land in any
         * region under MSI-X; INTx has a single region == the whole ring) */
        ULONG v;
        for (v = 0; v < dev->NumVectors; v++) {
            ULONG b = v * dev->VectorCap, vi = dev->HostRrqIdx[v];
            if (vi < b || vi >= b + dev->VectorCap) vi = b;
            entry = rrq[vi] & 0x7FFFFFFFu;
            if (entry != 0) {
                rrq[vi] = 0;
                vi++; if (vi >= b + dev->VectorCap) vi = b;
                dev->HostRrqIdx[v] = vi;
                idx = vi;
                break;
            }
        }
        if (entry != 0) break;
        StorPortStallExecution(100);
    }
    SrcWrite32(dev, SRC_REG_ODR_CLEAR, SRC_ODB_RESP_SENT);
    SRCDBG("srcraid: IO SELFTEST (comm-area SG) entry=0x%08x reply[0..3]=%08x %08x %08x %08x\n",
           entry, reply[0], reply[1], reply[2], reply[3]);
    SRCDBG("srcraid: IO SELFTEST scratch[0..3]=%08x %08x %08x %08x scratch[126..127]=%08x %08x\n",
           s[0], s[1], s[2], s[3], s[126], s[127]);
}

/* ---- interrupt enable + completion drain ---- */

/*
 * Switch the firmware to MSI-X mode (IDR bit 6 + arm IOAR) WITHOUT unmasking
 * interrupts. This firmware services container block I/O only in MSI-X mode
 * (the vendor runs the card on MSI-X), so this must happen before any block I/O
 * - including the polled self-test. Interrupts stay masked until
 * SrcIoEnableInterrupts so the self-test can poll the RRQ without an ISR race.
 */
VOID
SrcIoEnableMsixMode(_In_ PDEVICE_EXTENSION dev)
{
    ULONG idr = SrcRead32(dev, SRC_REG_IDR);
    SrcWrite32(dev, SRC_REG_IDR, idr | SRC_IDR_MSIX_ENABLE);
    (void)SrcRead32(dev, SRC_REG_IDR);
    SrcWrite32(dev, SRC_REG_IOAR, PMC_ALL_INTERRUPT_BITS);
    (void)SrcRead32(dev, SRC_REG_IOAR);
    dev->MsiEnabled = TRUE;
    SRCDBG("srcraid: MSI-X mode enabled (IDR bit6, IOAR=0x%08x)\n", PMC_ALL_INTERRUPT_BITS);
}

VOID
SrcIoEnableInterrupts(_In_ PDEVICE_EXTENSION dev)
{
    /* Unmask: MSI-X if we switched the firmware to it, else INTx. StorPort/HAL
     * program the MSI-X table from the INF; the firmware routes host-RRQ
     * completions to MSI-X vector 0 -> HwInterrupt. */
    ULONG mask = dev->MsiEnabled ? AAC_INT_ENABLE_MSIX : AAC_INT_ENABLE_INTX;
    SrcWrite32(dev, SRC_REG_OIMR, mask);
    dev->Oimr = mask;
    (void)SrcRead32(dev, SRC_REG_OIMR);
    dev->InterruptsEnabled = TRUE;
}

/* Switch the firmware back out of MSI-X mode to INTx (clear IDR bit 6). Used by the
 * MSI-X completion self-test fallback: if MSI-X completions don't drain, we revert to the
 * known-good INTx completion path so bring-up still succeeds instead of hanging. */
VOID
SrcIoDisableMsixMode(_In_ PDEVICE_EXTENSION dev)
{
    ULONG idr;
    SrcWrite32(dev, SRC_REG_OIMR, 0xFFFFFFFF);          /* mask while switching modes */
    (void)SrcRead32(dev, SRC_REG_OIMR);
    idr = SrcRead32(dev, SRC_REG_IDR);
    SrcWrite32(dev, SRC_REG_IDR, idr & ~SRC_IDR_MSIX_ENABLE);
    (void)SrcRead32(dev, SRC_REG_IDR);
    dev->MsiEnabled = FALSE;
    SRCDBG("srcraid: MSI-X mode DISABLED (reverted to INTx)\n");
}

/*
 * SUSTAINED, bounded, interrupt-driven MSI-X completion self-test. Submits SRC_MSIX_PROBE_ROUNDS
 * benign RequestAdapterInfo FIBs ONE AT A TIME on the reserved probe slot; each round waits
 * (bounded) for the ISR to drain that round's completion (firmware -> MSI-X -> SrcIoDrainRrq
 * sets MsixProbeDone). Returns TRUE only if EVERY round's completion arrives through the
 * interrupt path - i.e. the MSI-X interrupt actually RE-ARMS after each completion.
 *
 * Why sustained, not one shot: a single-completion probe (0.1.0.158) PASSED yet the box still
 * hung, because MSI-X delivered the FIRST interrupt but did not re-arm (the ISR was missing the
 * ODR_CLEAR response-doorbell ack). Requiring N sequential re-armed completions catches exactly
 * that: if re-arm is broken, round 2 times out, we return FALSE, and the caller falls back to
 * INTx - so a broken MSI-X path can never hang boot (Ready is set only after a path is proven).
 */
BOOLEAN
SrcMsixProbe(_In_ PDEVICE_EXTENSION dev)
{
    PAAC_FIBHDR fib = (PAAC_FIBHDR)(dev->AdapterFibs + (SIZE_T)SRC_MSIXPROBE_SLOT * SRC_FIB_SIZE);
    ULONG fibPhys = dev->AdapterFibsPhys + SRC_MSIXPROBE_SLOT * SRC_FIB_SIZE;
    ULONG fibSize = sizeof(AAC_FIBHDR) + 512, fibSize128, iqLow, r, i;

    if (dev->AdapterFibs == NULL) {
        return FALSE;
    }
    fibSize128 = (fibSize + 127) / 128 - 1;
    if (fibSize128 > 31) fibSize128 = 31;
    iqLow = (fibPhys & ~0x7Fu) | fibSize128;

    for (r = 0; r < SRC_MSIX_PROBE_ROUNDS; r++) {
        InterlockedExchange(&dev->MsixProbeDone, 0);
        RtlZeroMemory(fib, SRC_FIB_SIZE);
        fib->XferState          = XS_SEND_SYNC | XS_Async;
        fib->Command            = (USHORT)FIBCMD_RequestAdapterInfo;   /* 703 - benign read */
        fib->StructType         = FIB_MAGIC2;               /* 4 - IQ delivery */
        fib->Size               = (USHORT)fibSize;
        fib->SenderSize         = (USHORT)SRC_FIB_SIZE;
        fib->SenderFibAddress   = fibPhys;
        fib->Handle             = (SRC_MSIXPROBE_SLOT << 2) + 1;
        KeMemoryBarrier();
        SrcWrite32(dev, SRC_REG_IQ_H, 0);
        SrcWrite32(dev, SRC_REG_IQ_L, iqLow);

        for (i = 0; i < 10000; i++) {                       /* ~1 s/round at 100 us/iter */
            if (InterlockedCompareExchange(&dev->MsixProbeDone, 0, 0) != 0) {
                break;
            }
            StorPortStallExecution(100);
        }
        if (InterlockedCompareExchange(&dev->MsixProbeDone, 0, 0) == 0) {
            dev->MsixProbeRounds = r;                 /* DIAG: how many rounds re-armed */
            SRCDBG("srcraid: MSI-X probe FAILED at round %u/%u - interrupt did not re-arm "
                   "(only %u sustained completions, ISR fired %d)\n",
                   r + 1, (ULONG)SRC_MSIX_PROBE_ROUNDS, r, dev->MsixIsrCount);
            return FALSE;
        }
    }
    dev->MsixProbeRounds = SRC_MSIX_PROBE_ROUNDS;
    SRCDBG("srcraid: MSI-X probe OK - %u sustained interrupt-driven completions (ISR fired %d)\n",
           (ULONG)SRC_MSIX_PROBE_ROUNDS, dev->MsixIsrCount);
    return TRUE;
}

BOOLEAN
SrcIoDrainRrq(_In_ PDEVICE_EXTENSION dev)
{
    volatile ULONG *rrq = (volatile ULONG *)dev->HostRrq;
    ULONG odr, entry, handle, slot, idx;
    ULONG vec, nvec, base, limit, vecLo, vecHi;
    BOOLEAN handled = FALSE;

    if (rrq == NULL || dev->VectorCap == 0) {
        return FALSE;
    }
    /* Which regions to drain. Default: ALL (robust - a completion can land in any region, and
     * InterruptSynchronizeAll serializes ISRs so it's race-free). Per-vector (SRC_PERVEC_DRAIN):
     * when SrcMsInterrupt set DrainVec, drain ONLY that region (vendor model, better locality).
     * INTx is always nvec==1 (the single region spans the whole ring - today's exact behavior). */
    nvec = dev->MsiEnabled ? dev->NumVectors : 1;
    if (nvec < 1) nvec = 1;
    if (nvec > SRC_MAX_VECTORS) nvec = SRC_MAX_VECTORS;
    vecLo = 0;
    vecHi = nvec;
#if SRC_PERVEC_DRAIN
    if (dev->MsiEnabled) {
        LONG dv = dev->DrainVec;
        if (dv >= 0 && dv < (LONG)nvec) {
            vecLo = (ULONG)dv;              /* this message's region only */
            vecHi = vecLo + 1;
        }
    }
#endif

    if (dev->MsiEnabled) {
        /*
         * MSI-X: completions are in the host RRQ. Classify AIF/sync via ODR_MSI, then - the
         * ROOT-CAUSE FIX - ACK the response doorbell exactly like the INTx path and the vendor
         * driver do: read ODR_STATUS (0x9c) and, if RESP_SENT (bit 1) is set, write
         * 0x2 to ODR_CLEAR (0xa0). The vendor does this for EVERY interrupt, MSI-X included;
         * without it the doorbell stays asserted and the SRC never re-arms, so only the FIRST
         * MSI-X completion ever fires (this hung boot on 0.1.0.156/158 - all I/O after the first
         * completion was lost). Edge-signalled or not, the SRC requires the doorbell ack to
         * re-arm.
         */
        ULONG bell = SrcRead32(dev, SRC_REG_ODR_MSI);
        ULONG ostat;
        InterlockedIncrement(&dev->MsixIsrCount);   /* DIAG: count MSI-X ISR fires (re-arm check) */
        if (InterlockedIncrement(&g_dbgIsr) <= DBG_LIMIT) {
            SRCDBG("srcraid: ISR(MSI) ODR_MSI=0x%08x idx0=%u rrq[idx0]=0x%08x nvec=%u cap=%u\n",
                   bell, dev->HostRrqIdx[0], rrq[dev->HostRrqIdx[0]], dev->NumVectors, dev->VectorCap);
        }
        if (bell != 0xFFFFFFFF && (bell & SRC_ODB_MSI_AIF)) {
            SrcWrite32(dev, SRC_REG_IDR,
                       SrcRead32(dev, SRC_REG_IDR) | SRC_IDR_CLEAR_AIF);  /* drop AIF */
        }
        ostat = SrcRead32(dev, SRC_REG_ODR_STATUS);
        if (ostat != 0xFFFFFFFF && (ostat & SRC_ODB_RESP_SENT)) {
            SrcWrite32(dev, SRC_REG_ODR_CLEAR, SRC_ODB_RESP_SENT);        /* re-arm (vendor: [0xa0]=2) */
            (void)SrcRead32(dev, SRC_REG_ODR_CLEAR);
        }
        /* fall through to the common RRQ drain */
    } else {
        odr = SrcRead32(dev, SRC_REG_ODR_STATUS);
        if (InterlockedIncrement(&g_dbgIsr) <= DBG_LIMIT) {
            SRCDBG("srcraid: ISR ODR=0x%08x idx=%u rrq[idx]=0x%08x\n",
                   odr, dev->HostRrqIdx[0], rrq[dev->HostRrqIdx[0]]);
        }
        if (odr == 0xFFFFFFFF) {
            return FALSE;                     /* device gone */
        }
        if (!(odr & SRC_ODB_RESP_SENT)) {
            /* Not a completion. If an AIF (bit 18) or other outbound doorbell is
             * set, ack it so the level INTx line doesn't storm. */
            if (odr & 0x000FFFFFu) {
                /* AIF-RECEPTION DIAGNOSTIC: on an adapter event (bit 18 / any
                 * outbound doorbell that isn't a response) dump where the firmware
                 * might have posted the AIF - OMR/scratch registers, the sync
                 * mailbox, and the RRQ ring - so we can map the SRC AIF mechanism
                 * from a real triggered event (drive pull/insert, config change). */
                if (odr & 0x00040000u) {
                    volatile ULONG *mb = (volatile ULONG *)dev->Mailbox;
                    SRCDBG("srcraid: *** AIF *** odr=0x%08x OMR0=0x%08x r_c0=0x%08x r_c4=0x%08x "
                           "ODRMSI=0x%08x r_cc=0x%08x r_d0=0x%08x\n",
                           odr, SrcRead32(dev, 0xbc), SrcRead32(dev, 0xc0), SrcRead32(dev, 0xc4),
                           SrcRead32(dev, 0xc8), SrcRead32(dev, 0xcc), SrcRead32(dev, 0xd0));
                    if (mb != NULL) {
                        SRCDBG("srcraid:     AIF mailbox[0..7]=%08x %08x %08x %08x %08x %08x %08x %08x\n",
                               mb[0], mb[1], mb[2], mb[3], mb[4], mb[5], mb[6], mb[7]);
                    }
                    SRCDBG("srcraid:     AIF rrq idx=%u [idx..+3]=%08x %08x %08x %08x\n",
                           dev->HostRrqIdx[0],
                           rrq[dev->HostRrqIdx[0]],
                           rrq[(dev->HostRrqIdx[0] + 1) % dev->HostRrqEntries],
                           rrq[(dev->HostRrqIdx[0] + 2) % dev->HostRrqEntries],
                           rrq[(dev->HostRrqIdx[0] + 3) % dev->HostRrqEntries]);
                }
                SrcWrite32(dev, SRC_REG_ODR_CLEAR, odr & ~SRC_ODB_RESP_SENT);
                (void)SrcRead32(dev, SRC_REG_ODR_CLEAR);
                return TRUE;
            }
            return FALSE;                     /* not ours */
        }
        /* ack the response doorbell */
        SrcWrite32(dev, SRC_REG_ODR_CLEAR, SRC_ODB_RESP_SENT);
        (void)SrcRead32(dev, SRC_REG_ODR_CLEAR);
    }

    for (vec = vecLo; vec < vecHi; vec++) {
      base  = vec * dev->VectorCap;
      limit = base + dev->VectorCap;
      idx   = dev->HostRrqIdx[vec];
      if (idx < base || idx >= limit) idx = base;   /* clamp a stale index into the region */
      for (;;) {
        entry = rrq[idx] & 0x7FFFFFFFu;       /* strip toggle bit (31) */
        handle = entry & RRQ_HANDLE_MASK;     /* low 16 bits */
        if (handle == 0) {
            break;                            /* no more completions in this region */
        }
        rrq[idx] = 0;
        idx++; if (idx >= limit) idx = base;  /* advance within [base, base+VectorCap) */

        slot = handle >> 2;
        if (slot == SRC_MSIXPROBE_SLOT) {
            /* the MSI-X completion self-test FIB came back through the interrupt path =
             * MSI-X completion routing works. Signal SrcMsixProbe (no SRB to complete). */
            InterlockedExchange(&dev->MsixProbeDone, 1);
            handled = TRUE;
            continue;
        }
        if (slot == SRC_AIF_SLOT) {
            /* the held AifRequest completed = an adapter event arrived; queue it
             * and re-post. The slot stays pinned (not an SRB). */
            handled = TRUE;
            SrcAifOnComplete(dev);
            continue;
        }
        if (slot >= SRC_IO_SLOT_MIN && slot < SRC_NUM_IO_SLOTS) {
            /* PEEK the SRB but DO NOT free the slot yet. The slot's FIB buffer holds the
             * firmware's response we are about to copy; freeing the slot first lets a
             * concurrent HwStartIo (StorSynchronizeFullDuplex runs it on another CPU)
             * re-allocate this slot and RtlZeroMemory its FIB mid-copy -> corrupted/zeroed
             * mgmt responses (observed: ~5/621 ContainerCommand replies came back
             * XferState=0 or cross-contaminated, breaking maxView enumeration). Keeping the
             * slot non-NULL blocks re-allocation (SrcIoAllocSlot CAS needs NULL) until we
             * free it below, after the copy. */
            PSCSI_REQUEST_BLOCK srb = (PSCSI_REQUEST_BLOCK)dev->IoSlotSrb[slot];
            if (srb != NULL) {
                PAAC_FIBHDR fib = (PAAC_FIBHDR)(dev->AdapterFibs +
                                                (SIZE_T)slot * SRC_FIB_SIZE);
                PUCHAR rep = (PUCHAR)fib + sizeof(AAC_FIBHDR);
                PSRB_EXTENSION ext = (PSRB_EXTENSION)srb->SrbExtension;
                UCHAR kind = (ext != NULL) ? ext->Kind : SRC_FIBK_RAWIO2;

                if (kind == SRC_FIBK_MGMT) {
                    /* AAC mgmt FIB passthrough: copy the WHOLE response FIB back to the
                     * user's IOCTL buffer, exactly like the vendor driver does
                     * (memmove(ApiFibAddress, Fib, Fib->Header.Size)). The SRB_IO_CONTROL
                     * header is at DataBuffer+0 (HeaderLength@0), the FIB at +HeaderLength. */
                    PUCHAR db     = (PUCHAR)srb->DataBuffer;
                    ULONG  hdrLen = (db != NULL) ? *(PULONG)db : 0;
                    ULONG  maxLen = (srb->DataTransferLength > hdrLen)
                                  ? srb->DataTransferLength - hdrLen : 0;
                    ULONG  respLen = (ULONG)fib->Size;        /* response FIB total (hdr+data) */
                    if (respLen > maxLen)        respLen = maxLen;
                    if (respLen > SRC_FIB_SIZE)  respLen = SRC_FIB_SIZE;
                    if (db != NULL && respLen != 0) {
                        RtlCopyMemory(db + hdrLen, fib, respLen);
                    }
                    srb->SrbStatus = SRB_STATUS_SUCCESS;
                    if (InterlockedIncrement(&g_dbgDone) <= DBG_LIMIT) {
                        SRCDBG("srcraid: MGMT done slot=%u cmd=%u respsz=%u maxlen=%u\n",
                               slot, (ULONG)fib->Command, respLen, maxLen);
                    }
                    {   /* DIAGNOSTIC in-memory trace: mgmt FIB response (kind 2). */
                        PULONG rs = (PULONG)((PUCHAR)fib + sizeof(AAC_FIBHDR));
                        SrcTrace(dev, 2, (ULONG)fib->Command, respLen, fib->XferState,
                                 rs[0], rs[1], rs[2], rs[3]);
                    }
                } else if (kind == SRC_FIBK_RAWIOCTL) {
                    /* maxView raw-SRB passthrough (0x42264/0x4201B) completed async: copy the
                     * device data from the per-slot bounce region back into the IOCTL buffer,
                     * write the actual transfer length and the SRB_IO_CONTROL ReturnCode. The
                     * request fields in the IOCTL buffer are still intact, so recompute the data
                     * offset/length/direction from them (p+0x10 is overwritten LAST). */
                    PAAC_SRB_REPLY r   = (PAAC_SRB_REPLY)rep;
                    PUCHAR  db      = (PUCHAR)srb->DataBuffer;
                    ULONG   hdrLen  = (db != NULL) ? *(PULONG)db : 0;
                    PUCHAR  pl      = (db != NULL) ? db + hdrLen : NULL;
                    PUCHAR  dataBuf = (ext != NULL && ext->BounceIdx >= 0)
                                    ? dev->RawBounce + (SIZE_T)ext->BounceIdx * SRC_RAWBOUNCE_SIZE
                                    : (PUCHAR)fib + SRC_RAWIOCTL_DATAOFF;
                    if (pl != NULL) {
                        ULONG   flags   = *(PULONG)(pl + 0x0c);
                        ULONG   dataLen = *(PULONG)(pl + 0x10);
                        ULONG   dataOff = ((ULONG)(*(PUSHORT)(pl + 0x00)) + (ULONG)pl[0x0b] + 7) & ~3u;
                        BOOLEAN dataIn  = (flags & AAC_SRB_DataIn) ? TRUE : FALSE;
                        ULONG   xfer;
                        if (dataLen > SRC_RAWIOCTL_DATAMAX) dataLen = SRC_RAWIOCTL_DATAMAX;
                        if (entry & RRQ_FASTRESP) {
                            xfer = dataLen;              /* fast success: full transfer in the bounce */
                        } else {
                            xfer = (r->dataXferLength <= dataLen) ? r->dataXferLength : dataLen;
                        }
                        if (dataIn && dataLen) {
                            RtlZeroMemory(pl + dataOff, dataLen);
                            if (xfer) RtlCopyMemory(pl + dataOff, dataBuf, xfer);
                        }
                        *(PULONG)(pl + 0x10) = xfer;     /* actual transfer (last - was the length) */
                        *(PULONG)(db + 0x14) = 0;        /* SRB_IO_CONTROL.ReturnCode = 0 (vendor success) */
                    }
                    if (ext != NULL && ext->BounceIdx >= 0) {
                        SrcRawBounceFree(dev, ext->BounceIdx);   /* release the >1KB pool buffer */
                        ext->BounceIdx = -1;
                    }
                    srb->SrbStatus = SRB_STATUS_SUCCESS;
                    if (InterlockedIncrement(&g_dbgDone) <= DBG_LIMIT) {
                        SRCDBG("srcraid: RAWIOCTL done slot=%u fast=%u srbStatus=0x%x xfer=%u\n",
                               slot, (entry & RRQ_FASTRESP) ? 1u : 0u, r->srbStatus, r->dataXferLength);
                    }
                    SrcTrace(dev, 7, r->srbStatus, r->scsiStatus, r->dataXferLength,
                             r->status, (entry & RRQ_FASTRESP) ? 1u : 0u, slot, 0xA5);
                } else if (kind == SRC_FIBK_SRB) {
                    /* physical pass-through: aac_srb_reply carries the Windows
                     * SrbStatus/ScsiStatus (same codes), transfer length + sense. */
                    PAAC_SRB_REPLY r = (PAAC_SRB_REPLY)rep;
                    if (entry & RRQ_FASTRESP) {
                        /* FAST RESPONSE = success with NO reply FIB written (the aac_srb_reply
                         * fields below are STALE). For a read the data was already DMA'd to the
                         * SRB buffer. Must check this FIRST - otherwise a successful pass-through
                         * read is mis-read from stale senseDataSize as a CHECK CONDITION and
                         * returned as an I/O error (raw pass-through disks showed uninitialized). */
                        srb->SrbStatus = SRB_STATUS_SUCCESS;
                    } else if (r->srbStatus != 0) {
                        srb->SrbStatus  = (UCHAR)r->srbStatus;
                        srb->ScsiStatus = (UCHAR)r->scsiStatus;
                        if (r->dataXferLength <= srb->DataTransferLength) {
                            srb->DataTransferLength = r->dataXferLength;
                        }
                        if (r->senseDataSize != 0 && srb->SenseInfoBuffer != NULL &&
                            srb->SenseInfoBufferLength != 0) {
                            ULONG sn = (r->senseDataSize < srb->SenseInfoBufferLength)
                                       ? r->senseDataSize : srb->SenseInfoBufferLength;
                            if (sn > sizeof(r->senseData)) sn = sizeof(r->senseData);
                            RtlCopyMemory(srb->SenseInfoBuffer, r->senseData, sn);
                            srb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
                        }
                    } else if (r->senseDataSize != 0) {
                        /* device check-condition reported with aac srb_status 0:
                         * surface it as an error + autosense (don't fake success,
                         * so a failed WRITE isn't silently reported OK) */
                        srb->ScsiStatus = (UCHAR)r->scsiStatus;
                        srb->SrbStatus  = SRB_STATUS_ERROR;
                        if (r->dataXferLength <= srb->DataTransferLength) {
                            srb->DataTransferLength = r->dataXferLength;
                        }
                        if (srb->SenseInfoBuffer != NULL && srb->SenseInfoBufferLength != 0) {
                            ULONG sn = (r->senseDataSize < srb->SenseInfoBufferLength)
                                       ? r->senseDataSize : srb->SenseInfoBufferLength;
                            if (sn > sizeof(r->senseData)) sn = sizeof(r->senseData);
                            RtlCopyMemory(srb->SenseInfoBuffer, r->senseData, sn);
                            srb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
                        }
                    } else {
                        srb->SrbStatus = (entry & RRQ_FASTRESP)
                                       ? SRB_STATUS_SUCCESS : SRB_STATUS_ERROR;
                    }
                    if (InterlockedIncrement(&g_dbgDone) <= DBG_LIMIT) {
                        SRCDBG("srcraid: PHYS done slot=%u srbStatus=0x%x scsi=0x%x xfer=%u "
                               "sense=%u SrbStatus=0x%x\n", slot, r->srbStatus, r->scsiStatus,
                               r->dataXferLength, r->senseDataSize, srb->SrbStatus);
                    }
                    /* DIAGNOSTIC trace: PHYS completion (kind 7). a=srbStatus, b=scsiStatus,
                     * c=senseDataSize, d0=dataXfer, d1=status, d2=fastresp, d3=SrbStatus set. */
                    SrcTrace(dev, 7, r->srbStatus, r->scsiStatus, r->senseDataSize,
                             r->dataXferLength, r->status, (entry & RRQ_FASTRESP) ? 1u : 0u,
                             srb->SrbStatus);
                } else if (kind == SRC_FIBK_FLUSH) {
                    /* CT_FLUSH_CACHE completion. This is a VM_ContainerConfig CT command, so the
                     * reply uses the CT-response layout (same as the enumeration path, see
                     * src_fib.c): resp[1] = the CT command echoed back (CT_FLUSH_CACHE = 129),
                     * resp[2] = CT status (CT_OK = 218), resp[3] = result. Fast response (no reply
                     * FIB written) = success. NOTE: an earlier version read resp[1] as the status
                     * and saw the echoed command (129) as an error, failing EVERY non-fast-response
                     * flush (SYNCHRONIZE_CACHE -> "I/O device error"); it only escaped notice where
                     * the firmware fast-completed the flush. Success is CT_OK (218) at resp[2]. */
                    PULONG fr = (PULONG)rep;
                    if (entry & RRQ_FASTRESP) {
                        srb->SrbStatus = SRB_STATUS_SUCCESS;
                    } else {
                        srb->SrbStatus = (fr[2] == 218) ? SRB_STATUS_SUCCESS : SRB_STATUS_ERROR;
                    }
                    /* trace kind 13 (FLUSH): a=fr[0] b=fr[1](cmd echo) c=fr[2](CT status)
                     * d0=fastresp d1=cid d2=fr[3] d3=SrbStatus. */
                    SrcTrace(dev, 13, fr[0], fr[1], fr[2],
                             (entry & RRQ_FASTRESP) ? 1u : 0u, (ULONG)srb->TargetId,
                             fr[3], srb->SrbStatus);
                    if (InterlockedIncrement(&g_dbgDone) <= DBG_LIMIT) {
                        SRCDBG("srcraid: FLUSH done slot=%u fast=%u status=0x%08x SrbStatus=0x%x\n",
                               slot, (entry & RRQ_FASTRESP) ? 1u : 0u, fr[1], srb->SrbStatus);
                    }
                } else {
                    PULONG rw = (PULONG)rep;
                    ULONG st = rw[0];
                    /* fast response (bit 30) = success; else reply status is in the FIB.
                     * Both 0 AND 20000 mean success for container I/O - the error condition is
                     * `status != 0 && status != 20000`. Treating 20000 as an error caused spurious
                     * I/O failures (and, on retry, a possible volume dismount). */
                    if (entry & RRQ_FASTRESP) {
                        srb->SrbStatus = SRB_STATUS_SUCCESS;
                    } else if (st == 0 || st == 20000) {
                        srb->SrbStatus = SRB_STATUS_SUCCESS;
                    } else if (st == 72) {
                        /* H1: firmware busy/retry - the vendor driver maps status 72 to
                         * SRB_STATUS_BUSY so StorPort re-queues the I/O rather than failing it
                         * (same shape as the 20000-as-error fix; a hard error here risks a
                         * volume dismount on retry). */
                        srb->SrbStatus = SRB_STATUS_BUSY;
                    } else if (st == 8) {
                        SrcIoSetSense(srb, 0x03, 0x11);  /* H2: media error (MEDIUM ERROR) */
                    } else {
                        srb->SrbStatus = SRB_STATUS_ERROR;
                    }
                    if (InterlockedIncrement(&g_dbgDone) <= DBG_LIMIT) {
                        SRCDBG("srcraid: IO done slot=%u entry=0x%08x fast=%u XferState=0x%08x "
                               "reply[0..3]=%08x %08x %08x %08x SrbStatus=0x%x\n",
                               slot, entry, (entry & RRQ_FASTRESP) ? 1 : 0, fib->XferState,
                               rw[0], rw[1], rw[2], rw[3], srb->SrbStatus);
                    }
                }
                /* response fully copied -> now free the slot (makes it re-allocatable) and
                 * complete. Freeing here (not before the copy) closes the slot-reuse race. */
                SrcIoFreeSlot(dev, slot);
                StorPortNotification(RequestComplete, dev, srb);
            }
        }
        handled = TRUE;
      }   /* inner for(;;) - drains one region */
      dev->HostRrqIdx[vec] = idx;
    }     /* for (vec) - all per-vector regions */
    if (handled && dev->ResetCount != 0) {
        dev->ResetCount = 0;   /* a completion drained -> the path is alive; clear the dead-counter */
    }
    return handled;
}

/* HwMSInterruptRoutine (MSI-X): StorPort calls this per MSI-X message with its MessageId.
 * We ack the MSI doorbell + drain all regions inside SrcIoDrainRrq (which, when MsiEnabled,
 * walks every per-vector region). MessageId is advisory here - draining all regions is the
 * correctness-first design; a later optimization can drain only region MessageId. */
BOOLEAN
SrcMsInterrupt(_In_ PVOID DeviceExtension, _In_ ULONG MessageId)
{
    PDEVICE_EXTENSION dev = (PDEVICE_EXTENSION)DeviceExtension;
    if (!dev->InterruptsEnabled) {
        return FALSE;
    }
#if SRC_PERVEC_DRAIN
    if ((LONG)MessageId > dev->MsixMaxMsg) {
        dev->MsixMaxMsg = (LONG)MessageId;   /* DIAG: confirm MessageId maxes at NumVectors-1 now */
    }
    /* drain just this message's region (MessageId maps to region MessageId % NumVectors;
     * InterruptSynchronizeAll serializes ISRs so this shared hint is race-free). */
    dev->DrainVec = (dev->NumVectors > 0) ? (LONG)(MessageId % dev->NumVectors) : -1;
#else
    UNREFERENCED_PARAMETER(MessageId);
    dev->DrainVec = -1;
#endif
    return SrcIoDrainRrq(dev);
}
