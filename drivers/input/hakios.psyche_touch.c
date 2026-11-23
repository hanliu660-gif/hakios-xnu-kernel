/*
 * psyche_touch.c —— ST FTS V521 over SPI：小米 12X（psyche / SM8250-AC / kona）的触摸驱动
 *
 * 移植自 Android 的 drivers/input/touchscreen/fts_521/（psyche 的内核配置
 * CONFIG_TOUCHSCREEN_ST_FTS_V521_SPI=y）。SPI/GENI/bitbang 的底层则是复用 HakiOS
 * 现有 drivers/input/hakios.goodix.c 的实现，只换地址与引脚。
 *
 * ===== 和 redwood 那个 goodix.c 的关系 =====
 * 引脚、基址、芯片全部不同；**协议也完全不同**：
 *   FocalTech： [addr][0xA0][lenH][lenL][0,0,0][data][CRC16LE]   —— 有帧头、有 CRC、有状态字节
 *   ST FTS  ： [cmd][addr...]  然后直接读 (len + 1) 字节，丢第一个  —— 无 CRC
 *
 * ===== 协议（全部据 fts_521 源码，行号见各常量注释）=====
 *   fts_read (cmd, addr, addr_size, out, len, dummy)
 *       写 [cmd][addr...]；再读 (len + 1)；丢第 1 个字节
 *   fts_write(cmd, addr, addr_size, data, len)
 *       写 [cmd][addr...][data...]
 *   FIFO 读      cmd = 0x87（SPI 模式；I2C 模式才是 0x86），无地址，dummy = 1
 *   事件 8 字节，第 7 字节的 [4:0] = FIFO 里还剩几个
 *
 * ===== 硬件（真机 DTB 实测：/sys/firmware/fdt 842,746 字节）=====
 *   SE4          spi@990000   0x990000 + 0x4000   status=ok
 *   fts@0        compatible "st,spi"              status=ok
 *   MISO/MOSI/CLK/CS = gpio 8 / 9 / 10 / 11
 *   IRQ = gpio39（低电平 = 有事件），RESET = gpio33（低 = 复位）
 *   avdd = PMIC L10C 轨，vdd = PMIC L7A 轨（都不是 GPIO）
 *   TLMM = 0x0F000000（16 MB），GCC = 0x00100000，RSC = 0x18220000，cmd-db = 0x80860000
 *
 * ===== GCC 时钟（wrap0 / SE4），由 drivers/clk/qcom/gcc-kona.c 查实 =====
 *   gcc_qupv3_wrap0_s4_clk_src.cmd_rcgr = 0x174d0  →  CMD +0, CFG +4, M +8, N +0xc, D +0x10
 *                                                DFSR +0x14, PERF_CFG +0x1c/+0x5c/+0x9c
 *   gcc_qupv3_wrap0_s4_clk.halt_reg     = 0x174cc  (CBCR)，enable_mask BIT(14) @ 0x52008
 *   gcc_qupv3_wrap_0_m_ahb_clk.halt_reg = 0x17004  (enable BIT(6))
 *   gcc_qupv3_wrap_0_s_ahb_clk.halt_reg = 0x17008  (enable BIT(7))
 *   gcc_qupv3_wrap0_core_clk.halt_reg   = 0x23000  (enable BIT(8))
 *   gcc_qupv3_wrap0_core_2x_clk.halt_reg= 0x23008  (enable BIT(9))
 *
 * ===== 坐标 =====
 *   两个小端 16 位字段：
 *     x = e[2] | (e[3] << 8)
 *     y = e[4] | (e[5] << 8)
 *   满量程按 DTS 的 fts,x-max=10800 / fts,y-max=24000，可用
 *   touch_x_max= / touch_y_max= 在运行时覆盖。
 *
 *   为什么不是 ST SDK 文档里的 12 位打包（x 取 e[2] 与 e[3] 低半字节，
 *   y 取 e[4] 与 e[3] 高半字节）：那样 y 用了 e[3] 的高半字节，而 e[3]
 *   是 x 的高字节。真机连续拖动时 y 从 4081 跳到 465，单调性只有 31/37；
 *   按小端 16 位解释，x 和 y 都是 37/37 单调，e[5] 随手指下移平滑递增、
 *   e[4] 每 256 回绕。本芯片是 super-resolution（DTS 里
 *   fts,support-super-resolution = <1>），12 位公式属于老固件。
 *
 * ===== boot-args =====
 *   touchscreen=1       启用（不写就不启动）
 *   touch_poll_us=N     轮询间隔微秒，默认 8000 = 125 Hz（够用；最小 1000）
 *   touch_spi_hz=N      目标 SPI 时钟，默认 8000000（DTS 写的 8 MHz）
 *   touch_bitbang=1     强制走 GPIO 位翻转（跳过 GENI）
 *   touch_lat=1         每次读报告打印一行延迟测量
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/hakios.input.h>
#include <libkern/libkern.h>
#include <sys/kernel.h>

extern void hakios_tl(const char *format, ...) __printflike(1, 2);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

#if defined(__arm64__) || defined(__aarch64__)
#define barrier() __asm__ volatile("dmb sy" ::: "memory")
#else
#define barrier() __asm__ volatile("" ::: "memory")
#endif

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	barrier();
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ========== 物理地址（psyche 实测）==================================================== */

/*
 * The pin controller of this SoC is three tiles, not one block. The phone's own kernel says so
 * (pinctrl-kona.c: NORTH 0x00900000, SOUTH 0x00500000, WEST 0x00100000, all relative to
 * 0x0F000000), and every pin's registers sit at tile + 0x1000 * its absolute pin number:
 *
 *   WEST   0x0F100000   GPIO 133..179
 *   SOUTH  0x0F500000   GPIO 0..3, 24..27, 32..65, 69..76, 85..91, 125..132
 *   NORTH  0x0F900000   GPIO 4..23, 28..31, 66..68, 77..84, 92..124
 *
 * This phone's pins: MISO/MOSI/CLK/CS = 8/9/10/11 (NORTH), RESET 33 and IRQ 39 (SOUTH).
 * 0x0F000000 belongs to none of them: every word read there was zero, a write did not come
 * back, and reading past the first word killed the kernel.
 */
#define TILE_WEST_PHYS          0x0F100000UL
#define TILE_SOUTH_PHYS         0x0F500000UL
#define TILE_NORTH_PHYS         0x0F900000UL
#define TILE_SIZE               0x00080000UL   /* 512 KB: covers pins 0..127 in each of
                                              * the three tiles.  It was 0x40000, which
                                              * stops at pin 63 and left GPIO 69 (touch
                                              * VDDIO) unmapped -- the write that turns
                                              * that supply on landed past the end of
                                              * the mapping and took the machine with
                                              * it.  The full 4 MB spacing between the
                                              * tiles would be the safe window, but 12
                                              * MB does not fit: kmem_alloc returns
                                              * KERN_NO_SPACE this early in boot. */
#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x60000UL
#define SE_PHYS                 0x00990000UL
#define SE_SIZE                 0x4000UL
/*
 * The RPMh window this system uses. The device tree's rsc has three windows and says
 * qcom,drv-id = <2>, so it is the third: 0x18220000. The sequence blocks sit at tcs-offset
 * (0xd00) from that base. Using the first window made every write hang.
 */
#define RSC_PHYS                0x18220000UL
#define RSC_SIZE                0x10000UL
#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL

/* ========== 引脚 ===================================================================== */

static vm_offset_t tlmm_west, tlmm_south, tlmm_north;

/* which tile a pin's registers live in */
static bool
pin_is_north(unsigned pin)
{
	return (pin >= 4 && pin <= 23) || (pin >= 28 && pin <= 31) || (pin >= 66 && pin <= 68) ||
	       (pin >= 77 && pin <= 84) || (pin >= 92 && pin <= 124);
}

static vm_offset_t
pin_base(unsigned pin)
{
	if (pin >= 133) {
		return tlmm_west;
	}
	if (pin_is_north(pin)) {
		return tlmm_north;
	}
	return tlmm_south;
}

#define GPIO_CFG(n)             (pin_base(n) + 0x1000UL * (n))   /* [1:0]上拉 [5:2]功能 [8:6]驱动 [9]输出 */
#define GPIO_IO(n)              (pin_base(n) + 0x1000UL * (n) + 4) /* bit0 输入 bit1 输出 */

#define PULL_NONE               0
#define PULL_UP                 3

#define PIN_MISO                8
#define PIN_MOSI                9
#define PIN_CLK                 10
#define PIN_CS                  11
#define PIN_INT                 39      /* 低 = 有报告 */
#define PIN_RESET               33      /* 低 = 复位中 */
#define PIN_VDDIO               69      /* touch_vddio_vreg 的硬开关，DTS: enable-active-high */
#define PIN_FUNCTION_SPI        1       /* "qup4" */

/* ========== GENI / SPI 寄存器（与 redwood 同一套，只是基址不同）======================== */

#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)    /* bit0: 命令在跑 */
#define GENI_SER_M_CLK_CFG      (se + 0x048)    /* bit0 使能, [15:4] 分频 */
#define GENI_IF_DISABLE_RO      (se + 0x064)    /* bit0: 无 FIFO 接口 */
#define GENI_FW_REVISION_RO     (se + 0x068)    /* [15:8] 协议: 1 = SPI */
#define GENI_CLK_SEL            (se + 0x07c)
#define SPI_CPHA                (se + 0x224)
#define SPI_LOOPBACK            (se + 0x22c)
#define SPI_CPOL                (se + 0x230)
#define SPI_DEMUX_OUTPUT_INV    (se + 0x24c)
#define SPI_DEMUX_SEL           (se + 0x250)
#define GENI_BYTE_GRAN          (se + 0x254)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define SPI_TRANS_CFG           (se + 0x25c)
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_TX_PACKING_CFG1    (se + 0x264)
#define SPI_WORD_LEN            (se + 0x268)
#define SPI_TX_TRANS_LEN        (se + 0x26c)
#define SPI_RX_TRANS_LEN        (se + 0x270)
#define SPI_DELAY_COUNTERS      (se + 0x278)
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_RX_PACKING_CFG1    (se + 0x288)
#define GENI_M_CMD0             (se + 0x600)
#define GENI_M_CMD_CTRL         (se + 0x604)
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_IRQ_CLEAR        (se + 0x648)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_TX_FIFO_STATUS     (se + 0x800)
#define GENI_RX_FIFO_STATUS     (se + 0x804)
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_DMA_TX_IRQ_CLR       (se + 0xc44)
#define SE_DMA_RX_IRQ_CLR       (se + 0xd44)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)
#define SE_DMA_GENERAL_CFG      (se + 0xe30)

#define M_CMD_DONE              (1u << 0)
#define M_CMD_CANCEL            (1u << 4) /* IRQ status */
#define M_CMD_ABORT             (1u << 5) /* IRQ status */
#define M_CTRL_CANCEL           (1u << 2)
#define M_CTRL_ABORT            (1u << 1)
#define M_ERRORS                ((1u << 1) | (1u << 2) | (1u << 3) | \
                                 (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))
#define SPI_TX_ONLY             (1u << 27)
#define SPI_RX_ONLY             (2u << 27)
#define SPI_FULL_DUPLEX         (3u << 27)
#define SPI_FRAGMENTATION       (1u << 2)

/* ========== GCC 时钟（wrap0 / SE4）—— gcc-kona.c 查实 ================================ */

#define GCC_VOTE                (gcc + 0x52008)
#define VOTE_WRAP0_M_AHB        (1u << 6)
#define VOTE_WRAP0_S_AHB        (1u << 7)
#define VOTE_WRAP0_CORE         (1u << 8)
#define VOTE_WRAP0_CORE_2X      (1u << 9)
#define VOTE_WRAP0_S4           (1u << 14)

#define GCC_WRAP0_M_AHB_CBCR    (gcc + 0x17004)
#define GCC_WRAP0_S_AHB_CBCR    (gcc + 0x17008)
#define GCC_WRAP0_CORE_CBCR     (gcc + 0x23000)
#define GCC_WRAP0_CORE_2X_CBCR  (gcc + 0x23008)
#define GCC_WRAP0_S4_CBCR       (gcc + 0x174cc)
#define GCC_WRAP0_S4_CMD        (gcc + 0x174d0)   /* bit0: 提交新配置 */
#define GCC_WRAP0_S4_CFG        (gcc + 0x174d4)   /* [4:0] 2*div-1, [10:8] 源, [13:12] 模式 */
#define GCC_WRAP0_S4_M          (gcc + 0x174d8)
#define GCC_WRAP0_S4_N          (gcc + 0x174dc)
#define GCC_WRAP0_S4_D          (gcc + 0x174e0)
#define GCC_WRAP0_S4_DFSR       (gcc + 0x174e4)   /* bit0: 引擎自己选预设速率 */
#define GCC_WRAP0_S4_PERF_CFG(l) (gcc + 0x174ec + 4 * (l))
#define GCC_WRAP0_S4_PERF_M(l)  (gcc + 0x1752c + 4 * (l))
#define GCC_WRAP0_S4_PERF_N(l)  (gcc + 0x1756c + 4 * (l))

#define CLOCK_OFF               (1u << 31)
#define XO_HZ                   19200000

/* ========== ST FTS V521（SPI 模式）====================================================
 * 注释里的行号是 Android 源码 fts_521/ 里的位置，便于复核。
 */

#define FTS_FIFO_CMD_READALL        0x87    /* ftsHardware.h:115  (I2C 才是 0x86) */
#define FTS_CMD_SCAN_MODE           0xA0    /* ftsSoftware.h:56 */
#define SCAN_MODE_ACTIVE            0x00    /* ftsSoftware.h:86 */
#define FTS_DUMMY_FIFO              1       /* ftsHardware.h:140  (I2C 才是 0)    */
#define FTS_FIFO_EVENT_SIZE         8       /* ftsHardware.h:109 */
#define FTS_FIFO_DEPTH              32      /* ftsHardware.h:110 */
#define FTS_EVENTS_REMAINING_POS    7       /* fts.c:4652 */
#define FTS_EVENTS_REMAINING_MASK   0x1F    /* fts.c:4653 */

#define FTS_CMD_HW_REG_R            0xFB    /* ftsHardware.h:127 (I2C 才是 0xFA) */
#define FTS_CMD_HW_REG_W            0xFA    /* ftsHardware.h:128 */
#define FTS_ADDR_SYSTEM_RESET       0x0000000020000024ULL  /* ftsHardware.h:159 */
#define FTS_SYSTEM_RESET_VALUE      0x80    /* ftsHardware.h:160 */
#define FTS_ADDR_DCHIP_ID           0x0000000020000000ULL
#define FTS_ADDR_SIZE_HW_REG        4    /* BITS_32: the hardware-register commands take four address bytes */  /* ftsHardware.h:169 */
#define FTS_ADDR_IER                0x0000000020000029ULL  /* ftsHardware.h:166 */
#define FTS_IER_ENABLE              0x41    /* ftsHardware.h:45 */
#define FTS_IER_DISABLE             0x00    /* ftsHardware.h:46 */
#define FTS_DCHIP_ID_0              0x36    /* ftsHardware.h:25 */
#define FTS_DCHIP_ID_1              0x39    /* ftsHardware.h:26 */

#define FTS_EVT_NOEVENT             0x00    /* ftsSoftware.h:273 */
#define FTS_EVT_CONTROLLER_READY    0x03    /* ftsSoftware.h:274 */
#define FTS_EVT_ENTER_POINT         0x13    /* ftsSoftware.h:275 */
#define FTS_EVT_MOTION_POINT        0x23    /* ftsSoftware.h:276 */
#define FTS_EVT_LEAVE_POINT         0x33    /* ftsSoftware.h:277 */

/*
 * The panel coordinates arrive as two packed 12-bit fields:
 *
 *   x = ((e[3] & 0x0F) << 8) | e[2]
 *   y = (e[4] << 4) | ((e[3] & 0xF0) >> 4)
 *
 * so each axis is 0..4095 whatever the panel is.  The device tree says 10800 and
 * 24000, but those are the super-resolution values (fts,support-super-resolution
 * in the psyche DT) and only apply once the chip has been given ST's config.  This
 * driver never loads one -- it only reads the 0xA7 system info -- so the chip runs
 * in its default 12-bit mode.  Measured: a sweep across the whole screen stopped at
 * raw 4090 in x and 3630 in y, i.e. the 12-bit ceiling, while the driver was still
 * dividing by ten and clamping at 1080, which is why touches only ever landed in
 * the top left corner.
 */
/*
 * Two-point calibration, measured on this phone: touching the four corners of the
 * display gave
 *
 *     top left  (1985, 1255)     top right    (9445, 1315)
 *     bottom left (1755, 22965)  bottom right (9155, 23165)
 *
 * The order came out right (the top row has the small y, the left column the small
 * x), which is what settled the frame layout: the 12-bit reading the ST SDK
 * documents mixes the axes and would have put the top left at a larger x than the
 * top right.
 *
 * Scaling those straight from the device tree (fts,x-max=10800, fts,y-max=24000)
 * put a corner touch at 18%..87% of the width, so the pointer never reached the
 * sides.  Mapping the measured corners onto the panel instead makes a corner touch
 * land in the corner.  The y numbers are consistent with the device tree -- 5% in
 * from each end is about a fingertip -- but the x ones are not, so the measured
 * ends win.
 *
 * Every value can be overridden at boot (touch_x_min= and friends), so a wrong
 * calibration costs one run rather than a build.
 */
#define FTS_X_MIN_DEFAULT           1870
#define FTS_X_MAX_DEFAULT           9300
#define FTS_Y_MIN_DEFAULT           1285
#define FTS_Y_MAX_DEFAULT           23065

#define FTS_X_SIZE_DEFAULT          1080    /* the panel, in pixels */
#define FTS_Y_SIZE_DEFAULT          2400

static uint32_t fts_x_min = FTS_X_MIN_DEFAULT;
static uint32_t fts_x_max = FTS_X_MAX_DEFAULT;
static uint32_t fts_y_min = FTS_Y_MIN_DEFAULT;
static uint32_t fts_y_max = FTS_Y_MAX_DEFAULT;
#define FTS_MAX_TOUCHES             10      /* fts.h: TOUCH_ID_MAX */

#define FTS_TX_SIZE                 (9 + FTS_FIFO_EVENT_SIZE * FTS_FIFO_DEPTH + 8)
#define FTS_RX_SIZE                 (FTS_TX_SIZE + 8)

/* ========== RPMh（L10C / L7A 供电）=================================================== */

#define TCS(reg)                (rsc + 0xd00 + (reg))
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18
#define TCS_CMD_ENABLE          0x1c      /* bitmap, one bit per slot in use */
#define TCS_CMD_WAIT_FOR_CMPL   0x20      /* bitmap, which TCS to wait for completion */
#define TCS_CMD_STATUS          0x3c
#define TCS_CMD_ISSUED          (1u << 8)     /* rsc_drv: CMD_STATUS_ISSUED */
#define TCS_CMD_COMPLETED       (1u << 16)    /* rsc_drv: CMD_STATUS_COMPL */
#define RSC_CMD_RESP_DATA       0x40          /* rsc_drv: RSC_DRV_CMD_RESP_DATA */
#define MSGID_READ_4_BYTES      0x108         /* LEN(8) | RESP_REQ(BIT8), no WRITE(BIT16) */
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008

#define VRM_VOLTAGE             0       /* rsc_drv: RPMH_REGULATOR_REG_VRM_VOLTAGE = addr + 0 */
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7

/* ========== 状态 ===================================================================== */

static vm_offset_t gcc, se, rsc, cmddb;
static uint32_t fifo_depth;
static uint32_t se_source_hz, se_clock_select;
static uint32_t touch_poll_us = 8000;
/*
 * 1 MHz, not the device tree^s 8 MHz. Measured on this board (logs/231434-run.log, twenty
 * reads timed at each rate): at the DFS table^s 6.4 MHz point every read came back with no
 * signature, at 19.2 MHz source levels they all passed. The diagnostics entry points call
 * geni_spi_init(1000000) and are the only place the chip has answered, so the normal path has
 * to use the same rate -- an 8 MHz request divides down to the same 6.4 MHz that fails.
 */
static uint32_t target_spi_hz = 1000000;

static struct fts_contact {
	bool        down;
	uint32_t    x, y;
} contacts[FTS_MAX_TOUCHES];

static uint32_t stat_polls, stat_reports, stat_touches, stat_bad;
static uint32_t stat_irq_low;   /* polls that found the interrupt line low */
static uint32_t stat_nonempty;  /* reads that came back with a real event */
static uint32_t stat_last_x, stat_last_y;
static bool force_bitbang;
static bool do_wirecheck;
static bool do_cstest;
static bool do_genidrive;
static bool do_clkprobe;
static bool do_bbprobe;
static bool do_gpioset;
static bool do_bbcount;
static bool do_chipcheck;
static bool do_rstseq;
static bool do_fastpoll;
static bool do_misostat;
static bool do_crosstalk;
static bool do_3wire;
static bool do_misopull;
static bool do_setvolt;
static bool do_sestate;
static bool do_useexisting;
static bool do_untouched;
static bool do_wrapper;
static bool do_wrapper2;
static bool do_wrapper3;
static bool do_wrapper4;
static bool do_ios;
static uint32_t psyche_loopback;
static bool trace_rx;
static bool trace_latency;
static uint32_t prob_only;      /* touch_probonly=1: prove the probe, then stop */
static uint32_t touch_rawdump;    /* touch_rawdump=N: dump the first N events raw */
static uint32_t touch_step;     /* touch_step=N: panic at checkpoint N */
static uint32_t psyche_dump_db;
static uint32_t psyche_vote_rails;
static uint32_t do_powerseq;
static uint32_t do_wirepow;
static uint32_t do_probe0xa7;
static uint32_t do_0xa7clk;
static uint32_t do_0xa7dump;
static uint32_t do_fifowatch;
static uint32_t do_clklevels;
static uint32_t do_speed;
static uint32_t powerseq_avdd_mv = 3300;   /* touch_powerseq_avdd=N, DTS init is 3000 */
static uint32_t powerseq_vddio = 1;        /* touch_powerseq_vddio=0 leaves gpio69 alone */
static uint32_t powerseq_reps = 1;         /* touch_powerseq_reps=N runs the sequence N times */
static uint32_t touch_gpio_diag;
static unsigned bitbang_in = PIN_MISO, bitbang_out = PIN_MOSI;

static bool (*spi_transfer)(const uint8_t *tx, uint8_t *rx, uint32_t length);
static bool (*engine_transfer2)(const uint8_t *tx, uint32_t txlen, uint8_t *rx, uint32_t rxlen);

/* ========== 基础 GPIO ============================================================== */

static void
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
	wr(GPIO_CFG(pin), pull | function << 2 | (ma / 2 - 1) << 6 | (output ? 1u << 9 : 0));
}

static void
gpio_set(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), high ? 2 : 0);
}

static bool
gpio_get(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
gpio_output(unsigned pin, bool high, unsigned pull, unsigned ma)
{
	gpio_set(pin, high);            /* 先定电平，再切成输出，免得驱动错电平 */
	gpio_configure(pin, 0, pull, ma, true);
}

static void
gpio_dump(const char *tag, unsigned pin)
{
	printf("psyche_touch: gpio %s %u cfg=%#x io=%#x level=%u\n",
	    tag, pin, rd(GPIO_CFG(pin)), rd(GPIO_IO(pin)), gpio_get(pin));
}

static void
bitbang_gpio_diagnostic(void)
{
	gpio_dump("data-in", bitbang_in);
	gpio_dump("data-out", bitbang_out);
	gpio_dump("clock", PIN_CLK);
	gpio_dump("select", PIN_CS);
	gpio_dump("reset", PIN_RESET);
	gpio_dump("interrupt", PIN_INT);

	gpio_set(PIN_CS, false);
	gpio_set(PIN_CLK, true);
	gpio_set(bitbang_out, true);
	printf("psyche_touch: gpio driven-high cs=%#x clk=%#x out=%#x in=%#x irq=%#x\n",
	    rd(GPIO_IO(PIN_CS)), rd(GPIO_IO(PIN_CLK)), rd(GPIO_IO(bitbang_out)),
	    rd(GPIO_IO(bitbang_in)), rd(GPIO_IO(PIN_INT)));
	gpio_set(bitbang_out, false);
	gpio_set(PIN_CLK, false);
	gpio_set(PIN_CS, true);
	printf("psyche_touch: gpio idle cs=%#x clk=%#x out=%#x in=%#x irq=%#x\n",
	    rd(GPIO_IO(PIN_CS)), rd(GPIO_IO(PIN_CLK)), rd(GPIO_IO(bitbang_out)),
	    rd(GPIO_IO(bitbang_in)), rd(GPIO_IO(PIN_INT)));
}

/* ========== GENI SPI（移植自 redwood，只改基址）======================================= */

static const char *
geni_spi_init(uint32_t hz)
{
	uint32_t divider;

	if ((rd(GENI_FW_REVISION_RO) >> 8 & 0xff) != 1) {
		return "the serial engine has no SPI firmware";
	}
	if (rd(GENI_IF_DISABLE_RO) & 1) {
		return "the serial engine's FIFO interface is disabled";
	}
	fifo_depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;
	if (fifo_depth < 4) {
		return "the serial engine reports no FIFO";
	}
	wr(SE_GSI_EVENT_EN, 0);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
	wr(SE_DMA_GENERAL_CFG, rd(SE_DMA_GENERAL_CFG) | 0xf);
	wr(GENI_OUTPUT_CTRL, 0x7f);
	wr(GENI_FORCE_DEFAULT, 1);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1u);
	wr(GENI_RX_WATERMARK, fifo_depth - 3);
	wr(GENI_RX_RFR_WATERMARK, fifo_depth - 2);
	wr(GENI_TX_WATERMARK, 0);
	wr(SE_IRQ_EN, rd(SE_IRQ_EN) | 0xf);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | 0x7fc0007f);

	/* mode 0, chip select 0, 8 bits, MSB first, four bytes in a FIFO word */
	wr(SPI_LOOPBACK, 0);
	wr(SPI_DEMUX_SEL, 0);
	wr(SPI_CPHA, 0);
	wr(SPI_CPOL, 0);
	wr(SPI_DEMUX_OUTPUT_INV, 0);
	/* spi-geni-qcom.c:717 clears CS_TOGGLE (BIT1), not BIT0, for a host-mode transfer. */
	wr(SPI_TRANS_CFG, rd(SPI_TRANS_CFG) & ~2u);
	wr(SPI_DELAY_COUNTERS, 0);
	wr(GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_TX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_RX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_BYTE_GRAN, 0);
	/* Qualcomm SPI encodes bits_per_word - MIN_WORD_LEN (4). */
	wr(SPI_WORD_LEN, 8 - 4);

	divider = (se_source_hz + hz - 1) / hz;
	wr(GENI_CLK_SEL, se_clock_select & 7);
	wr(GENI_SER_M_CLK_CFG, (divider ? divider : 1) << 4 | 1);
	return NULL;
}

