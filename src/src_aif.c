/*
 * src_aif.c - adapter-initiated event (AIF) channel.
 *
 * The firmware notifies the host of adapter events (drive hot-plug, container/
 * logical-device changes, physical-device state, rebuild progress, enclosure/
 * temperature) through Adapter-Initiated FIBs (AIFs). The delivery model:
 *
 *   - The driver keeps ONE AifRequest FIB outstanding (FIB.Command = 700 / 0x2bc,
 *     request data[0] = 200 / 0xc8 "want events"). The firmware HOLDS it and
 *     COMPLETES it through the normal host RRQ when an event occurs - i.e. the AIF
 *     arrives as an ordinary slot completion, not a separate push.
 *   - On completion we copy the event FIB into a small ring and immediately
 *     re-post a fresh AifRequest (so we never miss the next event). A ~10s timer
 *     re-posts as a safety net.
 *   - maxView polls GET_NEXT_ADAPTER_FIB (0x4200f); SrcAifDeliver hands it the
 *     next queued event FIB verbatim.
 *
 * The request lives in a reserved FIB slot (SRC_AIF_SLOT) whose IoSlotSrb[] entry
 * is pinned non-NULL so the I/O allocator never reuses it; SrcIoDrainRrq routes
 * that slot's completion here instead of to an SRB.
 */
#include "srcraid.h"

static LONG g_aifRx = 0;   /* rate-limit the AIF-event diagnostic log */

/* Build and submit the AifRequest FIB into SRC_AIF_SLOT via the inbound queue,
 * exactly like an I/O FIB (same XferState/StructType/handle scheme) so the
 * firmware completes it back through the host RRQ. */
VOID
SrcAifPost(_In_ PDEVICE_EXTENSION dev)
{
    PAAC_FIBHDR fib;
    ULONG fibPhys, fibSize, fibSize128, iqLow;

    if (dev->AdapterFibs == NULL || !dev->Ready) {
        return;
    }
    /* keep exactly one outstanding */
    if (InterlockedCompareExchange(&dev->AifReqPending, 1, 0) != 0) {
        return;
    }

    fib     = (PAAC_FIBHDR)(dev->AdapterFibs + (SIZE_T)SRC_AIF_SLOT * SRC_FIB_SIZE);
    fibPhys = dev->AdapterFibsPhys + SRC_AIF_SLOT * SRC_FIB_SIZE;
    fibSize = sizeof(AAC_FIBHDR) + 4;                  /* header + 4-byte request */

    RtlZeroMemory(fib, SRC_FIB_SIZE);
    /* AIF long-poll delivery (0.1.0.119): same inbound-queue bits as the working mgmt FIBs
     * but WITHOUT XS_FastResponseCapable (bit 19), which the firmware requires to HOLD the
     * request instead of fast-completing it. With bit 19 SET
     * (the old 0x830ad) the firmware FAST-COMPLETES the AifRequest immediately -> no events;
     * with it CLEARED the firmware HOLDS the request (cmd 700) and completes it only when an
     * adapter event occurs -> real AIF delivery. StructType=4 (the proven IQ delivery; the old
     * 0x30a0+magic5 attempt is what wedged, not the 0x30a0 bits). Result = 0x000030ad. */
    fib->XferState          = (XS_SEND_SYNC & ~XS_FastResponseCapable) | XS_Async;
    fib->Command            = (USHORT)FIBCMD_AifRequest;
    fib->StructType         = FIB_MAGIC2;              /* 4 - new-comm inbound delivery */
    fib->Size               = (USHORT)fibSize;
    fib->SenderSize         = (USHORT)SRC_FIB_SIZE;
    fib->SenderFibAddress   = fibPhys;
    fib->ReceiverFibAddress = 0;
    fib->Handle             = (SRC_AIF_SLOT << 2) + 1;
    *(PULONG)((PUCHAR)fib + sizeof(AAC_FIBHDR)) = AIF_REQ_EVENT;   /* data[0] = 200 */

    KeMemoryBarrier();

    fibSize128 = (fibSize + 127) / 128 - 1;
    if (fibSize128 > 31) fibSize128 = 31;
    iqLow = (fibPhys & ~0x7Fu) | fibSize128;

    SrcWrite32(dev, SRC_REG_IQ_H, 0);
    SrcWrite32(dev, SRC_REG_IQ_L, iqLow);
    SrcTrace(dev, 5, 0 /*post*/, 0, 0, 0, 0, 0, 0);   /* DIAGNOSTIC: AIF request posted */
}

