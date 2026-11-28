# New downloaded models (DeepSeek / Kimi architecture)

Data source: GGUF header read by a Python script with no libraries (format v3).
Sizes checked against the Hugging Face API (they match byte for byte). The files live in
`models/` (outside git).

## Files

| File | Size (bytes) | Repository |
|---|---|---|
| `models/deepseek-v2-lite-chat-q4k.gguf` | 10,364,416,576 (9.65 GiB) | legraphista/DeepSeek-V2-Lite-Chat-IMat-GGUF (`DeepSeek-V2-Lite-Chat.Q4_K.gguf`, with imatrix) |
| `models/moonlight-16b-a3b-instruct-q4km.gguf` | 10,540,747,360 (9.82 GiB) | gabriellarson/Moonlight-16B-A3B-Instruct-GGUF (`...-Q4_K_M.gguf`) |

Notes:
- There is no DeepSeek-V2-Lite-Chat Q4_K_M in the community repositories. The legraphista `Q4_K` is the
  same scheme (Q4_K_M mix, `file_type = 15`) as the real Chat model, in 1 file.
- DeepSeek-Coder-V2-Lite-Instruct (bartowski, same architecture) is an alternative, not downloaded.

## Why Moonlight as the second model

- It is the only small model with the DeepSeek-V3/Kimi architecture (Moonshot trained Moonlight
  on the DeepSeek-V3 architecture): sigmoid router (`expert_gating_func = 2`) with bias
  (`exp_probs_b.bias`), normalized and scaled weights (2.446), 2 shared experts,
  1 dense layer, MLA. It covers exactly what V2-Lite lacks (softmax without bias).
- It fits with room to spare (9.8 GiB), runs on the machine, allows quick validation of logits/quality, and is the
  shortest path to Kimi K2 / DeepSeek-V3 (same code, just scale).
- Caveat: it is NOT a "large" MoE. If scale needs to be tested (reading experts from
  disk), the next candidate is GLM-4.5-Air (`glm4moe` architecture, no MLA): unsloth/GLM-4.5-Air-GGUF,
  folder `Q4_K_M/`, 2 parts, 50,000,746,752 + 22,975,001,632 = ~73 GB. Not downloaded (choose ONE).
  The full Kimi K2 and DeepSeek-V3 were not downloaded, as requested.

## DeepSeek-V2-Lite-Chat (`general.architecture = deepseek2`)

- blocks: 27; embedding 2048; vocab 102400; context 163840 (yarn factor 40, original 4096,
  `yarn_log_multiplier` 0.0707); rope base 10000; rms eps 1e-6.
- attention: 16 heads, `head_count_kv` 16; `key_length` 192 (= 128 nope + 64 rope),
  `value_length` 128; `kv_lora_rank` 512; **no `q_lora_rank`** (Q is a direct matrix
  `attn_q` [2048, 3072]); `rope.dimension_count` 64.
- MoE: 64 experts, 6 used, 2 shared, `expert_feed_forward_length` 1408;
  `leading_dense_block_count` 1 (layer 0 dense, `feed_forward_length` 10944);
  `expert_weights_scale` 1.0; no `expert_weights_norm` or `gating_func` (softmax, no bias).
- tokenizer: gpt2 / pre `deepseek-llm`, bos 100000, eos 100001.
- global tensors: `token_embd.weight` [2048,102400] Q4_K; `output.weight` Q6_K;
  `output_norm.weight` F32.
- layer 0 (dense): `attn_norm` F32; `attn_q` [2048,3072] Q4_K; `attn_kv_a_mqa` [2048,576] Q4_K
  (576 = 512 latent + 64 rope); `attn_kv_a_norm` [512] F32; `attn_kv_b` [512,4096] Q4_K
  (4096 = 16 x (128 k_nope + 128 v)); `attn_output` [2048,2048] Q4_K; `ffn_norm` F32;
  `ffn_gate`/`ffn_up` [2048,10944] Q4_K; `ffn_down` [10944,2048] Q8_0.
- layer 1 (MoE): same attention; `ffn_gate_inp` [2048,64] F32 (router);
  `ffn_gate_exps`/`ffn_up_exps` [2048,1408,64] Q4_K; `ffn_down_exps` [1408,2048,64] Q8_0;
  `ffn_gate_shexp`/`ffn_up_shexp` [2048,2816] Q4_K; `ffn_down_shexp` [2816,2048] Q6_K
  (2816 = 2 x 1408, the 2 shared experts fused).

## Moonlight-16B-A3B-Instruct (`general.architecture = deepseek2`)

- blocks: 27; embedding 2048; vocab 163840; context 8192; rope base 50000; rms eps 1e-5;
  `feed_forward_length` 11264; MIT license.
- attention: 16 heads, `head_count_kv` 1 ("absorbed" form of MLA); `key_length` 576,
  `value_length` 512 (= latent + rope / latent); `key_length_mla` 192, `value_length_mla` 128;
  `kv_lora_rank` 512; no `q_lora_rank`; rope dims 64; no yarn in the metadata.
- MoE: 64 experts, 6 used, 2 shared, expert ffn 1408, 1 dense layer;
  `expert_gating_func = 2` (sigmoid), `expert_weights_norm = true`,
  `expert_weights_scale = 2.446`.
- Tensor difference relative to V2-Lite: instead of a single `attn_kv_b`, there are separate
  `attn_k_b` [128,512,16] Q5_0 and `attn_v_b` [512,128,16] Q4_K (per head), and the MoE layer
  has `exp_probs_b.bias` [64] F32 (router bias).
- global tensors: `token_embd` [2048,163840] Q4_K; `output` Q6_K; `output_norm` F32.
- layer 0 (dense): `attn_norm`, `attn_q` [2048,3072] Q4_K, `attn_kv_a_mqa` [2048,576] Q4_K,
  `attn_kv_a_norm` [512], `attn_k_b`, `attn_v_b`, `attn_output` [2048,2048] Q4_K, `ffn_norm`,
  `ffn_gate`/`ffn_up` [2048,11264] Q4_K, `ffn_down` [11264,2048] Q6_K.
- layer 1 (MoE): same attention; `ffn_gate_inp` [2048,64] F32; `exp_probs_b.bias` [64] F32;
  `ffn_gate_exps`/`ffn_up_exps` [2048,1408,64] Q4_K; `ffn_down_exps` [1408,2048,64] Q8_0;
  `ffn_*_shexp` (2816): gate/up Q4_K, down Q6_K.

## Points for the engine

- New keys to read: `kv_lora_rank`, `key_length_mla`/`value_length_mla`, `expert_shared_count`,
  `leading_dense_block_count`, `expert_gating_func`, `expert_weights_norm`, `expert_weights_scale`,
  `rope.scaling.*` (yarn, V2-Lite only).
- The two models have different tensor names for MLA (`attn_kv_b` vs `attn_k_b`+`attn_v_b`).
- Quantization types present: Q4_K, Q6_K, Q8_0, Q5_0, F32.

## State in katana (`deepseek2` support)

What works (CPU, reference path):
- Reading the `deepseek2` metadata (kv_lora_rank, key/value_length and *_mla, rope.dimension_count,
  leading_dense_block_count, expert_shared_count, expert_gating_func, expert_weights_norm/scale,
  rope.scaling yarn) and the tensors of both MLA forms (single `attn_kv_b` or `attn_k_b` +
  `attn_v_b`). Q5_0 type decoded (only used in Moonlight's `attn_k_b`).
- MLA attention in the absorbed form for both models: the cache stores per position only the normalized
  latent (512) and the rope part of k (64) = 576 floats per layer (versus 16 x 320 = 5120 for the
  decompressed form). q_nope is multiplied by k_b (becoming a vector in the latent space), the score is
  q_lat . c_kv + q_rope . k_rope, the average stays in the latent space and only at the end goes through v_b. k_b/v_b stay
  in float in RAM (~8 MB per layer). RoPE on adjacent pairs ("normal" convention).
- V2-Lite yarn: frequencies with a ramp between interpolation (factor 40) and extrapolation (beta 32/1,
  original context 4096) and q.k scale multiplied by mscale^2 (mscale = 1 + 0.0707 ln 40).
- MoE: layer 0 dense; softmax router (V2) or sigmoid with `exp_probs_b` bias used only for selection,
  weights normalized and scaled by 2.446 (Moonlight); shared experts (`*_shexp`) always summed.
- The BPE tokenizer loads both vocabularies (102,400 and 163,840). Pre-tokenizer: grouped digits
  (`deepseek-llm`: no limit; `kimi-k2`: up to 3); the rest uses katana's GPT-2 approximation (the real
  regexes differ in details: CJK, line breaks). New chat formats:
  5 = DeepSeek (`User: ...\n\nAssistant:`), 6 = Kimi (`<|im_user|>user<|im_middle|>...`).
- GPU (OpenCL and Vulkan): for now "layers on the CPU, dense products on the GPU": q, kv_a, attention
  output, dense feed-forward, shared experts and vocabulary go up; MLA attention and routed experts
  stay on the CPU. `selftest` checks these products and end to end against the CPU.

Tests (KATANA_CPU=1, KATANA_CTX=512, Linux, greedy):
- V2-Lite `run "The capital of France is" 20`: " Paris.\n\nThe official language is French.\n\nThe
  currency is the Euro (€)." (~2 tok/s with cold pages; 10 tok/s over 116 tokens with the file in cache).
