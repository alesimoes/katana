#!/bin/sh
# regenera os cabecalhos SPIR-V a partir dos .comp (mv_*, rms_wg, att1f em duas variantes de cache KV)
cd "$(dirname "$0")"
G=/c/source/katana/ref/vk/glslang/bin/glslang.exe
[ -x "$G" ] || G=$(command -v glslang || command -v glslangValidator)
for n in mv_f32 mv_q8 mv_q4k mv_q5k mv_q6k mv_q2k rms_wg moe_sum moe_sum_rms moe_router; do
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
# variantes precisas, so planas, para os produtos sensiveis ao arredondamento: _ws (pesos Q8_0/F32 em alta + resto fp16)
# e _xs (ativacao fp32 em alta + resto, e tambem pesos em alta + resto)
mmx() { # tipo nome BM BN SM SN sufixo XS
  $G -V --target-env vulkan1.3 -DWT=$1 -DBM=$3 -DBN=$4 -DSM=$5 -DSN=$6 -DXF32=$8 -DXS=$8 -DWS=1 -DSCTAB=0 -DBR=1 -DGLU=0 -DYH=0 --vn mm_t$1_$2_$7_spv -o mm_t$1_$2_$7.h mm.comp > /dev/null || { echo FALHOU mmx $*; exit 1; }
}
mmg() { # glu preciso (xs + ws), saida fp32: tipo nome BM BN SM SN
  $G -V --target-env vulkan1.3 -DWT=$1 -DBM=$3 -DBN=$4 -DSM=$5 -DSN=$6 -DXF32=1 -DXS=1 -DWS=1 -DSCTAB=0 -DBR=1 -DGLU=1 -DYH=0 --vn mm_t$1_$2_gx_spv -o mm_t$1_$2_gx.h mm.comp > /dev/null || { echo FALHOU mmg $*; exit 1; }
}
for t in 0 1 2 3 4; do
  mmx $t 64x64 64 64 4 2 xs 1 & mmx $t 64x32w4 64 32 4 1 xs 1 & mmx $t 64x16w4 64 16 4 1 xs 1 &
  mmx $t 64x64 64 64 4 2 ws 0 & mmx $t 64x32w4 64 32 4 1 ws 0 & mmx $t 64x16w4 64 16 4 1 ws 0 &
  mmg $t 64x64 64 64 2 2 & mmg $t 64x32 64 32 2 2 & mmg $t 64x16 64 16 2 1 &
  mm $t 64x64 64 64 4 2 0 & mm $t 64x64 64 64 2 2 1 & mm $t 64x32w4 64 32 4 1 0 & mm $t 64x32 64 32 2 2 1 & mm $t 64x16w4 64 16 4 1 0 & mm $t 64x16 64 16 2 1 1 &
  wait
done
$G -V --target-env vulkan1.3 --vn cvt_spv -o cvt.h cvt.comp || exit 1
$G -V --target-env vulkan1.2 --vn rms_wg16_spv -o rms_wg16.h rms_wg16.comp || exit 1
$G -V --target-env vulkan1.2 -DOF32 --vn rms_wg32_spv -o rms_wg32.h rms_wg16.comp || exit 1
$G -V --target-env vulkan1.2 --vn gather_spv -o gather.h gather.comp || exit 1
for kv in 0 1; do
  s=kv32; [ $kv = 1 ] && s=kv16
  $G -V --target-env vulkan1.2 -DKV16=$kv --vn qkv_post_${s}_spv -o qkv_post_$s.h qkv_post.comp || exit 1
  $G -V --target-env vulkan1.2 -DKV16=$kv --vn att_flash_${s}_spv -o att_flash_$s.h att_flash.comp || exit 1
  $G -V --target-env vulkan1.2 -DKV16=$kv -DSTAGE=0 --vn att_scores_${s}_spv -o att_scores_$s.h att_split.comp || exit 1
  $G -V --target-env vulkan1.2 -DKV16=$kv -DSTAGE=1 --vn att_softmax_${s}_spv -o att_softmax_$s.h att_split.comp || exit 1
  $G -V --target-env vulkan1.2 -DKV16=$kv -DSTAGE=2 --vn att_mix_${s}_spv -o att_mix_$s.h att_split.comp || exit 1
done
# ---- qwen35moe: Gated DeltaNet (geracao e lote em blocos, de tools/vkdelta), portao da atencao e MoE em lote
for n in dn_step dn_seq dn_prep dn_cstate dn_chunk dn_state dn_norm; do
  $G -V --target-env vulkan1.3 --vn ${n}_spv -o $n.h $n.comp || exit 1
done
for n in qn_gate moe_gath moe_scat; do
  $G -V --target-env vulkan1.2 --vn ${n}_spv -o $n.h $n.comp || exit 1
done
$G -V --target-env vulkan1.2 -DF32 --vn moe_gath32_spv -o moe_gath32.h moe_gath.comp || exit 1
$G -V --target-env vulkan1.2 -DF32 --vn qn_gate32_spv -o qn_gate32.h qn_gate.comp || exit 1
for kv in 0 1; do
  s=kv32; [ $kv = 1 ] && s=kv16
  $G -V --target-env vulkan1.2 -DKV16=$kv -DOF32 --vn att_flash32_${s}_spv -o att_flash32_$s.h att_flash.comp || exit 1
done
echo gen ok
