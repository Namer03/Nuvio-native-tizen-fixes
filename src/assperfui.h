// A folha de teste "ASS performance" no player (.tpk). Ver assperf.h.
#ifndef NV_ASSPERFUI_H
#define NV_ASSPERFUI_H
#include <SDL2/SDL.h>
#include "gfx.h"

int   assperfui_aberto(void);
void  assperfui_abrir(void);
void  assperfui_fechar(void);
// Tecla com a folha aberta. 0 = Voltar fechou a folha (quem chama volta ao Estilo).
int   assperfui_tecla(SDL_Keycode k);
float assperfui_altura(void);
// Corpo dentro da ilha do player (a mesma de Legendas > Estilo).
void  assperfui_corpo(GfxRect c, float a);

#endif
