# Formatos de modelo, runtimes e ferramentas (base de conhecimento para o katana)

Data da pesquisa: 16/nov/2026. Fontes primárias lidas diretamente: `ggml.h`, `gguf-py/constants.py` (llama.cpp, branch master), `docs/gguf.md` (ggml), `onnx.proto`, `tensor.rs` e README do safetensors, documentação do Hub (DDUF, GGUF, parsing de safetensors), releases do ONNX Runtime. Itens só confirmados por notícia/blog/agregador estão marcados "(verificar)". Nada foi inventado: onde não achei fonte, está escrito.

## Tabela-resumo

| Nome | Tipo | Ativo em 2026? | Relevância para o katana |
|---|---|---|---|
| GGUF | formato de arquivo (tensores + metadados + tokenizer) | Sim, muito ativo (ggml.h tem 44 tipos, ids até 43) | Máxima: formato nativo do katana; manter o leitor em dia com os tipos novos |
| Safetensors | formato de arquivo (só tensores) | Sim; desde 8/abr/2026 projeto da PyTorch Foundation | Alta: ler direto do Hub sem conversão (header JSON simples, mmap) |
| DDUF | formato de arquivo (ZIP sem compressão, difusão) | Pouco movimento; só Diffusers dá suporte (docs) | Baixa: só se o katana um dia rodar difusão; o leitor ZIP é trivial |
| ONNX | formato de arquivo (protobuf, grafo) | Sim (IR 13, opset 28 no ORT 1.32) | Média-baixa: referência de operadores; ler ONNX de LLM não compensa |
| ONNX Runtime | runtime | Sim (1.32.0 em 13/nov/2026, mensal) | Média: ideia de "plugin EP", EPContext; concorrente/benchmark |
| OpenVINO | runtime + toolkit (IR .xml/.bin) | Sim (2026.4 em set/2026) | Média: alvo Intel (CPU/iGPU/NPU); inspiração de decodificação especulativa |
| Core ML | formato (.mlpackage) + runtime Apple | Sim; nota de "Core AI" em iOS 27 (verificar) | Baixa: só Apple; katana é Linux/Windows |
| LiteRT (ex-TFLite) | runtime + formato (.tflite FlatBuffers); LiteRT-LM (.litertlm) | Sim (LiteRT 2.x, CompiledModel API) | Baixa-média: ideias de aceleradores e fallback por operador |
| ExecuTorch | runtime + formato (.pte FlatBuffers) | Sim (1.0 GA em out/2025) | Baixa-média: backend Vulkan de produção; arquitetura de delegates |
| llamafile | ferramenta de empacotamento + runtime (llama.cpp + Cosmopolitan) | Sim (0.10.x; 0.10.5 em ago/2026) | Alta como ideia: um executável + pesos embutidos |
| Transformers.js | biblioteca JS (runtime via ONNX Runtime Web) | Sim (v4 em 2026) | Baixa: referência de pipelines e do que o Hub serve em ONNX |
| unity-sentis | runtime (pacote Unity) | Sim (2.x; nome oscilou Sentis/Inference Engine) | Muito baixa: nicho Unity; só lista de operadores como referência |
| Joblib | formato/ferramenta (pickle + numpy) | Sim (1.5.3 em dez/2025) | Nenhuma como entrada (inseguro); só saber reconhecer e recusar |
| TensorBoard | ferramenta de visualização (+ formato tfevents) | Sim (repositório ativo) | Baixa: opcionalmente gravar métricas de treino/benchmark em tfevents |

---

## 1. GGUF

**O que é / quem mantém.** Formato binário de arquivo único para ggml, criado por Georgi Gerganov; mantido pelo projeto ggml-org (ggml e llama.cpp). Guarda tensores e metadados padronizados (arquitetura, hiperparâmetros, tokenizer, template de chat) no mesmo arquivo. O Hub tem visualizador de metadados e tensores para GGUF.

**Estado em 2026.** Muito ativo. `ggml.h` (master) tem `GGML_TYPE_COUNT = 44`. `constants.py` lista 174 entradas em `MODEL_ARCH` (inclui qwen3, qwen3moe, qwen3next, qwen35, gemma3, gemma3n, gemma4, gpt-oss, deepseek2/32/4, glm4moe, mistral3/4, nemotron_h, lfm2, olmoe, granitehybrid, mamba2, rwkv7, kimi-linear, minimax-m2, step35, etc.). Versão do formato continua 3.

### Layout exato (docs/gguf.md)

Ordem de bytes: little-endian por padrão (v3 admite big-endian, sem como detectar pelo arquivo).

```
uint32  magic            = "GGUF" (0x47 0x47 0x55 0x46)
uint32  version          = 3
uint64  tensor_count
uint64  metadata_kv_count
metadata_kv[metadata_kv_count]
tensor_info[tensor_count]
padding até múltiplo de general.alignment (bytes 0x00)
tensor_data (cada tensor alinhado)
```

- **String GGUF:** `uint64 len` + bytes UTF-8, sem terminador nulo.
- **KV:** `string key` (ASCII, até 65535 bytes, segmentos `lower_snake_case` separados por ponto), `uint32 value_type`, valor.
- **Tipos de valor (GGUFValueType):** 0 UINT8, 1 INT8, 2 UINT16, 3 INT16, 4 UINT32, 5 INT32, 6 FLOAT32, 7 BOOL (1 byte, 0/1), 8 STRING, 9 ARRAY (`uint32 elem_type`, `uint64 len`, elementos; pode aninhar), 10 UINT64, 11 INT64, 12 FLOAT64.
- **tensor_info:** `string name` (até 64 bytes), `uint32 n_dims` (máx. 4, `GGML_MAX_DIMS`), `uint64 dims[n_dims]`, `uint32 type` (ggml_type), `uint64 offset`.
  - `offset` é relativo ao início da região `tensor_data` (não ao arquivo) e múltiplo do alinhamento.
  - Ordem das dimensões: ggml (ne[0] é a dimensão mais rápida, contígua). Um tensor `[out, in]` do PyTorch aparece como `dims = [in, out]`. A linha contígua tem `ne[0]` elementos; `ne[0]` deve ser múltiplo do tamanho de bloco do tipo.
- **Alinhamento:** chave `general.alignment` (uint32, múltiplo de 8; padrão 32 se ausente). Início de `tensor_data` = fim do bloco de tensor_info arredondado para cima ao alinhamento; cada tensor também.
- **Tamanho em bytes de um tensor:** `nbytes = (ne0 / block_size) * type_size * ne1 * ne2 * ne3` (tabela abaixo).

### Lista completa de tipos de tensor (ggml_type, ids exatos de `ggml.h` master)

Colunas de tamanho: elementos por bloco / bytes por bloco (de `GGML_QUANT_SIZES` em `gguf-py/constants.py`, QK_K = 256).