- V2-Lite `chat "Quanto e 17 vezes 23?"`: " 17 x 23 = 391".
- Moonlight `run`: " Paris, which is located in the northern part of the country. It is one of the most visited" (4.9 tok/s).
- Moonlight `chat`: "O resultado de 17 vezes 23 é 391." (7.3 tok/s).

What is missing:
- Full MLA graph on the GPU (latent attention kernel, cache of 576 per position,
  k_b/v_b absorption on the GPU) and experts in the VRAM cache (reuse `gx`/`vk_moe`, which
  today assume all layers are MoE and have no shared experts).
- Batch path on the CPU for the prompt (today token by token, like the other MoEs).
- Full Kimi K2 / DeepSeek-V3: `q_lora_rank` (attn_q_a + attn_q_a_norm + attn_q_b), grouped routing
  (n_group/topk_group; Moonlight uses 1 group), 256/384 experts, 61 layers with 3
  dense, and reading experts from disk (they do not fit in this machine's RAM).

GPU (RTX 3050 4 GB, OpenCL, KATANA_CTX=512): `selftest` OK on both (Moonlight: largest logit difference
0.00026; V2-Lite: 0.035, below the 0.05 limit, with equal argmax); Vulkan (`KATANA_BACKEND=vulkan`)
also OK on Moonlight. Chat "17 vezes 23" equal to the CPU's, ~7 tok/s on both, that is, no gain
over the CPU (6.7 tok/s; isolated measurement, not alternated): the round trip per product eats the gain.
`q_lora_rank` (attn_q_a/attn_q_a_norm/attn_q_b) is already read and used on the CPU, but there is no model to test it.
Regression: `selftest` OK on qwen2.5-1.5b and olmoe (KATANA_KV16=0).

## Qwen3.6-35B-A3B (`general.architecture = qwen35moe`)

File: `models/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf` (22,134,528,992 bytes, Unsloth, imatrix).
40 layers, dim 2048, vocab 248320, 16 heads (kv 2) of 256, MoE 256 experts / 8 active
(ffn 512) + 1 shared (ffn 512) with a scalar sigmoid gate (`ffn_gate_inp_shexp`).
Layers with (l+1) % 4 == 0 (3, 7, ..., 39) are full attention with a gate; the other 30 are Gated
DeltaNet. Types: Q8_0 (attention/DeltaNet projections, shared expert, embedding), Q4_K (gate/up of the
experts), Q5_K/Q6_K (down), Q6_K (output), F32 (norms, router, ssm_alpha/beta, conv1d).
Tokenizer gpt2, pre `qwen35` (loaded with the existing BPE; pre-tokenization the same as qwen2, with no
rule of its own registered - ordinary text tokenizes and generates correctly).

### Conventions confirmed by running the real model (CPU reference)

1. q/k group -> v head map: **h % 16** (interleaved). With h / 2 the output becomes garbage
   ("The capital of France is诚聘英才IES..."); with h % 16 it gives "Paris, a city renowned for...".
   (`KATANA_DN_TILE=0` turns on the other map, for testing only.)
2. RMS norm weights (attn_norm, post_attention_norm, ssm_norm, attn_q_norm, attn_k_norm,
   output_norm): **w directly** (the file already carries the 1+w where the original model uses it). ssm_norm is
   an RMS norm per v head (128) followed by multiplication by SiLU(z).
3. `ssm_a` already comes ready as **-exp(A_log)** (negative): g = ssm_a * softplus(alpha + dt_bias),
   decay = exp(g).
4. `attn_qkv` [2048 -> 8192]: contiguous blocks **q (16x128) | k (16x128) | v (32x128)**; the
   convolution [4][8192] (dims [4, 8192], 4 contiguous taps per channel, tap 3 = current token) runs over
   the 8192 channels before the split; then SiLU, L2 (eps 1e-6) per group on q and k, q * 1/sqrt(128).
   `attn_q` of the full attention [2048 -> 8192]: **interleaved per head**, [q 256 | gate 256] x 16;
   the output of each head * sigmoid(gate) before attn_output.
5. Partial neox RoPE: the **first 64 dims of each head rotate, pairs (i, i+32)**, frequency
   1e7^(-2i/64); dims 64..255 do not rotate. mRoPE [11,11,10] with pure text = ordinary RoPE.
Router: softmax over the 256, top-8, renormalized weights. Format: ChatML, the assistant starts
with `<think>\n`; tools in XML (`<function=..><parameter=..>`).

### State in katana

- Exact CPU: per-layer steps in functions with buffers (`dn_proj`, `dn_conv`, `dn_recur`,
  `dn_gate`, `qn_core`, `ffn_moe`, `qn_layer`). DeltaNet state contiguous in `m->ssm`: per linear
  layer, S [32][128][128] and (4-1) x 8192 of the convolution (`dn_S`, `dn_CV`); zeroed at pos 0. The
  server restarts from scratch if the reused prefix is not the whole cache.
- GPU (OpenCL, `gpu.mv_host` mode): layers on the CPU, 251 dense products on the GPU (qkv, z, ssm_out,
  q/k/v/o, shared expert, vocabulary; 1.9 GB) and the experts through the same slot cache as
  qwen3moe (`gx_setup`, extracted from `gpu_setup`; heat, loader thread, `heat_*`), via
  `gpu_moe_host` (uploads xb, `gpu_moe_ffn`, reads the sum). On the RTX 3050 4 GB: 19 of 256 slots per layer.
- Batch prompt on the CPU (`qn_layers_n`, up to 16 tokens): projections, outputs, shared expert and
  (token, expert) pairs in batch; conv/recurrence/attention token by token (`dn_recur` is the loop to
  replace with the chunked form). Default: batch only without GPU (`KATANA_QN_BATCH=1/0` forces).
- Measurements (indicative, shared machine, alternated at the same hour, 46 prompt tokens,
  60 generated): GPU+expert cache: token-by-token prompt 15.2 tok/s, generation 14.6-18.8 tok/s;
  batch prompt on the CPU 15.4-17.6 tok/s. CPU only (`KATANA_CPU=1`): ~8-10 tok/s generation, prompt
  10-14 tok/s. Experts: ~46% of uses in VRAM.
- Missing (OpenCL): full graph on the GPU; `katana pack` not tested for this model; the CPU
  KV cache allocates all 40 layers (only 10 use it).

### Full graph in Vulkan (2026-11-16)

With `KATANA_BACKEND=vulkan` qwen35moe runs entirely on the GPU (`vkq_setup`, `vk_qn_forward_1`,
`vk_qn_forward_n`); OpenCL stays in hybrid mode. `KATANA_VK_QN=0` goes back to hybrid on Vulkan.
- Dense weights go up through the same `mla_gpu_upload` (attn_q keeps q and gate interleaved: the
  kernels read q with stride 2*hd per head). alpha/beta become a float matrix [a | b] per layer.
- New kernels in `vk/shaders/` (coming from `tools/vkdelta/`): `dn_step` (position from meta; position
  0 treats S and ring as zero, no need to zero them), `dn_prep`, `dn_cstate`, `dn_chunk`, `dn_state`
  (with `pos0`; S as an operand in two fp16 parts, high + remainder: the state error in the batch dropped ~10x
  in the middle layers), `dn_norm`; `qn_gate` (attention gate in the batch), `moe_gath`/`moe_scat`
  (MoE in batch). `att1f` and `qkv_post` gained `nrot` (partial neox RoPE: pairs (i, i+nrot/2), dims
  >= nrot do not rotate) and `qst` (q stride per head); `att1f` also `gate` (output * sigmoid of the
  gate, applied per chunk, since it is linear). `mm.comp` gained `woff` (weight offset:
  expert in a slot). Group -> head map `kmap` = `KATANA_DN_TILE` (default h % 16).
- One token: attention/DeltaNet on the GPU; MoE as in `vk_forward_moe` (router on the CPU, hot experts in
  slots, cold ones on the CPU), now with the shared expert on the GPU (row K of `vkm.bt`, weight = sigmoid
  gate). One 8 KB read per layer.
- Batch (up to 256 per pass): products in `mm.comp`, DeltaNet in blocks of 64, attention
  `qkv_post` + `att_flash` (hd 256, group 8) + `qn_gate`. MoE: the residual goes down (norm and routing
  exact on the CPU); the shared expert and each hot slot run on the GPU over the columns of their tokens;
  cold ones on the CPU (blocks of 16 tokens) and go up summed. `KATANA_QN_PROF=1` shows the profile;
  `KATANA_QN_DNSTEP=1` makes the recurrence token by token (fp32) inside the batch (check).
- VRAM (4 GB, ctx 4096): dense 1.9 GB + state/cache/batch ~0.2 GB; the rest becomes 18-20 slots per
  layer.
- Fix: `qn_batch` must not send the prompt to the CPU with the Vulkan graph on (the state lives
  in VRAM; the token-by-token `vkcheck` fell into that path).
- Validation (before the fix below): `selftest` batch 0.06-0.23 (special tolerance of 3% of the largest logit);
  `vkcheck` 600 tokens 0.42-0.46 with divergence at the 8th generated token. Fixed afterwards (see "Batch precision").
  `chat` "17 vezes 23" -> 391.
- Indicative measurement (bench 256+64, alternated, run without external load): Vulkan prompt 82.7 tok/s,
  generation 25.2-25.5; OpenCL hybrid prompt 27.3-27.5, generation 18.0-18.7. In the batch, 2.7 of 3.1 s
  per pass are the cold experts on the CPU (~65% of the pairs with a random prompt).
- Next: cold experts of the batch on the GPU (upload the experts used by several tokens of the batch to a
  temporary area), group pairs by expert on the CPU, router on the GPU to record the graph of
  one token.
- Agent validated: question about the Linux kernel -> `web_search` and `web_fetch` called in XML,
  converted to the JSON arguments, answer "7.3.4" citing kernel.org. Agent speed
  8-11 tok/s, dropping to ~2 tok/s in one run (11535 expert swaps: the heat swaps too much
  with 19 slots; investigate `KATANA_SWAP`/margin for this model).


### Batch precision of qwen35moe on Vulkan (2026-11-16)

Cause (measured): the top-8 routing out of 256 has near ties. Tool `KATANA_QN_DBG=1` in `selftest` (batch
against the exact CPU, per layer: relative error of the residual after the mixer, maximum over the 23 tokens, and how many tokens
chose a different top-8 set; for each swap it prints the margin between the 8th and 9th). Before: 3.4e-4 at layer 0,
~8e-4 at layer 5, where token 15 swaps one expert (relative margin 2.8e-4; after the 1st fix the same
margin measured 1e-5) and the error of that token jumps to 4.7e-2 (a different expert with weight ~1/8); other
swaps follow in the next layers. There is no value outside the fp16 range (CPU emulation: scaling the activation by
64 or 1024 changes nothing; the relative rounding of every product input stays at 3-4e-4). The error of 1e-4
to 3e-4 per layer came from several sources, and any one of them alone is enough to flip some tie:
- `mm.comp` products in fp16: activation rounded and decoded weight rounded (Q8_0 d*q has up to 19 bits);
  qkv projection of layer 0: 9.5e-5 relative against the fp64 computation;
- DeltaNet in blocks (q/k/v, T, Wn, Qm in fp16): ~2e-4 at layer 0 even on the 1st token;
- attention output (att_flash) and gate in fp16 before the output product; experts in fp16 (input,
  weight, glu intermediate).
CPU emulation (rounding only the input of one class of products to fp16) confirms: ssm_out alone already takes the final
logit to 0.09; only the input projection weights to 0.13; only wo to 0.02. Since each source alone flips ties, the
difference does not fall by fixing just one (fixing ssm_out alone: 0.110 -> 0.109).

Fix (all in the qwen35moe batch; the dense models and the other MoEs do not change):
- `mm.comp` with WS (decoded weight in high + fp16 remainder) and XS (activation fp32 in high + remainder): three products in the
  same tile, remainders scaled by 2048 (without the scale the weight remainder, ~5e-6, falls into subnormal) in a second
  accumulator. The high part is cut by a bit mask (`hi16`): the round trip `float16_t(float(x))` (and also
  `unpackHalf2x16(packHalf2x16())`) the driver compiler folds to x and the remainder came out zero (the 1st version changed
  nothing because of this). qkv of layer 0: 9.5e-5 -> 4.4e-6. Variants `_ws`, `_xs` (flat) and `_gx` (glu, fp32 output).
- Batch norms in fp32 (`rms_wg32`, a variant of `rms_wg16`), `moe_gath32`, glu intermediate in fp32;
  `att_flash32` and `qn_gate32` (attention output in fp32 in `vkq.ub`); `dn_norm` now writes fp32.
- Batch DeltaNet through the exact recurrence: `dn_seq.comp` (one launch per layer, a group of 512 per head walks
  the B tokens with the state in registers; previous conv inputs come from the batch itself or from the ring; the ring is
  updated afterwards by `dn_cstate`). Layer 0: 2e-4 -> 1e-5. The chunked path stays under `KATANA_QN_CHUNK=1`
  (with the precise products: 0.0028 in selftest).
- The 3% exception in selftest removed: the qwen35moe batch uses the common limit of 0.05.

Result: error per layer 1e-5 (layer 0) to ~2e-4 (layers 31+), no token with a swapped top-8 in selftest.
`selftest` batch x exact CPU: 0.11 -> 0.0004 (batch 16: 0.08 -> 0.0004; token by token 0.0003). `vkcheck` 600 tokens
(docs/knowledge.md): 0.298 -> 0.094 (largest |logit| 23.6), 40 next greedy tokens equal in both (on that text the old
binary also got all 40 right). What remains in `vkcheck` comes from the cold experts on the CPU, which use the integer
product (activation quantized to 8 bits) in both paths, but on different sets of experts:
with the cold ones in exact float (`KATANA_VKC_EXACT=1`, measurement only) the difference falls to 0.0042.
Speed (bench 256+64, alternated with the previous binary, GPU free, runs 2 and 3): prompt 86.1/82.8/82.8/80.5
against 84.2/87.6/87.7/81.3 tok/s (~-3%, within noise; the pass is dominated by the cold experts on the CPU); generation
equal (26.0-26.7 x 25.2-25.9). Regression: selftest qwen2.5-1.5b (OpenCL 0.00004, Vulkan 0.0078), llama3.2-3b
Vulkan 0.026, OLMoE (KV16=0) 0.0008, Moonlight (ctx 512) 0.0003, Qwen3.6 OpenCL 0.0004: all OK.
Pending: `dn_state` (chunked path) splits S into high + remainder with a coopmat conversion; check whether the driver
also folds that round trip. The dense models can use the same precise variants if the batch error is a nuisance
(llama3.2-3b 0.026), at the cost of 3 products per tile. `KATANA_VKC_EXACT=1` in `vkcheck` puts the CPU cold experts in
exact float (measurement).

## Qwen3.8-Flash-Next (`general.architecture = qwen4exp`)

File: `models/flash-next/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_XXS-00001-of-00002.gguf` (47,039,860,096 bytes, the core) and
`-00002-of-00002.gguf` (28,800,138,432 bytes, only the n-gram table). Open the first part; the others are found by name.
48 layers, dim 2560, 512 experts / 10 active (ffn 640) plus a shared expert, DeltaNet in 36 layers, attention in 12
(3, 7, ..., 47). CPU only. Code: `src/engine/qwen4exp.c`; hooks in `model.c` (metadata, parts, type 42) and `forward.c`.

### Conventions (each compared with the published original weights by range requests: difference 0)

- Norm weights in the file already carry 1 + w. `ssm_a` is -exp(A_log). The value heads (qkv, conv, gate, alpha, beta,
  dt, output) are interleaved across the q/k groups, as in qwen35moe. `indexer.q_proj`/`k_proj` are the two parts of
  one original projection. `ple_layers` in the GGUF is 0-based (layer 1), 1-based in the original config.
- The DeltaNet output gate is sigmoid, not SiLU. No `attn_norm`, `ffn_norm` or `output_norm`: the residual mixers normalise.
- Type 42 (Q2_0): 64 weights in 18 bytes, fp16 scale d then 16 bytes with four 2-bit codes (first weight in the low
  bits), weight = d * (code - 1). Checked on `blk.1.ffn_down_exps` against the original: correlation 0.898 with this
  rule; 0.581, 0.788, 0.009 and -0.004 with code - 2, code - 1.5, scale at the end and reversed bit order.
- N-gram table: 320,001,536 rows of 160, type IQ4_NL (90 bytes a row); row 0-39 match the original with 7-9% error.
- Equal block scores in the sparse attention: the older block wins (the original leaves ties undefined).

### State

- The n-gram table is mapped with random-access advice and never copied; the pages of a prompt (up to 4096 tokens) are
  requested together, sorted, before the first batch. BF16 matrices (mixers, router, ple) are multiplied from the
  mapped file. Prompt in batches of 16, each weight row opened once per batch.
- Test without a real model: `sh tools/tinymodel/qwen4exp_test.sh` (pure Python reference, `katana q4check`): states of
  8 layers and logits, token by token, batches and mixed pieces, 18 times over the sparse budget at 72 tokens; float32
  agreement 1e-5 (integer products: within 5e-3 but for isolated near-ties of the router); eight mutations of the reference are all detected; saturated activations.
- Bug found with the real file and reproduced in the tiny one: under `-ffast-math` the vectorised sigmoid divides by an
  approximate reciprocal with a Newton step, which is NaN when expf overflows (argument below -88). All sigmoids of
  the file are now clamped (`q4_sig`). The same pattern exists in other loops of the engine when they vectorise.
- Measured on the real file (2026-11-22, i7-11800H, CPU only, ctx 4096, both hashes verified, under the measure lock).
  Text: "The capital of France is Paris. The capital of Germany is Berlin...", 17 x 23 = 391 in a Portuguese chat, the
  right CPU out of a 472-token text, and a password in the first line of a 2,586-token prompt (over the 2,048 budget).
  Same 24 tokens, alternated, two rounds each, float path against the low-bit integer kernels: generation 0.7 and 0.7
  against 1.9 (cold pages, 216 thousand major faults) and 4.1 tok/s (warm, 0 major faults), same text.
  | run (integer kernels) | prompt tok/s | generation tok/s | peak RSS | of it, table | major faults |
  |---|---|---|---|---|---|
  | 5 + 24 tokens, warm | 5.8 | 4.1 | 9.5 GB | 1 MB | 0 |
  | 472 + 130 tokens | 4.3 | 3.0 | 31.9 GB | 16 MB | 317,337 |
  | 2,586 + 60 tokens | 6.5 | 3.2 | 37.7 GB | 134 MB | 71,285 |

  Anonymous memory is 0.17-0.30 GB (KATANA_BF16_RAW is not needed here, BF16 stays mapped); the rest is the mapped core,
  which grows with the experts touched (ceiling 47 GB). Batches and single tokens were compared on the tiny model only.

### Missing

- Conversation prefix cache (disabled for this architecture); training; GPU.
- MTP head: its tensors (one gated-attention layer with its own mixers and experts, `mtp.fc_embedding`,
  `mtp.fc_hidden`) are not in this GGUF; it would need them converted and the verify loop of speculative decoding with
  rollback of DeltaNet, n-gram and indexer state.
- Images: vision tower (27 layers, not in this GGUF), 3-axis RoPE positions (also in the indexer keys) and the image
  token id in the n-gram hashes (the original feeds the placeholder id 248056 to them).
- GPU: the 4-branch mixers and the sigmoid gate as shaders, block scores and top-k, the table lookup stays on the CPU.

## Gemma 4 26B-A4B in the agent (working model since 2026-11-21)

Chat template (read from the GGUF metadata, `tokenizer.chat_template`) and what the agent renders:

- **System turn**: `<|turn>system\n` + (`<|think|>\n` when thinking is on) + system text (trimmed) + every tool as
  `<|tool>declaration:NAME{description:<|"|>..<|"|>,parameters:{properties:{..},required:[..],type:<|"|>OBJECT<|"|>}}<tool|>`
  + `<turn|>\n`. The template puts the declarations right after the system text and nothing after them, so the
  declarations must stay at the END of the system turn. Consequence for the prefix cache: the per-folder block
  (environment, project instructions, memory) sits between the stable rules and the declarations, so a new folder
  recomputes the environment block AND the declarations. Measured with the real tokenizer (vocabulary only) on
  2026-11-21, default tool set, one MCP server, one skill: 2,227 tokens = 822 stable (rules + MCP cards) + 430 per
  folder (depends on the folder listing) + 977 declarations. Cheapest order the template allows: rules and MCP
  cards (stable, cached once for every folder) -> environment/project/memory -> declarations; that is the default.
  Putting the per-folder block after the declarations would recompute only ~430 tokens per folder, but puts text where the template
  never puts any; that order was not adopted.
- **Declarations**: `g4_agent_sys` (`src/agent/fmt_gemma4.c`) converts the JSON `<tools>` block. Checked byte for byte
  against the template rendered with jinja2 (`tools/traintest/gemma4_golden.py sys`), both tool sets, thinking on and
  off, with an MCP server and a skill. Fixed on 2026-11-21: `items` lost its `required` list (AskUserQuestion), and
  `items` keys now follow the template order (description, enum, properties, required, type); a property without
  `type` no longer dereferences a null pointer. MCP servers are cards in the system text (before the environment),
  the skills line is in the environment block: both survive the conversion.
- **Calls and results**: the model writes `<|tool_call>call:NAME{k:<|"|>v<|"|>,n:3}<tool_call|>` and the engine stops
  at `<|tool_response>` (type 4, not a control token: every side generation stops on it explicitly). Results go inside
  the model's own turn: `<|tool_response>response:NAME{value:<|"|>...<|"|>}<tool_response|>`, then the model continues
  (with thinking on the prompt reopens `<|channel>thought\n`, as the template's generation prompt does). Since
  2026-11-21 the result is always in this native block, also when the model used the generic `<tool_call>{json}`
- **Engine notes**: the notes attached to a result (repeat, failure streak, blocked prerequisite, plan first at the
  3rd/6th call, step reminder every 5th result, round limit) are inside the result value, read as tool output. The
  notes that need a user turn (project verification at the end of a request, todo reminder, live compaction request,
  sub-agent closing notes) close the model turn and open `<|turn>user`: the template accepts a model turn that ends in
  tool responses followed by a user turn.
- **Known difference kept on purpose**: the KV cache holds each model turn as generated. With thinking off every model
  turn starts with the empty channel `<|channel>thought\n<channel|>` (the template writes it only in the generation
  prompt); with thinking on, earlier thoughts stay (the template drops them from history). Re-rendering history the
  template way would cost a recompute every turn.
- **Call-format constraint**: `src/agent/callgrammar.c` reads the declared tools from the JSON block saved by the
  conversion plus the table tools and the MCP tools. It does not switch on inside the thought channel, also when the prompt opened the
  channel after tool results (`g_g4_thought_open`). A string value may no longer contain `<tool_call|>`,
  `<|tool_call>` or `<|tool_response>` (single tokens in Gemma 4: the random tiny model produced them inside a value
  once the tiny vocabulary had the real special tokens, and the call reader cut the call there).
- **Mechanisms ported on 2026-11-21** (each with a switch): live compaction inside a long request and for `/compact`
  (`KATANA_COMPACT_LIVE=0` off, both formats); reading notes and the sub-agent `delegate`/`Agent` (`KATANA_G4_SIDE=0`
  off; `KATANA_DIGEST=0` still turns off the notes in every format). Side contexts and the summary run with the empty
  thought channel (no reasoning). Tests: `tools/traintest/gemma4_part.sh` (tiny Gemma 4 model with the real special
  tokens and their types, `make_tiny_gguf.py --arch gemma4 --template gemma4`, scripted stand-in teacher).

Checks that remain for the owner's machine once the GPU is free (Gemma 4, real model):
1. First start time in a new folder and in a folder already seen.
2. Codebench pass on the `write-*`, `conv-*`, `prereq-*` and `extend-*` tasks.
3. Tool-call validity per 100 calls in `agente.log` with and without the constraint (`KATANA_CALL_GRAMMAR=0`), thinking
   off (desktop) and on.
4. Compaction time on a long job (live summary against `KATANA_COMPACT_LIVE=0`) and the quality of the summary.
5. One sub-agent call and one large `Read` note (time, note quality, main context reloaded).

## Gemma 4 E2B in the engine (text only, working since 2026-11-27)

Model: `models/gemma4e2b/model.gguf` (ggml-org/gemma-4-E2B-it-GGUF, Q4_0, 2.84 GB). Same GGUF architecture name
as the 26B-A4B (`general.architecture = gemma4`), but **dense** (no experts) and with three mechanisms the
26B-A4B does not use: Per-Layer Embeddings (PLE), K/V shared by the last layers, and a feed-forward width that
changes partway through the stack. Confirmed by reading the GGUF header directly (`gemma4.*` keys and the tensor
list, byte for byte) and by comparing, in llama.cpp's public source (`src/models/gemma4.cpp` and `gemma3n.cpp`,
read only to understand the formula, nothing copied), with the equivalent, already-working 26B-A4B code
(`src/engine/model.c`, `src/engine/qwen35.c`'s `g4_layer`/`g4_layers_n`). Gemma 3n (llama.cpp's `gemma3n.cpp`) has
AltUp and LAuReL on top of PLE; Gemma 4 dropped both, confirmed by the GGUF having no `altup_*`/`laurel_*` tensor
and no such key in the header. Vision and audio (`general.tags = any-to-any` in the GGUF) are out of scope here.

### Metadata and tensors confirmed in the GGUF (35 layers, dim 1536, 4.6B listed, 4628.6M counted by the engine)

| Key | Value | What it is |
|---|---|---|
| `gemma4.embedding_length_per_layer_input` | 256 | PLE width per layer (`ple_dim`) |
| `gemma4.attention.shared_kv_layers` | 20 | last 20 of 35 layers have no own K/V (`kv_shared_n`) |
| `gemma4.attention.sliding_window_pattern` | array[35], period 5 (4 local + 1 global) | per layer, `swa` |
| `gemma4.feed_forward_length` | array[35]: 6144 (layers 0-14), 12288 (15-34) | dense feed-forward width, per layer |
| `gemma4.attention.head_count_kv` | 1 (scalar, not array in this file) | MQA |
| `gemma4.attention.key_length` / `..._swa` | 512 / 256 | head width, global / local |
| `gemma4.rope.freq_base` / `..._swa` | 1000000 / 10000 | RoPE base, global / local |
| `gemma4.final_logit_softcapping` | 30.0 | same mechanism already in `g4_layer`/`cpu_head` |

Tensors (`tools` used to dump them: a small standalone Python GGUF reader, stdlib only, read-only, discarded
after use): global `per_layer_token_embd.weight` [262144, 8960] (= vocab x `ple_dim*n_layers`),
`per_layer_model_proj.weight` [8960, 1536], `per_layer_proj_norm.weight` [256]; per layer `blk.N.inp_gate.weight`
[256, 1536], `blk.N.proj.weight` [1536, 256], `blk.N.post_norm.weight` [1536] (note: this GGUF names the
per-layer PLE tensors `inp_gate`/`proj`/`post_norm`, not the longer `per_layer_*` names llama.cpp's generic
tensor table uses). Layers 15-34 have no `attn_k`/`attn_v`/`attn_k_norm` at all; `attn_q`/`attn_q_norm`/
`attn_output` stay on every layer.

### Formula (llama.cpp `gemma4.cpp`, confirmed working at `src/engine/qwen35.c:g4_ple_prep`/`g4_layer`/`g4_layers_n`)

Per token, once (`g4_ple_prep`, called right after `embed_row`, before any layer): with `x0` = the token
embedding already scaled by `sqrt(dim)` (same value written to `m->x`/`X`), `E = ple_dim`, `Ln = n_layers`:

```
raw[l]  = per_layer_token_embd[token][l]        (E values per layer, layer-major)  * sqrt(E)/sqrt(2)
proj    = per_layer_model_proj @ x0                                  (E*Ln values)
proj[l] = rmsnorm(proj[l], eps*dim) * (per_layer_proj_norm * 1/sqrt(2))    (no extra scale after: both
out[l]  = raw[l] + proj[l]                                                  branches already carry the 1/sqrt(2))
```

Per layer, after the usual attention and feed-forward residual (`x += ffn_out`), a third residual, before
`out_scale`:

```
t = gelu(per_layer_inp_gate[l] @ x)      (E values)
t = t * out[l]                           (the per-token buffer above, this layer's slice)
t = per_layer_proj[l] @ t                (back to dim)
t = rmsnorm(t) * per_layer_post_norm[l]
x = x + t
x = x * out_scale                        (unchanged: out_scale still applies to the whole layer, PLE included)
```

K/V sharing: layers from `n_layers - kv_shared_n` on have no own `attn_k`/`attn_v`; each reuses the K/V cache
of the last earlier layer of the same kind (local or global) — computed once, at load time, in `model.c`'s
cache-sizing loop (`last_local`/`last_global`), by pointing the shared layer's `kv_off`/`kv_cap` at the donor's.
No per-token lookup: the forward pass doesn't need to know which layer donated, it just skips computing/writing
K/V for that layer and reads a cache that was already written earlier **in the same pass**, since donors
always come before the layers that share from them. Feed-forward width per layer (`Layer.hidden_l`): read
from the `feed_forward_length` array when present (new array parsing in `model.c`, alongside the existing
`sliding_window_pattern`/`head_count_kv` one), else the old scalar (26B-A4B, unaffected).

### GPU (Vulkan) for E2B/E4B (working since 2026-11-28)

`src/engine/vk_gemma4.c`'s hybrid path (`vk_g4_build`/`vk_g4_n`: the big products — `wq`/`wk`/`wv`/`wo`,
`gate`/`up`/`down` — go to the GPU each layer, round trip per layer; norms, RoPE, cache writes and attention
stay on the CPU, same split as the already-working 26B-A4B) now covers PLE and K/V sharing too, for both the
prompt (batch, `vk_g4_n` with B tokens) and generation (one token at a time: `forward()` now falls through to
`vk_g4_n` with B=1 when `vkg4.ok`, same function as the batch path). The old blanket `c->g4 && (c->ple_dim ||
c->kv_shared_n)` bail-out in `gpu_setup`/`vk_g4_build` is gone.

