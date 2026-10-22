/*
 * sargo touch bring-up in the shim (build with BRINGUP_TOUCH): Synaptics
 * RMI4 at 0x20 on i2c9 = QUP wrapper 1 SE1 at 0xa84000, the same engine,
 * GCC clock registers and SDA/SCL pins (gpio6/7, function 1) as sweet's
 * touch bus. Reset is gpio99, IRQ gpio125 (Linux sdm670-google-common).
 * Unlike sweet, this SE's FIFO interface is disabled (t01): transfers go
 * through the GPI DMA engine, see gpi_init() below.
 *
 * Writes only: GCC clock vote/RCG, TLMM pin config, GENI and GPI (GPII 1)
 * registers, and on the touch IC the RMI4 page select register (0xff = 0).
 * TOUCH=bb also drives PM660 GPIO 12 high over SPMI (the IC's 1.8 V
 * switch, left off by ABL: t08 read SDA/SCL low with pull-ups on).
 *
 * MMU off: aligned 32-bit accesses only (Device memory).
 */
#include <stdint.h>
#include "board.h"
#include "console.h"

#ifdef TOUCH_BB
/* the GENI/GPI/SMMU path stays in the file but is not used by TOUCH=bb */
#pragma clang diagnostic ignored "-Wunused-function"
#endif

#define GCC_PA		0x00100000UL
#define GCC_VOTE	0x5200c		/* QUP wrapper 1 + SE1 votes */
#define SE1_RCG		0x18148		/* gcc_qupv3_wrap1_s1_clk_src */
#define TLMM_NORTH	0x03900000UL	/* TLMM 0x3400000 + NORTH 0x500000 */
#define SE_PA		0x00a84000UL	/* i2c9 */
#define GPIO_CFG(n)	(TLMM_NORTH + (uint32_t)(n) * 0x1000u)
#define GPIO_IO(n)	(GPIO_CFG(n) + 4u)
#define PIN_SDA		6
#define PIN_SCL		7
#define PIN_RESET	99
#define PIN_IRQ		125
#define RMI_ADDR	0x20

#define SE_STATUS	0x040
#define SE_MCLK		0x048
#define SE_FW		0x068
#define SE_CLK_SEL	0x07c
#define SE_DMA		0x258
#define SE_TX_LEN	0x26c
#define SE_RX_LEN	0x270
#define SE_SCL		0x278
#define SE_CMD		0x600
#define SE_CTRL		0x604
#define SE_IRQ		0x610
#define SE_IRQ_EN	0x614
#define SE_IRQ_CLR	0x618
#define SE_TX		0x700
#define SE_RX		0x780
#define SE_TX_COUNT	0x800
#define SE_RX_COUNT	0x804
#define SE_TX_WM	0x80c
#define SE_HW_TX	0xe24
#define SE_HW_RX	0xe28
#define SE_RX_WM_IRQ	(1u << 26)
#define SE_RX_LAST	(1u << 27)
#define SE_TX_WM_IRQ	(1u << 30)
#define SE_DONE		(1u << 0)
#define SE_NACK		(1u << 10)
#define SE_ERRORS	(0x3eu | (1u << 9) | (1u << 10) | (1u << 12) | \
			 (1u << 13) | (3u << 24) | (3u << 28))
#define SE_EVENTS	(SE_DONE | SE_ERRORS | SE_RX_WM_IRQ | SE_RX_LAST | SE_TX_WM_IRQ)
#define SE_PACK0	0x0007f8feu
#define SE_PACK1	0x000ffefeu

#define E_NACK		1
#define E_IO		2
#define E_TIME		3

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; __asm__ volatile("dsb sy" ::: "memory"); }

static uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

#define TICKS_PER_MS	19200

static void mdelay(unsigned ms)
{
	uint64_t end = ticks() + (uint64_t)ms * TICKS_PER_MS;
	while (ticks() < end)
		;
}

static void udelay(unsigned us)
{
	uint64_t end = ticks() + (uint64_t)us * 19 + 1;
	while (ticks() < end)
		;
}

static int wait_clear(uintptr_t a, uint32_t mask)
{
	for (unsigned i = 0; i < 5000; i++) {
		if (!(rd(a) & mask))
			return 1;
		udelay(1);
	}
	return 0;
}

static void gpio(unsigned pin, unsigned mux, unsigned pull, unsigned drive, int out, int high)
{
	uint32_t cfg = rd(GPIO_CFG(pin));
	if (out)
		wr(GPIO_IO(pin), (rd(GPIO_IO(pin)) & ~2u) | (high ? 2u : 0u));
	cfg &= ~0x3ffu;
	cfg |= (mux << 2) | pull | (drive << 6) | (out ? 0x200u : 0u);
	wr(GPIO_CFG(pin), cfg);
}

static int clocks(void)
{
	uint32_t mask = (0x0fu << 18) | (1u << 23);
	wr(GCC_PA + GCC_VOTE, rd(GCC_PA + GCC_VOTE) | mask);
	uint32_t dfs = rd(GCC_PA + SE1_RCG + 0x14), cfg = rd(GCC_PA + SE1_RCG + 4);
	con_hex2("GCC", "vote=", rd(GCC_PA + GCC_VOTE), "cfg=", cfg);
	con_hex2(" rcg", "cmd=", rd(GCC_PA + SE1_RCG), "dfs=", dfs);
	if (dfs & 1) {
		cfg = rd(GCC_PA + SE1_RCG + 0x1c);
		if ((cfg & 0x371f) != 1) {
			con_hex2("GCC", "DFS lvl0 NOT XO=", cfg, 0, 0);
			return 0;
		}
	} else {
		cfg = (cfg & ~(0x371fu | (1u << 20))) | 1u;	/* XO / 1 = 19.2 MHz */
		wr(GCC_PA + SE1_RCG + 4, cfg);
		wr(GCC_PA + SE1_RCG, rd(GCC_PA + SE1_RCG) | 1);
		if (!wait_clear(GCC_PA + SE1_RCG, 1)) {
			con_str("GCC", "RCG update timeout");
			return 0;
		}
	}
	static const uint32_t branches[] = { 0x18004, 0x18008, 0x1800c, 0x18010, 0x18144 };
	for (unsigned i = 0; i < 5; i++)
		if (!wait_clear(GCC_PA + branches[i], 1u << 31)) {
			con_hex2("GCC", "halted br=", branches[i], "v=", rd(GCC_PA + branches[i]));
			return 0;
		}
	return 1;
}

