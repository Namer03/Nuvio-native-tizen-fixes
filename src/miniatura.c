// SEEK-BAR THUMBNAILS MADE ON THE TV (.tpk only). Replaces the Seekr API on
// the Samsung .tpk: it serves the same seekr_* calls player.c and ajustes.c
// already make (seekr.h), so the drawing code stays as it is. seekr.c and
// seekrquota.c are not built into the .tpk.
//
// While a title plays, a low-priority thread makes one 320x180 JPEG per 10 s
// of the film from the nearest keyframe (miniatura_motor.c), in this order:
//   1. where the person scrubs;
//   2. +-1 min around the playhead;
//   3. one every 5 min over the whole film (a 4K tile takes ~0.75 s on the
//      S95C, so this coarse pass is what makes scrubbing far away show
//      something within the first minute);
//   4. the rest of +-5% of the runtime around the playhead, ahead first;
//   5. one every 60 s; 6. the rest, every 10 s.
// While a slot is not made yet, the seek bar shows the nearest made tile
// (up to 5 min away) with ITS own time, dimmed as "still loading".
// A keyframe that serves several 10 s slots is decoded once.
//
// MEMORY (owner: never run the TV out of memory, at all costs). Before every
// frame MemAvailable must cover a floor plus what the next frame needs
// (mini_motor_memoria); otherwise the decoder is freed and the thread waits.
// One decoded frame at a time; tight memory = 1 thread and the decoder closed
// between frames. The tiles stay compressed in RAM (~8 KB each); only the few
// on screen become textures.
//
// Nothing is kept on the TV: with Google Drive linked (minidrive.c) the set is
// looked up before anything is made and saved when the title stops.
//
// Logs `[miniatura]`, never URLs or keys.
#include "seekr.h"

#ifdef NV_TPK
#include "minidrive.h"
#include "minilab.h"
#include "dados.h"
#include "miniatura_motor.h"
#include "rede.h"
#include "streams.h"
#include "partes.h"
#include "video.h"
#include "gfx.h"
#include "jpegrapido.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define GRADE_MS 10000L
#define TOL_MS 5000L
// MEMORY GATE (knobs in the thumbnail lab, minilab.h): MemAvailable must cover
// a floor (owner's test value 150 MB; the DTS converter with thumbnails on has
// taken this TV, Tizen 9 / 1774 MB, to 175 MB with no trouble) plus the next
// tile's cost. Measured on the TV (logs of 22/10): a 4K tile costs 35-45 MB of
// MemAvailable. Before any is measured the cost is a guess (60 MB for 4K,
// scaled by picture size); after `medirApos` tiles it is the largest of the
// last MEDIDAS measured costs x margemPct (never under MEDIDA_MIN_KB).
#define MEDIDAS 8
#define MEDIDA_MIN_KB (20L * 1024)
#define PERTO_SLOTS 6              // +-1 min around the playhead first
#define GROSSO_SLOTS 30            // then one every 5 min
#define VIZINHO_SLOTS 30           // seek bar: nearest made tile up to 5 min away
#define FATIA 0.05                 // +-5% of the runtime around the playhead
#define SLOTS 4

typedef struct { unsigned char *jpg; int n; long pts; } Tile;

// One title. The display and the worker both hold a reference; the worker
// keeps its own after the title is switched off, so it can still save the set
// to Drive, and frees it last.
typedef struct {
  unsigned g;
  int refs;
  Tile *tiles; unsigned char *falhas; int n;
  int feitos, novos;
  long dur;
  volatile int parar;
  int vivo;                  // the worker thread is running
} Job;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int estado = SEEKR_DESLIGADO;
static unsigned geracao;
static Job *job;                 // the title being shown (NULL = none)
static char urlAtual[4096];      // its link (identity: video_url_atual)
static long durAtual;
static Uint32 batida;            // last seekr_pedir: the player still shows this title

// ------------------------------------------------------------------ the lab's knobs and readout
static MiniAjustes ajustes;
static int ajustesLidos;
static MiniStats lab;            // under mu
#define ARQ_LAB "miniteste.txt"
static const struct { const char *nome; size_t off; } CAMPOS[] = {
#define C(x) { #x, offsetof(MiniAjustes, x) }
  C(pisoMB), C(dtsExtraMB), C(margemPct), C(estimativa4kMB), C(medirApos), C(threads), C(manterDecoder),
  C(prefetch), C(lerBlocos), C(exato), C(tsBlocos), C(tsSonda), C(tsJanelaKB), C(tsPassoKB), C(tsSondasMax),
  C(pausaMs), C(intervaloMs), C(parado)
#undef C
};
void mini_ajustes_padrao(MiniAjustes *a) {
  memset(a, 0, sizeof *a);
  a->pisoMB = 150; a->dtsExtraMB = 0; a->margemPct = 125; a->estimativa4kMB = 60; a->medirApos = 1;
  a->threads = 1; a->manterDecoder = 0; a->prefetch = 1; a->lerBlocos = 4; a->exato = 0;
  a->tsBlocos = 7; a->tsSonda = 0; a->tsJanelaKB = 64; a->tsPassoKB = 512; a->tsSondasMax = 12;
  a->pausaMs = 1500; a->intervaloMs = 30; a->parado = 0;
}
MiniAjustes *mini_ajustes(void) {
  if (!ajustesLidos) {
    char *t = dados_ler(ARQ_LAB), *p;
    ajustesLidos = 1;
    mini_ajustes_padrao(&ajustes);
    for (p = t; p && *p; ) {
      char nome[32]; int v;
      if (sscanf(p, "%31[^=]=%d", nome, &v) == 2)
        for (size_t i = 0; i < sizeof CAMPOS / sizeof *CAMPOS; i++)
          if (!strcmp(nome, CAMPOS[i].nome)) *(int *)((char *)&ajustes + CAMPOS[i].off) = v;
      p = strchr(p, '\n');
      if (p) p++;
    }
    free(t);
  }
  return &ajustes;
}
void mini_ajustes_gravar(void) {
  char buf[1024]; size_t u = 0;
  mini_ajustes();
  for (size_t i = 0; i < sizeof CAMPOS / sizeof *CAMPOS && u < sizeof buf; i++)
    u += (size_t)snprintf(buf + u, sizeof buf - u, "%s=%d\n", CAMPOS[i].nome,
                          *(int *)((char *)&ajustes + CAMPOS[i].off));
  if (!dados_gravar(ARQ_LAB, buf)) { printf("[miniatura] lab: could not save the test values\n"); fflush(stdout); }
}
void mini_stats(MiniStats *s) {
  pthread_mutex_lock(&mu);
  *s = lab;
  pthread_mutex_unlock(&mu);
}
static __attribute__((unused)) void estadoLab(const char *e) {
  pthread_mutex_lock(&mu);
  snprintf(lab.estado, sizeof lab.estado, "%s", e);
  pthread_mutex_unlock(&mu);
}
static long alvoScrub = -1; static Uint32 alvoTick;

