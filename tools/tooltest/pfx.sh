#!/bin/sh
# O prefixo gravado em disco so vale para o mesmo arquivo de modelo: sh tools/tooltest/pfx.sh ./katana
# (dois arquivos do mesmo tamanho com pesos diferentes, como duas fusoes da mesma base, e um arquivo regravado)
D=$(cd "$(dirname "$0")/../.." && pwd)
K=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
[ -x "$K" ] || { echo "uso: $0 <katana>"; exit 2; }
T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT
export HOME="$T/home" KATANA_CACHE_DIR="$T/cache" KATANA_CPU=1 KATANA_CTX=16000 KATANA_LOG="$T/log" KATANA_WEB=0 KATANA_MAXTOK=2
mkdir -p "$HOME" "$T/w" "$T/cache"; : > "$T/log"; cd "$T/w" || exit 2   # pasta pai com o mesmo conteudo nas duas execucoes: a listagem entra no prompt
python3 "$D/tools/tinymodel/make_tiny_gguf.py" "$T/a.gguf" --arch qwen35moe --template chatml-xml > /dev/null || exit 2
# b: o mesmo arquivo com os primeiros 256 pesos dos dados somados de 0.5 (blk.0 ou token_embd; o output_norm nao muda)
python3 - "$T/a.gguf" "$T/b.gguf" <<'PY'
import sys, struct
d = bytearray(open(sys.argv[1], "rb").read())
i = d.find(b"blk.0.attn_qkv.weight"); assert i > 0
p = i + len(b"blk.0.attn_qkv.weight"); nd = struct.unpack_from("<I", d, p)[0]
off = struct.unpack_from("<Q", d, p + 4 + 8 * nd + 4)[0]
p = d.rfind(b".weight") + 7; nd = struct.unpack_from("<I", d, p)[0]; end = p + 4 + 8 * nd + 12
base = (end + 31) // 32 * 32
for k in range(256):
    struct.pack_into("<f", d, base + off + 4 * k, struct.unpack_from("<f", d, base + off + 4 * k)[0] + 0.5)
open(sys.argv[2], "wb").write(d)
PY
F=0
run() { OUT=$(printf 'oi\n/exit\n' | timeout 60 "$K" agent "$1" 2>&1); }
has() {
    case "$OUT" in *"loaded from the cache"*) R=sim;; *"agent warm: system prompt"*) R=nao;; *) R="sem aviso";; esac
    if [ "$R" = "$2" ]; then echo "ok   $1: do cache = $R"; else echo "FAIL $1: do cache = $R, esperava $2"; F=1; fi
}
run "$T/a.gguf"; has "primeira vez" nao
run "$T/a.gguf"; has "mesmo modelo" sim
touch -d '2031-01-01' "$T/b.gguf"
run "$T/b.gguf"; has "outro arquivo do mesmo tamanho" nao
cp "$T/b.gguf" "$T/a.gguf"; touch -d '2030-01-01' "$T/a.gguf"
run "$T/a.gguf"; has "arquivo regravado no mesmo caminho" nao
[ $F = 0 ] && echo OK || { echo FAILED; exit 1; }
