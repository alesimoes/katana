# UIOE research: context, memory, planning, routing and robustness for small local agents

Status: research study, no code, nothing measured by katana (owner decision 2026-11-22).
Access date for every URL below: 2026-11-23.

Unlike the rest of `docs/`, this is a reference study: it names the projects it studies, with URLs.

## 0. How to read this document

Every factual claim carries a tag:

- **[V]** VERIFIED: seen on the cited source during this study (usually the arXiv abstract page or the
  official post). Numbers are only given when they appeared on that page. Where only the abstract was
  readable, the entry says so and gives no numbers the abstract did not contain.
- **[L]** RECALLED: from the author's background knowledge, not rechecked. Treat as a lead, not a fact.
- **[design]** a proposal for katana, not a claim about any source.

Verdicts for the katana UIOE: **TAKE** (adopt nearly as is), **ADAPT** (adopt the idea, change the mechanism
to fit local, small, slow, deterministic, no embeddings), **REJECT** (does not fit, with the reason).

Scope note. The owner pasted the real UIOE architecture:

```
USER INPUT -> UIOE (Interpret, Classify, Route, Retrieve, Orchestrate)
           -> SOURCES (Filesystem, Web Search, Financial APIs, Code Repositories, Legal Sources, Databases)
           -> CONTEXT BUILDER (select and organize relevant data)
           -> LLM EXECUTION (request + prepared context)
```

So the UIOE is a general, PRE-LLM layer with pluggable connectors; code is only one source. Sections 1 to 12
cover the original theme (context, memory, planning, routing, tools, loops, verification, evals, security).
Sections 13 to 17 cover the scope adjustment: interpretation and classification, source routing and
orchestration, connectors, the context builder, and the evidence that deterministic pre-processing helps small
models. Sections 18 to 21 are the lessons, the missing layers, open questions and sources.

Constraints that drive every verdict (from the brief and CLAUDE.md):

1. The model is Qwen3.6-35B-A3B (MoE, about 3B active), local, 1 to 2 minutes per step. Every saved step is
   worth minutes. Token price is irrelevant; wall-clock steps and prefill are what matter.
2. Context up to 131k tokens, with katana's own compaction and a prefix cache. Breaking the prefix is expensive.
3. No embeddings and no external service in the core. The local model may be an optional expander.
4. Deterministic and explainable: every decision of the layer must be reproducible and printable
   ("why did you consult X").
5. Existing katana pieces: fact memory (remember/recall, text), docsel (document selection per request),
   session history (knowledge), compaction, prefix cache, `[katana: ...]` engine notes in tool results,
   repetition and consecutive-failure detection, mandatory TodoWrite plan, subagents, skills, and the
   learning loop (learn/export/train).

---

## 1. Context engineering for agents

### 1.1 The ideas and the evidence

**Context rot and focused prompts.**
- Chroma's "Context Rot" study tested 18 models and reports that performance gets increasingly unreliable as
  input grows, even on simple tasks; one distractor hurts, four compound it; on LongMemEval, focused prompts of
  about 300 tokens beat full prompts of about 113k tokens for every model family tested [V]
  https://www.trychroma.com/research/context-rot
- Same study: needle position mattered little in their NIAH variant, and shuffled haystacks beat coherent ones
  [V]. This tempers the older "position is everything" reading.
- Liu et al., "Lost in the Middle": accuracy is highest when the relevant information is at the start or end of
  the input and drops when it is in the middle, even for long-context models [V] https://arxiv.org/abs/2307.03172
- Anthropic's engineering post treats context as a finite resource with diminishing marginal returns [V]
  https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents

**Techniques named in that Anthropic post [V]:**
- Compaction: summarize near the limit and restart with the summary; keep decisions, open bugs, implementation
  details; drop redundant output; tune for recall first, then precision.
- Tool-result clearing: the lightest compaction, removing raw outputs of old tool calls (observation masking).
- Structured note-taking: the agent writes notes to persistent storage outside the window and reloads them.
- Sub-agents with clean windows that return short summaries (the post says often 1,000 to 2,000 tokens).
- Just-in-time retrieval: keep lightweight references (paths, queries, links) and load on demand; hybrid with
  up-front files such as CLAUDE.md.

**Manus lessons** https://manus.im/blog/Context-Engineering-for-AI-Agents-Lessons-from-Building-Manus [V]:
- KV-cache hit rate is the key production metric; stable prefix, append-only context, deterministic
  serialization; a timestamp at the start of the system prompt breaks caching. (The post cites a 10x price gap
  between cached and uncached input; irrelevant to local cost, but the latency logic is identical: prefill is
  the dominant cost, input to output about 100:1.)
- Mask tools by constraining decoding instead of adding or removing definitions mid-task (changing tool
  definitions invalidates the cache and confuses the model).
- Files as externalized memory; compression must be restorable (keep the path or URL, drop the body).
- Recite the plan (todo.md rewritten as work proceeds) to keep goals in recent attention (about 50 tool calls
  per task).
- Keep failed actions and stack traces in context so the model updates its beliefs.
- Avoid few-shot ruts: add small structured variation so the agent does not copy its own pattern blindly.

**Prompt compression.** LLMLingua reports up to 20x compression with little performance loss on GSM8K, BBH,
ShareGPT, Arxiv-March23 (abstract only; per-dataset losses not seen) [V] https://arxiv.org/abs/2310.05736.
It needs a small LM to score tokens; it is a lossy, model-dependent step.

### 1.2 Cost
Observation masking and file offloading are nearly free. Summarizing compaction costs one model step (minutes
locally) and risks losing facts. Re-ordering context is free. Prompt compression with a second LM costs an
extra model load.

### 1.3 Verdicts for katana

| Technique | Verdict | Why |
|---|---|---|
| Treat the window as scarce, send focused evidence | TAKE | Core UIOE thesis; Chroma shows focused prompts win. |
| Tool-result clearing with restorable stub (path, query, hash) | TAKE | Deterministic; cache-friendly if done at compaction boundaries, not every turn. |
| Append-only context and stable prefix | TAKE | Katana has the prefix cache; the UIOE must only append, never rewrite earlier messages. |
| Mask tools by decoding constraint, not by editing the tool list | ADAPT | Katana controls the sampler, so a grammar mask per phase is natural. |
| Plan recitation | TAKE | TodoWrite exists; the UIOE re-emits the current step in the evidence package footer. |
| Keep failures in context | TAKE | UIOE stores a compact failure ledger instead of the raw trace. |
| LLMLingua-style token pruning | REJECT for core | Needs another model, lossy, not explainable. Prefer extractive selection (section 16). |
| Lost-in-the-middle ordering | ADAPT | Put the request and strongest evidence at the start and end; cheap and deterministic. |

