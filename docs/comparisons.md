# katana: all measured comparisons

Everything measured on 2026-11-15 on the same machine: i7-11800H, 64 GB, RTX 3050 Laptop 4 GB, Windows 11.
Reference engine: official binary b12418, in the CUDA and Vulkan versions. Speeds in tokens per
second (tok/s), unless stated otherwise. The machine drifts over the day; where it says "alternating", the
engines were measured in sequence A, B, A, B in the same hour.

## 1. Dense models: end-of-day situation (alternating, 64+64 benchmark)

| Model | | katana OpenCL | katana Vulkan | reference Vulkan | reference CUDA* |
|---|---|---|---|---|---|
| SmolLM2 135M Q8 | prompt | 1,227 | 5,972–8,478 | 7,995–8,408 | 6,708 |
| | generation | 119–139 | 385–409 | 198–223 | 402 |
| Qwen2.5 1.5B Q4_K_M | prompt | 212 | 1,349–1,364 | 2,362–2,374 | 2,776 |
| | generation | 98 | 98–100 | 113–115 | 117 |
| Llama 3.2 3B Q4_K_M | prompt | 108 | 753–761 | 1,357–1,364 | 1,685 |
| | generation | 57 | 56 | 63–64 | 69 |

\* CUDA measured earlier the same day, without alternating with the others.

Who wins where:

| | Result |
|---|---|
| Generation, 135 M model | katana Vulkan ~2x the reference Vulkan; tie with CUDA |
| Generation, 1.5 B and 3 B | reference ahead: ~13–15% (Vulkan), ~20% (CUDA) |
| Prompt, 135 M model | tie with the reference Vulkan |
| Prompt, 1.5 B and 3 B | reference ahead: ~1.7–1.8x (Vulkan), ~2.1–2.2x (CUDA) |

## 2. Dense models: how it was before the Vulkan backend (64+64 benchmark)

| Model | | katana OpenCL | reference CUDA | reference Vulkan | distance on the prompt |
|---|---|---|---|---|---|
| SmolLM2 135M | generation | 127 | 402 | 224 | |
| | prompt | 1,222 | 6,708 | 8,638 | 5–7x |
| Qwen2.5 1.5B | generation | 96 | 117 | 90 | |
| | prompt | 212 | 2,776 | 2,343 | 11–13x |
| Llama 3.2 3B | generation | 57 | 69 | 63 | |
| | prompt | 108 | 1,685 | 1,375 | 13–16x |

Note: the reference Vulkan generation on Qwen 1.5B was 90 in this measurement and 113–115 in the later
alternating measurement. The later one holds; the difference shows how misleading an isolated measurement is.

## 3. katana prompt evolution (Qwen2.5 1.5B, 64+64 benchmark)

| Version | Prompt |
|---|---|
| OpenCL, batch of 16 (float) | 212 |
| Vulkan, one token at a time (step 4) | 121 |
| Vulkan, batch with tensor cores (step 5) | 1,349–1,364 |
| Vulkan, 512-token prompt (agent measurement) | 1,544–1,563 |
| reference Vulkan | 2,362–2,374 |

## 4. Qwen3-30B-A3B (mixture of experts, 18.6 GB on a 4 GB card)

Real text, 200 tokens generated, three alternating rounds, katana on OpenCL:

| | katana | reference with expert cache on the GPU | reference with experts on the CPU only |
|---|---|---|---|
| generation | 12.5–14.9 | 13.2–14.4 | 6.7–8.6 |
| short cold prompt | 9–11 | ~5.5 | ~5.5 |

Synthetic 64+64 benchmark (reference with experts on the CPU only; katana before and after the batch):

| | katana before | katana after | reference |
|---|---|---|---|
| prompt | ~22 | 28–33 | 26.7 ± 8.5 |
| generation | 16–17 | 15–18 | 14.0 |

Other reference configurations on the same model: CPU only 5.2 generation; 9 layers on the GPU 7.7.

Comparison history: we first measured against the reference without an expert cache and
concluded "1.6x in our favor on generation". Then we discovered that it has its own cache
(`--moe-cache-mib`, `--moe-cache-cpu-share`); with it turned on, generation is tied.

Output check: katana's greedy text matches the reference's for ~60 tokens (one test).

## 5. Isolated batched product (trillions of multiplications per second)

