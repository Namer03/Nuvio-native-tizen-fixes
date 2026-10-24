/*
 * libass NEON32 engine check (tools/tpk/libass-nv). CI only: runs under
 * qemu-arm in the TPK build image, never on the TV and never in tests/.
 *
 *   verify <font dir> <file.ass>...
 *
 * Renders every sample on a fixed time grid at 1920x1080 with the plain C
 * bitmap engine, then again with the NEON engine (ass_nv_set_engine, cold
 * caches both times), and compares every ASS_Image byte for byte: position,
 * size, colour and the w bytes of every row. Exit status 1 on any difference.
 * Also checks that the stage timers (ass_nv_perf_*) count something, that 3
 * renderers at once give the same frames (0004), and that every deferred-blur
 * recipe (0005) rebuilt on the CPU equals the normal path byte for byte.
 */
/* 64-bit inodes (CI overlay fs): without this, 32-bit readdir() fails with
 * EOVERFLOW and no font gets loaded. */
#define _FILE_OFFSET_BITS 64

#include <ass/ass.h>
#include <dirent.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 1920
#define H 1080
#define STEP_MS 83
#define MAX_FRAMES 4096

typedef struct { uint64_t hash; unsigned imgs; } Frame;

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const unsigned char *b = p;
    while (n--) { h ^= *b++; h *= 1099511628211ULL; }
    return h;
}

static Frame hash_frame(ASS_Image *img)
{
    Frame f = { 1469598103934665603ULL, 0 };
    for (; img; img = img->next) {
        int hdr[5] = { img->w, img->h, img->dst_x, img->dst_y, (int) img->color };
        f.hash = fnv(f.hash, hdr, sizeof(hdr));
        for (int y = 0; y < img->h; y++)
            f.hash = fnv(f.hash, img->bitmap + (size_t) y * img->stride, img->w);
        f.imgs++;
    }
    return f;
}

/* 0005: the same hash, but every deferred image is first rebuilt from its
 * recipe on the CPU (ass_nv_defer_apply_cpu, libass's own blur/fix/shift).
 * It must equal the normal path byte for byte. */
static unsigned n_deferred, n_defer_fail;

static Frame hash_frame_defer(ASS_Renderer *r, ASS_Image *img)
{
    Frame f = { 1469598103934665603ULL, 0 };
    static unsigned char *buf;
    static size_t cap;
    for (; img; img = img->next) {
        int hdr[5] = { img->w, img->h, img->dst_x, img->dst_y, (int) img->color };
        ASS_NvDefer d;
        f.hash = fnv(f.hash, hdr, sizeof(hdr));
        if (ass_nv_image_defer(img, &d)) {
            size_t need = (size_t) img->w * img->h;
            if (need > cap) { free(buf); cap = need; buf = malloc(cap); }
            if (!buf || !ass_nv_defer_apply_cpu(r, img, buf, img->w)) { n_defer_fail++; continue; }
            f.hash = fnv(f.hash, buf, need);
            n_deferred++;
        } else {
            for (int y = 0; y < img->h; y++)
                f.hash = fnv(f.hash, img->bitmap + (size_t) y * img->stride, img->w);
        }
        f.imgs++;
    }
    return f;
}

static int n_fonts, n_msgs;

static void load_fonts(ASS_Library *lib, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "no font dir %s\n", dir); exit(2); }
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 5 || (strcmp(e->d_name + n - 4, ".ttf") && strcmp(e->d_name + n - 4, ".otf")))
            continue;
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        FILE *fp = fopen(path, "rb");
        if (!fp) continue;
        fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
        char *buf = malloc(sz);
        if (fread(buf, 1, sz, fp) == (size_t) sz) {
            ass_add_font(lib, e->d_name, buf, sz);
            n_fonts++;
        }
        free(buf);
        fclose(fp);
    }
    closedir(d);
    if (!n_fonts) { fprintf(stderr, "no fonts loaded from %s\n", dir); exit(2); }
}

static void quiet(int level, const char *fmt, va_list va, void *data)
{
    (void) data;
    if (level > 2 || n_msgs >= 40) return;
    n_msgs++;
    fprintf(stderr, "  libass[%d]: ", level);
    vfprintf(stderr, fmt, va);
    fputc('\n', stderr);
}

static long long track_end(ASS_Track *t)
{
    long long end = 0;
    for (int i = 0; i < t->n_events; i++) {
        long long e = t->events[i].Start + t->events[i].Duration;
        if (e > end) end = e;
    }
    return end;
}

