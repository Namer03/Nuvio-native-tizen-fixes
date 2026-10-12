// "EXPORTAR REGISTRO PARA O CELULAR" (celular.h): servidor de uso unico que
// entrega um arquivo, falado por curl de verdade. Texto de teste, nao e log real.
#include "celular.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *i18n(const char *s) { return s; }
int ajustes_idioma(void) { return 0; }
void ajustes_acento(float *r, float *g, float *b) { *r = 0.96f; *g = 0.55f; *b = 0.2f; }
int ajustes_tinta_foco(void) { return 18; }

static char saida[16384];
static int curl(const char *args) {
  char cmd[2048];
  FILE *f;
  size_t n;
  int cod = 0;
  snprintf(cmd, sizeof cmd, "curl -s -m 5 -o /tmp/nv-celexp-corpo.txt -w '%%{http_code}' %s", args);
  f = popen(cmd, "r");
  if (!f) return -1;
  if (fscanf(f, "%d", &cod) != 1) cod = 0;
  pclose(f);
  f = fopen("/tmp/nv-celexp-corpo.txt", "r");
  saida[0] = 0;
  if (f) { n = fread(saida, 1, sizeof saida - 1, f); saida[n] = 0; fclose(f); }
  return cod;
}

int main(void) {
  char a[512], url[160], arq[200], tok[16];
  char grande[9000];
  const char *u, *barra;
  int p;
  memset(grande, 'x', sizeof grande - 1); grande[sizeof grande - 1] = 0;
  memcpy(grande, "inicio\n", 7); memcpy(grande + sizeof grande - 8, "\nfinal\n", 7);

  assert(!celular_exportar_abrir("Exportar registro", "", 0));      // nada a exportar
  assert(celular_exportar_abrir("Exportar registro", grande, strlen(grande)));
  u = celular_url(); p = celular_porta();
  assert(!strncmp(u, "http://", 7) && p > 0);
  barra = strrchr(u, '/'); snprintf(tok, sizeof tok, "%s", barra + 1);
  snprintf(url, sizeof url, "http://127.0.0.1:%d/%s", p, tok);
  snprintf(arq, sizeof arq, "%s/" CEL_EXPORT_NOME, url);

  // pagina com o botao; POST nao existe neste modo; token errado = 404; Host de fora = 403
  snprintf(a, sizeof a, "'%s'", u);
  assert(curl(a) == 200 && strstr(saida, CEL_EXPORT_NOME) && strstr(saida, "download") && !strstr(saida, "<textarea"));
  snprintf(a, sizeof a, "-X POST -d t=oi '%s'", url); assert(curl(a) == 405);
  snprintf(a, sizeof a, "'http://127.0.0.1:%d/aaaaaaaa/" CEL_EXPORT_NOME "'", p); assert(curl(a) == 404);
  snprintf(a, sizeof a, "-H 'Host: evil.test' '%s'", arq); assert(curl(a) == 403);
  snprintf(a, sizeof a, "-X DELETE '%s'", arq); assert(curl(a) == 405);
  snprintf(a, sizeof a, "-I '%s'", arq); assert(curl(a) == 200);     // HEAD nao consome
  assert(celular_estado() == CEL_ESPERANDO);

  // o download: o arquivo inteiro, uma vez so
  snprintf(a, sizeof a, "-D /tmp/nv-celexp-cab.txt '%s'", arq);
  assert(curl(a) == 200);
  assert(strlen(saida) == strlen(grande) && !strcmp(saida, grande));
  { char cab[1024] = ""; FILE *f = fopen("/tmp/nv-celexp-cab.txt", "r"); size_t n = f ? fread(cab, 1, sizeof cab - 1, f) : 0; cab[n] = 0; if (f) fclose(f);
    assert(strstr(cab, "attachment; filename=\"" CEL_EXPORT_NOME "\"") && strstr(cab, "text/plain")); }
  usleep(200000);
  assert(celular_estado() == CEL_RECEBIDO);
  snprintf(a, sizeof a, "'%s'", arq); assert(curl(a) == 0);          // servidor fechou
  celular_fechar();
  assert(celular_estado() == CEL_PARADO);

  // fechar antes de baixar: tambem fecha, e o texto nao fica
  assert(celular_exportar_abrir("x", "abc\n", 4));
  p = celular_porta(); celular_fechar();
  snprintf(a, sizeof a, "'http://127.0.0.1:%d/'", p); assert(curl(a) == 0);

  // o modo "digitar" continua igual depois de um export
  assert(celular_abrir("Chave"));
  u = celular_url(); snprintf(a, sizeof a, "'%s'", u);
  assert(curl(a) == 200 && strstr(saida, "<textarea"));
  celular_fechar();
  puts("celular_exportar: ok");
  return 0;
}
