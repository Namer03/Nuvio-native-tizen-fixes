// Ver seektune.h. Temporary tuning tool for the .tpk seek gesture.
#include "seektune.h"
#include "dados.h"
#include "escala.h"
#include "gfx.h"
#include "logrede.h"
#include "plrui.h"
#include "text.h"
#include <stdio.h>
#include <string.h>

#define ST_SLOTS 3

static SeekParams par = SEEKG_PADRAO;
static SeekParams slots[ST_SLOTS];
static int slotOk[ST_SLOTS], slot, logRede, carregado, aberto, linha;
static Uint32 avisoAte;
static char aviso[64];

enum { L_TAP, L_CRESCE, L_THROTTLE, L_SEGURA, L_TETO, L_VIRA, L_COMMIT, L_FILA,
       L_LOG, L_PRESET, L_SALVAR, L_PADRAO, L_N };

static const char *const NOMES[L_N] = {
  "Tap step", "Growth per press", "Repeat throttle", "Hold detect gap", "Step cap",
  "Direction flip", "Commit delay", "Press while seeking", "Network log", "Preset",
  "Save current to preset", "Reset to defaults"
};
static const char *const DICAS[L_N] = {
  "First press, and every press after a commit",
  "Each counted press grows the step (taps and hold repeats)",
  "Counts at most one hold repeat per this time; also the glide tick",
  "Keydown this close to the previous one = held key",
  "Step never grows past this share of the video",
  "Changing direction multiplies the step by this",
  "Silence before the seek is sent (Auto = 420 ms, 1000 ms with Seekr)",
  "A press while the TV is still seeking",
  "Live log at http://<tv-ip>:8099/log (Seek tab)",
  "Left/Right choose A/B/C, OK loads it",
  "OK saves the current values into the chosen preset",
  "OK puts every value back to the built-in defaults",
};

static void linhaPar(char *b, size_t n, const SeekParams *p) {
  snprintf(b, n, "%d %d %d %d %d %d %d %d", p->tapS, p->growthPct, p->throttleMs, p->holdGapMs,
           p->capPct, p->flipPct, p->commitMs, p->fila);
}
static int lerPar(const char *s, SeekParams *p) {
  SeekParams q;
  if (sscanf(s, "%d %d %d %d %d %d %d %d", &q.tapS, &q.growthPct, &q.throttleMs, &q.holdGapMs,
             &q.capPct, &q.flipPct, &q.commitMs, &q.fila) != 8) return 0;
  *p = q;
  return 1;
}

static void limitar(SeekParams *p) {
#define LIM(v, a, b) do { if ((v) < (a)) (v) = (a); if ((v) > (b)) (v) = (b); } while (0)
  LIM(p->tapS, 1, 60); LIM(p->growthPct, 0, 50); LIM(p->throttleMs, 40, 400);
  LIM(p->holdGapMs, 80, 800); LIM(p->capPct, 1, 50); LIM(p->flipPct, 10, 100);
  LIM(p->commitMs, 0, 2000); LIM(p->fila, 0, 1);
  if (p->commitMs > 0 && p->commitMs < 100) p->commitMs = 100;
#undef LIM
}

static void carregar(void) {
  char cam[600], l[200];
  FILE *f;
  if (carregado) return;
  carregado = 1;
  if (!dados_caminho(cam, sizeof cam, "seektune.txt") || !(f = fopen(cam, "r"))) return;
  while (fgets(l, sizeof l, f)) {
    if (!strncmp(l, "cur ", 4)) lerPar(l + 4, &par);
    else if (!strncmp(l, "log ", 4)) logRede = l[4] == '1';
    else if (!strncmp(l, "slot ", 5)) { slot = l[5] - '0'; if (slot < 0 || slot >= ST_SLOTS) slot = 0; }
    else if (l[0] >= 'A' && l[0] < 'A' + ST_SLOTS && l[1] == ' ')
      slotOk[l[0] - 'A'] = lerPar(l + 2, &slots[l[0] - 'A']);
  }
  fclose(f);
  limitar(&par);
  for (int i = 0; i < ST_SLOTS; i++) if (slotOk[i]) limitar(&slots[i]);
  { char b[96]; linhaPar(b, sizeof b, &par);
    printf("[seek] tuning loaded: %s log=%d\n", b, logRede); fflush(stdout); }
}

