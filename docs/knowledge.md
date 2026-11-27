# katana: what we know

Project knowledge document, for whoever (person or AI session) picks the work back up.
Written on 2026-11-15. Everything marked **measured** was measured on this machine; the rest
is estimate or code reading and is marked as such. Plans are in `plan.md`; the catalog of
techniques not yet used is in `optimization-roadmap.md`.

## 0. State in one paragraph (2026-11-23)

The working model is Qwen3.6-35B-A3B only. The engine runs it on the CPU, Vulkan and OpenCL paths with the expert tiers (GPU, RAM, SSD); the agent
sits on a lean tool set whose search and symbol tools read one code index (`docs/coder.md`: roots by `katana init`, a crawler that builds
levels N0 to N3 on demand, a session watch for freshness, ignored content shown with its reason). A pre-LLM layer (UIOE, `docs/uioe.md`, U0 to U5
and U8 built) prepares a context block before the model runs. The legacy code and the old training data were removed (the tool list is 12 tools); the
curriculum generator was regenerated for it and training runs only at night. As of commit `44748e1` (main). **The recent work (index, UIOE, tools) was compiled and not
measured**; every measured number in this file carries its date and belongs to the engine as it was then (sections 3 to 10 describe the
engine of 2026-11-15 to 2026-11-22, `katana.c` has since been split into `src/engine/`, `src/agent/`, `src/coder/`, `src/tools/`, `src/app/`).

## 1. Goal

AI infrastructure built from scratch, in low-level C, able to serve very large models
("trillion parameters") on small hardware, with the weights flowing from disk to RAM to GPU,
and in the future with the work split across machines (each one stores and solves a part).

The project owner's central idea: **only what matters to the topic of the conversation goes into
the GPU**. At the start it is slow; as the topic becomes clear, only the parts of the model tied to
it occupy VRAM. This turned out to be false for dense models and true for mixture-of-experts
models (section 4).

Project rules:

- Everything local, no third-party services. Nothing proprietary: use what is published and write our own.
- The code does not mention other inference engines. The word "llama" in the code is the name of a
  model architecture. Comparisons use "reference engine".
- Private repository at github.com/alesimoes/katana. Commits with push; do not make it public without being asked.
- Every speed claim needs alternating measurement (section 7).

## 2. Development machine

- i7-11800H (8 cores, 16 threads, AVX2 and AVX-512 with VNNI), 64 GB of RAM, Windows 11.
- NVIDIA RTX 3050 Laptop 4 GB + Intel UHD. The NVIDIA has a software power cap at
  60 W and reaches ~75–85 °C under load. There is no compute "lock".
- Disk: ~45 GB free (the models take up the rest).
- Compiler: MSVC BuildTools 2022 (`build.bat`). No OpenCL or Vulkan SDK: both
  libraries are loaded at run time.
- The machine usually has 60–75% of the CPU busy with other processes (WSL/Docker, antivirus).

## 3. The engine (as of 2026-11-15; the unit `src/katana.c` now only includes the parts in `src/`)

**Input.** GGUF through a mapped file. Architectures `llama`, `qwen2`, `qwen3`, `olmoe`,
`qwen3moe`. Tensors F32, F16, Q8_0, Q4_0, Q3_K, Q4_K, Q5_K, Q6_K. BPE tokenizers (with
per-family exact pre-tokenization, see section 9) and SentencePiece. Chat formats ChatML, Llama 3, Zephyr,
`[INST]` and plain text, chosen from the model's template.

**GPU (OpenCL loaded at run time).** Quantized weights stay in VRAM in a custom layout: rows
interleaved 32 by 32 in 32-bit words, so that a group of 32 threads reads neighboring words.
Kernel sets per batch size (16, 4, 1). Fused kernels in generation: q, k and
v in one launch; gate/up pair with SwiGLU; the whole attention of a token (bias, norm, RoPE, KV
cache, softmax and mix) in one launch. If the model does not fit, the first layers stay on the GPU and
the rest runs on the CPU. VRAM margin of 92%.

**CPU.** Integer products with 16-bit activations directly on the Q4_K/Q5_K/Q6_K/Q3_K blocks.
Own thread pool (12 threads, blocks handed out by an atomic counter). Exact floating-point path as the reference for
`selftest`.

