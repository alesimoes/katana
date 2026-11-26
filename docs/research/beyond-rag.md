# Beyond RAG: retrieval, memory, context and loop control for an engine that owns its internals

Status (2026-11-25): research study, no code, **nothing measured by katana** (owner decision 2026-11-22). Every
proposal for katana in this document is a **hypothesis, NOT MEASURED**, including every "gain" word. Access date for
every URL: 2026-11-25. Function and file names were read in the tree at `06e0803`.

Related: `docs/uioe.md` (the layer these ideas plug into), `docs/research/uioe-context-memory-routing.md` (the
earlier survey; this document does not repeat its verdicts, it goes past them), `docs/investigation-engine.md`
(what was measured about retrieval), `docs/embedding-usage.md` (the embedder as a filter).

## 0. How to read this document

Tags on factual claims:

- **[V]** seen on the cited source during this study, at abstract or search-snippet level. Numbers are quoted only
  when they appeared there; none was reproduced.
- **[L]** recalled from background knowledge, not rechecked. A lead, not a fact.
- **[H]** a hypothesis for katana. Not built, not measured.

Each technique has the same five fields: **What**, **Why it works**, **Local cost**, **Where in katana**,
**Disruptive version [H]**. Other inference engines and products are not named ("reference engine", "a lab's
system"); techniques and papers are cited by technique name and authors/year.

**The thesis.** Published RAG and agent systems are built *outside* the model: they see text in and text out,
through an API. Everything they do costs whole model calls, so they economize calls with extra models (embedders,
rerankers, judges, draft models). katana is the opposite case: one slow model (a step costs 1 to 2.5 minutes,
observed, `docs/uioe.md` 1.1), but the engine is ours, so logits, KV, recurrent state, the sampler, the call grammar
and the attention weights are plain C variables. The inversion that runs through this document: **use the big model
as a sensor and a scorer inside one forward pass, and make the context a branchable data structure, instead of
treating the model as a text function called once per step.** The second asset is deterministic structure nobody
else has next to the sampler: the code index, the evidence graph with verification, and structured logs of our own
sessions.

Cost vocabulary used below (all from existing documents, none measured here): generation 15 to 27 tokens/s; prefill
35 to 5000 tokens/s depending on the path (`docs/embedding-usage.md` section 0); engine-side search 1 to 50 ms; the
embedder 10 to 25 ms per token under load. "A step" always means one model turn.

## 1. Six engine primitives the disruptive versions rest on

Defined once; sections 2 to 6 refer to them as P1 to P6. Each is small in itself. Most of the value of this
document is that several unrelated-looking ideas reduce to the same few primitives.

| # | Primitive | What it is | What exists today | What is missing |
|---|---|---|---|---|
| **P1** | **Trunk checkpoints and rollback** | A named point in the conversation = (KV length, recurrent state snapshot, sampler/grammar state, history index). Rollback = truncate the linear KV to that length and restore the recurrent state. O(state size), no prefill | `lookup.c` already checkpoints the recurrent state per position on the CPU and rolls back on draft mismatch; `prefix_cache.c` saves and loads KV plus recurrent state (`pfx_ssm_io`); `delegate.c` parks the whole main KV on disk | an in-RAM checkpoint stack at turn boundaries; the Vulkan path where the recurrent state lives in VRAM needs a readback copy (the cost `lookup.c` already notes) |
| **P2** | **Closed-set logit probe** | Fork at a checkpoint (P1), prefill a short question whose answer is one token from a fixed set (`A`/`B`/`C`, `yes`/`no`, a tool name), read the logits of that set, roll back. No generation, no trace left in the context | the call grammar already computes vocabulary masks; logits are in hand after every `agent_feed` | the probe function and calibration of each probe against a content-free input |
| **P3** | **Forced prefill / external draft** | Tokens the engine already knows (a predicted call, schema key names, a quoted line) are fed through the batch path instead of being sampled one by one. Two modes: *forced* (engine writes them; used only for syntax the grammar would force anyway) and *draft* (speculative: accepted token by token under the exact acceptance rule of `lookup.c`, so the output distribution is unchanged) | `lookup.c` drafts from text already in the context | a second draft source that is not the context: a playbook, a schema, an index entry |
| **P4** | **Grounded grammar** | The call grammar restricts not only syntax but *values*, from live engine state: paths from the index trie, symbol names from the identifier table, the tool set from the current loop state | `callgrammar.c`: names, keys, required keys and typed values valid by construction; text values are free; lazy checking (mask computed only on rejection) | value automata fed by the index; a per-state tool mask; reading the rejected probability mass as a signal |
| **P5** | **Internal telemetry** | Cheap signals read during generation: token entropy and margin, grammar rejection events, attention mass received per context segment, the MoE router's expert choices | all of these are computed and thrown away | taps, sampled sparsely so they cost almost nothing |
| **P6** | **Session-log mining** | Deterministic offline passes over `~/.katana/agente.log`, the `.ksess` sessions and `uioe.log` that turn our own trajectories into tables: tool transition statistics, failure-to-fix pairs, implicit relevance labels (what was read after a search and cited in the answer) | the logs; `katana learn/export` for training data | the miner and the tables. Note: this builds **engine tables**, not training text (owner rule: transcripts stay out of training) |

Design rule carried over from `docs/uioe.md` principle 12: anything that runs the model (P2, P3) is in the optional
expander slot, born switched off behind one environment variable, and the deterministic path must give the same
result class without it.

## 2. Retrieval

### 2.1 Hybrid lexical + dense retrieval and fusion

- **What.** Run BM25 and a dense retriever, fuse the rankings (reciprocal rank fusion, Cormack et al. 2009; or a
  convex combination of normalized scores, Bruch et al. 2023) [L].
- **Why it works.** The failure sets differ: lexical misses paraphrase and language gaps, dense misses rare
  identifiers and exact strings. A 2026 scaling study reports plain BM25 staying competitive as corpora grow [V,
  title and snippet only]. For code, identifiers make the lexical side unusually strong.
- **Local cost.** BM25 1 to 7 ms. Dense: the query is tens of tokens (under a second); documents are the cost and
  are cached by content.
- **Where in katana.** Already there: per-field BM25, sparse activation over the file graph, the embedder as a
  time-budgeted reorder (`coder/search.c`), RRF with k = 60 in `cs_search_core` and `evgraph.c`.
- **Disruptive version [H]: fusion weights learned from our own reading behaviour.** Every system tunes fusion on
  public benchmarks. katana has P6: for each past `Find`, the log says which candidates the model then read and
  which files the final answer cited. That is a click model with a strong positive (cited after reading) and a
  strong negative (read and abandoned). Fit the 13-feature logistic ranker already specified in
  `docs/investigation-engine.md` 7.3 on these labels, per project, offline, in C. The ranker becomes personal to
  the owner's repositories and phrasing (Portuguese questions, English code) at zero model cost. The earlier
  result that a learned ranker was "optional" was trained on commits, a proxy; these labels are the real task.

### 2.2 Late interaction / multi-vector

- **What.** Keep one vector per token instead of one per chunk; score with MaxSim (ColBERT, Khattab and Zaharia
  2020; PLAID, Santhanam et al. 2022) [L]. Recent work shrinks the index: fixed-dimensional encodings that
  approximate multi-vector similarity (Dhulipala et al. 2024) [V mention]; token *merging* is reported strictly
  better than token *pruning* for the same index size, with hierarchical pooling for offline indexing [V]; a SIGIR
  2025 result keeps quality with 30% of the tokens [V snippet]. "Late chunking" (Günther et al. 2024) embeds the
  long document first and pools per chunk afterwards so each chunk vector carries document context [L].
- **Why it works.** A single vector averages away the one rare token that decides relevance; token-level matching
  keeps it. It is the dense method closest to lexical precision.
- **Local cost.** Index size is the problem (tens of vectors per chunk). Scoring 40 candidates by MaxSim is
  microseconds once vectors exist.
- **Where in katana.** Retrieve, as a second stage over the 40-file active set only (`FG_K_FILE`), never as a
  first-stage index.
- **Disruptive version [H]: multi-vector as a by-product, and late chunking for free.** The embedder runs in our
  engine, so the per-token states exist before mean pooling; today they are discarded. (a) Store 4 to 8 merged
  token clusters per chunk beside the pooled vector in the same content-keyed cache: one forward pass, two indexes.
  (b) Late chunking costs nothing extra for us: embed the file window once, pool per 40-line chunk. Caveat that
  makes this a real hypothesis: the embedder was trained for mean pooling, not for MaxSim; its token states may not
  be discriminative. This is the first thing to check when measuring is allowed, on the 10-snippet sanity set of
  `docs/embed.md`.

### 2.3 Reranking

- **What.** A second, more expensive model reorders the top candidates: cross-encoders, listwise LLM reranking
  (Sun et al. 2023), query-likelihood scoring (Sachan et al. 2022: score = log P(question | passage)) [L].
  In-context reranking (Chen, Jiménez Gutiérrez and Su 2025) scores documents by the attention the query tokens
  pay to them, minus the attention from a content-free query, with no generation, reported better than a
  generative listwise reranker at over 60% lower latency [V]. Follow-ups: one intermediate layer carries most of
  the relevance signal [V]; attention reranking suffers from signal concentration and lexical bias and can be
  re-weighted post hoc (Tian et al. 2026) [V].
- **Why it works.** Joint reading of query and candidate sees interactions a bi-encoder cannot.
- **Local cost.** A dedicated reranker is another model in RAM and another sequential run. Generative reranking
  costs a full step. Attention or likelihood scoring costs one prefill of the candidates, zero generated tokens.
- **Where in katana.** Orchestrate (order within a tier of the evidence graph) and the Context Builder (what makes
  the 1500-token cut).
- **Disruptive version [H]: the reading pass *is* the reranker.** Others pay a prefill to rerank and then a second
  prefill to answer. With P1 and P5 we pay once: feed the packet candidates in a fork, tap the attention from the
  question tokens at one calibrated middle layer, subtract the content-free baseline, roll back, and render the
  final packet with only the winners in the calibrated order. When the final packet is a *prefix* of what was fed
  (winners first), the fork is not even discarded: truncate after the winners and continue, so the reranking
  prefill is the answer prefill. Second form, cheaper to build: query likelihood through P2 on the top 5 only,
  when the tier-4 ("probable, words only") candidates are tied. Both off by default; the deterministic order
  remains the fallback.

### 2.4 Query rewriting, decomposition, HyDE

- **What.** Rewrite the request into better queries (Ma et al. 2023), decompose multi-hop questions into
  sub-questions (Self-Ask, Press et al. 2022; IRCoT, Trivedi et al. 2023), or generate a hypothetical answer and
  embed that (HyDE, Gao et al. 2022) [L].
- **Why it works.** The request and the evidence are written in different vocabularies; a generated bridge moves
  the query to the document side.
- **Local cost.** Each is a generation: 50 to 200 tokens is 3 to 15 s plus the prefill, and it runs before the
  first real step. Deterministic expansion (the Portuguese-English knowledge base, request-word filter) is free and
  already exists.
- **Where in katana.** Interpret (expansion), Route (decomposition into pipeline steps).
- **Disruptive version [H], two inversions.**
  1. **Index-constrained generative retrieval for the hop slice.** The measured weak spot is that 21% of answer
     files share no informative term with the question, mostly Portuguese questions about English code
     (`docs/investigation-engine.md` 3.4); "no word-based method improves it". Generative retrieval (Bevilacqua et
     al. 2022, autoregressive search constrained to substrings that exist in the corpus) [L] fits exactly: with P1
     fork, prefill a one-line cue ("identifiers in this project most related to the request:") and decode a small
     beam of 5 to 8 identifiers **under a grammar that is the project's identifier trie** (P4). The model can only
     emit names that exist; each costs 3 to 8 tokens; total well under a step, and nothing stays in the context.
     The big model does the cross-language semantic hop that BM25 and a 0.3 G embedder cannot, and it cannot
     hallucinate. The identifiers enter `Find` as ordinary anchors with provenance "model-suggested (guess)".
     This is HyDE with the hypothetical document replaced by a guaranteed-real one.
  2. **Gap-driven decomposition.** Published decomposition asks the model for sub-questions. The evidence graph
     already knows what is missing: an anchor with no definition, a sender with no handler, a chain with a hole
     (`rc_limit`, `rc_hyp` in `pipes.c`). Sub-questions are the graph's gaps, generated by C code, each mapped to
     the query that would close it. The model is consulted (P2) only to choose between gap fillers that tie.

### 2.5 Iterative and agentic retrieval

- **What.** Retrieval interleaved with reasoning over several rounds: IRCoT, ReAct-style search loops, and the
  2025 to 2026 "agentic RAG" systems where the model plans queries, judges sufficiency and re-queries [V surveys].
  A 2026 benchmark asks whether a prebuilt graph still matters once an agent drives multi-round retrieval [V
  title].
- **Why it works.** Multi-hop evidence cannot be named in one query; each round's result names the next query.
- **Local cost.** This is the expensive case for katana: every round is a model step. Session 3 of `docs/uioe.md`
  1.1 was exactly this pattern, 18 calls in 26.5 minutes.
- **Where in katana.** Orchestrate runs the rounds deterministically before the model (`pipes.c`, `orch.c`); inside
  the loop the facades continue the same request state.
- **Disruptive version [H]: speculative retrieval ahead of the model.** While the model generates (15 to 27
  tokens/s is an eternity for the CPU threads that are idle between GPU launches, and tools take milliseconds), the
  engine predicts the next read-only calls from two sources: the playbook transition table (P6) and the `next:`
  lines it printed itself. It executes them in the background and holds the results. If the model then emits one
  of them, the result is instant (already true for cheap tools) and, more important, the engine already knows
  whether it would add evidence. If the predicted call would return nothing new (`rs_cover` says everything is
  shown), the engine can say so **before** the call is spent, by biasing P4 (2.7 and 6). Branch prediction applied
  to an agent: tools are cheap, steps are not, so run the tools on speculation.

### 2.6 GraphRAG and evidence graphs

- **What.** Build a graph over the corpus and retrieve along it. Variants: LLM-extracted entity graphs with
  community summaries for global questions (Edge et al. 2024); dual-level graph retrieval (LightRAG, Guo et al.
  2024); personalized PageRank over an entity graph seeded by query entities (HippoRAG and HippoRAG 2, Jiménez
  Gutiérrez et al. 2024, 2025), reported to use far fewer tokens than the LLM-heavy graph builders [V]; a lab's
  "lazy" variant defers all LLM work to query time and reports indexing cost equal to vector indexing [V, vendor
  claim]; LinearRAG (2025) avoids relation extraction [V title].
- **Why it works.** Edges carry the hop that no single chunk contains. The cost is the build: LLM extraction per
  chunk, reported super-linear for some builders [V].
- **Local cost.** LLM-built graphs are out of reach locally (thousands of chunks times a slow model). Deterministic
  graphs cost seconds.
- **Where in katana.** Already ahead of the literature in one respect: the graph is deterministic, typed (calls,
  imports, text keys, co-change), verified against the file at answer time (`verify.c`) and carries confidence 0
  to 100. Sparse activation is the same family as personalized PageRank.
- **Disruptive version [H]: evidence memory, a persistent graph of *verified answer paths*.** Today the evidence
  graph lives on the stack for one request. Persist, per project, the paths that ended in an answer: nodes (file,
  range, symbol), edges with provenance, plus a content hash of each cited range and the request's interpreted
  entities as the key. Effects: (a) a later request that names the same entities gets the path in milliseconds
  with tier-1 rank; the 26-minute investigation is paid once. (b) Invalidation is exact, not time-based: when a
  cited range's hash changes, that edge drops to "(guess)" and `verify.c` rechecks it. (c) Edges the *model*
  established (it read A and B and stated the link, and a later verification pass or a test confirmed it) are
  edges no static analysis finds; they are the only LLM-built part of the graph, and they cost nothing extra
  because the model did the work for a real request. (d) Global questions ("what does this project do") are
  answered from the union of stored paths plus the folder cards, the local stand-in for community summaries. This
  is GraphRAG whose graph is built by use rather than by a batch job, with verification that the published systems
  lack.

### 2.7 Adaptive retrieval: deciding when to retrieve

- **What.** Retrieve only when needed. By classifier before generation (Adaptive-RAG, Jeong et al. 2024); by
  reflection tokens (Self-RAG, Asai et al. 2023); by a retrieval evaluator with fallback (CRAG, Yan et al. 2024);
  by uncertainty during generation: low-probability tokens (FLARE, Jiang et al. 2023), token probability plus
  attention (DRAGIN, Su et al. 2024), internal states (SeaKR, Yao et al. 2024) [V for the last three]. A 2025
  comparison of 35 methods found plain uncertainty estimators (for example maximum token entropy) match the
  complex pipelines on answer quality while being cheaper and better at self-knowledge [V].
- **Why it works.** Retrieval that is not needed costs context and can mislead; retrieval that is needed and
  skipped produces confident errors. Token-level uncertainty is a usable, if imperfect, proxy for "the model is
  guessing".
- **Local cost.** For API-bound systems the uncertainty signals need logprobs and re-generation. For us entropy and
  margin are free (P5).
- **Where in katana.** Classify ("does retrieval help") and Route ("do not retrieve") pre-LLM; LLM Execution during
  the answer.
- **Disruptive version [H]: grounded answer decoding.** Do not ask *whether* the model is uncertain in general;
  watch the only tokens that can be checked. While the final answer streams, a tiny scanner recognizes checkable
  spans: a path, `file:line`, an identifier in backticks, a command. At the end of each span the engine checks it
  against the index and against the request state (was that range ever shown to the model?). Three outcomes: real
  and shown (pass); real but never shown (annotate as unverified, or fetch the three lines and continue); not real.
  For "not real", P1 rolls back to the start of the span, P4 masks the span to names that exist in the active set,
  and generation resumes. This is FLARE with the uncertainty estimator replaced by ground truth, and the retrieval
  query replaced by an index lookup. Entropy and margin (P5) are used only to choose which spans deserve the
  check when the scanner is ambiguous. It closes the "Verification" failure of `docs/uioe.md` 1.2 (session 2
  stated a link between files it never read) at the token where it happens instead of after the answer. A
  rollback-free first version only appends a `Not verified:` line after the answer.

### 2.8 Contextual retrieval

- **What.** Before indexing, prepend to each chunk a short description of where it sits in its document, generated
  by an LLM, then index lexically and densely. A lab reported large reductions in retrieval failures, larger with
  reranking added [L: roughly a third to two thirds; not rechecked].
- **Why it works.** A chunk cut from its document loses its subject ("the function above", "this section"); the
  prefix restores it for both BM25 and the embedder.
- **Local cost.** One LLM call per chunk is days of compute for a repository here. The published recipe relies on
  cheap prefix-cached API calls.
- **Where in katana.** Index build (`coder/`), document prefix of the embedder (`title: ... | text: ...`).
- **Disruptive version [H]: structural context, and model-written context only where use proves it pays.** (a) The
  prefix for a code chunk is computed, not generated: path, enclosing symbol, the two strongest callers and the
  text keys it sends or handles, all in the index already. It goes in the embedder's `title:` field and as a
  low-weight BM25 field. (b) For prose documents the prefix is the heading path. (c) Model-written descriptions
  are made only for chunks that P6 shows were retrieved, read and then abandoned at least N times (the retrieval
  keeps failing there), in idle time (3.2). The expensive method is spent on the measured failure set, not on the
  corpus.

## 3. Agent memory

### 3.1 Hierarchical memory

- **What.** Tiers with different lifetimes: working memory in the context, a paged store managed by the agent
  (MemGPT, Packer et al. 2023), episodic records of sessions, semantic facts, procedural skills; recent systems add
  bi-level designs with automatic evaluation, consolidation and pruning [V, 2026, introduction only] and
  graph-structured or temporal fact stores with validity intervals [L].
- **Why it works.** One store cannot serve "what did I just read", "what did we agree last week" and "how do I do
  this kind of task": they differ in volume, trust and decay.
- **Local cost.** Storage and lookup are free; the cost is any model call used to *write* memory.
- **Where in katana.** All four tiers exist in pieces: the facts notebook (`facts.c`, working), sessions and
  `knowledge.c` (episodic), `memoria.txt` with supersession, tombstones and pins (`memory.c`, semantic), `howto.c`,
  skills and `diagnose.c` (procedural, hand-written). U8 already injects memory as dated, low-confidence evidence.
- **Disruptive version [H]: one memory, four projections of the evidence graph.** Instead of four stores with four
  formats, treat every memory item as an evidence node with the same fields the packet already uses: source,
  date, confidence, what it was verified against, validity. Working memory = this request's graph; episodic = the
  stored answer paths of 2.6; semantic = facts with subject keys; procedural = playbooks (3.4). One fusion and
  ordering code path (`evgraph.c`), one rendering, one staleness rule. The novelty is that memory inherits
  *verification*: a remembered fact about code is rechecked against the file like any other node, which no
  text-blob memory can do.

### 3.2 Consolidation

- **What.** Turn raw episodes into compact durable knowledge: reflection over recent memories (Park et al. 2023),
  offline "sleep-time" processing that uses idle periods to precompute what later queries will need, reported to
  cut test-time compute about 5 times for equal accuracy when queries are predictable from the context (Lin et
  al. 2025) [V]; several 2026 proposals add an explicit sleep phase [V titles].
- **Why it works.** Work moved from the latency-critical path to idle time is free to the user; consolidation also
  deduplicates and resolves conflicts once instead of at every recall.
- **Local cost.** The machine is idle most of the day; the only cost is electricity and that the single model is
  busy (it must yield instantly when the owner types).
- **Where in katana.** A background job class beside `autoindex_dir`; `katana memory`; `learn.c`.
- **Disruptive version [H]: idle time buys *state*, not summaries.** Published sleep-time work produces text. We
  can also produce engine state: (a) **warm trunks**: prefill and save, as prefix-cache files, the per-project
  preamble (brief, folder map, pinned facts, top stored answer paths) so the first request in a project starts
  from a checkpoint instead of a prefill; `pfx_fill` in `prefix_cache.c` already distinguishes a stable part, this extends it
  to a small ladder (tools -> project -> session). (b) **Predicted requests**: P6 gives the owner's most frequent
  request shapes per project; run the UIOE pipelines for them and store the packets (milliseconds each, no model).
  (c) **Model work only for measured failures**: the contextual descriptions of 2.8(c) and the playbook
  distillation of 3.4. Everything in (a) and (b) is deterministic or pure prefill, so it is safe to run unattended;
  it is preempted by any keystroke.

### 3.3 Forgetting, update, conflict

- **What.** Decay by recency and use (the Ebbinghaus-style schedules of several memory systems), utility scores
  with pruning (ProcMEM, 2026: score-based maintenance with a verification gate) [V], supersession and validity
  intervals for facts [L].
- **Why it works.** Recall precision falls as the store grows; stale facts are worse than missing ones for a small
  model.
- **Local cost.** None; bookkeeping.
- **Where in katana.** `memory.c` already supersedes by subject (`#sup`) and tombstones (`#del`).
- **Disruptive version [H]: forgetting by outcome, with a Beta posterior per item.** Count, per memory item and per
  playbook, how often it was injected and whether the request then succeeded by the engine's own signals (no
  failure streak, no user correction detected by `says_already_done`-style cues, verification passed). Keep
  Beta(successes + 1, failures + 1); rank by a lower confidence bound; items whose bound falls below a floor stop
  being injected but are never deleted (owner rule: history is kept). MACLA (2025) tracks procedure reliability
  with Bayesian posteriors [V]; the difference here is that the success signal is deterministic and already logged,
  so no model judges memory quality.

