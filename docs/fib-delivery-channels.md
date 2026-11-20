# FIB delivery channels (Series 7 / SRC new-comm) — RE + hardware-verified

There are **two** ways to hand a FIB to the firmware, and a given command only
works on the right one. Discovered the hard way during P3 (build 0.1.0.20):
`VM_NameServe` is silently dropped on the inbound queue and only answered via
the synchronous mailbox.

## 1. RRQ / inbound-queue path (async, `SrcFibSubmitSync`)

Used for: `RequestAdapterInfo` (703), `VM_ContainerConfig` (2) / `CT_GET_CONTAINER_COUNT`.

- FIB header: `XferState = 0x000810ad` (HostOwned|FibInit|FibEmpty|SentFromHost|
  **ResponseExpected**|NormalPriority|**FastResponseCapable**), `StructType = FIB_MAGIC2 (4)`,
  `Handle = (slot<<2)+1`.
- Deliver: `IQ_L (+0xc0) = (fibPhys & ~0x7f) | fibsize128`, `IQ_H (+0xc4) = 0`,
  where `fibsize128 = (header.Size + 127)/128 - 1`.
- Complete: firmware posts the handle into the **host RRQ ring**. The ring is
  consumed by an **advancing index** — completion N lands at `rrq[idx]`, not
  always `rrq[0]`. Mask the toggle bit (31); `handle = entry & 0xffff`, `slot = handle>>2`.
  Advance and zero the slot.
- `CT_GET_CONTAINER_COUNT` returns `MaxContainers` = the probe **ceiling** (256),
  not the live volume count.

## 2. Synchronous-mailbox path (`SrcFibSubmitSyncEx(inPlace=TRUE)`)

Used for: FSA object-server commands — `VM_NameServe` (1) / `VM_NameServe64` (22),
and (by the vendor) the other enumeration/config FIBs. **These are dropped on the
inbound queue regardless of header** (no completion, `XferState` untouched, no RRQ /
queue / doorbell activity).

- FIB header (synchronous-FIB form): `XferState = 0x0000102d` (HostOwned|FibInit|
  FibEmpty|SentFromHost|NormalPriority — **NO ResponseExpected, NO FastResponseCapable**),
  `StructType = FIB_MAGIC2_64 (5)`, `Handle = 0`.
- `header.Size = sizeof(FIBHDR) + INPUT size` (e.g. `aac_query_mount` = 12 → Size 44),
  **not** the response size. The firmware writes the larger response (`aac_mount`, 104 B)
  back into the same 2048-byte FIB buffer regardless.
- Deliver: **mailbox** command `SEND_SYNCHRONOUS_FIB (0x0c)` with `p1 = fibPhys`
  (via `SrcSyncCommand`, same mailbox mechanism as `GET_ADAPTER_PROPERTIES` /
  `INIT_STRUCT_BASE_ADDRESS`). This is aacraid `aac_adapter_sync_cmd(SEND_SYNCHRONOUS_FIB,
  hw_fib_pa, ...)`, which is how the vendor driver delivers a synchronous FIB.
- Complete: firmware processes the FIB **in place**, sets `AdapterProcessed`
  (XferState bit 9), raises the sync-done doorbell (ODR bit 12). Read the response
  from the FIB data area.

### Container probe (VM_NameServe64 → aac_mount)

`{command=22, type=FT_FILESYS(8), count=cid}` → `aac_mntent`: valid when
`status==ST_OK(0) && vol!=CT_NONE(0)`; `size = capacity | (capacityhigh<<32)` blocks;
`state & FSCS_READONLY`. Offsets inside `mnt` (pack 4): capacity +36, vol +40,
state +48, capacityhigh +88; `mnt` begins at +12 in `aac_mount`.

Verified result (test volume): `container 0 VALID type=3 size=937144320 blocks
(~447 GiB) blk=512 ro=0`.

## WEDGE HAZARD

A timed-out or aborted sync FIB wedges the firmware's mailbox channel: subsequent
sync commands — even `GET_ADAPTER_PROPERTIES` — then time out (`rc=1`, status 0),
while `OMR0` still reads 0x80 (kernel up). A **warm reboot clears it**; a cold boot
is not required. The I/O path must add a firmware-health check + IOP-reset recovery
(aacraid `aac_src_check_health` / `aac_src_restart_adapter`) so a stuck command can't
strand the controller until a reboot.
