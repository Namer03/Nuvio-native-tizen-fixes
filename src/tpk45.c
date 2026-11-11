// Tizen 4-5.5 .tpk: memoria da conversao e das miniaturas + linhas de teste
// do menu de audio. Contrato em tpk45.h.
#include "tpk45.h"
#include <stdio.h>
#include <string.h>

#ifndef NV_TPK40

// Fora da .so do NuvioTpk40 (hosts 6+, LG, Android, desktop): nada muda.
int tpk45_ativo(void) { return 0; }
int tpk45_perfil(void) { return TPK45_PERFIL_NORMAL; }
int tpk45_perfil_efetivo(void) { return TPK45_PERFIL_NORMAL; }
int tpk45_entrega(void) { return TPK45_ENTREGA_HLS; }
const char *tpk45_marca(void) { return ""; }
int tpk45_anel_mib(void) { return 0; }
int tpk45_fila_mib(void) { return 0; }
int tpk45_aperto(long livreKb, int *apertado) { (void)livreKb; (void)apertado; return 0; }
long tpk45_mini_piso_kb(void) { return 0; }
int tpk45_mini_altura_max(void) { return 0; }
int tpk45_mini_espera_conversor(void) { return 0; }
void tpk45_mini_estado(int feitas, int total, int pausa) { (void)feitas; (void)total; (void)pausa; }
int tpk45_linhas(void) { return 0; }
void tpk45_rotulo(int linha, char *nome, size_t tn, char *sub, size_t ts) {
  (void)linha;
  if (nome && tn) nome[0] = 0;
  if (sub && ts) sub[0] = 0;
}
void tpk45_passo(int linha, int d) { (void)linha; (void)d; }
int tpk45_mudanca_assentou(void) { return 0; }
void tpk45_tv(void) {}
void tpk45_tick(int convertendo, const char *modo) { (void)convertendo; (void)modo; }

#else

#include "dados.h"
#include "dts/dts_engine.h"
#include "dts/dts_remux.h"
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

// fcntl64 DENTRO da lib do 4/5. O FFmpeg e compilado com arquivos de 64 bits,
// e na libc da build (2.28) isso troca cada fcntl() dele por fcntl64, funcao
// que nasceu nessa mesma 2.28. A libc das TVs Tizen 4/5 e mais antiga: nao tem
// esse nome, o carregador ELF do Program40.cs nao acharia o simbolo e o app nem
// abriria (tools/tpk.sh barra a build por isso). Definida aqui, a referencia
// do FFmpeg fecha dentro da propria lib e nada novo e pedido a TV.
// E o que a libc faz: a syscall fcntl64 do kernel ARM de 32 bits, que existe
// desde o 2.4 (o resto do app segue no fcntl de sempre). O FFmpeg so a usa com
// F_SETFD, F_SETFL/F_GETFL e F_DUPFD_CLOEXEC.
#ifdef SYS_fcntl64
__attribute__((visibility("hidden"))) int fcntl64(int fd, int cmd, ...) {
  va_list ap;
  void *arg;
  va_start(ap, cmd);
  arg = va_arg(ap, void *);
  va_end(ap);
  return (int)syscall(SYS_fcntl64, fd, cmd, arg);
}
#endif

#define ARQ "tpk45-teste.txt"

// Os tres perfis. "Normal" e o que as TVs Tizen 6-7 ja usam (dts_hls_tpk.c:
// anel 16 MiB, fila 128 MiB) e o piso de 150 MB das miniaturas. O anel aceita
// 8..32 MiB e a fila 32..512 MiB (dts_engine.c, dts_remux.c).
// O ANEL e a memoria de verdade (um por leitor vivo, ate tres). A FILA e so um
// teto: no uso normal ela guarda poucos MB (o intervalo entre audio e video no
// arquivo) e bater no teto e ERRO do conversor ("Queue memory budget reached"),
// nao pausa. Por isso os perfis baixam o anel a serio e a fila com cuidado.
static const struct {
  const char *nome;
  int anel, fila;        // conversor, MiB
  long miniPisoMb;       // miniaturas: piso de MemAvailable
  int miniAltura;        // 0 = qualquer; senao so ate esta altura de video
  int miniEspera;        // esperam enquanto o conversor trabalha
} P[TPK45_PERFIS] = {
  [TPK45_PERFIL_AUTO]   = { "auto",    0,   0,   0,    0, 0 },
  [TPK45_PERFIL_NORMAL] = { "normal", 16, 128, 150,    0, 0 },
  [TPK45_PERFIL_LEVE]   = { "light",   8,  96, 150,    0, 1 },
  [TPK45_PERFIL_MINIMO] = { "minimum", 8,  64, 200, 1152, 1 },
};
static const char *const E[TPK45_ENTREGAS] = { "auto", "HLS only", "continuous TS only" };

