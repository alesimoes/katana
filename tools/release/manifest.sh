#!/bin/sh
# Imprime sha256 e tamanho em bytes de cada arquivo que sera hospedado (formato: sha256  bytes  caminho relativo a models/).
# Uso: sh tools/release/manifest.sh [pasta models]   (arquivos grandes: leva alguns minutos)
M=${1:-models}
[ -d "$M" ] || { echo "pasta de modelos nao encontrada: $M" >&2; exit 1; }
for f in "$M"/flux2klein/gguf/*.gguf "$M"/render/sd15-*.gguf "$M"/embeddinggemma-2/embeddinggemma-2-q8_0.gguf; do
  [ -f "$f" ] || { echo "faltando: $f" >&2; exit 1; }
  h=$(sha256sum "$f" | cut -d' ' -f1)
  b=$(wc -c < "$f" | tr -d ' ')
  printf '%s  %s  %s\n' "$h" "$b" "${f#"$M"/}"
done
