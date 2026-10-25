// SCREEN LAG OF THE APP'S OWN LAYER AND THE GPU QUEUE LIMIT (.tpk, owner 25/10/2026).
//
// A GL fence after each frame's last draw, polled (never waited on) at the
// next frames: lag = from the start of the frame (where the subtitle time is
// chosen) to the GPU being done. It is only reported in the 10 s [relogio]
// line. (Taking it off the subtitle correction was tried on 25/10 and did
// nothing: the lag stays ~17 ms and only single frames spike.)
//
// The GPU queue limit waits at the start of a frame until the GPU is at most
// one frame behind, but only where it helped on the S95C: Render resolution
// Auto at 75 % on heavy signs (telalag.c, limite).
//
// Needs an OpenGL ES 3 context (fences are core there). Elsewhere, and off the
// .tpk, every function is a no-op.
#ifndef NV_TELALAG_H
#define NV_TELALAG_H

#include <stddef.h>

void telalag_quadro_inicio(void);   // GL thread, before the frame's first draw
void telalag_quadro_fim(void);      // GL thread, right before the swap
// " | screen lag ..." for the 10 s [relogio] line; resets the window.
void telalag_relatorio(char *dst, size_t n);

#endif
