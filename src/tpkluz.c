// LIMITE DE LUMINANCIA DA TV (.tpk, dono 24/10/2026): a linha
// "TV luminance limit (test)" da folha ASS performance.
//
// O branco do libass (255 lido de volta do framebuffer) aparece ~5 % mais
// cinza que o branco do video na S95C; o preto esta certo. A suspeita e a
// protecao do painel OLED limitando a camada grafica. O teste: desligar o
// "luminance limit" da TV durante a reproducao e comparar o branco do Colour
// test card com o do video.
//
// FONTE: so o que o dono extraiu do firmware (avoc_findings_full.md, 24/10):
//   int avoc_get_luminance_limit(gint *out_value, gint type);   // libavoc.so
//   int avoc_set_luminance_limit(gint limit, gint type);
//   limit: 0 = limite desligado, 1 = ligado (o avocd recusa fora de 0..1)
//   type:  1 = AVOC_SAVE (o valor em uso, gravado no banco do avocd)
//          2 = AVOC_SAVE_DEFAULT (o de fabrica; aqui nunca e escrito)
// O retorno 0 = sucesso NAO esta no documento (e a convencao das APIs Tizen):
// qualquer outro retorno conta como falha e a TV nao e mexida. Nada de
// ppi_ve_* (camada abaixo do avocd, sem leitura do valor) nem de
// avoc_set_osd_information (faixas desconhecidas).
//
// E UM AJUSTE DA TV INTEIRA, gravado no banco do avocd, e protege o painel.
// Por isso:
//  - so vale com o player em tela cheia COM video (tpkluz_quadro);
//  - le o valor antes, e devolve ele quando a chave volta a Off, a reproducao
//    para / sai da tela cheia, ou o app pausa ou fecha (tpkluz_sistema(0),
//    que espera a devolucao antes de o host seguir);
//  - antes de mudar grava luz-original.txt em data/ (com fsync); se o app cair
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

typedef int (*LuzLer)(int *valor, int tipo);
typedef int (*LuzPor)(int limite, int tipo);
static LuzLer fLer;
static LuzPor fPor;

static pthread_mutex_t tr = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int iniciado;
// Pedido (sob tr): o que a folha e o player querem agora.
static int chave;          // AP_LUZ: 0 Off, 1 = por 0, 2 = por 1
static int tela;           // player em tela cheia com video
static int suspenso;       // app pausado / saindo
// Estado do fio (sob tr para quem espera; so o fio muda).
static int ocupado = 1;    // ate a recuperacao do arranque terminar
static int feito = -1;     // valor pedido que o fio ja tratou (-1 = o da TV)
// So o fio le/escreve (sem trava).
static int segura;         // 1 = a TV esta com o NOSSO valor
static int orig, posto;
static char marca[600];

static int alvoDe(int ch, int tl, int su) { return su || !tl || ch <= 0 ? -1 : ch - 1; }

static void luzLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void luzLog(const char *fmt, ...) {
  va_list va;
  printf("[assperf] luminance limit: ");
  va_start(va, fmt);
  vprintf(fmt, va);
  va_end(va);
  printf("\n");
  fflush(stdout);
}

// ---- a nota de recuperacao (data/luz-original.txt) ----------------------------
static int marcaGravar(int o, int p) {
  char tmp[620];
  FILE *f;
  int ok;
  if (!marca[0]) return 0;
  snprintf(tmp, sizeof tmp, "%s.tmp", marca);
  f = fopen(tmp, "w");
  if (!f) return 0;
  ok = fprintf(f, "orig=%d set=%d\n", o, p) > 0;
  ok = fflush(f) == 0 && ok;
  ok = fsync(fileno(f)) == 0 && ok;
  ok = fclose(f) == 0 && ok;
  if (!ok || rename(tmp, marca) != 0) { unlink(tmp); return 0; }
  return 1;
}
static void marcaApagar(void) { if (marca[0]) unlink(marca); }
static int marcaLer(int *o, int *p) {
  FILE *f = marca[0] ? fopen(marca, "r") : NULL;
  int n;
  if (!f) return -1;   // nao existe
  n = fscanf(f, "orig=%d set=%d", o, p);
  fclose(f);
  return n == 2 && (*o == 0 || *o == 1) && (*p == 0 || *p == 1) ? 1 : 0;
}

// ---- libavoc ----------------------------------------------------------------
static int carregar(void) {
  static int tentou, ok;
  static const char *const NOMES[] = { "libavoc.so", "libavoc.so.0", NULL };
  void *h = NULL;
  char erro[300] = "";
  int i;
  if (tentou) return ok;
  tentou = 1;
  for (i = 0; NOMES[i] && !h; i++) {
    h = dlopen(NOMES[i], RTLD_LAZY | RTLD_LOCAL);
    if (!h && !erro[0]) { const char *e = dlerror(); snprintf(erro, sizeof erro, "%s", e ? e : "?"); }
  }
  if (!h) { luzLog("not supported on this TV (libavoc: %s): nothing changed", erro); return 0; }
  fLer = (LuzLer)dlsym(h, "avoc_get_luminance_limit");
  fPor = (LuzPor)dlsym(h, "avoc_set_luminance_limit");
  if (!fLer || !fPor) {
    luzLog("not supported on this TV (libavoc %s, avoc_get %s, avoc_set %s): nothing changed",
         NOMES[i - 1], fLer ? "found" : "missing", fPor ? "found" : "missing");
    fLer = NULL; fPor = NULL;
    return 0;
  }
  luzLog("%s loaded (avoc_get/set_luminance_limit, slot AVOC_SAVE=1)", NOMES[i - 1]);
  ok = 1;
  return 1;
}

