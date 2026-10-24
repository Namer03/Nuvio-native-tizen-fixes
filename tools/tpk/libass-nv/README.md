# libass for the Samsung .tpk (libass 0.17.5 + patches)

Only the .tpk uses this. webOS, the .wgt and Android keep their own libass.

| Patch | What it does |
|---|---|
| `0001-neon32-bitmap-engine.patch` | ARMv7 NEON bitmap engine (`libass/arm/neon32.c`): rasterizer, blend, `\be` and the Gaussian blur stripes. It is a port of libass's aarch64 engine to NEON intrinsics. It keeps the C engine's exact integer arithmetic, so the output is bit-identical. It is selected in `ass_bitmap_engine_init` on 32-bit ARM with NEON, independent of `--disable-asm`, and checkasm gets an armv7 entry. |
| `0002-stage-timers-engine-switch.patch` | Per-stage timers inside `ass_render_frame` (shape, outline, raster, blur, be, compose, clip, other) and outline/bitmap/composite cache hit counters. Also adds `ass_nv_set_engine` (C or NEON at runtime) and `ass_nv_flush_caches`. The public part is at the end of `ass.h` (`ASS_NV_API`). |
| `0003-threaded-blur.patch` | The NEON blur stripe pipeline is written per stripe range, and big blurs are split across helper threads (`ass_nv_set_threads`, 1–4). Each stripe depends only on the read-only source, so the output is identical. |
| `0004-parallel-renderers.patch` | For the app's parallel renderers. The stage timers become per thread (pthread keys, not compiler TLS: the Tizen 4/5 `.so` must have no `PT_TLS`), so several renderers can time frames at once. `ass_nv_track_clone` deep-copies a track for another renderer (libass keeps per-renderer layout state inside the events). `ass_nv_set_par_min` sets the smallest blur job that is split, and `ass_nv_par_stats_take` counts what each job did: split, below the minimum, pool busy, 1 thread. |
| `0005-deferred-blur.patch` | Deferred blur, for the app's GPU blur (`src/assgpublur.c`). With `ass_nv_set_deferred_blur(renderer, 1)`, a composite with a Gaussian blur, no `\be` and no vector clip skips the blur, the outline fix and the shadow's subpixel shift. Its bitmaps stay sharp, padded to the size the blur would give them, and `ass_nv_image_defer` returns the recipe of each image: source bitmap, σ², outline fix against the fill, shadow shift. The `FILTER_NV_DEFER` flag keeps deferred and normal composites apart in the cache. `ass_nv_defer_apply` runs a recipe on the CPU with libass's own functions; the app uses it as its fallback and for the snapshot comparison. `ass_nv_defer_stats_take` counts blurred, deferred and kept composites per renderer, and times libass's own CPU blurs by bitmap size. `ass_nv_set_deferred_min_area` keeps blurs below a size on the CPU (cheaper there than a GPU job). |

`cflags` holds extra compiler flags for libass only (now `-O3 -mtune=cortex-a53`, plan Step 2). They are part of the stamp, so changing them rebuilds libass.

## How it is built
- `tools/tpk.sh` applies the patches on the host, to a fresh copy of the tarball in `$CACHE/src/libass-nv`. The build image has no `patch`.
- It writes the hash of the patches as `.nv-stamp`.
- `tools/tpk/deps.sh` builds a copy of that tree and records the same hash in `prefix/lib/.libass-nv`.
- Changing a patch rebuilds libass only.

## How it is checked
CI runs `verify.sh` in the same linux/arm image under qemu (see `.github/workflows/build-tpk.yml`):
1. libass's checkasm compares every NEON function with the C engine on random input. Odd seeds run with 4 threads and every job split.
2. `verify.c` renders `samples/*.ass` with C, NEON with 1 thread and NEON with 4 threads on a time grid, and compares every `ASS_Image` byte for byte. It then renders the same grid with 3 renderers at the same time (frame i on renderer i % 3, each with its own `ass_nv_track_clone`, the library shared, 4 blur threads) and compares that too. Last, it renders the grid with deferred blur on, rebuilds every deferred image from its recipe with `ass_nv_defer_apply_cpu`, and requires the normal path's bytes exactly, once with every blur deferred and once with the 64k-pixel minimum (`samples/blur.ass` covers every fill/border/shadow flag case, BorderStyle 3, `\kf`, rect and inverse clips, screen edges and blur from 0.4 to 40). This proves the recipe is complete; the GPU only replaces the blur itself.

On the TV, the player's ASS performance panel has "ASS bench → C vs NEON". It does the same comparison on the real track and logs the speed of each engine. "ASS bench → this engine" renders the 10 s with the CPU blur, then again with the blur deferred (the libass part of "Blur on GPU"). "ASS snapshot" compares every GPU-blurred image on screen with `ass_nv_defer_apply` and logs the difference (largest, mean, share of pixels off).

The app no longer runs parallel renderers (slower on the TV: memory-bound), but 0004 stays: its per-thread timers and split counters are still used.
