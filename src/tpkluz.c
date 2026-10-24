// AJUSTES DE IMAGEM DA TV (.tpk, dono 24/10/2026): as linhas
// "TV luminance limit (test)" e "TV video gray (test)" da folha ASS performance.
//
// O branco do libass (255 lido de volta do framebuffer) aparece ~5 % mais
// cinza que o branco do video na S95C; o preto esta certo. A suspeita e a
// protecao do painel OLED limitando a camada grafica. O teste: desligar o
// "luminance limit" da TV durante a reproducao e comparar o branco do Colour
// test card com o do video. Resultado na S95C (24/10): o limite ja esta em 0
// (desligado) e o Set 1 nao pega (le 0 de volta); o menu da propria TV tambem
// fica cinza. Segundo teste: o "video gray" do avocd, que tem leitura.
//
// FONTE: so o que o dono extraiu do firmware (avoc_findings_full.md, 24/10):
//   int avoc_get_luminance_limit(gint *out_value, gint type);   // libavoc.so
//   int avoc_set_luminance_limit(gint limit, gint type);
//   limit: 0 = limite desligado, 1 = ligado (o avocd recusa fora de 0..1)
//   type:  1 = AVOC_SAVE (o valor em uso, gravado no banco do avocd)
//          2 = AVOC_SAVE_DEFAULT (o de fabrica; aqui nunca e escrito)
//   int avoc_get_video_gray_onoff(gint *out_value);   // 0/1 (resposta (valor, 0))
//   int avoc_set_video_gray_onoff(gint onoff);        // 0/1, chave 0xfc no banco
// O retorno 0 = sucesso NAO esta no documento (e a convencao das APIs Tizen):
// qualquer outro retorno conta como falha e a TV nao e mexida. Nada de
// ppi_ve_* (camada abaixo do avocd, sem leitura do valor) nem de
// avoc_set_osd_information (so escrita: nao ha como ler o original).
//
// E UM AJUSTE DA TV INTEIRA, gravado no banco do avocd, e protege o painel.
// Por isso:
//  - so vale com o player em tela cheia COM video (tpkluz_quadro);
//  - le o valor antes, e devolve ele quando a chave volta a Off, a reproducao
//    para / sai da tela cheia, ou o app pausa ou fecha (tpkluz_sistema(0),
//    que espera a devolucao antes de o host seguir);
//  - antes de mudar grava <ajuste>-original.txt em data/ (com fsync); se o app cair
//    com o valor mudado, o proximo arranque devolve o original
//    (tpkluz_iniciar). Sem conseguir gravar a nota, nao muda nada;
//  - se, na hora de devolver, a TV ja tem outro valor que nao o nosso (a
//    pessoa mexeu no menu da TV), deixa como esta;
//  - TV sem libavoc.so ou sem as funcoes: loga uma vez e nao faz nada;
//  - a chave nunca volta ligada do arquivo (assperf.c a zera no arranque).
// Toda chamada a libavoc sai de UM fio (o daqui): o D-Bus pode demorar, e o
// fio GL e o do app nunca esperam por ele.
#ifdef NV_TPK
#include "tpkluz.h"
#include "assperf.h"
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LUZ_AVOC_SAVE 1
#define LUZ_ESPERA_MS 3000   // tpkluz_sistema(0): teto da espera pela devolucao

typedef struct {
  const char *nome;          // no log: "[assperf] <nome>: ..."
  const char *fnLer, *fnPor; // em libavoc.so
  int comTipo;               // 1 = (valor, AVOC_SAVE); 0 = so o valor
  int ap;                    // linha da folha (AP_*): 0 Off, 1 = por 0, 2 = por 1
  const char *arqMarca;      // nota de recuperacao em data/
  // so o fio
  void *ler, *por;
  int lib;                   // 0 = nao tentou, 1 = ok, -1 = sem suporte
  int segura, orig, posto;   // segura = a TV esta com o NOSSO valor
  char marca[600];
  // sob tr
  int chave;                 // pedido da folha
  int feito;                 // valor pedido que o fio ja tratou (-1 = o da TV)
} Ajuste;