**Mixture of experts (MoE).** Attention of all layers pinned in VRAM; the rest of the VRAM is
an expert cache with slots per layer. Each expert has a "heat" (recent use with
decay). Those in VRAM run together on the GPU, one launch per layer, including in
batches. Those outside run on the CPU on the spot (the token never waits for disk) and a loader thread
brings them in if they are hot. `pack` writes the experts already in the GPU layout. The heat is saved
between sessions and the hottest are preloaded.

**Usage.** `run`, `chat` (a single question or interactive multi-turn conversation), `agent` (conversation
with tools: read and list in the current folder; write file and run command only with
confirmation), `bench`, `selftest`, `micro`, `tune`, `pack`/`verify`, `sparsity`, `routing`.
`compare.sh` alternates katana and the reference engine.

## 4. What we learned about the central idea (measured)

**A dense model cannot be cut by topic** (`katana sparsity`, Qwen 1.5B). Keeping only the
50% most active feed-forward neurons on a text of the same topic, 45% of the predictions change.
Even choosing the best neurons for each token, 6% change. Activation is spread out: the 10% most
active account for 19% of it.

**An MoE model can** (`katana routing`). With 25% of the experts in VRAM:

| | OLMoE-1B-7B (64 per layer) | Qwen3-30B-A3B (128 per layer) |
|---|---|---|
| set calibrated on the same topic | 63% of uses | 79% |
| ceiling (best possible set) | 64% | 83% |
| calibrated on another topic (OLMoE) / chance | 34% / 25% | — / 25% |

On Qwen3-30B, 15% of the experts cover up to 65% of uses and 35% cover 93%: the curve is steep,
every extra percentage point of VRAM counts. Distributing slots unevenly across layers
does not help (ceiling 64.4% with equal slots versus 65.1%).

## 5. Measured facts about performance

**Basic costs on this machine**

| What | Value |
|---|---|
| Upload an expert (2.8–3.6 MB) to VRAM | ~0.75 ms |
| Read an expert from disk (in system cache) | ~1.75 ms |
| Convert an expert from GGUF to the GPU layout | ~4.6 ms |
| Run an expert on the CPU | 0.15–0.5 ms |
| First read of a mapped-file page (Windows) | ~2.5 µs per 4 KB page |
| One `clEnqueueNDRangeKernel` (host) | 22–25 µs |
| One `clSetKernelArg` | ~0.08 µs |
| One recorded and resubmitted Vulkan dispatch | ~2 µs |
| CPU, product with cold weights | 27–33 G weights/s (memory bandwidth limit) |

Consequences: the token must not wait for an expert load (running on the CPU is cheaper); the
cold start of an MoE is dominated by pages touched for the first time; above 92% of VRAM the
card uses shared memory and generation drops from 13.5 to 7.9 tok/s.

**Where the time goes**

- Generation, models of 1 B or more: the limit is the computation on the GPU (Qwen 1.5B reads ~0.85 GB of weights per
  token). The host finishes enqueuing in ~5.6 ms and waits. Removing one launch per layer did not
  change anything; flushing the queue at each layer did not either.
- Generation, tiny model (135 M): the limit is the cost per launch (~7 ms per token, almost all
  enqueuing).
- Batched prompt on OpenCL: batches of 4 and 16 yield the same (~0.32 T multiplications/s). The NVIDIA
  OpenCL driver offers neither 8-bit integer product nor half precision.
- MoE larger than VRAM (Qwen3-30B): almost half of the time per token is the experts on the CPU.
  Per layer the time is attention on the GPU + the larger of experts on the GPU and on the CPU, in series
  across layers.

**Things tested that gave no gain**: 8 threads instead of 12 (20% worse), pinning threads to
cores, turning off Windows power throttling, fast math in OpenCL, busy-wait on
the read, per-layer queue dispatch, fused attention (correct, but no gain above 1 B),
uneven slots per layer, converting only experts used twice (got worse), 8-bit activations
with one scale for the whole vector (changed the chosen token in the 7B model).

## 5a. Long context (measured 2026-11-17)