| id | tipo | bloco | bytes | observação |
|---|---|---|---|---|
| 0 | F32 | 1 | 4 | |
| 1 | F16 | 1 | 2 | |
| 2 | Q4_0 | 32 | 18 | `half d` + 16 B de nibbles |
| 3 | Q4_1 | 32 | 20 | `d`, `m` (half) + 16 B |
| 4, 5 | (Q4_2, Q4_3) | | | removidos |
| 6 | Q5_0 | 32 | 22 | |
| 7 | Q5_1 | 32 | 24 | |
| 8 | Q8_0 | 32 | 34 | `half d` + 32 int8 |
| 9 | Q8_1 | 32 | 36 | |
| 10 | Q2_K | 256 | 84 | |
| 11 | Q3_K | 256 | 110 | |
| 12 | Q4_K | 256 | 144 | |
| 13 | Q5_K | 256 | 176 | |
| 14 | Q6_K | 256 | 210 | |
| 15 | Q8_K | 256 | 292 | só para ativações intermediárias |
| 16 | IQ2_XXS | 256 | 66 | |
| 17 | IQ2_XS | 256 | 74 | |
| 18 | IQ3_XXS | 256 | 98 | |
| 19 | IQ1_S | 256 | 50 | |
| 20 | IQ4_NL | 32 | 18 | |
| 21 | IQ3_S | 256 | 110 | |
| 22 | IQ2_S | 256 | 82 | |
| 23 | IQ4_XS | 256 | 136 | |
| 24 | I8 | 1 | 1 | |
| 25 | I16 | 1 | 2 | |
| 26 | I32 | 1 | 4 | |
| 27 | I64 | 1 | 8 | |
| 28 | F64 | 1 | 8 | |
| 29 | IQ1_M | 256 | 56 | |
| 30 | BF16 | 1 | 2 | |
| 31-33 | (Q4_0_4_4, 4_8, 8_8) | | | removidos dos arquivos; usar Q4_0 e repack em tempo de execução |
| 34 | TQ1_0 | 256 | 54 | ternário |
| 35 | TQ2_0 | 256 | 66 | ternário |
| 36-38 | (IQ4_NL_4_4, 4_8, 8_8) | | | removidos |
| 39 | MXFP4 | 32 | 17 | 1 byte de escala E8M0 + 16 B (32 valores FP4 E2M1); formato do gpt-oss |
| 40 | NVFP4 | 64 | 36 | 4 escalas E4M3 (1 B cada) + 32 B; 4 sub-blocos de 16 |
| 41 | Q1_0 | 128 | 18 | `half` + 16 B (1 bit/peso) |
| 42 | Q2_0 | 64 | 18 | `half` + 16 B |
| 43 | F8_E4M3 | 1 | 1 | FP8 E4M3 cru |
| 44 | COUNT | | | não é tipo |

Notas: ids 39 a 43 são os de 2025-2026 (MXFP4 chegou com o gpt-oss em ago/2025, PR 15091 do llama.cpp). O layout interno de byte de Q1_0, Q2_0, NVFP4 e F8_E4M3 (ordem dos bits, fórmula de escala) não foi lido nas fontes de C: só tamanhos e ids estão confirmados (verificar nas structs de `ggml-common.h` antes de implementar). Para os demais, as structs de `ggml-common.h` são a referência; o katana já as implementa para os tipos que usa (ver `docs/conhecimento.md`).

### `general.file_type` (LlamaFileType, informativo; não confundir com ggml_type)

0 ALL_F32, 1 MOSTLY_F16, 2 Q4_0, 3 Q4_1, 7 Q8_0, 8 Q5_0, 9 Q5_1, 10 Q2_K, 11 Q3_K_S, 12 Q3_K_M, 13 Q3_K_L, 14 Q4_K_S, 15 Q4_K_M, 16 Q5_K_S, 17 Q5_K_M, 18 Q6_K, 19 IQ2_XXS, 20 IQ2_XS, 21 Q2_K_S, 22 IQ3_XS, 23 IQ3_XXS, 24 IQ1_S, 25 IQ4_NL, 26 IQ3_S, 27 IQ3_M, 28 IQ2_S, 29 IQ2_M, 30 IQ4_XS, 31 IQ1_M, 32 BF16, 36 TQ1_0, 37 TQ2_0, 38 MXFP4_MOE, 39 NVFP4, 40 Q1_0, 41 Q2_0, 42 F8_E4M3, 1024 GUESSED. (4, 5, 6, 33-35 removidos.) O katana não deve depender deste campo: o tipo real está em cada tensor_info.

### Chaves de metadados padrão

Fontes: `docs/gguf.md` (especificação) e `Keys` em `constants.py` (o que o llama.cpp realmente lê/escreve em 2026; a spec em markdown está atrasada em relação ao código). `{arch}` = valor de `general.architecture`.

**Gerais.** `general.architecture` (string, obrigatória), `general.type` (model, mmproj, adapter, imatrix: valores usados no código; a spec não define, verificar), `general.quantization_version` (uint32, 2 atualmente; obrigatória se há tensor quantizado), `general.alignment`, `general.file_type`, `general.name`, `.author`, `.version`, `.organization`, `.finetune`, `.basename`, `.description`, `.quantized_by`, `.size_label`, `.license` (SPDX), `.license.name`, `.license.link`, `.url`, `.doi`, `.uuid`, `.repo_url`, `general.source.{url,doi,uuid,repo_url}`, `general.base_model.count` e `general.base_model.{i}.{name,author,version,organization,description,url,doi,uuid,repo_url}`, `general.dataset.count` e `general.dataset.{i}.*`, `general.tags`, `general.languages`, `general.sampling.{sequence,top_k,top_p,min_p,xtc_probability,xtc_threshold,temp,penalty_last_n,penalty_repeat,mirostat,mirostat_tau,mirostat_eta}` (parâmetros de amostragem recomendados pelo autor).

**Dimensões do modelo (`{arch}.*`).** `vocab_size`, `context_length`, `embedding_length`, `block_count`, `feed_forward_length`, `leading_dense_block_count`, `use_parallel_residual`, `tensor_data_layout`, `pooling_type`, `logit_scale`, `decoder_start_token_id`, `attn_logit_softcapping`, `final_logit_softcapping`, `router_logit_softcapping`, `embedding_scale`, `residual_scale`, `full_attention_interval` (modelos híbridos como qwen3next/qwen35), `nextn_predict_layers` (MTP), `hidden_activation`, `swiglu_clamp`.

**MoE.** `expert_count`, `expert_used_count`, `expert_shared_count`, `expert_feed_forward_length`, `expert_shared_feed_forward_length`, `expert_weights_scale`, `expert_weights_norm`, `expert_gating_func` (softmax/sigmoid), `expert_group_count`, `expert_group_used_count`, `expert_group_scale`, `moe_every_n_layers`, `interleave_moe_layer_step`.

