# Port: Google Pixel 3a (`sargo`, SDM670)

Second board for the ios-sweet XNU + iOS 16 stack. The sweet (SM7150) and
sargo (SDM670) share most Qualcomm blocks, so the plan is to reuse the shim,
the XNU patches and the userland, and to make the board-specific parts selectable.

## Hardware facts (from mainline Linux, not yet checked on the phone)

Sources: `arch/arm64/boot/dts/qcom/sdm670.dtsi`, `sdm670-google-common.dtsi`,
`sdm670-google-sargo.dts`.

| Block | sargo | Same as sweet? |
|---|---|---|
| GICv3 | GICD `0x17a00000`, GICR `0x17a60000` (8 x 128K) | yes |
| Timer | PPI 1/2/3/0, vtimer INTID 19 | yes |
| CPUs | 8x Kryo 360 (6x A55 + 2x A75), MPIDR `0x000`..`0x700`, PSCI 1.0 smc | layout yes, A75 not A76 |
| Framebuffer | `0x9c000000` (fb_mem `0x2400000`), 1080x2220, stride 4320, a8r8g8b8 | base yes, height 2220 not 2400 |
| Storage | eMMC 5.1, `sdhc_1` hc `0x7c4000`, cqhci `0x7c5000`, ice `0x7c8000`, SPI 641/644, HS400 1.8V, CQE | **no** (UFS on sweet) |
| USB | DWC3 `0xa600000`, Qualcomm wrapper/QSCRATCH `0xa6f8800`, QUSB2 PHY `0x88e2000` | DWC3 + wrapper yes |
| Touch | Synaptics RMI4 (`syna,rmi4-i2c`) on i2c9 `0xa84000` (QUP1 SE1) @0x20, IRQ TLMM 125 (falling), reset gpio99, 1.8V via PM660 GPIO 12 | **no** (Goodix GT9896 on sweet) |
| Keys | power = PM660 PON pwrkey, vol down = PON resin, **vol up = PM660L GPIO 7** | partly (PMIC differs) |
| PMICs | PM660 + PM660L over SPMI (`0xc440000`) | **no** (PM6150 family) |
| Panel | Samsung S6E3FA7 (AMS559NK06) OLED, left on by ABL | no |
| RAM | 4 GB. Live `/memory` (stage 1): `0x80000000+0x80000000`, `0x1_00000000+0x7e2c0000`. The high bank has no carve-outs and becomes XNU RAM (~1.97 GB) | no (6/8 GB) |
| Fixed carve-outs | hyp `0x85700000` ... qseecom `0x9e400000`, debug-info `0xa1800000`; modem/ADSP etc. moved to `0x8b000000`..`0x97a17000` on Google boards | different |

## Stage 1 results (2026-10-21, boot 0001, photo kept off git)

ABL `b4s4-0.1-5653622` (2019 launch), stock boot.img = Android 9 / 2019-03, header v1, one
appended DTB. **`fastboot boot` of an uncompressed Image-dtb hangs ABL** (no OKAY/FAIL, the
UI stays responsive, fastboot stops answering). With a gzip kernel (`Image.gz-dtb`, the
postmarketOS layout) it answers `Booting OKAY` after 3.8 s and the shim runs. All lines observed on screen:

- live FDT model `Google Inc. MSM sdm670 S4 PVT v1.0`, qcom,board-id `0x41e05 0` (dtbo overlay applied)
- initrd at `0x84700000`, DTB at `0x84500000`
- IMEM restart reason `0x146bf65c` (found in the FDT)
- boot CPU MIDR `0x517f803c` (Kryo 360 Silver), MPIDR `0x81000000` (MT bit set, Aff1 = core)
- ID_AA64MMFR0 PARange 2 (40 bit), TGran16 = 1 (16K pages supported), CNTFRQ 19.2 MHz, EL1
- PSCI 1.1, SMCCC_VERSION not supported (-1); CPU_ON ok for all 7, MPIDRs `0x81000100`..`0x81000700`
- GICD CTLR `0x10 -> 0x12`, GICR for CPU0 at `0x17a60000`, vtimer 30 IRQs / 300 ms, SGI 10/10
- sdhc_1: SDHCI version `0x6c02` (spec 3.0), CAPS `0x742dcbb2 / 0x0000808f` (ADMA2, 64-bit,
  base clock field 0xcb), PRESENT `0x01f800f0` (card inserted, DAT lines idle), CLOCK `0x3`
  (internal clock on and stable, **SD clock off**), host ctl/ctl2/int-enable all 0, CQHCI 5.10 disabled.
  -> ABL deinitialised the host. The driver must re-initialise the bus (clock, power, width, timing).
  Whether the card itself is still in transfer state is unknown; CMD0 re-init is the safe path.

## Stage plan

1. **Shim stage 1** (`BOARD=sargo shim/build.sh`): boots via `fastboot boot`, draws on
   the splash framebuffer and reads the live FDT. It shows the model and board-id,
   the `/memory` banks, the IMEM restart reason, MIDR/MPIDR, 16K granule support,
   PSCI, SMP (CPU_ON for all 7 secondaries), GIC timer and SGI interrupts, and a
   read-only dump of the eMMC host registers. Photograph the screen. **Status: done, see results above.**
2. **ADT for sargo**: `mkadt.py` board table: memory range (from stage 1), 1080x2220
   fbcon, CPU frequencies (A55 1.7 GHz, A75 2.0 GHz), compatible `google,sargo`, `sweet-sdhc`
   node in place of `sweet-ufs`. Boot XNU to the framebuffer console with a ramdisk root
   (`rd=md0`, patches 0021/0022/0024). This needs no storage driver.
3. **eMMC**: SDHCI driver for XNU (`bsd/dev/sweet_sdhc.c`, the same block-device
   interface as `sweet_ufs.c`). Start PIO/ADMA2 in the mode ABL left (stage 1 shows
   whether clocks/card are still up), no CQE, no ICE. Then HFS+ root in `userdata`.
4. **Input**: RMI4 F11/F12 touch over GENI I2C (reuse the GENI code from patch 0063),
   keys via PM660 PON + PM660L GPIO 7.
5. **USB networking/live log**: the DWC3 code should port as-is (same wrapper address);
   check the QUSB2 PHY state ABL leaves.

## Testing on the phone

You need an unlocked bootloader (`fastboot flashing unlock`). Verizon units can't be
unlocked. Stage 1 to 2 write **nothing** to the phone: `fastboot boot` only.

```sh
# Stock boot.img: from the phone (adb root / recovery) or from Google's factory image
# for your current build. Put it in backup/ (gitignored), then:
python3 tools/unpack_bootv2.py backup/sargo-boot.img dtb/sargo     # -> dtb/sargo/dtb (appended DTBs)
cat dtb/sargo/header.txt   # header_version, kernel/ramdisk addresses, os_version
BOARD=sargo shim/build.sh  # (LLVM=1 on macOS with lld + llvm-objcopy installed)
fastboot boot shim/out/sargo/shim-boot.img
```

If `header.txt` shows `header_version: 2`, build with `HEADER=v2`. If ABL rejects the
image, match `--os_version`/`--os_patch_level` to header.txt (`OS_VERSION=`, `PATCH_LEVEL=`).

Expected: the screen turns red and fills with diagnostic lines. After about 3 minutes it
resets to the bootloader (via PSCI reset; IMEM reason if found in the FDT). If the screen
stays on the Google logo, the image did not start. If it freezes partway, the last line
shows how far it got (the eMMC dump runs last because it can hang if the SDCC clocks are off).

## Open questions

- Does ABL leave the eMMC host clocked and the card in transfer state after `fastboot boot`?
- Exact `/memory` layout: how much contiguous RAM is above the carve-outs (the iPhone X has 3 GB)?
- A/B: `userdata` is shared between slots; the iOS root goes there the same way as on sweet,
  **only after** explicit approval (it erases Android user data).