### 3.4 Procedural memory: playbooks learned from trajectories

- **What.** Distill past trajectories into reusable procedures. Memp (Fang et al. 2025): step-level instructions
  and script-like abstractions, with build, retrieve and update policies [V]. ACE (Zhang et al. 2025): the context
  is an evolving playbook maintained by generate, reflect, curate with incremental delta updates, designed against
  "brevity bias" and "context collapse" from repeated rewriting; +10.6% on agent benchmarks, from execution
  feedback without labels [V]. MACLA (2025): 2851 trajectories compressed into 187 procedures with Bayesian
  reliability and hierarchical playbooks with skip, repeat, abort [V]. ReMe (2025) and ProcMEM (2026) add
  refinement and pruning [V]. Earlier: Voyager's skill library, ExpeL, agent workflow memory [L].
- **Why it works.** Most requests repeat a small number of shapes; the successful order of operations is knowledge
  the base model lacks and a 3B-active model will not rediscover each time.
- **Local cost.** The published pipelines use the LLM to reflect and curate after every episode. Injection costs
  context tokens on every request.
- **Where in katana.** Orchestrate (a playbook per task kind already exists as hand-written pipelines in
  `pipes.c`), the `Next:` line of the packet, `loop.c` notes, `howto.c`.
- **Disruptive version [H]: playbooks are compiled into the decoder, not written into the prompt.** Three steps.
  1. *Mine without a model* (P6). Our logs are structured: state = (task kind from Classify, last tool, last
     outcome class, plan state); action = (tool, argument shape). Count transitions and outcomes; the frequent
     successful paths per task kind are the playbooks, each edge with a Beta posterior. The model is needed only
     to *name* a playbook for display.
  2. *Use them in three places that cost zero context tokens.* As a **prior on the tool-name mask** (P4): in state
     S, tools that never led anywhere from S are not forbidden but lose the tie. As a **draft** (P3): if the
     playbook's next call has posterior above a threshold, its tokens are proposed through the speculative path of
     `lookup.c`; the model accepts or rejects token by token under the exact rule, so behaviour is unchanged when
     the model disagrees and the call's tokens come at batch speed when it agrees. As a **prefetch list** for
     speculative retrieval (2.5).
  3. *Show text only on deviation.* The playbook step appears as a `[katana: ...]` note only when the model leaves
     the path and stalls (5.3). ACE's observation that rewritten contexts collapse does not apply: nothing is
     rewritten by a model; counts only grow.
  This removes the standing objection to learned playbooks for small models: that they spend the context they are
  meant to save.