Qwen3.6-35B-A3B on the CPU: KV cache only in the 10 attention layers, fp16, 20 KB/token (21.5 GB at 1M). Generation at
depth: 128k 4.5 tok/s, 256k 2.7, 1M 0.7 (yarn x4). The attention of one token is limited by reading the
cache (flash-decoding with grouped heads gave 2.5x at 128k). Filling 1M of prompt: estimated ~8 days on the CPU
(attention prefill still token by token). Details in handoff.md, section 0a.

gemma4 (measured 2026-11-17): 25 local layers keep only the window (ring of 1040), 5 global ones the whole context;
at 131072 the KV is 2.90 GB fp16 (an old warning said 128.85 GB, it was only the arithmetic). The agent runs at
131072 by default, maximum RSS 15 GB (mapped weights), fp32 output identical to the previous one.

## 5b. Compacted context, conversation tokens and heat per session (2026-11-17)

- Agent compaction (summary by the model itself + shrunken tool results) and markers
  `\x01TOKENS`/`\x01COMPACT`: see handoff 0b. Measured on the 1.5B with context 2048: 1726->1430 and 1890->1420
  tokens; the floor is the agent's system prompt (~950 tokens).
- Expert heat per session (idea: store which experts each session used and preload them on the GPU
  when resuming/compacting, with a `\x01WARM` marker). Evaluated, not implemented. There is already heat per model
  (`.ktn.heat`, written on exit, read at startup, the S hottest per layer go to the loader thread
  before the first token). A per-session version only changes which heat vector is read. It only has an effect in
  hybrid mode with an expert cache, that is, when the MoE does not fit in VRAM (RTX 3050 4 GB here): OLMoE and
  the small MoEs fit entirely or run without this cache, so there is no way to measure a gain with them; only Qwen3-30B
  and Qwen3.6 exercise the path, and RAM was occupied by the other process. Without a measurement, no code was left.
  Estimate (not measured): the gain is limited to the start of the resumed session (the cache warms up by use within tens
  of tokens), and only exists if the session's topic differs from the global heat.


## 5c. Expert tiers: GPU, RAM and SSD (measured 2026-11-22, Qwen3.6-35B-A3B Q4_K_M, CPU path)

Goal: run MoE models larger than RAM. `src/engine/expert_tier.c` (off by default, `KATANA_EXPERT_RAM=<GB>`) gives the
CPU expert path an explicit RAM budget; experts are read from the file with parallel `pread` (O_DIRECT) instead of
page faults. Hooks: one line each in `moe_route`, `moe_cpu_multi`, `qn_layer` and `load_model`. Everything below was
measured on the 61 GB / RTX 3050 machine, shared with other jobs (load 9-37), with the measurement lock; speeds have a
noise of about +-0.4 tok/s. Qwen3.6 has 40 layers x 256 experts, K=8; one expert is 1.96 MB in the arena (three
matrices of about 0.59 MB each, far apart in the file).

**(a) Cache hit rate over a real text** (`katana tiersim run`, `katana tiersim sim`): 2,586 tokens in four segments with
a topic change (prose about the engine, C code, Portuguese project rules, a README), generation order, cold start.
Share of expert requests served from RAM:

| policy / scope | 10% | 15% | 25% | 50% | 75% |
|---|---|---|---|---|---|
| LRU, one set for all layers | 61.8 | 71.6 | 84.1 | 95.9 | 98.3 |
| LRU, per layer | 61.4 | 71.2 | 83.2 | 94.7 | 97.8 |
| decayed frequency g=0.95, one set | 60.5 | 73.5 | 85.0 | 96.0 | 98.3 |
| decayed frequency g=0.98, one set | 48.9 | 64.4 | 84.0 | 96.0 | 98.3 |
| decayed frequency g=0.995, one set | 23.7 | 36.8 | 59.4 | 92.1 | 98.3 |
| decayed frequency g=0.98, per layer | 61.2 | 71.1 | 83.8 | 94.9 | 97.9 |

One shared set beats equal slots per layer by 0.6-1.3 points. Decayed frequency is a tie with LRU (+1 point at 15-25%,
-1 point at 10%) and collapses when the decay is slow (g=0.995: new entries evict each other, old hot ones never leave).
Topic change costs little at 25%: hit rate in the first 100 tokens of each new segment is within 0-4 points of the whole
segment for LRU (81-88%); frequency with g=0.98 loses up to 6 points there. The text is short (2.6k tokens) and the topics
are all about this project, so real long sessions can be better or worse: not measured.

