#!/bin/bash
# macOS twin of tools/mkiosroot.sh: build the iOS root as one case-sensitive
# journaled HFS+ volume image, for the Pixel 3a's userdata partition
# (rd=sdhcroot0, docs/SARGO-INSTALL.md stage 2). Same layout and extras as the
# Linux script, then tools/sweet-root-tweaks.py on the mounted volume.
#
#   sudo tools/mkiosroot-mac.sh OS.dmg SYSTEMOS.dmg APPOS.dmg OUT.img [SIZE] [--no-tweaks]
#   (SIZE default 16g, hdiutil syntax)
#
# Needs: macOS hdiutil (mounts the IPSW's APFS images natively; decompresses
# transparently), rsync 3.x (MacPorts `rsync`: Apple's openrsync is not
# enough for -H), root (owners, modes and uids must be copied exactly).
# OUT.img is the raw volume (no partition map), like the Linux mkfs.hfsplus
# image; flash it with `fastboot -S 256M flash userdata OUT.img` (ERASES
# userdata: only with the owner's explicit yes, see docs/SARGO-INSTALL.md).
set -euo pipefail

usage() { sed -n '2,15p' "$0"; exit 2; }
tweaks=1
args=()
for a in "$@"; do
	case $a in
	--no-tweaks) tweaks=0 ;;
	-h|--help) usage ;;
	*) args+=("$a") ;;
	esac