// Caller holds mu.
static void soltarJob(Job *j) {
  if (!j || --j->refs > 0) return;
  for (int k = 0; k < j->n; k++) free(j->tiles[k].jpg);
  free(j->tiles); free(j->falhas); free(j);
}

// ------------------------------------------------------------------ helpers
static long memDisponivelKB(void) {
  FILE *f = fopen("/proc/meminfo", "r");
  char l[128]; long v = -1;
  if (!f) return -1;
  while (fgets(l, sizeof l, f)) if (!strncmp(l, "MemAvailable:", 13)) { v = atol(l + 13); break; }
  fclose(f);
  return v;
}
static long rssKB(void) {
  FILE *f = fopen("/proc/self/status", "r");
  char l[128]; long v = -1;
  if (!f) return -1;
  while (fgets(l, sizeof l, f)) if (!strncmp(l, "VmRSS:", 6)) { v = atol(l + 6); break; }
  fclose(f);
  return v;
}
static void dormirJ(Job *j, int ms) { while (ms > 0 && !j->parar) { usleep(100000); ms -= 100; } }

// ------------------------------------------------------------------ pack (Drive file)
// "NVMT" u32 version=1, u32 grid ms, u32 n slots, u32 count, then per tile
// u32 slot, i32 pts ms, u32 len, JPEG bytes. Little-endian.
static void put32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
static uint32_t get32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static unsigned char *empacotar(Job *j, size_t *n, int *conta) {
  size_t tam = 20;
  int c = 0;
  pthread_mutex_lock(&mu);
  for (int k = 0; k < j->n; k++) if (j->tiles[k].jpg) { tam += 12 + (size_t)j->tiles[k].n; c++; }
  unsigned char *b = c ? malloc(tam) : NULL;
  if (b) {
    unsigned char *p = b;
    memcpy(p, "NVMT", 4); put32(p + 4, 1); put32(p + 8, GRADE_MS); put32(p + 12, (uint32_t)j->n); put32(p + 16, (uint32_t)c);
    p += 20;
    for (int k = 0; k < j->n; k++) if (j->tiles[k].jpg) {
      put32(p, (uint32_t)k); put32(p + 4, (uint32_t)j->tiles[k].pts); put32(p + 8, (uint32_t)j->tiles[k].n);
      memcpy(p + 12, j->tiles[k].jpg, (size_t)j->tiles[k].n); p += 12 + j->tiles[k].n;
    }
  }
  pthread_mutex_unlock(&mu);
  *n = b ? tam : 0; *conta = c;
  return b;
}

// Adds the tiles of a pack to the set (never replaces one already made). Count added.
static int desempacotar(Job *jb, const unsigned char *b, size_t n) {
  if (n < 20 || memcmp(b, "NVMT", 4) || get32(b + 4) != 1 || get32(b + 8) != GRADE_MS) return 0;
  uint32_t c = get32(b + 16);
  size_t pos = 20;
  int add = 0;
  pthread_mutex_lock(&mu);
  for (uint32_t i = 0; i < c && pos + 12 <= n; i++) {
    uint32_t k = get32(b + pos), len = get32(b + pos + 8);
    long pts = (long)(int32_t)get32(b + pos + 4);
    if (pos + 12 + len > n || len < 125 || len > 200000) break;
    if ((int)k < jb->n && !jb->tiles[k].jpg) {
      unsigned char *j = malloc(len);
      if (j) { memcpy(j, b + pos + 12, len); jb->tiles[k] = (Tile){ j, (int)len, pts }; jb->feitos++; add++; }
    }
    pos += 12 + len;
  }
  if (add && jb == job) estado = SEEKR_PRONTO;
  pthread_mutex_unlock(&mu);
  return add;
}

// ------------------------------------------------------------------ order of work
static int pronto(const Job *j, int k) { return k >= 0 && k < j->n && j->tiles[k].jpg; }
static int livre(const Job *j, int k) { return k >= 0 && k < j->n && !j->tiles[k].jpg && j->falhas[k] < 2; }

