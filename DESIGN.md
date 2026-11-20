# srcraid — a Server 2025 Storport miniport for Adaptec Series 7 (SRC interface)

Clean-room Storport miniport for the Adaptec/PMC-Sierra **Series 7** RAID controllers
(ASR-71605 et al., PCI `VEN_9005 DEV_028C`, "Denali", **SRC** register interface),
targeting **Windows Server 2022/2025 and Windows 11** with correct D3→D0 (sleep/hibernate)
resume — the one thing the frozen 2017 vendor driver (`arcsas.sys`) gets wrong on modern
StorPort.

## Why we're building this (root cause, proven)
The vendor `arcsas.sys` resume recovery depends on StorPort's pre-2019 power-callback
sequence (it un-pauses I/O only in `ScsiSetRunningConfig` and re-inits only in a
`ScsiRestartAdapter` path guarded by flags that `ScsiSetRunningConfig`/`SetBootConfig`
were expected to clear first). Server 2022/2025 reworked StorPort power management and no
longer delivers that sequence on resume, so after an S3/S4 the adapter is left **paused
at the StorPort level** (`StorPortPause` from the sync-command path never matched by
`StorPortResume`) with a **desynced/power-cycled firmware comm interface** — I/O never
reaches `StartIo`, threads deadlock. Live-debug confirmed every step. Minimal binary patches
to the vendor driver proved the diagnosis but can't cleanly recover a power-cycled firmware;
that needs a full reset+reinit on resume, which belongs in a driver we control.

## References (hardware interface is exact, not guessed)
The SRC hardware interface and the comm/FIB protocol were established from two references,
cross-checked against the controller on real hardware:
1. **Linux `aacraid` driver** (open source, GPL — used as an *interface specification*, not
   copied): `drivers/scsi/aacraid/src.c` (SRC register interface, ISR, sync command,
   `aac_src_restart_adapter`, `aac_src_check_health`, soft reset), `comminit.c` (AAC_INIT,
   comm queues), `commsup.c` (FIB send/complete), `aachba.c` (SCSI translation), `linit.c`
   (suspend/resume/reset) — this is where the *correct* reset/resume sequence lives.
2. **The vendor `arcsas.sys` driver's behavior**, used only as a *behavioral reference* (not
   copied) to confirm the exact FIB formats and register sequences this firmware requires.

## Target
- OS: Windows Server 2025 (build 26100) x64; also 2022 (20348) / Win11. WDK 10.0.26100.
- Device: `PCI\VEN_9005&DEV_028C&SUBSYS_*` (Series 7). Extendable to 028B/028D (Series 6/8).
- Model: Storport **HBA** miniport (not virtual), MSI-X, 64-bit DMA.

## Phased plan (each phase builds, signs, and is testable on hardware)
- **P0 Skeleton:** project + build.ps1 + signing; `DriverEntry` →
  `StorPortInitialize`; stub callbacks; compiles+links a valid `.sys`.
- **P1 Enumerate + bring-up:** `HwFindAdapter` (PCI config, map BAR0 SRC registers,
  access ranges, MSI-X, uncached extension sizing), `HwInitialize` (adapter soft reset,
  wait-for-ready, allocate + push `AAC_INIT` comm area, start comm). Driver attaches, no I/O.
- **P2 Sync command path:** the synchronous mailbox/doorbell command path, read
  adapter info + container list.
- **P3 I/O:** `HwBuildIo`/`HwStartIo` → build FIBs (read/write/SCSI passthrough), submit
  via the SRC inbound queue; `HwInterrupt` (MSI-X) → drain outbound response queue →
  complete SRBs. Read/write to a container works.
- **P4 Reset + power:** `HwResetBus` (IOP reset), and `HwAdapterControl`:
  `ScsiStopAdapter` = quiesce; **`ScsiRestartAdapter` = full `aac_src_restart_adapter`
  style reset + comm re-init + StorPort resume** (the whole point). Hibernate/resume clean.
- **P5 Hardening:** error recovery, AIFs, hot-plug, multi-controller (5 cards), WHQL/HLK
  prep, MS attestation submission.

## Build & sign
`build.ps1` compiles with `cl /kernel` against WDK 10.0.26100 km headers and links with
`storport.lib ntoskrnl.lib hal.lib BufferOverflowFastFailK.lib` (`/DRIVER /INTEGRITYCHECK
/ENTRY:GsDriverEntry`). Test cycle: self-signed test cert + `bcdedit /set testsigning on`
on the lab server (Secure Boot already off for KD). Production: Microsoft attestation signing
(via Partner Center) → loads under Secure Boot/CKS.

## Status
Feature-complete and hardware-validated: block I/O, raw pass-through, sleep/hibernate resume,
multi-vector MSI-X, full maxView/arcconf management, write-back cache flush, and a clean
Driver Verifier pass. See the README and the commit history for specifics.
