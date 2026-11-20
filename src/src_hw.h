/*
 * src_hw.h - Adaptec/PMC Series 6/7/8 "SRC" hardware interface.
 *
 * Register offsets are derived from the SRC interface and cross-checked against
 * the open-source Linux aacraid driver
 * (drivers/scsi/aacraid/src.c, src.h). These are the values this exact silicon uses.
 *
 * Interface selection is by PCI device id:
 *   0x028B Series 6 (Tupelo), 0x028C Series 7 (Denali, our 71605), 0x028D Series 8.
 * All three use the "SRC" message unit (the (devid-0x28B) <= 2 test in the vendor driver).
 */
#pragma once

/* ---- PCI ---- */
#define SRC_VID                 0x9005
#define SRC_DID_SERIES6         0x028B
#define SRC_DID_SERIES7         0x028C   /* primary target */
#define SRC_DID_SERIES8         0x028D

/*
 * SRC message-unit registers, byte offsets within the mapped BAR window
 * (the StorPortGetDeviceBase result). SRC (Series 6/7/8) register map:
 *   OMR0 (FW status/health):   [base+0xBC]   (older IOP: [base+0x18])
 *   IDR  (inbound doorbell):   [base+0x20], SRC writes (val << 9)
 *   ODR  (outbound doorbell):  SRC -> [base+0xa0] (val << 12); older -> [base+0x2c]
 */
#define SRC_REG_OMR0            0xbc    /* Outbound Message Reg 0 = FW status/health (SRC) */
#define SRC_REG_IOAR            0x18    /* IOA->host interrupt register (MSI-X setup)  */
#define SRC_REG_ODR_MSI         0xc8    /* MSI doorbell (sync/AIF cause in MSI-X mode) */
#define SRC_REG_IDR             0x20    /* Inbound  Doorbell Register (host->adapter)  */
#define SRC_REG_OIMR            0x34    /* Outbound Interrupt Mask / control           */
#define SRC_REG_OICR            0x38    /* Outbound Interrupt Clear (written 3)        */
#define SRC_REG_OMISR           0x44    /* Outbound msg/interrupt status (read)        */
#define SRC_REG_ODR_STATUS      0x9c    /* SRC Outbound Doorbell/intr status (read)    */
#define SRC_REG_ODR_CLEAR       0xa0    /* SRC Outbound Doorbell clear (host ack)      */
#define SRC_REG_ODR_LEGACY      0x2c    /* legacy (non-SRC) outbound doorbell          */
#define SRC_REG_OMR_LEGACY      0x18    /* legacy (non-SRC) FW status                  */
#define SRC_SERIES8_IBW         0x4000  /* Series-8-only inbound message window        */
#define SRC_REG_IQ_L            0xc0    /* Inbound Queue low  (normal FIB submit)      */
#define SRC_REG_IQ_H            0xc4    /* Inbound Queue high                          */
#define SRC_REG_IQN_L           0xd0    /* Inbound Queue (Native HBA) low              */
#define SRC_REG_IQN_H           0xd4
#define SRC_ODB_RESP_SENT       0x00000002  /* ODR_STATUS: responses in host RRQ (PmDoorBellResponseSent) */
#define SRC_ODB_AIF_PENDING     (1u << 6)   /* shifted by SRC_ODR_SHIFT in ODR_STATUS */

/* Doorbell value shifts for the SRC message unit */
#define SRC_IDR_SHIFT           9       /* inbound doorbell: write (bit << 9)  */
#define SRC_ODR_SHIFT           12      /* outbound ack:     write (bit << 12) */

/* Outbound doorbell bits (post-shift, as written/read at ODR_STATUS/ODR_CLEAR) */
#define SRC_ODB_SYNC_DONE       0x00001000  /* bit 12: sync command complete (OUTBOUND_DOORBELL_0<<12) */
#define SRC_IDB_SYNC            0x00000200  /* INBOUND_DOORBELL_0 << 9: signal a new sync command */

/*
 * Sync-command mailbox location within the register window, per device id
 * (verified from the vendor driver's mailbox layout). 8 dwords: Mailbox[0]=command/status,
 * Mailbox[1..6]=args/results.
 */
