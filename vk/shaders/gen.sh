#!/bin/sh
# regenera os cabecalhos SPIR-V a partir dos .comp (mv_*, rms_wg, att1f em duas variantes de cache KV)
cd "$(dirname "$0")"
G=/c/source/katana/ref/vk/glslang/bin/glslang.exe
for n in mv_f32 mv_q8 mv_q4k mv_q5k mv_q6k rms_wg; do
  $G -V --target-env vulkan1.2 --vn ${n}_spv -o ${n}.h ${n}.comp || exit 1
done
$G -V --target-env vulkan1.2 -DKV16=0 --vn att1f_kv32_spv -o att1f_kv32.h att1f.comp || exit 1
$G -V --target-env vulkan1.2 -DKV16=1 --vn att1f_kv16_spv -o att1f_kv16.h att1f.comp || exit 1
echo gen ok
