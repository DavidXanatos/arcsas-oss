/*
 * srcraid.c - Storport miniport for Adaptec/PMC Series 7 (SRC interface)
 * Phase 0 skeleton: valid DriverEntry + StorPortInitialize + stub callbacks that
 * compile, link and load. Hardware logic arrives in P1..P4 (see ../DESIGN.md).
 */
#include "srcraid.h"

/* Deferred firmware bring-up timer (defined with the restart path below). Shared by
 * ScsiRestartAdapter (resume) and HwInitialize (boot) so a slow/wedged firmware never
 * busy-waits at an elevated IRQL - the timer polls readiness and runs SrcBringUp once. */
#define SRC_RESTART_POLL_US    100000u   /* 100 ms between fw-ready polls */
#define SRC_RESTART_RESET_AT   50u       /* ~5 s of polls with fw down -> IOP reset kick */
#define SRC_RESTART_GIVEUP     600u      /* ~60 s of polls/retries -> give up, let DeadAdapter
                                          * boot the box degraded instead of hanging forever */
/* Debug-output master switch (see SRCDBG in srcraid.h). 0 = quiet (default); set from
 * HKLM\...\Services\srcraid\Parameters\DebugLevel at HwFindAdapter time. */
ULONG g_SrcDebugEnabled = 0;

/* Friendly controller name from the PCI device id (for startup logging). */
static const char *
SrcCardTypeName(_In_ USHORT deviceId)
{
    switch (deviceId) {
    case SRC_DID_SERIES6: return "Adaptec Series 6 (PMC SRC)";
    case SRC_DID_SERIES7: return "Adaptec Series 7 (ASR-71605 / Denali)";
    case SRC_DID_SERIES8: return "Adaptec Series 8 (PMC SRC)";
    default:              return "Adaptec SRC (unknown series)";
    }
}

static VOID SrcRestartTimer(_In_ PVOID DeviceExtension);

/* Windows system-event-log UniqueIds for controller faults we surface to Event Viewer, mirroring
 * the vendor's SendEvent2OS. Numbered like its EventIds so the two drivers' logs line up. */
#define SRC_OSEVT_BUS_RESET     0x00000027u  /* a bus reset was issued to the controller      */
#define SRC_OSEVT_ADAPTER_DEAD  0x0000002Eu  /* fail-fast: completion path dead, I/O failing   */
#define SRC_OSEVT_FW_NOT_UP     0x00000035u  /* firmware never came back up (resume or boot)   */

/* Record a controller fault in the Windows system event log. StorPortLogError tags it with the
 * driver + a standard storage message + our UniqueId for correlation (monitoring tools scrape
 * these); unlike maxView's in-band AIF stream it survives a reboot and needs no agent running. */
static VOID
SrcLogOsEvent(_In_ PDEVICE_EXTENSION dev, _In_ UCHAR pathId, _In_ ULONG errorCode, _In_ ULONG uniqueId)
{
    StorPortLogError(dev, NULL, pathId, 0xFF, 0xFF, errorCode, uniqueId);
}

/* ------------------------------------------------------------------ */
/* HwFindAdapter: StorPort calls once per candidate adapter. P1 fills in
 * PCI config read, BAR0 mapping, access ranges, MSI-X, uncached ext sizing.
 * For P0 we report "not found" so we never half-own a device while the real
 * vendor driver may still be installed on the test box.                */
