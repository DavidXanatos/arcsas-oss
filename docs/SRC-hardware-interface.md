# Adaptec/PMC Series 6/7/8 "SRC" hardware interface

Authoritative hardware spec for the `srcraid` driver, derived from the observed behavior of
the vendor `arcsas` driver and cross-checked against the Linux `aacraid`
driver. Offsets marked **[HW-verified]** have been confirmed on a live ASR-71605.

## 1. PCI identity & interface selection

| | |
|---|---|
| Vendor | `0x9005` (Adaptec/PMC) |
| Device | `0x028B` Series 6 (Tupelo), **`0x028C` Series 7 (Denali)**, `0x028D` Series 8 (Yosemite) |
| Our card | `VEN_9005 DEV_028C SUBSYS_05039005` rev 01 (a Series 7) |

The vendor driver selects the register layout with `devid - 0x28B <= 2` → the **SRC** path
(Series 6/7/8). Anything else is the older IOP/Rx path with different offsets.

## 2. BARs (as StorPort reports them) [HW-verified]

| AR | Type | Example | Use |
|---|---|---|---|
| 0 | mem 64-bit | `0xfb600000`, **0x100000 (1 MB)** | **SRC register window** (map this) |
| 1 | mem | `0xfb780000`, 0x400 (1 KB) | MSI-X table |
| 2 | **I/O** | `0xe000`, 0x100 | **unused** — opt out via INF `Needs=PciIoSpaceNotRequired`, else PnP start fails |

The vendor keeps the mapped BAR0 base at `DeviceExtension+0x410`; this doc calls it
`regbase`. `srcraid` stores it in `DEVICE_EXTENSION.Regs`.

## 3. SRC message-unit registers  (offsets from `regbase`) [HW-verified]

| Off | Name | Dir | Notes |
|---|---|---|---|
| **0xBC** | **OMR0** | R | Firmware status / health. **The one I had wrong (it is NOT 0x18 for SRC).** |
| 0x20 | IDR | W | Inbound doorbell (host→adapter). SRC writes **`value << 9`**. |
| 0x34 | OIMR | R/W | Outbound interrupt mask / control (re-armed after sync cmd). |
| 0x38 | OICR | W | Outbound interrupt clear (written `3`). |
| 0x44 | OMISR | R | Outbound msg/interrupt status. |
| 0x9C | ODR_STATUS | R | Outbound doorbell/interrupt status. **Bit 12 (0x1000) = sync-command done.** |
| 0xA0 | ODR_CLEAR | W | Outbound doorbell clear/ack. Write **0x1000** to ack a sync completion. SRC writes `value << 12`. |
| 0x18 | (legacy OMR0) | R | FW status on OLDER non-SRC chips — not used by Series 6/7/8. |
| 0x2C | (legacy ODR) | - | legacy outbound doorbell (non-SRC). |
| 0x4000 | IBW | R/W | Series-8 (028D) only inbound message window. |

### Doorbell helpers (vendor semantics)
- Ring inbound: `IDR = bit << 9`.
- Ack outbound: `ODR_CLEAR = bit << 12`  (e.g. sync-done ack = `0x1000`).

## 4. Firmware status word (OMR0 @ 0xBC) [HW-verified]

Read as a 32-bit value. Bits (cross-checked against aacraid):

| Bit/val | Meaning |
|---|---|
| `0x00000080` | **KERNEL_UP_AND_RUNNING** — firmware alive |
| `0x00000100` | KERNEL_PANIC (panic code in bits 16..23) |
| `0x00000004` | SELF_TEST_FAILED |
| `0xFFFFFFFF` | device not answering on the PCI bus (D3 / config not restored) |
| `0x00010080` | observed "healthy, fully configured" on a running card |
| `0x00000080` | observed right after a cold boot (kernel up, not yet fully configured) |

**Wait-for-ready**: fast-fail if `SELF_TEST_FAILED` or
`KERNEL_PANIC`; else poll until `(status != 0xFFFFFFFF) && (status & 0x80)`, 100 µs/iter.
Implemented in `src/src_init.c : SrcWaitForFwReady`.

## 5. Sync-command channel (mailbox)

Pre-queue control path: write command+args to the mailbox, ring IDR, poll ODR_STATUS
bit 12, read the result, ack ODR_CLEAR. **Completion side is hardware-verified:**
SRC reads `ODR_STATUS (0x9c) >> 12 & 1`, result from
the mailbox DMA buffer, ack `ODR_CLEAR (0xa0) = 0x1000`, then
re-arm `OIMR (0x34)`.

**TODO P2:** the mailbox base offset within `regbase` and the exact send sequence are
still to be finalized when implementing P2. aacraid equivalent:
`src_sync_cmd()` — writes `IndexRegs.Mailbox[0..6]`, rings `INBOUND_DOORBELL`, waits
`OUTBOUND_DOORBELL`, reads `Mailbox[0..6]`.

Monitor/sync opcodes used: `INIT_STRUCT_BASE_ADDRESS` (5), `GET_ADAPTER_PROPERTIES`
(0x19), `GET_COMM_PREFERRED_SETTINGS` (0x26), `READ_PERMANENT_PARAMS` (0xa), etc.
(see `src/src_hw.h`).

## 6. Comm model (AAC new-comm / MSGU)  — P2/P3

Standard AAC: host allocates a DMA-coherent region containing the `AAC_INIT` structure,
the inbound (host→fw) queue, the outbound **Response Return Queue (RRQ)**, and a FIB pool;
the physical base is handed to the firmware with the `INIT_STRUCT_BASE_ADDRESS` sync
command. Thereafter commands are **FIBs** posted to the inbound queue and completions are
drained from the RRQ (interrupt-driven). The exact init-structure and comm-region layout
is firmware-defined and confirmed on hardware.
aacraid equivalents: `comminit.c` (`struct aac_init`, `aac_comm_init`), `commsup.c`
(FIB send/complete), `src.c` (`aac_src_init`).

## 7. Device limits — **no artificial caps**

This controller presents **both RAID containers (logical volumes) and raw pass-through
physical disks**, and with a SAS expander can address a large number of drives. The driver
must not hardcode a small limit.

- **Containers (RAID volumes):** count is queried from firmware (`GET_CONTAINER_COUNT` /
  container-config FIBs), not fixed. Historically the AAC family capped the *managed* set
  around 64, but the exposed count is firmware-driven.
- **Physical / pass-through:** bounded by the SAS topology (expanders → hundreds of PHYs),
  reported by firmware bus/target enumeration.
- **StorPort reporting:** report the storport ceiling — `MaximumNumberOfTargets = 255` per
  bus — and use multiple buses (`NumberOfBuses`) so containers and physical devices each
  get address space. The *actual* present devices come from firmware enumeration at init
  (P2/P3), so there is no driver-imposed limit; we expose what the controller reports.
- aacraid reference constants to confirm against its current source: `AAC_MAX_BUSES`,
  `AAC_MAX_TARGETS`, `AAC_MAX_LUN`, `MAXIMUM_NUM_CONTAINERS`.

**Decision implemented:** `srcraid` reports generous maxima (255 targets/bus, multiple
buses, >1 LUN) and will size queues/enumeration from the firmware's own counts in P2/P3.

## 8. Interrupts

- **INTx (line) works** and is used for bring-up.
- **MSI-X via the INF caused device start to fail with Code 10 "not supported"** on this
  box — disabled for now (INF `srcraid_MSI` AddReg commented out). Revisit in P3: the
  vendor uses MSI-X with 32 messages + affinity policy and a dedicated MSI-X interrupt
  path; our single `HwInterrupt` + INF MSI config needs more work before re-enabling.
