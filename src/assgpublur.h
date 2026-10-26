// BLUR DAS LEGENDAS ASS NA GPU (.tpk, dono 24/10/2026). Ver assgpublur.c.
//
// O libass (patch 0005, "deferred blur") entrega o desenho NITIDO de cada
// letreiro com blur e a receita do que faria depois: o blur gaussiano, o
// "fix" do contorno (o > g ? o - g/2 : 0) e o deslocamento de subpixel da
// sombra. Aqui isso roda no fio GL, em shaders, e sai uma textura RGBA do
// tamanho da imagem do libass, desenhada como as outras (GFX_MARCA).
#ifndef NV_ASSGPUBLUR_H
#define NV_ASSGPUBLUR_H

#include "gl_compat.h"

typedef struct {
  const unsigned char *src;          // o bitmap nitido inteiro (compacto: stride = srcW)
  int srcW, srcH;
  unsigned long long srcChave;       // conteudo de src (nunca 0)
  const unsigned char *fill;         // so com fix: o preenchimento nitido (compacto)
  int fillW, fillH;
  unsigned long long fillChave;
  double r2x, r2y;                   // variancia do gaussiano (px^2)
  int subX, subY, w, h;              // a imagem dentro de src
  int fix, fillDx, fillDy;           // fill em (x + fillDx, y + fillDy) de src
  int shiftX, shiftY;                // 1/64 px, depois do fix
  int soCache;                       // 1 = so devolve o que ja esta pronto (nao borra)
} AgbPedido;

// Fio GL. 1 = shaders e alvos prontos (uma vez; depois so devolve o resultado).
int  assgpublur_iniciar(void);
// Lote: salva/restaura o estado GL do app em volta dos pedidos de um quadro.
void assgpublur_lote_ini(void);
void assgpublur_lote_fim(void);
// Textura final (RGBA8, w x h, alfa = o valor) do pedido; 0 = falhou.
// `quadro` = contador de quadros GL (o cache nunca recicla o que esta em uso).
GLuint assgpublur_obter(const AgbPedido *p, unsigned quadro, unsigned long long *chave);
// Quadro repetido: mantem viva a textura de `chave`.
void assgpublur_usar(unsigned long long chave, unsigned quadro);
void assgpublur_apagar(void);           // solta tudo da GPU (troca de faixa, regra da RAM)
// Tamanho da TEXTURA devolvida pelo ultimo assgpublur_obter (>= a imagem: classe
// de tamanho, a sobra transparente). Desenhe a textura inteira com este tamanho.
void assgpublur_ultimo_tam(int *w, int *h);
// Chamar por quadro: depois de ~5 s sem blur novo solta caches e reserva.
void assgpublur_aparar(unsigned quadro);

#endif