/* Called from the RRQ drain when SRC_AIF_SLOT's handle completes: the firmware
 * has written an event FIB into the slot. Copy it into the delivery ring and
 * re-arm by posting a fresh AifRequest. Runs at DISPATCH (ISR). */
VOID
SrcAifOnComplete(_In_ PDEVICE_EXTENSION dev)
{
    PUCHAR  fib = dev->AdapterFibs + (SIZE_T)SRC_AIF_SLOT * SRC_FIB_SIZE;
    PULONG  d   = (PULONG)(fib + sizeof(AAC_FIBHDR));
    ULONG   aifCmd = d[0];                 /* aac_aifcmd.command (0 = no event) */
    LONG    tail;

    /* the held request has been consumed either way */
    InterlockedExchange(&dev->AifReqPending, 0);
    SrcTrace(dev, 5, 1 /*complete*/, aifCmd, (ULONG)dev->AifQTail,
             d[0], d[1], d[2], d[3]);   /* DIAGNOSTIC: AIF completion (event or empty) */

    if (aifCmd == 0) {
        /* an empty/"no event" completion (seen when the firmware answers the poll
         * without an event). Do NOT queue it and do NOT re-post inline - the ~10s
         * timer re-arms. (Inline re-posting here was the 26k-completion busy loop.) */
        return;
    }

    /* A real adapter event arrived. A config/topology change (volume or disk
     * added/removed) may have altered the device set, so ask the timer to re-sweep
     * and re-probe (the sweep touches the sync mailbox and must not run in this ISR;
     * the timer runs it at DISPATCH as a short RescanTicks burst once it commits).
     *
     * But DON'T rescan on pure PROGRESS events: a RAID rebuild emits class-2
     * AifCmdJobProgress continuously, and rescanning on each would hold MgmtBusy in
     * back-to-back 256-probe sweeps, starving block I/O and maxView.
     *
     * We previously DEFAULTED to rescan (skipping only JobProgress + temperature). That was wrong:
     * under load the firmware STREAMS status events (e.g. aifCmd 1 sub 0x17 enclosure/thermal, with
     * a changing counter) and we rescanned on every one - back-to-back synchronous probe FIBs from
     * this DISPATCH timer that time out under load and stall I/O (the RND4K stutter that lines up
     * with the AIF EVENT log lines). Instead we now mirror the vendor driver's ALLOW-LIST and
     * rescan ONLY on a genuine topology change; every other event is still buffered for the agent
     * (below) but does NOT kick a rescan. aif data[0]=cmd (d[0]), data[8]=sub (d[2]),
     * data[20]=EM type (d[5]). */
    {
        ULONG   sub  = d[2];
        BOOLEAN topo = FALSE;
        if (aifCmd == 1) {                       /* AifCmdEventNotify */
            topo = (sub == 0x04 ||               /* container config change    */
                    sub == 0x0F ||               /* container added            */
                    sub == 0x10 ||               /* container removed          */
                    sub == 0x1E ||               /* JBOD added                 */
                    sub == 0x1F);                /* JBOD removed               */
            if (sub == 0x0D) {                   /* enclosure mgmt: only real device-change types */
                ULONG emType = d[5];
                topo = (emType == 26 || (emType >= 31 && emType <= 33));
            }
        } else if (aifCmd == 4) {                /* AifCmdDriverNotify (raw/HBA device, morph) */
            topo = (sub == 0xC8 ||               /* morph complete             */
                    sub == 0xCB ||               /* raw device removed         */
                    sub == 0xCC ||               /* native HBA device added    */
                    sub == 0xCD);                /* native HBA device removed  */
        } else if (aifCmd != 2) {
            topo = TRUE;                         /* unknown cmd class (not 1/2/4): rescan to be safe */
        }
        /* aifCmd == 2 (JobProgress) never kicks a rescan by itself. */
        if (topo) {
            InterlockedExchange(&dev->RescanPending, 1);
        }
    }

    if (InterlockedIncrement(&g_aifRx) <= 32) {
        PAAC_FIBHDR h = (PAAC_FIBHDR)fib;
        SRCDBG("srcraid: *** AIF EVENT *** cmd=%u size=%u aifcmd=%u seq=%u data=%08x %08x %08x %08x\n",
               (ULONG)h->Command, (ULONG)h->Size, aifCmd, d[1], d[2], d[3], d[4], d[5]);
    }

    /* Queue a verbatim copy of the event FIB (header + up to SRC_AIF_ENTRY bytes).
     * This ISR path is the SOLE writer of AifQTail and no longer touches AifQHead:
     * writing the slot then bumping only the tail makes it a single-producer ring, so
     * there is no Exchange/Increment race with the consumer over the head cursor. The
     * ring is lossy by design - if the agent falls more than SRC_AIF_QDEPTH behind, the
     * oldest copies are overwritten here and the consumer skips to the oldest survivor
     * (see SrcAifDeliver). */
    tail = dev->AifQTail;
    RtlCopyMemory(dev->AifQ[tail % SRC_AIF_QDEPTH], fib, SRC_AIF_ENTRY);
    KeMemoryBarrier();
    InterlockedIncrement(&dev->AifQTail);
    /* NOTE: we deliberately do NOT re-post here. The timer re-arms (<=3s), which
     * structurally caps the request rate - inline re-posting is what let a stream
     * of fast completions wedge the controller. */
}

