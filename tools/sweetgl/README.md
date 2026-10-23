# sweetgl — GPU-accelerated GLES for iOS on sargo (Adreno 615)

The userland half of the GPU stack: **Mesa's `freedreno` GLES driver** talking
to our `SweetGPU` IOKit user client (patches 0092–0101) through a libdrm/msm
shim, plus an EAGL shim so CoreAnimation renders through it. Packaged as
`sweetgl.dylib` and injected into `backboardd` with `DYLD_INSERT_LIBRARIES` +
`CA_*` env.

**Transplanted, with the author's permission, from
[joranv1/ios-sailfish](https://github.com/joranv1/ios-sailfish)** (the Pixel 1
/ Adreno 530 port). Adapted to sargo's Adreno 615 (a6xx): `sweet_drm.c`
reports `MSM_PARAM_GPU_ID = 615` so Mesa's freedreno selects the a6xx path
(it supports a5xx and a6xx from one build, chosen at runtime from `chip_id`).

## Pieces

- `sweet_drm.c/.h` — libdrm/msm shim: turns Mesa's DRM ioctls
  (`DRM_MSM_GEM_NEW/INFO/SUBMIT`, `GET_PARAM`, `WAIT_FENCE`, IOSurface import)
  into `SweetGPU` user-client calls (selectors 10–16).
- `eagl.c/.h` — EAGL/CoreAnimation GLES shim.
- `sweetgl.c/.h`, `gen-dispatch.py`, `ios-dispatch.txt` — GLES dispatch glue.
- `mesa.patch` — small Mesa changes for iOS (mmap via the shim, build the
  freedreno/drm backend on darwin).
- `mesa-setup.sh` — meson configure (generates the cross file from the system
  Xcode via xcrun). `build.sh` — link `sweetgl.dylib`.

## Build (not yet run on this machine)

Prerequisites still to set up here: a Mesa checkout at `research/mesa` (tag **mesa-24.3.4**) matching
`mesa.patch`, `meson` (e.g. in a venv), `ninja` (have: MacPorts). Then:

```
git -C research/mesa apply tools/sweetgl/mesa.patch
tools/sweetgl/mesa-setup.sh      # meson configure -> research/mesa-ios
tools/sweetgl/build.sh           # ninja Mesa + link sweetgl.dylib
```

Then deploy into the iOS rootfs (sweetgl.dylib + the DYLD_INSERT/CA_* env for
backboardd) and boot the full root (`rd=sdhcroot0`). In the full userland the
normal dyld/libSystem path is used, so the Mach/IOKit hardening that blocks a
bare static binary does not apply.
