/*
 * src_fib.c - FIB submission + host-RRQ completion (SRC new comm / TYPE2).
 *
 * Submit (derived from the SRC interface + aacraid aac_src_deliver_message):
 *   build FIB in the adapter pool, StructType=FIB_MAGIC2, SenderFibAddress=fib_pa,
 *   fibsize=(Size+127)/128-1 (<=31) in the low addr bits, write IQ_H(+0xc4)/IQ_L(+0xc0).
 * Complete (aac_src_intr_message): firmware writes the FIB handle into the host RRQ ring;
 *   non-zero entry (bits 0..15, >>2 = slot) is a completion. Handle scheme VERIFIED on HW:
 *   submit Handle=(slot<<2)+1, complete slot=(rrq & 0xffff)>>2.
 *
 * SrcFibSubmitSync does a serialized (slot-0) sync FIB used during init/enumeration;
 * concurrent BuildIo/StartIo queueing with per-request slots is the next step.
 */
#include "srcraid.h"

/*
 * Build a FIB with `command` carrying `inSize` bytes of input, size it for `fibDataSize`
 * bytes of payload (so the firmware's response fits), post it to the inbound queue, and
 * poll the host RRQ for completion. The response is left in place in the FIB data area.
 */
ULONG
SrcFibSubmitSyncEx(
    _In_ PDEVICE_EXTENSION dev,
    _In_ USHORT command,
    _In_reads_bytes_opt_(inSize) PVOID inData,
    _In_ ULONG inSize,
    _In_ ULONG fibDataSize,
    _In_ ULONG timeoutMs,
    _In_ BOOLEAN inPlace
    )
{
    PAAC_FIBHDR fib;
    PUCHAR fibData;
    volatile ULONG *rrq;
    ULONG fibPhys, fibSize, fibSize128, iqLow, iters, i, entry, idx;

    if (dev->AdapterFibs == NULL || dev->HostRrq == NULL) {
        return SRC_SYNC_FAILED;
    }

    fib     = (PAAC_FIBHDR)dev->AdapterFibs;                 /* slot 0 (page-aligned) */
    fibData = (PUCHAR)fib + sizeof(AAC_FIBHDR);
    fibPhys = dev->AdapterFibsPhys;
    fibSize = sizeof(AAC_FIBHDR) + fibDataSize;

    RtlZeroMemory(fib, SRC_FIB_SIZE);
    if (inData != NULL && inSize != 0) {
        RtlCopyMemory(fibData, inData, inSize);
    }
    /*
     * Two delivery/completion models:
     *  - RRQ (inPlace=FALSE): ResponseExpected|FastResponseCapable + a handle,
     *    posted to the inbound queue (IQ_L/IQ_H); firmware posts the handle to
     *    the host RRQ ring. Works for RequestAdapterInfo and VM_ContainerConfig.
     *  - Synchronous FIB (inPlace=TRUE): the vendor driver / aacraid
     *    sync-mode model. The FIB (XferState=0x102d, StructType=FIB_MAGIC2_64,
     *    no handle) is handed to the firmware by MAILBOX command
     *    SEND_SYNCHRONOUS_FIB with p1 = FIB physical address - NOT the inbound
     *    queue. The firmware reads the FIB at that address, processes it in
     *    place (response written back into the same buffer), sets
     *    AdapterProcessed, and raises the sync-done doorbell. VM_NameServe is
     *    only serviced through this channel; it is silently dropped on the
     *    inbound queue regardless of FIB header.
     */
    if (inPlace) {
        fib->XferState  = XS_HostOwned | XS_FibInitialized | XS_FibEmpty |
                          XS_SentFromHost | XS_NormalPriority;   /* 0x102d */
        fib->StructType = FIB_MAGIC2_64;                         /* 5 */
        fib->Handle     = 0;
    } else {
        fib->XferState  = XS_SEND_SYNC;
        fib->StructType = FIB_MAGIC2;
        fib->Handle     = (0u << 2) + 1;                         /* slot 0 */
    }
    fib->Command          = command;
    fib->Size             = (USHORT)fibSize;
    fib->SenderSize       = (USHORT)SRC_FIB_SIZE;
    fib->SenderFibAddress = fibPhys;
    fib->ReceiverFibAddress = 0;

    KeMemoryBarrier();

    if (inPlace) {
        /* Deliver via the synchronous mailbox: SEND_SYNCHRONOUS_FIB(p1=fibPhys).
         * SrcSyncCommand rings the inbound sync doorbell and waits for the
         * sync-done bit; the processed response is then in the FIB buffer. */
        ULONG st = 0;
        ULONG sc = SrcSyncCommand(dev, SEND_SYNCHRONOUS_FIB, fibPhys,
                                  0, 0, 0, 0, 0, &st, NULL, NULL, NULL, NULL, timeoutMs);
        if (sc != SRC_SYNC_OK) {
            PULONG fd = (PULONG)fibData;
            SRCDBG("srcraid: SEND_SYNCHRONOUS_FIB cmd=%u sc=%u status=0x%08x "
                   "XferState=0x%08x data[0..3]=%08x %08x %08x %08x\n",
                   command, sc, st, fib->XferState, fd[0], fd[1], fd[2], fd[3]);
            return sc;
        }
        return SRC_SYNC_OK;
    }

    fibSize128 = (fibSize + 127) / 128 - 1;
    if (fibSize128 > 31) {
        fibSize128 = 31;
    }
    iqLow = (fibPhys & ~0x7Fu) | fibSize128;

    SrcWrite32(dev, SRC_REG_IQ_H, 0);
    SrcWrite32(dev, SRC_REG_IQ_L, iqLow);

    /*
     * RRQ completion: poll the ring at the advancing consumer index
     * (completion N lands at rrq[idx], not always rrq[0]); strip the toggle
     * bit (31) and advance so the next sync FIB polls the next slot.
     */
    rrq   = (volatile ULONG *)dev->HostRrq;
    idx   = dev->HostRrqIdx[0];
    iters = timeoutMs * 10;
    for (i = 0; ; i++) {
        /* scan every per-vector region for this sync FIB's completion. Under MSI-X the
         * firmware may post it into any vector's region; INTx has a single region (the
         * whole ring). Serialized sync path (MgmtBusy), so no ISR races the indices. */
        ULONG v;
        entry = 0;
        for (v = 0; v < dev->NumVectors; v++) {
            ULONG b = v * dev->VectorCap, vi = dev->HostRrqIdx[v];
            if (vi < b || vi >= b + dev->VectorCap) vi = b;
            entry = rrq[vi] & 0x7fffffffu;        /* strip toggle bit (31) */
            if (entry != 0) {
                rrq[vi] = 0;
                vi++; if (vi >= b + dev->VectorCap) vi = b;
                dev->HostRrqIdx[v] = vi;
                idx = vi;
                break;
            }
        }
        if (entry != 0) {
            break;
        }
        if (i >= iters) {
            volatile ULONG *rr = (volatile ULONG *)dev->HostRrq;
            PULONG fd = (PULONG)fibData;
            SRCDBG("srcraid: SrcFibSubmitSync cmd=%u TIMEOUT at rrq[%u]\n", command, idx);
            SRCDBG("  fib: XferState=0x%08x Cmd=%u Size=%u StructType=%u Handle=0x%x "
                   "data[0..3]=%08x %08x %08x %08x\n",
                   fib->XferState, fib->Command, fib->Size, fib->StructType, fib->Handle,
                   fd[0], fd[1], fd[2], fd[3]);
            SRCDBG("  rrq[0..7]=%08x %08x %08x %08x %08x %08x %08x %08x\n",
                   rr[0], rr[1], rr[2], rr[3], rr[4], rr[5], rr[6], rr[7]);
            SRCDBG("  regs: OMR0=0x%08x ODR_STATUS=0x%08x OMISR=0x%08x\n",
                   SrcRead32(dev, SRC_REG_OMR0), SrcRead32(dev, SRC_REG_ODR_STATUS),
                   SrcRead32(dev, SRC_REG_OMISR));
            return SRC_SYNC_TIMEOUT;
        }
        StorPortStallExecution(100);
    }
    SrcWrite32(dev, SRC_REG_ODR_CLEAR, SRC_ODB_RESP_SENT);
    return SRC_SYNC_OK;
}

