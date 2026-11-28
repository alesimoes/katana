# Research: data movement across the hardware and algorithms that activate only what matters

Written on 2026-11-28. Research and proposals only: **nothing in this document was implemented**, with one
exception tagged **[measured]** below (P8, point (d): a standalone kernel benchmark on the real phone, not
wired into the engine). Every other number is tagged as one of:

- **[katana]** measured earlier in this project (source: `knowledge.md`, `optimization-roadmap.md`, `handoff.md`);
- **[paper]** reported by the cited paper, on its own hardware and baselines (not comparable across papers);
- **[spec]** from a public specification or data sheet (theoretical ceiling, not what a program gets);
- **[estimate]** my own arithmetic or an order-of-magnitude figure from general knowledge; treat as a hypothesis.

Naming: research systems are cited by the title of their paper, because that is the only way to cite them. Shipping
inference engines are not named, as in the rest of the docs ("reference engine").

The request behind this document, in the owner's words: *"ativar o que realmente importa para responder a pergunta,
não é ativar todos os neurônios, mas enviar direto para o neurônio certo"*.

Target hardware (the proposals are judged against these two machines only):

| | Notebook | Phone |
|---|---|---|
| CPU | i7-11800H, 8 cores / 16 threads, AVX-512 VNNI | Exynos 1580 (s5e8855): 1 Cortex-A720 at 2.9 GHz + 3 A720 at 2.6 GHz + 4 A520 at 1.95 GHz [spec] |
| GPU | RTX 3050 Laptop, 4 GB VRAM, behind PCIe | Xclipse 540 (Vulkan, no cooperative matrix), shares the system memory |
| RAM | 64 GB DDR4 | 7.6 GB total, about 5.4 GB free |
| Storage | NVMe | UFS 3.1 [spec] |
| Working model | Qwen3.6-35B-A3B Q4_K_M: 22 GB, of which 18.3 GB are routed experts; 40 layers x 256 experts, 8 active, 1.96 MB per expert | same file |

---

## 1. How the data moves, and where the latency is

### 1.1 The tiers, with numbers

| Hop | Throughput | Latency of one access | Source |
|---|---|---|---|
| L1d (48 KB per core, notebook) | n/a | about 1 ns (4-5 cycles) | [spec] size, [estimate] latency |
| L2 (1.25 MB per core, notebook) | n/a | about 3-4 ns | [spec] size, [estimate] latency |
| L3 (24 MB shared, notebook) | n/a | about 10-15 ns | [spec] size, [estimate] latency |
| RAM, sequential stream (notebook) | 20-24 GB/s effective; 27-33 G weights/s on cold Q4_K weights | n/a | [katana] |
| RAM, DDR4-3200 dual channel ceiling | 51.2 GB/s | n/a | [spec] |
| RAM, random access that misses cache and TLB | n/a | about 70-100 ns, plus a page walk | [estimate] |
| First touch of a mapped-file page already in the page cache | n/a | about 2.5 us per 4 KB page | [katana] |
| NVMe, one 0.59 MB direct read, 1 in flight | 0.69 GB/s | about 0.85 ms | [katana] |
| NVMe, 16 reads in flight | 1.3 GB/s (0.59 MB blocks), 2.2 GB/s (1.77 MB blocks) | n/a | [katana] |
| NVMe, 8 experts of one layer through the `.exb` bundle | 3.2 GB/s | 4.4 ms for the 8 | [katana] |
| NVMe, a 4 KB random read at queue depth 1 | n/a | about 50-100 us | [estimate] |
| RAM to VRAM over PCIe, one expert (2.8-3.6 MB) | about 4 GB/s effective | about 0.75 ms | [katana] |
| PCIe ceiling | 7.9 GB/s (Gen3 x8), 15.8 GB/s (Gen4 x8) | n/a | [spec]; the link of this card was not read (`lspci -vv`) |
| VRAM, weights read by the product kernels | about 78 GB/s (Qwen 1.5B: 0.85 GB per token) | n/a | [katana] |
| GPU kernel launch, OpenCL | n/a | 22-25 us host time each | [katana] |
| GPU dispatch, Vulkan, recorded graph | n/a | 1.5-2.4 us each; 156 us if the host waits after each | [katana] |
| Expert on the CPU (already in RAM) | n/a | 0.15-0.5 ms | [katana] |
| Phone RAM (LPDDR5) | 25.6 GB/s ceiling at 6400 Mb/s x 32 bits; 10-20 GB/s is what programs usually get | n/a | [spec] ceiling, [estimate] practical; **not measured on the A56** |
| Phone UFS 3.1 | 2.9 GB/s line rate (2 lanes x 11.6 Gb/s); the SliceMoE paper models it at 1.25 GB/s | about 100-200 us for a small random read at depth 1 | [spec], [paper], [estimate]; **not measured on the A56** |
| Energy per bit, flash vs. DRAM (phone) | 103 pJ/bit vs. up to 1.5 pJ/bit, about 70x | n/a | [paper] SliceMoE's hardware model |

### 1.2 What these numbers say

1. **The latency cliffs are at the tier boundaries, not inside a tier.** Running a cold expert on the CPU costs
   0.15-0.5 ms; uploading it to VRAM costs 0.75 ms; reading it from NVMe costs about 0.5-1 ms *when eight are read in
   parallel* and about 3 ms alone [katana, 3 x 0.85 ms before the bundle]. The engine already follows the right rule
   ("the token never waits for a load"). The remaining stall is the SSD tier, where the token has no alternative but to
   wait, and that is the case on the phone for almost every token (1.4).
2. **One expert matrix fits in L2, one expert does not.** Gate, up and down are 0.59 MB each [katana]; L2 is 1.25 MB.
   The eight experts of a layer are 15.7 MB against 24 MB of L3. A Q2_K cold copy of a whole expert is about 1.1 MB
   [estimate from 2.6 vs. 4.85 bits per weight] and fits in L2 whole. This is why prefetching one layer ahead into L3
   (`expert_pf.c`) is the right granularity and why a smaller cold format helps even when the bytes are in RAM.
