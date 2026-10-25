// Ver telalag.h.
#include "telalag.h"

#if defined(NV_TPK)
#include "assperf.h"
#include "gl_compat.h"
#include "tpk_egl.h"
#include <math.h>
#include <stdio.h>
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

/* 10 s window: lag (ms per frame, frame start -> GPU done) and the queue limit */
static double jLagSoma, jLagMax, jEspera;
static int jN, jEsperaN, jLimN, jQuadros;

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

static void colher(void) {
  int i = 0;
  double t;
  while (i < nFila) {
    unsigned r = pWait(fila[i].s, 0, 0);
    if (r != TL_ALREADY_SIGNALED && r != TL_CONDITION_SATISFIED && r != TL_WAIT_FAILED) break;
    t = agoraMs();
    if (r != TL_WAIT_FAILED) {
      double lag = t - fila[i].t0;
      jLagSoma += lag; jN++;
      if (lag > jLagMax) jLagMax = lag;
    }
    pDel(fila[i].s);
    i++;
  }
  if (i) { memmove(fila, fila + i, (size_t)(nFila - i) * sizeof fila[0]); nFila -= i; }
}

/* GPU QUEUE LIMIT, by situation (owner 25/10, logs 5 and 7): 1 frame helped
 * only while Render resolution Auto was at 75 % on heavy signs (lag peaks
 * 47-56 ms against 67-117 without); at a fixed 100 % on heavy signs it made
 * them worse (169-217 ms against ~118): there the GL thread itself is the
 * bottleneck and the wait only adds a stall. Light scenes: no difference. */
static int limite(void) {
  return assperf_get(AP_ESCALA) == 5 && assperf_escala() < 1.0 ? 1 : 0;
}

void telalag_quadro_inicio(void) {
  int lim;
  if (!pronto()) return;
  colher();
  jQuadros++;
  lim = limite();
  if (lim) jLimN++;
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

void telalag_relatorio(char *dst, size_t n) {
  if (!n) return;
  dst[0] = 0;
  if (estado > 0 && jN) {
    size_t o = (size_t)snprintf(dst, n, " | screen lag avg %.0f max %.0f ms", jLagSoma / jN, jLagMax);
    if (o < n)
      snprintf(dst + o, n - o, " | GPU queue limit (auto, at 75 %%): on in %d of %d frames, waited %.0f ms",
               jLimN, jQuadros, jEspera);
  }
  jLagSoma = jLagMax = jEspera = 0;
  jN = jEsperaN = jLimN = jQuadros = 0;
}

#else
void telalag_quadro_inicio(void) {}
void telalag_quadro_fim(void) {}
void telalag_relatorio(char *dst, size_t n) { if (n) dst[0] = 0; }
#endif
