/*
 * src_phys.c - physical (pass-through) device support.
 *
 * Step 1 (this build): a read-only enumeration DIAGNOSTIC. It (a) dumps the
 * RequestAdapterInfo response - the controller's management device address (its
 * channel,id,lun) is carried in the adapter info, so it should be visible here -
 * and (b) probes CISS REPORT PHYSICAL LUNS
 * (CDB 0xC3) via the sync mailbox across a few candidate controller addresses,
 * dumping the aac_srb reply + the first bytes of the returned LUN list. From the
 * log we learn the real mgmt address and the device-list layout, then wire up
 * presentation (a physical bus) + aac_srb I/O (ScsiPortCommand64, 601).
 *
 * aac_srb (StructType=1) delivered by SEND_SYNCHRONOUS_FIB, exactly like the
 * vendor driver's physical-device query (an aac_srb built + sent synchronously).
 * See docs/management-api.md for the raw-SRB / pass-through surface.
 */
#include "srcraid.h"

/*
 * Send one aac_srb (SCSI passthrough / CISS) through the synchronous mailbox and
 * leave the aac_srb_reply in the FIB data area. Data (if any) DMAs to dataPhys.
 * Uses slot 0 (serialized, init-time only). Returns SRC_SYNC_*.
 */
ULONG
SrcSrbSync(
    _In_ PDEVICE_EXTENSION dev,
    _In_ ULONG channel,
    _In_ ULONG id,
    _In_ ULONG lun,
    _In_reads_bytes_(cdbLen) const UCHAR *cdb,
    _In_ ULONG cdbLen,
    _In_ ULONG dataPhys,
    _In_ ULONG dataLen,
    _In_ BOOLEAN dataIn,
    _In_ ULONG timeoutMs
    )
{
    PAAC_FIBHDR fib = (PAAC_FIBHDR)dev->AdapterFibs;                 /* slot 0 */
    PAAC_SRB    srb = (PAAC_SRB)((PUCHAR)fib + sizeof(AAC_FIBHDR));
    ULONG fibPhys = dev->AdapterFibsPhys;
    ULONG fibSize, sc, st = 0;

    if (dev->AdapterFibs == NULL) {
        return SRC_SYNC_FAILED;
    }

    RtlZeroMemory(fib, SRC_FIB_SIZE);
    srb->function   = SRBF_ExecuteScsi;
    srb->channel    = channel;
    srb->id         = id;
    srb->lun        = lun;
    srb->timeout    = 0;
    srb->flags      = dataIn ? AAC_SRB_DataIn : AAC_SRB_DataOut;
    srb->count      = dataLen;
    srb->retryLimit = 0;
    srb->cdbSize    = cdbLen;
    RtlCopyMemory(srb->cdb, cdb, (cdbLen > 16) ? 16 : cdbLen);
    srb->sgCount         = 1;
    srb->sg[0].addrLow   = dataPhys;
    srb->sg[0].addrHigh  = 0;
    srb->sg[0].len       = dataLen;

    fibSize = sizeof(AAC_FIBHDR) + AAC_SRB_FIXED + sizeof(AAC_SGENTRY64);

    /* sync-mailbox delivery (vendor-style sync FIB): StructType=1 (FIB_MAGIC),
     * no handle, host-owned; firmware processes in place + raises sync-done. */
    fib->XferState        = XS_HostOwned | XS_FibInitialized | XS_FibEmpty |
                            XS_SentFromHost | XS_NormalPriority;    /* 0x102d */
    fib->Command          = (USHORT)FIBCMD_ScsiPortCommand64;      /* 601 */
    fib->StructType       = FIB_MAGIC;                             /* 1 */
    fib->Size             = (USHORT)fibSize;
    fib->SenderSize       = (USHORT)SRC_FIB_SIZE;
    fib->SenderFibAddress = fibPhys;
    fib->Handle           = 0;

    KeMemoryBarrier();

    sc = SrcSyncCommand(dev, SEND_SYNCHRONOUS_FIB, fibPhys,
                        0, 0, 0, 0, 0, &st, NULL, NULL, NULL, NULL, timeoutMs);
    return sc;
}

/* Classify one (channel,id,lun) with an INQUIRY through the sync mailbox:
 *   ABSENT   - no device (selection timeout / no INQUIRY data)
 *   MEMBER   - present but claimed by a container (qualifier != 0; don't expose raw)
 *   PRESENT  - accessible pass-through device (qualifier 0; *typeOut = device type)
 *   MBOXDEAD - the sync mailbox itself timed out (firmware wedged) */
