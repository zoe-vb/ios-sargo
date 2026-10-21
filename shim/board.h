#pragma once
/*
 * Per-board constants, selected at build time (build.sh BOARD=sweet|sargo).
 * Everything that can be read from the live FDT is read from it instead.
 */

#if defined(BOARD_SARGO)
/* Google Pixel 3a (sargo), SDM670. Mainline sdm670-google-{common,sargo}.dts */
#define BOARD_NAME		"sargo"
#define FB_H			2220
#define IMEM_RESTART_FALLBACK	0UL		/* unknown: taken from the live FDT */
#define SDHC1_BASE		0x007c4000UL	/* sdhc_1 hc_mem (eMMC) */
#define CON_SCALE		2		/* the bring-up screen needs ~60 lines */
#else
/* Xiaomi Redmi Note 10 Pro (sweet), SM7150 */
#define BOARD_SWEET		1
#define BOARD_NAME		"sweet"
#define FB_H			2400
#define IMEM_RESTART_FALLBACK	0x146aa65cUL	/* qcom,msm-imem restart_reason@65c */
#endif

/* Common to both: reserved-memory cont_splash / simplefb region, a8r8g8b8 */
#define FB_BASE			0x9c000000UL
#define FB_W			1080
