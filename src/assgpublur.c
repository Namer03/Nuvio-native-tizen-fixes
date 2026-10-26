// BLUR DAS LEGENDAS ASS NA GPU (.tpk, dono 24/10/2026). Ver assgpublur.h.
//
// Por que: na S95C o blur do libass e ~2/3 do tempo de um quadro de letreiro
// pesado (bench da cena da caixa de entrada de Kaguya-sama: 28 de 43 ms), e e
// limitado pela memoria: mais fios de CPU quase nao ajudam (1 fio 51 ms,
// 3 fios 43). A Mali-G51 fica parada (o video esta no plano proprio).
//
// O que roda aqui, por pedido (uma imagem do libass com a receita do 0005):
//  1. o bitmap nitido sobe uma vez (GL_LUMINANCE) e e reduzido pela metade
//     k vezes em x e em y (filtro [1 3 3 1]/8, 2 leituras bilineares), como os
//     "levels" do blur do libass: o gaussiano grande vira um pequeno;
//  2. gaussiano separavel no nivel reduzido (pares de leituras bilineares),
//     num alvo de meia precisao quando a GPU tem (R16F), senao RGBA8;
//  3. passada final no tamanho da imagem: amplia (bilinear), faz o fix do
//     contorno contra o preenchimento nitido e o deslocamento de subpixel da
//     sombra, e grava RGBA8 (alfa = o valor) — desenhada como as outras.
// A variancia de cada etapa entra na conta (reducao 0,75 px^2 por nivel,
// ampliacao ~h^2/6), para o total bater com o sigma^2 que o libass pediu.
// Dois caches pelo CONTEUDO: o borrado (fonte + sigma) e a textura final
// (borrado + recorte + fix + deslocamento). Letreiro parado borra uma vez.
//
// O estado GL do app (FBO, viewport, programa, texturas, VBO, mistura,
// tesoura, dither) e salvo no comeco do lote e devolvido no fim.
#if defined(NV_TPK) || defined(NV_AGB_TESTE)
#include "assgpublur.h"
#ifndef NV_AGB_TESTE
#include "gfx.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GL_RED
#define GL_RED 0x1903
#endif
#ifndef GL_R16F
#define GL_R16F 0x822D
#endif
#ifndef GL_HALF_FLOAT
#define GL_HALF_FLOAT 0x140B
#endif
#ifndef GL_HALF_FLOAT_OES
#define GL_HALF_FLOAT_OES 0x8D61
#endif
#ifndef GL_MAX_TEXTURE_SIZE
#define GL_MAX_TEXTURE_SIZE 0x0D33
#endif

#define AGB_PARES_MAX 8              // pares de leituras do gaussiano (ate sigma ~4,5 no nivel)
#define AGB_NIVEL_MAX 7
/* 25/10 (owner: memory matters): 24 + 24 MB, and the reserve 16 MB; all of it
 * is trimmed after a few seconds without blur work (assgpublur_aparar). */
#define AGB_BLUR_BYTES (16u * 1024u * 1024u)    // 26/10: 24 -> 16 (memory; the free RAM still dips to 175-190 MB)
#define AGB_FINAL_BYTES (16u * 1024u * 1024u)

// ---- formato intermediario ------------------------------------------------------
typedef struct { GLint interno; GLenum fmt, tipo; int bpp; const char *nome; } AgbFmt;
static AgbFmt fmtMeio;
static int pronto = -1;      // -1 nao tentado, 0 falhou, 1 ok
static GLint maxTex = 2048;

// ---- programas ------------------------------------------------------------------
static GLuint progReduz, progBlur[AGB_PARES_MAX + 1], progFinal[AGB_PARES_MAX + 1];
static GLint rzT, rzEsc, rzOff, rzEixo;
static GLint blT[AGB_PARES_MAX + 1], blPasso[AGB_PARES_MAX + 1], blW0[AGB_PARES_MAX + 1],
             blW[AGB_PARES_MAX + 1], blO[AGB_PARES_MAX + 1];
typedef struct { GLint B, G, BEsc, Sub, Saida, Fonte, GOff, GTam, Fix, Desl, TemG, PassoY, Wy0, Wy, Oy; } AgbLocFinal;
static AgbLocFinal fn[AGB_PARES_MAX + 1];
static GLuint vbo, fbo;

static const char *VS =
  "attribute vec2 aPos;\n"
  "varying vec2 vUv;\n"
  "void main() { vUv = aPos; gl_Position = vec4(aPos * 2.0 - 1.0, 0.0, 1.0); }\n";

#define FS_PREC \
  "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n"

// Reducao pela metade num eixo: [1 3 3 1]/8 em volta de 2x+1 = duas leituras
// bilineares em 2x+0,25 e 2x+1,75 (pixels da entrada). uEsc = 2*Wsaida/Wentrada.
static const char *FS_REDUZ =
  FS_PREC
  "varying vec2 vUv;\n"
  "uniform sampler2D uT;\n"
  "uniform float uEsc, uOff;\n"
  "uniform vec2 uEixo;\n"   // (1,0) = x, (0,1) = y
  "void main() {\n"
  "  vec2 c = mix(vUv, vUv * uEsc, uEixo);\n"
  "  vec2 d = uEixo * uOff;\n"
  "  float v = 0.5 * (texture2D(uT, c - d).r + texture2D(uT, c + d).r);\n"
  "  gl_FragColor = vec4(v);\n"
  "}\n";