**Atenção (`{arch}.attention.*`).** `head_count` (pode ser array por camada), `head_count_kv` (idem), `key_length`, `value_length`, `layer_norm_epsilon`, `layer_norm_rms_epsilon`, `group_norm_epsilon`, `group_norm_groups`, `causal`, `max_alibi_bias`, `clamp_kqv`, `sliding_window`, `sliding_window_pattern`, `scale`, MLA/DeepSeek: `q_lora_rank`, `kv_lora_rank`, `key_length_mla`, `value_length_mla`; indexador esparso (DeepSeek-V3.2 e similares): `indexer.head_count`, `indexer.key_length`, `indexer.top_k`; além de dezenas de chaves específicas de arquitetura (`shared_kv_layers`, `kv_source_layers`, `compress_ratios`, `temperature_*`, etc.).

**RoPE (`{arch}.rope.*`).** `dimension_count`, `dimension_sections` (M-RoPE: qwen2vl/qwen3vl), `freq_base`, `freq_base_swa`, `dimension_count_swa`, `scaling.type` (none, linear, yarn, ...), `scaling.factor`, `scaling.original_context_length`, `scaling.finetuned`, `scaling.attn_factor`, `scaling.yarn_beta_fast`, `scaling.yarn_beta_slow`, `scaling.yarn_ext_factor`, `scaling.yarn_attn_factor`, `scaling.yarn_log_multiplier`, `scaling.yarn_truncate`. (Legado: `rope.scale_linear`.)

**SSM / Mamba (`{arch}.ssm.*`).** `conv_kernel`, `inner_size`, `state_size`, `time_step_rank`, `group_count`, `dt_b_c_rms`, `norm_epsilon`. RWKV/linear-attention: `wkv.head_size`, `attention.decay_lora_rank`, `time_mix_extra_dim`, etc. (verificar nomes exatos em `Keys.WKV`).

**Tokenizer.** `tokenizer.ggml.model` (llama = SentencePiece, gpt2 = BPE byte-level, bert = WordPiece, t5, rwkv, ...), `tokenizer.ggml.pre` (nome do pré-tokenizer/regex), `tokenizer.ggml.tokens` (array string), `tokenizer.ggml.token_type` (array int32: 1 normal, 2 desconhecido, 3 controle, 4 definido pelo usuário, 5 não usado, 6 byte), `tokenizer.ggml.scores` (array float32), `tokenizer.ggml.merges` (array string "a b"), ids especiais uint32: `bos_token_id`, `eos_token_id`, `eot_token_id`, `eom_token_id`, `unknown_token_id`, `seperator_token_id` (a grafia errada é a do código), `padding_token_id`, `mask_token_id`; flags: `add_bos_token`, `add_eos_token`, `add_sep_token`, `add_space_prefix`, `remove_extra_whitespaces`, `clean_spaces`; `precompiled_charsmap` (SentencePiece), FIM: `fim_pre_token_id`, `fim_suf_token_id`, `fim_mid_token_id`, `fim_pad_token_id`, `fim_rep_token_id`, `fim_sep_token_id`; `tokenizer.chat_template` (string Jinja) e `tokenizer.chat_template.{nome}` / `tokenizer.chat_templates`; `tokenizer.huggingface.json` (tokenizer.json inteiro, legado); `tokenizer.rwkv.world`.

**Divisão em partes (split).** `split.no`, `split.count`, `split.tensors.count`; nome de arquivo `<nome>-00001-of-00003.gguf`. Cada parte é um GGUF completo; só a primeira tem os metadados completos.

**Nomes de tensores (convenção).** `token_embd.weight`, `output_norm.weight`, `output.weight`, `rope_freqs.weight`; por bloco `blk.N.attn_norm.weight`, `blk.N.attn_q.weight`, `attn_k`, `attn_v`, `attn_output`, `attn_q_norm`, `attn_k_norm`, `ffn_norm`, `ffn_gate`, `ffn_up`, `ffn_down`; MoE: `ffn_gate_inp` (roteador), `ffn_gate_exps`, `ffn_up_exps`, `ffn_down_exps`, `ffn_*_shexp` (compartilhados). A spec só fixa `blk.N.BB.weight` e `AA.weight`; os nomes completos estão em `MODEL_TENSOR` de `constants.py`.

### O que o katana pode aproveitar
- Já é o formato nativo. Ações: (1) tolerar tipos desconhecidos com mensagem clara em vez de abortar; (2) suportar MXFP4 (39) se o roteiro incluir gpt-oss; NVFP4/Q1_0/Q2_0/F8_E4M3 só quando houver modelo real; (3) ler `general.sampling.*` como padrão de amostragem; (4) ler `split.*` para modelos em partes (Qwen3-30B grande).
- Os ids/tamanhos acima servem de teste de regressão do leitor: `nbytes` somado de todos os tensores + offset de dados deve igualar o tamanho do arquivo.

### Exemplo mínimo (C, leitor de cabeçalho)
```c
// le cabecalho GGUF v3 (little-endian) e imprime contagens
uint32_t magic, ver; uint64_t nt, nkv;
fread(&magic,4,1,f); fread(&ver,4,1,f); fread(&nt,8,1,f); fread(&nkv,8,1,f);
if (magic != 0x46554747 || ver != 3) return -1;   // "GGUF" lido como uint32 LE
// depois: nkv x (string key, uint32 tipo, valor); nt x (string nome, uint32 nd, uint64 dims[nd], uint32 tipo, uint64 off)
// data_start = alinha(pos_atual, general.alignment ou 32)
```

### Fontes
- https://github.com/ggml-org/ggml/blob/master/docs/gguf.md
- https://github.com/ggml-org/ggml/blob/master/include/ggml.h
- https://github.com/ggml-org/llama.cpp/blob/master/gguf-py/gguf/constants.py
- https://huggingface.co/docs/hub/gguf

---

## 2. Safetensors

**O que é / quem mantém.** Formato simples e seguro (sem pickle, sem execução de código) para guardar tensores, com acesso zero-copy/mmap. Criado pela Hugging Face; em 8/abr/2026 foi contribuído à PyTorch Foundation (Linux Foundation), com governança neutra, e há trabalho para usá-lo no núcleo do PyTorch (fonte: blog da HF e comunicado da Foundation, via busca; ler o anúncio original para detalhes). É o formato padrão de pesos no Hub.

**Para que serve.** Distribuir pesos de LLMs, difusão, visão, áudio. Guarda só tensores: arquitetura vem de `config.json`, tokenizer de `tokenizer.json`/`tokenizer_config.json`, template de chat de `chat_template` em `tokenizer_config.json` (ou `chat_template.jinja`).

### Layout exato (README do safetensors + `tensor.rs`)

```
bytes 0..7        N : uint64 little-endian = tamanho do cabecalho JSON
bytes 8..8+N-1    cabecalho JSON UTF-8 (deve comecar com '{'; pode ter espacos finais de padding)
bytes 8+N..fim    buffer de bytes dos tensores
```

