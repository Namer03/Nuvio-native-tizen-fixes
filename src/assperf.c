// Ver assperf.h.
#include "assperf.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef NV_TPK
#include "dados.h"
#include "avisos.h"
#endif

#define AP_ARQ   "assperf.txt"
#define AP_MARCA "assperf-ativo.txt"

typedef struct {
  const char *chave;     // no arquivo
  const char *rotulo;    // na folha
  const char *ajuda;     // uma linha na folha
  int recomendado, leve; // presets (o Legacy e sempre 0)
  int n;
  const char *valores[31];
  int fixa;              // 1 = decidida na TV (23/10): fora da folha, sempre `recomendado` no .tpk
} ApDef;

static const ApDef DEF[AP_N] = {
  [AP_MOTOR] = { "motor", "Bitmap engine",
    "NEON runs libass's blur, rasterizer and blending with ARM SIMD; the picture is bit-identical. C is libass's plain engine.",
    1, 1, 2, { "C (old)", "NEON" }, 1 },
  [AP_UPLOAD] = { "upload", "Upload format",
    "Alpha 8-bit sends 1 byte per pixel and tints on the GPU; RGBA builds 4 bytes per pixel on the CPU.",
    1, 1, 2, { "RGBA (old)", "Alpha 8-bit" }, 1 },
  [AP_TEXCACHE] = { "texcache", "Reuse unchanged images",
    "Keeps a texture per image content: a sign that did not change is not uploaded again.",
    1, 1, 2, { "Off (old)", "On" }, 1 },
  [AP_TEXANEL] = { "texanel", "Texture recycle delay",
    "How many frames a texture rests before it is reused, so the GPU never waits on one still in use.",
    1, 1, 3, { "1 frame", "2 frames", "3 frames" }, 1 },
  [AP_FORATRAVA] = { "foratrava", "Upload outside lock",
    "Uploads after releasing the lock the libass worker needs, so a slow upload never stalls rendering.",
    1, 1, 2, { "Off (old)", "On" }, 1 },
  [AP_INCREMENTAL] = { "incremental", "Batch loading",
    "New lines only adds just the lines each harvested batch brought; the old way re-reads the whole document.",
    1, 1, 2, { "Full reparse (old)", "New lines only" }, 1 },
  [AP_PODA] = { "poda", "Drop past events",
    "With new-lines loading, events that ended this long before the playhead leave libass memory.",
    2, 2, 4, { "Off", "30 s", "2 min", "5 min" }, 1 },
  [AP_ADIANTE] = { "adiante", "Render ahead",
    "Renders the moment the frame will actually be on screen. 100 ms (tested 24/10) also gives the GPU blur time to prepare queued frames.",
    3, 3, 6, { "Off (old)", "Auto", "50 ms", "100 ms", "150 ms", "250 ms" } },
  [AP_SEGURAR] = { "segurar", "Early frames",
    "With render ahead, a frame that is ready early waits for its moment instead of showing at once.",
    1, 1, 2, { "Show at once", "Hold until due" }, 1 },
  [AP_CACHE] = { "cache", "libass bitmap cache",
    "Memory libass keeps for rendered glyphs and blurs. Bigger means fewer re-renders of repeated signs. With the blur on the GPU the blurred pictures live there, so 32 MB is enough (25/10, memory).",
    0, 1, 4, { "32 MB", "64 MB", "128 MB", "16 MB" } },
  [AP_GLIFOS] = { "glifos", "libass glyph cache",
    "How many glyph outlines libass keeps.",
    0, 0, 4, { "Default (old)", "1000", "4000", "10000" }, 1 },
  [AP_ESCALA] = { "escala", "Render resolution",
    "Renders subtitles smaller and scales them up. Blur-heavy signs get much cheaper; text gets softer. "
    "Auto stays at 100% and drops to 75% only while the subtitles are heavy (the log says when).",
    5, 2, 6, { "100% (old)", "85%", "75%", "66%", "50%", "Auto (75% when heavy)" } },
  [AP_HINTING] = { "hinting", "Font hinting",
    "None is the fastest; Light is what the app always used.",
    0, 1, 4, { "Light (old)", "None", "Normal", "Native" } },
  [AP_SHAPER] = { "shaper", "Text shaper",
    "Simple skips HarfBuzz: faster, but Arabic, Thai and other complex scripts can break.",
    0, 0, 2, { "Complex (old)", "Simple" }, 1 },
  [AP_FONTES] = { "fontes", "Attached fonts limit",
    "Largest MKV Attachments block that is read. Fansub CJK fonts often pass 16 MB.",
    3, 3, 5, { "16 MB (old)", "32 MB", "48 MB", "64 MB", "96 MB" }, 1 },
  [AP_CUES] = { "cues", "App cue copy",
    "The app's own cue list (fallback overlay, sync reference) is rebuilt from the whole document on each batch.",
    1, 1, 2, { "Every batch (old)", "Every 10 s" }, 1 },
  [AP_RELAY] = { "relay", "Read subs from local relay",
    "Takes subtitle blocks from the bytes the TV player already downloads: earlier lines, fewer CDN requests.",
    1, 1, 2, { "Off (old)", "On" }, 1 },
  [AP_PRIORIDADE] = { "prioridade", "Render thread priority",
    "Higher asks the system for a better slot (may be refused; the log says).",
    0, 0, 3, { "Normal (old)", "Higher", "Lower" }, 1 },
  [AP_MUDANCA] = { "mudanca", "Render only on changes",
    "Skips libass when no event starts, ends or animates before the next moment: ~24 renders/s on frame-by-frame signs instead of 60.",
    1, 1, 2, { "Every frame (old)", "On" }, 1 },
  [AP_ESTAGIOS] = { "estagios", "Stage timers",
    "Adds an [assperf] stages: line to the 10 s log: libass time per frame split into shaping, raster, blur, compose, clip.",
    1, 1, 2, { "Off", "On" } },
  [AP_BENCH] = { "bench", "ASS bench",
    "Pause first. \xe2\x86\x92 runs this engine, \xe2\x86\x90 runs C vs NEON: the 15 s before the playhead, re-rendered off-screen, cold caches.",
    0, 0, 3, { "Off", "Run (this engine)", "Run C vs NEON" } },
  [AP_ESTILO] = { "estilo", "Your style on styled ASS",
    "Ignored draws styled ASS as authored, like the phone. Applies puts your Style colour, border, box, font and opacity on every sign (masks turn grey, rings appear).",
    1, 1, 2, { "Applies (old)", "Ignored" } },
  [AP_FIOS] = { "fios", "Blur threads",
    "Big blurs are split across this many CPU cores inside libass. The picture is identical; more threads = faster heavy signs.",
    2, 2, 4, { "1 (old)", "2", "3", "4" } },
  [AP_GPUBLUR] = { "gpublur", "Blur on GPU",
    "On: libass leaves the blur to the GPU (shaders) and the CPU does the rest. Very close to libass's blur, not bit-identical (Snapshot measures the difference). Off: libass's exact CPU blur.",
    1, 1, 2, { "Off (CPU, exact)", "On" } },
  [AP_GPUMIN] = { "gpumin", "GPU blur from size",
    "Only signs at least this big go to the GPU; smaller blurs stay on the CPU, where they are cheaper. The log times both by size (Stage timers On).",
    2, 2, 4, { "All", "16k px", "64k px", "256k px" } },
  [AP_LOG] = { "log", "Subtitle log",
    "Summary writes one [assperf] line every 10 s. Detailed also logs each slow frame.",
    1, 1, 3, { "Off", "Summary (10 s)", "Detailed" } },
  [AP_GAMA] = { "gama", "Subtitle gamma (match TV)",
    "Set the same number as the TV's Gamma/BT.1886 (it only changes the video): subtitles get Samsung's own curve "
    "(1 - 0.0426 per step).",
    0, 0, 11, { "Off", "-1", "-2", "-3", "-4", "-5", "+5", "+4", "+3", "+2", "+1" } },
  [AP_FILA] = { "fila", "GPU queue limit",
    "Waits at the start of a frame until the GPU is at most this many frames behind. Heavy signs reach the screen sooner "
    "(less lag), but the menu and subtitles draw fewer frames per second. The [relogio] line shows the lag and the wait.",
    0, 0, 3, { "Off", "1 frame", "2 frames" } },
  [AP_LAGCOMP] = { "lagcomp", "Heavy-scene compensation",
    "On: when the GPU falls behind on heavy signs, the subtitle correction gets smaller by the measured extra lag, "
    "so one correction set on light scenes holds on heavy ones too. Off: the correction stays fixed.",
    1, 1, 2, { "Off", "On" } },
  [AP_CONTRASTE] = { "contraste", "Subtitle contrast (match TV)",
    "Set the same number as the TV's Contrast (it only changes the video). 30 is where subtitle white matches video "
    "white; lower values dim subtitles the way the TV dims the video (Samsung's gain, (c + 70) / 100).",
    0, 0, 31, { "30 (TV default)", "29", "28", "27", "26", "25", "24", "23", "22", "21", "20", "19", "18", "17", "16", "15", "14", "13", "12", "11", "10", "9", "8", "7", "6", "5", "4", "3", "2", "1", "0" } },
};