typedef enum { SRC_PP_ABSENT = 0, SRC_PP_PRESENT, SRC_PP_MEMBER, SRC_PP_MBOXDEAD } SRC_PHYS_PROBE;

static SRC_PHYS_PROBE
SrcPhysProbe(
    _In_ PDEVICE_EXTENSION dev, _In_ ULONG ch, _In_ ULONG id, _In_ ULONG lun,
    _In_ PUCHAR scratch, _In_ ULONG scratchPhys, _In_ PAAC_SRB_REPLY rep,
    _In_ ULONG timeoutMs, _Out_ PUCHAR typeOut
    )
{
    UCHAR cdb[6];
    ULONG rc;

    RtlZeroMemory(cdb, sizeof(cdb));
    cdb[0] = 0x12;                          /* INQUIRY */
    cdb[4] = 36;                            /* allocation length */
    RtlZeroMemory(scratch, 64);
    rc = SrcSrbSync(dev, ch, id, lun, cdb, 6, scratchPhys, 36, TRUE, timeoutMs);
    if (rc == SRC_SYNC_TIMEOUT) {
        return SRC_PP_MBOXDEAD;             /* mailbox didn't answer - firmware wedged */
    }
    if (rc != SRC_SYNC_OK || rep->srbStatus == 0x0a) {
        return SRC_PP_ABSENT;               /* selection timeout: no device here */
    }
    if (rep->dataXferLength == 0 && scratch[0] == 0 && scratch[8] == 0) {
        return SRC_PP_ABSENT;               /* no INQUIRY data */
    }
    if ((UCHAR)(scratch[0] >> 5) != 0) {
        return SRC_PP_MEMBER;               /* qualifier != 0: claimed by a container */
    }
    if (typeOut) {
        *typeOut = (UCHAR)(scratch[0] & 0x1f);   /* peripheral device type */
    }
    return SRC_PP_PRESENT;
}

/*
 * Probe for the controller's CISS management device - the endpoint the vendor
 * driver sends CISS REPORT PHYSICAL LUNS (CDB C3 02 .. alloc) to in
 * native-HBA mode. We don't know its (channel,id,lun) a-priori (it lives in the vendor's
 * device extension, populated only in HBA mode), so we probe the (channel,id) space with the CISS
 * CDB and look for a response carrying the CISS table marker (data[4] == 2) and a sane
 * byte count. Harmless: C3 is a read-type vendor command; a disk that isn't the mgmt
 * device just rejects it. On this RAID-mode box we EXPECT to find nothing (CissAvail
 * stays FALSE -> SrcEnumPhysical uses the INQUIRY sweep). Diagnostic traces: kind 10 =
 * CISS table found, kind 11 = a device responded but not with a CISS table.
 */