Cabeçalho JSON:
```json
{
  "__metadata__": {"format": "pt"},
  "model.embed_tokens.weight": {"dtype": "BF16", "shape": [151936, 2048], "data_offsets": [0, 622329856]},
  "model.layers.0.self_attn.q_proj.weight": {"dtype": "BF16", "shape": [2048, 2048], "data_offsets": [622329856, 630718464]}
}
```
Regras (validadas pela implementação de referência):
- `data_offsets = [início, fim]` em bytes, relativos ao início do **buffer de dados** (byte 8+N do arquivo), não do arquivo. Tamanho = fim - início.
- O buffer é totalmente indexado: sem buracos, sem sobreposição; offsets contíguos, o primeiro começa em 0, o último termina no fim do arquivo (do contrário erro `InvalidOffset` / `MetadataIncompleteBuffer`).
- `__metadata__` é opcional: mapa string→string apenas (JSON arbitrário não é permitido). Costuma ter `"format": "pt"` (ou `"mlx"`, `"tf"`, `"flax"`).
- Tamanho esperado = produto das dimensões x bits do dtype / 8; dimensão zero → tensor vazio (0 bytes, shape preservado); `shape: []` é escalar.
- Ordem: row-major (C), empacotado, sem strides, little-endian.
- Cabeçalho limitado a 100.000.000 bytes (`MAX_HEADER_SIZE`).
- Chaves duplicadas proibidas (nem todo parser verifica; o katana deve verificar). Valores não são validados (NaN/Inf possíveis).
- Ao gravar, a biblioteca preenche o cabeçalho com espaços até múltiplo de 8 (então o buffer de dados começa alinhado a 8 bytes) e ordena os tensores por dtype decrescente (alinhamento) e depois por nome. Ao ler, os tensores são reordenados por offset; arquivos antigos podem ter outra ordem. Não dependa da ordem das chaves do JSON.
- Alinhamento além de 8 bytes não é garantido: o início de um tensor F32 é múltiplo de 4 porque os tensores são ordenados por dtype (maiores primeiro), mas **não** há alinhamento de 64/4096 para mmap+GPU; o katana deve copiar ou usar mmap e confiar em ler com `memcpy`/instruções não alinhadas, ou alinhar ao enviar para a GPU.

**Dtypes (enum `Dtype` em `tensor.rs`, nomes da string JSON em maiúsculas):** `BOOL`(8 bits), `F4` (MXF4, 4 bits), `F6_E2M3`, `F6_E3M2` (6 bits), `U8`, `I8`, `F8_E5M2`, `F8_E4M3`, `F8_E8M0` (escala OCP microscaling), `F8_E4M3FNUZ`, `F8_E5M2FNUZ`, `I16`, `U16`, `F16`, `BF16`, `I32`, `U32`, `F32`, `C64` (complexo 2x32), `F64`, `I64`, `U64`. A doc do Hub (parsing) ainda lista só `F64 F32 F16 BF16 I64 I32 I16 I8 U8 BOOL`: tipos novos aparecem no código mas podem não estar em todos os leitores. Dtypes sub-byte: a biblioteca erra em leituras desalinhadas e prevê APIs não tradicionais no futuro.

**Quantização em safetensors.** Não há tipo "quantizado" no formato: cada esquema vira vários tensores comuns.
- gpt-oss (MXFP4): para cada projeção MoE existem `...experts.gate_up_proj_blocks` e `...gate_up_proj_scales` (e `down_proj_blocks`/`down_proj_scales`); o índice do gpt-oss-20b tem `total_size` 13.761.264.768 bytes e 2 shards. Os blocos são bytes U8 com dois FP4 por byte e as escalas U8 em E8M0; o shape e a ordem dos nibbles exatos devem ser lidos do código de `transformers` (integração mxfp4) antes de implementar (verificar).
- GPTQ/AWQ/compressed-tensors: tensores `qweight`, `qzeros`, `scales`, `g_idx` mais a seção `quantization_config` do `config.json` (verificar o layout de bits de cada esquema).
- FP8 (DeepSeek, Qwen3-FP8): pesos `F8_E4M3` + `weight_scale_inv` (F32/BF16) em blocos 128x128, descrito em `quantization_config` (`fmt`, `weight_block_size`) (verificar).

**Arquivos de vários shards.** `model.safetensors.index.json`: `{"metadata": {"total_size": N}, "weight_map": {"nome_do_tensor": "model-00001-of-00004.safetensors", ...}}`. O Hub também aceita `diffusion_pytorch_model.safetensors` etc. Leitura remota: HTTP Range `bytes=0-7`, depois `bytes=8-(7+N)` (a doc oficial traz o exemplo).

### Como o katana lê direto do Hub (receita)
1. Baixar `config.json`, `tokenizer.json`, `tokenizer_config.json`, `generation_config.json`, `model.safetensors[.index.json]`.
2. Ler os 8 bytes, o JSON, mmap do arquivo; para cada tensor `base + 8 + N + data_offsets[0]`.
3. Mapear nomes HF → nomes internos (por exemplo `model.layers.{i}.self_attn.q_proj.weight` → `blk.{i}.attn_q.weight`). Atenção: os pesos HF de Q/K de modelos LLaMA-like vêm com layout de RoPE "rotate_half" (pares separados por metade), enquanto os GGUF convertidos de LLaMA permutam Q/K para pares intercalados; o katana precisa escolher uma convenção de RoPE por origem do arquivo.
4. Shape HF é `[out, in]` (row-major) = `ne = [in, out]` em ggml: o mesmo bloco de memória, sem transpor.
5. Converter BF16/F16 para o formato interno na carga, ou quantizar para Q8_0/Q4_K na hora (opção `--quant`), já que a maioria dos modelos do Hub é BF16.
6. Tokenizer: implementar leitor do `tokenizer.json` (BPE byte-level com `model.vocab`, `model.merges`, `pre_tokenizer`, `added_tokens`); o chat template Jinja exige um mini-interpretador (o katana já lida com o do GGUF; reutilizar).
Esforço estimado: ~300 linhas de C para o leitor + parser JSON mínimo; o resto é o mapeamento de nomes por arquitetura (estimativa, não medida).

### Exemplo mínimo (Python, verificação rápida)
```python
import struct, json
f = open("model.safetensors", "rb")
n = struct.unpack("<Q", f.read(8))[0]
hdr = json.loads(f.read(n))
meta = hdr.pop("__metadata__", {})
for nome, t in hdr.items():
    ini, fim = t["data_offsets"]; print(nome, t["dtype"], t["shape"], 8 + n + ini)
```

### Fontes
- https://github.com/huggingface/safetensors (README, seção Format)
- https://raw.githubusercontent.com/huggingface/safetensors/main/safetensors/src/tensor.rs
- https://huggingface.co/docs/safetensors/index e /metadata_parsing
- https://huggingface.co/blog/safetensors-joins-pytorch-foundation
- https://huggingface.co/openai/gpt-oss-20b/blob/main/model.safetensors.index.json

---

## 3. DDUF (DDUF's Diffusion Unified Format)

**O que é / quem mantém.** Formato de arquivo único para pipelines de difusão, da Hugging Face (inspirado no GGUF). Um `.dduf` **é** um arquivo ZIP com restrições. Doc: https://huggingface.co/docs/hub/dduf.

