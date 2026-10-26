// BLUR NA GPU NUM FIO PROPRIO (.tpk, dono 26/10/2026; teste "GPU blur
// experiment" = Own GPU thread). Ver agbfio.c.
//
// Nos logs de 25/10 o fio da UI ficava 1,1-2,1 s a cada 10 s so com o blur na
// GPU (envios, passadas, criar texturas: trabalho de CPU do driver), e a 100 %
// a cena pesada ficava atrasada na tela; os 4 nucleos da TV ficavam 35-45 %
// ocupados. Aqui o assgpublur inteiro roda num fio com um contexto GL
// compartilhado com o da UI: a UI so desenha as texturas prontas.
//
// Fio GL (a UI) chama tudo daqui; o assgpublur passa a ser do fio do blur
// (a UI nao chama mais nenhuma funcao dele enquanto este modo estiver ligado).
#ifndef NV_AGBFIO_H
#define NV_AGBFIO_H

#include "assgpublur.h"

// 1 = o fio esta rodando (cria na primeira chamada; 0 = nao deu, o motivo vai
// para o log uma vez e as chamadas seguintes devolvem 0 sem tentar de novo).
int  agbfio_ligar(void);
void agbfio_desligar(void);   // para o fio e solta tudo; o assgpublur volta para a UI
int  agbfio_ativo(void);
// Quadro da UI (idade para soltar texturas): uma vez por quadro com ASS.
void agbfio_quadro(unsigned quadro);
// Adianta o trabalho (quadros na fila do Render ahead). Copia os bitmaps.
void agbfio_pedir(const AgbPedido *p);
// Textura final pronta (espera ate esperaMs pelo fio). 0 = falhou ou nao deu
// tempo (quem chama cai na CPU). *tw, *th = tamanho da textura.
GLuint agbfio_obter(const AgbPedido *p, unsigned quadro, unsigned long long *chave, int *tw, int *th, int esperaMs);
void agbfio_usar(unsigned long long chave, unsigned quadro);
// Sem esperar: 1 = a textura deste pedido ja esta pronta (ou falhou; quem
// chama cai na CPU). Fora do modo fio, sempre 1.
int  agbfio_pronto(const AgbPedido *p);
// Seek: o trabalho na fila (quadros que nao vao mais aparecer) sai; as
// texturas prontas ficam (voltar ao mesmo trecho nao borra de novo).
void agbfio_descartar_fila(void);
void agbfio_apagar(void);     // troca de faixa
// O que o fio fez desde a ultima chamada (copia e zera).
void agbfio_colher(AgbStats *st, AgbPassos *ps, unsigned *esperas, double *esperaMs);

#endif
