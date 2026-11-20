# AAC comm-init protocol for Series 7 (TYPE2 / init rev 7)

Complete firmware bring-up spec, assembled from the Linux `aacraid` driver (interface
reference) and verified against the arcsas RE + live hardware. This is what `HwInitialize`
must do to make the controller usable.

## Comm model (determined live)

Our 71605 returns from `GET_ADAPTER_PROPERTIES` (opcode `0x19`): `status[1]=0x241f1d7c`,
which has `AAC_OPT_NEW_COMM` (1<<17), `AAC_OPT_NEW_COMM_64` (1<<18) and
**`AAC_OPT_NEW_COMM_TYPE2` (1<<29)** set → **`AAC_COMM_MESSAGE_TYPE2`**, i.e. the SRC
"new comm" with a host Response Return Queue and **init struct revision 7** (`_r7`),
64-bit SG capable. `status[3]=0x100` = doorbell mask.

## Init sequence (HwInitialize)

1. **Wait firmware ready** — OMR0(+0xBC) bit 7 set (SrcWaitForFwReady).
2. **(reset if needed)** — HWSoftReset path for resume (P4), not cold boot.
3. **GET_ADAPTER_PROPERTIES (0x19)** sync cmd → decode comm type + raw_io_64 (above).
4. **GET_COMM_PREFERRED_SETTINGS (0x26)** sync cmd → sizes:
   - `status[1] >> 16` = max command size (KB) → max_sectors = that << 1
   - `status[1] & 0xFFE0` = **max_fib_size** (multiple of 32)
   - `status[2] >> 16` = max SG to driver; `status[2] & 0xFFFF` = max SG from driver
   - `status[3] >> 16` = **max outstanding FIBs** (SRC uses the high word) → can_queue; minus AAC_NUM_MGT_FIB(8)
   - `status[4] & 0xFFFF` = max_num_aif
5. **Allocate the DMA-coherent comm area** (StorPortGetUncachedExtension), containing:
   - **comm header**: `NUMBER_OF_COMM_QUEUES(8) * 2 * sizeof(u32)` = 64 B (producer/consumer index pairs)
   - **comm queues**: `struct aac_entry {__le32 size; __le32 addr;}` × `TOTAL_QUEUE_ENTRIES(1056)` = 8448 B
     - entries: HostNormCmd 8, HostHighCmd 4, AdapNormCmd 512, AdapHighCmd 4,
       HostNormResp 512, HostHighResp 4, AdapNormResp 8, AdapHighResp 4
     - align QUEUE_ALIGNMENT(16)
   - **adapter FIB pool**: `max_outstanding * max_fib_size` (the hw_fib array the adapter owns)
   - **host RRQ**: `__le32 host_rrq[max_msix * vector_cap]` — SRC completion ring. A completed
     entry holds the FIB handle with `PMC_HOST_RRQ_VALID (1<<1)`; drained host-side, zeroed after.
     INTx = 1 vector, vector_cap = outstanding.
   - **printf buffer**: small debug buffer (optional)
   - **the init struct** itself (`union aac_init._r7`, must be 32-bit-addressable)
6. **Fill `aac_init._r7`** (all `__le32`):
   `init_struct_revision=7`, `no_of_msix_vectors`, `fsrev`, `comm_header_address`=phys(comm hdr),
   `fast_io_comm_area_address`, `adapter_fibs_physical_address`=phys(FIB pool),
   `adapter_fibs_virtual_address`=0, `adapter_fibs_size`, `adapter_fib_align`=sizeof(hw_fib),
   `printfbuf`/`printfbufsiz`, `host_phys_mem_pages`(total RAM/4K, cap 0xfffff),
   `host_elapsed_seconds`(secs since 1970), `init_flags`
   (`INITFLAGS_NEW_COMM_SUPPORTED|NEW_COMM_TYPE2_SUPPORTED|DRIVER_USES_UTC_TIME|...`),
   `max_io_commands`, `max_io_size`, `max_fib_size`, `max_num_aif`,
   **`host_rrq_addr_low/high`**=phys(host RRQ).
7. **Push**: `SrcSyncCommand(dev, INIT_STRUCT_BASE_ADDRESS(5), (u32)init_pa, 0,0,0,0,0, ...)`.
   (TYPE2/r7: only the low 32 bits of the init phys addr in p1. TYPE3/r8 differs.)
8. **Start** — reset host_rrq_idx[v]=v*vector_cap; the adapter begins processing.
9. **Enumerate** — query container (logical-drive) list + physical devices via FIBs (P3).

## FIB (command block) header  — `struct aac_fibhdr` (32 B)
`__le32 XferState; __le16 Command; u8 StructType; u8 Unused; __le16 Size; __le16 SenderSize;
__le32 SenderFibAddress; union{__le32 ReceiverFibAddress / SenderFibAddressHigh / TimeStamp};
__le32 Handle; u32 Previous; u32 Next;`

## Key opcodes / constants (verified)
INIT_STRUCT_BASE_ADDRESS=5, SEND_SYNCHRONOUS_FIB=0x0c, GET_ADAPTER_PROPERTIES=0x19,
GET_COMM_PREFERRED_SETTINGS=0x26. AAC_NUM_MGT_FIB=8, AAC_MAX_HRRQ=64, AAC_MAX_MSIX=32,
PMC_HOST_RRQ_VALID=1<<1. Sync mailbox regbase+0x1000; ring IDR=0x200; done ODR_STATUS bit12;
ack ODR_CLEAR=0x1000. (See SRC-hardware-interface.md.)

## Windows implementation notes
- Comm area = `StorPortGetUncachedExtension` (DMA-coherent); get its phys via the returned
  physical address / `StorPortGetPhysicalAddress`. Must fit 32-bit phys for the init struct
  and (for TYPE2) the structures the firmware addresses with 32-bit fields.
- Command delivery (P3): build a FIB, put it in the adapter FIB pool, post its index to the
  SRC inbound queue (IQ), ring the inbound doorbell; completion drains the host RRQ.
- Status returned by a sync cmd: `status[0]==1` means success (observed on GET_ADAPTER_PROPERTIES).
