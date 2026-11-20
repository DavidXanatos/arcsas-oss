/*
 * srcraid.h - Storport miniport for Adaptec/PMC Series 7 (SRC interface)
 * See ../DESIGN.md. Hardware interface in src_hw.h.
 */
#pragma once

#include <ntddk.h>
#include <storport.h>
#include "src_hw.h"

#define SRCRAID_TAG 0x37637253u   /* 'Src7' */

#ifndef VER_BUILD                 /* injected by build.ps1 (auto-increment) */
#define VER_BUILD 0
#endif

/* Diagnostics go to the kernel debugger via DbgPrintEx (IHVDRIVER component),
 * which is visible with the IHVDRIVER mask set - unlike StorPortDebugPrint,
 * which uses StorPort's STORMINI component and is filtered separately.
 *
 * OFF by default (g_SrcDebugEnabled == 0): with a kernel debugger attached, the
 * DbgPrintEx stream is a synchronous transport call that throttles the whole box, so
 * production runs must be quiet. Enable it per-install via the registry DWORD
 *   HKLM\SYSTEM\CurrentControlSet\Services\srcraid\Parameters\DebugLevel = 1
 * (read once in HwFindAdapter). The flag-check compiles to a single load+branch when
 * disabled, so leaving SRCDBG calls in the hot path is free. */
extern ULONG g_SrcDebugEnabled;
#define SRCDBG(...) \
    do { if (g_SrcDebugEnabled) { DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, __VA_ARGS__); } } while (0)

/* adapter run states (mirrors the vendor's run-state semantics) */
#define SRC_STATE_INIT      0
#define SRC_STATE_RUNNING   1
#define SRC_STATE_PAUSED    4

/* Discovered logical volume (container). Bus CONTAINER_CHANNEL, target = cid. */
#define SRC_MAX_CONTAINERS  256
#define SRC_CONTAINER_BUS   0      /* StorPort bus we present containers on */

/* I/O FIB slots. Slot 0 is reserved for the serialized sync/enumeration FIB;
 * slots 1..SRC_NUM_IO_SLOTS-1 carry concurrent READ/WRITE requests. Each slot i
 * owns the FIB buffer at AdapterFibs + i*SRC_FIB_SIZE. */
#define SRC_NUM_IO_SLOTS    512
#define SRC_IO_SLOT_MIN     1

/* AIF (adapter-initiated event) channel. The driver keeps one AifRequest FIB
 * outstanding in a reserved slot; the firmware holds it and completes it via the
 * host RRQ when an adapter event occurs (hot-plug, config/status change). The
 * completed FIB (an aac_aifcmd event) is queued and relayed to the maxView agent
 * via GET_NEXT_ADAPTER_FIB (0x4200f). Mirrors the vendor driver's AIF request/process path. */
#define SRC_AIF_SLOT        103          /* reserved FIB slot for the AifRequest */
#define SRC_MSIXPROBE_SLOT  104          /* reserved FIB slot for the MSI-X completion self-test */
#define SRC_MSIX_PROBE_ROUNDS 8          /* sustained completions required to accept MSI-X (re-arm) */
#define SRC_MGMT_SLOT       104          /* reserved FIB slot for the synchronous mgmt
                                          * passthrough via the inbound-queue/RRQ path
                                          * (0x42260 ContainerCommand needs full firmware
                                          * processing - the mailbox path only partial) */
/* Reserved FIB-slot band used by the fixed/sync paths that address a slot by a
 * hard-coded index instead of SrcIoAllocSlot: slot 100 (SrcIoSelfTest scratch),
 * 101 (phys-sync scratch, src_phys.c), 102 (SEND_RAW_SRB data scratch, src_mgmt.c),
 * 103 (AIF FIB), 104 (legacy sync-mgmt FIB). These MUST be excluded from the
 * dynamic I/O allocator - otherwise a concurrent READ/WRITE can be handed the
 * same slot and the two paths stomp each other's FIB buffer (data corruption,
 * leaked/misrouted completions -> bugcheck). Slot 0 is below SRC_IO_SLOT_MIN so
 * it is already safe. */
#define SRC_RSVD_SLOT_FIRST 100
#define SRC_RSVD_SLOT_LAST  104
#define FIBCMD_AifRequest   700          /* vendor AIF request: FIB.Command = 0x2bc */
#define AIF_REQ_EVENT       200          /* request data[0] = 0xc8 (want events) */
#define SRC_AIF_QDEPTH      8            /* delivery ring depth */
#define SRC_AIF_ENTRY       512          /* bytes stored per queued AIF FIB */
#define SRC_AIF_WATCHDOG_TICKS 20        /* SrcAifTimer runs every 3s; 20 ticks = ~60s with a
                                          * request outstanding and no completion -> force a
                                          * fresh re-arm (recover a silently-lost AifRequest) */
