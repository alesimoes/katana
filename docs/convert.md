# katana convert

Reads a Hugging Face checkpoint and writes a GGUF the engine loads. Source: `src/app/convert.c`. Status: **written and
checked on tiny checkpoints only; no conversion of the real 35B model has been run** (see "Not verified").

```
katana convert <hf-folder> <out.gguf> [type]
katana convert <hf-folder> --list          # tensors found in the safetensors files (name, dtype, shape, bytes)
```

`type`: `q4_k_m` (default), `q4_k`, `q5_k_m`, `q6_k`, `q8_0`, `f16`, `bf16`, `f32`. Case does not matter. The output file
must not exist. The data goes to `<out>.part` and is renamed at the end, so a stopped run leaves no half GGUF under the
final name.

## What it reads

| File | Use |
|---|---|
| `*.safetensors` (one or many) | weights. `model.safetensors.index.json`, when present, decides the file list; otherwise every `*.safetensors` in the folder, sorted. dtypes: BF16, F16, F32 (anything else is an error naming the tensor) |
| `config.json` | hyperparameters (`text_config` when present, else the top level); `architectures`/`model_type` pick the architecture |
| `tokenizer.json` | BPE vocabulary (`model.vocab`), merges (`"a b"` strings or `["a","b"]` pairs), `added_tokens` (with `special`) |
| `tokenizer_config.json` | `eos_token`, `bos_token`, `pad_token`, `add_bos_token`, `chat_template` (string or list of `{name, template}`) |
| `chat_template.jinja` / `chat_template.json` | the chat template, when it lives outside `tokenizer_config.json` (they win) |
| `generation_config.json` | `top_k`, `top_p`, `temperature` -> `general.sampling.*` |

Architectures: `Qwen3_5MoeForConditionalGeneration` / `Qwen3_5MoeForCausalLM` -> `qwen35moe` (Qwen3.6-35B-A3B: gated
attention + Gated DeltaNet hybrid, MoE with 256 experts, 8 active, and a gated shared expert), and
`Qwen3_5ForConditionalGeneration` / `Qwen3_5ForCausalLM` -> `qwen35` (dense FFN variant). Anything else stops with a
message. Vision (`visual.*`) and multi-token-prediction (`mtp.*`) tensors are skipped and counted; the vision projector
the engine uses comes from a separate GGUF, as with the downloaded model.

## Memory

Nothing is loaded whole. Every safetensors file is mapped (`mmap`); the GGUF is planned in a first pass (the tensor table
needs final sizes, so the types are chosen from shapes alone) and written in a second pass, one tensor at a time, in
slices of rows (at most 16M elements): read the rows, apply the permutation, convert to float, encode (same encoders as
`katana quantize`), write. The worker threads of the engine's pool split the rows of a slice. After each tensor the mapped
pages it read are returned with `madvise(MADV_DONTNEED)` (not on Windows). The peak is one slice (tens of MB), plus the
page cache the kernel chooses to keep, plus the JSON of `tokenizer.json` (about 30 MB for 248k tokens) and the KV block in
memory (about 10 MB). The experts of a layer in the old one-tensor-per-expert layout are stacked straight into the output
slice, never into a temporary 3D tensor.

## Name mapping and transformations

`P` is `model.language_model.` (or `model.` when the checkpoint has no such prefix). Layer `l`. GGUF dimensions are the
HF shape reversed (`ne0` is the contiguous axis), so a `[out, in]` HF matrix is the GGUF `(in, out)` with the same bytes:
**nothing is transposed**; tensors are only reordered where the table says so.