| Version | batch 64 | batch 256 |
|---|---|---|
| katana OpenCL (any batch) | ~0.32 | ~0.32 |
| Naive Vulkan (one tile per subgroup) | 0.51 | — |
| + tile in shared memory, 4 accumulators | 1.17 | 1.36 |
| + loads of 4 values, 64x64x64 tile (fp16 weights) | 1.88 | 2.4–2.6 |
| Q4_K weights in the katana layout | 1.96–2.50 | 2.65–2.83 |
| Q6_K weights in the katana layout | 2.17–3.27 | 3.23–3.34 |
| reference Vulkan (estimated from the prompt) | ~3.5 | — |

Batch of 16 on Q4_K: 0.57 padding up to 64; 1.40 with a narrow tile.

## 6. Cost per kernel dispatch

| | µs per dispatch |
|---|---|
| OpenCL, enqueueing one kernel (host) | 22–25 |
| Vulkan waiting on every dispatch | ~156 |
| Vulkan recording the queue live | ~4.7 |
| Vulkan with a recorded and resubmitted graph | 1.5–2.4 |

Effect on the engine: 135 M model, generation 119–139 (OpenCL) → 385–409 (recorded Vulkan). On
models of 1 B or more it changes nothing, because the limit is the computation on the GPU.

## 7. katana evolution on mixture of experts (OpenCL)

| Change | Before | After |
|---|---|---|
| OLMoE: one kernel for the token's experts | 20 | ~88 |
| OLMoE CPU only: experts in one dispatch | 20 | 31 |
| OLMoE with 25% of the experts in VRAM: cold ones run on the CPU | 16 | 35 |
| OLMoE, batched prompt | ~90 | 110–155 |
| OLMoE, prompt, norm inside the kernel | ~145 | ~174 |
| Qwen3-30B, real text: page warm-up and less agitated swapping | 8.4 | 13.9–14.4 |

OLMoE with the VRAM cache deliberately reduced:

| Experts in VRAM per layer | Uses served from VRAM | Generation |
|---|---|---|
| 51 of 64 (default) | 95% | 55–90 |
| 32 of 64 | 83% | 42 |
| 16 of 64 | 62% | 35 |
| 8 of 64 | 45% | 32 |
| CPU only | — | 20–31 |

## 8. The idea "only what matters to the topic goes into the GPU"

Dense model (Qwen 1.5B, `katana sparsity`): predictions that stay the same when keeping only part of the
feed-forward neurons.

| Neurons kept | same topic | other topic | at random | ideal per-token choice |
|---|---|---|---|---|
| 10% | 6% | 4% | 0% | 49% |
| 30% | 30% | 17% | 1% | 84% |
| 50% | 55% | 38% | 8% | 94% |
| 70% | 72% | 49% | 21% | — |

MoE model (`katana routing`): uses served with part of the experts in VRAM.

| In VRAM | OLMoE: ceiling | OLMoE: calibrated on the same topic | OLMoE: other topic | Qwen3-30B: ceiling | Qwen3-30B: same topic | chance |
|---|---|---|---|---|---|---|
| 5% | 28% | 28% | 5% | 34% | 31% | 5% |
| 10% | 40% | 39% | 13% | 52% | 47% | 10% |
| 15% | 49% | 48% | 21% | 65% | 60% | 15% |
| 25% | 64% | 63% | 34% | 83% | 79% | 25% |
| 35% | 76% | 74% | 46% | 93% | 89% | 35% |
| 50% | 89% | 87% | 59% | 99% | 96% | 50% |

Qwen3-30B with equal slots per layer: ceiling of 64.4% with 15% in VRAM, against 65.1% with free slots.

## 9. The engine's first numbers (OpenCL, start of the day, without alternating)

| Model | Situation | Prompt | Generation |
|---|---|---|---|
| Qwen2.5 1.5B | 28/28 layers on the GPU | ~212 | ~94 |
| TinyLlama 1.1B | GPU | ~287 | ~115 |
| SmolLM2 135M | GPU | ~1,150 | ~105 |
| Llama 3.2 3B | 28/28 on the GPU | 107 | 55 |
| Qwen2.5 7B | 26/28 on the GPU | ~32 | ~15 |
| Mistral 7B Q5_K_M | 23/32 on the GPU | 12 | 8.5 |
| Qwen2.5 14B (9 GB) | 21/48 on the GPU | 4.5 | 3.3–3.6 |
| Qwen2.5 1.5B | CPU only, 12 threads | 26 | ~22 |