#define SRC_TRANSPARENT_RELAY  1         /* 1 = vendor model: no driver-side INQUIRY sweep; StorPort
                                          * probes each physical target and we relay straight to the
                                          * firmware (PathId 1 -> channel 0, id = TargetId, lun = Lun).
                                          * 0 = legacy compacted sweep (SrcEnumPhysical + Physicals[]). */
#define SRC_MAX_VECTORS        16        /* max MSI-X vectors we provision (== SRC_NUM_MSIX); the
                                          * host RRQ is split into up to this many per-vector regions */
#define SRC_PERVEC_DRAIN       0         /* 0 = drain ALL regions (the ONLY correct option on THIS card).
                                          * Per-vector MSI-X completion queues are a V8-INIT-STRUCT feature:
                                          * the V8 init struct gives each vector a SEPARATE physical RRQ, and
                                          * V8 is gated by firmware option bit 0x40000000. OUR firmware does
                                          * NOT set it (measured 0.1.0.180: optHi=0x24, V8=no), so the vendor
                                          * ITSELF would use
                                          * revision-7 drain-all on this card - per-vector is impossible for
                                          * EITHER driver here. Three HW attempts (.173/.176/.178) confirmed
                                          * it empirically before we proved why. Drain-all +
                                          * DPC_REDIRECTION == vendor behavior + our multi-vector MSI-X is
                                          * actually MORE than the vendor does on a non-V8 card. The toggle +
                                          * SrcFibHandle stay dormant; they'd only matter on a V8-capable card. */
#define SRC_LUN_QUEUE_DEPTH    254       /* per-LUN tagged queue depth (StorPort max; vendor sets a
                                          * deep depth for RAID volumes - without it the StorPort
                                          * default can cap concurrency below the array's capability) */
#define SRC_WEDGE_RECOVER_MAX  3         /* C2: max deferred IOP-reset + re-bring-up recovery
                                          * attempts for a runtime FW wedge before DeadAdapter */
#define SRC_RESET_DEAD_LIMIT   12        /* consecutive HwResetBus with NO completion drained in
                                          * between => the completion path is dead (e.g. a bad MSI-X
                                          * attempt); mark DeadAdapter + fail I/O fast so StorPort
                                          * fails the device (box boots degraded, SSH-recoverable)
                                          * instead of retrying forever -> PnP boot deadlock. */

/* DIAGNOSTIC mgmt-FIB trace ring entry (32 bytes). kind: 1=mgmt req(42260), 2=mgmt resp,
 * 3=SEND_RAW_SRB req(42264), 4=SEND_RAW_SRB resp. a/b/c are kind-specific; d0..d3 = first
 * FSA/data dwords. Recorded lock-free; read out via the "SRCDBG" IOCTL. */
#define SRC_TRACE_N 1024                 /* power of 2 */
typedef struct _SRC_TRACE_ENT {
    ULONG kind, a, b, c;
    ULONG d0, d1, d2, d3;
} SRC_TRACE_ENT, *PSRC_TRACE_ENT;

typedef struct _SRC_CONTAINER {
    BOOLEAN     Valid;
    UCHAR       RescanMiss;            /* consecutive runtime re-probes that came back not-present */
    BOOLEAN     ReadOnly;
    BOOLEAN     NotReady;              /* firmware FSCS_NOT_READY (degraded/initializing/foreign) */
    BOOLEAN     Hidden;               /* maxView unpresented this LUN (IOCTL 0x42017); hide from Windows */
    ULONG       Type;              /* mnt[0].vol substrate type */
    ULONGLONG   SizeBlocks;        /* capacity in logical blocks */
    ULONG       BlockSize;         /* logical block size (default 512) */
    ULONG       Serial;            /* unique per-container id for SCSI VPD 0x80/0x83 (cid<<24 | oid) */
    ULONG       CtUid;             /* firmware 32-bit container UID (CT cmd 165) for maxView IOCTL map */
    BOOLEAN     QueueDepthSet;     /* StorPortSetDeviceQueueDepth issued for this LUN (once) */
} SRC_CONTAINER, *PSRC_CONTAINER;

/* Physical (pass-through) devices, presented on SRC_PHYSICAL_BUS. Windows target
 * = index into dev->Physicals[]; the firmware (channel,id,lun) lives in the entry
 * and goes into the aac_srb (ScsiPortCommand64). Only accessible devices (INQUIRY
 * peripheral qualifier 0 = not claimed by a container) are exposed.
 *
 * 255 entries: the controller supports up to ~238 physical devices behind cascaded
 * expanders; with append-only indexing the target id = array index must stay < 255
 * (StorPort per-bus target ceiling). */
