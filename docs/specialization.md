# Specialization: katana's own training engine

Goal set by the owner (2026-11-20): katana specializes models itself, with no external tool or package. Techniques
are fair game (backpropagation, AdamW, low-rank adapters are knowledge, not dependencies); the code is ours, in the
same C binary that runs and serves the model.

## What makes it different

1. **One binary does everything.** The engine that serves a model also trains it: `katana train` writes a
   specialization, `KATANA_SPEC` loads it, `katana serve` answers with it. No Python, no framework, no conversion step.
2. **The frozen base stays quantized.** Products with frozen weights, forward and backward (the transposed product
   that carries the gradient through a frozen layer), read each row straight from the GGUF format (Q8_0, Q4_K, Q6_K,
   ...), dequantized on the fly. Only what is trained gets a float master, a gradient and two AdamW moments (16 bytes
   per trained weight). Memory = the quantized model + the trained state + the activations of one sequence.
3. **Specializations are small files on top of an untouched base.** A `.kspec` holds only the trained tensors (whole,
   or as low-rank adapters A, B). The base GGUF is never rewritten; the file checks that it was trained on the same
   base (file size and parameter count) and refuses another.
4. **The data comes from the agent's own verified work.** Sessions record the exact text the model read and wrote in
   its own chat format; codebench says which sessions solved their task (`docs/training-data.md`). Training only on
   the model's turns of passed sessions turns "what worked" into "what the model does by default", in the exact tool
   format the agent parses.

5. **Heat-guided specialization of mixture-of-experts models** (`--experts N`): before training, the base model's
   router is run over the training data and, in each MoE layer, the N experts it picks most often at the positions
   that count for the loss become trainable (whole, or with adapters under `--rank`), together with the router.
   A small fraction of the experts carries the specialization; they are the experts that topic routes to anyway.

Points 1 to 5 are implemented. The next ones are the plan (below), in order.

6. **Specializations per request** (next): the server keeps one base in memory and applies the adapters of the
   specialization named in the request's `model` field, unmerged (`W x + s B (A x)`), so one machine serves many
   specialists without reloading.

## Commands

```
katana gradcheck <model.gguf> [T]
katana train <model.gguf> <data.jsonl> [--train PATTERNS] [--rank R] [--alpha A] [--steps N] [--lr X]
             [--seq T] [--wd X] [--clip X] [--seed N] [--every N] [--out file.kspec]
KATANA_SPEC=file.kspec katana serve|chat|agent|run <model.gguf> ...
```

- `--train`: comma-separated patterns over GGUF tensor names, `*` matches anything (`blk.*.ffn_*`,
  `blk.1*.attn_q.weight`, `output_norm.weight`, `all`). Default `all`.
- `--experts N` (MoE models): heat-guided choice of N trainable experts per layer (see point 5). Without `--train`,
  the router is trained too; with `--train`, the patterns add to the hot experts. Expert tensors are addressed as
  `blk.<l>.ffn_{gate,up,down}_exps.weight.e<expert>`.
- `--rank R`: matrices get a rank-R adapter instead of training the whole matrix (A random, B zero: training starts
  exactly at the base model); norms and the embedding still train whole. `--alpha` sets the scale `s = alpha / R`
  (default `alpha = R`).
- Data: JSONL lines with `text` and optional `train_spans` (character offsets of the parts to learn, as written by
  `tools/dataset/ksess2jsonl.py`). Without `train_spans` every token is a target. Long texts are cut into windows of
  `--seq` tokens; windows without targets are skipped. A line whose tokens do not reproduce its text byte for byte is
  skipped with a warning (the spans would not line up).
- `--plan 1`: load the model and the data, choose what trains (including the hot experts) and print the memory plan
  (activations of one window, trainer state, model size) without training. Use it first on a big model.
- `katana merge <model.gguf> <spec.kspec> <out.gguf>`: writes a new GGUF with the specialization inside (whole
  tensors replaced, adapters merged as `W + s B A`, trained experts written into their rows of the stacked tensor),
  each changed tensor in its original type when katana can encode it (F32, F16, Q8_0, Q6_K, Q5_K, Q4_K, Q2_K),
  otherwise Q8_0; everything else copied byte for byte; the file records `katana.specialization`. Checked: the merged
  GGUF answers exactly like the base with `KATANA_SPEC` on five tiny layouts (whole tensors, adapters on Q8_0, MoE hot
  experts, qwen35moe, gemma4). Gemma 4 experts come from one fused gate/up tensor; their parts are named
  `ffn_gate_up_exps.weight.eN.gate` / `.up` and go back to rows `2N·H` and `(2N+1)·H`.
- Optimizer: AdamW (betas 0.9/0.999), global gradient-norm clipping (default 1.0), 10-step warm-up, weight decay only
  on matrices.
- Loading: whole tensors replace the base tensor; adapters are merged at load (`W + s B A`, dequantizing only the
  adapted matrices to float). The specialization is applied before the GPU is prepared. A specialization that
  changes experts runs on the CPU for now (the GPU expert cache assumes one format for all experts of a layer).

## How correctness is checked

`katana gradcheck` runs two tests on a random sequence:

