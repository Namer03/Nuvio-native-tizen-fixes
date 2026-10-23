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
 * Also checks that the stage timers (ass_nv_perf_*) count something.
 */
/* 64-bit inodes (CI overlay fs): without this, 32-bit readdir() fails with
 * EOVERFLOW and no font gets loaded. */
#define _FILE_OFFSET_BITS 64

#include <ass/ass.h>
#include <dirent.h>
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

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: verify <font dir> <file.ass>...\n"); return 2; }
    if (!ass_nv_simd_available()) { fprintf(stderr, "no SIMD engine in this libass build\n"); return 1; }
    ASS_Library *lib = ass_library_init();
    ass_set_message_cb(lib, quiet, NULL);
    load_fonts(lib, argv[1]);
    ass_nv_perf_enable(1);

    static Frame a[MAX_FRAMES], b[MAX_FRAMES], c[MAX_FRAMES];
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
        bad |= diff != 0;
    }
    printf("total C %.0f ms, NEON %.0f ms (x%.2f; emulated, not TV speed)\n", tc, tn, tn > 0 ? tc / tn : 0);
    printf(bad ? "RESULT: FAIL\n" : "RESULT: NEON engine (1 and 4 threads) bit-identical to C\n");
    ass_library_done(lib);
    return bad;
}
