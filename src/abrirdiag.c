// Ver abrirdiag.h para o porque.
#include "abrirdiag.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- ajustes ------------------------------------------------------------------

static volatile int ligFalha = 1, ligOk = 0, ligCdn = 0, lidoAjuste;

#ifndef __EMSCRIPTEN__
#include "dados.h"
static void carregar(void) {
  char *t;
  if (lidoAjuste) return;
  lidoAjuste = 1;
  t = dados_ler("logdiag.txt");
  if (t) {
    if (strstr(t, "falha=0")) ligFalha = 0;
    if (strstr(t, "ok=1")) ligOk = 1;
    if (strstr(t, "cdn2min=1")) ligCdn = 1;
    free(t);
  }
}
static void gravar(void) {
  char b[64];
  snprintf(b, sizeof b, "falha=%d\nok=%d\ncdn2min=%d\n", ligFalha, ligOk, ligCdn);
  dados_gravar("logdiag.txt", b);
  printf("[abrir-diag] settings: header on refusal=%s, header on every open=%s, CDN links expire after 2 min (test)=%s\n",
         ligFalha ? "on" : "off", ligOk ? "on" : "off", ligCdn ? "on" : "off");
  fflush(stdout);
}
void abrirdiag_definir(int falha, int ok) {
  carregar();
  ligFalha = falha ? 1 : 0; ligOk = ok ? 1 : 0;
  gravar();
}
void abrirdiag_definir_cdn_teste(int sim) {
  carregar();
  ligCdn = sim ? 1 : 0;
  gravar();
}
int abrirdiag_falha(void) { carregar(); return ligFalha; }
int abrirdiag_ok(void) { carregar(); return ligOk; }
int abrirdiag_cdn_teste(void) { carregar(); return ligCdn; }
#else
void abrirdiag_definir(int falha, int ok) { (void)falha; (void)ok; }
void abrirdiag_definir_cdn_teste(int sim) { (void)sim; }
int abrirdiag_falha(void) { return 0; }
int abrirdiag_ok(void) { return 0; }
int abrirdiag_cdn_teste(void) { return 0; }
#endif

// --- EBML ---------------------------------------------------------------------

typedef struct {
  // cabecalho EBML
  char docType[16]; int docVer, docReadVer;
  long long segIni;      // onde comecam os filhos do Segment (absoluto)
  long long segTam;      // -1 = tamanho desconhecido
  // ordem dos elementos de nivel 1
  char ordem[600]; size_t ordemN; int nElem;
  // SeekHead
  long long seekCues, seekAnexos, seekTags, seekCaps, seekTracks, seekInfo;
  long long seekSH2; int cuesSH2;   // segundo SeekHead (mkvpropedit) e o Cues achado nele
  // Info
  char muxing[64], writing[64]; double dur; unsigned long long escala;
  // Anexos
  int anexos; long long anexosTam, anexosFontes;
  // 1o Cluster
  long long cluster;
  // faixas
  int faixas, comprimidas, defAudioCodecSet;
  char defAudio[32], primAudio[32];
  char notas[700]; size_t notasN;
  long long total;
  const char *tag;
} Est;

