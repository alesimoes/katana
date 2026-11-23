#!/bin/sh
# Compila o katana no Linux com gcc (AVX2). Usa ~/.local/tc se nao houver gcc no sistema.
cd "$(dirname "$0")"
CC=${CC:-gcc}; EXTRA=""
if ! command -v "$CC" >/dev/null 2>&1; then
  R="$HOME/.local/tc/root"; CC="$R/usr/bin/gcc-15"; export PATH="$R/usr/bin:$PATH"
  export LD_LIBRARY_PATH="$R/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  EXTRA="-isystem $R/usr/include -isystem $R/usr/include/x86_64-linux-gnu -B$R/usr/lib/x86_64-linux-gnu -L$R/usr/lib/x86_64-linux-gnu"
fi
V=$(git log -1 --format=%h 2>/dev/null); [ -n "$(git status --porcelain -uno 2>/dev/null)" ] && V="$V+"
# a pagina do katana web (web/ e os .js do desktop) embutida no binario: src/app/web.inc (docs/web.md)
sh tools/genweb.sh || exit 1
# ultimas mudancas (assuntos dos commits) embutidas no binario: katana version --changes; a tela de abertura do desktop mostra
{ git log -6 --format=%s 2>/dev/null | cut -c1-120 | sed 's/\\/\\\\/g; s/"/\\"/g; s/^/"- /; s/$/\\n"/'; echo '""'; } > src/app/changes.inc
exec "$CC" -DKATANA_BUILD="\"${V:-?} $(date +%Y-%m-%d)\"" -O2 -mavx2 -mfma -mf16c -ffast-math -Wall -I . -I src -I vk/include $EXTRA src/katana.c -o katana -lm -lpthread -ldl "$@"
