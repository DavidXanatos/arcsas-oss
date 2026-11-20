# srcraid management API — how to drive the controller from user mode (GUI reference)

This is the **as-built** management surface the `srcraid` driver exposes to user-mode
software — everything a custom GUI (a replacement for maxView Storage Manager) needs to
enumerate, monitor, and configure the controller. It documents what the driver *actually*
implements (see `src/src_mgmt.c`), not just what the vendor did.

maxView and `arcconf` drive the controller entirely through this surface, so a GUI that
speaks it has exactly the same capabilities. Nothing here needs a kernel component of your
own — it is all `DeviceIoctl` to the storage miniport.

---

## 1. Transport

Everything is an `IOCTL_SCSI_MINIPORT` (`0x0004D008`) to the controller's SCSI port.

```
HANDLE h = CreateFile("\\\\.\\ScsiN:", GENERIC_READ|GENERIC_WRITE,
                      FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
DeviceIoControl(h, IOCTL_SCSI_MINIPORT, buf, len, buf, len, &ret, NULL);
```

`N` is the SCSI port number (0..15). Find the srcraid controller by sending the ping
(`0x42003`, below) or the diagnostic read (`SRCDBG`) to each `\\.\ScsiN:` and keeping the
one that answers.

Every request begins with an **`SRB_IO_CONTROL`** header; the command payload follows it:

| Offset | Size | Field          | Notes                                              |
|--------|------|----------------|----------------------------------------------------|
| 0x00   | 4    | HeaderLength   | 28 (0x1C) — also the byte offset to the payload     |
| 0x04   | 8    | Signature[8]   | `"AACAPI"`, `"CSMISAS"`, or `"SRCDBG"` (see below)  |
| 0x0C   | 4    | Timeout        | seconds                                            |
| 0x10   | 4    | ControlCode    | the function code (e.g. `0x42260`)                 |
| 0x14   | 4    | ReturnCode     | **status, filled by the driver** (see codes below) |
| 0x18   | 4    | Length         | payload length                                     |
| 0x1C   | ...  | payload        | function-specific                                  |

The whole buffer (header + payload) is passed as both input and output to
`DeviceIoControl`; the driver writes results back in place.

**Signatures** select the interface:
- `AACAPI ` — the AAC/Adaptec management API (this document). Primary.
- `CSMISAS` — the CSMI SAS interface (we service only identify/controller-status; generic
  CSMI tools degrade gracefully). Not needed for a GUI.
- `SRCDBG` — srcraid's private diagnostic trace ring (§9).

**Status codes** (`ReturnCode`, mirrors the vendor arcsas):

| Code | Meaning                                                      |
|------|-------------------------------------------------------------|
| 1    | success                                                      |
| 0    | async / pending (the passthrough completes later)           |
| 5    | busy — an IOP reset is in progress, retry                   |
| 6    | unsupported / unhandled control code                        |
| 9    | failure (the operation was attempted and failed)           |

`DeviceIoControl` returning TRUE only means the transport succeeded; always read
`ReturnCode` for the operation result.

---

## 2. Device model (what the GUI will see and address)

The driver presents two logical buses to Windows (`NumberOfBuses = 8`,
`MaximumNumberOfTargets = 255`, `MaximumNumberOfLogicalUnits = 8`):

| Bus | Meaning                     | Target id          | LUN                 |
|-----|-----------------------------|--------------------|---------------------|
| 0   | **Containers** (RAID volumes) | firmware container number (cid) | 0 only |
| 1   | **Physical pass-through disks** | enumeration index | relayed to the device |

- A RAID volume `cid` appears to Windows as the disk at `(bus 0, target cid, lun 0)` and is
  what the user sees as a drive letter.
- A raw/pass-through physical disk appears at `(bus 1, target index, lun)`. The index is the
  driver's discovery order, **not** the firmware channel/id (those live inside the driver).
  Multi-LUN devices present their extra LUNs under the same target (vendor model).

