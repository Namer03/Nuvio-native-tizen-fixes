// "Scan display APIs" e "Window brightness (test)" da folha ASS performance
// (.tpk). Ver tpkscan.c.
#ifndef NV_TPKSCAN_H
#define NV_TPKSCAN_H

// Fio GL, a cada quadro com a folha aberta: dispara o scan pedido na folha e
// aplica o pedido de brilho da janela quando a chave muda.
void tpkscan_tick(void);

#endif
