#include "assrender.h"
#include "legfontes.h"

#include <pthread.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include <dirent.h>
#include <sys/stat.h>

/* A PASTA DE FONTES DO SISTEMA NAO E SO DE FONTES. Na C9, /usr/share/fonts
 * traz tabelas do firmware (arib_mrg_v5-10.bin e parecidas) ao lado das .ttf.
 * ass_set_fonts_dir entregava TUDO ao libass como fonte em memoria, e cada
 * arquivo que nao e fonte virava "[libass] Error opening memory font" no log
 * — alem de ser lido inteiro para a memoria por nada. Aqui a pasta e lida
 * pelo app, com o mesmo criterio do libass (arquivo regular, sem ponto no
 * comeco, sem descer em subpasta), mas so passa o que tem assinatura de fonte
 * TrueType/OpenType: 00 01 00 00, "OTTO", "true", "typ1" ou colecao "ttcf". */
int assrender_bytes_sao_fonte(const void *dados, size_t n) {
  const unsigned char *p = (const unsigned char *)dados;
  if (!p || n < 4) return 0;
  return (p[0] == 0 && p[1] == 1 && p[2] == 0 && p[3] == 0) ||
         !memcmp(p, "OTTO", 4) || !memcmp(p, "true", 4) ||
         !memcmp(p, "typ1", 4) || !memcmp(p, "ttcf", 4);
}