/* RRQ-completion sync FIB (RequestAdapterInfo, VM_ContainerConfig). */
ULONG
SrcFibSubmitSync(
    _In_ PDEVICE_EXTENSION dev,
    _In_ USHORT command,
    _In_reads_bytes_opt_(inSize) PVOID inData,
    _In_ ULONG inSize,
    _In_ ULONG fibDataSize,
    _In_ ULONG timeoutMs
    )
{
    return SrcFibSubmitSyncEx(dev, command, inData, inSize, fibDataSize, timeoutMs, FALSE);
}

ULONG
SrcFibSelfTest(
    _In_ PDEVICE_EXTENSION dev,
    _In_ ULONG timeoutMs
    )
{
    ULONG rc = SrcFibSubmitSync(dev, (USHORT)FIBCMD_RequestAdapterInfo, NULL, 0, 480, timeoutMs);
    if (rc == SRC_SYNC_OK) {
        /* AAC_ADAPTER_INFO dwords (aacraid layout): [0]platform [1]cpu [3]clockMHz [6]totalmem
         * [7]kernelrev [8]kernelbuild [9]monitorrev [10]monitorbuild [13]biosrev [14]biosbuild.
         * Each *rev packs major.minor.sub in its top three bytes; *build is the build number
         * (vendor FW string is e.g. "7.5.0.54013"). Raw krev/kbuild are logged too so the
         * decode can be verified against the controller's reported version. */
        PULONG r = (PULONG)((PUCHAR)dev->AdapterFibs + sizeof(AAC_FIBHDR));
        SRCDBG("srcraid: FIRMWARE  kernel %u.%u.%u-%u  monitor %u.%u.%u-%u  bios %u.%u.%u-%u\n",
               (r[7]  >> 24) & 0xffu, (r[7]  >> 16) & 0xffu, (r[7]  >> 8) & 0xffu, r[8],
               (r[9]  >> 24) & 0xffu, (r[9]  >> 16) & 0xffu, (r[9]  >> 8) & 0xffu, r[10],
               (r[13] >> 24) & 0xffu, (r[13] >> 16) & 0xffu, (r[13] >> 8) & 0xffu, r[14]);
        SRCDBG("srcraid:   adapter platform=0x%x cpu=0x%x clock=%uMHz totalMemMB=%u "
               "(raw krev=0x%08x kbuild=%u)\n",
               r[0], r[1], r[3], r[6] >> 20, r[7], r[8]);
    }
    return rc;
}

