# vkq4k: produto matriz x lote com pesos Q4_K / Q6_K quantizados na VRAM

Y[rows][tokens] = W[rows][cols] * X[cols][tokens]; W fica quantizado no layout do katana
(linhas entrelacadas de 32 em 32: palavra w da linha i em W[(i/32)*wpr*32 + w*32 + i%32]),
X em fp16 transposto (x[col*tokens + token]), acumulador fp32, matrizes cooperativas 16x16x16.

## Algoritmo (q4k.comp / q6k.comp, gerados por gen.sh a partir dos pedacos *.inc)
- Grupo de 128 threads (4 subgrupos), ladrilho 64x64 da saida, BK = 64 colunas por passo,
  cada subgrupo mantem 2x2 acumuladores. Mesma estrutura do prototipo vkgemm.
- Preenchimento de buf_a: thread id -> linha id%32 (e +32), grupo id/32. Leituras de 32 threads
  seguidas sao palavras consecutivas (o entrelacado de 32 linhas deixa tudo coalescido).
- Q4_K: um passo de BK=64 = um par de sub-blocos (2s, 2s+1) = 8 palavras de dados por linha
  (palavra 4+s*8+l: nibble baixo -> sub-bloco 2s, coluna 4l+b; nibble alto -> sub-bloco 2s+1,
  coluna 32+4l+b). Escala/minimo de 6 bits sao decodificados por thread a cada passo a partir
  das 4 palavras de cabecalho (ficam na cache L1); peso = d*sc*q - dmin*m calculado em fp32 e
  gravado como float16 em buf_a. (Nao foi feita a versao com tabela de escalas em memoria
  compartilhada: nao testada.)
- Q6_K: 53 palavras por bloco; um passo = 4 grupos de 16 pesos (3 palavras de 6 bits cada),
  escala int8 vem da palavra 48+s, d (float) da palavra 52. peso = d*sc*(q-32).
- Linhas fora de multiplo de 64: W e preenchido com zeros ate multiplo de 128 linhas (como o
  katana) e Y tem linhas de sobra; o harness le so as `rows` primeiras. Lote: X e preenchido
  com zeros ate multiplo de 64 (a taxa "com pad" conta as colunas de preenchimento).
- Divisao de K (split): gl_WorkGroupID.z processa uma fatia de colunas (multiplo de 256) e grava
  numa fatia propria de Y; red.comp soma as fatias num segundo dispatch. Util quando ha poucos
  ladrilhos de linhas (K longo ou lote pequeno).

## Verificacao
O harness gera a matriz aleatoria direto no formato de bloco do GGUF, repacota como o katana,
calcula a referencia na CPU (double) com pesos exatos e com pesos arredondados a fp16 (como a
GPU os ve) e compara TODAS as saidas. Contra os pesos fp16: erro/rms <= ~1e-4 (erro relativo max
com piso 5% do rms <= 6e-4). Contra os pesos exatos: erro/rms ~1e-3 (so o arredondamento dos
pesos a fp16).

## Medidas (RTX 3050 Laptop; GPU compartilhada com outros processos, entao ha ruido)
Melhor de varias execucoes, T = trilhoes de multiplicacoes-acumulacoes/s (lote real, sem pad):
- Q4_K 8960x1536 lote 64: 0.42 ms, 2.1 T
- Q4_K 8960x1536 lote 256: 1.28 ms, 2.75 T
- Q4_K 1536x8960 lote 256: 1.18 ms (split 4), 2.98 T (sem split 1.45 ms, 2.4 T)
- Q4_K 2048x2048 lote 16 (pad 64): 0.18 ms (split 2), 0.37 T real (1.5 T com pad)
- Q6_K 8960x1536 lote 64: 0.35 ms, 2.5 T
- Q6_K 8960x1536 lote 256: 1.05 ms, 3.35 T
- Q6_K 1536x8960 lote 256: 1.12 ms (split 4), 3.1 T
- Q6_K 2048x2048 lote 16: 0.21 ms, 0.32 T
Para comparar: o prototipo fp16 fazia 1.9-2.6 T.

## O que nao deu certo / ressalvas
- Acumulador fp16 (variante -DACC16): sem ganho de velocidade e erro/rms de 2-6e-2: descartado.
- Split de K ajuda pouco (K longo: 1.45 -> 1.18 ms; lote 16: 0.22 -> 0.18 ms) e atrapalha quando ja
  ha ladrilhos de sobra (8960 linhas).
- Lote 16 e preenchido ate 64: 75% do trabalho dos nucleos tensoriais e desperdicado. Um ladrilho
  BN=16/32 seria o proximo passo (nao feito). BK 32 e outros ladrilhos nao foram testados.
- Escritas em buf_a em halfs soltos (stride 72 halfs) provavelmente tem conflito de banco; nao medido.
- Mexer na coerencia do cache entre execucoes seguidas nao foi tratado: a taxa mede pesos quentes
  na L2 (4 MB de L2 pode reter parte dos pesos pequenos).

## Reconstruir
    sh gen.sh                      (SPIR-V: q4k.h q6k.h q4k16.h q6k16.h red.h)
    cmd //c "C:\source\katana\tools\vkq4k\b.bat"
    vkq4k.exe [acc16] [q4|q6]
