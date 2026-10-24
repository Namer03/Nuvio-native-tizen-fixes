// Provas do "TV luminance limit (test)" (src/tpkluz.c) contra uma libavoc.so
// falsa (tests/tpkluz_avoc.c). Ver tests/tpkluz.sh. Um modo por processo:
//   normal  : muda so em tela cheia, devolve ao sair dela / na Off / na pausa
//             (a pausa espera a devolucao), volta na retomada, "ja igual"
//   cai     : muda e morre sem devolver (a nota fica em data/)
//   arranque: o arranque seguinte devolve o original e apaga a nota
//   semlib  : TV sem libavoc: nada muda, nenhuma nota
//   erroget : o get falha: nada muda, nenhuma nota
#include "../src/tpkluz.h"
#include "../src/assperf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int chaveTeste;
int assperf_get(int id) { return id == AP_LUZ ? chaveTeste : 0; }

static const char *dados;
static int falhas;

static int db(void) {
  FILE *f = fopen(getenv("FAKE_AVOC_DB"), "r");
  int v = -1;
  if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
  return v;
}
static void dbPor(int v) { FILE *f = fopen(getenv("FAKE_AVOC_DB"), "w"); fprintf(f, "%d\n", v); fclose(f); }
static int temNota(void) {
  char c[600];
  snprintf(c, sizeof c, "%s/luz-original.txt", dados);
  return access(c, F_OK) == 0;
}
static void dorme(int ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
// Espera o fio levar a TV a `v` (ate 2 s).
static int espera(int v) {
  for (int i = 0; i < 200; i++) { if (db() == v) return 1; dorme(10); }
  return 0;
}
#define CONFERE(c, msg) do { if (!(c)) { printf("FALHOU: %s (db=%d nota=%d)\n", msg, db(), temNota()); falhas++; } } while (0)

static void quadro(int chave, int tela) { chaveTeste = chave; tpkluz_quadro(tela); }

int main(int argc, char **argv) {
  const char *modo = argc > 1 ? argv[1] : "normal";
  dados = getenv("TPKLUZ_DADOS");
  if (!strcmp(modo, "arranque")) {
    tpkluz_iniciar(dados);
    CONFERE(espera(1), "arranque: o original (1) volta");
    dorme(50);
    CONFERE(!temNota(), "arranque: a nota sai");
  } else {
    if (strcmp(modo, "cai")) dbPor(1);
    tpkluz_iniciar(dados);
    dorme(50);
    if (!strcmp(modo, "semlib") || !strcmp(modo, "erroget")) {
      quadro(1, 1); dorme(200);
      CONFERE(db() == 1 && !temNota(), "sem suporte: nada muda");
      tpkluz_sistema(0);
    } else if (!strcmp(modo, "cai")) {
      dbPor(1);
      quadro(1, 1);
      CONFERE(espera(0) && temNota(), "cai: mudou e anotou");
      fflush(stdout);
      _exit(falhas ? 1 : 0);   // sem devolver: como uma queda
    } else {
      quadro(1, 0); dorme(100);
      CONFERE(db() == 1 && !temNota(), "chave ligada fora da tela cheia: nada muda");
      quadro(1, 1);
      CONFERE(espera(0) && temNota(), "tela cheia + Set 0: a TV vai a 0 e a nota existe");
      quadro(1, 0);
      CONFERE(espera(1), "saiu da tela cheia: volta o 1");
      dorme(30);
      CONFERE(!temNota(), "devolvido: a nota sai");
      quadro(1, 1);
      CONFERE(espera(0), "de novo em tela cheia: 0");
      tpkluz_sistema(0);
      CONFERE(db() == 1 && !temNota(), "pausa: o 1 ja voltou quando tpkluz_sistema(0) retorna");
      quadro(1, 1); dorme(100);
      CONFERE(db() == 1, "pausado: nao muda mesmo em tela cheia");
      tpkluz_sistema(1);
      CONFERE(espera(0), "retomada: volta a valer");
      quadro(0, 1);
      CONFERE(espera(1), "chave Off: volta o 1");
      quadro(2, 1); dorme(100);
      CONFERE(db() == 1 && !temNota(), "Set 1 com a TV em 1: ja igual, nada muda");
      quadro(1, 1);
      CONFERE(espera(0), "Set 2 -> Set 0: troca direto");
      dbPor(1);   // a pessoa mexeu no menu da TV
      quadro(0, 1); dorme(150);
      CONFERE(db() == 1 && !temNota(), "mudado fora do app: fica como esta, nota sai");
      tpkluz_sistema(0);
    }
  }
  printf("tpkluz %s: %s\n", modo, falhas ? "FALHOU" : "ok");
  return falhas ? 1 : 0;
}