static int val[AP_N] __attribute__((unused));
static int iniciado;
static unsigned versao;
static pthread_mutex_t trava = PTHREAD_MUTEX_INITIALIZER;

#ifdef NV_TPK
/* 0 Recommended, 2 Lightest, 1 "Safe" (a guarda de queda): Recommended com
 * 1 fio de blur. O Legacy de antes nao existe mais: as chaves fixas (decididas
 * na TV) ficam sempre no valor bom. */
static void aplicarPreset(int p) {
  for (int i = 0; i < AP_N; i++) {
    if (assperf_e_medida(i)) continue;   // medida nao e comportamento: o preset nao mexe
    val[i] = p == 2 ? DEF[i].leve : DEF[i].recomendado;
  }
  if (p == 1) { val[AP_FIOS] = 0; val[AP_GPUBLUR] = 0; }
}

static void gravar(void) {
  char b[1024]; size_t o = 0;
  for (int i = 0; i < AP_N && o < sizeof b; i++)
    o += (size_t)snprintf(b + o, sizeof b - o, "%s=%d\n", DEF[i].chave, val[i]);
  dados_gravar(AP_ARQ, b);
}
#endif

// O arquivo e lido UMA vez, sob a trava; `iniciado` so vira 1 (release) com
// os valores ja no lugar, entao um assperf_get de outro fio nunca le meio
// arquivo. Chamar antes de avisos_iniciar perderia a guarda de queda: o
// primeiro uso e sempre de uma faixa ASS ou da folha, bem depois do arranque.
void assperf_iniciar(void) {
  if (__atomic_load_n(&iniciado, __ATOMIC_ACQUIRE)) return;
  pthread_mutex_lock(&trava);
  if (iniciado) { pthread_mutex_unlock(&trava); return; }
#ifdef NV_TPK
  {
    char *t = dados_ler(AP_ARQ), *marca;
    aplicarPreset(0);
    val[AP_LOG] = DEF[AP_LOG].recomendado;
    val[AP_ESTAGIOS] = DEF[AP_ESTAGIOS].recomendado;
    if (t) {
      char *l = t, *nl;
      for (; l && *l; l = nl) {
        nl = strchr(l, '\n'); if (nl) *nl++ = 0;
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = 0;
        for (int i = 0; i < AP_N; i++)
          if (!strcmp(l, DEF[i].chave)) {
            int v = atoi(eq + 1);
            if (v >= 0 && v < DEF[i].n) val[i] = v;
          }
      }
      free(t);
    }
    val[AP_BENCH] = 0;   // acoes: nunca voltam ligadas do arquivo
    for (int i = 0; i < AP_N; i++) if (DEF[i].fixa) val[i] = DEF[i].recomendado;
    marca = dados_ler(AP_MARCA);
    if (marca) {
      free(marca);
      dados_apagar(AP_MARCA);
      if (avisos_sessao_anterior_caiu()) {
        aplicarPreset(1);   // as chaves de medida ficam como estavam
        gravar();
        printf("[assperf] the previous session crashed while ASS subtitles were drawing: "
               "options back to Recommended with 1 blur thread and the blur on the CPU (Safe)\n");
      }
    }
  }
  __atomic_store_n(&iniciado, 1, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&trava);
  {
    char r[512];
    assperf_resumo(r, sizeof r);
    printf("[assperf] options (%s): %s\n", assperf_preset_nome(), r);
    fflush(stdout);
  }
#else
  __atomic_store_n(&iniciado, 1, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&trava);
#endif
}

