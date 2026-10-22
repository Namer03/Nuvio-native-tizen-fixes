// THUMBNAIL LAB panel (see minilab.h). A plain list on the left of the player:
// the live readout on top, then one row per knob. UP/DOWN move, LEFT/RIGHT
// change the value (saved at once, used by the next tile), OK flips an on/off
// row or runs an action row, Back closes. Test tool: English only, no
// animation, solid rectangles (cheap on the TV's GPU).
#include "minilab.h"

#if defined(NV_TPK)
#define NV_ESCALA_TELA
#include "escala.h"
#include "gfx.h"
#include "player.h"
#include "teclavoltar.h"
#include "text.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum { T_NUM, T_LIG, T_ACAO };
enum { A_PADRAO = 1, A_FECHAR };

typedef struct {
  const char *rotulo;
  int tipo;
  size_t off;                 // field in MiniAjustes (T_NUM / T_LIG)
  int min, max, passo;
  const char *unidade;
  const char *dica;
} Linha;

#define F(x) offsetof(MiniAjustes, x)
static const Linha LINHAS[] = {
  { "Make thumbnails",          T_LIG, F(parado),        0, 1, 1, "",     "Off stops making tiles (testing without them)" },
  { "Memory floor",             T_NUM, F(pisoMB),       50, 400, 10, "MB", "MemAvailable always left free" },
  { "Extra floor while DTS",    T_NUM, F(dtsExtraMB),    0, 200, 10, "MB", "Added while the DTS converter runs" },
  { "Measured cost margin",     T_NUM, F(margemPct),   100, 200, 5,  "%",  "Largest recent tile cost x this" },
  { "4K cost before measuring", T_NUM, F(estimativa4kMB), 20, 120, 5, "MB", "Scaled by picture size" },
  { "Measure after",            T_NUM, F(medirApos),     1, 8, 1,    "tiles", "Then the measured cost is used" },
  { "Decoding threads",         T_NUM, F(threads),       1, 3, 1,    "",   "More = faster only if the CPU is free" },
  { "Keep decoder open",        T_LIG, F(manterDecoder), 0, 1, 1,    "",   "Faster restart, holds ~30 MB between tiles" },
  { "Prefetch next keyframe",   T_LIG, F(prefetch),      0, 1, 1,    "",   "Downloads the next one while decoding (MKV/MP4)" },
  { "Read per request",         T_NUM, F(lerBlocos),     1, 7, 1,    "x256 KB", "MKV/MP4 range request size" },
  { "Exact reads",              T_LIG, F(exato),         0, 1, 1,    "",   "Only what the demuxer asks for (more requests)" },
  { "M2TS read per request",    T_NUM, F(tsBlocos),      1, 7, 1,    "x256 KB", "M2TS/TS forward read size" },
  { "M2TS probe windows",       T_LIG, F(tsSonda),       0, 1, 1,    "",   "Look for the keyframe in small windows" },
  { "Probe window",             T_NUM, F(tsJanelaKB),   16, 512, 16, "KB", "" },
  { "Probe step",               T_NUM, F(tsPassoKB),    64, 4096, 64, "KB", "" },
  { "Probes per try",           T_NUM, F(tsSondasMax),   1, 32, 1,   "",   "" },
  { "Memory re-check",          T_NUM, F(pausaMs),     250, 5000, 250, "ms", "While paused for memory" },
  { "Pause between tiles",      T_NUM, F(intervaloMs),   0, 2000, 10, "ms", "Gives the CPU back to the player" },
  { "Reset to defaults",        T_ACAO, 0, A_PADRAO, 0, 0, "", "" },
  { "Close",                    T_ACAO, 0, A_FECHAR, 0, 0, "", "Back also closes" },
};
#undef F
#define NL ((int)(sizeof LINHAS / sizeof *LINHAS))
#define VISIVEIS 13

static int aberto, foco, topo;

static int *campo(const Linha *l) { return (int *)((char *)mini_ajustes() + l->off); }

void minilab_abrir(void) {
  if (!mini_lab_disponivel()) return;
  aberto = 1;
  printf("[miniatura] lab opened\n");
  fflush(stdout);
}
void minilab_fechar(void) { aberto = 0; }
int  minilab_aberto(void) { return aberto; }

static void mudar(int dir) {
  const Linha *l = &LINHAS[foco];
  int *v;
  if (l->tipo == T_ACAO) return;
  v = campo(l);
  if (l->tipo == T_LIG) *v = !*v;
  else {
    *v += dir * l->passo;
    if (*v < l->min) *v = l->min;
    if (*v > l->max) *v = l->max;
  }
  mini_ajustes_gravar();
  printf("[miniatura] lab: %s = %d\n", l->rotulo, *v);
  fflush(stdout);
}

int minilab_evento(const SDL_Event *e) {
  SDL_Keycode k;
  if (!aberto) return 0;
  if (!player_aberto()) { aberto = 0; return 0; }   // the player closed under it
  if (e->type != SDL_KEYDOWN) return 1;            // modal: the player gets nothing
  k = e->key.keysym.sym;
  if (nv_tecla_voltar(e)) { minilab_fechar(); return 1; }
  if (k == SDLK_UP) foco = (foco + NL - 1) % NL;
  else if (k == SDLK_DOWN) foco = (foco + 1) % NL;
  else if (k == SDLK_LEFT) mudar(-1);
  else if (k == SDLK_RIGHT) mudar(1);
  else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
    const Linha *l = &LINHAS[foco];
    if (l->tipo == T_LIG) mudar(1);
    else if (l->tipo == T_ACAO && l->min == A_PADRAO) {
      mini_ajustes_padrao(mini_ajustes());
      mini_ajustes_gravar();
      printf("[miniatura] lab: defaults restored\n");
      fflush(stdout);
    } else if (l->tipo == T_ACAO && l->min == A_FECHAR) minilab_fechar();
  }
  if (foco < topo) topo = foco;
  if (foco >= topo + VISIVEIS) topo = foco - VISIVEIS + 1;
  return 1;
}

