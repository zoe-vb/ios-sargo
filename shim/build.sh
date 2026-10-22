#!/bin/bash
# Build the shim and wrap it in a boot.img that mirrors the stock header.
#   BOARD=sweet|sargo (default sweet)  DTB=path to the stock DTB
#   LLVM=1: compile with clang (e.g. Apple clang on macOS); link with ld.lld + llvm-* if
#     present, else with GNU binutils $BINUTILS (default aarch64-elf-, MacPorts aarch64-elf-binutils)
#   mkbootimg: AOSP's if on PATH, else tools/mkbootimg.py
#   HEADER=v1|v2 (sargo default v1: stock Pixel 3a boot.img, DTB appended to the kernel)
#   KGZ=1: gzip the kernel before appending the DTB (v1 only)
#   EMBED=1: append the xnu payload to the Image (image_size covers it) instead of using it
#     as the ramdisk (sargo default: ABL's ramdisk slot ends 16 MiB later at hyp_region)
set -euo pipefail
cd "$(dirname "$0")"
BOARD=${BOARD:-sweet}
case $BOARD in
sweet) BDEF=; DTB=${DTB:-../dtb/boot_unpacked/dtb}; HEADER=${HEADER:-v2}
	DRV=ufs.c; OS_VERSION=17.0.0; PATCH_LEVEL=2035-06 ;;
sargo) BDEF=-DBOARD_SARGO; DTB=${DTB:-../dtb/sargo/dtb}; HEADER=${HEADER:-v1}
	# ABL b4s4-0.1-5653622 hangs on "fastboot boot" of an uncompressed Image-dtb
	KGZ=${KGZ-1}; EMBED=${EMBED-1}
	DRV="emmc.c touch.c gpu.c"; OS_VERSION=${OS_VERSION:-9.0.0}; PATCH_LEVEL=${PATCH_LEVEL:-2019-03} ;;	# = stock boot.img header
*) echo "unknown BOARD=$BOARD" >&2; exit 1 ;;
esac
if [ -n "${LLVM:-}" ]; then
	# no switch lookup tables: they hold absolute pointers
	CC="clang --target=aarch64-none-elf -fno-jump-tables"
	if command -v ld.lld >/dev/null; then
		LD="ld.lld"; OBJCOPY=llvm-objcopy; READELF=llvm-readelf; NM=llvm-nm
	else
		B=${BINUTILS:-aarch64-elf-}
		LD="${B}ld --no-warn-rwx-segments"; OBJCOPY=${B}objcopy; READELF=${B}readelf; NM=${B}nm
	fi
else
	CC=aarch64-linux-gnu-gcc; LD=
	OBJCOPY=aarch64-linux-gnu-objcopy; READELF=aarch64-linux-gnu-readelf; NM=aarch64-linux-gnu-nm
fi
command -v mkbootimg >/dev/null && MKBOOTIMG=mkbootimg || MKBOOTIMG="python3 ../tools/mkbootimg.py"
# TOUCH=1 (sargo): the diagnostic screen runs the touch probe instead of the eMMC test
[ -n "${TOUCH:-}" ] && BDEF="$BDEF -DBRINGUP_TOUCH"
# TOUCH=dump: only dump the SMMU stream routing
[ "${TOUCH:-}" = dump ] && BDEF="$BDEF -DTOUCH_SMMU_DUMP"
# TOUCH=bb: bit-banged I2C on gpio6/7 instead of GENI/GPI
[ "${TOUCH:-}" = bb ] && BDEF="$BDEF -DTOUCH_BB"
# GPU=1 (sargo): the diagnostic screen runs the Adreno/GMU probe (shim/gpu.c)
[ -n "${GPU:-}" ] && BDEF="$BDEF -DBRINGUP_GPU"
# GPU=gmu: also boot the GMU (GPU=gx: then GX power through it; GPU=cp: then SQE + ring test; GPU=zap: then the zap shader); embeds research/sargo-gpu-fw/a630_gmu.bin (not in git)
GMUFW=
if [ "${GPU:-}" = gmu ] || [ "${GPU:-}" = gx ] || [ "${GPU:-}" = cp ] || [ "${GPU:-}" = zap ]; then
	GMUFW=$(cd .. && pwd)/research/sargo-gpu-fw/a630_gmu.bin
	[ -f "$GMUFW" ] || { echo "error: $GMUFW missing (docs/SARGO.md, GPU bring-up)" >&2; exit 1; }
	BDEF="$BDEF -DGPU_GMU_BOOT"
	[ "$GPU" = gx ] && BDEF="$BDEF -DGPU_GX_ON"
	[ "$GPU" = cp ] && BDEF="$BDEF -DGPU_GX_ON -DGPU_CP"
	[ "$GPU" = zap ] && BDEF="$BDEF -DGPU_GX_ON -DGPU_CP -DGPU_ZAP"