## 4. Context management

### 4.1 Compaction

- **What.** When the window fills: clear old tool results, summarize old turns with the model, keep locators
  instead of bodies, keep a structured notebook outside the summary [V in the earlier survey].
- **Why it works.** Most tokens in an agent context are stale tool output; focused context beats long context.
- **Local cost.** For katana compaction is unusually expensive: the KV is linear and the recurrent state only
  continues from the end, so any removal in the middle forces a recompute from the system prefix (`context.c`
  header).
- **Where in katana.** `context.c` layers 1, 1b, 2, 3; the facts notebook survives summaries.
- **Disruptive version [H]: do not compact; never let the waste enter the trunk.** Compaction repairs a context
  that was allowed to fill with material of unknown value. With P1 the order can be inverted:
  1. **Read on a branch.** A large tool result (a whole-file `Read`, a long `Bash` output, a fetched page) is fed
     in a fork. The model, still in the fork, answers one grammar-constrained question: which line ranges matter
     for the current intent, or "none". Roll back; append to the trunk only those ranges plus a locator for the
     rest. The trunk grows by tens of lines instead of hundreds. Cost: the fork's prefill is paid and discarded,
     plus about 20 generated tokens; saved: every later token of every later step no longer attends over the bulk,
     and the compaction recompute is postponed or never happens. The deterministic selector (`rd_best_windows`,
     the embedder trim of `exec.c`) stays the default; the fork is for results where it has low confidence.
  2. **Subagents as branches.** `delegate.c` today parks the main KV on disk, runs the subagent from position 0
     and reloads. The fork variant keeps the trunk in place, runs the subtask *on top of it* (it inherits
     everything the main agent knows, at zero prefill), and rolls back to leave only the conclusion. Clean-context
     delegation remains for tasks that should not see the conversation.
  3. **Attention-ledger eviction.** When compaction is still needed, evict by evidence of use instead of by age:
     P5 accumulates, per context segment, the attention mass it received from generated tokens (sampled every k
     tokens at one or two layers). Segments the model never looked at since they arrived go first. Published
     compaction policies cannot see this signal.