/*
 * Container (logical volume) count: ContainerCommand FIB carrying
 * { VM_ContainerConfig, CT_GET_CONTAINER_COUNT }; response has MaxContainers.
 */
ULONG
SrcGetContainerCount(
    _In_ PDEVICE_EXTENSION dev,
    _Out_ PULONG count
    )
{
    ULONG req[2];
    ULONG rc;
    PULONG resp;

    *count = 0;
    req[0] = FIBCMD_VM_ContainerConfig;   /* 2 */
    req[1] = CT_GET_CONTAINER_COUNT;      /* 4 */

    rc = SrcFibSubmitSync(dev, (USHORT)FIBCMD_ContainerCommand,
                          req, sizeof(req), 32 /* room for the 24-byte response */, 1000);
    if (rc != SRC_SYNC_OK) {
        return rc;
    }
    resp = (PULONG)((PUCHAR)dev->AdapterFibs + sizeof(AAC_FIBHDR));
    /* resp: [0]=response(ST_OK=0), [1]=dummy, [2]=MaxContainers, [3]=switchEntries,
     *       [4]=MaxPartitions, [5]=MaxSimpleVolumes */
    *count = resp[2];
    SRCDBG("srcraid: container count: resp=0x%08x MaxContainers=%u switch=%u "
           "maxPart=%u maxSimple=%u\n", resp[0], resp[2], resp[3], resp[4], resp[5]);
    return SRC_SYNC_OK;
}

/*
 * Probe one container slot with VM_NameServe64 (aacraid _aac_probe_container).
 * A present volume returns status==ST_OK and mnt[0].vol != CT_NONE, with the
 * 64-bit block count in capacity/capacityhigh. An empty slot returns ST_OK with
 * vol==CT_NONE: that is not an error, out->Valid stays FALSE.
 */
