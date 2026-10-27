// Ver assperf.h.
#include "assperf.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef NV_TPK
#include "dados.h"
#endif

#define AP_ARQ   "assperf.txt"
#define AP_MARCA "assperf-ativo.txt"   // old crash-guard mark (GPU blur, gone 27/10): deleted at start

/* The .tpk values, decided on the S95C (23-27/10). The blur is libass's own,
 * on the CPU with 3 threads: the GPU blur (24-27/10) was slower and heavier
 * on the S95C in the phone scene, and was removed. The person's options
 * (AP_ESCALA..AP_ASSOPAC) start here and live in assperf.txt. */
static const int TPK[AP_N] = {
  [AP_MOTOR] = 1, [AP_UPLOAD] = 1, [AP_TEXCACHE] = 1, [AP_TEXANEL] = 1, [AP_FORATRAVA] = 1,
  [AP_INCREMENTAL] = 1, [AP_PODA] = 2, [AP_ADIANTE] = 3, [AP_SEGURAR] = 1, [AP_FONTES] = 3,
  [AP_CUES] = 1, [AP_RELAY] = 1, [AP_MUDANCA] = 1, [AP_ESTILO] = 1, [AP_FIOS] = 2,
  [AP_PARQUE] = 1, [AP_LEVE3] = 1,
  [AP_ESCALA] = 5, [AP_GAMA] = 0, [AP_CONTRASTE] = 0, [AP_LIBASS] = 1, [AP_ASSTAM] = 100, [AP_ASSOPAC] = 100,
};
#define AP_PRIMEIRA_DA_PESSOA AP_ESCALA
static const struct { const char *chave; int min, max; } PESSOA[AP_N - AP_PRIMEIRA_DA_PESSOA] = {
  { "escala", 0, 5 }, { "gama", 0, 10 }, { "contraste", 0, 30 }, { "libass", 0, 1 },
  { "asstam", 50, 200 }, { "assopac", 20, 100 },
};

static int val[AP_N] __attribute__((unused));
static int iniciado;
static unsigned versao;
static pthread_mutex_t trava = PTHREAD_MUTEX_INITIALIZER;

static int valido(int id, int v) {
  if (id == AP_ESCALA) return v == 0 || v == 2 || v == 5;
  return v >= PESSOA[id - AP_PRIMEIRA_DA_PESSOA].min && v <= PESSOA[id - AP_PRIMEIRA_DA_PESSOA].max;
}

#ifdef NV_TPK
static void gravar(void) {
  char b[256]; size_t o = 0;
  int i;
  for (i = AP_PRIMEIRA_DA_PESSOA; i < AP_N && o < sizeof b; i++)
    o += (size_t)snprintf(b + o, sizeof b - o, "%s=%d\n", PESSOA[i - AP_PRIMEIRA_DA_PESSOA].chave, val[i]);
  dados_gravar(AP_ARQ, b);
}
#endif

// The file is read ONCE, under the lock; `iniciado` turns 1 (release) only
// with the values in place, so an assperf_get from another thread never reads
// half a file. The first use is always an ASS track or the Style sheet, long
// after avisos_iniciar (the crash guard needs it).
void assperf_iniciar(void) {
  if (__atomic_load_n(&iniciado, __ATOMIC_ACQUIRE)) return;
  pthread_mutex_lock(&trava);
  if (iniciado) { pthread_mutex_unlock(&trava); return; }
#ifdef NV_TPK
  {
    char *t = dados_ler(AP_ARQ);
    int i;
    for (i = AP_PRIMEIRA_DA_PESSOA; i < AP_N; i++) val[i] = TPK[i];
    if (t) {
      char *l = t, *nl;
      for (; l && *l; l = nl) {
        char *eq;
        nl = strchr(l, '\n'); if (nl) *nl++ = 0;
        eq = strchr(l, '=');
        if (!eq) continue;
        *eq = 0;
        for (i = AP_PRIMEIRA_DA_PESSOA; i < AP_N; i++)
          if (!strcmp(l, PESSOA[i - AP_PRIMEIRA_DA_PESSOA].chave) && valido(i, atoi(eq + 1))) val[i] = atoi(eq + 1);
      }
      free(t);
    }
    { char *marca = dados_ler(AP_MARCA); if (marca) { free(marca); dados_apagar(AP_MARCA); } }
    printf("[assperf] subtitles: libass %s, blur CPU x%d, size %d%%, opacity %d%%, render resolution %s, contrast %d, gamma %d\n",
           val[AP_LIBASS] ? "on" : "off", TPK[AP_FIOS] + 1,
           val[AP_ASSTAM], val[AP_ASSOPAC],
           val[AP_ESCALA] == 5 ? "Auto" : val[AP_ESCALA] == 2 ? "75%" : "100%", 30 - val[AP_CONTRASTE],
           val[AP_GAMA] <= 5 ? -val[AP_GAMA] : 11 - val[AP_GAMA]);
    fflush(stdout);
  }
#endif
  __atomic_store_n(&iniciado, 1, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&trava);
}

