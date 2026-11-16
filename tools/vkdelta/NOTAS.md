# vkdelta: kernels Vulkan da camada linear Gated DeltaNet (qwen35moe)

Bancada isolada (nao mexe no katana.c). Referencia em double na propria bancada (recorrencia token a token, com
convolucao, normas e portao); a GPU e conferida contra ela.

## Reconstruir e rodar (Linux)
    sh gen.sh        (shaders -> .h, glslang de ~/.local/tc/gl, --target-env vulkan1.3)
    sh b.sh          (gcc-15 de ~/.local/tc, -I ../../vk/include, -ldl)
    ./vkdelta        (conferencia + tempos)   -c so conferencia, -t so tempos, -B lote (padrao 256), -r rodadas, -k 0|1

## Dimensoes e convencoes
dim 2048; qkv 8192 canais = q 16x128 | k 16x128 | v 32x128; z 4096; a, b 32; estado S[32][128][128] fp32 (i = dim de k,
j = dim de v, j contiguo). Cabeca h usa o grupo q/k `h/2` (`kmap 0`, padrao) ou `h%16` (`kmap 1`); as duas conferem, mas
TEM de bater com a referencia da CPU do katana.c (repeticao intercalada ou em mosaico).
- conv1d: `x_t = sum_tau cw[c*4 + 3-tau] * in(t-tau)` (tap 3 = token atual), sem bias, depois SiLU.
- normas L2 de q e k: `x / sqrt(sum x^2 + 1e-6)`; q * 1/sqrt(128).
- `beta = sigmoid(b)`, `g = ssm_a * softplus(a + dt_bias)` com `ssm_a = -exp(A_log)` (como fica no arquivo do modelo).
- `S = exp(g) S; delta = beta (v - S^T k); S += k delta^T; o = S^T q`; saida = rms(o) * w * SiLU(z), eps 1e-6, w de 128
  compartilhado entre cabecas (conferir na CPU se o peso e `w` ou `1+w`).
- Anel da convolucao: `cs[(pos & 3)*8192 + c]` (4 slots indexados pela posicao absoluta). Zerar anel e S no inicio da sequencia.
  Nao ha como "voltar" o estado (reaproveitar prefixo exige guardar copia de S/anel).

## Kernels
| kernel | lancamento | o que faz |
|---|---|---|
| `dn_step` | 32 grupos x 512 | geracao: conv+SiLU+L2+beta/g, recorrencia (S lido e gravado uma vez, thread = coluna j x fatia de 32 linhas), norma com portao. Um lancamento por camada |
| `dn_prep` | 64 unidades x Bp/32, 128 | lote: conv+SiLU+L2, beta, g; lane = token (coalescido no layout transposto do lote) |
| `dn_cstate` | 32 x 256 | grava no anel as ultimas 3 entradas do lote (depois do prep: sem corrida) |
| `dn_chunk` | blocos x 32 cabecas, 256 | por bloco de 64 tokens, em paralelo: q k^T, k k^T (coopmat), T=(I+L)^-1 (substituicao direta em compartilhada fp32), U = T diag(beta) V, Wn = -T diag(beta e^gc) K (coopmat), Qm, qg, kd |
| `dn_state` | 32 cabecas x 8 faixas de 16 colunas, 256 | sequencial nos blocos: V' = U + Wn S; O = qg S + Qm V'; S = e^gl S + kd^T V'. S fica em acumuladores coopmat (1 por subgrupo + 1 temporario = 2, regra do driver Linux) |
| `dn_norm` | 32 x B/32, 128 | rms por cabeca * w * SiLU(z), saida fp16 transposta y[(h*128+j)*B+b] (entrada do produto de saida) |

Forma em blocos (derivada e conferida): com gc = soma acumulada de g no bloco, G_ij = exp(gc_i - gc_j),
L = estrita-inf(beta_i (k_i.k_j) G_ij), T = (I+L)^-1, U = T diag(beta) V, W = T diag(beta e^gc) K:
V' = U - W S0; O = diag(e^gc) Q S0 + inf-com-diag(Q K^T . G) V'; S = e^gc_C S0 + (diag(e^(gc_C - gc)) K)^T V'.
Todos os expoentes sao <= 0 (nada estoura em fp16). Tokens de preenchimento (B ate Bp, multiplo de 64) entram com
q=k=v=0, beta=0, g=0 e nao mexem no estado. Layouts: q/k/v/qg/kd dimensao-major [cabeca][128][Bp] fp16, U [cabeca][Bp][128]
fp32, Wn fp16, Qm [cabeca][bloco][64][64] fp16, o [(h*128+j)*Bp + t] fp32.

## Erro contra a referencia (double), 2026-11-16
Dados aleatorios: qkv cru U(-1.5,1.5), z U(-2,2), a/b U(-2,2), cw U(-0.6,0.6), A_log U(-2,1), dt_bias U(-1,1).
Sequencia: lote 200 (pos 0) -> lote 77 -> lote 2 -> 8 tokens de geracao -> lote 256; e geracao pura de 64 tokens.
| caso | max erro / max |ref| |
|---|---|
| lote: o antes da norma | 6e-4 a 1,0e-3 |
| lote: saida com portao (fp16, inclui o arredondamento fp16 da saida) | 5e-4 a 1,2e-3 |
| lote: estado S ao fim | 5e-4 a 1,0e-3 (nao cresce ao encadear lotes: 5,4e-4 / 8,2e-4 / 6,6e-4 / 9,6e-4) |
| geracao pura (fp32), 64 tokens | 2,7e-7 (saida), 2,2e-7 (S) |
| geracao depois de lotes | 2,9e-4 (herda o erro do lote) |
NC=2 (faixa de 32 colunas, 3 acumuladores) e kmap 1 dao erros iguais. O erro do lote vem dos operandos fp16 dos
produtos (S em fp16 como operando, T em fp16); tudo acumula em fp32.