static int largura(unsigned char b) { int w = 1; unsigned char m = 0x80; while (w <= 8 && !(b & m)) { m >>= 1; w++; } return w; }
// ID com o marcador (como nas tabelas do Matroska). 0 = invalido.
static uint32_t lerId(const unsigned char *p, long resta, int *usou) {
  int w; uint32_t v = 0;
  if (resta < 1) return 0;
  w = largura(p[0]);
  if (w > 4 || w > resta) return 0;
  for (int i = 0; i < w; i++) v = (v << 8) | p[i];
  *usou = w; return v;
}
// Tamanho sem o marcador. -1 = desconhecido, -2 = invalido.
static long long lerTam(const unsigned char *p, long resta, int *usou) {
  int w; unsigned long long v; int todos = 1;
  if (resta < 1) return -2;
  w = largura(p[0]);
  if (w > 8 || w > resta) return -2;
  v = p[0] & (0xFF >> w);
  if (v != (0xFFu >> w)) todos = 0;
  for (int i = 1; i < w; i++) { v = (v << 8) | p[i]; if (p[i] != 0xFF) todos = 0; }
  *usou = w;
  return todos ? -1 : (long long)v;
}
static unsigned long long lerUint(const unsigned char *p, long long n) {
  unsigned long long v = 0;
  for (long long i = 0; i < n && i < 8; i++) v = (v << 8) | p[i];
  return v;
}
static double lerFloat(const unsigned char *p, long long n) {
  if (n == 4) { uint32_t u = (uint32_t)lerUint(p, 4); float f; memcpy(&f, &u, 4); return f; }
  if (n == 8) { uint64_t u = lerUint(p, 8); double d; memcpy(&d, &u, 8); return d; }
  return 0;
}
static void lerTexto(const unsigned char *p, long long n, char *d, size_t t) {
  size_t k = 0;
  for (long long i = 0; i < n && k + 1 < t; i++) {
    unsigned char c = p[i];
    if (!c) break;
    d[k++] = (c < 32 || c == 127) ? '?' : (char)c;
  }
  d[k] = 0;
}
static void nota(Est *e, const char *s) {
  if (e->notasN < sizeof e->notas - 2)
    e->notasN += (size_t)snprintf(e->notas + e->notasN, sizeof e->notas - e->notasN, "%s%s", e->notasN ? "; " : "", s);
}
static const char *nomeNivel1(uint32_t id) {
  switch (id) {
    case 0x114D9B74: return "SeekHead";  case 0x1549A966: return "Info";
    case 0x1654AE6B: return "Tracks";    case 0x1043A770: return "Chapters";
    case 0x1941A469: return "Attachments"; case 0x1254C367: return "Tags";
    case 0x1C53BB6B: return "Cues";      case 0x1F43B675: return "Cluster";
    case 0xEC: return "Void";            case 0xBF: return "CRC32";
    default: return NULL;
  }
}
static void mb(char *d, size_t t, long long b) {
  if (b < 0) snprintf(d, t, "?");
  else if (b >= 1024LL * 1024) snprintf(d, t, "%.1f MB", b / 1048576.0);
  else if (b >= 1024) snprintf(d, t, "%.0f KB", b / 1024.0);
  else snprintf(d, t, "%lld B", b);
}

// --- faixas -------------------------------------------------------------------

typedef struct {
  unsigned num, tipo, def, hab, forc, lacing, canais, bits, w, h, bpc, algo, cifra, comp;
  double freq; long long cpN;
  char codec[32], lang[12], nome[48], perfil[48];
} Faixa;

static void perfilAvc(const unsigned char *c, long long n, char *d, size_t t) {
  static const struct { int id; const char *nome; } P[] = {
    {66, "Baseline"}, {77, "Main"}, {88, "Extended"}, {100, "High"}, {110, "High 10"},
    {122, "High 4:2:2"}, {244, "High 4:4:4"}, {0, NULL } };
  const char *nome = "?";
  int prof, lvl, bits = -1;
  if (n < 7 || c[0] != 1) { snprintf(d, t, "avcC unreadable (%lld B)", n); return; }
  prof = c[1]; lvl = c[3];
  for (int i = 0; P[i].nome; i++) if (P[i].id == prof) nome = P[i].nome;
  // Depois das SPS/PPS, perfis 100+ trazem chroma e profundidade.
  { long long o = 5; int ns = c[o++] & 31;
    for (int i = 0; i < ns && o + 2 <= n; i++) { int l = (c[o] << 8) | c[o + 1]; o += 2 + l; }
    if (o < n) { int np = c[o++]; for (int i = 0; i < np && o + 2 <= n; i++) { int l = (c[o] << 8) | c[o + 1]; o += 2 + l; } }
    if (prof >= 100 && o + 3 <= n) bits = 8 + (c[o + 1] & 7); }
  if (bits < 0) bits = prof == 110 ? 10 : 8;
  snprintf(d, t, "AVC %s@L%d.%d %d-bit", nome, lvl / 10, lvl % 10, bits);
}
static void perfilHevc(const unsigned char *c, long long n, char *d, size_t t) {
  int prof, lvl, bits;
  if (n < 23) { snprintf(d, t, "hvcC unreadable (%lld B)", n); return; }
  prof = c[1] & 31; lvl = c[12]; bits = 8 + (c[17] & 7);
  snprintf(d, t, "HEVC %s@L%d.%d %d-bit", prof == 1 ? "Main" : prof == 2 ? "Main 10" : prof == 4 ? "RExt" : "?",
           lvl / 30, (lvl % 30) / 3, bits);
}

