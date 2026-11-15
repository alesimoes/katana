#!/bin/sh
# regenera os cabecalhos SPIR-V (mv_*.h) a partir dos .comp
cd "$(dirname "$0")"
for n in mv_f32 mv_q8 mv_q4k mv_q5k mv_q6k; do
  /c/source/katana/ref/vk/glslang/bin/glslang.exe -V --target-env vulkan1.2 --vn ${n}_spv -o ${n}.h ${n}.comp || exit 1
done