// Gaussiano separavel: centro + N pares (cada par = uma leitura bilinear de
// cada lado, peso e deslocamento combinados).
static const char *FS_BLUR_FMT =
  FS_PREC
  "#define N %d\n"
  "varying vec2 vUv;\n"
  "uniform sampler2D uT;\n"
  "uniform vec2 uPasso;\n"
  "uniform float uW0;\n"
  "uniform float uW[N];\n"
  "uniform float uO[N];\n"
  "void main() {\n"
  "  float v = texture2D(uT, vUv).r * uW0;\n"
  "  for (int i = 0; i < N; i++) {\n"
  "    vec2 o = uPasso * uO[i];\n"
  "    v += (texture2D(uT, vUv + o).r + texture2D(uT, vUv - o).r) * uW[i];\n"
  "  }\n"
  "  gl_FragColor = vec4(v);\n"
  "}\n";

// Passada final, em pixels do bitmap de origem (X inteiro):
//   F(X) = 0 fora de src; o = borrado(X) (0..255); com fix:
//   o > g ? o - floor(g/2) : 0, g = fill(X + uGOff) (0 fora do fill).
//   valor = deslocamento do libass entre F(X), F(X-1,y), F(x,y-1), F(X-1,y-1).
// NY > 0: o gaussiano vertical (sem reducao em y) e feito AQUI, nas linhas
// exatas da textura, em vez de numa passada propria (uma passada a menos por
// letreiro: na Mali cada passada custa caro, 24/10).
static const char *FS_FINAL_FMT =
  FS_PREC
  "#define NY %d\n"
  "varying vec2 vUv;\n"
  "uniform sampler2D uB, uG;\n"
  "uniform vec2 uBEsc;\n"     // 1 / (Wk * 2^kx), 1 / (Hk * 2^ky)
  "uniform vec2 uSub;\n"      // canto da imagem em src
  "uniform vec2 uSaida;\n"    // w, h da imagem
  "uniform vec2 uFonte;\n"    // srcW, srcH
  "uniform vec2 uGOff;\n"
  "uniform vec2 uGTam;\n"
  "uniform float uFix, uTemG;\n"
  "uniform vec2 uDesl;\n"     // shift / 64
  "#if NY > 0\n"
  "uniform float uPassoY, uWy0;\n"
  "uniform float uWy[NY];\n"
  "uniform float uOy[NY];\n"
  "#endif\n"
  "float amostra(vec2 c) {\n"
  "#if NY > 0\n"
  "  float v = texture2D(uB, c).r * uWy0;\n"
  "  for (int i = 0; i < NY; i++) {\n"
  "    vec2 o = vec2(0.0, uPassoY * uOy[i]);\n"
  "    v += (texture2D(uB, c + o).r + texture2D(uB, c - o).r) * uWy[i];\n"
  "  }\n"
  "  return v;\n"
  "#else\n"
  "  return texture2D(uB, c).r;\n"
  "#endif\n"
  "}\n"
  "float dentro(vec2 p, vec2 t) {\n"
  "  vec2 a = step(vec2(0.0), p) * step(p, t - vec2(1.0));\n"
  "  return a.x * a.y;\n"
  "}\n"
  "float F(vec2 X) {\n"
  "  float o = amostra((X + 0.5) * uBEsc) * 255.0;\n"
  "  if (uFix > 0.5) {\n"
  "    vec2 gp = X + uGOff;\n"
  "    float g = texture2D(uG, (gp + 0.5) / uGTam).r * 255.0;\n"
  "    g = floor(g + 0.5) * dentro(gp, uGTam) * uTemG;\n"
  "    o = floor(o + 0.5) > g ? o - floor(g * 0.5) : 0.0;\n"
  "  }\n"
  "  return o * dentro(X, uFonte);\n"
  "}\n"
  "void main() {\n"
  "  vec2 X = floor(vUv * uSaida) + uSub;\n"
  "  float v;\n"
  "  if (uDesl.x <= 0.0 && uDesl.y <= 0.0) v = F(X);\n"
  "  else if (uFix < 0.5) {\n"
  /* sem fix o valor e linear: o deslocamento do libass e UMA leitura
   * bilinear em X - desl (4x menos trabalho na sombra comum, 24/10) */
  "#if NY > 0\n"
  /* com o gaussiano vertical aqui, os pares de leitura so valem em linha
   * inteira: duas linhas, e o x fracionario numa leitura bilinear */
  "    float xs = X.x - uDesl.x;\n"
  "    float a = amostra((vec2(xs, X.y) + 0.5) * uBEsc), b = amostra((vec2(xs, X.y - 1.0) + 0.5) * uBEsc);\n"
  "    v = mix(a, b, uDesl.y) * 255.0 * dentro(X, uFonte);\n"
  "#else\n"
  "    v = amostra((X - uDesl + 0.5) * uBEsc) * 255.0 * dentro(X, uFonte);\n"
  "#endif\n"
  "  } else {\n"
  "    float a = F(X - vec2(1.0, 0.0)), b = F(X - vec2(0.0, 1.0)), c = F(X - vec2(1.0, 1.0));\n"
  "    v = mix(mix(F(X), a, uDesl.x), mix(b, c, uDesl.x), uDesl.y);\n"
  "  }\n"
  "  gl_FragColor = vec4(clamp(v / 255.0, 0.0, 1.0));\n"
  "}\n";

