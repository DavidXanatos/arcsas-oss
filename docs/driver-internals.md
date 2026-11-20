# srcraid driver internals — as-built behaviors

How the `srcraid` driver actually behaves at runtime, beyond the raw hardware/FIB protocol
(those are in `SRC-hardware-interface.md`, `comm-init-protocol.md`,
`fib-delivery-channels.md`, `block-io-raw-io2.md`). This captures the enumeration,
event, resume, multi-LUN, SMART/cache, and diagnostic behaviors that were reverse-engineered
and hardware-verified while bringing the driver to full maxView manageability. The
management IOCTL surface a GUI uses is in `management-api.md`.

Source of truth is the code; this is the map. File references are to `src/`.

---

## 1. Device enumeration model

Two buses (`srcraid.c` HwFindAdapter: `NumberOfBuses=8`, `MaximumNumberOfTargets=255`,
`MaximumNumberOfLogicalUnits=8`):

- **Bus 0 — containers** (`SRC_CONTAINER_BUS`). Windows target = firmware container number
  (cid). `dev->Containers[cid]` holds `Valid`, `NotReady`, `Hidden`, `SizeBlocks`, `Type`,
  `BlockSize`, and `Serial` (= `cid<<24 | oid`, used for VPD 0x80/0x83 unique identity).
- **Bus 1 — physical pass-through** (`SRC_PHYSICAL_BUS`). Windows target = index into
  `dev->Physicals[]`; the firmware `(channel,id,lun)` lives in the entry.

### Containers (`src_fib.c`)
Enumerated at bring-up by probing each cid with `VM_NameServe64` (fall back to
`VM_NameServe` if the 64-bit variant is rejected) — `SrcProbeContainer`. The chosen variant
is remembered (`dev->NameServeCmd`) for the runtime re-probe. Each probe yields size, type,
object id (`oid`), serial, and a not-ready flag.

### Physical disks (`src_phys.c`)
Enumerated by an **INQUIRY sweep** over `(channel 0..3, id 0..255, lun 0)` via the sync
mailbox (`SrcEnumPhysical`). A device answers accessible (peripheral qualifier 0) only if the
firmware exposes it (not an array member). The sweep aborts safely after
`SRC_SWEEP_MBOX_ABORT` consecutive wedged-mailbox probes so a half-stuck firmware can't hang
boot. The span was widened to id 0..255 across channels specifically to catch devices behind
**cascaded expanders** and SES on non-zero channels (verified on HW with 2 cascaded expanders
+ SES on channel 2, id 36).

### Runtime re-enumeration (hot add/remove, no reboot)
On an adapter event (§3) the AIF timer runs a short burst of re-scans:
- `SrcRescanContainers` — re-probes cids; append-or-refresh present ones; a container that
  comes back "not present" is only retired after **two consecutive misses** (a clean
  `CT_NONE` empty slot is authoritative and retires immediately). This protects a healthy
  volume from a one-off probe glitch while config is committing, yet still drops a genuinely
  deleted volume. `dev->Containers[cid].Hidden` is preserved across a refresh.
- `SrcRescanPhysical` — append-only re-sweep; a new device gets the next free target index, a
  vanished one is marked invalid. Append-only indexing keeps target ids stable (StorPort
  requires target id < 255).

If either set changed, the driver raises `StorPortNotification(BusChangeDetected, …)` for that
bus and Windows re-probes — so a disk going raw→initialized, or a volume created/deleted in
maxView, appears/disappears live.

---

## 2. Multi-LUN pass-through (vendor model)

The driver follows the vendor's transparent model: it does **not** probe LUNs itself
(`SRC_SWEEP_LUNS=1`, LUN 0 only, because a blind mailbox LUN sweep gets fooled by old disks
that echo their LUN-0 INQUIRY for every LUN → phantom duplicates). Instead:

- `MaximumNumberOfLogicalUnits=8` lets **Storport** drive the LUN scan on each physical target.
- `SrcPhysExecute` (`src_io.c`) **relays the OS-requested `Srb->Lun`** to the firmware at the
  device's `(channel,id)` — exactly as the vendor passes `pSrb->Lun` through. A real
  multi-LUN device answers its extra LUNs; a single-LUN device
  selection-times-out LUN>0 at the firmware, so no phantom is created. This path goes through
  the device's own `REPORT LUNS`, which the blind mailbox sweep could not.
- The maxView raw-SRB passthrough (`0x42264`, `src_mgmt.c`) likewise relays the requested LUN
  (`p[0x07]`) rather than hardcoding 0.