static int run(ASS_Library *lib, const char *file, int simd, int threads, Frame *out, int *n_out, double *ms)
{
    ass_nv_set_threads(threads);
    ASS_Renderer *r = ass_renderer_init(lib);
    ass_set_frame_size(r, W, H);
    ass_set_storage_size(r, W, H);
    ass_set_fonts(r, NULL, "Roboto", ASS_FONTPROVIDER_NONE, NULL, 0);
    int got = ass_nv_set_engine(r, simd);
    if (got != simd) {
        fprintf(stderr, "engine %d requested, %d in use\n", simd, got);
        return -1;
    }
    ASS_Track *t = ass_read_file(lib, (char *) file, NULL);
    if (!t) { fprintf(stderr, "cannot read %s\n", file); return -1; }
    long long end = track_end(t);
    printf("  track: %d events, %d styles, end %lld ms, %d fonts added\n", t->n_events, t->n_styles, end, n_fonts);
    ASS_NvPerf p;
    ass_nv_perf_take(&p);
    int n = 0;
    for (long long ms_t = 0; ms_t < end && n < MAX_FRAMES; ms_t += STEP_MS)
        out[n++] = hash_frame(ass_render_frame(r, t, ms_t, NULL));
    ass_nv_perf_take(&p);
    *n_out = n;
    *ms = p.ns_frame / 1e6;
    printf("  %-4s x%d %4u frames %6u images  %9.1f ms  shape %.1f outline %.1f raster %.1f blur %.1f be %.1f compose %.1f clip %.1f  bitmap cache %u/%u\n",
           simd ? "NEON" : "C", ass_nv_get_threads(), p.frames, p.images, p.ns_frame / 1e6,
           p.ns_stage[1] / 1e6, p.ns_stage[2] / 1e6, p.ns_stage[3] / 1e6, p.ns_stage[4] / 1e6,
           p.ns_stage[5] / 1e6, p.ns_stage[6] / 1e6, p.ns_stage[7] / 1e6,
           p.cache_hit[1], p.cache_hit[1] + p.cache_miss[1]);
    if (!p.frames || p.frames != (unsigned) n) {
        fprintf(stderr, "stage timers counted %u frames, expected %d\n", p.frames, n);
        return -1;
    }
    ass_free_track(t);
    ass_renderer_done(r);
    return 0;
}

/* 0004: N renderers render the same grid AT THE SAME TIME, frame i on
 * renderer i % N, each with its own renderer and its own ass_nv_track_clone
 * of the track, sharing the library (the app's parallel renderers). Every
 * frame must equal the single-renderer NEON pass. */
typedef struct {
    ASS_Library *lib;
    ASS_Track *src;
    int k, n, frames;
    Frame *out;
    unsigned timed;
    int fail;
} Par;

static void *par_worker(void *u)
{
    Par *p = u;
    ASS_Renderer *r = ass_renderer_init(p->lib);
    ASS_Track *t = ass_nv_track_clone(p->src);
    if (!r || !t) { p->fail = 1; return NULL; }
    ass_set_frame_size(r, W, H);
    ass_set_storage_size(r, W, H);
    ass_set_fonts(r, NULL, "Roboto", ASS_FONTPROVIDER_NONE, NULL, 0);
    if (ass_nv_set_engine(r, 1) != 1) { p->fail = 1; return NULL; }
    ASS_NvPerf pf;
    ass_nv_perf_take(&pf);
    for (int i = p->k; i < p->frames; i += p->n)
        p->out[i] = hash_frame(ass_render_frame(r, t, (long long) i * STEP_MS, NULL));
    ass_nv_perf_take(&pf);
    p->timed = pf.frames;
    ass_free_track(t);
    ass_renderer_done(r);
    return NULL;
}

static int run_parallel(ASS_Library *lib, const char *file, int nr, Frame *out, int frames)
{
    ASS_Track *t = ass_read_file(lib, (char *) file, NULL);
    if (!t) return -1;
    pthread_t th[4];
    Par p[4];
    unsigned timed = 0;
    int fail = 0;
    for (int k = 0; k < nr; k++) {
        p[k] = (Par) { lib, t, k, nr, frames, out, 0, 0 };
        if (pthread_create(&th[k], NULL, par_worker, &p[k])) return -1;
    }
    for (int k = 0; k < nr; k++) {
        pthread_join(th[k], NULL);
        fail |= p[k].fail;
        timed += p[k].timed;
    }
    ass_free_track(t);
    if (timed != (unsigned) frames) {
        fprintf(stderr, "per-thread stage timers counted %u frames, expected %d\n", timed, frames);
        return -1;
    }
    return fail ? -1 : 0;
}