static GLuint compilar(GLenum tipo, const char *src) {
  GLuint s = glCreateShader(tipo);
  GLint ok = 0;
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512] = "";
    glGetShaderInfoLog(s, sizeof log, NULL, log);
    printf("[assperf] gpu blur: shader did not compile: %s\n", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

static GLuint ligar(const char *fs) {
  GLuint v = compilar(GL_VERTEX_SHADER, VS), f = compilar(GL_FRAGMENT_SHADER, fs), p;
  GLint ok = 0;
  if (!v || !f) { if (v) glDeleteShader(v); if (f) glDeleteShader(f); return 0; }
  p = glCreateProgram();
  glAttachShader(p, v); glAttachShader(p, f);
  glBindAttribLocation(p, 0, "aPos");
  glLinkProgram(p);
  glDeleteShader(v); glDeleteShader(f);
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[512] = "";
    glGetProgramInfoLog(p, sizeof log, NULL, log);
    printf("[assperf] gpu blur: program did not link: %s\n", log);
    glDeleteProgram(p);
    return 0;
  }
  return p;
}

static GLuint prog_blur(int n) {
  char fs[1400];
  GLuint p;
  if (progBlur[n]) return progBlur[n];
  snprintf(fs, sizeof fs, FS_BLUR_FMT, n);
  p = ligar(fs);
  if (!p) return 0;
  blT[n] = glGetUniformLocation(p, "uT"); blPasso[n] = glGetUniformLocation(p, "uPasso");
  blW0[n] = glGetUniformLocation(p, "uW0"); blW[n] = glGetUniformLocation(p, "uW");
  blO[n] = glGetUniformLocation(p, "uO");
  progBlur[n] = p;
  return p;
}

static GLuint prog_final(int n) {
  char fs[3200];
  GLuint p;
  AgbLocFinal *l = &fn[n];
  if (progFinal[n]) return progFinal[n];
  snprintf(fs, sizeof fs, FS_FINAL_FMT, n);
  p = ligar(fs);
  if (!p) return 0;
  l->B = glGetUniformLocation(p, "uB"); l->G = glGetUniformLocation(p, "uG");
  l->BEsc = glGetUniformLocation(p, "uBEsc"); l->Sub = glGetUniformLocation(p, "uSub");
  l->Saida = glGetUniformLocation(p, "uSaida"); l->Fonte = glGetUniformLocation(p, "uFonte");
  l->GOff = glGetUniformLocation(p, "uGOff"); l->GTam = glGetUniformLocation(p, "uGTam");
  l->Fix = glGetUniformLocation(p, "uFix"); l->Desl = glGetUniformLocation(p, "uDesl");
  l->TemG = glGetUniformLocation(p, "uTemG");
  l->PassoY = glGetUniformLocation(p, "uPassoY"); l->Wy0 = glGetUniformLocation(p, "uWy0");
  l->Wy = glGetUniformLocation(p, "uWy"); l->Oy = glGetUniformLocation(p, "uOy");
  progFinal[n] = p;
  return p;
}

static int tem_ext(const char *lista, const char *nome) {
  size_t n = strlen(nome);
  const char *p = lista;
  while (p && (p = strstr(p, nome)) != NULL) {
    if ((p == lista || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0)) return 1;
    p += n;
  }
  return 0;
}

// O formato da textura serve de alvo? (FBO completo e um clear que passa.)
static int alvo_ok(const AgbFmt *f) {
  GLuint t = 0;
  GLenum st;
  int ok;
  while (glGetError() != GL_NO_ERROR) {}
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexImage2D(GL_TEXTURE_2D, 0, f->interno, 8, 8, 0, f->fmt, f->tipo, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
  st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  ok = st == GL_FRAMEBUFFER_COMPLETE && glGetError() == GL_NO_ERROR;
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glDeleteTextures(1, &t);
  return ok;
}

// ---- estado do app (salvo no lote) -----------------------------------------------
static struct {
  GLint fbo, vp[4], prog, tex0, tex1, ativa, vbo, aSize, aType, aNorm, aStride, aOn, unpack;
  void *aPtr;
  GLboolean blend, tesoura, dither, mascara[4];
  GLfloat limpa[4];      // glClearColor do app (a passada final limpa a textura)
  int aberto, trabalho;
} E;

void assgpublur_lote_ini(void) {
  if (E.aberto) return;
  memset(&E, 0, sizeof E);
  E.aberto = 1;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &E.fbo);
  glGetIntegerv(GL_VIEWPORT, E.vp);
  glGetIntegerv(GL_CURRENT_PROGRAM, &E.prog);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &E.ativa);
  glActiveTexture(GL_TEXTURE1);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &E.tex1);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &E.tex0);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &E.vbo);
  glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &E.aOn);
  glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &E.aSize);
  glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &E.aType);
  glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &E.aNorm);
  glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &E.aStride);
  glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &E.aPtr);
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &E.unpack);
  E.blend = glIsEnabled(GL_BLEND);
  E.tesoura = glIsEnabled(GL_SCISSOR_TEST);
  E.dither = glIsEnabled(GL_DITHER);
  glGetBooleanv(GL_COLOR_WRITEMASK, E.mascara);
  glGetFloatv(GL_COLOR_CLEAR_VALUE, E.limpa);
}

// Antes do primeiro trabalho do lote: o estado das nossas passadas.
static void lote_trabalhar(void) {
  if (E.trabalho) return;
  E.trabalho = 1;
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DITHER);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void *)0);
}