static void lerVideo(const unsigned char *p, long long n, Faixa *f) {
  long long o = 0;
  while (o < n) {
    int ui, us; uint32_t id = lerId(p + o, (long)(n - o), &ui); long long s;
    if (!id) break;
    s = lerTam(p + o + ui, (long)(n - o - ui), &us);
    if (s < 0 || o + ui + us + s > n) break;
    const unsigned char *v = p + o + ui + us;
    if (id == 0xB0) f->w = (unsigned)lerUint(v, s);
    else if (id == 0xBA) f->h = (unsigned)lerUint(v, s);
    else if (id == 0x55B0) {   // Colour > BitsPerChannel
      long long q = 0;
      while (q < s) { int a, b; uint32_t i2 = lerId(v + q, (long)(s - q), &a); long long s2;
        if (!i2) break; s2 = lerTam(v + q + a, (long)(s - q - a), &b); if (s2 < 0 || q + a + b + s2 > s) break;
        if (i2 == 0x55B2) f->bpc = (unsigned)lerUint(v + q + a + b, s2);
        q += a + b + s2; }
    }
    o += ui + us + s;
  }
}
static void lerAudio(const unsigned char *p, long long n, Faixa *f) {
  long long o = 0;
  while (o < n) {
    int ui, us; uint32_t id = lerId(p + o, (long)(n - o), &ui); long long s;
    if (!id) break;
    s = lerTam(p + o + ui, (long)(n - o - ui), &us);
    if (s < 0 || o + ui + us + s > n) break;
    const unsigned char *v = p + o + ui + us;
    if (id == 0xB5) f->freq = lerFloat(v, s);
    else if (id == 0x9F) f->canais = (unsigned)lerUint(v, s);
    else if (id == 0x6264) f->bits = (unsigned)lerUint(v, s);
    o += ui + us + s;
  }
}
// ContentEncodings > ContentEncoding > ContentCompression(ContentCompAlgo) /
// ContentEncryption: varre a subarvore inteira atras dos dois.
static void lerCodif(const unsigned char *p, long long n, Faixa *f) {
  long long o = 0;
  while (o < n) {
    int ui, us; uint32_t id = lerId(p + o, (long)(n - o), &ui); long long s;
    if (!id) break;
    s = lerTam(p + o + ui, (long)(n - o - ui), &us);
    if (s < 0 || o + ui + us + s > n) break;
    const unsigned char *v = p + o + ui + us;
    if (id == 0x6240 || id == 0x5034) { if (id == 0x5034) { f->comp = 1; f->algo = 0; } lerCodif(v, s, f); }
    else if (id == 0x4254) f->algo = (unsigned)lerUint(v, s);
    else if (id == 0x5035) f->cifra = 1;
    o += ui + us + s;
  }
}