3. **An expert is almost exactly one 2 MB huge page.** With 4 KB pages an expert spans about 480 pages; a token touches
   320 experts, about 150,000 distinct pages, far more than a second-level TLB holds (about 2,000 entries [estimate]).
   Every expert product therefore starts with a run of page walks. See proposal P7.
4. **PCIe is not a throughput problem here, it is a decision problem.** At about 4 GB/s effective the bus could move
   the 627 MB of experts a token uses in about 150 ms, which is longer than the token itself (70-90 ms). So experts can
   never follow the token over PCIe; only a *prediction* of the topic can live in VRAM. That is what the heat cache does,
   and `knowledge.md` section 4 measured its ceiling (83% of uses with 25% of the experts resident, same topic).
5. **On unified memory "where the bytes are" stops being the question.** The phone has no PCIe hop: a buffer visible to
   the GPU is the same DRAM the CPU reads. Three consequences: (a) there is no upload cost, so a VRAM-style expert
   cache with eviction is pure overhead there; the decision becomes "which device runs this product"; (b) CPU and GPU
   compete for the same memory bandwidth, so running cold experts on the CPU "while the GPU works" is no longer free;
   (c) a second copy of a weight in a GPU layout doubles its cost against the 5.4 GB budget. See proposal P8.
6. **The fixed cost per GPU launch matters only for tiny models** [katana, roadmap section 1]. For the working model
   the limit is weight bytes read per token, on both devices. Any technique that does not reduce *bytes read per token*
   or *bytes that miss a tier* has a low ceiling.

### 1.3 Where the bytes of one token go (Qwen3.6, [estimate] from the model's shape)

| Part | Weights read per token | Share |
|---|---|---|
| Routed experts: 40 layers x 8 x 3.1 M | about 1.0 G | about 1/3 |
| Attention / DeltaNet projections and the shared expert | about 1.5 G | about 1/2 |
| Output head: 248,320 x 2,048 | about 0.5 G | about 1/6 |

This agrees with two project measurements: the routed experts were 24% of a 5-position CPU pass and the dense
projections 56% [katana, roadmap 7.1], and a logits row costs about 0.15 of a plain token on the CPU [katana, roadmap 7.1].
Two readings: the routed experts are the only part the model itself already makes sparse, and they are only a third
of the reads; the output head is the largest *single* dense product and nothing in the roadmap targets it (P4).

### 1.4 The phone is an I/O-bound machine for this model ([estimate], to be confirmed by the phone tests)

The non-expert part of the file is about 3.7 GB (22 - 18.3). With 5.4 GB free, about 1.5 GB is left for experts:
roughly 750 experts at Q4_K, 7% of the 10,240. The project's own hit-rate table gives about 62% at 10% residency
(LRU, one shared set) [katana, `knowledge.md` 5c (a)], so at 7% at least 40% of the 320 expert uses of a token miss:
about 130 reads, about 250 MB per token at Q4_K (about 140 MB with Q2_K cold copies). At 1-2 GB/s that is 70-250 ms of
storage time per token before any arithmetic. On the phone, **the only techniques that matter first are those that
remove misses or make a miss smaller**: P1, P2, P3, P5 and P6. On the notebook (64 GB, everything resident) the same
techniques matter for a different reason: they move work from the CPU cold-expert path (11-14 ms per token [katana,
roadmap 3.2]) to the GPU, or remove it.

---

## 2. "Activate only what matters": the levels at which a model can be made selective

| Level | What is selected | Literature | State in katana |
|---|---|---|---|
| Which experts | the router's top-K | MoE itself | native; heat cache (roadmap 4), tiers (`expert_tier.c`) |
| Which experts are *worth a miss* | routing biased toward what is resident | Cache-Prior (2412.00099), CECAR (NeurIPS 2026) | absent: **P2** |
| How many experts | fewer than K when the gate is concentrated | HOBBIT (2411.01433), LExI (2509.02753), "Don't Skip the Baseline" (2610.28369), MoBiLE (2510.12357) | absent: **P1, P3** |
| At which precision | bits per expert, per use | HOBBIT, SliceMoE (2512.12990), EdgeMoE (2308.14352) | by heat only (KTNEXB02, handoff 2026-11-28): **P1** extends it |
| Which bytes are resident for a topic | per-domain expert sets | MoE-Infinity (2401.14361), task-specific expert pruning (2206.00277), SliceMoE warm-up | per-model heat file only; per-session heat evaluated and dropped (`knowledge.md` 5b): **P5** |
| Which neurons inside an expert | rows of gate/up/down | CATS (2404.08763), Deja Vu (2310.17157), PowerInfer (2312.12456) | measured as **not viable on a dense model** (`knowledge.md` 4); not measured inside experts: **P9** |
| Which vocabulary rows | shortlist of the output head | SVD-softmax (NeurIPS 2017), Learning to Screen (ICLR 2019) | absent: **P4** |
| Which layers | early exit, layer skipping | LayerSkip (2404.16710), SWIFT (2410.06916), DEL (2504.05598), FlexEE (2609.17008), Mixture-of-Depths (2404.02258), Router-Tuning (2410.13184) | absent; the draft/verify machinery exists (`lookup.c`): **P12, P13** |
| Which tokens are computed at all | drafts from the context | prompt lookup | done, off by default (roadmap 7.1) |

The honest summary of the literature: on a model that was **not trained for it**, selectivity works where the model
already has a discrete choice (experts, vocabulary shortlist, cache residency) and works poorly where the choice has to
be imposed afterwards (neurons of a SiLU network, layers of a network without exit training). The project's own
measurement on a dense model (50% of neurons kept, 45% of predictions change) is the same result the sparsity papers
get: Deja Vu and PowerInfer depend on ReLU-family activations or on models retrained to be sparse (ProSparse,
2402.13516). So the proposals below spend the effort on the first kind.