ULONG
SrcProbeContainer(
    _In_ PDEVICE_EXTENSION dev,
    _In_ ULONG cid,
    _In_ ULONG command,
    _Out_ PSRC_CONTAINER out,
    _In_ ULONG timeoutMs
    )
{
    AAC_QUERY_MOUNT q;
    PAAC_MOUNT resp;
    ULONG rc;

    RtlZeroMemory(out, sizeof(*out));
    q.command = command;                    /* VM_NameServe / VM_NameServe64 / VM_NameServeAllBlk */
    /* VM_NameServeAllBlk enumerates block devices (FT_BLK), which is how variable/4K-sector
     * containers are reported; the legacy name-serve uses FT_FILESYS. */
    q.type    = (command == VM_NameServeAllBlk) ? FT_BLK : FT_FILESYS;
    q.count   = cid;

    /*
     * header.Size (and thus the inbound-queue fibsize) must describe the
     * HOST->ADAPTER payload = sizeof(aac_query_mount), exactly as aacraid's
     * aac_fib_send(ContainerCommand, fib, sizeof(struct aac_query_mount), ...).
     * The firmware writes the larger aac_mount response back into the same
     * 2048-byte FIB buffer; header.Size does not bound the response. Sizing
     * this by sizeof(AAC_MOUNT) made header.Size=136 and the name-serve
     * handler silently dropped the FIB.
     */
    rc = SrcFibSubmitSyncEx(dev, (USHORT)FIBCMD_ContainerCommand,
                            &q, sizeof(q), sizeof(q), timeoutMs, TRUE /* in-place */);

    resp = (PAAC_MOUNT)((PUCHAR)dev->AdapterFibs + sizeof(AAC_FIBHDR));
    /* DIAGNOSTIC (cid 0/1): dump the raw name-serve response in EVERY case - success, empty, or
     * rejected - so a firmware that hides a container is visible (status/count/vol/capacity). */
    if (cid < 2) {
        SRCDBG("srcraid: PROBE cid=%u cmd=%u qtype=%u rc=%u | status=0x%x rtype=%u count=%u | "
               "mnt0 vol=%u oid=%u obj=%u state=0x%x cap=%u capHi=%u fi[0..3]=%08x %08x %08x %08x\n",
               cid, command, q.type, rc, resp->status, resp->type, resp->count,
               resp->mnt[0].vol, resp->mnt[0].oid, resp->mnt[0].obj, resp->mnt[0].state,
               resp->mnt[0].capacity, resp->mnt[0].capacityhigh,
               resp->mnt[0].fileinfo[0], resp->mnt[0].fileinfo[1],
               resp->mnt[0].fileinfo[2], resp->mnt[0].fileinfo[3]);
    }

    if (rc != SRC_SYNC_OK) {
        return rc;
    }
    if (resp->status != ST_OK) {
        return SRC_SYNC_FAILED;
    }
    if (resp->mnt[0].vol == CT_NONE) {
        /* A variable/4K-sector container is reported by the filesystem name-serve
         * (VM_NameServe/VM_NameServe64) as vol=CT_NONE with obj=FT_BLK (status still ST_OK);
         * it only materializes under VM_NameServeAllBlk. Signal the caller to re-probe this
         * one slot with AllBlk, so a 4K array enumerates even alongside 512-byte arrays
         * (a whole-scan "found nothing -> try AllBlk" fallback misses it on a mixed box). */
        if (command != VM_NameServeAllBlk && resp->mnt[0].obj == FT_BLK) {
            return SRC_SYNC_BLKHINT;
        }
        return SRC_SYNC_OK;                 /* genuinely empty slot */
    }
    if (resp->mnt[0].state & FSCS_HIDDEN) {
        /* M3: firmware-hidden container (the vendor driver skips
         * ContentState & 4): a snapshot / host-reserved / in-creation container Windows
         * must not see. Treat the slot as empty rather than presenting it. */
        return SRC_SYNC_OK;
    }

    SRCDBG("srcraid: mnt cid=%u cmd=%u cap=%u capHi=%u vol=%u obj=%u state=0x%x\n",
           cid, command, resp->mnt[0].capacity, resp->mnt[0].capacityhigh,
           resp->mnt[0].vol, resp->mnt[0].obj, resp->mnt[0].state);
    SRCDBG("srcraid: mnt cid=%u fileinfo[0..7]=%08x %08x %08x %08x %08x %08x %08x %08x\n",
           cid, resp->mnt[0].fileinfo[0], resp->mnt[0].fileinfo[1],
           resp->mnt[0].fileinfo[2], resp->mnt[0].fileinfo[3],
           resp->mnt[0].fileinfo[4], resp->mnt[0].fileinfo[5],
           resp->mnt[0].fileinfo[6], resp->mnt[0].fileinfo[7]);

    out->Valid    = TRUE;
    out->Type     = resp->mnt[0].vol;
    out->ReadOnly = (resp->mnt[0].state & FSCS_READONLY) ? TRUE : FALSE;
    out->NotReady = (resp->mnt[0].state & FSCS_NOT_READY) ? TRUE : FALSE;
    /* capacityhigh is meaningful for the 64-bit and AllBlk variants; for the 32-bit
     * name-serve the field is undefined, so mask it to the low 32 bits. */
    out->SizeBlocks = (command == VM_NameServe)
        ? (ULONGLONG)resp->mnt[0].capacity
        : (((ULONGLONG)resp->mnt[0].capacityhigh << 32) | resp->mnt[0].capacity);
    /* Legacy name-serve reports 512-byte blocks. VM_NameServeAllBlk is used for variable/4K
     * containers and reports the real logical block size in the mount's fileinfo (vendor's
     * ObjExtension.FileSys block-size field); honor it so a 4K volume gets the right capacity. */
    out->BlockSize = 512;
    if (command == VM_NameServeAllBlk) {
        ULONG bs = resp->mnt[0].fileinfo[0];
        if (bs == 512 || bs == 1024 || bs == 2048 || bs == 4096) {
            out->BlockSize = bs;
        }
    }
    /* Stable, UNIQUE per-container id for the SCSI VPD serial (page 0x80) and device
     * id (page 0x83). The firmware object id (oid) gives cross-reboot stability; the
     * container slot (cid) in the top byte GUARANTEES uniqueness across presented
     * targets even if two oids ever collide. Without this every volume returned the
     * same page-0x83 designator and Windows treated multiple volumes as one disk. */
    out->Serial = ((cid & 0xFFu) << 24) | (resp->mnt[0].oid & 0x00FFFFFFu);
    return SRC_SYNC_OK;
}