static void lerFaixa(Est *e, const unsigned char *p, long long n) {
  Faixa f;
  long long o = 0;
  const unsigned char *cp = NULL;
  char linha[512], ex[200] = "";
  memset(&f, 0, sizeof f);
  f.def = 1; f.hab = 1; f.lacing = 1; snprintf(f.lang, sizeof f.lang, "eng");
  while (o < n) {
    int ui, us; uint32_t id = lerId(p + o, (long)(n - o), &ui); long long s;
    if (!id) break;
    s = lerTam(p + o + ui, (long)(n - o - ui), &us);
    if (s < 0 || o + ui + us + s > n) break;
    const unsigned char *v = p + o + ui + us;
    switch (id) {
      case 0xD7: f.num = (unsigned)lerUint(v, s); break;
      case 0x83: f.tipo = (unsigned)lerUint(v, s); break;
      case 0x88: f.def = (unsigned)lerUint(v, s); break;
      case 0xB9: f.hab = (unsigned)lerUint(v, s); break;
      case 0x55AA: f.forc = (unsigned)lerUint(v, s); break;
      case 0x9C: f.lacing = (unsigned)lerUint(v, s); break;
      case 0x86: lerTexto(v, s, f.codec, sizeof f.codec); break;
      case 0x536E: lerTexto(v, s, f.nome, sizeof f.nome); break;
      case 0x22B59C: lerTexto(v, s, f.lang, sizeof f.lang); break;
      case 0x22B59D: lerTexto(v, s, f.lang, sizeof f.lang); break;
      case 0x63A2: cp = v; f.cpN = s; break;
      case 0xE0: lerVideo(v, s, &f); break;
      case 0xE1: lerAudio(v, s, &f); break;
      case 0x6D80: lerCodif(v, s, &f); break;
    }
    o += ui + us + s;
  }
  e->faixas++;
  if (f.tipo == 1 && cp) {
    if (!strcmp(f.codec, "V_MPEG4/ISO/AVC")) perfilAvc(cp, f.cpN, f.perfil, sizeof f.perfil);
    else if (!strcmp(f.codec, "V_MPEGH/ISO/HEVC")) perfilHevc(cp, f.cpN, f.perfil, sizeof f.perfil);
  }
  if (f.tipo == 1) snprintf(ex, sizeof ex, " %ux%u %s%s", f.w, f.h, f.perfil[0] ? f.perfil : "",
                            f.bpc ? " colour-bits=" : "");
  if (f.tipo == 1 && f.bpc) snprintf(ex + strlen(ex), sizeof ex - strlen(ex), "%u", f.bpc);
  if (f.tipo == 2) snprintf(ex, sizeof ex, " %uch %.0f Hz", f.canais, f.freq);
  if (f.tipo == 2 && f.bits) snprintf(ex + strlen(ex), sizeof ex - strlen(ex), " %u-bit", f.bits);
  snprintf(linha, sizeof linha,
           "[abrir-diag] %s track #%u %s %s lang=%s default=%u enabled=%u forced=%u%s codecPrivate=%lld B%s%s%s%s \"%s\"\n",
           e->tag, f.num, f.tipo == 1 ? "video" : f.tipo == 2 ? "audio" : f.tipo == 17 ? "subtitle" : "other",
           f.codec, f.lang, f.def, f.hab, f.forc, ex, f.cpN,
           f.comp ? (f.algo == 3 ? " COMPRESSED(header-stripping)" : f.algo == 0 ? " COMPRESSED(zlib)" : " COMPRESSED(other)") : "",
           f.cifra ? " ENCRYPTED" : "", f.lacing ? "" : " no-lacing", "", f.nome);
  fputs(linha, stdout);
  if (f.comp) e->comprimidas++;
  if (f.tipo == 2 && !e->primAudio[0]) snprintf(e->primAudio, sizeof e->primAudio, "%s #%u", f.codec, f.num);
  if (f.tipo == 2 && f.def && !e->defAudioCodecSet) { snprintf(e->defAudio, sizeof e->defAudio, "%s", f.codec); e->defAudioCodecSet = 1; }
  { char n2[160];
    if (f.comp) { snprintf(n2, sizeof n2, "track #%u %s is %s-compressed", f.num, f.codec, f.algo == 3 ? "header-stripping" : "zlib"); nota(e, n2); }
    if (f.tipo == 1 && (strstr(f.perfil, "10-bit") || strstr(f.perfil, "High 10")) && !strcmp(f.codec, "V_MPEG4/ISO/AVC"))
      nota(e, "10-bit H.264 video (no hardware decoder on Samsung)");
    if (f.tipo == 2 && (!strncmp(f.codec, "A_DTS", 5) || !strncmp(f.codec, "A_TRUEHD", 8)) && f.def) {
      snprintf(n2, sizeof n2, "default audio #%u is %s (the TV has no decoder)", f.num, f.codec); nota(e, n2); }
    if (f.cifra) nota(e, "encrypted track"); }
}