void assgpublur_lote_fim(void) {
  if (!E.aberto) return;
  if (E.trabalho) {
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)E.fbo);
    glViewport(E.vp[0], E.vp[1], E.vp[2], E.vp[3]);
    glUseProgram((GLuint)E.prog);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, (GLuint)E.tex1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)E.tex0);
    glActiveTexture((GLenum)E.ativa);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)E.vbo);
    if (E.aOn) glEnableVertexAttribArray(0); else glDisableVertexAttribArray(0);
    glVertexAttribPointer(0, E.aSize ? E.aSize : 4, E.aType ? (GLenum)E.aType : GL_FLOAT, (GLboolean)E.aNorm,
                          E.aStride, E.aPtr);
    glPixelStorei(GL_UNPACK_ALIGNMENT, E.unpack);
    if (E.blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (E.tesoura) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (E.dither) glEnable(GL_DITHER); else glDisable(GL_DITHER);
    glColorMask(E.mascara[0], E.mascara[1], E.mascara[2], E.mascara[3]);
    glClearColor(E.limpa[0], E.limpa[1], E.limpa[2], E.limpa[3]);
#ifndef NV_AGB_TESTE
    gfx_estado_externo();   // o cache de programa/textura do gfx nao vale mais
#endif
  }
  E.aberto = 0;
}

// ---- iniciar ---------------------------------------------------------------------
int assgpublur_iniciar(void) {
  static const GLfloat quad[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
  const char *ext, *ver;
  GLint vboAnt = 0, fboAnt = 0, texAnt = 0;
  int es3;
  if (pronto >= 0) return pronto;
  pronto = 0;
  ext = (const char *)glGetString(GL_EXTENSIONS);
  ver = (const char *)glGetString(GL_VERSION);
  if (!ext) ext = "";
  es3 = ver && strstr(ver, "OpenGL ES ") && ver[10] >= '3';
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &vboAnt);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fboAnt);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &texAnt);
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)vboAnt);
  glGenFramebuffers(1, &fbo);
  progReduz = ligar(FS_REDUZ);
  if (progReduz) {
    rzT = glGetUniformLocation(progReduz, "uT"); rzEsc = glGetUniformLocation(progReduz, "uEsc");
    rzOff = glGetUniformLocation(progReduz, "uOff"); rzEixo = glGetUniformLocation(progReduz, "uEixo");
  }
  {
    /* Meia precisao quando da para renderizar nela E filtrar (bilinear). */
    static const AgbFmt R16F = { GL_R16F, GL_RED, GL_HALF_FLOAT, 2, "R16F" };
    static const AgbFmt H16 = { GL_RGBA, GL_RGBA, GL_HALF_FLOAT_OES, 8, "RGBA16F (OES)" };
    static const AgbFmt R8 = { GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, 4, "RGBA8" };
    int cbHalf = tem_ext(ext, "GL_EXT_color_buffer_half_float") || tem_ext(ext, "GL_EXT_color_buffer_float");
    if (es3 && cbHalf && alvo_ok(&R16F)) fmtMeio = R16F;
    else if (tem_ext(ext, "GL_OES_texture_half_float") && tem_ext(ext, "GL_OES_texture_half_float_linear") &&
             tem_ext(ext, "GL_EXT_color_buffer_half_float") && alvo_ok(&H16)) fmtMeio = H16;
    else fmtMeio = R8;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fboAnt);
  glBindTexture(GL_TEXTURE_2D, (GLuint)texAnt);
#ifndef NV_AGB_TESTE
  gfx_tex_esquecer(0);
#endif
  pronto = progReduz && prog_final(0) && prog_final(1) && prog_blur(1) && vbo && fbo && alvo_ok(&fmtMeio);
  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fboAnt);
  glBindTexture(GL_TEXTURE_2D, (GLuint)texAnt);
  printf("[assperf] gpu blur: %s | intermediate %s | %s | max texture %d\n", pronto ? "ready" : "NOT available",
         fmtMeio.nome ? fmtMeio.nome : "-", ver ? ver : "?", (int)maxTex);
  fflush(stdout);
  return pronto;
}


// ---- reserva de texturas ----------------------------------------------------------
/* Criar textura na Mali custa (24/10: ~1,75 ms por letreiro entre alocacoes e
 * passadas). As soltas voltam para esta reserva, por tipo e tamanho exato, e
 * a proxima do mesmo tamanho reaproveita o armazenamento. */
enum { TX_MEIO, TX_RGBA, TX_LUM };
#define AGB_RESERVA 128
#define AGB_RESERVA_BYTES (12u * 1024u * 1024u)
/* `liberado` = quadro GL em que voltou para a reserva: so sai de novo dois
 * quadros depois (a GPU da Mali ainda pode estar lendo; regravar antes faria o
 * driver esperar ou copiar a textura inteira). */
typedef struct { GLuint tex; int tipo, w, h; unsigned uso, liberado; } AgbRes;
static unsigned quadroAgora;   // o `quadro` do pedido em curso (assgpublur_obter)
static int ultW, ultH;          // tamanho da textura do ultimo assgpublur_obter
static unsigned ultimoTrabalho; // quadro do ultimo blur/final novo (aparar)
static AgbRes reserva[AGB_RESERVA];
static int nRes;
static size_t bytesRes;
static unsigned relogio;