### 4.2 Prefix caching and KV reuse across calls

- **What.** Keep the KV of an unchanged prefix (every serious engine); design prompts append-only so the cache
  survives [V earlier survey]. Go further by precomputing KV per document chunk and concatenating at query time:
  TurboRAG (Lu et al. 2024), up to 9.4 times lower time to first token, with a fine-tuned model and adjusted
  positions and masks to compensate for missing cross-chunk attention [V]; cache-augmented generation (Chan et al.
  2024) preloads the whole corpus into one cached context [L]; block-attention variants; 2026 work reports that
  naive chunk-KV reuse loses quality for lack of cross-chunk context and patches it by recomputing selected
  tokens [V].
- **Why it works.** Prefill dominates time to first token; the same text re-read is the same computation.
- **Local cost.** Disk per cached token is layers x KV width x 2, plus the recurrent state per checkpoint; to be
  read from `PfxHdr` before sizing anything.
- **Where in katana.** `prefix_cache.c` (system prompt, keyed by model, tokens, context size, backend; survives
  restarts), reused after compaction.
- **Why chunk-KV does not transfer [H].** Independent per-chunk KV assumes attention-only layers. katana's working
  model is a hybrid with a recurrent mixer: the state after a chunk depends on everything before it, and it cannot
  be concatenated. Even for pure attention the published method needs fine-tuning. So the transferable form is not
  "KV per chunk" but **"checkpoints along one trunk"**.
