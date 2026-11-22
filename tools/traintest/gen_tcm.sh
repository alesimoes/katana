#!/bin/sh
# SPIR-V das medidas (tcm_bench; fora do git): variantes de ladrilho do vk/shaders/tcm.comp e os testes de teto
cd "$(dirname "$0")/../.."
G=/c/source/katana/ref/vk/glslang/bin/glslang.exe
[ -x "$G" ] || G=$(command -v glslang || command -v glslangValidator)
S="$G -V --target-env vulkan1.3"
T=tools/traintest
: > $T/tcm_var.h
v() { $S -DBN=$2 -DSN=$3 -DPREC=$4 -DPAD=$5 -DDUP=$6 --vn v$1_spv -o $T/tcm_tmp.h vk/shaders/tcm.comp >/dev/null || exit 1; grep -v pragma $T/tcm_tmp.h >> $T/tcm_var.h; L="$L, { v$1_spv, sizeof v$1_spv, $2, $3, $4, $5, $6 }"; }
#  n bn sn prec pad dup
v 1 16 1 0 8 0; v 2 16 1 0 8 1; v 3 32 1 0 8 0; v 4 64 2 0 8 0; v 5 16 1 1 8 0; v 6 16 1 0 0 0; v 7 64 1 0 8 0; v 8 32 2 0 8 0; v 9 64 2 1 8 0; v 10 16 1 2 8 0; v 11 32 1 2 8 0; v 12 32 2 2 8 0; v 13 64 2 2 8 0
echo "static const struct { const uint32_t *s; size_t n; int bn, sn, prec, pad, dup; } Vr[] = { { 0 }$L };
#define NVR ((int)(sizeof Vr / sizeof Vr[0]))" >> $T/tcm_var.h
rm -f $T/tcm_tmp.h
for p in "tcm_peak tcm_peak_spv" "tcm_peak2 tcm_peak2_spv" "tcm8 tcm8_spv"; do set -- $p; $S --vn $2 -o $T/$1.h $T/$1.comp || exit 1; done
$S -DI8 --vn tcm_peak8_spv -o $T/tcm_peak8.h $T/tcm_peak.comp || exit 1