static void salvar(void) {
  char cam[600], b[96];
  FILE *f;
  if (!dados_caminho(cam, sizeof cam, "seektune.txt") || !(f = fopen(cam, "w"))) return;
  linhaPar(b, sizeof b, &par); fprintf(f, "cur %s\nlog %d\nslot %d\n", b, logRede, slot);
  for (int i = 0; i < ST_SLOTS; i++)
    if (slotOk[i]) { linhaPar(b, sizeof b, &slots[i]); fprintf(f, "%c %s\n", 'A' + i, b); }
  fclose(f);
}

static void registrar(const char *o_que) {
  char b[96];
  linhaPar(b, sizeof b, &par);
  printf("[seek] tuning %s: tap=%d growth=%d throttle=%d holdGap=%d cap=%d flip=%d commit=%d queue=%d  (%s)\n",
         o_que, par.tapS, par.growthPct, par.throttleMs, par.holdGapMs, par.capPct, par.flipPct,
         par.commitMs, par.fila, b);
  fflush(stdout);
}

const SeekParams *seektune_params(void) { carregar(); return &par; }
int seektune_log_rede(void) { carregar(); return logRede; }
int seektune_aberto(void) { return aberto; }
void seektune_abrir(void) { carregar(); aberto = 1; registrar("opened"); }
void seektune_fechar(void) {
  if (!aberto) return;
  aberto = 0;
  salvar();
  registrar("closed");
}

static void avisar(const char *s) {
  snprintf(aviso, sizeof aviso, "%s", s);
  avisoAte = SDL_GetTicks() + 1800u;
}

static void mudar(int d, int rep) {
  int k = rep ? 2 : 1;
  switch (linha) {
    case L_TAP:      par.tapS += d * k; break;
    case L_CRESCE:   par.growthPct += d * k; break;
    case L_THROTTLE: par.throttleMs += d * 10 * k; break;
    case L_SEGURA:   par.holdGapMs += d * 10 * k; break;
    case L_TETO:     par.capPct += d * k; break;
    case L_VIRA:     par.flipPct += d * 5; break;
    case L_COMMIT:
      if (par.commitMs == 0 && d > 0) par.commitMs = 100;
      else if (par.commitMs <= 100 && d < 0) par.commitMs = 0;
      else par.commitMs += d * 20 * k;
      break;
    case L_FILA:     if (!rep) par.fila = !par.fila; break;
    case L_LOG:      if (!rep) { logRede = !logRede; salvar(); } return;
    case L_PRESET:   if (!rep) slot = (slot + ST_SLOTS + d) % ST_SLOTS; return;
    default: return;
  }
  limitar(&par);
  registrar("changed");
}

void seektune_tecla(SDL_Keycode k, int repeticao) {
  carregar();
  if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE || k == SDLK_DELETE) {
    if (!repeticao) seektune_fechar();
    return;
  }
  if (k == SDLK_UP) { if (linha > 0) linha--; return; }
  if (k == SDLK_DOWN) { if (linha < L_N - 1) linha++; return; }
  if (k == SDLK_LEFT) { mudar(-1, repeticao); return; }
  if (k == SDLK_RIGHT) { mudar(1, repeticao); return; }
  if ((k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && !repeticao) {
    char b[48];
    if (linha == L_PRESET) {
      if (slotOk[slot]) {
        par = slots[slot]; limitar(&par); salvar();
        snprintf(b, sizeof b, "Preset %c loaded", 'A' + slot); avisar(b); registrar(b);
      } else { snprintf(b, sizeof b, "Preset %c is empty", 'A' + slot); avisar(b); }
    } else if (linha == L_SALVAR) {
      slots[slot] = par; slotOk[slot] = 1; salvar();
      snprintf(b, sizeof b, "Saved to preset %c", 'A' + slot); avisar(b); registrar(b);
    } else if (linha == L_PADRAO) {
      SeekParams d = SEEKG_PADRAO;
      par = d; salvar(); avisar("Defaults restored"); registrar("reset");
    } else if (linha == L_FILA || linha == L_LOG) mudar(1, 0);
    else seektune_fechar();
  }
}