// AUTO pela memoria LIVRE (MemAvailable) de agora. Na S95C o conversor mais as
// miniaturas rodaram bem com 201-375 MB livres e valores maiores que "normal";
// aqui a escada desce antes, porque ninguem mediu uma TV de 2018-2020.
#define AUTO_NORMAL_MB 400
#define AUTO_LEVE_MB   250
// Aperto: o piso do app (memlog.h, 150 MB); sai dele com folga para nao oscilar.
#define APERTO_MB  150
#define FOLGA_MB   220

static atomic_int perfil = TPK45_PERFIL_AUTO, entrega = TPK45_ENTREGA_AUTO;
static atomic_int lido;
static Uint32 mudouEm;          // 0 = nada pendente (so o fio do desenho mexe)
static atomic_int miniFeitas, miniTotal, miniPausa = 3;

static long lerKb(const char *arq, const char *chave) {
  char l[160];
  size_t k = strlen(chave);
  long v = -1;
  FILE *f = fopen(arq, "r");
  if (!f) return -1;
  while (fgets(l, sizeof l, f))
    if (!strncmp(l, chave, k) && l[k] == ':') { v = atol(l + k + 1); break; }
  fclose(f);
  return v;
}
static long livreMb(void) { long v = lerKb("/proc/meminfo", "MemAvailable"); return v < 0 ? -1 : v / 1024; }

static void ler(void) {
  char *t;
  int p = -1, e = -1;
  if (atomic_exchange(&lido, 1)) return;
  t = dados_ler(ARQ);
  if (t && sscanf(t, "perfil=%d entrega=%d", &p, &e) == 2) {
    if (p >= 0 && p < TPK45_PERFIS) atomic_store(&perfil, p);
    if (e >= 0 && e < TPK45_ENTREGAS) atomic_store(&entrega, e);
  }
  free(t);
}
static void gravar(void) {
  char b[48];
  snprintf(b, sizeof b, "perfil=%d entrega=%d\n", atomic_load(&perfil), atomic_load(&entrega));
  dados_gravar(ARQ, b);
}

int tpk45_ativo(void) { return 1; }
int tpk45_perfil(void) { ler(); return atomic_load(&perfil); }
int tpk45_entrega(void) { ler(); return atomic_load(&entrega); }

// O Auto resolvido. A leitura vale 5 s: quem pergunta por quadro (miniaturas)
// nao reabre o /proc a cada vez, e o perfil nao troca no meio de uma conta.
int tpk45_perfil_efetivo(void) {
  static atomic_int cache = TPK45_PERFIL_LEVE;
  static atomic_uint lidoEm;
  int p = tpk45_perfil();
  unsigned agora = SDL_GetTicks();
  if (p != TPK45_PERFIL_AUTO) return p;
  if (!atomic_load(&lidoEm) || (int)(agora - atomic_load(&lidoEm)) > 5000) {
    long mb = livreMb();
    atomic_store(&cache, mb < 0 ? TPK45_PERFIL_LEVE
                       : mb >= AUTO_NORMAL_MB ? TPK45_PERFIL_NORMAL
                       : mb >= AUTO_LEVE_MB ? TPK45_PERFIL_LEVE : TPK45_PERFIL_MINIMO);
    atomic_store(&lidoEm, agora ? agora : 1u);
  }
  return atomic_load(&cache);
}