/* Count only valid bytes in the final FIFO word, including short transfers. */
static void
geni_drain(uint8_t *rx, uint32_t length, uint32_t *received)
{
	for (int guard = 0; guard < 64; guard++) {
		uint32_t status = rd(GENI_RX_FIFO_STATUS);
		uint32_t words = status & 0x01ffffff;
		uint32_t valid = 4, word;

		if (!words) return;
		if (words == 1 && (status & (1u << 31))) {
			uint32_t tail = (status >> 28) & 7;
			if (tail && tail < 4) valid = tail;
		}
		word = rd(GENI_RX_FIFO);
		for (uint32_t i = 0; i < valid; i++) {
			if (rx && *received < length)
				rx[*received] = (uint8_t)(word >> (8 * i));
			(*received)++;
		}
	}
}

static void
geni_recover(void)
{
	/* Command-control bits differ from the completion IRQ bits. */
	wr(GENI_M_CMD_CTRL, M_CTRL_CANCEL);
	for (int i = 0; i < 100 && (rd(GENI_STATUS) & 1); i++) sleep_us(10);
	if (rd(GENI_STATUS) & 1) {
		wr(GENI_M_CMD_CTRL, M_CTRL_ABORT);
		for (int i = 0; i < 100 && (rd(GENI_STATUS) & 1); i++) sleep_us(10);
	}
	for (int i = 0; i < 64 && (rd(GENI_RX_FIFO_STATUS) & 0x01ffffff); i++)
		(void)rd(GENI_RX_FIFO);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
}

static bool
geni_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	static unsigned complaints;
	uint32_t sent = 0, received = 0, status = 0;
	uint64_t deadline;

	for (int i = 0; rd(GENI_STATUS) & 1; i++) {
		if (i > 2000) {
			geni_recover();
			break;
		}
		sleep_us(100);
	}
	if (!length) return true;
	if ((rd(GENI_STATUS) & 1) || (rd(GENI_TX_FIFO_STATUS) & 0x0fffffff)) return false;
	{ uint32_t stale = 0; geni_drain(NULL, 0, &stale); }
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_BYTE_GRAN, 0); /* four 8-bit protocol words per FIFO entry */
	wr(SPI_TX_TRANS_LEN, length);
	wr(SPI_RX_TRANS_LEN, length);
	wr(GENI_M_CMD0, SPI_FULL_DUPLEX);
	clock_interval_to_deadline(200, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		while (sent < length &&
		       (rd(GENI_TX_FIFO_STATUS) & 0x0fffffff) + 1 < fifo_depth) {
			uint32_t word = 0;
			for (int j = 0; j < 4 && sent < length; j++, sent++) {
				word |= (uint32_t)(tx ? tx[sent] : 0) << (8 * j);
			}
			wr(GENI_TX_FIFO, word);
		}
		status = rd(GENI_M_IRQ_STATUS);
		geni_drain(rx, length, &received);
		if (status & M_ERRORS) {
			break;
		}
		if (status & M_CMD_DONE) {
			geni_drain(rx, length, &received);
			wr(GENI_M_IRQ_CLEAR, status);
			if (sent == length && received == length) {
				return true;
			}
			break;
		}
	}
	if (complaints++ < 8) {
		printf("psyche_touch: SPI transfer of %u bytes failed: status %#x, sent %u, received %u\n",
		    length, status, sent, received);
	}
	geni_recover();
	return false;
}

static bool
geni_transfer_phase(const uint8_t *tx, uint8_t *rx, uint32_t length,
                    uint32_t command, bool keep_cs)
{
	static unsigned complaints;
	uint32_t sent = 0, received = 0, status = 0;
	uint32_t want_tx = tx ? length : 0;
	uint32_t want_rx = rx ? length : 0;
	uint64_t deadline;

	if (!length || (!want_tx && !want_rx)) return false;
	if (rd(GENI_STATUS) & 1) return false;
	if (want_tx && (rd(GENI_TX_FIFO_STATUS) & 0x0fffffff)) return false;
	{ uint32_t stale = 0; geni_drain(NULL, 0, &stale); }
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_BYTE_GRAN, 0);
	if (want_tx) wr(SPI_TX_TRANS_LEN, length);
	if (want_rx) wr(SPI_RX_TRANS_LEN, length);
	wr(GENI_M_CMD0, command | (keep_cs ? SPI_FRAGMENTATION : 0));

	clock_interval_to_deadline(200, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		while (sent < want_tx &&
		       (rd(GENI_TX_FIFO_STATUS) & 0x0fffffff) + 1 < fifo_depth) {
			uint32_t word = 0;
			for (int j = 0; j < 4 && sent < want_tx; j++, sent++) {
				word |= (uint32_t)tx[sent] << (8 * j);
			}
			wr(GENI_TX_FIFO, word);
		}
		status = rd(GENI_M_IRQ_STATUS);
		if (want_rx) geni_drain(rx, length, &received);
		if (status & M_ERRORS) break;
		if (status & M_CMD_DONE) {
			if (want_rx) geni_drain(rx, length, &received);
			wr(GENI_M_IRQ_CLEAR, status);
			if (sent == want_tx && received == want_rx) return true;
			break;
		}
	}
	if (complaints++ < 8) {
		printf("psyche_touch: SPI phase cmd %#x len %u failed: status %#x, sent %u, received %u\n",
		    command, length, status, sent, received);
	}
	geni_recover();
	return false;
}

/* Match the Android spi_message: TX_ONLY with fragmentation keeps CS low,
 * then RX_ONLY supplies the clocks for the response and releases CS. */
static bool
geni_transfer2(const uint8_t *tx, uint32_t txlen, uint8_t *rx, uint32_t rxlen)
{
	if (txlen > FTS_TX_SIZE || rxlen > FTS_RX_SIZE) return false;
	if (!geni_transfer_phase(tx, NULL, txlen, SPI_TX_ONLY, true)) return false;
	return geni_transfer_phase(NULL, rx, rxlen, SPI_RX_ONLY, false);
}

static bool
geni_transfer_single(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	if (tx && rx) return geni_transfer(tx, rx, length);
	if (tx) return geni_transfer_phase(tx, NULL, length, SPI_TX_ONLY, false);
	if (rx) return geni_transfer_phase(NULL, rx, length, SPI_RX_ONLY, false);
	return false;
}

/* ========== GPIO 位翻转 SPI（移植自 redwood）========================================= */


static void
bitbang_init(void)
{
	gpio_output(PIN_CS, true, PULL_NONE, 6);
	gpio_output(PIN_CLK, false, PULL_NONE, 6);
	gpio_output(bitbang_out, false, PULL_NONE, 6);
	gpio_configure(bitbang_in, 0, PULL_NONE, 2, false);
}

/*
 * One chip-select period with two phases: the command out, then the answer in. ST FTS wants the
 * select held low across both -- a transfer that lets it rise between the command and the read
 * aborts the transaction, and every read comes back as zeroes.
 */
static unsigned bitbang_delay_ns = 200;

static void
bitbang_delay(void)
{
	if (bitbang_delay_ns != 0) {
		delay_for_interval((uint32_t)(bitbang_delay_ns / 1000 + 1), NSEC_PER_USEC);
	}
}

static bool
bitbang_transfer2(const uint8_t *tx, uint32_t txlen, uint8_t *rx, uint32_t rxlen)
{
	gpio_set(PIN_CS, false);
	for (uint32_t i = 0; i < txlen; i++) {
		uint8_t out = tx ? tx[i] : 0;

		for (int bit = 7; bit >= 0; bit--) {
			gpio_set(bitbang_out, out >> bit & 1);
			bitbang_delay();
			gpio_set(PIN_CLK, true);
			bitbang_delay();
			gpio_set(PIN_CLK, false);
		}
	}
	for (uint32_t i = 0; i < rxlen; i++) {
		uint8_t in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			gpio_set(PIN_CLK, true);
			bitbang_delay();
			in = (uint8_t)(in << 1 | gpio_get(bitbang_in));
			gpio_set(PIN_CLK, false);
			bitbang_delay();
		}
		if (rx) {
			rx[i] = in;
		}
	}
	gpio_set(PIN_CS, true);
	return true;
}

static bool
bitbang_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	gpio_set(PIN_CS, false);
	for (uint32_t i = 0; i < length; i++) {
		uint8_t out = tx ? tx[i] : 0, in = 0;
		for (int bit = 7; bit >= 0; bit--) {
			gpio_set(bitbang_out, out >> bit & 1);
			gpio_set(PIN_CLK, true);
			in = (uint8_t)(in << 1 | gpio_get(bitbang_in));
			gpio_set(PIN_CLK, false);
		}
		if (rx) {
			rx[i] = in;
		}
	}
	gpio_set(PIN_CS, true);
	return true;
}

/*
 * The phone's live device tree puts all four active SE4 pins on qup4 at
 * 6 mA with bias disabled. The CS pull-up belongs to the sleep state only.
 */
static void
use_engine_pins(void)
{
	gpio_configure(PIN_MISO, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	gpio_configure(PIN_MOSI, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	gpio_configure(PIN_CLK, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	gpio_configure(PIN_CS, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
}

/* ========== 时钟 ==================================================================== */

static uint32_t
rcg_rate(uint32_t cfg, uint32_t m, uint32_t n)
{
	uint32_t divider = cfg & 0x1f, mode = cfg >> 12 & 3;
	uint32_t hz = XO_HZ;

	if (divider) {
		hz = hz * 2 / (divider + 1);
	}
	if (mode == 1 && n) {
		hz = (uint32_t)((uint64_t)hz * m / n);
	}
	return hz;
}

static bool
clocks_on(void)
{
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_WRAP0_CORE_2X | VOTE_WRAP0_CORE |
	    VOTE_WRAP0_M_AHB | VOTE_WRAP0_S_AHB);

	if (rd(GCC_WRAP0_S4_DFSR) & 1) {
			/*
			 * Choose by what the bus does, not by the DFS table.
			 *
			 * The table and rcg_rate() describe level 5 as 6.4 MHz,, too slow, yet every read at
			 * that level came back with no signature, while level 1 carried the same 208-byte
			 * read correctly at 243 us and level 0 at 325 us. So the table is not describing
			 * this clock; the reads are, and they say the 19.2 MHz point is the working one.
			 *
			 * Levels 1..4 share that point and differ only in their M/N; level 1 measured
			 * fastest, so it leads, and the others stay as fallbacks.
			 */
			static const uint32_t order[] = { 1, 0, 2, 3, 4 };
			unsigned i;

			for (i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
				uint32_t hz = rcg_rate(rd(GCC_WRAP0_S4_PERF_CFG(order[i])),
				    rd(GCC_WRAP0_S4_PERF_M(order[i])),
				    rd(GCC_WRAP0_S4_PERF_N(order[i])));
				if (hz >= 4000000) {
					se_source_hz = hz;
					se_clock_select = order[i];
					break;
				}
			}
			if (se_source_hz == 0) {
			printf("psyche_touch: no usable preset clock rate\n");
			return false;
		}
	} else {
		wr(GCC_WRAP0_S4_CFG, (rd(GCC_WRAP0_S4_CFG) & ~0x371fu) | 1);
		wr(GCC_WRAP0_S4_CMD, rd(GCC_WRAP0_S4_CMD) | 1);
		for (int i = 0; i < 500 && (rd(GCC_WRAP0_S4_CMD) & 1); i++) {
			sleep_us(10);
		}
		if (rd(GCC_WRAP0_S4_CMD) & 1) {
			printf("psyche_touch: SE4 clock does not take its configuration (%#x)\n",
			    rd(GCC_WRAP0_S4_CMD));
			return false;
		}
		se_source_hz = XO_HZ;
		se_clock_select = 0;
	}
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_WRAP0_S4);

	/*
	 * These branches are voted (BRANCH_HALT_VOTED in gcc-kona.c): a branch is halted only when its
	 * CBCR reports halted *and* its bit in the vote register is clear. Testing the CBCR bit alone
	 * made this give up on clocks that were already running.
	 */
	for (int i = 0; i < 1000; i++) {
		uint32_t vote = rd(GCC_VOTE);
		bool off;

		off = (rd(GCC_WRAP0_M_AHB_CBCR) & CLOCK_OFF) && !(vote & VOTE_WRAP0_M_AHB);
		off = off || ((rd(GCC_WRAP0_S_AHB_CBCR) & CLOCK_OFF) && !(vote & VOTE_WRAP0_S_AHB));
		off = off || ((rd(GCC_WRAP0_S4_CBCR) & CLOCK_OFF) && !(vote & VOTE_WRAP0_S4));
		off = off || ((rd(GCC_WRAP0_CORE_CBCR) & CLOCK_OFF) && !(vote & VOTE_WRAP0_CORE));
		off = off || ((rd(GCC_WRAP0_CORE_2X_CBCR) & CLOCK_OFF) && !(vote & VOTE_WRAP0_CORE_2X));
		if (!off) {
			return true;
		}
		sleep_us(10);
	}
	printf("psyche_touch: clocks stay off: m_ahb=%#x s_ahb=%#x s4=%#x vote=%#x\n",
	    rd(GCC_WRAP0_M_AHB_CBCR), rd(GCC_WRAP0_S_AHB_CBCR), rd(GCC_WRAP0_S4_CBCR),
	    rd(GCC_VOTE));
	return false;
}

/* ========== ST FTS 命令层 ============================================================ */

/*
 * 写 [cmd][addr...][data...]。addr_size = 0 表示无地址段（FIFO 命令如此）。
 */
static bool
fts_write(uint8_t cmd, uint64_t address, uint8_t addr_size,
          const uint8_t *data, uint32_t length)
{
	uint8_t tx[FTS_TX_SIZE];
	uint32_t n = 0;

	if (1 + addr_size + length > sizeof(tx)) {
		return false;
	}
	tx[n++] = cmd;
	for (uint8_t i = 0; i < addr_size; i++) {
		tx[n++] = (uint8_t)(address >> ((addr_size - 1 - i) * 8));
	}
	if (length && data) {
		memcpy(tx + n, data, length);
		n += length;
	}
	return spi_transfer(tx, NULL, n);
}

/*
 * 写 [cmd][addr...]，再读 (length + 1)，丢掉第 1 个。
 * 等价于 Android 的 fts_writeReadU8UX(cmd, addrSize, addr, out, len, hasDummyByte=1)。
 * 注意：GENI 是全双工单段，所以分两次调用（写段、读段）。
 */
static bool
fts_read(uint8_t cmd, uint64_t address, uint8_t addr_size,
         uint8_t *out, uint32_t length)
{
	uint8_t tx[9], rx[FTS_RX_SIZE];
	uint32_t n = 0, want = length + FTS_DUMMY_FIFO;

	if (1 + addr_size > sizeof(tx) || want > sizeof(rx)) {
		return false;
	}
	tx[n++] = cmd;
	for (uint8_t i = 0; i < addr_size; i++) {
		tx[n++] = (uint8_t)(address >> ((addr_size - 1 - i) * 8));
	}
	/*
	 * On the pins, the select has to stay low from the command to the answer; through the GENI
	 * engine the two phases are two FIFO commands and the engine keeps the select between them.
	 */
	/* one select period for the command and the answer */
	if (engine_transfer2 != NULL) {
		if (!engine_transfer2(tx, n, rx, want)) {
			return false;
		}
	} else if (!bitbang_transfer2(tx, n, rx, want)) {
		return false;
	}
	if (trace_rx) {
		printf("psyche_touch: wire cmd");
		for (uint32_t i = 0; i < n; i++) {
			printf(" %02x", tx[i]);
		}
		printf(" ->");
		for (uint32_t i = 0; i < want; i++) {
			printf(" %02x", rx[i]);
		}
		printf("\n");
	}
	memcpy(out, rx + FTS_DUMMY_FIFO, length);
	return true;
}

/* ========== RPMh 供电 =============================================================== */


/* ---- the command database the boot loader left (same layout as goodix.c uses) ------------------- */

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

/* The RPMh address of a resource by its name; 0 = none. */
static uint32_t
psyche_rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmddb;
	char id[8] = { 0 };

	if (db == NULL) {
		return 0;
	}
	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		printf("psyche_touch: no command database (magic %02x %02x %02x %02x)\n",
		    db->magic[0], db->magic[1], db->magic[2], db->magic[3]);
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
 * Fire one command in the sequencer and wait for the hardware's own acknowledgement.
 *
 * The order matters and the old version of this function got it wrong: it wrote
 * TCS_CONTROL = TRIGGER alone, which clears the AMC enable bit that was already there, so the
 * command sat in its slot unscanned. rsc_drv never does that -- __tcs_trigger() clears TRIGGER,
 * clears AMC_MODE_ENABLE, sets AMC_MODE_ENABLE, and only then sets both bits for the final
 * write (drivers/soc/qcom/rpmh-rsc.c:230). A slot that was never scanned keeps CMD_STATUS =
 * ISSUED|stuck and never gains COMPLETED, which is exactly what the saved logs show
 * (CMD_STATUS = 0x101 for ten milliseconds).
 *
 * The completion test is also the hardware's, not "the register happened to read zero": the
 * oscilloscope-worthy proof that a command ran is CMD_STATUS_COMPL.
 */
static bool
rpmh_send(uint32_t msgid, uint32_t addr, uint32_t data, uint32_t *response)
{
	int i;
	uint32_t st;
	bool completed;

	/* fill the slot while it is disabled, then enable it -- rsc_drv __tcs_buffer_write() */
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_CMD_WAIT_FOR_CMPL), 0);
	wr(TCS(TCS_CMD_MSGID(0)), msgid);
	wr(TCS(TCS_CMD_ADDR(0)), addr);
	wr(TCS(TCS_CMD_DATA(0)), data);
	wr(TCS(TCS_CMD_ENABLE), 1u);
	wr(TCS(TCS_CMD_WAIT_FOR_CMPL), 1u);

	/* clear TRIGGER, clear ENABLE, set ENABLE, then ENABLE|TRIGGER */
	wr(TCS(TCS_CONTROL), 0);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	completed = false;
	for (i = 0; i < 2000; i++) {
		st = rd(TCS(TCS_CMD_STATUS));
		if (st & TCS_CMD_COMPLETED) {
			completed = true;
			break;
		}
		sleep_us(10);
	}
	if (response != NULL && completed) {
		*response = rd(TCS(RSC_CMD_RESP_DATA));
	}

	wr(TCS(TCS_CONTROL), 0);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_CMD_WAIT_FOR_CMPL), 0);
	wr(TCS(TCS_IRQ_CLEAR), rd(TCS(TCS_IRQ_STATUS)));
	if (!completed) {
		/* Say what the slot holds, so the next run explains itself. */
		printf("psyche_touch: rpmh slot did not complete: msgid=%#x addr=%#x data=%#x"
		    " status=%#x ctrl=%#x\n",
		    msgid, addr, data, rd(TCS(TCS_CMD_STATUS)), rd(TCS(TCS_CONTROL)));
	}
	return completed;
}

static bool
rpmh_write(uint32_t msgid, uint32_t addr, uint32_t data)
{
	return rpmh_send(msgid, addr, data, NULL);
}

/* The same slot, read instead of written: MSGID loses the WRITE bit and gains nothing else. */
static bool
rpmh_read(uint32_t addr, uint32_t *value)
{
	return rpmh_send(MSGID_READ_4_BYTES, addr, 0, value);
}

/*
 * 供电：psyche 的 avdd = L10C、vdd = L7A（DTS 明写）。
 * 这两条轨在 cmd-db 里的地址要实测（redwood 用的是别的轨名）。
 * 首版先用 boot-arg 给：psyche_vdd_addr=0x.. psyche_avdd_addr=0x..
 * 不写就跳过（bootloader 已经上电的情况很常见）。
 */
/*
 * Stop at checkpoint n when touch_step asked for it.  A panic goes back to the loader,
 * which writes the log out; a step that never produces one is the step that took the
 * machine with it.
 */
static void
step_probe(uint32_t n)
{
	if (touch_step == n) {
		panic("psyche_touch: reached step %u", n);
	}
}

static bool
power_on(void)
{
	uint32_t v = 0;

	/*
	 * The four lines touch_fifowatch uses, with the command-database lookup taken out.
	 *
	 * psyche_rpmh_address() walks the command database the boot loader left in RAM, and
	 * every run that went through it (the old power_on) stopped dead shortly after printing
	 * the two rail addresses. The entry point that answers every time does not walk it: it
	 * uses the addresses the dump already showed -- ldoa7 = 0x41f00 (vdd, 1.8 V) and
	 * ldoc10 = 0x41200 (avdd, 3.0 V) -- and reads before writing, because an unconditional
	 * vote is what hangs this phone.
	 */
	step_probe(1);
	/*
	 * Not mapped, the sequencer's registers are not there to be written: TCS(reg) would be
	 * 0xd00 + reg, a physical address in the vectors, and the store goes wherever that lands.
	 * Every other entry point tests this; the normal probe did not, and a run whose mapping
	 * failed went straight on to write it.
	 */
	if (rsc == 0) {
		printf("psyche_touch: no RPMh window mapped, supplies left as they are\n");
		return true;
	}
	(void)rpmh_read(0x41f00 + VRM_ENABLE, &v);
	step_probe(2);
	if (!v) {
		(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41f00 + VRM_VOLTAGE, 1800);
		(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41f00 + VRM_ENABLE, 1);
		printf("psyche_touch: vdd voted on\n");
	} else {
		printf("psyche_touch: vdd already on\n");
	}

	/*
	 * The same order touch_fifowatch uses, which is the only entry point that has ever
	 * got an answer on this board: read the second rail straight after the first, and
	 * put the settle delay inside the branch that actually writes.  With the delay
	 * before the read instead, a run of the normal probe stops the whole machine inside
	 * this function -- the log ends at "core 700: the firmware answers 0" and not even
	 * the loader's two-second note appears.
	 */
	step_probe(3);
	v = 0;
	(void)rpmh_read(0x41200 + VRM_ENABLE, &v);
	step_probe(4);
	if (!v) {
		sleep_ms(15);
		(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41200 + VRM_VOLTAGE, 3000);
		(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41200 + VRM_ENABLE, 1);
		printf("psyche_touch: avdd voted on\n");
	} else {
		printf("psyche_touch: avdd already on\n");
	}

	step_probe(5);
	gpio_set(PIN_VDDIO, true);
	step_probe(6);
	gpio_configure(PIN_VDDIO, 0, PULL_NONE, 8, true);
	step_probe(7);
	sleep_ms(15);
	step_probe(8);
	printf("psyche_touch: supplies ready\n");
	return true;
}

/* ========== ST FTS 操作 ============================================================= */

/* 收集 FIFO 里的所有事件 */
static bool
fts_read_fifo(uint8_t *data, uint32_t *count)
{
	uint8_t first[FTS_FIFO_EVENT_SIZE];
	uint8_t remaining;

	if (!fts_read(FTS_FIFO_CMD_READALL, 0, 0, first, FTS_FIFO_EVENT_SIZE)) {
		return false;
	}
	remaining = first[FTS_EVENTS_REMAINING_POS] & FTS_EVENTS_REMAINING_MASK;
	if (remaining > FTS_FIFO_DEPTH - 1) {
		remaining = FTS_FIFO_DEPTH - 1;
	}
	memcpy(data, first, FTS_FIFO_EVENT_SIZE);

	if (remaining > 0) {
		if (!fts_read(FTS_FIFO_CMD_READALL, 0, 0,
		              data + FTS_FIFO_EVENT_SIZE,
		              FTS_FIFO_EVENT_SIZE * remaining)) {
			return false;
		}
	}
	*count = remaining + 1;
	return true;
}

/*
 * Read the chip's system-information block and keep the identity it reports.
 *
 * The phone reads this with 0xA7 / two-byte address / 208 bytes / one dummy byte
 * (ftsCore.c:521-523) and validates two fields: the header signature 0xA5 and the data id
 * 0x01. Everything after that is data it logs rather than checks. This kernel was demanding
 * an exact chip id from a register read the phone never performs on a probe, so a working
 * chip looked broken.
 *
 * The chip this board carries reports chip0Id = 0x3640, not the 0x3936 in the vendor
 * header -- and the phone would not mind: its DCHIP_ID_* constants are used only by the
 * diagnostics reader (fts_proc.c:1665), never on the probe path. So record what is there,
 * refuse only a missing signature, and let the FIFO say whether the controller is alive.
 */
static bool
fts_system_info(void)
{
	#define SYS_INFO_SIZE 208u
	uint8_t info[SYS_INFO_SIZE];
	unsigned attempt;

	for (attempt = 0; attempt < 4; attempt++) {
		memset(info, 0, sizeof(info));
		if (!fts_read(0xA7, 0x0000, 2, info, SYS_INFO_SIZE)) {
			printf("psyche_touch: system info read did not complete\n");
			sleep_ms(20);
			continue;
		}
		if (info[0] != 0xA5) {
			printf("psyche_touch: system info header %02x, not a signature\n", info[0]);
			sleep_ms(20);
			continue;
		}
		printf("psyche_touch: system info header ok, data id %02x\n", info[1]);
		printf("psyche_touch: chip id %04x, fw ver %04x, config ver %04x\n",
		    (unsigned)(info[10] | ((uint16_t)info[11] << 8)),
		    (unsigned)(info[16] | ((uint16_t)info[17] << 8)),
		    (unsigned)(info[20] | ((uint16_t)info[21] << 8)));
		/*
		 * ftsCore.c:545-600 walks the block as: header 4 bytes, apiVer_rev(2),
		 * apiVer_minor(1), apiVer_major(1), chip0Ver(2), chip0Id(2), chip1Ver(2),
		 * chip1Id(2), fwVer(2), svnRev(2), cfgVer(2), cfgProjId(2), cxVer(2),
		 * cxProjId(2) -- and only then the three AFE bytes and the protocol.
		 * The cxProjId step is easy to drop, and dropping it shifts the AFE bytes two
		 * slots left, so a perfectly calibrated panel reads as 00 00 07.
		 */
		printf("psyche_touch: afe ver cfg %02x cx %02x panel %02x, protocol %02x\n",
		    info[28], info[29], info[30], info[31]);
		printf("psyche_touch: identity recorded (the phone does not compare this field)\n");
		return true;
	}
	return false;
}

