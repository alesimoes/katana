# katana optimization roadmap

Techniques known from inference engines that katana does not yet use (or only partly uses), with
what each one solves, the expected gain and the effort. Gains marked *estimate* were not
measured here; they only become fact after being measured with `compare.sh` or `katana bench`.

Measured situation (RTX 3050 4 GB, i7-11800H, Qwen3-30B-A3B Q4_K_M): generation tied with the reference
engine when it uses an expert cache on the GPU (12.5–14.9 versus 13.2–14.4 tok/s); short
cold prompt ~1.8x faster on katana.

## 0. Where we stand on dense models (measured on 2026-11-15, RTX 3050, benchmark 64+64)

| Model | | katana (OpenCL) | reference CUDA | reference Vulkan |
|---|---|---|---|---|
| SmolLM2 135M Q8 | generation | 127 tok/s | 402 | 224 |
| | prompt | 1,222 tok/s | 6,708 | 8,638 |
| Qwen2.5 1.5B Q4_K_M | generation | 96 tok/s | 117 | 90 |
| | prompt | 212 tok/s | 2,776 | 2,343 |
| Llama 3.2 3B Q4_K_M | generation | 57 tok/s | 69 | 63 |
| | prompt | 108 tok/s | 1,685 | 1,375 |

Reading: in generation for models of 1 B or more we are ~20% behind CUDA and equal to Vulkan; the API is not what separates us. On the tiny model the cost per launch weighs (3x behind CUDA). **The real hole is the prompt: 11 to 16 times slower**, against both reference backends. This is the item "decode the weight block once per batch" in section 2, which becomes priority number one.

Why the batch is slow on this card's OpenCL (measured): with a batch of 4 or 16 the prompt yields the same (~210 tok/s on Qwen 1.5B, ~0.32 T multiplications/s). The kernel does one floating-point multiplication per weight per token, and the NVIDIA OpenCL driver does not expose 8-bit integer product (`cl_khr_integer_dot_product`) or half precision. Vulkan on the same card exposes both and the cooperative matrices (tensor cores): fp16 x fp16 at 16x16x16 and int8 x int8 -> int32 at 16x16x32. That is what the reference backends use for the batch.

Prototype `tools/vkgemm` (8960x1536 product by a batch of 64, one 16x16 tile per subgroup, loads straight from global memory, 32-bit accumulator, checked against the CPU): 0.51–0.78 T multiplications/s. This is the naive form; what delivers real throughput is missing (larger tiles per workgroup, weights and activations going through shared memory, quantized weights decoded when filling the tile).

Second version of the prototype (`tools/vkgemm/gemm.comp`): a workgroup of 128 threads (4 subgroups) owning a 64x64 tile of the output; every 32 columns all threads fill shared memory with the slice of the weights and of the activations, and each subgroup loads its cooperative matrices from there and keeps 4 accumulators. Result checked against the CPU:

| Case | naive | with shared memory |
|---|---|---|
| 8960x1536, batch 64 | 0.51 T/s | 1.17 T/s |
| 8960x1536, batch 256 | — | 1.36 T/s |
| 1536x8960, batch 64 (few rows, many columns) | 0.78 T/s | 0.49 T/s |

A 128x128 tile with 16 accumulators came out worse (0.40–0.60 T/s): filling shared memory, value by value, becomes the bottleneck. Next steps for the prototype: vectorized loads (several values per read), splitting the columns among groups when there are few rows, decoding Q4_K inside the load with the block scales computed once, and measuring the half-precision accumulator. Reference to reach on the same card: ~3.5 T/s.

Third version: loads of 4 values per read and a step of 64 columns (tile 64x64x64). Checked against the CPU: 1.75 T/s with a batch of 64 and 2.4–2.6 T/s with a batch of 256, in both matrix formats. A 128 tile (16 accumulators per subgroup) is still worse (0.4–0.7 T/s). Still with half-precision weights; measuring with Q4_K in our VRAM layout, decoding on load, is still to be done.

## 1. Fewer launches and fewer waits per token (GPU)

The floor of ~9–11 ms per token comes from the fixed cost per kernel launch (~40 µs), not from the computation.

| Technique | What it does | Gain | Effort |
|---|---|---|---|
| MoE router on the GPU | logits, top-k and weights in a small kernel; the experts kernel reads the indices from a buffer. A token for which all experts are in VRAM needs no blocking read per layer | estimate: the biggest gain in MoE with an almost full cache (OLMoE) | medium |
| q, k and v in one launch for any mix of formats | today only when all three are K-quants; the matrix is chosen by the grid index | 2 launches per layer | medium |
| RMS norm inside the consumer | one kernel computes 1/rms; the products multiply by the scale when reading the activation | 2 launches per layer | medium |
| Residual sum and bias when writing the product | already partly done ("sum" mode) | small | small |
| RoPE writing straight into the KV cache | already done | — | — |
| Busy-wait on the token read | poll the event after `clFlush` instead of a blocking read (the thread wake-up may cost part of the floor) | to measure | small |
| Kernels with arguments already bound per layer | one kernel object per layer, no `SetKernelArg` per token | to measure | small |
| Fast-math compile options in OpenCL | `-cl-mad-enable -cl-fast-relaxed-math` | small | small |
| Kernels specialized by constants (`-D` for columns, words per row) | loops with bounds known at compile time | estimate 5–15% of kernel time | small |

Measured on 2026-11-15 (RTX 3050, NVIDIA OpenCL driver):

- each `clEnqueueNDRangeKernel` costs 22–25 µs on the host; `clSetKernelArg` costs ~0.08 µs. On a 28-layer dense model that is ~226 launches per token, ~5.6 ms of the 10.8 ms. That is the floor.
- busy-wait in place of the blocking read: no effect. Kernels with bound arguments would not help either (the cost is not in the arguments).
- `-cl-fast-relaxed-math`: no effect (stays optional, `KATANA_FASTMATH=1`).
- only two paths remain: fuse kernels (each launch less per layer is worth ~0.7 ms per token across 28 layers) or switch to an API with a command buffer recorded once and submitted per token (Vulkan), which eliminates the cost per launch. The second is a rewrite of the backend.

