// "TV luminance limit (test)" e "TV video gray (test)" da folha ASS performance
// (.tpk). Ver tpkluz.c.
#ifndef NV_TPKLUZ_H
#define NV_TPKLUZ_H

// Uma vez, no arranque (nv_tpk_iniciar): `dados` e a pasta data/ do pacote.
// Sobe o fio que fala com a TV e, se a sessao anterior caiu com o valor da TV
// mudado, poe o original de volta.
void tpkluz_iniciar(const char *dados);
// Fio do app, todo quadro: 1 = o player esta em tela cheia com video.
void tpkluz_quadro(int telaCheia);
// Pausa / saida (0) e retomada (1) do app. O 0 espera (ate ~3 s) o valor
// original voltar a TV antes de retornar.
void tpkluz_sistema(int ativo);

// "Subtitle gamma" = Auto: o slider de gama da TV (-5..+5) lido por
// avoc_get_gamma no fio daqui, ou TPKLUZ_GAMA_DESCONHECIDO.
#define TPKLUZ_GAMA_DESCONHECIDO 0x7fff
int tpkluz_gama_tv(void);

#endif
