// Ver logexport.h.
#include "logexport.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define OCULTO "[oculto]"
#define OCULTO_N 8

static int ehTok(int c) { return isalnum((unsigned char)c) || c == '_' || c == '-'; }

// URL: "://host" + "/…" no lugar do resto (ate espaco/aspas/parenteses).
static size_t passoUrl(char *s, size_t n) {
  size_t r = 0, w = 0;
  while (r < n) {
    if (s[r] == ':' && r + 2 < n && s[r + 1] == '/' && s[r + 2] == '/' && r > 0 && isalpha((unsigned char)s[r - 1])) {
      size_t h = r + 3, fim;
      memmove(s + w, s + r, 3); w += 3; r += 3;
      while (h < n && !strchr("/? \t\r\n\"'<>)", s[h])) h++;
      memmove(s + w, s + r, h - r); w += h - r; r = h;
      if (r < n && (s[r] == '/' || s[r] == '?')) {
        fim = r;
        while (fim < n && !strchr(" \t\r\n\"'<>)", s[fim])) fim++;
        if (fim - r >= 4) { memcpy(s + w, "/\xe2\x80\xa6", 4); w += 4; r = fim; }
      }
      continue;
    }
    s[w++] = s[r++];
  }
  s[w] = 0;
  return w;
}

static const char *const CHAVES[] = {
  "api_key", "apikey", "api-key", "access_token", "token", "password", "passwd",
  "senha", "secret", "authorization", "bearer", NULL };

static int valorFim(int c) { return c == 0 || strchr(" \t\r\n&\"',;)<>", c) != NULL; }

// "chave=valor", "chave: valor", "chave":"valor", "Bearer valor".
static size_t passoChave(char *s, size_t n) {
  size_t r = 0, w = 0;
  while (r < n) {
    int k, feito = 0;
    if (r == 0 || !ehTok(s[r - 1])) {
      for (k = 0; CHAVES[k] && !feito; k++) {
        size_t m = strlen(CHAVES[k]), p;
        int bearer = !strcmp(CHAVES[k], "bearer");
        if (r + m >= n || strncasecmp(s + r, CHAVES[k], m)) continue;
        p = r + m;
        if (s[p] == '"' || s[p] == '\'') p++;
        if (bearer) { if (s[p] != ' ') continue; }
        else if (s[p] != '=' && s[p] != ':') continue;
        p++;
        while (p < n && (s[p] == ' ' || s[p] == '"' || s[p] == '\'')) p++;
        { size_t v = p;
          while (v < n && !valorFim((unsigned char)s[v])) v++;
          if (v - p >= OCULTO_N) {
            memmove(s + w, s + r, p - r); w += p - r;
            memcpy(s + w, OCULTO, OCULTO_N); w += OCULTO_N;
            r = v; feito = 1;
          } }
      }
      if (feito) continue;
    }
    s[w++] = s[r++];
  }
  s[w] = 0;
  return w;
}

// Sequencia opaca longa (32+) com letra e digito.
static size_t passoOpaco(char *s, size_t n) {
  size_t r = 0, w = 0;
  while (r < n) {
    if (ehTok((unsigned char)s[r]) && (r == 0 || !ehTok((unsigned char)s[r - 1]))) {
      size_t f = r;
      int letra = 0, dig = 0;
      while (f < n && ehTok((unsigned char)s[f])) {
        if (isdigit((unsigned char)s[f])) dig = 1; else if (isalpha((unsigned char)s[f])) letra = 1;
        f++;
      }
      if (f - r >= 32 && letra && dig) { memcpy(s + w, OCULTO, OCULTO_N); w += OCULTO_N; }
      else { memmove(s + w, s + r, f - r); w += f - r; }
      r = f;
      continue;
    }
    s[w++] = s[r++];
  }
  s[w] = 0;
  return w;
}