**(b) Predicting the next layer's experts** (recall of the 8 truly chosen experts, average over tokens and layers >= 1,
N predicted):

| predictor | N=8 | N=12 | N=16 |
|---|---|---|---|
| previous token, same layer | 40.4 | 48.8 | 54.6 |
| decayed frequency of the layer | 36.7 | 45.1 | 51.5 |
| co-occurrence layer l-1 -> l (online) | 48.8 | 59.8 | 67.2 |
| **early router: layer l's norm+router applied to the residual at layer l-1's router (1 layer ahead)** | **78.8** | **89.7** | **93.5** |
| same, 2 layers ahead | 70.0 | 81.0 | 85.8 |
| same, 3 layers ahead | 64.1 | 74.8 | 80.0 |
| early router on the output of layer l-1 (after its MoE; no lead time) | 82.0 | 92.3 | 95.4 |
| chance | 3.1 | 4.7 | 6.2 |

The early router is far better than history. Its cost is one rmsnorm and a 256x2048 product per layer (0.28 s over 96
tokens, 0.3%). Recall drops with distance because the residual stream keeps changing.

**(c) SSD random reads** (`katana iobench`, a 22 GB file copy on the NVMe, uncached, direct unless noted; threads = one
`pread` per thread, io_uring = raw syscalls, no library; GB/s by reads in flight):

| in flight | 0.59 MB direct, threads | io_uring | 1.77 MB direct, threads | io_uring | 0.59 MB buffered | 1.77 MB buffered |
|---|---|---|---|---|---|---|
| 1 | 0.69 | 0.70 | 1.26 | 1.23 | 0.32 | 0.50 |
| 4 | 1.20 | 1.20 | 2.03 | 1.96 | 0.96 | 1.24 |
| 16 | 1.30 | 1.27 | 2.17 | 2.03 | 1.43 | 1.90 |
| 64 | 1.32 | 1.31 | 2.07 | 2.02 | 1.02 | 1.32 |

io_uring works here without new libraries and is no faster than a thread pool (the device is the limit), so the engine
uses threads (16). The disk saturates at 8-16 reads in flight. Reading an expert as three separate 0.59 MB blocks gives
1.3 GB/s; one contiguous 1.77 MB block would give 2.1 GB/s (+60%): a sidecar file with each expert's three matrices
side by side is the next step for the SSD tier (not done).

**Tokens/s, CPU path, prompt of 30 tokens then 96 generated greedily (alternating runs are in comparisons.md 18)**

| experts in RAM | tier (this work) | plain mmap under a cgroup limit of budget + 3 GB |
|---|---|---|
| 4 GB (20% of experts) | 5.8 | 4.4 (7 GB limit) |
| 8 GB (40%) | 8.3 | 6.0 (11 GB limit) |
| 12 GB (60%) | 8.9 | 7.2 (15 GB limit) |
Ceiling for reference (not a tier case, the whole 22 GB file is in the page cache): plain mmap on the original file,
alternating with the 8 GB tier in the same hour, 2 rounds: generation 10.7 and 11.2 tok/s (prompt 16.2, 16.8) against
7.8 and 8.4 for the 8 GB tier (prompt 5.1, 6.8). So with 40% of the experts in RAM the tier keeps about 73-75% of the
generation speed of a fully resident model. The prompt phase is the weak point: a batch of 16 tokens touches most
experts of a layer, so prefill is bound by the disk (reading whole layers) and is 2.4-3x slower than resident.

The cgroup limit (`systemd-run --user --scope -p MemoryMax=...`) works on this machine. Both sides read a copy of the model
that was not in the page cache, so the first touches go to the SSD. Output text is bit-identical between the mapped path and
the arena (logits of the prompt compared with `cmp`; 96 generated tokens compared).