ULONG
SrcFindAdapter(
    _In_ PVOID DeviceExtension,
    _In_ PVOID HwContext,
    _In_ PVOID BusInformation,
    _In_ PCHAR ArgumentString,
    _Inout_ PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    _Out_ PBOOLEAN Reserved3
    )
{
    PDEVICE_EXTENSION dev = (PDEVICE_EXTENSION)DeviceExtension;
    PCI_COMMON_CONFIG pci;
    ULONG got, i;

    UNREFERENCED_PARAMETER(HwContext);
    UNREFERENCED_PARAMETER(BusInformation);
    UNREFERENCED_PARAMETER(ArgumentString);

    if (Reserved3) {
        *Reserved3 = FALSE;
    }
    RtlZeroMemory(dev, sizeof(*dev));
    dev->AdapterState = SRC_STATE_INIT;

    /* Read the debug master switch BEFORE the first SRCDBG so startup logging honors it.
     * HKLM\SYSTEM\CurrentControlSet\Services\srcraid\Parameters\DebugLevel (DWORD), 0 = quiet. */
    {
        ULONG dbg = 0, dbgLen = sizeof(dbg);
        if (StorPortRegistryRead(dev, (PUCHAR)"DebugLevel", 1, MINIPORT_REG_DWORD,
                                 (PUCHAR)&dbg, &dbgLen) && dbgLen == sizeof(dbg)) {
            g_SrcDebugEnabled = dbg;
        }
    }

    SRCDBG( "srcraid: FindAdapter enter bus=%d slot=%d nAccessRanges=%d\n",
        ConfigInfo->SystemIoBusNumber, ConfigInfo->SlotNumber,
        ConfigInfo->NumberOfAccessRanges);

    /* --- identify via PCI config space --- */
    got = StorPortGetBusData(dev, PCIConfiguration,
                             ConfigInfo->SystemIoBusNumber,
                             (ULONG)(ULONG_PTR)ConfigInfo->SlotNumber,
                             &pci, sizeof(pci));
    SRCDBG( "srcraid: GetBusData got=%d VID=%x DID=%x subsys=%x:%x\n",
        got, pci.VendorID, pci.DeviceID, pci.u.type0.SubVendorID, pci.u.type0.SubSystemID);
    if (got < (ULONG)FIELD_OFFSET(PCI_COMMON_CONFIG, DeviceSpecific)) {
        SRCDBG( "srcraid: config read too short -> ERROR\n");
        return SP_RETURN_ERROR;
    }

    dev->VendorId    = pci.VendorID;
    dev->DeviceId    = pci.DeviceID;
    dev->RevisionId  = pci.RevisionID;
    dev->SubVendorId = pci.u.type0.SubVendorID;
    dev->SubSystemId = pci.u.type0.SubSystemID;

    if (dev->VendorId != SRC_VID ||
        (dev->DeviceId != SRC_DID_SERIES6 &&
         dev->DeviceId != SRC_DID_SERIES7 &&
         dev->DeviceId != SRC_DID_SERIES8)) {
        SRCDBG( "srcraid: not our device -> NOT_FOUND\n");
        return SP_RETURN_NOT_FOUND;
    }
    dev->IsSrcInterface = TRUE;

    /* --- map the SRC register window (first memory BAR >= 4 KB) --- */
    for (i = 0; i < ConfigInfo->NumberOfAccessRanges; i++) {
        PACCESS_RANGE ar = &((*ConfigInfo->AccessRanges)[i]);
        SRCDBG( "srcraid:  AR[%d] mem=%d io=%d start=%x:%08x len=0x%x\n",
            i, ar->RangeInMemory, !ar->RangeInMemory,
            ar->RangeStart.HighPart, ar->RangeStart.LowPart, ar->RangeLength);
        if (!dev->Regs && ar->RangeInMemory && ar->RangeLength >= 0x1000) {
            dev->Regs = (PUCHAR)StorPortGetDeviceBase(
                            dev, ConfigInfo->AdapterInterfaceType,
                            ConfigInfo->SystemIoBusNumber,
                            ar->RangeStart, ar->RangeLength, FALSE);
            SRCDBG( "srcraid:   GetDeviceBase -> %p\n", dev->Regs);
            if (dev->Regs) {
                dev->RegsPhys   = ar->RangeStart;
                dev->RegsLength = ar->RangeLength;
            }
        }
    }
    if (!dev->Regs) {
        SRCDBG( "srcraid: no register BAR mapped -> ERROR\n");
        return SP_RETURN_ERROR;
    }

    /* Validate the register map right here (window is mapped) - OMR0 should
     * read ~0x00010080 on a running Series 7. Confirms offsets without needing
     * HwInitialize (which StorPort isn't reaching yet). */
    SRCDBG( "srcraid: OMR0=0x%08x (expect ~0x00010080)\n", SrcFwStatus(dev));

    /* --- capabilities / limits reported to StorPort --- */
    dev->MaxTransferLength = 0x40000;   /* 256 KB; revisit with SG caps in P3 */
    dev->MaxFib            = SRC_MAX_FIB;
    dev->NumMsix           = SRC_NUM_MSIX;

    /* No artificial device limits: this controller exposes RAID containers AND
     * raw pass-through disks, and with a SAS expander, many drives. Report the
     * StorPort ceiling (255 targets/bus) across several buses; the real present
     * counts come from firmware enumeration in P3. */
    ConfigInfo->NumberOfBuses               = 8;
    ConfigInfo->MaximumNumberOfTargets      = 255;   /* StorPort per-bus max */
    ConfigInfo->MaximumNumberOfLogicalUnits = 8;
    ConfigInfo->MaximumTransferLength       = dev->MaxTransferLength;
    ConfigInfo->NumberOfPhysicalBreaks      = 64;
    ConfigInfo->Master                      = TRUE;
    ConfigInfo->ScatterGather               = TRUE;
    ConfigInfo->CachesData                  = TRUE;
    ConfigInfo->Dma64BitAddresses           = SCSI_DMA64_SYSTEM_SUPPORTED;
    ConfigInfo->AlignmentMask               = 0x3;
    ConfigInfo->MapBuffers                  = STOR_MAP_NON_READ_WRITE_BUFFERS;
    /* MSI-X path: full-duplex requires an explicit InterruptSynchronizationMode
     * (newer StorPort rejects full-duplex without it -> STATUS_INTERNAL_ERROR).
     * We use a single RRQ serviced by one ISR, so synchronize all messages. */
    ConfigInfo->SynchronizationModel        = StorSynchronizeFullDuplex;
    ConfigInfo->InterruptSynchronizationMode = InterruptSynchronizeAll;
    /* MSI-X ISR: StorPort delivers each MSI-X message to this routine (with its MessageId).
     * Registered here (a PORT_CONFIGURATION_INFORMATION field, not in HW_INITIALIZATION_DATA).
     * Harmless under INTx - StorPort just uses HwInterrupt then. */
    ConfigInfo->HwMSInterruptRoutine        = SrcMsInterrupt;

    SRCDBG(
        "srcraid: CARD %s\n"
        "srcraid:   PCI VEN_%04X DEV_%04X SUBSYS_%04X%04X REV_%02X  driver build 0.1.0.%d\n"
        "srcraid:   regs=%p len=0x%x OMR0=0x%08x (firmware version logged after bring-up)\n",
        SrcCardTypeName(dev->DeviceId),
        dev->VendorId, dev->DeviceId, dev->SubSystemId, dev->SubVendorId, dev->RevisionId,
        VER_BUILD, dev->Regs, dev->RegsLength, SrcFwStatus(dev));

    /* P2: allocate the DMA comm area now (ConfigInfo is only available here).
     * The firmware handshake (build + push AAC_INIT) happens in HwInitialize. */
    {
        ULONG ba = SrcCommAlloc(dev, ConfigInfo);
        SRCDBG("srcraid: SrcCommAlloc -> %u\n", ba);
    }
    return SP_RETURN_FOUND;
}

/* HwInitialize: bring the adapter to a running state (soft reset, AAC_INIT,
 * start comm). P1/P2. */
/*
 * Enable StorPort completion-perf optimizations (once). DPC_REDIRECTION redirects each
 * completion DPC to the CPU that submitted the request, so completion processing spreads
 * across CPUs (and stops bouncing the originating CPU's cache) instead of piling onto the
 * single interrupt CPU - a real win under QD even on INTx (one ISR, DPCs fan out). Query
 * first for the supported set, then set the subset we want. Harmless if unsupported.
 */