#define SRC_MAILBOX_OFF_S6      0x7fc60   /* 028B Tupelo  */
#define SRC_MAILBOX_OFF_S7S8    0x1000    /* 028C Denali / 028D Yosemite */

/*
 * Firmware status bits read from OMR0 (SRC_REG_OMR0). The vendor health check
 * treats bit 7 (0x80) set as "up and running" and bit 8
 * (0x100) as a kernel panic; a running Series 7 reads 0x00010080.
 *   (matches aacraid: KERNEL_UP_AND_RUNNING, KERNEL_PANIC, SELF_TEST_FAILED, ...)
 */
#define FW_SELF_TEST_FAILED     0x00000004
#define FW_MONITOR_PANIC        0x00000020   /* bit 5  */
#define FW_KERNEL_BOOTING       0x00000040   /* bit 6: still (re)booting */
#define FW_UP_AND_RUNNING       0x00000080   /* bit 7  */
#define FW_KERNEL_PANIC         0x00000100   /* bit 8  */
#define FW_STATUS_MASK          0xFFFF0000   /* high word carries fw-status code */
#define FW_HEALTHY_RUNNING      0x00010080   /* observed healthy value on 71605 */

/* Inbound doorbell command bits (host->adapter), pre-shift */
#define INBOUND_DB_DBELL        0x00000004   /* generic "doorbell" notify       */
#define INBOUND_DB_INTR         0x00000002   /* normal command interrupt        */
#define INBOUND_DB_SRC_DBELL    0x00000800   /* SRC variant of notify           */
#define INBOUND_DB_SRC_INTR     0x00000400   /* SRC variant of command intr     */

/*
 * SRC "new comm" sync-command mailbox. The adapter exposes an 8-dword mailbox and
 * a sync-command doorbell. The vendor driver writes the
 * command + args to the inbound mailbox, rings IDR, and polls OMR0/IISR for
 * completion, reading the status back from the mailbox. Exact mailbox base offset
 * and the 0x9C00001F control word are being determined in P2; placeholders
 * below are refined before Initialize sends a real sync command.
 */
#define SRC_MAILBOX_CMD         0x9C00001F   /* control word seen written to OMR0 path */

/* Monitor/sync command opcodes (AAC common, used by SRC) - verified vs aacraid */
#define BREAKPOINT_REQUEST      0x00000004
#define INIT_STRUCT_BASE_ADDRESS 0x00000005  /* tell fw the AAC_INIT phys base    */
#define SEND_SYNCHRONOUS_FIB    0x0000000c
#define READ_PERMANENT_PARAMS   0x0000000a
#define GET_ADAPTER_PROPERTIES  0x00000019
#define RCV_TEMP_READINGS       0x00000025
#define GET_COMM_PREFERRED_SETTINGS 0x00000026
#define IOP_RESET_ALWAYS        0x00001001   /* notify fw of IOP reset (mailbox) */
#define IOP_RESET               0x00001000
#define IOP_SRC_RESET_MASK      0x00000100   /* written raw to IDR to trigger reset */

/* MSI-X enable (aacraid aac_src_access_devreg / PMC bits) */
#define AAC_INT_ENABLE_MSIX     0xFFFBFFFAu  /* OIMR: unmask MSI-X (bit1) + AIF (bit18) */
#define AAC_INT_ENABLE_INTX     0xFFFBFFFBu  /* OIMR: unmask responses (bit2) + AIF (bit18) */
#define SRC_IDR_MSIX_ENABLE     0x40         /* IDR bit 6: switch fw to MSI-X mode */
#define SRC_IDR_CLEAR_AIF       0x20         /* IDR bit 5: clear AIF doorbell       */
#define SRC_IDR_CLEAR_SYNC      0x10         /* IDR bit 4: clear sync doorbell      */
#define PMC_ALL_INTERRUPT_BITS  0x9C00001Fu  /* written to IOAR to arm interrupts   */
#define SRC_ODB_MSI_AIF         0x00040000   /* ODR_MSI bit 18: AIF pending         */
#define SRC_ODB_MSI_SYNC        0x00001000   /* ODR_MSI bit 12: sync-cmd complete   */

