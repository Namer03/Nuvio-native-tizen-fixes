#include "../src/logexport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int falhas;
#define CHECK(c) do { if (!(c)) { printf("FALHOU %d: %s\n", __LINE__, #c); falhas++; } } while (0)
static char *lim(const char *in) {
  static char b[4096];
  snprintf(b, sizeof b, "%s", in);
  logexport_limpar(b, strlen(b));
  return b;
}
int main(int argc, char **argv) {
  char *r;
  r = lim("GET https://debrid.example/dl/ABCDEF1234567890/file.mkv?apikey=zzzzzzzz ok");
  CHECK(strstr(r, "https://debrid.example/\xe2\x80\xa6 ok") && !strstr(r, "ABCDEF") && !strstr(r, "zzzz"));
  r = lim("http://192.168.0.5:8080/ curto"); CHECK(!strcmp(r, "http://192.168.0.5:8080/ curto"));
  r = lim("token=abcdefghijkl&x=1"); CHECK(!strcmp(r, "token=[oculto]&x=1"));
  r = lim("{\"password\":\"hunter2hunter2\"}"); CHECK(strstr(r, "[oculto]") && !strstr(r, "hunter"));
  r = lim("Authorization: Bearer eyJhbGciOiJIUzI1NiJ9abc"); CHECK(!strstr(r, "eyJ"));
  r = lim("token=1234"); CHECK(!strcmp(r, "token=1234"));
  r = lim("chave a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2 fim");
  CHECK(!strcmp(r, "chave [oculto] fim"));
  r = lim("palavralongaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa fica"); CHECK(strstr(r, "palavralonga"));
  r = lim("conta joao.silva@gmail.com ok"); CHECK(!strstr(r, "joao") && strstr(r, "@gmail.com"));
  r = lim("ab@c.d x@y.z"); CHECK(!strcmp(r, "ab@c.d x@y.z"));
  r = lim("sem nada secreto aqui: 12345 keycode=1073741906"); CHECK(!strcmp(r, "sem nada secreto aqui: 12345 keycode=1073741906"));
  if (argc > 2) {   // montar a partir de arquivos: atual, anterior
    size_t n; char *t = logexport_montar(argv[1], argv[2], NULL, "t1", "teste", &n);
    CHECK(t && n == strlen(t));
    CHECK(t && strstr(t, "nuvio.log") && strstr(t, "nuvio-anterior.log") && !strstr(t, "SEGREDO"));
    CHECK(t && strstr(t, "linha-final"));
    CHECK(t && n < 400000);
    free(t);
    t = logexport_montar("/nao/existe", NULL, NULL, "t1", "teste", &n); CHECK(!t && n == 0);
  }
  puts(falhas ? "logexport: FALHOU" : "logexport: ok");
  return falhas != 0;
}