#ifdef NV_ASSPERF_TESTE
/* Provas no computador (sem NV_TPK): liga as chaves sem arquivo. */
void assperf_teste_definir(int id, int v) {
  __atomic_store_n(&iniciado, 1, __ATOMIC_RELEASE);
  if (id >= 0 && id < AP_N) { val[id] = v; __atomic_add_fetch(&versao, 1, __ATOMIC_ACQ_REL); }
}
#endif

int assperf_get(int id) {
#ifdef NV_ASSPERF_TESTE
  return id >= 0 && id < AP_N ? __atomic_load_n(&val[id], __ATOMIC_RELAXED) : 0;
#elif defined(NV_TPK)
  if (id < 0 || id >= AP_N) return 0;
  if (DEF[id].fixa) return DEF[id].recomendado;
  assperf_iniciar();
  return __atomic_load_n(&val[id], __ATOMIC_RELAXED);
#else
  (void)id;
  return 0;
#endif
}

int assperf_e_medida(int id) {
  return id == AP_LOG || id == AP_ESTAGIOS || id == AP_BENCH ||
         id == AP_GAMA || id == AP_CONTRASTE;   // calibracao: fora dos presets e da guarda, mas fica gravada
}
int assperf_fixa(int id) { return id >= 0 && id < AP_N && DEF[id].fixa; }
int assperf_fios(void) { int v = assperf_get(AP_FIOS); return v < 0 ? 1 : v > 3 ? 4 : v + 1; }

int assperf_n_valores(int id) { return id >= 0 && id < AP_N ? DEF[id].n : 0; }
const char *assperf_rotulo(int id) { return id >= 0 && id < AP_N ? DEF[id].rotulo : ""; }
const char *assperf_ajuda(int id) { return id >= 0 && id < AP_N ? DEF[id].ajuda : ""; }
const char *assperf_valor_rotulo(int id, int v) {
  if (id < 0 || id >= AP_N || v < 0 || v >= DEF[id].n) return "";
  if (id == AP_BENCH && assperf_bench_rodando()) return "Running\xe2\x80\xa6";
  return DEF[id].valores[v];
}
unsigned assperf_versao(void) { return __atomic_load_n(&versao, __ATOMIC_ACQUIRE); }

void assperf_definir(int id, int v) {
#ifdef NV_TPK
  char r[512];
  if (id < 0 || id >= AP_N || v < 0 || v >= DEF[id].n) return;
  assperf_iniciar();
  pthread_mutex_lock(&trava);
  __atomic_store_n(&val[id], v, __ATOMIC_RELAXED);
  gravar();
  pthread_mutex_unlock(&trava);
  __atomic_add_fetch(&versao, 1, __ATOMIC_ACQ_REL);
  assperf_resumo(r, sizeof r);
  printf("[assperf] %s -> %s | options (%s): %s\n", DEF[id].rotulo, DEF[id].valores[v], assperf_preset_nome(), r);
  fflush(stdout);
#else
  (void)id; (void)v;
#endif
}

void assperf_preset(int p) {
#ifdef NV_TPK
  char r[512];
  if (p < 0 || p > 2) return;
  assperf_iniciar();
  pthread_mutex_lock(&trava);
  aplicarPreset(p);
  gravar();
  pthread_mutex_unlock(&trava);
  __atomic_add_fetch(&versao, 1, __ATOMIC_ACQ_REL);
  assperf_resumo(r, sizeof r);
  printf("[assperf] preset %s | options: %s\n", assperf_preset_nome(), r);
  fflush(stdout);
#else
  (void)p;
#endif
}

const char *assperf_preset_nome(void) {
  int rec = 1, lev = 1, safe = 1;
  for (int i = 0; i < AP_N; i++) {
    int v = assperf_get(i);
    if (assperf_e_medida(i) || DEF[i].fixa) continue;
    if (v != DEF[i].recomendado) rec = 0;
    if (v != DEF[i].leve) lev = 0;
    if (v != (i == AP_FIOS || i == AP_GPUBLUR ? 0 : DEF[i].recomendado)) safe = 0;
  }
  return rec ? "Recommended" : lev ? "Lightest" : safe ? "Safe" : "Custom";
}

void assperf_resumo(char *dst, size_t n) {
  size_t o = 0;
  if (!n) return;
  dst[0] = 0;
  for (int i = 0; i < AP_N && o + 1 < n; i++) {
    int k = snprintf(dst + o, n - o, "%s%s=%d", i ? " " : "", DEF[i].chave, assperf_get(i));
    if (k < 0) break;
    o += (size_t)k;
  }
}

