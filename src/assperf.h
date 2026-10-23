// ASS (libass) PERFORMANCE TEST OPTIONS (dono, 23/10/2026).
//
// O letreiro quadro a quadro de anime (a cena do celular de Kaguya-sama, faixa
// "Signs & Songs" com 36.944 blocos) deixava a legenda atrasada e aos trancos
// na S95C: o upload das texturas no fio de desenho custava ~200 ms por quadro,
// cada lote do mkvass re-lia o documento inteiro com o worker travado, o
// quadro saia para o instante em que foi PEDIDO (e nao para quando aparece) e
// as fontes anexadas nao entravam. Diagnostico: claude/libass-perf-DIAGNOSIS.md.
//
// Cada correcao tem a sua chave aqui, para o dono testar na TV e mandar o log
// de cada combinacao. As chaves vivem em assperf.txt (dados), so neste
// aparelho; a folha fica no player: Legendas > Estilo > "ASS performance".
//
// SO O .tpk MUDA. Nos outros alvos assperf_get devolve sempre o valor
// "Legacy" (o comportamento de antes de 23/10), e a folha nao existe.
//
// SEGURANCA: enquanto uma faixa ASS desenha com alguma chave fora do Legacy,
// fica gravada a marca assperf-ativo.txt. Se o app cair com ela gravada, o
// proximo arranque volta tudo para o Legacy e diz no log.
#ifndef NV_ASSPERF_H
#define NV_ASSPERF_H

#include <stddef.h>

enum {
  AP_MOTOR,        // libass bitmap engine: 0 plain C (old), 1 NEON (armv7 SIMD, bit-identical)
  AP_UPLOAD,       // 0 RGBA (old), 1 Alpha 8-bit + tint
  AP_TEXCACHE,     // 0 off (old), 1 reuse unchanged images
  AP_TEXANEL,      // texture recycle delay: 0 = 1 frame, 1 = 2 frames, 2 = 3 frames
  AP_FORATRAVA,    // 0 upload under the queue lock (old), 1 outside
  AP_INCREMENTAL,  // 0 full reparse per batch (old), 1 new lines only
  AP_PODA,         // drop past events: 0 off, 1 30 s, 2 2 min, 3 5 min
  AP_ADIANTE,      // render ahead: 0 off (old), 1 auto, 2 50, 3 100, 4 150, 5 250 ms
  AP_SEGURAR,      // early frames: 0 show at once, 1 hold until due
  AP_CACHE,        // libass bitmap cache: 0 32 MB (old), 1 64, 2 128, 3 16
  AP_GLIFOS,       // libass glyph cache: 0 default (old), 1 1000, 2 4000, 3 10000
  AP_ESCALA,       // render resolution: 0 100% (old), 1 85%, 2 75%, 3 66%, 4 50%
  AP_HINTING,      // 0 light (old), 1 none, 2 normal, 3 native
  AP_SHAPER,       // 0 complex (old), 1 simple
  AP_FONTES,       // attached fonts limit: 0 16 MB (old), 1 32, 2 48, 3 64, 4 96
  AP_CUES,         // app cue copy per batch: 0 every batch (old), 1 every 10 s while libass draws
  AP_RELAY,        // read subtitle blocks from the local relay stream: 0 off (old), 1 on
  AP_PRIORIDADE,   // render thread priority: 0 normal (old), 1 higher (try), 2 lower
  AP_MUDANCA,      // render only when the picture can change: 0 every UI frame (old), 1 on
  AP_ESTAGIOS,     // libass stage timers in the 10 s log: 0 off, 1 on (measurement, not in presets)
  AP_BENCH,        // ASS bench: 0 off, 1 run (this engine), 2 run C vs NEON (an action; back to 0 when it starts)
  AP_OPACIDADE,    // styled ASS + the player's subtitle opacity: 0 applies (old), 1 ignored (ASS drawn as authored)
  AP_LOG,          // subtitle log: 0 off, 1 summary every 10 s, 2 detailed
  AP_N
};