/*
 * t01 showed GENI_IF_DISABLE_RO (0x64) bit 0: the FIFO interface of this SE
 * is disabled by the QUP firmware, so every transfer goes through the GPI DMA
 * engine (GSI) of QUP wrapper 1: 0xa00000, apps GPIIs 0xfa (stock DT
 * qcom,gpi-dma@0xa00000; i2c9 dmas = <&gpi 0/1 1 3 64 0>: tx/rx, SE 1, I2C).
 * The SE goes to GPI DMA mode (Linux geni_se_select_gpi_mode); GPII 1 gets
 * one event ring and a TX/RX channel pair (Linux qcom/gpi.c gpi_ch_init),
 * polled instead of an interrupt. Transfers are built like Linux
 * geni_i2c_gpi_xfer: CONFIG0 + GO + DMA TREs.
 */
#define GPI_BASE	0x00a00000UL
#define GPII		1
#define GN		(GPI_BASE + 0x4000UL * GPII)
#define CH_CTX(k)	(GN + 0x20000 + 0x80 * (k))
#define CH_QOS(k)	(GN + 0x2005c + 0x80 * (k))
#define CH_SCRATCH(k, i)	(GN + 0x20060 + 0x80 * (k) + 4 * (i))
#define EV_CTX		(GN + 0x21000)
#define CH_DB(k)	(GN + 0x22000 + 8 * (k))
#define EV_DB		(GN + 0x22100)
#define CH_CMD		(GN + 0x23008)
#define EV_CMD		(GN + 0x23010)
#define TYPE_IRQ	(GN + 0x23080)
#define TYPE_IRQ_MSK	(GN + 0x23088)
#define SRC_CH_IRQ	(GN + 0x23090)
#define SRC_CH_IRQ_MSK	(GN + 0x23098)
#define SRC_EV_IRQ_MSK	(GN + 0x2309c)
#define SRC_CH_IRQ_CLR	(GN + 0x230a0)
#define SRC_EV_IRQ_CLR	(GN + 0x230a4)
#define SRC_IEOB_MSK	(GN + 0x230b8)
#define SRC_IEOB_CLR	(GN + 0x230c0)
#define GLOB_IRQ_STTS	(GN + 0x23100)
#define GLOB_IRQ_EN	(GN + 0x23108)
#define GPII_IRQ_STTS	(GN + 0x23118)
#define GPII_IRQ_EN	(GN + 0x23120)
#define INTSET		(GN + 0x23180)
#define MSI_LSB		(GN + 0x23188)
#define MSI_MSB		(GN + 0x2318c)
#define CNTXT_SCRATCH0	(GN + 0x23400)
#define CNTXT_SCRATCH1	(GN + 0x23404)
#define ERROR_LOG	(GN + 0x23200)

#define CTX_RLEN	0x04
#define CTX_BASE_LO	0x08
#define CTX_BASE_HI	0x0c
#define CTX_RP_LO	0x10
#define CTX_INT_MOD	0x20
#define CTX_MSI_LO	0x28
#define CTX_MSI_HI	0x2c
#define CTX_RPU_LO	0x30
#define CTX_RPU_HI	0x34
#define CHSTATE(v)	(((v) >> 20) & 0xf)

#define TRE_DMA		0x10
#define TRE_GO		0x20
#define TRE_CONFIG0	0x22
#define F_CHAIN		(1u << 0)
#define F_IEOT		(1u << 9)
#define F_LINK		(1u << 11)
#define I2C_WR		1
#define I2C_RD		2

#define RING_N		16			/* TREs per channel ring */
#define EV_N		64
static uint32_t tx_ring[RING_N * 4] __attribute__((aligned(RING_N * 16)));
static uint32_t rx_ring[RING_N * 4] __attribute__((aligned(RING_N * 16)));
static uint32_t ev_ring[EV_N * 4] __attribute__((aligned(EV_N * 16)));
static uint8_t xbuf[128] __attribute__((aligned(64)));
static unsigned tx_wp, rx_wp, ev_rp, ev_wp;
static uint32_t last_ev[4];

static uintptr_t pa(const void *p) { return (uintptr_t)p; }	/* MMU off: VA == PA */

static void gpi_dump(const char *why)
{
	con_str("GPI", why);
	con_hex2(" ch", "tx=", rd(CH_CTX(0)), "rx=", rd(CH_CTX(1)));
	con_hex2(" ev", "ctx=", rd(EV_CTX), "rp=", rd(EV_CTX + CTX_RP_LO));
	con_hex2(" ch rp", "tx=", rd(CH_CTX(0) + CTX_RP_LO), "rx=", rd(CH_CTX(1) + CTX_RP_LO));
	con_hex2(" irq", "type=", rd(TYPE_IRQ), "glob=", rd(GLOB_IRQ_STTS));
	con_hex2(" err", "log=", rd(ERROR_LOG), "gpii=", rd(GPII_IRQ_STTS));
	con_hex2(" last ev", "w2=", last_ev[2], "w3=", last_ev[3]);
}

/* Command, then poll the context state (Linux waits for the CH/EV ctrl IRQ). */
static int gpi_cmd(uintptr_t cmd_reg, uint32_t cmd, uintptr_t ctx, uint32_t want)
{
	wr(cmd_reg, cmd);
	uint64_t end = ticks() + 250ull * TICKS_PER_MS;
	while (ticks() < end) {
		if (CHSTATE(rd(ctx)) == want) {
			wr(SRC_CH_IRQ_CLR, rd(SRC_CH_IRQ));
			wr(SRC_EV_IRQ_CLR, 0xffffffffu);
			return 1;
		}
		udelay(10);
	}
	return 0;
}

