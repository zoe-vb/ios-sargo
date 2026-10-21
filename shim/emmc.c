/*
 * sargo: the eMMC host (sdhc_1, SDHCI 3.0 in the sdhci-msm v5 wrapper).
 *
 * Stage 1 showed that ABL leaves the host clocked but deinitialised (SD
 * clock off, bus power 0, host control and IRQ enables cleared). This
 * re-initialises the card the slow, simple way and reads only:
 * CMD0/1/2/3/9/7, EXT_CSD (CMD8), GPT header + entries (CMD17).
 * No CMD6 and no writes: nothing on the card changes.
 *
 * Boot 2 (2026-10-21) hung on CMD9 with no status bit at all: ABL leaves
 * the SDCC1 apps clock very slow (its deinit probably selects the 400 kHz
 * ident rate), and our /2046 ident divider made the bus ~200 Hz. Like
 * Linux sdhci-msm, set the rate at the GCC RCG and divide little.
 *
 * MMU off: all accesses are aligned 32-bit (Device memory).
 */
#include <stdint.h>
#include "board.h"
#include "console.h"
#include "ufs.h"

#define SDHCI_BLOCK_SIZE	0x04	/* 16-bit size, 16-bit count */
#define SDHCI_ARGUMENT		0x08
#define SDHCI_TRANSFER_MODE	0x0c	/* + command at 0x0e */
#define SDHCI_RESPONSE		0x10
#define SDHCI_BUFFER		0x20
#define SDHCI_PRESENT_STATE	0x24
#define SDHCI_HOST_CONTROL	0x28	/* host ctl, power, block gap, wakeup */
#define SDHCI_CLOCK_CONTROL	0x2c	/* clock, timeout, sw reset */
#define SDHCI_INT_STATUS	0x30
#define SDHCI_INT_ENABLE	0x34
#define SDHCI_SIGNAL_ENABLE	0x38
#define SDHCI_HOST_CONTROL2	0x3c
#define SDHCI_CAPABILITIES	0x40
#define SDHCI_CAPABILITIES_1	0x44
#define SDHCI_ADMA_ERROR	0x54
#define SDHCI_ADMA_ADDRESS	0x58
#define SDHCI_HOST_VERSION	0xfc

/* sdhci-msm v5 ("mci removed") vendor registers, in hc space (sdhci_msm_v5_offset) */
#define MSM_DLL_CONFIG		0x200
#define MSM_VENDOR_SPEC		0x20c
#define MSM_PWRCTL_STATUS	0x240
#define MSM_PWRCTL_MASK		0x244
#define MSM_PWRCTL_CLEAR	0x248
#define MSM_PWRCTL_CTL		0x24c
#define DLL_PDN			(1U << 29)
#define DLL_RST			(1U << 30)
#define HC_MCLK_SEL_DFLT	(2U << 8)
#define HC_MCLK_SEL_MASK	(3U << 8)
#define HC_SELECT_IN_EN		(1U << 18)
#define HC_SELECT_IN_MASK	(7U << 19)
#define VENDOR_SPEC_POR		0xa9c	/* sdhci_msm_probe writes this first */
#define PWRCTL_BUS_OFF		(1U << 0)
#define PWRCTL_BUS_ON		(1U << 1)
#define PWRCTL_IO_LOW		(1U << 2)
#define PWRCTL_IO_HIGH		(1U << 3)
#define PWRCTL_BUS_SUCCESS	(1U << 0)
#define PWRCTL_IO_SUCCESS	(1U << 2)

#define INT_CMD_DONE		(1U << 0)
#define INT_XFER_DONE		(1U << 1)
#define INT_READ_READY		(1U << 5)
#define INT_ERROR		(1U << 15)

#define PS_CMD_INHIBIT		(1U << 0)
#define PS_DAT_INHIBIT		(1U << 1)

/* command register (upper half of 0x0c) */
#define RSP_NONE		0x00
#define RSP_136			0x01
#define RSP_48			0x02
#define RSP_48_BUSY		0x03
#define CMD_CRC			0x08
#define CMD_INDEX		0x10
#define CMD_DATA		0x20
#define R1			(RSP_48 | CMD_CRC | CMD_INDEX)
#define R1B			(RSP_48_BUSY | CMD_CRC | CMD_INDEX)
#define R2			(RSP_136 | CMD_CRC)
#define R3			RSP_48
#define XFER_READ		(1U << 4)
#define XFER_DMA		(1U << 0)
#define HCTL_ADMA2_32		(2U << 3)	/* host control DMA select */
#define INT_ADMA_ERROR		(1U << 25)

