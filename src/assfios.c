// Ver assfios.h.
#include "assfios.h"

#if defined(NV_TPK) && defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static long tid_atual(void) { return (long)syscall(SYS_gettid); }

void assfios_nomear(const char *nome) {
  char n[16];
  snprintf(n, sizeof n, "%s", nome ? nome : "");
  prctl(PR_SET_NAME, (unsigned long)n, 0, 0, 0);
}

void assfios_nome_trocar(const char *nome, char *antigo) {
  antigo[0] = 0;
  if (prctl(PR_GET_NAME, (unsigned long)antigo, 0, 0, 0) != 0) antigo[0] = 0;
  antigo[15] = 0;
  assfios_nomear(nome);
}

void assfios_nome_voltar(const char *antigo) {
  if (antigo && antigo[0]) assfios_nomear(antigo);
}

/* "run_ns wait_ns timeslices" (kernel com CONFIG_SCHED_INFO/SCHEDSTATS). */
static int ler_schedstat(int fd, unsigned long long *run, unsigned long long *espera) {
  char b[96];
  ssize_t n;
  if (fd < 0) return 0;
  n = pread(fd, b, sizeof b - 1, 0);
  if (n <= 0) return 0;
  b[n] = 0;
  return sscanf(b, "%llu %llu", run, espera) == 2;
}

static int abrir_schedstat(long tid) {
  char p[64];
  snprintf(p, sizeof p, "/proc/self/task/%ld/schedstat", tid);
  return open(p, O_RDONLY | O_CLOEXEC);
}

static long uiTid;   // escrito so pelo fio da UI; o relatorio (mesmo fio) le

/* Sem schedstat (a S95C, 25/10): o tempo de CPU do fio por /proc/.../stat,
 * utime + stime (campos 14 e 15, em ticks), depois do ultimo ')' do comm. */
static int ler_stat_ns(const char *p, unsigned long long *run) {
  char b[512], *f;
  unsigned long ut = 0, st = 0;
  int fd = open(p, O_RDONLY | O_CLOEXEC);
  ssize_t k;
  static long hz;
  if (fd < 0) return 0;
  k = read(fd, b, sizeof b - 1);
  close(fd);
  if (k <= 0) return 0;
  b[k] = 0;
  f = strrchr(b, ')');
  if (!f || sscanf(f + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2) return 0;
  if (!hz) { hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100; }
  *run = (unsigned long long)(ut + st) * (1000000000ULL / (unsigned long long)hz);
  return 1;
}

void assfios_ui_quadro(double *cpuMs, double *esperaMs) {
  static int fd = -2;
  static unsigned long long runAnt, espAnt;
  static double cpuAnt;
  unsigned long long run, esp;
  *cpuMs = 0; *esperaMs = -1;
  if (fd == -2) {
    uiTid = tid_atual();
    fd = abrir_schedstat(uiTid);
    if (!ler_schedstat(fd, &runAnt, &espAnt)) { if (fd >= 0) close(fd); fd = -1; }
    { struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts); cpuAnt = ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6; }
    return;
  }
  if (fd >= 0 && ler_schedstat(fd, &run, &esp)) {
    *cpuMs = (double)(run - runAnt) / 1e6;
    *esperaMs = (double)(esp - espAnt) / 1e6;
    runAnt = run; espAnt = esp;
  } else {
    struct timespec ts;
    double c;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    c = ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
    *cpuMs = c - cpuAnt;
    cpuAnt = c;
  }
}

// ---- relatorio de 10 s --------------------------------------------------------
#define FIOS_MAX 160
#define GRUPOS_MAX 24
#define NUCLEOS_MAX 16
typedef struct { long tid; unsigned long long run, esp; } Fio;
typedef struct { char nome[16]; int n; unsigned long long run, esp; } Grupo;
static Fio ant[FIOS_MAX];
static int nAnt, temAnt;
static struct timespec tAnt;
static unsigned long long cpuOcup[NUCLEOS_MAX], cpuTotal[NUCLEOS_MAX];
static int nNucleosAnt;

