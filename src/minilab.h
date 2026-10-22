// THUMBNAIL LAB (.tpk test build): knobs for the seek-bar thumbnails made on
// the TV (miniatura.c / miniatura_motor.c) and a live readout, in a panel the
// player opens from the Control Center ("Thumbnail lab"). The values are kept
// on the TV (miniteste.txt) so a test survives a restart; "Reset" goes back to
// the defaults. Only the .tpk with FFmpeg has it (mini_lab_disponivel).
#ifndef NV_MINILAB_H
#define NV_MINILAB_H
#include <SDL2/SDL.h>

typedef struct {
  int pisoMB;          // MemAvailable always left free (default 150)
  int dtsExtraMB;      // added to the floor while the DTS converter runs (default 0)
  int margemPct;       // measured tile cost x this % (default 125)
  int estimativa4kMB;  // cost guessed for a 4K tile before any is measured (default 60)
  int medirApos;       // tiles measured before the measured cost is used (default 1)
  int threads;         // decoding threads 1..3 (default 1)
  int manterDecoder;   // keep the decoder open between tiles (default 0)
  int prefetch;        // fetch the next keyframe while decoding (default 1)
  int lerBlocos;       // 256 KiB blocks per request, 1..7 (default 4 = 1 MiB)
  int exato;           // request only what the demuxer asks for (default 0)
  int tsBlocos;        // M2TS/TS: 256 KiB blocks per request while reading forward, 1..7 (default 7)
  int tsSonda;         // M2TS/TS: probe small windows for the keyframe (default 0)
  int tsJanelaKB;      // probe window (default 64)
  int tsPassoKB;       // distance between probe windows (default 512)
  int tsSondasMax;     // windows per try (default 12)
  int pausaMs;         // memory re-check while paused (default 1500)
  int intervaloMs;     // pause between tiles (default 30)
  int parado;          // stop making tiles (default 0)
} MiniAjustes;

typedef struct {
  char estado[96];     // what the worker does now
  char arquivo[64];    // "matroska hevc 3840x2160 HDR"
  int feitos, n, feitosSessao;
  unsigned ultimoMs, ultimoDecodMs, mediaMs;
  long ultimoKB, adiantadoKB;
  int ultimoPedidos, ultimasSondas, achouSonda;
  long ultimoCustoMB, custoUsadoMB, livreMB, pisoEfetivoMB, menorLivreMB;
  long lidosMB;
  int pausasMem;
} MiniStats;

int  mini_lab_disponivel(void);
MiniAjustes *mini_ajustes(void);       // live: the worker re-reads it before every tile
void mini_ajustes_padrao(MiniAjustes *a);
void mini_ajustes_gravar(void);
void mini_stats(MiniStats *s);         // a copy, taken under the worker's lock

// The panel (minilab.c).
void minilab_abrir(void);
void minilab_fechar(void);
int  minilab_aberto(void);
int  minilab_evento(const SDL_Event *e);   // 1 = consumed (modal while open)
void minilab_desenhar(Uint32 agora);
#endif
