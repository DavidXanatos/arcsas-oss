# srcraid

A clean-room **Windows StorPort miniport driver** for Adaptec / PMC-Sierra **Series 7** RAID
controllers (ASR-71605 and siblings, PCI `VEN_9005 DEV_028C`, "Denali", **SRC** register
interface), targeting **Windows Server 2022 / 2025 and Windows 11** — with correct
sleep/hibernate (S3/S4 → D0) resume, which the frozen 2017 vendor driver (`arcsas.sys`) gets
wrong on modern StorPort.

> Status: **working and hardware-validated** — boots, does block I/O, resumes from
> hibernate, exposes RAID volumes and raw pass-through disks, is fully manageable by Adaptec
> **maxView / arcconf**, and runs **multi-vector MSI-X**. Current build: see `buildnum.txt`.

---

## Why this exists

The vendor `arcsas.sys` recovers from resume using StorPort's pre-2019 power-callback
sequence (it un-pauses I/O only in `ScsiSetRunningConfig` and re-inits only in a
`ScsiRestartAdapter` path gated by flags the older sequence cleared first). Server 2022/2025
reworked StorPort power management and no longer delivers that sequence, so after an S3/S4 the
adapter is left **paused at the StorPort level with a power-cycled firmware comm interface** —
I/O never reaches `StartIo` and threads deadlock. Root cause confirmed by live kernel debugging.

srcraid fixes this by doing an **unconditional full reset + comm re-init on resume**, deferred
off the elevated power IRQL onto a readiness-polling timer so nothing busy-waits.

## What works (hardware-verified)

- **Block I/O** to RAID containers (`ContainerRawIo2`) and **raw pass-through** disks (`aac_srb`),
  64-bit LBA, scatter-gather, write-back cache flush on `SYNCHRONIZE_CACHE`.
- **Sleep/hibernate resume** — the reason the project exists.
- **Multi-vector MSI-X** (`no_of_msix_vectors = N`, per-vector RRQ regions, `HwMSInterruptRoutine`)
  with StorPort `DPC_REDIRECTION`. *Note:* true per-vector completion queues require the V8 init
  struct, which this card's firmware does not advertise (option bit `0x40000000` clear); srcraid
  therefore drains all regions per interrupt — which is exactly what the vendor also does on
  non-V8 hardware. See the MSI-X commit history.
- **maxView / arcconf management** — full AACAPI IOCTL surface: enumeration, pass-through,
  SMART, async raw-SRB, config create/delete with live (reboot-free) hot re-enumeration.
- **Transparent-relay physical enumeration** (StorPort probes, firmware answers; no driver-side
  INQUIRY sweep).
- **AIF event channel** (adapter events → hot re-scan) with a liveness watchdog.
- **Windows event-log integration** — controller faults (bus reset, FW kernel panic / not-up,
  adapter-dead fail-fast) are recorded via `StorPortLogError`, so they survive a reboot and show
  up in Event Viewer without the maxView agent running.
- **FW time-of-day sync** — host wall-clock is re-pushed every ~15 min so the controller's
  log / SMART / enclosure timestamps stay aligned with the host over long uptime.
- **DeadAdapter fail-fast** safety net: a broken completion path boots the box *degraded and
  recoverable* instead of hanging PnP.

## Hardware

| Series | Example | PCI ID | Support |
|---|---|---|---|
| **Series 7** | ASR-71605 ("Denali") | `PCI\VEN_9005&DEV_028C` | **Primary target — fully hardware-validated** |
| Series 6 | | `PCI\VEN_9005&DEV_028B` | INF binds; **untested** |
| Series 8 | ASR-81605Z | `PCI\VEN_9005&DEV_028D` | INF binds; **untested / limited** — see below |

Primary target and validation platform is the **ASR-71605** on Windows Server 2025.

### Series 8 (028D) / Series 6 — untested, limited support

srcraid's INF binds to Series 6 and 8, and it has **attached** to a Series 8 (ASR-81605Z) in the
lab (enumerated cleanly; no array was configured on that card to exercise I/O). They share the SRC
register interface with Series 7, so in plain **RAID mode** a Series 8 is *expected* to work on the
standard 32-bit comm path — comm area **below the 4 GB line**, exactly as on Series 7 — but this is
**not validated on real Series 8 hardware**, so treat it as best-effort.

Two Series-8-specific firmware capabilities are deliberately **not implemented**:

- **SmartHBA / HBA mode** — raw-disk pass-through via the "Thor" HBA command path (gated in the
  vendor on `DeviceId==653 && SmartHBA`). srcraid supports RAID **containers** only, not HBA mode.
- **V8 init struct + 64-bit comm/FIBs** — gated on firmware option bit `0x40000000`, which the
  Series 7 does **not** advertise. srcraid uses the rev-7 **32-bit init**, which keeps the comm
  control area below 4 GB. This is functionally complete (the 64-bit path only changes *where* the
  small comm area sits in physical memory, not behavior or speed) and relies on the Series 8
  firmware still accepting the rev-7 init struct — normally backward-compatible, but unverified.

