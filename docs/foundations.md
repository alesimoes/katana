# Foundations audit: what katana stands on, where it departs, and what proves the departure

Written 2026-11-24 on branch `docs-foundations`. Documentation only: no code was changed, no model was run, no benchmark was run for this page.

The owner's decision this audit serves: *"our project is meant to be unusual, but better, and not outside the architecture because it is crazy or uninformed."*
So the test applied to every layer is the same four questions: which established technique is it built on; where does our code leave that technique, and why;
what measurement in this repo shows the departure helps (or the fact that there is none); and what to do about it.

How to read it:

- **Sources.** A reference is cited only if it was opened or checked on 2026-11-24 (arXiv abstract pages, Crossref records, the pages named). Whatever could not be
  checked is marked **unverified** and listed in section 14. No title, author, year or number was filled in from memory. "Checked" means the title and first author
  match the cited identifier; it does not mean every claim in the paper was re-read. Where a claim about a paper matters (sections 3 and 4), the abstract was read and
  is quoted in substance.
- **Repo evidence.** Every statement about katana was read in this tree (`docs/`, `git log`, file names under `src/` and `tools/`). The C code itself was not
  re-audited line by line; where a claim depends on the code and not on a document, it says "per docs".
- **Rule note.** `CLAUDE.md` says documents call other inference engines "reference engine". The GGUF and k-quant links in section 12 live in that engine's
  repositories, so the URLs necessarily show it; the text does not.
- **Classification** (one per row): `grounded`; `grounded, deviation justified`; `deviation unmeasured`; `ad hoc, needs test or replacement`.

A meta-finding comes first because it conditions everything else: `CLAUDE.md` currently says *"no tests or measurements in this phase (owner's decision,
2026-11-22)"* and that the only working model is Qwen3.6-35B-A3B. That rule is why most rows below read "unmeasured". It is a legitimate scheduling choice, but it
means that the part of this page that asks for measurements cannot start until the owner lifts it for the named items (section 12 proposes the smallest set).

---

## 1. Executive summary: the ten largest gaps, by priority

Priority = (how much the project leans on it) x (how little there is to show for it) x (how cheaply it can be closed).

| # | Gap | Why it ranks here | Class | First action |
|---|---|---|---|---|
| 1 | **Codegen has its own input syntax (the "code spec") and nobody has measured whether Qwen writes it correctly.** The first-try validity rate is "not measured" (`docs/codegen.md` 12, 14). The literature says models are weaker on formats and languages they saw little of (section 8), and the "fewer tokens" premise was measured false (spec = 1.16x idiomatic Python). | The whole codegen bet (phases F, G) rests on it, and the core is not even in the build yet. | ad hoc, needs test or replacement | Run the 20-request harness (module G) against three arms: direct Python, JSON under a schema grammar, code spec. Decide with numbers; keep the typed IR and validators either way. |
| 2 | **The briefing selector's weights were hand-picked on one briefing** (+2.5 for a colour, +1.5 for a style word, 60% mandatory cap, 0.35 exponent for the logo blend...). No second corpus, no comparison with plain truncation, no check that the image improves. | A silent cut changes what is drawn; the only "test" is that words are a subsequence and the old template terms do not reappear. | ad hoc, needs test or replacement | Build a 30-brief corpus; compare against head-truncation and against a standard prompt-compression baseline; judge by an image metric plus a blind pairwise look. |
| 3 | **Specialization (LoRA/DoRA/PiSSA, curriculum) has no result on the real model.** Everything is measured on tiny random models; "no codebench pass on the new data exist yet" (`docs/plan.md`); the curriculum is deferred until UIOE ends. | The product story ("the model learns katana's tools") depends on it, and gradient correctness is the only thing proven. | deviation unmeasured | One night-run on Qwen3.6, scored by `katana codebench` before/after on held-out tasks, plus the drift loss already in the harness. |
| 4 | **UIOE is built (U0 to U5, U8) with "nothing measured", and the only retrieval numbers we do have are weak** (BM25 at rank 1 right about 4% of the time on the 76-question set; the hop slice is a documented negative result). | It sits in front of every request; its routing, gating and context-packet format are all justified by design arguments, not by an A/B. | deviation unmeasured | Replay the owner's real sessions with UIOE on and off; count rounds, tokens and answer-correct. |
| 5 | **The Stable checkpoints (InteriorDesign, XSAchi) were chosen with no quality criterion.** The doc itself calls InteriorDesign "a re-upload of unknown lineage... 4 downloads"; XSAchi appears only in desktop code. | The user picks a model that changes every render; the licence tag is the uploader's claim. | ad hoc, needs test or replacement | Fixed prompt set x fixed seeds x each checkpoint; CLIP score plus a pairwise preference sheet; record licence evidence. Drop what does not win. |
| 6 | **The FLUX.2 klein LoRA has never been trained on real data.** Gradients match PyTorch to about 1e-5 (excellent), an overfit test learns, and `docs/render.md` says it "says nothing about generalization". | Correct machinery, unproven purpose. | deviation unmeasured | Collect the first 20 to 50 approved SketchUp/render pairs; hold out scenes; compare with and without the adapter on held-out views. |
| 7 | **The agent layer has no end-to-end task evaluation on the real model**; 765 tool checks run without a model, and several A-items are "code done, not measured with a model" (`docs/plan.md`). | The agent is the product; tool-use quality is the thing a user feels. | deviation unmeasured | Fix a task suite (codebench already exists), run it per release, publish pass rate and invalid-call rate. |
| 8 | **Engine numerics have no model-level quality number against a standard** (perplexity or KL of the real quantized model against its float reference on public text). Kernel-level selftests are good (1e-5 range); a KL study is *proposed* for the expert-tier idea, not run. | Quantization and GPU precision shortcuts (f16 activations, fp16 KV) are standard, but "standard" has to be shown on this engine. | grounded, deviation justified (kernels); deviation unmeasured (model level) | Run KL/top-1 agreement against a float reference on a fixed 20k-token text, per backend and quant. |
| 9 | **Code-intelligence's Edit-by-symbol and the LSP path have little real-world evidence**: the LSP tests use `fake_lsp.py`; the plan lists "LSP with a real server" among tools "written and never exercised"; `tools/editbench` exists but its results are not recorded in the docs I read. | The design is sound (symbol-addressed edits are used by published agent work), but our scanners are tolerant, not full parsers. | deviation unmeasured | Record editbench results; run one real server (for example a Python and a TypeScript one) end to end. |
| 10 | **Names.** the earlier three-letter name of the input format collided with three established meanings (now "code spec", `.kcode`); "UIOE" has no market equivalent; `.kspec` names an adapter file while `KATANA_SPEC` names the specialization switch (and a speculative-decoding switch used to share it). | Cost of confusion grows with every doc and every outside reader. | ad hoc, needs test or replacement | Rename code spec (section 11); keep UIOE as an internal name but always gloss it; resolve the `spec` overload. |

