// Silero VAD em C contra o modelo oficial (ONNX): mesma probabilidade por bloco
// de 32 ms, e o audvad no modo Silero achando a fala de uma fixture espeak-ng.
//   bash tests/silero.sh
#include "audvad.h"
#include "silero.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int falhas;
#define CHECA(c, ...) do { if (!(c)) { falhas++; printf("FALHOU %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void *ler(const char *p, long *n) {
  FILE *f = fopen(p, "rb");
  void *b;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
  b = malloc((size_t)*n);
  if (b && fread(b, 1, (size_t)*n, f) != (size_t)*n) { free(b); b = NULL; }
  fclose(f);
  return b;
}

int main(int argc, char **argv) {
  SileroEstado s;
  float x[SILERO_BLOCO], p;
  int i, k;
  CHECA(silero_disponivel(), "pesos nao carregaram (n=%u)", silero_pesos_n);
  if (!silero_disponivel()) return 1;

  // Silencio e ruido branco fraco: nada de fala.
  silero_zerar(&s);
  for (i = 0; i < SILERO_BLOCO; i++) x[i] = 0;
  for (k = 0; k < 20; k++) { p = silero_prob(&s, x); CHECA(p >= 0 && p < 0.1f, "silencio p=%.3f", p); }
  srand(3);
  for (k = 0; k < 40; k++) {
    for (i = 0; i < SILERO_BLOCO; i++) x[i] = 0.02f * ((float)rand() / RAND_MAX * 2 - 1);
    p = silero_prob(&s, x);
    if (k > 4) CHECA(p < 0.5f, "ruido p=%.3f bloco %d", p, k);
  }

  if (argc == 3) {
    long npcm, nprob;
    int16_t *pcm = ler(argv[1], &npcm);
    float *ref = ler(argv[2], &nprob), maxd = 0;
    int nb, discord = 0, falaRef = 0;
    double somaD = 0;
    AudVad *v = calloc(1, sizeof *v);
    CHECA(pcm && ref && v, "fixture");
    if (!pcm || !ref || !v) return 1;
    nb = (int)(nprob / 4);
    silero_zerar(&s);
    for (k = 0; k < nb; k++) {
      float d;
      for (i = 0; i < SILERO_BLOCO; i++) x[i] = pcm[k * SILERO_BLOCO + i] / 32768.0f;
      p = silero_prob(&s, x);
      d = fabsf(p - ref[k]);
      somaD += d;
      if (d > maxd) maxd = d;
      if ((p >= 0.5f) != (ref[k] >= 0.5f)) discord++;
      falaRef += ref[k] >= 0.5f;
    }
    printf("silero paridade: %d blocos, %d com fala, dif max %.4f media %.5f, discordam %d\n",
           nb, falaRef, maxd, somaD / nb, discord);
    CHECA(maxd < 0.05f, "dif max %.4f", maxd);
    CHECA(somaD / nb < 0.005, "dif media %.5f", somaD / nb);
    CHECA(discord <= nb / 100, "discordam %d", discord);

    // audvad no modo Silero: tres falas (com ruido no meio), acorde e ruido sem fala.
    audvad_iniciar(v);
    CHECA(v->modo == AUDVAD_SILERO, "modo %s", audvad_nome(v));
    audvad_pcm(v, pcm, (int)(npcm / 2), 0);
    audvad_fechar(v);
    {
      double fala = 0, ultimaFim = 0;
      for (i = 0; i < v->nseg; i++) {
        printf("  fala %.2f-%.2f\n", v->seg[i].inicio, v->seg[i].fim);
        fala += v->seg[i].fim - v->seg[i].inicio;
        CHECA(v->seg[i].inicio >= ultimaFim, "ordem");
        ultimaFim = v->seg[i].fim;
      }
      CHECA(v->nseg >= 3 && v->nseg <= 12, "segmentos %d", v->nseg);
      CHECA(fabs(fala - falaRef * 0.032) < 2.0, "fala %.1f s vs ref %.1f s", fala, falaRef * 0.032);
      printf("audvad silero: %d segmentos, %.1f s de fala, prob media %.3f, %.0f s analisados\n",
             v->nseg, fala, v->somaProb / v->quadros, audvad_segundos(v));
    }
    // Forcar energia continua funcionando.
    audvad_iniciar_modo(v, AUDVAD_ENERGIA);
    CHECA(v->modo == AUDVAD_ENERGIA, "forcar energia");
    free(v); free(pcm); free(ref);
  } else printf("(sem fixture: so os casos sinteticos)\n");

  if (falhas) { printf("%d falha(s)\n", falhas); return 1; }
  printf("silero ok\n");
  return 0;
}
