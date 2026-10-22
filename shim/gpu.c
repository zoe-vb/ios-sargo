/*
 * sargo GPU bring-up in the shim (build with GPU=1 -> BRINGUP_GPU):
 * Adreno 615 (chip id 0x06010500) with its GMU, stock DT /soc/qcom,kgsl-3d0,
 * /soc/qcom,gmu, /soc/qcom,gpucc@5090000, /soc/arm,smmu-kgsl@5040000.
 *
 * t10, read-mostly: GCC GPU clock gates, then the GPU clock controller's
 * GDSCs and GMU clock, then (only once those clocks/power are confirmed on)
 * the GPU SMMU stream routing and the GMU's reset/boot state.
 * Writes: GCC branch enables (cfg AHB, memnoc gfx, snoc dvm, GPLL0 votes),
 * the CX GDSC (SW_COLLAPSE off) and the GPU CC CXO/GMU branch enables -
 * what Linux does before it loads the GMU firmware. Nothing in the GX
 * domain (the GPU core) is touched; its registers are not read.
 *
 * Register offsets: Linux gcc-sdm845.c (also SDM670), gpucc-sdm845.c and
 * registers/adreno/a6xx_gmu.xml (dword offsets from the GPU base).
 *
 * MMU off: aligned 32-bit accesses only (Device memory).
 */
#include <stdint.h>
#include "board.h"
#include "console.h"

#define GCC		0x00100000UL
#define GCC_GPU_CFG_AHB		0x71004		/* gcc_gpu_cfg_ahb_clk */
#define GCC_GPU_MEMNOC_GFX	0x7100c		/* gcc_gpu_memnoc_gfx_clk (voted) */
#define GCC_GPU_SNOC_DVM	0x71018		/* gcc_gpu_snoc_dvm_gfx_clk */
#define GCC_DDRSS_GPU_AXI	0x44038		/* gcc_ddrss_gpu_axi_clk (voted) */
#define GCC_VOTE_1		0x52004		/* bit 15 gpu_gpll0, bit 16 gpu_gpll0_div */

#define GPUCC		0x05090000UL
#define GPUCC_PLL1_MODE		0x0100
#define GPUCC_GX_GDSCR		0x100c
#define GPUCC_CX_GDSCR		0x106c
#define GPUCC_CX_GMU_CBCR	0x1098
#define GPUCC_CXO_CBCR		0x109c
#define GPUCC_GMU_RCG		0x1120		/* cmd; cfg at +4 */
#define GPUCC_CX_GDS_HW_CTRL	0x1540

#define GPU		0x05000000UL
#define GMU_REG(dw)	(GPU + (uintptr_t)(dw) * 4u)
#define GMU_CORE_FW_VERSION	0x1d3f8
#define GMU_CM3_SYSRESET	0x1f800
#define GMU_CM3_BOOT_CONFIG	0x1f801
#define GMU_CM3_FW_BUSY		0x1f81a
#define GMU_CM3_FW_INIT_RESULT	0x1f81c
#define GMU_CM3_CFG		0x1f82d

#define GSMMU		0x05040000UL	/* arm,smmu-kgsl (qcom,smmu-v2) */

#define CLK_OFF		(1u << 31)	/* CBCR status */
#define GDSC_PWR_ON	(1u << 31)
#define GDSC_COLLAPSE	(1u << 0)

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; __asm__ volatile("dsb sy" ::: "memory"); }

static uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static void udelay(unsigned us)
{
	uint64_t end = ticks() + (uint64_t)us * 19 + 1;
	while (ticks() < end)
		;
}

/* poll until (rd(a) & mask) == want, up to ~2 ms */
static int wait_bits(uintptr_t a, uint32_t mask, uint32_t want)
{
	for (unsigned i = 0; i < 2000; i++) {
		if ((rd(a) & mask) == want)
			return 1;
		udelay(1);
	}
	return 0;
}

/* enable a CBCR branch, report whether it runs */
static int branch_on(const char *label, uintptr_t cbcr, int voted)
{
	uint32_t before = rd(cbcr);
	if (!(before & 1u))
		wr(cbcr, before | 1u);
	int ok = voted ? 1 : wait_bits(cbcr, CLK_OFF, 0);
	con_hex2(label, "was=", before, "now=", rd(cbcr) | (ok ? 0 : 0xbad00000u));
	return ok;
}