/*
 * Query config status, then commit any pending configuration so the firmware
 * brings containers online for block I/O. On a fresh firmware bring-up the
 * containers enumerate (VM_NameServe) but block reads return ST_INVAL until
 * CT_COMMIT_CONFIG runs. Mirrors the vendor HandleAdapterGetConfig +
 * HandleAdapterCommitConfig (both VM_ContainerConfig via the sync mailbox).
 */
ULONG
SrcCommitConfig(
    _In_ PDEVICE_EXTENSION dev
    )
{
    ULONG req[8];
    PULONG resp;
    ULONG rc;

    /* 1) GET_CONFIG_STATUS (aac_get_config_status: cmd,type,parm1..5,count) */
    RtlZeroMemory(req, sizeof(req));
    req[0] = FIBCMD_VM_ContainerConfig;     /* 2   */
    req[1] = CT_GET_CONFIG_STATUS;          /* 147 */
    req[7] = 8;                             /* count = sizeof(resp.data) */
    rc = SrcFibSubmitSyncEx(dev, (USHORT)FIBCMD_ContainerCommand,
                            req, sizeof(req), sizeof(req), 1000, TRUE);
    resp = (PULONG)((PUCHAR)dev->AdapterFibs + sizeof(AAC_FIBHDR));
    SRCDBG("srcraid: GET_CONFIG_STATUS rc=%u resp=0x%08x status=0x%08x action=0x%08x\n",
           rc, resp[0], resp[2], resp[8]);

    /* 2) COMMIT_CONFIG (aac_commit_config: cmd,type) */
    RtlZeroMemory(req, sizeof(req));
    req[0] = FIBCMD_VM_ContainerConfig;     /* 2   */
    req[1] = CT_COMMIT_CONFIG;              /* 152 */
    rc = SrcFibSubmitSyncEx(dev, (USHORT)FIBCMD_ContainerCommand,
                            req, 8, 8, 1000, TRUE);
    resp = (PULONG)((PUCHAR)dev->AdapterFibs + sizeof(AAC_FIBHDR));
    SRCDBG("srcraid: COMMIT_CONFIG rc=%u resp=0x%08x\n", rc, resp[0]);
    return rc;
}

/*
 * Firmware 32-bit container UID (the vendor driver, CT cmd 165 /
 * CT_CID_TO_32BITS_UID). maxView/storarc reads this UID from the firmware's GET_CONFIG and then
 * calls IOCTL 0x42047 with it to map the volume to a Windows target - so the mgmt handlers MUST
 * answer with the SAME value, not our constructed VPD serial. VM_ContainerConfig sub-command
 * 165, param[0]=cid: on CT_OK (status 218) the UID is param[1] (response resp[3]); otherwise the
 * vendor falls back to the cid. Runs a sync-mailbox FIB; callers already serialize (init is
 * single-threaded; rescan holds MgmtBusy). (A truncated 24-bit UID here caused maxView's
 * lookup to miss and over-index its device/lane array -> the GET_CONFIG heap overrun.)
 */
static ULONG
SrcGetContainerUid(_In_ PDEVICE_EXTENSION dev, _In_ ULONG cid)
{
    ULONG  req[8];
    PULONG resp;
    ULONG  rc;

    RtlZeroMemory(req, sizeof(req));
    req[0] = FIBCMD_VM_ContainerConfig;     /* 2   */
    req[1] = CT_CID_TO_32BITS_UID;          /* 165 */
    req[2] = cid;                           /* param[0] = container index */
    req[7] = 8;                             /* count */
    rc = SrcFibSubmitSyncEx(dev, (USHORT)FIBCMD_ContainerCommand,
                            req, sizeof(req), sizeof(req), 1000, TRUE);
    resp = (PULONG)((PUCHAR)dev->AdapterFibs + sizeof(AAC_FIBHDR));
    if (rc == SRC_SYNC_OK && resp[2] == 218) {   /* CT_OK -> param[1] (resp[3]) is the 32-bit UID */
        return resp[3];
    }
    return cid;                             /* fallback = cid (matches the vendor) */
}

/*
 * M1: controller-wide write-back-cache flush (vendor sends VM_CloseAll on ScsiStopAdapter).
 * Sweeps CT_FLUSH_CACHE across every presented read/write container via the SYNC mailbox so
 * acknowledged writes survive power-off even for a container the OS did not separately
 * SYNCHRONIZE_CACHE. Called from SRB_FUNCTION_SHUTDOWN - a terminal, one-time event at DISPATCH,
 * NOT the periodic DISPATCH hazard the removed time-sync was. Best-effort: a flush failure is
 * logged, not fatal (the OS already SYNCHRONIZE_CACHE'd the mounted volumes, and a timed-out sync
 * FIB can only wedge a controller that is about to lose power anyway - and C2 re-inits it on the
 * next boot). Holds MgmtBusy to serialize the slot-0 sync FIB against any lingering mgmt path.
 */
