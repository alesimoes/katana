#!/bin/sh
# regenera todos os cabecalhos SPIR-V (variantes KV32/KV16 para os kernels que tocam o cache)
cd "$(dirname "$0")"
G=/c/source/katana/ref/vk/glslang/bin/glslang.exe
c() { # nome fonte [defs...]
  n=$1; f=$2; shift 2
  $G -V --target-env vulkan1.2 "$@" --vn ${n}_spv -o $n.h $f || exit 1
}
for n in rms rms_wg rmsh gather moe_sum; do c $n $n.comp; done
for kv in 0 1; do
  s=kv32; [ $kv = 1 ] && s=kv16
  c qkv_post_$s qkv_post.comp -DKV16=$kv
  c att1_$s att1.comp -DKV16=$kv
  c att1f_$s att1f.comp -DKV16=$kv
  c att_scores_$s att_split.comp -DKV16=$kv -DSTAGE=0
  c att_softmax_$s att_split.comp -DKV16=$kv -DSTAGE=1
  c att_mix_$s att_split.comp -DKV16=$kv -DSTAGE=2
done
echo gen ok