## Tempos (RTX 3050 Laptop 4 GB, driver Linux; indicativos: GPU compartilhada, melhor de 5-7 rodadas, grafo gravado com
barreira entre despachos)
| | 64 tokens | 256 tokens | 512 tokens |
|---|---|---|---|
| dn_prep | 0,038 ms | 0,12-0,14 | 0,22 |
| dn_chunk | 0,088 | 0,29 | 0,57 |
| dn_state (NC=1) | 0,090 | 0,25 | 0,51 |
| dn_state (NC=2) | | 0,34 | |
| dn_norm | 0,018 | 0,061 | 0,12 |
| lote completo (NC=1) por camada | 0,22 ms (3,4 us/token) | 0,74 ms (2,9 us/token) | 1,47 ms (2,9 us/token) |
| dn_step B vezes | 1,59 ms | 6,28 ms | 12,5 ms |
| ganho da forma em blocos | 7,3x | 8,5x | 8,5x |

Geracao: **dn_step 0,024-0,025 ms por camada** (4 MB lidos+gravados -> ~85 GB/s); 30 camadas lineares ~0,75 ms por token.
Antes de passar o prep/norm para lane = token (layout de lote transposto), prep era 0,73 ms e norm 0,27 ms (leitura com passo B).

Perfil grosseiro do dn_chunk (256 tokens, desligando fases): sem nada alem de prefixo/mascaras/inversao 0,16 ms, inversao
~0,08 ms; os produtos coopmat ~0,07 ms; a escrita de qg/kd ~0,06 ms. Com 64 tokens (32 grupos, uma onda) o grupo leva
~90 us: e uma cadeia serial longa (muitas barreiras), nao banda.

### Proximos passos de desempenho (nao feitos)
- Inversao de T em blocos 16x16 (cada subgrupo inverte um bloco diagonal sem barreira de grupo; blocos fora da diagonal
  por produtos coopmat) no lugar das 63 linhas com 2 barreiras.
- dn_state: carregar Wn/qg/Qm/kd para a compartilhada em vez de coopMatLoad direto da memoria global (cada subgrupo
  faz ~25 cargas globais em serie por bloco); prefetch do bloco seguinte.
- Juntar prep+chunk quando B <= 64 (um bloco so).
- dn_step: 85 GB/s de ~190; dividir as colunas em 2-4 grupos por cabeca (64-128 grupos) com a norma do portao num
  segundo passo, ou S em fp16/bf16 (metade do trafego; precisa medir o erro acumulado em sequencias longas).

## Encaixe no grafo do katana
- Projecoes (matvec na geracao, mm no lote) da entrada normalizada: qkv (8192), z (4096), b (32), a (32). Podem ser uma
  matriz so concatenada; `ab` espera a nas linhas 0..31 e b em 32..63 (no lote: ab[h*B+b], ab[(32+h)*B+b]).
- Geracao: um `dn_step` por camada linear entre as projecoes e a projecao de saida (entrada o[4096] fp32). No grafo gravado
  `pos` precisa vir do buffer meta (meta[0]) em vez de push constant (troca simples: binding extra).
- Lote: `dn_prep`, `dn_cstate`, `dn_chunk`, `dn_state`, `dn_norm` e o mm de saida le y fp16 transposto (igual a att_flash).
  pos0 tambem do meta. Temporarios por lote (Bp=256): qh/kh 1 MB cada, vh/qg/kd/Wn 2 MB, U/o 4 MB, Qm 1 MB; reaproveitaveis
  entre camadas.
- Estado por camada: S 2 MB + anel 128 KB; 30 camadas lineares ~64 MB de VRAM fixos (independem do contexto).

## Atencao completa (camadas 3, 7, ...: 16 cabecas q de 256, 2 kv, RoPE parcial 64, q/k norm, portao)
Verificado so por leitura (nao medido nesta bancada):
- hd=256: `att1f` usa local_size = hd e vetores compartilhados de 256 -> suportado. `att_flash` aceita hd multiplo de 32 ate
  256 (HD=256, GRP=8: ksh 33 KB + qsh 8 KB + psh 1 KB ~ 42 KB < 48 KB) -> deve servir; `qkv_post` e generico em hd.
  Falta rodar vkcheck/selftest com hd=256.
- RoPE parcial (so as 64 primeiras dims, pares neox (i, i+32)): os kernels pareiam (i, i+hd/2) com freq[i], i < hd/2, entao
  nao suportam direto. Saida sem kernel novo: permutar na carga as linhas de Wq e Wk (e os pesos de q_norm/k_norm) de cada
  cabeca para que as dims 32..63 fiquem em 128..159 (as demais preenchem o resto), e usar freq[i] = base^(-2i/64) para
  i < 32 e freq[i] = 0 (rotacao identidade, cos 1 sen 0 exatos) para 32 <= i < 128. O produto q.k e a norma RMS por cabeca
  nao mudam com a mesma permutacao em q e k; v nao e permutado. Alternativa: parametro `nrot` nos tres kernels.
- Portao: a projecao q do modelo sai com [q_h (256) | portao_h (256)] por cabeca; separar na carga em duas matrizes (sem
  custo). Falta multiplicar a saida da atencao por sigmoid(portao): kernel elementar novo (ou epilogo extra em att1f/att_flash,
  um binding a mais).
- Conferir na CPU se q_norm/k_norm usam `w` ou `1+w` (a familia usa normas centradas em zero em alguns pontos).