#ifdef NV_ASSPERF_TESTE
/* Tests on the computer (no NV_TPK): set the options without a file. */
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
  if (id < AP_PRIMEIRA_DA_PESSOA) return TPK[id];
  assperf_iniciar();
  return __atomic_load_n(&val[id], __ATOMIC_RELAXED);
#else
  (void)id;
  return 0;
#endif
}

unsigned assperf_versao(void) { return __atomic_load_n(&versao, __ATOMIC_ACQUIRE); }

void assperf_definir(int id, int v) {
#ifdef NV_TPK
  if (id < AP_PRIMEIRA_DA_PESSOA || id >= AP_N || !valido(id, v)) return;
  assperf_iniciar();
  pthread_mutex_lock(&trava);
  __atomic_store_n(&val[id], v, __ATOMIC_RELAXED);
  gravar();
  pthread_mutex_unlock(&trava);
  __atomic_add_fetch(&versao, 1, __ATOMIC_ACQ_REL);
  printf("[assperf] %s = %d\n", PESSOA[id - AP_PRIMEIRA_DA_PESSOA].chave, v);
  fflush(stdout);
#else
  (void)id; (void)v;
#endif
}

void assperf_restaurar(void) {
#ifdef NV_TPK
  int i;
  assperf_iniciar();
  pthread_mutex_lock(&trava);
  for (i = AP_PRIMEIRA_DA_PESSOA; i < AP_N; i++) if (i != AP_LIBASS) __atomic_store_n(&val[i], TPK[i], __ATOMIC_RELAXED);
  gravar();
  pthread_mutex_unlock(&trava);
  __atomic_add_fetch(&versao, 1, __ATOMIC_ACQ_REL);
  printf("[assperf] subtitle style back to the defaults\n");
  fflush(stdout);
#endif
}

int assperf_fios(void) { int v = assperf_get(AP_FIOS); return v < 0 ? 1 : v > 3 ? 4 : v + 1; }

size_t assperf_fontes_teto(void) {
  static const size_t mb[5] = { 16, 32, 48, 64, 96 };
  int v = assperf_get(AP_FONTES);
  return mb[v < 0 || v > 4 ? 0 : v] * 1024u * 1024u;
}

int assperf_adiante_fixo_ms(void) {
  static const int ms[6] = { 0, -1, 50, 100, 150, 250 };
  int v = assperf_get(AP_ADIANTE);
  return ms[v < 0 || v > 5 ? 0 : v];
}

long assperf_poda_ms(void) {
  static const long ms[4] = { 0, 30000, 120000, 300000 };
  int v = assperf_get(AP_PODA);
  return ms[v < 0 || v > 3 ? 0 : v];
}

/* The TV's gamma slider (-5..+5) picks a relative curve in the GAMMATBLOSD_384
 * LUT of libave.so: out = in^(1 - 0.0426 x slider), measured on the 11 curves
 * (avoc_gamma_contrast_picture.md, 24/10). "-" = higher gamma (darker mids).
 * Only the video goes through it; the subtitles get the same sum. Black and
 * white do not move. */
double assperf_gama(void) {
  int v = assperf_get(AP_GAMA);
  int n = v <= 0 || v > 10 ? 0 : v <= 5 ? -v : 11 - v;
  return 1.0 - 0.0426 * n;
}
/* The TV's Contrast is a GAIN per RGB channel before the gamma curve
 * (PontusMVideoEnhancer::m_CalculateContrast in libave.so: 944 x (c + 70) /
 * 120; avoc_contrast_application.md, 24/10). Relative to 30 (where subtitle
 * white matches video white on the S95C): (c + 70) / 100. */