// Todas as combinacoes montadas uma vez: o ponteiro devolvido vale para sempre,
// em qualquer fio (o fio da memoria e o das miniaturas tambem logam com ele).
static char marcas[TPK45_PERFIS][TPK45_PERFIS][TPK45_ENTREGAS][64];
static pthread_once_t marcasUma = PTHREAD_ONCE_INIT;
static void marcasMontar(void) {
  for (int p = 0; p < TPK45_PERFIS; p++)
    for (int ef = 1; ef < TPK45_PERFIS; ef++)
      for (int e = 0; e < TPK45_ENTREGAS; e++)
        snprintf(marcas[p][ef][e], sizeof marcas[p][ef][e], "profile %s%s, delivery %s", P[ef].nome,
                 p == TPK45_PERFIL_AUTO ? " (auto)" : "", E[e]);
}
const char *tpk45_marca(void) {
  pthread_once(&marcasUma, marcasMontar);
  return marcas[tpk45_perfil()][tpk45_perfil_efetivo()][tpk45_entrega()];
}

int tpk45_anel_mib(void) { return P[tpk45_perfil_efetivo()].anel; }
int tpk45_fila_mib(void) { return P[tpk45_perfil_efetivo()].fila; }
int tpk45_aperto(long livreKb, int *apertado) {
  long mb = livreKb < 0 ? -1 : livreKb / 1024;
  if (mb >= 0 && !*apertado && mb < APERTO_MB) { *apertado = 1; return 1; }
  if (*apertado && mb >= FOLGA_MB) { *apertado = 0; return -1; }
  return 0;
}

long tpk45_mini_piso_kb(void) { return P[tpk45_perfil_efetivo()].miniPisoMb * 1024L; }
int tpk45_mini_altura_max(void) { return P[tpk45_perfil_efetivo()].miniAltura; }
int tpk45_mini_espera_conversor(void) { return P[tpk45_perfil_efetivo()].miniEspera; }
void tpk45_mini_estado(int feitas, int total, int pausa) {
  if (feitas >= 0) atomic_store(&miniFeitas, feitas);
  if (total >= 0) atomic_store(&miniTotal, total);
  atomic_store(&miniPausa, pausa);
}

// --- linhas de teste do menu de audio ----------------------------------------
int tpk45_linhas(void) { return 3; }
void tpk45_rotulo(int linha, char *nome, size_t tn, char *sub, size_t ts) {
  int p = tpk45_perfil(), ef = tpk45_perfil_efetivo(), e = tpk45_entrega();
  if (linha == 0) {
    snprintf(nome, tn, "Test: memory profile");
    snprintf(sub, ts, "%s%s (ring %d, queue cap %d MiB). OK: next", p == TPK45_PERFIL_AUTO ? "Auto, now " : "",
             P[ef].nome, P[ef].anel, P[ef].fila);
  } else if (linha == 1) {
    snprintf(nome, tn, "Test: delivery");
    snprintf(sub, ts, "%s. OK: next", e == TPK45_ENTREGA_AUTO ? "Auto (HLS, continuous TS if refused)" : E[e]);
  } else {
    // A folha redesenha a cada quadro: o /proc e relido uma vez por segundo.
    static char texto[96];
    static Uint32 em;
    Uint32 agora = SDL_GetTicks();
    if (!em || (Sint32)(agora - em) >= 1000) {
      long rss = lerKb("/proc/self/status", "VmRSS");
      snprintf(texto, sizeof texto, "free %ld MB, app %ld MB, thumbnails %d of %d", livreMb(), rss < 0 ? -1 : rss / 1024,
               atomic_load(&miniFeitas), atomic_load(&miniTotal));
      em = agora ? agora : 1u;
    }
    snprintf(nome, tn, "Memory now");
    snprintf(sub, ts, "%s", texto);
  }
}
void tpk45_passo(int linha, int d) {
  int n, v;
  ler();
  if (linha == 0) { n = TPK45_PERFIS; v = atomic_load(&perfil); }
  else if (linha == 1) { n = TPK45_ENTREGAS; v = atomic_load(&entrega); }
  else return;
  v = ((v + (d < 0 ? -1 : 1)) % n + n) % n;
  atomic_store(linha == 0 ? &perfil : &entrega, v);
  gravar();
  mudouEm = SDL_GetTicks();
  if (!mudouEm) mudouEm = 1u;   // nunca `| 1`: num tick par passava de agora e a conta sem sinal dava 4 G ms
  printf("[tpk45] test menu: %s (free %ld MB)\n", tpk45_marca(), livreMb());
  fflush(stdout);
}
int tpk45_mudanca_assentou(void) {
  if (!mudouEm || (Sint32)(SDL_GetTicks() - mudouEm) < 2500) return 0;
  mudouEm = 0;
  return 1;
}

