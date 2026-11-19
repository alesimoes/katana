# vkatt: kernels nao-matmul do katana em Vulkan (GLSL compute)

Porte de `rms`, `rmsh`, `gather`, `moe_sum`, `qkv_post`, `att1`, `att1f`, `att_scores`, `att_softmax`, `att_mix`
do `GPU_SRC` (OpenCL C) do `katana.c`. `bsum` NAO foi portado (so alimenta os matmuls em lote Q4_K/Q5_K, que ficam fora).
Harness `vkatt.c` usa o `vk/kvk.h` e compara tudo com referencia em double escrita no proprio harness.

## Reconstruir e rodar
    sh gen.sh                                   (shaders -> .h, via glslang)
    cmd //c "cd /d C:\source\katana\tools\vkatt && .\b.bat"
    vkatt.exe            (tudo: corretude + tempos; ~2 min)   -v: uma linha por caso   -t: so tempos   -k nome: so um kernel
`ultima_execucao.txt` e a saida da ultima execucao completa.

## Decisoes de porte
- **Lote B em push constant** (nao spec constant): um pipeline serve B = 1, 4, 16; o custo e um laco `for b < B`
  (B <= 16, `float ss[16]` em registradores) e nao apareceu nas medidas. No original o NB mudava o tipo (float4/float16);
  aqui os acessos sao escalares `x[col*B + b]`.
- **KV16 por variante de compilacao** (`-DKV16=0/1` -> `*_kv32.h` / `*_kv16.h`): o tipo do buffer muda (`float16_t[]`),
  exige storageBuffer16BitAccess + shaderFloat16 (o kvk ativa quando existem). Leitura/escrita de meia precisao via `float16_t(v)`.
- **hd por spec constant** (`local_size_x_id = 0`) em `att1` e `att1f`: um pipeline por tamanho de cabeca (64 e 128 testados; maximo 256).
- **Posicao em push constant**: simples para o harness. No motor com grafo gravado (`kvk_rec`), `pos`, `base` e o comprimento
  mudam por token e precisam vir de um buffer pequeno escrito com `kvk_buf_write_q` (regra do README do kvk (vk/README.md)). Mudanca mecanica:
  ler `pos` de um SSBO no inicio de `att1f`/`qkv_post`. NAO feita aqui.
- **Seno/cosseno do RoPE**: a instrucao de hardware do GLSL erra ate ~2e-4 em angulos grandes (pos 600 -> ate ~600 rad), o que
  falhou 100+ casos. Corrigido com reducao de argumento (`rope_cs` em `common.glsl`: 2*pi em duas partes com fma). O angulo
  continua `float(pos)*freq[i]` em float como no original (o arredondamento dessa multiplicacao e do motor, nao do porte).
- `qkv_post` com `norm==0` le q de `qs` e escreve em `q`; o motor passa o mesmo buffer nos dois bindings (testado assim).
  Com norma, sao buffers diferentes (testado assim tambem). Bias e norma juntos nao sao testados (o original soma os quadrados sem o bias).
- `att1f` e fiel: um grupo por cabeca, hd threads, reducoes por memoria local com barreira, expoentes sem normalizar em `att`.
  `att_softmax` continua com UMA thread por linha (fiel) e e lento (ver tempos).

## Cobertura dos testes (todos PASS, 1720 casos)
| kernel | casos | formas |
|---|---|---|
| rms / rms_wg | 6 + 6 | dim 1536, 2048; B 1, 4, 16 |
| rmsh | 4 | 12x128 B1, 16x64 B4, 32x128 B16, 12x128 B16 |
| gather | 12 | n 1536, 2048; stride 4, 16; 3 colunas |
| moe_sum | 12 | d 1536, 2048; B 1, 4, 16; K 2, 8 (pesos e linhas aleatorios, meta com offset) |
| qkv_post | 960 | 5 formas (12x128 kv2, 12x128 kv12, 16x128 kv4, 32x64 kv4, 16x64 kv16) x B 1,4,16 x pos0 0,1,37,600 x neox 0/1 x {plain, bias, norm1, norm2} x KV32/KV16 |
| att1f | 320 | 5 formas x pos 0,1,37,600 x neox x 4 modos x KV32/KV16 |
| att1 | 40 | 5 formas x 4 pos x KV32/KV16 |
| att_scores/softmax/mix | 120 cada | 5 formas x B 1,4,16 x pos0 0,1,37,600 x KV32/KV16 |