1. **Training forward against the engine**: the training forward (written separately, keeping every activation)
   must give the same logits as the engine's own `forward()` at every position.
2. **Gradients against finite differences**: for every tensor (and, in a second pass, for the A and B of rank-3
   adapters on every matrix), the analytic gradient projected on a random direction is compared with a central
   difference of the loss along that direction. Tolerance: 1% relative plus 1e-4 absolute (float rounding dominates
   on the smallest gradients; a wrong formula shows up as a large, step-independent error). In MoE layers the choice
   of the K experts is a step function with no derivative: the finite differences reuse the experts chosen in the
   base pass, which is exactly the gradient the trainer uses (the selection is held fixed, the weights of the chosen
   experts and the router logits behind them get the gradient). Each direction is estimated twice, by a plain
   central difference and by Richardson extrapolation (central differences at h and h/2, which removes the h^2
   curvature error that matters for parameters inside exponentials such as the DeltaNet decay); the closer estimate
   counts. A wrong formula misses both.

`tools/traintest/run.sh` runs the gradcheck on three layouts of the tiny random model (`tools/tinymodel`), trains it
with whole tensors and with adapters on a Q8_0 base, and checks that the server with `KATANA_SPEC` answers exactly the
trained replies at temperature 0.

## Measured (2026-11-20, cloud machine with 2 cores, no GPU, tiny random models only)

| check | result |
|---|---|
| training forward vs engine, max logit difference | 4.3e-6 (llama), 3.8e-6 (qwen2 layout with GQA and q/k/v bias), 5.0e-6 (Q8_0 base) |
| gradcheck, 21 whole tensors + 36 adapter A/B | pass on all three; largest relative error 1.4e-2, on gradients of ~0.002 |
| train all tensors, 150 steps, 3 examples | loss 12.0 -> 0.0001 in 1.0 s; `.kspec` 457 KB |
| rank-8 adapters on a frozen Q8_0 base, 400 steps | 0.018M trainable weights (15% of the model), loss 12.0 -> 0.0002 in 2.4 s; `.kspec` 70 KB |
| server with `KATANA_SPEC`, temperature 0 | the three trained answers word for word, each ending on its own end-of-turn token (before: random bytes) |
| training forward vs engine, MoE layouts | 5.5e-6 (qwen3, per-head q/k norm), 2.9e-6 (qwen3moe), 3.8e-6 (qwen3moe with shared expert, sigmoid gate, a leading dense layer and GQA), 3.6e-6 (olmoe: whole-vector q/k norm, no renormalization, K=3) |
| gradcheck on those four | pass (65 to 197 tensors and adapter parts each) |
| heat-guided MoE specialization: 2 hot experts of 8 per layer + router + attention and shared-expert adapters (rank 8) | the 2 hot experts cover 43% of the router's choices on the data; loss 12.9 -> 0.0002 in 2.5 s; `.kspec` 50 KB; server answers the three replies word for word |
| hot experts and router only (no attention) | loss 12.9 -> 0.49: on this tiny random model the experts alone do not carry enough capacity; with attention adapters they do |
| training forward vs engine, Qwen3.6 layout (qwen35moe: Gated DeltaNet + gated attention with partial RoPE + MoE with gated shared expert) | 3.8e-6; qwen35 dense: 5.4e-6 |
| gradcheck on qwen35moe and qwen35 | pass (204 and 72 tensors and adapter parts; DeltaNet tensors within 1%) |
| qwen35moe, rank-8 adapters on all layers, data in the model's thinking format | loss 12.5 -> 0.0001 in 2.2 s; `.kspec` 190 KB; the server answers the three replies word for word, with the empty thinking block kept out of `content` |
| training forward vs engine, Gemma 4 layout (gemma4: a sliding-window layer with head width 8 and 2 kv heads, a global layer with head width 16, 1 kv head and no `attn_v`, extra norms, GELU, dense FFN in parallel with 8 experts from a fused gate/up tensor, per-expert scales, embedding scale, logit softcap 30) | 1.2e-6; dense variant 9.2e-7; Q8_0 base 8.1e-7 |
| gradcheck on gemma4 (MoE, dense, Q8_0) | pass (237 tensors and adapter parts on the MoE one; the only parts above 1% relative are three expert gradients of size ~1e-5, inside the absolute floor) |
| gemma4, 2 hot experts of 8 + router + rank-8 adapters on attention + all norms | loss 13 -> 0.13 in 3.2 s; the server answers the three replies word for word, with the `.kspec` and after `katana merge` |
| AddressSanitizer + UndefinedBehaviorSanitizer | gradcheck (dense, MoE, DeltaNet, Gemma 4), train and loading: no reports |

What this proves: the backward pass is right for this layer layout, the optimizer converges, the frozen quantized
path works, and the file round-trips into the serving engine. What it does not prove: anything about quality on a
real model, or speed. Nothing was run yet on the real Qwen3.6 or Gemma 4 files (no model or GPU in the session that
wrote this); both layouts are covered on tiny random models.

## Training on the GPU (Vulkan)

