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
 * t11/t12 (GPU=gmu, -DGPU_GMU_BOOT): then boot the GMU as Linux a6xx_gmu_fw_start
 * does for the legacy (a630-family) firmware: GMU clock at 200 MHz, a GPU
 * SMMU context bank (CB 1, stage 1) mapping 64 KiB of shim memory at the
 * GMU's uncached window 0x60000000 for GMU streams 4/5, the firmware
 * (a630_gmu.bin, embedded at build time, not in git) into the CM3 ITCM, the
 * boot registers, then CM3 reset release and a poll for 0xbabeface.
 * t12: the GPU SMMU firmware drops masked SMRs; exact ones (5, 4) stick.
 *
 * t13 (GPU=gx, also -DGPU_GX_ON): before the boot, Linux a6xx_gmu_rpmh_init
 * (RSCC sequencer + PDC TCS/sequencer, a615 values), the HFI queue table and
 * a6xx_gmu_power_config (active idle level, RPMh votes on); after it,
 * a6xx_gmu_gfx_rail_on (GX/MX ARC vote indexes for the lowest GPU level,
 * MIN_SVS, from cmd-db "gfx.lvl"/"mx.lvl"; downstream finds the cmd-db
 * through the AOP message RAM word at 0xc3f000c), OOB BOOT_SLUMBER, SPTPRAC on,
 * HFI_CTRL_INIT, OOB GPU_SET. GPU core (GX) registers are read only if
 * GMU_SPTPRAC_PWR_CLK_STATUS says GX power and clock are on.
 *
 * t14 (GPU=cp, also -DGPU_CP): GPU stream 0 (exact SMR) into the same
 * context bank, 64 KiB of GPU memory at iova 0x60100000, then the CP part
 * of Linux a6xx hw_init: GBIF halts released, secure TSB range off, 64-bit
 * addressing, UCHE/ROQ/mem pool setup, a630_sqe.fw (minus its first dword,
 * as adreno_fw_create_bo) as the SQE instructions, a 4 KiB ring, SQE on,
 * CP_ME_INIT, then CP_MEM_WRITE of 0xc0ffee15 to GPU memory. The GPU stays
 * in secure mode (no zap shader yet, RBBM_SECVID_TRUST_CNTL untouched).
 *
 * t15 (GPU=zap, also -DGPU_ZAP): after the ring test, the zap shader as Linux
 * (t17: after the full Linux hw_init and CP_ME_INIT alone, see hw_init_tail;
 * t19: before the SQE starts, as downstream KGSL a6xx_microcode_load)
 * qcom_mdt_load + a6xx_zap_shader_init: TZ PAS (SIP SMC64, svc 2) for PAS id
 * 13 (stock DT /soc/qcom,kgsl-hyp): IS_SUPPORTED, INIT_IMAGE with the .mdt
 * (ELF header + hash segment = the metadata), MEM_SETUP of the reserved
 * gpu_region 0x95c15000 (8 KiB; one relocatable segment, pa 0x5000, 0x7b0
 * bytes from .b02), copy, AUTH_AND_RESET; then CP_SET_SECURE_MODE 0 and a
 * second CP_MEM_WRITE, with RBBM_SECVID_TRUST_CNTL read before and after.
 *
 * t16 (GPU=trust, -DGPU_TRUST): no zap shader; after the ring test the CPU
 * writes RBBM_SECVID_TRUST_CNTL = 0 (Linux's no-zap path), then the GPU
 * does a second CP_MEM_WRITE.
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
#define GMU_GX_SPTPRAC_POWER_CONTROL	0x1a881
#define GMU_BOOT_SLUMBER_OPTION	0x1cbf8
#define GMU_GX_VOTE_IDX		0x1cbf9
#define GMU_MX_VOTE_IDX		0x1cbfa
#define GMU_PWR_COL_INTER_FRAME_CTRL	0x1f8c0
#define GMU_SPTPRAC_PWR_CLK_STATUS	0x1f8d0
#define GMU_RPMH_CTRL		0x1f8e8
#define GMU_HFI_CTRL_INIT	0x1f986
#define GMU_GMU2HOST_INTR_CLR	0x1f991
#define GMU_HOST2GMU_INTR_SET	0x1f994
#define GMU_RSCC_CONTROL_ACK	0x23b08

/* RSCC = GMU block + 0x23000, dword offsets */
#define RSCC		(0x0506a000UL + 0x23000)
#define RSCC_REG(dw)	(RSCC + (uintptr_t)(dw) * 4u)
/* PDC (sdm670.dtsi gmu_pdc / gmu_pdc_seq), dword offsets */
#define PDC		0x0b280000UL
#define PDC_SEQ		0x0b480000UL
#define PDC_REG(dw)	(PDC + (uintptr_t)(dw) * 4u)
#define CMD_DB_PTR	0x0c3f000cUL	/* stock DT qcom,cmd-db: base, size of the cmd-db */

/* GPU core (GX) registers, dword offsets from the GPU base */
#define RBBM_STATUS	0x0210
#define RBBM_INT_0_STATUS	0x0201
#define CP_SCRATCH0	0x0883

#define GSMMU		0x05040000UL	/* arm,smmu-kgsl (qcom,smmu-v2) */
#define GSMMU_GR1	(GSMMU + 0x1000)
#define GSMMU_CB(n)	(GSMMU + 0x8000 + (uintptr_t)(n) * 0x1000)	/* 4K pages, 8 global pages */
/*
 * t15b hung in AUTH_AND_RESET with GMU and GPU both on CB 1. Downstream
 * KGSL gives gfx3d_user CB 0 and the secure bank(s) come next; TZ likely
 * programs a secure bank for stream 2 itself. GPU=zap: GPU on CB 0, GMU
 * on CB 3, CBs 1/2 left alone.
 */
#ifdef GPU_ZAP
#define GMU_CB		3
#define GPU_CB		0
#else
#define GMU_CB		1
#define GPU_CB		GMU_CB
#endif
#define GMU_IOVA	0x60000000u

#define CLK_OFF		(1u << 31)	/* CBCR status */
#define GDSC_PWR_ON	(1u << 31)
#define GDSC_COLLAPSE	(1u << 0)

/* GPU=cp needs the screen for the ring test: earlier stages' details off */
#ifdef GPU_CP
#define DETAIL(stmt)	do { if (0) { stmt; } } while (0)
#else
#define DETAIL(stmt)	do { stmt; } while (0)
#endif
#ifdef GPU_ZAP		/* and t14's ring-test details for t15 */
#define CPDETAIL(stmt)	do { if (0) { stmt; } } while (0)
#else
#define CPDETAIL(stmt)	do { stmt; } while (0)
#endif

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
	DETAIL(con_hex2(label, "was=", before, "now=", rd(cbcr) | (ok ? 0 : 0xbad00000u)));
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
#ifdef GPU_CP
extern const uint8_t sqe_fw[], sqe_fw_end[];	/* gmufw.S */
static uint8_t gpu_mem[0x10000] __attribute__((aligned(4096)));
#define GPU_IOVA	(GMU_IOVA + 0x100000)	/* same L3 table, entries 256.. */
#define GPU_RING	0x8000			/* offsets in gpu_mem */
#define GPU_TARGET	0x9000
#endif
#ifdef GPU_ZAP
extern const uint8_t zap_mdt[], zap_mdt_end[], zap_b02[], zap_b02_end[];	/* gmufw.S */
static uint8_t zap_meta[0x2000] __attribute__((aligned(4096)));
/* stock (Android 9) DT gpu_region; mainline sdm670-google-common has gpu@97a15000 (ZAPREGION=mainline) */
#ifdef ZAP_REGION_MAINLINE
#define ZAP_REGION	0x97a15000UL
#else
#define ZAP_REGION	0x95c15000UL
#endif
#define ZAP_PAS_ID	13
#endif

static int gmu_clock_200(void)
{
	/* gpu_cc_gmu_clk_src: GPLL0 main div (300 MHz, src 6) / 1.5 */
	wr(GPUCC + GPUCC_GMU_RCG + 4, (6u << 8) | 2u);
	wr(GPUCC + GPUCC_GMU_RCG, rd(GPUCC + GPUCC_GMU_RCG) | 1u);
	int ok = wait_bits(GPUCC + GPUCC_GMU_RCG, 1u, 0);
	DETAIL(con_hex2(" rcg200", "cmd=", rd(GPUCC + GPUCC_GMU_RCG), "cfg=", rd(GPUCC + GPUCC_GMU_RCG + 4)));
	return ok;
}

/* Stage-1 context bank n over pt_l1, VA 32 bits, 4K granule, start level 1. */
static int cb_program(unsigned n)
{
	uintptr_t cb = GSMMU_CB(n);
	wr(cb + 0x00, 0);				/* SCTLR off while programming */
	wr(GSMMU_GR1 + 0x800 + 4 * n, 1u);		/* CBA2R: AArch64 descriptors */
	wr(GSMMU_GR1 + 4 * n, 1u << 16);		/* CBAR: S1 translate, S2 bypass */
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
	DETAIL(con_hex2(" cb", "cbar=", rd(GSMMU_GR1 + 4 * n), "sctlr=", rd(cb)));
	DETAIL(con_hex2(" cb", "ttbr0=", rd(cb + 0x20), "tcr=", rd(cb + 0x30)));
	return (rd(cb) & 1u) && rd(cb + 0x20) == (uint32_t)(uintptr_t)pt_l1;
}

static int gsmmu_map_gmu(void)
{
	pt_l1[GMU_IOVA >> 30] = (uintptr_t)pt_l2 | 3u;
	pt_l2[(GMU_IOVA >> 21) & 511] = (uintptr_t)pt_l3 | 3u;
	for (unsigned i = 0; i < sizeof(gmu_mem) / 4096; i++)	/* AF, AP=01 (RW), attr 0 */
		pt_l3[((GMU_IOVA >> 12) & 511) + i] = ((uintptr_t)gmu_mem + i * 4096u) | (1u << 10) | (1u << 6) | 3u;
#ifdef GPU_CP
	for (unsigned i = 0; i < sizeof(gpu_mem) / 4096; i++)
		pt_l3[((GPU_IOVA >> 12) & 511) + i] = ((uintptr_t)gpu_mem + i * 4096u) | (1u << 10) | (1u << 6) | 3u;
#endif
	__asm__ volatile("dsb sy" ::: "memory");
	if (!cb_program(GMU_CB) || (GPU_CB != GMU_CB && !cb_program(GPU_CB)))
		return 0;
	/*
	 * t11: an SMR for streams 4+5 (id 4, mask 1) read back 0 while its
	 * S2CR stuck - the firmware drops it. Try exact single-stream SMRs
	 * (mainline maps only GMU stream 5; downstream also 4), several slots.
	 */
	static const struct { uint8_t slot, sid; } try[] = { {0, 5}, {1, 4}, {4, 5}, {3, 4}, {2, 5} };
#ifdef GPU_CP
	/* GPU (gfx3d_user) stream 0, exact, slot 2 */
	wr(GSMMU + 0xc00 + 4 * 2, GPU_CB);
	wr(GSMMU + 0x800 + 4 * 2, 1u << 31);
	con_hex2(" smr2", "smr=", rd(GSMMU + 0x808), "s2cr=", rd(GSMMU + 0xc08));
#endif
	int have5 = 0, have4 = 0;
	for (unsigned i = 0; i < sizeof(try) / sizeof(try[0]); i++) {
		unsigned s = try[i].slot, sid = try[i].sid;
		if ((sid == 5 && have5) || (sid == 4 && have4))
			continue;
		wr(GSMMU + 0xc00 + 4 * s, GMU_CB);
		wr(GSMMU + 0x800 + 4 * s, (1u << 31) | sid);
		uint32_t smr = rd(GSMMU + 0x800 + 4 * s);
		char lbl[] = " smr0";
		lbl[4] = (char)('0' + s);
		con_hex2(lbl, "smr=", smr, "s2cr=", rd(GSMMU + 0xc00 + 4 * s));
		if (smr == ((1u << 31) | sid)) {
			if (sid == 5)
				have5 = 1;
			else
				have4 = 1;
		} else {
			wr(GSMMU + 0xc00 + 4 * s, 0x000200ffu);	/* back to fault */
		}
	}
	con_hex2(" sids", "5=", have5, "4=", have4);
	return have5;
}

#ifdef GPU_GX_ON
/* Linux a6xx_gmu_rpmh_init for a615 (pdc_address_offset 0x30080, TCS3 data 3) */
static void gmu_rpmh_init(void)
{
	wr(RSCC_REG(0x0004), 1u << 24);		/* RSC_STATUS0_DRV0: no SDE clock gating */
	wr(RSCC_REG(0x000b), 1);		/* PDC_SLAVE_ID_DRV0 */
	wr(RSCC_REG(0x000e), 0);		/* HIDDEN_TCS_CMD0_DATA */
	wr(RSCC_REG(0x000d), 0);		/* HIDDEN_TCS_CMD0_ADDR */
	wr(RSCC_REG(0x000e + 2), 0);
	wr(RSCC_REG(0x000d + 2), 0);
	wr(RSCC_REG(0x000e + 4), 0x80000000u);
	wr(RSCC_REG(0x000d + 4), 0);
	wr(RSCC_REG(0x0100), 0);		/* OVERRIDE_START_ADDR */
	wr(RSCC_REG(0x0008), 0x4520);		/* PDC_SEQ_START_ADDR */
	wr(RSCC_REG(0x0009), 0x4510);		/* PDC_MATCH_VALUE_LO */
	wr(RSCC_REG(0x000a), 0x4514);		/* PDC_MATCH_VALUE_HI */
	wr(RSCC_REG(0x0180), 0xa7a506a0u);	/* SEQ_MEM_0_DRV0 */
	wr(RSCC_REG(0x0181), 0xa1e6a6e7u);
	wr(RSCC_REG(0x0182), 0xa2e081e1u);
	wr(RSCC_REG(0x0183), 0xe9a982e2u);
	wr(RSCC_REG(0x0184), 0x0020e8a8u);

	wr(PDC_SEQ + 0x0, 0xfebea1e1u);		/* PDC_GPU_SEQ_MEM_0.. */
	wr(PDC_SEQ + 0x4, 0xa5a4a3a2u);
	wr(PDC_SEQ + 0x8, 0x8382a6e0u);
	wr(PDC_SEQ + 0xc, 0xbce3e284u);
	wr(PDC_SEQ + 0x10, 0x002081fcu);

	wr(PDC_REG(0x1573), 7);			/* TCS1_CMD_ENABLE_BANK */
	wr(PDC_REG(0x1574), 0);			/* TCS1_CMD_WAIT_FOR_CMPL_BANK */
	wr(PDC_REG(0x1572), 0);			/* TCS1_CONTROL */
	wr(PDC_REG(0x1575), 0x10108);		/* TCS1_CMD0_MSGID/ADDR/DATA */
	wr(PDC_REG(0x1576), 0x30010);
	wr(PDC_REG(0x1577), 1);
	wr(PDC_REG(0x1575 + 4), 0x10108);
	wr(PDC_REG(0x1576 + 4), 0x30000);
	wr(PDC_REG(0x1577 + 4), 0);
	wr(PDC_REG(0x1575 + 8), 0x10108);
	wr(PDC_REG(0x1576 + 8), 0x30080);
	wr(PDC_REG(0x1577 + 8), 0);
	wr(PDC_REG(0x15d7), 7);			/* TCS3 */
	wr(PDC_REG(0x15d8), 0);
	wr(PDC_REG(0x15d6), 0);
	wr(PDC_REG(0x15d9), 0x10108);
	wr(PDC_REG(0x15da), 0x30010);
	wr(PDC_REG(0x15db), 2);
	wr(PDC_REG(0x15d9 + 4), 0x10108);
	wr(PDC_REG(0x15da + 4), 0x30000);
	wr(PDC_REG(0x15db + 4), 3);
	wr(PDC_REG(0x15d9 + 8), 0x10108);
	wr(PDC_REG(0x15da + 8), 0x30080);
	wr(PDC_REG(0x15db + 8), 3);
	wr(PDC_REG(0x1148), 0);			/* GPU_SEQ_START_ADDR */
	wr(PDC_REG(0x1140), 0x80000001u);	/* GPU_ENABLE_PDC */
	DETAIL(con_hex2(" pdc", "en=", rd(PDC_REG(0x1140)), "rscseq=", rd(RSCC_REG(0x0180))));
}

/* Linux cmd_db_read_aux_data(): the ARC level list of an RPMh resource */
static uint32_t cmd_db_addr;	/* RPMh address of the last cmd_db_aux() match */
static const uint16_t *cmd_db_aux(const char *id, unsigned *count)
{
	uint32_t base = rd(CMD_DB_PTR), size = rd(CMD_DB_PTR + 4);
	static int shown;
	if (!shown++)
		con_hex2("CMDDB", "base=", base, "size=", size);
	if (base < 0x80000000u || base >= 0xa0000000u || size < 144 || size > 0x40000)
		return 0;
	const uint8_t *db = (const uint8_t *)(uintptr_t)base;
	if (db[4] != 0xdb || db[5] != 0x30 || db[6] != 0x03 || db[7] != 0x0c)
		return 0;
	const uint8_t *data = db + 144;		/* sizeof(struct cmd_db_header) */
	for (unsigned s = 0; s < 8; s++) {
		const uint8_t *h = db + 8 + 16 * s;
		unsigned hoff = h[2] | h[3] << 8, doff = h[4] | h[5] << 8, cnt = h[6] | h[7] << 8;
		for (unsigned e = 0; e < cnt; e++) {
			const uint8_t *ent = data + hoff + 24 * e;
			unsigned k = 0;
			while (k < 8 && id[k] && ent[k] == (uint8_t)id[k])
				k++;
			if (k < 8 && (id[k] || ent[k]))
				continue;
			*count = (ent[20] | ent[21] << 8) / 2;
			cmd_db_addr = ent[16] | ent[17] << 8 | (uint32_t)ent[18] << 16 | (uint32_t)ent[19] << 24;
			return (const uint16_t *)(data + doff + (ent[22] | ent[23] << 8));
		}
	}
	return 0;
}

/* GX/MX vote for an RPMh level, as a6xx_gmu_rpmh_arc_votes_init */
static int gx_vote(unsigned level, unsigned *gx, unsigned *mx)
{
	unsigned np = 0, ns = 0;
	const uint16_t *pri = cmd_db_aux("gfx.lvl", &np), *sec = cmd_db_aux("mx.lvl", &ns);
	if (!pri || !sec || !np || !ns || np > 16 || ns > 16) {
		con_hex2("CMDDB", "gfx n=", np, "mx n=", ns);
		return 0;
	}
	con_hex2("CMDDB", "gfx n=", np, "mx n=", ns);
	DETAIL(con_hex2(" gfx", "l0-3=", (uint32_t)pri[0] << 16 | pri[1 % np], "l2-3=", (uint32_t)pri[2 % np] << 16 | pri[3 % np]));
	*gx = np;
	for (unsigned j = 0; j < np; j++)
		if (pri[j] >= level) {
			*gx = j;
			break;
		}
	*mx = 0;
	for (unsigned j = 0; j < ns; j++) {
		if (sec[j] >= level) {
			*mx = j;
			break;
		} else if (sec[j]) {
			*mx = j;
		}
	}
	con_hex2(" vote", "gx=", *gx, "mx=", *mx);
	return *gx < np;
}

/* a6xx_gmu_rpmh_arc_votes_init for one level: (pri level << 16) | (sec idx << 8) | pri idx */
static uint32_t arc_vote(const char *pri_id, unsigned level)
{
	unsigned np = 0, ns = 0, pi = 0, si = 0;
	const uint16_t *pri = cmd_db_aux(pri_id, &np), *sec = cmd_db_aux("mx.lvl", &ns);
	if (!pri || !sec || !np || !ns || np > 16 || ns > 16)
		return 0xffffffffu;
	for (unsigned j = 0; j < np; j++)
		if (pri[j] >= level) {
			pi = j;
			break;
		}
	for (unsigned j = 0; j < ns; j++) {
		if (sec[j] >= level) {
			si = j;
			break;
		} else if (sec[j]) {
			si = j;
		}
	}
	return (uint32_t)pri[pi] << 16 | si << 8 | pi;
}

/*
 * HFI v1 messages (a6xx_hfi_send_msg): command queue at hfi + 4K (header
 * 0), response queue at hfi + 8K (header 1), legacy = no 4-dword padding.
 * Returns 0, or 0x100 | step on a timeout / bad response, or the GMU's
 * error code.
 */
static uint32_t hfi_seq;
static uint32_t hfi_send(uint32_t id, uint32_t *msg, uint32_t dwords)
{
	uint32_t *tbl = (uint32_t *)(gmu_mem + 0x1000);
	volatile uint32_t *cq = (volatile uint32_t *)(gmu_mem + 0x2000), *rq = (volatile uint32_t *)(gmu_mem + 0x3000);
	volatile uint32_t *ch = (volatile uint32_t *)(tbl + 6), *rh = (volatile uint32_t *)(tbl + 6 + 12);
	uint32_t seq = ++hfi_seq % 0xfff;
	msg[0] = seq << 20 | 0u << 16 | dwords << 8 | id;
	uint32_t w = ch[11];
	for (uint32_t i = 0; i < dwords; i++) {
		cq[w] = msg[i];
		w = (w + 1) % 1024;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	ch[11] = w;
	__asm__ volatile("dsb sy" ::: "memory");
	wr(GMU_REG(GMU_HOST2GMU_INTR_SET), 1);
	for (unsigned tries = 0; tries < 4; tries++) {
		int got = 0;
		for (unsigned i = 0; i < 50 && !got; i++)	/* ~100 ms */
			got = wait_bits(GMU_REG(GMU_GMU2HOST_INTR_INFO), 1u, 1u);
		if (!got)
			return 0x100 | 1;
		wr(GMU_REG(GMU_GMU2HOST_INTR_CLR), 1);
		while (rh[10] != rh[11]) {
			uint32_t r0 = rh[10], hdr = rq[r0], n = (hdr >> 8) & 0xff, resp[8] = {0};
			for (uint32_t i = 0; i < n; i++) {
				if (i < 8)
					resp[i] = rq[r0];
				r0 = (r0 + 1) % 1024;
			}
			rh[10] = r0;
			__asm__ volatile("dsb sy" ::: "memory");
			if ((hdr & 0xff) == 100)		/* HFI_F2H_MSG_ERROR */
				return 0x200 | (resp[1] & 0xff);
			if (((resp[1] >> 20) & 0xfff) == seq)
				return resp[2];			/* error field, 0 = ok */
		}
	}
	return 0x100 | 2;
}

/* a6xx_hfi_start_v1 */
static void hfi_start_v1(void)
{
	uint32_t m[1 + 5 + 16 * 8 + 6 * 2 + 8 + 16 * 8 + 4];
	uint32_t res[5];
	for (unsigned i = 0; i < sizeof(m) / 4; i++) m[i] = 0;
	m[2] = GMU_IOVA + 0x9000; m[3] = 0x4000; m[4] = 1;	/* GMU_INIT: debug buffer, cold boot */
	res[0] = hfi_send(0, m, 5);
	for (unsigned i = 0; i < 2; i++) m[i] = 0;
	m[1] = (1u << 28) | (1u << 19) | (1u << 17);		/* FW_VERSION: 1.10 */
	res[1] = hfi_send(1, m, 2);
	for (unsigned i = 0; i < 3 + 2 * 16 + 2 * 4; i++) m[i] = 0;
	m[1] = 2; m[2] = 2;					/* PERF_TABLE v1: off + MIN_SVS */
	m[3] = arc_vote("gfx.lvl", 0);   m[4] = 0;
	uint32_t gxv = arc_vote("gfx.lvl", 48);
	m[5] = gxv;  m[6] = 180000;			/* 180 MHz, kHz */
	m[3 + 32] = arc_vote("cx.lvl", 0);   m[4 + 32] = 0;
	m[5 + 32] = arc_vote("cx.lvl", 48);  m[6 + 32] = 200000;	/* GMU 200 MHz */
	res[2] = hfi_send(4, m, 3 + 2 * 16 + 2 * 4);
	uint32_t bw = 6 + 6 + 12 + 8 + 128;			/* a6xx_build_bw_table */
	for (unsigned i = 0; i < bw; i++) m[i] = 0;
	m[1] = 1; m[2] = 3; m[3] = 3; m[4] = 0x05; m[5] = 0x07;
	m[6] = 0x50034; m[7] = 0x5007c; m[8] = 0x5004c;		/* cnoc addrs */
	m[12] = 0x40000000; m[13] = 0; m[14] = 0x40000000;	/* cnoc data[0] */
	m[18] = 0x60000001; m[19] = 0x20000001; m[20] = 0x60000001;	/* cnoc data[1] */
	m[24] = 0x50000; m[25] = 0x5005c; m[26] = 0x5000c;	/* ddr addrs */
	m[32] = 0x40000000; m[33] = 0x40000000; m[34] = 0x40000000;	/* ddr data[0] */
	res[3] = hfi_send(3, m, bw);
	res[4] = hfi_send(5, m, 1);				/* TEST: no more messages */
	con_hex2(" hfimsg", "init/ver=", res[0] << 16 | (res[1] & 0xffff), "perf/bw=", res[2] << 16 | (res[3] & 0xffff));
	con_hex2(" hfimsg", "test=", res[4], "gx vote=", gxv);
}

/* HFI v1 queue table: command, response (id 4), debug (id 5) queues */
static void hfi_table_init(void)
{
	uint32_t *tbl = (uint32_t *)(gmu_mem + 0x1000);
	tbl[0] = 0;				/* version */
	tbl[1] = 6 + 3 * 12;			/* size (dwords) */
	tbl[2] = 6;				/* qhdr0_offset */
	tbl[3] = 12;				/* qhdr_size */
	tbl[4] = 3;
	tbl[5] = 3;
	static const uint32_t ids[3] = {0, 4, 5};
	for (unsigned q = 0; q < 3; q++) {
		uint32_t *h = tbl + 6 + 12 * q;
		h[0] = 1;				/* status */
		h[1] = GMU_HFI_IOVA + 0x1000 * (q + 1);	/* iova */
		h[2] = 10u << 8 | ids[q];		/* type */
		h[3] = 4096 >> 2;			/* size */
		h[4] = 0; h[5] = 0;
		h[6] = 1; h[7] = 1; h[8] = 1; h[9] = 0;
		h[10] = 0; h[11] = 0;
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

/* legacy OOB request: set bit, wait for the ack bit, clear it */
static int gmu_oob(const char *label, unsigned set, unsigned ack)
{
	wr(GMU_REG(GMU_HOST2GMU_INTR_SET), 1u << set);
	int ok = wait_bits(GMU_REG(GMU_GMU2HOST_INTR_INFO), 1u << ack, 1u << ack) ||
	    wait_bits(GMU_REG(GMU_GMU2HOST_INTR_INFO), 1u << ack, 1u << ack) ||
	    wait_bits(GMU_REG(GMU_GMU2HOST_INTR_INFO), 1u << ack, 1u << ack) ||
	    wait_bits(GMU_REG(GMU_GMU2HOST_INTR_INFO), 1u << ack, 1u << ack) ||
	    wait_bits(GMU_REG(GMU_GMU2HOST_INTR_INFO), 1u << ack, 1u << ack);	/* ~10 ms */
	con_hex2(label, "ack=", ok, "g2h=", rd(GMU_REG(GMU_GMU2HOST_INTR_INFO)));
	wr(GMU_REG(GMU_GMU2HOST_INTR_CLR), 1u << ack);
	return ok;
}

#ifdef GPU_CP
static void gpu_w(unsigned dw, uint32_t v) { wr(GPU + (uintptr_t)dw * 4u, v); }
static uint32_t gpu_r(unsigned dw) { return rd(GPU + (uintptr_t)dw * 4u); }
static void gpu_w64(unsigned dw, uint64_t v) { gpu_w(dw, (uint32_t)v); gpu_w(dw + 1, (uint32_t)(v >> 32)); }

static uint32_t pm4_parity(uint32_t v)
{
	return (0x9669u >> (0xfu & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^
	    (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1u;
}

static uint32_t pkt7(unsigned op, unsigned cnt)
{
	return 0x70000000u | cnt | pm4_parity(cnt) << 15 | (op & 0x7fu) << 16 | pm4_parity(op) << 23;
}

#ifdef GPU_ZAP
/* qcom_scm SMCCC convention: SIP, SMC64, standard call; x0 status, x1 result */
static int64_t scm(uint32_t svc_cmd, uint32_t arginfo, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t *res)
{
	register uint64_t x0 __asm__("x0") = 0x42000000u | svc_cmd;
	register uint64_t x1 __asm__("x1") = arginfo;
	register uint64_t x2 __asm__("x2") = a0;
	register uint64_t x3 __asm__("x3") = a1;
	register uint64_t x4 __asm__("x4") = a2;
	register uint64_t x5 __asm__("x5") = 0;
	register uint64_t x6 __asm__("x6") = 0;
	for (unsigned tries = 0; tries < 20; tries++) {
		uint64_t fn = 0x42000000u | svc_cmd, sess = 0;
		do {	/* QCOM_SCM_INTERRUPTED (1): resume with x0 = 1, x6 as returned */
			x0 = fn; x1 = arginfo; x2 = a0; x3 = a1; x4 = a2; x5 = 0; x6 = sess;
			__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6)
			    : : "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
			fn = 1;
			sess = x6;
		} while (x0 == 1);
		if ((int64_t)x0 != -12)		/* QCOM_SCM_V2_EBUSY */
			break;
		udelay(30000);
	}
	if (res)
		*res = x1;
	return (int64_t)x0;
}

/*
 * t15e: the GMU no longer faults (HFI v1 setup), but AUTH_AND_RESET still
 * never returns. TZ checks the signature with the crypto engine; downstream
 * scm_pas_enable_bw() votes crypto bandwidth around every PAS auth, which on
 * RPMh is the "CE0" BCM. One ACTIVE vote through the apps RSC (DRV 2,
 * stock DT mailbox@179e0000: TCS 0/1 ACTIVE at +0xd00, stride 0x2a0), as
 * the sweet RPMh client (patch 0062) sends its LDO vote.
 */
#define RSC_TCS		0x179e0d00UL
#define TCS_STRIDE	0x2a0
static uint32_t rpmh_bcm_vote(const char *id, uint32_t data)
{
	unsigned n = 0;
	if (!cmd_db_aux(id, &n) || (cmd_db_addr >> 16) != 5)	/* BCM slave */
		return 0xbad00000u | (cmd_db_addr >> 16);
	uint32_t addr = cmd_db_addr;
	unsigned tcs = 2;
	for (unsigned i = 0; i < 2; i++) {
		uintptr_t b = RSC_TCS + i * TCS_STRIDE;
		if (rd(b + 0x18) && !rd(b + 0x14) && !rd(b + 0x1c) && !(rd(RSC_TCS + 0x04) & (1u << i))) {
			tcs = i;
			break;
		}
	}
	if (tcs == 2)
		return 0xbad0b05eu;
	uintptr_t b = RSC_TCS + tcs * TCS_STRIDE;
	uint32_t irq_en = rd(RSC_TCS + 0x00);
	wr(RSC_TCS + 0x00, irq_en & ~(1u << tcs));
	wr(b + 0x30, 8u | (1u << 8) | (1u << 16));	/* MSGID: write, response */
	wr(b + 0x34, addr);
	wr(b + 0x38, data);
	wr(b + 0x10, 1);				/* WAIT_FOR_CMPL */
	wr(b + 0x1c, 1);				/* CMD_ENABLE */
	wr(b + 0x14, 0);
	wr(b + 0x14, 1u << 16);				/* AMC mode */
	wr(b + 0x14, (1u << 16) | (1u << 24));		/* trigger */
	uint32_t st = 0;
	for (unsigned us = 0; us < 100000; us++) {
		st = rd(b + 0x3c);
		if ((st & ((1u << 8) | (1u << 16))) == ((1u << 8) | (1u << 16)) && rd(b + 0x18))
			break;
		udelay(1);
	}
	wr(b + 0x14, 1u << 16);
	wr(b + 0x14, 0);
	wr(b + 0x1c, 0);
	wr(b + 0x10, 0);
	wr(RSC_TCS + 0x08, 1u << tcs);			/* IRQ clear */
	wr(RSC_TCS + 0x00, irq_en);
	con_hex2(" bcm", "addr=", addr, "status=", st);
	return st;
}

/* Linux qcom_mdt_load + qcom_scm_pas_auth_and_reset for the a615 zap shader */
static int zap_load(void)
{
	uint32_t mdt_size = (uint32_t)(zap_mdt_end - zap_mdt), b02_size = (uint32_t)(zap_b02_end - zap_b02);
	uint64_t avail = 0, sup = 0;
	int64_t ra = scm(0x0601, 1, 0x42000201u, 0, 0, &avail);	/* INFO IS_CALL_AVAIL(PAS_INIT_IMAGE) */
	int64_t rs = scm(0x0207, 1, ZAP_PAS_ID, 0, 0, &sup);		/* PAS_IS_SUPPORTED */
	con_hex2("ZAP", "avail=", (uint32_t)(ra << 16 | (avail & 0xffff)), "supported=", (uint32_t)(rs << 16 | (sup & 0xffff)));
	if (mdt_size > sizeof(zap_meta) || b02_size > 0x2000 || mdt_size < 0x94) {
		con_hex2("ZAP", "bad files mdt=", mdt_size, "b02=", b02_size);
		return 0;
	}
	for (uint32_t i = 0; i < mdt_size; i++)
		zap_meta[i] = zap_mdt[i];
	__asm__ volatile("dsb sy" ::: "memory");
	/*
	 * t15 (IMG_0068) hung after INIT_IMAGE/MEM_SETUP returned 0, with no
	 * exception: in the copy or in AUTH_AND_RESET. t15b marks each step.
	 */
	CPDETAIL(con_hex2(" region", "before=", *(volatile uint32_t *)ZAP_REGION, "meta=", (uint32_t)(uintptr_t)zap_meta));
	/*
	 * t18: Linux (qcom_scm_pas_init_image: "ret ? : res.result[0]") and
	 * ios-sailfish (r != 0 || r1 != 0) also fail on a non-zero x1 result;
	 * t15..t17b only checked x0. Show both and stop on either.
	 */
	uint64_t ri1 = 0xdead, rm1 = 0xdead;
	int64_t ri = scm(0x0201, 2 | (2u << 6), ZAP_PAS_ID, (uintptr_t)zap_meta, 0, &ri1);	/* VAL, RW */
	con_hex2(" init", "x0=", (uint32_t)ri, "x1=", (uint32_t)ri1);
	if (ri || ri1) {
		con_str("ZAP", "INIT_IMAGE refused (x1): auth not attempted");
		return 0;
	}
	int64_t rm = scm(0x0202, 3, ZAP_PAS_ID, ZAP_REGION, 0x1000, &rm1);
	con_hex2(" memset", "x0=", (uint32_t)rm, "x1=", (uint32_t)rm1);
	if (rm || rm1) {
		con_str("ZAP", "MEM_SETUP refused (x1): auth not attempted");
		return 0;
	}
	con_hex2(" pas", "region=", (uint32_t)ZAP_REGION, "pas=", ZAP_PAS_ID);
	if (ri || rm)
		return 0;
	CPDETAIL(con_hex2(" region", "read=", *(volatile uint32_t *)ZAP_REGION, "step=", 1));
	volatile uint32_t *dst = (volatile uint32_t *)ZAP_REGION;	/* segment pa 0x5000 = region + 0 */
	dst[0] = 0x12345678u;
	__asm__ volatile("dsb sy" ::: "memory");
	CPDETAIL(con_hex2(" region", "wrote1=", dst[0], "step=", 2));
	for (uint32_t i = 0; i < 0x1000 / 4; i++) {
		uint32_t w = 0;
		for (unsigned k = 0; k < 4; k++)
			if (i * 4 + k < b02_size)
				w |= (uint32_t)zap_b02[i * 4 + k] << (8 * k);
		dst[i] = w;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	con_hex2(" region", "copied0=", dst[0], "step=", 3);
	gmu_oob(" oob gpu2", 16, 24);
	CPDETAIL(con_hex2(" gx", "sptprac=", rd(GMU_REG(GMU_SPTPRAC_PWR_CLK_STATUS)), "trust=", rd(GPU + 0xf400 * 4)));
	rpmh_bcm_vote("CE0", (1u << 30) | (1u << 29) | (1u << 14) | 1u);	/* commit, valid, x=1, y=1 */
	con_str(" pas", "AUTH_AND_RESET...");
	int64_t rr = scm(0x0205, 1, ZAP_PAS_ID, 0, 0, 0);		/* AUTH_AND_RESET */
	con_hex2(" pas", "auth=", (uint32_t)rr, "word0=", *(volatile uint32_t *)ZAP_REGION);
	return rr == 0;
}
#endif

#ifdef GPU_ZAP
/*
 * t17: the rest of Linux a6xx hw_init (a615: a615_hwcg, a630_protect,
 * gmu_cgc_mode 0x222, prim_fifo_threshold 0x18000; UBWC per the stock DT:
 * highest bank bit 14, UBWC 2.0, 32 B min access), in Linux's order around
 * the setup cp_test() already did. Tables generated from Linux
 * a6xx_catalog.c with offsets from registers/adreno/a6xx.xml.
 */
static const struct { uint32_t reg, val; } a615_hwcg[] = {
	{0x000b0, 0x02222222},	/* RBBM_CLOCK_CNTL_SP0 */
	{0x000b4, 0x02222220},	/* RBBM_CLOCK_CNTL2_SP0 */
	{0x000b8, 0x00000080},	/* RBBM_CLOCK_DELAY_SP0 */
	{0x000bc, 0x0000f3cf},	/* RBBM_CLOCK_HYST_SP0 */
	{0x000c0, 0x02222222},	/* RBBM_CLOCK_CNTL_TP0 */
	{0x000c1, 0x02222222},	/* RBBM_CLOCK_CNTL_TP1 */
	{0x000c4, 0x22222222},	/* RBBM_CLOCK_CNTL2_TP0 */
	{0x000c5, 0x22222222},	/* RBBM_CLOCK_CNTL2_TP1 */
	{0x000c8, 0x22222222},	/* RBBM_CLOCK_CNTL3_TP0 */
	{0x000c9, 0x22222222},	/* RBBM_CLOCK_CNTL3_TP1 */
	{0x000cc, 0x00022222},	/* RBBM_CLOCK_CNTL4_TP0 */
	{0x000cd, 0x00022222},	/* RBBM_CLOCK_CNTL4_TP1 */
	{0x000e0, 0x77777777},	/* RBBM_CLOCK_HYST_TP0 */
	{0x000e1, 0x77777777},	/* RBBM_CLOCK_HYST_TP1 */
	{0x000e4, 0x77777777},	/* RBBM_CLOCK_HYST2_TP0 */
	{0x000e5, 0x77777777},	/* RBBM_CLOCK_HYST2_TP1 */
	{0x000e8, 0x77777777},	/* RBBM_CLOCK_HYST3_TP0 */
	{0x000e9, 0x77777777},	/* RBBM_CLOCK_HYST3_TP1 */
	{0x000ec, 0x00077777},	/* RBBM_CLOCK_HYST4_TP0 */
	{0x000ed, 0x00077777},	/* RBBM_CLOCK_HYST4_TP1 */
	{0x000d0, 0x11111111},	/* RBBM_CLOCK_DELAY_TP0 */
	{0x000d1, 0x11111111},	/* RBBM_CLOCK_DELAY_TP1 */
	{0x000d4, 0x11111111},	/* RBBM_CLOCK_DELAY2_TP0 */
	{0x000d5, 0x11111111},	/* RBBM_CLOCK_DELAY2_TP1 */
	{0x000d8, 0x11111111},	/* RBBM_CLOCK_DELAY3_TP0 */
	{0x000d9, 0x11111111},	/* RBBM_CLOCK_DELAY3_TP1 */
	{0x000dc, 0x00011111},	/* RBBM_CLOCK_DELAY4_TP0 */
	{0x000dd, 0x00011111},	/* RBBM_CLOCK_DELAY4_TP1 */
	{0x0010b, 0x22222222},	/* RBBM_CLOCK_CNTL_UCHE */
	{0x0010c, 0x22222222},	/* RBBM_CLOCK_CNTL2_UCHE */
	{0x0010d, 0x22222222},	/* RBBM_CLOCK_CNTL3_UCHE */
	{0x0010e, 0x00222222},	/* RBBM_CLOCK_CNTL4_UCHE */
	{0x00110, 0x00000004},	/* RBBM_CLOCK_HYST_UCHE */
	{0x0010f, 0x00000002},	/* RBBM_CLOCK_DELAY_UCHE */
	{0x000f0, 0x22222222},	/* RBBM_CLOCK_CNTL_RB0 */
	{0x000f4, 0x00002222},	/* RBBM_CLOCK_CNTL2_RB0 */
	{0x000f8, 0x00002020},	/* RBBM_CLOCK_CNTL_CCU0 */
	{0x000f9, 0x00002220},	/* RBBM_CLOCK_CNTL_CCU1 */
	{0x000fa, 0x00002220},	/* RBBM_CLOCK_CNTL_CCU2 */
	{0x000fb, 0x00002220},	/* RBBM_CLOCK_CNTL_CCU3 */
	{0x00100, 0x00040f00},	/* RBBM_CLOCK_HYST_RB_CCU0 */
	{0x00101, 0x00040f00},	/* RBBM_CLOCK_HYST_RB_CCU1 */
	{0x00102, 0x00040f00},	/* RBBM_CLOCK_HYST_RB_CCU2 */
	{0x00103, 0x00040f00},	/* RBBM_CLOCK_HYST_RB_CCU3 */
	{0x00104, 0x05022022},	/* RBBM_CLOCK_CNTL_RAC */
	{0x00105, 0x00005555},	/* RBBM_CLOCK_CNTL2_RAC */
	{0x00106, 0x00000011},	/* RBBM_CLOCK_DELAY_RAC */
	{0x00107, 0x00445044},	/* RBBM_CLOCK_HYST_RAC */
	{0x00108, 0x04222222},	/* RBBM_CLOCK_CNTL_TSE_RAS_RBBM */
	{0x00114, 0x00222222},	/* RBBM_CLOCK_MODE_GPC */
	{0x00111, 0x00002222},	/* RBBM_CLOCK_MODE_VFD */
	{0x0010a, 0x00000000},	/* RBBM_CLOCK_HYST_TSE_RAS_RBBM */
	{0x00116, 0x04104004},	/* RBBM_CLOCK_HYST_GPC */
	{0x00113, 0x00000000},	/* RBBM_CLOCK_HYST_VFD */
	{0x0011c, 0x00000000},	/* RBBM_CLOCK_DELAY_HLSQ */
	{0x00109, 0x00004000},	/* RBBM_CLOCK_DELAY_TSE_RAS_RBBM */
	{0x00115, 0x00000200},	/* RBBM_CLOCK_DELAY_GPC */
	{0x00112, 0x00002222},	/* RBBM_CLOCK_DELAY_VFD */
	{0x00117, 0x00000002},	/* RBBM_CLOCK_DELAY_HLSQ_2 */
	{0x0011b, 0x00002222},	/* RBBM_CLOCK_MODE_HLSQ */
	{0x00118, 0x00000222},	/* RBBM_CLOCK_CNTL_GMU_GX */
	{0x00119, 0x00000111},	/* RBBM_CLOCK_DELAY_GMU_GX */
	{0x0011a, 0x00000555},	/* RBBM_CLOCK_HYST_GMU_GX */
};
static const uint32_t a630_protect[32] = {
	0x13fc0000,
	0x00140501,
	0x0bd0050b,
	0x8000050e,
	0x80000510,
	0x80000534,
	0x82080800,
	0x802008a0,
	0x809008ab,
	0x02b808de,
	0x81340900,
	0x89c8098d,
	0x80040e00,
	0x80300e03,
	0x830c3c00,
	0x7ffc3cc4,
	0x873c8630,
	0x80008e00,
	0x80008e08,
	0x807c8e50,
	0x876c9624,
	0x80049e70,
	0x861c9e78,
	0x873ca630,
	0x8000ae02,
	0x8cbcae50,
	0x8000b604,
	0x8004be02,
	0xdf7cbe20,
	0xaffcf000,
	0x7ffcfc00,
	0x80011c00
};

static void hwcg_on(void)
{
	wr(GMU_REG(0x23b09), 0x222);			/* GPU_GMU_AO_GMU_CGC_MODE_CNTL */
	wr(GMU_REG(0x23b0a), 0x111);			/* .. DELAY */
	wr(GMU_REG(0x23b0b), 0x555);			/* .. HYST */
	if (gpu_r(0x00ae) == 0x8aa8aa82u)		/* RBBM_CLOCK_CNTL already on */
		return;
	wr(GMU_REG(0x1a880), rd(GMU_REG(0x1a880)) & ~1u);	/* SPTPRAC clock off */
	for (unsigned i = 0; i < sizeof(a615_hwcg) / sizeof(a615_hwcg[0]); i++)
		gpu_w(a615_hwcg[i].reg, a615_hwcg[i].val);
	wr(GMU_REG(0x1a880), rd(GMU_REG(0x1a880)) | 1u);
	gpu_w(0x00ae, 0x8aa8aa82u);
}

static void hw_init_tail(void)
{
	gpu_w(0x9e00, 0x0018000);			/* PC_DBG_ECO_CNTL: prim fifo */
	/* (CP_AHB_CNTL done by the caller) */
	gpu_w(0x0500, 1);				/* RBBM_PERFCTR_CNTL */
	gpu_w(0x08d0, 0);				/* CP_PERFCTR_CP_SEL(0) = ALWAYS_COUNT */
	gpu_w(0x8e08, 1u << 4 | 1u << 1);		/* RB_NC_MODE_CNTL: amsbc, hbb_lo 1 */
	gpu_w(0xb604, 1u << 1);				/* TPL1_NC_MODE_CNTL */
	gpu_w(0xae02, 1u << 1);				/* SP_NC_MODE_CNTL */
	gpu_w(0x0e01, 1u << 21);			/* UCHE_MODE_CNTL */
	gpu_w(0x0534, 0);				/* RBBM_NC_MODE_CNTL */
	gpu_w(0x001f, (1u << 30) | 0x1fffff);		/* RBBM_INTERFACE_HANG_INT_CNTL */
	gpu_w(0x0e19, (1u << 7) | 1u);			/* UCHE_CLIENT_PF */
	wr(GMU_REG(0x23b0e), 0xff000000u);		/* GPU_GMU_AO_GPU_CX_BUSY_MASK */
	wr(GMU_REG(0x1f841), (rd(GMU_REG(0x1f841)) & ~0xffu) | (1u << 5));	/* POWER_COUNTER_SELECT_0 */
	wr(GMU_REG(0x1f840), 1);			/* POWER_COUNTER_ENABLE */
	gpu_w(0x084f, 0xb);				/* CP_PROTECT_CNTL: enable, fault, last span inf */
	for (unsigned i = 0; i < 31; i++)
		if (a630_protect[i])
			gpu_w(0x0850 + i, a630_protect[i]);
	gpu_w(0x0850 + 31, a630_protect[31]);
	gpu_w(0x0038, 0x3d0e242);			/* RBBM_INT_0_MASK: A6XX_INT_MASK */
}
#endif

/* the CP part of Linux a6xx hw_init + a6xx_cp_init + a ring write test */
static void cp_test(void)
{
	uint32_t sqe_size = (uint32_t)(sqe_fw_end - sqe_fw);
	CPDETAIL(con_hex2("SQE", "size=", sqe_size, "word1=", ((const uint32_t *)sqe_fw)[1]));
	if (sqe_size < 8 || sqe_size - 4 > GPU_RING || (sqe_size & 3)) {
		con_str("SQE", "bad firmware size: stop");
		return;
	}
	gpu_w(0x3c45, 0); (void)gpu_r(0x3c45);		/* GBIF_HALT */
	gpu_w(0x0016, 0); (void)gpu_r(0x0016);		/* RBBM_GBIF_HALT */
	gpu_w(0xf803, 0);				/* RBBM_SECVID_TSB_CNTL */
	gpu_w64(0xf800, 0);				/* TSB_TRUSTED_BASE */
	gpu_w(0xf802, 0);				/* TSB_TRUSTED_SIZE */
	static const uint16_t addr_mode[] = { 0x0842, 0x0c01, 0x8601, 0x8e05, 0x9e01, 0xbe05,
	    0xa601, 0x9601, 0x0e00, 0xae01, 0xb601, 0xf810 };
	for (unsigned i = 0; i < sizeof(addr_mode) / sizeof(addr_mode[0]); i++)
		gpu_w(addr_mode[i], 1);			/* 64-bit addressing */
#ifdef GPU_ZAP
	hwcg_on();
#endif
	gpu_w(0x0010, 3);				/* RBBM_VBIF_CLIENT_QOS_CNTL */
#ifdef GPU_ZAP
	gpu_w(0x050b, 0xffffffffu);			/* RBBM_PERFCTR_GPU_BUSY_MASKED */
#endif
	uint64_t trap = 0x1fffffffff000ull;
	gpu_w64(0x0e05, trap + 0xfc0);			/* UCHE_WRITE_RANGE_MAX */
	gpu_w64(0x0e09, trap);				/* UCHE_TRAP_BASE */
	gpu_w64(0x0e07, trap);				/* UCHE_WRITE_THRU_BASE */
	gpu_w64(0x0e0b, 0x100000);			/* UCHE_GMEM_RANGE_MIN */
	gpu_w64(0x0e0d, 0x100000 + 0x80000 - 1);	/* .. 512 KiB GMEM */
	gpu_w(0x0e18, 0x804);				/* UCHE_FILTER_CNTL */
	gpu_w(0x0e17, 0x4);				/* UCHE_CACHE_WAYS */
	gpu_w(0x08c2, 0x010000c0);			/* CP_ROQ_THRESHOLDS_2 */
	gpu_w(0x08c1, 0x8040362c);			/* CP_ROQ_THRESHOLDS_1 */
	gpu_w(0x08c3, 128);				/* CP_MEM_POOL_SIZE */
	gpu_w(0x098d, 1);				/* CP_AHB_CNTL: AHB error response */
#ifdef GPU_ZAP
	hw_init_tail();
#endif

	volatile uint32_t *mem = (volatile uint32_t *)gpu_mem;
	for (uint32_t i = 0; i < (sqe_size - 4) / 4; i++)
		mem[i] = ((const uint32_t *)sqe_fw)[i + 1];
	volatile uint32_t *ring = mem + GPU_RING / 4;
	for (unsigned i = 0; i < 1024; i++)
		ring[i] = 0;
	mem[GPU_TARGET / 4] = 0xdeadbeefu;
	__asm__ volatile("dsb sy" ::: "memory");

	gpu_w64(0x0830, GPU_IOVA);			/* CP_SQE_INSTR_BASE */
	gpu_w64(0x0800, GPU_IOVA + GPU_RING);		/* CP_RB_BASE */
	gpu_w(0x0802, (1u << 27) | (2u << 8) | 9u);	/* CP_RB_CNTL: no rptr update, 32 B blocks, 4 KiB */
#ifdef GPU_ZAP
	/*
	 * t19: downstream KGSL (the driver this TZ shipped with) loads the zap
	 * in a6xx_microcode_load, after CP_SQE_INSTR_BASE and before the SQE is
	 * started; mainline does it after CP_ME_INIT. t15..t18 used mainline's
	 * order and AUTH_AND_RESET never returned. Use downstream's.
	 */
	int zap_ok = zap_load();
	if (!zap_ok)
		return;
#endif
	gpu_w(0x0808, 1);				/* CP_SQE_CNTL: start the SQE */

	unsigned n = 0;
	ring[n++] = pkt7(0x48, 8);			/* CP_ME_INIT, as a6xx_cp_init */
	ring[n++] = 0x0000002f;
	ring[n++] = 0x00000003;
	ring[n++] = 0x20000000;
	ring[n++] = 0; ring[n++] = 0; ring[n++] = 0; ring[n++] = 0; ring[n++] = 0;
	unsigned after_init = n;
#ifndef GPU_ZAP		/* t17: Linux runs only CP_ME_INIT, waits for idle, then loads the zap */
	ring[n++] = pkt7(0x3d, 3);			/* CP_MEM_WRITE */
	ring[n++] = GPU_IOVA + GPU_TARGET;
	ring[n++] = 0;
	ring[n++] = 0xc0ffee15u;
#endif
	__asm__ volatile("dsb sy" ::: "memory");
	gpu_w(0x0807, n);				/* CP_RB_WPTR */

	uint32_t rptr = 0, got = 0;
	unsigned ms;
	for (ms = 0; ms < 200; ms++) {
		rptr = gpu_r(0x0806);
		got = mem[GPU_TARGET / 4];
#ifdef GPU_ZAP
		if (rptr == n && !(gpu_r(RBBM_STATUS) & (1u << 23)))	/* GPU_BUSY_IGN_AHB clear: idle */
			got = 0xc0ffee15u;
#endif
		if (rptr == n && got == 0xc0ffee15u)
			break;
		udelay(1000);
	}
	con_hex2("CP", "rptr=", rptr, "wptr=", n);
	CPDETAIL(con_hex2(" ring", "init-end=", after_init, "ms=", ms));
	con_hex2(" target", "value=", got, "rbbm=", gpu_r(RBBM_STATUS));
	CPDETAIL(con_hex2(" fault", "cphw=", gpu_r(0x0821), "int0=", gpu_r(RBBM_INT_0_STATUS)));
	CPDETAIL(con_hex2(" cp", "status1=", gpu_r(0x0825), "trust=", gpu_r(0xf400)));
	CPDETAIL(con_hex2(" cb1", "fsr=", rd(GSMMU_CB(GPU_CB) + 0x58), "far=", rd(GSMMU_CB(GPU_CB) + 0x60)));
#ifdef GPU_ZAP
	con_str("CP", got == 0xc0ffee15u ? "ME_INIT done, GPU idle" : "CP not idle after ME_INIT");
#else
	con_str("CP", got == 0xc0ffee15u ? "RING TEST PASSED: GPU WROTE MEMORY" : "ring test failed");
#endif
#ifdef GPU_ZAP
	if (got != 0xc0ffee15u)
		return;
	unsigned start = n;
	ring[n++] = pkt7(0x66, 1);			/* CP_SET_SECURE_MODE */
	ring[n++] = 0;
	ring[n++] = pkt7(0x3d, 3);			/* CP_MEM_WRITE */
	ring[n++] = GPU_IOVA + GPU_TARGET + 4;
	ring[n++] = 0;
	ring[n++] = 0x5ec0ff15u;
	mem[GPU_TARGET / 4 + 1] = 0xdeadbeefu;
	__asm__ volatile("dsb sy" ::: "memory");
	gpu_w(0x0807, n);
	for (ms = 0; ms < 200; ms++) {
		rptr = gpu_r(0x0806);
		got = mem[GPU_TARGET / 4 + 1];
		if (rptr == n && got == 0x5ec0ff15u)
			break;
		udelay(1000);
	}
	con_hex2("CP2", "rptr=", rptr, "wptr=", n);
	con_hex2(" from", "start=", start, "ms=", ms);
	con_hex2(" target", "value=", got, "cphw=", gpu_r(0x0821));
	con_hex2(" trust", "now=", gpu_r(0xf400), "int0=", gpu_r(RBBM_INT_0_STATUS));
	con_str("ZAP", gpu_r(0xf400) == 0 && got == 0x5ec0ff15u ? "SECURE MODE OFF, GPU STILL RUNS" : "secure mode not left");
#endif
#ifdef GPU_TRUST
	/*
	 * t16: the zap shader's AUTH_AND_RESET hangs (t15..t15f). Linux's
	 * no-zap path instead: RBBM_SECVID_TRUST_CNTL = 0 from the CPU. On
	 * a5xx (ios-sailfish) this reset the phone. Then a second CP_MEM_WRITE.
	 */
	if (got != 0xc0ffee15u)
		return;
	con_hex2("TRUST", "before=", gpu_r(0xf400), "writing=", 0);
	gpu_w(0xf400, 0);
	con_hex2("TRUST", "after=", gpu_r(0xf400), "int0=", gpu_r(RBBM_INT_0_STATUS));
	unsigned start = n;
	ring[n++] = pkt7(0x3d, 3);			/* CP_MEM_WRITE */
	ring[n++] = GPU_IOVA + GPU_TARGET + 4;
	ring[n++] = 0;
	ring[n++] = 0x5ec0ff16u;
	mem[GPU_TARGET / 4 + 1] = 0xdeadbeefu;
	__asm__ volatile("dsb sy" ::: "memory");
	gpu_w(0x0807, n);
	for (ms = 0; ms < 200; ms++) {
		rptr = gpu_r(0x0806);
		got = mem[GPU_TARGET / 4 + 1];
		if (rptr == n && got == 0x5ec0ff16u)
			break;
		udelay(1000);
	}
	con_hex2("CP2", "rptr=", rptr, "wptr=", n);
	con_hex2(" from", "start=", start, "ms=", ms);
	con_hex2(" target", "value=", got, "cphw=", gpu_r(0x0821));
	con_hex2(" trust", "now=", gpu_r(0xf400), "int0=", gpu_r(RBBM_INT_0_STATUS));
	con_str("TRUST", gpu_r(0xf400) == 0 && got == 0x5ec0ff16u ? "SECURE MODE OFF, GPU STILL RUNS" : "secure mode not left");
#endif
}
#endif

static void gx_on_and_probe(void)
{
	unsigned gx = 0, mx = 0;
	if (!gx_vote(48, &gx, &mx)) {		/* RPMH_REGULATOR_LEVEL_MIN_SVS */
		con_str("GX", "no ARC level for MIN_SVS: stop");
		return;
	}
	/* a6xx_gmu_gfx_rail_on */
	wr(GMU_REG(GMU_BOOT_SLUMBER_OPTION), 0);
	wr(GMU_REG(GMU_GX_VOTE_IDX), gx);
	wr(GMU_REG(GMU_MX_VOTE_IDX), mx);
	udelay(50000);
	if (!gmu_oob(" oob boot", 22, 30)) {
		con_str("GX", "no BOOT_SLUMBER ack: stop");
		return;
	}
	/* a6xx_sptprac_enable */
	wr(GMU_REG(GMU_GX_SPTPRAC_POWER_CONTROL), 0x778000);
	int sp = 0;
	for (unsigned i = 0; i < 10 && !sp; i++)
		sp = wait_bits(GMU_REG(GMU_SPTPRAC_PWR_CLK_STATUS), 0x38, 0x28);
	con_hex2(" sptprac", "ok=", sp, "status=", rd(GMU_REG(GMU_SPTPRAC_PWR_CLK_STATUS)));
	/* a6xx_gmu_hfi_start */
	wr(GMU_REG(GMU_HFI_CTRL_INIT), 1);
	int hfi = 0;
	for (unsigned i = 0; i < 5 && !hfi; i++)
		hfi = wait_bits(GMU_REG(GMU_HFI_CTRL_STATUS), 1, 1);
	con_hex2(" hfi", "ok=", hfi, "status=", rd(GMU_REG(GMU_HFI_CTRL_STATUS)));
	/*
	 * t14..t15c: GPU_SET not acked, GMU2HOST bit 23 = CM3_FAULT (the GMU
	 * crashed); t13 (slow, lots of console output between steps) got the
	 * ack. t15d: 50 ms before the request, and the state just before it.
	 */
	/* t15d: the GMU is clean before GPU_SET and faults (CM3_FAULT) on it: send Linux's HFI v1 setup first */
	hfi_start_v1();
	gmu_oob(" oob gpu", 16, 24);

	uint32_t st = rd(GMU_REG(GMU_SPTPRAC_PWR_CLK_STATUS));
	con_hex2("GX", "gdscr=", rd(GPUCC + GPUCC_GX_GDSCR), "sptprac=", st);
	if (st & ((1u << 6) | (1u << 7))) {	/* GX_HM_GDSC_POWER_OFF | GX_HM_CLK_OFF */
		con_str("GX", "GPU core still off: registers not read");
		return;
	}
	con_hex2(" rbbm", "status=", rd(GPU + RBBM_STATUS * 4), "int0=", rd(GPU + RBBM_INT_0_STATUS * 4));
	wr(GPU + CP_SCRATCH0 * 4, 0x615c0de5u);
	uint32_t sc = rd(GPU + CP_SCRATCH0 * 4);
	con_hex2(" scratch", "wrote=", 0x615c0de5u, "read=", sc);
	con_str("GX", sc == 0x615c0de5u ? "GPU CORE REGISTERS ALIVE" : "scratch readback differs");
#ifdef GPU_CP
	if (sc == 0x615c0de5u)
		cp_test();
#endif
}
#endif

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
	DETAIL(con_hex2(" itcm", "mismatch=", bad, "dtcm+ff8=", rd(GMU_REG(GMU_CM3_DTCM_START + 0xff8))));

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
#ifdef GPU_GX_ON
	gmu_rpmh_init();
	hfi_table_init();
	wr(GMU_REG(GMU_PWR_COL_INTER_FRAME_CTRL), 0x9c40400);	/* idle level: active */
	wr(GMU_REG(GMU_RPMH_CTRL), rd(GMU_REG(GMU_RPMH_CTRL)) |
	    (1u << 0) | (1u << 4) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11));
#endif

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
	DETAIL(con_hex2(" fw", "version=", rd(GMU_REG(GMU_CORE_FW_VERSION)), "hfi=", rd(GMU_REG(GMU_HFI_CTRL_STATUS))));
	DETAIL(con_hex2(" intr", "g2h=", rd(GMU_REG(GMU_GMU2HOST_INTR_INFO)), "ao=", rd(GMU_REG(GMU_AO_HOST_INTR_STATUS))));
	con_hex2(" cb1", "fsr=", rd(GSMMU_CB(GMU_CB) + 0x58), "far=", rd(GSMMU_CB(GMU_CB) + 0x60));
	DETAIL(con_hex2(" mem", "log0=", *(volatile uint32_t *)(gmu_mem + 0x5000), "hfi0=", *(volatile uint32_t *)(gmu_mem + 0x1000)));
	con_str("GMU", res == 0xbabefaceu ? "FIRMWARE RUNNING" : "no 0xbabeface");
#ifdef GPU_GX_ON
	if (res == 0xbabefaceu)
		gx_on_and_probe();
#endif
}
#endif

void test_gpu(void)
{
	con_str("GPU", "Adreno 615 probe t10 (read-mostly)");
	DETAIL(con_hex2("GCC", "vote1=", rd(GCC + GCC_VOTE_1), "axi=", rd(GCC + GCC_DDRSS_GPU_AXI)));
	if (!branch_on(" cfgahb", GCC + GCC_GPU_CFG_AHB, 0)) {
		con_str("GPU", "gpu_cfg_ahb_clk stays off: GPU CC unreachable, stop");
		return;
	}
	wr(GCC + GCC_VOTE_1, rd(GCC + GCC_VOTE_1) | (1u << 15) | (1u << 16));
	branch_on(" memnoc", GCC + GCC_GPU_MEMNOC_GFX, 1);
	branch_on(" snocdvm", GCC + GCC_GPU_SNOC_DVM, 0);
	udelay(10);

	DETAIL(con_hex2("GPUCC", "cx=", rd(GPUCC + GPUCC_CX_GDSCR), "gx=", rd(GPUCC + GPUCC_GX_GDSCR)));
	DETAIL(con_hex2(" gmuclk", "cbcr=", rd(GPUCC + GPUCC_CX_GMU_CBCR), "cxo=", rd(GPUCC + GPUCC_CXO_CBCR)));
	DETAIL(con_hex2(" rcg", "cmd=", rd(GPUCC + GPUCC_GMU_RCG), "cfg=", rd(GPUCC + GPUCC_GMU_RCG + 4)));
	DETAIL(con_hex2(" pll1", "mode=", rd(GPUCC + GPUCC_PLL1_MODE), "hwctl=", rd(GPUCC + GPUCC_CX_GDS_HW_CTRL)));

	/* CX GDSC on, as gdsc_enable() for gpu_cx_gdsc */
	uint32_t cx = rd(GPUCC + GPUCC_CX_GDSCR);
	if (cx & GDSC_COLLAPSE)
		wr(GPUCC + GPUCC_CX_GDSCR, cx & ~GDSC_COLLAPSE);
	int cx_on = wait_bits(GPUCC + GPUCC_CX_GDSCR, GDSC_PWR_ON, GDSC_PWR_ON) &&
	    wait_bits(GPUCC + GPUCC_CX_GDS_HW_CTRL, GDSC_PWR_ON, GDSC_PWR_ON);
	DETAIL(con_hex2("CXGDSC", "gdscr=", rd(GPUCC + GPUCC_CX_GDSCR), "hwctl=", rd(GPUCC + GPUCC_CX_GDS_HW_CTRL)));
	if (!cx_on) {
		con_str("GPU", "CX GDSC did not report power on: stop");
		return;
	}
	if (!branch_on(" cxo", GPUCC + GPUCC_CXO_CBCR, 0) ||
	    !branch_on(" cxgmu", GPUCC + GPUCC_CX_GMU_CBCR, 0)) {
		con_str("GPU", "GMU clocks stay off: stop");
		return;
	}

	DETAIL(smmu_dump(GSMMU));
	DETAIL(con_hex2("GSMMU", "cb1 sctlr=", rd(GSMMU_CB(GMU_CB)), "cbar=", rd(GSMMU_GR1 + 4 * GMU_CB)));

	DETAIL(con_hex2("GMU", "sysreset=", rd(GMU_REG(GMU_CM3_SYSRESET)),
	    "bootcfg=", rd(GMU_REG(GMU_CM3_BOOT_CONFIG))));
	DETAIL(con_hex2(" fw", "busy=", rd(GMU_REG(GMU_CM3_FW_BUSY)),
	    "init=", rd(GMU_REG(GMU_CM3_FW_INIT_RESULT))));
	DETAIL(con_hex2(" fw", "version=", rd(GMU_REG(GMU_CORE_FW_VERSION)),
	    "cm3cfg=", rd(GMU_REG(GMU_CM3_CFG))));
#ifdef GPU_GMU_BOOT
	gmu_boot();
#endif
	con_str("GPU", "probe done");
}