---

## 2. Method, and how the layers map to the established vocabulary

Short glossary first, because half of the "unusual" feeling is vocabulary. The full table (our term to market term) is in section 11.2.

| Our name | Closest established concept |
|---|---|
| UIOE (Interpret, Classify, Route, Retrieve, Orchestrate) | query understanding, then query routing, then retrieval-augmented generation, assembled by "context engineering" |
| Context Builder | context assembly / prompt construction; context engineering |
| Briefing selector (`sdsel.c`) | extractive prompt compression with a lexical relevance score |
| Codegen | a code generator driven by a typed intermediate representation (a program-synthesis back end), fed by a small DSL |
| Code spec (file extension `.kcode`) | a domain-specific language (DSL) for programs; not Hoare's process algebra |
| Evidence graph / evidence pack | retrieved-context set with provenance |
| Prefix cache | prompt (prefix) caching, KV-cache reuse |

---

## 3. Inference engine

Per `docs/knowledge.md`, `docs/comparisons.md`, `docs/optimization-roadmap.md`, `docs/handoff.md`. Numbers quoted are the repo's own.

### 3.1 Model format and quantization (GGUF, Q4_K/Q6_K and relatives)

- **Established basis.** GGUF file format (specification in the ggml repository); k-quant block formats with super-block scales (reference-engine PR "k-quants", #1684). Both opened. Wider context for why 4-bit weight-only quantization is acceptable: GPTQ (arXiv 2210.17323) and AWQ (arXiv 2306.00978), both checked; katana does not implement them, it reads the GGUF k-quants.
- **Departure.** Katana re-lays the quantized weights in its own interleaved layout for its kernels ("Q4_K weights in the katana layout" in `docs/comparisons.md`); the numerics of the format are unchanged. Reads the catalog's pinned, sha256-verified files.
- **Evidence.** Kernel-level: `katana selftest` compares GPU and CPU matrix products and the CPU path with exact float math; `src/engine/qtest.c`; `cpu_lowbit_test.c`. Layout speedups are measured in `docs/comparisons.md`. Model-level quality (perplexity/KL vs a float reference on real text) is described as the *deciding experiment* for a proposed 2-bit expert tier (`docs/knowledge.md` around lines 249-254), so it is a plan, not a result. A KL study of the GPU path against float32 on real text is mentioned at line 295; I did not find it recorded as a table.
- **Class.** grounded (format), deviation unmeasured at model level (layout and precision shortcuts).
- **Action.** Add one table to `docs/comparisons.md`: KL and top-1 agreement vs a float reference for the working model on a fixed 20k-token text, per backend. Until it exists, do not claim "same quality as the reference engine", only "same format".

### 3.2 GEMM / matrix-vector kernels (CPU AVX2, Vulkan GLSL, CUDA PTX)

- **Established basis.** Tiled GEMM with shared memory, tensor-core `mma.sync` with f16 inputs and f32 accumulate (PTX ISA, NVIDIA documentation: **unverified**, the page was not opened); FlashAttention-style online-softmax tiling (arXiv 2205.14135, checked) for the non-causal attention in the image kernels.
- **Departure.** CUDA is loaded through the driver API with `dlopen` and kernels are emitted as PTX text by C code, with no SDK (owner's decision, `CLAUDE.md`). A non-standard "exact" precision mode keeps every activation and weight as hi+lo f16 halves and sums three terms for about 22 bits (`docs/render.md`, "Precision modes"); a `fast` mode drops it.
- **Evidence.** `flux-dit selftest` worst block 1.1e-5, final output 5.5e-5 against a float32 reference (CPU path 1.8e-5, tolerance 1e-3); `flux-vae selftest` 3.4e-5 to 5.2e-5; speed 86 s (exact) vs 52 s (fast) at 1024x576. Stable on GPU vs CPU: 63.6 dB PSNR (CUDA) and 67.5 dB (Vulkan) with LCM (`docs/render.md`).
- **Class.** grounded, deviation justified (the unusual part, the exact mode, is measured against a float reference).
- **Action.** None urgent. State in the doc that `fast` is the default for users if it is, with its measured error next to it.

### 3.3 KV cache

- **Established basis.** The transformer attention cache (arXiv 1706.03762, checked); grouped-query attention to shrink it (arXiv 2305.13245, checked); paged cache management in serving systems (arXiv 2309.06180, checked, for multi-request servers).
- **Departure.** Per-architecture sizing (only the 10 attention layers of the hybrid keep it; sliding-window layers use a ring), fp16 storage, 20 KB/token for Qwen3.6; saved to disk so a session resumes. No paging, because one local user is one sequence.
- **Evidence.** KV size arithmetic and a corrected warning (`docs/knowledge.md` lines 137-143: 2.90 GB at 131072, an old 128.85 GB message "was only the arithmetic"). I found no measurement that fp16 KV loses nothing against f32 for this model.
- **Class.** grounded, deviation justified (single-user sizing); fp16 KV quality deviation unmeasured, though fp16 is the common default.
- **Action.** Include a KV-fp16 vs f32 top-1 agreement check in the KL table of 3.1.

### 3.4 Mixture of experts and the expert cache

- **Established basis.** Sparsely-gated MoE (arXiv 1701.06538), Switch Transformers (2101.03961), Mixtral (2401.04088), Qwen3 technical report (2505.09388): all checked. Offloading weights to host memory for a single GPU: FlexGen (2303.06865) and ZeRO-Infinity (2104.07857), checked.
- **Departure.** VRAM is treated as a cache of experts: attention resident, hot experts in slots, cold experts computed on the CPU straight from the mmap'd file, a background loader promoting experts that become hot (`README.md`). An "early router" predicts the next layer's experts one layer ahead (`docs/knowledge.md` line 198: 78.8 / 89.7 / 93.5 in the table at that line; read the table for what the columns mean).
- **Evidence.** Generation 24.9 tok/s on Qwen3.6 on a 4 GB laptop GPU, alternated runs, ties or beats the reference engine on the compared case (`docs/comparisons.md`). That is a strong, honest comparison.
- **Class.** grounded, deviation justified (measured).
- **Action.** None; keep alternating-run discipline.

### 3.5 Gated DeltaNet (the Qwen3.6 hybrid layers)

- **Established basis.** DeltaNet parallelization (arXiv 2406.06484) and Gated Delta Networks (2412.06464), both checked; the model's own technical report is the real specification and was not opened beyond the Qwen3 one.
- **Departure.** Implemented from scratch for CPU and Vulkan; in training, recurrent state is checkpointed every 64 tokens and recomputed in the backward pass.
- **Evidence.** Checked against exact float math in selftest; the lookup decoder's recurrent-state copy and restore is tested for equivalence (section 3.7). I did not find a comparison of katana's outputs against the original model implementation on the same text beyond the logit-difference notes (`docs/comparisons.md` around line 331).
- **Class.** grounded.
- **Action.** Record top-1 agreement with the model's reference implementation on a text sample (part of 3.1's table).

### 3.6 Backends: CUDA, Vulkan, OpenCL, CPU

- **Established basis.** Vulkan compute (GLSL compiled to SPIR-V), CUDA driver API, OpenCL. Official documents were not opened: **unverified**, though nothing here is exotic.
- **Departure.** All loaded at run time from the driver, nothing linked; CUDA is the single exception to "nothing proprietary" (`CLAUDE.md`).
- **Evidence.** Per-backend tables in `docs/comparisons.md` and `docs/render.md`; CUDA/Vulkan vs CPU PSNR given in 3.2.
- **Class.** grounded, deviation justified.
- **Action.** None.

### 3.7 Speculative decoding by copy ("prompt lookup", `src/engine/lookup.c`)

- **Established basis.** Speculative decoding (Leviathan et al., arXiv 2211.17192) and speculative sampling (Chen et al., 2302.01318), both checked; the draft-from-the-prompt variant (`prompt-lookup-decoding` repository, opened; its README text was not read beyond the title).
- **Departure.** Adds exponential back-off driven by measured seconds per token, and a copy/restore of the DeltaNet recurrent state (about 66 MB on Qwen3.6) so a rejected draft can be undone; verified with a batch whose MoE experts may multiply reads.
- **Evidence.** Measured 2026-11-24 on Qwen3.6, CPU: output byte-identical to plain decoding in 5 of 5 runs; acceptance 35% to 61% on copy-heavy tasks; **no speed gain, 0.7x to 0.8x**, so the feature is off. The machine was loaded (load average 10 to 22), the GPU graph was not tried (`docs/optimization-roadmap.md` 7.1).
- **Class.** grounded, deviation justified (the equivalence is measured; the lack of gain is measured and acted on by leaving it off).
- **Action.** Either retest on the Vulkan graph once, on an idle machine, or delete the dead path. Do not leave a measured-negative feature accumulating complexity indefinitely.

---

## 4. Agent and tool use

- **Established basis.** The reason-then-act loop (ReAct, arXiv 2210.03629) and tool-use learning (Toolformer, 2302.04761), checked. Constrained decoding for guaranteed-valid structured output: Outlines (2307.09702), grammar-constrained decoding (2305.13971), both checked. Agent-computer interface design (SWE-agent, 2405.15793, checked) and the finding that simpler pipelines can match agents (Agentless, 2407.01489, checked). Edit-format choice matters per model: the agent project's own page on edit formats (opened, content not read in detail).
- **Departure.** Tools are a lean list of 12; most are found with `ToolSearch` (deferred schemas); the model is trained, or not, on declarations; a grammar-constrained sampler guarantees valid JSON/tool calls; rule-based nudges (plan at the 3rd and 6th call, repeat and failure-streak notes, round blocks of 40). Many of these are engine policies written after observing the owner's sessions.
- **Evidence.** 765 checks in `tools/tooltest/*.sh` with a scripted stand-in, no model. `katana agentlog` gives per-tool calls, failures and identical repeats. The plan marks A2, A3, A8 to A11 as "code done, not measured with a model" and A6 as "written and never exercised" for several tools.
- **Class.** grounded (loop, constrained decoding); deviation unmeasured (the nudges, the lean tool list, the deferral by `ToolSearch`).
- **Action.** Define 30 to 50 fixed tasks (codebench has 12 conversation tasks plus others), run weekly on the real model with nudges on and off; report pass rate and invalid-call rate. One of the nudges (A9 plan-first) is a cheap first ablation.

---

## 5. Specialization (SFT, LoRA/DoRA/PiSSA, curriculum)

- **Established basis.** LoRA (arXiv 2106.09685), QLoRA (2305.14314), DoRA (2402.09353), PiSSA (2404.02948), NEFTune (2310.05914): all checked, and the repo's `docs/specialization.md` cites the same. The "baseline loss before step 1" and "do not write a `.kspec` that does not beat the base" guards are ordinary held-out-validation practice.
- **Departure.** Training is inside the C engine (CPU, partly Vulkan), with its own backward pass for DeltaNet and MoE, nightly windows of 100 to 150, adapters merged at load, tool-call tokens weighted. A "learn from its own use" loop (`katana learn`).
- **Evidence.** Gradient correctness is the best-documented part: against PyTorch autograd, loss 1.59846532 vs 1.59846568, adapter gradients within 4.3e-5 of the largest (`docs/render.md`, FLUX LoRA; the language-side gradcheck is in `tools/traintest/run.sh`). Method comparison is on a tiny random model only, "not evidence for the real model" (`docs/specialization.md`). The KL-anchor result (0.562 vs 0.582 held-out, 3/3 seeds) is on the same tiny model. Whether training changes Qwen3.6 or Gemma 4 behaviour is "not measured".
- **Class.** grounded (methods); deviation unmeasured (everything about the real model).
- **Action.** Single pre-registered experiment: freeze the codebench task set now; run one night of specialization; compare base vs adapter on held-out tasks and on drift text. Publish whichever way it goes. Until then, README language about "learning" should say "implemented and gradient-checked, not yet shown to help".

---

## 6. UIOE (pre-model layer: interpret, classify, route, retrieve, assemble context)

- **Established basis.** This is four well-studied things in sequence. Query understanding and rewriting for retrieval-augmented models (arXiv 2305.14283), routing by question complexity (Adaptive-RAG, 2403.14403) and by model strength (RouteLLM, 2406.18665), retrieval-augmented generation itself (2005.11401), self-reflective retrieval (Self-RAG, 2310.11511); all checked. "Context engineering" as a discipline (Anthropic engineering post of 2025-09-29, opened). Position effects in the context (arXiv 2307.03172, "Lost in the Middle", checked) bear on where the packet goes. BM25 (Robertson and Zaragoza, Crossref record checked).
- **Departure.** The pre-model path is deterministic and rule-based (no LLM call in Interpret/Classify/Route), connectors declare capability bits, results form an evidence graph with confidence grades, and the packet is injected before the model starts; tools remain as facades afterwards.
- **Evidence.** `docs/uioe.md` is candid: "compiled, not measured, no model trained on the block"; "the 10% share and the 260-byte floor are guesses"; the optional expander is "unmeasured". The underlying retrieval study (`docs/investigation-engine.md`) measured: commit bench hit@1 57.8% for BM25 on katana, graph hybrid no better; on 76 questions BM25 R@5 16.9%, best method (sparse activation) 33.0%; **the hop slice is a negative result** (nothing reliably reaches files sharing no word with the question). That is useful honesty, but it means the design's central premise, that structure beats words, is only partly supported.
- **Class.** grounded (the concept), deviation unmeasured (the particular design and all thresholds).
- **Action.** (a) Replay N real sessions with UIOE off and on and count rounds, tokens, and answer-correct. (b) Treat each threshold ("gate", "10% share", "260-byte floor") as a parameter to sweep on the 76-question set, not as a design constant. (c) Call it "UIOE (query understanding, routing and retrieval front end)" in outside-facing text.

---

## 7. Code-intelligence layer

- **Established basis.** Language Server Protocol 3.17 (specification, opened); tree-sitter as the common incremental parser (project site, opened); inverted-index BM25 for identifier search; symbol-addressed edits and tool interfaces designed for models (SWE-agent, 2405.15793, checked; Agentless, 2407.01489, checked).
- **Departure.** No full parsers: three "tolerant" scanner families plus a line-rule fallback, with every relation graded exact, derived or probable (`docs/code-intelligence-architecture.md` 0). Cross-file resolution at query time over memory-mapped sorted segments; no database. LSP optional and lazy. `Edit` by symbol validates before writing (scanner of the family, callers by name, optional LSP) (`src/coder/symedit.c`).
- **Evidence.** The architecture document says of itself: design first, "every number is either quoted or marked estimate", with web checks "marked checked / not verified". Retrieval numbers are those of section 6. `tools/tooltest/symedit.sh` (335 lines) and `tools/editbench/bench.py` (352 lines) exist; their outcomes were not found in the docs I read. LSP is exercised against a fake server only.
- **Class.** grounded, deviation justified (tolerance is argued; real parsers would be heavier); deviation unmeasured (precision of the scanners vs tree-sitter, and the end-to-end effect of validated edits).
- **Action.** Measure scanner precision/recall for definitions and call sites against tree-sitter on three repos and three languages; report editbench pass rate with and without validation; run a real language server end to end once.

---

## 8. Codegen (typed IR, AST, flow analyses, generation)

This is where the repo is furthest from an established path, and the owner's concern applies most.

- **Established basis, by component.**
  - Typed IR and code generation from an AST: standard compiler practice.
  - SSA form: Cytron et al., TOPLAS 1991 (Crossref record checked: "Efficiently computing static single assignment form and the control dependence graph"). Worklist data-flow, SCCP, liveness: textbook (not opened here, **unverified**).
  - Dominators by iteration (the "CHK" the doc cites): Cooper, Harvey and Kennedy: **unverified**, the record was not found on Crossref in two tries and the PDF link returned 404.
  - Hindley-Milner inference: Damas and Milner, POPL 1982 (Crossref record checked: "Principal type-schemes for functional programs"); the Wikipedia overview page was opened.
  - Constrained generation to a grammar: Outlines (2307.09702), GCD (2305.13971), JSONSchemaBench (2501.10868), all checked.
  - Models do worse on low-resource languages: MultiPL-T (arXiv 2308.09895, abstract read: code models "struggle with low-resource languages that have limited training data").
  - Format restrictions can hurt reasoning: "Let Me Speak Freely?" (arXiv 2408.02442, abstract read: "a significant decline in LLMs reasoning abilities under format restrictions", and stricter constraints degrade more).
- **Departure.** Four, and they should be judged separately:
  1. *A new surface syntax (the code spec) instead of a language the model knows.* The repo mitigates it: an indentation pseudocode close to Python/Kotlin, a lenient parser that accepts `def`/`function`/`func`, `&&`/`and`, etc., errors that say what to write, a JSON form. That is a good design for a small model. It is still a format the model has never seen as training data, and the literature above is the warning. The second cited paper cuts both ways: if the spec is also grammar-constrained, reasoning may degrade; if it is not, validity may be low. Neither side has been measured here.
  2. *"The model does not write the final code."* The decision (2026-11-24) is stronger than any measured benefit: the doc's own numbers are spec = 0.86x generated Python but **1.16x idiomatic Python**, and with `test` lines and comments longer still; `raw` tasks are longer than the code. The stated benefits (determinism, multi-target, validation before generation, the model need not know idioms) are real but unmeasured with a model, and multi-target currently means Python only (JS, TS, C backends are planned modules A and B).
  3. *A unified semantic core with a conservative intersection of language rules* (no shadowing, no truthiness, floor division). Sound; it trades expressiveness for portability, and `raw` becomes the escape hatch. 13 of 15 tasks avoided `raw`, **written by the same author who wrote the language**, which the doc itself flags as an upper bound.
  4. *Scope.* HM, CFG/SSA, dominators and the data-flow analyses are planned (modules C and D), not built; the core is not in the `katana.c` build; "the machine was under load" is why no model was run. A heavy analysis stack designed before the first model measurement is a classic order-of-work risk.
- **Evidence.** `tools/codegen/test.sh` (60 checks), the 15 tasks, 53 test cases pass in the generated Python (`docs/codegen.md` 12). Model validity rate: not measured. "Fewer tokens": measured, false (stated honestly in the doc).
- **Class.** typed IR, validator, generated-code checks: grounded. Code spec surface syntax: **ad hoc, needs test or replacement**. "Model never writes final code" policy: **deviation unmeasured**. Analysis stack: grounded in theory, not yet built.
- **Action.**
  1. Before building modules C and D, run module G: 20 natural-language requests, Qwen3.6, three arms (direct Python; JSON under a JSON-schema or grammar constraint; code spec with and without grammar). Metrics: first-try validity, repairs to pass the given tests, tokens, total latency. Decision rule written down in advance: Code spec continues only if it beats "direct Python plus our validators" on tests passed per repair budget.
  2. Keep the typed IR and validators as an independent value: they can validate *Python the model wrote* (undeclared names, arity, missing returns) without any new syntax. That is a cheaper path to most of benefits (3) and (4) in the doc.
  3. Rename (section 11.1).
  4. State plainly in `docs/codegen.md` 1 that multi-language is a plan until a second backend passes the same 13 tasks.

---

## 9. Image engine

### 9.1 Stable Diffusion 1.5 pipeline (CLIP text encoder, UNet, VAE)

- **Established basis.** Latent diffusion (arXiv 2112.10752), CLIP (2103.00020), DDPM (2006.11239), classifier-free guidance (2207.12598), Euler-type samplers in the design-space paper (2206.00364), all checked. Weighted prompt chunks of 75 CLIP tokens are a community convention, not a paper (**unverified** as a standard).
- **Departure.** Written from scratch in C, with GPU kernels for UNet, ControlNet and VAE; prompts longer than 75 tokens are chunked (default 4 chunks); weights `(term:1.3)`.
- **Evidence.** GPU vs CPU: 63.6 dB (CUDA), 67.5 dB (Vulkan), LCM 4 steps; Euler 20 steps tables in `docs/render.md` 317-335. Timings on the laptop card.
- **Class.** grounded.
- **Action.** None.

### 9.2 ControlNet, LCM, img2img strength

- **Established basis.** ControlNet (arXiv 2302.05543), LCM (2310.04378), LCM-LoRA (2311.05556), SDEdit (2108.01073, the basis of img2img strength; the diffusers img2img page was opened), all checked.
- **Departure.** Canny edge extraction is done in C on the CPU; LCM weights are produced by merging the LCM-LoRA into a checkpoint at conversion time. Fidelity "strength" is exposed to the user in the desktop.
- **Evidence.** One qualitative case recorded (a SketchUp reception view became a photographic preview in 317 s: geometry kept, "a burgundy wall came out slate blue", garbled wall text). No metric. Strength 0.75 vs others is not swept.
- **Class.** grounded; deviation unmeasured (strength and guidance defaults).
- **Action.** Sweep strength (for example 0.4 to 0.9) and guidance on a small set of real views, scoring edge preservation (an edge-overlap metric) and CLIP score (arXiv 2104.08718, checked); pick defaults from the knee of the curve.

### 9.3 Checkpoint choice: Realistic Vision, InteriorDesign, XSAchi

- **Established basis.** Community fine-tunes of SD 1.5 distributed as model cards; model quality is judged by metrics such as FID (arXiv 1706.08500), CLIP score (2104.08718), learned preference scores (ImageReward 2304.05977; Pick-a-Pic 2305.01569), all checked, plus human comparison. The Realistic Vision card page was opened (HTTP 200); its text was not read.
- **Departure.** Three checkpoints are selectable. No criterion for inclusion is written down.
- **Evidence.** `docs/render.md` is explicit about InteriorDesign: "the card is a generic marketplace template (the example prompt is a cyberpunk portrait; no trigger words... no training data, no author credit)... probably a merge of SD 1.5 derivatives, and the licence tag is the uploader's own claim. It had 4 downloads when checked. Treat it as usable for a preview, and re-check the licence before depending on it for client work." XSAchi: `git log` says "XSAchi as a Stable model option (non-commercial use)"; the identifier appears only in `desktop-qt/` and I found no model card, hash or evaluation for it in `docs/`.
- **Class.** ad hoc, needs test or replacement.
- **Action.** Fixed set of 20 interior prompts x 3 seeds x checkpoint; report CLIP score and a pairwise blind preference sheet; add the source, hash and licence evidence for XSAchi to `docs/render.md`; remove any checkpoint that does not beat the default on the set or whose licence cannot be shown.

### 9.4 FLUX.2 klein (rectified flow, DiT), reference image, LoRA

- **Established basis.** Rectified flow (arXiv 2209.03003), rectified-flow transformers for images (2403.03206), diffusion transformers (2212.09748), all checked; the model page (`black-forest-labs/FLUX.2-klein-4B`, opened; shows `License: apache-2.0`, a diffusers pipeline, image-editing task). LoRA (2106.09685).
- **Departure.** A from-scratch C implementation of the DiT and VAE, with quantized (Q4_K, Q5_K, BF16) frozen weights and float32 adapters. Reference-image handling for the cascade; `KATANA_FLUX_STRENGTH` starts from a noised reference ("cheap SDEdit"); 1 to 8 steps exposed.
- **Evidence.** DiT and VAE selftests against a float reference at 1e-5 level; adapter gradients vs PyTorch; time 86 s exact / 52 s fast at 1024x576 with reference. Quality of FLUX output vs the Stable preview is not scored. The LoRA has only "an overfit test" and no real data (section 1, gap 6).
- **Class.** grounded (architecture, numerics); deviation unmeasured (reference strength, LoRA benefit).
- **Action.** As gap 6 plus a strength sweep as in 9.2.

### 9.5 Tiled VAE and weight streaming

- **Established basis.** VAE tiling as a memory optimization (diffusers documentation page "memory", opened; it has a "VAE tiling" section; the page text was not read further). Overlapped/blended tiling for diffusion: MultiDiffusion (arXiv 2302.08113, checked) is related but is a different technique (tiling in latent denoising, not only decode). Offloading weights through a size-limited device: FlexGen (2303.06865), ZeRO-Infinity (2104.07857).
- **Departure.** Row-block activations, resident weights up to a budget and streaming of the rest, the DiT weights released before the VAE, blocked mid-attention (S at most 256 MB), "2 tiles" when memory is short.
- **Evidence.** The 5120-token step "fits on the 4 GB card"; time table with 2 tiles at 5 s (exact) and 2 s (fast). I did not find a measurement of seam artifacts between tiles or of overlap size.
- **Class.** grounded, deviation justified (memory fit is measured); seam quality unmeasured.
- **Action.** Compare tiled vs untiled decode of the same latent where untiled fits (a smaller size) with PSNR in the seam band; pick overlap from that.

---

## 10. Briefing selection and prompt compression

- **Established basis.** Lexical relevance scoring (BM25; Robertson and Zaragoza, Foundations and Trends in IR, 2009, Crossref record checked); prompt compression for LLMs (LLMLingua, arXiv 2310.05736, checked); position and relevance effects in long contexts (2307.03172, checked). Image-model prompt limits come from CLIP's 77-token context (2103.00020) and FLUX's text-encoder budget (the repo counts 504 positions).
- **Departure.** `src/render/sdsel.c` cuts units by a hand-set additive score: colour (RGB/hex) +2.5, units +1 to +1.5, colour/material/light/style words +0.8 to +1.5, ALL-CAPS +1, within-text idf up to +2, slight earlier bias; mandatory units (first line, rule markers such as ABSOLUTAS, NUNCA, NEVER, MUST); a 60% cap on mandatory. No learned weights.
- **Evidence.** Selection was exercised on **one real briefing** (the client reception text, 492 of 504 tokens, 45 units cut) and one synthetic; `katana render brief selftest` checks structural properties (short text identical, output words are a subsequence, order kept, old template terms absent). The doc says the real briefing "is the wrong thing to send to an image engine", and the design answer is that the chat model now writes the prompt within the limit; the selector is a fallback. Whether Portuguese prompts work on either engine is "unknown" (`docs/render.md`). Nothing compares the selector against cutting from the end.
- **Class.** ad hoc, needs test or replacement.
- **Action.** (1) Build a 30-brief corpus (Portuguese and English, interiors, illustration, products). (2) Arms: whole text truncated at the limit; our selector; an LLM-assisted compression baseline (for example LLMLingua-style) if feasible offline. (3) Metric: CLIP score of the output to the full brief, and a blind pairwise preference over 3 seeds. (4) Fit or sweep weights on half, test on half; if the selector does not beat truncation, delete the weights and keep the "mandatory + rules + first line" skeleton. (5) Keep the instruction to the model to write short briefs: it is the real fix.

---

## 11. Naming, glossary, and the code spec name

### 11.1 The code spec name (decided)

The earlier three-letter name of the input format already meant, to any outside reader: **Communicating Sequential Processes** (Hoare's process algebra; Wikipedia page opened), **Constraint Satisfaction Problem** (the AI/OR term; the repo's own UIR work also touches constraints), and **Content Security Policy** (a web security header; MDN page opened). Its expansion was a fourth meaning, and it is not a protocol either (it is a language). Decision of the owner (2026-11-24): katana is the name of the engine and `.kspec` stays; no new acronyms. The module is called **codegen** and the input format **"code spec"**, with the file extension `.kcode`. Do **not** use `.kspec`, which is the adapter file of `docs/specialization.md` (the adapter loader opens `*.kspec` by glob).

Also describe it as "a small program-description DSL" on first use. Done: `docs/codegen.md`, `src/coder/codegen*.c`, `tools/codegen/`, `docs/architecture.md` and the architecture page.

Other collisions worth fixing: **"spec"** is used for the adapter file (`.kspec`), the specialization switch (`KATANA_SPEC`, per `docs/optimization-roadmap.md` it is "already the name of the specialization setting" and an older speculative draft switch once used the same name), speculative decoding, `sdspec.c`, and the image spec. Pick one word per meaning.

### 11.2 Glossary

| Our term | Market term | Note |
|---|---|---|
| UIOE | query understanding + query routing + RAG front end ("context engineering" pipeline) | No product or paper uses UIOE; gloss it every time |
| Interpret | query understanding / query rewriting | arXiv 2305.14283 |
| Classify | query classification / intent detection | |
| Route | query routing / retrieval planning (adaptive retrieval) | arXiv 2403.14403 |
| Retrieve | retrieval (lexical BM25 and graph-assisted) | |
| Orchestrate | retrieval pipeline / multi-step retrieval planner | |
| Evidence graph | retrieved-context set with provenance and confidence | |
| Context Builder / context packet | context assembly / context engineering | Anthropic post |
| Facade tools (`Find`, `Symbols`) | tool wrappers over a retrieval backend | |
| Prefix cache | prompt caching / KV-cache reuse | |
| Expert cache, VRAM slots | MoE expert offloading with an LRU/hot-set cache | FlexGen-style offload, arXiv 2303.06865 |
| Early router | expert prefetching by predicted routing | |
| Prompt lookup (`lookup.c`) | prompt-lookup decoding, a form of speculative decoding | arXiv 2211.17192 |
| `.kspec` | LoRA/DoRA/PiSSA adapter file | arXiv 2106.09685 |
| Curriculum | training data schedule / SFT dataset | |
| Teacher mode | distillation / data generation by a stronger model | |
| Briefing / brief | text-to-image prompt | |
| Briefing selector | extractive prompt compression (BM25-like scoring) | |
| Preview / final (Stable then FLUX) | cascade: fast draft, high-fidelity refine; img2img with a reference | |
| Fidelity (strength) | img2img denoising strength | SDEdit, arXiv 2108.01073 |
| Tolerant scanner / line-rule fallback | lightweight (non-compiler) parsers, regex/lexical symbol extraction | contrast: tree-sitter |
| Edit by symbol | symbol-addressed code edit with pre-write validation | |
| Codegen | typed-IR code generator / program-synthesis backend | |
| UIR (UIR-H / UIR-G) | high-level typed IR / graph IR (CFG+SSA) | SSA: Cytron et al. |
| Code spec (codegen) | a small code-description DSL | named in 11.1 |
| `raw` block | escape hatch / foreign code block | |
| Operation Registry | builtin/intrinsic table | |
| Evidence "grade" exact/derived/probable | provenance / confidence labels | |

---

## 12. Test and measurement plan to close each gap

Each item is the smallest experiment that would let the owner decide. All need the owner's permission to lift the "no measurements" rule for that item; all can use the existing harnesses named. Costs are rough effort, not machine time, and are estimates.

| Gap | Experiment | Metric and decision rule (write it before running) | Uses |
|---|---|---|---|
| 1 code spec | 20 NL requests x 3 arms (direct Python; JSON under a grammar; code spec with and without grammar), 3 seeds each, Qwen3.6 on CPU | first-try validity, repairs to pass given tests, tokens, latency. Code spec continues only if tests-passed per repair budget exceeds "direct Python + our validators" | module G of `docs/codegen.md`, `tools/codegen/tasks` |
| 2 Selector | 30-brief corpus; arms truncation / selector / compressor; 3 seeds | CLIP score to the full brief; blind pairwise preference; keep the selector only if it wins on at least a stated margin | `katana render brief`, `--explain-brief` |
| 3 Specialization | one night of `katana specialize` on a frozen task set | codebench pass rate base vs adapter on held-out tasks; drift loss; adopt only if pass rate rises and drift loss does not | `tools/codebench`, `katana train --compare` |
| 4 UIOE | replay 30 recorded real requests with UIOE off/on; plus the 76-question retrieval set swept over gate/share/floor | rounds, tokens, answer-correct (human check); recall at 5 for each parameter setting | `~/.katana` sessions, `tools/findbench`, `katana uioe ask --pack` |
| 5 Checkpoints | 20 prompts x 3 seeds x each checkpoint | CLIP score and blind preference; licence evidence recorded; drop losers | `render generate --sd-model` |
| 6 FLUX LoRA | 20 to 50 approved real pairs; hold out whole scenes | held-out loss and blind pairwise on held-out views, adapter on/off | `katana flux-train`, `--lora` |
| 7 Agent | 30 to 50 fixed tasks, nudges on/off | pass rate, invalid-call rate, rounds; one ablation per nudge starting with A9 | `tools/codebench`, `katana agentlog` |
| 8 Engine quality | KL and top-1 agreement vs float reference on 20k tokens, per backend, KV fp16 vs f32 | KL below a pre-stated bound (for example the one the reference engine's own quantization shows); report even if worse | `katana selftest`, new table in `docs/comparisons.md` |
| 9 Code intelligence | scanner vs tree-sitter on 3 repos x 3 languages; editbench with/without validation; one real LSP server | precision/recall of defs and call sites; edit success rate; the real-server run completes | `tools/editbench`, `tools/tooltest/symedit.sh` |
| 10 Names | name the code spec (done); resolve `spec` overload | grep for the old term returns nothing outside history | docs, `src/coder/codegen*` |
| 3.7 Lookup decoding | retest once on the Vulkan graph, idle machine | decoded tokens per second plain vs lookup, 5 alternations; keep only if faster by more than the noise | `tools/spectest.sh` |
| 9.2, 9.4 Strength/guidance | sweep strength 0.4-0.9, guidance on 10 real views | edge-overlap and CLIP score; defaults at the knee | `render generate` |
| 9.5 Tiling | tiled vs untiled decode of one latent where both fit | PSNR in the seam band as a function of overlap | `flux-vae selftest` style |

---

## 13. References

**Checked: arXiv abstract page opened 2026-11-24, title and first author match.** Leviathan 2211.17192; Chen 2302.01318; Vaswani 1706.03762; Ainslie 2305.13245; Kwon 2309.06180; Dao 2205.14135; Shazeer 1701.06538; Fedus 2101.03961; Jiang (Mixtral) 2401.04088; Yang (Qwen3) 2505.09388; Yang (DeltaNet) 2406.06484; Yang (Gated Delta) 2412.06464; Hu (LoRA) 2106.09685; Dettmers 2305.14314; Liu (DoRA) 2402.09353; Meng (PiSSA) 2404.02948; Jain (NEFTune) 2310.05914; Frantar (GPTQ) 2210.17323; Lin (AWQ) 2306.00978; Yao (ReAct) 2210.03629; Schick 2302.04761; Willard 2307.09702; Geng 2305.13971; (authors not shown by the page for) "Let Me Speak Freely?" 2408.02442; Geng (JSONSchemaBench) 2501.10868; Cassano (MultiPL-T) 2308.09895; Yang (SWE-agent) 2405.15793; Xia (Agentless) 2407.01489; Jiang (LLMLingua) 2310.05736; Liu (Lost in the Middle) 2307.03172; Lewis (RAG) 2005.11401; Asai (Self-RAG) 2310.11511; Jeong (Adaptive-RAG) 2403.14403; Ma (query rewriting) 2305.14283; Ong (RouteLLM) 2406.18665; Rombach 2112.10752; Radford (CLIP) 2103.00020; Zhang (ControlNet) 2302.05543; Luo (LCM) 2310.04378; Luo (LCM-LoRA) 2311.05556; Ho (DDPM) 2006.11239; Ho (CFG) 2207.12598; Meng (SDEdit) 2108.01073; Karras 2206.00364; Liu (rectified flow) 2209.03003; Esser 2403.03206; Peebles (DiT) 2212.09748; Bar-Tal (MultiDiffusion) 2302.08113; Rajbhandari (ZeRO-Infinity) 2104.07857; Sheng (FlexGen) 2303.06865; Heusel (FID) 1706.08500; Hessel (CLIPScore) 2104.08718; Xu (ImageReward) 2304.05977; Kirstain (Pick-a-Pic) 2305.01569; Zhang (LPIPS) 1801.03924; Song (score SDE) 2011.13456.

**Checked: Crossref record (title, first author, year).** Robertson, *The Probabilistic Relevance Framework: BM25 and Beyond*, Foundations and Trends in IR, 2009 (DOI 10.1561/1500000019). Cytron et al., *Efficiently computing static single assignment form and the control dependence graph*, TOPLAS, 1991 (DOI 10.1145/115372.115320). Damas and Milner, *Principal type-schemes for functional programs*, POPL 1982 (DOI 10.1145/582153.582176).

**Checked: page opened (HTTP 200, title seen).** GGUF specification (ggml repository, `docs/gguf.md`); k-quants pull request #1684 in the reference engine's repository; Language Server Protocol 3.17 specification (microsoft.github.io/language-server-protocol); prompt-lookup-decoding repository (`apoorvumang/prompt-lookup-decoding`); diffusers memory-optimization page (has a "VAE tiling" section); diffusers img2img pipeline page; Anthropic, "Effective context engineering for AI agents" (2025-09-29); Wikipedia pages for Communicating sequential processes, Hindley-Milner type system, Static single-assignment form; MDN "Content Security Policy"; tree-sitter documentation site; aider "Edit formats" page; `black-forest-labs/FLUX.2-klein-4B` model page (license shown as apache-2.0); `imagepipeline/InteriorDesign-Checkpoint` page; `SG161222/Realistic_Vision_V5.1_noVAE` page.

## 14. What could not be verified (unverified)

- Cooper, Harvey and Kennedy, "A Simple, Fast Dominance Algorithm": the CHK method cited in `docs/codegen.md` was not confirmed (Crossref returned other records; the PDF link returned 404). Do not quote title, venue or year until checked.
- ACM Digital Library pages returned 403 to automated access; the Crossref records above stand in for the SSA and Damas-Milner citations, but the papers themselves were not read.
- The Foundations and Trends BM25 paper itself (publisher 403, the author's PDF link 404); only the Crossref record was checked.
- NVIDIA PTX ISA and CUDA driver API documentation, Vulkan and OpenCL specifications: not opened; the claims about `mma.sync`, `ldmatrix` and driver loading come from the repo's own docs.
- Worklist data-flow analysis, SCCP (Wegman and Zadeck), Tarjan's algorithm, Canny edge detection: textbook, no source opened.
- The 75-token CLIP chunking convention for long prompts: a community convention, no authoritative source found.
- The InteriorDesign and Realistic Vision licences beyond what `docs/render.md` quotes; the XSAchi model's origin, licence and quality: nothing found in the repo or opened.
- The contents of the opened web pages were sampled (titles and the first lines), not read in full; the aider edit-format page and the prompt-lookup README in particular were opened to confirm they exist, not to confirm any claim.
- Every katana measurement quoted is the repo's own and was not reproduced.
