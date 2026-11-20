/*
 * src_init.c - SRC firmware bring-up: health check and wait-for-ready.
 *
 * Derived from the SRC interface (cross-checked with Linux aacraid aac_src_check_health).
 *
 * Firmware status lives in OMR0 (SRC_REG_OMR0). A running Series 7 reads
 * 0x00010080: bit 7 (FW_UP_AND_RUNNING) set. 0xFFFFFFFF means the device isn't
 * answering on the PCI bus (powered down / config not restored). Bit 8
 * (FW_KERNEL_PANIC) or bit 2 (FW_SELF_TEST_FAILED) mean the firmware is wedged.
 */
#include "srcraid.h"

/*
 * Poll OMR0 until the firmware reports up-and-running. Mirrors the vendor
 * driver's boot-wait loop: fast-fail on self-test-failed / panic, then spin
 * (100 us/iter) until (status != 0xFFFFFFFF) && (status & UP_AND_RUNNING).
 */
BOOLEAN
SrcWaitForFwReady(
    _In_ PDEVICE_EXTENSION dev,
    _In_ ULONG timeoutMs
    )
{
    ULONG status;
    ULONG iters;

    status = SrcFwStatus(dev);
    if (status != 0xFFFFFFFF) {
        if (status & FW_SELF_TEST_FAILED) {
            return FALSE;
        }
        if (status & FW_KERNEL_PANIC) {
            return FALSE;
        }
    }

    iters = timeoutMs * 10;   /* 10 iterations per ms at 100 us each */
    for (;;) {
        status = SrcFwStatus(dev);
        if ((status != 0xFFFFFFFF) && (status & FW_UP_AND_RUNNING)) {
            return TRUE;
        }
        if (iters == 0) {
            return FALSE;
        }
        iters--;
        StorPortStallExecution(100);   /* 100 microseconds */
    }
}

/*
 * Full IOP reset (aacraid aac_send_iop_reset): reboot the firmware to a clean
 * baseline. The vendor does this at the start of init so the
 * host-I/O subsystem starts in a known state; a fresh POST otherwise leaves the
 * firmware in the option-ROM's state. Also recovers a wedged sync channel (P4).
 *
 * Sequence: notify via mailbox IOP_RESET_ALWAYS, clear OMR0 (so a fresh
 * KERNEL_UP is detectable), write IDR = IOP_SRC_RESET_MASK (raw 0x100) to
 * trigger, then poll OMR0 for UP_AND_RUNNING (skipping KERNEL_BOOTING). Runs at
 * DISPATCH during HwInitialize, so the wait is a bounded busy-wait.
 */
/*
 * Non-blocking IOP reset TRIGGER: write the reset registers and return immediately,
 * WITHOUT the ~1 s settle + up-to-60 s kernel-reboot wait that SrcIopReset does. Used
 * by the deferred restart timer (DISPATCH), which can't afford a long busy-wait - its
 * own per-tick fw-ready poll detects when the kernel comes back after this kick.
 */
VOID
SrcIopResetTrigger(
    _In_ PDEVICE_EXTENSION dev
    )
{
    SRCDBG("srcraid: IOP reset TRIGGER (non-blocking) OMR0=0x%08x\n", SrcFwStatus(dev));
    SrcWrite32(dev, SRC_REG_OMR0, 0);             /* clear so post-reset KERNEL_UP is observable */
    (void)SrcRead32(dev, SRC_REG_OMR0);
    SrcWrite32(dev, SRC_REG_IDR, IOP_SRC_RESET_MASK);
}

BOOLEAN
SrcIopReset(
    _In_ PDEVICE_EXTENSION dev
    )
{
    ULONG omr, i;

    SRCDBG("srcraid: IOP reset: OMR0=0x%08x -> resetting\n", SrcFwStatus(dev));

    /* best-effort notify (may time out mid-reset; ignore) */
    (void)SrcSyncCommand(dev, IOP_RESET_ALWAYS, 0, 0, 0, 0, 0, 0,
                         NULL, NULL, NULL, NULL, NULL, 2000);

    /* clear OMR0 so the post-reset KERNEL_UP transition is observable */
    SrcWrite32(dev, SRC_REG_OMR0, 0);
    (void)SrcRead32(dev, SRC_REG_OMR0);

    /* trigger the IOP reset (raw mask to the inbound doorbell) */
    SrcWrite32(dev, SRC_REG_IDR, IOP_SRC_RESET_MASK);

    /* settle ~1 s before the firmware status is meaningful */
    for (i = 0; i < 10000; i++) {
        StorPortStallExecution(100);
    }

    /* Wait up to ~60 s (aacraid SOFT_RESET_TIME) for the kernel to reboot;
     * early-exit as soon as it is up. Runs at PASSIVE_LEVEL (no DPC watchdog),
     * polling every 1 ms. The firmware transits KERNEL_BOOTING -> UP_AND_RUNNING. */
    for (i = 0; i < 60000; i++) {
        omr = SrcFwStatus(dev);
        if (omr == 0xFFFFFFFF) {
            omr = 0;
        }
        if ((omr & FW_UP_AND_RUNNING) && !(omr & FW_KERNEL_BOOTING)) {
            SRCDBG("srcraid: IOP reset OK after ~%u ms, OMR0=0x%08x\n", 1000 + i, omr);
            return TRUE;
        }
        StorPortStallExecution(1000);   /* 1 ms */
    }
    SRCDBG("srcraid: IOP reset: firmware not up after 60s, OMR0=0x%08x\n", SrcFwStatus(dev));
    return FALSE;
}

/*
 * Read firmware health. Returns the raw OMR0;
 * if a kernel panic is flagged, *panicCode (optional) receives the panic code
 * from bits 16..23.
 */
ULONG
SrcCheckHealth(
    _In_ PDEVICE_EXTENSION dev,
    _Out_opt_ PULONG panicCode
    )
{
    ULONG status = SrcFwStatus(dev);

    if (panicCode != NULL) {
        *panicCode = (status & FW_KERNEL_PANIC) ? ((status >> 16) & 0xff) : 0;
    }
    return status;
}