/* GET_ADAPTER_PROPERTIES status[1] option bits (aacraid AAC_OPT_*) */
#define AAC_OPT_NEW_COMM        (1u << 17)
#define AAC_OPT_NEW_COMM_64     (1u << 18)
#define AAC_OPT_NEW_COMM_TYPE1  (1u << 28)
#define AAC_OPT_NEW_COMM_TYPE2  (1u << 29)
#define AAC_OPT_NEW_COMM_TYPE3  (1u << 30)
#define AAC_OPT_NEW_COMM_TYPE4  (1u << 31)

/* comm interface model (aacraid values) */
#define AAC_COMM_PRODUCER       0
#define AAC_COMM_MESSAGE        1
#define AAC_COMM_MESSAGE_TYPE1  3   /* Tupelo / Series 6  (init rev 6) */
#define AAC_COMM_MESSAGE_TYPE2  4   /* Denali/Yosemite / Series 7/8 (init rev 7) - our card */
#define AAC_COMM_MESSAGE_TYPE3  5

#define AAC_NUM_MGT_FIB         8
#define PMC_HOST_RRQ_VALID      (1u << 1)

/*
 * AAC init structure revisions (comminit). SRC/Series 6+ uses rev 7 ("MSGU") comm
 * with the host RRQ (Response Return Queue). Full struct aac_init layout is added
 * in P2 from comminit RE + aacraid comminit.c.
 */
#define ADAPTER_INIT_STRUCT_REVISION      3
#define ADAPTER_INIT_STRUCT_REVISION_4    4   /* rev 4 : 64-bit            */
#define ADAPTER_INIT_STRUCT_REVISION_6    6
#define ADAPTER_INIT_STRUCT_REVISION_7    7   /* rev 7 : SRC new-comm/MSGU */

/* Host RRQ / inbound-queue message-unit constants (SRC new comm) - refined in P3 */
#define SRC_NUM_MSIX            16
#define SRC_MAX_FIB            512     /* outstanding FIBs (vendor caps similarly)  */

/* ---- AAC comm structures (TYPE2 / init rev 7). All fields little-endian; the
 *      layouts are naturally aligned so no packing pragma is needed. ---- */

/* Init struct revision 7 (Denali/Series 7). 80 bytes. */
typedef struct _AAC_INIT_R7 {
    ULONG init_struct_revision;
    ULONG no_of_msix_vectors;
    ULONG fsrev;
    ULONG comm_header_address;            /* phys32 of the comm queue header */
    ULONG fast_io_comm_area_address;
    ULONG adapter_fibs_physical_address;  /* phys32 of the adapter FIB pool  */
    ULONG adapter_fibs_virtual_address;
    ULONG adapter_fibs_size;
    ULONG adapter_fib_align;
    ULONG printfbuf;
    ULONG printfbufsiz;
    ULONG host_phys_mem_pages;            /* total RAM / 4K (cap 0xfffff) */
    ULONG host_elapsed_seconds;           /* seconds since 1970 */
    ULONG init_flags;                     /* INITFLAGS_* */
    ULONG max_io_commands;
    ULONG max_io_size;
    ULONG max_fib_size;
    ULONG max_num_aif;
    ULONG host_rrq_addr_low;              /* phys of host RRQ (SRC completion ring) */
    ULONG host_rrq_addr_high;
} AAC_INIT_R7, *PAAC_INIT_R7;

/* init_flags bits */
#define INITFLAGS_NEW_COMM_SUPPORTED        0x00000001
#define INITFLAGS_DRIVER_USES_UTC_TIME      0x00000010
#define INITFLAGS_DRIVER_SUPPORTS_PM        0x00000020
#define INITFLAGS_NEW_COMM_TYPE1_SUPPORTED  0x00000040
#define INITFLAGS_FAST_JBOD_SUPPORTED       0x00000080
#define INITFLAGS_NEW_COMM_TYPE2_SUPPORTED  0x00000100
#define INITFLAGS_DRIVER_SUPPORTS_HBA_MODE  0x00000400

