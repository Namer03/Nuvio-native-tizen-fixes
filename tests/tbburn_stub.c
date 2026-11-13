// TorBox burn-in (tbburn.h) for the fake-host tests that link src/video_tpk.c
// without the network: burn-in is never possible, so video_tpk.c behaves as
// before it existed.
#include "tbburn.h"
#include <stdint.h>
void tbburn_fonte(const char *u, const char *h, const char *a, uint64_t b) { (void)u; (void)h; (void)a; (void)b; }
void tbburn_ids(const char *u, int s, int i, int f) { (void)u; (void)s; (void)i; (void)f; }
int  tbburn_pode(void) { return 0; }
void tbburn_arquivo_tem_ass(int s) { (void)s; }
void tbburn_pedir(const char *u, int s, int a) { (void)u; (void)s; (void)a; }
int  tbburn_resultado(char *m, unsigned n, char *mo, unsigned mn) {
  if (m && n) m[0] = 0;
  if (mo && mn) mo[0] = 0;
  return TBB_NADA;
}
void tbburn_cancelar(void) {}
int  rede_torbox_em_voo(int *p) { if (p) *p = 0; return 0; }
int  ajustes_leg_queimar(void) { return 0; }
void tbburn_definir_escolha(int v) { (void)v; }
int  tbburn_memoria(TbFaixa *m) { (void)m; return 0; }
void tbburn_lembrar(const TbFaixa *f) { (void)f; }
int  tbburn_escolher_auto(const TbFaixa *f, int n, int mem, const TbFaixa *m, const char *p) {
  (void)f; (void)n; (void)mem; (void)m; (void)p; return -1;
}
// Weak: some of these tests link the real ones.
__attribute__((weak)) void mkvass_teto_conexoes(int n) { (void)n; }
__attribute__((weak)) const char *ling_legenda(void) { return ""; }
// HDR rule and original audio with burn-in (owner 13/11): never active here.
void tbburn_hdr_fonte(int s) { (void)s; }
int  tbburn_hdr_da_fonte(void) { return 0; }
int  tbburn_hdr_permitido(void) { return 0; }
void tbburn_hdr_lembrar(int s) { (void)s; }
__attribute__((weak)) int  tbburn_linha_visivel(void) { return 0; }
__attribute__((weak)) int  ajustes_tb_audio_remux(void) { return 0; }
__attribute__((weak)) void ajustes_definir_tb_audio_remux(int r) { (void)r; }
__attribute__((weak)) int  legref_em_rede(void) { return 0; }
#include "dts/tbaudio_tpk.h"
__attribute__((weak)) int  tba_disponivel(void) { return 0; }
__attribute__((weak)) int  tba_abrir(const char *t, const char *o, const char *c, int a, int m, int f, char *out, size_t n) {
  (void)t; (void)o; (void)c; (void)a; (void)m; (void)f; if (out && n) out[0] = 0; return 0;
}
__attribute__((weak)) void tba_fechar(void) {}
__attribute__((weak)) int  tba_modo(void) { return -1; }
__attribute__((weak)) int  tba_falhou(char *m, size_t n) { if (m && n) m[0] = 0; return 0; }
__attribute__((weak)) int  tba_n_rendicoes(void) { return 0; }
__attribute__((weak)) int  tba_rendicao_de(int o) { (void)o; return -1; }
__attribute__((weak)) void tba_definir_audio(int o) { (void)o; }
__attribute__((weak)) int  tba_audio(void) { return -1; }
__attribute__((weak)) void tba_definir_flac(int c) { (void)c; }
__attribute__((weak)) int  tba_flac_converter(void) { return 0; }
__attribute__((weak)) int  tba_audio_e_flac(void) { return 0; }
__attribute__((weak)) void tba_log(void) {}
__attribute__((weak)) void tba_sockets_vigiar(const char *a, const char *b, const char *c, const char *d) { (void)a; (void)b; (void)c; (void)d; }
__attribute__((weak)) int  tba_sockets_contar(int *n) { if (n) *n = -1; return -1; }
