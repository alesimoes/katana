#!/bin/sh
# Regenera src/engine/cu_kern.inc (PTX dos kernels de tools/cuda/kern.cu: FLUX) e src/engine/cu_sdkern.inc (tools/cuda/sdkern.cu: Stable
# Diffusion). Ferramenta de desenvolvimento: precisa do NVRTC (NVRTC=/caminho/libnvrtc.so.13) e do python3; o katana em si nao precisa de nenhum dos dois.
# Uso: gen.sh [kern|sdkern]  (sem argumento, os dois)
set -e
cd "$(dirname "$0")/../.."
NVRTC=${NVRTC:-/mnt/dados/render-ref/lib/nvidia/cu13/lib/libnvrtc.so.13}
cc -o /tmp/ptxgen tools/cuda/ptxgen.c -ldl
gen() {   # fonte.cu saida.inc simbolo
    /tmp/ptxgen "$NVRTC" sm_80 "tools/cuda/$1.cu" > "/tmp/$1.ptx"
    # versao de PTX mais baixa que serve (so mma.sync, ldmatrix e cp.async: 7.0), para aceitar drivers mais antigos
    sed -i 's/^\.version .*/.version 7.0/; /^\/\//d' "/tmp/$1.ptx"
    python3 - "$1" "$2" "$3" <<'PY'
import sys
src, out, sym = sys.argv[1:4]
b = open('/tmp/%s.ptx' % src, 'rb').read() + b'\0'
with open(out, 'w') as f:
    f.write('/* gerado por tools/cuda/gen.sh a partir de tools/cuda/%s.cu: nao editar */\nstatic const unsigned char %s[] = {\n' % (src, sym))
    for i in range(0, len(b), 24): f.write(','.join(str(x) for x in b[i:i+24]) + ',\n')
    f.write('};\n')
PY
}
case "${1:-all}" in
    kern) gen kern src/engine/cu_kern.inc cu_kern_ptx ;;
    sdkern) gen sdkern src/engine/cu_sdkern.inc cu_sdkern_ptx ;;
    *) gen kern src/engine/cu_kern.inc cu_kern_ptx; gen sdkern src/engine/cu_sdkern.inc cu_sdkern_ptx ;;
esac
