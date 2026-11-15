#!/bin/sh
# monta q4k.comp / q6k.comp a partir dos pedacos e compila (gera tambem as variantes com acumulador fp16: q4k16.h, q6k16.h)
G=C:/source/katana/ref/vk/glslang/bin/glslang.exe
for t in q4k q6k; do
  f=$([ $t = q4k ] && echo fillq4.inc || echo fillq6.inc)
  { cat head.inc; [ $t = q4k ] && echo '#define SB(k) ((hw[(k) >> 2] >> (((k) & 3) * 8)) & 255u)'; cat mainhead.inc
    awk -v F=$f '/FILLA/{while((getline l < F)>0) print l; next} {print}' tail.inc; } > $t.comp
  $G -V --target-env vulkan1.3 --vn ${t}_spv -o ${t}.h $t.comp || exit 1
  $G -V --target-env vulkan1.3 -DACC16 --vn ${t}16_spv -o ${t}16.h $t.comp || exit 1
done
$G -V --target-env vulkan1.3 --vn red_spv -o red.h red.comp || exit 1