- **Disruptive version [H]: a checkpoint tree keyed by content.** Generalize the prefix cache from one file to a
  trie of token-sequence prefixes with a checkpoint at chosen nodes: tools -> project preamble -> session ->
  request. Consequences: (a) switching project or resuming a session loads the deepest matching node; (b)
  compaction that rewrites history after node N recomputes only from N, not from the system prefix; (c) the
  rollback-based ideas of this document (2.3, 2.4, 2.7, 4.1, 5.4) are all "return to a node"; (d) the Context
  Builder gets a new objective next to relevance: **order the context from most stable to most volatile**, because
  everything after the first changed token is paid again. Published prompt-caching advice says this for static
  prompts; here the builder can optimize it per request since it knows each item's content hash and change rate.

### 4.3 Positional ordering against lost-in-the-middle

- **What.** Models use the beginning and end of a context better than the middle (Liu et al. 2023) [L]. The cause
  was traced to a U-shaped positional attention bias that can be estimated and subtracted, with up to 15 points
  gained on RAG tasks (Hsieh et al. 2024) [V]; a 2026 training-free calibration of positional encodings and a
  survey of mitigations exist [V]. Cheap mitigations: best items first and last, restate the question after the
  evidence [L].
- **Why it works.** The bias is a property of the model, not of the content, so it can be measured once.
- **Local cost.** Ordering is free. Calibration is a one-time set of prefill passes per model.
- **Where in katana.** Context Builder (order inside the packet); the packet already sits at the end of the user
  turn, the best position.
- **Disruptive version [H]: a measured position curve per model, used as a layout table.** In idle time, place one
  fixed probe passage at a grid of depths in a synthetic context and record, by P5, the attention it receives from
  a fixed question (the published calibration procedure, which needs attention access that API users lack). Store
  the curve with the model's fingerprint. The Context Builder then assigns items to slots by matching item
  importance to slot strength, instead of the folk rule "edges good, middle bad". The same curve tells the
  compactor which old segments are effectively invisible already (cheap to drop) and tells 4.1(3) how to normalize
  the attention ledger. Whether a hybrid recurrent-attention model shows the same U shape is unknown; finding that
  out is itself a result.

## 5. Control of the agent loop

### 5.1 State machines and planning

- **What.** Plan first and execute without the model (ReWOO, Xu et al. 2023), plan-and-solve, explicit graph or
  state-machine orchestration, tree search with backtracking (LATS, Zhou et al. 2023) [L].
- **Why it works.** Small models lose the thread over long horizons; an external state holds it.
- **Local cost.** A plan costs a step; tree search costs many, and re-prefills each branch.
- **Where in katana.** `orch.c` is a state machine pre-LLM; `loop.c` has plan notes (`loop_plan_note`), plan mode,
  `TodoWrite`; rounds limit.
- **Disruptive version [H]: the loop state is explicit and the grammar follows it.** Name the loop's states
  (orient, locate, read, edit, verify, answer) and derive the state from observable facts (the facts notebook,
  the plan, the evidence graph), never from the model's claims. Each state owns: its playbook transitions (3.4),
  its tool prior for P4, its stall rule (5.3) and its exit test. Example exit test for `edit`: a file changed and
  the brief's `test` command has not run, already half present as the end-of-request note of `brief.c`. Tree
  search becomes affordable in a narrow form: with P1, two candidate *next calls* can each be executed and scored
  by evidence gained (5.5) and only the better result appended, since read-only tools cost milliseconds and no
  branch needs a re-prefill.

### 5.2 Verifiers

