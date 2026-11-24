#!/bin/sh
# Regenera src/engine/cu_kern.inc (PTX dos kernels de tools/cuda/kern.cu). Ferramenta de desenvolvimento: precisa do NVRTC
# (NVRTC=/caminho/libnvrtc.so.13) e do python3; o katana em si nao precisa de nenhum dos dois.
set -e
cd "$(dirname "$0")/../.."
NVRTC=${NVRTC:-/mnt/dados/render-ref/lib/nvidia/cu13/lib/libnvrtc.so.13}
cc -o /tmp/ptxgen tools/cuda/ptxgen.c -ldl
/tmp/ptxgen "$NVRTC" sm_80 tools/cuda/kern.cu > /tmp/kern.ptx
# versao de PTX mais baixa que serve (so mma.sync, ldmatrix e cp.async: 7.0), para aceitar drivers mais antigos
sed -i 's/^\.version .*/.version 7.0/; /^\/\//d' /tmp/kern.ptx
python3 - <<'PY'
b = open('/tmp/kern.ptx', 'rb').read() + b'\0'
with open('src/engine/cu_kern.inc', 'w') as f:
    f.write('/* gerado por tools/cuda/gen.sh a partir de tools/cuda/kern.cu: nao editar */\nstatic const unsigned char cu_kern_ptx[] = {\n')
    for i in range(0, len(b), 24): f.write(','.join(str(x) for x in b[i:i+24]) + ',\n')
    f.write('};\n')
PY