VOID
SrcFlushAllContainers(
    _In_ PDEVICE_EXTENSION dev
    )
{
    ULONG req[8];
    ULONG cid, rc;

    if (dev->AdapterFibs == NULL || !dev->Ready || dev->DeadAdapter) {
        return;
    }
    while (InterlockedCompareExchange(&dev->MgmtBusy, 1, 0) != 0) {
        StorPortStallExecution(20);
    }
    for (cid = 0; cid < SRC_MAX_CONTAINERS; cid++) {
        if (!dev->Containers[cid].Valid || dev->Containers[cid].ReadOnly) {
            continue;                       /* empty slot or read-only: nothing to flush */
        }
        RtlZeroMemory(req, sizeof(req));
        req[0] = FIBCMD_VM_ContainerConfig;     /* 2   */
        req[1] = CT_FLUSH_CACHE;                /* 129 */
        req[2] = cid;                           /* container to flush */
        req[7] = 16;                            /* count = sizeof(reply.data) */
        rc = SrcFibSubmitSyncEx(dev, (USHORT)FIBCMD_ContainerCommand,
                                req, sizeof(req), sizeof(req), 3000, TRUE);
        SRCDBG("srcraid: SHUTDOWN cache flush cid=%u rc=%u\n", cid, rc);
    }
    InterlockedExchange(&dev->MgmtBusy, 0);
}

/*
 * M2: firmware config-ready poll (vendor IsAdapterReadyForConfigCommand). After the comm is up
 * the firmware may still be COMPUTING its container config; issuing GET_CONFIG_STATUS / enum FIBs
 * too early then returns no containers. This bites the post-IOP-reset resume / passive-init path
 * (the driver's reason to exist); a cold BIOS-POSTed boot is already config-ready so it returns on
 * the first poll. VM_ContainerConfig CT cmd 253: the firmware signals "still computing" ONLY when
 * status==0 && resp[2]==218 && resp[3]==0 (vendor check); anything else = ready. Bounded,
 * best-effort - proceed anyway after the cap so a quirky firmware can't hang bring-up.
 */
#define SRC_CONFIG_READY_TRIES  15u     /* x 100ms = 1.5s cap */

static BOOLEAN
SrcIsConfigReady(_In_ PDEVICE_EXTENSION dev)
{
    ULONG  req[8];
    PULONG resp;
    ULONG  rc;

    RtlZeroMemory(req, sizeof(req));
    req[0] = FIBCMD_VM_ContainerConfig;     /* 2   */
    req[1] = CT_IS_ADAPTER_READY;           /* 253 */
    req[7] = 8;
    rc = SrcFibSubmitSyncEx(dev, (USHORT)FIBCMD_ContainerCommand,
                            req, sizeof(req), sizeof(req), 1000, TRUE);
    if (rc != SRC_SYNC_OK) {
        return FALSE;                        /* FIB failed -> not ready (vendor returns 0) */
    }
    resp = (PULONG)((PUCHAR)dev->AdapterFibs + sizeof(AAC_FIBHDR));
    if (resp[0] == 0 && resp[2] == 218 && resp[3] == 0) {
        return FALSE;                        /* firmware still computing its config */
    }
    return TRUE;
}

VOID
SrcWaitForConfigReady(
    _In_ PDEVICE_EXTENSION dev
    )
{
    ULONG i;

    if (dev->AdapterFibs == NULL) {
        return;
    }
    for (i = 0; i < SRC_CONFIG_READY_TRIES; i++) {
        if (SrcIsConfigReady(dev)) {
            if (i > 0) {
                SRCDBG("srcraid: FW config ready after %u poll(s)\n", i + 1);
            }
            return;
        }
        StorPortStallExecution(100000);     /* 100 ms */
    }
    SRCDBG("srcraid: FW config NOT ready after %u polls - proceeding anyway\n",
           SRC_CONFIG_READY_TRIES);
}

/*
 * Walk every container slot the firmware exposes, record the valid ones in
 * dev->Containers, and log each. Stops early on a submit timeout (ring desync)
 * so a misbehaving probe can't hang HwInitialize for the full 256 * timeout.
 */
