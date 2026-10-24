// A folha "ASS performance" do player (.tpk): Legendas > Estilo > ultima
// celula. Uma linha por chave de assperf.h, em duas colunas; a de cima e o
// preset. Cima/baixo andam pelas linhas (desce da coluna da esquerda para a da
// direita), esquerda/direita e OK mudam o valor na hora, Voltar fecha. No pe:
// a ajuda da linha em foco e as medidas da ultima janela de 10 s, as mesmas
// que vao ao log "[assperf] 10s".
#include "assperfui.h"
#include "assperf.h"
#ifdef NV_TPK
#include "tpkscan.h"
#endif
#include "plrui.h"
#include "text.h"
#include "gfx.h"
#include <stdio.h>
#include <string.h>

/* Linhas da folha: o preset e as chaves que ainda se ajustam, nesta ordem.
 * As chaves fixas (assperf_fixa: decididas nos testes na TV) nao aparecem. */
static const int AU_ORDEM[] = {
  AP_GPUBLUR, AP_GPUMIN, AP_FIOS, AP_ESCALA, AP_ADIANTE, AP_CACHE, AP_HINTING, AP_FONTES,
  AP_PODA, AP_RELAY, AP_SNAP, AP_CARTAO, AP_LUZ, AP_CINZA, AP_JANELA, AP_SCAN, AP_BENCH, AP_ESTAGIOS, AP_LOG,
};
#define AU_NCHAVES ((int)(sizeof AU_ORDEM / sizeof AU_ORDEM[0]))
#define AU_LINHAS (AU_NCHAVES + 1)          // 0 = preset, 1.. = as chaves
#define AU_POR_COLUNA ((AU_LINHAS + 1) / 2)
#define AU_LINHA_H 50.0f
#define AU_TOPO 64.0f
#define AU_PE 118.0f
#define AU_PAD_X 34.0f
#define AU_PAD_Y 22.0f

static int aberto, foco;

int assperfui_aberto(void) { return aberto; }
void assperfui_abrir(void) { aberto = 1; foco = 0; assperf_iniciar(); }
void assperfui_fechar(void) { aberto = 0; }

float assperfui_altura(void) {
  return AU_PAD_Y + AU_TOPO + AU_POR_COLUNA * AU_LINHA_H + 12.0f + AU_PE + AU_PAD_Y;
}

static void mudar(int d) {
  if (foco == 0) {
    // Recommended <-> Lightest; de "Custom"/"Safe" vai para Recommended.
    const char *n = assperf_preset_nome();
    (void)d;
    assperf_preset(!strcmp(n, "Recommended") ? 2 : 0);
    return;
  }
  { int id = AU_ORDEM[foco - 1], n = assperf_n_valores(id), v = assperf_get(id);
    if (n < 2) return;
    assperf_definir(id, (v + (d < 0 ? n - 1 : 1)) % n); }
}

int assperfui_tecla(SDL_Keycode k) {
  if (!aberto) return 0;
  if (k == SDLK_AC_BACK || k == SDLK_ESCAPE || k == SDLK_BACKSPACE) { aberto = 0; return 0; }
  if (k == SDLK_UP) { if (foco > 0) foco--; }
  else if (k == SDLK_DOWN) { if (foco + 1 < AU_LINHAS) foco++; }
  else if (k == SDLK_LEFT) mudar(-1);
  else if (k == SDLK_RIGHT || k == SDLK_RETURN || k == SDLK_KP_ENTER) mudar(1);
  return 1;
}

static void linha(int i, GfxRect r, float a) {
  int sel = i == foco;
  const char *rot = i == 0 ? "Preset" : assperf_rotulo(AU_ORDEM[i - 1]);
  const char *val = i == 0 ? assperf_preset_nome() : assperf_valor_rotulo(AU_ORDEM[i - 1], assperf_get(AU_ORDEM[i - 1]));
  int velho = i > 0 && strstr(val, "(old)") != NULL;
  if (sel) plrui_linha_foco(r, 16.0f, a);
  { TxtLinha lr = txt_linha_corta(TXT_G20B, rot, 243, 242, 239, sel ? 255 : 200, r.w * 0.56f - 20.0f);
    TxtLinha lv = txt_linha_corta(TXT_G20B, val, velho ? 170 : 243, velho ? 170 : 242, velho ? 170 : 239, 255,
                                  r.w * 0.44f - 46.0f);
    float yc = r.y + r.h * 0.5f;
    txt_desenhar_alpha(lr, r.x + 18.0f, yc - (float)lr.h * 0.5f, a);
    txt_desenhar_alpha(lv, r.x + r.w - 18.0f - (float)lv.w, yc - (float)lv.h * 0.5f, a);
    if (sel) {
      TxtLinha s1 = txt_linha(TXT_G16B, "\xe2\x80\xb9", 243, 242, 239, 160), s2 = txt_linha(TXT_G16B, "\xe2\x80\xba", 243, 242, 239, 160);
      txt_desenhar_alpha(s1, r.x + r.w - 30.0f - (float)lv.w - (float)s1.w, yc - (float)s1.h * 0.5f, a);
      txt_desenhar_alpha(s2, r.x + r.w - 10.0f, yc - (float)s2.h * 0.5f, a);
    } }
}

void assperfui_corpo(GfxRect c, float a) {
  float x0 = c.x + AU_PAD_X, w = c.w - AU_PAD_X * 2.0f, y = c.y + AU_PAD_Y;
  float cw = (w - 24.0f) * 0.5f;
  int i;
#ifdef NV_TPK
  tpkscan_tick();   // fio GL: "Scan display APIs"
#endif
  { TxtLinha t = txt_linha(TXT_G30B, "ASS performance (test)", 243, 242, 239, 255);
    const char *k[3] = { "\xe2\x86\x91 \xe2\x86\x93", "\xe2\x86\x90 \xe2\x86\x92", "Voltar" };
    const char *rt[3] = { "Op\xc3\xa7\xc3\xa3o", "Mudar", "Fechar" };
    float yc = y + AU_TOPO * 0.5f;
    txt_desenhar_alpha(t, x0, yc - (float)t.h * 0.5f, a);
    plrui_dicas(k, rt, 3, x0 + w, yc, 1, a); }
  y += AU_TOPO;
  for (i = 0; i < AU_LINHAS; i++) {
    int col = i / AU_POR_COLUNA, lin = i % AU_POR_COLUNA;
    GfxRect r = { x0 + col * (cw + 24.0f), y + lin * AU_LINHA_H, cw, AU_LINHA_H - 4.0f };
    linha(i, r, a);
  }
  y += AU_POR_COLUNA * AU_LINHA_H + 12.0f;
  { char med[200];
    const char *ajuda = foco == 0 ? "Recommended = the tested settings (NEON, authored style, 3 blur threads) + blur on the GPU. Safe = CPU blur, 1 thread. "
                                     "Lightest = Recommended + 75% render resolution, no hinting, 128 MB cache."
                                  : assperf_ajuda(AU_ORDEM[foco - 1]);
    txt_bloco_corta(TXT_G18M, ajuda, 243, 242, 239, x0, y, w, 26.0f, a * 0.85f, 2);
    assperf_med_texto(med, sizeof med);
    { TxtLinha lm = txt_linha_corta(TXT_G18M, med, 160, 200, 255, 255, w);
      txt_desenhar_alpha(lm, x0, y + 62.0f, a); }
    { TxtLinha lt = txt_linha_corta(TXT_G16B, "Changes apply live. Every change is written to the network log (Subtitles tab).",
                                    243, 242, 239, 128, w);
      txt_desenhar_alpha(lt, x0, y + 92.0f, a); } }
}