// Next slot to make (-1 = none left). Caller holds mu.
static int proximo(const Job *j, long phMs, int *classe) {
  int ph = (int)(phMs / GRADE_MS), w;
  if (ph >= j->n) ph = j->n - 1;
  if (ph < 0) ph = 0;
  // 1. the scrub target, if the person looked at the seek bar in the last 4 s
  if (alvoScrub >= 0 && (Sint32)(SDL_GetTicks() - alvoTick) < 4000) {
    int k0 = (int)((alvoScrub + GRADE_MS / 2) / GRADE_MS);
    for (int d = 0; d <= 3; d++) {
      if (livre(j, k0 + d)) { *classe = 1; return k0 + d; }
      if (d && livre(j, k0 - d)) { *classe = 1; return k0 - d; }
    }
  }
  // 2. +-1 min around the playhead, ahead first at equal distance
  for (int d = 0; d <= PERTO_SLOTS; d++) {
    if (livre(j, ph + d)) { *classe = 2; return ph + d; }
    if (d && livre(j, ph - d)) { *classe = 2; return ph - d; }
  }
  // 3. one every 5 min over the whole film, nearest to the playhead first
  for (int d = 0; d < j->n; d++) {
    int a = ph + d, b = ph - d;
    if (a % GROSSO_SLOTS == 0 && livre(j, a)) { *classe = 3; return a; }
    if (d && b >= 0 && b % GROSSO_SLOTS == 0 && livre(j, b)) { *classe = 3; return b; }
  }
  // 4. the rest of +-5% of the runtime around the playhead
  w = (int)(j->dur * FATIA / GRADE_MS) + 1;
  for (int d = 0; d <= w; d++) {
    if (livre(j, ph + d)) { *classe = 4; return ph + d; }
    if (d && livre(j, ph - d)) { *classe = 4; return ph - d; }
  }
  // 5. one every 60 s, then 6. the rest, nearest to the playhead first
  for (int passo = 6; passo >= 1; passo -= 5) {
    for (int d = 0; d < j->n; d++) {
      int a = ph + d, b = ph - d;
      if (a % passo == 0 && livre(j, a)) { *classe = passo == 6 ? 5 : 6; return a; }
      if (d && b >= 0 && b % passo == 0 && livre(j, b)) { *classe = passo == 6 ? 5 : 6; return b; }
    }
  }
  return -1;
}

typedef struct {
  Job *j;
  char url[4096], cab[2048];
  char nome[512]; uint64_t bytes;
  char imdb[24]; int t, e;
} Trabalho;

#ifdef NV_DTS_FFMPEG
// ------------------------------------------------------------------ source reader
typedef struct {
  char url[4096], cab[2048], final[4096];
  int cruzou;                    // the redirect left the original origin: no headers
  int64_t total;
  int falhas;
  Job *j;
} Leitor;

static long lerFonte(void *u, int64_t ini, int64_t fim, unsigned char *dst) {
  Leitor *l = u;
  for (int tent = 0; tent < 3 && !l->j->parar; tent++) {
    long n = 0; int64_t total = -1; int st = 0, cross = 0;
    char fin[4096] = "";
    const char *url = l->final[0] ? l->final : l->url;
    const char *cab = l->final[0] && l->cruzou ? "" : l->cab;
    char *b = rede_baixar_trecho64_final(url, cab, ini, fim, &n, &total, &st, &l->j->parar, fin, &cross);
    if (b && n > 0) {
      if (!l->final[0] && fin[0]) { snprintf(l->final, sizeof l->final, "%s", fin); l->cruzou = cross; }
      if (total > 0) l->total = total;
      long k = n < (long)(fim - ini + 1) ? n : (long)(fim - ini + 1);
      memcpy(dst, b, (size_t)k);
      free(b);
      l->falhas = 0;
      return k;
    }
    free(b);
    // A CDN link that expired: resolve the original link again.
    if (l->final[0] && (st == 401 || st == 403 || st == 404 || st == 410)) { l->final[0] = 0; continue; }
    if (st == 0) dormirJ(l->j, 500 * (tent + 1));
  }
  l->falhas++;
  return -1;
}

static void salvarDrive(Job *j, const MiniMeta *meta, const char *id) {
  size_t n; int c;
  if (!minidrive_ligado()) return;
  pthread_mutex_lock(&mu); int ha = j->novos; pthread_mutex_unlock(&mu);
  if (!ha) { if (id[0]) minidrive_tocar(id); return; }
  unsigned char *b = empacotar(j, &n, &c);
  if (!b) return;
  MiniMeta m = *meta; m.quadros = c;
  if (minidrive_enviar(id, &m, b, n) == 0) { pthread_mutex_lock(&mu); j->novos = 0; pthread_mutex_unlock(&mu); }
  free(b);
}

