#!/bin/sh
# regenera os cabecalhos SPIR-V a partir dos .comp (mv_*, rms_wg, att1f em duas variantes de cache KV)
cd "$(dirname "$0")"
G=/c/source/katana/ref/vk/glslang/bin/glslang.exe
[ -x "$G" ] || G=$(command -v glslang || command -v glslangValidator)
for n in mv_f32 mv_q8 mv_q4k mv_q5k mv_q6k rms_wg moe_sum; do
  $G -V --target-env vulkan1.2 --vn ${n}_spv -o ${n}.h ${n}.comp || exit 1
done
$G -V --target-env vulkan1.2 -DKV16=0 --vn att1f_kv32_spv -o att1f_kv32.h att1f.comp || exit 1
$G -V --target-env vulkan1.2 -DKV16=1 --vn att1f_kv16_spv -o att1f_kv16.h att1f.comp || exit 1
$G -V --target-env vulkan1.2 --vn att1r_spv -o att1r.h att1r.comp || exit 1
# ---- prompt em lote (Vulkan): produto em matriz cooperativa (mm.comp), so as variantes que o motor usa
# tipos WT: 0 Q4_K, 1 Q6_K, 2 Q5_K, 3 Q8_0, 4 F32; ladrilhos: 64x64 (2x2), 64x32w4 e 64x16w4 (4 subgrupos, sem glu),
# 64x32 e 64x16 (com glu). No 64x64 plano 4x2 subgrupos e no 64x32 glu 2x2: cada subgrupo fica com
# no maximo 2 acumuladores 16x16 (com 4, o driver Linux cai a metade; tools/vkq4k/NOTAS.md). Ativacao em fp16 (XF32=0), sem tabela de escalas.
# Os glu gravam a saida em fp16 (YH=1): ela e a entrada do produto down, sem passo de conversao.
mm() { # tipo nome BM BN SM SN glu
  $G -V --target-env vulkan1.3 -DWT=$1 -DBM=$3 -DBN=$4 -DSM=$5 -DSN=$6 -DXF32=0 -DSCTAB=0 -DBR=1 -DGLU=$7 -DYH=$7 --vn mm_t$1_$2_g$7_spv -o mm_t$1_$2_g$7.h mm.comp > /dev/null || { echo FALHOU mm $*; exit 1; }
}
for t in 0 1 2 3 4; do
  mm $t 64x64 64 64 4 2 0 & mm $t 64x64 64 64 2 2 1 & mm $t 64x32w4 64 32 4 1 0 & mm $t 64x32 64 32 2 2 1 & mm $t 64x16w4 64 16 4 1 0 & mm $t 64x16 64 16 2 1 1 &
  wait
done
$G -V --target-env vulkan1.3 --vn cvt_spv -o cvt.h cvt.comp || exit 1
$G -V --target-env vulkan1.2 --vn rms_wg16_spv -o rms_wg16.h rms_wg16.comp || exit 1
$G -V --target-env vulkan1.2 --vn gather_spv -o gather.h gather.comp || exit 1
for kv in 0 1; do
  s=kv32; [ $kv = 1 ] && s=kv16
  $G -V --target-env vulkan1.2 -DKV16=$kv --vn qkv_post_${s}_spv -o qkv_post_$s.h qkv_post.comp || exit 1
  $G -V --target-env vulkan1.2 -DKV16=$kv --vn att_flash_${s}_spv -o att_flash_$s.h att_flash.comp || exit 1
  $G -V --target-env vulkan1.2 -DKV16=$kv -DSTAGE=0 --vn att_scores_${s}_spv -o att_scores_$s.h att_split.comp || exit 1
  $G -V --target-env vulkan1.2 -DKV16=$kv -DSTAGE=1 --vn att_softmax_${s}_spv -o att_softmax_$s.h att_split.comp || exit 1
  $G -V --target-env vulkan1.2 -DKV16=$kv -DSTAGE=2 --vn att_mix_${s}_spv -o att_mix_$s.h att_split.comp || exit 1
done
echo gen ok