static void se_gsi_mode(void)
{
	wr(SE_PA + SE_IRQ_CLR, 0xffffffffu);
	wr(SE_PA + 0x648, 0xffffffffu);		/* S IRQ clear */
	wr(SE_PA + 0xe1c, 0);			/* SE_IRQ_EN */
	wr(SE_PA + SE_IRQ_EN, rd(SE_PA + SE_IRQ_EN) & ~(SE_DONE | SE_TX_WM_IRQ | SE_RX_WM_IRQ | SE_RX_LAST));
	wr(SE_PA + SE_DMA, 1);			/* SE_GENI_DMA_MODE_EN */
	wr(SE_PA + 0xe18, rd(SE_PA + 0xe18) | 0xf);	/* SE_GSI_EVENT_EN: DMA rx/tx, M, S */
	wr(SE_PA + SE_CLK_SEL, 0);
	wr(SE_PA + SE_MCLK, (7u << 4) | 1u);
}

static int gpi_init(void)
{
	uint32_t fw = rd(SE_PA + SE_FW);
	con_hex2("SE", "fw=", fw, "ifdis=", rd(SE_PA + 0x64));
	if (((fw >> 8) & 0xff) != 3 || !(rd(SE_PA + 0x64) & 1)) {
		con_str("SE", "not an I2C SE in GSI mode");
		return 0;
	}
	con_hex2("GPII1", "tx=", rd(CH_CTX(0)), "rx=", rd(CH_CTX(1)));
	con_hex2(" ev", "ctx=", rd(EV_CTX), "errlog=", rd(ERROR_LOG));
	/* Left allocated by the bootloader? reset + de-allocate first. */
	for (unsigned k = 0; k < 2; k++)
		if (CHSTATE(rd(CH_CTX(k)))) {
			if (CHSTATE(rd(CH_CTX(k))) == 2)
				gpi_cmd(CH_CMD, 2u << 24 | k, CH_CTX(k), 3);	/* STOP */
			gpi_cmd(CH_CMD, 9u << 24 | k, CH_CTX(k), 1);		/* RESET */
			wr(CH_CMD, 10u << 24 | k);				/* DE_ALLOC */
			mdelay(5);
		}
	if (CHSTATE(rd(EV_CTX))) {
		gpi_cmd(EV_CMD, 9u << 24, EV_CTX, 1);
		wr(EV_CMD, 10u << 24);
		mdelay(5);
	}
	se_gsi_mode();

	/* gpi_config_interrupts(DEFAULT): masks and INTSET as Linux; no GIC IRQ used */
	wr(TYPE_IRQ_MSK, 0x4f);
	wr(SRC_IEOB_MSK, 1);
	wr(SRC_CH_IRQ_MSK, 3);
	wr(SRC_EV_IRQ_MSK, 1);
	wr(GLOB_IRQ_EN, 0xf);
	wr(GPII_IRQ_EN, 0xf);
	wr(MSI_LSB, 0);
	wr(MSI_MSB, 0);
	wr(CNTXT_SCRATCH0, 0);
	wr(CNTXT_SCRATCH1, 0);
	wr(INTSET, 1);
	wr(ERROR_LOG, 0);

	/* event ring (gpi_alloc_ev_chan) */
	for (unsigned i = 0; i < EV_N * 4; i++)
		ev_ring[i] = 0;
	if (!gpi_cmd(EV_CMD, 0, EV_CTX, 1)) {
		gpi_dump("EV allocate timeout");
		return 0;
	}
	wr(EV_CTX, 16u << 24 | 1u << 16 | 2u);		/* el size 16, IRQ type, GPI event */
	wr(EV_CTX + CTX_RLEN, EV_N * 16);
	wr(EV_CTX + CTX_BASE_LO, (uint32_t)pa(ev_ring));
	wr(EV_CTX + CTX_BASE_HI, (uint32_t)(pa(ev_ring) >> 32));
	wr(EV_DB + 4, (uint32_t)(pa(ev_ring) >> 32));
	wr(EV_CTX + CTX_INT_MOD, 0);
	wr(EV_CTX + CTX_MSI_LO, 0);
	wr(EV_CTX + CTX_MSI_HI, 0);
	wr(EV_CTX + CTX_RPU_LO, 0);
	wr(EV_CTX + CTX_RPU_HI, 0);
	ev_rp = 0;
	ev_wp = EV_N - 1;
	wr(EV_DB, (uint32_t)pa(&ev_ring[ev_wp * 4]));

	/* channels (gpi_alloc_chan): 0 = TX (dir out), 1 = RX (dir in) */
	for (unsigned k = 0; k < 2; k++) {
		uint32_t *ring = k ? rx_ring : tx_ring;
		for (unsigned i = 0; i < RING_N * 4; i++)
			ring[i] = 0;
		if (!gpi_cmd(CH_CMD, 0u << 24 | k, CH_CTX(k), 1)) {
			gpi_dump(k ? "RX allocate timeout" : "TX allocate timeout");
			return 0;
		}
		wr(CH_CTX(k), 16u << 24 | 0u << 14 | (k ? 0u : 1u) << 3 | 2u);
		wr(CH_CTX(k) + CTX_RLEN, RING_N * 16);
		wr(CH_CTX(k) + CTX_BASE_LO, (uint32_t)pa(ring));
		wr(CH_CTX(k) + CTX_BASE_HI, (uint32_t)(pa(ring) >> 32));
		wr(CH_DB(k) + 4, (uint32_t)(pa(ring) >> 32));
		wr(CH_SCRATCH(k, 0), (uint32_t)(!k) << 16 | 3u << 4 | 1u);	/* pair, I2C, SE 1 */
		wr(CH_SCRATCH(k, 1), 0);
		wr(CH_SCRATCH(k, 2), 0);
		wr(CH_SCRATCH(k, 3), 0);
		wr(CH_QOS(k), 1);
	}
	for (unsigned k = 0; k < 2; k++)
		if (!gpi_cmd(CH_CMD, 1u << 24 | k, CH_CTX(k), 2)) {
			gpi_dump(k ? "RX start timeout" : "TX start timeout");
			return 0;
		}
	tx_wp = rx_wp = 0;
	con_hex2("GPI", "started tx=", rd(CH_CTX(0)), "rx=", rd(CH_CTX(1)));
	return 1;
}