Not done / not measured: the GPU path (hot experts uploaded to VRAM still come from the mapping; only the CPU expert
path uses the arena, and the router hook does not run when the router is on the GPU), Gemma 4 (the early router uses
the residual convention of qwen35moe; the others fall back to previous-token prediction or none), long sessions, a
model that really does not fit in RAM (Qwen3.6 fits here, so the page cache can always cover the mapped path), a
contiguous per-expert sidecar.

**2-bit cold experts (estimate, nothing built).** With 20-40% of the experts in RAM, 10-25% of the expert uses are
misses. Keeping *all* experts in RAM at 2 bits (Q2_K is about 2.6 bits per weight: about 58% of Q4_K, so about 10.5 GB for
this model) and using the 4-bit copy only for the hot ones would turn every miss into a RAM read with no disk stall.
Quality is the open question. Estimate (not measured): if cold experts are used for 10-16% of the expert uses and a 2-bit
expert has the error of a whole-model 2-bit quantization (typically a few percent of perplexity), the effect on the whole model
should be a fraction of that, maybe 1-3% perplexity. The experiment that decides it: (1) quantize the expert tensors from
the original weights (not from Q4_K) to Q2_K and IQ2_XS; (2) make a model where each (layer, expert) outside the top X% by
heat of the sample text uses the 2-bit tensor (X = 25, 50, 75, 100%); (3) compare against the Q4_K file on the same
held-out text: perplexity, KL of the next-token distribution, top-1 agreement over at least 20k tokens, and the codebench
tasks. Decide by the KL/agreement as a function of the share of expert uses that land on 2-bit experts, not of the share of
experts. The engine can already run Q2_K/IQ2 dot products; it has no quantizer to make the files.

## 6. Where we stand relative to the reference engine (measured, same machine)

Reference engine: official binary b12418 with CUDA and with Vulkan.

**Qwen3-30B-A3B (18.6 GB in 4 GB of VRAM), real text**

| | katana | reference with expert cache | reference, experts on CPU only |
|---|---|---|---|
| generation | 12.5–14.9 | 13.2–14.4 | 6.7–8.6 |
| short cold prompt | 9–11 | ~5.5 | ~5.5 |

Honest reading:

- **We are equal or close** in generation: a tie on the large MoE, ~20% behind CUDA on dense models of
  1 B or more, equal to Vulkan.
- **We are ahead** only on the short cold prompt of the large MoE (~1.8x) and on startup (the token does not
  wait for disk, preload by heat).
- **We are far behind** on the prompt: 11 to 16 times on dense models. The cause is access to the tensor
  cores, which CUDA and Vulkan give and NVIDIA's OpenCL does not.
- The reference engine already has an expert cache on the GPU with the same idea as ours. The
  first comparison we made (1.6x in our favor) was against their configuration without a cache.

## 7. How to measure without fooling yourself

- The machine drifts: the same measurement varied from 15.5 to 11.5 tok/s over hours, on both
  engines. Only alternating comparison (A, B, A, B) at the same hour is valid: `compare.sh` or two binaries
  side by side (`ref/prev`).
- `bench` uses synthetic tokens and several rounds; `chat` with real text includes the cold start. The two
  numbers differ considerably and must be cited separately.
- Differences below ~10% are not conclusive with a single round.
- Before stating where a bottleneck is, measure the parts (`KATANA_DEBUG=1`, `KATANA_TIME=1`). The
  hypothesis "the floor is the cost per launch" seemed right and was wrong for models of 1 B or more.
- Check the competitor's configuration: their best may use a feature we do not know about.
- `selftest` compares the GPU with our exact CPU; an error common to both paths gets through. The
  external check done so far: Qwen3-30B's greedy output matches that of the reference
  engine for ~60 tokens (one test).
- `selftest <model> --deep [--tokens N] [--text file] [--only names] [--long N] [--full-long] [--long-cpu]
  [--save-ref f | --ref f] [--no-gpu]` covers what `selftest` does not: GGUF tensors nobody reads, every path against
  the float32 CPU on real text (KL, argmax flips, error per layer, router near-ties, dispatches and submits per call),
  state save/restore, going back in the cache, long context, non-finite values (bit test: `isnan` is dead under
  `-ffast-math`).
- `tools/tinymodel/check_graph.py ./katana` is the external check of the graph: a plain-Python forward of Qwen3.6 and
  Gemma 4 written from the published definitions, against the engine's logits on tiny models (no model download).