#define SRC_MAX_PHYSICALS   255
#define SRC_PHYSICAL_BUS    1
/* INQUIRY-sweep span for enumeration (channel,id) + per-target multi-LUN probe.
 * Direct-attach disks report on channel 0 (ids ~16+), expander disks climb to high
 * ids (observed id 59 with 2 cascaded expanders; the controller can reach ~255), and
 * enclosure-service (SES) devices appear on other channels (observed channel 2). */
#define SRC_SWEEP_CHANNELS  4       /* channels 0..3 (direct ch0 + SES ch2 + margin) */
#define SRC_SWEEP_IDS       256     /* ids 0..255 (high ids behind cascaded expanders) */
/* LUNs probed per present target. Deliberately 1 (lun 0 only): all real devices here
 * (disks + SES enclosures) are lun 0, and BLIND lun 1..7 probing is unsafe - many old /
 * non-compliant drives echo their lun-0 INQUIRY (peripheral qualifier 0) for every LUN,
 * which would expose phantom duplicate disks. The plumbing (SRC_PHYSICAL.Lun, SrcPhysExecute)
 * is lun-aware, so a genuine multi-LUN device is best discovered via REPORT LUNS on the
 * present target (echo-safe) rather than a blind sweep - add that only if such hardware appears. */
#define SRC_SWEEP_LUNS      1
#define SRC_SWEEP_MBOX_ABORT 16     /* consecutive sync-mailbox timeouts -> abort (fw wedged) */
typedef struct _SRC_PHYSICAL {
    BOOLEAN     Valid;
    UCHAR       Channel;           /* firmware bus/channel */
    UCHAR       Id;                /* firmware target id */
    UCHAR       Lun;
    UCHAR       DeviceType;        /* INQUIRY peripheral device type (byte0 & 0x1f) */
} SRC_PHYSICAL, *PSRC_PHYSICAL;

/*
 * Per-adapter device extension. StorPort allocates DeviceExtensionSize bytes and
 * hands us the pointer in every callback. Comm-area / FIB-pool / RRQ fields land
 * here in P2/P3; the P1 set below is enough to find, map and identify the adapter.
 */