VOID
SrcCissDiscover(
    _In_ PDEVICE_EXTENSION dev
    )
{
    const ULONG scratchSlot = 101;
    PUCHAR scratch     = dev->AdapterFibs + (SIZE_T)scratchSlot * SRC_FIB_SIZE;
    ULONG  scratchPhys = dev->AdapterFibsPhys + scratchSlot * SRC_FIB_SIZE;
    PAAC_SRB_REPLY rep = (PAAC_SRB_REPLY)((PUCHAR)dev->AdapterFibs + sizeof(AAC_FIBHDR));
    const ULONG allocLen = 2000;            /* fits the 2048-byte scratch; enough to detect */
    UCHAR  cdb[12];
    ULONG  ch, id, rc, responders = 0;

    dev->CissAvail  = FALSE;
    dev->CissMgmtCh = 0xFF;

    RtlZeroMemory(cdb, sizeof(cdb));
    cdb[0] = 0xC3;                          /* CISS REPORT PHYSICAL LUNS */
    cdb[1] = 0x02;                          /* extended report (24-byte entries) */
    cdb[8] = (UCHAR)(allocLen >> 8);        /* allocation length, big-endian */
    cdb[9] = (UCHAR)(allocLen & 0xFF);

    for (ch = 0; ch < 4 && !dev->CissAvail; ch++) {
        for (id = 0; id < 64; id++) {
            RtlZeroMemory(scratch, 64);
            rc = SrcSrbSync(dev, ch, id, 0, cdb, 12, scratchPhys, allocLen, TRUE, 200);
            if (rc != SRC_SYNC_OK || rep->srbStatus == 0x0a) {
                continue;                   /* no device at this address */
            }
            if (scratch[4] == 2) {          /* CISS valid-table marker */
                ULONG cnt = ((ULONG)scratch[0] << 24) | ((ULONG)scratch[1] << 16) |
                            ((ULONG)scratch[2] << 8)  |  (ULONG)scratch[3];
                SrcTrace(dev, 10, (ch << 8) | id, rep->srbStatus, cnt,
                         scratch[4], scratch[5], scratch[0x0e], scratch[0x0f]);
                SRCDBG("srcraid: CISS mgmt device FOUND at ch=%u id=%u (len=%u -> %u entries)\n",
                       ch, id, cnt, cnt / 24);
                dev->CissMgmtCh = (UCHAR)ch; dev->CissMgmtId = (UCHAR)id; dev->CissMgmtLun = 0;
                dev->CissAvail  = TRUE;
                break;
            }
            /* responded, but not a CISS table (a disk rejecting the vendor CDB) */
            if (responders < 24) {
                SrcTrace(dev, 11, (ch << 8) | id, rep->srbStatus, rep->dataXferLength,
                         scratch[0], scratch[4], 0, 0);
            }
            responders++;
        }
    }
    SRCDBG("srcraid: SrcCissDiscover -> CissAvail=%u mgmt=(%u,%u,%u) responders=%u\n",
           dev->CissAvail, dev->CissMgmtCh, dev->CissMgmtId, dev->CissMgmtLun, responders);
}

/* one physical address from the CISS list */
typedef struct { UCHAR ch, id, lun; } SRC_PA;

/*
 * Query CISS REPORT PHYSICAL LUNS from the discovered management device and fill out[]
 * with the present pass-through (channel,id,lun). Returns TRUE if CISS answered with a
 * valid table. Parse follows the vendor driver's layout: be32 byte-count @ resp[0],
 * marker resp[4]==2, 24-byte entries from resp+0xe (entry[0]=id (skip 0xFF), entry[1]&0x3f=
 * channel, entry[0xb]&8=present, entry[0xe..0x11]=nexus (!=0)). NOTE: untested on live HW
 * (no native-HBA controller available); dormant in RAID mode where CissAvail is FALSE, so
 * callers fall back to the INQUIRY sweep. Reuses scratch slot 101 (<=82 devices @ 2000 B).
 */
static BOOLEAN
SrcCissGetList(
    _In_ PDEVICE_EXTENSION dev, _Out_writes_(max) SRC_PA *out, _In_ ULONG max, _Out_ PULONG count
    )
{
    const ULONG scratchSlot = 101;
    PUCHAR scratch     = dev->AdapterFibs + (SIZE_T)scratchSlot * SRC_FIB_SIZE;
    ULONG  scratchPhys = dev->AdapterFibsPhys + scratchSlot * SRC_FIB_SIZE;
    const ULONG allocLen = 2000;
    UCHAR  cdb[12];
    ULONG  rc, nbytes, nent, k, n = 0;

    *count = 0;
    if (!dev->CissAvail) {
        return FALSE;
    }
    RtlZeroMemory(cdb, sizeof(cdb));
    cdb[0] = 0xC3; cdb[1] = 0x02;
    cdb[8] = (UCHAR)(allocLen >> 8); cdb[9] = (UCHAR)(allocLen & 0xFF);
    RtlZeroMemory(scratch, 64);
    rc = SrcSrbSync(dev, dev->CissMgmtCh, dev->CissMgmtId, dev->CissMgmtLun,
                    cdb, 12, scratchPhys, allocLen, TRUE, 500);
    if (rc != SRC_SYNC_OK || scratch[4] != 2) {
        return FALSE;
    }
    nbytes = ((ULONG)scratch[0] << 24) | ((ULONG)scratch[1] << 16) |
             ((ULONG)scratch[2] << 8)  |  (ULONG)scratch[3];
    nent = nbytes / 24;
    if (nent > (allocLen - 14) / 24) {
        nent = (allocLen - 14) / 24;       /* bound to the scratch buffer */
    }
    for (k = 0; k < nent && n < max; k++) {
        PUCHAR e = scratch + 14 + k * 24;
        ULONG  nexus = ((ULONG)e[0x11] << 24) | ((ULONG)e[0x10] << 16) |
                       ((ULONG)e[0x0f] << 8)  |  (ULONG)e[0x0e];
        if (e[0] == 0xFF || (e[0x0b] & 8) == 0 || nexus == 0) {
            continue;
        }
        out[n].ch = (UCHAR)(e[1] & 0x3F);
        out[n].id = e[0];
        out[n].lun = 0;
        n++;
    }
    *count = n;
    return TRUE;
}

