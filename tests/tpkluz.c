// Provas do "TV luminance limit (test)" (src/tpkluz.c) contra uma libavoc.so
// falsa (tests/tpkluz_avoc.c). Ver tests/tpkluz.sh. Um modo por processo:
//   normal  : muda so em tela cheia, devolve ao sair dela / na Off / na pausa
//             (a pausa espera a devolucao), volta na retomada, "ja igual"
//   cai     : muda e morre sem devolver (a nota fica em data/)
//   arranque: o arranque seguinte devolve o original e apaga a nota
//   semlib  : TV sem libavoc: nada muda, nenhuma nota
//   erroget : o get falha: nada muda, nenhuma nota
//   cinza   : o "video gray" (sem slot) com as mesmas regras, junto do limite
//   caicinza / arranquecinza: queda e recuperacao do video gray
//   gama    : Auto le o gama da TV so em tela cheia; nota de guarda some
#include "../src/tpkluz.h"
#include "../src/assperf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int chaveTeste, chaveCinza, chaveGama;
int assperf_get(int id) { return id == AP_LUZ ? chaveTeste : id == AP_CINZA ? chaveCinza : id == AP_GAMA ? chaveGama : 0; }

static const char *dados;
static int falhas;

static int db(void) {
  FILE *f = fopen(getenv("FAKE_AVOC_DB"), "r");
  int v = -1;
  if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
  return v;
}
static void dbPor(int v) { FILE *f = fopen(getenv("FAKE_AVOC_DB"), "w"); fprintf(f, "%d\n", v); fclose(f); }
static int cinza(void) {
  FILE *f = fopen(getenv("FAKE_AVOC_GRAY_DB"), "r");
  int v = -1;
  if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
  return v;
}
static void cinzaPor(int v) { FILE *f = fopen(getenv("FAKE_AVOC_GRAY_DB"), "w"); fprintf(f, "%d\n", v); fclose(f); }
static int temNotaCinza(void) {
  char c[600];
  snprintf(c, sizeof c, "%s/cinza-original.txt", dados);
  return access(c, F_OK) == 0;
}
static int esperaCinza(int v) {
  for (int i = 0; i < 200; i++) { if (cinza() == v) return 1; usleep(10000); }
  return 0;
}
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
static void quadro2(int luz, int cz, int tela) { chaveTeste = luz; chaveCinza = cz; tpkluz_quadro(tela); }

int main(int argc, char **argv) {
  const char *modo = argc > 1 ? argv[1] : "normal";
  dados = getenv("TPKLUZ_DADOS");
  if (!strcmp(modo, "gama")) {
    char c[600];
    tpkluz_iniciar(dados);
    usleep(50000);
    chaveGama = AP_GAMA_AUTO; tpkluz_quadro(0); usleep(100000);
    CONFERE(tpkluz_gama_tv() == TPKLUZ_GAMA_DESCONHECIDO, "gama: fora da tela cheia nao le");
    tpkluz_quadro(1);
    for (int i = 0; i < 200 && tpkluz_gama_tv() != -1; i++) usleep(10000);
    CONFERE(tpkluz_gama_tv() == -1, "gama: Auto le -1 da TV");
    snprintf(c, sizeof c, "%s/gama-auto-testando.txt", dados);
    CONFERE(access(c, F_OK) != 0, "gama: a nota de guarda sai depois da primeira leitura");
    tpkluz_sistema(0);
  } else if (!strcmp(modo, "arranquecinza")) {
    tpkluz_iniciar(dados);
    CONFERE(esperaCinza(0), "arranque: o video gray original (0) volta");
    usleep(50000);
    CONFERE(!temNotaCinza(), "arranque: a nota do video gray sai");
  } else if (!strcmp(modo, "caicinza")) {
    cinzaPor(0);
    tpkluz_iniciar(dados);
    usleep(50000);
    quadro2(0, 2, 1);
    CONFERE(esperaCinza(1) && temNotaCinza(), "caicinza: mudou e anotou");
    fflush(stdout);
    _exit(falhas ? 1 : 0);
  } else if (!strcmp(modo, "cinza")) {
    dbPor(1); cinzaPor(0);
    tpkluz_iniciar(dados);
    usleep(50000);
    quadro2(0, 2, 1);
    CONFERE(esperaCinza(1) && temNotaCinza(), "video gray Set 1: a TV vai a 1 e a nota existe");
    CONFERE(db() == 1 && !temNota(), "o limite fica como esta");
    quadro2(1, 2, 1);
    CONFERE(espera(0) && cinza() == 1, "os dois juntos");
    tpkluz_sistema(0);
    CONFERE(db() == 1 && cinza() == 0 && !temNota() && !temNotaCinza(), "pausa: os dois voltam antes de retornar");
    tpkluz_sistema(1);
    CONFERE(espera(0) && esperaCinza(1), "retomada: os dois voltam a valer");
    quadro2(1, 0, 1);
    CONFERE(esperaCinza(0) && db() == 0, "video gray Off: so ele volta");
    quadro2(1, 0, 0);
    CONFERE(espera(1) && cinza() == 0, "saiu da tela cheia: o limite volta");
    quadro2(0, 1, 1); usleep(100000);
    CONFERE(cinza() == 0 && !temNotaCinza(), "video gray Set 0 com a TV em 0: nada muda");
    tpkluz_sistema(0);
  } else if (!strcmp(modo, "arranque")) {
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