// Valor atual (barato: le um int). Fora do .tpk: sempre o Legacy.
int  assperf_get(int id);
// Le assperf.txt (uma vez; chamadas seguintes nao fazem nada) e aplica a
// guarda de queda. Seguro em qualquer fio.
void assperf_iniciar(void);

// Folha: rotulos e valores.
int         assperf_n_valores(int id);
const char *assperf_rotulo(int id);
const char *assperf_valor_rotulo(int id, int v);
const char *assperf_ajuda(int id);
void        assperf_definir(int id, int v);   // grava e loga
// Presets: 0 Recommended, 1 Legacy (all old), 2 Lightest.
void        assperf_preset(int p);
const char *assperf_preset_nome(void);        // "Recommended" / "Legacy" / "Lightest" / "Custom"
// Uma linha "upload=alpha texcache=on ..." para o log e para a folha.
void        assperf_resumo(char *dst, size_t n);
// Contador que muda a cada assperf_definir/preset (quem le reaplica).
unsigned    assperf_versao(void);

// Valores derivados.
size_t assperf_fontes_teto(void);            // bytes
double assperf_escala(void);                 // 1.0 .. 0.5
int    assperf_adiante_fixo_ms(void);        // -1 = auto, 0 = off
long   assperf_poda_ms(void);                // 0 = off

// Marca da guarda de queda (assrender: faixa ASS ligada/desligada).
void assperf_faixa_ativa(int ativa);

// --- medidas (o log "Subtitles" da pagina da rede) --------------------------
// Tudo barato e seguro em qualquer fio. O relatorio sai a cada 10 s, so com
// faixa ASS ativa, em uma linha "[assperf] 10s: ...".
void assperf_med_render(double ms, double esperaMs, int mudou);   // worker
void assperf_med_upload(double ms, size_t bytes, int imagens, int reusadas, int quadroNovo); // fio GL
void assperf_med_quadro(double desenhoMs);                        // fio GL, por quadro com ASS
void assperf_med_lote(double ms, int novos, int total, int cheio); // carga do documento
void assperf_med_atraso(double ms);                               // quadro mostrado vs. o instante dele
void assperf_med_adiante(double ms);                              // adiantamento usado
void assperf_med_relay(int blocos, int usados);                   // mkvfix -> mkvass
void assperf_med_poda(int removidos);
void assperf_med_alfa(float alfa, float opacidadeUsuario);         // fio GL: alfa do desenho do libass
void assperf_med_tick(void);                                      // fio GL: imprime a cada 10 s
#ifdef NV_ASSPERF_TESTE
void assperf_teste_definir(int id, int v);   // provas no computador
void assperf_med_forcar(void);               // imprime a janela agora
#endif
// libass por etapa (o libass do .tpk tem cronometros: tools/tpk/libass-nv).
// O worker manda um por render; sai na linha "[assperf] stages:" da janela.
typedef struct {
  int motor;                 // 0 C, 1 NEON, -1 desconhecido
  double frameMs;            // dentro de ass_render_frame
  double stageMs[8];         // other, shape, outline, raster, blur, be, compose, clip
  unsigned frames, events, images;
  unsigned hit[3], miss[3];  // caches outline, bitmap, composite
} AssperfEstagios;
void assperf_med_estagios(const AssperfEstagios *e);
extern const char *const assperf_estagio_nome[8];
// Chave de medida (log, cronometros, bench): fora dos presets e da guarda.
int  assperf_e_medida(int id);
// Bench: estado para a folha ("Running...") e o resumo do ultimo.
void assperf_bench_estado(int rodando, const char *resumo);
int  assperf_bench_rodando(void);

// Linha curta para a folha ("render 4.1/38 ms · upload 0.6/3 ms · late 12 ms").
void assperf_med_texto(char *dst, size_t n);

#endif
