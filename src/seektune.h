// SEEK TUNING PANEL (owner, 20/10/2026) — a TEMPORARY tool for the .tpk.
//
// Opened from the "Seek tuning" button in the player's transport row. Every
// number of the seek gesture (seekgesto.h) changes live, sets can be saved to
// presets A/B/C, and the network log (logrede.h, http://<tv-ip>:8099/log) can
// be switched on from here. Values live in <data>/seektune.txt.
//
// Once the right values are found they become the SEEKG_PADRAO defaults and
// this panel, its button and the [seek] log lines are removed.
#ifndef NV_SEEKTUNE_H
#define NV_SEEKTUNE_H
#include <SDL2/SDL.h>
#include "seekgesto.h"

// The live parameters (never NULL). Loaded from disk on first use.
const SeekParams *seektune_params(void);
// Network log switch (stored with the parameters).
int seektune_log_rede(void);

int  seektune_aberto(void);
void seektune_abrir(void);
void seektune_fechar(void);
// Key while the panel is open. Always consumes the key.
void seektune_tecla(SDL_Keycode k, int repeticao);
// Draws on the player's virtual screen (caller does ESCALA_INI/FIM).
void seektune_desenhar(Uint32 agora, float a);

#endif
