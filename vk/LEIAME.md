# kvk: camada de computacao Vulkan do katana

Arquivo unico `kvk.h`. Em uma unica unidade de traducao faca `#define KVK_IMPL` antes de `#include "kvk.h"`.
Precisa so dos cabecalhos Khronos (`ref/vk/include`, `VK_NO_PROTOTYPES`); `vulkan-1.dll` / `libvulkan.so.1` e carregada em tempo de execucao.
Falhas nunca chamam `exit()`: funcoes devolvem 0 / handle com campo `b`/`p`/`cb` zerado e `kvk_error()` traz o texto. Uma thread so.

## API
- `int kvk_init(KvkInfo*)`: 1 ok, 0 falha. Escolhe a GPU discreta; `KATANA_VK_DEVICE=<indice>` forca (indice invalido = falha). Exige Vulkan 1.2.
  Ativa o que existir: cooperative_matrix, shaderFloat16, storageBuffer16BitAccess, shaderInt8/storageBuffer8BitAccess, shaderInt16, shader_integer_dot_product.
  `KvkInfo`: nome, vram_bytes (maior heap DEVICE_LOCAL), subgroup_size, limites, flags `coopmat fp16 storage16 int8 int16 int_dot`, lista `coop[]` de formas.
- `kvk_shutdown()`, `kvk_active()`, `kvk_error()`, `kvk_stats()`.
- Buffers: `kvk_buf_create(bytes, KVK_USAGE_RW)`, `kvk_buf_destroy`, `kvk_bytes_allocated()` (para o orcamento de VRAM),
  `kvk_buf_write` (completa antes de voltar), `kvk_buf_write_q` (copia os dados na hora para a area de upload e enfileira a copia no fluxo; roda no flush/wait),
  `kvk_buf_read` = `kvk_read_sync` (flush + espera + leitura). Offsets/tamanhos quaisquer. Area de upload (coerente) e de download (cacheada) persistentes, crescem sob demanda ate 64 MB e escritas maiores vao em pedacos.
- `kvk_pipe_create(spirv, bytes, n_buffers, push_bytes, spec_ids, spec_vals, n_spec)`: todos os bindings sao storage buffers no set 0, binding 0..n-1; push multiplo de 4; spec constants de 32 bits.
- Fluxo: `kvk_begin` (opcional), `kvk_dispatch(pipe, bufs, n, push, push_bytes, gx, gy, gz)`, `kvk_flush` (submete, nao espera), `kvk_wait`.
  Barreira compute/transfer -> compute/transfer antes de cada despacho e copia (`kvk_set_auto_barrier(0)` + `kvk_barrier()` para otimizar depois).
- Grafo gravado: `kvk_rec_begin()`, `kvk_dispatch...`, `KvkRec r = kvk_rec_end()`, `kvk_rec_submit(r)` quantas vezes quiser, `kvk_rec_free(&r)`, `kvk_rec_cancel()`.

## Regras do grafo gravado
Buffers (handles) e push constants ficam congelados na gravacao. O que muda por token (posicao, token atual, comprimento do cache) deve estar
em buffer pequeno escrito com `kvk_buf_write_q` antes do `kvk_rec_submit` (o submit faz flush da escrita antes; a ordem e garantida por barreira).
Nao e permitido `write_q`, `read`, `flush`, `wait` durante a gravacao. O buffer de comandos usa SIMULTANEOUS_USE, entao resubmeter sem esperar e valido.
Destruir buffer/pipeline usado por um grafo invalida o grafo (responsabilidade do chamador).

## Exemplo
```c
#define KVK_IMPL
#include "kvk.h"
KvkInfo I; if (!kvk_init(&I)) { /* cai para OpenCL/CPU; kvk_error() */ }
KvkPipe p = kvk_pipe_create(spv, sizeof spv, 2, 12, NULL, NULL, 0);
KvkBuf x = kvk_buf_create(n*4, KVK_USAGE_RW), y = kvk_buf_create(n*4, KVK_USAGE_RW);
kvk_buf_write(x, 0, hx, n*4);
KvkBuf io[2] = { x, y }; struct { float a, b; uint32_t n; } pc = { 3, 1, n };
kvk_dispatch(p, io, 2, &pc, 12, (n+63)/64, 1, 1);
kvk_buf_read(y, 0, hy, n*4);           /* flush + espera + leitura */
```

## Decisoes e limites
- Descritores: um conjunto por (pipeline, tupla de buffers), em tabela hash; pools de 256 conjuntos, novo pool quando enche (FREE_DESCRIPTOR_SET para liberar ao destruir buffer/pipeline). Binding sempre do buffer inteiro (sem offset).
- Cada buffer tem memoria propria (sem sub-alocacao): respeita `maxMemoryAllocationCount` (4096 em algumas placas); o motor deve usar poucos buffers grandes.
- Anel de 8 buffers de comando + fences; `kvk_wait` espera todos e reinicia a area de upload.
- Memoria de upload preferindo host-visible coerente fora de device-local; download preferindo cacheada (invalida o range se nao for coerente).
- Nao ha timeline semaphores, fila de transferencia separada nem fila assincrona; tudo em uma fila de computacao.
- Nao testado: Linux (so guardas `#ifdef _WIN32`), GPUs sem Vulkan 1.2, pressao real de VRAM (OOM), perda de dispositivo.

## Build e teste
    glslang.exe -V --target-env vulkan1.2 --vn addc_spv -o addc.h addc.comp   (idem axpb, addp)
    b.bat                      (compila kvk_test.exe)
    kvk_test.exe               (GPU discreta)   |   set KVK_TEST_DEV=0 & kvk_test.exe   (escolhe indice; 0=Intel, 1=NVIDIA nesta maquina)

## Shaders do prompt em lote (`vk/shaders`)
`sh vk/shaders/gen.sh` gera todos os cabecalhos SPIR-V. `mm.comp` (produto em matriz cooperativa, copia de `tools/vkq4k`; so as variantes
usadas: 5 tipos x 64x64 / 64x32w4 / 64x16w4 e os de glu com saida fp16), `cvt`, `rms_wg16`, `gather`, `qkv_post`, `att_split` (tres kernels,
`KATANA_VK_ATT=3`) e `att_flash` (atencao de um lancamento, padrao). Variaveis: `KATANA_VK_BATCH=0` (prompt token a token), `KATANA_VK_PASS`
(maior lote por passada, padrao 256), `KATANA_VK_CAP`, `KATANA_VK_MINB` (menor lote, padrao 2), `KATANA_VK_SKIP` (mascara de fases omitidas, so para medir).
Comando oculto `katana vkcheck modelo texto.txt [ngen] [ntokens]` compara lote e token a token.