**Especificação (documentação do Hub, citada quase literal):**
- ZIP **sem compressão** (método "stored", flag 0), para permitir mmap e leitura remota previsível.
- Usa ZIP64 (arquivos acima de 4 GB).
- Só pode conter arquivos `.json`, `.safetensors`, `.model` e `.txt`.
- `model_index.json` obrigatório na raiz: mapa com metadados do modelo e dos componentes (`_class_name`, `_diffusers_version`, `_name_or_path`, e uma entrada por componente).
- Cada componente em seu diretório (`vae/`, `text_encoder/`, `transformer/`...), separador `/`. Cada diretório corresponde a um componente do `model_index.json` e deve ter um JSON de configuração (um de `config.json`, `tokenizer_config.json`, `preprocessor_config.json`, `scheduler_config.json`).
- Subdiretórios proibidos (evita ZIP bombs). Imutável: para atualizar, gerar outro arquivo.
- HTTP-friendly: o diretório central do ZIP (no fim) é lido com Range.

**Estado em 2026.** A doc diz que só o Diffusers dá suporte e pede feedback; há org `DDUF` no Hub com modelos (ex.: FLUX.1-dev-DDUF), pacote `huggingface_hub` (`read_dduf_file`, `export_folder_as_dduf`, `export_entries_as_dduf`) e um verificador "very alpha" em JS (`@huggingface/dduf`). Não encontrei evidência de adoção ampla ou de mudanças em 2026 (verificar).

**Como ler (para implementar).** Procurar o End Of Central Directory (e o localizador ZIP64) no fim, percorrer o diretório central e, para cada entrada, pegar `local header offset`, `compressed size` (= `uncompressed size`), e o offset dos dados = offset do local header + 30 + len(nome) + len(extra). Dali em diante é um safetensors normal, com offsets relativos a esse ponto.

**O que o katana pode aproveitar.** Hoje, pouco: o katana é motor de linguagem. Ideia útil: "um arquivo, vários componentes, leitura por Range" já existe no GGUF; DDUF só faz sentido se o roteiro incluir geração de imagem. O leitor ZIP "stored" é ~100 linhas e serve também para o empacotamento estilo llamafile (seção 9).

**Exemplo mínimo (Python).**
```python
from huggingface_hub import read_dduf_file
import json
e = read_dduf_file("FLUX.1-dev.dduf")
print(json.loads(e["model_index.json"].read_text())["_class_name"])
with e["vae/diffusion_pytorch_model.safetensors"].as_mmap() as mm: pass  # bytes de um safetensors
```

**Fontes:** https://huggingface.co/docs/hub/dduf ; https://github.com/huggingface/huggingface.js/tree/main/packages/dduf ; https://huggingface.co/spaces/DDUF/dduf-check

---

## 4. ONNX e ONNX Runtime

**ONNX (formato).** Padrão aberto (LF AI & Data) de grafo de computação em protobuf. `onnx.proto` (main): `IR_VERSION` vai até 14 (0xE, "TBD"); a última datada é a 13 (`IR_VERSION_2025_11_06`). O ONNX Runtime 1.32 reintegrou ONNX 1.23.2 com **opset 28**.
- `ModelProto`: ir_version=1, producer_name=2, producer_version=3, domain=4, model_version=5, doc_string=6, graph=7, opset_import=8, metadata_props=14, training_info=20, functions=25, configuration=26.
- `GraphProto`: node=1, name=2, initializer=5, sparse_initializer=15, doc_string=10, input=11, output=12, value_info=13, quantization_annotation=14, metadata_props=16.
- `NodeProto`: input=1, output=2, name=3, op_type=4, attribute=5, doc_string=6, domain=7, overload=8, metadata_props=9, device_configurations=10.
- `TensorProto`: dims=1, data_type=2, segment=3, float_data=4, int32_data=5, string_data=6, int64_data=7, name=8, raw_data=9, double_data=10, uint64_data=11, doc_string=12, external_data=13, data_location=14, metadata_props=16.
- **`TensorProto.DataType`:** 0 UNDEFINED, 1 FLOAT, 2 UINT8, 3 INT8, 4 UINT16, 5 INT16, 6 INT32, 7 INT64, 8 STRING, 9 BOOL, 10 FLOAT16, 11 DOUBLE, 12 UINT32, 13 UINT64, 14 COMPLEX64, 15 COMPLEX128, 16 BFLOAT16, 17 FLOAT8E4M3FN, 18 FLOAT8E4M3FNUZ, 19 FLOAT8E5M2, 20 FLOAT8E5M2FNUZ, 21 UINT4, 22 INT4, 23 FLOAT4E2M1, 24 FLOAT8E8M0, 25 UINT2, 26 INT2, 27 FLOAT6E2M3, 28 FLOAT6E3M2.
- **Dados externos** (pesos grandes, limite de 2 GB do protobuf): `data_location = EXTERNAL (1)`, `external_data` = lista de pares chave/valor `location` (caminho relativo POSIX, obrigatório), `offset` (string; múltiplos de 4096 recomendados para mmap), `length`, `checksum` (SHA1). Na prática: `model.onnx` (grafo) + `model.onnx_data` (pesos crus).
- Quantização em ONNX: operadores `QuantizeLinear/DequantizeLinear` (por tensor/canal/bloco), `MatMulNBits` (contrib do ORT, 4 e 8 bits em blocos), `GroupQueryAttention`/`MultiHeadAttention`/`MoE`/`QMoE` (contrib), e tipos de bit baixo (INT4/UINT4/FP4/INT2) no enum acima.

**ONNX Runtime (runtime).** Microsoft. Estado em 2026: release mensal; **v1.32.0 em 13/nov/2026** (última na página de releases), com offload CPU/GPU de MoE, API de capacidade de memória do dispositivo, "weightless compilation", Python Model Package; v1.31.0 (9/out) estabilizou APIs de model-package e EPContext e colocou CoreML na seleção de EP por dispositivo; v1.30.0 (10/set) ampliou geração de texto e adicionou bindings Go. O provedor ACL foi removido na 1.32; o NvTensorRTRTX legado foi removido em favor de um plugin EP separado.
- Execução: o grafo é carregado, otimizado (fusões, constant folding, layout), particionado entre **Execution Providers** (EP) por capacidade de operador; nós que um EP não suporta caem para o CPU. EPs: CPU (MLAS), CUDA, TensorRT / TensorRT-RTX, DirectML, WebGPU (Dawn: D3D12/Vulkan/Metal), WebNN, OpenVINO, QNN (Qualcomm), CoreML, ROCm/MIGraphX, XNNPACK, NNAPI e outros. Novidade de 2025-2026: **Plugin EPs** (bibliotecas carregadas em tempo de execução, ABI estável: CUDA Plugin EP 0.4.0, WebGPU Plugin EP 0.5.0). **EPContext**: nó que guarda o blob já compilado de um EP (evita recompilar).
- Formato `.ort`: serialização FlatBuffers otimizada para ORT mobile/reduzido (conhecimento geral, sem fonte aberta nesta pesquisa; verificar).
- LLMs: **onnxruntime-genai** faz o laço de geração (KV cache, amostragem, beam) em cima do ORT (não aparece na página de releases lida; verificar versão).

