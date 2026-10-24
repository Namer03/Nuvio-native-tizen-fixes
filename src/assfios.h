// QUEM ESPERA PELA CPU (.tpk, Linux; dono 25/10/2026). Ver assfios.c.
//
// Na cena pesada da Kaguya o quadro da UI levava 100-215 ms na fase de
// desenho com ~45 retangulos e, com o blur na CPU, so 35-57 ms dentro do
// codigo das legendas. O resto e espera: ou o fio da UI perde o nucleo para o
// libass e os fios do blur, ou fica bloqueado (driver, trava). O
// /proc/.../schedstat do Linux separa os dois: tempo num nucleo e tempo na
// fila esperando um nucleo livre.
//
// Fora do .tpk (e sem /proc) tudo vira nada: as funcoes existem e nao medem.
#ifndef NV_ASSFIOS_H
#define NV_ASSFIOS_H

// Nome do fio que chama (Linux comm, ate 15 letras). Fios que ele criar depois
// herdam o nome: e assim que os fios do blur do libass ficam identificaveis.
void assfios_nomear(const char *nome);
// Troca o nome so durante uma chamada: `antigo` (16 bytes) guarda o atual.
void assfios_nome_trocar(const char *nome, char *antigo);
void assfios_nome_voltar(const char *antigo);

// Fio da UI, uma vez por quadro: tempo num nucleo e tempo esperando um nucleo
// desde a chamada anterior (ms). A primeira chamada marca o fio como "ui".
// *esperaMs = -1 quando o kernel nao tem schedstat (so a CPU, pelo relogio do fio).
void assfios_ui_quadro(double *cpuMs, double *esperaMs);

// "[assperf] threads" (janela de 10 s): CPU e espera por grupo de fios
// (ui, nv-assrender, nv-assblur, os demais pelo nome) e a carga de cada nucleo.
void assfios_relatorio(void);

#endif
