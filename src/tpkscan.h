// "Scan display APIs" da folha ASS performance
// (.tpk). Ver tpkscan.c.
#ifndef NV_TPKSCAN_H
#define NV_TPKSCAN_H

// Fio GL, a cada quadro com a folha aberta: dispara o scan pedido na folha
// (so leitura).
void tpkscan_tick(void);
// Pagina do log (/ass-lib/): bibliotecas de imagem do sistema e quem as chama,
// para baixar e estudar as assinaturas. 1 = `nome` pode sair (caminho em c).
#include <stddef.h>
int    tpkscan_lib_caminho(const char *nome, char *c, size_t n);
size_t tpkscan_lib_lista(char *dst, size_t n);
// Pagina /ass-bin: nomes de TODOS os executaveis em /usr/bin, /usr/sbin, /bin
// (so nomes + tamanho) para achar o nome do daemon avoc. Download continua
// restrito a nomes com "avoc" em tpkscan_lib_caminho.
size_t tpkscan_bin_lista(char *dst, size_t n);
// Pagina /ass-cfg: arquivos de config / featureconf (featureconf,
// dynamic_backlight, ...), so leitura, confinado a raizes fixas.
size_t tpkscan_cfg_lista(char *dst, size_t n);
int    tpkscan_cfg_caminho(const char *rel, char *c, size_t n);

#endif