- **What.** Check outputs before accepting them: execution feedback (tests, compilers), self-consistency, a second
  model as judge, chain-of-verification (Dhuliawala et al. 2023) [L]. A 2026 study finds that after its first
  mistake an agent seldom recovers or even notices, and that outcome-level scores hide where verifiers fail [V].
- **Why it works.** Generation and checking are different tasks; a check with ground truth is worth more than any
  amount of self-reflection.
- **Local cost.** A model judge is a step. Deterministic checks are milliseconds.
- **Where in katana.** `verify.c` (packet items), `sy_verify`, the brief's `test`/`done`; nothing checks the
  model's claims.
- **Disruptive version [H].** Grounded answer decoding (2.7) is the verifier for claims about code. For actions,
  the "first mistake is rarely recovered" finding argues for acting at the first failure instead of after a
  streak: see 5.4.

### 5.3 Loop and stall detection

- **What.** Detect repeated calls by hash, repeated outputs, step budgets; 2026 work monitors hidden states with
  linear probes, reporting most help on loop-prone, open-ended tasks and none or negative on structured ones [V].
- **Why it works.** Loops are the dominant failure of small agents and are cheap to detect once defined.
- **Local cost.** None.
- **Where in katana.** `rr_*` (identical call), `rp_*` (failure streak, same result), `bk_*` (blocked programs),
  notes R1 to R6.
- **Disruptive version [H]: detect absence of progress, not presence of repetition.** Hash-based detectors miss the
  loop that varies its arguments (session 3 read the same file in five different ranges). Define progress as the
  **evidence gained per step**: new nodes or edges added to the request's evidence graph, new ranges covered in
  `rs_*`, plan items closed, files changed, a failing check turned passing. A step with zero gain is a stall
  step; k stall steps in a state trigger that state's intervention. This needs no model signal and is exact about
  what "new" means because the request state already tracks what was shown. P5 adds two cheap early warnings: a
  rise in grammar rejections (the model keeps trying something invalid) and collapsing token entropy over a long
  span that repeats earlier output (the "same sentence three times" of session 1).

### 5.4 Reflection

- **What.** After a failure the model writes a lesson that is fed back (Reflexion, Shinn et al. 2023); self-refine
  loops [L].
- **Why it works.** It converts an error signal into text the model can condition on. It also leaves the failed
  attempt in the context.
- **Local cost.** A step per reflection; and the failed attempts stay in the window, where a small model tends to
  imitate them.
- **Where in katana.** `diagnose.c` writes the "reflection" deterministically as a hint; notes R4, R5.
- **Disruptive version [H]: rollback instead of reflection.** When a stall or failure streak is detected, the usual
  remedy appends a note to a context that still contains the three failed attempts, the strongest prompt in the
  window for a fourth. With P1: return to the checkpoint before the first failed attempt and append one
  engine-written line: what was tried, the exact error, what is ruled out (from the facts notebook, so no model
  summary is involved). The failed tokens leave the window; the lesson stays. The session log keeps everything.
  It is backtracking search where the engine, not the model, writes the pruned branch's epitaph, at O(1) cost.
  Open question for the owner: the visible transcript then differs from the model's context; the desktop should
  show the pruned attempts greyed, not hide them.

### 5.5 Process reward

- **What.** Score intermediate steps instead of final outcomes: process reward models for reasoning (Lightman et
  al. 2023), step-level verifiers; 2026 work replaces the separate reward model with small probes on the acting
  model's hidden states (under 10 M parameters), or uses probes as dense rewards, with the caveat that probes
  degrade under contaminated contexts [V].
- **Why it works.** Dense signals let a controller stop or redirect early.
- **Local cost.** A separate reward model does not fit. A linear probe on a hidden state already computed is free
  at run time; its training needs labels.
- **Where in katana.** Orchestrate (stop rules), `loop.c`.
- **Disruptive version [H]: a process reward with no model and labels for free.** Reward of a step = evidence
  gained (5.3) minus its cost in tokens added to the trunk. It is computable for every step of every logged
  session (P6), which yields a labelled set of (hidden state at the end of the call, reward) pairs without any
  annotation. A linear probe on that state, trained offline in C (`train.c` exists), predicts "this call will gain
  nothing" before the tool runs. Start with the deterministic reward alone; the probe is a later, optional layer,
  and the contamination finding says to trust it least exactly when the context is already polluted.

### 5.6 Tool routing and selection with many tools

- **What.** Accuracy falls as the candidate set grows, for all models and steeply for small ones [V]. Remedies:
  retrieve a tool subset per request, hierarchical or latent tool retrieval that keeps the registry out of the
  context [V], a search tool that loads schemas on demand, adapting schemas to the small model rather than the
  model to the schemas (2025: enum-style constraints raised single-turn tool selection of a 7B model from 49.5%
  to 78.1% on one benchmark, but sometimes hurt on another by pushing the model to call a tool when none fits)
  [V]. A 2026 paper shows part of the reported gain of constrained tool selection is positional artefact [V].
- **Why it works.** Tool descriptions compete for attention like any distractor; removing irrelevant ones removes
  the confusion.
- **Local cost.** Each on-demand schema lookup is a full step (minutes).
- **Where in katana.** Route (the packet's `Next:` names the tool); `prompt.c` short declaration plus `ToolSearch`
  (owner decision 2026-11-25: stays as is until training); MCP cards.
- **Disruptive version [H]: just-in-time schemas through the grammar.** The grammar knows every tool's schema even
  when the prompt shows only its name. So a tool the model has never seen described can still be called
  correctly: once the model emits the name, the engine *writes* the required key syntax itself (P3 forced mode:
  these tokens are fully determined by the schema) and the model generates only the values, with a one-line
  description of each parameter injected as the key is opened. The `ToolSearch` round trip, a whole step, is
  replaced by a few forced tokens. Compatible with the current declaration (nothing changes in the prompt), and it
  is a natural fit for MCP servers with many tools. The abstention finding above is the constraint: the "no call"
  exit must always stay open, and the tool prior must only reorder, never force.

### 5.7 Learning tool use from failures

- **What.** Collect failed calls and corrections and feed them back: as in-context lessons (ExpeL), as refined tool
  documentation, or as fine-tuning data filtered by verification [L]; ReMe reports gains on tool-calling
  benchmarks from distilled experience [V].
- **Why it works.** Tool misuse is systematic: the same wrong argument shape recurs until something changes.
- **Local cost.** Fine-tuning is the expensive route and is governed by the curriculum rules.
- **Where in katana.** `diagnose.c` (hand-written failure hints), `rp_*`, `katana learn`.
- **Disruptive version [H]: a mined failure-to-fix table and call repair at the dispatcher.** From P6, for every
  failed call find the next successful call to the same tool within the request: (error signature, argument
  shape) -> (what changed). Frequent pairs become three things, none of which is training text: new rows for
  `diagnose.c` written by the miner and reviewed by the owner; **pre-dispatch repair** for the unambiguous ones
  (the observed `mode=refs` with a sentence becomes a normal search, and the result says so, saving the failed
  step); and negative priors for P4 so the bad shape loses ties. The same table marks which misuse is frequent
  enough to deserve a curriculum scenario, which is the part that does go to training, as a convention.

## 6. Grammar-constrained decoding for tool calls

- **What.** Mask the vocabulary at each step so the output stays in a formal language: regular or context-free
  constraints compiled to token-level automata with precomputed masks (Willard and Louf 2023), pushdown automata
  with mask caches for full JSON Schema at negligible overhead (Dong et al. 2024) [L]. Known problems: masking
  distorts the distribution relative to the model's own conditional over valid strings (grammar-aligned decoding,
  Park et al. 2024); format restrictions over the whole output degrade reasoning (Tam et al. 2024) [L]; token
  boundaries misalign with grammar terminals; constraints can force a call when none fits [V, 5.6].
