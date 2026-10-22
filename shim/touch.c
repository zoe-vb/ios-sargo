/*
 * sargo touch bring-up in the shim (build with BRINGUP_TOUCH): Synaptics
 * RMI4 at 0x20 on i2c9 = QUP wrapper 1 SE1 at 0xa84000, the same engine,
 * GCC clock registers and SDA/SCL pins (gpio6/7, function 1) as sweet's
 * touch bus. Reset is gpio99, IRQ gpio125 (Linux sdm670-google-common).
 * GENI sequences follow xnu bsd/dev/sweet_touch.c (Linux geni-i2c).
 *
 * Writes only: GCC clock vote/RCG, TLMM pin config, GENI registers, and on
 * the touch IC the RMI4 page select register (0xff = 0). No PMIC writes:
 * if the IC does not answer, its 1.8 V (PM660 GPIO 12) is probably off.
 *
 * MMU off: aligned 32-bit accesses only (Device memory).
 */
#include <stdint.h>
#include "board.h"
#include "console.h"

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

static int se_init(void)
{
	uint32_t fw = rd(SE_PA + SE_FW), tx = rd(SE_PA + SE_HW_TX), rx = rd(SE_PA + SE_HW_RX);
	con_hex2("SE", "fw=", fw, "status=", rd(SE_PA + SE_STATUS));
	con_hex2(" fifo", "tx=", tx, "rx=", rx);
	if (((fw >> 8) & 0xff) != 3 || (rd(SE_PA + 0x64) & 1) ||
	    ((tx >> 24) & 0x3f) != 32 || ((rx >> 24) & 0x3f) != 32 ||
	    ((tx >> 16) & 0x3f) < 2 || ((rx >> 16) & 0x3f) < 2) {
		con_str("SE", "no I2C firmware / FIFO");
		return 0;
	}
	if (rd(SE_PA + SE_STATUS) & 0x1001) {
		con_str("SE", "busy");
		return 0;
	}
	wr(SE_PA + 0xe18, 0);
	wr(SE_PA + 0xe1c, 0);
	wr(SE_PA + 0x644, 0);
	wr(SE_PA + SE_IRQ_CLR, 0xffffffffu);
	wr(SE_PA + 0x648, 0xffffffffu);
	wr(SE_PA + SE_DMA, 0);
	wr(SE_PA + 0x28, rd(SE_PA + 0x28) | 0x7f);
	wr(SE_PA + 0xe30, rd(SE_PA + 0xe30) | 0x0f);
	wr(SE_PA + 0x24, 0x7f);
	wr(SE_PA + 0x20, 1);
	wr(SE_PA + SE_CLK_SEL, 0);
	wr(SE_PA + SE_MCLK, (7u << 4) | 1u);
	wr(SE_PA + SE_SCL, (10u << 20) | (11u << 10) | 26u);	/* 100 kHz from 19.2 MHz */
	wr(SE_PA + 0x254, 0);
	wr(SE_PA + 0x260, SE_PACK0);
	wr(SE_PA + 0x264, SE_PACK1);
	wr(SE_PA + 0x284, SE_PACK0);
	wr(SE_PA + 0x288, SE_PACK1);
	wr(SE_PA + SE_TX_WM, 0);
	wr(SE_PA + 0x810, 0);
	wr(SE_PA + 0x814, ((rx >> 16) & 0x3f) - 1);
	wr(SE_PA + SE_IRQ_EN, SE_EVENTS);
	if (rd(SE_PA + 0x260) != SE_PACK0 || rd(SE_PA + 0x284) != SE_PACK0 ||
	    (rd(SE_PA + SE_TX_COUNT) & 0xfffffff) || (rd(SE_PA + SE_RX_COUNT) & 0x1ffffff)) {
		con_str("SE", "packing/FIFO check failed");
		return 0;
	}
	return 1;
}

static uint32_t last_irq;

static void se_abort(void)
{
	wr(SE_PA + SE_CTRL, 2);
	for (unsigned i = 0; i < 5000 && !(rd(SE_PA + SE_IRQ) & (1u << 5)); i++)
		udelay(1);
	wait_clear(SE_PA + SE_STATUS, 1);
	for (unsigned i = 0; i < 64 && (rd(SE_PA + SE_RX_COUNT) & 0x1ffffff); i++)
		(void)rd(SE_PA + SE_RX);
	wr(SE_PA + SE_IRQ_CLR, 0xffffffffu);
}