Correction after fusing the whole attention into one launch (one less per layer, checked on five models): generation did not change on any model of 1 B or more (Qwen 1.5B 10.9 ms, Llama 3B 18.7 ms, OLMoE ~10 ms per token, with and without the fusion). Conclusion: in those models the host finishes enqueuing in ~5.6 ms and waits for the GPU; the time per token is the GPU's work in the products (Qwen 1.5B reads ~0.85 GB of weights per token, ~78 GB/s), not the cost per launch. Fewer launches (or a recorded command buffer) only pays off on tiny models, such as the 135 M one. For the models that matter, what counts is the throughput of the product kernels and the part that runs on the CPU.

Command buffer prototype (`tools/vkprobe`, Vulkan, same card): 226 compute dispatches with a barrier between them, recorded once and submitted per token, take 0.44 ms in total (2 µs per dispatch); re-recording on every token, ~1 ms. On OpenCL just the enqueuing of those 226 costs ~5.6 ms. Flushing the OpenCL queue at each layer (`KATANA_FLUSH`) changes nothing, which confirms that the GPU already executes in parallel with the enqueuing. Therefore a Vulkan backend would remove ~5 ms of host time per token, which only shows up where the host is the limit: small models (the 135 M one spends ~7 ms per token almost entirely enqueuing). On models of 1 B or more the limit is the computation on the GPU and the expected gain is close to zero.

## 2. Batches (prompt)

| Technique | What it does | Gain | Effort |
|---|---|---|---|
| Decode the weight block once per batch | on GPU and CPU: unpack a tile of weights to bytes and apply it to all tokens of the batch, instead of decoding per token | estimate: several times on the prompt | large |
| Larger batches (128–512 tokens) | depends on the item above | together | large |
| Last layer only for the last token | the feed-forward of the last layer, the final norm and the projection only matter to the token that produces logits | small | small |
| Group tokens by expert in the MoE batch | each expert processes at once all the tokens routed to it | together with the first | large |

## 3. Products on the CPU

Token by token with cold weights we are at the memory bandwidth limit (~27–33 G weights/s); the gain
is in what fits in cache (small experts, batches).

| Technique | What it does | Gain | Effort |
|---|---|---|---|
| 8-bit activations with scale per block of 256 and partial sums | twice the elements per instruction; the minimum term (Q4_K) and the offset (Q6_K) come out of the partial sums. Our old 8-bit test used one scale for the whole vector and changed the token; with a scale per block it needs to be redone | implemented and validated numerically 2026-11-28 (3.3), not measured | done (not wired into the live MoE path yet, see 3.3) |
| 8 rows in parallel per step | one load of the activation multiplies 8 rows; 8 independent accumulators | implemented and validated numerically 2026-11-28 (3.3), not measured | done (not wired into the live MoE path yet, see 3.3) |
| AVX-512 VNNI (`vpdpbusd`) with run-time selection | fuses three instructions and doubles the width | implemented and validated numerically 2026-11-28 (3.3): on this machine (AVX-512 VNNI available) the 512-bit kernel is bit-identical to the 256-bit one, not measured for speed | done (not wired into the live MoE path yet, see 3.3) |
| Threads = physical cores, pinned, without Windows power throttling | less contention and less variation | measured: 8 threads are ~20% slower than 12 on this engine (with or without pinning); turning off power throttling changed nothing | done |
| Different number of threads for generation and for batch | generation is memory-limited; batch is compute-limited | to measure | small |
| Vectorized `exp` for SiLU and softmax | degree-5 polynomial | small | small |

### 3.1 Low-bit lookup formats on the CPU (`src/engine/cpu_lowbit.c`, measured 2026-11-22)

IQ2_XXS/XS/S, IQ3_XXS/S, IQ4_XS/NL and Q2_0 (GGUF type 42, 64 values per 18 bytes: f16 `d`, 2-bit codes
`(q-1)*d`, element `e` in byte `e/4` at shift `2*(e%4)`) used to be dequantized block by block and multiplied
in float. Now each 256-weight chunk is decoded to int16 (grid lookup, sign and the integer sub-block scale
folded in) and multiplied by the same 16-bit activations as Q4_K/Q6_K with `vpdpwssd`. Modes: 1 AVX2,
2 VNNI 256-bit, 3 AVX-512 (default when the CPU has it; `KATANA_LOWBIT=0|1|2|3`, 0 = old float path).
Batches decode a row once for up to 32 tokens, in blocks of 4 tokens per weight vector. Checked against the
dequantized double-precision product on random blocks and on the real IQ3_XXS Flash-Next tensors
(`katana lbtest [file.gguf] [bench]`): relative error 1e-6 to 4e-6 of the sum of magnitudes, the AVX-512
decoders are bit-identical to the AVX2 ones.

Measured on the i7-11800H (8 cores, shared machine, alternating paths in one run), real expert tensors,
G weights/s, 1 thread / 8 threads sweeping 200–400 MB (not in cache):

| type | float (before) | AVX-512 (now) | 32-token batch, 1 thread (G weights x tokens/s): per token / decode once |
|---|---|---|---|
| IQ2_XXS | 1.4 / 8.4 | 8.8 / 47.2 | 8.5 / 32.2 |
| IQ2_XS | 1.4 / 8.7 | 8.0 / 45.4 | 7.8 / 30.2 |
| IQ3_XXS | 0.25 / 2.0 | 7.6 / 52.4 | 7.7 / 29.6 |
| IQ3_S | 0.21 / 1.8 | 8.1 / 55.5 | 8.0 / 30.1 |
| IQ2_S | 0.20 / 1.8 | 8.7 / 54.1 | 8.6 / 30.8 |
| IQ4_NL | 7.5 / 38.9 | 7.5 / 35.3 (memory bound, no gain) | 8.2 / 28.1 |
| Q2_0 | 0.76 / 4.9 | 8.4 / 40.2 | 9.8 / 29.1 |