- **K/V sharing**: a layer with `kv_shared` skips the `wk`/`wv` GPU dispatch and the cache write entirely (there
  is nothing to compute or write — `model.c` already points `kv_off`/`kv_cap` at the donor layer); `q` still
  goes through the GPU and gets RoPE on the CPU same as any layer. The attention kernel reads the donor's
  cache without knowing it is shared, exactly like the CPU path.
- **PLE stays on CPU**, by choice, inside `vk_g4_n` (same formula as `g4_layer`, reusing `g4_ple_prep` for the
  per-token table): `per_layer_inp_gate`/`per_layer_proj` are `dim x 256`/`256 x dim`, tiny next to the main
  `1536x6144..12288` feed-forward — one more GPU round trip per layer for ~0.4M MACs would not pay for itself.
  Everything bigger stays on the GPU.
- **A real bug found and fixed along the way**: `gemma4.feed_forward_length` is an array in this GGUF —
  6144 for layers 0-14, 12288 for 15-34 (`Layer.hidden_l`, already read correctly by the CPU path).
  `mla_gpu_upload` (`shards.c`), used for every `gemma4` model's GPU upload, was uploading `w_gate`/`w_up`/
  `w_down` using `c->hidden_dense` (the *largest* width across layers, 12288) for every layer instead of each
  layer's own `hidden_l`. This was never caught before because every `gemma4` model previously enabled for GPU
  (the 26B-A4B) has one uniform width, so `c->hidden_dense` and `hidden_l` always agreed; E2B's two different
  widths exposed it immediately (`katana selftest` on E2B: wrong logits, `produtos: FALHOU`, the per-matrix
  `ffn_down` GPU check off by 1.12e-01 for the narrower layers once GPU upload was turned on for E2B). Fixed by
  uploading each layer with its own `L->hidden_l`; `vk_g4_build`/`vk_g4_n` now read `L->hidden_l` per layer too
  (`c->hidden_dense`/the old single `Hd` is kept only to size the shared work buffers, generously, for the
  widest layer).
