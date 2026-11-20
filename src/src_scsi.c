/*
 * src_scsi.c - SCSI command translation for container (logical volume) targets.
 *
 * Step A (this file, no FIBs issued): synthesize the non-data commands Windows
 * needs to enumerate the volume - INQUIRY, READ CAPACITY (10/16), TEST UNIT
 * READY, MODE SENSE, REPORT LUNS - directly from dev->Containers[]. READ/WRITE
 * are answered NOT READY here; the real data path (ContainerRawIo2 FIBs + RRQ
 * completion) is Step B in src_io.c.
 *
 * Target mapping: bus SRC_CONTAINER_BUS, TargetId = container id (cid), LUN 0.
 */
#include "srcraid.h"

/* big-endian stores (SCSI fields are big-endian) */
static VOID PutBe16(_Out_writes_(2) PUCHAR p, _In_ USHORT v)
{
    p[0] = (UCHAR)(v >> 8); p[1] = (UCHAR)v;
}
static VOID PutBe32(_Out_writes_(4) PUCHAR p, _In_ ULONG v)
{
    p[0] = (UCHAR)(v >> 24); p[1] = (UCHAR)(v >> 16);
    p[2] = (UCHAR)(v >> 8);  p[3] = (UCHAR)v;
}
static VOID PutBe64(_Out_writes_(8) PUCHAR p, _In_ ULONGLONG v)
{
    PutBe32(p, (ULONG)(v >> 32));
    PutBe32(p + 4, (ULONG)v);
}

/* Copy up to the SRB's data length; set DataTransferLength to what we returned. */
static VOID SrcScsiReturnData(_In_ PSCSI_REQUEST_BLOCK Srb,
                              _In_reads_bytes_(len) PVOID data, _In_ ULONG len)
{
    ULONG n = (len < Srb->DataTransferLength) ? len : Srb->DataTransferLength;
    if (Srb->DataBuffer != NULL && n != 0) {
        RtlCopyMemory(Srb->DataBuffer, data, n);
    }
    Srb->DataTransferLength = n;
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
}

/* Minimal fixed-format sense -> SRB_STATUS_ERROR (used for NOT READY etc.). */
static VOID SrcScsiSetSense(_In_ PSCSI_REQUEST_BLOCK Srb, _In_ UCHAR senseKey,
                            _In_ UCHAR asc, _In_ UCHAR ascq)
{
    Srb->SrbStatus = SRB_STATUS_ERROR | SRB_STATUS_AUTOSENSE_VALID;
    Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
    if (Srb->SenseInfoBuffer != NULL &&
        Srb->SenseInfoBufferLength >= sizeof(SENSE_DATA)) {
        PSENSE_DATA s = (PSENSE_DATA)Srb->SenseInfoBuffer;
        RtlZeroMemory(s, sizeof(*s));
        s->ErrorCode           = 0x70;        /* current, fixed format */
        s->Valid               = 0;
        s->SenseKey            = senseKey;
        s->AdditionalSenseLength = sizeof(SENSE_DATA) - RTL_SIZEOF_THROUGH_FIELD(SENSE_DATA, AdditionalSenseLength);
        s->AdditionalSenseCode = asc;
        s->AdditionalSenseCodeQualifier = ascq;
    } else {
        Srb->SrbStatus = SRB_STATUS_ERROR;
    }
}

/* Write val as 8 uppercase-hex ASCII chars at dst (for VPD serial/device-id). */
static VOID SrcHex32(_Out_writes_(8) PUCHAR dst, _In_ ULONG val)
{
    static const CHAR hx[] = "0123456789ABCDEF";
    ULONG i;
    for (i = 0; i < 8; i++) {
        dst[i] = (UCHAR)hx[(val >> ((7 - i) * 4)) & 0xF];
    }
}