static size_t tx_bytes(int tipo, int w, int h) {
  return (size_t)w * (size_t)h * (tipo == TX_MEIO ? (size_t)fmtMeio.bpp : tipo == TX_RGBA ? 4u : 1u);
}

static void tex_param(GLint filtro) {
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtro);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtro);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* Textura do tipo e tamanho, ligada na unidade ativa. *nova = 1 quando o
 * armazenamento acabou de ser criado (sem conteudo). */
static GLuint res_pegar(int tipo, int w, int h, int *nova) {
  int i;
  GLuint t = 0;
  relogio++;
  for (i = 0; i < nRes; i++)
    /* So a textura que a CPU regrava (TX_LUM, glTexSubImage2D) espera dois
     * quadros: as que so a GPU desenha seguem a ordem dos comandos. */
    if (reserva[i].tipo == tipo && reserva[i].w == w && reserva[i].h == h &&
        (tipo != TX_LUM || quadroAgora - reserva[i].liberado >= 2)) {
      t = reserva[i].tex;
      bytesRes -= tx_bytes(tipo, w, h);
      reserva[i] = reserva[--nRes];
      glBindTexture(GL_TEXTURE_2D, t);
      if (nova) *nova = 0;
      return t;
    }
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  if (tipo == TX_MEIO) glTexImage2D(GL_TEXTURE_2D, 0, fmtMeio.interno, w, h, 0, fmtMeio.fmt, fmtMeio.tipo, NULL);
  else if (tipo == TX_RGBA) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  /* TX_LUM: quem pede sobe o conteudo com glTexImage2D */
  tex_param(tipo == TX_LUM ? GL_NEAREST : GL_LINEAR);
  if (nova) *nova = 1;
  return t;
}

static void res_soltar(GLuint t, int tipo, int w, int h) {
  size_t b = tx_bytes(tipo, w, h);
  if (!t) return;
  while (nRes && (nRes >= AGB_RESERVA || bytesRes + b > AGB_RESERVA_BYTES)) {
    int i, v = 0;
    for (i = 1; i < nRes; i++) if (reserva[i].uso < reserva[v].uso) v = i;
    glDeleteTextures(1, &reserva[v].tex);
    bytesRes -= tx_bytes(reserva[v].tipo, reserva[v].w, reserva[v].h);
    reserva[v] = reserva[--nRes];
  }
  if (b > AGB_RESERVA_BYTES) { glDeleteTextures(1, &t); return; }
  reserva[nRes].tex = t; reserva[nRes].tipo = tipo; reserva[nRes].w = w; reserva[nRes].h = h;
  reserva[nRes].uso = ++relogio;
  reserva[nRes].liberado = quadroAgora;
  nRes++;
  bytesRes += b;
}

/* Sobe um bitmap compacto (GL_LUMINANCE) numa textura da reserva. */
static GLuint subir_lum(const unsigned char *px, int w, int h, GLint filtro) {
  int nova = 0;
  GLuint t;
  t = res_pegar(TX_LUM, w, h, &nova);
  if (nova) glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, px);
  else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_LUMINANCE, GL_UNSIGNED_BYTE, px);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtro);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtro);
  return t;
}

// ---- caches -----------------------------------------------------------------------
typedef struct {
  unsigned long long chave;
  GLuint tex;
  int tipo, w, h, kx, ky;   // textura (tipo da reserva, tamanho) e niveis
  int ny;                   // > 0: o gaussiano vertical fica para a passada final
  float wy0, wy[AGB_PARES_MAX], oy[AGB_PARES_MAX];
  unsigned uso;
  size_t bytes;
} AgbBlur;
typedef struct {
  unsigned long long chave;
  GLuint tex;
  int w, h;              // a imagem
  int tw, th;            // a textura (classe de tamanho: w, h arredondados para cima)
  unsigned uso;
} AgbFinal;
static AgbBlur *cb; static int nb, capb; static size_t bytesB;
static AgbFinal *cf; static int nf, capf; static size_t bytesF;

static unsigned long long mistura(unsigned long long h, unsigned long long v) {
  h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
  h *= 0x100000001B3ULL;
  return h ^ (h >> 29);
}
static unsigned long long bits_d(double d) { unsigned long long u; memcpy(&u, &d, sizeof u); return u; }

// Solta as entradas mais antigas fora de uso (nao usadas neste quadro nem no
// anterior) ate caber `novo` bytes.
static void blur_caber(size_t novo, unsigned quadro) {
  while (bytesB + novo > AGB_BLUR_BYTES && nb) {
    int i, v = -1;
    for (i = 0; i < nb; i++)
      if (quadro - cb[i].uso >= 2 && (v < 0 || cb[i].uso < cb[v].uso)) v = i;
    if (v < 0) break;
    res_soltar(cb[v].tex, cb[v].tipo, cb[v].w, cb[v].h);
    bytesB -= cb[v].bytes;
    cb[v] = cb[--nb];
  }
}
static void final_caber(size_t novo, unsigned quadro) {
  while (bytesF + novo > AGB_FINAL_BYTES && nf) {
    int i, v = -1;
    for (i = 0; i < nf; i++)
      if (quadro - cf[i].uso >= 2 && (v < 0 || cf[i].uso < cf[v].uso)) v = i;
    if (v < 0) break;
    res_soltar(cf[v].tex, TX_RGBA, cf[v].tw, cf[v].th);
    bytesF -= (size_t)cf[v].tw * (size_t)cf[v].th * 4u;
    cf[v] = cf[--nf];
  }
}

