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
} AgbPedido;

typedef struct {
  unsigned blurs;        // gaussianos novos (fontes que nao estavam no cache)
  unsigned finais;       // texturas finais novas
  unsigned reusos;       // texturas finais achadas no cache
  unsigned falhas;       // pedidos que a GPU nao fez (o chamador cai na CPU)
  double ms;             // tempo dos lotes com trabalho novo
  int medido;            // 1 = ms com glFinish (tempo real da GPU); 0 = so o envio
} AgbStats;

// Fio GL. 1 = shaders e alvos prontos (uma vez; depois so devolve o resultado).
int  assgpublur_iniciar(void);
const char *assgpublur_formato(void);   // formato intermediario em uso ("R16F", ...)
// Lote: salva/restaura o estado GL do app em volta dos pedidos de um quadro.
// medir = glFinish no comeco e no fim (tempo real; so com os cronometros).
void assgpublur_lote_ini(int medir);
void assgpublur_lote_fim(void);
// Textura final (RGBA8, w x h, alfa = o valor) do pedido; 0 = falhou.
// `quadro` = contador de quadros GL (o cache nunca recicla o que esta em uso).
GLuint assgpublur_obter(const AgbPedido *p, unsigned quadro, unsigned long long *chave);
// Quadro repetido: mantem viva a textura de `chave`.
void assgpublur_usar(unsigned long long chave, unsigned quadro);
// Le o alfa de uma textura final (snapshot). 1 = ok.
int  assgpublur_ler(GLuint tex, int w, int h, unsigned char *alfa);
void assgpublur_stats(AgbStats *out);   // copia e zera
void assgpublur_apagar(void);           // solta tudo da GPU (troca de faixa)
// So para testes: forca o formato intermediario (0 auto, 1 RGBA8).
void assgpublur_forcar_rgba8(int on);

#endif