/* ADMA2 32-bit descriptor: attr (valid|end|act=tran), length, address */
#define ADMA_VALID		0x01
#define ADMA_END		0x02
#define ADMA_TRAN		0x20

/* GCC (sdm845/sdm670 gcc driver): SDCC1 apps RCG and branch */
#define GCC_BASE		0x00100000UL
#define SDCC1_APPS_CBCR		(GCC_BASE + 0x26004)
#define SDCC1_APPS_RCGR		(GCC_BASE + 0x26028)	/* CMD; CFG +4, M +8, N +0xc, D +0x10 */
#define RCG_UPDATE		(1U << 0)
#define RCG_ROOT_OFF		(1U << 31)

#define OCR_BUSY		(1U << 31)
#define OCR_SECTOR_MODE		0x40000000U
#define OCR_VOLTAGES		0x00ff8080U	/* 2.7-3.6 V + 1.70-1.95 V */
#define RCA			1

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

#define TICKS_PER_MS	19200	/* CNTFRQ 19.2 MHz (stage 1) */

static void mdelay(unsigned ms)
{
	uint64_t end = ticks() + (uint64_t)ms * TICKS_PER_MS;
	while (ticks() < end)
		;
}

#ifdef SDHC1_BASE
#define H	SDHC1_BASE

static uint32_t last_int;
static uint64_t last_ticks;	/* duration of the last cmd() */
static uint32_t base_khz;	/* SDCC1 apps clock = SDHCI base clock */
static uint32_t blk[128] __attribute__((aligned(64)));	/* one 512-byte block (DMA target) */
static uint32_t adma_desc[2] __attribute__((aligned(8)));

/* SDHCI 3.0 divided clock: f = base / (2N), N = 1..1023 (0 = undivided) */
static unsigned div_for(uint32_t khz)
{
	unsigned n = (base_khz + 2 * khz - 1) / (2 * khz);
	if (base_khz <= khz)
		return 0;
	return n > 1023 ? 1023 : n;
}

static uint32_t parent_khz(unsigned src)
{
	switch (src) {
	case 0: return 19200;	/* bi_tcxo */
	case 1: return 600000;	/* gpll0 main */
	case 2: return 384000;	/* gpll6 main */
	case 6: return 300000;	/* gpll0 out even */
	default: return 0;
	}
}

/* rate of the RCG as programmed (clk-rcg2: hid div = 2*div-1, N stored as ~(n-m)) */
static uint32_t rcg_khz(void)
{
	uint32_t cfg = rd32(SDCC1_APPS_RCGR + 4);
	uint32_t p = parent_khz(cfg >> 8 & 7);
	uint32_t hid = cfg & 0x1f;
	uint64_t r = hid ? (uint64_t)p * 2 / (hid + 1) : p;
	if ((cfg >> 12 & 3) == 2) {	/* dual-edge M/N */
		uint32_t m = rd32(SDCC1_APPS_RCGR + 8) & 0xff;
		uint32_t n = ((~rd32(SDCC1_APPS_RCGR + 12)) & 0xff) + m;
		r = n ? r * m / n : 0;
	}
	return (uint32_t)r;
}