- **Decoding: measured, not assumed, which path is faster**. `vk_g4_n`'s GPU products use a cooperative-matrix
  batched GEMM, built for B >= 2 (the prompt). Routing one-token generation through it (B=1) works and is
  correct, but on the RTX 3050 with this 2.8B model it measured **~17 tok/s**, slower than just letting the
  existing CPU path (`cpu_layers`/`g4_layer`) run, which already had its own lighter-weight opportunistic GPU
  path (`vk_matvecs` in `shards.c`: one upload, several matvec dispatches, one read-back) — once GPU upload is
  enabled at all for E2B, `g4_layer` picks it up for free and measured **~36 tok/s**. Not a formal benchmark,
  just what `katana run`'s own counter printed, run twice to check it wasn't noise. Given that, `vk_g4_n` on
  B=1 defaults **off**; `KATANA_VK_G4DEC=1` turns it on for whoever wants to compare on different hardware.
  The batch path (prompt) always uses `vk_g4_n`, unaffected by this switch.

### Numeric verification (`katana selftest`, no reference engine, as the project requires)

- **Exact product vs fast CPU kernels**: every weight matrix, including the new ones (`ple_tok_embd`,
  `ple_model_proj`, and, per sampled layer, `ple_inp_gate`/`ple_proj`) — worst relative error 2.93e-08 (exact)
  and 1.45e-06 (CPU integer path), same order as every other working architecture. `qtest.c`'s generic
  "layers in batch vs token by token" check was extended to use `Layer.hidden_l` (was the single
  `hidden_dense`, wrong for a layer with the smaller width) and now also runs the PLE tensors.