---

## 3. Proposals, in priority order

Effort: small = days, one file; medium = a week or two, several files; large = a format or a backend.
"Changes output" means the logits differ from today's by more than rounding, so the proposal needs a switch and a
quality check (KL of the next-token distribution and top-1 agreement against the unmodified engine, on the project's
own text; `selftest --deep` already computes both) before the owner decides its default.

### P1. Gate-weight-aware treatment of experts that are not resident (precision and skip by importance)

- **Solves.** The cost of a miss. Today a selected expert that is not in VRAM always runs in full on the CPU, and one
  that is not in RAM is always read; the cold Q2_K copy is chosen by the expert's *heat*, not by how much this token
  needs it.
- **Idea.** The 8 gate weights of a token are far from equal. Sort the selected experts by gate weight and define, for
  the expert at rank i, s_i = the sum of the (normalized) gate weights of the experts ranked above it. Then:
  s_i <= T1: run at full precision whatever it costs; T1 < s_i <= T2: if it is not resident, use the low-precision
  copy; s_i > T2: if it is not resident, skip it. The top-ranked expert is always full precision. Resident experts are
  never degraded, so a token whose experts are all hot is bit-identical to today.
- **Grounding.** HOBBIT (2411.01433): the gate weight correlates 0.99 with the norm of the expert's output; with
  T1 = 0.6 and T2 = 0.9 on a top-2 model about 67% of the misses load at high precision, 30% at low and 3% are
  skipped; accuracy loss at most 1% on its two benchmarks; 2.3-3.9x over prior offloading systems on a 24 GB card
  and 3.6-9.9x on a unified-memory board [paper]. SliceMoE (2512.12990) applies the same rule on a phone-class memory
  model: only the 0-2 highest-scoring experts of a token get the full-precision slice [paper]. EdgeMoE (2308.14352) is
  the earlier form (bit width per expert by importance). The thresholds of those papers are for K = 2 and do not
  transfer to K = 8; they have to be chosen here.
- **Already in katana.** The two copies exist (`.exb` hi and Q2_K cold records), the Q2_K kernel is in the fast path
  (commit `07c539c`), and `et.ghot` tracks heat per (layer, expert). Missing: the gate weight reaching the arena's
  choice of record, and the skip.
- **Applicability.** Notebook: applies to the CPU cold path (an expert not in VRAM can run from a Q2_K copy kept in
  RAM: about 1.8x fewer bytes through a bandwidth-bound kernel [estimate]) and to the skip. Phone: directly reduces the
  bytes per miss and the number of misses; this is where it is worth the most.
- **Effort.** Small to medium: `moe_route` already has the weights; the arena read path already picks between two
  records. Open point: whether to renormalize the remaining gate weights after a skip (decide by the KL check).
- **Changes output.** Yes, only on tokens with a miss.
- **Priority: 1.**

### P2. Cache-aware routing: let residency break near-ties in the router

- **Solves.** The number of misses, at the source. Many router decisions are near-ties (the deep `selftest` already
  counts "router near-ties"); choosing the resident expert among near-equal candidates costs almost no quality and
  removes a miss.
- **Idea.** Before the top-K: z' = z + lambda * D * m, where m is the mask of resident experts (VRAM slots on the
  notebook, arena on the phone), D is a running average of the layer's logit range (max - min), and lambda in [0, 1]
  is the single knob. Select the top-K by z'; compute the mixing weights from the **unmodified** z. Optionally the
  top-J by z are always kept.
- **Grounding.** "Mixture of Cache-Conditional Experts" (2412.00099, Qualcomm AI Research): training-free; at
  lambda = 0.5 the miss rate went from 35% to 16% on one model and from 28% to 7% on another, for a perplexity increase
  of 0.1% to 3% depending on the model; up to 2x tokens/s on two Android phones against plain routing with an LRU
  cache; a *learned* prior was not notably better than this one [paper]. CECAR (NeurIPS 2026 poster) argues that the
  hit rate is bounded even under the optimal (Belady) eviction and that a binary resident/not-resident bias can make
  routing unstable; it predicts reuse distance instead [abstract only; not read in full].
- **Already in katana.** No. The hit-rate ceiling measured here (83% of uses at 25% residency, same topic) is the
  ceiling *with the router untouched*; this proposal is the only item in this document that moves that ceiling.
- **Applicability.** Both. On the phone it attacks the dominant cost (1.4). On the notebook it shifts expert work from
  the CPU to the GPU. Caveat on the notebook's Vulkan path: when the router runs on the GPU the mask must be a small
  buffer updated when slots change.
- **Effort.** Small on the CPU router (one add before the selection); medium where the router is on the GPU.
- **Changes output.** Yes, every token, by an amount set by lambda. lambda = 0 is today's behavior.
- **Interaction with P1.** They compose: P2 lowers how often a miss happens, P1 lowers what a miss costs. Tune P2 first;
  it changes which experts are missed.
- **Priority: 1.**

### P3. Static reduction of K, per layer, as the baseline every dynamic rule must beat

- **Solves.** Establishes how much of the 8-expert budget this model needs at all, with almost no code.
- **Idea.** A global and a per-layer override of the number of active experts (K = 8 -> 7, 6, ...), weights
  renormalized over the kept ones.
- **Grounding.** "Don't Skip the Baseline" (2610.28369) compares four dynamic expert-skipping methods (importance
  scores, expert similarity, learned per-token allocators) against exactly this static reduction at matched compute, on
  Qwen3-30B-A3B and Gemma4-26B-A4B among others: the static baseline "is hard to outperform consistently", and the
  dynamic methods often lose to it [abstract; no numbers on the page read]. It also notes that skipping only saves
  time at small batch sizes, which is this project's case. LExI (2509.02753) chooses a different K per layer without
  data. NAEE (2402.14800) is the per-token skipping the comparison includes.
