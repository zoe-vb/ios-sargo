#!/bin/sh
# Build sweetgpu-test for iOS 16 arm64 with the system Xcode.
#   tools/sweetgpu-test/build.sh -> tools/sweetgpu-test/out/sweetgpu-test
set -e
H=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$H/out"
xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 -Wall -Wextra \
	-framework IOKit -framework CoreFoundation -o "$H/out/sweetgpu-test" "$H/sweetgpu-test.c"
codesign -f -s - "$H/out/sweetgpu-test"
echo "built $H/out/sweetgpu-test"