static void lerTracks(Est *e, const unsigned char *p, long long n) {
  long long o = 0;
  while (o < n) {
    int ui, us; uint32_t id = lerId(p + o, (long)(n - o), &ui); long long s;
    if (!id) break;
    s = lerTam(p + o + ui, (long)(n - o - ui), &us);
    if (s < 0 || o + ui + us + s > n) break;
    if (id == 0xAE) lerFaixa(e, p + o + ui + us, s);
    o += ui + us + s;
  }
}
static void lerSeek(Est *e, const unsigned char *p, long long n) {
  long long o = 0;
  while (o < n) {
    int ui, us; uint32_t id = lerId(p + o, (long)(n - o), &ui); long long s;
    if (!id) break;
    s = lerTam(p + o + ui, (long)(n - o - ui), &us);
    if (s < 0 || o + ui + us + s > n) break;
    if (id == 0x4DBB) {
      const unsigned char *v = p + o + ui + us; long long q = 0; uint32_t alvo = 0; long long pos = -1;
      while (q < s) { int a, b; uint32_t i2 = lerId(v + q, (long)(s - q), &a); long long s2;
        if (!i2) break; s2 = lerTam(v + q + a, (long)(s - q - a), &b); if (s2 < 0 || q + a + b + s2 > s) break;
        if (i2 == 0x53AB) alvo = (uint32_t)lerUint(v + q + a + b, s2);
        else if (i2 == 0x53AC) pos = (long long)lerUint(v + q + a + b, s2);
        q += a + b + s2; }
      if (pos >= 0) {
        pos += e->segIni;
        if (alvo == 0x1C53BB6B) e->seekCues = pos; else if (alvo == 0x1941A469) e->seekAnexos = pos;
        else if (alvo == 0x1254C367) e->seekTags = pos; else if (alvo == 0x1043A770) e->seekCaps = pos;
        else if (alvo == 0x1654AE6B) e->seekTracks = pos; else if (alvo == 0x1549A966) e->seekInfo = pos;
        else if (alvo == 0x114D9B74) e->seekSH2 = pos;
      }
    }
    o += ui + us + s;
  }
}
static void lerInfo(Est *e, const unsigned char *p, long long n) {
  long long o = 0;
  while (o < n) {
    int ui, us; uint32_t id = lerId(p + o, (long)(n - o), &ui); long long s;
    if (!id) break;
    s = lerTam(p + o + ui, (long)(n - o - ui), &us);
    if (s < 0 || o + ui + us + s > n) break;
    const unsigned char *v = p + o + ui + us;
    if (id == 0x2AD7B1) e->escala = lerUint(v, s);
    else if (id == 0x4489) e->dur = lerFloat(v, s);
    else if (id == 0x4D80) lerTexto(v, s, e->muxing, sizeof e->muxing);
    else if (id == 0x5741) lerTexto(v, s, e->writing, sizeof e->writing);
    o += ui + us + s;
  }
}
static void lerAnexos(Est *e, const unsigned char *p, long long n) {
  long long o = 0;
  while (o < n) {
    int ui, us; uint32_t id = lerId(p + o, (long)(n - o), &ui); long long s;
    if (!id) break;
    s = lerTam(p + o + ui, (long)(n - o - ui), &us);
    if (s < 0 || o + ui + us + s > n) break;
    if (id == 0x61A7) {
      const unsigned char *v = p + o + ui + us; long long q = 0; char mime[48] = "";
      while (q < s) { int a, b; uint32_t i2 = lerId(v + q, (long)(s - q), &a); long long s2;
        if (!i2) break; s2 = lerTam(v + q + a, (long)(s - q - a), &b); if (s2 < 0 || q + a + b + s2 > s) break;
        if (i2 == 0x4660) lerTexto(v + q + a + b, s2, mime, sizeof mime);
        q += a + b + s2; }
      e->anexos++;
      if (strstr(mime, "font") || strstr(mime, "truetype") || strstr(mime, "opentype")) e->anexosFontes++;
    }
    o += ui + us + s;
  }
}

static void ordem(Est *e, const char *nome, long long abs, long long tam, int inteiro) {
  char a[24], b[24];
  e->nElem++;
  if (e->nElem > 24) return;
  mb(a, sizeof a, abs); mb(b, sizeof b, tam);
  if (e->ordemN < sizeof e->ordem - 2)
    e->ordemN += (size_t)snprintf(e->ordem + e->ordemN, sizeof e->ordem - e->ordemN, "%s%s@%s(%s%s)",
                                  e->ordemN ? " > " : "", nome, a, b, inteiro ? "" : ", not read");
}