// --- log ---------------------------------------------------------------------
static void sysinfo(const char *chave, char *dst, size_t n) {
  static int (*get)(const char *, char **);
  static int tentou;
  char *t = NULL;
  snprintf(dst, n, "?");
  if (!tentou) {
    static const char *const libs[] = { "libcapi-system-info.so.0", "libcapi-system-info.so", NULL };
    void *h = NULL;
    tentou = 1;
    for (int i = 0; libs[i] && !h; i++) h = dlopen(libs[i], RTLD_NOW);
    if (h) *(void **)&get = dlsym(h, "system_info_get_platform_string");
  }
  if (get && get(chave, &t) == 0 && t) { snprintf(dst, n, "%s", t); free(t); }
}
// Como a lib entrou (Program40.cs): memfd aparece no mapa do processo; o
// carregador ELF proprio so deixa memoria anonima.
static const char *carregador(void) {
  char l[512];
  const char *r = "ELF loader (anonymous memory)";
  FILE *f = fopen("/proc/self/maps", "r");
  if (!f) return "?";
  while (fgets(l, sizeof l, f)) if (strstr(l, "memfd:")) { r = "memfd"; break; }
  fclose(f);
  return r;
}
// So o fio do desenho chama (tpk45_tick): uma vez por execucao.
void tpk45_tv(void) {
  static int feito;
  char ver[32], modelo[64], build[96];
  if (feito) return;
  feito = 1;
  sysinfo("http://tizen.org/feature/platform.version", ver, sizeof ver);
  sysinfo("http://tizen.org/system/model_name", modelo, sizeof modelo);
  sysinfo("http://tizen.org/system/build.string", build, sizeof build);
  printf("[tpk45] tv: tizen %s, model %s, build %s, RAM %ld MB, free %ld MB, %ld core(s), library by %s, FFmpeg %s; %s\n",
         ver, modelo, build, lerKb("/proc/meminfo", "MemTotal") / 1024, livreMb(), sysconf(_SC_NPROCESSORS_ONLN),
         carregador(), dts_remux_available() ? "linked" : "missing", tpk45_marca());
  printf("[tpk45] profiles: normal ring 16 queue cap 128 MiB, thumbnails floor 150 MB | light 8 / 96, thumbnails wait for "
         "the converter | minimum 8 / 64, thumbnails floor 200 MB, up to 1080p, wait | auto: free >= %d MB normal, >= %d MB "
         "light, else minimum | low-memory line under %d MB free\n",
         AUTO_NORMAL_MB, AUTO_LEVE_MB, APERTO_MB);
  fflush(stdout);
}
void tpk45_tick(int convertendo, const char *modo) {
  static Uint32 ultimo;
  static const char *const PAUSA[4] = { "working", "waiting for memory", "waiting for the converter", "idle" };
  Uint32 agora = SDL_GetTicks();
  DtsEngineRings rg;
  long rss, swap;
  int pausa;
  if (ultimo && (Sint32)(agora - ultimo) < 10000) return;
  ultimo = agora ? agora : 1u;
  tpk45_tv();
  rss = lerKb("/proc/self/status", "VmRSS");
  swap = lerKb("/proc/meminfo", "SwapFree");
  dts_engine_rings(&rg, 0);
  pausa = atomic_load(&miniPausa);
  printf("[tpk45] 10s: free %ld MB, swap free %ld MB, app %ld MB; converter %s%s%s, rings %d = %lld MiB, queue %lld of %d MiB; "
         "thumbnails %d of %d, %s; %s\n",
         livreMb(), swap < 0 ? -1 : swap / 1024, rss < 0 ? -1 : rss / 1024,
         convertendo ? "on" : "off", convertendo && modo ? ", " : "", convertendo && modo ? modo : "",
         rg.alive, rg.bytes >> 20, (long long)(dts_remux_queued_bytes() >> 20), dts_remux_queue_budget_mib(),
         atomic_load(&miniFeitas), atomic_load(&miniTotal), PAUSA[pausa >= 0 && pausa < 4 ? pausa : 3], tpk45_marca());
  fflush(stdout);
}
#endif
