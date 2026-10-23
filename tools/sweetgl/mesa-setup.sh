#!/bin/sh
# Configure Mesa (research/mesa, + tools/sweetgl/mesa.patch) for iOS 16 arm64:
# gallium freedreno + GLES, no window system, no LLVM -> research/mesa-ios.
# Uses the system Xcode (xcode-select) and generates the meson cross file from
# xcrun, so it is not pinned to one machine's Xcode like ios-sailfish was.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
H=$R/tools/sweetgl
SDK=$(xcrun -sdk iphoneos --show-sdk-path)
CLANG=$(xcrun -sdk iphoneos -f clang); CLANGXX=$(xcrun -sdk iphoneos -f clang++)
AR=$(xcrun -sdk iphoneos -f ar); STRIP=$(xcrun -sdk iphoneos -f strip)
cat > "$H/ios-cross.txt" <<CROSS
[binaries]
c = ['$CLANG', '-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=16.0']
cpp = ['$CLANGXX', '-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=16.0']
objc = ['$CLANG', '-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=16.0']
ar = '$AR'
strip = '$STRIP'
pkg-config = 'false'
[built-in options]
c_args = ['-I$H/include']
cpp_args = ['-I$H/include']
[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
CROSS
cd "$R/research/mesa"
rm -rf ../mesa-ios
meson setup ../mesa-ios --cross-file "$H/ios-cross.txt" \
	--buildtype=release -Db_ndebug=true -Ddefault_library=static \
	-Dplatforms= -Degl=disabled -Dglx=disabled -Dgbm=disabled \
	-Dgallium-drivers=freedreno -Dvulkan-drivers= -Dfreedreno-kmds=msm \
	-Dllvm=disabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dzstd=disabled -Dzlib=disabled -Dexpat=disabled -Dxmlconfig=disabled \
	-Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
	-Dbuild-tests=false -Dshader-cache=disabled -Dperfetto=false \
	-Dgallium-va=disabled -Dgallium-rusticl=false -Dtools= \
	-Dhtml-docs=disabled -Ddisplay-info=disabled -Dteflon=false "$@"
