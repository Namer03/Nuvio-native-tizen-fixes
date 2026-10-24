// SCAN DAS APIS DE EXIBICAO (.tpk, dono 23/10/2026).
//
// O branco das legendas sai mais escuro que o branco do video na S95C: o
// libass e o framebuffer do app estao certos (snapshot de 23/10: mascara
// F1F5F6..FEFEFE, alfa 255), o preto do cartao de teste e preto puro, e o
// branco do cartao fica ~5 % abaixo do branco do video ao lado. Ou seja, a TV
// limita o brilho da camada grafica (a nossa janela) em relacao ao plano de
// video. A API publica da Samsung (Tizen.TV.*) nao tem nada de brilho ou de
// plano; este scan procura, SO LENDO, o que o aparelho oferece:
//
//  1. EGL/GL (fio GL): extensoes, espaco de cor da superficie, as configs que
//     existem (10 bits? ponto flutuante? = caminho para branco acima do SDR);
//  2. o protocolo do gerenciador de janelas (Wayland): cada global que ele
//     anuncia e, das interfaces que achar nas bibliotecas, cada requisicao com
//     a assinatura exata (para a proxima build poder chamar a certa);
//  3. as bibliotecas do sistema: funcoes exportadas com nomes de brilho,
//     luminancia, gama, plano, OSD, HDR, PQ...;
//  4. as chaves de configuracao (vconf) e o sysfs com nomes de imagem;
//  5. a API .NET (pelo host, Exibe.cs): membros com esses nomes e as "aux
//     hints" que o gerenciador de janelas aceita nas nossas janelas.
//
// Nada e chamado alem de leituras: nenhuma funcao achada e executada. O texto
// completo vai para /ass-snap-display-scan.txt na pagina do log; o log
// (aba Subtitles) leva o resumo.
#ifdef NV_TPK
#include "tpkscan.h"
#include "assperf.h"
#include "tpk_egl.h"
#include <GLES2/gl2.h>
#include <ctype.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
/* So o glibc com _GNU_SOURCE declara RTLD_DEFAULT (a imagem do .tpk nao
 * define: CI de 23/10). No glibc ele vale ((void *) 0). */
#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT ((void *)0)
#endif

// ---- gancho do host .NET (tizen-tpk/Exibe.cs) ---------------------------------
/* O teste de Window.SetBrightness saiu (24/10: devolveu False, nada mudou na
 * tela); o host so registra o scan .NET. */
typedef void (*TsFnSem)(void);
static TsFnSem hScanNet;

__attribute__((visibility("default")))
void nv_tpk_exibe_registrar(TsFnSem scanNet) {
  hScanNet = scanNet;
}

// ---- texto do scan ------------------------------------------------------------
typedef struct { char *b; size_t n, cap; int noLog; } ScanTxt;
#define SCAN_MAX_BYTES (4u * 1024u * 1024u)
#define SCAN_MAX_LOG 400

static void sl(ScanTxt *t, int log, const char *fmt, ...) {
  char l[2048];
  va_list va;
  int k;
  va_start(va, fmt);
  k = vsnprintf(l, sizeof l, fmt, va);
  va_end(va);
  if (k < 0) return;
  if ((size_t)k >= sizeof l) k = (int)sizeof l - 1;
  if (log && t->noLog < SCAN_MAX_LOG) { printf("[assperf] scan %s\n", l); t->noLog++; }
  if (t->n + (size_t)k + 2 > SCAN_MAX_BYTES) return;
  if (t->n + (size_t)k + 2 > t->cap) {
    size_t nc = (t->cap ? t->cap * 2 : 65536) + (size_t)k;
    char *nb = realloc(t->b, nc);
    if (!nb) return;
    t->b = nb; t->cap = nc;
  }
  memcpy(t->b + t->n, l, (size_t)k); t->n += (size_t)k; t->b[t->n++] = '\n';
}

// Lista separada por espacos (extensoes): uma linha por ~10 itens no arquivo;
// no log so as que casam com `filtro` (NULL = todas).
static int casa(const char *nome, const char *const *chaves) {
  char low[256];
  size_t i;
  for (i = 0; nome[i] && i + 1 < sizeof low; i++) low[i] = (char)tolower((unsigned char)nome[i]);
  low[i] = 0;
  for (i = 0; chaves[i]; i++) if (strstr(low, chaves[i])) return 1;
  return 0;
}