/* comm queue entry (adapter<->host producer/consumer queues) */
typedef struct _AAC_QENTRY { ULONG size; ULONG addr; } AAC_QENTRY, *PAAC_QENTRY;

/* FIB header (32 bytes) */
typedef struct _AAC_FIBHDR {
    ULONG  XferState;
    USHORT Command;
    UCHAR  StructType;
    UCHAR  Unused;
    USHORT Size;
    USHORT SenderSize;
    ULONG  SenderFibAddress;
    ULONG  ReceiverFibAddress;   /* union: / SenderFibAddressHigh / TimeStamp */
    ULONG  Handle;
    ULONG  Previous;
    ULONG  Next;
} AAC_FIBHDR, *PAAC_FIBHDR;

#define SRC_FIB_SIZE            2048    /* negotiated max_fib_size (Series 7) */

/* FIB StructType magics */
#define FIB_MAGIC               0x0001
#define FIB_MAGIC2              0x0004   /* TYPE2 new-comm */
#define FIB_MAGIC2_64           0x0005

/* FIB XferState bits (aacraid enum fib_xfer_state) */
#define XS_HostOwned            (1u << 0)
#define XS_FibInitialized       (1u << 2)
#define XS_FibEmpty             (1u << 3)
#define XS_SentFromHost         (1u << 5)
#define XS_ResponseExpected     (1u << 7)
#define XS_NoResponseExpected   (1u << 8)
#define XS_AdapterProcessed     (1u << 9)   /* firmware set: in-place completion */
#define XS_NormalPriority       (1u << 12)
#define XS_Async                (1u << 13)  /* asynchronous FIB (required for block I/O) */
#define XS_FastResponseCapable  (1u << 19)
/* state for a synchronous host->adapter command FIB */
#define XS_SEND_SYNC  (XS_HostOwned | XS_FibInitialized | XS_FibEmpty | \
                       XS_FastResponseCapable | XS_ResponseExpected | \
                       XS_SentFromHost | XS_NormalPriority)

/* FIB commands (header.Command) */
#define FIBCMD_VM_NameServe          1     /* container (logical drive) enumeration */
#define FIBCMD_VM_ContainerConfig    2     /* container config (payload cmd value)  */
#define FIBCMD_ContainerCommand      500
#define FIBCMD_ContainerCommand64    501
#define FIBCMD_ContainerRawIo        502
#define FIBCMD_ContainerRawIo2       503   /* SRC block I/O (verified on live HW) */
#define FIBCMD_RequestAdapterInfo    703
#define FIBCMD_SetAdapterTime        0x2C1 /* 705: send host wall-clock secs so FW log/SMART/
                                            * enclosure timestamps stay aligned (vendor SetArcsasFwTime) */

/* VM dispatch classes for container block I/O.
 * 32-bit path (this firmware): VM_CtBlockRead/Write (6/7) + ContainerCommand
 *   (500) + aac_read (DWORD cid/block/count-in-BYTES) + 8-byte SG32 entries.
 * 64-bit paths (ContainerCommand64 (501), rejected ST_INVAL on this firmware):
 *   VM_CtBlockRead64/Write64 (16/17) and VM_CtHostRead64/Write64 (19/20). */
#define VM_CtBlockRead               6
#define VM_CtBlockWrite              7
#define VM_CtBlockRead64             16
#define VM_CtBlockWrite64            17
#define VM_CtHostRead64              19
#define VM_CtHostWrite64             20

/*
 * 64-bit block read/write FIB (aacraid struct aac_read64/aac_write64), carried
 * by FIBCMD_ContainerCommand64. block is a 32-bit LBA (<= 2 TB at 512 B);
 * larger volumes need ContainerRawIo2 (a later addition). sg is an sgmap64:
 * ULONG count followed by `count` sgentry64 { addrLow, addrHigh, len }.
 */
#include <pshpack4.h>
typedef struct _AAC_SGENTRY64 {
    ULONG   addrLow;
    ULONG   addrHigh;
    ULONG   len;
} AAC_SGENTRY64;

