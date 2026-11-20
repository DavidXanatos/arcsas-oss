/*
 * src_mgmt.c - management IOCTL interface (maxView Storage Manager / arcconf).
 *
 * Tools talk to the miniport via IOCTL_SCSI_MINIPORT, which StorPort delivers as
 * SRB_FUNCTION_IO_CONTROL with an SRB_IO_CONTROL header at the start of the SRB
 * data buffer. The vendor arcsas driver dispatches on the ControlCode:
 *   0x1B0501..0x1B050C = ACI2 FIB passthrough (relay a FIB to the firmware)
 *   < 0x1B0501         = CSMI, signature "CSMISAS"
 *
 * Step 1 (this build): a DIAGNOSTIC that logs every management IOCTL (signature,
 * control code, lengths, first payload bytes) so we can see exactly what arcconf /
 * maxView send and implement the real passthrough + CSMI responses to match.
 */
#include "srcraid.h"

/* standard SRB_IO_CONTROL (ntddscsi.h) - defined locally to avoid a UM header dep */
typedef struct _SRC_SRB_IO_CONTROL {
    ULONG   HeaderLength;
    UCHAR   Signature[8];
    ULONG   Timeout;
    ULONG   ControlCode;
    ULONG   ReturnCode;
    ULONG   Length;
} SRC_SRB_IO_CONTROL, *PSRC_SRB_IO_CONTROL;

static LONG g_dbgIoctl = 0;
static LONG g_dbgAif = 0;
static LONG g_dbgRaw = 0;
#define IOCTL_DBG_LIMIT 0    /* mgmt path must stay SILENT. Every DbgPrint here traps over the
                              * KD wire when a kernel debugger is attached (milliseconds, CPU-
                              * serializing); that latency, injected mid-response, widens a race
                              * in the multi-threaded maxView agent and corrupts its heap
                              * (0xC0000374 -> tomcat9 dies). The vendor arcsas emits no such
                              * prints, so maxView never races against it. 0 = ALL mgmt logging
                              * off (incl. the RAW_SRB/0x42047 dumps); raise only to re-map
                              * traffic, and never while maxView is driving under a debugger. */

/* 0x42043 (open adapter-FIB context) response: a 216-byte adapter/context struct
 * (driver name, PCI ids, firmware build, caps, adapter-options). Captured verbatim
 * from the vendor driver on this HW; the runtime kernel pointers (at +0x18,
 * +0x28, +0xc0, +0xd0) are zeroed (opaque to the user-mode agent) and the driver
 * name is set to ours. Everything else (PCI 9005:028c subsys 0503, build 0xd2fd,
 * adapter options 0x241f1d7c, max-fib 0x800, ...) is this card's real values. */
static const UCHAR g_ctx43Template[0xd8] = {
    /* +0x00 driver name "srcraid.sys" */
    's','r','c','r','a','i','d','.','s','y','s',0,0,0,0,0,
    /* +0x10 */ 0x9e,0xc9,0x07,0x00, 0x00,0x00,0x00,0x00,
    /* +0x18 ptr->0 */ 0,0,0,0,0,0,0,0,
    /* +0x20 */ 0x00,0x00,0x00,0x00, 0xe6,0x07,0x00,0x00,
    /* +0x28 ptr->0 */ 0,0,0,0,0,0,0,0,
    /* +0x30 */ 0xff,0x0f,0x00,0x00, 0x00,0x00,0x00,0x00, 0x02,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    /* +0x40 */ 0x00,0x00,0x00,0x00, 0x05,0x90,0x00,0x00, 0x8c,0x02,0x00,0x00, 0x05,0x90,0x00,0x00,
    /* +0x50 */ 0x03,0x05,0x00,0x00, 0x00,0x00,0x10,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x60,0xfb,
    /* +0x60 */ 0x01,0x01,0xd4,0x17, 0x00,0x00,0x04,0x00, 0x00,0x08,0x00,0x00, 0x40,0x00,0x00,0x00,
    /* +0x70 */ 0x51,0x00,0x00,0x00, 0x00,0x04,0x00,0x00, 0x00,0x01,0x01,0x00, 0x04,0x00,0x00,0x00,
    /* +0x80 */ 0x01,0x00,0x00,0x00, 0x00,0x20,0x00,0x00, 0x00,0x00,0x00,0x00, 0x01,0x00,0x00,0x00,
    /* +0x90 */ 0x76,0x7d,0x00,0x00, 0x76,0x7d,0x00,0x00, 0xfd,0xd2,0x00,0x00, 0x01,0xf0,0xfa,0xfa,
    /* +0xa0 */ 0xe7,0x6e,0x30,0x00, 0x7c,0x1d,0x1f,0x24, 0xdf,0x57,0xbf,0x7f, 0x18,0x00,0x00,0x00,
    /* +0xb0 */ 0x00,0x6f,0x6e,0x54, 0x00,0x00,0x00,0x00, 0x00,0x20,0x00,0x00, 0x00,0x00,0x00,0x00,
    /* +0xc0 ptr->0 */ 0,0,0,0,0,0,0,0, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    /* +0xd0 ptr->0 */ 0,0,0,0,0,0,0,0,
};

/*
 * Submit the FIB already built in SRC_MGMT_SLOT via the inbound queue and wait for
 * its handle to complete on the host RRQ - the vendor's ResponseExpected/RRQ delivery.
 * This is required for the 0x42260 ContainerCommand passthrough: the
 * firmware FULLY processes the command this way (filling the object-graph response incl.
 * the 0xffffffff terminator fields), whereas the mailbox SEND_SYNCHRONOUS_FIB only
 * partially processes it -> maxView's storarc walks the graph forever -> JVM heap smash.
 *
 * Runs at DISPATCH (HwStartIo). We mask the outbound interrupt for the duration and
 * drain the RRQ ourselves (SrcIoDrainRrq also completes any normal I/O that finishes
 * while we poll), so there's no ISR race on HostRrqIdx. Caller holds MgmtBusy.
 */