static Ajuste AJ[] = {
  { .nome = "luminance limit", .fnLer = "avoc_get_luminance_limit", .fnPor = "avoc_set_luminance_limit",
    .comTipo = 1, .ap = AP_LUZ, .arqMarca = "luz-original.txt" },
  { .nome = "video gray", .fnLer = "avoc_get_video_gray_onoff", .fnPor = "avoc_set_video_gray_onoff",
    .comTipo = 0, .ap = AP_CINZA, .arqMarca = "cinza-original.txt" },
};
#define NAJ ((int)(sizeof AJ / sizeof AJ[0]))
_Static_assert(sizeof AJ / sizeof AJ[0] <= 8, "tpkluz_quadro guarda ate 8 ajustes");

static pthread_mutex_t tr = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int iniciado;
// Pedido (sob tr): o que o player quer agora.
static int tela;           // player em tela cheia com video
static int suspenso;       // app pausado / saindo
static int ocupado = 1;    // o fio esta falando com a TV (1 ate a recuperacao do arranque terminar)

static int alvoDe(int ch, int tl, int su) { return su || !tl || ch <= 0 ? -1 : ch - 1; }

static void ajLog(const Ajuste *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void ajLog(const Ajuste *a, const char *fmt, ...) {
  va_list va;
  printf("[assperf] %s: ", a->nome);
  va_start(va, fmt);
  vprintf(fmt, va);
  va_end(va);
  printf("\n");
  fflush(stdout);
}

// ---- a nota de recuperacao (data/<ajuste>-original.txt) ------------------------
static int marcaGravar(Ajuste *a, int o, int p) {
  char tmp[620];
  FILE *f;
  int ok;
  if (!a->marca[0]) return 0;
  snprintf(tmp, sizeof tmp, "%s.tmp", a->marca);
  f = fopen(tmp, "w");
  if (!f) return 0;
  ok = fprintf(f, "orig=%d set=%d\n", o, p) > 0;
  ok = fflush(f) == 0 && ok;
  ok = fsync(fileno(f)) == 0 && ok;
  ok = fclose(f) == 0 && ok;
  if (!ok || rename(tmp, a->marca) != 0) { unlink(tmp); return 0; }
  return 1;
}
static void marcaApagar(Ajuste *a) { if (a->marca[0]) unlink(a->marca); }
static int marcaLer(Ajuste *a, int *o, int *p) {
  FILE *f = a->marca[0] ? fopen(a->marca, "r") : NULL;
  int n;
  if (!f) return -1;   // nao existe
  n = fscanf(f, "orig=%d set=%d", o, p);
  fclose(f);
  return n == 2 && (*o == 0 || *o == 1) && (*p == 0 || *p == 1) ? 1 : 0;
}

// ---- libavoc ----------------------------------------------------------------
// libavoc.so, aberta uma vez (so o fio). NULL = esta TV nao tem; `erro` diz por que.
static const char *nomeLib = "";
static char erroLib[300];
static void *libavoc(void) {
  static void *h;
  static int tentou;
  static const char *const NOMES[] = { "libavoc.so", "libavoc.so.0", NULL };
  if (!tentou) {
    int i;
    tentou = 1;
    for (i = 0; NOMES[i] && !h; i++) {
      h = dlopen(NOMES[i], RTLD_LAZY | RTLD_LOCAL);
      if (h) nomeLib = NOMES[i];
      else if (!erroLib[0]) { const char *e = dlerror(); snprintf(erroLib, sizeof erroLib, "%s", e ? e : "?"); }
    }
  }
  return h;
}

static int carregar(Ajuste *a) {
  void *h;
  const char *erro = erroLib;
  if (a->lib) return a->lib > 0;
  h = libavoc();
  a->lib = -1;
  if (!h) { ajLog(a, "not supported on this TV (libavoc: %s): nothing changed", erro); return 0; }
  a->ler = dlsym(h, a->fnLer);
  a->por = dlsym(h, a->fnPor);
  if (!a->ler || !a->por) {
    ajLog(a, "not supported on this TV (%s: %s %s, %s %s): nothing changed", nomeLib,
          a->fnLer, a->ler ? "found" : "missing", a->fnPor, a->por ? "found" : "missing");
    a->ler = a->por = NULL;
    return 0;
  }
  ajLog(a, "%s loaded (%s / %s%s)", nomeLib, a->fnLer, a->fnPor, a->comTipo ? ", slot AVOC_SAVE=1" : "");
  a->lib = 1;
  return 1;
}

// 1 = leu um valor valido (0/1). `r` recebe o retorno cru.
static int ler(Ajuste *a, int *v, int *r) {
  int x = -1;
  *r = a->comTipo ? ((int (*)(int *, int))a->ler)(&x, LUZ_AVOC_SAVE) : ((int (*)(int *))a->ler)(&x);
  *v = x;
  return *r == 0 && (x == 0 || x == 1);
}
static int por(Ajuste *a, int v) {
  return a->comTipo ? ((int (*)(int, int))a->por)(v, LUZ_AVOC_SAVE) : ((int (*)(int))a->por)(v);
}

// ---- mudar e devolver (so o fio) -----------------------------------------------
static void aplicar(Ajuste *a, int q) {
  int antes, depois, r, rl;
  if (!carregar(a)) return;
  if (!ler(a, &antes, &r)) {
    ajLog(a, "before=? (get returned %d, value %d) set=%d result=not changed", r, antes, q);
    return;
  }
  if (antes == q) {
    ajLog(a, "before=%d set=%d result=already %d on the TV, nothing changed", antes, q, antes);
    return;
  }
  if (!marcaGravar(a, antes, q)) {
    ajLog(a, "before=%d set=%d result=not changed (could not write the restore note %s: %s)", antes, q, a->marca,
          strerror(errno));
    return;
  }
  r = por(a, q);
  rl = ler(a, &depois, &(int){ 0 });
  ajLog(a, "before=%d set=%d result=%d (reads back %s%d) during full-screen playback", antes, q, r,
        rl ? "" : "? ", depois);
  if (rl && depois == antes) { marcaApagar(a); return; }   // a TV nao mudou: nada a devolver
  a->segura = 1; a->orig = antes; a->posto = rl ? depois : q;
}

static void restaurar(Ajuste *a, const char *motivo) {
  int agora, depois, r, rl, tent;
  if (!a->segura) return;
  a->segura = 0;
  if (ler(a, &agora, &r)) {
    if (agora == a->orig) { ajLog(a, "restore (%s): before=%d set=- result=already back", motivo, agora); marcaApagar(a); return; }
    if (agora != a->posto) {
      ajLog(a, "restore (%s): before=%d set=- result=left as is (not the %d the app set: changed outside the app)",
            motivo, agora, a->posto);
      marcaApagar(a);
      return;
    }
  } else agora = -1;
  for (tent = 0; tent < 2; tent++) {
    if (tent) usleep(300 * 1000);
    r = por(a, a->orig);
    rl = ler(a, &depois, &(int){ 0 });
    ajLog(a, "restore (%s): before=%d set=%d result=%d (reads back %s%d)%s", motivo, agora, a->orig, r,
          rl ? "" : "? ", depois, tent ? " [retry]" : "");
    if (rl ? depois == a->orig : r == 0) { marcaApagar(a); return; }
  }
  ajLog(a, "restore FAILED: the note %s stays; the next app start puts %d back", a->marca, a->orig);
}

// Arranque: a sessao anterior caiu (ou foi morta) segurando o valor da TV.
static void recuperar(Ajuste *a) {
  int o, p, n = marcaLer(a, &o, &p);
  if (n < 0) return;
  if (n == 0) { ajLog(a, "restore note %s unreadable: removed", a->marca); marcaApagar(a); return; }
  ajLog(a, "the previous session ended while it held the TV value (original %d, set %d): restoring", o, p);
  if (!carregar(a)) { marcaApagar(a); return; }
  a->segura = 1; a->orig = o; a->posto = p;
  restaurar(a, "previous session");
}

// ---- gama da TV, SO LEITURA ("Subtitle gamma" = Auto) ------------------------
// avoc_gamma_contrast_picture.md (24/10): avoc_get_gamma, D-Bus (i) -> (ii),
// slider -5..+5 (avocd, id 0x16a). A assinatura C segue a do
// avoc_get_luminance_limit, que tem o mesmo (i) -> (ii) e funcionou na S95C:
// int avoc_get_gamma(gint *out, gint type), type = AVOC_SAVE (1).
// Guarda: antes da PRIMEIRA chamada grava gama-auto-testando.txt e apaga
// quando ela volta. Se o app morrer dentro dela (assinatura errada), a nota
// fica e o Auto nao tenta mais nesta TV (o log diz).
#define GAMA_DESCONHECIDO 0x7fff
static int gamaTv = GAMA_DESCONHECIDO;
static char marcaGama[600];
static int autoGama;       // sob tr: a folha pede Auto e o player esta em tela cheia
int tpkluz_gama_tv(void) { return __atomic_load_n(&gamaTv, __ATOMIC_ACQUIRE); }

static void gamaLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void gamaLog(const char *fmt, ...) {
  va_list va;
  printf("[assperf] TV gamma: ");
  va_start(va, fmt);
  vprintf(fmt, va);
  va_end(va);
  printf("\n");
  fflush(stdout);
}

static void gamaLer(void) {
  static int (*f)(int *, int);
  static int estado, ultV = GAMA_DESCONHECIDO - 1, ultR = -12345;
  int v = GAMA_DESCONHECIDO, r;
  if (estado < 0) return;
  if (!estado) {
    void *h;
    FILE *m;
    if (marcaGama[0] && access(marcaGama, F_OK) == 0) {
      gamaLog("a previous avoc_get_gamma call never returned (note %s): Auto stays off on this TV", marcaGama);
      estado = -1; return;
    }
    h = libavoc();
    if (!h) { gamaLog("not supported on this TV (libavoc: %s): Auto = Off", erroLib); estado = -1; return; }
    f = (int (*)(int *, int))dlsym(h, "avoc_get_gamma");
    if (!f) { gamaLog("not supported on this TV (%s has no avoc_get_gamma): Auto = Off", nomeLib); estado = -1; return; }
    m = marcaGama[0] ? fopen(marcaGama, "w") : NULL;
    if (!m) { gamaLog("could not write the guard note %s: Auto = Off", marcaGama); estado = -1; return; }
    fputs("1\n", m); fflush(m); fsync(fileno(m)); fclose(m);
  }
  r = f(&v, LUZ_AVOC_SAVE);
  if (!estado) { unlink(marcaGama); estado = 1; }
  if (r != 0 || v < -5 || v > 5) {
    if (r != ultR || v != ultV) gamaLog("avoc_get_gamma(&v, AVOC_SAVE) returned %d, value %d: not a -5..+5 slider, Auto = Off", r, v);
    __atomic_store_n(&gamaTv, GAMA_DESCONHECIDO, __ATOMIC_RELEASE);
  } else {
    if (r != ultR || v != ultV)
      gamaLog("avoc_get_gamma(&v, AVOC_SAVE) returned %d, slider %+d: subtitle exponent %.4f (Samsung LUT: 1 - 0.0426 x slider)",
              r, v, 1.0 - 0.0426 * v);
    __atomic_store_n(&gamaTv, v, __ATOMIC_RELEASE);
  }
  ultR = r; ultV = v;
}

// Algum ajuste com pedido novo? (sob tr)
static int pendente(void) {
  for (int i = 0; i < NAJ; i++) if (alvoDe(AJ[i].chave, tela, suspenso) != AJ[i].feito) return 1;
  return 0;
}
static int soltos(void) {
  for (int i = 0; i < NAJ; i++) if (AJ[i].feito != -1) return 0;
  return 1;
}

static void *fio(void *u) {
  (void)u;
  for (int i = 0; i < NAJ; i++) recuperar(&AJ[i]);
  pthread_mutex_lock(&tr);
  ocupado = 0;
  pthread_cond_broadcast(&cv);
  for (;;) {
    int i;
    if (!pendente()) {
      if (autoGama && tela && !suspenso) {
        // Auto: le o gama da TV ao entrar e a cada 3 s (a pessoa pode mudar no menu).
        struct timespec ate;
        ocupado = 1;
        pthread_mutex_unlock(&tr);
        gamaLer();
        pthread_mutex_lock(&tr);
        ocupado = 0;
        pthread_cond_broadcast(&cv);
        clock_gettime(CLOCK_REALTIME, &ate);
        ate.tv_sec += 3;
        pthread_cond_timedwait(&cv, &tr, &ate);
      } else pthread_cond_wait(&cv, &tr);
      continue;
    }
    for (i = 0; i < NAJ; i++) {
      Ajuste *a = &AJ[i];
      int q = alvoDe(a->chave, tela, suspenso);
      const char *motivo;
      if (q == a->feito) continue;
      motivo = suspenso ? "app paused or closed" : !tela ? "playback stopped or left full screen"
             : a->chave <= 0 ? "toggle Off" : "value changed";
      ocupado = 1;
      pthread_mutex_unlock(&tr);
      restaurar(a, motivo);
      if (q >= 0) aplicar(a, q);
      pthread_mutex_lock(&tr);
      a->feito = q;
      ocupado = 0;
      pthread_cond_broadcast(&cv);
    }
  }
  return NULL;
}

void tpkluz_iniciar(const char *dados) {
  pthread_t t;
  if (iniciado || !dados) return;
  snprintf(marcaGama, sizeof marcaGama, "%s/gama-auto-testando.txt", dados);
  for (int i = 0; i < NAJ; i++) {
    snprintf(AJ[i].marca, sizeof AJ[i].marca, "%s/%s", dados, AJ[i].arqMarca);
    AJ[i].feito = -1;
  }
  if (pthread_create(&t, NULL, fio, NULL) != 0) { ajLog(&AJ[0], "could not start its thread: the toggles do nothing"); return; }
  pthread_detach(t);
  __atomic_store_n(&iniciado, 1, __ATOMIC_RELEASE);
}

void tpkluz_quadro(int telaCheia) {
  static int ultTela = -1, ult[8] = { -1, -1, -1, -1, -1, -1, -1, -1 }, ultAuto = -1;
  int ch[8], i, mudou = telaCheia != ultTela, au;
  if (!__atomic_load_n(&iniciado, __ATOMIC_ACQUIRE)) return;
  // assperf_get so com o player em tela cheia: ler o assperf.txt cedo demais
  // perderia a guarda de queda (assperf_iniciar). Fora dela vale a ultima.
  for (i = 0; i < NAJ; i++) {
    ch[i] = telaCheia ? assperf_get(AJ[i].ap) : ult[i] < 0 ? 0 : ult[i];
    if (ch[i] != ult[i]) mudou = 1;
  }
  au = telaCheia ? assperf_get(AP_GAMA) == AP_GAMA_AUTO : ultAuto > 0;
  if (au != ultAuto) mudou = 1;
  if (!mudou) return;
  ultTela = telaCheia; ultAuto = au;
  pthread_mutex_lock(&tr);
  for (i = 0; i < NAJ; i++) { ult[i] = ch[i]; AJ[i].chave = ch[i]; }
  tela = telaCheia;
  autoGama = au;
  pthread_cond_broadcast(&cv);
  pthread_mutex_unlock(&tr);
}

void tpkluz_sistema(int ativo) {
  struct timespec fim;
  int r = 0;
  if (!__atomic_load_n(&iniciado, __ATOMIC_ACQUIRE)) return;
  clock_gettime(CLOCK_REALTIME, &fim);
  fim.tv_sec += LUZ_ESPERA_MS / 1000;
  fim.tv_nsec += (long)(LUZ_ESPERA_MS % 1000) * 1000000L;
  if (fim.tv_nsec >= 1000000000L) { fim.tv_sec++; fim.tv_nsec -= 1000000000L; }
  pthread_mutex_lock(&tr);
  suspenso = !ativo;
  pthread_cond_broadcast(&cv);
  if (!ativo)
    while ((ocupado || !soltos()) && r != ETIMEDOUT) r = pthread_cond_timedwait(&cv, &tr, &fim);
  pthread_mutex_unlock(&tr);
  if (r == ETIMEDOUT) ajLog(&AJ[0], "pause/exit: the TV did not answer within %d ms (the restore keeps going)", LUZ_ESPERA_MS);
}

// Host .NET (Program.cs): OnResume = 1, OnTerminate = 0. A pausa ja chega
// por nv_tpk_ciclo(0) (tpk.c). Host antigo sem esta chamada: depois da
// primeira pausa os testes ficam desligados ate o app reabrir (o lado seguro).
__attribute__((visibility("default")))
void nv_tpk_luz(int ativo) { tpkluz_sistema(ativo); }
#endif