static void sl_lista(ScanTxt *t, const char *titulo, const char *lista, const char *const *filtro) {
  char item[256], linha[1500];
  const char *p = lista;
  size_t o = 0;
  int n = 0, nl = 0;
  if (!lista) { sl(t, 1, "%s: (none)", titulo); return; }
  linha[0] = 0;
  while (*p) {
    size_t k = 0;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && k + 1 < sizeof item) item[k++] = *p++;
    item[k] = 0;
    if (!k) continue;
    n++;
    if (filtro && casa(item, filtro)) { sl(t, 1, "%s: %s", titulo, item); nl++; }
    o += (size_t)snprintf(linha + o, sizeof linha - o, "%s%s", o ? " " : "", item);
    if (o > 1100) { sl(t, 0, "%s: %s", titulo, linha); o = 0; linha[0] = 0; }
  }
  if (o) sl(t, 0, "%s: %s", titulo, linha);
  sl(t, 1, "%s: %d in total%s", titulo, n, filtro ? (nl ? " (colour/HDR-related ones above)" : " (none colour/HDR-related)") : "");
}

static const char *const CHAVES_COR[] = {
  "srgb", "colorspace", "color_space", "gl_colorspace", "hdr", "pq", "hlg", "bt2020", "scrgb", "display_p3",
  "float", "half", "10bit", "1010102", "rgb10", "pixel_format", "yuv", "smpte", "cta861", "luminance", "tonemap", NULL
};

// ---- 1. EGL / GL (fio GL) -------------------------------------------------------
typedef const char *(*FnQueryString)(EGLDisplay, EGLint);
typedef EGLBoolean (*FnGetConfigs)(EGLDisplay, EGLConfig *, EGLint, EGLint *);