/* Fim de cena pesada: sem blur novo ha ~5 s (300 quadros GL), solta o que nao
 * foi desenhado nos ultimos ~2 s e esvazia a reserva (memoria de volta). */
void assgpublur_aparar(unsigned quadro) {
  int i;
  if (pronto != 1 || quadro - ultimoTrabalho < 300u || (!nb && !nf && !nRes)) return;
  for (i = 0; i < nb; ) {
    if (quadro - cb[i].uso >= 120u) { glDeleteTextures(1, &cb[i].tex); bytesB -= cb[i].bytes; cb[i] = cb[--nb]; }
    else i++;
  }
  for (i = 0; i < nf; ) {
    if (quadro - cf[i].uso >= 120u) {
      glDeleteTextures(1, &cf[i].tex); bytesF -= (size_t)cf[i].tw * (size_t)cf[i].th * 4u; cf[i] = cf[--nf];
    } else i++;
  }
  for (i = 0; i < nRes; i++) glDeleteTextures(1, &reserva[i].tex);
  nRes = 0; bytesRes = 0;
  ultimoTrabalho = quadro;   // so de novo depois de outra cena
#ifndef NV_AGB_TESTE
  gfx_tex_esquecer(0);
#endif
}

void assgpublur_apagar(void) {
  int i;
  for (i = 0; i < nb; i++) glDeleteTextures(1, &cb[i].tex);
  for (i = 0; i < nf; i++) glDeleteTextures(1, &cf[i].tex);
  for (i = 0; i < nRes; i++) glDeleteTextures(1, &reserva[i].tex);
  free(cb); free(cf); cb = NULL; cf = NULL; nb = nf = capb = capf = 0; bytesB = bytesF = 0;
  nRes = 0; bytesRes = 0;
#ifndef NV_AGB_TESTE
  gfx_tex_esquecer(0);
#endif
}

// ---- o gaussiano -------------------------------------------------------------------
// Nivel e sigma no nivel para a variancia V (px^2) de um eixo.
static void plano(double V, int *k, double *v2) {
  int n;
  *k = 0; *v2 = V;
  for (n = 1; n <= AGB_NIVEL_MAX; n++) {
    double p = ldexp(1.0, 2 * n);   // 4^n
    double r = (V - (p - 1.0) / 4.0 - p / 6.0) / p;
    if (r < 4.0) break;              // sigma no nivel >= 2: a ampliacao bilinear fica abaixo de ~2 niveis
    *k = n; *v2 = r;
  }
}

// Pesos de um eixo: centro + pares lineares. 0 pares = sem blur nesse eixo.
static int pesos(double v2, float *w0, float *w, float *o) {
  double g[2 * AGB_PARES_MAX + 2], soma;
  int R, i, n;
  if (v2 <= 0.001) { *w0 = 1.0f; return 0; }
  if (v2 < 0.3) {
    /* sigma pequeno: 3 taps [a, 1-2a, a] com variancia exata 2a */
    *w0 = (float)(1.0 - v2); w[0] = (float)(v2 * 0.5); o[0] = 1.0f;
    return 1;
  }
  R = (int)ceil(3.0 * sqrt(v2));
  if (R > 2 * AGB_PARES_MAX) R = 2 * AGB_PARES_MAX;
  g[0] = 1.0;
  for (i = 1; i <= R + 1; i++) g[i] = i <= R ? exp(-(double)i * i / (2.0 * v2)) : 0.0;
  soma = g[0];
  for (i = 1; i <= R; i++) soma += 2.0 * g[i];
  n = 0;
  for (i = 1; i <= R; i += 2) {
    double a = g[i] / soma, b = g[i + 1] / soma;
    w[n] = (float)(a + b);
    o[n] = (float)((i * a + (i + 1) * b) / (a + b));
    n++;
  }
  *w0 = (float)(g[0] / soma);
  return n;
}

/* CLASSE DE TAMANHO (25/10): a textura final arredonda w e h para cima (32 px;
 * 64 acima de 512). No log da S95C 284 de 327 finais criavam textura nova
 * (tamanho exato raramente se repete com o letreiro rolando) e criar textura
 * na Mali e o grosso dos 0,85 ms por passada final. A sobra fica transparente
 * e o desenho usa o tamanho da textura (assgpublur_ultimo_tam). */
static int classe(int v) { int m = v > 512 ? 64 : 32; return (v + m - 1) / m * m; }
void assgpublur_ultimo_tam(int *w, int *h) { *w = ultW; *h = ultH; }

static void desenhar_em(GLuint tex, int w, int h);
/* Final numa textura da classe: limpa a textura inteira (a sobra de quem a usou
 * antes; no tile da Mali o clear tambem poupa a leitura do conteudo velho) e
 * desenha so a parte w x h. */
static void desenhar_em_limpo(GLuint tex, int tw, int th, int w, int h) {
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  glViewport(0, 0, tw, th);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  desenhar_em(tex, w, h);
}

static void desenhar_em(GLuint tex, int w, int h) {
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  glViewport(0, 0, w, h);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static int passo_blur(GLuint de, int w, int h, int eixo, int n, const float *w0, const float *ws, const float *os,
                      GLuint para) {
  GLuint p = prog_blur(n);
  if (!p) return 0;
  glUseProgram(p);
  glUniform1i(blT[n], 0);
  glUniform2f(blPasso[n], eixo == 0 ? 1.0f / (float)w : 0.0f, eixo == 1 ? 1.0f / (float)h : 0.0f);
  glUniform1f(blW0[n], *w0);
  glUniform1fv(blW[n], n, ws);
  glUniform1fv(blO[n], n, os);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, de);
  desenhar_em(para, w, h);
  return 1;
}

