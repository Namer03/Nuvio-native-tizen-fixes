// Ver agbfio.h.
//
// Como:
//  - um contexto GL novo, COMPARTILHADO com o da UI (texturas e programas sao
//    os mesmos objetos; FBO nao: o do assgpublur e criado no contexto do fio),
//    sem janela (EGL_KHR_surfaceless_context) ou com um pbuffer 1x1;
//  - a UI entrega pedidos (bitmaps copiados) e pega texturas prontas por chave;
//  - o fio borra, faz a passada final, poe uma cerca (glFenceSync) e glFlush; a
//    UI faz glWaitSync na cerca antes de desenhar a textura pela primeira vez
//    (a GPU da UI espera a do fio; a CPU nao espera);
//  - so o fio solta texturas finais, e so as que a UI nao usa ha 3+ quadros; a
//    reserva do assgpublur (modo fio) segura qualquer textura solta por 3
//    quadros da UI antes de regravar.
#include "agbfio.h"

#if defined(NV_TPK) || defined(NV_AGB_TESTE)
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef NV_AGB_TESTE
#include <EGL/egl.h>
static void *proc(const char *n) { return (void *)eglGetProcAddress(n); }
#define nomear(n) ((void)0)
#else
#include "tpk_egl.h"
#include "assfios.h"
static void *proc(const char *n) { return tpkEgl.GetProcAddress ? tpkEgl.GetProcAddress(n) : NULL; }
#define nomear(n) assfios_nomear(n)
#endif

#define AF_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define AF_TIMEOUT_IGNORED 0xFFFFFFFFFFFFFFFFull
typedef void *AfSync;
typedef AfSync (*FnFence)(unsigned, unsigned);
typedef void (*FnWaitSync)(AfSync, unsigned, unsigned long long);
typedef void (*FnDel)(AfSync);
static FnFence pFence;
static FnWaitSync pWaitSync;
static FnDel pDel;

#define AF_FILA 128
#define AF_FINAL_BYTES (24u * 1024u * 1024u)   // as finais, como no assgpublur

typedef struct {
  unsigned long long chave;
  AgbPedido p;
  unsigned char *src, *fill;   // copias (o quadro do libass pode sumir antes)
} AfTrab;

typedef struct {
  unsigned long long chave;
  GLuint tex;
  int tw, th, falha, esperado;
  AfSync cerca;
  unsigned uso;
} AfPronto;

static pthread_t fio;
static pthread_mutex_t trava = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t temTrab = PTHREAD_COND_INITIALIZER, pronto = PTHREAD_COND_INITIALIZER;
static int estado;             // 0 desligado, 1 rodando, -1 nao deu (nao tenta mais)
static int iniciou;            // fio: 1 ok, -1 falhou (resposta para agbfio_ligar)
static int parar, querApagar;
static AfTrab fila[AF_FILA];
static int nFila;
static unsigned long long emCurso;
static AfPronto *tab;
static int nTab, capTab;
static size_t bytesTab;
static unsigned quadroUi;
static AgbStats acSt;
static AgbPassos acPs;
static unsigned acEsperas;
static double acEsperaMs;
static EGLDisplay dpy;
static EGLContext ctx;
static EGLSurface sup;

static double agora_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static AfPronto *achar(unsigned long long k) {
  int i;
  for (i = 0; i < nTab; i++) if (tab[i].chave == k) return &tab[i];
  return NULL;
}
static int pendente(unsigned long long k) {
  int i;
  if (emCurso == k) return 1;
  for (i = 0; i < nFila; i++) if (fila[i].chave == k) return 1;
  return 0;
}

static void soma_stats(const AgbStats *s) {
  int i;
  acSt.blurs += s->blurs; acSt.finais += s->finais; acSt.reusos += s->reusos; acSt.falhas += s->falhas;
  acSt.ms += s->ms; acSt.adiantados += s->adiantados; acSt.criadas += s->criadas;
  acSt.reaproveitadas += s->reaproveitadas;
  for (i = 0; i < 4; i++) { acSt.nTam[i] += s->nTam[i]; acSt.msTam[i] += s->msTam[i]; }
}
static void soma_passos(const AgbPassos *p) {
  acPs.lotes += p->lotes; acPs.lotesTrab += p->lotesTrab; acPs.envios += p->envios;
  acPs.passadas += p->passadas; acPs.finais += p->finais;
  acPs.salvar += p->salvar; acPs.envio += p->envio; acPs.passadasMs += p->passadasMs;
  acPs.finalMs += p->finalMs; acPs.restaurar += p->restaurar; acPs.loteMs += p->loteMs;
  if (p->loteMax > acPs.loteMax) acPs.loteMax = p->loteMax;
}