---

## 2. Agent memory

### 2.1 Systems studied

**MemGPT / Letta**: OS-style virtual context management; memory tiers, the model moves data between them with
function calls, interrupts for control flow [V] https://arxiv.org/abs/2310.08560. The abstract page had no
numbers. Zep reports 93.4% for MemGPT on Deep Memory Retrieval (below) [V].

**Mem0**: extract, consolidate and retrieve salient facts; graph variant. On LOCOMO: 26% relative improvement
over OpenAI's memory system by LLM-as-judge, 91% lower p95 latency and more than 90% token savings versus
full-context [V] https://arxiv.org/abs/2504.19413. Authors' own results on one benchmark.

**Zep / Graphiti**: temporally-aware knowledge graph engine merging conversation and structured data while
preserving history. DMR 94.8% versus 93.4% for MemGPT; LongMemEval up to 18.5% better accuracy and 90% lower
latency versus baselines [V] https://arxiv.org/abs/2501.13956 (authors' own results). The transferable idea:
facts have validity intervals, and a newer fact supersedes an older one without deleting it [L for the detail].

**A-MEM**: Zettelkasten-style; each memory becomes a note with context, keywords, tags, linked to related
notes; new notes can update old ones [V] https://arxiv.org/abs/2502.12110. Abstract has no metrics.

**Reflexion**: verbal reflections stored in an episodic buffer guide later trials; 91% pass@1 on HumanEval vs
80% for GPT-4, as reported by the authors [V] https://arxiv.org/abs/2303.11366.

**File memory** (CLAUDE.md, AGENTS.md): human-readable, versionable files loaded at start; Anthropic describes
loading CLAUDE.md up front while using glob and grep just in time [V] (context-engineering post above).

**Taxonomy [L]:** episodic (what happened in a session), semantic (stable facts), procedural (how to do
things: skills, conventions). Katana maps them: session history = episodic, remember/recall = semantic,
skills and learned conventions = procedural.

### 2.2 Forgetting, update, conflict (the hard part)
- Public systems mostly add and consolidate; explicit conflict handling is thin. Zep's temporal validity is the
  clearest model [L for details]. Mem0 describes consolidation, not a consistency guarantee [V, abstract only].
- A deterministic scheme that fits katana [design]: every fact has `subject`, `predicate`, `value`, `source`,
  `observed_at`, `valid_from`, `valid_to`, `confidence_class` (user-stated, tool-observed, model-inferred).
  A new fact with the same subject and predicate and a different value closes the old interval and keeps it as
  history. User-stated beats tool-observed beats model-inferred. Facts about files carry the file hash and are
  marked stale when the hash changes. Decay is by staleness check, not by time alone.

### 2.3 Verdicts

| Idea | Verdict | Note |
|---|---|---|
| Three tiers (working, session, durable) with explicit moves | ADAPT | Katana has the pieces; the UIOE owns the "what moves where" policy and prints it. |
| Model self-edits memory with function calls (MemGPT) | ADAPT | Allow it, but writes go through a UIOE validator (schema, provenance, conflict check). A 3B-active model will write sloppy facts otherwise. |
| LLM extraction and consolidation of facts (Mem0) | ADAPT | Optional, by the local model off the hot path (idle time or end of session); never blocking a step. |
| Vector retrieval of memories | REJECT | No embeddings. Use keyword and structured lookup (section 16). |
| Temporal knowledge graph | ADAPT | Take validity intervals and supersession; implement as a flat table with indexes, not a graph database. |
| A-MEM link-on-write | ADAPT | Link by shared entities and file paths (deterministic), not by embedding similarity. |
| Reflexion episodic lessons | TAKE | Matches the learning loop; store "lesson" facts tied to a task signature. |
| Memory in plain files | TAKE | Already katana's style; keep it diffable. |

---

## 3. Planning

### 3.1 Systems and evidence
- **ReAct**: interleave reasoning and actions. Authors report absolute success-rate gains of 34% on ALFWorld
  and 10% on WebShop over imitation and RL baselines, with one or two in-context examples [V]
  https://arxiv.org/abs/2210.03629
- **Plan-and-Solve**: plan first, then execute sub-tasks, zero-shot; beats zero-shot CoT on all ten datasets
  with GPT-3 [V] https://arxiv.org/abs/2305.04091
- **ReWOO**: separate planning from observations so tool results do not force re-reading all prior tokens;
  about 5x token efficiency and 4% accuracy gain on HotpotQA, robust to tool failure, reasoning moved from
  175B GPT-3.5 to a 7B model by instruction tuning [V] https://arxiv.org/abs/2305.18323. The most relevant to a
  slow local model: plan once, execute a batch of lookups, then solve.
- **Tree of Thoughts**: explore and evaluate several paths, backtrack; GPT-4 CoT 4% vs ToT 74% on Game of 24
  (the figures stated on the page) [V] https://arxiv.org/abs/2305.10601
- **LATS**: Monte Carlo tree search with LM value functions and reflection; 92.7% pass@1 on HumanEval with
  GPT-4, 75.9 average WebShop score with GPT-3.5 [V] https://arxiv.org/abs/2310.04406
- **CodeAct**: actions as executable code; up to 20% higher success than alternatives across 17 LLMs [V]
  https://arxiv.org/abs/2402.01030
- **AFlow**: workflow search with MCTS; smaller models with generated workflows beat GPT-4o on specific tasks
  at 4.55% of the cost, 5.7% average gain over baselines [V] https://arxiv.org/abs/2410.10762
- **Agentless**: no agent loop, fixed pipeline of localization, repair and validation: 32.00% on SWE-bench Lite
  at $0.70, claimed best open-source result at the time [V] https://arxiv.org/abs/2407.01489
- **Multi-agent failures** (MAST): 14 failure modes in three groups (system design, inter-agent misalignment,
  task verification) from 1,600+ traces; multi-agent systems often give minimal gains [V]
  https://arxiv.org/abs/2503.13657

### 3.2 Cost
Tree search multiplies model calls. At 1 to 2 minutes per step it is unaffordable in the model loop. ReWOO and
Agentless-style fixed pipelines need fewer model calls than ReAct. Search is cheap only when the "model" is a
deterministic scorer.

### 3.3 Verdicts

