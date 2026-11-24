# katana embed (EmbeddingGemma 2, text only)

Source: `src/app/embed.c` (encoder, `katana embed`, `embed_text()`), `src/app/convert.c` (checkpoint -> GGUF,
architecture `embeddinggemma2`). Status (2026-11-23): written, compiled, checked against the reference implementation
on 7 texts (see "Verification"). **Not connected to `Find` or to the coder index**: that is a separate step. The model
is an optional search filter (see `docs/embedding-usage.md`): it reorders candidates by meaning, it never decides and
never excludes.

```
katana embed <model.gguf> [--task code|search|document|raw] [--title T] [--dim 128|256|512|768] [--json] [--cosine] [--ids] "text"...
katana convert <hf-folder> <out.gguf> [f32|f16|bf16|q8_0|...]      # default for this architecture: q8_0
```

```c
EmbModel *m = xalloc(1, sizeof *m);
emb_load(m, "embeddinggemma-2-q8_0.gguf");                          /* once */
float v[768];
embed_text(m, "sort an array", EMB_CODE, NULL, 768, v);             /* query: "task: code retrieval | query: ..." */
embed_text(m, code, EMB_DOCUMENT, "sort.c", 256, v256);             /* document: "title: sort.c | text: ..." (NULL = none) */
```