/* 0005: deferred blur renderer (NEON), every frame rebuilt from the recipes. */
static int run_defer(ASS_Library *lib, const char *file, Frame *out, int frames, ASS_NvDeferStats *st, int min_area)
{
    ASS_Renderer *r = ass_renderer_init(lib);
    ass_set_frame_size(r, W, H);
    ass_set_storage_size(r, W, H);
    ass_set_fonts(r, NULL, "Roboto", ASS_FONTPROVIDER_NONE, NULL, 0);
    if (ass_nv_set_engine(r, 1) != 1 || ass_nv_set_deferred_blur(r, 1) != 1) return -1;
    ass_nv_set_deferred_min_area(r, min_area);
    ASS_Track *t = ass_read_file(lib, (char *) file, NULL);
    if (!t) return -1;
    ass_nv_defer_stats_take(r, st);
    for (int i = 0; i < frames; i++)
        out[i] = hash_frame_defer(r, ass_render_frame(r, t, (long long) i * STEP_MS, NULL));
    ass_nv_defer_stats_take(r, st);
    ass_free_track(t);
    ass_renderer_done(r);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: verify <font dir> <file.ass>...\n"); return 2; }
    if (!ass_nv_simd_available()) { fprintf(stderr, "no SIMD engine in this libass build\n"); return 1; }
    ASS_Library *lib = ass_library_init();
    ass_set_message_cb(lib, quiet, NULL);
    load_fonts(lib, argv[1]);
    ass_nv_perf_enable(1);

    static Frame a[MAX_FRAMES], b[MAX_FRAMES], c[MAX_FRAMES], d[MAX_FRAMES];
    int bad = 0;
    double tc = 0, tn = 0;
    for (int i = 2; i < argc; i++) {
        int na = 0, nb = 0, nc = 0;
        double ma, mb, mc;
        printf("%s\n", argv[i]);
        if (run(lib, argv[i], 0, 1, a, &na, &ma) || run(lib, argv[i], 1, 1, b, &nb, &mb) ||
            run(lib, argv[i], 1, 4, c, &nc, &mc)) { bad = 1; continue; }
        tc += ma; tn += mb;
        int diff = 0, imgs = 0;
        for (int k = 0; k < na; k++) {
            imgs += a[k].imgs;
            if (a[k].hash != b[k].hash || a[k].imgs != b[k].imgs ||
                a[k].hash != c[k].hash || a[k].imgs != c[k].imgs) {
                if (diff < 5)
                    printf("  DIFF at %d ms (%u vs %u images)\n", k * STEP_MS, a[k].imgs, b[k].imgs);
                diff++;
            }
        }
        if (na != nb || na != nc) diff++;
        if (!imgs) { printf("  no images rendered\n"); diff++; }
        printf("  %s: %d frames, %d differ\n", diff ? "FAIL" : "identical", na, diff);
        /* 3 renderers at once, threaded blur on (the TV runs both together). */
        ass_nv_set_threads(4);
        if (run_parallel(lib, argv[i], 3, d, na)) { printf("  parallel renderers: FAIL to run\n"); bad = 1; continue; }
        int pdiff = 0;
        for (int k = 0; k < na; k++)
            if (d[k].hash != b[k].hash || d[k].imgs != b[k].imgs) {
                if (pdiff < 5)
                    printf("  PARALLEL DIFF at %d ms (%u vs %u images)\n", k * STEP_MS, d[k].imgs, b[k].imgs);
                pdiff++;
            }
        printf("  3 renderers at once (track clones, shared library): %s, %d differ\n", pdiff ? "FAIL" : "identical", pdiff);
        diff += pdiff;
        /* 0005: deferred blur, recipes applied on the CPU = the normal path;
         * every blur deferred, then only those of 64k pixels or more. */
        for (int ma = 0; ma <= 65536; ma += 65536) {
            ASS_NvDeferStats st;
            unsigned d0 = n_deferred, f0 = n_defer_fail;
            if (run_defer(lib, argv[i], d, na, &st, ma)) { printf("  deferred blur: FAIL to run\n"); bad = 1; break; }
            int ddiff = 0;
            for (int k = 0; k < na; k++)
                if (d[k].hash != b[k].hash || d[k].imgs != b[k].imgs) {
                    if (ddiff < 5)
                        printf("  DEFERRED DIFF at %d ms (%u vs %u images)\n", k * STEP_MS, d[k].imgs, b[k].imgs);
                    ddiff++;
                }
            printf("  deferred blur (min area %d), recipes on the CPU: %s, %d differ | %u images from recipes, %u failed"
                   " | blurred composites %u: deferred %u, kept for \\be %u, vector clip %u, size %u\n",
                   ma, ddiff || n_defer_fail != f0 ? "FAIL" : "identical", ddiff, n_deferred - d0, n_defer_fail - f0,
                   st.blurred, st.deferred, st.kept_be, st.kept_clip, st.kept_small);
            diff += ddiff + (int) (n_defer_fail - f0);
        }
        bad |= diff != 0;
    }
    printf("total C %.0f ms, NEON %.0f ms (x%.2f; emulated, not TV speed)\n", tc, tn, tn > 0 ? tc / tn : 0);
    if (!n_deferred) { printf("no image was deferred: the 0005 check tested nothing\n"); bad = 1; }
    printf(bad ? "RESULT: FAIL\n" : "RESULT: NEON engine (1 and 4 threads, and 3 renderers at once) bit-identical to C;"
                 " deferred-blur recipes rebuild the same pictures\n");
    ass_library_done(lib);
    return bad;
}