The firmware's own addressing is `(channel, id, lun)`; the driver maps bus 1 targets onto it
internally. A GUI never needs the raw `(channel,id)` for I/O — it uses the Windows
`(bus,target,lun)` — but it *does* see the firmware container number and the container UID
through the identity calls (§5), which is how you correlate a Windows disk with a firmware
object.

---

## 3. The AACAPI control codes (quick map)

All use signature `"AACAPI"`. Grouped by purpose; details in later sections.

**Session / capability handshake** (call at startup, mimic maxView's order):

| Code     | Name / purpose                                                        |
|----------|-----------------------------------------------------------------------|
| 0x42003  | Ping / keepalive → ReturnCode 1                                        |
| 0x4203B  | Open management session                                               |
| 0x4200B  | Register adapter-FIB context (ack)                                     |
| 0x42013  | Register OS management callbacks + "start adapter" (ack)               |
| 0x42033 / 0x42268 | Capability/parameter negotiation (LargeLBA, JBOD, AIF, max FIB size) |
| 0x4202B  | Miniport revision check (driver API rev + build)                      |
| 0x4202F  | Version/signature match strings                                       |
| 0x42043  | HBA info struct (PCI ids, firmware build, caps, serial) — 216 bytes    |

**Enumeration & identity:**

| Code     | Name / purpose                                                        |
|----------|-----------------------------------------------------------------------|
| 0x42023  | Container index → (AdapterContainerNo, ctUid)                         |
| 0x42047  | ctUid → Windows SCSI target (container index)                          |
| 0x42017  | Present / unpresent (show/hide) a logical device by ctUid             |

**The two workhorses:**

| Code     | Name / purpose                                                        |
|----------|-----------------------------------------------------------------------|
| 0x42260  | **FIB passthrough** — send any AAC FIB to the firmware (§6)            |
| 0x42264  | **Raw SCSI/ATA passthrough** — INQUIRY, VPD, SMART (§7)               |

**Events & control:**

| Code     | Name / purpose                                                        |
|----------|-----------------------------------------------------------------------|
| 0x4200F  | GET_NEXT_ADAPTER_FIB — poll for an async event (AIF) (§8)              |
| 0x4203F  | RESET_IOP — acknowledged without a live teardown (§8)                  |

Any other AACAPI code returns `ReturnCode = 6` (unsupported) with a zeroed payload.

---

## 4. Startup handshake

maxView opens a session with this rough sequence; a GUI should do the same so the driver's
capability state matches what the agent expects:

1. `0x42003` ping — confirm the controller answers.
2. `0x4203B` open session.
3. `0x42043` HBA info — read PCI ids, firmware build, serial, supported options.
4. `0x4202B` revision check, `0x42033`/`0x42268` capability negotiation.
5. `0x42043`/`0x4200B`/`0x42013` as needed to register for events.
6. Begin polling `0x4200F` for AIF events (§8) and enumerating (§5/§6).

### 0x42033 / 0x42268 — capability negotiation
Payload is a struct with a selector at `payload[0]` and a status word at `payload[0x100]`,
value words at `+0x104`/`+0x108`. The driver reports: host-interface present, **write-back
(LargeLBA) = on**, JBOD support, **AIF/async-event capability = on**, and the max FIB/transfer
size (`0x800`) / xfer limit (`0x40000`). Getting the AIF bit right is what stops a newer
maxView from busy-polling `0x4200F`.

### 0x42043 — HBA info (216 bytes)
Returns `DriverName` ("arcsas.sys"-compatible), VendorID/DeviceID/SubVendor/SubSystem,
adapter number, bus number, new-comm flag, max command/FIB/SG sizes, firmware & BIOS build
numbers, controller serial number, supported options, feature bits, port count. Use it to
populate the controller summary panel.

---

## 5. Enumerate & identify RAID volumes

There are two complementary ways to list RAID volumes:

**(a) Windows enumeration** — just enumerate disks on bus 0 (e.g. `Get-Disk`, or
`IOCTL_STORAGE_QUERY_PROPERTY`). Each is a container at `(bus 0, cid, 0)`.

**(b) Firmware enumeration via FIB passthrough** (§6) — send `VM_NameServe` /
`VM_NameServe64` to get the authoritative firmware container table (count, capacity,
filesystem type, state). This is what you need for firmware-level detail (degraded,
rebuilding, etc.).

**Correlating the two** is what `0x42023` / `0x42047` are for:

### 0x42023 — container index → identity
Request: `payload[0]` = container index (= the Windows bus-0 target = cid).
If that container is present/exposed, the driver writes:
- `payload[1]` = **AdapterContainerNo** (firmware container number — equals the index here)
- `payload[2]` = **ctUid** (the firmware container's unique object id; stable across reboots,
  also exposed as SCSI VPD page 0x83) — unless `Length == 8` (caller wants only the number).
`ReturnCode` = 1 if present, 9 if not.

### 0x42047 — ctUid → Windows target
Request: `payload[0]` = a container UID (ctUid, as learned from the firmware or from 0x42023).
The driver scans its exposed containers and writes `payload[1]` = the Windows SCSI target
(cid) that carries that UID, or `0xFFFFFFFF` if none. `ReturnCode` = 1.

> Use this to answer "the firmware says volume with UID X exists — which drive letter is it?"
> maxView calls `0x42047` heavily. The UID is the firmware object id (`oid`) from
> `VM_NameServe`.

### 0x42017 — present / unpresent (show / hide) a logical device
Request: `payload[2]` = ctUid, `payload[3]` = new state: `1` = present/show, `2` = ready,
`3` = unpresent/hide, `4` = no-op. The driver sets the container's exposed state (hidden
volumes answer selection-timeout so Windows drops them) and raises a bus re-scan so Windows
follows. `ReturnCode` = 1 if the UID was found, else 9.