static VOID SrcScsiInquiry(_In_ PSRC_CONTAINER c, _In_ PSCSI_REQUEST_BLOCK Srb, _In_ PCDB cdb)
{
    UCHAR buf[96];

    if (cdb->CDB6INQUIRY3.EnableVitalProductData) {
        UCHAR page = cdb->CDB6INQUIRY3.PageCode;
        RtlZeroMemory(buf, sizeof(buf));
        buf[0] = 0;                 /* direct-access device */
        buf[1] = page;
        if (page == 0x00) {         /* supported VPD pages */
            buf[3] = 3;
            buf[4] = 0x00; buf[5] = 0x80; buf[6] = 0x83;
            SrcScsiReturnData(Srb, buf, 7);
        } else if (page == 0x80) {  /* unit serial number - UNIQUE per container */
            buf[3] = 8;
            SrcHex32(&buf[4], c->Serial);           /* 8 hex chars, distinct per volume */
            SrcScsiReturnData(Srb, buf, 12);
        } else if (page == 0x83) {  /* device identification - UNIQUE T10 designator */
            buf[3] = 4 + 8 + 16;
            buf[4] = 0x02;          /* code set = ASCII */
            buf[5] = 0x01;          /* id type = T10 vendor id */
            buf[7] = 8 + 16;
            RtlCopyMemory(&buf[8], "ADAPTEC ", 8);
            RtlCopyMemory(&buf[16], "SRCRAID-", 8); /* 8 */
            SrcHex32(&buf[24], c->Serial);          /* + 8 hex = 16-byte unique id */
            SrcScsiReturnData(Srb, buf, 8 + 8 + 16);
        } else {
            SrcScsiSetSense(Srb, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
        }
        return;
    }

    RtlZeroMemory(buf, sizeof(buf));
    buf[0] = 0;                     /* DIRECT_ACCESS_DEVICE */
    buf[1] = 0;                     /* not removable */
    buf[2] = 5;                     /* SPC-3 */
    buf[3] = 2;                     /* response data format */
    buf[4] = 31;                    /* additional length */
    buf[7] = 0x02;                  /* CmdQue=1: tagged command queuing supported. WITHOUT this bit
                                     * Windows issues ONE command at a time per LUN (queue depth 1),
                                     * which serialized reads to single-disk speed and killed queue
                                     * scaling (CDM read did not improve from Q1T1 to Q8T1); writes
                                     * were unaffected because the BBU write-back cache absorbs them.
                                     * The vendor sets this bit too. */
    RtlCopyMemory(&buf[8],  "ADAPTEC ", 8);
    RtlCopyMemory(&buf[16], "srcraid Volume  ", 16);
    RtlCopyMemory(&buf[32], "0001", 4);
    SrcScsiReturnData(Srb, buf, 36);
}

static VOID SrcScsiReadCapacity10(_In_ PSRC_CONTAINER c, _In_ PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR buf[8];
    ULONGLONG lastLba = (c->SizeBlocks != 0) ? c->SizeBlocks - 1 : 0;

    /* If the last LBA doesn't fit in 32 bits, report 0xFFFFFFFF to force RC16. */
    PutBe32(&buf[0], (lastLba > 0xFFFFFFFFULL) ? 0xFFFFFFFFul : (ULONG)lastLba);
    PutBe32(&buf[4], c->BlockSize);
    SrcScsiReturnData(Srb, buf, sizeof(buf));
}

static VOID SrcScsiReadCapacity16(_In_ PSRC_CONTAINER c, _In_ PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR buf[32];
    ULONGLONG lastLba = (c->SizeBlocks != 0) ? c->SizeBlocks - 1 : 0;

    RtlZeroMemory(buf, sizeof(buf));
    PutBe64(&buf[0], lastLba);
    PutBe32(&buf[8], c->BlockSize);
    SrcScsiReturnData(Srb, buf, sizeof(buf));
}

/* Caching mode page (0x08): advertise write-cache enabled (WCE=1) so Windows stays
 * flush-aware and keeps issuing SYNCHRONIZE CACHE (which we now honor via CT_FLUSH_CACHE).
 * We do NOT set DPOFUA - we don't implement FUA, so Windows must use explicit flushes. */
static VOID SrcFillCachingPage(_Out_writes_(20) PUCHAR p)
{
    RtlZeroMemory(p, 20);
    p[0] = 0x08;        /* page code */
    p[1] = 0x12;        /* page length = 18 */
    p[2] = 0x04;        /* WCE = 1 */
}

static VOID SrcScsiModeSense6(_In_ PSRC_CONTAINER c, _In_ PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR   buf[24];
    UCHAR   page = (UCHAR)(Srb->Cdb[2] & 0x3F);
    ULONG   len  = 4;                               /* 4-byte parameter header */

    RtlZeroMemory(buf, sizeof(buf));
    buf[2] = c->ReadOnly ? 0x80 : 0x00;             /* device-specific param: WP (no DPOFUA) */
    if (page == 0x08 || page == 0x3F) {             /* caching page or all pages */
        SrcFillCachingPage(&buf[4]);
        len = 4 + 20;
    }
    buf[0] = (UCHAR)(len - 1);                       /* mode data length (excludes itself) */
    SrcScsiReturnData(Srb, buf, len);
}

static VOID SrcScsiModeSense10(_In_ PSRC_CONTAINER c, _In_ PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR   buf[32];
    UCHAR   page = (UCHAR)(Srb->Cdb[2] & 0x3F);
    ULONG   len  = 8;                               /* 8-byte parameter header */

    RtlZeroMemory(buf, sizeof(buf));
    buf[3] = c->ReadOnly ? 0x80 : 0x00;             /* device-specific param: WP */
    if (page == 0x08 || page == 0x3F) {
        SrcFillCachingPage(&buf[8]);
        len = 8 + 20;
    }
    PutBe16(&buf[0], (USHORT)(len - 2));            /* mode data length (excludes itself) */
    SrcScsiReturnData(Srb, buf, len);
}

static VOID SrcScsiReportLuns(_In_ PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR buf[16];
    RtlZeroMemory(buf, sizeof(buf));
    PutBe32(&buf[0], 8);            /* LUN list length = one 8-byte entry */
    SrcScsiReturnData(Srb, buf, 16);
}

/*
 * Translate one SRB_FUNCTION_EXECUTE_SCSI for a container target. Returns TRUE
 * if completed synchronously here (caller completes the SRB); FALSE if the
 * request was queued for async completion (Step B READ/WRITE).
 */
BOOLEAN
SrcScsiExecute(
    _In_ PDEVICE_EXTENSION dev,
    _In_ PSCSI_REQUEST_BLOCK Srb
    )
{
    PCDB cdb = (PCDB)Srb->Cdb;
    PSRC_CONTAINER c;

    /* Physical pass-through bus: relay every CDB to the real device via aac_srb. */
    if (Srb->PathId == SRC_PHYSICAL_BUS) {
        if (SrcPhysExecute(dev, Srb)) {
            return FALSE;       /* queued async; completed later in the ISR */
        }
        return TRUE;            /* SrcPhysExecute set the error SrbStatus */
    }

    if (Srb->PathId != SRC_CONTAINER_BUS || Srb->TargetId >= SRC_MAX_CONTAINERS ||
        Srb->Lun != 0 || !dev->Containers[Srb->TargetId].Valid ||
        dev->Containers[Srb->TargetId].Hidden) {
        /* .Hidden: maxView unpresented this LUN via IOCTL 0x42017 - answer selection
         * timeout so Windows drops the disk (it re-scans on the BusChangeDetected). */
        Srb->SrbStatus = SRB_STATUS_SELECTION_TIMEOUT;
        return TRUE;
    }
    c = &dev->Containers[Srb->TargetId];

    /* Raise the per-LUN tagged queue depth once (the vendor sets a deep depth for RAID volumes;
     * StorPort's default can otherwise cap outstanding I/O below what the array can service, which
     * shows up as reads not scaling with queue depth). Paired with the INQUIRY CmdQue bit. */
    if (!c->QueueDepthSet) {
        c->QueueDepthSet = TRUE;
        (void)StorPortSetDeviceQueueDepth(dev, Srb->PathId, Srb->TargetId, Srb->Lun,
                                          SRC_LUN_QUEUE_DEPTH);
    }

    /* A container the firmware reports NOT READY (degraded/rebuilding/initializing/
     * foreign) must not claim ready or accept I/O - otherwise Windows mounts it and
     * issues reads/writes the firmware rejects, producing an I/O-error storm. Reject
     * TEST UNIT READY and all data access with a clean NOT READY; INQUIRY / READ
     * CAPACITY / REPORT LUNS / MODE SENSE still answer so the disk still enumerates
     * (shown in Windows, but inaccessible) rather than vanishing. */
    if (c->NotReady) {
        switch (cdb->CDB10.OperationCode) {
        case SCSIOP_TEST_UNIT_READY:
        case SCSIOP_READ6:   case SCSIOP_READ:   case SCSIOP_READ12:   case SCSIOP_READ16:
        case SCSIOP_WRITE6:  case SCSIOP_WRITE:  case SCSIOP_WRITE12:  case SCSIOP_WRITE16:
        case SCSIOP_SYNCHRONIZE_CACHE: case SCSIOP_SYNCHRONIZE_CACHE16:
            SrcScsiSetSense(Srb, SCSI_SENSE_NOT_READY, SCSI_ADSENSE_LUN_NOT_READY, 0);
            return TRUE;
        default:
            break;
        }
    }

    switch (cdb->CDB10.OperationCode) {
    case SCSIOP_TEST_UNIT_READY:
    case SCSIOP_START_STOP_UNIT:
    case SCSIOP_VERIFY:
    case SCSIOP_VERIFY16:
    case SCSIOP_RESERVE_UNIT:
    case SCSIOP_RELEASE_UNIT:
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        break;

    case SCSIOP_SYNCHRONIZE_CACHE:
    case SCSIOP_SYNCHRONIZE_CACHE16:
        /* actually flush the controller write-back cache (async CT_FLUSH_CACHE) rather
         * than faking success - so acknowledged writes survive a power loss. */
        if (SrcIoFlush(dev, Srb)) {
            return FALSE;       /* queued async; completed in the ISR */
        }
        /* not queued (slot BUSY): SrcIoFlush set SrbStatus (StorPort retries) */
        break;

    case SCSIOP_INQUIRY:
        SrcScsiInquiry(c, Srb, cdb);
        break;
    case SCSIOP_READ_CAPACITY:
        SrcScsiReadCapacity10(c, Srb);
        break;
    case SCSIOP_READ_CAPACITY16:    /* SERVICE_ACTION_IN(16), SA = 0x10 */
        if (cdb->READ_CAPACITY16.ServiceAction == SERVICE_ACTION_READ_CAPACITY16) {
            SrcScsiReadCapacity16(c, Srb);
        } else {
            SrcScsiSetSense(Srb, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
        }
        break;
    case SCSIOP_MODE_SENSE:
        SrcScsiModeSense6(c, Srb);
        break;
    case SCSIOP_MODE_SENSE10:
        SrcScsiModeSense10(c, Srb);
        break;
    case SCSIOP_REPORT_LUNS:
        SrcScsiReportLuns(Srb);
        break;

    case SCSIOP_READ6:   case SCSIOP_READ:   case SCSIOP_READ12:   case SCSIOP_READ16:
    case SCSIOP_WRITE6:  case SCSIOP_WRITE:  case SCSIOP_WRITE12:  case SCSIOP_WRITE16:
        if (SrcIoReadWrite(dev, Srb, c)) {
            return FALSE;       /* queued async; completed later in the ISR */
        }
        /* Not queued: SrcIoReadWrite set BUSY or SUCCESS (0-length); otherwise
         * it was an unsupported/too-large request still marked PENDING. */
        if (Srb->SrbStatus == SRB_STATUS_PENDING) {
            SrcScsiSetSense(Srb, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
        }
        break;

    default:
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        break;
    }
    return TRUE;
}