size_t assperf_fontes_teto(void) {
  static const size_t mb[5] = { 16, 32, 48, 64, 96 };
  int v = assperf_get(AP_FONTES);
  return mb[v < 0 || v > 4 ? 0 : v] * 1024u * 1024u;
}
/* ---- Render resolution Auto ---------------------------------------------------
 * 100 % normally; 75 % while the subtitles are heavy (dono 25/10: "only as a
 * fallback when subtitles get heavy"). The cost is what one NEW subtitle frame
 * takes: libass on the worker plus upload/GPU blur on the GL thread, each an
 * average over the last frames that changed. Measured on the S95C (24/10, the
 * inbox scene): heavy = 40-90 ms per frame at 100 %, ordinary dialogue 1-10 ms.
 *   - down to 75 % when the average passes AUTO_DESCE_MS;
 *   - back to 100 % after AUTO_LEVE_S seconds below AUTO_SOBE_MS (at 75 %), and
 *     never before AUTO_MIN75_S seconds at 75 %;
 *   - after a switch the first AUTO_IGNORA frames do not count (cold caches at
 *     the new size would read as "heavy" and bounce straight back);
 *   - a pause in new frames (a heavy sign standing still) is NOT light: in
 *     log 2 of 25/10 Auto went back to 100 % on "libass 0.0 + GL 0.0" and was
 *     heavy again a second later, 2-3 switches per 10 s, each one redrawing
 *     everything at the new size. Only AUTO_OCIOSO_S with nothing new (the
 *     scene is over) counts as light; shorter pauses neither count nor reset. */
#define AUTO_DESCE_MS  28.0
#define AUTO_FORTE_MS  45.0   // this heavy goes down even right after coming back up
#define AUTO_SOBE_MS   12.0
#define AUTO_LEVE_S     3.0
#define AUTO_MIN75_S    5.0
#define AUTO_MIN100_S   2.0
#define AUTO_IGNORA       3
#define AUTO_OCIOSO_S   8.0
static pthread_mutex_t autoTrava = PTHREAD_MUTEX_INITIALIZER;
static struct {
  int em75;                  // 1 = rendering at 75 % now
  double emaLib, emaGl;      // ms per new frame
  int nLib, nGl, ignora;
  double ultAmostra, ultLib, ultGl, desde, leveDesde;   // agoraS()
  unsigned trocas;           // for the 10 s line
  double seg75, marca;       // seconds at 75 % in this window
} A;

static double agoraS(void);

static double auto_custo_locked(double t) {
  /* Each side ages on its own: a side with no new frame for 1 s counts 0. */
  return (t - A.ultLib > 1.0 ? 0.0 : A.emaLib) + (t - A.ultGl > 1.0 ? 0.0 : A.emaGl);
}

static void auto_trocar_locked(int em75, double t) {
  if (A.em75) A.seg75 += t - A.marca;
  if (em75)
    printf("[assperf] render resolution Auto: 75 %% (subtitles heavy: libass %.1f + GL %.1f ms per new frame)\n",
           A.emaLib, A.emaGl);
  else
    printf("[assperf] render resolution Auto: back to 100 %% (light for %.0f s at 75 %%: libass %.1f + GL %.1f ms per new frame)\n",
           t - A.leveDesde, t - A.ultLib > 1.0 ? 0.0 : A.emaLib, t - A.ultGl > 1.0 ? 0.0 : A.emaGl);
  fflush(stdout);
  A.em75 = em75; A.desde = t; A.marca = t; A.leveDesde = 0;
  A.ignora = AUTO_IGNORA; A.trocas++;
  A.emaLib = A.emaGl = 0; A.nLib = A.nGl = 0;
}

double assperf_escala_quadro(void) {
  static const double e[5] = { 1.0, 0.85, 0.75, 0.66, 0.5 };
  int v = assperf_get(AP_ESCALA);
  double t, custo, r;
  if (v != 5) {
    pthread_mutex_lock(&autoTrava);
    if (A.em75) { A.seg75 += agoraS() - A.marca; A.em75 = 0; }
    pthread_mutex_unlock(&autoTrava);
    return e[v < 0 || v > 4 ? 0 : v];
  }
  t = agoraS();
  pthread_mutex_lock(&autoTrava);
  custo = auto_custo_locked(t);
  if (!A.em75) {
    int pronto = A.ignora <= 0 && (A.nLib > 0 || A.nGl > 0);
    if (pronto && (custo > AUTO_FORTE_MS || (custo > AUTO_DESCE_MS && t - A.desde >= AUTO_MIN100_S)))
      auto_trocar_locked(1, t);
  } else {
    double parado = t - A.ultAmostra;
    if (parado > AUTO_OCIOSO_S || (parado <= 1.0 && custo < AUTO_SOBE_MS && A.ignora <= 0 && (A.nLib > 0 || A.nGl > 0))) {
      if (!A.leveDesde) A.leveDesde = t;
      if (t - A.leveDesde >= AUTO_LEVE_S && t - A.desde >= AUTO_MIN75_S) auto_trocar_locked(0, t);
    } else if (parado <= 1.0 && A.ignora <= 0 && custo >= AUTO_SOBE_MS) A.leveDesde = 0;
    /* else: a pause of 1-8 s, or frames still being ignored: hold */
  }
  r = A.em75 ? 0.75 : 1.0;
  pthread_mutex_unlock(&autoTrava);
  return r;
}

int assperf_legendas_pesadas(void) {
  double t = agoraS();
  int r;
  pthread_mutex_lock(&autoTrava);
  r = A.em75 || (t - A.ultAmostra < 2.0 && auto_custo_locked(t) > AUTO_DESCE_MS);
  pthread_mutex_unlock(&autoTrava);
  return r;
}

void assperf_escala_custo(double libassMs) {
  pthread_mutex_lock(&autoTrava);
  A.ultAmostra = A.ultLib = agoraS();
  if (A.ignora > 0) A.ignora--;
  else { A.emaLib = A.nLib ? A.emaLib * 0.7 + libassMs * 0.3 : libassMs; A.nLib++; }
  pthread_mutex_unlock(&autoTrava);
}

void assperf_escala_custo_gl(double glMs) {
  pthread_mutex_lock(&autoTrava);
  A.ultAmostra = A.ultGl = agoraS();
  if (A.ignora <= 0) { A.emaGl = A.nGl ? A.emaGl * 0.7 + glMs * 0.3 : glMs; A.nGl++; }
  pthread_mutex_unlock(&autoTrava);
}

void assperf_escala_reset(void) {
  pthread_mutex_lock(&autoTrava);
  if (A.em75) { A.seg75 += agoraS() - A.marca; A.em75 = 0; }
  A.emaLib = A.emaGl = 0; A.nLib = A.nGl = 0; A.ignora = 0; A.leveDesde = 0; A.desde = agoraS();
  pthread_mutex_unlock(&autoTrava);
}