static void valor(int i, char *b, size_t n) {
  switch (i) {
    case L_TAP:      snprintf(b, n, "%d s", par.tapS); break;
    case L_CRESCE:   snprintf(b, n, "+%d%%  (x%.2f)", par.growthPct, 1.0 + par.growthPct / 100.0); break;
    case L_THROTTLE: snprintf(b, n, "%d ms", par.throttleMs); break;
    case L_SEGURA:   snprintf(b, n, "%d ms", par.holdGapMs); break;
    case L_TETO:     snprintf(b, n, "%d%% of length", par.capPct); break;
    case L_VIRA:     snprintf(b, n, "x%.2f", par.flipPct / 100.0); break;
    case L_COMMIT:   if (par.commitMs) snprintf(b, n, "%d ms", par.commitMs); else snprintf(b, n, "Auto"); break;
    case L_FILA:     snprintf(b, n, "%s", par.fila ? "Queue after landing" : "Continue from target"); break;
    case L_LOG:      snprintf(b, n, "%s", logRede ? "On" : "Off"); break;
    case L_PRESET:   snprintf(b, n, "%c%s", 'A' + slot, slotOk[slot] ? "" : "  (empty)"); break;
    case L_SALVAR:   snprintf(b, n, "-> %c", 'A' + slot); break;
    default:         b[0] = 0; break;
  }
}

void seektune_desenhar(Uint32 agora, float a) {
  const float W = 700.0f, LH = 52.0f, PAD = 28.0f;
  float h, x, y, yy;
  char b[96];
  if (!aberto || a <= 0.01f) return;
  h = PAD + 44.0f + L_N * LH + 16.0f + 64.0f + PAD;
  x = NV_VTELA_W - 96.0f - W;
  y = (NV_VTELA_H - h) * 0.5f - 60.0f;
  if (y < 40.0f) y = 40.0f;
  plrui_material((GfxRect){ x, y, W, h }, 26.0f, 1, a);
  { TxtLinha t = txt_linha(TXT_ILHA_NOME, "Seek tuning", 243, 242, 239, 255);
    txt_desenhar_alpha(t, x + PAD, y + PAD, a);
    if (avisoAte > agora) {
      TxtLinha v = txt_linha(TXT_G19M, aviso, 120, 220, 150, 255);
      txt_desenhar_alpha(v, x + W - PAD - v.w, y + PAD + 4.0f, a);
    } }
  yy = y + PAD + 44.0f;
  for (int i = 0; i < L_N; i++, yy += LH) {
    GfxRect r = { x + 12.0f, yy, W - 24.0f, LH - 6.0f };
    int sel = i == linha, tinta = 243;
    TxtLinha ln, lv;
    if (sel) plrui_linha_foco(r, 18.0f, a);
    ln = txt_linha(TXT_G21B, NOMES[i], tinta, tinta, tinta, sel ? 255 : 200);
    txt_desenhar_alpha(ln, r.x + 16.0f, r.y + (r.h - ln.h) * 0.5f, a);
    valor(i, b, sizeof b);
    if (b[0]) {
      int ajusta = i < L_SALVAR;
      float xr = r.x + r.w - 16.0f - (sel && ajusta ? 22.0f : 0.0f);
      lv = txt_linha(TXT_G21B, b, 243, 242, 239, 255);
      txt_desenhar_alpha(lv, xr - lv.w, r.y + (r.h - lv.h) * 0.5f, a);
      if (sel && ajusta) {
        gfx_icone((GfxRect){ xr - lv.w - 26.0f, r.y + (r.h - 20.0f) * 0.5f, 20, 20 }, "pl_chevron-left", 1, 1, 1, a * 0.8f);
        gfx_icone((GfxRect){ xr + 4.0f, r.y + (r.h - 20.0f) * 0.5f, 20, 20 }, "pl_chevron-right", 1, 1, 1, a * 0.8f);
      }
    }
  }
  yy += 16.0f;
  { TxtLinha d = txt_linha_corta(TXT_G19M, DICAS[linha], 243, 242, 239, 170, W - PAD * 2.0f);
    txt_desenhar_alpha(d, x + PAD, yy, a); }
  if (linha == L_LOG && logRede && logrede_url()[0]) {
    TxtLinha u = txt_linha_corta(TXT_G19M, logrede_url(), 120, 200, 255, 255, W - PAD * 2.0f);
    txt_desenhar_alpha(u, x + PAD, yy + 28.0f, a);
  } else {
    TxtLinha d = txt_linha(TXT_G19M, "Up/Down choose  -  Left/Right change  -  Back close", 243, 242, 239, 120);
    txt_desenhar_alpha(d, x + PAD, yy + 28.0f, a);
  }
}
