# Pixel 3a: installing the iOS root into `userdata` (plan)

Status: **plan only. Nothing has been erased or written.** Every step that changes
the phone's storage is marked **[ERASES]** or **[WRITES]** and runs only after an
explicit yes from the owner at that moment.

Today (see `docs/SARGO.md`) everything boots from RAM with `fastboot boot`: XNU,
PID 1 from a RAM disk, and a read-only eMMC driver that reads `userdata` at
~36.8 MB/s (8-bit, HS52). The phone's storage has never been written.

## Why `userdata`, and what that means

The iOS root is one case-sensitive journaled HFS+ volume of **16 GiB** (the same
layout that reaches the home screen on sweet: iPhone X iOS 16.7.x OS volume +
SystemOS/AppOS cryptexes + `tools/sweet-root-tweaks.py`). Partition sizes on this
phone (`fastboot getvar all`):

| Partition | Size | Fits 16 GiB? |
|---|---|---|
| `userdata` | 51,163 MiB | yes |
| `system_b` (inactive slot) | 3,116 MiB | no |
| `vendor_b` | 768 MiB | no |

So the root has to go into `userdata`. It occupies its first 16 GiB; the rest
stays unused (later: the persistent console log ring, as on sweet).

### What gets erased

Everything Android keeps in `userdata`, which is all of the user's data:

- installed apps and their data, app settings
- accounts (Google account sign-in on the device), Wi-Fi networks, device settings
- **internal storage**: photos, videos, downloads, music, documents (`/sdcard` lives in `userdata`)
- local SMS/MMS and call history, unless backed up elsewhere
- fingerprint enrolment and screen-lock setup (they have to be redone)

Android's encryption keys for that data are useless afterwards; the data is not
recoverable once overwritten.

### What stays untouched

Bootloaders (`xbl`, `abl`), `boot_a/b`, `system_a/b`, `vendor_a/b`, `dtbo`, `vbmeta`,
recovery-in-boot, modem/`modemst1/2`/`fsg`/`fsc`, `persist`, `metadata` (until a
return to Android), and the stock `boot.img` copy in `backup/`. Android itself is
still installed; it just has no user data partition it can use.

While the iOS root is in `userdata`, a **normal power-on boots Android**, which
cannot mount `userdata` and shows Android recovery ("Can't load Android system"
/ "Try again" / "Factory data reset"). Choosing **Try again** or powering off is
harmless. **Factory data reset there erases the iOS root** (it is the way back to
Android, see below). The iOS side is only ever started with `fastboot boot`.

## Before the erase (owner)

1. **Back up** anything wanted: Google Photos / Drive sync, or `adb pull /sdcard`
   from Android, SMS backup app, etc.
2. **Remove the Google account from the phone** (Settings > Accounts) *before* the
   erase. A `fastboot` erase counts as an untrusted reset: Factory Reset
   Protection would otherwise ask for that account's password when Android is set
   up again.
3. Note the screen-lock PIN is gone afterwards; nothing else to prepare.

## Stages

Each stage ends with a check; nothing moves on without it.

### 1. Write path in the XNU driver (no phone writes) — patch 0085

`bsd/dev/sweet_sdhc.c` gets writes, modelled on `sweet_ufs.c`:

- off unless boot-arg **`sdhcrw=1`**; without it the device stays read-only as now
- every write is checked against the partition bounds **right before it is issued**
  (start and end inside `userdata`); anything else is refused and logged — the
  same guard that kept sweet's other partitions safe
- `CMD25` (multi-block write) with Auto-CMD12, ADMA2 from memory (cache clean
  before the DMA), then the card's busy end and `CMD13` back to `tran`
- `DKIOCSYNCHRONIZE`: flush the eMMC write cache (`CMD6 FLUSH_CACHE`) when
  `EXT_CSD CACHE_CTRL` has it enabled, so the HFS+ journal's ordering holds
- a write self-test, **`sdhcwtest=1`**: writes a pattern to one sector **past the
  end of the 16 GiB image** (unused space of `userdata`), reads it back, compares,
  restores the old contents. Only run in stage 4.

Check: CI builds, syntax and bounds logic reviewed. No boot with `sdhcrw=1` yet.

