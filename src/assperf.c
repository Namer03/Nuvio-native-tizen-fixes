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
  const char *valores[6];
} ApDef;

static const ApDef DEF[AP_N] = {
  [AP_UPLOAD] = { "upload", "Upload format",
    "Alpha 8-bit sends 1 byte per pixel and tints on the GPU; RGBA builds 4 bytes per pixel on the CPU.",
    1, 1, 2, { "RGBA (old)", "Alpha 8-bit" } },
  [AP_TEXCACHE] = { "texcache", "Reuse unchanged images",
    "Keeps a texture per image content: a sign that did not change is not uploaded again.",
    1, 1, 2, { "Off (old)", "On" } },
  [AP_TEXANEL] = { "texanel", "Texture recycle delay",
    "How many frames a texture rests before it is reused, so the GPU never waits on one still in use.",
    1, 1, 3, { "1 frame", "2 frames", "3 frames" } },
  [AP_FORATRAVA] = { "foratrava", "Upload outside lock",
    "Uploads after releasing the lock the libass worker needs, so a slow upload never stalls rendering.",
    1, 1, 2, { "Off (old)", "On" } },
  [AP_INCREMENTAL] = { "incremental", "Batch loading",
    "New lines only adds just the lines each harvested batch brought; the old way re-reads the whole document.",
    1, 1, 2, { "Full reparse (old)", "New lines only" } },
  [AP_PODA] = { "poda", "Drop past events",
    "With new-lines loading, events that ended this long before the playhead leave libass memory.",
    2, 2, 4, { "Off", "30 s", "2 min", "5 min" } },
  [AP_ADIANTE] = { "adiante", "Render ahead",
    "Renders the moment the frame will actually be on screen. Auto measures render + upload time.",
    1, 1, 6, { "Off (old)", "Auto", "50 ms", "100 ms", "150 ms", "250 ms" } },
  [AP_SEGURAR] = { "segurar", "Early frames",
    "With render ahead, a frame that is ready early waits for its moment instead of showing at once.",
    1, 1, 2, { "Show at once", "Hold until due" } },
  [AP_CACHE] = { "cache", "libass bitmap cache",
    "Memory libass keeps for rendered glyphs and blurs. Bigger means fewer re-renders of repeated signs.",
    1, 2, 4, { "32 MB (old)", "64 MB", "128 MB", "16 MB" } },
  [AP_GLIFOS] = { "glifos", "libass glyph cache",
    "How many glyph outlines libass keeps.",
    0, 0, 4, { "Default (old)", "1000", "4000", "10000" } },
  [AP_ESCALA] = { "escala", "Render resolution",
    "Renders subtitles smaller and scales them up. Blur-heavy signs get much cheaper; text gets softer.",
    0, 2, 5, { "100% (old)", "85%", "75%", "66%", "50%" } },
  [AP_HINTING] = { "hinting", "Font hinting",
    "None is the fastest; Light is what the app always used.",
    0, 1, 4, { "Light (old)", "None", "Normal", "Native" } },
  [AP_SHAPER] = { "shaper", "Text shaper",
    "Simple skips HarfBuzz: faster, but Arabic, Thai and other complex scripts can break.",
    0, 0, 2, { "Complex (old)", "Simple" } },
  [AP_FONTES] = { "fontes", "Attached fonts limit",
    "Largest MKV Attachments block that is read. Fansub CJK fonts often pass 16 MB.",
    1, 1, 5, { "16 MB (old)", "32 MB", "48 MB", "64 MB", "96 MB" } },
  [AP_CUES] = { "cues", "App cue copy",
    "The app's own cue list (fallback overlay, sync reference) is rebuilt from the whole document on each batch.",
    1, 1, 2, { "Every batch (old)", "Every 10 s" } },
  [AP_RELAY] = { "relay", "Read subs from local relay",
    "Takes subtitle blocks from the bytes the TV player already downloads: earlier lines, fewer CDN requests.",
    1, 1, 2, { "Off (old)", "On" } },
  [AP_PRIORIDADE] = { "prioridade", "Render thread priority",
    "Higher asks the system for a better slot (may be refused; the log says).",
    0, 0, 3, { "Normal (old)", "Higher", "Lower" } },
  [AP_MUDANCA] = { "mudanca", "Render only on changes",
    "Skips libass when no event starts, ends or animates before the next moment: ~24 renders/s on frame-by-frame signs instead of 60.",
    1, 1, 2, { "Every frame (old)", "On" } },
  [AP_LOG] = { "log", "Subtitle log",
    "Summary writes one [assperf] line every 10 s. Detailed also logs each slow frame.",
    1, 1, 3, { "Off", "Summary (10 s)", "Detailed" } },
};