VOID
SrcEnumContainers(
    _In_ PDEVICE_EXTENSION dev
    )
{
    /*
     * Base filesystem name-serve command: VM_NameServe64 for current firmware, with a
     * VM_NameServe (32-bit) fallback only if the 64-bit query enumerates nothing at all
     * (old firmware). The all-block query is NOT a whole-scan variant - it is applied PER
     * SLOT: a 4K/variable-sector container is reported by the filesystem name-serve as
     * vol=CT_NONE + obj=FT_BLK (SrcProbeContainer returns SRC_SYNC_BLKHINT), and only that
     * slot is re-probed with VM_NameServeAllBlk. This finds a 4K array even when 512-byte
     * arrays exist on the SAME controller - the old "fall back to AllBlk only if the whole
     * scan found nothing" gate found the 512 array first and left the 4K volume invisible.
     * (Firmware 7.4 + a 4K-drive RAID5 hit exactly this: name-serve returned CT_NONE/FT_BLK
     * for the slot while arcconf showed the array Optimal.)
     */
    static const ULONG bases[] = { VM_NameServe64, VM_NameServe };
    ULONG cid, maxc, found = 0, base = VM_NameServe64, vi;
    SRC_CONTAINER c;
    ULONG rc;

    maxc = dev->MaxContainers;
    if (maxc == 0 || maxc > SRC_MAX_CONTAINERS) {
        maxc = SRC_MAX_CONTAINERS;
    }

    for (vi = 0; vi < RTL_NUMBER_OF(bases) && found == 0; vi++) {
        base = bases[vi];
        found = 0;
        for (cid = 0; cid < maxc; cid++) {
            rc = SrcProbeContainer(dev, cid, base, &c, 1000);
            if (rc == SRC_SYNC_BLKHINT) {
                /* variable/4K-sector container hidden from the filesystem name-serve */
                rc = SrcProbeContainer(dev, cid, VM_NameServeAllBlk, &c, 1000);
            }
            if (rc == SRC_SYNC_TIMEOUT) {
                SRCDBG("srcraid: container probe cid=%u timed out, stopping scan\n", cid);
                break;
            }
            if (rc != SRC_SYNC_OK || !c.Valid) {
                continue;
            }
            dev->Containers[cid] = c;
            dev->Containers[cid].CtUid = SrcGetContainerUid(dev, cid);  /* H4: FW 32-bit UID for maxView */
            found++;
            SRCDBG("srcraid: container %u VALID type=%u size=%I64u blocks "
                   "(%I64u MB) blk=%u ro=%u ctUid=0x%x\n",
                   cid, c.Type, c.SizeBlocks,
                   (c.SizeBlocks * c.BlockSize) / (1024ULL * 1024ULL),
                   c.BlockSize, c.ReadOnly, dev->Containers[cid].CtUid);
        }
        SRCDBG("srcraid: scan base=%u -> %u valid of %u probed\n", base, found, maxc);
        if (found > 0) {
            dev->NameServeCmd = base;           /* remember the base cmd for re-probe */
        }
    }

    if (dev->NameServeCmd == 0) {
        dev->NameServeCmd = VM_NameServe64;     /* default for rescan even if nothing found */
    }
    dev->NumValidContainers = found;
    SRCDBG("srcraid: SrcEnumContainers -> %u valid (base cmd=%u)\n", found, dev->NameServeCmd);
}

/*
 * Runtime re-probe of the logical-volume (container) bus, driven by a config-
 * change AIF (e.g. a RAID volume created/deleted at runtime via maxView/arcconf).
 * The counterpart to SrcRescanPhysical but for bus SRC_CONTAINER_BUS. Container
 * target id == container id (a fixed firmware slot), so ids are naturally stable:
 * we re-probe each slot in place and only flag a change on a valid<->empty
 * transition (add/remove), refreshing the stored fields otherwise. VM_NameServe
 * rides the sync mailbox (slot 0), so we hold MgmtBusy for the whole scan to
 * serialize against the 0x42264 mgmt path and the physical rescan. Returns TRUE
 * if the exposed set changed (caller issues BusChangeDetected on bus 0).
 */