ULONG
SrcMgmtFibRrq(_In_ PDEVICE_EXTENSION dev, _In_ ULONG fibSize, _In_ ULONG timeoutMs)
{
    ULONG fibPhys = dev->AdapterFibsPhys + SRC_MGMT_SLOT * SRC_FIB_SIZE;
    ULONG fibSize128, iqLow, iters;

    if (dev->AdapterFibs == NULL || dev->HostRrq == NULL) {
        return SRC_SYNC_FAILED;
    }

    fibSize128 = (fibSize + 127) / 128 - 1;
    if (fibSize128 > 31) {
        fibSize128 = 31;
    }
    iqLow = (fibPhys & ~0x7Fu) | fibSize128;

    InterlockedExchange(&dev->MgmtRrqDone, 0);
    KeMemoryBarrier();

    /* Submit and let the NORMAL ISR path complete it. The miniport runs
     * StorSynchronizeFullDuplex, so HwInterrupt (SrcIoDrainRrq) runs concurrently
     * with this HwStartIo context on another CPU (or preempts us at DIRQL on this
     * one). We must NOT mask the interrupt or drain the RRQ ourselves - two concurrent
     * SrcIoDrainRrq drainers race HostRrqIdx and the mgmt completion gets lost
     * (-> timeout -> the caller echoes the request -> maxView storarc walks a response
     * with no terminators -> runaway). So the ISR is the sole drainer; it sets
     * MgmtRrqDone when SRC_MGMT_SLOT's handle completes, and we just poll that. */
    SrcWrite32(dev, SRC_REG_IQ_H, 0);
    SrcWrite32(dev, SRC_REG_IQ_L, iqLow);

    iters = timeoutMs * 10;                      /* ~100us per iteration */
    for (;;) {
        if (InterlockedCompareExchange(&dev->MgmtRrqDone, 0, 0) != 0) {
            return SRC_SYNC_OK;
        }
        if (iters == 0) {
            return SRC_SYNC_TIMEOUT;
        }
        iters--;
        StorPortStallExecution(100);
    }
}

/*
 * Handle a management IOCTL. Returns TRUE (SRB completed here). For now: log and
 * reject, so we can observe the tool's traffic without pretending to service it.
 */