Compara TODAS as saidas: q, os caches k e v inteiros (posicoes nao escritas tem que ficar intactas), saida da atencao,
e o buffer `att` (expoentes ate `pos`, e nas etapas em lote o que esta alem de `pos0+b` nao pode ser escrito).
Tolerancias: f32 ~1e-4 absoluto/relativo; KV16: 1.2e-3 relativo + 4e-4 (meia precisao: o erro de 9.8e-4 observado e o arredondamento da meia).
Piores erros: rms 2.5e-6, rmsh 1.1e-6, moe_sum 6.9e-7, att1 1.3e-6, att_scores 1.7e-6, att_softmax 3.1e-7, att_mix 6.4e-7;
qkv_post e att1f ~1e-3 absoluto so nos caches KV16 (arredondamento de meia).
A referencia de KV16 le o cache ja decodificado; na posicao atual a atencao usa k e v locais em float32 (como o original).

## Tempos (us por despacho, 200 despachos num grafo, 1 espera, com barreira entre despachos; dim 1536, 12x128, 2 cabecas kv, pos 600)
Medidos com a GPU quase livre (min de 9 repeticoes). A mesma maquina chegou a dar o dobro com outro processo usando a GPU: ruido alto.

| kernel | us |
|---|---|
| rms n=1536 B=1 / 4 / 16 (fiel) | 35 / 311 / 992 |
| **rms_wg (melhoria)** B=1 / 4 / 16 | **3.0 / 3.5 / 6.2** |
| rmsh 12x128 B=1 | 16 |
| gather | 1.6 |
| moe_sum K=8 B=1 / 16 | 2.3 / 7.1 |
| qkv_post B=1 bias+neox (kv32 / kv16) | 2.0 / 2.1 |
| qkv_post B=16 | 2.9 / 3.1 |
| qkv_post B=1 norm1 (vetor inteiro) | 34 (kv32) / 34 (kv16) |
| qkv_post B=1 norm2 (por cabeca) | 3.6 / 3.8 |
| att1f (kv32 / kv16) | 90 (mediana 112) / 72 |
| att1 (kv32 / kv16) | 71 / 78 |
| att_scores B=1 / 16 | 39 / 524 |
| att_softmax B=1 / 16 | 105 / 353 |
| att_mix B=1 / 16 | 81 / 289 |

## Nota de projeto: o que o Vulkan permite e o OpenCL do motor nao usava
Implementada UMA melhoria: **`rms_wg`** (`rms_wg.comp`). O `rms` fiel faz cada thread refazer a soma dos quadrados de
todas as n colunas (O(n^2): 35 us com 1 token, ~1 ms com 16). `rms_wg` usa um grupo de 256 threads por token, `subgroupAdd` dentro
do subgrupo e uma memoria local de 8 floats entre subgrupos: O(n), e o custo vira quase so a latencia do despacho.
Medido: 35 -> 3.0 us (B=1), 311 -> 3.5 us (B=4), 992 -> 6.2 us (B=16), mesmo erro (<= 5e-7). O motor chama rms 2x por camada
mais uma no fim, entao em B=1 isso e ~70 us por camada a menos nesta GPU. Escolhi esta por ser o maior ganho absoluto e o menor risco
(mesmos buffers e push constants, troca direta).
Tentei antes, e depois retirei (so uma melhoria, conforme o pedido), `subgroupMax/subgroupAdd` no lugar das duas reducoes seriais
sobre memoria local do `att1f`: ganho de cerca de 8% numa medida com a GPU ocupada, dentro do ruido; nao medi de novo sem ruido.
Nao implementadas, em ordem de valor estimado (NAO medido):
1. `qkv_post` com norm 1: cada thread refaz a soma dos quadrados do vetor inteiro (34 us contra 2-4 us sem norma); usar o mesmo
   padrao do `rms_wg` (um grupo reduz, as outras leem) ou um pre-passo.
2. `att_softmax` em lote: uma thread por linha (105 us B=1, 353 us B=16); um grupo por linha com subgroupMax/Add.
3. Atencao em um despacho para o lote com softmax online (flash), eliminando `att` e os 3 despachos; leitura do cache KV16 (ja suportada)
   ou ate de 8 bits reduz a banda, que domina em posicoes grandes.
4. `att1f`: dividir o contexto entre varios grupos por cabeca (so 12 grupos ocupam pouco a GPU) e juntar com um passo final.
5. Fundir rms + proximo matmul (o rms vira leitura de `x` com fator escalar).

## O que NAO foi feito / nao verificado
- `bsum` nao portado. Nenhuma integracao ao `katana.c`.
- Posicao/`pos0` como push constant (ver acima); o grafo gravado exige buffer de metadados.
- Bias junto com norma nao testado; hd > 256 e hd impar fora do escopo (como no original); B > 16 nao suportado em rms/rmsh.
- So testado na RTX 3050 Laptop (subgrupo 32). `rms_wg` assume subgrupo >= 8 e <= 256 (usa `gl_SubgroupSize` para contar subgrupos).
- Tempos com ruido de outros processos na GPU; comparar sempre medidas alternadas.
