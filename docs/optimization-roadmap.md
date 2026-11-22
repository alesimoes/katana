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
| 8-bit activations with scale per block of 256 and partial sums | twice the elements per instruction; the minimum term (Q4_K) and the offset (Q6_K) come out of the partial sums. Our old 8-bit test used one scale for the whole vector and changed the token; with a scale per block it needs to be redone | estimate 1.5–2x on what fits in cache | medium |
| 8 rows in parallel per step | one load of the activation multiplies 8 rows; 8 independent accumulators | estimate 10–30% in cache | medium |
| AVX-512 VNNI (`vpdpbusd`) with run-time selection | fuses three instructions and doubles the width | estimate up to 2x in cache | medium |
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

## 4. Expert cache (MoE)

We already have: slots per layer, heat with decay, cold expert on the CPU, background loading,
preload by the heat of the previous session.

| Technique | What it does | Effort |
|---|---|---|
| Frequency classes in eviction | class = log2 of the decayed use; one queue per class; the one with the greatest age relative to its class leaves. The accumulated use survives eviction | small |
| Large batch does not evict | the prompt only takes free slots, so as not to push out the generation's experts | done |
| Grouped uploads | neighboring misses take neighboring slots and are uploaded in a single copy | small |
| Slots shared between layers of the same format | we measured that equal slots per layer are already within 1 point of the ceiling; low priority | medium |
| Log of the choices for simulation outside the engine | `katana routing` already simulates; recording the trace of a real session is missing | small |


**Expert tier (RAM budget + SSD): done for the CPU expert path (2026-11-22).** `KATANA_EXPERT_RAM=<GB>`,
`src/engine/expert_tier.c`, measurements in knowledge.md 5c and comparisons.md 18. Next: contiguous per-expert sidecar file
(+60% disk throughput), use the arena for the GPU upload path and the batch path of the VRAM cache, early router
prediction also for Gemma 4, prefetch 2 layers ahead with a smaller N.

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

### 7.1 Prompt lookup decoding (`src/engine/lookup.c`, written 2026-11-22, speed NOT measured)

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
- **NOT checked:** any speed (no measurement at all); the Vulkan graph path (the tiny `--gpu` model falls to the
  hybrid mode, which crashes with `malloc(): corrupted top size` also on the base commit, unrelated to this work);
  Qwen3.6 itself; temperature > 0 statistics; the agent loop end to end; prefix cache on disk after a turn that used
  the draft (should be safe because the state is restored before the turn ends).

## 8. New architectures and formats

| Item | What it requires | Effort |
|---|---|---|
| Low-rank attention with compressed KV (DeepSeek-V3/Kimi family) | two-stage q, compressed K and V shared across heads, RoPE only on a slice, per-head decompression, sigmoid router with bias, shared expert, initial dense layers | large |
| Native Q2_K and Q3_K | on top of the Q4_K kernel | small to medium |
| Table formats (IQ) | do not write a direct product: unpack to bytes in a tile and use the generic 8-bit kernel (same base as the batches) | large |

## Suggested order

1. Cheap experiments: threads, power, busy-wait, fast math, block page request, warm-up.
2. Cache policy: frequency classes, batch does not evict, grouped uploads.
3. Router on the GPU.
4. 8-bit activations per block and AVX-512 VNNI on the CPU.
5. Tile decoded once per batch (CPU and GPU).
6. KV: used positions, prefix reuse, shifting.
7. Sampling and exact pre-tokenizer.
8. Low-rank attention (DeepSeek/Kimi).