// Borra a fonte e guarda no cache. NULL = falhou.
//   reducoes (x, depois y) -> gaussiano x -> gaussiano y. Sem reducao em y o
//   gaussiano y fica para a passada final (ny na entrada); sem gaussiano x
//   tambem, a entrada E a textura da fonte (nenhuma passada aqui).
static AgbBlur *borrar(const AgbPedido *p, unsigned long long chave, unsigned quadro) {
  GLuint fonte, atual, destino = 0;
  int atualTipo = TX_LUM, destTipo = TX_MEIO;
  int kx, ky, w = p->srcW, h = p->srcH, i, nx, ny, ok = 1, adiaY;
  double vx, vy;
  float wx0, wy0, wx[AGB_PARES_MAX], ox[AGB_PARES_MAX], wy[AGB_PARES_MAX], oy[AGB_PARES_MAX];
  AgbBlur *e;
  plano(p->r2x, &kx, &vx);
  plano(p->r2y, &ky, &vy);
  nx = pesos(vx, &wx0, wx, ox);
  ny = pesos(vy, &wy0, wy, oy);
  adiaY = ky == 0 && ny > 0 && ny <= AGB_PARES_MAX;
  lote_trabalhar();
  glActiveTexture(GL_TEXTURE0);
  fonte = subir_lum(p->src, w, h, GL_LINEAR);
  atual = fonte;
  /* reducoes: x depois y */
  for (i = 0; i < kx + ky && ok; i++) {
    int ex = i < kx, nova;
    int nw = ex ? (w + 1) / 2 : w, nh = ex ? h : (h + 1) / 2;
    GLuint t = res_pegar(TX_MEIO, nw, nh, &nova);
    glUseProgram(progReduz);
    glUniform1i(rzT, 0);
    glUniform1f(rzEsc, ex ? 2.0f * (float)nw / (float)w : 2.0f * (float)nh / (float)h);
    glUniform1f(rzOff, ex ? 0.75f / (float)w : 0.75f / (float)h);
    glUniform2f(rzEixo, ex ? 1.0f : 0.0f, ex ? 0.0f : 1.0f);
    glBindTexture(GL_TEXTURE_2D, atual);
    desenhar_em(t, nw, nh);
    res_soltar(atual, atualTipo, w, h);
    atual = t; atualTipo = TX_MEIO; w = nw; h = nh;
  }
  /* gaussiano */
  if (adiaY && !nx) {
    destino = atual; destTipo = atualTipo; atual = 0;
  } else {
    int nova;
    destino = res_pegar(TX_MEIO, w, h, &nova);
    if (adiaY || !ny) {
      if (nx) ok = passo_blur(atual, w, h, 0, nx, &wx0, wx, ox, destino);
      else { float w1 = 1.0f, o1 = 0.0f, z = 0.0f; ok = passo_blur(atual, w, h, 1, 1, &w1, &z, &o1, destino); }
    } else if (nx) {
      GLuint tb = res_pegar(TX_MEIO, w, h, &nova);
      ok = passo_blur(atual, w, h, 0, nx, &wx0, wx, ox, tb) && passo_blur(tb, w, h, 1, ny, &wy0, wy, oy, destino);
      res_soltar(tb, TX_MEIO, w, h);
    } else ok = passo_blur(atual, w, h, 1, ny, &wy0, wy, oy, destino);
    res_soltar(atual, atualTipo, w, h);
  }
  if (!ok || glGetError() != GL_NO_ERROR) { res_soltar(destino, destTipo, w, h); return NULL; }
  {
    size_t bytes = tx_bytes(destTipo, w, h);
    blur_caber(bytes, quadro);
    if (nb == capb) {
      int nc = capb ? capb * 2 : 32;
      AgbBlur *nv = realloc(cb, (size_t)nc * sizeof *nv);
      if (!nv) { res_soltar(destino, destTipo, w, h); return NULL; }
      cb = nv; capb = nc;
    }
    e = &cb[nb++];
    memset(e, 0, sizeof *e);
    e->chave = chave; e->tex = destino; e->tipo = destTipo; e->w = w; e->h = h; e->kx = kx; e->ky = ky;
    e->uso = quadro; e->bytes = bytes;
    if (adiaY) {
      e->ny = ny; e->wy0 = wy0;
      memcpy(e->wy, wy, sizeof wy); memcpy(e->oy, oy, sizeof oy);
      if (destTipo == TX_LUM) {   // a fonte vira a entrada: filtro bilinear para a ampliacao
        glBindTexture(GL_TEXTURE_2D, destino);
        tex_param(GL_LINEAR);
      }
    }
    bytesB += bytes;
    return e;
  }
}

