# sweetgpu-test

Proves the SweetGPU submit/fence path (sargo Adreno 615, XNU patches
0092+0093, boot-arg `sargogpu=1`) from user space: open the user client,
`bo_new` a target buffer and a command buffer, build one `CP_MEM_WRITE`
packet that makes the GPU write a sentinel into the target, `submit`, `wait`
on the fence, and check the sentinel landed. Then a negative case (an IB
outside the client's buffers must be refused).

## Build

```
tools/sweetgpu-test/build.sh        # -> tools/sweetgpu-test/out/sweetgpu-test
```

iOS arm64, `-miphoneos-version-min=16.0`, ad-hoc codesigned. Uses the system
Xcode's iPhoneOS SDK.

## Run on the device

There is no interactive shell on the phone, so the binary is baked into the
iOS root and run once at boot, with its stdout teed into the kernel console
log:

1. Copy `out/sweetgpu-test` into the root at `usr/local/bin/sweetgpu-test`
   (0755 root:wheel) when building it with `tools/mkiosroot-mac.sh`.
2. Add a `RunAtLoad` LaunchDaemon for it (e.g. in `tools/sweet-root-tweaks.py`'s
   `launchd()`), or run it from an existing early daemon.
3. Boot with `sweetnull=sweetgpu-test` added to the cmdline so the kernel tees
   this process's stdout to the console as `|sweetgpu-test| ...`.

Expected console line on success:

```
|sweetgpu-test| SweetGPU: chip 0x6010500, GMEM 0x80000 @ 0x100000, 180 MHz
|sweetgpu-test| submit seq 1, wait 0x0, target[0] = 0x5eed600d (want 0x5eed600d); foreign IB 0xe00002c7 -> SUBMIT/FENCE OK
```

`target[0]` still `0xdeadbeef` (unchanged) means the `CACHE_FLUSH_TS` fence or
the `CP_MEM_WRITE` IB did not take effect on sargo — the one unconfirmed piece
of 0093.