/*
 * Read-only look at apps_smmu (qcom,qsmmu-v500 = MMU-500 at 0x15000000, the
 * SMMU in GPI's iommus = <&apps_smmu 0x6d6 0>; the eMMC node has no iommus).
 * Stream match entries for GPI's SID: type 0 translate, 1 bypass, 2 fault.
 */
#define SMMU		0x15000000UL
static void smmu_probe(unsigned sid)
{
	uint32_t scr0 = rd(SMMU), idr0 = rd(SMMU + 0x20);
	con_hex2("SMMU", "scr0=", scr0, "idr0=", idr0);
	unsigned n = idr0 & 0xff, hits = 0;
	for (unsigned i = 0; i < n && i < 256; i++) {
		uint32_t smr = rd(SMMU + 0x800 + 4 * i);
		uint32_t id = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;
		if (!(smr & (1u << 31)) || ((id ^ sid) & ~mask & 0x7fff))
			continue;
		uint32_t s2cr = rd(SMMU + 0xc00 + 4 * i);
		con_hex2(" smr", "v=", smr, "s2cr=", s2cr);
		hits++;
	}
	con_hex2(" sid", "id=", sid, "matches=", hits);
}

/*
 * t03: sCR0 0x00200406 (enabled, USFCFG: unmatched streams fault) and no
 * stream match entry for 0x6d6, so GSI's first TRE fetch faulted and the CPU's
 * doorbell write never completed. Install one, as Linux's SMMU driver would:
 * S2CR bypass; if the hypervisor turns bypass into fault (Linux arm-smmu-qcom
 * "bypass quirk"), point the stream at the last context bank configured as
 * S1-translate/S2-bypass with SCTLR.M = 0, i.e. untranslated.
 */
static uintptr_t smmu_cb_base;		/* context bank used for 0x6d6, 0 = plain bypass */

static int __attribute__((unused)) smmu_bypass(unsigned sid)	/* t04/t05: CB 46 - the system stopped */
{
	uint32_t idr0 = rd(SMMU + 0x20), idr1 = rd(SMMU + 0x24);
	unsigned n = idr0 & 0xff, slot = n;
	con_hex2("SMMU", "idr1=", idr1, "gfsr=", rd(SMMU + 0x48));
	for (unsigned i = n; i-- > 0;)
		if (!(rd(SMMU + 0x800 + 4 * i) & (1u << 31))) {
			slot = i;
			break;
		}
	if (slot == n) {
		con_str("SMMU", "no free stream match entry");
		return 0;
	}
	wr(SMMU + 0xc00 + 4 * slot, 1u << 16);			/* S2CR: bypass */
	wr(SMMU + 0x800 + 4 * slot, 1u << 31 | (sid & 0x7fff));	/* SMR: valid, exact id */
	uint32_t s2cr = rd(SMMU + 0xc00 + 4 * slot);
	con_hex2(" slot", "n=", slot, "s2cr=", s2cr);
	con_hex2(" smr", "v=", rd(SMMU + 0x800 + 4 * slot), 0, 0);
	if (((s2cr >> 16) & 3) == 1)
		return 1;
	/* bypass quirk: last context bank, S1 translate / S2 bypass, translation off */
	unsigned pgsize = (idr1 >> 31) ? 0x10000 : 0x1000;
	unsigned npage = 1u << (((idr1 >> 28) & 7) + 1);
	unsigned cb = (idr1 & 0xff) - 1;
	uintptr_t cbb = SMMU + (uintptr_t)npage * pgsize + (uintptr_t)cb * pgsize;
	con_hex2(" cb", "n=", cb, "cbar=", rd(SMMU + pgsize + 4 * cb));
	wr(SMMU + pgsize + 4 * cb, 1u << 16);			/* GR1 CBAR: S1 trans, S2 bypass */
	wr(cbb + 0x0, 0);					/* CB SCTLR: M = 0 */
	wr(SMMU + 0xc00 + 4 * slot, 0u << 16 | cb);		/* S2CR: translate via that CB */
	smmu_cb_base = cbb;
	con_hex2(" cb", "cbar=", rd(SMMU + pgsize + 4 * cb), "s2cr=", rd(SMMU + 0xc00 + 4 * slot));
	return 1;
}

/*
 * t05: the system stops within ~1 ms of the first doorbell even with the
 * CB 46 (SCTLR.M = 0) workaround: likely the secure side faulting GSI's
 * untranslated access. Dump how the firmware routes the streams that do
 * work (the eMMC has no iommus but DMAs fine) to copy that for 0x6d6.
 * Per valid SMR: index, SMR, S2CR; for S2CR type translate also CBAR/SCTLR.
 */
static void __attribute__((unused)) smmu_dump(void)
{
	uint32_t idr0 = rd(SMMU + 0x20), idr1 = rd(SMMU + 0x24);
	unsigned n = idr0 & 0xff, pgsize = (idr1 >> 31) ? 0x10000 : 0x1000;
	unsigned npage = 1u << (((idr1 >> 28) & 7) + 1), shown = 0;
	con_hex2("SMMU", "scr0=", rd(SMMU), "gfsr=", rd(SMMU + 0x48));
	con_hex2(" ids", "idr0=", idr0, "idr1=", idr1);
	for (unsigned i = 0; i < n && shown < 44; i++) {
		uint32_t smr = rd(SMMU + 0x800 + 4 * i);
		if (!(smr & (1u << 31)))
			continue;
		uint32_t s2cr = rd(SMMU + 0xc00 + 4 * i);
		char lbl[] = "S00";
		lbl[1] = "0123456789ABCDEF"[i >> 4];
		lbl[2] = "0123456789ABCDEF"[i & 15];
		if (((s2cr >> 16) & 3) == 0) {
			unsigned cb = s2cr & 0xff;
			uint32_t cbar = rd(SMMU + pgsize + 4 * cb);
			uint32_t sctlr = rd(SMMU + (uintptr_t)npage * pgsize + (uintptr_t)cb * pgsize);
			con_hex2(lbl, "smr=", smr, "s2cr=", s2cr);
			con_hex2(" cb", "cbar=", cbar, "sctlr=", sctlr);
			shown += 2;
		} else {
			con_hex2(lbl, "smr=", smr, "s2cr=", s2cr);
			shown++;
		}
	}
}