- `tools/tooltest/reap.sh` (commands die with the agent) and `tools/tooltest/pfx.sh` (prefix cache tied to the weights).

## 8. Strengths

- Small code, one file, no dependencies; quick to experiment with.
- Own layout in VRAM and own kernels, with weights always quantized.
- Expert cache by heat with the CPU handling the cold ones, background loading, on-disk format
  already in the GPU layout and topic memory across sessions.
- Own measurement tools: `sparsity`, `routing`, `micro`, `compare.sh`.
- Multi-turn conversation and agent with tools and confirmation.

## 9. Weaknesses

- Batched prompt on OpenCL 11–16x slower than the reference; the Vulkan backend (tensor cores) closes it to 1.7x on Qwen 1.5B and 1.8x on Llama 3B.
- Few architectures; missing the DeepSeek/Kimi family (low-rank attention), Gemma, Phi.
- Tokenizer exactness (audited 2026-11-26, `katana toktest <gguf>`, code compiled and the audit run on the vocabulary only, no weights):
  the byte-level pre-tokenizers are exact for the regexes of the official `tokenizer.json` of Qwen3.6 / Flash-Next (`qwen35`: NFC, `\p{N}` one digit,
  `\p{M}` counted as letter) and GLM-4.7-Flash (`glm4`: no normalizer, digits in groups of 3, `ignore_merges`); Unicode classes come from compact
  range tables (`src/unitab.h`, `src/nfc.h`), no regex engine. Fixed in this audit: (1) GLM/Llama-3 `ignore_merges` was ignored (a whole pre-token
  that is already a token must not go through the merges): 25 of 313 corpus files tokenized differently, a few tokens each (`KATANA_TOK_IGNMERGES=0`
  turns it off); (2) Gemma 4 `<eos>` is a plain token in the GGUF but an added (special) token in the official tokenizer: now special;
  (3) Gemma 4 BPE was quadratic per line (a very long line would be too slow; not timed), now the heap BPE (same ids on the whole corpus). Still not exact or
  not checked: the 7 Qwen audio/TTS control tokens (`<tts_pad>`, `<|audio_start|>`...) are special in the GGUF but not in the official
  `tokenizer.json` (text containing them is one token here); Unicode tables follow Python 3.14 (Unicode 16/17), the reference library may use another
  version; invalid UTF-8 and NUL bytes are not covered; the audit's hand-written
  golden cases are derived from the published regexes, not from running the official tokenizer (it could not be installed here), so agreement with the
  official ids is verified only by the earlier `tools/tokref` run (Qwen3.6, 1,898 cases) and not re-run for GLM and Gemma 4.
- SentencePiece (`llama`) path (2026-11-27, no `llama` model in `models/`): `spm_encode` was quadratic (it rescanned every neighbour pair at each merge);
  now a linked list of symbols plus a heap of candidate pairs (best score first, ties to the leftmost position, stale entries dropped lazily), O(n log n), same ids
  by construction. `katana toktest --spm-synth` builds a synthetic vocabulary (256 byte tokens, ~6,000 random pieces with many tied scores, specials) and
  compares the new code with the old one (kept as `spm_encode_ref`) on ~7,500 random texts (0 to 45,000 bytes, with and without the space prefix; accents, CJK, emoji,
  unknown characters going through byte fallback): 0 id differences, round trip 0 failures, specials recognized. Time on a 50,000-character line:
  0.011 s heap against 9.6 s old (~860x); 200,000 characters: 0.05 s heap (see handoff for the old one). Not checked: the real vocabularies of a `llama` model
  (real score distribution, `<unk>` handling, the dummy-prefix option), and agreement with the official SentencePiece ids, since no such model or library is here.
- Fixed context of 4,096 tokens, KV cache allocated in full, no prefix reuse or shifting.
- Sampling only with temperature and top-k.
- No quantizations below 3 bits (Q2_K, IQ family).
- CPU only with AVX2 (the machine has AVX-512 VNNI); 16-bit activations.
- Tested on one machine only; the Linux path compiles in theory and has never been run.
- No automated tests beyond the manual per-model `selftest`.
- No network between machines and no router between unstarted models.