| HF | GGUF | Transformation |
|---|---|---|
| `P embed_tokens.weight` | `token_embd.weight` | none |
| `P norm.weight` | `output_norm.weight` | `+ 1` (the HF RMSNorm is `x * (1 + w)`; the engine reads the whole factor) |
| `lm_head.weight` | `output.weight` | none; absent means tied weights and `output.weight` is left out (the engine then uses `token_embd`) |
| `P layers.l.input_layernorm.weight` | `blk.l.attn_norm.weight` | `+ 1` |
| `P layers.l.post_attention_layernorm.weight` | `blk.l.post_attention_norm.weight` | `+ 1` |
| `self_attn.q_proj.weight` | `attn_q.weight` | none (rows are `heads * head_dim * 2`: query and output gate together, as the engine expects) |
| `self_attn.{k,v,o}_proj.weight` | `attn_k`, `attn_v`, `attn_output` | none |
| `self_attn.{q,k}_norm.weight` | `attn_q_norm`, `attn_k_norm` | `+ 1` |
| `linear_attn.in_proj_qkv.weight` | `attn_qkv.weight` | v rows re-ordered (below); q and k rows kept |
| `linear_attn.in_proj_z.weight` | `attn_gate.weight` | rows re-ordered |
| `linear_attn.in_proj_b.weight` / `in_proj_a.weight` | `ssm_beta.weight` / `ssm_alpha.weight` | rows re-ordered (one row per v head) |
| `linear_attn.conv1d.weight` `[C,1,K]` | `ssm_conv1d.weight` `(K,C)` | the size-1 axis dropped; the v channels (after `2 * key_dim`) re-ordered |
| `linear_attn.A_log` | `ssm_a` | `-exp(A_log)` then re-ordered |
| `linear_attn.dt_bias` | `ssm_dt.bias` | re-ordered |
| `linear_attn.norm.weight` | `ssm_norm.weight` | none (the gated norm uses the plain weight, no `+ 1`) |
| `linear_attn.out_proj.weight` | `ssm_out.weight` | **columns** re-ordered |
| `mlp.gate.weight` | `ffn_gate_inp.weight` | none (router; always F32) |
| `mlp.experts.gate_up_proj` `[E,2F,D]` | `ffn_gate_exps.weight`, `ffn_up_exps.weight` `(D,F,E)` | first `F` rows of each expert = gate, last `F` = up |
| `mlp.experts.down_proj` `[E,D,F]` | `ffn_down_exps.weight` `(F,D,E)` | none |
| `mlp.experts.N.{gate,up,down}_proj.weight` (old layout) | same three tensors | stacked in the order of `N` |
| `mlp.shared_expert.{gate,up,down}_proj.weight` | `ffn_{gate,up,down}_shexp.weight` | none |
| `mlp.shared_expert_gate.weight` `[1,D]` | `ffn_gate_inp_shexp.weight` `(D)` | the size-1 axis dropped |
| `mlp.{gate,up,down}_proj.weight` (dense `qwen35`) | `ffn_{gate,up,down}.weight` | none |

**Value-head order of the DeltaNet.** The HF checkpoint stores the `nv` value heads grouped by key head
(`[k0:v0..v(r-1), k1:v0.., ...]`, `r = nv / nk`); the engine, like the GGUF files made by the reference converter, reads
them tiled (`[k0:v0, k1:v0, ..., k0:v1, ...]`). Output head `h = a * nk + b` comes from input head `b * r + a`
(`a < r`, `b < nk`). The permutation applies to every tensor indexed by value head, each with its own head width:
128 rows in `attn_qkv` (v part only) and `attn_gate`, 1 row in `ssm_alpha`/`ssm_beta`, 128 conv channels, 1 element in
`ssm_a`/`ssm_dt.bias`, 128 columns in `ssm_out`. With `nk == nv` it is the identity.

Metadata written (names as in the downloaded GGUF): `general.architecture/type/name/file_type/quantization_version/
quantized_by/sampling.*`; `<arch>.block_count, context_length, embedding_length, attention.head_count, head_count_kv,
key_length, value_length, layer_norm_rms_epsilon, rope.freq_base, rope.dimension_count (head_dim * partial_rotary_factor),
rope.dimension_sections (mrope_section, default 11 11 10 0), expert_count, expert_used_count, expert_feed_forward_length,
expert_shared_feed_forward_length, ssm.conv_kernel/state_size/group_count/time_step_rank/inner_size,
full_attention_interval`; `tokenizer.ggml.model = gpt2`, `tokenizer.ggml.pre = qwen35`, tokens, token types (1 normal, 3
special added tokens, 4 non-special added tokens such as `<think>`, 5 for the padding ids between the tokenizer size and
the embedding rows, written `[PADn]`), merges, `eos/bos/padding` ids, `add_bos_token`, `chat_template`. The `qwen35`
pre-tokenizer name is fixed for this family: the reference converter finds it by hashing the output of the tokenizer, which
`katana convert` does not do.

## Types

Small and sensitive tensors are always F32 in every quantizing mode (and also in `f16`/`bf16` for the same list): 1D
tensors (norms, `ssm_a`, `ssm_dt.bias`), the router `ffn_gate_inp`, `ffn_gate_inp_shexp`, `ssm_alpha`, `ssm_beta`,
`ssm_conv1d`. This is what the downloaded GGUF does too. `f32` writes everything as F32.

| mode | big matrices |
|---|---|
| `f16` | F16 (round to nearest; a BF16 value outside the F16 range overflows or flushes, so this is not lossless from BF16) |
| `bf16` | BF16 (exact from a BF16 checkpoint; correctly rounded from F16/F32) |
| `q8_0` | Q8_0 |
| `q6_k` | Q6_K |
| `q5_k_m` | Q5_K, embeddings and output Q6_K |
| `q4_k` | Q4_K, embeddings and output Q6_K |
| `q4_k_m` (default, "dynamic") | see below |

**Dynamic `q4_k_m`:** the cheap, sensitive tensors get 8 bits; the experts, which are almost all of the 35B parameters,
get 4 and 6.