`embed_text(EmbModel*, const char *text, int task, const char *title, int dim, float *out)` returns `dim` (or -1),
`out` is L2-normalised. Tasks: `EMB_CODE`, `EMB_SEARCH`, `EMB_DOCUMENT`, `EMB_RAW` (text already carries its prefix).
`dim` 0 means native (768); otherwise one of the Matryoshka sizes stored in the GGUF. `m->last_tokens` is the token count
of the last call. Not thread safe (one call at a time; each call uses the engine's thread pool).

## Model (google/embeddinggemma-2)

Files downloaded to `models/embeddinggemma-2/` (outside git): `config.json`, `config_sentence_transformers.json`,
`modules.json`, `sentence_bert_config.json`, `1_Pooling/config.json`, `2_Normalize/config.json`, `tokenizer.json`,
`tokenizer.model`, `tokenizer_config.json`, `chat_template.jinja`, `README.md`, `model.safetensors` (1.49 GB: text, vision
and audio towers together, 1376 tensors; only the 413 `language_model.*` tensors are converted, the other 963 are skipped).
The repository has no modeling code and no `2_Dense` module: the architecture was read from the `embedding_gemma2`
implementation in the Hugging Face `transformers` package (version 5.24, `modeling_embedding_gemma2.py`), and the
projection is inside the checkpoint (`language_model.embedding_projection.weight`).

What the config and the code say (the model page leaves these open):

| Question | Answer | Source |
|---|---|---|
| Attention bidirectional? | **Yes, on every layer** (`is_causal = False`; masks `create_bidirectional_mask` / `create_bidirectional_sliding_window_mask`) | modeling code |
| Sliding window | `sliding_window = 512` in `config.json` (the page says 1024). The mask keeps `abs(i - j) <= 512` (inclusive, both sides), so a local token sees up to 1025 positions | config + `masking_utils.py` |
| Layers | 24: layers 5, 11, 17, 23 are global (`full_attention`), the other 20 are local (`sliding_attention`) | `layer_types` |
| Heads | 4 query heads; local layers: 2 KV heads, head_dim 256; global layers: 1 KV head, head_dim 512 | `per_layer_config` |
| Model / FFN width | 512 / 2048, gated FFN with `gelu_pytorch_tanh` | config |
| Attention scale | **1.0** (no 1/sqrt(d)); q and k get an RMSNorm per head (with weight), v gets an RMSNorm per head **without weight**; RoPE on the whole head (`rotate_half` pairing i, i + d/2), base 1,000,000 (global) / 10,000 (local) | modeling code |
| RMSNorm | `x * w` (plain weight, **not** `1 + w`); eps 1e-6; computed in float32 | modeling code |
| Embedding | `embed_tokens[id] * sqrt(512)` | modeling code |
| Per-layer inputs (PLE) | "projection-only": `per_layer_model_projection(h0) * 512^-0.5`, reshaped `[24, 512]`, RMSNorm with `ple.per_layer_projection_norm` (no token-identity table). Each layer ends with a third residual block: `h += RMSNorm(per_layer_projection(gelu(per_layer_input_gate(h)) * pli[l]))` | modeling code |
| Layer end | `h *= layer_scalar` (a stored scalar per layer) | modeling code |
| Layer order | `h += post_attn_norm(attn(input_norm(h)))`; `h += post_ffn_norm(mlp(pre_ffn_norm(h)))`; PLE block; scalar | modeling code |
| After the layers | final RMSNorm, linear projection 512 -> 768 (per token, equals projecting after the mean), **mean pooling over all tokens** (prompt included: `include_prompt: true`), then L2 normalisation (`2_Normalize`) | `1_Pooling`, `modules.json`, modeling code |
| Tokens | `<bos> ... <eos>` from the tokenizer's post-processor; space -> U+2581; vocabulary 262144; same tokenizer family as Gemma 4 | `tokenizer.json` |
| Matryoshka | 768/512/256/128: keep the first `d` values and **normalise again** | README |
| Prefixes | `task: code retrieval \| query: `, `task: search result \| query: `, `title: none \| text: ` (from `config_sentence_transformers.json`; stored in the GGUF) | config |
| Precision | float32 or bfloat16; **float16 overflows** (NaN). The engine's code for this model is float32 only and never touches the fp16 paths | README |

Parameters: the text part is 0.27 G (130 M transformer + 134 M token embedding + 6 M PLE projection); the page's 740 M
includes the 170 M vision and 300 M audio encoders.

### License

The page says `apache-2.0` and links to `https://ai.google.dev/gemma/docs/gemma_4_license`. That page (read on
2026-11-23) is titled "Apache License 2.0" and contains the plain Apache License 2.0 text (no extra use policy, no
acceptable-use annex). The repository has **no LICENSE file** (the file list of the model API has none), so the licence is
the one stated in the model card header and on that page. This meets the "nothing proprietary" rule; if the repository
later ships a LICENSE or NOTICE, redistribution of the converted GGUF should keep it. The conversion adds nothing to the
weights.

## GGUF layout (`embeddinggemma2`)

Written by `katana convert`; nothing about the model is fixed in the engine except the forward pass itself.

| Key (`embeddinggemma2.` prefix) | Value |
|---|---|
| `block_count`, `embedding_length`, `feed_forward_length`, `attention.head_count` | 24, 512, 2048, 4 |
| `attention.head_count_kv`, `attention.head_dim_list` | int32 arrays per layer (2/1, 256/512) |
| `attention.sliding_window_pattern` | bool array per layer (1 = local) |
| `attention.key_length`, `attention.key_length_swa`, `attention.sliding_window`, `attention.causal` | 512, 256, 512, false |
| `rope.freq_base`, `rope.freq_base_swa`, `attention.layer_norm_rms_epsilon` | 1e6, 1e4, 1e-6 |
| `ple_length`, `embedding_dim`, `pooling_type`, `matryoshka_dims`, `context_length` | 512, 768, 1 (mean), [128, 256, 512, 768], 8192 (capped from the config's 262144 to the page's window) |
| `prompt.code`, `prompt.search`, `prompt.document_title`, `prompt.document_text` | the prefixes (`title: ` and ` \| text: ` for documents) |
| `tokenizer.ggml.model = gemma4`, tokens, token types (3 control, 4 user, 6 byte `<0xNN>`), merges, `bos/eos/padding` ids, `add_bos_token`, `add_eos_token` | from `tokenizer.json` |

Tensors: `token_embd.weight`, `ple_proj.weight`, `ple_norm.weight`, `embd_proj.weight`, `output_norm.weight`; per layer
`blk.N.{attn_norm, attn_q, attn_k, attn_v, attn_output, attn_q_norm, attn_k_norm, post_attention_norm, ffn_norm, ffn_gate,
ffn_up, ffn_down, post_ffw_norm, ple_gate, ple_out, ple_post_norm, layer_scale}.weight`. No weight is transformed (norms
are stored as they are, not `+ 1`). 1-D tensors are always F32; matrices follow the requested type. The engine dequantises
the matrices to float32 at load (about 0.55 GB of RAM) and reads the embedding rows from the mapped file on demand.

Files (this machine): `embeddinggemma-2-f32.gguf` 1,098,751,652 bytes; `embeddinggemma-2-q8_0.gguf` 302,907,044 bytes
(0.27 GB of tensors plus 0.03 GB of vocabulary and merges). Conversion takes about 1 s.

## Verification

A float32 reference was built in a scratch directory (pip `--target`, no virtualenv) with `transformers` 5.24 and CPU `torch` 2.15
(`EmbeddingGemma2TextModel` from `modeling_embedding_gemma2.py`, weights from the checkpoint in float32, mean pooling and
L2 normalisation done by hand, `AutoTokenizer` for ids). Seven prefixed texts: Portuguese question, Portuguese passage,
English sentence, code query, a 13-line C function (161 tokens), accented/CJK/emoji text with tabs and blank lines, and a
1505-token source excerpt (so the sliding window of 512 is active).

| GGUF | cosine against the reference (768d) | token ids |
|---|---|---|
| F32 | 0.99999999 to 1.00000010 on all 7 texts | same count as the reference on all 7 (ids were not compared one by one; a cosine of 1.0 implies the same ids) |
| Q8_0 | 0.99990 to 0.99995 (first 6 texts; the 1505-token one was run only in F32) | same |

Sanity tests (Q8_0): 10 C snippets as documents (`title: file | text: code`) and 10 queries (English and Portuguese,
`task: code retrieval`): **top-1 correct 10/10 at 256d and at 768d**, scores 0.77 to 0.86. Related sentence pairs (English
with its Portuguese translation, paraphrases) 0.88 to 0.94 against 0.48 to 0.59 for unrelated sentences. 256d equals the
truncation plus renormalisation of 768d (cosine 1.000000).

## Cost (observation, not a benchmark)

Measured while a training job held the GPU and most of the CPU (load average about 10 on 16 threads), with
`nice -n 19` and `KATANA_THREADS=2`, so these numbers are pessimistic and noisy: model load 0.45 to 0.55 s; 18 tokens
about 0.2 s; 161 tokens about 1.9 s; 1505 tokens about 40 s (attention grows with the square of the length). That is
about 10 to 25 ms per token on two busy threads. Unloaded numbers, more threads and the effect of Q8_0 vs F32 on speed
were **not** measured. A 40-line chunk is roughly 300 to 500 tokens.

## Not verified / limits

* Only the text tower exists in the engine. Images, audio and video are ignored; the three placeholder tokens in the
  vocabulary are not special-cased.
* The window rule was checked against the reference only on one 1505-token text; the page says 1024 and the config 512
  (the code uses the config, which is what the reference matched). Texts above 8192 tokens are cut (the last token kept is
  `<eos>`).
* Pure float32 matrix products with no batching tricks: the cost above is not tuned. No GPU path (by design for now).
* bf16 and the K-quant types were not run for this model (they should load: the loader uses the engine's dequantiser).
* Quality was checked on 10 snippets and 3 sentence pairs, not on a retrieval benchmark.
* `katana embed` has no batch/stdin mode yet; the indexer should link `embed_text()` and keep the model loaded.