## 10. Work in progress: Vulkan backend

Reason: it is the only way, on this card, to use the tensor cores in batches without depending on CUDA.

Measured with the prototypes in `tools/`:

| Batched product version | T multiplications/s |
|---|---|
| our OpenCL batch | ~0.32 |
| Vulkan, one tile per subgroup, loads from global memory | 0.51–0.78 |
| + 64x64 tile in shared memory, 4 accumulators per subgroup | 1.17–1.36 |
| + loads of 4 values, step of 64 columns | 1.88 (batch 64), 2.4–2.6 (batch 256) |
| Vulkan reference (estimated from the prompt) | ~3.5 |

With the real weights, quantized in the katana layout and decoded when filling the tile
(`tools/vkq4k`, all outputs checked against the CPU, card idle):

| Weights | 8960x1536, batch 64 | 8960x1536, batch 256 | 1536x8960, batch 256 |
|---|---|---|---|
| Q4_K | 1.96 T/s | 2.65–2.69 T/s | 2.34 (2.79 splitting the columns in 4) |
| Q6_K | 2.17 T/s | 3.34 T/s | 3.84 splitting the columns in 4 |

Decoding does not cost throughput: less memory is read. A small batch still wastes (a batch of 16 is
padded to 64: ~0.25 T/s useful); a narrow tile is missing. A half-precision accumulator does not
speed things up and errs by 2–6%: discarded.

Per-token product in Vulkan (`tools/vkmv`, 106 checks with no failures; throughput reported with the card
shared, not redone): Q4_K 136–231 G weights/s and Q6_K 150–202 depending on the format, versus ~93 and
~180 from the OpenCL profile (different methods; indicates the same level). Alternative organization with
one row per group of 32 threads is 2.5x faster on matrices with few rows (69 versus 27).

Generalized batch (`tools/vkq4k/mm.comp`, harness `vkmm`; 1,400 + 1,650 cases checked, including
odd sizes): types Q4_K, Q5_K, Q6_K, Q8_0 and F32; modes write, add and gate/up pair with
SwiGLU; narrow tiles for small batches. Redone with the card idle at 8960x1536: Q4_K
1.40 T/s with batch of 16 (was 0.57 padding to 64), 2.50 with 64 and 2.83 with 256; Q6_K 2.10,
3.27 and 3.23. Rule: up to 16 tokens tile 64x16, up to 32 64x32, above that 64x64; split the columns only
with few rows and many columns. Discarded by measurement: Q4_K scale table in shared
memory (slower, because of the extra barrier), tiles of 2 subgroups, reading fp32 directly
(2–6% slower than converting once when several products use the same input).

Kernels that are not products (`tools/vkatt`, 1,720 cases checked): norms, RoPE with KV cache,
single-token attention, batched attention, sum of the experts. GLSL's sine and cosine lose
precision at large angles (error ~2e-4 at position 600): the angle must be reduced first. RMS
norm with subgroup sum: from 35 to 3 µs (batch of 16: from ~1,000 to 6–25 µs). Weak point: batched
attention in three kernels costs 1.2–2.6 ms per batch of 16 at position 600, more than the
products; it needs to become a single launch.

Integration (steps 3 and 4): with `KATANA_BACKEND=vulkan` the dense models run the whole graph of
one token on the card, recorded once and resubmitted (`KATANA_VK_REC=0` uses the live queue). Measured
alternating the backends, benchmark 64+64: Qwen 1.5B 99.7 tok/s of generation on both; SmolLM2 135M
409 tok/s on Vulkan versus ~130 on OpenCL (Vulkan reference 223, CUDA 402). `selftest` passes on
5 dense models with a logit difference of ~3–4e-5 and the greedy text of Qwen 1.5B comes out identical to that of
OpenCL.