/* Read the SDCC1 apps clock; if ABL left it below 1 MHz, set 50 MHz = gpll0_even / 6. */
static int gcc_sdcc1(void)
{
	uint32_t cmdr = rd32(SDCC1_APPS_RCGR), cfg = rd32(SDCC1_APPS_RCGR + 4);
	con_hex2("GCC RCG", "cmd=", cmdr, "cfg=", cfg);
	con_hex2(" cbcr", "apps=", rd32(SDCC1_APPS_CBCR), "m/n=", rd32(SDCC1_APPS_RCGR + 8) << 16 | (rd32(SDCC1_APPS_RCGR + 12) & 0xffff));
	base_khz = rcg_khz();
	if (!parent_khz(cfg >> 8 & 7) || (cmdr & RCG_ROOT_OFF)) {
		con_hex2(" rate", "kHz=", base_khz, "UNSURE=", cmdr);
		return -1;	/* not what the gcc driver describes: do not touch it */
	}
	if (base_khz < 1000) {
		/* gpll0_out_even / 6, no M/N; other CFG bits kept (as clk-rcg2 masks its update) */
		wr32(SDCC1_APPS_RCGR + 4, (cfg & ~0x371fU) | 6 << 8 | (2 * 6 - 1));
		wr32(SDCC1_APPS_RCGR, cmdr | RCG_UPDATE);
		uint64_t end = ticks() + 10 * TICKS_PER_MS;
		while (rd32(SDCC1_APPS_RCGR) & RCG_UPDATE)
			if (ticks() > end)
				return -2;
		uint32_t was = base_khz;
		base_khz = rcg_khz();
		con_hex2(" rate", "was kHz=", was, "now=", base_khz);
	} else {
		con_hex2(" rate", "kHz=", base_khz, "kept=", 1);
	}
	return 0;
}

static int set_clock(unsigned n)
{
	uint32_t c = rd32(H + SDHCI_CLOCK_CONTROL);
	uint32_t keep = 0x0e << 16;		/* max data timeout; reset byte stays 0 */
	wr32(H + SDHCI_CLOCK_CONTROL, keep | (c & 0xffff & ~4U));	/* SD clock off */
	uint32_t clk = (n & 0xff) << 8 | ((n >> 8) & 3) << 6 | 1;	/* divided mode, internal on */
	wr32(H + SDHCI_CLOCK_CONTROL, keep | clk);
	uint64_t end = ticks() + 20 * TICKS_PER_MS;
	while (!(rd32(H + SDHCI_CLOCK_CONTROL) & 2))
		if (ticks() > end)
			return -1;
	wr32(H + SDHCI_CLOCK_CONTROL, keep | clk | 4);	/* SD clock on */
	return 0;
}

/*
 * ABL leaves the msm wrapper in its HS400 setup. Put it back in the default
 * mode like msm_hc_select_default(), and hold the DLL in reset/power-down
 * as sdhci-msm does for bus clocks <= 100 MHz.
 */
static void msm_legacy_mode(void)
{
	uint32_t vs = rd32(H + MSM_VENDOR_SPEC), dll = rd32(H + MSM_DLL_CONFIG);
	con_hex2("MSM was", "vspec=", vs, "dll=", dll);
	vs = (vs & ~(HC_MCLK_SEL_MASK | HC_SELECT_IN_EN | HC_SELECT_IN_MASK)) | HC_MCLK_SEL_DFLT;
	wr32(H + MSM_VENDOR_SPEC, vs);
	wr32(H + MSM_DLL_CONFIG, dll | DLL_RST | DLL_PDN);
	con_hex2("MSM now", "vspec=", rd32(H + MSM_VENDOR_SPEC), "dll=", rd32(H + MSM_DLL_CONFIG));
}

/* Ack msm power IRQs like sdhci_msm_handle_pwr_irq for up to ms; stop early on stop_on. */
static uint32_t pwr_service(unsigned ms, uint32_t stop_on)
{
	uint32_t seen = 0;
	uint64_t end = ticks() + (uint64_t)ms * TICKS_PER_MS;
	while (ticks() < end) {
		uint32_t st = rd32(H + MSM_PWRCTL_STATUS);
		if (!st)
			continue;
		seen |= st;
		wr32(H + MSM_PWRCTL_CLEAR, st);
		uint32_t ack = 0;
		if (st & (PWRCTL_BUS_ON | PWRCTL_BUS_OFF))
			ack |= PWRCTL_BUS_SUCCESS;
		if (st & (PWRCTL_IO_HIGH | PWRCTL_IO_LOW))
			ack |= PWRCTL_IO_SUCCESS;
		wr32(H + MSM_PWRCTL_CTL, ack);
		if (st & stop_on)
			break;
	}
	return seen;
}

/* Bus power on raises the msm power IRQ (BUS_ON). */
static uint32_t power_on(void)
{
	uint32_t hc = rd32(H + SDHCI_HOST_CONTROL) & ~0xff00U;
	wr32(H + SDHCI_HOST_CONTROL, hc | (0x0d << 8));	/* 3.0 V | bus power on, 1-bit */
	return pwr_service(50, PWRCTL_BUS_ON);
}