// 1 = leu um valor valido (0/1). `r` recebe o retorno cru.
static int ler(int *v, int *r) {
  int x = -1;
  *r = fLer(&x, LUZ_AVOC_SAVE);
  *v = x;
  return *r == 0 && (x == 0 || x == 1);
}

// ---- mudar e devolver (so o fio) -----------------------------------------------
static void aplicar(int q) {
  int antes, depois, r, rl;
  if (!carregar()) return;
  if (!ler(&antes, &r)) {
    luzLog("before=? (get returned %d, value %d) set=%d result=not changed", r, antes, q);
    return;
  }
  if (antes == q) {
    luzLog("before=%d set=%d result=already %d on the TV, nothing changed", antes, q, antes);
    return;
  }
  if (!marcaGravar(antes, q)) {
    luzLog("before=%d set=%d result=not changed (could not write the restore note %s: %s)", antes, q, marca,
         strerror(errno));
    return;
  }
  r = fPor(q, LUZ_AVOC_SAVE);
  rl = ler(&depois, &(int){ 0 });
  luzLog("before=%d set=%d result=%d (reads back %s%d) during full-screen playback", antes, q, r,
       rl ? "" : "? ", depois);
  if (rl && depois == antes) { marcaApagar(); return; }   // a TV nao mudou: nada a devolver
  segura = 1; orig = antes; posto = rl ? depois : q;
}

static void restaurar(const char *motivo) {
  int agora, depois, r, rl, tent;
  if (!segura) return;
  segura = 0;
  if (ler(&agora, &r)) {
    if (agora == orig) { luzLog("restore (%s): before=%d set=- result=already back", motivo, agora); marcaApagar(); return; }
    if (agora != posto) {
      luzLog("restore (%s): before=%d set=- result=left as is (not the %d the app set: changed outside the app)",
           motivo, agora, posto);
      marcaApagar();
      return;
    }
  } else agora = -1;
  for (tent = 0; tent < 2; tent++) {
    if (tent) usleep(300 * 1000);
    r = fPor(orig, LUZ_AVOC_SAVE);
    rl = ler(&depois, &(int){ 0 });
    luzLog("restore (%s): before=%d set=%d result=%d (reads back %s%d)%s", motivo, agora, orig, r,
         rl ? "" : "? ", depois, tent ? " [retry]" : "");
    if (rl ? depois == orig : r == 0) { marcaApagar(); return; }
  }
  luzLog("restore FAILED: the note %s stays; the next app start puts %d back", marca, orig);
}

// Arranque: a sessao anterior caiu (ou foi morta) segurando o valor da TV.
static void recuperar(void) {
  int o, p, n = marcaLer(&o, &p);
  if (n < 0) return;
  if (n == 0) { luzLog("restore note %s unreadable: removed", marca); marcaApagar(); return; }
  luzLog("the previous session ended while it held the TV value (original %d, set %d): restoring", o, p);
  if (!carregar()) { marcaApagar(); return; }
  segura = 1; orig = o; posto = p;
  restaurar("previous session");
}

static void *fio(void *u) {
  (void)u;
  recuperar();
  pthread_mutex_lock(&tr);
  ocupado = 0;
  pthread_cond_broadcast(&cv);
  for (;;) {
    int q = alvoDe(chave, tela, suspenso);
    const char *motivo;
    if (q == feito) { pthread_cond_wait(&cv, &tr); continue; }
    motivo = suspenso ? "app paused or closed" : !tela ? "playback stopped or left full screen"
           : chave <= 0 ? "toggle Off" : "value changed";
    ocupado = 1;
    pthread_mutex_unlock(&tr);
    restaurar(motivo);
    if (q >= 0) aplicar(q);
    pthread_mutex_lock(&tr);
    feito = q;
    ocupado = 0;
    pthread_cond_broadcast(&cv);
  }
  return NULL;
}

void tpkluz_iniciar(const char *dados) {
  pthread_t t;
  if (iniciado || !dados) return;
  snprintf(marca, sizeof marca, "%s/luz-original.txt", dados);
  if (pthread_create(&t, NULL, fio, NULL) != 0) { luzLog("could not start its thread: the toggle does nothing"); return; }
  pthread_detach(t);
  __atomic_store_n(&iniciado, 1, __ATOMIC_RELEASE);
}

void tpkluz_quadro(int telaCheia) {
  static int ultTela = -1, ultChave = -1;
  int ch;
  if (!__atomic_load_n(&iniciado, __ATOMIC_ACQUIRE)) return;
  // assperf_get so com o player em tela cheia: ler o assperf.txt cedo demais
  // perderia a guarda de queda (assperf_iniciar). Fora dela vale a ultima.
  ch = telaCheia ? assperf_get(AP_LUZ) : ultChave < 0 ? 0 : ultChave;
  if (ch == ultChave && telaCheia == ultTela) return;
  ultChave = ch; ultTela = telaCheia;
  pthread_mutex_lock(&tr);
  chave = ch; tela = telaCheia;
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
    while ((ocupado || feito != -1) && r != ETIMEDOUT) r = pthread_cond_timedwait(&cv, &tr, &fim);
  pthread_mutex_unlock(&tr);
  if (r == ETIMEDOUT) luzLog("pause/exit: the TV did not answer within %d ms (the restore keeps going)", LUZ_ESPERA_MS);
}

// Host .NET (Program.cs): OnResume = 1, OnTerminate = 0. A pausa ja chega
// por nv_tpk_ciclo(0) (tpk.c). Host antigo sem esta chamada: depois da
// primeira pausa o teste fica desligado ate o app reabrir (o lado seguro).
__attribute__((visibility("default")))
void nv_tpk_luz(int ativo) { tpkluz_sistema(ativo); }
#endif