int assrender_ler_pasta_fontes(const char *dir,
                               void (*cb)(const char *nome, const void *dados,
                                          size_t tam, void *u),
                               void *u, int *ignorados) {
  DIR *d;
  struct dirent *e;
  int lidas = 0, fora = 0;
  if (ignorados) *ignorados = 0;
  if (!dir || !*dir || !(d = opendir(dir))) return 0;
  while ((e = readdir(d)) != NULL) {
    char caminho[1024];
    struct stat st;
    unsigned char cab[4];
    FILE *f;
    char *buf;
    if (e->d_name[0] == '.') continue;
    snprintf(caminho, sizeof caminho, "%s/%s", dir, e->d_name);
    if (stat(caminho, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 4 ||
        (unsigned long long)st.st_size > (unsigned long long)INT_MAX) continue;
    f = fopen(caminho, "rb");
    if (!f) continue;
    if (fread(cab, 1, 4, f) != 4 || !assrender_bytes_sao_fonte(cab, 4)) {
      fclose(f); fora++; continue;
    }
    buf = malloc((size_t)st.st_size);
    if (buf && fseek(f, 0, SEEK_SET) == 0 &&
        fread(buf, 1, (size_t)st.st_size, f) == (size_t)st.st_size) {
      if (cb) cb(e->d_name, buf, (size_t)st.st_size, u);
      lidas++;
    }
    free(buf);
    fclose(f);
  }
  closedir(d);
  if (ignorados) *ignorados = fora;
  return lidas;
}

/* Pasta das fontes do app que o libass le (Noto Naskh incluso). `wasm` e o
 * __EMSCRIPTEN__: la SDL_GetBasePath devolve "/" e as fontes vivem em /app/fonts. */
void assrender_pasta_fontes_app(const char *base, int wasm, char *out, size_t n) {
  if (n == 0) return;
  out[0] = 0;
  if (wasm) { snprintf(out, n, "%s", "/app/fonts"); return; }
  if (base) snprintf(out, n, "%sfonts", base);
  if (access(out, R_OK)) snprintf(out, n, "%s", "deploy/app/fonts");
}

/* #369: o libass nao cai para "qualquer fonte com o glifo" quando as fontes sao as
 * da memoria (sem fontconfig: Android, .tpk, wasm): o tailandes de uma trilha
 * ASS com estilo "Arial"/Inter saia em .notdef mesmo com o NotoSansThai ja
 * entregue. Cada corrida de tailandes (U+0E00-0E7F, E0 B8/B9 xx) fora de chaves
 * de override ganha {\fnNoto Sans Thai} ... {\fn}, o mesmo expediente do texto
 * simples arabe (plainass.c). So corre se a fonte chegou ao libass. */
static int ass_byte_tailandes(const unsigned char *p) {
  return p[0] == 0xE0 && (p[1] == 0xB8 || p[1] == 0xB9) && (p[2] & 0xC0) == 0x80;
}
char *assrender_marcar_tailandes(const char *txt) {
  static const char ini[] = "{\\fnNoto Sans Thai}", fim[] = "{\\fn}";
  size_t n = strlen(txt), cap = n + 1, i, o = 0;
  int achou = 0, chaves = 0, em = 0;
  char *out;
  for (i = 0; i + 2 < n; i++)
    if (ass_byte_tailandes((const unsigned char *)txt + i)) { achou = 1; cap += sizeof ini + sizeof fim; i += 2; }
  if (!achou) return NULL;
  if (!(out = (char *)malloc(cap))) return NULL;
  for (i = 0; i < n;) {
    if (txt[i] == '{') chaves = 1;
    if (txt[i] == '}') chaves = 0;
    if (!chaves && i + 2 < n && ass_byte_tailandes((const unsigned char *)txt + i)) {
      if (!em) { memcpy(out + o, ini, sizeof ini - 1); o += sizeof ini - 1; em = 1; }
      memcpy(out + o, txt + i, 3); o += 3; i += 3; continue;
    }
    if (em) { memcpy(out + o, fim, sizeof fim - 1); o += sizeof fim - 1; em = 0; }
    out[o++] = txt[i++];
  }
  if (em) { memcpy(out + o, fim, sizeof fim - 1); o += sizeof fim - 1; }
  out[o] = 0;
  return out;
}

#ifdef NV_ASS_LIBASS
#include <SDL2/SDL.h>
#include <ass/ass.h>
#include "gfx.h"
#include "bidi.h"
#include "assperf.h"
/* Blur na GPU (.tpk, libass 0005): o libass entrega o nitido + a receita e o
 * fio GL borra (assgpublur.c). */
#if defined(NV_TPK) && defined(ASS_NV_API_DEFER)
#define ASS_GPU 1
#include "assgpublur.h"
#else
#define ASS_GPU 0
#endif
#if !defined(__EMSCRIPTEN__)
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

#define ASS_TEX_INICIAL 64

typedef struct {
  GLuint tex;
  int w, h;
} AssTex;

#if ASS_GPU
/* Bitmap nitido inteiro de um letreiro adiado (copia compacta, stride = w),
 * uma vez por quadro mesmo quando varias imagens saem dele (\kf, recortes). */
typedef struct {
  unsigned char *buf;
  int w, h;
  unsigned long long chave;
  const unsigned char *orig;   // ponteiro do libass (so para achar repetidas na copia)
} AssGpuFonte;
/* A receita do libass (ASS_NvDefer) com indices nas fontes do quadro. */
typedef struct {
  int fonte, fill;             // fill -1 = sem fix
  double r2x, r2y;
  int subX, subY, fix, fillDx, fillDy, shiftX, shiftY;
  int coreX, coreY, coreW, coreH;
} AssGpuRec;
#endif

typedef struct {
  int x, y, w, h, stride, type;
  unsigned color;
  unsigned char *bitmap;      // NULL numa imagem adiada (gpu): sai da receita
  unsigned long long chave;   // assperf: conteudo (bitmap + w + h); 0 = nao calculada
#if ASS_GPU
  int gpu;
  AssGpuRec g;
#endif
} AssCpuImage;

typedef struct {
  AssCpuImage *images;
  int count;
  unsigned generation, serial;
  size_t bytes;
  long long ms;          // instante (relogio do video) que este quadro mostra
  long long inicioMax;   // maior Start dos eventos vivos em ms; -1 sem evento
  float ox, oy;          // canto da area do video na tela quando foi renderizado
  int k;                 // pixels do alvo por unidade de layout: o quadro foi renderizado k vezes maior (#335)
  float esc;             // assperf: pixels do quadro por unidade de layout (k x resolucao); 0 = k
#if ASS_GPU
  AssGpuFonte *fontes;   // dos letreiros adiados (blur na GPU)
  int nfontes;
#endif
} AssCpuFrame;

static pthread_mutex_t assTrava = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t assFilaTrava = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t assFilaCond = PTHREAD_COND_INITIALIZER;
static pthread_t assWorker;
static int assWorkerCriado, assTrackAtivo, assWorkerParar, assPedidoPendente;
static int assTextoSimples;
static PlainAssStyle plainPed, plainApl;
static int plainPedValido, plainAplValido;
static ASS_Style *authoredStyles;
static int authoredStyleCount;

static void ass_authored_styles_free(void) {
  for (int i = 0; i < authoredStyleCount; i++) free(authoredStyles[i].FontName);
  free(authoredStyles); authoredStyles = NULL; authoredStyleCount = 0;
}
/* Protected by assTrava. Re-evaluate active cues only at a timing boundary
 * or after seeking, not on every video frame. */
static int plainTimingValid;
static long long plainLastTime, plainNextBoundary;
static uint64_t plainActiveSet;
static int assAtualValido;
static double assPedidoMs;
static unsigned assPedidoGeracao, assPedidoSerial, assSerial;
static unsigned assEpoch;
/* FILA DOS QUADROS PRONTOS (assperf, render ahead). Era um quadro so
 * (assPronto): o novo apagava o anterior, e o desenho mostrava o que chegasse.
 * Com "Hold until due" o quadro renderizado para daqui a 80 ms espera a sua
 * vez; enquanto isso o worker ja faz os seguintes, entao cabe mais de um.
 * Sem hold a fila tem teto 1 e e o comportamento de sempre. Sob assFilaTrava,
 * em ordem de pedido (ms crescente dentro de um epoch). */
#define ASS_FILA_MAX 4
#define ASS_FILA_BYTES (24u * 1024u * 1024u)
static AssCpuFrame assFila[ASS_FILA_MAX];
static int assFilaN;
static int assFilaTeto = 1;      // 1 = como antes; ASS_FILA_MAX com hold
static AssCpuFrame assAtual;
static double assUltimoRenderPuroMs;   // assperf: render sem a espera da trava (sob assFilaTrava)
/* assperf "Render only on changes": o ultimo render vale para [de, ate) — nenhum
 * evento comeca, acaba ou anima nesse trecho. Sob assFilaTrava. */
static long long assValidoDe = LLONG_MAX, assValidoAte = LLONG_MIN;
static double assUltimoVistoMs;      // o ultimo instante que o desenho pediu OU pulou
static unsigned assUltimoVistoGeracao;
static unsigned assValidoEpoch, assValidoGeracao;
static double assUltimoPedidoMs;
static unsigned assUltimoPedidoGeracao;
static int assTemUltimoPedido;
static int assEventos, assFontes, assResolucaoFonte;
static long long assCoberturaIni, assCoberturaFim;
static ASS_Library *assLib;
static ASS_Renderer *assRenderer;
static ASS_Track *assTrack;
static unsigned assTrackGeracao;
static AssTex *assTex;
static int assTexCap;
static int assTexResetar;
static int assFrameW, assFrameH;
/* AREA DO VIDEO. O libass desenhava num quadro 1920x1080 fixo, como se o video
 * ocupasse a tela inteira: num 4:3 uma placa em \pos(64,48) caia em x=195,
 * DENTRO da barra preta (o video comeca em x=240); num 2.39:1 caia na barra de
 * cima e 33 % maior. O quadro do libass agora e o retangulo em que o video
 * aparece, e o storage e o tamanho do quadro decodificado — o que o mpv faz e
 * o que o ass.js do app web faz com resampling "video_height".
 * O fio grafico PEDE (layPed*, sob assFilaTrava) e o worker APLICA antes do
 * proximo render (layApl*, sob assTrava): o fio grafico nunca espera um
 * ass_render_frame para mudar o layout. */
static float layPedX, layPedY;
static int layPedW = 1920, layPedH = 1080, layPedVW = 1920, layPedVH = 1080;
static double layPedEscala = 1.0;
static int layPedK = 1;
static int layAplW, layAplH, layAplVW, layAplVH;
static double layAplEscala;
static unsigned assGeracao;
static int assCorAtiva, assCorR, assCorG, assCorB;
static unsigned long long assUltimoRenderUs;
static size_t assBytesQuadro;
/* Quadro PUBLICADO por ultimo (epoch + geracao): com o `changed` do libass em
 * zero, o quadro novo e identico a ele e nao precisa ser copiado, publicado
 * nem reenviado a GPU. */
static unsigned assProduzidoEpoch, assProduzidoGeracao;
static int assProduziu;
/* Textura: serial do quadro cujas imagens estao nas texturas e a cor usada.
 * Reenviar tudo a cada quadro (o que se fazia) custa a conversao RGBA de
 * todos os glifos 60 vezes por segundo. */
static unsigned assTexSerial;
static int assTexCorChave = -1;
/* Medidas (#92): o dono ve "atrasada e piscando"; o log precisa dizer quanto. */
static struct {
  long quadros, trocas, renders, iguais, piscas, vaziosPisca, falas;
  unsigned long long renderSomaUs, renderMaxUs;
  long long atrasoSoma, atrasoMax;
  int textoAnterior, vazios;
  long long ultimoTextoMs, ultimoInicioLogado;
  double ultimoRelatorio;
} assMed;
static pthread_mutex_t assDiagTrava = PTHREAD_MUTEX_INITIALIZER;
static char assDiag[160] = "libass pronto";
static int assGlifoMsgs;   // assperf: mensagens de glifo ausente desta faixa (sob assTrava)

static void ass_diag(const char *s) {
  pthread_mutex_lock(&assDiagTrava);
  snprintf(assDiag, sizeof assDiag, "%s", s ? s : "");
  pthread_mutex_unlock(&assDiagTrava);
}

static void ass_mensagem(int nivel, const char *fmt, va_list args, void *dados) {
  char linha[768];
  (void)dados;
  vsnprintf(linha, sizeof linha, fmt, args);
  if (strstr(linha, "fontselect:")) assResolucaoFonte++;
  /* So ate MSGL_INFO (4), o mesmo corte do callback padrao do libass (que
   * para em 5). Os niveis 6-7 sao depuracao por quadro e por glifo. */
  if (nivel > 4) return;
#ifdef NV_TPK
  /* assperf (log limpo): o laco "Glyph ... not found / selecting one more
   * font / Trying all charmaps" e tres linhas POR GLIFO (60+ no log de
   * 23/10). Ficam as 6 primeiras por faixa; o resto vira uma contagem. */
  if (assperf_get(AP_LOG) >= 1 &&
      (strstr(linha, "Glyph 0x") || strstr(linha, "already is available"))) {
    if (++assGlifoMsgs > 6) return;
  }
#endif
  fprintf(stderr, "[libass] %s\n", linha);
}

/* FONTES ANEXADAS JA ENTREGUES AO libass (#92, queda na C9 em 22/09/2026).
 *
 * O libass NAO aceita esvaziar a lista de fontes e seguir renderizando.
 * ass_start_frame guarda quantas fontes ja passou ao fontselect
 * (num_emfonts) e, quando a biblioteca muda de tamanho, afirma que so CRESCEU:
 *   assert(library->num_fontdata > num_emfonts)
 * ass_clear_fonts zera num_fontdata e deixa num_emfonts como estava; a proxima
 * fonte anexada (ou nenhuma) deixa a conta menor, e o quadro seguinte ABORTA o
 * app. MEDIDO: tests/ass_fontes.sh reproduz com o libass 0.17.5 do Homebrew, e
 * o log da C9 morre em "ass_render.c:3069: ass_start_frame: Assertion" logo
 * depois da SEGUNDA entrega do mkvass — que e quem chamava
 * assrender_limpar_fontes a cada lote.
 *
 * O remedio "limpar e chamar ass_set_fonts de novo" zera a conta, mas o
 * fontselect novo rele a pasta de fontes: na C9 e /usr/share/fonts, 106 MB e
 * 29 arquivos carregados na memoria — a cada lote do mkvass e a cada troca de
 * faixa. Entao a lista SO CRESCE: cada fonte entra uma vez (nome + tamanho +
 * amostra do conteudo) e fica para as proximas faixas. So quando o acumulado
 * passa do teto e que a biblioteca e reiniciada por inteiro, com ass_set_fonts
 * logo depois do ass_clear_fonts — o caro fica no caso raro. */
typedef struct { char nome[96]; size_t tam; unsigned long long amostra; } AssFonteVista;
#define ASS_FONTES_MAX 192
#define ASS_FONTES_TETO_BYTES (96u * 1024u * 1024u)
static AssFonteVista assFontesVistas[ASS_FONTES_MAX];
static int assNFontesVistas;
static size_t assBytesFontesVistas;
static char assFallbackFont[768], assFonteLocal[768];
static int assFonteRegistrada;
#define ASS_ATUAIS_MAX 32
#define ASS_ATUAIS_BYTES (32u * 1024u * 1024u)
typedef struct { char nome[256]; unsigned char *dados; size_t tam; } AssFonteAtual;
static AssFonteAtual assFontesAtuais[ASS_ATUAIS_MAX];
static int assNFontesAtuais, assAtuaisCompletas = 1;
static size_t assBytesAtuais;
static void ass_atuais_limpar_locked(void) {
  for (int i = 0; i < assNFontesAtuais; i++) free(assFontesAtuais[i].dados);
  memset(assFontesAtuais, 0, sizeof assFontesAtuais);
  assNFontesAtuais = 0; assBytesAtuais = 0; assAtuaisCompletas = 1;
}
static void ass_atual_guardar_locked(const char *nome, const void *dados, size_t n) {
  if (!assAtuaisCompletas) return;
  for (int i = 0; i < assNFontesAtuais; i++) {
    const AssFonteAtual *f = &assFontesAtuais[i];
    if (f->tam == n && !strcmp(f->nome, nome) && !memcmp(f->dados, dados, n)) return;
  }
  unsigned char *copia = NULL;
  if (strlen(nome) < sizeof assFontesAtuais[0].nome && assNFontesAtuais < ASS_ATUAIS_MAX &&
      n <= ASS_ATUAIS_BYTES - assBytesAtuais) copia = malloc(n);
  if (!copia) {
    ass_atuais_limpar_locked(); assAtuaisCompletas = 0;
    return; /* Keep the original renderer path; never replay a partial batch. */
  }
  AssFonteAtual *f = &assFontesAtuais[assNFontesAtuais++];
  memcpy(copia, dados, n); f->dados = copia; f->tam = n;
  snprintf(f->nome, sizeof f->nome, "%s", nome); assBytesAtuais += n;
}
static unsigned char *ass_fonte_local_ler(const char *path, size_t *n) {
  struct stat st; *n = 0;
  if (lstat(path, &st) || !S_ISREG(st.st_mode) || st.st_size < 12 ||
      (unsigned long long)st.st_size > LEGFONTE_MAX_BYTES) return NULL;
  FILE *f = fopen(path, "rb"); if (!f) return NULL;
  unsigned char *b = malloc((size_t)st.st_size);
  if (b && fread(b, 1, (size_t)st.st_size, f) == (size_t)st.st_size &&
      fgetc(f) == EOF && !ferror(f) && assrender_bytes_sao_fonte(b, (size_t)st.st_size)) *n = (size_t)st.st_size;
  fclose(f);
  if (!*n) { free(b); b = NULL; }
  return b;
}

static unsigned long long ass_amostra_fonte(const unsigned char *p, size_t n) {
  unsigned long long h = 1469598103934665603ULL;
  size_t i, pedaco = n < 65536u ? n : 65536u;
  for (i = 0; i < pedaco; i++) { h ^= p[i]; h *= 1099511628211ULL; }
  for (i = n - pedaco; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
  return h;
}

/* Pasta das fontes do sistema, lida por assrender_ler_pasta_fontes em vez de
 * ass_set_fonts_dir. Guardada porque ass_clear_fonts (teto de fontes anexadas)
 * tira estas junto, e elas precisam voltar antes do ass_set_fonts seguinte. */
static char assPastaFontes[640], assPastaFontesApp[640];
static int assUsaSistema;
static int assVistoNaskh;          /* NotoNaskhArabic-Regular chegou ao libass nesta carga */
static int assNaskhBold;           /* NotoNaskhArabic-Bold chegou ao libass nesta carga (#335) */
static int assVistoThai;           /* NotoSansThai-Regular chegou ao libass nesta carga (#369) */
static char assPastaExtra[640];    /* NV_TPK: pasta de fontes baixadas (dados/fontes) */
static void ass_fonte_da_pasta(const char *nome, const void *dados, size_t tam, void *u) {
  (void)u;
  if (!strcmp(nome, "NotoNaskhArabic-Regular.ttf")) assVistoNaskh = 1;
  if (!strcmp(nome, "NotoNaskhArabic-Bold.ttf")) assNaskhBold = 1;
  if (!strcmp(nome, "NotoSansThai-Regular.ttf")) assVistoThai = 1;
  /* The UI subset shares the family name but lacks mixed-script glyphs.
   * Plain subtitles use the verified complete face; leave UI selection intact. */
  if (!strcmp(nome, "NotoNaskhArabic-Subset.ttf")) return;
  ass_add_font(assLib, nome, (const char *)dados, (int)tam);
}
#ifdef NV_TPK
static int assNaskhFalhou;         /* a completa nao veio: vale o recorte da interface */
static void ass_subset_naskh_locked(void) {
  /* Sem a completa (sem rede, tag ausente) o recorte da interface ainda tem as
   * letras: sem juncao, mas legiveis, em vez de quadrados. */
  char r[700]; FILE *f; long n = 0; char *b;
  if (!assLib || !assPastaFontesApp[0]) return;
  snprintf(r, sizeof r, "%s/NotoNaskhArabic-Subset.ttf", assPastaFontesApp);
  if (!(f = fopen(r, "rb"))) return;
  fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
  if (n > 4 && n < (1L << 22) && (b = malloc((size_t)n))) {
    if (fread(b, 1, (size_t)n, f) == (size_t)n) {
      ass_add_font(assLib, "NotoNaskhArabic-Subset.ttf", b, (int)n);
      fprintf(stderr, "[libass] sem NotoNaskhArabic-Regular: usando o recorte da interface\n");
    }
    free(b);
  }
  fclose(f);
}
#endif
static void ass_carregar_pasta_locked(void) {
  int ignorados = 0, lidas;
  if (!assLib || !assPastaFontes[0]) return;
  assNaskhBold = 0;
  assVistoThai = 0;
  lidas = assUsaSistema ? assrender_ler_pasta_fontes(assPastaFontes, ass_fonte_da_pasta, NULL, &ignorados) : 0;
  assVistoNaskh = 0;
  if (assPastaFontesApp[0] && (!assUsaSistema || strcmp(assPastaFontesApp, assPastaFontes)))
    assrender_ler_pasta_fontes(assPastaFontesApp, ass_fonte_da_pasta, NULL, NULL);
#ifdef NV_TPK
  /* O res/ do .tpk e o da INSTALACAO (a auto-atualizacao troca so a .so), entao
   * o NotoNaskhArabic-Regular.ttf, novo na 2.0.2, falta em quem atualizou por
   * dentro do app: o libass nao achava "Noto Naskh Arabic" e caia na Inter, que
   * nao tem arabe (quadradinhos). A copia baixada da tag fica em dados/fontes. */
  if (assPastaExtra[0]) assrender_ler_pasta_fontes(assPastaExtra, ass_fonte_da_pasta, NULL, NULL);
  if (!assVistoNaskh && assNaskhFalhou) ass_subset_naskh_locked();
#endif
  plainass_arabic_bold = assNaskhBold;
  fprintf(stderr, "[libass] pasta %s: %d fonte(s); %d arquivo(s) que nao sao fonte ignorado(s)\n",
          assPastaFontes, lidas, ignorados);
}

static unsigned assFontesVersao;   // muda a cada ass_set_fonts (o bench confere)
static void ass_aplicar_fontes_locked(void) {
  if (!assRenderer) return;
  assFontesVersao++;
  ass_set_fonts(assRenderer, assFallbackFont[0] ? assFallbackFont : NULL, "Arial",
                ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
}

// Explicit selection changes only. Routine packet delivery keeps the original
// font cache path; a full current batch lets us restore attachment precedence.
static void ass_fontes_reconstruir_locked(const unsigned char *selecionada, size_t n) {
  ass_clear_fonts(assLib);
  if (selecionada) ass_add_font(assLib, assFonteLocal, (const char *)selecionada, (int)n);
  assFonteRegistrada = selecionada != NULL;
  ass_carregar_pasta_locked();
  assNFontesVistas = 0; assBytesFontesVistas = 0;
  for (int i = 0; i < assNFontesAtuais; i++) {
    const AssFonteAtual *f = &assFontesAtuais[i];
    ass_add_font(assLib, f->nome, (const char *)f->dados, (int)f->tam);
    AssFonteVista *v = &assFontesVistas[assNFontesVistas++];
    snprintf(v->nome, sizeof v->nome, "%.95s", f->nome);
    v->tam = f->tam; v->amostra = ass_amostra_fonte(f->dados, f->tam);
    assBytesFontesVistas += f->tam;
  }
  ass_aplicar_fontes_locked();
}

static void ass_iniciar_locked(int sistema) {
  char fontDir[640] = "";
  char fallbackFont[768] = "";
  struct timespec t0, t1;
  if (assLib) {
    if (sistema && !assUsaSistema) {
      assUsaSistema = 1; ass_carregar_pasta_locked(); ass_aplicar_fontes_locked();
    }
    return;
  }
  assUsaSistema = sistema;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  assLib = ass_library_init();
  if (!assLib) { ass_diag("libass: falha ao iniciar biblioteca"); return; }
  ass_set_message_cb(assLib, ass_mensagem, NULL);
  ass_set_extract_fonts(assLib, 1);
  assRenderer = ass_renderer_init(assLib);
  if (!assRenderer) {
    ass_library_done(assLib); assLib = NULL;
    ass_diag("libass: falha ao iniciar renderer"); return;
  }
  ass_set_shaper(assRenderer, ASS_SHAPING_COMPLEX);
  /* Fontes anexadas continuam tendo prioridade. A pasta da plataforma ajuda
   * o libass a resolver familias instaladas; Inter, distribuida com o app,
   * evita texto vazio quando a familia pedida nao existe. */
#ifdef __EMSCRIPTEN__
  snprintf(fontDir, sizeof fontDir, "%s", "/usr/share/fonts");
  snprintf(fallbackFont, sizeof fallbackFont, "%s", "/app/fonts/InterDisplay-Regular.ttf");
  if (access(fontDir, R_OK) != 0) snprintf(fontDir, sizeof fontDir, "%s", "/app/fonts");
#else
  {
    char *base = SDL_GetBasePath();
    if (base) {
      snprintf(fontDir, sizeof fontDir, "%sfonts", base);
      snprintf(fallbackFont, sizeof fallbackFont, "%sfonts/InterDisplay-Regular.ttf", base);
      SDL_free(base);
    }
    if (access(fontDir, R_OK) != 0)
      snprintf(fontDir, sizeof fontDir, "%s", "deploy/app/fonts");
    if (access(fallbackFont, R_OK) != 0)
      snprintf(fallbackFont, sizeof fallbackFont, "%s", "deploy/app/fonts/InterDisplay-Regular.ttf");
#ifdef NV_ANDROID
    /* Android: a pasta de fontes e a que o NuvioActivity extraiu (<arte>/../fonts,
     * ja achada acima). Nao ha /usr/share/fonts; o sistema entra so como
     * ultimo recurso para a fonte de reserva. */
    {
      const char *env = getenv("NUVIO_ARTE");
      if (env && env[0]) {
        char r[512];
        snprintf(r, sizeof r, "%s/../fonts", env);
        if (access(r, R_OK) == 0) {
          snprintf(fontDir, sizeof fontDir, "%s", r);
          snprintf(fallbackFont, sizeof fallbackFont, "%s/InterDisplay-Regular.ttf", r);
        }
      }
    }
    if (access(fallbackFont, R_OK) != 0 && access("/system/fonts/Roboto-Regular.ttf", R_OK) == 0)
      snprintf(fallbackFont, sizeof fallbackFont, "%s", "/system/fonts/Roboto-Regular.ttf");
#elif defined(NV_TPK)
    /* .tpk (Samsung): SDL_GetBasePath e o diretorio do host .NET, nao o do
     * pacote. O host passa a arte em NUVIO_TPK_ARTE (src/tpk.c) e fonts/ fica
     * ao lado de art/ (a mesma conta do txt_iniciar). Sem fontconfig e sem
     * pasta de sistema: as fontes sao as do app, entregues ao libass em memoria. */
    {
      const char *env = getenv("NUVIO_TPK_ARTE");
      if (env && env[0]) {
        char r[640];
        snprintf(r, sizeof r, "%s/../fonts", env);
        if (access(r, R_OK) == 0) {
          snprintf(fontDir, sizeof fontDir, "%s", r);
          snprintf(fallbackFont, sizeof fallbackFont, "%s/InterDisplay-Regular.ttf", r);
        }
      }
    }
#else
    if (access("/usr/share/fonts", R_OK) == 0)
      snprintf(fontDir, sizeof fontDir, "%s", "/usr/share/fonts");
    if (access(fallbackFont, R_OK) != 0 && access("/usr/share/fonts/LG_Display-Regular.ttf", R_OK) == 0)
      snprintf(fallbackFont, sizeof fallbackFont, "%s", "/usr/share/fonts/LG_Display-Regular.ttf");
    if (access(fallbackFont, R_OK) != 0 && access("/usr/share/fonts/DroidSans.ttf", R_OK) == 0)
      snprintf(fallbackFont, sizeof fallbackFont, "%s", "/usr/share/fonts/DroidSans.ttf");
#endif
  }
#endif
  { char *base = SDL_GetBasePath();
#ifdef __EMSCRIPTEN__
    assrender_pasta_fontes_app(base, 1, assPastaFontesApp, sizeof assPastaFontesApp);
#else
    assrender_pasta_fontes_app(base, 0, assPastaFontesApp, sizeof assPastaFontesApp);
#endif
    if (base) SDL_free(base);
#ifdef NV_TPK
    if (fontDir[0] && strstr(fontDir, "/../fonts")) snprintf(assPastaFontesApp, sizeof assPastaFontesApp, "%s", fontDir);
#endif
  }
  snprintf(assPastaFontes, sizeof assPastaFontes, "%s", fontDir);
  if (assFonteLocal[0]) {
    size_t n; unsigned char *b = ass_fonte_local_ler(assFonteLocal, &n);
    if (b) { ass_add_font(assLib, assFonteLocal, (const char *)b, (int)n); assFonteRegistrada = 1; }
    else assFonteRegistrada = 0;
    free(b);
  }
  ass_carregar_pasta_locked();
  snprintf(assFallbackFont, sizeof assFallbackFont, "%s", fallbackFont);
  ass_aplicar_fontes_locked();
  assNFontesVistas = 0; assBytesFontesVistas = 0;
  ass_set_cache_limits(assRenderer, 0, 32);
  assFrameW = 1920; assFrameH = 1080;
  ass_set_frame_size(assRenderer, assFrameW, assFrameH);
  ass_set_storage_size(assRenderer, assFrameW, assFrameH);
  ass_set_font_scale(assRenderer, 1.0);
  ass_set_hinting(assRenderer, ASS_HINTING_LIGHT);   /* explicito: nao depender do padrao da versao */
  layAplW = layAplVW = assFrameW; layAplH = layAplVH = assFrameH; layAplEscala = 1.0;
  clock_gettime(CLOCK_MONOTONIC, &t1);
  /* Uma vez por sessao; na C9 a pasta tem 106 MB de fontes. */
  printf("[ass] libass iniciado em %ld ms (fontes de %s)\n",
         (long)((t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L),
         fontDir[0] ? fontDir : "-");
  fflush(stdout);
}

static void ass_frame_liberar(AssCpuFrame *frame) {
  int i;
  for (i = 0; i < frame->count; i++) free(frame->images[i].bitmap);
#if ASS_GPU
  for (i = 0; i < frame->nfontes; i++) free(frame->fontes[i].buf);
  free(frame->fontes);
#endif
  free(frame->images);
  memset(frame, 0, sizeof *frame);
}

/* Fila dos prontos (ver assFila). Todas sob assFilaTrava. */
static void ass_fila_limpar_locked(void) {
  int i;
  for (i = 0; i < assFilaN; i++) ass_frame_liberar(&assFila[i]);
  assFilaN = 0;
}
static size_t ass_fila_bytes_locked(void) {
  size_t b = 0; int i;
  for (i = 0; i < assFilaN; i++) b += assFila[i].bytes;
  return b;
}
static void ass_fila_tirar_locked(int i, AssCpuFrame *saida) {
  if (i < 0 || i >= assFilaN) return;
  if (saida) *saida = assFila[i]; else ass_frame_liberar(&assFila[i]);
  memmove(&assFila[i], &assFila[i + 1], (size_t)(assFilaN - i - 1) * sizeof assFila[0]);
  assFilaN--;
  memset(&assFila[assFilaN], 0, sizeof assFila[0]);
}
/* Publica (worker). Teto 1 = substitui, como o assPronto antigo; com mais,
 * a fila cheia perde o mais velho. */
static void ass_fila_publicar_locked(AssCpuFrame *f) {
  int teto = assFilaTeto < 1 ? 1 : assFilaTeto > ASS_FILA_MAX ? ASS_FILA_MAX : assFilaTeto;
  while (assFilaN >= teto) ass_fila_tirar_locked(0, NULL);
  /* Quadros enormes (letreiro com blur de tela cheia): a fila nao guarda mais
   * que ASS_FILA_BYTES; sai o mais velho. */
  while (assFilaN && ass_fila_bytes_locked() + f->bytes > ASS_FILA_BYTES) ass_fila_tirar_locked(0, NULL);
  assFila[assFilaN++] = *f;
  memset(f, 0, sizeof *f);
}

/* assperf (texcache): chave do CONTEUDO de uma imagem, para a textura de um
 * letreiro que nao mudou nao subir de novo. Palavra de 4 bytes por vez (o
 * ARMv7 da TV), linha a linha do bitmap ja compacto. Nunca 0. */
static unsigned long long ass_chave_bitmap(const unsigned char *b, int w, int h, unsigned extra) {
  unsigned long long hsh = 0x9E3779B97F4A7C15ULL ^ ((unsigned long long)(unsigned)w << 32) ^ (unsigned)h ^
                           ((unsigned long long)extra << 17);
  size_t n = (size_t)w * (size_t)h, i = 0;
  for (; i + 4 <= n; i += 4) {
    uint32_t v; memcpy(&v, b + i, 4);
    hsh = (hsh ^ v) * 0x100000001B3ULL;
    hsh ^= hsh >> 29;
  }
  for (; i < n; i++) hsh = (hsh ^ b[i]) * 0x100000001B3ULL;
  return hsh ? hsh : 1;
}

#if ASS_GPU
/* Indice da copia de um bitmap do libass nas fontes do quadro (copia na
 * primeira vez). -1 sem memoria. */
static int ass_fonte_copiar(AssCpuFrame *out, int *cap, const unsigned char *src, int w, int h, int stride,
                            size_t *total) {
  int i, y;
  AssGpuFonte *f;
  size_t bytes = (size_t)w * (size_t)h;
  for (i = 0; i < out->nfontes; i++)
    if (out->fontes[i].orig == src && out->fontes[i].w == w && out->fontes[i].h == h) return i;
  if (bytes > 16u * 1024u * 1024u - *total) return -1;
  if (out->nfontes == *cap) {
    int nc = *cap ? *cap * 2 : 16;
    AssGpuFonte *nv = realloc(out->fontes, (size_t)nc * sizeof *nv);
    if (!nv) return -1;
    out->fontes = nv; *cap = nc;
  }
  f = &out->fontes[out->nfontes];
  f->buf = malloc(bytes ? bytes : 1);
  if (!f->buf) return -1;
  for (y = 0; y < h; y++) memcpy(f->buf + (size_t)y * (size_t)w, src + (size_t)y * (size_t)stride, (size_t)w);
  f->w = w; f->h = h; f->orig = src;
  f->chave = ass_chave_bitmap(f->buf, w, h, 0x5A17u);
  *total += bytes;
  return out->nfontes++;
}
#endif

static int ass_frame_copiar(ASS_Image *im, AssCpuFrame *out, int hashear, int corNaChave) {
  ASS_Image *p;
  int n = 0, i = 0;
  size_t total = 0;
#if ASS_GPU
  int capFontes = 0;
#endif
  memset(out, 0, sizeof *out);
  for (p = im; p; p = p->next) {
    if (p->bitmap && p->w > 0 && p->h > 0 && p->stride >= p->w) n++;
    if (n > 4096) return 0;
  }
  if (n) {
    out->images = calloc((size_t)n, sizeof *out->images);
    if (!out->images) return 0;
    out->count = n;
  }
  for (p = im; p; p = p->next) {
    AssCpuImage *dst;
    size_t bytes;
    int y;
    if (!p->bitmap || p->w <= 0 || p->h <= 0 || p->stride < p->w) continue;
    bytes = (size_t)p->w * (size_t)p->h;
    /* Mantem o snapshot CPU de cada frame abaixo de 16 MiB. O quadro atual e
     * o pronto podem coexistir por um instante, portanto o teto total e 32 MiB. */
    if (bytes > 16u * 1024u * 1024u - total) {
      ass_frame_liberar(out); return 0;
    }
    dst = &out->images[i];
#if ASS_GPU
    { ASS_NvDefer d;
      if (ass_nv_image_defer(p, &d)) {
        /* Letreiro adiado: o nitido inteiro (e o preenchimento do fix) vao uma
         * vez por quadro; a imagem guarda so a receita. */
        int fo = ass_fonte_copiar(out, &capFontes, d.src, d.src_w, d.src_h, d.src_stride, &total), fi = -1;
        if (fo >= 0 && d.fix && d.fill)
          fi = ass_fonte_copiar(out, &capFontes, d.fill, d.fill_w, d.fill_h, d.fill_stride, &total);
        if (fo < 0 || (d.fix && d.fill && fi < 0)) { ass_frame_liberar(out); return 0; }
        dst->gpu = 1;
        dst->g.fonte = fo; dst->g.fill = fi;
        dst->g.r2x = d.r2x; dst->g.r2y = d.r2y;
        dst->g.subX = d.sub_x; dst->g.subY = d.sub_y;
        dst->g.fix = d.fix && fi >= 0; dst->g.fillDx = d.fill_dx; dst->g.fillDy = d.fill_dy;
        dst->g.shiftX = d.shift_x; dst->g.shiftY = d.shift_y;
        dst->g.coreX = d.core_x; dst->g.coreY = d.core_y; dst->g.coreW = d.core_w; dst->g.coreH = d.core_h;
        dst->bitmap = NULL;
        dst->x = p->dst_x; dst->y = p->dst_y;
        dst->w = p->w; dst->h = p->h; dst->stride = p->w;
        dst->type = p->type; dst->color = p->color;
        dst->chave = 0;
        i++;
        continue;
      } }
#endif
    dst->bitmap = malloc(bytes);
    if (!dst->bitmap) { ass_frame_liberar(out); return 0; }
    dst->x = p->dst_x; dst->y = p->dst_y;
    dst->w = p->w; dst->h = p->h; dst->stride = p->w;
    dst->type = p->type; dst->color = p->color;
    for (y = 0; y < p->h; y++)
      memcpy(dst->bitmap + (size_t)y * (size_t)p->w,
             p->bitmap + (size_t)y * (size_t)p->stride, (size_t)p->w);
    /* RGBA leva a cor assada na textura: entra na chave. Alpha 8-bit tinge
     * no desenho: o mesmo bitmap serve a qualquer cor. */
    dst->chave = hashear ? ass_chave_bitmap(dst->bitmap, p->w, p->h, corNaChave ? p->color : 0u) : 0;
    total += bytes; i++;
  }
  out->count = i; out->bytes = total;
  return 1;
}

static void ass_trilha_tailandes(ASS_Track *t) {
  int i;
  if (!assVistoThai) return;
  for (i = 0; i < t->n_events; i++) {
    char *novo;
    if (!t->events[i].Text || !(novo = assrender_marcar_tailandes(t->events[i].Text))) continue;
    free(t->events[i].Text); t->events[i].Text = novo;
  }
}

static void ass_plain_style_locked(const PlainAssStyle *p) {
  ASS_Style *s;
  if (!assTrack || !assTrack->n_styles) return;
  if (plainAplValido && !memcmp(p, &plainApl, sizeof *p)) return;
  if (!assTextoSimples) {
    if (!authoredStyles) {
      authoredStyles = calloc(assTrack->n_styles, sizeof *authoredStyles);
      if (!authoredStyles) return;
      for (int i = 0; i < assTrack->n_styles; i++) {
        authoredStyles[i] = assTrack->styles[i];
        authoredStyles[i].FontName = strdup(assTrack->styles[i].FontName);
        if (!authoredStyles[i].FontName) { ass_authored_styles_free(); return; }
        authoredStyleCount++;
      }
    }
    for (int i = 0; i < authoredStyleCount; i++) {
      ASS_Style *original = &authoredStyles[i];
      char *font = strdup(p->overrides & PLAIN_ASS_FONT ? p->font : original->FontName);
      if (!font) return;
      s = &assTrack->styles[i];
      free(s->FontName); *s = *original; s->FontName = font;
      if (p->overrides & PLAIN_ASS_COLOR) s->PrimaryColour = (uint32_t)p->rgb << 8;
      if (p->overrides & PLAIN_ASS_OUTLINE) s->OutlineColour = (uint32_t)p->outlineRgb << 8;
      if (p->overrides & PLAIN_ASS_BORDER) {
        s->BorderStyle = 1;
        s->Outline = p->border == 1 ? 2 : 0;
        s->Shadow = p->border == 2 ? 4 : 0;
      }
      if (p->overrides & PLAIN_ASS_BG) {
        s->BorderStyle = p->background ? 4 : 1;
        s->BackColour = p->background ? (unsigned)(255 - p->background * 255 * 16 / 100) : 46u;
      }
      // BackColour carries shadow RGB unless it is a background box.
      if ((p->overrides & PLAIN_ASS_OUTLINE) && s->BorderStyle == 1)
        s->BackColour = ((uint32_t)p->outlineRgb << 8) | (s->BackColour & 255u);
      if (p->overrides & PLAIN_ASS_POS) s->MarginV = p->marginV * assTrack->PlayResY / 1080;
      if (p->overrides & PLAIN_ASS_BOLD) s->Bold = p->bold;
    }
    /* Flush cached event layout after an explicit style change or reset. */
    ass_set_selective_style_override_enabled(assRenderer, ASS_OVERRIDE_BIT_MARGINS);
    ass_set_selective_style_override_enabled(assRenderer, 0);
    plainApl = *p; plainAplValido = 1;
    return;
  }
  s = &assTrack->styles[assTrack->default_style];
  if (!s->FontName || strcmp(s->FontName, p->font)) {
    char *name = strdup(p->font);
    if (!name) return;
    free(s->FontName); s->FontName = name;
  }
  s->FontSize = p->size;
  s->PrimaryColour = s->SecondaryColour = ((uint32_t)p->rgb << 8);
  s->OutlineColour = (uint32_t)p->outlineRgb << 8;
  // BorderStyle=4 uses BackColour for the existing black background box.
  // Without that box, BackColour is the shadow, at its original opacity.
  s->BackColour = p->background ? (unsigned)(255 - (p->background * 255 * 16 / 100))
                   : (p->border == 2 ? (uint32_t)p->outlineRgb << 8 : 0u) | 46u;
  s->Bold = p->bold ? 1 : 0;
  s->BorderStyle = p->background ? 4 : 1;
  s->Outline = p->border == 1 ? 2 : 0;
  s->Shadow = p->border == 2 ? 4 : 0;
  s->MarginV = p->marginV;
  s->MarginL = s->MarginR = 130;
  /* Direct style edits leave libass's per-event collision position cached.
   * A cue first rendered above player controls can stay raised after they
   * close. Apply margins through the public renderer setter and invalidate
   * layout even on older libass (0.17.1 doesn't invalidate in the setter).
   * This runs only when the user's/plain player style changes, not per frame. */
  ass_set_selective_style_override_enabled(assRenderer, 0);
  ass_set_selective_style_override(assRenderer, s);
  ass_set_selective_style_override_enabled(assRenderer, ASS_OVERRIDE_BIT_MARGINS);
  plainApl = *p; plainAplValido = 1;
}

static void ass_plain_reflow_locked(long long t) {
  long long next = LLONG_MAX;
  uint64_t active = UINT64_C(1469598103934665603);
  if (!assTextoSimples || !assTrack || !plainAplValido) return;
  if (plainTimingValid && t >= plainLastTime && t < plainNextBoundary) {
    plainLastTime = t;
    return;
  }
  for (int i = 0; i < assTrack->n_events; i++) {
    ASS_Event *e = &assTrack->events[i];
    long long end = e->Start + e->Duration;
    if (e->Start > t && e->Start < next) next = e->Start;
    if (end > t && end < next) next = end;
    if (e->Start <= t && end > t)
      active = (active ^ (uint64_t)(i + 1)) * UINT64_C(1099511628211);
  }
  if (plainTimingValid && active != plainActiveSet) {
    /* libass keeps a collided cue's old vertical shift even after the
     * other cues end. Plain captions must reflow from the current bottom,
     * like the existing text overlay; authored ASS keeps its semantics. */
    ass_set_selective_style_override_enabled(assRenderer, 0);
    ass_set_selective_style_override_enabled(assRenderer, ASS_OVERRIDE_BIT_MARGINS);
  }
  plainTimingValid = 1; plainLastTime = t;
  plainNextBoundary = next; plainActiveSet = active;
}

/* assperf "Your style on styled ASS = Ignored": o ASS com estilo do autor
 * desenha como o autor fez (cores, borda, caixa, fonte, opacidade da folha
 * Estilo NAO entram), e o tamanho da folha nao escala os letreiros
 * posicionados (ASS_OVERRIDE_BIT_SELECTIVE_FONT_SCALE, o padrao do libass e
 * do mpv). Texto simples segue a folha como sempre. Sob assTrava. */
static int ass_estilo_autor_locked(void) {
#ifdef NV_TPK
  return assperf_get(AP_ESTILO) && !__atomic_load_n(&assTextoSimples, __ATOMIC_ACQUIRE);
#else
  return 0;
#endif
}

#if defined(NV_TPK) && defined(ASS_NV_API)
static int assBenchAtivo;   // sob assTrava; o bench liga os cronometros por conta propria

/* Fios do blur: valem para o processo inteiro, entao sao aplicados assim que
 * a folha muda (do fio GL a cada quadro, do worker e do bench). Antes so o
 * worker aplicava, e com o video pausado o bench rodava com o valor anterior
 * (logs de 23/10: "blur threads=4" sempre). */
static void ass_globais_aplicar(void) {
  static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
  static int fiosAplicados = -1;
  int f = assperf_fios();
  pthread_mutex_lock(&m);
  if (f != fiosAplicados) {
    int got = ass_nv_set_threads(f);
    printf("[assperf] blur threads: %d%s\n", got, got != f ? " (fewer threads could be started)" : "");
    fflush(stdout);
    fiosAplicados = f;
  }
  pthread_mutex_unlock(&m);
}

/* Cronometros do libass do ultimo ass_render_frame (sob assTrava, logo depois
 * dele) para a janela de 10 s. */
static void ass_estagios_colher_locked(ASS_Renderer *r, AssperfEstagios *e) {
  ASS_NvPerf p;
  int i;
  ass_nv_perf_take(&p);
  memset(e, 0, sizeof *e);
  e->motor = r ? ass_nv_get_engine(r) : -1;
  e->frameMs = p.ns_frame / 1e6;
  for (i = 0; i < 8; i++) e->stageMs[i] = p.ns_stage[i] / 1e6;
  e->frames = p.frames; e->events = p.events; e->images = p.images;
  for (i = 0; i < 3; i++) { e->hit[i] = p.cache_hit[i]; e->miss[i] = p.cache_miss[i]; }
  e->renderers = 1;
#ifdef ASS_NV_API_PARALELO
  { ASS_NvParStats ps;
    ass_nv_par_stats_take(&ps);
    e->parJobs = ps.jobs; e->parSplit = ps.split; e->parSmall = ps.small; e->parBusy = ps.busy; e->parOne = ps.single;
    e->parLanes = ps.lanes; e->parLanesSplit = ps.lanes_split; }
#endif
}
#endif

/* assperf: o que o renderer aplica das chaves (cache, hinting, shaper) e a
 * prioridade do fio. Sob assTrava, no worker, so quando a versao das chaves
 * muda. O Legacy repete exatamente o que ass_iniciar_locked faz. */
static unsigned assApVersao = ~0u;
static int assApSimples = -1, assApPrioridade = -1;
static void ass_aplicar_assperf_locked(void) {
  static const int glifos[4] = { 0, 1000, 4000, 10000 };
  static const int cacheMb[4] = { 32, 64, 128, 16 };
  static const int hinting[4] = { ASS_HINTING_LIGHT, ASS_HINTING_NONE, ASS_HINTING_NORMAL, ASS_HINTING_NATIVE };
  unsigned v = assperf_versao();
  int simples = __atomic_load_n(&assTextoSimples, __ATOMIC_ACQUIRE);
  if (!assRenderer || (v == assApVersao && simples == assApSimples)) return;
  assApVersao = v; assApSimples = simples;
  { int g = assperf_get(AP_GLIFOS), c = assperf_get(AP_CACHE), h = assperf_get(AP_HINTING);
    ass_set_cache_limits(assRenderer, glifos[g & 3], cacheMb[c & 3]);
    ass_set_hinting(assRenderer, (ASS_Hinting)hinting[h & 3]);
    /* O texto simples RTL (arabe) PRECISA do HarfBuzz: o Simple so vale
     * para o ASS de verdade. */
    ass_set_shaper(assRenderer, assperf_get(AP_SHAPER) && !simples ? ASS_SHAPING_SIMPLE : ASS_SHAPING_COMPLEX); }
#if defined(NV_TPK) && defined(ASS_NV_API)
  /* libass do .tpk (tools/tpk/libass-nv): motor de bitmap e cronometros. */
  { static int motorAplicado = -1;
    int m = assperf_get(AP_MOTOR) ? 1 : 0;
    if (m != motorAplicado) {
      int got = ass_nv_set_engine(assRenderer, m);
      printf("[assperf] bitmap engine: %s%s\n", got ? "NEON (armv7 SIMD)" : "C (plain)",
             m && !got ? " - NEON not available in this libass build" : "");
      motorAplicado = m;
    }
    if (!assBenchAtivo) ass_nv_perf_enable(assperf_get(AP_ESTAGIOS));
    ass_globais_aplicar(); }
#endif
#if !defined(__EMSCRIPTEN__) && defined(SYS_gettid)
  { int p = assperf_get(AP_PRIORIDADE);
    if (p != assApPrioridade) {
      int nice = p == 1 ? -4 : p == 2 ? 5 : 0;
      int r = setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), nice);
      if (assApPrioridade >= 0 || p)
        printf("[assperf] render thread priority nice=%d: %s\n", nice, r == 0 ? "applied" : "refused by the system");
      assApPrioridade = p;
    } }
#endif
}

#if ASS_GPU
/* -1 = a GPU ainda nao tentou; 0 = os shaders falharam (o blur fica na CPU);
 * 1 = pronta. Escrito pelo fio GL. */
static int assGpuOk = -1;

/* Worker, sob assTrava, antes de cada render: o libass adia o blur quando a
 * folha pede e a GPU nao falhou. */
static void ass_adiar_aplicar_locked(void) {
  int quer = assperf_get(AP_GPUBLUR) && __atomic_load_n(&assGpuOk, __ATOMIC_ACQUIRE) != 0;
  if (!assRenderer) return;
  ass_nv_set_deferred_min_area(assRenderer, (int)assperf_gpu_min());
  if (ass_nv_get_deferred_blur(assRenderer) == quer) return;
  ass_nv_set_deferred_blur(assRenderer, quer);
  printf("[assperf] blur on GPU: %s\n", quer ? "on (libass hands blurred signs to the GL thread)"
         : assperf_get(AP_GPUBLUR) ? "off - the GPU shaders are not available, blur stays on the CPU"
                                   : "off (libass CPU blur, exact)");
  fflush(stdout);
}
#endif

/* Evento que muda SOZINHO entre Start e End: \t, \move, \fad/\fade,
 * karaoke (\k, \kf, \ko, \K) ou Effect (Banner/Scroll). */
static int ass_evento_anima(const ASS_Event *e) {
  const char *p;
  if (e->Effect && e->Effect[0]) return 1;
  if (!e->Text) return 0;
  for (p = strchr(e->Text, '\\'); p; p = strchr(p + 1, '\\'))
    if (!strncmp(p, "\\t(", 3) || !strncmp(p, "\\move", 5) || !strncmp(p, "\\fad", 4) || p[1] == 'k' || p[1] == 'K')
      return 1;
  return 0;
}

static double ass_ms_entre(const struct timespec *a, const struct timespec *b) {
  return (double)(b->tv_sec - a->tv_sec) * 1000.0 + (double)(b->tv_nsec - a->tv_nsec) / 1e6;
}

static void *ass_worker_loop(void *unused) {
  (void)unused;
  for (;;) {
    AssCpuFrame frame;
    unsigned generation, serial, epoch;
    double ms, esc, resol;
    float ox, oy;
    int lw, lh, vw, vh, plainValid, k;
    PlainAssStyle plain;
    struct timespec a, b, c0;
    ASS_Image *images = NULL;
    int changed = 0, pronto = 0, igual = 0;
    long long validoDe = LLONG_MAX, validoAte = LLONG_MAX;
    memset(&frame, 0, sizeof frame);
    pthread_mutex_lock(&assFilaTrava);
    while (!assWorkerParar && !assPedidoPendente)
      pthread_cond_wait(&assFilaCond, &assFilaTrava);
    if (assWorkerParar) { pthread_mutex_unlock(&assFilaTrava); break; }
    generation = assPedidoGeracao; serial = assPedidoSerial; epoch = assEpoch; ms = assPedidoMs;
    ox = layPedX; oy = layPedY; lw = layPedW; lh = layPedH; k = layPedK;
    /* assperf: resolucao do render. O quadro sai `resol` do tamanho do alvo e
     * o desenho amplia (esc = k x resol pixels por unidade). 1 = como antes. */
    resol = assperf_escala();
    lw = (int)lround((double)lw * k * resol); lh = (int)lround((double)lh * k * resol);   /* libass renderiza em pixels do alvo, nao em unidades de layout */
    if (lw < 16) lw = 16;
    if (lh < 16) lh = 16;
    vw = layPedVW; vh = layPedVH; esc = layPedEscala;
    plain = plainPed; plainValid = plainPedValido;
    assPedidoPendente = 0;
    pthread_mutex_unlock(&assFilaTrava);

    clock_gettime(CLOCK_MONOTONIC, &a);
    pthread_mutex_lock(&assTrava);
    clock_gettime(CLOCK_MONOTONIC, &c0);
    if (generation == __atomic_load_n(&assGeracao, __ATOMIC_ACQUIRE) &&
        generation == assTrackGeracao &&
        assTrack && assRenderer) {
      long long t = (long long)llround(ms);
      ass_aplicar_assperf_locked();
      if (lw != layAplW || lh != layAplH) {
        ass_set_frame_size(assRenderer, lw, lh);
        assFrameW = layAplW = lw; assFrameH = layAplH = lh;
      }
      if (vw != layAplVW || vh != layAplVH) {
        ass_set_storage_size(assRenderer, vw, vh);
        layAplVW = vw; layAplVH = vh;
      }
      if (esc != layAplEscala) {
        ass_set_font_scale(assRenderer, esc);
        layAplEscala = esc;
      }
      if (plainValid) {
        PlainAssStyle ps = plain;
        if (ass_estilo_autor_locked()) ps.overrides = 0;   // estilos do autor de volta
        ass_plain_style_locked(&ps);
      }
      if (ass_estilo_autor_locked())
        ass_set_selective_style_override_enabled(assRenderer, ASS_OVERRIDE_BIT_SELECTIVE_FONT_SCALE);
      ass_plain_reflow_locked(t);
#if ASS_GPU
      ass_adiar_aplicar_locked();
#endif
      images = ass_render_frame(assRenderer, assTrack, t, &changed);
#if ASS_GPU
      { ASS_NvDeferStats ds;
        ass_nv_defer_stats_take(assRenderer, &ds);
        if (ds.blurred) {
          AssperfAdiado a;
          int q;
          a.comBlur = ds.blurred; a.adiados = ds.deferred; a.be = ds.kept_be; a.clip = ds.kept_clip; a.pequenos = ds.kept_small;
          for (q = 0; q < 4; q++) { a.cpuN[q] = ds.cpu_n[q]; a.cpuMs[q] = ds.cpu_ms[q]; }
          assperf_med_adiado(&a);
        } }
#endif
#if defined(NV_TPK) && defined(ASS_NV_API)
      { AssperfEstagios e;
        ass_estagios_colher_locked(assRenderer, &e);
        if (assperf_get(AP_ESTAGIOS)) assperf_med_estagios(&e); }
#endif
      /* assperf: ate quando este quadro continua certo (ver assValidoAte). */
      if (assperf_get(AP_MUDANCA)) {
        int i, anima = 0;
        for (i = 0; i < assTrack->n_events && !anima; i++) {
          const ASS_Event *ev = &assTrack->events[i];
          long long ini = ev->Start, fimE = ev->Start + ev->Duration;
          if (ini > t) { if (ini < validoAte) validoAte = ini; }
          else if (fimE > t) { if (fimE < validoAte) validoAte = fimE; if (ass_evento_anima(ev)) anima = 1; }
        }
        validoDe = t;
        if (anima) validoAte = t;   // animando: cada quadro e outro
      }
      /* Igual ao publicado: nada a fazer, o quadro em tela continua certo. */
      pthread_mutex_lock(&assFilaTrava);
      igual = !changed && assProduziu && assProduzidoEpoch == epoch &&
              assProduzidoGeracao == generation && epoch == assEpoch;
      pthread_mutex_unlock(&assFilaTrava);
      if (!igual) {
        pronto = ass_frame_copiar(images, &frame, assperf_get(AP_TEXCACHE), !assperf_get(AP_UPLOAD));
        frame.ms = t; frame.inicioMax = -1;
        frame.ox = ox; frame.oy = oy; frame.k = k;
        frame.esc = (float)((double)k * resol);
        { int i;
          for (i = 0; i < assTrack->n_events; i++) {
            long long ini = assTrack->events[i].Start;
            if (ini <= t && ini + assTrack->events[i].Duration > t && ini > frame.inicioMax)
              frame.inicioMax = ini;
          } }
      }
    }
    pthread_mutex_unlock(&assTrava);
    clock_gettime(CLOCK_MONOTONIC, &b);
    /* assperf: o render PURO (depois de pegar a trava) e a espera pela trava,
     * separados. O "[ass] 10s" de sempre continua somando os dois. */
    assperf_med_render(ass_ms_entre(&c0, &b), ass_ms_entre(&a, &c0), !igual);

    pthread_mutex_lock(&assFilaTrava);
    { unsigned long long us = (unsigned long long)((b.tv_sec - a.tv_sec) * 1000000ll +
                              (b.tv_nsec - a.tv_nsec) / 1000ll);
      assUltimoRenderPuroMs = ass_ms_entre(&c0, &b);
      assMed.renders++; assMed.renderSomaUs += us;
      if (us > assMed.renderMaxUs) assMed.renderMaxUs = us;
      if (igual) assMed.iguais++;
      assUltimoRenderUs = us; }
    /* SEM o antigo "fabs(assPedidoMs - ms) <= 50": com o relogio interpolado o
     * pedido anda 16 ms por quadro, e um render de mais de 50 ms na C9 jogava
     * fora TODO quadro — a legenda nunca aparecia. Um quadro de 30 ms atras e
     * melhor que nenhum; o proximo pedido ja esta na fila. */
    if (pronto && !assWorkerParar && epoch == assEpoch &&
        generation == __atomic_load_n(&assGeracao, __ATOMIC_ACQUIRE) &&
        generation == assPedidoGeracao) {
      frame.generation = generation; frame.serial = serial;
      assBytesQuadro = frame.bytes;
      ass_fila_publicar_locked(&frame);
      assProduziu = 1; assProduzidoEpoch = epoch; assProduzidoGeracao = generation;
    }
    if (validoDe != LLONG_MAX && !assWorkerParar && epoch == assEpoch &&
        generation == __atomic_load_n(&assGeracao, __ATOMIC_ACQUIRE) && (pronto || igual)) {
      assValidoDe = validoDe; assValidoAte = validoAte;
      assValidoEpoch = epoch; assValidoGeracao = generation;
    } else if (!igual && !pronto) {
      assValidoDe = LLONG_MAX; assValidoAte = LLONG_MIN;
    }
    ass_frame_liberar(&frame);
    pthread_mutex_unlock(&assFilaTrava);
  }
  return NULL;
}

static int ass_worker_iniciar(void) {
  int ok = 1;
  pthread_mutex_lock(&assFilaTrava);
  if (!assWorkerCriado) {
    assWorkerParar = 0;
    if (pthread_create(&assWorker, NULL, ass_worker_loop, NULL) == 0) {
      pthread_detach(assWorker);
      __atomic_store_n(&assWorkerCriado, 1, __ATOMIC_RELEASE);
    } else ok = 0;
  }
  pthread_mutex_unlock(&assFilaTrava);
  return ok;
}

static void ass_apagar_texturas_locked(void) {
  int i;
  for (i = 0; i < assTexCap; i++) {
    if (assTex[i].tex) {
      gfx_tex_esquecer(assTex[i].tex);
      glDeleteTextures(1, &assTex[i].tex);
      assTex[i].tex = 0;
    }
    assTex[i].w = assTex[i].h = 0;
  }
  free(assTex); assTex = NULL; assTexCap = 0;
  assTexSerial = 0;
}

static int ass_reservar_texturas_locked(int slot) {
  int nc, i;
  AssTex *nv;
  if (slot < assTexCap) return 1;
  nc = assTexCap ? assTexCap : ASS_TEX_INICIAL;
  while (nc <= slot) {
    if (nc > 4096) { nc = slot + 1; break; }
    nc *= 2;
  }
  nv = realloc(assTex, (size_t)nc * sizeof *nv);
  if (!nv) return 0;
  for (i = assTexCap; i < nc; i++) { nv[i].tex = 0; nv[i].w = nv[i].h = 0; }
  assTex = nv; assTexCap = nc;
  return 1;
}

static GLuint ass_textura_locked(int slot, const AssCpuImage *im) {
  int x, y;
  unsigned char *rgba;
  unsigned color = im->color;
  unsigned r = (color >> 24) & 255u;
  unsigned g = (color >> 16) & 255u;
  unsigned b = (color >> 8) & 255u;
  unsigned opacidade = 255u - (color & 255u);
  size_t bytes = (size_t)im->w * (size_t)im->h * 4u;

  if (assCorAtiva && im->type == IMAGE_TYPE_CHARACTER) {
    r = (unsigned)assCorR; g = (unsigned)assCorG; b = (unsigned)assCorB;
  }

  if (slot < 0 || !im->bitmap || !im->w || !im->h ||
      !ass_reservar_texturas_locked(slot)) return 0;
  if (!assTex[slot].tex) glGenTextures(1, &assTex[slot].tex);
  rgba = (unsigned char *)malloc(bytes);
  if (!rgba) return 0;
  for (y = 0; y < im->h; y++) {
    const unsigned char *src = im->bitmap + (size_t)y * (size_t)im->stride;
    for (x = 0; x < im->w; x++) {
      unsigned a = ((unsigned)src[x] * opacidade + 127u) / 255u;
      unsigned char *dst = rgba + ((size_t)y * (size_t)im->w + (size_t)x) * 4u;
      dst[0] = (unsigned char)r;
      dst[1] = (unsigned char)g;
      dst[2] = (unsigned char)b;
      dst[3] = (unsigned char)a;
    }
  }
  glBindTexture(GL_TEXTURE_2D, assTex[slot].tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, im->w, im->h, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gfx_tex_esquecer(0);
  free(rgba);
  assTex[slot].w = im->w; assTex[slot].h = im->h;
  return assTex[slot].tex;
}

/* ===== assperf: O CAMINHO NOVO DO DESENHO (fio GL) ============================
 *
 * O caminho antigo (acima, assAtual + assTex) segue intacto e e o usado com as
 * tres chaves de upload em "old". Este e o das chaves:
 *   - Upload format = Alpha 8-bit: a textura e o bitmap do libass como veio
 *     (GL_ALPHA, 1 byte por pixel, sem a conversao RGBA por pixel na CPU); a
 *     cor e a opacidade da imagem vao no desenho (GFX_MARCA: forma do alfa,
 *     cor de uCor).
 *   - Reuse unchanged images: cache de texturas pela CHAVE do conteudo; um
 *     letreiro que nao mudou nao sobe de novo. Uma textura so e regravada
 *     depois de `Texture recycle delay` quadros sem ser desenhada.
 *   - Upload outside lock: o quadro em tela (assMain) e do fio GL; o upload
 *     corre sem assFilaTrava, que o worker precisa para pegar o proximo pedido.
 * Quem invalida de outro fio (troca de faixa, seek, geracao) liga
 * assTexResetar, como sempre; o fio GL entao solta assMain e as texturas. */
typedef struct {
  GLuint tex;
  int w, h, alfa;
  unsigned long long chave;
  unsigned uso;          // assGlQuadro do ultimo desenho
} AssTc;
#define ASS_TC_TETO 768
#define ASS_TC_BYTES (24u * 1024u * 1024u)
static AssTc *assTc;
static int assTcN, assTcCap;
static size_t assTcBytes;
static unsigned assGlQuadro = 1000;
static AssTc *assSlot;           // sem cache: uma textura por posicao da lista, como o antigo
static int assSlotCap;
static AssCpuFrame assMain;      // o quadro em tela no caminho novo (so o fio GL toca)
static int assMainValido;
static GLuint *assMainTex;       // textura de cada imagem do assMain
static int *assMainIdx;          // e o indice dela em assTc (-1 = sem cache)
#if ASS_GPU
static unsigned long long *assMainGk;   // imagem adiada: a chave da textura no cache do assgpublur
#endif
static int assMainTexCap, assMainEnviado;
static int assMainCorChave = -2, assMainFmt = -1, assMainCache = -1;
static int assCaminhoNovoAntes = -1;
static int assMainResetar;       // assrender_aplicar_invalidacao consumiu um assTexResetar

static size_t ass_tc_tam(int w, int h, int alfa) { return (size_t)w * (size_t)h * (alfa ? 1u : 4u); }

static void ass_tc_apagar(void) {
  int i;
  for (i = 0; i < assTcN; i++)
    if (assTc[i].tex) { gfx_tex_esquecer(assTc[i].tex); glDeleteTextures(1, &assTc[i].tex); }
  free(assTc); assTc = NULL; assTcN = assTcCap = 0; assTcBytes = 0;
  for (i = 0; i < assSlotCap; i++)
    if (assSlot[i].tex) { gfx_tex_esquecer(assSlot[i].tex); glDeleteTextures(1, &assSlot[i].tex); }
  free(assSlot); assSlot = NULL; assSlotCap = 0;
  assMainEnviado = 0;
#if ASS_GPU
  if (__atomic_load_n(&assGpuOk, __ATOMIC_ACQUIRE) == 1) assgpublur_apagar();
#endif
}

static void ass_main_soltar(void) {
  ass_frame_liberar(&assMain); assMainValido = 0; assMainEnviado = 0;
}

/* A imagem em RGBA, com a cor e a opacidade assadas (o formato antigo). */
static unsigned char *ass_rgba(const AssCpuImage *im, int corAtiva, int cr, int cg, int cb) {
  unsigned color = im->color, op = 255u - (color & 255u), lut[256], i;
  unsigned char r = (unsigned char)((color >> 24) & 255u), g = (unsigned char)((color >> 16) & 255u),
                b = (unsigned char)((color >> 8) & 255u);
  size_t n = (size_t)im->w * (size_t)im->h, k;
  unsigned char *o = (unsigned char *)malloc(n * 4u);
  if (!o) return NULL;
  if (corAtiva && im->type == IMAGE_TYPE_CHARACTER) { r = (unsigned char)cr; g = (unsigned char)cg; b = (unsigned char)cb; }
  for (i = 0; i < 256; i++) lut[i] = (i * op + 127u) / 255u;
  for (k = 0; k < n; k++) {
    unsigned char *d = o + k * 4u;
    d[0] = r; d[1] = g; d[2] = b; d[3] = (unsigned char)lut[im->bitmap[k]];
  }
  return o;
}

/* Sobe `im` em `tex`. `novo` = a textura nao tem armazenamento desse tamanho
 * e formato (glTexImage2D); senao glTexSubImage2D, sem realocar. */
static size_t ass_tc_subir(GLuint tex, const AssCpuImage *im, int alfa, int novo,
                           int corAtiva, int cr, int cg, int cb) {
  unsigned char *rgba = NULL;
  const void *px = im->bitmap;
  if (!alfa) { rgba = ass_rgba(im, corAtiva, cr, cg, cb); if (!rgba) return 0; px = rgba; }
  glBindTexture(GL_TEXTURE_2D, tex);
  if (novo) {
    glTexImage2D(GL_TEXTURE_2D, 0, alfa ? GL_ALPHA : GL_RGBA, im->w, im->h, 0,
                 alfa ? GL_ALPHA : GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, im->w, im->h, alfa ? GL_ALPHA : GL_RGBA, GL_UNSIGNED_BYTE, px);
  }
  free(rgba);
  return ass_tc_tam(im->w, im->h, alfa);
}

/* Textura para a imagem pelo CONTEUDO (Reuse unchanged images). `recicla` =
 * quadros GL sem desenho que uma textura precisa ter para ser regravada. */
static GLuint ass_tc_obter(const AssCpuImage *im, unsigned long long chave, int alfa, unsigned recicla,
                           int corAtiva, int cr, int cg, int cb, int *reusou, size_t *enviados, int *idx) {
  int i, livre = -1, velha = -1;
  *idx = -1;
  for (i = 0; i < assTcN; i++) {
    AssTc *t = &assTc[i];
    if (t->chave == chave && t->w == im->w && t->h == im->h && t->alfa == alfa && t->tex) {
      t->uso = assGlQuadro; *reusou = 1; *idx = i;
      return t->tex;
    }
  }
  *reusou = 0;
  for (i = 0; i < assTcN; i++) {
    AssTc *t = &assTc[i];
    if (assGlQuadro - t->uso < recicla) continue;   // desenhada ha pouco: a GPU ainda pode le-la
    if (t->w == im->w && t->h == im->h && t->alfa == alfa) { livre = i; break; }
    if (velha < 0 || t->uso < assTc[velha].uso) velha = i;
  }
  if (livre >= 0) {   // mesmo tamanho: regrava sem realocar
    AssTc *t = &assTc[livre];
    *enviados += ass_tc_subir(t->tex, im, alfa, 0, corAtiva, cr, cg, cb);
    t->chave = chave; t->uso = assGlQuadro; *idx = livre;
    return t->tex;
  }
  if (assTcN < ASS_TC_TETO && assTcBytes + ass_tc_tam(im->w, im->h, alfa) <= ASS_TC_BYTES) {
    AssTc *t;
    if (assTcN == assTcCap) {
      int nc = assTcCap ? assTcCap * 2 : 64;
      AssTc *nv = (AssTc *)realloc(assTc, (size_t)nc * sizeof *nv);
      if (!nv) return 0;
      assTc = nv; assTcCap = nc;
    }
    t = &assTc[assTcN];
    memset(t, 0, sizeof *t);
    glGenTextures(1, &t->tex);
    if (!t->tex) return 0;
    *idx = assTcN++;
    *enviados += ass_tc_subir(t->tex, im, alfa, 1, corAtiva, cr, cg, cb);
    t->w = im->w; t->h = im->h; t->alfa = alfa; t->chave = chave; t->uso = assGlQuadro;
    assTcBytes += ass_tc_tam(im->w, im->h, alfa);
    return t->tex;
  }
  if (velha >= 0) {   // cheio: a mais antiga fora de uso muda de tamanho
    AssTc *t = &assTc[velha];
    assTcBytes -= ass_tc_tam(t->w, t->h, t->alfa);
    *enviados += ass_tc_subir(t->tex, im, alfa, 1, corAtiva, cr, cg, cb);
    t->w = im->w; t->h = im->h; t->alfa = alfa; t->chave = chave; t->uso = assGlQuadro;
    assTcBytes += ass_tc_tam(im->w, im->h, alfa);
    *idx = velha;
    return t->tex;
  }
  return 0;   // tudo em uso agora (raro): a imagem fica de fora deste quadro
}

/* Sem cache: uma textura por posicao (o antigo), mas regravada no lugar
 * quando o tamanho bate. */
static GLuint ass_slot_obter(int i, const AssCpuImage *im, int alfa, int corAtiva, int cr, int cg, int cb,
                             size_t *enviados) {
  AssTc *t;
  if (i >= assSlotCap) {
    int nc = assSlotCap ? assSlotCap : 64;
    AssTc *nv;
    while (nc <= i) nc *= 2;
    nv = (AssTc *)realloc(assSlot, (size_t)nc * sizeof *nv);
    if (!nv) return 0;
    memset(nv + assSlotCap, 0, (size_t)(nc - assSlotCap) * sizeof *nv);
    assSlot = nv; assSlotCap = nc;
  }
  t = &assSlot[i];
  if (!t->tex) glGenTextures(1, &t->tex);
  if (!t->tex) return 0;
  *enviados += ass_tc_subir(t->tex, im, alfa, !(t->w == im->w && t->h == im->h && t->alfa == alfa),
                            corAtiva, cr, cg, cb);
  t->w = im->w; t->h = im->h; t->alfa = alfa; t->uso = assGlQuadro;
  return t->tex;
}

#if ASS_GPU
static unsigned assGpuCpu;   // imagens adiadas que a CPU fez (GPU indisponivel ou recusou)

/* A receita de uma imagem adiada como o libass a entende, sobre as copias. */
static void ass_gpu_receita(const AssCpuFrame *f, const AssCpuImage *im, ASS_NvDefer *d) {
  const AssGpuFonte *s = &f->fontes[im->g.fonte];
  memset(d, 0, sizeof *d);
  d->src = s->buf; d->src_w = s->w; d->src_h = s->h; d->src_stride = s->w;
  d->sub_x = im->g.subX; d->sub_y = im->g.subY;
  d->core_x = im->g.coreX; d->core_y = im->g.coreY; d->core_w = im->g.coreW; d->core_h = im->g.coreH;
  d->r2x = im->g.r2x; d->r2y = im->g.r2y;
  if (im->g.fix && im->g.fill >= 0) {
    const AssGpuFonte *g = &f->fontes[im->g.fill];
    d->fix = 1; d->fill = g->buf; d->fill_w = g->w; d->fill_h = g->h; d->fill_stride = g->w;
    d->fill_dx = im->g.fillDx; d->fill_dy = im->g.fillDy;
  }
  d->shift_x = im->g.shiftX; d->shift_y = im->g.shiftY;
}

static void ass_gpu_pedido(const AssCpuFrame *f, const AssCpuImage *im, AgbPedido *p) {
  const AssGpuFonte *s = &f->fontes[im->g.fonte];
  const AssGpuFonte *g = im->g.fix && im->g.fill >= 0 ? &f->fontes[im->g.fill] : NULL;
  memset(p, 0, sizeof *p);
  p->src = s->buf; p->srcW = s->w; p->srcH = s->h; p->srcChave = s->chave;
  if (g) { p->fill = g->buf; p->fillW = g->w; p->fillH = g->h; p->fillChave = g->chave; }
  p->r2x = im->g.r2x; p->r2y = im->g.r2y;
  p->subX = im->g.subX; p->subY = im->g.subY; p->w = im->w; p->h = im->h;
  p->fix = g != NULL; p->fillDx = im->g.fillDx; p->fillDy = im->g.fillDy;
  p->shiftX = im->g.shiftX; p->shiftY = im->g.shiftY;
  p->area = (long)im->g.coreW * (long)im->g.coreH;
}

/* PRE-BLUR (fio GL, sob assFilaTrava): os quadros que ja estao na fila
 * (Render ahead) tem os seus letreiros borrados aos poucos, alguns por quadro
 * de UI, antes da hora. Quando o quadro entra em tela, as texturas ja estao
 * no cache do assgpublur e o fio GL nao para 20-75 ms de uma vez (24/10). */
static void ass_gpu_adiantar_locked(void) {
  int q, i;
  double gasto = 0, orcamento = (double)assperf_gpu_adiante();   // ms de GPU por quadro de UI
  AssperfGpu g;
  if (__atomic_load_n(&assGpuOk, __ATOMIC_ACQUIRE) != 1 || !assperf_get(AP_GPUBLUR) || !assFilaN) return;
  memset(&g, 0, sizeof g);
  assgpublur_lote_ini(assperf_get(AP_ESTAGIOS));   // com os cronometros: custo por tamanho tambem daqui
  for (q = 0; q < assFilaN && gasto < orcamento; q++) {
    const AssCpuFrame *f = &assFila[q];
    for (i = 0; i < f->count && gasto < orcamento; i++) {
      const AssCpuImage *im = &f->images[i];
      AgbPedido p;
      unsigned long long k;
      AgbStats st;
      int z;
      if (!im->gpu) continue;
      ass_gpu_pedido(f, im, &p);
      p.adiantado = 1;
      p.soCache = 1;
      if (assgpublur_obter(&p, assGlQuadro, &k)) { assgpublur_stats(&st); continue; }   // ja pronta
      p.soCache = 0;
      assgpublur_obter(&p, assGlQuadro, &k);
      assgpublur_stats(&st);   // conta e zera: o que este pedido fez (reusos nao contam aqui)
      if (st.blurs) gasto += assgpublur_custo(p.area);
      else if (st.finais) gasto += 0.3;
      g.blurs += st.blurs; g.finais += st.finais; g.falhas += st.falhas; g.adiantados += st.adiantados;
      g.criadas += st.criadas; g.reaproveitadas += st.reaproveitadas;
      for (z = 0; z < 4; z++) { g.nTam[z] += st.nTam[z]; g.msTam[z] += st.msTam[z]; }
      g.msGpu += st.msGpu; g.nGpu += st.nGpu;
    }
  }
  assgpublur_lote_fim();
  { AgbStats st; assgpublur_stats(&st); }   // o tempo deste lote nao entra na media do quadro em tela
  if (g.blurs || g.finais) assperf_med_gpublur(&g);
}

/* Textura de uma imagem adiada: a GPU borra; se ela nao pode, a receita roda
 * na CPU (o mesmo desenho do libass) e sobe como uma imagem comum. Fio GL. */
static GLuint ass_gpu_textura(const AssCpuFrame *f, const AssCpuImage *im, unsigned long long *gk,
                              unsigned recicla, size_t *enviados, int *idx) {
  GLuint t = 0;
  int ok = __atomic_load_n(&assGpuOk, __ATOMIC_ACQUIRE);
  *gk = 0; *idx = -1;
  if (ok != 0) {
    if (ok < 0) {
      ok = assgpublur_iniciar();
      __atomic_store_n(&assGpuOk, ok, __ATOMIC_RELEASE);
      assperf_gpublur_formato(ok ? assgpublur_formato() : "not available");
    }
    if (ok) {
      AgbPedido p;
      ass_gpu_pedido(f, im, &p);
      t = assgpublur_obter(&p, assGlQuadro, gk);
      if (t) return t;
    }
  }
  { ASS_NvDefer d;
    AssCpuImage c = *im;
    int r = 0;
    ass_gpu_receita(f, im, &d);
    c.gpu = 0; c.stride = c.w;
    c.bitmap = malloc((size_t)c.w * (size_t)c.h);
    if (!c.bitmap) return 0;
    if (!ass_nv_defer_apply(&d, c.w, c.h, c.bitmap, c.w)) { free(c.bitmap); return 0; }
    assGpuCpu++;
    t = ass_tc_obter(&c, ass_chave_bitmap(c.bitmap, c.w, c.h, 0), 1, recicla, 0, 0, 0, 0, &r, enviados, idx);
    free(c.bitmap);
    return t;
  }
}
#endif

/* Sobe (se o quadro e novo ou a cor mudou) e desenha assMain. Fio GL. */
static int ass_main_desenhar(float x, float y, float alpha, int corAtiva, int cr, int cg, int cb,
                             int quadroNovo) {
  int i, alfa = assperf_get(AP_UPLOAD) != 0, cache = assperf_get(AP_TEXCACHE) != 0;
  int corChave = corAtiva ? (int)(((unsigned)cr << 16) | ((unsigned)cg << 8) | (unsigned)cb) : -1;
  unsigned recicla = (unsigned)assperf_get(AP_TEXANEL) + 1u;
  int reenviar = quadroNovo || !assMainEnviado || alfa != assMainFmt || cache != assMainCache ||
                 (!alfa && corChave != assMainCorChave);
  float kk = assMain.k > 1 ? (float)assMain.k : 1.0f, esc = assMain.esc > 0.01f ? assMain.esc : kk;
  assGlQuadro++;
  if (alfa != assMainFmt || cache != assMainCache) {
    /* Formato ou modo mudou no meio: nada do que esta na GPU serve. */
    ass_tc_apagar();
    assMainFmt = alfa; assMainCache = cache;
  }
  if (reenviar) {
    struct timespec t0, t1;
    size_t enviados = 0;
    int reusadas = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (assMain.count > assMainTexCap) {
      GLuint *nv = (GLuint *)realloc(assMainTex, (size_t)assMain.count * sizeof *nv);
      int *ni;
      if (!nv) return 0;
      assMainTex = nv;
      ni = (int *)realloc(assMainIdx, (size_t)assMain.count * sizeof *ni);
      if (!ni) return 0;
      assMainIdx = ni;
#if ASS_GPU
      { unsigned long long *ng = (unsigned long long *)realloc(assMainGk, (size_t)assMain.count * sizeof *ng);
        if (!ng) return 0;
        assMainGk = ng; }
#endif
      assMainTexCap = assMain.count;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
#if ASS_GPU
    { int temGpu = 0;
      for (i = 0; i < assMain.count && !temGpu; i++) temGpu = assMain.images[i].gpu;
      if (temGpu) assgpublur_lote_ini(assperf_get(AP_ESTAGIOS)); }
#endif
    for (i = 0; i < assMain.count; i++) {
      const AssCpuImage *im = &assMain.images[i];
      int r = 0;
#if ASS_GPU
      assMainGk[i] = 0;
      if (im->gpu) {
        assMainTex[i] = alfa ? ass_gpu_textura(&assMain, im, &assMainGk[i], recicla, &enviados, &assMainIdx[i]) : 0;
        continue;
      }
#endif
      if (cache) {
        /* RGBA leva a cor do usuario assada: ela entra na chave tambem. */
        unsigned long long ch = im->chave ? im->chave : ass_chave_bitmap(im->bitmap, im->w, im->h, alfa ? 0u : im->color);
        if (!alfa && corAtiva && im->type == IMAGE_TYPE_CHARACTER) ch = (ch ^ (unsigned long long)(unsigned)corChave) * 0x100000001B3ULL;
        assMainTex[i] = ass_tc_obter(im, ch ? ch : 1, alfa, recicla, corAtiva, cr, cg, cb, &r, &enviados,
                                     &assMainIdx[i]);
      } else {
        assMainTex[i] = ass_slot_obter(i, im, alfa, corAtiva, cr, cg, cb, &enviados);
        assMainIdx[i] = -1;
      }
      reusadas += r;
    }
#if ASS_GPU
    { AgbStats st;
      assgpublur_lote_fim();
      assgpublur_stats(&st);
      if (st.blurs || st.finais || st.reusos || st.falhas || assGpuCpu) {
        AssperfGpu g;
        int z;
        g.blurs = st.blurs; g.finais = st.finais; g.reusos = st.reusos; g.falhas = st.falhas; g.cpu = assGpuCpu;
        g.adiantados = st.adiantados; g.criadas = st.criadas; g.reaproveitadas = st.reaproveitadas;
        g.ms = st.ms; g.medido = st.medido; g.msGpu = st.msGpu; g.nGpu = st.nGpu;
        for (z = 0; z < 4; z++) { g.nTam[z] = st.nTam[z]; g.msTam[z] = st.msTam[z]; }
        assperf_med_gpublur(&g);
      }
      assGpuCpu = 0; }
#endif
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gfx_tex_esquecer(0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    assMainEnviado = 1; assMainCorChave = corChave;
    assperf_med_upload(ass_ms_entre(&t0, &t1), enviados, assMain.count, reusadas, quadroNovo);
  } else if (cache) {
    /* Quadro repetido: as texturas dele continuam "em uso" (a reciclagem
     * conta quadros GL sem desenho). */
    for (i = 0; i < assMain.count; i++) {
      if (assMainIdx[i] >= 0 && assMainIdx[i] < assTcN) assTc[assMainIdx[i]].uso = assGlQuadro;
#if ASS_GPU
      if (assMain.images[i].gpu) assgpublur_usar(assMainGk[i], assGlQuadro);
#endif
    }
  }
  for (i = 0; i < assMain.count; i++) {
    const AssCpuImage *im = &assMain.images[i];
    GLuint tex = assMainTex[i];
    float dx, dy;
    if (!tex) continue;
    gfx_tex_aspect_atual = 0.0f;
    /* Origem arredondada ao pixel do ALVO (kk); a imagem em pixels do
     * quadro do libass (esc = kk x resolucao), ampliada quando esc < kk. */
    dx = roundf((x + assMain.ox) * kk) / kk + (float)im->x / esc;
    dy = roundf((y + assMain.oy) * kk) / kk + (float)im->y / esc;
    if (alfa) {
      unsigned c = im->color;
      float r = (float)((c >> 24) & 255u) / 255.0f, g = (float)((c >> 16) & 255u) / 255.0f,
            b = (float)((c >> 8) & 255u) / 255.0f, op = (float)(255u - (c & 255u)) / 255.0f;
      if (corAtiva && im->type == IMAGE_TYPE_CHARACTER) { r = cr / 255.0f; g = cg / 255.0f; b = cb / 255.0f; }
      gfx_rect((GfxRect){ dx, dy, (float)im->w / esc, (float)im->h / esc }, tex, GFX_MARCA,
               0, 0, 0, 0, r, g, b, alpha * op);
    } else {
      gfx_rect((GfxRect){ dx, dy, (float)im->w / esc, (float)im->h / esc }, tex, GFX_TEXTO,
               0, 0, 0, 0, 1, 1, 1, alpha);
    }
  }
  gfx_tex_aspect_atual = 0.0f;
  return assMain.count;
}

/* ===== assperf: CARGA INCREMENTAL ("Batch loading = New lines only") =======
 *
 * Cada lote do mkvass entregava o documento INTEIRO (milhares de linhas de
 * letreiro quadro a quadro) e ass_carregar o re-lia com ass_read_memory sob
 * assTrava: o worker ficava parado a carga toda, 58 vezes em 20 s no log.
 * Aqui o documento e varrido FORA da trava (so o hash de cada linha) e so as
 * linhas que ainda nao estao na faixa do libass entram, com ass_process_data,
 * sob a trava por microssegundos.
 *
 * As contas sao de MULTICONJUNTO: uma linha que aparece duas vezes no
 * documento (letreiro em duas camadas iguais) entra duas vezes. incEv[i] e o
 * hash da linha de que veio o evento i da faixa, alinhado com
 * assTrack->events. "Drop past events": sai da faixa o evento que ja acabou
 * ha mais de N antes do playhead E que o documento nao traz mais (o mkvass o
 * despejou da janela). Se o documento ainda o traz, fica — o seek para tras
 * continua achando a fala.
 *
 * Trava: incTrava antes de assTrava, sempre. O cabecalho do documento (tudo
 * antes da primeira "Dialogue:") muda = faixa outra: volta a carga cheia. */
typedef struct { unsigned long long *k; int *v; unsigned *g; int cap, n; unsigned gen; } AssMapa;
static pthread_mutex_t incTrava = PTHREAD_MUTEX_INITIALIZER;
static int incPronto;
static unsigned incGeracao;
static unsigned long long incCab;
static unsigned long long *incEv;
static int incEvN, incEvCap;
static AssMapa incAdd, incDoc;

static unsigned long long ass_hash_bytes(const char *p, size_t n) {
  unsigned long long h = 0xCBF29CE484222325ULL ^ (unsigned long long)n;
  size_t i = 0;
  for (; i + 4 <= n; i += 4) { uint32_t v; memcpy(&v, p + i, 4); h = (h ^ v) * 0x100000001B3ULL; h ^= h >> 31; }
  for (; i < n; i++) h = (h ^ (unsigned char)p[i]) * 0x100000001B3ULL;
  return h ? h : 1;
}

static void mapa_limpar(AssMapa *m) { m->gen++; m->n = 0; if (!m->gen) { m->gen = 1; if (m->g) memset(m->g, 0, (size_t)m->cap * sizeof *m->g); } }

static int mapa_crescer(AssMapa *m) {
  int nc = m->cap ? m->cap * 2 : 4096, i;
  AssMapa novo; memset(&novo, 0, sizeof novo);
  novo.k = (unsigned long long *)calloc((size_t)nc, sizeof *novo.k);
  novo.v = (int *)calloc((size_t)nc, sizeof *novo.v);
  novo.g = (unsigned *)calloc((size_t)nc, sizeof *novo.g);
  if (!novo.k || !novo.v || !novo.g) { free(novo.k); free(novo.v); free(novo.g); return 0; }
  novo.cap = nc; novo.gen = m->gen ? m->gen : 1;
  for (i = 0; i < m->cap; i++) {
    if (m->g[i] != m->gen) continue;
    { unsigned j = (unsigned)(m->k[i] ^ (m->k[i] >> 32)) & (unsigned)(nc - 1);
      while (novo.g[j] == novo.gen) j = (j + 1) & (unsigned)(nc - 1);
      novo.k[j] = m->k[i]; novo.v[j] = m->v[i]; novo.g[j] = novo.gen; novo.n++; }
  }
  free(m->k); free(m->v); free(m->g);
  *m = novo;
  return 1;
}

/* Ponteiro para a contagem de `k` (criada com 0). NULL so sem memoria. */
static int *mapa_conta(AssMapa *m, unsigned long long k) {
  unsigned j;
  if (!m->gen) m->gen = 1;
  if ((m->n + 1) * 10 >= m->cap * 7 && !mapa_crescer(m)) return NULL;
  j = (unsigned)(k ^ (k >> 32)) & (unsigned)(m->cap - 1);
  while (m->g[j] == m->gen) {
    if (m->k[j] == k) return &m->v[j];
    j = (j + 1) & (unsigned)(m->cap - 1);
  }
  m->g[j] = m->gen; m->k[j] = k; m->v[j] = 0; m->n++;
  return &m->v[j];
}

static void ass_inc_esquecer(void) {
  pthread_mutex_lock(&incTrava);
  incPronto = 0; incEvN = 0; incCab = 0;
  pthread_mutex_unlock(&incTrava);
}

/* Bytes do cabecalho: tudo antes da primeira linha "Dialogue:". */
static size_t ass_inc_cabecalho(const char *c, size_t n) {
  size_t i = 0;
  while (i < n) {
    if (n - i >= 9 && !memcmp(c + i, "Dialogue:", 9)) return i;
    { const char *nl = (const char *)memchr(c + i, '\n', n - i);
      if (!nl) break;
      i = (size_t)(nl - c) + 1; }
  }
  return n;
}

static void ass_tailandes_de(ASS_Track *t, int de) {
  int i;
  if (!assVistoThai) return;
  for (i = de < 0 ? 0 : de; i < t->n_events; i++) {
    char *novo;
    if (!t->events[i].Text || !(novo = assrender_marcar_tailandes(t->events[i].Text))) continue;
    free(t->events[i].Text); t->events[i].Text = novo;
  }
}

static int ass_inc_ev_por(int n) {
  if (n <= incEvCap) return 1;
  { int nc = incEvCap ? incEvCap : 1024;
    unsigned long long *nv;
    while (nc < n) nc *= 2;
    nv = (unsigned long long *)realloc(incEv, (size_t)nc * sizeof *nv);
    if (!nv) return 0;
    incEv = nv; incEvCap = nc; }
  return 1;
}

static int ass_carregar(const char *corpo, size_t tamanho, unsigned geracao, int manter, int simples);

/* Um lote do mkvass. 1 = aplicado; 0 = geracao velha (descartado).
 * Cabecalho diferente ou faixa nao pronta: a carga cheia de sempre. */
static int ass_inc_lote(const char *corpo, size_t tamanho, unsigned geracao) {
  struct timespec t0, t1;
  size_t cabN, i;
  unsigned long long cab;
  size_t *novasO = NULL; int *novasN = NULL; unsigned long long *novasH = NULL;
  int nNovas = 0, capNovas = 0, reconstruir, removidos = 0, aceitas = 0, total = 0, ok = 0, desalinhou = 0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  cabN = ass_inc_cabecalho(corpo, tamanho);
  cab = ass_hash_bytes(corpo, cabN);
  pthread_mutex_lock(&incTrava);
  if (incGeracao != geracao || incCab != cab) {
    pthread_mutex_unlock(&incTrava);
    return ass_carregar(corpo, tamanho, geracao, 1, 0);
  }
  reconstruir = !incPronto;
  mapa_limpar(&incDoc);
  if (reconstruir) mapa_limpar(&incAdd);
  /* Varredura: so hashes, sem trava do libass. */
  for (i = cabN; i < tamanho;) {
    const char *l = corpo + i, *nl = (const char *)memchr(l, '\n', tamanho - i);
    size_t n = nl ? (size_t)(nl - l) : tamanho - i;
    i += n + (nl ? 1 : 0);
    if (n > 0 && l[n - 1] == '\r') n--;
    if (n < 10 || memcmp(l, "Dialogue:", 9)) continue;
    { unsigned long long h = ass_hash_bytes(l, n);
      int *d = mapa_conta(&incDoc, h), *a = mapa_conta(&incAdd, h);
      if (!d || !a) goto falta;
      (*d)++;
      if (*d <= *a) continue;
      (*a)++;
      if (nNovas == capNovas) {
        int nc = capNovas ? capNovas * 2 : 256;
        size_t *no = (size_t *)realloc(novasO, (size_t)nc * sizeof *no);
        int *nn; unsigned long long *nh;
        if (!no) goto falta;
        novasO = no;
        nn = (int *)realloc(novasN, (size_t)nc * sizeof *nn);
        if (!nn) goto falta;
        novasN = nn;
        nh = (unsigned long long *)realloc(novasH, (size_t)nc * sizeof *nh);
        if (!nh) goto falta;
        novasH = nh; capNovas = nc;
      }
      novasO[nNovas] = (size_t)(l - corpo); novasN[nNovas] = (int)n; novasH[nNovas] = h; nNovas++; }
  }
  pthread_mutex_lock(&assTrava);
  if (geracao != __atomic_load_n(&assGeracao, __ATOMIC_ACQUIRE) || !assTrack || assTrackGeracao != geracao ||
      __atomic_load_n(&assTextoSimples, __ATOMIC_ACQUIRE) || !assLib) {
    pthread_mutex_unlock(&assTrava);
    incPronto = 0;
    goto fim;
  }
  if (reconstruir) { ass_flush_events(assTrack); incEvN = 0; }
  if (incEvN != assTrack->n_events) {
    /* Desalinhou (nao deveria): refaz na proxima entrega. */
    pthread_mutex_unlock(&assTrava);
    incPronto = 0;
    goto fim;
  }
  { int antes = assTrack->n_events, k;
    for (k = 0; k < nNovas; k++) {
      int n0 = assTrack->n_events;
      ass_process_data(assTrack, (char *)(corpo + novasO[k]), novasN[k]);
      if (assTrack->n_events > n0 && ass_inc_ev_por(assTrack->n_events)) {
        while (incEvN < assTrack->n_events) incEv[incEvN++] = novasH[k];
        aceitas++;
      } else if (assTrack->n_events != incEvN) {
        /* Sem memoria para o hash: perde o alinhamento, refaz depois. */
        desalinhou = 1;
      }
    }
    ass_tailandes_de(assTrack, antes); }
  /* Drop past events. O playhead e o ultimo pedido do desenho. */
  { long poda = assperf_poda_ms();
    if (poda > 0 && !desalinhou && incEvN == assTrack->n_events) {
      long long playhead, limite;
      int k, w = 0;
      pthread_mutex_lock(&assFilaTrava); playhead = (long long)assPedidoMs; pthread_mutex_unlock(&assFilaTrava);
      limite = playhead - poda;
      for (k = 0; k < assTrack->n_events; k++) {
        ASS_Event *e = &assTrack->events[k];
        int *a = mapa_conta(&incAdd, incEv[k]), *d = mapa_conta(&incDoc, incEv[k]);
        if (a && d && *a > *d && e->Start + e->Duration < limite) {
          ass_free_event(assTrack, k);
          (*a)--; removidos++;
          continue;
        }
        if (w != k) { assTrack->events[w] = assTrack->events[k]; incEv[w] = incEv[k]; }
        w++;
      }
      assTrack->n_events = w; incEvN = w;
    } }
  total = assTrack->n_events;
  assEventos = total;
  assCoberturaIni = LLONG_MAX; assCoberturaFim = LLONG_MIN;
  { int k;
    for (k = 0; k < total; k++) {
      long long ini = assTrack->events[k].Start, fimE = ini + assTrack->events[k].Duration;
      if (ini < assCoberturaIni) assCoberturaIni = ini;
      if (fimE > assCoberturaFim) assCoberturaFim = fimE;
    } }
  incPronto = !desalinhou && incEvN == assTrack->n_events;
  pthread_mutex_unlock(&assTrava);
  if (aceitas || removidos || reconstruir) {
    pthread_mutex_lock(&assFilaTrava);
    assTemUltimoPedido = 0;   // o worker ve o documento novo mesmo pausado
    pthread_mutex_unlock(&assFilaTrava);
  }
  ok = 1;
  goto fim;
falta:
  incPronto = 0;   // sem memoria no meio: a proxima entrega reconstroi
fim:
  pthread_mutex_unlock(&incTrava);
  free(novasO); free(novasN); free(novasH);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  if (ok) {
    assperf_med_lote(ass_ms_entre(&t0, &t1), aceitas, total, reconstruir);
    if (removidos) assperf_med_poda(removidos);
  }
  return ok;
}

static int ass_carregar(const char *corpo, size_t tamanho, unsigned geracao, int manter, int simples) {
  ASS_Track *track;
  char *copia;
  if (!corpo || !tamanho) return 0;
  { /* assperf: a carga cheia redefine a base da carga incremental. */
    size_t cabN = ass_inc_cabecalho(corpo, tamanho);
    pthread_mutex_lock(&incTrava);
    incPronto = 0; incEvN = 0; incGeracao = geracao; incCab = simples ? 0 : ass_hash_bytes(corpo, cabN);
    pthread_mutex_unlock(&incTrava); }
  pthread_mutex_lock(&assTrava);
  if (geracao != __atomic_load_n(&assGeracao, __ATOMIC_ACQUIRE)) {
    pthread_mutex_unlock(&assTrava); return 0;
  }
  ass_iniciar_locked(!simples);
  if (!assLib || !assRenderer) { pthread_mutex_unlock(&assTrava); return 0; }
  copia = (char *)malloc(tamanho + 1u);
  if (!copia) { ass_diag("libass: memoria insuficiente para documento"); pthread_mutex_unlock(&assTrava); return 0; }
  memcpy(copia, corpo, tamanho); copia[tamanho] = 0;
  track = ass_read_memory(assLib, copia, tamanho, "UTF-8");
  free(copia);
  if (!track || track->n_events <= 0) {
    if (track) ass_free_track(track);
    ass_diag("libass: documento ASS invalido ou incompleto");
    pthread_mutex_unlock(&assTrava); return 0;
  }
  if (simples) {
    /* Converted plain text follows Unicode layout, not VSFilter's legacy
     * per-style-run layout. Keep authored ASS compatibility unchanged. */
    ass_track_set_feature(track, ASS_FEATURE_WHOLE_TEXT_LAYOUT, 1);
    ass_track_set_feature(track, ASS_FEATURE_BIDI_BRACKETS, 1);
  }
  if (geracao != __atomic_load_n(&assGeracao, __ATOMIC_ACQUIRE)) {
    ass_free_track(track);
    pthread_mutex_unlock(&assTrava); return 0;
  }
  ass_authored_styles_free();
  if (assTrack) ass_free_track(assTrack);
  assTrack = track;
  ass_trilha_tailandes(track);
  plainTimingValid = 0;
  /* Authored ASS retains its own margins after a converted plain track. */
  ass_set_selective_style_override_enabled(assRenderer, 0);
  __atomic_store_n(&assTextoSimples, simples, __ATOMIC_RELEASE);
  plainAplValido = 0;
  assEventos = track->n_events;
  assResolucaoFonte = 0;
  assCoberturaIni = LLONG_MAX; assCoberturaFim = LLONG_MIN;
  { int i;
    for (i = 0; i < track->n_events; i++) {
      long long ini = track->events[i].Start;
      long long fim = ini + track->events[i].Duration;
      if (ini < assCoberturaIni) assCoberturaIni = ini;
      if (fim > assCoberturaFim) assCoberturaFim = fim;
    } }
  __atomic_store_n(&assTrackGeracao, geracao, __ATOMIC_RELEASE);
  __atomic_store_n(&assTrackAtivo, 1, __ATOMIC_RELEASE);
  ass_diag(simples ? "libass: texto RTL simples ativo" : "libass: ASS completo ativo");
  pthread_mutex_unlock(&assTrava);
  assperf_faixa_ativa(1);
  if (!ass_worker_iniciar()) {
    pthread_mutex_lock(&assTrava);
    if (assTrack == track) { ass_authored_styles_free(); ass_free_track(assTrack); assTrack = NULL; }
    assEventos = 0;
    __atomic_store_n(&assTrackAtivo, 0, __ATOMIC_RELEASE);
    ass_diag("libass: worker de render indisponivel");
    pthread_mutex_unlock(&assTrava);
    return 0;
  }
  pthread_mutex_lock(&assFilaTrava);
  if (manter) {
    /* Mesma faixa, mais eventos (o mkvass entregou um lote). O quadro em tela
     * CONTINUA: apagar aqui era o outro "pisca", um por lote colhido. So o
     * pedido e refeito, para o worker ver o documento novo mesmo pausado. */
    assTemUltimoPedido = 0;
  } else {
    assPedidoPendente = 0; ++assEpoch; ++assSerial; assPedidoSerial = assSerial;
    ass_fila_limpar_locked();
    ass_frame_liberar(&assAtual); assAtualValido = 0;
    assTemUltimoPedido = 0; assUltimoRenderUs = 0; assBytesQuadro = 0;
    assTexResetar = 1;
  }
  pthread_mutex_unlock(&assFilaTrava);
  return 1;
}

int assrender_carregar(const char *corpo, size_t tamanho, unsigned geracao) {
  struct timespec t0, t1; int r;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  pthread_mutex_lock(&assTrava);
  if (assGlifoMsgs > 6)
    fprintf(stderr, "[libass] %d more missing-glyph messages hidden on the previous track\n", assGlifoMsgs - 6);
  assGlifoMsgs = 0;
  pthread_mutex_unlock(&assTrava);
  r = ass_carregar(corpo, tamanho, geracao, 0, 0);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  if (r) assperf_med_lote(ass_ms_entre(&t0, &t1), 0, assEventos, 1);
  return r;
}

#ifdef NV_TPK
#include <sys/stat.h>
#include "dados.h"
#include "rede.h"
#ifndef NV_VERSAO
#define NV_VERSAO "dev"
#endif
/* Garante a fonte arabe completa para o texto simples RTL (ver ass_carregar_pasta_locked).
 * Chamada fora de assTrava, no fio que baixou a legenda: ~175 KB, uma vez por instalacao. */
static void ass_garantir_naskh(void) {
  static int pronto, tentativas;
  const char *arte = getenv("NUVIO_TPK_ARTE"), *v;
  char caminho[700], pasta[640], url[320], tmp[720];
  char *buf; long n = 0; FILE *f;
  if (pronto || tentativas >= 1) return;
  if (arte && arte[0]) {
    snprintf(caminho, sizeof caminho, "%s/../fonts/NotoNaskhArabic-Regular.ttf", arte);
    if (access(caminho, R_OK) == 0) { pronto = 1; return; }
  }
  if (!dados_dir() || !dados_dir()[0]) return;
  if (!dados_caminho(pasta, sizeof pasta, "fontes")) goto falhou;
  snprintf(caminho, sizeof caminho, "%s/NotoNaskhArabic-Regular.ttf", pasta);
  if (access(caminho, R_OK) != 0) {
    if (NV_VERSAO[0] < '0' || NV_VERSAO[0] > '9') { pronto = 1; return; }
    for (v = NV_VERSAO; *v; v++) if (!((*v >= '0' && *v <= '9') || *v == '.')) { pronto = 1; return; }
    tentativas++;
    mkdir(pasta, 0755);
    snprintf(url, sizeof url, "https://raw.githubusercontent.com/iqui27/nuvio-native-legacy/v%s/deploy/app/fonts/NotoNaskhArabic-Regular.ttf", NV_VERSAO);
    buf = rede_baixar_bin(url, 20, &n);
    if (!buf || n < 100000 || n > 400000 || !assrender_bytes_sao_fonte((const unsigned char *)buf, 4)) {
      fprintf(stderr, "[libass] NotoNaskhArabic-Regular nao veio da tag v%s (%ld bytes)\n", NV_VERSAO, buf ? n : -1L);
      free(buf); goto falhou;
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", caminho);
    f = fopen(tmp, "wb");
    if (!f || fwrite(buf, 1, (size_t)n, f) != (size_t)n) { if (f) fclose(f); free(buf); remove(tmp); goto falhou; }
    fclose(f); free(buf);
    if (rename(tmp, caminho) != 0) { remove(tmp); goto falhou; }
    fprintf(stderr, "[libass] NotoNaskhArabic-Regular baixada da tag v%s (%ld bytes)\n", NV_VERSAO, n);
    /* Negrito real (#335), de cortesia: sem ele o libass cai no "sem negrito"
     * para trilhas com arabe, que e o comportamento antigo seguro. */
    snprintf(url, sizeof url, "https://raw.githubusercontent.com/iqui27/nuvio-native-legacy/v%s/deploy/app/fonts/NotoNaskhArabic-Bold.ttf", NV_VERSAO);
    snprintf(caminho, sizeof caminho, "%s/NotoNaskhArabic-Bold.ttf", pasta);
    buf = rede_baixar_bin(url, 20, &n);
    if (buf && n > 100000 && n < 400000 && assrender_bytes_sao_fonte((const unsigned char *)buf, 4)) {
      snprintf(tmp, sizeof tmp, "%s.tmp", caminho);
      f = fopen(tmp, "wb");
      if (f && fwrite(buf, 1, (size_t)n, f) == (size_t)n) { fclose(f); if (rename(tmp, caminho)) remove(tmp); }
      else { if (f) fclose(f); remove(tmp); }
    }
    free(buf);
  }
  pronto = 1;
  pthread_mutex_lock(&assTrava);
  snprintf(assPastaExtra, sizeof assPastaExtra, "%s", pasta);
  if (assLib) {
    /* A biblioteca ja subiu sem ela: so esta pasta entra (nao duplica as outras
     * fontes) e o seletor e refeito com a lista completa. */
    assrender_ler_pasta_fontes(assPastaExtra, ass_fonte_da_pasta, NULL, NULL);
    ass_aplicar_fontes_locked();
  }
  pthread_mutex_unlock(&assTrava);
  return;
falhou:
  pthread_mutex_lock(&assTrava);
  assNaskhFalhou = 1;
  if (assLib) { ass_subset_naskh_locked(); ass_aplicar_fontes_locked(); }
  pthread_mutex_unlock(&assTrava);
}
#endif

int assrender_fonte_local_transacao(const char *caminho, int (*confirmar)(void *), void *u) {
  if (!caminho) caminho = "";
  if (strlen(caminho) >= sizeof assFonteLocal) return 0;
  pthread_mutex_lock(&assTrava);
  int mudou = strcmp(assFonteLocal, caminho) != 0 || (assLib && caminho[0] && !assFonteRegistrada);
  if (mudou && assLib && !assAtuaisCompletas) { pthread_mutex_unlock(&assTrava); return 0; }
  size_t n = 0; unsigned char *b = NULL;
  if (mudou && caminho[0]) {
    b = ass_fonte_local_ler(caminho, &n);
    if (!b) { pthread_mutex_unlock(&assTrava); return 0; }
  }
  if (confirmar && !confirmar(u)) { free(b); pthread_mutex_unlock(&assTrava); return 0; }
  if (mudou) {
    snprintf(assFonteLocal, sizeof assFonteLocal, "%s", caminho);
    if (assLib) ass_fontes_reconstruir_locked(b, n);
    else assFonteRegistrada = 0;
  }
  free(b);
  pthread_mutex_unlock(&assTrava);
  if (mudou) {
    pthread_mutex_lock(&assFilaTrava);
    assPedidoPendente = 0; ++assEpoch; assTemUltimoPedido = 0;
    ass_fila_limpar_locked();
    ass_frame_liberar(&assAtual); assAtualValido = 0; assTexResetar = 1;
    pthread_mutex_unlock(&assFilaTrava);
  }
  return 1;
}
int assrender_fonte_local_ativa(const char *caminho) {
  pthread_mutex_lock(&assTrava);
  int ativa = caminho && caminho[0] && assFonteRegistrada && !strcmp(assFonteLocal, caminho);
  pthread_mutex_unlock(&assTrava);
  return ativa;
}

int assrender_carregar_texto(const char *corpo, size_t tamanho, unsigned geracao) {
#ifdef NV_TPK
  ass_garantir_naskh();
#endif
  return ass_carregar(corpo, tamanho, geracao, 0, 1);
}

int assrender_texto_simples(void) {
  return assrender_ativo() && __atomic_load_n(&assTextoSimples, __ATOMIC_ACQUIRE);
}

void assrender_definir_texto_estilo(const PlainAssStyle *style) {
  if (!style) return;
  pthread_mutex_lock(&assFilaTrava);
  if (!plainPedValido || memcmp(style, &plainPed, sizeof *style)) {
    plainPed = *style; plainPedValido = 1;
    ++assEpoch; assTemUltimoPedido = 0;
  }
  pthread_mutex_unlock(&assFilaTrava);
}

int assrender_atualizar(const char *corpo, size_t tamanho, unsigned geracao) {
  int mesma, simples;
  pthread_mutex_lock(&assTrava);
  mesma = assTrack && assTrackGeracao == geracao;
  simples = __atomic_load_n(&assTextoSimples, __ATOMIC_ACQUIRE);
  pthread_mutex_unlock(&assTrava);
  /* assperf: so as linhas novas do lote (ver ass_inc_lote). */
  if (mesma && !simples && corpo && tamanho && assperf_get(AP_INCREMENTAL))
    return ass_inc_lote(corpo, tamanho, geracao);
  { struct timespec t0, t1; int r;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    r = ass_carregar(corpo, tamanho, geracao, mesma, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (r) assperf_med_lote(ass_ms_entre(&t0, &t1), 0, assEventos, 1);
    return r; }
}

void assrender_limpar(void) {
  ass_inc_esquecer();
  assperf_faixa_ativa(0);
  pthread_mutex_lock(&assTrava);
  ass_authored_styles_free();
  if (assTrack) { ass_free_track(assTrack); assTrack = NULL; }
  assEventos = 0;
  assCoberturaIni = assCoberturaFim = 0;
  __atomic_store_n(&assTrackGeracao, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&assTrackAtivo, 0, __ATOMIC_RELEASE);
  ass_diag("libass: sem faixa");
  pthread_mutex_unlock(&assTrava);
  pthread_mutex_lock(&assFilaTrava);
  assPedidoPendente = 0; ++assEpoch; ++assSerial; assPedidoSerial = assSerial;
  ass_fila_limpar_locked();
  ass_frame_liberar(&assAtual); assAtualValido = 0;
  assTemUltimoPedido = 0; assUltimoRenderUs = 0; assBytesQuadro = 0;
  assTexResetar = 1;
  pthread_mutex_unlock(&assFilaTrava);
}

void assrender_limpar_fontes(void) {
  ass_inc_esquecer();
  assperf_faixa_ativa(0);
  pthread_mutex_lock(&assTrava);
  ass_atuais_limpar_locked();
  ass_authored_styles_free();
  if (assTrack) { ass_free_track(assTrack); assTrack = NULL; }
  assEventos = 0; assFontes = 0; assResolucaoFonte = 0;
  assCoberturaIni = assCoberturaFim = 0;
  __atomic_store_n(&assTrackGeracao, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&assTrackAtivo, 0, __ATOMIC_RELEASE);
  if (assLib && assFonteLocal[0]) {
    size_t n; unsigned char *b = ass_fonte_local_ler(assFonteLocal, &n);
    ass_fontes_reconstruir_locked(b, n); free(b);
  }
  /* SEM ass_clear_fonts: ver a nota de assFontesVistas. As fontes da faixa
   * anterior ficam na biblioteca; so sao usadas se a proxima pedir o mesmo
   * nome. assFontes (diagnostico) volta a contar as desta faixa. */
  pthread_mutex_unlock(&assTrava);
  pthread_mutex_lock(&assFilaTrava);
  assPedidoPendente = 0; ++assEpoch; ++assSerial; assPedidoSerial = assSerial;
  ass_fila_limpar_locked();
  ass_frame_liberar(&assAtual); assAtualValido = 0;
  assTemUltimoPedido = 0; assUltimoRenderUs = 0; assBytesQuadro = 0;
  assTexResetar = 1;
  pthread_mutex_unlock(&assFilaTrava);
}

// #415: no aviso de memoria, esvazia os caches do libass — o loop
// "Glyph not found -> selecting one more font" encheu ~190 MB na G4 sem
// que gpu-cache ou texturas mostrassem. ass_set_cache_limits NAO serve:
// 0 vira o default e o corte so acontece preguiçosamente no proximo
// ass_render_frame. O que despeja na hora e o ass_reconfigure dentro de
// ass_set_fonts (bitmap/composite/outline/font/metrics + fontselect
// refeito, o que tambem encerra o loop de fallback). Custa uma releitura
// da pasta de fontes; aceitavel porque so roda no aviso.
void assrender_aliviar(void) {
  pthread_mutex_lock(&assTrava);
  if (assRenderer) ass_aplicar_fontes_locked();
  pthread_mutex_unlock(&assTrava);
}

int assrender_adicionar_fonte(const char *nome, const void *dados, size_t tamanho) {
  if (!nome || !*nome || !dados || !tamanho || tamanho > (size_t)INT_MAX) return 0;
  pthread_mutex_lock(&assTrava);
  ass_iniciar_locked(1);
  if (assLib) {
    ass_atual_guardar_locked(nome, dados, tamanho);
    unsigned long long am = ass_amostra_fonte((const unsigned char *)dados, tamanho);
    int i, visto = 0;
    for (i = 0; i < assNFontesVistas; i++)
      if (assFontesVistas[i].tam == tamanho && assFontesVistas[i].amostra == am &&
          !strncmp(assFontesVistas[i].nome, nome, sizeof assFontesVistas[i].nome - 1)) { visto = 1; break; }
    if (!visto) {
      /* TETO: episodios em sequencia com fontes DIFERENTES fariam a lista
       * crescer sem fim. Passou dele, reinicia tudo — e ai sim ass_set_fonts
       * logo apos o ass_clear_fonts, que e o par que mantem a conta do
       * ass_start_frame coerente. Nao ha faixa sendo desenhada com fonte
       * velha: adicionar fonte so acontece antes de carregar o documento. */
      if (assNFontesVistas >= ASS_FONTES_MAX ||
          assBytesFontesVistas + tamanho > ASS_FONTES_TETO_BYTES) {
        ass_clear_fonts(assLib);
        // ass_clear_fonts removes every memory face, including the imported
        // device-local selection. Restore it before attachments and system
        // fonts so the selection keeps the same precedence after rollover.
        if (assFonteLocal[0]) {
          size_t localN = 0;
          unsigned char *local = ass_fonte_local_ler(assFonteLocal, &localN);
          if (local) ass_add_font(assLib, assFonteLocal, (const char *)local, (int)localN);
          assFonteRegistrada = local != NULL;
          free(local);
        } else assFonteRegistrada = 0;
        ass_carregar_pasta_locked();
        ass_aplicar_fontes_locked();
        assNFontesVistas = 0; assBytesFontesVistas = 0;
      }
      ass_add_font(assLib, nome, (const char *)dados, (int)tamanho);
      { AssFonteVista *v = &assFontesVistas[assNFontesVistas++];
        snprintf(v->nome, sizeof v->nome, "%s", nome);
        v->tam = tamanho; v->amostra = am; }
      assBytesFontesVistas += tamanho;
    }
    assFontes++;
  }
  { int ok = assLib != NULL; pthread_mutex_unlock(&assTrava); return ok; }
}

/* #335: o layout do app e sempre 1920x1080. Com a interface em 4K o alvo tem
 * 3840x2160, e um quadro do libass feito em 1920x1080 era esticado 2x com filtro
 * linear: a legenda ASS saia mole. Agora o libass renderiza no tamanho REAL do
 * alvo (k = pixels por unidade, 1 ou 2) e o quadro volta a ser desenhado 1:1.
 * Quem desenha informa (player.c); sem informar, k = 1. */
static int layPxUnid = 1;
void assrender_pixels_por_unidade(float f) { layPxUnid = f >= 1.5f ? 2 : 1; }

void assrender_definir_layout(float x, float y, float w, float h,
                              int videoW, int videoH, double escalaFonte) {
  int lw = (int)lroundf(w), lh = (int)lroundf(h), k;
  if (lw < 16 || lh < 16) return;
  if (videoW < 2 || videoH < 2) { videoW = lw; videoH = lh; }
  if (!(escalaFonte > 0.1 && escalaFonte < 4.0)) escalaFonte = 1.0;
  k = layPxUnid;
  pthread_mutex_lock(&assFilaTrava);
  if (lw != layPedW || lh != layPedH || videoW != layPedVW || videoH != layPedVH ||
      k != layPedK || escalaFonte != layPedEscala || fabsf(x - layPedX) > 0.5f || fabsf(y - layPedY) > 0.5f) {
    layPedX = x; layPedY = y; layPedW = lw; layPedH = lh;
    layPedVW = videoW; layPedVH = videoH; layPedEscala = escalaFonte; layPedK = k;
    /* O quadro em tela fica ate o novo chegar (sem piscar), mas nada
     * renderizado com o layout velho pode ser publicado depois disto, e o
     * proximo desenhar pede um render mesmo com o video pausado. */
    ++assEpoch;
    assTemUltimoPedido = 0;
  }
  pthread_mutex_unlock(&assFilaTrava);
}

void assrender_definir_cor(int enabled, int r, int g, int b) {
  pthread_mutex_lock(&assFilaTrava);
  assCorAtiva = !!enabled;
  assCorR = r < 0 ? 0 : r > 255 ? 255 : r;
  assCorG = g < 0 ? 0 : g > 255 ? 255 : g;
  assCorB = b < 0 ? 0 : b > 255 ? 255 : b;
  pthread_mutex_unlock(&assFilaTrava);
}

void assrender_aplicar_invalidacao(void) {
  pthread_mutex_lock(&assFilaTrava);
  if (assTexResetar) {
    assTexResetar = 0;
    ass_apagar_texturas_locked();
    assMainResetar = 1;   // o caminho novo tambem solta o quadro e as texturas dele
  }
  pthread_mutex_unlock(&assFilaTrava);
}

static double ass_mono(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Medidas por quadro desenhado, sob assFilaTrava. `agora` e o relogio do
 * video em ms (o mesmo que foi pedido ao libass). */
static void ass_medir_locked(long long agora, int temTexto, long long inicioMax) {
  double m = ass_mono();
  assMed.quadros++;
  if (temTexto) {
    if (!assMed.textoAnterior && assMed.ultimoTextoMs &&
        agora - assMed.ultimoTextoMs < 300 && agora >= assMed.ultimoTextoMs) {
      /* Texto -> vazio -> texto em menos de 300 ms: e o "pisca". Uma fala
       * que termina e outra que comeca nao passam por aqui, a menos que o
       * proprio arquivo tenha esse buraco (raro: fansub cola as falas). */
      assMed.piscas++; assMed.vaziosPisca += assMed.vazios;
    }
    if (inicioMax >= 0 && inicioMax > assMed.ultimoInicioLogado) {
      long long atraso = agora - inicioMax;
      assMed.ultimoInicioLogado = inicioMax;
      assMed.falas++; assMed.atrasoSoma += atraso;
      if (atraso > assMed.atrasoMax) assMed.atrasoMax = atraso;
      /* Uma linha por fala que entra: Start do evento, quando apareceu no
       * relogio do video, e a diferenca. E a medida do "atrasada". */
      printf("[ass] fala %lld.%03lld apareceu em %lld.%03lld (%+lld ms)\n",
             inicioMax / 1000, inicioMax % 1000, agora / 1000, agora % 1000, atraso);
    }
    assMed.ultimoTextoMs = agora; assMed.vazios = 0;
  } else assMed.vazios++;
  assMed.textoAnterior = temTexto;
  if (m - assMed.ultimoRelatorio >= 10.0) {
    if (assMed.ultimoRelatorio > 0)
      printf("[ass] 10s: quadros=%ld trocas=%ld renders=%ld iguais=%ld render medio=%.1fms max=%.1fms "
             "piscas=%ld (vazios=%ld) falas=%ld atraso medio=%lldms max=%lldms eventos=%d\n",
             assMed.quadros, assMed.trocas, assMed.renders, assMed.iguais,
             assMed.renders ? assMed.renderSomaUs / 1000.0 / assMed.renders : 0.0,
             assMed.renderMaxUs / 1000.0, assMed.piscas, assMed.vaziosPisca, assMed.falas,
             assMed.falas ? assMed.atrasoSoma / assMed.falas : 0, assMed.atrasoMax, assEventos);
    fflush(stdout);
    assMed.quadros = assMed.trocas = assMed.renders = assMed.iguais = 0;
    assMed.piscas = assMed.vaziosPisca = assMed.falas = 0;
    assMed.renderSomaUs = assMed.renderMaxUs = 0;
    assMed.atrasoSoma = 0; assMed.atrasoMax = 0;
    assMed.ultimoRelatorio = m;
  }
}

#if defined(NV_TPK) && defined(ASS_NV_API)
/* ===== assperf: ASS BENCH (TPK) =============================================
 * A mesma carga a cada build: os 10 s ANTES do instante em que foi pedido
 * (segundo inteiro), ja colhidos, re-renderizados fora da tela em uma grade de
 * 41,7 ms (240 quadros, 23,976 fps) por um ASS_Renderer PROPRIO, criado frio
 * (caches vazios) com o mesmo tamanho, escala, hinting, shaper e caches do
 * principal. A trava da faixa e pega quadro a quadro, entao o desenho, a
 * colheita e o render normal seguem; com o video pausado nada compete.
 * "C vs NEON" faz dois passes (um renderer novo cada) e compara as imagens
 * byte a byte. Nada vai para a rede: so os eventos que ja estao no libass. */
#define ASS_BENCH_QUADROS 240

typedef struct {
  double totalMs, maxMs, stageMs[8];
  unsigned quadros, comLegenda, imagens, hit[3], miss[3];
  int motor;
  unsigned comBlur, adiados, keptBe, keptClip;   // libass 0005 (passada com o blur adiado)
} AssBenchRes;

typedef struct { int modo; long long fim; unsigned geracao; } AssBenchPed;

static unsigned long long ass_bench_hash(ASS_Image *img, unsigned *n) {
  unsigned long long h = 1469598103934665603ULL;
  *n = 0;
  for (; img; img = img->next) {
    int y, x, hdr[5];
    hdr[0] = img->w; hdr[1] = img->h; hdr[2] = img->dst_x; hdr[3] = img->dst_y; hdr[4] = (int)img->color;
    for (x = 0; x < (int)sizeof hdr; x++) { h ^= ((unsigned char *)hdr)[x]; h *= 1099511628211ULL; }
    for (y = 0; y < img->h; y++) {
      const unsigned char *b = img->bitmap + (size_t)y * (size_t)img->stride;
      for (x = 0; x < img->w; x++) { h ^= b[x]; h *= 1099511628211ULL; }
    }
    (*n)++;
  }
  return h;
}

/* Um ASS_Renderer novo (caches vazios) com tudo o que o principal usa: o
 * bench e o snapshot desenham o mesmo que a tela. Sob assTrava. */
static ASS_Renderer *ass_renderer_igual_locked(void) {
  static const int glifos[4] = { 0, 1000, 4000, 10000 };
  static const int cacheMb[4] = { 32, 64, 128, 16 };
  static const int hinting[4] = { ASS_HINTING_LIGHT, ASS_HINTING_NONE, ASS_HINTING_NORMAL, ASS_HINTING_NATIVE };
  ASS_Renderer *r = ass_renderer_init(assLib);
  if (!r) return NULL;
  ass_set_fonts(r, assFallbackFont[0] ? assFallbackFont : NULL, "Arial", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
  ass_set_frame_size(r, layAplW, layAplH);
  ass_set_storage_size(r, layAplVW > 0 ? layAplVW : layAplW, layAplVH > 0 ? layAplVH : layAplH);
  ass_set_font_scale(r, layAplEscala > 0 ? layAplEscala : 1.0);
  ass_set_cache_limits(r, glifos[assperf_get(AP_GLIFOS) & 3], cacheMb[assperf_get(AP_CACHE) & 3]);
  ass_set_hinting(r, (ASS_Hinting)hinting[assperf_get(AP_HINTING) & 3]);
  ass_set_shaper(r, assperf_get(AP_SHAPER) ? ASS_SHAPING_SIMPLE : ASS_SHAPING_COMPLEX);
  ass_set_selective_style_override_enabled(r, ass_estilo_autor_locked() ? ASS_OVERRIDE_BIT_SELECTIVE_FONT_SCALE : 0);
  return r;
}

/* 1 = completo; 0 = abortado (motivo em `motivo`). */
static int ass_bench_passe(int simd, int adiar, const AssBenchPed *pd, unsigned long long *hashes,
                           AssBenchRes *res, char *motivo, size_t nm) {
  ASS_Renderer *r;
  unsigned fontes;
  long long ini = pd->fim - 10000;
  int i, ok = 1;
  memset(res, 0, sizeof *res);
  if (ini < 0) ini = 0;
  pthread_mutex_lock(&assTrava);
  if (!assLib || !assRenderer || !assTrack || pd->geracao != __atomic_load_n(&assGeracao, __ATOMIC_ACQUIRE) ||
      assTrackGeracao != pd->geracao || layAplW <= 0 || layAplH <= 0) {
    pthread_mutex_unlock(&assTrava);
    snprintf(motivo, nm, "no ASS track drawing");
    return 0;
  }
  r = ass_renderer_igual_locked();
  if (!r) { pthread_mutex_unlock(&assTrava); snprintf(motivo, nm, "ass_renderer_init failed"); return 0; }
  res->motor = ass_nv_set_engine(r, simd);
#if ASS_GPU
  ass_nv_set_deferred_blur(r, adiar);
#else
  (void)adiar;
#endif
  fontes = assFontesVersao;
  pthread_mutex_unlock(&assTrava);

  for (i = 0; i < ASS_BENCH_QUADROS; i++) {
    long long t = ini + ((long long)i * 1001 + 12) / 24;
    ASS_Image *img;
    ASS_NvPerf p;
    struct timespec a, b;
    double ms;
    unsigned n;
    int k;
    pthread_mutex_lock(&assTrava);
    if (!assTrack || pd->geracao != __atomic_load_n(&assGeracao, __ATOMIC_ACQUIRE) ||
        assTrackGeracao != pd->geracao || fontes != assFontesVersao) {
      pthread_mutex_unlock(&assTrava);
      snprintf(motivo, nm, "the track or its fonts changed at frame %d", i);
      ok = 0;
      break;
    }
    ass_nv_perf_take(&p);   // o que o render normal deixou (ele colhe logo depois do dele)
    clock_gettime(CLOCK_MONOTONIC, &a);
    img = ass_render_frame(r, assTrack, t, NULL);
    clock_gettime(CLOCK_MONOTONIC, &b);
    ass_nv_perf_take(&p);
    pthread_mutex_unlock(&assTrava);
    /* As imagens sao do renderer do bench: validas ate o proximo render dele. */
    if (hashes) hashes[i] = ass_bench_hash(img, &n);
    else { n = 0; for (; img; img = img->next) n++; }
    ms = ass_ms_entre(&a, &b);
    res->totalMs += ms;
    if (ms > res->maxMs) res->maxMs = ms;
    for (k = 0; k < 8; k++) res->stageMs[k] += p.ns_stage[k] / 1e6;
    for (k = 0; k < 3; k++) { res->hit[k] += p.cache_hit[k]; res->miss[k] += p.cache_miss[k]; }
    res->quadros++;
    res->imagens += n;
    if (n) res->comLegenda++;
  }
  pthread_mutex_lock(&assTrava);
#if ASS_GPU
  { ASS_NvDeferStats ds;
    ass_nv_defer_stats_take(r, &ds);
    res->comBlur = ds.blurred; res->adiados = ds.deferred; res->keptBe = ds.kept_be; res->keptClip = ds.kept_clip; }
#endif
  ass_renderer_done(r);
  pthread_mutex_unlock(&assTrava);
  return ok;
}

static void ass_bench_logar(const char *nome, const AssBenchRes *r) {
  char per[200];
  size_t o = 0;
  int i;
  double f = r->quadros ? (double)r->quadros : 1.0;
  per[0] = 0;
  for (i = 1; i <= 8 && o < sizeof per; i++) {
    int s = i & 7;
    o += (size_t)snprintf(per + o, sizeof per - o, "%s%s %.1f", i > 1 ? " " : "", assperf_estagio_nome[s], r->stageMs[s] / f);
  }
  printf("[assperf] bench %s: total %.0f ms | avg %.1f max %.1f ms/frame | %u of %u frames with subtitles, %u images"
         " | ms/frame: %s | bitmap cache hit %d%%\n",
         nome, r->totalMs, r->totalMs / f, r->maxMs, r->comLegenda, r->quadros, r->imagens, per,
         r->hit[1] + r->miss[1] ? (int)(100.0 * r->hit[1] / (r->hit[1] + r->miss[1]) + 0.5) : 0);
}

static void *ass_bench_fio(void *u) {
  AssBenchPed pd = *(AssBenchPed *)u;
  AssBenchRes rc, rn;
  unsigned long long *hc = NULL, *hn = NULL;
  char motivo[120] = "", resumo[160];
  int okC = 0, okN = 0, simd = assperf_get(AP_MOTOR) ? 1 : 0, w, h;
  long long ini = pd.fim - 10000 < 0 ? 0 : pd.fim - 10000;
  free(u);
  ass_globais_aplicar();   // os fios do blur da folha valem para este bench, mesmo pausado
  pthread_mutex_lock(&assTrava);
  assBenchAtivo = 1;
  ass_nv_perf_enable(1);
  w = layAplW; h = layAplH;
  pthread_mutex_unlock(&assTrava);
  printf("[assperf] bench: start | window %.1f-%.1f s of this track | %d frames on the 41.7 ms grid | %dx%d | cold caches"
         " | mode=%s | engine option=%s | blur threads=%d (panel %d)\n",
         ini / 1000.0, pd.fim / 1000.0, ASS_BENCH_QUADROS, w, h,
         pd.modo == 2 ? "C vs NEON" : "this engine", simd ? "NEON" : "C", ass_nv_get_threads(), assperf_fios());
  fflush(stdout);
  if (pd.modo == 2) {
    hc = calloc(ASS_BENCH_QUADROS, sizeof *hc);
    hn = calloc(ASS_BENCH_QUADROS, sizeof *hn);
    if (!hc || !hn) snprintf(motivo, sizeof motivo, "out of memory");
    else {
      okC = ass_bench_passe(0, 0, &pd, hc, &rc, motivo, sizeof motivo);
      if (okC) ass_bench_logar("C", &rc);
      if (okC) okN = ass_bench_passe(1, 0, &pd, hn, &rn, motivo, sizeof motivo);
      if (okN) ass_bench_logar(rn.motor ? "NEON" : "C (NEON not available)", &rn);
    }
    if (okC && okN) {
      int i, iguais = 0, primeira = -1;
      for (i = 0; i < ASS_BENCH_QUADROS; i++) {
        if (hc[i] == hn[i]) iguais++;
        else if (primeira < 0) primeira = i;
      }
      printf("[assperf] bench C vs NEON: %s %d/%d frames identical | NEON %.2fx faster (C %.0f ms, NEON %.0f ms, %.1f vs %.1f ms/frame)\n",
             iguais == ASS_BENCH_QUADROS ? "OK" : "DIFFERENT", iguais, ASS_BENCH_QUADROS,
             rn.totalMs > 0 ? rc.totalMs / rn.totalMs : 0.0,
             rc.totalMs, rn.totalMs, rc.totalMs / ASS_BENCH_QUADROS, rn.totalMs / ASS_BENCH_QUADROS);
      if (primeira >= 0)
        printf("[assperf] bench: first different frame %d at %.3f s\n", primeira,
               (ini + ((long long)primeira * 1001 + 12) / 24) / 1000.0);
      snprintf(resumo, sizeof resumo, "Bench: C %.1f \xc2\xb7 NEON %.1f ms/frame (%.2fx) \xc2\xb7 %s",
               rc.totalMs / ASS_BENCH_QUADROS, rn.totalMs / ASS_BENCH_QUADROS,
               rn.totalMs > 0 ? rc.totalMs / rn.totalMs : 0.0,
               iguais == ASS_BENCH_QUADROS ? "identical" : "DIFFERENT pictures");
    }
  } else {
#ifdef ASS_NV_API_PARALELO
    ASS_NvParStats ps;
    ass_nv_par_stats_take(&ps);
#endif
    okN = ass_bench_passe(simd, 0, &pd, NULL, &rn, motivo, sizeof motivo);
    if (okN) {
      ass_bench_logar(rn.motor ? "NEON" : "C", &rn);
      snprintf(resumo, sizeof resumo, "Bench %s: avg %.1f ms/frame, max %.0f ms, %u frames with subtitles",
               rn.motor ? "NEON" : "C", rn.totalMs / ASS_BENCH_QUADROS, rn.maxMs, rn.comLegenda);
#ifdef ASS_NV_API_PARALELO
      ass_nv_par_stats_take(&ps);
      printf("[assperf] bench blur split (%d blur threads, minimum %u lanes): jobs %u split %u (%d%%)"
             " | one thread: below minimum %u, pool busy %u, 1 thread %u | lanes split %d%%\n",
             ass_nv_get_threads(), (unsigned)ass_nv_get_par_min(), ps.jobs, ps.split,
             ps.jobs ? (int)(100.0 * ps.split / ps.jobs + 0.5) : 0, ps.small, ps.busy, ps.single,
             ps.lanes ? (int)(100.0 * (double)ps.lanes_split / (double)ps.lanes + 0.5) : 0);
#endif
#if ASS_GPU
      /* A mesma janela com o blur adiado: o que sobra para a CPU. O trabalho
       * da GPU roda no fio GL ao tocar (linha "[assperf] gpu blur"). */
      { AssBenchRes rg;
        fflush(stdout);
        if (ass_bench_passe(simd, 1, &pd, NULL, &rg, motivo, sizeof motivo)) {
          ass_bench_logar("blur deferred to the GPU (libass part only)", &rg);
          printf("[assperf] bench blur on GPU: libass %.1f ms/frame avg, max %.1f (CPU blur: %.1f avg, max %.1f)"
                 " | composites with blur %u: to GPU %u, kept on CPU for \\be %u, vector clip %u"
                 " | GPU time: see [assperf] gpu blur while playing\n",
                 rg.totalMs / ASS_BENCH_QUADROS, rg.maxMs, rn.totalMs / ASS_BENCH_QUADROS, rn.maxMs,
                 rg.comBlur, rg.adiados, rg.keptBe, rg.keptClip);
          snprintf(resumo, sizeof resumo, "Bench: CPU blur %.1f ms/frame (max %.0f) \xc2\xb7 blur on GPU: libass %.1f (max %.0f)",
                   rn.totalMs / ASS_BENCH_QUADROS, rn.maxMs, rg.totalMs / ASS_BENCH_QUADROS, rg.maxMs);
        }
      }
#endif
    }
  }
  if (motivo[0]) {
    printf("[assperf] bench: stopped - %s\n", motivo);
    snprintf(resumo, sizeof resumo, "Bench stopped: %s", motivo);
  }
  fflush(stdout);
  free(hc); free(hn);
  pthread_mutex_lock(&assTrava);
  assBenchAtivo = 0;
  ass_nv_perf_enable(assperf_get(AP_ESTAGIOS));
  pthread_mutex_unlock(&assTrava);
  assperf_bench_estado(0, resumo);
  return NULL;
}

/* Fio GL (assrender_desenhar): a chave virou "Run" -> volta a Off e o bench
 * sai num fio proprio, com o instante atual como fim da janela. */
static void ass_bench_talvez(long long agora, unsigned geracao) {
  int modo = assperf_get(AP_BENCH);
  pthread_t fio;
  AssBenchPed *pd;
  if (!modo) return;
  assperf_definir(AP_BENCH, 0);
  if (assperf_bench_rodando()) { printf("[assperf] bench: already running\n"); fflush(stdout); return; }
  pd = malloc(sizeof *pd);
  if (!pd) return;
  pd->modo = modo; pd->geracao = geracao;
  pd->fim = agora < 0 ? 0 : agora / 1000 * 1000;
  assperf_bench_estado(1, "Bench running\xe2\x80\xa6 (pause the video for clean numbers)");
  if (pthread_create(&fio, NULL, ass_bench_fio, pd) != 0) {
    free(pd);
    assperf_bench_estado(0, "Bench: could not start a thread");
    return;
  }
  pthread_detach(fio);
}
#endif

#if defined(NV_TPK) && defined(ASS_NV_API)
/* ===== assperf: ASS SNAPSHOT (TPK) ==========================================
 * Na folha, "ASS snapshot -> Take now" com o video pausado no quadro errado:
 *  1. texto no log ([assperf] snap ...): o cabecalho do script (PlayRes,
 *     YCbCr Matrix, ScaledBorderAndShadow), o que a folha Estilo do player
 *     pede, as linhas ativas naquele instante (texto inteiro) com o estilo e as
 *     cores de cada uma, e as imagens que o libass entregou (cor, alfa,
 *     tamanho, cobertura);
 *  2. figuras na pagina do log (metade da resolucao, BMP):
 *     /ass-snap-libass.bmp       o quadro do libass sobre cinza 50%
 *     /ass-snap-libass-alpha.bmp a cobertura dele (branco = opaco)
 *     /ass-snap-screen.bmp       o que o app de fato pintou (glReadPixels,
 *                                logo depois das legendas, antes do resto)
 *     /ass-snap-screen-alpha.bmp o alfa da tela (0 = video aparece)
 * As duas primeiras dizem se o libass pinta certo; as duas ultimas, se o
 * desenho no fio GL muda alguma coisa. Nada vai para a rede. */
#define SNAP_W 960
#define SNAP_H 540

static unsigned char *ass_bmp24(const unsigned char *rgb, int w, int h, size_t *n) {
  size_t linha = ((size_t)w * 3 + 3) & ~(size_t)3, tam = 54 + linha * (size_t)h;
  unsigned char *b = calloc(1, tam);
  int y, x;
  if (!b) return NULL;
  b[0] = 'B'; b[1] = 'M';
#define LE32(o, v) do { b[o] = (unsigned char)(v); b[o + 1] = (unsigned char)((v) >> 8); \
                        b[o + 2] = (unsigned char)((v) >> 16); b[o + 3] = (unsigned char)((v) >> 24); } while (0)
  LE32(2, (unsigned)tam); LE32(10, 54u); LE32(14, 40u); LE32(18, (unsigned)w); LE32(22, (unsigned)h);
  b[26] = 1; b[28] = 24; LE32(34, (unsigned)(linha * (size_t)h));
#undef LE32
  for (y = 0; y < h; y++) {
    unsigned char *d = b + 54 + (size_t)(h - 1 - y) * linha;
    const unsigned char *s = rgb + (size_t)y * (size_t)w * 3;
    for (x = 0; x < w; x++) { d[3 * x] = s[3 * x + 2]; d[3 * x + 1] = s[3 * x + 1]; d[3 * x + 2] = s[3 * x]; }
  }
  *n = tam;
  return b;
}

static void ass_snap_guardar_rgb(const char *nome, const unsigned char *rgb, int w, int h) {
  size_t n = 0;
  unsigned char *b = ass_bmp24(rgb, w, h, &n);
  if (b) assperf_snap_guardar(nome, b, n);
}

/* Texto do snapshot: um buffer que cresce, uma linha "[assperf] snap" cada. */
typedef struct { char *b; size_t n, cap; } SnapTxt;
static void snap_linha(SnapTxt *t, const char *fmt, ...) {
  char l[1400];
  va_list va;
  int k;
  va_start(va, fmt);
  k = vsnprintf(l, sizeof l, fmt, va);
  va_end(va);
  if (k < 0) return;
  if ((size_t)k >= sizeof l) k = (int)sizeof l - 1;
  printf("[assperf] snap %s\n", l);
  if (t->n + (size_t)k + 2 > t->cap) {
    size_t nc = (t->cap ? t->cap * 2 : 16384) + (size_t)k;
    char *nb = realloc(t->b, nc);
    if (!nb) return;
    t->b = nb; t->cap = nc;
  }
  memcpy(t->b + t->n, l, (size_t)k); t->n += (size_t)k; t->b[t->n++] = '\n';
}

static const char *snap_matriz(int m) {
  static const char *nomes[] = { "missing", "unknown", "None", "TV.601", "PC.601", "TV.709", "PC.709",
                                 "TV.240M", "PC.240M", "TV.FCC", "PC.FCC" };
  return m >= 0 && m < (int)(sizeof nomes / sizeof nomes[0]) ? nomes[m] : "?";
}

typedef struct { long long t; unsigned geracao; } AssSnapPed;
static int assSnapTela;   // fio GL: ler a tela no proximo quadro desenhado

static void *ass_snap_fio(void *u) {
  AssSnapPed pd = *(AssSnapPed *)u;
  SnapTxt tx = { NULL, 0, 0 };
  ASS_Renderer *r = NULL;
  ASS_Image *img, *it;
  unsigned char *tela = NULL, *rgb = NULL, *alf = NULL;
  int fw = 0, fh = 0, i, nimg = 0, ativos = 0;
  free(u);
  pthread_mutex_lock(&assTrava);
  if (!assLib || !assRenderer || !assTrack || pd.geracao != __atomic_load_n(&assGeracao, __ATOMIC_ACQUIRE) ||
      assTrackGeracao != pd.geracao || layAplW <= 0) {
    pthread_mutex_unlock(&assTrava);
    printf("[assperf] snap: no ASS track drawing\n"); fflush(stdout);
    assperf_bench_estado(0, "Snapshot: no ASS track drawing");
    return NULL;
  }
  snap_linha(&tx, "at %.3f s | frame %dx%d storage %dx%d font scale %.3f | PlayRes %dx%d | LayoutRes %dx%d | "
             "YCbCr Matrix %s | ScaledBorderAndShadow %d | WrapStyle %d | Kerning %d | %d events, %d styles in libass",
             pd.t / 1000.0, layAplW, layAplH, layAplVW, layAplVH, layAplEscala,
             assTrack->PlayResX, assTrack->PlayResY, assTrack->LayoutResX, assTrack->LayoutResY,
             snap_matriz((int)assTrack->YCbCrMatrix), assTrack->ScaledBorderAndShadow, assTrack->WrapStyle,
             assTrack->Kerning, assTrack->n_events, assTrack->n_styles);
  snap_linha(&tx, "player style: your-style option=%s | plain text track=%d | Style sheet overrides bits=0x%x "
             "(1 colour 2 font 4 outline colour 8 border 16 box 32 position 64 bold) applied=%d colour=%06x outline=%06x "
             "border=%d box=%d bold=%d font=%s",
             assperf_get(AP_ESTILO) ? "Ignored" : "Applies", assTextoSimples, plainPedValido ? plainPed.overrides : 0,
             plainAplValido ? plainApl.overrides : 0, plainPed.rgb, plainPed.outlineRgb, plainPed.border,
             plainPed.background, plainPed.bold, plainPed.font);
  { int usados[512] = { 0 }, nu = 0;
    for (i = 0; i < assTrack->n_events; i++) {
      const ASS_Event *e = &assTrack->events[i];
      if (e->Start > pd.t || e->Start + e->Duration <= pd.t) continue;
      ativos++;
      if (ativos <= 150)
        snap_linha(&tx, "event %d: layer %d %.3f-%.3f style=%s effect=%s | %.1100s", i, e->Layer,
                   e->Start / 1000.0, (e->Start + e->Duration) / 1000.0,
                   e->Style >= 0 && e->Style < assTrack->n_styles && assTrack->styles[e->Style].Name ? assTrack->styles[e->Style].Name : "?",
                   e->Effect ? e->Effect : "", e->Text ? e->Text : "");
      if (e->Style >= 0 && e->Style < assTrack->n_styles && nu < 512) {
        int k, novo = 1;
        for (k = 0; k < nu; k++) if (usados[k] == e->Style) novo = 0;
        if (novo) usados[nu++] = e->Style;
      }
    }
    for (i = 0; i < nu; i++) {
      const ASS_Style *s = &assTrack->styles[usados[i]];
      snap_linha(&tx, "style %s: font=%s size=%.1f primary=%08X secondary=%08X outline=%08X back=%08X "
                 "bold=%d borderstyle=%d outline=%.2f shadow=%.2f scale=%.0f/%.0f align=%d margins=%d/%d/%d blur=%.2f",
                 s->Name ? s->Name : "?", s->FontName ? s->FontName : "?", s->FontSize,
                 s->PrimaryColour, s->SecondaryColour, s->OutlineColour, s->BackColour,
                 s->Bold, s->BorderStyle, s->Outline, s->Shadow, s->ScaleX * 100, s->ScaleY * 100,
                 s->Alignment, s->MarginL, s->MarginR, s->MarginV, s->Blur);
    } }
  snap_linha(&tx, "%d event(s) active (the first 150 listed)", ativos);

  r = ass_renderer_igual_locked();
  fw = layAplW; fh = layAplH;
  img = r ? ass_render_frame(r, assTrack, pd.t, NULL) : NULL;
  pthread_mutex_unlock(&assTrava);

  /* As imagens sao do renderer do snapshot: validas ate o ass_renderer_done. */
  tela = calloc((size_t)fw * (size_t)fh, 4);
  if (tela) {
    for (i = 0; i < fw * fh; i++) { tela[4 * i] = tela[4 * i + 1] = tela[4 * i + 2] = 128; tela[4 * i + 3] = 0; }
    for (it = img; it; it = it->next) {
      unsigned cr = it->color >> 24, cg = (it->color >> 16) & 255, cb = (it->color >> 8) & 255;
      unsigned op = 255u - (it->color & 255u);
      long cheio = 0, cobre = 0;
      int x, y, maxA = 0;
      for (y = 0; y < it->h; y++) {
        int py = it->dst_y + y;
        const unsigned char *src = it->bitmap + (size_t)y * (size_t)it->stride;
        if (py < 0 || py >= fh) continue;
        for (x = 0; x < it->w; x++) {
          int px = it->dst_x + x;
          unsigned a = (unsigned)src[x] * op / 255u;
          if (src[x] > maxA) maxA = src[x];
          if (src[x]) cobre++;
          if (src[x] >= 250) cheio++;
          if (px < 0 || px >= fw || !a) continue;
          { unsigned char *d = tela + 4 * ((size_t)py * (size_t)fw + (size_t)px);
            d[0] = (unsigned char)((cr * a + d[0] * (255u - a) + 127u) / 255u);
            d[1] = (unsigned char)((cg * a + d[1] * (255u - a) + 127u) / 255u);
            d[2] = (unsigned char)((cb * a + d[2] * (255u - a) + 127u) / 255u);
            d[3] = (unsigned char)(a + d[3] * (255u - a) / 255u); }
        }
      }
      nimg++;
      if (nimg <= 80 || (long)it->w * it->h > 200000)
        snap_linha(&tx, "image %d: %s colour=%02X%02X%02X alpha=%u (ASS &H%02X) at %d,%d %dx%d max=%d covered=%ld full(>=250)=%ld",
                   nimg, it->type == IMAGE_TYPE_CHARACTER ? "fill" : it->type == IMAGE_TYPE_OUTLINE ? "outline" : "shadow",
                   cr, cg, cb, op, it->color & 255u, it->dst_x, it->dst_y, it->w, it->h, maxA, cobre, cheio);
    }
    rgb = malloc((size_t)SNAP_W * SNAP_H * 3);
    alf = malloc((size_t)SNAP_W * SNAP_H * 3);
    if (rgb && alf) {
      int x, y;
      for (y = 0; y < SNAP_H; y++)
        for (x = 0; x < SNAP_W; x++) {
          const unsigned char *s = tela + 4 * ((size_t)(y * fh / SNAP_H) * (size_t)fw + (size_t)(x * fw / SNAP_W));
          unsigned char *d = rgb + 3 * ((size_t)y * SNAP_W + x), *da = alf + 3 * ((size_t)y * SNAP_W + x);
          d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
          da[0] = da[1] = da[2] = s[3];
        }
      ass_snap_guardar_rgb("ass-snap-libass.bmp", rgb, SNAP_W, SNAP_H);
      ass_snap_guardar_rgb("ass-snap-libass-alpha.bmp", alf, SNAP_W, SNAP_H);
    }
  }
  snap_linha(&tx, "%d image(s) from libass (%dx%d frame). Pictures: http://<TV IP>:8099/ass-snap-libass.bmp, "
             "/ass-snap-libass-alpha.bmp, /ass-snap-screen.bmp, /ass-snap-screen-alpha.bmp", nimg, fw, fh);
  free(rgb); free(alf); free(tela);
  if (r) { pthread_mutex_lock(&assTrava); ass_renderer_done(r); pthread_mutex_unlock(&assTrava); }
  if (tx.b) assperf_snap_guardar("ass-snap.txt", (unsigned char *)tx.b, tx.n);
  fflush(stdout);
  __atomic_store_n(&assSnapTela, 1, __ATOMIC_RELEASE);
  assperf_bench_estado(0, "Snapshot taken: see the Subtitles log and /ass-snap-*.bmp");
  return NULL;
}

/* Fio GL: a chave virou "Take now" -> volta a Off e o snapshot sai num fio. */
static void ass_snap_talvez(long long agora, unsigned geracao) {
  pthread_t fio;
  AssSnapPed *pd;
  if (!assperf_get(AP_SNAP)) return;
  assperf_definir(AP_SNAP, 0);
  pd = malloc(sizeof *pd);
  if (!pd) return;
  pd->t = agora < 0 ? 0 : agora; pd->geracao = geracao;
  if (pthread_create(&fio, NULL, ass_snap_fio, pd) != 0) { free(pd); return; }
  pthread_detach(fio);
}

#if ASS_GPU
/* Snapshot, fio GL: cada imagem adiada do quadro em tela, a da GPU contra a
 * receita na CPU (o desenho exato do libass). Diferenca em niveis de alfa
 * (0..255) e a figura da maior: CPU | GPU | diferenca x8. */
static void ass_snap_gpublur(void) {
  int i, n = 0, maior = -1, maxD = 0;
  double soma = 0, px = 0, acima2 = 0, acima8 = 0;
  long area = 0;
  for (i = 0; i < assMain.count; i++) {
    const AssCpuImage *im = &assMain.images[i];
    unsigned char *a, *b;
    ASS_NvDefer d;
    int k, dImg = 0;
    if (!im->gpu || !assMainTex || !assMainTex[i] || !assMainGk || !assMainGk[i]) continue;
    a = malloc((size_t)im->w * (size_t)im->h);
    b = malloc((size_t)im->w * (size_t)im->h);
    ass_gpu_receita(&assMain, im, &d);
    if (!a || !b || !ass_nv_defer_apply(&d, im->w, im->h, a, im->w) ||
        !assgpublur_ler(assMainTex[i], im->w, im->h, b)) { free(a); free(b); continue; }
    for (k = 0; k < im->w * im->h; k++) {
      int df = a[k] > b[k] ? a[k] - b[k] : b[k] - a[k];
      soma += df; px++;
      if (df > 2) acima2++;
      if (df > 8) acima8++;
      if (df > dImg) dImg = df;
    }
    if (dImg > maxD) maxD = dImg;
    n++;
    if ((long)im->w * im->h > area) {
      /* a figura da maior imagem: tres paineis lado a lado, cinza */
      int esc = (im->w * 3 + 1919) / 1920, W, H, x, y;
      unsigned char *rgb;
      if (esc < 1) esc = 1;
      W = im->w * 3 / esc; H = im->h / esc;
      rgb = W > 0 && H > 0 ? malloc((size_t)W * (size_t)H * 3) : NULL;
      if (rgb) {
        for (y = 0; y < H; y++)
          for (x = 0; x < W; x++) {
            int painel = x / (im->w / esc > 0 ? im->w / esc : 1), sx = (x % (im->w / esc > 0 ? im->w / esc : 1)) * esc;
            size_t o = (size_t)(y * esc) * (size_t)im->w + (size_t)sx;
            int v = painel == 0 ? a[o] : painel == 1 ? b[o] : (a[o] > b[o] ? a[o] - b[o] : b[o] - a[o]) * 8;
            unsigned char *q = rgb + 3 * ((size_t)y * (size_t)W + (size_t)x);
            if (v > 255) v = 255;
            q[0] = q[1] = q[2] = (unsigned char)v;
            if (painel == 2 && v) { q[0] = (unsigned char)v; q[1] = q[2] = 0; }
          }
        ass_snap_guardar_rgb("ass-snap-gpublur.bmp", rgb, W, H);
        free(rgb);
        area = (long)im->w * im->h; maior = i;
      }
    }
    free(a); free(b);
  }
  if (n)
    printf("[assperf] snap gpu blur vs libass CPU: %d image(s), %.0f pixels | max difference %d | mean %.2f"
           " | pixels off by more than 2: %.2f%%, more than 8: %.3f%% (alpha levels 0-255, %s) | picture of image %d:"
           " http://<TV IP>:8099/ass-snap-gpublur.bmp (CPU | GPU | difference x8)\n",
           n, px, maxD, px > 0 ? soma / px : 0.0, px > 0 ? 100.0 * acima2 / px : 0.0, px > 0 ? 100.0 * acima8 / px : 0.0,
           assgpublur_formato(), maior + 1);
  else
    printf("[assperf] snap gpu blur: no image on screen came from the GPU blur (Blur on GPU %s)\n",
           assperf_get(AP_GPUBLUR) ? "on" : "off");
  fflush(stdout);
}
#endif

/* Fio GL, logo depois das legendas: le o que foi pintado (snapshot). */
static void ass_snap_ler_tela(void) {
  GLint vp[4];
  int w, h, y0, x, y;
  unsigned char *faixa, *rgb, *alf;
  double soma[4] = { 0 };
  if (!__atomic_exchange_n(&assSnapTela, 0, __ATOMIC_ACQ_REL)) return;
  glGetIntegerv(GL_VIEWPORT, vp);
  w = vp[2]; h = vp[3];
  if (w <= 0 || h <= 0) return;
  faixa = malloc((size_t)w * 4 * 16);
  rgb = malloc((size_t)SNAP_W * SNAP_H * 3);
  alf = malloc((size_t)SNAP_W * SNAP_H * 3);
  if (!faixa || !rgb || !alf) { free(faixa); free(rgb); free(alf); return; }
  for (y = 0; y < SNAP_H; y++) {
    /* a linha y da figura (de cima) e a linha h-1-sy do GL (de baixo) */
    int sy = y * h / SNAP_H;
    y0 = h - 1 - sy;
    glReadPixels(vp[0], vp[1] + y0, w, 1, GL_RGBA, GL_UNSIGNED_BYTE, faixa);
    for (x = 0; x < SNAP_W; x++) {
      const unsigned char *s = faixa + 4 * (size_t)(x * w / SNAP_W);
      unsigned char *d = rgb + 3 * ((size_t)y * SNAP_W + x), *da = alf + 3 * ((size_t)y * SNAP_W + x);
      d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; da[0] = da[1] = da[2] = s[3];
      soma[0] += s[0]; soma[1] += s[1]; soma[2] += s[2]; soma[3] += s[3];
    }
  }
  ass_snap_guardar_rgb("ass-snap-screen.bmp", rgb, SNAP_W, SNAP_H);
  ass_snap_guardar_rgb("ass-snap-screen-alpha.bmp", alf, SNAP_W, SNAP_H);
#if ASS_GPU
  ass_snap_gpublur();
#endif
  printf("[assperf] snap screen: %dx%d framebuffer read after the subtitles | average R %.0f G %.0f B %.0f alpha %.0f"
         " | gl error 0x%x\n", w, h, soma[0] / (SNAP_W * SNAP_H), soma[1] / (SNAP_W * SNAP_H),
         soma[2] / (SNAP_W * SNAP_H), soma[3] / (SNAP_W * SNAP_H), (unsigned)glGetError());
  fflush(stdout);
  free(faixa); free(rgb); free(alf);
}

/* Cartao de teste de cor: branco 100/90/75/50 %, branco a 50 % de alfa e
 * preto, 120x120 cada, no alto da tela, pelo mesmo blending das legendas. */
static void ass_cartao_desenhar(void) {
  static const float cor[6][4] = {
    { 1, 1, 1, 1 }, { .9f, .9f, .9f, 1 }, { .75f, .75f, .75f, 1 }, { .5f, .5f, .5f, 1 }, { 1, 1, 1, .5f }, { 0, 0, 0, 1 }
  };
  int i;
  if (!assperf_get(AP_CARTAO)) return;
  gfx_tex_aspect_atual = 0.0f;
  for (i = 0; i < 6; i++)
    gfx_rect((GfxRect){ 600.0f + i * 130.0f, 40.0f, 120.0f, 120.0f }, 0, GFX_COR, 0, 0, 0, 0.0f,
             cor[i][0], cor[i][1], cor[i][2], cor[i][3]);
}
#endif

/* assperf (render ahead): estado do fio GL. */
static double assAdianteEma;          // ms; "Auto"
static double assUploadEma;           // ms do upload de um quadro novo
static long long assUltimoAgora = LLONG_MIN;
static int assParadoN;                // chamadas seguidas com o mesmo `agora`

int assrender_desenhar(double posSeg, int atrasoMs, float alpha,
                       float x, float y, float w, float h) {
  unsigned generation;
  int i, n = 0, reset, corChave, reenviar, novoCaminho, segurar, saltou = 0, quadroNovo = 0;
  int corAtiva, cr, cg, cb, foraTrava, adiante = 0, fixo;
  long long agora, pedido;
  AssCpuFrame novo;
  struct timespec td0, td1;
  (void)w; (void)h;
  if (alpha <= 0.001f) return 0;
  clock_gettime(CLOCK_MONOTONIC, &td0);
  memset(&novo, 0, sizeof novo);
  generation = __atomic_load_n(&assGeracao, __ATOMIC_ACQUIRE);
  agora = (long long)llround(posSeg * 1000.0) + (long long)atrasoMs;
#if defined(NV_TPK) && defined(ASS_NV_API)
  ass_globais_aplicar();   // fios do blur: na hora, mesmo pausado
  ass_bench_talvez(agora, generation);
  ass_snap_talvez(agora, generation);
#endif
  /* assperf: caminho de desenho novo (qualquer chave de upload ligada) e
   * render ahead. Tudo em "old": o mesmo de sempre, linha por linha. */
  novoCaminho = assperf_get(AP_UPLOAD) || assperf_get(AP_TEXCACHE) || assperf_get(AP_FORATRAVA);
  foraTrava = assperf_get(AP_FORATRAVA);
  fixo = assperf_adiante_fixo_ms();
  /* Parado (pausa, buffering, relogio sem amostra nova): o mesmo instante
   * 3 vezes seguidas. Ai nao se adianta nada: a pausa mostra o quadro exato. */
  if (agora == assUltimoAgora) { if (assParadoN < 1000) assParadoN++; } else assParadoN = 0;
  assUltimoAgora = agora;
  if (fixo != 0 && assParadoN < 3) {
    if (fixo > 0) adiante = fixo;
    else {
      double renderMs;
      pthread_mutex_lock(&assFilaTrava); renderMs = assUltimoRenderPuroMs; pthread_mutex_unlock(&assFilaTrava);
      { double alvo = renderMs + assUploadEma + 16.7;
        assAdianteEma = assAdianteEma <= 0.0 ? alvo : assAdianteEma * 0.92 + alvo * 0.08;
        if (assAdianteEma > 300.0) assAdianteEma = 300.0; }
      adiante = (int)lround(assAdianteEma);
    }
    assperf_med_adiante((double)adiante);
  }
  segurar = adiante > 0 && assperf_get(AP_SEGURAR);
  pedido = agora + adiante;
  if (novoCaminho != assCaminhoNovoAntes) {
    /* Trocou de caminho com a faixa ligada: nada de um serve ao outro. */
    if (assCaminhoNovoAntes >= 0) { ass_main_soltar(); ass_tc_apagar(); }
    assCaminhoNovoAntes = novoCaminho;
    pthread_mutex_lock(&assFilaTrava);
    ass_frame_liberar(&assAtual); assAtualValido = 0;
    assTexResetar = 1; assTemUltimoPedido = 0;
    pthread_mutex_unlock(&assFilaTrava);
  }
  pthread_mutex_lock(&assFilaTrava);
  if (!assWorkerCriado) { pthread_mutex_unlock(&assFilaTrava); return 0; }
  assFilaTeto = segurar ? ASS_FILA_MAX : 1;
  /* SALTO de verdade (seek): o quadro velho mostra outra cena e sai. ERA 200
   * ms — e o currentTime da C9 anda 197..247 ms por evento, 77 % acima de 200
   * (log de 23/09): a legenda era apagada a cada evento e piscava 2-4 vezes
   * por segundo. Com o relogio interpolado o passo e de um quadro; 1,5 s so
   * pega salto. Fora disso o quadro atual fica ate o proximo estar pronto. */
  /* Com "Render only on changes" o ultimo PEDIDO pode ser de segundos atras
   * (a fala parada nao pede nada): o salto e medido contra o ultimo instante
   * VISTO, senao uma fala longa virava "seek" e piscava. */
  if (assTemUltimoPedido && assUltimoPedidoGeracao == generation &&
      fabs((double)pedido - (assperf_get(AP_MUDANCA) && assUltimoVistoGeracao == generation
                             ? assUltimoVistoMs : assUltimoPedidoMs)) > 1500.0) {
    ass_fila_limpar_locked();
    ass_frame_liberar(&assAtual); assAtualValido = 0;
    ++assEpoch;
    ++assSerial;
    saltou = 1;
  }
  /* Com "Hold until due" a fila cheia (ou grande demais) para os pedidos ate
   * um quadro sair: sem isso o worker renderizaria sempre mais adiante e o
   * quadro "devido" nunca chegaria. */
  /* assperf "Render only on changes": o ultimo render ainda vale para este
   * instante (nada comeca, acaba ou anima ate la) — o worker nao e chamado. */
  { int vale = assperf_get(AP_MUDANCA) && assTemUltimoPedido && assUltimoPedidoGeracao == generation &&
               assValidoEpoch == assEpoch && assValidoGeracao == generation &&
               pedido >= assValidoDe && pedido < assValidoAte;
  if (!vale && (!assTemUltimoPedido || assUltimoPedidoGeracao != generation ||
       fabs((double)pedido - assUltimoPedidoMs) >= 5.0) &&
      (!segurar || (assFilaN < ASS_FILA_MAX && ass_fila_bytes_locked() < ASS_FILA_BYTES))) {
    assUltimoPedidoMs = (double)pedido;
    assUltimoPedidoGeracao = generation;
    assTemUltimoPedido = 1;
    assPedidoMs = (double)pedido;
    assPedidoGeracao = generation;
    assPedidoSerial = ++assSerial;
    assPedidoPendente = 1;
    pthread_cond_signal(&assFilaCond);
  } }

  if (assAtualValido && assAtual.generation != generation) {
    ass_frame_liberar(&assAtual); assAtualValido = 0;
  }
  /* O quadro da fila que entra: o mais novo ja devido (os anteriores a ele
   * saem). Sem hold, qualquer um esta devido — o de sempre. */
  { int devido = -1;
    for (i = 0; i < assFilaN; i++) {
      if (assFila[i].generation != generation) { ass_fila_tirar_locked(i, NULL); i--; continue; }
      if (!segurar || assFila[i].ms <= agora + 8) devido = i;
    }
    if (devido >= 0) {
      for (i = 0; i < devido; i++) ass_fila_tirar_locked(0, NULL);
      ass_fila_tirar_locked(0, &novo);
      quadroNovo = 1;
      assMed.trocas++;
    } }
  reset = assTexResetar; assTexResetar = 0;
  corAtiva = assCorAtiva; cr = assCorR; cg = assCorG; cb = assCorB;
  assUltimoVistoMs = (double)pedido; assUltimoVistoGeracao = generation;

  if (!novoCaminho) {
    /* ---- o caminho de sempre ---- */
    if (quadroNovo) {
      ass_frame_liberar(&assAtual);
      assAtual = novo; memset(&novo, 0, sizeof novo);
      assAtualValido = 1;
      if (assParadoN < 3) assperf_med_atraso((double)(agora - assAtual.ms));
    }
    if (reset) ass_apagar_texturas_locked();
    if (!assAtualValido) {
      ass_medir_locked(agora, 0, -1);
      pthread_mutex_unlock(&assFilaTrava);
      assperf_med_tick();
      return 0;
    }
    corChave = assCorAtiva ? (int)(((unsigned)assCorR << 16) | ((unsigned)assCorG << 8) | (unsigned)assCorB) : -1;
    reenviar = assAtual.serial != assTexSerial || corChave != assTexCorChave;
    { struct timespec tu0, tu1;
      clock_gettime(CLOCK_MONOTONIC, &tu0);
      for (i = 0; i < assAtual.count; i++) {
        const AssCpuImage *im = &assAtual.images[i];
        GLuint tex = reenviar ? ass_textura_locked(i, im)
                              : (i < assTexCap ? assTex[i].tex : 0);
        if (!tex) continue;
        /* libass trabalha no mesmo sistema de coordenadas do arquivo: o ponto
         * (0,0) e o canto superior esquerdo. A textura contem somente a caixa do
         * glyph e pode ser composta diretamente pelo shader de texto. */
        gfx_tex_aspect_atual = 0.0f;
        { float kk = assAtual.k > 1 ? (float)assAtual.k : 1.0f;
          float esc = assAtual.esc > 0.01f ? assAtual.esc : kk;
          /* Origem arredondada ao pixel do alvo: 1:1, sem meio pixel de filtro. */
          float dx = roundf((x + assAtual.ox) * kk) / kk + (float)im->x / esc;
          float dy = roundf((y + assAtual.oy) * kk) / kk + (float)im->y / esc;
          gfx_rect((GfxRect){ dx, dy, (float)im->w / esc, (float)im->h / esc }, tex, GFX_TEXTO,
                   0, 0, 0, 0, 1, 1, 1, alpha); }
      }
      if (reenviar) {
        size_t bytes = 0;
        for (i = 0; i < assAtual.count; i++) bytes += (size_t)assAtual.images[i].w * (size_t)assAtual.images[i].h * 4u;
        clock_gettime(CLOCK_MONOTONIC, &tu1);
        { double up = ass_ms_entre(&tu0, &tu1);
          assUploadEma = assUploadEma * 0.9 + up * 0.1;
          assperf_med_upload(up, bytes, assAtual.count, 0, quadroNovo); }
      } }
    if (reenviar) { assTexSerial = assAtual.serial; assTexCorChave = corChave; }
    gfx_tex_aspect_atual = 0.0f;
    n = assAtual.count;
    ass_medir_locked(agora, n > 0, assAtual.inicioMax);
    pthread_mutex_unlock(&assFilaTrava);
  } else {
    /* ---- o caminho novo (assperf) ---- */
    long long inicioMax;
    int temTexto;
    if (!foraTrava) {
      /* "Upload outside lock" desligado: a trava fica do pedido ate o fim do
       * desenho, como no caminho antigo. */
    } else pthread_mutex_unlock(&assFilaTrava);
    if (reset || saltou || assMainResetar) { ass_main_soltar(); ass_tc_apagar(); assMainResetar = 0; }
    if (assMainValido && assMain.generation != generation) ass_main_soltar();
    if (quadroNovo) {
      ass_frame_liberar(&assMain);
      assMain = novo; memset(&novo, 0, sizeof novo);
      assMainValido = 1;
      if (assParadoN < 3) assperf_med_atraso((double)(agora - assMain.ms));
    }
    if (assMainValido) {
      struct timespec tu0, tu1;
      clock_gettime(CLOCK_MONOTONIC, &tu0);
      n = ass_main_desenhar(x, y, alpha, corAtiva, cr, cg, cb, quadroNovo);
      clock_gettime(CLOCK_MONOTONIC, &tu1);
      if (quadroNovo) assUploadEma = assUploadEma * 0.9 + ass_ms_entre(&tu0, &tu1) * 0.1;
    }
    temTexto = assMainValido && n > 0;
    inicioMax = assMainValido ? assMain.inicioMax : -1;
    if (foraTrava) pthread_mutex_lock(&assFilaTrava);
#if ASS_GPU
    ass_gpu_adiantar_locked();
#endif
    ass_medir_locked(agora, temTexto, inicioMax);
    pthread_mutex_unlock(&assFilaTrava);
  }
  ass_frame_liberar(&novo);
#if defined(NV_TPK) && defined(ASS_NV_API)
  ass_cartao_desenhar();
  ass_snap_ler_tela();
#endif
  clock_gettime(CLOCK_MONOTONIC, &td1);
  assperf_med_quadro(ass_ms_entre(&td0, &td1));
  assperf_med_tick();
  return n;
}

/* Quadro SINCRONO, sem GL e sem o worker: so para teste e diagnostico
 * (tests/ass_fontes.sh). Passa pelo mesmo ass_render_frame — e ali, no
 * ass_start_frame, que mora a asserção que derrubava a C9. */
int assrender_quadro_cpu(double posSeg) {
  int n = -1, mudou = 0;
  pthread_mutex_lock(&assTrava);
  if (assTrack && assRenderer) {
    ASS_Image *im = ass_render_frame(assRenderer, assTrack, (long long)llround(posSeg * 1000.0), &mudou);
    for (n = 0; im; im = im->next) n++;
  }
  pthread_mutex_unlock(&assTrava);
  return n;
}

int assrender_ativo(void) {
  unsigned geracao = __atomic_load_n(&assGeracao, __ATOMIC_ACQUIRE);
  int ativo = __atomic_load_n(&assTrackAtivo, __ATOMIC_ACQUIRE) &&
              __atomic_load_n(&assTrackGeracao, __ATOMIC_ACQUIRE) == geracao &&
              __atomic_load_n(&assWorkerCriado, __ATOMIC_ACQUIRE);
  return ativo;
}

const char *assrender_diagnostico(void) {
  static char texto[256];
  char base[sizeof assDiag];
  unsigned long long renderUs;
  size_t memoria;
  int eventos, fontes, resolucoes;
  long long coberturaIni, coberturaFim;
  pthread_mutex_lock(&assDiagTrava);
  snprintf(base, sizeof base, "%s", assDiag);
  pthread_mutex_unlock(&assDiagTrava);
  pthread_mutex_lock(&assFilaTrava);
  renderUs = assUltimoRenderUs; memoria = assBytesQuadro;
  pthread_mutex_unlock(&assFilaTrava);
  pthread_mutex_lock(&assTrava);
  eventos = assEventos; fontes = assFontes; resolucoes = assResolucaoFonte;
  coberturaIni = assCoberturaIni; coberturaFim = assCoberturaFim;
  pthread_mutex_unlock(&assTrava);
  snprintf(texto, sizeof texto,
           "%s; eventos=%d fontes=%d fontselect=%d cobertura=%lld-%lldms render=%lluus quadro=%zuB",
           base, eventos, fontes, resolucoes,
           coberturaIni == LLONG_MAX ? 0 : coberturaIni,
           coberturaFim == LLONG_MIN ? 0 : coberturaFim, renderUs, memoria);
  return texto;
}

static void *ass_preaquecer_fio(void *u) {
  (void)u;
  pthread_mutex_lock(&assTrava);
  ass_iniciar_locked(1);
  pthread_mutex_unlock(&assTrava);
  return NULL;
}

void assrender_preaquecer(void) {
  static int pedido;
  pthread_t t;
  if (__atomic_exchange_n(&pedido, 1, __ATOMIC_ACQ_REL)) return;
  if (pthread_create(&t, NULL, ass_preaquecer_fio, NULL) == 0) pthread_detach(t);
  else __atomic_store_n(&pedido, 0, __ATOMIC_RELEASE);
}

void assrender_geracao(unsigned geracao) {
  __atomic_store_n(&assGeracao, geracao, __ATOMIC_RELEASE);
  pthread_mutex_lock(&assFilaTrava);
  assPedidoPendente = 0; ++assEpoch; ++assSerial; assPedidoSerial = assSerial;
  ass_fila_limpar_locked();
  ass_frame_liberar(&assAtual); assAtualValido = 0;
  assTemUltimoPedido = 0; assUltimoRenderUs = 0; assBytesQuadro = 0;
  assTexResetar = 1;
  pthread_mutex_unlock(&assFilaTrava);
}

#else

static char assDiag[96] = "libass: backend nao compilado";
static unsigned assGeracao;

int assrender_carregar_texto(const char *corpo, size_t tamanho, unsigned geracao) { (void)corpo; (void)tamanho; (void)geracao; return 0; }
int assrender_texto_simples(void) { return 0; }
int assrender_fonte_local_transacao(const char *caminho, int (*confirmar)(void *), void *u) {
  (void)caminho; return confirmar ? confirmar(u) : 1;
}
int assrender_fonte_local_ativa(const char *caminho) { (void)caminho; return 0; }
void assrender_definir_texto_estilo(const PlainAssStyle *style) { (void)style; }
int assrender_carregar(const char *corpo, size_t tamanho, unsigned geracao) { (void)corpo; (void)tamanho; (void)geracao; return 0; }
int assrender_atualizar(const char *corpo, size_t tamanho, unsigned geracao) { (void)corpo; (void)tamanho; (void)geracao; return 0; }
void assrender_limpar(void) { assGeracao++; }
void assrender_limpar_fontes(void) {}
void assrender_aliviar(void) {}
int assrender_adicionar_fonte(const char *nome, const void *dados, size_t tamanho) {
  (void)nome; (void)dados; (void)tamanho; return 0;
}
void assrender_definir_cor(int enabled, int r, int g, int b) {
  (void)enabled; (void)r; (void)g; (void)b;
}
void assrender_aplicar_invalidacao(void) {}
int assrender_desenhar(double posSeg, int atrasoMs, float alpha,
                       float x, float y, float w, float h) {
  (void)posSeg; (void)atrasoMs; (void)alpha; (void)x; (void)y; (void)w; (void)h; return 0;
}
void assrender_definir_layout(float x, float y, float w, float h,
                              int videoW, int videoH, double escalaFonte) {
  (void)x; (void)y; (void)w; (void)h; (void)videoW; (void)videoH; (void)escalaFonte;
}
void assrender_pixels_por_unidade(float f) { (void)f; }
int assrender_ativo(void) { return 0; }
int assrender_quadro_cpu(double posSeg) { (void)posSeg; return -1; }
const char *assrender_diagnostico(void) { return assDiag; }
void assrender_geracao(unsigned geracao) { assGeracao = geracao; }
void assrender_preaquecer(void) {}

#endif
