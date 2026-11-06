// Ver silero.h. Matematica pura: sem SDL, sem malloc por chamada.
#include "silero.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define N_FFT   256
#define HOP     128
#define QUADROS 4
#define BINS    129
#define ENTRADA (SILERO_CTX + SILERO_BLOCO + 64)   // 640 after the reflect pad

// Layout of silero_pesos_f16 (tools/silero/converter.py, ORDEM).
#define N_C1W (128 * BINS * 3)
#define N_C2W (64 * 128 * 3)
#define N_C3W (64 * 64 * 3)
#define N_C4W (128 * 64 * 3)
#define N_LSTM (512 * 128)
#define N_TOTAL (N_C1W + 128 + N_C2W + 64 + N_C3W + 64 + N_C4W + 128 + 2 * N_LSTM + 512 + 512 + 128 + 1)

static struct {
  const float *c1w, *c1b, *c2w, *c2b, *c3w, *c3b, *c4w, *c4b, *wih, *whh, *fw;
  float b[512];   // bias_ih + bias_hh
  float fb;
  float cosT[N_FFT / 2], sinT[N_FFT / 2], hann[N_FFT];
  int ok;
} W;
static pthread_once_t uma = PTHREAD_ONCE_INIT;

static float meia(unsigned short h) {   // IEEE float16 -> float32
  unsigned s = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
  float v;
  if (e == 0) v = ldexpf((float)m, -24);
  else if (e == 31) v = m ? NAN : INFINITY;
  else v = ldexpf((float)(m | 1024u), (int)e - 25);
  return s ? -v : v;
}

static void carregar(void) {
  float *p, *q;
  unsigned i;
  if (silero_pesos_n != N_TOTAL) return;
  p = malloc(sizeof *p * N_TOTAL);
  if (!p) return;
  for (i = 0; i < N_TOTAL; i++) p[i] = meia(silero_pesos_f16[i]);
  q = p;
  W.c1w = q; q += N_C1W; W.c1b = q; q += 128;
  W.c2w = q; q += N_C2W; W.c2b = q; q += 64;
  W.c3w = q; q += N_C3W; W.c3b = q; q += 64;
  W.c4w = q; q += N_C4W; W.c4b = q; q += 128;
  W.wih = q; q += N_LSTM; W.whh = q; q += N_LSTM;
  for (i = 0; i < 512; i++) W.b[i] = q[i] + q[512 + i];
  q += 1024;
  W.fw = q; q += 128;
  W.fb = *q;
  for (i = 0; i < N_FFT / 2; i++) {
    W.cosT[i] = (float)cos(2.0 * M_PI * i / N_FFT);
    W.sinT[i] = (float)-sin(2.0 * M_PI * i / N_FFT);
  }
  for (i = 0; i < N_FFT; i++) W.hann[i] = (float)(0.5 - 0.5 * cos(2.0 * M_PI * i / N_FFT));   // periodic
  W.ok = 1;   // p lives for the whole process (one model, ~1 MB)
}

int silero_disponivel(void) {
  pthread_once(&uma, carregar);
  return W.ok;
}

void silero_zerar(SileroEstado *s) { memset(s, 0, sizeof *s); }

static void fft(float *re, float *im) {
  int i, j = 0, k, len;
  for (i = 1; i < N_FFT; i++) {
    int bit = N_FFT >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
  }
  for (len = 2; len <= N_FFT; len <<= 1) {
    int passo = N_FFT / len, m = len / 2;
    for (i = 0; i < N_FFT; i += len)
      for (k = 0; k < m; k++) {
        float wr = W.cosT[k * passo], wi = W.sinT[k * passo];
        float xr = re[i + k + m] * wr - im[i + k + m] * wi, xi = re[i + k + m] * wi + im[i + k + m] * wr;
        re[i + k + m] = re[i + k] - xr; im[i + k + m] = im[i + k] - xi;
        re[i + k] += xr; im[i + k] += xi;
      }
  }
}

// Contiguous dot product with 8 partial sums (vectorizes without -ffast-math).
static float dot(const float *a, const float *b, int n) {
  float s[8] = { 0 }, r;
  int i, k;
  for (i = 0; i + 8 <= n; i += 8)
    for (k = 0; k < 8; k++) s[k] += a[i + k] * b[i + k];
  r = ((s[0] + s[1]) + (s[2] + s[3])) + ((s[4] + s[5]) + (s[6] + s[7]));
  for (; i < n; i++) r += a[i] * b[i];
  return r;
}

// x[ci][L] -> y[co][Lo], kernel 3, padding 1, then ReLU. The input window of
// each output step is laid out like a weight row ([ci][3]), so every output is
// one dot product.
static int conv(const float *x, int ci, int L, const float *w, const float *b, int co, int passo, float *y) {
  float col[BINS * 3];   // widest input: the 129 STFT bins
  int Lo = (L + 2 - 3) / passo + 1, o, t, i, k;
  for (t = 0; t < Lo; t++) {
    for (i = 0; i < ci; i++)
      for (k = 0; k < 3; k++) {
        int p = t * passo + k - 1;
        col[i * 3 + k] = p >= 0 && p < L ? x[(size_t)i * L + p] : 0.0f;
      }
    for (o = 0; o < co; o++) {
      float a = b[o] + dot(w + (size_t)o * ci * 3, col, ci * 3);
      y[(size_t)o * Lo + t] = a > 0 ? a : 0;
    }
  }
  return Lo;
}

static float sig(float v) { return 1.0f / (1.0f + expf(-v)); }

float silero_prob(SileroEstado *s, const float *x) {
  float in[ENTRADA], mag[BINS * QUADROS], y1[128 * QUADROS], y2[64 * 2], y3[64], y4[128], g[512];
  float re[N_FFT], im[N_FFT], a;
  int i, f, L;
  if (!silero_disponivel()) return -1.0f;
  memcpy(in, s->ctx, sizeof s->ctx);
  memcpy(in + SILERO_CTX, x, sizeof *x * SILERO_BLOCO);
  for (i = 0; i < 64; i++) in[SILERO_CTX + SILERO_BLOCO + i] = in[SILERO_CTX + SILERO_BLOCO - 2 - i];
  memcpy(s->ctx, x + SILERO_BLOCO - SILERO_CTX, sizeof s->ctx);
  for (f = 0; f < QUADROS; f++) {
    for (i = 0; i < N_FFT; i++) { re[i] = in[f * HOP + i] * W.hann[i]; im[i] = 0; }
    fft(re, im);
    for (i = 0; i < BINS; i++) mag[i * QUADROS + f] = sqrtf(re[i] * re[i] + im[i] * im[i]);
  }
  L = conv(mag, BINS, QUADROS, W.c1w, W.c1b, 128, 1, y1);
  L = conv(y1, 128, L, W.c2w, W.c2b, 64, 2, y2);
  L = conv(y2, 64, L, W.c3w, W.c3b, 64, 2, y3);
  conv(y3, 64, L, W.c4w, W.c4b, 128, 1, y4);   // L == 1: y4 is the encoder output
  for (i = 0; i < 512; i++)
    g[i] = W.b[i] + dot(W.wih + (size_t)i * 128, y4, 128) + dot(W.whh + (size_t)i * 128, s->h, 128);
  a = W.fb;
  for (i = 0; i < 128; i++) {
    float c = sig(g[128 + i]) * s->c[i] + sig(g[i]) * tanhf(g[256 + i]);
    float h = sig(g[384 + i]) * tanhf(c);
    s->c[i] = c; s->h[i] = h;
    if (h > 0) a += h * W.fw[i];
  }
  return sig(a);
}
