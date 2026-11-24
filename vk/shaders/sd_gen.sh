#!/bin/sh
# regenera os cabecalhos SPIR-V do Stable Diffusion (render/sdvk.c): sd_*.h a partir de sd_*.comp
cd "$(dirname "$0")"
G=$(command -v glslang || command -v glslangValidator)
c() { n=$1; f=$2; shift 2; $G -V --target-env vulkan1.2 "$@" --vn ${n}_spv -o $n.h $f || exit 1; }
c sd_flash40 sd_flash.comp -DHDV=40 -DLNV=8; c sd_flash80 sd_flash.comp -DHDV=80 -DLNV=16; c sd_flash160 sd_flash.comp -DHDV=160 -DLNV=32
for n in conv gn ln gemv sm tr; do c sd_$n sd_$n.comp; done
c sd_geglu sd_ew.comp -DMODE=0; c sd_cat sd_ew.comp -DMODE=1; c sd_add sd_ew.comp -DMODE=2
echo gen ok
