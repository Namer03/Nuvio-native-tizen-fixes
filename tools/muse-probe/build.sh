#!/usr/bin/env bash
# Build nvdec-probe as a static, freestanding armv7 EABI binary (no libc),
# so it runs on the Tizen TV regardless of its userland.
#
# Requires: clang + lld (any recent version). No ARM sysroot needed.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="${1:-$here/nvdec-probe}"

clang \
  --target=armv7-unknown-linux-gnueabi \
  -march=armv7-a -mfloat-abi=soft \
  -Os -ffreestanding -fno-builtin -fno-stack-protector -fomit-frame-pointer \
  -nostdlib -static -fno-pic -fno-pie \
  -Wl,-e,_start -fuse-ld=lld \
  -o "$out" "$here/nvdec-probe.c"

llvm-strip "$out" 2>/dev/null || true
echo "built: $out"
file "$out"
ls -l "$out"