**Bottom line:** a RAID-mode Series 8 will most likely run unchanged; a SmartHBA-mode card, or one
whose firmware only honors the V8 init struct, would need additional work. The 64-bit comm path is
a Smart-Array / SmartHBA firmware feature — those controllers (the PQI "Smart Family") use
Microsoft's separate `smartpqi` driver, not this one.

## Build

Requires the Windows WDK (StorPort) + a test-signing cert. From this directory:

```powershell
.\package.ps1        # compiles, signs, and stages .\package\  (bumps buildnum.txt)
```

The `Number of errors: 1` at the end is the expected cert-verify step failing until the test
cert is trusted on the target; `PACKAGE READY` means the build succeeded.

## Install (lab target, test-signed)

```
install.bat          # trust the test cert + devcon install the staged package
rollback.bat         # revert to the previous driver
```

The INF uses `StartType = DEMAND (3)` for safe lab testing on a **non-boot** controller; a
production boot controller needs `StartType = 0`.

### Post-install note — disk stats in Task Manager (Server only)
Windows **Server** ships `IOCTL_DISK_PERFORMANCE` counters **off**, so Task Manager /
TaskExplorer show no per-disk activity for non-boot disks (any controller, not srcraid-specific).
Enable them once with:

```
diskperf -y
```

## Layout

```
srcraid.inf           INF (PCI IDs, MSI-X, service install)
src/
  srcraid.c           HwFindAdapter/Initialize/StartIo/Interrupt/AdapterControl/ResetBus,
                      power + deferred resume (SrcAdapterRestart / SrcRestartTimer), fail-fast
  src_comm.c          comm-area alloc + AAC_INIT build/push (vector split, raw-SRB bounce pool)
  src_io.c            async I/O slots, RRQ completion ISR (drain-all), MSI-X ISR, raw-SRB, SG
  src_scsi.c          SCSI command translation (INQUIRY/READ CAPACITY/MODE SENSE/REPORT LUNS/RW)
  src_phys.c          physical pass-through enumeration (transparent relay)
  src_fib.c           synchronous FIB (init/enumeration)
  src_sync.c          sync mailbox + IOP reset
  src_mgmt.c          maxView/arcconf IOCTLs (AACAPI, raw-SRB, mgmt-FIB, trace)
  src_aif.c           AIF event channel + periodic timer + liveness watchdog
tools/                srctrace.c (in-memory trace reader), regress.ps1 (post-deploy harness)
docs/                 hardware interface, comm/FIB protocol, block I/O, management API, internals
DESIGN.md             design + phased plan
```

## Documentation

Start with **[`docs/README.md`](docs/README.md)** — it indexes the SRC hardware interface,
the comm/FIB protocol, block I/O, the **management API** (everything needed to write your own
maxView replacement), and the runtime internals. **`DESIGN.md`** has the design rationale and
phased plan. The source under `src/` is heavily commented throughout.

## Validation

Validated on the **ASR-71605** (Windows Server 2025), build `0.1.0.214`–`217` (identical source,
rebuilt per test):

- **Driver Verifier** (standard + force-pending-I/O) — clean through bring-up, ~69 GB of
  force-flushed block I/O, and a concurrent management-IOCTL loop; **0 bugchecks**. Verifier
  caught one **real bug**: `SYNCHRONIZE_CACHE` was misreading the `VM_ContainerConfig` CT reply
  (status is `CT_OK` = 218 at `resp[2]`, not a zero at `resp[1]` — the latter is the command echo),
  so every non-fast-response flush reported a device error. **Fixed** (`src_io.c`), both write-back
  and write-through arrays now flush correctly.
- **Fault injection** — failing a RAID5 member under continuous verified I/O → degraded parity
  reconstruction (~110 GB, 0 errors), then hot-spare rebuild under heavy load (~117 GB, 0 errors)
  → array back to Optimal. **~263 GB through fail → degraded → rebuild → recovered, 0 corruption.**
- **4-hour soak** — ~10 TB of continuous unbuffered (`FILE_FLAG_NO_BUFFERING | WRITE_THROUGH`)
  tagged write/read/verify under Verifier: **0 integrity errors, 0 bugchecks**, controller Optimal.
- **Static analysis** — **CodeQL** (full C/C++ security-and-quality suite, 183 queries) and MSVC
  `/analyze`: **no actionable defects** (one benign always-false bounds guard on a `UCHAR`). SDV is
  deprecated/removed from the current WDK; CodeQL is its successor and what driver cert now uses.

## Status / known items

Feature-complete and audited against the vendor driver's observed behavior; no missing subsystems. Deliberately **not** implemented (not needed by maxView/arcconf or not
applicable to this non-V8 card): generic WMI provider, full CSMI command set, V8 per-vector
completion queues.

## License / provenance

Clean-room implementation. The Linux `aacraid` driver (GPL) was used only as an *interface
specification*, and the vendor `arcsas.sys` only as a reverse-engineered *behavioral reference* —
neither was copied. See `DESIGN.md` for the design and references.