- **Batch (`g4_layers_n`, prompt) vs token by token (`g4_layer`, generation)**, through the real
  `forward_seq`/`forward` dispatch (not a direct call, so the dispatch itself is covered too): 23 tokens,
  **0.000e+00 difference**, same argmax. Added as a new `qtest.c` check (`c->g4` block, mirrors the existing
  `c->gr`/granite one); the old generic batch-vs-token check (`cpu_layers_n` vs `cpu_layers`) does not know
  about `gemma4`'s per-layer head width/FFN width/shared K/V and was excluded for `c->g4` (`&& !c->g4`), since
  it would read past a layer's real tensor size or dereference the empty K/V of a shared layer.
- **Regression**: the 26B-A4B (`models/gemma-4-26B-A4B-it-UD-Q4_K_M.gguf`) still passes every check above
  unchanged (25233.1M parameters, same as before; GPU hybrid, including the expert-slot MoE cache, still
  engages for it, since it has neither PLE nor shared K/V and one feed-forward width).
- **GPU vs CPU, E2B, after the Vulkan path above** (same `katana selftest` block that drives `forward_seq`/
  `forward` for every architecture, 23 synthetic tokens, exact-CPU reference): batch (`vk_g4_n`, prompt) vs
  exact CPU, worst logit difference **0.000429**; token by token (generation, through `forward()`, GPU upload
  on but `vkg4.ok` decode path off by default so this exercises `g4_layer`+`vk_matvecs`) vs exact CPU, worst
  difference **0.000035**; both same argmax as the CPU reference and well inside the tolerance (0.05) the
  project already uses for every other architecture's GPU-vs-CPU check. With `KATANA_VK_G4DEC=1` (forcing
  `vk_g4_n` at B=1 for decoding too) the difference is 0.000429 as well, same order. All "produtos" per-matrix
  checks (including the new `ple_inp_gate`/`ple_proj`) pass at ~1e-8, same as the 26B-A4B.
- **End to end, GPU and CPU**: `katana run models/gemma4e2b/model.gguf "Oi, como vai?" 30 0.5` — coherent
  Portuguese in both `KATANA_CPU=1` (pure CPU) and the default (GPU) run, and in a 600-token run crossing the
  512-token local attention window (so the ring buffer wraps and the shared-K/V donor layers keep being read
  correctly past that point) — no garbled text, no crash. `katana run models/gemma4e2b/model.gguf "The capital
  of Japan is"` -> `Tokyo.`; `"2 + 2 ="` -> `4\n\nThis is a simple arithmetic example.`; `"List three
  colors."` -> `Red, Blue, Green`. This is the actual logits-to-text check the project asks for, since there is
  no reference engine to diff against; tok/s printed by `katana run` itself (not a controlled benchmark): CPU
  ~3.6-3.8 tok/s generation, GPU (default) ~32-36 tok/s generation and ~50-70 tok/s on the prompt, on the same
  RTX 3050 Laptop (4 GB).