- **Why it works.** A 3B-active model's commonest tool failure is malformed syntax, and syntax is the one thing
  that can be guaranteed mechanically.
- **Local cost.** Near zero with lazy checking.
- **Where in katana.** `callgrammar.c`: three native formats, names and keys from the live tool set, required keys
  enforced, typed values by the JSON grammar of `forward.c`, text values free, reasoning unconstrained (only the
  call segment is constrained, the recommended practice), lazy mask.
- **Disruptive versions [H].** The published state of the art stops at syntax and schema. Owning the index and the
  loop allows four further steps:
  1. **Semantic values.** `path` of `Read`/`Edit` is constrained to the trie of paths that exist (plus an explicit
     escape for new files in `Write`); `name` of `Symbols` to the identifier table; enum-like arguments to their
     set. A call to a file that does not exist becomes unrepresentable. Lazy checking keeps the cost at zero when
     the model is right.
  2. **State-conditioned sets.** The allowed tool set and argument ranges depend on the loop state (5.1): in plan
     mode the read-only lock is enforced at the token level instead of by an error after the call; an identical
     call that `rr_check` would refuse is refused before it is written, and the refusal costs one resampled token
     instead of a step.
  3. **The grammar as a sensor.** Each rejection is logged with the probability mass that was masked. High masked
     mass at the tool name means the model wanted a tool it does not have (a gap for `ToolSearch` or training);
     at a path, that it believes in a file that does not exist (a hallucination caught at the token, feeding
     2.7); repeated rejections in one call feed stall detection (5.3). Published constrained decoders discard
     this signal.
  4. **Distribution-aware fallback.** When the masked mass exceeds a threshold, resampling inside the mask
     produces a call the model did not "mean" (the distortion problem). Then do not force: close the call, and let
     the engine state what was not possible. Forced validity is right for syntax and wrong for intent.

## 7. Ranking and order of implementation

Scores are judgment, not measurement. Gain: effect on steps per request and on wrong answers. Effort: S days, M one
to two weeks, L more. Every item is born off behind one variable.

| Rank | Idea | Section | Primitives | Expected gain | Effort | Why this rank |
|---|---|---|---|---|---|---|
| 1 | Session-log miner: transition table, failure-to-fix table, implicit relevance labels | 5.7, 3.4, 2.1 | P6 | high (feeds five other items; call repair saves a step each time) | S | pure offline C over logs that exist; no model, no risk to the loop |
| 2 | Stall detection by evidence gained per step | 5.3, 5.5 | none | high on the observed failure (rereads, varied loops) | S | request state already tracks what was shown; replaces a count with a meaning |
| 3 | Grounded grammar: values from the index, state-conditioned sets, rejection as signal | 6 | P4 | high (invalid calls and repeats cost a token, not a step) | M | `callgrammar.c` and the index exist; lazy checking keeps cost nil |
| 4 | Trunk checkpoints and rollback on failure | 5.4, 4.2 | P1 | high (removes failed attempts from the window) and enabling for 5 to 9 | M | the per-position checkpoint and state I/O exist on the CPU path; the GPU-state path is the work |
| 5 | Evidence memory: persisted verified answer paths with hash invalidation | 2.6, 3.1 | none | high for repeated topics (minutes to milliseconds) | M | graph, verification and trace exist; needs a store and a key |
| 6 | Branch reads and branch subagents | 4.1 | P1, P2 | high on long sessions (context grows by ranges, compaction postponed) | M | needs 4; `delegate.c` shows the save and restore discipline |
| 7 | Index-constrained generative retrieval for the hop slice | 2.4 | P1, P4 | medium to high on the 21% no-shared-term cases, nothing else addresses them | M | needs 3 and 4; small generation, no context cost |
| 8 | Just-in-time schemas and playbook drafts | 5.6, 3.4 | P3, P6 | medium (saves `ToolSearch` steps; call tokens at batch speed) | M | `lookup.c` already implements exact acceptance; gain depends on the prefill path speed |
| 9 | Grounded answer decoding | 2.7 | P1, P4, P5 | medium to high on wrong answers; first version (append `Not verified:`) is S | M | the check is an index lookup; the rollback form needs 4 |
| 10 | Attention taps: position curve, read-pass reranking, ledger eviction | 4.3, 2.3, 4.1 | P5 | uncertain (unknown bias shape in a hybrid model) | L | taps in every attention path including the GPU; highest research value, lowest certainty |

Order of implementation, with the reason for each dependency:

1. **Miner (rank 1).** No dependency; its tables are the data every later item is tuned on, and the only source of
   evidence available while measuring is suspended.
2. **Evidence-gain stall detector (rank 2).** Uses the request state as is; defines the progress signal that items
   4, 6 and 8 need as their trigger.
3. **Grounded grammar, steps 1 to 3 of section 6 (rank 3).** Independent of checkpoints; starts producing the
   rejection signal for the miner.
4. **Checkpoint stack on the CPU-KV path, then failure rollback (rank 4).** First as a pure mechanism with a
   self-check (rollback then re-feed must reproduce the logits), then the rollback policy driven by step 2.
5. **Evidence memory (rank 5).** Parallel to step 4; touches only `src/uioe/`.
6. **Closed-set probe (P2) and branch reads (rank 6).** The first consumer of forks; branch subagents after.
7. **Index-constrained generative retrieval (rank 7).** Reuses 3 and 4 directly.
8. **Draft sources for `lookup.c`: schema keys first (forced), then playbook calls (speculative) (rank 8).**
9. **Grounded answer decoding (rank 9):** the append-only form can ship right after step 3; the rollback form here.
10. **Attention taps (rank 10)**, starting with the one-time position curve, which is useful alone.