What limits it: decoding alone runs at 11–17 G weights/s (1 thread) while the integer product alone runs at
~48–52, so for single-token generation the table lookup/sign/scale work is what is left; IQ3_S, IQ3_XXS and
IQ4_NL already sit at the ~20–24 GB/s the memory gives on this machine. Repacking the experts into a
lookup-free integer layout would only pay if it were 8 bits per weight: ~121 G expert weights in 48 layers
= ~121 GB (+73 GB over the 47 GB file), not possible. A same-size repack (e.g. IQ2_S grids into plain 2-bit
codes) cannot be faster than the Q2_0 decode (15–17 G weights/s vs 12–13 now), a ~30% best case.
BF16 tensors are converted to float32 at load (twice the memory); `KATANA_BF16_RAW=1` keeps them in BF16
on the CPU (`lowbit_bf16_dot`, ~15 G weights/s on one thread in cache). The 8-thread figures vary by ~20% between
runs on this shared machine (IQ2_S 8-thread: 34–54 in three runs); the 1-thread ones by ~10%. Q6_K already had
the VNNI kernel (`dot_q6k_v`) and is unchanged.

### 3.2 Cold-expert path on the CPU (`moe_cpu_multi`, implemented 2026-11-26, NOT measured)

Goal: cut the ~11-14 ms/token that cold experts cost on the CPU in Qwen3.6-35B-A3B generation. Everything below
is on by default and each piece has an off switch. Nothing was measured on the real Qwen3.6 (only a correctness
check on synthetic Q4_K/Q6_K experts); the expected gain is unknown until measured.

| Switch | What it does |
|---|---|
| `KATANA_MOE_ROWS=0` | off: 4 weight rows per step (`dot_q4k_v4`, `dot_q6k_v4`, AVX-512 VNNI, runtime-selected; AVX2 machines keep the single-row path). The activation loads of a block are shared by 4 rows and the 4 accumulator chains are independent. Same integer arithmetic as `dot_q4k_v`/`dot_q6k_v`; only the float summation order differs. |
| `KATANA_MOE_PF=0` | off: software prefetch (`__builtin_prefetch`) of the next 4 rows (next row in the scalar tail) of the same expert matrix, inside each pool job. Rows of a chunk are contiguous, so each thread already reads one contiguous stream. |
| `KATANA_MOE_ACT=0` | off: vectorized SiLU*up (`silu_mul_v`, degree-6 polynomial exp, relative error ~3e-7 against `expf`). Not used for GELU (Gemma 4). |
| `KATANA_MOE_ALIGN=0` | off: pool slices rounded up to a multiple of 16 rows (64 bytes of float output), so two threads never write the same cache line of `hg/hu/outs`. |

The jobs (`moe_job`, `moe_gjob`, `moe_down_job`, `moe_gdown_job`) now walk segments of consecutive rows of one
matrix and call `moe_rows`. Not done: a gate+up fused pass (the two matrices are different weights, so the only
shared work is the already-quantized activation, which the 4-row kernel now amortizes), NUMA-aware placement (the
test machine is single-socket), and cross-job prefetch of the next expert pair (the pool gives no ordering
guarantee between jobs).

Check: `MOEB_SYNTH=1 tools/moebench/moebench x` (no model, no GPU) builds random Q4_K/Q6_K experts and compares
the new path against all switches off through `moe_cpu_multi` (1 token and a grouped batch), `moe_rows` with row
counts 1..9 at unaligned starts, and `silu_mul_v`. Result: relative difference of the outputs 6e-7 to 8e-6 (the
SiLU polynomial plus summation order), `silu_mul_v` 3e-7. `MOEB_CHECK=1 moebench <model.gguf>` does the same on
real expert tensors (written, not run). `katana lbtest` still passes.

### 3.3 8-bit activations, AVX-512 VNNI (512-bit) and 8 rows per step (`src/engine/cpu_kernels.c`, implemented 2026-11-28, NOT measured)

Builds on top of the existing VNNI path (`dot_q4k_v`/`dot_q6k_v`/`dot_q4k_v4`/`dot_q6k_v4`, which already used
`vpdpbusd` at 256-bit width with an EXACT 16-bit activation split into two signed 8-bit halves, `qh`/`ql`, two
`vpdpbusd` per 32-weight group — no precision loss, just two 8-bit instructions standing in for widen+madd).
The three items below are a genuine speed-for-precision trade, scoped to this cache-resident product path only
(not wired into the general CPU dense/attention path, see "Not done" below):

- **8-bit activations, scale per block of 256 (item 1).** `xq_prepare` already computed `qh = round(q/256)`
  as an exact half of the 16-bit activation; used *by itself* (without `ql`) it is already an 8-bit
  quantization of the activation with scale `xs*256` (per 256-column block, same granularity as the existing
  16-bit path — not a single whole-vector scale, which is what made the old 8-bit attempt change the chosen
  token, per `docs/knowledge.md` section 4). New partial sums `bs32_h`/`bs16_h` (sum of `qh` per 32/16, mirroring
  the existing `bs32`/`bs16`) feed the Q4_K minimum term and the Q6_K offset term. New kernels `dot_q4k_v8`/
  `dot_q6k_v8`: one `vpdpbusd` per 32-weight group instead of two (twice the elements per instruction, as the
  roadmap estimated). `KATANA_ACT8=0` would disable it once wired in (see below); the flag and the runtime
  detection (`act8_opt`) exist now.
- **AVX-512 VNNI at 512-bit width (item 3).** `dot_q4k_v8w`/`dot_q6k_v8w`: the same 8-bit activation, but the
  two 256-bit `vpdpbusd` calls of `dot_q4k_v8`/`dot_q6k_v8` (lo+hi for Q4_K, two 32-weight sub-groups for Q6_K)
  become a single 512-bit `vpdpbusd` (`_mm512_dpbusd_epi32`, `__m512i`/zmm) — doubling the width on top of item
  1. Detected at runtime (`cpu_vnni512`, `__builtin_cpu_supports("avx512f")`, `KATANA_VNNI512=0` disables it);
  this machine has AVX-512 VNNI (`avx512_vnni` in `/proc/cpuinfo`), so it was actually exercised, not just
  compiled: `v8w` is bit-identical to `v8` on every row tested (max difference 0.00e+00), confirming the
  512-bit regrouping does not change the integer arithmetic.
