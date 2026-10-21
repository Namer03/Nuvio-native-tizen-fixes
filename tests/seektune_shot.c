// Capture of the .tpk seek tuning panel (seektune.h) over the player OSD.
// Out of the suite: GL window and a human eye. bash tests/seektune_shot.sh <dir>
#include "../src/player.c"
// Linux (Mesa): the app's shaders are GLES2; ask for an ES 2.0 context.
static int shotAttr(SDL_GLattr a, int v) {
  if (a == SDL_GL_CONTEXT_MAJOR_VERSION) SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  if (a == SDL_GL_CONTEXT_MINOR_VERSION) v = 0;
  return SDL_GL_SetAttribute(a, v);
}
#define SDL_GL_SetAttribute shotAttr
#define main iniciarFixtureGlass
#include "player_glass_shot.c"
#undef main
#undef SDL_GL_SetAttribute
#include "../src/seektune.h"

// ES only guarantees RGBA readback.
static void salvarEs(const char *id) {
  char nome[800];
  SDL_Surface *sf = SDL_CreateRGBSurface(0, LW, LH, 32, 0xff, 0xff00, 0xff0000, 0xff000000);
  unsigned char *p = sf->pixels, *t = malloc((size_t)sf->pitch);
  glFinish(); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glReadPixels(0, 0, LW, LH, GL_RGBA, GL_UNSIGNED_BYTE, sf->pixels);
  for (int y = 0; y < LH / 2; y++) {
    memcpy(t, p + y * sf->pitch, (size_t)sf->pitch);
    memcpy(p + y * sf->pitch, p + (LH - 1 - y) * sf->pitch, (size_t)sf->pitch);
    memcpy(p + (LH - 1 - y) * sf->pitch, t, (size_t)sf->pitch);
  }
  free(t);
  snprintf(nome, sizeof nome, "%s/%s.bmp", saida, id);
  assert(SDL_SaveBMP(sf, nome) == 0);
  SDL_FreeSurface(sf);
}
#define salvar salvarEs
static void painel(void) { ESCALA_INI(); seektune_desenhar(relogio, 1.0f); ESCALA_FIM(); }

int main(int argc, char **argv) {
  assert(argc == 2);
  char *init[] = {argv[0], argv[1], "seektune-init"};
  assert(iniciarFixtureGlass(3, init) == 0);
  abrir(&filme); simular(3840, 1606, "", 0, 0);
  player_shot_estado(relogio, 4360, 9420, 1, 0, 0, 0);
  seektune_abrir();
  extra = painel;
  quadros(60); salvar("seektune-top");
  for (int i = 0; i < 8; i++) seektune_tecla(SDLK_DOWN, 0);   // Network log
  seektune_tecla(SDLK_RETURN, 0);
  quadros(30); salvar("seektune-log");
  seektune_tecla(SDLK_DOWN, 0); seektune_tecla(SDLK_RIGHT, 0);   // Preset B
  quadros(30); salvar("seektune-preset");
  seektune_fechar();
  return 0;
}