/*
 * Start the chip scanning.
 *
 * The phone's driver sends this as a plain three-byte write -- [0xA0][SCAN_MODE_ACTIVE][settings]
 * (ftsCore.c:333-350), byte for byte the same command the Mi 10S port issues as
 * {0xa0, 0x00, 0x01}. Nothing in this kernel has ever sent it, so after a reset the chip has
 * been waiting idle: the interrupt line never falls, and the FIFO only holds the empty event.
 */
static bool
fts_scan_mode(uint8_t settings)
{
	return fts_write(FTS_CMD_SCAN_MODE, 0, 0,
	    (uint8_t[]){ (uint8_t)SCAN_MODE_ACTIVE, settings }, 2);
}

static bool
fts_sense_on(void)
{
	/*
	 * fts_init_sensing (fts.c:5306-5316) stops sensing, waits, then starts it:
	 * setScanMode(ACTIVE, 0x00); mdelay(WAIT_AFTER_SENSEOFF); setScanMode(ACTIVE, 0x01).
	 * Sending only the enable leaves the chip having produced its startup calibration and
	 * then nothing -- INT fell once in 600 reads.
	 */
	if (!fts_scan_mode(0x00)) {
		return false;
	}
	sleep_ms(50);
	if (!fts_scan_mode(0x01)) {
		return false;
	}
	/*
	 * Wait without reading. Draining the FIFO here looks harmless and is not: the scans the
	 * engine is starting to produce get consumed by the drain, and the chip then sits
	 * silent (measured: 0 events in 1000 reads, INT never low). The watch loop does the
	 * reading, and it has to start from a FIFO nobody has touched since the enable.
	 */
	return true;
}

static bool
fts_reset(void)
{
	uint8_t ev[FTS_FIFO_EVENT_SIZE];
	uint32_t n;

	/* psyche 有 RESET 引脚（33），不需要写 FTS_ADDR_SYSTEM_RESET */
	gpio_set(PIN_RESET, false);
	if (touch_gpio_diag) printf("psyche_touch: reset low io=%#x irq=%#x\n",
	    rd(GPIO_IO(PIN_RESET)), rd(GPIO_IO(PIN_INT)));
	sleep_ms(10);
	gpio_set(PIN_RESET, true);
	if (touch_gpio_diag) printf("psyche_touch: reset high io=%#x irq=%#x\n",
	    rd(GPIO_IO(PIN_RESET)), rd(GPIO_IO(PIN_INT)));
	sleep_ms(5);
	if (touch_gpio_diag) printf("psyche_touch: reset +5ms io=%#x irq=%#x\n",
	    rd(GPIO_IO(PIN_RESET)), rd(GPIO_IO(PIN_INT)));

	/*
	 * The event is 0x03 exactly: an empty FIFO reads 0x00, and comparing only the high nibble
	 * makes 0x00 look like 0x03 and calls a chip that never answered "ready".
	 */
	for (int attempt = 0; attempt < 3; attempt++) {
		if (fts_read_fifo(ev, &n) && ev[0] == FTS_EVT_CONTROLLER_READY) {
			printf("psyche_touch: ready event %02x %02x %02x %02x %02x %02x %02x %02x\n",
			    ev[0], ev[1], ev[2], ev[3], ev[4], ev[5], ev[6], ev[7]);
			return true;
		}
		sleep_ms(100);
	}
	/*
	 * Then a register reset, for a chip whose reset pin is not wired. The same rule applies:
	 * the event is 0x03 exactly, not "whatever has the same high nibble".
	 */
	gpio_set(PIN_RESET, true);
	if (fts_write(FTS_CMD_HW_REG_W, FTS_ADDR_SYSTEM_RESET, FTS_ADDR_SIZE_HW_REG,
	    (uint8_t[]){ FTS_SYSTEM_RESET_VALUE }, 1)) {
		for (int attempt = 0; attempt < 3; attempt++) {
			if (fts_read_fifo(ev, &n) && ev[0] == FTS_EVT_CONTROLLER_READY) {
				return true;
			}
			sleep_ms(100);
		}
	}
	return false;
}

static uint16_t
fts_chip_id(void)
{
	uint8_t d[2] = { 0, 0 };

	if (!fts_read(FTS_CMD_HW_REG_R, FTS_ADDR_DCHIP_ID, FTS_ADDR_SIZE_HW_REG, d, 2)) {
		printf("psyche_touch: the chip id read did not complete\n");
		return 0;
	}
	printf("psyche_touch: chip id raw %02x %02x\n", d[0], d[1]);
	return (uint16_t)(d[1] << 8 | d[0]);
}

/* ========== 报告解析 ================================================================ */

/*
 * One FTS event to one input sample.
 *
 * The reported position is a pixel pair on a 1080x2400 panel, and hakios_touch_post wants
 * 0..65535 across the display, so it has to be scaled -- posting pixels directly would put
 * every touch in the bottom-left fifteenth of the screen.
 *
 * The event id is compared whole: 0x13/0x23/0x33 differ in the low nibble, and shifting them
 * right by four makes ENTER and LEAVE indistinguishable.
 */
static void
fts_report_event(const uint8_t *e)
{
	static uint32_t dumped;

	/*
	 * touch_rawdump=N: print the raw frame of the first N presses, so the corners
	 * can be calibrated in one run.  A press (ENTER_POINT) is what carries a new
	 * position; the MOTION_POINT that follows repeats it.
	 */
	if (touch_rawdump != 0 && e[0] == 0x13 && dumped < touch_rawdump) {
		uint32_t x12 = (((uint32_t)e[3] & 0x0F) << 8) | e[2];
		uint32_t y12 = ((uint32_t)e[4] << 4) | ((e[3] & 0xF0) >> 4);
		uint32_t x16 = (uint32_t)e[2] | ((uint32_t)e[3] << 8);
		uint32_t y16 = (uint32_t)e[4] | ((uint32_t)e[5] << 8);

		dumped++;
		printf("psyche_touch: press %u: %02x %02x %02x %02x %02x %02x %02x %02x"
		    "  x16=%u y16=%u (x12=%u y12=%u)\n",
		    dumped, e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7],
		    x16, y16, x12, y12);
	}
	uint32_t id    = (e[1] & 0xF0) >> 4;      /* high nibble: which finger */
	uint32_t kind  = e[1] & 0x0F;             /* low nibble: 1 = finger, 2 = glove ... */
	/*
	 * Two little-endian 16-bit fields.  The 12-bit layout the ST SDK documents
	 * (x from e[2] and the low nibble of e[3], y from e[4] and the HIGH nibble of
	 * e[3]) mixes the two axes: on a continuous drag it produced a y that jumped
	 * from 4081 to 465 while the finger moved smoothly, and the touches landed in
	 * the wrong place.  This chip reports super-resolution (fts,support-super-
	 * resolution in the psyche DT), and there e[3] is simply x's high byte:
	 * e[5] counts up smoothly as the finger moves down, e[4] wraps every 256.
	 */
	uint32_t raw_x = (uint32_t)e[2] | ((uint32_t)e[3] << 8);
	uint32_t raw_y = (uint32_t)e[4] | ((uint32_t)e[5] << 8);
	uint32_t x     = raw_x;
	uint32_t y     = raw_y;

	(void)kind;                               /* all touch types arrive as a plain finger */

	if (id >= FTS_MAX_TOUCHES) {
		return;
	}
	/* Clamp into the calibrated window, then scale it onto the panel. */
	if (x < fts_x_min) {
		x = fts_x_min;
	}
	if (x > fts_x_max) {
		x = fts_x_max;
	}
	if (y < fts_y_min) {
		y = fts_y_min;
	}
	if (y > fts_y_max) {
		y = fts_y_max;
	}
	x = (x - fts_x_min) * FTS_X_SIZE_DEFAULT / (fts_x_max - fts_x_min);
	y = (y - fts_y_min) * FTS_Y_SIZE_DEFAULT / (fts_y_max - fts_y_min);
	if (x >= FTS_X_SIZE_DEFAULT) {
		x = FTS_X_SIZE_DEFAULT - 1;
	}
	if (y >= FTS_Y_SIZE_DEFAULT) {
		y = FTS_Y_SIZE_DEFAULT - 1;
	}

	stat_last_x = x;
	stat_last_y = y;

	switch (e[0]) {
	case FTS_EVT_ENTER_POINT:
	case FTS_EVT_MOTION_POINT: {
		bool was_down = contacts[id].down;
		uint32_t nx = x * 65535u / (FTS_X_SIZE_DEFAULT - 1);
		uint32_t ny = y * 65535u / (FTS_Y_SIZE_DEFAULT - 1);

		/*
		 * The chip repeats a motion event for every scan even when the finger has not
		 * moved; a still finger produced 4142 identical events in one run.  Every one of
		 * them became a HID event in the userland side, and backboardd fell from 10 to 1
		 * frames a second while the finger was down.  FocalTech's driver, the one that
		 * works on redwood, drops the repeats the same way.
		 */
		if (was_down && contacts[id].x == x && contacts[id].y == y) {
			break;
		}
		hakios_touch_post(was_down ? HAKIOS_TOUCH_MOVE : HAKIOS_TOUCH_DOWN, id, nx, ny);
		contacts[id].down = true;
		contacts[id].x = x;
		contacts[id].y = y;
		stat_touches++;
		if (trace_latency && stat_touches <= 4) {
			printf("psyche_touch: %s[%u] (%u,%u) evt %#x\n",
			    was_down ? "move" : "down", id, x, y, e[0]);
		}
		break;
	}

	case FTS_EVT_LEAVE_POINT:
		if (contacts[id].down) {
			uint32_t nx = contacts[id].x * 65535u / (FTS_X_SIZE_DEFAULT - 1);
			uint32_t ny = contacts[id].y * 65535u / (FTS_Y_SIZE_DEFAULT - 1);

			hakios_touch_post(HAKIOS_TOUCH_UP, id, nx, ny);
			contacts[id].down = false;
			stat_touches++;
			if (trace_latency) {
				printf("psyche_touch: up[%u] (%u,%u)\n",
				    id, contacts[id].x, contacts[id].y);
			}
		}
		break;

	default:
		stat_bad++;
		break;
	}
}

static void
fts_poll_once(void)
{
	uint8_t data[FTS_FIFO_EVENT_SIZE * FTS_FIFO_DEPTH];
	uint32_t events = 0;

	if (!fts_read_fifo(data, &events)) {
		stat_bad++;
		return;
	}
	stat_reports++;
	if (data[0] != FTS_EVT_NOEVENT) {
		stat_nonempty++;
	}
	for (uint32_t i = 0; i < events; i++) {
		const uint8_t *e = &data[i * FTS_FIFO_EVENT_SIZE];
		if (e[0] == FTS_EVT_NOEVENT) {
			break;
		}
		fts_report_event(e);
	}
}

/* ========== 主线程 ================================================================== */

static void
fts_thread(__unused void *parameter, __unused wait_result_t result)
{
	thread_t self = current_thread();

	for (;;) {
		bool irq_low;

		stat_polls++;

		/*
		 * Read every poll, not only when the interrupt line is low.  The one entry point
		 * on this board that has ever read a real event (touch_fifowatch) does exactly
		 * this: it reads on every pass and merely counts the line's level.  Gating on the
		 * line meant a run with a real userland sat at reports=0 touches=0 bad=0 for
		 * 16250 polls, because the line never went low.
		 */
		irq_low = !gpio_get(PIN_INT);
		if (irq_low) {
			stat_irq_low++;
		}
		fts_poll_once();
		sleep_us(touch_poll_us);

		/*
		 * The only line a normal run may print: every ~5 s at the default 8 ms poll (625
		 * polls). Every line here costs a loader_log_putc, which is why nothing else is
		 * printed per sample.
		 */
		if ((stat_polls % 625) == 0) {
			printf("psyche_touch: poll=%u reads=%u nonempty=%u touches=%u bad=%u"
			    " irq_low=%u last=(%u,%u)\n",
			    stat_polls, stat_reports, stat_nonempty, stat_touches, stat_bad,
			    stat_irq_low, stat_last_x, stat_last_y);
		}
	}
	(void)self;
}

/* ========== 启动 ==================================================================== */

/* Diagnostic only: validate the full-duplex primitive, not the sequential
 * command/response wrapper. No chip reset or regulator writes are needed. */
static void
geni_loopback_probe(void)
{
	static const uint32_t lengths[] = {1, 2, 3, 4, 5, 7, 8, 9, 10, 63, 64, 65};
	uint8_t tx[65], rx[65];
	const char *problem;
	unsigned passed = 0;

	if (!clocks_on()) return;
	se = ml_io_map(SE_PHYS, SE_SIZE);
	if (!se) return;
	problem = geni_spi_init(target_spi_hz);
	if (problem) {
		printf("psyche_touch: loopback init: %s\n", problem);
		return;
	}
	printf("psyche_touch: loopback cfg=%#x/%#x gran=%u wordlen=%u\n",
	    rd(GENI_TX_PACKING_CFG0), rd(GENI_TX_PACKING_CFG1),
	    rd(GENI_BYTE_GRAN), rd(SPI_WORD_LEN));
	wr(SPI_LOOPBACK, 1);
	for (unsigned round = 0; round < 2; round++) {
		for (unsigned k = 0; k < sizeof(lengths) / sizeof(lengths[0]); k++) {
			uint32_t n = lengths[k];
			bool ok;
			for (uint32_t i = 0; i < n; i++) tx[i] = (uint8_t)(0xa5 ^ (i * 17) ^ (round * 0x5a));
			memset(rx, 0, sizeof(rx));
			ok = geni_transfer(tx, rx, n);
			ok = ok && memcmp(tx, rx, n) == 0;
			printf("psyche_touch: loopback round=%u len=%u %s\n", round, n, ok ? "PASS" : "FAIL");
			if (!ok) {
				printf("psyche_touch: loopback bytes:");
				for (uint32_t i = 0; i < n && i < 16; i++) printf(" %02x/%02x", tx[i], rx[i]);
				printf("\n");
				goto done;
			}
			passed++;
		}
	}
done:
	wr(SPI_LOOPBACK, 0);
	printf("psyche_touch: loopback %u/24 cases passed; diagnostic ends here\n", passed);
}

/*
 * Every wire of the bus, measured. The engine's own loopback passes, both transports return
 * zeroes and the supplies are voted on, so what is left to check is the outside: whether the
 * four pins reach the chip at all, and whether any line changes when it should. Each pad is
 * read with no pull, a pull-down and a pull-up in turn -- a line the chip holds stays put while
 * a floating one follows the pull.
 */
static void
wirecheck_probe(void)
{
	static const struct { unsigned pin; const char *name; } wires[] = {
		{ PIN_MISO,  "MISO" },
		{ PIN_MOSI,  "MOSI" },
		{ PIN_CLK,   "CLK" },
		{ PIN_CS,    "CS" },
		{ PIN_INT,   "INT" },
		{ PIN_RESET, "RESET" },
	};
	unsigned i;

	printf("psyche_touch: wirecheck ----- pads as they are -----\n");
	for (i = 0; i < sizeof(wires) / sizeof(wires[0]); i++) {
		unsigned n = wires[i].pin;
		uint32_t cfg = rd(GPIO_CFG(n)), io = rd(GPIO_IO(n));

		printf("psyche_touch: wirecheck %-5s gpio%-2u cfg=%#04x io=%#04x fn=%u pull=%u drv=%umA out=%u level=%u\n",
		    wires[i].name, n, cfg, io, (cfg >> 2) & 7, cfg & 3,
		    (((cfg >> 6) & 7) + 1) * 2, (cfg >> 9) & 1, io & 1);
	}

	/* every signal pad as a bare input, and read it three ways */
	printf("psyche_touch: wirecheck ----- is anything driving these? -----\n");
	for (i = 0; i < sizeof(wires) / sizeof(wires[0]); i++) {
		unsigned n = wires[i].pin;
		uint32_t no_pull, pull_dn, pull_up;

		wr(GPIO_IO(n), 0);
		wr(GPIO_CFG(n), 0x0u);
		sleep_us(20);
		no_pull = gpio_get(n);

		wr(GPIO_CFG(n), 0x1u);
		sleep_us(20);
		pull_dn = gpio_get(n);

		wr(GPIO_CFG(n), 0x3u);
		sleep_us(20);
		pull_up = gpio_get(n);

		printf("psyche_touch: wirecheck %-5s no-pull=%u pull-dn=%u pull-up=%u  %s\n",
		    wires[i].name, no_pull, pull_dn, pull_up,
		    (no_pull == 0 && pull_dn == 0 && pull_up == 1) ? "-> FLOATING (nothing drives it)"
		    : "-> driven by something");
	}

	/* the select, watched while the engine runs a transaction */
	printf("psyche_touch: wirecheck ----- select during a transfer -----\n");
	{
		uint8_t ev[FTS_FIFO_EVENT_SIZE];
		uint32_t seen_low = 0, seen_high = 0;

		wr(GPIO_CFG(PIN_CS), 0x3u);        /* CS as an input, pull-up: watch who drives it */
		wr(GPIO_IO(PIN_CS), 0);
		sleep_us(20);
		printf("psyche_touch: wirecheck CS idle as input = %u (1 = nothing pulling it low)\n",
		    gpio_get(PIN_CS));

		/*
		 * Put the pins back on the engine before a transfer and do not fire one while the
		 * select is still a plain input: the engine must own all four pads, and the value
		 * left behind by reading it as an input would make a stale transfer fire.
		 */
		use_engine_pins();
		if (se != 0 && engine_transfer2 != NULL) {
			(void)fts_read(FTS_FIFO_CMD_READALL, 0, 0, ev, FTS_FIFO_EVENT_SIZE);
		}
		for (i = 0; i < 20; i++) {
			if (gpio_get(PIN_CS)) {
				seen_high++;
			} else {
				seen_low++;
			}
			sleep_us(5);
		}
		printf("psyche_touch: wirecheck after a transfer: CS samples high=%u low=%u\n",
		    seen_high, seen_low);
	}

	/* and the interrupt with the reset held both ways */
	printf("psyche_touch: wirecheck ----- reset vs interrupt -----\n");
	wr(GPIO_IO(PIN_INT), 0);
	wr(GPIO_CFG(PIN_INT), 0x3u);           /* interrupt: input, pull-up */
	wr(GPIO_IO(PIN_RESET), 0);
	wr(GPIO_CFG(PIN_RESET), 0x200u);       /* reset: output */

	wr(GPIO_IO(PIN_RESET), 0);
	for (i = 0; i < 6; i++) {
		sleep_ms(20);
		printf("psyche_touch: wirecheck   reset LOW  +%ums int=%u miso=%u\n",
		    (i + 1) * 20, gpio_get(PIN_INT), gpio_get(PIN_MISO));
	}
	wr(GPIO_IO(PIN_RESET), 2);
	for (i = 0; i < 12; i++) {
		sleep_ms(20);
		printf("psyche_touch: wirecheck   reset HIGH +%ums int=%u miso=%u\n",
		    (i + 1) * 20, gpio_get(PIN_INT), gpio_get(PIN_MISO));
	}
	printf("psyche_touch: wirecheck done\n");
}

/*
 * The select line, the one thing that decides whether the chip hears anything. Sampled around a
 * transfer it stayed high throughout, so no command can ever reach the chip -- which is exactly
 * why every read comes back zero. First the pad is driven by hand to prove the line can be
 * pulled low at all, then the engine's own select control is inspected.
 */
static void
cstest_probe(void)
{
	uint8_t ev[FTS_FIFO_EVENT_SIZE];
	unsigned i;

	printf("psyche_touch: cstest ----- can this pad be driven low by hand? -----\n");

	/* the select as a plain GPIO output */
	wr(GPIO_IO(PIN_CS), 2);
	wr(GPIO_CFG(PIN_CS), 0x200u);         /* output, function gpio */
	sleep_us(20);
	printf("psyche_touch: cstest CS as output HIGH: cfg=%#04x io=%#04x level=%u\n",
	    rd(GPIO_CFG(PIN_CS)), rd(GPIO_IO(PIN_CS)), gpio_get(PIN_CS));

	wr(GPIO_IO(PIN_CS), 0);
	sleep_us(20);
	printf("psyche_touch: cstest CS as output LOW : cfg=%#04x io=%#04x level=%u\n",
	    rd(GPIO_CFG(PIN_CS)), rd(GPIO_IO(PIN_CS)), gpio_get(PIN_CS));

	wr(GPIO_IO(PIN_CS), 2);

	printf("psyche_touch: cstest ----- the engine's select registers -----\n");
	if (se == 0) {
		printf("psyche_touch: cstest no engine mapped\n");
		return;
	}
	printf("psyche_touch: cstest SPI_DEMUX_SEL     = %#x\n", rd(SPI_DEMUX_SEL));
	printf("psyche_touch: cstest SPI_TRANS_CFG     = %#x\n", rd(SPI_TRANS_CFG));
	printf("psyche_touch: cstest GENI_OUTPUT_CTRL  = %#x\n", rd(GENI_OUTPUT_CTRL));
	printf("psyche_touch: cstest GENI_FORCE_DEFAULT= %#x\n", rd(GENI_FORCE_DEFAULT));
	printf("psyche_touch: cstest GENI_CGC_CTRL     = %#x\n", rd(GENI_CGC_CTRL));
	printf("psyche_touch: cstest SPI_DELAY_COUNTERS= %#x\n", rd(SPI_DELAY_COUNTERS));

	/* run one transfer and sample the select as often as possible through it */
	printf("psyche_touch: cstest ----- select while the engine runs -----\n");
	use_engine_pins();
	wr(GPIO_CFG(PIN_CS), 0x3u);           /* read the pad while the engine drives it */
	wr(GPIO_IO(PIN_CS), 0);

	(void)fts_read(FTS_FIFO_CMD_READALL, 0, 0, ev, FTS_FIFO_EVENT_SIZE);

	for (i = 0; i < 40; i++) {
		printf("psyche_touch: cstest   after transfer sample %u: CS=%u\n",
		    i, gpio_get(PIN_CS));
		sleep_us(5);
	}

	printf("psyche_touch: cstest ----- after the transfer, pad state -----\n");
	printf("psyche_touch: cstest CS cfg=%#04x io=%#04x\n",
	    rd(GPIO_CFG(PIN_CS)), rd(GPIO_IO(PIN_CS)));
	printf("psyche_touch: cstest done\n");
}

/*
 * The whole system-information block, out of a read that is already working.
 *
 * At 1 MHz the 0xA7 read returns a valid A5 01 header, three reads in three identical, while
 * every faster rate returns noise -- so the bus can carry the data and the sampling is only
 * sound at the slow rate. What is still open is the chip id: it came back 0x3640 where 0x3936
 * is expected, and 0x36 is the low byte of the expected id, so either the chip is a variant or
 * the field sits one byte off from where the vendor driver reads it.
 *
 * Dumping the block and printing it byte-by-byte, with the known offsets marked, makes that a
 * reading rather than a guess.
 */
static void
probe0xa7dump_probe(void)
{
	#define SYS_INFO_SIZE 208u
	uint8_t info[SYS_INFO_SIZE];
	unsigned attempt;

	printf("psyche_touch: 0xa7dump ----- 208 bytes of system info at 1 MHz -----\n");

	if (geni_spi_init(1000000u) != NULL) {
		printf("psyche_touch: 0xa7dump engine will not run at 1 MHz\n");
		return;
	}
	use_engine_pins();

	for (attempt = 0; attempt < 4; attempt++) {
		unsigned i;

		gpio_output(PIN_RESET, false, PULL_NONE, 6);
		sleep_ms(10);
		gpio_output(PIN_RESET, true, PULL_NONE, 6);
		sleep_ms(60);

		memset(info, 0, sizeof(info));
		if (!fts_read(0xA7, 0x0000, 2, info, SYS_INFO_SIZE)) {
			printf("psyche_touch: 0xa7dump attempt %u: transfer failed\n", attempt);
			continue;
		}
		if (info[0] != 0xA5) {
			printf("psyche_touch: 0xa7dump attempt %u: header %02x (not A5)\n",
			    attempt, info[0]);
			continue;
		}

		printf("psyche_touch: 0xa7dump attempt %u: header OK\n", attempt);
		for (i = 0; i < SYS_INFO_SIZE; i += 16) {
			unsigned j;

			printf("psyche_touch: 0xa7dump %08x:", i);
			for (j = 0; j < 16 && i + j < SYS_INFO_SIZE; j++) {
				printf(" %02x", info[i + j]);
			}
			printf("\n");
		}

		printf("psyche_touch: 0xa7dump fields, read the way the phone reads them:\n");
		printf("psyche_touch: 0xa7dump   apiVer_rev   %04x\n",
		    (unsigned)(info[4] | ((uint16_t)info[5] << 8)));
		printf("psyche_touch: 0xa7dump   apiVer_minor %02x\n", info[6]);
		printf("psyche_touch: 0xa7dump   apiVer_major %02x\n", info[7]);
		printf("psyche_touch: 0xa7dump   chip0Ver     %04x\n",
		    (unsigned)(info[8] | ((uint16_t)info[9] << 8)));
		printf("psyche_touch: 0xa7dump   chip0Id      %04x   (raw %02x %02x, want 3936)\n",
		    (unsigned)(info[10] | ((uint16_t)info[11] << 8)), info[10], info[11]);
		printf("psyche_touch: 0xa7dump   chip1Ver     %04x\n",
		    (unsigned)(info[12] | ((uint16_t)info[13] << 8)));
		printf("psyche_touch: 0xa7dump   chip1Id      %04x\n",
		    (unsigned)(info[14] | ((uint16_t)info[15] << 8)));
		printf("psyche_touch: 0xa7dump   fwVer        %04x\n",
		    (unsigned)(info[16] | ((uint16_t)info[17] << 8)));

		/* hunt for the expected id bytes anywhere in the block */
		{
			unsigned hits = 0;

			for (i = 0; i + 1 < SYS_INFO_SIZE; i++) {
				if (info[i] == 0x36 && info[i + 1] == 0x39) {
					printf("psyche_touch: 0xa7dump *** 36 39 at offset %u ***\n", i);
					hits++;
				}
				if (info[i] == 0x39 && info[i + 1] == 0x36) {
					printf("psyche_touch: 0xa7dump *** 39 36 (swapped) at offset %u ***\n", i);
					hits++;
				}
			}
			if (!hits) {
				printf("psyche_touch: 0xa7dump no 3639/3936 pair in the block\n");
			}
		}
		break;
	}

	(void)geni_spi_init(target_spi_hz);
	use_engine_pins();
	printf("psyche_touch: 0xa7dump done\n");
}

