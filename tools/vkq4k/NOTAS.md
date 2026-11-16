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

---
# Versao 2 (vkmm): ladrilhos estreitos, bordas, mais tipos, modos de saida

Arquivos: mm.comp (um shader parametrizado por -D), red2.comp (soma das fatias de K, com modo soma),
cvt.comp (fp32 -> fp16), gen2.sh (gera spv/*.spv, uma variante por tipo/ladrilho/x32/tabela/glu),
vkmm.c (harness), b2.bat (compila), best.sh (resume varreduras: melhor linha por forma/lote).
O codigo antigo (vkq4k.c, q4k.comp...) foi mantido sem mudanca.

## O que existe
- Tipos: Q4_K, Q6_K, Q5_K (44 palavras), Q8_0 (9 palavras; serve para Q4_0 reempacotado), F32 (pesos
  viram fp16 ao encher o ladrilho).
- Ladrilhos (BMxBN, subgrupos): 64x64 (2x2), 128x16 (4x1), 64x16 (2x1, 64 threads), 128x32 (4x1),
  64x32 (2x1, 64 threads), 64x16w4 (4x1, acumulador 16x16 por subgrupo), 64x32w4 (4x1).
- Bordas: linhas e lote qualquer. Pesos so precisam de padding ate multiplo de 32 linhas (grupos
  inteiros; grupo fora de `rows` nao e lido, vira zero). Ativacoes e saida com teste de limite
  (lote nao multiplo de 4 usa leitura escalar). Ladrilho de borda grava via memoria compartilhada.
  cols tem de ser multiplo de 256 (divisao de K em multiplos de 256).
- Modos de saida: plain, add (y += W*x, push constant `add`), glu (buffer com gate/up intercalados
  por grupo de 32 linhas, gs=2: saida silu(gate*x)*(up*x); gate e up da mesma linha ficam no
  mesmo subgrupo via permutacao de linhas do ladrilho; so ladrilhos com CR par, nao os "w4").
  Divisao de K nao existe para glu; com add a soma e feita em red2.
- Ativacoes: -x 0 (fp16 pronto), -x 1 (fp32 lido direto, converte ao encher), -x cvt (fp32 -> fp16
  num despacho antes, incluido no tempo).

## Licao importante
O primeiro 64x64 generico ficou 2x mais lento (1.4 T) so por causa do epilogue de borda: os lacos
sobre os acumuladores nao eram desenrolados e o vetor acc foi para memoria local. `[[unroll]]`
(GL_EXT_control_flow_attributes) resolveu: 64x64 lote 256 8960x1536 = 2.8-2.9 T (antes 2.7).

## Verificacao (todas as saidas contra a CPU, pesos arredondados a fp16, limite erro relativo 1e-3)
- Formas 8960x1536, 1536x8960, 2048x2048; lotes 4..512; 5 tipos; 7 ladrilhos; split 1 e 4: 1400 OK, 0 falhas.
- Bordas: linhas 1537 e 8900, lotes 1,3,17,70,100, 5 tipos, 7 ladrilhos, splits 1 e 2, nos modos plain,
  add (700 cada) e glu (250): tudo OK. Erro/rms tipico 1e-5..1e-4.
- Tambem: -x 1, -x cvt e -sc 1 em formas menores: OK.

## Medidas (T mult/s uteis, melhor de varias rodadas, GPU compartilhada: +-10% de ruido)
Q4_K (melhor ladrilho / 64x64 sem split):
| lote | 8960x1536 | 1536x8960 | 2048x2048 |
| 4 | 0.44 (64x16w4) / 0.14 | 0.45 (64x16w4 sp4) | 0.34 (64x16w4 sp4) |
| 8 | 0.87 / 0.36 | 0.87 | 0.69 |
| 16 | 1.69 (64x16w4) / 0.71 | 1.77 (64x16w4 sp4) | 1.29 (128x16 sp4) |
| 32 | 2.70 (64x32w4) / 1.40 | 2.37 (128x32 sp4) | 2.09 (64x32w4 sp4) |
| 64 | 2.72 (64x64) | 2.79 (64x64 sp4) | 2.70 (64x32w4) |
| 128 | 2.73 (64x64) | 2.67 (64x64 sp4) | 2.85 (64x64) |
| 256 | 2.82 (64x64) | 2.74 (64x64 sp4) | 2.85 |
| 512 | 2.87 (64x64) | 2.80 (64x64 sp4) | 2.76 |
Q6_K: lote 16 2.05 / 2.17 / 1.54; lote 64 3.49 (64x32w4) / 3.35 / 3.18; lote 256 3.19-3.40; lote 512 3.2-3.3.
Q8_0 8960x1536: lote 16 1.82, 32 2.98, 64 3.04, 256 3.19. Q5_K: lote 16 1.45, 64 2.55, 256 2.70.
F32 (limitado por banda, 55 MB): lote 16 0.70, 64 2.4, 256 2.46.
Lotes pequenos (4-8) ficam em ~0.1 ms para 8960x1536 Q4_K: limitados por latencia/decodificacao, nao
pelos nucleos tensoriais (banda seria ~0.04 ms).
Add e glu custam o mesmo que plain (glu 8960x1536 Q4 lote 256: 2.61 T contando gate e up).

## Regra de escolha recomendada
- lote <= 16: 64x16w4 (ou 128x16 com split 4 quando ha poucos ladrilhos de linhas, p.ex. 2048 linhas ou K longo)
- lote 17..32: 64x32w4 (128x32 sp4 se K longo)
- lote 33..64: 64x32w4 (Q6/Q8) ou 64x64 (Q4/Q5/F32); com K >= 8000 usar split 4
- lote >= 65: 64x64; Q6_K/Q8_0 aceitam 64x32w4 / 128x32 com ganho de ~5%
- divisao de K (2 ou 4) so quando linhas/ladrilho < ~100 grupos e K longo ou lote pequeno.
Lote 1: a regra serve (64x16w4), mas o produto por token dedicado (vkmv) deve ser melhor; nao comparado.

## Rejeitado / resultados negativos
- Tabela de escala/minimo em memoria compartilhada (-sc 1, Q4_K/Q5_K): mais lenta em todos os ladrilhos
  (ex.: 64x64 lote 256 2.54 contra 2.79 T; 128x32 1.74 contra 2.31): barreira extra e a decodificacao por
  thread ja e barata. Mantida so como opcao de compilacao.
- Ativacao fp32 lida direto: 2-6% mais lenta que fp16 pronto; fp32 + despacho de conversao: empate dentro do
  ruido com fp16 pronto. Recomendacao: converter uma vez por vetor de entrada quando varios produtos o
  compartilham (q/k/v, gate/up); fp32 direto quando so um produto usa a entrada.
- Ladrilhos de 2 subgrupos (64x16, 64x32) perdem para os de 4 subgrupos (w4).

## Nao feito
- Rota int8 (tarefa 6): nao tentada.
- O arquivo do vkq4k antigo nao foi alterado. Nao comparado com o produto por token (vkmv).
- Variacao entre rodadas: os numeros de 64x16w4 lote 4-8 e os de split variam ~10%.

## Reconstruir e rodar
    bash gen2.sh                         (spv/*.spv, ~170 variantes)
    cmd //c "C:\source\katana\tools\vkq4k\b2.bat"
    vkmm.exe -t q4,q6 -s 8960x1536 -b 16,64,256 -tiles all -splits 1,4          (confere e mede)
    vkmm.exe -nocheck -t q4 -s 2048x2048 -b 4,8,16,32 -tiles all -splits 1,2,4   (so tempo)
    vkmm.exe -t q4,q6,q5,q8,f32 -s 1537x1536,8900x1536 -b 1,3,17,70,100 -tiles all -mode add|glu|plain
    opcoes: -x 0|1|cvt  -sc 0|1  -reps n ; bash best.sh arquivo resume varreduras

## Linux (2026-11-15): o driver penaliza 4 acumuladores por subgrupo
Driver NVIDIA 595.99 no Ubuntu, mesma placa. SPIR-V identico ao do Windows (conferido byte a byte), mas
todo ladrilho em que um subgrupo carrega 4 acumuladores 16x16 (64x64 2x2, 64x32 2x1, 128x32 4x1, 128x64
4x2) cai para ~1,3 T, em plano e em glu. Com ate 2 acumuladores volta ao nivel do Windows ou acima.
Q4_K/Q6_K 8960x1536 lote 256 (T mult/s): 64x64 2x2 1,30/1,33; 64x64 4x2 ("64x64w8") 2,93/3,19;
128x32 4x2 2,67/3,36; 64x32w4 2,52/3,16. Glu: 64x64 1,32/1,36; 64x32 2x2 ("64x32s22") 2,43/3,02
(tambem lotes 32-128: 2,1-2,4 / 2,9-3,0, contra 1,1-1,3 do 64x32 2x1).
Motor passou a usar: glu acima de 16 tokens 64x32 2x2; plano ate 64 tokens 64x32w4, acima 64x64 4x2.
No Windows a mudanca nao foi medida. vkmm agora compila no Linux (dlopen, clock_gettime) e tem os
ladrilhos 64x64w8, 64x32s22, 128x32w8, 128x64w8.