/* Sob a trava: tira da tabela as finais que a UI nao usa ha 3+ quadros quando
 * passa do teto (ou todas as velhas, `idade` > 0). As chaves vao para `soltar`
 * (o fio solta no assgpublur fora da trava). */
static int escolher_soltar(unsigned long long *soltar, int max, unsigned q, unsigned idade) {
  int n = 0;
  for (;;) {
    int i, v = -1;
    for (i = 0; i < nTab; i++) {
      unsigned a = q - tab[i].uso;
      if (a < 3u || (int)a < 0) continue;
      if (idade ? a >= idade : bytesTab > AF_FINAL_BYTES)
        if (v < 0 || tab[i].uso < tab[v].uso) v = i;
    }
    if (v < 0 || n >= max) break;
    if (!tab[v].falha) { soltar[n++] = tab[v].chave; bytesTab -= (size_t)tab[v].tw * (size_t)tab[v].th * 4u; }
    if (tab[v].cerca) pDel(tab[v].cerca);
    tab[v] = tab[--nTab];
  }
  return n;
}

static void *laco(void *u) {
  unsigned ultimoTrab = 0;
  (void)u;
  nomear("nv-agbgl");
  if (!eglMakeCurrent(dpy, sup, sup, ctx)) {
    pthread_mutex_lock(&trava); iniciou = -1; pthread_cond_broadcast(&pronto); pthread_mutex_unlock(&trava);
    return NULL;
  }
  assgpublur_modo_fio(1);
  {
    int ok = assgpublur_iniciar();
    *(void **)&pFence = proc("glFenceSync");
    *(void **)&pWaitSync = proc("glWaitSync");
    *(void **)&pDel = proc("glDeleteSync");
    ok = ok && pFence && pWaitSync && pDel;
    pthread_mutex_lock(&trava); iniciou = ok ? 1 : -1; pthread_cond_broadcast(&pronto); pthread_mutex_unlock(&trava);
    if (!ok) { assgpublur_reiniciar(); assgpublur_modo_fio(0); eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); return NULL; }
  }
  for (;;) {
    AfTrab t;
    unsigned q;
    unsigned long long soltar[64];
    int ns, i, temT = 0;
    pthread_mutex_lock(&trava);
    while (!parar && !querApagar && !nFila) {
      struct timespec ate;
      clock_gettime(CLOCK_REALTIME, &ate);
      ate.tv_sec += 1;
      if (pthread_cond_timedwait(&temTrab, &trava, &ate)) break;   // 1 s ocioso: aparar
    }
    if (parar) { pthread_mutex_unlock(&trava); break; }
    if (querApagar) {
      querApagar = 0;
      for (i = 0; i < nTab; i++) if (tab[i].cerca) pDel(tab[i].cerca);
      nTab = 0; bytesTab = 0;
      pthread_mutex_unlock(&trava);
      assgpublur_apagar();
      glFlush();
      pthread_mutex_lock(&trava); pthread_cond_broadcast(&pronto); pthread_mutex_unlock(&trava);
      continue;
    }
    q = __atomic_load_n(&quadroUi, __ATOMIC_RELAXED);
    if (nFila) {
      t = fila[0];
      memmove(fila, fila + 1, (size_t)(nFila - 1) * sizeof fila[0]);
      nFila--;
      emCurso = t.chave;
      temT = 1;
    }
    ns = escolher_soltar(soltar, 64, q, temT ? 0u : (q - ultimoTrab >= 300u ? 120u : 0u));
    pthread_mutex_unlock(&trava);
    for (i = 0; i < ns; i++) assgpublur_soltar_final(soltar[i]);
    if (!temT) {
      AgbStats st; AgbPassos ps;
      assgpublur_aparar(q);
      assgpublur_stats(&st); assgpublur_passos(&ps);
      pthread_mutex_lock(&trava); soma_stats(&st); soma_passos(&ps); pthread_mutex_unlock(&trava);
      continue;
    }
    {
      GLuint tex;
      unsigned long long k = 0;
      int tw = 0, th = 0;
      AfSync c = NULL;
      AgbStats st; AgbPassos ps;
      AfPronto *e;
      assgpublur_lote_ini(0);
      tex = assgpublur_obter(&t.p, q, &k);
      if (tex) { assgpublur_ultimo_tam(&tw, &th); c = pFence(AF_SYNC_GPU_COMMANDS_COMPLETE, 0); }
      assgpublur_lote_fim();
      glFlush();
      assgpublur_stats(&st); assgpublur_passos(&ps);
      free(t.src); free(t.fill);
      ultimoTrab = q;
      pthread_mutex_lock(&trava);
      soma_stats(&st); soma_passos(&ps);
      e = achar(t.chave);
      if (!e) {
        if (nTab == capTab) {
          int nc = capTab ? capTab * 2 : 64;
          AfPronto *nv = realloc(tab, (size_t)nc * sizeof *nv);
          if (nv) { tab = nv; capTab = nc; }
        }
        if (nTab < capTab) { e = &tab[nTab++]; memset(e, 0, sizeof *e); }
      } else if (e->cerca) { pDel(e->cerca); e->cerca = NULL; }
      if (e) {
        if (!e->falha && e->tex) bytesTab -= (size_t)e->tw * (size_t)e->th * 4u;
        e->chave = t.chave; e->tex = tex; e->tw = tw; e->th = th; e->falha = !tex; e->esperado = 0;
        e->cerca = c; e->uso = q;
        if (tex) bytesTab += (size_t)tw * (size_t)th * 4u;
      } else if (c) pDel(c);
      emCurso = 0;
      pthread_cond_broadcast(&pronto);
      pthread_mutex_unlock(&trava);
    }
  }
  /* fim: solta tudo neste contexto */
  pthread_mutex_lock(&trava);
  for (int i = 0; i < nTab; i++) if (tab[i].cerca) pDel(tab[i].cerca);
  nTab = 0; bytesTab = 0;
  for (int i = 0; i < nFila; i++) { free(fila[i].src); free(fila[i].fill); }
  nFila = 0;
  pthread_mutex_unlock(&trava);
  assgpublur_reiniciar();
  assgpublur_modo_fio(0);
  glFinish();
  eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  return NULL;
}

