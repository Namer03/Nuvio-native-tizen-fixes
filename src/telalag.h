// SCREEN LAG OF THE APP'S OWN LAYER (.tpk, owner 25/10/2026).
//
// The subtitle correction (legimagem.h) is set on light scenes, ~200 ms on the
// S95C. On heavy signs the owner needed only 50-70 ms at 100 % and 150-160 ms
// at 75 % (log 4, 25/10): the GPU runs 2-3 frames behind, so a frame built at
// time T reaches the screen ~100-150 ms later, and the subtitles were already
// that late on their own. "shown late" never saw it: it ends where the picture
// is handed to the GPU.
//
// Here the GPU itself says when each frame finished: a fence after the frame's
// last draw, polled (never waited on) at the next frames. Lag = from the start
// of the frame (where the subtitle time is chosen) to the GPU being done. The
// extra over the light-scene baseline comes off the correction while playing.
//
// "GPU queue limit" (AP_FILA) is the other answer: wait at the start of a frame
// until the GPU is at most N frames behind. Less lag, fewer frames per second.
//
// Needs an OpenGL ES 3 context (fences are core there). Elsewhere, and off the
// .tpk, every function is a no-op and the extra is 0.
#ifndef NV_TELALAG_H
#define NV_TELALAG_H

#include <stddef.h>

void telalag_quadro_inicio(void);   // GL thread, before the frame's first draw
void telalag_quadro_fim(void);      // GL thread, right before the swap
// ms to take off the subtitle correction now (0 when light, off or unknown).
// Called every frame while subtitles are timed; 2 s without a call starts over.
int  telalag_extra_ms(void);
int  telalag_atual_ms(void);        // the last value telalag_extra_ms gave (read only)
// " | screen lag ..." for the 10 s [relogio] line; resets the window.
void telalag_relatorio(char *dst, size_t n);

#endif
