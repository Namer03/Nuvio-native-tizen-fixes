#!/bin/bash
# Exportar registro por QR contra curl de verdade. Ver tests/celular_exportar.c.
#   bash tests/celular_exportar.sh
set -eu
cd "$(dirname "$0")/.."
tmp="$(mktemp -d "${TMPDIR:-/tmp}/nuvio-celexp-XXXXXX")"
trap 'rm -rf "$tmp" /tmp/nv-celexp-corpo.txt /tmp/nv-celexp-cab.txt' EXIT
flags=(-O1 -g -Isrc -Wall -Wextra -Wno-unused-parameter)
if [ "${SANITIZE:-0}" = 1 ]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
# O IP da LAN so conta se for privado (10/172.16/192.168); um CI/container nao
# tem. A copia aceita qualquer IP: o resto do servidor e o de producao.
sed 's/return (h >> 24) == 10.*/(void)h; return 1;/' src/celular.c > "$tmp/celular.c"
cc "${flags[@]}" "$tmp/celular.c" tests/celular_exportar.c -o "$tmp/t" -lpthread
"$tmp/t"