/*
 * Does the long read carry the chip's memory, or the engine's own leftovers?
 *
 * Two things separate a memory from a shift register: contents that stay put when the bus
 * clock changes, and contents that differ between addresses. A real system-information block
 * has a signature at byte 0 and an id at byte 1; nothing about it depends on whether the bus
 * runs at 1 MHz or 8 MHz, and reading a different address must give different bytes.
 *
 * The 0xa7 header has so far come back as eight zeros at every rate and every attempt -- the
 * signature the phone looks for (0xA5) has never appeared -- while the tail carried structured
 * patterns. That is the shape of data sampled from the wrong place, so measure it properly.
 */
static void
probe0xa7clk_probe(void)
{
	#define SYS_INFO_SIZE 208u
	static const uint32_t hzs[] = { 1000000u, 2000000u, 4000000u, 8000000u };
	static const struct { const char *name; uint16_t addr; } places[] = {
		{ "0x0000", 0x0000 },
		{ "0x0000 again", 0x0000 },
		{ "0x0080", 0x0080 },
		{ "0xfff0", 0xfff0 },
	};
	uint8_t info[SYS_INFO_SIZE];
	unsigned k, p, i;

	printf("psyche_touch: 0xa7clk ----- same read, four clocks, four addresses -----\n");

	for (k = 0; k < sizeof(hzs) / sizeof(hzs[0]); k++) {
		const char *p2 = geni_spi_init(hzs[k]);

		if (p2 != NULL) {
			printf("psyche_touch: 0xa7clk clk %u Hz: engine init failed (%s)\n", hzs[k], p2);
			continue;
		}
		use_engine_pins();

		for (p = 0; p < sizeof(places) / sizeof(places[0]); p++) {
			int same = 0;

			memset(info, 0, sizeof(info));
			if (!fts_read(0xA7, places[p].addr, 2, info, SYS_INFO_SIZE)) {
				printf("psyche_touch: 0xa7clk %u Hz @%s: transfer failed\n",
				    hzs[k], places[p].name);
				continue;
			}
			printf("psyche_touch: 0xa7clk %u Hz @%-12s : %08x %08x %08x %08x | %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
			    hzs[k], places[p].name,
			    (unsigned)((info[0] << 24) | (info[1] << 16) | (info[2] << 8) | info[3]),
			    (unsigned)((info[4] << 24) | (info[5] << 16) | (info[6] << 8) | info[7]),
			    (unsigned)((info[8] << 24) | (info[9] << 16) | (info[10] << 8) | info[11]),
			    (unsigned)((info[12] << 24) | (info[13] << 16) | (info[14] << 8) | info[15]),
			    info[0], info[1], info[2], info[3],
			    info[4], info[5], info[6], info[7],
			    info[8], info[9], info[10], info[11]);

			/* three more reads at this rate, to see if it repeats */
			for (i = 0; i < 3; i++) {
				uint8_t again[SYS_INFO_SIZE];

				memset(again, 0, sizeof(again));
				if (fts_read(0xA7, places[p].addr, 2, again, SYS_INFO_SIZE) &&
				    memcmp(again, info, SYS_INFO_SIZE) == 0) {
					same++;
				}
			}
			printf("psyche_touch: 0xa7clk %u Hz @%-12s : %d of 3 repeats identical\n",
			    hzs[k], places[p].name, same);
		}
	}

	(void)geni_spi_init(target_spi_hz);
	use_engine_pins();
	printf("psyche_touch: 0xa7clk done\n");
}

/*
 * Which source clock, and which division, actually carries the chip's data correctly.
 *
 * geni_spi_init() picks the slowest preset at or above 4 MHz -- 6.4 MHz, level 5 -- because a
 * slow bus is the safe choice. The DFS table also holds 19.2 MHz. Three times the bus would
 * make a capture worth watching, but a table number is not evidence: the question is whether
 * the chip still answers the same at 19.2. So ask it, at each candidate, and compare against
 * what it says at the rate already known to work.
 */
static void
speed_probe(void)
{
	#define SYS_INFO_SIZE 208u
	static const struct { uint32_t level; const char *what; } sources[] = {
		{ 5, "6.4 MHz (current)" },
		{ 0, "19.2 MHz (fastest preset)" },
		{ 1, "19.2 MHz (level 1)" },
	};
	uint8_t info[SYS_INFO_SIZE];
	unsigned k, r;
	unsigned good_level = 0;
	unsigned good_hz = 0;

	printf("psyche_touch: speed ----- which source carries the data -----\n");

	for (k = 0; k < sizeof(sources) / sizeof(sources[0]); k++) {
		unsigned repeats = 0;
		uint8_t first[SYS_INFO_SIZE];
		uint32_t source_hz = rcg_rate(rd(GCC_WRAP0_S4_PERF_CFG(sources[k].level)),
		    rd(GCC_WRAP0_S4_PERF_M(sources[k].level)),
		    rd(GCC_WRAP0_S4_PERF_N(sources[k].level)));
		uint64_t ns = 0, elapsed = 0;

		/* select this source, then set the divider the driver would use for 8 MHz */
		wr(GENI_CLK_SEL, sources[k].level & 7);
		{
			uint32_t divider = (source_hz + 8000000u - 1) / 8000000u;

			wr(GENI_SER_M_CLK_CFG, (divider ? divider : 1) << 4 | 1);
		}
		sleep_ms(5);

		gpio_output(PIN_RESET, false, PULL_NONE, 6);
		sleep_ms(10);
		gpio_output(PIN_RESET, true, PULL_NONE, 6);
		sleep_ms(60);

		memset(info, 0, sizeof(info));
		/*
		 * Twenty reads timed together: one 208-byte read is well under a millisecond,
		 * so a single read's elapsed time rounds to zero and says nothing.
		 */
		{
			const unsigned timed = 20;
			uint64_t t0, t1;
			uint32_t us;
			unsigned t;
			uint8_t tmp[SYS_INFO_SIZE];

			t0 = mach_absolute_time();
			for (t = 0; t < timed; t++) {
				memset(tmp, 0, sizeof(tmp));
				(void)fts_read(0xA7, 0x0000, 2, tmp, SYS_INFO_SIZE);
			}
			t1 = mach_absolute_time();
			ns = t1 - t0;
			absolutetime_to_nanoseconds(ns, &elapsed);
			us = (uint32_t)(elapsed / 1000ull);
			printf("psyche_touch: speed %-26s : %u us for %u reads (%u us each)\n",
			    sources[k].what, us, timed, us / timed);
			memcpy(info, tmp, sizeof(info));
			if (info[0] != 0xA5) {
				printf("psyche_touch: speed %-26s : read failed\n", sources[k].what);
				continue;
			}
		}
		printf("psyche_touch: speed %-26s : %02x %02x | fw %04x | chipid %04x\n",
		    sources[k].what, info[0], info[1],
		    (unsigned)(info[16] | ((uint16_t)info[17] << 8)),
		    (unsigned)(info[10] | ((uint16_t)info[11] << 8)));
		memcpy(first, info, sizeof(first));

		/* four more reads: the header has to stay a signature and the data has to agree */
		for (r = 0; r < 4; r++) {
			uint8_t again[SYS_INFO_SIZE];

			memset(again, 0, sizeof(again));
			if (fts_read(0xA7, 0x0000, 2, again, SYS_INFO_SIZE) &&
			    again[0] == 0xA5 && memcmp(again, first, sizeof(first)) == 0) {
				repeats++;
			}
			sleep_ms(10);
		}
		printf("psyche_touch: speed %-26s : %u of 4 repeats identical\n",
		    sources[k].what, repeats);
		if (repeats == 4 && good_hz == 0) {
			good_level = sources[k].level;
			good_hz = source_hz;
		}
	}

	printf("psyche_touch: speed ----- result -----\n");
	if (good_hz != 0) {
		printf("psyche_touch: speed fastest that repeats cleanly: level %u at %u Hz\n",
		    good_level, good_hz);
	} else {
		printf("psyche_touch: speed nothing repeated cleanly\n");
	}
	printf("psyche_touch: speed done\n");
}

/*
 * What rates the SE4 clock source can actually produce.
 *
 * geni_spi_init() divides the source down: divider = ceil(source / wanted), so the source is a
 * ceiling on the bus. It currently picks the slowest preset above 4 MHz, which is why asking
 * for the device tree's 8 MHz yields 6.4. If a faster preset exists, a faster bus is possible
 * without touching anything else -- so read the whole DFS table out.
 */
static void
clklevels_probe(void)
{
	unsigned level;
	unsigned best = 0, best_hz = 0;

	printf("psyche_touch: clklevels ----- SE4 preset rates -----\n");
	printf("psyche_touch: clklevels DFSR=%#x  source now %u Hz (level %u)\n",
	    rd(GCC_WRAP0_S4_DFSR), se_source_hz, se_clock_select);

	for (level = 0; level < 8; level++) {
		uint32_t hz = rcg_rate(rd(GCC_WRAP0_S4_PERF_CFG(level)),
		    rd(GCC_WRAP0_S4_PERF_M(level)), rd(GCC_WRAP0_S4_PERF_N(level)));

		printf("psyche_touch: clklevels level %u: cfg=%#x m=%#x n=%#x -> %u Hz\n",
		    level, rd(GCC_WRAP0_S4_PERF_CFG(level)),
		    rd(GCC_WRAP0_S4_PERF_M(level)), rd(GCC_WRAP0_S4_PERF_N(level)), hz);
		if (hz > best_hz) {
			best_hz = hz;
			best = level;
		}
	}
	printf("psyche_touch: clklevels fastest preset: level %u at %u Hz\n", best, best_hz);
	printf("psyche_touch: clklevels DTS allows %u Hz (spi-max-frequency)\n", 8000000);

	/*
	 * Select the fastest preset and see what the bus then reports. This is a read-only
	 * experiment: if the selection does not stick, the clock framework will keep what it has.
	 */
	wr(GENI_CLK_SEL, best & 7);
	wr(GENI_SER_M_CLK_CFG, (1u << 4) | 1u);
	printf("psyche_touch: clklevels switched to level %u, divider 1 (no division)\n", best);
	printf("psyche_touch: clklevels GENI_CLK_SEL=%#x SER_M_CLK_CFG=%#x CLK_CTRL_RO=%#x\n",
	    rd(GENI_CLK_SEL), rd(GENI_SER_M_CLK_CFG), rd(se + 0x60));
	printf("psyche_touch: clklevels done\n");
}

/*
 * Watch the chip's FIFO from a plain loop, for seconds rather than milliseconds.
 *
 * Everything else in this kernel is verified: the chip answers system info (fw 0016), comes out
 * of reset reporting CONTROLLER_READY, and the FIFO read completes. What is never observed is a
 * touch event, because the two places that could observe one do not live long enough: the poll
 * thread is killed with the kernel by the userland launch ~5 ms after it starts, and the probe's
 * three reads cover about one tenth of a second -- too short for a hand.
 *
 * A loop here needs no thread, no userland, and no interrupt: it just reads the FIFO and prints
 * anything that is not the empty event. Counting every non-empty event, plus the INT pin level
 * at each one, is the measurement.
 */
static void
fifowatch_probe(void)
{
	#define WATCH_SECONDS 20
	#define WATCH_STEP_MS 100
	unsigned reads = 0, nonempty = 0, failed = 0, irq_low = 0, i;

	printf("psyche_touch: fifowatch ----- watching the fifo for %u s -----\n", WATCH_SECONDS);
	printf("psyche_touch: fifowatch touch the screen now\n");

	for (i = 0; i < WATCH_SECONDS * (1000 / WATCH_STEP_MS); i++) {
		uint8_t data[FTS_FIFO_EVENT_SIZE * FTS_FIFO_DEPTH];
		uint32_t events = 0;

		if (!gpio_get(PIN_INT)) {
			irq_low++;
		}
		memset(data, 0, sizeof(data));
		reads++;
		if (!fts_read_fifo(data, &events)) {
			failed++;
		} else if (data[0] != FTS_EVT_NOEVENT) {
			unsigned e;
			unsigned j = i;

			/* drain and print every event the read returned, not only the first */
			for (e = 0; e < events && e < FTS_FIFO_DEPTH; e++) {
				const uint8_t *ev = &data[e * FTS_FIFO_EVENT_SIZE];

				if (ev[0] == FTS_EVT_NOEVENT) {
					break;
				}
				nonempty++;
				/*
				 * Print a few and count the rest. Every hakios_tl() runs a
				 * loader_checkpoint(), so a line per event means the loader is asked
				 * thousands of times whether it wants the CPU back, and it stops
				 * getting turns often enough to answer USB -- which is what makes the
				 * runner think the kernel hung.
				 */
				if (nonempty <= 8) {
					printf("psyche_touch: fifowatch %u.%u: irq=%u : %02x %02x %02x %02x %02x %02x %02x %02x\n",
					    j, e, gpio_get(PIN_INT),
					    ev[0], ev[1], ev[2], ev[3],
					    ev[4], ev[5], ev[6], ev[7]);
				}
				/* and actually act on it -- printing is not consuming */
				fts_report_event(ev);
				stat_reports++;
			}
		}
		sleep_ms(WATCH_STEP_MS);
	}

	printf("psyche_touch: fifowatch ----- result -----\n");
	printf("psyche_touch: fifowatch reads=%u nonempty=%u failed=%u irq_low=%u\n",
	    reads, nonempty, failed, irq_low);
	printf("psyche_touch: fifowatch posted to input queue: touches=%u bad=%u last=(%u,%u)\n",
	    stat_touches, stat_bad, stat_last_x, stat_last_y);
	if (nonempty) {
		printf("psyche_touch: fifowatch *** THE CHIP REPORTED EVENTS ***\n");
	} else {
		printf("psyche_touch: fifowatch only empty events; irq stayed %s\n",
		    irq_low ? "mostly low" : "high");
	}
	printf("psyche_touch: fifowatch done\n");
}

/*
 * The phone's own probe sequence, replayed byte for byte.
 *
 * Everything in this kernel so far has led with 0xFB/0x20000000, a register read that the
 * vendor driver only ever issues from its diagnostics interface. The transaction that actually
 * starts a working probe on this hardware is the system-information read: 0xA7 with a two-byte
 * address of zero, 209 bytes back, first byte dummy. Its answer carries a signature, an id and
 * the chip id -- so one successful read proves the whole chain and hands over the identification.
 *
 * Retries matter here: the phone issues this once and accepts a 100 ms poll for the controller
 * ready event first, so a single failed attempt says nothing.
 */
static void
probe0xa7_probe(void)
{
	#define SYS_INFO_SIZE 208u
	#define LOAD_SYS_INFO 0x01
	#define HEADER_SIGNATURE 0xA5
	uint8_t info[SYS_INFO_SIZE];
	unsigned attempt;

	printf("psyche_touch: 0xa7 ----- the phone's first read, replayed -----\n");

	for (attempt = 0; attempt < 6; attempt++) {
		uint8_t before[FTS_FIFO_EVENT_SIZE];
		uint32_t n = 0;
		unsigned i;

		/* whatever the fifo holds, drain it before the reset: the phone does the same */
		(void)fts_read_fifo(before, &n);

		gpio_output(PIN_RESET, false, PULL_NONE, 6);
		sleep_ms(10);
		gpio_output(PIN_RESET, true, PULL_NONE, 6);
		/* the phone polls the ready event for up to 100 ms before reading system info */
		sleep_ms(50);

		/* and it drains the fifo once more, so a pending 0x03 is not left to confuse things */
		(void)fts_read_fifo(before, &n);

		memset(info, 0, sizeof(info));
		if (!fts_read(0xA7, 0x0000, 2, info, SYS_INFO_SIZE)) {
			printf("psyche_touch: 0xa7 attempt %u: the transfer did not complete\n", attempt);
			continue;
		}

		printf("psyche_touch: 0xa7 attempt %u: header %02x %02x %02x %02x %02x %02x %02x %02x\n",
		    attempt, info[0], info[1], info[2], info[3],
		    info[4], info[5], info[6], info[7]);
		printf("psyche_touch: 0xa7 attempt %u: bytes 8..23:", attempt);
		for (i = 8; i < 24; i++) {
			printf(" %02x", info[i]);
		}
		printf("\n");

		if (info[0] == HEADER_SIGNATURE) {
			uint16_t chip_id = (uint16_t)(info[10] | ((uint16_t)info[11] << 8));

			printf("psyche_touch: 0xa7 *** HEADER SIGNATURE 0xA5 *** data id %02x (want %02x)\n",
			    info[1], LOAD_SYS_INFO);
			printf("psyche_touch: 0xa7 *** chip id %04x (raw %02x %02x) ***\n",
			    chip_id, info[10], info[11]);
			printf("psyche_touch: 0xa7 fw version %04x\n",
			    (unsigned)(info[16] | ((uint16_t)info[17] << 8)));
			if (chip_id == ((FTS_DCHIP_ID_1 << 8) | FTS_DCHIP_ID_0)) {
				printf("psyche_touch: 0xa7 *** THIS IS THE CHIP: 0x3936 ***\n");
			}
			break;
		}
		printf("psyche_touch: 0xa7 attempt %u: no signature, ios=%#010x\n",
		    attempt, rd(se + 0x908));
		sleep_ms(20);
	}

	/* for comparison, the command this kernel has been leading with */
	printf("psyche_touch: 0xa7 ----- the register read, for comparison -----\n");
	{
		uint8_t d[2] = { 0, 0 };

		(void)fts_read(FTS_CMD_HW_REG_R, FTS_ADDR_DCHIP_ID, FTS_ADDR_SIZE_HW_REG, d, 2);
		printf("psyche_touch: 0xa7 0xfb/0x20000000 -> %02x %02x\n", d[1], d[0]);
	}
	printf("psyche_touch: 0xa7 done\n");
}

/*
 * The data line, under power, asked the same question three ways: left alone, pulled up,
 * pulled down. Whatever the answer is, it has to change when the line's own bias changes -- if
 * it does not, then the bytes are the engine sampling a line it drives, not the chip talking.
 *
 * Every command the driver uses goes through this, because the FIFO read and the register read
 * take different paths through the engine.
 */
static void
wirepow_probe(void)
{
	static const struct { const char *name; uint8_t cmd; uint8_t addr[4]; uint8_t addr_size; uint32_t len; } cmds[] = {
		{ "fifo 0x87", 0x87, { 0 }, 0, 8 },
		{ "chipid fb20", 0xfb, { 0x20, 0x00, 0x00, 0x00 }, 4, 2 },
		{ "sysinfo a6", 0xa6, { 0x00, 0x00, 0x00, 0x00 }, 3, 4 },
	};
	static const struct { const char *name; uint32_t bias; } bias[] = {
		{ "none", 0x0 }, { "pull-up", 0x3 }, { "pull-down", 0x1 },
	};
	uint8_t rx[FTS_RX_SIZE];
	unsigned b, c, r;

	printf("psyche_touch: wirepow ----- supplies first -----\n");
	{
		uint32_t v = 0;

		(void)rpmh_read(0x41f00 + VRM_ENABLE, &v);
		printf("psyche_touch: wirepow vdd  enable = %u\n", v);
		(void)rpmh_read(0x41200 + VRM_ENABLE, &v);
		printf("psyche_touch: wirepow avdd enable = %u\n", v);
		(void)rpmh_read(0x41200 + VRM_VOLTAGE, &v);
		printf("psyche_touch: wirepow avdd voltage = %u mV\n", v);
		printf("psyche_touch: wirepow vddio gpio%u cfg=%#x io=%#x\n",
		    PIN_VDDIO, rd(GPIO_CFG(PIN_VDDIO)), rd(GPIO_IO(PIN_VDDIO)));
	}

	use_engine_pins();

	for (b = 0; b < sizeof(bias) / sizeof(bias[0]); b++) {
		uint32_t cfg;

		cfg = rd(GPIO_CFG(PIN_MISO));
		wr(GPIO_CFG(PIN_MISO), (cfg & ~0x3u) | bias[b].bias);
		printf("psyche_touch: wirepow ----- data pad %s (cfg %#04x) -----\n",
		    bias[b].name, rd(GPIO_CFG(PIN_MISO)));

		/* a reset with this bias in place, so every row starts from the same chip state */
		gpio_output(PIN_RESET, false, PULL_NONE, 6);
		sleep_ms(10);
		gpio_output(PIN_RESET, true, PULL_NONE, 6);
		sleep_ms(30);

		for (c = 0; c < sizeof(cmds) / sizeof(cmds[0]); c++) {
			printf("psyche_touch: wirepow   %-12s :", cmds[c].name);
			for (r = 0; r < 4; r++) {
				uint64_t addr = 0;
				bool ok;

				for (int k = 0; k < cmds[c].addr_size; k++) {
					addr = (addr << 8) | cmds[c].addr[k];
				}
				ok = fts_read(cmds[c].cmd, addr, cmds[c].addr_size, rx, cmds[c].len);
				if (!ok) {
					printf(" <failed>");
				} else {
					printf(" [");
					for (uint32_t k = 0; k < cmds[c].len; k++) {
						printf("%02x", rx[k]);
					}
					printf("]");
				}
				sleep_ms(5);
			}
			printf("  pad io=%#04x\n", rd(GPIO_IO(PIN_MISO)));
		}
	}

	/* put the pad back the way the engine wants it */
	{
		uint32_t cfg = rd(GPIO_CFG(PIN_MISO));

		wr(GPIO_CFG(PIN_MISO), cfg & ~0x3u);
	}
	printf("psyche_touch: wirepow done\n");
}

/*
 * Whether the engine is actually driving the pads. A manual poke proves the select line can be
 * pulled low, yet it never moves during a transfer -- and GENI_FORCE_DEFAULT, which makes the
 * engine take over the default signals, reads zero although it was set during init. That is the
 * shape of a bus whose engine works inside and says nothing outside.
 */
static void
genidrive_probe(void)
{
	uint8_t ev[FTS_FIFO_EVENT_SIZE];
	unsigned i;
	uint32_t before;

	printf("psyche_touch: genidrive ----- force default and select -----\n");

	if (se == 0) {
		printf("psyche_touch: genidrive no engine\n");
		return;
	}

	/* put the four pads back on the engine before touching anything */
	use_engine_pins();
	sleep_us(20);

	before = rd(GENI_FORCE_DEFAULT);
	printf("psyche_touch: genidrive FORCE_DEFAULT before = %#x\n", before);

	/* 1: let the engine own the default signals */
	wr(GENI_FORCE_DEFAULT, 1);
	printf("psyche_touch: genidrive FORCE_DEFAULT after  = %#x\n", rd(GENI_FORCE_DEFAULT));
	printf("psyche_touch: genidrive OUTPUT_CTRL = %#x\n", rd(GENI_OUTPUT_CTRL));

	/*
	 * The select can only be watched with the pad on the engine: reading it as a plain input
	 * reports the pin, not the engine's intent. So sample the pad configuration instead, and
	 * look at where the engine leaves its own signal state.
	 */
	printf("psyche_touch: genidrive CS pad cfg=%#04x io=%#04x\n",
	    rd(GPIO_CFG(PIN_CS)), rd(GPIO_IO(PIN_CS)));

	/*
	 * SPI_DELAY_COUNTERS is the engine's own chip-select delay; the phone's driver waits 10 us
	 * before raising the select (SPI_DELAY_CS). Writing a non-zero value here gives the chip
	 * that settling time.
	 */
	wr(SPI_DELAY_COUNTERS, 0x00000010);       /* ~10 us in engine units */
	printf("psyche_touch: genidrive DELAY_COUNTERS = %#x\n", rd(SPI_DELAY_COUNTERS));

	/* run a few transfers and see whether anything about the pads changes */
	for (i = 0; i < 3; i++) {
		(void)fts_read(FTS_FIFO_CMD_READALL, 0, 0, ev, FTS_FIFO_EVENT_SIZE);
		printf("psyche_touch: genidrive transfer %u: CS pad io=%#04x cfg=%#04x  MISO io=%#04x\n",
		    i, rd(GPIO_IO(PIN_CS)), rd(GPIO_CFG(PIN_CS)), rd(GPIO_IO(PIN_MISO)));
		sleep_ms(5);
	}

	/* and the chip id */
	{
		uint8_t d[2] = { 0, 0 };

		if (fts_read(FTS_CMD_HW_REG_R, FTS_ADDR_DCHIP_ID, FTS_ADDR_SIZE_HW_REG, d, 2)) {
			printf("psyche_touch: genidrive chip id %02x %02x\n", d[0], d[1]);
		} else {
			printf("psyche_touch: genidrive chip id read failed\n");
		}
	}
	printf("psyche_touch: genidrive done\n");
}

/*
 * Is the engine putting anything on the wire? Sampling the clock pin as an input during a
 * transfer counts how often the line changes: a clocked bus changes it many times, a silent one
 * never. This is the one measurement that separates "the engine works" from "the engine reaches
 * the pads", which the select line already suggests it does not.
 */
