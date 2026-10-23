# libass for the Samsung .tpk (libass 0.17.5 + patches)

Only the .tpk uses this. webOS, the .wgt and Android keep their own libass.

| Patch | What it does |
|---|---|
| `0001-neon32-bitmap-engine.patch` | ARMv7 NEON bitmap engine (`libass/arm/neon32.c`): rasterizer, blend, `\be` and the Gaussian blur stripes. It is a port of libass's aarch64 engine to NEON intrinsics. It keeps the C engine's exact integer arithmetic, so the output is bit-identical. It is selected in `ass_bitmap_engine_init` on 32-bit ARM with NEON, independent of `--disable-asm`, and checkasm gets an armv7 entry. |
| `0002-stage-timers-engine-switch.patch` | Per-stage timers inside `ass_render_frame` (shape, outline, raster, blur, be, compose, clip, other) and outline/bitmap/composite cache hit counters. Also adds `ass_nv_set_engine` (C or NEON at runtime) and `ass_nv_flush_caches`. The public part is at the end of `ass.h` (`ASS_NV_API`). |
| `0003-threaded-blur.patch` | The NEON blur stripe pipeline is written per stripe range, and big blurs are split across helper threads (`ass_nv_set_threads`, 1–4). Each stripe depends only on the read-only source, so the output is identical. |
| `0004-parallel-renderers.patch` | For the app's parallel renderers. The stage timers become per thread (pthread keys, not compiler TLS: the Tizen 4/5 `.so` must have no `PT_TLS`), so several renderers can time frames at once. `ass_nv_track_clone` deep-copies a track for another renderer (libass keeps per-renderer layout state inside the events). `ass_nv_set_par_min` sets the smallest blur job that is split, and `ass_nv_par_stats_take` counts what each job did: split, below the minimum, pool busy, 1 thread. |

`cflags` holds extra compiler flags for libass only (now `-O3 -mtune=cortex-a53`, plan Step 2). They are part of the stamp, so changing them rebuilds libass.

## How it is built
- `tools/tpk.sh` applies the patches on the host, to a fresh copy of the tarball in `$CACHE/src/libass-nv`. The build image has no `patch`.
- It writes the hash of the patches as `.nv-stamp`.
- `tools/tpk/deps.sh` builds a copy of that tree and records the same hash in `prefix/lib/.libass-nv`.
- Changing a patch rebuilds libass only.

## How it is checked
CI runs `verify.sh` in the same linux/arm image under qemu (see `.github/workflows/build-tpk.yml`):
1. libass's checkasm compares every NEON function with the C engine on random input. Odd seeds run with 4 threads and every job split.
2. `verify.c` renders `samples/*.ass` with C, NEON with 1 thread and NEON with 4 threads on a time grid, and compares every `ASS_Image` byte for byte. It then renders the same grid with 3 renderers at the same time (frame i on renderer i % 3, each with its own `ass_nv_track_clone`, the library shared, 4 blur threads) and compares that too.

On the TV, the player's ASS performance panel has "ASS bench → C vs NEON". It does the same comparison on the real track and logs the speed of each engine. "ASS bench → this engine" renders the 10 s with 1 renderer, then with 2 and 3 renderers at once, and compares every frame with the 1-renderer pass.