static void scan_egl(ScanTxt *t) {
  void *h = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_NOLOAD);
  FnQueryString qs = NULL;
  FnGetConfigs gc = NULL;
  EGLDisplay d;
  EGLSurface s;
  EGLContext c;
  if (!h) h = dlopen("libEGL.so", RTLD_LAZY | RTLD_NOLOAD);
  if (!h) h = dlopen("libEGL.so.1", RTLD_LAZY);
  if (h) { qs = (FnQueryString)dlsym(h, "eglQueryString"); gc = (FnGetConfigs)dlsym(h, "eglGetConfigs"); }
  if (!tpkEgl.GetCurrentDisplay) { sl(t, 1, "egl: not loaded"); return; }
  d = eglGetCurrentDisplay(); s = eglGetCurrentSurface(EGL_DRAW); c = eglGetCurrentContext();
  sl(t, 1, "egl: display %p surface %p context %p | eglQueryString %s", d, s, c, qs ? "found" : "missing");
  if (qs && d != EGL_NO_DISPLAY) {
    sl(t, 1, "egl vendor: %s | version: %s | client APIs: %s", qs(d, EGL_VENDOR) ? qs(d, EGL_VENDOR) : "?",
       qs(d, EGL_VERSION) ? qs(d, EGL_VERSION) : "?", qs(d, EGL_CLIENT_APIS) ? qs(d, EGL_CLIENT_APIS) : "?");
    sl_lista(t, "egl display extension", qs(d, EGL_EXTENSIONS), CHAVES_COR);
    sl_lista(t, "egl client extension", qs(EGL_NO_DISPLAY, EGL_EXTENSIONS), CHAVES_COR);
  }
  if (d != EGL_NO_DISPLAY && s != EGL_NO_SURFACE) {
    static const struct { const char *nome; EGLint id; } SUP[] = {
      { "EGL_GL_COLORSPACE (0x308A linear, 0x3089 sRGB, other = an EXT colour space)", 0x309D },
      { "EGL_RENDER_BUFFER", 0x3086 }, { "EGL_SWAP_BEHAVIOR", 0x3093 }, { "EGL_VG_ALPHA_FORMAT (0x308C pre)", 0x3088 },
      { "EGL_VG_COLORSPACE", 0x3087 }, { "EGL_WIDTH", 0x3057 }, { "EGL_HEIGHT", 0x3056 }, { "EGL_CONFIG_ID", 0x3028 },
      { "EGL_SMPTE2086_MAX_LUMINANCE_EXT", 0x3349 }, { "EGL_CTA861_3_MAX_CONTENT_LIGHT_LEVEL_EXT", 0x3360 },
    };
    size_t i;
    for (i = 0; i < sizeof SUP / sizeof SUP[0]; i++) {
      EGLint v = 0;
      if (eglQuerySurface(d, s, SUP[i].id, &v)) sl(t, 1, "egl surface %s = %d (0x%x)", SUP[i].nome, v, v);
      else sl(t, 1, "egl surface %s: n/a (error 0x%x)", SUP[i].nome, eglGetError());
    }
  }
  if (d != EGL_NO_DISPLAY && c != EGL_NO_CONTEXT) {
    EGLint id = 0, n = 0, pede[] = { EGL_CONFIG_ID, 0, EGL_NONE };
    EGLConfig cfg;
    eglQueryContext(d, c, EGL_CONFIG_ID, &id);
    pede[1] = id;
    if (eglChooseConfig(d, pede, &cfg, 1, &n) && n == 1) {
      static const struct { const char *nome; EGLint id; } CFG[] = {
        { "R", EGL_RED_SIZE }, { "G", EGL_GREEN_SIZE }, { "B", EGL_BLUE_SIZE }, { "A", EGL_ALPHA_SIZE },
        { "component type (0x333A fixed, 0x333B float)", 0x3339 }, { "native visual", 0x302E },
        { "surface type", 0x3033 }, { "renderable", 0x3040 }, { "transparent type", 0x3034 }, { "colour buffer type", 0x303F },
      };
      size_t i;
      for (i = 0; i < sizeof CFG / sizeof CFG[0]; i++) {
        EGLint v = 0;
        if (eglGetConfigAttrib(d, cfg, CFG[i].id, &v)) sl(t, 1, "egl our config %d: %s = %d (0x%x)", id, CFG[i].nome, v, v);
        else sl(t, 1, "egl our config %d: %s n/a", id, CFG[i].nome);
      }
    }
    if (gc) {
      EGLint total = 0;
      if (gc(d, NULL, 0, &total) && total > 0 && total < 4096) {
        EGLConfig *all = calloc((size_t)total, sizeof *all);
        if (all && gc(d, all, total, &total)) {
          /* Combinacoes distintas de bits/tipo: 10 bits ou float = caminho para
           * uma superficie HDR/estendida (branco acima do SDR). */
          struct { EGLint r, g, b, a, tipo, surf; int n; } comb[64];
          int nc = 0, i, j;
          for (i = 0; i < total; i++) {
            EGLint r = 0, g = 0, b = 0, a = 0, tipo = 0x333A, surf = 0;
            eglGetConfigAttrib(d, all[i], EGL_RED_SIZE, &r); eglGetConfigAttrib(d, all[i], EGL_GREEN_SIZE, &g);
            eglGetConfigAttrib(d, all[i], EGL_BLUE_SIZE, &b); eglGetConfigAttrib(d, all[i], EGL_ALPHA_SIZE, &a);
            if (!eglGetConfigAttrib(d, all[i], 0x3339, &tipo)) tipo = 0x333A;
            eglGetConfigAttrib(d, all[i], 0x3033, &surf);
            for (j = 0; j < nc; j++)
              if (comb[j].r == r && comb[j].g == g && comb[j].b == b && comb[j].a == a && comb[j].tipo == tipo &&
                  comb[j].surf == (surf & 4)) { comb[j].n++; break; }
            if (j == nc && nc < 64) { comb[nc].r = r; comb[nc].g = g; comb[nc].b = b; comb[nc].a = a; comb[nc].tipo = tipo; comb[nc].surf = surf & 4; comb[nc].n = 1; nc++; }
          }
          sl(t, 1, "egl configs: %d in total, %d distinct colour formats", total, nc);
          for (j = 0; j < nc; j++)
            sl(t, 1, "egl config format: R%d G%d B%d A%d %s%s x%d", comb[j].r, comb[j].g, comb[j].b, comb[j].a,
               comb[j].tipo == 0x333B ? "FLOAT" : "fixed", comb[j].surf ? " window" : " (no window)", comb[j].n);
        }
        free(all);
      }
    }
  }
  { const char *v = (const char *)glGetString(GL_VENDOR), *r = (const char *)glGetString(GL_RENDERER);
    const char *ve = (const char *)glGetString(GL_VERSION);
    sl(t, 1, "gl: %s | %s | %s", v ? v : "?", r ? r : "?", ve ? ve : "?");
    sl_lista(t, "gl extension", (const char *)glGetString(GL_EXTENSIONS), CHAVES_COR); }
}