static void
clkprobe_probe(void)
{
	uint8_t ev[FTS_FIFO_EVENT_SIZE];
	unsigned i, edges_live = 0, edges_quiet = 0;

	printf("psyche_touch: clkprobe ----- quiet baseline -----\n");
	wr(GPIO_IO(PIN_CLK), 0);
	wr(GPIO_CFG(PIN_CLK), 0x0u);           /* plain input, no pull */
	sleep_us(20);
	{
		unsigned last = gpio_get(PIN_CLK);

		for (i = 0; i < 2000; i++) {
			unsigned now = gpio_get(PIN_CLK);

			if (now != last) {
				edges_live++;
				last = now;
			}
			sleep_us(1);
		}
		printf("psyche_touch: clkprobe quiet: %u edges in 2000 samples\n", edges_live);
		edges_live = 0;
	}

	printf("psyche_touch: clkprobe ----- with the engine running -----\n");
	use_engine_pins();
	if (se != 0 && engine_transfer2 != NULL) {
		/*
		 * The engine owns the clock now, so this reads the pad while the engine toggles it.
		 * fts_read clocks out a command and clocks in an answer, which is a lot of edges.
		 */
		wr(GPIO_IO(PIN_CLK), 0);
		wr(GPIO_CFG(PIN_CLK), 0x0u);
		sleep_us(5);

		(void)fts_read(FTS_FIFO_CMD_READALL, 0, 0, ev, FTS_FIFO_EVENT_SIZE);
	}
	printf("psyche_touch: clkprobe after a transfer, CLK io=%#04x cfg=%#04x level=%u\n",
	    rd(GPIO_IO(PIN_CLK)), rd(GPIO_CFG(PIN_CLK)), gpio_get(PIN_CLK));

	/* try the transfer again with the pad left on the engine, sampling before and after */
	use_engine_pins();
	{
		uint32_t cs_before = rd(GPIO_IO(PIN_CS));
		uint32_t miso_before = rd(GPIO_IO(PIN_MISO));

		if (se != 0 && engine_transfer2 != NULL) {
			(void)fts_read(FTS_FIFO_CMD_READALL, 0, 0, ev, FTS_FIFO_EVENT_SIZE);
		}
		printf("psyche_touch: clkprobe pads: CS %#04x -> %#04x, MISO %#04x -> %#04x\n",
		    cs_before, rd(GPIO_IO(PIN_CS)), miso_before, rd(GPIO_IO(PIN_MISO)));
		printf("psyche_touch: clkprobe pad cfgs: CS=%#04x CLK=%#04x MISO=%#04x MOSI=%#04x\n",
		    rd(GPIO_CFG(PIN_CS)), rd(GPIO_CFG(PIN_CLK)),
		    rd(GPIO_CFG(PIN_MISO)), rd(GPIO_CFG(PIN_MOSI)));
	}

	/* what the engine says it did */
	printf("psyche_touch: clkprobe GENI_STATUS=%#x M_IRQ_STATUS=%#x\n",
	    rd(GENI_STATUS), rd(GENI_M_IRQ_STATUS));
	printf("psyche_touch: clkprobe TX_FIFO_STATUS=%#x RX_FIFO_STATUS=%#x\n",
	    rd(GENI_TX_FIFO_STATUS), rd(GENI_RX_FIFO_STATUS));
	printf("psyche_touch: clkprobe done\n");
}

/*
 * The software transport, watched pin by pin. Bit banging drives the pads as plain GPIOs and
 * never involves the serial engine, so if the select and clock move here the pads work and the
 * fault is in the engine; if they do not move here either, nothing this kernel drives reaches
 * the bus.
 */
static void
bbprobe_probe(void)
{
	unsigned i;
	unsigned cs_changes = 0, clk_changes = 0, out_changes = 0;

	printf("psyche_touch: bbprobe ----- bit bang the bus, watching pins -----\n");

	/* three plain GPIOs out and one in, as bitbang_init does */
	bitbang_in = PIN_MISO;
	bitbang_out = PIN_MOSI;
	bitbang_init();

	printf("psyche_touch: bbprobe before: CS io=%#04x cfg=%#04x  CLK io=%#04x cfg=%#04x\n",
	    rd(GPIO_IO(PIN_CS)), rd(GPIO_CFG(PIN_CS)),
	    rd(GPIO_IO(PIN_CLK)), rd(GPIO_CFG(PIN_CLK)));

	/*
	 * One FIFO command through the bit banger, then the same pins read back. Each change is
	 * counted through the pad's own input bit, which sees the level the pad is actually at.
	 */
	{
		uint8_t ev[FTS_FIFO_EVENT_SIZE];
		unsigned last_cs = gpio_get(PIN_CS), last_clk = gpio_get(PIN_CLK);
		unsigned last_out = gpio_get(PIN_MOSI);

		fts_read(FTS_FIFO_CMD_READALL, 0, 0, ev, FTS_FIFO_EVENT_SIZE);

		for (i = 0; i < 200; i++) {
			unsigned c = gpio_get(PIN_CS), k = gpio_get(PIN_CLK), o = gpio_get(PIN_MOSI);

			if (c != last_cs) { cs_changes++; last_cs = c; }
			if (k != last_clk) { clk_changes++; last_clk = k; }
			if (o != last_out) { out_changes++; last_out = o; }
			sleep_us(2);
		}
		printf("psyche_touch: bbprobe after a transfer: cs_changes=%u clk_changes=%u out_changes=%u\n",
		    cs_changes, clk_changes, out_changes);
	}

	/* now hold the select low by hand and read the level it settles at */
	printf("psyche_touch: bbprobe ----- hand drive the three outputs -----\n");
	gpio_output(PIN_CS, false, PULL_NONE, 6);
	sleep_us(20);
	printf("psyche_touch: bbprobe CS low  -> io=%#04x level=%u\n",
	    rd(GPIO_IO(PIN_CS)), gpio_get(PIN_CS));
	gpio_output(PIN_CS, true, PULL_NONE, 6);
	sleep_us(20);
	printf("psyche_touch: bbprobe CS high -> io=%#04x level=%u\n",
	    rd(GPIO_IO(PIN_CS)), gpio_get(PIN_CS));

	gpio_output(PIN_CLK, true, PULL_NONE, 6);
	sleep_us(20);
	printf("psyche_touch: bbprobe CLK high-> io=%#04x level=%u\n",
	    rd(GPIO_IO(PIN_CLK)), gpio_get(PIN_CLK));
	gpio_output(PIN_CLK, false, PULL_NONE, 6);
	sleep_us(20);
	printf("psyche_touch: bbprobe CLK low -> io=%#04x level=%u\n",
	    rd(GPIO_IO(PIN_CLK)), gpio_get(PIN_CLK));

	/* MOSI moves too? */
	gpio_output(PIN_MOSI, true, PULL_NONE, 6);
	sleep_us(20);
	printf("psyche_touch: bbprobe MOSI high-> io=%#04x level=%u\n",
	    rd(GPIO_IO(PIN_MOSI)), gpio_get(PIN_MOSI));
	gpio_output(PIN_MOSI, false, PULL_NONE, 6);
	sleep_us(20);
	printf("psyche_touch: bbprobe MOSI low -> io=%#04x level=%u\n",
	    rd(GPIO_IO(PIN_MOSI)), gpio_get(PIN_MOSI));

	printf("psyche_touch: bbprobe done\n");
}

/*
 * Whether writing the pad's own output bit changes the pin. A hand-driven pad reaches both levels
 * while the same pads sit still through a bit-banged transfer, so the question is what a bare
 * write to the output latch does on this pad -- and whether the pad has to be an output first.
 */
static void
gpioset_probe(void)
{
	unsigned i;

	printf("psyche_touch: gpioset ----- what a bare output write does -----\n");

	for (i = 0; i < 4; i++) {
		unsigned n = i == 0 ? PIN_CS : i == 1 ? PIN_CLK : i == 2 ? PIN_MOSI : PIN_MISO;
		const char *nm = i == 0 ? "CS" : i == 1 ? "CLK" : i == 2 ? "MOSI" : "MISO";

		/* as it is now */
		printf("psyche_touch: gpioset %-4s start: cfg=%#04x io=%#04x level=%u\n",
		    nm, rd(GPIO_CFG(n)), rd(GPIO_IO(n)), gpio_get(n));

		/* make it an output, GPIO function */
		wr(GPIO_IO(n), 0);
		wr(GPIO_CFG(n), 0x280u);          /* function 0, output, 12 mA */
		sleep_us(20);
		printf("psyche_touch: gpioset %-4s as out: cfg=%#04x io=%#04x level=%u\n",
		    nm, rd(GPIO_CFG(n)), rd(GPIO_IO(n)), gpio_get(n));

		/* the bare write the bit banger uses */
		gpio_set(n, true);
		sleep_us(20);
		printf("psyche_touch: gpioset %-4s bare gpio_set(1): io=%#04x level=%u\n",
		    nm, rd(GPIO_IO(n)), gpio_get(n));

		gpio_set(n, false);
		sleep_us(20);
		printf("psyche_touch: gpioset %-4s bare gpio_set(0): io=%#04x level=%u\n",
		    nm, rd(GPIO_IO(n)), gpio_get(n));

		/* what gpio_output does */
		gpio_output(n, true, PULL_NONE, 6);
		sleep_us(20);
		printf("psyche_touch: gpioset %-4s gpio_output(1): io=%#04x level=%u\n",
		    nm, rd(GPIO_IO(n)), gpio_get(n));
		gpio_output(n, false, PULL_NONE, 6);
		sleep_us(20);
		printf("psyche_touch: gpioset %-4s gpio_output(0): io=%#04x level=%u\n",
		    nm, rd(GPIO_IO(n)), gpio_get(n));
	}

	/* and the bit banger's own delay setting */
	printf("psyche_touch: gpioset ----- bitbang delay -----\n");
	printf("psyche_touch: gpioset bitbang_delay_ns=%u\n", (unsigned)bitbang_delay_ns);

	bitbang_in = PIN_MISO;
	bitbang_out = PIN_MOSI;
	bitbang_init();
	printf("psyche_touch: gpioset after bitbang_init: CS cfg=%#04x io=%#04x  CLK cfg=%#04x io=%#04x  MISO cfg=%#04x\n",
	    rd(GPIO_CFG(PIN_CS)), rd(GPIO_IO(PIN_CS)),
	    rd(GPIO_CFG(PIN_CLK)), rd(GPIO_IO(PIN_CLK)),
	    rd(GPIO_CFG(PIN_MISO)));

	printf("psyche_touch: gpioset done\n");
}

/*
 * Counting the select and the clock from inside the transfer, not after it. Sampling afterwards
 * can only ever see an idle bus -- which is what made an earlier run look like the pads never
 * moved, while the very same pads reach both levels when driven by hand.
 */
static unsigned count_cs_toggles, count_clk_toggles, count_miso_high;

static bool
bitbang_counting(const uint8_t *tx, uint32_t txlen, uint8_t *rx, uint32_t rxlen)
{
	unsigned prev_cs = gpio_get(PIN_CS), prev_clk = gpio_get(PIN_CLK);

	count_cs_toggles = 0;
	count_clk_toggles = 0;
	count_miso_high = 0;

	gpio_set(PIN_CS, false);
	if (gpio_get(PIN_CS) != prev_cs) {
		count_cs_toggles++;
	}

	for (uint32_t i = 0; i < txlen; i++) {
		uint8_t out = tx ? tx[i] : 0;

		for (int bit = 7; bit >= 0; bit--) {
			gpio_set(bitbang_out, out >> bit & 1);
			gpio_set(PIN_CLK, true);
			if (gpio_get(PIN_CLK) != prev_clk) {
				count_clk_toggles++;
				prev_clk = gpio_get(PIN_CLK);
			}
			delay_for_interval(1, NSEC_PER_USEC);
			gpio_set(PIN_CLK, false);
			if (gpio_get(PIN_CLK) != prev_clk) {
				count_clk_toggles++;
				prev_clk = gpio_get(PIN_CLK);
			}
			delay_for_interval(1, NSEC_PER_USEC);
		}
	}
	for (uint32_t i = 0; i < rxlen; i++) {
		uint8_t in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			gpio_set(PIN_CLK, true);
			if (gpio_get(PIN_CLK) != prev_clk) {
				count_clk_toggles++;
				prev_clk = gpio_get(PIN_CLK);
			}
			delay_for_interval(1, NSEC_PER_USEC);
			if (gpio_get(bitbang_in)) {
				count_miso_high++;
			}
			in = (uint8_t)(in << 1 | gpio_get(bitbang_in));
			gpio_set(PIN_CLK, false);
			if (gpio_get(PIN_CLK) != prev_clk) {
				count_clk_toggles++;
				prev_clk = gpio_get(PIN_CLK);
			}
			delay_for_interval(1, NSEC_PER_USEC);
		}
		if (rx) {
			rx[i] = in;
		}
	}
	gpio_set(PIN_CS, true);
	if (gpio_get(PIN_CS) != prev_cs) {
		count_cs_toggles++;
	}
	return true;
}

static void
bbcount_probe(void)
{
	uint8_t ev[FTS_FIFO_EVENT_SIZE];
	uint32_t i;

	printf("psyche_touch: bbcount ----- counting from inside the transfer -----\n");

	bitbang_in = PIN_MISO;
	bitbang_out = PIN_MOSI;
	bitbang_init();
	bitbang_delay_ns = 0;

	/* one FIFO read command: 1 byte out, 9 bytes in */
	engine_transfer2 = NULL;
	spi_transfer = bitbang_transfer;

	printf("psyche_touch: bbcount before: CS cfg=%#04x io=%#04x  CLK cfg=%#04x io=%#04x\n",
	    rd(GPIO_CFG(PIN_CS)), rd(GPIO_IO(PIN_CS)),
	    rd(GPIO_CFG(PIN_CLK)), rd(GPIO_IO(PIN_CLK)));

	(void)bitbang_counting((uint8_t[]){ FTS_FIFO_CMD_READALL }, 1, ev, FTS_FIFO_EVENT_SIZE + FTS_DUMMY_FIFO);

	printf("psyche_touch: bbcount inside the transfer: cs_toggles=%u clk_toggles=%u miso_high=%u\n",
	    count_cs_toggles, count_clk_toggles, count_miso_high);
	printf("psyche_touch: bbcount read:");
	for (i = 0; i < FTS_FIFO_EVENT_SIZE + FTS_DUMMY_FIFO; i++) {
		printf(" %02x", ev[i]);
	}
	printf("\n");

	/* and what the pads settled at */
	printf("psyche_touch: bbcount after: CS io=%#04x CLK io=%#04x MISO io=%#04x\n",
	    rd(GPIO_IO(PIN_CS)), rd(GPIO_IO(PIN_CLK)), rd(GPIO_IO(PIN_MISO)));

	/* expected: cs_toggles should be 2 (low at start, high at end), clk_toggles = 2*clock bits */
	printf("psyche_touch: bbcount expected: cs_toggles>=2, clk_toggles=%u (2 per bit, %u bits)\n",
	    (unsigned)(2 * (8 * 1 + 8 * (FTS_FIFO_EVENT_SIZE + FTS_DUMMY_FIFO))),
	    (unsigned)(8 * 1 + 8 * (FTS_FIFO_EVENT_SIZE + FTS_DUMMY_FIFO)));
	printf("psyche_touch: bbcount done\n");
}

/*
 * Is anything on the other end of these wires? The bus itself is proven: the clock toggles the
 * expected number of times, the select moves, and the pads reach both levels by hand. What has
 * never been shown is the chip responding. Giving the data line a pull-up and then talking to
 * the chip is the test: a chip that is there and running pulls the line against the pull, one
 * that is absent or unpowered cannot.
 */
static void
chipcheck_probe(void)
{
	uint8_t ev[FTS_FIFO_EVENT_SIZE];
	unsigned i;
	unsigned miso_low_during = 0, miso_high_during = 0;

	printf("psyche_touch: chipcheck ----- the data line, with and without the chip talking -----\n");

	bitbang_in = PIN_MISO;
	bitbang_out = PIN_MOSI;
	bitbang_init();
	bitbang_delay_ns = 0;

	/* 1. the line with nothing going on */
	gpio_configure(PIN_MISO, 0, PULL_UP, 2, false);
	sleep_us(50);
	printf("psyche_touch: chipcheck idle, MISO pulled up = %u (1 = nothing driving it)\n",
	    gpio_get(PIN_MISO));

	/* 2. reset the chip, then talk to it, sampling the line while the select is low */
	printf("psyche_touch: chipcheck resetting the chip\n");
	gpio_output(PIN_RESET, false, PULL_NONE, 6);
	sleep_ms(10);
	gpio_output(PIN_RESET, true, PULL_NONE, 6);
	sleep_ms(20);

	gpio_configure(PIN_MISO, 0, PULL_UP, 2, false);

	printf("psyche_touch: chipcheck after reset, MISO = %u\n", gpio_get(PIN_MISO));

	/* 3. hold the select low (the chip is selected) and watch the data line */
	gpio_output(PIN_CS, false, PULL_NONE, 6);
	printf("psyche_touch: chipcheck select LOW, watching the data line for 100 ms\n");
	for (i = 0; i < 100; i++) {
		if (gpio_get(PIN_MISO)) {
			miso_high_during++;
		} else {
			miso_low_during++;
		}
		sleep_us(1000);
	}
	printf("psyche_touch: chipcheck with select low: MISO low=%u high=%u\n",
	    miso_low_during, miso_high_during);
	gpio_output(PIN_CS, true, PULL_NONE, 6);

	/* 4. and one real command, counting the data line through it */
	{
		unsigned low = 0, high = 0;

		gpio_configure(PIN_MISO, 0, PULL_UP, 2, false);
		gpio_set(PIN_CS, false);
		for (i = 0; i < 80; i++) {
			gpio_set(PIN_CLK, true);
			delay_for_interval(1, NSEC_PER_USEC);
			if (gpio_get(PIN_MISO)) {
				high++;
			} else {
				low++;
			}
			gpio_set(PIN_CLK, false);
			delay_for_interval(1, NSEC_PER_USEC);
		}
		gpio_set(PIN_CS, true);
		printf("psyche_touch: chipcheck through 80 clocks: MISO low=%u high=%u\n", low, high);
	}

	/* 5. the same, on the two neighbouring lines, as a control */
	printf("psyche_touch: chipcheck controls: MOSI(pull-up)=%u CLK(pull-up)=%u\n",
	    ({ gpio_configure(PIN_MOSI, 0, PULL_UP, 2, false); gpio_get(PIN_MOSI); }),
	    ({ gpio_configure(PIN_CLK, 0, PULL_UP, 2, false); gpio_get(PIN_CLK); }));

	(void)ev;
	printf("psyche_touch: chipcheck done\n");
}

/*
 * The reset, tried six ways. Everything else about this bus is proven -- the engine loops back
 * cleanly, the pads reach both levels by hand, the clock is clocked the expected number of times
 * and the supplies read the right voltages -- and the chip still never pulls the data line. The
 * one thing never varied is how the reset is timed, so it is varied here, and after each the
 * chip's own id is read.
 */
static uint16_t
read_chip_id_now(void)
{
	uint8_t d[2] = { 0, 0 };

	if (se != 0 && engine_transfer2 != NULL) {
		(void)fts_read(FTS_CMD_HW_REG_R, FTS_ADDR_DCHIP_ID, FTS_ADDR_SIZE_HW_REG, d, 2);
	} else {
		(void)fts_read(FTS_CMD_HW_REG_R, FTS_ADDR_DCHIP_ID, FTS_ADDR_SIZE_HW_REG, d, 2);
	}
	return (uint16_t)(d[1] << 8 | d[0]);
}

static void
rstseq_probe(void)
{
	static const struct {
		const char *name;
		unsigned low_ms;
		unsigned after_ms;
		bool do_reset;
	} seqs[] = {
		{ "A: low 10ms, wait 20ms  (as the phone does)", 10, 20, true },
		{ "B: low 10ms, wait 100ms", 10, 100, true },
		{ "C: low 5ms,  wait 20ms  (short pulse)", 5, 20, true },
		{ "D: low 50ms, wait 20ms  (long pulse)", 50, 20, true },
		{ "E: no reset at all", 0, 0, false },
		{ "F: low 20ms, wait 200ms", 20, 200, true },
	};
	unsigned i;

	printf("psyche_touch: rstseq ----- six ways to reset -----\n");

	/* the reset pad: output, idle high, as the phone's driver sets it up */
	gpio_output(PIN_RESET, true, PULL_NONE, 6);
	sleep_ms(5);
	printf("psyche_touch: rstseq reset pad ready: cfg=%#04x io=%#04x level=%u\n",
	    rd(GPIO_CFG(PIN_RESET)), rd(GPIO_IO(PIN_RESET)), gpio_get(PIN_RESET));

	for (i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
		unsigned wait;

		if (seqs[i].do_reset) {
			gpio_set(PIN_RESET, false);
			sleep_ms(seqs[i].low_ms);
			gpio_set(PIN_RESET, true);
		}
		sleep_ms(seqs[i].after_ms);

		/* read the id a few times, in case the first attempt lands mid-boot */
		for (wait = 0; wait < 3; wait++) {
			uint16_t id = read_chip_id_now();

			printf("psyche_touch: rstseq %-38s id=%04x  int=%u miso=%u\n",
			    seqs[i].name, id, gpio_get(PIN_INT), gpio_get(PIN_MISO));
			if (id == ((FTS_DCHIP_ID_1 << 8) | FTS_DCHIP_ID_0)) {
				printf("psyche_touch: rstseq *** CHIP ANSWERED ***\n");
			}
			sleep_ms(30);
		}
		gpio_set(PIN_RESET, true);
		sleep_ms(50);
	}
	printf("psyche_touch: rstseq done\n");
}

/*
 * The window. One run of the timing sweep read id=ffff right after a reset and zeroes after
 * that, which is the first sign in this whole session that the chip is on the bus at all: all
 * ones across eight clocks is the chip driving the line, not a floating pad. The phone reads its
 * firmware version 21 ms after starting the reset, so the window is short -- this polls it as
 * fast as it can, from the instant the reset is released.
 */
static void
fastpoll_probe(void)
{
	uint8_t d[2];
	uint32_t attempt, hits_ffff = 0, hits_real = 0, hits_zero = 0;

	printf("psyche_touch: fastpoll ----- polling the instant the reset comes off -----\n");

	gpio_output(PIN_RESET, true, PULL_NONE, 6);
	sleep_ms(5);
	gpio_set(PIN_RESET, false);
	sleep_ms(10);

	/* release and start reading at once, no settling pause */
	gpio_set(PIN_RESET, true);

	for (attempt = 0; attempt < 60; attempt++) {
		uint16_t id;

		d[0] = 0;
		d[1] = 0;
		(void)fts_read(FTS_CMD_HW_REG_R, FTS_ADDR_DCHIP_ID, FTS_ADDR_SIZE_HW_REG, d, 2);
		id = (uint16_t)(d[1] << 8 | d[0]);

		if (id == 0xffff) {
			hits_ffff++;
		} else if (id == 0x0000) {
			hits_zero++;
		} else {
			hits_real++;
		}

		if (attempt < 30 || id != 0) {
			printf("psyche_touch: fastpoll %2u: id=%04x (%02x %02x) int=%u miso=%u\n",
			    attempt, id, d[1], d[0], gpio_get(PIN_INT), gpio_get(PIN_MISO));
		}
	}

	printf("psyche_touch: fastpoll ----- summary -----\n");
	printf("psyche_touch: fastpoll id=ffff: %u   id=0000: %u   other: %u\n",
	    hits_ffff, hits_zero, hits_real);
	if (hits_real > 0) {
		printf("psyche_touch: fastpoll *** a real answer was seen ***\n");
	}

	/* and the same with the bit banger, which has finer control of the clock */
	printf("psyche_touch: fastpoll ----- now through the bit banger -----\n");
	engine_transfer2 = NULL;
	spi_transfer = bitbang_transfer;
	bitbang_in = PIN_MISO;
	bitbang_out = PIN_MOSI;
	bitbang_init();
	bitbang_delay_ns = 0;

	hits_ffff = hits_real = hits_zero = 0;
	gpio_set(PIN_RESET, false);
	sleep_ms(10);
	gpio_set(PIN_RESET, true);

	for (attempt = 0; attempt < 40; attempt++) {
		uint16_t id;

		d[0] = 0;
		d[1] = 0;
		(void)fts_read(FTS_CMD_HW_REG_R, FTS_ADDR_DCHIP_ID, FTS_ADDR_SIZE_HW_REG, d, 2);
		id = (uint16_t)(d[1] << 8 | d[0]);

		if (id == 0xffff) {
			hits_ffff++;
		} else if (id == 0x0000) {
			hits_zero++;
		} else {
			hits_real++;
		}
		if (attempt < 20 || id != 0) {
			printf("psyche_touch: fastpoll bb %2u: id=%04x int=%u miso=%u\n",
			    attempt, id, gpio_get(PIN_INT), gpio_get(PIN_MISO));
		}
	}
	printf("psyche_touch: fastpoll bb: ffff=%u 0000=%u other=%u\n",
	    hits_ffff, hits_zero, hits_real);
	printf("psyche_touch: fastpoll done\n");
}

/*
 * Whether the data line ever carries a mix of levels while the bus is being clocked. A value of
 * all ones is a line held high and all zeroes is a line held low; only a mix means the chip is
 * actually shifting something out. This counts high and low samples inside a real read, over
 * many attempts, which is the difference between "the line is stuck" and "the chip answers".
 */
static void
misostat_probe(void)
{
	unsigned attempt;

	printf("psyche_touch: misostat ----- 20 resets, sampling the data line inside each read -----\n");

	bitbang_in = PIN_MISO;
	bitbang_out = PIN_MOSI;
	bitbang_init();
	bitbang_delay_ns = 0;
	engine_transfer2 = NULL;
	spi_transfer = bitbang_transfer;
	(void)attempt;

	{
		unsigned tries;
		unsigned all_zero = 0, all_one = 0, mixed = 0;
		unsigned total_high = 0, total_low = 0;

		for (tries = 0; tries < 20; tries++) {
			unsigned k, high = 0, low = 0;
			uint8_t cmd = FTS_CMD_HW_REG_R;
			uint32_t addr = FTS_ADDR_DCHIP_ID;
			uint8_t id_lo = 0, id_hi = 0;

			/* reset the chip */
			gpio_set(PIN_RESET, false);
			sleep_ms(10);
			gpio_set(PIN_RESET, true);
			sleep_ms(2);

			/*
			 * Read the chip id by hand so every bit can be inspected: the command byte with its
			 * four address bytes out, then two bytes back.
			 */
			gpio_set(PIN_CS, false);
			{
				uint8_t tx[5] = { cmd, (uint8_t)(addr >> 24), (uint8_t)(addr >> 16),
				                  (uint8_t)(addr >> 8), (uint8_t)addr };
				int bit;

				for (k = 0; k < 5; k++) {
					for (bit = 7; bit >= 0; bit--) {
						gpio_set(PIN_MOSI, tx[k] >> bit & 1);
						gpio_set(PIN_CLK, true);
						delay_for_interval(1, NSEC_PER_USEC);
						gpio_set(PIN_CLK, false);
						delay_for_interval(1, NSEC_PER_USEC);
					}
				}
				/* the dummy byte */
				for (bit = 7; bit >= 0; bit--) {
					gpio_set(PIN_CLK, true);
					delay_for_interval(1, NSEC_PER_USEC);
					gpio_set(PIN_CLK, false);
					delay_for_interval(1, NSEC_PER_USEC);
				}
				/* two answer bytes */
				{
					uint8_t v[2] = { 0, 0 };
					unsigned w;

					for (w = 0; w < 2; w++) {
						int bit;

						for (bit = 7; bit >= 0; bit--) {
							gpio_set(PIN_CLK, true);
							delay_for_interval(1, NSEC_PER_USEC);
							if (gpio_get(PIN_MISO)) {
								high++;
								total_high++;
								v[w] = (uint8_t)(v[w] << 1 | 1);
							} else {
								low++;
								total_low++;
							}
							gpio_set(PIN_CLK, false);
							delay_for_interval(1, NSEC_PER_USEC);
						}
					}
					id_lo = v[0];
					id_hi = v[1];
				}
			}
			gpio_set(PIN_CS, true);

			if (high == 0) {
				all_zero++;
			} else if (low == 0) {
				all_one++;
			} else {
				mixed++;
			}
			printf("psyche_touch: misostat try %2u: id=%02x%02x  high=%u low=%u  (%s)\n",
			    tries, id_hi, id_lo, high, low,
			    high == 0 ? "all low" : low == 0 ? "all high" : "MIXED");
			sleep_ms(20);
		}
		printf("psyche_touch: misostat ----- summary -----\n");
		printf("psyche_touch: misostat all-low=%u all-high=%u MIXED=%u  (total high=%u low=%u)\n",
		    all_zero, all_one, mixed, total_high, total_low);
		if (mixed > 0) {
			printf("psyche_touch: misostat *** the line carried real data ***\n");
		}
	}
	printf("psyche_touch: misostat done\n");
}