int agbfio_ativo(void) { return estado == 1; }

int agbfio_ligar(void) {
  EGLContext atual;
  EGLint id = 0, n = 0, versao = 2, tipo = 0;
  EGLConfig cfg;
  const char *ext;
  if (estado) return estado == 1;
  estado = -1;
  dpy = eglGetCurrentDisplay();
  atual = eglGetCurrentContext();
  if (dpy == EGL_NO_DISPLAY || atual == EGL_NO_CONTEXT) { printf("[assperf] GPU blur thread: no current EGL context\n"); return 0; }
#ifndef NV_AGB_TESTE
  if (!tpkEgl.CreateContext || !tpkEgl.QueryString || !tpkEgl.DestroyContext) {
    printf("[assperf] GPU blur thread: libEGL is missing functions\n"); return 0;
  }
#endif
  eglQueryContext(dpy, atual, EGL_CONFIG_ID, &id);
  eglQueryContext(dpy, atual, EGL_CONTEXT_CLIENT_VERSION, &versao);
  {
    EGLint a[] = { EGL_CONFIG_ID, id, EGL_NONE };
    if (!eglChooseConfig(dpy, a, &cfg, 1, &n) || n < 1) { printf("[assperf] GPU blur thread: config %d not found\n", (int)id); return 0; }
  }
  {
    EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, versao < 2 ? 2 : versao, EGL_NONE };
    ctx = eglCreateContext(dpy, cfg, atual, ca);
  }
  if (ctx == EGL_NO_CONTEXT) { printf("[assperf] GPU blur thread: shared context refused (EGL 0x%x)\n", (unsigned)eglGetError()); return 0; }
  ext = eglQueryString(dpy, EGL_EXTENSIONS);
  sup = EGL_NO_SURFACE;
  if (!ext || !strstr(ext, "EGL_KHR_surfaceless_context")) {
    EGLint pa[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    eglGetConfigAttrib(dpy, cfg, EGL_SURFACE_TYPE, &tipo);
#ifndef NV_AGB_TESTE
    if (tpkEgl.CreatePbufferSurface && (tipo & EGL_PBUFFER_BIT)) sup = eglCreatePbufferSurface(dpy, cfg, pa);
#else
    if (tipo & EGL_PBUFFER_BIT) sup = eglCreatePbufferSurface(dpy, cfg, pa);
#endif
    if (sup == EGL_NO_SURFACE) {
      printf("[assperf] GPU blur thread: no surfaceless context and no pbuffer for config %d\n", (int)id);
      eglDestroyContext(dpy, ctx); ctx = EGL_NO_CONTEXT;
      return 0;
    }
  }
  /* o assgpublur sai do contexto da UI: o fio recria tudo no dele */
  assgpublur_reiniciar();
  parar = 0; querApagar = 0; iniciou = 0; nFila = 0; nTab = 0; bytesTab = 0; emCurso = 0;
  if (pthread_create(&fio, NULL, laco, NULL)) {
    printf("[assperf] GPU blur thread: thread not created\n");
    eglDestroyContext(dpy, ctx); ctx = EGL_NO_CONTEXT;
    if (sup != EGL_NO_SURFACE) { eglDestroySurface(dpy, sup); sup = EGL_NO_SURFACE; }
    return 0;
  }
  pthread_mutex_lock(&trava);
  while (!iniciou) pthread_cond_wait(&pronto, &trava);
  pthread_mutex_unlock(&trava);
  if (iniciou < 0) {
    pthread_join(fio, NULL);
    eglDestroyContext(dpy, ctx); ctx = EGL_NO_CONTEXT;
    if (sup != EGL_NO_SURFACE) { eglDestroySurface(dpy, sup); sup = EGL_NO_SURFACE; }
    printf("[assperf] GPU blur thread: the thread's GL context did not start\n");
    return 0;
  }
  estado = 1;
  printf("[assperf] GPU blur thread: on (shared GL context %s, ES %d)\n",
         sup == EGL_NO_SURFACE ? "without a surface" : "on a 1x1 pbuffer", (int)versao);
  fflush(stdout);
  return 1;
}