**O que o katana pode aproveitar.** (1) Lista de operadores e semântica (referência de "o que é um bloco de atenção" padronizado). (2) A ideia de EP plugável: separar o núcleo do motor dos backends (Vulkan, OpenCL, CPU) por tabela de funções carregada por `dlopen`, o que já se alinha ao katana. (3) Não vale ler ONNX de LLM: modelos exportados trazem grafo fixo com opset e contrib ops específicos; o GGUF/safetensors é mais direto.

**Exemplo mínimo (Python).**
```python
import onnxruntime as ort, numpy as np
s = ort.InferenceSession("model.onnx", providers=["CPUExecutionProvider"])
print([i.name for i in s.get_inputs()])
```

**Fontes:** https://github.com/onnx/onnx/blob/main/onnx/onnx.proto ; https://github.com/microsoft/onnxruntime/releases ; https://onnxruntime.ai/docs/execution-providers/WebGPU-ExecutionProvider.html ; https://onnxruntime.ai/docs/execution-providers/TensorRTRTX-ExecutionProvider.html

---

## 5. OpenVINO

**O que é / quem mantém.** Toolkit da Intel para otimizar e executar inferência em CPU, GPU integrada/discreta e NPU Intel. Formato próprio **IR**: um `.xml` (topologia) e um `.bin` (pesos). Converte de PyTorch, TensorFlow, ONNX, TFLite, PaddlePaddle, JAX/Flax, Keras. Compressão de pesos via **NNCF** (INT8, INT4, e quantização microscaling MX). Há **OpenVINO GenAI** (pipelines de LLM, VLM, Whisper, difusão, TTS) e o OpenVINO Model Server.

**Estado em 2026.** Releases: 2026.0, 2026.1 (inclui backend para llama.cpp, segundo a imprensa; verificar), 2026.2, 2026.3, **2026.4 (16/set/2026 segundo a página oficial de release notes via busca)**, com patches (2026.3.1 etc.). Destaques: decodificação especulativa EAGLE-3 (LLM e VLM), MTP para Gemma 4, Qwen3.5 e Qwen3.6, preview de DFlash para Qwen em GPU, TaylorSeer Lite para difusão. Um demo do Model Server menciona carregar GGUF; o suporte de leitura nativa de GGUF não está confirmado (verificar).

**Como executa.** Modelo → `ov::Model` (grafo) → passes de transformação → plugin do dispositivo (CPU, GPU, NPU, AUTO, HETERO, MULTI) compila → requisições de inferência assíncronas. Estado de LLM via "stateful models" (KV cache como variável). 

**O que o katana pode aproveitar.** Alvo Intel (iGPU/NPU) só via OpenVINO; o katana usa Vulkan/OpenCL. Ideias: decodificação especulativa (EAGLE-3/MTP), cache de KV estático para NPU, compressão de pesos em grupos de tamanho configurável. Não ler IR.

**Exemplo mínimo (Python).**
```python
import openvino_genai as ov_genai
pipe = ov_genai.LLMPipeline("modelo_ov_dir", "CPU")
print(pipe.generate("Ola", max_new_tokens=32))
```

**Fontes:** https://docs.openvino.ai/releasenotes ; https://github.com/openvinotoolkit/openvino/releases ; https://docs.openvino.ai/2026/openvino-workflow/model-preparation.html

---

## 6. Core ML

**O que é / quem mantém.** Framework da Apple para inferência no iOS/macOS/etc. (CPU, GPU e Neural Engine). Formatos: `.mlmodel` (protobuf único; legado) e **`.mlpackage`** (diretório: `Manifest.json`, `Data/com.apple.CoreML/model.mlmodel` e `weights/weight.bin`). Tipos de modelo: **mlprogram** (programa de operações MIL, spec v5+, recomendado) e neuralnetwork (legado). Conversão por `coremltools` (9.0 em 10/nov/2025; conda-forge de fev/2026). Compressão: paletização k-means (1 a 8 bits), quantização linear, esparsidade. A doc nota limite de ~1 GB para modelos no Neural Engine de iPhone.

**Estado em 2026.** Ativo, mas há indicação de um novo framework "Core AI" com formato `.aimodel` para iOS 27/macOS 27 (fonte secundária: documentação da Ultralytics; **verificar** na Apple). Um relato não oficial no Hub aponta path traversal no parsing de `.mlpackage` do coremltools 9.0 (sem CVE encontrado; **verificar**). Para LLM em Apple, o caminho comum é MLX ou llama.cpp/Metal, não Core ML.

**O que o katana pode aproveitar.** Quase nada (Linux/Windows). Lição de segurança: nunca confiar em caminhos relativos dentro de manifestos; vale também para o leitor DDUF/ZIP do katana.

**Exemplo mínimo (Python, macOS).**
```python
import coremltools as ct
m = ct.convert(traced, inputs=[ct.TensorType(shape=(1, 3, 224, 224))], convert_to="mlprogram")
m.save("m.mlpackage")
```

**Fontes:** https://apple.github.io/coremltools/docs-guides/source/mlmodel-utilities.html ; https://pypi.org/project/coremltools/

---

## 7. LiteRT (ex-TensorFlow Lite) e LiteRT-LM

**O que é / quem mantém.** Google (AI Edge). Renomeado de TensorFlow Lite em set/2024; o formato `.tflite` (FlatBuffers) **não mudou**. Converte de PyTorch (`litert-torch`), TensorFlow e JAX. **LiteRT 2.x** traz a API **CompiledModel** (recomendada) e mantém a API Interpreter antiga por compatibilidade. **LiteRT-LM** é a camada de orquestração para LLMs (streaming, KV cache, tool calling; Gemma, Llama, Qwen), com arquivos `.litertlm` (fonte: post do Google de 28/jan/2026 via busca; o fato de `.litertlm` ser distinto de `.tflite` vem de blog de terceiros, verificar).

**Como executa.** Grafo FlatBuffers → delegados/aceleradores: CPU (XNNPACK), GPU (OpenCL, OpenGL, Metal, WebGPU ou Vulkan conforme a plataforma), NPU (Qualcomm, MediaTek, Google Tensor, Samsung Exynos); trocar de acelerador é opção de compilação e há fallback por operador. Quantização com **AI Edge Quantizer**. SDKs: Python (`ai-edge-litert`), Android Kotlin/C++, Web (`@litertjs/core`), C++ (CMake) para Linux/Windows/embarcados, Swift via LiteRT-LM (LiteRT para iOS ainda em pré-lançamento na doc). Versões citadas por terceiros: LiteRT 2.1.x e LiteRT-LM 0.13 (verificar).

**O que o katana pode aproveitar.** Particionamento por operador com fallback (útil se o katana ganhar mais de um backend por modelo); NPUs via LiteRT são caminho separado do Vulkan. Formato não é alvo de leitura.

**Exemplo mínimo (Python).**
```python
from ai_edge_litert.interpreter import Interpreter
it = Interpreter(model_path="m.tflite"); it.allocate_tensors()
print(it.get_input_details()[0]["shape"])
```