// Percorre os filhos do Segment em [base, base+n) a partir de `pos` (absoluto).
// Devolve o proximo `pos` que ficou fora do trecho, ou -1 (Cluster achado /
// fim / invalido).
static long long percorrer(Est *e, const unsigned char *buf, long n, long long base, long long pos) {
  while (pos - base >= 0 && pos - base < n) {
    long long o = pos - base, s; int ui, us;
    uint32_t id = lerId(buf + o, (long)(n - o), &ui);
    const char *nome;
    if (!id) return pos;   // cabecalho cortado: pede de novo a partir daqui
    s = lerTam(buf + o + ui, (long)(n - o - ui), &us);
    if (s == -2) return pos;
    nome = nomeNivel1(id);
    if (!nome) { char d[40]; snprintf(d, sizeof d, "unknown element 0x%X at offset %lld", (unsigned)id, pos); nota(e, d); return -1; }
    if (id == 0x1F43B675) { e->cluster = pos; ordem(e, nome, pos, s, 1); return -1; }
    if (s < 0) { nota(e, "level-1 element with unknown size"); return -1; }
    { int inteiro = o + ui + us + s <= n;
      const unsigned char *v = buf + o + ui + us;
      ordem(e, nome, pos, s, inteiro);
      if (id == 0x1941A469) e->anexosTam += s;
      if (inteiro) {
        if (id == 0x114D9B74) lerSeek(e, v, s);
        else if (id == 0x1549A966) lerInfo(e, v, s);
        else if (id == 0x1654AE6B) lerTracks(e, v, s);
        else if (id == 0x1941A469) lerAnexos(e, v, s);
      } else if (id == 0x1654AE6B) nota(e, "Tracks did not fit in the read window");
    }
    pos += ui + us + s;
  }
  return pos;
}

// Cabecalho EBML + inicio do Segment. Devolve o 1o filho do Segment, ou -1.
static long long abrirSegmento(Est *e, const unsigned char *buf, long n) {
  int ui, us; uint32_t id; long long s, o;
  id = lerId(buf, n, &ui);
  if (id != 0x1A45DFA3) return -1;
  s = lerTam(buf + ui, n - ui, &us);
  if (s < 0 || ui + us + s > n) return -1;
  { long long q = 0; const unsigned char *v = buf + ui + us;
    while (q < s) { int a, b; uint32_t i2 = lerId(v + q, (long)(s - q), &a); long long s2;
      if (!i2) break; s2 = lerTam(v + q + a, (long)(s - q - a), &b); if (s2 < 0 || q + a + b + s2 > s) break;
      if (i2 == 0x4282) lerTexto(v + q + a + b, s2, e->docType, sizeof e->docType);
      else if (i2 == 0x4287) e->docVer = (int)lerUint(v + q + a + b, s2);
      else if (i2 == 0x4285) e->docReadVer = (int)lerUint(v + q + a + b, s2);
      q += a + b + s2; } }
  o = ui + us + s;
  id = lerId(buf + o, n - (long)o, &ui);
  if (id != 0x18538067) return -1;
  s = lerTam(buf + o + ui, n - (long)o - ui, &us);
  e->segTam = s;
  e->segIni = o + ui + us;
  return e->segIni;
}