static int ler_texto(const char *p, char *b, size_t n) {
  int fd = open(p, O_RDONLY | O_CLOEXEC);
  ssize_t k;
  if (fd < 0) return 0;
  k = read(fd, b, n - 1);
  close(fd);
  if (k <= 0) return 0;
  b[k] = 0;
  return 1;
}

/* Carga de cada nucleo (toda a TV, nao so o app) desde o relatorio anterior. */
static int nucleos_carga(int *pct, int max) {
  static char b[4096];
  unsigned long long ocup[NUCLEOS_MAX], tot[NUCLEOS_MAX];
  int n = 0, i, primeira = !nNucleosAnt;
  char *l;
  if (!ler_texto("/proc/stat", b, sizeof b)) return 0;
  for (l = b; l && *l && n < NUCLEOS_MAX; l = strchr(l, '\n'), l = l ? l + 1 : NULL) {
    unsigned long long u, ni, s, id, io, irq, sirq, st;
    int c;
    if (strncmp(l, "cpu", 3) || l[3] < '0' || l[3] > '9') continue;
    u = ni = s = id = io = irq = sirq = st = 0;
    if (sscanf(l, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &c, &u, &ni, &s, &id, &io, &irq, &sirq, &st) < 5) continue;
    ocup[n] = u + ni + s + irq + sirq + st;
    tot[n] = ocup[n] + id + io;
    n++;
  }
  for (i = 0; i < n && i < max; i++) {
    unsigned long long dT = tot[i] - (primeira || i >= nNucleosAnt ? 0 : cpuTotal[i]);
    unsigned long long dO = ocup[i] - (primeira || i >= nNucleosAnt ? 0 : cpuOcup[i]);
    pct[i] = dT ? (int)(100.0 * (double)dO / (double)dT + 0.5) : 0;
  }
  memcpy(cpuOcup, ocup, sizeof ocup); memcpy(cpuTotal, tot, sizeof tot);
  nNucleosAnt = n;
  return primeira ? -n : n;
}

static Grupo *grupo(Grupo *g, int *ng, const char *nome) {
  int i;
  for (i = 0; i < *ng; i++) if (!strcmp(g[i].nome, nome)) return &g[i];
  if (*ng >= GRUPOS_MAX) return &g[GRUPOS_MAX - 1];   // o ultimo junta o resto
  memset(&g[*ng], 0, sizeof g[*ng]);
  snprintf(g[*ng].nome, sizeof g[*ng].nome, "%.15s", *ng == GRUPOS_MAX - 1 ? "others" : nome);
  return &g[(*ng)++];
}

static int cmp_grupo(const void *a, const void *b) {
  const Grupo *x = a, *y = b;
  return x->run < y->run ? 1 : x->run > y->run ? -1 : 0;
}

