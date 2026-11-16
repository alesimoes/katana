#!/bin/sh
# gera os cabecalhos SPIR-V da bancada
cd "$(dirname "$0")"
G=${GLSLANG:-$HOME/.local/tc/gl/root/usr/bin/glslang}
for n in dn_step dn_prep dn_cstate dn_chunk dn_state dn_norm; do
  $G -V --target-env vulkan1.3 --vn ${n}_spv -o $n.h $n.comp || exit 1
done
echo gen ok
