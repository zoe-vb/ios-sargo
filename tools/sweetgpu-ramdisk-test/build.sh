#!/bin/sh
# Build the static ramdisk SweetGPU test (iOS arm64, PID 1, no libc/dyld).
#   tools/sweetgpu-ramdisk-test/build.sh -> out/sweetgpu-ramdisk-test
set -e
H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/../.." && pwd); mkdir -p "$H/out"
xcrun -sdk iphoneos clang -arch arm64 -c -O2 -fno-stack-protector -fno-jump-tables \
	-ffreestanding -nostdlib "$H/init.c" -o "$H/out/init.o"
python3 "$R/tools/mkinit.py" "$H/out/init.o" "$H/out/sweetgpu-ramdisk-test"
echo "built $H/out/sweetgpu-ramdisk-test"