double assperf_escala(void) {
  static const double e[5] = { 1.0, 0.85, 0.75, 0.66, 0.5 };
  int v = assperf_get(AP_ESCALA), em75;
  if (v != 5) return e[v < 0 || v > 4 ? 0 : v];
  pthread_mutex_lock(&autoTrava); em75 = A.em75; pthread_mutex_unlock(&autoTrava);
  return em75 ? 0.75 : 1.0;
}
int assperf_adiante_fixo_ms(void) {
  static const int ms[6] = { 0, -1, 50, 100, 150, 250 };
  int v = assperf_get(AP_ADIANTE);
  return ms[v < 0 || v > 5 ? 0 : v];
}
/* O slider de gama da TV (-5..+5) escolhe uma curva relativa na LUT
 * GAMMATBLOSD_384 do libave.so: out = in^(1 - 0,0426 x slider), medido nas
 * 11 curvas (avoc_gamma_contrast_picture.md, 24/10). "-" = gama maior (meios
 * mais escuros). So o video passa por ela; a legenda recebe a mesma conta.
 * Preto e branco nao mudam. */
double assperf_gama(void) {
  int v = assperf_get(AP_GAMA);
  int n = v <= 0 || v > 10 ? 0 : v <= 5 ? -v : 11 - v;
  return 1.0 - 0.0426 * n;
}
/* O Contraste da TV e um GANHO por canal RGB antes da curva de gama
 * (PontusMVideoEnhancer::m_CalculateContrast no libave.so: 944 x (c + 70) /
 * 120; avoc_contrast_application.md, 24/10). Relativo ao 30 (onde o branco
 * da legenda bate com o do video na S95C): (c + 70) / 100. */
double assperf_contraste(void) {
  int v = assperf_get(AP_CONTRASTE);
  int c = 30 - (v < 0 || v > 30 ? 0 : v);
  return (c + 70) / 100.0;
}
long assperf_poda_ms(void) {
  static const long ms[4] = { 0, 30000, 120000, 300000 };
  int v = assperf_get(AP_PODA);
  return ms[v < 0 || v > 3 ? 0 : v];
}

void assperf_faixa_ativa(int ativa) {
#ifdef NV_TPK
  static int marcada;
  int teste = 0;
  for (int i = 0; i < AP_N; i++) if (!assperf_e_medida(i) && assperf_get(i)) teste = 1;
  if (assperf_get(AP_FIOS) == 0 && assperf_get(AP_GPUBLUR) == 0) teste = 0;   // ja no Safe: nada a desfazer
  if (ativa && teste) { if (!__atomic_exchange_n(&marcada, 1, __ATOMIC_ACQ_REL)) dados_gravar(AP_MARCA, "1\n"); }
  else if (__atomic_exchange_n(&marcada, 0, __ATOMIC_ACQ_REL)) dados_apagar(AP_MARCA);
#else
  (void)ativa;
#endif
}

// --- medidas ------------------------------------------------------------------
static pthread_mutex_t medTrava = PTHREAD_MUTEX_INITIALIZER;
static struct {
  long renders, mudou, quadros, novos, uploads, imagens, reusadas, lotes, linhasNovas, cheios, atrasos;
  long relayBlocos, relayUsados, podados;
  double renderSoma, renderMax, esperaMax, upSoma, upMax, desMax, loteSoma, loteMax;
  double atrasoSoma, atrasoMax, atrasoMin, adianteSoma; long adianteN;
  size_t upBytes;
  int total;
  double ini;
  // a ultima janela fechada, para a folha
  char texto[160];
  int lentos;   // linhas "detalhadas" nesta janela
  float alfaMin, alfaOpc; int alfaN;   // assperf_med_alfa
  // libass por etapa (assperf_med_estagios)
  struct {
    int motor;
    unsigned frames, events, images, hit[3], miss[3];
    unsigned parJobs, parSplit, parSmall, parBusy, parOne;
    unsigned long long parLanes, parLanesSplit;
    double frameMs, frameMax, stageMs[8], piorStage[8];
  } est;
  // blur na GPU
  unsigned gbBlurs, gbFinais, gbReusos, gbFalhas, gbCpu, gbLotes, gbAdiant, gbCriadas, gbReap;
  double gbMs, gbMax;
  int gbMedido;
  unsigned gbN[4]; double gbMsTam[4];
  double gbMsGpu; unsigned gbNGpu;
  unsigned adComBlur, adAdiados, adBe, adClip, adPeq;
  unsigned cpuN[4]; double cpuMs[4];
  AssperfGpuPassos gp;   // passos dos lotes do blur na GPU (soma da janela)
} M;

const char *const assperf_estagio_nome[8] = {
  "other", "shape", "outline", "raster", "blur", "be", "compose", "clip"
};

static int benchRodando;
static char benchResumo[160];
static time_t benchResumoT;   // a folha mostra o resumo por 2 min

void assperf_bench_estado(int rodando, const char *resumo) {
  pthread_mutex_lock(&trava);
  if (resumo) { snprintf(benchResumo, sizeof benchResumo, "%s", resumo); benchResumoT = time(NULL); }
  pthread_mutex_unlock(&trava);
  __atomic_store_n(&benchRodando, rodando, __ATOMIC_RELEASE);
}
int assperf_bench_rodando(void) { return __atomic_load_n(&benchRodando, __ATOMIC_ACQUIRE); }

