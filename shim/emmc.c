/*
 * sargo stage 1: read-only look at the eMMC host (sdhc_1, SDHCI v5 in the
 * sdhci-msm wrapper) as ABL leaves it. Decides whether XNU can reuse
 * ABL's clocks/card state the way the UFS driver does on sweet.
 *
 * Nothing is written. If ABL gated the SDCC1 clocks, the first read can
 * hang the bus: it runs last, after everything else is on screen.
 */
#include <stdint.h>
#include "board.h"
#include "console.h"
#include "ufs.h"

#define SDHCI_DMA_ADDR		0x00
#define SDHCI_BLOCK_SIZE	0x04	/* 16-bit size, 16-bit count */
#define SDHCI_ARGUMENT		0x08
#define SDHCI_TRANSFER_MODE	0x0c	/* + command at 0x0e */
#define SDHCI_RESPONSE		0x10
#define SDHCI_PRESENT_STATE	0x24
#define SDHCI_HOST_CONTROL	0x28	/* host ctl, power, block gap, wakeup */
#define SDHCI_CLOCK_CONTROL	0x2c	/* clock, timeout, sw reset */
#define SDHCI_INT_STATUS	0x30
#define SDHCI_INT_ENABLE	0x34
#define SDHCI_HOST_CONTROL2	0x3c	/* + acmd12 err at 0x3c lo */
#define SDHCI_CAPABILITIES	0x40
#define SDHCI_CAPABILITIES_1	0x44
#define SDHCI_ADMA_ADDRESS	0x58
#define SDHCI_HOST_VERSION	0xfc	/* slot int status lo, version hi */

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }

void test_emmc(void)
{
#ifdef SDHC1_BASE
	uintptr_t h = SDHC1_BASE;

	con_str("EMMC", "sdhc_1 @ 007C4000 (read only)");
	uint32_t ver = rd32(h + SDHCI_HOST_VERSION);
	con_hex2("SDHCI", "ver=", ver >> 16, "slot=", ver & 0xffff);
	con_hex2("CAPS", "lo=", rd32(h + SDHCI_CAPABILITIES), "hi=", rd32(h + SDHCI_CAPABILITIES_1));
	con_hex2("PRESENT", "st=", rd32(h + SDHCI_PRESENT_STATE), "hctl=", rd32(h + SDHCI_HOST_CONTROL));
	con_hex2("CLOCK", "ctl=", rd32(h + SDHCI_CLOCK_CONTROL), "hctl2=", rd32(h + SDHCI_HOST_CONTROL2));
	con_hex2("INT", "st=", rd32(h + SDHCI_INT_STATUS), "en=", rd32(h + SDHCI_INT_ENABLE));
	con_hex2("XFER", "blk=", rd32(h + SDHCI_BLOCK_SIZE), "mode=", rd32(h + SDHCI_TRANSFER_MODE));
	con_hex2("LASTCMD", "arg=", rd32(h + SDHCI_ARGUMENT), "rsp0=", rd32(h + SDHCI_RESPONSE));
	con_hex2("ADMA", "lo=", rd32(h + SDHCI_ADMA_ADDRESS), "hi=", rd32(h + SDHCI_ADMA_ADDRESS + 4));
	/* CQHCI (cmdq) block that follows hc_mem: version + config, is it enabled? */
	con_hex2("CQHCI", "ver=", rd32(h + 0x1000), "cfg=", rd32(h + 0x1008));
#else
	con_str("EMMC", "no eMMC on this board");
#endif
}