| tensor | type |
|---|---|
| `token_embd.weight` | Q8_0 |
| `output.weight` | Q6_K |
| attention q/k/v/output, DeltaNet `attn_qkv`/`attn_gate`/`ssm_out`, shared expert, dense FFN | Q8_0 |
| `ffn_gate_exps`, `ffn_up_exps` | Q4_K |
| `ffn_down_exps` | Q6_K in the first eighth and last eighth of the layers and in every third layer of the middle (`(l - n/8) % 3 == 2`), else Q4_K |

A tensor whose row length is not a multiple of the block (256 for the K types, 32 for Q8_0) falls back to Q8_0, or F16
when not even 32 divides it. The estimate for the 35B model is about 21-22 GB (about 5 bits per weight), similar to the
downloaded Q4_K_M, but the mix is simpler: there is **no importance matrix** and the encoders are plain round-to-nearest
(the same ones as `katana quantize`).

## Differences from the reference converter

* `+ 1` on the norms is done in float32 (the reference adds in the tensor's own precision, bf16, then widens): the
  results can differ in the last bits of a bf16 value.
* `A_log` and `-exp` use `expf` in float32.
* No importance matrix, no per-tensor tuned mix like the downloaded file's.
* The MTP head and the vision tower are dropped, not converted.

## Verification

Tiny checkpoints only (a few hundred KB; CPU, since the GPU was busy). The tools are in `tools/convert/`:

* `make_tiny_hf.py` writes a Hugging Face checkpoint of the same architecture (2 layers, 8 experts, DeltaNet with
  `nk=2`, `nv=4` so the head reordering is not the identity) **from the weights of `tools/tinymodel/make_tiny_gguf.py`**
  (one generator, same seed), applying the inverse transformations in Python. Options: shard count with an index,
  fused or per-expert experts, BF16/F16/F32 storage, merges as pairs, template in `chat_template.jinja`, tied weights,
  decoy `visual.*`/`mtp.*` tensors.
* `compare_gguf.py A B` compares two GGUFs tensor by tensor (names, dims, types, bytes) and key by key.

`katana convert ... f32` of those checkpoints gave GGUFs with **all tensors byte-identical** to the generator's own GGUF
(38 tensors for the MoE model, 28 for the dense one), for: one file, 4-5 shards, per-expert layout, BF16 and F16
storage (the generator rounds its weights the same way; `ssm_a` is compared with a tolerance there because `A_log` was
stored rounded), merges as pairs, template file. The keys that differ are the expected ones (the generator does not write
`value_length`, `rope.dimension_sections` or `general.sampling.*`, and writes `feed_forward_length`, which an MoE model
does not need). The quantizing modes were checked against `katana quantize` of the F32 conversion: every tensor that
both put in the same type (Q8_0, Q6_K, Q5_K, Q4_K) is byte-identical. The converted tiny GGUFs (F32, F16, BF16, Q8_0,
Q4_K_M) load in the engine (`katana info`) and generate (`katana run`, CPU) the same greedy text as the generator's file.

`sh tools/convert/convert_test.sh` repeats these checks. The converter also ran clean under AddressSanitizer + UBSan (a separate build, leak check off) on the tiny checkpoints in several types and on the 1.7 GB sparse one.

The reader was also run (`--list`) on real safetensors files that happen to be on the machine (Stable Diffusion weights
in F16/F32), which exercises the header parser on real files; they are not a language model, so nothing was converted.

## Not verified

* **No conversion of `Qwen/Qwen3.6-35B-A3B` was run** (no checkpoint on the machine, GPU busy). Real `config.json`,
  tensor names and `tokenizer.json` are taken from the reference converter's mapping and from the downloaded GGUF, not
  from the real files. The names and the fused `gate_up_proj` layout are the places most likely to need a fix.
* Equivalence with the downloaded Unsloth `UD-Q4_K_M` was not measured (no logits or perplexity comparison; the mix is
  also different). The tests above prove the permutations are the inverse of mine, i.e. a consistent reading of the
  reference converter, not that the real model produces the same logits.
* BF16 and F16 GGUFs were only loaded on the CPU; whether every GPU path of the engine accepts them for `qwen35moe` was
  not checked. Q4_K/Q6_K output was only run on tiny shapes.
* Peak memory and time were measured only on synthetic checkpoints (below), never on the real model. A sparse (all-zero)
  BF16 checkpoint of 1.70 GB in 8 shards (`tools/convert/make_big_hf.py`: 8 layers, 64 experts, 0.85 G parameters) converted
  to `q4_k_m` with a peak resident set of **92 MB** (`/usr/bin/time -v`) in 1.1 s of wall time (6.6 s of CPU across the
  pool threads; zeros, so not representative of the encoders on real data). 39 tensors spread over 39 files: 11 MB.
  The slice size is a constant, so the expectation for the real model is a few hundred MB, but it was not run.
* The Windows build of `convert.c` was not compiled (it uses the same portable calls as the rest of the program; the
  `madvise` call is excluded there).
* Only BPE tokenizers in `tokenizer.json` are handled; `tokenizer.ggml.pre` is fixed to `qwen35`.