---

## 6. FIB passthrough (0x42260) — the primitive for everything firmware-side

This is the most important call. The payload **is an AAC FIB** (header + data); the driver
relays it to the firmware and copies the response FIB back. Through it a GUI issues the full
AAC command set: container enumeration, create/delete/modify, cache/consistency operations,
adapter info, enclosure/SES queries, rebuild control, etc.

Payload layout (an `AAC_FIBHDR` at offset 0, then FIB data at +0x20):

| FIB offset | Field       | Notes                                               |
|------------|-------------|-----------------------------------------------------|
| +0x00      | XferState   | delivery flags (driver fixes these)                 |
| +0x04      | Command     | the FIB command (see catalog below)                 |
| +0x08      | Size        | **data length** (driver adds the 0x20 header)       |
| +0x20      | data[]      | command-specific payload                            |

The driver submits it on the firmware's inbound queue and completes the IOCTL when the
firmware answers (status 0 = pending, then the response FIB is written back into the same
buffer). Delivery/addressing fields are fixed up by the driver; the caller supplies
`Command`, `Size`, and `data`.

### FIB command catalog (the ones a GUI uses)

| Command | Name                     | Purpose                                             |
|---------|--------------------------|-----------------------------------------------------|
| 1       | VM_NameServe             | enumerate containers (32-bit capacity)              |
| 22      | VM_NameServe64           | enumerate containers (64-bit capacity, >2 TB)       |
| 500     | ContainerCommand         | wraps a VM_ContainerConfig request (below)          |
| 501     | ContainerCommand64       | 64-bit variant                                      |
| 703     | RequestAdapterInfo       | full adapter info / firmware revision               |
| 601     | ScsiPortCommand64        | aac_srb SCSI passthrough (also reachable via 0x42264)|

Inside a `ContainerCommand` (500) FIB, `data[0]` = `VM_ContainerConfig` (2) and `data[1]` is
the **CT_\* sub-command**:

| CT_* | Value | Purpose                                                        |
|------|-------|----------------------------------------------------------------|
| CT_GET_CONTAINER_COUNT | 4   | number of containers                                 |
| CT_FLUSH_CACHE         | 129 | flush write-back cache for a cid (`data[2]`=cid)     |
| CT_READ_NAME           | 130 | container label                                      |
| CT_GET_CONFIG_STATUS   | 147 | pending-config status                                |
| CT_COMMIT_CONFIG       | 152 | bring a pending config online                        |

> **Create / delete / modify a volume** are `VM_ContainerConfig` operations (add/delete
> container, set RAID level, assign spare, start rebuild/verify, set cache policy) issued the
> same way. The driver is a faithful relay, so the exact `CT_*`/payload a GUI sends is the
> standard AAC container-config protocol — identical to what maxView/arcconf put on the wire.
> The most reliable way to author these is to capture maxView performing the operation (see
> `docs/tools/`), or follow the AAC `aacraid` headers. The driver imposes no restriction
> beyond relaying and, on completion, re-scanning so Windows picks up the new/removed volume.

See `docs/fib-delivery-channels.md` for the delivery mechanics and
`docs/block-io-raw-io2.md` for the block-I/O FIB (not needed by a GUI — Windows does I/O).

---

## 7. Physical-device detail & SMART (0x42264) — raw SCSI/ATA passthrough

Sends a raw CDB to a physical device and returns its data. maxView uses it for per-disk
`INQUIRY`, VPD pages, and **ATA PASS-THROUGH (12)** (`CDB[0]=0xA1`) for SMART / IDENTIFY.

Request payload layout (packed, after the SRB_IO_CONTROL header):

| Offset | Size | Field                                                           |
|--------|------|-----------------------------------------------------------------|
| 0x00   | 2    | length of this header block                                     |
| 0x05   | 1    | channel (firmware)                                              |
| 0x06   | 1    | target id (firmware)                                            |
| 0x07   | 1    | LUN (relayed to the firmware)                                   |
| 0x0A   | 1    | CDB length                                                      |
| 0x0B   | 1    | (data-offset component)                                         |
| 0x0C   | 4    | flags: 0x40 = data-in, 0x80 = data-out                         |
| 0x10   | 4    | data length (driver writes the **actual** transfer back here)  |
| 0x14   | 4    | timeout (seconds)                                              |
| 0x48   | 16   | CDB                                                            |
| data   | —    | at `(len@0x00 + byte@0x0B + 7) & ~3` (4-byte aligned)          |

`ReturnCode` = 0 on success (note: **0**, not 1, for this call — matches the vendor), 6 if it
could not execute. The transferred byte count is written back at `+0x10`, and the data region
is filled for a data-in command.

> The `(…+7)&~3` data alignment matters: an 8-byte alignment reads SMART attribute tables 4
> bytes off and produces garbled/repeating attribute ids. Use the 4-byte alignment shown.

---

## 8. Change notifications & reset

### 0x4200F — GET_NEXT_ADAPTER_FIB (event poll)
The firmware signals adapter events (drive hot-plug, container add/remove, rebuild progress,
enclosure/temperature) as **Adapter-Initiated FIBs (AIFs)**. The GUI polls `0x4200F`; the
driver answers by writing the **event status into the `ControlCode` field** of the returned
SRB_IO_CONTROL:
- `ControlCode = 2` → an event FIB follows in the payload (process it, then poll again)
- `ControlCode = 0` → no event (stop draining; poll again later)

When an event is delivered, the payload is a verbatim AIF FIB: `data[0]` = aif command/class,
`data[2]` = sub-code. A GUI should re-enumerate (containers and/or physical bus) on any
container/JBOD/HBA add/remove event, and treat class-2 (JobProgress) as a progress update
only. Poll at a modest rate — the driver already paces the firmware side; a ~1 s poll is
plenty. (The driver internally keeps one event request outstanding and self-heals a lost one;
see `docs/driver-internals.md`.)