static VOID
SrcInitPerfOpts(_In_ PDEVICE_EXTENSION dev)
{
    PERF_CONFIGURATION_DATA perf;
    ULONG st, supported, want;

    if (dev->PerfInitDone) {
        return;
    }
    RtlZeroMemory(&perf, sizeof(perf));
    perf.Version = STOR_PERF_VERSION;
    perf.Size    = sizeof(perf);
    st = StorPortInitializePerfOpts(dev, TRUE, &perf);          /* query supported */
    if (st != STOR_STATUS_SUCCESS) {
        SRCDBG("srcraid: PerfOpts query failed 0x%x\n", st);
        return;
    }
    supported = perf.Flags;
    want = STOR_PERF_DPC_REDIRECTION & supported;               /* single-queue HBA: just DPC redirect */

    RtlZeroMemory(&perf, sizeof(perf));
    perf.Version = STOR_PERF_VERSION;
    perf.Size    = sizeof(perf);
    perf.Flags   = want;
    st = StorPortInitializePerfOpts(dev, FALSE, &perf);         /* apply */
    SRCDBG("srcraid: PerfOpts supported=0x%x want=0x%x apply=0x%x\n", supported, want, st);
    if (st == STOR_STATUS_SUCCESS) {
        dev->PerfInitDone = TRUE;
    }
}

/*
 * Comm bring-up: negotiate properties, push AAC_INIT, enumerate containers,
 * commit config, enable interrupts. Runs at PASSIVE_LEVEL (from SrcPassiveInit)
 * after the firmware reset, or directly at DISPATCH if passive init is
 * unavailable (then without the reset). Sets dev->Ready on success.
 */
static VOID
SrcBringUp(
    _In_ PDEVICE_EXTENSION dev
    )
{
    ULONG rc, st = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0;
    BOOLEAN ok = FALSE;             /* set only when comm bring-up fully succeeds */

    /* P2 sync-channel self-test: send a safe read-only query. If it completes
     * (rc=0, not timeout), the mailbox + doorbell protocol is correct on hardware
     * and we can build the comm area / push AAC_INIT next. The returned status
     * tells us whether the firmware understood the opcode. */
    rc = SrcSyncCommand(dev, GET_ADAPTER_PROPERTIES,
                        0, 0, 0, 0, 0, 0,
                        &st, &r1, &r2, &r3, &r4, 500);
    SRCDBG("srcraid: GET_ADAPTER_PROPERTIES rc=%u status=0x%08x opt=0x%08x "
           "r2=0x%08x doorbell=0x%08x r4=0x%08x\n", rc, st, r1, r2, r3, r4);
    dev->AdapterOptions = r1;   /* DIAG: bit 30 (0x40000000) == firmware V8 support (per-vector gate) */

    /* decode the comm model from the option flags (status[1]) */
    dev->CommType = AAC_COMM_PRODUCER;
    dev->RawIo64  = 0;
    if (rc == SRC_SYNC_OK && st == 1 && (r1 & AAC_OPT_NEW_COMM)) {
        dev->RawIo64 = (r1 & AAC_OPT_NEW_COMM_64) ? 1 : 0;
        if      (r1 & AAC_OPT_NEW_COMM_TYPE1) dev->CommType = AAC_COMM_MESSAGE_TYPE1;
        else if (r1 & AAC_OPT_NEW_COMM_TYPE2) dev->CommType = AAC_COMM_MESSAGE_TYPE2;
        else if (r1 & AAC_OPT_NEW_COMM_TYPE3) dev->CommType = AAC_COMM_MESSAGE_TYPE3;
        else                                  dev->CommType = AAC_COMM_MESSAGE;
    }

    /* negotiate comm sizes (status: [1]=max cmd KB<<16|max fib; [2]=sg; [3]=can_queue<<16) */
    {
        ULONG ps = 0, c1 = 0, c2 = 0, c3 = 0, c4 = 0, rc2;
        rc2 = SrcSyncCommand(dev, GET_COMM_PREFERRED_SETTINGS,
                             0, 0, 0, 0, 0, 0, &ps, &c1, &c2, &c3, &c4, 500);
        if (rc2 == SRC_SYNC_OK && ps == 1) {
            dev->MaxFibSize     = c1 & 0xFFE0;          /* multiple of 32 */
            dev->MaxSectors     = (c1 >> 16) << 1;
            dev->SgTableSize    = c2 & 0xFFFF;
            dev->MaxOutstanding = (c3 >> 16) - AAC_NUM_MGT_FIB;   /* SRC: high word */
            dev->MaxNumAif      = c4 & 0xFFFF;
        }
        SRCDBG("srcraid: COMM_PREFERRED rc=%u status=0x%08x -> commType=%u raw64=%u "
               "maxFib=%u maxSectors=%u sg=%u outstanding=%u aif=%u\n",
               rc2, ps, dev->CommType, dev->RawIo64, dev->MaxFibSize, dev->MaxSectors,
               dev->SgTableSize, dev->MaxOutstanding, dev->MaxNumAif);
    }

    /* MSI-X decision - MUST happen BEFORE SrcCommStart builds the AAC_INIT struct, because
     * no_of_msix_vectors is baked into that struct at push time (there is NO runtime toggle;
     * the toggle poisoned completions on 0.1.0.156-162). The firmware then splits the host RRQ
     * into N per-vector regions. Single-vector (N=1) does NOT trigger firmware MSI-X delivery
     * (0.1.0.164), so N = CPU count (vendor: SystemProcessorNumber), capped at SRC_MAX_VECTORS.
     * The DeadAdapter fail-fast makes a bad attempt boot degraded + SSH-recoverable. */
#ifndef SRCRAID_TRY_MSIX
#define SRCRAID_TRY_MSIX 1    /* 1 = attempt multi-vector MSI-X; 0 = INTx (stable) */
#endif
#if SRCRAID_TRY_MSIX
    {
        ULONG cpus = (ULONG)KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
        if (cpus < 1) cpus = 1;
        if (cpus > SRC_MAX_VECTORS) cpus = SRC_MAX_VECTORS;
        dev->NumMsix    = cpus;        /* N vectors = CPU count (capped) */
        dev->MsiEnabled = TRUE;        /* firmware comes up MSI-X from the init struct + PCI cap */
        SRCDBG("srcraid: MSI-X attempt: NumMsix=%u (cpus capped to %u)\n", cpus, SRC_MAX_VECTORS);
    }
#endif

    /* P2: build the AAC_INIT struct and push it to the firmware. On success the
     * comm interface is live; container enumeration + I/O is P3. */
    if (dev->CommArea != NULL && dev->CommType == AAC_COMM_MESSAGE_TYPE2) {
        ULONG bs = SrcCommStart(dev);
        SRCDBG("srcraid: SrcCommStart -> %u  OMR0=0x%08x\n", bs, SrcFwStatus(dev));
        if (bs == SRC_BRINGUP_OK) {
            /* P3: prove the new-comm interface is live by round-tripping one FIB, then
             * enumerate containers (logical volumes). */
            ULONG cnt = 0, ft, cc;
            ft = SrcFibSelfTest(dev, 1000);
            SRCDBG("srcraid: SrcFibSelfTest -> %u\n", ft);
            SrcWaitForConfigReady(dev);   /* M2: let the FW finish computing its config first */
            cc = SrcGetContainerCount(dev, &cnt);
            SRCDBG("srcraid: SrcGetContainerCount -> %u  maxContainers=%u\n", cc, cnt);
            SrcCommitConfig(dev);         /* bring pending config online for I/O */
            if (cc == SRC_SYNC_OK) {
                dev->MaxContainers = cnt;
                SrcEnumContainers(dev);   /* probe each slot, fill dev->Containers */
            }
            SrcIoSelfTest(dev);           /* diag: read LBA0 into comm-area SG (RRQ poll) */
            dev->ExposePhysicals = TRUE;  /* TODO: read ExposeNonDASD registry param */
#if !SRC_TRANSPARENT_RELAY
            SrcEnumPhysical(dev);         /* legacy: driver-side INQUIRY sweep -> compacted bus 1 */
#endif
            /* transparent relay: no pre-sweep. StorPort probes bus SRC_PHYSICAL_BUS and we relay
             * each target to the firmware (SrcPhysExecute), so the physical set enumerates itself. */

            /* MSI-X was decided + baked into the init struct above (before SrcCommStart). Here we
             * just unmask: SrcIoEnableInterrupts picks the MSI-X mask when MsiEnabled, else INTx. */
            SrcIoEnableInterrupts(dev);   /* MSI-X mask if MsiEnabled, else INTx */
            SrcInitPerfOpts(dev);         /* DPC redirection: spread completion DPCs across CPUs */
            SRCDBG("srcraid: interrupts enabled (OIMR=0x%08x) - ready for I/O\n",
                   SrcRead32(dev, SRC_REG_OIMR));
            ok = TRUE;            /* comm + enum + interrupts all up */
        }
    } else {
        SRCDBG("srcraid: skipping comm start (commArea=%p commType=%u)\n",
               dev->CommArea, dev->CommType);
    }
    /* Only report Ready when bring-up actually succeeded. Setting Ready=TRUE
     * unconditionally (the old behavior) masked a failed comm-start / sync-timeout
     * as a healthy adapter: StorPort would complete PnP, I/O would hang or report
     * no volumes, and maxView's mgmt passthrough (which checks only dev->Ready)
     * would post to an inbound queue the firmware never services -> permanent hang.
     * On failure leave Ready=FALSE so HwStartIo returns BUSY (StorPort retries) and
     * the failure is visible instead of silently masked. */
    dev->AdapterState = ok ? SRC_STATE_RUNNING : SRC_STATE_INIT;
    dev->Ready = ok;
    if (ok) {
        SrcAifInit(dev);      /* arm the adapter-event (AIF) channel */
    } else {
        SRCDBG("srcraid: BRING-UP FAILED - Ready stays FALSE (no AIF arm; I/O returns BUSY)\n");
    }
}

