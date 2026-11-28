#!/bin/sh
# regenera os cabecalhos SPIR-V a partir dos .comp (glslang nativo, nao o glslang.exe do Windows)
cd "$(dirname "$0")"
G=$(command -v glslang || command -v glslangValidator || echo /home/alexandre/.local/bin/glslang)
$G -V --target-env vulkan1.2 --vn naive_spv -o naive.h naive.comp || exit 1
$G -V --target-env vulkan1.2 --vn tiled_vec_spv -o tiled_vec.h tiled_vec.comp || exit 1
$G -V --target-env vulkan1.2 --vn tiled_int8dp_spv -o tiled_int8dp.h tiled_int8dp.comp || exit 1
echo ok