**Fontes:** https://developers.google.com/edge/litert/overview ; https://developers.googleblog.com/tensorflow-lite-is-now-litert/ ; https://developers.googleblog.com/en/litert-the-universal-framework-for-on-device-ai/

---

## 8. ExecuTorch

**O que é / quem mantém.** Runtime de inferência no dispositivo do ecossistema PyTorch (Meta, com Arm, Apple, Qualcomm e outros). **1.0 GA em out/2025** (há 1.0.1). Fluxo: `torch.export` → lowering (`to_edge_transform_and_lower`) para um ou mais **backends/delegates** → `to_executorch` gera `.pte` (FlatBuffers). Recomenda-se um `.pte` por backend, pois cada arquivo é especializado.

**Backends (1.0).** XNNPACK (CPU), Core ML/MPS (Apple), Qualcomm QNN, **Vulkan GPU (promovido a produção)**, Arm Ethos-U e VGF, NXP eIQ Neutron, Samsung Exynos, MediaTek, Cadence DSP, OpenVINO; Apple MPS e alguns outros em alfa/beta. Também: LoRA com vários `.pte` compartilhando os pesos base, quantização 4 bits HQQ, JS experimental via WASM (via notas de release e blog do 1.0; verificar versões atuais).

**O que o katana pode aproveitar.** O backend Vulkan do ExecuTorch é a referência pública mais próxima do que o katana faz em GPU (shaders GLSL → SPIR-V, grafo compilado); vale consultar a estrutura de memória planejada em tempo de compilação ("memory planning": todo o uso de memória do grafo é decidido na exportação). Não é alvo de leitura de formato.

**Exemplo mínimo (Python).**
```python
import torch
from executorch.exir import to_edge_transform_and_lower
ep = torch.export.export(model.eval(), (x,))
et = to_edge_transform_and_lower(ep).to_executorch()
open("m.pte", "wb").write(et.buffer)
```

**Fontes:** https://pytorch.org/blog/introducing-executorch-1-0/ ; https://docs.pytorch.org/executorch/stable/intro-overview.html ; https://docs.pytorch.org/executorch/1.0/backends-overview.html

---

## 9. llamafile

**O que é / quem mantém.** Mozilla.ai (criado por Justine Tunney, autora do Cosmopolitan Libc): combina llama.cpp com Cosmopolitan para gerar **um único executável** que roda em Linux, macOS, Windows, BSD e várias arquiteturas, com os pesos GGUF embutidos. Inclui também whisperfile (fala-para-texto) e, na 0.10.4 (jul/2026), "transcribefile" (cosmocc). Licença Apache 2.0 (alterações em llama.cpp/whisper.cpp em MIT).

**Estado em 2026.** Ativo: **0.10.0 (mar/2026)** reescreveu o sistema de build, acompanha o llama.cpp mais de perto e trouxe suporte de GPU (algumas funções antigas podem faltar); 0.10.4 (jul/2026); 0.10.5 (ago/2026, modelos grandes novos). Windows só executa binários abaixo de 4 GB: llamafiles maiores precisam de GGUF externo (README).

**Como funciona (conhecimento geral sobre Cosmopolitan/APE; o README lido não detalha, verificar na documentação do Cosmopolitan).** O executável é um "Actually Portable Executable": um arquivo que é ao mesmo tempo script de shell, PE do Windows e ELF/Mach-O, e que também é um arquivo ZIP (o diretório central fica no fim). Os pesos entram como entrada ZIP **sem compressão** e alinhada (ferramenta `zipalign`), de modo que o programa abre o próprio executável e faz mmap direto do GGUF. Módulos de GPU (CUDA/ROCm/Metal) são compilados na primeira execução com o compilador do sistema, ou carregados por dlopen.

**O que o katana pode aproveitar (pergunta explícita do pedido).**
1. **Empacotar modelo no executável:** viável sem Cosmopolitan. Truque do ZIP/trailer: anexar o GGUF ao final do binário do katana e gravar no fim um rodapé (magic + offset + tamanho). Na inicialização, o katana abre `/proc/self/exe` (Linux) ou `GetModuleFileName` (Windows) e faz mmap do trecho. O alinhamento do GGUF (`general.alignment`) deve ser respeitado em relação ao offset absoluto no arquivo (alinhar o início do GGUF a 4096).
2. **Um binário por plataforma:** Linux e Windows continuam com dois executáveis (o katana carrega Vulkan/OpenCL dinamicamente, o que o llamafile resolve com seu mecanismo próprio); portabilidade APE total exige Cosmopolitan, que conflita com a regra "C puro, sem dependências" — decisão a tomar, não prioritária.
3. Limite de 4 GB no Windows para executáveis: manter a opção de GGUF externo.
4. Ideia de produto: `katana pack modelo.gguf -o app.exe` (gravar arquivo e executar comando continuam exigindo confirmação do usuário, conforme as regras do projeto).

**Exemplo mínimo.**
```sh
chmod +x meu-modelo.llamafile && ./meu-modelo.llamafile          # servidor local + UI
./meu-modelo.llamafile --cli -p "Ola"                            # linha de comando
```

**Fontes:** https://github.com/mozilla-ai/llamafile ; https://www.helpnetsecurity.com/2026/03/20/llamafile-0-10-0-released/ ; https://phoronix.com/news/Llamafile-0.10.4

---

## 10. Transformers.js

**O que é / quem mantém.** Biblioteca JavaScript da Hugging Face (`@huggingface/transformers`) que roda modelos do Hub no navegador e no Node/Deno/Bun, com API de `pipeline()` análoga à do `transformers` Python. Executa via **ONNX Runtime Web/Node**; os modelos são exportados para ONNX (Optimum) e ficam em subpasta `onnx/` do repositório, com variantes de quantização (`model_fp16.onnx`, `model_q4.onnx`, `model_quantized.onnx`...).

**Estado em 2026.** **v4**: preview em 9/fev/2026 (tag `@next`), versão completa em mar/2026 (relatos de terceiros; 4.2.0 em 23/abr/2026, verificar versão atual). Novidade principal: runtime WebGPU reescrito em C++ em colaboração com o time do ONNX Runtime; ganhos reportados de até 4x em modelos tipo BERT (número de terceiros, verificar). Um `npm install` sem tag pode instalar v3 (nota de terceiros; verificar).

**O que o katana pode aproveitar.** (1) Catálogo de tarefas/pipelines e como o Hub distribui pesos por dtype. (2) Se o katana ganhar UI web: WebGPU/WASM é outro alvo, não prioritário. (3) Nada de formato.

**Exemplo mínimo.**
```js
import { pipeline } from "@huggingface/transformers";
const gen = await pipeline("text-generation", "onnx-community/Qwen3-0.6B-ONNX", { device: "webgpu", dtype: "q4" });
console.log(await gen("Ola", { max_new_tokens: 32 }));
```

**Fontes:** https://huggingface.co/blog/transformersjs-v4 ; https://github.com/huggingface/transformers.js/releases

---

## 11. unity-sentis (Unity Sentis / "Inference Engine")

