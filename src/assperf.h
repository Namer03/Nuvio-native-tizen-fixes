// ASS (libass) SETTINGS FOR THE SAMSUNG .tpk (owner, 23-26/10/2026).
//
// The frame-by-frame anime signs (the phone scene of Kaguya-sama, a "Signs &
// Songs" track with 36,944 blocks) made the subtitles late and jerky on the
// S95C. Each fix was tested on the TV behind its own option; the winners are
// now fixed here (the TPK column of assperf.c) and the test panel is gone.
//
// What the person can still change lives in Subtitles > Style on an ASS/SSA
// track (faixas.c): size and opacity of the dialogue, contrast and gamma to
// match the TV, render resolution and libass itself (on/off). They are kept in
// assperf.txt (data), on this TV only.
//
// ONLY THE .tpk CHANGES. assrender.c is shared with webOS, the .wgt and
// Android: there assperf_get returns 0 for every option, the behaviour from
// before 23/10.
//
// BLUR: libass's own, on the CPU (NEON, 3 threads). A GPU blur was tried on
// the S95C (24-27/10) and removed: CPU only was faster and lighter there.
#ifndef NV_ASSPERF_H
#define NV_ASSPERF_H

#include <stddef.h>

enum {
  // Fixed on the .tpk (0 elsewhere = before 23/10):
  AP_MOTOR,        // libass bitmap engine: 0 plain C, 1 NEON (armv7 SIMD, bit-identical)
  AP_UPLOAD,       // 0 RGBA, 1 Alpha 8-bit + tint
  AP_TEXCACHE,     // 1 = reuse the texture of an unchanged image
  AP_TEXANEL,      // texture recycle delay: 0 = 1 frame, 1 = 2 frames
  AP_FORATRAVA,    // 1 = upload outside the queue lock
  AP_INCREMENTAL,  // 1 = new lines only per batch (0 = full reparse)
  AP_PODA,         // drop past events: 0 off, 2 = 2 min
  AP_ADIANTE,      // render ahead: 0 off, 3 = 100 ms
  AP_SEGURAR,      // 1 = an early frame waits for its moment
  AP_FONTES,       // attached fonts limit: 0 16 MB, 3 64 MB
  AP_CUES,         // 1 = the app's own cue copy every 10 s while libass draws
  AP_RELAY,        // 1 = subtitle blocks from the local relay stream
  AP_MUDANCA,      // 1 = render only when the picture can change
  AP_ESTILO,       // 1 = styled ASS drawn as authored (the person's colour, border... do not apply)
  AP_FIOS,         // NEON blur threads - 1 (2 = 3 threads)
  AP_PARQUE,       // relay blocks beyond the harvest window: 0 thrown away, 1 kept up to 6 MB
  AP_LEVE3,        // normal scenes: 1 = 3 frames ahead while frames cost > 40 ms
  // The person's (Subtitles > Style on ASS/SSA, .tpk):
  AP_ESCALA,       // render resolution: 0 100%, 2 75%, 5 Auto (100%, 75% while busy)
  AP_GAMA,         // gamma like the TV's slider: 0 off, 1..5 = -1..-5, 6..10 = +5..+1
  AP_CONTRASTE,    // contrast like the TV's: 0 = 30 (TV default) .. 30 = 0
  AP_LIBASS,       // 1 = libass draws ASS/SSA; 0 = dialogue only, as plain text
  AP_ASSTAM,       // dialogue size, %: 50..200 (100)
  AP_ASSOPAC,      // dialogue opacity, %: 20..100 (100)
  AP_N
};

// Current value (cheap). Outside the .tpk: always 0.
int  assperf_get(int id);
// Reads assperf.txt once (later calls do nothing).
void assperf_iniciar(void);
// Saves one of the person's options (AP_ESCALA .. AP_ASSOPAC).
void assperf_definir(int id, int v);
// The person's options back to their defaults.
void assperf_restaurar(void);
// Changes with every assperf_definir (the worker re-applies).
unsigned assperf_versao(void);

// Derived values.
size_t assperf_fontes_teto(void);            // bytes
int    assperf_fios(void);                   // 1..4
int    assperf_adiante_fixo_ms(void);        // 0 = off
long   assperf_poda_ms(void);                // 0 = off
double assperf_escala(void);                 // render resolution now (Auto: the one in use)
// Subtitle colour like the video's (AP_GAMA, AP_CONTRASTE): the TV's picture
// processing only touches the video plane.
double assperf_gama(void);                   // exponent: 1.0 = off
double assperf_contraste(void);              // gain: 1.0 (30) .. 0.70 (0)

// Render resolution Auto and the busy-scene flag (picture sync in legimagem.c).
double assperf_escala_quadro(void);          // worker: the scale for the next render
void   assperf_escala_reset(void);           // track change: back to 100 %
void   assperf_escala_custo(double libassMs);   // worker: a render that changed the picture
void   assperf_carga_gl(double ms);          // GL thread: cost of a new frame (0 = playing tick)
// Worker: lines starting in the next second, their text size and the pictures
// in this frame (busy-scene forecast); and the same for the second starting
// 0.5 s ahead (render resolution Auto only, so it drops to 75 % just before).
void   assperf_previsao(int inicios, long chars, int imagens, int iniciosCedo, long charsCedo);
int    assperf_pesado(void);                 // 1 = busy scene now
int    assperf_legendas_pesadas(void);       // busy or Auto at 75 % (others yield to libass)

#ifdef NV_ASSPERF_TESTE
void assperf_teste_definir(int id, int v);   // tests on the computer
#endif

#endif