Verified phantom-free on the current single-LUN topology (Storport probed LUN 1–7 on every
physical target; no duplicate disks appeared). A genuine multi-LUN device (e.g. an enclosure
exposing SES as a separate LUN) would now enumerate; that positive case can't be proven on a
LUN-0-only controller but the mechanism is harmless and matches the vendor.

---

## 3. Adapter-initiated events (AIF channel) — `src_aif.c`

The firmware reports events by **completing a held request**, not by pushing:

- The driver keeps **one** `AifRequest` FIB (Command 700, `data[0]=200` "want events")
  outstanding in a reserved slot (`SRC_AIF_SLOT`), delivered on the inbound queue with
  `XS_FastResponseCapable` **cleared** (so the firmware holds it instead of fast-completing).
- On this firmware the request is actually completed **almost immediately** — empty
  (`aifCmd=0`) when there is no event, or carrying the event FIB when there is one (observed:
  tight post→complete pairs ~every 3 s, paced by the timer). A real event sets
  `RescanPending`, and a verbatim copy of the FIB is queued for delivery.
- Delivery to user mode is `0x4200F` (`management-api.md` §8): `SrcAifDeliver` hands the next
  queued FIB back and sets `ControlCode = 2`; no event → `ControlCode = 0`.

**Why it does not re-post inline:** the firmware's instant empty-completion means re-posting on
every completion is a tight busy loop (it once produced a 26k-completion wedge). A ~3 s
StorPort timer re-arms instead (`SrcAifTimer`), which structurally caps the rate.

**Delivery ring (single-producer):** the ISR producer advances only `AifQTail`; the consumer
(`SrcAifDeliver`) handles overflow with a lock-free `InterlockedCompareExchange` claim and a
lap-skip, so there is no producer/consumer race on the head cursor and concurrent pollers are
serialized. The ring is intentionally lossy (depth `SRC_AIF_QDEPTH=8`) — events are a refresh
hint, and the re-scan burst catches the settled config regardless.

**Liveness watchdog:** if a request is ever lost (missed completion, or a firmware re-init that
dropped it without a driver bring-up), `AifReqPending` would stick and the channel would die
silently. `SrcAifTimer` counts ticks with a request outstanding and, after ~60 s of silence,
reclaims the slot and force-posts a fresh request. Because the firmware normally completes
within a tick, 60 s of silence means the request is genuinely gone — so this never re-posts
over a request the firmware still holds (the vendor driver guards against the same hazard with
an equivalent outstanding-request check).

**Rescan throttle:** pure-progress events do **not** trigger a re-scan — class-2 JobProgress
(rebuild/morph %) and class-1 sub-0x2A (temperature/supercap) only. Everything else
(container/JBOD/HBA add/remove, and any unknown code — safe default) does. Without this, a
RAID rebuild's continuous progress AIFs would hold the mailbox in back-to-back sweeps and
starve I/O.

---

## 4. Resume / hibernate robustness — `src_aif.c` is unrelated; see `srcraid.c`, `src_init.c`

`ScsiRestartAdapter` (and the cold-boot `HwInitialize`) run the adapter-control callback at
**IRQL 8** (a device IRQL, above DISPATCH — StorPort synchronizes it with the HW interrupt).
A blocking firmware-ready wait there would hard-hang with no watchdog. So re-init is
**deferred**:

- `SrcAdapterRestart` does only quick, non-blocking cleanup, sets `RestartPending`, and arms a
  StorPort timer (`SrcRestartTimer`, DISPATCH) that polls `FW_UP_AND_RUNNING` one register
  read per 100 ms tick, runs `SrcBringUp` **once** when ready, and kicks a wedged firmware with
  a **non-blocking** `SrcIopResetTrigger` after a timeout.
- While deferred, `dev->Ready = FALSE`, so block I/O BUSY-retries and all management paths
  bail — nothing touches the unlocked mailbox during bring-up.
- Cold boot uses the same split: `HwInitialize` does inline `SrcBringUp` if the firmware is
  already up, else defers to the same timer.

`dev->Ready` is set TRUE **only** on full bring-up success, so a dead controller never looks
healthy. Verified across repeated S4 hibernate/resume on hardware (RTC-wake autonomous loop):
volumes return, D: write/read/verify OK, no bugcheck.

---

## 5. SMART, write-back cache, not-ready — `src_scsi.c`, `src_io.c`

- **Unique identity:** VPD pages 0x80/0x83 emit a per-container serial (`Serial`), so Windows
  sees multiple volumes as distinct disks (they previously collided into one).
- **MODE SENSE:** both MODE SENSE(6) and MODE SENSE(10) answer the caching mode page (0x08,
  WCE=1) so Windows knows write-back is on.