/*
 * Enumerate physical (pass-through) devices by sweeping INQUIRY across the firmware
 * (channel,id,lun) space via the sync mailbox. A qualifier-0 device is accessible for
 * pass-through (not a container member) and is recorded in dev->Physicals[] / presented
 * on SRC_PHYSICAL_BUS. The sweep covers SRC_SWEEP_CHANNELS x SRC_SWEEP_IDS (wide enough
 * for high ids behind cascaded expanders) and, for each present target, probes
 * SRC_SWEEP_LUNS LUNs (SES / multi-LUN). Empty (channel,id) answer selection-timeout
 * fast; a wedged mailbox (consecutive MBOXDEAD) aborts the sweep rather than spinning.
 */
VOID
SrcEnumPhysical(
    _In_ PDEVICE_EXTENSION dev
    )
{
    const ULONG scratchSlot = 101;
    PUCHAR scratch     = dev->AdapterFibs + (SIZE_T)scratchSlot * SRC_FIB_SIZE;
    ULONG  scratchPhys = dev->AdapterFibsPhys + scratchSlot * SRC_FIB_SIZE;
    PAAC_SRB_REPLY rep = (PAAC_SRB_REPLY)((PUCHAR)dev->AdapterFibs + sizeof(AAC_FIBHDR));
    ULONG  ch, id, lun, mboxDead = 0;

    dev->NumPhysicals = 0;
    RtlZeroMemory(dev->Physicals, sizeof(dev->Physicals));
    if (!dev->ExposePhysicals) {
        SRCDBG("srcraid: physical pass-through disabled (ExposeNonDASD=0)\n");
        return;
    }

    /* Prefer the vendor-style CISS device list when a management device exists (native-HBA
     * mode). In RAID mode SrcCissDiscover finds nothing -> CissAvail=FALSE -> we fall through
     * to the INQUIRY sweep below (the proven path for "RAID (Expose RAW)" mode). CISS is used
     * only when it actually returns devices, so a dormant/empty CISS can't hide the disks. */
    SrcCissDiscover(dev);
    {
        SRC_PA list[SRC_MAX_PHYSICALS];
        ULONG  n = 0, k;
        if (SrcCissGetList(dev, list, SRC_MAX_PHYSICALS, &n) && n > 0) {
            for (k = 0; k < n && dev->NumPhysicals < SRC_MAX_PHYSICALS; k++) {
                PSRC_PHYSICAL p = &dev->Physicals[dev->NumPhysicals++];
                p->Valid = TRUE; p->Channel = list[k].ch; p->Id = list[k].id;
                p->Lun = list[k].lun; p->DeviceType = 0;
            }
            SRCDBG("srcraid: SrcEnumPhysical -> %u pass-through device(s) via CISS (bus %u)\n",
                   dev->NumPhysicals, SRC_PHYSICAL_BUS);
            return;
        }
    }

    for (ch = 0; ch < SRC_SWEEP_CHANNELS; ch++) {
        for (id = 0; id < SRC_SWEEP_IDS; id++) {
            UCHAR type = 0;
            SRC_PHYS_PROBE pp = SrcPhysProbe(dev, ch, id, 0, scratch, scratchPhys, rep, 300, &type);

            if (pp == SRC_PP_MBOXDEAD) {
                if (++mboxDead >= SRC_SWEEP_MBOX_ABORT) {
                    SRCDBG("srcraid: SrcEnumPhysical - mailbox wedged, aborting at ch=%u id=%u\n", ch, id);
                    goto done;
                }
                continue;
            }
            mboxDead = 0;
            if (pp != SRC_PP_PRESENT) {
                continue;                   /* ABSENT or MEMBER: nothing to expose */
            }
            /* lun 0 present: record it, then probe lun 1..N for extra LUNs (SES/multi-LUN) */
            for (lun = 0; lun < SRC_SWEEP_LUNS; lun++) {
                UCHAR t2 = type;
                if (lun != 0) {
                    SRC_PHYS_PROBE p2 = SrcPhysProbe(dev, ch, id, lun, scratch, scratchPhys, rep, 300, &t2);
                    if (p2 == SRC_PP_MBOXDEAD) { mboxDead++; break; }
                    mboxDead = 0;
                    if (p2 != SRC_PP_PRESENT) break;    /* no more LUNs on this target */
                }
                if (dev->NumPhysicals >= SRC_MAX_PHYSICALS) {
                    goto done;              /* array full */
                }
                {
                    PSRC_PHYSICAL p = &dev->Physicals[dev->NumPhysicals];
                    p->Valid = TRUE; p->Channel = (UCHAR)ch; p->Id = (UCHAR)id;
                    p->Lun = (UCHAR)lun; p->DeviceType = t2;
                    SRCDBG("srcraid: PHYS target %u = ch=%u id=%u lun=%u type=0x%x\n",
                           dev->NumPhysicals, ch, id, lun, t2);
                    dev->NumPhysicals++;
                }
            }
        }
    }
done:
    SRCDBG("srcraid: SrcEnumPhysical -> %u pass-through device(s) on bus %u\n",
           dev->NumPhysicals, SRC_PHYSICAL_BUS);
}