## 10. What was tested and gave no gain

| Attempt | Result |
|---|---|
| 8 threads instead of 12 | ~20% worse |
| Pinning threads to cores; turning off Windows power throttling | no effect |
| Fast math in OpenCL; busy-wait on read; queue dispatch per layer | no effect |
| One-token attention fused into a single launch (OpenCL) | correct, no gain above 1 B |
| Using 97% of VRAM instead of 92% | generation drops from 13.5 to 7.9 |
| Uneven VRAM slots per layer | 0.7 points below the ceiling |
| Converting only experts used twice | got worse (50 against 60) |
| 128x128 batch tile with 16 accumulators | 0.4–0.7 T/s against 2.5 |
| Half-precision accumulator in the batch | no gain and a 2–6% error |
| Q4_K scale table in shared memory | slower (2.54 against 2.79 T/s) |
| Reading fp32 activations directly in the batch | 2–6% slower than converting once |

## 11. Linux (Ubuntu, NVIDIA driver 595.99, 2026-11-15)

Vulkan, previous binary against the new one (tiles with at most 2 accumulators per subgroup),
alternating, round 3, tok/s:

| Model, benchmark | | previous | new |
|---|---|---|---|
| Qwen2.5 1.5B, 512+128 | prompt | 851–854 | 1,512–1,518 |
| | generation | 80.5–81.0 | 80.8–81.1 |
| Llama 3.2 3B, 512+128 | prompt | 411–412 | 749–752 |
| | generation | 47.2–47.4 | 47.2–47.4 |
| Qwen2.5 1.5B, 64+64 | prompt | 900–903 | 1,561–1,562 |
| | generation | 98.0–98.8 | 98.4–99.0 |

In the same hour, OpenCL on Qwen2.5 1.5B 64+64: prompt 211–212, generation 100.7–101.5.
The "Vulkan slower on generation" seen at the start (86–88 tok/s) was thermal drift: measured again,
it is 2–3% below OpenCL, as on Windows. Fence wait in a loop (`KVK_SPIN`, default on):
generation +2–3% in a 512+256 test (13.1 → 12.8 ms/token), within what drift also explains.

## 12. Batched MoE prompt on Vulkan (Linux, 2026-11-16)

`bench`, previous binary (Vulkan token by token on the MoE prompt), new and OpenCL, alternating in the same hour, two series,
rounds 2 and 3, tok/s. Wait until the GPU is at 72 °C or less before each run.

| Model, benchmark | | previous | new | OpenCL |
|---|---|---|---|---|
| Qwen3-30B-A3B, 256+16 (`KATANA_KV16=0 KATANA_CTX=1024`) | prompt | 48.1 / 48.5 / 48.7 / 49.0 | 140.7 / 144.2 / 145.5 / 149.4 | 57.6 / 57.9 / 57.9 / 58.1 |
| | generation | 26.4–26.7 | 23.5–41.8 | 34.3–34.8 |
| Qwen3-30B-A3B, 64+64 | prompt | 44.6–45.2 | 66.4–68.1 | 60.3–60.6 |
| | generation | 41.8–42.6 | 42.3–42.9 | 41.6–41.8 |
| OLMoE 1B-7B, 256+16 (`KATANA_KV16=0`; Vulkan with `KATANA_VK_MOE=1`) | prompt | 139.8–141.3 | 362.5–375.5 | 209.0–212.0 |
| | generation | 121.6–122.4 | 118.4–122.5 | 126.6–127.0 |

Sweep of the temporary slots (Qwen3-30B 256+4, not alternating, GPU heating from 81 to 85 °C over the series: only
indicative): 16 slots 144–146, 32 slots 113–121, 48 slots 97–108 tok/s.


## gemma4 26B-A4B: batched prompt on Vulkan (2026-11-17, worktree agent-abec)

`katana bench <gemma4> 128 32`, RTX 3050 4 GB, alternating in the same hour (two rounds of each; rounds 2 and 3 of the
bench). Before = `KATANA_VK_G4=0` (batch on the CPU only), after = hybrid batch (dense on the GPU, MoE on the CPU). Measured:

| case | prompt tok/s | generation tok/s |
|---|---|---|
| before, r1 | 4.8 / 3.2 | 7.7 / 7.8 |
| after, r1 | 3.4 / 3.2 | 8.2 / 8.0 |
| before, r2 | 3.3 / 3.3 | 7.9 / 7.9 |
| after, r2 | 3.3 / 3.3 | 7.9 / 6.6 |

No difference beyond noise. An earlier series with 256+32 (one round of each, sequential, not alternating:
only indicative) gave prompt 4.2 (before) x 5.7 (after, MoE on the CPU) x 6.1 tok/s (MoE in the slots, inexact).
Generation does not change (the batch only affects the prompt).

## 13. qwen35moe on Vulkan with ctx 131072: full attention on the CPU (Linux, 2026-11-17)

bench 512+32, Qwen3.6-35B-A3B Q4_K_M, `KATANA_CTX=131072`, main binary and new one alternating in the same hour
(GPU free at the start of each run; CPU with other agents).

| | main (hybrid mode) | new (graph + attention on the CPU) |
|---|---|---|
| prompt (rounds 2-3) | 18.8-18.9 tok/s | 83.6-85.0 tok/s |
| generation (rounds 2-3) | 17.3 tok/s | 27.0-27.3 tok/s |

The first series of main gave 3.7-5.3 / 3.1-17.8 (cold). DeltaNet in blocks x exact: inconclusive (high load), see handoff 0d.

## 14. MoE router on the GPU in generation (Linux, 2026-11-18, worktree agent-a516)

Generation profile (`KATANA_VK_GENPROF=1`, bench 16+128, 3 rounds, ctx 4096, GPU free, load 4-9 on the shared
machine: indicative). Averages per MoE layer, generation only:

| | Qwen3.6-35B-A3B (40 layers, Vulkan graph) | gemma4 26B-A4B (30 layers, hybrid mode) |
|---|---|---|
| slots per layer | 17 of 256 | 9 of 128 |
| expert uses in VRAM | 36% | 74% |
| layers with all 8 chosen in the slots | **0.3%** | 25% |
| blocking read per layer (includes the GPU computation) | 0.43 ms | (none: layers on the CPU) |
| router on the CPU | 0.053 ms (~2.1 ms/token) | 0.038 ms (~1.1 ms/token) |
| cold on the CPU | 0.27-0.35 ms (~11-14 ms/token) | 0.64 ms (~19 ms/token) |
| generation | ~34 ms/token | ~46-48 ms/token |

One-token router on the GPU in qwen35moe (`KATANA_VK_GPUROUTER=1`), alternating 0/1 in the same hour, 3 series,
rounds 2-3 (ms/token):

| series | off | on |
|---|---|---|
| 1 | 33.56 / 33.80 | 33.22 / 33.36 |
| 2 | 34.27 / 34.27 | 33.50 / 33.76 |
| 3 | 34.68 / 34.84 | 34.65 / 34.55 |
| average | 34.24 | 33.84 (-1.2%, within noise) |

The router leaves the CPU (0.053 -> 0.002 ms) but the read grows 0.03 ms (one more dispatch and 1 KB more).
Greedy generation of 80 tokens identical. Stays off by default.

## 15. MoE experts on the CPU, one token (Linux, 2026-11-18, worktree agent-a5fc)

Microbenchmark `tools/moebench` (no GPU): 8 experts drawn per layer, all MoE layers, as in a
token with all experts cold. `KATANA_MOE_OLD=1` = previous path. Alternating OLD/NEW in the same hour,
with another katana process using 8 threads (load 11-18): **indicative**. Read bandwidth measured by the
microbenchmark itself: 33-37 GB/s (8 threads), 30-40 GB/s (16).

| Model | bytes/token | ceiling (35 GB/s) | previous ms/token | new ms/token |
|---|---|---|---|---|
| Gemma 4 26B (down Q5_1) | 897 MB | ~25 ms | 214 / 208 | 34.4 / 37.8 |
| Qwen3.6-35B (down Q5_K) | 611 MB | ~17 ms | 42.4* / 23.1 / 24.2 | 22.7 / 21.5 / 21.2 |

\* run with a load spike. Phases on Qwen3.6 (new): gate/up ~12.3 ms, activation+quantization 0.3 (before 1.2),
down ~8.8 (before ~10). Threads (previous, Qwen3.6): 8 = 31-32 ms, 12 = 48-50, 16 = 49-50.