- **8 rows per step with the 8-bit activation (item 2).** `dot_q4k_r8`/`dot_q6k_r8`: like the existing
  `dot_q4k_v4`/`dot_q6k_v4` (4 rows, 16-bit exact) but 8 rows and 8 independent accumulators, built on the
  8-bit activation of item 1 so the shared activation load is a single `vpdpbusd` per row per group instead
  of two. `KATANA_MOE_ROWS8=0` would disable it once wired in.

**Validation (`katana qtest`, new `qtest_act8`).** Independent reference: `mat_row` (dequantizes the real
Q4_K/Q6_K block layout) dotted against the activation in double precision — the same pattern `qtest` already
uses for every other type, not derived from `dot_row`/`xq_prepare`/`cpu_exact`. Random Q4_K and Q6_K rows,
2048 columns:

| kernel | relative error vs double-precision reference |
|---|---|
| `dot_q4k_v8` (256-bit, 1 row) | 2.53e-4 |
| `dot_q4k_r8` (256-bit, 8 rows) | 2.53e-4 |
| `dot_q4k_v8w` (512-bit, 1 row) | 2.53e-4 (0.00e+00 vs `dot_q4k_v8`) |
| `dot_q6k_v8` (256-bit, 1 row) | 3.18e-4 |
| `dot_q6k_r8` (256-bit, 8 rows) | 3.18e-4 |
| `dot_q6k_v8w` (512-bit, 1 row) | 3.18e-4 (0.00e+00 vs `dot_q6k_v8`) |