### Left for later (not started)

- **Vision and audio** (the "any-to-any" tag in the GGUF): need the per-layer image/audio embedding tables and
  the encoders; this GGUF only ships the text tower. Out of scope for this pass by request.
- **PLE on the GPU**: deliberately left on the CPU (see above) — small enough that it likely never pays for an
  extra round trip, but not measured in isolation.
- **`katana pack`/shards, `katana tune`, the expert-cache machinery**: not applicable (no experts) or not
  exercised for this model yet.

### Phone (Xclipse 540, Galaxy A56): why the batch path never engages, and what a tiled shader gets without it

Measured directly on the device (`adb`), not estimated. `katana vkcaps` (new diagnostic command, `d9a1653`)
confirms what the "Vulkan: sem matriz cooperativa fp16 16x16x16: prompt token a token" message already implied:
the Xclipse 540 has **no `VK_KHR_cooperative_matrix`** (0 shapes listed), so `vkg.coop` is false and the whole
batched path (`vk_plan`, `mm.comp`) is disabled — every token, prompt or generation, goes through the
token-by-token kernels. The device does have `VK_KHR_shader_integer_dot_product`, `shaderFloat16`, `shaderInt8`
and `storageBuffer8/16BitAccess`, all active; `subgroup_size` 64; `max_shared_bytes` 32768 (32 KB, smaller than
a discrete card); unified memory (`unified_memory=1`, `vram_mb=6500` is the shared budget, not separate VRAM).
Real baseline, this GGUF, this device (`katana run gemma4e2b.gguf "..." 40 0.5`, GPU path as it ships today):
**prompt 3.7 tok/s, generation 3.5 tok/s** — effectively CPU speed; the opportunistic GPU matvec path
(`vk_matvecs`) that helps on the notebook is not winning here.

The project's own earlier finding (`docs/optimization-roadmap.md`, "Prototype tools/vkgemm": an unorganized
compute shader on the notebook's RTX 3050 did 0.51 T mult/s; tiling with shared memory alone got to
1.17-1.75; vectorized loads (4 values/read) to 1.75-2.6 — a 3-5x gain with **no matrix hardware involved**)
was reproduced on this phone, as a standalone kernel microbenchmark (`tools/vkgemm_mobile/`, not wired into
the engine yet), at shapes matching this model's dense products (1536x1536, 12288x1536, 1536x12288, batch 64
and 256 tokens; fp16 storage, fp32 accumulate; every result checked bit-for-bit reproducible and validated
against a CPU fp64 reference before timing):

| Kernel | T mult/s | vs naive |
|---|---|---|
| naive (one output per thread, no tile, no vector) | 0.020-0.025 | 1x |
| tiled + shared memory + vectorized `f16vec4` loads (64x64x32 tile, 256 threads, 4x4 register block) | 0.095-0.101 | **~4.1x** |
| same tile, inputs packed int8 and accumulated with `dotPacked4x8EXT` (`GL_EXT_integer_dot_product`) | 0.064-0.068 | ~2.8x (slower than the fp16 tiled kernel) |

Consistent across all four tested shapes and both batch sizes (64 and 256 tokens) — the ~4.1x tiled/vectorized
gain and the int8-dot-product kernel landing about a third slower than the fp16 tiled one both held every time.
The int8 kernel is bit-exact against an int64 CPU reference (packed dot product is correct on this driver), it
is just not faster here: `dotPacked4x8EXT` is most likely software-emulated on this GPU rather than backed by a
dedicated instruction, so packing to int8 adds overhead (the packing itself, and 4 separate operands to unpack
per dot) without the throughput the instruction is supposed to buy. **Conclusion for this device: tiling +
vectorized fp16 loads is the real, hardware-independent win (as the roadmap's own notebook prototype already
showed); the int8 dot-product extension, while present and correct, is not worth using here.**

What this does **not** yet answer: the end-to-end generation tok/s if this kernel replaced the token-by-token
path. `mm.comp` (the engine's real batched kernel) already does the hard part for every weight type (Q4_K/Q5_K/
Q6_K/Q8_0/Q5_1 decode into a shared-memory tile, vectorized) — only its inner step
(`coopMatLoad`/`coopMatMulAdd`/`coopMatStore`) is coopmat-specific. Swapping that inner step for the
register-blocked scalar loop proven in `tools/vkgemm_mobile/tiled_vec.comp`, and gating `vkg.coop` in
`vk_plan` (`src/engine/vk_dense.c`) on `fp16` instead of requiring `coopmat`, is the next step — not done in
this pass to avoid touching `vk_dense.c`/`mm.comp` while other work was in flight there, and because it needs a
correctness pass per weight type (same GPU-vs-CPU check already used for every other architecture) before it
can default on. `tools/vkgemm_mobile/bench.c` is a standalone, reusable harness (device probe, CPU-reference
validation, timed loop) for whoever picks this up.

### Update (2026-11-28): the swap above, done, and the real end-to-end number on the device

`mm.comp` now has a `COOP=0` build (macro, same file): `fill()` — the weight decode into the shared-memory
tile, identical for every WT type — is untouched; only the inner 16x16x16 step changes, from
`coopMatLoad`/`coopMatMulAdd`/`coopMatStore` to the register-blocked scalar loop (4x2 outputs per of 32
logical "lanes", `tools/vkgemm_mobile/tiled_vec.comp`'s technique, read straight off the same shared tile).
`gen.sh` generates a `_tiled` SPIR-V for all 90 variants the engine loads (plain, glu, and the `_ws`/`_xs`/`_gx`
precise ones gemma4 actually uses through `vk_mm_p`). `vkg.coop` (`src/engine/vk_dense.c`) now means "batch
path usable" — true for real `coopmat` **or** for `fp16` + `storageBuffer16BitAccess` + enough shared memory —
and a separate `vkg.coopmat` flag (true capability) picks which SPIR-V set `vk_mm_spv`/`vk_mm_pw` load.
`KATANA_VK_FORCE_TILED=1` forces the tiled set even when `coopmat` exists, so the new path can be checked on a
machine that has coopmat (the notebook) without the phone. `qwen35.c`'s full DeltaNet-in-Vulkan build (the one
that uses `dn_chunk`/`dn_state`, which are coopMat-only and have no tiled variant in this pass) still checks
`vkg.coopmat` specifically, so it is unaffected and keeps falling back to its CPU-DeltaNet hybrid mode
(`vk_qwen35d.c`, which only touches `mm.comp` and does benefit) on a coopmat-less device.

**Numeric validation (notebook, RTX 3050, real coopmat available):** `katana selftest` on gemma4e2b, olmoe and
granite, normal and with `KATANA_VK_FORCE_TILED=1`, same argmax both times and the logit difference in both
cases of the same order (gemma4e2b batch vs token-by-token: 4.37e-04 normal, 1.05e-04 forced-tiled; olmoe: 0.035
vs 0.0027; granite: 0.0013 vs 0.0014) — the tiled path is not less accurate than coopMat here, if anything
slightly better on two of the three. qwen3.6 unaffected (still builds the full coopmat graph; same output).

**On the device (Galaxy A56, `adb`, binary cross-compiled with NDK r27c for `aarch64-linux-android30`,
`katana version` `6d165c9`), `katana vkcaps` confirms the gate now opens: `shaderFloat16=1`,
`storageBuffer16BitAccess=1`, `max_shared_bytes=32768` (exactly the threshold). Before this pass (binary at
`2609c7c`, `!vkg.coop` was true because it only accepted real coopmat), `vk_gemma4.c`'s whole Vulkan graph for
gemma4 — not just the batched prompt path, the per-token one too — returned 0 immediately and gemma4 never
touched the GPU on this phone at all.** Measured with `katana run gemma4e2b.gguf "..." N 0.5`, same prompt
alternated a few times on each binary (not a formal alternated benchmark, just enough to rule out a one-off):

| Prompt | Before (`2609c7c`, no GPU graph) | After (`6d165c9`) | After, `KATANA_VK_BATCH=0` (same binary, batching off) |
|---|---|---|---|
| "Oi, como vai?" (6 tok) | prompt 3.4-3.5 tok/s, gen 3.7 tok/s | prompt 2.4 tok/s, gen 3.8 tok/s | — |
| ~275-token prompt (tripled paragraph) | prompt 4.1-4.2 tok/s, gen 3.3 tok/s | prompt **7.0-7.3** tok/s, gen 3.3-3.5 tok/s | prompt 6.3-7.0 tok/s, gen 3.5 tok/s |