static void chaves(const AgbPedido *p, unsigned long long *kbOut, unsigned long long *kfOut) {
  unsigned long long kb, kf;
  kb = mistura(mistura(mistura(p->srcChave, ((unsigned long long)(unsigned)p->srcW << 32) | (unsigned)p->srcH),
                       bits_d(p->r2x)), bits_d(p->r2y));
  kf = mistura(kb, ((unsigned long long)(unsigned)p->subX << 32) | (unsigned)p->subY);
  kf = mistura(kf, ((unsigned long long)(unsigned)p->w << 32) | (unsigned)p->h);
  kf = mistura(kf, ((unsigned long long)(unsigned)p->shiftX << 8) | (unsigned)p->shiftY);
  if (p->fix && p->fill) {
    kf = mistura(kf, p->fillChave);
    kf = mistura(kf, ((unsigned long long)(unsigned)p->fillDx << 32) | (unsigned)p->fillDy);
    kf = mistura(kf, ((unsigned long long)(unsigned)p->fillW << 32) | (unsigned)p->fillH);
  }
  if (!kf) kf = 1;
  *kbOut = kb; *kfOut = kf;
}
GLuint assgpublur_obter(const AgbPedido *p, unsigned quadro, unsigned long long *chaveOut) {
  unsigned long long kb, kf;
  AgbBlur *b = NULL;
  GLuint fillTex = 0, out = 0, prog;
  int i, nova, tw, th;
  quadroAgora = quadro;
  AgbLocFinal *l;
  *chaveOut = 0;
  /* Never touch GL state outside a batch: nothing would put the app's back. */
  if (!E.aberto) return 0;
  if (pronto != 1 || !p->src || p->w <= 0 || p->h <= 0 || p->srcW <= 0 || p->srcH <= 0 ||
      p->srcW > maxTex || p->srcH > maxTex || p->w > maxTex || p->h > maxTex ||
      (p->fix && p->fill && (p->fillW > maxTex || p->fillH > maxTex))) return 0;
  chaves(p, &kb, &kf);
  for (i = 0; i < nf; i++)
    if (cf[i].chave == kf && cf[i].w == p->w && cf[i].h == p->h) {
      cf[i].uso = quadro; *chaveOut = kf;
      ultW = cf[i].tw; ultH = cf[i].th;
      return cf[i].tex;
    }
  if (p->soCache) return 0;
  for (i = 0; i < nb; i++) if (cb[i].chave == kb) { b = &cb[i]; b->uso = quadro; break; }
  if (!b) b = borrar(p, kb, quadro);
  if (!b) return 0;
  prog = prog_final(b->ny);
  if (!prog) return 0;
  l = &fn[b->ny];
  lote_trabalhar();
  tw = classe(p->w); th = classe(p->h);
  final_caber((size_t)tw * (size_t)th * 4u, quadro);
  glActiveTexture(GL_TEXTURE0);
  out = res_pegar(TX_RGBA, tw, th, &nova);
  if (p->fix && p->fill) {
    glActiveTexture(GL_TEXTURE1);
    fillTex = subir_lum(p->fill, p->fillW, p->fillH, GL_NEAREST);
  }
  glUseProgram(prog);
  glUniform1i(l->B, 0);
  glUniform1i(l->G, 1);
  glUniform2f(l->BEsc, 1.0f / (float)ldexp((double)b->w, b->kx), 1.0f / (float)ldexp((double)b->h, b->ky));
  glUniform2f(l->Sub, (float)p->subX, (float)p->subY);
  glUniform2f(l->Saida, (float)p->w, (float)p->h);
  glUniform2f(l->Fonte, (float)p->srcW, (float)p->srcH);
  glUniform2f(l->GOff, (float)p->fillDx, (float)p->fillDy);
  glUniform2f(l->GTam, fillTex ? (float)p->fillW : 1.0f, fillTex ? (float)p->fillH : 1.0f);
  glUniform1f(l->Fix, p->fix ? 1.0f : 0.0f);
  glUniform1f(l->TemG, fillTex ? 1.0f : 0.0f);
  glUniform2f(l->Desl, (float)p->shiftX / 64.0f, (float)p->shiftY / 64.0f);
  if (b->ny) {
    glUniform1f(l->PassoY, 1.0f / (float)b->h);
    glUniform1f(l->Wy0, b->wy0);
    glUniform1fv(l->Wy, b->ny, b->wy);
    glUniform1fv(l->Oy, b->ny, b->oy);
  }
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, b->tex);
  desenhar_em_limpo(out, tw, th, p->w, p->h);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
  if (fillTex) {
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    res_soltar(fillTex, TX_LUM, p->fillW, p->fillH);
    glActiveTexture(GL_TEXTURE0);
  }
  if (glGetError() != GL_NO_ERROR) { res_soltar(out, TX_RGBA, tw, th); return 0; }
  if (nf == capf) {
    int nc = capf ? capf * 2 : 64;
    AgbFinal *nv = realloc(cf, (size_t)nc * sizeof *nv);
    if (!nv) { res_soltar(out, TX_RGBA, tw, th); return 0; }
    cf = nv; capf = nc;
  }
  cf[nf].chave = kf; cf[nf].tex = out; cf[nf].w = p->w; cf[nf].h = p->h; cf[nf].uso = quadro;
  cf[nf].tw = tw; cf[nf].th = th;
  nf++;
  bytesF += (size_t)tw * (size_t)th * 4u;
  ultW = tw; ultH = th; ultimoTrabalho = quadro;
  *chaveOut = kf;
  return out;
}

void assgpublur_usar(unsigned long long chave, unsigned quadro) {
  int i;
  if (!chave) return;
  for (i = 0; i < nf; i++) if (cf[i].chave == chave) { cf[i].uso = quadro; return; }
}

#endif
