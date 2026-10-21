#!/bin/bash
# Linux: needs Xvfb (xvfb-run) and the SDL2/GLES dev packages of tools/linux.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
saida="${1:-${TMPDIR:-/tmp}/nuvio-seektune-shot}"
mkdir -p "$saida"
dados=$(mktemp -d "${TMPDIR:-/tmp}/nuvio-seektune.XXXXXX")
trap 'rm -rf "$dados"' EXIT
sources=()
for source in src/*.c src/dts/*.c; do
  case "$source" in src/main.c|src/player.c) continue;; esac
  sources+=("$source")
done
read -r -a FL <<< "$(pkg-config --cflags --libs sdl2 SDL2_image SDL2_ttf glesv2 egl zlib)"
cc -DNV_SHOT_HOOKS -DNV_LINUX_DESKTOP "${sources[@]}" tests/seektune_shot.c -Isrc -DGL_UNPACK_ROW_LENGTH=0x0CF2 -o "$dados/shot" -O1 \
  "${FL[@]}" -lGL -ldl -pthread -lm -w
NUVIO_SHOT_VIDRO=1 NUVIO_DADOS="$dados" NUVIO_TESTE_DIR="$dados" xvfb-run -a -s "-screen 0 1920x1080x24" "$dados/shot" "$saida"
for b in "$saida"/*.bmp; do ffmpeg -v error -y -i "$b" "${b%.bmp}.png" && rm -f "$b"; done