- **Why it is here and not P1.** A static cut degrades every token, hot or not. P1 degrades only misses. But without
  this baseline nobody can tell whether P1's thresholds are doing better than "just use K = 6", and the paper above says
  that is the usual mistake.
- **Already in katana.** No (`n_used` comes from the model).
- **Applicability.** Both. On the phone a "K = 6" mode is a 25% cut of expert reads with one integer.
- **Effort.** Small.
- **Changes output.** Yes.
- **Priority: 2** (do it together with P1, as its control).

### P4. Two-stage output head with a certified shortlist (exact top-k without the full product)

- **Solves.** The largest single dense product of a token: 248,320 x 2,048, about 0.5 G weights, about 0.15 of a CPU
  token [katana]. Nothing in the roadmap addresses it. It is also the "send straight to the right neuron" idea applied
  where it can be made **exact**.
- **Idea.** Offline, factor the head once: W = U S V^T, B = U S (same shape as W). At run time h' = V^T h (one
  2,048 x 2,048 product). The logit of row i is B_i . h'. Compute only the first r coordinates (the *preview*,
  r = 128-256): z~_i = B_i[0:r] . h'[0:r]. By Cauchy-Schwarz the error is bounded:
  |z_i - z~_i| <= ||B_i[r:]|| * ||h'[r:]|| = e_i, where the row norms ||B_i[r:]|| are precomputed and ||h'[r:]|| is one
  number per token. Let L be the k-th largest of (z~_i - e_i). Every row with z~_i + e_i < L is **proved** outside the
  top-k. Compute the exact logit, from the original quantized rows, only for the survivors. Because the singular values
  decay, most of the energy of h' is in the first coordinates and the bound is tight for most rows.
- **Grounding.** SVD-softmax (Shim et al., NeurIPS 2017) introduced the preview window plus exact rescoring of the top
  candidates, as an approximation. Learning to Screen (Chen et al., ICLR 2019, 1810.12406) learns clusters instead.
  The certification by the norm bound is standard maximum-inner-product-search pruning; combining it with the preview
  is my proposal and its shortlist size on this model is **unknown** (it depends on the spectrum of this head). If the
  shortlist comes out large, fall back to the full product for that token; correctness never depends on the bound
  being tight.
- **Cost model [estimate].** Preview at r = 256, 8-bit: 248,320 x 256 = 64 MB read instead of about 300 MB (Q4_K-class
  head), plus the survivors. If survivors are a few thousand rows the head drops to roughly a quarter of its cost,
  about 10% of a CPU token.
- **Sampling.** The engine samples with temperature and top-k; an exact top-k set with exact logits is sufficient.
  Top-p/min-p applied inside the top-k set stay exact as well.
- **Already in katana.** No. The lazy per-position logits of `lookup.c` reduce *how many* head products are done, not
  the cost of one. Roadmap section 7 has "top-k by histogram", which speeds the selection, not the product.
- **Applicability.** CPU paths on both machines; phone especially (the head is 0.5 GB of DRAM traffic per token).
  Irrelevant where the head runs on the GPU with the whole graph. Extra storage: the preview matrix (64 MB at 8 bits)
  and V (4 M floats). The factorization is a one-time offline step (an SVD of a 248,320 x 2,048 matrix needs only the
  2,048 x 2,048 Gram matrix and a symmetric eigen-solver, which is writable in C).
- **Effort.** Medium.
- **Changes output.** No for greedy and top-k sampling (the kept logits are the exact ones).
- **Priority: 2.**

### P5. Domain heat profiles chosen before the first token, and prefill-informed warm-up

- **Solves.** The cold start of each topic: the cache warms "by use within tens of tokens" [katana, 5b], which on the
  phone means tens of tokens at the miss rate of an empty cache.
- **Idea.** (a) Keep several heat vectors per model, one per domain (C code, Portuguese prose, tool calls, ...), built
  from the routing traces the engine already produces; the pre-LLM layer (UIOE) already classifies the request before
  the model runs, so its label picks the vector that drives preload, pinning, and which experts get the hi record.
  (b) During prefill the router outputs of every prompt token are known before generation starts: use their counts to
  reorder the cache and start the reads for generation while the prompt is still being processed.
- **Grounding.** MoE-Infinity (2401.14361) matches each request against stored expert-activation traces to drive
  prefetch and caching (2-20x latency against offloading baselines, on servers) [paper]. SliceMoE's predictive cache
  warm-up reshapes the cache from prefill statistics: 1.96x decode speed against an empty cache in its model [paper].
  Task-specific expert pruning (2206.00277) and NAEE (2402.14800) show that a task uses a stable subset of experts.
  The project's own numbers say the same: a set calibrated on the same topic serves 79% of uses at 25% residency
  against 25% by chance, and on another topic 34% (OLMoE) [katana].
- **Already in katana.** One heat file per model (`.ktn.heat`), preload of the hottest. Per-session heat was evaluated
  and not built because no gain could be measured on a machine where the model fits in RAM (`knowledge.md` 5b). What
  changed since: the UIOE layer gives a topic label for free, and the phone is a machine where the start of a session
  is expensive. Whether prefill routing already feeds the heat was not checked in the code.
- **Applicability.** Both; small on the notebook (the VRAM cache warms quickly), large on the phone.
- **Effort.** Small.
- **Changes output.** No.
- **Priority: 2.**

### P6. A fitted predictor of the experts 2-4 layers ahead

- **Solves.** Lead time for the SSD tier. The early router is good one layer ahead (78.8 / 89.7 / 93.5% recall at
  N = 8 / 12 / 16) and decays with distance (70.0 / 81.0 / 85.8 at two layers, 64.1 / 74.8 / 80.0 at three) [katana].
  One layer of lead is about 2 ms on the notebook; reading 8 experts takes 4.4 ms there and more on UFS. The reads
  have to be issued two to four layers early to finish in time.
