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

Points 1 to 4 are implemented. The next ones are the plan (below), in order.

5. **Specializations per request** (next): the server keeps one base in memory and applies the adapters of the
   specialization named in the request's `model` field, unmerged (`W x + s B (A x)`), so one machine serves many
   specialists without reloading.
6. **Heat-guided specialization of mixture-of-experts models**: katana already measures which experts each topic
   uses (expert heat, `katana routing`). Specializing only the experts that are hot on the user's data (plus the
   router) trains a small fraction of a 35B model, and those are the experts the expert cache keeps in VRAM anyway.

## Commands

```
katana gradcheck <model.gguf> [T]
katana train <model.gguf> <data.jsonl> [--train PATTERNS] [--rank R] [--alpha A] [--steps N] [--lr X]
             [--seq T] [--wd X] [--clip X] [--seed N] [--every N] [--out file.kspec]
KATANA_SPEC=file.kspec katana serve|chat|agent|run <model.gguf> ...
```

- `--train`: comma-separated patterns over GGUF tensor names, `*` matches anything (`blk.*.ffn_*`,
  `blk.1*.attn_q.weight`, `output_norm.weight`, `all`). Default `all`.
- `--rank R`: matrices get a rank-R adapter instead of training the whole matrix (A random, B zero: training starts
  exactly at the base model); norms and the embedding still train whole. `--alpha` sets the scale `s = alpha / R`
  (default `alpha = R`).
- Data: JSONL lines with `text` and optional `train_spans` (character offsets of the parts to learn, as written by
  `tools/dataset/ksess2jsonl.py`). Without `train_spans` every token is a target. Long texts are cut into windows of
  `--seq` tokens; windows without targets are skipped. A line whose tokens do not reproduce its text byte for byte is
  skipped with a warning (the spans would not line up).
- Optimizer: AdamW (betas 0.9/0.999), global gradient-norm clipping (default 1.0), 10-step warm-up, weight decay only
  on matrices.
- Loading: whole tensors replace the base tensor; adapters are merged at load (`W + s B A`, dequantizing only the
  adapted matrices to float). The specialization is applied before the GPU is prepared.

## How correctness is checked

`katana gradcheck` runs two tests on a random sequence:

1. **Training forward against the engine**: the training forward (written separately, keeping every activation)
   must give the same logits as the engine's own `forward()` at every position.
2. **Gradients against finite differences**: for every tensor (and, in a second pass, for the A and B of rank-3
   adapters on every matrix), the analytic gradient projected on a random direction is compared with a central
   difference of the loss along that direction. Tolerance: 1% relative plus 1e-4 absolute (float rounding dominates
   on the smallest gradients; a wrong formula shows up as a large, step-independent error).

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
| AddressSanitizer + UndefinedBehaviorSanitizer | gradcheck, train and loading: no reports |

What this proves: the backward pass is right for this layer layout, the optimizer converges, the frozen quantized
path works, and the file round-trips into the serving engine. What it does not prove: anything about quality on a
real model, or speed. Nothing was run on Qwen3.6 or Gemma 4 (they need the work below).

## Limits today

- Architectures: dense llama/qwen2-style only (RMSNorm, GQA, RoPE in pairs or halves, SwiGLU, optional q/k/v/o bias).
  Not yet: q/k norm (qwen3), MoE routing, Gated DeltaNet and gated attention (Qwen3.6), Gemma 4 layers.
- CPU only, one sequence per step, attention probabilities stored for the whole sequence (T x T per head and layer).
- Adapters are merged at load, so an adapted matrix costs float memory at serving time.

## Next steps, in order

1. Run `tools/traintest/run.sh` on the owner's machine, then a first real run on a small dense model with sessions
   exported from the agent.
2. q/k norm (qwen3), then the MoE layer: router softmax/top-k backward (straight-through on the selection), expert
   gradients only for the experts a token used; heat-guided selection of trainable experts (`--train` by heat).
3. Gated DeltaNet and gated attention backward (Qwen3.6), Gemma 4 layers, each with its own gradcheck.
4. Memory: recompute attention in the backward pass instead of storing it; gradient accumulation over several windows.
5. Speed: the matrix products of forward and backward on the GPU (Vulkan), reusing the batch kernels of the prompt path.
6. Serving: adapters applied unmerged in the products and selected per request (`model` field), hot-swapped.
7. Optional: merge a specialization into a new GGUF and requantize it (`katana quantize` already writes GGUF).