/*
 * PASSIVE_LEVEL init routine (StorPortEnablePassiveInitialization): reset the
 * firmware to a clean baseline - the ~60 s KERNEL_BOOTING wait is safe here -
 * then run the comm bring-up. Called once, after SrcInitialize returns.
 */
BOOLEAN
SrcPassiveInit(
    _In_ PVOID DeviceExtension
    )
{
    PDEVICE_EXTENSION dev = (PDEVICE_EXTENSION)DeviceExtension;

    SRCDBG("srcraid: passive init (PASSIVE_LEVEL) - resetting firmware\n");
    SrcIopReset(dev);
    SrcBringUp(dev);
    return TRUE;
}

BOOLEAN
SrcInitialize(
    _In_ PVOID DeviceExtension
    )
{
    PDEVICE_EXTENSION dev = (PDEVICE_EXTENSION)DeviceExtension;

    SrcSetupMailbox(dev);
    SRCDBG("srcraid: HwInitialize dev=%04x:%04x OMR0=0x%08x regs=%p mbox=%p\n",
           dev->VendorId, dev->DeviceId, SrcFwStatus(dev), dev->Regs, dev->Mailbox);

    /* The firmware's POST state is the good baseline: a full IOP reset here
     * breaks the FSA/SEND_SYNCHRONOUS_FIB path (enumeration fails) and queues a
     * stray AIF, so we do NOT reset.
     *
     * Init-time robustness: normally BIOS has POSTed the controller before the driver
     * loads, so OMR0 shows UP_AND_RUNNING and we bring up the comm interface directly
     * (the proven fast path). If the firmware is NOT ready yet (slow/cold POST), DON'T
     * busy-wait here - HwInitialize can run at DISPATCH; hand it to the same deferred
     * restart timer the resume path uses, which polls readiness one register-read per
     * tick and kicks a wedged fw with a non-blocking IOP reset. I/O returns BUSY until
     * the deferred bring-up completes. */
    {
        ULONG status = SrcFwStatus(dev);
        if (status != 0xFFFFFFFFu && (status & FW_UP_AND_RUNNING)) {
            SrcBringUp(dev);                       /* fw ready (normal boot) */
        } else {
            SRCDBG("srcraid: HwInitialize - fw not up (OMR0=0x%08x), deferring bring-up to timer\n",
                   status);
            dev->RestartAttempts = 0;
            InterlockedExchange(&dev->RestartPending, 1);
            StorPortNotification(RequestTimerCall, dev, SrcRestartTimer, SRC_RESTART_POLL_US);
        }
    }
    return TRUE;
}

/* HwBuildIo: pre-process an SRB at DISPATCH (build the FIB/SG list). P3. */
BOOLEAN
SrcBuildIo(
    _In_ PVOID DeviceExtension,
    _In_ PSCSI_REQUEST_BLOCK Srb
    )
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    UNREFERENCED_PARAMETER(Srb);
    return TRUE;  /* allow StartIo to run */
}