/*
 * What pulls the data line low while the clock runs. With a pull-up and nothing else happening
 * the line reads high; inside a clocked transfer it reads low for every sample. The difference
 * is the clock itself, so each signal is exercised on its own to see which one takes the line
 * down.
 */
static void
crosstalk_probe(void)
{
	unsigned i;

	printf("psyche_touch: crosstalk ----- which signal pulls the data line low? -----\n");

	bitbang_in = PIN_MISO;
	bitbang_out = PIN_MOSI;
	bitbang_init();
	bitbang_delay_ns = 0;

	/* the data line with a pull-up, nothing happening */
	gpio_configure(PIN_MISO, 0, PULL_UP, 2, false);
	sleep_us(50);
	printf("psyche_touch: crosstalk 0. idle with pull-up            : MISO=%u\n", gpio_get(PIN_MISO));

	/* select low only */
	gpio_output(PIN_CS, false, PULL_NONE, 6);
	sleep_us(50);
	printf("psyche_touch: crosstalk 1. select low only                : MISO=%u\n", gpio_get(PIN_MISO));

	/* clock toggling, select still low, data line untouched */
	{
		unsigned high = 0;

		for (i = 0; i < 32; i++) {
			gpio_set(PIN_CLK, true);
			delay_for_interval(1, NSEC_PER_USEC);
			if (gpio_get(PIN_MISO)) {
				high++;
			}
			gpio_set(PIN_CLK, false);
			delay_for_interval(1, NSEC_PER_USEC);
			if (gpio_get(PIN_MISO)) {
				high++;
			}
		}
		printf("psyche_touch: crosstalk 2. clocking with select low     : MISO high=%u/64\n", high);
	}

	/* select high again, clock still toggling */
	gpio_set(PIN_CS, true);
	{
		unsigned high = 0;

		for (i = 0; i < 32; i++) {
			gpio_set(PIN_CLK, true);
			delay_for_interval(1, NSEC_PER_USEC);
			if (gpio_get(PIN_MISO)) {
				high++;
			}
			gpio_set(PIN_CLK, false);
			delay_for_interval(1, NSEC_PER_USEC);
			if (gpio_get(PIN_MISO)) {
				high++;
			}
		}
		printf("psyche_touch: crosstalk 3. clocking with select high    : MISO high=%u/64\n", high);
	}

	/* data out line driven both ways, clock still */
	gpio_output(PIN_MOSI, true, PULL_NONE, 6);
	sleep_us(50);
	printf("psyche_touch: crosstalk 4. MOSI driven high              : MISO=%u\n", gpio_get(PIN_MISO));
	gpio_output(PIN_MOSI, false, PULL_NONE, 6);
	sleep_us(50);
	printf("psyche_touch: crosstalk 5. MOSI driven low               : MISO=%u\n", gpio_get(PIN_MISO));

	/* everything idle again, data line with a pull-up */
	gpio_output(PIN_CS, true, PULL_NONE, 6);
	gpio_configure(PIN_MISO, 0, PULL_UP, 2, false);
	sleep_us(50);
	printf("psyche_touch: crosstalk 6. all idle, pull-up again       : MISO=%u\n", gpio_get(PIN_MISO));

	/* and with no pull at all, driven nothing */
	gpio_configure(PIN_MISO, 0, PULL_NONE, 2, false);
	sleep_us(50);
	printf("psyche_touch: crosstalk 7. all idle, no pull              : MISO=%u\n", gpio_get(PIN_MISO));

	/* the pad's own register, for the record */
	printf("psyche_touch: crosstalk MISO pad: cfg=%#04x io=%#04x\n",
	    rd(GPIO_CFG(PIN_MISO)), rd(GPIO_IO(PIN_MISO)));
	printf("psyche_touch: crosstalk done\n");
}

/*
 * Three wires or four. The data line has read high in one test and low in another, and it never
 * carries a mix inside a transfer -- which is what a four wire bus with an answering chip looks
 * like versus a three wire bus where the same pad is both output and input. Talking to the chip
 * while watching both data pads says which one actually answers.
 */
static void
threewire_probe(void)
{
	unsigned i;
	uint8_t cmd = FTS_FIFO_CMD_READALL;

	printf("psyche_touch: 3wire ----- which pad carries the answer? -----\n");

	/* both data pads as inputs with a pull-up, clock and select as outputs */
	gpio_output(PIN_CS, true, PULL_NONE, 6);
	gpio_output(PIN_CLK, false, PULL_NONE, 6);
	gpio_configure(PIN_MISO, 0, PULL_UP, 2, false);    /* gpio 8 */
	gpio_configure(PIN_MOSI, 0, PULL_UP, 2, false);    /* gpio 9 */
	sleep_us(50);

	printf("psyche_touch: 3wire idle: gpio8=%u gpio9=%u (both pulled up)\n",
	    gpio_get(PIN_MISO), gpio_get(PIN_MOSI));

	/*
	 * Drive the command out on gpio 8 and watch gpio 9, then the other way round: on a three
	 * wire bus one pad does both jobs and the other is silent.
	 */
	for (int pass = 0; pass < 2; pass++) {
		unsigned out_pin = pass == 0 ? PIN_MISO : PIN_MOSI;   /* the pad used to send */
		unsigned in_pin  = pass == 0 ? PIN_MOSI : PIN_MISO;   /* the pad watched for an answer */
		unsigned high = 0, low = 0;

		printf("psyche_touch: 3wire pass %d: sending on gpio%u, watching gpio%u\n",
		    pass, out_pin, in_pin);

		/* chip reset */
		gpio_output(PIN_RESET, false, PULL_NONE, 6);
		sleep_ms(10);
		gpio_output(PIN_RESET, true, PULL_NONE, 6);
		sleep_ms(5);

		/* the data pad becomes an output */
		gpio_output(out_pin, false, PULL_NONE, 6);
		/* the pad we watch stays an input with a pull-up */
		wr(GPIO_IO(in_pin), 0);
		wr(GPIO_CFG(in_pin), 0x3u);
		sleep_us(20);

		gpio_set(PIN_CS, false);
		/* command byte out */
		for (i = 0; i < 8; i++) {
			gpio_set(out_pin, cmd >> (7 - i) & 1);
			gpio_set(PIN_CLK, true);
			delay_for_interval(1, NSEC_PER_USEC);
			gpio_set(PIN_CLK, false);
			delay_for_interval(1, NSEC_PER_USEC);
		}
		/* answer: watch the other pad for 9 bytes of clocks */
		for (i = 0; i < 9 * 8; i++) {
			gpio_set(PIN_CLK, true);
			delay_for_interval(1, NSEC_PER_USEC);
			if (gpio_get(in_pin)) {
				high++;
			} else {
				low++;
			}
			gpio_set(PIN_CLK, false);
			delay_for_interval(1, NSEC_PER_USEC);
		}
		gpio_set(PIN_CS, true);

		printf("psyche_touch: 3wire pass %d: watched pad high=%u low=%u  %s\n",
		    pass, high, low,
		    low > 0 && high > 0 ? "*** MIXED - this pad answers ***"
		    : high > 0 ? "all high" : "all low");
	}

	/* and the register states, for the record */
	printf("psyche_touch: 3wire pads: gpio8 cfg=%#04x io=%#04x  gpio9 cfg=%#04x io=%#04x\n",
	    rd(GPIO_CFG(PIN_MISO)), rd(GPIO_IO(PIN_MISO)),
	    rd(GPIO_CFG(PIN_MOSI)), rd(GPIO_IO(PIN_MOSI)));
	printf("psyche_touch: 3wire done\n");
}

/*
 * The data line with a pull-up, while the chip is being read. Nineteen runs have shown the bus
 * itself is sound -- the engine loops back, the pads reach both levels, the clock toggles the
 * expected number of times -- while the line never carries a mix. A data line that only ever
 * reads low is what an open-drain output looks like with nothing pulling it up: the chip can
 * pull it low, but it cannot drive it high, and so every bit reads zero. A pull-up on that line
 * is the one thing that would let a one arrive.
 */
static void
misopull_probe(void)
{
	unsigned attempt;
	unsigned got_real = 0;

	printf("psyche_touch: misopull ----- reading the chip with a pull-up on the data line -----\n");

	/* the engine owns the pins, so configure the data pad through the engine's mux */
	use_engine_pins();

	/*
	 * The engine sets the mux, but a pad's pull is separate from its function: writing the pull
	 * bits while leaving the function alone gives the line a pull-up without taking it off the
	 * serial engine.
	 */
	{
		uint32_t cfg = rd(GPIO_CFG(PIN_MISO));

		wr(GPIO_CFG(PIN_MISO), (cfg & ~0x3u) | 0x3u);
		printf("psyche_touch: misopull data pad cfg %#04x -> %#04x (pull-up, function %u)\n",
		    cfg, rd(GPIO_CFG(PIN_MISO)), (rd(GPIO_CFG(PIN_MISO)) >> 2) & 7);
	}

	/* reset and read, with the pull-up in place */
	gpio_output(PIN_RESET, false, PULL_NONE, 6);
	sleep_ms(10);
	gpio_output(PIN_RESET, true, PULL_NONE, 6);
	sleep_ms(20);

	for (attempt = 0; attempt < 8; attempt++) {
		uint8_t d[2] = { 0, 0 };
		uint16_t id;

		(void)fts_read(FTS_CMD_HW_REG_R, FTS_ADDR_DCHIP_ID, FTS_ADDR_SIZE_HW_REG, d, 2);
		id = (uint16_t)(d[1] << 8 | d[0]);
		printf("psyche_touch: misopull %u: chip id %04x (%02x %02x)  MISO pad cfg=%#04x io=%#04x\n",
		    attempt, id, d[1], d[0],
		    rd(GPIO_CFG(PIN_MISO)), rd(GPIO_IO(PIN_MISO)));
		if (id != 0 && id != 0xffff) {
			got_real++;
		}
		if (id == ((FTS_DCHIP_ID_1 << 8) | FTS_DCHIP_ID_0)) {
			printf("psyche_touch: misopull *** CHIP ID MATCHES 0x3936 ***\n");
		}
		sleep_ms(20);
	}

	/* the same read with the pull-down instead, as a control */
	printf("psyche_touch: misopull ----- control: pull-down -----\n");
	{
		uint32_t cfg = rd(GPIO_CFG(PIN_MISO));

		wr(GPIO_CFG(PIN_MISO), (cfg & ~0x3u) | 0x1u);
	}
	{
		uint8_t d[2] = { 0, 0 };

		(void)fts_read(FTS_CMD_HW_REG_R, FTS_ADDR_DCHIP_ID, FTS_ADDR_SIZE_HW_REG, d, 2);
		printf("psyche_touch: misopull pull-down: chip id %02x%02x\n", d[1], d[0]);
	}

	printf("psyche_touch: misopull real answers seen: %u\n", got_real);
	printf("psyche_touch: misopull done\n");
}

/*
 * Everything the phone does between the chip being cold and it answering, in its own order, with
 * every step checked against the hardware instead of assumed:
 *
 *   read the two rails as they are  ->  vdd 1.8 V  ->  15 ms  ->  avdd 3.3 V  ->  20 ms
 *   ->  touch_vddio_vreg (gpio 69) high  ->  15 ms  ->  reset low 10 ms, high  ->  poll ready
 *   ->  chip id
 *
 * Each rail step is acknowledged by CMD_STATUS_COMPL, and each is read back through the sequencer
 * afterwards, so "the vote worked" is a measurement rather than a guess.
 */
static void
powerseq_probe(void)
{
	const uint32_t VDD_ADDR = 0x41f00;     /* ldoa7  = pm8150_l7  = vdd,  1.8 V */
	const uint32_t AVDD_ADDR = 0x41200;    /* ldoc10 = pm8150a_l10 = avdd, 3.0-3.3 V */
	uint8_t ev[FTS_FIFO_EVENT_SIZE];
	uint8_t d[2] = { 0, 0 };
	uint32_t v = 0;
	uint16_t id;
	bool ok;
	int i;

	printf("psyche_touch: powerseq ----- the rails as this kernel finds them -----\n");
	ok = rpmh_read(VDD_ADDR + VRM_ENABLE, &v);
	printf("psyche_touch: powerseq vdd  ldoa7  0x%05x enable read -> %s %u\n",
	    VDD_ADDR, ok ? "ack" : "NO ACK", v);
	ok = rpmh_read(VDD_ADDR + VRM_VOLTAGE, &v);
	printf("psyche_touch: powerseq vdd  ldoa7  0x%05x voltage read -> %s %u mV\n",
	    VDD_ADDR, ok ? "ack" : "NO ACK", v);
	ok = rpmh_read(AVDD_ADDR + VRM_ENABLE, &v);
	printf("psyche_touch: powerseq avdd ldoc10 0x%05x enable read -> %s %u\n",
	    AVDD_ADDR, ok ? "ack" : "NO ACK", v);
	ok = rpmh_read(AVDD_ADDR + VRM_VOLTAGE, &v);
	printf("psyche_touch: powerseq avdd ldoc10 0x%05x voltage read -> %s %u mV\n",
	    AVDD_ADDR, ok ? "ack" : "NO ACK", v);

	printf("psyche_touch: powerseq ----- the phone's order -----\n");

	/* fts_enable_reg() fts.c:5201: vdd first, 15 ms, then avdd. */
	ok = rpmh_write(MSGID_WRITE_4_BYTES, VDD_ADDR + VRM_VOLTAGE, 1800);
	printf("psyche_touch: powerseq vdd  1.8 V set   -> %s\n", ok ? "ack" : "NO ACK");
	ok = rpmh_write(MSGID_WRITE_4_BYTES, VDD_ADDR + VRM_ENABLE, 1);
	printf("psyche_touch: powerseq vdd  enable      -> %s\n", ok ? "ack" : "NO ACK");
	sleep_ms(15);

	ok = rpmh_write(MSGID_WRITE_4_BYTES, AVDD_ADDR + VRM_VOLTAGE, powerseq_avdd_mv);
	printf("psyche_touch: powerseq avdd %u mV set  -> %s\n", powerseq_avdd_mv,
	    ok ? "ack" : "NO ACK");
	ok = rpmh_write(MSGID_WRITE_4_BYTES, AVDD_ADDR + VRM_ENABLE, 1);
	printf("psyche_touch: powerseq avdd enable      -> %s\n", ok ? "ack" : "NO ACK");
	sleep_ms(20);

	ok = rpmh_read(VDD_ADDR + VRM_ENABLE, &v);
	printf("psyche_touch: powerseq vdd  enable now  -> %s %u\n", ok ? "ack" : "NO ACK", v);
	ok = rpmh_read(AVDD_ADDR + VRM_ENABLE, &v);
	printf("psyche_touch: powerseq avdd enable now  -> %s %u\n", ok ? "ack" : "NO ACK", v);
	ok = rpmh_read(AVDD_ADDR + VRM_VOLTAGE, &v);
	printf("psyche_touch: powerseq avdd voltage now -> %s %u mV\n", ok ? "ack" : "NO ACK", v);

	/*
	 * xiaomi-sm8250-common.dtsi:103-110: touch_vddio_vreg, regulator-fixed, enable-active-high,
	 * gpio = <&tlmm 69 0>. The phone brings it up in probe; this kernel never has.
	 */
	if (powerseq_vddio) {
		gpio_set(PIN_VDDIO, true);
		gpio_configure(PIN_VDDIO, 0, PULL_NONE, 8, true);
		printf("psyche_touch: powerseq vddio gpio%u cfg=%#x io=%#x\n",
		    PIN_VDDIO, rd(GPIO_CFG(PIN_VDDIO)), rd(GPIO_IO(PIN_VDDIO)));
		sleep_ms(15);
	} else {
		printf("psyche_touch: powerseq vddio gpio%u left as found (cfg=%#x io=%#x)\n",
		    PIN_VDDIO, rd(GPIO_CFG(PIN_VDDIO)), rd(GPIO_IO(PIN_VDDIO)));
	}

	/* reset, the way fts_system_reset() does it when a reset pin exists (ftsCore.c:85) */
	wr(GPIO_IO(PIN_RESET), 0);
	gpio_configure(PIN_RESET, 0, PULL_NONE, 8, true);
	sleep_ms(10);
	wr(GPIO_IO(PIN_RESET), 2);
	gpio_configure(PIN_RESET, 0, PULL_NONE, 8, true);
	printf("psyche_touch: powerseq reset released, irq=%u\n", gpio_get(PIN_INT));

	for (i = 0; i < 3; i++) {
		uint32_t n = 0;

		sleep_ms(100);
		if (fts_read_fifo(ev, &n)) {
			printf("psyche_touch: powerseq fifo 0: %02x %02x %02x %02x %02x %02x %02x %02x\n",
			    ev[0], ev[1], ev[2], ev[3], ev[4], ev[5], ev[6], ev[7]);
			if (ev[0] == FTS_EVT_CONTROLLER_READY) {
				printf("psyche_touch: powerseq *** CONTROLLER READY ***\n");
				break;
			}
		}
	}

	for (i = 0; i < 4; i++) {
		if (fts_read(FTS_CMD_HW_REG_R, FTS_ADDR_DCHIP_ID, FTS_ADDR_SIZE_HW_REG, d, 2)) {
			id = (uint16_t)(d[1] << 8 | d[0]);
			printf("psyche_touch: powerseq read %u: chip id %04x (%02x %02x) ios=%#010x\n",
			    i, id, d[1], d[0], rd(se + 0x908));
			if (id == ((FTS_DCHIP_ID_1 << 8) | FTS_DCHIP_ID_0)) {
				printf("psyche_touch: powerseq *** CHIP ID MATCHES ***\n");
			}
		}
		sleep_ms(20);
	}

	/*
	 * Does the data line carry an answer, or is the pattern an artefact of how this engine
	 * samples a line that floats? A real chip's bits do not move when only the clock changes;
	 * sampling noise does. The FIFO read is the transfer that started returning bytes, so it is
	 * the one to repeat: same command, four different clock rates, and the raw bytes printed.
	 */
	printf("psyche_touch: powerseq ----- is the answer real, or sampling noise? -----\n");
	{
		static const uint32_t hzs[] = { 1000000u, 2000000u, 4000000u, 8000000u };
		for (unsigned k = 0; k < sizeof(hzs) / sizeof(hzs[0]); k++) {
			uint8_t f[FTS_FIFO_EVENT_SIZE];
			int distinct = 0;
			uint8_t first[FTS_FIFO_EVENT_SIZE];

			(void)geni_spi_init(hzs[k]);
			use_engine_pins();
			ok = fts_read(FTS_FIFO_CMD_READALL, 0, 0, f, FTS_FIFO_EVENT_SIZE);
			if (!ok) {
				printf("psyche_touch: powerseq clk %u Hz: read failed\n", hzs[k]);
				continue;
			}
			memcpy(first, f, FTS_FIFO_EVENT_SIZE);
			printf("psyche_touch: powerseq clk %u Hz: %02x %02x %02x %02x %02x %02x %02x %02x\n",
			    hzs[k], first[0], first[1], first[2], first[3],
			    first[4], first[5], first[6], first[7]);
			/* how many of the next eight reads say the same thing */
			for (unsigned r = 0; r < 8; r++) {
				uint8_t g[FTS_FIFO_EVENT_SIZE];

				if (fts_read(FTS_FIFO_CMD_READALL, 0, 0, g, FTS_FIFO_EVENT_SIZE) &&
				    memcmp(g, first, FTS_FIFO_EVENT_SIZE) == 0) {
					distinct++;
				}
				sleep_ms(10);
			}
			printf("psyche_touch: powerseq clk %u Hz: %d of 8 repeats identical\n",
			    hzs[k], distinct);
		}
		/* leave the engine at the speed the driver normally uses */
		(void)geni_spi_init(target_spi_hz);
		use_engine_pins();
	}
	printf("psyche_touch: powerseq done\n");
}

/*
 * The one thing the phone does that this kernel has never done: it sets the supply's voltage,
 * not just its enable bit. Its driver calls regulator_set_voltage(avdd, 3.3 V) between enabling
 * vdd and enabling avdd (fts.c:6823-6824). Only the enable bit has ever been voted here, so the
 * rail may be powered at whatever the boot loader left rather than at what the chip needs.
 */
static void
setvolt_probe(void)
{
	static const struct { uint32_t addr; uint32_t volts; const char *name; } rails[] = {
		{ 0x41200, 3300, "avdd ldoc10 3.3 V" },
		{ 0x41200, 3000, "avdd ldoc10 3.0 V (the DT's init value)" },
		{ 0x41f00, 1800, "vdd ldoa7 1.8 V" },
	};

	unsigned i;

	printf("psyche_touch: setvolt ----- voltages, the way the phone sets them -----\n");
	if (rsc == 0) {
		printf("psyche_touch: setvolt no RPMh mapped\n");
		return;
	}

	for (i = 0; i < sizeof(rails) / sizeof(rails[0]); i++) {
		bool ok;

		/* VRM_ENABLE first, then the voltage level */
		ok = rpmh_write(MSGID_WRITE_4_BYTES, rails[i].addr + 4, 1);
		printf("psyche_touch: setvolt %s: enable -> %s\n",
		    rails[i].name, ok ? "ok" : "failed");

		ok = rpmh_write(MSGID_WRITE_4_BYTES, rails[i].addr + VRM_VOLTAGE, rails[i].volts);
		printf("psyche_touch: setvolt %s: set voltage %u mV -> %s\n",
		    rails[i].name, rails[i].volts, ok ? "ok" : "failed");
		sleep_ms(5);
	}

	/* now reset the chip and read it */
	printf("psyche_touch: setvolt ----- reset and read -----\n");
	gpio_output(PIN_RESET, false, PULL_NONE, 6);
	sleep_ms(10);
	gpio_output(PIN_RESET, true, PULL_NONE, 6);
	sleep_ms(20);

	for (i = 0; i < 5; i++) {
		uint8_t d[2] = { 0, 0 };
		uint16_t id;

		(void)fts_read(FTS_CMD_HW_REG_R, FTS_ADDR_DCHIP_ID, FTS_ADDR_SIZE_HW_REG, d, 2);
		id = (uint16_t)(d[1] << 8 | d[0]);
		printf("psyche_touch: setvolt read %u: chip id %04x (%02x %02x) int=%u miso=%u\n",
		    i, id, d[1], d[0], gpio_get(PIN_INT), gpio_get(PIN_MISO));
		if (id == ((FTS_DCHIP_ID_1 << 8) | FTS_DCHIP_ID_0)) {
			printf("psyche_touch: setvolt *** CHIP ID MATCHES ***\n");
		}
		sleep_ms(20);
	}
	printf("psyche_touch: setvolt done\n");
}

/*
 * The serial engine, looked at before this driver touches it. The same hardware block is both
 * an I2C and an SPI controller on this phone (i2c@990000 and spi@990000 in the device tree are
 * the same address), and this driver loads eighteenth -- several drivers ahead of it could have
 * reconfigured the block. Reading it first says what state it arrived in.
 */