void agbfio_desligar(void) {
  if (estado != 1) { if (estado < 0) estado = 0; return; }
  pthread_mutex_lock(&trava); parar = 1; pthread_cond_broadcast(&temTrab); pthread_mutex_unlock(&trava);
  pthread_join(fio, NULL);
  eglDestroyContext(dpy, ctx); ctx = EGL_NO_CONTEXT;
  if (sup != EGL_NO_SURFACE) { eglDestroySurface(dpy, sup); sup = EGL_NO_SURFACE; }
  estado = 0;
  printf("[assperf] GPU blur thread: off\n");
  fflush(stdout);
}

void agbfio_quadro(unsigned quadro) { __atomic_store_n(&quadroUi, quadro, __ATOMIC_RELAXED); }

/* Sob a trava. frente = 1: a UI esta esperando por este. */
static int enfileirar_locked(const AgbPedido *p, unsigned long long k, int frente) {
  AfTrab t;
  if (nFila >= AF_FILA) {
    if (!frente) return 0;
    free(fila[nFila - 1].src); free(fila[nFila - 1].fill);
    nFila--;   // o ultimo adiantado cede o lugar
  }
  memset(&t, 0, sizeof t);
  t.chave = k; t.p = *p;
  t.src = malloc((size_t)p->srcW * (size_t)p->srcH);
  if (!t.src) return 0;
  memcpy(t.src, p->src, (size_t)p->srcW * (size_t)p->srcH);
  t.p.src = t.src;
  if (p->fix && p->fill) {
    t.fill = malloc((size_t)p->fillW * (size_t)p->fillH);
    if (!t.fill) { free(t.src); return 0; }
    memcpy(t.fill, p->fill, (size_t)p->fillW * (size_t)p->fillH);
    t.p.fill = t.fill;
  } else t.p.fill = NULL;
  t.p.soCache = 0;
  if (frente) { memmove(fila + 1, fila, (size_t)nFila * sizeof fila[0]); fila[0] = t; }
  else fila[nFila] = t;
  nFila++;
  pthread_cond_signal(&temTrab);
  return 1;
}