void assfios_relatorio(void) {
  Fio agora[FIOS_MAX];
  Grupo g[GRUPOS_MAX];
  int nAgora = 0, ng = 0, i, j, pct[NUCLEOS_MAX], nn, temEspera = 0, lidos = 0;
  DIR *d = opendir("/proc/self/task");
  struct dirent *e;
  struct timespec t;
  double janela;
  char linha[1200];
  size_t o = 0;
  if (!d) return;
  clock_gettime(CLOCK_MONOTONIC, &t);
  janela = temAnt ? (double)(t.tv_sec - tAnt.tv_sec) + (double)(t.tv_nsec - tAnt.tv_nsec) / 1e9 : 0;
  while ((e = readdir(d)) != NULL && nAgora < FIOS_MAX) {
    char p[64], nome[32] = "?", st[96];
    long tid = strtol(e->d_name, NULL, 10);
    unsigned long long run = 0, esp = 0, runAnt = 0, espAnt = 0;
    Grupo *gr;
    if (tid <= 0) continue;
    snprintf(p, sizeof p, "/proc/self/task/%ld/schedstat", tid);
    if (ler_texto(p, st, sizeof st) && sscanf(st, "%llu %llu", &run, &esp) == 2) temEspera = 1;
    else {
      snprintf(p, sizeof p, "/proc/self/task/%ld/stat", tid);
      if (!ler_stat_ns(p, &run)) continue;
      esp = 0;
    }
    lidos++;
    agora[nAgora].tid = tid; agora[nAgora].run = run; agora[nAgora].esp = esp; nAgora++;
    if (!temAnt) continue;
    for (j = 0; j < nAnt; j++) if (ant[j].tid == tid) { runAnt = ant[j].run; espAnt = ant[j].esp; break; }
    if (run <= runAnt && esp <= espAnt) continue;   // parado a janela toda
    if (tid == uiTid) snprintf(nome, sizeof nome, "ui");
    else {
      snprintf(p, sizeof p, "/proc/self/task/%ld/comm", tid);
      if (ler_texto(p, nome, sizeof nome)) { char *nl = strchr(nome, '\n'); if (nl) *nl = 0; }
    }
    gr = grupo(g, &ng, nome);
    gr->n++; gr->run += run - runAnt; gr->esp += esp - espAnt;
  }
  closedir(d);
  memcpy(ant, agora, (size_t)nAgora * sizeof agora[0]); nAnt = nAgora;
  nn = nucleos_carga(pct, NUCLEOS_MAX);
  tAnt = t;
  if (!temAnt) { temAnt = 1; return; }   // a primeira janela so guarda a base
  if (!lidos) return;
  qsort(g, (size_t)ng, sizeof g[0], cmp_grupo);
  o += (size_t)snprintf(linha + o, sizeof linha - o, temEspera ? "[assperf] threads %.0fs (on a core / waiting for a core, s):"
                                                               : "[assperf] threads %.0fs (on a core, s; this TV has no wait stats):", janela);
  if (nn > 0) {
    o += (size_t)snprintf(linha + o, sizeof linha - o, " cores %d busy", nn);
    for (i = 0; i < nn && o < sizeof linha; i++)
      o += (size_t)snprintf(linha + o, sizeof linha - o, "%s%d", i ? "/" : " ", pct[i]);
    if (o < sizeof linha) o += (size_t)snprintf(linha + o, sizeof linha - o, " %% (whole TV) |");
  }
  /* ui e os nossos fios primeiro, depois os 5 que mais usaram CPU. */
  { static const char *const nossos[3] = { "ui", "nv-assrender", "nv-assblur" };
    int k, mostrados = 0;
    for (k = 0; k < 3; k++)
      for (i = 0; i < ng; i++)
        if (!strcmp(g[i].nome, nossos[k]) && o < sizeof linha) {
          o += temEspera ? (size_t)snprintf(linha + o, sizeof linha - o, " %s%s%.0d: %.2f/%.2f", g[i].nome, g[i].n > 1 ? " x" : "",
                                            g[i].n > 1 ? g[i].n : 0, g[i].run / 1e9, g[i].esp / 1e9)
                         : (size_t)snprintf(linha + o, sizeof linha - o, " %s%s%.0d: %.2f", g[i].nome, g[i].n > 1 ? " x" : "",
                                            g[i].n > 1 ? g[i].n : 0, g[i].run / 1e9);
          g[i].n = -1;
        }
    for (i = 0; i < ng && mostrados < 5 && o < sizeof linha; i++) {
      if (g[i].n < 0) continue;
      o += temEspera ? (size_t)snprintf(linha + o, sizeof linha - o, " | %s%s%.0d: %.2f/%.2f", g[i].nome, g[i].n > 1 ? " x" : "",
                                        g[i].n > 1 ? g[i].n : 0, g[i].run / 1e9, g[i].esp / 1e9)
                     : (size_t)snprintf(linha + o, sizeof linha - o, " | %s%s%.0d: %.2f", g[i].nome, g[i].n > 1 ? " x" : "",
                                        g[i].n > 1 ? g[i].n : 0, g[i].run / 1e9);
      mostrados++;
    } }
  printf("%s\n", linha);
}

#else
void assfios_nomear(const char *nome) { (void)nome; }
void assfios_nome_trocar(const char *nome, char *antigo) { (void)nome; if (antigo) antigo[0] = 0; }
void assfios_nome_voltar(const char *antigo) { (void)antigo; }
void assfios_ui_quadro(double *cpuMs, double *esperaMs) { *cpuMs = 0; *esperaMs = -1; }
void assfios_relatorio(void) {}
#endif