static void smmu_dump(uintptr_t base)
{
	uint32_t idr0 = rd(base + 0x20), idr1 = rd(base + 0x24);
	unsigned n = idr0 & 0xff, pgsize = (idr1 >> 31) ? 0x10000 : 0x1000;
	unsigned npage = 1u << (((idr1 >> 28) & 7) + 1), shown = 0;
	con_hex2("GSMMU", "scr0=", rd(base), "gfsr=", rd(base + 0x48));
	con_hex2(" ids", "idr0=", idr0, "idr1=", idr1);
	if (n > 128)
		return;
	for (unsigned i = 0; i < n && shown < 24; i++) {
		uint32_t smr = rd(base + 0x800 + 4 * i), s2cr = rd(base + 0xc00 + 4 * i);
		if (!(smr & (1u << 31)) && s2cr == 0)
			continue;
		char lbl[] = " S00";
		lbl[2] = "0123456789ABCDEF"[(i >> 4) & 15];
		lbl[3] = "0123456789ABCDEF"[i & 15];
		con_hex2(lbl, "smr=", smr, "s2cr=", s2cr);
		shown++;
		if (((s2cr >> 16) & 3) == 0 && (smr & (1u << 31))) {
			unsigned cb = s2cr & 0xff;
			con_hex2("  cb", "cbar=", rd(base + pgsize + 4 * cb),
			    "sctlr=", rd(base + (uintptr_t)npage * pgsize + (uintptr_t)cb * pgsize));
			shown++;
		}
	}
	if (!shown)
		con_str(" GSMMU", "no stream match entries in use");
}

void test_gpu(void)
{
	con_str("GPU", "Adreno 615 probe t10 (read-mostly)");
	con_hex2("GCC", "vote1=", rd(GCC + GCC_VOTE_1), "axi=", rd(GCC + GCC_DDRSS_GPU_AXI));
	if (!branch_on(" cfgahb", GCC + GCC_GPU_CFG_AHB, 0)) {
		con_str("GPU", "gpu_cfg_ahb_clk stays off: GPU CC unreachable, stop");
		return;
	}
	wr(GCC + GCC_VOTE_1, rd(GCC + GCC_VOTE_1) | (1u << 15) | (1u << 16));
	branch_on(" memnoc", GCC + GCC_GPU_MEMNOC_GFX, 1);
	branch_on(" snocdvm", GCC + GCC_GPU_SNOC_DVM, 0);
	udelay(10);

	con_hex2("GPUCC", "cx=", rd(GPUCC + GPUCC_CX_GDSCR), "gx=", rd(GPUCC + GPUCC_GX_GDSCR));
	con_hex2(" gmuclk", "cbcr=", rd(GPUCC + GPUCC_CX_GMU_CBCR), "cxo=", rd(GPUCC + GPUCC_CXO_CBCR));
	con_hex2(" rcg", "cmd=", rd(GPUCC + GPUCC_GMU_RCG), "cfg=", rd(GPUCC + GPUCC_GMU_RCG + 4));
	con_hex2(" pll1", "mode=", rd(GPUCC + GPUCC_PLL1_MODE), "hwctl=", rd(GPUCC + GPUCC_CX_GDS_HW_CTRL));

	/* CX GDSC on, as gdsc_enable() for gpu_cx_gdsc */
	uint32_t cx = rd(GPUCC + GPUCC_CX_GDSCR);
	if (cx & GDSC_COLLAPSE)
		wr(GPUCC + GPUCC_CX_GDSCR, cx & ~GDSC_COLLAPSE);
	int cx_on = wait_bits(GPUCC + GPUCC_CX_GDSCR, GDSC_PWR_ON, GDSC_PWR_ON) &&
	    wait_bits(GPUCC + GPUCC_CX_GDS_HW_CTRL, GDSC_PWR_ON, GDSC_PWR_ON);
	con_hex2("CXGDSC", "gdscr=", rd(GPUCC + GPUCC_CX_GDSCR), "hwctl=", rd(GPUCC + GPUCC_CX_GDS_HW_CTRL));
	if (!cx_on) {
		con_str("GPU", "CX GDSC did not report power on: stop");
		return;
	}
	if (!branch_on(" cxo", GPUCC + GPUCC_CXO_CBCR, 0) ||
	    !branch_on(" cxgmu", GPUCC + GPUCC_CX_GMU_CBCR, 0)) {
		con_str("GPU", "GMU clocks stay off: stop");
		return;
	}

	smmu_dump(GSMMU);

	con_hex2("GMU", "sysreset=", rd(GMU_REG(GMU_CM3_SYSRESET)),
	    "bootcfg=", rd(GMU_REG(GMU_CM3_BOOT_CONFIG)));
	con_hex2(" fw", "busy=", rd(GMU_REG(GMU_CM3_FW_BUSY)),
	    "init=", rd(GMU_REG(GMU_CM3_FW_INIT_RESULT)));
	con_hex2(" fw", "version=", rd(GMU_REG(GMU_CORE_FW_VERSION)),
	    "cm3cfg=", rd(GMU_REG(GMU_CM3_CFG)));
	con_str("GPU", "t10 done");
}