/* One I2C message: write nt (<= 3) bytes, or read nr (<= 90) bytes. */
static int msg(const uint8_t *tx, unsigned nt, uint8_t *rx, unsigned nr, int stretch)
{
	unsigned sent = 0, got = 0;
	if (!wait_clear(SE_PA + SE_STATUS, 1))
		return E_IO;
	wr(SE_PA + SE_IRQ_CLR, 0xffffffffu);
	wr(SE_PA + SE_TX_WM, nt ? 1 : 0);
	wr(SE_PA + (nr ? SE_RX_LEN : SE_TX_LEN), nr ? nr : nt);
	wr(SE_PA + SE_CMD, ((nr ? 2u : 1u) << 27) | ((uint32_t)RMI_ADDR << 9) | (stretch ? 4u : 0u));
	for (unsigned i = 0; i < 20000; i++) {
		uint32_t irq = last_irq = rd(SE_PA + SE_IRQ);
		if (irq & SE_ERRORS) {
			int err = (irq & SE_NACK) ? E_NACK : E_IO;
			for (unsigned j = 0; j < 5000 && !(irq & SE_DONE); j++) {
				udelay(1);
				irq = rd(SE_PA + SE_IRQ);
			}
			if ((irq & SE_DONE) && !(rd(SE_PA + SE_STATUS) & 1)) {
				wr(SE_PA + SE_IRQ_CLR, irq);
				return err;
			}
			se_abort();
			return err;
		}
		if (nt && !sent && (irq & SE_TX_WM_IRQ)) {
			uint32_t word = 0;
			for (unsigned j = 0; j < nt; j++)
				word |= (uint32_t)tx[j] << (8 * j);
			wr(SE_PA + SE_TX, word);
			sent = nt;
			wr(SE_PA + SE_TX_WM, 0);
		}
		uint32_t count = rd(SE_PA + SE_RX_COUNT) & 0x1ffffff;
		while (count-- && got < nr) {
			uint32_t word = rd(SE_PA + SE_RX);
			for (unsigned j = 0; j < 4 && got < nr; j++)
				rx[got++] = (uint8_t)(word >> (8 * j));
		}
		if (irq & SE_DONE) {
			wr(SE_PA + SE_IRQ_CLR, irq);
			return (got == nr && sent == nt) ? 0 : E_IO;
		}
		wr(SE_PA + SE_IRQ_CLR, irq & (SE_RX_WM_IRQ | SE_RX_LAST | SE_TX_WM_IRQ));
		udelay(1);
	}
	se_abort();
	return E_TIME;
}

/* RMI4 register read on the current page: pointer write (held bus), then read. */
static int rmi_read(uint8_t reg, uint8_t *buf, unsigned len)
{
	int err = msg(&reg, 1, 0, 0, 1);
	return err ? err : msg(0, 0, buf, len, 0);
}

static int rmi_write1(uint8_t reg, uint8_t v)
{
	uint8_t b[2] = { reg, v };
	return msg(b, 2, 0, 0, 0);
}

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

void test_touch(void)
{
	con_str("TOUCH", "RMI4 0x20 on i2c9 (SE @ A84000)");
	if (!clocks() || !se_init())
		return;
	con_hex2("TLMM", "rst=", rd(GPIO_CFG(PIN_RESET)), "irq=", rd(GPIO_CFG(PIN_IRQ)));
	con_hex2(" i2c", "sda=", rd(GPIO_CFG(PIN_SDA)), "scl=", rd(GPIO_CFG(PIN_SCL)));
	gpio(PIN_IRQ, 0, 3, 1, 0, 0);		/* input, pull-up */
	gpio(PIN_SDA, 1, 0, 0, 0, 0);		/* qup9, 2 mA, no pull */
	gpio(PIN_SCL, 1, 0, 0, 0, 0);
	gpio(PIN_RESET, 0, 0, 1, 1, 0);		/* reset low */
	mdelay(10);
	gpio(PIN_RESET, 0, 0, 1, 1, 1);		/* release */
	mdelay(200);				/* syna,startup-delay-ms */

	int err = rmi_write1(0xff, 0);		/* page 0 */
	con_hex2("PAGE0", "err=", err, "irq=", last_irq);
	if (err) {
		con_str("TOUCH", err == E_NACK ? "NACK: IC unpowered? (PM660 GPIO12)" : "bus error");
		return;
	}
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
