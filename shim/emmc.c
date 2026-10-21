/*
 * sargo: the eMMC host (sdhc_1, SDHCI 3.0 in the sdhci-msm v5 wrapper).
 *
 * Stage 1 showed that ABL leaves the host clocked but deinitialised (SD
 * clock off, bus power 0, host control and IRQ enables cleared). This
 * re-initialises the card the slow, simple way and reads only:
 * CMD0/1/2/3/9/7, EXT_CSD (CMD8), GPT header + entries (CMD17).
 * No CMD6 and no writes: nothing on the card changes.
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
#define SDHCI_HOST_VERSION	0xfc

/* sdhci-msm v5 ("mci removed") vendor registers, in hc space */
#define MSM_PWRCTL_STATUS	0xdc
#define MSM_PWRCTL_MASK		0xe0
#define MSM_PWRCTL_CLEAR	0xe4
#define MSM_PWRCTL_CTL		0xe8
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
static uint32_t blk[128];	/* one 512-byte block */

/* <= 400 kHz for identification, then legacy speed (<= 26 MHz) */
#define DIV_IDENT		1023	/* f = base / (2 * N) */
#define DIV_XFER		8	/* <= 24 MHz even if ABL left 384 MHz (HS400) */

static int set_clock(unsigned n)
{
	uint32_t c = rd32(H + SDHCI_CLOCK_CONTROL);
	uint32_t keep = c & 0x00ff0000;		/* timeout control; reset byte stays 0 */
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

/* Bus power on raises the msm power IRQ; ack it like sdhci_msm_handle_pwr_irq. */
static uint32_t power_on(void)
{
	uint32_t hc = rd32(H + SDHCI_HOST_CONTROL) & ~0xff00U;
	wr32(H + SDHCI_HOST_CONTROL, hc | (0x0d << 8));	/* 3.0 V | bus power on, 1-bit */
	uint32_t seen = 0;
	uint64_t end = ticks() + 50 * TICKS_PER_MS;
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
		if (st & PWRCTL_BUS_ON)
			break;
	}
	return seen;
}

static int cmd(unsigned idx, uint32_t arg, unsigned flags, unsigned xfer)
{
	uint64_t end = ticks() + 500 * TICKS_PER_MS;
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
			break;
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

/* One 512-byte block into blk[] via the PIO buffer port (CMD8 or CMD17). */
static int read_block(unsigned idx, uint32_t arg)
{
	wr32(H + SDHCI_BLOCK_SIZE, 1U << 16 | 512);
	if (cmd(idx, arg, R1 | CMD_DATA, XFER_READ))
		return -1;
	uint64_t end = ticks() + 500 * TICKS_PER_MS;
	while (!(rd32(H + SDHCI_INT_STATUS) & (INT_READ_READY | INT_ERROR)))
		if (ticks() > end)
			return -2;
	if (rd32(H + SDHCI_INT_STATUS) & INT_ERROR) {
		last_int = rd32(H + SDHCI_INT_STATUS);
		return -3;
	}
	wr32(H + SDHCI_INT_STATUS, INT_READ_READY);
	for (int i = 0; i < 128; i++)
		blk[i] = rd32(H + SDHCI_BUFFER);
	while (!(rd32(H + SDHCI_INT_STATUS) & (INT_XFER_DONE | INT_ERROR)))
		if (ticks() > end)
			return -4;
	wr32(H + SDHCI_INT_STATUS, INT_XFER_DONE);
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
	con_hex2("PWRCTL", "irq=", power_on(), "hctl=", rd32(H + SDHCI_HOST_CONTROL));
	wr32(H + SDHCI_INT_ENABLE, 0xfffffeff);	/* status bits on, card IRQ off */
	wr32(H + SDHCI_SIGNAL_ENABLE, 0);		/* polled: nothing to the GIC */
	if (set_clock(DIV_IDENT))
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
	con_hex2("CID", "mid=", r[3] >> 16 & 0xff, "r3=", r[3]);
	con_str(" name", pnm);

	if (cmd(3, RCA << 16, R1, 0))
		return fail("CMD3", -1);
	if (cmd(9, RCA << 16, R2, 0))
		return fail("CMD9", -1);
	if (cmd(7, RCA << 16, R1B, 0))
		return fail("CMD7", -1);
	if (set_clock(DIV_XFER))
		return fail("CLOCK2", -1);

	int rc = read_block(8, 0);
	if (rc)
		return fail("EXTCSD", rc);
	uint32_t sec = le32(212);
	con_hex2("EXTCSD", "rev=", b8(192), "sectors=", sec);
	con_hex2(" cfg", "part=", b8(179), "cmdq=", b8(15));
	con_hex2(" size", "MiB=", sec / 2048, "bootsz=", b8(226));

	rc = read_block(17, 1);
	if (rc)
		return fail("LBA1", rc);
	if (le32(0) != 0x20494645 || le32(4) != 0x54524150)	/* "EFI PART" */
		return fail("GPT sig", (int)le32(0));
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