Two honest readings, not the one hoped for going in: (1) **the win is real and reproducible for a prompt long
enough to amortize the per-dispatch setup** (~70% more prompt tok/s at 275 tokens), but it is mostly the
*existence* of `vk_gemma4.c`'s Vulkan graph turning on at all (`vkg.coop` was false, now true) — the
`KATANA_VK_BATCH=0` column shows that disabling just the multi-token batching inside the now-enabled graph
barely moves the number (6.3-7.0 vs 7.0-7.3), because the per-token matvecs inside that same graph already call
the same tiled `mm.comp` kernel with B=1. (2) **For a 6-token prompt the new path is slightly slower than the
old CPU-only path** (2.4 vs 3.5 tok/s) — fixed GPU dispatch/buffer-setup cost for the now-enabled graph is not
amortized over so few tokens; generation (always token-by-token, B=1, unaffected by the batch/no-batch
distinction) is flat to slightly better in every case (3.7-3.8 tok/s). The isolated kernel benchmark's 4.1x
(section above) does not show up end to end: `fill()`'s weight decode (identical in both paths) and VRAM
bandwidth for the same quantized bytes, not the final 16x16x16 reduction, dominate the real workload's time —
exactly what `docs/research-hardware-algorithms.md` section 1.2 already says about this device ("any technique
that does not reduce bytes read per token... has a low ceiling"). The kernel-level proposal was right that
tiling beats the naive scalar path (now proven integrated, not just isolated); it was the generalization to
end-to-end tok/s that this pass had to measure rather than assume.

OLMoE on the same device, same two binaries, short prompt: before 1.3 tok/s prompt / 6.3 tok/s generation,
after 1.6 / 5.8 — noise-level either way (OLMoE's cost on this device is dominated by expert residency/paging,
`especialistas: X% na VRAM`, not the dense product this pass touches). Qwen3.6 not re-tested on the device in
this pass (its Vulkan path needs real `coopmat`, confirmed absent on this GPU by `vkcaps`, so it is logically
unaffected and was only re-checked on the notebook, where it is unchanged).

## IBM Granite 4.0 H Tiny in the engine (CPU + GPU hybrid, working since 2026-11-27/28)

Model: `models/granite/model.gguf` (unsloth/granite-4.0-h-tiny-GGUF, Q4_K_M, ~4.1 GB). First architecture in
katana with a **Mamba-2** (state-space) layer type: `general.architecture = granitehybrid` is a hybrid of
Mamba-2 (SSD) layers and full-attention layers, with mixture-of-experts (MoE, softmax router, shared experts)
on every layer, none of which the engine had before (`src/engine/qwen35.c`'s DeltaNet is a different, gated
linear-attention formula, not Mamba-2). Implemented in a new file, `src/engine/granite.c`. The Mamba-2 formula
(state-space duality, SSD) is public/academic (Dao & Gu, 2024); understood here by reading the reference
engine's C++ source (`ref/llama-src/src/models/mamba-base.cpp`'s `build_mamba2_layer`,
`ggml/src/ggml-cpu/ops.cpp`'s `ggml_compute_forward_ssm_scan_f32`, `granite-hybrid.cpp`) and the published
Mamba-2 paper, only to confirm the exact arithmetic — nothing copied; the katana code is its own, in the
project's own style (see `CLAUDE.md`).

### Metadata and tensors confirmed in the GGUF (40 layers, dim 1536, 64 experts/6 used, 6939.0M counted)

| Key | Value | What it is |
|---|---|---|
| `granitehybrid.attention.head_count_kv` | array[40]: 0 except layers 5/15/25/35 (= 4) | 0 = Mamba-2 layer, else attention (`L->lin`) |
| `granitehybrid.ssm.conv_kernel` / `.state_size` / `.group_count` / `.time_step_rank` / `.inner_size` | 4 / 128 / 1 / 48 / 3072 | conv window, `d_state`, `n_group`, `n_head` (reuses the `time_step_rank` key, which means something else for DeltaNet), `d_inner` |
| `granitehybrid.attention.scale` | 0.0078125 (= 1/128, not `1/sqrt(hd)` = 1/11.3) | q.k scale, used as-is (muP), not derived from head width |
| `granitehybrid.embedding_scale` / `.residual_scale` / `.logit_scale` | 12.0 / 0.22 / 6.0 | muP scales: embedding (once), each sub-layer's output before its residual add (twice per layer), final logits (divide) |
| `granitehybrid.expert_count` / `.expert_used_count` | 64 / 6 | MoE, softmax router, renormalized (no `expert_gating_func`/`expert_weights_norm` key in this GGUF, so the default softmax + no-renorm engine behaviour had to be overridden on purpose for this arch, see below) |
| `granitehybrid.expert_feed_forward_length` / `.expert_shared_feed_forward_length` | 512 / 1024 | expert width; shared-expert width (2 shared experts, always summed, no gate) |

Per Mamba-2 layer: `blk.N.ssm_in.weight` [1536, 6448] (fused projection: `z`(3072) + `x|B|C`(3328) +
`dt`(48), in that order — confirmed against `mamba-base.cpp`'s view offsets), `ssm_conv1d.weight` [4, 3328] +
`.bias` [3328], `ssm_a`/`ssm_d` [1, 48] (already the final per-head scalars, no `-exp()` transform needed —
confirmed by using them directly, matching `ggml_compute_forward_ssm_scan_f32`), `ssm_dt.bias` [48],
`ssm_norm.weight` [3072] (grouped RMSNorm, `n_group=1` here so one norm over the whole vector), `ssm_out.weight`
[3072, 1536]. Per attention layer: plain `attn_q`/`attn_k`/`attn_v`/`attn_output`, no bias, no q/k norm, no MLA.
Every layer: `ffn_gate_inp`/`ffn_{gate,up,down}_exps` (MoE) + `ffn_{gate,up,down}_shexp` (2 shared experts fused,
`n_shared` computed the same way as the other MoE architectures). RoPE is the "normal" (adjacent-pair)
convention, not NEOX (confirmed in the reference engine's `llama_model_rope_type`: granite falls in the same
list as `llama`/`deepseek2`, not `qwen2`/`qwen3`), full head width (`rope.dimension_count` = `hd` = 128, so no
partial rotation).

### Mamba-2 formula (one token; batched prompt does the projections for all tokens at once, then the
convolution and recurrence token by token, same structure as the DeltaNet code)

```
z, xBC, dt = split(ssm_in(xb))                              # 3072, 3328, 48
xBC = silu(causal_conv1d(xBC, window=4) + conv_bias)         # state: last 3 taps per channel
x, B, C = split(xBC)                                         # 3072 (48 heads x 64), 128, 128
per head h (48 heads, 1 group so every head reads the same B/C):
  dt_sp = softplus(dt[h] + dt_bias[h]);  dA = exp(ssm_a[h] * dt_sp)
  S[h] = S[h] * dA + outer(B, x[h] * dt_sp)                  # state [128][64], carried across positions
  y[h] = S[h]^T . C  +  x[h] * ssm_d[h]
y = silu(z) * y                                               # gate
y = rmsnorm(y) * ssm_norm                                     # n_group=1: one norm over all 3072
out = ssm_out(y)
```

Each layer (Mamba-2 or attention) and the feed-forward both scale their output by `residual_scale` before
adding it to the residual stream (muP; two `ggml_scale` calls per layer in the reference graph, confirmed by
reading `granite-hybrid.cpp`'s `build_layer_ffn`/`build_attention_layer`). Final logits are divided by
`logit_scale` (`src/engine/forward.c`'s `logit_softcap`, extended; 0 = no-op, so every other architecture is
unaffected).

### Numeric verification

- **Exact product vs fast CPU kernels** (`katana selftest`): every new weight matrix — `ssm_in`/`ssm_out` of a
  Mamba-2 layer and `attn_q/k/v/output` + expert 0 of an attention layer (the generic two-layer sample in
  `qtest.c` only ever lands on Mamba-2 layers for this model, so a third, explicit attention-layer check was
  added) — worst relative error 4.61e-08 (exact) and 3.18e-06 (CPU integer path), same order as every other
  architecture.
- **Mamba-2 recurrence, a second formula**: `grh_recur_selftest` (`katana qtest`) runs the production
  per-head kernel (`grh_job`) against an independently written, double-precision version of the same formula
  (synthetic heads/state, 5 steps with the state carried between them, like real positions) — worst difference
  **1.24e-07**. Same method as the recent `dq_q4k_selftest` (NEON x scalar) in `model.c`.
- **Batch (`grh_layers_n`, prompt, B up to 16) vs token by token (B=1), through the real `forward_seq`/
  `forward` dispatch**: 23 synthetic tokens, split across a batch boundary (16+7) so the Mamba-2 state and
  conv history have to carry correctly between calls — **0.000e+00 difference**, same argmax. New `qtest.c`
  check (`c->gr` block, mirrors the `c->g4` one).
- **Against an independent, from-scratch computation (not the engine's own code, not the reference engine
  either)**: wrote a throwaway, stdlib+numpy-only Python script that reads the raw GGUF bytes, dequantizes
  Q4_K/Q6_K by hand, and recomputes layer 0's whole Mamba-2 path in double precision (embedding -> rmsnorm ->
  `ssm_in` matmul -> conv -> recurrence -> gate -> grouped norm -> `ssm_out` matmul) for the real token id used
  by `rawlogits`. katana's own numbers match this ground truth at every one of those steps to float32 rounding
  (e.g. `ssm_out` output: ground truth sum=36.2554/sumsq=2125.4731, katana sum=36.2480/sumsq=2125.4774, first
  three values matching to 4-5 significant digits). This is the strongest check done here, independent of any
  other engine's code.
- **Against the reference engine on the same raw token ids** (new hidden command `katana rawlogits <gguf>
  [N]`: `N` synthetic ids, `(1000003*(i+1)) % vocab`, no tokenizer involved, so this does not depend on the
  `dbrx` pre-tokenizer below): for a single token the argmax already matches (token 198 both ways) but the
  logit values differ by a few percent; for 23 tokens the final argmax differs. Traced layer by layer (`cb_eval`
  on a locally built, throwaway copy of the reference engine, never committed, never shipped — see
  `ref/llama-src/CLAUDE.md`/`AGENTS.md`, which only restrict contributing *to that project*, not reading it for
  this purpose) down to the exact step: the Mamba-2 scan itself (`mamba2_y_add_d`, i.e. everything up to and
  including the state-space recurrence and the `D` skip) matches between katana and the reference engine to
  1e-5 relative — but the reference engine's own gate+norm+`ssm_out` output for layer 0 is *already* ~2-3% off
  from the from-scratch ground truth above, more than katana's. Most likely explanation: the reference
  engine's own quantized-activation GEMM for this specific Q4_K matrix is less precise than katana's for this
  model (both engines' "exact" vs "fast" paths agree closely *within themselves*, per the first check above).
  A few percent of per-layer error, compounding over 40 layers through a MoE-routed (discontinuous: top-6 of
  64 experts) network, is enough to flip which experts get picked partway through and diverge the final
  answer — a known behaviour of quantized MoE models across different inference engines, not evidence by
  itself that either engine's formula is wrong. Given the ground-truth match above and the end-to-end text
  below, this is treated as inconclusive rather than a katana bug, but is **not fully proven** either way for
  all 40 layers; flagged here for whoever picks this up next.
- **End to end** (no chat template yet, plain completion): `katana run models/granite/model.gguf "The capital
  of France is"` -> `Paris.` (then degenerates into repetition, expected for greedy decoding without the real
  sampler defaults tuned for this model); `"2 + 2 ="` -> `4`; `"List three colors:"` -> `red, green, blue.`
  Coherent and correct, the best signal available without a reference engine to diff against.

### GPU (Vulkan) hybrid for MoE, attention and the dense Mamba-2 projections (working since 2026-11-28)

New file `src/engine/vk_granite.c` (`vk_gr_build`/`vk_gr_n`), wired into `mla_gpu_upload` (`src/engine/shards.c`)
and into the `forward`/`forward_seq_in` dispatch (`src/engine/forward.c`), the same shape already used for the
gemma4 and qwen35-dense hybrids (`vk_gemma4.c`, `vk_qwen35d.c`): no recorded Vulkan graph (the layers are not
uniform — Mamba-2 vs attention — and the "grafo completo" path, `vk_emit_n`, assumes every layer is attention),
instead a residual kept on the CPU, with the big matrix products going to the GPU each layer and coming back:

- **Dense projections (GPU)**: `ssm_in`/`ssm_out` of a Mamba-2 layer and `attn_q/k/v/output` of an attention
  layer, through the same precise batched product (`vk_mm_p`, weights decoded in two fp16 parts, activations
  in fp32) already used by gemma4/qwen35-dense; `gr_mul` falls back to the CPU product per matrix when a
  matrix did not make it to VRAM or its shape is not a multiple of 64 (same guard as `vk_qwen35d.c`'s
  `qd_mul`), so nothing breaks if a future, bigger Granite checkpoint has odd dimensions.
- **MoE (GPU), every layer, with the shared experts**: reused as-is, no new kernel — `vk_moe_init`/`vk_moe_n`
  (`src/engine/vk_moe.c`), the exact "vagas" expert cache and batched routing already serving gemma4 and
  qwen35moe. The only Granite-specific orchestration: `vkg.fn[l]` (the per-layer `ffn_norm` vector, which
  `vk_moe_n` reads directly from the GPU) had to be uploaded by `vk_gr_build` itself, since Granite does not go
  through the "grafo completo" setup that normally fills it; and the MoE output comes back through its own
  buffer (`vkgr.moe_out`, zeroed before each call) instead of being added straight into the residual buffer,
  because `residual_scale` (muP) has to multiply the feed-forward's output *before* the add — the same reason
  gemma4 routes its MoE output through its own buffer instead of `vkb.x` directly (its `post_ffn2` norm).
- **Attention core and Mamba-2 (CPU, on purpose)**: RoPE, the KV cache and the softmax/weighted-sum (`att_job`)
  for attention layers, and the causal convolution + SSD recurrence + gate (`grh_conv`/`grh_recur`/`grh_gate`,
  unchanged from `src/engine/granite.c`) for Mamba-2 layers, all stay on the CPU. The SSD recurrence is
  sequential by construction (each position's state feeds the next), so there is little to parallelize per
  token on the GPU without a dedicated chunked-scan kernel (the kind the reference engine and DeltaNet-style
  engines use for Mamba-2/-1 on CUDA); writing one was judged too large for this session (a kernel class the
  project has not needed before, on top of validating it numerically) against the measured benefit — see
  below — so, like the gemma4 PLE residual (small matrices, sequential, not worth the round trip), the
  recurrence and the attention core stay on the CPU and only the big matrix products move to the GPU. This is
  a deliberate hybrid, not an unfinished GPU path: `vk_gr_build` returns 1 (GPU on) whenever the MoE expert
  cache and the projection buffers fit in VRAM.
- **Decoding (one token) stays on the CPU by default**: same finding as gemma4 (`KATANA_VK_G4DEC`) — the
  round trip to the GPU per layer for a single token does not pay for itself at this batch size. `vk_gr_n`
  does accept `B = 1` (validated below) and `KATANA_VK_GR_DEC=1` turns it on for decoding too, but the
  default keeps `forward()`'s token-by-token path on `grh_layers_n` (CPU) and only uses `vk_gr_n` for batches
  (the prompt, `forward_seq_in`, `KATANA_VK_GR=0` turns the whole hybrid off).
- A pre-existing buffer-sizing bug surfaced and was fixed along the way: `vk_setup`'s single-token GPU
  work buffers (`gpu.vxb`/`gpu.vob`, used by `katana selftest`'s per-matrix GPU check and by the hybrid
  "host" mode of other architectures) were sized from `dim`/`hidden`/`n_heads*hd`/`hidden_dense`/
  `n_shared*hidden` only, never from `ssm_inner` — harmless for the DeltaNet models (`ssm_inner` happens to be
  smaller than those), but Granite's `ssm_inner` (3072) is bigger than `dim` (1536), which overflowed the
  buffer and crashed Vulkan as soon as `ssm_out` (3072 columns) was tested. Fixed with one extra bound in
  `vk_setup` (`src/engine/vk_dense.c`); also benefits any future DeltaNet/Mamba checkpoint with a large
  `ssm_inner`.

### Numeric verification of the GPU path

Same method and tolerance as every other architecture's GPU work this session (gemma4 included), via
`katana selftest models/granite/model.gguf`, GPU vs CPU:

- **Per matrix** (uploaded `ssm_in`/`ssm_out`, `attn_q/k/v/output`, the shared experts' `gate`/`down`): GPU
  single-vector product vs the exact CPU product, worst relative error **4.59e-08** (same order as the
  CPU-only numbers already in this section; the routed experts themselves are not tested individually here,
  same as every other MoE architecture — they go through the "vagas" cache, not a plain uploaded matrix).
- **Whole pipeline, batch (`vk_gr_n`, prompt) vs exact CPU, 23 tokens**: maior diferenca de logit **1.1e-03**,
  same argmax — well inside the project's logit tolerance (~0.05).
- **Whole pipeline, one token at a time (`vk_gr_n`, `KATANA_VK_GR_DEC=1`) vs exact CPU, 23 tokens**: maior
  diferenca de logit **6.9e-04** to **1.9e-03** depending on the run (float16-tiled products have run-to-run
  tiling variance, like every other Vulkan batch path in this engine), same argmax.
- `katana run models/granite/model.gguf "Oi, como vai?" ... 0.5`, with and without `KATANA_CPU=1`: coherent
  Portuguese in both. With a short prompt (5-21 tokens) the observed tok/s for prompt and decoding were close
  between GPU and CPU-only (noise-level differences, consistent with the decode-on-CPU default above and with
  a cold expert cache during a short run); with a longer prompt (94 tokens) the GPU batch path measured faster
  for the prompt phase (~48 vs ~36 tok/s) in one, informal, non-alternated run — not a controlled benchmark
  (none were done this phase, per the project's current rules), just the ordinary `katana run` output, reported
  as observed and not as a result.

### Left for later (not started)

- **Chat template / tool calling**: the GGUF's Jinja template is a full tool-calling format (own
  `tools_system_message_prefix` etc.); `chat_fmt` was not mapped for `granitehybrid`, so `katana chat` falls
  back to plain text. Needs its own pass.
- **Tokenizer pre-tokenizer**: `tokenizer.ggml.pre = "dbrx"` is not one of katana's known pre-tokenizer kinds
  (`PRE_*` in `tokenizer.c`); it currently falls back to the legacy pattern, which is good enough for the plain
  English above but not guaranteed byte-exact for code, numbers or non-English text. Needs the actual DBRX
  regex.
- **Mamba-2 on the GPU**: a real SSD chunked-scan kernel, if a future measurement shows the CPU recurrence is
  the bottleneck on a machine without a fast CPU (e.g. the phone from the handoff notes, which currently has no
  GPU path for this architecture at all). Not attempted this session (see above).
- **The 40-layer reference-engine divergence above**: not fully explained; worth finishing if a GPU-free,
  bit-exact cross-check matters more than it does today.