- **SYNCHRONIZE_CACHE** is a real flush: `SrcIoFlush` builds an async
  `VM_ContainerConfig/CT_FLUSH_CACHE(129)` FIB for the cid; verified to actually flush with
  write-back enabled (dirty flush ~1.5 s, clean ~170 ms).
- **Not-ready containers:** a firmware `FSCS_NOT_READY` volume (degraded/initializing/foreign)
  answers TEST UNIT READY and all data access with a clean NOT READY sense, but still answers
  INQUIRY / READ CAPACITY / REPORT LUNS / MODE SENSE — so it enumerates (visible but
  inaccessible) instead of vanishing or storming I/O errors.
- **Block I/O** is `ContainerRawIo2` (FIB Command 503); see `block-io-raw-io2.md`.

---

## 6. CISS physical enumeration (conditional, native-HBA only)

The vendor's `REPORT PHYSICAL LUNS` (CISS, CDB `0xC3 0x02 …`) enumeration is **native-HBA-mode
only** — gated on adapter option `0x2000000` + a management device address the firmware only
populates in HBA mode. In the controller's "RAID (Expose RAW)" mode the vendor's CISS path
bails, so the INQUIRY sweep (§1) is the correct enumeration.

`srcraid` implements a **conditional** CISS path (`src_phys.c`): `SrcCissDiscover` probes for a
CISS management device at init; if found (`CissAvail`), `SrcEnumPhysical`/`SrcRescanPhysical`
build the device list from `REPORT PHYSICAL LUNS` instead of sweeping, else they fall back to
the sweep. On this controller `CissAvail = 0` (confirmed via the trace header), so the path is
dormant and the sweep runs — the CISS parse is present but **untested** (no HBA-mode controller
available) and would be validated if the controller is ever switched to HBA mode.

---

## 7. Management IOCTL behaviors worth knowing — `src_mgmt.c`

- All AACAPI codes maxView/arcconf send are implemented (see `management-api.md` §3). Unknown
  codes return `ReturnCode = 6` (unsupported) with a **zeroed** payload — the zeroing is a
  safety requirement (the multi-threaded maxView agent must never parse stale request bytes as
  a result; that caused a JVM heap-corruption / Tomcat crash).
- `0x42260` FIB passthrough is **async** — submitted on the inbound queue, completed by the
  ISR (kind `SRC_FIBK_MGMT`), so it doesn't busy-wait and each passthrough rides its own slot.
  It requires `dev->Ready`.
- `0x42264` raw-SRB uses the **sync mailbox** under `MgmtBusy` serialization, requires
  `dev->Ready`, and places response data at `(len + byte[0x0B] + 7) & ~3` (4-byte aligned —
  an 8-byte alignment garbles SMART tables).
- `MgmtBusy` serializes the sync mailbox / slot-0 between the rescan sweeps and the `0x42264`
  path so they can't interleave on the single sync resource.

---

## 8. Diagnostics — the trace ring

A lock-free 1024-entry circular trace (`dev->Trace`, `SrcTrace`) records mgmt FIBs, raw-SRB
passthroughs, AIF post/complete, physical probes, CISS discovery, and container-UID lookups —
each as `{kind, a, b, c, d0..d3}`. It is read out via the `SRCDBG` IOCTL (`management-api.md`
§9) with `tools/srctrace.c` (which also prints the CISS-availability header). It survives
across a hibernate/resume in the device extension (unlike live `DbgPrint`), which is how the
IRQL-8 resume behavior was measured.

`kind`: 1/2 mgmt in/out · 3 raw-SRB · 5 AIF (a=0 post / a=1 complete, b=aifCmd) · 6/7 physical
probe in/out · 10/11 CISS discover found/responder · 12 container-UID (42023/42047) lookup.

---

## 9. Testing — `tools/regress.ps1`

The post-deploy regression gate, run on the target after every driver update:

```
powershell -ExecutionPolicy Bypass -File C:\Temp\regress.ps1 [-IoSizeMB N] [-Full]
```

Non-destructive by default. Checks: driver load + version, target-volume health, Windows
enumeration with baseline-drift detection, firmware enumeration + the management round-trip
(`arcconf GETCONFIG AD` → "Controller Status: Optimal", which exercises the `0x42260`
mgmt-FIB passthrough), 32/64 MB block I/O integrity (write/flush/read/SHA-256 verify),
write-back flush timing, physical-disk health, the maxView agent, and the AIF channel liveness
(post+complete pairs via srctrace). Exit code = number of critical failures. `-Full` adds a
create+delete cycle (destructive of free array space).

---

*All of the above reflects observed firmware behavior; offsets marked HW-verified were
confirmed on a live ASR-71605.*