// ---- 2. Wayland: globais e interfaces ------------------------------------------------
typedef struct WlMsg { const char *name, *signature; const struct WlIface **types; } WlMsg;
typedef struct WlIface { const char *name; int version, method_count; const WlMsg *methods; int event_count; const WlMsg *events; } WlIface;
typedef struct { void (*global)(void *, void *, uint32_t, const char *, uint32_t); void (*global_remove)(void *, void *, uint32_t); } WlRegLis;
typedef void *(*FnConnect)(const char *);
typedef void (*FnDisconnect)(void *);
typedef int (*FnRoundtrip)(void *);
typedef void *(*FnMarshalCtor)(void *, uint32_t, const void *, ...);
typedef int (*FnAddListener)(void *, void (**)(void), void *);
typedef void (*FnProxyDestroy)(void *);

#define WL_MAX 160
static struct { char nome[96]; unsigned versao; } wlGlob[WL_MAX];
static int wlN;

static void wl_global(void *data, void *reg, uint32_t name, const char *iface, uint32_t ver) {
  (void)data; (void)reg; (void)name;
  if (wlN < WL_MAX) { snprintf(wlGlob[wlN].nome, sizeof wlGlob[wlN].nome, "%s", iface ? iface : "?"); wlGlob[wlN].versao = ver; wlN++; }
}
static void wl_global_remove(void *data, void *reg, uint32_t name) { (void)data; (void)reg; (void)name; }

static void *libsProto[64];
static int nLibsProto;

/* As bibliotecas de protocolo (…-client.so) de /usr/lib e /lib: so tabelas. */
static void scan_abrir_protocolos(ScanTxt *t);

static const WlIface *wl_achar(const char *nome) {
  char sym[160];
  int i;
  const WlIface *w;
  snprintf(sym, sizeof sym, "%s_interface", nome);
  w = (const WlIface *)dlsym(RTLD_DEFAULT, sym);
  for (i = 0; !w && i < nLibsProto; i++) w = (const WlIface *)dlsym(libsProto[i], sym);
  return w;
}

/* Argumentos de uma assinatura ("?" e os digitos de versao nao contam): o
 * vetor `types` tem um por argumento. */
static int wl_nargs(const char *sig) {
  int n = 0;
  for (; sig && *sig; sig++) if (*sig != '?' && (*sig < '0' || *sig > '9')) n++;
  return n;
}

static const WlIface *wlVistos[256];
static int nWlVistos;

static void wl_despejar(ScanTxt *t, const WlIface *w, int nivel) {
  int i, j;
  if (!w || !w->name || nivel > 3) return;
  for (i = 0; i < nWlVistos; i++) if (wlVistos[i] == w) return;
  if (nWlVistos < 256) wlVistos[nWlVistos++] = w;
  sl(t, 1, "wayland interface %s v%d: %d request(s), %d event(s)", w->name, w->version, w->method_count, w->event_count);
  for (i = 0; i < w->method_count && i < 128; i++) {
    char tipos[600]; size_t o = 0;
    const WlMsg *m = &w->methods[i];
    int na = wl_nargs(m->signature);
    tipos[0] = 0;
    for (j = 0; j < na && j < 40; j++)
      if (m->types && m->types[j] && m->types[j]->name)
        o += (size_t)snprintf(tipos + o, sizeof tipos - o, "%s%s", o ? "," : "", m->types[j]->name);
    sl(t, 1, "wayland   request %s.%s(%s)%s%s", w->name, m->name ? m->name : "?", m->signature ? m->signature : "",
       o ? " types: " : "", tipos);
  }
  for (i = 0; i < w->event_count && i < 128; i++)
    sl(t, 0, "wayland   event %s.%s(%s)", w->name, w->events[i].name ? w->events[i].name : "?",
       w->events[i].signature ? w->events[i].signature : "");
  /* Interfaces filhas (objetos criados pelas requisicoes): tizen_video_object... */
  for (i = 0; i < w->method_count && i < 128; i++) {
    const WlMsg *m = &w->methods[i];
    int na = wl_nargs(m->signature);
    for (j = 0; j < na && j < 40; j++)
      if (m->types && m->types[j]) wl_despejar(t, m->types[j], nivel + 1);
  }
}