static void
sestate_probe(void)
{
	printf("psyche_touch: sestate ----- SE4 as this driver finds it -----\n");

	se = ml_io_map(SE_PHYS, SE_SIZE);
	if (!se) {
		printf("psyche_touch: sestate cannot map the engine\n");
		return;
	}

	printf("psyche_touch: sestate GENI_FW_REVISION_RO = %#x  (protocol = %u, want 1 for SPI)\n",
	    rd(GENI_FW_REVISION_RO), rd(GENI_FW_REVISION_RO) >> 8 & 0xff);
	printf("psyche_touch: sestate GENI_IF_DISABLE_RO  = %#x\n", rd(GENI_IF_DISABLE_RO));
	printf("psyche_touch: sestate SE_HW_PARAM_0       = %#x  (fifo depth = %u)\n",
	    rd(SE_HW_PARAM_0), rd(SE_HW_PARAM_0) >> 16 & 0x3f);

	printf("psyche_touch: sestate GENI_STATUS         = %#x\n", rd(GENI_STATUS));
	printf("psyche_touch: sestate GENI_CLK_SEL        = %#x\n", rd(GENI_CLK_SEL));
	printf("psyche_touch: sestate GENI_SER_M_CLK_CFG  = %#x\n", rd(GENI_SER_M_CLK_CFG));
	printf("psyche_touch: sestate GENI_FORCE_DEFAULT  = %#x\n", rd(GENI_FORCE_DEFAULT));
	printf("psyche_touch: sestate GENI_OUTPUT_CTRL    = %#x\n", rd(GENI_OUTPUT_CTRL));
	printf("psyche_touch: sestate GENI_CGC_CTRL       = %#x\n", rd(GENI_CGC_CTRL));
	printf("psyche_touch: sestate SPI_TRANS_CFG       = %#x\n", rd(SPI_TRANS_CFG));
	printf("psyche_touch: sestate SPI_CPHA            = %#x\n", rd(SPI_CPHA));
	printf("psyche_touch: sestate SPI_CPOL            = %#x\n", rd(SPI_CPOL));
	printf("psyche_touch: sestate SPI_DEMUX_SEL       = %#x\n", rd(SPI_DEMUX_SEL));
	printf("psyche_touch: sestate SPI_WORD_LEN        = %#x\n", rd(SPI_WORD_LEN));
	printf("psyche_touch: sestate SPI_LOOPBACK        = %#x\n", rd(SPI_LOOPBACK));
	printf("psyche_touch: sestate SPI_DELAY_COUNTERS  = %#x\n", rd(SPI_DELAY_COUNTERS));
	printf("psyche_touch: sestate GENI_TX_PACKING_CFG0= %#x\n", rd(GENI_TX_PACKING_CFG0));
	printf("psyche_touch: sestate GENI_TX_PACKING_CFG1= %#x\n", rd(GENI_TX_PACKING_CFG1));
	printf("psyche_touch: sestate GENI_RX_PACKING_CFG0= %#x\n", rd(GENI_RX_PACKING_CFG0));
	printf("psyche_touch: sestate GENI_RX_PACKING_CFG1= %#x\n", rd(GENI_RX_PACKING_CFG1));
	printf("psyche_touch: sestate GENI_BYTE_GRAN      = %#x\n", rd(GENI_BYTE_GRAN));
	printf("psyche_touch: sestate GENI_M_CMD0         = %#x\n", rd(GENI_M_CMD0));
	printf("psyche_touch: sestate GENI_M_CMD_CTRL     = %#x\n", rd(GENI_M_CMD_CTRL));
	printf("psyche_touch: sestate GENI_M_IRQ_STATUS   = %#x\n", rd(GENI_M_IRQ_STATUS));
	printf("psyche_touch: sestate GENI_M_IRQ_EN       = %#x\n", rd(GENI_M_IRQ_EN));
	printf("psyche_touch: sestate SE_IRQ_EN           = %#x\n", rd(SE_IRQ_EN));

	/* and the four pads, before this driver configures them */
	printf("psyche_touch: sestate pads as found: 8=%#04x 9=%#04x 10=%#04x 11=%#04x 33=%#04x 39=%#04x\n",
	    rd(GPIO_CFG(PIN_MISO)), rd(GPIO_CFG(PIN_MOSI)), rd(GPIO_CFG(PIN_CLK)),
	    rd(GPIO_CFG(PIN_CS)), rd(GPIO_CFG(PIN_RESET)), rd(GPIO_CFG(PIN_INT)));
	printf("psyche_touch: sestate done\n");
}

/*
 * Using the serial engine as the boot loader left it. Every register of SE4 is already set when
 * this driver loads -- clock select 5, a divider programmed, word length 4, FIFO packing filled
 * in, and the four pads already muxed to qup4 -- so the engine has been initialised by something
 * that runs before this driver. Re-initialising it can only fight with that; sending a command
 * through it as it stands is the other way round.
 */
static void
useexisting_probe(void)
{
	uint8_t ev[FTS_FIFO_EVENT_SIZE];
	uint32_t i;

	printf("psyche_touch: useexisting ----- the engine as found, no re-init -----\n");

	se = ml_io_map(SE_PHYS, SE_SIZE);
	if (!se) {
		printf("psyche_touch: useexisting no engine\n");
		return;
	}
	printf("psyche_touch: useexisting fw_rev=%#x if_disable=%#x fifo_depth=%u\n",
	    rd(GENI_FW_REVISION_RO), rd(GENI_IF_DISABLE_RO), rd(SE_HW_PARAM_0) >> 16 & 0x3f);
	printf("psyche_touch: useexisting CLK_SEL=%#x SER_M_CLK_CFG=%#x WORD_LEN=%#x\n",
	    rd(GENI_CLK_SEL), rd(GENI_SER_M_CLK_CFG), rd(SPI_WORD_LEN));

	fifo_depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;

	/* do not re-init; just hand the transfer routine over and talk */
	use_engine_pins();
	engine_transfer2 = geni_transfer2;

	/* reset the chip, then read it with the engine exactly as it stands */
	gpio_output(PIN_RESET, false, PULL_NONE, 6);
	sleep_ms(10);
	gpio_output(PIN_RESET, true, PULL_NONE, 6);
	sleep_ms(20);

	for (i = 0; i < 6; i++) {
		uint8_t d[2] = { 0, 0 };
		uint16_t id;

		(void)fts_read(FTS_CMD_HW_REG_R, FTS_ADDR_DCHIP_ID, FTS_ADDR_SIZE_HW_REG, d, 2);
		id = (uint16_t)(d[1] << 8 | d[0]);
		printf("psyche_touch: useexisting read %u: id=%04x (%02x %02x)  int=%u miso=%u\n",
		    i, id, d[1], d[0], gpio_get(PIN_INT), gpio_get(PIN_MISO));
		if (id == ((FTS_DCHIP_ID_1 << 8) | FTS_DCHIP_ID_0)) {
			printf("psyche_touch: useexisting *** CHIP ID MATCHES 0x3936 ***\n");
		}
		sleep_ms(20);
	}

	/* and a FIFO read, the other command the driver uses */
	for (i = 0; i < 3; i++) {
		if (fts_read(FTS_FIFO_CMD_READALL, 0, 0, ev, FTS_FIFO_EVENT_SIZE)) {
			printf("psyche_touch: useexisting fifo %u: %02x %02x %02x %02x %02x %02x %02x %02x  int=%u\n",
			    i, ev[0], ev[1], ev[2], ev[3], ev[4], ev[5], ev[6], ev[7],
			    gpio_get(PIN_INT));
		} else {
			printf("psyche_touch: useexisting fifo %u: failed\n", i);
		}
		sleep_ms(20);
	}
	printf("psyche_touch: useexisting done\n");
}

/*
 * Leaving the pins exactly as the boot loader set them. It configured all six: the four on the
 * serial engine (0x84 = qup4), the reset as an output at 12 mA (0x280) and the interrupt as an
 * input (0xc0). This driver rewrites the last two as plain inputs, which means it drives a reset
 * line it should only be releasing, and then puts them back -- and the chip may well have been
 * reset or held exactly by the state the loader left. So: touch nothing, and just talk.
 */
static void
untouched_probe(void)
{
	uint8_t ev[FTS_FIFO_EVENT_SIZE];
	uint32_t i;

	printf("psyche_touch: untouched ----- pins left as found, engine as found -----\n");

	se = ml_io_map(SE_PHYS, SE_SIZE);
	if (!se) {
		printf("psyche_touch: untouched no engine\n");
		return;
	}
	fifo_depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;

	printf("psyche_touch: untouched before: 8=%#04x 9=%#04x 10=%#04x 11=%#04x 33=%#04x 39=%#04x\n",
	    rd(GPIO_CFG(PIN_MISO)), rd(GPIO_CFG(PIN_MOSI)), rd(GPIO_CFG(PIN_CLK)),
	    rd(GPIO_CFG(PIN_CS)), rd(GPIO_CFG(PIN_RESET)), rd(GPIO_CFG(PIN_INT)));
	printf("psyche_touch: untouched levels: int39=%u rst33=%u cs11=%u miso8=%u\n",
	    gpio_get(PIN_INT), gpio_get(PIN_RESET), gpio_get(PIN_CS), gpio_get(PIN_MISO));

	/* the transfer routine, and nothing else */
	engine_transfer2 = geni_transfer2;

	/* read the chip as it stands -- no reset, no re-muxing */
	for (i = 0; i < 6; i++) {
		uint8_t d[2] = { 0, 0 };
		uint16_t id;

		(void)fts_read(FTS_CMD_HW_REG_R, FTS_ADDR_DCHIP_ID, FTS_ADDR_SIZE_HW_REG, d, 2);
		id = (uint16_t)(d[1] << 8 | d[0]);
		printf("psyche_touch: untouched read %u: id=%04x (%02x %02x)  int=%u miso=%u\n",
		    i, id, d[1], d[0], gpio_get(PIN_INT), gpio_get(PIN_MISO));
		if (id == ((FTS_DCHIP_ID_1 << 8) | FTS_DCHIP_ID_0)) {
			printf("psyche_touch: untouched *** CHIP ID MATCHES 0x3936 ***\n");
		}
		sleep_ms(20);
	}

	/* a FIFO read too, which is what the driver starts with */
	for (i = 0; i < 3; i++) {
		if (fts_read(FTS_FIFO_CMD_READALL, 0, 0, ev, FTS_FIFO_EVENT_SIZE)) {
			printf("psyche_touch: untouched fifo %u: %02x %02x %02x %02x %02x %02x %02x %02x  int=%u\n",
			    i, ev[0], ev[1], ev[2], ev[3], ev[4], ev[5], ev[6], ev[7],
			    gpio_get(PIN_INT));
		} else {
			printf("psyche_touch: untouched fifo %u: failed\n", i);
		}
		sleep_ms(20);
	}

	printf("psyche_touch: untouched after: 33=%#04x 39=%#04x\n",
	    rd(GPIO_CFG(PIN_RESET)), rd(GPIO_CFG(PIN_INT)));
	printf("psyche_touch: untouched done\n");
}

/*
 * The QUP wrapper, a layer this driver has never touched. The phone's device tree puts one over
 * the whole serial engine bus:
 *
 *     qcom,qupv3_0_geni_se@9c0000   reg = <0x9c0000 0x2000>   (qupv3-geni-se)
 *
 * and every per-engine node points at it with qcom,wrapper-core. The wrapper gates the engine's
 * clocks and releases its reset, so an engine configured correctly but inside a wrapper that
 * never enabled it would look exactly like this: internal state right, pins silent.
 */
#define WRAPPER_PHYS    0x9c0000UL
#define WRAPPER_SIZE    0x2000UL
#define WRAPPER_REGS(n) (wrapper + 4 * (n))

static void
wrapper_probe(void)
{
	vm_offset_t wrapper = ml_io_map(WRAPPER_PHYS, WRAPPER_SIZE);
	unsigned i;

	printf("psyche_touch: wrapper ----- the QUP wrapper over this engine (%#lx) -----\n",
	    (unsigned long)WRAPPER_PHYS);

	if (!wrapper) {
		printf("psyche_touch: wrapper cannot map\n");
		return;
	}

	for (i = 0; i < 16; i++) {
		printf("psyche_touch: wrapper +%03x = %#010x\n", 4 * i, rd(WRAPPER_REGS(i)));
	}

	/*
	 * The wrapper's own enable registers, per the geni-se layout: the top half of the block
	 * holds the per-engine clock and reset controls. Print the region that usually carries them.
	 */
	for (i = 0x0; i < WRAPPER_SIZE / 4; i += 0x10) {
		uint32_t v = rd(WRAPPER_REGS(i));

		if (v != 0 && v != 0xffffffffu) {
			printf("psyche_touch: wrapper scan +%#05x = %#010x\n", 4 * i, v);
		}
	}

	/* and the two places qcom's own driver toggles */
	printf("psyche_touch: wrapper ----- known geni-se controls -----\n");
	printf("psyche_touch: wrapper +0x000 = %#010x\n", rd(WRAPPER_REGS(0x00 / 4)));
	printf("psyche_touch: wrapper +0x004 = %#010x\n", rd(WRAPPER_REGS(0x04 / 4)));
	printf("psyche_touch: wrapper +0x008 = %#010x\n", rd(WRAPPER_REGS(0x08 / 4)));
	printf("psyche_touch: wrapper +0x00c = %#010x\n", rd(WRAPPER_REGS(0x0c / 4)));
	printf("psyche_touch: wrapper done\n");
}

/*
 * The wrapper, read one word at a time. A single pass over it killed the kernel outright, so
 * each word is announced before it is read: the last line of the log then names the address
 * that cannot be touched. The window is only a few words wide for the same reason.
 */
static void
wrapper2_probe(void)
{
	vm_offset_t wrapper;
	unsigned i;

	printf("psyche_touch: wrapper2 ----- stepping into the wrapper -----\n");
	printf("psyche_touch: wrapper2 about to map %#lx, size %#lx\n",
	    (unsigned long)0x9c0000UL, (unsigned long)0x2000UL);

	wrapper = ml_io_map(0x9c0000UL, 0x2000UL);
	printf("psyche_touch: wrapper2 mapped at %#lx\n", (unsigned long)wrapper);
	if (!wrapper) {
		printf("psyche_touch: wrapper2 no map\n");
		return;
	}

	for (i = 0; i < 4; i++) {
		printf("psyche_touch: wrapper2 reading +%u (%#lx) ...\n", i, (unsigned long)(0x9c0000UL + 4 * i));
		printf("psyche_touch: wrapper2   +%u = %#010x\n", i, rd(wrapper + 4 * i));
	}

	printf("psyche_touch: wrapper2 first four words read fine\n");

	/* now the region qcom's driver uses for per-engine clock control */
	for (i = 0; i < 4; i++) {
		unsigned off = 0x1000 + 4 * i;

		printf("psyche_touch: wrapper2 reading +%#x ...\n", off);
		printf("psyche_touch: wrapper2   +%#x = %#010x\n", off, rd(wrapper + off));
	}
	printf("psyche_touch: wrapper2 done\n");
}

/*
 * The wrapper again, but inside a window this driver already maps. Two attempts to map the
 * wrapper on its own killed the kernel before they printed anything, and the io submap is only
 * 8 MB for the whole kernel. The wrapper sits at 0x9c0000 -- 0x30000 past the serial engine at
 * 0x990000 -- so mapping the engine's window wider covers it without adding a mapping of its
 * own.
 */
static void
wrapper3_probe(void)
{
	vm_offset_t wide;
	unsigned i;

	printf("psyche_touch: wrapper3 ----- wrapper inside the engine's window -----\n");

	/* one window from the engine up past the wrapper */
	wide = ml_io_map(SE_PHYS, 0x40000UL);
	printf("psyche_touch: wrapper3 mapped %#lx..%#lx at %#lx\n",
	    (unsigned long)SE_PHYS, (unsigned long)(SE_PHYS + 0x40000UL), (unsigned long)wide);
	if (!wide) {
		printf("psyche_touch: wrapper3 no map\n");
		return;
	}

	/* the engine's own first word, which is known to answer */
	printf("psyche_touch: wrapper3 engine +0x000 = %#010x (known to answer)\n", rd(wide));

	/* now step toward the wrapper one 4 KB page at a time, announcing each */
	for (i = 0x10000UL; i < 0x40000UL; i += 0x1000UL) {
		printf("psyche_touch: wrapper3 reading +%#lx ...\n", (unsigned long)i);
		printf("psyche_touch: wrapper3   +%#lx = %#010x\n",
		    (unsigned long)i, rd(wide + i));
	}
	printf("psyche_touch: wrapper3 done\n");
}

/*
 * The wrapper, read safely. Its first page answers -- 0x248f0001 and 0x20050000 at +0 and +4 --
 * while +0x1000 kills the kernel, so this stops short of that page and prints everything that is
 * not zero. The wrapper gates the serial engines' clocks and their reset, so the interesting
 * bits are the ones that look like enables.
 */
static void
wrapper4_probe(void)
{
	vm_offset_t wide;
	unsigned off, shown = 0;

	printf("psyche_touch: wrapper4 ----- wrapper, staying below +0x1000 -----\n");

	/*
	 * Map it through the engine's own window: from the engine at 0x990000 a quarter of a
	 * megabyte covers the wrapper at 0x9c0000 without adding another mapping to the 8 MB io
	 * submap.
	 */
	wide = ml_io_map(SE_PHYS, 0x40000UL);
	printf("psyche_touch: wrapper4 engine window at %#lx (engine=%#lx, wrapper=%#lx)\n",
	    (unsigned long)wide, (unsigned long)SE_PHYS, (unsigned long)(SE_PHYS + 0x30000UL));
	if (!wide) {
		printf("psyche_touch: wrapper4 no map\n");
		return;
	}

	/* the wrapper's first page, word by word, stopping before the page that faults */
	for (off = 0; off < 0x1000; off += 4) {
		uint32_t v = rd(wide + 0x30000UL + off);

		if (v != 0 && v != 0xffffffffu) {
			printf("psyche_touch: wrapper4 +%#05x = %#010x\n",
			    (unsigned long)(0x9c0000UL + off), v);
			if (++shown > 200) {
				printf("psyche_touch: wrapper4 (truncated)\n");
				break;
			}
		}
	}
	printf("psyche_touch: wrapper4 %u non-zero words in the first page\n", shown);

	/* and the engine's own first page, as a control */
	printf("psyche_touch: wrapper4 engine control: fw_rev=%#x if_disable=%#x\n",
	    rd(wide + 0x68), rd(wide + 0x64));
	printf("psyche_touch: wrapper4 done\n");
}

/*
 * What the serial engine itself sees on its IO pins. SE_GENI_IOS is the engine's own view of the
 * bus, taken in hardware rather than through a pad's GPIO register, and it is the closest a
 * driver can get to a scope: if the chip is driving a line, this register shows it.
 */
#define SE_GENI_IOS             0x908
#define SE_GENI_M_GP_LENGTH     0x910
#define SE_GENI_S_GP_LENGTH     0x914
#define GENI_SER_S_CLK_CFG      0x4c
#define GENI_CLK_CTRL_RO        0x60

static void
ios_probe(void)
{
	uint8_t ev[FTS_FIFO_EVENT_SIZE];
	unsigned i;

	printf("psyche_touch: ios ----- what the engine sees on its own pins -----\n");

	se = ml_io_map(SE_PHYS, SE_SIZE);
	if (!se) {
		printf("psyche_touch: ios no engine\n");
		return;
	}
	fifo_depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;

	printf("psyche_touch: ios idle: IOS=%#010x  CLK_CTRL_RO=%#010x  SER_S_CLK_CFG=%#x\n",
	    rd(se + SE_GENI_IOS), rd(se + GENI_CLK_CTRL_RO), rd(se + GENI_SER_S_CLK_CFG));

	use_engine_pins();
	engine_transfer2 = geni_transfer2;

	/*
	 * Read the chip id and sample IOS right after: the engine has clocked eight bytes in, so
	 * whatever the chip put on its data line is in this register.
	 */
	{
		uint8_t d[2] = { 0, 0 };

		(void)fts_read(FTS_CMD_HW_REG_R, FTS_ADDR_DCHIP_ID, FTS_ADDR_SIZE_HW_REG, d, 2);
		printf("psyche_touch: ios after chip-id read: id=%02x%02x  IOS=%#010x  M_GP_LENGTH=%#x\n",
		    d[1], d[0], rd(se + SE_GENI_IOS), rd(se + SE_GENI_M_GP_LENGTH));
	}

	/* and the FIFO read the driver starts with */
	for (i = 0; i < 3; i++) {
		uint8_t f[FTS_FIFO_EVENT_SIZE];

		(void)fts_read(FTS_FIFO_CMD_READALL, 0, 0, f, FTS_FIFO_EVENT_SIZE);
		printf("psyche_touch: ios fifo %u: %02x %02x %02x %02x %02x %02x %02x %02x  IOS=%#010x\n",
		    i, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], rd(se + SE_GENI_IOS));
		sleep_ms(10);
	}

	/* the engine's status and interrupt state, which say whether it saw activity */
	printf("psyche_touch: ios SE_GENI_STATUS=%#010x  M_IRQ_STATUS=%#010x  S_IRQ_STATUS=%#010x\n",
	    rd(GENI_STATUS), rd(GENI_M_IRQ_STATUS), rd(se + 0x640));
	printf("psyche_touch: ios M_CMD0=%#010x  M_CMD_CTRL=%#010x\n",
	    rd(GENI_M_CMD0), rd(GENI_M_CMD_CTRL));
	(void)ev;
	printf("psyche_touch: ios done\n");
}

