#!/bin/bash
export TMPDIR="${TMPDIR:-/tmp}"
TMPDIR="${TMPDIR%/}"
# "TV luminance limit (test)" e "TV video gray (test)" (src/tpkluz.c) contra uma
# libavoc.so falsa.
# Ver tests/tpkluz.c. So precisa do cc do sistema.
set -eu
cd "$(dirname "$0")/.."
D="$TMPDIR/nuvio-tpkluz"
rm -rf "$D"; mkdir -p "$D/lib" "$D/semlib" "$D/dados"
SO_FLAGS="-shared -fPIC"; [ "$(uname)" = Darwin ] && SO_FLAGS="-dynamiclib"
cc $SO_FLAGS -o "$D/lib/libavoc.so" tests/tpkluz_avoc.c
cc -DNV_TPK -Wall -Wextra -Wno-unused-parameter tests/tpkluz.c src/tpkluz.c -o "$D/teste" -lpthread -ldl 2>/dev/null ||
  cc -DNV_TPK -Wall -Wextra -Wno-unused-parameter tests/tpkluz.c src/tpkluz.c -o "$D/teste" -lpthread
export FAKE_AVOC_DB="$D/db" FAKE_AVOC_GRAY_DB="$D/cinza" TPKLUZ_DADOS="$D/dados"
roda() {   # $1 = pasta da lib, $2.. = modo e ambiente
  local lib="$1"; shift
  LD_LIBRARY_PATH="$lib" DYLD_LIBRARY_PATH="$lib" "$@" >>"$D/log" 2>&1 || { cat "$D/log"; exit 1; }
}
roda "$D/lib" "$D/teste" normal
roda "$D/lib" "$D/teste" cai
[ -f "$D/dados/luz-original.txt" ] || { echo "tpkluz: a queda nao deixou a nota"; exit 1; }
roda "$D/lib" "$D/teste" arranque
roda "$D/lib" "$D/teste" cinza
roda "$D/lib" "$D/teste" caicinza
[ -f "$D/dados/cinza-original.txt" ] || { echo "tpkluz: a queda nao deixou a nota do video gray"; exit 1; }
roda "$D/lib" "$D/teste" arranquecinza
roda "$D/semlib" "$D/teste" semlib
roda "$D/lib" env FAKE_AVOC_GET_ERR=1 "$D/teste" erroget
grep -q "before=1 set=0 result=0" "$D/log" || { cat "$D/log"; echo "tpkluz: falta a linha before/set/result"; exit 1; }
grep -q "video gray: before=0 set=1 result=0" "$D/log" || { cat "$D/log"; echo "tpkluz: falta a linha do video gray"; exit 1; }
grep -q "not supported on this TV" "$D/log" || { cat "$D/log"; echo "tpkluz: falta o aviso de TV sem suporte"; exit 1; }
grep "tpkluz " "$D/log"