### 2. Build the iOS root image on this Mac (no phone writes)

`tools/mkiosroot.sh` is Linux-only (`apfs-fuse`, `hfsprogs`). macOS does it
natively with **`tools/mkiosroot-mac.sh`** (written; its hdiutil steps tested on
small images: APFS UDIF and raw attach, case-sensitive journaled HFS+ with owners,
`fsck_hfs` clean, raw output starting with the HFSX signature). It needs MacPorts
`rsync` 3.x (`sudo port install rsync`; Apple's openrsync lacks what `-H` needs)
and runs as root:

```sh
sudo tools/mkiosroot-mac.sh OS.dmg SystemOS.dmg AppOS.dmg ios16-root-sargo.img 16g
```

What it does:

1. `hdiutil attach` the IPSW's OS, SystemOS and AppOS images (APFS, read-only;
   macOS decompresses transparently)
2. `hdiutil create -size 16g -fs "Case-sensitive Journaled HFS+" -layout NONE`
   (a bare volume, no partition map, like `mkfs.hfsplus` on the raw image), mount it
   with ownership enabled
3. copy with `sudo rsync -aH` (owners, modes, symlinks, hard links) and add the
   same extras as `mkiosroot.sh` (dyld cache links, plain system keybag, log dirs)
4. `sudo python3 tools/sweet-root-tweaks.py <mount>`
5. convert to a raw image (`hdiutil convert -format UDTO`), checksum it

Owner provides: the **iPhone X (iPhone10,3) iOS 16.7.x IPSW** from Apple (~6 GB),
the build sweet uses (20H392). Apple artifacts stay out of git (`research/`,
`backup/` are ignored). The ADT's `/product` node also comes from that IPSW
(`DeviceTree.d22ap.im4p` -> `research/ipsw-x/d22ap.adt`); today the sargo ADT
lacks it, which MobileGestalt needs.

Check: the image mounts read-only on the Mac and `fsck_hfs -n` is clean.

### 3. Put the image into `userdata` **[ERASES]**

Only after the owner confirms the backup and the account removal, and says yes:

```sh
fastboot -S 256M flash userdata ios16-root-sargo.img
```

`fastboot` (not our driver) writes it; zero regions are sent as fill chunks.
`-S 256M` matches this ABL's `max-download-size` (0x10000000). Over USB 2 expect
roughly 10–15 minutes; do not unplug. This touches `userdata` only.

### 4. Boot it, read-only first, then the write test **[WRITES only in 4b]**

- **4a** `rd=sdhcroot0` without `sdhcrw`: expect `hfs: mounted ... on device` and then
  launchd stopping at the first remount (`mount: / failed`, as sweet's 0036 boot) —
  proves the root mounts from the eMMC.
- **4b** `sdhcrw=1 sdhcwtest=1`, still not `rootrw`: the one-sector pattern test
  past the image end. **[WRITES]** (unused space only)

### 5. Writable iOS root **[WRITES]**

`rd=sdhcroot0 sdhcrw=1 rootrw=1` plus sweet's userland boot-args
(`unentitled_ios_sim_launch=1 ignition_level=1 launchd_unsecure_cache=1 wdt=-1
wdt_test=no_user_checkin`). launchd, keybagd, the daemons, backboardd and
SpringBoard start; the first boot is slow (sweet: ~4–5 min on cpus=1). Without
touch/keys drivers the screen can be looked at but not used yet.

## Going back to Android

From the bootloader (Volume Down + Power):

```sh
fastboot -w
fastboot reboot
```

`-w` erases `userdata` (the iOS root) and `metadata`; Android starts like a new
phone, sign in with the same Google account and restore the backup. Nothing
else was changed, so this is enough. If Android ever does not come up, Google's
factory image for the installed build (`flash-all.sh`) reinstalls everything;
it also erases `userdata`.

## Open points

- 16 GiB vs. more: the root could be larger (51 GiB available); 16 GiB is the
  tested sweet size.
- Persistent console log (sweet patch 0047) is UFS-only; porting it to the eMMC
  driver needs the write path and lives past the image end.
- Input: Synaptics RMI4 touch (i2c9 @0x20) and PM660/PM660L keys are not written
  yet; USB networking needs sargo GCC/GDSC offsets checked.