static double agoraS(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void lento(const char *o, double ms) {
  // Chamado com medTrava. Detalhado: no maximo 8 linhas por janela de 10 s.
  if (assperf_get(AP_LOG) < 2 || M.lentos >= 8) return;
  M.lentos++;
  printf("[assperf] slow %s: %.1f ms\n", o, ms);
}

void assperf_med_render(double ms, double esperaMs, int mudou) {
  pthread_mutex_lock(&medTrava);
  M.renders++; if (mudou) M.mudou++;
  M.renderSoma += ms; if (ms > M.renderMax) M.renderMax = ms;
  if (esperaMs > M.esperaMax) M.esperaMax = esperaMs;
  if (ms > 33.0) lento("libass render", ms);
  if (esperaMs > 16.0) lento("render waited for the track lock", esperaMs);
  pthread_mutex_unlock(&medTrava);
}
void assperf_med_upload(double ms, size_t bytes, int imagens, int reusadas, int quadroNovo) {
  pthread_mutex_lock(&medTrava);
  if (quadroNovo) M.novos++;
  M.uploads++; M.upSoma += ms; if (ms > M.upMax) M.upMax = ms;
  M.upBytes += bytes; M.imagens += imagens; M.reusadas += reusadas;
  if (ms > 16.0) lento("texture upload", ms);
  pthread_mutex_unlock(&medTrava);
}
void assperf_med_quadro(double desenhoMs) {
  pthread_mutex_lock(&medTrava);
  M.quadros++; if (desenhoMs > M.desMax) M.desMax = desenhoMs;
  pthread_mutex_unlock(&medTrava);
}
void assperf_med_lote(double ms, int novos, int total, int cheio) {
  pthread_mutex_lock(&medTrava);
  M.lotes++; M.linhasNovas += novos; M.total = total; if (cheio) M.cheios++;
  M.loteSoma += ms; if (ms > M.loteMax) M.loteMax = ms;
  if (ms > 16.0) lento(cheio ? "full document load" : "batch load", ms);
  pthread_mutex_unlock(&medTrava);
}
void assperf_med_atraso(double ms) {
  pthread_mutex_lock(&medTrava);
  if (!M.atrasos || ms > M.atrasoMax) M.atrasoMax = ms;
  if (!M.atrasos || ms < M.atrasoMin) M.atrasoMin = ms;
  M.atrasos++; M.atrasoSoma += ms;
  pthread_mutex_unlock(&medTrava);
}
void assperf_med_adiante(double ms) {
  pthread_mutex_lock(&medTrava);
  M.adianteSoma += ms; M.adianteN++;
  pthread_mutex_unlock(&medTrava);
}
void assperf_med_relay(int blocos, int usados) {
  pthread_mutex_lock(&medTrava);
  M.relayBlocos += blocos; M.relayUsados += usados;
  pthread_mutex_unlock(&medTrava);
}
void assperf_med_estagios(const AssperfEstagios *e) {
  int i;
  if (!e || !e->frames) return;
  pthread_mutex_lock(&medTrava);
  M.est.motor = e->motor;
  M.est.frames += e->frames; M.est.events += e->events; M.est.images += e->images;
  M.est.parJobs += e->parJobs; M.est.parSplit += e->parSplit; M.est.parSmall += e->parSmall;
  M.est.parBusy += e->parBusy; M.est.parOne += e->parOne;
  M.est.parLanes += e->parLanes; M.est.parLanesSplit += e->parLanesSplit;
  for (i = 0; i < 3; i++) { M.est.hit[i] += e->hit[i]; M.est.miss[i] += e->miss[i]; }
  M.est.frameMs += e->frameMs;
  for (i = 0; i < 8; i++) M.est.stageMs[i] += e->stageMs[i];
  if (e->frames == 1 && e->frameMs > M.est.frameMax) {
    M.est.frameMax = e->frameMs;
    memcpy(M.est.piorStage, e->stageMs, sizeof M.est.piorStage);
  }
  pthread_mutex_unlock(&medTrava);
}

static char gbFormato[24] = "-";
void assperf_gpublur_formato(const char *s) {
  pthread_mutex_lock(&medTrava);
  snprintf(gbFormato, sizeof gbFormato, "%s", s ? s : "-");
  pthread_mutex_unlock(&medTrava);
}
void assperf_med_gpublur(const AssperfGpu *g) {
  int i;
  pthread_mutex_lock(&medTrava);
  M.gbBlurs += g->blurs; M.gbFinais += g->finais; M.gbReusos += g->reusos; M.gbFalhas += g->falhas; M.gbCpu += g->cpu;
  M.gbAdiant += g->adiantados; M.gbCriadas += g->criadas; M.gbReap += g->reaproveitadas;
  if ((g->blurs || g->finais) && g->ms > 0) { M.gbLotes++; M.gbMs += g->ms; if (g->ms > M.gbMax) M.gbMax = g->ms; M.gbMedido = g->medido; }
  for (i = 0; i < 4; i++) { M.gbN[i] += g->nTam[i]; M.gbMsTam[i] += g->msTam[i]; }
  M.gbMsGpu += g->msGpu; M.gbNGpu += g->nGpu;
  pthread_mutex_unlock(&medTrava);
}
void assperf_med_adiado(const AssperfAdiado *a) {
  int i;
  pthread_mutex_lock(&medTrava);
  M.adComBlur += a->comBlur; M.adAdiados += a->adiados; M.adBe += a->be; M.adClip += a->clip; M.adPeq += a->pequenos;
  for (i = 0; i < 4; i++) { M.cpuN[i] += a->cpuN[i]; M.cpuMs[i] += a->cpuMs[i]; }
  pthread_mutex_unlock(&medTrava);
}
void assperf_med_gpupassos(const AssperfGpuPassos *p) {
  pthread_mutex_lock(&medTrava);
  M.gp.lotes += p->lotes; M.gp.lotesTrab += p->lotesTrab; M.gp.envios += p->envios;
  M.gp.passadas += p->passadas; M.gp.finais += p->finais;
  M.gp.salvar += p->salvar; M.gp.envio += p->envio; M.gp.passadasMs += p->passadasMs;
  M.gp.finalMs += p->finalMs; M.gp.restaurar += p->restaurar; M.gp.loteMs += p->loteMs;
  if (p->loteMax > M.gp.loteMax) M.gp.loteMax = p->loteMax;
  M.gp.qEmit += p->qEmit; M.gp.qLidas += p->qLidas; M.gp.qDisj += p->qDisj;
  M.gp.qPend = p->qPend; if (p->qModo) M.gp.qModo = p->qModo;
  pthread_mutex_unlock(&medTrava);
}

/* So o fio GL (assrender_desenhar e o laco principal): sem trava. */
static AssperfSub subQuadro;
void assperf_sub_somar(const AssperfSub *x) {
  subQuadro.total += x->total; subQuadro.upload += x->upload; subQuadro.gpu += x->gpu;
  subQuadro.desenho += x->desenho; subQuadro.adiante += x->adiante; subQuadro.n += x->n;
}
void assperf_sub_tomar(AssperfSub *out) { *out = subQuadro; memset(&subQuadro, 0, sizeof subQuadro); }

long assperf_gpu_min(void) {
  static const long px[4] = { 0, 16384, 65536, 262144 };
  int v = assperf_get(AP_GPUMIN);
  return px[v < 0 || v > 3 ? 0 : v];
}
int assperf_gpu_adiante(void) { return 3; }   // ms medidos no fio GL por quadro de UI (25/10)

void assperf_med_alfa(float alfa, float opacidadeUsuario) {
  pthread_mutex_lock(&medTrava);
  if (!M.alfaN || alfa < M.alfaMin) M.alfaMin = alfa;
  M.alfaOpc = opacidadeUsuario; M.alfaN++;
  pthread_mutex_unlock(&medTrava);
}

void assperf_med_poda(int removidos) {
  pthread_mutex_lock(&medTrava);
  M.podados += removidos;
  pthread_mutex_unlock(&medTrava);
}

static int medForcar;
#ifdef NV_ASSPERF_TESTE
void assperf_med_forcar(void) { medForcar = 1; assperf_med_tick(); }
#endif

int assperf_med_tick(void) {
  double t = agoraS();
  int imprimiu = 0;
  pthread_mutex_lock(&medTrava);
  if (M.ini <= 0) { M.ini = t; pthread_mutex_unlock(&medTrava); return 0; }
  if (t - M.ini < 10.0 && !medForcar) { pthread_mutex_unlock(&medTrava); return 0; }
  medForcar = 0;
  if (M.quadros || M.lotes || M.renders) {
    double rAvg = M.renders ? M.renderSoma / M.renders : 0, uAvg = M.uploads ? M.upSoma / M.uploads : 0;
    double aAvg = M.atrasos ? M.atrasoSoma / M.atrasos : 0, adAvg = M.adianteN ? M.adianteSoma / M.adianteN : 0;
    snprintf(M.texto, sizeof M.texto,
             "render %.1f/%.0f ms \xc2\xb7 upload %.1f/%.0f ms \xc2\xb7 late %+.0f/%+.0f ms \xc2\xb7 ahead %.0f ms",
             rAvg, M.renderMax, uAvg, M.upMax, aAvg, M.atrasoMax, adAvg);
    if (assperf_get(AP_LOG) >= 1) {
      printf("[assperf] 10s: preset=%s frames=%ld new=%ld | render n=%ld changed=%ld avg=%.1f max=%.1f lockwait max=%.1f ms"
             " | upload avg=%.2f max=%.1f ms %.2f MB imgs=%ld reused=%ld | draw max=%.1f ms"
             " | shown late avg=%+.0f min=%+.0f max=%+.0f ms (n=%ld) ahead avg=%.0f ms"
             " | batches=%ld full=%ld lines+=%ld events=%d load avg=%.1f max=%.1f ms"
             " | relay blocks=%ld used=%ld | pruned=%ld\n",
             assperf_preset_nome(), M.quadros, M.novos, M.renders, M.mudou, rAvg, M.renderMax, M.esperaMax,
             uAvg, M.upMax, M.upBytes / 1048576.0, M.imagens, M.reusadas, M.desMax,
             aAvg, M.atrasoMin, M.atrasoMax, M.atrasos, adAvg,
             M.lotes, M.cheios, M.linhasNovas, M.total, M.lotes ? M.loteSoma / M.lotes : 0, M.loteMax,
             M.relayBlocos, M.relayUsados, M.podados);
      imprimiu = 1;
      { int v = assperf_get(AP_ESCALA);
        double s75;
        unsigned trocas;
        int em75;
        pthread_mutex_lock(&autoTrava);
        s75 = A.seg75 + (A.em75 ? t - A.marca : 0.0); trocas = A.trocas; em75 = A.em75;
        A.seg75 = 0; A.trocas = 0; A.marca = t;
        pthread_mutex_unlock(&autoTrava);
        if (v == 5)
          printf("[assperf] render resolution Auto: now %s | at 75%% for %.1f s of the last %.0f s, %u switch(es)\n",
                 em75 ? "75%" : "100%", s75, t - M.ini, trocas); }
      if (M.adComBlur || M.gbLotes || M.gbReusos || M.gbCpu || assperf_get(AP_GPUBLUR))
        printf("[assperf] gpu blur: %s (%s) | new: %u blurs, %u pictures in %u batches, avg %.1f max %.1f ms%s"
               " | reused pictures %u | CPU fallback %u (GPU refused %u)"
               " | libass composites with blur %u: to GPU %u, kept on CPU for \\be %u, vector clip %u\n",
               assperf_get(AP_GPUBLUR) ? "on" : "off", gbFormato, M.gbBlurs, M.gbFinais, M.gbLotes,
               M.gbLotes ? M.gbMs / M.gbLotes : 0.0, M.gbMax,
               M.gbMedido == 2 ? " (submit time; GPU time below)" : M.gbMedido == 1 ? " (GPU time, glFinish)"
                                : " (submit time; Stage timers On measures GPU time)",
               M.gbReusos, M.gbCpu, M.gbFalhas, M.adComBlur, M.adAdiados, M.adBe, M.adClip);
      if (M.adComBlur || M.gbLotes || M.gbAdiant)
        printf("[assperf] gpu blur 2: kept on CPU for size %u (minimum %ld px) | pictures made ahead for queued frames %u"
               " | textures created %u, reused from the reserve %u | GPU time %.0f ms in %u measured jobs (%.2f ms each)\n",
               M.adPeq, assperf_gpu_min(), M.gbAdiant, M.gbCriadas, M.gbReap, M.gbMsGpu, M.gbNGpu,
               M.gbNGpu ? M.gbMsGpu / M.gbNGpu : 0.0);
      if (M.gp.lotesTrab || M.gp.qEmit) {
        double resto = M.gp.loteMs - M.gp.salvar - M.gp.envio - M.gp.passadasMs - M.gp.finalMs - M.gp.restaurar;
        printf("[assperf] gpu blur steps (GL thread time, all batches): %u batches with work (of %u), %.1f ms in all, longest %.1f ms"
               " | state save %.1f ms | source uploads %u, %.1f ms | blur passes %u, %.1f ms | final passes %u, %.1f ms"
               " | state restore %.1f ms | rest %.1f ms\n",
               M.gp.lotesTrab, M.gp.lotes, M.gp.loteMs, M.gp.loteMax, M.gp.salvar, M.gp.envios, M.gp.envio,
               M.gp.passadas, M.gp.passadasMs, M.gp.finais, M.gp.finalMs, M.gp.restaurar, resto < 0 ? 0.0 : resto);
        printf("[assperf] gpu timer: %s | queries issued %u, read %u, dropped as disjoint %u, still pending %u\n",
               M.gp.qModo == 2 ? "sampling with glFinish (1 batch in 16; the queries never gave a result)"
               : M.gp.qModo == 1 ? "GL_EXT_disjoint_timer_query" : "off (Stage timers Off, or no timer extension)",
               M.gp.qEmit, M.gp.qLidas, M.gp.qDisj, M.gp.qPend);
      }
      { int i, alg = 0;
        for (i = 0; i < 4; i++) alg |= M.cpuN[i] || M.gbN[i];
        if (alg) {
          static const char *const nome[4] = { "<16k", "16-64k", "64-256k", ">=256k" };
          char b[400]; size_t o = 0;
          for (i = 0; i < 4; i++)
            o += (size_t)snprintf(b + o, sizeof b - o, "%s %s px: CPU %u x %.2f ms, GPU %u x %.2f ms", i ? " |" : "", nome[i],
                                  M.cpuN[i], M.cpuN[i] ? M.cpuMs[i] / M.cpuN[i] : 0.0,
                                  M.gbN[i], M.gbN[i] ? M.gbMsTam[i] / M.gbN[i] : 0.0);
          printf("[assperf] blur cost by size (per blur; GPU only with Stage timers On):%s\n", b);
        } }
      if (M.alfaN)
        printf("[assperf] draw alpha min=%.2f (Subtitles > Style > Opacity = %.0f%%, your style %s on styled ASS)\n",
               M.alfaMin, M.alfaOpc * 100.0f, assperf_get(AP_ESTILO) ? "ignored" : "applied");
      if (M.est.frames && assperf_get(AP_ESTAGIOS)) {
        char per[200], pior[200];
        size_t o1 = 0, o2 = 0;
        int i;
        double f = (double)M.est.frames;
        per[0] = pior[0] = 0;
        for (i = 1; i <= 8; i++) {   // other por ultimo
          int s = i & 7;
          o1 += (size_t)snprintf(per + o1, sizeof per - o1, "%s%s %.1f", i > 1 ? " " : "", assperf_estagio_nome[s], M.est.stageMs[s] / f);
          o2 += (size_t)snprintf(pior + o2, sizeof pior - o2, "%s%s %.0f", i > 1 ? " " : "", assperf_estagio_nome[s], M.est.piorStage[s]);
          if (o1 >= sizeof per || o2 >= sizeof pior) break;
        }
#define AP_PCT(k) (M.est.hit[k] + M.est.miss[k] ? (int)(100.0 * M.est.hit[k] / (M.est.hit[k] + M.est.miss[k]) + 0.5) : 0)
        printf("[assperf] stages: engine=%s frames=%u avg=%.1f max=%.1f ms | ms/frame: %s | worst frame: %s"
               " | cache hit outline %d%% bitmap %d%% composite %d%% (misses %u/%u/%u) | per frame: events %.1f images %.1f\n",
               M.est.motor == 1 ? "NEON" : M.est.motor == 0 ? "C" : "?", M.est.frames, M.est.frameMs / f, M.est.frameMax,
               per, pior, AP_PCT(0), AP_PCT(1), AP_PCT(2), M.est.miss[0], M.est.miss[1], M.est.miss[2],
               M.est.events / f, M.est.images / f);
        if (M.est.parJobs)
          printf("[assperf] blur split: jobs=%u split=%u (%d%%) one-thread: below minimum %u, pool busy %u, 1 thread %u"
                 " | lanes split %d%%\n",
                 M.est.parJobs, M.est.parSplit,
                 (int)(100.0 * M.est.parSplit / M.est.parJobs + 0.5), M.est.parSmall, M.est.parBusy, M.est.parOne,
                 M.est.parLanes ? (int)(100.0 * (double)M.est.parLanesSplit / (double)M.est.parLanes + 0.5) : 0);
#undef AP_PCT
      }
      fflush(stdout);
    }
  }
  { char guarda[sizeof M.texto]; int total = M.total;
    memcpy(guarda, M.texto, sizeof guarda);
    memset(&M, 0, sizeof M);
    memcpy(M.texto, guarda, sizeof guarda);
    M.total = total; }
  M.ini = t;
  pthread_mutex_unlock(&medTrava);
  return imprimiu;
}

void assperf_med_texto(char *dst, size_t n) {
  pthread_mutex_lock(&trava);
  if (benchResumo[0] && (assperf_bench_rodando() || time(NULL) - benchResumoT < 120)) {
    snprintf(dst, n, "%s", benchResumo);
    pthread_mutex_unlock(&trava);
    return;
  }
  pthread_mutex_unlock(&trava);
  pthread_mutex_lock(&medTrava);
  snprintf(dst, n, "%s", M.texto[0] ? M.texto : "measuring\xe2\x80\xa6 (10 s windows)");
  pthread_mutex_unlock(&medTrava);
}