typedef struct _AAC_READWRITE64 {
    ULONG   command;                 /* VM_CtHostRead64 / VM_CtHostWrite64 */
    USHORT  cid;
    USHORT  sectorCount;
    ULONG   block;                   /* LBA (low 32 bits) */
    USHORT  pad;
    USHORT  flags;
    ULONG   sgCount;                 /* sgmap64.count */
    AAC_SGENTRY64 sg[1];             /* variable length */
} AAC_READWRITE64, *PAAC_READWRITE64;

/* 32-bit block read/write (aacraid struct aac_read/aac_write), carried by
 * FIBCMD_ContainerCommand. All DWORD fields; count is the transfer length in
 * BYTES (not sectors). sg is an sgmap: ULONG count + sgentry { addr32, len }. */
typedef struct _AAC_SGENTRY32 {
    ULONG   addr;
    ULONG   len;
} AAC_SGENTRY32;

typedef struct _AAC_READWRITE32 {
    ULONG   command;                 /* VM_CtBlockRead / VM_CtBlockWrite */
    ULONG   cid;
    ULONG   block;                   /* LBA (32-bit) */
    ULONG   byteCount;               /* transfer length in BYTES */
    ULONG   sgCount;                 /* sgmap.count */
    AAC_SGENTRY32 sg[1];             /* variable length */
} AAC_READWRITE32, *PAAC_READWRITE32;
#include <poppack.h>
#define AAC_RW32_FIXED  (FIELD_OFFSET(AAC_READWRITE32, sg))
#define AAC_RW64_FIXED  (FIELD_OFFSET(AAC_READWRITE64, sg))  /* bytes before sg[] */

/*
 * ContainerRawIo2 (FIBCMD 503) - the block read/write command THIS firmware
 * actually accepts (VM_CtHostRead64/501 is rejected ST_INVAL). Verified against
 * the vendor driver's behavior on a live READ(10):
 *   hdr.Command=503, hdr.StructType=4, hdr.Size=0x50 (hdr 0x20 + fixed 0x20 +
 *   1 SGE 0x10), hdr.XferState=0x000830ad (== XS_SEND_SYNC|XS_Async).
 *   payload: blockLow=LBA, byteCount in BYTES, cid=container id, flags=0x2011
 *   (READ|SGL_CONFORMANT|SG_FORMAT_IEEE1212), sgeCnt=1,
 *   sge[0]={addrLow,addrHigh,len,flags=0}.
 * SGE is the 16-byte IEEE1212 form (addr64 + len + flags), NOT the 12-byte
 * sgentry64 used by the (rejected) aac_read64 path.
 */
#define RIO2_IO_TYPE_WRITE       0x0000
#define RIO2_IO_TYPE_READ        0x0001
#define RIO2_IO_TYPE_VERIFY      0x0002
#define RIO2_IO_TYPE_MASK        0x0003
#define RIO2_IO_ERROR            0x0004
#define RIO2_IO_SUREWRITE        0x0008
#define RIO2_SGL_CONFORMANT      0x0010   /* all SGEs but last == sgeFirstSize */
#define RIO2_SG_FORMAT_ARC       0x0000
#define RIO2_SG_FORMAT_SRL       0x1000
#define RIO2_SG_FORMAT_IEEE1212  0x2000

#include <pshpack4.h>
typedef struct _AAC_SGE_IEEE1212 {
    ULONG   addrLow;
    ULONG   addrHigh;
    ULONG   length;
    ULONG   flags;                   /* 0 */
} AAC_SGE_IEEE1212;                  /* 16 bytes */

typedef struct _AAC_RAW_IO2 {
    ULONG   blockLow;                /* +0x00 LBA low  */
    ULONG   blockHigh;               /* +0x04 LBA high (true 64-bit LBA) */
    ULONG   byteCount;               /* +0x08 transfer length in BYTES */
    USHORT  cid;                     /* +0x0c container id */
    USHORT  flags;                   /* +0x0e RIO2_* */
    ULONG   sgeFirstSize;            /* +0x10 length of first SGE element */
    ULONG   sgeNvme;                 /* +0x14 reserved (0) */
    ULONG   sgeCnt;                  /* +0x18 number of SGE entries */
    ULONG   resv;                    /* +0x1c reserved (0) */
    AAC_SGE_IEEE1212 sge[1];         /* +0x20 variable length */
} AAC_RAW_IO2, *PAAC_RAW_IO2;
#include <poppack.h>
#define AAC_RAWIO2_FIXED  (FIELD_OFFSET(AAC_RAW_IO2, sge))  /* 0x20 */