/* GET_NEXT_ADAPTER_FIB (0x4200f): hand the agent the next queued event FIB.
 * Returns TRUE and fills dst (up to cap bytes) if an event is queued, else FALSE
 * ("no fib pending"). */
BOOLEAN
SrcAifDeliver(_In_ PDEVICE_EXTENSION dev, _Out_writes_bytes_(cap) PUCHAR dst, _In_ ULONG cap)
{
    ULONG n = (cap < SRC_AIF_ENTRY) ? cap : SRC_AIF_ENTRY;

    /* AifHead is advanced ONLY here (the ISR producer touches only AifQTail), so there
     * is no producer/consumer race on the head cursor. Concurrent pollers (the agent is
     * multi-threaded) are serialized by the CompareExchange claim: each caller atomically
     * takes exactly one index and retries if another thread moved head first. If the
     * producer has lapped us (more than SRC_AIF_QDEPTH unread), skip to the oldest entry
     * still present in the ring instead of handing back an overwritten slot. */
    for (;;) {
        LONG head  = dev->AifQHead;
        LONG tail  = dev->AifQTail;
        LONG claim;

        if (head >= tail) {
            return FALSE;                              /* queue empty */
        }
        claim = (tail - head > SRC_AIF_QDEPTH) ? (tail - SRC_AIF_QDEPTH) : head;
        if (InterlockedCompareExchange(&dev->AifQHead, claim + 1, head) != head) {
            continue;                                  /* another poller advanced head; retry */
        }
        RtlCopyMemory(dst, dev->AifQ[claim % SRC_AIF_QDEPTH], n);
        if (cap > n) {
            RtlZeroMemory(dst + n, cap - n);
        }
        return TRUE;
    }
}

/* StorPort timer: safety re-post (the firmware normally completes->we re-post on
 * each event, but if a post was ever lost this re-arms within ~10s). */