static float linhaTexto(TxtEstilo e, const char *s, float x, float y, float maxW, float a, int forte) {
  TxtLinha t = txt_linha_corta(e, s, forte ? 255 : 200, forte ? 255 : 200, forte ? 255 : 204, 255, maxW);
  txt_desenhar_alpha(t, x, y, a);
  return (float)t.h;
}

static void desenhar(void) {
  MiniStats st;
  char b[256];
  float x = 60, y = 90, w = 820, pad = 24, lh = 34, rowH = 44, yy;
  int nStats = 6;
  float h = pad * 2 + 46 + nStats * lh + 16 + VISIVEIS * rowH + 40;
  mini_stats(&st);
  gfx_cor((GfxRect){ x, y, w, h }, 18.0f / h, 0.07f, 0.07f, 0.08f, 0.92f);
  yy = y + pad;
  linhaTexto(TXT_G23B, "Thumbnail lab", x + pad, yy, w - 2 * pad, 1, 1);
  yy += 46;
  snprintf(b, sizeof b, "Now: %s", st.estado[0] ? st.estado : "idle");
  linhaTexto(TXT_G20M, b, x + pad, yy, w - 2 * pad, 1, 1); yy += lh;
  snprintf(b, sizeof b, "%s  ·  %d/%d tiles (%d this session)", st.arquivo[0] ? st.arquivo : "no file yet",
           st.feitos, st.n, st.feitosSessao);
  linhaTexto(TXT_G20M, b, x + pad, yy, w - 2 * pad, 0.85f, 0); yy += lh;
  snprintf(b, sizeof b, "Last tile %u ms (decode %u)  ·  average %u ms", st.ultimoMs, st.ultimoDecodMs, st.mediaMs);
  linhaTexto(TXT_G20M, b, x + pad, yy, w - 2 * pad, 0.85f, 0); yy += lh;
  snprintf(b, sizeof b, "Read %ld KB in %d req  ·  ahead %ld KB  ·  probes %d%s  ·  total %ld MB",
           st.ultimoKB, st.ultimoPedidos, st.adiantadoKB, st.ultimasSondas, st.achouSonda ? " (found)" : "", st.lidosMB);
  linhaTexto(TXT_G20M, b, x + pad, yy, w - 2 * pad, 0.85f, 0); yy += lh;
  snprintf(b, sizeof b, "Free %ld MB (lowest %ld)  ·  floor %ld MB", st.livreMB, st.menorLivreMB, st.pisoEfetivoMB);
  linhaTexto(TXT_G20M, b, x + pad, yy, w - 2 * pad, 0.85f, 0); yy += lh;
  snprintf(b, sizeof b, "Tile cost: last %ld MB, planned %ld MB  ·  memory pauses %d",
           st.ultimoCustoMB, st.custoUsadoMB, st.pausasMem);
  linhaTexto(TXT_G20M, b, x + pad, yy, w - 2 * pad, 0.85f, 0); yy += lh;
  yy += 8;
  gfx_cor((GfxRect){ x + pad, yy, w - 2 * pad, 1 }, 0, 1, 1, 1, 0.15f);
  yy += 8;
  for (int i = topo; i < NL && i < topo + VISIVEIS; i++) {
    const Linha *l = &LINHAS[i];
    GfxRect r = { x + pad - 10, yy, w - 2 * pad + 20, rowH - 4 };
    if (i == foco) gfx_cor(r, 0.25f, 1, 1, 1, 0.14f);
    linhaTexto(TXT_G20M, l->rotulo, x + pad, yy + 8, w * 0.48f, 1, i == foco);
    b[0] = 0;
    if (l->tipo == T_LIG) {
      int v = *campo(l);
      if (l->off == offsetof(MiniAjustes, parado)) v = !v;   // "Make thumbnails" shows the opposite of `parado`
      snprintf(b, sizeof b, "%s", v ? "On" : "Off");
    } else if (l->tipo == T_NUM) snprintf(b, sizeof b, "‹ %d %s ›", *campo(l), l->unidade);
    if (b[0]) linhaTexto(TXT_G20B, b, x + w * 0.52f, yy + 8, w * 0.22f, 1, 1);
    if (i == foco && l->dica[0]) linhaTexto(TXT_G18R, l->dica, x + w * 0.52f + w * 0.2f, yy + 10, w * 0.26f - pad, 0.6f, 0);
    yy += rowH;
  }
  snprintf(b, sizeof b, "UP/DOWN choose  ·  LEFT/RIGHT change  ·  OK on/off  ·  Back close   (%d/%d)", foco + 1, NL);
  linhaTexto(TXT_ILHA_HORA, b, x + pad, y + h - pad - 18, w - 2 * pad, 0.6f, 0);
}

void minilab_desenhar(Uint32 agora) {
  (void)agora;
  if (!aberto || !player_aberto()) return;
  ESCALA_INI();
  desenhar();
  ESCALA_FIM();
}
#else
typedef int minilab_vazio;   // ISO C: a translation unit must not be empty
#endif
