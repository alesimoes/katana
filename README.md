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
katana chat <modelo.gguf>          conversa interativa de vários turnos (/novo, /sair; KATANA_SYSTEM, KATANA_TEMP, KATANA_MAXTOK)
katana pack  <modelo.gguf>        gera os shards no layout da GPU (<modelo>.gguf.ktn/)
katana verify <modelo.gguf>       confere o SHA-256 de cada shard
katana sparsity <modelo.gguf> <calibracao.txt> <avaliacao.txt>   esparsidade do feed-forward por assunto (modelo denso)
katana routing  <modelo.gguf> <calibracao.txt> <avaliacao.txt>   concentracao dos especialistas por assunto (modelo MoE)
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

## Mistura de especialistas (MoE)

Arquiteturas `olmoe` (OLMoE-1B-7B) e `qwen3moe` (Qwen3-30B-A3B), as duas testadas. Num modelo MoE cada token usa poucos especialistas (8 de 64 ou de 128 por camada), então o motor trata a VRAM como um cache:

- a atenção de todas as camadas fica fixa na VRAM;
- o resto da VRAM é dividido em vagas de especialistas por camada;
- cada especialista tem um "calor" (usos recentes, com decaimento). Os que estão na VRAM rodam juntos na GPU, num lançamento só por camada;
- um especialista fora da VRAM roda na CPU na hora, direto do arquivo mapeado: o token nunca espera por disco. Se ele estiver mais quente que o residente mais frio, uma thread carregadora o traz em segundo plano e ele sobe num token seguinte;
- `katana pack` grava os especialistas já no layout da GPU (um arquivo por camada), e a carga vira leitura de disco + cópia, sem conversão;
- ao sair, o calor é gravado (`<modelo>.gguf.ktn.heat`); na partida seguinte os mais quentes são pré-carregados;
- threads de aquecimento tocam as páginas dos especialistas logo na carga (no Windows a primeira leitura de cada página mapeada é cara).

Medido numa RTX 3050 de 4 GB (i7-11800H, 64 GB), geração:

| Modelo | katana | llama.cpp b12418 (CUDA, melhor configuração: `-ngl 99 -ot exps=CPU`) |
|---|---|---|
| Qwen3-30B-A3B Q4_K_M (18,6 GB), texto real, 200 tokens | 11,5–15,5 tok/s (varia com a temperatura da máquina) | 8,6 tok/s (uma medida) |
| idem, benchmark sintético 64+64 | 15–18 tok/s (prompt 28–33, em lote) | 14,0 tok/s (prompt 26,7) |
| OLMoE-1B-7B Q4_K_M (4,2 GB) | ~60 tok/s (texto real), ~88 (sintético) | não medido |

No Qwen3-30B só 19 de 128 especialistas por camada cabem na VRAM e eles atendem ~56% dos usos em texto real. A saída gulosa bate com a do llama.cpp por ~60 tokens no teste feito (depois diverge numa escolha apertada).

OLMoE com o cache reduzido de propósito (`KATANA_EXPERT_MB`): 51 de 64 por camada na VRAM atendem 95% dos usos; 16 de 64 atendem 62% (35 tok/s); 8 de 64 atendem 45% (32 tok/s); só CPU, ~20 tok/s.

`katana routing` mede o efeito de assunto: com 25% dos especialistas na VRAM, o conjunto calibrado num texto do mesmo assunto atende 63% dos usos (o teto é 64%); calibrado em outro assunto, 34%; ao acaso, 25%.

## Variáveis de ambiente

| Variável | Efeito |
|---|---|
| `KATANA_CPU=1` | não usa a GPU |
| `KATANA_GPU_LAYERS=N` | no máximo N camadas na GPU |
| `KATANA_VRAM_PCT=N` | fração da VRAM que o motor pode ocupar (padrão 92) |
| `KATANA_CTX=N` | contexto máximo (padrão: o do modelo, até 4096) |
| `KATANA_KV16=0/1` | força o cache KV em float32 ou float16 |
| `KATANA_BATCH=N` | maior lote de prompt (16, 4 ou 1) |
| `KATANA_SPEC=0` | desliga a decodificação especulativa |
| `KATANA_PREFETCH=0/1` | leitura antecipada do disco para as camadas da CPU |
| `KATANA_NOSHARDS=1` | ignora os shards e converte do GGUF |
| `KATANA_HEAD_GPU=1` | com modelo que não cabe, sobe a projeção final antes das camadas |
| `KATANA_SEG`, `KATANA_ROWS`, `KATANA_ROWSB` | ajustes dos kernels (ver `katana tune`) |
| `KATANA_DEBUG=1` | perfil por kernel da GPU e por tipo de produto da CPU |
| `KATANA_THREADS=N` | threads da CPU (padrão: média entre núcleos físicos e lógicos) |
| `KATANA_PRIO=0` | não eleva a prioridade das threads do motor |
| `KATANA_PIN=1` | fixa cada thread num núcleo físico |
| `KATANA_EXPERT_MB` | MoE: limita o cache de especialistas na VRAM |
| `KATANA_MOE_CPU=0` | MoE: especialista fora da VRAM sempre sobe (em vez de rodar na CPU) |
| `KATANA_SWAP` | MoE: quanto mais quente que o residente mais frio um especialista precisa estar para subir (padrão 4) |
| `KATANA_NOLOADER=1` | MoE: sem thread carregadora (a carga bloqueia o token) |
| `KATANA_NOHEAT=1` | MoE: não grava nem lê o calor entre sessões |
| `KATANA_WARM=0` | MoE: sem threads de aquecimento das páginas |
| `KATANA_ROPE` | força o RoPE: 0 = pares adjacentes, 1 = metades |

## Arquivos

- `katana.c`: o motor inteiro.
- `ia.c`: protótipo inicial (transformer mínimo com formato próprio), mantido como referência.