CPU-only generation (`KATANA_CPU=1 katana chat ... 64 0`), Gemma 4: 2.8 -> 6.3 tok/s; Qwen3.6: 7.5 -> 7.6 tok/s.
Generated text identical in both. Full model with GPU: not measured (command in handoff 0i).

## 16. Expert slots: policies and swap margin (Linux, 2026-11-18, branch worktree-agent-a518)

Offline simulation (measured on real routing records, `KATANA_EXPLOG`; hits only in generation, 3 agent
prompts). Qwen3.6 current policy by number of slots (conversation / code / tool):

| slots | 8 | 12 | 17 | 18 | 20 | 23 | 34 |
|---|---|---|---|---|---|---|---|
| Qwen3.6 margin 4 | 30.9/25.4/22.5 | 39.2/32.3/29.1 | 47.1/39.2/35.9 | 48.4/40.3/37.1 | 50.7/42.5/39.4 | 54.0/45.6/42.4 | 63.1/54.3/51.3 |

| slots | 9 | 10 | 12 | 18 |
|---|---|---|---|---|
| gemma4 margin 4 | 51.0/47.2/46.6 | 53.7/49.8/49.1 | 58.8/54.1/53.6 | 70.4/64.2/63.9 |

Policy comparison: table in `handoff.md` section 0j.

Measured on the GPU, alternating, loaded machine (load 5-8, another agent on CPU microbenchmarks): indicative.

| Qwen3.6 `bench 256 128` (ms/token, rounds 2 and 3 of each run) | margin 4 | margin 8 | margin 16 |
|---|---|---|---|
| run 1 | 37.1 / 36.9 | 34.9 / 34.9 | 34.8 / 35.1 |
| run 2 | 37.6 / 37.9 | 36.5 / 36.1 | 35.8 / 35.6 |
| run 3 | 38.3 / 39.6 | 35.9 / 35.9 | 35.8 / 36.0 |
| uploads / hits (with the prompt) | 3950 / 22.5% | 2130 / 27.4% | 1030 / 27.3% |

| Qwen3.6 `chat` 128 generated (tok/s, 2 rounds) | margin 4 | margin 8 |
|---|---|---|
| conversation (1067 of prompt) | 26.1 / 26.4 | 28.3 / 28.3 |
| code (1891) | 24.9 / 25.2 | 26.9 / 26.8 |
| tool (2154) | 24.2 / 24.4 | 24.8 / 25.1 |

gemma4 `bench 64 128`: margin 4 66.1-76.3 ms, margin 8 68.4-73.4 ms (equal). Margin 1 on Qwen3.6 (`bench 16 128`):
36.1-39.2 ms against 33.3-36.9 with 4 (worse, despite +1.6 p.p. of hits). Command: `handoff.md` section 0j.

## 17. Final measurements with the machine free: GPU, MoE on the CPU and swap margin (Linux, 2026-11-18)

Machine free of other agents (load measured before each run: 3.7 to 8.6; the script's own load
explains the rise). Vulkan GPU, script `final.sh` (text in `handoff.md` section 0k). Each run has
rounds 2 and 3 (round 1 is warm-up); 3 alternating runs per condition, so 6 measurements per row. Range in
parentheses (minimum-maximum).

Selftest with GPU (batch, token by token and batch 16, against the exact CPU, 23 tokens): all OK. Gemma 4: products
relative error 5e-8, batch 0.0056 and token by token 0.0058 of logit difference, argmax equal. Qwen3.6: batch 0.00040,
token by token 0.00043, batch 16 0.00035. Qwen3-30B (`KATANA_KV16=0 KATANA_CTX=512`): batch 0.00044, token by token
0.00046, batch 16 0.00049. Argmax equal in all.

Gemma 4 26B-A4B, `bench 64 64`, old MoE (`KATANA_MOE_OLD=1`) against new (default):

| metric | old | new | ratio new/old |
|---|---|---|---|
| generation tok/s | 13.5 (12.7-14.3) | 23.7 (23.1-24.5) | 1.76x |
| generation ms/token | 74.3 (70.1-78.6) | 42.2 (40.9-43.2) | 0.57 (-43%) |
| prompt tok/s | 22.9 (22.2-24.3) | 60.7 (58.1-65.1) | 2.65x |

The 3 pairs give 1.72x, 1.77x and 1.79x on generation. Order within each pair fixed (old first), not
randomized.

