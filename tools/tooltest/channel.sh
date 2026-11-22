#!/bin/sh
# Canal MCP (docs/channels.md): compila tools/tooltest/channel_test.c (servidor falso, professor de mentira e cenarios, tudo
# em C) e roda os cenarios com o modelo minusculo, num HOME temporario. Nada sai da maquina.
# Uso: sh tools/tooltest/channel.sh /caminho/do/katana [cenario]   (a partir da raiz do repositorio)
K=$1
[ -x "$K" ] || { echo "uso: $0 <binario katana> [cenario]"; exit 2; }
K=$(cd "$(dirname "$K")" && pwd)/$(basename "$K")
T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM
CC=${CC:-gcc}; X=""
if ! command -v "$CC" >/dev/null 2>&1; then
  R="$HOME/.local/tc/root"; CC="$R/usr/bin/gcc-15"; export PATH="$R/usr/bin:$PATH"
  export LD_LIBRARY_PATH="$R/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  X="-isystem $R/usr/include -isystem $R/usr/include/x86_64-linux-gnu -B$R/usr/lib/x86_64-linux-gnu -L$R/usr/lib/x86_64-linux-gnu"
fi
"$CC" -O1 -Wall -Wno-unused -Wno-format-truncation $X tools/tooltest/channel_test.c -o "$T/channel_test" -lutil || exit 2
python3 tools/tinymodel/make_tiny_gguf.py "$T/agent.gguf" --template chatml-xml >/dev/null || exit 2
unset KATANA_MCP_CONFIG KATANA_MCP_CHANNEL KATANA_MCP_STRICT KATANA_MCP_TRUST KATANA_MCP_TIMEOUT KATANA_CONFIRM KATANA_TRUST KATANA_UI
"$T/channel_test" run "$K" "$T/agent.gguf" "$T" ${2:+"$2"}
