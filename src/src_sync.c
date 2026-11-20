/*
 * src_sync.c - SRC mailbox sync-command channel.
 *
 * The pre-queue control path to the firmware: write command+args to the mailbox,
 * ring the inbound doorbell, poll the outbound doorbell for completion, read the
 * result, ack. Used for the comm-init handshake (INIT_STRUCT_BASE_ADDRESS) and
 * adapter/container queries before the FIB queues exist.
 *
 * Offsets derived from the SRC interface & HW-verified (see docs/):
 *   mailbox      = regbase + 0x1000  (Series 7/8;  Series 6 = +0x7fc60)
 *   ring         = IDR(0x20)        = 0x200  (INBOUND_DOORBELL_0 << 9)
 *   complete     = ODR_STATUS(0x9c) bit 12
 *   ack          = ODR_CLEAR(0xa0)  = 0x1000 (OUTBOUND_DOORBELL_0 << 12)
 *   OIMR(0x34)   masked during the command, restored after
 * Matches Linux aacraid src_sync_cmd().
 */
#include "srcraid.h"

VOID
SrcSetupMailbox(_In_ PDEVICE_EXTENSION dev)
{
    ULONG off;

    switch (dev->DeviceId) {
    case SRC_DID_SERIES6:
        off = SRC_MAILBOX_OFF_S6;
        break;
    case SRC_DID_SERIES7:
    case SRC_DID_SERIES8:
    default:
        off = SRC_MAILBOX_OFF_S7S8;
        break;
    }
    dev->Mailbox = dev->Regs + off;
}

ULONG
SrcSyncCommand(
    _In_ PDEVICE_EXTENSION dev,
    _In_ ULONG command,
    _In_ ULONG p1, _In_ ULONG p2, _In_ ULONG p3,
    _In_ ULONG p4, _In_ ULONG p5, _In_ ULONG p6,
    _Out_opt_ PULONG status, _Out_opt_ PULONG r1,
    _Out_opt_ PULONG r2, _Out_opt_ PULONG r3, _Out_opt_ PULONG r4,
    _In_ ULONG timeoutMs
    )
{
    PULONG mb = (PULONG)dev->Mailbox;
    ULONG  savedOimr;
    ULONG  iters = timeoutMs * 10;   /* 100 us / iteration */
    ULONG  odr;

    if (mb == NULL) {
        return SRC_SYNC_FAILED;
    }

    /* command + up to 6 args into Mailbox[0..6] */
    StorPortWriteRegisterUlong(dev, &mb[0], command);
    StorPortWriteRegisterUlong(dev, &mb[1], p1);
    StorPortWriteRegisterUlong(dev, &mb[2], p2);
    StorPortWriteRegisterUlong(dev, &mb[3], p3);
    StorPortWriteRegisterUlong(dev, &mb[4], p4);
    StorPortWriteRegisterUlong(dev, &mb[5], p5);
    StorPortWriteRegisterUlong(dev, &mb[6], p6);

    /* clear any stale sync-done doorbell */
    SrcWrite32(dev, SRC_REG_ODR_CLEAR, SRC_ODB_SYNC_DONE);

    /* mask outbound (doorbell) interrupts for the duration; force the write out */
    savedOimr = SrcRead32(dev, SRC_REG_OIMR);
    SrcWrite32(dev, SRC_REG_OIMR, 0xFFFFFFFF);
    (void)SrcRead32(dev, SRC_REG_OIMR);

    /* ring: a new sync command is ready */
    SrcWrite32(dev, SRC_REG_IDR, SRC_IDB_SYNC);

    /* wait for the firmware to raise the sync-done doorbell */
    for (;;) {
        odr = SrcRead32(dev, SRC_REG_ODR_STATUS);
        if (odr & SRC_ODB_SYNC_DONE) {
            break;
        }
        if (iters == 0) {
            SrcWrite32(dev, SRC_REG_OIMR, savedOimr);
            return SRC_SYNC_TIMEOUT;
        }
        iters--;
        StorPortStallExecution(100);
    }

    /* ack the completion */
    SrcWrite32(dev, SRC_REG_ODR_CLEAR, SRC_ODB_SYNC_DONE);

    /* pull status + results from the mailbox */
    if (status) { *status = StorPortReadRegisterUlong(dev, &mb[0]); }
    if (r1)     { *r1     = StorPortReadRegisterUlong(dev, &mb[1]); }
    if (r2)     { *r2     = StorPortReadRegisterUlong(dev, &mb[2]); }
    if (r3)     { *r3     = StorPortReadRegisterUlong(dev, &mb[3]); }
    if (r4)     { *r4     = StorPortReadRegisterUlong(dev, &mb[4]); }

    /* restore the interrupt mask */
    SrcWrite32(dev, SRC_REG_OIMR, savedOimr);

    return SRC_SYNC_OK;
}
