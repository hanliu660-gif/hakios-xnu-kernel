/*
 * PCIe0 on the Xiaomi 12X (psyche, SM8250-AC kona): bring the link up to the
 * QCA6390 that is the phone's Wi-Fi, and read its identity. Only with a
 * boot-arg, and nothing here is written unless one asks for it.
 *
 * Why this file exists. The Wi-Fi driver this port has (hakios.wifi.c) is for
 * the POCO X5 Pro's WCN6750: a Hexagon DSP inside the SoC that runs the radio's
 * firmware from the phone's flash. This phone has no WPSS at all (its device
 * tree names wpss 0 times and qca6390 7 times): its radio is a QCA6390 on the
 * other side of PCI Express, with its own firmware loaded over MHI. Different
 * chip, different bus, different way of getting the firmware in. Nothing in
 * hakios.wifi.c applies below the ath11k layer.
 *
 * Where the sequence comes from. apollo (the Xiaomi Mi 10T port, also SM8250)
 * brought its QCA6390 up, and it came from the Xiaomi Mi 10 port
 * (github.com/JopangJovan/hackios-xnu-kernel-umi devices/xiaomi-mi-10,
 * umi_pcie.c, APSL 2.0). This is that sequence, checked against this phone's
 * own device tree rather than trusted:
 *
 *   pcie@1c00000  reg-names parf phy dm_core elbi iatu conf, and every one of
 *                 those six windows has the same address here as on apollo:
 *                 0x01c00000, 0x01c06000, 0x60000000, 0x60001000, 0x60100000
 *   qcom,phy-sequence  99 triples. Byte for byte the same 99 that apollo's
 *                 sweet_pcie_phy.h holds -- dumped from this phone's device
 *                 tree and compared entry by entry, not assumed.
 *   qcom,ep-latency  0xa (10 ms), the wait between the PHY being ready and
 *                 PERST going high.
 *   PERST / CLKREQ / WAKE  gpio79 / gpio80 / gpio81, and wlan-en gpio20, with
 *                 the same pull, drive strength and function (pci_e0 for
 *                 CLKREQ, plain gpio for the other three) as apollo.
 *   the radio's supplies  vdd-wlan-io smpa4, vdd-wlan-rfa1 smpa5,
 *                 vdd-wlan-aon and vdd-wlan-dig both smpa6, vdd-wlan-rfa2
 *                 smpc8 -- the same rails apollo votes, including the one
 *                 thing apollo's device tree changes from kona's stock: the
 *                 digital rail on S6 rather than PM8009 S2.
 *
 * So the only thing this port supplies that apollo had to guess at is the
 * rail voltages, and it does not guess: it reads each rail from RPMh first
 * (the touch screen's driver learned that an unconditional vote is what hangs
 * this phone) and votes only a rail that is off.
 *
 * Boot-args:
 *   pcie_step=1       read-only survey: the PCIe0 clocks and resets in GCC, and
 *                     the radio's four pins. Nothing written, no RPMh, no link.
 *                     This is the step that says whether the windows above are
 *                     readable from this kernel at all -- the concern
 *                     hakios.wifi.c records for its own region (a read that
 *                     ended the kernel with an asynchronous abort) is the
 *                     reason it is separate from step 2.
 *   pcie_step=2       and then: the rails, wlan-en, the PCIe0 clocks, the PHY
 *                     and the link, and the QCA6390's vendor and device ID.
 *                     The endpoint's bus mastering stays off: this step asks
 *                     whether the radio answers, not to use it.
 *   pcie=1            the same as pcie_step=2.
 *   pcie_delay=N      seconds to wait before starting (default 3), so that the
 *                     run is otherwise quiet.
 *
 * Status: NOT RUN ON THE PHONE YET. Written 2026-11-04 from apollo's measured
 * sequence and this phone's device tree. The line that says it worked is
 * "pcie: the QCA6390 answers on PCIe0" with the ID 17cb:1101; until a run
 * prints it, treat every value below as apollo's, not this phone's.
 *
 * What comes next (not in this file): SMMU stream 0x1c01 and its context, bus
 * mastering, MHI (the channel and event rings the firmware is downloaded
 * through), the firmware itself from /vendor/firmware_mnt, then ath11k's
 * bus layer pointed at PCI instead of AHB. apollo has each of those as a
 * separate file (sweet_mhi.c, sweet_wifi.c) and the same split is intended
 * here: hakios.pcie.c owns the link, hakios.mhi.c would own the transport.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, uint32_t flags);
extern lck_mtx_t hakios_rsc_lock;       /* RPMh's sequencer, shared with the other drivers */

SYSCTL_DECL(_kern_hakios);

/* ---- the windows, from this phone's device tree (reg-names of pcie@1c00000) -------------------- */

#define GCC_PHYS                0x00100000u     /* dt: qcom,gcc@100000, reg 0x100000 0x1f0000 */
#define GCC_SIZE                0x0a0000u
#define TLMM_NORTH_PHYS         0x0f900000u     /* kona NORTH tile: gpio 20 and 79..81 live here */
#define TLMM_NORTH_SIZE         0x080000u
#define PARF_PHYS               0x01c00000u     /* "parf" */
#define PARF_SIZE               0x00003000u
#define PHY_PHYS                0x01c06000u     /* "phy" */
#define PHY_SIZE                0x00001000u
#define DBI_PHYS                0x60000000u     /* "dm_core" */
#define DBI_SIZE                0x00001000u
#define ATU_PHYS                0x60001000u     /* "iatu" */
#define ATU_SIZE                0x00001000u
#define CONF_PHYS               0x60100000u     /* "conf" */
#define CONF_SIZE               0x00100000u
#define BAR_PHYS                0x60300000u     /* dt ranges: 0x60300000 for 0x3d00000 */
#define BAR_SIZE                0x3d00000u     /* the whole window the device tree
                                                 * describes; the copy engines are at
                                                 * +0xa00000, past the first MiB */

/*
 * The PCIe MSI controller (dt: pcie0_msi@17a10040, qcom,pci-msi) and the application processors'
 * SMMU (dt: apps-smmu@15000000, qcom,qsmmu-v500).  Both windows start on a page boundary: the
 * MSI register sits at 0x40 into its page, and 0x2000 of SMMU registers covers SCR0/IDR0/IDR1 and
 * every SMR/S2CR pair (the last one ends at 0xffc).
 */
#define MSI_PHYS                0x17a10000u
#define MSI_SIZE                0x00002000u
#define MSI_REGISTER            0x17a10040u
#define SMMU_PHYS               0x15000000u
#define SMMU_SIZE               0x00100000u     /* dt: reg 0x15000000 0x100000 */

/*
 * The SMMU's registers.  The first page is the global set (SCR0, IDR0, IDR1 and one SMR/S2CR pair
 * per stream); the pages after it hold one 4 KiB context bank each.  A context's own page holds
 * SCTLR, TCR/TCR2, TTBR0/TTBR1, MAIR and the fault registers.
 */
#define SMMU_SCR0               0x000u
#define SMMU_IDR0               0x020u
#define SMMU_IDR1               0x024u
#define SMMU_SMR(n)             (0x800u + 4u * (n))
#define SMMU_S2CR(n)            (0xc00u + 4u * (n))
#define SMMU_SMR_VALID          (1u << 31)
#define SMMU_S2CR_TRANSLATE     (0u << 16)
#define SMMU_CBAR(n)            (4u * (n))              /* in the second page */
#define SMMU_CBA2R(n)           (0x800u + 4u * (n))     /* in the second page */
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R_64_BIT       1u
#define SMMU_CB_SCTLR           0x00u
#define SMMU_CB_TCR2            0x10u
#define SMMU_CB_TTBR0           0x20u
#define SMMU_CB_TTBR1           0x28u
#define SMMU_CB_TCR             0x30u
#define SMMU_CB_MAIR0           0x38u
#define SMMU_CB_MAIR1           0x3cu
#define SMMU_CB_FSR             0x58u
#define SMMU_CB_TLBIASID        0x610u
#define SMMU_CB_TLBSYNC         0x7f0u
#define SMMU_CB_TLBSTATUS       0x7f4u
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2_BITS          (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0             0xf404ff44u     /* Linux's: 0 uncached, 1 cached, 2 device */
#define SMMU_FSR_ALL            0xc00001feu

#define PTE_ADDRESS             0x0000fffffffff000ull
#define PTE_PAGE                (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)
#define PTE_DEVICE              (PTE_PAGE | 2ull << 2 | 1ull << 53 | 1ull << 54)

/* The radio's own address space, and how many 4 KiB tables are set aside for it. */
#define RADIO_IOVA              0xa0000000ull
#define RADIO_IOVA_SIZE         0x10000000ull
#define RADIO_TABLES            96u

/*
 * the three addresses stock CNSS uses
 * (apollo's sweet_wlfw.c: QCA_DDR_IOVA, QCA_CAL_IOVA, QCA_M3_IOVA).  pcie_fixed_iova=1
 * uses them instead of handing out addresses upward from RADIO_IOVA.
 */
#define QCA_DDR_IOVA            0xa1000000ull
#define QCA_CAL_IOVA            0xa1600000ull
#define QCA_M3_IOVA             0xa1880000ull

/*
 * MHI's own registers, in the endpoint's BAR (stage 2 read the layout: register length 0x100,
 * BHI at 0x100, BHIE at 0x224, channel doorbells at 0x300, event doorbells at 0x700).
 */
#define MHI_VERSION             0x08u   /* 0xdeadbeef until firmware runs: the reserved sentinel */
#define MHI_CONFIG              0x10u
#define MHI_CHANNEL_DB          0x18u
#define MHI_EVENT_DB            0x20u
#define MHI_BHI_OFFSET          0x28u
#define MHI_BHIE_OFFSET         0x2cu
#define MHI_CONTROL             0x38u
#define MHI_STATUS              0x48u
#define MHI_HOST_DMA_LOW        0x80u
#define MHI_HOST_DMA_HIGH       0x88u
#define MHI_DEV_DMA_LOW         0x98u
#define MHI_DEV_DMA_HIGH        0xa0u

/* The transfer's own registers, in the BHI block. */
#define BHI_CONTROL             0x20u   /* polling mode, vector 0 */
#define BHI_COMPLETION          0x2cu   /* the status of the last transfer */
#define BHI_ADDRESS             0x08u   /* where the image is, as the radio sees it */
#define BHI_LENGTH              0x10u
#define BHI_SEQUENCE            0x18u   /* writing this starts the transfer */

#define BHI_COMPLETE            (2u << 30)
#define BHI_FAILED              (3u << 30)
#define SEGMENT_SIZE            0x00080000u     /* 512 KiB: what one transfer may carry */
#define IMAGE_LIMIT             0x01000000u     /* 16 MiB: the most the radio's window is meant for */

/* In the BHI block. */
#define BHI_VERSION             0x04u   /* must read 1 */
#define BHI_EE                  0x28u   /* 0 PBL, 1 SBL, 2 AMSS, 4 mission mode */
#define BHI_ERROR               0x30u
#define BHI_ERROR_DEBUG0        0x34u
#define BHI_ERROR_DEBUG1        0x38u
#define BHI_ERROR_DEBUG2        0x3cu

/*
 * Where the copy engines live in the same BAR.  From ath11k's qca6390_regs (hw.c:2366) and
 * ath11k_hal_srng_create_config() (hal.c:1278): CE0's source block starts at 0xa00000, CE0's
 * destination block at 0xa01000, and the next engine is 0x2000 further on.  The rings sit at the
 * start of each block and the host pointer 0x400 into it.
 */
#define CE_SRC_BASE             0x00a00000u
#define CE_DST_BASE             0x00a01000u
#define CE_STRIDE               0x00002000u
#define CE_STATUS_BASE          0x00000058u
#define CE_STATUS_HP            0x00000408u
#define CE_DEST_CTRL            0x000000b0u
/* ath11k pcic.h: the endpoint's BAR is 1 MiB, of which the second half is a window. */
#define WINDOW_REG              0x0000310cu
#define WINDOW_ENABLE           0x40000000u
#define WINDOW_VALUE_SHIFT      19u
#define WINDOW_VALUE_MASK       0x3fu
#define WINDOW_START            0x00080000u
#define WINDOW_RANGE_MASK       0x0007ffffu

/*
 * ath11k's host_ce_config_qca6390 (ce.c:118) and ath11k_hal_srng_create_config() (hal.c:1278).
 * Nine engines, each with a source ring (host writes, chip reads), a destination ring (host
 * fills with buffers the chip writes into) and a destination-status ring (chip writes, host
 * reads).  A ring's registers are a 0x2000 bytes block: base/size/id/misc at the start, the
 * head or tail pointer 0x400 into it, and a second group (R2) that holds the empty ring's
 * pointers.  The descriptor sizes are ath11k's: 4 dwords for a source descriptor, 2 for a
 * destination one and 4 for a destination-status one.
 */
#define CE_ENGINES              9u
#define CE_HOST_SRC_ENTRIES     0u
#define CE_HOST_SRC_MAX         1u
#define CE_HOST_DST_ENTRIES     2u
#define CE_RING_TYPE_SRC        0u
#define CE_RING_TYPE_DST        1u
#define CE_RING_TYPE_STATUS     2u
#define CE_RING_ID_SRC0         32u
#define CE_RING_ID_DST0         56u
#define CE_RING_ID_STATUS0      80u
#define CE_RING_ID_MAX          128u
#define CE_SRC_ENTRY_SIZE       4u    /* sizeof(hal_ce_srng_src_desc) >> 2 */
#define CE_DST_ENTRY_SIZE       2u    /* sizeof(hal_ce_srng_dest_desc) >> 2 */
#define CE_STATUS_ENTRY_SIZE    4u    /* sizeof(hal_ce_srng_dst_status_desc) >> 2 */
#define CE_DESC_ALIGN           8u
#define SRNG_R0_BASE_LSB        0x00u
#define SRNG_R0_BASE_MSB        0x04u
#define SRNG_R0_RING_ID         0x08u
#define SRNG_R0_MISC            0x10u
#define SRNG_R0_HP_ADDR_LSB     0x14u
#define SRNG_R0_HP_ADDR_MSB     0x18u
#define SRNG_R0_TP_ADDR_LSB     0x1cu
#define SRNG_R0_TP_ADDR_MSB     0x20u
#define SRNG_R0_PRDR_INT_SETUP  0x24u
#define SRNG_R0_CONSR_INT_IX0   0x30u
#define SRNG_R0_CONSR_INT_IX1   0x34u
#define SRNG_REG_HP             0x00000400u
#define SRNG_REG_TP_DELTA       0x04u
#define SRNG_MISC_SRNG_ENABLE   (1u << 6)
#define SRNG_MISC_LOOPCNT_DIS   (1u << 1)

/* The radio's vector 0 is SPI 768, and the GIC's register counts INTIDs from 32 (spi_base). */
#define RADIO_FIRST_SPI         768u
#define GIC_SPI_BASE            32u
#define RADIO_STREAM            0x1c01u

#define SMMU_SCR0               0x000u  /* bit 0: the whole SMMU off; bit 3: 16-bit streams; bit 10: an
                                         * unmatched stream faults rather than passing untranslated */
#define SMMU_IDR0               0x020u  /* [7:0] stream match registers */
#define SMMU_IDR1               0x024u  /* [7:0] translation contexts, bit 31: 64K pages */
#define SMMU_SMR(n)             (0x800u + 4u * (n))     /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00u + 4u * (n))     /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */

/*
 * Physical, not the mapped pointers: everything below hands these to pwr()/prd(), and those
 * go through pcie_reg(), which translates a physical address to its window.  (As the mapped
 * pointers they produced virtual addresses, and every access in step 2 was refused as
 * "outside every window this driver mapped" -- 0x9c157004 and its like.)  The survey reads
 * rd(gcc + offset) directly, which is why step 1 never showed it.
 */
#define GCC                     GCC_PHYS
#define GPIO                    TLMM_NORTH_PHYS
#define PARF                    PARF_PHYS
#define PHY                     PHY_PHYS
#define DBI                     DBI_PHYS
#define ATU                     ATU_PHYS
#define CONF                    CONF_PHYS
#define BAR                     BAR_PHYS

#define BIT(n)                  (1u << (n))

/* ---- the QCA6390's PCI identity: vendor 0x17cb, device 0x1101 --------------------------------- */

#define QCA6390_ID              0x110117cbu     /* device << 16 | vendor, as a 32-bit little-endian read */
#define QCA6390_MHI_VERSION     0x01000000u     /* what MHIVER reads once the firmware's PBL is up */

/* ---- RPMh: the sequencer, the command database, the register a rail answers at ---------------- */

#define RSC_PHYS                0x18220000u     /* qcom,drv-id = <2>: the third of rsc's windows */
#define RSC_SIZE                0x00010000u
#define CMD_DB_PHYS             0x80860000u     /* dt: reserved-memory cmd_db@80860000 */
#define CMD_DB_SIZE             0x00020000u

#define TCS(reg)                (rsc + 0xd00 + (reg))
#define TCS_BLOCK               0x100u          /* one TCS's registers, from the block's base */
#define TCS_N(n, reg)           (rsc + 0xd00 + (n) * TCS_BLOCK + (reg))
#define TCS_STATUS_IDLE         (1u << 0)
#define TCS_STATUS_AMC_EN       (1u << 16)
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_WAIT_FOR_CMPL   0x20
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define TCS_CMD_STATUS          0x3c
#define TCS_CMD_COMPLETED       (1u << 16)
#define RSC_CMD_RESP_DATA       0x40

#define MSGID_READ_4_BYTES      0x108           /* LEN(8) | RESP_REQ(BIT8), no WRITE(BIT16) */
#define MSGID_WRITE_4_BYTES     0x10108         /* LEN(8) | RESP_REQ(BIT8) | WRITE(BIT16):
                                                 * RESP_REQ because every command this
                                                 * driver sends sets WAIT_FOR_CMPL, and the
                                                 * sequencer will not scan a write that does
                                                 * not ask for a response (measured: 0x10008
                                                 * never completes, 0x10108 does) */

#define VRM_VOLTAGE             0               /* a regulator's registers, from its RPMh address */
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

/* ---- the PCIe0 PHY, this phone's own qcom,phy-sequence (99 triples, offset, value, delay us) --- */

static const struct { uint32_t offset, value, delay_us; } pcie_phy[] = {
	{ 0x840, 0x03, 0 }, { 0x094, 0x08, 0 }, { 0x154, 0x34, 0 }, { 0x16c, 0x08, 0 },
	{ 0x058, 0x0f, 0 }, { 0x0a4, 0x42, 0 }, { 0x110, 0x24, 0 }, { 0x11c, 0x03, 0 },
	{ 0x118, 0xb4, 0 }, { 0x10c, 0x02, 0 }, { 0x1bc, 0x11, 0 }, { 0x0bc, 0x82, 0 },
	{ 0x0d4, 0x03, 0 }, { 0x0d0, 0x55, 0 }, { 0x0cc, 0x55, 0 }, { 0x0b0, 0x1a, 0 },
	{ 0x0ac, 0x0a, 0 }, { 0x0c4, 0x68, 0 }, { 0x0e0, 0x02, 0 }, { 0x0dc, 0xaa, 0 },
	{ 0x0d8, 0xab, 0 }, { 0x0b8, 0x34, 0 }, { 0x0b4, 0x14, 0 }, { 0x158, 0x01, 0 },
	{ 0x074, 0x06, 0 }, { 0x07c, 0x16, 0 }, { 0x084, 0x36, 0 }, { 0x078, 0x06, 0 },
	{ 0x080, 0x16, 0 }, { 0x088, 0x36, 0 }, { 0x1b0, 0x1e, 0 }, { 0x1ac, 0xca, 0 },
	{ 0x1b8, 0x18, 0 }, { 0x1b4, 0xa2, 0 }, { 0x050, 0x07, 0 }, { 0x010, 0x01, 0 },
	{ 0x01c, 0x31, 0 }, { 0x020, 0x01, 0 }, { 0x024, 0xde, 0 }, { 0x028, 0x07, 0 },
	{ 0x030, 0x4c, 0 }, { 0x034, 0x06, 0 }, { 0x29c, 0x12, 0 }, { 0x284, 0x35, 0 },
	{ 0x23c, 0x11, 0 }, { 0x51c, 0x03, 0 }, { 0x518, 0x1c, 0 }, { 0x524, 0x1e, 0 },
	{ 0x4e8, 0x00, 0 }, { 0x4ec, 0x0e, 0 }, { 0x4f0, 0x4a, 0 }, { 0x4f4, 0x0f, 0 },
	{ 0x5b4, 0x04, 0 }, { 0x434, 0x7f, 0 }, { 0x444, 0x70, 0 }, { 0x510, 0x17, 0 },
	{ 0x4d4, 0x04, 0 }, { 0x4d8, 0x07, 0 }, { 0x598, 0xd4, 0 }, { 0x59c, 0x54, 0 },
	{ 0x5a0, 0xdb, 0 }, { 0x5a4, 0x3b, 0 }, { 0x5a8, 0x31, 0 }, { 0x584, 0x24, 0 },
	{ 0x588, 0xe4, 0 }, { 0x58c, 0xec, 0 }, { 0x590, 0x3b, 0 }, { 0x594, 0x36, 0 },
	{ 0x570, 0x3f, 0 }, { 0x574, 0x3f, 0 }, { 0x578, 0xff, 0 }, { 0x57c, 0x7f, 0 },
	{ 0x580, 0x14, 0 }, { 0x4fc, 0x00, 0 }, { 0x4f8, 0xc0, 0 }, { 0x460, 0x30, 0 },
	{ 0x464, 0x00, 0 }, { 0x5bc, 0x0c, 0 }, { 0x4dc, 0x1b, 0 }, { 0x408, 0x0c, 0 },
	{ 0x414, 0x03, 0 }, { 0x5b8, 0x30, 0 }, { 0x9a4, 0x01, 0 }, { 0xc90, 0x00, 0 },
	{ 0xc40, 0x01, 0 }, { 0xc48, 0x01, 0 }, { 0xc50, 0x00, 0 }, { 0xcb4, 0x33, 0 },
	{ 0xcbc, 0x00, 0 }, { 0xce0, 0x58, 0 }, { 0xca4, 0x0f, 0 }, { 0x048, 0x90, 0 },
	{ 0xc1c, 0xc1, 0 }, { 0x988, 0x77, 0 }, { 0x998, 0x0b, 0 }, { 0x8dc, 0x0d, 0 },
	{ 0x9ec, 0x12, 0 }, { 0x800, 0x00, 0 }, { 0x844, 0x03, 0 },
};

/*
 * The rails this phone's device tree gives the radio and the PCIe PHY, and the
 * voltage to ask for. The numbers are the ones apollo votes for the same chip
 * on the same SoC family; each is inside the range the device tree declares for
 * that rail here, and what a rail already says is read before anything is
 * written. "smpa6" carries both vdd-wlan-aon and vdd-wlan-dig on this phone.
 */
static const struct {
	const char *resource;
	unsigned mv;
	bool ldo;                       /* a PMIC5 LDO wants its mode set before it is enabled */
} pcie_rails[] = {
	{ "smpa4", 1800, false },       /* PM8150 S4: WLAN IO */
	{ "smpa6",  950, false },       /* PM8150 S6: WLAN AON, and the digital rail */
	{ "smpa5", 1900, false },       /* PM8150 S5: WLAN RFA1 */
	{ "smpc8", 1350, false },       /* PM8150A S8: WLAN RFA2 */
	{ "ldoa9", 1200, true },        /* PM8150 L9: PCIe vreg-1p8 */
	{ "ldoa5",  880, true },        /* PM8150 L5: PCIe vreg-0p9 */
};

/* ---- the platform the sequence runs on, so that the same code can serve another phone ---------- */

struct pcie_io {
	void *context;
	uint32_t (*read32)(void *, uint32_t physical_address);
	void (*write32)(void *, uint32_t physical_address, uint32_t value);
	int (*power_on)(void *);        /* the rails; no endpoint DMA */
	void (*delay_us)(void *, unsigned);
	void (*report)(void *, const char *, uint32_t);
};

struct pcie {
	const struct pcie_io *io;
	bool failed, enumerated;
	uint32_t vendor_device;
};

/* ---- mapped once, never unmapped: the same choice hakios.wifi.c makes for its regions ---------- */

static struct {
	uint32_t pa, size;
	vm_offset_t va;
} pcie_regions[] = {
	{ GCC_PHYS, GCC_SIZE, 0 },
	{ TLMM_NORTH_PHYS, TLMM_NORTH_SIZE, 0 },
	{ PARF_PHYS, PARF_SIZE, 0 },
	{ PHY_PHYS, PHY_SIZE, 0 },
	{ DBI_PHYS, DBI_SIZE, 0 },
	{ ATU_PHYS, ATU_SIZE, 0 },
	{ CONF_PHYS, CONF_SIZE, 0 },
	{ BAR_PHYS, BAR_SIZE, 0 },
	{ MSI_PHYS, MSI_SIZE, 0 },
	{ SMMU_PHYS, SMMU_SIZE, 0 },
};

static vm_offset_t gcc, tlmm, parf, phy, dbi, atu, conf, bar, msi, smmu;
static vm_offset_t rsc, cmddb;

static uint32_t pcie_steps, pcie_start_delay = 3, pcie_upto = 6;
static uint32_t pcie_vote = 1, pcie_railmask = 0x3f, pcie_writemode = 0, pcie_writetest = 0;
static uint32_t pcie_revote = 0;                /* vote a rail even when its aggregate reads enabled */
static uint32_t pcie_swreset = 0;               /* QCA6390's software reset of MHI, on the way up and down */
static uint32_t pcie_linkretry = 1;             /* train the link again when its status is not the known good one */
static uint32_t pcie_rpmhmode = 0;
static uint32_t pcie_smmu_mode = 1;     /* 0 = look only, 1 = build the context for 0x1c01 */
static uint32_t pcie_image_wait = 10;   /* seconds to wait for the small image to become a device */
static uint32_t pcie_sbl_send = 1;      /* pcie_sbl=0: copy the SBL into DMA memory, do not hand it over */
static uint32_t pcie_mhi_mode = 1;      /* pcie_mhi=0: the READY handshake is not made */
static uint32_t pcie_amss_mode = 1;     /* pcie_amss=0: the AMSS is not sent through BHIe */
static uint32_t pcie_amss_wait = 5000;  /* ms to wait for the execution environment to leave the SBL */
static uint32_t pcie_nop_mode = 1;      /* pcie_nop=0: the command ring is not exercised */
static uint32_t pcie_ipcr_mode = 1;     /* pcie_ipcr=0: the IPCR channels are not started */
static uint32_t pcie_wlfw_mode = 1;     /* pcie_wlfw=0: the QMI handshake is skipped */
static uint32_t pcie_mem_phys;          /* pcie_mem_phys=1: the memory answer carries kvtophys() */
static uint32_t pcie_fixed_iova;        /* pcie_fixed_iova=1: the three addresses stock CNSS uses */
static uint32_t pcie_stage20_mode = 1;  /* pcie_stage20=0: stop once the memory is ready */
static uint32_t pcie_stage21_mode = 1;  /* pcie_stage21=0: stop once M3 is known */
static uint32_t pcie_mode = 7;          /* pcie_mode=N: the firmware mode to ask for, 7 is cold boot */
static uint32_t pcie_ce_survey = 1;     /* pcie_cesurvey=0: the copy-engine registers are not read */
static char pcie_bdf_name[64];   /* pcie_bdf=bd_j11x.elf: which board file */
static uint32_t pcie_bdf_send = 1;      /* pcie_bdf_send=0: the board file is not sent */
static uint32_t pcie_bdf_id = 0xffu;    /* pcie_bdf_id=N: the file id sent with each piece */

static uint32_t pcie_ce_init = 1;       /* pcie_ceinit=0: the copy engines are left alone */
static uint32_t pcie_cfg_send = 1;      /* pcie_cfg_send=0: the copy-engine configuration is skipped */
static uint32_t pcie_ini_send = 1;      /* pcie_ini_send=0: the logging switch is skipped */
static uint32_t pcie_cfg_parts = 7;     /* pcie_cfg_parts=N: which parts of the configuration go */
static uint32_t pcie_cfg_after;         /* pcie_cfg_after=1: the configuration follows the calibration */
static uint32_t pcie_mode_after = 0xffu;  /* pcie_mode_after=N: a second mode request once the rings are up */
static uint32_t pcie_ce_rx = 1u;          /* pcie_ce_rx=1: give the firmware receive buffers */
static uint32_t pcie_htc = 1u;              /* pcie_htc=1: say hello to HTC on the copy engines */
static uint32_t pcie_second;                /* pcie_second=1: reset after the calibration, then mode 0 */
static uint32_t pcie_cfg_during;        /* pcie_cfg_during=1: fill the rings while the chip calibrates */
static uint32_t pcie_bdf_piece = 6144u;   /* pcie_bdf_piece=N: bytes per piece, 6144 is the most */
static uint32_t pcie_ipcr_wait = 3000;  /* pcie_ipcr_wait=N: ms to wait for a transfer */
static uint32_t pcie_sbl_from_file;     /* pcie_fwfile=1: take the SBL from fw/amss20.bin, not the blob */
static uint32_t pcie_panic;             /* pcie_panic=N: panic at step N of stage 14, to get the log back */
static uint32_t pcie_fail_panic;        /* pcie_fail_panic=1: any stage failure panics, so the log comes back */
static uint32_t pcie_sbl_wait = 3000;   /* pcie_sbl_wait=ms: how long the SBL is given to leave the PBL */

/*
 * What stage 11 built.  Kept so a later stage can map more of the firmware's memory into the same
 * context without repeating the match-register search.
 */
static struct {
	bool built;
	uint32_t match, context;
	uint32_t page, pages;
	vm_offset_t tables;             /* RADIO_TABLES of them, the root first */
	uint32_t table_count;
	uint64_t next;                  /* the first radio address not handed out */
	bool reused;                    /* the routing was already this code's own */
} radio;
static int pcie_id;                     /* the ID as it read, for kern.hakios.pcie_id */

static int
say(const char *format, ...) __printflike(1, 2);

static void umi_input(const uint8_t *packet, uint32_t length);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	printf("pcie: %s\n", line);
	return 0;
}

/* ---- raw register access ---------------------------------------------------------------------- */

static uint32_t
rd(vm_offset_t address)
{
	uint32_t value = *(volatile uint32_t *)address;

	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

static void
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)address = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static bool
pcie_map_regions(void)
{
	for (unsigned i = 0; i < sizeof(pcie_regions) / sizeof(pcie_regions[0]); i++) {
		if (pcie_regions[i].va != 0) {
			continue;
		}
		/*
		 * Unmappable: into kernel_map rather than the 8 MiB io_submap, so that this driver cannot
		 * run the submap out for the others (hakios.wifi.c maps the same way and for the same reason).
		 *
		 * Announced before it happens: a map that takes the machine down leaves the line before it
		 * as the last thing in the live log, which is how the dying step gets named.
		 */
		say("map %u: %#x (+%#x)", i, pcie_regions[i].pa, pcie_regions[i].size);
		pcie_regions[i].va = ml_io_map_unmappable(pcie_regions[i].pa, pcie_regions[i].size, VM_WIMG_IO);
		if (pcie_regions[i].va == 0) {
			say("cannot map %#x (+%#x)", pcie_regions[i].pa, pcie_regions[i].size);
			return false;
		}
	}
	gcc = pcie_regions[0].va;
	tlmm = pcie_regions[1].va;
	parf = pcie_regions[2].va;
	phy = pcie_regions[3].va;
	dbi = pcie_regions[4].va;
	atu = pcie_regions[5].va;
	conf = pcie_regions[6].va;
	bar = pcie_regions[7].va;
	msi = pcie_regions[8].va;
	smmu = pcie_regions[9].va;
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	cmddb = ml_io_map(CMD_DB_PHYS, CMD_DB_SIZE);
	if (rsc == 0 || cmddb == 0) {
		say("cannot map RPMh (%#x) or its command database (%#x)", RSC_PHYS, CMD_DB_PHYS);
		return false;
	}
	return true;
}

/* ---- RPMh: one command at a time, the sequence hakios.psyche_touch.c runs on this phone -------- */

/* The RPMh address of a resource by its name in the database the boot loader left; 0 = none. */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmddb;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		say("no command database (magic %02x %02x %02x %02x)", db->magic[0], db->magic[1], db->magic[2],
		    db->magic[3]);
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

/*
 * Fire one command in the sequencer and wait for the hardware's own acknowledgement. The order is
 * hakios.psyche_touch.c's, which is the one that works on this phone: TRIGGER cleared, then ENABLE
 * set alone, then ENABLE|TRIGGER -- writing TRIGGER alone clears the AMC enable bit that was already
 * there and the command sits in its slot unscanned (CMD_STATUS = ISSUED|stuck for ever).
 */
static bool
rpmh_send_slot(unsigned slot, uint32_t msgid, uint32_t addr, uint32_t data, uint32_t *response)
{
	uint32_t st;
	bool completed;

	lck_mtx_lock(&hakios_rsc_lock);
	wr(TCS_N(slot, TCS_CMD_ENABLE), 0);
	wr(TCS_N(slot, TCS_CMD_WAIT_FOR_CMPL), 0);
	wr(TCS_N(slot, TCS_CMD_MSGID(0)), msgid);
	wr(TCS_N(slot, TCS_CMD_ADDR(0)), addr);
	wr(TCS_N(slot, TCS_CMD_DATA(0)), data);
	wr(TCS_N(slot, TCS_CMD_ENABLE), 1u);
	wr(TCS_N(slot, TCS_CMD_WAIT_FOR_CMPL), 1u);
	wr(TCS_N(slot, TCS_CONTROL), 0);
	wr(TCS_N(slot, TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS_N(slot, TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	completed = false;
	for (int i = 0; i < 2000; i++) {
		st = rd(TCS_N(slot, TCS_CMD_STATUS));
		if (st & TCS_CMD_COMPLETED) {
			completed = true;
			break;
		}
		sleep_us(10);
	}
	if (response != NULL && completed) {
		*response = rd(TCS_N(slot, RSC_CMD_RESP_DATA));
	}
	wr(TCS_N(slot, TCS_CONTROL), 0);
	wr(TCS_N(slot, TCS_CMD_ENABLE), 0);
	wr(TCS_N(slot, TCS_CMD_WAIT_FOR_CMPL), 0);
	wr(TCS_N(slot, TCS_IRQ_CLEAR), rd(TCS_N(slot, TCS_IRQ_STATUS)));
	lck_mtx_unlock(&hakios_rsc_lock);

	if (!completed) {
		say("the sequencer's slot %u did not complete: msgid=%#x addr=%#x data=%#x status=%#x control=%#x",
		    slot, msgid, addr, data, rd(TCS_N(slot, TCS_CMD_STATUS)),
		    rd(TCS_N(slot, TCS_CONTROL)));
	}
	return completed;
}

static bool
rpmh_send(uint32_t msgid, uint32_t addr, uint32_t data, uint32_t *response)
{
	return rpmh_send_slot(0, msgid, addr, data, response);
}

/* ---- the same command, sent the way apollo's sweet_rpmh.c sends it ---------------------------- */

#define TCS_BLOCK               0x100u          /* one TCS's registers, from the block's base */
#define TCS_SLOTS               3u              /* the ones this driver will consider */

/* TCS(n) for a slot other than 0: the registers repeat every 0x100. */
#define TCS_N(n, reg)           (rsc + 0xd00 + (n) * TCS_BLOCK + (reg))

#define TCS_STATUS_IDLE         (1u << 0)
#define TCS_STATUS_AMC_EN       (1u << 16)

static void
tcs_dump(const char *when)
{
	for (unsigned n = 0; n < TCS_SLOTS; n++) {
		say("  %s: TCS%u status %#x control %#x irq %#x cmd_status %#x", when, n,
		    rd(TCS_N(n, TCS_STATUS)), rd(TCS_N(n, TCS_CONTROL)), rd(TCS_N(n, TCS_IRQ_STATUS)),
		    rd(TCS_N(n, TCS_CMD_STATUS)));
	}
}

/*
 * apollo's rh_send: pick a slot, wait for it to be idle, keep the CONTROL the boot chain left,
 * write the commands as a mask, and put CONTROL back the way it was found.
 */
static bool
rpmh_send_apollo(unsigned slot, uint32_t msgid, uint32_t addr, uint32_t data, uint32_t *response)
{
	uint32_t inherited, control;
	bool completed = false;

	/* Wait for the slot to be idle, AMC bit included: an armed but idle payload still counts. */
	for (int i = 0; i < 200; i++) {
		if ((rd(TCS_N(slot, TCS_CONTROL)) & ~TCS_STATUS_AMC_EN) == 0) {
			break;
		}
		sleep_us(100);
	}
	inherited = rd(TCS_N(slot, TCS_CONTROL));
	say("slot %u: inherited control %#x, status %#x", slot, inherited, rd(TCS_N(slot, TCS_STATUS)));

	say("slot %u: writing the command", slot);
	wr(TCS_N(slot, TCS_CMD_MSGID(0)), msgid);
	wr(TCS_N(slot, TCS_CMD_ADDR(0)), addr);
	wr(TCS_N(slot, TCS_CMD_DATA(0)), data);
	/* A mask of one command, not the number one: the register is a bit per command. */
	wr(TCS_N(slot, TCS_CMD_ENABLE), 1u);
	wr(TCS_N(slot, TCS_CMD_WAIT_FOR_CMPL), 1u);

	say("slot %u: triggering", slot);
	wr(TCS_N(slot, TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS_N(slot, TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	for (int i = 0; i < 2000; i++) {
		uint32_t st = rd(TCS_N(slot, TCS_CMD_STATUS));

		if (st & TCS_CMD_COMPLETED) {
			completed = true;
			break;
		}
		sleep_us(10);
	}
	if (response != NULL && completed) {
		*response = rd(TCS_N(slot, RSC_CMD_RESP_DATA));
	}

	/* Back the way it was found, AMC bit and all. */
	control = rd(TCS_N(slot, TCS_CONTROL));
	wr(TCS_N(slot, TCS_CONTROL), control | TCS_STATUS_AMC_EN);
	wr(TCS_N(slot, TCS_CONTROL), inherited);
	wr(TCS_N(slot, TCS_CMD_ENABLE), 0);
	wr(TCS_N(slot, TCS_CMD_WAIT_FOR_CMPL), 0);
	wr(TCS_N(slot, TCS_IRQ_CLEAR), rd(TCS_N(slot, TCS_IRQ_STATUS)));

	if (!completed) {
		say("apollo-style slot %u did not complete: status %#x control %#x cmd_status %#x", slot,
		    rd(TCS_N(slot, TCS_STATUS)), rd(TCS_N(slot, TCS_CONTROL)),
		    rd(TCS_N(slot, TCS_CMD_STATUS)));
	}
	return completed;
}

static bool
rpmh_write_mode(uint32_t addr, uint32_t value)
{
	unsigned slot = 0;

	if (pcie_rpmhmode == 2) {
		for (slot = 0; slot < TCS_SLOTS; slot++) {
			if (rd(TCS_N(slot, TCS_STATUS)) & TCS_STATUS_IDLE) {
				break;
			}
		}
		if (slot == TCS_SLOTS) {
			say("no idle TCS among %u", TCS_SLOTS);
			return false;
		}
	}
	return rpmh_send_apollo(slot, MSGID_WRITE_4_BYTES, addr, value, NULL);
}

static bool
rpmh_read(uint32_t addr, uint32_t *value)
{
	return rpmh_send(MSGID_READ_4_BYTES, addr, 0, value);
}

static bool
rpmh_write(uint32_t addr, uint32_t value)
{
	return rpmh_send(MSGID_WRITE_4_BYTES, addr, value, NULL);
}

/*
 * Vote a rail up, but only a rail that is down: this phone hangs on an unconditional RPMh vote
 * (hakios.psyche_touch.c's power_on says so and had to be rewritten to read first), so the enable
 * register is read, and a rail already on is left alone and said so.
 */
static int
rail_on(const char *resource, unsigned mv, bool ldo)
{
	uint32_t address = rpmh_address(resource), enabled = 0, now = 0, mode = 0;

	if (address == 0) {
		say("RPMh has no %s: the rail is not voted", resource);
		return ENOENT;
	}
	if (!rpmh_read(address + VRM_ENABLE, &enabled)) {
		say("%s at %#x: cannot read its enable", resource, address);
		return EIO;
	}
	if (!rpmh_read(address + VRM_VOLTAGE, &now)) {
		say("%s at %#x: cannot read its voltage", resource, address);
		return EIO;
	}
	(void)rpmh_read(address + VRM_MODE, &mode);
	if (!pcie_vote) {
		/*
		 * Look, do not touch.  A rail that an aggregate read calls off can still be feeding
		 * something the boot chain brought up in another state set, and writing its voltage
		 * then browns the SoC out -- which is what pcie_upto=1 looked like.
		 */
		say("%s at %#x: enable %#x voltage %u mV mode %#x (not voted: pcie_vote=0)", resource, address,
		    enabled, now, mode);
		return 0;
	}
	if (enabled && !pcie_revote) {
		say("%s at %#x: already on (%#x, %u mV, mode %#x), left alone", resource, address, enabled, now,
		    mode);
		return 0;
	}
	if (enabled) {
		/*
		 * The aggregate says somebody asked for this rail.  RPMh takes the maximum of the
		 * votes, so putting ours in as well can only raise it to what the radio needs --
		 * and the number printed below is a status word, not a voltage, which is why the
		 * read is not to be trusted (docs 51.16).
		 */
		say("%s at %#x: the aggregate reads %#x (%u, mode %#x); voting %u mV anyway (pcie_revote=1)",
		    resource, address, enabled, now, mode, mv);
	}
	/*
	 * The two halves of a vote, separately selectable: a rail that is left off cannot brown
	 * anything out, so whichever half takes the machine down names the mechanism.
	 */
	if (pcie_writemode != 2) {
		/*
		 * Mode 3 writes back what the register already holds: a write that changes nothing.
		 * If that alone takes the machine down, RPMh's write path is what cannot be used this
		 * early, and no amount of rail reasoning will help; if it lives, the rail change is
		 * the thing to look at.
		 */
		uint32_t want = (pcie_writemode == 3) ? now : mv;

		say("%s at %#x: writing the voltage, %u mV%s", resource, address, want,
		    pcie_writemode == 3 ? " (the value it already holds: a write that changes nothing)" : "");
		if (!rpmh_write(address + VRM_VOLTAGE, want)) {
			return EIO;
		}
		if (ldo) {
			say("%s at %#x: writing the mode, LDO HPM", resource, address);
			if (!rpmh_write(address + VRM_MODE, VRM_MODE_LDO_HPM)) {
				return EIO;
			}
		}
	}
	if (pcie_writemode == 1 || pcie_writemode == 3) {
		say("%s at %#x: written, the enable left alone (pcie_writemode=%u)", resource, address,
		    pcie_writemode);
		return 0;
	}
	say("%s at %#x: writing the enable", resource, address);
	if (!rpmh_write(address + VRM_ENABLE, 1)) {
		return EIO;
	}
	say("%s at %#x: %u mV, enabled", resource, address, mv);
	return 0;
}

static int
pcie_power(void *context)
{
	(void)context;
	for (unsigned i = 0; i < sizeof(pcie_rails) / sizeof(pcie_rails[0]); i++) {
		int error;

		if ((pcie_railmask & (1u << i)) == 0) {
			say("%s: left alone (not in pcie_railmask %#x)", pcie_rails[i].resource, pcie_railmask);
			continue;
		}
		error = rail_on(pcie_rails[i].resource, pcie_rails[i].mv, pcie_rails[i].ldo);
		if (error != 0) {
			return error;
		}
	}
	return 0;
}

/* ---- the io the sequence sees ----------------------------------------------------------------- */

static vm_offset_t
pcie_reg(uint32_t pa)
{
	if (pa & 3) {
		return 0;
	}
	for (unsigned i = 0; i < sizeof(pcie_regions) / sizeof(pcie_regions[0]); i++) {
		uint32_t offset = pa - pcie_regions[i].pa;

		if (pcie_regions[i].va != 0 && pa >= pcie_regions[i].pa && offset <= pcie_regions[i].size - 4) {
			return pcie_regions[i].va + offset;
		}
	}
	say("a read of %#x is outside every window this driver mapped", pa);
	return 0;
}

static uint32_t
pcie_read(void *context, uint32_t pa)
{
	vm_offset_t va = pcie_reg(pa);

	(void)context;
	return va != 0 ? rd(va) : UINT32_MAX;
}

static void
pcie_write(void *context, uint32_t pa, uint32_t value)
{
	vm_offset_t va = pcie_reg(pa);

	(void)context;
	if (va != 0) {
		wr(va, value);
	}
}

static void
pcie_delay_us(void *context, unsigned us)
{
	(void)context;
	sleep_us(us);
}

static void
pcie_report(void *context, const char *name, uint32_t value)
{
	(void)context;
	say("%s = %#x", name, value);
}

static const struct pcie_io pcie_platform = {
	.context = NULL,
	.read32 = pcie_read,
	.write32 = pcie_write,
	.power_on = pcie_power,
	.delay_us = pcie_delay_us,
	.report = pcie_report,
};

/* ---- the sequence ----------------------------------------------------------------------------- */

#define TRY(expr) do { error = (expr); if (error != 0) goto fail; } while (0)

static uint32_t
prd(struct pcie *p, uint32_t address)
{
	return p->io->read32(p->io->context, address);
}

static void
pwr(struct pcie *p, uint32_t address, uint32_t value)
{
	p->io->write32(p->io->context, address, value);
}

static void
pause_us(struct pcie *p, unsigned us)
{
	p->io->delay_us(p->io->context, us);
}

static void
report(struct pcie *p, const char *name, uint32_t value)
{
	if (p->io->report != NULL) {
		p->io->report(p->io->context, name, value);
	}
}

static int
update(struct pcie *p, uint32_t address, uint32_t clear, uint32_t set)
{
	uint32_t value = prd(p, address);

	if (value == UINT32_MAX) {
		return EIO;
	}
	pwr(p, address, (value & ~clear) | set);
	return 0;
}

static int
wait_bits(struct pcie *p, uint32_t address, uint32_t mask, uint32_t wanted, unsigned timeout_us)
{
	uint32_t value = 0;

	for (unsigned us = 0; us < timeout_us; us += 10) {
		value = prd(p, address);
		if (value == UINT32_MAX) {
			return EIO;
		}
		if ((value & mask) == wanted) {
			return 0;
		}
		pause_us(p, 10);
	}
	report(p, "poll timeout address", address);
	report(p, "poll timeout value", value);
	return ETIMEDOUT;
}

static int
gpio_output(struct pcie *p, unsigned pin, bool high, uint32_t config)
{
	/* The level first, so that the pin never drives the wrong one while the mux changes. */
	int error = update(p, GPIO + pin * 0x1000 + 4, BIT(1), high ? BIT(1) : 0);

	if (error != 0) {
		return error;
	}
	return update(p, GPIO + pin * 0x1000, 0x3ff, config | BIT(9));
}

static int
source_xo(struct pcie *p, uint32_t cmd)
{
	/* 19.2 MHz XO, divide by 1: the reference rate the PHY's Gen1/Gen2 link uses. */
	int error = update(p, cmd + 4, 0x371f, 1);

	if (error != 0) {
		return error;
	}
	error = update(p, cmd, 0, 1);
	return error != 0 ? error : wait_bits(p, cmd, 1, 0, 10000);
}

static int
branch(struct pcie *p, uint32_t offset)
{
	int error = update(p, GCC + offset, 0, 1);

	return error != 0 ? error : wait_bits(p, GCC + offset, BIT(31), 0, 10000);
}

/*
 * The clocks and resets of PCIe0, in GCC. The GDSC's status is checked before the controller or the
 * PHY is touched at all: with it off every access to the rest is a bus error, not a wrong value.
 */
static int
pcie_clocks(struct pcie *p)
{
	int error;

	TRY(update(p, GCC + 0x6b004, 7, 0));
	pause_us(p, 1);
	TRY(wait_bits(p, GCC + 0x6b004, BIT(31), BIT(31), 10000));
	TRY(source_xo(p, GCC + 0x6b038));
	TRY(source_xo(p, GCC + 0x6f014));
	TRY(branch(p, 0x8c004));        /* the QCA6390's reference clock / LDO */
	TRY(branch(p, 0x6f02c));
	TRY(branch(p, 0x9000c));
	TRY(branch(p, 0x8d058));
	/* The AXI and PIPE clocks must not keep memory or peripheral state across the link's training. */
	TRY(update(p, GCC + 0x6b01c, 0x6000, 0));
	/*
	 * The diagnostic AHB and slave clocks are forced on. In hardware clock gating the halt bit can
	 * assert merely because the bus is idle, so it is not a readiness test.
	 */
	TRY(update(p, GCC + 0x6b014, 0x6002, 0));
	TRY(update(p, GCC + 0x6b024, BIT(1), 0));
	TRY(update(p, GCC + 0x6b02c, 0x6000, 0));
	TRY(update(p, GCC + 0x52008, 0, 0x2f)); /* every PCIe0 vote except PIPE */
	{
		static const uint32_t gates[] = { 0x6b010, 0x6b014, 0x6b01c, 0x6b024, 0x6b028 };

		for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
			TRY(wait_bits(p, GCC + gates[i], BIT(31), 0, 10000));
		}
	}
	{
		static const uint32_t resets[] = { 0x6b000, 0x6c01c };

		for (unsigned i = 0; i < sizeof(resets) / sizeof(resets[0]); i++) {
			TRY(update(p, GCC + resets[i], 0, 1));
			pause_us(p, 1000);
			TRY(update(p, GCC + resets[i], 1, 0));
		}
	}
	return 0;
fail:
	return error;
}

/* One entry of the outbound iATU: a window of the SoC's addresses that becomes a PCIe address. */
static int
outbound(struct pcie *p, unsigned index, uint32_t type, uint32_t start, uint32_t end, uint32_t target)
{
	uint32_t a = ATU + index * 0x200;

	pwr(p, a + 4, 0);
	pwr(p, a, type);
	pwr(p, a + 8, start);
	pwr(p, a + 12, 0);
	pwr(p, a + 16, end);
	pwr(p, a + 20, target);
	pwr(p, a + 24, 0);
	pwr(p, a + 4, BIT(31));
	return wait_bits(p, a + 4, BIT(31), BIT(31), 1000);
}

/*
 * Step 1: read only. Every window above is read once, and the radio's four pins with it, so that a
 * failure here is a failure to map rather than a failure of the link.
 */
static void
pcie_survey(void)
{
	static const struct { const char *name; uint32_t offset; } clocks[] = {
		{ "pcie0_bcr", 0x6b000 }, { "pcie0_gdsc", 0x6b004 },
		{ "pcie0_clock_votes", 0x52008 }, { "pcie0_slv_q2a", 0x6b010 },
		{ "pcie0_slv_axi", 0x6b014 }, { "pcie0_mstr_axi", 0x6b01c },
		{ "pcie0_cfg_ahb", 0x6b024 }, { "pcie0_aux", 0x6b028 },
		{ "pcie0_pipe", 0x6b02c }, { "pcie0_aux_cmd", 0x6b038 },
		{ "pcie0_aux_cfg", 0x6b03c }, { "pcie0_phy_bcr", 0x6c01c },
		{ "pcie0_phy_refgen", 0x6f02c }, { "pcie0_clkref", 0x8c004 },
	};
	static const struct { const char *name; uint32_t pin; } pins[] = {
		{ "wlan_en", 20 }, { "pcie0_perst", 79 }, { "pcie0_clkreq", 80 }, { "pcie0_wake", 81 },
	};

	say("step 1: the PCIe0 clocks in GCC and the radio's pins, read only");
	for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		say("  GCC %s (%#x) = %#x", clocks[i].name, GCC_PHYS + clocks[i].offset,
		    rd(gcc + clocks[i].offset));
	}
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		say("  reading GPIO%u %s", pins[i].pin, pins[i].name);
		say("  GPIO%u %s cfg=%#x io=%#x", pins[i].pin, pins[i].name,
		    rd(tlmm + pins[i].pin * 0x1000), rd(tlmm + pins[i].pin * 0x1000 + 4));
	}
	say("  the command database has smpa4=%#x smpa5=%#x smpa6=%#x smpc8=%#x ldoa5=%#x ldoa9=%#x",
	    rpmh_address("smpa4"), rpmh_address("smpa5"), rpmh_address("smpa6"), rpmh_address("smpc8"),
	    rpmh_address("ldoa5"), rpmh_address("ldoa9"));
}

/*
 * Step 2: the rails, wlan-en, the clocks, the PHY, the link, and the endpoint's identity. Bus
 * mastering stays off throughout: the endpoint's command register is written with memory decoding
 * on and bus mastering off, and read back to prove it.
 */
/* ---- stage 11: the radio's stream through a context of our own --------------------------------- */

/* Stage 9's programming function, used again by stage 12 with the real doorbell. */
static int pcie_setup_mhi_msi(struct pcie *p, uint64_t address, uint32_t data);

extern uint64_t kvtophys(vm_offset_t va);

static uint64_t *
radio_table(uint32_t n)
{
	return (uint64_t *)(radio.tables + 4096u * n);
}

static vm_offset_t
radio_table_va(uint32_t n)
{
	return radio.tables + 4096u * n;
}

/* The table an entry of a higher table leads to; made and linked in if the entry is empty. */
static uint64_t *
radio_next_table(uint64_t *entry)
{
	uint64_t *table;

	if ((*entry & 1u) != 0) {
		for (uint32_t i = 0; i < radio.table_count; i++) {
			if (kvtophys(radio_table_va(i)) == (*entry & PTE_ADDRESS)) {
				return radio_table(i);
			}
		}
		say("an entry points at a table this driver does not own");
		return NULL;
	}
	if (radio.table_count == RADIO_TABLES) {
		say("the pool of page tables is used up");
		return NULL;
	}
	table = radio_table(radio.table_count++);
	*entry = kvtophys(radio_table_va(radio.table_count - 1)) | 3;
	return table;
}

/*
 * Show the radio a run of physical memory at an address of its own, and answer with that address.
 *
 * Not the physical address itself: the radio's addresses come out of a window of their own
 * (RADIO_IOVA on), because the READY handshake publishes exactly that window to the chip as the
 * range it may DMA into, and a buffer outside it would be refused.  The two views are joined by
 * the page tables, which is the whole point of the context stage 11 built.
 */
static bool
radio_map(uint64_t physical, uint64_t size, uint64_t attributes, uint64_t *address)
{
	size = (size + 4095) & ~4095ull;
	if (radio.tables == 0) {
		if (kmem_alloc(kernel_map, &radio.tables, round_page(4096 * RADIO_TABLES),
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			return false;
		}
		radio.table_count = 1;
	}
	if (radio.next + size > RADIO_IOVA_SIZE) {
		say("the radio's address window is used up");
		return false;
	}
	*address = RADIO_IOVA + radio.next;
	radio.next += size;
	for (uint64_t offset = 0; offset < size; offset += 4096) {
		uint64_t at = *address + offset, *level2, *level3;

		level2 = radio_next_table(&radio_table(0)[at >> 30 & 0x1ff]);
		level3 = level2 == NULL ? NULL : radio_next_table(&level2[at >> 21 & 0x1ff]);
		if (level3 == NULL) {
			return false;
		}
		level3[at >> 12 & 0x1ff] = (physical + offset) | attributes;
	}
	for (vm_offset_t line = radio.tables; line < radio.tables + 4096 * RADIO_TABLES; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" : : : "memory");
	return true;
}

/* One contiguous run of memory, in the radio's view. */
struct dma {
	vm_offset_t bytes;              /* the host's cached mapping */
	uint64_t address;               /* the radio's */
	uint64_t size;
};

static bool
dma_alloc(struct dma *dma, uint64_t size, uint64_t attributes)
{
	dma->size = (size + 4095) & ~4095ull;
	dma->bytes = 0;
	dma->address = 0;
	if (kmem_alloc_contig(kernel_map, &dma->bytes, dma->size, 0, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		dma->bytes = 0;
		return false;
	}
	return radio_map(kvtophys(dma->bytes), dma->size, attributes, &dma->address);
}

/*
 * The same mapping at an address the caller names instead of the next free one.  The page
 * table walk is radio_map()'s; only the address is not chosen here.  `want` has to be page
 * aligned and inside the window the READY handshake published to the chip.
 */
static bool
radio_map_at(uint64_t physical, uint64_t size, uint64_t attributes, uint64_t want,
    uint64_t *address)
{
	uint64_t within;

	size = (size + 4095) & ~4095ull;
	if ((want & 4095ull) != 0 || want < RADIO_IOVA ||
	    want + size > RADIO_IOVA + RADIO_IOVA_SIZE) {
		say("the address %#llx is not inside the radio's window", want);
		return false;
	}
	within = want - RADIO_IOVA;
	if (radio.tables == 0) {
		if (kmem_alloc(kernel_map, &radio.tables, round_page(4096 * RADIO_TABLES),
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			return false;
		}
		radio.table_count = 1;
	}
	*address = want;
	/* Keep the bump allocator clear of what was placed by hand. */
	if (radio.next < within + size) {
		radio.next = within + size;
	}
	for (uint64_t offset = 0; offset < size; offset += 4096) {
		uint64_t at = *address + offset, *level2, *level3;

		level2 = radio_next_table(&radio_table(0)[at >> 30 & 0x1ff]);
		level3 = level2 == NULL ? NULL : radio_next_table(&level2[at >> 21 & 0x1ff]);
		if (level3 == NULL) {
			return false;
		}
		level3[at >> 12 & 0x1ff] = (physical + offset) | attributes;
	}
	for (vm_offset_t line = radio.tables; line < radio.tables + 4096 * RADIO_TABLES;
	    line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" : : : "memory");
	return true;
}

static bool
dma_alloc_at(struct dma *dma, uint64_t size, uint64_t attributes, uint64_t want)
{
	dma->size = (size + 4095) & ~4095ull;
	dma->bytes = 0;
	dma->address = 0;
	if (kmem_alloc_contig(kernel_map, &dma->bytes, dma->size, 0, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		dma->bytes = 0;
		return false;
	}
	return radio_map_at(kvtophys(dma->bytes), dma->size, attributes, want, &dma->address);
}

/* The chip reads physical memory: everything written through the cache has to reach it. */
/*
 * Throw away the CPU's copy of a region the device has written, so the next read goes to memory.
 * `dc ivac` invalidates without writing back, which is what is wanted: the CPU's copy is the stale
 * one, and putting it back over the device's work is the failure this function exists to prevent.
 */
static void
dma_invalidate(struct dma *dma)
{
	for (vm_offset_t line = dma->bytes; line < dma->bytes + dma->size; line += 64) {
		__asm__ volatile ("dc ivac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" : : : "memory");
}

static void
dma_clean(struct dma *dma)
{
	for (vm_offset_t line = dma->bytes; line < dma->bytes + dma->size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" : : : "memory");
}

/* ---- stage 14: the SBL ---- */

extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern const uint64_t hakios_amss_size;
extern const uint8_t hakios_amss_bytes[];

static uint64_t bhi_sequence = 1;

/* Poll a BHI status register until it says the transfer is done, or it is clearly not going to. */
static int
bhi_wait(struct pcie *p, uint32_t offset, uint32_t sequence, bool check_sequence)
{
	uint32_t status = 0;

	for (unsigned i = 0; i < 3000; i++) {
		status = prd(p, BAR + offset);
		if (status == UINT32_MAX || (status >> 30) == 3) {
			break;
		}
		if ((status >> 30) == 2 && (!check_sequence || (status & 0x3fffffffu) == sequence)) {
			report(p, "the firmware transfer completed, status", status);
			return 0;
		}
		pause_us(p, 1000);
	}
	report(p, "THE FIRMWARE TRANSFER DID NOT COMPLETE, status", status);
	report(p, "BHI error", prd(p, BAR + prd(p, BAR + MHI_BHI_OFFSET) + BHI_ERROR));
	return status == UINT32_MAX || (status >> 30) == 3 ? EIO : ETIMEDOUT;
}

/*
 * The small image as a device.
 *
 * hakios_image_file() asks mdevlookup() for the RAM disk, and until bsd_init has registered them
 * it answers -1 for every disk: a driver that runs at early boot sees "no such file" for a file
 * that is there.  This driver's window is set up during early boot (stage 1 must be, the rails
 * are voted through RPMh which is up then), but the firmware is only wanted at stage 14, by which
 * time the image is a device -- or will be in a moment.  So it is waited for.
 */
static const uint8_t *
image_awaited(struct pcie *p, const char *name, uint64_t *size)
{
	const uint8_t *data;
	unsigned waited;

	for (waited = 0; waited <= pcie_image_wait * 10; waited++) {
		data = hakios_image_file("fw", name, size, NULL);
		if (data != NULL) {
			if (waited != 0) {
				say("fw/%s appeared after %u ms", name, waited * 100);
			}
			return data;
		}
		if (waited == pcie_image_wait * 10) {
			break;
		}
		pause_us(p, 100000);
	}
	return NULL;
}

/* pcie_panic=N: stop at step N of stage 14 on purpose.  A kernel that neither panics nor returns
 * sends no log at all -- "xl: no answer" and an empty logs/ -- so a deliberate panic is the only way
 * to see how far a step got.  One run, one step. */
static void
pcie_stop_here(uint32_t step, const char *what)
{
	if (pcie_panic != step) {
		return;
	}
	say("pcie_panic=%u: stopping on purpose here, so the log comes back -- %s", pcie_panic, what);
	panic("pcie: stage 14 stopped on purpose at step %u (%s)", step, what);
}

static int
pcie_sbl(struct pcie *p)
{
	const uint8_t *image;
	/* hakios.sbl.c: the first SEGMENT_SIZE bytes of fw-wifi/qca6390/amss20.bin, generated */
	/* The first SEGMENT_SIZE of hakios_amss_bytes is the SBL: the same blob serves both stages. */
	uint64_t size = 0, length;
	struct dma sbl;
	uint32_t bhi, ee, sequence;
	int error;

	report(p, "stage 14: the SBL, through BHI", 0);
	pcie_stop_here(1, "stage 14 entered, nothing read yet");
	if (pcie_sbl_from_file) {
		image = image_awaited(p, "amss20.bin", &size);
		if (image == NULL) {
			say("no fw/amss20.bin in the small image after %u s: it is not in this build, or no RAM disk "
			    "has been registered", pcie_image_wait);
			return ENOENT;
		}
		say("the radio's firmware is %llu bytes in the small image", size);
	} else {
		image = hakios_amss_bytes;
		size = hakios_amss_size;
		say("the SBL is in the kernel image, %llu bytes of firmware, and no RAM disk is needed", size);
	}
	if (size < SEGMENT_SIZE) {
		say("it is smaller than the first segment the SBL is taken from");
		return EINVAL;
	}
	length = SEGMENT_SIZE;

	bhi = prd(p, BAR + MHI_BHI_OFFSET);
	if (bhi == UINT32_MAX || bhi < 0xa8u) {
		say("the BHI block is not where stage 2 found it");
		return ENODEV;
	}
	ee = prd(p, BAR + bhi + BHI_EE);
	if (ee != 0) {
		say("the chip is not in its PBL (environment %u): nothing is sent", ee);
		return EBUSY;
	}

	if (!dma_alloc(&sbl, length, PTE_PAGE)) {
		say("no %llu bytes of contiguous memory, or no room in the radio's window", length);
		return ENOMEM;
	}
	say("the SBL goes into %#llx, which the radio sees at %#llx", kvtophys(sbl.bytes), sbl.address);
	pcie_stop_here(2, "the DMA buffer is allocated");
	memcpy((void *)sbl.bytes, image, length);
	dma_clean(&sbl);
	pcie_stop_here(3, "the SBL is copied and its cache is cleaned");
	if (!pcie_sbl_send) {
		say("the SBL is copied and clean in memory, and nothing is sent: pcie_sbl=0 is a dry run");
		say("stage 14: done, the dry half");
		return 0;
	}

	/* Polling mode, and no stale completion left to be mistaken for this transfer's. */
	pwr(p, BAR + bhi + BHI_CONTROL, 0);
	pwr(p, BAR + bhi + BHI_COMPLETION, 0);
	pwr(p, BAR + bhi + BHI_ADDRESS + 4, (uint32_t)(sbl.address >> 32));
	pwr(p, BAR + bhi + BHI_ADDRESS, (uint32_t)sbl.address);
	pwr(p, BAR + bhi + BHI_LENGTH, (uint32_t)length);
	__asm__ volatile ("dmb oshst" : : : "memory");
	say("the doorbell is armed: %llu bytes at %#llx, and nothing has rung it yet", length, sbl.address);
	pcie_stop_here(4, "the address and length are written, before the sequence");

	sequence = (uint32_t)bhi_sequence++;
	pwr(p, BAR + bhi + BHI_SEQUENCE, sequence);
	say("the SBL is handed over: the sequence register now reads %u", sequence);
	pcie_stop_here(5, "the sequence is written, the chip has been told to fetch");

	error = bhi_wait(p, bhi + BHI_COMPLETION, sequence, false);
	say("the wait for the hand-over returned %d", error);
	pcie_stop_here(6, "the wait for completion returned");
	if (error != 0) {
		return error;
	}
	report(p, "the execution environment right after the hand-over", prd(p, BAR + bhi + BHI_EE));
	report(p, "MHI status right after the hand-over", prd(p, BAR + MHI_STATUS));
	pcie_stop_here(7, "the execution environment has been read back");
	{
		/*
		 * The copy is answered; the SBL then has to be checked and started.  It is not instant,
		 * so watch the execution environment rather than sampling it once: 0 is the PBL it was
		 * handed over from, 1 is the SBL running, 2 the AMSS.
		 */
		uint32_t ee_seen = prd(p, BAR + bhi + BHI_EE);
		uint32_t mhi_seen = prd(p, BAR + MHI_STATUS);
		unsigned waited;

		for (waited = 0; waited < pcie_sbl_wait; waited += 50) {
			uint32_t ee_now = prd(p, BAR + bhi + BHI_EE);
			uint32_t mhi_now = prd(p, BAR + MHI_STATUS);

			if (ee_now != ee_seen) {
				report(p, "the execution environment changed to", ee_now);
				ee_seen = ee_now;
			}
			if (mhi_now != mhi_seen) {
				report(p, "the MHI status changed to", mhi_now);
				mhi_seen = mhi_now;
			}
			if (ee_seen != 0u) {
				break;
			}
			pause_us(p, 50 * 1000);
		}
		say("after %u ms the execution environment reads %u and the MHI status %u", waited, ee_seen, mhi_seen);
		report(p, "BHI error after the hand-over", prd(p, BAR + bhi + BHI_ERROR));
		report(p, "BHI error debug 0 after the hand-over", prd(p, BAR + bhi + BHI_ERROR_DEBUG0));
		pcie_stop_here(8, "the execution environment has been watched");
	}
	if (prd(p, BAR + bhi + BHI_EE) == 0u) {
		say("THE CHIP IS STILL IN ITS PBL: it did not take the SBL");
		return EIO;
	}
	say("stage 14: done, the SBL runs");
	return 0;
}


/* ---- stage 15: the READY handshake, and the chip's M0 ----------------------------------------- */

/*
 * The SBL is running (stage 14 watched the execution environment go from 0 to 1), and it has
 * already published itself: MHI status reads 0x101 -- bit 0 "MHI ready", state 1 in the byte at
 * bit 8 -- with MHI control's reset bit clear.  That is the state the host is supposed to answer,
 * and apollo's sweet_mhi.c treats exactly this combination as READY.
 *
 * Answering means describing, in the chip's own address window, where every ring lives: the channel
 * and event context arrays, the command ring and one ring per event, plus the address ranges the
 * host and the device may each DMA in.  Then the host asks for M0 and the chip runs its own AMSS.
 *
 * All of it is memory the chip will write to, so it comes from dma_alloc() -- the radio's view of
 * physical pages, which is what stage 11's SMMU context was built for.
 */
#define MHI_CHANNEL_COUNT 128u
#define MHI_EVENT_COUNT 3u
#define MHI_CONTEXT_BYTES 44u
#define MHI_COMMAND_ELEMENTS 128u
#define MHI_EVENT_ELEMENTS_0 32u
#define MHI_EVENT_ELEMENTS_1 256u
#define MHI_EVENT_ELEMENTS_2 32u
#define MHI_IPCR_TX_CHANNEL 20u
#define MHI_IPCR_RX_CHANNEL 21u
#define MHI_IPCR_EVENT_RING 1u
#define MHI_CCABAP 0x58u
#define MHI_ECABAP 0x60u
#define MHI_CRCBAP 0x68u
#define MHI_CTRL_M0 0x200u
#define MHI_STATUS_READY 1u
#define MHI_STATUS_ERROR 4u
#define MHI_STATUS_STATE 0xff00u
#define MHI_CONTROL_RESET 2u
#define MHI_READY_POLLS 3000u

static struct {
	bool on;
	struct dma channels, events, commands, command_ring;
	struct dma event_ring[MHI_EVENT_COUNT];
	uint32_t state;
} mhi;

/*
 * A 64-bit register of the MHI block, written as two words: the high half first, then the low
 * half.  Upstream mhi_write_db() does the same and says why -- the low half is what latches the
 * register, so writing it first would publish a value whose high half is still the old one.
 */
static void
mhi_reg_address(struct pcie *p, uint32_t offset, uint64_t value)
{
	pwr(p, BAR + offset + 4, (uint32_t)(value >> 32));
	pwr(p, BAR + offset, (uint32_t)value);
}

/* Context fields are 4-byte aligned, including the 64-bit pointers: words, never a 64-bit store. */
static void
mhi_context_address(uint32_t *context, unsigned word, uint64_t value)
{
	context[word] = (uint32_t)value;
	context[word + 1] = (uint32_t)(value >> 32);
}

static int
pcie_mhi_ready(struct pcie *p)
{
	uint32_t status, control, config, ee, state;
	const uint64_t sizes[4] = {
		MHI_CHANNEL_COUNT * MHI_CONTEXT_BYTES, MHI_EVENT_COUNT * MHI_CONTEXT_BYTES,
		MHI_CONTEXT_BYTES, MHI_COMMAND_ELEMENTS * 16u,
	};
	const uint64_t ring_sizes[MHI_EVENT_COUNT] = { MHI_EVENT_ELEMENTS_0 * 16u,
		MHI_EVENT_ELEMENTS_1 * 16u, MHI_EVENT_ELEMENTS_2 * 16u };

	if (pcie_mhi_mode == 0) {
		say("the READY handshake is not made: pcie_mhi=0");
		return 0;
	}
	report(p, "stage 15: the READY handshake and M0", 0);
	if (mhi.on) {
		say("the handshake was already made");
		return 0;
	}

	ee = prd(p, BAR + prd(p, BAR + MHI_BHI_OFFSET) + BHI_EE);
	status = prd(p, BAR + MHI_STATUS);
	control = prd(p, BAR + MHI_CONTROL);
	config = prd(p, BAR + MHI_CONFIG);
	say("before the handshake: environment %u, MHI status %#x, control %#x, config %#x", ee, status,
	    control, config);
	if (ee == 0u) {
		say("THE CHIP IS STILL IN ITS PBL: there is no SBL to answer");
		return EBUSY;
	}
	if ((status & MHI_STATUS_READY) == 0u || (control & MHI_CONTROL_RESET) != 0u ||
	    ((status & MHI_STATUS_STATE) >> 8) != 1u) {
		say("the chip is not READY (status %#x, control %#x): the rings are not described", status,
		    control);
		return EBUSY;
	}
	say("the chip is READY: its status is %#x and it is waiting for the rings", status);

	if (!dma_alloc(&mhi.channels, sizes[0], PTE_PAGE) ||
	    !dma_alloc(&mhi.events, sizes[1], PTE_PAGE) ||
	    !dma_alloc(&mhi.commands, sizes[2], PTE_PAGE) ||
	    !dma_alloc(&mhi.command_ring, sizes[3], PTE_PAGE)) {
		say("the channel, event, command or command ring memory was not allocated");
		return ENOMEM;
	}
	for (unsigned i = 0; i < MHI_EVENT_COUNT; i++) {
		if (!dma_alloc(&mhi.event_ring[i], ring_sizes[i], PTE_PAGE)) {
			say("event ring %u was not allocated", i);
			return ENOMEM;
		}
	}
	say("the rings are at %#llx (channels), %#llx (events), %#llx (commands), %#llx (command ring)",
	    mhi.channels.address, mhi.events.address, mhi.commands.address, mhi.command_ring.address);
	for (unsigned i = 0; i < MHI_EVENT_COUNT; i++) {
		say("event ring %u is at %#llx, %llu bytes", i, mhi.event_ring[i].address, ring_sizes[i]);
	}
	pcie_stop_here(11, "the rings are allocated in the radio's window");

	/*
	 * The channel contexts: the stock UMI channels publish their direction and event ring now, and
	 * the two IPCR channels do too.  The rest stay zero -- disabled -- which is what the firmware
	 * expects of channels it has not been asked to start.
	 */
	{
		static const struct { unsigned channel, direction; } stock[] = {
			{ 0, 1 }, { 1, 2 }, { 4, 1 }, { 5, 2 },
			{ MHI_IPCR_TX_CHANNEL, 1 }, { MHI_IPCR_RX_CHANNEL, 2 },
		};

		for (unsigned i = 0; i < sizeof(stock) / sizeof(stock[0]); i++) {
			uint32_t *context = (uint32_t *)(uintptr_t)mhi.channels.bytes +
			    stock[i].channel * (MHI_CONTEXT_BYTES / 4);

			context[0] = 2u << 8;       /* disabled, stock burst mode 2, poll configuration 0 */
			context[1] = stock[i].direction;
			context[2] = MHI_IPCR_EVENT_RING;
		}
	}
	for (unsigned i = 0; i < MHI_EVENT_COUNT; i++) {
		uint32_t *context = (uint32_t *)(uintptr_t)mhi.events.bytes +
		    i * (MHI_CONTEXT_BYTES / 4);
		uint64_t base = mhi.event_ring[i].address;
		uint64_t length = ring_sizes[i];

		context[0] = i == 2 ? 1u << 16 : 0u;    /* stock interrupt moderation */
		context[1] = 1u;                        /* a valid software event ring */
		context[2] = i == 0 ? 1u : (i == 1 ? 2u : 0u);
		mhi_context_address(context, 3, base);
		mhi_context_address(context, 5, length);
		mhi_context_address(context, 7, base);
		mhi_context_address(context, 9, base + length - 16u);
	}
	{
		uint32_t *command = (uint32_t *)(uintptr_t)mhi.commands.bytes;

		mhi_context_address(command, 3, mhi.command_ring.address);
		mhi_context_address(command, 5, MHI_COMMAND_ELEMENTS * 16u);
		mhi_context_address(command, 7, mhi.command_ring.address);
		mhi_context_address(command, 9, mhi.command_ring.address);
	}
	dma_clean(&mhi.channels);
	dma_clean(&mhi.events);
	dma_clean(&mhi.commands);
	dma_clean(&mhi.command_ring);
	for (unsigned i = 0; i < MHI_EVENT_COUNT; i++) {
		dma_clean(&mhi.event_ring[i]);
	}
	say("the channel, event and command contexts are written and cleaned out of the cache");
	pcie_stop_here(12, "the contexts are written");

	mhi_reg_address(p, MHI_CCABAP, mhi.channels.address);
	mhi_reg_address(p, MHI_ECABAP, mhi.events.address);
	mhi_reg_address(p, MHI_CRCBAP, mhi.commands.address);
	mhi_reg_address(p, MHI_HOST_DMA_LOW, RADIO_IOVA);
	mhi_reg_address(p, MHI_HOST_DMA_HIGH, RADIO_IOVA + RADIO_IOVA_SIZE);
	mhi_reg_address(p, MHI_DEV_DMA_LOW, RADIO_IOVA);
	mhi_reg_address(p, MHI_DEV_DMA_HIGH, RADIO_IOVA + RADIO_IOVA_SIZE);
	/*
	 * Only the NER field belongs to us.  Writing (config & 0xffff) | (count << 16) also clears
	 * bits [31:24], which is MHICFG_NHWER, the number of hardware event rings -- the value the
	 * chip reports before we touch it is 0x2800880, so NHWER was 2 and we were making it 0.
	 * Upstream uses mhi_write_reg_field() for exactly this reason.
	 */
	config = prd(p, BAR + MHI_CONFIG);
	pwr(p, BAR + MHI_CONFIG,
	    (config & ~0x00ff0000u) | ((uint32_t)MHI_EVENT_COUNT << 16));
	__asm__ volatile ("dsb sy" : : : "memory");
	report(p, "MHI config now reads", prd(p, BAR + MHI_CONFIG));
	report(p, "the hardware event rings the chip reports", prd(p, BAR + MHI_CONFIG) >> 24);
	report(p, "the event rings we asked for", prd(p, BAR + MHI_CONFIG) >> 16 & 0xffu);
	report(p, "the host's DMA window starts at (low)", prd(p, BAR + MHI_HOST_DMA_LOW));
	report(p, "the host's DMA window ends at (low)", prd(p, BAR + MHI_HOST_DMA_HIGH));
	pcie_stop_here(13, "the base addresses and the DMA windows are written");

	/*
	 * The event rings have to have their free entries published before M0: the chip writes into
	 * them as soon as it starts, and an event ring whose producer has nowhere to go is a fault.
	 */
	{
		uint32_t event_db = prd(p, BAR + MHI_EVENT_DB);

		for (unsigned i = 0; i < MHI_EVENT_COUNT; i++) {
			mhi_reg_address(p, event_db + i * 8u,
			    mhi.event_ring[i].address + ring_sizes[i] - 16u);
		}
		__asm__ volatile ("dsb sy" : : : "memory");
		say("the %u event doorbells are published, and M0 is asked for", MHI_EVENT_COUNT);
	}
	pwr(p, BAR + MHI_CONTROL, (control & ~0xff00u) | MHI_CTRL_M0);
	__asm__ volatile ("dsb sy" : : : "memory");
	report(p, "MHI control now reads", prd(p, BAR + MHI_CONTROL));
	pcie_stop_here(14, "M0 has been asked for, before waiting");

	for (state = 0, status = 0; state < MHI_READY_POLLS; state++) {
		status = prd(p, BAR + MHI_STATUS);
		if (status == UINT32_MAX || (status & MHI_STATUS_ERROR) != 0u) {
			break;
		}
		if (((status & MHI_STATUS_STATE) >> 8) == 2u) {
			break;
		}
		pause_us(p, 1000);
	}
	say("after %u polls the MHI status reads %#x (state %u)", state, status,
	    (status & MHI_STATUS_STATE) >> 8);
	pcie_stop_here(15, "the wait for M0 is over");
	if (((status & MHI_STATUS_STATE) >> 8) != 2u) {
		say("THE CHIP DID NOT ENTER M0: the status stayed at %#x", status);
		return ETIMEDOUT;
	}
	mhi.state = (status & MHI_STATUS_STATE) >> 8;
	mhi.on = true;
	say("stage 15: done, the chip is in M0 (the SBL's M0: no AMSS yet)");
	pcie_stop_here(16, "the chip is in M0");
	return 0;
}


/* ---- stage 16: the AMSS, through BHIe ---------------------------------------------------------- */

/*
 * Stage 15 asked for M0 and the chip went there, but the firmware running in it is the SBL's own:
 * the radio has no AMSS until the rest of amss20.bin is handed over.  BHIe is how: the whole image
 * in one contiguous run in the radio's DMA window, a vector table describing it as 512 KiB
 * segments, and then the table's address and length written to BHIe.
 *
 * The execution environment gates it: 1 is the SBL, which accepts an AMSS; 2 or 4 mean an AMSS is
 * already running, and 0 means the PBL, which only takes the SBL.
 */
#define BHIE_VECTORS 0x2cu
#define BHIE_TABLE_LENGTH 0x34u
#define BHIE_SEQUENCE 0x3cu
#define BHIE_COMPLETION 0x44u
#define MHI_EE_AMSS 2u
#define MHI_EE_AMSS_WITH_SBL 4u

static struct {
	bool on;
	struct dma image, vectors;
	uint32_t segments;
} amss;

static int
pcie_amss(struct pcie *p)
{
	uint32_t bhi, bhie, ee, sequence, length;
	uint64_t *vectors;
	const uint8_t *image;
	uint64_t size;
	int error;

	if (pcie_amss_mode == 0) {
		say("the AMSS is not sent: pcie_amss=0");
		return 0;
	}
	report(p, "stage 16: the AMSS, through BHIe", 0);
	if (amss.on) {
		say("the AMSS was already sent");
		return 0;
	}
	if (!mhi.on) {
		say("the handshake of stage 15 has not been made: BHIe needs READY first");
		return EBUSY;
	}
	bhi = prd(p, BAR + MHI_BHI_OFFSET);
	bhie = prd(p, BAR + MHI_BHIE_OFFSET);
	ee = prd(p, BAR + bhi + BHI_EE);
	say("before BHIe: environment %u, BHIe offset %#x, MHI status %#x", ee, bhie,
	    prd(p, BAR + MHI_STATUS));
	if (ee != 1u) {
		say("THE CHIP IS NOT IN ITS SBL (environment %u): an AMSS is not taken from here", ee);
		return EBUSY;
	}
	if (bhie == 0u || bhie == UINT32_MAX) {
		say("the BHIe block is not where stage 2 found it");
		return ENODEV;
	}

	image = hakios_amss_bytes;
	size = hakios_amss_size;
	if (size < SEGMENT_SIZE) {
		say("it is smaller than one segment");
		return EINVAL;
	}
	if (size > IMAGE_LIMIT) {
		say("it is larger than the %u bytes the radio's window is meant for", IMAGE_LIMIT);
		return EINVAL;
	}
	amss.segments = (uint32_t)((size + SEGMENT_SIZE - 1) / SEGMENT_SIZE);
	length = amss.segments * 16u;
	say("the AMSS is in the kernel image, %llu bytes in %u segments of %u", size, amss.segments,
	    SEGMENT_SIZE);

	if (!dma_alloc(&amss.image, size, PTE_PAGE)) {
		say("the image does not fit in the radio's window");
		return ENOMEM;
	}
	memcpy((void *)amss.image.bytes, image, (size_t)size);
	dma_clean(&amss.image);
	say("the image is at %#llx, which the radio sees at %#llx", amss.image.bytes, amss.image.address);
	pcie_stop_here(17, "the AMSS image is copied and clean");

	if (!dma_alloc(&amss.vectors, length, PTE_PAGE)) {
		say("the vector table was not allocated");
		return ENOMEM;
	}
	vectors = (uint64_t *)amss.vectors.bytes;
	for (uint32_t i = 0; i < amss.segments; i++) {
		uint64_t offset = (uint64_t)i * SEGMENT_SIZE;
		uint64_t chunk = size - offset;

		if (chunk > SEGMENT_SIZE) {
			chunk = SEGMENT_SIZE;
		}
		vectors[2 * i] = amss.image.address + offset;
		vectors[2 * i + 1] = chunk;
	}
	dma_clean(&amss.vectors);
	say("the vector table is at %#llx, %u bytes: segment 0 at %#llx, %llu bytes", amss.vectors.address,
	    length, vectors[0], vectors[1]);
	say("segment %u at %#llx, %llu bytes", amss.segments - 1u, vectors[2 * (amss.segments - 1u)],
	    vectors[2 * (amss.segments - 1u) + 1]);
	pcie_stop_here(18, "the vector table is written");

	mhi_reg_address(p, bhie + BHIE_VECTORS, amss.vectors.address);
	pwr(p, BAR + bhie + BHIE_TABLE_LENGTH, length);
	__asm__ volatile ("dmb oshst" : : : "memory");
	report(p, "the BHIe vector address reads", prd(p, BAR + bhie + BHIE_VECTORS));
	report(p, "the BHIe table length reads", prd(p, BAR + bhie + BHIE_TABLE_LENGTH));
	pcie_stop_here(19, "BHIe has the table, before the sequence");

	sequence = (uint32_t)bhi_sequence++;
	pwr(p, BAR + bhie + BHIE_SEQUENCE, sequence);
	say("BHIe is rung: sequence %u, %u segments, %llu bytes", sequence, amss.segments, size);
	__asm__ volatile ("dsb sy" : : : "memory");
	pcie_stop_here(20, "BHIe has been rung");

	error = bhi_wait(p, bhie + BHIE_COMPLETION, sequence, true);
	say("the wait for the AMSS hand-over returned %d", error);
	pcie_stop_here(21, "the wait for the AMSS hand-over is over");
	if (error != 0) {
		return error;
	}

	{
		uint32_t waited;

		for (waited = 0; waited < pcie_amss_wait; waited += 50) {
			uint32_t now = prd(p, BAR + bhi + BHI_EE);

			if (now == MHI_EE_AMSS || now == MHI_EE_AMSS_WITH_SBL) {
				report(p, "the execution environment after the AMSS", now);
				amss.on = true;
				say("stage 16: done, the AMSS is running in the chip (environment %u)", now);
				pcie_stop_here(22, "the AMSS is running");
				return 0;
			}
			pause_us(p, 50 * 1000);
		}
		report(p, "the execution environment after %u ms", prd(p, BAR + bhi + BHI_EE));
		report(p, "MHI status after %u ms", prd(p, BAR + MHI_STATUS));
		report(p, "BHI error after %u ms", prd(p, BAR + bhi + BHI_ERROR));
		report(p, "BHI error debug 0 after %u ms", prd(p, BAR + bhi + BHI_ERROR_DEBUG0));
	}
	say("THE CHIP DID NOT START THE AMSS: the environment never left the SBL");
	return ETIMEDOUT;
}

/* ---- stage 17: the command ring, and a completion through an event ring ------------------------ */

/*
 * A NOP down the command ring proves that the two halves of MHI can talk: the descriptor is read,
 * the doorbell is watched, and the completion comes back through an event ring that the host
 * services.  Until this works, nothing else about the radio can be trusted.
 */
#define MHI_COMMAND_DB 0x70u
#define MHI_CMD_NOP 1u << 16

/*
 * The command types upstream implements (main.c:1262-1320).  The NOOP above is apollo's own
 * probe and may not exist in this firmware; these three are the ones the specification and
 * every upstream driver actually send.
 */
#define MHI_CMD_NOP_TYPE        1u
#define MHI_CMD_RESET_CHAN_TYPE 16u
#define MHI_CMD_STOP_CHAN_TYPE  17u
#define MHI_CMD_START_CHAN_TYPE 18u
#define MHI_EVENT_TYPE_COMMAND_COMPLETION 0x21u
#define MHI_CC_SUCCESS 1u
#define MHI_COMMAND_POLLS 5000u

/*
 * The wake doorbell.  Upstream writes 1 here in mhi_assert_dev_wake() the first time anyone
 * needs the device awake, and 0 in mhi_deassert_dev_wake() when the last reference goes away
 * (pm.c:1120-1160).  Its address is base + the channel doorbell offset + 8 * 127, because the
 * wake doorbell is the last of the 128 channel doorbells.  We never wrote it at all.
 */
#define MHI_DEV_WAKE_DB 127u
#define MHI_EVENT_RING_COUNT MHI_EVENT_COUNT

/* The transfer event, and its handler: stage 18 writes it, mhi_service_events() forwards it. */
#define IPCR_EVENT_TRANSFER_COMPLETION 0x22u

static int mhi_ipcr_event(struct pcie *p, unsigned ring, uint64_t pointer, uint32_t dword0,
    uint32_t dword1);
/* MHI_COMMAND_ELEMENTS, MHI_CONTEXT_BYTES and the event element counts come from stage 15. */

static uint64_t mhi_event_consumer[MHI_EVENT_RING_COUNT];
static uint64_t mhi_event_wp[MHI_EVENT_RING_COUNT];
static uint64_t mhi_command_wp;
static uint32_t mhi_commands_sent;
static uint32_t pcie_nop_delay = 2000;
static uint32_t pcie_cmd = MHI_CMD_NOP_TYPE;
static uint32_t pcie_cmd_channel;
static uint32_t events_seen_total;
static bool mhi_events_started;

static uint64_t
mhi_ring_next(uint64_t base, uint64_t length, uint64_t pointer)
{
	pointer += 16u;
	return pointer == base + length ? base : pointer;
}

static bool
mhi_ring_pointer_valid(uint64_t base, uint64_t length, uint64_t pointer)
{
	return pointer >= base && pointer < base + length && ((pointer - base) & 15u) == 0u;
}

/* The 64-bit field at a given word of a context entry, read as two words: never a 64-bit load. */
static uint64_t
mhi_context_read(const uint32_t *context, unsigned word)
{
	return (uint64_t)context[word] | (uint64_t)context[word + 1] << 32;
}

static void
mhi_events_start(void)
{
	static const uint64_t lengths[MHI_EVENT_RING_COUNT] = {
		MHI_EVENT_ELEMENTS_0 * 16u, MHI_EVENT_ELEMENTS_1 * 16u, MHI_EVENT_ELEMENTS_2 * 16u,
	};

	/*
	 * Only once.  A later stage calls this again, and resetting the consumer to the base of each
	 * ring after the chip has already advanced the producer would hand it a read pointer that
	 * points backwards -- an event ring replay it has no way to understand.
	 */
	if (mhi_events_started) {
		return;
	}
	mhi_events_started = true;
	for (unsigned i = 0; i < MHI_EVENT_RING_COUNT; i++) {
		mhi_event_consumer[i] = mhi.event_ring[i].address;
		mhi_event_wp[i] = mhi.event_ring[i].address + lengths[i] - 16u;
	}
	mhi_command_wp = mhi.command_ring.address;
	mhi_commands_sent = 0;
}

/*
 * Drain one event ring.  Returns the completion code of the command at `command` in *code when one
 * arrives, or -1 when none did.  Each event is a 64-bit pointer followed by two dwords; the byte at
 * bit 16 of the second dword is the type, and the top byte of the first dword is the code.
 */
static int
mhi_service_events(struct pcie *p, unsigned ring, uint64_t command, int *code, unsigned *events_seen)
{
	static const uint64_t lengths[MHI_EVENT_RING_COUNT] = {
		MHI_EVENT_ELEMENTS_0 * 16u, MHI_EVENT_ELEMENTS_1 * 16u, MHI_EVENT_ELEMENTS_2 * 16u,
	};
	uint32_t *context;
	uint64_t base, length, producer;
	unsigned processed = 0;
	int error;

	if (ring >= MHI_EVENT_RING_COUNT || !mhi.event_ring[ring].bytes) {
		return EINVAL;
	}
	context = (uint32_t *)(uintptr_t)mhi.events.bytes + ring * (MHI_CONTEXT_BYTES / 4);
	base = mhi.event_ring[ring].address;
	length = lengths[ring];
	/*
	 * The producer pointer is the device's, and the ring's contents are the device's too.  Both
	 * are read here, so both are invalidated here first -- otherwise a line the CPU has been
	 * serving from cache looks like an event ring that never advances.
	 */
	dma_invalidate(&mhi.events);
	dma_invalidate(&mhi.event_ring[ring]);
	producer = mhi_context_read(context, 7);
	if (!mhi_ring_pointer_valid(base, length, producer)) {
		say("the event ring %u has a producer pointer of %#llx, which is outside it", ring, producer);
		return EIO;
	}
	while (mhi_event_consumer[ring] != producer && processed < length / 16u) {
		uint64_t current = mhi_event_consumer[ring];
		size_t offset = (size_t)(current - base);
		volatile uint32_t *event = (volatile uint32_t *)(volatile void *)(void *)
		    (mhi.event_ring[ring].bytes + offset);
		uint64_t pointer;
		uint32_t dword0, dword1;
		unsigned type;

		__asm__ volatile ("dmb oshld" : : : "memory");
		pointer = (uint64_t)event[0] | (uint64_t)event[1] << 32;
		dword0 = event[2];
		dword1 = event[3];
		type = (dword1 >> 16) & 0xffu;
		if (type == MHI_EVENT_TYPE_COMMAND_COMPLETION || type == 0x20u || type == 0x40u) {
			report(p, "an event arrived: type", type);
			report(p, "the event's code and ring", (dword0 >> 24) | (ring << 8));
			report(p, "the event's pointer (low)", (uint32_t)pointer);
			report(p, "the event's second dword", dword1);
		}
		if (type == 0x20u) {
			/*
			 * A state-change event.  dword1's low byte is the execution environment the
			 * firmware has moved to, and 0xff is the one it dies in; both raw dwords are
			 * printed so the reading can be checked against the layout instead of trusted.
			 */
			report(p, "the state change: dword0", dword0);
			report(p, "the state change: dword1", dword1);
			report(p, "the state change: the execution environment is", dword1 & 0xffu);
		}
		if (ring == 0u && type == MHI_EVENT_TYPE_COMMAND_COMPLETION && pointer == command && code) {
			*code = (int)(dword0 >> 24);
		}
		if (type == IPCR_EVENT_TRANSFER_COMPLETION) {
			error = mhi_ipcr_event(p, ring, pointer, dword0, dword1);
			if (error != 0) {
				return error;
			}
		}
		events_seen_total++;
		if (events_seen) {
			(*events_seen)++;
		}
		mhi_event_consumer[ring] = mhi_ring_next(base, length, current);
		mhi_event_wp[ring] = mhi_ring_next(base, length, mhi_event_wp[ring]);
		processed++;
	}
	if (processed == length / 16u && mhi_event_consumer[ring] != producer) {
		say("the event ring %u is full and the producer has not caught up", ring);
		return ELOOP;
	}
	if (processed != 0u) {
		mhi_context_address(context, 9, mhi_event_wp[ring]);
		dma_clean(&mhi.events);
		/* Hand the freed slots back before the chip runs out of them. */
		mhi_reg_address(p, prd(p, BAR + MHI_EVENT_DB) + ring * 8u, mhi_event_wp[ring]);
		__asm__ volatile ("dsb sy" : : : "memory");
	}
	return 0;
}

static int
pcie_mhi_nop(struct pcie *p)
{
	uint64_t base, command, next;
	uint32_t *context;
	volatile uint32_t *tre;
	int code = -1;
	int error;

	if (pcie_nop_mode == 0) {
		say("the command ring is not exercised: pcie_nop=0");
		return 0;
	}
	report(p, "stage 17: a NOP down the command ring", 0);
	if (!amss.on) {
		say("the AMSS is not running (stage 16): there is nothing to answer a command");
		return EBUSY;
	}
	if (mhi_commands_sent != 0u) {
		say("a command has already been sent");
		return 0;
	}
	pcie_stop_here(40, "stage 17 is entered, before the event rings are started");
	/*
	 * This has to come before the write pointer is read: `mhi_command_wp` is zero until the one
	 * place that sets it runs, and that place is here.  Read it first and the range check below
	 * fails on the first call, every time -- stage 17 would answer EIO forever and never panic,
	 * which is exactly what it did.
	 */
	mhi_events_start();
	pcie_stop_here(41, "the event rings are started");
	/*
	 * `EE == 2` says the AMSS started running, not that it has brought its MHI host side up.
	 * Without this pause the very first command goes out forty microseconds after the hand-over
	 * and is never answered, which looks exactly like a broken command ring.
	 */
	if (pcie_nop_delay != 0u) {
		uint32_t waited = 0, drained = 0;

		report(p, "before the first command: MHI status", prd(p, BAR + MHI_STATUS));
		for (waited = 0; waited < pcie_nop_delay; waited++) {
			for (unsigned ring = 0; ring < MHI_EVENT_RING_COUNT; ring++) {
				unsigned seen = 0;

				error = mhi_service_events(p, ring, UINT64_MAX, NULL, &seen);
				if (error != 0) {
					say("the event ring %u could not be drained while waiting", ring);
					return error;
				}
				drained += seen;
			}
			pause_us(p, 1000);
		}
		report(p, "the events retired before the first command", drained);
		report(p, "the wait before the first command, in ms", waited);
		report(p, "after the wait: MHI status", prd(p, BAR + MHI_STATUS));
		report(p, "after the wait: the execution environment",
		    prd(p, BAR + prd(p, BAR + MHI_BHI_OFFSET) + BHI_EE));
	}
	/*
	 * The command ring's read pointer does not move while the event rings work, so before
	 * anything else is blamed this prints what MHI actually holds now: every register stage 15
	 * wrote, next to the value it wrote, and the three context entries the command ring depends
	 * on.  A register that reads back as something else means the AMSS replaced the
	 * configuration; one that reads back as intended means the configuration is not the problem.
	 */
	{
		uint32_t *event_context = (uint32_t *)(uintptr_t)mhi.events.bytes;
		uint32_t *command_context = (uint32_t *)(uintptr_t)mhi.commands.bytes;
		uint32_t *chan = (uint32_t *)(uintptr_t)mhi.channels.bytes;

		say("--- MHI as the AMSS left it ---");
		report(p, "register length (0x00)", prd(p, BAR + 0x00u));
		report(p, "version (0x08)", prd(p, BAR + MHI_VERSION));
		report(p, "config (0x10)", prd(p, BAR + MHI_CONFIG));
		report(p, "channel doorbell offset (0x18)", prd(p, BAR + MHI_CHANNEL_DB));
		report(p, "event doorbell offset (0x20)", prd(p, BAR + MHI_EVENT_DB));
		report(p, "BHI offset (0x28)", prd(p, BAR + MHI_BHI_OFFSET));
		report(p, "BHIE offset (0x2c)", prd(p, BAR + MHI_BHIE_OFFSET));
		report(p, "control (0x38)", prd(p, BAR + MHI_CONTROL));
		report(p, "status (0x48)", prd(p, BAR + MHI_STATUS));
		say("channel contexts: wrote %#llx at 0x58, reads back %#x", mhi.channels.address,
		    prd(p, BAR + MHI_CCABAP));
		say("event contexts: wrote %#llx at 0x60, reads back %#x", mhi.events.address,
		    prd(p, BAR + MHI_ECABAP));
		say("command contexts: wrote %#llx at 0x68, reads back %#x", mhi.commands.address,
		    prd(p, BAR + MHI_CRCBAP));
		say("host window: wrote %#llx..%#llx at 0x80, reads back %#x..%#x", RADIO_IOVA,
		    RADIO_IOVA + RADIO_IOVA_SIZE, prd(p, BAR + MHI_HOST_DMA_LOW),
		    prd(p, BAR + MHI_HOST_DMA_HIGH));
		say("device window: wrote %#llx..%#llx at 0x98, reads back %#x..%#x", RADIO_IOVA,
		    RADIO_IOVA + RADIO_IOVA_SIZE, prd(p, BAR + MHI_DEV_DMA_LOW),
		    prd(p, BAR + MHI_DEV_DMA_HIGH));
		say("command context: rbase %#llx (want %#llx), rlen %#llx (want %#llx)",
		    mhi_context_read(command_context, 3), mhi.command_ring.address,
		    mhi_context_read(command_context, 5), (uint64_t)(MHI_COMMAND_ELEMENTS * 16u));
		say("command context: rp %#llx, wp %#llx", mhi_context_read(command_context, 7),
		    mhi_context_read(command_context, 9));
		say("event context 0: type %#x, rbase %#llx, rlen %#llx, rp %#llx, wp %#llx",
		    event_context[1], mhi_context_read(event_context, 3),
		    mhi_context_read(event_context, 5), mhi_context_read(event_context, 7),
		    mhi_context_read(event_context, 9));
		say("channel 20: flags %#x, direction %#x, event ring %#x",
		    chan[20 * (MHI_CONTEXT_BYTES / 4u)], chan[20 * (MHI_CONTEXT_BYTES / 4u) + 1u],
		    chan[20 * (MHI_CONTEXT_BYTES / 4u) + 2u]);
		say("channel 21: flags %#x, direction %#x, event ring %#x",
		    chan[21 * (MHI_CONTEXT_BYTES / 4u)], chan[21 * (MHI_CONTEXT_BYTES / 4u) + 1u],
		    chan[21 * (MHI_CONTEXT_BYTES / 4u) + 2u]);
		say("--- end of the MHI dump ---");
	}
	/*
	 * Two things upstream does when it enters mission mode, and neither of which we have ever
	 * done.  Both are cheap and both are observable, so they go in before the command that has
	 * been ignored six runs in a row.
	 */
	{
		uint32_t channel_db = prd(p, BAR + MHI_CHANNEL_DB);
		uint32_t wake_db = channel_db + 8u * MHI_DEV_WAKE_DB;

		report(p, "the channel doorbell offset, as the chip reports it", channel_db);
		report(p, "the wake doorbell, before it is written", prd(p, BAR + wake_db));
		mhi_reg_address(p, wake_db, 1);
		__asm__ volatile ("dsb sy" : : : "memory");
		report(p, "the wake doorbell, after it is written", prd(p, BAR + wake_db));
		report(p, "the wake doorbell reads back (high)", prd(p, BAR + wake_db + 4u));
	}
	{
		uint32_t event_db = prd(p, BAR + MHI_EVENT_DB);

		report(p, "the event doorbell offset, as the chip reports it", event_db);
		for (unsigned ring = 0; ring < MHI_EVENT_RING_COUNT; ring++) {
			mhi_reg_address(p, event_db + ring * 8u, mhi_event_wp[ring]);
			say("event ring %u is rung again, at %#llx", ring, mhi_event_wp[ring]);
		}
		__asm__ volatile ("dsb sy" : : : "memory");
		report(p, "the command ring's read pointer, after the rings are rung",
		    (uint32_t)mhi_context_read((const uint32_t *)(uintptr_t)mhi.commands.bytes, 7));
	}
	base = mhi.command_ring.address;
	command = mhi_command_wp;
	if (!mhi_ring_pointer_valid(base, MHI_COMMAND_ELEMENTS * 16u, command)) {
		say("the command write pointer %#llx is outside the ring", command);
		return EIO;
	}
	tre = (volatile uint32_t *)(volatile void *)(void *)
	    (mhi.command_ring.bytes + (size_t)(command - base));
	tre[0] = 0;
	tre[1] = 0;
	tre[2] = 0;
	tre[3] = (pcie_cmd_channel << 24) | (pcie_cmd << 16);
	next = mhi_ring_next(base, MHI_COMMAND_ELEMENTS * 16u, command);
	context = (uint32_t *)(uintptr_t)mhi.commands.bytes;
	mhi_context_address(context, 9, next);
	dma_clean(&mhi.command_ring);
	dma_clean(&mhi.commands);
	__asm__ volatile ("dsb sy" : : : "memory");
	say("a command is at %#llx (type %u, channel %u), the ring's write pointer moves to %#llx",
	    command, pcie_cmd, pcie_cmd_channel, next);
	pcie_stop_here(23, "the NOP descriptor is written, before the doorbell");

	mhi_reg_address(p, MHI_COMMAND_DB, next);
	__asm__ volatile ("dsb sy" : : : "memory");
	say("the command doorbell is rung at %#llx", next);
	/*
	 * Read the doorbell back.  A doorbell that reads back as what was written did land in the
	 * register file; one that reads zero was consumed or never arrived.  Either way this costs
	 * no extra run, and without it "the device ignored the ring" and "the write was lost" look
	 * exactly the same from the outside.
	 */
	report(p, "the command doorbell reads back (low)", prd(p, BAR + MHI_COMMAND_DB));
	report(p, "the command doorbell reads back (high)", prd(p, BAR + MHI_COMMAND_DB + 4u));
	report(p, "the command ring context word 9 (wp)", (uint32_t)mhi_context_read(
	    (const uint32_t *)(uintptr_t)mhi.commands.bytes, 9));
	report(p, "the command ring context word 7 (rp)", (uint32_t)mhi_context_read(
	    (const uint32_t *)(uintptr_t)mhi.commands.bytes, 7));
	pcie_stop_here(24, "the command doorbell is rung");

	for (uint32_t i = 0; i < MHI_COMMAND_POLLS; i++) {
		unsigned seen = 0;

		for (unsigned ring = 0; ring < MHI_EVENT_RING_COUNT; ring++) {
			error = mhi_service_events(p, ring, command, &code, &seen);
			if (error != 0) {
				return error;
			}
		}
		if (code >= 0) {
			break;
		}
		pause_us(p, 1000);
	}
	{
		/*
		 * Everything below is what tells a dead AMSS apart from a dropped doorbell, and it has
		 * to run before the probe: pcie_stop_here(25) panics, and a panic that happens first
		 * takes the answer with it.
		 */
		uint32_t bhi_here = prd(p, BAR + MHI_BHI_OFFSET);
		uint32_t *event_context = (uint32_t *)(uintptr_t)mhi.events.bytes;
		uint32_t *command_context = (uint32_t *)(uintptr_t)mhi.commands.bytes;

		report(p, "after the poll: MHI status", prd(p, BAR + MHI_STATUS));
		report(p, "after the poll: MHI control", prd(p, BAR + MHI_CONTROL));
		report(p, "after the poll: the execution environment",
		    prd(p, BAR + bhi_here + BHI_EE));
		report(p, "after the poll: BHI error", prd(p, BAR + bhi_here + BHI_ERROR));
		report(p, "after the poll: BHI error debug 0",
		    prd(p, BAR + bhi_here + BHI_ERROR_DEBUG0));
		report(p, "after the poll: event ring 0's producer",
		    (uint32_t)mhi_context_read(event_context, 7));
		report(p, "after the poll: event ring 0's consumer", (uint32_t)mhi_event_consumer[0]);
		report(p, "after the poll: the command ring's read pointer",
		    (uint32_t)mhi_context_read(command_context, 7));
		report(p, "after the poll: the command ring's write pointer",
		    (uint32_t)mhi_context_read(command_context, 9));
		report(p, "after the poll: the commands completed", mhi_commands_sent);
		report(p, "after the poll: the events processed", (uint32_t)events_seen_total);
		say("after the poll: the ring is at %#llx, which the radio sees at %#llx",
		    (uint64_t)(uintptr_t)mhi.command_ring.bytes, mhi.command_ring.address);
	}
	pcie_stop_here(25, "the poll for the completion is over");
	if (code < 0) {
		say("THE COMMAND DID NOT COMPLETE: the chip did not answer the NOP");
		return ETIMEDOUT;
	}
	mhi_command_wp = next;
	mhi_commands_sent++;
	report(p, "the command completed with code", (uint32_t)code);
	if (code != (int)MHI_CC_SUCCESS) {
		say("THE NOP WAS REFUSED: its completion code is %d, not %u", code, MHI_CC_SUCCESS);
		return EIO;
	}
	pcie_stop_here(26, "the NOP completed");
	say("stage 17: done, the command ring and the event rings both work");
	pcie_stop_here(27, "stage 17 is over");
	return 0;
}

/* ---- stage 18: the IPCR channels, and QRTR's hello -------------------------------------------- */

/*
 * Stage 17 proved that the command ring works.  The radio's control plane does not run on the
 * command ring, though: on a QCA6390, QRTR rides two MHI channels -- 20 out of the host and 21
 * back into it.  Each channel is a ring of 16-byte transfer descriptors whose payloads live in a
 * separate pool of 8 KiB buffers, and each completion comes back as a transfer event (type 0x22)
 * on event ring 1.
 *
 * The first thing QRTR says is hello: a 52-byte packet, a 32-byte header with version 1 and a
 * 16-byte control payload.  If the radio answers it, the name service is up, and every later
 * step -- the WLFW handshake, m3.bin, and ath11k itself -- has somewhere to happen.
 *
 * apollo's sweet_mhi.c and umi's umi_mhi.c do this in the same order, and the order matters:
 * the channels must be started before their descriptors mean anything, and the receive buffers
 * must be posted only after channel 21 exists.  Draining the event rings in between is not
 * housekeeping -- M0 and the AMSS both leave transfer events behind, and a host that reads them
 * as completions for work it has not done will conclude the ring is corrupt.
 */
#define IPCR_RING_ELEMENTS 32u
#define IPCR_RX_BUFFERS (IPCR_RING_ELEMENTS - 1u)
#define IPCR_BUFFER_SIZE 8192u
#define IPCR_TRE_TRANSFER ((2u << 16) | (1u << 9))       /* transfer, interrupt on end of transfer */
#define IPCR_TRANSFER_EOT 2u                             /* not MHI_CC_SUCCESS: this is a transfer */
#define MHI_CHANNEL_START 18u
/* Channel state, channel context word 0 bits 7:0 (drivers/bus/mhi/common.h:240-245). */
#define MHI_CH_STATE_RUNNING 2u
#define QRTR_HELLO_SIZE 52u
/*
 * The firmware's own QMI service, as it advertised it: service 0x45 is QMI_SERVICE_ID_WLFW
 * (Linux, include/linux/soc/qcom/qmi.h:104); instance 0x0101 is version 1 with instance 1,
 * which is ATH11K_QMI_WLFW_SERVICE_VERS_V01 and ..._INS_ID_V01_QCA6390 (ath11k/qmi.h:18-20).
 * Node 7 is the Wi-Fi DSP; port 1 is where the service answers.
 */
#define QRTR_TYPE_DATA 1u
#define QRTR_TYPE_HELLO 2u
#define QRTR_TYPE_NEW_SERVER 4u

/*
 * QRTR's transmit flow control.  The sender counts the datagrams it has sent to a port and
 * not been confirmed for; every fifth carries `confirm_rx`, and at the tenth the eleventh
 * send sleeps until the other side answers QRTR_TYPE_RESUME_TX.  Nothing else in this file
 * ever sent one, which is why the firmware stopped talking after ten datagrams.
 */
#define QRTR_TYPE_RESUME_TX 7u
#define QRTR_QRTR_TX_FLOW_HIGH 10u      /* net/qrtr/af_qrtr.c:157 */
#define QRTR_QRTR_TX_FLOW_LOW 5u        /* net/qrtr/af_qrtr.c:158 */
#define QRTR_CONTROL_WORDS 5u           /* cmd, client.node, client.port, and one word of slack */
#define WLFW_SERVICE 0x45u
#define WLFW_INSTANCE 0x0101u
#define WLFW_NODE 7u
#define WLFW_PORT 1u
#define QRTR_OUR_PORT 0x4000u
#define QRTR_HEADER_WORDS 8u
#define QMI_HEADER_SIZE 7u
#define QMI_REQUEST 0u
#define QMI_RESPONSE 2u
#define QMI_INDICATION 4u
#define QMI_WLANFW_REQUEST_MEM_IND 0x0035u
#define QMI_WLANFW_HOST_CAP_REQ 0x0034u
#define QMI_WLANFW_RESPOND_MEM_REQ 0x0036u
#define QMI_WLANFW_CAP_REQ 0x0024u
#define QMI_WLANFW_CAP_CHIP_INFO 0x10u
#define QMI_WLANFW_CAP_BOARD_INFO 0x11u
#define QMI_WLANFW_CAP_SOC_INFO 0x12u
#define QMI_WLANFW_BDF_DOWNLOAD_REQ 0x0025u
#define QMI_WLANFW_M3_INFO_REQ 0x003Cu
#define QMI_WLANFW_BDF_TYPE_ELF 1u
#define QMI_WLANFW_BDF_PIECE 6144u
#define QMI_WLANFW_M3_ADDR 0x01u
#define QMI_WLANFW_M3_SIZE_TLV 0x02u
#define QMI_WLANFW_FW_MEM_READY_IND 0x0037u
#define QMI_WLANFW_WLAN_MODE_REQ 0x0022u
#define QMI_WLANFW_FW_READY_IND 0x0021u
#define QMI_WLANFW_FW_INIT_DONE_IND 0x0038u
#define QMI_WLANFW_CAL_DONE_IND 0x003Eu
#define WLFW_FIRMWARE_MODE_COLD_BOOT 7u
#define WLFW_MODE_BODY_BYTES 11u
#define QMI_WLANFW_WLAN_INI_REQ 0x002Fu
#define QMI_WLANFW_WLAN_CFG_REQ 0x0023u
#define WLFW_INI_BODY_BYTES 4u
#define WLFW_CFG_BODY_BYTES 486u
#define WLFW_CFG_TARGET_CE 9u
#define WLFW_CFG_SERVICES 14u
#define WLFW_CFG_SHADOW_REGS 30u
#define WLFW_MODE_BODY_BYTES_NORMAL 11u
#define WLFW_INIT_WAIT 10000u
#define WLFW_CAL_WAIT 60000u
#define WLFW_MEM_SEGMENTS_MAX 8u
#define WLFW_MEM_SEGMENT_BYTES 17u

/* What the firmware has asked for, and what we answered with. */
static struct {
	bool on;
	uint32_t count;
	struct dma segment[WLFW_MEM_SEGMENTS_MAX];
	uint32_t size[WLFW_MEM_SEGMENTS_MAX];
	uint32_t type[WLFW_MEM_SEGMENTS_MAX];
} wlfw_mem;
#define QMI_WLANFW_IND_REGISTER_REQ 0x0020u
#define QMI_WLANFW_CLIENT_ID 0x4b4e454cu
#define WLFW_REPLY_WAIT 3000u
#define WLFW_PACKET_WAIT 50u
#define WLFW_RX_QUEUE 8u

static struct {
	bool on;
	struct dma tx_ring, tx_buffers, rx_ring, rx_buffers;
	uint64_t tx_wp, rx_wp;
	uint32_t tx_slot;
	bool tx_pending, tx_done, response_done, waiting_response;
	uint32_t tx_completions, rx_completions, last_rx_bytes;
	/*
	 * Every datagram the firmware sends us, kept whole and in order.  A queue, not a slot:
	 * the firmware answers the memory answer -- message 0x0036 -- with a response and, in
	 * the same breath, the "my memory is ready" indication 0x0037, both inside one drain
	 * of the event rings.  A single slot kept the second and threw the first away, which
	 * looked exactly like the firmware refusing the request.
	 */
	struct {
		uint32_t node, port, length;
		uint8_t data[1024];
	} rx_queue[WLFW_RX_QUEUE];
	uint32_t rx_head, rx_tail;      /* the firmware pushes at the head, QMI reads at the tail */
	uint32_t rx_lost;               /* datagrams that arrived with the queue full */
} ipcr;

/*
 * How many datagrams have arrived asking us to resume the firmware's transmit, and how many
 * answers we have sent.  They are counted in the receive handler, where no transmit can be
 * started, and answered at the next place the transmit ring is known to be free.
 */
static uint32_t qrtr_resume_due;
static uint32_t qrtr_resume_sent;

/* True once the firmware says the memory we handed it is ready (message 0x0037). */
static bool wlfw_mem_ready;
/*
 * The copy engines the firmware is to configure, in ath11k's own order and with ath11k's
 * own numbers (ath11k_target_ce_config_wlan_qca6390, hw.c:1623): pipe number, direction,
 * ring entries, largest transfer, attribute flags.  `reserved` is not part of the message.
 */
static const uint32_t wlfw_ce_config[WLFW_CFG_TARGET_CE][5] = {
	{ 0u, 2u, 32u, 2048u, 0u },	/* CE0  host -> target, HTC control */
	{ 1u, 1u, 32u, 2048u, 0u },	/* CE1  target -> host, HTT and HTC control */
	{ 2u, 1u, 32u, 2048u, 0u },	/* CE2  target -> host, WMI */
	{ 3u, 2u, 32u, 2048u, 0u },	/* CE3  host -> target, WMI */
	{ 4u, 2u, 256u, 256u, 8u },	/* CE4  host -> target, HTT, no interrupts */
	{ 5u, 1u, 32u, 2048u, 0u },	/* CE5  target -> host, pktlog */
	{ 6u, 3u, 32u, 16384u, 0u },	/* CE6  the target's own copy engine */
	{ 7u, 4u, 0u, 0u, 8u },		/* CE7  host-to-host only */
	{ 8u, 3u, 32u, 16384u, 0u },	/* CE8  target -> host, IPA */
};

/* Which copy engine carries which service (ath11k_target_service_to_ce_map_wlan_qca6390). */
static const uint32_t wlfw_svc_config[WLFW_CFG_SERVICES][3] = {
	{ 0x0104u, 2u, 3u },		/* WMI_DATA_VO   out */
	{ 0x0104u, 1u, 2u },		/* WMI_DATA_VO   in  */
	{ 0x0102u, 2u, 3u },		/* WMI_DATA_BK   out */
	{ 0x0102u, 1u, 2u },		/* WMI_DATA_BK   in  */
	{ 0x0101u, 2u, 3u },		/* WMI_DATA_BE   out */
	{ 0x0101u, 1u, 2u },		/* WMI_DATA_BE   in  */
	{ 0x0103u, 2u, 3u },		/* WMI_DATA_VI   out */
	{ 0x0103u, 1u, 2u },		/* WMI_DATA_VI   in  */
	{ 0x0100u, 2u, 3u },		/* WMI_CONTROL   out */
	{ 0x0100u, 1u, 2u },		/* WMI_CONTROL   in  */
	{ 0x0001u, 2u, 0u },		/* RSVD_CTRL     out */
	{ 0x0001u, 1u, 2u },		/* RSVD_CTRL     in  */
	{ 0x0300u, 2u, 4u },		/* HTT_DATA_MSG  out */
	{ 0x0300u, 1u, 1u },		/* HTT_DATA_MSG  in  */
};

/*
 * The ring head pointers the firmware is to shadow, in the order ath11k builds them
 * (ath11k_hal_srng_shadow_config then ath11k_ce_shadow_config): the receive and transmit
 * rings first, then the copy engines.  The addresses are qca6390's own.
 */
static const uint32_t wlfw_shadow_regs[WLFW_CFG_SHADOW_REGS] = {
	0x00a3b034u, 0x00a3b03cu, 0x00a3b044u, 0x00a3b04cu,	/* REO_DST 0..3 */
	0x00a3b054u,
	0x00a3b028u,						/* REO_REINJECT */
	0x00a3b020u,						/* REO_CMD */
	0x00a3b06cu,						/* REO_STATUS */
	0x00a46000u, 0x00a46008u, 0x00a46010u,			/* TCL_DATA 0..2 */
	0x00a46018u,						/* TCL_CMD */
	0x00a46034u,						/* TCL_STATUS */
	0x00a370b0u,						/* WBM_IDLE_LINK */
	0x00a37018u,						/* SW2WBM_RELEASE */
	0x00a370c4u, 0x00a370ccu, 0x00a370d4u,			/* WBM2SW_RELEASE 0..2 */
	0x00a370dcu, 0x00a370e4u,				/* WBM2SW_RELEASE 3,4 */
	0x00a00400u,						/* CE_SRC 0 */
	0x00a03400u, 0x00a0340cu,				/* CE_DST 1, status */
	0x00a05400u, 0x00a0540cu,				/* CE_DST 2, status */
	0x00a06400u,						/* CE_SRC 3 */
	0x00a08400u,						/* CE_SRC 4 */
	0x00a0b400u, 0x00a0b40cu,				/* CE_DST 5, status */
	0x00a0e400u,						/* CE_SRC 7 */
};

static bool wlfw_init_done;            /* 0x0038: the board data and M3 were taken */
static bool wlfw_fw_ready;             /* 0x0021: the firmware says it is ready */
static bool wlfw_cal_done;             /* 0x003e: the cold-boot calibration finished */

/* The board files and the M3 image, put in the kernel by tools/gen_wlanfw_blobs.py. */
extern const uint32_t hakios_wlanfw_board_count;
extern const struct hakios_wlanfw_board {
	const char *name;
	uint64_t size;
	const uint8_t *bytes;
} hakios_wlanfw_boards[];
extern const uint8_t hakios_wlanfw_m3_bytes[];
extern const uint64_t hakios_wlanfw_m3_size;

/* Where the M3 image will be copied to, so that the firmware can be told its address. */
static struct dma wlanfw_m3;
static bool wlanfw_m3_loaded;

/*
 * What the firmware says it is.  The board file's name is built out of chip_id and
 * board_id (ath11k_core_create_board_name), so both have to come from the chip: the
 * package holds forty-seven board files and only one of them belongs to this phone.
 */
static struct {
	bool on;
	uint32_t chip_id;
	uint32_t chip_family;
	uint32_t board_id;
	uint32_t soc_id;
} wlfw_cap;

static uint32_t
le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 |
	    (uint32_t)bytes[3] << 24;
}

/* What came back, in the only terms QRTR defines: a version, a type, and two node/port pairs. */
static void
mhi_qrtr_report(struct pcie *p, const uint8_t *bytes, uint32_t length)
{
	if (length < 8u) {
		say("a %u-byte packet came back: too short to be QRTR", length);
		return;
	}
	report(p, "QRTR version", le32(bytes));
	report(p, "QRTR type", le32(bytes + 4));
	if (length >= 12u) {
		report(p, "QRTR source node", le32(bytes + 8));
	}
	if (length >= 16u) {
		report(p, "QRTR source port", le32(bytes + 12));
	}
	if (length >= 36u) {
		say("the packet's control payload begins %02x %02x %02x %02x, at %u bytes",
		    bytes[32], bytes[33], bytes[34], bytes[35], length);
	}
	/*
	 * The control payload is five words: a command, then four whose meaning depends on it.  For
	 * NEW_SERVER (4) they are a service, its instance, and the node and port it answers at -- the
	 * address of the firmware's QMI service, which is what stage 19 is looking for.  For HELLO (2)
	 * and BYE (3) only the command is meaningful, so the four words are not read out loud.
	 */
	if (length >= 52u) {
		uint32_t command = le32(bytes + 32);

		report(p, "QRTR control command", command);
		if (command == 4u || command == 5u || command == 10u) {
			report(p, "the service number", le32(bytes + 36));
			report(p, "the service's instance", le32(bytes + 40));
			report(p, "the service's node", le32(bytes + 44));
			report(p, "the service's port", le32(bytes + 48));
		}
	}
}

/*
 * One MHI channel's context, in the shape Linux's mhi_init_ctxt() leaves it: enabled, burst mode
 * disabled, one event ring, and the same address for the ring's base and its write pointer's
 * starting point.
 */
static void
mhi_setup_ipcr_channel(unsigned channel, uint32_t direction, const struct dma *ring,
    uint64_t write_pointer)
{
	uint32_t *context = (uint32_t *)(uintptr_t)mhi.channels.bytes +
	    channel * (MHI_CONTEXT_BYTES / 4);

	memset((void *)context, 0, MHI_CONTEXT_BYTES);
	context[0] = 1u | (2u << 8);             /* enabled, burst mode 2, pollcfg 0 */
	context[1] = direction;
	context[2] = MHI_IPCR_EVENT_RING;
	mhi_context_address(context, 3, ring->address);
	mhi_context_address(context, 5, IPCR_RING_ELEMENTS * 16u);
	mhi_context_address(context, 7, ring->address);
	mhi_context_address(context, 9, write_pointer);
}

/*
 * A transfer event, again in Linux's terms: 0x22, and the byte below the type is the channel it
 * belongs to.  Channel 20 completes a transmit; channel 21 hands a received packet back and the
 * buffer has to be put at the producer end of the ring again, or the chip runs out of places to
 * write and stops reporting.
 */
static int
mhi_ipcr_event(struct pcie *p, unsigned ring, uint64_t pointer, uint32_t dword0, uint32_t dword1)
{
	uint32_t code = dword0 >> 24;
	unsigned channel = (dword1 >> 24) & 0xffu;

	if (ring != MHI_IPCR_EVENT_RING) {
		report(p, "a transfer event arrived on the wrong ring", ring);
		return EIO;
	}
	if (channel == MHI_IPCR_TX_CHANNEL) {
		uint64_t expected = ipcr.tx_ring.address + (uint64_t)ipcr.tx_slot * 16u;

		if (!ipcr.tx_pending) {
			say("an IPCR transmit completion arrived with nothing outstanding");
			return EIO;
		}
		if (pointer != expected) {
			report(p, "the transmit completion points at", (uint32_t)pointer);
			report(p, "the descriptor it should have pointed at", (uint32_t)expected);
			say("THE TRANSMIT COMPLETION DOES NOT BELONG TO THE TRANSFER WE ASKED FOR");
			return EIO;
		}
		ipcr.tx_pending = false;
		ipcr.tx_done = code == IPCR_TRANSFER_EOT;
		ipcr.tx_completions++;
		report(p, "the transmit completion's code and bytes", (code << 24) | (dword0 & 0xffffu));
		if (code != IPCR_TRANSFER_EOT) {
			say("THE TRANSFER WAS NOT COMPLETED: its code is %u, not %u", code,
			    IPCR_TRANSFER_EOT);
			return EIO;
		}
		return 0;
	}
	if (channel == MHI_IPCR_RX_CHANNEL) {
		uint64_t base = ipcr.rx_ring.address;
		uint64_t length = IPCR_RING_ELEMENTS * 16u;
		volatile uint32_t *tre;
		uint64_t buffer;
		uint32_t done = dword0 & 0xffffu;
		unsigned slot;
		const uint8_t *bytes;
		uint64_t next;
		volatile uint32_t *post;
		uint32_t *context;

		if (!mhi_ring_pointer_valid(base, length, pointer)) {
			report(p, "a receive completion points outside the ring, at", (uint32_t)pointer);
			return EIO;
		}
		tre = (volatile uint32_t *)(volatile void *)(void *)
		    (ipcr.rx_ring.bytes + (size_t)(pointer - base));
		__asm__ volatile ("dmb oshld" : : : "memory");
		buffer = (uint64_t)tre[0] | (uint64_t)tre[1] << 32;
		if (buffer < ipcr.rx_buffers.address ||
		    buffer >= ipcr.rx_buffers.address + ipcr.rx_buffers.size ||
		    ((buffer - ipcr.rx_buffers.address) % IPCR_BUFFER_SIZE) != 0u ||
		    done > IPCR_BUFFER_SIZE) {
			report(p, "a receive completion describes a buffer of ours badly, at",
			    (uint32_t)buffer);
			return EIO;
		}
		slot = (unsigned)((buffer - ipcr.rx_buffers.address) / IPCR_BUFFER_SIZE);
		bytes = (const uint8_t *)(uintptr_t)(ipcr.rx_buffers.bytes +
		    (size_t)slot * IPCR_BUFFER_SIZE);
		ipcr.rx_completions++;
		ipcr.last_rx_bytes = done;
		{
			const volatile uint32_t *raw = (const volatile uint32_t *)(const void *)bytes;

			if (raw[0] == 0xa5a5a5a5u) {
				say("the chip did NOT write into the receive buffer: our marker is still there");
			} else if (raw[0] == 0u) {
				say("the receive buffer holds zeroes where our marker was: something wrote, "
				    "and what it wrote was zero");
			} else {
				say("THE RECEIVE BUFFER HOLDS THE CHIP'S OWN WORDS NOW");
			}
		}
		/*
		 * The chip wrote this buffer; the CPU's copy of it is stale.  Only the slot that was just
		 * filled is invalidated -- the rest of the pool is untouched and stays in cache.
		 */
		{
			struct dma window = ipcr.rx_buffers;
			window.bytes = (vm_offset_t)(uintptr_t)bytes;
			window.size = IPCR_BUFFER_SIZE;
			dma_invalidate(&window);
		}
		report(p, "a packet came back: bytes", done);
		report(p, "the completion's pointer", (uint32_t)pointer);
		report(p, "the buffer it named", (uint32_t)buffer);
		report(p, "the buffer's slot", slot);
		{
			const volatile uint32_t *words = (const volatile uint32_t *)(const void *)bytes;

			report(p, "the buffer's first word", words[0]);
			report(p, "the buffer's second word", words[1]);
			report(p, "the buffer's third word", words[2]);
			report(p, "the buffer's fourth word", words[3]);
		}
		mhi_qrtr_report(p, bytes, done);
		umi_input(bytes, done);
		/*
		 * A QRTR DATA datagram is the firmware's side of the QMI conversation: keep it, whole,
		 * for the stage that is waiting on one.  Every other type is control traffic, which the
		 * hello already carried us through.
		 */
		if (done >= QRTR_HEADER_WORDS * 4u) {
			const uint32_t *header = (const uint32_t *)(const void *)bytes;

			/*
			 * The sender's flow control, read out of its every datagram: `confirm` says
			 * it has passed its low watermark and is waiting for an answer before it
			 * sends many more.  We cannot answer from here -- this runs inside a
			 * transmit -- so the request is only counted, and answered later.
			 */
			report(p, "the datagram's confirm flag", header[4]);
			if (header[4] != 0u && header[1] == QRTR_TYPE_DATA) {
				qrtr_resume_due++;
				say("THE FIRMWARE ASKS US TO RESUME ITS TRANSMIT, having sent %u since the "
				    "last answer", header[4]);
			}
			if (header[1] == QRTR_TYPE_RESUME_TX) {
				report(p, "a resume for our own transmit, which never needs one", header[1]);
			}
			if (header[1] == QRTR_TYPE_DATA) {
				uint32_t size = header[5];
				uint32_t head = ipcr.rx_head;

				if (size <= sizeof(ipcr.rx_queue[0].data) &&
				    QRTR_HEADER_WORDS * 4u + size <= done) {
					if ((head + 1u) % WLFW_RX_QUEUE == ipcr.rx_tail) {
						ipcr.rx_lost++;
						report(p, "a datagram arrived with nowhere to put it, of bytes",
						    size);
					} else {
						ipcr.rx_queue[head].node = header[2];
						ipcr.rx_queue[head].port = header[3];
						ipcr.rx_queue[head].length = size;
						memcpy(ipcr.rx_queue[head].data,
						    bytes + QRTR_HEADER_WORDS * 4u, size);
						ipcr.rx_head = (head + 1u) % WLFW_RX_QUEUE;
						report(p, "a datagram from the firmware, of bytes", size);
					}
				} else {
					report(p, "a datagram that does not fit its buffer, of bytes", size);
				}
			}
		}

		/* Repost the buffer at the producer end of the ring: the chip needs the room back. */
		next = ipcr.rx_wp;
		if (!mhi_ring_pointer_valid(base, length, next)) {
			say("the receive ring's write pointer is outside it");
			return EIO;
		}
		post = (volatile uint32_t *)(volatile void *)(void *)
		    (ipcr.rx_ring.bytes + (size_t)(next - base));
		post[0] = (uint32_t)buffer;
		post[1] = (uint32_t)(buffer >> 32);
		post[2] = IPCR_BUFFER_SIZE;
		post[3] = IPCR_TRE_TRANSFER;
		ipcr.rx_wp = mhi_ring_next(base, length, next);
		context = (uint32_t *)(uintptr_t)mhi.channels.bytes +
		    MHI_IPCR_RX_CHANNEL * (MHI_CONTEXT_BYTES / 4);
		mhi_context_address(context, 9, ipcr.rx_wp);
		dma_clean(&ipcr.rx_ring);
		dma_clean(&mhi.channels);
		__asm__ volatile ("dsb sy" : : : "memory");
		mhi_reg_address(p, prd(p, BAR + MHI_CHANNEL_DB) + MHI_IPCR_RX_CHANNEL * 8u,
		    ipcr.rx_wp);
		__asm__ volatile ("dsb sy" : : : "memory");
		return 0;
	}
	report(p, "a transfer event for a channel we did not start", channel);
	return 0;
}

/* A command down the command ring, and the completion that comes back through event ring 0. */
static int
mhi_start_channel(struct pcie *p, unsigned channel)
{
	uint64_t base = mhi.command_ring.address;
	uint64_t length = MHI_COMMAND_ELEMENTS * 16u;
	uint64_t command = mhi_command_wp;
	volatile uint32_t *tre;
	uint32_t *context;
	uint64_t next;
	int code = -1;
	int error;

	if (!mhi_ring_pointer_valid(base, length, command)) {
		say("the command write pointer is outside the ring");
		return EIO;
	}
	tre = (volatile uint32_t *)(volatile void *)(void *)
	    (mhi.command_ring.bytes + (size_t)(command - base));
	tre[0] = 0;
	tre[1] = 0;
	tre[2] = 0;
	tre[3] = (uint32_t)channel << 24 | MHI_CHANNEL_START << 16;
	next = mhi_ring_next(base, length, command);
	context = (uint32_t *)(uintptr_t)mhi.commands.bytes;
	mhi_context_address(context, 9, next);
	dma_clean(&mhi.command_ring);
	dma_clean(&mhi.commands);
	__asm__ volatile ("dsb sy" : : : "memory");
	say("a channel start for %u is at %#llx, the ring's write pointer moves to %#llx", channel,
	    command, next);
	mhi_reg_address(p, MHI_COMMAND_DB, next);
	__asm__ volatile ("dsb sy" : : : "memory");

	for (uint32_t i = 0; i < MHI_COMMAND_POLLS; i++) {
		for (unsigned ring = 0; ring < MHI_EVENT_RING_COUNT; ring++) {
			error = mhi_service_events(p, ring, command, &code, NULL);
			if (error != 0) {
				return error;
			}
		}
		if (code >= 0) {
			break;
		}
		pause_us(p, 1000);
	}
	mhi_command_wp = next;
	mhi_commands_sent++;
	if (code < 0) {
		const uint32_t *cmd = (const uint32_t *)(uintptr_t)mhi.commands.bytes;
		const uint32_t *ctx = (const uint32_t *)(uintptr_t)mhi.channels.bytes +
		    channel * (MHI_CONTEXT_BYTES / 4);
		uint32_t state;

		report(p, "the channel start never completed; MHI status", prd(p, BAR + MHI_STATUS));
		report(p, "MHI control", prd(p, BAR + MHI_CONTROL));
		/*
		 * The command ring's read pointer is the device's own copy.  If the firmware took the
		 * descriptor this run wrote, this moved by one element (16 bytes) off the ring base.  It
		 * is the only witness to "the command was read" that our host-side bookkeeping cannot
		 * fake, because the device wrote it.
		 */
		report(p, "the command ring's read pointer, after the failure",
		    (uint32_t)mhi_context_read(cmd, 7));
		report(p, "the command ring's write pointer, after the failure",
		    (uint32_t)mhi_context_read(cmd, 9));
		report(p, "the command ring's base, for comparison", (uint32_t)mhi.command_ring.address);
		report(p, "the channel's context, enabled word", ctx[0]);
		report(p, "the channel's context, event ring", ctx[2]);
		report(p, "the channel's context, ring base", ctx[3]);
		report(p, "the channel's context, ring length", ctx[5]);
		report(p, "the channel's context, read pointer", (uint32_t)mhi_context_read(ctx, 7));
		report(p, "the channel's context, write pointer", (uint32_t)mhi_context_read(ctx, 9));
		for (unsigned ring = 0; ring < MHI_EVENT_RING_COUNT; ring++) {
			const uint32_t *event_ctx = (const uint32_t *)(uintptr_t)mhi.events.bytes +
			    ring * (MHI_CONTEXT_BYTES / 4);

			report(p, "an event ring's read pointer, after the failure",
			    (uint32_t)mhi_context_read(event_ctx, 7));
			report(p, "an event ring's write pointer, after the failure",
			    (uint32_t)mhi_context_read(event_ctx, 9));
		}
		say("--- the command context, word by word ---");
		for (unsigned word = 0; word < MHI_CONTEXT_BYTES / 4u; word++) {
			say("command context word %u = %#x", word, cmd[word]);
		}
		say("--- channel %u's context, word by word ---", channel);
		for (unsigned word = 0; word < MHI_CONTEXT_BYTES / 4u; word++) {
			say("channel context word %u = %#x", word, ctx[word]);
		}
		say("--- what stage 18 wrote into that context ---");
		say("word 0 should be %#x (enabled, burst mode 2)", 1u | (2u << 8));
		say("word 3 should be the ring base");
		say("word 5 should be %#x (32 elements of 16 bytes)", IPCR_RING_ELEMENTS * 16u);
		say("word 7 should be the ring base too (the read pointer starts there)");
		say("word 9 should be the write pointer after the first transfer");
		state = ctx[0] & 0xffu;
		if (state == MHI_CH_STATE_RUNNING) {
			say("the channel %u never reported, but the chip has moved it to RUNNING: the "
			    "command was executed", channel);
			return 0;
		}
		say("THE CHANNEL %u WAS NOT STARTED: its state reads %u", channel, state);
		return ETIMEDOUT;
	}
	report(p, "the channel start completed with code", (uint32_t)code);
	if (code != (int)MHI_CC_SUCCESS) {
		say("THE CHANNEL %u WAS REFUSED: its completion code is %d, not %u", channel, code,
		    MHI_CC_SUCCESS);
		return EIO;
	}
	return 0;
}

/*
 * A QRTR packet out on channel 20, and the wait for it to be over: the transfer's own completion
 * first, and -- when the caller names a transaction -- the radio's answer after it.  The answer
 * is recognised the way umi recognises it: version 1, a hello, and the transaction number in the
 * two bytes after the control payload's first byte.
 */
static int
mhi_send_ipcr_packet(struct pcie *p, const void *packet, uint32_t length, uint32_t wait_ms,
    uint16_t transaction)
{
	uint64_t base = ipcr.tx_ring.address;
	uint64_t length16 = IPCR_RING_ELEMENTS * 16u;
	uint64_t buffer;
	volatile uint32_t *tre;
	uint32_t *context;
	uint32_t slot;
	int error;

	if (!ipcr.on) {
		say("the IPCR channels are not running");
		return ENXIO;
	}
	if (length == 0u || length > IPCR_BUFFER_SIZE) {
		return EINVAL;
	}
	if (!mhi_ring_pointer_valid(base, length16, ipcr.tx_wp)) {
		say("the transmit ring's write pointer is outside it");
		return EIO;
	}
	slot = (uint32_t)((ipcr.tx_wp - base) / 16u);
	buffer = ipcr.tx_buffers.address + (uint64_t)slot * IPCR_BUFFER_SIZE;
	memcpy((void *)(uintptr_t)(ipcr.tx_buffers.bytes + (size_t)slot * IPCR_BUFFER_SIZE), packet,
	    length);
	tre = (volatile uint32_t *)(volatile void *)(void *)
	    (ipcr.tx_ring.bytes + (size_t)(ipcr.tx_wp - base));
	tre[0] = (uint32_t)buffer;
	tre[1] = (uint32_t)(buffer >> 32);
	tre[2] = length;
	tre[3] = IPCR_TRE_TRANSFER;
	ipcr.tx_slot = slot;
	ipcr.tx_pending = true;
	ipcr.tx_done = false;
	ipcr.response_done = false;
	ipcr.waiting_response = transaction != UINT16_MAX;
	context = (uint32_t *)(uintptr_t)mhi.channels.bytes +
	    MHI_IPCR_TX_CHANNEL * (MHI_CONTEXT_BYTES / 4);
	ipcr.tx_wp = mhi_ring_next(base, length16, ipcr.tx_wp);
	mhi_context_address(context, 9, ipcr.tx_wp);
	dma_clean(&ipcr.tx_buffers);
	dma_clean(&ipcr.tx_ring);
	dma_clean(&mhi.channels);
	__asm__ volatile ("dsb sy" : : : "memory");
	say("%u bytes are queued in transmit slot %u at %#llx; the ring moves to %#llx", length, slot,
	    buffer, ipcr.tx_wp);
	/*
	 * Upstream's mhi_assert_dev_wake(): a ring doorbell does not wake a device that has gone into
	 * M3.  Without this the request is read whenever the poll thread's heartbeat next asserts the
	 * wake doorbell -- up to a second late, long past the firmware's cold boot window, and it goes
	 * SYS_ERR on it.  The heartbeat alone is not enough: it is a poll, not a guarantee.
	 */
	mhi_reg_address(p, prd(p, BAR + MHI_CHANNEL_DB) + 8u * MHI_DEV_WAKE_DB, 0);
	__asm__ volatile ("dsb sy" : : : "memory");
	mhi_reg_address(p, prd(p, BAR + MHI_CHANNEL_DB) + 8u * MHI_DEV_WAKE_DB, 1);
	__asm__ volatile ("dsb sy" : : : "memory");
	mhi_reg_address(p, prd(p, BAR + MHI_CHANNEL_DB) + MHI_IPCR_TX_CHANNEL * 8u,
	    ipcr.tx_wp);
	__asm__ volatile ("dsb sy" : : : "memory");

	for (uint32_t i = 0; i < wait_ms; i++) {
		for (unsigned ring = 0; ring < MHI_EVENT_RING_COUNT; ring++) {
			error = mhi_service_events(p, ring, UINT64_MAX, NULL, NULL);
			if (error != 0) {
				return error;
			}
		}
		if (ipcr.tx_done && (!ipcr.waiting_response || ipcr.response_done)) {
			break;
		}
		pause_us(p, 1000);
	}
	/*
	 * The transaction is over either way: drop the wake so the next send's write of 1 is
	 * a fresh rising edge, not a level the device stopped listening to (docs section 48.7,
	 * lead 1: pinned at 1, the firmware held its downlink for 60 s until the next channel
	 * doorbell).
	 */
	mhi_reg_address(p, prd(p, BAR + MHI_CHANNEL_DB) + 8u * MHI_DEV_WAKE_DB, 0);
	__asm__ volatile ("dsb sy" : : : "memory");
	if (!ipcr.tx_done) {
		report(p, "the transmit never completed; MHI status", prd(p, BAR + MHI_STATUS));
		report(p, "MHI control", prd(p, BAR + MHI_CONTROL));
		say("THE PACKET WAS NOT TRANSMITTED");
		return ETIMEDOUT;
	}
	say("the packet is transmitted: %u bytes, %u completions so far; the wake is dropped",
	    length, ipcr.tx_completions);
	return 0;
}

/*
 * The answer to a `confirm_rx`: a five-word control datagram whose command is RESUME_TX and
 * whose client is the stream it clears -- ours, (our node, our port).  The firmware resets
 * the whole count when one arrives (qrtr_tx_resume(): flow->pending = 0), so one answer is
 * enough however many confirms were seen.
 *
 * This must never be called from the receive path itself: it starts a transmit, and the
 * receive path runs inside one (`mhi_send_ipcr_packet()` drains the event rings while it
 * waits).  The caller checks `qrtr_resume_due` at a point where nothing is in flight.
 */
static int
mhi_qrtr_resume_tx(struct pcie *p)
{
	static uint8_t packet[QRTR_HEADER_WORDS * 4u + QRTR_CONTROL_WORDS * 4u];
	uint32_t *header = (uint32_t *)(void *)packet;
	uint32_t *control = header + QRTR_HEADER_WORDS;
	int error;

	memset(packet, 0, sizeof(packet));
	header[0] = 1u;                          /* version */
	header[1] = QRTR_TYPE_RESUME_TX;
	header[2] = 1u;                          /* our node */
	header[3] = QRTR_OUR_PORT;
	header[4] = 0u;                          /* control traffic never asks for a confirm */
	header[5] = QRTR_CONTROL_WORDS * 4u;     /* the control packet is five words */
	header[6] = WLFW_NODE;
	header[7] = WLFW_PORT;
	control[0] = QRTR_TYPE_RESUME_TX;        /* the command */
	control[1] = 1u;                         /* the client whose stream this resumes: us */
	control[2] = QRTR_OUR_PORT;
	error = mhi_send_ipcr_packet(p, packet, (uint32_t)sizeof(packet), pcie_ipcr_wait,
	    UINT16_MAX);
	if (error != 0) {
		report(p, "the resume could not be sent, errno", (uint32_t)error);
		return error;
	}
	qrtr_resume_sent++;
	qrtr_resume_due = 0u;
	say("the firmware's transmit is resumed: %u answers sent so far", qrtr_resume_sent);
	return 0;
}

/*
 * The register space beyond 0x7ffff is reached through a window: write the window number to
 * +0x310c and the registers of that window appear at +0x80000.  Nothing else in the BAR moves,
 * so MHI and the doorbell registers stay where they were.
 */
static uint32_t
pcie_window_number(uint32_t offset)
{
	return (offset >> WINDOW_VALUE_SHIFT) & WINDOW_VALUE_MASK;
}

static void
pcie_window_select(struct pcie *p, uint32_t offset)
{
	pwr(p, BAR + WINDOW_REG, WINDOW_ENABLE | pcie_window_number(offset));
	(void)prd(p, BAR + WINDOW_REG);
}

static uint32_t
pcie_window_address(uint32_t offset)
{
	return BAR + WINDOW_START + (offset & WINDOW_RANGE_MASK);
}

/*
 * A register read/write anywhere in the endpoint's register space: direct below the window,
 * through the window above it.  The window number is written every time, which costs one extra
 * write but keeps the function free of state and safe from a device that resets the register.
 */
static uint32_t
pcie_reg_read(struct pcie *p, uint32_t offset)
{
	if (offset < WINDOW_START) {
		return prd(p, BAR + offset);
	}
	pcie_window_select(p, offset);
	return prd(p, pcie_window_address(offset));
}

static void
pcie_reg_write(struct pcie *p, uint32_t offset, uint32_t value)
{
	if (offset < WINDOW_START) {
		pwr(p, BAR + offset, value);
		return;
	}
	pcie_window_select(p, offset);
	pwr(p, pcie_window_address(offset), value);
}

/*
 * ath11k_pci_sw_reset(), the sequence Linux runs on every QCA6390 power up and power down
 * (commit f3c603d4).  Its own comment says why: "normal power up and power down can't bring MHI
 * to a workable state ... especially in warm reboot", and "after SOC_GLOBAL_RESET, MHISTATUS has
 * SYSERR bit set and thus need to set MHICTRL_RESET to clear SYSERR".  Ours had none of it, and
 * the SYSERR our logs keep showing (0xff05) is that bit.
 *
 * Register access goes through this driver's own model (pcie_reg_read/pcie_reg_write): below
 * 0x80000 direct, above it through the window.
 */
static unsigned pcie_syserr_clear = 1u;

static void
pcie_sw_reset(struct pcie *p)
{
	uint32_t value;

	say("sw reset: the soc global reset, the vectors, the latched syserr and the warm-entry cookie");

	value = pcie_reg_read(p, 0x3008u);
	pcie_reg_write(p, 0x3008u, value | 0x1u);
	pause_us(p, 10000);
	pcie_reg_write(p, 0x3008u, value & ~0x1u);
	pause_us(p, 10000);
	if (pcie_reg_read(p, 0x3008u) == UINT32_MAX) {
		say("sw reset: the link is down during the global reset");
	}

	/* the four vector registers, in the MHI bar below the window */
	pwr(p, BAR + 0x360u, 0u);
	pwr(p, BAR + 0x368u, 0u);
	pwr(p, BAR + 0x394u, 0u);
	pwr(p, BAR + 0x39cu, 0u);

	value = pcie_reg_read(p, 0x3008u);
	pcie_reg_write(p, 0x3008u, value | 0x1u);
	pause_us(p, 10000);
	pcie_reg_write(p, 0x3008u, value & ~0x1u);
	pause_us(p, 10000);

	/* the one that clears SYSERR */
	pwr(p, BAR + MHI_CONTROL, 0x2u);
	pause_us(p, 10000);
	say("sw reset: MHISTATUS %#x, MHICTRL %#x", prd(p, BAR + MHI_STATUS), prd(p, BAR + MHI_CONTROL));

	/* the Q6 continues its warm path if its entry word is left set */
	(void)pcie_reg_read(p, 0x1f80500u);
	(void)pcie_reg_read(p, 0x1f80504u);
	pause_us(p, 10000);
	pcie_reg_write(p, 0x1f80504u, 0u);
	pause_us(p, 10000);
	(void)pcie_reg_read(p, 0x1f80504u);
	(void)pcie_reg_read(p, 0x1f8060cu);

	/* wake the UMAC */
	pcie_reg_write(p, 0x3004u, 1u);
	pause_us(p, 5000);
}

/*
 * The host's copy-engine table, ath11k_host_ce_config_qca6390[] (ce.c:118): how many entries a
 * source ring has, the largest buffer it may carry, and how many entries the destination ring
 * has.  Engine 4 and engine 6 are the ones ath11k marks CE_ATTR_DIS_INTR; engine 6 and 8 carry
 * nothing at all.
 */
static const uint32_t wlfw_ce_host[CE_ENGINES][3] = {
	{ 16u,   2048u, 0u   },   /* 0: host to target, HTC control and raw streams */
	{ 0u,    2048u, 512u },   /* 1: target to host, HTT and HTC control */
	{ 0u,    2048u, 512u },   /* 2: target to host, WMI */
	{ 32u,   2048u, 0u   },   /* 3: host to target, WMI for the first MAC */
	{ 2048u, 256u,  0u   },   /* 4: host to target, HTT -- no interrupt */
	{ 0u,    2048u, 512u },   /* 5: target to host, pktlog */
	{ 0u,    0u,    0u   },   /* 6: the chip's own memory copy -- no interrupt */
	{ 32u,   2048u, 0u   },   /* 7: host to target, WMI for the second MAC */
	{ 0u,    0u,    0u   },   /* 8: the chip's own memory copy */
};

struct wlfw_ce_ring {
	struct dma desc;
	uint32_t type;          /* CE_RING_TYPE_* */
	uint32_t engine;
	uint32_t nentries;
	uint32_t entry_size;    /* dwords per descriptor */
	uint32_t ring_id;
	uint32_t max_buffer;    /* the destination ring's largest buffer */
	uint32_t sw_index;      /* the next descriptor the host will look at */
};

#define WLFW_CE_BUFFER_BYTES  2048u
#define WLFW_CE_SHADOW_BASE   0x000008fcu
#define WLFW_CE_SHADOW_WORDS  64u
#define WLFW_CE_RINGS_MAX  16u
static struct wlfw_ce_ring wlfw_ce_rings[WLFW_CE_RINGS_MAX];
static uint32_t wlfw_ce_ring_count;
static uint32_t wlfw_ce_ring_base(uint32_t type, uint32_t engine);
static uint32_t wlfw_ce_ring_hp(uint32_t type, uint32_t engine);
static struct dma wlfw_ce_rdp;         /* one u32 per ring id: where the chip leaves HP or TP */
#define WLFW_CE_POOL_RINGS     4u
#define WLFW_CE_BUFFERS_PER_RING 64u
static struct dma wlfw_ce_pool[WLFW_CE_POOL_RINGS];  /* a block per receive ring */
static uint32_t wlfw_ce_pool_count;

/*
 * The head and tail pointers of a ring live in the shadow registers when the chip
 * supports them: the host writes its own pointer to 0x8fc + 4 * index, and the chip
 * writes its pointer into the shared pointer block that was handed over at setup.
 */
static uint32_t
wlfw_ce_shadow_index(const struct wlfw_ce_ring *ring)
{
	for (uint32_t i = 0; i < wlfw_ce_ring_count; i++) {
		if (&wlfw_ce_rings[i] == ring) {
			return i;
		}
	}
	return WLFW_CE_SHADOW_WORDS;
}

static uint64_t
wlfw_ce_buffer(const struct dma *pool, uint32_t index)
{
	return kvtophys(pool->bytes + (uint64_t)index * WLFW_CE_BUFFER_BYTES);
}

/* Hand the chip every receive buffer a destination ring has room for. */
static void
wlfw_ce_fill_dest(struct pcie *p, struct wlfw_ce_ring *ring, const struct dma *pool)
{
	uint32_t base = wlfw_ce_ring_base(ring->type, ring->engine);
	uint32_t index, shadow;
	uint64_t address;

	for (index = 0; index < WLFW_CE_BUFFERS_PER_RING && index + 2u < ring->nentries; index++) {
		uint32_t at = index * ring->entry_size * 4u;
		uint32_t *desc = (uint32_t *)(ring->desc.bytes + at);

		/*
		 * The descriptor goes into the ring the chip was pointed at, at the offset of
		 * its slot -- not into the register window, which only ever holds the ring's
		 * base address and its pointers.
		 */
		address = wlfw_ce_buffer(pool, index);
		desc[0] = (uint32_t)address;
		desc[1] = (uint32_t)((address >> 32) & 0xffu);
	}
	dma_clean(&ring->desc);
	shadow = wlfw_ce_shadow_index(ring);
	if (shadow < WLFW_CE_SHADOW_WORDS) {
		/*
		 * The pointers are counted in dwords, not in descriptors: a ring whose
		 * descriptors are four dwords long advances four per message.
		 */
		pwr(p, BAR + WLFW_CE_SHADOW_BASE + shadow * 4u, index * ring->entry_size);
	}
	say("the receive ring of copy engine %u holds %u buffers of %u bytes, tail %u dwords",
	    ring->engine, index, WLFW_CE_BUFFER_BYTES, index * ring->entry_size);
}

static int
mhi_wlfw_ce_post_rx(struct pcie *p)
{
	for (uint32_t i = 0; i < wlfw_ce_ring_count; i++) {
		struct wlfw_ce_ring *ring = &wlfw_ce_rings[i];
		struct dma *pool;

		if (ring->type != CE_RING_TYPE_DST || ring->nentries <= 2u) {
			continue;
		}
		if (wlfw_ce_pool_count >= WLFW_CE_POOL_RINGS) {
			break;
		}
		pool = &wlfw_ce_pool[wlfw_ce_pool_count];
		if (!dma_alloc(pool, (uint64_t)WLFW_CE_BUFFERS_PER_RING * WLFW_CE_BUFFER_BYTES,
		    PTE_PAGE)) {
			say("THE RECEIVE BUFFER POOL FOR COPY ENGINE %u WENT NOWHERE", ring->engine);
			return ENOMEM;
		}
		dma_clean(pool);
		wlfw_ce_pool_count++;
		wlfw_ce_fill_dest(p, ring, pool);
	}
	if (wlfw_ce_pool_count == 0u) {
		say("NO RECEIVE RING TO FILL");
		return 0;
	}
	say("THE FIRMWARE HAS %u RECEIVE RINGS OF %u BUFFERS",
	    wlfw_ce_pool_count, WLFW_CE_BUFFERS_PER_RING);
	return 0;
}

/*
 * A source ring is filled the other way round: the host writes the descriptor at its
 * own head, then tells the chip where that head is by way of the shadow register.
 * Returns the number of buffers handed over.
 */
static uint32_t
wlfw_ce_post(struct pcie *p, struct wlfw_ce_ring *src, uint64_t address, uint32_t length,
    uint32_t transfer)
{
	uint32_t base = wlfw_ce_ring_base(src->type, src->engine);
	uint32_t slot = src->sw_index % src->nentries;
	uint32_t shadow = wlfw_ce_shadow_index(src);
	uint32_t at = slot * src->entry_size * 4u;

	if (src->sw_index + 1u >= src->nentries) {
		return 0u;
	}
	{
		uint32_t *desc = (uint32_t *)(src->desc.bytes + at);

		desc[0] = (uint32_t)address;
		desc[1] = (uint32_t)(((address >> 32) & 0xffu) | ((length & 0xffffu) << 16));
		desc[2] = transfer & 0xffffu;
		desc[3] = 0u;
		dma_clean(&src->desc);
	}
	src->sw_index = (slot + 1u) % src->nentries;
	if (shadow < WLFW_CE_SHADOW_WORDS) {
		pwr(p, BAR + WLFW_CE_SHADOW_BASE + shadow * 4u,
		    src->sw_index * src->entry_size);
	}
	return 1u;
}

static struct wlfw_ce_ring *
wlfw_ce_ring_of(uint32_t type, uint32_t engine)
{
	for (uint32_t i = 0; i < wlfw_ce_ring_count; i++) {
		if (wlfw_ce_rings[i].type == type && wlfw_ce_rings[i].engine == engine) {
			return &wlfw_ce_rings[i];
		}
	}
	return NULL;
}

/*
 * The first thing HTC expects is the host's hello on endpoint 0: a two-word HTC header
 * (endpoint in the low byte, payload length above it) followed by one word carrying the
 * message id and the host's own version.
 */
static int
mhi_wlfw_htc_hello(struct pcie *p)
{
	struct wlfw_ce_ring *src = wlfw_ce_ring_of(CE_RING_TYPE_SRC, 0u);
	struct dma packet;
	uint8_t *bytes;
	uint32_t length = 12u;

	if (src == NULL) {
		say("THERE IS NO COPY ENGINE 0 SOURCE RING TO SAY HELLO ON");
		return EINVAL;
	}
	if (!dma_alloc(&packet, length, PTE_PAGE)) {
		say("OUT OF MEMORY FOR THE HELLO");
		return ENOMEM;
	}
	bytes = (uint8_t *)packet.bytes;
	/* endpoint 0, payload of four bytes */
	bytes[0] = 0u;
	bytes[1] = 0u;
	bytes[2] = 4u;
	bytes[3] = 0u;
	/* sequence number 0 */
	bytes[4] = 0u;
	bytes[5] = 0u;
	bytes[6] = 0u;
	bytes[7] = 0u;
	/* message id 1 (setup), version 2 in the byte above it */
	bytes[8] = 1u;
	bytes[9] = 0u;
	bytes[10] = 2u;
	bytes[11] = 0u;
	dma_clean(&packet);
	if (wlfw_ce_post(p, src, kvtophys(packet.bytes), length, 0u) == 0u) {
		say("THE COPY ENGINE 0 RING HAD NO ROOM FOR THE HELLO");
		return ENOBUFS;
	}
	say("THE HELLO WENT OUT ON COPY ENGINE 0: %u bytes at %#llx", length,
	    (uint64_t)kvtophys(packet.bytes));
	return 0;
}

/*
 * Read whatever the chip has finished writing into a destination ring.  It counts
 * completions in the status ring, and the buffers come back in the order they were
 * handed over, so the host keeps its own cursor for both.
 */
static uint32_t
wlfw_ce_take(struct pcie *p, struct wlfw_ce_ring *dest)
{
	struct wlfw_ce_ring *status = NULL;
	uint32_t status_base, dest_base;
	uint32_t taken = 0u;
	uint32_t head, flags, length, slot, low, high;
	uint64_t address;

	for (uint32_t i = 0; i < wlfw_ce_ring_count; i++) {
		if (wlfw_ce_rings[i].type == CE_RING_TYPE_STATUS &&
		    wlfw_ce_rings[i].engine == dest->engine) {
			status = &wlfw_ce_rings[i];
			break;
		}
	}
	if (status == NULL) {
		return 0u;
	}
	status_base = wlfw_ce_ring_base(CE_RING_TYPE_STATUS, dest->engine);
	dest_base = wlfw_ce_ring_base(dest->type, dest->engine);
	dma_invalidate(&wlfw_ce_rdp);
	head = 0u;
	memcpy(&head, (void *)(wlfw_ce_rdp.bytes + (uint64_t)status->ring_id * 4u), 4u);
	/* The chip's head pointer is a dword count; the host keeps a descriptor count. */
	while ((status->sw_index + 1u) * status->entry_size <= head) {
		slot = status->sw_index % status->nentries;
		dma_invalidate(&status->desc);
		flags = *(const uint32_t *)(status->desc.bytes + slot * status->entry_size * 4u);
		length = flags >> 16;
		slot = dest->sw_index % dest->nentries;
		dma_invalidate(&dest->desc);
		low = *(const uint32_t *)(dest->desc.bytes + slot * dest->entry_size * 4u);
		high = *(const uint32_t *)(dest->desc.bytes + slot * dest->entry_size * 4u + 4u);
		address = (uint64_t)low | ((uint64_t)(high & 0xffu) << 32);
		say("copy engine %u: %u bytes came back at %#llx (status %#x, head %u dwords)",
		    dest->engine, length, address, flags, head);
		for (uint32_t pool = 0; pool < wlfw_ce_pool_count; pool++) {
			uint64_t base_address = kvtophys(wlfw_ce_pool[pool].bytes);
			uint64_t offset;

			if (length == 0u || address < base_address) {
				continue;
			}
			offset = address - base_address;
			if (offset + length > wlfw_ce_pool[pool].size) {
				continue;
			}
			dma_invalidate(&wlfw_ce_pool[pool]);
			{
				const uint8_t *packet =
				    (const uint8_t *)(wlfw_ce_pool[pool].bytes + offset);

				say("  it begins %02x %02x %02x %02x %02x %02x %02x %02x",
				    packet[0], packet[1], packet[2], packet[3],
				    packet[4], packet[5], packet[6], packet[7]);
			}
		}
		status->sw_index = (status->sw_index + 1u) % status->nentries;
		dest->sw_index = (dest->sw_index + 1u) % dest->nentries;
		taken++;
	}
	return taken;
}

static bool wlfw_ce_ready;

/*
 * Which registers the firmware is to keep a shadow copy of.  ath11k builds this table from the
 * rings it has made (ath11k_ce_shadow_config, ce.c:821): a source ring contributes its head
 * pointer register, a destination ring its tail pointer -- four bytes further on.  With the host
 * table that is ten entries, not the thirty this driver used to send.  pcie_shadow=N asks for N
 * of the derived entries; pcie_shadow=30 keeps the old table; pcie_shadow=0 leaves the table out.
 */
static uint32_t pcie_shadow = 1u;       /* pcie_shadow=N: how many shadow registers to send */
static uint32_t wlfw_shadow_count;
static uint32_t wlfw_shadow_ce[WLFW_CE_RINGS_MAX];

static uint32_t
wlfw_ce_roundup(uint32_t value)
{
	uint32_t power = 1u;

	while (power < value) {
		power <<= 1;
	}
	return power;
}

/*
 * The head or tail pointer register of the ring a host table entry stands for, in the order
 * ath11k_ce_shadow_config() walks the engines: engine 0's source ring, then each engine's
 * destination ring followed by its destination-status ring, then the next engine.
 */
static uint32_t wlfw_ce_ring_hp(uint32_t type, uint32_t engine);

static uint32_t
wlfw_ce_shadow_count(void)
{
	uint32_t count = 0;

	for (uint32_t engine = 0; engine < CE_ENGINES; engine++) {
		if (wlfw_ce_host[engine][CE_HOST_SRC_ENTRIES] != 0u) {
			count++;
		}
		if (wlfw_ce_host[engine][CE_HOST_DST_ENTRIES] != 0u) {
			count += 2u;
		}
	}
	return count;
}

static void
wlfw_ce_shadow_fill(void)
{
	uint32_t at = 0;

	for (uint32_t engine = 0; engine < CE_ENGINES; engine++) {
		if (wlfw_ce_host[engine][CE_HOST_SRC_ENTRIES] != 0u) {
			wlfw_shadow_ce[at++] = wlfw_ce_ring_hp(CE_RING_TYPE_SRC, engine);
		}
		if (wlfw_ce_host[engine][CE_HOST_DST_ENTRIES] != 0u) {
			/* A destination ring's shadow register is its tail pointer: head plus four. */
			wlfw_shadow_ce[at++] = wlfw_ce_ring_hp(CE_RING_TYPE_DST, engine) + 4u;
			wlfw_shadow_ce[at++] = wlfw_ce_ring_hp(CE_RING_TYPE_STATUS, engine);
		}
	}
}

static uint32_t
wlfw_ce_ring_base(uint32_t type, uint32_t engine)
{
	if (type == CE_RING_TYPE_SRC) {
		return CE_SRC_BASE + CE_STRIDE * engine;
	}
	if (type == CE_RING_TYPE_DST) {
		return CE_DST_BASE + CE_STRIDE * engine;
	}
	return CE_DST_BASE + CE_STATUS_BASE + CE_STRIDE * engine;
}

static uint32_t
wlfw_ce_ring_hp(uint32_t type, uint32_t engine)
{
	if (type == CE_RING_TYPE_STATUS) {
		return CE_DST_BASE + CE_STATUS_HP + CE_STRIDE * engine;
	}
	return (type == CE_RING_TYPE_SRC ? CE_SRC_BASE : CE_DST_BASE) + SRNG_REG_HP +
	    CE_STRIDE * engine;
}

/*
 * One ring, set up the way ath11k_hal_srng_src_hw_init() (hal.c:347) and
 * ath11k_hal_srng_dst_hw_init() (hal.c:271) do it.  The source and destination rings are both
 * source rings as far as HAL is concerned -- the host writes their descriptors -- so they get
 * the tail pointer address and the consumer interrupt setup; the destination-status ring is a
 * destination ring and gets the head pointer address and the producer interrupt setup.  Every
 * ring gets the enable bit, and a source ring also gets the loop-count-disable bit.
 */
static int
wlfw_ce_ring_setup(struct pcie *p, struct wlfw_ce_ring *ring)
{
	uint32_t r0 = wlfw_ce_ring_base(ring->type, ring->engine);
	uint32_t hp = wlfw_ce_ring_hp(ring->type, ring->engine);
	uint32_t bytes = ring->nentries * ring->entry_size * 4u;
	uint64_t rdpt = (uint64_t)wlfw_ce_rdp.address + (uint64_t)ring->ring_id;
	uint32_t misc = SRNG_MISC_SRNG_ENABLE;
	uint64_t address;

	if (!dma_alloc(&ring->desc, bytes, PTE_PAGE)) {
		say("OUT OF MEMORY FOR THE COPY ENGINE %u's RING OF %u ENTRIES", ring->engine,
		    ring->nentries);
		return ENOMEM;
	}
	dma_clean(&ring->desc);
	address = ring->desc.address;

	/* The chip reads the ring through the SMMU, so the address it is given is the IOVA. */
	pcie_reg_write(p, r0 + SRNG_R0_BASE_LSB, (uint32_t)(address & 0xffffffffull));
	pcie_reg_write(p, r0 + SRNG_R0_BASE_MSB,
	    (uint32_t)((address >> 32) & 0xffull) |
	    (uint32_t)(((uint32_t)(ring->entry_size * ring->nentries) & 0xfffffu) << 8));
	if (ring->type == CE_RING_TYPE_STATUS) {
		pcie_reg_write(p, r0 + SRNG_R0_RING_ID,
		    ((ring->ring_id & 0xffu) << 8) | (ring->entry_size & 0xffu));
		pcie_reg_write(p, r0 + SRNG_R0_PRDR_INT_SETUP, 0u);
		pcie_reg_write(p, r0 + SRNG_R0_HP_ADDR_LSB, (uint32_t)(rdpt & 0xffffffffull));
		pcie_reg_write(p, r0 + SRNG_R0_HP_ADDR_MSB, (uint32_t)(rdpt >> 32));
	} else {
		/* A source ring writes only the descriptor size here, not the ring id. */
		pcie_reg_write(p, r0 + SRNG_R0_RING_ID, ring->entry_size & 0xffu);
		pcie_reg_write(p, r0 + SRNG_R0_CONSR_INT_IX0, 0u);
		pcie_reg_write(p, r0 + SRNG_R0_CONSR_INT_IX1, 0u);
		pcie_reg_write(p, r0 + SRNG_R0_TP_ADDR_LSB, (uint32_t)(rdpt & 0xffffffffull));
		pcie_reg_write(p, r0 + SRNG_R0_TP_ADDR_MSB, (uint32_t)(rdpt >> 32));
		misc |= SRNG_MISC_LOOPCNT_DIS;
	}
	/* The second group holds the head and tail of an empty ring. */
	pcie_reg_write(p, hp, 0u);
	pcie_reg_write(p, hp + SRNG_REG_TP_DELTA, 0u);
	if (ring->type == CE_RING_TYPE_DST && ring->max_buffer != 0u) {
		uint32_t ctrl = pcie_reg_read(p, r0 + CE_DEST_CTRL);

		ctrl &= ~0xffffu;
		ctrl |= ring->max_buffer & 0xffffu;
		pcie_reg_write(p, r0 + CE_DEST_CTRL, ctrl);
	}
	pcie_reg_write(p, r0 + SRNG_R0_MISC, misc);

	say("  copy engine %u, %s ring %u: %u entries of %u bytes at %#llx, head pointer "
	    "register %#x, control %#x", ring->engine,
	    ring->type == CE_RING_TYPE_SRC ? "source" :
	    (ring->type == CE_RING_TYPE_DST ? "destination" : "destination-status"),
	    ring->ring_id, ring->nentries, ring->entry_size * 4u, address, hp,
	    pcie_reg_read(p, r0 + SRNG_R0_MISC));
	return 0;
}

static int
wlfw_ce_ring_add(struct wlfw_ce_ring *ring, uint32_t type, uint32_t engine, uint32_t nentries,
    uint32_t entry_size, uint32_t ring_id, uint32_t max_buffer)
{
	if (wlfw_ce_ring_count >= WLFW_CE_RINGS_MAX) {
		return ENOSPC;
	}
	ring = &wlfw_ce_rings[wlfw_ce_ring_count++];
	ring->type = type;
	ring->engine = engine;
	ring->nentries = wlfw_ce_roundup(nentries);
	ring->entry_size = entry_size;
	ring->ring_id = ring_id;
	ring->max_buffer = max_buffer;
	return 0;
}

/*
 * Stage 21's other half.  ath11k sets the copy engines up in ath11k_ce_init_pipes() (ce.c:914)
 * right after the mode request comes back, and the firmware faults if it cannot find them; this
 * driver has never written a single one of those registers, so they read as zeros or as whatever
 * the chip left there.  Every register is written through the sliding window at +0x310c.
 */
static int
mhi_wlfw_ce_init(struct pcie *p)
{
	int error;

	if (wlfw_ce_ready) {
		return 0;
	}
	if (!dma_alloc(&wlfw_ce_rdp, CE_RING_ID_MAX * 4u, PTE_PAGE)) {
		say("OUT OF MEMORY FOR THE RING POINTERS THE CHIP WRITES");
		return ENOMEM;
	}
	dma_clean(&wlfw_ce_rdp);
	wlfw_ce_ring_count = 0;
	for (uint32_t engine = 0; engine < CE_ENGINES; engine++) {
		if (wlfw_ce_host[engine][CE_HOST_SRC_ENTRIES] != 0u &&
		    (error = wlfw_ce_ring_add(NULL, CE_RING_TYPE_SRC, engine,
		    wlfw_ce_host[engine][CE_HOST_SRC_ENTRIES], CE_SRC_ENTRY_SIZE,
		    CE_RING_ID_SRC0 + engine, 0u)) != 0) {
			return error;
		}
		if (wlfw_ce_host[engine][CE_HOST_DST_ENTRIES] != 0u) {
			if ((error = wlfw_ce_ring_add(NULL, CE_RING_TYPE_DST, engine,
			    wlfw_ce_host[engine][CE_HOST_DST_ENTRIES], CE_DST_ENTRY_SIZE,
			    CE_RING_ID_DST0 + engine, wlfw_ce_host[engine][CE_HOST_SRC_MAX])) != 0 ||
			    (error = wlfw_ce_ring_add(NULL, CE_RING_TYPE_STATUS, engine,
			    wlfw_ce_host[engine][CE_HOST_DST_ENTRIES], CE_STATUS_ENTRY_SIZE,
			    CE_RING_ID_STATUS0 + engine, 0u)) != 0) {
				return error;
			}
		}
	}
	say("the copy engines: %u rings to set up, their pointers at %#llx",
	    wlfw_ce_ring_count, wlfw_ce_rdp.address);
	for (uint32_t i = 0; i < wlfw_ce_ring_count; i++) {
		if ((error = wlfw_ce_ring_setup(p, &wlfw_ce_rings[i])) != 0) {
			return error;
		}
	}
	wlfw_ce_ready = true;
	say("THE COPY ENGINES ARE SET UP: %u rings, and the mode request is next",
	    wlfw_ce_ring_count);
	return 0;
}


/*
 * Which offsets of the endpoint's register space actually decode?  Below 0x80000 the BAR answers
 * directly; above it the answer depends on the window, so the window is selected first.  A range
 * that does not exist reads 0xffffffff.  Everything here is a read.
 */
static void
mhi_wlfw_window_scan(struct pcie *p)
{
	static const uint32_t offsets[] = {
		0x00000000u, 0x00010000u, 0x0007ff00u, 0x00080000u, 0x000a0000u,
		0x00a00000u, 0x00a01000u, 0x01b80000u, 0x01f80000u,
	};
	uint32_t decoded = 0;

	say("%s: %u of them, one word each; the ones above +0x80000 go through the window",
	    "mhi_wlfw_window_scan says the endpoint answers at these offsets",
	    (uint32_t)(sizeof(offsets) / sizeof(offsets[0])));
	for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
		uint32_t offset = offsets[i];
		uint32_t value;

		if (offset >= WINDOW_START) {
			pcie_window_select(p, offset);
			value = prd(p, pcie_window_address(offset));
		} else {
			value = prd(p, BAR + offset);
		}
		if (value != UINT32_MAX) {
			decoded++;
		}
		say("  +%#010x (window %#x): %#x%s", offset, pcie_window_number(offset), value,
		    value == UINT32_MAX ? "  (nothing there)" : "");
	}
	say("%u of the endpoint's offsets answered", decoded);
}

/*
 * How big is the endpoint's BAR really?  Writing all ones to the BAR register and reading it back
 * leaves the size mask: the lowest set bit is the size.  The written value is put back at once.
 * This is asked here because the copy engines live at +0xa00000 and a 1 MiB BAR could not hold
 * them.
 */
static void
mhi_wlfw_bar_size(struct pcie *p)
{
	uint32_t saved = prd(p, CONF + 0x10u);
	uint32_t mask;

	pwr(p, CONF + 0x10u, 0xfffffff0u);
	mask = prd(p, CONF + 0x10u);
	pwr(p, CONF + 0x10u, saved);
	report(p, "the BAR's size mask, read back", mask);
	if (mask == UINT32_MAX || mask == 0) {
		say("THE BAR REGISTER DOES NOT ANSWER A SIZE QUERY, so the window is whatever was programmed");
		return;
	}
	say("the endpoint's BAR is %u bytes: the lowest set bit of %#x",
	    (mask & ~0xfu) & (0u - (mask & ~0xfu)), mask);
	if (((mask & ~0xfu) & (0u - (mask & ~0xfu))) <= CE_SRC_BASE) {
		say("the BAR is one MiB, which is the design: +0x80000 and above is the "
		    "sliding window that reaches the copy engines at +%#x", CE_SRC_BASE);
	}
}

/*
 * Stage 21's question, now with the window in hand.  ath11k hands the firmware nine copy engines
 * before it asks for a mode: ath11k_ce_init_pipes() (ce.c:914) sets up each engine's source ring,
 * destination ring and destination-status ring, writing their base addresses into the endpoint
 * (CE0_SRC is 0xa00000 in qca6390_regs) and giving the firmware the matching HP registers through
 * the shadow table.  This driver has never written any of that, so the question is what those
 * registers hold now -- the firmware may have set some of them up itself before the mode request.
 *
 * All of it is read through the sliding window at +0x310c; nothing is written.
 */
static void
mhi_wlfw_ce_survey(struct pcie *p)
{
	uint32_t src = pcie_reg_read(p, CE_SRC_BASE + 0x00u);
	uint32_t srchp = pcie_reg_read(p, CE_SRC_BASE + 0x400u);
	uint32_t dst = pcie_reg_read(p, CE_DST_BASE + 0x00u);
	uint32_t dsthp = pcie_reg_read(p, CE_DST_BASE + 0x400u);
	uint32_t status = pcie_reg_read(p, CE_DST_BASE + CE_STATUS_BASE);
	uint32_t ctrl = pcie_reg_read(p, CE_DST_BASE + CE_DEST_CTRL);

	say("THE SLIDING WINDOW AT +0x310C REACHES THE COPY ENGINES: engine 0 source base %#x, "
	    "source hp %#x, destination base %#x, destination hp %#x", src, srchp, dst, dsthp);
	say("  engine 0 status base %#x, destination control %#x", status, ctrl);
	if (src == UINT32_MAX && srchp == UINT32_MAX && dst == UINT32_MAX && status == UINT32_MAX) {
		say("THE CE WINDOW DOES NOT DECODE EVEN THROUGH THE WINDOW: every register from +%#x "
		    "reads 0xffffffff", CE_SRC_BASE);
		return;
	}
	for (unsigned ce = 0; ce < 9u; ce++) {
		uint32_t src_at = CE_SRC_BASE + ce * CE_STRIDE;
		uint32_t dst_at = CE_DST_BASE + ce * CE_STRIDE;

		say("  engine %u source +%#x: base %#x msb %#x id %#x misc %#x hp %#x", ce, src_at,
		    pcie_reg_read(p, src_at + 0x00u), pcie_reg_read(p, src_at + 0x04u),
		    pcie_reg_read(p, src_at + 0x08u), pcie_reg_read(p, src_at + 0x10u),
		    pcie_reg_read(p, src_at + 0x400u));
		say("  engine %u destination +%#x: base %#x msb %#x id %#x misc %#x hp %#x", ce, dst_at,
		    pcie_reg_read(p, dst_at + 0x00u), pcie_reg_read(p, dst_at + 0x04u),
		    pcie_reg_read(p, dst_at + 0x08u), pcie_reg_read(p, dst_at + 0x10u),
		    pcie_reg_read(p, dst_at + 0x400u));
		say("  engine %u status +%#x: base %#x msb %#x id %#x hp %#x", ce,
		    dst_at + CE_STATUS_BASE, pcie_reg_read(p, dst_at + CE_STATUS_BASE),
		    pcie_reg_read(p, dst_at + CE_STATUS_BASE + 0x04u),
		    pcie_reg_read(p, dst_at + CE_STATUS_BASE + 0x08u),
		    pcie_reg_read(p, dst_at + CE_STATUS_HP));
	}
	say("the copy engines answer through the window; nothing has been written to them");
}

/*
 * One QRTR datagram to the firmware: an eight-word header and the payload after it.  The header
 * is Linux's (include/uapi/linux/qrtr.h): version, type, from_node, from_port, confirm, size,
 * to_node, to_port -- all little-endian 32-bit words, with `size` counting the payload only.
 */
static int
mhi_qrtr_send_from(struct pcie *p, uint32_t from_port, uint32_t to_node, uint32_t to_port,
    const void *payload, uint32_t length)
{
	static uint8_t packet[QRTR_HEADER_WORDS * 4u + IPCR_BUFFER_SIZE];
	uint32_t *header = (uint32_t *)(void *)packet;
	uint32_t padded;
	int error;

	/*
	 * The ceiling is the receive buffer size, not a round number: a board file goes down in
	 * pieces of 6144 bytes and there is no reason for the datagram that carries one to be
	 * smaller than the datagrams the firmware sends us.  It used to be 1024, which refused
	 * the first piece of the first board file with EINVAL.
	 */
	if (length == 0u || length > IPCR_BUFFER_SIZE) {
		return EINVAL;
	}
	/* Nothing is in flight here, so this is where a pending resume is answered. */
	if (qrtr_resume_due != 0u) {
		(void)mhi_qrtr_resume_tx(p);
	}
	memset(packet, 0, sizeof(packet));
	header[0] = 1u;                          /* version */
	header[1] = QRTR_TYPE_DATA;
	header[2] = 1u;                          /* our node */
	header[3] = from_port;
	header[4] = 0u;                          /* confirm */
	header[5] = length;                      /* the payload's own length */
	header[6] = to_node;
	header[7] = to_port;
	memcpy(packet + QRTR_HEADER_WORDS * 4u, payload, length);
	/*
	 * Linux pads the payload to four bytes on the way out (qrtr_node_enqueue:
	 * skb_put_padto(skb, ALIGN(len, 4) + sizeof(*hdr))) while the header's size keeps the
	 * unpadded length.  The firmware's parser reads with the same rule, so this is not
	 * cosmetic: a datagram of 66 bytes is one it will not look at.  `packet` is zeroed
	 * above, so the padding is already there -- only the length handed to the ring changes.
	 */
	padded = (length + 3u) & ~3u;
	error = mhi_send_ipcr_packet(p, packet, QRTR_HEADER_WORDS * 4u + padded, pcie_ipcr_wait,
	    UINT16_MAX);
	if (error != 0) {
		return error;
	}
	return 0;
}

/* The hand-written stages' send: the one port the firmware's own exchange began on. */
static int
mhi_qrtr_send(struct pcie *p, uint32_t to_node, uint32_t to_port, const void *payload,
    uint32_t length)
{
	return mhi_qrtr_send_from(p, QRTR_OUR_PORT, to_node, to_port, payload, length);
}

/*
 * The QMI WLFW host-capability request, byte for byte what ath11k sends: a seven-byte QMI header
 * (request, transaction 1, message 0x0034, length of what follows) and then TLVs of type, length
 * and value.  Every field ath11k fills in is here, with its number from
 * qmi_wlanfw_host_cap_req_msg_v01_ei[] (ath11k/qmi.c:33) beside it:
 *
 *   0x10 num_clients = 1                     -- one client, us
 *   0x13 nm_modem    = 0x1e                  -- non-Qualcomm host (0x04), internal sleep clock
 *                                               (0x02), PCIe global reset (0x08), D3cold PME (0x10)
 *   0x14 bdf_support = 1                     -- we will send a board file
 *   0x16 m3_support  = 1                     -- we will send the firmware's M3 segment
 *   0x17 m3_cache_support = 1                -- and it may be cached
 *   0x1a cal_done    = 0                     -- the firmware has not been told calibration is done
 *   0x1c mem_cfg_mode = 0                    -- fixed memory regions, which is QCA6390's mode
 *
 * The bits of nm_modem are ath11k/qmi.c:21-24.  qwlanfw does not mind their order; ath11k sends
 * them in the order of its element table, and so does this.
 */
static uint32_t
mhi_wlfw_host_cap_request(uint8_t *buffer, uint32_t size)
{
	static const struct {
		uint8_t type, bytes;
		uint32_t value;
	} tlvs[] = {
		{ 0x10, 4, 1u },                 /* num_clients */
		{ 0x13, 1, 0x1eu },              /* nm_modem */
		{ 0x14, 1, 1u },                 /* bdf_support */
		{ 0x16, 1, 1u },                 /* m3_support */
		{ 0x17, 1, 1u },                 /* m3_cache_support */
		{ 0x1a, 1, 0u },                 /* cal_done */
		{ 0x1c, 1, 0u },                 /* mem_cfg_mode */
	};
	uint32_t at = QMI_HEADER_SIZE;

	if (at > size) {
		return 0;
	}
	for (unsigned i = 0; i < sizeof(tlvs) / sizeof(tlvs[0]); i++) {
		if (at + 3u + tlvs[i].bytes > size) {
			return 0;
		}
		buffer[at] = tlvs[i].type;
		buffer[at + 1] = tlvs[i].bytes;
		buffer[at + 2] = 0u;
		for (uint8_t b = 0; b < tlvs[i].bytes; b++) {
			buffer[at + 3u + b] = (uint8_t)(tlvs[i].value >> (8u * b));
		}
		at += 3u + tlvs[i].bytes;
	}
	buffer[0] = QMI_REQUEST;
	buffer[1] = 1u;                          /* transaction */
	buffer[2] = 0u;
	buffer[3] = (uint8_t)QMI_WLANFW_HOST_CAP_REQ;
	buffer[4] = (uint8_t)(QMI_WLANFW_HOST_CAP_REQ >> 8);
	buffer[5] = (uint8_t)(at - QMI_HEADER_SIZE);
	buffer[6] = (uint8_t)((at - QMI_HEADER_SIZE) >> 8);
	return at;
}

/* One wait for a datagram from the firmware; defined below, next to the stage that uses it. */
static bool
mhi_qrtr_wait(struct pcie *p, uint32_t wait_ms);

/* Take the oldest datagram out of the queue; false when there is none. */
static bool
mhi_qrtr_take(struct pcie *p, uint8_t *data, uint32_t size, uint32_t *length);

/* Act on an indication the firmware volunteers; true when we knew what to do with it. */
static bool
mhi_qmi_indication(struct pcie *p, uint32_t answer, const uint8_t *reply, uint32_t length);

/* Act on every indication already waiting in the receive queue. */
static void
mhi_qmi_drain(struct pcie *p);

/*
 * Say what the firmware's memory request contains -- message 0x0035, REQUEST_MEM_IND.  Its body
 * is the wire encoding of ath11k's qmi_wlanfw_request_mem_ind_msg_v01 (qmi.h:266): a segment
 * count, then that many qmi_wlanfw_mem_seg_s_v01 entries, each a size, a type and its own
 * configuration.  The body is dumped word by word, because the reply we owe it has to match the
 * encoding exactly and guessing at an encoder is how a conversation stalls.
 */

/*
 * The answer to the memory request: message 0x0036, qmi_wlanfw_respond_mem_req_msg_v01.
 * Its body is the same two-layer shape as the request we just read -- a segment count, then
 * one entry per segment -- but the entry is qmi_wlanfw_mem_seg_resp_s_v01, whose fields the
 * element table (ath11k/qmi.c:663) writes as addr, size, type, restore: 8 + 4 + 4 + 1 bytes
 * packed.  `want` and `count` are what the firmware asked for; when we cannot hand it the
 * memory, the answer carries no segments at all, which is ath11k's own "delay" answer.
 */
static uint32_t
mhi_wlfw_mem_response(uint8_t *buffer, uint32_t size)
{
	uint32_t at = QMI_HEADER_SIZE;
	uint32_t tlv = at;

	if (at + 3u + 1u > size || wlfw_mem.count > WLFW_MEM_SEGMENTS_MAX) {
		return 0;
	}
	/*
	 * One TLV holds both the count and the segments, because that is what qmi_encode() does:
	 * a QMI_DATA_LEN element is written inside the TLV opened for the array that follows it,
	 * and leaves that TLV's header to the array.  The firmware's own memory request has this
	 * shape -- `01 13 00 | 02 | segments`.  Writing the count in a TLV of its own is refused.
	 */
	at += 3u;                        /* TLV header, filled in once the body length is known */
	buffer[at++] = (uint8_t)wlfw_mem.count;
	for (uint32_t i = 0; i < wlfw_mem.count; i++) {
		/*
		 * The radio's address by default; the host's physical address when pcie_mem_phys=1.
		 * ath11k answers with dma_alloc_coherent()'s paddr, which is the physical address, and
		 * this is the first address the firmware keeps and reuses after the handshake.
		 */
		uint64_t address = pcie_mem_phys != 0u
		    ? (uint64_t)kvtophys(wlfw_mem.segment[i].bytes)
		    : wlfw_mem.segment[i].address;
		uint32_t bytes = wlfw_mem.size[i];
		uint32_t type = wlfw_mem.type[i];

		if (at + WLFW_MEM_SEGMENT_BYTES > size) {
			return 0;
		}
		for (unsigned b = 0; b < 8u; b++) {
			buffer[at + b] = (uint8_t)(address >> (8u * b));
		}
		for (unsigned b = 0; b < 4u; b++) {
			buffer[at + 8u + b] = (uint8_t)(bytes >> (8u * b));
		}
		for (unsigned b = 0; b < 4u; b++) {
			buffer[at + 12u + b] = (uint8_t)(type >> (8u * b));
		}
		buffer[at + 16u] = 0u;          /* restore */
		at += WLFW_MEM_SEGMENT_BYTES;
	}
	buffer[tlv] = 0x01u;
	buffer[tlv + 1] = (uint8_t)((at - tlv - 3u) & 0xffu);
	buffer[tlv + 2] = (uint8_t)(((at - tlv - 3u) >> 8) & 0xffu);
	buffer[0] = QMI_REQUEST;
	buffer[1] = 3u;                          /* transaction */
	buffer[2] = 0u;
	buffer[3] = (uint8_t)QMI_WLANFW_RESPOND_MEM_REQ;
	buffer[4] = (uint8_t)(QMI_WLANFW_RESPOND_MEM_REQ >> 8);
	buffer[5] = (uint8_t)((at - QMI_HEADER_SIZE) & 0xffu);
	buffer[6] = (uint8_t)(((at - QMI_HEADER_SIZE) >> 8) & 0xffu);
	return at;
}

/*
 * Read the firmware's memory request into `wlfw_mem` and say what it asked for.  The body is
 * one TLV: a one-byte segment count, then qmi_wlanfw_mem_seg_s_v01 entries of size(u32),
 * type(4-byte enum), mem_cfg_len(u8) and that many 13-byte configurations.
 */
static uint32_t
mhi_wlfw_parse_mem_request(struct pcie *p, const uint8_t *reply, uint32_t length)
{
	uint32_t at = QMI_HEADER_SIZE;
	uint32_t tlv, body, count, size, type, configs;

	wlfw_mem.count = 0;
	if (at + 3u > length || reply[at] != 0x01u) {
		say("THE MEMORY REQUEST DOES NOT BEGIN WITH A SEGMENT TLV: its first byte is %#x",
		    at < length ? reply[at] : 0u);
		return 0;
	}
	tlv = (uint32_t)reply[at + 1] | (uint32_t)reply[at + 2] << 8;
	body = at + 3u;
	if (body + 1u > length) {
		say("THE MEMORY REQUEST'S TLV IS EMPTY");
		return 0;
	}
	count = reply[body];
	report(p, "the memory request's segment count", count);
	report(p, "the memory request's TLV length", tlv);
	at = body + 1u;
	for (uint32_t i = 0; i < count && i < WLFW_MEM_SEGMENTS_MAX; i++) {
		if (at + 9u > length) {
			say("THE MEMORY REQUEST ENDS IN THE MIDDLE OF SEGMENT %u", i);
			break;
		}
		size = (uint32_t)reply[at] | (uint32_t)reply[at + 1] << 8 |
		    (uint32_t)reply[at + 2] << 16 | (uint32_t)reply[at + 3] << 24;
		type = (uint32_t)reply[at + 4] | (uint32_t)reply[at + 5] << 8 |
		    (uint32_t)reply[at + 6] << 16 | (uint32_t)reply[at + 7] << 24;
		configs = reply[at + 8];
		report(p, "a segment's size", size);
		report(p, "a segment's type", type);
		report(p, "a segment's configuration count", configs);
		wlfw_mem.size[wlfw_mem.count] = size;
		wlfw_mem.type[wlfw_mem.count] = type;
		wlfw_mem.count++;
		at += 9u + configs * 13u;
	}
	return wlfw_mem.count;
}

/*
 * Give the firmware the addresses it asked for.  Each segment is one contiguous run in the
 * radio's window; the size we report is the size it asked for, not the page-rounded allocation.
 */
static bool
mhi_wlfw_allocate_mem(struct pcie *p)
{
	uint64_t total = 0;

	for (uint32_t i = 0; i < wlfw_mem.count; i++) {
		bool placed;

		if (pcie_fixed_iova != 0u && wlfw_mem.type[i] == 1u) {
			placed = dma_alloc_at(&wlfw_mem.segment[i], wlfw_mem.size[i], PTE_PAGE,
			    QCA_DDR_IOVA);
		} else if (pcie_fixed_iova != 0u && wlfw_mem.type[i] == 4u) {
			placed = dma_alloc_at(&wlfw_mem.segment[i], wlfw_mem.size[i], PTE_PAGE,
			    QCA_CAL_IOVA);
		} else {
			placed = dma_alloc(&wlfw_mem.segment[i], wlfw_mem.size[i], PTE_PAGE);
		}
		if (!placed) {
			say("SEGMENT %u OF THE MEMORY REQUEST COULD NOT BE ALLOCATED: %u bytes", i,
			    wlfw_mem.size[i]);
			return false;
		}
		say("segment %u: %u bytes at %#llx; the radio sees it at %#llx, its own address is %#llx",
		    i, wlfw_mem.size[i], wlfw_mem.segment[i].bytes, wlfw_mem.segment[i].address,
		    (uint64_t)kvtophys(wlfw_mem.segment[i].bytes));
		total += wlfw_mem.size[i];
	}
	report(p, "the memory the firmware asked for, in bytes", (uint32_t)total);
	wlfw_mem.on = true;
	return true;
}

/*
 * What the firmware left in its DDR segment.
 *
 * The logging switch sent in stage 21 is enablefwlog=1, and the DDR segment the firmware
 * asked for (type 1) is where it keeps its own account of what it is doing.  Nothing has
 * ever read that segment back.  Two things come out of reading it now: whether the radio
 * can reach the address at all (a segment of zeroes says no), and, if it can, the lines
 * it wrote about the mode request it refused or the death that followed it.
 *
 * Called once the ring has stopped mattering, so nothing is racing this read.
 */
static void
mhi_wlfw_dump_log(struct pcie *p)
{
	struct dma *segment = NULL;
	uint32_t size = 0, written = 0, found = 0;

	for (uint32_t i = 0; i < wlfw_mem.count; i++) {
		if (wlfw_mem.type[i] == 1u) {
			segment = &wlfw_mem.segment[i];
			size = wlfw_mem.size[i];
			break;
		}
	}
	if (segment == NULL) {
		say("THERE IS NO DDR SEGMENT TO READ");
		return;
	}
	if (size > (8u << 20)) {
		size = 8u << 20;
	}
	dma_invalidate(segment);

	for (uint32_t i = 0; i < size; i++) {
		if (((const uint8_t *)segment->bytes)[i] != 0u) {
			written++;
		}
	}
	say("the DDR segment is %u bytes at %#llx; %u of them are not zero", size,
	    segment->bytes, written);
	if (written == 0u) {
		say("THE FIRMWARE HAS NOT WRITTEN INTO THE MEMORY WE GAVE IT: the address it "
		    "was answered with is one it cannot reach");
		return;
	}

	/* the first bytes, so the shape of what it writes is visible even if unprintable */
	for (uint32_t at = 0; at < 512u && at < size; at += 16u) {
		const uint8_t *line = (const uint8_t *)segment->bytes + at;

		say("%04x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
		    "%02x %02x %02x", at, line[0], line[1], line[2], line[3], line[4], line[5],
		    line[6], line[7], line[8], line[9], line[10], line[11], line[12], line[13],
		    line[14], line[15]);
	}

	/* then every printable run in it, which is how a log looks once it is text */
	for (uint32_t at = 0; at < size; at++) {
		const uint8_t *line = (const uint8_t *)segment->bytes + at;
		uint32_t length = 0;

		while (at + length < size && length < 190u) {
			uint8_t c = line[length];

			if (c < 0x20u && c != 0x09u) {
				break;
			}
			if (c > 0x7eu) {
				break;
			}
			length++;
		}
		if (length >= 8u) {
			char text[191];

			if (found < 200u) {
				for (uint32_t i = 0; i < length; i++) {
					text[i] = (char)line[i];
				}
				text[length] = '\0';
				say("the firmware says: %s", text);
				found++;
			}
			at += length;
			continue;
		}
		at += length;
	}
	say("the firmware's log: %u lines read out of the DDR segment", found);
}

/* The reader a captured reply is handed to; see mhi_qmi_request_capture(). */
typedef void (*mhi_qmi_reader)(struct pcie *p, const uint8_t *reply, uint32_t length);

/*
 * One QMI message out, and its whole answer handed to `reader` once the result TLV
 * says the firmware accepted it.  This is mhi_qmi_request()'s loop with the tail
 * swapped: the messages that carry news in their later TLVs (the chip's identity,
 * the memory it wants) cannot be read by looking only at the result.
 */
static int
mhi_qmi_request_capture(struct pcie *p, uint16_t message, const uint8_t *request,
    uint32_t length, const char *what, mhi_qmi_reader reader)
{
	uint8_t reply[1024];
	uint32_t type, answer, at, waited, reply_length;
	int error;

	error = mhi_qrtr_send(p, WLFW_NODE, WLFW_PORT, request, length);
	if (error != 0) {
		report(p, "the request could not be sent, errno", (uint32_t)error);
		return error;
	}
	for (waited = 0; waited < WLFW_REPLY_WAIT; waited += WLFW_PACKET_WAIT) {
		if (!mhi_qrtr_wait(p, WLFW_PACKET_WAIT) && ipcr.rx_head == ipcr.rx_tail) {
			continue;
		}
		if (!mhi_qrtr_take(p, reply, sizeof(reply), &reply_length)) {
			continue;
		}
		if (reply_length < QMI_HEADER_SIZE) {
			report(p, "a reply too short to be QMI, of bytes", reply_length);
			continue;
		}
		type = reply[0];
		answer = (uint32_t)reply[3] | (uint32_t)reply[4] << 8;
		if (type == QMI_INDICATION) {
			mhi_qmi_indication(p, answer, reply, reply_length);
			continue;
		}
		if (type != QMI_RESPONSE || answer != (uint32_t)message) {
			say("THE REPLY DOES NOT ANSWER %s: type %u, message %#x, and %#x was asked",
			    what, type, answer, message);
			return EIO;
		}
		at = QMI_HEADER_SIZE;
		if (at + 3u > reply_length || reply[at] != 0x02u) {
			say("THE REPLY TO %s DOES NOT BEGIN WITH A RESULT TLV: its first byte is %#x",
			    what, at < reply_length ? reply[at] : 0u);
			return EIO;
		}
		report(p, "the result", reply[at + 3]);
		report(p, "the error", at + 4u < reply_length ? reply[at + 4] : 0u);
		if ((reply[at + 3] | (at + 4u < reply_length ? reply[at + 4] : 0u)) != 0u) {
			say("THE FIRMWARE REFUSED %s: result %u, error %u", what, reply[at + 3],
			    at + 4u < reply_length ? reply[at + 4] : 0u);
			return EIO;
		}
		report(p, "the answer's length, in bytes", reply_length);
		reader(p, reply, reply_length);
		return 0;
	}
	report(p, "no reply to this message, of number", message);
	report(p, "packets received so far", ipcr.rx_completions);
	report(p, "datagrams lost for want of room", ipcr.rx_lost);
	say("NO REPLY TO %s: it went out and the firmware did not answer it", what);
	return ETIMEDOUT;
}

/*
 * One QMI message out and the answer back.  `message` is the QMI message id, which the firmware
 * echoes in its reply; `request` is the whole message, header and TLVs.
 *
 * A reply is a QMI RESPONSE (type 2) of the same message id whose first TLV is type 2, holding a
 * two-byte result and a two-byte error -- success when both are zero.  Anything else is reported
 * as it came, because a refusal that is not read is a refusal that is guessed at.
 */
static int
mhi_qmi_request(struct pcie *p, uint16_t message, const uint8_t *request, uint32_t length,
    const char *what)
{
	uint8_t reply[1024];
	uint32_t type, transaction, answer, body, at, waited, reply_length;
	int error;

	error = mhi_qrtr_send(p, WLFW_NODE, WLFW_PORT, request, length);
	if (error != 0) {
		report(p, "the request could not be sent, errno", (uint32_t)error);
		return error;
	}
	/*
	 * Read packets until the one that answers this message turns up, acting on any
	 * indication that gets in first.  A request is not answered by a request, and the
	 * firmware does not wait for us to look before it sends the next thing.
	 */
	for (waited = 0; waited < WLFW_REPLY_WAIT; waited += WLFW_PACKET_WAIT) {
		if (!mhi_qrtr_wait(p, WLFW_PACKET_WAIT) && ipcr.rx_head == ipcr.rx_tail) {
			continue;
		}
		if (!mhi_qrtr_take(p, reply, sizeof(reply), &reply_length)) {
			continue;
		}
		if (reply_length < QMI_HEADER_SIZE) {
			report(p, "a reply too short to be QMI, of bytes", reply_length);
			continue;
		}
		type = reply[0];
		transaction = (uint32_t)reply[1] | (uint32_t)reply[2] << 8;
		answer = (uint32_t)reply[3] | (uint32_t)reply[4] << 8;
		body = (uint32_t)reply[5] | (uint32_t)reply[6] << 8;
		report(p, "the reply's QMI type", type);
		report(p, "the reply's transaction", transaction);
		report(p, "the reply's message id", answer);
		report(p, "the reply's body length", body);
		if (type == QMI_INDICATION) {
			mhi_qmi_indication(p, answer, reply, reply_length);
			continue;
		}
		if (type != QMI_RESPONSE || answer != (uint32_t)message) {
			say("THE REPLY DOES NOT ANSWER %s: type %u, message %#x, and %#x was asked",
			    what, type, answer, message);
			return EIO;
		}
		at = QMI_HEADER_SIZE;
		if (at + 3u > reply_length || reply[at] != 0x02u) {
			say("THE REPLY TO %s DOES NOT BEGIN WITH A RESULT TLV: its first byte is %#x",
			    what, at < reply_length ? reply[at] : 0u);
			return EIO;
		}
		report(p, "the result", reply[at + 3]);
		report(p, "the error", at + 4u < reply_length ? reply[at + 4] : 0u);
		if ((reply[at + 3] | (at + 4u < reply_length ? reply[at + 4] : 0u)) != 0u) {
			say("THE FIRMWARE REFUSED %s: result %u, error %u", what, reply[at + 3],
			    at + 4u < reply_length ? reply[at + 4] : 0u);
			return EIO;
		}
		return 0;
	}
	report(p, "no reply to this message, of number", message);
	report(p, "packets received so far", ipcr.rx_completions);
	report(p, "datagrams lost for want of room", ipcr.rx_lost);
	say("NO REPLY TO %s: it went out and the firmware did not answer it", what);
	return ETIMEDOUT;
}

/*
 * The indication-register request, message 0x0020: the client tells the firmware which
 * asynchronous messages it wants to hear.  ath11k sends this before anything else.
 *
 *   0x15 client_id       = 0x4b4e454c   (QMI_WLANFW_CLIENT_ID, ath11k/qmi.h:255)
 *   0x10 fw_ready_enable = 1
 *   0x1b cal_done_enable = 1
 *   0x18 fw_init_done_enable = 1
 *   0x16 request_mem_enable  = 1        -- the firmware will ask us for memory
 *   0x17 fw_mem_ready_enable = 1
 */
static uint32_t
mhi_wlfw_ind_register(uint8_t *buffer, uint32_t size)
{
	static const struct {
		uint8_t type, bytes;
		uint32_t value;
	} tlvs[] = {
		{ 0x15, 4, QMI_WLANFW_CLIENT_ID },
		{ 0x10, 1, 1u },
		{ 0x1b, 1, 1u },
		{ 0x18, 1, 1u },
		{ 0x16, 1, 1u },
		{ 0x17, 1, 1u },
	};
	uint32_t at = QMI_HEADER_SIZE;

	for (unsigned i = 0; i < sizeof(tlvs) / sizeof(tlvs[0]); i++) {
		if (at + 3u + tlvs[i].bytes > size) {
			return 0;
		}
		buffer[at] = tlvs[i].type;
		buffer[at + 1] = tlvs[i].bytes;
		buffer[at + 2] = 0u;
		for (uint8_t b = 0; b < tlvs[i].bytes; b++) {
			buffer[at + 3u + b] = (uint8_t)(tlvs[i].value >> (8u * b));
		}
		at += 3u + tlvs[i].bytes;
	}
	buffer[0] = QMI_REQUEST;
	buffer[1] = 1u;
	buffer[2] = 0u;
	buffer[3] = (uint8_t)QMI_WLANFW_IND_REGISTER_REQ;
	buffer[4] = (uint8_t)(QMI_WLANFW_IND_REGISTER_REQ >> 8);
	buffer[5] = (uint8_t)(at - QMI_HEADER_SIZE);
	buffer[6] = (uint8_t)((at - QMI_HEADER_SIZE) >> 8);
	return at;
}

/* Drain the event rings for up to `wait_ms` milliseconds, and say whether a datagram turned up. */
static bool
mhi_qrtr_wait(struct pcie *p, uint32_t wait_ms)
{
	for (uint32_t i = 0; i < wait_ms; i++) {
		for (unsigned ring = 0; ring < MHI_EVENT_RING_COUNT; ring++) {
			if (mhi_service_events(p, ring, UINT64_MAX, NULL, NULL) != 0) {
				return false;
			}
		}
		/*
		 * A firmware that has stopped talking because it is waiting to be told it may
		 * carry on: answer it here, where no transmit is outstanding, or the reply it
		 * owes us is a datagram it will never be allowed to send.
		 */
		if (qrtr_resume_due != 0u && mhi_qrtr_resume_tx(p) != 0) {
			return false;
		}
		if (ipcr.rx_head != ipcr.rx_tail) {
			return true;
		}
		pause_us(p, 1000);
	}
	return false;
}

/*
 * Take the oldest datagram out of the queue.  `length` gets the datagram's real length even
 * when it is longer than `size`, so the caller can say so rather than parse a truncated one.
 */
static bool
mhi_qrtr_take(struct pcie *p, uint8_t *data, uint32_t size, uint32_t *length)
{
	uint32_t tail = ipcr.rx_tail;
	uint32_t have;

	(void)p;
	if (tail == ipcr.rx_head) {
		return false;
	}
	have = ipcr.rx_queue[tail].length;
	*length = have;
	if (have > size) {
		have = size;
	}
	memcpy(data, ipcr.rx_queue[tail].data, have);
	ipcr.rx_tail = (tail + 1u) % WLFW_RX_QUEUE;
	return true;
}

/*
 * An indication is the firmware talking without being asked: it asked us for memory
 * (0x0035) and it tells us when the memory we gave it is ready (0x0037).  Both arrive in
 * the middle of a request-and-answer conversation, so they are handled here, where any
 * wait can see them.
 */
static bool
mhi_qmi_indication(struct pcie *p, uint32_t answer, const uint8_t *reply, uint32_t length)
{
	if (answer == QMI_WLANFW_REQUEST_MEM_IND) {
		say("THE FIRMWARE ASKS FOR ITS MEMORY");
		mhi_wlfw_parse_mem_request(p, reply, length);
		if (!mhi_wlfw_allocate_mem(p)) {
			say("THE MEMORY THE FIRMWARE ASKED FOR COULD NOT BE ALLOCATED");
			return false;
		}
		return true;
	}
	if (answer == QMI_WLANFW_FW_MEM_READY_IND) {
		say("THE FIRMWARE SAYS ITS MEMORY IS READY");
		wlfw_mem_ready = true;
		return true;
	}
	if (answer == QMI_WLANFW_FW_INIT_DONE_IND) {
		say("THE FIRMWARE SAYS ITS BOARD DATA AND M3 ARE IN: INIT DONE");
		wlfw_init_done = true;
		return true;
	}
	if (answer == QMI_WLANFW_CAL_DONE_IND) {
		say("THE FIRMWARE SAYS ITS COLD-BOOT CALIBRATION IS DONE");
		wlfw_cal_done = true;
		return true;
	}
	if (answer == QMI_WLANFW_FW_READY_IND) {
		say("THE FIRMWARE SAYS IT IS READY");
		wlfw_fw_ready = true;
		return true;
	}
	report(p, "an indication we do not act on, of message id", answer);
	return false;
}

/* Act on every indication already waiting in the receive queue. */
static void
mhi_qmi_drain(struct pcie *p)
{
	uint8_t packet[1024];
	uint32_t length;

	while (mhi_qrtr_take(p, packet, sizeof(packet), &length)) {
		if (length >= QMI_HEADER_SIZE && packet[0] == QMI_INDICATION) {
			uint32_t answered = (uint32_t)packet[3] | (uint32_t)packet[4] << 8;

			mhi_qmi_indication(p, answered, packet, length);
		}
	}
}

/*
 * The three requests ath11k sends, in its order, once the firmware has said "init done".
 * Each returns the length of the message it wrote, or 0 if it did not fit.
 */

/* ath11k_qmi_wlanfw_wlan_ini_send: 0x002f, enablefwlog = 1.  Four bytes. */
static uint32_t
mhi_wlfw_ini_request(uint8_t *buffer, uint32_t size)
{
	if (size < QMI_HEADER_SIZE + WLFW_INI_BODY_BYTES) {
		return 0u;
	}
	buffer[0] = QMI_REQUEST;
	buffer[3] = (uint8_t)(QMI_WLANFW_WLAN_INI_REQ & 0xffu);
	buffer[4] = (uint8_t)((QMI_WLANFW_WLAN_INI_REQ >> 8) & 0xffu);
	buffer[5] = WLFW_INI_BODY_BYTES;
	buffer[6] = 0u;
	buffer[7] = 0x10u;			/* TLV 0x10, one byte: enable firmware logging */
	buffer[8] = 0x01u;
	buffer[9] = 0x00u;
	buffer[10] = 0x01u;
	return QMI_HEADER_SIZE + WLFW_INI_BODY_BYTES;
}

/* Put one little-endian 32-bit value in a buffer and step past it. */
static uint32_t
mhi_wlfw_put32(uint8_t *buffer, uint32_t at, uint32_t value)
{
	buffer[at + 0u] = (uint8_t)(value & 0xffu);
	buffer[at + 1u] = (uint8_t)((value >> 8) & 0xffu);
	buffer[at + 2u] = (uint8_t)((value >> 16) & 0xffu);
	buffer[at + 3u] = (uint8_t)((value >> 24) & 0xffu);
	return at + 4u;
}

/*
 * ath11k_qmi_wlanfw_wlan_cfg_send: 0x0023, four TLVs and 486 bytes of body.
 * TLV 0x13 (the older shadow-register list) is not part of it at all: ath11k sets
 * shadow_reg_valid = 0 and the encoder skips every element that shares that type.
 */
static uint32_t
mhi_wlfw_cfg_request(uint8_t *buffer, uint32_t size)
{
	uint32_t at;

	if (size < QMI_HEADER_SIZE + WLFW_CFG_BODY_BYTES) {
		return 0u;
	}
	buffer[0] = QMI_REQUEST;
	buffer[3] = (uint8_t)(QMI_WLANFW_WLAN_CFG_REQ & 0xffu);
	buffer[4] = (uint8_t)((QMI_WLANFW_WLAN_CFG_REQ >> 8) & 0xffu);
	buffer[5] = (uint8_t)(WLFW_CFG_BODY_BYTES & 0xffu);
	buffer[6] = (uint8_t)((WLFW_CFG_BODY_BYTES >> 8) & 0xffu);
	at = QMI_HEADER_SIZE;

	/* 0x10: the host's version string, three bytes, no length prefix and no NUL. */
	buffer[at++] = 0x10u;
	buffer[at++] = 0x03u;
	buffer[at++] = 0x00u;
	buffer[at++] = 'W';
	buffer[at++] = 'I';
	buffer[at++] = 'N';

	/* 0x11: the copy engines.  The one-byte count is the number of entries, not bytes. */
	if ((pcie_cfg_parts & 1u) == 0u) {
		goto service_table;
	}
	buffer[at++] = 0x11u;
	buffer[at++] = (uint8_t)((1u + WLFW_CFG_TARGET_CE * 20u) & 0xffu);
	buffer[at++] = (uint8_t)(((1u + WLFW_CFG_TARGET_CE * 20u) >> 8) & 0xffu);
	buffer[at++] = (uint8_t)WLFW_CFG_TARGET_CE;
	for (unsigned i = 0; i < WLFW_CFG_TARGET_CE; i++) {
		for (unsigned j = 0; j < 5u; j++) {
			at = mhi_wlfw_put32(buffer, at, wlfw_ce_config[i][j]);
		}
	}

service_table:
	/* 0x12: which engine carries which service. */
	if ((pcie_cfg_parts & 2u) == 0u) {
		goto shadow_table;
	}
	buffer[at++] = 0x12u;
	buffer[at++] = (uint8_t)((1u + WLFW_CFG_SERVICES * 12u) & 0xffu);
	buffer[at++] = (uint8_t)(((1u + WLFW_CFG_SERVICES * 12u) >> 8) & 0xffu);
	buffer[at++] = (uint8_t)WLFW_CFG_SERVICES;
	for (unsigned i = 0; i < WLFW_CFG_SERVICES; i++) {
		for (unsigned j = 0; j < 3u; j++) {
			at = mhi_wlfw_put32(buffer, at, wlfw_svc_config[i][j]);
		}
	}

shadow_table:
	/* 0x14: the ring pointers the firmware is to keep a shadow copy of. */
	if (wlfw_shadow_count != 0u) {
		buffer[at++] = 0x14u;
		buffer[at++] = (uint8_t)((1u + wlfw_shadow_count * 4u) & 0xffu);
		buffer[at++] = (uint8_t)(((1u + wlfw_shadow_count * 4u) >> 8) & 0xffu);
		buffer[at++] = (uint8_t)wlfw_shadow_count;
		for (unsigned i = 0; i < wlfw_shadow_count; i++) {
			at = mhi_wlfw_put32(buffer, at, wlfw_shadow_ce[i]);
		}
	}
	buffer[5] = (uint8_t)((at - QMI_HEADER_SIZE) & 0xffu);
	buffer[6] = (uint8_t)(((at - QMI_HEADER_SIZE) >> 8) & 0xffu);
	return at;
}

/* ath11k_qmi_wlanfw_mode_send: 0x0022, eleven bytes of body. */
static uint32_t
mhi_wlfw_mode_request(uint8_t *buffer, uint32_t size, uint32_t mode)
{
	uint32_t at = QMI_HEADER_SIZE;

	if (size < QMI_HEADER_SIZE + WLFW_MODE_BODY_BYTES_NORMAL) {
		return 0u;
	}
	buffer[0] = QMI_REQUEST;
	buffer[3] = (uint8_t)(QMI_WLANFW_WLAN_MODE_REQ & 0xffu);
	buffer[4] = (uint8_t)((QMI_WLANFW_WLAN_MODE_REQ >> 8) & 0xffu);
	buffer[5] = WLFW_MODE_BODY_BYTES_NORMAL;
	buffer[6] = 0u;
	buffer[at++] = 0x01u;			/* TLV 0x01, four bytes: the mode */
	buffer[at++] = 0x04u;
	buffer[at++] = 0x00u;
	at = mhi_wlfw_put32(buffer, at, mode);
	buffer[at++] = 0x10u;			/* TLV 0x10, one byte: hardware debug */
	buffer[at++] = 0x01u;
	buffer[at++] = 0x00u;
	buffer[at++] = 0x00u;
	return at;
}

/*
 * Wait for one of the firmware's indications, servicing the event rings and draining the
 * receive queue while it comes.  `ms` is how long it is given; the rings are looked at once
 * a millisecond, which is what the memory-ready wait above does.
 */
static bool
mhi_wlfw_wait_for(struct pcie *p, const bool *flag, uint32_t ms, const char *what)
{
	for (uint32_t i = 0; i < ms; i++) {
		for (unsigned ring = 0; ring < MHI_EVENT_RING_COUNT; ring++) {
			if (mhi_service_events(p, ring, UINT64_MAX, NULL, NULL) != 0) {
				break;
			}
		}
		mhi_qmi_drain(p);
		if (*flag) {
			return true;
		}
		pause_us(p, 1000);
	}
	say("NOTHING CAME OF %s WITHIN %u ms", what, ms);
	return false;
}

/*
 * Find one TLV in a QMI message and hand back its payload.  `size` is the payload's
 * length, or 0 when the TLV is absent; the payload itself is never copied.
 */
static const uint8_t *
mhi_qmi_tlv(const uint8_t *reply, uint32_t length, uint8_t wanted, uint32_t *size)
{
	uint32_t at = QMI_HEADER_SIZE;

	*size = 0u;
	while (at + 3u <= length) {
		uint8_t type = reply[at];
		uint32_t body = (uint32_t)reply[at + 1u] | ((uint32_t)reply[at + 2u] << 8);

		if (at + 3u + body > length) {
			break;
		}
		if (type == wanted) {
			*size = body;
			return reply + at + 3u;
		}
		at += 3u + body;
	}
	return NULL;
}

/* A little-endian word out of a TLV that holds at least four bytes, or 0. */
static uint32_t
mhi_qmi_tlv_word(const uint8_t *reply, uint32_t length, uint8_t wanted, uint32_t index)
{
	const uint8_t *payload;
	uint32_t size;

	payload = mhi_qmi_tlv(reply, length, wanted, &size);
	if (payload == NULL || size < 4u * (index + 1u)) {
		return 0u;
	}
	payload += 4u * index;
	return (uint32_t)payload[0] | ((uint32_t)payload[1] << 8) |
	    ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
}

/* Print every TLV in a QMI message: its type, its length, and its first bytes. */
static void
mhi_qmi_dump_tlvs(struct pcie *p, const uint8_t *reply, uint32_t length)
{
	uint32_t at = QMI_HEADER_SIZE;
	unsigned shown = 0;

	while (at + 3u <= length && shown < 12u) {
		uint8_t type = reply[at];
		uint32_t body = (uint32_t)reply[at + 1u] | ((uint32_t)reply[at + 2u] << 8);

		if (at + 3u + body > length) {
			say("tlv %#x says it is %u bytes, which runs past the message", type, body);
			break;
		}
		report(p, "a capability answer TLV, of type", type);
		report(p, "  its payload length, in bytes", body);
		if (body >= 1u) {
			report(p, "  its first word", (uint32_t)reply[at + 3u]);
		}
		if (body >= 2u) {
			report(p, "  its first two words",
			    (uint32_t)reply[at + 3u] | ((uint32_t)reply[at + 4u] << 8));
		}
		if (body >= 4u) {
			report(p, "  its first four words",
			    (uint32_t)reply[at + 3u] | ((uint32_t)reply[at + 4u] << 8) |
			    ((uint32_t)reply[at + 5u] << 16) | ((uint32_t)reply[at + 6u] << 24));
		}
		at += 3u + body;
		shown++;
	}
}

/*
 * Read the chip's answer to QMI_WLANFW_CAP_REQ into wlfw_cap.  chip_id and
 * chip_family share TLV 0x10 (they are one struct of two words); board_id is TLV
 * 0x11 and soc_id TLV 0x12, one word each.
 */
static void
mhi_wlfw_read_capabilities(struct pcie *p, const uint8_t *reply, uint32_t length)
{
	const uint8_t *chip;
	uint32_t size;

	(void)p;
	mhi_qmi_dump_tlvs(p, reply, length);
	wlfw_cap.chip_id = mhi_qmi_tlv_word(reply, length, QMI_WLANFW_CAP_CHIP_INFO, 0u);
	wlfw_cap.chip_family = mhi_qmi_tlv_word(reply, length, QMI_WLANFW_CAP_CHIP_INFO, 1u);
	wlfw_cap.board_id = mhi_qmi_tlv_word(reply, length, QMI_WLANFW_CAP_BOARD_INFO, 0u);
	wlfw_cap.soc_id = mhi_qmi_tlv_word(reply, length, QMI_WLANFW_CAP_SOC_INFO, 0u);
	chip = mhi_qmi_tlv(reply, length, QMI_WLANFW_CAP_CHIP_INFO, &size);
	wlfw_cap.on = (chip != NULL && size >= 8u);
	say("the chip says it is chip_id %#x, chip_family %#x, board_id %#x, soc_id %#x",
	    wlfw_cap.chip_id, wlfw_cap.chip_family, wlfw_cap.board_id, wlfw_cap.soc_id);
}

/* The board file the boot argument names, or NULL if it names none of them. */
static const struct hakios_wlanfw_board *
mhi_wlfw_board(const char *name)
{
	if (name[0] == 0) {
		return NULL;
	}
	for (uint32_t i = 0; i < hakios_wlanfw_board_count; i++) {
		const char *wanted = name;
		const char *have = hakios_wlanfw_boards[i].name;
		bool same = true;

		while (*wanted != 0 && *have != 0 && same) {
			same = *wanted == *have;
			wanted++;
			have++;
		}
		if (same && *wanted == 0 && *have == 0) {
			return &hakios_wlanfw_boards[i];
		}
	}
	return NULL;
}

/*
 * The board file, in pieces of QMI_WLANFW_BDF_PIECE bytes, as ath11k's
 * ath11k_qmi_load_file_target_mem() sends one: `total_size` is what is still to come
 * (not the file's size), `seg_id` counts the pieces from zero, and `end` is set on the
 * last one.  Every file in this phone's package begins \x7fELF, so the type is ELF.
 */
static int
mhi_wlfw_send_board_file(struct pcie *p, const struct hakios_wlanfw_board *board)
{
	static uint8_t request[4096 + QMI_WLANFW_BDF_PIECE];   /* 7 of header, 38 of TLVs, then 6144 */
	uint32_t left, piece = 0;
	const uint8_t *at = board->bytes;

	say("the board file is %s, %llu bytes, beginning %02x %02x %02x %02x",
	    board->name, board->size, board->bytes[0], board->bytes[1], board->bytes[2],
	    board->bytes[3]);
	for (left = (uint32_t)board->size; left > 0u; piece++) {
		uint32_t portion = left < pcie_bdf_piece ? left : pcie_bdf_piece;
		uint32_t at_out = QMI_HEADER_SIZE;
		uint32_t declared_at;
		int error;

		if (pcie_bdf_piece == 0u || pcie_bdf_piece > QMI_WLANFW_BDF_PIECE) {
			say("THE PIECE SIZE IS OUT OF RANGE: pcie_bdf_piece=%u, and the most this "
			    "build can send is %u", pcie_bdf_piece, QMI_WLANFW_BDF_PIECE);
			return EINVAL;
		}

		request[0] = QMI_REQUEST;
		request[1] = (uint8_t)(4u + piece);
		request[2] = 0u;
		request[3] = (uint8_t)(QMI_WLANFW_BDF_DOWNLOAD_REQ & 0xffu);
		request[4] = (uint8_t)(QMI_WLANFW_BDF_DOWNLOAD_REQ >> 8);
		request[5] = 0u;
		request[6] = 0u;

		/*
		 * No optional flag reaches the wire.  qmi_encode()'s QMI_OPT_FLAG case
		 * encodes into a local `opt_flag_value`, never into the output buffer, and
		 * does not call UPDATE_ENCODE_VARIABLES: the flag only chooses between
		 * emitting the value element that follows and skipping it.  Each optional
		 * element therefore becomes a TLV holding the value alone.  (The flag byte
		 * was here for a while, put there by a misreading of that case; it made the
		 * body 6188 bytes, seven over the 6182 the firmware is told to expect.)
		 */
		/* 0x01 valid, which is not optional at all: a plain one byte TLV. */
		request[at_out++] = 0x01u;
		request[at_out++] = 1u;
		request[at_out++] = 0u;
		request[at_out++] = 1u;
		/* 0x10 the file id: what ath11k takes from the board id, 0xff when the
		 * firmware named none (this one did not: its capability answer has no
		 * board TLV at all). */
		request[at_out++] = 0x10u;
		request[at_out++] = 4u;
		request[at_out++] = 0u;
		request[at_out++] = (uint8_t)(pcie_bdf_id & 0xffu);
		request[at_out++] = (uint8_t)((pcie_bdf_id >> 8) & 0xffu);
		request[at_out++] = (uint8_t)((pcie_bdf_id >> 16) & 0xffu);
		request[at_out++] = (uint8_t)((pcie_bdf_id >> 24) & 0xffu);
		/* 0x11 how much is left to come, not the file's size. */
		request[at_out++] = 0x11u;
		request[at_out++] = 4u;
		request[at_out++] = 0u;
		request[at_out++] = (uint8_t)(left & 0xffu);
		request[at_out++] = (uint8_t)((left >> 8) & 0xffu);
		request[at_out++] = (uint8_t)((left >> 16) & 0xffu);
		request[at_out++] = (uint8_t)((left >> 24) & 0xffu);
		/* 0x12 the piece's number. */
		request[at_out++] = 0x12u;
		request[at_out++] = 4u;
		request[at_out++] = 0u;
		request[at_out++] = (uint8_t)(piece & 0xffu);
		request[at_out++] = (uint8_t)((piece >> 8) & 0xffu);
		request[at_out++] = (uint8_t)((piece >> 16) & 0xffu);
		request[at_out++] = (uint8_t)((piece >> 24) & 0xffu);
		/*
		 * 0x13 the piece, in the order ath11k's table has it: qmi_encode walks
		 * qmi_wlanfw_bdf_download_req_msg_v01_ei[] top to bottom and emits each TLV
		 * as it goes, so data (0x13) comes before end (0x14) and bdf_type (0x15).
		 * The length in front of the data is two bytes, because that element's
		 * elem_size is sizeof(u16); qmi_encode writes it and then sets
		 * encode_tlv = 0, which is how the array that follows lands in the same TLV.
		 */
		{
			declared_at = at_out;

			request[at_out++] = 0x13u;
			at_out += 2u;                 /* the length, filled in below */
			request[at_out++] = (uint8_t)(portion & 0xffu);
			request[at_out++] = (uint8_t)((portion >> 8) & 0xffu);
			memcpy(request + at_out, at, portion);
			at_out += portion;
			request[declared_at + 1u] = (uint8_t)((at_out - declared_at - 3u) & 0xffu);
			request[declared_at + 2u] = (uint8_t)(((at_out - declared_at - 3u) >> 8) & 0xffu);
		}
		/* 0x14 end, set on the last piece only. */
		request[at_out++] = 0x14u;
		request[at_out++] = 1u;
		request[at_out++] = 0u;
		request[at_out++] = (left <= pcie_bdf_piece) ? 1u : 0u;
		/* 0x15 the type: every file of this package begins \x7fELF. */
		request[at_out++] = 0x15u;
		request[at_out++] = 1u;
		request[at_out++] = 0u;
		request[at_out++] = QMI_WLANFW_BDF_TYPE_ELF;
		/*
		 * The body length, which two of these builders left at zero.  Zero is not
		 * "nothing to say": the firmware reads it as a request with no elements
		 * at all, and answered the first piece with result 1 without ever
		 * mentioning the TLVs.  The builders that filled it in (the indication
		 * register, the host capability) were accepted first time.
		 */
		request[5] = (uint8_t)((at_out - QMI_HEADER_SIZE) & 0xffu);
		request[6] = (uint8_t)(((at_out - QMI_HEADER_SIZE) >> 8) & 0xffu);
		say("piece %u: %u bytes of the file, a body of %u bytes beginning "
		    "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", piece, portion,
		    at_out - QMI_HEADER_SIZE, request[7], request[8], request[9], request[10],
		    request[11], request[12], request[13], request[14], request[15], request[16],
		    request[17], request[18]);
		error = mhi_qmi_request(p, QMI_WLANFW_BDF_DOWNLOAD_REQ, request, at_out,
		    "a piece of the board file");
		if (error != 0) {
			say("THE BOARD FILE WAS REFUSED AT PIECE %u (%u bytes of it, %u still to come)",
			    piece, portion, left);
			return error;
		}
		left -= portion;
		at += portion;
	}
	say("THE BOARD FILE IS IN: %u pieces, %u bytes", piece, (uint32_t)board->size);
	return 0;
}

/*
 * Where the M3 image is, so the firmware can fetch it.  `QMI_WLANFW_M3_INFO_REQ`: TLV
 * 0x01 the address (eight bytes) and 0x02 the size (four).  ath11k passes the address
 * from dma_alloc_coherent(), which is a physical address -- and this firmware takes it
 * literally, as the memory answer showed.  The image is copied in and cleaned first,
 * because the firmware will read it from memory with the radio off.
 */
static int
mhi_wlfw_send_m3(struct pcie *p)
{
	uint8_t request[64];
	uint32_t at = QMI_HEADER_SIZE;
	uint64_t address;

	if (pcie_fixed_iova != 0u
	    ? !dma_alloc_at(&wlanfw_m3, (vm_offset_t)hakios_wlanfw_m3_size, PTE_PAGE, QCA_M3_IOVA)
	    : !dma_alloc(&wlanfw_m3, (vm_offset_t)hakios_wlanfw_m3_size, PTE_PAGE)) {
		say("THE M3 IMAGE COULD NOT BE PUT ANYWHERE: %llu bytes", hakios_wlanfw_m3_size);
		return ENOMEM;
	}
	memcpy((void *)wlanfw_m3.bytes, hakios_wlanfw_m3_bytes, (size_t)hakios_wlanfw_m3_size);
	dma_clean(&wlanfw_m3);
	/*
	 * the M3 address follows the same rule as the memory answer: the firmware reaches memory
	 * through the radio's own address space, and that is the mapped one, not the physical one.
	 */
	address = pcie_mem_phys != 0u ? (uint64_t)kvtophys(wlanfw_m3.bytes) : wlanfw_m3.address;
	wlanfw_m3_loaded = true;
	say("the M3 image is %llu bytes at %#llx, which the radio sees at %#llx",
	    hakios_wlanfw_m3_size, address, wlanfw_m3.address);

	request[0] = QMI_REQUEST;
	request[1] = 6u;
	request[2] = 0u;
	request[3] = (uint8_t)(QMI_WLANFW_M3_INFO_REQ & 0xffu);
	request[4] = (uint8_t)(QMI_WLANFW_M3_INFO_REQ >> 8);
	request[5] = 0u;
	request[6] = 0u;
	request[at++] = QMI_WLANFW_M3_ADDR;
	request[at++] = 8u;
	request[at++] = 0u;
	for (unsigned i = 0; i < 8u; i++) {
		request[at++] = (uint8_t)((address >> (8u * i)) & 0xffu);
	}
	request[at++] = QMI_WLANFW_M3_SIZE_TLV;
	request[at++] = 4u;
	request[at++] = 0u;
	for (unsigned i = 0; i < 4u; i++) {
		request[at++] = (uint8_t)((hakios_wlanfw_m3_size >> (8u * i)) & 0xffu);
	}
	/* The body length: eighteen bytes of TLV, and the header says so. */
	request[5] = (uint8_t)((at - QMI_HEADER_SIZE) & 0xffu);
	request[6] = (uint8_t)(((at - QMI_HEADER_SIZE) >> 8) & 0xffu);
	if (mhi_qmi_request(p, QMI_WLANFW_M3_INFO_REQ, request, at, "the M3 image") != 0) {
		return EIO;
	}
	say("THE FIRMWARE KNOWS WHERE M3 IS");
	return 0;
}

/*
 * Stage 19: the firmware's QMI service, and the first two requests the Wi-Fi stack makes of it.
 * The order is ath11k's (ath11k_qmi_event_server_arrive, qmi.c:2991): the indication register
 * first, then the host capability.  Everything after those -- the memory the firmware asks for,
 * the M3 segment, the board file -- is more of the same conversation, so the point here is that
 * the conversation happens at all.
 */
static int pcie_enumerate(struct pcie *p);

/* The state the handshake leaves behind, so that a second one starts from nothing. */
static void
pcie_wlfw_forget(void)
{
	wlfw_mem.count = 0u;
	wlfw_mem_ready = false;
	wlfw_cap.on = false;
	wlfw_cal_done = false;
	wlfw_ce_ring_count = 0u;
	wlfw_ce_pool_count = 0u;
	wlfw_ce_ready = false;
}

/*
 * The firmware is in reset, its memory is kept, and it is walked up again exactly as a first
 * start would: the same QMI conversation, then mission mode instead of calibration.  Nothing
 * here is new code -- it is the same calls the first round makes, in the same order, with the
 * mode at the end set to 0.
 */
static int
pcie_wlfw_second(struct pcie *p)
{
	static uint8_t request[1024];
	const struct hakios_wlanfw_board *board;
	uint32_t length;
	int error;

	/*
	 * NOT hakios_pcie_radio_boot(): it takes umi_lock, and this whole path already runs
	 * inside it (radio_boot -> pcie_enumerate -> pcie_wlfw -> here).  XNU's mutex is not
	 * recursive, so that second lock is an instant deadlock -- which is exactly what hung
	 * the first run of this code: no panic, no log, USB wedged.
	 */
	say("SECOND ROUND: the firmware goes back into reset, as ath11k's does after a calibration");
	if (p->io != NULL) {
		(void)gpio_output(p, 79, false, 1);
		pause_us(p, 1000);
		(void)update(p, GCC + 0x52008, BIT(4), 0);   /* the PIPE clock: apollo 0130 */
		(void)gpio_output(p, 20, false, 3 | (7u << 6));
	}
	mhi.on = false;
	amss.on = false;
	ipcr.on = false;
	mhi_events_started = false;
	pcie_wlfw_forget();
	pause_us(p, 100000);
	{
		uint32_t upto = pcie_upto;

		memset(p, 0, sizeof(*p));
		pcie_upto = 18;
		error = pcie_enumerate(p);
		pcie_upto = upto;
	}
	if (error != 0) {
		say("SECOND ROUND: the radio did not come back: errno %d", error);
		return error;
	}
	say("SECOND ROUND: the endpoint is up again, with a new firmware in it");
	pcie_stop_here(67, "the second enumeration is through");

	length = mhi_wlfw_ind_register(request, sizeof(request));
	if (length == 0u) {
		return EMSGSIZE;
	}
	error = mhi_qmi_request(p, QMI_WLANFW_IND_REGISTER_REQ, request, length,
	    "the second indication register");
	if (error != 0) {
		return error;
	}
	say("SECOND ROUND: the indication register is taken");

	length = mhi_wlfw_host_cap_request(request, sizeof(request));
	if (length == 0u) {
		return EMSGSIZE;
	}
	error = mhi_qmi_request(p, QMI_WLANFW_HOST_CAP_REQ, request, length,
	    "the second host-capability request");
	if (error != 0) {
		return error;
	}
	say("SECOND ROUND: the host capability is taken");

	mhi_qmi_drain(p);
	if (wlfw_mem.count == 0u) {
		say("SECOND ROUND: the firmware asked for no memory at all");
		return EIO;
	}
	length = mhi_wlfw_mem_response(request, sizeof(request));
	if (length == 0u) {
		return EMSGSIZE;
	}
	error = mhi_qmi_request(p, QMI_WLANFW_RESPOND_MEM_REQ, request, length,
	    "the second memory answer");
	if (error != 0) {
		return error;
	}
	say("SECOND ROUND: the memory answer is taken");

	mhi_qmi_drain(p);
	for (unsigned i = 0; !wlfw_mem_ready && i < WLFW_REPLY_WAIT; i++) {
		for (unsigned ring = 0; ring < MHI_EVENT_RING_COUNT; ring++) {
			if (mhi_service_events(p, ring, UINT64_MAX, NULL, NULL) != 0) {
				break;
			}
		}
		mhi_qmi_drain(p);
		if (!wlfw_mem_ready) {
			pause_us(p, 1000);
		}
	}
	if (!wlfw_mem_ready) {
		say("SECOND ROUND: THE MEMORY WAS NEVER CALLED READY");
		return ETIMEDOUT;
	}
	say("SECOND ROUND: the firmware says its memory is ready");

	if (pcie_bdf_send != 0u) {
		board = mhi_wlfw_board(pcie_bdf_name);
		if (board == NULL) {
			return EINVAL;
		}
		error = mhi_wlfw_send_board_file(p, board);
		if (error != 0) {
			return error;
		}
	}
	error = mhi_wlfw_send_m3(p);
	if (error != 0) {
		return error;
	}
	say("SECOND ROUND: the board file is in and the firmware knows where M3 is");

	/*
	 * mode 0 = mission.  This is the request section 24 only ever sent on a firmware that
	 * had just been calibrated and not restarted; it went SYS_ERR in 180 ms.
	 */
	length = mhi_wlfw_mode_request(request, sizeof(request), 0u);
	if (length == 0u) {
		return EMSGSIZE;
	}
	say("SECOND ROUND: the mode request is %u bytes, mode 0 (mission)", length);
	error = mhi_qmi_request(p, QMI_WLANFW_WLAN_MODE_REQ, request, length,
	    "the mission-mode request");
	if (error != 0) {
		say("SECOND ROUND: THE FIRMWARE REFUSED MISSION MODE: errno %d", error);
		return error;
	}
	say("SECOND ROUND: THE FIRMWARE TOOK MISSION MODE");
	pcie_stop_here(68, "the firmware is in mission mode");
	return 0;
}

static int
pcie_wlfw(struct pcie *p)
{
	static uint8_t request[1024];
	uint32_t length;
	int error;

	if (pcie_wlfw_mode == 0) {
		say("the QMI stage is skipped: pcie_wlfw=0");
		return 0;
	}
	if (!ipcr.on) {
		say("the QMI stage needs the IPCR channels: they are not running");
		return ENXIO;
	}
	pcie_stop_here(50, "the QMI stage is entered");
	say("the firmware's service is %#x at node %u port %u; asking it for a host-capability "
	    "handshake", WLFW_SERVICE, WLFW_NODE, WLFW_PORT);

	/* 1. the indication register, which is how the firmware learns who is talking to it. */
	length = mhi_wlfw_ind_register(request, sizeof(request));
	if (length == 0u) {
		say("THE INDICATION REGISTER DID NOT FIT");
		return EMSGSIZE;
	}
	say("the indication register is %u bytes: message %#x", length,
	    QMI_WLANFW_IND_REGISTER_REQ);
	error = mhi_qmi_request(p, QMI_WLANFW_IND_REGISTER_REQ, request, length,
	    "the indication register");
	if (error != 0) {
		return error;
	}
	say("THE FIRMWARE TOOK THE INDICATION REGISTER");
	pcie_stop_here(51, "the indication register is accepted");

	/* 2. the host capability, which is what stage 19 exists for. */
	length = mhi_wlfw_host_cap_request(request, sizeof(request));
	if (length == 0u) {
		say("THE REQUEST DID NOT FIT");
		return EMSGSIZE;
	}
	say("the host-capability request is %u bytes: message %#x, %u TLVs", length,
	    QMI_WLANFW_HOST_CAP_REQ, 7u);
	error = mhi_qmi_request(p, QMI_WLANFW_HOST_CAP_REQ, request, length,
	    "the host-capability request");
	if (error != 0) {
		return error;
	}
	say("THE FIRMWARE ACCEPTED THE HOST-CAPABILITY HANDSHAKE");
	pcie_stop_here(52, "the host-capability handshake is accepted");

	/*
	 * 3. the memory answer.  The request asked for two segments; both fit this driver's
	 * window, so the answer carries the addresses it asked for rather than ath11k's
	 * "delayed" empty answer, which this firmware refused outright.
	 */
	/*
	 * The memory request usually lands in the same drain as the answer to the host-cap,
	 * so it is still in the queue when the answer has been read: act on it before asking
	 * whether the firmware asked for anything.
	 */
	mhi_qmi_drain(p);
	if (wlfw_mem.count == 0u) {
		say("THE FIRMWARE ASKED FOR NO MEMORY AT ALL, WHICH CANNOT BE");
		return EIO;
	}
	length = mhi_wlfw_mem_response(request, sizeof(request));
	if (length == 0u) {
		say("THE MEMORY ANSWER DID NOT FIT");
		return EMSGSIZE;
	}
	say("the memory answer is %u bytes: message %#x, %u segments", length,
	    QMI_WLANFW_RESPOND_MEM_REQ, wlfw_mem.count);
	error = mhi_qmi_request(p, QMI_WLANFW_RESPOND_MEM_REQ, request, length, "the memory answer");
	if (error != 0) {
		return error;
	}
	say("THE FIRMWARE TOOK THE MEMORY");
	pcie_stop_here(54, "the firmware has its memory");

	/*
	 * 4. the firmware's own word that the memory we handed it is ready, message 0x0037.
	 * It usually arrives in the same breath as the answer above, so this drains what is
	 * left in the queue before it goes looking for more: ath11k loads the board file and
	 * the M3 image off the back of this indication, not off the answer.
	 */
	mhi_qmi_drain(p);
	for (unsigned i = 0; !wlfw_mem_ready && i < WLFW_REPLY_WAIT; i++) {
		for (unsigned ring = 0; ring < MHI_EVENT_RING_COUNT; ring++) {
			if (mhi_service_events(p, ring, UINT64_MAX, NULL, NULL) != 0) {
				break;
			}
		}
		mhi_qmi_drain(p);
		if (!wlfw_mem_ready) {
			pause_us(p, 1000);
		}
	}
	if (wlfw_mem_ready) {
		say("THE MEMORY IS READY: the firmware can take its board file and its M3 image");
	} else {
		say("THE MEMORY WAS ACCEPTED AND NEVER CALLED READY: the firmware went quiet");
	}
	pcie_stop_here(55, "the firmware's memory is ready");
	say("stage 19: done, the firmware's QMI service answered both requests");
	pcie_stop_here(53, "stage 19 is over");

	/*
	 * Stage 20 begins where the memory left off.  The firmware has somewhere to put
	 * things, but we still do not know which board file belongs to this phone: its
	 * answer to CAP_REQ carries chip_id and board_id, and ath11k builds the board
	 * file's name out of those two numbers.
	 */
	if (pcie_stage20_mode == 0u) {
		return 0;
	}
	pcie_stop_here(56, "stage 20 is entered");
	request[0] = QMI_REQUEST;
	request[1] = 5u;
	request[2] = 0u;
	request[3] = (uint8_t)(QMI_WLANFW_CAP_REQ & 0xffu);
	request[4] = (uint8_t)(QMI_WLANFW_CAP_REQ >> 8);
	request[5] = 0u;
	request[6] = 0u;
	error = mhi_qmi_request_capture(p, QMI_WLANFW_CAP_REQ, request, QMI_HEADER_SIZE,
	    "the capability request", mhi_wlfw_read_capabilities);
	if (error != 0) {
		return error;
	}
	if (!wlfw_cap.on) {
		say("THE CHIP DID NOT SAY WHAT BOARD IT IS, AND WITHOUT THAT THERE IS NO BOARD FILE");
		return EIO;
	}
	pcie_stop_here(57, "the chip's identity is known");

	/*
	 * Stage 20's second half.  ath11k does these two in this order off the firmware's
	 * "memory ready" indication (ath11k_qmi_event_load_bdf, qmi.c:3241): the board file
	 * first, because the radio cannot come up without it, then the address of M3.
	 */
	if (pcie_bdf_send != 0u) {
		const struct hakios_wlanfw_board *board = mhi_wlfw_board(pcie_bdf_name);

		if (board == NULL) {
			say("NO BOARD FILE IS NAMED: give pcie_bdf= one of the %u names in the kernel "
			    "(pcie_bdf=bd_j11.elf, for instance)", hakios_wlanfw_board_count);
			return EINVAL;
		}
		pcie_stop_here(58, "the board file is about to go");
		error = mhi_wlfw_send_board_file(p, board);
		if (error != 0) {
			return error;
		}
		pcie_stop_here(59, "the board file is in");
	} else {
		say("the board file is not sent: pcie_bdf_send=0");
	}
	error = mhi_wlfw_send_m3(p);
	if (error != 0) {
		return error;
	}
	pcie_stop_here(60, "the firmware knows where M3 is");
	say("stage 20: done, the firmware has its board file and knows where M3 is");

	/*
	 * Stage 21.  The firmware has its board file and knows where M3 is, but it has not
	 * started: apollo's step 10 waits for "init done" (0x0038) at this point, and this
	 * firmware had already sent that by the time M3's answer turned up.
	 */
	if (pcie_stage21_mode == 0u) {
		return 0;
	}
	pcie_stop_here(61, "stage 21 is entered");
	if (!wlfw_init_done) {
		(void)mhi_wlfw_wait_for(p, &wlfw_init_done, WLFW_INIT_WAIT,
		    "the init-done indication");
	}
	if (wlfw_init_done) {
		say("THE FIRMWARE HAS TAKEN ITS BOARD DATA AND M3: INIT DONE");
	} else {
		say("THE FIRMWARE NEVER SAID INIT DONE AFTER ITS BOARD DATA AND M3");
	}
	pcie_stop_here(62, "the firmware has said init done");

	/*
	 * Now the firmware is to be configured and started.  apollo's step 10 sends the
	 * mode request by itself, and this firmware refuses it that way: mode 7 (cold boot)
	 * put the chip into SYS_ERR at once, and mode 0 was refused outright.  ath11k's
	 * ath11k_qmi_firmware_start() sends three requests in this order, and the mode is
	 * the third: first the firmware logging switch, then the copy-engine configuration,
	 * then the mode.
	 */
	if (pcie_cfg_after != 0u || pcie_cfg_during != 0u) {
		say("the configuration is held back until after the mode request");
		goto mode_request;
	}
	if (pcie_ini_send == 0u && pcie_cfg_send == 0u) {
		say("the logging switch and the copy-engine configuration are left out: "
		    "only the mode request goes out, as apollo's step 10 sends it");
		goto mode_request;
	}
	if (pcie_ini_send == 0u) {
		say("the logging switch is left out");
		goto cfg_request;
	}
	length = mhi_wlfw_ini_request(request, sizeof(request));
	if (length == 0u) {
		say("THE FIRMWARE-LOGGING REQUEST DID NOT FIT");
		return EMSGSIZE;
	}
	say("the firmware-logging request is %u bytes: message %#x", length,
	    QMI_WLANFW_WLAN_INI_REQ);
	error = mhi_qmi_request(p, QMI_WLANFW_WLAN_INI_REQ, request, length,
	    "the firmware-logging switch");
	if (error != 0) {
		return error;
	}
	say("THE FIRMWARE TOOK THE LOGGING SWITCH");
	pcie_stop_here(63, "the logging switch is accepted");

cfg_request:
	if (pcie_cfg_send == 0u) {
		say("the copy-engine configuration is left out");
		goto mode_request;
	}
	wlfw_shadow_count = pcie_shadow != 0u ? wlfw_ce_shadow_count() : 0u;
	if (wlfw_shadow_count != 0u) {
		wlfw_ce_shadow_fill();
		say("the shadow table follows the rings that were made: %u entries, the first %#x",
		    wlfw_shadow_count, wlfw_shadow_ce[0]);
	} else {
		say("the shadow table is left out: the firmware keeps no copy of the ring pointers");
	}
	length = mhi_wlfw_cfg_request(request, sizeof(request));
	if (length == 0u) {
		say("THE CONFIGURATION REQUEST DID NOT FIT");
		return EMSGSIZE;
	}
	say("the configuration request is %u bytes: message %#x, %u engines, %u services, "
	    "%u shadow registers", length, QMI_WLANFW_WLAN_CFG_REQ, WLFW_CFG_TARGET_CE,
	    WLFW_CFG_SERVICES, wlfw_shadow_count);
	error = mhi_qmi_request(p, QMI_WLANFW_WLAN_CFG_REQ, request, length,
	    "the copy-engine configuration");
	if (error != 0) {
		return error;
	}
	say("THE FIRMWARE TOOK THE COPY-ENGINE CONFIGURATION");
	pcie_stop_here(64, "the copy-engine configuration is accepted");
	if (pcie_ce_survey != 0u) {
		mhi_wlfw_window_scan(p);
	mhi_wlfw_bar_size(p);
	mhi_wlfw_ce_survey(p);
	}

	if (pcie_ce_init != 0u) {
		error = mhi_wlfw_ce_init(p);
		if (error != 0) {
			return error;
		}
		if (pcie_ce_survey != 0u) {
			mhi_wlfw_ce_survey(p);
		}
	}

mode_request:
	length = mhi_wlfw_mode_request(request, sizeof(request), pcie_mode);
	if (length == 0u) {
		say("THE MODE REQUEST DID NOT FIT");
		return EMSGSIZE;
	}
	say("the mode request is %u bytes: message %#x, mode %u", length,
	    QMI_WLANFW_WLAN_MODE_REQ, pcie_mode);
	error = mhi_qmi_request(p, QMI_WLANFW_WLAN_MODE_REQ, request, length,
	    "the mode request");
	if (error != 0) {
		if (pcie_ce_survey != 0u) {
			mhi_wlfw_ce_survey(p);
		}
		mhi_wlfw_dump_log(p);
		return error;
	}
	say("THE FIRMWARE TOOK THE MODE REQUEST");
	pcie_stop_here(65, "the mode request is accepted");
	/*
	 * The chip takes three or four seconds to calibrate, and it sends its own HTC
	 * hello the moment it is ready.  So the configuration and the rings have to be
	 * in place *during* that window, not after it.
	 */
	if (pcie_cfg_during != 0u && pcie_cfg_send != 0u) {
		wlfw_shadow_count = pcie_shadow != 0u ? wlfw_ce_shadow_count() : 0u;
		if (wlfw_shadow_count != 0u) {
			wlfw_ce_shadow_fill();
		}
		length = mhi_wlfw_cfg_request(request, sizeof(request));
		if (length == 0u) {
			say("THE MIDWAY CONFIGURATION REQUEST DID NOT FIT");
			return EMSGSIZE;
		}
		say("while the chip calibrates, the configuration request is %u bytes: "
		    "message %#x, %u engines, %u services, %u shadow registers", length,
		    QMI_WLANFW_WLAN_CFG_REQ, WLFW_CFG_TARGET_CE, WLFW_CFG_SERVICES, wlfw_shadow_count);
		error = mhi_qmi_request(p, QMI_WLANFW_WLAN_CFG_REQ, request, length,
		    "the copy-engine configuration");
		if (error != 0) {
			return error;
		}
		say("THE FIRMWARE TOOK THE COPY-ENGINE CONFIGURATION MIDWAY");
		if (pcie_ce_init != 0u) {
			error = mhi_wlfw_ce_init(p);
			if (error != 0u) {
				return error;
			}
			say("THE COPY ENGINES ARE SET UP MIDWAY");
		}
		if (pcie_ce_rx != 0u && pcie_ce_init != 0u) {
			error = mhi_wlfw_ce_post_rx(p);
			if (error != 0) {
				return error;
			}
		}
		if (pcie_htc != 0u && pcie_ce_init != 0u) {
			error = mhi_wlfw_htc_hello(p);
			if (error != 0) {
				return error;
			}
		}
	}

	/*
	 * Then it either calibrates or says it is ready.  Sixty seconds is what apollo
	 * allows; either "calibration done" (0x003e) or "ready" (0x0021) ends the wait.
	 */
	for (uint32_t ms = 0; ms < WLFW_CAL_WAIT; ms++) {
		for (unsigned ring = 0; ring < MHI_EVENT_RING_COUNT; ring++) {
			if (mhi_service_events(p, ring, UINT64_MAX, NULL, NULL) != 0) {
				break;
			}
		}
		mhi_qmi_drain(p);
		if (wlfw_cal_done || wlfw_fw_ready) {
			break;
		}
		pause_us(p, 1000);
	}
	if (!wlfw_cal_done && !wlfw_fw_ready) {
		say("NEITHER CALIBRATION DONE NOR READY WITHIN %u ms", WLFW_CAL_WAIT);
		mhi_wlfw_dump_log(p);
		return ETIMEDOUT;
	}
	say("stage 21: the firmware is up: calibration done %s, ready %s",
	    wlfw_cal_done ? "said" : "not said", wlfw_fw_ready ? "said" : "not said");
	if (pcie_cfg_after != 0u && pcie_cfg_send != 0u) {
		wlfw_shadow_count = pcie_shadow != 0u ? wlfw_ce_shadow_count() : 0u;
		if (wlfw_shadow_count != 0u) {
			wlfw_ce_shadow_fill();
		}
		length = mhi_wlfw_cfg_request(request, sizeof(request));
		if (length == 0u) {
			say("THE LATE CONFIGURATION REQUEST DID NOT FIT");
			return EMSGSIZE;
		}
		say("now that the firmware has calibrated, the configuration request is %u bytes: "
		    "message %#x, %u engines, %u services, %u shadow registers", length,
		    QMI_WLANFW_WLAN_CFG_REQ, WLFW_CFG_TARGET_CE, WLFW_CFG_SERVICES, wlfw_shadow_count);
		error = mhi_qmi_request(p, QMI_WLANFW_WLAN_CFG_REQ, request, length,
		    "the copy-engine configuration");
		if (error != 0) {
			return error;
		}
		say("THE FIRMWARE TOOK THE COPY-ENGINE CONFIGURATION AFTER CALIBRATING");
		if (pcie_ce_init != 0u) {
			error = mhi_wlfw_ce_init(p);
			if (error != 0u) {
				return error;
			}
		say("THE COPY ENGINES ARE SET UP AFTER THE CALIBRATION");
	}
	if (pcie_ce_rx != 0u && pcie_ce_init != 0u) {
		error = mhi_wlfw_ce_post_rx(p);
		if (error != 0) {
			return error;
		}
	}
	if (pcie_mode_after != 0xffu) {
		length = mhi_wlfw_mode_request(request, sizeof(request), pcie_mode_after);
		if (length == 0u) {
			say("THE SECOND MODE REQUEST DID NOT FIT");
			return EMSGSIZE;
		}
		say("with the rings in place, the mode request is repeated: %u bytes, mode %u",
		    length, pcie_mode_after);
		error = mhi_qmi_request(p, QMI_WLANFW_WLAN_MODE_REQ, request, length,
		    "the second mode request");
		if (error != 0) {
			return error;
		}
		say("THE FIRMWARE TOOK THE SECOND MODE REQUEST");
		for (unsigned i = 0; i < 2000u; i++) {
			for (unsigned ring = 0; ring < MHI_EVENT_RING_COUNT; ring++) {
				if (mhi_service_events(p, ring, UINT64_MAX, NULL, NULL) != 0) {
					break;
				}
			}
			mhi_qmi_drain(p);
			pause_us(p, 1000);
		}
		say("after the second mode request: datagrams lost for want of room = %u",
		    ipcr.rx_lost);
	}
	}
	/*
	 * ath11k never says hello first: it waits for the firmware's own HTC ready message
	 * on the receive rings.  So the only thing to do here is watch them.
	 */
	if (pcie_ce_rx != 0u && wlfw_ce_pool_count != 0u) {
		uint32_t total = 0u;

		for (unsigned i = 0; i < 30000u; i++) {
			mhi_qmi_drain(p);
			for (uint32_t r = 0; r < wlfw_ce_ring_count; r++) {
				if (wlfw_ce_rings[r].type == CE_RING_TYPE_DST &&
				    wlfw_ce_rings[r].nentries > 2u) {
					total += wlfw_ce_take(p, &wlfw_ce_rings[r]);
				}
			}
			if (total != 0u) {
				break;
			}
			pause_us(p, 1000);
		}
		say("THE COPY ENGINES SAID %u COMPLETIONS BACK", total);
	}
	pcie_stop_here(66, "the firmware has calibrated");
	say("stage 21: done, the firmware is configured, started and calibrated");
	if (pcie_second != 0u) {
		error = pcie_wlfw_second(p);
		if (error != 0) {
			return error;
		}
	}
	return 0;
}

static int
pcie_ipcr(struct pcie *p)
{
	/* 13 words: version 1, type 2, node 1, port 0xfffffffe, confirm 0, size 20, dst 0xffffffff
	 * and 0xfffffffe, then a four-word control payload that begins with 2.  Copied verbatim
	 * from umi_mhi.c's umi_mhi_send_qrtr_hello(), which the chip is known to answer. */
	static const uint32_t hello[13] = {
		1u, 2u, 1u, 0xfffffffeu, 0u, 20u, 0xffffffffu, 0xfffffffeu,
		2u, 0u, 0u, 0u, 0u,
	};
	const uint64_t sizes[4] = {
		IPCR_RING_ELEMENTS * 16u, IPCR_RING_ELEMENTS * IPCR_BUFFER_SIZE,
		IPCR_RING_ELEMENTS * 16u, IPCR_RX_BUFFERS * IPCR_BUFFER_SIZE,
	};
	uint32_t ee;
	int error;

	if (pcie_ipcr_mode == 0) {
		say("the IPCR channels are not started: pcie_ipcr=0");
		return 0;
	}
	report(p, "stage 18: the IPCR channels and QRTR", 0);
	if (!amss.on) {
		say("the AMSS is not running (stage 16): there is no control plane to talk to");
		return EBUSY;
	}
	ee = prd(p, BAR + prd(p, BAR + MHI_BHI_OFFSET) + BHI_EE);
	if (ee != MHI_EE_AMSS && ee != MHI_EE_AMSS_WITH_SBL) {
		say("THE AMSS IS NOT IN CHARGE: the environment reads %u", ee);
		return EBUSY;
	}
	if (ipcr.on) {
		say("the IPCR channels were already started");
		return 0;
	}
	if (!dma_alloc(&ipcr.tx_ring, sizes[0], PTE_PAGE) ||
	    !dma_alloc(&ipcr.tx_buffers, sizes[1], PTE_PAGE) ||
	    !dma_alloc(&ipcr.rx_ring, sizes[2], PTE_PAGE) ||
	    !dma_alloc(&ipcr.rx_buffers, sizes[3], PTE_PAGE)) {
		say("THE IPCR RINGS WERE NOT ALLOCATED");
		return ENOMEM;
	}
	report(p, "the transmit ring is at", (uint32_t)ipcr.tx_ring.address);
	report(p, "the transmit buffers are at", (uint32_t)ipcr.tx_buffers.address);
	report(p, "the receive ring is at", (uint32_t)ipcr.rx_ring.address);
	report(p, "the receive buffers are at", (uint32_t)ipcr.rx_buffers.address);
	pcie_stop_here(28, "the IPCR buffers are allocated");

	ipcr.tx_wp = ipcr.tx_ring.address;
	ipcr.rx_wp = ipcr.rx_ring.address;
	ipcr.tx_slot = 0;
	ipcr.tx_pending = false;
	ipcr.tx_done = false;
	ipcr.response_done = false;
	ipcr.waiting_response = false;
	mhi_setup_ipcr_channel(MHI_IPCR_TX_CHANNEL, 1u, &ipcr.tx_ring, ipcr.tx_wp);
	mhi_setup_ipcr_channel(MHI_IPCR_RX_CHANNEL, 2u, &ipcr.rx_ring, ipcr.rx_wp);
	dma_clean(&mhi.channels);
	__asm__ volatile ("dsb sy" : : : "memory");
	pcie_stop_here(29, "the channel contexts are installed, before any doorbell");

	/*
	 * The event rings are read here for the first time in this stage: their consumers live in
	 * mhi_event_consumer[], which only mhi_events_start() fills in.  With pcie_nop=0 stage 17
	 * never ran, so that array was all zeroes and the loop below subtracted the ring's base from
	 * zero -- a data abort far outside the mapping.  mhi_events_start() is idempotent.
	 */
	mhi_events_start();

	/* M0 and the AMSS leave transfer events behind: they are not answers to anything we asked. */
	for (unsigned ring = 0; ring < MHI_EVENT_RING_COUNT; ring++) {
		error = mhi_service_events(p, ring, UINT64_MAX, NULL, NULL);
		if (error != 0) {
			goto fail;
		}
	}
	pcie_stop_here(30, "the event rings are drained");

	/* A NOP first: if the command ring has stopped answering, nothing below can be believed. */
	mhi_commands_sent = 0;
	error = pcie_mhi_nop(p);
	if (error != 0) {
		goto fail;
	}
	pcie_stop_here(31, "a NOP went through before the channels were started");

	error = mhi_start_channel(p, MHI_IPCR_TX_CHANNEL);
	if (error != 0) {
		goto fail;
	}
	pcie_stop_here(32, "channel 20, the transmit side, is started");
	error = mhi_start_channel(p, MHI_IPCR_RX_CHANNEL);
	if (error != 0) {
		goto fail;
	}
	pcie_stop_here(33, "channel 21, the receive side, is started");

	/*
	 * Only now can the receive side be described: 31 buffers of 8 KiB, each one a transfer
	 * descriptor at the producer end of the ring.  The last slot stays empty so that a full ring
	 * and an empty one never look alike.
	 */
	for (uint32_t i = 0; i < IPCR_RX_BUFFERS; i++) {
		uint64_t buffer = ipcr.rx_buffers.address + (uint64_t)i * IPCR_BUFFER_SIZE;
		volatile uint32_t *tre = (volatile uint32_t *)(volatile void *)(void *)
		    (ipcr.rx_ring.bytes + (size_t)i * 16u);

		tre[0] = (uint32_t)buffer;
		tre[1] = (uint32_t)(buffer >> 32);
		tre[2] = IPCR_BUFFER_SIZE;
		tre[3] = IPCR_TRE_TRANSFER;
	}
	/* A marker the chip has to overwrite for its packet to be believed. */
	{
		volatile uint32_t *pool = (volatile uint32_t *)(volatile void *)(void *)
		    ipcr.rx_buffers.bytes;

		for (uint32_t i = 0; i < IPCR_RX_BUFFERS * (IPCR_BUFFER_SIZE / 4u); i++) {
			pool[i] = 0xa5a5a5a5u;
		}
		dma_clean(&ipcr.rx_buffers);
	}
	ipcr.rx_wp = ipcr.rx_ring.address + (uint64_t)IPCR_RX_BUFFERS * 16u;
	{
		uint32_t *context = (uint32_t *)(uintptr_t)mhi.channels.bytes +
		    MHI_IPCR_RX_CHANNEL * (MHI_CONTEXT_BYTES / 4);

		mhi_context_address(context, 9, ipcr.rx_wp);
	}
	dma_clean(&ipcr.rx_ring);
	dma_clean(&mhi.channels);
	__asm__ volatile ("dsb sy" : : : "memory");
	say("%u receive buffers are posted; the ring's producer is at %#llx", IPCR_RX_BUFFERS,
	    ipcr.rx_wp);
	mhi_reg_address(p, prd(p, BAR + MHI_CHANNEL_DB) + MHI_IPCR_RX_CHANNEL * 8u,
	    ipcr.rx_wp);
	__asm__ volatile ("dsb sy" : : : "memory");
	ipcr.on = true;
	pcie_stop_here(34, "the receive buffers are posted");

	error = mhi_send_ipcr_packet(p, hello, QRTR_HELLO_SIZE, pcie_ipcr_wait, UINT16_MAX);
	if (error != 0) {
		goto fail;
	}
	pcie_stop_here(35, "QRTR's hello is sent");
	say("stage 18: done, QRTR has said hello: %u transmit completions, %u packets back, the last "
	    "one %u bytes", ipcr.tx_completions, ipcr.rx_completions, ipcr.last_rx_bytes);
	if (ipcr.rx_completions == 0u) {
		say("NOTHING CAME BACK: the hello went out and QRTR did not answer it");
	}
	pcie_stop_here(36, "stage 18 is over");
	return 0;

fail:
	report(p, "the IPCR stage failed, errno", (uint32_t)error);
	return error;
}


/* Which match register and which context are free.  Reads only. */
static bool
radio_find(struct pcie *p)
{
	bool used[256] = { false };
	uint32_t scr0 = prd(p, SMMU_PHYS + SMMU_SCR0);
	uint32_t idr0 = prd(p, SMMU_PHYS + SMMU_IDR0);
	uint32_t idr1 = prd(p, SMMU_PHYS + SMMU_IDR1);
	uint32_t matches = idr0 & 0xffu, contexts = idr1 & 0xffu;

	radio.reused = false;

	if ((scr0 & (1u << 3)) != 0) {
		say("this SMMU numbers its streams with 16 bits, which nothing here is written for");
		return false;
	}
	radio.page = (idr1 >> 31) != 0 ? 0x10000u : 0x1000u;
	radio.pages = 2u << (idr1 >> 28 & 7);
	if (2u * radio.pages * radio.page != SMMU_SIZE) {      /* dt: the reg of apps-smmu */
		say("the SMMU says it has 2 x %u pages of %#x bytes; its contexts are not looked for", radio.pages, radio.page);
		return false;
	}
	radio.match = ~0u;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00u; n++) {
		uint32_t smr = prd(p, SMMU_PHYS + SMMU_SMR(n));
		uint32_t s2cr = prd(p, SMMU_PHYS + SMMU_S2CR(n));

		if ((smr & SMMU_SMR_VALID) != 0) {
			/*
			 * A stream routed exactly the way this code routes it is this code's own work,
			 * left behind by an earlier pass: the radio is reset and enumerated again (ath11k
			 * does that after its cold boot calibration) and the SMMU keeps what was written
			 * to it.  Take it back.  Calling it somebody else's is what killed the second
			 * enumeration with ENODEV in log 061152.
			 */
			if ((smr & 0x7fffu) == RADIO_STREAM && ((smr >> 16) & 0x7fffu) == 0) {
				radio.match = n;
				radio.context = s2cr & 0xffu;
				radio.reused = true;
				say("stream %#x is still routed through match register %u and context %u: kept",
				    RADIO_STREAM, n, radio.context);
				return true;
			}
			if ((((smr & 0x7fffu) ^ RADIO_STREAM) & ~((smr >> 16) & 0x7fffu)) == 0) {
				say("stream %#x already belongs to match register %u (%#x, s2cr %#x): left alone", RADIO_STREAM, n,
				    smr, s2cr);
				return false;
			}
			used[s2cr & 0xffu] = true;
		} else if (radio.match == ~0u) {
			radio.match = n;
		}
	}
	for (radio.context = 0; radio.context < contexts && used[radio.context]; radio.context++) {
	}
	if (radio.match == ~0u || radio.context >= contexts) {
		say("the SMMU has no free match register or no free context");
		return false;
	}
	say("free: match register %u and context %u (its registers at %#x)", radio.match, radio.context,
	    SMMU_PHYS + (radio.pages + radio.context) * radio.page);
	return true;
}

/* Switch the radio's stream through our context. */
static int
radio_switch_on(struct pcie *p)
{
	vm_offset_t context = SMMU_PHYS + (radio.pages + radio.context) * radio.page;
	uint64_t root = kvtophys(radio.tables) | (uint64_t)radio.context << 48;
	uint32_t smr = SMMU_SMR(radio.match), s2cr = SMMU_S2CR(radio.match);
	uint32_t sees = SMMU_SMR_VALID | RADIO_STREAM;          /* mask 0: exactly 0x1c01, nothing else */

	report(p, "the radio's context registers are at", (uint32_t)context);
	report(p, "the root table is at", (uint32_t)root);
	report(p, "the context's sctlr before", prd(p, context + SMMU_CB_SCTLR));
	if (radio.reused &&
	    prd(p, context + SMMU_CB_TTBR0) == (uint32_t)root &&
	    prd(p, context + SMMU_CB_TTBR0 + 4) == (uint32_t)(root >> 32)) {
		/*
		 * The tables are the ones this code built: nothing to rebuild, and the "somebody
		 * else's context" guard below must not fire on our own work.
		 */
		pwr(p, context + SMMU_CB_SCTLR, SMMU_SCTLR_ON);
		pwr(p, SMMU_PHYS + s2cr, radio.context);
		pwr(p, SMMU_PHYS + smr, sees);
		say("the context was already this code's own: its tables are kept and the route is put back");
		goto verify;
	}
	if ((prd(p, context + SMMU_CB_SCTLR) & 1u) != 0) {
		say("context %u is switched on although no stream uses it: it is somebody's, left alone", radio.context);
		return EBUSY;
	}

	pwr(p, context + SMMU_CB_SCTLR, 0);
	pwr(p, SMMU_PHYS + radio.page + SMMU_CBA2R(radio.context), SMMU_CBA2R_64_BIT);
	pwr(p, SMMU_PHYS + radio.page + SMMU_CBAR(radio.context), SMMU_CBAR_STAGE_1);
	pwr(p, context + SMMU_CB_TCR2, SMMU_TCR2_BITS);
	pwr(p, context + SMMU_CB_TCR, SMMU_TCR_39_BITS);
	pwr(p, context + SMMU_CB_TTBR0, (uint32_t)root);
	pwr(p, context + SMMU_CB_TTBR0 + 4, (uint32_t)(root >> 32));
	pwr(p, context + SMMU_CB_TTBR1, 0);
	pwr(p, context + SMMU_CB_TTBR1 + 4, 0);
	pwr(p, context + SMMU_CB_MAIR0, SMMU_MAIR0);
	pwr(p, context + SMMU_CB_MAIR1, 0);
	pwr(p, context + SMMU_CB_FSR, SMMU_FSR_ALL);
	pwr(p, context + SMMU_CB_TLBIASID, radio.context);
	pwr(p, context + SMMU_CB_TLBSYNC, 1);
	for (unsigned ms = 0; (prd(p, context + SMMU_CB_TLBSTATUS) & 1u) != 0; ms++) {
		if (ms == 100) {
			say("the SMMU does not finish forgetting old translations");
			return EIO;
		}
		pause_us(p, 1000);
	}
	pwr(p, context + SMMU_CB_SCTLR, SMMU_SCTLR_ON);
	pwr(p, SMMU_PHYS + s2cr, radio.context);                /* translate, through this context */
	pwr(p, SMMU_PHYS + smr, sees);
	say("the match register and the context have been written");

verify:
	/* The global registers are the hypervisor's to grant: read back what it made of them. */
	if (prd(p, SMMU_PHYS + smr) != sees ||
	    (prd(p, SMMU_PHYS + s2cr) & 0x300ffu) != radio.context ||
	    (prd(p, SMMU_PHYS + radio.page + SMMU_CBAR(radio.context)) & 0x30000u) != (SMMU_CBAR_STAGE_1 & 0x30000u) ||
	    (prd(p, context + SMMU_CB_SCTLR) & 1u) == 0 ||
	    prd(p, context + SMMU_CB_TTBR0) != (uint32_t)root ||
	    prd(p, context + SMMU_CB_TTBR0 + 4) != (uint32_t)(root >> 32)) {
		say("THE SMMU DID NOT TAKE WHAT WAS WRITTEN: stream %#x is not going through context %u", RADIO_STREAM,
		    radio.context);
		return EIO;
	}
	report(p, "the match register reads", prd(p, SMMU_PHYS + smr));
	report(p, "the context's sctlr reads", prd(p, context + SMMU_CB_SCTLR));
	report(p, "the context's tcr reads", prd(p, context + SMMU_CB_TCR));
	radio.built = true;
	return 0;
}

/*
 * Stage 13: what the firmware transfer will depend on.  Nothing is written.
 *
 * The endpoint is in its cold PBL (stage 2 read MHIVER as 0xdeadbeef), which is the only state
 * the SBL may be sent from.  These reads say whether that is still true now that the link, the
 * SMMU context and the MSI have all been set up, and they say it before anything is written.
 */
static void
pcie_mhi_survey(struct pcie *p)
{
	uint32_t version = prd(p, BAR + MHI_VERSION);
	uint32_t bhi = prd(p, BAR + MHI_BHI_OFFSET);
	uint32_t bhie = prd(p, BAR + MHI_BHIE_OFFSET);
	uint32_t config, status, control, ee, bhi_version;

	report(p, "MHI version", version);
	report(p, "MHI config", prd(p, BAR + MHI_CONFIG));
	report(p, "BHI offset", bhi);
	report(p, "BHIE offset", bhie);
	if (bhi == UINT32_MAX || bhie == UINT32_MAX || bhi < 0xa8u || bhie < 0xa8u) {
		say("the MHI register block did not answer, or its layout is not the one stage 2 read");
		return;
	}

	bhi_version = prd(p, BAR + bhi + BHI_VERSION);
	ee = prd(p, BAR + bhi + BHI_EE);
	status = prd(p, BAR + MHI_STATUS);
	control = prd(p, BAR + MHI_CONTROL);
	config = prd(p, BAR + MHI_CONFIG);

	report(p, "BHI version (1 expected)", bhi_version);
	report(p, "the execution environment", ee);
	report(p, "MHI status", status);
	report(p, "MHI control", control);
	report(p, "BHI error", prd(p, BAR + bhi + BHI_ERROR));
	report(p, "BHI error debug 0", prd(p, BAR + bhi + BHI_ERROR_DEBUG0));
	report(p, "BHI error debug 1", prd(p, BAR + bhi + BHI_ERROR_DEBUG1));
	report(p, "BHI error debug 2", prd(p, BAR + bhi + BHI_ERROR_DEBUG2));
	report(p, "the host's DMA aperture starts", prd(p, BAR + MHI_HOST_DMA_LOW));
	report(p, "the host's DMA aperture ends", prd(p, BAR + MHI_HOST_DMA_HIGH));
	report(p, "the chip's DMA aperture starts", prd(p, BAR + MHI_DEV_DMA_LOW));
	report(p, "the chip's DMA aperture ends", prd(p, BAR + MHI_DEV_DMA_HIGH));

	if (version == 0xdeadbeefu) {
		say("MHIVER is still the reserved sentinel: no firmware has run in this chip");
	} else if (version == 0x01000000u) {
		say("MHIVER says the chip is running MHI already");
	} else {
		say("MHIVER is neither the sentinel nor a mission-mode version");
	}
	if (bhi_version != 1u) {
		say("THE BHI VERSION IS NOT 1: a firmware transfer is refused from this endpoint");
	}
	if (ee == 0u) {
		say("the chip is in its PBL, which is the only state the SBL may be sent from");
	} else {
		say("the chip is NOT in its PBL (environment %u): the SBL is not to be sent", ee);
	}
	if ((config & 0xffffu) != 0u) {
		say("the configuration word has something in its low half: %#x", config);
	}
	if ((status & 4u) != 0u) {
		say("the status word says the chip is in an error state: %#x", status);
	}
	say("the READY handshake will look for status bit 0, control bit 1 clear and status >> 8 == 1");
}

/*
 * Stage 11: the radio's stream through a context of our own
 *
 * Stage 8 cleared the BDF-to-SID bypass, so the endpoint's transactions now carry stream 0x1c01,
 * and stage 10 found that no match register covers it while the SMMU faults unmatched streams.
 * Until there is a context the endpoint can neither DMA nor write its MSI.
 */
static int
pcie_smmu_context(struct pcie *p)
{
	uint64_t mapped = 0;
	int error;

	if (pcie_smmu_mode == 0) {
		say("the context is not built: pcie_smmu_mode=0");
		return 0;
	}
	report(p, "stage 11: a context for the radio's stream", 0);
	if (!radio_find(p)) {
		return ENODEV;
	}
	/*
	 * The MSI register is a device, not memory: the endpoint writes it and the write has to go
	 * out to the GIC rather than sit in a cache.
	 */
	if (!radio_map(MSI_PHYS, 4096, PTE_DEVICE, &mapped)) {
		say("cannot map the MSI register into the radio's view");
		return ENOMEM;
	}
	report(p, "the MSI register is mapped for the radio at", (uint32_t)mapped);
	error = radio_switch_on(p);
	if (error != 0) {
		return error;
	}
	say("stage 11: done, stream %#x has context %u", RADIO_STREAM, radio.context);
	return 0;
}

/*
 * Stage 12: point the MHI MSI at the real doorbell.
 *
 * Stage 9 programmed a marker (0xfee00000, the x86 LAPIC) only to prove the capability walk
 * worked.  The real one is the GIC-600's message register: writing the INTID raises that SPI, the
 * radio's vector 0 is SPI 768, and the GIC numbers INTIDs from 32, so the data is 800.  The mask
 * register keeps vectors 0..2 masked, so the endpoint still cannot raise anything: this stage
 * proves the programming, not the interrupt.
 */
static int
pcie_msi_point(struct pcie *p)
{
	int error;

	report(p, "stage 12: the MHI MSI, at the real doorbell", 0);
	error = pcie_setup_mhi_msi(p, MSI_REGISTER, RADIO_FIRST_SPI + GIC_SPI_BASE);
	if (error != 0) {
		return error;
	}
	report(p, "stage 12: done, the endpoint would raise SPI", RADIO_FIRST_SPI);
	return 0;
}

/*
 * Stage 10: the PCIe MSI controller and the radio's SMMU stream.  Nothing is written.
 *
 * The GIC-600 raises an SPI by writing the INTID to the message register, so the endpoint's MSI
 * capability has to be pointed at MSI_PHYS and its data has to be RADIO_FIRST_SPI + 32 (the GIC
 * numbers INTIDs from 32).  Whether the endpoint can be pointed there at all is the SMMU's
 * business: the write goes through stream 0x1c01 like any other, and stage 8 turned the bypass
 * off.
 */
static void
pcie_msi_survey(struct pcie *p)
{
	static const char *const kinds[] = {
		"translated by its context", "PASSED UNTRANSLATED (bypass)", "MADE TO FAULT", "of a reserved kind"
	};
	uint32_t scr0, idr0, idr1, matches, valid = 0, covering = 0, message;

	message = prd(p, MSI_REGISTER);
	report(p, "the PCIe MSI controller reads", message);

	scr0 = prd(p, SMMU_PHYS + SMMU_SCR0);
	idr0 = prd(p, SMMU_PHYS + SMMU_IDR0);
	idr1 = prd(p, SMMU_PHYS + SMMU_IDR1);
	report(p, "apps SMMU scr0", scr0);
	report(p, "apps SMMU idr0", idr0);
	report(p, "apps SMMU idr1", idr1);
	if (scr0 == UINT32_MAX || idr0 == UINT32_MAX || idr1 == UINT32_MAX) {
		say("the SMMU did not answer: its registers read back as all ones");
		return;
	}
	say("the SMMU is %s and numbers streams with %u bits; a stream nobody matches %s",
	    (scr0 & 1u) != 0 ? "OFF" : "on", (scr0 & (1u << 3)) != 0 ? 16 : 15,
	    (scr0 & (1u << 10)) != 0 ? "FAULTS" : "passes untranslated");

	matches = idr0 & 0xffu;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00u; n++) {
		uint32_t smr = prd(p, SMMU_PHYS + SMMU_SMR(n));
		uint32_t s2cr = prd(p, SMMU_PHYS + SMMU_S2CR(n));
		uint32_t wide = scr0 & (1u << 3);
		uint32_t stream = smr & (wide ? 0xffffu : 0x7fffu);
		uint32_t mask = (smr >> 16) & (wide ? 0xffffu : 0x7fffu);

		if ((wide != 0 ? s2cr & (1u << 10) : smr >> 31) == 0) {
			continue;
		}
		valid++;
		if (((stream ^ RADIO_STREAM) & ~(mask | 1u)) != 0) {
			continue;
		}
		say("match register %u covers the radio's stream: stream %#x mask %#x (%#x), %s %u; s2cr %#x",
		    n, stream, mask, smr, kinds[(s2cr >> 16) & 3], s2cr & 0xffu, s2cr);
		covering++;
	}
	say("%u of the %u match registers are in use; %u cover stream %#x%s", valid, matches, covering,
	    RADIO_STREAM, covering != 0 ? "" : ": THE BOOT CHAIN HAS SET NOTHING UP FOR THE RADIO'S STREAM");
	say("vector 0 of the radio is SPI %u, so its MSI data is %u and it writes that at %#x",
	    RADIO_FIRST_SPI, RADIO_FIRST_SPI + GIC_SPI_BASE, MSI_REGISTER);
}

/*
 * Stage 7: WLAON power control.
 *
 * CNSS clears SHUTDOWN_EN and VDD4BLOW_SW_EN in WLAON_QFPROM_PWR_CTRL before the first MHI
 * request.  The register is only reachable through the serialized BAR window, which is put back
 * the way it was found; the bits cleared here are fuse-programming enables, so this can only
 * turn them off, never on.
 */
static int
pcie_prepare_radio(struct pcie *p)
{
	uint32_t saved = prd(p, BAR + 0x310c), power, now;

	if (saved == UINT32_MAX || saved == 0xdeadbeefu) {
		return EIO;
	}
	pwr(p, BAR + 0x310c, 0x4000003fu);       /* internal window at 0x01f80000 */
	if ((prd(p, BAR + 0x310c) & 0x4000003fu) != 0x4000003fu) {
		pwr(p, BAR + 0x310c, saved);
		return EIO;
	}
	power = prd(p, BAR + 0x8031c);
	report(p, "WLAON power control before", power);
	if (power == UINT32_MAX || power == 0xdeadbeefu) {
		pwr(p, BAR + 0x310c, saved);
		return EIO;
	}
	pwr(p, BAR + 0x8031c, power & ~5u);      /* SHUTDOWN_EN | VDD4BLOW_SW_EN */
	now = prd(p, BAR + 0x8031c);
	report(p, "WLAON power control after", now);
	pwr(p, BAR + 0x310c, saved);
	return now == (power & ~5u) ? 0 : EIO;
}

/*
 * Stage 8: let the endpoint DMA.
 *
 * Kona hashes the two BDF bytes (bus 1, devfn 0) with CRC-8/0x07; the table entry holds the low
 * byte of the stream ID and PCIe0 supplies the wired 0x1c00 base, so the stream is 0x1c01.
 * SM8250 also defaults to BDF-to-SID bypass, and a table entry alone does not route requests
 * through the owned context: the bypass bit has to be cleared as well.
 */
static int
pcie_enable_dma(struct pcie *p)
{
	uint8_t hash = 0;
	const uint8_t bdf[2] = { 1, 0 };
	uint32_t route, old, command, configuration;
	int error;

	for (unsigned i = 0; i < sizeof(bdf); i++) {
		hash ^= bdf[i];
		for (unsigned b = 0; b < 8; b++) {
			hash = (uint8_t)((hash << 1) ^ ((hash & 0x80) ? 7 : 0));
		}
	}
	route = PARF + 0x2000 + 4 * (uint32_t)hash;
	old = prd(p, route);
	report(p, "BDF-to-SID route address", route);
	report(p, "BDF-to-SID previous route", old);
	if (old != 0 && old != 0x01000100u) {
		return EBUSY;
	}
	pwr(p, route, 0x01000100u);
	if (prd(p, route) != 0x01000100u) {
		return EIO;
	}
	command = prd(p, CONF + 4);
	if (command == UINT32_MAX || (command & 7) != 2) {
		return EIO;
	}
	configuration = prd(p, PARF + 0x2c00);
	report(p, "BDF-to-SID configuration before", configuration);
	error = update(p, PARF + 0x2c00, 1, 0);   /* clear the bypass */
	if (error != 0 || (prd(p, PARF + 0x2c00) & 1) != 0) {
		return EIO;
	}
	report(p, "BDF-to-SID configuration after", prd(p, PARF + 0x2c00));
	pwr(p, DBI + 4, 6);                       /* allow protected transactions upstream */
	pwr(p, CONF + 4, (command & 0xffff) | 4); /* bus mastering on */
	if ((prd(p, CONF + 4) & 7) != 6) {
		return EIO;
	}
	report(p, "protected DMA stream", 0x1c01);
	return 0;
}

/*
 * Stage 9: the MSI capability.
 *
 * Walk the capability list to cap id 5, require a 64-bit capable MSI with at least five vectors
 * and MSI enabled, then program the message address and data.  The mask register is left with
 * vectors 0..2 masked out because those belong to MHI.
 */
static int
pcie_setup_mhi_msi(struct pcie *p, uint64_t address, uint32_t data)
{
	uint32_t cap = prd(p, CONF + 0x34) & 0xffu;
	uint64_t seen = 0;

	if ((address & 3) != 0 || data > 0xffff || (data & 31) != 0) {
		return EINVAL;
	}
	for (unsigned i = 0; i < 48 && cap != 0; i++) {
		uint32_t header, control, msg, mask;

		if (cap < 0x40 || cap > 0xfc || (cap & 3) != 0 || (seen & (1ull << (cap / 4))) != 0) {
			return EINVAL;
		}
		seen |= 1ull << (cap / 4);
		header = prd(p, CONF + cap);
		if (header == UINT32_MAX) {
			return EIO;
		}
		if ((header & 0xffu) != 5) {
			cap = (header >> 8) & 0xffu;
			continue;
		}
		control = header >> 16;
		if (((control >> 1) & 7) < 5 || (control & 0x100) == 0) {
			return ENOTSUP;
		}
		if ((control & 0x80) == 0 && address > UINT32_MAX) {
			return ERANGE;
		}
		msg = cap + ((control & 0x80) != 0 ? 12 : 8);
		mask = msg + 4;
		if (mask > 0xfc) {
			return EINVAL;
		}
		pwr(p, CONF + cap, header & ~BIT(16));      /* masked while it is programmed */
		pwr(p, CONF + mask, UINT32_MAX);
		pwr(p, CONF + cap + 4, (uint32_t)address);
		if ((control & 0x80) != 0) {
			pwr(p, CONF + cap + 8, (uint32_t)(address >> 32));
		}
		pwr(p, CONF + msg, data);
		pwr(p, CONF + mask, 0xfffffff8u);           /* vectors 0, 1 and 2 belong to MHI */
		pwr(p, CONF + cap, (header & ~(0x71u << 16)) | (0x51u << 16));
		if ((prd(p, CONF + cap) & BIT(16)) == 0) {
			return EIO;
		}
		report(p, "MSI capability at", cap);
		report(p, "MSI address low", (uint32_t)address);
		report(p, "MSI data", data);
		report(p, "MSI mask", prd(p, CONF + mask));
		return 0;
	}
	return ENODEV;
}

static int
pcie_enumerate(struct pcie *p)
{
	int error;

	if (p->io != NULL) {
		return EBUSY;
	}
	p->io = &pcie_platform;

	/* Hold the endpoint in reset while its supplies come up. */
	report(p, "stage 1: PERST low", 0);
	TRY(gpio_output(p, 79, false, 1));      /* dt: drive-strength 2, bias-pull-down */
	pause_us(p, 1000);
	report(p, "stage 1: the rails", 0);
	TRY(p->io->power_on(p->io->context));
	report(p, "stage 1: done, the rails are up", 0);
	if (pcie_upto < 2) {
		return 0;
	}

	/* dt: CLKREQ function pci_e0 with a pull-up; WAKE a plain input with a pull-up. */
	report(p, "stage 2: the pins and the clocks", 0);
	TRY(update(p, GPIO + 80 * 0x1000, 0x3ff, 7));
	TRY(update(p, GPIO + 81 * 0x1000, 0x3ff, 3));
	TRY(gpio_output(p, 20, true, 3 | (7u << 6)));   /* dt: pull-up, 16 mA, output high */
	pause_us(p, 1000);

	TRY(pcie_clocks(p));
	report(p, "stage 2: done, the clocks are up", 0);
	if (pcie_upto < 3) {
		return 0;
	}
	report(p, "power and clocks ready, GDSC status", prd(p, GCC + 0x6b004));

	report(p, "stage 3: the root complex", 0);
	pwr(p, PARF + 0x1000, 4);               /* root complex */
	TRY(update(p, PARF + 0x40, 1, 0));
	pwr(p, PARF + 0x350, 0);
	pwr(p, PARF, 0x365e);
	TRY(update(p, PARF + 0x174, 0, BIT(4)));
	pwr(p, PARF + 0x22c, 0);                /* no unhandled PCIe interrupt during enumeration */
	pwr(p, PARF + 0x358, 0x04000000);
	TRY(update(p, PARF + 0x1a8, 0, BIT(31)));
	report(p, "stage 3: done", 0);
	if (pcie_upto < 4) {
		return 0;
	}

	report(p, "stage 4: the PHY sequence, %u writes", (uint32_t)(sizeof(pcie_phy) / sizeof(pcie_phy[0])));
	for (unsigned i = 0; i < sizeof(pcie_phy) / sizeof(pcie_phy[0]); i++) {
		pwr(p, PHY + pcie_phy[i].offset, pcie_phy[i].value);
		if (pcie_phy[i].delay_us != 0) {
			pause_us(p, pcie_phy[i].delay_us);
		}
	}
	pause_us(p, 1000);
	TRY(update(p, GCC + 0x52008, 0, BIT(4)));   /* PIPE: the vendor skips halt polling here */
	TRY(wait_bits(p, PHY + 0x814, BIT(6), 0, 100000));
	report(p, "PHY ready, PCS status", prd(p, PHY + 0x814));
	report(p, "stage 4: done, the PHY is ready", 0);
	if (pcie_upto < 5) {
		return 0;
	}
	pause_us(p, 10000);                     /* dt: qcom,ep-latency = 0xa, 10 ms */
	report(p, "stage 5: PERST high, the endpoint may train", 0);
	TRY(gpio_output(p, 79, true, 1));       /* PERST high: the endpoint may train */
	pause_us(p, 1000);

	TRY(update(p, DBI + 0x80c, 0x1f00, 0x100));
	pwr(p, DBI + 0x8a8, 0x20);
	TRY(update(p, DBI + 0x890, BIT(0), 0));
	TRY(update(p, DBI + 0x8bc, 0, BIT(0)));
	pwr(p, DBI + 0x154, 0x55555555);
	TRY(update(p, DBI + 0x8bc, BIT(0), 0));
	TRY(update(p, DBI + 0xa0, 0xf, 2));     /* Gen2 x1: the stock UMI link rate */
	{
		/* Only the control half of the capability: the upper half is write-1-to-clear. */
		uint32_t control = prd(p, DBI + 0x78);

		if (control == UINT32_MAX) {
			error = EIO;
			goto fail;
		}
		pwr(p, DBI + 0x78, (control & 0xffff) & ~0x7000u);
	}
	TRY(update(p, PARF + 0x1b0, 0, BIT(8)));
	TRY(wait_bits(p, DBI + 0xf28, BIT(10), BIT(10), 100000));
	TRY(wait_bits(p, DBI + 0x80, BIT(29), BIT(29), 100000));
	report(p, "link status", prd(p, DBI + 0x80));
	/*
	 * Every good boot so far has trained to 0xb0120008; the boots whose calibration never
	 * answered had 0xb8120008 and 0x38110008 here (docs 51.18).  Rather than hand a link in
	 * an unknown state to the firmware, put the endpoint back into reset and train again.
	 */
	if (pcie_linkretry != 0u && (prd(p, DBI + 0x80) & 0xff000000u) != 0xb0000000u) {
		say("the link trained into %#x: the endpoint is put back and trained again",
		    prd(p, DBI + 0x80));
		TRY(gpio_output(p, 79, false, 1));
		pause_us(p, 5000);
		TRY(gpio_output(p, 79, true, 1));
		pause_us(p, 20000);
		TRY(update(p, DBI + 0x8bc, 0, BIT(0)));
		TRY(update(p, DBI + 0x8bc, BIT(0), 0));
		pause_us(p, 20000);
		report(p, "link status after the second training", prd(p, DBI + 0x80));
	}
	report(p, "stage 5: done, the link is up", 0);
	if (pcie_upto < 6) {
		return 0;
	}

	report(p, "stage 6: the endpoint's configuration space", 0);
	pwr(p, DBI + 0x18, 0x00010100);         /* primary bus 0, secondary and subordinate 1 */
	pwr(p, DBI + 0x20, 0x60306030);         /* a 1 MiB non-prefetchable bridge window */
	pwr(p, DBI + 0xb40, 0x14);              /* AUX 19.2 MHz */
	TRY(outbound(p, 0, 4, CONF, CONF + 0xfff, 0x01000000));   /* CFG0: 01:00.0 */

	{
		uint32_t id = 0;

		for (unsigned i = 0; i < 1000; i++) {
			id = prd(p, CONF);
			if (id == QCA6390_ID) {
				break;
			}
			pause_us(p, 1000);
		}
		report(p, "endpoint vendor:device", id);
		p->vendor_device = id;
		pcie_id = id;
		if (id != QCA6390_ID) {
			error = ENODEV;
			goto fail;
		}
	}

	{
		uint32_t command = prd(p, CONF + 4), bar0 = prd(p, CONF + 0x10);

		if (command == UINT32_MAX || bar0 == UINT32_MAX || (bar0 & 1) ||
		    ((bar0 >> 1) & 3) == 1 || ((bar0 >> 1) & 3) == 3) {
			error = EIO;
			goto fail;
		}
		/* Memory decoding on, bus mastering OFF: nothing here may start a DMA. */
		pwr(p, CONF + 4, ((command & 0xffff) & ~7u) | BIT(10));
		if (prd(p, CONF + 4) & 4) {
			error = EIO;
			goto fail;
		}
		/* The SID table holds whatever reset left in it: zero it, as the stock driver does. */
		for (unsigned i = 0; i < 256; i++) {
			pwr(p, PARF + 0x2000 + 4 * i, 0);
			if (prd(p, PARF + 0x2000 + 4 * i) != 0) {
				error = EIO;
				goto fail;
			}
		}
		report(p, "BDF-to-SID table zeroed, bus mastering still off", 256);
		pwr(p, CONF + 0x10, BAR | (bar0 & 15));
		if ((bar0 & 6) == 4) {
			pwr(p, CONF + 0x14, 0);
		}
		/*
		 * The whole BAR, not the first MiB: the copy engines sit at +0xa00000 and the
		 * firmware cannot be started without them.  The device tree describes the window
		 * as 0x60300000 for 0x3d00000, which is what BAR_SIZE now is.
		 */
		TRY(outbound(p, 1, 0, BAR, BAR + 0xfffff, BAR));
		/*
		 * The copy engines are at +0xa00000, which no window above covers.  An outbound
		 * window has to be a power of two with its target aligned to that size, so the
		 * whole 0x3d00000 range cannot be one window; this one covers just the engines
		 * (two MiB at BAR + 0xa00000, i.e. 0x60d00000, which is 1 MiB aligned).
		 */
		TRY(outbound(p, 2, 0, BAR + CE_SRC_BASE, BAR + CE_SRC_BASE + 0xfffff,
		    BAR + CE_SRC_BASE));
		pwr(p, DBI + 4, 2);                 /* the root bridge: memory decode only */
		pwr(p, CONF + 4, ((command & 0xffff) & ~7u) | 2 | BIT(10));
		report(p, "endpoint command", prd(p, CONF + 4));
		report(p, "endpoint BAR0", prd(p, CONF + 0x10));
		if ((prd(p, CONF + 4) & 7) != 2 || (prd(p, CONF + 0x10) & ~15u) != BAR) {
			error = EIO;
			goto fail;
		}
	}

	/* The radio's own registers: this is what says whether its PBL is alive and MHI is there. */
	{
		uint32_t version = prd(p, BAR + 8);

		report(p, "MHI version (DMA off)", version);
		report(p, "MHICFG", prd(p, BAR + 0x10));
		report(p, "BHI offset", prd(p, BAR + 0x28));
		report(p, "BHIE offset", prd(p, BAR + 0x2c));
		if (version != QCA6390_MHI_VERSION) {
			report(p, "MHI register length", prd(p, BAR));
			report(p, "MHI control", prd(p, BAR + 0x38));
			report(p, "channel doorbells", prd(p, BAR + 0x18));
			report(p, "event doorbells", prd(p, BAR + 0x20));
			/*
			 * A cold PBL has not been given the firmware yet: MHIVER holds a placeholder and
			 * BHI is readable. These are the values apollo captured on a real UMI PBL, and a
			 * match means the endpoint is waiting for its image, which is the state this step
			 * expects to leave it in.
			 */
			if (version == 0xdeadbeefu && prd(p, BAR) == 0x100 && prd(p, BAR + 0x18) == 0x300 &&
			    prd(p, BAR + 0x20) == 0x700 && prd(p, BAR + 0x28) == 0x100 &&
			    prd(p, BAR + 0x2c) == 0x224 && prd(p, BAR + 0x104) == 1 &&
			    prd(p, BAR + 0x128) == 0) {
				say("the QCA6390 is in its cold PBL: it has no firmware yet (that is step 3 and beyond)");
			}
		}
	}

	if (pcie_swreset != 0u) {
		/* up as well: the endpoint is out of reset and the link is trained by now */
		pcie_sw_reset(p);
	}

	if (pcie_upto >= 7) {
		report(p, "stage 7: WLAON power control", 0);
		error = pcie_prepare_radio(p);
		if (error != 0) {
			goto fail;
		}
		report(p, "stage 7: done, the radio's power control is clear", 0);
	}

	if (pcie_upto >= 8) {
		report(p, "stage 8: bus mastering and the SID route", 0);
		error = pcie_enable_dma(p);
		if (error != 0) {
			goto fail;
		}
		report(p, "stage 8: done, the endpoint may DMA", 0);
	}

	if (pcie_upto >= 9) {
		/*
		 * A marker address, not a real GIC doorbell: until the PCIe MSI controller
		 * (dt: pcie0_msi@17a10040) is set up there is nowhere for a message to land, so this
		 * run only proves the capability walk and the register programming.  Nothing below
		 * raises an interrupt, so the marker is never dereferenced.
		 */
		report(p, "stage 9: the MHI MSI capability", 0);
		error = pcie_setup_mhi_msi(p, 0x00000000fee00000ull, 0x0040u);
		if (error != 0) {
			goto fail;
		}
		report(p, "stage 9: done, MSI is programmed (marker address)", 0);
	}

	if (pcie_upto >= 10) {
		report(p, "stage 10: the MSI controller and the SMMU stream, read only", 0);
		pcie_msi_survey(p);
		report(p, "stage 10: done", 0);
	}

	if (pcie_upto >= 11) {
		error = pcie_smmu_context(p);
		if (error != 0) {
			goto fail;
		}
	}

	if (pcie_upto >= 12) {
		error = pcie_msi_point(p);
		if (error != 0) {
			goto fail;
		}
	}

	if (pcie_upto >= 13) {
		report(p, "stage 13: the MHI and BHI registers, read only", 0);
		pcie_mhi_survey(p);
		report(p, "stage 13: done", 0);
	}

	if (pcie_upto >= 14) {
		error = pcie_sbl(p);
		if (error != 0) {
			goto fail;
		}
	}

	if (pcie_upto >= 15) {
		error = pcie_mhi_ready(p);
		if (error != 0) {
			goto fail;
		}
	}

	if (pcie_upto >= 16) {
		error = pcie_amss(p);
		if (error != 0) {
			goto fail;
		}
	}

	if (pcie_upto >= 17) {
		error = pcie_mhi_nop(p);
		if (error != 0) {
			goto fail;
		}
	}

	if (pcie_upto >= 18) {
		error = pcie_ipcr(p);
		if (error != 0) {
			goto fail;
		}
	}
	if (pcie_upto >= 19) {
		error = pcie_wlfw(p);
		if (error != 0) {
			goto fail;
		}
	}

	p->enumerated = true;
	say("THE QCA6390 ANSWERS ON PCIe0: %04x:%04x", pcie_id & 0xffff, pcie_id >> 16);
	return 0;
fail:
	p->failed = true;
	report(p, "enumeration failed errno", (uint32_t)error);
	/*
	 * A stage that refuses and returns is invisible from the outside: with no panic there is no
	 * log, so "the stage said no" and "the kernel hung" look exactly alike.  Stage 17 spent a
	 * whole session looking like a hang when it was a silent EIO.  With this one on, the refusal
	 * itself carries the log back.
	 */
	if (pcie_fail_panic != 0u) {
		panic("pcie: the enumeration stopped with errno %d (pcie_fail_panic=1)", error);
	}
	/* Back into reset: the endpoint must not be left with decoding on for the loader's return. */
	pwr(p, CONF + 4, (prd(p, CONF + 4) & 0xffff) & ~7u);
	(void)gpio_output(p, 79, false, 1);
	pause_us(p, 1000);
	(void)gpio_output(p, 20, false, 3 | (7u << 6));
	return error;
}

/* ---- Linux's ath11k, as a tenant --------------------------------------------------------------- */

/*
 * ath11k (wifi/lx, built by tools/mk-ath11k.sh) knows the QCA6390's protocol and knows nothing
 * about this port's platform: that its radio is an endpoint behind PCI Express, that the memory
 * its firmware asks for has to sit at three fixed addresses, that its interrupt would come
 * through the MSI page, and that its QMI goes over MHI's IPCR channels rather than GLINK.
 * Everything it cannot learn is supplied from here; nothing below is a second implementation of
 * anything, it is the adapter over what this file already does.
 *
 * The order the driver takes them in is lx_bus.c's: lx_os_rproc_boot() starts the radio, the QMI
 * exchange that follows asks for its firmware's memory (lx_os_radio_firmware_memory), the
 * driver's own buffers are placed in the radio's window (hakios_pcie_radio_map), and while it
 * waits the rings are serviced by a thread (no GIC interrupt carries the radio's doorbell yet).
 */

/* What the firmware asks for, and where CNSS puts it (QCA_*_IOVA above). */
#define UMI_DDR_SIZE            0x00600000ull   /* the DDR segment: 6 MiB is the most it asks for */
#define UMI_CAL_SIZE            0x00280000ull   /* the calibration: 2.5 MiB, kept across a restart */
#define UMI_M3_SIZE             0x00080000ull   /* 512 KiB, contiguous: the image it is started with */
#define UMI_POOL_FIRST          0xa1900000ull   /* first address left to the driver: above the three */
#define UMI_RX_SLOTS            16u             /* datagrams held for the driver's thread */

static LCK_GRP_DECLARE(umi_lock_group, "hakios-umi");
static LCK_MTX_DECLARE(umi_lock, &umi_lock_group);      /* the rings, the stages, the memory */
static LCK_MTX_DECLARE(umi_rx_lock, &umi_lock_group);   /* the queue below, which a receive fills */

static struct pcie umi;                         /* the endpoint, for the calls that arrive from the driver */
static bool umi_up;                             /* the radio has been started and not reset since */
static bool umi_reset_pending;                  /* the driver took the radio down: the next start is a restart */
static struct {                                 /* 0: DDR, 1: CAL, 2: M3 -- placed once, kept */
	bool there;
	struct dma dma;
	vm_offset_t uncached;                   /* how the CPU writes it: round the cache */
} umi_region[3];
static struct {                                 /* what the firmware sent, whole, for the driver's thread */
	uint32_t length[UMI_RX_SLOTS];
	uint8_t *data;
	uint32_t head, tail;
} umi_rx;
static thread_t umi_thread;
static bool umi_polling;
static bool umi_poll_thread_running;
static uint32_t umi_poll_ticks;                 /* how many times the rings have been looked at */
static bool umi_dumped;                         /* the firmware's memory has been read once */
static uint32_t umi_poll_heartbeat_every = 10000; /* one line every ten seconds: at one a
                                                    * second the two lines the heartbeat
                                                    * prints filled the log and evicted the
                                                    * bring-up (run 083001) */

extern void hakios_wifi_qrtr_install(void);
extern void hakios_wifi_qrtr_input(const uint8_t *data, uint32_t length);
extern void hakios_wifi_qrtr_fresh(void);

/*
 * The page tables the radio reads.  The driver's memory is placed before the radio is started
 * (lx_prepare runs first), so they cannot be left to stage 11: made here on first use, and stage
 * 11 finds them and keeps them.
 */
static bool
umi_tables(void)
{
	if (radio.tables != 0) {
		return true;
	}
	if (kmem_alloc(kernel_map, &radio.tables, round_page(4096 * RADIO_TABLES),
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		radio.tables = 0;
		return false;
	}
	radio.table_count = 1;
	return true;
}

/*
 * A buffer of the driver's, in the radio's own address space: the same page tables, the same
 * window, but never on top of the three addresses its firmware insists on -- radio_map() hands
 * addresses out upward and checks nothing, so the first request (the driver's pool, 64 MiB, made
 * before the radio starts) is moved above them.
 */
bool
hakios_pcie_radio_map(uint64_t physical, uint64_t size, uint64_t *address)
{
	uint64_t at = 0;
	bool placed;

	if (size == 0 || !umi_tables()) {
		return false;
	}
	lck_mtx_lock(&umi_lock);
	if (radio.next < UMI_POOL_FIRST - RADIO_IOVA) {
		radio.next = UMI_POOL_FIRST - RADIO_IOVA;
	}
	placed = radio_map(physical, size, PTE_PAGE, &at);
	lck_mtx_unlock(&umi_lock);
	if (placed && address != NULL) {
		*address = at;
	}
	return placed;
}

/*
 * The memory the firmware's request names, at the address its image was built to find it at.
 * Placed in full the first time (the request may be for less) and answered with the same place
 * after that: the calibration region is what has to survive the driver's restart of the radio.
 */
bool
hakios_pcie_firmware_memory(unsigned type, uint64_t size, vm_offset_t *memory, uint64_t *address)
{
	uint64_t want, most;
	unsigned slot;
	bool placed;

	switch (type) {
	case 0:
	case 1:
		slot = 0; want = QCA_DDR_IOVA; most = UMI_DDR_SIZE;
		break;
	case 4:
		slot = 1; want = QCA_CAL_IOVA; most = UMI_CAL_SIZE;
		break;
	case 3:
		slot = 2; want = QCA_M3_IOVA; most = UMI_M3_SIZE;
		break;
	default:
		say("the driver asked for firmware memory of type %u, which this platform has none of", type);
		return false;
	}
	if (size == 0 || size > most) {
		say("the driver asked for %#llx bytes of firmware memory of type %u; that region holds %#llx",
		    size, type, most);
		return false;
	}
	lck_mtx_lock(&umi_lock);
	placed = true;
	if (!umi_region[slot].there) {
		if (!umi_tables() || !dma_alloc_at(&umi_region[slot].dma, most, PTE_PAGE, want)) {
			placed = false;
		} else {
			/*
			 * The driver fills these with plain stores and then says "dsb osh", which does
			 * not clean anything: handed the cacheable mapping, the firmware would read
			 * what was in memory before the driver wrote.  So it is given a window that
			 * goes round the cache instead.
			 */
			umi_region[slot].uncached = ml_io_map_unmappable(
			    kvtophys(umi_region[slot].dma.bytes), umi_region[slot].dma.size,
			    VM_WIMG_WCOMB);
			if (umi_region[slot].uncached == 0) {
				placed = false;
			} else {
				umi_region[slot].there = true;
				say("the firmware's memory of type %u is at %#llx, %#llx bytes", type,
				    want, most);
			}
		}
	}
	if (placed) {
		*memory = umi_region[slot].uncached;
		*address = umi_region[slot].dma.address;
	}
	lck_mtx_unlock(&umi_lock);
	if (!placed) {
		say("the firmware's memory of type %u could not be placed", type);
	}
	return placed;
}

/*
 * Start the radio, from the beginning, for the driver.  The stages remember what they set up (the
 * MHI rings, the AMSS, the IPC router); a start after a reset has to begin as a first start does,
 * so those latches are cleared.  The page tables and the firmware's memory are not among them: the
 * calibration has to survive this.  Stage 19 is the hand-written QMI exchange, which the driver
 * does itself now, so the count stops at 18.
 */
int
hakios_pcie_radio_boot(void)
{
	uint32_t upto = pcie_upto;
	bool restart;
	int error = 0;

	lck_mtx_lock(&umi_lock);
	if (umi_up) {
		lck_mtx_unlock(&umi_lock);
		return 0;
	}
	if (!pcie_map_regions()) {
		lck_mtx_unlock(&umi_lock);
		say("the endpoint's windows could not be mapped");
		return ENXIO;
	}
	/*
	 * apollo 0130's other half.  A start the driver asked for (its power_up after the cold
	 * boot calibration) gets a moment with the radio off, and if the link does not come
	 * back, three tries with the reset's teardown in between instead of one.  Its boot 55
	 * lost the restart with errno 60 and never got a second chance; boot 56 records the
	 * fix working -- "the driver restarts the radio: errno 0", up 444 ms after its start.
	 */
	restart = umi_reset_pending;
	umi_reset_pending = false;
	for (unsigned attempt = 1; ; attempt++) {
		if (restart || attempt > 1) {
			if (umi.io != NULL) {
				pause_us(&umi, 100000);
			}
		}
		memset(&umi, 0, sizeof(umi));
		mhi.on = false;
		amss.on = false;
		ipcr.on = false;
		mhi_events_started = false;
		pcie_upto = 18;
		error = pcie_enumerate(&umi);
		pcie_upto = upto;
		if (error == 0 || attempt == 3) {
			break;
		}
		say("the driver restarts the radio: attempt %u: errno %d; again", attempt, error);
		/*
		 * The quiesce: what the reset writes, without ath11k's own bookkeeping -- the
		 * two lines every enumeration starts from (apollo's sweet_pcie_quiesce).
		 */
		if (umi.io != NULL) {
			(void)gpio_output(&umi, 79, false, 1);
			(void)update(&umi, GCC + 0x52008, BIT(4), 0);
			(void)gpio_output(&umi, 20, false, 3 | (7u << 6));
		}
	}
	if (error != 0) {
		lck_mtx_unlock(&umi_lock);
		say("the radio did not start: errno %d", error);
		return error;
	}
	umi_up = true;
	lck_mtx_unlock(&umi_lock);
	say("the radio is up and its IPC router is answering");
	return 0;
}

/*
 * The driver's power_down: back into reset, by the two writes the failure path makes.  What the
 * radio was told not to forget -- its page tables, its firmware's memory, the calibration in it --
 * is not touched.
 */
void
hakios_pcie_radio_reset(void)
{
	extern void hakios_wifi_qrtr_radio_reset(void);

	/* The services the firmware announced leave with it: the fresh instance must look
	 * new to the QMI helpers, or its announcement is swallowed as a replay. */
	hakios_wifi_qrtr_radio_reset();
	lck_mtx_lock(&umi_lock);
	if (umi.io != NULL) {
		/*
		 * Down first, the way Linux's power_down does it: the software reset needs the
		 * link, and it is what clears the SYSERR the next start would otherwise inherit.
		 */
		if (pcie_swreset != 0u) {
			pcie_sw_reset(&umi);
		}
		if (pcie_syserr_clear != 0u) {
			uint32_t status = pcie_reg_read(&umi, MHI_STATUS);

			/*
			 * The latch Linux clears on every QCA6390 power up (commit f3c603d4): after
			 * the radio goes, MHISTATUS keeps SYSERR set, and a start that inherits it
			 * answers nothing.  Only the latch is touched -- the global reset in that
			 * commit belongs to the pci core's own power-up (docs 51.17).
			 *
			 * It has to happen here, with the link trained: the first attempt read the
			 * same registers after PERST went low and hung the core.
			 */
			report(&umi, "syserr clear: MHI status before", status);
			if ((status & MHI_STATUS_ERROR) != 0u) {
				pcie_reg_write(&umi, MHI_CONTROL, MHI_CONTROL_RESET);
				pause_us(&umi, 1000);
				report(&umi, "syserr clear: MHI status after",
				    pcie_reg_read(&umi, MHI_STATUS));
				pcie_reg_write(&umi, MHI_CONTROL, 0);
			} else {
				say("syserr clear: the latch was already clear; nothing written");
			}
		}
		(void)gpio_output(&umi, 79, false, 1);
		pause_us(&umi, 1000);
		/*
		 * The PIPE clock is voted on once the PHY runs and stays voted when the endpoint
		 * is put in reset: an enumeration after this one would reset the controller and
		 * the PHY with it running, and the link never comes back the same way.  Start
		 * every enumeration as the first one does, with the vote off (apollo 0130; its
		 * boot 55 lost the restart exactly here).
		 */
		(void)update(&umi, GCC + 0x52008, BIT(4), 0);
		(void)gpio_output(&umi, 20, false, 3 | (7u << 6));
	}
	mhi.on = false;
	amss.on = false;
	ipcr.on = false;
	mhi_events_started = false;
	umi_up = false;
	umi_reset_pending = true;
	/*
	 * apollo 0130: "a moment with the radio off first".  Its restart -- ath11k's own, after
	 * the cold-boot calibration -- enumerates the endpoint again, and a link asked for the
	 * instant PERST comes up does not always come back (its boot 55: errno 60).
	 */
	if (umi.io != NULL) {
		pause_us(&umi, 100000);
	}
	lck_mtx_unlock(&umi_lock);
	say("the radio is back in reset; its memory and its calibration are kept");
}

bool
hakios_pcie_radio_up(void)
{
	return umi_up;
}

/* The megabyte of the first BAR that was mapped when the endpoint was enumerated. */
vm_offset_t
hakios_pcie_bar_window(void)
{
	for (unsigned i = 0; i < sizeof(pcie_regions) / sizeof(pcie_regions[0]); i++) {
		if (pcie_regions[i].pa != BAR_PHYS) {
			continue;
		}
		/*
		 * The driver asks for this window before it powers the radio: it maps the registers
		 * first and starts the bus after.  So the regions cannot be left to the start, and
		 * they are mapped here if they are not mapped yet.  Nothing is read from them.
		 */
		if (pcie_regions[i].va == 0) {
			pcie_map_regions();
		}
		return pcie_regions[i].va;
	}
	return 0;
}

/*
 * The state the chip says it is in, read from the register rather than from an event: the
 * firmware sends a state-change event only when it chooses to, so a firmware that stops
 * talking leaves nothing behind, while this register keeps answering as long as the link is
 * up.  0 PBL, 1 SBL, 2 AMSS, 4 mission mode, 0xff dead; -1 means the window is not mapped.
 */
/*
 * Whether the firmware has said its cold-boot calibration is done (0x003e), and not yet been
 * reset since.  The driver answers a start that calibrated with EIO, and the retry loop asks this
 * so it stops throwing such a draw away (see hakios.wifi.c).
 */
int
hakios_pcie_cal_done(void)
{
	return wlfw_cal_done ? 1 : 0;
}

int
hakios_pcie_ee_read(void)
{
	vm_offset_t bar = 0;
	uint32_t bhi;

	/*
	 * Passive on purpose: this is read from inside the thread that carries qmi, and
	 * hakios_pcie_bar_window() maps the window when it is not mapped yet -- mapping
	 * there hung the core (log 021248).  Unmapped answers -1 and changes nothing.
	 */
	for (unsigned i = 0; i < sizeof(pcie_regions) / sizeof(pcie_regions[0]); i++) {
		if (pcie_regions[i].pa == BAR_PHYS) {
			bar = pcie_regions[i].va;
			break;
		}
	}
	if (bar == 0) {
		return -1;
	}
	bhi = prd(NULL, bar + MHI_BHI_OFFSET);
	return (int)prd(NULL, bar + bhi + BHI_EE);
}

/* Where the radio writes, and what, to raise the interrupt vector 0 it was given: stage 12's doorbell. */
bool
hakios_pcie_msi_point(uint64_t *address, uint32_t *data)
{
	if (address != NULL) {
		*address = MSI_REGISTER;
	}
	if (data != NULL) {
		*data = RADIO_FIRST_SPI + GIC_SPI_BASE;
	}
	return true;
}

/*
 * A datagram from the firmware, whole (header and all) and before the hand-written stages look at
 * it.  The driver's QRTR wants control traffic as much as data -- a server announcing itself is
 * what its lookup waits for -- and the queue below is drained by its own thread, so that an answer
 * is never sent from inside the receive it came out of.
 */
static void
umi_input(const uint8_t *packet, uint32_t length)
{
	uint32_t head;

	if (length == 0 || length > IPCR_BUFFER_SIZE) {
		return;
	}
	lck_mtx_lock(&umi_rx_lock);
	if (umi_rx.data == NULL || (umi_rx.head + 1u) % UMI_RX_SLOTS == umi_rx.tail) {
		lck_mtx_unlock(&umi_rx_lock);
		return;
	}
	head = umi_rx.head;
	memcpy(umi_rx.data + (size_t)head * IPCR_BUFFER_SIZE, packet, length);
	umi_rx.length[head] = length;
	umi_rx.head = (head + 1u) % UMI_RX_SLOTS;
	lck_mtx_unlock(&umi_rx_lock);
}

/* One datagram out, from the port the driver's QRTR asked from. */
int
hakios_pcie_qrtr_send(uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *payload,
    uint32_t length)
{
	int error;

	lck_mtx_lock(&umi_lock);
	if (!umi_up) {
		lck_mtx_unlock(&umi_lock);
		return ENETDOWN;
	}
	error = mhi_qrtr_send_from(&umi, from_port, to_node, to_port, payload, length);
	lck_mtx_unlock(&umi_lock);
	return error;
}

void
hakios_pcie_qrtr_install(void)
{
	hakios_wifi_qrtr_install();
	say("the driver's QMI has a road: MHI/IPCR, not GLINK");
}

/*
 * Watch the radio's rings while the driver waits: it was given no interrupt to be told by, so
 * everything the firmware sends waits for this to be looked at.  One millisecond, as apollo's did.
 */
/*
 * What the firmware left in its memory when it went SYS_ERR.  The DDR segment is mapped round the
 * cache, so this reads what the firmware wrote and not what the CPU had.  Its own log lives there;
 * the printable runs are what is worth the log's 24 KB, so they are printed and the hex is not.
 */
static void umi_dump_one_region(unsigned slot, uint32_t *lines, unsigned shortest);

static void
umi_dump_firmware_memory(void)
{
	uint32_t lines = 0;

	/*
	 * All three segments the firmware was given: its DDR, the CAL segment and the M3
	 * segment.  The old 4 MiB cap is gone: the mission-mode crash of docs section 49.9
	 * left 77730 nonzero bytes but no readable text in the first 4 MiB, and the log the
	 * firmware flushes may sit past that or in another segment.
	 */
	/*
	 * DDR first (where the flushed assert text lives), then the M3 segment (its exception
	 * strings), and the CAL segment last: its calibration data is full of long runs that
	 * only look printable, and they ate the whole text budget before the useful segments
	 * were scanned (docs section 50.9).
	 */
	const unsigned order[] = { 0u, 2u, 1u };
	for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
		umi_dump_one_region(order[i], &lines, order[i] == 1u ? 24u : 8u);
	}
	if (lines == 0u) {
		say("the firmware wrote no readable text into any of its memory");
	}
}

static void
umi_dump_one_region(unsigned slot, uint32_t *lines, unsigned shortest)
{
	struct dma *segment = &umi_region[slot].dma;
	uint32_t size, written = 0;

	if (!umi_region[slot].there || segment->bytes == 0 || segment->size == 0) {
		say("there is no segment %u to read", slot);
		return;
	}
	size = (uint32_t)segment->size;
	dma_invalidate(segment);
	for (uint32_t i = 0; i < size; i++) {
		if (((const uint8_t *)segment->bytes)[i] != 0u) {
			written++;
		}
	}
	say("the firmware's segment %u is %u bytes at %#llx, which the radio sees at %#llx; "
	    "%u of them are not zero", slot, size, (uint64_t)(uintptr_t)segment->bytes,
	    segment->address, written);
	for (uint32_t at = 0; at < size && *lines < 40u; at++) {
		const uint8_t *run = (const uint8_t *)segment->bytes + at;
		char text[160];
		uint32_t length = 0, copy;

		while (at + length < size && length < sizeof(text) - 1u) {
			uint8_t c = run[length];

			if (c < 0x20u && c != 0x09u) {
				break;
			}
			if (c > 0x7eu) {
				break;
			}
			length++;
		}
		if (length < shortest) {
			continue;
		}
		copy = length < sizeof(text) - 1u ? length : (uint32_t)sizeof(text) - 1u;
		memcpy(text, run, copy);
		text[copy] = 0;
		say("fw log (segment %u) %#x: %s", slot, at, text);
		(*lines)++;
		at += length - 1u;
	}
	/*
	 * The assert, byte for byte: its text stops at the first non-printable, and the failed
	 * condition and the file's name are behind that.  Widen around the word rather than guess.
	 */
	for (uint32_t at = 0; at + 8u < size; at++) {
		const uint8_t *p = (const uint8_t *)segment->bytes + at;

		if (p[0] != 'A' || p[1] != 's' || p[2] != 's' || p[3] != 'e' || p[4] != 'r' ||
		    p[5] != 't') {
			continue;
		}
		say("the firmware asserts at offset %#x; the bytes from %#x follow", at,
		    at >= 128u ? at - 128u : 0u);
		for (uint32_t line = (at >= 128u ? at - 128u : 0u);
		    line < at + 384u && line + 16u <= size; line += 16u) {
			const uint8_t *b = (const uint8_t *)segment->bytes + line;

			say("%06x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
			    "%02x %02x  |%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c|", line,
			    b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11],
			    b[12], b[13], b[14], b[15],
			    b[0] >= 0x20 && b[0] < 0x7f ? b[0] : '.', b[1] >= 0x20 && b[1] < 0x7f ? b[1] : '.',
			    b[2] >= 0x20 && b[2] < 0x7f ? b[2] : '.', b[3] >= 0x20 && b[3] < 0x7f ? b[3] : '.',
			    b[4] >= 0x20 && b[4] < 0x7f ? b[4] : '.', b[5] >= 0x20 && b[5] < 0x7f ? b[5] : '.',
			    b[6] >= 0x20 && b[6] < 0x7f ? b[6] : '.', b[7] >= 0x20 && b[7] < 0x7f ? b[7] : '.',
			    b[8] >= 0x20 && b[8] < 0x7f ? b[8] : '.', b[9] >= 0x20 && b[9] < 0x7f ? b[9] : '.',
			    b[10] >= 0x20 && b[10] < 0x7f ? b[10] : '.', b[11] >= 0x20 && b[11] < 0x7f ? b[11] : '.',
			    b[12] >= 0x20 && b[12] < 0x7f ? b[12] : '.', b[13] >= 0x20 && b[13] < 0x7f ? b[13] : '.',
			    b[14] >= 0x20 && b[14] < 0x7f ? b[14] : '.', b[15] >= 0x20 && b[15] < 0x7f ? b[15] : '.');
		}
		break;
	}
}

static void
umi_poll(void *parameter, wait_result_t result)
{
	uint8_t *datagram;

	(void)parameter;
	(void)result;
	say("pcie: the thread that watches the radio's rings is running");
	datagram = kalloc_data(IPCR_BUFFER_SIZE, Z_WAITOK | Z_ZERO);
	if (datagram == NULL) {
		say("the thread that watches the radio's rings has nowhere to put a datagram");
		return;
	}
	for (;;) {
		uint32_t length, slot;
		bool have, heartbeat = false;
		uint32_t rings_ready = 0, mhi_status = 0, channel_db = 0, wake_db = 0, wake_value = 0;
		uint64_t consumer = 0, producer[3] = { 0, 0, 0 };
		uint64_t events_memory = 0;

		lck_mtx_lock(&umi_lock);
		if (umi_up) {
			for (unsigned ring = 0; ring < MHI_EVENT_RING_COUNT; ring++) {
				(void)mhi_service_events(&umi, ring, UINT64_MAX, NULL, NULL);
			}
		}
		umi_poll_ticks++;
		if (umi_poll_ticks == 1u || (umi_poll_ticks % umi_poll_heartbeat_every) == 0u) {
			heartbeat = true;
			for (unsigned ring = 0; ring < MHI_EVENT_RING_COUNT; ring++) {
				if (mhi.event_ring[ring].bytes != 0) {
					rings_ready++;
				}
			}
			consumer = mhi_event_consumer[MHI_IPCR_EVENT_RING];
			events_memory = mhi.events.bytes;
			/*
			 * The producers live in the device's context entries and the ring contents are
			 * the device's too: invalidate before reading, exactly as the scan does.
			 */
			if (events_memory != 0) {
				dma_invalidate(&mhi.events);
			}
			for (unsigned ring = 0; ring < MHI_EVENT_RING_COUNT; ring++) {
				if (mhi.event_ring[ring].bytes != 0) {
					dma_invalidate(&mhi.event_ring[ring]);
				}
				if (events_memory != 0) {
					producer[ring] = mhi_context_read(
					    (const uint32_t *)(uintptr_t)events_memory +
					    ring * (MHI_CONTEXT_BYTES / 4u), 7);
				}
			}
			if (umi_up) {
				/*
				 * The host's half of mhi_assert_dev_wake(): 1 in the wake doorbell says the
				 * device is wanted awake, and it stays out of M3 until that is cleared.
				 * Upstream writes it the first time anything needs the device; this driver
				 * never wrote it, so the device was free to park in M3 between packets --
				 * which is exactly a 60 s silence followed by an answer the moment we send.
				 */
				mhi_status = prd(&umi, BAR + MHI_STATUS);
				channel_db = prd(&umi, BAR + MHI_CHANNEL_DB);
				wake_db = channel_db + 8u * MHI_DEV_WAKE_DB;
				/*
				 * A doorbell pinned at 1 is a level, not an event: the firmware held
				 * its downlink for 60 s until the next channel doorbell (docs section
				 * 48.7, lead 1).  Toggle, so every heartbeat hands it a fresh rising
				 * edge no matter how long the wait has been.
				 */
				mhi_reg_address(&umi, wake_db, 0);
				__asm__ volatile ("dsb sy" : : : "memory");
				mhi_reg_address(&umi, wake_db, 1);
				__asm__ volatile ("dsb sy" : : : "memory");
				wake_value = prd(&umi, BAR + wake_db);
			}
		}
		lck_mtx_unlock(&umi_lock);
		if (heartbeat) {
			say("pcie: heart %u: %u events, %u/%u rings, device state %u (status %#x), "
			    "wake %#x, producer %#llx/%#llx/%#llx, consumer1 %#llx",
			    umi_poll_ticks, events_seen_total, rings_ready, MHI_EVENT_RING_COUNT,
			    (mhi_status & MHI_STATUS_STATE) >> 8, mhi_status, wake_value,
			    producer[0], producer[1], producer[2], consumer);
			if (((mhi_status & MHI_STATUS_STATE) >> 8) == 0xffu && !umi_dumped) {
				umi_dumped = true;
				say("the device says SYS_ERR: its memory is read, once, for what it wrote there");
				umi_dump_firmware_memory();
			}
			/*
			 * Whether the device consumed what was rung.  Channel 20 is the one the QMI
			 * goes out on: if its read pointer never moves the device never looked at the
			 * ring element at all; if it moves and nothing comes back the device has it and
			 * chose not to answer.  Two different faults (docs section 37.3).
			 */
			if (mhi.channels.bytes != 0 && (umi_poll_ticks % (umi_poll_heartbeat_every * 10u)) == 0u) {
				const uint32_t *txcontext = (const uint32_t *)mhi.channels.bytes +
				    MHI_IPCR_TX_CHANNEL * (MHI_CONTEXT_BYTES / 4);
				const uint32_t *rxcontext = (const uint32_t *)mhi.channels.bytes +
				    MHI_IPCR_RX_CHANNEL * (MHI_CONTEXT_BYTES / 4);

				say("pcie: heart %u: channel 20 read pointer %#llx, our write pointer %#llx; "
				    "channel 21 write pointer %#llx, our read pointer %#llx",
				    umi_poll_ticks, mhi_context_read(txcontext, 7),
				    mhi_context_read(txcontext, 9), mhi_context_read(rxcontext, 9),
				    mhi_context_read(rxcontext, 7));
			}
		}
		/* Handed on outside that lock: the driver may answer, and answering takes it. */
		for (;;) {
			lck_mtx_lock(&umi_rx_lock);
			have = umi_rx.data != NULL && umi_rx.head != umi_rx.tail;
			if (have) {
				slot = umi_rx.tail;
				length = umi_rx.length[slot];
				memcpy(datagram, umi_rx.data + (size_t)slot * IPCR_BUFFER_SIZE, length);
				umi_rx.tail = (slot + 1u) % UMI_RX_SLOTS;
			}
			lck_mtx_unlock(&umi_rx_lock);
			if (!have) {
				break;
			}
			hakios_wifi_qrtr_input(datagram, length);
		}
		/* Services announced before the driver looked: told to it now, as the link's thread does. */
		hakios_wifi_qrtr_fresh();
		sleep_us(1000);
	}
}

void
hakios_pcie_poll_start(void)
{
	if (umi_rx.data == NULL) {
		umi_rx.data = kalloc_data((vm_size_t)UMI_RX_SLOTS * IPCR_BUFFER_SIZE,
		    Z_WAITOK | Z_ZERO);
		if (umi_rx.data == NULL) {
			say("no room for the datagrams the radio sends the driver");
			return;
		}
	}
	if (umi_polling) {
		return;
	}
	umi_polling = true;
	if (kernel_thread_start(umi_poll, NULL, &umi_thread) != KERN_SUCCESS) {
		say("the thread that watches the radio's rings did not start");
		umi_polling = false;
		return;
	}
	thread_deallocate(umi_thread);
	umi_poll_thread_running = true;
	say("the radio's rings are looked at every millisecond");
}

/* ---- start ------------------------------------------------------------------------------------ */

/*
 * One write of a value the register already holds, to the one rail that is certainly on
 * (ldoa5: enable 1, 880 mV, mode 7).  A write that changes nothing cannot brown anything out,
 * so if this alone ends the run, RPMh's write path is what cannot be used from here.
 */
static void
pcie_write_test(void)
{
	uint32_t address = rpmh_address("ldoa5"), value = 0;

	if (address == 0) {
		say("write test: RPMh has no ldoa5");
		return;
	}
	if (pcie_writetest == 1) {
		if (!rpmh_read(address + VRM_ENABLE, &value)) {
			say("write test: cannot read the enable");
			return;
		}
		say("write test: writing ldoa5's enable (%#x) back over itself", value);
		tcs_dump("before");
		if (!rpmh_write_mode(address + VRM_ENABLE, value)) {
			say("write test: the write did not complete");
			tcs_dump("after");
			return;
		}
		tcs_dump("after");
		say("write test: survived a write of the enable");
	} else if (pcie_writetest == 2) {
		if (!rpmh_read(address + VRM_VOLTAGE, &value)) {
			say("write test: cannot read the voltage");
			return;
		}
		say("write test: writing ldoa5's voltage (%u mV) back over itself", value);
		if (!rpmh_write(address + VRM_VOLTAGE, value)) {
			say("write test: the write did not complete");
			return;
		}
		say("write test: survived a write of the voltage");
	} else if (pcie_writetest == 3) {
		/*
		 * A read, then a write, then the sequencer's registers: the read proves the path in,
		 * and the dump proves whether the stores out reach the hardware.
		 */
		say("write test: first a read of ldoa5's enable, the thing that has always worked");
		if (!rpmh_read(address + VRM_ENABLE, &value)) {
			say("write test: even the read did not complete now");
			return;
		}
		say("write test: the read answered %#x", value);

		say("write test: now the same register, written");
		say("  before: msgid %#x addr %#x data %#x enable %#x wait %#x control %#x status %#x",
		    rd(TCS(TCS_CMD_MSGID(0))), rd(TCS(TCS_CMD_ADDR(0))), rd(TCS(TCS_CMD_DATA(0))),
		    rd(TCS(TCS_CMD_ENABLE)), rd(TCS(TCS_CMD_WAIT_FOR_CMPL)), rd(TCS(TCS_CONTROL)),
		    rd(TCS(TCS_STATUS)));
		wr(TCS(TCS_CMD_ENABLE), 1u);
		say("  after enable=1: enable %#x", rd(TCS(TCS_CMD_ENABLE)));
		wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
		say("  after control=ENABLE: control %#x", rd(TCS(TCS_CONTROL)));
		wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);
		say("  after control=ENABLE|TRIGGER: control %#x status %#x", rd(TCS(TCS_CONTROL)),
		    rd(TCS(TCS_STATUS)));
		if (!rpmh_write(address + VRM_ENABLE, value)) {
			say("write test: the write did not complete");
			say("  after: msgid %#x addr %#x data %#x enable %#x wait %#x control %#x status %#x",
			    rd(TCS(TCS_CMD_MSGID(0))), rd(TCS(TCS_CMD_ADDR(0))), rd(TCS(TCS_CMD_DATA(0))),
			    rd(TCS(TCS_CMD_ENABLE)), rd(TCS(TCS_CMD_WAIT_FOR_CMPL)), rd(TCS(TCS_CONTROL)),
			    rd(TCS(TCS_CMD_STATUS)));
			return;
		}
		say("write test: the write completed");
	} else if (pcie_writetest == 5) {
		/*
		 * The same write, several message IDs.  A read completes with 0x108; a write with
		 * 0x10008 does not, and the two differ only in bit 16.  One run settles which ID the
		 * sequencer will actually scan.
		 */
		/*
		 * Write-type IDs only.  The first attempt at this put the read ID (0x108) at the
		 * front of the table and stopped on it, so every write candidate went untested.
		 * 0x10108 is what Linux sends: LEN | RESP_REQ | WRITE, RESP_REQ because the driver
		 * also sets WAIT_FOR_CMPL.
		 */
		static const struct { uint32_t msgid; const char *what; } ids[] = {
			{ 0x10108,  "LEN(8)|RESP_REQ|WRITE, what Linux sends when it waits" },
			{ 0x10008,  "LEN(8)|WRITE, what this driver has been sending" },
			{ 0x10104,  "LEN(4)|RESP_REQ|WRITE" },
			{ 0x10010,  "LEN(16)|WRITE" },
		};

		if (!rpmh_read(address + VRM_ENABLE, &value)) {
			say("write test: cannot read the enable");
			return;
		}
		say("write test: writing %#x over ldoa5's enable with %u IDs", value,
		    (uint32_t)(sizeof(ids) / sizeof(ids[0])));
		for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
			bool done = rpmh_send(ids[i].msgid, address + VRM_ENABLE, value, NULL);

			say("  ID %#x (%s): %s", ids[i].msgid, ids[i].what,
			    done ? "COMPLETED" : "not scanned");
			if (done) {
				break;
			}
		}
		say("write test: done");
	} else if (pcie_writetest == 6) {
		/*
		 * Read-only survey of the sequencer.  The trigger bit sticking (CONTROL = 0x1010000 after
		 * the final write) says the sequencer is not consuming slots; these registers say what
		 * state it is in and which slots are even of the kind that would be scanned.
		 */
		say("rsc: drv_config %#x", rd(rsc + 0x00c));
		say("rsc: status %#x irq_enable %#x irq_status %#x", rd(rsc + 0x000), rd(rsc + 0x004),
		    rd(rsc + 0x008));
		say("rsc: cmd_wait_for_cmpl %#x control %#x", rd(rsc + 0x010), rd(rsc + 0x014));
		say("rsc: tcs_offset %#x cmd_offset %#x", rd(rsc + 0x0d00), rd(rsc + 0x0d04));
		for (unsigned n = 0; n < 8; n++) {
			say("  TCS%u: status %#x control %#x enable %#x wait %#x irqen %#x irqst %#x cmdst %#x", n,
			    rd(TCS_N(n, TCS_STATUS)), rd(TCS_N(n, TCS_CONTROL)),
			    rd(TCS_N(n, TCS_CMD_ENABLE)), rd(TCS_N(n, TCS_CMD_WAIT_FOR_CMPL)),
			    rd(TCS_N(n, TCS_IRQ_ENABLE)), rd(TCS_N(n, TCS_IRQ_STATUS)),
			    rd(TCS_N(n, TCS_CMD_STATUS)));
		}
		say("rsc: dump done");
	} else if (pcie_writetest == 7) {
		/*
		 * The dump says TCS0 is dirty before this driver touches anything (TRIGGER still set,
		 * CMD_STATUS 0x10101) and TCS1..3 are clean.  Put the same harmless write through each
		 * in turn: the one that completes is the one the sequencer will actually scan.
		 */
		if (!rpmh_read(address + VRM_ENABLE, &value)) {
			say("write test: cannot read the enable");
			return;
		}
		say("write test: writing %#x over ldoa5's enable, one slot at a time", value);
		for (unsigned slot = 0; slot < 4; slot++) {
			uint32_t before = rd(TCS_N(slot, TCS_CMD_STATUS));
			bool done = rpmh_send_slot(slot, MSGID_WRITE_4_BYTES, address + VRM_ENABLE, value,
			    NULL);

			say("  TCS%u (cmd_status before %#x): %s", slot, before,
			    done ? "COMPLETED" : "not scanned");
			if (done) {
				break;
			}
		}
		say("write test: done");
	}
}

static void
pcie_run(void)
{
	static struct pcie pcie;
	int error;

	say("steps 1..%u, upto %u, power %u, railmask %#x, writemode %u, writetest %u, rpmhmode %u",
	    pcie_steps, pcie_upto, pcie_vote, pcie_railmask, pcie_writemode, pcie_writetest,
	    pcie_rpmhmode);
	if (pcie_writetest != 0) {
		/*
		 * The command database and RPMh have to be mapped for this, and the survey below maps
		 * them; do it here first so that the write is the very first thing that touches
		 * hardware in this run.
		 */
		rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
		cmddb = ml_io_map(CMD_DB_PHYS, CMD_DB_SIZE);
		if (rsc == 0 || cmddb == 0) {
			say("write test: cannot map RPMh (%#x) or its command database (%#x)", RSC_PHYS,
			    CMD_DB_PHYS);
			return;
		}
		pcie_write_test();
		return;
	}

	/*
	 * Step 1 in pieces, because as one piece it killed the machine without a word.  GCC first, on
	 * its own, then the pins, then RPMh's command database, and only then the PCIe windows -- so
	 * that the last line the live log holds names the one that did it.  Nothing here writes.
	 */
	say("probe a: GCC");
	gcc = ml_io_map_unmappable(GCC_PHYS, GCC_SIZE, VM_WIMG_IO);
	if (gcc == 0) {
		say("cannot map GCC at %#x", GCC_PHYS);
		return;
	}
	say("probe a: GCC reads %#x", rd(gcc + 0x6b004));

	say("probe b: the pins");
	tlmm = ml_io_map_unmappable(TLMM_NORTH_PHYS, TLMM_NORTH_SIZE, VM_WIMG_IO);
	if (tlmm == 0) {
		say("cannot map the north tile at %#x", TLMM_NORTH_PHYS);
		return;
	}
	say("probe b: GPIO20 cfg %#x io %#x", rd(tlmm + 20 * 0x1000), rd(tlmm + 20 * 0x1000 + 4));

	say("probe c: the command database");
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	cmddb = ml_io_map(CMD_DB_PHYS, CMD_DB_SIZE);
	if (rsc == 0 || cmddb == 0) {
		say("cannot map RPMh (%#x) or its command database (%#x)", RSC_PHYS, CMD_DB_PHYS);
		return;
	}
	say("probe c: smpa6=%#x", rpmh_address("smpa6"));

	say("probe d: the PCIe windows");
	if (!pcie_map_regions()) {
		return;
	}
	say("probe d: mapped, nothing read");

	pcie_survey();
	if (pcie_steps < 2) {
		return;
	}
	say("step 2: rails, wlan-en, the PCIe0 link; the endpoint's bus mastering stays off");
	memset(&pcie, 0, sizeof(pcie));
	error = pcie_enumerate(&pcie);
	say("done: errno %d%s", error, error == 0 ? "" : "; the endpoint is back in reset");
}

/*
 * Once, from a thread call a few seconds in: the enumeration waits on a link, and that is no place
 * for the start-up thread. Every other driver that runs at this rank defers the same way
 * (hakios.thermal.c, hakios.pmic.c, hakios.battguard.c). A kernel thread started from here instead
 * is what kept the kernel from starting at all: STARTUP(EARLY_BOOT) runs before there is a
 * scheduler to put one on.
 */
static void
pcie_run_thread(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	pcie_run();
}

__startup_func
static void
pcie_init(void)
{
	thread_call_t call;
	uint64_t deadline;
	uint32_t on = 0;

	(void)PE_parse_boot_argn("pcie", &on, sizeof(on));
	(void)PE_parse_boot_argn("pcie_step", &pcie_steps, sizeof(pcie_steps));
	(void)PE_parse_boot_argn("pcie_delay", &pcie_start_delay, sizeof(pcie_start_delay));
	(void)PE_parse_boot_argn("pcie_upto", &pcie_upto, sizeof(pcie_upto));
	(void)PE_parse_boot_argn("pcie_power", &pcie_vote, sizeof(pcie_vote));
	(void)PE_parse_boot_argn("pcie_revote", &pcie_revote, sizeof(pcie_revote));
	(void)PE_parse_boot_argn("pcie_swreset", &pcie_swreset, sizeof(pcie_swreset));
	(void)PE_parse_boot_argn("pcie_linkretry", &pcie_linkretry, sizeof(pcie_linkretry));
	(void)PE_parse_boot_argn("pcie_railmask", &pcie_railmask, sizeof(pcie_railmask));
	(void)PE_parse_boot_argn("pcie_writemode", &pcie_writemode, sizeof(pcie_writemode));
	(void)PE_parse_boot_argn("pcie_writetest", &pcie_writetest, sizeof(pcie_writetest));
	(void)PE_parse_boot_argn("pcie_rpmhmode", &pcie_rpmhmode, sizeof(pcie_rpmhmode));
	(void)PE_parse_boot_argn("pcie_smmu_mode", &pcie_smmu_mode, sizeof(pcie_smmu_mode));
	(void)PE_parse_boot_argn("pcie_image_wait", &pcie_image_wait, sizeof(pcie_image_wait));
	(void)PE_parse_boot_argn("pcie_sbl", &pcie_sbl_send, sizeof(pcie_sbl_send));
	(void)PE_parse_boot_argn("pcie_mhi", &pcie_mhi_mode, sizeof(pcie_mhi_mode));
	(void)PE_parse_boot_argn("pcie_amss", &pcie_amss_mode, sizeof(pcie_amss_mode));
	(void)PE_parse_boot_argn("pcie_amss_wait", &pcie_amss_wait, sizeof(pcie_amss_wait));
	(void)PE_parse_boot_argn("pcie_nop", &pcie_nop_mode, sizeof(pcie_nop_mode));
	(void)PE_parse_boot_argn("pcie_nop_delay", &pcie_nop_delay,
	    sizeof(pcie_nop_delay));
	(void)PE_parse_boot_argn("pcie_cmd", &pcie_cmd, sizeof(pcie_cmd));
	(void)PE_parse_boot_argn("pcie_cmd_channel", &pcie_cmd_channel, sizeof(pcie_cmd_channel));
	(void)PE_parse_boot_argn("pcie_ipcr", &pcie_ipcr_mode, sizeof(pcie_ipcr_mode));
	(void)PE_parse_boot_argn("pcie_ipcr_wait", &pcie_ipcr_wait, sizeof(pcie_ipcr_wait));
	(void)PE_parse_boot_argn("pcie_wlfw", &pcie_wlfw_mode, sizeof(pcie_wlfw_mode));
	(void)PE_parse_boot_argn("pcie_mem_phys", &pcie_mem_phys, sizeof(pcie_mem_phys));
	(void)PE_parse_boot_argn("pcie_fixed_iova", &pcie_fixed_iova, sizeof(pcie_fixed_iova));
	(void)PE_parse_boot_argn("pcie_stage20", &pcie_stage20_mode,
	    sizeof(pcie_stage20_mode));
	(void)PE_parse_boot_argn("pcie_bdf", pcie_bdf_name, sizeof(pcie_bdf_name));
	(void)PE_parse_boot_argn("pcie_bdf_send", &pcie_bdf_send, sizeof(pcie_bdf_send));
	(void)PE_parse_boot_argn("pcie_bdf_id", &pcie_bdf_id, sizeof(pcie_bdf_id));

	(void)PE_parse_boot_argn("pcie_ceinit", &pcie_ce_init, sizeof(pcie_ce_init));
	(void)PE_parse_boot_argn("pcie_cfg_send", &pcie_cfg_send, sizeof(pcie_cfg_send));
	(void)PE_parse_boot_argn("pcie_ini_send", &pcie_ini_send, sizeof(pcie_ini_send));
	(void)PE_parse_boot_argn("pcie_cfg_parts", &pcie_cfg_parts, sizeof(pcie_cfg_parts));
	(void)PE_parse_boot_argn("pcie_cfg_after", &pcie_cfg_after, sizeof(pcie_cfg_after));
	(void)PE_parse_boot_argn("pcie_mode_after", &pcie_mode_after, sizeof(pcie_mode_after));
	(void)PE_parse_boot_argn("pcie_ce_rx", &pcie_ce_rx, sizeof(pcie_ce_rx));
	(void)PE_parse_boot_argn("pcie_htc", &pcie_htc, sizeof(pcie_htc));
	(void)PE_parse_boot_argn("pcie_second", &pcie_second, sizeof(pcie_second));
	(void)PE_parse_boot_argn("pcie_cfg_during", &pcie_cfg_during, sizeof(pcie_cfg_during));
	(void)PE_parse_boot_argn("pcie_shadow", &pcie_shadow, sizeof(pcie_shadow));
	(void)PE_parse_boot_argn("pcie_bdf_piece", &pcie_bdf_piece, sizeof(pcie_bdf_piece));
	(void)PE_parse_boot_argn("pcie_stage21", &pcie_stage21_mode,
	    sizeof(pcie_stage21_mode));
	(void)PE_parse_boot_argn("pcie_mode", &pcie_mode, sizeof(pcie_mode));
	(void)PE_parse_boot_argn("pcie_cesurvey", &pcie_ce_survey, sizeof(pcie_ce_survey));
	(void)PE_parse_boot_argn("pcie_fwfile", &pcie_sbl_from_file, sizeof(pcie_sbl_from_file));
	(void)PE_parse_boot_argn("pcie_panic", &pcie_panic, sizeof(pcie_panic));
	(void)PE_parse_boot_argn("pcie_sbl_wait", &pcie_sbl_wait, sizeof(pcie_sbl_wait));
	(void)PE_parse_boot_argn("pcie_fail_panic", &pcie_fail_panic, sizeof(pcie_fail_panic));
	if (pcie_upto > 19) {
		pcie_upto = 19;
	}
	if (pcie_steps == 0 && on) {
		pcie_steps = 2;
	}
	if (pcie_steps == 0) {
		return;
	}
	if (pcie_steps > 2) {
		pcie_steps = 2;         /* steps 3 and up are not written yet */
	}
	call = thread_call_allocate(pcie_run_thread, NULL);
	if (call == NULL) {
		say("the thread call for the link was not allocated");
		return;
	}
	clock_interval_to_deadline(pcie_start_delay, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, pcie_init);

SYSCTL_INT(_kern_hakios, OID_AUTO, pcie_id, CTLFLAG_RD | CTLFLAG_LOCKED, &pcie_id, 0,
    "the vendor:device the QCA6390 answered on PCIe0 with (0 until a run with pcie_step=2)");