static void scan_wayland(ScanTxt *t) {
  void *h = dlopen("libwayland-client.so.0", RTLD_LAZY | RTLD_NOLOAD);
  FnConnect connect; FnDisconnect disconnect; FnRoundtrip roundtrip; FnMarshalCtor ctor; FnAddListener addl; FnProxyDestroy pdestroy;
  const void *regIface;
  void *dpy, *reg;
  static WlRegLis lis = { wl_global, wl_global_remove };
  int i;
  if (!h) h = dlopen("libwayland-client.so.0", RTLD_LAZY);
  if (!h) { sl(t, 1, "wayland: libwayland-client.so.0 not available (%s)", dlerror()); return; }
  connect = (FnConnect)dlsym(h, "wl_display_connect"); disconnect = (FnDisconnect)dlsym(h, "wl_display_disconnect");
  roundtrip = (FnRoundtrip)dlsym(h, "wl_display_roundtrip"); ctor = (FnMarshalCtor)dlsym(h, "wl_proxy_marshal_constructor");
  addl = (FnAddListener)dlsym(h, "wl_proxy_add_listener"); pdestroy = (FnProxyDestroy)dlsym(h, "wl_proxy_destroy");
  regIface = dlsym(h, "wl_registry_interface");
  if (!connect || !disconnect || !roundtrip || !ctor || !addl || !pdestroy || !regIface) { sl(t, 1, "wayland: client symbols missing"); return; }
  scan_abrir_protocolos(t);
  /* Uma conexao NOSSA, so para listar: nao mexe na do DALi/ecore. */
  dpy = connect(NULL);
  if (!dpy) { sl(t, 1, "wayland: wl_display_connect(NULL) failed (WAYLAND_DISPLAY=%s XDG_RUNTIME_DIR=%s)",
                 getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "-", getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "-"); return; }
  wlN = 0;
  reg = ctor(dpy, 1 /* wl_display.get_registry */, regIface, NULL);
  if (reg) { addl(reg, (void (**)(void))&lis, NULL); roundtrip(dpy); }
  sl(t, 1, "wayland: %d global(s) announced by the window system", wlN);
  for (i = 0; i < wlN; i++) {
    const WlIface *w = wl_achar(wlGlob[i].nome);
    sl(t, 1, "wayland global %s v%u%s", wlGlob[i].nome, wlGlob[i].versao, w ? "" : " (interface table not found)");
  }
  nWlVistos = 0;
  for (i = 0; i < wlN; i++) {
    /* Os do sistema base (wl_compositor, wl_seat...) sao conhecidos: so os da
     * Tizen/Samsung e os de video/cor sao despejados por inteiro. */
    const char *n = wlGlob[i].nome;
    if (!strncmp(n, "wl_", 3) && !strstr(n, "video") && !strstr(n, "color")) continue;
    wl_despejar(t, wl_achar(n), 0);
  }
  if (reg) pdestroy(reg);
  disconnect(dpy);
}

// ---- 3. bibliotecas: funcoes exportadas com nomes de brilho/plano/HDR ---------------
static const char *const CHAVES_SIMBOLO[] = {
  "bright", "lumin", "gamma", "contrast", "picture", "_pq", "pq_", "osd", "graphic_plane", "gfx_plane", "plane_",
  "_plane", "mixer", "hdr", "tonemap", "tone_map", "colorspace", "color_space", "colour", "white", "backlight",
  "dimming", "video_hole", "layer_alpha", "global_alpha", "blend_", "csc", "gain", NULL
};

typedef struct { int64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; } Dirent64;

/* getdents64 direto: o readdir de 32 bits falha com inodes de 64 bits. */
static int listar(const char *dir, void (*cada)(const char *dir, const char *nome, void *u), void *u) {
  char buf[16384];
  int fd = open(dir, O_RDONLY | O_DIRECTORY), n = 0;
  long k;
  if (fd < 0) return -1;
  while ((k = syscall(SYS_getdents64, fd, buf, sizeof buf)) > 0) {
    long o = 0;
    while (o < k) {
      Dirent64 *e = (Dirent64 *)(buf + o);
      if (e->name[0] != '.') { cada(dir, e->name, u); n++; }
      o += e->reclen;
    }
  }
  close(fd);
  return n;
}