Qwen3.6-35B-A3B, `bench 256 128`, swap margin 4 (old) against 8 (new default):

| metric | margin 4 | margin 8 | ratio 8/4 |
|---|---|---|---|
| generation tok/s | 24.2 (24.0-24.7) | 24.9 (24.4-25.3) | 1.03x |
| generation ms/token | 41.2 (40.5-41.6) | 40.2 (39.6-40.9) | 0.97 (-2.6%) |
| prompt tok/s | 59.6 (55.9-61.9) | 68.0 (65.0-69.8) | 1.14x |

Margin 8 was faster than 4 in all 3 pairs (+4.0%, +2.9% and +2.1% on generation), but the gain on generation is small:
about 3%, smaller than the 4-8% estimated earlier (section 16, handoff 0j). The generation ranges touch (24.4-24.7),
so the gain on generation is a hint, not proof. The prompt gain (+14%, ranges without overlap) is the clearest.

Measured and estimate: everything above is measured (katana bench, rounds 2 and 3, free machine, 3 alternating runs).
There is no estimate in this section; the 1.76x ratio and the ms/token reading come straight from the rounds. Not measured: real
conversation (`chat`), long prompts, other models and the comparison of old MoE against new with the CPU alone (already in
15).


## 18. Expert tier: RAM budget and SSD against plain mmap (Linux, 2026-11-22, branch worktree-agent-af15cd690dba92f7d)

Qwen3.6-35B-A3B-UD-Q4_K_M on the CPU path (`KATANA_CPU=1`), prompt of 30 tokens, 96 tokens generated greedily
(`katana run`). Machine shared (load 9-37 from other jobs), measurement lock held; each cell is one run, noise about
+-0.4 tok/s. The tier and the cgroup-limited mmap read a copy of the model that was not in the page cache (the copy lives
outside `models/` and was deleted afterwards); the tier uses O_DIRECT. The mmap limit is the expert budget plus 3 GB for
the other weights and buffers.

| case | prompt tok/s | generation tok/s |
|---|---|---|
| tier 4 GB (2,092 slots, 75% of expert requests in RAM) | 4.8 | 5.8 |
| tier 8 GB (4,185 slots, 86.5%) | 6.6 | 8.3 |
| tier 12 GB (6,278 slots, 87.6%) | 6.6 | 8.9 |
| mmap, cgroup MemoryMax 7 GB | 5.0 | 4.4 |
| mmap, cgroup MemoryMax 11 GB | 4.1 | 6.0 |
| mmap, cgroup MemoryMax 15 GB | 4.1 | 7.2 |
Ceiling for reference (not a tier case, the whole 22 GB file is in the page cache): plain mmap on the original file,
alternating with the 8 GB tier in the same hour, 2 rounds: generation 10.7 and 11.2 tok/s (prompt 16.2, 16.8) against
7.8 and 8.4 for the 8 GB tier (prompt 5.1, 6.8). So with 40% of the experts in RAM the tier keeps about 73-75% of the
generation speed of a fully resident model. The prompt phase is the weak point: a batch of 16 tokens touches most
experts of a layer, so prefill is bound by the disk (reading whole layers) and is 2.4-3x slower than resident.

Variants at 8 GB, 128 tokens (single runs, differences inside the noise): no prediction 7.8; early router 8 predicted/layer,
1 ahead 8.1; 12 predicted 8.1; 2 ahead 8.1-8.4; co-occurrence 7.8; previous token 7.9; LRU + early router 8.7. At 4 GB, 64
tokens: no prediction 4.9, early router 1 ahead 5.7, 2 ahead 5.4 (LRU 5.4). The prediction helps about 0.5-0.8 tok/s at 4 GB and is
hard to see at 8-12 GB, where few requests miss. At 4 GB the run is disk-bound (about 1.2 GB/s read, token stall 9-11 s of 17 s).
What did not help: slower frequency decay (worse), predicting 12 or 16 experts instead of 8 (more reads wasted, no speed
gain), co-occurrence and previous-token prediction (recall 49% and 40%).
Selftest (`katana selftest`, option off) on Qwen3.6 and Gemma 4: the CPU lines are identical to the binary built from the base
commit; the GPU-vs-exact lines move by a few 1e-4 between runs of the same binary because they depend on the free VRAM and
swap timing (another job used the GPU), the argmax agrees (OK) in all.