double assperf_contraste(void) {
  int v = assperf_get(AP_CONTRASTE);
  int c = 30 - (v < 0 || v > 30 ? 0 : v);
  return (c + 70) / 100.0;
}

static double agoraS(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- Busy scenes ----------------------------------------------------------
 * Picture sync (legimagem.c), render resolution Auto and the frame-by-frame
 * worker (assrender.c) follow this flag. Owner 26/10, log 12: the cost of ONE
 * new frame made the fingers scene (19:00) busy on every 40 ms frame and let
 * the phone scene (22:12) fall back to normal halfway. So it is the subtitle
 * WORK PER SECOND (libass on the worker + drawing on the GL thread) over a 2 s
 * window. Logs 9 and 12: fingers 2-66 ms/s, phone 330-624 ms/s.
 *   - busy above CG_LIGA in the window, or when the forecast says so;
 *   - normal only after CG_LEVE_S seconds of playing below CG_DESLIGA with a
 *     simple screen and no busy forecast (a pause does not count);
 *   - CG_OCIOSO_S with nothing new (the scene is over): normal. */
#define CG_BALDES   20      /* 100 ms each: 2 s */
#define CG_LIGA     300.0   /* ms of work per second */
#define CG_DESLIGA  150.0
#define CG_LEVE_S   3.0
#define CG_OCIOSO_S 8.0
/* Forecast (the next second of the track, assrender.c): busy with this many
 * lines starting or this much text. Normal only with few pictures on screen. */
#define CG_PREV_INICIOS 300
#define CG_PREV_CHARS   400000L
#define CG_TELA_SIMPLES 20
static pthread_mutex_t autoTrava = PTHREAD_MUTEX_INITIALIZER;
static struct { double ms[CG_BALDES]; long long id[CG_BALDES]; double ult, leveAcum, leveDesde; int on, prevPesada, prevCedo, imagens, inicios; long chars; } CG;

static double carga_locked(double t) {
  long long b = (long long)floor(t * 10.0);
  double soma = 0;
  int i;
  for (i = 0; i < CG_BALDES; i++) if (CG.id[i] <= b && b - CG.id[i] < CG_BALDES) soma += CG.ms[i];
  return soma / (CG_BALDES / 10.0);
}
static void carga_somar_locked(double t, double ms) {
  long long b = (long long)floor(t * 10.0);
  int i = (int)(b % CG_BALDES);
  double c, dt = CG.ult > 0 ? t - CG.ult : 0;
  if (CG.id[i] != b) { CG.id[i] = b; CG.ms[i] = 0; }
  CG.ms[i] += ms > 0 ? ms : 0;
  c = carga_locked(t);
  if (!CG.on) {
    if (c > CG_LIGA || CG.prevPesada) {
      CG.on = 1; CG.leveAcum = 0; CG.leveDesde = 0;
      if (CG.prevPesada && c <= CG_LIGA)
        printf("[assperf] subtitles busy: on (coming second: %d lines starting, %ldk characters)\n", CG.inicios, CG.chars / 1000);
      else printf("[assperf] subtitles busy: on (%.0f ms of subtitle work per second)\n", c);
      fflush(stdout);
    }
  } else if (c < CG_DESLIGA && !CG.prevPesada && CG.imagens < CG_TELA_SIMPLES) {
    CG.leveAcum += dt < 0 ? 0 : dt > 0.25 ? 0.25 : dt;   /* only while playing: a pause sends no tick (assrender) */
    if (!CG.leveDesde) CG.leveDesde = t;
    if (CG.leveAcum >= CG_LEVE_S) {
      CG.on = 0;
      printf("[assperf] subtitles busy: off (normal for %.0f s, %.0f ms per second)\n", CG.leveAcum, c);
      fflush(stdout);
    }
  } else { CG.leveAcum = 0; CG.leveDesde = 0; }
  CG.ult = t;
}
static int pesado_locked(double t) {
  if (CG.on && CG.ult > 0 && t - CG.ult > CG_OCIOSO_S) {
    CG.on = 0;
    printf("[assperf] subtitles busy: off (nothing new for %.0f s)\n", CG_OCIOSO_S);
    fflush(stdout);
  }
  return CG.on;
}
void assperf_previsao(int inicios, long chars, int imagens, int iniciosCedo, long charsCedo) {
  pthread_mutex_lock(&autoTrava);
  CG.inicios = inicios; CG.chars = chars; CG.imagens = imagens;
  CG.prevPesada = inicios >= CG_PREV_INICIOS || chars >= CG_PREV_CHARS;
  /* The same thresholds, 0.5 s further on: only Auto looks at it, so the
   * busy flag (picture sync) still switches with the scene itself. */
  CG.prevCedo = iniciosCedo >= CG_PREV_INICIOS || charsCedo >= CG_PREV_CHARS;
  carga_somar_locked(agoraS(), 0.0);   /* now: on before the first busy frame */
  pthread_mutex_unlock(&autoTrava);
}
void assperf_carga_gl(double ms) {
  pthread_mutex_lock(&autoTrava);
  carga_somar_locked(agoraS(), ms);
  pthread_mutex_unlock(&autoTrava);
}
void assperf_escala_custo(double libassMs) {
  pthread_mutex_lock(&autoTrava);
  carga_somar_locked(agoraS(), libassMs);
  pthread_mutex_unlock(&autoTrava);
}
int assperf_pesado(void) {
  int r;
  pthread_mutex_lock(&autoTrava);
  r = pesado_locked(agoraS());
  pthread_mutex_unlock(&autoTrava);
  return r;
}

/* ---- Render resolution Auto -----------------------------------------------
 * 100 % normally; 75 % while the subtitles are busy (owner 25/10: "only as a
 * fallback when subtitles get heavy"). At least 5 s at 75 % and 2 s at 100 %
 * between switches: each one redraws everything at the new size. */
#define AUTO_MIN75_S  5.0
#define AUTO_MIN100_S 2.0
static struct { int em75; double desde; } A;

static void auto_trocar_locked(int em75, double t, int antes) {
  if (em75) printf("[assperf] render resolution Auto: 75 %% (%s)\n", antes ? "busy scene in 0.5 s" : "subtitles busy");
  else printf("[assperf] render resolution Auto: back to 100 %% (%.0f s at 75 %%)\n", t - A.desde);
  fflush(stdout);
  A.em75 = em75; A.desde = t;
}

double assperf_escala_quadro(void) {
  static const double e[5] = { 1.0, 0.85, 0.75, 0.66, 0.5 };
  int v = assperf_get(AP_ESCALA);
  double t, r;
  if (v != 5) {
    pthread_mutex_lock(&autoTrava);
    A.em75 = 0;
    pthread_mutex_unlock(&autoTrava);
    return e[v < 0 || v > 4 ? 0 : v];
  }
  t = agoraS();
  pthread_mutex_lock(&autoTrava);
  { int pesado = pesado_locked(t) || CG.prevCedo;
    if (!A.em75 && pesado && t - A.desde >= AUTO_MIN100_S) auto_trocar_locked(1, t, !CG.on);
    else if (A.em75 && !pesado && t - A.desde >= AUTO_MIN75_S) auto_trocar_locked(0, t, 0); }
  r = A.em75 ? 0.75 : 1.0;
  pthread_mutex_unlock(&autoTrava);
  return r;
}

void assperf_escala_reset(void) {
  pthread_mutex_lock(&autoTrava);
  A.em75 = 0; A.desde = agoraS();
  memset(&CG, 0, sizeof CG);   /* new track: the load starts from zero */
  pthread_mutex_unlock(&autoTrava);
}

double assperf_escala(void) {
  static const double e[5] = { 1.0, 0.85, 0.75, 0.66, 0.5 };
  int v = assperf_get(AP_ESCALA), em75;
  if (v != 5) return e[v < 0 || v > 4 ? 0 : v];
  pthread_mutex_lock(&autoTrava); em75 = A.em75; pthread_mutex_unlock(&autoTrava);
  return em75 ? 0.75 : 1.0;
}

int assperf_legendas_pesadas(void) {
  int r;
  pthread_mutex_lock(&autoTrava);
  r = A.em75 || pesado_locked(agoraS());
  pthread_mutex_unlock(&autoTrava);
  return r;
}