/*
 * aac_srb SCSI passthrough (FIB Command ScsiPortCommand64=601, StructType=1).
 * Used for physical (pass-through) device I/O and for CISS management commands
 * (REPORT PHYSICAL LUNS). Payload = struct aac_srb at FIB+0x20 (Linux layout),
 * SG = sgmap64 { count, 12-byte sgentry64 }. Reply = aac_srb_reply in place.
 */
#define FIBCMD_ScsiPortCommand       600   /* aac_srb, SG32 */
#define FIBCMD_ScsiPortCommand64     601   /* aac_srb, SG64 (host64 path) */

#define SRBF_ExecuteScsi             0x0000
#define AAC_SRB_DataIn               0x0040   /* == SRB_FLAGS_DATA_IN  */
#define AAC_SRB_DataOut              0x0080   /* == SRB_FLAGS_DATA_OUT */

/* CISS commands (sent as the CDB of an aac_srb to the controller mgmt device) */
#define CISS_REPORT_PHYSICAL_LUNS    0xC3
#define CISS_IDENTIFY_PHYSICAL_DEVICE 0x15

/* physical->virtual channel map (aacraid): containers on bus 0, physicals on
 * aac_phys_to_logical(phys_bus) = phys_bus+1, enclosure on bus 3. */
#define SRC_CONTAINER_CHANNEL        0
#define SRC_NATIVE_CHANNEL           1
#define SRC_ENCLOSURE_CHANNEL        3

#include <pshpack4.h>
typedef struct _AAC_SRB {
    ULONG   function;                /* SRBF_ExecuteScsi */
    ULONG   channel;                 /* firmware bus */
    ULONG   id;                      /* firmware target */
    ULONG   lun;
    ULONG   timeout;
    ULONG   flags;                   /* AAC_SRB_DataIn / DataOut */
    ULONG   count;                   /* data transfer length (bytes) */
    ULONG   retryLimit;
    ULONG   cdbSize;
    UCHAR   cdb[16];
    ULONG   sgCount;                 /* sgmap64.count */
    AAC_SGENTRY64 sg[1];             /* 12-byte { addrLow, addrHigh, len } */
} AAC_SRB, *PAAC_SRB;

typedef struct _AAC_SRB_REPLY {
    ULONG   status;
    ULONG   srbStatus;
    ULONG   scsiStatus;
    ULONG   dataXferLength;
    ULONG   senseDataSize;
    UCHAR   senseData[30];
} AAC_SRB_REPLY, *PAAC_SRB_REPLY;
#define AAC_SRB_FIXED  (FIELD_OFFSET(AAC_SRB, sg))

/* CISS REPORT PHYSICAL LUNS response (aac_ciss_phys_luns_resp): 8-byte header
 * then 24-byte entries. list_length is big-endian bytes of the lun[] array. */
typedef struct _CISS_LUN {
    UCHAR   tid[3];                  /* target id */
    UCHAR   bus;                     /* bus; flag bits 6,7 */
    UCHAR   level3[2];
    UCHAR   level2[2];
    UCHAR   nodeIdent[16];           /* physical node identifier */
} CISS_LUN, *PCISS_LUN;

typedef struct _CISS_PHYS_LUNS_RESP {
    UCHAR   listLength[4];           /* big-endian byte count of lun[] */
    UCHAR   respFlag;                /* extended response flag (==2) */
    UCHAR   reserved[3];
    CISS_LUN lun[1];
} CISS_PHYS_LUNS_RESP, *PCISS_PHYS_LUNS_RESP;
#include <poppack.h>

/* VM_ContainerConfig sub-types (payload type value) */
#define CT_GET_CONTAINER_COUNT       4
#define CT_FLUSH_CACHE               129   /* 0x81: flush the controller write-back cache for a cid
                                            * (aacraid aac_synchronize -> SYNCHRONIZE CACHE) */