Left outside the ranking as open research bets: multi-vector from the embedder's token states (2.2; depends on an
unverified property of the embedder), learned fusion weights (2.1; trivial once the miner has labels, value
unknown), the hidden-state probe as process reward (5.5), and **router traces as a free fingerprint**: in a
mixture-of-experts model the router's expert choices per layer are a sparse, discrete signature of what the model
is doing with a span. Averaged over the request tokens they might serve as a zero-cost request embedding for
Classify, as a loop signal (the same expert trajectory recurring), and as the prefetch list for the expert tiers
per loop state. Nothing in the literature read for this study uses the router this way; it may carry no usable
signal.

## 8. Risks and what would falsify each bet

- **Rollback changes what the model saw (P1).** A model that cannot see its failed attempts may repeat them unless
  the epitaph line is good. Falsified if repeat rate after rollback is not lower than after a note.
- **Forks cost prefill.** On the slow prefill path a discarded fork is seconds to minutes. Branch reads pay only
  when the trunk saving exceeds the discarded prefill over the remaining steps; with few remaining steps they
  lose. The policy must estimate remaining steps (the playbook gives a prior).
- **Probes are uncalibrated by default.** A yes/no logit comparison carries the model's prior for "yes". Every
  probe needs the content-free baseline subtraction that the attention-reranking work uses, and must be allowed to
  answer "no signal".
- **Constraining values can hide intent.** If the model wants a file that does not exist, forcing the nearest
  existing path creates a confident wrong call. Section 6 step 4 is the guard; it needs a threshold nobody has
  measured.
- **Mined playbooks encode the owner's past, including bad habits of the current model.** Posteriors must be
  conditioned on outcome, and a playbook mined before a retrain is suspect after it: key the tables by model
  fingerprint and rebuild (owner rule: no migration).
- **Attention is not explanation.** Attention received is a weak proxy for use; the 2026 critiques (signal
  concentration, lexical bias) apply. Rank 10 reflects this.
- **Everything here is single-user.** Tables learned from one person's sessions are small. Beta posteriors with
  honest priors and "do nothing below N observations" are mandatory, not optional.
- **Hybrid recurrent state.** Several published results assumed attention-only models (chunk KV, position bias
  curves, attention reranking layers). Each may behave differently with the recurrent mixer; none was checked.

## 9. Sources

Verified at abstract or snippet level on 2026-11-25 [V]:

- Agentic RAG surveys: https://arxiv.org/abs/2501.09136 ; https://arxiv.org/abs/2603.07379 ; graph versus agentic
  search benchmark: https://arxiv.org/abs/2604.09666 ; graph RAG survey: https://arxiv.org/abs/2408.08921
- Graph retrieval cost and comparisons: HippoRAG 2 https://arxiv.org/abs/2502.14802 ; when to use graphs
  https://arxiv.org/abs/2506.05690 ; GraphRAG-Bench https://arxiv.org/abs/2506.02404 ; LinearRAG
  https://arxiv.org/abs/2510.10114 ; BM25 scaling study https://arxiv.org/abs/2607.26497
- Late interaction compression: https://arxiv.org/abs/2603.22434 ; https://arxiv.org/abs/2603.09933 ;
  https://arxiv.org/abs/2602.02827 ; https://arxiv.org/abs/2504.12778 ; https://arxiv.org/abs/2403.13291
- Attention-based reranking: https://arxiv.org/abs/2410.02642 ; https://arxiv.org/abs/2602.19969 ;
  https://arxiv.org/abs/2602.22591
- Adaptive retrieval and uncertainty: https://arxiv.org/abs/2501.12835 ; SeaKR https://arxiv.org/abs/2406.19215
- KV reuse: TurboRAG https://arxiv.org/abs/2410.07590 ; https://arxiv.org/abs/2608.07458 ;
  https://arxiv.org/abs/2601.12904
- Position bias: https://arxiv.org/abs/2406.16008 ; EACL 2026 findings paper 120 (calibration of positional
  encodings)
- Procedural memory: Memp https://arxiv.org/abs/2508.06433 ; ACE https://arxiv.org/abs/2510.04618 ; MACLA
  https://arxiv.org/abs/2512.18950 ; ProcMEM https://arxiv.org/abs/2602.01869 ; ReMe
  https://arxiv.org/abs/2512.10696 ; bi-level memory with pruning https://arxiv.org/abs/2606.06787
- Sleep-time compute: https://arxiv.org/abs/2504.13171 ; https://arxiv.org/abs/2605.26099 ;
  https://arxiv.org/abs/2606.03979
- Step-level failure signals and probes: https://arxiv.org/abs/2605.17877 ; https://arxiv.org/abs/2604.13759 ;
  https://arxiv.org/abs/2604.19775 ; https://arxiv.org/abs/2609.17930 ; https://arxiv.org/abs/2511.06209
- Tool selection and constraints: https://arxiv.org/abs/2510.07248 ; https://arxiv.org/abs/2603.11495 ;
  https://arxiv.org/abs/2606.12451 ; https://arxiv.org/abs/2608.13959 ; https://arxiv.org/abs/2606.06566 ;
  https://arxiv.org/abs/2610.31483

Recalled, not rechecked [L]: reciprocal rank fusion (Cormack et al. 2009); score fusion analysis (Bruch et al.
2023); ColBERT (Khattab and Zaharia 2020) and PLAID (Santhanam et al. 2022); late chunking (Günther et al. 2024);
query likelihood reranking (Sachan et al. 2022); listwise LLM reranking (Sun et al. 2023); query rewriting (Ma et
al. 2023); Self-Ask (Press et al. 2022); IRCoT (Trivedi et al. 2023); HyDE (Gao et al. 2022); generative retrieval
with corpus-constrained decoding (Bevilacqua et al. 2022); graph RAG with community summaries (Edge et al. 2024);
LightRAG (Guo et al. 2024); Adaptive-RAG (Jeong et al. 2024); Self-RAG (Asai et al. 2023); CRAG (Yan et al. 2024);
FLARE (Jiang et al. 2023); DRAGIN (Su et al. 2024); contextual retrieval (a lab's 2024 engineering post); MemGPT
(Packer et al. 2023); generative agents (Park et al. 2023); cache-augmented generation (Chan et al. 2024); lost in
the middle (Liu et al. 2023); ReWOO (Xu et al. 2023); LATS (Zhou et al. 2023); chain-of-verification (Dhuliawala et
al. 2023); Reflexion (Shinn et al. 2023); process supervision (Lightman et al. 2023); guided generation with
finite-state machines (Willard and Louf 2023); pushdown-automaton structured generation (Dong et al. 2024);
grammar-aligned decoding (Park et al. 2024); format restrictions and reasoning (Tam et al. 2024).