static void abrir_proto(const char *dir, const char *nome, void *u) {
  char c[600];
  (void)u;
  /* So as de protocolo (libtizen-*-client, libwayland-*-client): tabelas de
   * interface, sem codigo que rode ao abrir. */
  if (nLibsProto >= 64 || !strstr(nome, "-client.so") || !(!strncmp(nome, "libtizen-", 9) || !strncmp(nome, "libwayland-", 11)))
    return;
  snprintf(c, sizeof c, "%s/%s", dir, nome);
  { void *h = dlopen(c, RTLD_LAZY | RTLD_LOCAL); if (h) libsProto[nLibsProto++] = h; }
}

static void scan_abrir_protocolos(ScanTxt *t) {
  if (nLibsProto) return;
  listar("/usr/lib", abrir_proto, NULL);
  listar("/lib", abrir_proto, NULL);
  sl(t, 0, "wayland: %d protocol client librar%s opened for interface tables", nLibsProto, nLibsProto == 1 ? "y" : "ies");
}

typedef struct { ScanTxt *t; int libs, comAchado, simbolos; } ScanLib;

/* Bibliotecas de imagem da Samsung achadas no scan de 23/10: TODAS as
 * exportacoes vao para o arquivo (sem filtro de nome nem limite de 80), para
 * ver se ha um ganho/brilho da camada grafica. So leitura, como o resto. */
static int lib_inteira(const char *nome) {
  static const char *const L[] = { "libavoc", "libvideoenhance", "libgraphics-control", "libpicture", "libgfx", "libpq", NULL };
  int i;
  for (i = 0; L[i]; i++) if (!strncmp(nome, L[i], strlen(L[i]))) return 1;
  return 0;
}

static void scan_um(const char *dir, const char *nome, void *u) {
  ScanLib *sc = u;
  char c[600], primeiros[400];
  struct stat st;
  int fd, achados = 0, inteira = lib_inteira(nome), todas = 0;
  size_t po = 0;
  unsigned char *m;
  if (!strstr(nome, ".so")) return;
  snprintf(c, sizeof c, "%s/%s", dir, nome);
  if (lstat(c, &st) || !S_ISREG(st.st_mode)) return;   // links: a mesma biblioteca duas vezes
  fd = open(c, O_RDONLY);
  if (fd < 0) return;
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < (off_t)sizeof(Elf32_Ehdr) || st.st_size > (off_t)256 * 1024 * 1024) { close(fd); return; }
  m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (m == MAP_FAILED) return;
  sc->libs++;
  primeiros[0] = 0;
  {
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)m;
    size_t tam = (size_t)st.st_size;
    if (!memcmp(eh->e_ident, ELFMAG, SELFMAG) && eh->e_ident[EI_CLASS] == ELFCLASS32 && eh->e_shoff &&
        eh->e_shentsize == sizeof(Elf32_Shdr) && (size_t)eh->e_shoff + (size_t)eh->e_shnum * sizeof(Elf32_Shdr) <= tam) {
      const Elf32_Shdr *sh = (const Elf32_Shdr *)(m + eh->e_shoff);
      int i;
      for (i = 0; i < eh->e_shnum; i++) {
        const Elf32_Shdr *ds = &sh[i], *ss;
        size_t ns, j;
        if (ds->sh_type != SHT_DYNSYM || ds->sh_link >= eh->e_shnum) continue;
        ss = &sh[ds->sh_link];
        if ((size_t)ds->sh_offset + ds->sh_size > tam || (size_t)ss->sh_offset + ss->sh_size > tam || !ds->sh_entsize) continue;
        ns = ds->sh_size / ds->sh_entsize;
        for (j = 1; j < ns; j++) {
          const Elf32_Sym *sy = (const Elf32_Sym *)(m + ds->sh_offset + j * ds->sh_entsize);
          const char *sn;
          int tp = ELF32_ST_TYPE(sy->st_info), bd = ELF32_ST_BIND(sy->st_info);
          if (sy->st_shndx == SHN_UNDEF || sy->st_name >= ss->sh_size || (tp != STT_FUNC && tp != STT_OBJECT) ||
              (bd != STB_GLOBAL && bd != STB_WEAK)) continue;
          sn = (const char *)(m + ss->sh_offset + sy->st_name);
          if (inteira) { todas++; sl(sc->t, 0, "lib-all %s: %s %s", nome, tp == STT_FUNC ? "fn" : "data", sn); }
          if (!casa(sn, CHAVES_SIMBOLO)) continue;
          /* C++ (DALi, Evas) tem milhares de "Plane"/"Color" de geometria: so
           * os nomes C e os mangled com as palavras mais fortes. */
          if (!strncmp(sn, "_Z", 2) && !strstr(sn, "right") && !strstr(sn, "umin") && !strstr(sn, "amma") &&
              !strstr(sn, "dr") && !strstr(sn, "Picture") && !strstr(sn, "Backlight")) continue;
          achados++;
          if (achados <= 80) sl(sc->t, 0, "lib %s: %s %s", nome, tp == STT_FUNC ? "fn" : "data", sn);
          if (achados <= 6 && po < sizeof primeiros - 80)
            po += (size_t)snprintf(primeiros + po, sizeof primeiros - po, "%s%.70s", po ? ", " : "", sn);
        }
        break;
      }
    }
  }
  munmap(m, (size_t)st.st_size);
  if (inteira) sl(sc->t, 1, "lib %s: all %d exports written to the file (lines \"lib-all %s\")", nome, todas, nome);
  if (achados) {
    sc->comAchado++; sc->simbolos += achados;
    if (achados > 80) sl(sc->t, 0, "lib %s: ... %d more", nome, achados - 80);
    sl(sc->t, sc->comAchado <= 120, "lib %s: %d display-related export(s): %s%s", nome, achados, primeiros, achados > 6 ? ", ..." : "");
  }
}