static void *trabalhar(void *u) {
  Trabalho *w = u;
  Job *j = w->j;
  Leitor *l = calloc(1, sizeof *l);
  MiniMotor *m = NULL;
  MiniMeta meta; memset(&meta, 0, sizeof meta);
  char id[128] = "", erro[128] = "";
  // One decoding thread: on the TV 2 threads were no faster while the player
  // decodes 4K (log 4: 451/744 ms vs 1 thread 286-622 ms) and cost more memory.
  int threads = 1, seguidas = 0, feitosAqui = 0;
  unsigned long somaMs = 0;
  long medidas[MEDIDAS] = {0}; int nMedidas = 0;
  Uint32 ultimaPausaLog = 0; int pausasCaladas = 0;
  pthread_mutex_lock(&mu);
  memset(&lab, 0, sizeof lab);
  snprintf(lab.estado, sizeof lab.estado, "starting");
  lab.n = j->n;
  pthread_mutex_unlock(&mu);
  if (!l) goto fim;
  l->j = j;
  snprintf(l->url, sizeof l->url, "%s", w->url);
  snprintf(l->cab, sizeof l->cab, "%s", w->cab);
  meta.durMs = j->dur;
  snprintf(meta.imdb, sizeof meta.imdb, "%s", w->imdb);
  meta.temporada = w->t; meta.episodio = w->e;
  minidrive_chave_nome(w->nome, w->bytes, meta.nk);

  // Let the player settle first.
  for (int k = 0; k < 30 && !j->parar && !video_pronto(); k++) dormirJ(j, 500);
  dormirJ(j, 500);
  if (j->parar) goto fim;

  // Fingerprint: two 64 KiB reads.
  {
    unsigned char *cab = malloc(65536), *cau = malloc(65536);
    long a = cab && cau ? lerFonte(l, 0, 65535, cab) : -1;
    long b = -1;
    if (a > 0 && l->total > 65536) b = lerFonte(l, l->total - 65536, l->total - 1, cau);
    else if (a > 0) { memcpy(cau, cab, (size_t)a); b = a; }
    if (a > 0 && b > 0 && l->total > 0) {
      mini_impressao((uint64_t)l->total, cab, (size_t)a, cau, (size_t)b, meta.fp);
      if (!meta.nk[0]) minidrive_chave_nome(w->nome, (uint64_t)l->total, meta.nk);
    }
    free(cab); free(cau);
    printf("[miniatura] source: %lld MB, fingerprint %s, file name %s\n", (long long)(l->total / 1048576),
           meta.fp[0] ? "read" : "unreadable", meta.nk[0] ? "known" : "unknown");
    fflush(stdout);
  }

  // Google Drive first: a saved set means nothing to make.
  if (minidrive_ligado() && (meta.fp[0] || meta.nk[0]) && !j->parar) {
    unsigned char *b = NULL; size_t n = 0;
    if (minidrive_buscar(&meta, &b, &n, id, sizeof id) == 1) {
      int add = desempacotar(j, b, n);
      printf("[miniatura] %d tile(s) from Google Drive\n", add); fflush(stdout);
      minidrive_tocar(id);
    }
    free(b);
    minidrive_limpeza();
  }
  pthread_mutex_lock(&mu); int faltam = j->feitos < j->n; pthread_mutex_unlock(&mu);
  if (!faltam || j->parar) goto fim;
  if (l->total <= 0) { printf("[miniatura] stopped: source size unknown\n"); goto semPrevia; }

  m = mini_motor_abrir(lerFonte, l, l->total, erro, sizeof erro);
  if (!m) { printf("[miniatura] not made for this file: %s\n", erro); goto semPrevia; }
  {
    const MiniInfo *i = mini_motor_info(m);
    pthread_mutex_lock(&mu);
    snprintf(lab.arquivo, sizeof lab.arquivo, "%s %s %dx%d%s", i->formato, i->codec, i->largura, i->altura, i->hdr ? " HDR" : "");
    pthread_mutex_unlock(&mu);
    printf("[miniatura] %s %s %dx%d %d-bit%s, %d keyframes, %d slots, needs ~%zu MB per frame\n",
           i->formato, i->codec, i->largura, i->altura, i->bits, i->hdr ? " HDR" : "", i->chaves, j->n,
           mini_motor_memoria(m, 1) / 1048576);
    fflush(stdout);
  }

  while (!j->parar) {
    MiniAjustes aj = *mini_ajustes();             // the lab's knobs, re-read every loop
    {
      MiniConfig c;
      mini_motor_config_padrao(&c);
      c.lerBlocos = aj.lerBlocos; c.exato = aj.exato; c.prefetch = aj.prefetch; c.tsBlocos = aj.tsBlocos;
      c.tsSonda = aj.tsSonda; c.tsJanelaKB = aj.tsJanelaKB; c.tsPassoKB = aj.tsPassoKB; c.tsSondasMax = aj.tsSondasMax;
      mini_motor_config(m, &c);
    }
    threads = aj.threads < 1 ? 1 : aj.threads > 3 ? 3 : aj.threads;
    // Pauses: the lab stopped it, the player is not showing this title, or is buffering / opening.
    if (aj.parado) { mini_motor_soltar_decoder(m); estadoLab("stopped in the lab"); dormirJ(j, 500); continue; }
    if ((Sint32)(SDL_GetTicks() - batida) > 6000) { mini_motor_soltar_decoder(m); estadoLab("waiting: title not on screen"); dormirJ(j, 500); continue; }
    if (video_bufferando_ms() > 0 || !video_pronto()) { mini_motor_soltar_decoder(m); estadoLab("waiting: player buffering"); dormirJ(j, 700); continue; }
    // Memory before every frame.
    long disp = memDisponivelKB();
    long precisa = (long)(mini_motor_memoria(m, threads) / 1024);
    {
      // Static guess until measured: the lab's 4K figure, scaled by picture size.
      const MiniInfo *mi = mini_motor_info(m);
      double px = (double)mi->largura * (double)mi->altura / (3840.0 * 2160.0);
      long g = (long)(aj.estimativa4kMB * 1024.0 * (px > 0.05 ? px : 0.05));
      if (g < precisa) precisa = g;
    }
    if (nMedidas >= (aj.medirApos < 1 ? 1 : aj.medirApos)) {
      long maior = 0;
      for (int i = 0; i < nMedidas && i < MEDIDAS; i++) if (medidas[i] > maior) maior = medidas[i];
      maior = maior * aj.margemPct / 100;
      if (maior < MEDIDA_MIN_KB) maior = MEDIDA_MIN_KB;
      precisa = maior;
    }
    int dts = video_dts_estado() == VIDEO_DTS_PREPARANDO || video_dts_estado() == VIDEO_DTS_CONVERTIDO;
    long piso = (long)(aj.pisoMB + (dts ? aj.dtsExtraMB : 0)) * 1024;
    pthread_mutex_lock(&mu);
    lab.livreMB = disp / 1024; lab.custoUsadoMB = precisa / 1024; lab.pisoEfetivoMB = piso / 1024;
    if (disp >= 0 && (lab.menorLivreMB <= 0 || disp / 1024 < lab.menorLivreMB)) lab.menorLivreMB = disp / 1024;
    pthread_mutex_unlock(&mu);
    if (disp >= 0 && disp < piso + precisa) {
      mini_motor_soltar_decoder(m);
      char e[96];
      snprintf(e, sizeof e, "paused: low memory (%ld < %ld + %ld MB)", disp / 1024, piso / 1024, precisa / 1024);
      estadoLab(e);
      // One line when it starts, then at most one every 30 s.
      if (!ultimaPausaLog || SDL_GetTicks() - ultimaPausaLog > 30000) {
        printf("[miniatura] paused: low memory (free %ld MB, needs %ld MB + floor %ld MB%s)%s\n",
               disp / 1024, precisa / 1024, piso / 1024, dts ? ", DTS converting" : "", pausasCaladas ? ", still waiting" : "");
        fflush(stdout);
        ultimaPausaLog = SDL_GetTicks(); pausasCaladas = 0;
        pthread_mutex_lock(&mu); lab.pausasMem++; pthread_mutex_unlock(&mu);
      } else pausasCaladas++;
      dormirJ(j, aj.pausaMs < 100 ? 100 : aj.pausaMs);
      continue;
    }
    ultimaPausaLog = 0; pausasCaladas = 0;

    int classe = 0, k, k2 = -1, c2 = 0;
    long ph = (long)(video_pos() * 1000.0);
    pthread_mutex_lock(&mu);
    k = proximo(j, ph, &classe);
    if (k >= 0 && aj.prefetch) {
      // The slot after this one, for the prefetch: same order, this one taken.
      unsigned char f0 = j->falhas[k];
      j->falhas[k] = 99;
      k2 = proximo(j, ph, &c2);
      j->falhas[k] = f0;
    }
    pthread_mutex_unlock(&mu);
    if (k < 0) break;
    long alvo = k * GRADE_MS;
    if (alvo > j->dur - 1) alvo = j->dur - 1;
    {
      long alvo2 = k2 >= 0 ? k2 * GRADE_MS : -1;
      if (alvo2 > j->dur - 1) alvo2 = j->dur - 1;
      mini_motor_proximo(m, alvo2, TOL_MS);
    }

    // Keyframe shared with a slot already made: copy it, no decode.
    long kf = mini_motor_chave(m, alvo, TOL_MS);
    int irmao = -1;
    if (kf >= 0) {
      pthread_mutex_lock(&mu);
      for (int d = 1; d <= 3 && irmao < 0; d++) {
        if (pronto(j, k - d) && labs(j->tiles[k - d].pts - kf) < 50) irmao = k - d;
        else if (pronto(j, k + d) && labs(j->tiles[k + d].pts - kf) < 50) irmao = k + d;
      }
      if (irmao >= 0) {
        unsigned char *c = malloc((size_t)j->tiles[irmao].n);
        if (c) { memcpy(c, j->tiles[irmao].jpg, (size_t)j->tiles[irmao].n);
                 j->tiles[k] = (Tile){ c, j->tiles[irmao].n, j->tiles[irmao].pts }; j->feitos++; j->novos++; }
        else irmao = -1;
      }
      pthread_mutex_unlock(&mu);
      if (irmao >= 0) continue;
    }

    unsigned char *jpg = NULL; int nj = 0; MiniQuadro q;
    long antes = memDisponivelKB();
    { char e[64]; snprintf(e, sizeof e, "making slot %d (%ld s)", k, alvo / 1000); estadoLab(e); }
    if (mini_motor_quadro(m, alvo, TOL_MS, threads, &j->parar, &jpg, &nj, &q, erro, sizeof erro) < 0) {
      if (j->parar) break;
      printf("[miniatura] slot %d (%lds) failed: %s\n", k, alvo / 1000, erro); fflush(stdout);
      pthread_mutex_lock(&mu); j->falhas[k]++; pthread_mutex_unlock(&mu);
      if (++seguidas >= 3) {
        printf("[miniatura] 3 failures in a row: waiting 30 s\n"); fflush(stdout);
        estadoLab("3 failures in a row: waiting 30 s");
        mini_motor_soltar_decoder(m); dormirJ(j, 30000); seguidas = 0;
      }
      if (l->falhas >= 6) { printf("[miniatura] source unreachable: stopped\n"); break; }
      continue;
    }
    seguidas = 0;
    long depois = memDisponivelKB();
    pthread_mutex_lock(&mu);
    if (!j->tiles[k].jpg) {
      j->tiles[k] = (Tile){ jpg, nj, q.ptsMs }; jpg = NULL; j->feitos++; j->novos++;
      if (j == job) estado = SEEKR_PRONTO;
    }
    int f = j->feitos, n = j->n;
    pthread_mutex_unlock(&mu);
    free(jpg);
    feitosAqui++; somaMs += q.totalMs;
    // One line per tile while the first window fills, then one every 20.
    if (feitosAqui <= 20 || feitosAqui % 20 == 0) {
      printf("[miniatura] slot %d (%lds, class %d) frame %lds decode %ums total %ums, %d thread(s), read %ld KB in %d request(s)",
             k, alvo / 1000, classe, q.ptsMs / 1000, q.decodMs, q.totalMs, q.threads, q.bytesLidos / 1024, q.pedidos);
      if (q.adiantadoKB) printf(" + %ld KB ahead", q.adiantadoKB);
      if (q.sondas) printf(", %d probe(s) %ld KB%s", q.sondas, q.sondaKB, q.achouSonda ? " found" : "");
      printf(", free %ld->%ld MB, rss %ld MB, %d/%d done\n", antes / 1024, depois / 1024, rssKB() / 1024, f, n);
    }
    fflush(stdout);
    // What this tile really cost (MemAvailable before -> after, decoder still open).
    if (antes >= 0 && depois >= 0 && antes > depois) medidas[nMedidas++ % MEDIDAS] = antes - depois;
    pthread_mutex_lock(&mu);
    lab.feitos = f; lab.n = n; lab.feitosSessao = feitosAqui;
    lab.ultimoMs = q.totalMs; lab.ultimoDecodMs = q.decodMs; lab.mediaMs = (unsigned)(somaMs / (unsigned long)feitosAqui);
    lab.ultimoKB = q.bytesLidos / 1024; lab.ultimoPedidos = q.pedidos; lab.adiantadoKB = q.adiantadoKB;
    lab.ultimasSondas = q.sondas; lab.achouSonda = q.achouSonda;
    lab.ultimoCustoMB = antes > depois ? (antes - depois) / 1024 : 0;
    lab.lidosMB = (long)(mini_motor_lidos(m) / 1048576);
    if (depois >= 0 && depois / 1024 < lab.menorLivreMB) lab.menorLivreMB = depois / 1024;
    pthread_mutex_unlock(&mu);
    // Give everything back between frames (unless the lab keeps the decoder open):
    // nothing of ours stays in memory while the player runs.
    if (!aj.manterDecoder) mini_motor_soltar_decoder(m);
    if (f >= n) break;
    if (aj.intervaloMs > 0) dormirJ(j, aj.intervaloMs);   // yield
  }
  estadoLab(j->parar ? "stopped (title closed)" : "finished");
  if (feitosAqui)
    printf("[miniatura] made %d tile(s), %lu ms average, %lld MB read\n", feitosAqui,
           somaMs / (unsigned long)feitosAqui, mini_motor_lidos(m) / 1048576);
  goto fim;
semPrevia:
  pthread_mutex_lock(&mu);
  if (j == job && !j->feitos) estado = SEEKR_SEM_PREVIA;
  pthread_mutex_unlock(&mu);
fim:
  if (m) mini_motor_fechar(m);
  fflush(stdout);
  salvarDrive(j, &meta, id);
  free(l);
  pthread_mutex_lock(&mu);
  j->vivo = 0;
  soltarJob(j);
  pthread_mutex_unlock(&mu);
  free(w);
  return NULL;
}
int mini_lab_disponivel(void) { return 1; }
#else
int mini_lab_disponivel(void) { return 0; }
// NuvioTpk40 (no FFmpeg): nothing can be decoded.
static void *trabalhar(void *u) {
  Trabalho *w = u;
  pthread_mutex_lock(&mu);
  if (w->j == job) estado = SEEKR_SEM_PREVIA;
  w->j->vivo = 0;
  soltarJob(w->j);
  pthread_mutex_unlock(&mu);
  free(w);
  return NULL;
}
#endif