### 0x4203F — RESET_IOP
Requests an IOP reset. The driver **acknowledges** (`ReturnCode = 1`) but deliberately does
**not** tear down a live, healthy adapter — it self-recovers via its restart path, and a
DISPATCH-level teardown with active I/O is unsafe. A GUI can send it; the controller stays up
and the GUI should simply re-enumerate afterward.

---

## 9. Diagnostics (SRCDBG) — the in-driver trace ring

Signature `"SRCDBG"`. `ControlCode = 0` resets the ring; `ControlCode = 1` reads it. The
reply payload is: `'SRCT'` magic (4) · total-writes (4) · **CISS status** (4: bit0 = a CISS
mgmt device was found, then its channel/id/lun) · reserved (4) · then up to 1024 trace
entries of 8 dwords each `{kind, a, b, c, d0, d1, d2, d3}`.

`kind`: 1/2 = mgmt FIB in/out, 3 = raw-SRB passthrough, 5 = AIF post/complete, 6/7 = physical
probe in/out, 10/11 = CISS discover, 12 = container-UID lookup. The `tools/srctrace.c` utility
prints it. This is for driver debugging, not GUI function, but it is the fastest way to see
what the firmware is actually doing.

---

## 10. Putting it together — a minimal GUI's call flow

1. **Find & open** the controller (`\\.\ScsiN:`, ping `0x42003`).
2. **Handshake**: `0x4203B`, `0x42043` (controller summary), `0x42033`/`0x4202B`.
3. **List volumes**: enumerate bus 0 (Windows) and/or `VM_NameServe64` via `0x42260`;
   correlate firmware UID ↔ drive with `0x42023`/`0x42047`.
4. **List physical disks**: enumerate bus 1; pull SMART/INQUIRY per disk with `0x42264`.
5. **Monitor**: poll `0x4200F`; re-enumerate on add/remove events, show rebuild progress.
6. **Configure**: issue `VM_ContainerConfig` operations (create/delete/modify/cache/verify)
   through `0x42260`; the driver re-scans so the change appears without a reboot.
7. **Health**: `0x42043` + firmware container state from `VM_NameServe` + per-disk SMART.

Everything maxView does is some combination of the above. The driver adds no policy — it is a
faithful, re-scanning relay — so a GUI's capability ceiling is the firmware's AAC command set.

## Performance note — arcconf command latency (~15 s is normal, not a srcraid issue)

`arcconf GETCONFIG <n> AD` takes **~15 s** on this firmware. Measured on box 10.70.10.80 (2026-11-20),
this is **inherent to arcconf + the Series 7 firmware, not a srcraid defect** — it was confirmed by
running the same commands against the **vendor WHQL driver (arcsas 7.5.0.54013)**:

| arcconf command | srcraid 0.1.0.216 | vendor arcsas 7.5 |
|---|---|---|
| GETVERSION      | fast (<2 s)       | **~15 s** |
| GETCONFIG AD    | **~15 s**         | **~15 s** |
| GETCONFIG PD    | fast              | **~15 s** |
| GETCONFIG LD    | fast              | **~15 s** |

So the vendor driver is **~15 s for *every* arcconf invocation** (a fixed per-call cost — arcconf's
controller/enclosure probe on startup). srcraid is actually **faster than the vendor** for most
commands; only the AD query (full adapter details incl. enclosure/temperature/cache-backup) hits the
same ~15 s. Nothing to fix in srcraid. Consumers that need the controller settings quickly (e.g.
SrcView's Controller-Settings dialog) should load them **asynchronously** rather than block on
`GETCONFIG AD`.

---

*See also: `src/src_mgmt.c` (the authoritative implementation) and `driver-internals.md`
(how enumeration, events, resume, and multi-LUN behave under the hood).*