void agbfio_pedir(const AgbPedido *p) {
  unsigned long long k;
  if (estado != 1 || !p->src) return;
  k = assgpublur_chave(p);
  pthread_mutex_lock(&trava);
  if (!achar(k) && !pendente(k)) { AgbPedido c = *p; c.adiantado = 1; enfileirar_locked(&c, k, 0); }
  pthread_mutex_unlock(&trava);
}

GLuint agbfio_obter(const AgbPedido *p, unsigned quadro, unsigned long long *chave, int *tw, int *th, int esperaMs) {
  unsigned long long k;
  AfPronto *e;
  GLuint t = 0;
  AfSync c = NULL;
  double t0 = 0;
  *chave = 0;
  if (estado != 1 || !p->src) return 0;
  k = assgpublur_chave(p);
  pthread_mutex_lock(&trava);
  e = achar(k);
  if (!e) {
    struct timespec ate;
    if (!pendente(k)) enfileirar_locked(p, k, 1);
    else {   // ja na fila: passa para a frente
      int i;
      for (i = 1; i < nFila; i++) if (fila[i].chave == k) { AfTrab x = fila[i]; memmove(fila + 1, fila, (size_t)i * sizeof fila[0]); fila[0] = x; break; }
    }
    t0 = agora_ms();
    clock_gettime(CLOCK_REALTIME, &ate);
    ate.tv_nsec += (long)esperaMs * 1000000L;
    ate.tv_sec += ate.tv_nsec / 1000000000L; ate.tv_nsec %= 1000000000L;
    while (!(e = achar(k)))
      if (pthread_cond_timedwait(&pronto, &trava, &ate)) { e = achar(k); break; }
    acEsperas++; acEsperaMs += agora_ms() - t0;
  }
  if (e && !e->falha && e->tex) {
    e->uso = quadro;
    t = e->tex; *tw = e->tw; *th = e->th; *chave = k;
    if (!e->esperado) { c = e->cerca; e->esperado = 1; }
  }
  pthread_mutex_unlock(&trava);
  if (c) pWaitSync(c, 0, AF_TIMEOUT_IGNORED);   // a GPU da UI espera a do fio
  return t;
}

void agbfio_usar(unsigned long long chave, unsigned quadro) {
  AfPronto *e;
  if (estado != 1 || !chave) return;
  pthread_mutex_lock(&trava);
  e = achar(chave);
  if (e) e->uso = quadro;
  pthread_mutex_unlock(&trava);
}

void agbfio_apagar(void) {
  if (estado != 1) return;
  pthread_mutex_lock(&trava);
  while (nFila) { nFila--; free(fila[nFila].src); free(fila[nFila].fill); }
  querApagar = 1;
  pthread_cond_signal(&temTrab);
  pthread_mutex_unlock(&trava);
}

void agbfio_colher(AgbStats *st, AgbPassos *ps, unsigned *esperas, double *esperaMs) {
  pthread_mutex_lock(&trava);
  *st = acSt; *ps = acPs; *esperas = acEsperas; *esperaMs = acEsperaMs;
  memset(&acSt, 0, sizeof acSt); memset(&acPs, 0, sizeof acPs); acEsperas = 0; acEsperaMs = 0;
  pthread_mutex_unlock(&trava);
}

#else
#include <string.h>
int  agbfio_ligar(void) { return 0; }
void agbfio_desligar(void) {}
int  agbfio_ativo(void) { return 0; }
void agbfio_quadro(unsigned quadro) { (void)quadro; }
void agbfio_pedir(const AgbPedido *p) { (void)p; }
GLuint agbfio_obter(const AgbPedido *p, unsigned quadro, unsigned long long *chave, int *tw, int *th, int esperaMs) {
  (void)p; (void)quadro; (void)tw; (void)th; (void)esperaMs; *chave = 0; return 0;
}
void agbfio_usar(unsigned long long chave, unsigned quadro) { (void)chave; (void)quadro; }
void agbfio_apagar(void) {}
void agbfio_colher(AgbStats *st, AgbPassos *ps, unsigned *esperas, double *esperaMs) {
  memset(st, 0, sizeof *st); memset(ps, 0, sizeof *ps); *esperas = 0; *esperaMs = 0;
}
#endif