// ------------------------------------------------------------------ title lifecycle
static void nomeDaFonte(char *nome, size_t n, uint64_t *bytes) {
  nome[0] = 0; *bytes = 0;
  const PartesLista *pl = partes_sessao_lista();
  int k = partes_sessao_indice();
  if (pl && k >= 0 && k < pl->n && pl->arq) {      // a file opened inside a source (TorBox's name)
    snprintf(nome, n, "%s", pl->arq[k].nome); *bytes = pl->arq[k].bytes; return;
  }
  const Stream *s = stream_item(stream_atual());
  if (s) { snprintf(nome, n, "%s", s->arquivo); *bytes = s->tamanhoBytes; }
}

void seekr_pedir(const char *imdb, int t, int e, long durMs) {
  const char *url = video_url_atual();
  Trabalho *w;
  Job *j;
  pthread_t f;
  if (!url || !*url) return;
  pthread_mutex_lock(&mu);
  batida = SDL_GetTicks();
  // Same title still on screen: only the heartbeat.
  if (urlAtual[0] && !strcmp(urlAtual, url) && labs(durAtual - durMs) < 3000) { pthread_mutex_unlock(&mu); return; }
  pthread_mutex_unlock(&mu);
  seekr_desligar();                        // the previous title's worker saves its set and ends
  pthread_mutex_lock(&mu);
  snprintf(urlAtual, sizeof urlAtual, "%s", url);
  durAtual = durMs;
  batida = SDL_GetTicks();
  if (!strncmp(url, "http://127.0.0.1", 16) || strstr(url, ".m3u8") || durMs < 60000) {
    estado = SEEKR_SEM_PREVIA; pthread_mutex_unlock(&mu); return;
  }
  pthread_mutex_unlock(&mu);
  j = calloc(1, sizeof *j);
  w = calloc(1, sizeof *w);
  int n = (int)((durMs - 1) / GRADE_MS + 1);
  if (j) { j->tiles = calloc((size_t)n, sizeof *j->tiles); j->falhas = calloc((size_t)n, 1); }
  if (!j || !w || !j->tiles || !j->falhas) {
    if (j) { free(j->tiles); free(j->falhas); }
    free(j); free(w);
    return;
  }
  j->n = n; j->dur = durMs; j->refs = 2; j->vivo = 1;
  w->j = j;
  snprintf(w->url, sizeof w->url, "%s", url);
  snprintf(w->cab, sizeof w->cab, "%s", video_cabecalhos() ? video_cabecalhos() : "");
  nomeDaFonte(w->nome, sizeof w->nome, &w->bytes);
  snprintf(w->imdb, sizeof w->imdb, "%s", imdb ? imdb : "");
  w->t = t; w->e = e;
  pthread_mutex_lock(&mu);
  j->g = ++geracao;
  job = j;
  estado = SEEKR_BUSCANDO;
  alvoScrub = -1;
  pthread_mutex_unlock(&mu);
  printf("[miniatura] new title: %ld min, %d slots of 10 s%s\n", durMs / 60000, n,
         minidrive_ligado() ? ", Google Drive linked" : "");
  fflush(stdout);
  if (pthread_create(&f, NULL, trabalhar, w) == 0) pthread_detach(f);
  else {
    pthread_mutex_lock(&mu);
    j->vivo = 0; soltarJob(j);            // the worker's reference
    estado = SEEKR_SEM_PREVIA;
    pthread_mutex_unlock(&mu);
    free(w);
  }
}

