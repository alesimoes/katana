# katana

Motor de inferência de modelos de linguagem em C puro, sem dependências: lê arquivos GGUF,
roda na CPU (AVX2) e na GPU (OpenCL carregado do driver em tempo de execução, sem SDK).

## Compilar

Windows, com as Build Tools do Visual Studio 2022:

```
build.bat
```

## Usar

```
katana chat  <modelo.gguf> "pergunta" [max_tokens] [temperatura]
katana run   <modelo.gguf> "texto"    [max_tokens] [temperatura]
katana info  <modelo.gguf>
katana bench <modelo.gguf> [tokens_prompt] [tokens_geracao]
katana selftest <modelo.gguf>     confere GPU e CPU inteira contra a conta exata em float
katana micro <modelo.gguf>        mede os produtos da CPU isolados, com calibração da máquina
katana tune  <modelo.gguf>        mede e grava em katana.cfg o melhor ajuste para a GPU local
katana pack  <modelo.gguf>        gera os shards no layout da GPU (<modelo>.gguf.ktn/)
katana verify <modelo.gguf>       confere o SHA-256 de cada shard
```

## O que é suportado

- Arquiteturas `llama` e `qwen2` (RMSNorm, RoPE, GQA, SwiGLU).
- Tensores F32, F16, Q8_0, Q4_0, Q3_K, Q4_K, Q5_K e Q6_K.
- Tokenizadores BPE (estilo GPT-2) e SentencePiece, lidos do próprio GGUF.
- Formatos de conversa ChatML, Llama 3, Zephyr e `[INST]`, escolhidos pelo template do modelo.

## Como roda

- Os pesos sobem para a VRAM quantizados, num layout próprio: linhas entrelaçadas de 32 em 32
  (leitura coalescida por warp) e palavras de 32 bits alinhadas.
- Se o modelo cabe na VRAM, o token inteiro roda na GPU: uma escrita (embedding) e uma leitura
  (logits) por token. O prompt entra em lotes de 16 tokens.
- Se não cabe, as primeiras camadas rodam na GPU e o resto na CPU, com uma transferência do
  estado por token; o cache KV passa para float16 para caber mais camadas.
- Na CPU os produtos são inteiros, com ativações de 16 bits.
- Decodificação especulativa por repetição: quando o contexto recente se repete, o motor confere
  quatro tokens numa passada. A saída é a mesma de gerar um por um.

## Variáveis de ambiente

| Variável | Efeito |
|---|---|
| `KATANA_CPU=1` | não usa a GPU |
| `KATANA_GPU_LAYERS=N` | no máximo N camadas na GPU |
| `KATANA_VRAM_PCT=N` | fração da VRAM que o motor pode ocupar (padrão 95) |
| `KATANA_CTX=N` | contexto máximo (padrão: o do modelo, até 4096) |
| `KATANA_KV16=0/1` | força o cache KV em float32 ou float16 |
| `KATANA_BATCH=N` | maior lote de prompt (16, 4 ou 1) |
| `KATANA_SPEC=0` | desliga a decodificação especulativa |
| `KATANA_PREFETCH=0/1` | leitura antecipada do disco para as camadas da CPU |
| `KATANA_NOSHARDS=1` | ignora os shards e converte do GGUF |
| `KATANA_HEAD_GPU=1` | com modelo que não cabe, sobe a projeção final antes das camadas |
| `KATANA_SEG`, `KATANA_ROWS`, `KATANA_ROWSB` | ajustes dos kernels (ver `katana tune`) |
| `KATANA_DEBUG=1` | perfil por kernel da GPU e por tipo de produto da CPU |
| `OMP_NUM_THREADS`, `OMP_WAIT_POLICY=ACTIVE` | threads da CPU; a espera ativa ajuda quando há camadas na CPU |

## Arquivos

- `katana.c`: o motor inteiro.
- `ia.c`: protótipo inicial (transformer mínimo com formato próprio), mantido como referência.