/*
 * t06: the firmware routes the streams that work - the eMMC's 0x140/0xf
 * among them - to context bank 0 (CBAR 0x10000 = S1 translate / S2 bypass,
 * SCTLR 0xe0 = translation off): Linux's bypass-quirk setup, but in CB 0.
 * t04/t05 used CB 46 and the system stopped at the first DMA. Point 0x6d6
 * at the firmware's own CB 0 instead; change nothing else.
 */
static int smmu_route_cb0(unsigned sid)
{
	uint32_t idr0 = rd(SMMU + 0x20), idr1 = rd(SMMU + 0x24);
	unsigned n = idr0 & 0xff, slot = n, pgsize = (idr1 >> 31) ? 0x10000 : 0x1000;
	unsigned npage = 1u << (((idr1 >> 28) & 7) + 1);
	uint32_t cbar0 = rd(SMMU + pgsize), sctlr0 = rd(SMMU + (uintptr_t)npage * pgsize);
	con_hex2("CB0", "cbar=", cbar0, "sctlr=", sctlr0);
	if (cbar0 != 0x10000 || (sctlr0 & 1)) {
		con_str("CB0", "not the untranslated bank seen in t06");
		return 0;
	}
	for (unsigned i = n; i-- > 0;)
		if (!(rd(SMMU + 0x800 + 4 * i) & (1u << 31))) {
			slot = i;
			break;
		}
	if (slot == n) {
		con_str("SMMU", "no free stream match entry");
		return 0;
	}
	wr(SMMU + 0xc00 + 4 * slot, 0);				/* S2CR: translate via CB 0 */
	wr(SMMU + 0x800 + 4 * slot, 1u << 31 | (sid & 0x7fff));	/* SMR: valid, exact id */
	con_hex2(" slot", "n=", slot, "s2cr=", rd(SMMU + 0xc00 + 4 * slot));
	con_hex2(" smr", "v=", rd(SMMU + 0x800 + 4 * slot), 0, 0);
	return 1;
}