/* HwStartIo: submit the request to the controller. P3 pushes a FIB to the
 * SRC inbound queue; for P0 every request is completed as an error. */
BOOLEAN
SrcStartIo(
    _In_ PVOID DeviceExtension,
    _In_ PSCSI_REQUEST_BLOCK Srb
    )
{
    PDEVICE_EXTENSION dev = (PDEVICE_EXTENSION)DeviceExtension;
    BOOLEAN complete = TRUE;

    switch (Srb->Function) {
    case SRB_FUNCTION_EXECUTE_SCSI:
        if (dev->DeadAdapter) {
            Srb->SrbStatus = SRB_STATUS_SELECTION_TIMEOUT;  /* completion path dead; fail (no retry) */
            break;
        }
        if (!dev->Ready) {
            Srb->SrbStatus = SRB_STATUS_BUSY;  /* bring-up not finished; retry */
            break;
        }
        complete = SrcScsiExecute(dev, Srb);   /* FALSE => queued async (Step B) */
        break;
    case SRB_FUNCTION_FLUSH:
    case SRB_FUNCTION_FLUSH_QUEUE:
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        break;
    case SRB_FUNCTION_SHUTDOWN:
        /* M1: flush the controller write-back cache before the OS cuts power (vendor sends
         * VM_CloseAll on stop) instead of faking success. The OS already SYNCHRONIZE_CACHE'd
         * the mounted volumes; this is the controller-wide backstop for anything it didn't.
         * Bounded, best-effort, terminal (see SrcFlushAllContainers). */
        if (dev->Ready && !dev->DeadAdapter) {
            SrcFlushAllContainers(dev);
        }
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        break;
    case SRB_FUNCTION_IO_CONTROL:
        complete = SrcIoctl(dev, Srb);   /* maxView/arcconf management interface */
        break;
    case SRB_FUNCTION_PNP:
    case SRB_FUNCTION_POWER:
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        break;
    case SRB_FUNCTION_WMI:
        /* srcraid implements no WMI data blocks. Acking SUCCESS with an UNFILLED WNODE made the
         * WMI subsystem register/free a garbage buffer -> intermittent heap-corruption bugcheck
         * (0x7E: nt!WmipRegisterOrUpdateDS -> ExFreePool -> GP fault). Decline cleanly instead so
         * WMI marks the storage provider unavailable rather than parsing stale bytes. maxView and
         * arcconf use the AACAPI IOCTL path (SRB_FUNCTION_IO_CONTROL), not WMI, so nothing is lost. */
        Srb->DataTransferLength = 0;
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        break;
    default:
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        break;
    }

    if (complete) {
        StorPortNotification(RequestComplete, dev, Srb);
    }
    return TRUE;
}

/* HwInterrupt: MSI-X ISR - drain the outbound response queue, complete SRBs. P3. */
BOOLEAN
SrcInterrupt(
    _In_ PVOID DeviceExtension
    )
{
    PDEVICE_EXTENSION dev = (PDEVICE_EXTENSION)DeviceExtension;

    if (!dev->InterruptsEnabled) {
        return FALSE;
    }
    dev->DrainVec = -1;          /* INTx: drain all regions (the single ring) */
    return SrcIoDrainRrq(dev);   /* drain host RRQ, complete finished SRBs */
}

/* Abandon every outstanding async I/O slot with the given SRB status. Used on
 * power-down (firmware state is about to be lost), bus reset, and before a
 * restart re-init (stale in-flight FIBs must not be mistaken for completions). */
static VOID
SrcCompleteAllOutstanding(
    _In_ PDEVICE_EXTENSION dev,
    _In_ UCHAR srbStatus
    )
{
    ULONG slot;

    for (slot = SRC_IO_SLOT_MIN; slot < SRC_NUM_IO_SLOTS; slot++) {
        PSCSI_REQUEST_BLOCK srb;
        if (slot == SRC_AIF_SLOT) {
            continue;                 /* reserved AIF slot is a pinned sentinel, not an SRB */
        }
        srb = (PSCSI_REQUEST_BLOCK)InterlockedExchangePointer(&dev->IoSlotSrb[slot], NULL);
        if (srb != NULL) {
            srb->SrbStatus = srbStatus;
            StorPortNotification(RequestComplete, dev, srb);
        }
    }
    /* every outstanding request is now aborted -> no raw-SRB holds a >1KB pool buffer; clear the
     * whole bitmap so an aborted passthrough can't leak one (ISR frees them on normal completion). */
    dev->RawBounceBusy = 0;
}

/* Quiesce for power-down (ScsiStopAdapter): stop accepting I/O, mask the
 * outbound interrupt, and abandon anything in flight. The firmware loses state
 * on S3 (power cut) / S4 (hibernate), so there is nothing to "save" - we just
 * make sure StorPort sees a clean stop and the ISR goes silent. */
static VOID
SrcQuiesce(
    _In_ PDEVICE_EXTENSION dev
    )
{
    dev->Ready = FALSE;
    dev->InterruptsEnabled = FALSE;
    SrcWrite32(dev, SRC_REG_OIMR, 0xFFFFFFFFu);   /* mask all outbound interrupts */
    (void)SrcRead32(dev, SRC_REG_OIMR);
    dev->Oimr = 0xFFFFFFFFu;
    SrcCompleteAllOutstanding(dev, SRB_STATUS_BUS_RESET);
}