static void scan_libs(ScanTxt *t) {
  ScanLib sc = { t, 0, 0, 0 };
  static const char *const DIRS[] = { "/usr/lib", "/lib", "/usr/lib/driver", "/usr/lib/dali", "/usr/lib/tv", NULL };
  int i;
  for (i = 0; DIRS[i]; i++) {
    int n = listar(DIRS[i], scan_um, &sc);
    sl(t, 0, "libs: %s: %d entries", DIRS[i], n);
  }
  sl(t, 1, "libs: %d libraries read, %d with display-related exports, %d exports in total (full list in the file)",
     sc.libs, sc.comAchado, sc.simbolos);
}

// ---- 4. chaves de configuracao (vconf) e sysfs: o que a TV guarda de imagem ----------
/* So nomes e valores pequenos, lidos como arquivo; o sandbox pode negar (o
 * log diz). As chaves com nomes de imagem dizem o que o modo de imagem mexe. */
static const char *const CHAVES_CFG[] = {
  "picture", "bright", "lumin", "gamma", "contrast", "hdr", "tone", "osd", "graphic", "gfx", "backlight", "dimm",
  "colour", "color", "white", "peak", "abl", "logo", "eco", "pq", NULL
};

typedef struct { ScanTxt *t; int nivel, vistos, achados, logados; } ScanCfg;

static void cfg_valor(ScanTxt *t, int log, const char *c) {
  unsigned char b[96];
  char txt[200];
  size_t o = 0;
  ssize_t k;
  int fd = open(c, O_RDONLY), i;
  if (fd < 0) { sl(t, log, "cfg %s: unreadable", c); return; }
  k = read(fd, b, sizeof b);
  close(fd);
  if (k < 0) { sl(t, log, "cfg %s: unreadable", c); return; }
  /* vconf: int32 tipo (40 string, 41 int, 42 double, 43 bool) e o valor. */
  if (k >= 8 && b[1] == 0 && b[2] == 0 && b[3] == 0 && (b[0] == 41 || b[0] == 43)) {
    int32_t v; memcpy(&v, b + 4, 4);
    sl(t, log, "cfg %s = %d (%s)", c, v, b[0] == 41 ? "int" : "bool"); return;
  }
  if (k >= 12 && b[0] == 42 && !b[1] && !b[2] && !b[3]) { double v; memcpy(&v, b + 4, 8); sl(t, log, "cfg %s = %g (double)", c, v); return; }
  for (i = (k >= 4 && b[0] == 40 && !b[1]) ? 4 : 0; i < k && o + 4 < sizeof txt; i++)
    o += (size_t)snprintf(txt + o, sizeof txt - o, (b[i] >= 32 && b[i] < 127) ? "%c" : "\\x%02x", b[i]);
  txt[o] = 0;
  sl(t, log, "cfg %s = \"%s\"%s", c, txt, k == (ssize_t)sizeof b ? "..." : "");
}