done
[[ ${#args[@]} -ge 4 ]] || usage
os=${args[0]} sysos=${args[1]} appos=${args[2]} out=${args[3]} size=${args[4]:-16g}
for f in "$os" "$sysos" "$appos"; do
	[[ -f $f ]] || { echo "no such image: $f" >&2; exit 1; }
done
[[ ! -e $out ]] || { echo "refusing to overwrite $out" >&2; exit 1; }
[[ $(uname -s) == Darwin ]] || { echo "macOS only (on Linux use tools/mkiosroot.sh)" >&2; exit 1; }
[[ $EUID == 0 ]] || { echo "run as root (sudo): owners and modes must be copied exactly" >&2; exit 1; }

RSYNC=
for r in /opt/local/bin/rsync /usr/local/bin/rsync /opt/homebrew/bin/rsync rsync; do
	if command -v "$r" >/dev/null && "$r" --version 2>/dev/null | head -1 | grep -q 'version 3'; then
		RSYNC=$(command -v "$r")
		break
	fi
done
[[ -n $RSYNC ]] || { echo "need rsync 3.x for hard links (MacPorts: sudo port install rsync)" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)

# /tmp is /private/tmp: mount(8) reports the resolved path
work=$(cd "$(mktemp -d /tmp/mkiosroot.XXXXXX)" && pwd -P)
devs=()
cleanup() {
	for d in "${devs[@]}"; do
		hdiutil detach "$d" -force >/dev/null 2>&1 || true
	done
	rm -f "$work/root.sparseimage"
	rmdir "$work"/* "$work" 2>/dev/null || true
}
trap cleanup EXIT

# Attach read-only with owners honoured; print the /dev/diskN of the image.
# The OS image is UDIF; the cryptexes are raw APFS images.
attach() {
	local img=$1 mnt=$2 class=()
	mkdir -p "$mnt"
	if ! hdiutil imageinfo "$img" >/dev/null 2>&1; then
		class=(-imagekey diskimage-class=CRawDiskImage)
	fi
	local dev
	dev=$(hdiutil attach "${class[@]}" -readonly -nobrowse -noverify -owners on \
	    -mountpoint "$mnt" "$img" | awk 'NR==1 {print $1}')
	devs+=("$dev")
	mount | grep -q " on $mnt " || { echo "could not mount $img at $mnt" >&2; exit 1; }
}
attach "$os" "$work/os"
attach "$sysos" "$work/sysos"
attach "$appos" "$work/appos"

hdiutil create -size "$size" -type SPARSE -layout NONE -fs "Case-sensitive Journaled HFS+" \
    -volname iOSRoot "$work/root.sparseimage" >/dev/null
mkdir "$work/root"
rdev=$(hdiutil attach -nobrowse -owners on -mountpoint "$work/root" "$work/root.sparseimage" |
    awk 'NR==1 {print $1}')
devs=("$rdev" "${devs[@]}")
R=$work/root
# Keep macOS bookkeeping (Spotlight, fseventsd) off the iOS volume.
touch "$R/.metadata_never_index"
mkdir -p "$R/.fseventsd" && touch "$R/.fseventsd/no_log"
mdutil -i off "$R" >/dev/null 2>&1 || true

copy() {
	echo "copying $1 -> /${2#"$R"/}"
	mkdir -p "$2"
	"$RSYNC" -aH --numeric-ids "$1/" "$2/"
}
copy "$work/os" "$R"
copy "$work/sysos" "$R/private/preboot/Cryptexes/OS"
copy "$work/appos" "$R/private/preboot/Cryptexes/App"

# Shared cache where dyld looks at ignition level 1: hard links, no extra space.
dsc=private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld
mkdir -p "$R/System/Library/Caches/com.apple.dyld"
for f in "$R/$dsc"/*; do
	ln "$f" "$R/System/Library/Caches/com.apple.dyld/${f##*/}"
done

# dyld does not search the cryptexes at ignition level 1: hard-link every
# cryptex file that has no counterpart in the OS root to its root path
# (symlinks are copied, as on Linux, so both scripts give the same tree).
for c in OS App; do
	(cd "$R/private/preboot/Cryptexes/$c" && find . ! -type d ! -path './System/Library/Caches/com.apple.dyld/*') |
	while IFS= read -r f; do
		f=${f#./}
		if [[ ! -e $R/$f && ! -L $R/$f ]]; then
			mkdir -p "$R/$(dirname "$f")"
			src=$R/private/preboot/Cryptexes/$c/$f
			if [[ -L $src ]]; then cp -P "$src" "$R/$f"; else ln "$src" "$R/$f"; fi
			echo "linked $c:/$f"
		fi
	done
done

# logd's log stores, normally made at restore (owner _logd 272, .migrated markers).
db=$R/private/var/db
install -d -o 272 -g 272 -m 0751 "$db/diagnostics"
install -d -o 272 -g 272 -m 0755 "$db/uuidtext" "$db/diagnostics/timesync"
for d in diagnostics uuidtext diagnostics/timesync; do
	: > "$db/$d/.migrated"
	chown 272:272 "$db/$d/.migrated"
	chmod 0644 "$db/$d/.migrated"
done

# System keybag for the built-in AppleKeyStore (no passcode).
install -d -m 0700 -o 0 -g 0 "$R/private/var/keybags"
kb=$work/systembag.kb
printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	'<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>' > "$kb"
install -m 0600 -o 0 -g 0 "$kb" "$R/private/var/keybags/systembag.kb"
rm -f "$kb"

if [[ $tweaks == 1 ]]; then
	python3 "$here/sweet-root-tweaks.py" "$R"
fi

rm -rf "$R/.fseventsd" "$R/.Spotlight-V100" "$R/.Trashes" "$R/.TemporaryItems" \
    "$R/.DocumentRevisions-V100" "$R/.metadata_never_index"
df -h "$R"
sync
hdiutil detach "$rdev" >/dev/null
devs=("${devs[@]:1}")

# Verify the volume, then write the raw image.
vdev=$(hdiutil attach -nomount -readonly "$work/root.sparseimage" | awk 'NR==1 {print $1}')
devs=("$vdev" "${devs[@]}")
fsck_hfs -fn "$vdev"
hdiutil detach "$vdev" >/dev/null
devs=("${devs[@]:1}")
hdiutil convert "$work/root.sparseimage" -format UDTO -o "$work/root.cdr" >/dev/null
mv "$work/root.cdr" "$out"
shasum -a 256 "$out" | tee "$out.sha256"
ls -l "$out"