BOOLEAN
SrcRescanContainers(
    _In_ PDEVICE_EXTENSION dev
    )
{
    ULONG   cid, maxc, command, found = 0;
    BOOLEAN changed = FALSE;

    if (dev->AdapterFibs == NULL || !dev->Ready) {
        return FALSE;
    }
    maxc = dev->MaxContainers;
    if (maxc == 0 || maxc > SRC_MAX_CONTAINERS) {
        maxc = SRC_MAX_CONTAINERS;
    }
    command = dev->NameServeCmd ? dev->NameServeCmd : VM_NameServe64;

    while (InterlockedCompareExchange(&dev->MgmtBusy, 1, 0) != 0) {
        StorPortStallExecution(20);
    }

    for (cid = 0; cid < maxc; cid++) {
        SRC_CONTAINER c;
        /* short probe timeout: this runs in the AIF timer DPC at DISPATCH, so a
         * slow/half-wedged firmware must not spin a full second per empty slot
         * (the init enum can afford 1000ms at PASSIVE; here we bound it to 100ms,
         * matching SrcRescanPhysical's 60ms philosophy). */
        ULONG   rc = SrcProbeContainer(dev, cid, command, &c, 100);
        BOOLEAN present, wasValid;

        if (rc == SRC_SYNC_BLKHINT) {
            /* variable/4K-sector container: re-probe this slot via AllBlk (see SrcEnumContainers) */
            rc = SrcProbeContainer(dev, cid, VM_NameServeAllBlk, &c, 100);
        }
        if (rc == SRC_SYNC_TIMEOUT) {
            /* ring/firmware desync: a stalled probe is NOT evidence a container is
             * gone, so stop here rather than risk unpresenting a healthy volume. */
            SRCDBG("srcraid: RESCAN cid=%u probe TIMEOUT, stopping scan\n", cid);
            break;
        }
        present  = (rc == SRC_SYNC_OK && c.Valid);
        wasValid = dev->Containers[cid].Valid;

        if (present) {
            /* preserve a maxView hide/show (IOCTL 0x42017) across the refresh - a fresh
             * probe doesn't know about it; a re-added slot starts visible (wasValid=FALSE). */
            BOOLEAN keepHidden = wasValid ? dev->Containers[cid].Hidden : FALSE;
            ULONG   keepUid    = wasValid ? dev->Containers[cid].CtUid  : 0;
            dev->Containers[cid] = c;           /* present: add or refresh (resets RescanMiss) */
            dev->Containers[cid].Hidden = keepHidden;
            dev->Containers[cid].CtUid  = wasValid ? keepUid : SrcGetContainerUid(dev, cid);
            if (!wasValid) {
                changed = TRUE;
                SRCDBG("srcraid: RESCAN new container %u type=%u size=%I64u blocks\n",
                       cid, c.Type, c.SizeBlocks);
            }
            found++;
        } else if (wasValid) {
            /* previously presented, now NOT cleanly present. A deleted container
             * reports back either a clean empty slot (rc=OK, vol=CT_NONE) OR - as the
             * firmware actually does, confirmed on hardware - an error status (rc!=OK).
             * A clean CT_NONE is authoritative => unpresent immediately. An error status
             * COULD be a transient glitch while the firmware is busy committing config,
             * so require two consecutive misses before unpresenting (protects a healthy
             * volume, e.g. D:, from a one-off probe failure during a create/delete). The
             * rescan burst runs enough passes to still drop a genuinely-deleted volume. */
            BOOLEAN authoritative = (rc == SRC_SYNC_OK);   /* clean empty slot */
            if (authoritative || ++dev->Containers[cid].RescanMiss >= 2) {
                dev->Containers[cid].Valid = FALSE;
                dev->Containers[cid].RescanMiss = 0;
                changed = TRUE;
                SRCDBG("srcraid: RESCAN container %u REMOVED (rc=%u ctNone=%u)\n",
                       cid, rc, authoritative ? 1u : 0u);
            } else {
                found++;                        /* keep this pass; confirm on the next */
                SRCDBG("srcraid: RESCAN container %u miss #%u (rc=%u), pending\n",
                       cid, (ULONG)dev->Containers[cid].RescanMiss, rc);
            }
        }
    }
    dev->NumValidContainers = found;

    InterlockedExchange(&dev->MgmtBusy, 0);
    return changed;
}

/* FW time-of-day sync (vendor SetArcsasFwTime): push host wall-clock seconds to the firmware at
 * most every ~15 min so FW-side event-log / SMART / enclosure timestamps stay aligned with the
 * host (the init struct sets it once; without this they drift over long uptime). Called from the
 * AIF timer at DISPATCH; takes MgmtBusy to serialize the slot-0 sync FIB against mgmt/rescan.
 * Informational only - a failure is non-fatal (we just retry at the next interval). */
#define SRC_FWTIME_SYNC_SECS 900u   /* vendor threshold 0x384 */
VOID
SrcSyncFwTime(_In_ PDEVICE_EXTENSION dev)
{
    ULONG elapsed = SrcElapsedSeconds(dev);
    ULONG rc;

    if (!dev->Ready || dev->AdapterFibs == NULL || elapsed == 0) {
        return;
    }
    if (dev->LastFwTimeSync != 0 && (elapsed - dev->LastFwTimeSync) < SRC_FWTIME_SYNC_SECS) {
        return;                         /* not due yet */
    }
    if (InterlockedCompareExchange(&dev->RestartPending, 0, 0) != 0) {
        return;                         /* don't poke the FW mid-resume */
    }
    while (InterlockedCompareExchange(&dev->MgmtBusy, 1, 0) != 0) {
        StorPortStallExecution(20);
    }
    rc = SrcFibSubmitSync(dev, (USHORT)FIBCMD_SetAdapterTime,
                          &elapsed, sizeof(elapsed), sizeof(elapsed), 1000);
    InterlockedExchange(&dev->MgmtBusy, 0);
    dev->LastFwTimeSync = elapsed;      /* attempted - next try is one interval out, not every tick */
    SRCDBG("srcraid: FW time sync (elapsed=%u) rc=%u\n", elapsed, rc);
}