| Idea | Verdict | Note |
|---|---|---|
| ReAct as the base loop | TAKE (already) | Keep. |
| ReWOO: plan, then batch the evidence gathering without the model | ADAPT, high value | The UIOE is the "worker" side of ReWOO, run before the model's first step. The model sees evidence, not a series of tool calls. |
| Plan-and-Solve / mandatory TodoWrite | TAKE (already) | UIOE can seed the plan skeleton from the request class (section 13). |
| Agentless fixed pipeline for known task classes | ADAPT | For classes like "where is X defined", run a deterministic recipe and hand over the result; the model only judges. |
| ToT / LATS in the model loop | REJECT | Too many model calls. |
| Search over deterministic UIOE decisions (source order, query variants) | ADAPT | Cheap beam of query variants scored by a deterministic fitness (hit count, coverage). |
| Multi-agent debate teams | REJECT | MAST shows minimal gains and a larger failure surface. Keep subagents for context isolation only. |

---

## 4. Query routing in RAG

### 4.1 Systems [V]
- **Self-RAG**: one model learns when to retrieve and critiques passages and its own output with reflection
  tokens; 7B and 13B versions beat ChatGPT and retrieval-augmented Llama2-chat on several tasks (no numbers on
  the abstract page) https://arxiv.org/abs/2310.11511. Needs fine-tuning.
- **CRAG**: a lightweight retrieval evaluator scores document quality and triggers a corrective action (fall
  back to web search when weak); decompose-then-recompose filters irrelevant parts; improves several RAG
  methods on four datasets (no numbers seen) https://arxiv.org/abs/2401.15884
- **Adaptive-RAG**: a small classifier predicts query complexity and routes to no retrieval, single-step, or
  iterative; labels collected automatically from model outcomes and dataset biases (no numbers seen)
  https://arxiv.org/abs/2403.14403
- **IRCoT**: interleave retrieval with chain-of-thought steps; retrieval up to 21 points and QA up to 15 points
  better with GPT-3, gains also with Flan-T5-large, fewer hallucinations; HotpotQA, 2WikiMultihopQA, MuSiQue,
  IIRC https://arxiv.org/abs/2212.10509
- **Self-Ask**: the model asks and answers follow-up sub-questions; the compositionality gap does not shrink
  with scale in the GPT-3 family; self-ask with a search engine helps further https://arxiv.org/abs/2210.03350
- **Rewrite-Retrieve-Read**: rewrite the query before retrieval with a small trainable rewriter; consistent
  improvements (no numbers on page) https://arxiv.org/abs/2305.14283
- **Rethinking with Retrieval**: retrieve per reasoning step, no training https://arxiv.org/abs/2301.00303
- **GraphRAG**: entity graph and community summaries for global questions; better comprehensiveness and
  diversity on million-token corpora https://arxiv.org/abs/2404.16130

### 4.2 Takeaways
- The decision "retrieve or not, and how much" can be made by a small classifier (Adaptive-RAG). For katana the
  classifier is rules plus a feature table (section 13), with the local model as tie-breaker only.
- A retrieval evaluator (CRAG) maps to a deterministic sufficiency check: hit count, term coverage, source
  agreement, freshness. When it fails, escalate to the next source in the route (section 14).
