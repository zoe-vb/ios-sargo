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
 * t11 (GPU=gmu, -DGPU_GMU_BOOT): then boot the GMU as Linux a6xx_gmu_fw_start
 * does for the legacy (a630-family) firmware: GMU clock at 200 MHz, a GPU
 * SMMU context bank (CB 1, stage 1) mapping 64 KiB of shim memory at the
 * GMU's uncached window 0x60000000 for GMU streams 4/5, the firmware
 * (a630_gmu.bin, embedded at build time, not in git) into the CM3 ITCM, the
 * boot registers, then CM3 reset release and a poll for 0xbabeface. Not
 * done yet: RPMh/PDC init, RPMH_CTRL votes, GX power, HFI queues.
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
#define GMU_CM3_ITCM_START	0x1b400
#define GMU_CM3_DTCM_START	0x1c400
#define GMU_ICACHE_CONFIG	0x1f400
#define GMU_DCACHE_CONFIG	0x1f401
#define GMU_SYS_BUS_CONFIG	0x1f40f
#define GMU_PWR_COL_CP_MSG	0x1f900
#define GMU_PWR_COL_CP_RESP	0x1f901
#define GMU_HFI_CTRL_STATUS	0x1f980
#define GMU_HFI_SFR_ADDR	0x1f982
#define GMU_HFI_QTBL_INFO	0x1f984
#define GMU_HFI_QTBL_ADDR	0x1f985
#define GMU_GMU2HOST_INTR_INFO	0x1f992
#define GMU_GENERAL_7		0x1f9cc
#define GMU_AO_HOST_INTR_STATUS	0x23b05
#define GMU_AHB_FENCE_RANGE_0	0x23b11

#define GSMMU		0x05040000UL	/* arm,smmu-kgsl (qcom,smmu-v2) */
#define GSMMU_GR1	(GSMMU + 0x1000)
#define GSMMU_CB(n)	(GSMMU + 0x8000 + (uintptr_t)(n) * 0x1000)	/* 4K pages, 8 global pages */
#define GMU_CB		1
#define GMU_IOVA	0x60000000u

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

#ifdef GPU_GMU_BOOT
extern const uint8_t gmu_fw[], gmu_fw_end[];	/* gmufw.S, generated by build.sh */

/* GMU memory: 16 x 4K at iova 0x60000000 (dummy, hfi, log, debug) */
static uint8_t gmu_mem[0x10000] __attribute__((aligned(4096)));
static uint64_t pt_l1[512] __attribute__((aligned(4096)));
static uint64_t pt_l2[512] __attribute__((aligned(4096)));
static uint64_t pt_l3[512] __attribute__((aligned(4096)));
#define GMU_HFI_IOVA	(GMU_IOVA + 0x1000)
#define GMU_LOG_IOVA	(GMU_IOVA + 0x5000)

static int gmu_clock_200(void)
{
	/* gpu_cc_gmu_clk_src: GPLL0 main div (300 MHz, src 6) / 1.5 */
	wr(GPUCC + GPUCC_GMU_RCG + 4, (6u << 8) | 2u);
	wr(GPUCC + GPUCC_GMU_RCG, rd(GPUCC + GPUCC_GMU_RCG) | 1u);
	int ok = wait_bits(GPUCC + GPUCC_GMU_RCG, 1u, 0);
	con_hex2(" rcg200", "cmd=", rd(GPUCC + GPUCC_GMU_RCG), "cfg=", rd(GPUCC + GPUCC_GMU_RCG + 4));
	return ok;
}