/* Start from the state Linux probe + sdhci_init leave: vendor spec POR, RESET_ALL. */
static int msm_reset(void)
{
	uint32_t stale = pwr_service(5, 0);	/* probe: handle a pending BUS_ON/IO_HIGH first */
	wr32(H + MSM_VENDOR_SPEC, VENDOR_SPEC_POR);
	uint32_t c = rd32(H + SDHCI_CLOCK_CONTROL) & 0x00ffffff;
	wr32(H + SDHCI_CLOCK_CONTROL, c | 1U << 24);	/* SOFTWARE_RESET_ALL */
	uint64_t end = ticks() + 100 * TICKS_PER_MS;
	while (rd32(H + SDHCI_CLOCK_CONTROL) & (1U << 24))
		if (ticks() > end)
			return -1;
	uint32_t after = pwr_service(20, 0);	/* reset can raise BUS_OFF */
	con_hex2("RESET", "pwr pre=", stale, "post=", after);
	return 0;
}

static int cmd_once(unsigned idx, uint32_t arg, unsigned flags, unsigned xfer)
{
	uint64_t start = ticks(), end = start + 3000 * TICKS_PER_MS;
	uint32_t inhibit = PS_CMD_INHIBIT | ((flags & CMD_DATA) || (flags & 3) == RSP_48_BUSY ? PS_DAT_INHIBIT : 0);
	while (rd32(H + SDHCI_PRESENT_STATE) & inhibit)
		if (ticks() > end) {
			last_int = 0xdead0000 | rd32(H + SDHCI_PRESENT_STATE) >> 16;
			return -1;
		}
	wr32(H + SDHCI_INT_STATUS, 0xffffffff);
	wr32(H + SDHCI_ARGUMENT, arg);
	wr32(H + SDHCI_TRANSFER_MODE, (idx << 8 | flags) << 16 | xfer);
	for (;;) {
		uint32_t st = rd32(H + SDHCI_INT_STATUS);
		if (st & INT_ERROR) {
			last_int = st;
			wr32(H + SDHCI_INT_STATUS, st);
			/* reset CMD + DAT lines so the next command can go */
			uint32_t c = rd32(H + SDHCI_CLOCK_CONTROL) & 0x00ffffff;
			wr32(H + SDHCI_CLOCK_CONTROL, c | 6U << 24);
			while (rd32(H + SDHCI_CLOCK_CONTROL) & (6U << 24))
				;
			return -1;
		}
		if (st & INT_CMD_DONE) {
			wr32(H + SDHCI_INT_STATUS, INT_CMD_DONE);
			last_int = st;
			last_ticks = ticks() - start;
			break;
		}
		/* R1b: one run saw busy-end (transfer complete) but never command complete */
		if ((flags & 3) == RSP_48_BUSY && (st & INT_XFER_DONE) && ticks() - start > 10 * TICKS_PER_MS) {
			wr32(H + SDHCI_INT_STATUS, INT_XFER_DONE);
			last_int = st | 0x40000000;	/* marker: accepted without cmd done */
			last_ticks = ticks() - start;
			return 0;
		}
		if (ticks() > end) {
			last_int = st | 0x80000000;	/* timeout marker */
			return -1;
		}
	}
	if ((flags & 3) == RSP_48_BUSY && !(flags & CMD_DATA)) {
		while (!(rd32(H + SDHCI_INT_STATUS) & (INT_XFER_DONE | INT_ERROR)))
			if (ticks() > end)
				return -1;
		wr32(H + SDHCI_INT_STATUS, INT_XFER_DONE);
	}
	return 0;
}

static int retries;

static int cmd(unsigned idx, uint32_t arg, unsigned flags, unsigned xfer)
{
	for (int i = 0; i < 3; i++) {
		if (!cmd_once(idx, arg, flags, xfer))
			return 0;
		retries++;
		con_hex2(" retry", "cmd=", idx, "int=", last_int);
	}
	return -1;
}