static int val[AP_N] __attribute__((unused));
static int iniciado;
static unsigned versao;
static pthread_mutex_t trava = PTHREAD_MUTEX_INITIALIZER;

#ifdef NV_TPK
static void aplicarPreset(int p) {
  for (int i = 0; i < AP_N; i++) {
    if (i == AP_LOG) continue;   // o log nao e comportamento: o preset nao mexe
    val[i] = p == 0 ? DEF[i].recomendado : p == 2 ? DEF[i].leve : 0;
  }
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
    marca = dados_ler(AP_MARCA);
    if (marca) {
      free(marca);
      dados_apagar(AP_MARCA);
      if (avisos_sessao_anterior_caiu()) {
        int log = val[AP_LOG];
        aplicarPreset(1);
        val[AP_LOG] = log;
        gravar();
        printf("[assperf] the previous session crashed while ASS subtitles were drawing with test options: "
               "everything is back to Legacy (old behaviour)\n");
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
  assperf_iniciar();
  return __atomic_load_n(&val[id], __ATOMIC_RELAXED);
#else
  (void)id;
  return 0;
#endif
}

int assperf_n_valores(int id) { return id >= 0 && id < AP_N ? DEF[id].n : 0; }
const char *assperf_rotulo(int id) { return id >= 0 && id < AP_N ? DEF[id].rotulo : ""; }
const char *assperf_ajuda(int id) { return id >= 0 && id < AP_N ? DEF[id].ajuda : ""; }
const char *assperf_valor_rotulo(int id, int v) {
  if (id < 0 || id >= AP_N || v < 0 || v >= DEF[id].n) return "";
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
  int rec = 1, leg = 1, lev = 1;
  for (int i = 0; i < AP_N; i++) {
    int v = assperf_get(i);
    if (i == AP_LOG) continue;
    if (v != DEF[i].recomendado) rec = 0;
    if (v != 0) leg = 0;
    if (v != DEF[i].leve) lev = 0;
  }
  return rec ? "Recommended" : leg ? "Legacy" : lev ? "Lightest" : "Custom";
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
double assperf_escala(void) {
  static const double e[5] = { 1.0, 0.85, 0.75, 0.66, 0.5 };
  int v = assperf_get(AP_ESCALA);
  return e[v < 0 || v > 4 ? 0 : v];
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

void assperf_faixa_ativa(int ativa) {
#ifdef NV_TPK
  static int marcada;
  int teste = 0;
  for (int i = 0; i < AP_N; i++) if (i != AP_LOG && assperf_get(i)) teste = 1;
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
} M;

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
void assperf_med_poda(int removidos) {
  pthread_mutex_lock(&medTrava);
  M.podados += removidos;
  pthread_mutex_unlock(&medTrava);
}

static int medForcar;
#ifdef NV_ASSPERF_TESTE
void assperf_med_forcar(void) { medForcar = 1; assperf_med_tick(); }
#endif

void assperf_med_tick(void) {
  double t = agoraS();
  pthread_mutex_lock(&medTrava);
  if (M.ini <= 0) { M.ini = t; pthread_mutex_unlock(&medTrava); return; }
  if (t - M.ini < 10.0 && !medForcar) { pthread_mutex_unlock(&medTrava); return; }
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
}

void assperf_med_texto(char *dst, size_t n) {
  pthread_mutex_lock(&medTrava);
  snprintf(dst, n, "%s", M.texto[0] ? M.texto : "measuring\xe2\x80\xa6 (10 s windows)");
  pthread_mutex_unlock(&medTrava);
}