void seekr_desligar(void) {
  // The worker keeps its own reference: it saves the set to Drive and frees it.
  pthread_mutex_lock(&mu);
  if (job) { job->parar = 1; soltarJob(job); job = NULL; }
  geracao++;
  urlAtual[0] = 0; durAtual = 0;
  estado = SEEKR_DESLIGADO;
  pthread_mutex_unlock(&mu);
}

// ------------------------------------------------------------------ seekr.h surface
int seekr_estado(void) { int r; pthread_mutex_lock(&mu); r = estado; pthread_mutex_unlock(&mu); return r; }
int seekr_tem_chave(void) { return 1; }          // nothing to configure: made on this TV
void seekr_definir_chave(const char *c) { (void)c; }
void seekr_chave_propria(int p) { (void)p; }
int seekr_validar(const char *c) { (void)c; return 1; }
void seekr_tentar_novamente(void) {}
void seekr_definir_ajuste_ms(long ms) { (void)ms; }   // frames of this very file: no sync offset
void seekr_ocioso(void) {}
void seekr_uso(SeekrUso *u) { if (u) memset(u, 0, sizeof *u); }
const char *seekr_estado_rotulo(int est) {
  switch (est) {
    case SEEKR_BUSCANDO: return "Buscando miniaturas";
    case SEEKR_PRONTO: return "Miniaturas disponíveis";
    case SEEKR_SEM_PREVIA: return "Este título não tem miniaturas";
    default: return "Miniaturas não solicitadas";
  }
}
int seekr_horario_local(long long utc, char *saida, size_t tamanho) {
  struct tm local; time_t t = (time_t)utc;
  if (!saida || !tamanho) return 0;
  saida[0] = 0;
  if (utc <= 0 || !localtime_r(&t, &local)) return 0;
  return strftime(saida, tamanho, "%d/%m %H:%M", &local) != 0;
}