/*
 * One 512-byte block into blk[] (CMD8 or CMD17) by ADMA2.
 * Boots 5/6: on this msm wrapper PIO reads of the buffer port return word 0
 * over and over (the FIFO never advances, transfer-complete never comes).
 * Linux and ABL use ADMA, so do the same. MMU and caches are off: no
 * cache maintenance; the SMMU is assumed in bypass (as for UFS on sweet).
 */
static int read_block(unsigned idx, uint32_t arg)
{
	uintptr_t buf = (uintptr_t)blk;
	if (buf >> 32)
		return -6;	/* 32-bit ADMA2 only */
	for (int i = 0; i < 128; i++)
		blk[i] = 0xdeadbeef;	/* so a DMA that never lands is visible */
	adma_desc[0] = 512U << 16 | ADMA_TRAN | ADMA_END | ADMA_VALID;
	adma_desc[1] = (uint32_t)buf;
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(H + SDHCI_ADMA_ADDRESS, (uint32_t)(uintptr_t)adma_desc);
	wr32(H + SDHCI_ADMA_ADDRESS + 4, 0);
	wr32(H + SDHCI_HOST_CONTROL, (rd32(H + SDHCI_HOST_CONTROL) & ~(3U << 3)) | HCTL_ADMA2_32);
	wr32(H + SDHCI_BLOCK_SIZE, 1U << 16 | 512);
	if (cmd(idx, arg, R1 | CMD_DATA, XFER_READ | XFER_DMA))
		return -1;
	uint64_t end = ticks() + 500 * TICKS_PER_MS;
	uint32_t st;
	while (!((st = rd32(H + SDHCI_INT_STATUS)) & (INT_XFER_DONE | INT_ERROR)))
		if (ticks() > end) {
			last_int = st | 0x80000000;
			return -4;
		}
	if (st & INT_ERROR) {
		last_int = st;
		con_hex2(" adma", "err=", rd32(H + SDHCI_ADMA_ERROR), "ps=", rd32(H + SDHCI_PRESENT_STATE));
		return -3;
	}
	wr32(H + SDHCI_INT_STATUS, INT_XFER_DONE | INT_READ_READY);
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}

static uint8_t b8(int off) { return blk[off / 4] >> (8 * (off & 3)); }
static uint32_t le32(int off) { return blk[off / 4]; }	/* callers pass 4-aligned offsets */

static int fail(const char *what, int rc)
{
	con_hex2(what, "FAIL rc=", (uint32_t)rc, "int=", last_int);
	return -1;
}

/* GPT partition name (UTF-16LE, ASCII subset) of entry e within blk[] -> buf */
static void gpt_name(int e, char *buf)
{
	int i;
	for (i = 0; i < 36; i++) {
		uint8_t ch = b8(e * 128 + 56 + 2 * i);
		if (!ch)
			break;
		buf[i] = ch;
	}
	buf[i] = 0;
}

