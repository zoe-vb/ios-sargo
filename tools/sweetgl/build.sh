#!/bin/sh
# Build sweetgl.dylib (Mesa freedreno + GLES + our frontend) for iOS 16
# arm64: Mesa static libs from research/mesa-ios (tools/sweetgl/mesa-setup.sh,
# after `git apply tools/sweetgl/mesa.patch` in research/mesa), ad-hoc signed,
# zero-padded to 64 MiB so the file on the phone keeps one size and can be
# rewritten in place (like tools/sweetgpu-ca).
set -e
H=$(cd "$(dirname "$0")" && pwd)
R=$H/../..
M=$R/research/mesa
B=$R/research/mesa-ios
export PATH=$R/research/tools/venv/bin:$R/research/tools/bin:$PATH
# All static libs Mesa can build (layout drifts between versions): build them
# all and let ld's -dead_strip drop whatever sweetgl.dylib does not reference.
LIBS=$(cd "$B" && ninja -t targets all 2>/dev/null | grep -oE '[^ :]+\.a' | sort -u)
(cd "$B" && ninja $LIBS)
mkdir -p "$H/out"
BRIDGE=$(cd "$B" && echo $LIBS | tr ' ' '\n' | grep 'glapi_bridge\.a' | head -1)
nm -g "$B/$BRIDGE" > "$H/out/bridge.nm"
CC="xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0"
INC="-I$H/include -I$M/include -I$M/src -I$M/src/mesa -I$M/src/mapi -I$M/src/gallium/include
	-I$M/src/gallium/auxiliary -I$M/src/gallium/drivers -I$M/src/gallium/winsys
	-I$B/src -I$B/src/mesa -I$B/src/mapi"
O=$H/out
mkdir -p "$O"
# Mesa's configure-time defines (HAVE_PTHREAD, ...) as the state tracker gets them
python3 -I -c '
import json, shlex, sys
cc = json.load(open(sys.argv[1]))
e = [x for x in cc if x["file"].endswith("state_tracker/st_manager.c")][0]
print("\n".join(shlex.quote(a) for a in shlex.split(e["command"]) if a.startswith("-D")))
' "$B/compile_commands.json" > "$O/mesa-defines.rsp"
python3 -I "$H/gen-dispatch.py" "$H/ios-dispatch.txt" "$M/include" "$O/bridge.nm" "$O/dispatch.c"
for f in sweetgl sweet_drm eagl layerdump "$O/dispatch"; do
	case $f in /*) src=$f.c ;; *) src=$H/$f.c ;; esac
	$CC -O2 -Wall -fvisibility=hidden @"$O/mesa-defines.rsp" $INC -I"$H" -c "$src" -o "$O/$(basename "$f").o"
done
printf '_sweetgl_init\n_sweetgl_create\n_sweetgl_make_current\n_sweetgl_destroy\n_sweetgl_selftest\n' > "$O/exports.txt"
# shared-glapi is shared-only in recent Mesa (no static .a); pull its objects in
GLAPIOBJ=$(find "$B" -path '*shared-glapi*' -name '*.o' 2>/dev/null)
xcrun -sdk iphoneos clang++ -arch arm64 -miphoneos-version-min=16.0 -dynamiclib -dead_strip \
	-install_name /usr/lib/sweetgl.dylib -exported_symbols_list "$O/exports.txt" \
	-o "$O/sweetgl.dylib" "$O/sweetgl.o" "$O/sweet_drm.o" "$O/eagl.o" "$O/layerdump.o" "$O/dispatch.o" $GLAPIOBJ $(for l in $LIBS; do echo "$B/$l"; done) \
	-framework IOKit -framework CoreFoundation -framework IOSurface -framework OpenGLES -framework CoreGraphics -lobjc -lc++ -lz
codesign -f -s - "$O/sweetgl.dylib"
SIZE=$(stat -f %z "$O/sweetgl.dylib")
[ "$SIZE" -le 67108864 ] || { echo "sweetgl.dylib is $SIZE bytes, over 64 MiB"; exit 1; }
cp "$O/sweetgl.dylib" "$O/sweetgl-unpadded.dylib"
dd if=/dev/zero of="$O/sweetgl.dylib" bs=1 count=0 seek=67108864 2>/dev/null
echo "built $O/sweetgl.dylib ($SIZE bytes, padded to 64 MiB)"