typedef struct _DEVICE_EXTENSION {
    /* identity (HwFindAdapter) */
    USHORT          VendorId;
    USHORT          DeviceId;
    USHORT          SubVendorId;
    USHORT          SubSystemId;
    UCHAR           RevisionId;
    BOOLEAN         IsSrcInterface;   /* devid in {028B,028C,028D} */

    /* mapped SRC register window (BAR0) */
    PUCHAR          Regs;             /* virtual base from StorPortGetDeviceBase */
    STOR_PHYSICAL_ADDRESS RegsPhys;
    ULONG           RegsLength;
    PUCHAR          Mailbox;          /* Regs + per-devid sync mailbox offset (8 dwords) */
    ULONG           Oimr;             /* saved outbound interrupt mask */

    /* capabilities / limits (reported to StorPort in HwFindAdapter) */
    ULONG           MaxTransferLength;
    ULONG           MaxFib;           /* outstanding commands */
    ULONG           NumMsix;

    /* firmware-negotiated comm parameters (GET_ADAPTER_PROPERTIES / _COMM_PREFERRED) */
    ULONG           CommType;         /* AAC_COMM_* (TYPE2 for Series 7) */
    ULONG           RawIo64;          /* 64-bit SG supported */
    ULONG           MaxFibSize;       /* largest FIB to adapter */
    ULONG           MaxSectors;       /* largest I/O (sectors) */
    ULONG           SgTableSize;      /* max SG elements */
    ULONG           MaxOutstanding;   /* max outstanding FIBs (can_queue) */
    ULONG           MaxNumAif;        /* max adapter-initiated FIBs */

    /* uncached (DMA-coherent) comm area + its sub-regions (P2) */
    PUCHAR          CommArea;         /* StorPortGetUncachedExtension result */
    STOR_PHYSICAL_ADDRESS CommAreaPhys;
    ULONG           CommAreaLength;
    ULONG           NumFibs;          /* host FIB count we provision */

    PUCHAR          CommHdr;          ULONG CommHdrPhys;      /* producer/consumer header+queues */
    PUCHAR          AdapterFibs;      ULONG AdapterFibsPhys;  ULONG AdapterFibsSize;
    PUCHAR          RawBounce;        ULONG RawBouncePhys;    /* raw-SRB >1KB bounce pool */
    volatile LONG   RawBounceBusy;    /* per-buffer busy bitmap (SRC_RAWBOUNCE_COUNT bits) */
    PULONG          HostRrq;          ULONGLONG HostRrqPhys;  /* SRC completion ring */
    ULONG           HostRrqEntries;
    /* Multi-vector MSI-X: the host RRQ is one contiguous ring (revision-7 init struct, no
     * per-vector VirQAddr) split into NumVectors regions of VectorCap entries each. Vector v
     * owns [v*VectorCap, (v+1)*VectorCap); the firmware posts a completion into the region of
     * the vector it signals and raises that MSI-X message. INTx is just NumVectors==1 (one
     * region == the whole ring == today's behavior). Each region has its own consumer index. */
    ULONG           NumVectors;       /* 1 = INTx; N = MSI-X (vendor: SystemProcessorNumber) */
    ULONG           VectorCap;        /* HostRrqEntries / NumVectors (entries per region) */
    volatile LONG   DrainVec;         /* per-vector drain: region the current ISR should drain
                                       * (set by SrcMsInterrupt); -1 = drain all (INTx/fallback) */
    ULONG           HostRrqIdx[SRC_MAX_VECTORS];  /* per-region consumer index (region base-relative) */
    PAAC_INIT_R7    Init;             ULONG InitPhys;         /* must be 32-bit */

    /* logical volumes (containers) discovered in HwInitialize */
    ULONG           MaxContainers;    /* probe ceiling from GET_CONTAINER_COUNT */
    ULONG           NumValidContainers;
    ULONG           NameServeCmd;     /* VM_NameServe64/32 variant chosen at enum (for re-probe) */
    SRC_CONTAINER   Containers[SRC_MAX_CONTAINERS];

    /* physical (pass-through) devices discovered by INQUIRY sweep (or CISS, HBA mode) */
    ULONG           NumPhysicals;
    BOOLEAN         ExposePhysicals;  /* ExposeNonDASD registry parameter */
    SRC_PHYSICAL    Physicals[SRC_MAX_PHYSICALS];
    /* CISS REPORT PHYSICAL LUNS enumeration (the vendor driver's physical-device enumeration). Only usable
     * when the firmware exposes a management device (native-HBA mode); in RAID mode the
     * discovery finds nothing (CissAvail=FALSE) and we use the INQUIRY sweep instead. */
    BOOLEAN         CissAvail;         /* a CISS management device was discovered */
    UCHAR           CissMgmtCh;        /* its (channel,id,lun) - 0xFF channel = none */
    UCHAR           CissMgmtId;
    UCHAR           CissMgmtLun;

    /* async I/O: slot -> SRB map (lock-free via Interlocked pointer ops) and ISR state */
    PVOID           IoSlotSrb[SRC_NUM_IO_SLOTS];
    volatile LONG   NextSlotHint;     /* round-robin search start */
    BOOLEAN         InterruptsEnabled;
    BOOLEAN         MsiEnabled;       /* MSI-X mode (vs INTx) */
    ULONG           AdapterOptions;   /* GET_ADAPTER_PROPERTIES option flags (AAC_OPT_*); bit 30
                                       * (AAC_OPT_NEW_COMM_TYPE3=0x40000000) == firmware V8 support
                                       * (the vendor driver -> SupportV8InitStruct), the gate for
                                       * true per-vector MSI-X */
    BOOLEAN         Ready;            /* passive bring-up done; OK to service I/O */
    BOOLEAN         DeadAdapter;      /* completions stopped (N resets w/o progress); fail I/O fast */
    volatile LONG   ResetCount;       /* consecutive HwResetBus calls with no completion in between */
    ULONG           WedgeRecoveries;  /* C2: deferred FW-wedge recovery attempts used this load */

    /* run / power state */
    volatile LONG   AdapterState;     /* SRC_STATE_* */
    BOOLEAN         IoPaused;          /* we called StorPortPause */
    BOOLEAN         FwLostOnPower;     /* set on ScsiStopAdapter -> full reinit on restart */
    volatile LONG   RestartPending;    /* deferred ScsiRestartAdapter re-init in progress (timer) */
    ULONG           RestartAttempts;   /* fw-ready poll count in the restart timer */
    BOOLEAN         MgmtOpen;          /* AACAPI management session opened */
    BOOLEAN         PerfInitDone;      /* StorPortInitializePerfOpts applied once */
    volatile LONG   MsixProbeDone;     /* ISR sets when the MSI-X self-test completion drains */
    volatile LONG   MsixIsrCount;      /* DIAG: # times the MSI-X ISR branch fired during the probe */
    ULONG           MsixProbeRounds;   /* DIAG: # sustained MSI-X probe rounds that completed */
    volatile LONG   MsixMaxMsg;        /* DIAG: highest MSI-X MessageId StorPort ever delivered
                                        * (== granted vector count - 1; if < NumVectors-1 the high
                                        * vectors never fire -> low-QD I/O on them stalls) */
    volatile LONG   MgmtBusy;          /* serialize the mgmt FIB passthrough (slot 0 + mailbox) */

    /* AIF (adapter-initiated event) channel */
    BOOLEAN         AifCtxOpen;        /* agent registered an AIF context (0x42043) */
    volatile LONG   AifReqPending;     /* an AifRequest FIB is outstanding in SRC_AIF_SLOT */
    volatile LONG   AifIdleTicks;      /* SrcAifTimer ticks with a request outstanding + silent (watchdog) */
    volatile LONG   AifQHead;          /* delivery ring: next AIF to hand the agent */
    volatile LONG   AifQTail;          /* delivery ring: next slot to fill from the ISR */
    UCHAR           AifQ[SRC_AIF_QDEPTH][SRC_AIF_ENTRY];  /* queued AIF FIB copies */

    /* hot re-enumeration: an AIF event (config change) asks the timer to re-sweep the
     * physical bus and tell StorPort to re-probe (so a disk uninitialized -> raw
     * pass-through at runtime appears in Windows without a reboot). RescanCooldown
     * throttles the sweep under event storms (e.g. rebuild progress). */
    volatile LONG   RescanPending;     /* set by the AIF ISR when an event arrives */
    LONG            RescanTicks;       /* remaining timer ticks to re-scan (burst after an event) */
    ULONG           LastFwTimeSync;    /* elapsed-seconds at the last FW time-of-day push (0=never) */

    /* synchronous mgmt passthrough via the RRQ path (SRC_MGMT_SLOT) */
    volatile LONG   MgmtRrqDone;       /* set by SrcIoDrainRrq when SRC_MGMT_SLOT completes */

    /* DIAGNOSTIC: in-memory mgmt-FIB trace ring (no DbgPrint -> safe under the maxView
     * agent). Read out via the "SRCDBG" IOCTL (src_mgmt.c) with the srctrace tool. */
    volatile LONG   TraceIdx;          /* total writes; entry = Trace[idx & (SRC_TRACE_N-1)] */
    SRC_TRACE_ENT   Trace[SRC_TRACE_N];
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

/* Per-request SRB extension (FIB builder scratch) - sized in DriverEntry (P3). */
#define SRC_FIBK_RAWIO2   0    /* container block I/O (ContainerRawIo2) */
#define SRC_FIBK_SRB      1    /* physical pass-through (aac_srb) */
#define SRC_FIBK_MGMT     2    /* AAC mgmt FIB passthrough (0x42260/0x42007): async like
                                * the vendor driver, completed by the ISR which copies the
                                * whole response FIB back to the user IOCTL buffer. */
#define SRC_FIBK_FLUSH    3    /* container cache flush (CT_FLUSH_CACHE) for SYNCHRONIZE CACHE */
#define SRC_FIBK_RAWIOCTL 4    /* 0x42264/0x4201B maxView raw-SRB passthrough (SMART/INQUIRY):
                                * async aac_srb like the vendor driver (NOT the sync mailbox);
                                * ISR copies the device data from the per-slot bounce region to
                                * the IOCTL buffer + writes the transfer length + ReturnCode. */
/* per-slot data bounce for SRC_FIBK_RAWIOCTL, inside the slot's SRC_FIB_SIZE FIB buffer:
 * the aac_srb FIB (~128 B) lives at the front; device data DMAs into the back half. */
#define SRC_RAWIOCTL_DATAOFF 1024
#define SRC_RAWIOCTL_FIBTAIL (SRC_FIB_SIZE - SRC_RAWIOCTL_DATAOFF)   /* 1024: per-slot in-FIB bounce */
/* Raw-SRB bounce POOL: lets a maxView passthrough with >1KB of data (log/VPD/SES/mode pages)
 * stay ASYNC instead of falling back to the DISPATCH sync mailbox. Small dedicated DMA pool
 * (COUNT buffers of SIZE each); <=FIBTAIL uses the per-slot FIB tail (no pool pressure). */
#define SRC_RAWBOUNCE_COUNT  16
#define SRC_RAWBOUNCE_SIZE   8192
#define SRC_RAWIOCTL_DATAMAX SRC_RAWBOUNCE_SIZE   /* >this still falls back to sync (very rare) */
typedef struct _SRB_EXTENSION {
    ULONG           Slot;          /* I/O FIB slot assigned in StartIo */
    UCHAR           Kind;          /* SRC_FIBK_* - how the ISR reads the reply */
    LONG            BounceIdx;     /* raw-SRB >1KB: index into dev->RawBounce pool; -1 = FIB tail */
} SRB_EXTENSION, *PSRB_EXTENSION;

/* ---- SRC register access (MMIO) ---- */
FORCEINLINE ULONG SrcRead32(_In_ PDEVICE_EXTENSION dev, _In_ ULONG off)
{
    return StorPortReadRegisterUlong(dev, (PULONG)(dev->Regs + off));
}
FORCEINLINE VOID SrcWrite32(_In_ PDEVICE_EXTENSION dev, _In_ ULONG off, _In_ ULONG val)
{
    StorPortWriteRegisterUlong(dev, (PULONG)(dev->Regs + off), val);
}
/* OMR0 firmware status register */
FORCEINLINE ULONG SrcFwStatus(_In_ PDEVICE_EXTENSION dev)
{
    return SrcRead32(dev, SRC_REG_OMR0);
}
/* ring the inbound doorbell */
FORCEINLINE VOID SrcInboundDoorbell(_In_ PDEVICE_EXTENSION dev, _In_ ULONG bit)
{
    SrcWrite32(dev, SRC_REG_IDR, bit << SRC_IDR_SHIFT);
}

/* DIAGNOSTIC: record one mgmt-FIB trace entry (lock-free ring). */
FORCEINLINE VOID SrcTrace(_In_ PDEVICE_EXTENSION dev, _In_ ULONG kind, _In_ ULONG a,
                          _In_ ULONG b, _In_ ULONG c, _In_ ULONG d0, _In_ ULONG d1,
                          _In_ ULONG d2, _In_ ULONG d3)
{
    LONG i = InterlockedIncrement(&dev->TraceIdx) - 1;
    PSRC_TRACE_ENT e = &dev->Trace[((ULONG)i) & (SRC_TRACE_N - 1)];
    e->kind = kind; e->a = a; e->b = b; e->c = c;
    e->d0 = d0; e->d1 = d1; e->d2 = d2; e->d3 = d3;
}

/* ---- firmware bring-up helpers (src_init.c) ---- */
BOOLEAN SrcWaitForFwReady(_In_ PDEVICE_EXTENSION dev, _In_ ULONG timeoutMs);
ULONG   SrcCheckHealth(_In_ PDEVICE_EXTENSION dev, _Out_opt_ PULONG panicCode);
/* Full IOP reset: reboot firmware to a clean baseline (vendor-style soft reset / P4). */
BOOLEAN SrcIopReset(_In_ PDEVICE_EXTENSION dev);
VOID    SrcSetupMailbox(_In_ PDEVICE_EXTENSION dev);
/* Non-blocking IOP reset trigger (writes the reset registers, no wait) for the deferred
 * restart timer, which runs at DISPATCH and must not do SrcIopReset's long busy-wait. */
VOID    SrcIopResetTrigger(_In_ PDEVICE_EXTENSION dev);

/* ---- comm bring-up (src_comm.c) ---- */
#define SRC_BRINGUP_OK          0
#define SRC_BRINGUP_NOMEM       1
#define SRC_BRINGUP_ADDR        2   /* comm area not 32-bit addressable */
#define SRC_BRINGUP_INITFAIL    3
/* Allocate + lay out the DMA comm area (call from HwFindAdapter; needs ConfigInfo). */
ULONG SrcCommAlloc(_In_ PDEVICE_EXTENSION dev, _In_ PPORT_CONFIGURATION_INFORMATION ConfigInfo);
/* Build AAC_INIT (r7) and push via INIT_STRUCT_BASE_ADDRESS (call from HwInitialize). */
ULONG SrcCommStart(_In_ PDEVICE_EXTENSION dev);

/* ---- FIB path (src_fib.c) ---- */
/* General serialized (slot-0) sync FIB submit; response left in the FIB data area. */
ULONG SrcFibSubmitSync(_In_ PDEVICE_EXTENSION dev, _In_ USHORT command,
                       _In_reads_bytes_opt_(inSize) PVOID inData, _In_ ULONG inSize,
                       _In_ ULONG fibDataSize, _In_ ULONG timeoutMs);
/* As above, with inPlace=TRUE selecting the vendor driver's sync-FIB completion
 * model (XferState=0x102d, StructType=FIB_MAGIC2_64, poll AdapterProcessed). */
ULONG SrcFibSubmitSyncEx(_In_ PDEVICE_EXTENSION dev, _In_ USHORT command,
                         _In_reads_bytes_opt_(inSize) PVOID inData, _In_ ULONG inSize,
                         _In_ ULONG fibDataSize, _In_ ULONG timeoutMs, _In_ BOOLEAN inPlace);
/* Host wall-clock seconds since 1970 (for the FW time-of-day sync). */
ULONG SrcElapsedSeconds(_In_ PDEVICE_EXTENSION dev);
/* Push host wall-clock time to the FW (FIBCMD_SetAdapterTime) at most every ~15 min, so FW-side
 * log/SMART/enclosure timestamps stay aligned. Called from the AIF timer; takes MgmtBusy. */
VOID  SrcSyncFwTime(_In_ PDEVICE_EXTENSION dev);
/* Submit a single RequestAdapterInfo FIB and poll the host RRQ for completion. */
ULONG SrcFibSelfTest(_In_ PDEVICE_EXTENSION dev, _In_ ULONG timeoutMs);
/* Query the container probe ceiling (max container slots) the firmware exposes. */
ULONG SrcGetContainerCount(_In_ PDEVICE_EXTENSION dev, _Out_ PULONG count);
/* Probe one container slot with `command` (VM_NameServe or VM_NameServe64);
 * fills *out (out->Valid if a volume is present). */
ULONG SrcProbeContainer(_In_ PDEVICE_EXTENSION dev, _In_ ULONG cid,
                        _In_ ULONG command, _Out_ PSRC_CONTAINER out,
                        _In_ ULONG timeoutMs);
/* Probe all slots 0..MaxContainers-1, populate dev->Containers, log the valid ones. */
VOID  SrcEnumContainers(_In_ PDEVICE_EXTENSION dev);
BOOLEAN SrcRescanContainers(_In_ PDEVICE_EXTENSION dev);  /* runtime re-probe (stable cids) */
VOID  SrcFlushAllContainers(_In_ PDEVICE_EXTENSION dev);  /* M1: sync CT_FLUSH_CACHE sweep on shutdown */
VOID  SrcWaitForConfigReady(_In_ PDEVICE_EXTENSION dev);  /* M2: poll FW config-ready before enum */
VOID  SrcEnumPhysical(_In_ PDEVICE_EXTENSION dev);        /* INQUIRY-sweep enumeration */
VOID  SrcCissDiscover(_In_ PDEVICE_EXTENSION dev);        /* probe for a CISS mgmt device (HBA mode) */
BOOLEAN SrcRescanPhysical(_In_ PDEVICE_EXTENSION dev);    /* runtime re-sweep (append-only, stable ids) */
BOOLEAN SrcPhysExecute(_In_ PDEVICE_EXTENSION dev, _In_ PSCSI_REQUEST_BLOCK Srb);
/* aac_srb SCSI passthrough via the sync mailbox (slot 0); reply left at
 * dev->AdapterFibs+sizeof(AAC_FIBHDR), data DMAs to/from dataPhys. Caller holds MgmtBusy. */
ULONG SrcSrbSync(_In_ PDEVICE_EXTENSION dev, _In_ ULONG channel, _In_ ULONG id, _In_ ULONG lun,
                 _In_reads_bytes_(cdbLen) const UCHAR *cdb, _In_ ULONG cdbLen,
                 _In_ ULONG dataPhys, _In_ ULONG dataLen, _In_ BOOLEAN dataIn, _In_ ULONG timeoutMs);

/* Submit the FIB already built in SRC_MGMT_SLOT via the inbound queue and wait
 * (OIMR masked, poll the host RRQ) for its handle to complete - the vendor's
 * ResponseExpected/RRQ delivery, needed so the firmware FULLY processes a
 * ContainerCommand (the mailbox SEND_SYNCHRONOUS_FIB only partially does).
 * Caller holds MgmtBusy and has set up the FIB. Returns SRC_SYNC_OK/TIMEOUT. */
ULONG SrcMgmtFibRrq(_In_ PDEVICE_EXTENSION dev, _In_ ULONG fibSize, _In_ ULONG timeoutMs);

/* AIF channel (src_aif.c) */
VOID    SrcAifInit(_In_ PDEVICE_EXTENSION dev);        /* reserve slot, post first request, arm timer */
VOID    SrcAifPost(_In_ PDEVICE_EXTENSION dev);        /* post/re-post the AifRequest FIB */
VOID    SrcAifOnComplete(_In_ PDEVICE_EXTENSION dev);  /* RRQ drain: AIF FIB completed - queue + re-post */
BOOLEAN SrcAifDeliver(_In_ PDEVICE_EXTENSION dev, _Out_writes_bytes_(cap) PUCHAR dst, _In_ ULONG cap);
BOOLEAN SrcIoctl(_In_ PDEVICE_EXTENSION dev, _In_ PSCSI_REQUEST_BLOCK Srb);   /* mgmt IOCTL */
/* Query config status and commit pending config (CT_COMMIT_CONFIG) so the
 * firmware brings containers online for block I/O (vendor HandleAdapter*Config). */
ULONG SrcCommitConfig(_In_ PDEVICE_EXTENSION dev);

/* ---- SCSI translation (src_scsi.c) ---- */
/* Translate one EXECUTE_SCSI SRB for a container target; TRUE if completed now. */
BOOLEAN SrcScsiExecute(_In_ PDEVICE_EXTENSION dev, _In_ PSCSI_REQUEST_BLOCK Srb);

/* ---- async I/O path (src_io.c) ---- */
/* Build + submit a READ/WRITE as a ContainerCommand64 FIB. Returns TRUE if the
 * request was queued (complete later in the ISR); FALSE if it could not be
 * queued (caller sets Srb error status and completes it now). */
BOOLEAN SrcIoReadWrite(_In_ PDEVICE_EXTENSION dev, _In_ PSCSI_REQUEST_BLOCK Srb,
                       _In_ PSRC_CONTAINER c);
/* SYNCHRONIZE CACHE -> async CT_FLUSH_CACHE FIB for the container (write-back safety). */
BOOLEAN SrcIoFlush(_In_ PDEVICE_EXTENSION dev, _In_ PSCSI_REQUEST_BLOCK Srb);
/* 0x42264/0x4201B raw-SRB passthrough submitted ASYNC (vendor-style async submit) instead of the
 * sync mailbox. p = IOCTL payload, have = its length. TRUE = SRB is pending (ISR completes it);
 * FALSE = could not submit async (no slot, or transfer > SRC_RAWIOCTL_DATAMAX) - caller falls back. */
BOOLEAN SrcRawSrbSubmit(_In_ PDEVICE_EXTENSION dev, _In_ PSCSI_REQUEST_BLOCK Srb,
                        _In_ PUCHAR p, _In_ ULONG have);
/* Switch the firmware to MSI-X mode (before any block I/O); interrupts masked. */
VOID    SrcIoEnableMsixMode(_In_ PDEVICE_EXTENSION dev);
/* Revert the firmware out of MSI-X back to INTx (MSI-X self-test fallback). */
VOID    SrcIoDisableMsixMode(_In_ PDEVICE_EXTENSION dev);
/* Bounded interrupt-driven MSI-X completion self-test; TRUE = MSI-X completions drain. */
BOOLEAN SrcMsixProbe(_In_ PDEVICE_EXTENSION dev);
/* Unmask the outbound (response) interrupt once bring-up is complete. */
VOID    SrcIoEnableInterrupts(_In_ PDEVICE_EXTENSION dev);
/* One-shot init diagnostic: read LBA 0 into a comm-area SG buffer. */
VOID    SrcIoSelfTest(_In_ PDEVICE_EXTENSION dev);
/* Drain the host RRQ and complete finished SRBs. Returns TRUE if it was ours.
 * SrcIoDrainRrq is the INTx entry (drains region 0 == whole ring when NumVectors==1).
 * SrcMsInterrupt is the HwMSInterruptRoutine (MSI-X); it acks the MSI doorbell and drains. */
BOOLEAN SrcIoDrainRrq(_In_ PDEVICE_EXTENSION dev);
BOOLEAN SrcMsInterrupt(_In_ PVOID DeviceExtension, _In_ ULONG MessageId);
/* AAC mgmt FIB passthrough: submit `p` (an AAC FIB, length plSize) ASYNC on the
 * inbound queue; TRUE = SRB now pending (ISR completes it), FALSE = no slot free. */
BOOLEAN SrcMgmtSubmit(_In_ PDEVICE_EXTENSION dev, _In_ PSCSI_REQUEST_BLOCK Srb,
                      _In_ PUCHAR p, _In_ ULONG plSize);

/* ---- sync-command channel (src_sync.c) ---- */
#define SRC_SYNC_OK        0
#define SRC_SYNC_TIMEOUT   1
#define SRC_SYNC_FAILED    2
#define SRC_SYNC_BLKHINT   3   /* filesystem name-serve found a block-type (variable/4K-sector)
                                * container hidden as vol=CT_NONE+obj=FT_BLK; re-probe the slot
                                * with VM_NameServeAllBlk (so 4K arrays enumerate alongside 512B) */
/* Send a mailbox sync command and wait for completion. status/r1..r4 optional. */
ULONG SrcSyncCommand(_In_ PDEVICE_EXTENSION dev, _In_ ULONG command,
                     _In_ ULONG p1, _In_ ULONG p2, _In_ ULONG p3,
                     _In_ ULONG p4, _In_ ULONG p5, _In_ ULONG p6,
                     _Out_opt_ PULONG status, _Out_opt_ PULONG r1,
                     _Out_opt_ PULONG r2, _Out_opt_ PULONG r3, _Out_opt_ PULONG r4,
                     _In_ ULONG timeoutMs);

/* ---- Storport callback prototypes ---- */
DRIVER_INITIALIZE DriverEntry;

HW_FIND_ADAPTER      SrcFindAdapter;
HW_INITIALIZE        SrcInitialize;
HW_STARTIO           SrcStartIo;
HW_BUILDIO           SrcBuildIo;
HW_INTERRUPT         SrcInterrupt;
HW_RESET_BUS         SrcResetBus;
HW_ADAPTER_CONTROL   SrcAdapterControl;
