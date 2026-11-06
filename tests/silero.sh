#!/bin/bash
# Silero VAD (src/silero.c) contra o ONNX oficial. A fixture (fala espeak-ng +
# probabilidades do onnxruntime) e gerada na hora por tools/silero/golden.py;
# sem espeak-ng/onnxruntime/silero-vad roda so os casos sinteticos.
#   bash tests/silero.sh     SANITIZE=1 (ASan/UBSan)
set -euo pipefail
cd "$(dirname "$0")/.."
D=$(mktemp -d "${TMPDIR:-/tmp}/nv-silero.XXXXXX"); trap 'rm -rf "$D"' EXIT
flags=()
if [ "${SANITIZE:-0}" = 1 ]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all); fi
cc "${flags[@]}" -DNV_SILERO_PESOS -Isrc -O2 -g -Wall -Wextra src/silero.c src/silero_pesos.c src/audvad.c \
  tests/silero.c -pthread -lm -o "$D/t"
if command -v espeak-ng >/dev/null && python3 -c "import onnxruntime, silero_vad, numpy" 2>/dev/null; then
  python3 tools/silero/golden.py "$D/g.pcm" "$D/g.prob"
  "$D/t" "$D/g.pcm" "$D/g.prob"
else
  "$D/t"
fi
# Os pesos so entram onde a sincronia por audio roda.
for d in "-DNV_ANDROID" "-DNV_TPK" "-DNV_TPK -DNV_TPK40" ""; do
  cc -Isrc $d -fsyntax-only -Wall -Wextra -Werror src/silero.c src/silero_pesos.c src/audvad.c src/audsync.c
done
cat > "$D/n.c" <<'C'
#include "silero.h"
#include <stdio.h>
int main(void) { printf("%u\n", silero_pesos_n); return silero_disponivel(); }
C
cc -Isrc -DNV_TPK -DNV_TPK40 "$D/n.c" src/silero.c src/silero_pesos.c -pthread -lm -o "$D/n"
[ "$("$D/n")" = 0 ] || { echo "pesos no .tpk 4/5"; exit 1; }
echo "silero.sh ok"