static VOID
SrcAifTimer(_In_ PVOID DeviceExtension)
{
    PDEVICE_EXTENSION dev = (PDEVICE_EXTENSION)DeviceExtension;
    if (dev->Ready) {
        /* Liveness watchdog. On this firmware each AifRequest is completed almost
         * immediately - empty (aifCmd=0) when there is no event, with the real event FIB
         * when there is one (confirmed by trace: tight post->empty-complete pairs, ~one
         * per 3s tick). SrcAifOnComplete clears AifReqPending, so at a normal tick it is
         * already 0 and the else branch re-posts - AifIdleTicks never accumulates. The
         * watchdog therefore fires ONLY if completions STOP for ~60s (a lost post, or a
         * firmware re-init that dropped the request without a driver bring-up to reset
         * AifReqPending): it reclaims the slot and forces a fresh post so the event
         * channel can't die silently. Because a healthy firmware always completes within a
         * tick, a 60s silence means the request really is gone - this never re-posts over a
         * request the firmware still holds (which the vendor driver guards against). */
        if (dev->AifReqPending) {
            if (InterlockedIncrement(&dev->AifIdleTicks) >= SRC_AIF_WATCHDOG_TICKS) {
                dev->AifIdleTicks = 0;
                InterlockedExchange(&dev->AifReqPending, 0);   /* reclaim the stale slot */
                SRCDBG("srcraid: AIF watchdog - request silent ~60s, re-arming\n");
                SrcAifPost(dev);
            }
        } else {
            dev->AifIdleTicks = 0;
            SrcAifPost(dev);                           /* normal idle re-arm */
        }

        /* hot re-enumeration: on a config-change event, re-sweep the physical bus
         * and (if the exposed set changed) tell StorPort to re-probe it, so a disk
         * uninitialized -> raw at runtime appears in Windows without a reboot.
         * RescanTicks spreads the (sync-mailbox) sweep over a few ticks after an event. */
        /* A config-change event schedules a short burst of rescans (not just one):
         * the AIF can fire while the create/delete is still committing, so one
         * immediate rescan may see the old state. Re-scanning for the next few
         * ticks (~9s) reliably catches the settled config. */
        if (InterlockedExchange(&dev->RescanPending, 0) != 0) {
            dev->RescanTicks = 3;
        }
        if (dev->RescanTicks > 0) {
            dev->RescanTicks--;
            /* physical (pass-through) bus: raw disks added/removed at runtime */
#if SRC_TRANSPARENT_RELAY
            /* transparent relay: no driver-side sweep - just ask StorPort to re-probe the physical
             * bus; it re-INQUIRYs each target and the firmware reports the new present/absent set
             * (an added raw disk appears, a removed one selection-times-out and drops). */
            StorPortNotification(BusChangeDetected, dev, (UCHAR)SRC_PHYSICAL_BUS);
#else
            if (SrcRescanPhysical(dev)) {
                SRCDBG("srcraid: config change -> physical set changed; "
                       "BusChangeDetected on bus %u\n", SRC_PHYSICAL_BUS);
                StorPortNotification(BusChangeDetected, dev, (UCHAR)SRC_PHYSICAL_BUS);
            }
#endif
            /* logical (container) bus: RAID volumes created/deleted at runtime */
            if (SrcRescanContainers(dev)) {
                SRCDBG("srcraid: config change -> container set changed; "
                       "BusChangeDetected on bus %u\n", SRC_CONTAINER_BUS);
                StorPortNotification(BusChangeDetected, dev, (UCHAR)SRC_CONTAINER_BUS);
            }
        }
    }
    /* NOTE: the periodic FW time-of-day sync was removed - cmd 705 never completes on this
     * firmware's IQ sync channel, so SrcFibSubmitSync burned its full timeout spinning at DISPATCH
     * while holding MgmtBusy every ~15 min -> multi-hundred-ms lock-hold stalls under load. The
     * init struct already seeds the FW clock once; keeping it fresh isn't worth a blocking FIB from
     * a DISPATCH timer. If wanted later, do it async (fire-and-forget on an I/O slot, no wait). */
    StorPortNotification(RequestTimerCall, dev, SrcAifTimer, 3000000);   /* 3s */
}

/* Bring-up: pin the reserved AIF slot so the I/O allocator skips it, post the
 * first AifRequest, and start the safety timer. */
VOID
SrcAifInit(_In_ PDEVICE_EXTENSION dev)
{
    dev->AifReqPending = 0;
    dev->AifIdleTicks = 0;
    dev->AifQHead = 0;
    dev->AifQTail = 0;
    dev->AifCtxOpen = FALSE;
    dev->RescanPending = 0;
    dev->RescanTicks = 0;

    /* AIF CHANNEL RE-ENABLED (0.1.0.126). The 0.1.0.119 "crash" was really the 0x4200F FLOOD (maxView
     * spun because our 0x4200F left ControlCode=0x4200f instead of 0/2) - FIXED in 0.1.0.124
     * (ioc->ControlCode = gotEvent?2:0, src_mgmt.c). With the flood gone, re-enabling AIF lets the
     * firmware's CHANGE events (e.g. a config/cache write) flow: SrcAifPost holds an AifRequest, the
     * firmware completes it with the event FIB on a change, SrcAifOnComplete queues it, and the
     * 0x4200F handler delivers it to maxView with ControlCode=2. Verified the vendor's event is a
     * plain AIF FIB (Command=700, data[0]=aifCmd=1) - exactly what SrcAifDeliver hands back. maxView
     * needs these to REFRESH its display after a change (writes already apply; they just don't show).
     * Empty completions (aifCmd=0, poll-model idle) are not queued -> 0x4200F returns ControlCode=0. */
    SrcAifPost(dev);
    StorPortNotification(RequestTimerCall, dev, SrcAifTimer, 3000000);   /* 3s re-arm */
    SRCDBG("srcraid: AIF channel ENABLED (0x4200F ControlCode fix in place; events deliver on change)\n");
}