#define CT_READ_NAME                 130
#define CT_GET_CONFIG_STATUS         147   /* 0x93 */
#define CT_COMMIT_CONFIG             152   /* 0x98: bring pending config online */
#define CT_CID_TO_32BITS_UID         165   /* 0xA5: firmware 32-bit container UID (maxView mapping) */
#define CT_IS_ADAPTER_READY          253   /* 0xFD: has the firmware finished computing its config? */

/* VM dispatch classes (ContainerCommand payload[0]) */
#define VM_NameServe                 1
#define VM_NameServeAllBlk           30    /* 0x1E - enumerate ALL block devices incl. variable/4K-
                                            * sector containers (the vendor uses this dispatch class
                                            * when a container has a variable block size) */
#define FT_BLK                       3     /* block-device object (VM_NameServeAllBlk mount type) */
#define VM_NameServe64               22    /* 64-bit capacity probe          */
#define FT_FILESYS                   8     /* Adaptec FSA container object   */

/* mount / probe response flags + sentinels */
#define CT_NONE                      0     /* mnt[0].vol == CT_NONE => empty */
#define ST_OK                        0     /* aac_mount.status               */
#define FSCS_READONLY                0x0002
#define FSCS_HIDDEN                  0x0004   /* firmware-hidden container - do not present to Windows */
#define FSCS_NOT_READY               0x0008

/*
 * Container probe wire structs (aacraid struct aac_query_mount / aac_mount).
 * All fields are naturally 4-byte aligned; pack(4) guards against any ABI
 * padding so the ULONG offsets match the firmware exactly.
 */
#include <pshpack4.h>
typedef struct _AAC_QUERY_MOUNT {
    ULONG   command;                       /* VM_NameServe64 */
    ULONG   type;                          /* FT_FILESYS     */
    ULONG   count;                         /* container id   */
} AAC_QUERY_MOUNT, *PAAC_QUERY_MOUNT;

typedef struct _AAC_CREATION_INFO {        /* 16 bytes */
    UCHAR   buildnum, usec, via, year;
    ULONG   date;
    ULONG   serial[2];
} AAC_CREATION_INFO;

typedef struct _AAC_MNTENT {
    ULONG   oid;
    UCHAR   name[16];
    AAC_CREATION_INFO create_info;         /* 16 */
    ULONG   capacity;                      /* low 32 bits of block count */
    ULONG   vol;                           /* substrate type; CT_NONE => none */
    ULONG   obj;                           /* FT_FILESYS, ...  */
    ULONG   state;                         /* FSCS_* flags     */
    ULONG   fileinfo[8];                   /* union aac_contentinfo, 32 bytes */
    ULONG   altoid;
    ULONG   capacityhigh;                  /* high 32 bits of block count */
} AAC_MNTENT;

typedef struct _AAC_MOUNT {
    ULONG   status;                        /* ST_OK */
    ULONG   type;
    ULONG   count;
    AAC_MNTENT mnt[1];
} AAC_MOUNT, *PAAC_MOUNT;
#include <poppack.h>

/* host RRQ completion entry bits */
#define RRQ_TOGGLE          0x80000000
#define RRQ_FASTRESP        0x40000000
#define RRQ_HANDLE_MASK     0x0000ffff

/* comm queue layout (aacraid aac_comm_init) */
#define NUMBER_OF_COMM_QUEUES   8
#define HOST_NORM_CMD_ENTRIES   8
#define HOST_HIGH_CMD_ENTRIES   4
#define ADAP_NORM_CMD_ENTRIES   512
#define ADAP_HIGH_CMD_ENTRIES   4
#define HOST_NORM_RESP_ENTRIES  512
#define HOST_HIGH_RESP_ENTRIES  4
#define ADAP_NORM_RESP_ENTRIES  8
#define ADAP_HIGH_RESP_ENTRIES  4
#define TOTAL_QUEUE_ENTRIES     1056   /* sum of the 8 above */
#define QUEUE_ALIGNMENT         16
