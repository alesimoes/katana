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
- There is no DeepSeek-V2-Lite-Chat Q4_K_M in bartowski/lmstudio. The legraphista `Q4_K` is the
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
  `KATANA_G4_ENVLAST=1` moves the per-folder block after the declarations (only ~430 tokens recomputed per folder)
  but puts text where the template never puts any; it stays off until a codebench pass shows it does not hurt.
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
  (`KATANA_G4_PLAIN_RESULT=1` restores the old plain block).
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
  conversion (before, with `KATANA_TOOLS=1` it saw no tools on Gemma 4), plus the table tools, the old tools named in
  the prompt and the MCP tools. It does not switch on inside the thought channel, also when the prompt opened the
  channel after tool results (`g_g4_thought_open`). A string value may no longer contain `<tool_call|>`,
  `<|tool_call>` or `<|tool_response>` (single tokens in Gemma 4: the random tiny model produced them inside a value
  once the tiny vocabulary had the real special tokens, and the call reader cut the call there).
- **Mechanisms ported on 2026-11-21** (each with a switch): live compaction inside a long request and for `/compact`
  (`KATANA_COMPACT_LIVE=0` off, both formats); reading notes and the sub-agent `delegate`/`Agent` (`KATANA_G4_SIDE=0`
  off; `KATANA_DIGEST=0` still turns off the notes in every format). Side contexts and the summary run with the empty
  thought channel (no reasoning). Tests: `tools/traintest/gemma4_part.sh` (tiny Gemma 4 model with the real special
  tokens and their types, `make_tiny_gguf.py --arch gemma4 --template gemma4`, scripted stand-in teacher).

Checks that remain for the owner's machine once the GPU is free (Gemma 4, real model):
1. First start time in a new folder (default order) and in a folder already seen; the same with `KATANA_G4_ENVLAST=1`.
2. Codebench pass on the `write-*`, `conv-*`, `prereq-*` and `extend-*` tasks, default switches; then with
   `KATANA_G4_ENVLAST=1` to decide its default.
3. Tool-call validity per 100 calls in `agente.log` with and without the constraint (`KATANA_CALL_GRAMMAR=0`), thinking
   off (desktop) and on.
4. Compaction time on a long job (live summary against `KATANA_COMPACT_LIVE=0`) and the quality of the summary.
5. One sub-agent call and one large `read_file` note (time, note quality, main context reloaded).