/*
 * Full recovery after a power transition (ScsiRestartAdapter) - THE reason this
 * driver exists. The vendor arcsas armed its resume re-init only when a pause
 * flag was already cleared (by ScsiSetRunningConfig); if StorPort called
 * ScsiRestartAdapter first, re-init never ran -> dead comm -> lockup. We re-init
 * the firmware comm interface UNCONDITIONALLY.
 *
 * CRITICAL IRQL NOTE (measured on hardware, 0.1.0.141 trace): StorPort invokes
 * HwAdapterControl/ScsiRestartAdapter SYNCHRONIZED WITH THE HW INTERRUPT (because of
 * InterruptSynchronizeAll + StorSynchronizeFullDuplex) - at a DEVICE IRQL (= 8), ABOVE
 * DISPATCH_LEVEL. So this callback must NOT busy-wait for the firmware: the old inline
 * SrcWaitForFwReady(.,30000) + SrcBringUp would, on a slow firmware re-POST, spin for
 * seconds at IRQL 8 and hard-hang the CPU (there is no DPC watchdog above DISPATCH to
 * recover it). In practice the fw re-POSTs during BIOS/early-resume so it was already
 * up (OMR0=0x80) by the time this ran and the wait returned instantly - which is why it
 * worked - but the latent hang defeats the driver's whole purpose.
 *
 * FIX: do only the quick, non-blocking cleanup here, then hand the fw-ready wait + the
 * heavy bring-up to SrcRestartTimer (a StorPort timer at DISPATCH). The timer polls
 * readiness one register read per tick and runs SrcBringUp exactly ONCE, after the fw
 * reports UP_AND_RUNNING - so nothing ever busy-waits at the elevated callback IRQL.
 */
static VOID
SrcRestartTimer(
    _In_ PVOID DeviceExtension
    )
{
    PDEVICE_EXTENSION dev = (PDEVICE_EXTENSION)DeviceExtension;
    ULONG status;

    if (InterlockedCompareExchange(&dev->RestartPending, 0, 0) == 0) {
        return;                       /* restart already finished (or never armed) - stop */
    }

    status = SrcFwStatus(dev);        /* single non-blocking register read */
    if (status != 0xFFFFFFFFu && (status & FW_UP_AND_RUNNING)) {
        /* firmware is up: run the one-shot bring-up (re-negotiate, AAC_INIT, re-enum,
         * re-enable IRQ, re-arm AIF). SrcBringUp sets dev->Ready on success and arms
         * the AIF timer (which becomes the live periodic timer); we clear the pending
         * flag and do NOT re-arm the restart timer. SrcBringUp runs at DISPATCH here -
         * safe because we only reach it once the fw is responsive, so its sync-mailbox
         * steps complete quickly (unlike a blind 30 s wait). */
        SrcBringUp(dev);
        if (dev->Ready) {
            dev->RestartAttempts = 0;
            dev->WedgeRecoveries = 0;     /* recovered cleanly - refresh the wedge-recovery budget */
            InterlockedExchange(&dev->RestartPending, 0);   /* success: stop the restart timer */
            SRCDBG("srcraid: deferred restart bring-up done - Ready=1 OMR0=0x%08x\n",
                   SrcFwStatus(dev));
            return;
        }
        /* RETRY (the vendor driver re-arms + re-runs init until it settles):
         * the FW set UP_AND_RUNNING but wasn't yet ready for config FIBs, so bring-up timed out.
         * Keep RestartPending set, re-arm, and kick an IOP reset periodically. Without this one
         * transient failure left I/O BUSY forever. Give up only after SRC_RESTART_GIVEUP so a
         * genuinely dead FW still lets the box boot degraded (DeadAdapter) rather than hang. */
        SRCDBG("srcraid: deferred restart bring-up FAILED (Ready=0 OMR0=0x%08x) attempt %u - retrying\n",
               SrcFwStatus(dev), dev->RestartAttempts);
        if (++dev->RestartAttempts >= SRC_RESTART_GIVEUP) {
            InterlockedExchange(&dev->RestartPending, 0);   /* give up -> DeadAdapter path */
            SrcLogOsEvent(dev, 0, SP_BAD_FW_ERROR, SRC_OSEVT_FW_NOT_UP);
            SRCDBG("srcraid: deferred restart GAVE UP after %u attempts - degraded\n", dev->RestartAttempts);
            return;
        }
        if ((dev->RestartAttempts % SRC_RESTART_RESET_AT) == 0) {
            SrcIopResetTrigger(dev);     /* kick a stuck FW, then keep polling */
        }
        StorPortNotification(RequestTimerCall, dev, SrcRestartTimer, SRC_RESTART_POLL_US);
        return;
    }

    /* firmware not up yet: after a while assume a wedge and kick it with a NON-blocking
     * IOP reset (the per-tick poll then detects the post-reset KERNEL_UP), and keep
     * polling. This also covers the audit's "wedge detected via OMR0 only" case. */
    if (++dev->RestartAttempts == SRC_RESTART_RESET_AT) {
        SRCDBG("srcraid: deferred restart - fw still down after %u polls, IOP reset trigger\n",
               dev->RestartAttempts);
        SrcIopResetTrigger(dev);
    }
    if (dev->RestartAttempts >= SRC_RESTART_GIVEUP) {
        InterlockedExchange(&dev->RestartPending, 0);       /* fw never came up -> give up, degraded */
        SrcLogOsEvent(dev, 0, SP_BAD_FW_ERROR, SRC_OSEVT_FW_NOT_UP);
        SRCDBG("srcraid: deferred restart - fw never came up after %u polls, GIVING UP\n", dev->RestartAttempts);
        return;
    }
    StorPortNotification(RequestTimerCall, dev, SrcRestartTimer, SRC_RESTART_POLL_US);
}

