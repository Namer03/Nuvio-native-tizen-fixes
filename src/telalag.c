// Ver telalag.h.
#include "telalag.h"

#if defined(NV_TPK)
#include "assperf.h"
#include "gl_compat.h"
#include "tpk_egl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* OpenGL ES 3.0 sync objects, by hand: the link only declares ES 2. */
#define TL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define TL_SYNC_FLUSH_COMMANDS_BIT    0x00000001
#define TL_ALREADY_SIGNALED           0x911A
#define TL_CONDITION_SATISFIED        0x911C
#define TL_WAIT_FAILED                0x911D
typedef void *TlSync;
typedef TlSync (*FnFence)(unsigned, unsigned);
typedef unsigned (*FnWait)(TlSync, unsigned, unsigned long long);
typedef void (*FnDel)(TlSync);
static FnFence pFence;
static FnWait pWait;
static FnDel pDel;
static int estado;   /* 0 not tried, 1 ready, -1 no ES 3 / no functions */

#define TL_FILA 24   /* frames in flight; at 60 fps that is 400 ms of GPU backlog */
static struct { TlSync s; double t0; } fila[TL_FILA];
static int nFila;
static double t0Quadro;

/* lag (ms per frame, frame start -> GPU done) */
static double ema, base, segSoma, segInicio, ultUso;
static int segN, temBase, aplicado;
/* 10 s window */
static double jLagSoma, jLagMax, jEspera, jExtraMax;
static int jN, jEsperaN, jExtraN;
static double jExtraSoma;

static double agoraMs(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int pronto(void) {
  if (estado) return estado > 0;
  estado = -1;
  {
    const char *v = (const char *)glGetString(GL_VERSION);
    if (!v || !strstr(v, "OpenGL ES ") || v[10] < '3') {
      printf("[relogio] screen lag: not measured (needs OpenGL ES 3, this context is %s)\n", v ? v : "?");
      fflush(stdout);
      return 0;
    }
  }
  if (!tpkEgl.GetProcAddress) return 0;
  *(void **)&pFence = tpkEgl.GetProcAddress("glFenceSync");
  *(void **)&pWait = tpkEgl.GetProcAddress("glClientWaitSync");
  *(void **)&pDel = tpkEgl.GetProcAddress("glDeleteSync");
  if (!pFence || !pWait || !pDel) {
    printf("[relogio] screen lag: not measured (no glFenceSync)\n");
    fflush(stdout);
    return 0;
  }
  estado = 1;
  return 1;
}

static void amostra(double lag, double t) {
  if (t - ultUso > 2000.0) {   /* not timing subtitles: start over when they come back */
    temBase = 0; ema = 0; segN = 0; segSoma = 0; segInicio = t; aplicado = 0;
    return;
  }
  ema = ema > 0 ? ema * 0.85 + lag * 0.15 : lag;
  jLagSoma += lag; jN++;
  if (lag > jLagMax) jLagMax = lag;
  /* Baseline = the lightest 1 s average since subtitles started. It creeps up
   * 0.2 ms per second, so one odd light second is forgotten over minutes, and
   * a minute of heavy signs moves it only ~12 ms. */
  segSoma += lag; segN++;
  if (t - segInicio >= 1000.0) {
    double m = segSoma / segN;
    if (!temBase || m < base) { base = m; temBase = 1; }
    else base += 0.2;
    segSoma = 0; segN = 0; segInicio = t;
  }
}

static void colher(void) {
  int i = 0;
  double t;
  while (i < nFila) {
    unsigned r = pWait(fila[i].s, 0, 0);
    if (r != TL_ALREADY_SIGNALED && r != TL_CONDITION_SATISFIED && r != TL_WAIT_FAILED) break;
    t = agoraMs();
    if (r != TL_WAIT_FAILED) amostra(t - fila[i].t0, t);
    pDel(fila[i].s);
    i++;
  }
  if (i) { memmove(fila, fila + i, (size_t)(nFila - i) * sizeof fila[0]); nFila -= i; }
}

void telalag_quadro_inicio(void) {
  int lim;
  if (!pronto()) return;
  colher();
  lim = assperf_get(AP_FILA);   /* 1 or 2 frames behind at most */
  if (lim > 0 && nFila >= lim) {
    double t = agoraMs();
    /* wait for the frame that leaves only lim-1 still running (100 ms cap) */
    unsigned r = pWait(fila[nFila - lim].s, TL_SYNC_FLUSH_COMMANDS_BIT, 100000000ull);
    (void)r;
    jEspera += agoraMs() - t; jEsperaN++;
    colher();
  }
  t0Quadro = agoraMs();
}

void telalag_quadro_fim(void) {
  TlSync s;
  if (estado <= 0) return;
  if (nFila == TL_FILA) { pDel(fila[0].s); memmove(fila, fila + 1, (TL_FILA - 1) * sizeof fila[0]); nFila--; }
  s = pFence(TL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  if (!s) return;
  fila[nFila].s = s; fila[nFila].t0 = t0Quadro; nFila++;
}

int telalag_extra_ms(void) {
  double t = agoraMs(), x;
  int alvo;
  ultUso = t;
  if (estado <= 0 || !temBase || !assperf_get(AP_LAGCOMP)) { aplicado = 0; return 0; }
  x = ema - base;
  if (x < 0) x = 0;
  if (x > 250) x = 250;
  alvo = (int)(x / 5.0 + 0.5) * 5;
  /* 10 ms of hysteresis: the frame grid would otherwise flip a sign between
   * two frames on every small wobble */
  if (abs(alvo - aplicado) >= 10 || (alvo == 0 && aplicado < 10)) aplicado = alvo;
  jExtraSoma += aplicado; jExtraN++;
  if (aplicado > jExtraMax) jExtraMax = aplicado;
  return aplicado;
}

int telalag_atual_ms(void) { return aplicado; }

void telalag_relatorio(char *dst, size_t n) {
  if (!n) return;
  dst[0] = 0;
  if (estado <= 0 || !jN) { jEspera = 0; jEsperaN = 0; return; }
  {
    size_t o = (size_t)snprintf(dst, n,
      " | screen lag avg %.0f max %.0f ms (light baseline %.0f) -> correction lowered by %.0f ms on average, %.0f at most%s",
      jLagSoma / jN, jLagMax, temBase ? base : 0.0, jExtraN ? jExtraSoma / jExtraN : 0.0, jExtraMax,
      assperf_get(AP_LAGCOMP) ? "" : " (compensation Off)");
    if (o < n && assperf_get(AP_FILA))
      snprintf(dst + o, n - o, " | GPU queue limit %d: waited %.0f ms in %d frames", assperf_get(AP_FILA), jEspera, jEsperaN);
  }
  jLagSoma = jLagMax = jEspera = jExtraMax = jExtraSoma = 0;
  jN = jEsperaN = jExtraN = 0;
}

#else
void telalag_quadro_inicio(void) {}
void telalag_quadro_fim(void) {}
int  telalag_extra_ms(void) { return 0; }
int  telalag_atual_ms(void) { return 0; }
void telalag_relatorio(char *dst, size_t n) { if (n) dst[0] = 0; }
#endif