static void resumo(Est *e) {
  char a[24], b[24], c[24], t[24];
  mb(t, sizeof t, e->total);
  printf("[abrir-diag] %s container: %s v%d (read v%d), file %s, segment %s, muxer=\"%s\" writer=\"%s\", duration %.0f s\n",
         e->tag, e->docType[0] ? e->docType : "?", e->docVer, e->docReadVer, t,
         e->segTam < 0 ? "unknown size (live-style)" : "sized", e->muxing, e->writing,
         e->dur > 0 ? e->dur * (e->escala ? (double)e->escala : 1e6) / 1e9 : 0.0);
  printf("[abrir-diag] %s layout: %s\n", e->tag, e->ordem);
  mb(a, sizeof a, e->cluster); mb(b, sizeof b, e->anexosTam); mb(c, sizeof c, e->seekCues);
  printf("[abrir-diag] %s first frame at %s | attachments %d (%d fonts, %s) | cues at %s%s | %d track(s), %d compressed | default audio %s%s\n",
         e->tag, e->cluster >= 0 ? a : "not found", e->anexos, (int)e->anexosFontes, b,
         e->seekCues >= 0 ? c : "?",
         e->seekCues >= 0 && e->total > 0 ? (e->seekCues > e->total / 2 ? (e->cuesSH2 ? " (end of file, via second SeekHead)" : " (end of file)")
                                                                   : (e->cuesSH2 ? " (start of file, via second SeekHead)" : " (start of file)")) : "",
         e->faixas, e->comprimidas, e->defAudio[0] ? e->defAudio : e->primAudio[0] ? "none flagged; first is " : "-",
         e->defAudio[0] ? "" : e->primAudio);
  if (e->cluster > 8LL * 1024 * 1024) { char d[80]; snprintf(d, sizeof d, "the player must read %s before the first frame", a); nota(e, d); }
  if (e->anexosTam > 4LL * 1024 * 1024) nota(e, "large attachments (fonts) before the video");
  if (e->docVer > 4) nota(e, "DocTypeVersion above 4");
  if (e->faixas > 24) nota(e, "many tracks");
  printf("[abrir-diag] %s notes: %s\n", e->tag, e->notas[0] ? e->notas : "nothing unusual in the header");
  fflush(stdout);
}

long long abrirdiag_ler(const unsigned char *buf, long n, long long total, const char *tag) {
  Est e;
  long long pos;
  memset(&e, 0, sizeof e);
  e.seekCues = e.seekAnexos = e.seekTags = e.seekCaps = e.seekTracks = e.seekInfo = -1;
  e.seekSH2 = -1;
  e.cluster = -1; e.total = total; e.tag = tag ? tag : "";
  pos = abrirSegmento(&e, buf, n);
  if (pos < 0) { printf("[abrir-diag] %s not a Matroska file (or header unreadable)\n", e.tag); fflush(stdout); return -1; }
  pos = percorrer(&e, buf, n, 0, pos);
  resumo(&e);
  return pos;
}

// --- rede ---------------------------------------------------------------------

#ifndef __EMSCRIPTEN__
#include "rede.h"
#include <pthread.h>
#include <SDL2/SDL.h>

#define AD_JANELA (4L * 1024 * 1024 - 1)
#define AD_SALTO  (64L * 1024)

typedef struct { char *url, *cab, porque[24]; } Pedido;

static pthread_mutex_t vistosTrava = PTHREAD_MUTEX_INITIALIZER;
static unsigned vistos[64]; static int nVistos, proxVisto;
static unsigned hashUrl(const char *s) { unsigned h = 2166136261u; for (; *s && *s != '?'; s++) h = (h ^ (unsigned char)*s) * 16777619u; return h; }

static void hostDe(const char *u, char *d, size_t t) {
  const char *p = strstr(u, "://"), *f;
  d[0] = 0;
  if (!p) return;
  p += 3; f = p;
  while (*f && *f != '/' && *f != '?' && *f != ':') f++;
  snprintf(d, t, "%.*s", (int)(f - p), p);
}