// Display side (draw thread only): a few textures keyed by slot.
typedef struct { GLuint tex; int slot; unsigned g; Uint32 uso; } Slot;
static Slot slots[SLOTS];
static Uint32 relogioUso;
static int quadroVelho;
int seekr_quadro_velho(void) { return quadroVelho; }

static int slotTex(int k, unsigned g) {
  for (int i = 0; i < SLOTS; i++) if (slots[i].tex && slots[i].slot == k && slots[i].g == g) return i;
  return -1;
}

// Uploads tile k (JPEG -> texture), keeping the slots in `proteger`. Draw thread.
static void subir(int k, unsigned g, const int *proteger, int np) {
  unsigned char *copia = NULL; int n = 0;
  pthread_mutex_lock(&mu);
  if (job && job->g == g && pronto(job, k) && (copia = malloc((size_t)job->tiles[k].n)) != NULL) {
    memcpy(copia, job->tiles[k].jpg, (size_t)job->tiles[k].n); n = job->tiles[k].n;
  }
  pthread_mutex_unlock(&mu);
  if (!copia) return;
  int w0 = 0, h0 = 0;
  SDL_Surface *s = jpeg_rapido_carregar_mem(copia, (size_t)n, 1 << 20, &w0, &h0);
  if (!s) { SDL_RWops *rw = SDL_RWFromConstMem(copia, n); s = rw ? IMG_Load_RW(rw, 1) : NULL; }
  free(copia);
  if (!s) return;
  if (s->format->format != SDL_PIXELFORMAT_ABGR8888) {
    SDL_Surface *c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(s); s = c;
    if (!s) return;
  }
  int alvo = -1;
  for (int i = 0; i < SLOTS; i++) {
    int prot = 0;
    for (int q = 0; q < np; q++) if (slots[i].tex && slots[i].g == g && slots[i].slot == proteger[q]) prot = 1;
    if (prot) continue;
    if (!slots[i].tex) { alvo = i; break; }
    if (alvo < 0 || slots[i].uso < slots[alvo].uso) alvo = i;
  }
  if (alvo < 0) alvo = 0;
  if (!slots[alvo].tex) glGenTextures(1, &slots[alvo].tex);
  glBindTexture(GL_TEXTURE_2D, slots[alvo].tex);
  if (SDL_MUSTLOCK(s)) SDL_LockSurface(s);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s->w, s->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, s->pixels);
  if (SDL_MUSTLOCK(s)) SDL_UnlockSurface(s);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gfx_tex_esquecer(0);
  SDL_FreeSurface(s);
  slots[alvo].slot = k; slots[alvo].g = g; slots[alvo].uso = ++relogioUso;
}