fi
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra $BDEF"
# sweet keeps the historical out/ (NOTES.md refers to it)
[ $BOARD = sweet ] && OUT=out || OUT=out/$BOARD
mkdir -p $OUT
OBJS=()
for f in start.S shim.c console.c mmu.c fdt.c loader.c $DRV; do
	$CC $CFLAGS -c $f -o $OUT/${f%.*}.o
	OBJS+=($OUT/${f%.*}.o)
done
if [ -n "$GMUFW" ]; then
	printf '\t.section .rodata\n\t.balign 16\n\t.globl gmu_fw, gmu_fw_end\ngmu_fw:\n\t.incbin "%s"\ngmu_fw_end:\n' "$GMUFW" > $OUT/gmufw.S
	printf '\t.balign 16\n\t.globl sqe_fw, sqe_fw_end\nsqe_fw:\n\t.incbin "%s"\nsqe_fw_end:\n' "${GMUFW%/*}/a630_sqe.fw" >> $OUT/gmufw.S
	for z in mdt b02; do
		printf '\t.balign 16\n\t.globl zap_%s, zap_%s_end\nzap_%s:\n\t.incbin "%s"\nzap_%s_end:\n' $z $z $z "${GMUFW%/*}/a615_zap.$z" $z >> $OUT/gmufw.S
	done
	$CC $CFLAGS -c $OUT/gmufw.S -o $OUT/gmufw.o
	OBJS+=($OUT/gmufw.o)
fi
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$($READELF -r "${OBJS[@]:1}" | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
if [ -n "$LD" ]; then
	$LD -static -T shim.ld --build-id=none -N "${OBJS[@]}" -o $OUT/shim.elf
else
	$CC $CFLAGS -static -T shim.ld -Wl,--no-warn-rwx-segments -Wl,--build-id=none -Wl,-N "${OBJS[@]}" -o $OUT/shim.elf
fi
$OBJCOPY -O binary $OUT/shim.elf $OUT/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$(( 0x$($NM $OUT/shim.elf | awk '$3=="_end"{print $1}') ))
python3 -c 'import os, sys; os.truncate(sys.argv[1], int(sys.argv[2]))' $OUT/Image "$size"
# Ramdisk: an xnu payload (tools/mkpayload.py) if given as $1, else an empty
# newc cpio (ABL dislikes a zero-size ramdisk on some builds) -> diagnostics.
if [ -n "${1:-}" ] && [ -n "${EMBED:-}" ]; then
	# payload 4K-aligned after the zero-padded image; header image_size grows to cover it
	python3 - $OUT/Image "$1" <<'PY'
import struct, sys
img, pay = bytearray(open(sys.argv[1], 'rb').read()), open(sys.argv[2], 'rb').read()
img += b'\0' * (-len(img) % 0x1000) + pay
img += b'\0' * (-len(img) % 0x1000)
struct.pack_into('<Q', img, 0x10, len(img))
open(sys.argv[1], 'wb').write(img)
PY
	set -- ""
fi
if [ -n "${1:-}" ]; then
	cp "$1" $OUT/ramdisk.bin
else
	(cd $OUT && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi
[ -f "$DTB" ] || { echo "error: stock DTB $DTB missing (see NOTES.md / docs/SARGO.md)" >&2; exit 1; }
if [ "$HEADER" = v2 ]; then
	$MKBOOTIMG --header_version 2 --kernel $OUT/Image --ramdisk $OUT/ramdisk.bin \
		--dtb "$DTB" --base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 \
		--tags_offset 0x100 --dtb_offset 0x1f00000 --pagesize 4096 \
		--os_version $OS_VERSION --os_patch_level $PATCH_LEVEL \
		--cmdline "console=null" -o $OUT/shim-boot.img
else
	# Header v1, Image-dtb: ABL finds the DTB(s) right after the kernel.
	# KGZ=1: Image.gz-dtb (the layout postmarketOS boots on sargo).
	if [ -n "${KGZ:-}" ]; then
		gzip -9 -n -c $OUT/Image > $OUT/Image.gz
		cat $OUT/Image.gz "$DTB" > $OUT/Image-dtb
	else
		cat $OUT/Image "$DTB" > $OUT/Image-dtb
	fi
	$MKBOOTIMG --header_version 1 --kernel $OUT/Image-dtb --ramdisk $OUT/ramdisk.bin \
		--base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 \
		--tags_offset 0x100 --pagesize 4096 \
		--os_version $OS_VERSION --os_patch_level $PATCH_LEVEL \
		--cmdline "console=null" -o $OUT/shim-boot.img
fi
# Host render of the console layout.
cc -O1 -Wall $BDEF -o $OUT/host_test console.c host_test.c && ./$OUT/host_test
ls -l $OUT/Image $OUT/shim-boot.img
