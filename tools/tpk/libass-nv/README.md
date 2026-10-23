# libass for the Samsung .tpk (libass 0.17.5 + patches)

Only the .tpk uses this. webOS, the .wgt and Android keep their own libass.

| Patch | What it does |
|---|---|
| `0001-neon32-bitmap-engine.patch` | ARMv7 NEON bitmap engine (`libass/arm/neon32.c`): rasterizer, blend, `\be` and the Gaussian blur stripes. It is a port of libass's aarch64 engine to NEON intrinsics. It keeps the C engine's exact integer arithmetic, so the output is bit-identical. It is selected in `ass_bitmap_engine_init` on 32-bit ARM with NEON, independent of `--disable-asm`, and checkasm gets an armv7 entry. |
| `0002-stage-timers-engine-switch.patch` | Per-stage timers inside `ass_render_frame` (shape, outline, raster, blur, be, compose, clip, other) and outline/bitmap/composite cache hit counters. Also adds `ass_nv_set_engine` (C or NEON at runtime) and `ass_nv_flush_caches`. The public part is at the end of `ass.h` (`ASS_NV_API`). |

## How it is built
- `tools/tpk.sh` applies the patches on the host, to a fresh copy of the tarball in `$CACHE/src/libass-nv`. The build image has no `patch`.
- It writes the hash of the patches as `.nv-stamp`.
- `tools/tpk/deps.sh` builds a copy of that tree and records the same hash in `prefix/lib/.libass-nv`.
- Changing a patch rebuilds libass only.

## How it is checked
CI runs `verify.sh` in the same linux/arm image under qemu (see `.github/workflows/build-tpk.yml`):
1. libass's checkasm compares every NEON function with the C engine on random input.
2. `verify.c` renders `samples/*.ass` with both engines on a time grid and compares every `ASS_Image` byte for byte.

On the TV, the player's ASS performance panel has "ASS bench → C vs NEON". It does the same comparison on the real track and logs the speed of each engine.