// Nearest slot with a tile within `raio` slots of k (-1 none). Caller holds mu.
static int vizinho(const Job *j, int k, int raio) {
  for (int d = 0; d <= raio; d++) {
    if (pronto(j, k + d)) return k + d;
    if (d && pronto(j, k - d)) return k - d;
  }
  return -1;
}

int seekr_quadros(double posSeg, int n, GLuint *texs, double *cueSeg) {
  int quer[3], nq = 0, i, achou = 0, atualK = -1, antes = -1, depois = -1, longe = 0;
  unsigned g = 0;
  if (n < 1) return 0;
  if (n > 3) n = 3;
  for (i = 0; i < n; i++) { texs[i] = 0; if (cueSeg) cueSeg[i] = -1.0; }
  long pos = (long)(posSeg * 1000.0);
  pthread_mutex_lock(&mu);
  // The person is looking here: it becomes the first thing to make.
  alvoScrub = pos; alvoTick = SDL_GetTicks();
  if (!job) { pthread_mutex_unlock(&mu); return 0; }
  g = job->g;
  int k = (int)((pos + GRADE_MS / 2) / GRADE_MS);
  if (k >= job->n) k = job->n - 1;
  atualK = vizinho(job, k, VIZINHO_SLOTS);          // nearest made tile, up to 5 min away
  longe = atualK >= 0 && abs(atualK - k) > 1;
  if (atualK >= 0) {
    // Strip neighbours: the nearest made tile on each side.
    for (int d = 1; d <= VIZINHO_SLOTS && antes < 0; d++) if (pronto(job, atualK - d)) antes = atualK - d;
    for (int d = 1; d <= VIZINHO_SLOTS && depois < 0; d++) if (pronto(job, atualK + d)) depois = atualK + d;
    quer[nq++] = atualK;
    if (n > 1 && antes >= 0) quer[nq++] = antes;
    if (n > 1 && depois >= 0) quer[nq++] = depois;
    if (cueSeg) {
      if (n == 1) cueSeg[0] = job->tiles[atualK].pts / 1000.0;
      else {
        cueSeg[0] = antes >= 0 ? job->tiles[antes].pts / 1000.0 : -1.0;
        cueSeg[1] = job->tiles[atualK].pts / 1000.0;
        cueSeg[2] = depois >= 0 ? job->tiles[depois].pts / 1000.0 : -1.0;
      }
    }
  }
  pthread_mutex_unlock(&mu);
  // At most one JPEG decode per call (320x180: a few ms): the current one first.
  for (i = 0; i < nq; i++) if (slotTex(quer[i], g) < 0) { subir(quer[i], g, quer, nq); break; }
  quadroVelho = longe;                               // not this spot's own tile yet: shown dimmed
  if (atualK >= 0) {
    int ordem[3] = { n == 1 ? atualK : antes, atualK, depois };
    int no = n == 1 ? 1 : 3;
    for (i = 0; i < no; i++) {
      int sl = ordem[i] >= 0 ? slotTex(ordem[i], g) : -1;
      if (sl >= 0) { texs[i] = slots[sl].tex; slots[sl].uso = ++relogioUso; if (ordem[i] == atualK) achou = 1; }
    }
  }
  if (!achou) {
    // Not made yet (or still uploading): the last one shown, marked as old.
    int m = -1, c = n == 1 ? 0 : 1;
    for (i = 0; i < SLOTS; i++) if (slots[i].tex && slots[i].g == g && (m < 0 || slots[i].uso > slots[m].uso)) m = i;
    if (m >= 0) { texs[c] = slots[m].tex; achou = 1; quadroVelho = 1; }
  }
  return achou;
}

GLuint seekr_quadro(double posSeg, double *cueSeg) {
  GLuint t = 0;
  seekr_quadros(posSeg, 1, &t, cueSeg);
  return t;
}

#ifdef NV_SHOT_HOOKS
void seekr_shot(int est, const GLuint *t, const double *cue, int velho) { (void)est; (void)t; (void)cue; (void)velho; }
#endif

#endif // NV_TPK