- **Idea.** For each layer l and distance d, fit offline a linear map from the residual at layer l to the router
  logits of layer l + d: ridge regression on recorded (residual, router logits) pairs, closed form through the
  2,048 x 2,048 normal equations, solvable in C with a Cholesky factorization. Reusing layer l + d's own router on the
  residual of layer l (what the early router does) is the special case "map = that router"; the fit corrects for the
  drift of the residual stream. A low-rank version (rank 64-128) keeps the cost per layer below the router's.
- **Grounding.** HOBBIT stacks the next p gating modules on the current input: about 96% top-1 for the next layer and
  about 90% for the next two or three [paper; top-1 on 8-expert layers, not comparable with recall of 8 among 256].
  PreScope / LayerScope (2509.23638) trains a layer-aware predictor and schedules prefetch across layers against PCIe
  contention: +141% throughput against prior systems [paper, servers]. A 2026 paper (2603.19289) studies small neural
  estimators on intermediate states for the same purpose [abstract only]. Pre-gated MoE (2308.12066) moves the
  decision one block earlier by training; Read-ME (2410.19123) decouples the router entirely, also by training.
- **Already in katana.** Early router one layer ahead (`expert_tier.c`), prefetch during the GPU wait
  (`expert_pf.c`); "prefetch 2 layers ahead with a smaller N" is listed as a next step in roadmap section 4. The fitted
  map is the new part.
- **Applicability.** Phone first (storage latency is the longest hop); on the notebook only with a small RAM budget.
- **Effort.** Medium (trace recording, the solver, a small file of maps per model).
- **Changes output.** No (prediction only reads).
- **Priority: 3.**

### P7. Page and memory-class discipline for expert bytes (huge pages, clean pages, pinning)

- **Solves.** Hidden per-expert latency that is not arithmetic: page walks, first-touch faults, and on Android the
  reclaim path.
- **Idea.** (a) Notebook: back the expert arena with 2 MB pages (`madvise(MADV_HUGEPAGE)` on a 2 MB-aligned
  allocation; one expert is 1.96 MB, so one expert is one TLB entry instead of about 480). (b) Phone: prefer
  **file-backed, clean** pages for expert bytes over an anonymous copy. Under memory pressure Android drops clean file
  pages for free and they are re-read from UFS, but anonymous pages are compressed into zram by the kernel, costing CPU
  on both directions and competing with the engine's own threads. An anonymous arena filled by `pread` is the right
  design on the notebook and possibly the wrong one on the phone; a mapped `.exb` with `mlock` on the hot set and
  `MADV_POPULATE_READ` for prefetch (already used in `qwen4exp.c`) may behave better. (c) Keep threads on the four
  A720 cores for bandwidth-bound work; the four A520 add little to a memory-bound loop and heat the package.
- **Grounding.** Standard operating-system behavior; the paging cost is this project's own measurement (2.5 us per
  first-touched 4 KB page; cold start of an MoE "dominated by pages touched for the first time") [katana]. The
  smartphone paper PowerInfer-2 (2406.06282) organizes its whole storage engine around the same facts (compute and UFS
  I/O pipelined at small granularity, a segmented cache) and Alibaba's mobile LLM paper (2506.10443) balances work
  across unequal cores [paper]. The zram point is an **[estimate]** about this phone, to be checked with
  `/proc/meminfo` and `/proc/vmstat` during a run.
- **Already in katana.** No huge-page or `mlock` use in `src/engine/` (checked by search). The thread pool already
  hands out blocks by an atomic counter, which adapts to unequal cores by itself.
- **Applicability.** (a) notebook; (b) and (c) phone. Transparent huge pages may be disabled on the phone's kernel.
- **Effort.** Small.
- **Changes output.** No.
- **Priority: 2** (cheap, no quality risk).

### P8. A unified-memory execution model for the phone

- **Solves.** The notebook design (pin attention in VRAM, cache experts in VRAM, cold ones on the CPU in parallel)
  assumes two memories and a bus. On the phone that design pays for copies and eviction that buy nothing (1.2, item 5).
- **Idea.** (a) One copy of each weight, in a layout both the CPU kernel and the shader can read, in memory that is
  both host-visible and device-local; import instead of copy where the driver allows it (`VK_EXT_external_memory_host`
  or Android hardware buffers; **support on the Xclipse 540 is unknown**). (b) Replace "is it in VRAM?" by a cost
  rule per product: rows x columns large and batched -> GPU; one expert for one token -> whichever device is idle,
  because the bytes are equally near to both. (c) Do not run bandwidth-bound CPU work concurrently with
  bandwidth-bound GPU work and expect the sum: measure the pair, they share one bus. (d) Without cooperative matrices
  the batched product needs the packed 8-bit integer dot product (`VK_KHR_shader_integer_dot_product`) or plain
  fp16 arithmetic; probe both.
- **Grounding.** The unified-memory results in the literature are large precisely because the copy disappears: HOBBIT
  reports its largest gains on a 32 GB unified-memory board [paper]; Google's Gemma 3n keeps large, cheap-to-compute
  parameter tables (per-layer embeddings) on the CPU side and only the core weights in accelerator memory, and loads
  modality parameters conditionally [vendor guide]; PowerInfer-2 splits work between a dense accelerator path and a
  sparse CPU path over one memory [paper].
- **Already in katana.** The Vulkan runtime (`vk/kvk.h`) and the MoE Vulkan path exist; the residency logic is the
  notebook's.
- **Applicability.** Phone only (and any future unified-memory machine).
- **Effort.** Medium to large. Another agent is working on the phone now; this item is input for that work, not a
  separate task.
