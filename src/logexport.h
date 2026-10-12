// EXPORTAR O REGISTRO PARA O CELULAR (QR): monta o texto que o celular baixa.
//
// O texto sai da TV pela LAN em http puro, entao NADA que possa ser segredo vai
// junto: o registro passa por logexport_limpar antes de virar arquivo. Regras
// (todas so ENCURTAM o texto, nunca o aumentam):
//   - URL: o caminho e a query somem, fica "http://host/…" (a chave do debrid
//     viaja no caminho; o painel do registro ja faz o mesmo na tela);
//   - "token=", "api_key:", "password", "senha", "secret", "Authorization",
//     "Bearer" e parentes: o valor (8+ bytes) vira "[oculto]";
//   - qualquer sequencia opaca de 32+ caracteres [A-Za-z0-9_-] com letra E
//     digito (chave de API, JWT em partes, hash): "[oculto]";
//   - e-mail: a parte antes do @ vira "…".
// Puro C, sem SDL: tests/logexport.sh.
#ifndef NV_LOGEXPORT_H
#define NV_LOGEXPORT_H
#include <stddef.h>

#define LOGEXP_ATUAL_MAX    (200 * 1024)   // o mesmo teto do "Enviar registro"
#define LOGEXP_ANTERIOR_MAX (100 * 1024)
#define LOGEXP_ETAPAS_MAX   (8 * 1024)

// Limpa `s` (n bytes, NUL no fim) NO LUGAR. Devolve o novo tamanho.
size_t logexport_limpar(char *s, size_t n);

// Monta "cabecalho + etapas + anterior + atual", ja limpo (malloc; NULL se nao
// ha nada para exportar). Qualquer caminho pode ser NULL ou nao existir.
// `versao` e `plataforma` vao no cabecalho. *n = tamanho em bytes.
char *logexport_montar(const char *atual, const char *anterior, const char *etapas,
                       const char *versao, const char *plataforma, size_t *n);
#endif