static void *fio(void *u) {
  Pedido *pd = u;
  Est e;
  long n = 0; int64_t total = -1; int status = 0, cruzou = 0, saltos = 0;
  char final[8192] = "", host[96], hostF[96], tag[48];
  Uint32 t0 = SDL_GetTicks();
  unsigned char *buf;
  long long pos;
  snprintf(tag, sizeof tag, "[%s]", pd->porque);
  hostDe(pd->url, host, sizeof host);
  buf = (unsigned char *)rede_baixar_trecho64_final(pd->url, pd->cab, 0, AD_JANELA, &n, &total, &status, NULL, final, &cruzou);
  hostDe(final[0] ? final : pd->url, hostF, sizeof hostF);
  printf("[abrir-diag] %s header read: HTTP %d, %ld bytes in %u ms, file size %lld, host %s%s%s\n", tag, status, n,
         (unsigned)(SDL_GetTicks() - t0), (long long)total, host, strcmp(host, hostF) ? " -> " : "", strcmp(host, hostF) ? hostF : "");
  fflush(stdout);
  if (!buf || n < 64) { free(buf); goto fim; }
  memset(&e, 0, sizeof e);
  e.seekCues = e.seekAnexos = e.seekTags = e.seekCaps = e.seekTracks = e.seekInfo = -1;
  e.seekSH2 = -1;
  e.cluster = -1; e.total = total; e.tag = tag;
  pos = abrirSegmento(&e, buf, n);
  if (pos < 0) {
    printf("[abrir-diag] %s not a Matroska file: first bytes %02X %02X %02X %02X\n", tag, buf[0], buf[1], buf[2], buf[3]);
    fflush(stdout); free(buf); goto fim;
  }
  pos = percorrer(&e, buf, n, 0, pos);
  free(buf);
  // Elementos maiores que a janela (anexos/fontes): salta pelo cabecalho de
  // cada um ate o 1o Cluster, com Ranges pequenos.
  while (pos > 0 && saltos < 12 && (total < 0 || pos < total)) {
    long m = 0; int st = 0;
    unsigned char *b = (unsigned char *)rede_baixar_trecho64_final(pd->url, pd->cab, pos, pos + AD_SALTO - 1, &m, NULL, &st, NULL, NULL, NULL);
    saltos++;
    if (!b || m < 16) { free(b); nota(&e, "could not read past the window"); break; }
    pos = percorrer(&e, b, m, pos, pos);
    free(b);
  }
  if (saltos) { char d[48]; snprintf(d, sizeof d, "%d extra read(s) to reach the first frame", saltos); nota(&e, d); }
  // O SeekHead do inicio nao lista o Cues e aponta para outro SeekHead (o que o
  // mkvpropedit deixa quando falta espaco): le o segundo. O player da TV nao o
  // segue e nao busca nesse arquivo (mkvfix.h).
  if (e.seekCues < 0 && e.seekSH2 > 0) {
    long m = 0; int st = 0, ui = 0, us = 0;
    unsigned char *b = (unsigned char *)rede_baixar_trecho64_final(pd->url, pd->cab, e.seekSH2, total > 0 && e.seekSH2 + 4095 >= total ? total - 1 : e.seekSH2 + 4095, &m, NULL, &st, NULL, NULL, NULL);
    uint32_t id = b && m > 8 ? lerId(b, m, &ui) : 0;
    long long s = id == 0x114D9B74 ? lerTam(b + ui, m - ui, &us) : -2;
    if (s > 0 && ui + us + s <= m) { lerSeek(&e, b + ui + us, s); e.cuesSH2 = e.seekCues >= 0; }
    nota(&e, e.cuesSH2 ? "the first SeekHead only points to a second SeekHead, which lists the Cues (the TV player does not follow it)"
                       : "the first SeekHead only points to a second SeekHead, without Cues in it");
    free(b);
  }
  resumo(&e);
fim:
  free(pd->url); free(pd->cab); free(pd);
  return NULL;
}

void abrirdiag_sondar(const char *url, const char *cabecalhos, const char *porque) {
  Pedido *pd;
  pthread_t t;
  unsigned h;
  if (!url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))) return;
  if (strstr(url, "127.0.0.1")) return;   // conversor/proxy local: nao e o arquivo
  h = hashUrl(url);
  pthread_mutex_lock(&vistosTrava);
  for (int i = 0; i < nVistos; i++) if (vistos[i] == h) { pthread_mutex_unlock(&vistosTrava); return; }
  vistos[proxVisto] = h; proxVisto = (proxVisto + 1) % 64; if (nVistos < 64) nVistos++;
  pthread_mutex_unlock(&vistosTrava);
  pd = calloc(1, sizeof *pd);
  if (!pd) return;
  pd->url = strdup(url); pd->cab = strdup(cabecalhos ? cabecalhos : "");
  snprintf(pd->porque, sizeof pd->porque, "%s", porque ? porque : "?");
  if (!pd->url || !pd->cab || pthread_create(&t, NULL, fio, pd) != 0) { free(pd->url); free(pd->cab); free(pd); return; }
  pthread_detach(t);
}
#else
void abrirdiag_sondar(const char *url, const char *cabecalhos, const char *porque) { (void)url; (void)cabecalhos; (void)porque; }
#endif
