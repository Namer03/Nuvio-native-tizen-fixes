// DIAGNOSTICO DE ABERTURA (dono, 16/10/2026): fontes que tocam em outros apps
// da mesma TV (Stremio, NuvioTVSmart) e que o player do .tpk recusa no Prepare
// (Kaguya-sama BD T3/T5: 0xfe6c0026 em ~3 s). Para achar o porque sem chute:
//
//   [abrir-diag] — a fonte NAO abriu: o que o player disse, quanto demorou, e o
//                  CABECALHO do arquivo lido por conta propria (EBML, ordem dos
//                  elementos, faixas com codec/perfil/bits/padrao/compressao,
//                  anexos, onde fica o 1o Cluster e o Cues).
//   [tocar-diag] — a fonte ABRIU e tocou, e depois deu erro.
//
// A leitura do cabecalho e LATERAL (fio proprio, ate 4 MiB + saltos de 64 KiB)
// e so roda quando a pagina de registro na rede pede (abrirdiag_definir). Sem
// URL e sem chave no log: so host, tamanhos e campos do Matroska.
#ifndef NUVIO_ABRIRDIAG_H
#define NUVIO_ABRIRDIAG_H

// Ajustes da pagina de registro (persistidos em logdiag.txt):
//   falha: le o cabecalho de toda fonte que o player recusar (padrao: ligado)
//   ok:    le tambem o de toda fonte que abrir, para comparar (padrao: desligado;
//          custa uma leitura de ate 4 MiB ao lado do video)
//   cdn2min: teste da troca do link do CDN (cdnlink.h): os links do CDN
//          "vencem" em 2 min em vez de 2 h 30 (padrao: desligado)
void abrirdiag_definir(int falha, int ok);
void abrirdiag_definir_cdn_teste(int sim);
int  abrirdiag_falha(void);
int  abrirdiag_ok(void);
int  abrirdiag_cdn_teste(void);

// Le e loga o cabecalho de `url` num fio proprio. `porque` entra na linha
// ("refused", "opened"). Uma vez por URL por sessao do app.
void abrirdiag_sondar(const char *url, const char *cabecalhos, const char *porque);

// O parser, sem rede (exposto para teste): loga o que achou em `buf` (inicio
// do arquivo, `n` bytes). Devolve o deslocamento do proximo elemento de nivel
// 1 que nao coube no trecho (para o salto), ou -1 quando achou o 1o Cluster.
long long abrirdiag_ler(const unsigned char *buf, long n, long long total, const char *tag);

#endif