static bool
probe(void)
{
	uint16_t id;
	const char *problem;
	bool on_engine = false;

	tlmm_north = ml_io_map(TILE_NORTH_PHYS, TILE_SIZE);
	tlmm_south = ml_io_map(TILE_SOUTH_PHYS, TILE_SIZE);
	tlmm_west  = ml_io_map(TILE_WEST_PHYS, TILE_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	cmddb = ml_io_map(CMD_DB_PHYS, CMD_DB_SIZE);
	if (!tlmm_north || !tlmm_south || !gcc) {
		printf("psyche_touch: cannot map its registers\n");
		return false;
	}

	if (psyche_loopback) {
		geni_loopback_probe();
		return false;
	}

	if (do_setvolt) {
		gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
		rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
		cmddb = ml_io_map(CMD_DB_PHYS, CMD_DB_SIZE);
		(void)clocks_on();
		se = ml_io_map(SE_PHYS, SE_SIZE);
		if (se) {
			const char *p2 = geni_spi_init(target_spi_hz);

			printf("psyche_touch: setvolt engine init -> %s\n", p2 ? p2 : "ok");
			use_engine_pins();
			if (p2 == NULL) {
				/* fts_read() uses the two-phase primitive, fts_write() the single-phase
				 * one: both have to be wired or a command write jumps through null. */
				engine_transfer2 = geni_transfer2;
				spi_transfer = geni_transfer_single;
			}
		}
		setvolt_probe();
		return false;
	}

	if (do_speed) {
		se = ml_io_map(SE_PHYS, SE_SIZE);
		gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
		tlmm_west = ml_io_map(TILE_WEST_PHYS, TILE_SIZE);
		tlmm_south = ml_io_map(TILE_SOUTH_PHYS, TILE_SIZE);
		tlmm_north = ml_io_map(TILE_NORTH_PHYS, TILE_SIZE);
		rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
		(void)clocks_on();
		if (rsc != 0) {
			uint32_t v = 0;

			(void)rpmh_read(0x41f00 + VRM_ENABLE, &v);
			if (!v) {
				(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41f00 + VRM_VOLTAGE, 1800);
				(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41f00 + VRM_ENABLE, 1);
				sleep_ms(15);
			}
			(void)rpmh_read(0x41200 + VRM_ENABLE, &v);
			if (!v) {
				(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41200 + VRM_VOLTAGE, 3000);
				(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41200 + VRM_ENABLE, 1);
				sleep_ms(20);
			}
		}
		gpio_set(PIN_VDDIO, true);
		gpio_configure(PIN_VDDIO, 0, PULL_NONE, 8, true);
		sleep_ms(15);
		if (se) {
			const char *p2 = geni_spi_init(1000000u);

			printf("psyche_touch: speed engine init -> %s\n", p2 ? p2 : "ok");
			use_engine_pins();
			engine_transfer2 = geni_transfer2;
			spi_transfer = geni_transfer_single;
		}
		speed_probe();
		return false;
	}

	if (do_clklevels) {
		se = ml_io_map(SE_PHYS, SE_SIZE);
		gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
		tlmm_west = ml_io_map(TILE_WEST_PHYS, TILE_SIZE);
		tlmm_south = ml_io_map(TILE_SOUTH_PHYS, TILE_SIZE);
		tlmm_north = ml_io_map(TILE_NORTH_PHYS, TILE_SIZE);
		(void)clocks_on();
		if (se) {
			const char *p2 = geni_spi_init(target_spi_hz);

			printf("psyche_touch: clklevels engine init -> %s\n", p2 ? p2 : "ok");
		}
		clklevels_probe();
		return false;
	}

	if (do_fifowatch) {
		se = ml_io_map(SE_PHYS, SE_SIZE);
		tlmm_west = ml_io_map(TILE_WEST_PHYS, TILE_SIZE);
		tlmm_south = ml_io_map(TILE_SOUTH_PHYS, TILE_SIZE);
		tlmm_north = ml_io_map(TILE_NORTH_PHYS, TILE_SIZE);
		gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
		rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
		cmddb = ml_io_map(CMD_DB_PHYS, CMD_DB_SIZE);
		(void)clocks_on();
		se = ml_io_map(SE_PHYS, SE_SIZE);
		if (se) {
			const char *p2 = geni_spi_init(1000000u);

			printf("psyche_touch: fifowatch engine init -> %s\n", p2 ? p2 : "ok");
			use_engine_pins();
			if (p2 == NULL) {
				/* fts_read() uses the two-phase primitive, fts_write() the single-phase
				 * one: both have to be wired or a command write jumps through null. */
				engine_transfer2 = geni_transfer2;
				spi_transfer = geni_transfer_single;
			}
		}
		/*
		 * If the engine would not come up, drive the pins by hand rather than leave the
		 * transfer pointers null: a null pointer here is an instruction-fetch abort at pc 0.
		 */
		if (engine_transfer2 == NULL || spi_transfer == NULL) {
			bitbang_in = PIN_MISO;
			bitbang_out = PIN_MOSI;
			bitbang_init();
			engine_transfer2 = NULL;
			printf("psyche_touch: fifowatch driving the pins by hand\n");
		}
		gpio_configure(PIN_INT, 0, PULL_NONE, 8, false);   /* input, as the DT's active state */
		gpio_output(PIN_RESET, true, PULL_NONE, 8);

		/*
		 * The supplies have to be established here, not inherited: a previous RAM session's
		 * rail votes outlive the session, so a probe that works after one run can fail on a
		 * fresh boot from Android -- which is exactly what this entry point did
		 * ("system info header 00, not a signature", four times).
		 */
		{
			uint32_t v = 0;

			(void)rpmh_read(0x41f00 + VRM_ENABLE, &v);
			if (!v) {
				(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41f00 + VRM_VOLTAGE, 1800);
				(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41f00 + VRM_ENABLE, 1);
			}
			(void)rpmh_read(0x41200 + VRM_ENABLE, &v);
			if (!v) {
				sleep_ms(15);
				(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41200 + VRM_VOLTAGE, 3000);
				(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41200 + VRM_ENABLE, 1);
			}
			gpio_set(PIN_VDDIO, true);
			gpio_configure(PIN_VDDIO, 0, PULL_NONE, 8, true);
			sleep_ms(15);
			printf("psyche_touch: fifowatch supplies established\n");
		}

		(void)fts_system_info();
		if (fts_reset()) {
			printf("psyche_touch: fifowatch controller ready\n");
		} else {
			printf("psyche_touch: fifowatch controller did not report ready\n");
		}
		if (fts_sense_on()) {
			printf("psyche_touch: fifowatch sensing started\n");
		} else {
			printf("psyche_touch: fifowatch sensing command failed\n");
		}
		fifowatch_probe();
		return false;
	}

	if (do_0xa7dump) {
		se = ml_io_map(SE_PHYS, SE_SIZE);
		tlmm_west = ml_io_map(TILE_WEST_PHYS, TILE_SIZE);
		tlmm_south = ml_io_map(TILE_SOUTH_PHYS, TILE_SIZE);
		tlmm_north = ml_io_map(TILE_NORTH_PHYS, TILE_SIZE);
		gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
		rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
		(void)clocks_on();
		if (se) {
			const char *p2 = geni_spi_init(target_spi_hz);

			printf("psyche_touch: 0xa7dump engine init -> %s\n", p2 ? p2 : "ok");
			use_engine_pins();
			engine_transfer2 = geni_transfer2;
			spi_transfer = geni_transfer_single;
		}
		probe0xa7dump_probe();
		return false;
	}

	if (do_0xa7clk) {
		uint32_t v = 0;

		tlmm_west = ml_io_map(TILE_WEST_PHYS, TILE_SIZE);
		tlmm_south = ml_io_map(TILE_SOUTH_PHYS, TILE_SIZE);
		tlmm_north = ml_io_map(TILE_NORTH_PHYS, TILE_SIZE);
		gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
		rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
		cmddb = ml_io_map(CMD_DB_PHYS, CMD_DB_SIZE);
		(void)clocks_on();
		if (rsc != 0) {
			(void)rpmh_read(0x41f00 + VRM_ENABLE, &v);
			if (!v) {
				(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41f00 + VRM_VOLTAGE, 1800);
				(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41f00 + VRM_ENABLE, 1);
			}
			(void)rpmh_read(0x41200 + VRM_ENABLE, &v);
			if (!v) {
				sleep_ms(15);
				(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41200 + VRM_VOLTAGE, 3000);
				(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41200 + VRM_ENABLE, 1);
			}
		}
		se = ml_io_map(SE_PHYS, SE_SIZE);
		if (se) {
			const char *p2 = geni_spi_init(target_spi_hz);

			printf("psyche_touch: 0xa7clk engine init -> %s\n", p2 ? p2 : "ok");
			use_engine_pins();
			engine_transfer2 = geni_transfer2;
			spi_transfer = geni_transfer_single;
		}
		probe0xa7clk_probe();
		return false;
	}

	if (do_probe0xa7) {
		uint32_t v = 0;

		tlmm_west = ml_io_map(TILE_WEST_PHYS, TILE_SIZE);
		tlmm_south = ml_io_map(TILE_SOUTH_PHYS, TILE_SIZE);
		tlmm_north = ml_io_map(TILE_NORTH_PHYS, TILE_SIZE);
		gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
		rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
		cmddb = ml_io_map(CMD_DB_PHYS, CMD_DB_SIZE);
		(void)clocks_on();
		if (rsc != 0) {
			(void)rpmh_read(0x41f00 + VRM_ENABLE, &v);
			if (!v) {
				(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41f00 + VRM_VOLTAGE, 1800);
				(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41f00 + VRM_ENABLE, 1);
			}
			(void)rpmh_read(0x41200 + VRM_ENABLE, &v);
			if (!v) {
				sleep_ms(15);
				(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41200 + VRM_VOLTAGE, 3000);
				(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41200 + VRM_ENABLE, 1);
			}
			printf("psyche_touch: 0xa7 supplies: vdd=%s avdd=%s\n",
			    v ? "on" : "off", v ? "on" : "off");
		}
		se = ml_io_map(SE_PHYS, SE_SIZE);
		if (se) {
			const char *p2 = geni_spi_init(target_spi_hz);

			printf("psyche_touch: 0xa7 engine init -> %s\n", p2 ? p2 : "ok");
			use_engine_pins();
			engine_transfer2 = geni_transfer2;
			spi_transfer = geni_transfer_single;
		}
		if (engine_transfer2 == NULL) {
			bitbang_in = PIN_MISO;
			bitbang_out = PIN_MOSI;
			bitbang_init();
		}
		probe0xa7_probe();
		return false;
	}

	if (do_wirepow) {
		char p3[64];

		tlmm_west = ml_io_map(TILE_WEST_PHYS, TILE_SIZE);
		tlmm_south = ml_io_map(TILE_SOUTH_PHYS, TILE_SIZE);
		tlmm_north = ml_io_map(TILE_NORTH_PHYS, TILE_SIZE);
		gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
		rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
		cmddb = ml_io_map(CMD_DB_PHYS, CMD_DB_SIZE);
		(void)clocks_on();
		se = ml_io_map(SE_PHYS, SE_SIZE);
		/*
		 * Bring the supplies up here rather than relying on the default path: this probe
		 * exists to answer a question about a powered chip.
		 */
		if (rsc != 0) {
			(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41f00 + VRM_VOLTAGE, 1800);
			(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41f00 + VRM_ENABLE, 1);
			sleep_ms(15);
			(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41200 + VRM_VOLTAGE, 3000);
			(void)rpmh_write(MSGID_WRITE_4_BYTES, 0x41200 + VRM_ENABLE, 1);
			sleep_ms(20);
		}
		gpio_set(PIN_VDDIO, true);
		gpio_configure(PIN_VDDIO, 0, PULL_NONE, 8, true);
		sleep_ms(15);
		if (se) {
			strlcpy(p3, geni_spi_init(target_spi_hz) ? "failed" : "ok", sizeof(p3));
			printf("psyche_touch: wirepow engine init -> %s\n", p3);
			use_engine_pins();
			engine_transfer2 = geni_transfer2;
			spi_transfer = geni_transfer_single;
		}
		wirepow_probe();
		return false;
	}

	if (do_powerseq) {
		tlmm_west = ml_io_map(TILE_WEST_PHYS, TILE_SIZE);
		tlmm_south = ml_io_map(TILE_SOUTH_PHYS, TILE_SIZE);
		tlmm_north = ml_io_map(TILE_NORTH_PHYS, TILE_SIZE);
		gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
		rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
		cmddb = ml_io_map(CMD_DB_PHYS, CMD_DB_SIZE);
		(void)clocks_on();
		se = ml_io_map(SE_PHYS, SE_SIZE);
		if (se) {
			const char *p2 = geni_spi_init(target_spi_hz);

			printf("psyche_touch: powerseq engine init -> %s\n", p2 ? p2 : "ok");
			use_engine_pins();
			if (p2 == NULL) {
				/* fts_read() uses the two-phase primitive, fts_write() the single-phase
				 * one: both have to be wired or a command write jumps through null. */
				engine_transfer2 = geni_transfer2;
				spi_transfer = geni_transfer_single;
			}
		}
		if (engine_transfer2 == NULL) {
			bitbang_in = PIN_MISO;
			bitbang_out = PIN_MOSI;
			bitbang_init();
		}
		powerseq_probe();
		return false;
	}

	if (do_misopull) {
		if (!power_on()) {
			return false;
		}
		(void)clocks_on();
		se = ml_io_map(SE_PHYS, SE_SIZE);
		if (se) {
			const char *p2 = geni_spi_init(target_spi_hz);

			printf("psyche_touch: misopull engine init -> %s\n", p2 ? p2 : "ok");
			use_engine_pins();
			if (p2 == NULL) {
				/* fts_read() uses the two-phase primitive, fts_write() the single-phase
				 * one: both have to be wired or a command write jumps through null. */
				engine_transfer2 = geni_transfer2;
				spi_transfer = geni_transfer_single;
			}
		}
		misopull_probe();
		return false;
	}

	if (do_3wire) {
		if (!power_on()) {
			return false;
		}
		threewire_probe();
		return false;
	}

	if (do_crosstalk) {
		if (!power_on()) {
			return false;
		}
		crosstalk_probe();
		return false;
	}

	if (do_misostat) {
		if (!power_on()) {
			return false;
		}
		misostat_probe();
		return false;
	}

	if (do_fastpoll) {
		if (!power_on()) {
			return false;
		}
		(void)clocks_on();
		se = ml_io_map(SE_PHYS, SE_SIZE);
		if (se) {
			const char *p2 = geni_spi_init(target_spi_hz);

			printf("psyche_touch: fastpoll engine init -> %s\n", p2 ? p2 : "ok");
			use_engine_pins();
			if (p2 == NULL) {
				/* fts_read() uses the two-phase primitive, fts_write() the single-phase
				 * one: both have to be wired or a command write jumps through null. */
				engine_transfer2 = geni_transfer2;
				spi_transfer = geni_transfer_single;
			}
		}
		fastpoll_probe();
		return false;
	}

	if (do_rstseq) {
		if (!power_on()) {
			return false;
		}
		(void)clocks_on();
		se = ml_io_map(SE_PHYS, SE_SIZE);
		if (se) {
			const char *p2 = geni_spi_init(target_spi_hz);

			printf("psyche_touch: rstseq engine init -> %s\n", p2 ? p2 : "ok");
			use_engine_pins();
			if (p2 == NULL) {
				/* fts_read() uses the two-phase primitive, fts_write() the single-phase
				 * one: both have to be wired or a command write jumps through null. */
				engine_transfer2 = geni_transfer2;
				spi_transfer = geni_transfer_single;
			}
		}
		if (engine_transfer2 == NULL) {
			bitbang_in = PIN_MISO;
			bitbang_out = PIN_MOSI;
			bitbang_init();
			bitbang_delay_ns = 0;
		}
		rstseq_probe();
		return false;
	}

	if (do_chipcheck) {
		if (!power_on()) {
			return false;
		}
		chipcheck_probe();
		return false;
	}

	if (do_bbcount) {
		if (!power_on()) {
			return false;
		}
		bbcount_probe();
		return false;
	}

	if (do_gpioset) {
		if (!power_on()) {
			return false;
		}
		gpioset_probe();
		return false;
	}

	if (do_bbprobe) {
		if (!power_on()) {
			return false;
		}
		bbprobe_probe();
		return false;
	}

	if (do_clkprobe) {
		if (!power_on()) {
			return false;
		}
		(void)clocks_on();
		se = ml_io_map(SE_PHYS, SE_SIZE);
		if (se) {
			const char *p2 = geni_spi_init(target_spi_hz);

			printf("psyche_touch: clkprobe init -> %s\n", p2 ? p2 : "ok");
			use_engine_pins();
			engine_transfer2 = geni_transfer2;
			spi_transfer = geni_transfer_single;
		}
		clkprobe_probe();
		return false;
	}

	if (do_genidrive) {
		if (!power_on()) {
			return false;
		}
		(void)clocks_on();
		se = ml_io_map(SE_PHYS, SE_SIZE);
		if (se) {
			const char *p2 = geni_spi_init(target_spi_hz);

			printf("psyche_touch: genidrive init -> %s\n", p2 ? p2 : "ok");
			use_engine_pins();
			engine_transfer2 = geni_transfer2;
			spi_transfer = geni_transfer_single;
		}
		genidrive_probe();
		return false;
	}

	if (do_cstest) {
		if (!power_on()) {
			return false;
		}
		(void)clocks_on();
		se = ml_io_map(SE_PHYS, SE_SIZE);
		if (se) {
			const char *p2 = geni_spi_init(target_spi_hz);

			printf("psyche_touch: cstest engine init -> %s\n", p2 ? p2 : "ok");
			use_engine_pins();
			engine_transfer2 = geni_transfer2;
			spi_transfer = geni_transfer_single;
		}
		cstest_probe();
		return false;
	}

	if (do_wirecheck) {
		if (!power_on()) {
			printf("psyche_touch: wirecheck supplies unavailable\n");
			return false;
		}
		(void)clocks_on();
		wirecheck_probe();
		return false;
	}

	if (do_ios) {
		ios_probe();
		return false;
	}

	if (do_wrapper4) {
		wrapper4_probe();
		return false;
	}

	if (do_wrapper3) {
		wrapper3_probe();
		return false;
	}

	if (do_wrapper2) {
		wrapper2_probe();
		return false;
	}

	if (do_wrapper) {
		wrapper_probe();
		return false;
	}

	if (do_untouched) {
		if (!power_on()) {
			return false;
		}
		(void)clocks_on();
		untouched_probe();
		return false;
	}

	if (do_useexisting) {
		if (!power_on()) {
			return false;
		}
		(void)clocks_on();
		useexisting_probe();
		return false;
	}

	if (do_sestate) {
		sestate_probe();
		return false;
	}

	/*
	 * Configure the engine first, then the pads, then the supplies -- the order the one entry
	 * point that ever got an answer uses (touch_fifowatch). The normal probe used to do it the
	 * other way round and got "system info header 00" every time.
	 *
	 * The rate is 1 MHz because that is what works: at the DFS table's 6.4 MHz point every read
	 * came back with no signature (logs/231434-run.log times twenty reads at each rate), and an
	 * 8 MHz request divides down to that same failed point.
	 */
	target_spi_hz = 1000000;

	/*
	 * The windows the normal probe uses. It used to use none of them: gcc, se and rsc are
	 * static and start at zero, and only the experimental entry points below ever mapped
	 * them, so on this path clocks_on() wrote through a null pointer and power_on() fired
	 * its sequencer command at 0xd00 -- the vectors, not the RSC. The result was a machine
	 * that stopped with nothing said, at the first supply read.
	 *
	 * The pins come first because clocks_on() reads and writes GCC and the pin controller
	 * before anything else here touches them.
	 */
	if (tlmm_west == 0) {
		tlmm_west = ml_io_map(TILE_WEST_PHYS, TILE_SIZE);
	}
	if (tlmm_south == 0) {
		tlmm_south = ml_io_map(TILE_SOUTH_PHYS, TILE_SIZE);
	}
	if (tlmm_north == 0) {
		tlmm_north = ml_io_map(TILE_NORTH_PHYS, TILE_SIZE);
	}
	if (gcc == 0) {
		gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	}
	if (rsc == 0) {
		rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	}
	if (gcc == 0 || rsc == 0) {
		printf("psyche_touch: no window for the clocks or the supplies (gcc=%#lx rsc=%#lx)\n",
		    (unsigned long)gcc, (unsigned long)rsc);
		return false;
	}

	/* 先试 GENI，失败退到位翻转 */
	if (!force_bitbang && clocks_on()) {
		se = ml_io_map(SE_PHYS, SE_SIZE);
		if (se) {
			problem = geni_spi_init(target_spi_hz);
			if (problem == NULL) {
				use_engine_pins();
				spi_transfer = geni_transfer_single;
				engine_transfer2 = geni_transfer2;
				on_engine = true;
				printf("psyche_touch: serial engine %#x, interface %#x, params %#x, source %u Hz\n",
				    rd(GENI_FW_REVISION_RO), rd(GENI_IF_DISABLE_RO),
				    rd(SE_HW_PARAM_0), se_source_hz);
			} else {
				printf("psyche_touch: %s; driving the pins by hand\n", problem);
			}
		}
	}

	if (!on_engine) {
		spi_transfer = bitbang_transfer;
		bitbang_init();
		printf("psyche_touch: bitbang SPI in/out/clk/cs gpio %u/%u/%u/%u\n",
		    bitbang_in, bitbang_out, PIN_CLK, PIN_CS);
		if (touch_gpio_diag) bitbang_gpio_diagnostic();
	}

	/* Match the active touch pinctrl state from the phone's live DT. */
	gpio_configure(PIN_INT, 0, PULL_NONE, 8, false);
	gpio_output(PIN_RESET, true, PULL_NONE, 8);

	if (!power_on()) {
		printf("psyche_touch: its supplies could not be turned on\n");
		return false;
	}

	if (!fts_system_info()) {
		printf("psyche_touch: no system info from the chip\n");
		return false;
	}

	if (!fts_reset()) {
		printf("psyche_touch: the chip did not report CONTROLLER_READY after reset\n");
	} else {
		printf("psyche_touch: controller ready (%s)\n", on_engine ? "GENI" : "bitbang");
		if (fts_sense_on()) {
			printf("psyche_touch: sensing started (scan mode active)\n");
		} else {
			printf("psyche_touch: the scan-mode command did not go out\n");
		}
	}

	/*
	 * Read one event now, from the probe, rather than trusting the poll thread to prove it:
	 * the thread is started microseconds before launchd panics this session, so it has never
	 * managed to log anything. The chip is in sensing mode at this point, so a FIFO read here
	 * answers the only question left open -- whether this kernel can get data off it at all.
	 */
	/*
	 * Only when the probe is the whole experiment.  fts_sense_on() ends by
	 * warning that reading the fifo right after the sense enable consumes the
	 * scans the engine is just starting to produce, and the chip then sits
	 * silent -- which is what a normal run measured: 9375 reads, nonempty=0,
	 * irq_low=1.  touch_probonly=1 stops before the poll thread, so there the
	 * reads cost nothing and still prove the engine can get a frame off the part.
	 */
	for (int probe_read = 0; prob_only && probe_read < 3; probe_read++) {
		uint8_t data[FTS_FIFO_EVENT_SIZE * 4];
		uint32_t events = 0;
		unsigned i;

		memset(data, 0, sizeof(data));
		if (!fts_read_fifo(data, &events)) {
			printf("psyche_touch: fifo probe %d: the read did not complete\n", probe_read);
			sleep_ms(20);
			continue;
		}
		printf("psyche_touch: fifo probe %d: %u event(s), irq=%u\n",
		    probe_read, events, gpio_get(PIN_INT));
		for (i = 0; i < events && i < 4; i++) {
			const uint8_t *e = &data[i * FTS_FIFO_EVENT_SIZE];

			printf("psyche_touch: fifo probe %d event %u: %02x %02x %02x %02x %02x %02x %02x %02x\n",
			    probe_read, i, e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
		}
		sleep_ms(30);
	}
	if (prob_only) {
		printf("psyche_touch: fifo probe done\n");
	}

	/*
	 * touch_probonly=1: the probe has done what it came to do, so stop here.
	 *
	 * touch_fifowatch, the one entry point that ever got an answer, ends its probe the same
	 * way. Stopping here keeps the poll thread and everything after it out of the
	 * experiment, so a run either proves the probe or proves nothing.
	 */
	if (prob_only) {
		printf("psyche_touch: probe only -- stopping before the poll thread\n");
		return false;
	}

	/*
	 * Leave the chip's interrupt enable ON.  This write used to send
	 * FTS_IER_DISABLE (0x00), which is the register that decides whether the part
	 * ever pulls its INT line low: with it at zero the line stayed high for the
	 * whole session and the poll thread counted irq_low=0 over 7500 polls.  The
	 * fifowatch entry point, the only one that has ever read a real event, never
	 * runs this write -- and it saw the line low on 188 of 200 reads.
	 *
	 * Nothing is wired to a GIC line here, so the pin only ever gets polled; an
	 * asserted interrupt cannot land anywhere it is not handled.
	 */
	if (fts_write(FTS_CMD_HW_REG_W, FTS_ADDR_IER, FTS_ADDR_SIZE_HW_REG,
	    (uint8_t[]){ FTS_IER_ENABLE }, 1)) {
		printf("psyche_touch: interrupt enable set (%#x)\n", FTS_IER_ENABLE);
	} else {
		printf("psyche_touch: the interrupt-enable write did not go out\n");
	}

	return true;
}

void
hakios_psyche_touch_init(void)
{
	int enabled = 0;

	PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled));
	if (!enabled) {
		return;
	}
	PE_parse_boot_argn("touch_poll_us", &touch_poll_us, sizeof(touch_poll_us));
	if (touch_poll_us < 1000) {
		touch_poll_us = 1000;
	}
	PE_parse_boot_argn("touch_spi_hz", &target_spi_hz, sizeof(target_spi_hz));
	PE_parse_boot_argn("psyche_trace_rx", &trace_rx, sizeof(trace_rx));
	PE_parse_boot_argn("touch_lat", &trace_latency, sizeof(trace_latency));
	PE_parse_boot_argn("touch_probonly", &prob_only, sizeof(prob_only));
	PE_parse_boot_argn("touch_step", &touch_step, sizeof(touch_step));
	PE_parse_boot_argn("touch_rawdump", &touch_rawdump, sizeof(touch_rawdump));
	PE_parse_boot_argn("touch_x_min", &fts_x_min, sizeof(fts_x_min));
	PE_parse_boot_argn("touch_x_max", &fts_x_max, sizeof(fts_x_max));
	PE_parse_boot_argn("touch_y_min", &fts_y_min, sizeof(fts_y_min));
	PE_parse_boot_argn("touch_y_max", &fts_y_max, sizeof(fts_y_max));
	if (fts_x_max <= fts_x_min + 16) {
		fts_x_min = FTS_X_MIN_DEFAULT;
		fts_x_max = FTS_X_MAX_DEFAULT;
	}
	if (fts_y_max <= fts_y_min + 16) {
		fts_y_min = FTS_Y_MIN_DEFAULT;
		fts_y_max = FTS_Y_MAX_DEFAULT;
	}
	PE_parse_boot_argn("psyche_dump_db", &psyche_dump_db, sizeof(psyche_dump_db));
	{ int s = 0; PE_parse_boot_argn("touch_wirecheck", &s, sizeof(s)); do_wirecheck = s != 0; }
	{ int s = 0; PE_parse_boot_argn("touch_cstest", &s, sizeof(s)); do_cstest = s != 0; }
	{ int s = 0; PE_parse_boot_argn("touch_genidrive", &s, sizeof(s)); do_genidrive = s != 0; }
	{ int s = 0; PE_parse_boot_argn("touch_clkprobe", &s, sizeof(s)); do_clkprobe = s != 0; }
	{ int s = 0; PE_parse_boot_argn("touch_bbprobe", &s, sizeof(s)); do_bbprobe = s != 0; }
	{ int s = 0; PE_parse_boot_argn("touch_gpioset", &s, sizeof(s)); do_gpioset = s != 0; }
	{ int s = 0; PE_parse_boot_argn("touch_bbcount", &s, sizeof(s)); do_bbcount = s != 0; }
	{ int s = 0; PE_parse_boot_argn("touch_chipcheck", &s, sizeof(s)); do_chipcheck = s != 0; }
	{ int s = 0; PE_parse_boot_argn("touch_rstseq", &s, sizeof(s)); do_rstseq = s != 0; }
	{ int s = 0; PE_parse_boot_argn("touch_fastpoll", &s, sizeof(s)); do_fastpoll = s != 0; }
	{ int s = 0; PE_parse_boot_argn("touch_misostat", &s, sizeof(s)); do_misostat = s != 0; }
	{ int s = 0; PE_parse_boot_argn("touch_crosstalk", &s, sizeof(s)); do_crosstalk = s != 0; }
	{ int s = 0; PE_parse_boot_argn("touch_3wire", &s, sizeof(s)); do_3wire = s != 0; }
	{ int s = 0; PE_parse_boot_argn("touch_misopull", &s, sizeof(s)); do_misopull = s != 0; }
	{ int s = 0; PE_parse_boot_argn("touch_setvolt", &s, sizeof(s)); do_setvolt = s != 0; }
	PE_parse_boot_argn("touch_powerseq", &do_powerseq, sizeof(do_powerseq));
	PE_parse_boot_argn("touch_wirepow", &do_wirepow, sizeof(do_wirepow));
	PE_parse_boot_argn("touch_probe0xa7", &do_probe0xa7, sizeof(do_probe0xa7));
	PE_parse_boot_argn("touch_0xa7clk", &do_0xa7clk, sizeof(do_0xa7clk));
	PE_parse_boot_argn("touch_0xa7dump", &do_0xa7dump, sizeof(do_0xa7dump));
	PE_parse_boot_argn("touch_fifowatch", &do_fifowatch, sizeof(do_fifowatch));
	PE_parse_boot_argn("touch_clklevels", &do_clklevels, sizeof(do_clklevels));
	PE_parse_boot_argn("touch_speed", &do_speed, sizeof(do_speed));
	PE_parse_boot_argn("touch_powerseq_avdd", &powerseq_avdd_mv, sizeof(powerseq_avdd_mv));
	PE_parse_boot_argn("touch_powerseq_vddio", &powerseq_vddio, sizeof(powerseq_vddio));
	if (powerseq_reps == 0) powerseq_reps = 1;
	{ int s = 0; PE_parse_boot_argn("touch_sestate", &s, sizeof(s)); do_sestate = s != 0; }
	{ int s = 0; PE_parse_boot_argn("touch_useexisting", &s, sizeof(s)); do_useexisting = s != 0; }
	{ int s = 0; PE_parse_boot_argn("touch_untouched", &s, sizeof(s)); do_untouched = s != 0; }
	{ int s = 0; PE_parse_boot_argn("touch_wrapper", &s, sizeof(s)); do_wrapper = s != 0; }
	{ int s = 0; PE_parse_boot_argn("touch_wrapper2", &s, sizeof(s)); do_wrapper2 = s != 0; }
	{ int s = 0; PE_parse_boot_argn("touch_wrapper3", &s, sizeof(s)); do_wrapper3 = s != 0; }
	{ int s = 0; PE_parse_boot_argn("touch_wrapper4", &s, sizeof(s)); do_wrapper4 = s != 0; }
	{ int s = 0; PE_parse_boot_argn("touch_ios", &s, sizeof(s)); do_ios = s != 0; }
	PE_parse_boot_argn("psyche_vote_rails", &psyche_vote_rails, sizeof(psyche_vote_rails));
	PE_parse_boot_argn("touch_gpio_diag", &touch_gpio_diag, sizeof(touch_gpio_diag));
	PE_parse_boot_argn("touch_bb_delay_ns", &bitbang_delay_ns, sizeof(bitbang_delay_ns));
	int bb = 0;
	PE_parse_boot_argn("touch_bitbang", &bb, sizeof(bb));
	force_bitbang = bb != 0;
	int bb_swap = 0;
	PE_parse_boot_argn("touch_bb_swap", &bb_swap, sizeof(bb_swap));
	if (bb_swap) {
		bitbang_in = PIN_MOSI;
		bitbang_out = PIN_MISO;
	}
	PE_parse_boot_argn("psyche_loopback", &psyche_loopback, sizeof(psyche_loopback));

	if (!probe()) {
		return;
	}

	printf("psyche_touch: %u Hz polling, %u touches max, raw x %u..%u y %u..%u -> %ux%u\n",
	    1000000 / touch_poll_us, FTS_MAX_TOUCHES, fts_x_min, fts_x_max, fts_y_min, fts_y_max,
	    FTS_X_SIZE_DEFAULT, FTS_Y_SIZE_DEFAULT);
	/*
	 * The chip reports nothing on its own: a run with nobody at the screen reads
	 * thousands of empty frames and counts nonempty=0.  Say so once, the way the
	 * fifowatch entry point always has, so an empty run is not mistaken for a dead
	 * driver.
	 */
	printf("psyche_touch: touch the screen now\n");

	thread_t th;
	kernel_thread_start((thread_continue_t)fts_thread, NULL, &th);
}