This is ~25-30x worse than the existing exact 16-bit path (~1e-5) — expected, since 8 bits per element is a
real loss of resolution versus the lossless `qh`/`ql` split, even with a scale per block of 256 instead of
one for the whole vector. It is far better than the old whole-vector 8-bit attempt (which flipped the chosen
token); whether 2.5-3e-4 per dot product is safe to use token after token, 40+ layers deep, on the real model
is unmeasured and untested on Qwen3.6 generation (not just `qtest`/`moebench` synthetic blocks).
`MOEB_SYNTH=1 tools/moebench/moebench x` (section 3.2's check) still passes unchanged: nothing above touches
`moe_cpu_multi`, `moe_rows`, `dot_q4k_v4`/`dot_q6k_v4` or `silu_mul_v`.

**Not done — wiring into the live MoE path.** The three kernels above are new building blocks with the same
signature shape as `dot_q4k_v4`/`dot_q6k_v4` (`dot_q4k_r8`/`dot_q6k_r8`) or as `dot_q4k_v`/`dot_q6k_v`
(`dot_q4k_v8`/`dot_q4k_v8w`), ready to be selected from `moe_rows` in `src/engine/vk_dense.c` (which currently
calls `dot_q4k_v4`/`dot_q6k_v4` under `KATANA_MOE_ROWS`) and from `dot_row` in this file (which calls
`dot_q4k_v`/`dot_q6k_v`). Neither call site was touched in this session: `vk_dense.c` and `qwen35.c`'s `g4_*`
functions were locked for a concurrent session working on the Gemma 4 GPU path at the same time. Wiring them in
is a small change (a size check plus the three env switches, `KATANA_ACT8`/`KATANA_VNNI512`/`KATANA_MOE_ROWS8`,
following the `KATANA_MOE_ROWS`/`moe_opt_rows` pattern) but needs: (a) deciding whether the 8-bit activation's
~3e-4 error is acceptable for the MoE expert path specifically (not the general dense/attention path, where no
change was made), and (b) `vk_dense.c` being free to edit again.

## 4. Expert cache (MoE)

We already have: slots per layer, heat with decay, cold expert on the CPU, background loading,
preload by the heat of the previous session.

| Technique | What it does | Effort |
|---|---|---|
| Frequency classes in eviction | class = log2 of the decayed use; one queue per class; the one with the greatest age relative to its class leaves. The accumulated use survives eviction | done |
| Large batch does not evict | the prompt only takes free slots, so as not to push out the generation's experts | done |
| Grouped uploads | neighboring misses take neighboring slots and are uploaded in a single copy | done |
| Slots shared between layers of the same format | we measured that equal slots per layer are already within 1 point of the ceiling; low priority | medium |
| Log of the choices for simulation outside the engine | `katana routing` already simulates; recording the trace of a real session is missing | small |


**Expert tier (RAM budget + SSD): done for the CPU expert path (2026-11-22).** `KATANA_EXPERT_RAM=<GB>`,
`src/engine/expert_tier.c`, measurements in knowledge.md 5c and comparisons.md 18. Next: contiguous per-expert sidecar file
(+60% disk throughput), use the arena for the GPU upload path and the batch path of the VRAM cache, early router
prediction also for Gemma 4, prefetch 2 layers ahead with a smaller N.

**Frequency classes in eviction and grouped uploads: implemented, on by default, not measured (2026-11-28).**
`src/engine/expert_tier.c`. Both reuse `et.ghot`, the per-(layer, expert) decayed-use counter the heat-based mixed
quantization already tracks (independent of which slot holds the expert, so it survives eviction) instead of adding
a second tracker. Frequency classes (`KATANA_EXPERT_CLASS=0` turns off): `et_fill` used to reset a slot's heat to 0
whenever it took on a new expert, so a formerly hot expert came back from eviction "cold" and could be evicted again
before re-accumulating use; now `et_slot_take` seeds the slot's heat from `et.ghot`, so accumulated use survives the
round trip through eviction. `et_victim` buckets each candidate slot's heat into a class (`et_class`, the binary
exponent of the heat, i.e. a discretized log2) and evicts the coldest class first, oldest (`stamp`) within that class
breaking ties - the same outcome a queue-per-class design would give (the oldest member of the coldest non-empty
queue leaves first), computed by the scan `et_victim` already did instead of separate queue structures, since
`nslots` stays in the hundreds to low thousands. Grouped uploads (`KATANA_EXPERT_GROUP=0` turns off, package-only):
when a layer switch misses several experts at once, `et_fill_grouped` asks `et_bx_pick` (factored out of `et_fill` so
the single-request and grouped paths can never disagree on hi vs. cold) for each miss's file position, sorts them,
and merges neighbors (gap of at most one record) into a single `EtGJob`, up to `ET_GMAX` (8) experts per read; one
pool thread issues the one `pread` and splits it into each slot with `memcpy`. Isolated misses and the raw-GGUF path
(no `.exb`) are unaffected (still one `pread` per expert). Neither change can alter which expert the router picked
or the bytes that land in its slot - only residency order and read count. **Validated:** `katana bundle check` (hi
and `cold`) still reports byte-identical against the mapped GGUF / a fresh requantization. With `KATANA_THREADS=1`
(removes the thread pool's own floating-point reduction-order nondeterminism) a real greedy (temperature 0) generation
on Qwen3.6-35B-A3B-UD-Q4_K_M produced byte-identical output across `KATANA_EXPERT_CLASS`/`KATANA_EXPERT_GROUP` on/off
in every combination, and with `KATANA_EXPERT_POLICY=lru`; with the normal thread pool (no `KATANA_THREADS=1`), two
runs of the *same* settings already differ (pre-existing nondeterminism from parallel reduction order, unrelated to
this change). Nothing was measured for speed; `[expert_tier] leituras agrupadas: ...` at exit reports how many reads
each grouped `pread` served (observed 2.0-2.1 experts/read on a 4 GB arena with Qwen3.6, not a controlled benchmark).

**Cold-expert prefetch during the GPU wait: implemented, on by default, not measured (2026-11-26).**
`src/engine/expert_pf.c`, hooks `pf_before` / `pf_after` around `vk_moe_ffn` in the Qwen3.6 one-token Vulkan path
(`vk_qn_forward_1`). While the CPU blocks on the per-layer read (the GPU is running DeltaNet/attention of layer l+1), a
dedicated helper thread (not the compute pool) applies the early router of layer l+1 to the state the CPU already
holds (the FFN input of layer l; the residual is recovered up to scale by dividing by the layer norm weight, and RMS
norm is scale invariant), picks the N most likely experts that are NOT in the VRAM slots, and reads the gate, up and
down blocks of each one line by line (one load per 64 bytes plus software prefetch). This takes the page faults of
the mapped file and brings the lines to L2/L3 before the cold-expert products run. Volume per layer is capped at half
the L3 (sysconf, then sysfs, default 8 MB) and the thread stops within a page when the real layer arrives. It only
reads, so no numeric output changes. Controls: `KATANA_MOE_PREDICT=0` turns it off, `KATANA_MOE_PREDICT_N` (default 12),
`KATANA_MOE_PREDICT_MB`, and `KATANA_MOE_PREDICT_STATS=1` (or `KATANA_DEBUG`) prints at exit the jobs, cancelled jobs, MB read and
how many of the real cold experts were predicted (fully read / at least started). Nothing was measured: the recall of
this proxy input (FFN input of layer l instead of the residual after the MoE of layer l and the attention of layer
l+1), the share of the prediction that is read before the cancel (about 0.4 ms per layer, roughly 2 experts at
single-thread read speed) and the effect on the 11-14 ms/token of cold experts are all unknown. The helper competes
with the compute pool for memory bandwidth only if a job is still running when the cold products start (it is cancelled
first). Disabled when the RAM arena (`KATANA_EXPERT_RAM`) is on. Linux only.

## 5. Loading and startup

| Technique | What it does | Effort |
|---|---|---|
| Ask the system for the pages in a single call, with aligned and merged ranges | covers only the tensors that stay on the CPU; replaces the threads that touch page by page | small |
| Read straight into a fixed buffer what goes to the GPU | read without the system cache, one reused buffer, larger tensors first | medium |
| Warm-up | run two tokens after loading and clear the state | small |
| Memory calculator without allocating | weights per device + KV + scratch, to choose the cache size before allocating | small |
| VRAM margin | measured: above ~92% the card starts using shared memory and generation drops from 13.5 to 7.9 tok/s | done |

## 6. KV cache and context

| Technique | What it does | Effort |
|---|---|---|
| Attend only to the used positions (rounded to 256) | independent of the maximum context size | small |
| KV always in 16 bits | half the VRAM and bandwidth | small |
| Prefix reuse | keep the tokens of the previous conversation and keep the common prefix | small |
| Context shifting | discard the old part by re-rotating K by the position difference, without reprocessing | medium |
| Save and restore the KV of a fixed system prompt | instant agent startup | small |
| Attention in a single kernel with online softmax, reading K and V once per group of heads | we already have fused attention for a single token; the batch is missing | medium |

## 7. Sampling, tokenizer, formats

| Technique | What it does | Effort |
|---|---|---|
| Sampling chain | repetition penalty (ring of the last N tokens + count), top-k, top-p, min-p, temperature, in that order; min-p without sorting | small |
| Top-k by histogram | 128 logit bins find the bin of the k-th; only it is sorted | small |
| Lazy grammar | sample first and check only the chosen token; redo with the mask if it is rejected (useful for tool calls in JSON) | medium |
| Exact pre-tokenizer | state machine by type declared in the GGUF (`tokenizer.ggml.pre`): digit grouping (1 or 1–3), last space before the word, line breaks, contractions, Unicode letter and number classes; split the special tokens beforehand | medium |
| Chat format by detection in the template | we already do it by template fragments; Gemma, Phi, Mistral, DeepSeek, Kimi are missing | small |

### 7.1 Prompt lookup decoding (`src/engine/lookup.c`, written 2026-11-22, measured 2026-11-24: optimized the same day, 1.17x on whole-file reproduction and neutral elsewhere on the CPU, off by default)

Speculative decoding with a free draft taken from the context, aimed at code (an Edit repeats the old text,
unchanged lines, identifiers). Off by default.

- **Draft.** After each token, the last `n` tokens (`KATANA_LOOKUP_N`, default 3) are searched backwards in the
  token history (prompt + generated, recorded by a hook in `forward` and `forward_seq`); the up to `k` tokens
  (`KATANA_LOOKUP_K`, default 8, max 31, also capped by the batch size: 16 on the CPU) that followed the most
  recent earlier occurrence become the draft.
- **Verification.** One batched pass runs the current token plus the draft and keeps the hidden state of every
  position (`qn_layers_n` on the CPU; `vk_forward_n` on the Vulkan graph, where `vkb.x` keeps all rows). Logits are
  computed lazily, one position at a time (CPU: `cpu_head` on the stored row; Vulkan: gather of row r + head), so a
  rejected tail costs no vocabulary projection.
- **Acceptance rule.** `lk_forward(m, token, pos)` replaces `forward(m, token, pos, 1)` in the generation loops.
  The caller keeps sampling exactly as before (temperature, top-k, top-p, JSON/tool-call grammar) from the logits it
  gets; when the sampled token equals the draft token, the next logits are already there. With a deterministic
  draft this is exactly speculative sampling: the proposed x is accepted with probability p(x), and on rejection the
  emitted token is a sample of p conditioned on being different from x, i.e. the normalized residual p without x.
  Greedy output is the greedy output, up to the numeric difference between the batched and the one-token path.
- **Recurrent state (DeltaNet).** The KV cache fixes itself (positions are rewritten); the DeltaNet state does not.
  Before each fresh pass the state is copied (CPU: `m->ssm` memcpy, about 66 MB on Qwen3.6; Vulkan: device-side
  copy of the conv ring and state buffers into a second set of VRAM buffers through the new `kvk_buf_copy`, same
  size). On a mismatch the copy is restored and the next pass redoes the accepted tokens together with the new token
  and the next draft, in one pass. `lk_finish` (end of `generate` and of each console/agent turn, and automatically
  when anything calls `forward`/`forward_seq` while a pass is pending) restores and replays up to the last consumed
  token, so the state after a turn is the same as without the draft.
- **Adaptation** (`KATANA_LOOKUP_ADAPT=0` turns it off). The engine measures seconds per served token with and
  without the draft (moving averages, the first 4 tokens without draft). When the draft is slower it stays off for
  16 tokens, doubling on each failure up to 1024 (exponential back-off, then retried); `k` doubles when a whole draft
  is accepted and halves when less than half is. This also covers reasoning blocks with little repetition.
- **Report.** With `KATANA_LOOKUP=1`, a `[lookup: ...]` line on stderr at the end of each generation: drafted,
  accepted, verification passes, mismatches, tokens without draft, current k, ms/token with and without.
- **Backends.** Active: qwen35moe on the CPU batch path (`qn_batch`), and the full Vulkan graph of qwen35moe
  (`vkq.on`). Falls back to token by token on its own: dense models, Gemma 4 (sliding-window ring would need its own
  check), MLA, qwen4exp, the hybrid paths with GPU layers or GPU products (`gpu.n_gl`, `gpu.mv_host`), the Vulkan
  graph with attention on the CPU (`vkq.cpuatt`), images in the context, and if the VRAM copy cannot be allocated.
  Hooked loops: `katana run` (`generate`) and the console/agent (`gen_console`). The HTTP servers (`server.c`,
  `openai.c`) and `knowledge.c` keep their own loops and do not use it; the hook in `forward` only records history
  there. The older 3-token repetition draft of the dense Vulkan graph (`KATANA_SPEC`) is turned off when
  `KATANA_LOOKUP=1`.
- **MoE risk.** Verifying k+1 positions activates up to 8(k+1) distinct experts per layer; on the CPU and on the
  expert-cache paths this can cost close to k+1 times the expert reads of one token. That is why the feature is off
  by default and adaptive; whether it pays on Qwen3.6 is unknown.
- **Checked:** `katana lookuptest <model> [n]` (with `KATANA_LOOKUP_FORCED=1` the sequence follows the repeated
  prompt with a deviation every 11 tokens) on the tiny random qwen35moe of `tools/tinymodel`, CPU path: 69 drafted,
  38 accepted, 8 mismatches, same greedy choice at every position, max logit difference 2.9e-6, and after
  `lk_finish` the logits of an extra token match the plain run within 1.4e-6.
- **Measured 2026-11-24 (Qwen3.6-35B-A3B Q4_K_M, CPU batch path, `KATANA_CPU=1 KATANA_THINK=0`, temperature 0, GPU busy with
  an image job so the GPU graph was not tried; the machine was also loaded by other jobs, load average 10 to 22, so tokens/s
  below are noisy and only the alternated pairs and the counters mean anything).** Prompt: a 112-line C file (a ring buffer)
  plus an instruction; `tools/spectest.sh` compares the outputs.
  - Equivalence: 4 tasks (rewrite one function, rewrite three functions with a new enum, reproduce the whole file with an
    added function, write two new functions), adaptive on, plus one run with `KATANA_LOOKUP_ADAPT=0 KATANA_LOOKUP_K=4`:
    output byte-identical to the run without drafting in all 5 (300 to 500 generated tokens each).
  - Acceptance: adaptive, whole-file task 54 of 88 drafts accepted (61%); 3-function rewrite 21 of 60 (35%); new code 6 of 26.
    Forced k=4 on the whole-file task: 201 of 380 drafts (53%) in 95 verification passes.
  - Speed: no gain. Adaptive runs ended at 0.7x to 0.8x of the plain run on the copy-heavy tasks (the first few outputs were
    even noisier: 7.2 vs 11.7 tok/s on a 91-token answer, an artifact of the load changing between the two runs); forced k=4
    on the whole-file task: 8.5 vs 10.5 tok/s (0.81x) in the quieter pair. The adaptation behaves as designed: it stays
    without draft for most tokens (423 of 500 on the whole-file task) because a verification pass costs more per served token
    than a plain token. Cause: on the CPU a block of B tokens activates the union of the experts of its tokens, and the block
    path is only about 1.6x faster per token than the one-token path (prompt 19.8 tok/s against 12 tok/s generation at the
    first, quiet measurement), so a pass of 5 positions costs about 3 plain tokens and needs more than 3 accepted tokens to
    pay, while 35 to 60% acceptance gives about 2. Every mismatch also restores the 66 MB recurrent state and redoes the
    accepted tokens.
  - Draft search cost: the backward scan over 100,000 tokens takes at most 38 microseconds (measured in isolation) against
    80 to 250 ms per token, so a hash index was not added; it would only matter at contexts far beyond the model's.
  - Decision: stays off (`KATANA_LOOKUP=1` to enable). It would pay only with a verification pass that costs close to one token
    (dense model, or an expert-grouped block kernel) or on a path that is not bandwidth-bound; `KATANA_SPEC` is already the
    name of the specialization setting, so the switch keeps its name. The gate is still the engine's own timing, so turning it
    on for experiments cannot make generation much slower, only the first 4 tokens and the probes.
- **Optimization pass, 2026-11-24 (branch `feat-spec-optimize`).** Goal: make the draft pay on the CPU. What was changed and
  what each change was worth (same model, `KATANA_CPU=1 KATANA_THINK=0`, temperature 0; the machine was shared: 16 threads,
  load average 10 to 25 during every run, so only alternated pairs and ratios are used; GPU not used, `nvidia-smi` showed a
  desktop job on it).
  - *Cost of the batch pass, measured first* (`katana spectime <model> <file>`, new: alternates passes of B = 1, 2, 3, 5, 9
    positions that continue a copied context, median of rounds): one pass of B positions costs about 0.65 B + 0.35 plain
    tokens (B=2: 1.7 to 2.0x, B=3: 2.2x, B=5: 3.3x, B=9: 5.6 to 6.0x). Phase profile of a B=5 pass (`KATANA_SPEC_PROF=1`,
    `KATANA_SPEC_B=5`): dense projections of the attention/DeltaNet layers and the shared expert about 56%, routed experts
    about 24%, DeltaNet conv + recurrence about 12%, router 2%. So the marginal position is limited by compute in the dense
    integer dot products, not by reading expert weights; the original diagnosis (union of experts) explains only the 24%.
  - *Expert-grouped MoE block* (`moe_gjob`, `KATANA_MOE_GROUP=0` turns it off): pairs (token, expert) are sorted by expert;
    each expert row is read once and dotted with the inputs of all tokens that chose it. Alternated in the same process
    (`KATANA_SPEC_AB=1`): B=5 3.30x -> 3.10x of a token, B=9 6.00x -> 5.30x (B=2, 3: no difference, within noise). Kept.
  - *Cheap rollback of the recurrent state (CPU)*: during the pass the DeltaNet state is copied after each position
    (`qn_ckpt`, 66 MB per position, taken while the layer state is still in cache). A disagreement restores the state after the
    last correct position and the new pass starts right there; nothing is replayed and no copy is taken before the pass.
    Before, a disagreement restored the 66 MB snapshot and redid every accepted token in the next pass (about 0.65 token each).
    Forced draft (no policy), same alternated pairs: rewrite of 3 functions k=4 0.7-0.8x -> 0.98-1.0x, whole file k=4
    1.12 to 1.23x, k=8 1.16 to 1.33x. Memory: up to `KATANA_LOOKUP_K` x 66 MB of checkpoints (pages touched only if used).
  - *Draft policy* (`lk_pick`; `KATANA_LOOKUP_ADAPT=0` turns it off and drafts always): among all earlier occurrences of the
    last n tokens it takes the one with the longest backward match (ties: the most recent; up to 64 tokens), puts the match
    length in a bucket and keeps a moving per-token acceptance rate per bucket; for each draft length k it computes the expected
    served tokens 1 + p + ... + p^k against the measured cost of a pass of k+1 positions plus one logits row per served token
    plus part of a restore, and drafts only if the best ratio against a plain token is above 1.10, with that k. Without
    measurements yet it uses the 0.65 B + 0.2 model. The old "k doubles/halves" rule is gone; the safety back-off stays.
  - *Not done*: several candidate drafts or a draft tree (every extra position costs 0.65 token and the tree needs per-branch
    DeltaNet state, so a second candidate is worth it only if its extra acceptance exceeds 0.65 per position, which the measured
    rates (35 to 60% outside long copies) do not give); a shared-decode multi-token Q4_K kernel (the weight decode is about 25%
    of the work of one dot; sharing it across 4 tokens would save at most 12 to 15% of the dense phase while keeping the
    arithmetic bit-identical to the one-token path, so the pass would still cost about 0.55 B).
  - *Why it cannot pay much more on this CPU path*: a draft token served costs about 0.65 (position) + 0.15 (logits row) + 0.05
    (checkpoint) = 0.85 of a plain token even at 100% acceptance, so the ceiling is about 1/0.85 = 1.18x for long drafts and
    less for short ones; to reach 1.15x the draft must be accepted at 95% or more over 8 tokens. Only verbatim reproduction does.
  - *Final alternated measurement* (policy on, 3 repetitions of each task, plain/with-draft pairs run back to back, generation
    tok/s printed by the engine, load average 10 to 18 during the pairs; 120 to 300 generated tokens; outputs byte-identical to
    the run without draft in all 12 pairs; and in the 12 forced-k pairs):
    | task | plain tok/s | with draft tok/s | ratio (median) | draft use |
    |---|---|---|---|---|
    | rewrite 1 function (126-line C file in the context) | 7.9, 8.1, 8.0 | 8.0, 8.0, 8.1 | 1.01 | 2 of 3 drafts accepted, rest plain |
    | rewrite 3 functions | 8.1, 8.1, 8.1 | 7.9, 7.7, 7.7 | 0.95 | 2 of 7 accepted (the policy refuses most) |
    | reproduce the file with a new function (300 tokens) | 8.1, 7.8, 7.7 | 9.5, 9.2, 8.9 | 1.17 | 216 to 245 of 217 to 253 accepted, 34 to 46 passes |
    | new code, no copy | 8.0, 7.7, 8.1 | 8.2, 7.9, 8.5 | 1.03 (noise) | none (33 drafts refused) |
    Before the optimization (previous agent, adaptive): 0.7x to 0.9x on the copy tasks; with the policy but the old rollback the
    whole-file task was 1.12 to 1.16x and the 3-function rewrite 0.91x; the 3-function rewrite is still about 5% slower with
    the draft on (two disagreements, no gain), which is more than the measurement noise between the three pairs.
  - *Decision*: stays off by default. The 1.15x gate is met only by whole-file reproduction (1.17x); the rewrite tasks give
    1.01x and 0.95x, so turning it on would lose a little where the model copies only fragments. `KATANA_LOOKUP=1` is a clear
    win for "reproduce the file with a change" and neutral elsewhere; the policy keeps the loss on new code at 0 to 3%.
  - *Hybrid GPU path of the agent (4 GB card, `gpu.n_gl > 0`)*: still one token at a time. Not implemented and not measured: the
    GPU was in use, and the same arithmetic applies (a position added to the batch costs about 0.65 token on the CPU part, the
    experts are on the CPU or in the arena), so the ceiling is the same ~1.2x with extra complexity (checkpoint of the GPU-resident
    DeltaNet layers); worth revisiting only after the dense batch kernel gets faster.
- **NOT checked:** the hybrid GPU path of the agent on the 4 GB card (it falls back to one token at a time, so the
  option does nothing there), the full Vulkan graph (needs more VRAM than the card has), the Vulkan graph path (the tiny `--gpu` model falls to the
  hybrid mode, which crashes with `malloc(): corrupted top size` also on the base commit, unrelated to this work);
  Qwen3.6 itself; temperature > 0 statistics; the agent loop end to end; prefix cache on disk after a turn that used
  the draft (should be safe because the state is restored before the turn ends).

## 8. New architectures and formats

| Item | What it requires | Effort |
|---|---|---|
| Low-rank attention with compressed KV (DeepSeek-V3/Kimi family) | two-stage q, compressed K and V shared across heads, RoPE only on a slice, per-head decompression, sigmoid router with bias, shared expert, initial dense layers | large |
| Native Q2_K and Q3_K | on top of the Q4_K kernel | small to medium |
| Table formats (IQ) | do not write a direct product: unpack to bytes in a tile and use the generic 8-bit kernel (same base as the batches) | large |

## 9. Code retrieval without a model: context, symbol units, rerank (`src/coder/search.c`, 2026-11-23)

The search index holds one document per file, so a passage found alone loses where it comes from. Three deterministic steps add that context
(details and formulas in `docs/code-intelligence.md`, section 12.6a). Checked by hand on 15 queries (the greymod and katana trees; query list and
positions below), not a campaign. Nothing about a model's task result was measured. Times were taken with a training job running on the machine, so
they only show the order of magnitude.

| Step | Status | Result |
|---|---|---|
| 1. Structural context field (`CF_CTX`, weight 1.5): header comment, imports, defined names, per file, in the index (`STORE_FORMAT` 3) | done, on | **neutral** on the 15 queries when it is the only change (file positions moved by at most 1 in both directions). Costs about 10% more tokens indexed and 5% more postings. Kept on because it is not worse; it has no measured benefit yet. `-DCS_CTX_W=0` removes its effect on the scores. |
| 2. Symbol as unit (per-symbol BM25F with the symbol's own context: path, containing chain, neighbours, header, imports) | done, on | the right symbol is listed first or among the first for the cases below; typo query `listSesions` now returns `listSessions` (file 13 -> 2, symbol first); the unit context (1.5) is slightly better than without it (`-DCS_UCTX_W=0`: 5 of the 15 queries 1-2 positions worse without it, 1 better). |
| 3. Rerank of the top 30 within each tier: fusion position, best unit, symbol-name coverage (definition against use), path, whole identifier defined in the file, re-read check | done, on | most of the gain of the file positions (below). Worse on 2 queries by 1 and 2 positions. `-DCS_NO_RERANK` turns it off. |
| `semantic` channel for the embedder | extension point only (`g_cs_semantic`, `CH_SEM`), absent | no effect today |

Positions of the right file in `coder search` (limit 30; `-` = not in the 30), base binary -> new binary:

| Query | Right target | Before | After |
|---|---|---|---|
| how katana sends session info to the phone (greymod) | bridge/sessions.mjs | 13 | 10 |
| the same in Portuguese | bridge/sessions.mjs | 16 | 15 |
| terminalSessionIds (not in the current tree: the answer is "not found") | bridge/sessions.mjs | 12 | 10 |
| listSesions (typo) | bridge/sessions.mjs (`listSessions`) | 13 | 2 (symbol 1st) |
| where the repeat-call counter lives | src/agent/loop.c `rr_check` | 11 | 13 |
| how the read cursor remembers lines | src/tools/common.c `rc_*` | - | - |
| where the tokenizer splits identifiers into words | src/coder/text.c `coder_tokens_ex` | - | - |
| where secret files are skipped from the index | src/coder/ignore.c `ign_secret_name` | 5 | 6 |
| how a file is judged binary by its extension when indexing | src/coder/storedocs.c `st_binary_ext` | 4 (symbol: not listed) | 4 (symbol 4th) |
| function that merges the search lexicons when segments are compacted | src/coder/search.c `cs_merge_terms` | 5 | 1 |
| where the knowledge base concept files are loaded from disk | src/coder/kb.c `coder_kb_load` | - | - |
| how the Edit tool replaces text | src/tools/edit.c `ct_edit` | 1 | 1 (symbol 1st) |
| where the Find tool reads a file into memory | src/tools/find.c `fd_slurp` | - | - |
| how the web fetch tool limits repeated fetches of one url | src/tools/webfetch.c `ct_wf_at` | 17 | 14 |
| ordem dos tokens na gramatica de chamadas | not known | - | - |

The four misses that stay `-` are recall failures (the right file is not among the candidates of the six channels: the words of the question are not
in its text); reordering the top 30 cannot fix them. A model or a bigger concept base would. `mode=refs` (name and phrase) gave identical output before
and after. The Find (`tool Find`) text is unchanged in format; on 15 queries its output differed in 3 (candidate order or the "similar names" lines for
a name that does not exist; one of them, `listSesions`, now names the right file).

Cost: the CLI `coder search` reads the 30 candidates again: about +70 ms per query on the katana tree (98 -> 167 ms, process total, measured under load).
The Find does not read them again (symbol-table signals only): +19 ms, within noise. Index size on the katana tree: 14.0 MiB before and after (+0.05).

## Suggested order

1. Cheap experiments: threads, power, busy-wait, fast math, block page request, warm-up.
2. Cache policy: frequency classes, batch does not evict, grouped uploads.
3. Router on the GPU.
4. 8-bit activations per block and AVX-512 VNNI on the CPU.
5. Tile decoded once per batch (CPU and GPU).
6. KV: used positions, prefix reuse, shifting.
7. Sampling and exact pre-tokenizer.
8. Low-rank attention (DeepSeek/Kimi).