Step 5 (batched prompt in Vulkan, `vk_forward_n`): passes of up to 256 tokens (`KATANA_VK_PASS`, `KATANA_VK_CAP`;
256 and 512 tie, 128 loses ~3% on a 512 prompt), products in cooperative matrix (`vk/shaders/mm.comp`, only the 30
variants used: 5 types x tiles 64x64, 64x32w4, 64x16w4 and those of glu), transposed activations
`X[column*B + token]`, norm already in fp16 (`rms_wg16`), gate/up with fp16 output, batched `qkv_post`, attention in
a single launch (`att_flash`: group per (token, kv head), online softmax, KV cache read once per group) and, in the last
layer, only the last token does the feed-forward and the final projection. Without fp16 cooperative matrix the prompt goes token by token.
Measured alternating, benchmark 64+64 (prompt tok/s, OpenCL -> Vulkan): Qwen 1.5B 212 -> 1,387; Llama 3B 107 -> 757;
SmolLM2 135M 1,200 -> 8,100; at 512 tokens: 186 -> 1,550, 95 -> 766, 800 -> 9,300. Generation did not change. Logit
difference against the exact CPU on the batched path: 0.008–0.026 (weights and activations become fp16 in the tiles; the float
path gives ~3e-5); argmax and the following 40 greedy tokens equal to token by token on a 600-token prompt. Batched attention,
three kernels versus one launch (mean position ~320, 64 tokens): 2.3 ms versus 0.40 ms per layer. Cost of a
256-token pass on Qwen 1.5B (149 ms, measured by omitting each phase): gate/up 71 ms, down 34, q/k/v 10, output 7,
attention 19 (14 only in the subgroupAdd of the q.k product), norms 4, qkv_post 2. The products run at ~2.7 T mult/s: below
~110 ms per pass only with a faster product (reference: ~3.5 T).
MoE on Vulkan falls back to OpenCL. Not tested: interactive session and agent through Vulkan, model
larger than VRAM (today it falls back to the CPU), Intel card.

Runtime layer (`vk/kvk.h`, 9 tests passing on NVIDIA and Intel): dispatch waiting after each
one costs ~156 µs; recording the queue, ~4.7 µs; recorded and resubmitted graph, 1.5–2.4 µs.

The card supports fp16 x fp16 cooperative matrices (16x16x16, fp16 or fp32 result) and
int8 x int8 -> int32 (16x16x32), besides the packed integer product. A 128 tile with 16
accumulators per subgroup is worse (0.4–0.7 T/s).

Design decision: Vulkan and OpenCL do not share VRAM, so the Vulkan backend must be
complete (weights, per-token product, batch, attention, experts). OpenCL remains for cards
without these features.

Pieces ready and checked (separate folders; still to be integrated into `katana.c`):

- `vk/kvk.h`: runtime layer (load the library, device, buffers, pipelines, dispatch
  queue, recorded graph).
- `tools/vkq4k`: batched product with Q4_K/Q6_K in the katana layout.
- `tools/vkmv`: per-token product (port of the `mv_*` kernels).

To compile shaders: `ref/vk/glslang/bin/glslang.exe` (allowed in `.claude/settings.local.json`).
Vulkan headers in `ref/vk/include` (outside git; decide whether they go into the repository).

## 11. Where things are

| Path | What |
|---|---|
| `katana.c`, `build.bat` | the engine and how to build it |
| `compare.sh` | alternating comparison with the reference engine |
| `docs/` | this document, the plan and the techniques roadmap |
| `tools/vkprobe`, `tools/vkgemm` | Vulkan prototypes |
| `models/` (outside git) | GGUFs and `.ktn` folders with shards |
| `ref/` (outside git) | reference engine binaries and code, Vulkan headers, shader compiler, old copies of katana for comparison |

Models on disk: SmolLM2 135M, TinyLlama (several formats), Qwen2.5 1.5B/7B/14B, Llama 3.2 3B,
Mistral 7B, OLMoE-1B-7B, Qwen3-30B-A3B.

## 12. Tips for working on the code

- `katana.c` is large: edit with Python replacement scripts with a count `assert`, saved
  to a file. Commands embedded in the shell mangle `\n` and quotes.
- Build: `cmd //c "cd /d C:\source\katana && .\build.bat"` (the warning about `vswhere.exe` is harmless).
- After any kernel change: `selftest` on a dense model, on OLMoE and on Qwen3-30B
  (the latter with `KATANA_KV16=0 KATANA_CTX=512` to fit and compare in 32 bits).
- Large downloads: in parallel ranges with resume and size check before joining.
- Do not run measurements with a download, packing or another benchmark in parallel.