static int streq8(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static int emmc_read_test(void)
{
	int rc = gcc_sdcc1();
	if (rc)
		return fail("GCC", rc);
	con_hex2("DIV", "ident=", div_for(400), "xfer=", div_for(25000));
	if ((rc = msm_reset()))
		return fail("RESET", rc);
	msm_legacy_mode();
	con_hex2("PWRCTL", "irq=", power_on(), "hctl=", rd32(H + SDHCI_HOST_CONTROL));
	wr32(H + SDHCI_INT_ENABLE, 0xfffffeff);	/* status bits on, card IRQ off */
	wr32(H + SDHCI_SIGNAL_ENABLE, 0);		/* polled: nothing to the GIC */
	if (set_clock(div_for(400)))
		return fail("CLOCK", -1);
	mdelay(10);	/* >= 1 ms + 74 clocks after power-up */

	if (cmd(0, 0, RSP_NONE, 0))
		return fail("CMD0", -1);
	mdelay(2);
	uint32_t ocr = 0;
	uint64_t end = ticks() + 2000 * TICKS_PER_MS;
	int tries = 0;
	do {
		if (cmd(1, OCR_SECTOR_MODE | OCR_VOLTAGES, R3, 0))
			return fail("CMD1", tries);
		ocr = rd32(H + SDHCI_RESPONSE);
		tries++;
		mdelay(5);
	} while (!(ocr & OCR_BUSY) && ticks() < end);
	con_hex2("OCR", "ocr=", ocr, "tries=", tries);
	if (!(ocr & OCR_BUSY))
		return fail("CMD1", -2);

	if (cmd(2, 0, R2, 0))
		return fail("CMD2", -1);
	/* R2 in RESPONSE[0..3] = CID[127:8]; product name PNM = CID[103:56] */
	uint32_t r[4];
	for (int i = 0; i < 4; i++)
		r[i] = rd32(H + SDHCI_RESPONSE + 4 * i);
	char pnm[7];
	for (int i = 0; i < 6; i++) {
		int bit = 56 + 8 * (5 - i) - 8;	/* byte position in the CRC-stripped response */
		char c = (char)(r[bit / 32] >> (bit % 32));
		pnm[i] = c >= 0x20 && c < 0x7f ? c : '.';
	}
	pnm[6] = 0;
	uint32_t t2 = (uint32_t)(last_ticks * 10 / 192);	/* us */
	con_hex2("CID", "mid=", r[3] >> 16 & 0xff, "r3=", r[3]);
	con_str(" name", pnm);

	if (cmd(3, RCA << 16, R1, 0))
		return fail("CMD3", -1);
	con_hex2("TIME us", "cmd2=", t2, "cmd3=", (uint32_t)(last_ticks * 10 / 192));
	if (cmd(9, RCA << 16, R2, 0))
		fail("CMD9", -1);	/* CSD is not needed: size comes from EXT_CSD */
	if (cmd(7, RCA << 16, R1B, 0))
		return fail("CMD7", -1);
	if (set_clock(div_for(25000)))
		return fail("CLOCK2", -1);

	rc = read_block(8, 0);
	if (rc)
		return fail("EXTCSD", rc);
	con_hex2("DMA", "buf=", (uint32_t)(uintptr_t)blk, "w0=", blk[0]);
	uint32_t sec = le32(212);
	con_hex2("EXTCSD", "rev=", b8(192), "sectors=", sec);
	con_hex2(" cfg", "part=", b8(179), "cmdq=", b8(15));
	con_hex2(" size", "MiB=", sec / 2048, "bootsz=", b8(226));

	rc = read_block(17, 1);
	if (rc)
		return fail("LBA1", rc);
	if (le32(0) != 0x20494645 || le32(4) != 0x54524150)	/* "EFI PART" */
		return fail("GPT sig", (int)le32(4));
	uint32_t ent_lba = le32(72), nent = le32(80), esz = le32(84);
	con_hex2("GPT", "ents=", nent, "at lba=", ent_lba);
	if (esz != 128)
		return fail("GPT esz", (int)esz);

	/* walk the entries: count them, print the first and userdata */
	int found = 0, shown = 0;
	char name[37];
	for (uint32_t lba = ent_lba; lba < ent_lba + (nent + 3) / 4; lba++) {
		if ((rc = read_block(17, lba)))
			return fail("GPT ent", rc);
		for (int e = 0; e < 4; e++) {
			if (!le32(e * 128) && !le32(e * 128 + 4))
				continue;	/* unused entry */
			gpt_name(e, name);
			int is_ud = streq8(name, "userdata");
			if (shown < 2 || is_ud) {
				con_str(is_ud ? "USERDATA" : " part", name);
				con_hex2(" lba", "first=", le32(e * 128 + 32), "last=", le32(e * 128 + 40));
				shown++;
			}
			found += is_ud;
		}
	}
	con_str("EMMC", found ? "read OK, userdata found" : "read OK, NO userdata");
	return 0;
}
#endif

void test_emmc(void)
{
#ifdef SDHC1_BASE
	con_str("EMMC", "sdhc_1 @ 007C4000: re-init, read only");
	uint32_t ver = rd32(H + SDHCI_HOST_VERSION);
	con_hex2("SDHCI", "ver=", ver >> 16, "caps=", rd32(H + SDHCI_CAPABILITIES));
	con_hex2("STATE", "ps=", rd32(H + SDHCI_PRESENT_STATE), "clk=", rd32(H + SDHCI_CLOCK_CONTROL));
	con_hex2("PWRCTL", "st=", rd32(H + MSM_PWRCTL_STATUS), "mask=", rd32(H + MSM_PWRCTL_MASK));
	emmc_read_test();
#else
	con_str("EMMC", "no eMMC on this board");
#endif
}