static BOOLEAN
SrcAdapterRestart(
    _In_ PDEVICE_EXTENSION dev
    )
{
    SRCDBG("srcraid: ScsiRestartAdapter - DEFERRING re-init (IRQL=%u FwLostOnPower=%u OMR0=0x%08x)\n",
           (ULONG)KeGetCurrentIrql(), dev->FwLostOnPower, SrcFwStatus(dev));

    dev->Ready = FALSE;

    /* quick, NON-BLOCKING cleanup only (see the IRQL note above): drop any pre-sleep
     * in-flight I/O, wipe the host comm ring so stale entries can't look like
     * completions once the fw re-POSTs, and re-point the mailbox. */
    SrcCompleteAllOutstanding(dev, SRB_STATUS_BUS_RESET);
    if (dev->HostRrq != NULL && dev->HostRrqEntries != 0) {
        RtlZeroMemory((PVOID)dev->HostRrq, dev->HostRrqEntries * sizeof(ULONG));
    }
    RtlZeroMemory(dev->HostRrqIdx, sizeof(dev->HostRrqIdx));  /* SrcBringUp re-splits the regions */
    dev->NextSlotHint = 0;
    /* clear the fail-fast state carried in from before the power transition - a fresh resume
     * must start with a clean counter so a pre-sleep reset burst can't pre-trip DeadAdapter. */
    dev->ResetCount  = 0;
    dev->DeadAdapter = FALSE;
    SrcSetupMailbox(dev);

    /* hand the fw-ready wait + full bring-up to the restart timer (DISPATCH). Ready
     * stays FALSE until it finishes: HwStartIo returns BUSY (StorPort retries) and the
     * mgmt/rescan paths bail on !Ready, so nothing touches the mailbox meanwhile. */
    dev->RestartAttempts = 0;
    dev->FwLostOnPower   = FALSE;
    InterlockedExchange(&dev->RestartPending, 1);
    StorPortNotification(RequestTimerCall, dev, SrcRestartTimer, SRC_RESTART_POLL_US);

    SRCDBG("srcraid: ScsiRestartAdapter - re-init deferred to timer (returning promptly)\n");
    return TRUE;
}

/* StorPortSynchronizeAccess thunk: run SrcCompleteAllOutstanding SERIALIZED with the completion
 * ISR. HwResetBus runs at DISPATCH on another CPU while HwInterrupt can be mid-copy on the same
 * slot (full-duplex: StorPort does NOT serialize them). Without this, the reset's abort could
 * exchange a slot to NULL + complete the SRB while the ISR is copying the firmware reply into
 * that same SRB -> double completion / use-after-free -> bugcheck. Running the abort inside the
 * interrupt lock closes the window. (Power-down/restart don't need this: the FW has lost state,
 * so no concurrent firmware completions exist there.) */
static BOOLEAN
SrcAbortAllSync(_In_ PVOID DeviceExtension, _In_ PVOID Context)
{
    SrcCompleteAllOutstanding((PDEVICE_EXTENSION)DeviceExtension, (UCHAR)(ULONG_PTR)Context);
    return TRUE;
}

/* HwResetBus: abort outstanding I/O so StorPort can retry. Runs at DISPATCH, so
 * no firmware reset here (that can take seconds) - just clear the slots. */
BOOLEAN
SrcResetBus(
    _In_ PVOID DeviceExtension,
    _In_ ULONG PathId
    )
{
    PDEVICE_EXTENSION dev = (PDEVICE_EXTENSION)DeviceExtension;

    UNREFERENCED_PARAMETER(PathId);
    SRCDBG("srcraid: HwResetBus - aborting outstanding I/O (OMR0=0x%08x) reset#%d\n",
           SrcFwStatus(dev), dev->ResetCount + 1);
    StorPortSynchronizeAccess(dev, SrcAbortAllSync, (PVOID)(ULONG_PTR)SRB_STATUS_BUS_RESET);
    /* Resume guard (mirrors the vendor's "return TRUE immediately while the FW is doing an IOP reset"):
     * during a deferred resume/restart the FW is doing a cold re-POST (tens of seconds) while
     * Ready=FALSE, so NO completion drains and the fail-fast counter can't self-clear. Without
     * this, StorPort's pre-sleep SRB timeouts escalate to ~12 HwResetBus calls and brick a
     * card that is merely resuming slowly - the exact failure this driver exists to prevent. */
    if (InterlockedCompareExchange(&dev->RestartPending, 0, 0) != 0) {
        return TRUE;
    }
    /* a genuine (non-resume) bus reset is a real fault (StorPort calls us after an SRB timed out) -
     * record it so it's visible after a reboot */
    SrcLogOsEvent(dev, (UCHAR)PathId, SP_REQUEST_TIMEOUT, SRC_OSEVT_BUS_RESET);
    /* Fail-fast: if we keep getting reset with NO completion draining in between (ISR resets
     * ResetCount=0 on any completion), the completion path is dead - e.g. a bad MSI-X config
     * where interrupts never arrive. Mark the adapter dead so HwStartIo fails I/O immediately
     * and StorPort fails the device, instead of retrying forever and DEADLOCKING PnP at boot. */
    if (InterlockedIncrement(&dev->ResetCount) >= SRC_RESET_DEAD_LIMIT && !dev->DeadAdapter) {
        ULONG   panic  = 0;
        ULONG   status = SrcCheckHealth(dev, &panic);    /* H3: OMR0 health (panic/blink code) */
        BOOLEAN fwWedged = (status == 0xFFFFFFFFu) ||
                           !(status & FW_UP_AND_RUNNING) ||
                           (status & FW_KERNEL_PANIC);

        /* C2: a runtime firmware wedge is recoverable - the vendor (ResetAdapter) escalates
         * HwResetBus to an IOP reset + re-init. If OMR0 shows the FW itself is wedged (panicked /
         * not up / off-bus), hand it to the SAME non-blocking deferred restart the resume path
         * uses instead of going permanently dead. If the FW is UP but completions simply aren't
         * draining (e.g. a broken interrupt path), an IOP reset won't help - fall through to the
         * DeadAdapter fast-fail so the box boots/runs degraded rather than retrying forever. */
        if (fwWedged && dev->WedgeRecoveries < SRC_WEDGE_RECOVER_MAX &&
            InterlockedCompareExchange(&dev->RestartPending, 0, 0) == 0) {
            dev->WedgeRecoveries++;
            dev->Ready = FALSE;
            dev->RestartAttempts = 0;
            SrcLogOsEvent(dev, (UCHAR)PathId, SP_BAD_FW_ERROR, SRC_OSEVT_FW_NOT_UP);
            SRCDBG("srcraid: *** FW WEDGE *** OMR0=0x%08x panic=0x%x - deferred IOP reset + "
                   "re-bring-up (recovery %u/%u)\n",
                   status, panic, dev->WedgeRecoveries, SRC_WEDGE_RECOVER_MAX);
            InterlockedExchange(&dev->RestartPending, 1);
            StorPortNotification(RequestTimerCall, dev, SrcRestartTimer, SRC_RESTART_POLL_US);
            return TRUE;
        }

        dev->DeadAdapter = TRUE;
        dev->Ready = FALSE;
        SrcLogOsEvent(dev, (UCHAR)PathId, SP_INTERNAL_ADAPTER_ERROR, SRC_OSEVT_ADAPTER_DEAD);
        SRCDBG("srcraid: *** ADAPTER DEAD *** %d resets w/o a completion (OMR0=0x%08x) - failing "
               "I/O fast (box boots/runs degraded instead of hanging)\n",
               SRC_RESET_DEAD_LIMIT, status);
    }
    return TRUE;
}