BOOLEAN
SrcIoctl(
    _In_ PDEVICE_EXTENSION dev,
    _In_ PSCSI_REQUEST_BLOCK Srb
    )
{
    PSRC_SRB_IO_CONTROL ioc = (PSRC_SRB_IO_CONTROL)Srb->DataBuffer;
    PUCHAR p;

    if (ioc == NULL || Srb->DataTransferLength < sizeof(SRC_SRB_IO_CONTROL)) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        return TRUE;
    }

    p = (PUCHAR)ioc + ioc->HeaderLength;        /* payload after the header */

    /* --- DIAGNOSTIC trace readback, signature "SRCDBG" (srctrace tool; no tool sends this).
     * ControlCode 0 = reset the ring, 1 = dump it: payload = {u32 'SRCT' magic, u32 total writes,
     * then SRC_TRACE_ENT[]}. Lets us compare arcconf's (working) query sequence vs maxView's. */
    if (ioc->Signature[0]=='S' && ioc->Signature[1]=='R' && ioc->Signature[2]=='C' &&
        ioc->Signature[3]=='D' && ioc->Signature[4]=='B' && ioc->Signature[5]=='G') {
        ULONG cap = (Srb->DataTransferLength > ioc->HeaderLength)
                  ? Srb->DataTransferLength - ioc->HeaderLength : 0;
        if (ioc->ControlCode == 0) {
            InterlockedExchange(&dev->TraceIdx, 0);
        } else if (cap >= 16) {
            ULONG total  = (ULONG)dev->TraceIdx;
            ULONG navail = (total < SRC_TRACE_N) ? total : SRC_TRACE_N;
            ULONG room   = (cap - 16) / sizeof(SRC_TRACE_ENT);
            ULONG ncopy  = (navail < room) ? navail : room;
            *(PULONG)(p + 0) = 0x54435253u;            /* 'SRCT' */
            *(PULONG)(p + 4) = total;
            /* CISS status (survives the trace-ring flood): bit0=CissAvail, mgmt (ch,id,lun). */
            *(PULONG)(p + 8) = (dev->CissAvail ? 1u : 0u) | ((ULONG)dev->CissMgmtCh << 8) |
                               ((ULONG)dev->CissMgmtId << 16) | ((ULONG)dev->CissMgmtLun << 24);
            /* bit0 = MSI-X active; [15:8] = MSI-X diag probe rounds; [23:16] = MSI-X ISR fire count */
            *(PULONG)(p + 12) = (dev->MsiEnabled ? 1u : 0u)
                              | (((ULONG)dev->MsixMaxMsg & 0xFFu) << 8)   /* max MSI-X MessageId seen */
                              | (((ULONG)dev->MsixIsrCount & 0xFFu) << 16)
                              | (dev->AdapterOptions & 0xFF000000u);      /* opt bits 24-31: TYPE1..4;
                                                                          * bit 30 (0x40000000)=V8 */
            if (ncopy) {
                RtlCopyMemory(p + 16, dev->Trace, (SIZE_T)ncopy * sizeof(SRC_TRACE_ENT));
            }
        }
        ioc->ReturnCode = (ULONG)dev->TraceIdx;
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        return TRUE;
    }

    /* (A diagnostic kind-20 "MGMTIN" capture lived here in 0.1.0.201 to record every mgmt code
     * maxView sends. Result: maxView uses ONLY 0x42003/0x4202B/0x42033/0x4203B/0x42043/0x42047/
     * 0x42260/0x42264 (all AACAPI, all handled) - never 0x42007/0x42037 (H6), ACI2 0x1B05xx (M9)
     * or CSMISAS; and its 0x42264 completions are non-fast with status=0 (M8/M10 never trigger).
     * So those audit items are confirmed non-issues; the capture was removed. srctrace kind 20
     * stays defined for future use.) */

    /* --- AAC management API (maxView / arcconf), signature "AACAPI" --- */
    if (ioc->Signature[0] == 'A' && ioc->Signature[1] == 'A' && ioc->Signature[2] == 'C' &&
        ioc->Signature[3] == 'A' && ioc->Signature[4] == 'P' && ioc->Signature[5] == 'I') {
        ULONG code = ioc->ControlCode;

        if (code == 0x42003) {
            /* ping / keepalive */
            ioc->ReturnCode = 1;
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            return TRUE;
        }
        if (code == 0x4203b) {
            /* open session: the vendor stores (payload[0]==0) as an "opened" gate;
             * arcconf sends payload[0]=1. Just accept. */
            dev->MgmtOpen = TRUE;
            if (InterlockedIncrement(&g_dbgIoctl) <= IOCTL_DBG_LIMIT) {
                SRCDBG("srcraid: MGMT open   inlen=%u hdrLen=%u len=%u\n",
                       Srb->DataTransferLength, ioc->HeaderLength, ioc->Length);
            }
            ioc->ReturnCode = 1;
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            return TRUE;
        }
        if (code == 0x42033 || code == 0x42268) {
            /* capability/parameter negotiation. 0x42268 is the newer alias the vendor
             * routes into the SAME handler as 0x42033. Handling only 0x42033 made a newer
             * maxView/arcconf (which negotiates via 0x42268) read back zero caps -> the
             * cap3=0 state that triggers the 0x4200f flood. Treat both codes identically.
             *
             * The payload is a struct: [0]=selector, [0x100]=status we fill, [0x104]/
             * [0x108]=values. Verified on HW (0.1.0.61): this gets arcconf past the
             * version check (was "Driver Update Required" -> now reaches "Inaccessible",
             * i.e. it now wants real config data). Bounds-checked against the ACTUAL
             * buffer (DataTransferLength), not the tool-claimed Length. */
            if (Srb->DataTransferLength >= ioc->HeaderLength + 0x10c) {
                PULONG sel  = (PULONG)(p);
                PULONG st   = (PULONG)(p + 0x100);
                PULONG v104 = (PULONG)(p + 0x104);
                PULONG v108 = (PULONG)(p + 0x108);
                ULONG  c    = *sel;
                const ULONG cap0 = 1;   /* host interface present */
                const ULONG cap3 = 1;   /* AIF/async-event capability (set to 1 on this
                                         * controller). sel=0 status -> 0x0f (vs our old 0x07 with
                                         * cap3=0). This is the ONE 42033 cap bit that diverged from
                                         * the vendor: with cap3=0 maxView busy-polls 0x4200f ~1300/s
                                         * (an AIF event FLOOD that races the multi-threaded agent and
                                         * intermittently crashes it); with cap3=1 maxView takes the
                                         * vendor's event path and does not flood. Verified by capturing
                                         * both drivers' 42033 responses on hardware. */
                if (c == 0) {
                    ULONG v = (*st & 0xFFFFFFFEu) | (cap0 & 1) | 6;
                    v ^= (((cap3 << 3) ^ v) & 8);
                    *st = v;
                } else {
                    if (c & 1) { *st ^= ((cap0 ^ *st) & 1); }
                    if (c & 8) { *st ^= (((cap3 << 3) ^ *st) & 8); }
                    if (c & 2) { *st |= 2; *v108 = 0x40000; *v104 = 0x800; }  /* v104 = max FIB/xfer size (vendor [dev+0x618c]=0x800); was wrongly DeviceId */
                    if (c & 4) { *st |= 4; *v104 = 0x10; }
                }
            }
            if (InterlockedIncrement(&g_dbgIoctl) <= IOCTL_DBG_LIMIT) {
                SRCDBG("srcraid: MGMT cap    inlen=%u hdrLen=%u len=%u sel=%08x\n",
                       Srb->DataTransferLength, ioc->HeaderLength, ioc->Length,
                       (Srb->DataTransferLength >= ioc->HeaderLength + 4) ? *(PULONG)p : 0xffffffff);
            }
            ioc->ReturnCode = 1;
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            return TRUE;
        }
        if (code == 0x4202b) {
            /* FSACTL_MINIPORT_REV_CHECK: report the driver API
             * revision + build so the tool's driver-compatibility gate passes.
             * Vendor values: RevGetExternalRev=0x07050400, RevGetBuildNumber=0xD2FD.
             * (The 2014 arcconf 1.7 bailed on this - too old - but the maxView
             * arcconf handles it.) */
            if (Srb->DataTransferLength >= ioc->HeaderLength + 0x18) {
                PULONG pl = (PULONG)p;
                pl[3] = 1;             /* +0xc compatibility = compatible */
                pl[4] = 0x07050400;    /* +0x10 external revision */
                pl[5] = 0x0000d2fd;    /* +0x14 build number */
            }
            if (InterlockedIncrement(&g_dbgIoctl) <= IOCTL_DBG_LIMIT) {
                SRCDBG("srcraid: MGMT revchk inlen=%u hdrLen=%u len=%u\n",
                       Srb->DataTransferLength, ioc->HeaderLength, ioc->Length);
            }
            ioc->ReturnCode = 1;
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            return TRUE;
        }
        if (code == 0x42260) {
            /* RAW FIB PASSTHROUGH: the payload is an
             * AAC FIB - header at p+0, Command at +4, Size at +8 = DATA length,
             * FIB data at +0x20. Relay it to the firmware via the sync mailbox and
             * copy the response FIB back. This is the primitive arcconf/maxView use
             * for adapter-info / container-config / phys queries. Short timeout
             * because HwStartIo may run at DISPATCH. */
            /* FAITHFUL raw-FIB passthrough. The
             * payload IS an AAC FIB: header at +0 (Command@+4, Size@+8 = DATA
             * length), data at +0x20. Copy the WHOLE user FIB verbatim into slot-0,
             * fix only the delivery fields (Size += 0x20 like the vendor, XferState/
             * StructType/SenderFibAddress), SEND_SYNCHRONOUS_FIB, then copy the WHOLE
             * response FIB back so the tool sees the firmware's processed header +
             * data. Short timeout (HwStartIo may be at DISPATCH). */
            {
                ULONG plSize = (Srb->DataTransferLength > ioc->HeaderLength)
                             ? Srb->DataTransferLength - ioc->HeaderLength : 0;
                if (plSize >= sizeof(AAC_FIBHDR) && dev->Ready) {
                    /* ASYNC passthrough (1:1 with the vendor driver's submit path): submit the
                     * caller's FIB on the inbound queue and leave the SRB PENDING; the ISR
                     * (SrcIoDrainRrq, kind SRC_FIBK_MGMT) copies the whole response FIB back
                     * to this buffer and completes it. ReturnCode=1 is set now - the ISR's
                     * copy-back only touches the FIB at +HeaderLength, not this header. No
                     * busy-wait, no MgmtBusy serialization: each passthrough rides its own
                     * slot, exactly like the vendor. */
                    ioc->ReturnCode = 1;
                    if (SrcMgmtSubmit(dev, Srb, p, plSize)) {
                        return FALSE;              /* pending; completed by the ISR */
                    }
                    /* no free slot: tell the tool to retry */
                    Srb->SrbStatus = SRB_STATUS_BUSY;
                    return TRUE;
                }
            }
            ioc->ReturnCode = 1;
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            return TRUE;
        }
        if (code == 0x42043) {
            /* OPEN/REGISTER ADAPTER-FIB CONTEXT (maxView sends it once at startup to
             * register for async adapter events). The vendor returns a 216-byte
             * adapter/context struct (driver name, PCI ids, firmware build, caps,
             * adapter options). Return our faithful copy of it so the agent builds a
             * complete controller/context object (previously we zeroed this). */
            ULONG have = (Srb->DataTransferLength > ioc->HeaderLength)
                       ? Srb->DataTransferLength - ioc->HeaderLength : 0;
            ULONG n = (have < sizeof(g_ctx43Template)) ? have : sizeof(g_ctx43Template);
            if (n > 0) {
                RtlCopyMemory(p, g_ctx43Template, n);
                if (have > n) {
                    RtlZeroMemory(p + n, have - n);
                }
            }
            dev->AifCtxOpen = TRUE;             /* agent now wants AIF events */
            ioc->ReturnCode = 1;
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            return TRUE;
        }
        if (code == 0x4200f) {
            /* GET_NEXT_ADAPTER_FIB: hand the agent the next queued adapter event (AIF) if
             * one is pending, else signal "no event".
             *
             * ROOT CAUSE of the maxView 0x4200F flood (identified 2026-11-15 from the SRC
             * interface + a live capture): the vendor writes the
             * EVENT-STATUS into the SRB_IO_CONTROL *ControlCode* field (DataBuffer[4]):
             *   ControlCode = 2  -> an event FIB follows (storarc processes it, polls again)
             *   ControlCode = 0  -> NO event        (storarc STOPS draining)
             * (That's why the vendor's 0x4200F replies show up in a user-mode hook as
             * code=00002 / code=00000 - the driver overwrote ControlCode.) Our old code left
             * ControlCode = 0x4200f (non-zero) and only wrote a 0xffffffff "sentinel" into the
             * payload, which storarc ignores - so it read every reply as "event present" and
             * polled 0x4200F in a tight unbounded loop (~1300/s), starving discovery. Setting
             * ControlCode=0 on the no-event path stops the drain loop (and =2 when we actually
             * deliver an event once the AIF channel is live). */
            ULONG   have = (Srb->DataTransferLength > ioc->HeaderLength)
                         ? Srb->DataTransferLength - ioc->HeaderLength : 0;
            BOOLEAN gotEvent = FALSE;
            if (have > 0) {
                if (SrcAifDeliver(dev, p, have)) {
                    gotEvent = TRUE;                        /* event FIB copied into p */
                } else {
                    RtlZeroMemory(p, have);
                    if (have >= 8) {
                        *(PULONG)(p + 4) = 0xFFFFFFFFu;     /* keep legacy payload sentinel too */
                    }
                }
            }
            ioc->ControlCode = gotEvent ? 2u : 0u;          /* <-- the fix storarc keys off */
            ioc->ReturnCode  = 1;
            Srb->SrbStatus   = SRB_STATUS_SUCCESS;
            return TRUE;
        }
        if (code == 0x42264 || code == 0x4201b) {
            /* SEND_RAW_SRB. 0x4201B is the same raw-SRB path in the vendor (shared with
             * 0x42264), just the non-large-buffer variant - the aac_srb request layout is
             * identical, so we handle both. The maxView agent sends a raw SCSI/
             * ATA passthrough to a physical device - INQUIRY, VPD pages, and ATA
             * PASS-THROUGH(12) (CDB[0]=0xA1) for SMART/IDENTIFY. Request structure
             * (mapped from a live capture on HW):
             *   +0x00 u16 length(0x58 hdr)   +0x05 u8 channel   +0x06 u8 targetId
             *   +0x0a u8 cdbLen              +0x0b u8 (data-off component)
             *   +0x0c u32 flags (0x40=DataIn / 0x80=DataOut)
             *   +0x10 u32 dataLen           +0x14 u32 timeout   +0x48 u8 cdb[16]
             *   data buffer at payload + ((u16@0 + u8@0xb + 7) & ~3)  [4-byte align, not 8]
             * We EXECUTE it for real via the aac_srb sync mailbox and copy the genuine
             * result data + transfer length back, so the GUI gets real physical-device
             * detail and the agent is never handed garbage (the earlier zeroed stub). */
            ULONG have = (Srb->DataTransferLength > ioc->HeaderLength)
                       ? Srb->DataTransferLength - ioc->HeaderLength : 0;
            ULONG ret  = 6;                         /* default: not executed */
            ULONG xfer = 0;
            /* PREFERRED PATH (vendor parity): submit ASYNC on an I/O slot like the vendor driver, so a
             * slow-drive SMART poll no longer busy-waits at DISPATCH / masks outbound interrupts
             * / holds MgmtBusy (which stalled block-I/O completions under load). The ISR
             * (SRC_FIBK_RAWIOCTL) copies the result back + sets ReturnCode. Falls through to the
             * sync mailbox below only when async can't be used (no free slot, or an oversized
             * transfer - rare for INQUIRY/SMART). */
            if (have >= 0x58 && dev->Ready && SrcRawSrbSubmit(dev, Srb, p, have)) {
                return FALSE;                       /* pending; completed by the ISR */
            }
            /* require dev->Ready: during ScsiRestartAdapter bring-up (Ready=FALSE) the
             * mailbox is used UNLOCKED by SrcBringUp; without this guard a SEND_RAW_SRB
             * arriving in that window would take MgmtBusy and ring the sync doorbell
             * concurrently -> clobbered bring-up / bogus SMART. The rescans already
             * bail on !Ready; this makes 0x42264 match them. */
            if (have >= 0x58 && dev->AdapterFibs != NULL && dev->Ready) {
                UCHAR   chan    = p[0x05];
                UCHAR   id      = p[0x06];
                UCHAR   lun     = p[0x07];   /* CANDIDATE lun offset - relayed like the vendor
                                              * passes pSrb->Lun (cousin of the SrcPhysExecute
                                              * multi-LUN relay). For the lun-0 devices here this
                                              * must be 0; if the firmware then rejects (ret=6) this
                                              * byte is NOT the lun and we revert to a hard 0. */
                UCHAR   cdbLen  = p[0x0a];
                ULONG   flags   = *(PULONG)(p + 0x0c);
                ULONG   dataLen = *(PULONG)(p + 0x10);
                /* data offset: 4-byte aligned, NOT 8 (verified vs arcsas - vendor places the
                 * SEND_RAW_SRB data at (len+byte[0x0b]+7)&~3 = 0x5c, ours was &~7 = 0x58, so maxView
                 * read our SMART attribute table 4 bytes misaligned -> garbled/repeating IDs). */
                ULONG   dataOff = ((ULONG)(*(PUSHORT)(p + 0x00)) + (ULONG)p[0x0b] + 7) & ~3u;
                BOOLEAN dataIn  = (flags & AAC_SRB_DataIn)  ? TRUE : FALSE;
                BOOLEAN dataOut = (flags & AAC_SRB_DataOut) ? TRUE : FALSE;
                const ULONG scratchSlot = 102;
                PUCHAR scratch     = dev->AdapterFibs + (SIZE_T)scratchSlot * SRC_FIB_SIZE;
                ULONG  scratchPhys = dev->AdapterFibsPhys + scratchSlot * SRC_FIB_SIZE;

                if (cdbLen == 0 || cdbLen > 16) cdbLen = 16;
                if (dataLen > SRC_FIB_SIZE) dataLen = SRC_FIB_SIZE;
                /* the data region must lie inside the supplied IOCTL buffer */
                if (dataLen == 0 ||
                    (dataOff >= 0x58 && (ULONGLONG)dataOff + dataLen <= (ULONGLONG)have)) {
                    ULONG sc;
                    while (InterlockedCompareExchange(&dev->MgmtBusy, 1, 0) != 0) {
                        StorPortStallExecution(20);
                    }
                    RtlZeroMemory(scratch, SRC_FIB_SIZE);
                    if (dataOut && dataLen) {
                        RtlCopyMemory(scratch, p + dataOff, dataLen);
                    }
#ifndef SRCRAID_RAWSRB_MAILBOX
#define SRCRAID_RAWSRB_MAILBOX 1   /* RE-ENABLED 0.1.0.117: the 112 diagnostic DISPROVED the mailbox
                                    * as the wedge cause (112 had it OFF and still wedged; the wedge
                                    * was the mgmt-FIB StructType=5, fixed in 115). maxView needs this
                                    * for per-disk INQUIRY/SMART to resolve array-member SSDs (and show
                                    * the array). 1 = execute the real passthrough. TODO: convert to
                                    * ASYNC inbound-queue delivery (like SrcPhysExecute) to drop the
                                    * DISPATCH-level busy-wait in SrcSrbSync. */
#endif
#if SRCRAID_RAWSRB_MAILBOX
                    sc = SrcSrbSync(dev, chan, id, lun, p + 0x48, cdbLen,
                                    scratchPhys, dataLen, dataIn, 300);
#else
                    sc = SRC_SYNC_FAILED;   /* no runtime mailbox; SMART/INQUIRY returns ret=6 */
#endif
                    if (sc == SRC_SYNC_OK) {
                        PAAC_SRB_REPLY rep =
                            (PAAC_SRB_REPLY)((PUCHAR)dev->AdapterFibs + sizeof(AAC_FIBHDR));
                        xfer = rep->dataXferLength;
                        if (xfer > dataLen) xfer = dataLen;
                        if (dataIn && dataLen) {
                            RtlZeroMemory(p + dataOff, dataLen);
                            if (xfer) RtlCopyMemory(p + dataOff, scratch, xfer);
                        }
                        *(PULONG)(p + 0x10) = xfer;      /* report actual transfer */
                        ret = 0;                         /* vendor success = 0 */
                    } else {
                        if (dataIn && dataLen) RtlZeroMemory(p + dataOff, dataLen);
                        *(PULONG)(p + 0x10) = 0;
                        ret = 6;
                    }
                    InterlockedExchange(&dev->MgmtBusy, 0);
                }
            }
            if (InterlockedIncrement(&g_dbgRaw) <= IOCTL_DBG_LIMIT) {
                SRCDBG("srcraid: MGMT RAW_SRB id=%u cdb0=0x%02x xfer=%u -> ret=%u\n",
                       (have >= 0x58) ? p[0x06] : 0xff,
                       (have >= 0x58) ? p[0x48] : 0, xfer, ret);
            }
            /* DIAGNOSTIC in-memory trace: SEND_RAW_SRB (kind 3). a=chan<<8|id, b=cdb0, c=ret,
             * d0=xfer, d1=have, d2=candidate-lun byte p[0x07] (confirm it is 0 for these lun-0
             * devices while ret stays 0 -> p[0x07] really is the lun). */
            SrcTrace(dev, 3,
                     (have >= 0x58) ? (((ULONG)p[0x05] << 8) | p[0x06]) : 0xffff,
                     (have >= 0x58) ? p[0x48] : 0, ret, xfer, have,
                     (have >= 0x58) ? p[0x07] : 0xff, 0);
            ioc->ReturnCode = ret;
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            return TRUE;
        }
        if (code == 0x42047) {
            /* FSACTL lookup: container INDEX by ctUid. maxView passes a firmware container UID
             * at payload[0] and
             * reads back the Windows SCSI target (= our cid/TargetId) at payload[1], or
             * 0xFFFFFFFF if no exposed container carries that UID. ctUid == the firmware's
             * 32-bit container UID (CT cmd 165), stored per-container in Container.CtUid and
             * matched in FULL - the SAME value the firmware reports in GET_CONFIG, so maxView's
             * lookup resolves. (A 24-bit-truncated match missed for any UID with a non-zero high
             * byte and over-indexed an internal array in the maxView agent -> the GET_CONFIG heap overrun. Earlier
             * still we returned a FIXED {0,1} -> every volume mapped onto target 1.) Trace the
             * key + our CtUids (kind 12) so a capture can confirm maxView's UID == our CtUid. */
            ULONG have = (Srb->DataTransferLength > ioc->HeaderLength)
                       ? Srb->DataTransferLength - ioc->HeaderLength : 0;
            ULONG key = (have >= 4) ? *(PULONG)p : 0;
            ULONG idx = 0xFFFFFFFFu, i;
            for (i = 0; i < SRC_MAX_CONTAINERS; i++) {
                PSRC_CONTAINER c2 = &dev->Containers[i];
                if (c2->Valid && !c2->Hidden && c2->CtUid == key) {
                    idx = i;
                    break;
                }
            }
            if (have >= 8) {
                *(PULONG)(p + 4) = idx;
            }
            SrcTrace(dev, 12, key, idx, 1,
                     dev->Containers[0].Valid ? dev->Containers[0].CtUid : 0xffffffffu,
                     dev->Containers[1].Valid ? dev->Containers[1].CtUid : 0xffffffffu,
                     dev->Containers[2].Valid ? dev->Containers[2].CtUid : 0xffffffffu,
                     dev->Containers[3].Valid ? dev->Containers[3].CtUid : 0xffffffffu);
            if (InterlockedIncrement(&g_dbgAif) <= IOCTL_DBG_LIMIT) {
                SRCDBG("srcraid: MGMT 0x42047 ctUid=0x%x -> target=%d\n", key, (LONG)idx);
            }
            ioc->ReturnCode = 1;
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            return TRUE;
        }
        if (code == 0x42023) {
            /* FSACTL get container identity:
             * payload[0] = container index (= our cid = TargetId). If exposed/ready, write
             * payload[1] = AdapterContainerNo (firmware container number = cid) and, unless
             * ioc->Length == 8, payload[2] = ctUid (firmware oid). rc=1 if present, else 9. */
            ULONG have = (Srb->DataTransferLength > ioc->HeaderLength)
                       ? Srb->DataTransferLength - ioc->HeaderLength : 0;
            ULONG idx  = (have >= 4) ? *(PULONG)p : 0xFFFFFFFFu;
            ULONG rc   = 9;
            if (idx < SRC_MAX_CONTAINERS && dev->Containers[idx].Valid &&
                !dev->Containers[idx].Hidden) {
                if (have >= 8) {
                    *(PULONG)(p + 4) = idx;                              /* AdapterContainerNo */
                }
                if (ioc->Length != 8 && have >= 12) {
                    *(PULONG)(p + 8) = dev->Containers[idx].CtUid;                  /* ctUid (full 32-bit) */
                }
                rc = 1;
            }
            SrcTrace(dev, 12, idx | 0x80000000u, rc,
                     (idx < SRC_MAX_CONTAINERS) ? dev->Containers[idx].CtUid : 0,
                     0, 0, 0, 0);
            ioc->ReturnCode = rc;
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            return TRUE;
        }
        if (code == 0x42017) {
            /* FSACTL present/unpresent a logical device: payload[2] = ctUid, payload[3] = new
             * state (1=present/show,
             * 2=ready, 3=unpresent/hide, 4=no-op). Find the container with that oid, apply
             * the hide/show (Container.Hidden -> the SCSI target disappears/reappears), and
             * re-enumerate bus 0 (BusChangeDetected) so Windows follows. rc=1 on success,
             * 9 if the UID isn't found. */
            ULONG have = (Srb->DataTransferLength > ioc->HeaderLength)
                       ? Srb->DataTransferLength - ioc->HeaderLength : 0;
            ULONG rc = 9;
            if (have >= 16) {
                ULONG key   = *(PULONG)(p + 8);
                ULONG state = *(PULONG)(p + 12);
                ULONG i;
                if (state == 4) {
                    rc = 1;                                             /* no-op */
                } else {
                    for (i = 0; i < SRC_MAX_CONTAINERS; i++) {
                        PSRC_CONTAINER c2 = &dev->Containers[i];
                        if (c2->Valid && c2->CtUid == key) {
                            c2->Hidden = (state == 3) ? TRUE : FALSE;  /* 3=hide, 1/2=show */
                            StorPortNotification(BusChangeDetected, dev, (UCHAR)SRC_CONTAINER_BUS);
                            rc = 1;
                            break;
                        }
                    }
                }
            }
            if (InterlockedIncrement(&g_dbgAif) <= IOCTL_DBG_LIMIT) {
                SRCDBG("srcraid: MGMT 0x42017 present/unpresent -> rc=%u\n", rc);
            }
            ioc->ReturnCode = rc;
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            return TRUE;
        }
        if (code == 0x4226c) {
            /* IOCTL debug MESSAGE (the vendor logs it as "* - IOCTL MESSAGE: %s"): the tool
             * passes a NUL-terminated string in the payload; log it and ack. */
            ULONG have = (Srb->DataTransferLength > ioc->HeaderLength)
                       ? Srb->DataTransferLength - ioc->HeaderLength : 0;
            if (have > 0) {
                p[have - 1] = 0;                                        /* force NUL-terminate */
                SRCDBG("srcraid: MGMT MESSAGE: %s\n", (char *)p);
            }
            ioc->ReturnCode = 1;
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            return TRUE;
        }
        if (code == 0x4200b || code == 0x42013) {
            /* 0x4200B: register an adapter-FIB context (vendor succeeds when the context id
             * fits a byte). 0x42013: register OS management callbacks and "start" the
             * adapter. Our adapter is already started in HwInitialize and we deliver AIF
             * events through 0x4200F, so both are a straight success ack. */
            ioc->ReturnCode = 1;
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            return TRUE;
        }
        if (code == 0x4202f) {
            /* FSACTL version/signature match: version-match
             * flag @ payload+132, code-stream identifier(32) @ +136, firmware-match
             * string(64) @ +200 - lets the tool's driver/firmware version pairing pass. */
            ULONG have = (Srb->DataTransferLength > ioc->HeaderLength)
                       ? Srb->DataTransferLength - ioc->HeaderLength : 0;
            if (have >= 136 + 33) {
                *(PULONG)(p + 132) = 1;                                 /* versions match */
                RtlCopyMemory(p + 136, "This is a code stream identifier", 32);
                p[136 + 32] = 0;
            }
            if (have >= 200 + 64) {
                RtlZeroMemory(p + 200, 64);
                RtlCopyMemory(p + 200, "srcraid", 7);
            }
            ioc->ReturnCode = 1;
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            return TRUE;
        }
        if (code == 0x4203f) {
            /* FSAMPCTL_RESET_IOP: the tool asks for an IOP
             * reset. Our controller self-recovers (resume + wedge paths re-init via the
             * restart timer), and we deliberately do NOT tear down a live, healthy adapter
             * with active I/O from a DISPATCH-level IOCTL - that live-reset path is untested
             * and would drop outstanding SRBs. Acknowledge so the tool proceeds; it then
             * re-enumerates and finds the healthy controller. */
            SRCDBG("srcraid: MGMT 0x4203F RESET_IOP acknowledged (no live teardown)\n");
            ioc->ReturnCode = 1;
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            return TRUE;
        }
        if (code == 0x42270) {
            /* the vendor rejects 0x42270 (rc=6). Match it so the tool treats the
             * feature as unsupported instead of mis-reading a faked success. */
            ioc->ReturnCode = 6;
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            return TRUE;
        }

        /* Remaining vendor codes are FIB-send variants that maxView/arcconf do NOT use
         * (they drive all firmware FIBs through 0x42260 instead): 0x42007/0x42037 (SEND
         * API FIB, FIB embedded at payload+28), 0x42027 (fire-and-forget sync FIB),
         * 0x4201F (container-config sync command). Each embeds a firmware FIB at a
         * DIFFERENT offset than 0x42260, so rather than risk delivering a malformed FIB to
         * the controller for a path no tool exercises here, they fall through to the
         * zero+ack default below. If a tool ever needs one, port its exact payload offset
         * and route through SrcMgmtSubmit like 0x42260.
         *
         * Any other AACAPI code we don't service.
         * Return code: the vendor rejects an unhandled/unsupported code with ReturnCode=6
         * (DataBuffer[5]=6), NOT success - so we do the
         * same (was a faked rc=1). maxView tolerates 6 (it's exactly what the real driver
         * returns for these), treating the feature as unavailable instead of parsing a
         * fake-success result. 5=busy/reset, 9=failure, 0=async-pending are the other
         * vendor codes; 6 is the right one for "I don't implement this code".
         * PRINCIPLED SAFETY (independent of the code): zero the response buffer first, so
         * the multi-threaded maxView agent can NEVER parse the stale request bytes left in
         * the buffer as a result. Stale bytes = a bogus embedded length -> the JNI/afaapi
         * layer over-copies -> JVM heap corruption -> tomcat9 crash. The only codes that
         * reach here are the FIB-send variants above, which no tool sends. */
        {
            ULONG have = (Srb->DataTransferLength > ioc->HeaderLength)
                       ? Srb->DataTransferLength - ioc->HeaderLength : 0;
            if (InterlockedIncrement(&g_dbgIoctl) <= IOCTL_DBG_LIMIT) {
                if (have >= 8) {
                    SRCDBG("srcraid: MGMT *** UNHANDLED *** code=0x%08x inlen=%u hdrLen=%u len=%u pl=%02x %02x %02x %02x %02x %02x %02x %02x (zeroed, rc=6)\n",
                           code, Srb->DataTransferLength, ioc->HeaderLength, ioc->Length,
                           p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7]);
                } else {
                    SRCDBG("srcraid: MGMT *** UNHANDLED *** code=0x%08x inlen=%u hdrLen=%u len=%u (payload<8, zeroed, rc=6)\n",
                           code, Srb->DataTransferLength, ioc->HeaderLength, ioc->Length);
                }
            }
            if (have > 0) {
                RtlZeroMemory(p, have);
            }
        }
        ioc->ReturnCode = 6;                         /* vendor: unsupported/unhandled code */
        Srb->SrbStatus = SRB_STATUS_SUCCESS;         /* the IOCTL transport itself succeeded */
        return TRUE;
    }

    /* --- CSMI (SAS) management interface, signature "CSMISAS" (the vendor's CSMI handler).
     * Used by generic CSMI-aware HBA tools; maxView/arcconf use AACAPI, not CSMI.
     * We service the identification codes (driver info, controller status); the
     * detailed RAID/PHY topology codes report "not supported" so a CSMI tool
     * degrades gracefully instead of parsing empty data. Spec-based (CSMI 0.81),
     * bounded writes - no CSMI client was available to validate against. The vendor
     * driver dispatches codes 1..0x1b via a jump table; out-of-range -> rc=2. */
    if (ioc->Signature[0]=='C' && ioc->Signature[1]=='S' && ioc->Signature[2]=='M' &&
        ioc->Signature[3]=='I' && ioc->Signature[4]=='S' && ioc->Signature[5]=='A' &&
        ioc->Signature[6]=='S') {
        ULONG ccode = ioc->ControlCode;
        ULONG have  = (Srb->DataTransferLength > ioc->HeaderLength)
                    ? Srb->DataTransferLength - ioc->HeaderLength : 0;
        ioc->ReturnCode = 0;                      /* CSMI_SAS_STATUS_SUCCESS */
        if (ccode == 1 && have >= 174) {          /* CC_CSMI_SAS_GET_DRIVER_INFO */
            RtlZeroMemory(p, 174);
            RtlCopyMemory(p, "srcraid", 7);                         /* szName[81] */
            RtlCopyMemory(p + 81, "Adaptec Series 7 RAID (srcraid)", 31); /* szDescription[81] */
            *(PUSHORT)(p + 162) = 7;              /* usMajorRevision */
            *(PUSHORT)(p + 164) = 5;              /* usMinorRevision */
            *(PUSHORT)(p + 166) = 0;              /* usBuildRevision */
            *(PUSHORT)(p + 168) = (USHORT)(54013 & 0xffff); /* usReleaseRevision */
            *(PUSHORT)(p + 170) = 0;              /* usCSMIMajorRevision */
            *(PUSHORT)(p + 172) = 81;             /* usCSMIMinorRevision (0.81) */
        } else if (ccode == 3 && have >= 36) {    /* CC_CSMI_SAS_GET_CNTLR_STATUS */
            RtlZeroMemory(p, 36);
            *(PULONG)(p + 0) = 1;                 /* uStatus = CSMI_SAS_CNTLR_STATUS_GOOD */
            /* uOfflineReason = 0 */
        } else {
            ioc->ReturnCode = 2;                  /* CSMI_SAS_STATUS_BAD_CNTL_CODE (not serviced) */
        }
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        return TRUE;
    }

    if (InterlockedIncrement(&g_dbgIoctl) <= IOCTL_DBG_LIMIT) {
        UCHAR s[9];
        ULONG i;
        for (i = 0; i < 8; i++) {
            UCHAR c = ioc->Signature[i];
            s[i] = (c >= 0x20 && c < 0x7f) ? c : '.';
        }
        s[8] = 0;
        /* skip the Windows storage-stack IOCTLs (SMART/firmware/hybrid) - only the
         * AAC management signature matters for maxView/arcconf */
        SRCDBG("srcraid: IOCTL sig=%s [%02x %02x %02x %02x %02x %02x %02x %02x] "
               "code=0x%08x hdrLen=%u len=%u xfer=%u\n", s,
               ioc->Signature[0], ioc->Signature[1], ioc->Signature[2], ioc->Signature[3],
               ioc->Signature[4], ioc->Signature[5], ioc->Signature[6], ioc->Signature[7],
               ioc->ControlCode, ioc->HeaderLength, ioc->Length, Srb->DataTransferLength);
        SRCDBG("  payload[0..31]=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
               "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
               "%02x %02x %02x %02x\n",
               p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11],
               p[12], p[13], p[14], p[15], p[16], p[17], p[18], p[19], p[20], p[21], p[22], p[23],
               p[24], p[25], p[26], p[27], p[28], p[29], p[30], p[31]);
    }

    /* not serviced yet - just observe */
    ioc->ReturnCode = 1;                        /* generic failure for the tool */
    Srb->SrbStatus = SRB_STATUS_SUCCESS;        /* the IOCTL transport succeeded */
    return TRUE;
}
