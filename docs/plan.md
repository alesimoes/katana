# katana: plan

Written on 2026-11-15. Each step has a measurable done criterion. Context is in
`knowledge.md`; techniques are in `optimization-roadmap.md`.

## Priority 1: Vulkan backend (closes the 11–16x gap on the prompt)

| Step | What | Done when |
|---|---|---|
| 1 | Batch with Q4_K (and Q6_K) in the katana layout, decoding at tile load (`tools/vkq4k`) | **done**: 1.96–2.69 T mult/s on Q4_K and 2.17–3.84 on Q6_K, verified. Still missing: a narrow tile for small batches |
| 2 | Runtime layer (`vk/kvk.h`) and port of the per-token kernels (`tools/vkmv`) | **done**: tests pass (runtime on both cards; kernels on NVIDIA). Throughput at the same level as OpenCL, still without an identical measurement. Missing: per-token batch, several experts per launch and fused kernels |
| 3 | Integration into `katana.c`: backend selection (`KATANA_BACKEND`), weight loading, per-token product | **done**: `selftest` (products) passes through Vulkan on 5 dense models; the graph still runs on the CPU in this backend. Not tested: MoE, Intel card, fallback to OpenCL when Vulkan fails |
| 4 | Full dense graph in Vulkan (norms, attention, final projection), generation | **done** (measured alternating): Qwen 1.5B 99.7 tok/s on both backends; SmolLM2 135M 409 tok/s on Vulkan against ~130 on OpenCL (reference Vulkan 223, CUDA 402). `selftest` passes on 5 models and the greedy text comes out identical to OpenCL's. Prompt still one token at a time (121 against 212 tok/s) |
| 5 | Batched prompt with tensor cores | **done**: Qwen 1.5B 1,387 tok/s on the 64+64 benchmark (was 212 on OpenCL; reference 2,343), Llama 3B 757, SmolLM2 8,100. Still missing: a faster product (today ~2.7 T) and cheaper attention to get close to the reference |
| 6 | MoE on Vulkan: expert cache, cold experts on the CPU | Qwen3-30B passes `selftest`; generation equal to or better than today's |
| 7 | Graph recorded and resubmitted per token | **done in step 4**: 409 tok/s on the 135 M model |

Pending decisions: Vulkan headers inside the repository or declared by hand; SPIR-V generated
at build time or versioned as a C header (versioned spares the shader compiler for anyone who only
builds the engine).

## Priority 2: DeepSeek/Kimi architecture

The "trillion-parameter model" target is from the Kimi family (MoE with low-rank attention). Kimi K2 is
~600 GB at 4 bits and does not fit on this machine; the code is validated on a small sibling.

| Step | What | Done when |
|---|---|---|
| 1 | Download DeepSeek-V2-Lite (16 B, ~10 GB) and read the metadata | `katana info` shows the architecture and the tensors |
| 2 | Low-rank attention on the CPU: two-stage q, compressed K and V shared across heads, RoPE on a slice, per-head decompression | greedy output matches the reference engine for dozens of tokens |
| 3 | Sigmoid router with bias, shared expert, initial dense layers | same |
| 4 | GPU path and expert cache | `selftest` passes; measure against the reference |
| 5 | Test with Kimi, if there is storage (external SSD ≥ 1 TB) | it runs, at whatever speed it gets |

## Priority 3: CPU for the cold experts

On Qwen3-30B almost half of the time per token is CPU.

- 8-bit activations with a scale per block of 256 and partial sums. Criterion: same token chosen
  as the 16-bit path over 1,000 tokens of real text, and faster.
- AVX-512 VNNI with run-time selection.
- 8 rows per step.

Estimated gain (not measured): up to 1.5–2x on what fits in cache.

## Priority 4: what makes the engine an assistant

- Exact pre-tokenizer by the type declared in the GGUF. Criterion: same tokenization as the reference
  engine on a set of texts (code, numbers, accents, line breaks).
- KV cache: attend only to the positions in use, prefix reuse, context shifting, context
  larger than 4,096.
- Sampling: repetition penalty, top-p, min-p.
- Context compaction: summarize old turns when the context fills up, and measure what is lost.
- Agent: JSON output guaranteed by grammar, more tools, configurable limits.

## Priority 5: network between machines

- Each machine holds part of the experts and answers for them; the one that asked sums the results.
- First experiment: two instances on the same machine exchanging experts over a socket, to
  measure the round-trip cost per layer.
- Depends on a second machine for the real test.

## Out of priority (with reasons)

- Fusing more kernels in OpenCL: no measured gain above 1 B.
- Uneven VRAM slots per layer: 1 percentage point below the ceiling.
- Using more than 92% of VRAM: it gets worse.
- Experts with fewer bits only in VRAM: changes the model's output; only as a measured experiment.

## How to record progress

When a step is finished: measured number (alternating when it is a comparison), update of the table in
`knowledge.md`, commit and push. A hypothesis that was not confirmed also goes into `knowledge.md`,
in the list of what gave no gain.
