// SUBTITLE CORRECTION (.tpk, owner 25/10). The TV's picture runs a little
// behind the app's own layer while playing (its picture processing), so
// subtitles drawn on time looked early (170-210 ms on the S95C); paused, the frame stands still
// and there is no difference. The correction delays subtitles by that much,
// ONLY while playing. "Auto": one learned value per kind of video (frame rate,
// HDR) that the person nudges from the Subtitles sheet; starts at 190 ms.
#ifndef NV_LEGIMAGEM_H
#define NV_LEGIMAGEM_H

#include <stddef.h>

int  legimagem_disponivel(void);          // 1 on the .tpk
int  legimagem_ms(void);                  // correction for the video playing now (ms; heavy scenes have their own)
// Both values for this kind of video and whether a heavy scene is on screen now.
void legimagem_valores(int *leve, int *pesada, int *agoraPesada);
void legimagem_ajustar(int deltaMs);      // nudge (+ = subtitles later), saved for this kind of video and scene (light/heavy)
// "24 fps", "24 fps HDR", "video" when unknown.
void legimagem_tipo(char *dst, size_t n);
double legimagem_fps(void);               // frame rate of the video playing now (0 = unknown)

#endif