/* Stage-1 context bank GMU_CB, VA 32 bits, 4K granule, start level 1. */
static int gsmmu_map_gmu(void)
{
	uintptr_t cb = GSMMU_CB(GMU_CB);
	pt_l1[GMU_IOVA >> 30] = (uintptr_t)pt_l2 | 3u;
	pt_l2[(GMU_IOVA >> 21) & 511] = (uintptr_t)pt_l3 | 3u;
	for (unsigned i = 0; i < sizeof(gmu_mem) / 4096; i++)	/* AF, AP=01 (RW), attr 0 */
		pt_l3[((GMU_IOVA >> 12) & 511) + i] = ((uintptr_t)gmu_mem + i * 4096u) | (1u << 10) | (1u << 6) | 3u;
	__asm__ volatile("dsb sy" ::: "memory");

	wr(cb + 0x00, 0);				/* SCTLR off while programming */
	wr(GSMMU_GR1 + 0x800 + 4 * GMU_CB, 1u);	/* CBA2R: AArch64 descriptors */
	wr(GSMMU_GR1 + 4 * GMU_CB, 1u << 16);		/* CBAR: S1 translate, S2 bypass */
	wr(cb + 0x10, 2u);				/* TCR2: 40-bit PA */
	wr(cb + 0x20, (uint32_t)(uintptr_t)pt_l1);	/* TTBR0, ASID 0 */
	wr(cb + 0x24, (uint32_t)((uint64_t)(uintptr_t)pt_l1 >> 32));
	wr(cb + 0x30, 32u | (1u << 23));		/* TCR: T0SZ 32, 4K, EPD1 */
	wr(cb + 0x38, 0x44u);				/* MAIR0: attr 0 normal non-cacheable */
	wr(cb + 0x3c, 0);
	wr(cb + 0x58, 0xffffffffu);			/* FSR clear */
	wr(cb + 0x618, 0);				/* TLBIALL */
	wr(cb + 0x7f0, 0);				/* TLBSYNC */
	wait_bits(cb + 0x7f4, 1u, 0);
	wr(cb + 0x00, 0x67u);				/* M, TRE, AFE, CFRE, CFIE */
	/* SMR 0: GMU streams 4 and 5 (id 4, mask 1) -> CB 1 */
	wr(GSMMU + 0xc00, GMU_CB);
	wr(GSMMU + 0x800, (1u << 31) | (1u << 16) | 4u);
	con_hex2(" smr0", "smr=", rd(GSMMU + 0x800), "s2cr=", rd(GSMMU + 0xc00));
	con_hex2(" cb1", "cbar=", rd(GSMMU_GR1 + 4 * GMU_CB), "sctlr=", rd(cb));
	con_hex2(" cb1", "ttbr0=", rd(cb + 0x20), "tcr=", rd(cb + 0x30));
	return rd(GSMMU + 0x800) == ((1u << 31) | (1u << 16) | 4u) && rd(GSMMU + 0xc00) == GMU_CB &&
	    (rd(cb) & 1u) && rd(cb + 0x20) == (uint32_t)(uintptr_t)pt_l1;
}

