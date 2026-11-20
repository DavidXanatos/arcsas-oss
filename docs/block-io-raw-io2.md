# Block I/O — ContainerRawIo2 (the read/write command this firmware accepts)

**TL;DR:** on the Series 7 (SRC, PCI 9005:028C) this firmware services container block
READ/WRITE **only** via FIB `Command = 503` (`ContainerRawIo2`) carrying an `aac_raw_io2`
payload with 16-byte IEEE1212 scatter-gather. The older `VM_CtHostRead64`(19)/
`ContainerCommand64`(501) and `VM_CtBlockRead(64)` paths are **rejected with
`ST_INVAL` (22 / 0x16)**. This was proven by a live kernel-debugger capture of the vendor
`arcsas` driver doing a real `READ(10)`, and validated on hardware in `srcraid` build
0.1.0.48 (reads + writes, INTx mode, CrystalDiskMark).

See also [`fib-delivery-channels.md`](fib-delivery-channels.md) (how FIBs are delivered) and
[`SRC-hardware-interface.md`](SRC-hardware-interface.md) (registers / comm model).

---

## 1. The wire format (verified byte-for-byte)

FIB = 0x20-byte header (`AAC_FIBHDR`) immediately followed by the `aac_raw_io2` payload.

### Header (from the captured vendor read)
| field | offset | value | note |
|---|---|---|---|
| XferState | +0x00 | `0x000830AD` | `XS_SEND_SYNC \| XS_Async` (see below) |
| Command | +0x04 | **`0x01F7` = 503** | `ContainerRawIo2` |
| StructType | +0x06 | `0x04` | `FIB_MAGIC2` (TYPE2 new-comm) |
| Size | +0x08 | `0x50` | total FIB: 0x20 hdr + 0x20 raw_io2 fixed + 1×0x10 SGE |
| SenderSize | +0x0a | `0x0800` | 2048 = negotiated max FIB size |
| SenderFibAddress | +0x0c | fib bus addr | for RRQ completion routing |

`XferState 0x000830AD` = `HostOwned | FibInitialized | FibEmpty | SentFromHost |
ResponseExpected | NormalPriority | Async | FastResponseCapable`. This is exactly our
existing `XS_SEND_SYNC | XS_Async` — the XferState was **never** the problem.

### Payload `aac_raw_io2` (fixed part = 0x20 bytes, then SGE array)
| field | offset | type | note |
|---|---|---|---|
| blockLow | +0x00 | u32 | LBA low |
| blockHigh | +0x04 | u32 | LBA high — **true 64-bit LBA** (no 2 TB limit) |
| byteCount | +0x08 | u32 | transfer length in **BYTES** (not sectors) |
| cid | +0x0c | u16 | container id |
| flags | +0x0e | u16 | RIO2 flags (below) |
| sgeFirstSize | +0x10 | u32 | length of the first SGE element |
| sgeNvme / resv | +0x14 | u32 | 0 |
| sgeCnt | +0x18 | u32 | number of SGE entries |
| resv | +0x1c | u32 | 0 |
| sge[] | +0x20 | — | 16-byte IEEE1212 entries |

### SGE — IEEE1212, **16 bytes** (not the 12-byte `sgentry64`)
| field | offset | note |
|---|---|---|
| addrLow | +0x00 | buffer phys low |
| addrHigh | +0x04 | buffer phys high |
| length | +0x08 | element length in bytes |
| flags | +0x0c | 0 |

### RIO2 flags
```
RIO2_IO_TYPE_WRITE       0x0000
RIO2_IO_TYPE_READ        0x0001
RIO2_IO_TYPE_VERIFY      0x0002
RIO2_IO_ERROR            0x0004
RIO2_IO_SUREWRITE        0x0008
RIO2_SGL_CONFORMANT      0x0010   // every SGE but the last == sgeFirstSize
RIO2_SG_FORMAT_SRL       0x1000
RIO2_SG_FORMAT_IEEE1212  0x2000
```
Observed read flags = **`0x2011`** = `READ | SGL_CONFORMANT | SG_FORMAT_IEEE1212`;
writes use `0x2010` (`WRITE | SGL_CONFORMANT | IEEE1212`). We set `SGL_CONFORMANT` only when
all SGEs except the last share the first SGE's length (then `sgeFirstSize` describes them);
otherwise leave it clear.

Implemented in [`../src/src_hw.h`](../src/src_hw.h) (`AAC_RAW_IO2`, `AAC_SGE_IEEE1212`,
`FIBCMD_ContainerRawIo2`, `RIO2_*`) and [`../src/src_io.c`](../src/src_io.c) (`SrcIoReadWrite`).

### Completion
Delivered on the inbound queue, completed via the host RRQ exactly like our other FIBs: a
**fast response** (RRQ entry bit 30, `0x40000000`) is the success signal. On a fast response
the FIB `reply[0]` merely echoes the LBA — it is **not** a status; do not treat non-zero
`reply[0]` as an error when the fast bit is set.

---

## 2. Why we had it wrong first (ContainerCommand64 → ST_INVAL)

The command is runtime-selected, and we picked the selector from the wrong place.

- We modelled the read FIB on a vendor-binary function that *looks* like the read builder and
  emits `ContainerCommand64`/`VM_CtHostRead64`. A live trace proved that path is **not on the hot
  read path for this controller** — real reads are built inline on the I/O path and emit
  `ContainerRawIo2`. Static analysis handed us a plausibly-named but dormant code path.
- Linux `aacraid` already shows the path is a capability switch: `aac_adapter_read/write`
  uses the **raw I/O** family when `dev->raw_io_interface` is set, else the old
  `ContainerCommand(64)`. Our own init even negotiated it — `COMM_PREFERRED` returned
  `raw64=1` — but we wired the FIB to the vendor-binary path instead of to that flag.
- Linux would still not have given the exact Windows values (its raw_io2 uses a different
  opcode and a slightly different struct); only the capture pinned Command=503, the field
  offsets, and the 16-byte SGE.

**Lesson:** when a negotiated capability (`raw64=1`) and a reversed code path disagree,
trust the capability and confirm with a live trace — not the best-looking statically-identified function.

---

## 3. How the format was confirmed

The exact `ContainerRawIo2` wire format — Command=503, the field offsets, and the 16-byte SGE —
was confirmed by capturing real reads on live hardware (an uncached read forced to reach the
controller) and cross-checking against the Linux `aacraid` raw-I/O path. Static analysis alone
had pointed at a plausible-but-dormant builder (§2), so the rule is: trust the negotiated
capability (`raw64=1`) and confirm with a live trace.

---

## 4. Status

P3 (block I/O) **validated on hardware** (build 0.1.0.48, INTx): self-test read succeeds,
Windows reads the NTFS partition, CrystalDiskMark passes read+write incl. QD32 concurrency
and multi-SGE 1 MB transfers. Next: strip bring-up debug cruft, then P4 (reset/resume).
