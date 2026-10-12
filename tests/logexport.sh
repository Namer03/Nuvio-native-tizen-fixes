#!/bin/bash
# Limpeza e montagem do registro exportado por QR. Ver src/logexport.h.
#   bash tests/logexport.sh
set -eu
cd "$(dirname "$0")/.."
D=$(mktemp -d "${TMPDIR:-/tmp}/nuvio-logexport.XXXXXX")
trap 'rm -rf "$D"' EXIT
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc src/logexport.c tests/logexport.c -o "$D/t"
# arquivo atual maior que o teto (so a cauda entra) e um segredo no inicio.
{ echo "SEGREDO api_key=abcdefghijklmnopq"; for i in $(seq 1 6000); do echo "linha $i de teste do registro com algum texto para encher"; done; echo "linha-final"; } > "$D/atual"
echo "sessao anterior apikey=ZZZZZZZZZZZZ fim" > "$D/ant"
"$D/t" "$D/atual" "$D/ant"