- Iterative retrieval (IRCoT, Self-Ask) maps to a bounded loop in the UIOE: decompose, retrieve per
  sub-question, merge. The decomposition can be rule-based for common patterns ("compare A and B", "X of the
  Y that Z") [design].
- GraphRAG-style LLM indexing is a heavy offline cost; REJECT for core.

| Idea | Verdict |
|---|---|
| Complexity classifier selecting no-retrieval, one-shot, iterative | ADAPT (rules, features) |
| Retrieval quality evaluator and corrective fallback | TAKE (deterministic sufficiency check) |
| Reflection tokens (fine-tuning the model) | REJECT for core; possible later use of the learn/train loop |
| Query rewrite | ADAPT (rule-based expansions; optional model expander) |
| Multi-hop interleaving | ADAPT (bounded UIOE loop) |
| GraphRAG | REJECT |

---

## 5. Model routing and cascades

- **RouteLLM**: routers trained on preference data pick a strong or weak model per query; cost savings of more
  than 2x in some cases with no quality loss on common benchmarks; robust to swapping models [V]
  https://arxiv.org/abs/2406.18665
- **FrugalGPT**: cascades; can match GPT-4 at up to 98% lower cost, or beat it by 4% at equal cost [V]
  https://arxiv.org/abs/2305.05176
- Speculative decoding and draft models: a small model drafts, a large model verifies tokens in parallel, exact
  output distribution preserved [L; Leviathan et al. 2022, Chen et al. 2023].

Katana has one working model, so the cascade is internal: (a) rules answer without any model (a path lookup, a
definition from the index); (b) the same model with a short, no-thinking classification prompt; (c) the full
model for the real work. Speculative decoding is an engine optimization for `docs/optimization-roadmap.md`,
not a UIOE feature.

| Idea | Verdict |
|---|---|
| Cascade: deterministic answer, then short model call, then full | TAKE as design principle ("do not wake the model if the layer can answer") |
| Learned router on preference data | REJECT (no data, no second model) |
| Speculative decoding | Outside UIOE; engine roadmap |

---

## 6. Tool use by small models

### 6.1 Evidence
- BFCL (Berkeley Function Calling Leaderboard) categories: simple, multiple (2 to 4 functions), parallel,
  parallel multiple, plus relevance detection, REST, Java, JavaScript, and later multi-turn [V]
  https://gorilla.cs.berkeley.edu/blogs/8_berkeley_function_calling_leaderboard.html. Findings on that page:
  simple calls are comparable between proprietary and open models; multiple and parallel favored GPT models;
  models struggled with implicit or converted parameters (5 instead of 0.05); models hallucinate calls when no
  function fits. The page did not test large tool sets.
- Gorilla: a fine-tuned model plus retriever for API docs reduces hallucinated API calls [V]
  https://arxiv.org/abs/2305.15334
- RAG-MCP: retrieving only relevant tool descriptions cut prompt tokens by more than 50% and raised tool
  selection accuracy to 43.13% from a 13.62% baseline (authors' benchmark) [V] https://arxiv.org/abs/2505.03275.
  Many tool definitions hurt selection. It uses semantic retrieval, which katana would replace by rules and
  keyword scoring.
- Grammar and format: "Let Me Speak Freely?" reports that strict structured output (JSON, XML) degrades
  reasoning, more so with stricter constraints [V] https://arxiv.org/abs/2408.02442. Implication: let the model
  reason in free text, then constrain only the final tool-call segment.
- Tool design: Anthropic's guidance: consolidate workflows into fewer tools, namespace names, return meaningful
  and token-efficient output with a concise/detailed switch, actionable error messages, clear descriptions,
  evaluate with real transcripts [V] https://www.anthropic.com/engineering/writing-tools-for-agents
- SWE-agent: a purpose-built agent-computer interface reached 12.5% pass@1 on SWE-bench (87.7% on
  HumanEvalFix) in that paper [V] https://arxiv.org/abs/2405.15793. Interface design matters as much as the
  model.

### 6.2 Verdicts
| Idea | Verdict |
|---|---|
| Fewer, consolidated tools; the UIOE as one "gather evidence" tool in front of many read-only tools | TAKE |
| Present only the tools relevant to the classified request (deterministic subset), keeping the prefix stable via logit masking rather than list edits | ADAPT |
| Grammar-constrained decoding for the tool-call segment only | ADAPT |
| Relevance detection ("no tool fits") as an explicit class in the router | TAKE |
| Descriptions evaluated with transcripts | TAKE (use the learning loop's session logs) |
| Embedding-based tool retrieval | REJECT; use keyword and class tables |

---

## 7. Loop detection and failure recovery

Public evidence here is mostly practice, not papers. Manus keeps errors in context for recovery and warns of
few-shot ruts [V]. MAST lists task-verification failures as a major class [V]. Common mechanisms [L]:
iteration caps, token or time budgets, repeated-action hashing, circuit breakers after N consecutive failures,
escalating strategies (change tool, change query, ask the user), checkpoints.

Katana already has repetition and consecutive-failure detection. What a UIOE adds:
- A **failure ledger** per investigation: (tool, normalized arguments, outcome class, step). The UIOE refuses
  to re-run an identical query and says why ("this exact search returned 0 results at step 3").
- **Budgets per investigation**: max sources consulted, max retries per source, max wall-clock for the layer.
- **Circuit breaker per connector**: after K failures a source is marked down for the rest of the session and
  the evidence package says so.
- **Escalation ladder**: widen query, switch source, then return an explicit "not found" (section 16).

Verdict: TAKE all four; they are deterministic and explainable.

---

## 8. Execution-oriented verification

- SWE-bench: 2,294 real GitHub issues over 12 Python repos; the best model at the time resolved 1.96% [V]
  https://arxiv.org/abs/2310.06770. Success is judged by running tests.
- Reflexion and LATS use execution feedback (unit tests on HumanEval) [V above]. Agentless uses patch
  validation as a pipeline stage [V]. CodeAct uses execution results as feedback [V].

For the UIOE, "verification" generalizes: before returning evidence it checks that (a) a cited file or line
still exists and the hash matches, (b) a quoted string is present verbatim in the source, (c) numbers quoted
from a financial API carry timestamp and unit, (d) a legal citation resolves in the legal source. These are
string and structure checks; no model needed. Verdict: TAKE; this is the explainable core.

---

## 9. Evaluation and observability

- OpenTelemetry GenAI semantic conventions: the page I reached only said they moved to a separate repository;
  section titles suggest spans, agent spans, metrics, events and MCP conventions, but status (development or
  stable) is unverified [V for the move, L for contents] https://opentelemetry.io/docs/specs/semconv/gen-ai/
- SWE-bench and SWE-agent for coding agents [V]. RE-Bench: 7 environments, human baseline of 71 eight-hour
  attempts; best AI agents scored about 4x humans at a 2-hour budget, humans did better with more time [V]
  https://arxiv.org/abs/2411.15114
- MAST uses an LLM judge to annotate traces at scale, agreeing well with humans [V].
- Anthropic's tool post recommends reading transcripts and tracking tool calls, errors, tokens [V].

Katana: no measurements now (owner decision). But the UIOE should emit a **decision trace** from day one
(structured line per decision: class, features, sources chosen, reason, hits, sufficiency, time). It costs
nothing, supports `katana learn`, and makes later evals possible. Format: JSON lines, field names aligned with
OTel GenAI where practical [design]. Verdict: TAKE.

---

## 10. Security

- **Lethal trifecta** (Simon Willison): private data, exposure to untrusted content, and external
  communication; combining all three lets an attacker exfiltrate; avoid the combination; after untrusted input,
  constrain consequential actions; guardrail products are not a full solution [V]
  https://simonwillison.net/2025/Jun/16/the-lethal-trifecta/
- **CaMeL**: separate trusted query from untrusted data, derive control and data flow from the trusted query so
  untrusted data cannot change program flow, attach capabilities to data; solves 77% of AgentDojo tasks with
  provable security versus 84% undefended [V] https://arxiv.org/abs/2503.18813
- **MCP spec**: explicit user consent, tool descriptions untrusted unless from a trusted server, data privacy
  principles [V] https://modelcontextprotocol.io/specification/2025-06-18

Katana's agent writes files and runs commands without confirmation (owner decision), and has web search and
file access: that is the full trifecta by design. The UIOE is where mitigation is cheap:
- Label every evidence item with a **trust class**: user, local file, web, third-party API. Web and third-party
  text is wrapped as data, never merged into the instruction part of the prompt.
- **Taint tracking**: once web content enters the context, the session is tainted; the UIOE can lower
  privileges (for example require confirmation for network egress and for writes outside the project) while
  tainted. A CaMeL-inspired adaptation [design], not full CaMeL.
- **Route before read**: the route is fixed from the user's request before any untrusted text is read (the
  CaMeL idea of control flow from the trusted query). Retrieved text cannot add sources to the route.
- Flag instruction-like text in retrieved content (imperatives, "ignore previous"); flag only, as a heuristic.

Verdict: TAKE trust classes and route-before-read; ADAPT taint-based privilege lowering (the owner chose
confirmation-free operation, so lowering must be opt-in or limited to egress; needs an owner decision);
REJECT detector-only defenses as the sole control (Willison's point).

---

## 11. Comparison table (original theme)

| Area | Best public idea | Evidence seen | Cost | Katana verdict |
|---|---|---|---|---|
| Context | Focused context, observation masking | Chroma: 300 vs 113k token prompts; Anthropic post | Low | TAKE |
| Cache | Stable prefix, append-only | Manus post | Free | TAKE |
| Memory | Temporal facts, tiers | Zep, Mem0 (own benchmarks) | Medium | ADAPT (no vectors) |
| Planning | ReWOO (plan, then gather) | about 5x tokens, +4% HotpotQA | Low | ADAPT (UIOE = worker) |
| Search planning | ToT, LATS | 74% vs 4% Game of 24; 92.7% HumanEval | High | REJECT in model loop |
| RAG routing | Complexity classifier, sufficiency check | Adaptive-RAG, CRAG (no numbers seen) | Low | ADAPT (rules) |
| Model routing | Cascade | FrugalGPT up to 98% cheaper; RouteLLM over 2x | Low | TAKE principle |
| Tools | Few tools, good descriptions, partial constraint | BFCL; RAG-MCP 13.62% to 43.13%; format study | Low | TAKE/ADAPT |
| Loops | Ledger, budgets, breaker | Practice; Manus | Free | TAKE |
| Verification | Execute, check quotes | SWE-bench, Agentless | Low | TAKE |
| Evals | Trace first | OTel GenAI (unverified detail), MAST | Low | TAKE trace now |
| Security | Trust classes, route before read | Trifecta; CaMeL 77% vs 84% | Low | TAKE/ADAPT |

---

## 12. What the UIOE is NOT

- Not a second agent. It has no free-form loop; it executes a bounded recipe chosen by rules.
- Not a vector store. Lexical, structural and exact-match methods only in the core.
- Not a prompt-compression model. It selects and cites; it does not paraphrase evidence.
- Not a policy engine for the owner's confirmation settings; it only reports taint and trust.

---

## 13. Interpretation and classification of natural-language requests

### 13.1 What to extract (the "Interpret" and "Classify" boxes)
For each request the UIOE produces a small record, every field derived deterministically, each with a `why`:

| Field | Examples | Method |
|---|---|---|
| Domain | code, filesystem, finance, legal, database, web, personal memory | Keyword and pattern tables per connector (connectors register their own vocabulary), file extensions, tickers, citation patterns |
| Intent | find, explain, compare, summarize, compute, modify, verify | Verb and question-word lexicon; imperative vs interrogative |
| Complexity | single lookup, multi-lookup, multi-hop, open-ended | Count of entities, conjunctions, comparative markers, nesting |
| Entities | paths, identifiers, tickers, dates, amounts, statute and case citations, table names | Regexes, identifier shape (camelCase, snake_case), path syntax, gazetteers built from the local index |
| Constraints | time range, jurisdiction, file glob, "only in module X", currency | Pattern grammar for dates and ranges; negation words |
| Ambiguity | several resolvable referents ("the config"), missing object, pronoun without antecedent | Count candidate resolutions in the local index; if several score similarly, flag |
| Needs retrieval | yes, no, maybe | Presence of entities that exist in sources, temporal words ("latest"), lookup verbs; no-retrieval for pure reasoning or chit-chat |
| Follow-up | refers to previous turn | Pronouns and ellipsis; resolve against the investigation state |

### 13.2 Evidence and prior art
- Adaptive-RAG: predicting query complexity with a small classifier and routing accordingly improves
  efficiency and accuracy (qualitative, no numbers seen) [V] https://arxiv.org/abs/2403.14403
- Self-Ask: multi-hop questions have a measurable compositionality gap that does not close with scale [V];
  detecting multi-hop shape in the request before the model sees it is worth doing.
- Rewrite-Retrieve-Read: there is a gap between the user's wording and the wording the knowledge needs;
  rewriting helps (no numbers seen) [V].
- BFCL's relevance detection: models invent calls when no tool fits [V]; a "no source applies" outcome must
  exist in the classifier.
- Semantic routers [L]: libraries such as Aurelio's `semantic-router` route by embedding similarity to example
  utterances; that is the embedding approach the brief excludes. Rule tables and lexical scoring give similar
  behavior for closed sets of intents and are explainable.
- Classic intent classification practice [L]: for a small closed set of intents, bag-of-words logistic
  regression or naive Bayes on a few hundred labelled examples is competitive and runs in microseconds. Katana's
  session logs (request, tools used) are free labels for a domain and tool-subset classifier [design].

### 13.3 Recommended design for katana [design]
1. Layer 0, rules: regex and lexicon extraction of entities and constraints (always on).
2. Layer 1, scored table: per domain, a weighted keyword and pattern score; pick top domains above a
   threshold; record scores in the trace. Ties produce several domains.
3. Layer 2, trained linear model (optional, offline): naive Bayes or logistic regression on token and
   character n-gram features, trained from session logs; shipped as a small table in the binary.
   Deterministic and explainable (top contributing tokens can be printed).
4. Layer 3, local model tie-breaker (optional, off by default): a short, no-thinking prompt returning a label
   from a closed list under a grammar constraint, used only when layers 1 and 2 disagree; logged as
   `source=model`.
5. Ambiguity: if ambiguity is high and a wrong guess is costly (writes, spending money, legal claims), return a
   clarification request for the model to relay; otherwise take the top candidate and state the assumption in
   the evidence package.

Verdict: TAKE layers 0 and 1; ADAPT layer 2 (offline, from katana's own logs); ADAPT layer 3 as optional.

---

## 14. Routing and orchestration across sources

### 14.1 Prior art
- **Federated search and mediators** [L]: the mediator/wrapper architecture (Wiederhold, 1992) puts a mediator
  between users and heterogeneous sources, with a wrapper per source translating a common query form into the
  source's native form. Enterprise federated search selects sources (resource selection), queries in parallel,
  then merges results. The UIOE's connectors are wrappers; the router is the source selector; the context
  builder does the merge.
- **Resource selection** [L]: classical methods score a source from sampled vocabulary statistics (CORI,
  ReDDE). A deterministic katana version: each connector publishes a capability declaration (domains, entity
  types, operators, freshness, cost class) matched against the interpreted request.
- **Result fusion without embeddings** [L]: Reciprocal Rank Fusion (Cormack, Clarke, Buettcher, SIGIR 2009)
  sums 1/(k + rank) over rankings and needs no score calibration, which matters because sources have
  incomparable scores. BM25 is the standard lexical ranker.
- **ReWOO** [V]: plan all lookups up front with placeholders for earlier results, execute without the model;
  directly the "Orchestrate" box. Reports robustness under tool failure.
- **IRCoT** [V]: when a hop depends on a previous result, interleave. The UIOE supports dependent steps with a
  small DAG (step B uses the entity found by step A), bounded depth.
- **Agentless** [V]: fixed phases with explicit stopping were cost-effective on SWE-bench Lite.
- **CRAG** [V]: when the primary source is weak, fall back to another source; an evaluator decides.
- **MAST** [V]: task verification and inter-agent misalignment are failure classes; avoid hidden state between
  components; keep one investigation record.

### 14.2 Orchestration design [design]
- **Route** = ordered list of (connector, operation, arguments, timeout, retries, depends_on), chosen by rules
  from the request record and printed as a plan ("Route: fs.grep(foo); code.defs(foo); web.search skipped
  because the answer is local").
- **Parallelism**: independent steps run concurrently with a per-step timeout and an overall deadline; the
  deadline is tiny compared with a model step, so the UIOE pays off if it finishes in seconds.
- **Cheap before expensive**: order by cost class (local index, local files, database, network).
- **Retries**: at most one retry for transient errors (timeout, 429 honoring Retry-After), none for semantic
  4xx errors; widen the query once (stemming, local synonym list, identifier splitting such as `fooBar` to
  `foo bar`); then stop.
- **Stop conditions**: sufficiency reached (coverage of required entities plus one authoritative or two
  corroborating sources), budget exhausted, or route done. The stop reason is part of the output.
- **Fusion and dedup**: normalize every result to an evidence item (section 16); fuse by RRF across sources;
  dedup by exact hash, then near-duplicate shingle overlap (Jaccard over word 5-grams [L]); keep provenance of
  all merged copies.
- **Conflict handling**: when two sources disagree on the same (subject, predicate), keep both, mark
  `conflict`, and order by a per-domain authority list (legal: primary source over commentary; finance:
  exchange data over news; code: working tree over index over memory). Never choose silently.
- **State**: an `Investigation` record (request record, route, per-step outcomes, evidence items, failure
  ledger, open questions) lives in session state, is appended to, and is summarized into compaction. Follow-up
  questions reuse it instead of redoing the route.

### 14.3 Verdicts
| Idea | Verdict |
|---|---|
| Mediator and wrapper architecture | TAKE |
| Resource selection by declared capabilities | TAKE (replaces sampled statistics) |
| RRF fusion | TAKE |
| Plan with dependencies and bounded depth (ReWOO plus IRCoT) | TAKE |
| Learned source selection | REJECT for now |
| Open-ended agent loop inside the UIOE | REJECT |

---

## 15. Connectors for finance, legal, databases and local files

### 15.1 Patterns
- **MCP** [V]: JSON-RPC 2.0, stateful connections, capability negotiation; servers expose resources (data),
  prompts and tools; clients may offer sampling, roots (filesystem and URI boundaries) and elicitation;
  security principles require user consent and treat tool descriptions as untrusted unless the server is
  trusted https://modelcontextprotocol.io/specification/2025-06-18. Katana already speaks MCP as a client
  (`docs/mcp.md`). The UIOE treats each MCP server as a connector with a declared capability set and trust class.
- **OpenAPI** [L]: machine-readable REST description; a generic connector can be generated from an OpenAPI
  document (operations become connector operations, parameters become typed arguments). Read-only by default.
- **Text-to-SQL and its risks.** BIRD: 12,751 pairs over 95 databases (33.4 GB, 37 domains); the best model in
  that paper reached 40.08% execution accuracy versus 92.96% for humans [V] https://arxiv.org/abs/2305.03111
  (2023; current models are better, but it shows that dirty values, large schemas and external knowledge
  matter). DIN-SQL decomposes the task into sub-problems and reports 85.3 on the Spider holdout and 55.9% on the
  BIRD holdout [V] https://arxiv.org/abs/2304.11015. Risks [L]: wrong but plausible joins, unbounded scans,
  writes, exfiltration through results, injection through stored text. Mitigations for katana [design]:
  read-only connection or allowlist of statements (SELECT only, parsed not regexed), row and time limits,
  schema pruning to tables matching request entities (deterministic schema linking), show generated SQL and
  row count in the evidence item, and a query catalogue (predefined parameterized queries for known question
  types) before free text-to-SQL; the model only picks an entry and arguments.
- **Financial APIs** [L]: typical needs are API keys, rate limits, data delay and licensing, currency and unit
  conversions, as-of timestamps. The connector must return `as_of`, `currency`, `unit`, `source`, `delay` as
  structured fields so the model never guesses (BFCL's 5 vs 0.05 case shows models mishandle implicit
  conversions [V]). Computation (returns, ratios) is done by the connector, not the model.
- **Legal sources** [L]: citation formats are structured (statute sections, neutral citations, reporter
  citations); authoritative source and version date matter (consolidated vs as-enacted). The connector returns
  exact text with a stable citation and version date and flags "may be superseded"; it never paraphrases a
  provision. Disclaimers belong to the product layer.
- **Local files and code**: already covered by katana (docsel, code index). The UIOE wraps them as connectors
  with hash-stamped provenance.

### 15.2 Common connector contract [design]
```
capabilities: domains, entity_types, operations[], cost_class, freshness, trust_class
call(op, args, budget) -> EvidenceItem[] + Outcome{ok|empty|error|timeout|rate_limited, detail}
limits: rate limiter (token bucket), concurrency, timeout, max_bytes
credentials: from environment or a local key file; never in prompts or traces
provenance: source_id, locator (path:line, URL, citation, SQL + row range), retrieved_at, content_hash
```
Credentials never enter model-visible text and traces redact them. Rate limits are enforced by the UIOE, so a
model that retries cannot hammer an API.

### 15.3 Source citation
Evidence items carry a short handle (`[E3]`); the model is told to cite handles; the UIOE can verify after the
fact that each cited handle exists and that quoted strings occur in the item. A deterministic check of the
answer, usable as a feedback signal by the learning loop.

### 15.4 Verdicts
| Item | Verdict |
|---|---|
| MCP servers as connectors | TAKE |
| OpenAPI-generated read-only connectors | ADAPT (later) |
| Free text-to-SQL | ADAPT with strict guardrails; query catalogue first |
| Financial and legal connectors in core | REJECT (plugins, per the project's third-party-as-plugin rule) |
| Structured provenance and as-of fields | TAKE |

---

## 16. The Context Builder

### 16.1 Principles with evidence
- Less and better beats more: focused 300-token prompts beat 113k full prompts on LongMemEval [V] (Chroma).
- Distractors hurt; four are worse than one [V]. So dedup and drop weak evidence.
- Position: Lost in the Middle [V] versus Chroma's finding that needle position mattered little in their NIAH
  variant [V]. Read together: ordering is a cheap hedge, not a guarantee. Put the request and top evidence at
  the ends.
- Extractive compression: LLMLingua shows compression is possible with a second model [V]; the deterministic
  alternative is sentence or line windows around query-term hits [design].
- Restorable compression (Manus) [V]: keep a locator so the model can fetch the full text.
- Stable prefix [V]: volatile material (evidence) goes after stable material (system prompt, tool list,
  project facts); never reorder earlier turns.
- Sub-agent summaries of 1,000 to 2,000 tokens [V] are a size hint for an evidence package.

### 16.2 Pipeline [design]
1. **Collect** evidence items from all connectors (section 14).
2. **Normalize**: item = {id, text, locator, source, trust_class, retrieved_at, hash, scores}.
3. **Dedup**: exact hash, then shingle overlap; keep the best locator and list the others as "also in".
4. **Rerank without embeddings**: score = weighted sum of (a) BM25-like term score against request terms and
   expansions, (b) entity match (exact identifiers weigh heavily), (c) source authority for the domain,
   (d) recency or version fit, (e) structural proximity (definition beats mention; same file as the active
   file; closeness to evidence the user already accepted), (f) corroboration count. RRF merges per-source
   ranks first. Every term is printable.
5. **Extract**: from each long item keep the best windows (lines or sentences around hits) with the enclosing
   heading or signature and a locator for expansion.
6. **Budget**: a token budget per request class, allocated by rank with a minimum per source and a cap per
   item; shrink windows before dropping items; drop whole items only below a score floor. The evidence is
   appended after the stable prefix, so the cache survives.
7. **Order**: request restated first, then the route summary, then evidence by descending score grouped by
   source, then the not-found and conflicts block, then the next-step hint at the end (recitation, as in
   Manus [V]).
8. **Cite**: handles and locators on every item.
9. **Negative evidence**: an explicit block, "Not found: searched X in A, B (queries ...); sources not
   consulted: C (reason)". Reasons: not applicable, down, rate-limited, budget. This should let a small model
   stop searching instead of repeating lookups. No source studied isolated this as an ablation, so the effect is
   unmeasured; it follows from Manus's failures-in-context lesson [V] and BFCL's finding that models hallucinate
   when nothing fits [V].
10. **Explain**: a one-line `[uioe: ...]` note like the existing `[katana: ...]` notes: class, route, stop
    reason, items kept and dropped.

### 16.3 Evidence package shape [design]
```
[uioe request] intent=find domain=code entities=[parseHeader] constraints=[]
[uioe route]   code.defs ; fs.grep ; (web skipped: local answer)  stop=sufficient
[E1] src/engine/header.c:120-148  definition  trust=local  hash=ab12..   <extract>
[E2] src/app/main.c:44            call site   trust=local  hash=9c01..   <extract>
[not found] docs mention of parseHeader (fs.grep docs/, 0 hits)
[conflicts] none
[assumptions] "the parser" resolved to parseHeader (2 candidates, picked by definition rank)
```

### 16.4 Verdicts
| Technique | Verdict |
|---|---|
| Deterministic rerank (BM25-like plus structure plus authority) | TAKE |
| Extractive windows with restorable locators | TAKE |
| Token budgets by class | TAKE |
| Ends-first ordering | ADAPT |
| LLM summarization of evidence | REJECT in the hot path; optional offline |
| Negative evidence block | TAKE (unmeasured; keep, flagged as hypothesis) |

---

## 17. Does deterministic pre-processing before the LLM help small models?

What the literature supports, honestly.

Supported [V]:
- Moving work out of the model loop reduces calls and tokens: ReWOO about 5x token efficiency and +4% on
  HotpotQA, with reasoning transferable to a 7B model; Agentless 32.00% on SWE-bench Lite at $0.70 with a fixed
  pipeline; AFlow smaller models beating GPT-4o on specific tasks at 4.55% of the cost with searched workflows.
- Interface design changes results: SWE-agent's interface; Anthropic's tool-design guidance; RAG-MCP tool
  selection from 13.62% to 43.13% by showing fewer tools.
- Focused context beats big context: Chroma (300 vs 113k tokens).
- Format constraints can hurt reasoning, so keep deterministic structure outside the model's reasoning text.
- Routing and cascades save cost without losing quality in the reported cases: FrugalGPT up to 98% cheaper at
  GPT-4 level on its tasks; RouteLLM over 2x savings in some cases.
- Retrieval quality gating helps downstream generation (CRAG, Self-RAG, Adaptive-RAG; qualitative on the
  abstract pages seen).

Not supported or untested in what I read:
- No source measured a pre-LLM deterministic router on a MoE with about 3B active parameters and 1 to 2 minute
  steps.
- None isolated the benefit of not-found blocks, trust labels or ordering rules.
- Benchmarks mix model sizes and tasks; the numbers are the authors' own and not comparable with each other.

Conclusion: the direction (move retrieval and routing out of the model loop, shrink and focus context) is
consistent with the public evidence, but the size of the gain for katana is unknown. When measurements resume,
the first one should be steps to completion on a fixed task set with and without the UIOE, using the decision
trace.

---

## 18. The ten most important lessons

1. Treat the context window as scarce; focused evidence beats large context (Chroma: 300 vs 113k tokens on
   LongMemEval; Anthropic: diminishing returns) [V].
2. Separate gathering from reasoning: ReWOO shows that planning lookups once and executing them without the
   model saves tokens and survives tool failure [V]. The UIOE is the executor.
3. Keep the prefix stable and append-only; volatile evidence goes last (Manus) [V].
4. Offload and restore: store locators, not bodies; clear old tool results (Anthropic, Manus) [V].
5. Fewer, better tools, and show only relevant ones (Anthropic tool guidance; RAG-MCP) [V]; the UIOE becomes
   one front tool.
6. Do not over-constrain the model's reasoning with strict formats; constrain only the call segment [V].
7. Classify before you retrieve: complexity and need-for-retrieval routing (Adaptive-RAG), with an explicit
   "no source applies" class (BFCL relevance) [V].
8. Evaluate retrieval sufficiency deterministically and fall back along a fixed ladder (CRAG idea) [V]; stop
   with an explicit stop reason.
9. Untrusted text is data: label trust, fix the route before reading, restrict egress after taint (trifecta,
   CaMeL, MCP) [V]. Katana's no-confirmation design makes this more important, not less.
10. Make every decision explainable and logged as a trace: it feeds the learning loop and later evals (MAST,
    OTel GenAI) [V partial].

---

## 19. Layers a complete system would have, and what katana lacks

Gap size: S small (days), M medium (weeks), L large (months). Estimates are the author's judgment.

| Layer | What it does | Katana today | Gap |
|---|---|---|---|
| 1. Request interpreter | Entity, constraint, intent, complexity extraction | docsel partially (documents); nothing general | M |
| 2. Domain and complexity classifier | Rule tables, optional trained linear model | None | M |
| 3. Ambiguity and clarification policy | Detect, assume, ask | None | S |
| 4. Source registry and capability declarations | Connectors declare domains, ops, cost, trust | MCP client, code index, docs; no unified registry | M |
| 5. Router and planner | Ordered route with dependencies and deadlines | The model decides tool by tool | L |
| 6. Orchestrator | Parallel execution, retries, circuit breakers, rate limits | Tool by tool; failure detection only | M |
| 7. Normalizer and provenance | Evidence item schema, hashes, locators | `[katana: ...]` notes, no schema | M |
| 8. Fusion and dedup | RRF, hash and shingle dedup, conflict marking | None | M |
| 9. Sufficiency and stop logic | Coverage checks, stop reason | None | S to M |
| 10. Context builder | Rerank, extract, budget, order, cite, negative evidence | docsel plus compaction | L |
| 11. Investigation state | Persistent per-task record reused in follow-ups and compaction | Session history; facts as text | M |
| 12. Fact memory with validity and provenance | Subject-predicate-value, intervals, trust, staleness | remember/recall as plain text | M |
| 13. Verification layer | Quote and locator checks, citation checks, SQL guardrails | Execution checks by the model | M |
| 14. Security layer | Trust classes, taint, egress limits, redaction | Action log; opt-in confirmation | M |
| 15. Decision trace and eval hooks | JSONL traces for learning and later evals | Session logs, no decision trace | S |
| 16. Learning of routing from sessions | Train classifier and tool subset from logs | learn/export/train for the model; nothing for routing | M |
| 17. Optional model expander and tie-breaker | Query expansion, label tie-break under grammar | Model only as the agent | S |
| 18. Domain plugins (finance, legal, SQL) | External connectors under the plugin rule | None (MCP exists) | L, deferred |

Suggested order when UIOE work starts [design]: 15 (trace), 7 (evidence schema), 1 and 2 (rules only), 5 and 6
(small route for code and files), 10 (builder with negative evidence), 9, 12, then 14, then connectors.

---

## 20. Open questions

- Which few request classes cover most of katana's sessions? The existing session history can answer this
  without any model.
- How strict should the sufficiency check be before the model is invoked? Too strict wastes seconds, too loose
  adds model steps.
- Does the 3B-active model use a not-found block correctly, or does it still repeat searches? Not measured.
- How should taint interact with the owner's no-confirmation rule? Needs an owner decision.
- Is a shingle-based near-duplicate step needed, or does the code and docs corpus rarely duplicate?

---

## 21. Sources (accessed 2026-11-23)

Papers (arXiv abstract pages read):
- MemGPT https://arxiv.org/abs/2310.08560
- ReAct https://arxiv.org/abs/2210.03629
- ReWOO https://arxiv.org/abs/2305.18323
- Reflexion https://arxiv.org/abs/2303.11366
- Self-RAG https://arxiv.org/abs/2310.11511
- CRAG https://arxiv.org/abs/2401.15884
- Adaptive-RAG https://arxiv.org/abs/2403.14403
- RouteLLM https://arxiv.org/abs/2406.18665
- Mem0 https://arxiv.org/abs/2504.19413
- Zep/Graphiti https://arxiv.org/abs/2501.13956
- A-MEM https://arxiv.org/abs/2502.12110
- Tree of Thoughts https://arxiv.org/abs/2305.10601
- LATS https://arxiv.org/abs/2310.04406
- IRCoT https://arxiv.org/abs/2212.10509
- Self-Ask https://arxiv.org/abs/2210.03350
- Plan-and-Solve https://arxiv.org/abs/2305.04091
- Lost in the Middle https://arxiv.org/abs/2307.03172
- Gorilla https://arxiv.org/abs/2305.15334
- RAG-MCP https://arxiv.org/abs/2505.03275
- Let Me Speak Freely https://arxiv.org/abs/2408.02442
- FrugalGPT https://arxiv.org/abs/2305.05176
- SWE-bench https://arxiv.org/abs/2310.06770
- SWE-agent https://arxiv.org/abs/2405.15793
- Agentless https://arxiv.org/abs/2407.01489
- CodeAct https://arxiv.org/abs/2402.01030
- AFlow https://arxiv.org/abs/2410.10762
- MAST (multi-agent failures) https://arxiv.org/abs/2503.13657
- RE-Bench https://arxiv.org/abs/2411.15114
- CaMeL https://arxiv.org/abs/2503.18813
- LLMLingua https://arxiv.org/abs/2310.05736
- BIRD https://arxiv.org/abs/2305.03111
- DIN-SQL https://arxiv.org/abs/2304.11015
- Rewrite-Retrieve-Read https://arxiv.org/abs/2305.14283
- Rethinking with Retrieval https://arxiv.org/abs/2301.00303
- GraphRAG https://arxiv.org/abs/2404.16130

Posts and specifications:
- Anthropic, effective context engineering https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents
- Anthropic, writing tools for agents https://www.anthropic.com/engineering/writing-tools-for-agents
- Manus, context engineering lessons https://manus.im/blog/Context-Engineering-for-AI-Agents-Lessons-from-Building-Manus
- Chroma, Context Rot https://www.trychroma.com/research/context-rot
- Berkeley Function Calling Leaderboard https://gorilla.cs.berkeley.edu/blogs/8_berkeley_function_calling_leaderboard.html
- Simon Willison, lethal trifecta https://simonwillison.net/2025/Jun/16/the-lethal-trifecta/
- Model Context Protocol specification https://modelcontextprotocol.io/specification/2025-06-18
- OpenTelemetry GenAI semantic conventions (moved; landing page only) https://opentelemetry.io/docs/specs/semconv/gen-ai/

Recalled only, not opened in this study: Letta project documentation; speculative decoding papers (Leviathan
et al. 2022; Chen et al. 2023); Cormack et al. RRF (SIGIR 2009); BM25 (Robertson); Wiederhold mediators
(1992); CORI and ReDDE resource selection; Aurelio semantic-router; OpenAPI specification; OWASP LLM Top 10.
Verify before citing externally.

Caveats: most pages read were arXiv abstracts, so numbers are limited to what abstracts state; all performance
figures are the authors' own on their own benchmarks and were not reproduced; nothing here was measured on
katana.
