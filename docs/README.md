# srcraid/docs — hardware & driver reference

The technical reference behind the `srcraid` driver — the SRC hardware interface, the comm/FIB
protocol, block I/O, the management API, and runtime behavior — so you can understand and extend
the driver.

| File | What |
|---|---|
| [`SRC-hardware-interface.md`](SRC-hardware-interface.md) | **The hardware spec** — PCI IDs, BARs, SRC register map (HW-verified offsets), FW status word, doorbell/sync protocol, comm model, device limits, interrupts. Start here. |
| [`comm-init-protocol.md`](comm-init-protocol.md) | The comm-area layout and the `AAC_INIT` init-struct handshake that brings the firmware's new-comm interface up. |
| [`fib-delivery-channels.md`](fib-delivery-channels.md) | The two FIB delivery channels (inbound-queue/RRQ vs synchronous mailbox) and which command needs which. |
| [`block-io-raw-io2.md`](block-io-raw-io2.md) | **Block READ/WRITE** — the `ContainerRawIo2` (503) wire format this firmware requires, and why `ContainerCommand64` gave `ST_INVAL`. |
| [`management-api.md`](management-api.md) | **The management API for a GUI** — the as-built IOCTL surface (SRB_IO_CONTROL transport, status codes, device model, every AACAPI control code with request/response layouts, FIB passthrough, raw-SRB/SMART, AIF events, identity, diagnostics). Read this to write your own maxView replacement. |
| [`driver-internals.md`](driver-internals.md) | **How the driver behaves at runtime** — enumeration + hot re-scan, multi-LUN relay, the AIF event channel (ring + watchdog), resume/restart deferral, SMART/write-back/not-ready, conditional CISS, and the `regress.ps1` test harness. |
| [`../DESIGN.md`](../DESIGN.md) | The `srcraid` driver design + phased plan. |

## Status
Functionally complete and hardware-validated on the ASR-71605:
- **Block I/O** (`ContainerRawIo2`, 503) — reads/writes, QD32, multi-SGE; CrystalDiskMark clean.
- **Resume/hibernate** — repeated S4 cycles, no bugcheck (deferred off-IRQL-8 re-init; see
  [`driver-internals.md`](driver-internals.md) §4).
- **Full maxView/arcconf manageability** — every AACAPI IOCTL the tools use is implemented
  (enumeration, pass-through/SMART, container identity, present/unpresent, FIB passthrough);
  create/delete/hot-plug re-enumerate live with no reboot. API in
  [`management-api.md`](management-api.md).
- **Write-back cache flush**, MODE SENSE caching page, NOT-READY handling, unique per-volume
  VPD identity, multi-LUN pass-through, conditional CISS (dormant in RAID mode).
- **Driver Verifier** clean — standard flags plus the full applicable flag set.

What's open: broad maxView-operation coverage (RAID migration, rebuild, hot-spare,
initialize/verify, cache policy, LED locate, firmware update) still wants a systematic sweep;
CISS enumeration is untested (needs an HBA-mode controller).