static void put_tre(uint32_t *ring, unsigned *wp, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{
	uint32_t *t = &ring[*wp * 4];
	t[0] = d0;
	t[1] = d1;
	t[2] = d2;
	t[3] = d3;
	*wp = (*wp + 1) % RING_N;
}

static void doorbell(unsigned k)
{
	uint32_t *ring = k ? rx_ring : tx_ring;
	unsigned wp = k ? rx_wp : tx_wp;
	__asm__ volatile("dsb sy" ::: "memory");
	wr(CH_DB(k), (uint32_t)pa(&ring[wp * 4]));
}

/* Wait for a transfer-completion event of channel chid; recycle consumed events. */
static int trace = 1;	/* first transfer: one screen line per step */

static int wait_event(unsigned chid)
{
	uint64_t end = ticks() + 100ull * TICKS_PER_MS;
	if (trace) {
		mdelay(20);
		con_str("t rd", "reading EV RP...");
		con_hex2("t EV", "rp=", rd(EV_CTX + CTX_RP_LO), "ctx=", rd(EV_CTX));
		con_str("t rd", "reading CH state...");
		con_hex2("t CH", "tx=", rd(CH_CTX(0)), "txrp=", rd(CH_CTX(0) + CTX_RP_LO));
		con_hex2("t IRQ", "type=", rd(TYPE_IRQ), "glob=", rd(GLOB_IRQ_STTS));
		con_hex2("t ERR", "log=", rd(ERROR_LOG), "gpii=", rd(GPII_IRQ_STTS));
		con_hex2("t EVM", "w2=", ev_ring[2], "w3=", ev_ring[3]);
		trace = 0;
	}
	while (ticks() < end) {
		uintptr_t hw = rd(EV_CTX + CTX_RP_LO);
		while (hw != pa(&ev_ring[ev_rp * 4])) {
			uint32_t *e = &ev_ring[ev_rp * 4];
			for (int i = 0; i < 4; i++)
				last_ev[i] = e[i];
			ev_rp = (ev_rp + 1) % EV_N;
			ev_wp = (ev_wp + 1) % EV_N;
			wr(EV_DB, (uint32_t)pa(&ev_ring[ev_wp * 4]));
			wr(SRC_IEOB_CLR, 1);
			uint32_t type = (last_ev[3] >> 16) & 0xff, ch = last_ev[3] >> 24;
			uint32_t code = last_ev[2] >> 24;
			if (type == 0x22 && ch == chid)
				return (code == 1 || code == 2) ? 0 : E_IO;
			hw = rd(EV_CTX + CTX_RP_LO);
		}
		udelay(10);
	}
	return E_TIME;
}

/* CONFIG0 for 100 kHz from 19.2 MHz (Linux geni_i2c_clk_map_19p2mhz): div 7, high 10, low 12, cycle 26 */
static void put_config(void)
{
	put_tre(tx_ring, &tx_wp, 12u | 10u << 8 | 26u << 16 | 1u << 24 | 1u << 25, 0, 7u,
	    TRE_CONFIG0 << 16 | F_CHAIN);
}

static int gpi_write(const uint8_t *data, unsigned n, int stretch)
{
	for (unsigned i = 0; i < n; i++)
		xbuf[i] = data[i];
	put_config();
	put_tre(tx_ring, &tx_wp, I2C_WR | (uint32_t)RMI_ADDR << 8 | (stretch ? 1u << 26 : 0), 0, n,
	    TRE_GO << 16 | F_CHAIN);
	put_tre(tx_ring, &tx_wp, (uint32_t)pa(xbuf), (uint32_t)(pa(xbuf) >> 32), n, TRE_DMA << 16 | F_IEOT);
	if (trace) {
		con_hex2("t TRE", "ring=", (uint32_t)pa(tx_ring), "wp=", tx_wp);
		con_hex2("t TRE0", "w0=", tx_ring[0], "w3=", tx_ring[3]);
		con_hex2("t TRE1", "w0=", tx_ring[4], "w3=", tx_ring[7]);
		con_hex2("t TRE2", "w0=", tx_ring[8], "w3=", tx_ring[11]);
	}
	doorbell(0);
	if (trace) {
		/*
		 * t04 stopped while drawing the next line: capture the fault state
		 * first, without touching the framebuffer, then print it all.
		 */
		mdelay(500);
		uint32_t gfsr = rd(SMMU + 0x48), syn0 = rd(SMMU + 0x50), syn1 = rd(SMMU + 0x54);
		uint32_t syn2 = rd(SMMU + 0x58);
		uint32_t fsr = smmu_cb_base ? rd(smmu_cb_base + 0x58) : 0;
		uint32_t far = smmu_cb_base ? rd(smmu_cb_base + 0x60) : 0;
		uint32_t fsyn = smmu_cb_base ? rd(smmu_cb_base + 0x68) : 0;
		uint32_t evrp = rd(EV_CTX + CTX_RP_LO), txrp = rd(CH_CTX(0) + CTX_RP_LO);
		uint32_t txs = rd(CH_CTX(0)), err = rd(ERROR_LOG);
		con_str("t DB", "doorbell + 0.5 s, then:");
		con_hex2("t GF", "sr=", gfsr, "syn0=", syn0);
		con_hex2("t GF", "syn1=", syn1, "syn2=", syn2);
		con_hex2("t CB", "fsr=", fsr, "far=", far);
		con_hex2("t CB", "fsyn0=", fsyn, 0, 0);
		con_hex2("t GPI", "evrp=", evrp, "txrp=", txrp);
		con_hex2("t GPI", "tx=", txs, "err=", err);
	}
	return wait_event(0);
}

#ifndef TOUCH_BB
/* RMI4 register read on the current page: pointer write (held bus), then read. */
static int rmi_read(uint8_t reg, uint8_t *buf, unsigned len)
{
	if (len > sizeof(xbuf) - 64)
		return E_IO;
	int err = gpi_write(&reg, 1, 1);
	if (err)
		return err;
	uint8_t *rx = xbuf + 64;
	put_tre(rx_ring, &rx_wp, (uint32_t)pa(rx), (uint32_t)(pa(rx) >> 32), len, TRE_DMA << 16 | F_IEOT);
	doorbell(1);
	put_tre(tx_ring, &tx_wp, I2C_RD | (uint32_t)RMI_ADDR << 8, 0, len, TRE_GO << 16 | F_LINK);
	doorbell(0);
	if ((err = wait_event(1)))
		return err;
	for (unsigned i = 0; i < len; i++)
		buf[i] = rx[i];
	return 0;
}

static int rmi_write1(uint8_t reg, uint8_t v)
{
	uint8_t b[2] = { reg, v };
	return gpi_write(b, 2, 0);
}
#else
/*
 * t02-t07: every GSI transfer halted the whole system (secure side), even
 * with GPI's stream routed like the eMMC's. TOUCH_BB: bit-banged I2C on the
 * same pins as plain GPIOs, open drain: a line is driven low (output 0) or
 * released (input; board/internal pull-up). ~50 kHz, clock stretching honoured.
 */
#define BB_HALF_US	10

static void bb_low(unsigned pin)
{
	wr(GPIO_IO(pin), rd(GPIO_IO(pin)) & ~2u);
	wr(GPIO_CFG(pin), (rd(GPIO_CFG(pin)) & ~0x3ffu) | 3u | (1u << 6) | 0x200u);	/* out, pull-up, 4 mA */
}

static void bb_release(unsigned pin)
{
	wr(GPIO_CFG(pin), (rd(GPIO_CFG(pin)) & ~0x3ffu) | 3u | (1u << 6));		/* input, pull-up */
}

static int bb_read(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static int bb_scl_high(void)
{
	bb_release(PIN_SCL);
	for (unsigned i = 0; i < 2000 && !bb_read(PIN_SCL); i++)	/* clock stretching, 20 ms */
		udelay(10);
	return bb_read(PIN_SCL);
}

static void bb_start(void)
{
	bb_release(PIN_SDA);
	bb_scl_high();
	udelay(BB_HALF_US);
	bb_low(PIN_SDA);
	udelay(BB_HALF_US);
	bb_low(PIN_SCL);
	udelay(BB_HALF_US);
}

static void bb_stop(void)
{
	bb_low(PIN_SDA);
	udelay(BB_HALF_US);
	bb_scl_high();
	udelay(BB_HALF_US);
	bb_release(PIN_SDA);
	udelay(BB_HALF_US);
}

/* returns 0 on ACK */
static int bb_write_byte(uint8_t b)
{
	for (int i = 7; i >= 0; i--) {
		if (b & (1u << i))
			bb_release(PIN_SDA);
		else
			bb_low(PIN_SDA);
		udelay(BB_HALF_US);
		bb_scl_high();
		udelay(BB_HALF_US);
		bb_low(PIN_SCL);
	}
	bb_release(PIN_SDA);
	udelay(BB_HALF_US);
	bb_scl_high();
	int nack = bb_read(PIN_SDA);
	udelay(BB_HALF_US);
	bb_low(PIN_SCL);
	return nack;
}

static uint8_t bb_read_byte(int ack)
{
	uint8_t b = 0;
	bb_release(PIN_SDA);
	for (int i = 7; i >= 0; i--) {
		udelay(BB_HALF_US);
		bb_scl_high();
		if (bb_read(PIN_SDA))
			b |= 1u << i;
		udelay(BB_HALF_US);
		bb_low(PIN_SCL);
	}
	if (ack)
		bb_low(PIN_SDA);
	udelay(BB_HALF_US);
	bb_scl_high();
	udelay(BB_HALF_US);
	bb_low(PIN_SCL);
	bb_release(PIN_SDA);
	return b;
}

static void bb_bus_init(void)
{
	bb_release(PIN_SDA);
	bb_release(PIN_SCL);
	udelay(100);
	con_hex2("BB I2C", "sda=", bb_read(PIN_SDA), "scl=", bb_read(PIN_SCL));
	/* free a slave holding SDA low: up to 9 clocks, then STOP */
	for (int i = 0; i < 9 && !bb_read(PIN_SDA); i++) {
		bb_low(PIN_SCL);
		udelay(BB_HALF_US);
		bb_scl_high();
		udelay(BB_HALF_US);
	}
	bb_stop();
}

static int rmi_read(uint8_t reg, uint8_t *buf, unsigned len)
{
	bb_start();
	if (bb_write_byte(RMI_ADDR << 1) || bb_write_byte(reg)) {
		bb_stop();
		return E_NACK;
	}
	bb_start();				/* repeated start */
	if (bb_write_byte(RMI_ADDR << 1 | 1)) {
		bb_stop();
		return E_NACK;
	}
	for (unsigned i = 0; i < len; i++)
		buf[i] = bb_read_byte(i + 1 < len);
	bb_stop();
	return 0;
}

static int rmi_write1(uint8_t reg, uint8_t v)
{
	bb_start();
	int err = bb_write_byte(RMI_ADDR << 1) || bb_write_byte(reg) || bb_write_byte(v);
	bb_stop();
	return err ? E_NACK : 0;
}
#endif

static void hexline(const char *label, const uint8_t *b, unsigned n)
{
	static const char hx[] = "0123456789ABCDEF";
	char s[3 * 16 + 1];
	unsigned i;
	for (i = 0; i < n && i < 16; i++) {
		s[3 * i] = hx[b[i] >> 4];
		s[3 * i + 1] = hx[b[i] & 15];
		s[3 * i + 2] = ' ';
	}
	s[3 * i] = 0;
	con_str(label, s);
}

/*
 * PMIC arbiter v5 (SPMI 0xc440000): touch 1.8 V = PM660 (SID 0) GPIO 12,
 * peripheral 0xcb (base 0xcb00). Reads go through the EE0 observer
 * channel, writes through the APID's RW channel, as in Linux
 * spmi-pmic-arb.c. Only GPIO 12's own registers are written, and only
 * after its TYPE register reads back 0x10 (GPIO) and the APID is owned
 * by the apps EE.
 */
#define SPMI_CORE	0x0c440000UL
#define SPMI_CHNLS	0x0c600000UL
#define SPMI_OBS	0x0e600000UL
#define SPMI_CNFG	0x0c40a000UL
#define TS_PPID		0x0cbu		/* SID 0, peripheral 0xcb */
#define SPMI_NONE	0xffffu

static int spmi_cmd(uintptr_t ch, uint32_t cmd, uint32_t *status)
{
	wr(ch, cmd);
	for (unsigned i = 0; i < 1000; i++) {
		uint32_t s = rd(ch + 0x08);
		if (s & 1u) {
			*status = s;
			return (s & 0x0eu) ? -1 : 0;
		}
		udelay(1);
	}
	*status = 0xdead;
	return -1;
}

static int spmi_rd(unsigned apid, uint8_t reg, uint8_t *v)
{
	uintptr_t ch = SPMI_OBS + 0x80u * apid;
	uint32_t s;
	if (spmi_cmd(ch, (1u << 27) | ((uint32_t)reg << 4), &s))	/* EXT_READL */
		return -1;
	*v = (uint8_t)rd(ch + 0x18);
	return 0;
}

static int spmi_wr(unsigned apid, uint8_t reg, uint8_t v)
{
	uintptr_t ch = SPMI_CHNLS + 0x10000u * apid;
	uint32_t s;
	wr(ch + 0x10, v);
	if (spmi_cmd(ch, ((uint32_t)reg << 4), &s)) {			/* EXT_WRITEL */
		con_hex2("SPMI wr", "reg=", reg, "st=", s);
		return -1;
	}
	return 0;
}

static unsigned spmi_apid(unsigned ppid, uint32_t *owner)
{
	uint32_t n = rd(SPMI_CORE + 4) & 0x7ffu;
	unsigned found = SPMI_NONE;
	for (uint32_t i = 0; i < n && i < 512; i++) {
		uint32_t map = rd(SPMI_CORE + 0x900 + 4 * i);
		if (!map || ((map >> 8) & 0xfffu) != ppid)
			continue;
		uint32_t o = rd(SPMI_CNFG + 0x700 + 4 * i) & 7u;
		if (found == SPMI_NONE || o == 0) {
			found = i;
			*owner = o;
		}
	}
	return found;
}

static void ts_gpio_dump(unsigned apid, const char *label)
{
	uint8_t st = 0, mode = 0, src = 0, en = 0;
	spmi_rd(apid, 0x08, &st);
	spmi_rd(apid, 0x40, &mode);
	spmi_rd(apid, 0x44, &src);
	spmi_rd(apid, 0x46, &en);
	con_hex2(label, "st/mode=", (uint32_t)st << 8 | mode, "src/en=", (uint32_t)src << 8 | en);
}

/* PM660 GPIO 12 -> output high (digital, constant source, inverted) */
static int ts_power_on(void)
{
	uint32_t ver = rd(SPMI_CORE), owner = 7;
	unsigned apid = spmi_apid(TS_PPID, &owner);
	con_hex2("SPMI", "ver=", ver, "apid/own=", apid << 8 | owner);
	if (ver < 0x50000000u || ver >= 0x70000000u || apid == SPMI_NONE)
		return 0;
	uint8_t type = 0, sub = 0;
	if (spmi_rd(apid, 0x04, &type) || spmi_rd(apid, 0x05, &sub) || type != 0x10) {
		con_hex2("PMGPIO12", "type=", type, "sub=", sub);
		return 0;
	}
	con_hex2("PMGPIO12", "type=", type, "sub=", sub);
	ts_gpio_dump(apid, " before");
	if (owner != 0) {
		con_str("PMGPIO12", "not owned by apps EE: no write");
		return 0;
	}
	int lvmv = sub >= 0x10;
	int err = 0;
	if (lvmv) {
		err |= spmi_wr(apid, 0x40, 0x01);	/* mode: digital out */
		err |= spmi_wr(apid, 0x44, 0x80);	/* invert, source 0 = high */
	} else {
		err |= spmi_wr(apid, 0x40, 0x11);	/* mode out, func 0, invert */
	}
	err |= spmi_wr(apid, 0x46, 0x80);		/* enable */
	ts_gpio_dump(apid, " after");
	return !err;
}

void test_touch(void)
{
	con_str("TOUCH", "RMI4 0x20 on i2c9 (SE @ A84000)");
#ifdef TOUCH_SMMU_DUMP
	smmu_dump();
	return;
#endif
#ifndef TOUCH_BB
	smmu_probe(0x6d6);
	if (!smmu_route_cb0(0x6d6))
		return;
	smmu_probe(0x6d6);
	if (!clocks() || !gpi_init())
		return;
#endif
	con_hex2("TLMM", "rst=", rd(GPIO_CFG(PIN_RESET)), "irq=", rd(GPIO_CFG(PIN_IRQ)));
	con_hex2(" i2c", "sda=", rd(GPIO_CFG(PIN_SDA)), "scl=", rd(GPIO_CFG(PIN_SCL)));
#ifdef TOUCH_BB
	if (!ts_power_on())
		return;
	mdelay(20);
#endif
	gpio(PIN_IRQ, 0, 3, 1, 0, 0);		/* input, pull-up */
#ifdef TOUCH_BB
	bb_bus_init();				/* gpio6/7 as plain GPIOs */
	if (!bb_read(PIN_SDA) || !bb_read(PIN_SCL)) {
		con_str("TOUCH", "SDA/SCL still low: bus unpowered, no probe");
		return;
	}
#else
	gpio(PIN_SDA, 1, 0, 0, 0, 0);		/* qup9, 2 mA, no pull */
	gpio(PIN_SCL, 1, 0, 0, 0, 0);
#endif
	gpio(PIN_RESET, 0, 0, 1, 1, 0);		/* reset low */
	mdelay(10);
	gpio(PIN_RESET, 0, 0, 1, 1, 1);		/* release */
	mdelay(200);				/* syna,startup-delay-ms */

	int err = rmi_write1(0xff, 0);		/* page 0 */
#ifdef TOUCH_BB
	con_hex2("PAGE0", "err=", err, "nack=", err == E_NACK);
	if (err) {
		con_str("TOUCH", "no ACK at 0x20: IC unpowered? (PM660 GPIO12)");
		return;
	}
#else
	con_hex2("PAGE0", "err=", err, "ev3=", last_ev[3]);
	if (err) {
		gpi_dump(err == E_TIME ? "page select: no completion event" : "page select failed");
		return;
	}
#endif
	/*
	 * Page Description Table (Linux rmi_driver.c): entries of 6 bytes read
	 * forward from 0xe9 (query, cmd, ctrl, data base, irq count/version,
	 * function number), the next one 6 bytes lower, until function 0/0xff.
	 */
	uint8_t f12q = 0, f12d = 0, f01q = 0;
	for (unsigned a = 0xe9; a >= 0x0a; a -= 6) {
		uint8_t e[6];
		if ((err = rmi_read((uint8_t)a, e, 6)))
			break;
		if (e[5] == 0 || e[5] == 0xff)
			break;
		char lbl[] = "F00";
		lbl[1] = "0123456789ABCDEF"[e[5] >> 4];
		lbl[2] = "0123456789ABCDEF"[e[5] & 15];
		con_hex2(lbl, "q/c/d/cmd=", (uint32_t)e[0] << 24 | (uint32_t)e[2] << 16 | (uint32_t)e[3] << 8 | e[1],
		    "irq/ver=", e[4]);
		if (e[5] == 0x12) {
			f12q = e[0];
			f12d = e[3];
		}
		if (e[5] == 0x01)
			f01q = e[0];
	}
	if (f01q) {
		uint8_t q[21];
		if (!rmi_read(f01q, q, sizeof(q))) {
			char id[11];
			for (int i = 0; i < 10; i++)
				id[i] = (q[11 + i] >= 0x20 && q[11 + i] < 0x7f) ? (char)q[11 + i] : '.';
			id[10] = 0;
			con_str("F01 id", id);
		}
	}
	if (!f12d) {
		con_str("TOUCH", "no F12 on page 0");
		return;
	}
	uint8_t q[32];
	if (!rmi_read(f12q, q, 16))
		hexline("F12 q", q, 16);
	if (!rmi_read(f12q + 16, q, 16))
		hexline("F12 q+", q, 16);
	con_str("TOUCH", "touch the screen: F12 data live 90 s");
	con_skip();
	/* Live rows: re-draw the same three lines. */
	int row = con_get_row();
	uint8_t d[24];
	uint64_t end = ticks() + 90ull * 1000 * TICKS_PER_MS;
	for (uint32_t n = 0; ticks() < end; n++) {
		err = rmi_read(f12d, d, sizeof(d));
		con_set_row(row);
		if (err) {
			con_hex2("F12 d", "err=", err, "n=", n);
		} else {
			hexline("F12 d0", d, 12);
			hexline("F12 d12", d + 12, 12);
			con_hex2("finger0", "x=", d[1] | d[2] << 8, "y=", d[3] | d[4] << 8);
		}
		mdelay(50);
	}
}