// "fulano@dominio.com" -> "…@dominio.com" (parte local com 3+ bytes).
static size_t passoEmail(char *s, size_t n) {
  size_t r = 0, w = 0;
  while (r < n) {
    if (s[r] == '@' && w >= 3) {
      size_t ini = w, d = r + 1;
      while (ini > 0 && (isalnum((unsigned char)s[ini - 1]) || strchr("._%+-", s[ini - 1]))) ini--;
      if (w - ini >= 3 && d < n && isalnum((unsigned char)s[d])) {
        size_t f = d;
        int ponto = 0;
        while (f < n && (isalnum((unsigned char)s[f]) || s[f] == '.' || s[f] == '-')) { if (s[f] == '.') ponto = 1; f++; }
        if (ponto) {
          memcpy(s + ini, "\xe2\x80\xa6", 3); w = ini + 3;
          s[w++] = '@'; r++;
          continue;
        }
      }
    }
    s[w++] = s[r++];
  }
  s[w] = 0;
  return w;
}

size_t logexport_limpar(char *s, size_t n) {
  if (!s) return 0;
  s[n] = 0;
  n = passoUrl(s, n);
  n = passoChave(s, n);
  n = passoOpaco(s, n);
  n = passoEmail(s, n);
  return n;
}

// Ultimos `max` bytes de um arquivo, sem comecar no meio de linha/UTF-8.
static char *cauda(const char *caminho, size_t max, size_t *n) {
  FILE *f = caminho && *caminho ? fopen(caminho, "rb") : NULL;
  char *t;
  long tam;
  size_t lidos, ini = 0;
  *n = 0;
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) || (tam = ftell(f)) <= 0) { fclose(f); return NULL; }
  if ((size_t)tam > max) fseek(f, tam - (long)max, SEEK_SET); else rewind(f);
  t = malloc(max + 1);
  if (!t) { fclose(f); return NULL; }
  lidos = fread(t, 1, max, f);
  fclose(f);
  t[lidos] = 0;
  if ((size_t)tam > max) {
    char *q = memchr(t, '\n', lidos);
    ini = q ? (size_t)(q - t) + 1 : 0;
    while (ini < lidos && ((unsigned char)t[ini] & 0xC0) == 0x80) ini++;
    memmove(t, t + ini, lidos - ini + 1);
    lidos -= ini;
  }
  *n = lidos;
  return t;
}

char *logexport_montar(const char *atual, const char *anterior, const char *etapas,
                       const char *versao, const char *plataforma, size_t *n) {
  const char *nome[3] = { "tpk-etapas.txt", "nuvio-anterior.log", "nuvio.log" };
  const char *cam[3] = { etapas, anterior, atual };
  size_t max[3] = { LOGEXP_ETAPAS_MAX, LOGEXP_ANTERIOR_MAX, LOGEXP_ATUAL_MAX };
  char *parte[3], *saida;
  size_t tam[3], total = 512, w = 0;
  int i, algum = 0;
  for (i = 0; i < 3; i++) {
    parte[i] = cauda(cam[i], max[i], &tam[i]);
    if (parte[i]) { tam[i] = logexport_limpar(parte[i], tam[i]); algum = 1; total += tam[i] + 96; }
  }
  *n = 0;
  if (!algum) return NULL;
  saida = malloc(total);
  if (!saida) { for (i = 0; i < 3; i++) free(parte[i]); return NULL; }
  w = (size_t)snprintf(saida, 256, "Nuvio - registro exportado\nversao: %s\nplataforma: %s\n"
                       "(URLs, chaves e e-mails foram encurtados/ocultados)\n",
                       versao ? versao : "?", plataforma ? plataforma : "?");
  for (i = 0; i < 3; i++) {
    if (!parte[i]) continue;
    w += (size_t)snprintf(saida + w, 96, "\n===== %s (%zu bytes) =====\n", nome[i], tam[i]);
    memcpy(saida + w, parte[i], tam[i]); w += tam[i];
    if (tam[i] && saida[w - 1] != '\n') saida[w++] = '\n';
    free(parte[i]);
  }
  saida[w] = 0;
  *n = w;
  return saida;
}
