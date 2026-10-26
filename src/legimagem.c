// Ver legimagem.h.
#include "legimagem.h"

#if defined(NV_TPK)
#include "video.h"
#include "assperf.h"
#include "dados.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LI_ARQ "legimagem.txt"
#define LI_PADRAO 190   /* owner 25/10: 170-210 ms on the S95C depending on the scene */
#define LI_MAX 300
#define LI_TIPOS 16
/* Cena pesada (assperf_pesado): o seu proprio valor, chave + "p" (dono 26/10:
 * 140-150 ms na cena pesada, 190-210 na leve). Sem valor ainda: leve - 50. */
#define LI_PESADA_MENOS 50

static struct { char chave[12]; int ms; } tipos[LI_TIPOS];
static int nTipos, lido;

static void ler(void) {
  char *t, *l, *nl;
  if (lido) return;
  lido = 1;
  t = dados_ler(LI_ARQ);
  if (!t) return;
  for (l = t; l && *l && nTipos < LI_TIPOS; l = nl) {
    char *eq;
    nl = strchr(l, '\n'); if (nl) *nl++ = 0;
    eq = strchr(l, '=');
    if (!eq || eq == l || eq - l >= (long)sizeof tipos[0].chave) continue;
    *eq = 0;
    snprintf(tipos[nTipos].chave, sizeof tipos[nTipos].chave, "%s", l);
    tipos[nTipos].ms = atoi(eq + 1);
    if (tipos[nTipos].ms < 0) tipos[nTipos].ms = 0;
    if (tipos[nTipos].ms > LI_MAX) tipos[nTipos].ms = LI_MAX;
    nTipos++;
  }
  free(t);
}

static void gravar(void) {
  char b[512]; size_t o = 0;
  int i;
  for (i = 0; i < nTipos && o < sizeof b; i++)
    o += (size_t)snprintf(b + o, sizeof b - o, "%s=%d\n", tipos[i].chave, tipos[i].ms);
  dados_gravar(LI_ARQ, b);
}

/* 23.976 and 24 behave the same; so do 29.97/30 and 59.94/60. */
static int fpsArredondado(double fps) {
  static const int nomes[] = { 24, 25, 30, 48, 50, 60, 120 };
  int i, melhor = 0;
  if (fps <= 1.0) return 0;
  for (i = 1; i < (int)(sizeof nomes / sizeof nomes[0]); i++)
    if (fabs(fps - nomes[i]) < fabs(fps - nomes[melhor])) melhor = i;
  return nomes[melhor];
}

static void chaveAtual(char *k, size_t n, int *fpsOut, int *hdrOut) {
  double fps; int hdr, f;
  video_tpk_quadro(&fps, &hdr);
  f = fpsArredondado(fps);
  if (fpsOut) *fpsOut = f;
  if (hdrOut) *hdrOut = hdr;
  if (f) snprintf(k, n, "%d%s", f, hdr ? "h" : "");
  else snprintf(k, n, "x%s", hdr ? "h" : "");
}
static void chavePesada(char *k, size_t n) {
  char b[12];
  chaveAtual(b, sizeof b, NULL, NULL);
  snprintf(k, n, "%sp", b);
}

static int achar(const char *k) {
  int i;
  for (i = 0; i < nTipos; i++) if (!strcmp(tipos[i].chave, k)) return i;
  return -1;
}

int legimagem_disponivel(void) { return 1; }

static int valor_leve(void) {
  char k[12]; int i;
  chaveAtual(k, sizeof k, NULL, NULL);
  i = achar(k);
  return i >= 0 ? tipos[i].ms : LI_PADRAO;
}
static int valor_pesado(void) {
  char k[12]; int i, v;
  chavePesada(k, sizeof k);
  i = achar(k);
  if (i >= 0) return tipos[i].ms;
  v = valor_leve() - LI_PESADA_MENOS;
  return v < 0 ? 0 : v;
}
int legimagem_ms(void) {
  ler();
  return assperf_pesado() ? valor_pesado() : valor_leve();
}
void legimagem_valores(int *leve, int *pesada, int *agoraPesada) {
  ler();
  *leve = valor_leve(); *pesada = valor_pesado(); *agoraPesada = assperf_pesado();
}

void legimagem_ajustar(int deltaMs, int pesada) {
  char k[12], tp[24]; int i, antes;
  ler();
  if (pesada < 0) pesada = assperf_pesado();   // -1: o valor da cena que esta na tela
  if (pesada) chavePesada(k, sizeof k); else chaveAtual(k, sizeof k, NULL, NULL);
  i = achar(k);
  if (i < 0) {
    int base = pesada ? valor_pesado() : LI_PADRAO;
    if (nTipos >= LI_TIPOS) i = nTipos - 1;
    else i = nTipos++;
    snprintf(tipos[i].chave, sizeof tipos[i].chave, "%s", k);
    tipos[i].ms = base;
  }
  antes = tipos[i].ms;
  tipos[i].ms += deltaMs;
  if (tipos[i].ms < 0) tipos[i].ms = 0;
  if (tipos[i].ms > LI_MAX) tipos[i].ms = LI_MAX;
  gravar();
  legimagem_tipo(tp, sizeof tp);
  printf("[relogio] subtitle correction for %s%s: %d ms (was %d ms)\n", tp, pesada ? ", heavy scenes" : "", tipos[i].ms, antes);
  fflush(stdout);
}

void legimagem_tipo(char *dst, size_t n) {
  int f, hdr; char k[12];
  chaveAtual(k, sizeof k, &f, &hdr);
  if (f) snprintf(dst, n, "%d fps%s", f, hdr ? " HDR" : "");
  else snprintf(dst, n, "video%s", hdr ? " HDR" : "");
}

double legimagem_fps(void) {
  double fps; int hdr;
  return video_tpk_quadro(&fps, &hdr) ? fps : 0.0;
}

#else
int  legimagem_disponivel(void) { return 0; }
int  legimagem_ms(void) { return 0; }
void legimagem_valores(int *leve, int *pesada, int *agoraPesada) { *leve = *pesada = 0; *agoraPesada = 0; }
void legimagem_ajustar(int deltaMs, int pesada) { (void)deltaMs; (void)pesada; }
void legimagem_tipo(char *dst, size_t n) { if (n) dst[0] = 0; }
double legimagem_fps(void) { return 0.0; }
#endif