/* HwAdapterControl: power / pnp. The reason this driver exists lives in the
 * ScsiRestartAdapter case (P4): full reset + comm re-init + StorPort resume. */
SCSI_ADAPTER_CONTROL_STATUS
SrcAdapterControl(
    _In_ PVOID DeviceExtension,
    _In_ SCSI_ADAPTER_CONTROL_TYPE ControlType,
    _In_ PVOID Parameters
    )
{
    PDEVICE_EXTENSION dev = (PDEVICE_EXTENSION)DeviceExtension;

    switch (ControlType) {
    case ScsiQuerySupportedControlTypes: {
        PSCSI_SUPPORTED_CONTROL_TYPE_LIST list =
            (PSCSI_SUPPORTED_CONTROL_TYPE_LIST)Parameters;
        ULONG i;
        static const BOOLEAN supported[] = {
            TRUE,  /* ScsiQuerySupportedControlTypes */
            TRUE,  /* ScsiStopAdapter   */
            TRUE,  /* ScsiRestartAdapter*/
            TRUE,  /* ScsiSetBootConfig */
            TRUE,  /* ScsiSetRunningConfig */
        };
        for (i = 0; i < list->MaxControlType; i++) {
            if (i < RTL_NUMBER_OF(supported)) {
                list->SupportedTypeList[i] = supported[i];
            }
        }
        break;
    }
    case ScsiStopAdapter:
        /* System going to sleep/hibernate (or driver stopping): quiesce and
         * remember the firmware will need a full re-init on restart. */
        SRCDBG("srcraid: ScsiStopAdapter - quiescing (OMR0=0x%08x)\n", SrcFwStatus(dev));
        SrcQuiesce(dev);
        dev->FwLostOnPower = TRUE;
        dev->AdapterState  = SRC_STATE_PAUSED;
        break;

    case ScsiRestartAdapter:
        /* Resume from sleep/hibernate: re-init the firmware comm UNCONDITIONALLY
         * (this is the vendor's resume-lockup bug - it re-init only behind a
         * pause-flag guard). See SrcAdapterRestart. */
        (void)SrcAdapterRestart(dev);
        dev->AdapterState = dev->Ready ? SRC_STATE_RUNNING : SRC_STATE_INIT;
        break;

    default:
        break;
    }
    return ScsiAdapterControlSuccess;
}

/* ------------------------------------------------------------------ */
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    HW_INITIALIZATION_DATA hwInitData;

    /* First line in the driver: goes straight to the kernel debugger (not via
     * StorPort), so it proves the driver image loaded and shows which build. */
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "srcraid: DriverEntry (build 0.1.0.%d)\n", VER_BUILD);

    /* Read the debug master switch HERE (robust): StorPortRegistryRead in HwFindAdapter proved
     * unreliable, so read HKLM\SYSTEM\CurrentControlSet\Services\srcraid\Parameters\DebugLevel
     * (DWORD, 0=quiet) via RtlQueryRegistryValues before any gated SRCDBG runs. */
    {
        RTL_QUERY_REGISTRY_TABLE qtab[2];
        ULONG dbg = 0;
        RtlZeroMemory(qtab, sizeof(qtab));
        qtab[0].Flags        = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
        qtab[0].Name         = L"DebugLevel";
        qtab[0].EntryContext = &dbg;
        qtab[0].DefaultType  = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
        qtab[0].DefaultData  = &dbg;
        qtab[0].DefaultLength = sizeof(dbg);
        (void)RtlQueryRegistryValues(RTL_REGISTRY_SERVICES, L"srcraid\\Parameters",
                                     qtab, NULL, NULL);
        g_SrcDebugEnabled = dbg;
        if (g_SrcDebugEnabled) {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "srcraid: debug output ENABLED (DebugLevel=%u)\n", g_SrcDebugEnabled);
        }
    }

    RtlZeroMemory(&hwInitData, sizeof(hwInitData));
    hwInitData.HwInitializationDataSize = sizeof(hwInitData);
    hwInitData.AdapterInterfaceType     = PCIBus;

    hwInitData.HwFindAdapter    = SrcFindAdapter;
    hwInitData.HwInitialize     = SrcInitialize;
    hwInitData.HwStartIo        = SrcStartIo;
    hwInitData.HwBuildIo        = SrcBuildIo;
    hwInitData.HwInterrupt      = SrcInterrupt;    /* INTx (also the line-based fallback) */
    hwInitData.HwResetBus       = SrcResetBus;     /* NB: HwMSInterruptRoutine is a ConfigInfo field,
                                                    * set in SrcFindAdapter, not here */
    hwInitData.HwAdapterControl = SrcAdapterControl;

    hwInitData.DeviceExtensionSize     = sizeof(DEVICE_EXTENSION);
    hwInitData.SrbExtensionSize        = sizeof(SRB_EXTENSION);
    hwInitData.SpecificLuExtensionSize = 0;
    hwInitData.NumberOfAccessRanges    = 6;      /* all PCI BARs; we pick the SRC window */
    hwInitData.MapBuffers              = STOR_MAP_NON_READ_WRITE_BUFFERS;
    hwInitData.MultipleRequestPerLu    = TRUE;
    hwInitData.TaggedQueuing           = TRUE;
    hwInitData.AutoRequestSense        = TRUE;

    SRCDBG("srcraid: HW_INIT size=0x%x (vendor 0xd0) nAccessRanges=%u\n",
           hwInitData.HwInitializationDataSize, hwInitData.NumberOfAccessRanges);
    {
        ULONG st = StorPortInitialize(DriverObject, RegistryPath, &hwInitData, NULL);
        SRCDBG("srcraid: StorPortInitialize -> 0x%08x\n", st);
        return (NTSTATUS)st;
    }
}