static void cfg_cada(const char *dir, const char *nome, void *u) {
  ScanCfg *sc = u;
  char c[700];
  struct stat st;
  snprintf(c, sizeof c, "%s/%s", dir, nome);
  if (lstat(c, &st)) return;
  if (++sc->vistos > 60000) return;
  /* Em /sys/class cada entrada e um link para o aparelho: segue so esses, e so 2 niveis. */
  if (S_ISLNK(st.st_mode) && sc->nivel == 0 && !strncmp(dir, "/sys/", 5) && !stat(c, &st) && S_ISDIR(st.st_mode)) {
    int antes = sc->nivel;
    sc->nivel = 6; listar(c, cfg_cada, sc); sc->nivel = antes;
    return;
  }
  if (S_ISDIR(st.st_mode)) {
    if (sc->nivel >= 7) return;
    sc->nivel++; listar(c, cfg_cada, sc); sc->nivel--;
    return;
  }
  if (!casa(c, CHAVES_CFG)) return;
  sc->achados++;
  if (S_ISREG(st.st_mode) && st.st_size <= 4096) cfg_valor(sc->t, sc->logados++ < 120, c);
  else sl(sc->t, 0, "cfg %s (%s, %lld bytes)", c, S_ISREG(st.st_mode) ? "file" : "other", (long long)st.st_size);
}

static void scan_cfg(ScanTxt *t) {
  static const char *const DIRS[] = {
    "/opt/var/kdb/db", "/opt/var/kdb/memory", "/var/run/memory", "/run/memory",
    "/sys/class/backlight", "/sys/class/graphics", "/sys/class/drm", "/sys/class/video4linux", "/sys/class/display",
    "/opt/usr/home/owner/share/picture", "/opt/etc/picture", "/etc/picture", NULL
  };
  int i;
  for (i = 0; DIRS[i]; i++) {
    ScanCfg sc = { t, 0, 0, 0, 0 };
    int n = listar(DIRS[i], cfg_cada, &sc);
    if (n < 0) sl(t, 1, "cfg %s: not readable here", DIRS[i]);
    else sl(t, 1, "cfg %s: %d entries looked at, %d with picture-related names", DIRS[i], sc.vistos, sc.achados);
  }
}

// ---- fio ------------------------------------------------------------------------
static int scanRodando;

static void *scan_fio(void *u) {
  ScanTxt *t = u;
  scan_wayland(t);
  scan_libs(t);
  scan_cfg(t);
  sl(t, 1, "done: full text at http://<TV IP>:8099/ass-snap-display-scan.txt%s",
     hScanNet ? " (the .NET part follows in the log as [assperf] scan .net)" : "");
  fflush(stdout);
  if (t->b) assperf_snap_guardar("ass-snap-display-scan.txt", (unsigned char *)t->b, t->n);
  free(t);
  assperf_bench_estado(0, "Display scan done: see the Subtitles log and /ass-snap-display-scan.txt");
  __atomic_store_n(&scanRodando, 0, __ATOMIC_RELEASE);
  return NULL;
}

void tpkscan_tick(void) {
  if (assperf_get(AP_SCAN)) {
    assperf_definir(AP_SCAN, 0);
    if (__atomic_exchange_n(&scanRodando, 1, __ATOMIC_ACQ_REL)) { printf("[assperf] scan: already running\n"); fflush(stdout); }
    else {
      ScanTxt *t = calloc(1, sizeof *t);
      pthread_t fio;
      if (!t) { __atomic_store_n(&scanRodando, 0, __ATOMIC_RELEASE); return; }
      assperf_bench_estado(1, "Display scan running\xe2\x80\xa6");
      sl(t, 1, "start: read-only scan of the display APIs (nothing found is called)");
      scan_egl(t);   // precisa do contexto GL: este fio
      fflush(stdout);
      if (hScanNet) hScanNet(); else sl(t, 1, ".net: not available on this host package");
      if (pthread_create(&fio, NULL, scan_fio, t) != 0) {
        free(t->b); free(t);
        __atomic_store_n(&scanRodando, 0, __ATOMIC_RELEASE);
        assperf_bench_estado(0, "Display scan: could not start a thread");
      } else pthread_detach(fio);
    }
  }
}
#endif