- **Changes output.** No.
- **Priority: 2 for the phone, not applicable to the notebook.**
- **Update [measured] (2026-11-28, on the device, point (d) only):** `katana vkcaps` (new, `d9a1653`) confirms the
  Xclipse 540 has no `VK_KHR_cooperative_matrix` and does have `VK_KHR_shader_integer_dot_product`, `shaderFloat16`
  and `shaderInt8`; `subgroup_size` 64, `max_shared_bytes` 32768. A standalone kernel benchmark
  (`tools/vkgemm_mobile/`, not wired into the engine) reproduced the roadmap's own notebook finding ("Prototype
  tools/vkgemm": tiling + shared memory + vectorized loads, no matrix hardware, gave 3-5x over an unorganized
  shader) on this phone: naive 0.020-0.025 T mult/s, tiled+vectorized fp16 **0.095-0.101 (~4.1x)**, the same tile
  with int8 input and `dotPacked4x8EXT` 0.064-0.068 (~2.8x, *slower* than the fp16 tiled kernel despite being
  bit-exact against a CPU reference — the packed dot product is most likely software-emulated on this driver).
  **So: the tiled/vectorized fp16 path is the one worth integrating; the int8 dot-product extension is present
  and correct but not worth using on this device.** This is a kernel-level measurement only (T mult/s on
  synthetic shapes matching Gemma 4 E2B's dense products); the end-to-end generation tok/s this would give if
  wired into `mm.comp`/`vk_plan` (today, without coopmat, batch is entirely disabled and every token — prompt
  and generation — runs through the token-by-token kernels at a measured 3.5-3.7 tok/s) is not measured yet and
  is intentionally not estimated here. Full write-up: `docs/new-models.md`, "Phone (Xclipse 540, Galaxy A56)"
  under the Gemma 4 E2B section.

### P9. Activation sparsity inside an expert (measure first)

- **Solves.** Two thirds of an expert's work (up and down) for the neurons whose gate activation is near zero.
- **Idea.** After the gate product, neurons with |SiLU(gate_j)| below a threshold are dropped: their `up` row is not
  computed and their column of `down` is not accumulated. Row-skipping `up` is trivial (rows are contiguous). `down`
  needs a neuron-major layout (each neuron's 2,048 outputs contiguous, eight Q4_K blocks) so that it becomes a sum of
  scaled columns over the active neurons.
- **Grounding.** CATS (2404.08763): at 50% activation sparsity two 7B models stay within 1-2% of the base model on
  downstream tasks without fine-tuning, and a custom kernel gives about 15% less latency per token [paper]. Apple's
  "LLM in a Flash" (2312.11514) stores row i of the up projection with column i of the down projection for the same
  reason, on ReLU-sparse models. Deja Vu (2310.17157) and PowerInfer (2312.12456) predict the active neurons instead
  of thresholding.
- **Against it.** This project measured the dense case: with the *best* 50% of neurons per token, 6% of predictions
  still change; with a fixed 50%, 45% change [katana, `knowledge.md` 4]. Experts are narrower and more specialized than
  a dense feed-forward, so the answer may differ, but that is a hope, not a result.
- **Already in katana.** `katana sparsity` exists for dense feed-forward layers; not for experts.
- **Effort.** Small for the measurement (extend `katana sparsity` to expert neurons, with the threshold form instead
  of top-N); medium to large for the kernel and the layout, only if the measurement supports it.
- **Changes output.** Yes.
- **Priority: 3 (measurement), 4 (implementation).**

### P10. Confidence-gated half-K token with fallback

- **Solves.** Expert reads on "easy" tokens.
- **Idea.** Run the token with K/2 experts; if the top probability is below a threshold, discard it and rerun with K.
  The discarded pass has already produced the router choices of every layer, so the full pass can prefetch all of them.
- **Grounding.** MoBiLE (2510.12357): threshold 0.7, 11-21% of tokens fall back, 1.6-1.7x on a 16 GB card with
  offloading [paper]. Its own table shows a visible drop on a math benchmark (0.641 -> 0.608 and 0.515 -> 0.448) that
  the text calls negligible; it is not.
- **Ceiling here [estimate].** Routed experts are about a third of the token's reads (1.3). Halving them saves about
  1/6 of a resident token, minus the fallbacks (each costs a whole extra token and, on Qwen3.6, a restore of the
  DeltaNet state; the per-position checkpoint of `lookup.c` can be reused). On the notebook the net is small. On the
  phone, where misses dominate, it is larger, but P1 gets most of the same saving without a second pass.
- **Effort.** Medium. **Changes output.** Yes. **Priority: 4**, after P1-P3 show what is left.

### P11. One nested expert format instead of two copies; low-rank shared-basis experts

- **Solves.** Disk and RAM: the `.exb` now stores a hi record and a Q2_K record of the same expert.
- **Idea A (bit slices).** Quantize so that the low-precision expert is the most-significant slice of the
  high-precision one; "upgrading" a cold expert reads only the missing slice. SliceMoE's AMAT derives the low slice by
  truncating value and zero-point by the same shift, with no calibration (naive truncation degraded badly in its
  table) [paper]. Matryoshka Quantization (2502.06786, Google DeepMind) trains the nesting.
- **Idea B (shared basis).** Factor each expert matrix into a small expert-specific part times a combination of basis
  matrices shared by the whole layer; the basis stays resident, the per-expert part is small. MoBE (2508.05257,
  ICLR 2026) and RFID-MoE (2602.09316) report this on Qwen3-30B-A3B, the second one at 60% compression with rank
  allocated by routing frequency and effective rank; MoE-SVD (ICML 2025) is the training-free ancestor [papers;
  quality numbers not extracted, read before committing].
- **Already in katana.** Roadmap section 8 lists low-rank *attention* (a model family), not low-rank experts.
- **Applicability.** Both; most valuable on the phone (storage is 128-256 GB and the bundle adds 18.3 GB).
- **Effort.** Large (a new on-disk format, a new kernel for A; an offline optimizer for B, which the papers run in a
  training framework). K-quant blocks are not nested, so A cannot reuse Q4_K/Q2_K.
- **Changes output.** A: no for hot experts if the high slice is kept exact; B: yes.
- **Priority: 4.**

### P12. Self-speculative drafting by early exit

- **Idea.** Draft tokens with the first E layers, verify with the full model in one batched pass.
- **Grounding.** LayerSkip (2404.16710, Meta): 1.34-2.16x, but the model is trained with layer dropout and an
  early-exit loss. SWIFT (2410.06916) skips intermediate layers without training. DEL (2504.05598) picks the exit
  layer and draft length at run time (up to 2.5x). FlexEE (2609.17008) targets weight offloading: 1.25-1.27x with
  nothing offloaded, 2.8-3.2x with 50% offloaded, with exit supervision (so, trained) [papers].
- **Why low here.** The project already measured the verification side: a batched position costs 0.65 of a plain token
  and a served draft token 0.85 even at 100% acceptance, a ceiling of about 1.18x [katana, roadmap 7.1], and that was
  with a *free* draft. A draft that costs E/40 of a token and is accepted less often than a verbatim copy cannot beat
  it on the CPU path. The FlexEE result suggests where it would pay: when skipped layers are layers whose weights would
  have had to be *fetched*; that is the phone, and only if the working model's intermediate layers predict well without
  exit training (unknown; a logit-lens measurement per layer would answer it cheaply).
- **Effort.** Medium (the draft/verify/rollback machinery is `lookup.c`). **Changes output.** No (verified).
- **Priority: 5**; revisit when the batched pass costs close to one token.

### P13. Dynamic depth (skip layers per token)

- **Grounding.** Mixture-of-Depths (2404.02258, Google DeepMind) trains the model with per-token routing around
  blocks. Router-Tuning (2410.13184, EMNLP 2025) trains only small routers on a frozen model: 21% faster for 0.2% loss
  in its first version [paper]. AdaInfer (2403.02181) stops early from logit statistics without training.
- **Why low here.** It needs trained routers (the project trains only at night and only tool/planning data), and
  Qwen3.6's DeltaNet layers carry recurrent state that a skipped layer would not update. **Priority: 5.**

---

## 4. What the large labs are doing for constrained hardware, and what transfers

| Who | Direction (2024-2026) | Transfers to katana? |
|---|---|---|
| Google | Gemma 3n: MatFormer nested sub-models, per-layer embeddings kept on the CPU side, conditional parameter loading, KV sharing from the middle layer to the top layers (2x prefill) [vendor guide]; Matryoshka Quantization; Mixture-of-Depths | The memory placement ideas yes (P8). MatFormer, KV sharing and MoD are trained into the model: only by running such a model. |
| Qualcomm AI Research | Cache-conditional routing for MoE on phones | Yes, directly: P2. |
| Apple | "LLM in a Flash": windowing and row-column bundling for flash-backed inference | The bundling is what `.exb` does per expert; the neuron-level version needs sparse activations (P9). |
| Microsoft | BitNet b1.58 2B4T (2504.12285): natively ternary models with lookup-table CPU kernels; small models for NPUs | A model-family decision (ternary kernels would sit next to `cpu_lowbit.c`); outside the "minimum 24 GB class model" catalog today. Their large-scale serving/training stack is data-center work and does not apply. |
| Meta | LayerSkip; sub-billion models designed for phones (2402.14905) | P12, low. |
| NVIDIA | Small hybrid state-space + attention models (Hymba, 2411.13676); on consumer cards the driver's system-memory fallback | The fallback is the cliff the project measured at 92% VRAM. The hybrid direction matches the `granitehybrid` work. |
| Alibaba / Qwen | MoE with a few billion active parameters as the consumer design point; mobile inference paper (2506.10443): embedding rows read from flash on demand, mixed quantization, big.LITTLE balancing | The model family is the working model. The embedding point is already true here (mapped file: only touched rows are resident). Core balancing: P7 (c). |
| DeepSeek | Low-rank attention with compressed KV, sigmoid router, shared expert | Roadmap section 8. |
| SJTU IPADS | PowerInfer / PowerInfer-2: hot neurons on the accelerator, cold on the CPU, I/O pipelined with compute; a 47B model at 11.68 tok/s on a phone [paper], with models retrained for ReLU sparsity | The architecture is the neuron-level twin of katana's expert cache. The neuron level itself needs retrained models. |
| Groq, Cerebras | Custom silicon that removes the memory-bandwidth limit by keeping weights next to the compute | Not applicable as engineering. As a lesson it agrees with 1.2 item 6: the limit is bytes per token per second, so the consumer equivalent is to read fewer bytes (P1-P4), not to compute faster. |
| Mistral | Small dense models for edge devices | No technique to transfer. |

Academic systems specific to "MoE larger than the fast memory", the project's exact problem: Pre-gated MoE
(2308.12066), EdgeMoE (2308.14352), SiDA-MoE (2310.18859), MoE-Infinity (2401.14361), Fiddler (2402.07033; cold experts
on the CPU, what katana does), AdapMoE (2408.10284), HOBBIT (2411.01433), Cache-Prior (2412.00099), PreScope
(2509.23638), MoBiLE (2510.12357), SliceMoE (2512.12990), CECAR (NeurIPS 2026). katana already has the union of what
the 2023-2024 systems proposed (heat cache, CPU fallback, early-router prefetch, bundle, mixed precision by heat). The
two ideas from that line that it does **not** have are the ones ranked first here: making the *router* aware of the
cache (P2) and making the *precision/skip* decision depend on the gate weight (P1).

---

## 5. Looked at and not recommended

| Technique | Reason |
|---|---|
| Contextual sparsity of the dense parts (attention projections, shared expert) | Measured here as not viable on a SiLU model without retraining (`knowledge.md` 4); the literature needs ReLU-family or retrained models. These parts are half the reads of a token and stay dense. |
| Draft trees / several candidate drafts | Already analyzed and rejected with numbers in roadmap 7.1. |
| A second small draft model | Against "minimum 24 GB class, no small dense models"; costs RAM the phone does not have. |
| Per-token learned expert allocators | The controlled comparison (2610.28369) finds they do not beat a static K consistently. |
| Sequence-level top-K (2511.06494), top-p routing trained into the model (2403.07652, 2405.14297) | Need training of the model. |
| io_uring for the SSD tier | Measured here: no faster than the thread pool, the device is the limit (`knowledge.md` 5c (c)). |

---

## 6. Suggested order

1. **P3 + P1 + P2 together, behind switches**, with one quality check (KL and top-1 agreement against the unmodified
   engine on the project's text, as a function of the share of expert uses that were degraded, skipped or rerouted).
   P3 is the control; P2 is tuned before P1.
2. **P7** (huge pages on the notebook; page-class check on the phone) and **P5** (domain heat from the UIOE label,
   prefill-informed warm-up): no change of output, small.
3. **P4** (certified two-stage head): first compute the spectrum of the head and the shortlist size on recorded hidden
   states; build only if the shortlist is small.
4. **P8** as input to the phone work in progress.
5. **P6**, then the measurement half of **P9**.
6. P10-P13 only if the earlier steps leave a gap that they address.

Everything above is a proposal. The gains quoted from papers were obtained on other models, other hardware and against
other baselines; none of them is a prediction for this engine.

---

## 7. Sources

Read for this document (abstract page or HTML full text, 2026-11-28):

- Mixture of Cache-Conditional Experts for Efficient Mobile Device Inference. arXiv:2412.00099. https://arxiv.org/abs/2412.00099
- HOBBIT: A Mixed Precision Expert Offloading System for Fast MoE Inference. arXiv:2411.01433. https://arxiv.org/abs/2411.01433
- SliceMoE (bit-sliced expert caching under miss-rate constraints). arXiv:2512.12990. https://arxiv.org/abs/2512.12990
- MoBiLE: Efficient Mixture-of-Experts Inference on Consumer GPU with Mixture of Big Little Experts. arXiv:2510.12357. https://arxiv.org/abs/2510.12357
- Don't Skip the Baseline: A Controlled Comparison of Dynamic Expert Skipping in MoEs. arXiv:2610.28369 (abstract only). https://arxiv.org/abs/2610.28369
- PreScope / LayerScope: predictive cross-layer scheduling for MoE inference. arXiv:2509.23638 (abstract only). https://arxiv.org/abs/2509.23638
- FlexEE: Self-Speculative and KV-Compatible Early Exiting for Offloading-Aware LLM Inference. arXiv:2609.17008 (abstract only). https://arxiv.org/abs/2609.17008
- CECAR: Cache & Expert Co-Aware Routing. NeurIPS 2026 poster (search summary only). https://neurips.cc/virtual/2026/poster/152287
- Introducing Gemma 3n: The developer guide (search summary). https://developers.googleblog.com/en/introducing-gemma-3n-developer-guide/
- Exynos 1580 / Galaxy A56 specifications (search summary). https://www.gsmarena.com/samsung_galaxy_a56-13603.php

Seen only as search summaries (title, id and headline claim; not read):

- PowerInfer-2: Fast Large Language Model Inference on a Smartphone. arXiv:2406.06282.
- LExI: Layer-Adaptive Active Experts. arXiv:2509.02753.
- MoE-Infinity. arXiv:2401.14361.
- MoBE: Mixture-of-Basis-Experts. arXiv:2508.05257. RFID-MoE. arXiv:2602.09316. MoE-SVD (ICML 2025). KBVQ-MoE. arXiv:2602.11184.
- Router-Tuning. arXiv:2410.13184 (EMNLP 2025).
- LayerSkip. arXiv:2404.16710. DEL. arXiv:2504.05598.
- CATS: Contextually-Aware Thresholding for Sparsity. arXiv:2404.08763.
- Sequence-level top-K routing. arXiv:2511.06494. Harder Tasks Need More Experts. arXiv:2403.07652. DynMoE. arXiv:2405.14297.
- Expert prefetch with neural estimators on intermediate states. arXiv:2603.19289.

Cited from memory (identifier and claim **not re-checked today**; verify before relying on them):

- Deja Vu: Contextual Sparsity for Efficient LLMs at Inference Time. arXiv:2310.17157.
- PowerInfer. arXiv:2312.12456. ProSparse. arXiv:2402.13516.
- LLM in a Flash. arXiv:2312.11514.
- Mixture-of-Depths. arXiv:2404.02258. AdaInfer. arXiv:2403.02181. SWIFT. arXiv:2410.06916.
- Pre-gated MoE. arXiv:2308.12066. EdgeMoE. arXiv:2308.14352. SiDA-MoE. arXiv:2310.18859. Fiddler. arXiv:2402.07033. AdapMoE. arXiv:2408.10284. Read-ME. arXiv:2410.19123.
- Not All Experts are Equal (NAEE). arXiv:2402.14800. Task-Specific Expert Pruning for Sparse MoE. arXiv:2206.00277.
- Matryoshka Quantization. arXiv:2502.06786. BitNet b1.58 2B4T. arXiv:2504.12285.
- MobileLLM. arXiv:2402.14905. Hymba. arXiv:2411.13676. Alibaba's mobile LLM inference paper. arXiv:2506.10443.
- SVD-softmax (Shim et al., NeurIPS 2017). Learning to Screen (Chen et al., ICLR 2019). arXiv:1810.12406.
- Cache sizes of the i7-11800H, DDR4/LPDDR5/PCIe/UFS line rates: public specifications; cache and DRAM latencies are
  order-of-magnitude figures.
