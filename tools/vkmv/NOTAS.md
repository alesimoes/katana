# vkmv: kernels mv_* do katana em Vulkan (um token, NB=1)

Porte dos kernels OpenCL `mv_f32`, `mv_q8`, `mv_q4k`, `mv_q5k`, `mv_q6k` (GPU_SRC do katana.c) para
GLSL compute, com teste de corretude e medida de vazao. Nada do katana.c foi alterado.

## Arquivos
- `mv_common.glsl`: layout, macros (rptr = RPTR, rrow = RROW, fin = FIN, finish = FINISH com PAIRS e reducao de segmentos).
- `mv_f32.comp mv_q8.comp mv_q4k.comp mv_q5k.comp mv_q6k.comp` (+ `mv_k45.glsl`, comum a Q4_K/Q5_K): porte fiel.
- `mv_q4k_alt.comp`: variante experimental (item 3), so Q4_K.
- `mv_*.h`: SPIR-V gerado (`comp.sh` recompila com o glslang de ref/vk).
- `vkmv.c`: harness (blocos GGUF aleatorios -> empacotamento -> referencia em double -> shader -> comparacao -> vazao).
- `b.bat`: compila com MSVC. `ultima_execucao.txt`: saida completa de uma execucao.

## O que foi portado
- Layout identico: linhas entrelacadas de 32, palavra w da linha i em W[(i/32)*wpr*32 + w*32 + i%32];
  GIDX com gs/go; NR linhas por thread (constante de especializacao 0).
- mode: bit 0 soma (residual), bit 1 SwiGLU com out, bit 2 bias, bit 3 par (gate/up em grupos alternados,
  grava silu(gate)*up). No par o kernel e compilado com NR = 2*nr, como o katana faz (kp_*).
- Segmentos de colunas: local_size = {32, nseg} (nseg = constante de especializacao 1, tambem o tamanho local y),
  reducao em memoria compartilhada igual ao FINISH; dispatch (threads/32, 1, 1). nseg escolhido como o gpu_nseg
  (alvo 16384 threads, ate 8 segmentos; VKMV_SEG muda o alvo). f32 nao usa segmentos (igual ao katana).
- h2f substituido por unpackHalf2x16 (conversao exata de float16).
- Empacotamento reimplementado em vkmv.c (Q4_K 36 palavras, Q5_K 44, Q6_K 53 via pack_q6p, Q8_0 9 palavras).
  Q4_0 e Q3_K nao foram testados separadamente (usam o layout Q8 e Q6, ja cobertos).
- NAO portado: lote (NB>1), MOEPRE, kernels fundidos (mv3, rd_q45...), bsum.

## Corretude (2 formatos de erro)
Referencia: dequantizacao independente a partir dos blocos GGUF, produto em double.
Entradas aleatorias; saida inicial de `out` aleatoria para os modos 1 e 2. Casos: 1536x1536; 256x1536
(com varredura de nseg = 1,2,3,6 alem do automatico); 8960x1536 par gate/up; 1536x8960 modo 1+bias;
1536x1536 com gs=2 go=1 (le a segunda matriz de um buffer intercalado); 1536x1536 modo 2+bias; todos com nr=1 e 2.
Resultado: 106 execucoes, 0 falhas. Piores erros: relativo (|d|/(|ref|+0.01 rms)) 1.9e-4 em f32 (soma sequencial de 8960
termos), <= 6e-5 nos quantizados; erro absoluto/rms <= 2.6e-5 (f32) e <= 4.5e-6 (quantizados).
Criterio de aprovacao: erro absoluto/rms < 1e-4 e relativo < 1e-3 por saida.

## Vazao (RTX 3050 Laptop, G pesos/s, melhor de 3 execucoes; 150 despachos por submissao com barreira entre eles,
pesos copiados ate 160 MB para sair da L2; inclui o custo de lancar/drenar cada despacho)
| caso | f32 | Q8 | Q4_K | Q5_K | Q6_K | Q4_K alt |
| 1536x1536 | 16 | 136 | 136 | 117 | 150 | 138 |
| 256x1536 | 3.1 | 47 | 27 | 25 | 35 | 69 |
| 8960x1536 par | 41 | 158 | 231 | 161 | 202 | 193 |
| 1536x8960 modo 1+bias | 19 | 156 | 173 | 145 | 184 | 193 |
(melhor nr de cada celula; nr=1 foi melhor ou igual quase sempre, exceto no par, onde nr=2 ganha ate 5%).
Referencia OpenCL (`KATANA_DEBUG=1 katana bench ... 16 16`, com sincronizacao por kernel, mistura de matrizes
da geracao): mv_q4k ~93 G/s, mv_q6k ~93 G/s no perfil de geracao; ~180 G/s nos kernels de par (contam 2 matrizes).
Nao sao a mesma metodologia: a medida Vulkan nao sincroniza a cada kernel. `katana micro` mede so a CPU.

## Variante alternativa (Q4_K)
Um subgrupo (32 threads) por linha, 8 threads por bloco de 256 pesos (cada uma uma carga uvec4 de nibbles),
subgroupAdd, RPW subgrupos por grupo de trabalho, layout por linha (bytes do GGUF, sem entrelacar 32 linhas).
Ganha muito em poucas linhas (256x1536: 69 contra 27 G/s), um pouco nas matrizes "down" (193 contra 173),
empata em 1536x1536 e perde no par gate/up (193 contra 231). Passa nas mesmas verificacoes.
Exige subgrupo de 32 (conferido: a placa informa 32; o programa nao testa isso). Nao foi feita variante com modo gs/go.

## Em aberto
- Medir o custo real de lancamento em uma cadeia de grafo (vkCmdDispatch em um unico command buffer com o resto do
  modelo); aqui a barreira entre despachos esta incluida.
- Vazao em lote (NB>1), MoE e kernels fundidos nao portados.
- Alt para Q5_K/Q6_K/Q8 e para o modo par com layout entrelacado por 32 linhas (para manter coalescencia).
- Precisao com -cl-fast-relaxed-math nao e comparada (aqui Vulkan usa float32 estrito do driver).

## Reconstruir e rodar
    sh tools/vkmv/comp.sh
    cmd /c "cd /d C:\source\katana\tools\vkmv && .\b.bat"
    tools\vkmv\vkmv.exe          (todas as verificacoes + vazao; -q so corretude; -t q4k so um tipo; -n sem a variante alt)
