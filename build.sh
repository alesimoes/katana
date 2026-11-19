#!/bin/sh
# Compila o katana no Linux com gcc (AVX2). Usa ~/.local/tc se nao houver gcc no sistema.
cd "$(dirname "$0")"
CC=${CC:-gcc}; EXTRA=""
if ! command -v "$CC" >/dev/null 2>&1; then
  R="$HOME/.local/tc/root"; CC="$R/usr/bin/gcc-15"; export PATH="$R/usr/bin:$PATH"
  export LD_LIBRARY_PATH="$R/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  EXTRA="-isystem $R/usr/include -isystem $R/usr/include/x86_64-linux-gnu -B$R/usr/lib/x86_64-linux-gnu -L$R/usr/lib/x86_64-linux-gnu"
fi
exec "$CC" -O2 -mavx2 -mfma -mf16c -ffast-math -Wall -Wno-unused -I . -I src -I vk/include $EXTRA src/katana.c -o katana -lm -lpthread -ldl "$@"