static void gmu_boot(void)
{
	uint32_t size = (uint32_t)(gmu_fw_end - gmu_fw);
	con_hex2("GMUFW", "size=", size, "word0=", *(const uint32_t *)gmu_fw);
	if (size == 0 || size > 0x8000 || (size & 3)) {
		con_str("GMUFW", "bad firmware size: stop");
		return;
	}
	wr(GCC + GCC_DDRSS_GPU_AXI, rd(GCC + GCC_DDRSS_GPU_AXI) | 1u);
	if (!gmu_clock_200() || !gsmmu_map_gmu()) {
		con_str("GMU", "clock or SMMU setup did not stick: stop");
		return;
	}
	wr(GMU_REG(GMU_CM3_SYSRESET), 1);
	wr(GMU_REG(GMU_GENERAL_7), 1);			/* TCM retention */
	for (uint32_t i = 0; i < size / 4; i++)
		wr(GMU_REG(GMU_CM3_ITCM_START + i), ((const uint32_t *)gmu_fw)[i]);
	uint32_t bad = 0;
	for (uint32_t i = 0; i < size / 4; i++)
		bad += rd(GMU_REG(GMU_CM3_ITCM_START + i)) != ((const uint32_t *)gmu_fw)[i];
	con_hex2(" itcm", "mismatch=", bad, "dtcm+ff8=", rd(GMU_REG(GMU_CM3_DTCM_START + 0xff8)));

	wr(GMU_REG(GMU_CM3_FW_INIT_RESULT), 0);
	wr(GMU_REG(GMU_CM3_BOOT_CONFIG), 2);
	wr(GMU_REG(GMU_HFI_QTBL_ADDR), GMU_HFI_IOVA);
	wr(GMU_REG(GMU_HFI_QTBL_INFO), 1);
	wr(GMU_REG(GMU_AHB_FENCE_RANGE_0), (1u << 31) | (0xau << 18) | 0xa0u);
	wr(GMU_REG(GMU_CM3_CFG), 0x4052);
	wr(GMU_REG(GMU_HFI_SFR_ADDR), 0x06015000u);	/* chip id 6.1.5.0 in GMU layout */
	wr(GMU_REG(GMU_PWR_COL_CP_MSG), GMU_LOG_IOVA | 3u);	/* 16K log */
	wr(GMU_REG(GMU_SYS_BUS_CONFIG), 1);
	wr(GMU_REG(GMU_ICACHE_CONFIG), 1);
	wr(GMU_REG(GMU_DCACHE_CONFIG), 1);

	/* a6xx_gmu_start */
	wr(GMU_REG(GMU_CM3_SYSRESET), 1);
	wr(GMU_REG(GMU_PWR_COL_CP_RESP), 0);
	wr(GMU_REG(GMU_CM3_SYSRESET), 0);
	uint32_t res = 0;
	unsigned ms;
	for (ms = 0; ms < 100; ms++) {
		res = rd(GMU_REG(GMU_CM3_FW_INIT_RESULT));
		if (res == 0xbabefaceu)
			break;
		udelay(1000);
	}
	con_hex2("GMU", "init=", res, "ms=", ms);
	con_hex2(" fw", "version=", rd(GMU_REG(GMU_CORE_FW_VERSION)), "hfi=", rd(GMU_REG(GMU_HFI_CTRL_STATUS)));
	con_hex2(" intr", "g2h=", rd(GMU_REG(GMU_GMU2HOST_INTR_INFO)), "ao=", rd(GMU_REG(GMU_AO_HOST_INTR_STATUS)));
	con_hex2(" cb1", "fsr=", rd(GSMMU_CB(GMU_CB) + 0x58), "far=", rd(GSMMU_CB(GMU_CB) + 0x60));
	con_hex2(" mem", "log0=", *(volatile uint32_t *)(gmu_mem + 0x5000), "hfi0=", *(volatile uint32_t *)(gmu_mem + 0x1000));
	con_str("GMU", res == 0xbabefaceu ? "FIRMWARE RUNNING" : "no 0xbabeface");
}
#endif

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
	con_hex2("GSMMU", "cb1 sctlr=", rd(GSMMU_CB(GMU_CB)), "cbar=", rd(GSMMU_GR1 + 4 * GMU_CB));

	con_hex2("GMU", "sysreset=", rd(GMU_REG(GMU_CM3_SYSRESET)),
	    "bootcfg=", rd(GMU_REG(GMU_CM3_BOOT_CONFIG)));
	con_hex2(" fw", "busy=", rd(GMU_REG(GMU_CM3_FW_BUSY)),
	    "init=", rd(GMU_REG(GMU_CM3_FW_INIT_RESULT)));
	con_hex2(" fw", "version=", rd(GMU_REG(GMU_CORE_FW_VERSION)),
	    "cm3cfg=", rd(GMU_REG(GMU_CM3_CFG)));
#ifdef GPU_GMU_BOOT
	gmu_boot();
#endif
	con_str("GPU", "probe done");
}