/*
 * Runtime re-sweep of the physical (pass-through) bus, driven by a config-change
 * AIF (e.g. a disk uninitialized -> raw at runtime). Unlike SrcEnumPhysical (a
 * one-shot init build), this must keep existing Windows target ids STABLE so a
 * mounted raw disk never moves: it is APPEND-ONLY. A newly-available (channel,id)
 * is appended at NumPhysicals (a fresh target id); a device that disappeared is
 * marked !Valid in place (its slot/id is retired, not reused, so nothing shifts).
 * The caller (SrcAifTimer) runs this at DISPATCH; we take MgmtBusy to serialize
 * the slot-0 FIB + sync mailbox against the 0x42264 mgmt path, and use a short
 * per-probe timeout to bound the busy-wait. Returns TRUE if the exposed set
 * changed (so the caller issues BusChangeDetected).
 */
BOOLEAN
SrcRescanPhysical(
    _In_ PDEVICE_EXTENSION dev
    )
{
    const ULONG scratchSlot = 101;
    PUCHAR scratch     = dev->AdapterFibs + (SIZE_T)scratchSlot * SRC_FIB_SIZE;
    ULONG  scratchPhys = dev->AdapterFibsPhys + scratchSlot * SRC_FIB_SIZE;
    PAAC_SRB_REPLY rep = (PAAC_SRB_REPLY)((PUCHAR)dev->AdapterFibs + sizeof(AAC_FIBHDR));
    ULONG   ch, id, lun, i, mboxDead = 0;
    BOOLEAN seen[SRC_MAX_PHYSICALS];
    BOOLEAN changed = FALSE, aborted = FALSE;

    if (!dev->ExposePhysicals || dev->AdapterFibs == NULL || !dev->Ready) {
        return FALSE;
    }
    RtlZeroMemory(seen, sizeof(seen));

    /* serialize slot-0 + sync mailbox against the mgmt (0x42264) path for the
     * whole sweep (each probe builds slot 0; the mgmt path must not interleave) */
    while (InterlockedCompareExchange(&dev->MgmtBusy, 1, 0) != 0) {
        StorPortStallExecution(20);
    }

    /* CISS path (native-HBA mode): re-read the device list and do the same append-only
     * match/retire as the sweep, so init (CISS) and rescan (CISS) keep stable target ids.
     * CissAvail is FALSE in RAID mode -> falls through to the sweep. */
    {
        SRC_PA list[SRC_MAX_PHYSICALS];
        ULONG  cn = 0, j;
        if (SrcCissGetList(dev, list, SRC_MAX_PHYSICALS, &cn) && cn > 0) {
            for (j = 0; j < cn; j++) {
                LONG found = -1;
                for (i = 0; i < dev->NumPhysicals; i++) {
                    if (dev->Physicals[i].Channel == list[j].ch &&
                        dev->Physicals[i].Id      == list[j].id &&
                        dev->Physicals[i].Lun     == list[j].lun) { found = (LONG)i; break; }
                }
                if (found >= 0) {
                    seen[found] = TRUE;
                    if (!dev->Physicals[found].Valid) { dev->Physicals[found].Valid = TRUE; changed = TRUE; }
                } else if (dev->NumPhysicals < SRC_MAX_PHYSICALS) {
                    PSRC_PHYSICAL p = &dev->Physicals[dev->NumPhysicals];
                    p->Valid = TRUE; p->Channel = list[j].ch; p->Id = list[j].id;
                    p->Lun = list[j].lun; p->DeviceType = 0;
                    seen[dev->NumPhysicals] = TRUE; dev->NumPhysicals++; changed = TRUE;
                }
            }
            goto retire;                        /* CISS supplied the present set */
        }
    }

    for (ch = 0; ch < SRC_SWEEP_CHANNELS && !aborted; ch++) {
        for (id = 0; id < SRC_SWEEP_IDS; id++) {
            UCHAR type = 0;
            SRC_PHYS_PROBE pp = SrcPhysProbe(dev, ch, id, 0, scratch, scratchPhys, rep, 60, &type);

            if (pp == SRC_PP_MBOXDEAD) {
                if (++mboxDead >= SRC_SWEEP_MBOX_ABORT) {
                    SRCDBG("srcraid: RESCAN - mailbox wedged, aborting at ch=%u id=%u\n", ch, id);
                    aborted = TRUE;         /* don't retire devices on an aborted sweep */
                    break;
                }
                continue;
            }
            mboxDead = 0;
            if (pp != SRC_PP_PRESENT) {
                continue;                   /* ABSENT or MEMBER */
            }
            for (lun = 0; lun < SRC_SWEEP_LUNS; lun++) {
                UCHAR t2 = type;
                LONG  found = -1;
                if (lun != 0) {
                    SRC_PHYS_PROBE p2 = SrcPhysProbe(dev, ch, id, lun, scratch, scratchPhys, rep, 60, &t2);
                    if (p2 == SRC_PP_MBOXDEAD) { mboxDead++; break; }
                    mboxDead = 0;
                    if (p2 != SRC_PP_PRESENT) break;
                }
                /* already presented at this (channel,id,lun)? */
                for (i = 0; i < dev->NumPhysicals; i++) {
                    if (dev->Physicals[i].Channel == (UCHAR)ch &&
                        dev->Physicals[i].Id      == (UCHAR)id &&
                        dev->Physicals[i].Lun     == (UCHAR)lun) {
                        found = (LONG)i;
                        break;
                    }
                }
                if (found >= 0) {
                    seen[found] = TRUE;
                    if (!dev->Physicals[found].Valid) {
                        dev->Physicals[found].Valid      = TRUE;   /* came back */
                        dev->Physicals[found].DeviceType = t2;
                        changed = TRUE;
                        SRCDBG("srcraid: RESCAN target %u (ch=%u id=%u lun=%u) back online\n",
                               (ULONG)found, ch, id, lun);
                    }
                } else if (dev->NumPhysicals < SRC_MAX_PHYSICALS) {
                    PSRC_PHYSICAL p = &dev->Physicals[dev->NumPhysicals];
                    p->Valid = TRUE; p->Channel = (UCHAR)ch; p->Id = (UCHAR)id;
                    p->Lun = (UCHAR)lun; p->DeviceType = t2;
                    seen[dev->NumPhysicals] = TRUE;     /* freshly added - not "gone" */
                    SRCDBG("srcraid: RESCAN new pass-through target %u = ch=%u id=%u lun=%u type=0x%x\n",
                           dev->NumPhysicals, ch, id, lun, t2);
                    dev->NumPhysicals++;
                    changed = TRUE;
                }
            }
        }
    }

    /* Retire devices not seen this sweep - but ONLY if the sweep ran to completion.
     * An aborted (mailbox-wedged) sweep saw nothing because the fw stopped answering,
     * not because the devices are gone, so retiring them would wrongly drop live disks. */
retire:
    if (!aborted) {
        for (i = 0; i < dev->NumPhysicals; i++) {
            if (dev->Physicals[i].Valid && !seen[i]) {
                dev->Physicals[i].Valid = FALSE;
                changed = TRUE;
                SRCDBG("srcraid: RESCAN target %u (ch=%u id=%u lun=%u) removed\n",
                       i, dev->Physicals[i].Channel, dev->Physicals[i].Id, dev->Physicals[i].Lun);
            }
        }
    }

    InterlockedExchange(&dev->MgmtBusy, 0);
    return changed;
}