**O que é / quem mantém.** Pacote da Unity para executar redes neurais dentro do jogo, em todas as plataformas de runtime do Unity. Sucedeu o Barracuda. Nome: **Sentis** (pacote `com.unity.sentis` até 2.1) → **Inference Engine** (2.2, id `com.unity.ai.inference`) → de volta a **Sentis** no nome de exibição (2.4 e 2.5); o id do pacote ficou `com.unity.ai.inference`. Versão lida na doc: **2.6.1**. Namespace de código: verificar (relatos conflitantes).

**Formatos e execução.** Importa **ONNX** (opsets 7 a 25), **LiteRT/TFLite** e **PyTorch** (exported programs decompostos em Core ATen IR). Executa em CPU ou GPU (compute shaders); a lista de operadores suportados tem páginas próprias por formato de origem. A página lida não cita quantização nem formato `.sentis` serializado (verificar).

**O que o katana pode aproveitar.** Nada direto. Só as listas de operadores como checklist de cobertura. Útil como caso de uso de "inferência em jogos": latência por quadro, fatiamento de trabalho de GPU entre quadros.

**Exemplo mínimo (C#).** Verificar na doc do pacote (API mudou entre `Unity.Sentis` e `Unity.InferenceEngine`); não incluo código sem fonte.

**Fontes:** https://docs.unity3d.com/Packages/com.unity.ai.inference@2.6/manual/index.html ; https://docs.unity3d.com/Packages/com.unity.ai.inference@2.5/changelog/CHANGELOG.html

---

## 12. Joblib

**O que é / quem mantém.** Biblioteca Python (projeto da comunidade scikit-learn/joblib) para paralelismo simples e **persistência de objetos Python**: `joblib.dump/load`, otimizada para objetos com grandes arrays NumPy. Versão mais recente que achei: 1.5.3 (15/dez/2025), via Snyk e errata do ALT Linux (verificar no PyPI).

**Formato.** É **pickle** (protocolo do Python) com extensões: arrays NumPy são gravados em separado, em bruto, para permitir `mmap_mode`; suporta compressão (zlib, gzip, bz2, lzma/xz, lz4) escolhida por argumento ou pela extensão do arquivo. Detalhes de byte (marcador do `NumpyArrayWrapper`, alinhamento) não foram lidos (a página de persistência da doc devolveu 404 na pesquisa; verificar em `joblib/numpy_pickle.py`). **Não é formato de intercâmbio**: carregar executa código arbitrário do arquivo.

**Segurança.** A doc oficial diz que `joblib.load` nunca deve ser usado com fonte não confiável, propriedade fundamental do pickle. O scikit-learn recomenda ONNX como o mais seguro para servir modelos e `skops.io` para recuperar objetos Python sem execução de código. Em 2026, o pyod 3.6.2 passou a exigir `trusted=True` para carregar artefatos pickle/joblib.

**O que o katana pode aproveitar.** Nada como entrada. Em `katana`, se um usuário apontar um `.joblib`/`.pkl`/`.pt` (pickle), **recusar com mensagem**: "formato inseguro, converta para safetensors". Isso reforça a decisão de suportar só GGUF e safetensors.

**Exemplo mínimo.**
```python
import joblib
joblib.dump(modelo, "m.joblib", compress=3)
modelo = joblib.load("m.joblib")   # so com arquivos de confianca
```

**Fontes:** https://joblib.readthedocs.io/ (persistência) ; https://scikit-learn.org/stable/model_persistence.html

---

## 13. TensorBoard

**O que é / quem mantém.** Painel web de visualização de experimentos do TensorFlow (repositório `tensorflow/tensorboard`), usado também com PyTorch (`torch.utils.tensorboard`) e outros. O repositório está ativo em 2026 (rodapé 2026, ~5.960 commits; a página não listava releases, versão atual: verificar). Funções: escalares (loss, lr), histogramas/distribuições, imagens, áudio, grafo, embedding projector, texto, séries temporais e sistema de plugins.

**Formato dos logs (tfevents).** Cada execução grava arquivos append-only em um diretório; cada subdiretório com eventos é uma "run".
- Nome: `events.out.tfevents.<tempo 10 dígitos>.<hostname>.<pid>.<uid>[sufixo]`.
- Conteúdo: sequência de registros no formato **TFRecord**; cada registro é uma mensagem protobuf `Event`. O primeiro evento tem `wall_time`, `file_version = "brain.Event:2"` e `source_metadata`.
- Framing de TFRecord (conhecimento geral da especificação do TFRecord; o código lido não o mostra, verificar em `record_writer.py`): `uint64 comprimento` (LE), `uint32 CRC32C mascarado do comprimento`, `dados`, `uint32 CRC32C mascarado dos dados`, com máscara `((crc >> 15) | (crc << 17)) + 0xa282ead8`.
- `Event` carrega `wall_time`, `step` e um de `file_version`, `graph_def`, `summary` (valores `simple_value`, `histo`, `image`, `audio`, `tensor`...), etc. (campos além dos citados: verificar no `event.proto`).

**O que o katana pode aproveitar.** Opcional e de baixa prioridade: `katana bench --tfevents dir` gravando tokens/s e latência por commit, para visualizar a deriva da máquina no TensorBoard (o escritor é ~80 linhas de C: protobuf escrito à mão + CRC32C). Isso se alinha à regra do projeto de medir sempre alternado. Alternativa mais simples: CSV.

**Exemplo mínimo (Python).**
```python
from torch.utils.tensorboard import SummaryWriter
w = SummaryWriter("runs/katana"); w.add_scalar("tok_s", 123.4, 1); w.close()
# tensorboard --logdir runs
```

**Fontes:** https://github.com/tensorflow/tensorboard ; https://github.com/tensorflow/tensorboard/blob/master/tensorboard/summary/writer/event_file_writer.py

---

## Recomendações ao katana (ordem sugerida)

1. Safetensors direto do Hub (config.json + tokenizer.json + mapeamento de nomes + conversão BF16 na carga). Maior ganho de usabilidade, risco baixo.
2. GGUF: tolerância a tipos novos, MXFP4 (id 39) para gpt-oss, `split.*`, `general.sampling.*`.
3. Empacotamento estilo llamafile via anexo no fim do executável (sem Cosmopolitan).
4. Recusar pickle/joblib explicitamente.
5. Itens de baixa prioridade: escritor tfevents para benchmarks; consulta ao backend Vulkan do ExecuTorch e à decodificação especulativa do OpenVINO como ideias.

## Lacunas desta pesquisa (verificar)
- Layout de bits de Q1_0, Q2_0, NVFP4, F8_E4M3 no GGUF (só ids e tamanhos confirmados).
- Shape/ordem de nibbles do MXFP4 dentro de safetensors do gpt-oss; layouts GPTQ/AWQ/FP8 por bloco.
- Detalhes internos do APE/zipalign do llamafile e do pickle do joblib; framing TFRecord (descrito de memória).
- "Core AI/.aimodel" da Apple; versões exatas de LiteRT e LiteRT-LM; versão atual do TensorBoard e do Transformers.js; onnxruntime-genai.