The three products with a weight, which are most of the arithmetic, run on the GPU through `vk/shaders/tmm.comp`:
forward `Y = X W^T`, transposed `dX = dY W` and the weight gradient `G = dY^T X`. Attention, norms, the DeltaNet
recurrence and routing stay on the CPU for now.

- Frozen weights go to VRAM once, in their GGUF format (F32, F16, BF16, Q8_0, Q4_K, Q5_K, Q6_K; no repacking), while
  they fit. The rest is uploaded per product in slices. Other formats fall back to the CPU for that tensor.
- Float masters of trained tensors are uploaded again after each optimizer step (a version counter).
- One float32 kernel for all three modes and formats (64x64 tiles, each weight element dequantized once per tile and
  reused 64 times). It does not use cooperative matrices yet, so it runs on any Vulkan 1.2 GPU and is far from the
  card's peak.
- `KATANA_TRAIN_GPU=0` turns it off; `KATANA_TRAIN_VRAM=<GB>` caps the resident weights; `KATANA_TRAIN_GPU_MIN=<n>`
  is the smallest product (multiply-adds) sent to the GPU (default 4M; 0 sends everything, for testing).
- `katana train` prints `velocidade: N tokens/s` and the GPU totals at the end. To compare, run the same command
  with and without `KATANA_TRAIN_GPU=0`, alternating, in the same hour.

How it was checked, without a GPU (none in the session that wrote it; a software Vulkan device could not be built
there):

| check | result |
|---|---|
| the shader's own code (`tmm_core.glsl` is GLSL and C++ at once) run on the CPU with 256 real threads and barriers per workgroup, 7 formats x 3 modes, odd sizes and an offset weight, against the CPU dequant in double (`tools/traintest/tmm_emu.cpp`) | max relative error 1.1e-6 |
| katana with an emulated GPU (`-DKATANA_TG_EMU`: the `kvk_*` calls of the trainer go to host memory and run the shader code; `tools/traintest/tg_emu.*`), every product on it, gradcheck on the 11 tiny layouts, weights resident and weights forced into 4 KB slices | forward equals the engine (1e-6 to 1e-5); gradcheck passes on 10 of 11; olmoe misses on one adapter gradient of 4e-4 by 1.2e-4: the analytic value is identical in every run and the finite difference converges to it with a larger step, so it is float noise, not a gradient bug |
| emulated GPU, rank-8 adapters on Q8_0 and whole-tensor training | the server answers the three trained replies word for word |

Not measured: speed on a real card, and the kernel on a real driver. `tools/traintest/run.sh` runs gradcheck on the
real GPU (5 layouts) when one is present; that is the first thing to run on the owner's machine.

## Limits today

- Architectures: llama, qwen2, qwen3, olmoe, qwen3moe-style (RMSNorm, GQA, RoPE in pairs or halves, SwiGLU,
  optional q/k/v/o bias, q/k norm per head or whole-vector, softmax-routed MoE with top-K, renormalization and scale,
  shared experts with an optional sigmoid gate, leading dense layers), and the Qwen3.6 family (qwen35moe, qwen35):
  Gated DeltaNet (causal convolution, L2-normalized q/k, gated delta rule, gated RMS output) and gated attention
  with partial RoPE, and Gemma 4 (gemma4: per-layer head width and kv heads, sliding window on local layers, k used
  as v when `attn_v` is missing, weightless v norm, extra norms after attention and around both FFN branches, GELU,
  dense FFN in parallel with the experts, router input scale and per-expert scales trainable, embedding scale and
  logit softcap in forward and backward). `layer_output_scale` stays frozen. Not yet: sigmoid routers with bias
  (DeepSeek/GLM), gpt-oss, MLA.
- Memory: the DeltaNet keeps its recurrent state only every 64 tokens (`KATANA_TRAIN_DNCK`) and the backward pass
  recomputes one block at a time from that checkpoint; attention probabilities are not stored either (each row is
  recomputed in the backward pass). Activations still grow with the window times the model width; the window is
  `--seq`. MoE tokens are grouped by expert, so each expert row is read once per window, not once per token.
- Only the weight products run on the GPU; attention, DeltaNet and MoE routing are on the CPU, and each product copies
  its activations to the GPU and back. One sequence per step.
- Adapters are merged at load, so an adapted matrix costs float memory at serving time.

## Next steps, in order

1. Run `tools/traintest/run.sh` on the owner's machine, then a first real run on a small dense model with sessions
   exported from the agent.
2. Sigmoid routers with bias, gpt-oss and MLA.
3. The first real specialization of Qwen3.6-35B-A3B and Gemma 4 26B-A4B on the owner's machine: `--plan 1` first, then a short run on
   exported sessions, measured with codebench against the base model.
4. Gradient accumulation over several windows.
5. Speed on the GPU, after measuring the first version on the owner's card: activations kept in VRAM between
   products (no copy per product), attention and the DeltaNet recurrence on the GPU, cooperative-matrix variants of
   `tmm`, MoE experts in one grouped dispatch, adapters on the GPU.
6. Serving: adapters applied unmerged in the products and selected per request (`model` field), hot-swapped.
7. Optional: merge a specialization into a new GGUF and requantize it (`katana quantize` already writes GGUF).
